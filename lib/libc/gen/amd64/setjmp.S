#include <amd64/asm.h>

/* int setjmp(jmp_buf env) */
ENTRY(setjmp)
	jmp setjmp
END(setjmp)

# vim: sw=8 ts=8 et
