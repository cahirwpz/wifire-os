#include <sys/event.h>

int kqueue(void) {
    return kqueue1(0);
}