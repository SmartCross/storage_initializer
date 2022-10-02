#define _GNU_SOURCE

#include <stdio.h>
#include <parted/parted.h>
#include <sys/mount.h>
#include <unistd.h>

#include "ini.h"

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

typedef struct
{
    const char* mmc_device;
    const char* mmc_model;
    const char* space_start;
    const char* space_end;
    const char* skeleton;
    const char* part_label;
    const char* temp_mount_path;
} configuration;

static int handler(void* user, const char* section, const char* name,
                   const char* value)
{
    configuration* pconfig = (configuration*)user;

    #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
    if (MATCH("initializer", "device")) {
      pconfig->mmc_device = strdup(value);
    } else if (MATCH("initializer", "model")) {
      pconfig->mmc_model = strdup(value);
    } else if (MATCH("initializer", "space_start")) {
      pconfig->space_start = strdup(value);
    } else if (MATCH("initializer", "space_end")) {
      pconfig->space_end = strdup(value);
    } else if (MATCH("initializer", "skeleton")) {
      pconfig->skeleton = strdup(value);
    } else if (MATCH("initializer", "part_label")) {
      pconfig->part_label = strdup(value);
    } else if (MATCH("initializer", "temp_mount_path")) {
      pconfig->temp_mount_path = strdup(value);
    } else {
      return 0;  /* unknown section/name, error */
    }
    return 1;
}

void print_help();

int main(int argc, char *argv[]) {
  int ret, fsck_ret, mkfs_ret;
  ped_exception_set_handler(exception_handler);

  configuration config;
  memset(&config, 0, sizeof(config));

  if (argc <= 1) {
    print_help();
    return 0;
  }

  if (ini_parse(argv[1], handler, &config) < 0) {
    fprintf(stderr, "Can't load config file\n");
    return 1;
  }

  if (!config.mmc_device || 
    !config.mmc_model ||
    !config.space_start || 
    !config.skeleton || 
    !config.part_label || 
    !config.temp_mount_path
  ) {
    fprintf(stderr, "Missing config item\n");
    return 1;
  }

  PedDevice* emmc = ped_device_get(config.mmc_device);
  if (!ped_device_open(emmc)) {
    fprintf(stderr, "Error opening %s!\n", config.mmc_device);
    return 1;
  }

  fprintf(stderr, "Model: %s (%d)\n", emmc->model, emmc->type);
  if (strcmp(emmc->model, config.mmc_model) != 0) {
    fprintf(stderr, "eMMC model mismatch (expected %s)\n", config.mmc_model);
    return 2;
  }

  PedDisk* disk = ped_disk_new(emmc);
  PedPartition* part;
  PedSector user_partition_start, user_partition_end = 0;
  if (!ped_unit_parse(config.space_start, emmc, &user_partition_start, NULL)) {
    fprintf(stderr, "Invalid start position config\n");
    return 3;
  }
  if (config.space_end) {
    if (!ped_unit_parse(config.space_end, emmc, &user_partition_end, NULL)) {
      fprintf(stderr, "Invalid end position config\n");
      return 3;
    }
  }

  bool creatable = false, exists = false, busy = false;

  for (part = ped_disk_next_partition (disk, NULL); part;
       part = ped_disk_next_partition (disk, part)) {
    if (part->type & PED_PARTITION_FREESPACE) {
      if (part->geom.start < user_partition_start
        && part->geom.end > user_partition_end) {
        if (user_partition_end == 0) {
          user_partition_end = part->geom.end;
        }
        creatable = true;
      }
    } else if (ped_partition_is_active(part)) {
      if (strcmp(ped_partition_get_name(part), config.part_label) == 0) {
        exists = true;
        busy = ped_partition_is_busy(part);
      }
    }
  }

  if (!exists) {
    if (creatable) {
      fprintf(stderr, "Adding user data partition to GPT table\n");
      part = ped_partition_new (disk, PED_PARTITION_NORMAL, NULL, user_partition_start, user_partition_end);
      ped_partition_set_name(part, config.part_label);
      bool ok = ped_disk_add_partition(disk, part, ped_device_get_optimal_aligned_constraint(emmc));
      if (!ok) {
        fprintf(stderr, "Failed to add new partition\n");
        ret = 2;
        goto done;
      }
      ok = ped_disk_commit(disk);

      if (!ok) {
        fprintf(stderr, "Failed to commit new partition table\n");
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
      fprintf(stderr, "Found existing user data partition\n");
    else
      fprintf(stderr, "Found mounted user data partition\n");
  }

  char *part_path;
  int err = asprintf(&part_path, "/dev/disk/by-partlabel/%s", config.part_label);
  char *udevadm_cmd, *fsck_cmd, *mkfs_cmd, *skeleton_cmd;
  asprintf(&udevadm_cmd, "udevadm settle --exit-if-exists=%s", part_path);
  asprintf(&fsck_cmd, "e2fsck -p %s", part_path);
  asprintf(&mkfs_cmd, "mke2fs -t ext4 -F %s", part_path);
  asprintf(&skeleton_cmd, "tar x -z -f %s -C %s", config.skeleton, config.temp_mount_path);

  if (!busy) {
    while (access(part_path, F_OK) != 0) {
      // file does not exist
      system(udevadm_cmd);
      usleep(5000); // wait for 5ms
    }

    fsck_ret = system(fsck_cmd);
    if (fsck_ret != 0 && fsck_ret != 1) {
      fprintf(stderr, "User data partition is not initialized or corrupt. Formatting now...\n");
      mkfs_ret = system(mkfs_cmd);
      if (mkfs_ret != 0) {
        fprintf(stderr, "Failed to format partition\n");
        ret = 2;
        goto done;
      }
      mount(part_path, config.temp_mount_path, "ext4", MS_NOATIME, NULL);
      system(skeleton_cmd);
      umount(part_path);
    }
  }

  ret = 0;
done:
  ped_disk_destroy(disk);
  ped_device_destroy(emmc);

  return ret;
}
