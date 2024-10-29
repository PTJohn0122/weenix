#include "fs/vfs_syscall.h"
#include "errno.h"
#include "fs/fcntl.h"
#include "fs/file.h"
#include "fs/lseek.h"
#include "fs/vfs.h"
#include "fs/vnode.h"
#include "globals.h"
#include "kernel.h"
#include "util/debug.h"
#include "util/string.h"
#include <limits.h>

/*
 * Read len bytes into buf from the fd's file using the file's vnode operation
 * read.
 *
 * Return the number of bytes read on success, or:
 *  - EBADF: fd is invalid or is not open for reading
 *  - EISDIR: fd refers to a directory
 *  - Propagate errors from the vnode operation read
 *
 * Hints:
 *  - Be sure to update the file's position appropriately.
 *  - Lock/unlock the file's vnode when calling its read operation.
 */
ssize_t do_read(int fd, void *buf, size_t len)
{
    if(fd<0 || fd>=NFILES || curproc->p_files[fd]==NULL|| !(curproc->p_files[fd]->f_mode&FMODE_READ)){ // If fd is invalid or it's not open for reading
        return -EBADF;
    }
    if(S_ISDIR(curproc->p_files[fd]->f_vnode->vn_mode)){ // EISDIR: fd refers to a directory
        return -EISDIR;
    }
    vlock(curproc->p_files[fd]->f_vnode);
    file_t *tmp_file= fget(fd);
    ssize_t tmp=curproc->p_files[fd]->f_vnode->vn_ops->read(curproc->p_files[fd]->f_vnode,
    curproc->p_files[fd]->f_pos,buf,len);
    curproc->p_files[fd]->f_pos+=tmp; // Update position
    vunlock(curproc->p_files[fd]->f_vnode);
    fput(&tmp_file);
    // NOT_YET_IMPLEMENTED("VFS: do_read");
    return tmp;
}

/*
 * Write len bytes from buf into the fd's file using the file's vnode operation
 * write.
 *
 * Return the number of bytes written on success, or:
 *  - EBADF: fd is invalid or is not open for writing
 *  - Propagate errors from the vnode operation read
 *
 * Hints:
 *  - Check out `man 2 write` for details about how to handle the FMODE_APPEND
 *    flag.
 *  - Be sure to update the file's position appropriately.
 *  - Lock/unlock the file's vnode when calling its write operation.
 */
ssize_t do_write(int fd, const void *buf, size_t len)
{
    if(fd<0 || fd>=NFILES || curproc->p_files[fd]==NULL || !(curproc->p_files[fd]->f_mode&FMODE_WRITE)){ // If fd is invalid or it's not open for reading
        return -EBADF;
    }
    if((curproc->p_files[fd]->f_mode&FMODE_APPEND)){
        curproc->p_files[fd]->f_pos=curproc->p_files[fd]->f_vnode->vn_len; 
        // Set the position to the end of file, which should be the current size of vnode
    }
    vlock(curproc->p_files[fd]->f_vnode);
    file_t *tmp_file= fget(fd);
    ssize_t tmp=curproc->p_files[fd]->f_vnode->vn_ops->write(curproc->p_files[fd]->f_vnode,
    curproc->p_files[fd]->f_pos,buf,len);
    curproc->p_files[fd]->f_pos+=tmp; // Update position
    vunlock(curproc->p_files[fd]->f_vnode);
    fput(&tmp_file);
    // NOT_YET_IMPLEMENTED("VFS: do_write");
    return tmp;
}

/*
 * Close the file descriptor fd.
 *
 * Return 0 on success, or:
 *  - EBADF: fd is invalid or not open
 * 
 * Hints: 
 * Check `proc.h` to see if there are any helpful fields in the 
 * proc_t struct for checking if the file associated with the fd is open. 
 * Consider what happens when we open a file and what counts as closing it
 */
long do_close(int fd)
{
    if(fd<0 || fd>=NFILES || curproc->p_files[fd]==NULL){ // If fd is invalid or it's not open
        return -EBADF;
    }
    //curproc->p_files[fd]->f_refcount=0; // Set the reference count as 0
    if(curproc->p_files[fd]){
        fput(&curproc->p_files[fd]);
    }
    // NOT_YET_IMPLEMENTED("VFS: do_close");
    return 0;
}

/*
 * Duplicate the file descriptor fd.
 *
 * Return the new file descriptor on success, or:
 *  - EBADF: fd is invalid or not open
 *  - Propagate errors from get_empty_fd()
 *
 * Hint: Use get_empty_fd() to obtain an available file descriptor.
 */
long do_dup(int fd)
{
    if(fd<0 || fd>=NFILES || curproc->p_files[fd]==NULL){ // If fd is invalid or it's not open
        return -EBADF;
    }
    int old_fd=fd;
    long if_success=get_empty_fd(&fd);
    if(if_success<0)    {return if_success;}
    curproc->p_files[fd]=curproc->p_files[old_fd];
    fref(curproc->p_files[fd]);
    // NOT_YET_IMPLEMENTED("VFS: do_dup");
    return fd;
}

/*
 * Duplicate the file descriptor ofd using the new file descriptor nfd. If nfd
 * was previously open, close it.
 *
 * Return nfd on success, or:
 *  - EBADF: ofd is invalid or not open, or nfd is invalid
 *
 * Hint: You don't need to do anything if ofd and nfd are the same.
 * (If supporting MTP, this action must be atomic)
 */
long do_dup2(int ofd, int nfd)
{
    if(ofd<0 || ofd>=NFILES || curproc->p_files[ofd]==NULL|| nfd<0 || nfd>=NFILES){ 
        // If ofd is invalid or not open, or nfd is invalid
        return -EBADF;
    }
    if(ofd!=nfd){
        if(curproc->p_files[nfd]!=NULL){
            do_close(nfd); // If nfd was previously open, close it
        }
        curproc->p_files[nfd]=curproc->p_files[ofd];
        fref(curproc->p_files[nfd]);
    }
    // NOT_YET_IMPLEMENTED("VFS: do_dup2");
    return nfd;
}

/*
 * Create a file specified by mode and devid at the location specified by path.
 *
 * Return 0 on success, or:
 *  - EINVAL: Mode is not S_IFCHR, S_IFBLK, or S_IFREG
 *  - Propagate errors from namev_open()
 *
 * Hints:
 *  - Create the file by calling namev_open() with the O_CREAT flag.
 *  - Be careful about refcounts after calling namev_open(). The newly created 
 *    vnode should have no references when do_mknod returns. The underlying 
 *    filesystem is responsible for maintaining references to the inode, which 
 *    will prevent it from being destroyed, even if the corresponding vnode is 
 *    cleaned up.
 *  - You don't need to handle EEXIST (this would be handled within namev_open, 
 *    but doing so would likely cause problems elsewhere)
 */
long do_mknod(const char *path, int mode, devid_t devid)
{
    if(!(mode&S_IFCHR)&&!(mode&S_IFBLK)&&!(mode&S_IFREG)){
        return -EINVAL; // Mode is not S_IFCHR, S_IFBLK, or S_IFREG
    }
    vnode_t *res_vnode; 
    long tmp=namev_open(curproc->p_cwd,path,O_CREAT,mode,devid,&res_vnode); // Create the file
    if (tmp < 0) {
        return tmp;
    }
    vput(&res_vnode);   // The newly created vnode should have no references
    // NOT_YET_IMPLEMENTED("VFS: do_mknod");
    return tmp;
}

/*
 * Create a directory at the location specified by path.
 *
 * Return 0 on success, or:
 *  - ENAMETOOLONG: The last component of path is too long
 *  - ENOTDIR: The parent of the directory to be created is not a directory
 *  - EEXIST: A file located at path already exists
 *  - Propagate errors from namev_dir(), namev_lookup(), and the vnode
 *    operation mkdir
 *
 * Hints:
 * 1) Use namev_dir() to find the parent of the directory to be created.
 * 2) Use namev_lookup() to check that the directory does not already exist.
 * 3) Use the vnode operation mkdir to create the directory.
 *  - Compare against NAME_LEN to determine if the basename is too long.
 *    Check out ramfs_mkdir() to confirm that the basename will be null-
 *    terminated.
 *  - Be careful about locking and refcounts after calling namev_dir() and
 *    namev_lookup().
 */
long do_mkdir(const char *path)
{
    vnode_t *res_vnode;
    const char** name=&path;
    size_t namelen=0; // Initialize namelen 
    long tmp=namev_dir(curproc->p_cwd,path,&res_vnode,name,&namelen);
    if(tmp<0)   {  return tmp; }
    if(namelen>NAME_LEN){ // If the last component of the path is too long
        vput(&res_vnode);
        return -ENAMETOOLONG;
    }
    if(!S_ISDIR(res_vnode->vn_mode)){ // If the parent of the directory to be created is not a directory
        vput(&res_vnode);
        return -ENOTDIR;
    }
    vnode_t *res_vnode2; // The parent vnode of the directory to be created
    vlock(res_vnode);
    long tmp2=namev_lookup(res_vnode,*name,namelen,&res_vnode2); // Look up the vnode for the basename
    vunlock(res_vnode);
    if(tmp2==0){ // If a file already exist, we don't need to make directory
        vput(&res_vnode);
        vput(&res_vnode2);  // Decrement the ref count of res_vnode2 added in namev_lookup
        return -EEXIST;
    }   
    vnode_t *res_vnode3;
    vlock(res_vnode);
    long tmp3=res_vnode->vn_ops->mkdir(res_vnode,*name,namelen,&res_vnode3); // Create the directory
    vunlock(res_vnode);
    vput(&res_vnode); // We don't need to use res_vnode any more
    if (tmp3 == 0) {
        vput(&res_vnode3);
    }
    
    // if(tmp3<0)  { return tmp3; }
    // TODO: How to check out ramfs_mkdir()
    // NOT_YET_IMPLEMENTED("VFS: do_mkdir");
    return tmp3;
}

/*
 * Delete a directory at path.
 *
 * Return 0 on success, or:
 *  - EINVAL: Attempting to rmdir with "." as the final component
 *  - ENOTEMPTY: Attempting to rmdir with ".." as the final component
 *  - ENOTDIR: The parent of the directory to be removed is not a directory
 *  - ENAMETOOLONG: the last component of path is too long
 *  - Propagate errors from namev_dir() and the vnode operation rmdir
 *
 * Hints:
 *  - Use namev_dir() to find the parent of the directory to be removed.
 *  - Be careful about refcounts from calling namev_dir().
 *  - Use the parent directory's rmdir operation to remove the directory.
 *  - Lock/unlock the vnode when calling its rmdir operation.
 */
long do_rmdir(const char *path)
{
    vnode_t *res_vnode;
    const char **name=&path;
    size_t namelen=0;
    long tmp=namev_dir(curproc->p_cwd,path,&res_vnode,name,&namelen);
    // The ref count for res_vnode will increment
    if(tmp<0)   { return tmp; } // The error from namev_dir 
    if(namelen>NAME_LEN)   {    // If the last component is too long
        vput(&res_vnode);
        return -ENAMETOOLONG;
    } 
    if(!S_ISDIR(res_vnode->vn_mode))  { // If it is not a directory
        vput(&res_vnode);
        return -ENOTDIR;
    } 
    if(**name=='.'&&namelen==1)   {
        vput(&res_vnode);
        return -EINVAL;
    }
    const char *two_dots="..";
    if(strcmp(*name,two_dots)==0 &&namelen==2)  { // If name is equal to ".." and the namelen is 2
        vput(&res_vnode);
        return -ENOTEMPTY;
    }
    vlock(res_vnode);
    long tmp2=res_vnode->vn_ops->rmdir(res_vnode,*name,namelen); // Use remove directory
    vunlock(res_vnode);
    vput(&res_vnode);
    // TODO: Check the refcounts
    // NOT_YET_IMPLEMENTED("VFS: do_rmdir");
    return tmp2;
}

/*
 * Remove the link between path and the file it refers to.
 *
 * Return 0 on success, or:
 *  - ENOTDIR: the parent of the file to be unlinked is not a directory
 *  - EPERM: the file to be unlinked is a directory 
 *  - ENAMETOOLONG: the last component of path is too long
 *  - Propagate errors from namev_dir(), namev_lookup(), and the vnode operation unlink
 *
 * Hints:
 *  - Use namev_dir() and be careful about refcounts.
 *  - Use namev_lookup() to get the vnode for the file to be unlinked. 
 *  - Lock/unlock the parent directory when calling its unlink operation.
 */
long do_unlink(const char *path)
{
    vnode_t *res_vnode;
    const char **name=&path;
    size_t namelen=0;
    long tmp=namev_dir(curproc->p_cwd,path,&res_vnode,name,&namelen);
    if(tmp<0){ return tmp; }
    if(!S_ISDIR(res_vnode->vn_mode))  {
        vput(&res_vnode);
        return -ENOTDIR;
    } // If it is not a directory
    if(namelen>NAME_LEN)    {    
        vput(&res_vnode);   
        return -ENAMETOOLONG;
    } // If the last component is too long
    vnode_t *res_vnode2;
    vlock(res_vnode);
    long tmp2=namev_lookup(res_vnode,*name,namelen,&res_vnode2); // Find the vnode we need to unlink
    vunlock(res_vnode);
    if(tmp2<0)      {
        vput(&res_vnode);
         return tmp2; 
    } // If we didn't find this vnode
    if(S_ISDIR(res_vnode2->vn_mode)) { // If the file to be unlinked is directory
        vput(&res_vnode);
        vput(&res_vnode2);
        return -EPERM;
    }
    vlock(res_vnode);
    long tmp3=res_vnode->vn_ops->unlink(res_vnode,*name,namelen);
    // TODO: Check it later
    vunlock(res_vnode);
    vput(&res_vnode2);
    vput(&res_vnode);
    // NOT_YET_IMPLEMENTED("VFS: do_unlink");
    return 0;
}

/*
 * Create a hard link newpath that refers to the same file as oldpath.
 *
 * Return 0 on success, or:
 *  - EPERM: oldpath refers to a directory
 *  - ENAMETOOLONG: The last component of newpath is too long
 *  - ENOTDIR: The parent of the file to be linked is not a directory
 *
 * Hints:
 * 1) Use namev_resolve() on oldpath to get the target vnode.
 * 2) Use namev_dir() on newpath to get the directory vnode.
 * 3) Use vlock_in_order() to lock the directory and target vnodes.
 * 4) Use the directory vnode's link operation to create a link to the target.
 * 5) Use vunlock_in_order() to unlock the vnodes.
 * 6) Make sure to clean up references added from calling namev_resolve() and
 *    namev_dir().
 */
long do_link(const char *oldpath, const char *newpath)
{
    vnode_t *res_vnode;
    const char **name=&newpath;
    size_t namelen=0;
    long tmp=namev_resolve(curproc->p_cwd,oldpath,&res_vnode); // Get the target vnode
    if(tmp<0) {return tmp;}
    if(S_ISDIR(res_vnode->vn_mode))  {
        vput(&res_vnode);
        return -EPERM;
    } // Oldpath refers to a directory
    vnode_t *res_vnode2;
    long tmp2=namev_dir(curproc->p_cwd,newpath,&res_vnode2,name,&namelen);
    if(tmp2<0)  {
        vput(&res_vnode);
        return tmp2;
    }
    vlock_in_order(res_vnode,res_vnode2);
    if(namelen>NAME_LEN)    {  // If the basename is too long
        vunlock_in_order(res_vnode,res_vnode2);
        vput(&res_vnode);
        vput(&res_vnode2);
        return -ENAMETOOLONG;
    }
    if(!S_ISDIR(res_vnode2->vn_mode))  { // If the res_vnode is not a directory
        vunlock_in_order(res_vnode,res_vnode2);
        vput(&res_vnode);
        vput(&res_vnode2);
        return -ENOTDIR;
    } 
    long tmp3=res_vnode2->vn_ops->link(res_vnode2,*name,namelen,res_vnode); // Link the target vnode
    vunlock_in_order(res_vnode,res_vnode2);
    vput(&res_vnode);
    vput(&res_vnode2);
    // NOT_YET_IMPLEMENTED("VFS: do_link");
    return tmp3;
}

/* Rename a file or directory.
 *
 * Return 0 on success, or:
 *  - ENOTDIR: the parent of either path is not a directory
 *  - ENAMETOOLONG: the last component of either path is too long
 *  - Propagate errors from namev_dir() and the vnode operation rename
 *
 * You DO NOT need to support renaming of directories.
 * Steps:
 * 1. namev_dir oldpath --> olddir vnode
 * 2. namev_dir newpath --> newdir vnode
 * 4. Lock the olddir and newdir in ancestor-first order (see `vlock_in_order`)
 * 5. Use the `rename` vnode operation
 * 6. Unlock the olddir and newdir
 * 8. vput the olddir and newdir vnodes
 *
 * Alternatively, you can allow do_rename() to rename directories if
 * __RENAMEDIR__ is set in Config.mk. As with all extra credit
 * projects this is harder and you will get no extra credit (but you
 * will get our admiration). Please make sure the normal version works first.
 * Steps:
 * 1. namev_dir oldpath --> olddir vnode
 * 2. namev_dir newpath --> newdir vnode
 * 3. Lock the global filesystem `vnode_rename_mutex`
 * 4. Lock the olddir and newdir in ancestor-first order (see `vlock_in_order`)
 * 5. Use the `rename` vnode operation
 * 6. Unlock the olddir and newdir
 * 7. Unlock the global filesystem `vnode_rename_mutex`
 * 8. vput the olddir and newdir vnodes
 *
 * P.S. This scheme /probably/ works, but we're not 100% sure.
 */
long do_rename(const char *oldpath, const char *newpath)
{
    vnode_t *oldres_vnode;
    const char **oldname=&oldpath; // Define old name and new name
    size_t oldnamelen=0;
    vnode_t *newres_vnode;
    const char **newname=&newpath; 
    size_t newnamelen=0;
    long tmp1=namev_dir(curproc->p_cwd,oldpath,&oldres_vnode,oldname,&oldnamelen); // Olddir vnode
    if(tmp1<0)  {return tmp1;}
    long tmp2=namev_dir(curproc->p_cwd,newpath,&newres_vnode,newname,&newnamelen); // Newdir vnode
    if(tmp2<0)  {
        vput(&oldres_vnode);
        return tmp2;
    }
    if(!S_ISDIR(oldres_vnode->vn_mode)||!S_ISDIR(newres_vnode->vn_mode)){
        vput(&oldres_vnode);
        vput(&newres_vnode);
        return -ENOTDIR; // If the parent of either path is not a directory
    }
    if(oldnamelen>NAME_LEN||newnamelen>NAME_LEN){ // If the last component of either path is too long
        vput(&oldres_vnode);
        vput(&newres_vnode);
        return -ENAMETOOLONG;
    }
    vlock_in_order(oldres_vnode,newres_vnode); // Lock the two vnodes
    long tmp3=oldres_vnode->vn_ops->rename(oldres_vnode,*oldname,oldnamelen,newres_vnode,*newname,newnamelen);
    vunlock_in_order(oldres_vnode,newres_vnode);
    // if(tmp3<0)  {return tmp3;}
    vput(&oldres_vnode); // vput the olddir and newdir vnodes
    vput(&newres_vnode);
    // NOT_YET_IMPLEMENTED("VFS: do_rename");
    return tmp3;
}

/* Set the current working directory to the directory represented by path.
 *
 * Returns 0 on success, or:
 *  - ENOTDIR: path does not refer to a directory
 *  - Propagate errors from namev_resolve()
 *
 * Hints:
 *  - Use namev_resolve() to get the vnode corresponding to path.
 *  - Pay attention to refcounts!
 *  - Remember that p_cwd should not be locked upon return from this function.
 *  - (If doing MTP, must protect access to p_cwd)
 */
long do_chdir(const char *path)
{
    vnode_t *res_vnode;
    long tmp=namev_resolve(curproc->p_cwd,path,&res_vnode);
    if(tmp<0)   {return tmp;}
    if(!S_ISDIR(res_vnode->vn_mode))  {
        vput(&res_vnode);
        return -ENOTDIR;
    } // If path does not refer to a directory
    vput(&curproc->p_cwd);
    curproc->p_cwd=res_vnode; // Set it to the directory represented by path
    // TODO: In this situation, should we decresase the ref count of res_vnode, because we set it as
    // current work directory and we may use it
    // NOT_YET_IMPLEMENTED("VFS: do_chdir");
    return 0;
}

/*
 * Read a directory entry from the file specified by fd into dirp.
 *
 * Return sizeof(dirent_t) on success, or:
 *  - EBADF: fd is invalid or is not open
 *  - ENOTDIR: fd does not refer to a directory
 *  - Propagate errors from the vnode operation readdir
 *
 * Hints:
 *  - Use the vnode operation readdir.
 *  - Be sure to update file position according to readdir's return value.
 *  - On success (readdir return value is strictly positive), return
 *    sizeof(dirent_t).
 */
ssize_t do_getdent(int fd, struct dirent *dirp)
{
    if(fd<0 || fd>=NFILES || curproc->p_files[fd]==NULL){
        return -EBADF;
    }
    if(!S_ISDIR(curproc->p_files[fd]->f_vnode->vn_mode)){
        return -ENOTDIR;
    }
    file_t *tmp_file=fget(fd);
    vlock(curproc->p_files[fd]->f_vnode);
    ssize_t tmp=curproc->p_files[fd]->f_vnode->vn_ops->readdir(curproc->p_files[fd]->f_vnode,curproc->p_files[fd]->f_pos,dirp);
    vunlock(curproc->p_files[fd]->f_vnode);
    curproc->p_files[fd]->f_pos+=tmp; // Update file position
    fput(&tmp_file);
    //NOT_YET_IMPLEMENTED("VFS: do_getdent");
    if(tmp>0) { 
        return sizeof(struct dirent);
    } else  {   
        return 0;  
    }
    // TODO: Not sure it is correct, because I'm not sure about the retern value from readdir, if it failed,
    // does the return value could be 0?
}

/*
 * Set the position of the file represented by fd according to offset and
 * whence.
 *
 * Return the new file position, or:
 *  - EBADF: fd is invalid or is not open
 *  - EINVAL: whence is not one of SEEK_SET, SEEK_CUR, or SEEK_END;
 *            or, the resulting file offset would be negative
 *
 * Hints:
 *  - See `man 2 lseek` for details about whence.
 *  - Be sure to protect the vnode if you have to access its vn_len.
 */
off_t do_lseek(int fd, off_t offset, int whence)
{
    if(fd<0 || fd>=NFILES || curproc->p_files[fd]==NULL){
        return -EBADF;
    }
    if(whence!=SEEK_SET&&whence!=SEEK_CUR&&whence!=SEEK_END){
        return -EINVAL;
    }

    // vlock(curproc->p_files[fd]->f_vnode);  // Protect the vnode
    // file_t *tmp_file=fget(fd);
    if(whence==SEEK_SET){
        offset=offset;
    } 
    else if(whence==SEEK_CUR){
        offset=curproc->p_files[fd]->f_pos+offset;
    }
    else if(whence==SEEK_END){
        offset=offset+curproc->p_files[fd]->f_vnode->vn_len; // TODO: Maybe the size of p_files[fd] can be represented by f_vnode->vn_len
    }
    // vunlock(curproc->p_files[fd]->f_vnode);
    // fput(&tmp_file);
    if(offset<0)   {return -EINVAL;} // If the resulting offset is negative
    curproc->p_files[fd]->f_pos=offset;
    // NOT_YET_IMPLEMENTED("VFS: do_lseek");
    return offset;
}

/* Use buf to return the status of the file represented by path.
 *
 * Return 0 on success, or:
 *  - Propagate errors from namev_resolve() and the vnode operation stat.
 */
long do_stat(const char *path, stat_t *buf)
{
    vnode_t *res_vnode;
    long tmp1=namev_resolve(curproc->p_cwd,path,&res_vnode);
    if(tmp1<0)   {return tmp1;}
    vlock(res_vnode);
    long tmp2=res_vnode->vn_ops->stat(res_vnode,buf);
    vunlock(res_vnode);
    vput(&res_vnode);
    // if(tmp2<0)  {return tmp2;}
    // NOT_YET_IMPLEMENTED("VFS: do_stat");
    return tmp2;
}

#ifdef __MOUNTING__
/*
 * Implementing this function is not required and strongly discouraged unless
 * you are absolutely sure your Weenix is perfect.
 *
 * This is the syscall entry point into vfs for mounting. You will need to
 * create the fs_t struct and populate its fs_dev and fs_type fields before
 * calling vfs's mountfunc(). mountfunc() will use the fields you populated
 * in order to determine which underlying filesystem's mount function should
 * be run, then it will finish setting up the fs_t struct. At this point you
 * have a fully functioning file system, however it is not mounted on the
 * virtual file system, you will need to call vfs_mount to do this.
 *
 * There are lots of things which can go wrong here. Make sure you have good
 * error handling. Remember the fs_dev and fs_type buffers have limited size
 * so you should not write arbitrary length strings to them.
 */
int do_mount(const char *source, const char *target, const char *type)
{
    NOT_YET_IMPLEMENTED("MOUNTING: do_mount");
    return -EINVAL;
}

/*
 * Implementing this function is not required and strongly discouraged unless
 * you are absolutley sure your Weenix is perfect.
 *
 * This function delegates all of the real work to vfs_umount. You should not
 * worry about freeing the fs_t struct here, that is done in vfs_umount. All
 * this function does is figure out which file system to pass to vfs_umount and
 * do good error checking.
 */
int do_umount(const char *target)
{
    NOT_YET_IMPLEMENTED("MOUNTING: do_unmount");
    return -EINVAL;
}
#endif
