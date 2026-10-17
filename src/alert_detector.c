/**
 * ============================================================================
 * Project: Multi-Patient Monitoring Simulator Using Shared Memory & Synchronization
 * File:    src/alert_detector.c
 * Purpose: Alert evaluation, duplicate suppression, and file logging (CO-2, CO-5, CO-6).
 *
 * Concepts Demonstrated:
 *   - Independent Alert Process: Continuously analyzes shared patient data
 *     without altering sensor behavior.
 *   - Atomic Snapshotting (CO-6): Uses per-patient mutex to copy a consistent
 *     state into local memory, then immediately releases the lock.
 *   - Duplicate Alert Prevention: Maintains sequence number cache to avoid
 *     re-logging the same reading during high-frequency polling.
 *   - POSIX File I/O (CO-5): Appends timestamped events to alerts.log using
 *     low-level open(), write(), and close() system calls with error checking.
 * ============================================================================
 */

#include "common.h"
#include "sync_helpers.h"

static volatile sig_atomic_t g_detector_running = 1;

static void detector_signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_detector_running = 0;
    }
}

/**
 * Appends an alert entry to the persistent log file using POSIX file descriptors (CO-5).
 */
static void log_alert_to_file(const char *entry) {
    if (!entry) return;

    /* open() system call with O_CREAT, O_WRONLY, O_APPEND flags */
    int fd = open(ALERT_LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd == -1) {
        perror("[ALERT DETECTOR] Error opening alerts.log");
        return;
    }

    size_t len = strlen(entry);
    ssize_t written = write(fd, entry, len);
    if (written == -1) {
        perror("[ALERT DETECTOR] Error writing to alerts.log");
    } else if ((size_t)written < len) {
        fprintf(stderr, "[ALERT DETECTOR] Partial write occurred to alerts.log\n");
    }

    /* Ensure buffer is pushed to disk */
    fsync(fd);
    close(fd);
}

/**
 * Main loop for the Alert Detection process.
 */
void run_alert_detector_process(void) {
    /* Setup signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = detector_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    SharedMemory *shm = shm_attach(false);
    if (!shm) {
        fprintf(stderr, "[Alert Detector PID %d] Failed to attach to shared memory\n", getpid());
        exit(EXIT_FAILURE);
    }

    /* Local tracking to prevent duplicate alerts during polling */
    uint64_t last_seen_seq[MAX_PATIENTS] = { 0 };
    PatientStatus last_seen_status[MAX_PATIENTS] = { STATUS_NORMAL };

    /* Fast polling cycle (300ms) to ensure timely detection */
    const useconds_t poll_interval_usec = 300000;

    while (g_detector_running && shm->simulation_active) {
        for (int i = 0; i < MAX_PATIENTS; i++) {
            PatientRecord local_copy;
            bool read_success = false;

            /* ----------------------------------------------------------------
             * CRITICAL SECTION (CO-6): Atomic read of patient record
             * ---------------------------------------------------------------- */
            if (safe_mutex_lock(&shm->patients[i].lock) == 0) {
                /* Copy full record to eliminate torn reads across multiple fields */
                local_copy = shm->patients[i];
                safe_mutex_unlock(&shm->patients[i].lock);
                read_success = true;
            }

            if (!read_success) continue;

            /* Check if this record has a new reading we haven't processed yet */
            if (local_copy.sequence_number != last_seen_seq[i]) {
                last_seen_seq[i] = local_copy.sequence_number;

                char reason[REASON_LEN];
                PatientStatus evaluated = evaluate_vitals_status(
                    local_copy.heart_rate, local_copy.spo2, reason, sizeof(reason));

                /* Case A: Abnormal reading detected (WARNING or CRITICAL) */
                if (evaluated == STATUS_WARNING || evaluated == STATUS_CRITICAL) {
                    char log_buffer[512];
                    snprintf(log_buffer, sizeof(log_buffer),
                             "[%s] [%s] Patient ID: %d (%s) | HR: %.1f BPM | SpO2: %.1f%% | Seq: %lu | Sensor PID: %d | Diagnosis: %s\n",
                             local_copy.timestamp,
                             status_to_string(evaluated),
                             local_copy.patient_id,
                             local_copy.name,
                             local_copy.heart_rate,
                             local_copy.spo2,
                             (unsigned long)local_copy.sequence_number,
                             local_copy.sensor_pid,
                             reason);

                    /* Write alert via POSIX file I/O */
                    log_alert_to_file(log_buffer);

                    /* Update shared alert count */
                    if (safe_mutex_lock(&shm->global_lock) == 0) {
                        shm->total_alerts_triggered++;
                        safe_mutex_unlock(&shm->global_lock);
                    }

                    last_seen_status[i] = evaluated;
                }
                /* Case B: Vitals resolved back to normal from an abnormal state */
                else if (last_seen_status[i] != STATUS_NORMAL && evaluated == STATUS_NORMAL) {
                    char log_buffer[512];
                    snprintf(log_buffer, sizeof(log_buffer),
                             "[%s] [RESOLVED] Patient ID: %d (%s) | HR: %.1f BPM | SpO2: %.1f%% | Seq: %lu | Vitals normalized.\n",
                             local_copy.timestamp,
                             local_copy.patient_id,
                             local_copy.name,
                             local_copy.heart_rate,
                             local_copy.spo2,
                             (unsigned long)local_copy.sequence_number);

                    log_alert_to_file(log_buffer);
                    last_seen_status[i] = STATUS_NORMAL;
                }
            }
            /* If sequence_number == last_seen_seq[i], NO duplicate alert is logged! */
        }

        usleep(poll_interval_usec);
    }

    shm_detach(shm);
    exit(EXIT_SUCCESS);
}
