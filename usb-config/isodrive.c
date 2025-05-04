// SPDX-License-Identifier: GPL-3.0
// Copyright (C) 2024 Bardia Moshiri <fakeshell@bardia.tech>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <mntent.h>
#include "isodrive.h"
#include "utils.h"

bool
is_configfs_supported (void)
{
  FILE *mounts = setmntent ("/proc/mounts", "r");
  if (!mounts)
    return false;

  struct mntent *ent;
  bool supported = false;

  while ((ent = getmntent (mounts))) {
    if (strcmp (ent->mnt_fsname, "configfs") == 0) {
      supported = true;
      break;
    }
  }

  endmntent (mounts);

  // Check alternate Android location
  if (!supported) {
    DIR *dir = opendir ("/config/usb_gadget");
    if (dir) {
      supported = true;
      closedir (dir);
    }
  }

  return supported;
}

bool
is_android_usb_supported (void)
{
  struct stat sb;
  return (stat (ANDROID0_SYSFS_ENABLE, &sb) == 0 && S_ISREG (sb.st_mode));
}

void
configure_mass_storage_configfs (const char *iso_path,
                                 bool cdrom,
                                 bool readonly)
{
  char controller[PROP_VALUE_MAX];
  property_get ("sys.usb.controller", controller, "usb0");

  // this is \n to flush LUN and UDC. writing an empty string is not enough
  write_to_file (GADGETDIR "/UDC", "\n");

  char functions_dir[256], mass_storage_dir[256], lun_dir[256];
  char configs_dir[256], config_dir[256], config_link[256];

  snprintf (functions_dir, sizeof (functions_dir),
            "%s/functions", GADGETDIR);
  snprintf (mass_storage_dir, sizeof (mass_storage_dir),
            "%s/functions/%s", GADGETDIR, MASS_STORAGE),
  snprintf (lun_dir, sizeof (lun_dir),
            "%s/functions/%s/lun.0", GADGETDIR, MASS_STORAGE);
  snprintf (configs_dir, sizeof (configs_dir),
            "%s/configs", GADGETDIR);
  snprintf (config_dir, sizeof (config_dir),
            "%s/configs/c.1", GADGETDIR);
  snprintf (config_link, sizeof (config_link),
            "%s/configs/c.1/%s", GADGETDIR, MASS_STORAGE);

  char lun_file[256], lun_cdrom[256], lun_ro[256];
  snprintf (lun_file, sizeof (lun_file),
            "%s/functions/%s/lun.0/file", GADGETDIR, MASS_STORAGE);
  snprintf (lun_cdrom, sizeof (lun_cdrom),
            "%s/functions/%s/lun.0/cdrom", GADGETDIR, MASS_STORAGE);
  snprintf (lun_ro, sizeof (lun_ro),
            "%s/functions/%s/lun.0/ro", GADGETDIR, MASS_STORAGE);

  // Empty the lun file if it exists
  struct stat st;
  if (stat (lun_file, &st) == 0)
    write_to_file (lun_file, "\n");

  if (strlen (iso_path) > 0) {
    mkdir (functions_dir, 0755);
    mkdir (mass_storage_dir, 0755);
    mkdir (lun_dir, 0755);
    mkdir (configs_dir, 0755);
    mkdir (config_dir, 0755);

    if (lstat (config_link, &st) != 0)
      symlink (mass_storage_dir, config_link);

    write_to_file (lun_cdrom, cdrom ? "1" : "0");
    write_to_file (lun_ro, readonly ? "1" : "0");
    write_to_file (lun_file, iso_path);
  } else {
    if (lstat (config_link, &st) == 0)
      unlink (config_link);
    if (stat (lun_dir, &st) == 0)
      rmdir (lun_dir);
    if (stat (mass_storage_dir, &st) == 0)
      rmdir (mass_storage_dir);
  }

  // Re-enable UDC
  write_to_file (GADGETDIR "/UDC", controller);
}

bool
is_android_usb_enabled (void)
{
  char *value = read_from_file (ANDROID0_SYSFS_ENABLE);
  if (!value)
    return false;

  bool enabled = (value[0] == '1');
  free (value);
  return enabled;
}

void
configure_mass_storage_android (const char *iso_path)
{
  if (is_android_usb_enabled ())
    write_to_file (ANDROID0_SYSFS_ENABLE, "0");

  write_to_file (ANDROID0_SYSFS_IMG_FILE, iso_path);

  if (iso_path[0] == '\0')
    write_to_file (ANDROID0_SYSFS_FEATURES, "mtp");
  else
    write_to_file (ANDROID0_SYSFS_FEATURES, "mass_storage");

  write_to_file (ANDROID0_SYSFS_ENABLE, "1");
}

void
mount_iso_file (const char *path,
                gboolean cdrom,
                gboolean readonly,
                gboolean force_configfs,
                gboolean force_usbgadget)
{
  if (cdrom && !readonly) {
    g_debug ("Incompatible arguments: Cannot mount CDROM in read-write mode");
    return;
  }

  if (access (path, F_OK) == -1) {
    g_print ("File does not exist: %s\n", path);
    return;
  }

  if (force_configfs) {
    if (!is_configfs_supported ()) {
      g_print ("ConfigFS is not supported on this device\n");
      return;
    }
    configure_mass_storage_configfs (path, cdrom, readonly);
    return;
  }

  if (force_usbgadget) {
    if (!is_android_usb_supported ()) {
      g_print ("Android USB Gadget is not supported on this device\n");
      return;
    }
    configure_mass_storage_android (path);
    return;
  }

  if (is_configfs_supported ()) {
    g_debug ("Using configfs to mount");
    configure_mass_storage_configfs (path, cdrom, readonly);
  } else if (is_android_usb_supported ()) {
    g_debug ("Using android usb to mount");
    if (cdrom || !readonly)
      g_debug ("Note: CDROM and read-write flags are ignored in Android USB mode");
    configure_mass_storage_android (path);
  } else {
    g_print ("No supported USB mass storage configuration method found\n");
  }
}

void
unmount_iso_file (void)
{
  if (is_configfs_supported ()) {
    g_debug ("Using configfs to unmount");
    configure_mass_storage_configfs ("", false, true);
  } else if (is_android_usb_supported ()) {
    g_debug ("Using android usb to unmount");
    configure_mass_storage_android ("");
  } else {
    g_print ("No supported USB mass storage configuration method found\n");
  }
}

gchar *
read_mounted_file (void)
{
  gchar *mounted_file = NULL;

  if (is_configfs_supported ()) {
    char path[256];
    snprintf (path, sizeof (path),
             "%s/functions/%s/lun.0/file",
             GADGETDIR, MASS_STORAGE);

    mounted_file = read_from_file (path);
  } else if (is_android_usb_supported ()) {
    mounted_file = read_from_file (ANDROID0_SYSFS_IMG_FILE);
  }

  if (!mounted_file || strlen (mounted_file) == 0) {
    g_free (mounted_file);
    return g_strdup ("");
  }

  return mounted_file;
}
