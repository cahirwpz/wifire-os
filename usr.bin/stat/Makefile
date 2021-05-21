TOPDIR = $(realpath ../..)

PROGRAM = stat
FORMAT-EXCLUDE = $(SOURCES)
LINK = readlink

include $(TOPDIR)/build/build.prog.mk

WFLAGS += -Wno-format-nonliteral

install-here:
	@echo "[SYMLINK] $(BINDIR)/$(PROGRAM) -> $(BINDIR)/$(LINK)"
	$(LN) -s -f $(PROGRAM) $(SYSROOT)/$(BINDIR)/$(LINK)
