TOPDIR = $(realpath ../..)

PROGRAM = tail
SOURCES = forward.c misc.c read.c reverse.c tail.c
FORMAT-EXCLUDE = $(SOURCES)

include $(TOPDIR)/build/build.prog.mk

CFLAGS.tail.c += -Wno-misleading-indentation
