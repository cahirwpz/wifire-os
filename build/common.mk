# vim: tabstop=8 shiftwidth=8 noexpandtab:
#
# Common makefile used throughout the mimiker build system.  It defines basic
# mimiker specific recipes and the structure of the build system itself. 
#
# The following make variables are set by the including makefile:
# - SRCDIR: Source directory path relative to $(TOPDIR). This may be used
#   to build some sources outside the cwd (current working directory).
#   Defaults to cwd.
# - SOURCES_C: C sources to format at the current recursion level
#   (besides format-here).
# - SOURCES_H: C headers to format at the current recursion level
#   (besides format-here).
#

SYSROOT = $(TOPDIR)/sysroot
DIR = $(patsubst $(TOPDIR)/%,%,$(CURDIR)/)

# Pass "VERBOSE=1" at command line to display command being invoked by GNU Make
ifneq ($(VERBOSE), 1)
.SILENT:
endif

# Disable all built-in recipes
.SUFFIXES:

SRCPATH = $(subst $(TOPDIR)/,,$(realpath $<))
DSTPATH = $(DIR)$@

# Define our own recipes
%.S: %.c
	@echo "[CC] $(SRCPATH) -> $(DSTPATH)"
	$(CC) $(CFLAGS) $(CFLAGS.$*.c) $(CPPFLAGS) $(WFLAGS) -S -o $@ \
	      $(realpath $<)

%.o: %.c
	@echo "[CC] $(SRCPATH) -> $(DSTPATH)"
	$(CC) $(CFLAGS) $(CFLAGS.$*.c) $(CFLAGS_KASAN) $(CFLAGS_KGPROF) $(CPPFLAGS) $(WFLAGS) \
	      -c -o $@ $(realpath $<)

%.o: %.S
	@echo "[AS] $(SRCPATH) -> $(DSTPATH)"
	$(AS) $(ASFLAGS) $(CPPFLAGS) -c -o $@ $(realpath $<)

%.c: %.y
	@echo "[YACC] $(SCRPATH) -> $(DSTPATH)"
	$(YACC) -o $@ $(realpath $<)

%.a:
	@echo "[AR] $(addprefix $(DIR),$^) -> $(DSTPATH)"
	$(AR) rs $@ $^ 2> /dev/null

assym.h: genassym.cf
	@echo "[ASSYM] $(DSTPATH)"
	$(GENASSYM) $(CC) $(ASSYM_CFLAGS) $(CFLAGS) $(CPPFLAGS) < $^ > $@

%/assym.h: %/genassym.cf
	@echo "[ASSYM] $(DSTPATH)"
	$(GENASSYM) $(CC) $(ASSYM_CFLAGS) $(CFLAGS) $(CPPFLAGS) < $^ > $@

include $(TOPDIR)/config.mk
include $(TOPDIR)/build/arch.$(ARCH).mk
include $(TOPDIR)/build/tools.mk

SRCDIR ?= .

vpath %.c $(SRCDIR)
vpath %.S $(SRCDIR)/$(ARCH)

# Recursive rules for subdirectories
%-format:
	@echo "[MAKE] format $(DIR)$*"
	$(MAKE) -C $* format

%-download:
	@echo "[MAKE] download $(DIR)$*"
	$(MAKE) -C $* download 

%-build: %-download %-before
	@echo "[MAKE] build $(DIR)$*"
	$(MAKE) -C $* build

%-install: %-build
	@echo "[MAKE] install $(DIR)$*"
	$(MAKE) -C $* install

%-clean:
	@echo "[MAKE] clean $(DIR)$*"
	$(MAKE) -C $* clean

%-distclean: %-clean
	@echo "[MAKE] distclean $(DIR)$*"
	$(MAKE) -C $* distclean

PHONY-TARGETS += $(SUBDIR:%=%-before)

download-recursive: $(SUBDIR:%=%-download)
build-recursive: $(SUBDIR:%=%-build)
install-recursive: $(SUBDIR:%=%-install)
clean-recursive: $(SUBDIR:%=%-clean)
distclean-recursive: $(SUBDIR:%=%-distclean)
format-recursive: $(SUBDIR:%=%-format)

# Define main rules of the build system
download: download-recursive download-here
build: $(DEPENDENCY-FILES) build-recursive $(BUILD-FILES) build-here
install: install-recursive $(INSTALL-FILES) install-here
clean: clean-recursive clean-here
	$(RM) -v $(CLEAN-FILES)
	$(RM) -v $(BUILD-FILES)
	$(RM) -v *~
distclean: distclean-recursive distclean-here

FORMAT-FILES = $(filter-out $(FORMAT-EXCLUDE),$(SOURCES_ALL_C) $(SOURCES_H))
FORMAT-RECURSE ?= format-recursive

format: $(FORMAT-RECURSE) format-here
ifneq ($(FORMAT-FILES),)
	@echo "[FORMAT] $(FORMAT-FILES)"
	$(FORMAT) -i $(FORMAT-FILES)
endif

PHONY-TARGETS += all no
PHONY-TARGETS += build build-dependencies build-recursive build-here
PHONY-TARGETS += clean clean-recursive clean-here
PHONY-TARGETS += install install-recursive install-here
PHONY-TARGETS += distclean distclean-recursive distclean-here
PHONY-TARGETS += download download-recursive download-here
PHONY-TARGETS += format format-recursive format-here

.PHONY: $(PHONY-TARGETS)
.PRECIOUS: $(BUILD-FILES)
