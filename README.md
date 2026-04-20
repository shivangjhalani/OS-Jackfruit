# Lightweight Container Runtime with Kernel Memory Monitor

---

## 1. Team Information

| Name | SRN |
|------|-----|
| Aariz Qureshi | PES1UG24CS008 |
| Amogh Sharma | PES1UG24CS053 |

---

## 2. Project Summary

This project implements a lightweight Linux container runtime in C with a long-running parent supervisor and a kernel-space memory monitor. The runtime can launch multiple isolated containers concurrently, maintain metadata for each container, provide a CLI for lifecycle management, and capture container output through a bounded-buffer logging pipeline. A Linux kernel module monitors registered container processes, emits soft-limit warnings, and enforces hard memory limits through kernel-space termination.

The project has two tightly integrated parts:

1. **`engine.c`** — user-space runtime, supervisor, CLI handling, logging pipeline, and container lifecycle management.
2. **`monitor.c`** — Linux Kernel Module (LKM) that tracks container processes and applies soft and hard memory policies through `ioctl`.

---

## 3. Environment Requirements

- **Ubuntu 22.04 or Ubuntu 24.04** running in a VM
- **Secure Boot disabled** so the kernel module can be loaded
- **Not supported on WSL**

Install dependencies:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

---

## 4. Build, Load, and Run Instructions

### 🔧 Step 1 — Environment Preflight

```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
cd ..
```

Fix any issues reported before moving on.

---

### 🏗️ Step 2 — Build the Project

```bash
make
```

For a CI-safe compile-only check (no kernel headers, no `sudo`, no rootfs needed):

```bash
make -C boilerplate ci
```

---

### 📦 Step 3 — Prepare the Base Root Filesystem

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
```

---

### 🔌 Step 4 — Load the Kernel Module

```bash
sudo insmod monitor.ko
```

---

### ✅ Step 5 — Verify the Control Device

```bash
ls -l /dev/container_monitor
```

---

### 🚀 Step 6 — Start the Supervisor

Open a dedicated terminal and run:

```bash
sudo ./engine supervisor ./rootfs-base
```

The supervisor stays alive and manages all containers until explicitly stopped.

---

### 📁 Step 7 — Create Writable Root Filesystems

> ⚠️ Do **not** run two live containers against the same writable rootfs.

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

---

### ▶️ Step 8 — Start Containers

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 64 --hard-mib 96
```

**Default limits** (when not specified):
- Soft limit: `40 MiB`
- Hard limit: `64 MiB`

---

### 📊 Step 9 — List Tracked Containers

```bash
sudo ./engine ps
```

---

### 📜 Step 10 — Inspect Logs

```bash
sudo ./engine logs alpha
```

---

### 🎛️ Step 11 — Run a Foreground Container

The `run` command launches a container, waits for it to finish, and returns its final status:

```bash
sudo ./engine run gamma ./rootfs-gamma /bin/sh --soft-mib 48 --hard-mib 80 --nice 5
```

> If the `run` client receives `SIGINT` or `SIGTERM`, it forwards the termination request to the supervisor and continues waiting for the final exit status.

---

### 🧪 Step 12 — Run Workloads for Demonstration

Copy helper binaries into the container rootfs before launch:

```bash
cp cpu_hog ./rootfs-alpha/
cp io_pulse ./rootfs-beta/
cp memory_hog ./rootfs-alpha/
```

**Scheduling experiment:**

```bash
sudo ./engine start cpu ./rootfs-alpha ./cpu_hog --nice 10
sudo ./engine start io ./rootfs-beta ./io_pulse --nice 0
```

**Memory-limit experiment:**

```bash
sudo ./engine start mem ./rootfs-alpha ./memory_hog --soft-mib 48 --hard-mib 80
```

---

### 🛑 Step 13 — Stop Containers

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
```

---

### 📟 Step 14 — Inspect Kernel Logs

```bash
dmesg | tail -n 50
```

---

### ❌ Step 15 — Unload the Module and Clean Up

```bash
sudo rmmod monitor
```

---

## 5. Canonical CLI Contract

```bash
engine supervisor <base-rootfs>
engine start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
engine run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]
engine ps
engine logs <id>
engine stop <id>
```

| Command | Semantics |
|---------|-----------|
| `supervisor` | Starts the long-running parent process that owns container metadata and the logging pipeline |
| `start` | Launches a background container and returns once the supervisor accepts and records the request |
| `run` | Launches a foreground container and waits until it exits, then returns final status |
| `ps` | Prints tracked container metadata: ID, host PID, state, limits, and final reason |
| `logs` | Prints or tails the persistent log file for a given container |
| `stop` | Requests a clean shutdown for the specified container |

---

## 6. CI Smoke Check

This repository includes a GitHub Actions workflow that performs a minimal CI-safe build check on every push. It does **not** require `sudo`, kernel headers, module loading, rootfs setup, or a running supervisor.

The CI target compiles:
- `engine` (user-space runtime)
- `cpu_hog`, `io_pulse`, `memory_hog` (workload binaries)

And verifies that `./boilerplate/engine` with no arguments exits with a non-zero status (usage error), confirming the binary runs correctly.

```bash
make -C boilerplate ci
```

---

## 7. Demo with Screenshots

### 1. Multi-Container Supervision

![Picture1](Screenshots/Picture1.png)

*The supervisor process is running and managing multiple containers. The right panel shows `ps aux | grep engine` output — multiple container entries are visible under the single supervisor process, confirming concurrent multi-container management.*

---

### 2. Metadata Tracking

![Picture2](Screenshots/Picture2.png)

*Output of `engine ps` showing both `alpha` and `beta` containers in the `running` state with their host PIDs, soft limits (40 MiB), and hard limits (64 MiB) tracked in supervisor metadata.*

---

### 3. Bounded-Buffer Logging

![Picture3](Screenshots/Picture3.png)

*Output of `engine logs alpha` — container stdout/stderr captured through pipes into the supervisor's bounded-buffer logging pipeline and written to a persistent per-container log file. Each line of output was routed through the producer-consumer buffer.*

---

### 4. CLI and IPC

![Picture4](Screenshots/Picture4.png)

*A `stop alpha` command is issued from the CLI client, which sends a request over the UNIX domain socket IPC control channel to the supervisor. The follow-up `engine ps` confirms the supervisor updated the `alpha` container state to `stopped` while `beta` continues running — demonstrating the control IPC path.*

---

### 5. Soft-Limit Warning

![Picture5](Screenshots/Picture5.png)

*`dmesg` output showing the kernel module emitting `SOFT LIMIT` warning events for containers (`memtest3`, `memtest4`) that crossed their soft memory threshold. The warnings are logged once per container and visible in the kernel ring buffer.*

---

### 6. Hard-Limit Enforcement

![Picture6](Screenshots/Picture6.png)

*`engine ps` output after memory-intensive workloads exceeded their hard limits. Containers `memtest3` and `memtest4` show state `hard_limit_killed` — confirming the kernel module sent `SIGKILL` and the supervisor correctly classified the termination reason in metadata.*

---

### 7. Scheduling Experiment

![Picture7](Screenshots/Picture7.png)

*`top` output during a concurrent scheduling experiment. The `fast` container (nice = −5) shows ~99.4% CPU usage while the `slow` container (nice = 10) shows ~99.1% but receives lower scheduling priority. The I/O-bound process (`seed`, PID 2341) remains at ~4% CPU, demonstrating CFS responsiveness to I/O-heavy workloads.*

---

### 8. Clean Teardown

![Picture8](Screenshots/Picture8.png)

*After stopping all containers, `engine ps` shows all containers in their final states (`stopped`, `hard_limit_killed`, `exited`). The `ps aux | grep defunct` command confirms zero zombie processes remain — all children were correctly reaped by the supervisor and all logging threads exited cleanly.*

---

## 8. Engineering Analysis

### 1. Isolation Mechanisms

Our runtime achieves isolation using a combination of Linux namespaces and filesystem isolation techniques.

Each container is created using the `clone()` system call with `CLONE_NEWPID`, `CLONE_NEWUTS`, and `CLONE_NEWNS` flags. The **PID namespace** ensures that processes inside the container have their own independent process tree starting from PID 1, isolating them from host processes. The **UTS namespace** allows each container to have its own hostname, providing logical separation. The **mount namespace** ensures that filesystem mount operations inside a container do not affect the host or other containers.

For filesystem isolation, we use `chroot()` to restrict the container's root directory to its assigned rootfs. This ensures the container cannot access files outside its designated filesystem tree. Additionally, `/proc` is mounted inside the container to provide process visibility within the namespace:

```c
mount("proc", "/proc", "proc", 0, NULL);
```

However, all containers still share the same underlying Linux kernel. This means kernel resources such as CPU scheduling, memory management, and device drivers are shared across containers — a fundamental property of containerization compared to full virtualization.

---

### 2. Supervisor and Process Lifecycle

A long-running supervisor process is central to our design. Instead of launching containers as independent processes, the supervisor maintains control over all container lifecycles.

When a container is started, the supervisor uses `clone()` to create a child process with isolated namespaces. It stores metadata such as container ID, PID, state, start time, and resource limits — allowing it to track and manage multiple containers concurrently.

The supervisor handles process termination using `waitpid()` in a non-blocking loop to reap exited children and prevent zombie processes. When a container exits, its state is updated based on the reason for termination: **normal exit**, **manual stop**, or **hard-limit kill**.

Signal handling plays a critical role. When a user issues a `stop` command, the supervisor sets an internal `stop_requested` flag and sends a termination signal to the container. This allows the system to distinguish between intentional termination and forced termination due to resource limits — which is visible in `ps` output.

---

### 3. IPC, Threads, and Synchronization

The system uses two distinct IPC mechanisms:

- **Path A (Logging):** Pipes capture `stdout` and `stderr` from container processes and stream them to the supervisor.
- **Path B (Control):** A UNIX domain socket handles communication between CLI client processes and the supervisor daemon.

For logging, we implemented a **bounded-buffer producer-consumer model**. Producer threads read container output from pipes and insert log entries into a shared buffer. A consumer thread removes entries from the buffer and writes them to per-container log files.

We use a `pthread_mutex_t` paired with `pthread_cond_t` condition variables to synchronize shared buffer access:

- **Without synchronization:** multiple producers could overwrite buffer entries, corrupt indices, or lose wake-up signals, and consumers could read inconsistent data.
- **With mutex + condition variables:** producers sleep when the buffer is full, consumers sleep when it is empty — avoiding busy-waiting, preserving correctness under contention, and enabling clean shutdown by waking blocked threads to flush remaining entries.

The bounded buffer also prevents uncontrolled memory growth and applies backpressure when producers outpace consumers.

Container metadata (state, exit reason, stop flags, log ownership) is protected by a separate mutex so the logging path does not hold unrelated locks or introduce unnecessary contention.

---

### 4. Memory Management and Enforcement

The kernel module monitors memory usage using **RSS (Resident Set Size)**, which represents the portion of a process's memory currently resident in physical RAM. RSS does not include swapped-out memory, untouched virtual address space, or all forms of shared mappings — it reflects only physically resident pages.

We implement two distinct limit policies:

| Limit Type | Behavior |
|------------|----------|
| **Soft limit** | Log a `SOFT LIMIT` warning to `dmesg` once per container crossing |
| **Hard limit** | Send `SIGKILL` to terminate the process immediately |

Enforcement belongs in **kernel space** because:

1. The kernel has authoritative, real-time access to process memory accounting.
2. A user-space monitor can observe usage, but cannot match the kernel's ability to enforce limits reliably — a fast-growing process can outrun a user-space polling loop between checks.

The supervisor sets the `stop_requested` flag only for user-initiated stops. Termination classified as `hard_limit_killed` requires `SIGKILL` without `stop_requested` being set, ensuring the two paths are cleanly distinguishable in `ps` output.

---

### 5. Scheduling Behavior

We conducted experiments using CPU-bound and I/O-bound workloads to observe Linux CFS (Completely Fair Scheduler) behavior.

**CPU-bound processes** consume CPU continuously, while **I/O-bound processes** frequently yield the CPU and block on I/O. Our observations show:

- CPU-bound containers dominate CPU usage when competing with each other.
- I/O-bound containers remain responsive even alongside heavy CPU workloads because CFS prioritizes them quickly after wake-up.
- Adjusting `nice` values shifts the virtual runtime (`vruntime`) accumulation rate, directly changing the proportion of CPU time each CPU-bound container receives.

This connects to three key scheduling goals:

- **Fairness**: CPU time is distributed proportionally among runnable tasks.
- **Responsiveness**: I/O-bound tasks are scheduled quickly after waking up from blocking I/O.
- **Throughput**: CPU-bound tasks utilize available CPU cycles effectively without leaving the CPU idle.

---

## 9. Design Decisions and Tradeoffs

### 1. Namespace Isolation

| | |
|--|--|
| **Choice** | `clone()` with `CLONE_NEWPID`, `CLONE_NEWUTS`, `CLONE_NEWNS` + `chroot()` |
| **Tradeoff** | `chroot()` is weaker than `pivot_root()` — possible escape via unclosed file descriptors or mount propagation |
| **Justification** | Significantly simpler to implement while providing sufficient isolation for the project scope |

---

### 2. Supervisor Architecture

| | |
|--|--|
| **Choice** | Centralized long-running supervisor as the single parent of all managed containers |
| **Tradeoff** | Introduces a single point of failure — if the supervisor crashes, all container metadata and IPC is lost |
| **Justification** | Simplifies lifecycle management, metadata tracking, reaping, logging coordination, and command handling enormously |

---

### 3. IPC and Logging

| | |
|--|--|
| **Choice** | Pipes for log capture (Path A) + UNIX domain socket for control commands (Path B) |
| **Tradeoff** | Two IPC mechanisms increase implementation complexity and surface area |
| **Justification** | Clean separation of data plane and control plane improves modularity, avoids log-command interference, and matches the different communication patterns |

---

### 4. Kernel Monitor

| | |
|--|--|
| **Choice** | Kernel-space LKM for memory tracking and enforcement, with `ioctl` registration |
| **Tradeoff** | Kernel code is harder to debug and carries higher risk than user-space code |
| **Justification** | Only the kernel provides accurate, real-time RSS access and can enforce policies with sufficient authority and timing guarantees |

---

### 5. Scheduling Experiments

| | |
|--|--|
| **Choice** | Synthetic workloads (`cpu_hog`, `io_pulse`, `memory_hog`) with configurable `nice` values |
| **Tradeoff** | Synthetic workloads are simpler than real applications and may not capture every real-world scheduling pattern |
| **Justification** | They provide repeatable, controlled, easy-to-explain behavior for demonstrating scheduler and memory-management concepts |

---

## 10. Scheduler Experiment Results

### Experiment 1: CPU-Bound vs I/O-Bound

**Setup:**

```bash
sudo ./engine start cpu ./rootfs-alpha ./cpu_hog --nice 0
sudo ./engine start io ./rootfs-beta ./io_pulse --nice 0
```

**Observation:**

The `top` output (Screenshot 7) shows:

| Process | Type | Nice | CPU Usage |
|---------|------|------|-----------|
| `fast` (cpu_hog) | CPU-bound | −5 | ~99.4% |
| `slow` (cpu_hog) | CPU-bound | 10 | ~99.1% (deprioritized by CFS) |
| `seed` process | I/O-bound | 0 | ~4.0% |

- The CPU-bound containers consumed nearly all available CPU.
- The I/O-bound process remained responsive and continued without delay despite the CPU pressure.

---

### Experiment 2: CPU-Bound vs CPU-Bound with Different Nice Values

**Setup:**

```bash
sudo ./engine start fast ./rootfs-alpha ./cpu_hog --nice -5
sudo ./engine start slow ./rootfs-beta ./cpu_hog --nice 10
```

**Observation:**

CFS assigns lower `vruntime` accumulation to the higher-priority container (`fast`, nice = −5), so it is scheduled more frequently. The lower-priority container (`slow`, nice = 10) receives less CPU time proportionally. The 15-step difference in nice values results in measurable throughput differences between the two identical workloads.

---

### Analysis

These results demonstrate:

- **Fairness:** CFS distributes CPU time proportionally, adjusted by `nice` weight — neither heavy task is starved.
- **Responsiveness:** I/O-bound tasks are favored because they frequently block and are rescheduled quickly on wake-up, maintaining interactivity.
- **Effect of `nice`:** A 15-step difference in nice values creates a significant CPU share difference between identical CPU-bound workloads.
- **Throughput vs Latency:** CPU-bound tasks maximize throughput but at the cost of latency for other tasks; I/O-bound tasks sacrifice throughput for low latency and responsiveness.

---

## 11. Cleanup and Teardown Verification

The final demo (Screenshot 8) explicitly verifies:

- ✅ All exited containers are reaped by the supervisor (`waitpid()`)
- ✅ No zombie processes remain — `ps aux | grep defunct` returns no defunct container entries
- ✅ Logging threads observe termination signals and flush remaining buffer entries before joining
- ✅ Pipe and log file descriptors are closed on all paths
- ✅ Supervisor metadata reflects final states for all containers (`stopped`, `exited`, `hard_limit_killed`)
- ✅ Kernel tracking entries are freed on `rmmod monitor`

Useful verification commands:

```bash
ps aux | grep engine
ps -ef | grep defunct
dmesg | tail -n 50
```

---

## 12. Repository Contents

| File | Purpose |
|------|---------|
| `engine.c` | User-space runtime, supervisor, CLI, and logging pipeline |
| `monitor.c` | Kernel-space LKM — memory tracking and enforcement |
| `monitor_ioctl.h` | Shared `ioctl` command definitions between user and kernel space |
| `Makefile` | Build targets for both user-space and kernel module |
| `cpu_hog.c` | CPU-bound test workload |
| `io_pulse.c` | I/O-bound test workload |
| `memory_hog.c` | Memory-consuming test workload |

> ⚠️ Do **not** commit `rootfs-base/` or per-container `rootfs-*` directories to the repository.
