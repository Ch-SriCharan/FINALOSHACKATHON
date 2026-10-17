/**
 * ============================================================================
 * Project: Multi-Patient Monitoring Simulator Using Shared Memory & Synchronization
 * File:    src/fifo_cmd.c
 * Purpose: Named FIFO (Named Pipe) IPC mechanism for asynchronous control (CO-3).
 *
 * Concepts Demonstrated:
 *   - Named Pipe (FIFO): Unidirectional IPC channel created with mkfifo() that
 *     exists as a special file in the file system hierarchy (/tmp/patient_monitor_fifo).
 *   - Multi-process Communication: Allows external terminal users or automated scripts
 *     to inject emergency vitals or trigger system shutdown without direct shared memory access.
 *   - Non-blocking/Loop Handling: Reliably handles repeated opens and closes from
 *     transient shell writers (e.g. echo "cmd" > /tmp/patient_monitor_fifo).
 * ============================================================================
 */

#include "common.h"
#include "sync_helpers.h"

static volatile sig_atomic_t g_fifo_running = 1;

static void fifo_signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_fifo_running = 0;
    }
}

/**
 * Parses and dispatches administrative commands received through the FIFO.
 */
static void process_fifo_command(SharedMemory *shm, const char *line) {
    if (!shm || !line) return;

    int patient_id = 0;
    float hr = 0.0f;
    float spo2 = 0.0f;

    /* Skip leading whitespace */
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '\n') return;

    if (sscanf(line, "inject %d %f %f", &patient_id, &hr, &spo2) == 3) {
        /* Inject abnormal vitals into designated patient */
        int index = -1;
        for (int i = 0; i < MAX_PATIENTS; i++) {
            if (shm->patients[i].patient_id == patient_id) {
                index = i;
                break;
            }
        }

        if (index != -1) {
            if (safe_mutex_lock(&shm->patients[index].lock) == 0) {
                shm->patients[index].heart_rate = hr;
                shm->patients[index].spo2 = spo2;
                char reason[REASON_LEN];
                shm->patients[index].status = evaluate_vitals_status(hr, spo2, reason, sizeof(reason));
                shm->patients[index].sequence_number++;
                get_current_timestamp(shm->patients[index].timestamp, TIMESTAMP_LEN);
                safe_mutex_unlock(&shm->patients[index].lock);

                fprintf(stderr, "\n[FIFO CMD] Injected vitals for Patient %d: HR=%.1f BPM, SpO2=%.1f%%\n",
                        patient_id, hr, spo2);
            }
        } else {
            fprintf(stderr, "[FIFO CMD ERROR] Patient ID %d not found (valid: 101, 102, 103)\n", patient_id);
        }
    } else if (sscanf(line, "signal %d", &patient_id) == 1) {
        /* Send SIGUSR1 to target sensor process (CO-2, CO-3) */
        int index = -1;
        for (int i = 0; i < MAX_PATIENTS; i++) {
            if (shm->patients[i].patient_id == patient_id) {
                index = i;
                break;
            }
        }

        if (index != -1 && shm->sensor_pids[index] > 0) {
            kill(shm->sensor_pids[index], SIGUSR1);
            fprintf(stderr, "\n[FIFO CMD] Dispatched SIGUSR1 to Sensor PID %d (Patient %d)\n",
                    shm->sensor_pids[index], patient_id);
        } else {
            fprintf(stderr, "[FIFO CMD ERROR] Sensor process for Patient %d not found\n", patient_id);
        }
    } else if (strncmp(line, "shutdown", 8) == 0) {
        fprintf(stderr, "\n[FIFO CMD] Received remote shutdown request. Notifying Master PID %d...\n",
                shm->master_pid);
        kill(shm->master_pid, SIGINT);
    } else if (strncmp(line, "status", 6) == 0) {
        fprintf(stderr, "\n[FIFO CMD] System Status: Active=%d, Total Updates=%lu, Total Alerts=%lu\n",
                shm->simulation_active,
                (unsigned long)shm->total_sensor_updates,
                (unsigned long)shm->total_alerts_triggered);
    } else {
        fprintf(stderr, "[FIFO CMD] Unrecognized command: %s", line);
        fprintf(stderr, "           Supported commands:\n");
        fprintf(stderr, "             inject <id> <hr> <spo2>   (e.g., inject 101 42 88)\n");
        fprintf(stderr, "             signal <id>               (dispatches SIGUSR1)\n");
        fprintf(stderr, "             status                    (queries metrics)\n");
        fprintf(stderr, "             shutdown                  (initiates clean termination)\n");
    }
}

/**
 * Main loop for the Named FIFO Listener process.
 */
void run_fifo_listener_process(void) {
    /* Setup signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fifo_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Attach shared memory */
    SharedMemory *shm = shm_attach(false);
    if (!shm) {
        fprintf(stderr, "[FIFO PID %d] Failed to attach to shared memory\n", getpid());
        exit(EXIT_FAILURE);
    }

    /* Create Named FIFO in the filesystem (CO-3) */
    unlink(FIFO_NAME);
    if (mkfifo(FIFO_NAME, 0666) == -1) {
        if (errno != EEXIST) {
            perror("[FIFO ERROR] mkfifo failed");
            shm_detach(shm);
            exit(EXIT_FAILURE);
        }
    }

    /* Open FIFO with O_RDWR in Linux so read() blocks cleanly without EOF on writer close */
    int fd = open(FIFO_NAME, O_RDWR);
    if (fd == -1) {
        perror("[FIFO ERROR] open failed");
        unlink(FIFO_NAME);
        shm_detach(shm);
        exit(EXIT_FAILURE);
    }

    char line_buffer[256];
    while (g_fifo_running && shm->simulation_active) {
        ssize_t bytes_read = read(fd, line_buffer, sizeof(line_buffer) - 1);
        if (bytes_read > 0) {
            line_buffer[bytes_read] = '\0';
            process_fifo_command(shm, line_buffer);
        } else if (bytes_read < 0) {
            if (errno == EINTR) continue;
            perror("[FIFO ERROR] read failed");
            break;
        }
    }

    close(fd);
    unlink(FIFO_NAME);
    shm_detach(shm);
    exit(EXIT_SUCCESS);
}
