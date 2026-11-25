/**
 * SPDX-License-Identifier: GPL-3.0
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include <glib.h>
#include <gio/gio.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <mntent.h>
#include <grp.h>
#include <errno.h>

#include "isodrive.h"
#include "utils.h"

static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='io.FuriOS.USBConfig'>"
  "    <method name='SetUSBMode'>"
  "      <arg type='s' name='mode' direction='in'/>"
  "    </method>"
  "    <method name='MountFile'>"
  "      <arg type='s' name='path' direction='in'/>"
  "      <arg type='b' name='cdrom' direction='in'/>"
  "      <arg type='b' name='readonly' direction='in'/>"
  "      <arg type='b' name='force_configfs' direction='in'/>"
  "      <arg type='b' name='force_usbgadget' direction='in'/>"
  "    </method>"
  "    <method name='UnmountFile'>"
  "    </method>"
  "    <method name='SetPowerRole'>"
  "      <arg type='s' name='mode' direction='in'/>"
  "    </method>"
  "    <method name='SetDataRole'>"
  "      <arg type='s' name='mode' direction='in'/>"
  "    </method>"
  "    <method name='SetPreferredRole'>"
  "      <arg type='s' name='mode' direction='in'/>"
  "    </method>"
  "    <method name='SetVCONNSource'>"
  "      <arg type='s' name='mode' direction='in'/>"
  "    </method>"
  "    <property name='CurrentState' type='s' access='read'/>"
  "    <property name='MountedFile' type='s' access='read'/>"
  "    <property name='SupportedUSBModes' type='as' access='read'/>"
  "    <property name='PowerRole' type='s' access='read'/>"
  "    <property name='DataRole' type='s' access='read'/>"
  "    <property name='PreferredRole' type='s' access='read'/>"
  "    <property name='VCONNSource' type='s' access='read'/>"
  "  </interface>"
  "</node>";

static gboolean
is_valid_usb_mode (const gchar *mode)
{
  return g_strcmp0 (mode, "none") == 0 ||
         g_strcmp0 (mode, "mtp") == 0 ||
         g_strcmp0 (mode, "rndis") == 0 ||
         g_strcmp0 (mode, "accessory") == 0 ||
         g_strcmp0 (mode, "acm") == 0 ||
         g_strcmp0 (mode, "acm,rndis") == 0 ||
         g_strcmp0 (mode, "meta") == 0;
}

static gchar *
read_file (const gchar *filename,
           GError     **error)
{
  gchar *content = NULL;
  gsize length = 0;

  if (g_file_get_contents (filename, &content, &length, error)) {
    if (length > 0 && content[length - 1] == '\n')
      content[length - 1] = '\0';
    return content;
  }

  return NULL;
}

static void
save_usb_mode (const gchar *mode)
{
  GError *error = NULL;
  g_autofree gchar *content = NULL;

  if (g_mkdir_with_parents (USB_CONFIG_CACHE_DIR, 0755) == -1) {
    g_warning ("Failed to create cache directory %s: %s",
               USB_CONFIG_CACHE_DIR,
               g_strerror (errno));
    return;
  }

  content = g_strdup_printf ("usb_mode=%s\n", mode);

  if (!g_file_set_contents (USB_CONFIG_CACHE_FILE, content, -1, &error)) {
    g_warning ("Failed to save USB mode to %s: %s",
               USB_CONFIG_CACHE_FILE,
               error->message);
    g_error_free (error);
  }
}

static gchar *
load_usb_mode (void)
{
  GError *error = NULL;
  g_autofree gchar *content = NULL;
  gchar **lines = NULL;
  gchar *mode = NULL;

  content = read_file (USB_CONFIG_CACHE_FILE, &error);

  if (content == NULL) {
    if (error != NULL)
      g_error_free (error);

    return g_strdup ("none");
  }

  lines = g_strsplit (content, "\n", -1);

  for (gint i = 0; lines[i] != NULL; i++) {
    if (g_str_has_prefix (lines[i], "usb_mode=")) {
      mode = g_strdup (lines[i] + strlen ("usb_mode="));
      break;
    }
  }

  g_strfreev (lines);

  if (mode == NULL)
    return g_strdup ("none");

  g_strstrip (mode);

  if (!is_valid_usb_mode (mode)) {
    g_warning ("Ignoring invalid saved USB mode '%s'", mode);
    g_free (mode);
    return g_strdup ("none");
  }

  return mode;
}

static gchar *
find_text_between_brackets (const gchar *text)
{
  const gchar *start, *end;

  start = g_strstr_len (text, -1, "[");
  if (start != NULL) {
    start++;
    end = g_strstr_len (start, -1, "]");
    if (end != NULL)
      return g_strndup (start, end - start);
  }

  return g_strdup (text);
}

static GVariant *
get_supported_usb_modes (void)
{
  const gchar *modes[] = {
    "none",
    "mtp",
    "rndis",
    "accessory",
    "acm",
    "acm,rndis",
    "meta",
    NULL
  };

  return g_variant_new_strv (modes, -1);
}

static gboolean
is_configfs_mounted (void)
{
  FILE *mounts;
  struct mntent *entry;
  gboolean mounted = FALSE;

  mounts = setmntent ("/proc/mounts", "r");
  if (mounts == NULL)
    return FALSE;

  while ((entry = getmntent (mounts)) != NULL) {
    if (g_strcmp0 (entry->mnt_dir, CONFIGFS) == 0 &&
        g_strcmp0 (entry->mnt_type, "configfs") == 0) {
      mounted = TRUE;
      break;
    }
  }

  endmntent (mounts);

  return mounted;
}

static gboolean
setup_configfs (GError **error)
{
  if (!is_configfs_mounted ()) {
    if (g_mkdir_with_parents (CONFIGFS, 0755) == -1 ||
        (mount ("none", CONFIGFS, "configfs", 0, NULL) == -1 &&
         !is_configfs_mounted ())) {
      gint saved_errno = errno;

      g_set_error (error,
                   G_IO_ERROR,
                   g_io_error_from_errno (saved_errno),
                   "Failed to mount configfs: %s",
                   g_strerror (saved_errno));
      return FALSE;
    }
  }

  mkdir (USBGADGET, 0755);
  mkdir (GADGETDIR, 0755);

  g_autofree gchar *bound_controller = read_file (GADGETDIR "/UDC", NULL);

  if (bound_controller != NULL && *g_strstrip (bound_controller) != '\0')
    write_to_file (GADGETDIR "/UDC", "\n");

  mkdir (GADGETDIR "/strings/0x409", 0755);

  mkdir (GADGETDIR "/functions", 0755);
  mkdir (GADGETDIR "/functions/" RNDISCONFIG, 0755);
  mkdir (GADGETDIR "/functions/" RNDISBAMCONFIG, 0755);

  mkdir (GADGETDIR "/configs", 0755);
  mkdir (GADGETDIR "/configs/" CONFIGNAME, 0755);
  mkdir (GADGETDIR "/configs/" CONFIGNAME "/strings", 0755);
  mkdir (GADGETDIR "/configs/" CONFIGNAME "/strings/0x409", 0755);

  write_to_file (GADGETDIR "/idVendor", IDVENDOR);
  write_to_file (GADGETDIR "/idProduct", IDPRODUCT);
  write_to_file (GADGETDIR "/bcdDevice", BCDDEVICE);
  write_to_file (GADGETDIR "/bcdUSB", BCDUSB);
  write_to_file (GADGETDIR "/bDeviceClass", "0x00");
  write_to_file (GADGETDIR "/bDeviceSubClass", "0x00");
  write_to_file (GADGETDIR "/bDeviceProtocol", "0x00");
  write_to_file (GADGETDIR "/configs/" CONFIGNAME "/bmAttributes", "0x80");
  write_to_file (GADGETDIR "/os_desc/use", "0");

  return TRUE;
}

static void
cleanup_configfs ()
{
  unlink (GADGETDIR "/configs/" CONFIGNAME "/" MTPCONFIG);
  unlink (GADGETDIR "/configs/" CONFIGNAME "/" RNDISCONFIG);
  unlink (GADGETDIR "/configs/" CONFIGNAME "/" RNDISBAMCONFIG);
  unlink (GADGETDIR "/configs/" CONFIGNAME "/" ACCESSORYCONFIG);
  unlink (GADGETDIR "/configs/" CONFIGNAME "/" ACMCONFIG);
}

static void
get_device_fallback (gchar *manufacturer,
                     gchar *product)
{
  g_autofree gchar *device = NULL;

  device = read_file ("/usr/lib/furios/device/preferred-hostname", NULL);

  if (device == NULL || *g_strstrip (device) == '\0') {
    g_clear_pointer (&device, g_free);
    device = read_file ("/usr/lib/furios/device/codename", NULL);
  }

  if (device == NULL || *g_strstrip (device) == '\0')
    return;

  if (manufacturer[0] == '\0')
    g_strlcpy (manufacturer, device, PROP_VALUE_MAX);

  if (product[0] == '\0')
    g_strlcpy (product, device, PROP_VALUE_MAX);
}

static gboolean
android_properties_ready (void)
{
  struct stat st;

  if (stat ("/dev/socket/property_service", &st) != 0 || !S_ISSOCK (st.st_mode))
    return FALSE;

  if (stat ("/dev/__properties__/property_info", &st) != 0 || st.st_size == 0)
    return FALSE;

  if (stat ("/dev/__properties__/properties_serial", &st) != 0 || st.st_size == 0)
    return FALSE;

  if (access (sizeof (void *) == 8 ?
              "/apex/com.android.runtime/lib64/bionic/libc.so" :
              "/apex/com.android.runtime/lib/bionic/libc.so", R_OK) != 0)
    return FALSE;

  return TRUE;
}

static gchar *
get_usb_controller (void)
{
  char controller[PROP_VALUE_MAX] = { 0 };
  g_autoptr (GDir) dir = NULL;
  const gchar *name;

  if (android_properties_ready ())
    property_get ("sys.usb.controller", controller, "");

  if (controller[0] != '\0')
    return g_strdup (controller);

  dir = g_dir_open ("/sys/class/udc", 0, NULL);
  if (dir == NULL)
    return NULL;

  while ((name = g_dir_read_name (dir)) != NULL) {
    if (g_strstr_len (name, -1, "dummy") != NULL)
      continue;

    return g_strdup (name);
  }

  return NULL;
}

static void
configure_gadget_strings (void)
{
  char serialnumber[PROP_VALUE_MAX] = { 0 };
  char manufacturer[PROP_VALUE_MAX] = { 0 };
  char product[PROP_VALUE_MAX] = { 0 };

  if (android_properties_ready ()) {
    property_get ("ro.serialno", serialnumber, "");
    property_get ("ro.product.vendor.manufacturer", manufacturer, "");
    property_get ("ro.product.vendor.model", product, "");
  }

  if (manufacturer[0] == '\0' || product[0] == '\0')
    get_device_fallback (manufacturer, product);

  write_to_file (GADGETDIR "/strings/0x409/serialnumber", serialnumber);
  write_to_file (GADGETDIR "/strings/0x409/manufacturer", manufacturer);
  write_to_file (GADGETDIR "/strings/0x409/product", product);
}

static gboolean
bind_gadget (GError **error)
{
  g_autofree gchar *controller = get_usb_controller ();
  g_autofree gchar *bound_controller = NULL;
  g_autofree gchar *mode_path = NULL;

  if (controller == NULL) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                         "No USB device controller found");
    return FALSE;
  }

  mode_path = g_build_filename ("/sys/class/udc", controller,
                                "device", "..", "mode", NULL);

  if (g_file_test (mode_path, G_FILE_TEST_EXISTS))
    write_to_file (mode_path, "peripheral");

  write_to_file (GADGETDIR "/UDC", controller);

  bound_controller = read_file (GADGETDIR "/UDC", NULL);
  if (bound_controller == NULL ||
      g_strcmp0 (g_strstrip (bound_controller), controller) != 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Failed to bind USB gadget to %s", controller);
    return FALSE;
  }

  return TRUE;
}

static void
configure_mtp ()
{
  g_debug ("Configuring for mode MTP");

  write_to_file (GADGETDIR "/os_desc/use", "1");
  write_to_file (GADGETDIR "/os_desc/b_vendor_code", "0x1");
  write_to_file (GADGETDIR "/os_desc/qw_sign", "MSFT100");

  configure_gadget_strings ();

  mkdir (GADGETDIR "/functions/" MTPCONFIG, 0755);
  symlink (GADGETDIR "/configs/" CONFIGNAME, GADGETDIR "/os_desc/" CONFIGNAME);

  chown (GADGETDIR, 0, getgrnam ("plugdev")->gr_gid);
  chown (GADGETDIR "/configs", 0, getgrnam ("plugdev")->gr_gid);
  chown (GADGETDIR "/configs/" CONFIGNAME, 0, getgrnam ("plugdev")->gr_gid);
  chown (MTP_USB, 0, getgrnam ("plugdev")->gr_gid);
  chmod (MTP_USB, 0660);

  cleanup_configfs ();

  write_to_file (GADGETDIR "/functions/" MTPCONFIG "/os_desc/interface.MTP/compatible_id", "mtp");
  write_to_file (GADGETDIR "/configs/" CONFIGNAME "/strings/0x409/configuration", "mtp");

  symlink (GADGETDIR "/functions/" MTPCONFIG, GADGETDIR "/configs/" CONFIGNAME "/" MTPCONFIG);

  write_to_file (GADGETDIR "/os_desc/use", "1");
}

static void
configure_rndis ()
{
  g_debug ("Configuring for mode RNDIS");

  cleanup_configfs ();

  write_to_file (GADGETDIR "/configs/" CONFIGNAME "/strings/0x409/configuration", "rndis");

  symlink (GADGETDIR "/functions/" RNDISCONFIG, GADGETDIR "/configs/" CONFIGNAME "/" RNDISCONFIG);

  symlink (GADGETDIR "/functions/" RNDISBAMCONFIG, GADGETDIR "/configs/" CONFIGNAME "/" RNDISBAMCONFIG);

  configure_gadget_strings ();
}

static void
configure_accessory ()
{
  g_debug ("Configuring for mode Accessory");

  cleanup_configfs ();

  write_to_file (GADGETDIR "/configs/" CONFIGNAME "/strings/0x409/configuration", "accessory");

  mkdir (GADGETDIR "/functions/" ACCESSORYCONFIG, 0755);

  symlink (GADGETDIR "/functions/" ACCESSORYCONFIG, GADGETDIR "/configs/" CONFIGNAME "/" ACCESSORYCONFIG);

  configure_gadget_strings ();
}

static void
configure_acm ()
{
  g_debug ("Configuring for mode ACM");

  cleanup_configfs ();

  mkdir (GADGETDIR "/functions/" ACMCONFIG, 0755);

  write_to_file (GADGETDIR "/configs/" CONFIGNAME "/strings/0x409/configuration", "acm");

  symlink (GADGETDIR "/functions/" ACMCONFIG, GADGETDIR "/configs/" CONFIGNAME "/" ACMCONFIG);

  configure_gadget_strings ();
}

static void
configure_acm_rndis ()
{
  g_debug ("Configuring for mode ACM + RNDIS");

  cleanup_configfs ();

  mkdir (GADGETDIR "/functions/" ACMCONFIG, 0755);

  write_to_file (GADGETDIR "/configs/" CONFIGNAME "/strings/0x409/configuration", "acm,rndis");

  symlink (GADGETDIR "/functions/" ACMCONFIG, GADGETDIR "/configs/" CONFIGNAME "/" ACMCONFIG);

  symlink (GADGETDIR "/functions/" RNDISCONFIG, GADGETDIR "/configs/" CONFIGNAME "/" RNDISCONFIG);

  symlink (GADGETDIR "/functions/" RNDISBAMCONFIG, GADGETDIR "/configs/" CONFIGNAME "/" RNDISBAMCONFIG);

  configure_gadget_strings ();
}

static void
configure_meta (void)
{
  g_debug ("Configuring for mode META");

  configure_acm ();

  write_to_file (GADGETDIR "/idVendor", "0x0e8d");
  write_to_file (GADGETDIR "/idProduct", "0x2007");

  write_to_file (GADGETDIR "/bDeviceClass", "0x02");
  write_to_file (GADGETDIR "/bDeviceSubClass", "0x00");
  write_to_file (GADGETDIR "/bDeviceProtocol", "0x00");

  write_to_file (GADGETDIR "/strings/0x409/manufacturer", "Android");
  write_to_file (GADGETDIR "/strings/0x409/product", "Android");
  write_to_file (GADGETDIR "/strings/0x409/serialnumber", "\n");
  write_to_file (GADGETDIR "/configs/" CONFIGNAME "/bmAttributes", "0xc0");
  write_to_file (GADGETDIR "/configs/" CONFIGNAME "/strings/0x409/configuration", "android");
}

static gboolean
apply_usb_mode (const gchar  *mode,
                gboolean      persist,
                GError      **error)
{
  if (!is_valid_usb_mode (mode)) {
    g_set_error (error,
                 G_IO_ERROR,
                 G_IO_ERROR_INVALID_ARGUMENT,
                 "Invalid USB mode '%s'",
                 mode);
    return FALSE;
  }

  if (!setup_configfs (error))
    return FALSE;

  if (g_strcmp0 (mode, "mtp") == 0)
    configure_mtp ();
  else if (g_strcmp0 (mode, "rndis") == 0)
    configure_rndis ();
  else if (g_strcmp0 (mode, "accessory") == 0)
    configure_accessory ();
  else if (g_strcmp0 (mode, "acm") == 0)
    configure_acm ();
  else if (g_strcmp0 (mode, "acm,rndis") == 0)
    configure_acm_rndis ();
  else if (g_strcmp0 (mode, "meta") == 0)
    configure_meta ();
  else if (g_strcmp0 (mode, "none") == 0)
    configure_acm ();

  if (!bind_gadget (error))
    return FALSE;

  if (persist)
    save_usb_mode (mode);

  return TRUE;
}

static gchar *
read_current_state ()
{
  char path[256];

  snprintf (path,
            sizeof (path),
            "%s/configs/%s/strings/0x409/configuration",
            GADGETDIR,
            CONFIGNAME);

  FILE *file = fopen (path, "r");

  if (!file) {
    perror ("fopen");
    return g_strdup ("none");
  }

  char buffer[256];

  if (!fgets (buffer, sizeof (buffer), file)) {
    perror ("fgets");
    fclose (file);
    return g_strdup ("none");
  }

  fclose (file);

  buffer[strcspn (buffer, "\n")] = '\0';

  if (g_strcmp0 (buffer, "android") == 0) {
    g_autofree gchar *vendor = read_file (GADGETDIR "/idVendor", NULL);
    g_autofree gchar *product = read_file (GADGETDIR "/idProduct", NULL);

    if (vendor != NULL && product != NULL &&
        g_ascii_strtoull (vendor, NULL, 16) == 0x0e8d &&
        g_ascii_strtoull (product, NULL, 16) == 0x2007)
      return g_strdup ("meta");
  }

  return g_strdup (buffer);
}

static void
handle_method_call (GDBusConnection *connection,
                    const gchar *sender,
                    const gchar *object_path,
                    const gchar *interface_name,
                    const gchar *method_name,
                    GVariant *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer user_data)
{
  if (g_strcmp0 (method_name, "SetUSBMode") == 0) {
    const gchar *mode = NULL;
    GError *error = NULL;

    g_variant_get (parameters, "(&s)", &mode);

    if (!apply_usb_mode (mode, TRUE, &error)) {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "%s",
                                             error->message);
      g_error_free (error);
      return;
    }

    g_dbus_method_invocation_return_value (invocation, NULL);
    return;
  }

  if (g_strcmp0 (method_name, "MountFile") == 0) {
    const gchar *path = NULL;
    gboolean cdrom = FALSE, readonly = FALSE, force_configfs = FALSE, force_usbgadget = FALSE;

    g_variant_get (parameters, "(&sbbbb)",
                   &path,
                   &cdrom,
                   &readonly,
                   &force_configfs,
                   &force_usbgadget);

    mount_iso_file (path, cdrom, readonly, force_configfs, force_usbgadget);

    g_dbus_method_invocation_return_value (invocation, NULL);
    return;
  }

  if (g_strcmp0 (method_name, "UnmountFile") == 0) {
    unmount_iso_file ();
    g_dbus_method_invocation_return_value (invocation, NULL);
    return;
  }

  if (g_strcmp0 (method_name, "SetPowerRole") == 0 ||
      g_strcmp0 (method_name, "SetDataRole") == 0 ||
      g_strcmp0 (method_name, "SetPreferredRole") == 0 ||
      g_strcmp0 (method_name, "SetVCONNSource") == 0) {
    const gchar *mode = NULL;
    g_autofree gchar *file_path = NULL;
    gboolean valid_input = FALSE;

    g_variant_get (parameters, "(&s)", &mode);

    if (g_strcmp0 (method_name, "SetPowerRole") == 0) {
      file_path = g_build_filename (TYPEC_PORT_PATH, "power_role", NULL);
      valid_input = (g_strcmp0 (mode, "source") == 0 || g_strcmp0 (mode, "sink") == 0);
    } else if (g_strcmp0 (method_name, "SetDataRole") == 0) {
      file_path = g_build_filename (TYPEC_PORT_PATH, "data_role", NULL);
      valid_input = (g_strcmp0 (mode, "host") == 0 || g_strcmp0 (mode, "device") == 0);
    } else if (g_strcmp0 (method_name, "SetPreferredRole") == 0) {
      file_path = g_build_filename (TYPEC_PORT_PATH, "preferred_role", NULL);
      valid_input = (g_strcmp0 (mode, "source") == 0 ||
                     g_strcmp0 (mode, "sink") == 0 ||
                     g_strcmp0 (mode, "none") == 0);
    } else if (g_strcmp0 (method_name, "SetVCONNSource") == 0) {
      file_path = g_build_filename (TYPEC_PORT_PATH, "vconn_source", NULL);
      valid_input = (g_strcmp0 (mode, "yes") == 0 || g_strcmp0 (mode, "no") == 0);
    }

    if (!valid_input) {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_INVALID_ARGS,
                                             "Invalid mode '%s' for method %s",
                                             mode,
                                             method_name);
      return;
    }

    write_to_file (file_path, mode);

    g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));
    return;
  }

  g_dbus_method_invocation_return_error (invocation,
                                        G_DBUS_ERROR,
                                        G_DBUS_ERROR_UNKNOWN_METHOD,
                                        "Unknown method %s",
                                        method_name);
}

static GVariant *
handle_get_property (GDBusConnection *connection,
                     const gchar *sender,
                     const gchar *object_path,
                     const gchar *interface_name,
                     const gchar *property_name,
                     GError **error,
                     gpointer user_data)
{
  if (g_strcmp0 (property_name, "CurrentState") == 0) {
    gchar *state = read_current_state ();
    GVariant *result = g_variant_new_string (state);
    g_free (state);
    return result;
  }

  if (g_strcmp0 (property_name, "MountedFile") == 0) {
    gchar *file = read_mounted_file ();
    GVariant *result = g_variant_new_string (file);
    g_free (file);
    return result;
  }

  if (g_strcmp0 (property_name, "SupportedUSBModes") == 0)
    return get_supported_usb_modes ();

  if (g_strcmp0 (property_name, "PowerRole") == 0 ||
      g_strcmp0 (property_name, "DataRole") == 0 ||
      g_strcmp0 (property_name, "PreferredRole") == 0 ||
      g_strcmp0 (property_name, "VCONNSource") == 0) {
    g_autofree gchar *filepath = NULL;
    g_autofree gchar *state = NULL;

    if (g_strcmp0 (property_name, "PowerRole") == 0)
      filepath = g_build_filename (TYPEC_PORT_PATH, "power_role", NULL);
    else if (g_strcmp0 (property_name, "DataRole") == 0)
      filepath = g_build_filename (TYPEC_PORT_PATH, "data_role", NULL);
    else if (g_strcmp0 (property_name, "PreferredRole") == 0)
      filepath = g_build_filename (TYPEC_PORT_PATH, "preferred_role", NULL);
    else if (g_strcmp0 (property_name, "VCONNSource") == 0)
      filepath = g_build_filename (TYPEC_PORT_PATH, "vconn_source", NULL);

    state = read_file (filepath, error);
    if (state == NULL)
      return NULL;

    if (g_strcmp0 (property_name, "PowerRole") == 0 ||
        g_strcmp0 (property_name, "DataRole") == 0) {
      g_autofree gchar *bracketed_text = find_text_between_brackets (state);
      return g_variant_new_string (bracketed_text);
    }

    return g_variant_new_string (state);
  }

  g_set_error (error,
               G_IO_ERROR,
               G_IO_ERROR_INVALID_ARGUMENT,
               "Property %s is not supported",
               property_name);
  return NULL;
}

static const GDBusInterfaceVTable interface_vtable = {
  .method_call = handle_method_call,
  .get_property = handle_get_property,
  .set_property = NULL
};

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar *name,
                 gpointer user_data)
{
  GDBusNodeInfo *introspection_data = (GDBusNodeInfo *) user_data;
  GError *error = NULL;

  g_dbus_connection_register_object (
    connection,
    "/io/FuriOS/USBConfig",
    introspection_data->interfaces[0],
    &interface_vtable,
    NULL,
    NULL,
    &error);

  if (error) {
    g_printerr ("Error registering object: %s\n", error->message);
    g_error_free (error);
  }
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar *name,
                  gpointer user_data)
{
  g_debug ("Name acquired: %s", name);
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar *name,
              gpointer user_data)
{
  g_debug ("Name lost: %s", name);
}

int
main (void)
{
  GMainLoop *loop;
  guint owner_id;
  GError *error = NULL;
  g_autofree gchar *saved_mode = NULL;

  GDBusNodeInfo *introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, &error);

  if (error) {
    g_printerr ("Error parsing introspection XML: %s\n", error->message);
    g_error_free (error);
    return 1;
  }

  if (!is_usb_tethering_active ()) {
    saved_mode = load_usb_mode ();
    g_debug ("Applying saved USB mode: %s", saved_mode);

    if (!apply_usb_mode (saved_mode, FALSE, &error)) {
      g_warning ("Failed to apply saved USB mode '%s': %s",
                 saved_mode,
                 error->message);
      g_clear_error (&error);
      apply_usb_mode ("none", FALSE, NULL);
    }
  } else {
    g_debug ("USB Tethering is active, not restoring USB mode");
  }

  owner_id = g_bus_own_name (
    G_BUS_TYPE_SYSTEM,
    "io.FuriOS.USBConfig",
    G_BUS_NAME_OWNER_FLAGS_NONE,
    on_bus_acquired,
    on_name_acquired,
    on_name_lost,
    introspection_data,
    NULL);

  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  g_bus_unown_name (owner_id);
  g_dbus_node_info_unref (introspection_data);
  g_main_loop_unref (loop);

  return 0;
}
