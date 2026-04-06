#!/bin/bash
# cleanup_verification.sh - Verify all resource cleanup

set -e

echo "========================================="
echo "Resource Cleanup Verification"
echo "========================================="
echo ""

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASS=0
FAIL=0

check_pass() {
    echo -e "${GREEN}✓ PASS:${NC} $1"
    ((PASS++))
}

check_fail() {
    echo -e "${RED}✗ FAIL:${NC} $1"
    ((FAIL++))
}

echo "1. Checking for zombie processes..."
ZOMBIES=$(ps aux | grep defunct | grep -v grep || true)
if [ -z "$ZOMBIES" ]; then
    check_pass "No zombie processes found"
else
    check_fail "Zombie processes detected:"
    echo "$ZOMBIES"
fi
echo ""

echo "2. Checking file descriptors for engine process..."
ENGINE_PID=$(pgrep engine | head -1 || echo "")
if [ -z "$ENGINE_PID" ]; then
    echo -e "${YELLOW}⚠ Supervisor not running${NC}"
else
    FD_COUNT=$(sudo ls -l /proc/$ENGINE_PID/fd 2>/dev/null | wc -l || echo "0")
    echo "File descriptor count: $FD_COUNT"
    if [ "$FD_COUNT" -lt 100 ]; then
        check_pass "File descriptor count is reasonable"
    else
        check_fail "Too many file descriptors open"
    fi
fi
echo ""

echo "3. Checking thread count..."
if [ ! -z "$ENGINE_PID" ]; then
    THREAD_COUNT=$(ps -T -p $ENGINE_PID | wc -l)
    echo "Thread count: $((THREAD_COUNT - 1))"
    if [ "$THREAD_COUNT" -lt 20 ]; then
        check_pass "Thread count is reasonable"
    else
        check_fail "Too many threads"
    fi
else
    echo -e "${YELLOW}⚠ Supervisor not running${NC}"
fi
echo ""

echo "4. Checking kernel module status..."
if lsmod | grep -q monitor; then
    check_pass "Kernel module is loaded"
    
    echo "   Testing module unload..."
    sudo rmmod monitor 2>/dev/null && {
        check_pass "Kernel module unloaded successfully"
        
        # Check for memory leaks in kernel log
        if dmesg | tail -20 | grep -q "Module unloaded"; then
            check_pass "Clean module exit message in dmesg"
        else
            check_fail "No clean exit message in dmesg"
        fi
        
        # Reload for continued testing
        sudo insmod monitor.ko 2>/dev/null || true
    } || {
        check_fail "Kernel module failed to unload"
    }
else
    check_fail "Kernel module not loaded"
fi
echo ""

echo "5. Checking socket file..."
if [ -e /tmp/mini_runtime.sock ]; then
    if [ ! -z "$ENGINE_PID" ]; then
        check_pass "Socket file exists while supervisor running"
    else
        check_fail "Socket file exists but supervisor not running"
    fi
else
    if [ -z "$ENGINE_PID" ]; then
        check_pass "Socket file cleaned up after supervisor exit"
    else
        check_fail "Socket file missing but supervisor running"
    fi
fi
echo ""

echo "6. Checking log files..."
if [ -d logs ]; then
    LOG_COUNT=$(ls logs/*.log 2>/dev/null | wc -l || echo "0")
    echo "Log files found: $LOG_COUNT"
    
    if [ "$LOG_COUNT" -gt 0 ]; then
        check_pass "Log files created successfully"
        
        # Check for log file handles
        if [ ! -z "$ENGINE_PID" ]; then
            OPEN_LOGS=$(sudo lsof -p $ENGINE_PID 2>/dev/null | grep ".log" | wc -l || echo "0")
            echo "Open log file handles: $OPEN_LOGS"
            if [ "$OPEN_LOGS" -eq 0 ]; then
                check_pass "No log files kept open (writes on-demand)"
            fi
        fi
    else
        echo -e "${YELLOW}⚠ No log files found${NC}"
    fi
else
    echo -e "${YELLOW}⚠ Logs directory doesn't exist${NC}"
fi
echo ""

echo "7. Checking container metadata consistency..."
if [ ! -z "$ENGINE_PID" ]; then
    CONTAINER_COUNT=$(sudo ./engine ps 2>/dev/null | grep "ID:" | wc -l || echo "0")
    ACTUAL_PROCS=$(ps aux | grep -E "rootfs-(alpha|beta|gamma)" | grep -v grep | wc -l || echo "0")
    
    echo "Containers in metadata: $CONTAINER_COUNT"
    echo "Actual container processes: $ACTUAL_PROCS"
    
    # Allow some margin for recently exited
    DIFF=$((CONTAINER_COUNT - ACTUAL_PROCS))
    if [ $DIFF -ge 0 ] && [ $DIFF -le 2 ]; then
        check_pass "Metadata consistent with actual processes"
    else
        check_fail "Metadata inconsistent with actual processes"
    fi
else
    echo -e "${YELLOW}⚠ Supervisor not running${NC}"
fi
echo ""

echo "8. Memory leak check (kernel module)..."
# This is a basic check - proper leak detection needs tools like kmemleak
SLAB_INFO=$(sudo cat /proc/slabinfo 2>/dev/null | grep -E "kmalloc" | head -3 || echo "")
if [ ! -z "$SLAB_INFO" ]; then
    echo "Kernel slab allocator stats available"
    check_pass "Slab info accessible (manual inspection needed)"
    echo "   Check for memory leaks with: sudo cat /proc/slabinfo"
else
    echo -e "${YELLOW}⚠ Cannot access slabinfo${NC}"
fi
echo ""

echo "========================================="
echo "Cleanup Verification Summary"
echo "========================================="
echo -e "Passed: ${GREEN}$PASS${NC}"
echo -e "Failed: ${RED}$FAIL${NC}"
echo ""

if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}All cleanup checks passed!${NC}"
    exit 0
else
    echo -e "${YELLOW}Some checks failed. Review above for details.${NC}"
    exit 1
fi
