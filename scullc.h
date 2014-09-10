#include <linux/ioctl.h>
#include <linux/cdev.h>

/*
 * The bare device is a variable-length region of memory.
 * Use a linked list of indirect blocks.
 *
 * "scullc_dev->data" points to an array of pointers, each
 * pointer refers to a memory page.
 *
 * The array (quantum-set) is SCULLC_QSET long.
 */

struct scullc_dev {
	void **data;
	struct scullc_dev *next;  /* next listitem */
	int vmas;                 /* active mappings */
	int quantum;              /* the current allocation size */
	int qset;                 /* the current array size */
	size_t size;              /* 32-bit will suffice */
	struct semaphore sem;     /* Mutual exclusion */
	struct cdev cdev;
};

extern struct scullc_dev *scullc_devices;

extern struct file_operations scullc_fops;

/*
 * The different configurable parameters
 */
extern int scullc_major;     /* main.c */
extern int scullc_devs;
extern int scullc_order;
extern int scullc_qset;

/*
 * Prototypes for shared functions
 */
int scullc_trim(struct scullc_dev *dev);
struct scullc_dev *scullc_follow(struct scullc_dev *dev, int n);


#ifdef SCULLC_DEBUG
#  define SCULLC_USE_PROC
#endif
