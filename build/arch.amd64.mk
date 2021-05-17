# vim: tabstop=8 shiftwidth=8 noexpandtab:
#
# Common makefile which specifies aarch64 architecture specific settings.
#
# Required common variables: KERNEL, BOARD.
#

TARGET := x86_64-mimiker-elf
GCC_ABIFLAGS :=
CLANG_ABIFLAGS := -target x86_64-elf
ELFTYPE := elf64-x86-64
ELF32TYPE := elf32-i386
ELFARCH := i386:x86-64
# To reduce the executable size, we change max page size 
# from the default 0x200000 to 0x1000.
LDFLAGS += -z max-page-size=0x1000
KERNEL-IMAGE := mimiker.elf32

ifeq ($(KERNEL), 1)
# In kernel the privilege level is 0, and thus an interrupt handling 
# doesn't switch the stack, therefore the current stack is used. 
# Leaf function are free to use so called red zones specified in
# https://uclibc.org/docs/psABI-x86_64.pdf (3.2.2 The Stack Frame).
# The following flag ensures that GCC won't use red zones throughout 
# the kernel (instead, data will be explicitly allocated on the stack).
CFLAGS += -mno-red-zone 
# Avoid SIMD registers to increase performance.
CFLAGS += -mno-mmx -mno-sse -mno-avx
# Ensure absence of x87 registers.
CFLAGS += -msoft-float
endif
