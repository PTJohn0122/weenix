#pragma once

#include "kernel.h"
#include "types.h"

#include "mm/page.h"

/* Invalidates any entries from the TLB which contain
 * mappings for the given virtual address. */
static inline void tlb_flush(uintptr_t vaddr)
{
    __asm__ volatile("invlpg (%0)" ::"r"(vaddr));
}

/* Invalidates any entries for count pages starting at
 * vaddr from the TLB. If this range is very large it may
 * be more efficient to call tlb_flush_all to invalidate
 * the entire TLB. */
static inline void tlb_flush_range(uintptr_t vaddr, size_t count)
{
    for (size_t i = 0; i < count; i++, vaddr += PAGE_SIZE)
    {
        tlb_flush(vaddr);
    }
}

/* Invalidates the entire TLB. */
static inline void tlb_flush_all()
{
    uintptr_t pdir;
    __asm__ volatile("movq %%cr3, %0"
                     : "=r"(pdir));
    __asm__ volatile("movq %0, %%cr3" ::"r"(pdir)
                     : "memory");
}
