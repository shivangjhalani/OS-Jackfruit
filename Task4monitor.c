#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/pid.h>

#define DEVICE_NAME "container_monitor"
#define MONITOR_MAGIC 'k'
#define REGISTER_CONTAINER _IOW(MONITOR_MAGIC, 1, struct reg_info)

/* Structure for IOCTL communication */
struct reg_info {
    pid_t pid;
    unsigned long soft_limit;  // in KB
    unsigned long hard_limit;  // in KB
};

/* Internal tracking structure */
struct monitored_proc {
    struct list_head list;
    pid_t pid;
    unsigned long soft_limit;
    unsigned long hard_limit;
    bool soft_limit_hit;
};

static LIST_HEAD(proc_list);
static DEFINE_MUTEX(list_lock);
static struct delayed_work monitor_work;
static int major_num;

/* Periodic check: Iterates through the list and enforces limits */
static void monitor_check_worker(struct work_struct *work) {
    struct monitored_proc *entry, *tmp;
    struct task_struct *task;
    struct mm_struct *mm;
    unsigned long rss_kb;

    mutex_lock(&list_lock);
    list_for_each_entry_safe(entry, tmp, &proc_list, list) {
        /* Find task by PID */
        task = get_pid_task(find_get_pid(entry->pid), PIDTYPE_PID);

        /* If process no longer exists, remove from tracking */
        if (!task) {
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Access Memory Management struct */
        mm = get_task_mm(task);
        if (mm) {
            /* get_mm_rss returns pages; convert to KB */
            rss_kb = get_mm_rss(mm) << (PAGE_SHIFT - 10);

            /* 1. Hard Limit Enforcement */
            if (rss_kb > entry->hard_limit) {
                pr_crit("[%s] PID %d killed: Hard limit exceeded (%lu KB > %lu KB)\n",
                        DEVICE_NAME, entry->pid, rss_kb, entry->hard_limit);
                send_sig(SIGKILL, task, 0);
            }
            /* 2. Soft Limit Notification (Log once) */
            else if (rss_kb > entry->soft_limit && !entry->soft_limit_hit) {
                pr_warn("[%s] PID %d warning: Soft limit exceeded (%lu KB > %lu KB)\n",
                        DEVICE_NAME, entry->pid, rss_kb, entry->soft_limit);
                entry->soft_limit_hit = true;
            }
            mmput(mm);
        }
        put_task_struct(task);
    }
    mutex_unlock(&list_lock);

    /* Reschedule for next check (1 second interval) */
    schedule_delayed_work(&monitor_work, msecs_to_jiffies(1000));
}

/* IOCTL Handler for registration */
static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    if (cmd == REGISTER_CONTAINER) {
        struct reg_info info;
        struct monitored_proc *new_entry;

        if (copy_from_user(&info, (struct reg_info __user *)arg, sizeof(info)))
            return -EFAULT;

        new_entry = kmalloc(sizeof(*new_entry), GFP_KERNEL);
        if (!new_entry) return -ENOMEM;

        new_entry->pid            = info.pid;
        new_entry->soft_limit     = info.soft_limit;
        new_entry->hard_limit     = info.hard_limit;
        new_entry->soft_limit_hit = false;

        mutex_lock(&list_lock);
        list_add(&new_entry->list, &proc_list);
        mutex_unlock(&list_lock);

        pr_info("[%s] Registered PID %d (Soft: %lu KB, Hard: %lu KB)\n",
                DEVICE_NAME, info.pid, info.soft_limit, info.hard_limit);
        return 0;
    }
    return -EINVAL;
}

static struct file_operations fops = {
    .unlocked_ioctl = device_ioctl,
    .owner          = THIS_MODULE,
};

static int __init monitor_init(void) {
    major_num = register_chrdev(0, DEVICE_NAME, &fops);
    if (major_num < 0) return major_num;
    INIT_DELAYED_WORK(&monitor_work, monitor_check_worker);
    schedule_delayed_work(&monitor_work, msecs_to_jiffies(1000));
    pr_info("[%s] Module loaded. Major: %d\n", DEVICE_NAME, major_num);
    return 0;
}

static void __exit monitor_exit(void) {
    struct monitored_proc *entry, *tmp;
    cancel_delayed_work_sync(&monitor_work);
    unregister_chrdev(major_num, DEVICE_NAME);
    mutex_lock(&list_lock);
    list_for_each_entry_safe(entry, tmp, &proc_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&list_lock);
    pr_info("[%s] Module unloaded\n", DEVICE_NAME);
}

module_init(monitor_init);
module_exit(monitor_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aditya Alur, Aadish Sarin");
MODULE_DESCRIPTION("Kernel Memory Monitor with Soft/Hard Limits");