#ifndef _SYS_PIPE_H_
#define _SYS_PIPE_H_

#ifdef _KERNEL

#include <machine/vm_param.h>

/* size of pipe buffer */
#define PIPE_SIZE PAGESIZE

typedef struct proc proc_t;

int do_pipe2(proc_t *p, int fds[2], int flags);

#endif /* !_KERNEL */

#endif /* !_SYS_PIPE_H_ */
