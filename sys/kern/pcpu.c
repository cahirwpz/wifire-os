#include <sys/pcpu.h>
#include <sys/thread.h>

#define PANIC_STACK_SIZE PAGESIZE

static char panic_stack[PANIC_STACK_SIZE];

pcpu_t _pcpu_data[1] = {{
  .curthread = &thread0,
  .panic_sp = &panic_stack[PANIC_STACK_SIZE],
}};
