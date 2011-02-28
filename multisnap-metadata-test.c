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

#define DATA_DEV_SIZE 10000
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

struct test_context {
	struct block_device *bdev;
	struct multisnap_metadata *mmd;
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
	int mode = FMODE_READ | FMODE_WRITE | FMODE_EXCL;

	multisnap_metadata_close(tc->mmd);
	blkdev_put(tc->bdev, mode);
	return 0;
}

/*----------------------------------------------------------------*/

static int check_create_mmd(void)
{
	int r;
	struct test_context tc;

	r = memset_block(TEST_DEVICE, 0, 0);
	if (r)
		return r;

	r = create_mmd(&tc);
	if (r)
		return r;

	return destroy_mmd(&tc);
}

static int check_reopen_mmd(void)
{
	int r;
	struct test_context tc;

	r = memset_block(TEST_DEVICE, 0, 0);
	if (r)
		return r;

	r = create_mmd(&tc);
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

	/* setup a valid mmd */
	r = memset_block(TEST_DEVICE, 0, 0);
	if (r) {
		printk(KERN_ALERT "memset failed");
		return r;
	}

	r = create_mmd(&tc);
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
		{"create new metadata device",		check_create_mmd},
		{"reopen metadata device",		check_reopen_mmd},
		{"reopen a bad superblock",		check_reopen_bad_fails},
		{"reopen a slightly bad superblock",	check_reopen_slightly_bad_fails}
	};

	int i, r = 0;

	for (i = 0; i < sizeof(table_) / sizeof(*table_); i++)
		r |= run_test(table_[i].name, table_[i].fn);

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
