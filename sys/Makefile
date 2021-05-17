# vim: tabstop=8 shiftwidth=8 noexpandtab:

TOPDIR = $(realpath ..)

include $(TOPDIR)/config.mk

SUBDIR = aarch64 drv kern libkern mips tests
KLIB = no

BUILD-FILES += $(KERNEL-IMAGE) mimiker.elf cscope.out etags tags
CLEAN-FILES += mimiker.elf.map

include $(TOPDIR)/build/build.kern.mk

LDFLAGS += -T $(TOPDIR)/sys/$(ARCH)/$(ARCH).ld
LDLIBS	= -Wl,--start-group \
	    -Wl,--whole-archive $(KLIBLIST) -Wl,--no-whole-archive \
            -lgcc \
          -Wl,--end-group

mimiker.elf: $(KLIBDEPS)
	@echo "[LD] Linking kernel image: $@"
	$(LD) $(LDFLAGS) -Wl,-Map=$@.map $(LDLIBS) -o $@

mimiker.elf32: mimiker.elf
	@echo "[OBJCOPY] Prepare kernel image: $@"
	$(OBJCOPY) -I $(ELFTYPE) -O $(ELF32TYPE) $^ $@

mimiker.img: mimiker.elf
	@echo "[OBJCOPY] Prepare kernel image: $@"
	$(OBJCOPY) -O binary $^ $@

mimiker.img.gz: mimiker.img
	@echo "[GZIP] Compress kernel image: $@"
	$(GZIP) -f $^ > $@

# Lists of all files that we consider operating system kernel sources.
SRCDIRS = $(TOPDIR)/include $(TOPDIR)/sys
SRCFILES_C = $(shell find $(SRCDIRS) -iname '*.[ch]')
SRCFILES_ASM = $(shell find $(SRCDIRS) -iname '*.S')
SRCFILES = $(SRCFILES_C) $(SRCFILES_ASM)

cscope.out: $(SRCFILES)
	@echo "[CSCOPE] Rebuilding cscope database..."
	$(CSCOPE) $(SRCFILES)

etags tags:
	@echo "[CTAGS] Rebuilding tags database..."
	$(CTAGS) --language-force=c $(SRCFILES_C)
	$(CTAGS) --language-force=c -e -f etags $(SRCFILES_C)
	$(CTAGS) --language-force=asm -a $(SRCFILES_ASM)
	$(CTAGS) --language-force=asm -aef etags $(SRCFILES_ASM)

BUILD-FILES += cscope.out etags tags
CLEAN-FILES += *.taghl
