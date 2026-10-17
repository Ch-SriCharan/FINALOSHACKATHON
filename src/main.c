/**
 * ============================================================================
 * Project: Multi-Patient Monitoring Simulator Using Shared Memory & Synchronization
 * File:    src/main.c
 * Purpose: Master orchestrator process managing lifecycle, forks, signals,
 *          shared memory setup, and teardown (CO-2, CO-3, CO-4).
 * ============================================================================
 */

#include "common.h"
#include "sync_helpers.h"

static volatile sig_atomic_t g_master_shutdown_requested = 0;
static SharedMemory *g_shm = NULL;

static void master_signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_master_shutdown_requested = 1;
        if (g_shm) {
            g_shm->simulation_active = 0;
        }
    }
}

static void print_usage(const char *prog_name) {
    printf("Usage: %s [OPTIONS]\n\n", prog_name);
    printf("Options:\n");
    printf("  -d, --demo-duration <sec>   Run simulation for specified seconds then terminate automatically\n");
    printf("  -r, --review-alerts         Review saved alert log history (alerts.log) and exit\n");
    printf("  -h, --help                  Display this comprehensive help message\n\n");
    printf("Examples:\n");
    printf("  %s                          Run interactive live simulation\n", prog_name);
    printf("  %s --demo-duration 10       Run automated 10-second laboratory test\n", prog_name);
    printf("  %s --review-alerts          Audit past abnormal alert events\n", prog_name);
}

int main(int argc, char *argv[]) {
    int demo_duration = 0;

    /* Parse Command-Line Arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--review-alerts") == 0) {
            review_alerts_log();
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--demo-duration") == 0) {
            if (i + 1 < argc) {
                demo_duration = atoi(argv[++i]);
                if (demo_duration <= 0) {
                    fprintf(stderr, "Invalid duration: must be a positive integer.\n");
                    return EXIT_FAILURE;
                }
            } else {
                fprintf(stderr, "Option %s requires an integer duration argument.\n", argv[i]);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    print_disclaimer();
    printf("\n[Master PID %d] Initializing POSIX Shared Memory segment (%s)...\n", getpid(), SHM_NAME);

    /* ------------------------------------------------------------------------
     * 1. Initialize POSIX Shared Memory & Primitives (CO-3, CO-4)
     * ------------------------------------------------------------------------ */
    g_shm = shm_attach(true);
    if (!g_shm) {
        fprintf(stderr, "[Master] Fatal error initializing shared memory. Exiting.\n");
        return EXIT_FAILURE;
    }

    /* ------------------------------------------------------------------------
     * 2. Install Master Signal Handlers (CO-2, CO-3)
     * ------------------------------------------------------------------------ */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = master_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("[Master] Creating independent child processes via fork()...\n");
    fflush(stdout);

    /* ------------------------------------------------------------------------
     * 3. Spawn 3 Independent Patient Sensor Processes (CO-2)
     * ------------------------------------------------------------------------ */
    for (int i = 0; i < MAX_PATIENTS; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork(sensor)");
            g_shm->simulation_active = 0;
            break;
        } else if (pid == 0) {
            /* Child process: execute sensor routine */
            run_sensor_process(i);
            _exit(EXIT_SUCCESS);
        } else {
            /* Parent process: record child PID */
            g_shm->sensor_pids[i] = pid;
            printf("  -> Spawned Sensor Process for Patient %d (Bed %d): PID %d\n",
                   g_shm->patients[i].patient_id, i + 1, pid);
            fflush(stdout);
        }
    }

    /* ------------------------------------------------------------------------
     * 4. Spawn Alert Detection Process (CO-2)
     * ------------------------------------------------------------------------ */
    pid_t alert_pid = fork();
    if (alert_pid < 0) {
        perror("fork(alert_detector)");
    } else if (alert_pid == 0) {
        run_alert_detector_process();
        _exit(EXIT_SUCCESS);
    } else {
        g_shm->alert_detector_pid = alert_pid;
        printf("  -> Spawned Alert Detection & File Logging Process: PID %d\n", alert_pid);
        fflush(stdout);
    }

    /* ------------------------------------------------------------------------
     * 5. Spawn Named FIFO Listener Process (CO-2, CO-3)
     * ------------------------------------------------------------------------ */
    pid_t fifo_pid = fork();
    if (fifo_pid < 0) {
        perror("fork(fifo_listener)");
    } else if (fifo_pid == 0) {
        run_fifo_listener_process();
        _exit(EXIT_SUCCESS);
    } else {
        g_shm->fifo_listener_pid = fifo_pid;
        printf("  -> Spawned Named FIFO Control Channel Process: PID %d\n", fifo_pid);
        fflush(stdout);
    }

    printf("\n[Master] All processes initialized. Launching live dashboard...\n");
    sleep(1); /* Brief pause so children have established baseline */

    /* ------------------------------------------------------------------------
     * 6. Run Live Terminal Dashboard
     * ------------------------------------------------------------------------ */
    run_monitor_dashboard(demo_duration);

    /* ------------------------------------------------------------------------
     * 7. Graceful Teardown & Process Lifecycle Management (CO-2, CO-3, CO-4)
     * ------------------------------------------------------------------------ */
    printf("\n" ANSI_BOLD ANSI_YELLOW "========================================================================================\n");
    printf("                  INITIATING GRACEFUL MULTI-PROCESS SHUTDOWN & CLEANUP                  \n");
    printf("========================================================================================" ANSI_RESET "\n");

    /* Broadcast termination signal to all child processes */
    for (int i = 0; i < MAX_PATIENTS; i++) {
        if (g_shm->sensor_pids[i] > 0) {
            printf("[Master] Sending SIGTERM to Sensor PID %d...\n", g_shm->sensor_pids[i]);
            kill(g_shm->sensor_pids[i], SIGTERM);
        }
    }

    if (g_shm->alert_detector_pid > 0) {
        printf("[Master] Sending SIGTERM to Alert Detector PID %d...\n", g_shm->alert_detector_pid);
        kill(g_shm->alert_detector_pid, SIGTERM);
    }

    if (g_shm->fifo_listener_pid > 0) {
        printf("[Master] Sending SIGTERM to FIFO Listener PID %d...\n", g_shm->fifo_listener_pid);
        kill(g_shm->fifo_listener_pid, SIGTERM);
    }

    /* Wait for child processes to exit cleanly using waitpid() to eliminate zombies (CO-2) */
    printf("[Master] Reaping all child processes with waitpid()...\n");
    int status;
    pid_t reaped_pid;
    while ((reaped_pid = waitpid(-1, &status, 0)) > 0) {
        if (WIFEXITED(status)) {
            printf("  -> Child PID %d exited normally with status %d\n", reaped_pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf("  -> Child PID %d terminated by signal %d\n", reaped_pid, WTERMSIG(status));
        }
    }

    /* Summary Metrics */
    printf("\n" ANSI_BOLD ANSI_GREEN "========================================================================================\n");
    printf("                                SIMULATION SESSION SUMMARY                              \n");
    printf("========================================================================================" ANSI_RESET "\n");
    printf("  Total Sensor Updates Recorded:  %lu\n", (unsigned long)g_shm->total_sensor_updates);
    printf("  Total Abnormal Alerts Logged:   %lu\n", (unsigned long)g_shm->total_alerts_triggered);
    printf("  Alert History File:             %s\n", ALERT_LOG_FILE);

    /* Release Shared Memory and Synchronization Primitives (CO-3, CO-4, CO-6) */
    printf("\n[Master] Detaching memory mapping (munmap)...\n");
    shm_detach(g_shm);
    g_shm = NULL;

    printf("[Master] Destroying process-shared mutexes & unlinking /dev/shm%s...\n", SHM_NAME);
    shm_destroy();

    printf("[Master] Unlinking Named FIFO (%s)...\n", FIFO_NAME);
    unlink(FIFO_NAME);

    printf(ANSI_BOLD ANSI_GREEN "[Master] All OS resources successfully reclaimed. Clean exit.\n\n" ANSI_RESET);
    return EXIT_SUCCESS;
}
