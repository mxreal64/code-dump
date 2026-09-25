# Define Multiboot 1 constants
.set MAGIC,    0x1BADB002
.set FLAGS,    0x00000003      # Bit 0 (align modules), Bit 1 (provide memory map)
.set CHECKSUM, -(MAGIC + FLAGS)

# CRITICAL FIX: Explicitly isolated header block padded to a 16-byte boundary
.section .multiboot
.align 16
multiboot_header:
    .long MAGIC
    .long FLAGS
    .long CHECKSUM
    .long 0                    # Explicit 4-byte padding word to force total header size to 16 bytes

.section .bootstrap_stack, "aw", @nobits
stack_bottom:
.skip 16384 # 16KB of stack space for Core 0
stack_top:

.section .text
.global _start
.code32

_start:
    # Disable interrupts immediately
    cli

    # Set up our initial stack pointer
    mov $stack_top, %esp

    # --- SET UP PAGING TO TRANSITION TO 64-BIT LONG MODE ---
    mov $0x9000, %edi
    xor %eax, %eax
    mov $4096, %ecx
    rep stosl

    movl $0xA003, (0x9000)      # PML4 -> PDPT
    movl $0xB003, (0xA000)      # PDPT -> PD_0
    movl $0x00000083, (0xB000)  # PD_0 -> 0x00000000 (2MB page)

    mov $0x9000, %eax
    mov %eax, %cr3

    mov %cr4, %eax
    or $0x20, %eax
    mov %eax, %cr4

    mov $0xC0000080, %ecx
    rdmsr
    or $0x00000100, %eax
    wrmsr

    mov %cr0, %eax
    or $0x80000001, %eax
    mov %eax, %cr0

    lgdt gdt64_pointer
    ljmp $0x08, $long_mode_start

.code64
long_mode_start:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    xor %rbp, %rbp
    call kernel_main

halt_loop:
    hlt
    jmp halt_loop

.align 8
gdt64:
    .quad 0x0000000000000000
    .quad 0x00209A0000000000
    .quad 0x0000920000000000

gdt64_pointer:
    .word . - gdt64 - 1
    .quad gdt64
