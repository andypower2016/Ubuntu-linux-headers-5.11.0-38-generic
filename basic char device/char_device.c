#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>			// file operations structure, allows user to open/read/write/close
#include <linux/cdev.h>			// this is a char driver, makes cdev available
#include <linux/semaphore.h>    // used to access semaphores for syncronization
#include <asm/uaccess.h>		// copy_to_user, copy_from_user (used to map data from user space to kernel space)

#include <linux/slab.h>       // kmalloc/kfree
#include <linux/seq_file.h>   // seq_printf
#include <linux/completion.h> // completion 
#include <linux/spinlock.h>   // spinlock
#include <linux/wait.h> 	  // wait_queue
#include <linux/ioctl.h>

#include "ioctrl_test.h"
#include "char_device.h"

struct char_device *fake_device = NULL;
// device registraion datas
int minior_num = 0; // minor number of the device, we set to zero here
int major_num; 		// will store our major number, extracted from dev_t using MAJOR macro
dev_t dev_num; 		// device number
int dev_count = 1;  // number of devices

int ret;
// completion not used here
//struct completion rw_completion; // device read will wait until the completion of device write
//DECLARE_COMPLETION(completion);	

spinlock_t g_spin_lock;
unsigned long flags;

wait_queue_head_t wq;
static int wait_flag = 0;

struct file_operations fileop = 
{
    .owner = THIS_MODULE,
    .open = open,
    .read = read,
    .write = write,
    .release = release,
    .llseek = seek,
    .unlocked_ioctl = ioctl_test,
 };

static void free_device_data(struct char_device *dev)
{
	if(dev->data)
	{
		int i = 0;
		for(; i < DATA_SIZE ; ++i)
		{
			if(dev->data[i])
			{
			   kfree(dev->data[i]);
			}
		}
		kfree(dev->data);
	}
	if(dev->ioctrl_data)
		kfree(dev->ioctrl_data);
}

static void cleanup_device(void)
{
	if(fake_device)
	{
	   cdev_del(&fake_device->mcdev);
	   free_device_data(fake_device);
	   kfree(fake_device);
	}
	unregister_chrdev_region(dev_num, 1);
}

static int setup_device(void)
{
    ret = 0; // 0 for success
	
	fake_device = kmalloc(sizeof(struct char_device), GFP_KERNEL);
	if(!fake_device)
	{
	   printk(KERN_ALERT "Fail to kalloc device\n");
	   ret = -ENOMEM;
	   cleanup_device();
	   return ret;
	}
	memset(fake_device, 0, sizeof(struct char_device));	

	// kmalloc ctrl data
	fake_device->ioctrl_data = (char*) kmalloc(buffer_size, GFP_KERNEL);
	memset(fake_device->ioctrl_data, 0, buffer_size);
	char init_data[] = "ctl init data";
	memcpy(fake_device->ioctrl_data, init_data, strlen(init_data));

	// init semaphore
	sema_init(&fake_device->sem, 1); // initial value one, means nothing is locked
	
	// init wait queue
	init_waitqueue_head(&wq);

	// init cdev
	cdev_init(&fake_device->mcdev, &fileop);
	fake_device->mcdev.owner = THIS_MODULE;
	ret = cdev_add(&fake_device->mcdev, dev_num, 1);
	if(ret < 0)
	{
	   DBG("Fail to add cdev to kernel");
	}
	return ret;
}

int open(struct inode *pinode, struct file *pfile)
{ 
	/*if(down_interruptible(&fake_device->sem)) */
	if(down_trylock(&fake_device->sem)) 
	{
	    DBG("Device is currently in use");
	}
	struct char_device *dev = container_of(pinode->i_cdev, struct char_device, mcdev);
	pfile->private_data = dev;
	DBG("Opened device ! device datasize=%d\n", dev->size);
	return 0;
}
int release(struct inode *pinode, struct file *pfile)
{
	DBG("Closed device");
	up(&fake_device->sem);
	return 0;
}

// Called when user wants to get information from the device
ssize_t read(struct file *pfile, char __user *buffer, size_t length, loff_t *f_pos)
{
	struct char_device *dev = pfile->private_data;
	DBG("fpos=%lld, offset=%lld\n", pfile->f_pos, *f_pos);
	
	int idx_chunk = *f_pos/(DATA_NUM*DATA_SIZE);
	int idx_data  = *f_pos%DATA_SIZE;

	if(!dev->data)
	{
		dev->data = (char**) kmalloc(DATA_NUM * sizeof(char*), GFP_KERNEL);
		memset(dev->data, 0, DATA_NUM * sizeof(char*));
	}

	if(!dev->data[idx_chunk])
	{
		dev->data[idx_chunk] = (char*) kmalloc(DATA_SIZE * sizeof(char), GFP_KERNEL);
		memset(dev->data[idx_chunk], 0, DATA_SIZE * sizeof(char));
	}

	if( (idx_data+length) > DATA_SIZE)
	{
		length = DATA_SIZE-idx_data;
	}

	// Take data from kernel space to user space
	// copy_from_user(destination, source, sizeToTransfer)	
	
	ret = copy_to_user(buffer, dev->data[idx_chunk]+idx_data, length);
	if(ret)
	{
	   ret = -EFAULT;
	   goto end;
	}
	// update file offset 
	*f_pos += length;
end:
	return ret;
}

// Called when user wants to send data to device
ssize_t write(struct file *pfile, const char __user *buffer, size_t length, loff_t *f_pos)
{
	struct char_device *dev = pfile->private_data;
	DBG("fpos=%lld, offset=%lld\n", pfile->f_pos, *f_pos);
	
	int idx_chunk = *f_pos/(DATA_NUM*DATA_SIZE);
	int idx_data  = *f_pos%DATA_SIZE;

	if(!dev->data)
	{
		dev->data = (char**) kmalloc(DATA_NUM * sizeof(char*), GFP_KERNEL);
		memset(dev->data, 0, DATA_NUM * sizeof(char*));
	}

	if(!dev->data[idx_chunk])
	{
		dev->data[idx_chunk] = (char*) kmalloc(DATA_SIZE * sizeof(char), GFP_KERNEL);
		memset(dev->data[idx_chunk], 0, DATA_SIZE * sizeof(char));
	}

	if( (idx_data+length) > DATA_SIZE)
	{
		length = DATA_SIZE-idx_data;
	}

	// Take data from user space to kernel space
	// copy_from_user(destination, source, sizeToTransfer)
	
	ret = copy_from_user(dev->data[idx_chunk]+idx_data, buffer, length);
	if(ret)
	{
	   ret = -EFAULT;
	   goto end;
	}
	*f_pos += length;
	if(*f_pos > dev->size)
	{
	   dev->size = *f_pos;
	}	
	DBG("write length=%ld, dev->size=%d", length, dev->size);
end:
	return ret;
}

loff_t seek(struct file* pfile, loff_t offset, int option)
{
	DBG("fpos=%lld, offset=%lld", pfile->f_pos, offset);
	struct char_device *dev = pfile->private_data;
	loff_t newpos;
	switch(option)
	{
	  case 0: // SEEK_SET
	     newpos = offset;
	     break;
	  case 1: // SEEK_CUR
	     newpos = pfile->f_pos + offset;
	     break;
	  case 2: // SEEK_END
	     newpos = dev->size + offset;
	     break;
	  default:
	      return -EINVAL;
	}
	if(newpos < 0)
	{
	    return -EINVAL;
	}
	pfile->f_pos = newpos;
	DBG("new file pos=%lld", newpos);
	return newpos;
}

long int ioctl_test(struct file *file, unsigned cmd, unsigned long arg)
{
	struct char_device *dev = file->private_data;
	switch(cmd)
	{
		case WR_VALUE:
			// wakes up all the process in the waitqueue (wq)
			wait_flag = 1;
			wake_up_interruptible(&wq);
 
			memset(dev->ioctrl_data, 0, strlen(dev->ioctrl_data));
			if(copy_from_user(dev->ioctrl_data, (char *) arg, strlen((char *)arg))) 
			{
				DBG("Error copying data from user!");
			}
			else
			{
				DBG("Update the data to : %s", dev->ioctrl_data);
			}
			break;
		case RD_VALUE:		
			if(copy_to_user((char *) arg, dev->ioctrl_data, strlen(dev->ioctrl_data))) 
				DBG("Error copying data to user!");
			else
				DBG("The data was copied!");

			// Puts the current reading process to waitqueue, until wake up is called and condition satisfied
			// function returns -ERESTARTSYS if it was interrupted by a signal, 0 when condition is true.
			if(wait_event_interruptible(wq, wait_flag != 0))
			{
				DBG("interrupt signal occurs");
				return -ERESTARTSYS;
			}
			wait_flag = 0; // reset flag to 0
			break;
		default:
			break;
	}
	return 0;
}

static __init int chrdev_init(void)
{
	// (1) use dynamic allocation to assign our device
	ret = alloc_chrdev_region(&dev_num, minior_num, dev_count, DEVICE_NAME);
	if(ret < 0)
	{
	   DBG("Fail to allocate major number");
	   return ret;
	}
	major_num = MAJOR(dev_num); // extracts the major number and store in variable
	DBG("Load Device");
	DBG("Major number=%d", major_num);
	DBG("Use mknod /dev/%s c %d 0\" for device file", DEVICE_NAME, major_num);

	// setup device dynamically
	ret = setup_device();
	if(ret < 0)
		return ret;

	// init char pipe
	dev_t dev = MKDEV(major_num, minior_num+1); // makes a device number(major, minior+1)
	ret = char_p_init(dev);
	if(ret < 0)
		return ret;

	return 0;
}

static __exit void chrdev_exit(void)
{
	cleanup_device();
	char_p_cleanup();
	DBG("Unload Device Complete");
}

module_init(chrdev_init);
module_exit(chrdev_exit);
MODULE_LICENSE("Dual BSD/GPL");
//MODULE_LICENSE("GPL");

