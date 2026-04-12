### Kernel Monitor Module (`monitor.c`)
| What We Built | Why & How We Built It |
|:---|:---|
| **`struct monitor_node`** | Think of this as an ID card for each tracked container. It stores the PID, container ID, soft/hard memory limits, and importantly, a flag `soft_warned` so we don't spam the logs if a process sits right at the limit edge. |
| **Lists & Locks** | We created a standard Linux linked list `LIST_HEAD(monitor_list)` to connect the ID cards and used a `mutex_lock` because both the timer (checking memory) and the ioctl (adding/removing things) access this list at the same time. Mutex prevents them from crashing into each other. |
| **`timer_callback()`** | This runs every second. It iterates through the ID cards. If a `get_rss_bytes` check is $<0$ (process died), it throws the ID card away. If RAM crosses the hard limit, it instantly triggers `kill_process()` and removes the node. If it crosses the soft limit, we trigger `log_soft_limit_event()` and set the warn flag to `1`. |
| **`MONITOR_REGISTER`** | The "add" function. A `kmalloc` gets us a blank ID card, we copy over the limits from user-space into the node, and blindly stick it on the tail end of the linked list. |
| **Module Exit Cleanup** | When we stop the entire kernel module `rmmod monitor.ko`, we spin a tiny loop deleting and running `kfree()` on any remaining ID cards so we don't leak server memory. |

### Userspace Runtime (`engine.c`)
| What We Built | Why & How We Built It |
|:---|:---|
| **Bounded Buffer (Push/Pop)** | A fixed-size array (`items`) that acts like a queue between producers and consumers. If it gets full on Push, the `pthread_cond_wait(&not_full)` makes it sleep. On Pop, if it's empty, `pthread_cond_wait(&not_empty)` sleeps. This avoids 100% CPU waiting loops. |
| **Logging Thread** | This is the worker that writes logs. It constantly loops on `bounded_buffer_pop()`. Every time it gets a chunk of log text, it does a standard `open(..., O_CREAT \| O_WRONLY \| O_APPEND)` on `logs/<container_id>.log` to dump the text inside permanently. |
| **Baby Containers (`child_fn`)** | This is the process launched by `clone()`! It first sets a `nice()` priority. Then, `chroot` gives it a scary fake root directory jail. `chdir("/")` pushes it physically inside. It blindly runs `/bin/sh -c <command>` using `execv`. |
| **Supervisor (`run_supervisor`)** | The "brain". It binds a Unix socket (mini_runtime.sock) and sets up a `listen()` loop forever. Anytime a client script contacts it, it intercepts the `control_request_t` struct, sees it, handles `CMD_START` to issue the `clone()` system call, and replies back! |
| **Client Sender (`send_control_request`)** | Instead of printing "not implemented", your client CLI (`./engine start ...`) literally takes a struct, connects to mini_runtime.sock, writes the struct across IPC, and blindly reads whatever the Supervisor decides to tell it. |

Made changes.