/* Linker scripts are documented here:
 * https://sourceware.org/binutils/docs/ld/Scripts.html */
OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64) 
ENTRY(_start)

PHDRS
{
  boot   PT_LOAD FLAGS(5); /* read-only, executable */
  text   PT_LOAD FLAGS(5); /* read-only, executable */
  rodata PT_LOAD FLAGS(4); /* read-only */
  data   PT_LOAD FLAGS(6); /* read-write */
}

PROVIDE(__kernel_size = (__kernel_end - __kernel_start) + (__eboot - __boot));

SECTIONS
{
  .boot 0x200000 : AT(0x200000) ALIGN(4096)
  {
    __boot = ABSOLUTE(.);
    KEEP(*(.boot))
    *(.boot.text)
    *(.boot.data)
    . = ALIGN (4096);
    __eboot = ABSOLUTE(.);
  } : boot

  HIDDEN(_kernel = 0xffff000000000000);

  .text _kernel + __eboot : AT(__eboot) ALIGN(4096)
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

  .rodata : ALIGN(4096)
  {
    *(.rodata .rodata.*)
  } : rodata

  .data : ALIGN(4096)
  {
    __data = ABSOLUTE(.);
    *(.data .data.*)
    . = ALIGN (4);
    __edata = ABSOLUTE(.);
  } : data

  .bss : ALIGN(4096)
  {
    __bss = ABSOLUTE(.);
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN (4096);
    __ebss = ABSOLUTE(.);
    __kernel_end = ABSOLUTE(.);
  }

  /* Sections to be discarded */
  /DISCARD/ :
  {
    *(.ARM.*)
    *(.comment)
    *(.note)
    *(.options)
    *(.pdr)
    *(.reginfo)
    *(.gnu.attributes)
  }
}
