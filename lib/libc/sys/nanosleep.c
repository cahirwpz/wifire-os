#include <sys/time.h>

int nanosleep(timespec_t *rqtp, timespec_t *rmtp) {
  return clock_nanosleep(CLOCK_REALTIME, 0, rqtp, rmtp);
}
