#include "errno.h"
#include "globals.h"
#include "mm/mm.h"
#include "util/debug.h"

#include "mm/mman.h"

/*
 * This function implements the brk(2) system call.
 *
 * This routine manages the calling process's "break" -- the ending address
 * of the process's dynamic region (heap)
 *
 * Some important details on the range of values 'p_brk' can take:
 * 1) 'p_brk' should not be set to a value lower than 'p_start_brk', since this
 *    could overrite data in another memory region. But, 'p_brk' can be equal to
 *    'p_start_brk', which would mean that there is no heap yet/is empty.
 * 2) Growth of the 'p_brk' cannot overlap with/expand into an existing
 *    mapping. Use vmmap_is_range_empty() to help with this.
 * 3) 'p_brk' cannot go beyond the region of the address space allocated for use by
 *    userland (USER_MEM_HIGH)
 *
 * Before setting 'p_brk' to 'addr', you must account for all scenarios by comparing
 * the page numbers of addr, p_brk and p_start_brk as the vmarea that represents the heap
 * has page granularity. Think about the following sub-cases (note that the heap 
 * should always be represented by at most one vmarea):
 * 1) The heap needs to be created. What permissions and attributes does a process
 *    expect the heap to have?
 * 2) The heap already exists, so you need to modify its end appropriately.
 * 3) The heap needs to shrink.
 *
 * Beware of page alignment!:
 * 1) The starting break is not necessarily page aligned. Since the loader sets
 *    'p_start_brk' to be the end of the bss section, 'p_start_brk' should always be
 *    aligned up to start the dynamic region at the first page after bss_end.
 * 2) vmareas only have page granularity, so you will need to take this
 *    into account when deciding how to set the mappings if p_brk or p_start_brk
 *    is not page aligned. The caller of do_brk() would be very disappointed if
 *    you give them less than they asked for!
 *
 * Some additional details:
 * 1) You are guaranteed that the process data/bss region is non-empty.
 *    That is, if the starting brk is not page-aligned, its page has
 *    read/write permissions.
 * 2) If 'addr' is NULL, you should return the current break. We use this to
 *    implement sbrk(0) without writing a separate syscall. Look in
 *    user/libc/syscall.c if you're curious.
 * 3) Return 0 on success, -errno on failure through the 'ret' argument.
 *
 * In do_brk, the stencil comments say, "3) Return 0 on success, -errno 
 * on failure through the ret argument." There is no need to set ret; you
 *  just need to return the error. The ret argument instead should be used 
 * to return the updated p_brk on success. 
 * Error cases do_brk is responsible for generating:
 *  - ENOMEM: attempting to set p_brk beyond its valid range
 */
long do_brk(void *addr, void **ret)
{
    if(addr==NULL){
        *ret=curproc->p_brk;
        return 0;
    }
    // If addr is not in valid range 
    if((size_t)addr>USER_MEM_HIGH||addr<curproc->p_start_brk){
        return -ENOMEM;
    }

    if(ADDR_TO_PN(PAGE_ALIGN_UP(curproc->p_brk))==ADDR_TO_PN(PAGE_ALIGN_UP(addr))){
        // We don't need to do anything here
        curproc->p_brk=addr;
    }else if(ADDR_TO_PN(PAGE_ALIGN_UP(curproc->p_brk))>ADDR_TO_PN(PAGE_ALIGN_UP(addr))){
        size_t lopage=ADDR_TO_PN(PAGE_ALIGN_UP(addr));
        size_t npages=ADDR_TO_PN(PAGE_ALIGN_UP(curproc->p_brk))-ADDR_TO_PN(PAGE_ALIGN_UP(addr));
        vmmap_remove(curproc->p_vmmap,lopage,npages);    // Clean up the specified range
        curproc->p_brk=addr; // Update the new end of the heap
    }else{
        size_t start_pn=ADDR_TO_PN(PAGE_ALIGN_UP(curproc->p_start_brk));
        size_t brk_pn=ADDR_TO_PN(PAGE_ALIGN_UP(curproc->p_brk));
        size_t add_pn=ADDR_TO_PN(PAGE_ALIGN_UP(addr));
        vmarea_t *new_vm=vmmap_lookup(curproc->p_vmmap,start_pn);
        // If we don't find it, we need to create a new vmarea
        if(new_vm==NULL){
            long tmp=vmmap_map(curproc->p_vmmap,NULL,start_pn,add_pn-start_pn,PROT_READ|PROT_WRITE, 
                MAP_PRIVATE|MAP_ANON | MAP_FIXED,0,VMMAP_DIR_HILO,&new_vm);
        
            if(tmp<0){
                return tmp;
            }
        }else{
            // We just need to expand the vmarea
            if(!vmmap_is_range_empty(curproc->p_vmmap,brk_pn,add_pn-brk_pn)){
                return -ENOMEM;     // Beyond its valid range
            }
            new_vm->vma_end=add_pn; // Expand it
        }
        curproc->p_brk=addr;
    }
    *ret=curproc->p_brk;
    // NOT_YET_IMPLEMENTED("VM: do_brk");
    return 0;
    
}
