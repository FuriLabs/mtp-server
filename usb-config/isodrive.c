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

  char lun_file[256];
  snprintf (lun_file, sizeof (lun_file), "%s/functions/%s/lun.0/file",
            GADGETDIR, MASS_STORAGE);
  write_to_file (lun_file, "\n");

  char lun_cdrom[256], lun_ro[256];
  snprintf (lun_cdrom, sizeof (lun_cdrom), "%s/functions/%s/lun.0/cdrom",
            GADGETDIR, MASS_STORAGE);
  snprintf (lun_ro, sizeof (lun_ro), "%s/functions/%s/lun.0/ro",
            GADGETDIR, MASS_STORAGE);

  write_to_file (lun_cdrom, "0");
  write_to_file (lun_ro, "0");

  // Now set the actual values
  if (strlen (iso_path) > 0) {
    write_to_file (lun_file, iso_path);
    write_to_file (lun_cdrom, cdrom ? "1" : "0");
    write_to_file (lun_ro, readonly ? "1" : "0");
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
    g_print ("Incompatible arguments: Cannot mount CDROM in read-write mode\n");
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
    g_print ("Using configfs to mount\n");
    configure_mass_storage_configfs (path, cdrom, readonly);
  } else if (is_android_usb_supported ()) {
    g_print ("Using android usb to mount\n");
    if (cdrom || !readonly)
      g_print ("Note: CDROM and read-write flags are ignored in Android USB mode\n");
    configure_mass_storage_android (path);
  } else {
    g_print ("No supported USB mass storage configuration method found\n");
  }
}

void
unmount_iso_file (void)
{
  if (is_configfs_supported ()) {
    g_print ("Using configfs to unmount\n");
    configure_mass_storage_configfs ("", false, true);
  } else if (is_android_usb_supported ()) {
    g_print ("Using android usb to unmount\n");
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
