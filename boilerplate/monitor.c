#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/signal.h>
#include <linux/uaccess.h>
#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS-Jackfruit Team");
MODULE_DESCRIPTION("Container process memory monitor with soft/hard limits");

#define DEVICE_NAME        "container_monitor"
#define CHECK_INTERVAL_MS  2000

/* ── Per-container kernel record ── */
struct container_entry {
    int  pid;
    int  soft_limit_mb;
    int  hard_limit_mb;
    int  soft_warned;
    char name[64];
    struct list_head list;
};

static LIST_HEAD(container_list);
static DEFINE_MUTEX(list_mutex);
static struct timer_list check_timer;

static dev_t          dev_num;
static struct cdev    monitor_cdev;
static struct class  *monitor_class;

/* ── RSS in MB for a given PID ── */
static long get_rss_mb(int pid) {
    struct task_struct *task;
    struct mm_struct   *mm;
    long rss = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) { rcu_read_unlock(); return -1; }
    mm = task->mm;
    if (mm)
        rss = (long)(get_mm_rss(mm)) >> (20 - PAGE_SHIFT);
    rcu_read_unlock();
    return rss;
}

/* ── Timer: check all registered containers ── */
static void check_memory(struct timer_list *t)
{
    struct container_entry *e, *tmp;

    mutex_lock(&list_mutex);
    list_for_each_entry_safe(e, tmp, &container_list, list) {
        long rss = get_rss_mb(e->pid);

        /* Process gone — remove stale entry */
        if (rss < 0) {
            printk(KERN_INFO "[jackfruit] container '%s' (PID %d) "
                   "exited — removing from monitor\n", e->name, e->pid);
            list_del(&e->list);
            kfree(e);
            continue;
        }

        /* Hard limit — kill the process */
        if (rss >= e->hard_limit_mb) {
            struct task_struct *task;
            printk(KERN_WARNING "[jackfruit] HARD LIMIT: container '%s' "
                   "(PID %d) using %ldMB >= %dMB — sending SIGKILL\n",
                   e->name, e->pid, rss, e->hard_limit_mb);
            rcu_read_lock();
            task = pid_task(find_vpid(e->pid), PIDTYPE_PID);
            if (task) send_sig(SIGKILL, task, 1);
            rcu_read_unlock();
            continue;
        }

        /* Soft limit — warn once */
        if (rss >= e->soft_limit_mb && !e->soft_warned) {
            printk(KERN_WARNING "[jackfruit] SOFT LIMIT: container '%s' "
                   "(PID %d) using %ldMB >= %dMB\n",
                   e->name, e->pid, rss, e->soft_limit_mb);
            e->soft_warned = 1;
        }
    }
    mutex_unlock(&list_mutex);

    mod_timer(&check_timer, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));
}

/* ── ioctl handler ── */
static long monitor_ioctl(struct file *file,
                          unsigned int cmd, unsigned long arg)
{
    switch (cmd) {

    case IOCTL_REGISTER_PID: {
        MonitorEntry ue;
        struct container_entry *entry;

        if (copy_from_user(&ue, (MonitorEntry __user *)arg, sizeof(ue)))
            return -EFAULT;

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry) return -ENOMEM;

        entry->pid           = ue.pid;
        entry->soft_limit_mb = ue.soft_limit_mb;
        entry->hard_limit_mb = ue.hard_limit_mb;
        entry->soft_warned   = 0;
        strncpy(entry->name, ue.name, 63);
        entry->name[63] = '\0';

        mutex_lock(&list_mutex);
        list_add(&entry->list, &container_list);
        mutex_unlock(&list_mutex);

        printk(KERN_INFO "[jackfruit] registered '%s' PID=%d "
               "soft=%dMB hard=%dMB\n",
               entry->name, entry->pid,
               entry->soft_limit_mb, entry->hard_limit_mb);
        return 0;
    }

    case IOCTL_UNREGISTER_PID: {
        int pid;
        struct container_entry *e, *tmp;

        if (copy_from_user(&pid, (int __user *)arg, sizeof(pid)))
            return -EFAULT;

        mutex_lock(&list_mutex);
        list_for_each_entry_safe(e, tmp, &container_list, list) {
            if (e->pid == pid) {
                list_del(&e->list);
                kfree(e);
                printk(KERN_INFO "[jackfruit] unregistered PID %d\n", pid);
                break;
            }
        }
        mutex_unlock(&list_mutex);
        return 0;
    }

    default:
        return -EINVAL;
    }
}

static const struct file_operations monitor_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

static int __init monitor_init(void)
{
    int ret;

    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        printk(KERN_ERR "[jackfruit] alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }

    cdev_init(&monitor_cdev, &monitor_fops);
    ret = cdev_add(&monitor_cdev, dev_num, 1);
    if (ret < 0) { unregister_chrdev_region(dev_num, 1); return ret; }

    monitor_class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(monitor_class)) {
        cdev_del(&monitor_cdev);
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(monitor_class);
    }

    device_create(monitor_class, NULL, dev_num, NULL, DEVICE_NAME);

    timer_setup(&check_timer, check_memory, 0);
    mod_timer(&check_timer, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));

    printk(KERN_INFO "[jackfruit] monitor loaded — /dev/%s ready\n",
           DEVICE_NAME);
    return 0;
}

static void __exit monitor_exit(void)
{
    struct container_entry *e, *tmp;

    del_timer_sync(&check_timer);

    mutex_lock(&list_mutex);
    list_for_each_entry_safe(e, tmp, &container_list, list) {
        list_del(&e->list);
        kfree(e);
    }
    mutex_unlock(&list_mutex);

    device_destroy(monitor_class, dev_num);
    class_destroy(monitor_class);
    cdev_del(&monitor_cdev);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[jackfruit] monitor unloaded cleanly\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
