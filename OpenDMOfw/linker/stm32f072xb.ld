/* STM32F072xB (CBT6): 128 KB flash @ 0x08000000, 16 KB SRAM @ 0x20000000.
   Cortex-M0, main stack at the top of SRAM.
   (x8 is the 64 KB part; do not use that name for this 128 KB layout.) */

ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 128K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
}

/* 2 KB stack, 1 KB heap-reserve (newlib-nano uses little RAM). */
_estack     = ORIGIN(RAM) + LENGTH(RAM);
_Min_Stack_Size = 0x800;
_Min_Heap_Size  = 0x400;

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.rodata)
    *(.rodata*)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM       : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT >FLASH

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /* No /DISCARD/ of libc.a / libm.a / libgcc.a here (the ST template carries
     one). Cortex-M0 has no hardware divide, so libgcc supplies __aeabi_uidivmod
     and __aeabi_uldivmod - both are actually referenced (usb_desc_init_serial,
     store.c page math). --gc-sections already drops what is unused. */
  .ARM.attributes 0 : { *(.ARM.attributes) }
}
