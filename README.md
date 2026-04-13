# Multi-Container Runtime

## 1. Team Information

| Name | SRN |
|------|-----|
| Mahathi S | PES2UG24AM084 |
| Leka N | PES2UG24AM082 |

---

## 2. Build, Load, and Run Instructions

### Prerequisites
- Ubuntu 22.04 or 24.04 VM
- Secure Boot OFF
- Install dependencies:
```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Build
```bash
cd boilerplate
make
```

### Prepare Root Filesystem
```bash
cd ~/OS-Jackfruit
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

### Copy Workloads into Rootfs
```bash
cd boilerplate
cp cpu_hog ../rootfs-alpha/
cp cpu_hog ../rootfs-beta/
cp memory_hog ../rootfs-alpha/
cp memory_hog ../rootfs-beta/
cp io_pulse ../rootfs-alpha/
cp io_pulse ../rootfs-beta/
```

### Load Kernel Module
```bash
sudo insmod boilerplate/monitor.ko
ls -l /dev/container_monitor
```

### Start Supervisor
```bash
sudo ./boilerplate/engine supervisor ./rootfs-base
```

### Launch Containers (in another terminal)
```bash
sudo ./boilerplate/engine start alpha ./rootfs-alpha /cpu_hog
sudo ./boilerplate/engine start beta ./rootfs-beta /cpu_hog
```

### CLI Commands
```bash
# List containers
sudo ./boilerplate/engine ps

# View logs
sudo ./boilerplate/engine logs alpha

# Stop a container
sudo ./boilerplate/engine stop alpha

# Start with memory limits
sudo ./boilerplate/engine start memtest ./rootfs-alpha /memory_hog --soft-mib 5 --hard-mib 10

# Start with scheduling priority
sudo ./boilerplate/engine start hipri ./rootfs-alpha /cpu_hog --nice -5
sudo ./boilerplate/engine start lopri ./rootfs-beta /cpu_hog --nice 10
```

### Cleanup
```bash
# Stop supervisor (Ctrl+C in supervisor terminal)
sudo rmmod monitor
sudo rm -f /tmp/engine.sock
```

### Watch Kernel Logs
```bash
sudo dmesg -w
```

---

## 3. Demo Screenshots

### Screenshot 1 — Multi-Container Supervision
<img width="940" height="698" alt="image" src="https://github.com/user-attachments/assets/1e4dbe00-9f0e-4b8e-a666-5f3c4a2f8f35" />

<img width="940" height="237" alt="image" src="https://github.com/user-attachments/assets/9eadc8b5-d43e-41e0-8369-34b484f6148e" />


### Screenshot 2 — Metadata Tracking
<img width="940" height="207" alt="image" src="https://github.com/user-attachments/assets/4cdbaf18-1bb6-4d59-af3c-ee95e3e7b3e7" />


### Screenshot 3 — Bounded-Buffer Logging
<img width="940" height="364" alt="image" src="https://github.com/user-attachments/assets/9519faac-91b5-45b1-a62a-c9df8081e132" />


### Screenshot 4 — CLI and IPC
<img width="805" height="234" alt="image" src="https://github.com/user-attachments/assets/91b13547-5897-493d-af07-de89c4279ef2" />
<img width="820" height="167" alt="image" src="https://github.com/user-attachments/assets/60bc82e0-7271-4aa0-90e7-a315f9ead095" />


### Screenshot 5 — Soft-Limit Warning
<img width="940" height="99" alt="image" src="https://github.com/user-attachments/assets/0806ca6b-0b90-4e9a-a699-7e1e84cced3b" />


### Screenshot 6 — Hard-Limit Enforcement
<img width="940" height="62" alt="image" src="https://github.com/user-attachments/assets/eb13a681-d560-40c0-9faf-805a25a48c36" />
<img width="940" height="181" alt="image" src="https://github.com/user-attachments/assets/46dba8e1-ad99-4af5-aeba-7bfe91e37c5e" />


### Screenshot 7 — Scheduling Experiment
<img width="940" height="181" alt="image" src="https://github.com/user-attachments/assets/87f01b98-0079-4716-be8f-9b900cf3a528" />


### Screenshot 8 — Clean Teardown
<img width="940" height="142" alt="image" src="https://github.com/user-attachments/assets/0f00189e-c866-43c4-b297-56d37df25f85" />


---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

Each container is created using `clone()` with three namespace flags:
- `CLONE_NEWPID` — gives the container its own PID namespace. The first process
  inside sees itself as PID 1, and cannot see host processes.
- `CLONE_NEWUTS` — gives the container its own hostname, set to "container".
- `CLONE_NEWNS` — gives the container its own mount namespace, allowing it to
  mount its own `/proc` without affecting the host.

After `clone()`, the child calls `chroot()` into its assigned rootfs directory.
This restricts the filesystem view to only that directory tree. `/proc` is then
mounted inside the container so tools like `ps` work correctly.

The host kernel is still shared — the same scheduler, memory allocator, and
device drivers serve all containers. There is no separate kernel per container.
Network and user namespaces are not used in this implementation, so network
interfaces and UIDs are shared with the host.

### 4.2 Supervisor and Process Lifecycle

A long-running supervisor is necessary because container processes are children
of the process that called `clone()`. If the parent exits, orphaned children
are reparented to init (PID 1), making it impossible to track their exit status
or collect logs.

The supervisor installs a `SIGCHLD` handler using `sigaction()`. When a container
exits, the kernel delivers SIGCHLD to the supervisor. The handler calls
`waitpid(-1, &status, WNOHANG)` in a loop to reap all exited children without
blocking. It then updates the container metadata with the exit code and state.

Container metadata is stored in a global array protected by a mutex. Each entry
tracks the container ID, host PID, start time, state, memory limits, log path,
and exit status.

### 4.3 IPC, Threads, and Synchronization

The project uses two IPC mechanisms:

**Path A — Logging (pipe):** Each container's stdout and stderr are connected
to the supervisor via a pipe created before `clone()`. The child inherits the
write end and the supervisor keeps the read end. A producer thread reads from
the pipe and inserts lines into a bounded buffer. A consumer thread removes
lines and writes them to a log file.

The bounded buffer uses a mutex to protect the head/tail/count fields, a
condition variable `not_empty` to block the consumer when the buffer is empty,
and a condition variable `not_full` to block the producer when the buffer is
full. Without these primitives, concurrent reads and writes to the buffer would
corrupt the head/tail pointers, causing lost or duplicated log lines.

**Path B — Control (UNIX domain socket):** The CLI client connects to a UNIX
socket created by the supervisor. It sends a command string and reads the
response. This is a separate channel from the logging pipes so that control
commands are never mixed with log data.

Container metadata is accessed from both the SIGCHLD handler and the socket
handler, so all metadata reads and writes are protected by a separate mutex
`meta_lock`.

### 4.4 Memory Management and Enforcement

RSS (Resident Set Size) measures the amount of physical RAM currently mapped
into a process's address space. It does not measure virtual memory, shared
libraries counted once per process, or memory that has been swapped out.

Soft and hard limits serve different purposes. A soft limit is a warning
threshold — the process is notified (via dmesg) but allowed to continue running.
This gives the application a chance to reduce its memory usage. A hard limit is
an enforcement threshold — the process is sent SIGKILL and terminated
immediately.

Memory enforcement belongs in kernel space because a user-space monitor can
be fooled or delayed. A misbehaving container process could consume memory
faster than a user-space polling loop can detect. The kernel module checks RSS
every second using a timer and can send SIGKILL atomically with the check,
with no race window between detection and enforcement.

### 4.5 Scheduling Behavior

Linux uses the Completely Fair Scheduler (CFS). CFS assigns CPU time based on
a virtual runtime counter. Processes with lower nice values have their virtual
runtime advance more slowly, meaning the scheduler picks them more often.

In our experiment, `hipri` ran with nice -5 and `lopri` ran with nice 10.
Both ran for 10 seconds of wall time. The final accumulator for `hipri` was
`12183341357141558892` compared to `8345701271268554399` for `lopri`. This
shows that `hipri` completed more computation in the same wall-clock time,
confirming that CFS allocated more CPU share to the higher-priority process.

The difference is not as dramatic as expected on a multi-core VM because both
processes can run on separate cores simultaneously. On a single-core system
the gap would be larger.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation
**Choice:** PID, UTS, and mount namespaces via `clone()` with `chroot()`.
**Tradeoff:** `chroot()` is simpler than `pivot_root()` but allows escape via
`..` traversal if the container process has root. For a course project this is
acceptable.
**Justification:** Sufficient isolation for the project scope without the
complexity of full pivot_root implementation.

### Supervisor Architecture
**Choice:** Single-process supervisor with a non-blocking accept loop.
**Tradeoff:** Cannot handle multiple simultaneous CLI connections. A
multi-threaded server would be more robust.
**Justification:** The CLI commands are short-lived and sequential, so a
single-threaded loop is sufficient and avoids additional synchronization
complexity.

### IPC and Logging
**Choice:** UNIX domain socket for control, pipes for logging, bounded buffer
with mutex and condition variables.
**Tradeoff:** The bounded buffer adds latency compared to direct writes, but
prevents log data from blocking container execution.
**Justification:** Decoupling log production from log writing ensures containers
are never blocked waiting for disk I/O.

### Kernel Monitor
**Choice:** Timer-based polling every 1 second with RSS checks.
**Tradeoff:** A 1-second polling interval means a container could exceed its
hard limit by a significant amount before being killed.
**Justification:** Event-driven memory notification (e.g., cgroups memory
events) would be more accurate but requires cgroup integration. Timer-based
polling is simpler and sufficient for demonstration purposes.

### Scheduling Experiments
**Choice:** nice values to differentiate priority between containers.
**Tradeoff:** nice only affects CFS weight, not real-time scheduling. For
stronger isolation, cgroups CPU quotas would be more precise.
**Justification:** nice is the simplest available mechanism and produces
measurable, observable differences in CPU allocation.

---

## 6. Scheduler Experiment Results

### Setup
Two containers ran the same `cpu_hog` workload simultaneously:
- `hipri`: nice value = -5 (higher priority)
- `lopri`: nice value = +10 (lower priority)

### Results

| Container | Nice Value | Duration | Final Accumulator |
|-----------|-----------|----------|-------------------|
| hipri | -5 | 10s | 12,183,341,357,141,558,892 |
| lopri | +10 | 10s | 8,345,701,271,268,554,399 |

### Analysis
The high-priority container (`hipri`) achieved a ~46% higher final accumulator
value than the low-priority container (`lopri`) in the same wall-clock time.
This demonstrates that the Linux CFS scheduler honored the nice value difference
and allocated more CPU time to the higher-priority process.

The result confirms CFS scheduling theory: lower nice values increase a
process's weight in the scheduler, causing its virtual runtime to advance more
slowly relative to other processes, resulting in more frequent scheduling and
more CPU time over a fixed wall-clock period.
