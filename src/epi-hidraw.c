/*
 * epi-hidraw: minimal HID raw device helper used by epinephrine.
 *
 * Runs as root through the persistent admin session (epi-privilege.c)
 * so hidraw ioctls/writes work without user permission tricks.
 *
 * Usage:
 *   epi-hidraw DEVICE get-feature ID_HEX LEN   -> prints LEN bytes as hex
 *   epi-hidraw DEVICE set-feature HEX          -> ioctl HIDIOCSFEATURE(len)
 *   epi-hidraw DEVICE write HEX                -> write(2) the bytes
 *
 * Exit status 0 on success, 1 on I/O error, 2 on usage error.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>

#ifndef _IOC
#include <asm-generic/ioctl.h>
#endif

#define MAX_BUF 64

static int
hex_to_bytes (const char *hex, uint8_t *out, size_t max)
{
  size_t n = strlen (hex);
  if (n % 2 != 0 || n == 0 || n / 2 > max) return -1;

  for (size_t i = 0; i < n; i += 2)
    {
      char byte[3] = { hex[i], hex[i + 1], '\0' };
      char *end = NULL;
      long v = strtol (byte, &end, 16);
      if (*end != '\0' || v < 0 || v > 0xFF) return -1;
      out[i / 2] = (uint8_t) v;
    }
  return (int) (n / 2);
}

static void
print_hex (const uint8_t *data, size_t n)
{
  for (size_t i = 0; i < n; i++)
    printf ("%02x", data[i]);
  printf ("\n");
}

int
main (int argc, char **argv)
{
  if (argc < 4)
    {
      fprintf (stderr, "usage: epi-hidraw DEVICE get-feature ID_HEX LEN | set-feature HEX | write HEX\n");
      return 2;
    }

  const char *dev = argv[1];
  const char *op = argv[2];

  int fd = open (dev, O_RDWR);
  if (fd < 0)
    {
      fprintf (stderr, "open %s: %s\n", dev, strerror (errno));
      return 1;
    }

  int rc = 1;

  if (strcmp (op, "get-feature") == 0 && argc == 5)
    {
      int id = (int) strtol (argv[3], NULL, 16);
      int len = atoi (argv[4]);
      if (len < 1 || len > MAX_BUF)
        rc = 2;
      else
        {
          uint8_t buf[MAX_BUF];
          memset (buf, 0, sizeof (buf));
          buf[0] = (uint8_t) id;
          if (ioctl (fd, HIDIOCGFEATURE (len), buf) >= 0)
            {
              print_hex (buf, (size_t) len);
              rc = 0;
            }
        }
    }
  else if (strcmp (op, "set-feature") == 0 && argc == 4)
    {
      uint8_t buf[MAX_BUF];
      int len = hex_to_bytes (argv[3], buf, sizeof (buf));
      if (len > 0 && ioctl (fd, HIDIOCSFEATURE (len), buf) >= 0)
        rc = 0;
    }
  else if (strcmp (op, "write") == 0 && argc == 4)
    {
      uint8_t buf[MAX_BUF];
      int len = hex_to_bytes (argv[3], buf, sizeof (buf));
      if (len > 0 && write (fd, buf, (size_t) len) == (ssize_t) len)
        rc = 0;
    }
  else
    rc = 2;

  close (fd);
  return rc;
}
