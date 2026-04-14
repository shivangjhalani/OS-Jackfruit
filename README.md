# Multi-Container Runtime

A lightweight Linux container runtime in C with a long-running supervisor and a kernel-space memory monitor.

---

## 1. Team Information

| Name | SRN |
|------|-----|
| ANIKET SEN | PES1UG24CS062 |
| ADITYA MALLIKARJUN PARANDE | PES1UG24CS028 |

---

## 2. Build, Load, and Run Instructions

### Prerequisites

- Ubuntu 22.04 / 24.04 VM
- Secure Boot OFF

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

---

### Build

```bash
make
```

---

### Load Kernel Module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

---

### Start Supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

---

### Run Containers

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh
sudo ./engine ps
```

---

### Stop Containers

```bash
sudo ./engine stop alpha
```

---

### Unload Module

```bash
sudo rmmod monitor
```

---

## 3. Demo (Screenshots)

### Screenshot 1 — Multi-container supervision

<img src="https://github.com/user-attachments/assets/080a1866-8229-4886-8d19-fd4c73bf3940" width="600"/>

<img src="https://github.com/user-attachments/assets/f366e003-c6b6-4774-a21b-86ab4f85bb5d" width="600"/>

**Caption:** Two containers running simultaneously under a single supervisor process.

---

### Screenshot 2 — Metadata tracking

<img src="https://github.com/user-attachments/assets/06223658-d8c6-4c32-9fe5-3133cd81084a" width="700"/>

**Caption:** Output of the `ps` command showing container metadata such as PID, state, and limits.

---

### Screenshot 3 — Bounded-buffer logging

<img src="https://github.com/user-attachments/assets/ad5e462e-4b59-4880-b532-94dbf3d75aa9" width="700"/>

**Caption:** Container output captured through the logging pipeline and stored in log files.

---

### Screenshot 4 — CLI and IPC

<img src="https://github.com/user-attachments/assets/6cb7d78b-7e2b-4e87-baa1-8d96697e8824" width="700"/>

**Caption:** CLI communicates with the supervisor via UNIX domain socket and receives responses.

---

### Screenshot 5 — Soft-limit warning

<img src="https://github.com/user-attachments/assets/849be352-f526-40e2-ad3e-c42ef547ea5d" width="700"/>

**Caption:** Kernel module logs a warning when container memory usage exceeds the soft limit.

---

### Screenshot 6 — Hard-limit enforcement

<img src="https://github.com/user-attachments/assets/2e945263-bbef-4089-b36c-a1ad9cdc6276" width="700"/>

**Caption:** Container is terminated when memory usage exceeds the hard limit.

---

### Screenshot 7 — Scheduling experiment

<img src="https://github.com/user-attachments/assets/ae6a9ef1-0379-41df-94e8-e3831ad8cd25" width="700"/>

<img src="https://github.com/user-attachments/assets/1ef68305-c900-4b32-b45a-88971daa142b" width="700"/>

<img src="https://github.com/user-attachments/assets/aaa66131-6b32-4c84-b587-18762025a695" width="700"/>

**Caption:** Comparison of CPU scheduling behavior under different nice values.

---

### Screenshot 8 — Clean teardown

<img src="https://github.com/user-attachments/assets/562bfe6f-5c87-4f87-9977-02e53e52d227" width="700"/>

**Caption:** All containers are properly terminated with no zombie processes remaining.

---

## 4. Engineering Analysis

### 1. Isolation Mechanisms  
In our implementation, container isolation is achieved using Linux namespaces along with `chroot()`. We use `clone()` with flags such as `CLONE_NEWPID`, `CLONE_NEWUTS`, and `CLONE_NEWNS`.  

The PID namespace ensures that each container has its own process tree and sees itself as PID 1. The UTS namespace provides a separate hostname for each container. The mount namespace ensures filesystem changes inside the container do not affect the host.  

The `chroot()` system call changes the root directory of the container process, restricting its filesystem view. Additionally, `/proc` is mounted inside the container to allow process-related tools to function correctly.  

However, the host kernel is still shared among all containers, meaning complete isolation (like network or user isolation) is not implemented.

---

### 2. Supervisor and Process Lifecycle  
The supervisor is a long-running process responsible for managing all containers. Each container is created as a child process using `clone()`, and the supervisor maintains metadata such as PID, state, and exit status.  

To prevent zombie processes, the supervisor handles `SIGCHLD` signals and reaps child processes using `waitpid()`.  

The supervisor also tracks whether a container was stopped manually or killed due to memory limits using a `stop_requested` flag. This helps classify termination as normal exit, manual stop, or forced kill.

---

### 3. IPC, Threads, and Synchronization  
Two IPC mechanisms are used in this project:  

- **Logging (pipes):** Each container’s stdout and stderr are redirected to pipes. A producer thread reads from these pipes and inserts data into a bounded buffer. A consumer thread reads from the buffer and writes logs to files.  

- **Control (UNIX socket):** CLI commands communicate with the supervisor using a UNIX domain socket. The supervisor listens for requests and sends responses accordingly.  

The bounded buffer is implemented using a mutex and condition variables (`not_empty`, `not_full`). This ensures proper synchronization between producer and consumer threads and prevents race conditions, data loss, or deadlocks.

---

### 4. Memory Management and Enforcement  
Memory usage is tracked using RSS (Resident Set Size), which represents the physical memory used by a process.  

Two limits are enforced:  
- **Soft limit:** Generates a warning when exceeded  
- **Hard limit:** Terminates the process using `SIGKILL`  

The enforcement is implemented in kernel space to ensure reliability. A kernel module periodically checks memory usage and enforces limits, preventing processes from bypassing restrictions.

---

### 5. Scheduling Behavior  
The Linux Completely Fair Scheduler (CFS) allocates CPU time based on process priority. This priority is influenced by the `nice` value.  

Processes with lower nice values receive higher CPU priority, while those with higher nice values receive less CPU time.  

In our experiments, we observed that higher-priority containers received more CPU time, while lower-priority containers experienced delays and preemption.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation  
**Choice:** Used PID, UTS, and mount namespaces with `chroot()`  
**Tradeoff:** `chroot()` is simpler but less secure than `pivot_root()`  
**Justification:** It is sufficient for demonstrating isolation in this project  

---

### Supervisor Architecture  
**Choice:** Single-process supervisor with event handling  
**Tradeoff:** Cannot handle many concurrent CLI requests efficiently  
**Justification:** Simpler design reduces complexity and bugs  

---

### IPC and Logging  
**Choice:** Pipes for logging and UNIX socket for control  
**Tradeoff:** More complex than direct logging  
**Justification:** Bounded buffer ensures no data loss and proper synchronization  

---

### Kernel Monitor  
**Choice:** Kernel module for memory enforcement  
**Tradeoff:** Requires kernel programming complexity  
**Justification:** Provides reliable enforcement that user-space cannot bypass  

---

### Scheduling Experiments  
**Choice:** Used `nice` values for priority differences  
**Tradeoff:** Does not enforce strict CPU isolation  
**Justification:** Clearly demonstrates Linux scheduling behavior  

---

## 6. Scheduler Experiment Results

### Experiment: Two CPU-bound containers with different priorities

Both containers ran `/cpu_hog 20` (20-second CPU burn) simultaneously on the same machine.

---

### Results Table

| Container | Nice Value | Progress Reported At (elapsed seconds) | Gaps |
|----------|-----------|----------------------------------------|------|
| c1 | +10 (low priority) | 1, 2, 3, 4, 5, 6, 7, 12, 13 | 4-second gap at 8–11 |
| c2 | -5 (high priority) | 1, 6, 7 | Got larger slices, fewer prints |

---

### Raw Log Output

#### c1 log (nice 10):

```bash
cpu_hog alive elapsed=1 accumulator=4548337513673617523
cpu_hog alive elapsed=2 accumulator=3469723412426831115
cpu_hog alive elapsed=3 accumulator=13684638064056133321
cpu_hog alive elapsed=4 accumulator=9237104230200268010
cpu_hog alive elapsed=5 accumulator=11903977776708818429
cpu_hog alive elapsed=6 accumulator=14308564023570326925
cpu_hog alive elapsed=7 accumulator=2312219186486121679
cpu_hog alive elapsed=12 accumulator=13583581612097865815
cpu_hog alive elapsed=13 accumulator=11636555306494358935
cpu_hog done duration=20
```

#### c2 log (nice -5):

```bash
cpu_hog alive elapsed=1 accumulator=1950438522884953651
cpu_hog alive elapsed=6 accumulator=5628460969466473060
cpu_hog alive elapsed=7 accumulator=5848389523760964930
cpu_hog done duration=20
```

---

### Analysis

c1 was preempted between elapsed 7 and elapsed 12 — a 4-second window where c2 was consuming the CPU continuously.  

c2 received longer uninterrupted CPU slices, visible as larger gaps between its own progress prints.  

The Linux CFS scheduler allocated CPU time proportional to process weight. A process with nice -5 has significantly higher weight than one with nice +10.  

This matches CFS behavior: the high-priority process receives more CPU time, while the low-priority process is preempted more often.
This demonstrates fairness and priority-based scheduling behavior in Linux.
