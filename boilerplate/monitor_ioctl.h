/*
 * monitor_ioctl.h - Shared ioctl definitions between engine (user space)
 *                   and monitor (kernel module).
 *
 * Both engine.c and monitor.c include this header.
 * Dual-use: compiles in both user space and kernel space.
 */

#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif

#define MONITOR_MAGIC    'M'
#define MONITOR_NAME_LEN 64

struct monitor_request {
    int           pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    char          container_id[MONITOR_NAME_LEN];
};

#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct monitor_request)

#endif /* MONITOR_IOCTL_H */
