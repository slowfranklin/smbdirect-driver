/*******************************************************************************
 * This file contains the smbdirect driver for Samba
 *
 * (c) Richard Sharpe <rsharpe@samba.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 ****************************************************************************/

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/socket.h>
#include <linux/ioctl.h>
#include <linux/inet.h>
#include <uapi/linux/in.h>
#include <linux/in6.h>
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>

#include "smbdirect.h"

/*
 * TODO: Convert to using dev_dbg, but probably need a platform_device for
 * that.
 */

static struct smbd_device *smbd_device;

static int smbd_open(struct inode *inode, struct file *filp)
{

        return 0;
}

static int smbd_release(struct inode *inode, struct file *filp)
{
        return 0;
}

/*
 * Get the params and mark us as initialized
 */
static long ioctl_set_params(unsigned long arg)
{
        int res = 0;
        struct smbd_params *params = (void __user *)arg;

        res = copy_from_user(&smbd_device->params,
                             params,
                             sizeof(struct smbd_params));

        if (!res) {
                printk(KERN_ERR "Error: Memory for SMBD_SET_PARAMS "
                       "not accessible\n");
                return -EFAULT;
        }

        /*
         * Check the values and also copy the blob
         */

        /*
         * We are initialized now
         */
        smbd_device->initialized = true;

        return res;
}

static long smbd_ioctl(struct file *filp,
                       unsigned int cmd,
                       unsigned long arg)
{
	long res;

        printk(KERN_INFO "Handling ioctl: %0X\n", cmd);

        switch (cmd) {
	case SMBD_LISTEN:
		res = smbd_listen(smbd_device);
		break;

        case SMBD_SET_PARAMS:
                res = ioctl_set_params(arg);
		break;

        case SMBD_GET_MEM_PARAMS:
		return -EINVAL;

        case SMBD_SET_SESSION_ID:
		return -EINVAL;

        default:
		return -EINVAL;
        }

        return res;
}

static unsigned int smbd_poll(struct file *f,
			      poll_table *wait)
{
        unsigned int mask = 0;

        poll_wait(f, &smbd_device->conn_queue, wait);

        return mask;
}

static struct file_operations smbd_fops = {
        .owner = THIS_MODULE,
        .open = smbd_open,
        .unlocked_ioctl = smbd_ioctl,
	.poll = smbd_poll,
        .release = smbd_release,
};

/*
 * A /proc file for some debugging ... replace this with configfs stuff ...
 */
static int read_proc_stuff(struct seq_file *m, void *data)
{

        seq_printf(m, "Connection Count = %d\n",
                   smbd_device->connection_count);
        return 0;
}

static int smbd_proc_open(struct inode *inode, struct file *file)
{
        return single_open(file, read_proc_stuff, NULL);
}

static struct file_operations smbd_proc_fops = {
        .open = smbd_proc_open,
        .read = seq_read,
        .llseek = seq_lseek,
        .release = seq_release,
};

static int __init smbdirect_init(void)
{
        int res = 0;

        proc_create("driver/smbdirect", 0, NULL, &smbd_proc_fops);

	smbd_device = kmalloc(sizeof(struct smbd_device), GFP_KERNEL | __GFP_ZERO);
	if (smbd_device == NULL) {
		return -ENOMEM;
	}

        mutex_init(&smbd_device->connection_list_mutex);
        INIT_LIST_HEAD(&smbd_device->connection_list);
        init_waitqueue_head(&smbd_device->conn_queue);

        res = alloc_chrdev_region(&smbd_device->smbdirect_dev_no, 0, 1, "smbdirect");
        if (res < 0) {
                printk(KERN_ERR "Major number allocation failed\n");
                return res;
        }

        smbd_device->smbd_wq = alloc_workqueue("SMB-Direct Work Queue", 0, 0);
        if (smbd_device->smbd_wq == NULL) {
                printk(KERN_ERR "Unable to allocate work queue\n");
                return -ENOMEM;
        }

        cdev_init(&smbd_device->cdev, &smbd_fops);
        smbd_device->cdev.owner = THIS_MODULE;

        res = cdev_add(&smbd_device->cdev, smbd_device->smbdirect_dev_no, 1);
        if (res) {
                printk(KERN_ERR "Unable to add smbdirect device: %d\n", res);
                goto out_wq;
        }

	smbd_device->kio_class = class_create(THIS_MODULE, "smbdirect");
	if (IS_ERR(smbd_device->kio_class)) {
		printk(KERN_ERR "class_create(): err: %ld\n", PTR_ERR(smbd_device->kio_class));
		goto out_cdev;
	}

	smbd_device->kio_device = device_create(smbd_device->kio_class, NULL,
						smbd_device->smbdirect_dev_no,
						NULL, "smbdirect");
	if (IS_ERR(smbd_device->kio_device)) {
		printk(KERN_ERR "device_create(): err: %ld\n",
		       PTR_ERR(smbd_device->kio_device));
		goto out;
	}

        return 0;

out:
	if (smbd_device->kio_device != NULL) {
		device_destroy(smbd_device->kio_class,
			       smbd_device->smbdirect_dev_no);
	}
	if (smbd_device->kio_class != NULL) {
		class_destroy(smbd_device->kio_class);
	}

out_cdev:
        cdev_del(&smbd_device->cdev);
out_wq:
        destroy_workqueue(smbd_device->smbd_wq);

	if (smbd_device != NULL) {
		kfree(smbd_device);
	}
        return res;
}

static void __exit smbdirect_exit(void)
{
        smbd_teardown_listen_and_connections(smbd_device);
	device_destroy(smbd_device->kio_class, smbd_device->smbdirect_dev_no);
	class_destroy(smbd_device->kio_class);
        cdev_del(&smbd_device->cdev);
        destroy_workqueue(smbd_device->smbd_wq);
	kfree(smbd_device);
        remove_proc_entry("driver/smbdirect", NULL);
}

MODULE_DESCRIPTION("smbdirect driver for Samba");
MODULE_VERSION("0.1");
MODULE_AUTHOR("rsharpe@samba.org");
MODULE_AUTHOR("slow@samba.org");
MODULE_LICENSE("GPL");

module_init(smbdirect_init);
module_exit(smbdirect_exit);
