#!/usr/bin/env bash
# ==============================================================================
# test.sh - Automated smoke/integration tests for OS-JACKFRUIT Option 2
#
# Must be run as root on an Ubuntu 22.04/24.04 VM with kernel headers installed.
# Expects to be run from the project root (where Makefile, engine.c, etc. live).
#
# Usage:
#   sudo ./test.sh [project_dir]
#
# If project_dir is given, cd into it first (for grading varied submissions).
# Otherwise runs in CWD.
# ==============================================================================

set -u

# ── Colors ──
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
TOTAL_COUNT=0

# ── Helpers ──

pass() {
    PASS_COUNT=$((PASS_COUNT + 1))
    TOTAL_COUNT=$((TOTAL_COUNT + 1))
    echo -e "  ${GREEN}✓ PASS${NC}: $1"
}

fail() {
    FAIL_COUNT=$((FAIL_COUNT + 1))
    TOTAL_COUNT=$((TOTAL_COUNT + 1))
    echo -e "  ${RED}✗ FAIL${NC}: $1"
    [ -n "${2:-}" ] && echo -e "         ${RED}$2${NC}"
}

skip() {
    SKIP_COUNT=$((SKIP_COUNT + 1))
    TOTAL_COUNT=$((TOTAL_COUNT + 1))
    echo -e "  ${YELLOW}○ SKIP${NC}: $1"
}

section() {
    echo ""
    echo -e "${CYAN}${BOLD}═══ $1 ═══${NC}"
}

cleanup() {
    echo ""
    echo -e "${YELLOW}Cleaning up...${NC}"

    # Kill supervisor if still running
    if [ -n "${SUPERVISOR_PID:-}" ] && kill -0 "$SUPERVISOR_PID" 2>/dev/null; then
        kill -TERM "$SUPERVISOR_PID" 2>/dev/null
        wait "$SUPERVISOR_PID" 2>/dev/null
    fi

    # Kill any leftover engine supervisors we may have spawned
    pkill -f "engine supervisor" 2>/dev/null || true

    sleep 1

    # Remove rootfs copies
    rm -rf "${WORK_DIR:-/tmp/test_workdir}"/rootfs-test-* 2>/dev/null

    # Unload kernel module if loaded by us
    if [ "${MODULE_LOADED:-0}" = "1" ]; then
        rmmod monitor 2>/dev/null || true
    fi

    # Clean socket
    rm -f /tmp/mini_runtime.sock 2>/dev/null
}

trap cleanup EXIT

wait_for_file() {
    local file="$1"
    local timeout="${2:-5}"
    local elapsed=0
    while [ ! -e "$file" ] && [ "$elapsed" -lt "$timeout" ]; do
        sleep 0.2
        elapsed=$((elapsed + 1))
    done
    [ -e "$file" ]
}

wait_for_socket() {
    local sock="$1"
    local timeout="${2:-5}"
    local elapsed=0
    while [ ! -S "$sock" ] && [ "$elapsed" -lt "$timeout" ]; do
        sleep 0.2
        elapsed=$((elapsed + 1))
    done
    [ -S "$sock" ]
}

# ── Pre-flight ──

if [ "$(id -u)" -ne 0 ]; then
    echo -e "${RED}ERROR: Must run as root (sudo ./test.sh)${NC}"
    exit 1
fi

PROJECT_DIR="${1:-.}"
REPO_ROOT="${2:-}"  # optional: repo root for finding README etc.
# Resolve REPO_ROOT to absolute before cd
if [ -n "$REPO_ROOT" ] && [ -d "$REPO_ROOT" ]; then
    REPO_ROOT="$(cd "$REPO_ROOT" && pwd)"
fi
cd "$PROJECT_DIR" || { echo "Cannot cd to $PROJECT_DIR"; exit 1; }
PROJECT_DIR="$(pwd)"
[ -z "$REPO_ROOT" ] && REPO_ROOT="$PROJECT_DIR"

WORK_DIR="$PROJECT_DIR"

echo -e "${BOLD}OS-JACKFRUIT Option 2 - Automated Test Suite${NC}"
echo -e "Project directory: ${PROJECT_DIR}"
echo -e "Date: $(date)"
echo ""

# ==============================================================================
section "1. Build Checks"
# ==============================================================================

# 1a. Makefile exists
if [ -f Makefile ]; then
    pass "Makefile exists"
else
    fail "Makefile not found"
fi

# 1b. Required source files exist
SRCS_FOUND=0
for src in engine.c monitor.c monitor_ioctl.h; do
    [ -f "$src" ] && SRCS_FOUND=$((SRCS_FOUND + 1))
done
if [ "$SRCS_FOUND" -eq 3 ]; then
    pass "All required source files present (engine.c, monitor.c, monitor_ioctl.h)"
else
    fail "Missing source files ($SRCS_FOUND/3 found)"
fi

# 1d. User-space build - try multiple strategies
echo -e "\n  Building user-space targets..."
BUILD_OUTPUT=$(timeout 30 make engine memory_hog cpu_hog io_pulse 2>&1) || true
# If specific targets failed, try plain make
if [ ! -x "./engine" ]; then
    BUILD_OUTPUT=$(timeout 30 make 2>&1) || true
fi
# Also try make all
if [ ! -x "./engine" ]; then
    BUILD_OUTPUT=$(timeout 30 make all 2>&1) || true
fi

if [ -x "./engine" ]; then
    pass "engine binary built successfully"
else
    fail "engine binary not built or not executable" "$BUILD_OUTPUT"
fi

WORKLOAD_BINS=0
for bin in memory_hog cpu_hog io_pulse; do
    [ -x "./$bin" ] && WORKLOAD_BINS=$((WORKLOAD_BINS + 1))
done
if [ "$WORKLOAD_BINS" -ge 1 ]; then
    pass "Workload binaries built ($WORKLOAD_BINS)"
else
    fail "No workload binaries built"
fi

# 1e. Kernel module build - try multiple targets
if [ -d "/lib/modules/$(uname -r)/build" ]; then
    echo -e "\n  Building kernel module..."
    MODULE_BUILD_OUTPUT=$(timeout 60 make monitor.ko 2>&1) || true
    if [ ! -f "monitor.ko" ]; then
        MODULE_BUILD_OUTPUT=$(timeout 60 make module 2>&1) || true
    fi
    if [ ! -f "monitor.ko" ]; then
        MODULE_BUILD_OUTPUT=$(timeout 60 make modules 2>&1) || true
    fi
    if [ -f "monitor.ko" ]; then
        pass "Kernel module monitor.ko built"
    else
        fail "Kernel module build failed" "$MODULE_BUILD_OUTPUT"
    fi
else
    skip "Kernel headers not installed, cannot build module"
fi

# ==============================================================================
section "2. CLI Interface Checks"
# ==============================================================================

if [ ! -x "./engine" ]; then
    skip "engine binary missing, skipping CLI checks"
else
    # 2a. No-args prints usage or help
    USAGE_OUTPUT=$(timeout 5 ./engine 2>&1) || true
    if echo "$USAGE_OUTPUT" | grep -qiE "usage|help|command|supervisor"; then
        pass "engine with no args prints usage/help"
    else
        fail "engine with no args does not print usage" "Got: $USAGE_OUTPUT"
    fi

    # 2b. Non-zero exit on no args
    timeout 5 ./engine >/dev/null 2>&1
    EXIT_CODE=$?
    if [ "$EXIT_CODE" -ne 0 ]; then
        pass "engine exits non-zero with no args (exit=$EXIT_CODE)"
    else
        fail "engine exits 0 with no args (should be non-zero)"
    fi

    # 2c. Subcommand coverage (lenient: pass if at least 3 recognized)
    CMD_COUNT=0
    for cmd in supervisor start run ps logs stop; do
        echo "$USAGE_OUTPUT" | grep -qi "$cmd" && CMD_COUNT=$((CMD_COUNT + 1))
    done
    if [ "$CMD_COUNT" -ge 3 ]; then
        pass "Usage text mentions $CMD_COUNT/6 subcommands"
    else
        fail "Usage text mentions only $CMD_COUNT/6 subcommands"
    fi
fi

# ==============================================================================
section "3. Kernel Module Checks"
# ==============================================================================

MODULE_LOADED=0

if [ ! -f "monitor.ko" ]; then
    skip "monitor.ko not built, skipping kernel module checks"
else
    # 3a. Module loads
    # First unload if already loaded
    rmmod monitor 2>/dev/null || true
    sleep 0.5

    INSMOD_OUT=$(insmod monitor.ko 2>&1)
    if lsmod | grep -q "^monitor "; then
        pass "Kernel module loaded successfully"
        MODULE_LOADED=1
    else
        fail "Kernel module failed to load" "$INSMOD_OUT"
    fi

    # 3b. Device node created
    if [ "$MODULE_LOADED" = "1" ]; then
        sleep 0.5
        if [ -c "/dev/container_monitor" ]; then
            pass "/dev/container_monitor char device exists"
        else
            fail "/dev/container_monitor not created"
        fi

        # 3c. Module unloads cleanly
        RMMOD_OUT=$(rmmod monitor 2>&1)
        if ! lsmod | grep -q "^monitor "; then
            pass "Kernel module unloaded successfully"
            MODULE_LOADED=0
        else
            fail "Kernel module failed to unload" "$RMMOD_OUT"
        fi

        # Reload for remaining tests
        insmod monitor.ko 2>/dev/null
        if lsmod | grep -q "^monitor "; then
            MODULE_LOADED=1
        fi
    fi
fi

# ==============================================================================
section "4. Rootfs Setup"
# ==============================================================================

ROOTFS_BASE=""
ROOTFS_AVAILABLE=0

# Look for an existing rootfs-base or alpine rootfs
for candidate in rootfs-base rootfs; do
    if [ -d "$candidate" ] && [ -x "$candidate/bin/sh" ]; then
        ROOTFS_BASE="$candidate"
        break
    fi
done

if [ -z "$ROOTFS_BASE" ]; then
    # Try to create one
    echo -e "  No rootfs found, attempting to download Alpine minirootfs..."
    ROOTFS_BASE="rootfs-base"
    mkdir -p "$ROOTFS_BASE"
    TARBALL="alpine-minirootfs-3.20.3-x86_64.tar.gz"
    if wget -q "https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/$TARBALL" -O "/tmp/$TARBALL" 2>/dev/null; then
        tar -xzf "/tmp/$TARBALL" -C "$ROOTFS_BASE" 2>/dev/null
        rm -f "/tmp/$TARBALL"
    fi
fi

if [ -d "$ROOTFS_BASE" ] && [ -x "$ROOTFS_BASE/bin/sh" ]; then
    pass "Root filesystem available at $ROOTFS_BASE"
    ROOTFS_AVAILABLE=1
else
    fail "No usable rootfs found (need Alpine minirootfs with /bin/sh)"
fi

# ==============================================================================
section "5. Supervisor Lifecycle Checks"
# ==============================================================================

SUPERVISOR_PID=""

if [ ! -x "./engine" ] || [ "$ROOTFS_AVAILABLE" = "0" ]; then
    skip "engine or rootfs not available, skipping supervisor tests"
else
    # Clean up any stale state
    rm -f /tmp/mini_runtime.sock 2>/dev/null
    rm -rf logs 2>/dev/null

    # Create rootfs copies for test containers
    cp -a "$ROOTFS_BASE" "${WORK_DIR}/rootfs-test-alpha"
    cp -a "$ROOTFS_BASE" "${WORK_DIR}/rootfs-test-beta"

    # Copy workloads into rootfs if available
    for bin in memory_hog cpu_hog io_pulse; do
        [ -x "./$bin" ] && cp "./$bin" "${WORK_DIR}/rootfs-test-alpha/" && cp "./$bin" "${WORK_DIR}/rootfs-test-beta/"
    done

    # 5a. Start supervisor in background
    ./engine supervisor "$ROOTFS_BASE" &
    SUPERVISOR_PID=$!
    sleep 1

    if kill -0 "$SUPERVISOR_PID" 2>/dev/null; then
        pass "Supervisor process started and alive (PID=$SUPERVISOR_PID)"
    else
        fail "Supervisor process died immediately"
        SUPERVISOR_PID=""
    fi

    # 5b. Control socket exists
    if [ -n "$SUPERVISOR_PID" ]; then
        if wait_for_socket /tmp/mini_runtime.sock 10; then
            pass "Control socket /tmp/mini_runtime.sock created"
        else
            fail "Control socket not created within timeout"
        fi
    fi

    # 5c. ps with no containers
    if [ -n "$SUPERVISOR_PID" ]; then
        PS_OUT=$(timeout 5 ./engine ps 2>&1)
        PS_RC=$?
        if [ "$PS_RC" -eq 0 ]; then
            pass "engine ps succeeds with no containers"
        else
            fail "engine ps failed with no containers (exit=$PS_RC)"
        fi
    fi
fi

# ==============================================================================
section "6. Container Lifecycle Checks"
# ==============================================================================

if [ -z "${SUPERVISOR_PID:-}" ]; then
    skip "No supervisor running, skipping container lifecycle tests"
else
    # 6a. Start a container
    START_OUT=$(timeout 10 ./engine start alpha "${WORK_DIR}/rootfs-test-alpha" "echo hello-from-alpha" 2>&1)
    START_RC=$?
    if [ "$START_RC" -eq 0 ]; then
        pass "engine start alpha succeeded"
    else
        fail "engine start alpha failed (exit=$START_RC)" "$START_OUT"
    fi

    sleep 2

    # 6b. ps shows the container
    PS_OUT=$(timeout 5 ./engine ps 2>&1)
    if echo "$PS_OUT" | grep -q "alpha"; then
        pass "engine ps shows container 'alpha'"
    else
        fail "engine ps does not show 'alpha'" "Got: $PS_OUT"
    fi

    # 6c. Log file created
    sleep 1
    LOG_FOUND=0
    for logfile in logs/alpha.log alpha.log; do
        if [ -f "$logfile" ]; then
            LOG_FOUND=1
            break
        fi
    done
    # Also check if logs command works
    LOGS_OUT=$(timeout 5 ./engine logs alpha 2>&1)
    LOGS_RC=$?
    if [ "$LOG_FOUND" = "1" ] || [ "$LOGS_RC" -eq 0 ]; then
        pass "Log capture working for container alpha"
    else
        fail "No log file or log output found for alpha"
    fi

    # 6d. Log contains container output
    if echo "$LOGS_OUT" | grep -q "hello-from-alpha"; then
        pass "Log contains expected container output 'hello-from-alpha'"
    else
        # Container may have already exited, check log file directly
        LOG_CONTENT=""
        for logfile in logs/alpha.log alpha.log; do
            [ -f "$logfile" ] && LOG_CONTENT=$(cat "$logfile" 2>/dev/null)
        done
        if echo "$LOG_CONTENT" | grep -q "hello-from-alpha"; then
            pass "Log file contains expected container output 'hello-from-alpha'"
        else
            fail "Expected 'hello-from-alpha' in logs" "Logs output: $LOGS_OUT"
        fi
    fi

    # 6e. Start a second container concurrently
    START_OUT2=$(timeout 10 ./engine start beta "${WORK_DIR}/rootfs-test-beta" "echo hello-from-beta" 2>&1)
    START_RC2=$?
    if [ "$START_RC2" -eq 0 ]; then
        pass "engine start beta succeeded (multi-container)"
    else
        fail "engine start beta failed (exit=$START_RC2)" "$START_OUT2"
    fi

    sleep 2

    # 6f. ps shows both containers
    PS_OUT2=$(timeout 5 ./engine ps 2>&1)
    if echo "$PS_OUT2" | grep -q "alpha" && echo "$PS_OUT2" | grep -q "beta"; then
        pass "engine ps shows both containers alpha and beta"
    else
        fail "engine ps missing one or both containers" "Got: $PS_OUT2"
    fi

    # 6g. Duplicate container ID rejected
    DUP_OUT=$(timeout 5 ./engine start alpha "${WORK_DIR}/rootfs-test-alpha" "echo dup" 2>&1)
    DUP_RC=$?
    if [ "$DUP_RC" -ne 0 ]; then
        pass "Duplicate container ID 'alpha' correctly rejected"
    else
        fail "Duplicate container ID 'alpha' was accepted (should be rejected)"
    fi

    # 6h. Stop a container
    STOP_OUT=$(timeout 5 ./engine stop beta 2>&1)
    STOP_RC=$?
    if [ "$STOP_RC" -eq 0 ]; then
        pass "engine stop beta succeeded"
    else
        # Container may have already exited naturally
        skip "engine stop beta returned $STOP_RC (container may have already exited)"
    fi

    sleep 1

    # 6i. Logs for beta
    LOGS_BETA=$(timeout 5 ./engine logs beta 2>&1)
    LOGS_BETA_RC=$?
    if [ "$LOGS_BETA_RC" -eq 0 ]; then
        pass "engine logs beta succeeds"
    else
        fail "engine logs beta failed (exit=$LOGS_BETA_RC)"
    fi
fi

# ==============================================================================
section "7. engine run (Foreground Container)"
# ==============================================================================

if [ -z "${SUPERVISOR_PID:-}" ]; then
    skip "No supervisor running, skipping run test"
else
    # run should block and return
    RUN_OUT=$(timeout 10 ./engine run gamma "${WORK_DIR}/rootfs-test-alpha" "echo run-output" 2>&1)
    RUN_RC=$?
    if [ "$RUN_RC" -eq 0 ] || [ "$RUN_RC" -eq 124 ]; then
        if [ "$RUN_RC" -eq 124 ]; then
            fail "engine run timed out (blocked too long)"
        else
            pass "engine run gamma completed successfully"
        fi
    else
        fail "engine run gamma failed (exit=$RUN_RC)" "$RUN_OUT"
    fi
fi

# ==============================================================================
section "8. No Zombie Processes"
# ==============================================================================

if [ -z "${SUPERVISOR_PID:-}" ]; then
    skip "No supervisor running, skipping zombie check"
else
    sleep 2

    ZOMBIES=$(ps aux | grep -E "\b${SUPERVISOR_PID}\b" | grep -c "Z" 2>/dev/null || true)
    ZOMBIES=${ZOMBIES:-0}
    ENGINE_ZOMBIES=$(ps aux | grep "[e]ngine" | grep -c "Z" 2>/dev/null || true)
    ENGINE_ZOMBIES=${ENGINE_ZOMBIES:-0}

    if [ "$ZOMBIES" -eq 0 ] && [ "$ENGINE_ZOMBIES" -eq 0 ]; then
        pass "No zombie processes found"
    else
        fail "Zombie processes detected (supervisor-related=$ZOMBIES, engine=$ENGINE_ZOMBIES)"
    fi
fi

# ==============================================================================
section "9. Supervisor Shutdown"
# ==============================================================================

if [ -z "${SUPERVISOR_PID:-}" ]; then
    skip "No supervisor running, skipping shutdown test"
else
    # Send SIGTERM
    kill -TERM "$SUPERVISOR_PID" 2>/dev/null
    SHUTDOWN_WAIT=0
    while kill -0 "$SUPERVISOR_PID" 2>/dev/null && [ "$SHUTDOWN_WAIT" -lt 10 ]; do
        sleep 1
        SHUTDOWN_WAIT=$((SHUTDOWN_WAIT + 1))
    done

    if ! kill -0 "$SUPERVISOR_PID" 2>/dev/null; then
        pass "Supervisor exited cleanly on SIGTERM (within ${SHUTDOWN_WAIT}s)"
    else
        fail "Supervisor did not exit within 10s of SIGTERM"
        kill -9 "$SUPERVISOR_PID" 2>/dev/null
    fi

    wait "$SUPERVISOR_PID" 2>/dev/null
    SUPERVISOR_PID=""

    sleep 1

    # Socket should be cleaned up
    if [ ! -S /tmp/mini_runtime.sock ]; then
        pass "Control socket cleaned up after shutdown"
    else
        fail "Control socket still exists after supervisor shutdown"
    fi

    # No leftover engine processes
    LEFTOVER=$(pgrep -c -f "engine supervisor" 2>/dev/null || true)
    LEFTOVER=${LEFTOVER:-0}
    if [ "$LEFTOVER" -eq 0 ]; then
        pass "No leftover supervisor processes"
    else
        fail "Found $LEFTOVER leftover supervisor processes"
    fi
fi

# ==============================================================================
section "10. Kernel Module Integration (Post-Supervisor)"
# ==============================================================================

if [ "$MODULE_LOADED" = "1" ]; then
    # After supervisor exits, module should still be loaded and healthy
    if lsmod | grep -q "^monitor "; then
        pass "Kernel module still loaded after supervisor exit"
    else
        fail "Kernel module disappeared after supervisor exit"
    fi

    # Unload and check for clean exit
    dmesg -C 2>/dev/null || true
    rmmod monitor 2>/dev/null
    sleep 0.5

    if ! lsmod | grep -q "^monitor "; then
        pass "Kernel module unloads cleanly after full test"
        MODULE_LOADED=0
    else
        fail "Kernel module failed to unload after test"
    fi

    if [ ! -c "/dev/container_monitor" ]; then
        pass "/dev/container_monitor removed after module unload"
    else
        fail "/dev/container_monitor still exists after unload"
    fi
else
    skip "Module not loaded, skipping post-test module checks"
fi

# ==============================================================================
section "11. Source Quality Checks"
# ==============================================================================

# 11a. engine.c uses required namespace flags
if grep -q "CLONE_NEWPID" engine.c 2>/dev/null; then
    pass "engine.c uses CLONE_NEWPID"
else
    fail "engine.c missing CLONE_NEWPID (PID namespace isolation required)"
fi

if grep -q "CLONE_NEWUTS" engine.c 2>/dev/null; then
    pass "engine.c uses CLONE_NEWUTS"
else
    fail "engine.c missing CLONE_NEWUTS (UTS namespace isolation required)"
fi

if grep -q "CLONE_NEWNS" engine.c 2>/dev/null; then
    pass "engine.c uses CLONE_NEWNS"
else
    fail "engine.c missing CLONE_NEWNS (mount namespace isolation required)"
fi

# 11d. engine.c uses chroot or pivot_root
if grep -qE "chroot|pivot_root" engine.c 2>/dev/null; then
    pass "engine.c uses chroot or pivot_root for filesystem isolation"
else
    fail "engine.c missing chroot/pivot_root"
fi

# 11e. engine.c mounts /proc
if grep -q 'mount.*proc' engine.c 2>/dev/null; then
    pass "engine.c mounts /proc inside container"
else
    fail "engine.c does not mount /proc"
fi

# 11f. Bounded buffer synchronization primitives
if grep -qE "pthread_mutex|pthread_cond|sem_wait|sem_post" engine.c 2>/dev/null; then
    pass "engine.c uses synchronization primitives for bounded buffer"
else
    fail "engine.c missing synchronization primitives"
fi

# 11g. monitor.c uses kernel linked list
if grep -q "list_head" monitor.c 2>/dev/null; then
    pass "monitor.c uses kernel linked list (struct list_head)"
else
    fail "monitor.c missing kernel linked list usage"
fi

# 11h. monitor.c uses locking
if grep -qE "mutex_lock|spin_lock" monitor.c 2>/dev/null; then
    pass "monitor.c uses kernel locking primitives"
else
    fail "monitor.c missing kernel locking"
fi

# 11g. io
else
    fail "monitor.c missing kernel linked list usage"
fi

# 11h. monitor.c uses locking
if grep -qE "mutex_lock|spin_lock" monitor.c 2>/dev/null; then
    pass "monitor.c uses kernel locking primitives"
else
    fail "monitor.c missing kernel locking"
fi

# 11g. ioctl shared header consistency (lenient: accept any ioctl definitions)
if grep -qiE "REGISTER|IOCTL|_IO[RW]?\(" monitor_ioctl.h 2>/dev/null; then
    pass "monitor_ioctl.h defines ioctl commands"
else
    fail "monitor_ioctl.h missing ioctl definitions"
fi

# ==============================================================================
# Summary
# ==============================================================================

echo ""
echo -e "${BOLD}══════════════════════════════════════${NC}"
echo -e "${BOLD}           TEST SUMMARY${NC}"
echo -e "${BOLD}══════════════════════════════════════${NC}"
echo -e "  ${GREEN}Passed${NC}: $PASS_COUNT"
echo -e "  ${RED}Failed${NC}: $FAIL_COUNT"
echo -e "  ${YELLOW}Skipped${NC}: $SKIP_COUNT"
echo -e "  Total:   $TOTAL_COUNT"
echo ""

if [ "$FAIL_COUNT" -eq 0 ]; then
    echo -e "  ${GREEN}${BOLD}All checks passed.${NC}"
else
    echo -e "  ${RED}${BOLD}$FAIL_COUNT check(s) failed.${NC}"
fi

echo ""
exit "$FAIL_COUNT"
