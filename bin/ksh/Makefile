TOPDIR = $(realpath ../..)

CPPFLAGS += -I.

PROGRAM = ksh
SOURCES = alloc.c c_ksh.c c_sh.c c_test.c c_ulimit.c edit.c emacs.c \
	eval.c exec.c expr.c history.c io.c jobs.c lex.c mail.c \
	main.c misc.c path.c shf.c sigact.c syn.c table.c trap.c \
	tree.c tty.c var.c version.c vi.c
FORMAT-EXCLUDE = $(SOURCES)

all: build

clean-here:
	$(RM) siglist.out siglist.out.tmp
	$(RM) emacs.out emacs.out.tmp

# two steps to prevent the creation of a bogus siglist.out
siglist.out: config.h sh.h siglist.in siglist.sh
	./siglist.sh "$(CC) -E $(CPPFLAGS) $(DEFS) -I. -I$(CURDIR)" \
	    < $(CURDIR)/siglist.in > siglist.out.tmp \
	    && mv siglist.out.tmp siglist.out

# two steps to prevent the creation of a bogus emacs.out
emacs.out: emacs.c
	./emacs-gen.sh $(CURDIR)/emacs.c > emacs.out.tmp \
	    && mv emacs.out.tmp emacs.out

ifneq ($(CLANG), 1)
# stringop-overflow is a GCC-specific warning
CFLAGS.history.c += -Wno-stringop-overflow
endif

include $(TOPDIR)/build/build.prog.mk
