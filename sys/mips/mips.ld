/* Linker scripts are documented here:
 * https://sourceware.org/binutils/docs/ld/Scripts.html */
OUTPUT_ARCH(mips)
ENTRY(_start)
PHDRS
{
  /* The boot segment is for things like boot code,
   * exception handlers and debug data/routines that all
   * must be in kseg0. It has all permission bits set
   * because it contains code as well as writable data. */
  boot   PT_LOAD FLAGS(7);      /* read-write, executable */
  text   PT_LOAD FLAGS(5);      /* read-only, executable */
  rodata PT_LOAD FLAGS(4);      /* read-only */
  data   PT_LOAD FLAGS(6);      /* read-write */
}
SECTIONS
{
  /* RAM is mapped in kseg0 (cacheable) and kseg1 (non-cacheable) */
  _ram = 0x80000000;

  .boot 0x80100000: AT(0x100000) ALIGN(4096)
  {
    __boot = ABSOLUTE(.);
    /* Exception entry points. */
    *(.ebase)
    . = ALIGN(0x1000);
    /*
     * The .boot.* sections also contain code and data used by GDB commands.
     * The debugger must always be able to jump to code in this section and to
     * read/write data in this section, so it should be placed in kseg0.
     */
    *(.boot .boot.*)
    __eboot = ABSOLUTE(.);
  } : boot

  .text 0xc0102000 : AT(0x102000) ALIGN(4096)
  {
    __kernel_start = ABSOLUTE(.);
    __text = ABSOLUTE(.);
    *(.text .text.*)
    __etext = ABSOLUTE(.);
    /* Constructors are used by KASAN to initialize global redzones */
    __CTOR_LIST__ = ABSOLUTE(.);
    *(.ctors)
    __CTOR_END__ = ABSOLUTE(.);
  } : text

  /* In order to be addressable using a 16-bit signed offset from _gp,
   * this section must be placed close to the .data section, as
   * _gp is defined here as the start of the .data section.  */
  .rodata : ALIGN(4)
  {
    *(.rodata .rodata.*)
  } : rodata

  .data : ALIGN(4096)
  {
    __data = ABSOLUTE(.);
    _gp = .;
    *(.data .data.*)
    *(.sdata .sdata.*)
    . = ALIGN (4);
    __edata = ABSOLUTE(.);
  } : data

  .bss : ALIGN(4)
  {
    __bss = ABSOLUTE(.);
    *(.sbss .scommon)
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN (4096);
    __ebss = ABSOLUTE(.);
    __kernel_end = ABSOLUTE(.);
  }

  /* Sections to be discarded */
  /DISCARD/ :
  {
    *(.comment)
    *(.note)
    *(.options)
    *(.pdr)
    *(.reginfo)
    *(.gnu.attributes)
    *(.MIPS.abiflags)
    *(.MIPS.options)
  }
}
