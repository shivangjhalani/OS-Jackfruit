# 🐳 Lightweight Multi-Container Runtime in C

## 📌 Project Overview

This project implements a **minimal container runtime** in C using Linux primitives such as:

* `clone()` with namespaces
* `chroot()` for filesystem isolation
* `/proc` mounting
* Process lifecycle management using `SIGCHLD`

The system mimics the **core working principles of Docker**, focusing on process isolation, container lifecycle, and runtime supervision.

---

## 🎯 Purpose of the Project

* Understand **Linux namespaces (PID, UTS, Mount)**
* Build a **container runtime from scratch**
* Implement a **supervisor process**
* Handle **process lifecycle and zombie cleanup**
* Support **multiple containers running concurrently**

---

## 👥 Team Members & Work Division

### 🔴 Guru — Core Runtime & Container Engine ✅ (Completed)

Responsible for designing and implementing the **entire container runtime layer**:

* Container creation using `clone()`
* Namespace isolation (PID, UTS, Mount)
* Filesystem isolation using `chroot()`
* `/proc` mounting inside containers
* Multi-container execution (alpha, beta)
* Supervisor process implementation
* Metadata management (linked list of containers)
* SIGCHLD handling (zombie process cleanup)
* Container lifecycle tracking (RUNNING → EXITED)
* Stop command implementation (`SIGTERM`)
* Interactive command loop (`ps`, `stop`)

👉 **Status: Fully completed and functional**

---

### 🔵 Harsh — Systems Integration & Advanced Features (In Progress)

Responsible for extending the runtime with advanced system features:

* Logging system (producer-consumer threads)
* IPC (CLI ↔ supervisor using UNIX sockets)
* Kernel module (`monitor.c`)
* Memory monitoring via `ioctl`
* Scheduling experiments
* Final integration and system cleanup

👉 **Status: Pending / In Progress**

---

## 📊 Project Progress

### ✅ Completed (Guru)

* ✔ Container runtime implementation
* ✔ Multi-container support
* ✔ Supervisor lifecycle management
* ✔ SIGCHLD handling (no zombie processes)
* ✔ Metadata tracking
* ✔ Stop command functionality
* ✔ Interactive command system

---

### 🔄 Remaining Work (Harsh)

* Logging system with bounded buffer
* IPC-based command interface
* Kernel-space memory monitor
* Scheduling and performance experiments
* Final system integration

---

## 🚀 Latest Update

* Implemented **multi-container runtime**
* Added **SIGCHLD-based lifecycle handling**
* Fixed **zombie process issues**
* Implemented **interactive supervisor commands**
* Corrected **state transition logic**
* Achieved **stable container execution**

---

## 🏗️ System Architecture

```text
Supervisor Process
    ├── Container (alpha)
    │     ├── PID namespace
    │     ├── UTS namespace
    │     └── chroot filesystem
    ├── Container (beta)
    └── Metadata (linked list)

SIGCHLD → detects exit → updates container state
```

---

## ⚙️ Build Instructions

```bash
gcc -O2 -Wall -Wextra -o engine engine.c -lpthread
```

---

## ▶️ Run Instructions

```bash
sudo ./engine supervisor ./rootfs-base
```

---

## 💻 Example Usage

```text
ps
stop alpha
ps
stop beta
ps
```

---

## 📸 Expected Output

```text
Container alpha started with PID: XXXX
Container beta started with PID: YYYY

--- Container List ---
ID: beta | PID: YYYY | STATE: running
ID: alpha | PID: XXXX | STATE: running
```

After stopping:

```text
STATE: exited
```

---

## ⚠️ Important Notes

* Run only on **Linux (Ubuntu VM recommended)**
* **WSL is not supported**
* Must run with `sudo`

---

## 📂 Repository Guidelines

### ✔ Include

* `engine.c`
* `Makefile`
* `README.md`

### ❌ Exclude

* `rootfs-base/`
* `rootfs-alpha/`
* `rootfs-beta/`
* `.tar.gz` files

---

## 🔮 Future Enhancements (Handled by Harsh)

* Full CLI using IPC
* Logging and monitoring system
* Kernel-level memory enforcement
* Performance benchmarking

---

## 🧠 Key Learnings

* Linux namespaces and process isolation
* Signal handling (`SIGCHLD`)
* Container lifecycle management
* System-level programming in C
* Internal working of container runtimes

---

## 🏁 Conclusion

This project successfully implements the **core engine of a container runtime**, demonstrating how operating systems enable process isolation and lifecycle management. The completed runtime serves as a strong foundation for advanced features like logging, IPC, and kernel integration.

---