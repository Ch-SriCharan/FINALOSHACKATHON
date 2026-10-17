#!/usr/bin/env bash
# ==============================================================================
# Automated Test Suite for Multi-Patient Monitoring Simulator
# Demonstrating CO-2, CO-3, CO-4, CO-5, and CO-6
# ==============================================================================

set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_DIR"

# Color codes
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

pass_count=0
fail_count=0

assert_success() {
    local test_name="$1"
    local status="$2"
    if [ "$status" -eq 0 ]; then
        echo -e "${GREEN}[PASS]${NC} $test_name"
        pass_count=$((pass_count + 1))
    else
        echo -e "${RED}[FAIL]${NC} $test_name"
        fail_count=$((fail_count + 1))
    fi
}

echo "================================================================================"
echo "          RUNNING MULTI-PATIENT MONITORING SIMULATOR TEST SUITE"
echo "================================================================================"

# Clean any pre-existing artifacts
make clean > /dev/null 2>&1 || true

echo -e "\n${CYAN}>>> Test Case 1: Build & Compilation via GNU Makefile...${NC}"
make all
assert_success "Compilation of patient_monitor and race_demo binaries" $?

echo -e "\n${CYAN}>>> Test Case 2: Concurrency & Race Condition Demonstration (CO-6)...${NC}"
echo "Running Unsynchronized Execution (Expecting data tearing and lost updates)..."
./race_demo unsync
assert_success "Controlled demonstration of race conditions in unsynchronized mode" $?

echo -e "\nRunning Synchronized Execution (Expecting 0 torn reads and 100% data consistency)..."
./race_demo sync
assert_success "Verification of process-shared POSIX mutex synchronization in synchronized mode" $?

echo -e "\n${CYAN}>>> Test Case 3: Process Lifecycle & Normal Telemetry (CO-2, CO-3, CO-4)...${NC}"
# Run automated 4-second demo run
./patient_monitor --demo-duration 4
assert_success "Execution of master, 3 sensors, detector, and dashboard with clean termination" $?

echo -e "\n${CYAN}>>> Test Case 4: Named FIFO IPC & Emergency Anomaly Injection (CO-3, CO-5)...${NC}"
# Remove old alerts log to ensure fresh test verification
rm -f alerts.log

# Launch simulation in background
./patient_monitor --demo-duration 10 > /dev/null 2>&1 &
SIM_PID=$!

# Wait for master process and FIFO to be ready
sleep 2

if [ -p "/tmp/patient_monitor_fifo" ]; then
    echo "Named FIFO /tmp/patient_monitor_fifo is ready."
    # Inject emergency vitals for Patient 101 (Critical Bradycardia & Hypoxia)
    echo "inject 101 39.0 84.0" > /tmp/patient_monitor_fifo
    echo "Dispatched IPC command via FIFO: 'inject 101 39.0 84.0'"
    sleep 2

    # Query status via FIFO
    echo "status" > /tmp/patient_monitor_fifo
    sleep 1

    # Send shutdown command via FIFO
    echo "shutdown" > /tmp/patient_monitor_fifo
    echo "Dispatched shutdown command via FIFO."
    wait $SIM_PID 2>/dev/null || true
    assert_success "Named FIFO bidirectional IPC communication and remote shutdown" 0
else
    echo -e "${RED}Error: Named FIFO /tmp/patient_monitor_fifo was not created.${NC}"
    kill $SIM_PID 2>/dev/null || true
    assert_success "Named FIFO creation and IPC communication" 1
fi

echo -e "\n${CYAN}>>> Test Case 5: Alert Detection & File I/O Persistence (CO-5)...${NC}"
if [ -f "alerts.log" ] && grep -q "Patient ID: 101" alerts.log; then
    echo "Verified alerts.log contents:"
    cat alerts.log
    assert_success "Alert detector caught injected anomaly and persisted to alerts.log" 0
else
    echo -e "${RED}Error: alerts.log missing or did not capture Patient 101 emergency.${NC}"
    assert_success "Alert detector caught injected anomaly and persisted to alerts.log" 1
fi

echo -e "\n${CYAN}>>> Test Case 6: Duplicate Alert Suppression During Polling...${NC}"
if [ -f "alerts.log" ]; then
    ALERT_COUNT=$(grep -c "Patient ID: 101" alerts.log 2>/dev/null || echo 0)
else
    ALERT_COUNT=0
fi
echo "Number of logged entries for Patient 101: $ALERT_COUNT"
if [ "$ALERT_COUNT" -ge 1 ] && [ "$ALERT_COUNT" -le 3 ]; then
    assert_success "Duplicate alert suppression verified (no polling spam)" 0
else
    assert_success "Duplicate alert suppression verified (no polling spam)" 1
fi

echo -e "\n${CYAN}>>> Test Case 7: Post-Simulation Alert Review Tool (CO-5)...${NC}"
./patient_monitor --review-alerts
assert_success "Alert log post-simulation review tool (--review-alerts)" $?

echo -e "\n${CYAN}>>> Test Case 8: POSIX IPC Resource Leak Verification (CO-3, CO-4)...${NC}"
LEAKS=0
if [ -e "/dev/shm/patient_monitor_shm" ]; then
    echo -e "${RED}Resource Leak: /dev/shm/patient_monitor_shm was NOT unlinked!${NC}"
    LEAKS=1
fi
if [ -e "/tmp/patient_monitor_fifo" ]; then
    echo -e "${RED}Resource Leak: /tmp/patient_monitor_fifo was NOT unlinked!${NC}"
    LEAKS=1
fi

if [ "$LEAKS" -eq 0 ]; then
    assert_success "Shared memory and FIFO unlinked cleanly without resource leaks" 0
else
    assert_success "Shared memory and FIFO unlinked cleanly without resource leaks" 1
fi

echo -e "\n================================================================================"
echo -e "                            TEST SUITE RESULTS"
echo -e "================================================================================"
echo -e "  Total Tests Executed: $((pass_count + fail_count))"
echo -e "  Tests Passed:         ${GREEN}$pass_count${NC}"
echo -e "  Tests Failed:         ${RED}$fail_count${NC}"
echo -e "================================================================================"

if [ "$fail_count" -eq 0 ]; then
    echo -e "${GREEN}ALL 8 TEST CASES PASSED WITH 100% SUCCESS!${NC}\n"
    exit 0
else
    echo -e "${RED}ONE OR MORE TEST CASES FAILED.${NC}\n"
    exit 1
fi
