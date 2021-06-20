# Debugging

Custom debugging tools for GDB.

Running
---

After you start the kernel in debugging mode, i.e. using
[launch](https://github.com/cahirwpz/mimiker#running) script with `-d` flag,
you can use following custom commands:

* `kdump` - Print on screen current state of kernel structures,
* `ktrace` - Add/remove breakpoint on selected function.

`kdump` parameter select which state to dump:

* `free_pages` - list of free pages per segment with virtual and physical
   addresses,
* `vm_map` - list memory map of process of given pid (when number is given as argument)
  or of current process (when no argument is given)
* `vm_address` - list all information about given address for process of given pid
* `vm_amap` - list all information about amap at given address
* `klog` - all log messages currently saved in the kernel,
* `threads` - all existing threads,
* `tlb` - Translation Lookaside Buffer with addresses and flags marking if page
  is dirty (D) and/or global (G).

`ktrace` parameter selects on which predefined function create/remove
breakpoints on:

* `thread-create` - function [thread_create](https://github.com/cahirwpz/mimiker/blob/master/sys/thread.c)
* `ctx-switch` - function [ctx_switch](https://github.com/cahirwpz/mimiker/blob/master/mips/switch.S)

How to debug user programs?
---

According to [mimiker.ld](https://github.com/cahirwpz/mimiker/blob/master/user/libmimiker/extra/mimiker.ld)
user space binaries are loaded at *0x400000* address. You could stop an entry
point to the program listed in ELF header. However using **gdb** without
debugging information is not practical in long-term. So... how to stop debugger
at user program `main` procedure and step through it in casual way?

You have to issuing following command (in this case for `utest` program) within
**gdb** session:

> `add-symbol-file user/utest/utest.uelf 0x400000`

... which will load debugging information, so you can use all gdb goodies in
user-space programs.
