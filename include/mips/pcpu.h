#ifndef _MIPS_PCPU_H_
#define _MIPS_PCPU_H_

#ifndef _MACHDEP
#error "Do not use this header file outside kernel machine dependent code!"
#endif

#ifdef __ASSEMBLER__

#include <mips/asm.h>
#include <mips/mips.h>

#define LOAD_PCPU(reg) LA reg, _pcpu_data

#endif /* !__ASSEMBLER__ */

#endif /* !_MIPS_PCPU_H_ */
