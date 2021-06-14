#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/input.h>

#define BUFSIZE 64
#define X_ORIGIN 0
#define Y_ORIGIN 0

typedef struct input_event input_event_t;

/* Buttons state. */
static uint8_t left;
static uint8_t right;
static uint8_t middle;

/* Mouse pointer position. */
static int16_t x = X_ORIGIN;
static int16_t y = Y_ORIGIN;

/* Process mouse buttons. */
static void process_buttons(uint16_t keycode, uint32_t value) {
  if (keycode == BTN_LEFT) {
    left = value;
  } else if (keycode == BTN_RIGHT) {
    right = value;
  } else {
    assert(keycode == BTN_MIDDLE);
    middle = value;
  }
}

/* Process mouse's movement. */
static void process_movement(uint16_t keycode, uint32_t value) {
  if (keycode == REL_X) {
    x += (int8_t)value;
  } else {
    assert(keycode == REL_Y);
    y += (int8_t)value;
  }
}

int main(int argc, char **argv) {
  /* TODO: there should be a better way to select the required event file.
   * FTTB, let's just hardcode the path. */
  int evfd = open("/dev/input/event0", O_RDONLY, 0);
  input_event_t buf[BUFSIZE];
  ssize_t n;

  while ((n = read(evfd, buf, sizeof(buf))) >= 0) {
    /* Process received events. */
    for (size_t i = 0; i < n / sizeof(input_event_t); i++) {
      input_event_t *evp = &buf[i];

      if (evp->type == EV_KEY) {
        /* We shouldn't see any reserved keycodes. */
        assert(evp->code != KEY_RESERVED);
        process_buttons(evp->code, evp->value);
      } else if (evp->type == EV_REL) {
        process_movement(evp->code, evp->value);
      } else {
        continue;
      }

      printf("x = %5d, y = %5d, left = %d, right = %d, middle = %d\n", x, y,
             left, right, middle);
    }
  }

  fprintf(stderr, "test_kbd error: %s\n", strerror(errno));
  return EXIT_FAILURE;
}
