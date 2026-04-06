# Multi-Container Runtime - Implementation Complete

## ✅ Implementation Status (Harsh's Part)

All code implementation for your tasks is **COMPLETE**. The following components have been fully implemented:

### Completed Components

#### 1. Bounded-Buffer Logging System ✅
- **File:** `boilerplate/engine.c`
- **Functions:**
  - `bounded_buffer_push()` - Producer inserts log entries with mutex/cond_var blocking
  - `bounded_buffer_pop()` - Consumer removes entries with graceful shutdown
  - `logging_thread()` - Consumer thread writing to per-container log files
  - `producer_thread()` - Reads from container pipes and pushes to buffer

#### 2. Kernel Memory Monitor ✅
- **File:** `boilerplate/monitor.c`
- **Components:**
  - `struct monitored_process` - Linked list node with PID, limits, warning flag
  - Global `monitored_list` and `monitored_lock` (mutex-based)
  - `timer_callback()` - Periodic RSS checking with soft/hard enforcement
  - REGISTER ioctl handler - Adds processes to monitoring
  - UNREGISTER ioctl handler - Removes processes
  - `monitor_exit()` - Cleanup all entries on module unload

#### 3. IPC Control Channel ✅
- **File:** `boilerplate/engine.c`
- **Components:**
  - UNIX domain socket at `/tmp/mini_runtime.sock`
  - `send_control_request()` - Client connects and sends commands
  - Supervisor accept loop - Receives and dispatches commands
  - Full request/response protocol

#### 4. Command Handlers ✅
- **File:** `boilerplate/engine.c`
- **Functions:**
  - `handle_cmd_start()` - Launch container in background
  - `handle_cmd_run()` - Launch and wait for container (blocking)
  - `handle_cmd_ps()` - List all containers with metadata
  - `handle_cmd_stop()` - Stop running container
  - `handle_cmd_logs()` - Display container log file

#### 5. Full Integration ✅
- **File:** `boilerplate/engine.c`
- **Components:**
  - `create_container()` - Creates container with pipes, registers with kernel
  - `handle_sigchld()` - Updates metadata and unregisters on exit
  - Container lifecycle tracking with state transitions
  - Proper cleanup and shutdown sequencing

---

## 🚀 How to Build and Test (Ubuntu VM Only)

### Prerequisites

You **MUST** run this on an Ubuntu 22.04 or 24.04 VM with Secure Boot OFF. macOS/WSL will NOT work.

```bash
# On Ubuntu VM:
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Step 1: Prepare Root Filesystems

```bash
cd /Users/harshpandya/Documents/Study/PES/os-lab/Kernel-Dock/boilerplate

# Download Alpine mini rootfs (if not already done)
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
sudo tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Create per-container copies
sudo cp -a ./rootfs-base ./rootfs-alpha
sudo cp -a ./rootfs-base ./rootfs-beta
sudo cp -a ./rootfs-base ./rootfs-gamma

# Copy test workloads into rootfs (after building)
# Will do this after make
```

### Step 2: Build Everything

```bash
cd /Users/harshpandya/Documents/Study/PES/os-lab/Kernel-Dock/boilerplate

# Clean previous builds
make clean

# Build all components
make

# This builds:
# - engine (user-space runtime)
# - monitor.ko (kernel module)
# - memory_hog, cpu_hog, io_pulse (test workloads)
```

### Step 3: Copy Workloads into Rootfs

```bash
# Copy test programs into container filesystems
sudo cp memory_hog ./rootfs-alpha/
sudo cp cpu_hog ./rootfs-beta/
sudo cp io_pulse ./rootfs-gamma/
```

### Step 4: Load Kernel Module

```bash
# Load the memory monitor kernel module
sudo insmod monitor.ko

# Verify it loaded
lsmod | grep monitor
ls -l /dev/container_monitor

# Check kernel log
dmesg | tail
# Should see: "[container_monitor] Module loaded. Device: /dev/container_monitor"
```

### Step 5: Start Supervisor

```bash
# In Terminal 1 - Start supervisor
sudo ./engine supervisor ./rootfs-base

# Should see:
# Supervisor started. Listening on /tmp/mini_runtime.sock
# Ready to accept container requests.
```

### Step 6: Test Commands (in another terminal)

```bash
# Open Terminal 2 for commands

# Start containers
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 32 --hard-mib 64
sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 48 --hard-mib 80

# List containers
sudo ./engine ps
# Should show:
# ID: beta | PID: XXXX | State: running | Soft: 48 MB | Hard: 80 MB
# ID: alpha | PID: YYYY | State: running | Soft: 32 MB | Hard: 64 MB

# Stop a container
sudo ./engine stop alpha

# Check again
sudo ./engine ps

# View logs
sudo ./engine logs alpha
sudo ./engine logs beta
```

### Step 7: Test Memory Limits

```bash
# Start a container that will exceed limits
sudo ./engine start memtest ./rootfs-alpha /memory_hog --soft-mib 30 --hard-mib 50

# Watch kernel log for soft limit warning
dmesg -w

# You should see:
# [container_monitor] SOFT LIMIT container=memtest pid=XXXX rss=YYYY limit=31457280
# [container_monitor] HARD LIMIT container=memtest pid=XXXX rss=ZZZZ limit=52428800

# Check container state
sudo ./engine ps
# memtest should show State: killed
```

---

## 🧪 Scheduling Experiments (Task 5)

### Experiment 1: CPU-bound with Different Priorities

```bash
# Terminal 1: Start supervisor
sudo ./engine supervisor ./rootfs-base

# Terminal 2: Start two CPU-bound containers with different nice values
sudo ./engine start cpu-high ./rootfs-alpha /cpu_hog --nice -10 --soft-mib 40 --hard-mib 64
sudo ./engine start cpu-low ./rootfs-beta /cpu_hog --nice 19 --soft-mib 40 --hard-mib 64

# Terminal 3: Monitor CPU usage
watch -n 1 'ps aux | grep cpu_hog'

# Observe: cpu-high should get more CPU time than cpu-low
# Record completion times, CPU percentages
```

### Experiment 2: CPU-bound vs I/O-bound

```bash
# Start one CPU-bound and one I/O-bound container
sudo ./engine start cpuwork ./rootfs-alpha /cpu_hog --soft-mib 40 --hard-mib 64
sudo ./engine start iowork ./rootfs-beta /io_pulse --soft-mib 40 --hard-mib 64

# Monitor behavior
top -p $(pgrep -d',' cpu_hog),$(pgrep -d',' io_pulse)

# Observe:
# - CPU-bound should have high CPU%, low wait%
# - I/O-bound should have lower CPU%, higher wait%
# - Scheduler gives I/O-bound better responsiveness
```

### Experiment 3: Memory Hog with Limits

```bash
# Test soft vs hard limits
sudo ./engine start mem1 ./rootfs-alpha /memory_hog --soft-mib 20 --hard-mib 40
sudo ./engine start mem2 ./rootfs-beta /memory_hog --soft-mib 30 --hard-mib 60

# Watch kernel log
dmesg -w

# Record:
# - When soft limit warnings appear
# - When hard limit kills happen
# - Container state transitions
```

---

## 📸 Required Screenshots

You need to capture 8 screenshots for the README:

### Screenshot 1: Multi-container Supervision
```bash
# Show supervisor running with multiple containers
sudo ./engine ps
# Capture: Terminal showing 2+ containers with "running" state
```

### Screenshot 2: Metadata Tracking
```bash
# Show ps output with all metadata
sudo ./engine ps
# Capture: Container IDs, PIDs, states, soft/hard limits
```

### Screenshot 3: Bounded-buffer Logging
```bash
# Show log files being created
ls -lh logs/
cat logs/alpha.log
# Capture: Log directory and log file contents
```

### Screenshot 4: CLI and IPC
```bash
# Show command being sent and response
sudo ./engine start test ./rootfs-alpha /bin/sh --soft-mib 40 --hard-mib 64
# Capture: Command execution and success response
```

### Screenshot 5: Soft-limit Warning
```bash
# Start memory hog and capture soft limit warning
sudo ./engine start memtest ./rootfs-alpha /memory_hog --soft-mib 20 --hard-mib 40
dmesg | tail -20
# Capture: Kernel log showing "[container_monitor] SOFT LIMIT" message
```

### Screenshot 6: Hard-limit Enforcement
```bash
# Continue from above, wait for hard limit kill
dmesg | tail -20
sudo ./engine ps
# Capture: 
#   1. Kernel log showing "[container_monitor] HARD LIMIT" message
#   2. ps output showing container in "killed" state
```

### Screenshot 7: Scheduling Experiment
```bash
# Show two containers with different nice values and their CPU usage
sudo ./engine start cpu1 ./rootfs-alpha /cpu_hog --nice -10
sudo ./engine start cpu2 ./rootfs-beta /cpu_hog --nice 19
top -p $(pgrep -d',' cpu_hog)
# Capture: top output showing different CPU% for different priorities
```

### Screenshot 8: Clean Teardown
```bash
# Stop all containers
sudo ./engine stop cpu1
sudo ./engine stop cpu2
sudo ./engine ps
ps aux | grep '[e]ngine'
# Capture: No zombie processes, containers in "stopped" state
```

---

## 🧹 Cleanup and Verification (Task 6)

### Verify No Zombies
```bash
# While supervisor is running with containers
ps aux | grep -i defunct
# Should show NO zombie processes
```

### Verify Thread Cleanup
```bash
# Stop supervisor (Ctrl+C in Terminal 1)
# Check that all threads exited cleanly (no errors in output)
```

### Verify Kernel Module Cleanup
```bash
# Unload module
sudo rmmod monitor

# Check kernel log
dmesg | tail
# Should see: "[container_monitor] Module unloaded."

# Verify device removed
ls /dev/container_monitor
# Should show: No such file or directory
```

### Verify File Descriptors
```bash
# While supervisor running, check FD count
sudo ls -l /proc/$(pgrep engine | head -1)/fd | wc -l
# Should be reasonable (< 50 for a few containers)

# After stopping all containers, count should decrease
```

---

## 📝 README.md Updates Needed

You need to update the project README.md with:

### 1. Team Information
Already done - you and Guru's names with SRNs

### 2. Build and Run Instructions
Copy the steps from "How to Build and Test" above

### 3. 8 Screenshots with Captions
Insert the 8 screenshots you captured with brief captions

### 4. Engineering Analysis (5 areas)

#### Analysis 1: Isolation Mechanisms
Explain:
- How PID namespace makes containers see PID 1 inside
- How UTS namespace allows different hostnames
- How mount namespace + chroot isolates filesystem
- What the kernel still shares (network, IPC, user namespace not isolated)

#### Analysis 2: Supervisor and Process Lifecycle
Explain:
- Why long-running supervisor is needed (manage multiple containers)
- Parent-child relationship via clone()
- SIGCHLD handling for zombie reaping
- Metadata tracking for container state

#### Analysis 3: IPC, Threads, and Synchronization
Explain:
- **Path A (logging):** Pipes from containers to supervisor
  - Race condition: Multiple producers pushing simultaneously
  - Solution: Mutex + condition variables for bounded buffer
- **Path B (control):** UNIX socket from CLI to supervisor
  - Race condition: Concurrent metadata access during command handling
  - Solution: Mutex protecting container list

#### Analysis 4: Memory Management and Enforcement
Explain:
- RSS measures actual physical memory used (not virtual)
- RSS doesn't include swapped pages or shared library duplication
- Soft limit = warning, allows container to continue
- Hard limit = enforcement, kills container
- Enforcement in kernel = can't be bypassed by userspace

#### Analysis 5: Scheduling Behavior
Explain using your experiment results:
- How CFS scheduler allocates CPU time based on nice values
- Why I/O-bound processes get better responsiveness
- How priority affects vruntime calculation
- Fairness vs throughput tradeoffs

### 5. Design Decisions and Tradeoffs

Example format:

**Bounded Buffer:**
- **Decision:** Used mutex + condition variables
- **Tradeoff:** Simpler than lock-free ring buffer but has mutex contention
- **Justification:** Correctness over performance for logging system

**Kernel Monitor Lock:**
- **Decision:** Used mutex instead of spinlock
- **Tradeoff:** Can't use in atomic context, but safer for long operations
- **Justification:** Timer runs in process context, RSS checking can be slow

**IPC Mechanism:**
- **Decision:** UNIX domain socket for control, pipes for logging
- **Tradeoff:** Two different mechanisms adds complexity
- **Justification:** Sockets support bidirectional request/response, pipes are one-way streaming

### 6. Scheduler Experiment Results

Include:
- Raw data (CPU percentages, completion times)
- Comparison table showing nice value vs CPU share
- Explanation of how Linux CFS behaved
- Connection to theoretical scheduling concepts

---

## ✅ Final Checklist

Before submission, verify:

- [ ] All code builds without errors on Ubuntu VM
- [ ] Kernel module loads and creates /dev/container_monitor
- [ ] Supervisor starts and accepts commands
- [ ] Containers start and run correctly
- [ ] Logging writes to per-container files
- [ ] Soft limit warnings appear in dmesg
- [ ] Hard limit kills containers correctly
- [ ] All 8 screenshots captured
- [ ] Engineering analysis written
- [ ] Design decisions documented
- [ ] Experiment results included
- [ ] No zombie processes remain after tests
- [ ] Module unloads cleanly

---

## 🎯 Summary

**You (Harsh) completed:**
- ✅ Bounded-buffer logging with producer-consumer threads
- ✅ Kernel memory monitor with soft/hard limits
- ✅ IPC control channel via UNIX sockets
- ✅ All CLI command handlers (start, run, ps, logs, stop)
- ✅ Full integration of user-space with kernel module
- ✅ Signal handling and cleanup paths

**Still TODO:**
- 🔄 Run experiments on Ubuntu VM (code is ready)
- 🔄 Capture 8 required screenshots
- 🔄 Write engineering analysis
- 🔄 Document design decisions
- 🔄 Complete README.md

**Time estimate:** 2-3 hours on Ubuntu VM to test, experiment, and document.

Good luck! The hard implementation is done. 🚀
