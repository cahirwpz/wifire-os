# vim: tabstop=8 shiftwidth=8 noexpandtab:

TOPDIR = $(realpath ../..)

SOURCES = \
	bus.c \
	callout.c \
	clock.c \
	cmdline.c \
	condvar.c \
	console.c \
	cred.c \
	cred_checks.c \
	cred_syscalls.c \
	devclass.c \
	device.c \
	dev_null.c \
	dev_procstat.c \
	devfs.c \
	exception.c \
	exec.c \
	exec_elf.c \
	exec_shebang.c \
	fdt.c \
	file.c \
	file_syscalls.c \
	filedesc.c \
	fork.c \
	initrd.c \
	interrupt.c \
	kenv.c \
	klog.c \
	kmem.c \
	ktest.c \
	main.c \
	malloc.c \
	mutex.c \
	mmap.c \
	pcpu.c \
	pipe.c \
	pool.c \
	proc.c \
	pty.c \
	ringbuf.c \
	rman.c \
	runq.c \
	sbrk.c \
	sched.c \
	signal.c \
	sleepq.c \
	spinlock.c \
	syscalls.c \
	taskqueue.c \
	turnstile.c \
	thread.c \
	time.c \
	timer.c \
	tmpfs.c \
	tty.c \
	uart_tty.c \
	uio.c \
	ustack.c \
	vfs.c \
	vfs_name.c \
	vfs_readdir.c \
	vfs_syscalls.c \
	vfs_vnode.c \
	vm_map.c \
	vm_object.c \
	vm_pager.c \
	vm_physmem.c \
	vmem.c

SOURCES-KASAN = \
	kasan.c \
	kasan_quar.c

SOURCES-LOCKDEP = \
	lockdep.c

SOURCES-KGPROF = \
	kgprof.c

ifeq ($(KGPROF), 1)
SOURCES += mcount.c
mcount.o : CFLAGS_KGPROF =
else
CLEAN-FILES += .mcount.D
endif

FORMAT-EXCLUDE = sysent.h

include $(TOPDIR)/build/build.kern.mk

SYSCALL_H = $(TOPDIR)/include/sys/syscall.h
SYSCALLARGS_H = $(TOPDIR)/include/sys/syscallargs.h

sysent.h $(SYSCALL_H) $(SYSCALLARGS_H): syscalls.conf syscalls.master
	@echo "[MAKESYSCALLS] $(lastword $^) -> $@"
	$(TOPDIR)/sys/script/makesyscalls.sh $^

kasan.o : CFLAGS_KASAN =
