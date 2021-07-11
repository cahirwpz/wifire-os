TOPDIR = $(realpath ../..)

CPPFLAGS += -I.
LDLIBS += -lutil

SOURCES = \
	  args.c \
	  conv.c \
	  conv_tab.c \
	  dd.c \
	  dd_hostops.c \
	  dd_swab.c \
	  misc.c \
	  position.c

PROGRAM = dd
FORMAT-EXCLUDE = $(SOURCES)

include $(TOPDIR)/build/build.prog.mk
