/**
 * SPDX-License-Identifier: GPL-3.0
 * Copyright (C) 2026 Bardia Moshiri <bardia@furilabs.com>
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#include <libnm/NetworkManager.h>

#include "utils.h"

void
write_to_file (const char *path,
               const char *value)
{
  g_debug ("Attempting to write to %s: %s", path, value);

  int fd = open (path, O_WRONLY);
  if (fd == -1) {
    perror ("open");
    return;
  }

  if (write (fd, value, strlen (value)) == -1)
    perror ("write");

  close (fd);
}

char *
read_from_file (const char *path)
{
  FILE *file = fopen (path, "r");
  if (!file)
    return NULL;

  char buffer[256];
  if (!fgets (buffer, sizeof (buffer), file)) {
    fclose (file);
    return NULL;
  }

  fclose (file);
  buffer[strcspn (buffer, "\n")] = '\0';
  return strdup (buffer);
}

gboolean
is_usb_tethering_active (void)
{
  gboolean active = FALSE;
  const char *con_name = "USB Tethering";

  NMClient *client = nm_client_new (NULL, NULL);
  if (!client)
      return FALSE;

  const GPtrArray *active_connections = nm_client_get_active_connections (client);

  for (guint i = 0; i < active_connections->len; i++) {
    NMActiveConnection *ac = NM_ACTIVE_CONNECTION (g_ptr_array_index (active_connections, i));

    NMRemoteConnection *rc = nm_active_connection_get_connection (ac);
    if (!rc)
      continue;

    if (g_strcmp0 (nm_connection_get_id (NM_CONNECTION (rc)), con_name) == 0) {
      active = TRUE;
      break;
    }
  }

  g_object_unref (client);
  return active;
}
