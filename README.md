# BORE (Burst-Oriented Response Enhancer) CPU Scheduler

BORE (Burst-Oriented Response Enhancer) is a modification to the Completely Fair Scheduler, the Linux default CPU scheduler.

It is an attempt to improve CFS' responsiveness for various desktop applications by re-defining scheduling fairness as "fair for same burstiness", and discriminating tasks of different interactiveness by using its unique scoring method based on tasks' "burstiness".

## How it works

![alt burst units vs vruntime growth rate (logarithmic)](https://raw.githubusercontent.com/firelzrd/bore-scheduler/main/burst%20units%20vs%20vruntime%20growth%20rate.png)

* It keeps track of each task's burst time. A burst time is basically how much CPU time in real time scale the task has consumed since last time yielding/sleeping/iowaiting.
* While a task is active, its burst score is continuously calculated by basically counting task's normalized burst time's bit count, with adjustment using pre-configured offset and coefficient.
* Then the burst score is used in the similar way as "niceness" works. It takes a value between 0-39, and as higher by 1, task can consume longer timeslice by approximately 1.25x.
* These steps combined works as a radix conversion from binary logarithm to common logarithm, converting between two different magnitude (nano-seconds-to-minutes timescale to about 0.01-100x scale) dimentionlessly.
* Burst scores are also used to affect how aggressively upwaking tasks can preempt the currently-running task.
* As a result, less "greedy" tasks are given more timeslice and wakeup preemption aggressiveness, while greedier tasks (that yields its timeslice in longer period of time) are weighted less.
* Newly-spawned process' burst score is calculated in a unique way to prevent the type of tasks like "make" from overwhelming interactive tasks by forking many CPU-hungry children.
* The final effect is that there is some kind of equilibrium between opposing greedy&weak tasks (=usually CPU-bound, batch tasks) and modest&strong tasks (=usually I/O-bound, interactive tasks), giving more responsive user experience under coexistence of various types of workloads.

## Special thanks

* Hamad Al Marri, the developer famous for his task schedulers Cachy, CacULE, Baby and TT. BORE has been massively inspired from his great works. He also helped me a lot in the development.
* Peter "ptr1337" Jung, the founder of CachyOS high-performance linux distribution, also being the admin of its development community. His continuous support, sharp analysis and dedicated tests and advice helped me shoot many problems.
* Ching-Chun Huang from National Cheng Kung University of Taiwan, and Hui Chun "foxhoundsk" Feng from National Taiwan Ocean University, for detailed analysis and explanation of the scheduler in their excellent treatise.
* And many whom I haven't added here yet.

## Performance note

Several performance improvements has been included in the most recent v1.7.x series (updated in early Dec. 2022), including the fix of a long-lasting calculation bug which has been degrading its overall performance.

