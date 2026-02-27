ENTRY(_start)

SECTIONS
{
    /* Загружаем ядро по адресу 0x10000 */
    . = 0x10000;
    
    _kernel_start = .;

    .text ALIGN(16) : {
        *(.text)
        *(.text.*)
        *(.tagfs)
    }

    .rodata ALIGN(16) : {
        *(.rodata)
        *(.rodata.*)
    }

    .data ALIGN(16) : {
        *(.data)
        *(.data.*)
    }

    .bss ALIGN(16) : {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }
    
    /* Убеждаемся что все секции включены */
    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
    }
    _kernel_end = ALIGN(4096);
}
