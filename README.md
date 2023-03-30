# BORE (Burst-Oriented Response Enhancer) CPU Scheduler

BORE (Burst-Oriented Response Enhancer) is a modification to the Completely Fair Scheduler, the Linux default CPU scheduler.

It is an attempt to improve CFS' responsiveness for various desktop applications by re-defining scheduling fairness as "fair for same burstiness", and discriminating tasks of different interactiveness by using its unique scoring method based on tasks' "burstiness".

## How it works

* It keeps track of each task's burst time. A burst time is basically how much CPU time in real time scale the task has consumed since last time yielding/sleeping/iowaiting.
* While a task is active, its burst score is continuously calculated by basically counting task's normalized burst time's bit count, with adjustment using pre-configured offset and coefficient.
* Then the burst score is used in the similar way as "niceness" works. It takes a value between 0-39, and as lower by 1, task can consume longer timeslice by approximately 1.25x.
* These steps combined works as a radix conversion from binary logarithm to common logarithm, converting between two different magnitude (nano-seconds-to-minutes timescale to about 0.01-100x scale) dimentionlessly.
* Burst scores are also used to affect how aggressively upwaking tasks can preempt the currently-running task.
* As a result, less "greedy" tasks are given more timeslice and wakeup preemption aggressiveness, while greedier tasks (that yields its timeslice in longer period of time) are weighted less.
* Newly-spawned process' burst score is calculated in a unique way to prevent the type of tasks like "make" from overwhelming interactive tasks by forking many CPU-hungry children.
* The final effect is that there is some kind of equilibrium between opposing greedy&weak tasks (=usually CPU-bound, batch tasks) and modest&strong tasks (=usually I/O-bound, interactive tasks), giving more responsive user experience under coexistence of various types of workloads.

## Tunables

### Example
`$ sudo sysctl -w kernel.sched_bore=1`

![alt Burst time bitcount vs Burst score](https://raw.githubusercontent.com/firelzrd/bore-scheduler/main/burst-time-bitcount-vs-burst-score.png)

### sched_bore (range: 0 - 3, default: 3)

sched_bore & 0x1 = vruntime scaling  
sched_bore & 0x2 = wakeup preemption scaling

setting sched_bore=0 makes the scheduler behave as pseudo CFS.

### sched_burst_penalty_offset (range: 0 - 64, default: 12)

How many bits to reduce from burst time bit count when calculating burst score.  
Increasing this value prevents tasks of shorter burst time from being too strong.  
Increasing this value also lengthens the effective burst time range.

### sched_burst_penalty_scale (range: 0 - 4095, default: 1292)

How strongly tasks are discriminated accordingly to their burst time ratio, scaled in 1/1024 of its precursor value.  
Increasing this value makes burst score rapidly grow as the burst time grows. That means tasks that run longer without sleeping/yielding/iowaiting rapidly lose their power against those that run shorter.  
Decreasing vice versa.

### sched_burst_preempt_offset (range: 0 - 64, default: 16)

How many bits of burst time in nanoseconds at minimum for the burst preempt feature to start functioning.  
Decreasing this value may strengthen wider range of interactive tasks by reinforcing their wakeup preemption, but going too far may harm some benchmark scores.  

### sched_burst_smoothness (range: 0 - 3, default: 1)

A task's actual burst score is the larger one of its latest calculated score or its "historical" score which inherits past score(s). This is done to smoothen the user experience under "burst spike" situations.  
Every time burst score is updated (when the task is dequeued/yielded), its historical score is also updated by mixing burst_score / (2 ^ burst_smoothness) into it. burst_smoothness=0 means no smoothening.

## Special thanks

* Hamad Al Marri, the developer famous for his task schedulers Cachy, CacULE, Baby and TT. BORE has been massively inspired from his great works. He also helped me a lot in the development.
* Peter "ptr1337" Jung, the founder of CachyOS high-performance linux distribution, also being the admin of its development community. His continuous support, sharp analysis and dedicated tests and advice helped me shoot many problems.
* Ching-Chun "jserv" Huang from National Cheng Kung University of Taiwan, and Hui Chun "foxhoundsk" Feng from National Taiwan Ocean University, for detailed analysis and explanation of the scheduler in their excellent treatise.
* And many whom I haven't added here yet.

