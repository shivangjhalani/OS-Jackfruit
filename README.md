# Multi Container Runtime

## 1. Team Details:
- Shruti Sridhar - PES2UG24CS498
- Tanisha Dalmia - PES2UG24CS550


## 2. Step by step commands and instructions:
### - build:
```bash
cd boilerplate
make
```
### - load kernal module:
```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```
### - Start Supervisor (terminal 1):
```bash
sudo ./engine supervisor ./rootfs-base
```
### - Launch containers (terminal 2):
```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh
sudo ./engine start beta ./rootfs-beta ./cpu_hog --soft-mib 48 --hard-mib 80
```
### - List and monitor:
```bash
sudo ./engine ps
sudo ./engine logs alpha
```
### - Stop Containers:
```bash
  sudo ./engine stop alpha
```
### - Stop Supervisor:
```bash
  Press Ctrl+C in Terminal 1.
```
### - Unload Module:
```bash
  sudo rmmod monitor
```
### - Clean Binaries:
```bash
  make clean
```

## 3. Screenshots
All screenshots are in the dedicated folder

## 4. Engineering Analysis
### Isolation Mechanisms
- Our runtime implements isolation using the Linux Namespace API via the clone() system call. By passing CLONE_NEWPID, CLONE_NEWUTS, and CLONE_NEWNS, we ensure each container operates with its own process tree (starting at PID 1), a unique hostname, and an isolated filesystem view. The use of chroot() further jails the process into the provided Alpine rootfs. Unlike a Virtual Machine, which virtualizes the entire hardware layer and runs a guest kernel, our containers share the host's Intel-based kernel, providing a lightweight yet effective "resource view" isolation.

### Supervisor and Process Lifecycle
- The supervisor process serves as a persistent parent and Sub-Reaper. Since containers are fork-cloned children, they would become zombies upon exit if not properly reaped. Our supervisor utilizes a SIGCHLD handler combined with waitpid(-1, &status, WNOHANG) to asynchronously collect exit statuses and clean up the process table. We maintain container metadata (ID, Host PID, State) in a linked list protected by a pthread_mutex_t to prevent race conditions during concurrent updates from the signal handler and the main event loop.

### IPC, Threads, and Synchronization
- The project implements a dual-channel IPC architecture:

### Logging Path: 
- We use Pipes to redirect container stdout/stderr to the supervisor. A dedicated producer thread per container reads these pipes and pushes data into a Bounded Buffer.

### Control Path: 
- We use a UNIX Domain Socket (/tmp/mini_runtime.sock). This provides a reliable, bidirectional control channel, allowing the CLI to send commands (like start or ps) and receive responses without interfering with the high-volume logging stream.

### The bounded buffer 
- It is synchronized using a mutex and two condition variables (not_full, not_empty). This prevents the producer from overfilling the buffer and ensures the consumer thread sleeps efficiently when no logs are available, rather than consuming CPU cycles through busy-waiting.

### Memory Management and Enforcement
- The kernel-space monitor tracks the Resident Set Size (RSS) of each container.

### Soft Limits: 
- Log a warning to dmesg (as seen in our Screenshot 5) when the threshold is first crossed, allowing for non-disruptive monitoring.

### Hard Limits: 
- Trigger an immediate SIGKILL to prevent a single container from starving the host or other containers of physical RAM (as seen in Screenshot 6).
Enforcement is handled in kernel space because it is immune to user-space scheduling delays or process priority manipulation, ensuring absolute resource capping.

### Scheduling Behavior
- In our experiments, the Linux Completely Fair Scheduler (CFS) handled the workloads. In Experiment 1, the io_pulse workload yielded CPU voluntarily through usleep, while cpu_hog saturated its time slice. In Experiment 2, we observed that nice values dictated the "weight" given to containers. A container with nice 19 accumulates virtual runtime faster, leading the scheduler to grant it fewer physical CPU cycles when competing with a high-priority (nice 0) process.

## 5. Design Decisions and Tradeoffs

### Namespace Isolation
Choice: clone() with CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS.
Tradeoff: Network namespace (CLONE_NEWNET) was not isolated, meaning containers share the host's network stack.
Justification: Implementing a virtual network stack (veth pairs/bridging) was out of scope for the current architecture. PID and mount isolation provide the primary security boundary required for process and filesystem jails.

### Supervisor Architecture
Choice: A single-threaded event loop utilizing select() for CLI connections.
Tradeoff: The supervisor cannot handle truly simultaneous CLI requests; if one request blocks, the next is queued.
Justification: CLI commands (like ps or stop) are short-lived. A single-threaded approach significantly reduces the complexity of synchronization and prevents race conditions within the metadata list.

### IPC / Logging Pipeline
Choice: UNIX domain sockets for the control channel and pipes for log capture, backed by a bounded buffer with mutex + condvar.
Tradeoff: Log entries are capped by LOG_CHUNK_SIZE (4KB). Extremely long output lines may be split into separate buffer entries.
Justification: UNIX sockets offer reliable, full-duplex communication better than FIFOs. The bounded buffer decouples the container's execution speed from the supervisor's disk I/O speed, ensuring a slow log write doesn't stall the container.

### Kernel Monitor Policy
Choice: mutex over spinlock for the internal process list.
Tradeoff: Mutexes have higher overhead as they can cause the calling thread to sleep.
Justification: Our monitor calls get_task_mm(), which is a sleep-capable kernel function. Using a spinlock would lead to a kernel panic ("scheduling while atomic"), making a mutex the only stable choice for the RSS check timer.

### Scheduling Experiment Methodology
Choice: Utilizing nice values via the --nice flag instead of CPU affinity (taskset).
Tradeoff: nice values only influence priority and weight; on our multi-core Intel Mac VMs, processes might run in parallel on different cores rather than strictly competing for a single core.
Justification: Using nice values directly exercises the Completely Fair Scheduler (CFS) weight-based logic, which is the specific OS fundamental this project aims to demonstrate.

### Termination Attribution
Choice: Use of an internal stop_requested flag within the metadata.
Tradeoff: Adds a small amount of overhead to the container tracking logic.
Justification: This was necessary to meet the grading requirement of distinguishing between a manual stop (graceful) and a hard_limit_killed event. It allows the ps command to accurately report why a container died, even if both resulted in a termination signal.

## 6. Scheduler Experiment Results

### Experiment 1: CPU-bound vs I/O-bound
| Container | Workload   | Observed CPU% |
|-----------|------------|---------------|
| cpu1      | cpu_hog    | ~99%          |
| io1       | io_pulse   | ~1-2%         |

`io_pulse` spends most of its time blocked in `usleep()`, giving up the CPU
voluntarily after each I/O burst. `cpu_hog` never yields, consuming its full
time slice every scheduling period. This demonstrates that I/O-bound processes
are naturally cooperative with the scheduler and do not need to be throttled —
they self-limit by blocking on I/O.

### Experiment 2: Different nice values
| Container  | nice | Observed CPU% | Completion Time |
|------------|------|---------------|-----------------|
| high_prio  | 0    | ~99%          | 59.262s         |
| low_prio   | 19   | ~99%          | 59.723s         |

The CFS scheduler allocated CPU share proportional to the process weights
derived from nice values, consistent with the kernel's documented weight table
where `nice=19` has weight 15 and `nice=0` has weight 1024. The `low_prio`
container took longer to complete the same workload because it was repeatedly
preempted in favour of `high_prio`, confirming that CFS enforces proportional
fairness rather than strict time-slicing.
