/*
 * $ gcc -o futex_ratio futex_ratio.c -pthread -lm
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <time.h>
#include <stdatomic.h>

// --- System call wrapper ---
static long futex(atomic_int *uaddr, int futex_op, int val, const struct timespec *timeout, atomic_int *uaddr2, int val3) {
	return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

// --- Global variables and data structure ---
#define NUM_THREADS_EACH 4 // Number of threads for each type
#define TARGET_CPU 0       // CPU core to run the test
#define WORKLOAD_ITER 500  // Workload per task

typedef struct {
	atomic_int futex_semaphore; // Futex waiter threads' semaphore (0 or 1)
	unsigned long futex_counts[NUM_THREADS_EACH];
	unsigned long timer_counts[NUM_THREADS_EACH];
	atomic_int terminate;
} shared_data_t;

shared_data_t *shared_mem;

// --- Thread functions ---

/**
 * Futex waiter thread (boosting candidate)
 * Waits for the semaphore to become available using futex
 */
void *futex_task(void *arg) {
	long id = (long)arg;
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(TARGET_CPU, &cpuset);
	pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

	while (!atomic_load(&shared_mem->terminate)) {
		// Try to acquire the semaphore (1 -> 0)
		while (1) {
			int one = 1;
			if (atomic_compare_exchange_strong(&shared_mem->futex_semaphore, &one, 0)) {
				break; // Acquired
			}
			// Semaphore is 0, so wait
			futex(&shared_mem->futex_semaphore, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
			if (atomic_load(&shared_mem->terminate)) break;
		}
		if (atomic_load(&shared_mem->terminate)) break;

		// --- Common workload ---
		for (volatile int i = 0; i < WORKLOAD_ITER; i++);
		shared_mem->futex_counts[id]++;

		// Release the semaphore (0 -> 1) and wake up a waiter
		atomic_store(&shared_mem->futex_semaphore, 1);
		futex(&shared_mem->futex_semaphore, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
	}
	return NULL;
}

/**
 * Timer sleep thread (non-boosting candidate)
 * Sleeps using nanosleep
 */
void *timer_task(void *arg) {
	long id = (long)arg;
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(TARGET_CPU, &cpuset);
	pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

	// Very short sleep time (1 microsecond)
	struct timespec sleep_time = { .tv_sec = 0, .tv_nsec = 1000 };

	while (!atomic_load(&shared_mem->terminate)) {
		// Sleep voluntarily
		nanosleep(&sleep_time, NULL);

		// --- Common workload ---
		for (volatile int i = 0; i < WORKLOAD_ITER; i++);
		shared_mem->timer_counts[id]++;
	}
	return NULL;
}

// --- Signal handler and main function ---

void handle_sigint(int sig) {
	if (atomic_load(&shared_mem->terminate)) return;
	atomic_store(&shared_mem->terminate, 1);
	atomic_store(&shared_mem->futex_semaphore, 1);
	futex(&shared_mem->futex_semaphore, FUTEX_WAKE_PRIVATE, NUM_THREADS_EACH, NULL, NULL, 0);
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "Usage: %s <test duration (seconds)>\n", argv[0]);
		return 1;
	}
	int test_duration_s = atoi(argv[1]);

	shared_mem = (shared_data_t *)malloc(sizeof(shared_data_t));
	memset(shared_mem, 0, sizeof(shared_data_t));
	atomic_init(&shared_mem->futex_semaphore, 1); // Initially available
	atomic_init(&shared_mem->terminate, 0);

	FILE *fp = fopen("/proc/sys/kernel/sched_futex_boost", "r");
	int current_boost = 0;
	if (fp) {
		fscanf(fp, "%d", &current_boost);
		fclose(fp);
	}

/*	printf("--- Time-sharing Test Setup ---\n");
	printf("Futex threads: %d, Timer threads: %d, CPU: %d, Duration: %ds\n",
		   NUM_THREADS_EACH, NUM_THREADS_EACH, TARGET_CPU, test_duration_s);
	printf("Current kernel.sched_futex_boost = %d\n", current_boost);
	printf("----------------------------------\nTest start...\n"); //*/

	signal(SIGINT, handle_sigint);

	pthread_t threads[NUM_THREADS_EACH * 2];
	for (long i = 0; i < NUM_THREADS_EACH; i++) {
		pthread_create(&threads[i], NULL, futex_task, (void *)i);
	}
	for (long i = 0; i < NUM_THREADS_EACH; i++) {
		pthread_create(&threads[NUM_THREADS_EACH + i], NULL, timer_task, (void *)i);
	}

	sleep(test_duration_s);
	handle_sigint(0);
/*    printf("Test duration finished. Waiting for threads to terminate...\n"); //*/

	for (int i = 0; i < NUM_THREADS_EACH * 2; i++) {
		pthread_join(threads[i], NULL);
	}

	unsigned long total_futex_count = 0;
	for (int i = 0; i < NUM_THREADS_EACH; i++) total_futex_count += shared_mem->futex_counts[i];
	unsigned long total_timer_count = 0;
	for (int i = 0; i < NUM_THREADS_EACH; i++) total_timer_count += shared_mem->timer_counts[i];
	double ratio = (total_timer_count > 0) ? (double)total_futex_count / total_timer_count : 0;

/*	printf("\n--- Test Results ---\n");
	printf("Total Futex waiter iterations: %lu (Average: %.2f)\n", total_futex_count, (double)total_futex_count / NUM_THREADS_EACH);
	printf("Total Timer sleep iterations:  %lu (Average: %.2f)\n", total_timer_count, (double)total_timer_count / NUM_THREADS_EACH); //*/
	printf("Ratio (Futex/Timer):           %.4f\n", ratio);
/*	printf("--------------------\n"); //*/

	free(shared_mem);
	return 0;
}
