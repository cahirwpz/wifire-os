#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#define BLOCK_SIZE 512
#define WRITE_DATA "Do you like fishsticks?"

int main(int argc, char **argv) {
  printf("USB mass storage device test:\n");
  fflush(stdout);

  int fd = open("/dev/umass", O_RDWR, 0);
  uint8_t buf[BLOCK_SIZE];

  while (1) {
    if (write(fd, WRITE_DATA, sizeof(WRITE_DATA)) != sizeof(WRITE_DATA))
      return EXIT_FAILURE;

    if (read(fd, buf, BLOCK_SIZE) != BLOCK_SIZE)
      return EXIT_FAILURE;

    printf("data: \"%s\"\n", buf);
    fflush(stdout);

    memset(buf, 0, BLOCK_SIZE);
  }

  return EXIT_SUCCESS;
}
