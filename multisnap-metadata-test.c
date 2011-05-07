#include <linux/blkdev.h>
#include <linux/device-mapper.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "md/persistent-data/dm-block-manager.h"
#include "md/dm-multisnap-metadata.h"

/*----------------------------------------------------------------*/

#define NR_BLOCKS 1024
#define BM_BLOCK_SIZE 4096
#define CACHE_SIZE 16
#define METADATA_BLOCK_SIZE 4096
#define DATA_BLOCK_SIZE ((1024 * 1024 * 128) >> SECTOR_SHIFT)

#define DATA_DEV_SIZE 512
#define TEST_DEVICE "/dev/sdc"	/* FIXME: get this from module parameters */

/*----------------------------------------------------------------*/

struct multisnap_map_result {
	dm_block_t origin;
	dm_block_t dest;
	int need_copy;
};

static int multisnap_metadata_map(struct dm_ms_device *msd,
				   dm_block_t block,
				   int io_direction,
				   int can_block,
				   struct multisnap_map_result *result)
{
	int r;
	struct dm_multisnap_lookup_result lookup_result;

	r = dm_multisnap_metadata_lookup(msd, block, can_block, &lookup_result);

	if (r) {
		if (r == -ENODATA && io_direction == WRITE) {
			r = dm_multisnap_metadata_alloc_data_block(msd, &result->dest);
			if (r)
				return r;

			r = dm_multisnap_metadata_insert(msd, block, result->dest);
			if (r) {
				dm_multisnap_metadata_free_data_block(msd, result->dest);
				return r;
			}

			result->origin = result->dest;
			result->need_copy = 0;
		} else
			return r;
	} else {
		result->origin = lookup_result.block;
		if (io_direction == WRITE && lookup_result.shared) {

			r = dm_multisnap_metadata_alloc_data_block(msd, &result->dest);
			if (r)
				return r;

			r = dm_multisnap_metadata_insert(msd, block, result->dest);
			if (r) {
				dm_multisnap_metadata_free_data_block(msd, result->dest);
				return r;
			}

			result->need_copy = 1;
		} else {
			result->dest = lookup_result.block;
			result->need_copy = 0;
		}
	}

	return 0;
}

static int with_block(const char *path, dm_block_t blk,
		      void (*fn)(void *, void *),
		      void *context)
{
	int r;
	struct dm_block_manager *bm;
	struct block_device *bdev;
	struct dm_block *b;
	int mode = FMODE_READ | FMODE_WRITE | FMODE_EXCL;
	bdev = blkdev_get_by_path(path, mode, &with_block);
	if (IS_ERR(bdev)) {
		printk(KERN_ALERT "blkdev_get_by_path failed");
		return -1;
	}

	bm = dm_block_manager_create(bdev, METADATA_BLOCK_SIZE, 1);
	if (!bm) {
		printk(KERN_ALERT "%s: couldn't create bm", __func__);
		return -1;
	}

	r = dm_bm_write_lock(bm, blk, &b);
	if (r)
		printk(KERN_ALERT "%s: couldn't lock block",
		       __func__);
	else {
		fn(context, dm_block_data(b));
		dm_bm_flush_and_unlock(bm, b);
	}

	dm_block_manager_destroy(bm);
	blkdev_put(bdev, mode);
	return r;
}

/*--------------------------------*/

static void memset_(void *context, void *data)
{
	unsigned char *v = (unsigned char *) context;
	memset(data, *v, METADATA_BLOCK_SIZE);
}

static int memset_block(const char *path, dm_block_t blk, unsigned char v)
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

static int set_block_byte(const char *path, dm_block_t blk, size_t offset, unsigned char v)
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
	struct dm_multisnap_metadata *mmd;

	unsigned nr_msd;
	struct dm_ms_device *msd[MAX_MSD];
};

static int create_mmd(struct test_context *tc)
{
	int mode = FMODE_READ | FMODE_WRITE | FMODE_EXCL;

	memset(tc, 0, sizeof(*tc));

	tc->bdev = blkdev_get_by_path(TEST_DEVICE, mode, &create_mmd);
	if (IS_ERR(tc->bdev))
		return -1;

	tc->mmd = dm_multisnap_metadata_open(tc->bdev,
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
			r = dm_multisnap_metadata_close_device(tc->msd[i]);
			if (r) {
				printk(KERN_ALERT "mmd_close_device failed");
				return r;
			}
		}
	}

	r = dm_multisnap_metadata_close(tc->mmd);
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

	return create_mmd(tc);
}

static int setup_fresh_mmd_and_thin(struct test_context *tc,
				    dm_multisnap_dev_t id)
{
	int r;

	r = setup_fresh_mmd(tc);
	if (r)
		return r;

	r = dm_multisnap_metadata_create_thin(tc->mmd, id, 0);
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
		r = dm_multisnap_metadata_create_thin(tc->mmd, i, 0);
		if (r) {
			printk(KERN_ALERT "mmd_create_thin failed");
			destroy_mmd(tc);
		}

		r = dm_multisnap_metadata_open_device(tc->mmd, i, tc->msd + i);
		if (r) {
			printk(KERN_ALERT "mmd open_device failed");
			destroy_mmd(tc);
			return r;
		}

		tc->nr_msd++;
	}

	return 0;
}

static int open_dev(struct test_context *tc, dm_multisnap_dev_t dev, unsigned *index)
{
	int r = dm_multisnap_metadata_open_device(tc->mmd, dev, tc->msd + tc->nr_msd);
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
	struct dm_ms_device *msd;

	r = setup_fresh_mmd(&tc);
	if (r)
		return r;

	r = dm_multisnap_metadata_open_device(tc.mmd, 0, &msd);
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
	struct dm_ms_device *msd;
	struct test_context tc;

	r = setup_fresh_mmd_and_thin(&tc, 0);
	if (r)
		return r;

	r = dm_multisnap_metadata_open_device(tc.mmd, 0, &msd);
	if (r) {
		printk(KERN_ALERT "mmd_open_device failed");
		destroy_mmd(&tc);
		return r;
	}

	r = dm_multisnap_metadata_close_device(msd);
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
	struct dm_ms_device *msd, *msd2;
	struct test_context tc;

	r = setup_fresh_mmd_and_thin(&tc, 0);
	if (r)
		return r;

	r = dm_multisnap_metadata_open_device(tc.mmd, 0, &msd);
	if (r) {
		printk(KERN_ALERT "mmd_open_device failed");
		destroy_mmd(&tc);
		return r;
	}

	r = dm_multisnap_metadata_open_device(tc.mmd, 0, &msd2);
	if (!r) {
		printk(KERN_ALERT "mmd_open_device (for the second time) unexpected succeeded");
		dm_multisnap_metadata_close_device(msd);
		destroy_mmd(&tc);
		return r;
	}

	r = dm_multisnap_metadata_close_device(msd);
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
	struct dm_ms_device *msd;
	struct test_context tc;

	r = setup_fresh_mmd_and_thin(&tc, 0);
	if (r)
		return r;

	r = dm_multisnap_metadata_open_device(tc.mmd, 0, &msd);
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
	r = dm_multisnap_metadata_close_device(msd);
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

	r = dm_multisnap_metadata_delete_device(tc.mmd, 0);
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
	struct dm_ms_device *msd;
	struct test_context tc;

	r = setup_fresh_mmd_and_thin(&tc, 0);
	if (r)
		return r;

	r = dm_multisnap_metadata_delete_device(tc.mmd, 0);
	if (r) {
		printk(KERN_ALERT "mmd_delete failed");
		destroy_mmd(&tc);
		return r;
	}

	r = dm_multisnap_metadata_open_device(tc.mmd, 0, &msd);
	if (!r) {
		printk(KERN_ALERT "mmd open_device unexpectedly succeeded");
		dm_multisnap_metadata_close_device(msd);
		destroy_mmd(&tc);
		return -1;
	}

	return destroy_mmd(&tc);
}

static int check_empty_msd_lookup_fails(void)
{
	int r;
	struct test_context tc;
	struct multisnap_map_result mapping;

	r = setup_fresh_and_open_thins(&tc, 1);
	if (r)
		return r;

	r = multisnap_metadata_map(tc.msd[0], 0, READ, 1, &mapping);
	if (!r) {
		printk(KERN_ALERT "mmd_lookup unexpectedly succeeded");
		destroy_mmd(&tc);
		return -1;
	}

	if (mapping.need_copy) {
		printk(KERN_ALERT "clone unexpectedly set");
		destroy_mmd(&tc);
		return -1;
	}

	return destroy_mmd(&tc);
}

static int check_insert_succeeds(void)
{
	int r;
	struct test_context tc;
	struct multisnap_map_result mapping;

	r = setup_fresh_and_open_thins(&tc, 1);
	if (r)
		return r;

	r = multisnap_metadata_map(tc.msd[0], 0, WRITE, 1, &mapping);
	if (r) {
		printk(KERN_ALERT "mmd_insert failed");
		destroy_mmd(&tc);
		return r;
	}

	if (mapping.need_copy) {
		printk(KERN_ALERT "clone unexpectedly set");
		destroy_mmd(&tc);
		return -1;
	}

	return destroy_mmd(&tc);
}

static int check_two_inserts_in_same_device_differ(void)
{
	int r;
	struct test_context tc;
	struct multisnap_map_result result1, result2;

	r = setup_fresh_and_open_thins(&tc, 1);
	if (r)
		return r;

	r = multisnap_metadata_map(tc.msd[0], 0, WRITE, 1, &result1);
	if (r) {
		printk(KERN_ALERT "mmd_insert failed");
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_map(tc.msd[0], 1, WRITE, 1, &result2);
	if (r) {
		printk(KERN_ALERT "mmd_insert failed");
		destroy_mmd(&tc);
		return r;
	}

	if (result1.dest == result2.dest) {
		printk(KERN_ALERT "mmd_inserts mapped to same destination");
		destroy_mmd(&tc);
		return -1;
	}

	if (result1.need_copy || result2.need_copy) {
		printk(KERN_ALERT "clone unexpectedly set");
		destroy_mmd(&tc);
		return -1;
	}

	return destroy_mmd(&tc);
}

static int check_lookup_after_insert(void)
{
	int r;
	struct test_context tc;
	struct multisnap_map_result result1, result2;

	r = setup_fresh_and_open_thins(&tc, 1);
	if (r)
		return r;

	r = multisnap_metadata_map(tc.msd[0], 0, WRITE, 1, &result1);
	if (r) {
		printk(KERN_ALERT "mmd_insert failed");
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_map(tc.msd[0], 0, READ, 1, &result2);
	if (r) {
		printk(KERN_ALERT "mmd_lookup failed");
		destroy_mmd(&tc);
		return r;
	}

	if (result1.dest != result2.dest) {
		printk(KERN_ALERT "mmd_insert and mmd_lookup returned different blocks");
		destroy_mmd(&tc);
		return r;
	}

	if (result1.need_copy || result2.need_copy) {
		printk(KERN_ALERT "clone unexpectedly set");
		destroy_mmd(&tc);
		return -1;
	}

	return destroy_mmd(&tc);
}

static int check_two_inserts_in_different_devices_differ(void)
{
	int r;
	struct test_context tc;
	struct multisnap_map_result result1, result2;

	r = setup_fresh_and_open_thins(&tc, 2);
	if (r)
		return r;

	r = multisnap_metadata_map(tc.msd[0], 0, WRITE, 1, &result1);
	if (r) {
		printk(KERN_ALERT "mmd_insert failed");
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_map(tc.msd[1], 0, WRITE, 1, &result2);
	if (r) {
		printk(KERN_ALERT "mmd_insert failed");
		destroy_mmd(&tc);
		return r;
	}

	if (result1.dest == result2.dest) {
		printk(KERN_ALERT "mmd_inserts mapped to same destination");
		destroy_mmd(&tc);
		return -1;
	}

	if (result1.need_copy || result2.need_copy) {
		printk(KERN_ALERT "clone unexpectedly set");
		destroy_mmd(&tc);
		return -1;
	}

	return destroy_mmd(&tc);
}

static int check_data_space_can_be_exhausted(void)
{
	int r;
	struct test_context tc;
	struct multisnap_map_result result;
	unsigned i;

	r = setup_fresh_and_open_thins(&tc, 1);
	if (r)
		return r;

	/* use up all the available data space */
	for (i = 0; i < DATA_DEV_SIZE; i++) {
		r = multisnap_metadata_map(tc.msd[0], i, WRITE, 1, &result);
		if (r) {
			printk(KERN_ALERT "mmd_insert failed");
			destroy_mmd(&tc);
			return r;
		}
	}

	/* next insert should fail */
	r = multisnap_metadata_map(tc.msd[0], i, WRITE, 1, &result);
	if (r != -ENOSPC) {
		printk(KERN_ALERT "insert unexpectedly succeeded");
		return -1;
	}

	return destroy_mmd(&tc);
}

static int check_data_space_can_be_exhausted_two_devs(void)
{
	int r;
	struct test_context tc;
	struct multisnap_map_result result;
	unsigned i;

	r = setup_fresh_and_open_thins(&tc, 2);
	if (r)
		return r;

	/* use up all the available data space */
	for (i = 0; i < DATA_DEV_SIZE; i++) {
		r = multisnap_metadata_map(tc.msd[i % 2], i, WRITE, 1, &result);
		if (r) {
			printk(KERN_ALERT "mmd_insert failed");
			destroy_mmd(&tc);
			return r;
		}
	}

	/* next insert should fail */
	r = multisnap_metadata_map(tc.msd[i % 2], i, WRITE, 1, &result);
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

	r = dm_multisnap_metadata_create_snap(tc.mmd, 1, 0);
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
	struct multisnap_map_result result1, result2;

	r = setup_fresh_and_open_thins(&tc, 1);
	if (r)
		return r;

	for (i = 0; i < 10; i++) {
		r = multisnap_metadata_map(tc.msd[0], i, WRITE, 1, &result1);
		if (r) {
			printk(KERN_ALERT "mmd_insert failed");
			destroy_mmd(&tc);
			return r;
		}
	}

	r = dm_multisnap_metadata_create_snap(tc.mmd, 1, 0);
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
		r = multisnap_metadata_map(tc.msd[0], i, READ, 1, &result1);
		if (r) {
			printk(KERN_ALERT "mmd_lookup1 failed");
			destroy_mmd(&tc);
			return r;
		}

		r = multisnap_metadata_map(tc.msd[index], i, READ, 1, &result2);
		if (r) {
			printk(KERN_ALERT "mmd_lookup2 failed");
			destroy_mmd(&tc);
			return r;
		}

		if (result1.dest != result2.dest) {
			printk(KERN_ALERT "blocks differ %u != %u",
			       (unsigned) result1.dest, (unsigned) result2.dest);
			destroy_mmd(&tc);
			return r;
		}

		if (result1.need_copy || result2.need_copy) {
			printk(KERN_ALERT "clone unexpectedly set");
			destroy_mmd(&tc);
			return -1;
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
	struct multisnap_map_result result1, result2;

	r = setup_fresh_and_open_thins(&tc, 1);
	if (r)
		return r;

	/* make sure one block is mapped on the origin */
	r = multisnap_metadata_map(tc.msd[0], 0, WRITE, 1, &result1);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	r = dm_multisnap_metadata_create_snap(tc.mmd, 1, 0);
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

	r = multisnap_metadata_map(tc.msd[index], 0, WRITE, 1, &result2);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (result1.dest == result2.dest) {
		printk(KERN_ALERT "blocks unexpectedly match %u",
		       (unsigned) result1.dest);
		destroy_mmd(&tc);
		return -1;
	}

	if (!result2.need_copy || result2.origin != result1.dest) {
		printk(KERN_ALERT "bad clone value");
		destroy_mmd(&tc);
		return -1;
	}

	r = multisnap_metadata_map(tc.msd[index], 0, READ, 1, &result1);
	if (r) {
		printk(KERN_ALERT "mmd_map2 failed");
		destroy_mmd(&tc);
		return r;
	}

	if (result1.dest != result2.dest) {
		printk(KERN_ALERT "blocks differ");
		destroy_mmd(&tc);
		return -1;
	}

	if (result1.need_copy) {
		printk(KERN_ALERT "bad clone value");
		destroy_mmd(&tc);
		return -1;
	}

	r = multisnap_metadata_map(tc.msd[index], 0, WRITE, 1, &result1);
	if (r) {
		printk(KERN_ALERT "mmd_map3 failed");
		destroy_mmd(&tc);
		return -1;
	}

	if (result1.dest != result2.dest) {
		printk(KERN_ALERT "blocks differ (2)");
		destroy_mmd(&tc);
		return -1;
	}

	if (result1.need_copy) {
		printk(KERN_ALERT "bad clone value");
		destroy_mmd(&tc);
		return -1;
	}

	r = multisnap_metadata_map(tc.msd[index], 0, READ, 1, &result1);
	if (r) {
		printk(KERN_ALERT "mmd_map4 failed");
		destroy_mmd(&tc);
		return -1;
	}

	if (result1.dest != result2.dest) {
		printk(KERN_ALERT "blocks differ (3)");
		destroy_mmd(&tc);
		return -1;
	}

	if (result1.need_copy) {
		printk(KERN_ALERT "bad clone value");
		destroy_mmd(&tc);
		return -1;
	}

	return destroy_mmd(&tc);
}

/* Scenario 2
 * 1 - origin <- snap1
 * 2 - snap1 <- snap2
 * 3 - write snap1 => IO_MAPPED
 * 4 - snap1 <- snap3
 * 5 - read snap2 => IO_MAPPED (!3)
 * 6 - read snap3 => IO_MAPPED (3)
 */
static int check_snap_scenario2(void)
{
	int r;
	unsigned index_snap1, index_snap2, index_snap3;
	struct test_context tc;
	struct multisnap_map_result result1, result2;

	r = setup_fresh_and_open_thins(&tc, 1);
	if (r)
		return r;

	/* make sure one block is mapped on the origin */
	r = multisnap_metadata_map(tc.msd[0], 0, WRITE, 1, &result1);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	/* 1 */
	r = dm_multisnap_metadata_create_snap(tc.mmd, 1, 0);
	if (r) {
		printk(KERN_ALERT "mmd_create_snap failed");
		destroy_mmd(&tc);
		return r;
	}

	/* 2 */
	r = dm_multisnap_metadata_create_snap(tc.mmd, 2, 1);
	if (r) {
		printk(KERN_ALERT "snapshot of snapshot failed");
		destroy_mmd(&tc);
		return r;
	}

	/* 3 */
	r = open_dev(&tc, 1, &index_snap1);
	if (r) {
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_map(tc.msd[index_snap1], 0, WRITE, 1, &result1);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (!result1.need_copy) {
		printk(KERN_ALERT "bad clone value (3)");
		destroy_mmd(&tc);
		return -1;
	}

	/* 4 */
	r = dm_multisnap_metadata_create_snap(tc.mmd, 3, 1);
	if (r) {
		printk(KERN_ALERT "snapshot of snapshot failed");
		destroy_mmd(&tc);
		return r;
	}

	/* 5 */
	r = open_dev(&tc, 2, &index_snap2);
	if (r) {
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_map(tc.msd[index_snap2], 0, READ, 1, &result2);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (result1.dest == result2.dest) {
		printk(KERN_ALERT "blocks match (5)");
		destroy_mmd(&tc);
		return r;
	}

	if (result2.need_copy) {
		printk(KERN_ALERT "bad clone value (5)");
		destroy_mmd(&tc);
		return -1;
	}

	/* 6 */
	r = open_dev(&tc, 3, &index_snap3);
	if (r) {
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_map(tc.msd[index_snap3], 0, READ, 1, &result2);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (result1.dest != result2.dest) {
		printk(KERN_ALERT "block differ (5)");
		destroy_mmd(&tc);
		return r;
	}

	if (result2.need_copy) {
		printk(KERN_ALERT "bad clone value (6)");
		destroy_mmd(&tc);
		return -1;
	}

	return destroy_mmd(&tc);
}

/* Scenario 3
 * 1 - origin1 <- snap1
 * 2 - origin2 <- snap2
 * 3 - write snap1 => IO_MAPPED
 * 4 - read snap1 => IO_MAPPED to (3)
 * 5 - read snap2 => IO_MAPPED to origin1
 * 6 - write snap2 => IO_MAPPED
 * 7 - read snap1 => IO_MAPPED to (3)
 * 8 - read snap2 => IO_MAPPED to (6)
 */
static int check_snap_scenario3(void)
{
	int r;
	int const snap1_dev = 2, snap2_dev = 3;
	unsigned index_snap1, index_snap2;
	struct test_context tc;
	struct multisnap_map_result block, result1, result3, result6;

	r = setup_fresh_and_open_thins(&tc, 2);
	if (r)
		return r;

	/* make sure one block is mapped on the origin */
	r = multisnap_metadata_map(tc.msd[0], 0, WRITE, 1, &result1);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_map(tc.msd[1], 0, WRITE, 1, &block);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	/* 1 */
	r = dm_multisnap_metadata_create_snap(tc.mmd, snap1_dev, 0);
	if (r) {
		printk(KERN_ALERT "mmd_create_snap failed");
		destroy_mmd(&tc);
		return r;
	}

	/* 2 */
	r = dm_multisnap_metadata_create_snap(tc.mmd, snap2_dev, 1);
	if (r) {
		printk(KERN_ALERT "snapshot of snapshot failed");
		destroy_mmd(&tc);
		return r;
	}

	/* 3 */
	r = open_dev(&tc, snap1_dev, &index_snap1);
	if (r) {
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_map(tc.msd[index_snap1], 0, WRITE, 1, &result3);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	/* 4 */
	r = multisnap_metadata_map(tc.msd[index_snap1], 0, READ, 1, &block);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (block.dest != result3.dest) {
		printk(KERN_ALERT "block differ (4)");
		destroy_mmd(&tc);
		return r;
	}

	/* 5 */
	r = open_dev(&tc, snap2_dev, &index_snap2);
	if (r) {
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_map(tc.msd[index_snap2], 0, READ, 1, &block);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (block.dest != result1.dest) {
		printk(KERN_ALERT "block differ (5)");
		destroy_mmd(&tc);
		return r;
	}

	/* 6 */
	r = multisnap_metadata_map(tc.msd[index_snap2], 0, WRITE, 1, &result6);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (!result6.need_copy) {
		printk(KERN_ALERT "bad clone value");
		destroy_mmd(&tc);
		return -1;
	}

	/* 7 */
	r = multisnap_metadata_map(tc.msd[index_snap1], 0, READ, 1, &block);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (block.dest != result3.dest) {
		printk(KERN_ALERT "blocks differ (7)");
		destroy_mmd(&tc);
		return r;
	}

	/* 8 */
	r = multisnap_metadata_map(tc.msd[index_snap2], 0, READ, 1, &block);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (block.dest != result6.dest) {
		printk(KERN_ALERT "blocks differ (8)");
		destroy_mmd(&tc);
		return r;
	}

	return destroy_mmd(&tc);
}

/* Scenario 4
 * 1 - origin <- snap1
 * 2 - write snap1 => IO_MAPPED
 * 3 - snap1 <- snap2
 * 4 - write snap1 => IO_MAPPED (!2)
 * 5 - snap1 <- snap3
 * 6 - read snap2 => IO_MAPPED (2)
 * 7 - read snap3 => IO_MAPPED (4)
 */
static int check_snap_scenario4(void)
{
	int r;
	int const snap1_dev = 1, snap2_dev = 2, snap3_dev = 3;
	unsigned index_snap1, index_snap2, index_snap3;
	struct test_context tc;
	struct multisnap_map_result block, result2, result4;

	r = setup_fresh_and_open_thins(&tc, 1);
	if (r)
		return r;

	/* make sure one block is mapped on the origin */
	r = multisnap_metadata_map(tc.msd[0], 0, WRITE, 1, &block);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	/* 1 */
	r = dm_multisnap_metadata_create_snap(tc.mmd, snap1_dev, 0);
	if (r) {
		printk(KERN_ALERT "mmd_create_snap failed");
		destroy_mmd(&tc);
		return r;
	}

	/* 2 */
	r = open_dev(&tc, snap1_dev, &index_snap1);
	if (r) {
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_map(tc.msd[index_snap1], 0, WRITE, 1, &result2);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (!result2.need_copy) {
		printk(KERN_ALERT "bad clone value");
		destroy_mmd(&tc);
		return -1;
	}

	/* 3 */
	r = dm_multisnap_metadata_create_snap(tc.mmd, snap2_dev, snap1_dev);
	if (r) {
		printk(KERN_ALERT "mmd_create_snap failed");
		destroy_mmd(&tc);
		return r;
	}

	/* 4 */
	r = multisnap_metadata_map(tc.msd[index_snap1], 0, WRITE, 1, &result4);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (result4.dest != result2.dest) {
		printk(KERN_ALERT "block differ (4)");
		destroy_mmd(&tc);
		return r;
	}

	if (!result4.need_copy) {
		printk(KERN_ALERT "bad clone value");
		destroy_mmd(&tc);
		return -1;
	}

	/* 5 */
	r = dm_multisnap_metadata_create_snap(tc.mmd, snap3_dev, snap1_dev);
	if (r) {
		printk(KERN_ALERT "mmd_create_snap failed");
		destroy_mmd(&tc);
		return r;
	}

	/* 6 */
	r = open_dev(&tc, snap2_dev, &index_snap2);
	if (r) {
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_map(tc.msd[index_snap2], 0, READ, 1, &block);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (block.dest != result2.dest) {
		printk(KERN_ALERT "block differ (6)");
		destroy_mmd(&tc);
		return r;
	}

	/* 7 */
	r = open_dev(&tc, snap3_dev, &index_snap3);
	if (r) {
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_map(tc.msd[index_snap3], 0, READ, 1, &block);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (block.dest != result4.dest) {
		printk(KERN_ALERT "block differ (7)");
		destroy_mmd(&tc);
		return r;
	}

	return destroy_mmd(&tc);
}

/* Scenario 5
 * 1 - origin <- snap1
 *     snap1 <- snap2
 * 2 - write snap2 => IO_MAPPED
 * 3 - snap2 <- snap3
 * 4 - read snap2 => IO_MAPPED (2)
 * 5 - read snap3 => IO_MAPPED (2)
 * 6 - write snap2 => IO_MAPPED (not 2)
 * 7 - read snap2 => IO_MAPPED (6)
 * 8 - read snap3 => IO_MAPPED (2)
 * 9 - write snap3 => IO_MAPPED (!2, !6)
 * 10 - read snap3 => IO_MAPPED (9)
 * 11 - read snap2 => IO_MAPPED (6)
 */
static int check_snap_scenario5(void)
{
	int r;
	int const snap1_dev = 1, snap2_dev = 2, snap3_dev = 3;
	unsigned index_snap2, index_snap3;
	struct test_context tc;
	struct multisnap_map_result block, result2, result6, result9;

	r = setup_fresh_and_open_thins(&tc, 1);
	if (r)
		return r;

	/* make sure one block is mapped on the origin */
	r = multisnap_metadata_map(tc.msd[0], 0, WRITE, 1, &block);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	/* 1 */
	r = dm_multisnap_metadata_create_snap(tc.mmd, snap1_dev, 0);
	if (r) {
		printk(KERN_ALERT "mmd_create_snap failed");
		destroy_mmd(&tc);
		return r;
	}

	r = dm_multisnap_metadata_create_snap(tc.mmd, snap2_dev, snap1_dev);
	if (r) {
		printk(KERN_ALERT "mmd_create_snap failed");
		destroy_mmd(&tc);
		return r;
	}

	/* 2 */
	r = open_dev(&tc, snap2_dev, &index_snap2);
	if (r) {
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_map(tc.msd[index_snap2], 0, WRITE, 1, &result2);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (!result2.need_copy) {
		printk(KERN_ALERT "bad clone value");
		destroy_mmd(&tc);
		return -1;
	}

	/* 3 */
	r = dm_multisnap_metadata_create_snap(tc.mmd, snap3_dev, snap2_dev);
	if (r) {
		printk(KERN_ALERT "mmd_create_snap failed");
		destroy_mmd(&tc);
		return r;
	}

	/* 4 */
	r = multisnap_metadata_map(tc.msd[index_snap2], 0, READ, 1, &block);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (block.dest != result2.dest) {
		printk(KERN_ALERT "block differ (4)");
		destroy_mmd(&tc);
		return r;
	}

	/* 5 */
	r = open_dev(&tc, snap3_dev, &index_snap3);
	if (r) {
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_map(tc.msd[index_snap3], 0, READ, 1, &block);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (block.dest != result2.dest) {
		printk(KERN_ALERT "block differ (5)");
		destroy_mmd(&tc);
		return r;
	}

	/* 6 */
	r = multisnap_metadata_map(tc.msd[index_snap2], 0, WRITE, 1, &result6);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (result6.dest == result2.dest) {
		printk(KERN_ALERT "block same (6)");
		destroy_mmd(&tc);
		return r;
	}

	/* 7 */
	r = multisnap_metadata_map(tc.msd[index_snap2], 0, READ, 1, &block);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (block.dest != result6.dest) {
		printk(KERN_ALERT "block differ (7)");
		destroy_mmd(&tc);
		return r;
	}

	/* 8 */
	r = multisnap_metadata_map(tc.msd[index_snap3], 0, READ, 1, &block);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (block.dest != result2.dest) {
		printk(KERN_ALERT "block differ (8)");
		destroy_mmd(&tc);
		return r;
	}

	/* 9 */
	r = multisnap_metadata_map(tc.msd[index_snap3], 0, WRITE, 1, &result9);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if ((result9.dest == result2.dest) || (result9.dest == result6.dest)) {
		printk(KERN_ALERT "block same (9)");
		destroy_mmd(&tc);
		return r;
	}

	/* 10 */
	r = multisnap_metadata_map(tc.msd[index_snap3], 0, READ, 1, &block);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (block.dest != result9.dest) {
		printk(KERN_ALERT "block differ (10)");
		destroy_mmd(&tc);
		return r;
	}

	/* 11 */
	r = multisnap_metadata_map(tc.msd[index_snap2], 0, READ, 1, &block);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (block.dest != result6.dest) {
		printk(KERN_ALERT "block differ (11)");
		destroy_mmd(&tc);
		return r;
	}

	return destroy_mmd(&tc);

}

static int check_devices_persist(void)
{
	int r;
	struct test_context tc;
	struct multisnap_map_result block, result1;
	unsigned index;

	r = setup_fresh_and_open_thins(&tc, 1);
	if (r)
		return r;

	/* make sure one block is mapped on the origin */
	r = multisnap_metadata_map(tc.msd[0], 0, WRITE, 1, &result1);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	r = dm_multisnap_metadata_commit(tc.mmd);
	if (r) {
		printk(KERN_ALERT "commit failed");
		destroy_mmd(&tc);
		return r;
	}

	r = destroy_mmd(&tc);
	if (r) {
		printk(KERN_ALERT "destroy_mmd failed");
		return r;
	}

	r = create_mmd(&tc);
	if (r) {
		printk(KERN_ALERT "couldn't recreate mmd");
		return r;
	}

	r = open_dev(&tc, 0, &index);
	if (r) {
		printk(KERN_ALERT "couldn't reopen device");
		destroy_mmd(&tc);
		return r;
	}

	r = multisnap_metadata_map(tc.msd[index], 0, READ, 1, &block);
	if (r) {
		printk(KERN_ALERT "mmd_map failed");
		destroy_mmd(&tc);
		return r;
	}

	if (block.dest != result1.dest) {
		printk(KERN_ALERT "blocks differ");
		destroy_mmd(&tc);
		return r;
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
		// {"reopen a slightly bad superblock", check_reopen_slightly_bad_fails},

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
		{"snapshot scenario 1",                           check_snap_scenario1},
		{"snapshot scenario 2",                           check_snap_scenario2},
		{"snapshot scenario 3",                           check_snap_scenario3},
		{"snapshot scenario 4",                           check_snap_scenario4},
		{"snapshot scenario 5",                           check_snap_scenario5},

		{"devices persist",    check_devices_persist},
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
