/*

  $ ./pseudocc 150000000

  (partial output) $ ps -e --forest
       7560 pts/0    00:00:00  |   |   \_ bash
     797374 pts/0    00:00:00  |   |   |   \_ pseudocc
     797417 pts/0    00:00:02  |   |   |       \_ pseudocc
     797423 pts/0    00:00:02  |   |   |       \_ pseudocc
     797430 pts/0    00:00:01  |   |   |       \_ pseudocc
     797433 pts/0    00:00:01  |   |   |       \_ pseudocc

     kernel.sched_burst_fork_atavistic = 0: OK
     kernel.sched_burst_fork_atavistic = 1: OK

  $ ./pseudocc -l2 150000000

  (partial output) $ ps -e --forest
       7560 pts/0    00:00:00  |   |   \_ bash
     795785 pts/0    00:00:00  |   |   |   \_ pseudocc
     796909 pts/0    00:00:00  |   |   |       \_ pseudocc
     796910 pts/0    00:00:01  |   |   |       |   \_ pseudocc
     796913 pts/0    00:00:00  |   |   |       \_ pseudocc
     796914 pts/0    00:00:01  |   |   |       |   \_ pseudocc
     796919 pts/0    00:00:00  |   |   |       \_ pseudocc
     796921 pts/0    00:00:01  |   |   |       |   \_ pseudocc
     796920 pts/0    00:00:00  |   |   |       \_ pseudocc
     796922 pts/0    00:00:01  |   |   |           \_ pseudocc
 
     kernel.sched_burst_fork_atavistic = 0: NG, system may suffer experience slowdown
     kernel.sched_burst_fork_atavistic = 1: OK

  2023-07-17 BORE v2.5.1:
     sched_burst_fork_atavistic now means how many hub (child process count >= 2) nodes
     update_child_burst_cache digs down recursively for each direct child.

  2023-07-16 BORE v2.5.0:
     Fix: Newly-forked child processes might inherit from wrong parents.
     Added atavistic inheritance feature.

  2023-05-17 BORE v2.2.7:
     Newly-forked child process' child_burst_cache and child_burst_last_cached are reset to 0.

  2023-05-12 BORE v2.2.4:
     Newly-forked child process' burst_time is reset to 0.

  2023-04-20 BORE v2.2.1:
     Child processes' average burst time is now held in the task's child_burst_cache variable,
     and is cached for sched_burst_cache_lifetime duration to reduce frequency of calculation.

  2023-03-18 BORE v1.7.12:
     Added 'max_burst_time' variable to hold the greather of burst_time and prev_burst_time
     so that these values wouldn't have to be compared everytime in fork().

  2022-12-10 BORE v1.7.4:
     Fix: Uninitialized local variables in sched_on_fork_calc_prev_burst_from_siblings
     (reported by Raymond K. Zhao)

  2022-03-27 BORE v1.6.34.0:
     On fork, parent calculates the average value of existing child processes' burst times,
     and copies it to the newly-forked process' burst_time.

  2022-03-27 BORE v1.2.28.0:
     First officially-released version with the earliest burst inheritance feature included.
     Parent tasks copies its own current burst_time to their children.

  2022-02-07 BORE Beta 25 'inherit' experimental branch:
     Beginning of the research of the burst inheritance feature.
     Parent tasks copy its own current burst_time and burst_history to their forked children.
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <getopt.h>
#include <stdint.h>
#include <time.h>

// Function to display usage information
void display_usage() {
  printf("pseudocc v1.0\n");
  printf("BORE (Burst-Oriented Response Enhancer) Burst time inheritance test\n");
  printf("Usage: ./pseudocc [-j <parallelism>] [-l <level>] <cycles>\n");
}

// Get current time in nanoseconds
long long get_nanoseconds() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Function to display elapsed time in appropriate units
void display_elapsed_time(long long elapsed_time) {
  if (elapsed_time >= 1000000) {
    printf("%lld ms.\n", elapsed_time / 1000000);
  } else if (elapsed_time >= 1000) {
    printf("%lld us.\n", elapsed_time / 1000);
  } else {
    printf("%lld ns.\n", elapsed_time);
  }
}

// Function to create child processes recursively and run the loop
void run_loop_in_child_processes(int level, uint64_t num);

// Function to create a child process and run the given function
void create_child_process(int level, uint64_t num) {
  pid_t pid = fork();

  if (pid < 0) {
    perror("fork failed");
    exit(1);
  } else if (pid == 0) {
    // Child process
    run_loop_in_child_processes(level - 1, num);
    exit(0);
  }
}

// Function to create child processes recursively and run the loop
void run_loop_in_child_processes(int level, uint64_t num) {
  if (level <= 0) {
    // Generate a random number between 1 and 9
    srand(getpid()); // Seed random number generator with process ID
    uint64_t rand_num = (uint64_t)(rand() % 9) + 1;
    uint64_t result = num * rand_num;

    printf("Child process (PID: %d) will loop %lu times.\n",
           getpid(), result);

    volatile uint64_t j; // Prevent compile-time loop optimization

    long long start_time = get_nanoseconds(); // Start recording time

    for (j = 0; j < result; j++) {
      // Empty loop
    }

    long long end_time = get_nanoseconds(); // End recording time
    long long elapsed_time = end_time - start_time;

    printf("Child process (PID: %d) finished in ", getpid());

    display_elapsed_time(elapsed_time); // Display elapsed time in appropriate units

    exit(0);
  } else {
    create_child_process(level, num); // Create a child process
    wait(NULL); // Wait for child process to finish
  }
}

// Function to maintain parallelism
void maintain_parallelism(int level, uint64_t num, int parallelism) {
  int running_processes = 0;

  while (1) {
    for (int i = 0; i < parallelism - running_processes; i++) {
      create_child_process(level, num); // Create a child process
      running_processes++;
    }

    // Wait for any child process to finish
    wait(NULL);
    running_processes--;
  }
}

int main(int argc, char *argv[]) {
  int cores = sysconf(_SC_NPROCESSORS_ONLN);
  int parallelism = cores;
  int level = 1;

  // Process the -j and -l command line options
  int option;
  while ((option = getopt(argc, argv, "j:l:")) != -1) {
    switch (option) {
      case 'j':
        parallelism = atoi(optarg);
        break;
      case 'l':
        level = atoi(optarg);
        break;
      default:
        display_usage();
        return 1;
    }
  }

  if (optind >= argc) {
    display_usage();
    return 1;
  }

  uint64_t num = atoll(argv[optind]);
  printf("Number of CPU cores: %d\n", cores);
  printf("Parallelism: %d\n", parallelism);
  printf("Level: %d\n", level);

  maintain_parallelism(level, num, parallelism);

  return 0;
}

