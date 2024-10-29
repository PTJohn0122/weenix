#include "globals.h"
#include "kernel.h"
#include <errno.h>

#include "vm/anon.h"
#include "vm/shadow.h"

#include "util/debug.h"
#include "util/printf.h"
#include "util/string.h"

#include "fs/file.h"
#include "fs/vfs_syscall.h"
#include "fs/vnode.h"

#include "mm/mm.h"
#include "mm/mman.h"
#include "mm/slab.h"
#include "mm/tlb.h"

static slab_allocator_t *vmmap_allocator;
static slab_allocator_t *vmarea_allocator;

void vmmap_init(void)
{
    vmmap_allocator = slab_allocator_create("vmmap", sizeof(vmmap_t));
    vmarea_allocator = slab_allocator_create("vmarea", sizeof(vmarea_t));
    KASSERT(vmmap_allocator && vmarea_allocator);
}

/*
 * Allocate and initialize a new vmarea using vmarea_allocator.
 */
vmarea_t *vmarea_alloc(void)
{
    vmarea_t *new_vmarea=(vmarea_t *)slab_obj_alloc(vmarea_allocator);

    if(new_vmarea==NULL)    {
        return NULL;
    }
    // Initialize it, need to come back
    memset(new_vmarea, 0, sizeof(vmarea_t));

    //NOT_YET_IMPLEMENTED("VM: vmarea_alloc");
    
    return new_vmarea;
}

/*
 * Free the vmarea by removing it from any lists it may be on, putting its
 * vma_obj if it exists, and freeing the vmarea_t.
 */
void vmarea_free(vmarea_t *vma)
{   
    // dbg(DBG_VM, " In vmarea_free, the freeed vmarea is %p",vma);
    if(list_link_is_linked(&vma->vma_plink)){
        list_remove(&vma->vma_plink); // Remove it from lists
    }   
    if(vma->vma_obj){
        mobj_put(&vma->vma_obj); // Put memory object
    }

    slab_obj_free(vmarea_allocator,vma); // Free the vmarea_t
    // NOT_YET_IMPLEMENTED("VM: vmarea_free");
}

/*
 * Create and initialize a new vmmap. Initialize all the fields of vmmap_t.
 */
vmmap_t *vmmap_create(void)
{
    // dbg(DBG_VM, " In vmmap_create \n");
    vmmap_t *new_vmmap=slab_obj_alloc(vmmap_allocator);

    if(new_vmmap==NULL) {return NULL;}

    list_init(&new_vmmap->vmm_list);
    new_vmmap->vmm_proc=NULL;
    // NOT_YET_IMPLEMENTED("VM: vmmap_create");
    return new_vmmap;
}

/*
 * Destroy the map pointed to by mapp and set *mapp = NULL.
 * Remember to free each vma in the maps list.
 */
void vmmap_destroy(vmmap_t **mapp)
{
    // dbg(DBG_VM, " In vmmap_destroy \n");
    // vmarea_t *cur_vmarea;
    list_iterate(&(*mapp)->vmm_list,cur_vmarea,vmarea_t,vma_plink){
        vmarea_free(cur_vmarea);    // Free each vma in the list
    }
    slab_obj_free(vmmap_allocator,*mapp);   // Free vmmap struct
    *mapp=NULL;
    // NOT_YET_IMPLEMENTED("VM: vmmap_destroy");
}

/*
 * Add a vmarea to an address space. Assumes (i.e. asserts to some extent) the
 * vmarea is valid. Iterate through the list of vmareas, and add it 
 * accordingly. 
 */
void vmmap_insert(vmmap_t *map, vmarea_t *new_vma)
{
    // dbg(DBG_VM, " In vmmap_insert \n");
    KASSERT(new_vma->vma_end>=new_vma->vma_start&&"Make sure the start cannot be greater than end");
    // KASSERT(list_link_is_linked(&new_vma->vma_plink) && "Make sure the link list is valid");
    //  KASSERT((new_vma->vma_flags&MAP_SHARED)||((new_vma->vma_flags&MAP_PRIVATE)&&
    // "Make sure either MAP_SHARED and MAP_PRIVATE is set"));

    size_t start_pn=ADDR_TO_PN(USER_MEM_LOW);
    size_t end_pn=0;

    vmarea_t *cur_vmarea;
    // vmarea_t *pre_vmarea;
    list_iterate(&map->vmm_list,cur_vmarea,vmarea_t,vma_plink){
        // If we found we can insert it into one vmarea
        end_pn=cur_vmarea->vma_start;
        // If new vmarea can be inserted in front of this cur_vmarea
        if(new_vma->vma_start>=start_pn&&new_vma->vma_end<=end_pn){
            list_insert_before(&cur_vmarea->vma_plink,&new_vma->vma_plink);
            new_vma->vma_vmmap=map; // Update it's corresponding vmmap
            return;
        }
        start_pn=cur_vmarea->vma_end;
        //pre_vmarea=cur_vmarea;
    }

    // If we cannot find an appropriate place, we may insert it in the tail
    list_insert_tail(&map->vmm_list,&new_vma->vma_plink);
    new_vma->vma_vmmap=map;
    // NOT_YET_IMPLEMENTED("VM: vmmap_insert");
}

/*
 * Find a contiguous range of free virtual pages of length npages in the given
 * address space. Returns starting page number for the range, without altering the map.
 * Return -1 if no such range exists.
 *
 * Your algorithm should be first fit. If dir is
 *    - VMMAP_DIR_HILO: a gap as high in the address space as possible, starting 
 *                      from USER_MEM_HIGH.  
 *    - VMMAP_DIR_LOHI: a gap as low in the address space as possible, starting 
 *                      from USER_MEM_LOW. 
 * 
 * Make sure you are converting between page numbers and addresses correctly! 
 */
ssize_t vmmap_find_range(vmmap_t *map, size_t npages, int dir)
{
    // dbg(DBG_VM, "vmmap_find_range,the current map is %p, page size is %ld, the Mode is %d \n",map,npages,dir);
    // KASSERT(dir==VMMAP_DIR_HILO||dir==VMMAP_DIR_LOHI);
    // Check the begining
    if(dir==VMMAP_DIR_LOHI){    // From low to high        
        size_t start_pn=ADDR_TO_PN(USER_MEM_LOW);
        size_t end_pn=0;
        if(list_empty(&map->vmm_list)&&start_pn+npages<=ADDR_TO_PN(USER_MEM_HIGH)){
            return start_pn;
        }
        
        list_iterate(&map->vmm_list,cur_vmarea,vmarea_t,vma_plink){
            end_pn=cur_vmarea->vma_start;
            if(end_pn-start_pn>=npages){
                return start_pn;
            }
            start_pn=cur_vmarea->vma_end;
        }

        // If we cannot find a range in the map list, check the last address space
        end_pn=ADDR_TO_PN(USER_MEM_HIGH);
        if(end_pn-start_pn>=npages){
            return start_pn;
        }
    }  else{    // From high to low
        size_t start_pn=ADDR_TO_PN(USER_MEM_HIGH);
        size_t end_pn=0;
        if(list_empty(&map->vmm_list)&&start_pn>=ADDR_TO_PN(USER_MEM_LOW)+npages){
            return (start_pn-npages);   // As high as possible
        }
        
        list_iterate_reverse(&map->vmm_list,cur_vmarea,vmarea_t,vma_plink){
            end_pn=cur_vmarea->vma_end;
            if(start_pn-end_pn>=npages){
                return start_pn-npages;
            }
            start_pn=cur_vmarea->vma_start;
        }

        // If we still cannot find one available page
        // Check the first vmarea
        end_pn=ADDR_TO_PN(USER_MEM_LOW);
        if(start_pn-end_pn>=npages){
            return (start_pn-npages);
        }
    }
    // NOT_YET_IMPLEMENTED("VM: vmmap_find_range");
    return -1;
}

/*
 * Return the vm_area that vfn (a page number) lies in. Scan the address space looking
 * for a vma whose range covers vfn. If the page is unmapped, return NULL.
 */
vmarea_t *vmmap_lookup(vmmap_t *map, size_t vfn)
{
    // dbg(DBG_VM,"vmmap_lookup, the current map is %p, page number is %ld \n",map,vfn);
    vmarea_t *cur_vmarea;
    list_iterate(&map->vmm_list,cur_vmarea,vmarea_t,vma_plink){
        if(cur_vmarea->vma_start<=vfn&&cur_vmarea->vma_end>vfn){
            return cur_vmarea;
        }
    }
    // NOT_YET_IMPLEMENTED("VM: vmmap_lookup");
    return NULL;
}

/*
 * For each vmarea in the map, if it is a shadow object, call shadow_collapse.
 */
void vmmap_collapse(vmmap_t *map)
{
    // dbg(DBG_VM,"vmmap_collapse, the current map is %p \n",map);
    list_iterate(&map->vmm_list, vma, vmarea_t, vma_plink)
    {
        if (vma->vma_obj->mo_type == MOBJ_SHADOW)
        {
            mobj_lock(vma->vma_obj);
            shadow_collapse(vma->vma_obj);
            mobj_unlock(vma->vma_obj);
        }
    }
}

/*
 * This is where the magic of fork's copy-on-write gets set up. 
 * 
 * Upon successful return, the new vmmap should be a clone of map with all 
 * shadow objects properly set up.
 *
 * For each vmarea, clone it's members. 
 *  1) vmarea is share-mapped, you don't need to do anything special. 
 *  2) vmarea is not share-mapped, time for shadow objects: 
 *     a) Create two shadow objects, one for map and one for the new vmmap you
 *        are constructing, both of which shadow the current vma_obj the vmarea
 *        being cloned. 
 *     b) After creating the shadow objects, put the original vma_obj
 *     c) and insert the shadow objects into their respective vma's.
 *
 * Be sure to clean up in any error case, manage the reference counts correctly,
 * and to lock/unlock properly.
 */
vmmap_t *vmmap_clone(vmmap_t *map)
{
    // dbg(DBG_VM,"vmmap_clone, the current map is %p \n",map);
    // vmmap_collapse(map);

    vmmap_t *new_map=vmmap_create();
    if(new_map== NULL){
        return NULL;
    }
    // vmarea_t *cur_vmarea;
    list_iterate(&map->vmm_list,cur_vmarea,vmarea_t,vma_plink){
        vmarea_t *new_vmarea=vmarea_alloc();
        if(new_vmarea==NULL){
            return NULL;
        }
        // Copy the new_vmarea
        new_vmarea->vma_start=cur_vmarea->vma_start;
        new_vmarea->vma_end=cur_vmarea->vma_end;
        new_vmarea->vma_flags=cur_vmarea->vma_flags;
        new_vmarea->vma_off=cur_vmarea->vma_off;
        new_vmarea->vma_prot=cur_vmarea->vma_prot;
        new_vmarea->vma_vmmap=new_map;
        new_vmarea->vma_obj=cur_vmarea->vma_obj;
        mobj_ref(new_vmarea->vma_obj);
        vmmap_insert(new_map,new_vmarea);

        if(!(cur_vmarea->vma_flags&MAP_SHARED)){
             mobj_t* sha_map=shadow_create(cur_vmarea->vma_obj);
             mobj_t* sha_newmap=shadow_create(new_vmarea->vma_obj);
             mobj_put(&cur_vmarea->vma_obj);
             mobj_put(&new_vmarea->vma_obj);
             // Insert into vmarea 
             cur_vmarea->vma_obj=sha_map;
             mobj_unlock(sha_map);
             
             new_vmarea->vma_obj=sha_newmap;   
             mobj_unlock(sha_newmap);
        }
        // list_insert_tail(&new_map->vmm_list,&new_vmarea->vma_plink);
    }

    // NOT_YET_IMPLEMENTED("VM: vmmap_clone");
    return new_map;
}

/*
 *
 * Insert a mapping into the map starting at lopage for npages pages.
 * 
 *  file    - If provided, the vnode of the file to be mapped in
 *  lopage  - If provided, the desired start range of the mapping
 *  prot    - See mman.h for possible values
 *  flags   - See do_mmap()'s comments for possible values
 *  off     - Offset in the file to start mapping at, in bytes
 *  dir     - VMMAP_DIR_LOHI or VMMAP_DIR_HILO
 *  new_vma - If provided, on success, must point to the new vmarea_t
 * 
 *  Return 0 on success, or:
 *  - ENOMEM: On vmarea_alloc, annon_create, shadow_create or 
 *    vmmap_find_range failure 
 *  - Propagate errors from file->vn_ops->mmap and vmmap_remove
 * 
 * Hints:
 *  - You can assume/assert that all input is valid. It may help to write
 *    this function and do_mmap() somewhat in tandem.
 *  - If file is NULL, create an anon object.
 *  - If file is non-NULL, use the vnode's mmap operation to get the mobj.
 *    Do not assume it is file->vn_obj (mostly relevant for special devices).
 *  - If lopage is 0, use vmmap_find_range() to get a valid range
 *  - If lopage is nonzero and MAP_FIXED is specified and 
 *    the given range overlaps with any preexisting mappings, 
 *    remove the preexisting mappings.
 *  - If MAP_PRIVATE is specified, set up a shadow object. Be careful with
 *    refcounts!
 *  - Be careful: off is in bytes (albeit should be page-aligned), but
 *    vma->vma_off is in pages.
 *  - Be careful with the order of operations. Hold off on any irreversible
 *    work until there is no more chance of failure.
 */
long vmmap_map(vmmap_t *map, vnode_t *file, size_t lopage, size_t npages,
               int prot, int flags, off_t off, int dir, vmarea_t **new_vma)
{
    dbg(DBG_VM,"vmmap_map, the current map is %p \n",map);
    // Assert all the input is valid
    KASSERT(map!=NULL&&"map should not be NULL");
    KASSERT(prot==PROT_NONE||prot&PROT_READ|| prot&PROT_WRITE||prot&PROT_EXEC);
    KASSERT((flags&MAP_SHARED)||(flags&MAP_PRIVATE));
    // KASSERT(dir==VMMAP_DIR_LOHI||dir==VMMAP_DIR_HILO);

    mobj_t *new_mobj;
    // Get the new memory object here
    if(file==NULL){ // If file is NULL
        new_mobj=anon_create();     // Create new memory object
        if(new_mobj==NULL){
            return -ENOMEM;
        }
        mobj_unlock(new_mobj);
    } else{
        long tmp=file->vn_ops->mmap(file,&new_mobj);    // Obtain the memory object
        if(tmp<0){
            return tmp;
        }
    }

    ssize_t start_pagenum=lopage;   // Get the start range and mapping
    if(lopage==0){
        start_pagenum =vmmap_find_range(map,npages,dir); // Get a new range
        if(start_pagenum<0){    // Error checking
            return -ENOMEM;
        }
    } else if(lopage!=0&&(flags&MAP_FIXED)&&!vmmap_is_range_empty(map,start_pagenum,npages)){
        // Remove the prexisiting mappings if there are any overlaps
        long tmp=vmmap_remove(map,start_pagenum,npages);   // 
        if(tmp<0){
            return tmp;
        }
    }

    if(flags&MAP_PRIVATE){
        mobj_t *sha_obj=shadow_create(new_mobj);    // Will be locked here
        
        if(sha_obj==NULL){          
            return -ENOMEM;
        }

        mobj_t *old_mobj=new_mobj;  // Store the previous mobj and unlock it

        new_mobj=sha_obj;   // It has been locked in shadow_create
        // mobj_ref(sha_obj);  // Ref the shadow obj

        mobj_put(&old_mobj);  // Put the previous mobj

        mobj_unlock(sha_obj);   // Unlock the created shadow object
    }

    // Initialize the new vmarea_t
    vmarea_t *new=vmarea_alloc();
    if(new==NULL){
        return -ENOMEM;
    }
    new->vma_start=start_pagenum;
    new->vma_end=start_pagenum+npages;
    new->vma_flags=flags;
    new->vma_prot=prot;
    new->vma_vmmap=map;
    new->vma_obj=new_mobj;
    // mobj_ref(new_mobj);

    new->vma_off=ADDR_TO_PN(off);
    
    vmmap_insert(map,new);
    if(new_vma!=NULL){
        *new_vma=new;
    }

    // NOT_YET_IMPLEMENTED("VM: vmmap_map");
    return 0;
}

/*
 * Iterate over the mapping's vmm_list and make sure that the specified range
 * is completely empty. You will have to handle the following cases:
 *
 * Key:     [             ] = existing vmarea_t
 *              *******     = region to be unmapped
 *
 * Case 1:  [   *******   ]
 * The region to be unmapped lies completely inside the vmarea. We need to
 * split the old vmarea into two vmareas. Be sure to increment the refcount of
 * the object associated with the vmarea.
 *
 * Case 2:  [      *******]**
 * The region overlaps the end of the vmarea. Just shorten the length of
 * the mapping.
 *
 * Case 3: *[*****        ]
 * The region overlaps the beginning of the vmarea. Move the beginning of
 * the mapping (remember to update vma_off), and shorten its length.
 *
 * Case 4: *[*************]**
 * The region completely contains the vmarea. Remove the vmarea from the
 * list.
 * 
 * Return 0 on success, or:
 *  - ENOMEM: Failed to allocate a new vmarea when splitting a vmarea (case 1).
 * 
 * Hints:
 *  - Whenever you shorten/remove any mappings, be sure to call pt_unmap_range()
 *    tlb_flush_range() to clean your pagetables and TLB.
 */
long vmmap_remove(vmmap_t *map, size_t lopage, size_t npages)
{
    // dbg(DBG_VM,"vmmap_remove, the current map is %p \n",map);
    if(npages==0){
        return 0;   // We don't need to remove
    }

    vmarea_t *cur_vmarea;
    int case_type=0;    // Consider the listed case type
    size_t end_page=lopage+npages;
    
    // TODO: Do need to clean TLB and pagetables when there are no mappings
    list_iterate(&map->vmm_list,cur_vmarea,vmarea_t,vma_plink){
        if(cur_vmarea->vma_start<lopage&&cur_vmarea->vma_end>end_page){   // Case 1
            vmarea_t *new_vmarea=vmarea_alloc();
            if(new_vmarea==NULL){
                return -ENOMEM;
            }
            // Update the start, end and off, and initalize it
            new_vmarea->vma_start=end_page;
            new_vmarea->vma_end=cur_vmarea->vma_end;
            new_vmarea->vma_off=cur_vmarea->vma_off+new_vmarea->vma_start-cur_vmarea->vma_start;
            new_vmarea->vma_flags=cur_vmarea->vma_flags;
            new_vmarea->vma_prot=cur_vmarea->vma_prot;
            new_vmarea->vma_obj=cur_vmarea->vma_obj;
            if(cur_vmarea->vma_obj!=NULL){
                mobj_ref(cur_vmarea->vma_obj);  // Increase the refcount of this mobj
            }

            cur_vmarea->vma_end=lopage; // Set the new end of current vmarea, so that we can split the previous vmarea
            if(map->vmm_proc){
                pt_unmap_range(map->vmm_proc->p_pml4,(uintptr_t)PN_TO_ADDR(lopage),(uintptr_t)PN_TO_ADDR(lopage+npages));
            }
            tlb_flush_range((uintptr_t)PN_TO_ADDR(lopage),npages);

            vmmap_insert(map,new_vmarea);  // Insert it into the map list 
        } else if(cur_vmarea->vma_end>lopage&&cur_vmarea->vma_end<=end_page&&cur_vmarea->vma_start<lopage){  // Case 2
            uintptr_t vmax=(uintptr_t)PN_TO_ADDR(cur_vmarea->vma_end);
            size_t range=cur_vmarea->vma_end-lopage;
            cur_vmarea->vma_end=lopage; // Cut the size of vmarea

            if(map->vmm_proc){
                pt_unmap_range(map->vmm_proc->p_pml4,(uintptr_t)PN_TO_ADDR(lopage),vmax);
            }
            tlb_flush_range((uintptr_t)PN_TO_ADDR(lopage),range);
        } else if(cur_vmarea->vma_end>end_page&&cur_vmarea->vma_start>=lopage&&cur_vmarea->vma_start<end_page) { // Case 3
            uintptr_t vmax=(uintptr_t)PN_TO_ADDR(lopage+npages);
            size_t range=lopage+npages-cur_vmarea->vma_start;

            size_t old_start=cur_vmarea->vma_start;
            cur_vmarea->vma_off=cur_vmarea->vma_off+end_page-cur_vmarea->vma_start;
            cur_vmarea->vma_start=end_page;

            if(map->vmm_proc){
                pt_unmap_range(map->vmm_proc->p_pml4,(uintptr_t)PN_TO_ADDR(old_start),vmax);
            }
            tlb_flush_range((uintptr_t)PN_TO_ADDR(old_start),range);
        } else if(cur_vmarea->vma_start>=lopage&&cur_vmarea->vma_end<=end_page){    // Case 4
            uintptr_t vmax=(uintptr_t)PN_TO_ADDR(cur_vmarea->vma_end);
            size_t range=cur_vmarea->vma_end-cur_vmarea->vma_start;

            if(map->vmm_proc){
                pt_unmap_range(map->vmm_proc->p_pml4,(uintptr_t)PN_TO_ADDR(cur_vmarea->vma_start),vmax);
            }
            vmarea_free(cur_vmarea);
            tlb_flush_range((uintptr_t)PN_TO_ADDR(cur_vmarea->vma_start),range);
        }
    }

    // NOT_YET_IMPLEMENTED("VM: vmmap_remove");
    return 0;
}

/*
 * Returns 1 if the given address space has no mappings for the given range,
 * 0 otherwise.
 */
long vmmap_is_range_empty(vmmap_t *map, size_t startvfn, size_t npages)
{
    // dbg(DBG_VM,"vmmap_is_range_empty, the current map is %p \n",map);
    if(npages==0){  // If there are no address space
        return 1;
    }
    vmarea_t *cur_vmarea;
    size_t endvfn=startvfn+npages; // Not inclusive
    list_iterate(&map->vmm_list,cur_vmarea,vmarea_t,vma_plink){
        if((startvfn<cur_vmarea->vma_start&&endvfn>cur_vmarea->vma_end)||
        (startvfn>=cur_vmarea->vma_start&&startvfn<cur_vmarea->vma_end)||
        (endvfn>cur_vmarea->vma_start&&endvfn<=cur_vmarea->vma_end)
        ){
        // There are 3 cases that the given address space is not empty
        // Start is inside of vmarea, end is inside of vmarea and vmarea is contained between start and end
            return 0;   
        }
    }
    // NOT_YET_IMPLEMENTED("VM: vmmap_is_range_empty");
    return 1;
}

/*
 * Read into 'buf' from the virtual address space of 'map'. Start at 'vaddr'
 * for size 'count'. 'vaddr' is not necessarily page-aligned. count is in bytes.
 * 
 * Hints:
 *  1) Find the vmareas that correspond to the region to read from.
 *  2) Find the pframes within those vmareas corresponding to the virtual 
 *     addresses you want to read.
 *  3) Read from those page frames and copy it into `buf`.
 *  4) You will not need to check the permissisons of the area.
 *  5) You may assume/assert that all areas exist.
 * 
 * Return 0 on success, -errno on error (propagate from the routines called).
 * This routine will be used within copy_from_user(). 
 */
long vmmap_read(vmmap_t *map, const void *vaddr, void *buf, size_t count)
{
    //dbg(DBG_VM,"vmmap_read, the current map is %p \n",map);
    KASSERT(map!=NULL&&"Assume map is not NULL");
    KASSERT(vaddr!=NULL&&"Should be a valid address");
    KASSERT(buf!=NULL&&"Should be a valid buf");

    if(count==0){
        return 0;
    }

    // size_t cur_read_bytes=0;  // Current reading bytes
    size_t cur_vaddr=(size_t)vaddr; // Initialize current address
    size_t end_vaddr=(size_t)vaddr+count;
    uintptr_t cur_page=ADDR_TO_PN(cur_vaddr); // The start reading page
    uintptr_t end_page=ADDR_TO_PN(PAGE_ALIGN_UP(cur_vaddr+count));

    while(cur_page<end_page){       
        vmarea_t *vma=vmmap_lookup(map,cur_page);
        if(vma==NULL){
            return -1;
        }
        // The offset relative to the mobj
        size_t cur_off=vma->vma_off+cur_page-vma->vma_start;

        pframe_t *pf;
        // Get the required page frame
        mobj_lock(vma->vma_obj);
        long tmp=mobj_get_pframe(vma->vma_obj,cur_off,0,&pf);
        mobj_unlock(vma->vma_obj);

        size_t this_page_read_bytes=0;
        // If the data we need to read didn't reach the end of the page
        if((cur_page+1)*PAGE_SIZE-cur_vaddr<end_vaddr-cur_vaddr){
            this_page_read_bytes=(cur_page+1)*PAGE_SIZE-cur_vaddr;
        }  else{
            this_page_read_bytes=end_vaddr-cur_vaddr;
        }
        
        // Copy the data this page into buf
        memcpy((char *)buf+cur_vaddr-(size_t)vaddr,(char *)pf->pf_addr+cur_vaddr-(size_t)PAGE_ALIGN_DOWN(cur_vaddr),
            this_page_read_bytes);
        
        // (char *)buf=(char *)buf+this_page_read_bytes;
        cur_vaddr=cur_vaddr+this_page_read_bytes;
        cur_page++;
        pframe_release(&pf);
    }

    return 0;
    // while(cur_read_bytes<count){
    //     uintptr_t start_page=ADDR_TO_PN(cur_vaddr); // The start writing page

    //     vmarea_t *vma=vmmap_lookup(map,start_page);
    //     if(vma==NULL){
    //         return -1;
    //     }
    //     // The offset relative to the mobj
    //     size_t cur_off=vma->vma_off+start_page-vma->vma_start;
    //     size_t needed_pagenum=0;

    //     // If the needed page number is greater than this vmarea have
    //     if(vma->vma_end-cur_off<ADDR_TO_PN(PAGE_ALIGN_UP(count-cur_read_bytes))){
    //         needed_pagenum=vma->vma_end-cur_off;
    //     }else{
    //     // We need to make sure we have enough page
    //         needed_pagenum=ADDR_TO_PN(PAGE_ALIGN_UP(count-cur_read_bytes));
    //     }

    //     for(size_t i=0;i<needed_pagenum;i++){
    //         pframe_t *pf;
    //         // Get the required page frame
    //         mobj_lock(vma->vma_obj);
    //         long tmp=mobj_get_pframe(vma->vma_obj,cur_off+i,0,&pf);
    //         mobj_unlock(vma->vma_obj);

    //         size_t page_offset=cur_vaddr%PAGE_SIZE;  // Get the start position in this page
    //         size_t this_page_read_bytes=0;

    //         // If the data we need to read didn't reach the end of the page
    //         if(PAGE_SIZE-page_offset<count-cur_read_bytes){
    //             this_page_read_bytes=PAGE_SIZE-page_offset;
    //         }  else{
    //             this_page_read_bytes=count-cur_read_bytes;
    //         }

    //         // Copy the data this page into buf
    //         memcpy((char *)buf+cur_read_bytes,(char *)pf->pf_addr+page_offset,this_page_read_bytes);

    //         // pf->pf_dirty=1; // Mark the page frame as dirtied
    //         cur_read_bytes+=this_page_read_bytes;
    //         cur_vaddr=cur_vaddr+this_page_read_bytes;

    //         pframe_release(&pf);
    //     }
    // }
    // return 0;

}

/*
 * Write from 'buf' into the virtual address space of 'map' starting at
 * 'vaddr' for size 'count'.
 * 
 * Hints:
 *  1) Find the vmareas to write to.
 *  2) Find the correct pframes within those areas that contain the virtual addresses
 *     that you want to write data to.
 *  3) Write to the pframes, copying data from buf.
 *  4) You do not need check permissions of the areas you use.
 *  5) Assume/assert that all areas exist.
 *  6) Remember to dirty the pages that you write to. 
 * 
 * Returns 0 on success, -errno on error (propagate from the routines called).
 * This routine will be used within copy_to_user(). 
 */
long vmmap_write(vmmap_t *map, void *vaddr, const void *buf, size_t count)
{
    // dbg(DBG_VM,"vmmap_write, the current map is %p \n",map);
    KASSERT(map!=NULL&&"Assume map is not NULL");
    KASSERT(vaddr!=NULL&&"Should be a valid address");
    KASSERT(buf!=NULL&&"Should be a valid buf");

    if(count==0){
        return 0;
    }

    size_t cur_vaddr=(size_t)vaddr; // Initialize current address
    size_t end_vaddr=(size_t)vaddr+count;
    uintptr_t cur_page=ADDR_TO_PN(cur_vaddr); // The start reading page
    uintptr_t end_page=ADDR_TO_PN(PAGE_ALIGN_UP(cur_vaddr+count));
    
    while(cur_page<end_page){       
        vmarea_t *vma=vmmap_lookup(map,cur_page);
        if(vma==NULL){
            return -1;
        }
        
        // The offset relative to the mobj
        size_t cur_off=vma->vma_off+cur_page-vma->vma_start;

        pframe_t *pf;
        // Get the required page frame
        mobj_lock(vma->vma_obj);
        long tmp=mobj_get_pframe(vma->vma_obj,cur_off,0,&pf);
        mobj_unlock(vma->vma_obj);

        size_t this_page_write_bytes=0;
        // If the data we need to read didn't reach the end of the page
        if((cur_page+1)*PAGE_SIZE-cur_vaddr<end_vaddr-cur_vaddr){
            this_page_write_bytes=(cur_page+1)*PAGE_SIZE-cur_vaddr;
        }  else{
            this_page_write_bytes=end_vaddr-cur_vaddr;
        }

        memcpy((char *)pf->pf_addr+cur_vaddr-(size_t)PAGE_ALIGN_DOWN(cur_vaddr),
            (char *)buf+cur_vaddr-(size_t)vaddr,this_page_write_bytes);
        
        // (char *)buf=(char *)buf+this_page_write_bytes;
        cur_vaddr=cur_vaddr+this_page_write_bytes;
        cur_page++;
        pframe_release(&pf);
    }
    return 0;
    // while(cur_write_bytes<count){
    //     uintptr_t start_page=ADDR_TO_PN(cur_vaddr); // The start writing page

    //     vmarea_t *vma=vmmap_lookup(map,start_page);
    //     if(vma==NULL){
    //         return -1;
    //     }
    //     // The offset relative to the mobj
    //     size_t cur_off=vma->vma_off+start_page-vma->vma_start;
    //     size_t needed_pagenum=0;

    //     // If the needed page number is greater than this vmarea have
    //     if(vma->vma_end-cur_off<ADDR_TO_PN(PAGE_ALIGN_UP(count-cur_write_bytes))){
    //         needed_pagenum=vma->vma_end-cur_off;
    //     }else{
    //     // We need to make sure we have enough page
    //         needed_pagenum=ADDR_TO_PN(PAGE_ALIGN_UP(count-cur_write_bytes));
    //     }

    //     for(size_t i=0;i<needed_pagenum;i++){
    //         pframe_t *pf;
    //         // Get the required page frame
    //         mobj_lock(vma->vma_obj);
    //         long tmp=mobj_get_pframe(vma->vma_obj,cur_off+i,0,&pf);
    //         mobj_unlock(vma->vma_obj);

    //         size_t page_offset=cur_vaddr%PAGE_SIZE;  // Get the start position in this page
    //         size_t this_page_write_bytes=0;

    //         // If the data we need to read didn't reach the end of the page
    //         if(PAGE_SIZE-page_offset<count-cur_write_bytes){
    //             this_page_write_bytes=PAGE_SIZE-page_offset;
    //         }  else{
    //             this_page_write_bytes=count-cur_write_bytes;
    //         }

    //         // Copy the data from buf into this page
    //         memcpy((char *)pf->pf_addr+page_offset,(char *)buf+cur_write_bytes,this_page_write_bytes);

    //         cur_write_bytes+=this_page_write_bytes;
    //         cur_vaddr=cur_vaddr+this_page_write_bytes;

    //         pframe_release(&pf);
    //     }
    // }
    // return 0;

}

size_t vmmap_mapping_info(const void *vmmap, char *buf, size_t osize)
{
    return vmmap_mapping_info_helper(vmmap, buf, osize, "");
}

size_t vmmap_mapping_info_helper(const void *vmmap, char *buf, size_t osize,
                                 char *prompt)
{
    KASSERT(0 < osize);
    KASSERT(NULL != buf);
    KASSERT(NULL != vmmap);

    vmmap_t *map = (vmmap_t *)vmmap;
    ssize_t size = (ssize_t)osize;

    int len =
        snprintf(buf, (size_t)size, "%s%37s %5s %7s %18s %11s %23s\n", prompt,
                 "VADDR RANGE", "PROT", "FLAGS", "MOBJ", "OFFSET", "VFN RANGE");

    list_iterate(&map->vmm_list, vma, vmarea_t, vma_plink)
    {
        size -= len;
        buf += len;
        if (0 >= size)
        {
            goto end;
        }

        len =
            snprintf(buf, (size_t)size,
                     "%s0x%p-0x%p  %c%c%c  %7s 0x%p %#.9lx %#.9lx-%#.9lx\n",
                     prompt, (void *)(vma->vma_start << PAGE_SHIFT),
                     (void *)(vma->vma_end << PAGE_SHIFT),
                     (vma->vma_prot & PROT_READ ? 'r' : '-'),
                     (vma->vma_prot & PROT_WRITE ? 'w' : '-'),
                     (vma->vma_prot & PROT_EXEC ? 'x' : '-'),
                     (vma->vma_flags & MAP_SHARED ? " SHARED" : "PRIVATE"),
                     vma->vma_obj, vma->vma_off, vma->vma_start, vma->vma_end);
    }

end:
    if (size <= 0)
    {
        size = osize;
        buf[osize - 1] = '\0';
    }
    return osize - size;
}
