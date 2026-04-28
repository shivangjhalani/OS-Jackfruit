#!/usr/bin/env bash
# ==============================================================================
# run_all.sh - Run test.sh against all submissions and generate results.md
#
# Usage: sudo ./run_all.sh
# ==============================================================================

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEST_SCRIPT="$SCRIPT_DIR/test.sh"
SUBMISSIONS_DIR="$SCRIPT_DIR/submissions"
RESULTS_FILE="$SCRIPT_DIR/results.md"
LOGS_DIR="$SCRIPT_DIR/test_logs"

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: Must run as root (sudo ./run_all.sh)"
    exit 1
fi

mkdir -p "$LOGS_DIR"

# Pre-download Alpine rootfs once and reuse for all submissions
SHARED_ROOTFS="$SCRIPT_DIR/.rootfs-base"
if [ ! -d "$SHARED_ROOTFS" ] || [ ! -x "$SHARED_ROOTFS/bin/sh" ]; then
    echo "Downloading Alpine minirootfs (one-time)..."
    mkdir -p "$SHARED_ROOTFS"
    TARBALL="alpine-minirootfs-3.20.3-x86_64.tar.gz"
    wget -q "https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/$TARBALL" -O "/tmp/$TARBALL"
    tar -xzf "/tmp/$TARBALL" -C "$SHARED_ROOTFS"
    rm -f "/tmp/$TARBALL"
    echo "  Done."
fi

# Map from links.txt: dirname -> github URL
declare -A URL_MAP
while read -r url; do
    [ -z "$url" ] && continue
    clean_url=$(echo "$url" | sed 's/[?&]authuser=[0-9]*//g; s/[?&]tab=[^&]*//g; s|/$||; s/\.git$//')
    owner_repo=$(echo "$clean_url" | sed 's|https://github.com/||')
    owner=$(echo "$owner_repo" | cut -d/ -f1)
    repo=$(echo "$owner_repo" | cut -d/ -f2)
    dirname="${owner}---${repo}"
    URL_MAP["$dirname"]="$clean_url"
done < "$SCRIPT_DIR/links.txt"

# Find project directory within a submission
find_project_dir() {
    local sub_dir="$1"
    
    # Priority: dir containing engine.c + Makefile
    # 1. Check root
    if [ -f "$sub_dir/engine.c" ] && [ -f "$sub_dir/Makefile" ]; then
        echo "$sub_dir"
        return
    fi
    # 2. Check boilerplate/
    if [ -f "$sub_dir/boilerplate/engine.c" ] && [ -f "$sub_dir/boilerplate/Makefile" ]; then
        echo "$sub_dir/boilerplate"
        return
    fi
    # 3. Search for engine.c anywhere
    local engine_path
    engine_path=$(find "$sub_dir" -name "engine.c" -not -path "*/.git/*" 2>/dev/null | head -1)
    if [ -n "$engine_path" ]; then
        dirname "$engine_path"
        return
    fi
    # 4. Fallback to root
    echo "$sub_dir"
}

# Parse test output to extract pass/fail/skip counts
parse_results() {
    local log_file="$1"
    local pass_count fail_count skip_count total_count
    pass_count=$(grep -c '✓ PASS' "$log_file" 2>/dev/null || true)
    pass_count=${pass_count:-0}
    fail_count=$(grep -c '✗ FAIL' "$log_file" 2>/dev/null || true)
    fail_count=${fail_count:-0}
    skip_count=$(grep -c '○ SKIP' "$log_file" 2>/dev/null || true)
    skip_count=${skip_count:-0}
    total_count=$((pass_count + fail_count + skip_count))
    echo "$pass_count|$fail_count|$skip_count|$total_count"
}

# Start results.md
cat > "$RESULTS_FILE" <<'EOF'
# OS-JACKFRUIT Option 2 - Automated Evaluation Results

**Date:** DATEPLACEHOLDER
**Test Script:** `test.sh` (11 sections, ~45 checks)

## Summary Table

| # | GitHub Repository | Pass | Fail | Skip | Total | Notes |
|---|-------------------|------|------|------|-------|-------|
EOF
sed -i "s/DATEPLACEHOLDER/$(date '+%Y-%m-%d %H:%M:%S')/" "$RESULTS_FILE"

count=0
total_subs=$(ls -d "$SUBMISSIONS_DIR"/*/ 2>/dev/null | wc -l)

for sub_dir in "$SUBMISSIONS_DIR"/*/; do
    sub_name=$(basename "$sub_dir")
    github_url="${URL_MAP[$sub_name]:-unknown}"
    count=$((count + 1))
    
    echo ""
    echo "================================================================"
    echo "[$count/$total_subs] Testing: $sub_name"
    echo "  URL: $github_url"
    echo "================================================================"
    
    log_file="$LOGS_DIR/${sub_name}.log"
    
    # Skip if already tested
    if [ -f "$log_file" ] && [ -s "$log_file" ]; then
        result=$(parse_results "$log_file")
        pass_c=$(echo "$result" | cut -d'|' -f1)
        fail_c=$(echo "$result" | cut -d'|' -f2)
        skip_c=$(echo "$result" | cut -d'|' -f3)
        total_c=$(echo "$result" | cut -d'|' -f4)
        echo "  SKIPPED (already tested): Pass=$pass_c Fail=$fail_c Skip=$skip_c Total=$total_c"
        echo "| $count | [$sub_name]($github_url) | $pass_c | $fail_c | $skip_c | $total_c | |" >> "$RESULTS_FILE"
        continue
    fi
    
    # Find the actual project directory
    project_dir=$(find_project_dir "$sub_dir")
    echo "  Project dir: $project_dir"
    
    # Clean up any stale state from previous run
    rm -f /tmp/mini_runtime.sock 2>/dev/null
    pkill -f "engine supervisor" 2>/dev/null || true
    rmmod monitor 2>/dev/null || true
    # Clean build artifacts from previous submission
    (cd "$project_dir" 2>/dev/null && make clean >/dev/null 2>&1 || true)
    rm -f "$project_dir"/engine "$project_dir"/memory_hog "$project_dir"/cpu_hog "$project_dir"/io_pulse "$project_dir"/monitor.ko 2>/dev/null
    # Unmount any leftover /proc mounts inside rootfs dirs
    for mnt in "$project_dir"/rootfs-*/proc "$project_dir"/rootfs-base/proc; do
        umount "$mnt" 2>/dev/null || umount -l "$mnt" 2>/dev/null || true
    done
    rm -rf "$project_dir"/rootfs-test-* "$project_dir"/logs 2>/dev/null
    # Pre-place shared rootfs so test.sh doesn't download
    rm -rf "$project_dir"/rootfs-base 2>/dev/null
    cp -a "$SHARED_ROOTFS" "$project_dir/rootfs-base"
    sleep 1
    
    # Run the test script with a timeout (3 minutes max per submission)
    notes=""
    timeout --kill-after=10 120 bash "$TEST_SCRIPT" "$project_dir" "$sub_dir" > "$log_file" 2>&1
    test_exit=$?
    
    if [ "$test_exit" -eq 124 ]; then
        notes="TIMEOUT"
    fi
    
    # Additional cleanup after each run - kill everything aggressively
    pkill -9 -f "engine" 2>/dev/null || true
    pkill -9 -f "supervisor" 2>/dev/null || true
    rmmod monitor 2>/dev/null || true
    rm -f /tmp/mini_runtime.sock /tmp/container_engine.sock 2>/dev/null
    sleep 1
    
    # Parse results
    result=$(parse_results "$log_file")
    pass_c=$(echo "$result" | cut -d'|' -f1)
    fail_c=$(echo "$result" | cut -d'|' -f2)
    skip_c=$(echo "$result" | cut -d'|' -f3)
    total_c=$(echo "$result" | cut -d'|' -f4)
    
    [ -n "$notes" ] && notes_str="$notes" || notes_str=""
    
    echo "  Results: Pass=$pass_c Fail=$fail_c Skip=$skip_c Total=$total_c $notes_str"
    
    # Write to results.md
    echo "| $count | [$sub_name]($github_url) | $pass_c | $fail_c | $skip_c | $total_c | $notes_str |" >> "$RESULTS_FILE"
done

# Add summary footer
total_pass=0; total_fail=0; total_skip=0
for log_file in "$LOGS_DIR"/*.log; do
    [ -f "$log_file" ] || continue
    result=$(parse_results "$log_file")
    total_pass=$((total_pass + $(echo "$result" | cut -d'|' -f1)))
    total_fail=$((total_fail + $(echo "$result" | cut -d'|' -f2)))
    total_skip=$((total_skip + $(echo "$result" | cut -d'|' -f3)))
done

cat >> "$RESULTS_FILE" <<EOF

## Aggregate Statistics

- **Total Submissions:** $count
- **Aggregate Pass:** $total_pass
- **Aggregate Fail:** $total_fail
- **Aggregate Skip:** $total_skip

## Test Sections

1. **Build Checks** - Makefile, source files, compilation
2. **CLI Interface** - Usage output, exit codes, subcommands
3. **Kernel Module** - Load/unload, device node, dmesg
4. **Rootfs Setup** - Alpine minirootfs availability
5. **Supervisor Lifecycle** - Start, socket creation, ps with no containers
6. **Container Lifecycle** - Start/stop/ps/logs, multi-container, duplicate rejection
7. **engine run** - Foreground container execution
8. **Zombie Processes** - No zombie detection
9. **Supervisor Shutdown** - Clean SIGTERM exit, socket cleanup
10. **Kernel Module Integration** - Post-supervisor module health
11. **Source Quality** - README, namespace flags, chroot, /proc mount, sync primitives, kernel list/locking

## Per-Submission Detailed Logs

Detailed logs for each submission are available in the \`test_logs/\` directory.
EOF

echo ""
echo "========================================"
echo "All done! Results written to: $RESULTS_FILE"
echo "Detailed logs in: $LOGS_DIR/"
echo "== for each submission are available in the \`test_logs/\` directory.
EOF

echo ""
echo "========================================"
echo "All done! Results written to: $RESULTS_FILE"
echo "Detailed logs in: $LOGS_DIR/"
echo "========================================"
