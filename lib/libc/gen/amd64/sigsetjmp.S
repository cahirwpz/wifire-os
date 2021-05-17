#include <amd64/asm.h>

/* int sigsetjmp(jmp_buf buf, int savesigs) */
ENTRY(sigsetjmp)
	# not implemented
	jmp sigsetjmp
END(sigsetjmp)

/* void siglongjmp(sigjmp_buf env, int val) */
ENTRY(siglongjmp)
	# not implemented
	jmp sigsetjmp
END(siglongjmp)
