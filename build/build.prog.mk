# This is a common makefile for various simple usermode programs that reside in
# subdirectories. This is mostly a template for program-specific
# makefiles. Generally custom user programs will want to provide their own
# makefile, but for our own purposes this template is very convenient.

# This template assumes following make variables are set:
#  PROGRAM: The name for the resulting userspace ELF file. Generally, this will
#  be the program name installed into /bin directory.
#  SOURCES: C or assembly files to compile. Can be omitted if there's only one
#  file to compile named $(PROGRAM).c

ifndef PROGRAM 
$(error PROGRAM is not set)
endif

SOURCES ?= $(PROGRAM).c

all: $(PROGRAM).uelf

include $(TOPDIR)/build/build.mk
include $(TOPDIR)/build/flags.user.mk

clean::
	$(RM) $(PROGRAM).uelf

# Linking the program according to the provided script
$(PROGRAM).uelf: $(OBJECTS)
	@echo "[LD] $(DIR)$< -> $(DIR)$@"
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS)

install: $(SYSROOT)/bin/$(PROGRAM)

$(SYSROOT)/bin/$(PROGRAM): $(PROGRAM).uelf
	@echo "[INSTALL] $(DIR)$< -> /bin/$(PROGRAM)"
	install -D $(PROGRAM).uelf $(SYSROOT)/bin/$(PROGRAM)
	@echo "[STRIP] /bin/$(PROGRAM)"
	$(STRIP) --strip-all $(SYSROOT)/bin/$(PROGRAM)

.PRECIOUS: $(PROGRAM).uelf
