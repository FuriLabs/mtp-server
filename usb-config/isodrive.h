// SPDX-License-Identifier: GPL-3.0
// Copyright (C) 2024 Bardia Moshiri <fakeshell@bardia.tech>

#ifndef ISODRIVE_H
#define ISODRIVE_H

#include <gio/gio.h>
#include <stdbool.h>
#include <hybris/properties/properties.h>

bool
is_configfs_supported (void);

bool
is_android_usb_supported (void);

void
configure_mass_storage_configfs (const char *iso_path,
                                 bool cdrom,
                                 bool readonly);

bool
is_android_usb_enabled (void);

void
configure_mass_storage_android (const char *iso_path);

void
mount_iso_file (const char *path,
                gboolean cdrom,
                gboolean readonly,
                gboolean force_configfs,
                gboolean force_usbgadget);

void
unmount_iso_file (void);

gchar *
read_mounted_file (void);

#endif // ISODRIVE_H
