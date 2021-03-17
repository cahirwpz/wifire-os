#ifndef _MIPS_VM_PARAM_H_
#define _MIPS_VM_PARAM_H_

#define KERNEL_SPACE_BEGIN 0xc0000000
#define KERNEL_SPACE_END 0xffffe000

#define USER_SPACE_BEGIN 0x00400000
#define USER_SPACE_END 0x80000000

#define USER_STACK_TOP 0x7f800000
#define USER_STACK_SIZE 0x800000 /* grows down up to that size limit */

#define PAGESIZE 4096
#define PAGE_SHIFT 12
#define SUPERPAGESIZE (1 << 22) /* 4 MB */

#define VM_PHYSSEG_NMAX 16

#endif /* !_MIPS_VM_PARAM_H_ */
