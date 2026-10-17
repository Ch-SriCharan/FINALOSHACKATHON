# Multi-Patient Monitoring Simulator Using Shared Memory and Synchronization

[![Language: C11](https://img.shields.io/badge/Language-C11-blue.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Platform: Linux/POSIX](https://img.shields.io/badge/Platform-Linux%2FPOSIX-orange.svg)](https://pubs.opengroup.org/)
[![Compiler: GCC](https://img.shields.io/badge/Compiler-GCC-green.svg)](https://gcc.gnu.org/)
[![OS Syllabus: CO2--CO6](https://img.shields.io/badge/OS_Syllabus-CO2--CO6-purple.svg)](#course-outcomes-co-syllabus-mapping)

> **ACADEMIC DISCLAIMER:**  
> This software is a pedagogical simulation developed strictly for academic Operating Systems laboratory demonstration and examination. It does **NOT** interface with clinical medical equipment and must **NEVER** be used for clinical diagnosis, medical telemetry, or real patient treatment.

---

## Table of Contents
1. [Overview & Primary Objective](#overview--primary-objective)
2. [Course Outcomes (CO) Syllabus Mapping](#course-outcomes-co-syllabus-mapping)
3. [Project Architecture](#project-architecture)
4. [Directory Structure](#directory-structure)
5. [Prerequisites & Build Instructions](#prerequisites--build-instructions)
6. [Running the Application](#running-the-application)
7. [External Control via Named FIFO](#external-control-via-named-fifo)
8. [Race Condition & Concurrency Test](#race-condition--concurrency-test)
9. [Automated Test Suite](#automated-test-suite)
10. [Sample Terminal Output](#sample-terminal-output)
11. [Operating Systems Viva Guide](#operating-systems-viva-guide)

---

## Overview & Primary Objective

The **Multi-Patient Monitoring Simulator** is a terminal-based Linux systems application that models a real-time clinical monitoring environment. Multiple concurrent processes cooperate to generate, exchange, analyze, and display physiological telemetry (Heart Rate in BPM, Blood Oxygen Saturation $SpO_2$ in %) for three patient beds.

### Key Highlights
- **POSIX Shared Memory:** High-speed data exchange via `shm_open()`, `ftruncate()`, and `mmap()`.
- **Process-Shared Mutual Exclusion:** Per-patient `pthread_mutex_t` configured with `PTHREAD_PROCESS_SHARED` and `PTHREAD_MUTEX_ROBUST` to eliminate torn reads and deadlocks.
- **Process Lifecycle Management:** Coordinated multi-process hierarchy using `fork()`, `waitpid()`, `kill()`, and signal handlers (`SIGINT`, `SIGTERM`, `SIGUSR1`).
- **Persistent Alert Audit Trail:** POSIX file I/O (`open()`, `write()`, `fsync()`, `close()`) persisting abnormal clinical vitals to `alerts.log`.
- **Smart Duplicate Alert Suppression:** Prevents log flooding during high-frequency polling.
- **Named FIFO Command Channel:** External runtime injection and administrative commands via `/tmp/patient_monitor_fifo`.
- **Controlled Race Demonstration:** Standalone experiment proving the necessity of synchronization.

---

## Course Outcomes (CO) Syllabus Mapping

| Course Outcome | OS Focus Area | Practical Implementation in Code | Source Files |
| :--- | :--- | :--- | :--- |
| **CO-2** | **Process Management** | • Creation of independent sensor processes, alert detector, and monitor via `fork()`<br>• Lifecycle monitoring, clean child reaping using `waitpid()`<br>• Asynchronous signals (`SIGINT`, `SIGTERM`, `SIGUSR1`) using `sigaction()` | [main.c](file:///C:/Users/mahan/.gemini/antigravity-ide/scratch/patient_monitor/src/main.c)<br>[sensor.c](file:///C:/Users/mahan/.gemini/antigravity-ide/scratch/patient_monitor/src/sensor.c) |
| **CO-3** | **Inter-Process Communication** | • Primary: POSIX shared memory (`/dev/shm/patient_monitor_shm`)<br>• Secondary: Named FIFO (`/tmp/patient_monitor_fifo`) for external runtime control<br>• Signal dispatch (`kill()`) for asynchronous notifications | [common.c](file:///C:/Users/mahan/.gemini/antigravity-ide/scratch/patient_monitor/src/common.c)<br>[fifo_cmd.c](file:///C:/Users/mahan/.gemini/antigravity-ide/scratch/patient_monitor/src/fifo_cmd.c) |
| **CO-4** | **Memory Management** | • Mapping shared physical memory frames into discrete process virtual address spaces via `mmap()`<br>• Virtual address translation concept demonstration<br>• Clean detachment via `munmap()` and removal via `shm_unlink()` | [common.h](file:///C:/Users/mahan/.gemini/antigravity-ide/scratch/patient_monitor/include/common.h)<br>[common.c](file:///C:/Users/mahan/.gemini/antigravity-ide/scratch/patient_monitor/src/common.c) |
| **CO-5** | **File Systems & File I/O** | • Low-level POSIX file descriptors (`open` with `O_CREAT \| O_WRONLY \| O_APPEND`)<br>• Immediate disk persistence via `write()` and `fsync()`<br>• Robust error reporting (`errno`) and post-simulation audit `--review-alerts` | [alert_detector.c](file:///C:/Users/mahan/.gemini/antigravity-ide/scratch/patient_monitor/src/alert_detector.c)<br>[common.c](file:///C:/Users/mahan/.gemini/antigravity-ide/scratch/patient_monitor/src/common.c) |
| **CO-6** | **Synchronization & Concurrency** | • Process-shared robust POSIX mutexes (`PTHREAD_PROCESS_SHARED`)<br>• Fine-grained per-patient locking preventing torn reads<br>• Controlled race condition experiment (`race_demo`) comparing unsync vs sync updates | [sync_helpers.h](file:///C:/Users/mahan/.gemini/antigravity-ide/scratch/patient_monitor/include/sync_helpers.h)<br>[race_demo.c](file:///C:/Users/mahan/.gemini/antigravity-ide/scratch/patient_monitor/tests/race_demo.c) |

---

## Project Architecture

```
                                  +-----------------------------+
                                  |   Master Process (main.c)   |
                                  |  - Manages Shared Memory    |
                                  |  - Spawns & Reaps Children  |
                                  +--------------+--------------+
                                                 | fork()
         +--------------------+------------------+------------------+--------------------+
         |                    |                  |                  |                    |
         v                    v                  v                  v                    v
  +--------------+     +--------------+   +--------------+   +--------------+     +--------------+
  |  Sensor #1   |     |  Sensor #2   |   |  Sensor #3   |   |Alert Detector|     |FIFO Listener |
  | Patient 101  |     | Patient 102  |   | Patient 103  |   | (Polling &   |     | (/tmp/       |
  | (Alice)      |     | (Bob)        |   | (Clara)      |   |  File I/O)   |     | patient_fifo)|
  +-------+------+     +-------+------+   +-------+------+   +-------+------+     +-------+------+
          |                    |                  |                  |                    |
          | Mutex Lock         | Mutex Lock       | Mutex Lock       | Mutex Lock Snapshot| Command /
          | Atomic Write       | Atomic Write     | Atomic Write     | Atomic Read        | Signal
          v                    v                  v                  v                    v
  +----------------------------------------------------------------------------------------------+
  |                   POSIX SHARED MEMORY REGION (/patient_monitor_shm)                         |
  |  - Global State: simulation_active, total_updates, total_alerts, global_lock                |
  |  - patients[0]: ID 101, HR, SpO2, Status, Seq #, Timestamp, Sensor PID, pthread_mutex_t    |
  |  - patients[1]: ID 102, HR, SpO2, Status, Seq #, Timestamp, Sensor PID, pthread_mutex_t    |
  |  - patients[2]: ID 103, HR, SpO2, Status, Seq #, Timestamp, Sensor PID, pthread_mutex_t    |
  +----------------------------------------------------------------------------------------------+
                                          ^                          |
                                          | Atomic Read Snapshot     | POSIX write()
                                          |                          v
                            +-------------+-------------+   +-------------------+
                            |  Live Terminal Dashboard  |   | Persistent File:  |
                            |  (ANSI Color Monitor)     |   |    alerts.log     |
                            +---------------------------+   +-------------------+
```

---

## Directory Structure

```
patient_monitor/
├── Makefile                # Build automation (all, clean, test, review-alerts)
├── README.md               # User manual and project documentation
├── include/
│   ├── common.h            # Data structures, constants, function prototypes
│   └── sync_helpers.h      # Process-shared robust POSIX mutex wrappers
├── src/
│   ├── main.c              # Master process: fork, lifecycle, signals, cleanup
│   ├── common.c            # SHM attachment, vitals evaluation, audit log review
│   ├── sensor.c            # Independent patient telemetry generator processes
│   ├── alert_detector.c    # Threshold evaluation, duplicate suppression, file I/O
│   ├── fifo_cmd.c          # Named FIFO IPC listener for administrative commands
│   └── monitor.c           # Real-time ANSI terminal telemetry dashboard
├── tests/
│   ├── race_demo.c         # Controlled race condition verification program
│   └── run_tests.sh        # Automated 8-step test harness
└── docs/
    └── ARCHITECTURE.md     # In-depth OS memory model, diagrams & Viva Q&A
```

---

## Prerequisites & Build Instructions

### Requirements
- **Operating System:** Linux (Ubuntu 20.04+, Debian, Fedora, Arch) or Windows Subsystem for Linux (WSL).
- **Toolchain:** `gcc` (with C11 support), `make`, `bash`.
- **POSIX Libraries:** `libpthread` and `librt` (standard on modern Linux distributions).

### Build Commands

```bash
# Clone or navigate to the project directory
cd patient_monitor

# Compile all binaries (patient_monitor and race_demo)
make all

# Clean all binaries, object files, and logs
make clean

# Display available targets
make help
```

---

## Running the Application

### 1. Interactive Live Mode
Runs the real-time clinical dashboard. Press **Ctrl+C** to trigger graceful multi-process shutdown.

```bash
./patient_monitor
```

### 2. Automated Demonstration Mode
Runs the live simulation for a specified number of seconds and automatically terminates cleanly.

```bash
./patient_monitor --demo-duration 10
```

### 3. Review Alert History (Audit Mode)
Inspects and pretty-prints previously logged abnormal physiological events from `alerts.log`.

```bash
./patient_monitor --review-alerts
# Or via Makefile shortcut:
make review-alerts
```

---

## External Control via Named FIFO

The application creates a Named FIFO at `/tmp/patient_monitor_fifo`. Open a **second terminal window** while the simulator is running to interact with it:

### Injected Emergency Anomaly
Inject severe Bradycardia ($HR=38$ BPM) and acute Hypoxemia ($SpO_2=84\%$) into Patient 101:
```bash
echo "inject 101 38.0 84.0" > /tmp/patient_monitor_fifo
```

### Inject Tachycardia
Inject elevated heart rate into Patient 102:
```bash
echo "inject 102 145.0 94.0" > /tmp/patient_monitor_fifo
```

### Trigger Process Signal (SIGUSR1)
Dispatch `SIGUSR1` to Sensor Process for Patient 103:
```bash
echo "signal 103" > /tmp/patient_monitor_fifo
```

### Query System Metrics
```bash
echo "status" > /tmp/patient_monitor_fifo
```

### Remote Graceful Shutdown
```bash
echo "shutdown" > /tmp/patient_monitor_fifo
```

---

## Race Condition & Concurrency Test

A standalone program, `race_demo`, proves the importance of OS synchronization by comparing **Unsynchronized vs Synchronized** access to shared memory.

### 1. Unsynchronized Execution (Exposing Race Conditions)
```bash
./race_demo unsync
```
**Results:**
- Non-atomic updates cause **over 10,000 torn reads (corrupted telemetry)**.
- Simultaneous increments cause **over 50,000 lost updates**.

### 2. Synchronized Execution (POSIX Mutex Enforced)
```bash
./race_demo sync
```
**Results:**
- **0 torn reads** (100% data integrity).
- **0 lost updates** (exact counter match).

---

## Automated Test Suite

Run the full automated test suite verifying all 5 syllabus outcomes:

```bash
make test
# Or directly:
./tests/run_tests.sh
```

### Test Suite Execution Summary:
- **Test 1:** Compilation verification with zero warnings.
- **Test 2:** Race condition demonstration (unsync data loss vs sync consistency).
- **Test 3:** Process lifecycle & telemetry generation across 3 independent sensors.
- **Test 4:** Named FIFO bidirectional IPC communication and remote shutdown.
- **Test 5:** Abnormal vital alert detection and POSIX file persistence in `alerts.log`.
- **Test 6:** Duplicate alert suppression during high-frequency polling.
- **Test 7:** Post-simulation audit reporting tool (`--review-alerts`).
- **Test 8:** Clean OS resource teardown verification (zero shared memory or FIFO leaks).

---

## Sample Terminal Output

### Live Clinical Dashboard
```
========================================================================================================
               MULTI-PATIENT CLINICAL TELEMETRY MONITOR (POSIX SHM & SYNCHRONIZATION)                   
========================================================================================================
 [ACADEMIC LAB SIMULATION ONLY] Not a medical device. Demonstrates OS IPC, Memory & Synchronization.
--------------------------------------------------------------------------------------------------------
 [SYSTEM RUNTIME] Elapsed: 00:08 | Updates: 24     | Alerts Logged: 2    | Master PID: 12450
 [PROCESS ROSTER] Sensor PIDs: [12451, 12452, 12453] | Alert Detector PID: 12454 | FIFO PID: 12455
--------------------------------------------------------------------------------------------------------

Bed/ID | Patient Profile          | Heart Rate   | SpO2 (%)   | Health Status  | Seq #    | Sensor | Timestamp          
--------------------------------------------------------------------------------------------------------
101    | Alice Walker (Cardio)    |  38.0 BPM    |  84.0 %    | [ CRITICAL ]   | 8        | 12451  | 2026-10-17 10:15:32
102    | Bob Martinez (ICU-1)     |  83.2 BPM    |  96.1 %    | [ NORMAL   ]   | 8        | 12452  | 2026-10-17 10:15:32
103    | Clara Zhao (Post-Op)     |  67.4 BPM    |  97.5 %    | [ NORMAL   ]   | 8        | 12453  | 2026-10-17 10:15:31
--------------------------------------------------------------------------------------------------------

 [RECENT ALERT AUDIT TRAIL] (Persistent in alerts.log)
  [2026-10-17 10:15:32] [CRITICAL] Patient ID: 101 (Alice Walker (Cardio)) | HR: 38.0 BPM | SpO2: 84.0% | Seq: 8 | Sensor PID: 12451 | Diagnosis: CRITICAL: Severe Bradycardia & Acute Hypoxemia
  [2026-10-17 10:15:30] [WARNING] Patient ID: 102 (Bob Martinez (ICU-1)) | HR: 124.0 BPM | SpO2: 95.0% | Seq: 6 | Sensor PID: 12452 | Diagnosis: WARNING: Elevated Heart Rate (>120 BPM)
--------------------------------------------------------------------------------------------------------
 [CONTROL CHANNELS] 
  - Named FIFO:  echo "inject <id> <hr> <spo2>" > /tmp/patient_monitor_fifo
                 (e.g., echo "inject 101 42 88" > /tmp/patient_monitor_fifo)
  - Signals:     Press Ctrl+C to trigger graceful shutdown.
========================================================================================================
```

### Post-Simulation Alert Audit (`./patient_monitor --review-alerts`)
```
========================================================================================
                          POST-SIMULATION AUDIT: ALERT HISTORY LOG                      
========================================================================================
Timestamp            | Level      | ID     | Vitals (HR/SpO2)   | Sequence #       | Physiological Diagnosis
--------------------------------------------------------------------------------------------------------------------
[2026-10-17 10:15:30] [WARNING] Patient ID: 102 (Bob Martinez (ICU-1)) | HR: 124.0 BPM | SpO2: 95.0% | Seq: 6 | Sensor PID: 12452 | Diagnosis: WARNING: Elevated Heart Rate (>120 BPM)
[2026-10-17 10:15:32] [CRITICAL] Patient ID: 101 (Alice Walker (Cardio)) | HR: 38.0 BPM | SpO2: 84.0% | Seq: 8 | Sensor PID: 12451 | Diagnosis: CRITICAL: Severe Bradycardia & Acute Hypoxemia
[2026-10-17 10:15:35] [RESOLVED] Patient ID: 101 (Alice Walker (Cardio)) | HR: 72.1 BPM | SpO2: 98.4% | Seq: 9 | Vitals normalized.
--------------------------------------------------------------------------------------------------------------------
Total Alert Entries Reviewed: 3
Log File Location: alerts.log
```

---

## Operating Systems Viva Guide

Detailed technical explanations, architectural diagrams, memory layout schematics, and model oral examination questions are available in:
**[docs/ARCHITECTURE.md](file:///C:/Users/mahan/.gemini/antigravity-ide/scratch/patient_monitor/docs/ARCHITECTURE.md)**.
