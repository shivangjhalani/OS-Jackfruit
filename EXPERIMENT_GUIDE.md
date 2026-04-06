# Scheduling Experiments - Quick Reference

## Experiment 1: CPU Priority Impact

### Setup
```bash
# Terminal 1: Supervisor
sudo ./engine supervisor ./rootfs-base

# Terminal 2: High priority CPU workload
sudo ./engine start cpu-high ./rootfs-alpha /cpu_hog --nice -10 --soft-mib 40 --hard-mib 64

# Terminal 3: Low priority CPU workload  
sudo ./engine start cpu-low ./rootfs-beta /cpu_hog --nice 19 --soft-mib 40 --hard-mib 64

# Terminal 4: Monitor
watch -n 1 'ps -eo pid,ni,pcpu,comm | grep cpu_hog'
```

### What to Measure
- CPU percentage for each container over 60 seconds
- Total CPU time consumed (from ps -o time)
- Completion time if workload terminates

### Expected Result
- High priority (-10) should get ~1.5-2x more CPU than low priority (19)
- Linux CFS scheduler gives better vruntime to higher priority

### Analysis Points
- CFS calculates vruntime based on nice value
- Lower nice = higher priority = slower vruntime growth = more CPU
- Fair scheduling within priority levels

---

## Experiment 2: CPU vs I/O Workload

### Setup
```bash
# CPU-bound container
sudo ./engine start cpuwork ./rootfs-alpha /cpu_hog --soft-mib 40 --hard-mib 64

# I/O-bound container
sudo ./engine start iowork ./rootfs-beta /io_pulse --soft-mib 40 --hard-mib 64

# Monitor both
top -p $(pgrep -d',' -f 'cpu_hog|io_pulse')
```

### What to Measure
- CPU% for each process
- Wait% or D state time
- Context switches (voluntary vs involuntary)
- Responsiveness (time to respond to signals)

### Expected Result
- CPU-bound: High CPU%, low wait%, fewer voluntary context switches
- I/O-bound: Lower CPU%, higher wait%, more voluntary context switches
- I/O-bound gets better responsiveness despite lower total CPU time

### Analysis Points
- I/O-bound processes sleep waiting for I/O
- Scheduler prioritizes interactive/I/O-bound for responsiveness
- CPU-bound processes use full time slices

---

## Experiment 3: Memory Limits

### Setup
```bash
# Low limits to trigger faster
sudo ./engine start mem1 ./rootfs-alpha /memory_hog --soft-mib 20 --hard-mib 35

# Watch kernel log
dmesg -w
```

### What to Measure
- Time until soft limit warning
- Time until hard limit kill
- Final container state (killed vs stopped)

### Expected Result
- Soft limit warning appears when RSS > 20 MB
- Hard limit kills when RSS > 35 MB
- Container state changes to "killed"
- dmesg shows both SOFT and HARD limit messages

### Analysis Points
- RSS measurement in kernel via get_mm_rss()
- Soft limit = policy (warning only)
- Hard limit = enforcement (SIGKILL)
- Kernel enforcement can't be bypassed

---

## Data Collection Template

### CPU Priority Experiment
```
Container | Nice | PID  | %CPU | Time  | Notes
----------|------|------|------|-------|--------
cpu-high  | -10  | XXXX | 67%  | 01:30 | Higher CPU share
cpu-low   | 19   | YYYY | 33%  | 03:00 | Lower CPU share
```

### CPU vs I/O Experiment
```
Container | Type | PID  | %CPU | %wa | Ctx Switches | Notes
----------|------|------|------|-----|--------------|--------
cpuwork   | CPU  | XXXX | 90%  | 1%  | 100          | Compute bound
iowork    | I/O  | YYYY | 10%  | 60% | 5000         | I/O bound
```

### Memory Limit Experiment
```
Container | Soft | Hard | Soft Hit | Hard Hit | State  | Notes
----------|------|------|----------|----------|--------|--------
mem1      | 20MB | 35MB | 5.2s     | 8.7s     | killed | Both limits triggered
```

---

## Quick Commands Reference

### Start Supervisor
```bash
sudo ./engine supervisor ./rootfs-base
```

### Container Operations
```bash
# Start
sudo ./engine start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]

# List
sudo ./engine ps

# Logs
sudo ./engine logs <id>

# Stop
sudo ./engine stop <id>
```

### Monitoring Commands
```bash
# Watch processes
watch -n 1 'ps aux | grep -E "cpu_hog|io_pulse|memory_hog"'

# CPU usage
top -p $(pgrep -d',' cpu_hog)

# Kernel logs
dmesg -w

# Container states
watch -n 1 'sudo ./engine ps'
```

### Kernel Module
```bash
# Load
sudo insmod monitor.ko

# Check
lsmod | grep monitor
ls -l /dev/container_monitor

# Unload
sudo rmmod monitor
```

---

## Screenshot Timing Guide

1. **Multi-container** - Capture after starting 2-3 containers
2. **Metadata** - Capture `ps` output showing all fields
3. **Logging** - Capture log files list + content
4. **IPC** - Capture command execution with response
5. **Soft limit** - Capture dmesg within 30s of start
6. **Hard limit** - Capture dmesg + ps showing "killed" state
7. **Scheduling** - Capture top output after 30s runtime
8. **Cleanup** - Capture ps showing no zombies after stop

---

## Troubleshooting

### Container won't start
- Check rootfs exists and has correct permissions
- Verify supervisor is running
- Check kernel module is loaded

### No soft/hard limit warnings
- Verify kernel module loaded: `lsmod | grep monitor`
- Check device exists: `ls /dev/container_monitor`
- Watch dmesg in real-time: `dmesg -w`
- Verify workload actually uses memory

### Logs not appearing
- Check logs/ directory exists
- Verify bounded buffer implementation
- Check logger thread started
- Look for pipe errors in supervisor output

### Can't connect to supervisor
- Verify supervisor is running
- Check socket exists: `ls -l /tmp/mini_runtime.sock`
- Try with sudo for all commands

---

## Success Criteria

✅ **You've succeeded when:**

1. Multiple containers run simultaneously
2. `ps` shows all metadata correctly
3. Log files contain container output
4. Soft limit warning appears in dmesg
5. Hard limit kills container and state = "killed"
6. Different nice values show different CPU shares
7. No zombie processes after stopping containers
8. Module unloads cleanly with no memory leaks

Good luck with the experiments! 🎯
