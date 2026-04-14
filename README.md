# Multi-Container Runtime with Kernel Monitor

## Overview

This project implements a lightweight container runtime in C with support for multiple containers, background execution, lifecycle management, and a kernel-space monitoring module.

It demonstrates key operating system concepts such as:
- Process isolation using namespaces
- Filesystem isolation using chroot
- Container lifecycle management
- Interaction between user-space and kernel-space

---

## Features

- Run containers in foreground (`run`)
- Start multiple containers in background (`start`)
- Stop running containers (`stop`)
- List active containers (`ps`)
- Persistent state tracking
- Logging of container lifecycle events
- Kernel module integration

---

## Project Structure
boilerplate/
├── engine.c # Container runtime
├── monitor.c # Kernel module
├── monitor_ioctl.h # IOCTL definitions
├── Makefile # Build system
├── cpu_hog.c # Test workload
├── memory_hog.c # Test workload
├── io_pulse.c # Test workload


---

## Setup Instructions

### 1. Clone Repository

```bash
git clone https://github.com/<your-username>/OS-Jackfruit.git
cd OS-Jackfruit/boilerplate
