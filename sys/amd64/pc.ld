/* Linker scripts are documented here:
 * https://sourceware.org/binutils/docs/ld/Scripts.html */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
  boot     PT_LOAD FLAGS(5); /* read-only, executable */
  text     PT_LOAD FLAGS(5); /* read-only, executable */
  rodata   PT_LOAD FLAGS(4); /* read-only */
  data     PT_LOAD FLAGS(6); /* read-write */
}

SECTIONS
{
  /* The first 1MB is used while operating in real address mode.
   * The kernel is loaded at the beginning of the extended memory. */
  . = 0x100000;

  .boot : ALIGN(4096)
  {
    __boot = ABSOLUTE(.);
    KEEP(amd64/amd64.ka:start.o)
    *(.boot .boot.*)
    __eboot = ABSOLUTE(.);
  } : boot

  /* TODO(MichalBlk): set appropriate VMA and LMA. */
  .text : ALIGN(4096)
  {
    __kernel_start = ABSOLUTE(.);
    __text = ABSOLUTE(.);
    *(.text .text.*)
    __etext = ABSOLUTE(.);
    /* TODO(MichalBlk): bother KASAN constructors. */
  } : text

  .rodata : ALIGN(4096)
  {
    *(.rodata .rodata.*)
  } : rodata

  .data : ALIGN(4096)
  {
    __data = ABSOLUTE(.);
    *(.data .data.*)
    . = ALIGN(4);
    __edata = ABSOLUTE(.);
  } : data

  .bss : ALIGN(4096)
  {
    __bss = ABSOLUTE(.);
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(4);
    __ebss = ABSOLUTE(.);
    __kernel_end = ABSOLUTE(.);
  }

  /* Sections to be discarded */
  /DISCARD/ :
  {
    *(.got .got.*)
    *(.igot .igot.*)
    *(.plt .plt.*)
    *(.iplt .iplt.*)
    *(.dynamic)
    *(.dynstr)
    *(.comment)
    *(.note)
    *(.options)
    *(.pdr)
    *(.reginfo)
    *(.gnu.attributes)
  }
}
