/*
 * Physical memory manager: one bit per 4 KiB page (1 = used, 0 = free).
 * Only memory Limine reports as USABLE is ever handed out. The bitmap itself
 * is carved out of the first usable region large enough to hold it.
 */
#include "kernel.h"
#include "limine.h"
#include "pmm.h"

uint64_t hhdm_offset;

static uint8_t *bitmap;
static uint64_t total_pages;     /* pages covered by the bitmap (0 .. highest usable) */
static uint64_t managed_pages;   /* pages that were usable at boot */
static uint64_t free_pages;
static uint64_t hint;

static inline bool page_used(uint64_t p) { return bitmap[p >> 3] & (1u << (p & 7)); }

static inline void mark_used(uint64_t p) {
    if (!page_used(p)) { bitmap[p >> 3] |= (1u << (p & 7)); free_pages--; }
}
static inline void mark_free(uint64_t p) {
    if (page_used(p)) { bitmap[p >> 3] &= ~(1u << (p & 7)); free_pages++; }
}

void pmm_init(struct limine_memmap_response *mm, uint64_t hhdm) {
    hhdm_offset = hhdm;

    uint64_t top = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->base + e->length > top)
            top = e->base + e->length;
    }
    total_pages = top / PAGE_SIZE;
    uint64_t bitmap_bytes = (total_pages + 7) / 8;
    uint64_t bitmap_pages = (bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t bitmap_phys = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= bitmap_pages * PAGE_SIZE
            && e->base >= 0x100000) {            /* keep clear of low memory */
            bitmap_phys = e->base;
            break;
        }
    }
    if (!bitmap_phys) kpanic("PMM: no usable region large enough for the page bitmap");

    bitmap = phys_to_virt(bitmap_phys);
    memset(bitmap, 0xFF, bitmap_bytes);          /* everything used until proven free */
    free_pages = 0;

    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        for (uint64_t p = e->base / PAGE_SIZE; p < (e->base + e->length) / PAGE_SIZE; p++)
            mark_free(p);
    }

    mark_used(0);                                /* never hand out physical page 0 */
    for (uint64_t p = 0; p < bitmap_pages; p++)  /* protect the bitmap itself */
        mark_used(bitmap_phys / PAGE_SIZE + p);

    managed_pages = free_pages;
    hint = 0;
}

static uint64_t scan(uint64_t start, uint64_t end) {
    for (uint64_t p = start; p < end; ) {
        if ((p & 7) == 0 && p + 8 <= end && bitmap[p >> 3] == 0xFF) { p += 8; continue; }
        if (!page_used(p)) { mark_used(p); hint = p + 1; return p * PAGE_SIZE; }
        p++;
    }
    return 0;
}

uint64_t pmm_alloc(void) {
    uint64_t r = scan(hint, total_pages);
    return r ? r : scan(0, hint);
}

uint64_t pmm_alloc_zeroed(void) {
    uint64_t p = pmm_alloc();
    if (p) memset(phys_to_virt(p), 0, PAGE_SIZE);
    return p;
}

uint64_t pmm_alloc_contiguous(uint64_t count) {
    if (!count) return 0;
    uint64_t run = 0;
    for (uint64_t p = 0x100; p < total_pages; p++) {       /* start at 1 MiB */
        if ((p & 7) == 0 && bitmap[p >> 3] == 0xFF) { p += 7; run = 0; continue; }
        if (page_used(p)) { run = 0; continue; }
        if (++run == count) {
            uint64_t start = p + 1 - count;
            for (uint64_t i = 0; i < count; i++) mark_used(start + i);
            return start * PAGE_SIZE;
        }
    }
    return 0;
}

void pmm_free(uint64_t phys) {
    uint64_t p = phys / PAGE_SIZE;
    if ((phys & (PAGE_SIZE - 1)) || p == 0 || p >= total_pages)
        kpanic("PMM: pmm_free of invalid address %016lx", phys);
    if (!page_used(p))
        kpanic("PMM: double free of page %016lx", phys);
    mark_free(p);
    if (p < hint) hint = p;
}

uint64_t pmm_total_bytes(void) { return managed_pages * PAGE_SIZE; }
uint64_t pmm_free_bytes(void)  { return free_pages * PAGE_SIZE; }
