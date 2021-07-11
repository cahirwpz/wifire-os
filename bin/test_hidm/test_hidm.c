#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

struct report {
  uint8_t buttons;
  uint8_t x;
  uint8_t y;
} __packed;

typedef struct report report_t;

static void print_button(uint8_t buttons, int num) {
  int pressed = (buttons & (1 << num));
  printf("button %d: %spressed\n", num + 1, (pressed ? "" : "not "));
}

static void print_report(report_t *report) {
  print_button(report->buttons, 0);
  print_button(report->buttons, 1);
  print_button(report->buttons, 2);
  printf("x displacement: %hhx\n", report->x);
  printf("y displacement: %hhx\n\n", report->y);
  fflush(stdout);
}

#define NREPORTS 64

int main(int argc, char **argv) {
  printf("hid mouse test:\n");
  fflush(stdout);

  int fd = open("/dev/hidm", O_RDONLY, 0);

  while (1) {
    report_t r;
    if (read(fd, &r, sizeof(report_t)) != sizeof(report_t))
      return EXIT_FAILURE;
    print_report(&r);
  }

  return EXIT_SUCCESS;
}
