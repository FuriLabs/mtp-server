// SPDX-License-Identifier: GPL-3.0
// Copyright (C) 2024 Bardia Moshiri <fakeshell@bardia.tech>

#ifndef UTILS_H
#define UTILS_H

#include <gio/gio.h>
#include <stdbool.h>

#define CONFIGFS "/sys/kernel/config"
#define USBGADGET CONFIGFS "/usb_gadget"
#define GADGETDIR USBGADGET "/g1"
#define CONFIGNAME "c.1"
#define RNDISCONFIG "rndis.usb0"
#define RNDISBAMCONFIG "rndis_bam.rndis"
#define MTPCONFIG "mtp.gs0"
#define MASS_STORAGE "mass_storage.0"
#define ACCESSORYCONFIG "accessory.usb0"
#define ACMCONFIG "acm.0"

#define ANDROID0_SYSFS_ENABLE "/sys/devices/virtual/android_usb/android0/enable"
#define ANDROID0_SYSFS_IMG_FILE "/sys/devices/virtual/android_usb/android0/f_mass_storage/lun/file"
#define ANDROID0_SYSFS_FEATURES "/sys/devices/virtual/android_usb/android0/functions"

#define IDVENDOR "0x2717"
#define IDPRODUCT "0xFF20"
#define BCDDEVICE "0x0223"
#define BCDUSB "0x0200"

#define MTP_USB "/dev/mtp_usb"

void
write_to_file (const char *path,
               const char *value);

char *
read_from_file (const char *path);

#endif // UTILS_H
