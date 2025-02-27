// SPDX-License-Identifier: GPL-3.0
// Copyright (C) 2024 Bardia Moshiri <fakeshell@bardia.tech>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <grp.h>
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
  "    <property name='CurrentState' type='s' access='read'/>"
  "    <property name='MountedFile' type='s' access='read'/>"
  "  </interface>"
  "</node>";

static void
setup_configfs ()
{
  // Mount configfs if not already mounted
  if (access (CONFIGFS, F_OK) == -1) {
    if (mount ("none", CONFIGFS, "configfs", 0, NULL) == -1) {
      perror ("mount");
      return;
    }
  }

  mkdir (USBGADGET, 0755);
  mkdir (GADGETDIR, 0755);

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
configure_mtp ()
{
  g_debug ("Configuring for mode MTP");

  setup_configfs ();

  write_to_file (GADGETDIR "/os_desc/use", "1");
  write_to_file (GADGETDIR "/os_desc/b_vendor_code", "0x1");
  write_to_file (GADGETDIR "/os_desc/qw_sign", "MSFT100");

  char serialnumber[PROP_VALUE_MAX];
  char manufacturer[PROP_VALUE_MAX];
  char product[PROP_VALUE_MAX];
  char controller[PROP_VALUE_MAX];

  property_get ("ro.serialno", serialnumber, "");
  property_get ("ro.product.vendor.manufacturer", manufacturer, "");
  property_get ("ro.product.vendor.model", product, "");
  property_get ("sys.usb.controller", controller, "");

  write_to_file (GADGETDIR "/strings/0x409/serialnumber", serialnumber);
  write_to_file (GADGETDIR "/strings/0x409/manufacturer", manufacturer);
  write_to_file (GADGETDIR "/strings/0x409/product", product);

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
  write_to_file (GADGETDIR "/UDC", controller);
}

static void
configure_rndis ()
{
  g_debug ("Configuring for mode RNDIS");

  setup_configfs ();

  cleanup_configfs ();

  write_to_file (GADGETDIR "/configs/" CONFIGNAME "/strings/0x409/configuration", "rndis");

  symlink (GADGETDIR "/functions/" RNDISCONFIG, GADGETDIR "/configs/" CONFIGNAME "/" RNDISCONFIG);
  symlink (GADGETDIR "/functions/" RNDISBAMCONFIG, GADGETDIR "/configs/" CONFIGNAME "/" RNDISBAMCONFIG);

  char serialnumber[PROP_VALUE_MAX];
  char manufacturer[PROP_VALUE_MAX];
  char product[PROP_VALUE_MAX];
  char controller[PROP_VALUE_MAX];

  property_get ("ro.serialno", serialnumber, "");
  property_get ("ro.product.vendor.manufacturer", manufacturer, "");
  property_get ("ro.product.vendor.model", product, "");
  property_get ("sys.usb.controller", controller, "");

  write_to_file (GADGETDIR "/strings/0x409/serialnumber", serialnumber);
  write_to_file (GADGETDIR "/strings/0x409/manufacturer", manufacturer);
  write_to_file (GADGETDIR "/strings/0x409/product", product);
  write_to_file (GADGETDIR "/UDC", controller);
}

static void
configure_accessory ()
{
  g_debug ("Configuring for mode Accessory");

  setup_configfs ();

  cleanup_configfs ();

  write_to_file (GADGETDIR "/configs/" CONFIGNAME "/strings/0x409/configuration", "accessory");

  mkdir (GADGETDIR "/functions/" ACCESSORYCONFIG, 0755);
  symlink (GADGETDIR "/functions/" ACCESSORYCONFIG, GADGETDIR "/configs/" CONFIGNAME "/" ACCESSORYCONFIG);

  char serialnumber[PROP_VALUE_MAX];
  char manufacturer[PROP_VALUE_MAX];
  char product[PROP_VALUE_MAX];
  char controller[PROP_VALUE_MAX];

  property_get ("ro.serialno", serialnumber, "");
  property_get ("ro.product.vendor.manufacturer", manufacturer, "");
  property_get ("ro.product.vendor.model", product, "");
  property_get ("sys.usb.controller", controller, "");

  write_to_file (GADGETDIR "/strings/0x409/serialnumber", serialnumber);
  write_to_file (GADGETDIR "/strings/0x409/manufacturer", manufacturer);
  write_to_file (GADGETDIR "/strings/0x409/product", product);
  write_to_file (GADGETDIR "/UDC", controller);
}

static void
configure_acm ()
{
  g_debug ("Configuring for mode ACM");

  setup_configfs ();

  cleanup_configfs ();

  mkdir (GADGETDIR "/functions/" ACMCONFIG, 0755);
  write_to_file (GADGETDIR "/configs/" CONFIGNAME "/strings/0x409/configuration", "acm");

  symlink (GADGETDIR "/functions/" ACMCONFIG, GADGETDIR "/configs/" CONFIGNAME "/" ACMCONFIG);

  char serialnumber[PROP_VALUE_MAX];
  char manufacturer[PROP_VALUE_MAX];
  char product[PROP_VALUE_MAX];
  char controller[PROP_VALUE_MAX];

  property_get ("ro.serialno", serialnumber, "");
  property_get ("ro.product.vendor.manufacturer", manufacturer, "");
  property_get ("ro.product.vendor.model", product, "");
  property_get ("sys.usb.controller", controller, "");

  write_to_file (GADGETDIR "/strings/0x409/serialnumber", serialnumber);
  write_to_file (GADGETDIR "/strings/0x409/manufacturer", manufacturer);
  write_to_file (GADGETDIR "/strings/0x409/product", product);
  write_to_file (GADGETDIR "/UDC", controller);
}

static void
configure_none ()
{
  g_debug ("Configuring for mode NONE");

  setup_configfs ();

  cleanup_configfs ();

  write_to_file (GADGETDIR "/configs/" CONFIGNAME "/strings/0x409/configuration", "none");
  write_to_file (GADGETDIR "/UDC", "");
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
    gchar *mode;
    g_variant_get (parameters, "(&s)", &mode);

    if (g_strcmp0 (mode, "mtp") == 0)
      configure_mtp ();
    else if (g_strcmp0 (mode, "rndis") == 0)
      configure_rndis ();
    else if (g_strcmp0 (mode, "accessory") == 0)
      configure_accessory ();
    else if (g_strcmp0 (mode, "acm") == 0)
      configure_acm ();
    else if (g_strcmp0 (mode, "none") == 0)
      configure_none ();
  } else if (g_strcmp0 (method_name, "MountFile") == 0) {
    gchar *path;
    gboolean cdrom, readonly, force_configfs, force_usbgadget;
    g_variant_get (parameters, "(&sbbbb)",
                   &path,
                   &cdrom,
                   &readonly,
                   &force_configfs,
                   &force_usbgadget);

    mount_iso_file (path, cdrom, readonly, force_configfs, force_usbgadget);
  } else if (g_strcmp0 (method_name, "UnmountFile") == 0) {
    unmount_iso_file ();
  }

  g_dbus_method_invocation_return_value (invocation, NULL);
}

static gchar *
read_current_state ()
{
  char path[256];
  snprintf (path, sizeof(path), "%s/configs/%s/strings/0x409/configuration", GADGETDIR, CONFIGNAME);

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
  return g_strdup (buffer);
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
  } else if (g_strcmp0 (property_name, "MountedFile") == 0) {
    gchar *file = read_mounted_file ();
    GVariant *result = g_variant_new_string (file);
    g_free (file);
    return result;
  }

  g_set_error (error,
               G_IO_ERROR,
               G_IO_ERROR_INVALID_ARGUMENT,
               "Property %s is not supported",
               property_name);
  return NULL;
}

static const
GDBusInterfaceVTable interface_vtable = {
  .method_call = handle_method_call,
  .get_property = handle_get_property,
  .set_property = NULL
};

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar *name, gpointer user_data)
{
  GDBusNodeInfo *introspection_data = (GDBusNodeInfo *) user_data;

  GError *error = NULL;

  g_dbus_connection_register_object(
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
                  const gchar *name, gpointer user_data)
{
  g_debug ("Name acquired: %s", name);
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar *name, gpointer user_data)
{
  g_debug ("Name lost: %s", name);
}

int
main (int argc, char *argv[])
{
  GMainLoop *loop;
  guint owner_id;
  GError *error = NULL;

  GDBusNodeInfo *introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, &error);
  if (error) {
    g_printerr ("Error parsing introspection XML: %s\n", error->message);
    g_error_free (error);
    return 1;
  }

  owner_id = g_bus_own_name(
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
