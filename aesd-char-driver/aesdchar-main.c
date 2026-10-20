/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include "aesdchar.h"
#include "aesd_ioctl.h"
#include <linux/slab.h>
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Salahddine GHANEM"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

static size_t aesd_circular_buffer_add(const char *data, size_t size);
static int aesd_circular_buffer_clear(void);
static loff_t aesd_char_adjust_file_offset(struct aesd_dev *dev, struct aesd_seekto *seekto);
int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    /* set the device file private data to the device structure */
    struct aesd_dev *dev;

    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;
    /* is ressource allocation needed? */
    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,loff_t *f_pos)
{
    ssize_t retval = 0;
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    struct aesd_dev *dev = filp->private_data;

     if (mutex_lock_interruptible(&dev->lock))
     {
        PDEBUG("read operation interrupted by signal");
         retval = -ERESTARTSYS;
         goto out;
     } 

    /* find the entry corresponding to the file position */
    size_t entry_offset_byte_rtn;
    struct aesd_buffer_entry *entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, *f_pos, &entry_offset_byte_rtn);
    if (entry == NULL) {
        PDEBUG("no entry found for offset %lld", *f_pos);
        retval = 0; // EOF
        goto out_unlock;
    }
    /* calculate the number of bytes to read */
    size_t bytes_to_read = entry->size - entry_offset_byte_rtn;
    size_t bytes_to_copy = min(bytes_to_read, count);

    /* copy the data to userspace */
    if (copy_to_user(buf, entry->buffptr + entry_offset_byte_rtn, bytes_to_copy)) {
        PDEBUG("failed to copy data to userspace");
        retval = -EFAULT;
        goto out_unlock;
    }
    /* update the file position */
    *f_pos += bytes_to_copy;
    PDEBUG("read %zu bytes from entry with size %zu at offset %zu", bytes_to_copy, entry->size, entry_offset_byte_rtn);
    PDEBUG("new file position is %lld", *f_pos);
    retval = bytes_to_copy;

    out_unlock:
    mutex_unlock(&dev->lock);

    out:
    PDEBUG("read returning %zd bytes with offset %lld", retval, *f_pos);    

    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf,size_t count, loff_t *f_pos)
{
    struct aesd_dev *dev = filp->private_data;
    char *kbuf = NULL;
    ssize_t retval;

    PDEBUG("write %zu bytes with offset %lld", count, *f_pos);

    /* 1. Acquire the device lock */
    if (mutex_lock_interruptible(&dev->lock)) {
        PDEBUG("write operation interrupted by signal");
        return -ERESTARTSYS;
    }

    /* 2. Allocate kernel buffer */
    kbuf = kmalloc(count, GFP_KERNEL);
    if (!kbuf) {
        PDEBUG("failed to allocate kernel buffer");
        retval = -ENOMEM;
        goto out_unlock;
    }

    /* 3. Copy data from userspace */
    if (copy_from_user(kbuf, buf, count)) {
        PDEBUG("failed to copy data from userspace");
        retval = -EFAULT;
        goto out_free;
    }

    /* 4. Process the data */
    retval = aesd_circular_buffer_add(kbuf, count);

    if (retval < 0) {
        PDEBUG("failed to add data to circular buffer");
        goto out_free;
    }

    /* 5. Successful write */
    PDEBUG("successfully processed %zd bytes", retval);
    PDEBUG("new file position is %d", *f_pos + retval);
   *f_pos += retval;

out_free:
    kfree(kbuf);

out_unlock:
    mutex_unlock(&dev->lock);

    return retval;
}

loff_t   aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    struct aesd_dev *dev = filp->private_data;
    size_t buffer_size = aesd_circular_buffer_size(&dev->buffer);
    loff_t  new_pos = 0;

    PDEBUG("llseek from %lld to %lld", filp->f_pos, offset);

    if (mutex_lock_interruptible(&dev->lock)) {
        PDEBUG("llseek operation interrupted by signal");
        return -ERESTARTSYS;
    }

    new_pos =  fixed_size_llseek(filp, offset, whence, buffer_size);


    if (new_pos < 0 || new_pos > buffer_size) {
        mutex_unlock(&dev->lock);
        PDEBUG("llseek to invalid position %lld", new_pos);
        return -EINVAL;
    }

    PDEBUG("llseek to new position %lld", new_pos);

    filp->f_pos = new_pos;

    mutex_unlock(&dev->lock);
    return new_pos;
}

long int aesd_ioctl(struct file *filp, unsigned int request,long unsigned int arg)
{
    struct aesd_dev *dev = filp->private_data;
    struct aesd_seekto *seekto = (struct aesd_seekto *)arg;
    struct aesd_seekto seekto_copy;
    loff_t new_pos = -EINVAL;
    int ret = 0;

    PDEBUG("ioctl request %u", request);

    if (mutex_lock_interruptible(&dev->lock)) {
        PDEBUG("ioctl operation interrupted by signal");
        return -ERESTARTSYS;
    }

    switch (request) {
        case AESDCHAR_IOCSEEKTO:
            if (seekto == NULL) {
                ret = -EINVAL;
                break;
            }
            /* Validate the seekto parameters */
            if (copy_from_user(&seekto_copy, seekto, sizeof(struct aesd_seekto))) {
                PDEBUG("failed to copy seekto structure from userspace");
                ret = -EFAULT;
                break;
            }
            else
            {   /* Calculate the new file position */
                 new_pos = aesd_char_adjust_file_offset(dev, &seekto_copy);
                 if (new_pos < 0) {
                    PDEBUG("invalid seekto parameters: write_cmd=%u, write_cmd_offset=%u", seekto_copy.write_cmd, seekto_copy.write_cmd_offset);
                    ret = -EINVAL;
                    break;
                 }
            }
            /* Set the new file position */
            PDEBUG("ioctl set file position to %lld", filp->f_pos);
            filp->f_pos = new_pos;
            break;
        default:
            ret = -ENOTTY; // Command not supported
            break;
    }

    mutex_unlock(&dev->lock);
    return ret;
}

/** @brief Adjusts the file offset based on the seekto parameters, 
 *         new pos is calculated based on logical out_off index and not physical index in the circular buffer.
 *  @param dev pointer to the AESD device structure
 *  @param seekto pointer to the seekto structure
 *  @return the adjusted file offset or an error code
 */
static loff_t aesd_char_adjust_file_offset(struct aesd_dev *dev, struct aesd_seekto *seekto)
{
    size_t cumulative_size = 0;
    uint8_t index = dev->buffer.out_offs;
    size_t entries_checked = 0;

    while (entries_checked < AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED) {
        
        if (!dev->buffer.full && index == dev->buffer.in_offs) {
            break;
        }

        if (entries_checked == seekto->write_cmd) {
            if (seekto->write_cmd_offset > dev->buffer.entry[index].size) {
                return -EINVAL; // Invalid offset
            }
            return cumulative_size + seekto->write_cmd_offset;
        }

        cumulative_size += dev->buffer.entry[index].size;
        index = (index + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
        entries_checked++;
    }

    return -EINVAL; // write_cmd not found
}

struct file_operations aesd_fops = {
    .owner          =    THIS_MODULE,
    .read           =     aesd_read,
    .write          =    aesd_write,
    .open           =     aesd_open,
    .release        =  aesd_release,
    .llseek         =   aesd_llseek,
    .unlocked_ioctl =    aesd_ioctl,
};

static int aesd_circular_buffer_clear(void)
{
    struct aesd_dev *dev = &aesd_device;
    int i;

    for (i = 0; i < AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED; i++) {
        if (dev->buffer.entry[i].buffptr) {
            kfree(dev->buffer.entry[i].buffptr);
            dev->buffer.entry[i].buffptr = NULL;
            dev->buffer.entry[i].size = 0;
        }
    }

    dev->buffer.in_offs = 0;
    dev->buffer.out_offs = 0;
    dev->buffer.full = false;

    return 0;
}

/** @brief this function check for newline character in the buffer and add the entry to the circular buffer if found, 
 * otherwise it will append the data to the current entry. 
 *  @param data pointer to the data to be added.
 *  @param size size of the data to be added.
 *  @return the total size of the data added to the circular buffer.
 */
static size_t aesd_circular_buffer_add(const char *data, size_t size)
{
    bool newline_found = false;
    size_t data_size   = size;

    if (data == NULL || size == 0) {
        return 0;
    }
    /*0: append new data to temporary buffer */

    /*1: check for newline character */
    for (size_t i = 0; i < size; i++) {
        if (data[i] == '\n') {
            /* process the entry up to the newline  & ignore data after newline */
             newline_found = true;
             data_size = i + 1; /* include the newline character in the entry */
             break;
        }

    }
    /* add data to the temporary buffer in all cases: require a kernel allocation */     
    if (aesd_device.temp_buffer_prefilled) {
        
        char *new_temp_buffer = krealloc(aesd_device.temp_buffer, aesd_device.temp_buffer_size + data_size, GFP_KERNEL);

        if (!new_temp_buffer) {
            PDEBUG("failed to allocate memory for temporary buffer");
            return 0;
        }

        aesd_device.temp_buffer = new_temp_buffer;
        /* Append the new data to the temporary buffer */
        memcpy(aesd_device.temp_buffer + (aesd_device.temp_buffer_size*sizeof(char)), data, data_size);
        aesd_device.temp_buffer_size += data_size;

    } else {
        aesd_device.temp_buffer = kmalloc(data_size, GFP_KERNEL);
        if (!aesd_device.temp_buffer) {
            PDEBUG("failed to allocate memory for temporary buffer");
            return 0;
        }
        memcpy(aesd_device.temp_buffer, data, data_size);
        aesd_device.temp_buffer_size = data_size;
        aesd_device.temp_buffer_prefilled = true;
    }

    if(newline_found) {
        /*2: add the entry to the circular buffer taken into account the temporary buffer */
        struct aesd_buffer_entry new_entry;
        new_entry.size = aesd_device.temp_buffer_size;

        new_entry.buffptr = kmalloc(new_entry.size, GFP_KERNEL);

        if (!new_entry.buffptr) {
            /* cleanup the temporary buffer */
            kfree(aesd_device.temp_buffer);
            aesd_device.temp_buffer = NULL;
            aesd_device.temp_buffer_size = 0;
            aesd_device.temp_buffer_prefilled = false;
            PDEBUG("failed to allocate memory for new entry");
            return 0;
        }
        memcpy(new_entry.buffptr, aesd_device.temp_buffer, new_entry.size);

        const char* deleted_entry = aesd_circular_buffer_add_entry(&aesd_device.buffer, &new_entry);
        /* cleanup the deleted entry if it exists  & temporary buffer */
        if (deleted_entry) {
            kfree(deleted_entry);
        }
        kfree(aesd_device.temp_buffer);
        aesd_device.temp_buffer = NULL;
        aesd_device.temp_buffer_size = 0;   
        aesd_device.temp_buffer_prefilled = false;
    }

    return data_size;
}

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}

int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,"aesdchar");
    aesd_major = MAJOR(dev);

    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));
    /* initialize the ring buffer */
    aesd_circular_buffer_init(&aesd_device.buffer);
    /* initialize the mutex */
    mutex_init(&aesd_device.lock);


    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    aesd_circular_buffer_clear();
    cdev_del(&aesd_device.cdev);

    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
