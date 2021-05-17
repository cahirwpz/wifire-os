# vim: tabstop=8 shiftwidth=8 noexpandtab:

TOPDIR = $(realpath ../..)

SOURCES = boot.c \
	  context.c \
	  copy.S \
	  evec.S \
	  interrupt.c \
	  pmap.c \
	  rootdev.c \
	  signal.c \
	  start.S \
	  switch.S \
	  thread.c

CLEAN-FILES += assym.h

CPPFLAGS += -D_MACHDEP

include $(TOPDIR)/build/build.kern.mk

$(SOURCES): assym.h
