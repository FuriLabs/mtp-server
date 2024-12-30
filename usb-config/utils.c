// SPDX-License-Identifier: GPL-3.0
// Copyright (C) 2024 Bardia Moshiri <fakeshell@bardia.tech>

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include "utils.h"

void
write_to_file (const char *path,
               const char *value)
{
  g_print ("Attempting to write to %s: %s\n", path, value);

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
