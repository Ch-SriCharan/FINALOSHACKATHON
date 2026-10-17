/**
 * ============================================================================
 * Project: Multi-Patient Monitoring Simulator Using Shared Memory & Synchronization
 * File:    include/common.h
 * Purpose: Global constants, data structures, and function prototypes
 *
 * Operating Systems Syllabus Mapping:
 *   - CO-2: Process Management (lifecycle, fork, waitpid, signals)
 *   - CO-3: Inter-Process Communication (POSIX Shared Memory, FIFO, Signals)
 *   - CO-4: Memory Management (shm_open, ftruncate, mmap, virtual address spaces)
 *   - CO-5: File Systems & File I/O (alerts.log, open, write, close, fsync)
 *   - CO-6: Synchronization & Concurrency (Process-shared POSIX mutexes, race prevention)
 *
 * DISCLAIMER:
 * This software is a pedagogical simulation designed strictly for academic
 * and Operating Systems laboratory demonstration. It is NOT a real-world
 * medical monitoring device and must not be used for medical diagnosis.
 * ============================================================================
 */

#ifndef COMMON_H
#define COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>

/* ============================================================================
 * Configuration Constants
 * ============================================================================ */
#define MAX_PATIENTS        3
#define PATIENT_NAME_LEN    32
#define TIMESTAMP_LEN       32
#define REASON_LEN          64
#define SHM_NAME            "/patient_monitor_shm"
#define FIFO_NAME           "/tmp/patient_monitor_fifo"
#define ALERT_LOG_FILE      "alerts.log"

/* Configurable Demonstration Vitals Thresholds */
#define NORMAL_HR_MIN       60.0f
#define NORMAL_HR_MAX       100.0f
#define WARNING_HR_MIN      50.0f
#define WARNING_HR_MAX      120.0f

#define NORMAL_SPO2_MIN     95.0f
#define WARNING_SPO2_MIN    90.0f

/* ANSI Color Escape Codes for Terminal Presentation */
#define ANSI_RESET          "\033[0m"
#define ANSI_BOLD           "\033[1m"
#define ANSI_DIM            "\033[2m"
#define ANSI_RED            "\033[31m"
#define ANSI_GREEN          "\033[32m"
#define ANSI_YELLOW         "\033[33m"
#define ANSI_BLUE           "\033[34m"
#define ANSI_MAGENTA        "\033[35m"
#define ANSI_CYAN           "\033[36m"
#define ANSI_WHITE          "\033[37m"
#define ANSI_BG_RED         "\033[41m"
#define ANSI_BG_YELLOW      "\033[43m"
#define ANSI_BG_GREEN       "\033[42m"
#define ANSI_CLEAR_SCREEN   "\033[2J\033[H"

/* ============================================================================
 * Data Types and Enumerations
 * ============================================================================ */

/* Patient Health Classification */
typedef enum {
    STATUS_NORMAL = 0,      /* Vitals within safe physiological ranges */
    STATUS_WARNING = 1,     /* Mild bradycardia/tachycardia or mild desaturation */
    STATUS_CRITICAL = 2     /* Severe cardiac event or acute hypoxemia */
} PatientStatus;

/* Shared Record for a Single Patient */
typedef struct {
    int patient_id;                     /* e.g., 101, 102, 103 */
    char name[PATIENT_NAME_LEN];        /* Human-readable label (e.g. "Alice Walker") */
    float heart_rate;                   /* Heart rate in beats per minute (BPM) */
    float spo2;                         /* Blood oxygen saturation percentage (%) */
    PatientStatus status;               /* Evaluated condition status */
    char timestamp[TIMESTAMP_LEN];      /* ISO-like timestamp of the last reading */
    uint64_t sequence_number;           /* Monotonically increasing update counter */
    pid_t sensor_pid;                   /* Process ID of the sensor that generated reading */
    pthread_mutex_t lock;               /* Process-shared POSIX mutex for this record */
} PatientRecord;

/* Master Shared Memory Region (CO-4) */
typedef struct {
    volatile sig_atomic_t simulation_active; /* 1 = active, 0 = initiate shutdown */
    pthread_mutex_t global_lock;             /* Process-shared mutex for global metrics */
    
    PatientRecord patients[MAX_PATIENTS];    /* Fixed array of patient records */
    
    uint64_t total_sensor_updates;           /* Aggregate count of updates across sensors */
    uint64_t total_alerts_triggered;         /* Aggregate count of alerts recorded */
    
    /* Process Identification for Monitoring and Graceful Cleanup */
    pid_t master_pid;
    pid_t sensor_pids[MAX_PATIENTS];
    pid_t alert_detector_pid;
    pid_t fifo_listener_pid;
} SharedMemory;

/* ============================================================================
 * Function Declarations
 * ============================================================================ */

/* Utility & Formatting */
void get_current_timestamp(char *buffer, size_t max_len);
PatientStatus evaluate_vitals_status(float hr, float spo2, char *reason_buf, size_t max_len);
const char* status_to_string(PatientStatus status);
const char* status_to_color_badge(PatientStatus status);
void print_disclaimer(void);

/* Shared Memory Lifecycle (CO-3, CO-4) */
SharedMemory* shm_attach(bool create);
void shm_detach(SharedMemory *shm);
void shm_destroy(void);

/* Sub-process Routines (CO-2) */
void run_sensor_process(int patient_index);
void run_alert_detector_process(void);
void run_fifo_listener_process(void);
void run_monitor_dashboard(int demo_duration_sec);

/* Alert History Management (CO-5) */
void review_alerts_log(void);

#endif /* COMMON_H */
