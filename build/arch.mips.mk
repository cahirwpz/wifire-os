# vim: tabstop=8 shiftwidth=8 noexpandtab:
#
# Common makefile which specifies MIPS architecture specific settings.
#
# Required common variables: KERNEL, BOARD.

TARGET := mipsel-mimiker-elf
# -G 0 disables small-data and small-bss,
# as otherwise they would exceed 64KB limit
GCC_ABIFLAGS := -mips32r2 -EL -G 0
CLANG_ABIFLAGS := -target mipsel-elf -march=mips32r2 -mno-abicalls \
		  -modd-spreg -G 0
ELFTYPE := elf32-littlemips 
ELFARCH := mips

ifeq ($(KERNEL), 1)
ifeq ($(KASAN), 1)
  # Added to files that are sanitized
  CFLAGS_KASAN = -fsanitize=kernel-address -fasan-shadow-offset=0xD8000000 \
                 --param asan-globals=1 \
                 --param asan-stack=1 \
                 --param asan-instrument-allocas=1
endif
# Added to all files
GCC_ABIFLAGS += -msoft-float
CLANG_ABIFLAGS += -msoft-float
ifeq ($(KGPROF), 1)
  CFLAGS_KGPROF = -finstrument-functions
endif
endif
