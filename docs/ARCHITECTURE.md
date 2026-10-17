# Multi-Patient Monitoring Simulator: System Architecture & OS Engineering Guide

> **DISCLAIMER:**  
> This software is an academic Operating Systems pedagogical laboratory simulation. It does **NOT** interface with clinical hardware and must **NEVER** be used for medical diagnosis, clinical telemetry, or life-critical patient monitoring.

---

## 1. Executive Summary & Syllabus Alignment

This project implements a multi-process, terminal-based clinical telemetry simulator in **C (C11)** using **Linux POSIX system calls** and **GCC**. It demonstrates how an operating system coordinates concurrent, autonomous tasks using **POSIX shared memory**, **process-shared synchronization primitives**, **named pipes (FIFO)**, **signals**, and **POSIX file I/O**.

### Course Outcome (CO) Mapping Matrix

| Course Outcome | OS Focus Area | Implementation Mechanism in This Project | Primary Files |
| :--- | :--- | :--- | :--- |
| **CO-2** | **Process Management** | Process tree creation via `fork()`, independent scheduling of 3 sensor processes, 1 alert detector process, and 1 FIFO listener. Lifecycle tracking with `waitpid()`, signal dispatch via `kill()`, and complete prevention of zombie processes. | `src/main.c`<br>`src/sensor.c` |
| **CO-3** | **Inter-Process Communication** | High-throughput POSIX shared memory (`shm_open`, `ftruncate`, `mmap`, `shm_unlink`), asynchronous signal handling (`SIGINT`, `SIGTERM`, `SIGUSR1`), and a Named FIFO (`mkfifo`, `/tmp/patient_monitor_fifo`) for external runtime control. | `src/common.c`<br>`src/fifo_cmd.c` |
| **CO-4** | **Memory Management** | Kernel page table mapping into discrete process virtual address spaces via `mmap()`, shared record layout design, memory protection (`PROT_READ \| PROT_WRITE`), and safe release via `munmap()`. | `include/common.h`<br>`src/common.c` |
| **CO-5** | **File Systems & File I/O** | Atomic appending of abnormal clinical telemetry alerts to `alerts.log` using low-level POSIX file descriptors (`open()`, `write()`, `fsync()`, `close()`), error detection, and historical post-simulation audit reporting (`--review-alerts`). | `src/alert_detector.c`<br>`src/common.c` |
| **CO-6** | **Synchronization & Concurrency** | Process-shared robust POSIX mutexes (`PTHREAD_PROCESS_SHARED`, `PTHREAD_MUTEX_ROBUST`) protecting fine-grained critical sections. Controlled race condition laboratory experiment (`tests/race_demo.c`) verifying elimination of torn reads and lost updates. | `include/sync_helpers.h`<br>`tests/race_demo.c` |

---

## 2. Process Architecture & Hierarchy

The application follows a hierarchical master-worker multi-process architecture:

```mermaid
graph TD
    Master[Master Process (PID: Master)] -->|fork| Sensor1[Sensor 1: Patient 101 (PID: S1)]
    Master -->|fork| Sensor2[Sensor 2: Patient 102 (PID: S2)]
    Master -->|fork| Sensor3[Sensor 3: Patient 103 (PID: S3)]
    Master -->|fork| AlertDet[Alert Detector Process (PID: A1)]
    Master -->|fork| FIFOProc[Named FIFO Listener (PID: F1)]
    Master --> Dashboard[Live Terminal Dashboard Loop]

    Sensor1 -->|POSIX Mutex Write| SHM[(POSIX Shared Memory: /patient_monitor_shm)]
    Sensor2 -->|POSIX Mutex Write| SHM
    Sensor3 -->|POSIX Mutex Write| SHM

    SHM -->|POSIX Mutex Read Snapshot| Dashboard
    SHM -->|POSIX Mutex Read Snapshot| AlertDet

    AlertDet -->|POSIX write| LogFile[(Persistent File: alerts.log)]
    ExtUser[External Terminal / Scripts] -->|echo 'inject...' > FIFO| FIFOPipe[(Named FIFO: /tmp/patient_monitor_fifo)]
    FIFOPipe -->|read| FIFOProc
    FIFOProc -->|Update Vitals / Signals| SHM
```

### Process Roles

1. **Master Orchestrator (`src/main.c`)**:
   - Creates the shared memory object (`shm_open`) and sizes it (`ftruncate`).
   - Maps shared memory into its address space (`mmap`).
   - Initializes process-shared mutexes with `PTHREAD_PROCESS_SHARED` and `PTHREAD_MUTEX_ROBUST`.
   - Spawns all child processes using `fork()`.
   - Captures terminal signals (`SIGINT`, `SIGTERM`).
   - Runs the live dashboard.
   - Upon termination, broadcasts `SIGTERM`, waits for all children with `waitpid()`, destroys mutexes, and unlinks shared memory and the FIFO.

2. **Patient Sensor Processes (`src/sensor.c`)**:
   - Three independent worker processes (one for each patient bed: 101, 102, 103).
   - Generate realistic physiological telemetry (Heart Rate in BPM, SpO2 in %) with random-walk jitter and periodic simulated anomalies.
   - Enters critical section with `safe_mutex_lock(&shm->patients[i].lock)` to perform atomic writes.
   - Increments monotonically increasing `sequence_number`.
   - Sleeps for 800ms - 1200ms with jitter to simulate asynchronous sensor hardware.

3. **Alert Detection Process (`src/alert_detector.c`)**:
   - Autonomous monitoring process running at high polling frequency (every 300ms).
   - Acquires lock on each patient record, copies a clean local snapshot, and releases lock immediately to minimize critical section latency.
   - **Duplicate Alert Suppression**: Tracks `last_seen_seq[i]` and `last_seen_status[i]`. Only logs an alert if a *new* reading has arrived (`seq != last_seen_seq`) AND either vitals are abnormal (WARNING/CRITICAL) or have just transitioned back to normal (RESOLVED).
   - Writes timestamped entries to `alerts.log` using POSIX `open`, `write`, `fsync`, `close`.

4. **Named FIFO Listener Process (`src/fifo_cmd.c`)**:
   - Demonstrates traditional IPC (Named Pipes / FIFOs).
   - Creates `/tmp/patient_monitor_fifo` via `mkfifo()`.
   - Listens for administrative commands from external shells:
     - `inject <patient_id> <hr> <spo2>`: Simulates immediate medical emergency on a specific patient bed.
     - `signal <patient_id>`: Dispatches `SIGUSR1` to that sensor process.
     - `status`: Logs diagnostic status.
     - `shutdown`: Asynchronously triggers system shutdown via `kill(master_pid, SIGINT)`.

---

## 3. Memory Management: Virtual Address Space & Shared Memory (CO-4)

### How Shared Memory Operates in the Linux Kernel

When independent processes execute in Linux, the CPU Memory Management Unit (MMU) enforces **virtual memory isolation**: process A cannot directly read or write process B's virtual memory addresses.

POSIX Shared Memory bypasses this isolation in a controlled, kernel-managed manner:

```
Process A Virtual Address Space           Physical RAM Frames          Process B Virtual Address Space
+-------------------------------+       +---------------------+       +-------------------------------+
| Code (.text)                  |       | Page Frame 0x7A100  |       | Code (.text)                  |
| Data (.data, .bss)            |       | Page Frame 0x7A101  |       | Data (.data, .bss)            |
| Heap                          |       |                     |       | Heap                          |
| ...                           |       | [SharedMemory Struct|       | ...                           |
| MMAP Region: 0x7fff80000000 --+------>|  - simulation_active| <-----+-- MMAP Region: 0x7fff40000000 |
|   (Virtual Address in Proc A) |       |  - global_lock      |       |   (Virtual Address in Proc B) |
| Stack                         |       |  - patients[0..2]   |       | Stack                         |
+-------------------------------+       +---------------------+       +-------------------------------+
```

### Steps Executed in Code:
1. `shm_open("/patient_monitor_shm", O_CREAT | O_RDWR, 0666)`:
   Allocates an inode in the Linux `tmpfs` virtual filesystem located at `/dev/shm/patient_monitor_shm`.
2. `ftruncate(fd, sizeof(SharedMemory))`:
   Sets the size of the kernel memory object to fit our struct.
3. `mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)`:
   The kernel page table creates entries mapping the physical RAM frames into the virtual memory page table of the calling process.
4. **Important Note for OS Viva:**
   The returned pointer (`void *addr`) may be at different numerical virtual addresses in Process A (e.g. `0x7fff80000000`) and Process B (e.g. `0x7fff40000000`), but both virtual addresses translate through the MMU page table to the **exact same physical RAM page frames**!

### Data Structure Definition:
```c
typedef struct {
    int patient_id;                     // 101, 102, 103
    char name[PATIENT_NAME_LEN];        // "Alice Walker (Cardio)", etc.
    float heart_rate;                   // Beats per minute
    float spo2;                         // Oxygen saturation %
    PatientStatus status;               // STATUS_NORMAL, STATUS_WARNING, STATUS_CRITICAL
    char timestamp[TIMESTAMP_LEN];      // "YYYY-MM-DD HH:MM:SS"
    uint64_t sequence_number;           // Incremented on every sensor update
    pid_t sensor_pid;                   // PID of generating sensor process
    pthread_mutex_t lock;               // Per-patient process-shared POSIX mutex
} PatientRecord;
```

---

## 4. Synchronization, Critical Sections & Race Prevention (CO-6)

### Mutual Exclusion Primitives

Because multiple processes (Sensors 1-3, Alert Detector, Monitor, FIFO Listener) access the shared memory structure concurrently, **race conditions** will occur without synchronization:
- **Torn Reads / Data Tearing:** A reader process reads `heart_rate` updated by the sensor, but reads `spo2` or `timestamp` from the *previous* update cycle because multi-word stores are not atomic in x86/ARM hardware.
- **Lost Updates:** Concurrent increments of `total_sensor_updates++` involve three assembly instructions (`MOV`, `ADD`, `MOV`). If two processes execute this simultaneously, one increment is lost.

### Our Synchronization Solution

1. **Per-Patient Process-Shared Mutex (`PTHREAD_PROCESS_SHARED`)**:
   Instead of a single coarse-grained global lock that forces all sensors to queue up behind each other, each patient record has its own dedicated mutex:
   ```c
   pthread_mutexattr_t attr;
   pthread_mutexattr_init(&attr);
   pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
   pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
   pthread_mutex_init(&record->lock, &attr);
   ```
2. **Robust Mutex (`PTHREAD_MUTEX_ROBUST`)**:
   If a sensor process crashes or is killed with `kill -9` while inside its critical section, standard mutexes deadlock all other waiting processes indefinitely. With `PTHREAD_MUTEX_ROBUST`, the next process calling `pthread_mutex_lock()` receives `EOWNERDEAD`, calls `pthread_mutex_consistent()`, and safely recovers without deadlocking the operating system!

### Controlled Race Condition Experiment (`tests/race_demo.c`)

We created a dedicated verification program comparing **Unsynchronized vs Synchronized** execution under heavy concurrency (4 worker processes, 80,000 total updates, simultaneous validator reader):

| Metric | Unsynchronized Mode (`./race_demo unsync`) | Synchronized Mode (`./race_demo sync`) |
| :--- | :--- | :--- |
| **Critical Section Lock** | None (Disabled) | Process-shared `pthread_mutex_t` |
| **Total Validation Reads** | 11,071 reads | 7,616 reads |
| **Torn Reads (Data Corruption)** | **10,132 corrupted reads detected!** | **0 (100% clean data)** |
| **Expected Counter Value** | 80,000 | 80,000 |
| **Actual Counter Value** | 25,605 | 80,000 |
| **Lost Updates (Data Loss)** | **54,395 lost updates!** | **0 lost updates (Exact)** |
| **Consistency Outcome** | **RACE CONDITION DEMONSTRATED** | **100.00% DATA CONSISTENCY** |

---

## 5. File Systems & Persistent Alert Logging (CO-5)

Abnormal physiological events (Bradycardia, Tachycardia, Hypoxemia) are detected and persisted to `alerts.log`.

### Low-Level POSIX File I/O
Instead of standard C buffered streams (`fopen`, `fprintf`), the alert detector utilizes POSIX system calls:
```c
int fd = open("alerts.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
write(fd, log_buffer, len);
fsync(fd);  // Flushes kernel disk write cache immediately
close(fd);
```

### Duplicate Alert Suppression Algorithm
In high-frequency monitoring systems, polling every 300ms against a 1-second sensor update rate would generate duplicate alert entries. Our system suppresses duplicates:
```c
if (local_copy.sequence_number != last_seen_seq[i]) {
    last_seen_seq[i] = local_copy.sequence_number;
    if (evaluated != STATUS_NORMAL) {
        log_alert_to_file(...); // Log abnormal event
        last_seen_status[i] = evaluated;
    } else if (last_seen_status[i] != STATUS_NORMAL) {
        log_alert_to_file(...); // Log "RESOLVED: Vitals normalized"
        last_seen_status[i] = STATUS_NORMAL;
    }
}
```

---

## 6. Operating Systems Lab Viva Questions & Answers

### Q1: What is the fundamental difference between System V Shared Memory and POSIX Shared Memory?
**Answer:**
System V shared memory uses numeric keys (`ftok()`), `shmget()`, `shmat()`, `shmdt()`, and `shmctl()`.  
POSIX shared memory uses file-path-like names (e.g. `"/patient_monitor_shm"`), integrates with the standard file descriptor abstraction via `shm_open()`, uses `ftruncate()` to size the segment, and maps it using standard `mmap()`. POSIX shared memory segments are visible in the Linux virtual filesystem under `/dev/shm`.

### Q2: Why is `ftruncate()` necessary after `shm_open()`?
**Answer:**
When `shm_open()` creates a new POSIX shared memory object with `O_CREAT`, its initial size is 0 bytes. Attempting to `mmap()` a 0-byte shared memory object causes `mmap()` to fail or raises a `SIGBUS` (bus error) signal when accessing the memory. `ftruncate(fd, size)` sets the object's length in bytes so the kernel allocates the required backing pages.

### Q3: What is a "race condition", and how did this project demonstrate it?
**Answer:**
A race condition occurs when multiple processes access and manipulate shared data concurrently, and the final outcome depends on the non-deterministic timing of process scheduling.  
In our `tests/race_demo.c` test, 4 processes incremented a counter without a mutex, losing 54,395 updates out of 80,000 due to preempted read-modify-write cycles. Simultaneously, the validator process detected 10,132 torn reads where telemetry fields were read in a partially updated state.

### Q4: How does a process-shared mutex differ from an ordinary thread mutex?
**Answer:**
By default, a POSIX mutex has its process-shared attribute set to `PTHREAD_PROCESS_PRIVATE`, meaning it can only synchronize threads inside the same process virtual address space. By configuring `pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED)` and placing the mutex in a POSIX shared memory region, the Linux kernel allows independent processes with different virtual address spaces to acquire and release the same lock.

### Q5: What is a zombie process, and how does this application prevent zombie creation?
**Answer:**
A zombie process is a child process that has completed execution via `exit()` or `_exit()`, but still possesses an entry in the OS process table because its parent has not yet read its exit status code.  
In `src/main.c`, when the master process initiates graceful shutdown, it invokes `waitpid(-1, &status, 0)` in a loop until all child PIDs are reaped, releasing all task structs from the kernel process table.

### Q6: What is a Named FIFO, and why is it useful alongside Shared Memory?
**Answer:**
A Named FIFO (Named Pipe) is a special file on the filesystem created using `mkfifo()`. Unlike shared memory (which provides high-speed random access), FIFOs provide a unidirectional byte-stream IPC queue. It enables external processes (like a shell or admin script running in another terminal window) to issue asynchronous commands (such as `echo "inject 101 42 88" > /tmp/patient_monitor_fifo`) without needing to attach to the shared memory segment or know its internal struct layout.
