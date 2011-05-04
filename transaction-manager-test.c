#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/blkdev.h>

#include "md/persistent-data/dm-transaction-manager.h"
#include "md/persistent-data/dm-space-map-core.h"

/*----------------------------------------------------------------*/

#define NR_BLOCKS 1024
#define BM_BLOCK_SIZE 4096
#define CACHE_SIZE 16

typedef int (*test_fn)(struct dm_transaction_manager *);

/*----------------------------------------------------------------*/

static int check_commit(struct dm_transaction_manager *tm)
{
	int r, i;
	dm_block_t sb;
	struct dm_block *superblock;

	r = dm_tm_begin(tm);
	if (r < 0)
		return r;

	r = dm_tm_new_block(tm, &superblock);
	if (r < 0)
		return r;

	for (i = 0; i < 10; i++) {
		struct dm_block *b;
		r = dm_tm_new_block(tm, &b);
		if (r < 0)
			return r;
	}

	r = dm_tm_pre_commit(tm);
	if (r < 0)
		return r;

	sb = dm_block_location(superblock);

	r = dm_tm_commit(tm, superblock);
	if (r < 0)
		return r;

	/* check the lock on superblock was dropped */
	r = dm_tm_read_lock(tm, sb, &superblock);
	if (r < 0)
		return r;

	return 0;
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

static int transaction_manager_test_init(void)
{
	static struct {
		const char *name;
		test_fn fn;
	} table_[] = {
		{"check commit", check_commit}
	};

	int i;

	for (i = 0; i < sizeof(table_) / sizeof(*table_); i++)
		run_test(table_[i].name, table_[i].fn);

	return 0;
}

static void transaction_manager_test_exit(void)
{
}

module_init(transaction_manager_test_init);
module_exit(transaction_manager_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joe Thornber");
MODULE_DESCRIPTION("Test code for transaction manager");

/*----------------------------------------------------------------*/
