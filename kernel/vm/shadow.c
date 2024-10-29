#include "vm/shadow.h"
#include "mm/page.h"
#include "mm/pframe.h"
#include "mm/slab.h"
#include "util/debug.h"
#include "util/string.h"

#define SHADOW_SINGLETON_THRESHOLD 5

typedef struct mobj_shadow
{
    // the mobj parts of this shadow object
    mobj_t mobj;
    // a reference to the mobj that is the data source for this shadow object
    // This should be a reference to a shadow object of some ancestor process.
    // This is used to traverse the shadow object chain.
    mobj_t *shadowed;
    // a reference to the mobj at the bottom of this shadow object's chain
    // this should NEVER be a shadow object (i.e. it should have some type other
    // than MOBJ_SHADOW)
    mobj_t *bottom_mobj;
} mobj_shadow_t;

#define MOBJ_TO_SO(o) CONTAINER_OF(o, mobj_shadow_t, mobj)

static slab_allocator_t *shadow_allocator;

static long shadow_get_pframe(mobj_t *o, size_t pagenum, long forwrite,
                              pframe_t **pfp);
static long shadow_fill_pframe(mobj_t *o, pframe_t *pf);
static long shadow_flush_pframe(mobj_t *o, pframe_t *pf);
static void shadow_destructor(mobj_t *o);

static mobj_ops_t shadow_mobj_ops = {.get_pframe = shadow_get_pframe,
                                     .fill_pframe = shadow_fill_pframe,
                                     .flush_pframe = shadow_flush_pframe,
                                     .destructor = shadow_destructor};

/*
 * Initialize shadow_allocator using the slab allocator.
 */
void shadow_init()
{
    shadow_allocator=slab_allocator_create("shadow",sizeof(mobj_shadow_t));
    KASSERT(shadow_allocator);
    //NOT_YET_IMPLEMENTED("VM: shadow_init");
}

/*
 * Create a shadow object that shadows the given mobj.
 *
 * Return a new, LOCKED shadow object on success, or NULL upon failure.
 *
 * Hints:
 *  1) Create and initialize a mobj_shadow_t based on the given mobj.
 *  2) Set up the bottom object of the shadow chain, which could have two cases:
 *     a) Either shadowed is a shadow object, and you can use its bottom_mobj
 *     b) Or shadowed is not a shadow object, in which case it is the bottom 
 *        object of this chain.
 * 
 *  Make sure to manage the refcounts correctly.
 */
mobj_t *shadow_create(mobj_t *shadowed)
{
    mobj_shadow_t *new_sha=slab_obj_alloc(shadow_allocator);
    if(new_sha==NULL){
        return NULL;
    }

    mobj_init(&new_sha->mobj,MOBJ_SHADOW,&shadow_mobj_ops);     // Initialize mobj field
    
    // Initialize the shadowed and buttom mobj in mobj_shadow_t
    new_sha->shadowed=shadowed;
    mobj_ref(shadowed);
    if(shadowed->mo_type==MOBJ_SHADOW){ // If the shadowed is a shadow object
        new_sha->bottom_mobj=MOBJ_TO_SO(shadowed)->bottom_mobj;
        mobj_ref(MOBJ_TO_SO(shadowed)->bottom_mobj);
    }else{              // If the shadowed is not a shadowed object
        new_sha->bottom_mobj=shadowed;
        mobj_ref(shadowed);
    }
    // new_sha->mobj.mo_refcount+=2;   // Have the refcount to bottom_mobj and shadowed
    mobj_lock(&new_sha->mobj);
    // NOT_YET_IMPLEMENTED("VM: shadow_create");
    return &new_sha->mobj;
    // return NULL;
}

/*
 * Given a shadow object o, collapse its shadow chain as far as you can.
 *
 * Hints:
 *  1) You can only collapse if the shadowed object is a shadow object.
 *  2) When collapsing, you must manually migrate pframes from o's shadowed
 *     object to o, checking to see if a copy doesn't already exist in o.
 *  3) Be careful with refcounting! In particular, when you put away o's
 *     shadowed object, its refcount should drop to 0, initiating its
 *     destruction (shadow_destructor).
 *  4) As a reminder, any refcounting done in shadow_collapse() must play nice
 *     with any refcounting done in shadow_destructor().
 *  5) Pay attention to mobj and pframe locking.
 */
void shadow_collapse(mobj_t *o)
{
    // o Should be locked on entry and return
    mobj_t *par_o=o;    // Parent mobj
    mobj_t *cur_o=MOBJ_TO_SO(o)->shadowed;  // Set the cur_o to the second mobj in the chain
    // mobj_shadow_t *sha_o=MOBJ_TO_SO(cur_o);     // Get the shadow object
    while(cur_o->mo_type==MOBJ_SHADOW){ 
        // If its shadow object only has 1 refcount
        // Previous one call find pframe
       if(cur_o->mo_refcount==1){
            // mobj_lock(sha_o->shadowed);
            list_iterate(&cur_o->mo_pframes,cur_pf,pframe_t,pf_link){
                // Check if this pframe exist in parent shadow object
                pframe_t *pf;
                if(par_o!=o){
                    mobj_lock(par_o);
                }
                mobj_find_pframe(par_o,cur_pf->pf_pagenum,&pf); 
                if(par_o!=o){
                    mobj_unlock(par_o);
                }  
                // If the pframe is NULL
                if(pf==NULL){   // If we cannot find it in parent shadow object, we should migrate it 
                    list_remove(&cur_pf->pf_link);  // Remove current pframe from its list on shadowed object
                    list_insert_tail(&par_o->mo_pframes,&cur_pf->pf_link);  // Insert it into current mobj
                }
                else{   // If the pframe is not NULL, which means that it exist on parent shadow object
                    pframe_release(&pf);        //  Unlock the pframe
                    // mobj_free_pframe(sha_o->shadowed,&cur_pf);    
                }
            }
            // mobj_t *removed_mobj=cur_o;   // Store the shadowed object need to be removed
            MOBJ_TO_SO(par_o)->shadowed=MOBJ_TO_SO(cur_o)->shadowed;  // Update parent's shadowed object
            mobj_ref(MOBJ_TO_SO(cur_o)->shadowed);  // Increase the reference of its new shadow object
            mobj_put(&cur_o);
            cur_o=MOBJ_TO_SO(par_o)->shadowed;
        } else{
            // If we cannot remove it, update par_o and cur_o to the next one
            par_o=cur_o;
            cur_o=MOBJ_TO_SO(cur_o)->shadowed;
            // TODO: Double check it
        }
    }
    // NOT_YET_IMPLEMENTED("VM: shadow_collapse");
}

/*
 * Obtain the desired pframe from the given mobj, traversing its shadow chain if
 * necessary. This is where copy-on-write logic happens!
 *
 * Arguments: 
 *  o        - The object from which to obtain a pframe
 *  pagenum  - Number of the desired page relative to the object
 *  forwrite - Set if the caller wants to write to the pframe's data, clear if
 *             only reading
 *  pfp      - Upon success, pfp should point to the desired pframe.
 *
 * Return 0 on success, or:
 *  - Propagate errors from mobj_default_get_pframe() and mobj_get_pframe()
 *
 * Hints:
 *  1) If forwrite is set, use mobj_default_get_pframe().
 *  2) If forwrite is clear, check if o already contains the desired frame.
 *     a) If not, iterate through the shadow chain to find the nearest shadow
 *        mobj that has the frame. Do not recurse! If the shadow chain is long,
 *        you will cause a kernel buffer overflow (e.g. from forkbomb).
 *     b) If no shadow objects have the page, call mobj_get_pframe() to get the
 *        page from the bottom object and return what it returns.
 * 
 *  Pay attention to pframe locking.
 */
static long shadow_get_pframe(mobj_t *o, size_t pagenum, long forwrite,
                              pframe_t **pfp)
{
    // This function returned with pfp locked when succeed
    // o is locked when enter this function and should be locked when return
    // Set another cur to loop it
    // If forwrite is set
    if(forwrite){
        pframe_t *cur_pf;
        long tmp=mobj_default_get_pframe(o,pagenum,forwrite,&cur_pf);  // Return with cur_pf locked
        *pfp=cur_pf;
        return tmp; // If tmp<0, pfp will be unlocked
    } else{     
        // If forwrite is clear
        // Check if o already contains the desired frame
        pframe_t *cur_pf;   // Using this variable when searching

        mobj_find_pframe(o,pagenum,&cur_pf);
        if(cur_pf!=NULL){
            // Return with the pf mutex locked
            *pfp=cur_pf;
            return 0;
        }
        
        // If not in o, iterate its shadow object chain, checked if the pf exist in this chain
        mobj_t *cur_o=MOBJ_TO_SO(o)->shadowed;
        while(cur_o->mo_type==MOBJ_SHADOW){
            mobj_lock(cur_o);
            // Check each shadowed object's page frame
            mobj_find_pframe(cur_o,pagenum,&cur_pf);
            mobj_unlock(cur_o); // Unlock o before updating it
            if(cur_pf!=NULL){
                *pfp=cur_pf;
                return 0;
            }

            cur_o=MOBJ_TO_SO(cur_o)->shadowed;
        }
   
        // If we still cannot find, and now cur_o has become the buttom object
        mobj_lock(cur_o);
        long tmp=mobj_get_pframe(cur_o,pagenum,forwrite,&cur_pf);   // This one will lock pfp on return when find it
        mobj_unlock(cur_o);
        if(tmp<0){         
            return tmp;
        }
        *pfp=cur_pf;
    }
    // NOT_YET_IMPLEMENTED("VM: shadow_get_pframe");
    return 0;
}

/*
 * Use the given mobj's shadow chain to fill the given pframe.
 *
 * Return 0 on success, or:
 *  - Propagate errors from mobj_get_pframe()
 *
 * Hints:
 *  1) Explore mobj_default_get_pframe(), which calls mobj_create_pframe(), to
 *     understand what state pf is in when this function is called, and how you
 *     can use it.
 *  2) As you can see above, shadow_get_pframe would call
 *     mobj_default_get_pframe (when the forwrite is set), which would 
 *     create and then fill the pframe (shadow_fill_pframe is called).
 *  3) Traverse the shadow chain for a copy of the frame, starting at the given
 *     mobj's shadowed object. You can use mobj_find_pframe to look for the 
 *     page frame. pay attention to locking/unlocking, and be sure not to 
 *     recurse when traversing.
 *  4) If none of the shadow objects have a copy of the frame, use
 *     mobj_get_pframe on the bottom object to get it.
 *  5) After obtaining the desired frame, simply copy its contents into pf.
 */
static long shadow_fill_pframe(mobj_t *o, pframe_t *pf)
{
    // KASSERT(o->mo_type==MOBJ_SHADOW&&"Make sure it is shadow object");
    mobj_t *cur_o=MOBJ_TO_SO(o)->shadowed;  // The first mobj we need to iterate
    size_t request_pagenum=pf->pf_pagenum;  // Requested page number
    pframe_t *cur_pf;       // The finded pframe
    while(cur_o->mo_type==MOBJ_SHADOW){
        mobj_lock(cur_o);   // Lock current mobj
        mobj_find_pframe(cur_o,request_pagenum,&cur_pf);
        mobj_unlock(cur_o);
        // If we found the page frame
        if(cur_pf!=NULL){
            memcpy(pf->pf_addr,cur_pf->pf_addr,PAGE_SIZE);  // Copy its content into pf
            pframe_release(&cur_pf);
            // kmutex_unlock(&cur_pf->pf_mutex);
            return 0;
        }        
        cur_o=MOBJ_TO_SO(cur_o)->shadowed;  // Update current mobj
    }
    
    // If none of the shadow object have a copy of the pframe, get it on the bottom object
    mobj_lock(cur_o);
    long tmp=mobj_get_pframe(cur_o,request_pagenum,0,&cur_pf);
    mobj_unlock(cur_o);
    if(tmp<0){
        return tmp;
    }
    // If get it, copy it to pf
    memcpy(pf->pf_addr,cur_pf->pf_addr,PAGE_SIZE);
    pframe_release(&cur_pf);
    // NOT_YET_IMPLEMENTED("VM: shadow_fill_pframe");
    return 0;
}

/*
 * Flush a shadow object's pframe to disk.
 *
 * Return 0 on success.
 *
 * Hint:
 *  - Are shadow objects backed to disk? Do you actually need to do anything
 *    here?
 */
static long shadow_flush_pframe(mobj_t *o, pframe_t *pf)
{
    // NOT_YET_IMPLEMENTED("VM: shadow_flush_pframe");
    return 0;
}

/*
 * Clean up all resources associated with mobj o.
 *
 * Hints:
 *  - Check out mobj_put() to understand how this function gets called.
 *
 *  1) Call mobj_default_destructor() to flush o's pframes.
 *  2) Put the shadow and bottom_mobj members of the shadow object.
 *  3) Free the mobj_shadow_t.
 */
static void shadow_destructor(mobj_t *o)
{
    mobj_default_destructor(o);
    mobj_put(&MOBJ_TO_SO(o)->shadowed);
    mobj_put(&MOBJ_TO_SO(o)->bottom_mobj);
    slab_obj_free(shadow_allocator,o);
    // NOT_YET_IMPLEMENTED("VM: shadow_destructor");
}
