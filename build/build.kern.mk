# vim: tabstop=8 shiftwidth=8 noexpandtab:

# Files required to link kernel image
KRT = $(foreach dir,$(KERNDIR),$(TOPDIR)/$(dir)/lib$(dir).ka)

# Process subdirectories before using KRT files.
$(KRT): | $(KERNDIR)
	true # Disable default recipe

LDFLAGS = -T $(TOPDIR)/mips/malta.ld
LDLIBS	= -Wl,--start-group \
	    -Wl,--whole-archive $(KRT) -Wl,--no-whole-archive \
            -lgcc \
          -Wl,--end-group

mimiker.elf: $(KRT) initrd.o | $(KERNDIR)
	@echo "[LD] Linking kernel image: $@"
	$(CC) $(LDFLAGS) -Wl,-Map=$@.map $(LDLIBS) initrd.o -o $@

# Detecting whether initrd.cpio requires rebuilding is tricky, because even if
# this target was to depend on $(shell find sysroot -type f), then make compares
# sysroot files timestamps BEFORE recursively entering bin and installing user
# programs into sysroot. This sounds silly, but apparently make assumes no files
# appear "without their explicit target". Thus, the only thing we can do is
# forcing make to always rebuild the archive.
initrd.cpio: force
	$(MAKE) -C lib install
	$(MAKE) -C bin install
	@echo "[INITRD] Building $@..."
	cd sysroot && find -depth -print | $(CPIO) -o -F ../$@ 2> /dev/null

initrd.o: initrd.cpio
	$(OBJCOPY) -I binary -O elf32-littlemips -B mips \
	  --rename-section .data=.initrd,alloc,load,readonly,data,contents \
	  $^ $@

include $(TOPDIR)/build/flags.kern.mk
include $(TOPDIR)/build/toolchain.mk
include $(TOPDIR)/build/tools.mk
include $(TOPDIR)/build/common.mk
