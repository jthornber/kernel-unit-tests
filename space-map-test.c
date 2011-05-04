#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/blkdev.h>

#include "md/persistent-data/dm-space-map.h"
#include "md/persistent-data/dm-space-map-disk.h"
#include "md/persistent-data/dm-space-map-staged.h"
#include "md/persistent-data/dm-transaction-manager.h"
#include "dm-space-map-core.h"

/*----------------------------------------------------------------*/

#define NR_BLOCKS 1024
#define BM_BLOCK_SIZE 4096
#define CACHE_SIZE 16

typedef int (*test_fn)(struct dm_space_map *);

/*----------------------------------------------------------------*/

static int check_alloc_n(struct dm_space_map *sm, dm_block_t max)
{
	int i;
	dm_block_t b;

	for (i = 0; i < max; i++) {
		if (dm_sm_new_block(sm, &b) < 0) {
			printk(KERN_ALERT "couldn't allocate the %u block\n", i);
			return -1;
		}
	}

	return 0;
}

static int check_alloc(struct dm_space_map *sm)
{
	int r;
	dm_block_t b;

	r = check_alloc_n(sm, NR_BLOCKS);
	if (r < 0)
		return r;


	printk(KERN_ALERT "should have allocated all blocks");
	if (dm_sm_new_block(sm, &b) == 0) {
		printk(KERN_ALERT "allocated more blocks than possible %u", (unsigned) b);
		return -1;
	}

	return 0;
}

static int check_staged_alloc(struct dm_space_map *sm)
{
	return check_alloc_n(sm, NR_BLOCKS / 2);
}

static int check_alloc_range(struct dm_space_map *sm)
{
	int i;
	dm_block_t b;
	dm_block_t low = 2, high = 4;

	BUG_ON(high > NR_BLOCKS);

	for (i = low; i < high; i++) {
		if (dm_sm_set_count(sm, (dm_block_t) i, 1) < 0) {
			printk(KERN_ALERT "couldn't set count for block %u\n", i);
			return -1;
		}
	}

	if (dm_sm_get_free_in_range(sm, low, high, &b) == 0) {
		printk(KERN_ALERT "allocated more blocks than possible %u", (unsigned) b);
		return -1;
	}

	if (dm_sm_get_free(sm, &b) < 0) {
		printk(KERN_ALERT "dm_sm_get_free failed");
		return -1;
	}

	return 0;

}

static int check_can_count(struct dm_space_map *sm)
{
	int i;
	dm_block_t b;

	if (dm_sm_new_block(sm, &b) < 0) {
		printk(KERN_ALERT "dm_sm_new_block failed");
		return -1;
	}

	for (i = 0; i < 8; i++) {
		if (dm_sm_inc_block(sm, b) < 0) {
			printk(KERN_ALERT "dm_sm_inc_block failed");
			return -1;
		}
	}

	for (; i > 0; --i) {
		uint32_t count;

		if (dm_sm_dec_block(sm, b) < 0) {
			printk(KERN_ALERT "dm_sm_dec_block failed");
			return -1;
		}

		if (dm_sm_get_count(sm, b, &count) < 0) {
			printk(KERN_ALERT "dm_sm_get_count failed");
			return -1;
		}

		if (count != i) {
			printk(KERN_ALERT "bad count, expected %u was %u",
			       (unsigned) i, (unsigned) count);
			return -1;
		}
	}

	return 0;
}

static int check_freeing(struct dm_space_map *sm)
{
	int r;
	dm_block_t b, b2;

	if (dm_sm_new_block(sm, &b) < 0) {
		printk(KERN_ALERT "dm_sm_new_block failed");
		return -1;
	}

	do {
		dm_block_t tmp;
		r = dm_sm_new_block(sm, &tmp);
	} while (r == 0);

	if (dm_sm_dec_block(sm, b) < 0) {
		printk(KERN_ALERT "dm_sm_dec_block failed");
		return -1;
	}

	if (dm_sm_new_block(sm, &b2) < 0) {
		printk(KERN_ALERT "dm_sm_new_block failed");
		return -1;
	}

	if (b != b2) {
		printk(KERN_ALERT "allocator weirdness");
		return -1;
	}

	return 0;
}

static int check_reopen_disk(void)
{
	int r;
	struct dm_space_map *sm = dm_sm_core_create(NR_BLOCKS), *smd;
	int mode = FMODE_READ | FMODE_WRITE | FMODE_EXCL;
	struct block_device *bdev = blkdev_get_by_path("/dev/sdb", mode, &check_reopen_disk);
	struct dm_block_manager *bm;
	struct dm_transaction_manager *tm;
	static unsigned char data[1024];
	size_t len;
	dm_block_t b;

	if (IS_ERR(bdev))
		return -1;

	bm = dm_block_manager_create(bdev, BM_BLOCK_SIZE, CACHE_SIZE);
	if (!bm)
		return -1;

	tm = dm_tm_create(bm, sm);
	if (!tm)
		return -1;

	smd = dm_sm_disk_create(tm, NR_BLOCKS);

	printk(KERN_ALERT "running check reopen disk ... ");

	/* Allocate one block */
	if (dm_sm_new_block(sm, &b) < 0) {
		printk(KERN_ALERT "couldn't allocate a block");
		return -1;
	}

	check_alloc_n(smd, 1);
	if (dm_sm_commit(smd) < 0) {
		printk(KERN_ALERT "commit failed");
		return -1;
	}

	/* save the root */
	if (dm_sm_root_size(smd, &len) < 0) {
		printk(KERN_ALERT "dm_sm_root_size failed");
		return -1;
	}

	if (dm_sm_copy_root(smd, data, len) < 0) {
		printk(KERN_ALERT "dm_sm_copy_root failed");
		return -1;
	}

	/* tear everything down */
	dm_sm_destroy(smd);

	/* reopen */
	smd = dm_sm_disk_open(tm, data, len);
	if (!smd) {
		printk(KERN_ALERT "reopen failed");
		return -1;
	}

	/* keep allocating until we're out of space, checking that first
	   allocated block never comes up. */
	do {
		dm_block_t tmp;
		r = dm_sm_new_block(sm, &tmp);
		if (tmp == b) {
			printk(KERN_ALERT "allocated duplicate");
			return -1;
		}
	} while (r == 0);

	printk(KERN_ALERT "pass");

	dm_sm_destroy(smd);
	dm_sm_destroy(sm);
	dm_tm_destroy(tm);
	dm_block_manager_destroy(bm);
	blkdev_put(bdev, mode);
	return 0;
}


/*----------------------------------------------------------------*/

static int run_test_core(const char *name, test_fn fn)
{
	int r;
	struct dm_space_map *sm = dm_sm_core_create(NR_BLOCKS);

	printk(KERN_ALERT "running %s ... ", name);
	r = fn(sm);
	printk(r == 0 ? KERN_ALERT "pass\n" : KERN_ALERT "fail\n");

	dm_sm_destroy(sm);
	return 0;
}

static int run_test_disk(const char *name, test_fn fn)
{
	int r;
	struct dm_space_map *sm = dm_sm_core_create(NR_BLOCKS), *smd;
	int mode = FMODE_READ | FMODE_WRITE | FMODE_EXCL;
	struct block_device *bdev = blkdev_get_by_path("/dev/sdb", mode, &run_test_disk);
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

	smd = dm_sm_disk_create(tm, NR_BLOCKS);

	printk(KERN_ALERT "running %s ... ", name);
	r = fn(smd);
	printk(r == 0 ? KERN_ALERT "pass\n" : KERN_ALERT "fail\n");

	dm_sm_destroy(sm);
	dm_tm_destroy(tm);
	dm_block_manager_destroy(bm);
	blkdev_put(bdev, mode);
	return 0;
}

static int run_test_staged_core(const char *name, test_fn fn)
{
	int r;
	struct dm_space_map *core = dm_sm_core_create(NR_BLOCKS);
	struct dm_space_map *staged = dm_sm_staged_create(core);

	printk(KERN_ALERT "running %s ... ", name);
	r = fn(staged);
	printk(r == 0 ? KERN_ALERT "pass\n" : KERN_ALERT "fail\n");

	dm_sm_destroy(staged);
	return 0;
}

static int run_test_staged_disk(const char *name, test_fn fn)
{
	int r;
	struct dm_space_map *sm;
	int mode = FMODE_READ | FMODE_WRITE | FMODE_EXCL;
	struct block_device *bdev = blkdev_get_by_path("/dev/sdb", mode, &run_test_staged_disk);
	struct dm_block_manager *bm;
	struct dm_transaction_manager *tm;
	struct dm_block *superblock;

	if (IS_ERR(bdev))
		return -1;

	bm = dm_block_manager_create(bdev, BM_BLOCK_SIZE, CACHE_SIZE);
	if (!bm)
		return -1;

	r = dm_tm_create_with_sm(bm, 0, &tm, &sm, &superblock);
	if (r < 0)
		return r;

	/* commit */
	r = dm_tm_pre_commit(tm);
	if (r < 0) {
		printk(KERN_ALERT "coudln't pre commit");
		return r;
	}

	r = dm_tm_commit(tm, superblock);
	if (r < 0) {
		printk(KERN_ALERT "couldn't commit");
		return r;
	}

	/* and we're finally ready for action */
	r = dm_bm_write_lock(dm_tm_get_bm(tm), 0, &superblock);
	if (r < 0) {
		printk(KERN_ALERT "couldn't lock superblock");
		return -1;
	}

	printk(KERN_ALERT "running %s ... ", name);
	r = fn(sm);
	printk(r == 0 ? KERN_ALERT "pass\n" : KERN_ALERT "fail\n");

	r = dm_tm_pre_commit(tm);
	if (r < 0) {
		printk(KERN_ALERT "coudln't pre commit");
		return r;
	}

	r = dm_tm_commit(tm, superblock);
	if (r < 0) {
		printk(KERN_ALERT "couldn't commit");
		return r;
	}

	dm_tm_destroy(tm);
	dm_sm_destroy(sm);
	dm_block_manager_destroy(bm);
	blkdev_put(bdev, mode);
	return 0;
}

struct entry {
	const char *name;
	test_fn fn;
};

static int space_map_test_init(void)
{
	static struct entry table_[] = {
		{"alloc all blocks", check_alloc},
		{"alloc range", check_alloc_range},
		{"inc/dec", check_can_count},
		{"freeing", check_freeing}
	};

	static struct entry staged_table_[] = {
		{"alloc some blocks", check_staged_alloc},
		{"alloc range", check_alloc_range},
		{"inc/dec", check_can_count},
	};

	int i;

	printk(KERN_ALERT "running tests with core space map");
	for (i = 0; i < sizeof(table_) / sizeof(*table_); i++)
		run_test_core(table_[i].name, table_[i].fn);

	printk(KERN_ALERT "running tests with disk space map");
	for (i = 0; i < sizeof(table_) / sizeof(*table_); i++)
		run_test_disk(table_[i].name, table_[i].fn);

	printk(KERN_ALERT "running tests with staged space map wrapping a core space map");
	for (i = 0; i < sizeof(staged_table_) / sizeof(*staged_table_); i++)
		run_test_staged_core(staged_table_[i].name, staged_table_[i].fn);

	check_reopen_disk();

	printk(KERN_ALERT "running tests with staged space map wrapping a disk space map (slightly different tests)");
	for (i = 0; i < sizeof(staged_table_) / sizeof(*staged_table_); i++)
		run_test_staged_disk(staged_table_[i].name, staged_table_[i].fn);

	return 0;
}

static void space_map_test_exit(void)
{
}

module_init(space_map_test_init);
module_exit(space_map_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joe Thornber");
MODULE_DESCRIPTION("Test code for space-map");

/*----------------------------------------------------------------*/
