#include <amd64/asm.h>
#include "assym.h"

#define MULTIBOOT_HEADER_CHECKSUM (-MULTIBOOT_HEADER_MAGIC)

	.align MULTIBOOT_HEADER_ALIGN

multiboot_header_start:

	.long MULTIBOOT_HEADER_MAGIC
	.long 0
	.long MULTIBOOT_HEADER_CHECKSUM

multiboot_header_end:

	.code32

_ENTRY(_start)
	jmp _start
_END(_start)

# vim: sw=8 ts=8 et
