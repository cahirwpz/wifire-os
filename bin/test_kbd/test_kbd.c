#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/input.h>

#define BUFSIZE 64

typedef struct input_event input_event_t;

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

      /* We only care about keycodes. */
      if (evp->type != EV_KEY)
        continue;

      /* We shouldn't see any reserved keycodes. */
      assert(evp->code != KEY_RESERVED);

      const char *state = evp->value ? "pressed" : "released";
      printf("keycode: %x, state: %s\n", (int)evp->code, state);
    }
  }

  fprintf(stderr, "test_kbd error: %s\n", strerror(errno));
  return EXIT_FAILURE;
}
