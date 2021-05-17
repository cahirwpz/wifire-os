# vim: tabstop=8 shiftwidth=8 noexpandtab:

TOPDIR = $(realpath ../..)

SOURCES-MIPS = \
	board.c \
	boot.c \
	context.c \
	copy.S \
	cpu.c \
	ebase.S \
	interrupt.c \
	pmap.c \
	sigcode.S \
	signal.c \
	start.S \
	switch.S \
	thread.c \
	timer.c \
	tlb.c \
	trap.c

CLEAN-FILES += assym.h

CPPFLAGS += -D_MACHDEP

include $(TOPDIR)/build/build.kern.mk

$(SOURCES-MIPS): assym.h

boot.o : CFLAGS_KASAN =
boot.o : CFLAGS_KGPROF =
