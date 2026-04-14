# OS-Jackfruit — Multi-Container Runtime

## 1. Team Information

| Name | SRN |
|---|---|
| Divya Gupta | PES2UG24CS901 |
| Rosalin Verma | PES2UG24CS677 |

**Course:** UE24CS242B — Operating Systems
**Institution:** PES University, Bengaluru
**Semester:** Jan – May 2026
**GitHub Repository:** https://github.com/rosa36-x/OS-Jackfruit

## 2. Build, Load, and Run Instructions

### Prerequisites

- Ubuntu 22.04 or 24.04 in a VM
- Secure Boot must be OFF (required for kernel module loading)
- WSL is NOT supported

### Install Dependencies

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Run Environment Check

```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
```

Fix any issues reported before proceeding.

### Prepare Root Filesystem

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Make one writable copy per container
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

### Build the Project

```bash
make
```

This builds `engine` (user-space runtime) and `monitor.ko` (kernel module) together.

### Load the Kernel Module

```bash
sudo insmod monitor.ko

# Verify the control device exists
ls -l /dev/container_monitor
```

### Start the Supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

Keep this running in a terminal. Open a new terminal for CLI commands.

### Launch Containers

```bash
# Start containers in the background
sudo ./engine start alpha ./rootfs-alpha /bin/sh
sudo ./engine start beta ./rootfs-beta /bin/sh

# Or run a container in the foreground
sudo ./engine run alpha ./rootfs-alpha /bin/sh
```

### Use the CLI

```bash
# List all tracked containers and their metadata
sudo ./engine ps

# View logs of a container
sudo ./engine logs alpha

# Stop a container
sudo ./engine stop alpha
sudo ./engine stop beta
```

### Run Workloads Inside a Container

```bash
# Copy workload binary into rootfs before launching the container
cp workload_cpu ./rootfs-alpha/
cp workload_io ./rootfs-beta/
```

### View Kernel Logs

```bash
dmesg | tail -20
```

### Unload the Kernel Module and Clean Up

```bash
sudo rmmod monitor

# Verify no zombie processes remain
ps aux | grep engine
```
## 3. Demo with Screenshots

### Screenshot 1 — Multi-Container Supervision
![Multi-container supervision](screenshots/task1_multicontainer.png)

**Caption:** Two containers (alpha and beta) running simultaneously under a single
supervisor process. The supervisor remains alive while both containers execute
independently in isolated namespaces.

---

### Screenshot 2 — Metadata Tracking
![Metadata tracking](screenshots/task1_ps.png)

**Caption:** Output of `sudo ./engine ps` showing tracked metadata for each container —
container ID, host PID, start time, current state, memory limits, log file path,
and exit status.

---

### Screenshot 3 — Bounded-Buffer Logging
![Bounded-buffer logging](screenshots/task3_logging.png)

**Caption:** Log file contents captured through the bounded-buffer logging pipeline.
Shows producer (container stdout/stderr) writing into the shared buffer and the
consumer thread flushing data to the per-container log file.

---

### Screenshot 4 — CLI and IPC
![CLI and IPC](screenshots/task2_cli.png)

**Caption:** A CLI command (`start`/`stop`/`logs`) being issued from the client and
the supervisor responding over the UNIX domain socket, demonstrating the second
IPC mechanism separate from the logging pipe.

---

### Screenshot 5 — Soft-Limit Warning
![Soft-limit warning](screenshots/task4_softlimit.png)

**Caption:** `dmesg` output showing the kernel module logging a soft-limit warning
event when a container's RSS first exceeded the configured soft memory limit.
The container continues running at this stage.

---

### Screenshot 6 — Hard-Limit Enforcement
![Hard-limit enforcement](screenshots/task4_hardlimit.png)

**Caption:** `dmesg` output showing the kernel module terminating a container after
its RSS exceeded the hard memory limit. The supervisor metadata reflects the
container state as `killed` due to hard-limit violation.

---

### Screenshot 7 — Scheduling Experiment
![Scheduling experiment](screenshots/task5_scheduling.png)

**Caption:** Terminal output comparing two containers running CPU-bound workloads
with different `nice` values. Observable difference in CPU share and completion
time confirms Linux CFS scheduler behavior based on priority weights.

---

### Screenshot 8 — Clean Teardown
![Clean teardown](screenshots/task6_teardown.png)

**Caption:** `ps aux` output and supervisor exit messages after stopping all
containers and unloading the kernel module. No zombie processes or stale
metadata remain after full shutdown.

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

Linux containers achieve isolation through **namespaces** — kernel features that
wrap global resources so each container sees its own private view of the system.

Our runtime uses three namespace types:

- **PID namespace:** Each container gets its own PID numbering starting from 1.
  The container's init process is PID 1 inside, but has a different host PID
  visible to the supervisor. This prevents containers from seeing or signalling
  each other's processes.
- **UTS namespace:** Each container has its own hostname and domain name,
  isolating system identity without affecting the host.
- **Mount namespace:** Each container gets its own filesystem mount table.
  Combined with `chroot`/`pivot_root` into the Alpine `rootfs`, the container
  cannot access the host filesystem. `/proc` is mounted fresh inside each
  container so process information is scoped correctly.

**What the host kernel still shares with all containers:**
The host kernel itself is shared — there is no separate kernel per container
unlike in a VM. System calls go to the same kernel. The network stack, unless
a network namespace is added, is also shared. This is why containers are
lighter than VMs but provide weaker isolation.

---

### 4.2 Supervisor and Process Lifecycle

A long-running parent supervisor is necessary because:

- **Zombie prevention:** When a child process exits, its entry stays in the
  process table until the parent calls `wait()`/`waitpid()`. Without a persistent
  parent, exited containers become zombies. Our supervisor handles `SIGCHLD`
  and reaps children promptly.
- **Metadata tracking:** The supervisor maintains per-container state
  (`starting`, `running`, `stopped`, `killed`) in a shared data structure.
  This is only possible with a process that outlives individual containers.
- **Signal delivery:** The supervisor forwards `SIGTERM` on a `stop` command
  and `SIGKILL` on a hard-limit violation. Only the parent (or a process with
  the right permissions) can reliably signal child containers.
- **Orderly shutdown:** On receiving `SIGINT`/`SIGTERM` itself, the supervisor
  stops all running containers, joins logging threads, closes file descriptors,
  and exits cleanly rather than orphaning containers.

Process creation uses `fork()` to create a new process and `exec()` to replace
it with the container workload inside the prepared namespace and `chroot`
environment. The supervisor retains the child's host PID for metadata and
signal delivery.

---

### 4.3 IPC, Threads, and Synchronization

Our project uses two IPC mechanisms:

**1. Pipes (logging pipeline)**
Each container's `stdout` and `stderr` are redirected into a pipe at `fork()`
time. A dedicated consumer thread in the supervisor reads from the pipe and
writes data into a bounded shared buffer.

**2. UNIX Domain Socket (CLI control channel)**
The CLI client connects to a UNIX domain socket to send commands (`start`,
`stop`, `ps`, `logs`) to the supervisor. This is separate from the logging
pipe so that control and data paths do not interfere.

**Bounded Buffer and Synchronization:**

The bounded buffer sits between log producer threads (one per container) and
the consumer thread (writes to log files). Without synchronization:

- A producer could write while the consumer reads → **data corruption**
- A producer could overflow the buffer → **lost log data**
- The consumer could read an empty buffer → **invalid memory access**

We use:

| Primitive | Purpose |
|---|---|
| `mutex` | Protects the buffer head/tail pointers and container metadata struct under concurrent access |
| `condition variable` | Producer waits when buffer is full; consumer waits when buffer is empty — avoids busy-waiting |
| `SIGCHLD` handler + `waitpid()` | Reaps children without racing against the main supervisor loop |

Shared container metadata (state, PID, exit status) is also accessed by both
the CLI handler thread and the `SIGCHLD` handler, so all reads and writes to
the metadata array are done under the same mutex.

---

### 4.4 Memory Management and Enforcement

**What RSS measures:**
RSS (Resident Set Size) is the amount of physical RAM currently held by a
process — the pages that are actually loaded in memory right now. It does not
count swapped-out pages, memory-mapped files not yet faulted in, or shared
library pages counted once per process even if shared.

**Why soft and hard limits are different policies:**

- **Soft limit** is a warning threshold. When a container's RSS first crosses
  it, the kernel module logs an event to `dmesg` but does not interrupt the
  container. This gives the supervisor a chance to react gracefully — it can
  notify the user or begin a controlled shutdown.
- **Hard limit** is an enforcement threshold. When RSS exceeds it, the kernel
  module sends `SIGKILL` to the container process. No recovery is attempted.
  The supervisor detects this via `SIGCHLD` and marks the container state as
  `killed`.

**Why enforcement belongs in kernel space:**
A user-space monitor can only observe RSS by polling `/proc/<pid>/status`,
which introduces a time gap between the violation and the response. During
that gap the process could allocate much more memory. The kernel module runs
in kernel space and can check RSS periodically with a timer, holding kernel
locks — making enforcement faster and tamper-proof. A container process cannot
disable the kernel module's monitoring by catching or ignoring signals the way
it could interfere with a user-space watcher.

---

### 4.5 Scheduling Behavior

Linux uses the **Completely Fair Scheduler (CFS)** for normal processes. CFS
assigns CPU time proportional to each process's **weight**, which is derived
from its `nice` value. A lower `nice` value means higher priority and a larger
weight, so CFS gives it a bigger share of CPU time.

**Our experiment:**
We ran two CPU-bound containers simultaneously:

- `alpha` — `nice` value 0 (default priority)
- `beta` — `nice` value 10 (lower priority)

**Observed results** (see Screenshot 7):
`alpha` consistently received approximately twice the CPU share of `beta` over
the same measurement window. Completion time for an identical workload was
noticeably shorter for `alpha`.

**What this shows:**
CFS does not starve `beta` — it still makes progress — but it enforces the
priority difference through weighted fair queueing. The scheduler's goal of
fairness is relative to weight, not absolute equality. For I/O-bound workloads
the difference is smaller because those processes voluntarily sleep, giving up
CPU time regardless of priority.

## 5. Design Decisions and Tradeoffs

### 5.1 Namespace Isolation

**Design choice:**
We use PID, UTS, and mount namespaces via `clone()` flags (`CLONE_NEWPID`,
`CLONE_NEWUTS`, `CLONE_NEWNS`) when spawning each container process.
Filesystem isolation is completed with `chroot` into the Alpine rootfs copy.

**Tradeoff:**
We did not implement network namespaces (`CLONE_NEWNET`). This means all
containers share the host network stack and can bind to the same ports,
which could cause conflicts in a multi-container setup.

**Justification:**
Network namespace setup requires additional configuration (virtual ethernet
pairs, bridge setup) that is outside the scope of this project. PID, UTS,
and mount namespaces are sufficient to demonstrate the core isolation
principles required by the project guide, and keeping the network shared
simplifies container-to-host communication during testing.

---

### 5.2 Supervisor Architecture

**Design choice:**
The supervisor runs as a single long-lived process that forks children for
containers, handles `SIGCHLD` for reaping, and accepts CLI commands over a
UNIX domain socket on a dedicated handler thread.

**Tradeoff:**
A single supervisor process means a supervisor crash takes down the entire
runtime and all tracking metadata. A more resilient design would persist
metadata to disk so it can recover after a restart.

**Justification:**
For an educational implementation, in-memory metadata is simpler and avoids
the complexity of serialisation and crash recovery. The single-process model
also makes signal handling straightforward — there is one clear owner of
`SIGCHLD` and `SIGTERM` with no ambiguity about which process should reap
which child.

---

### 5.3 IPC and Logging Pipeline

**Design choice:**
We use pipes for the logging data path (container stdout/stderr → supervisor
bounded buffer → log file) and a UNIX domain socket for the CLI control path.
These are two separate IPC mechanisms as required.

**Tradeoff:**
Pipes are unidirectional and have a fixed kernel buffer size (typically 64KB).
If the consumer thread falls behind a very verbose container, the pipe buffer
fills up and the container blocks on `write()`, which could affect container
performance.

**Justification:**
Pipes are the most natural fit for streaming stdout/stderr from a child process
to a parent — they are set up at `fork()` time with a simple `dup2()` redirect
and require no extra setup. The bounded buffer in user space absorbs bursts
and keeps the consumer decoupled from the producer, reducing the chance of
the pipe filling up under normal workloads.

---

### 5.4 Kernel Memory Monitor

**Design choice:**
The kernel module maintains a linked list of monitored PIDs protected by a
mutex. A periodic kernel timer fires to check each entry's RSS via
`get_task_mm()` and `get_mm_rss()`. Soft-limit events are logged to `dmesg`.
Hard-limit violations trigger `send_sig(SIGKILL, task, 0)`.

**Tradeoff:**
A timer-based polling approach means there is a window between when a process
exceeds a limit and when the module detects it. A process could allocate a
large burst of memory between two timer firings without being caught
immediately.

**Justification:**
Polling with a kernel timer is significantly simpler than hooking into the
memory allocator path (e.g., `mm` fault handlers or cgroup memory events),
which would require deeper kernel internals knowledge. For the purposes of
this project the polling interval is short enough to demonstrate the
soft/hard limit behavior reliably. Production systems use cgroups memory
controller for true event-driven enforcement.

---

### 5.5 Scheduling Experiments

**Design choice:**
We used `nice()` to set different scheduling priorities for containers running
CPU-bound workloads, and measured CPU share and wall-clock completion time
as observable outcomes.

**Tradeoff:**
`nice` values only affect CFS weight within the same scheduling class
(`SCHED_NORMAL`). They do not give real-time guarantees. A container with
`nice -20` can still be preempted by a real-time process on the host.

**Justification:**
`nice` values are the simplest and most portable way to influence CFS
scheduling without requiring root-level real-time scheduling permissions
for every experiment. They produce clearly observable and measurable
differences in CPU share, making them a good fit for demonstrating scheduler
behavior in an educational context.  


