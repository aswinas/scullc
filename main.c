#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything... */
#include <linux/errno.h>	/* error codes */
#include <linux/types.h>	/* size_t */
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <linux/aio.h>
#include <asm/uaccess.h>
#include "scullc.h"		/* local definitions */


int scullc_major;
int scullc_devs = 1;	/* number of bare scullc devices */
int scullc_qset =    500;
int scullc_quantum = 4000;

module_param(scullc_qset, int, 0);
module_param(scullc_quantum, int, 0);
MODULE_AUTHOR("Alessandro Rubini");
MODULE_LICENSE("Dual BSD/GPL");

struct scullc_dev *scullc_devices; /* allocated in scullc_init */

int scullc_trim(struct scullc_dev *dev);
void scullc_cleanup(void);

struct kmem_cache *scullc_cache;

int scullc_open (struct inode *inode, struct file *filp)
{
	struct scullc_dev *dev; /* device information */
	printk("scullc_open\n");

	/*  Find the device */
	dev = container_of(inode->i_cdev, struct scullc_dev, cdev);

    	/* now trim to 0 the length of the device if open was write-only */
	if ( (filp->f_flags & O_ACCMODE) == O_WRONLY) {
		if (down_interruptible (&dev->sem))
			return -ERESTARTSYS;
		scullc_trim(dev); /* ignore errors */
		up (&dev->sem);
	}

	/* and use filp->private_data to point to the device data */
	filp->private_data = dev;

	return 0;          /* success */
}

int scullc_release (struct inode *inode, struct file *filp)
{
	printk("scullc_release\n");
	return 0;
}

/*
 * Follow the list 
 */
struct scullc_dev *scullc_follow(struct scullc_dev *dev, int n)
{
	printk("scullc_follow\n");
	while (n--) {
		if (!dev->next) {
			dev->next = kmalloc(sizeof(struct scullc_dev), GFP_KERNEL);
			memset(dev->next, 0, sizeof(struct scullc_dev));
		}
		dev = dev->next;
		continue;
	}
	return dev;
}

ssize_t scullc_read (struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
	struct scullc_dev *dev = filp->private_data; /* the first listitem */
	struct scullc_dev *dptr;
	int quantum = dev->quantum;
	int qset = dev->qset;
	int itemsize = quantum * qset; /* how many bytes in the listitem */
	int item, s_pos, q_pos, rest;
	ssize_t retval = 0;
	printk("scullc_read\n");

	if (down_interruptible (&dev->sem))
		return -ERESTARTSYS;
	if (*f_pos > dev->size) 
		goto nothing;
	if (*f_pos + count > dev->size)
		count = dev->size - *f_pos;
	/* find listitem, qset index, and offset in the quantum */
	item = ((long) *f_pos) / itemsize;
	rest = ((long) *f_pos) % itemsize;
	s_pos = rest / quantum; q_pos = rest % quantum;

    	/* follow the list up to the right position (defined elsewhere) */
	dptr = scullc_follow(dev, item);

	if (!dptr->data)
		goto nothing; /* don't fill holes */
	if (!dptr->data[s_pos])
		goto nothing;
	if (count > quantum - q_pos)
		count = quantum - q_pos; /* read only up to the end of this quantum */

	if (copy_to_user (buf, dptr->data[s_pos]+q_pos, count)) {
		retval = -EFAULT;
		goto nothing;
	}
	up (&dev->sem);

	*f_pos += count;
	return count;

  nothing:
	up (&dev->sem);
	return retval;
}

ssize_t scullc_write (struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
	struct scullc_dev *dev = filp->private_data;
	struct scullc_dev *dptr;
	int quantum = dev->quantum;
	int qset = dev->qset;
	int itemsize = quantum * qset;
	int item, s_pos, q_pos, rest;
	ssize_t retval = -ENOMEM; /* our most likely error */
	printk("scullc_write\n");

	if (down_interruptible (&dev->sem))
		return -ERESTARTSYS;

	/* find listitem, qset index and offset in the quantum */
	item = ((long) *f_pos) / itemsize;
	rest = ((long) *f_pos) % itemsize;
	s_pos = rest / quantum; q_pos = rest % quantum;

	/* follow the list up to the right position */
	dptr = scullc_follow(dev, item);
	if (!dptr->data) {
		dptr->data = kmalloc(qset * sizeof(void *), GFP_KERNEL);
		if (!dptr->data)
			goto nomem;
		memset(dptr->data, 0, qset * sizeof(char *));
	}
	/* Allocate a quantum using the memory cache */
	if (!dptr->data[s_pos]) {
		dptr->data[s_pos] = kmem_cache_alloc(scullc_cache, GFP_KERNEL);
		if (!dptr->data[s_pos])
			goto nomem;
		memset(dptr->data[s_pos], 0, scullc_quantum);
	}
	if (count > quantum - q_pos)
		count = quantum - q_pos; /* write only up to the end of this quantum */
	if (copy_from_user (dptr->data[s_pos]+q_pos, buf, count)) {
		retval = -EFAULT;
		goto nomem;
	}
	*f_pos += count;
 
    	/* update the size */
	if (dev->size < *f_pos)
		dev->size = *f_pos;
	up (&dev->sem);
	return count;

  nomem:
	up (&dev->sem);
	return retval;
}


/*
 * The fops
 */

struct file_operations scullc_fops = {
	.owner =     THIS_MODULE,
	.read =	     scullc_read,
	.write =     scullc_write,
	.open =	     scullc_open,
	.release =   scullc_release,
};

int scullc_trim(struct scullc_dev *dev)
{
	struct scullc_dev *next, *dptr;
	int qset = dev->qset;   /* "dev" is not-null */
	int i;
	printk("scullc_trim\n");

	if (dev->vmas) /* don't trim: there are active mappings */
		return -EBUSY;

	for (dptr = dev; dptr; dptr = next) { /* all the list items */
		if (dptr->data) {
			for (i = 0; i < qset; i++)
				if (dptr->data[i])
					kmem_cache_free(scullc_cache, dptr->data[i]);

			kfree(dptr->data);
			dptr->data=NULL;
		}
		next=dptr->next;
		if (dptr != dev) kfree(dptr); /* all of them but the first */
	}
	dev->size = 0;
	dev->qset = scullc_qset;
	dev->quantum = scullc_quantum;
	dev->next = NULL;
	return 0;
}

static void scullc_setup_cdev(struct scullc_dev *dev, int index)
{
	int err, devno = MKDEV(scullc_major, index);
	printk("scullc_setup_cdev\n");
    
	cdev_init(&dev->cdev, &scullc_fops);
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &scullc_fops;
	err = cdev_add (&dev->cdev, devno, 1);
	/* Fail gracefully if need be */
	if (err)
		printk(KERN_NOTICE "Error %d adding scull%d", err, index);
}

/*
 * Finally, the module stuff
 */

int scullc_init(void)
{
	int result;
	dev_t dev ;
	printk("scullc_init\n");
	
	result = alloc_chrdev_region(&dev, 0, 1, "scullc");
	scullc_major = MAJOR(dev);
	if (result < 0)
		return result;
	
	/* 
	 * allocate the devices -- we can't have them static, as the number
	 * can be specified at load time
	 */
	scullc_devices = kmalloc(scullc_devs*sizeof (struct scullc_dev), GFP_KERNEL);   //gfp_kernel may block. reserve memory under normal condition.
										        //  used when process safe to sleep.
	if (!scullc_devices) {
		result = -ENOMEM;
		goto fail_malloc;
	}
	memset(scullc_devices, 0, scullc_devs*sizeof (struct scullc_dev));
		scullc_devices->quantum = scullc_quantum;
		scullc_devices->qset = scullc_qset;
		sema_init (&scullc_devices->sem, 1);
		scullc_setup_cdev(scullc_devices, 0);

	scullc_cache = kmem_cache_create("scullc", scullc_quantum,
			0, SLAB_HWCACHE_ALIGN, NULL); /* no ctor/dtor */
	if (!scullc_cache) {
		scullc_cleanup();
		return -ENOMEM;
	}

#ifdef SCULLC_USE_PROC /* only when available */
	proc_create("scullcmem", 0, NULL, &scullc_proc_ops);
#endif
	return 0; /* succeed */

  fail_malloc:
	unregister_chrdev_region(dev, scullc_devs);
	return result;
}

void scullc_cleanup(void)
{
	printk("scullc_cleanup\n");

#ifdef SCULLC_USE_PROC
	remove_proc_entry("scullcmem", NULL);
#endif

		cdev_del(&scullc_devices->cdev);
		scullc_trim(scullc_devices + 0);
	kfree(scullc_devices);

	if (scullc_cache)
		kmem_cache_destroy(scullc_cache);
	unregister_chrdev_region(MKDEV (scullc_major, 0), scullc_devs);
}

module_init(scullc_init);
module_exit(scullc_cleanup);
