#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <sys/types.h>
#endif

#define MONITOR_NAME_LEN 32

struct monitor_request {
    pid_t pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    char container_id[MONITOR_NAME_LEN];
};

typedef enum {
    HEALTH_OK = 0,
    HEALTH_WARN = 1,
    HEALTH_CRITICAL = 2
} container_health_t;

struct health_response {
    pid_t pid;
    container_health_t health;
};

#define MONITOR_MAGIC 'M'
#define MONITOR_REGISTER _IOW(MONITOR_MAGIC, 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct monitor_request)
#define MONITOR_GET_HEALTH _IOWR(MONITOR_MAGIC, 3, struct health_response)

#endif
