/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 *
 * Provided boilerplate:
 * - device registration and teardown
 * - timer setup
 * - RSS helper
 * - soft-limit and hard-limit event helpers
 * - ioctl dispatch shell
 *
 * YOUR WORK: Fill in all sections marked // TODO.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1
#ifndef del_timer_sync
#define del_timer_sync timer_delete_sync
#endif

/* ==============================================================
 * TODO 1: Define your linked-list node struct.
 *
 * Requirements:
 * - track PID, container ID, soft limit, and hard limit
 * - remember whether the soft-limit warning was already emitted
 * - include `struct list_head` linkage
 * ============================================================== */
struct monitor_node {
    pid_t pid;
    char container_id[256]; /* Buffer for container ID string */
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    bool soft_warning_emitted;
    struct list_head list;
};


/* ==============================================================
 * TODO 2: Declare the global monitored list and a lock.
 *
 * Requirements:
 * - shared across ioctl and timer code paths
 * - protect insert, remove, and iteration safely
 *
 * You may choose either a mutex or a spinlock, but your README must
 * justify the choice in terms of the code paths you implemented.
 * ============================================================== */
static LIST_HEAD(monitor_list);
/* * Spinlock is chosen here over a mutex. The timer_callback runs in an 
 * interrupt/softirq context, where sleeping is strictly forbidden. 
 * A mutex can sleep, so we MUST use a spinlock to protect the list.
 */
static DEFINE_SPINLOCK(monitor_lock);


/* --- Provided: internal device / timer state --- */
static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

/* ---------------------------------------------------------------
 * Provided: RSS Helper
 *
 * Returns the Resident Set Size in bytes for the given PID,
 * or -1 if the task no longer exists.
 * --------------------------------------------------------------- */
static long __attribute__((unused)) get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ---------------------------------------------------------------
 * Provided: soft-limit helper
 *
 * Log a warning when a process exceeds the soft limit.
 * --------------------------------------------------------------- */
static void __attribute__((unused)) log_soft_limit_event(const char *container_id,
                                 pid_t pid,
                                 unsigned long limit_bytes,
                                 long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Provided: hard-limit helper
 *
 * Kill a process when it exceeds the hard limit.
 * --------------------------------------------------------------- */
static __attribute__((unused)) void kill_process(const char *container_id,
                         pid_t pid,
                         unsigned long limit_bytes,
                         long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Timer Callback - fires every CHECK_INTERVAL_SEC seconds.
 * --------------------------------------------------------------- */
static void timer_callback(struct timer_list *t)
{
    /* ==============================================================
     * TODO 3: Implement periodic monitoring.
     *
     * Requirements:
     * - iterate through tracked entries safely
     * - remove entries for exited processes
     * - emit soft-limit warning once per entry
     * - enforce hard limit and then remove the entry
     * - avoid use-after-free while deleting during iteration
     * ============================================================== */
    struct monitor_node *node, *tmp;
    unsigned long flags;
    long rss;

    spin_lock_irqsave(&monitor_lock, flags);
    list_for_each_entry_safe(node, tmp, &monitor_list, list) {
        rss = get_rss_bytes(node->pid);

        /* Process exited, so we can stop tracking it */
        if (rss < 0) {
            list_del(&node->list);
            kfree(node);
            continue;
        }

        /* Check Hard Limit */
        if (node->hard_limit_bytes > 0 && rss > node->hard_limit_bytes) {
            kill_process(node->container_id, node->pid, node->hard_limit_bytes, rss);
            list_del(&node->list);
            kfree(node);
            continue; /* Node freed, move to next */
        }

        /* Check Soft Limit */
        if (node->soft_limit_bytes > 0 && rss > node->soft_limit_bytes) {
            if (!node->soft_warning_emitted) {
                log_soft_limit_event(node->container_id, node->pid, node->soft_limit_bytes, rss);
                node->soft_warning_emitted = true;
            }
        } else if (node->soft_warning_emitted && rss <= node->soft_limit_bytes) {
            /* Reset warning flag if memory drops back below the soft limit */
            node->soft_warning_emitted = false;
        }
    }
    spin_unlock_irqrestore(&monitor_lock, flags);

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ---------------------------------------------------------------
 * IOCTL Handler
 *
 * Supported operations:
 * - register a PID with soft + hard limits
 * - unregister a PID when the runtime no longer needs tracking
 * --------------------------------------------------------------- */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;
    struct monitor_node *new_node, *node, *tmp;
    unsigned long flags;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {
        printk(KERN_INFO
               "[container_monitor] Registering container=%s pid=%d soft=%lu hard=%lu\n",
               req.container_id, req.pid, req.soft_limit_bytes, req.hard_limit_bytes);

        /* ==============================================================
         * TODO 4: Add a monitored entry.
         *
         * Requirements:
         * - allocate and initialize one node from req
         * - validate allocation and limits
         * - insert into the shared list under the chosen lock
         * ============================================================== */
        
        /* Validate reasonable limits */
        if (req.soft_limit_bytes == 0 && req.hard_limit_bytes == 0)
            return -EINVAL;

        new_node = kmalloc(sizeof(*new_node), GFP_KERNEL);
        if (!new_node)
            return -ENOMEM;

        new_node->pid = req.pid;
        strncpy(new_node->container_id, req.container_id, sizeof(new_node->container_id) - 1);
        new_node->container_id[sizeof(new_node->container_id) - 1] = '\0'; // Ensure null-termination
        new_node->soft_limit_bytes = req.soft_limit_bytes;
        new_node->hard_limit_bytes = req.hard_limit_bytes;
        new_node->soft_warning_emitted = false;

        spin_lock_irqsave(&monitor_lock, flags);
        list_add_tail(&new_node->list, &monitor_list);
        spin_unlock_irqrestore(&monitor_lock, flags);

        return 0;
    }

    printk(KERN_INFO
           "[container_monitor] Unregister request container=%s pid=%d\n",
           req.container_id, req.pid);

    /* ==============================================================
     * TODO 5: Remove a monitored entry on explicit unregister.
     *
     * Requirements:
     * - search by PID, container ID, or both
     * - remove the matching entry safely if found
     * - return status indicating whether a matching entry was removed
     * ============================================================== */
    spin_lock_irqsave(&monitor_lock, flags);
    list_for_each_entry_safe(node, tmp, &monitor_list, list) {
        if (node->pid == req.pid && strncmp(node->container_id, req.container_id, sizeof(node->container_id)) == 0) {
            list_del(&node->list);
            kfree(node);
            spin_unlock_irqrestore(&monitor_lock, flags);
            return 0; /* Found and removed */
        }
    }
    spin_unlock_irqrestore(&monitor_lock, flags);

    return -ENOENT;
}

/* --- Provided: file operations --- */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* --- Provided: Module Init --- */
static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Module loaded. Device: /dev/%s\n", DEVICE_NAME);
    return 0;
}

/* --- Provided: Module Exit --- */
static void __exit monitor_exit(void)
{
    struct monitor_node *node, *tmp;
    unsigned long flags;

    del_timer_sync(&monitor_timer);

    /* ==============================================================
     * TODO 6: Free all remaining monitored entries.
     *
     * Requirements:
     * - remove and free every list node safely
     * - leave no leaked state on module unload
     * ============================================================== */
    spin_lock_irqsave(&monitor_lock, flags);
    list_for_each_entry_safe(node, tmp, &monitor_list, list) {
        list_del(&node->list);
        kfree(node);
    }
    spin_unlock_irqrestore(&monitor_lock, flags);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
