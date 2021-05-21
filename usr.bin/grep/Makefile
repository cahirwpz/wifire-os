TOPDIR = $(realpath ../..)

PROGRAM = grep
SOURCES = fastgrep.c file.c grep.c queue.c util.c
FORMAT-EXCLUDE = $(SOURCES)

CPPFLAGS = -DWITHOUT_GZIP -DWITHOUT_BZ2 -DWITHOUT_NLS
CFLAGS = -Wno-format-nonliteral

include $(TOPDIR)/build/build.prog.mk
