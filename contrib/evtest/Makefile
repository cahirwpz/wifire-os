TOPDIR = $(realpath ../..)

EVTEST_URL = https://github.com/freedesktop-unofficial-mirror/evtest.git

PROGRAM = evtest
BINDIR = usr/bin
SOURCES = evtest/evtest.c versionsort.c
PATCHES = 01.patch
CLEAN-FILES = patch-stamp

FORMAT-EXCLUDE = $(SOURCES)

all: download build install

evtest:
	@echo "[GIT] clone $(EVTEST_URL) -> $@"
	$(GIT) clone $(EVTEST_URL) $@

patch: patch-stamp
patch-stamp:
	@for p in $(PATCHES); do \
	  echo "[PATCH] apply $$p"; \
	  $(PATCH) -d evtest -p1 < $$p; \
	done
	touch $@

download-here: evtest patch

distclean-here:
	$(RM) -r evtest

include $(TOPDIR)/build/build.prog.mk

WFLAGS += -Wno-strict-prototypes -Wno-implicit-fallthrough -Wno-sign-compare \
          -Wno-char-subscripts -Wno-override-init
