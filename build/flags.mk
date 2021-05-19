# vim: tabstop=8 shiftwidth=8 noexpandtab:
#
# This is a common makefile used to set the basic compilation flags.
#

ASFLAGS  += -Wall -Wextra -Werror
WFLAGS   += -Wall -Wextra -Wno-unused-parameter -Wstrict-prototypes -Werror
OPTFLAGS ?= -Og
CFLAGS   += -std=gnu11 $(OPTFLAGS) -ggdb3 -fomit-frame-pointer
CPPFLAGS += -DDEBUG
