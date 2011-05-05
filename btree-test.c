#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/blkdev.h>

#include "md/persistent-data/dm-btree.h"
#include "md/persistent-data/dm-transaction-manager.h"
#include "dm-space-map-core.h"

/*----------------------------------------------------------------*/

#define NR_BLOCKS 1024
#define BM_BLOCK_SIZE 4096
#define CACHE_SIZE 16		/* small enough that there will be a lot of contention */

typedef int (*test_fn)(struct dm_transaction_manager *);

/*----------------------------------------------------------------*/

/*
 * For these tests we're not interested in the space map root, so we roll
 * tm_pre_commit() and tm_commit() into one function.
 */
static int begin(struct dm_transaction_manager *tm, struct dm_block **superblock)
{
	int r;

	r = dm_tm_new_block(tm, superblock);
	if (r < 0)
		return r;

	return dm_tm_begin(tm);
}

static int begin_again(struct dm_transaction_manager *tm, dm_block_t sb, struct dm_block **superblock)
{
	int r = dm_bm_write_lock(dm_tm_get_bm(tm), sb, superblock);
	if (r < 0)
		return r;

	return dm_tm_begin(tm);
}

static void commit(struct dm_transaction_manager *tm, struct dm_block *superblock)
{
	dm_tm_pre_commit(tm);
	dm_tm_commit(tm, superblock);
}

static uint64_t next_rand(uint64_t last)
{
	/* FIXME: check how good this is */
	const uint64_t a = 274177;
	const uint64_t c = 1;
	return a * last + c;
}

#define INSERT_COUNT 5000
static int check_insert_commit_every(struct dm_transaction_manager *tm,
				     unsigned commit_interval)
{
	int r, i, committed = 1;
	uint64_t key = 0;
	uint64_t value = 0;
	dm_block_t root = 0;
	struct dm_btree_info info;
	struct dm_block *superblock;

	info.tm = tm;
	info.levels = 1;
	info.value_type.size = sizeof(uint64_t);
	info.value_type.copy = NULL;
	info.value_type.del = NULL;
	info.value_type.equal = NULL;

	r = begin(tm, &superblock);
	if (r < 0) {
		printk(KERN_ALERT "begin failed");
		return r;
	}

	r = dm_btree_empty(&info, &root);
	if (r < 0) {
		printk(KERN_ALERT "dm_btree_empty failed");
		return r;
	}

	/* write some random entries into the btree */
	for (i = 0; i < INSERT_COUNT; i++) {
		committed = 0;
		key = next_rand(value);
		value = next_rand(key);
		r = dm_btree_insert(&info, root, &key, &value, &root);
		if (r < 0) {
			printk(KERN_ALERT "dm_btree_insert failed");
			return r;
		}

		if ((i + 1 % commit_interval) == 0) {
			dm_block_t b = dm_block_location(superblock);
			commit(tm, superblock);
			r = begin_again(tm, b, &superblock);
			if (r < 0)
				return r;
			committed = 1;
		}
	}

	if (!committed)
		commit(tm, superblock);

	/* check they're all still there */
	key = value = 0;
	for (i = 0; i < INSERT_COUNT; i++) {
		uint64_t value2;
		key = next_rand(value);
		value = next_rand(key);

		r = dm_btree_lookup_equal(&info, root, &key, &value2);
		if (r < 0)
			return r;

		if (value2 != value) {
			printk(KERN_ALERT "wrong value");
			return -1;
		}
	}

	return 0;
}

static int check_insert(struct dm_transaction_manager *tm)
{
	return check_insert_commit_every(tm, 100000);
}

static int check_multiple_commits(struct dm_transaction_manager *tm)
{
	return check_insert_commit_every(tm, 100);
}

static int check_lookup_empty(struct dm_transaction_manager *tm)
{
	int r;
	uint64_t key = 100;
	__le64 value;
	dm_block_t root = 0;
	struct dm_btree_info info;
	struct dm_block *superblock;

	info.tm = tm;
	info.levels = 1;
	info.value_type.size = sizeof(uint64_t);
	info.value_type.copy = NULL;
	info.value_type.del = NULL;
	info.value_type.equal = NULL;

	r = begin(tm, &superblock);
	if (r < 0) {
		printk(KERN_ALERT "begin failed");
		return r;
	}

	r = dm_btree_empty(&info, &root);
	if (r < 0) {
		printk(KERN_ALERT "btree_empty failed");
		return r;
	}

	r = dm_btree_lookup_equal(&info, root, &key, &value);
	if (r == 0) {
		printk(KERN_ALERT "value unexpectedly found");
		return -1;
	}

	if (r < 0 && r != -ENODATA)
		return r;

	commit(tm, superblock);
	return 0;
}

static int check_insert_h(struct dm_transaction_manager *tm)
{
	typedef uint64_t table_entry[5];
	static table_entry table[] = {
		{ 1, 1, 1, 1, 100 },
		{ 1, 1, 1, 2, 101 },
		{ 1, 1, 1, 3, 102 },

		{ 1, 1, 2, 1, 200 },
		{ 1, 1, 2, 2, 201 },
		{ 1, 1, 2, 3, 202 },

		{ 2, 1, 1, 1, 301 },
		{ 2, 1, 1, 2, 302 },
		{ 2, 1, 1, 3, 303 }
	};

	static table_entry overwrites[] = {
		{ 1, 1, 1, 1, 1000 }
	};

	uint64_t value;
	dm_block_t root = 0, sb;
	int i, r;
	struct dm_btree_info info;
	struct dm_block *superblock;

	info.tm = tm;
	info.levels = 4;
	info.value_type.size = sizeof(uint64_t);
	info.value_type.copy = NULL;
	info.value_type.del = NULL;
	info.value_type.equal = NULL;

	r = begin(tm, &superblock);
	if (r < 0)
		return r;
	sb = dm_block_location(superblock);

	r = dm_btree_empty(&info, &root);
	if (r < 0) {
		printk(KERN_ALERT "btree_empty() failed");
		return r;
	}

	for (i = 0; i < sizeof(table) / sizeof(*table); i++) {
		r = dm_btree_insert(&info, root, table[i], &table[i][4], &root);
		if (r < 0) {
			printk(KERN_ALERT "btree_insert failed");
			return r;
		}
	}
	commit(tm, superblock);

	for (i = 0; i < sizeof(table) / sizeof(*table); i++) {
		r = dm_btree_lookup_equal(&info, root, table[i], &value);
		if (r < 0) {
			printk(KERN_ALERT "btree_lookup_equal failed");
			return r;
		}

		if (value != table[i][4]) {
			printk(KERN_ALERT "bad lookup");
			return -1;
		}
	}

	/* check multiple transactions are ok */
	{
		uint64_t keys[4] = { 1, 1, 1, 4 }, value, v = 2112;

		r = begin_again(tm, sb, &superblock);
		if (r < 0)
			return r;

		r = dm_btree_insert(&info, root, keys, &v, &root);
		if (r < 0) {
			printk(KERN_ALERT "btree_insert failed");
			return r;
		}

		commit(tm, superblock);

		r = dm_btree_lookup_equal(&info, root, keys, &value);
		if (r < 0) {
			printk(KERN_ALERT "btree_lookup_equal failed");
			return r;
		}

		if (value != 2112) {
			printk(KERN_ALERT "unexpected lookup");
			return -1;
		}
	}

	/* check overwrites */
	begin_again(tm, sb, &superblock);
	for (i = 0; i < sizeof(overwrites) / sizeof(*overwrites); i++) {
		r = dm_btree_insert(&info, root, overwrites[i], &overwrites[i][4], &root);
		if (r < 0) {
			printk(KERN_ALERT "btree_insert failed");
			return r;
		}
	}
	commit(tm, superblock);

	for (i = 0; i < sizeof(overwrites) / sizeof(*overwrites); i++) {
		r = dm_btree_lookup_equal(&info, root, overwrites[i], &value);
		if (r < 0) {
			printk(KERN_ALERT "btree_lookup_equal failed");
			return r;
		}

		if (value != overwrites[i][4]) {
			printk(KERN_ALERT "bad lookup");
			return -1;
		}
	}
	return 0;
}

#define MAX_LEVELS 4
static int do_remove_scenario(struct dm_btree_info *info, dm_block_t root)
{
	int i, r;
	uint64_t key[MAX_LEVELS], bad_key[MAX_LEVELS];
	__le64 value = 0;

	if (info->levels > MAX_LEVELS) {
		printk(KERN_ALERT "too many levels");
		return -1;
	}

	for (i = 0; i < info->levels - 1; i++) {
		key[i] = 1;
		bad_key[i] = 1;
	}
	key[i] = 100;
	bad_key[i] = 101;

	r = dm_btree_insert(info, root, key, &value, &root);
	if (r) {
		printk(KERN_ALERT "insert failed");
		return -1;
	}

	r = dm_btree_remove(info, root, bad_key, &root);
	if (r != -ENODATA) {
		printk(KERN_ALERT "remove1 didn't return -ENODATA");
		return -1;
	}

	r = dm_btree_remove(info, root, key, &root);
	if (r) {
		printk(KERN_ALERT "remove failed");
		return r;
	}

	r = dm_btree_remove(info, root, bad_key, &root);
	if (r != -ENODATA) {
		printk(KERN_ALERT "remove2 didn't return -ENODATA");
		return -1;
	}

	r = dm_btree_remove(info, root, key, &root);
	if (r != -ENODATA) {
		printk(KERN_ALERT "remove3 didn't return -ENODATA");
		return -1;
	}

	r = dm_btree_lookup_equal(info, root, key, &value);
	if (r == 0) {
		printk(KERN_ALERT "value unexpectedly found");
		return -1;
	}

	return 0;
}

static int check_remove_one(struct dm_transaction_manager *tm)
{
	int r;
	dm_block_t root = 0;
	struct dm_btree_info info;
	struct dm_block *superblock;

	info.tm = tm;
	info.levels = 1;
	info.value_type.size = sizeof(uint64_t);
	info.value_type.copy = NULL;
	info.value_type.del = NULL;
	info.value_type.equal = NULL;

	r = begin(tm, &superblock);
	if (r < 0) {
		printk(KERN_ALERT "begin failed");
		return r;
	}

	r = dm_btree_empty(&info, &root);
	if (r < 0) {
		printk(KERN_ALERT "btree_empty failed");
		return r;
	}

	return do_remove_scenario(&info, root);
}

static int check_removal_with_internal_nodes(struct dm_transaction_manager *tm)
{
	int r;
	__le64 value = 0;
	dm_block_t root = 0;
	struct dm_btree_info info;
	struct dm_block *superblock;

	info.tm = tm;
	info.levels = 1;
	info.value_type.size = sizeof(uint64_t);
	info.value_type.copy = NULL;
	info.value_type.del = NULL;
	info.value_type.equal = NULL;

	r = begin(tm, &superblock);
	if (r < 0) {
		printk(KERN_ALERT "begin failed");
		return r;
	}

	r = dm_btree_empty(&info, &root);
	if (r < 0) {
		printk(KERN_ALERT "btree_empty failed");
		return r;
	}

	{
		/* prime the tree with enough entries that we know there are internal nodes */
		unsigned c;
		for (c = 0; c < 1000; c++) {
			uint64_t k = c + 10000;
			r = dm_btree_insert(&info, root, &k, &value, &root);
			if (r) {
				printk(KERN_ALERT "insert(%u) failed", c);
				return r;
			}
		}
	}

	return do_remove_scenario(&info, root);
}

static int check_removal_in_hierarchy(struct dm_transaction_manager *tm)
{
	int r;
	uint64_t key[3];
	__le64 value = 0;
	dm_block_t root = 0;
	struct dm_btree_info info;
	struct dm_block *superblock;

	info.tm = tm;
	info.levels = 3;
	info.value_type.size = sizeof(uint64_t);
	info.value_type.copy = NULL;
	info.value_type.del = NULL;
	info.value_type.equal = NULL;

	r = begin(tm, &superblock);
	if (r < 0) {
		printk(KERN_ALERT "begin failed");
		return r;
	}

	r = dm_btree_empty(&info, &root);
	if (r < 0) {
		printk(KERN_ALERT "btree_empty failed");
		return r;
	}

	{
		/* prime the tree with enough entries that we know there are internal nodes */
		unsigned c;
		key[0] = 1;
		key[1] = 1;
		for (c = 0; c < 1000; c++) {
			key[2] = c + 10000;
			r = dm_btree_insert(&info, root, key, &value, &root);
			if (r) {
				printk(KERN_ALERT "insert(%u) failed", c);
			}
		}
	}

	return do_remove_scenario(&info, root);
}

static int insert_remove_many_scenario(
	struct dm_transaction_manager *tm,
	unsigned *order,
	unsigned count)
{
	int r;
	unsigned c, check;
	__le64 value = 0;
	dm_block_t root = 0;
	struct dm_btree_info info;
	struct dm_block *superblock;

	info.tm = tm;
	info.levels = 1;
	info.value_type.size = sizeof(uint64_t);
	info.value_type.copy = NULL;
	info.value_type.del = NULL;
	info.value_type.equal = NULL;

	r = begin(tm, &superblock);
	if (r < 0) {
		printk(KERN_ALERT "begin failed");
		return r;
	}

	r = dm_btree_empty(&info, &root);
	if (r < 0) {
		printk(KERN_ALERT "btree_empty failed");
		return r;
	}

	for (c = 0; c < count; c++) {
		uint64_t k = order[c];
		r = dm_btree_insert(&info, root, &k, &value, &root);
		if (r) {
			printk(KERN_ALERT "insert(%u) failed", c);
			return r;
		}
	}

	for (check = c + 1; check < count; check++) {
		uint64_t k = order[check];
		void *value;
		r = dm_btree_lookup_equal(&info, root, &k, &value);
		if (r) {
			printk(KERN_ALERT "missing %d", order[check]);
			return r;
		}
	}

	for (c = 0; c < count; c++) {
		uint64_t k = order[c];
		__le64 value;
		r = dm_btree_remove(&info, root, &k, &root);
		if (r) {
			printk(KERN_ALERT "remove(%u) failed (r = %d)", order[c], r);
			return r;
		}

		for (check = c + 1; check < count; check++) {
			uint64_t k = order[check];
			r = dm_btree_lookup_equal(&info, root, &k, &value);
			if (r) {
				printk(KERN_ALERT "remove(%u) also removed %d", order[c], order[check]);
				return r;
			}
		}

		r = dm_btree_lookup_equal(&info, root, &k, &value);
		if (!r) {
			printk(KERN_ALERT "remove didn't work for %d", order[c]);
			return -1;
		}
	}

	return 0;
}

#define COUNT 1000
static int check_insert_remove_many(struct dm_transaction_manager *tm)
{
	static unsigned order[COUNT];

	int i;

	for (i = 0; i < COUNT; i++)
		order[i] = i;

	return insert_remove_many_scenario(tm, order, COUNT);
}

static int check_insert_remove_many_reverse(struct dm_transaction_manager *tm)
{
	static unsigned order[COUNT];

	int i;

	for (i = 0; i < COUNT; i++)
		order[i] = COUNT - 1 - i;

	return insert_remove_many_scenario(tm, order, COUNT);
}

// RNG snarfed from wikipedia
static unsigned random(unsigned limit)
{
	static unsigned W = 101;    /* must not be zero */
	static unsigned Z = 243;    /* must not be zero */

	Z = 36969 * (Z & 65535) + (Z >> 16);
	W = 18000 * (W & 65535) + (W >> 16);
	return (((Z << 16) + W) % limit);
}

static void shuffle(unsigned *array, unsigned count)
{
	unsigned i;

	for (i = 0; i < count; i++) {
		unsigned other = i + random(count - i);
		unsigned tmp = array[i];
		array[i] = array[other];
		array[other] = tmp;
	}
}

static int check_insert_remove_many_random(struct dm_transaction_manager *tm)
{
	static unsigned order[COUNT];

	int i;

	for (i = 0; i < COUNT; i++)
		order[i] = i;

	shuffle(order, COUNT);
	return insert_remove_many_scenario(tm, order, COUNT);
}

static int check_insert_remove_many_center(struct dm_transaction_manager *tm)
{
	static unsigned order[COUNT];

	int i;

	// First a central chunk of values
	for (i = 0; i < 500; i++)
		order[i] = i + 300;

	// Then the outliers
	for (i = 0; i < 300; i++)
		order[i + 500] = i;

	for (i = 0; i < 200; i++)
		order[i + 800] = i + 800;

	return insert_remove_many_scenario(tm, order, COUNT);
}

/*----------------------------------------------------------------*/

static int run_test(const char *name, test_fn fn)
{
	int r;
	struct dm_space_map *sm = dm_sm_core_create(NR_BLOCKS);
	int mode = FMODE_READ | FMODE_WRITE | FMODE_EXCL;
	struct block_device *bdev = blkdev_get_by_path("/dev/sdb", mode, &run_test);
	struct dm_block_manager *bm;
	struct dm_transaction_manager *tm;

	if (IS_ERR(bdev))
		return -1;

	bm = dm_block_manager_create(bdev, BM_BLOCK_SIZE, CACHE_SIZE);
	if (!bm)
		return -1;

	tm = dm_tm_create(bm, sm);
	if (!tm)
		return -1;

	printk(KERN_ALERT "running %s ... ", name);
	r = fn(tm);
	printk(r == 0 ? KERN_ALERT "pass\n" : KERN_ALERT "fail\n");

	dm_tm_destroy(tm);
	dm_block_manager_destroy(bm);
	blkdev_put(bdev, mode);
	dm_sm_destroy(sm);
	return 0;
}

static int btree_test_init(void)
{
	static struct {
		const char *name;
		test_fn fn;
	} table_[] = {
		{"lookup in an empty btree", check_lookup_empty},
		{"check insert", check_insert},
		{"check insert, commit every 100", check_multiple_commits},
		{"check hierarchical insert", check_insert_h},
		{"insert one, remove one", check_remove_one},
		{"insert many, remove one", check_removal_with_internal_nodes},
		{"insert many, remove one, hierarchical", check_removal_in_hierarchy},
		{"repeated insert/remove linear order", check_insert_remove_many},
		{"repeated insert/remove linear order", check_insert_remove_many_reverse},
		{"repeated insert/remove random order", check_insert_remove_many_random},
		{"repeated insert/remove center order", check_insert_remove_many_center},
	};

	int i;

	for (i = 0; i < sizeof(table_) / sizeof(*table_); i++)
		run_test(table_[i].name, table_[i].fn);

	return 0;
}

static void btree_test_exit(void)
{
}

module_init(btree_test_init);
module_exit(btree_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joe Thornber");
MODULE_DESCRIPTION("Test code for B+ trees");

/*----------------------------------------------------------------*/
