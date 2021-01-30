#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* the packet's max size */
/* now we (and rtl8139) do not support large packets or jumbo frames */
#define PACKET_SIZE 1600
uint8_t packet[PACKET_SIZE];

int
main(int argc, char *argv[])
{
  FILE *fp = fopen("/dev/rtl8139", "r+b");

  setprogname(argv[0]);
  (void)setlocale(LC_ALL, "");

  int n = fread(packet, sizeof(uint8_t), PACKET_SIZE, fp);
  printf("Netdump: read %d bytes of packet:\n", n);
  for (int i = 0; i < n; ++i) {
    if (i && !(i % 10))
      printf("\n");
    printf("%02X ", packet[i]);
  }
  printf("\n");
  fclose(fp);

  exit(0);
}
