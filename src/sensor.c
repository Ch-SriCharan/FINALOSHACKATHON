/**
 * ============================================================================
 * Project: Multi-Patient Monitoring Simulator Using Shared Memory & Synchronization
 * File:    src/sensor.c
 * Purpose: Independent sensor process simulation (CO-2, CO-3, CO-6).
 *
 * Concepts Demonstrated:
 *   - Process Autonomy: Each sensor process runs independently with its own
 *     scheduling slice, state, and pseudo-random seed.
 *   - Mutual Exclusion (CO-6): Critical section protected by a per-patient
 *     process-shared POSIX mutex prevents partial/torn writes.
 *   - Atomic Updates: Readers never see an updated heart rate paired with a
 *     stale SpO2 or mismatched sequence number.
 *   - Signal Handling (CO-2, CO-3): Responds to SIGUSR1 by forcing an acute
 *     vital anomaly, and gracefully terminates on SIGINT / SIGTERM.
 * ============================================================================
 */

#include "common.h"
#include "sync_helpers.h"

static volatile sig_atomic_t g_sensor_running = 1;
static volatile sig_atomic_t g_force_anomaly = 0;

static void sensor_signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_sensor_running = 0;
    } else if (sig == SIGUSR1) {
        g_force_anomaly = 1;
    }
}

/**
 * Runs an independent sensor process generating simulated telemetry for one patient.
 *
 * @param patient_index Array index (0, 1, or 2) corresponding to Patient 101, 102, or 103.
 */
void run_sensor_process(int patient_index) {
    /* Setup dedicated signal handlers for child process */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sensor_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);

    /* Attach to existing POSIX shared memory mapped by master (CO-4) */
    SharedMemory *shm = shm_attach(false);
    if (!shm) {
        fprintf(stderr, "[Sensor PID %d] Failed to attach to shared memory\n", getpid());
        exit(EXIT_FAILURE);
    }

    /* Seed random generator uniquely per sensor process using PID and time */
    unsigned int seed = (unsigned int)(time(NULL) ^ (getpid() << 8) ^ (patient_index * 1337));

    /* Baseline physiological parameters per patient profile */
    float base_hr;
    float base_spo2;
    int anomaly_period;

    switch (patient_index) {
        case 0: /* Patient 101: Baseline healthy cardio recovery */
            base_hr = 72.0f;
            base_spo2 = 98.5f;
            anomaly_period = 14; /* Periodic anomaly every ~14 cycles */
            break;
        case 1: /* Patient 102: Baseline ICU post-operative */
            base_hr = 84.0f;
            base_spo2 = 95.8f;
            anomaly_period = 11; /* Periodic anomaly every ~11 cycles */
            break;
        case 2: /* Patient 103: Baseline geriatric observation */
            base_hr = 66.0f;
            base_spo2 = 97.0f;
            anomaly_period = 17; /* Periodic anomaly every ~17 cycles */
            break;
        default:
            base_hr = 75.0f;
            base_spo2 = 97.0f;
            anomaly_period = 15;
            break;
    }

    float current_hr = base_hr;
    float current_spo2 = base_spo2;
    uint32_t cycle_count = 0;

    /* Main Sensor Acquisition Loop */
    while (g_sensor_running && shm->simulation_active) {
        cycle_count++;

        /* 1. Simulate physiological vital readings */
        if (g_force_anomaly) {
            /* External emergency injection trigger (e.g. from FIFO command or SIGUSR1) */
            g_force_anomaly = 0;
            if (patient_index == 0) {
                current_hr = 42.0f;   /* Critical Bradycardia */
                current_spo2 = 87.0f; /* Acute Hypoxemia */
            } else if (patient_index == 1) {
                current_hr = 142.0f;  /* Critical Tachycardia */
                current_spo2 = 89.0f; /* Acute Hypoxemia */
            } else {
                current_hr = 48.0f;   /* Severe Bradycardia */
                current_spo2 = 85.0f; /* Severe Desaturation */
            }
        } else if (cycle_count % anomaly_period == 0) {
            /* Natural simulated intermittent anomaly for demonstration */
            if (patient_index == 0) {
                current_hr = 44.0f + (rand_r(&seed) % 4);       /* Transient Bradycardia */
                current_spo2 = 91.0f - (rand_r(&seed) % 3);     /* Mild Hypoxia */
            } else if (patient_index == 1) {
                current_hr = 132.0f + (rand_r(&seed) % 10);     /* Transient Tachycardia */
                current_spo2 = 93.0f - (rand_r(&seed) % 3);     /* Warning SpO2 */
            } else {
                current_hr = 52.0f - (rand_r(&seed) % 5);       /* Mild Bradycardia */
                current_spo2 = 86.5f + (rand_r(&seed) % 4);     /* Acute Desaturation */
            }
        } else {
            /* Normal physiological random walk around baseline */
            float hr_delta = ((float)(rand_r(&seed) % 21) - 10.0f) * 0.15f;    /* -1.5 to +1.5 bpm */
            float spo2_delta = ((float)(rand_r(&seed) % 11) - 5.0f) * 0.08f;   /* -0.4 to +0.4 % */

            current_hr += hr_delta;
            current_spo2 += spo2_delta;

            /* Restore gently towards baseline */
            current_hr = current_hr * 0.85f + base_hr * 0.15f;
            current_spo2 = current_spo2 * 0.85f + base_spo2 * 0.15f;

            if (current_spo2 > 100.0f) current_spo2 = 100.0f;
        }

        /* Evaluate classification */
        char reason[REASON_LEN];
        PatientStatus status = evaluate_vitals_status(current_hr, current_spo2, reason, sizeof(reason));

        /* --------------------------------------------------------------------
         * CRITICAL SECTION (CO-6): Synchronized Shared Memory Write
         * -------------------------------------------------------------------- */
        if (safe_mutex_lock(&shm->patients[patient_index].lock) == 0) {
            PatientRecord *rec = &shm->patients[patient_index];

            rec->heart_rate = current_hr;
            rec->spo2 = current_spo2;
            rec->status = status;
            rec->sequence_number++;
            rec->sensor_pid = getpid();
            get_current_timestamp(rec->timestamp, TIMESTAMP_LEN);

            safe_mutex_unlock(&rec->lock);
        }

        /* Update global aggregate counter with quick lock */
        if (safe_mutex_lock(&shm->global_lock) == 0) {
            shm->total_sensor_updates++;
            safe_mutex_unlock(&shm->global_lock);
        }

        /* Asynchronous sleep with jitter (800ms - 1200ms) to simulate hardware variance */
        useconds_t jitter_usec = 800000 + (rand_r(&seed) % 400000);
        usleep(jitter_usec);
    }

    shm_detach(shm);
    exit(EXIT_SUCCESS);
}
