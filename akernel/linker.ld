ENTRY(_start)

SECTIONS
{
    /* Load the kernel strictly at the 1MB physical address mark */
    . = 0x100000;

    .text : AT(ADDR(.text))
    {
        KEEP(*(.multiboot)) /* Force the 16-byte aligned header block to sit at byte zero */
        *(.text)
        *(.text.*)
    }

    .rodata : AT(ADDR(.rodata))
    {
        *(.rodata)
        *(.rodata.*)
    }

    .data : AT(ADDR(.data))
    {
        *(.data)
        *(.data.*)
    }

    _bss_start = .;
    .bss : AT(ADDR(.bss))
    {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }
    _bss_end = .;

    /DISCARD/ :
    {
        *(.note.gnu.property)
        *(.note.GNU-stack)
        *(.note.*)
        *(.comment)
        *(.eh_frame)
    }
}
