#include <amd64/asm.h>

#
# long ctx_switch(thread_t *from, thread_t *to)
#
ENTRY(ctx_switch)
	# not implemented
	jmp ctx_switch
END(ctx_switch)

# vim: sw=8 ts=8 et
