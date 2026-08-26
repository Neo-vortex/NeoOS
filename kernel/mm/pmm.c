#include "pmm.h"
#include "serial.h"

#define PMM_MAX_FRAMES ((4ULL * 1024 * 1024 * 1024) / PMM_FRAME_SIZE) // 4GiB cap, see Global Constraints
#define ORDER_NONE 0xFF

extern char kernel_phys_start[];
extern char kernel_phys_end[];

struct free_block {
    struct free_block *next;
    struct free_block *prev;
};

static struct free_block *free_lists[PMM_MAX_ORDER + 1];
// One byte per frame: the order of the free block starting at that frame,
// or ORDER_NONE if this frame isn't a free-block head. Lets pmm_free check
// "is my buddy free" in O(1) instead of walking a free list.
static uint8_t frame_order[PMM_MAX_FRAMES];
static uint64_t total_free_frames;

static inline uint64_t frame_to_phys(uint64_t frame) {
    return frame * PMM_FRAME_SIZE;
}

static inline uint64_t phys_to_frame(uint64_t phys) {
    return phys / PMM_FRAME_SIZE;
}

static void list_push(unsigned order, struct free_block *block) {
    block->prev = 0;
    block->next = free_lists[order];
    if (free_lists[order]) {
        free_lists[order]->prev = block;
    }
    free_lists[order] = block;
    frame_order[phys_to_frame((uint64_t)(uintptr_t)block)] = (uint8_t)order;
}

static void list_remove(unsigned order, struct free_block *block) {
    if (block->prev) {
        block->prev->next = block->next;
    } else {
        free_lists[order] = block->next;
    }
    if (block->next) {
        block->next->prev = block->prev;
    }
    frame_order[phys_to_frame((uint64_t)(uintptr_t)block)] = ORDER_NONE;
}

uint64_t pmm_alloc(unsigned order) {
    if (order > PMM_MAX_ORDER) {
        return 0;
    }

    unsigned found_order = order;
    while (found_order <= PMM_MAX_ORDER && !free_lists[found_order]) {
        found_order++;
    }
    if (found_order > PMM_MAX_ORDER) {
        return 0; // out of memory
    }

    struct free_block *block = free_lists[found_order];
    list_remove(found_order, block);

    // Split the block down to the requested order, pushing each unused
    // buddy half onto its own free list.
    uint64_t phys = (uint64_t)(uintptr_t)block;
    while (found_order > order) {
        found_order--;
        uint64_t buddy_phys = phys + (PMM_FRAME_SIZE << found_order);
        list_push(found_order, (struct free_block *)(uintptr_t)buddy_phys);
    }

    total_free_frames -= (1ULL << order);
    return phys;
}

void pmm_free(uint64_t phys_addr, unsigned order) {
    uint64_t frame = phys_to_frame(phys_addr);
    total_free_frames += (1ULL << order); // caller's block wasn't counted as free before this call

    while (order < PMM_MAX_ORDER) {
        uint64_t buddy_frame = frame ^ (1ULL << order);
        if (buddy_frame >= PMM_MAX_FRAMES || frame_order[buddy_frame] != order) {
            break;
        }
        // Buddy is free at the same order: unlink it and merge upward.
        // Its frames are already counted in total_free_frames from when
        // it was freed, so no further accounting is needed here.
        list_remove(order, (struct free_block *)(uintptr_t)frame_to_phys(buddy_frame));
        frame = (frame < buddy_frame) ? frame : buddy_frame;
        order++;
    }

    list_push(order, (struct free_block *)(uintptr_t)frame_to_phys(frame));
}

uint64_t pmm_free_frame_count(void) {
    return total_free_frames;
}

static void add_region(uint64_t start, uint64_t end) {
    start = (start + PMM_FRAME_SIZE - 1) & ~(uint64_t)(PMM_FRAME_SIZE - 1);
    end = end & ~(uint64_t)(PMM_FRAME_SIZE - 1);

    for (uint64_t phys = start; phys + PMM_FRAME_SIZE <= end; phys += PMM_FRAME_SIZE) {
        if (phys_to_frame(phys) >= PMM_MAX_FRAMES) {
            break;
        }
        pmm_free(phys, 0);
    }
}

struct multiboot_tag {
    uint32_t type;
    uint32_t size;
};

struct multiboot_mmap_entry {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

struct multiboot_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct multiboot_mmap_entry entries[];
};

#define MULTIBOOT_TAG_TYPE_MMAP    6
#define MULTIBOOT_MEMORY_AVAILABLE 1

void pmm_init(void *multiboot_info) {
    for (unsigned i = 0; i <= PMM_MAX_ORDER; i++) {
        free_lists[i] = 0;
    }
    for (uint64_t i = 0; i < PMM_MAX_FRAMES; i++) {
        frame_order[i] = ORDER_NONE;
    }
    total_free_frames = 0;

    uint64_t kernel_start = (uint64_t)(uintptr_t)kernel_phys_start;
    uint64_t kernel_end = (uint64_t)(uintptr_t)kernel_phys_end;

    uint32_t total_size = *(uint32_t *)multiboot_info;
    uint8_t *ptr = (uint8_t *)multiboot_info + 8; // skip total_size + reserved
    uint8_t *end = (uint8_t *)multiboot_info + total_size;

    while (ptr < end) {
        struct multiboot_tag *tag = (struct multiboot_tag *)ptr;
        if (tag->type == 0) {
            break; // end tag
        }

        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
            struct multiboot_tag_mmap *mmap = (struct multiboot_tag_mmap *)tag;
            uint32_t entry_count = (mmap->size - 16) / mmap->entry_size;

            for (uint32_t i = 0; i < entry_count; i++) {
                struct multiboot_mmap_entry *entry =
                    (struct multiboot_mmap_entry *)((uint8_t *)mmap->entries + i * mmap->entry_size);
                if (entry->type != MULTIBOOT_MEMORY_AVAILABLE) {
                    continue;
                }

                uint64_t start = entry->base_addr;
                uint64_t region_end = entry->base_addr + entry->length;
                if (start < 0x100000) {
                    start = 0x100000; // never hand out the real-mode/BIOS/EBDA area
                }

                if (start < kernel_end && region_end > kernel_start) {
                    if (start < kernel_start) {
                        add_region(start, kernel_start);
                    }
                    if (region_end > kernel_end) {
                        add_region(kernel_end, region_end);
                    }
                } else {
                    add_region(start, region_end);
                }
            }
        }

        ptr += (tag->size + 7) & ~7u; // tags are 8-byte aligned
    }

    serial_write_string("[pmm] free_frames=");
    serial_write_hex64(total_free_frames);
    serial_write_string(" (");
    serial_write_hex64(total_free_frames * PMM_FRAME_SIZE / (1024 * 1024));
    serial_write_string(" MiB)\n");
}

void pmm_selftest(void) {
    uint64_t before = total_free_frames;

    uint64_t block = pmm_alloc(3); // 8 frames
    if (!block) {
        serial_write_string("[pmm] selftest FAILED: alloc returned 0\n");
        return;
    }
    if (frame_order[phys_to_frame(block)] != ORDER_NONE) {
        serial_write_string("[pmm] selftest FAILED: allocated block still marked free\n");
        return;
    }

    // Free the two order-2 halves separately -- they are buddies of each
    // other (block is 8-frame-aligned, hence also 4-frame-aligned) and
    // must recombine into at least an order-3 free block. It's legitimate
    // for coalescing to continue even further than order 3 if pmm_alloc(3)
    // itself had to split a larger block to satisfy the request (its
    // other split-off halves are still free neighbors at this point in
    // boot, before anything else has allocated memory) -- so this checks
    // "at least 3", not "exactly 3".
    uint64_t half_size = PMM_FRAME_SIZE << 2;
    pmm_free(block, 2);
    pmm_free(block + half_size, 2);

    if (frame_order[phys_to_frame(block)] < 3 || frame_order[phys_to_frame(block)] == ORDER_NONE) {
        serial_write_string("[pmm] selftest FAILED: buddies did not coalesce back to at least order 3\n");
        return;
    }
    if (total_free_frames != before) {
        serial_write_string("[pmm] selftest FAILED: frame count did not return to baseline\n");
        return;
    }

    serial_write_string("[pmm] selftest passed, free_frames=");
    serial_write_hex64(total_free_frames);
    serial_write_string("\n");
}
