/* Flat, position-independent CODE image: .text/.rodata/.data in file order. Base is 0.
 * This image is written into an executable-but-not-game-writable region, so it must contain NO writable
 * state except the one state pointer in .data (tools/gen_blob.py enforces: .data == 8 bytes, .bss == 0).
 * All mutable state is struct sky_hook_state, allocated separately in a writable region. */
ENTRY(skyhook_Init)
SECTIONS
{
  . = 0;
  .text   : { *(.text .text.*) }
  . = ALIGN(16);
  .rodata : { *(.rodata .rodata.*) }
  . = ALIGN(16);
  __data_start = .;
  .data   : { *(.data .data.*) }
  __data_end = .;
  . = ALIGN(16);
  __bss_start = .;
  .bss    : { *(.bss .bss.*) *(COMMON) }
  __bss_end = .;
  _blob_end = .;
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu*) *(.got*) *(.plt*) *(.dynamic) *(.interp) }
}
