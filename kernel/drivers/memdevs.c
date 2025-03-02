#include "errno.h"
#include "globals.h"

#include "util/debug.h"
#include "util/string.h"

#include "mm/kmalloc.h"
#include "mm/mobj.h"

#include "drivers/chardev.h"

#include "vm/anon.h"

#include "fs/vnode.h"

static ssize_t null_read(chardev_t *dev, size_t pos, void *buf, size_t count);

static ssize_t null_write(chardev_t *dev, size_t pos, const void *buf,
                          size_t count);

static ssize_t zero_read(chardev_t *dev, size_t pos, void *buf, size_t count);

static long zero_mmap(vnode_t *file, mobj_t **ret);

chardev_ops_t null_dev_ops = {.read = null_read,
                              .write = null_write,
                              .mmap = NULL,
                              .fill_pframe = NULL,
                              .flush_pframe = NULL};

chardev_ops_t zero_dev_ops = {.read = zero_read,
                              .write = null_write,
                              .mmap = zero_mmap,
                              .fill_pframe = NULL,
                              .flush_pframe = NULL};

/**
 * The char device code needs to know about these mem devices, so create
 * chardev_t's for null and zero, fill them in, and register them.
 *
 * Use kmalloc, MEM_NULL_DEVID, MEM_ZERO_DEVID, and chardev_register.
 * See dev.h for device ids to use with MKDEVID.
 */
void memdevs_init()
{
    chardev_t* null_dev = kmalloc(sizeof(chardev_t)); // Malloc a space for null
    null_dev->cd_id = MEM_NULL_DEVID;
    null_dev->cd_ops = &null_dev_ops;
    list_link_init(&null_dev->cd_link);
    chardev_register(null_dev);

    chardev_t* zero_dev=kmalloc(sizeof(chardev_t)); // Malloc a space for 0
    zero_dev->cd_id=MEM_ZERO_DEVID;
    zero_dev->cd_ops=&zero_dev_ops;
    list_link_init(&zero_dev->cd_link);
    chardev_register(zero_dev);
    // NOT_YET_IMPLEMENTED("DRIVERS: memdevs_init");
}

/**
 * Reads a given number of bytes from the null device into a
 * buffer. Any read performed on the null device should read 0 bytes.
 *
 * @param  dev   the null device
 * @param  pos   the offset to read from; should be ignored
 * @param  buf   the buffer to read into
 * @param  count the maximum number of bytes to read
 * @return       the number of bytes read, which should be 0
 */
static ssize_t null_read(chardev_t *dev, size_t pos, void *buf, size_t count)
{
    // TODO: Do we need to set priority here
    //ldisc_wait_read(&cd_to_tty(dev)->tty_ldisc,&cd_to_tty(dev)->tty_lock);
    // NOT_YET_IMPLEMENTED("DRIVERS: null_read");
    return 0;
    // return -ENOMEM;
}

/**
 * Writes a given number of bytes to the null device from a
 * buffer. Writing to the null device should _ALWAYS_ be successful
 * and write the maximum number of bytes.
 *
 * @param  dev   the null device
 * @param  pos   offset the offset to write to; should be ignored
 * @param  buf   buffer to read from
 * @param  count the maximum number of bytes to write
 * @return       the number of bytes written, which should be `count`
 */
static ssize_t null_write(chardev_t *dev, size_t pos, const void *buf,
                          size_t count)
{
    //int written_bytes=0;
    //written_bytes=vterminal_write(&cd_to_tty(dev)->tty_vterminal,buf,count);
    // NOT_YET_IMPLEMENTED("DRIVERS: null_write");
    return count;
}

/**
 * Reads a given number of bytes from the zero device into a
 * buffer. Any read from the zero device should be a series of zeros.
 *
 * @param  dev   the zero device
 * @param  pos   the offset to start reading from; should be ignored
 * @param  buf   the buffer to write to
 * @param  count the maximum number of bytes to read
 * @return       the number of bytes read. Hint: should always read the maximum
 *               number of bytes
 */
static ssize_t zero_read(chardev_t *dev, size_t pos, void *buf, size_t count)
{
    //ldisc_wait_read(&cd_to_tty(dev)->tty_ldisc,&cd_to_tty(dev)->tty_lock);
    memset(buf,'\0',count);
    //int cur_count=0;
    //cur_count=ldisc_read(&cd_to_tty(dev)->tty_ldisc, buf, count);
    // NOT_YET_IMPLEMENTED("DRIVERS: zero_read");
    return count;
}

/**
 * Unlike in s5fs_mmap(), you can't necessarily use the file's underlying mobj.
 * Instead, you should simply provide an anonymous object to ret.
 */
static long zero_mmap(vnode_t *file, mobj_t **ret)
{
    //*ret=&file->vn_mobj;
    mobj_t* new_anon=anon_create();
    if(new_anon==NULL){
        return -ENOMEM;
    }
    *ret=new_anon;
    mobj_unlock(new_anon);
    // NOT_YET_IMPLEMENTED("VM: zero_mmap");
    return 0;
}
