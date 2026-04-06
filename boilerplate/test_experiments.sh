#!/bin/bash
# test_experiments.sh - Run all required experiments and collect data

set -e

echo "========================================="
echo "Multi-Container Runtime - Test Suite"
echo "========================================="
echo ""

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Check prerequisites
check_prerequisites() {
    echo -e "${YELLOW}Checking prerequisites...${NC}"
    
    if ! lsmod | grep -q monitor; then
        echo -e "${RED}ERROR: Kernel module not loaded!${NC}"
        echo "Run: sudo insmod monitor.ko"
        exit 1
    fi
    
    if [ ! -e /dev/container_monitor ]; then
        echo -e "${RED}ERROR: /dev/container_monitor not found!${NC}"
        exit 1
    fi
    
    if [ ! -f engine ]; then
        echo -e "${RED}ERROR: engine binary not found!${NC}"
        echo "Run: make"
        exit 1
    fi
    
    echo -e "${GREEN}✓ Prerequisites OK${NC}"
    echo ""
}

# Create results directory
mkdir -p test_results
RESULTS_DIR="test_results"

# Test 1: Multi-container supervision
test_multicontainer() {
    echo -e "${YELLOW}Test 1: Multi-container Supervision${NC}"
    echo "Starting multiple containers..."
    
    sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 32 --hard-mib 64 > ${RESULTS_DIR}/test1_start_alpha.txt 2>&1
    sleep 1
    sudo ./engine start beta ./rootfs-beta /bin/sh --soft-mib 48 --hard-mib 80 > ${RESULTS_DIR}/test1_start_beta.txt 2>&1
    sleep 1
    
    echo "Checking container status..."
    sudo ./engine ps > ${RESULTS_DIR}/test1_ps.txt 2>&1
    
    echo -e "${GREEN}✓ Test 1 complete. See ${RESULTS_DIR}/test1_*.txt${NC}"
    echo "SCREENSHOT 1: Capture output of: sudo ./engine ps"
    echo ""
}

# Test 2: Logging system
test_logging() {
    echo -e "${YELLOW}Test 2: Logging System${NC}"
    
    # Start a container that produces output
    sudo ./engine start logger ./rootfs-gamma /bin/sh -c 'for i in 1 2 3 4 5; do echo "Log entry $i"; sleep 1; done' --soft-mib 40 --hard-mib 64 > ${RESULTS_DIR}/test2_start.txt 2>&1
    
    sleep 6
    
    echo "Checking log files..."
    ls -lh logs/ > ${RESULTS_DIR}/test2_logs_list.txt 2>&1
    
    if [ -f logs/logger.log ]; then
        cat logs/logger.log > ${RESULTS_DIR}/test2_log_content.txt 2>&1
        echo -e "${GREEN}✓ Test 2 complete. See ${RESULTS_DIR}/test2_*.txt${NC}"
        echo "SCREENSHOT 3: Capture: ls -lh logs/ && cat logs/logger.log"
    else
        echo -e "${RED}⚠ Warning: Log file not created${NC}"
    fi
    
    echo ""
}

# Test 3: Soft limit warning
test_soft_limit() {
    echo -e "${YELLOW}Test 3: Soft Limit Warning${NC}"
    
    # Clear dmesg buffer
    sudo dmesg -C
    
    echo "Starting memory-intensive container with low soft limit..."
    sudo ./engine start memsoft ./rootfs-alpha /memory_hog --soft-mib 20 --hard-mib 80 > ${RESULTS_DIR}/test3_start.txt 2>&1
    
    echo "Waiting for soft limit warning (15 seconds)..."
    sleep 15
    
    # Capture kernel log
    dmesg | grep -i "soft limit" > ${RESULTS_DIR}/test3_dmesg_soft.txt 2>&1 || echo "No soft limit warning yet"
    dmesg | tail -30 > ${RESULTS_DIR}/test3_dmesg_full.txt 2>&1
    
    if grep -q "SOFT LIMIT" ${RESULTS_DIR}/test3_dmesg_full.txt; then
        echo -e "${GREEN}✓ Test 3 complete. Soft limit detected!${NC}"
        echo "SCREENSHOT 5: Capture: dmesg | tail -30"
    else
        echo -e "${YELLOW}⚠ Soft limit not detected yet. May need longer runtime.${NC}"
    fi
    
    echo ""
}

# Test 4: Hard limit enforcement
test_hard_limit() {
    echo -e "${YELLOW}Test 4: Hard Limit Enforcement${NC}"
    
    # Clear dmesg buffer
    sudo dmesg -C
    
    echo "Starting container with very low hard limit..."
    sudo ./engine start memhard ./rootfs-beta /memory_hog --soft-mib 15 --hard-mib 30 > ${RESULTS_DIR}/test4_start.txt 2>&1
    
    echo "Waiting for hard limit kill (20 seconds)..."
    sleep 20
    
    # Capture results
    dmesg | grep -i "hard limit" > ${RESULTS_DIR}/test4_dmesg_hard.txt 2>&1 || echo "No hard limit kill yet"
    dmesg | tail -30 > ${RESULTS_DIR}/test4_dmesg_full.txt 2>&1
    sudo ./engine ps > ${RESULTS_DIR}/test4_ps.txt 2>&1
    
    if grep -q "HARD LIMIT" ${RESULTS_DIR}/test4_dmesg_full.txt; then
        echo -e "${GREEN}✓ Test 4 complete. Hard limit kill detected!${NC}"
        echo "SCREENSHOT 6: Capture: dmesg | tail -30 && sudo ./engine ps"
    else
        echo -e "${YELLOW}⚠ Hard limit not reached yet. Container may need more time.${NC}"
    fi
    
    echo ""
}

# Test 5: Scheduling experiment - CPU priority
test_cpu_priority() {
    echo -e "${YELLOW}Test 5: Scheduling Experiment - CPU Priority${NC}"
    
    echo "Starting high priority CPU workload..."
    sudo ./engine start cpu-high ./rootfs-alpha /cpu_hog --nice -10 --soft-mib 40 --hard-mib 64 > ${RESULTS_DIR}/test5_start_high.txt 2>&1
    
    sleep 2
    
    echo "Starting low priority CPU workload..."
    sudo ./engine start cpu-low ./rootfs-beta /cpu_hog --nice 19 --soft-mib 40 --hard-mib 64 > ${RESULTS_DIR}/test5_start_low.txt 2>&1
    
    echo "Collecting CPU usage data (30 seconds)..."
    for i in {1..6}; do
        echo "Sample $i/6..."
        ps aux | grep -E "cpu_hog|PID" | head -3 >> ${RESULTS_DIR}/test5_cpu_samples.txt 2>&1
        sleep 5
    done
    
    # Get final statistics
    ps -eo pid,ni,pcpu,time,comm | grep cpu_hog > ${RESULTS_DIR}/test5_final_stats.txt 2>&1
    
    echo -e "${GREEN}✓ Test 5 complete. See ${RESULTS_DIR}/test5_*.txt${NC}"
    echo "SCREENSHOT 7: Capture: ps -eo pid,ni,pcpu,time,comm | grep cpu_hog"
    echo ""
    
    # Stop the CPU workloads
    sudo ./engine stop cpu-high > /dev/null 2>&1
    sudo ./engine stop cpu-low > /dev/null 2>&1
}

# Test 6: CPU vs I/O bound
test_cpu_vs_io() {
    echo -e "${YELLOW}Test 6: CPU-bound vs I/O-bound${NC}"
    
    echo "Starting CPU-bound workload..."
    sudo ./engine start cpuwork ./rootfs-alpha /cpu_hog --soft-mib 40 --hard-mib 64 > ${RESULTS_DIR}/test6_start_cpu.txt 2>&1
    
    sleep 2
    
    echo "Starting I/O-bound workload..."
    sudo ./engine start iowork ./rootfs-beta /io_pulse --soft-mib 40 --hard-mib 64 > ${RESULTS_DIR}/test6_start_io.txt 2>&1
    
    echo "Collecting behavior data (30 seconds)..."
    for i in {1..6}; do
        echo "Sample $i/6..."
        ps aux | grep -E "cpu_hog|io_pulse|PID" | head -4 >> ${RESULTS_DIR}/test6_samples.txt 2>&1
        sleep 5
    done
    
    # Get context switch statistics
    pidstat -w 1 5 > ${RESULTS_DIR}/test6_context_switches.txt 2>&1 || echo "pidstat not available"
    
    echo -e "${GREEN}✓ Test 6 complete. See ${RESULTS_DIR}/test6_*.txt${NC}"
    echo ""
    
    # Stop workloads
    sudo ./engine stop cpuwork > /dev/null 2>&1
    sudo ./engine stop iowork > /dev/null 2>&1
}

# Test 7: Cleanup verification
test_cleanup() {
    echo -e "${YELLOW}Test 7: Cleanup Verification${NC}"
    
    echo "Stopping all containers..."
    sudo ./engine ps | grep -oE "ID: [a-z-]+" | cut -d' ' -f2 | while read container_id; do
        if [ ! -z "$container_id" ]; then
            echo "Stopping $container_id..."
            sudo ./engine stop $container_id > /dev/null 2>&1 || true
        fi
    done
    
    sleep 2
    
    echo "Checking for zombie processes..."
    ps aux | grep defunct > ${RESULTS_DIR}/test7_zombies.txt 2>&1 || echo "No zombies found" > ${RESULTS_DIR}/test7_zombies.txt
    
    echo "Checking container states..."
    sudo ./engine ps > ${RESULTS_DIR}/test7_final_ps.txt 2>&1
    
    if ! grep -q defunct ${RESULTS_DIR}/test7_zombies.txt; then
        echo -e "${GREEN}✓ No zombie processes!${NC}"
    else
        echo -e "${RED}⚠ Zombie processes detected!${NC}"
    fi
    
    echo "SCREENSHOT 8: Capture: ps aux | grep defunct && sudo ./engine ps"
    echo ""
}

# Generate summary report
generate_report() {
    echo -e "${YELLOW}Generating Summary Report...${NC}"
    
    cat > ${RESULTS_DIR}/SUMMARY.txt << 'EOF'
========================================
Multi-Container Runtime - Test Summary
========================================

Test Results Location: test_results/

Required Screenshots:
1. Multi-container supervision: test1_ps.txt
2. Metadata tracking: test1_ps.txt
3. Logging system: test2_logs_list.txt + test2_log_content.txt
4. CLI/IPC: test1_start_alpha.txt
5. Soft limit: test3_dmesg_full.txt
6. Hard limit: test4_dmesg_full.txt + test4_ps.txt
7. Scheduling: test5_final_stats.txt
8. Cleanup: test7_zombies.txt + test7_final_ps.txt

Experiment Data:
- CPU Priority: test5_*.txt
- CPU vs I/O: test6_*.txt
- Memory Limits: test3_*.txt, test4_*.txt

Next Steps:
1. Review all test result files
2. Capture screenshots while running tests manually
3. Analyze data for README.md
4. Write engineering analysis
5. Complete documentation

EOF

    cat ${RESULTS_DIR}/SUMMARY.txt
    echo -e "${GREEN}✓ Summary report generated${NC}"
}

# Main execution
main() {
    check_prerequisites
    
    echo "Starting automated test suite..."
    echo "This will take approximately 5-10 minutes."
    echo ""
    
    test_multicontainer
    sleep 2
    
    test_logging
    sleep 2
    
    test_soft_limit
    
    test_hard_limit
    
    test_cpu_priority
    
    test_cpu_vs_io
    
    test_cleanup
    
    generate_report
    
    echo ""
    echo -e "${GREEN}=========================================${NC}"
    echo -e "${GREEN}All Tests Complete!${NC}"
    echo -e "${GREEN}=========================================${NC}"
    echo ""
    echo "Results saved in: ${RESULTS_DIR}/"
    echo ""
    echo "Next steps:"
    echo "1. Review test output files"
    echo "2. Capture screenshots mentioned above"
    echo "3. Fill in README_TEMPLATE.md with results"
    echo "4. Unload kernel module: sudo rmmod monitor"
}

# Run main function
main
