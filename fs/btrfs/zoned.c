// SPDX-License-Identifier: GPL-2.0

#include <linux/slab.h>
#include <linux/blkdev.h>
#include "ctree.h"
#include "volumes.h"
#include "zoned.h"
#include "rcu-string.h"

/* Maximum number of zones to report per blkdev_report_zones() call */
#define BTRFS_REPORT_NR_ZONES   4096

static int copy_zone_info_cb(struct blk_zone *zone, unsigned int idx,
			     void *data)
{
	struct blk_zone *zones = data;

	memcpy(&zones[idx], zone, sizeof(*zone));

	return 0;
}

static int btrfs_get_dev_zones(struct btrfs_device *device, u64 pos,
			       struct blk_zone *zones, unsigned int *nr_zones)
{
	int ret;

	if (!*nr_zones)
		return 0;

	ret = blkdev_report_zones(device->bdev, pos >> SECTOR_SHIFT, *nr_zones,
				  copy_zone_info_cb, zones);
	if (ret < 0) {
		btrfs_err_in_rcu(device->fs_info,
				 "get zone at %llu on %s failed %d", pos,
				 rcu_str_deref(device->name), ret);
		return ret;
	}
	*nr_zones = ret;
	if (!ret)
		return -EIO;

	return 0;
}

int btrfs_get_dev_zone_info(struct btrfs_device *device)
{
	struct btrfs_zoned_device_info *zone_info = NULL;
	struct block_device *bdev = device->bdev;
	sector_t nr_sectors = bdev->bd_part->nr_sects;
	sector_t sector = 0;
	struct blk_zone *zones = NULL;
	unsigned int i, nreported = 0, nr_zones;
	unsigned int zone_sectors;
	int ret;
	char devstr[sizeof(device->fs_info->sb->s_id) +
		    sizeof(" (device )") - 1];

	if (!bdev_is_zoned(bdev))
		return 0;

	if (device->zone_info)
		return 0;

	zone_info = kzalloc(sizeof(*zone_info), GFP_KERNEL);
	if (!zone_info)
		return -ENOMEM;

	zone_sectors = bdev_zone_sectors(bdev);
	ASSERT(is_power_of_2(zone_sectors));
	zone_info->zone_size = (u64)zone_sectors << SECTOR_SHIFT;
	zone_info->zone_size_shift = ilog2(zone_info->zone_size);
	zone_info->nr_zones = nr_sectors >> ilog2(bdev_zone_sectors(bdev));
	if (!IS_ALIGNED(nr_sectors, zone_sectors))
		zone_info->nr_zones++;

	zone_info->seq_zones = bitmap_zalloc(zone_info->nr_zones, GFP_KERNEL);
	if (!zone_info->seq_zones) {
		ret = -ENOMEM;
		goto out;
	}

	zone_info->empty_zones = bitmap_zalloc(zone_info->nr_zones, GFP_KERNEL);
	if (!zone_info->empty_zones) {
		ret = -ENOMEM;
		goto out;
	}

	zones = kcalloc(BTRFS_REPORT_NR_ZONES,
			sizeof(struct blk_zone), GFP_KERNEL);
	if (!zones) {
		ret = -ENOMEM;
		goto out;
	}

	/* Get zones type */
	while (sector < nr_sectors) {
		nr_zones = BTRFS_REPORT_NR_ZONES;
		ret = btrfs_get_dev_zones(device, sector << SECTOR_SHIFT, zones,
					  &nr_zones);
		if (ret)
			goto out;

		for (i = 0; i < nr_zones; i++) {
			if (zones[i].type == BLK_ZONE_TYPE_SEQWRITE_REQ)
				set_bit(nreported, zone_info->seq_zones);
			if (zones[i].cond == BLK_ZONE_COND_EMPTY)
				set_bit(nreported, zone_info->empty_zones);
			nreported++;
		}
		sector = zones[nr_zones - 1].start + zones[nr_zones - 1].len;
	}

	if (nreported != zone_info->nr_zones) {
		btrfs_err_in_rcu(device->fs_info,
				 "inconsistent number of zones on %s (%u / %u)",
				 rcu_str_deref(device->name), nreported,
				 zone_info->nr_zones);
		ret = -EIO;
		goto out;
	}

	kfree(zones);

	device->zone_info = zone_info;

	devstr[0] = 0;
	if (device->fs_info)
		snprintf(devstr, sizeof(devstr), " (device %s)",
			 device->fs_info->sb->s_id);

	rcu_read_lock();
	pr_info(
"BTRFS info%s: host-%s zoned block device %s, %u zones of %llu sectors",
		devstr,
		bdev_zoned_model(bdev) == BLK_ZONED_HM ? "managed" : "aware",
		rcu_str_deref(device->name), zone_info->nr_zones,
		zone_info->zone_size >> SECTOR_SHIFT);
	rcu_read_unlock();

	return 0;

out:
	kfree(zones);
	bitmap_free(zone_info->empty_zones);
	bitmap_free(zone_info->seq_zones);
	kfree(zone_info);

	return ret;
}

void btrfs_destroy_dev_zone_info(struct btrfs_device *device)
{
	struct btrfs_zoned_device_info *zone_info = device->zone_info;

	if (!zone_info)
		return;

	bitmap_free(zone_info->seq_zones);
	bitmap_free(zone_info->empty_zones);
	kfree(zone_info);
	device->zone_info = NULL;
}

int btrfs_get_dev_zone(struct btrfs_device *device, u64 pos,
		       struct blk_zone *zone)
{
	unsigned int nr_zones = 1;
	int ret;

	ret = btrfs_get_dev_zones(device, pos, zone, &nr_zones);
	if (ret != 0 || !nr_zones)
		return ret ? ret : -EIO;

	return 0;
}
