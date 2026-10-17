/**
 * ============================================================================
 * Project: Multi-Patient Monitoring Simulator Using Shared Memory & Synchronization
 * File:    src/common.c
 * Purpose: Shared memory setup (CO-3, CO-4), vitals status evaluation,
 *          timestamp helpers, and alert log review (CO-5).
 * ============================================================================
 */

#include "common.h"
#include "sync_helpers.h"

/**
 * Formats current wall-clock time into YYYY-MM-DD HH:MM:SS format.
 */
void get_current_timestamp(char *buffer, size_t max_len) {
    if (!buffer || max_len == 0) return;
    
    time_t raw_time = time(NULL);
    struct tm tm_info;
    localtime_r(&raw_time, &tm_info);
    strftime(buffer, max_len, "%Y-%m-%d %H:%M:%S", &tm_info);
}

/**
 * Evaluates physiological vitals against configurable demonstration thresholds.
 * Classifies readings into NORMAL, WARNING, or CRITICAL and fills a human-readable reason.
 */
PatientStatus evaluate_vitals_status(float hr, float spo2, char *reason_buf, size_t max_len) {
    char reason[REASON_LEN] = "Vitals within normal physiological ranges";
    PatientStatus status = STATUS_NORMAL;

    /* Check for Critical Conditions first */
    if (hr < 45.0f || hr > 130.0f || spo2 < 88.0f) {
        status = STATUS_CRITICAL;
        if (hr < 45.0f && spo2 < 88.0f) {
            snprintf(reason, sizeof(reason), "CRITICAL: Severe Bradycardia & Acute Hypoxemia");
        } else if (hr > 130.0f && spo2 < 88.0f) {
            snprintf(reason, sizeof(reason), "CRITICAL: Severe Tachycardia & Acute Hypoxemia");
        } else if (hr < 45.0f) {
            snprintf(reason, sizeof(reason), "CRITICAL: Severe Bradycardia (<45 BPM)");
        } else if (hr > 130.0f) {
            snprintf(reason, sizeof(reason), "CRITICAL: Severe Tachycardia (>130 BPM)");
        } else {
            snprintf(reason, sizeof(reason), "CRITICAL: Acute Hypoxemia (SpO2 <88%%)");
        }
    }
    /* Check for Warning Conditions */
    else if (hr < WARNING_HR_MIN || hr > WARNING_HR_MAX || spo2 < NORMAL_SPO2_MIN) {
        status = STATUS_WARNING;
        if (hr < WARNING_HR_MIN) {
            snprintf(reason, sizeof(reason), "WARNING: Mild Bradycardia (<50 BPM)");
        } else if (hr > WARNING_HR_MAX) {
            snprintf(reason, sizeof(reason), "WARNING: Elevated Heart Rate (>120 BPM)");
        } else if (spo2 < NORMAL_SPO2_MIN) {
            snprintf(reason, sizeof(reason), "WARNING: Mild Desaturation (<95%%)");
        }
    }

    if (reason_buf && max_len > 0) {
        strncpy(reason_buf, reason, max_len - 1);
        reason_buf[max_len - 1] = '\0';
    }

    return status;
}

const char* status_to_string(PatientStatus status) {
    switch (status) {
        case STATUS_NORMAL:   return "NORMAL";
        case STATUS_WARNING:  return "WARNING";
        case STATUS_CRITICAL: return "CRITICAL";
        default:              return "UNKNOWN";
    }
}

const char* status_to_color_badge(PatientStatus status) {
    switch (status) {
        case STATUS_NORMAL:
            return ANSI_GREEN "[ NORMAL   ]" ANSI_RESET;
        case STATUS_WARNING:
            return ANSI_YELLOW "[ WARNING  ]" ANSI_RESET;
        case STATUS_CRITICAL:
            return ANSI_BG_RED ANSI_BOLD ANSI_WHITE "[ CRITICAL ]" ANSI_RESET;
        default:
            return "[ UNKNOWN  ]";
    }
}

void print_disclaimer(void) {
    printf(ANSI_YELLOW ANSI_BOLD);
    printf("========================================================================================\n");
    printf("  [ACADEMIC DISCLAIMER] MULTI-PATIENT MONITORING SIMULATOR (POSIX SHM & SYNCHRONIZATION)\n");
    printf("  This program is an Operating Systems laboratory simulation. It does NOT interface with\n");
    printf("  medical hardware and must NEVER be used for clinical diagnosis or patient treatment.\n");
    printf("========================================================================================\n");
    printf(ANSI_RESET);
}

/**
 * Attaches or creates the POSIX Shared Memory segment.
 *
 * Demonstrates:
 *   - shm_open(): Creates/opens POSIX shared memory object under /dev/shm
 *   - ftruncate(): Sets the capacity of the memory segment
 *   - mmap(): Maps the physical memory frames into the process virtual address space (CO-4)
 */
SharedMemory* shm_attach(bool create) {
    int flags = O_RDWR;
    mode_t mode = 0666;

    if (create) {
        flags |= O_CREAT;
        /* First unlink any stale segment from a previous abnormal termination */
        shm_unlink(SHM_NAME);
    }

    int fd = shm_open(SHM_NAME, flags, mode);
    if (fd == -1) {
        perror("shm_open");
        return NULL;
    }

    if (create) {
        if (ftruncate(fd, sizeof(SharedMemory)) == -1) {
            perror("ftruncate");
            close(fd);
            shm_unlink(SHM_NAME);
            return NULL;
        }
    }

    void *addr = mmap(NULL, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd); /* File descriptor is no longer needed after mmap() */

    if (addr == MAP_FAILED) {
        perror("mmap");
        if (create) shm_unlink(SHM_NAME);
        return NULL;
    }

    SharedMemory *shm = (SharedMemory*)addr;

    if (create) {
        memset(shm, 0, sizeof(SharedMemory));
        shm->simulation_active = 1;
        shm->master_pid = getpid();

        /* Initialize Process-Shared Global Mutex */
        if (init_process_shared_mutex(&shm->global_lock) != 0) {
            fprintf(stderr, "Failed to initialize global mutex\n");
        }

        /* Default Patient Record Initialization */
        const char *default_names[MAX_PATIENTS] = {
            "Alice Walker (Cardio)",
            "Bob Martinez (ICU-1)",
            "Clara Zhao (Post-Op)"
        };
        const float default_hr[MAX_PATIENTS] = { 72.0f, 80.0f, 68.0f };
        const float default_spo2[MAX_PATIENTS] = { 98.5f, 96.0f, 99.0f };

        for (int i = 0; i < MAX_PATIENTS; i++) {
            shm->patients[i].patient_id = 101 + i;
            strncpy(shm->patients[i].name, default_names[i], PATIENT_NAME_LEN - 1);
            shm->patients[i].heart_rate = default_hr[i];
            shm->patients[i].spo2 = default_spo2[i];
            shm->patients[i].status = STATUS_NORMAL;
            shm->patients[i].sequence_number = 0;
            shm->patients[i].sensor_pid = 0;
            get_current_timestamp(shm->patients[i].timestamp, TIMESTAMP_LEN);

            /* Initialize individual process-shared mutex for each patient record (CO-6) */
            if (init_process_shared_mutex(&shm->patients[i].lock) != 0) {
                fprintf(stderr, "Failed to initialize mutex for patient index %d\n", i);
            }
        }
    }

    return shm;
}

/**
 * Detaches the shared memory mapping from the calling process's virtual address space.
 */
void shm_detach(SharedMemory *shm) {
    if (shm && shm != MAP_FAILED) {
        if (munmap((void*)shm, sizeof(SharedMemory)) == -1) {
            perror("munmap");
        }
    }
}

/**
 * Destroys all synchronization primitives and removes the POSIX shared memory object.
 */
void shm_destroy(void) {
    /* Temporary attach to destroy mutexes cleanly */
    int fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (fd != -1) {
        SharedMemory *shm = (SharedMemory*)mmap(NULL, sizeof(SharedMemory),
                                               PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (shm != MAP_FAILED) {
            destroy_process_shared_mutex(&shm->global_lock);
            for (int i = 0; i < MAX_PATIENTS; i++) {
                destroy_process_shared_mutex(&shm->patients[i].lock);
            }
            munmap((void*)shm, sizeof(SharedMemory));
        }
    }

    /* Unlink POSIX shared memory object from /dev/shm (CO-3, CO-4) */
    if (shm_unlink(SHM_NAME) == -1) {
        if (errno != ENOENT) {
            perror("shm_unlink");
        }
    }
}

/**
 * Reviews and displays the contents of the alerts log file (CO-5).
 */
void review_alerts_log(void) {
    print_disclaimer();
    printf("\n" ANSI_BOLD ANSI_CYAN "========================================================================================\n");
    printf("                          POST-SIMULATION AUDIT: ALERT HISTORY LOG                      \n");
    printf("========================================================================================" ANSI_RESET "\n");

    int fd = open(ALERT_LOG_FILE, O_RDONLY);
    if (fd == -1) {
        if (errno == ENOENT) {
            printf(ANSI_YELLOW "No alert log file found (%s). Either no abnormal events occurred, or the simulation has not run yet.\n" ANSI_RESET, ALERT_LOG_FILE);
        } else {
            perror("open(alerts.log)");
        }
        return;
    }

    int line_count = 0;

    printf("%-20s | %-10s | %-6s | %-18s | %-16s | %s\n",
           "Timestamp", "Level", "ID", "Vitals (HR/SpO2)", "Sequence #", "Physiological Diagnosis");
    printf("--------------------------------------------------------------------------------------------------------------------\n");

    FILE *fp = fdopen(fd, "r");
    if (!fp) {
        perror("fdopen");
        close(fd);
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        line_count++;
        /* Strip trailing newline */
        line[strcspn(line, "\r\n")] = 0;
        
        /* Highlight based on severity */
        if (strstr(line, "CRITICAL")) {
            printf(ANSI_RED "%s" ANSI_RESET "\n", line);
        } else if (strstr(line, "WARNING")) {
            printf(ANSI_YELLOW "%s" ANSI_RESET "\n", line);
        } else if (strstr(line, "RESOLVED")) {
            printf(ANSI_GREEN "%s" ANSI_RESET "\n", line);
        } else {
            printf("%s\n", line);
        }
    }

    fclose(fp);
    printf("--------------------------------------------------------------------------------------------------------------------\n");
    printf(ANSI_BOLD "Total Alert Entries Reviewed: %d\n" ANSI_RESET, line_count);
    printf("Log File Location: %s\n\n", ALERT_LOG_FILE);
}
