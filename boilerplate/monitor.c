/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
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

struct container_node {
    pid_t pid;
    char container_id[MONITOR_NAME_LEN];
    unsigned long soft_limit;
    unsigned long hard_limit;
    bool soft_limit_triggered;
    struct list_head list;
};

static LIST_HEAD(monitored_containers);
static DEFINE_MUTEX(monitor_lock);

static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

static long get_rss_bytes(pid_t pid)
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

static void log_soft_limit_event(const char *container_id, pid_t pid, unsigned long limit_bytes, long rss_bytes)
{
    printk(KERN_WARNING "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

static void kill_process(const char *container_id, pid_t pid, unsigned long limit_bytes, long rss_bytes)
{
    struct task_struct *task;
    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

static void timer_callback(struct timer_list *t)
{
    struct container_node *entry, *tmp;
    long current_rss;

    mutex_lock(&monitor_lock);
    list_for_each_entry_safe(entry, tmp, &monitored_containers, list) {
        current_rss = get_rss_bytes(entry->pid);

        if (current_rss < 0) {
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (current_rss > entry->hard_limit) {
            kill_process(entry->container_id, entry->pid, entry->hard_limit, current_rss);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (current_rss > entry->soft_limit) {
            if (!entry->soft_limit_triggered) {
                log_soft_limit_event(entry->container_id, entry->pid, entry->soft_limit, current_rss);
                entry->soft_limit_triggered = true;
            }
        } else {
            entry->soft_limit_triggered = false;
        }
    }
    mutex_unlock(&monitor_lock);

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER && cmd != MONITOR_GET_HEALTH)
        return -EINVAL;

    if (cmd == MONITOR_GET_HEALTH) {
        struct health_response health_res;
        struct container_node *entry;

        if (copy_from_user(&health_res, (struct health_response __user *)arg, sizeof(health_res)))
            return -EFAULT;

        health_res.health = HEALTH_OK;
        mutex_lock(&monitor_lock);
        list_for_each_entry(entry, &monitored_containers, list) {
            if (entry->pid == health_res.pid) {
                if (entry->soft_limit_triggered)
                    health_res.health = HEALTH_WARN;
                else
                    health_res.health = HEALTH_OK;
                break;
            }
        }
        mutex_unlock(&monitor_lock);

        if (copy_to_user((struct health_response __user *)arg, &health_res, sizeof(health_res)))
            return -EFAULT;
        return 0;
    }

    if (cmd == MONITOR_REGISTER) {
        struct monitor_request req;
        struct container_node *entry;

        if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
            return -EFAULT;

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid = req.pid;
        entry->soft_limit = req.soft_limit_bytes;
        entry->hard_limit = req.hard_limit_bytes;
        entry->soft_limit_triggered = false;
        strlcpy(entry->container_id, req.container_id, MONITOR_NAME_LEN);

        mutex_lock(&monitor_lock);
        list_add(&entry->list, &monitored_containers);
        mutex_unlock(&monitor_lock);

        printk(KERN_INFO "[container_monitor] Registered container=%s pid=%d\n", req.container_id, req.pid);
        return 0;
    }

    if (cmd == MONITOR_UNREGISTER) {
        struct monitor_request req;
        struct container_node *entry, *tmp;

        if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
            return -EFAULT;

        mutex_lock(&monitor_lock);
        list_for_each_entry_safe(entry, tmp, &monitored_containers, list) {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                kfree(entry);
                mutex_unlock(&monitor_lock);
                return 0;
            }
        }
        mutex_unlock(&monitor_lock);
        return -ENOENT;
    }

    return -EINVAL;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

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

static void __exit monitor_exit(void)
{
    struct container_node *entry, *tmp;

    del_timer_sync(&monitor_timer);

    mutex_lock(&monitor_lock);
    list_for_each_entry_safe(entry, tmp, &monitored_containers, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&monitor_lock);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS Runtime Team");
MODULE_DESCRIPTION("Container memory monitor with soft and hard limits");
