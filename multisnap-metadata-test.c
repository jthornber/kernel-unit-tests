#include <linux/blkdev.h>
#include <linux/device-mapper.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "md/persistent-data/block-manager.h"
#include "md/multisnap-metadata.h"

/*----------------------------------------------------------------*/

#define NR_BLOCKS 1024
#define BM_BLOCK_SIZE 4096
#define CACHE_SIZE 16
#define METADATA_BLOCK_SIZE 4096
#define DATA_BLOCK_SIZE ((1024 * 1024 * 128) >> SECTOR_SHIFT)

#define DATA_DEV_SIZE 512
#define TEST_DEVICE "/dev/sdc"	/* FIXME: get this from module parameters */

/*----------------------------------------------------------------*/

static int with_block(const char *path, block_t blk,
		      void (*fn)(void *, void *),
		      void *context)
{
	int r;
	struct block_manager *bm;
	struct block_device *bdev;
	struct block *b;
	int mode = FMODE_READ | FMODE_WRITE | FMODE_EXCL;
	bdev = blkdev_get_by_path(path, mode, &with_block);
	if (IS_ERR(bdev)) {
		printk(KERN_ALERT "blkdev_get_by_path failed");
		return -1;
	}

	bm = block_manager_create(bdev, METADATA_BLOCK_SIZE, 1);
	if (!bm) {
		printk(KERN_ALERT "%s: couldn't create bm", __func__);
		return -1;
	}

	r = bm_write_lock(bm, blk, &b);
	if (r)
		printk(KERN_ALERT "%s: couldn't lock block",
		       __func__);
	else {
		fn(context, block_data(b));
		bm_unlock(b);
		r = bm_flush(bm, 1);
	}

	block_manager_destroy(bm);
	blkdev_put(bdev, mode);
	return r;
}

/*--------------------------------*/

static void memset_(void *context, void *data)
{
	unsigned char *v = (unsigned char *) context;
	memset(data, *v, METADATA_BLOCK_SIZE);
}

static int memset_block(const char *path, block_t blk, unsigned char v)
{
	return with_block(path, blk, memset_, &v);
}

/*--------------------------------*/

struct sb_context {
	size_t offset;
	unsigned char v;
};

static void set_byte_(void *context, void *data)
{
	unsigned char *as_chars = data;
	struct sb_context *sbc = (struct sb_context *) context;

	/* FIXME: this appears to zero the multisnap magic byte !
	   Something weird going on here. */
	as_chars[sbc->offset] = sbc->v;
}

static int set_block_byte(const char *path, block_t blk, size_t offset, unsigned char v)
{
	struct sb_context sbc;
	sbc.offset = offset;
	sbc.v = v;
	return with_block(path, blk, set_byte_, &sbc);
}

/*--------------------------------*/

#define MAX_MSD 32
struct test_context {
	struct block_device *bdev;
	struct multisnap_metadata *mmd;

	unsigned nr_msd;
	struct ms_device *msd[MAX_MSD];
};

static int create_mmd(struct test_context *tc)
{
	int mode = FMODE_READ | FMODE_WRITE | FMODE_EXCL;

	tc->bdev = blkdev_get_by_path(TEST_DEVICE, mode, &create_mmd);
	if (IS_ERR(tc->bdev))
		return -1;

	tc->mmd = multisnap_metadata_open(tc->bdev,
					  DATA_BLOCK_SIZE,
					  DATA_DEV_SIZE);
	if (!tc->mmd) {
		blkdev_put(tc->bdev, mode);
		printk(KERN_ALERT "couldn't create mmd");
		return -1;
	}

	return 0;
}

static int destroy_mmd(struct test_context *tc)
{
	int r;
	int mode = FMODE_READ | FMODE_WRITE | FMODE_EXCL;
	unsigned i;

	for (i = 0; i < tc->nr_msd; i++) {
		if (tc->msd[i]) {
			r = multisnap_metadata_close_device(tc->msd[i]);
			if (r) {
				printk(KERN_ALERT "mmd_close_device failed");
				return r;
			}
		}
	}

	r = multisnap_metadata_close(tc->mmd);
	if (r)
		return r;

	blkdev_put(tc->bdev, mode);
	return 0;
}

/*--------------------------------*/

static int setup_fresh_mmd(struct test_context *tc)
{
	int r = memset_block(TEST_DEVICE, 0, 0);
	if (r) {
		printk(KERN_ALERT "memset failed");
		return r;
	}

	memset(tc, 0, sizeof(*tc));
	return create_mmd(tc);
}

static int setup_fresh_mmd_and_thin(struct test_context *tc,
				    multisnap_dev_t id)
{
	int r;

	r = setup_fresh_mmd(tc);
	if (r)
		return r;

	r = multisnap_metadata_create_thin(tc->mmd, id);
	if (r) {
		printk(KERN_ALERT "mmd_create_thin failed");
		destroy_mmd(tc);
	}

	return r;
}

static int setup_fresh_and_open_thins(struct test_context *tc, unsigned count)
{
	int r;
	unsigned i;

	r = setup_fresh_mmd(tc);
	if (r)
		return r;

	for (i = 0; i < count; i++) {
		r = multisnap_metadata_create_thin(tc->mmd, i);
		if (r) {
			printk(KERN_ALERT "mmd_create_thin failed");
			destroy_mmd(tc);
		}

		r = multisnap_metadata_open_device(tc->mmd, i, tc->msd + i);
		if (r) {
			printk(KERN_ALERT "mmd open_device failed");
			destroy_mmd(tc);
			return r;
		}

		tc->nr_msd++;
	}

	return 0;
}

static int open_dev(struct test_context *tc, multisnap_dev_t dev, unsigned *index)
{
	int r = multisnap_metadata_open_device(tc->mmd, dev, tc->msd + tc->nr_msd);
	if (!r) {
		*index= tc->nr_msd;
		tc->nr_msd++;
	}

	return r;
}

/*----------------------------------------------------------------*/

static int check_create_mmd(void)
{
	int r;
	struct test_context tc;

	r = setup_fresh_mmd(&tc);
	if (r)
		return r;

	return destroy_mmd(&tc);
}

static int check_reopen_mmd(void)
{
	int r;
	struct test_context tc;

	r = setup_fresh_mmd(&tc);
	if (r)
		return r;

	r = destroy_mmd(&tc);
	if (r)
		return r;

	r = create_mmd(&tc);
	if (r)
		return r;

	return destroy_mmd(&tc);
}

static int check_reopen_bad_fails(void)
{
	int r;
	struct test_context tc;

	r = memset_block(TEST_DEVICE, 0, 63);
	if (r)
		return r;

	r = create_mmd(&tc);
	if (!r) {
		printk(KERN_ALERT "create_mmd unexpectedly succeeded");
		destroy_mmd(&tc);
		return -1;
	}

	return 0;
}

static int check_reopen_slightly_bad_fails(void)
{
	int r;
	struct test_context tc;

	r = setup_fresh_mmd(&tc);
	if (r)
		return r;

	r = destroy_mmd(&tc);
	if (r)
		return r;

	/*
	 * Touch just one byte, quite far into the block, so it's probably
	 * not used.
	 */
	set_block_byte(TEST_DEVICE, 0, 1024, 63);

	r = create_mmd(&tc);
	if (!r) {
		printk(KERN_ALERT "create_mmd unexpectedly succeeded");
		destroy_mmd(&tc);
		return -1;
	}

	return 0;
}

static int check_open_bad_msd(void)
{
	int r;
	struct test_context tc;
	struct ms_device *msd;

	r = setup_fresh_mmd(&tc);
	if (r)
		return r;

	r = multisnap_metadata_open_device(tc.mmd, 0, &msd);
	if (!r) {
		printk(KERN_ALERT "open msd unexpectedly succeeded");
		return -1;
	}

	return destroy_mmd(&tc);
}

static int check_create_thin_msd(void)
{
	int r;
	struct test_context tc;
	r = setup_fresh_mmd_and_thin(&tc, 0);
	if (r)
		return r;

	return destroy_mmd(&tc);
}

static int check_open_thin_msd(void)
{
	int r;
	struct ms_device *msd;
	struct test_context tc;

	r = setup_fresh_mmd_and_thin(&tc, 0);
	if (r)
		return r;

	r = multisnap_metadata_open_device(tc.mmd, 0, &msd);
	if (r) {
		printk(KERN_ALERT "mmd_open_device failed");
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_close_device(msd);
	if (r) {
		printk(KERN_ALERT "mmd_close_device failed");
		destroy_mmd(&tc);
		return r;
	}

	return destroy_mmd(&tc);
}

static int check_open_msd_twice_fails(void)
{
	int r;
	struct ms_device *msd, *msd2;
	struct test_context tc;

	r = setup_fresh_mmd_and_thin(&tc, 0);
	if (r)
		return r;

	r = multisnap_metadata_open_device(tc.mmd, 0, &msd);
	if (r) {
		printk(KERN_ALERT "mmd_open_device failed");
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_open_device(tc.mmd, 0, &msd2);
	if (!r) {
		printk(KERN_ALERT "mmd_open_device (for the second time) unexpected succeeded");
		multisnap_metadata_close_device(msd);
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_close_device(msd);
	if (r) {
		printk(KERN_ALERT "mmd_close_device failed");
		destroy_mmd(&tc);
		return r;
	}

	return destroy_mmd(&tc);
}

static int check_mmd_close_with_open_msd_fails(void)
{
	int r;
	struct ms_device *msd;
	struct test_context tc;

	r = setup_fresh_mmd_and_thin(&tc, 0);
	if (r)
		return r;

	r = multisnap_metadata_open_device(tc.mmd, 0, &msd);
	if (r) {
		printk(KERN_ALERT "mmd_open_device failed");
		destroy_mmd(&tc);
		return r;
	}

	r = destroy_mmd(&tc);
	if (!r) {
		printk(KERN_ALERT "destroy_mmd() unexpectedly succeeded");
		return -1;
	}

	/* tidy */
	r = multisnap_metadata_close_device(msd);
	if (r) {
		printk(KERN_ALERT "mmd_close_device failed");
		destroy_mmd(&tc);
		return r;
	}

	return destroy_mmd(&tc);
}

static int check_delete_msd(void)
{
	int r;
	struct test_context tc;

	r = setup_fresh_mmd_and_thin(&tc, 0);
	if (r)
		return r;

	r = multisnap_metadata_delete(tc.mmd, 0);
	if (r) {
		printk(KERN_ALERT "mmd_delete failed");
		destroy_mmd(&tc);
		return r;
	}

	return destroy_mmd(&tc);
}

static int check_open_of_deleted_msd_fails(void)
{
	int r;
	struct ms_device *msd;
	struct test_context tc;

	r = setup_fresh_mmd_and_thin(&tc, 0);
	if (r)
		return r;

	r = multisnap_metadata_delete(tc.mmd, 0);
	if (r) {
		printk(KERN_ALERT "mmd_delete failed");
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_open_device(tc.mmd, 0, &msd);
	if (!r) {
		printk(KERN_ALERT "mmd open_device unexpectedly succeeded");
		multisnap_metadata_close_device(msd);
		destroy_mmd(&tc);
		return -1;
	}

	return destroy_mmd(&tc);
}

static int check_empty_msd_lookup_fails(void)
{
	int r;
	struct test_context tc;
	uint64_t value;

	r = setup_fresh_and_open_thins(&tc, 1);
	if (r)
		return r;

	r = multisnap_metadata_map(tc.msd[0], 0, READ, 1, &value);
	if (!r) {
		printk(KERN_ALERT "mmd_lookup unexpectedly succeeded");
		destroy_mmd(&tc);
		return -1;
	}

	return destroy_mmd(&tc);
}

static int check_insert_succeeds(void)
{
	int r;
	struct test_context tc;
	uint64_t value;

	r = setup_fresh_and_open_thins(&tc, 1);
	if (r)
		return r;

	r = multisnap_metadata_map(tc.msd[0], 0, WRITE, 1, &value);
	if (r) {
		printk(KERN_ALERT "mmd_insert failed");
		destroy_mmd(&tc);
		return r;
	}

	return destroy_mmd(&tc);
}

static int check_two_inserts_in_same_device_differ(void)
{
	int r;
	struct test_context tc;
	uint64_t value1, value2;

	r = setup_fresh_and_open_thins(&tc, 1);
	if (r)
		return r;

	r = multisnap_metadata_map(tc.msd[0], 0, WRITE, 1, &value1);
	if (r) {
		printk(KERN_ALERT "mmd_insert failed");
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_map(tc.msd[0], 1, WRITE, 1, &value2);
	if (r) {
		printk(KERN_ALERT "mmd_insert failed");
		destroy_mmd(&tc);
		return r;
	}

	if (value1 == value2) {
		printk(KERN_ALERT "mmd_inserts mapped to same destination");
		destroy_mmd(&tc);
		return -1;
	}

	return destroy_mmd(&tc);
}

static int check_lookup_after_insert(void)
{
	int r;
	struct test_context tc;
	uint64_t value1, value2;

	r = setup_fresh_and_open_thins(&tc, 1);
	if (r)
		return r;

	r = multisnap_metadata_map(tc.msd[0], 0, WRITE, 1, &value1);
	if (r) {
		printk(KERN_ALERT "mmd_insert failed");
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_map(tc.msd[0], 0, READ, 1, &value2);
	if (r) {
		printk(KERN_ALERT "mmd_lookup failed");
		destroy_mmd(&tc);
		return r;
	}

	if (value1 != value2) {
		printk(KERN_ALERT "mmd_insert and mmd_lookup returned different blocks");
		destroy_mmd(&tc);
		return r;
	}

	return destroy_mmd(&tc);
}

static int check_two_inserts_in_different_devices_differ(void)
{
	int r;
	struct test_context tc;
	uint64_t value1, value2;

	r = setup_fresh_and_open_thins(&tc, 2);
	if (r)
		return r;

	r = multisnap_metadata_map(tc.msd[0], 0, WRITE, 1, &value1);
	if (r) {
		printk(KERN_ALERT "mmd_insert failed");
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_map(tc.msd[1], 0, WRITE, 1, &value2);
	if (r) {
		printk(KERN_ALERT "mmd_insert failed");
		destroy_mmd(&tc);
		return r;
	}

	if (value1 == value2) {
		printk(KERN_ALERT "mmd_inserts mapped to same destination");
		destroy_mmd(&tc);
		return -1;
	}

	return destroy_mmd(&tc);
}

static int check_data_space_can_be_exhausted(void)
{
	int r;
	struct test_context tc;
	uint64_t value;
	unsigned i;

	r = setup_fresh_and_open_thins(&tc, 1);
	if (r)
		return r;

	/* use up all the available data space */
	for (i = 0; i < DATA_DEV_SIZE; i++) {
		r = multisnap_metadata_map(tc.msd[0], i, WRITE, 1, &value);
		if (r) {
			printk(KERN_ALERT "mmd_insert failed");
			destroy_mmd(&tc);
			return r;
		}
	}

	/* next insert should fail */
	r = multisnap_metadata_map(tc.msd[0], i, WRITE, 1, &value);
	if (!r) {
		printk(KERN_ALERT "insert unexpectedly succeeded");
		return -1;
	}

	return destroy_mmd(&tc);
}

static int check_data_space_can_be_exhausted_two_devs(void)
{
	int r;
	struct test_context tc;
	uint64_t value;
	unsigned i;

	r = setup_fresh_and_open_thins(&tc, 2);
	if (r)
		return r;

	/* use up all the available data space */
	for (i = 0; i < DATA_DEV_SIZE; i++) {
		r = multisnap_metadata_map(tc.msd[i % 2], i, WRITE, 1, &value);
		if (r) {
			printk(KERN_ALERT "mmd_insert failed");
			destroy_mmd(&tc);
			return r;
		}
	}

	/* next insert should fail */
	r = multisnap_metadata_map(tc.msd[i % 2], i, WRITE, 1, &value);
	if (!r) {
		printk(KERN_ALERT "insert unexpectedly succeeded");
		return -1;
	}

	return destroy_mmd(&tc);
}

static int check_create_snapshot(void)
{
	int r;
	struct test_context tc;
	r = setup_fresh_and_open_thins(&tc, 1);
	if (r)
		return r;

	r = multisnap_metadata_create_snap(tc.mmd, 1, 0);
	if (r) {
		printk(KERN_ALERT "mmd_create_snap failed");
		destroy_mmd(&tc);
		return r;
	}

	return destroy_mmd(&tc);
}

static int check_fresh_snapshot_has_same_mappings(void)
{
	int r, i;
	unsigned index;
	struct test_context tc;
	block_t block1, block2;

	r = setup_fresh_and_open_thins(&tc, 1);
	if (r)
		return r;

	for (i = 0; i < 10; i++) {
		r = multisnap_metadata_map(tc.msd[0], i, WRITE, 1, &block1);
		if (r) {
			printk(KERN_ALERT "mmd_insert failed");
			destroy_mmd(&tc);
			return r;
		}
	}

	r = multisnap_metadata_create_snap(tc.mmd, 1, 0);
	if (r) {
		printk(KERN_ALERT "mmd_create_snap failed");
		destroy_mmd(&tc);
		return r;
	}

	r = open_dev(&tc, 1, &index);
	if (r) {
		destroy_mmd(&tc);
		return r;
	}

	for (i = 0; i < 10; i++) {
		r = multisnap_metadata_map(tc.msd[0], i, READ, 1, &block1);
		if (r) {
			printk(KERN_ALERT "mmd_lookup1 failed");
			destroy_mmd(&tc);
			return r;
		}

		r = multisnap_metadata_map(tc.msd[index], i, READ, 1, &block2);
		if (r) {
			printk(KERN_ALERT "mmd_lookup2 failed");
			destroy_mmd(&tc);
			return r;
		}

		if (block1 != block2) {
			printk(KERN_ALERT "blocks differ %u != %u",
			       (unsigned) block1, (unsigned) block2);
			destroy_mmd(&tc);
			return r;
		}
	}

	return destroy_mmd(&tc);
}

/* Scenario 1
 * 1 - origin <- snap
 * 2 - write snap => IO_MAPPED
 * 3 - read snap => IO_MAPPED (2)
 * 4 - write snap => IO_MAPPED (2)
 * 5 - read snap => IO_MAPPED (2)
 */
static int check_snap_scenario1(void)
{
	int r;
	unsigned index;
	struct test_context tc;
	block_t block1, block2;

	r = setup_fresh_and_open_thins(&tc, 1);
	if (r)
		return r;

	/* make sure one block is mapped on the origin */
	r = multisnap_metadata_map(tc.msd[0], 0, WRITE, 1, &block1);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_create_snap(tc.mmd, 1, 0);
	if (r) {
		printk(KERN_ALERT "mmd_create_snap failed");
		destroy_mmd(&tc);
		return r;
	}

	r = open_dev(&tc, 1, &index);
	if (r) {
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_map(tc.msd[index], 0, WRITE, 1, &block2);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (block1 == block2) {
		printk(KERN_ALERT "blocks unexpectedly match %u", (unsigned) block1);
		destroy_mmd(&tc);
		return -1;
	}

	r = multisnap_metadata_map(tc.msd[index], 0, READ, 1, &block1);
	if (r) {
		printk(KERN_ALERT "mmd_map2 failed");
		destroy_mmd(&tc);
		return r;
	}

	if (block1 != block2) {
		printk(KERN_ALERT "blocks differ");
		destroy_mmd(&tc);
		return -1;
	}

	r = multisnap_metadata_map(tc.msd[index], 0, WRITE, 1, &block1);
	if (r) {
		printk(KERN_ALERT "mmd_map3 failed");
		destroy_mmd(&tc);
		return -1;
	}

	if (block1 != block2) {
		printk(KERN_ALERT "blocks differ (2)");
		destroy_mmd(&tc);
		return -1;
	}

	r = multisnap_metadata_map(tc.msd[index], 0, READ, 1, &block1);
	if (r) {
		printk(KERN_ALERT "mmd_map4 failed");
		destroy_mmd(&tc);
		return -1;
	}

	if (block1 != block2) {
		printk(KERN_ALERT "blocks differ (3)");
		destroy_mmd(&tc);
		return -1;
	}

	return destroy_mmd(&tc);
}

static int check_get_workqueue(void)
{
	int r;
	struct test_context tc;
	struct workqueue_struct *wq;

	r = setup_fresh_and_open_thins(&tc, 1);
	if (r)
		return r;

	wq = multisnap_metadata_get_workqueue(tc.msd[0]);
	if (!wq) {
		printk(KERN_ALERT "get workqueue failed");
		destroy_mmd(&tc);
		return -1;
	}

	return destroy_mmd(&tc);
}

/*----------------------------------------------------------------*/

typedef int (*test_fn)(void);

static int run_test(const char *name, test_fn fn)
{
	int r;

	printk(KERN_ALERT "running %s ... ", name);
	r = fn();
	printk(r == 0 ? KERN_ALERT "pass\n" : KERN_ALERT "fail\n");

	return r;
}

static int multisnap_metadata_test_init(void)
{
	static struct {
		const char *name;
		test_fn fn;
	} table_[] = {
		/* creation of the metadata device */
		{"create new metadata device",	     check_create_mmd},
		{"reopen metadata device",	     check_reopen_mmd},
		{"reopen a bad superblock",	     check_reopen_bad_fails},
		{"reopen a slightly bad superblock", check_reopen_slightly_bad_fails},

		/* creation of virtual devices within the mmd */
		{"open non existent virtual device fails",	check_open_bad_msd},
		{"create a thin virtual device succeeds",	check_create_thin_msd},
		// FIXME: check you can't create the same device twice */

		{"open existing virtual device succeeds",	check_open_thin_msd},
		{"open existing virtual device twice fails",	check_open_msd_twice_fails},
		{"mmd close with open devices fails",		check_mmd_close_with_open_msd_fails},
		{"delete a thin virtual device succeeds",	check_delete_msd},

		// waiting for btree_remove()
		// {"opening a deleted virtual device fails",   check_open_of_deleted_msd_fails},

		{"lookup of empty virtual device fails",	check_empty_msd_lookup_fails},
		{"insert of a new mapping succeeds",		check_insert_succeeds},
		{"two inserted mappings differ (same dev)",	check_two_inserts_in_same_device_differ},
		{"two inserted mappings differ (diff devs)",	check_two_inserts_in_different_devices_differ},
		{"lookup after insert gives correct mapping",   check_lookup_after_insert},

		{"data space may be exhausted",			check_data_space_can_be_exhausted},
		{"data space may be exhausted (2 devs)",	check_data_space_can_be_exhausted_two_devs},

		{"create snapshot",		                 check_create_snapshot},
		{"fresh snapshots have same mappings as origin", check_fresh_snapshot_has_same_mappings},
		{"snapshot scenario1",                           check_snap_scenario1},
		{"get workqueue", check_get_workqueue},
	};

	int i, r;

	for (i = 0; i < sizeof(table_) / sizeof(*table_); i++) {
		r = run_test(table_[i].name, table_[i].fn);
		if (r)
			break;
	}

	return r;
}

static void multisnap_metadata_test_exit(void)
{
}

module_init(multisnap_metadata_test_init);
module_exit(multisnap_metadata_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joe Thornber");
MODULE_DESCRIPTION("Test code for multisnap metadata code");

/*----------------------------------------------------------------*/
