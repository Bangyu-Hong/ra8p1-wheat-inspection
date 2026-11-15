/*
                  Linker File for Renesas FSP
*/

INCLUDE memory_regions.ld
INCLUDE fsp_gen.ld

/* This deployment targets the 32 MiB U3 flash assembly detected by the board
 * programming loader.  The schematic also permits an incompatible 4 MiB U6
 * assembly, so runtime full-image CRC and a pending JEDEC identity gate remain
 * part of hardware qualification.  The generated FSP linker script treats
 * .ospi0_cs0 as NOLOAD, so keep the immutable model and NPU command stream in
 * explicit loadable sections. */
SECTIONS
{
  .wheat_model_image ORIGIN(OSPI0_CS0) :
  {
    __wheat_model_image_start__ = .;
    KEEP(*(.wheat_model_image))
    . = ALIGN(16);
    KEEP(*(.wheat_command_stream_image))
    __wheat_model_image_end__ = .;
  } > OSPI0_CS0

  .web_asset_image ALIGN(4096) :
  {
    __web_asset_image_start__ = .;
    KEEP(*(.web_asset_image))
    . = ALIGN(32);
    __web_asset_image_end__ = .;
  } > OSPI0_CS0
}

/* The generated memory_regions.ld describes the controller windows, not the
 * fitted memories.  Enforce the physical CPKHMI capacities at link time. */
ASSERT(__wheat_model_image_start__ >= 0x80000000,
       "wheat model image starts before physical OSPI0 CS0")
ASSERT(__wheat_model_image_end__ <= 0x82000000,
       "wheat model image exceeds physical 32 MiB OSPI0 CS0")
ASSERT(__web_asset_image_start__ >= __wheat_model_image_end__,
       "web asset overlaps wheat model image")
ASSERT(__web_asset_image_end__ <= 0x82000000,
       "web asset exceeds physical 32 MiB OSPI0 CS0")
ASSERT(__ddsc_SDRAM_END <= 0x69000000,
       "SDRAM sections exceed physical 16 MiB SDRAM")
ASSERT(__ddsc_FLASH_END <= 0x02100000,
       "FLASH sections exceed physical 1 MiB MRAM")
