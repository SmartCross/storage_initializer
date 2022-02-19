#include <stdio.h>
#include <parted/parted.h>
#include <sys/mount.h>
#include <unistd.h>

static PedExceptionOption
exception_handler (PedException* ex)
{
  PedExceptionOption fix_is_an_option = (ex->options & PED_EXCEPTION_FIX);
  if (fix_is_an_option) {
    fprintf(stderr, "%s: %s (Automatically fixed)\n", ped_exception_get_type_string(ex->type), ex->message);
    return PED_EXCEPTION_FIX;
  }

  fprintf(stderr, "%s: %s\n", ped_exception_get_type_string(ex->type), ex->message);

  return PED_EXCEPTION_UNHANDLED;
}

#define PART_NAME "smartcross_userdata"
#define PART_PATH "/dev/disk/by-partlabel/" PART_NAME

int main() {
  int ret, fsck_ret, mkfs_ret;
  ped_exception_set_handler(exception_handler);

  const char* dev_path  = "/dev/mmcblk0";
  PedDevice* emmc = ped_device_get(dev_path);
  if (!ped_device_open(emmc)) {
    fprintf(stderr, "Error opening %s!\n", dev_path);
    return 1;
  }

  fprintf(stderr, "Model: %s (%d)\n", emmc->model, emmc->type);

  if (strcmp(emmc->model, "MMC 4FTE4R") != 0) {
    fprintf(stderr, "Wrong MMC model %s\n", emmc->model);
    ped_device_destroy(emmc);
    return 1;
  }

  PedDisk* disk = ped_disk_new(emmc);
  PedPartition* part;
  PedSector user_partition_start, user_partition_end;
  ped_unit_parse("2100MB", emmc, &user_partition_start, NULL);
  ped_unit_parse("3900MB", emmc, &user_partition_end, NULL);

  bool creatable = false, exists = false, busy = false;

  for (part = ped_disk_next_partition (disk, NULL); part;
       part = ped_disk_next_partition (disk, part)) {
    if (part->type & PED_PARTITION_FREESPACE) {
      if (part->geom.start < user_partition_start
        && part->geom.end > user_partition_end) {
        creatable = true;
      }
    } else if (ped_partition_is_active(part)) {
      if (strcmp(ped_partition_get_name(part), PART_NAME) == 0) {
        exists = true;
        busy = ped_partition_is_busy(part);
      }
    }
  }

  if (!exists) {
    if (creatable) {
      fprintf(stderr, "Adding user data partition to GPT table\n");
      part = ped_partition_new (disk, PED_PARTITION_NORMAL, NULL, user_partition_start, user_partition_end);
      ped_partition_set_name(part, PART_NAME);
      bool ok = ped_disk_add_partition(disk, part, ped_device_get_optimal_aligned_constraint(emmc));
      if (!ok) {
        fprintf(stderr, "Failed to add new partition\n");
        ret = 2;
        goto done;
      }
      ok = ped_disk_commit_to_dev(disk);

      if (!ok) {
        fprintf(stderr, "Failed to commit new partition to OS\n");
        ret = 2;
        goto done;
      }

      system("partprobe");
    } else {
      fprintf(stderr, "User data partition doesn't exist, but is not creatable\n");
      ret = 2;
      goto done;
    }
  } else {
    if (!busy)
      fprintf(stderr, "Found existing user data partition, checking\n");
    else
      fprintf(stderr, "Found mounted user data partition, exiting\n");
  }

  if (!busy) {
    while (access(PART_PATH, F_OK) != 0) {
      // file does not exist
      system("udevadm settle --exit-if-exists=" PART_PATH);
      usleep(5000); // wait for 5ms
    }

    fsck_ret = system("e2fsck -p " PART_PATH);
    if (fsck_ret != 0 && fsck_ret != 1) {
      fprintf(stderr, "User data partition is not initialized or corrupt. Formatting now...\n");
      mkfs_ret = system("mke2fs -t ext4 -F " PART_PATH);
      if (mkfs_ret != 0) {
        fprintf(stderr, "Failed to format partition\n");
        ret = 2;
        goto done;
      }
      mount(PART_PATH, "/data", "ext4", MS_NOATIME, NULL);
      system("tar x -z -f /usr/share/data-skeleton.tar.gz -C /data");
      umount(PART_PATH);
    }
  }

  ret = 0;
done:
  ped_disk_destroy(disk);
  ped_device_destroy(emmc);

  return ret;
}
