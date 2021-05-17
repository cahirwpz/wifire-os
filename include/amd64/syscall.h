#ifndef _AMD64_SYSCALL_H_
#define _AMD64_SYSCALL_H_

#ifdef __ASSEMBLER__

#include <amd64/asm.h>

/* Do a syscall that is missing an implementation. */
#define SYSCALL_MISSING(name)                                                  \
  ENTRY(name);                                                                 \
  jmp name;                                                                    \
  END(name)

/* Do a syscall that cannot fail. */
#define SYSCALL_NOERROR(name, num)                                             \
  ENTRY(name);                                                                 \
  jmp name;                                                                    \
  END(name)

/* Do a normal syscall. */
#define SYSCALL(name, num)                                                     \
  ENTRY(name);                                                                 \
  jmp name;                                                                    \
  END(name)

#endif /* !__ASSEMBLER__ */

#endif /* !_AMD64_SYSCALL_H_ */
