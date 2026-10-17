/**
 * ============================================================================
 * Project: Multi-Patient Monitoring Simulator Using Shared Memory & Synchronization
 * File:    src/monitor.c
 * Purpose: Terminal-based real-time telemetry dashboard (CO-2, CO-6).
 *
 * Concepts Demonstrated:
 *   - Atomic Reads (CO-6): Uses per-patient mutex locks to capture a fully
 *     consistent snapshot of each patient record.
 *   - Dynamic Terminal Presentation: Visual display with ANSI colors, status badges,
 *     and live health indicators.
 *   - Non-blocking Telemetry: Refreshes at a steady cadence (500ms - 800ms) without
 *     stalling or starving sensor writer processes.
 * ============================================================================
 */

#include "common.h"
#include "sync_helpers.h"

/**
 * Fetches the last few lines from alerts.log to show in the live dashboard preview.
 */
static void get_recent_alerts_preview(char *output_buf, size_t max_len, int max_lines) {
    if (!output_buf || max_len == 0) return;
    output_buf[0] = '\0';

    FILE *fp = fopen(ALERT_LOG_FILE, "r");
    if (!fp) {
        snprintf(output_buf, max_len, "  (No alerts logged yet - all vitals operating within normal parameters)\n");
        return;
    }

    /* Read lines into circular buffer */
    char lines[10][256];
    int count = 0;
    char current[256];

    while (fgets(current, sizeof(current), fp)) {
        current[strcspn(current, "\r\n")] = 0; /* strip newline */
        snprintf(lines[count % 10], sizeof(lines[0]), "%s", current);
        count++;
    }
    fclose(fp);

    if (count == 0) {
        snprintf(output_buf, max_len, "  (No alerts logged yet - all vitals operating within normal parameters)\n");
        return;
    }

    int lines_to_show = count < max_lines ? count : max_lines;
    int start_index = count - lines_to_show;

    size_t offset = 0;
    for (int i = 0; i < lines_to_show; i++) {
        int idx = (start_index + i) % 10;
        int written = snprintf(output_buf + offset, max_len - offset, "  %s\n", lines[idx]);
        if (written > 0) offset += written;
        if (offset >= max_len - 1) break;
    }
}

/**
 * Executes the terminal monitoring dashboard.
 *
 * @param demo_duration_sec If > 0, automatically shuts down after this many seconds.
 */
void run_monitor_dashboard(int demo_duration_sec) {
    SharedMemory *shm = shm_attach(false);
    if (!shm) {
        fprintf(stderr, "[Monitor] Failed to attach to shared memory\n");
        return;
    }

    time_t start_time = time(NULL);
    uint32_t frame_count = 0;

    while (shm->simulation_active) {
        time_t now = time(NULL);
        int elapsed_sec = (int)difftime(now, start_time);

        if (demo_duration_sec > 0 && elapsed_sec >= demo_duration_sec) {
            printf("\n[Monitor] Demo duration of %d seconds elapsed. Requesting graceful shutdown...\n", demo_duration_sec);
            shm->simulation_active = 0;
            break;
        }

        /* --------------------------------------------------------------------
         * Capture synchronized snapshot of all patient records
         * -------------------------------------------------------------------- */
        PatientRecord snapshots[MAX_PATIENTS];
        for (int i = 0; i < MAX_PATIENTS; i++) {
            if (safe_mutex_lock(&shm->patients[i].lock) == 0) {
                snapshots[i] = shm->patients[i];
                safe_mutex_unlock(&shm->patients[i].lock);
            }
        }

        /* Read aggregate metrics */
        uint64_t total_updates = 0;
        uint64_t total_alerts = 0;
        if (safe_mutex_lock(&shm->global_lock) == 0) {
            total_updates = shm->total_sensor_updates;
            total_alerts = shm->total_alerts_triggered;
            safe_mutex_unlock(&shm->global_lock);
        }

        /* --------------------------------------------------------------------
         * Render ANSI Terminal Dashboard
         * -------------------------------------------------------------------- */
        printf(ANSI_CLEAR_SCREEN);
        printf(ANSI_BOLD ANSI_CYAN);
        printf("========================================================================================================\n");
        printf("               MULTI-PATIENT CLINICAL TELEMETRY MONITOR (POSIX SHM & SYNCHRONIZATION)                   \n");
        printf("========================================================================================================\n");
        printf(ANSI_RESET);

        printf(ANSI_YELLOW ANSI_BOLD " [ACADEMIC LAB SIMULATION ONLY] ");
        printf(ANSI_RESET ANSI_DIM "Not a medical device. Demonstrates OS IPC, Memory & Synchronization.\n" ANSI_RESET);

        printf("--------------------------------------------------------------------------------------------------------\n");
        printf(ANSI_BOLD " [SYSTEM RUNTIME] " ANSI_RESET "Elapsed: %02d:%02d | Updates: %-6lu | Alerts Logged: %-4lu | Master PID: %-5d\n",
               elapsed_sec / 60, elapsed_sec % 60,
               (unsigned long)total_updates,
               (unsigned long)total_alerts,
               shm->master_pid);
        printf(ANSI_BOLD " [PROCESS ROSTER] " ANSI_RESET "Sensor PIDs: [%d, %d, %d] | Alert Detector PID: %-5d | FIFO PID: %-5d\n",
               shm->sensor_pids[0], shm->sensor_pids[1], shm->sensor_pids[2],
               shm->alert_detector_pid, shm->fifo_listener_pid);
        printf("--------------------------------------------------------------------------------------------------------\n\n");

        /* Patient Table Header */
        printf(ANSI_BOLD "%-6s | %-24s | %-12s | %-10s | %-14s | %-8s | %-6s | %-19s\n",
               "Bed/ID", "Patient Profile", "Heart Rate", "SpO2 (%)", "Health Status", "Seq #", "Sensor", "Timestamp");
        printf("--------------------------------------------------------------------------------------------------------\n" ANSI_RESET);

        for (int i = 0; i < MAX_PATIENTS; i++) {
            PatientRecord *p = &snapshots[i];

            /* Color-code Heart Rate */
            char hr_str[32];
            if (p->heart_rate < WARNING_HR_MIN || p->heart_rate > WARNING_HR_MAX) {
                snprintf(hr_str, sizeof(hr_str), ANSI_RED ANSI_BOLD "%5.1f BPM" ANSI_RESET, p->heart_rate);
            } else if (p->heart_rate < NORMAL_HR_MIN || p->heart_rate > NORMAL_HR_MAX) {
                snprintf(hr_str, sizeof(hr_str), ANSI_YELLOW "%5.1f BPM" ANSI_RESET, p->heart_rate);
            } else {
                snprintf(hr_str, sizeof(hr_str), ANSI_GREEN "%5.1f BPM" ANSI_RESET, p->heart_rate);
            }

            /* Color-code SpO2 */
            char spo2_str[32];
            if (p->spo2 < WARNING_SPO2_MIN) {
                snprintf(spo2_str, sizeof(spo2_str), ANSI_RED ANSI_BOLD "%5.1f %%" ANSI_RESET, p->spo2);
            } else if (p->spo2 < NORMAL_SPO2_MIN) {
                snprintf(spo2_str, sizeof(spo2_str), ANSI_YELLOW "%5.1f %%" ANSI_RESET, p->spo2);
            } else {
                snprintf(spo2_str, sizeof(spo2_str), ANSI_GREEN "%5.1f %%" ANSI_RESET, p->spo2);
            }

            printf("%-6d | %-24s | %-21s | %-19s | %-23s | %-8lu | %-6d | %-19s\n",
                   p->patient_id,
                   p->name,
                   hr_str,
                   spo2_str,
                   status_to_color_badge(p->status),
                   (unsigned long)p->sequence_number,
                   p->sensor_pid,
                   p->timestamp);
        }

        printf("--------------------------------------------------------------------------------------------------------\n\n");

        /* Recent Alerts Box */
        printf(ANSI_BOLD ANSI_MAGENTA " [RECENT ALERT AUDIT TRAIL] " ANSI_RESET ANSI_DIM "(Persistent in %s)\n" ANSI_RESET, ALERT_LOG_FILE);
        char alerts_preview[1024];
        get_recent_alerts_preview(alerts_preview, sizeof(alerts_preview), 3);
        printf("%s", alerts_preview);

        printf("--------------------------------------------------------------------------------------------------------\n");
        printf(ANSI_BOLD " [CONTROL CHANNELS] " ANSI_RESET "\n");
        printf("  - Named FIFO:  echo \"inject <id> <hr> <spo2>\" > %s\n", FIFO_NAME);
        printf("                 (e.g., echo \"inject 101 42 88\" > %s)\n", FIFO_NAME);
        printf("  - Signals:     Press " ANSI_BOLD ANSI_CYAN "Ctrl+C" ANSI_RESET " to trigger graceful shutdown.\n");
        printf("========================================================================================================\n");

        fflush(stdout);
        frame_count++;
        usleep(600000); /* Refresh at ~1.6 Hz (every 600ms) */
    }

    shm_detach(shm);
}
