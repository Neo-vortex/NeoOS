ENTRY(_start)

/* PML4 index 64 (0x0000200000000000) -- deliberately NOT under PML4
   index 0. Every process's PML4[0] is a direct COPY of the kernel's own
   p4_table[0], which boot.asm created with flags PRESENT|WRITABLE only
   (no PAGE_USER) since it was only ever meant for kernel-internal
   low-identity-map use. PML4-level permissions gate everything beneath
   them, so ANY address under index 0 -- the first 512GiB of the address
   space -- is permanently inaccessible to user mode no matter what the
   PDPT/PD/PT entries beneath it say (confirmed via a direct page-table
   walk: the final PTE was correctly PRESENT|WRITABLE|USER with no NX,
   yet execution still faulted, because the copied PML4[0] entry itself
   lacks PAGE_USER). Index 64 is untouched by any shared entry, so
   paging_map_into's table_entry creates it fresh with proper
   PAGE_USER flags via its own default_flags. */
SECTIONS
{
    . = 0x200000000000;

    .text ALIGN(4K) : { *(.text .text.*) }
    .rodata ALIGN(4K) : { *(.rodata .rodata.*) }
    .data ALIGN(4K) : { *(.data .data.*) }
    .bss ALIGN(4K) : { *(COMMON) *(.bss .bss.*) }
}
