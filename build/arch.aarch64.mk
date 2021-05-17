# vim: tabstop=8 shiftwidth=8 noexpandtab:
#
# Common makefile which specifies aarch64 architecture specific settings.
#
# Required common variables: KERNEL, BOARD.
#

TARGET := aarch64-mimiker-elf
GCC_ABIFLAGS :=
CLANG_ABIFLAGS := -target aarch64-elf
ELFTYPE := elf64-littleaarch64
ELFARCH := aarch64

ifeq ($(KERNEL), 1)
	CFLAGS += -mcpu=cortex-a53+nofp -march=armv8-a+nofp -mgeneral-regs-only
	ifeq ($(KASAN), 1)
	# Added to files that are sanitized
	CFLAGS_KASAN = -fsanitize=kernel-address \
		       -fasan-shadow-offset=0xe0001f0000000000 \
		       --param asan-globals=1 \
		       --param asan-stack=1 \
		       --param asan-instrument-allocas=1
	endif
	ifeq ($(KGPROF), 1)
  CFLAGS_KGPROF = -finstrument-functions
	endif
else
	CFLAGS += -mcpu=cortex-a53 -march=armv8-a
endif

ifeq ($(BOARD), rpi3)
	KERNEL-IMAGE := mimiker.img.gz
endif
