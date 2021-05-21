TOPDIR = $(realpath ../..)

PROGRAM = resize
SOURCES = resize.c xstrings.c
FORMAT-EXCLUDE = $(SOURCES)

CFLAGS.resize.c = -Wno-discarded-qualifiers
CFLAGS.xstrings.c = -Wno-discarded-qualifiers

include $(TOPDIR)/build/build.prog.mk
