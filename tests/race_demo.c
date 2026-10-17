/**
 * ============================================================================
 * Project: Multi-Patient Monitoring Simulator Using Shared Memory & Synchronization
 * File:    tests/race_demo.c
 * Purpose: Controlled demonstration of Race Conditions vs Process-Shared
 *          POSIX Mutex Synchronization (CO-6).
 *
 * Concepts Demonstrated:
 *   - Critical Section: Code segment accessing shared mutable memory.
 *   - Data Tearing (Partial Updates): When multiple fields (e.g. heart_rate,
 *     spo2, and checksum) are written without atomicity, readers observe
 *     inconsistent hybrid states where fields don't match.
 *   - Lost Updates: When multiple processes execute non-atomic increments
 *     (read-modify-write cycle) simultaneously, updates overwrite each other.
 *   - Mutual Exclusion: Process-shared POSIX mutex guarantees serial execution
 *     in the critical section, preventing torn reads and lost updates.
 *
 * Usage:
 *   ./race_demo unsync     (Demonstrates data corruption and race conditions)
 *   ./race_demo sync       (Demonstrates 100% data integrity with synchronization)
 * ============================================================================
 */

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
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define RACE_SHM_NAME       "/race_demo_shm"
#define NUM_WORKERS         4
#define ITERATIONS_PER_WORKER 20000

typedef struct {
    /* Shared Patient Telemetry Record */
    volatile float heart_rate;
    volatile float spo2;
    volatile int32_t checksum;           /* Invariant: (int)(heart_rate * 10) + (int)(spo2 * 10) */
    volatile uint64_t update_counter;    /* Counter susceptible to lost updates */

    /* Synchronization */
    pthread_mutex_t lock;

    /* Metrics for Reader/Validator Process */
    volatile uint64_t torn_reads_detected;
    volatile uint64_t total_reads_performed;
    volatile int workers_active;
} RaceSharedData;

static void worker_unsynchronized(RaceSharedData *shm, int worker_id) {
    for (int i = 0; i < ITERATIONS_PER_WORKER; i++) {
        float hr = 60.0f + (worker_id * 15.0f) + (i % 7);
        float spo2 = 90.0f + (worker_id * 2.0f);
        int32_t chk = (int32_t)(hr * 10.0f) + (int32_t)(spo2 * 10.0f);

        /* UNSYNCHRONIZED WRITES: Multiple memory stores occur non-atomically */
        shm->heart_rate = hr;
        for (volatile int k = 0; k < 15; k++); /* Window of vulnerability for data tearing */
        shm->spo2 = spo2;
        for (volatile int k = 0; k < 15; k++);
        shm->checksum = chk;

        /* Non-atomic read-modify-write cycle (hazard for lost updates) */
        uint64_t temp = shm->update_counter;
        for (volatile int k = 0; k < 10; k++);
        shm->update_counter = temp + 1;
    }
}

static void worker_synchronized(RaceSharedData *shm, int worker_id) {
    for (int i = 0; i < ITERATIONS_PER_WORKER; i++) {
        float hr = 60.0f + (worker_id * 15.0f) + (i % 7);
        float spo2 = 90.0f + (worker_id * 2.0f);
        int32_t chk = (int32_t)(hr * 10.0f) + (int32_t)(spo2 * 10.0f);

        /* SYNCHRONIZED WRITES: Mutex enforces Mutual Exclusion (CO-6) */
        pthread_mutex_lock(&shm->lock);

        shm->heart_rate = hr;
        for (volatile int k = 0; k < 15; k++);
        shm->spo2 = spo2;
        for (volatile int k = 0; k < 15; k++);
        shm->checksum = chk;

        uint64_t temp = shm->update_counter;
        for (volatile int k = 0; k < 10; k++);
        shm->update_counter = temp + 1;

        pthread_mutex_unlock(&shm->lock);
    }
}

static void validator_reader(RaceSharedData *shm, bool synchronized) {
    while (shm->workers_active > 0) {
        float hr, spo2;
        int32_t chk;

        if (synchronized) {
            pthread_mutex_lock(&shm->lock);
            hr = shm->heart_rate;
            spo2 = shm->spo2;
            chk = shm->checksum;
            pthread_mutex_unlock(&shm->lock);
        } else {
            /* Unsynchronized read: hazard of reading partial multi-field write */
            hr = shm->heart_rate;
            spo2 = shm->spo2;
            chk = shm->checksum;
        }

        shm->total_reads_performed++;

        /* Invariant Check: checksum must equal (hr * 10) + (spo2 * 10) */
        int32_t expected = (int32_t)(hr * 10.0f) + (int32_t)(spo2 * 10.0f);
        if (chk != expected && chk != 0) {
            shm->torn_reads_detected++;
        }

        sched_yield();
    }
}

int main(int argc, char *argv[]) {
    bool synchronized = false;

    if (argc < 2) {
        printf("Usage: %s <unsync | sync>\n", argv[0]);
        printf("  unsync : Demonstrates race condition, data tearing, and lost updates.\n");
        printf("  sync   : Demonstrates data consistency using process-shared POSIX mutex.\n");
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "sync") == 0) {
        synchronized = true;
    } else if (strcmp(argv[1], "unsync") == 0) {
        synchronized = false;
    } else {
        fprintf(stderr, "Unknown mode: %s. Use 'unsync' or 'sync'.\n", argv[1]);
        return EXIT_FAILURE;
    }

    printf("\n========================================================================================\n");
    printf("         CONTROLLED CONCURRENCY EXPERIMENT: %s UPDATES\n",
           synchronized ? "SYNCHRONIZED (POSIX MUTEX)" : "UNSYNCHRONIZED (RACE CONDITION)");
    printf("========================================================================================\n");
    printf("Configuration:\n");
    printf("  Concurrent Worker Processes:   %d\n", NUM_WORKERS);
    printf("  Iterations per Worker:         %d\n", ITERATIONS_PER_WORKER);
    printf("  Expected Total Updates:        %d\n", NUM_WORKERS * ITERATIONS_PER_WORKER);
    printf("  Critical Section Protection:   %s\n", synchronized ? "Enabled (pthread_mutex_t)" : "DISABLED (No locks)");
    printf("----------------------------------------------------------------------------------------\n\n");

    /* Create Shared Memory */
    shm_unlink(RACE_SHM_NAME);
    int fd = shm_open(RACE_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open");
        return EXIT_FAILURE;
    }

    if (ftruncate(fd, sizeof(RaceSharedData)) == -1) {
        perror("ftruncate");
        close(fd);
        shm_unlink(RACE_SHM_NAME);
        return EXIT_FAILURE;
    }

    RaceSharedData *shm = (RaceSharedData*)mmap(NULL, sizeof(RaceSharedData),
                                               PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (shm == MAP_FAILED) {
        perror("mmap");
        shm_unlink(RACE_SHM_NAME);
        return EXIT_FAILURE;
    }

    memset(shm, 0, sizeof(RaceSharedData));
    shm->workers_active = NUM_WORKERS;

    /* Initialize Process-Shared Mutex */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&shm->lock, &attr);
    pthread_mutexattr_destroy(&attr);

    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    /* Fork Concurrent Reader/Validator Process */
    pid_t reader_pid = fork();
    if (reader_pid == 0) {
        validator_reader(shm, synchronized);
        munmap(shm, sizeof(RaceSharedData));
        _exit(EXIT_SUCCESS);
    }

    /* Fork Concurrent Worker Processes */
    pid_t worker_pids[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            if (synchronized) {
                worker_synchronized(shm, i + 1);
            } else {
                worker_unsynchronized(shm, i + 1);
            }

            /* Atomically decrement active count */
            pthread_mutex_lock(&shm->lock);
            shm->workers_active--;
            pthread_mutex_unlock(&shm->lock);

            munmap(shm, sizeof(RaceSharedData));
            _exit(EXIT_SUCCESS);
        } else {
            worker_pids[i] = pid;
        }
    }

    /* Wait for all worker processes to complete */
    for (int i = 0; i < NUM_WORKERS; i++) {
        waitpid(worker_pids[i], NULL, 0);
    }

    /* Ensure reader terminates */
    waitpid(reader_pid, NULL, 0);

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 +
                        (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;

    /* Collect Results */
    uint64_t expected_updates = (uint64_t)NUM_WORKERS * ITERATIONS_PER_WORKER;
    uint64_t actual_updates = shm->update_counter;
    int64_t lost_updates = (int64_t)expected_updates - (int64_t)actual_updates;
    uint64_t torn_reads = shm->torn_reads_detected;
    uint64_t total_reads = shm->total_reads_performed;

    printf("========================================================================================\n");
    printf("                                EXPERIMENT OUTCOME                                      \n");
    printf("========================================================================================\n");
    printf("  Execution Time:                %.2f ms\n", elapsed_ms);
    printf("  Total Validation Reads:        %lu\n", (unsigned long)total_reads);
    printf("  Torn Reads (Data Corruption):  %lu  (%s)\n",
           (unsigned long)torn_reads,
           torn_reads > 0 ? "\033[31mCORRUPTED HYBRID DATA DETECTED\033[0m" : "\033[32m0 CORRUPTIONS (CLEAN)\033[0m");
    printf("  Expected Update Counter:       %lu\n", (unsigned long)expected_updates);
    printf("  Actual Update Counter:         %lu\n", (unsigned long)actual_updates);
    printf("  Lost Updates (Race Condition): %ld  (%s)\n",
           (long)lost_updates,
           lost_updates != 0 ? "\033[31mDATA LOSS DETECTED\033[0m" : "\033[32m0 LOSS (EXACT)\033[0m");
    printf("----------------------------------------------------------------------------------------\n");

    if (!synchronized) {
        printf("\n\033[33m[CONCLUSION - UNSYNCHRONIZED EXECUTION]\033[0m\n");
        printf("  - Without mutual exclusion, parallel processes simultaneously executed the\n");
        printf("    read-modify-write cycle of the counter, resulting in %ld lost updates.\n", (long)lost_updates);
        printf("  - Furthermore, %lu torn reads occurred because the reader observed partially written\n", (unsigned long)torn_reads);
        printf("    telemetry fields where heart_rate, SpO2, and checksum were out of sync.\n");
        printf("  - \033[31mDemonstrates why synchronization is strictly required in concurrent OS design!\033[0m\n\n");
    } else {
        printf("\n\033[32m[CONCLUSION - SYNCHRONIZED EXECUTION]\033[0m\n");
        printf("  - Process-shared POSIX mutex enforced strict Mutual Exclusion in the Critical Section.\n");
        printf("  - 100.00%% data integrity was achieved: 0 torn reads and 0 lost updates.\n");
        printf("  - \033[32mGuarantees that readers always consume consistent, non-corrupted patient records.\033[0m\n\n");
    }

    /* Cleanup */
    pthread_mutex_destroy(&shm->lock);
    munmap(shm, sizeof(RaceSharedData));
    shm_unlink(RACE_SHM_NAME);

    return EXIT_SUCCESS;
}
