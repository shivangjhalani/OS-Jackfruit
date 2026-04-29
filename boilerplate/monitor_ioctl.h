#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

#define MONITOR_MAGIC 'M'

typedef struct {
    int  pid;
    int  soft_limit_mb;
    int  hard_limit_mb;
    char name[64];
} MonitorEntry;

#define IOCTL_REGISTER_PID   _IOW(MONITOR_MAGIC, 1, MonitorEntry)
#define IOCTL_UNREGISTER_PID _IOW(MONITOR_MAGIC, 2, int)

#endif
