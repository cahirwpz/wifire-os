#include <amd64/asm.h>

# This is the starting procedude for user ELF images, used as their
# entry point. It takes care of preparing argc and argc for main().
# For details about environment in which this procedure starts, refer
# to prepare_program_stack in exec.c.

ENTRY(_start)
	# not implemented
	jmp _start
END(_start)

# vim: sw=8 ts=8 et
