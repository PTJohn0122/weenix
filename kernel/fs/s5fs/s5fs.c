#include "errno.h"
#include "globals.h"
#include "kernel.h"
#include <mm/slab.h>

#include "util/debug.h"
#include "util/printf.h"
#include "util/string.h"

#include "proc/kmutex.h"

#include "fs/dirent.h"
#include "fs/file.h"
#include "fs/s5fs/s5fs.h"
#include "fs/s5fs/s5fs_subr.h"
#include "fs/stat.h"

#include "mm/kmalloc.h"

static long s5_check_super(s5_super_t *super);

static long s5fs_check_refcounts(fs_t *fs);

static void s5fs_read_vnode(fs_t *fs, vnode_t *vn);

static void s5fs_delete_vnode(fs_t *fs, vnode_t *vn);

static long s5fs_umount(fs_t *fs);

static void s5fs_sync(fs_t *fs);

static ssize_t s5fs_read(vnode_t *vnode, size_t pos, void *buf, size_t len);

static ssize_t s5fs_write(vnode_t *vnode, size_t pos, const void *buf,
                          size_t len);

static long s5fs_mmap(vnode_t *file, mobj_t **ret);

static long s5fs_mknod(struct vnode *dir, const char *name, size_t namelen,
                       int mode, devid_t devid, struct vnode **out);

static long s5fs_lookup(vnode_t *dir, const char *name, size_t namelen,
                        vnode_t **out);

static long s5fs_link(vnode_t *dir, const char *name, size_t namelen,
                      vnode_t *child);

static long s5fs_unlink(vnode_t *vdir, const char *name, size_t namelen);

static long s5fs_rename(vnode_t *olddir, const char *oldname, size_t oldnamelen,
                        vnode_t *newdir, const char *newname,
                        size_t newnamelen);

static long s5fs_mkdir(vnode_t *dir, const char *name, size_t namelen,
                       struct vnode **out);

static long s5fs_rmdir(vnode_t *parent, const char *name, size_t namelen);

static long s5fs_readdir(vnode_t *vnode, size_t pos, struct dirent *d);

static long s5fs_stat(vnode_t *vnode, stat_t *ss);

static void s5fs_truncate_file(vnode_t *vnode);

static long s5fs_release(vnode_t *vnode, file_t *file);

static long s5fs_get_pframe(vnode_t *vnode, size_t pagenum, long forwrite,
                            pframe_t **pfp);

static long s5fs_fill_pframe(vnode_t *vnode, pframe_t *pf);

fs_ops_t s5fs_fsops = {.read_vnode = s5fs_read_vnode,
                       .delete_vnode = s5fs_delete_vnode,
                       .umount = s5fs_umount,
                       .sync = s5fs_sync};

static vnode_ops_t s5fs_dir_vops = {.read = NULL,
                                    .write = NULL,
                                    .mmap = NULL,
                                    .mknod = s5fs_mknod,
                                    .lookup = s5fs_lookup,
                                    .link = s5fs_link,
                                    .unlink = s5fs_unlink,
                                    .rename = s5fs_rename,
                                    .mkdir = s5fs_mkdir,
                                    .rmdir = s5fs_rmdir,
                                    .readdir = s5fs_readdir,
                                    .stat = s5fs_stat,
                                    .acquire = NULL,
                                    .release = NULL,
                                    .get_pframe = s5fs_get_pframe,
                                    .fill_pframe = s5fs_fill_pframe,
                                    .flush_pframe = NULL,
                                    .truncate_file = NULL};

static vnode_ops_t s5fs_file_vops = {.read = s5fs_read,
                                     .write = s5fs_write,
                                     .mmap = s5fs_mmap,
                                     .mknod = NULL,
                                     .lookup = NULL,
                                     .link = NULL,
                                     .unlink = NULL,
                                     .mkdir = NULL,
                                     .rmdir = NULL,
                                     .readdir = NULL,
                                     .stat = s5fs_stat,
                                     .acquire = NULL,
                                     .release = NULL,
                                     .get_pframe = s5fs_get_pframe,
                                     .fill_pframe = s5fs_fill_pframe,
                                     .flush_pframe = NULL,
                                     .truncate_file = s5fs_truncate_file};

/*
 * Initialize the passed-in fs_t. The only members of fs_t that are initialized
 * before the call to s5fs_mount are fs_dev and fs_type ("s5fs"). You must
 * initialize everything else: fs_vnode_allocator, fs_i, fs_ops, fs_root.
 *
 * Initialize the block device for the s5fs_t that is created, and copy
 * the super block from disk into memory.
 */
long s5fs_mount(fs_t *fs)
{
    int num;

    KASSERT(fs);

    if (sscanf(fs->fs_dev, "disk%d", &num) != 1)
    {
        return -EINVAL;
    }

    blockdev_t *dev = blockdev_lookup(MKDEVID(DISK_MAJOR, num));
    if (!dev)
        return -EINVAL;

    slab_allocator_t *allocator =
        slab_allocator_create("s5_node", sizeof(s5_node_t));
    fs->fs_vnode_allocator = allocator;

    s5fs_t *s5fs = (s5fs_t *)kmalloc(sizeof(s5fs_t));

    if (!s5fs)
    {
        slab_allocator_destroy(fs->fs_vnode_allocator);
        fs->fs_vnode_allocator = NULL;
        return -ENOMEM;
    }

    s5fs->s5f_bdev = dev;

    pframe_t *pf;
    s5_get_disk_block(s5fs, S5_SUPER_BLOCK, 0, &pf);
    memcpy(&s5fs->s5f_super, pf->pf_addr, sizeof(s5_super_t));
    s5_release_disk_block(&pf);

    if (s5_check_super(&s5fs->s5f_super))
    {
        kfree(s5fs);
        slab_allocator_destroy(fs->fs_vnode_allocator);
        fs->fs_vnode_allocator = NULL;
        return -EINVAL;
    }

    kmutex_init(&s5fs->s5f_mutex);

    s5fs->s5f_fs = fs;

    fs->fs_i = s5fs;
    fs->fs_ops = &s5fs_fsops;
    fs->fs_root = vget(fs, s5fs->s5f_super.s5s_root_inode);
    // vunlock(fs->fs_root);

    return 0;
}

/* Initialize a vnode and inode by reading its corresponding inode info from
 * disk.
 *
 * Hints:
 *  - To read the inode from disk, you will need to use the following:
 *     - VNODE_TO_S5NODE to obtain the s5_node_t with the inode corresponding
 *       to the provided vnode
 *     - FS_TO_S5FS to obtain the s5fs object
 *     - S5_INODE_BLOCK(vn->v_vno) to determine the block number of the block that
 *       contains the inode info
 *     - s5_get_disk_block and s5_release_disk_block to handle the disk block
 *     - S5_INODE_OFFSET to find the desired inode within the disk block
 *       containing it (returns the offset that the inode is stored within the block)
 *  - You should initialize the s5_node_t's inode field by reading directly from
 *    the inode on disk by using the page frame returned from s5_get_disk_block. Also 
 *    make sure to initialize the dirtied_inode field.
 *  - Using the inode info, you need to initialize the following vnode fields:
 *    vn_len, vn_mode, and vn_ops using the fields found in the s5_inode struct.
 *  - See stat.h for vn_mode values.
 *  - For character and block devices:
 *    1) Initialize vn_devid by reading the inode's s5_indirect_block field.
 *    2) Set vn_ops to NULL.
 */
static void s5fs_read_vnode(fs_t *fs, vnode_t *vn)
{
    pframe_t * founded_pfram;
    s5_node_t *s5_node=VNODE_TO_S5NODE(vn); // Get s5 node
    s5fs_t* s5= FS_TO_S5FS(fs); // Get the s5fs object
    blocknum_t b_num=S5_INODE_BLOCK(vn->vn_vno); // Block number containing the inode info
    long in_offset=S5_INODE_OFFSET(vn->vn_vno); // Find the desired inode
    s5_get_disk_block(s5,b_num,0,&founded_pfram);

    s5_node->dirtied_inode=1; // Initialize dirty inode
    // (s5_node->inode)=*((s5_inode_t *)(founded_pfram->pf_addr)+in_offset); // Initialize inode
    memcpy(&s5_node->inode,((s5_inode_t *)(founded_pfram->pf_addr)+in_offset),sizeof(s5_inode_t));
    vn->vn_len=s5_node->inode.s5_un.s5_size; // Initialize the file length

    switch(s5_node->inode.s5_type){
        case S5_TYPE_CHR:
            vn->vn_mode=S_IFCHR;
            vn->vn_devid=s5_node->inode.s5_indirect_block;
            vn->vn_ops=NULL;
            break;
        case S5_TYPE_BLK:
            vn->vn_mode=S_IFBLK;
            vn->vn_devid=s5_node->inode.s5_indirect_block;
            vn->vn_ops=NULL;
            break;
        case S5_TYPE_DATA:
            vn->vn_mode=S_IFREG;
            vn->vn_devid=0;
            vn->vn_ops=&s5fs_file_vops;
            break;
        case S5_TYPE_DIR:
            vn->vn_mode=S_IFDIR;
            vn->vn_devid=0;
            vn->vn_ops=&s5fs_dir_vops;
            break;
    }
    s5_release_disk_block(&founded_pfram); // Release the page frame
    // NOT_YET_IMPLEMENTED("S5FS: s5fs_read_vnode");
}

/* Clean up the inode corresponding to the given vnode.
 *
 * Hints:
 *  - This function is called in the following way: 
 *          mobj_put -> vnode_destructor -> s5fs_delete_vnode.
 *  - Cases to consider:
 *    1) The inode is no longer in use (linkcount == 0), so free it using
 *       s5_free_inode.
 *    2) The inode is dirty, so write it back to disk.
 *    3) The inode is unchanged, so do nothing.
 */
static void s5fs_delete_vnode(fs_t *fs, vnode_t *vn)
{
    pframe_t * founded_pfram;
    s5_node_t *s5_node=VNODE_TO_S5NODE(vn); // Get s5 node
    s5fs_t* s5= FS_TO_S5FS(fs); // Get the s5fs object
    blocknum_t b_num=S5_INODE_BLOCK(vn->vn_vno); // Block number containing the inode info
    long in_offset=S5_INODE_OFFSET(vn->vn_vno); // Find the desired inode
    s5_get_disk_block(s5,b_num,1,&founded_pfram);

    // If it is dirty, copy the data to the page frame
    if(s5_node->dirtied_inode){
        memcpy(((s5_inode_t *)(founded_pfram->pf_addr)+in_offset),
            &(s5_node->inode),sizeof(s5_inode_t));
    }
    s5_release_disk_block(&founded_pfram);
    if(s5_node->inode.s5_linkcount==0){
        s5_free_inode(s5,s5_node->inode.s5_number);
    }
    // NOT_YET_IMPLEMENTED("S5FS: s5fs_delete_vnode");
}

/*
 * See umount in vfs.h
 *
 * Check reference counts and the super block.
 * Put the fs_root.
 * Write the super block out to disk.
 * Flush the underlying memory object.
 */
static long s5fs_umount(fs_t *fs)
{
    s5fs_t *s5fs = FS_TO_S5FS(fs);
    blockdev_t *bd = s5fs->s5f_bdev;

    if (s5fs_check_refcounts(fs))
    {
        panic(
            "s5fs_umount: WARNING: linkcount corruption "
            "discovered in fs on block device with major %d "
            "and minor %d!!\n",
            MAJOR(bd->bd_id), MINOR(bd->bd_id));
    }
    if (s5_check_super(&s5fs->s5f_super))
    {
        panic(
            "s5fs_umount: WARNING: corrupted superblock "
            "discovered on fs on block device with major %d "
            "and minor %d!!\n",
            MAJOR(bd->bd_id), MINOR(bd->bd_id));
    }

    vput(&fs->fs_root);

    s5fs_sync(fs);
    kfree(s5fs);
    return 0;
}

static void s5fs_sync(fs_t *fs)
{
    s5fs_t *s5fs = FS_TO_S5FS(fs);
    mobj_t *mobj = S5FS_TO_VMOBJ(s5fs);

    mobj_lock(mobj);

    pframe_t *pf;
    mobj_get_pframe(mobj, S5_SUPER_BLOCK, 1, &pf);
    memcpy(pf->pf_addr, &s5fs->s5f_super, sizeof(s5_super_t));
    pframe_release(&pf);

    mobj_flush(S5FS_TO_VMOBJ(s5fs));
    mobj_unlock(S5FS_TO_VMOBJ(s5fs));
}

/* Wrapper around s5_read_file. */
static ssize_t s5fs_read(vnode_t *vnode, size_t pos, void *buf, size_t len)
{
    KASSERT(!S_ISDIR(vnode->vn_mode) && "should be handled at the VFS level");
    s5_node_t *s5_node=VNODE_TO_S5NODE(vnode);
    ssize_t tmp=s5_read_file(s5_node,pos,buf,len);
    // NOT_YET_IMPLEMENTED("S5FS: s5fs_read");
    return tmp;
}

/* Wrapper around s5_write_file. */
static ssize_t s5fs_write(vnode_t *vnode, size_t pos, const void *buf,
                          size_t len)
{
    KASSERT(!S_ISDIR(vnode->vn_mode) && "should be handled at the VFS level");
    s5_node_t *s5_node=VNODE_TO_S5NODE(vnode);
    ssize_t tmp=s5_write_file(s5_node,pos,buf,len);
    // NOT_YET_IMPLEMENTED("S5FS: s5fs_write");
    return tmp;
}

/*
 * Any error handling should have been done before this function was called.
 * Simply add a reference to the underlying mobj and return it through ret.
 */
static long s5fs_mmap(vnode_t *file, mobj_t **ret)
{
    mobj_ref(&file->vn_mobj);   // Add a reference to the underlying mobj
    *ret=&file->vn_mobj;
    // NOT_YET_IMPLEMENTED("VM: s5fs_mmap");
    return 0;
}

/* Allocate and initialize an inode and its corresponding vnode.
 *
 *  dir     - The directory in which to make the new inode
 *  name    - The name of the new inode
 *  namelen - Name length
 *  mode    - vn_mode of the new inode, see S_IF{} macros in stat.h
 *  devid   - devid of the new inode for special devices
 *  out     - Upon success, out must point to the newly created vnode
 *            Upon failure, out must be unchanged
 *
 * Return 0 on success, or:
 *  - ENOTSUP: mode is not S_IFCHR, S_BLK, or S_ISREG
 *  - Propagate errors from s5_alloc_inode and s5_link
 *
 * Hints:
 *  - Use mode to determine the S5_TYPE_{} for the inode.
 *  - Use s5_alloc_inode is allocate a new inode.
 *  - Use vget to obtain the vnode corresponding to the newly created inode.
 *  - Use s5_link to link the newly created inode/vnode to the parent directory.
 *    - You will need to clean up the vnode using vput in the case that 
 *      the link operation fails. 
 */
static long s5fs_mknod(struct vnode *dir, const char *name, size_t namelen,
                       int mode, devid_t devid, struct vnode **out)
{
    KASSERT(S_ISDIR(dir->vn_mode) && "should be handled at the VFS level");
    if(mode!=S_IFCHR&&mode!=S_IFBLK&&mode!=S_IFREG){
        return -ENOTSUP;
    }
    switch(mode){   // Determine the type of inode
        case S_IFCHR:
            mode=S5_TYPE_CHR;
            break;
        case S_IFBLK:
            mode=S5_TYPE_BLK;
            break;
        default:
            mode=S5_TYPE_DATA;
            break;
    }
    s5fs_t *s5= VNODE_TO_S5FS(dir);
    s5_node_t *s5_parent_node=VNODE_TO_S5NODE(dir); // Get the s5node of parent directory
    long new_ino=s5_alloc_inode(s5,mode,devid);  // Get the new inode number
    if(new_ino<0)  {return new_ino;}
    vnode_t *chi_vnode=vget(s5->s5f_fs,new_ino); // Get corresponding vnode fot the new inode number
    s5_node_t *s5_child_node=VNODE_TO_S5NODE(chi_vnode);
    long tmp=s5_link(s5_parent_node,name,namelen,s5_child_node);  // Link these two inodes
    if(tmp<0)   {
        vput(&chi_vnode);  // Decremment the reference of child node
        return tmp;
    }
    *out=chi_vnode;
    // NOT_YET_IMPLEMENTED("S5FS: s5fs_mknod");
    return 0;
}

/* Search for a given entry within a directory.
 *
 *  dir     - The directory in which to search
 *  name    - The name to search for
 *  namelen - Name length
 *  ret     - Upon success, ret must point to the found vnode
 *
 * Return 0 on success, or:
 *  - Propagate errors from s5_find_dirent
 *
 * Hints:
 *  - Use s5_find_dirent, vget, and vref.
 *  - vref can be used in the case where the vnode you're looking for happens
 *    to be dir itself.
 */
long s5fs_lookup(vnode_t *dir, const char *name, size_t namelen,
                 vnode_t **ret)
{
    s5_node_t *s5node=VNODE_TO_S5NODE(dir);
    long find_ino=s5_find_dirent(s5node,name,namelen,NULL);
    if(find_ino<0)  {return find_ino;}
    if(VNODE_TO_S5NODE(dir)->inode.s5_number==find_ino)   {
        vref(dir);
        *ret=dir;
    } else {
        *ret=vget(dir->vn_fs,find_ino);
    }
    // TODO: Not sure, because the refcount of ret has been increased in vget
    // NOT_YET_IMPLEMENTED("S5FS: s5fs_lookup");
    return 0;
}

/* Wrapper around s5_link.
 *
 * Return whatever s5_link returns, or:
 *  - EISDIR: child is a directory
 */
static long s5fs_link(vnode_t *dir, const char *name, size_t namelen,
                      vnode_t *child)
{
    KASSERT(S_ISDIR(dir->vn_mode) && "should be handled at the VFS level");
    if(S_ISDIR(child->vn_mode)) {return -EISDIR;} // Child is a directory
    s5_node_t *dir_node=VNODE_TO_S5NODE(dir);
    s5_node_t *chl_node=VNODE_TO_S5NODE(child);
    long tmp=s5_link(dir_node,name,namelen,chl_node);
    // NOT_YET_IMPLEMENTED("S5FS: s5fs_link");
    return tmp;
}

/* Remove the directory entry in dir corresponding to name and namelen.
 *
 * Return 0 on success, or:
 *  - Propagate errors from s5_find_dirent
 *
 * Hints:
 *  - Use s5_find_dirent and s5_remove_dirent.
 *  - You will probably want to use vget_locked and vput_locked to protect the
 *    found vnode. Make sure your implementation of s5_remove_dirent knows what
 *    to expect.
 */
static long s5fs_unlink(vnode_t *dir, const char *name, size_t namelen)
{
    KASSERT(S_ISDIR(dir->vn_mode) && "should be handled at the VFS level");
    KASSERT(!name_match(".", name, namelen));
    KASSERT(!name_match("..", name, namelen));

    s5_node_t *dir_node= VNODE_TO_S5NODE(dir);
    long find_ino=s5_find_dirent(dir_node,name,namelen,NULL); // Child inode number
    if(find_ino<0)  {return find_ino;}

    // Get the child vnode and corresponding s5_node
    vnode_t *child_vnode=vget_locked(dir->vn_fs,find_ino); 
    s5_node_t *chl_node=VNODE_TO_S5NODE(child_vnode);
    
    s5_remove_dirent(dir_node,name,namelen,chl_node);
    vput_locked(&child_vnode);
    // NOT_YET_IMPLEMENTED("S5FS: s5fs_unlink");
    return 0;
}

/* Change the name or location of a file.
 *
 *  olddir     - The directory in which the file currently resides
 *  oldname    - The old name of the file
 *  oldnamelen - Length of the old name
 *  newdir     - The directory in which to place the file
 *  newname    - The new name of the file
 *  newnamelen - Length of the new name
 *
 * Return 0 on success, or:
 *  - ENAMETOOLONG: newname is >= NAME_LEN
 *  - ENOTDIR: newdir is not a directory
 *  - EISDIR: newname is a directory 
 *  - Propagate errors from s5_find_dirent and s5_link
 *
 * Steps:
 * 1) Use s5_find_dirent and vget_locked to obtain the vnode corresponding to old name.
 * 2) If newdir already contains an entry for newname:
 *      a) Compare node numbers and do nothing if old name and new name refer to the same inode
 *	    b) Check if new-name is a directory
 *	    c) Remove the previously existing entry for new name using s5_remove_dirent
 *	    d) Link the new direct using s5_link
 * 3) If there is no entry for newname, use s5_link to add a link to the old node at new name
 * 4) Use s5_remove_dirent to remove old name’s entry in olddir
 *
 * 
 * Hints:
 *  - olddir and newdir should be locked on entry and not unlocked during the
 *    duration of this function. Any other vnodes locked should be unlocked and
 *    put before return.
 *  - Be careful with locking! Because you are making changes to the vnodes,
 *    you should always be using vget_locked and vput_locked. Be sure to clean
 *    up properly in error/special cases.
 *  - You DO NOT need to support renaming of directories in Weenix. If you were to support this
 *    in the s5fs layer (which is not extra credit), you can use the following routine:
 *	  1) Use s5_find_dirent and vget_locked to obtain the vnode corresponding to old name.
 *	  2) If newer already contains an entry for newname:
 *		   a) Compare node numbers and do nothing if old name and new name refer to the same inode
 *		   b) Check if new-name is a directory
 *		   c) Remove the previously existing entry for new name using s5_remove_dirent
 *		   d) Link the new direct using s5_link
 * 3) If there is no entry for newname, use s5_link to add a link to the old node at new name
 * 4) Use s5_remove_dirent to remove old name’s entry in olddir
 */
static long s5fs_rename(vnode_t *olddir, const char *oldname, size_t oldnamelen,
                        vnode_t *newdir, const char *newname,
                        size_t newnamelen)
{   
//     // TODO: Need come back
//     // Check the newname length and is it directory
//     if(newnamelen>=NAME_LEN)    {return -ENAMETOOLONG;} 
//     if(!S_ISDIR(newdir->vn_mode))    {return -ENOTDIR;}
    
//     // Obtain the vnode corresponding to old name
//     s5_node_t *old_node=VNODE_TO_S5NODE(olddir);
//     long old_inode=s5_find_dirent(old_node,oldname,oldnamelen,NULL);
//     if(old_inode<0) {return old_inode;}
//     vnode_t *o_vnode=vget_locked(olddir->vn_fs,old_inode);

//     // If newdir contains a entry with for newname
//     s5_node_t * new_node=VNODE_TO_S5NODE(newdir);
//     long new_inode=s5_find_dirent(new_node,newname,newnamelen,NULL);

//  /* 2) If newdir already contains an entry for newname:
//  *      a) Compare node numbers and do nothing if old name and new name refer to the same inode
//  *	    b) Check if new-name is a directory
//  *	    c) Remove the previously existing entry for new name using s5_remove_dirent
//  *	    d) Link the new direct using s5_link
//  */
//     if(new_inode>0){
//         vnode_t *n_vnode=vget_locked(newdir->vn_fs,new_inode);
//         if(new_inode!=old_inode){
//             if(S_ISDIR(newdir->vn_mode))    {
//                 vput_locked(&o_vnode);
//                 vput_locked(&n_vnode);
//                 return -EISDIR;
//             }
//             s5_remove_dirent(new_node,newname,newnamelen,n_vnode);
//             long tmp1=s5_link(newdir,newname,newnamelen,n_vnode);
//             if(tmp1<0){
//                 vput_locked(&o_vnode);
//                 vput_locked(&n_vnode);
//                 return tmp1;
//             }
//         }
//     } 
//     if(new_inode<0)  {
//         vput_locked(&o_vnode);
//         return new_inode;
//     }
//     vnode_t *n_vnode=vget_locked(newdir->vn_fs,new_inode);
//     if(n_vnode!=NULL&&new_inode!=old_inode){
//         if(S_ISDIR(n_vnode->vn_mode))    {
//             vput_locked(&o_vnode);
//             vput_locked(&n_vnode);
//             return -EISDIR;
//         }
//         s5_remove_dirent(new_node,newname,newnamelen,n_vnode);
//         long tmp1=s5_link(olddir,newname,newnamelen,newdir);
//         if(tmp1<0){
//             vput_locked(&o_vnode);
//             vput_locked(&n_vnode);
//             return tmp1;
//         }
//     } else if(n_vnode==NULL){
//         long tmp1=s5_link(o_vnode,newname,newnamelen,n_vnode);
//         if(tmp1<0){
//             vput_locked(&o_vnode);
//             vput_locked(&n_vnode);
//             return tmp1;
//         }
//         s5_remove_dirent(old_node,oldname,oldnamelen,o_vnode);
//     }
//     vput_locked(&o_vnode);
//     vput_locked(&n_vnode);
    NOT_YET_IMPLEMENTED("S5FS: s5fs_rename");
    return 0;
}

/* Create a directory.
 *
 *  dir     - The directory in which to create the new directory
 *  name    - The name of the new directory
 *  namelen - Name length of the new directory
 *  out     - On success, must point to the new directory, unlocked
 *            On failure, must be unchanged
 *
 * Return 0 on success, or:
 *  - Propagate errors from s5_alloc_inode and s5_link
 *
 * Steps:
 * 1) Allocate an inode.
 * 2) Get the child directory vnode.
 * 3) Create the "." entry.
 * 4) Create the ".." entry.
 * 5) Create the name/namelen entry in the parent (that corresponds 
 *    to the new directory)
 *
 * Hints:
 *  - If you run into any errors, you must undo previous steps.
 *  - You may assume/assert that undo operations do not fail.
 *  - It may help to assert that linkcounts are correct.
 */
static long s5fs_mkdir(vnode_t *dir, const char *name, size_t namelen,
                       struct vnode **out)
{
    KASSERT(S_ISDIR((dir)->vn_mode) && "should be handled at the VFS level");

    const char *dot=".";
    const char *doubleDot="..";
    // Allocate an inode
    s5fs_t *s5=VNODE_TO_S5FS(dir);
    s5_node_t *par_node=VNODE_TO_S5NODE(dir);
    long ino=s5_alloc_inode(s5,S5_TYPE_DIR,NULL); 
    if(ino<0)   {return ino;}

    // Obtain child vnode
    vnode_t *chl_vnode=vget_locked(s5->s5f_fs,ino);
    s5_node_t *chl_node=VNODE_TO_S5NODE(chl_vnode);

    long tmp1=s5_link(chl_node,dot,1,chl_node); // Represent the directory itself
    if(tmp1<0)  {
        vput_locked(&chl_vnode);
        return tmp1;
    }
    long tmp2=s5_link(chl_node,doubleDot,2,par_node); // Parent directory and child directory
    if(tmp2<0){
        s5_remove_dirent(chl_node,dot,1,chl_node); // Undo the previous steps
        vput_locked(&chl_vnode);
        return tmp2;
    }

    // KASSERT(chl_node->inode.s5_linkcount==2); // Assert its linkcount is correct
    // TODO: Do I link it correctly
    long tmp3=s5_link(par_node,name,namelen,chl_node); // Create name/namelen entry
    if(tmp3<0){
        s5_remove_dirent(chl_node,dot,1,chl_node);
        s5_remove_dirent(chl_node,doubleDot,2,par_node);
        vput_locked(&chl_vnode);
        return tmp3;
    }
    *out=chl_vnode;
    vunlock(chl_vnode);
    // NOT_YET_IMPLEMENTED("S5FS: s5fs_mkdir");
    return 0;
}

/* Remove a directory.
 *
 * Return 0 on success, or:
 *  - ENOTDIR: The specified entry is not a directory
 *  - ENOTEMPTY: The directory to be removed has entries besides "." and ".."
 *  - Propagate errors from s5_find_dirent
 *
 * Hints:
 *  - If you are confident you are managing directory entries properly, you can
 *    check for ENOTEMPTY by simply checking the length of the directory to be
 *    removed. An empty directory has two entries: "." and "..". 
 *  - Remove the three entries created in s5fs_mkdir.
 */
static long s5fs_rmdir(vnode_t *parent, const char *name, size_t namelen)
{
    KASSERT(!name_match(".", name, namelen));
    KASSERT(!name_match("..", name, namelen));
    if(!S_ISDIR(parent->vn_mode)){
        return -ENOTDIR;    
    }
    KASSERT(S_ISDIR(parent->vn_mode) && "should be handled at the VFS level");

    s5_node_t *par_node=VNODE_TO_S5NODE(parent);
    s5fs_t *par_s5= VNODE_TO_S5FS(parent);
    long ino=s5_find_dirent(par_node,name,namelen,NULL); // Obtain the child inode
    if(ino<0)   {return ino;}
    
    vnode_t *child=vget_locked(par_s5->s5f_fs,ino); // Obtain child vnode
    if(!S_ISDIR(child->vn_mode))    {
        vput_locked(&child);
        return -ENOTDIR;
    }
    
    // If it has entries besides "." and ".."
    if(child->vn_len!=2*sizeof(s5_dirent_t)){
        vput_locked(&child);
        return -ENOTEMPTY;
    }
  
    s5_node_t *chl_node=VNODE_TO_S5NODE(child);
    const char *dot=".";
    const char *doubleDot="..";
    long ino1=s5_find_dirent(chl_node,dot,1,NULL); // Check the two entries
    if(ino1<0)  {
        vput_locked(&child);
        return ino1;
    }
    long ino2=s5_find_dirent(chl_node,doubleDot,2,NULL);
    if(ino2<0)  {
        vput_locked(&child);
        return ino2;
    }

    // Remove the three entries created in mkdir
    //par_node->dirtied_inode=1; // Mark it as dirtied
    chl_node->dirtied_inode=1;
    s5_remove_dirent(chl_node,dot,1,chl_node);
    s5_remove_dirent(chl_node,doubleDot,2,par_node);
    s5_remove_dirent(par_node,name,namelen,chl_node);
    //par_node->inode.s5_linkcount-=2;
    vput_locked(&child);
    // NOT_YET_IMPLEMENTED("S5FS: s5fs_rmdir");
    return 0;
}

/* Read a directory entry.
 *
 *  vnode - The directory from which to read an entry
 *  pos   - The position within the directory to start reading from
 *  d     - Caller-allocated dirent that must be properly initialized on
 *          successful return
 *
 * Return bytes read on success, or:
 *  - Propagate errors from s5_read_file
 *
 * Hints:
 *  - Use s5_read_file to read an s5_dirent_t. To do so, you can create a local 
 *    s5_dirent_t variable and use that as the buffer to pass into s5_read_file. 
 *  - Be careful that you read into an s5_dirent_t and populate the provided
 *    dirent_t properly.
 */
static long s5fs_readdir(vnode_t *vnode, size_t pos, struct dirent *d)
{
    KASSERT(S_ISDIR(vnode->vn_mode) && "should be handled at the VFS level");
    
    s5_node_t *s5node=VNODE_TO_S5NODE(vnode);
    s5_dirent_t s5_dir;
    //s5_dir.s5d_inode=0;
    //memset(s5_dir.s5d_name,0,sizeof(s5_dir.s5d_name));
    // Read from s5node into s5_dir
    ssize_t read_num=s5_read_file(s5node,pos,(char *)&s5_dir,sizeof(s5_dirent_t));
    if(read_num<0)  {return read_num;}

    // If read successfully, initialize d_ino
    d->d_ino=s5_dir.s5d_inode;
    strcpy(d->d_name,s5_dir.s5d_name);
    d->d_off=pos+read_num;
    // NOT_YET_IMPLEMENTED("S5FS: s5fs_readdir");
    
    return read_num;
}

/* Get file status.
 *
 *  vnode - The vnode of the file in question
 *  ss    - Caller-allocated stat_t struct that must be initialized on success
 *
 * This function should not fail.
 *
 * Hint:
 *  - Initialize st_blocks using s5_inode_blocks.
 *  - Initialize st_mode using the corresponding vnode modes in stat.h.
 *  - Initialize st_rdev with the devid of special devices.
 *  - Initialize st_ino with the inode number.
 *  - Initialize st_nlink with the linkcount.
 *  - Initialize st_blksize with S5_BLOCK_SIZE.
 *  - Initialize st_size with the size of the file.
 *  - Initialize st_dev with the bd_id of the s5fs block device.
 *  - Set all other fields to 0.
 */
static long s5fs_stat(vnode_t *vnode, stat_t *ss)
{
    s5_node_t *s5node=VNODE_TO_S5NODE(vnode);
 
    // Initialize different fields
    ss->st_blocks=s5_inode_blocks(s5node);
    ss->st_mode=vnode->vn_mode;
    ss->st_rdev=vnode->vn_devid;
    ss->st_ino=s5node->inode.s5_number;
    ss->st_nlink=s5node->inode.s5_linkcount;
    ss->st_blksize=S5_BLOCK_SIZE;
    ss->st_size=vnode->vn_len;
    ss->st_dev=VNODE_TO_S5FS(vnode)->s5f_bdev->bd_id;


    // Set other fields to 0
    ss->st_atime=0;
    ss->st_ctime=0;
    ss->st_gid=0;
    ss->st_mtime=0;
    ss->st_uid=0;
    // NOT_YET_IMPLEMENTED("S5FS: s5fs_stat");
    return 0;
}

/**
 * Truncate the vnode and inode length to be 0. 
 * 
 * file - the vnode, whose size should be truncated 
 * 
 * This routine should only be called from do_open via 
 * vn_ops in the case that a regular file is opened with the 
 * O_TRUNC flag specified. 
 */
static void s5fs_truncate_file(vnode_t *file)
{
    KASSERT(S_ISREG(file->vn_mode) && "This routine should only be called for regular files");
    file->vn_len = 0;
    s5_node_t* s5_node = VNODE_TO_S5NODE(file); 
    s5_inode_t* s5_inode = &s5_node->inode; 
    // setting the size of the inode to be 0 as well 
    s5_inode->s5_un.s5_size = 0; 
    s5_node->dirtied_inode = 1; 
    
    // Call subroutine to free the blocks that were used 
    vlock(file); 
    s5_remove_blocks(s5_node);  
    vunlock(file); 
}

/*
 * Wrapper around mobj_get_pframe. Remember to lock the memory object around
 * the call to mobj_get_pframe. Assert that the get_pframe does not fail.
 */
inline void s5_get_disk_block(s5fs_t *s5fs, blocknum_t blocknum, long forwrite,
                              pframe_t **pfp)
{
    mobj_lock(S5FS_TO_VMOBJ(s5fs));
    long ret = mobj_get_pframe(S5FS_TO_VMOBJ(s5fs), blocknum, forwrite, pfp);
    mobj_unlock(S5FS_TO_VMOBJ(s5fs));
    KASSERT(!ret && *pfp);
}

/* Wrapper around pframe_release.
 *
 * Note: All pframe_release does is unlock the pframe. Why aren't we actually
 * writing anything back yet? Because the pframe remains associated with
 * whatever mobj we provided when we originally called mobj_get_pframe. If
 * anyone tries to access the pframe later, Weenix will just give them the
 * cached page frame from the mobj. If the pframe is ever freed (most likely on
 * shutdown), then it will be written back to disk: mobj_flush_pframe ->
 * blockdev_flush_pframe.
 */
inline void s5_release_disk_block(pframe_t **pfp) { pframe_release(pfp); }

/*
 * This is where the abstraction of vnode file block/page --> disk block is
 * finally implemented. Check that the requested page lies within vnode->vn_len.
 *
 * Of course, you will want to use s5_file_block_to_disk_block. Pay attention
 * to what the forwrite argument to s5fs_get_pframe means for the alloc argument
 * in s5_file_block_to_disk_block.
 *
 * If the disk block for the corresponding file block is sparse, you should use
 * mobj_default_get_pframe on the vnode's own memory object. This will trickle
 * down to s5fs_fill_pframe if the pframe is not already resident.
 *
 * Otherwise, if the disk block is NOT sparse, you will want to simply use
 * s5_get_disk_block. NOTE: in this case, you also need to make sure you free
 * the pframe that resides in the vnode itself for the requested pagenum. To
 * do so, you will want to use mobj_find_pframe and mobj_free_pframe.
 *
 * Given the above design, we s5fs itself does not need to implement
 * flush_pframe. Any pframe that will be written to (forwrite = 1) should always
 * have a disk block backing it on successful return. Thus, the page frame will
 * reside in the block device of the filesystem, where the flush_pframe is
 * already implemented. We do, however, need to implement fill_pframe for sparse
 * blocks.
 */
static long s5fs_get_pframe(vnode_t *vnode, uint64_t pagenum, long forwrite,
                            pframe_t **pfp)
{
    if (vnode->vn_len <= pagenum * PAGE_SIZE)
        return -EINVAL;
    long loc =
        s5_file_block_to_disk_block(VNODE_TO_S5NODE(vnode), pagenum, forwrite);
    if (loc < 0)
        return loc;
    if (loc)
    {
        mobj_find_pframe(&vnode->vn_mobj, pagenum, pfp);
        if (*pfp)
        {
            mobj_free_pframe(&vnode->vn_mobj, pfp);
        }
        s5_get_disk_block(VNODE_TO_S5FS(vnode), (blocknum_t)loc, forwrite, pfp);
        return 0;
    }
    else
    {
        KASSERT(!forwrite);
        return mobj_default_get_pframe(&vnode->vn_mobj, pagenum, forwrite, pfp);
    }
}

/*
 * According the documentation for s5fs_get_pframe, this only gets called when
 * the file block for a given page number is sparse. In other words, pf
 * corresponds to a sparse block.
 */
static long s5fs_fill_pframe(vnode_t *vnode, pframe_t *pf)
{
    memset(pf->pf_addr, 0, PAGE_SIZE);
    return 0;
}

/*
 * Verify the superblock. 0 on success; -1 on failure.
 */
static long s5_check_super(s5_super_t *super)
{
    if (!(super->s5s_magic == S5_MAGIC &&
          (super->s5s_free_inode < super->s5s_num_inodes ||
           super->s5s_free_inode == (uint32_t)-1) &&
          super->s5s_root_inode < super->s5s_num_inodes))
    {
        return -1;
    }
    if (super->s5s_version != S5_CURRENT_VERSION)
    {
        dbg(DBG_PRINT,
            "Filesystem is version %d; "
            "only version %d is supported.\n",
            super->s5s_version, S5_CURRENT_VERSION);
        return -1;
    }
    return 0;
}

/*
 * Calculate refcounts on the filesystem.
 */
static void calculate_refcounts(int *counts, vnode_t *vnode)
{
    long ret;

    size_t pos = 0;
    dirent_t dirent;
    vnode_t *child;

    while ((ret = s5fs_readdir(vnode, pos, &dirent)) > 0)
    {
        counts[dirent.d_ino]++;
        dbg(DBG_S5FS, "incrementing count of inode %d to %d\n", dirent.d_ino,
            counts[dirent.d_ino]);
        if (counts[dirent.d_ino] == 1)
        {
            child = vget_locked(vnode->vn_fs, dirent.d_ino);
            if (S_ISDIR(child->vn_mode))
            {
                calculate_refcounts(counts, child);
            }
            vput_locked(&child);
        }
        pos += ret;
    }

    KASSERT(!ret);
}

/*
 * Verify refcounts on the filesystem. 0 on success; -1 on failure.
 */
long s5fs_check_refcounts(fs_t *fs)
{
    s5fs_t *s5fs = (s5fs_t *)fs->fs_i;
    int *refcounts;
    long ret = 0;

    refcounts = kmalloc(s5fs->s5f_super.s5s_num_inodes * sizeof(int));
    KASSERT(refcounts);
    memset(refcounts, 0, s5fs->s5f_super.s5s_num_inodes * sizeof(int));

    vlock(fs->fs_root);
    refcounts[fs->fs_root->vn_vno]++;
    calculate_refcounts(refcounts, fs->fs_root);
    refcounts[fs->fs_root->vn_vno]--;

    vunlock(fs->fs_root);

    dbg(DBG_PRINT,
        "Checking refcounts of s5fs filesystem on block "
        "device with major %d, minor %d\n",
        MAJOR(s5fs->s5f_bdev->bd_id), MINOR(s5fs->s5f_bdev->bd_id));

    for (uint32_t i = 0; i < s5fs->s5f_super.s5s_num_inodes; i++)
    {
        if (!refcounts[i])
        {
            continue;
        }

        vnode_t *vn = vget(fs, i);
        KASSERT(vn);
        s5_node_t *sn = VNODE_TO_S5NODE(vn);

        if (refcounts[i] != sn->inode.s5_linkcount)
        {
            dbg(DBG_PRINT, "   Inode %d, expecting %d, found %d\n", i,
                refcounts[i], sn->inode.s5_linkcount);
            ret = -1;
        }
        vput(&vn);
    }

    dbg(DBG_PRINT,
        "Refcount check of s5fs filesystem on block "
        "device with major %d, minor %d completed %s.\n",
        MAJOR(s5fs->s5f_bdev->bd_id), MINOR(s5fs->s5f_bdev->bd_id),
        (ret ? "UNSUCCESSFULLY" : "successfully"));

    kfree(refcounts);
    return ret;
}
