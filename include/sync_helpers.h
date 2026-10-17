/**
 * ============================================================================
 * Project: Multi-Patient Monitoring Simulator Using Shared Memory & Synchronization
 * File:    include/sync_helpers.h
 * Purpose: Process-shared POSIX mutex wrappers, robust error handling, and
 *          synchronization utilities (CO-6).
 *
 * Concepts Demonstrated:
 *   - Process-Shared Mutex: Created with PTHREAD_PROCESS_SHARED to allow
 *     synchronization across independent processes with distinct address spaces.
 *   - Robust Mutex: Configured with PTHREAD_MUTEX_ROBUST to prevent deadlocks
 *     if an updating sensor process terminates abnormally while holding a lock.
 *   - Critical Section Protection: Guarantees atomic reads and writes of patient
 *     records so reader processes never observe torn or partially updated fields.
 * ============================================================================
 */

#ifndef SYNC_HELPERS_H
#define SYNC_HELPERS_H

#include "common.h"

/**
 * Initializes a process-shared, robust POSIX mutex.
 *
 * @param mutex Pointer to pthread_mutex_t located within shared memory.
 * @return 0 on success, -1 on failure.
 */
static inline int init_process_shared_mutex(pthread_mutex_t *mutex) {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        perror("pthread_mutexattr_init");
        return -1;
    }

    /* Enable sharing across processes (CO-6) */
    if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
        perror("pthread_mutexattr_setpshared");
        pthread_mutexattr_destroy(&attr);
        return -1;
    }

    /* Enable robust mutex semantics to handle process crashes without deadlock */
#ifdef PTHREAD_MUTEX_ROBUST
    if (pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) != 0) {
        perror("pthread_mutexattr_setrobust");
        pthread_mutexattr_destroy(&attr);
        return -1;
    }
#endif

    if (pthread_mutex_init(mutex, &attr) != 0) {
        perror("pthread_mutex_init");
        pthread_mutexattr_destroy(&attr);
        return -1;
    }

    pthread_mutexattr_destroy(&attr);
    return 0;
}

/**
 * Locks a process-shared mutex with robust recovery support.
 *
 * If a process dies while holding the lock, pthread_mutex_lock returns EOWNERDEAD.
 * We invoke pthread_mutex_consistent() to restore consistent state and proceed safely.
 *
 * @param mutex Pointer to the mutex in shared memory.
 * @return 0 on success, -1 on fatal error.
 */
static inline int safe_mutex_lock(pthread_mutex_t *mutex) {
    int ret = pthread_mutex_lock(mutex);
    if (ret == 0) {
        return 0;
    }

#ifdef EOWNERDEAD
    if (ret == EOWNERDEAD) {
        /* Previous lock owner died in its critical section. Make it consistent. */
        if (pthread_mutex_consistent(mutex) == 0) {
            fprintf(stderr, "[SYNC WARN] Recovered robust lock from terminated process.\n");
            return 0;
        }
    }
#endif

    fprintf(stderr, "[SYNC ERROR] pthread_mutex_lock failed: %s\n", strerror(ret));
    return -1;
}

/**
 * Unlocks a process-shared mutex.
 *
 * @param mutex Pointer to the mutex in shared memory.
 * @return 0 on success, -1 on failure.
 */
static inline int safe_mutex_unlock(pthread_mutex_t *mutex) {
    int ret = pthread_mutex_unlock(mutex);
    if (ret != 0) {
        fprintf(stderr, "[SYNC ERROR] pthread_mutex_unlock failed: %s\n", strerror(ret));
        return -1;
    }
    return 0;
}

/**
 * Destroys a process-shared mutex.
 *
 * @param mutex Pointer to the mutex in shared memory.
 */
static inline void destroy_process_shared_mutex(pthread_mutex_t *mutex) {
    pthread_mutex_destroy(mutex);
}

#endif /* SYNC_HELPERS_H */
