# ThreadPool

Provides low-overhead concurrent scheduling in C++11 through [thread pools](https://en.wikipedia.org/wiki/Thread_pool "Wikipedia: Thread pool") and [work stealing](https://en.wikipedia.org/wiki/Work_stealing "Wikipedia: Work stealing"). The thread pool approach allows fine-grained parallelism by minimizing the overhead involved in scheduling a task. The work stealing approach allows efficient balancing of scheduled tasks across available threads.

## Why use this library?

- It fulfills common scheduling needs:
    + Performs multiple tasks concurrently.
    + Tasks can be scheduled for an arbitrary later time-point. This provides an efficient replacement for timed waits.
    + A task can spawn subtasks, with a hint that the pool ought to complete them as soon as possible.
- It fulfills some uncommon scheduling needs:
    + The pool can be paused, and later resumed.
- It is designed for efficiency and scalability:
    + Load balancing ensures that as long as there is work to do, it is being done by as many threads as possible.
    + Lock-free data structures use [weak atomic orderings](https://en.cppreference.com/w/cpp/atomic/memory_order#Release-Acquire_ordering "C++ Reference: Release-Acquire ordering"); only when those cores that require new tasks are synchronized.
    + No busy-waiting. Idle threads use [condition variables](https://en.cppreference.com/w/cpp/thread/condition_variable "C++ Reference: condition_variable) to wait without using the CPU.
- It is explicitly documented:
    + Full generated documentation via [Doxygen](http://www.doxygen.nl/).
    + Memory synchronization between task scheduling and execution is explicitly stated in terms of [C++11's memory model](https://en.cppreference.com/w/cpp/atomic/memory_order "C++ Reference: Memory order").
    + Provides usage recommendations for maximizing performance.

## Getting Started

This library consists of a single header and source file. One may compile the source file either as part of one's own project, or as a static library.

### Prerequisites

You will require a C++11 compiler. If you are using MinGW-w64, you may require [MinGW STD Threads](https://github.com/meganz/mingw-std-threads "MinGW STD Threads") to supply `std::thread` and similar.

### Installing

Either compile `threadpool.cpp` as part of your project, or [compile it as a static library](https://en.wikipedia.org/wiki/Static_library "Wikipedia: Static library").

### Using the library

The library is designed to enable a simple use pattern:
1. Create a `ThreadPool` object.
2. Give tasks to the pool by calling the pool's `schedule()`, `schedule_subtask()`, or `schedule_after()` methods.
3. Wait for tasks to complete.

Full documentation for this library may be generated using  Doxygen.

A simple example of how to the library follows:
```
#include "threadpool.hpp"

//  Create a new thread pool, letting the implementation determine the number of worker threads to use.
ThreadPool pool;

//  Put a task into the pool. Because this isn't called from within a worker thread, it takes the scheduler's slow path.
pool.schedule([](void)
{
//  Put a task into the pool. This is called from within a worker thread, so it takes the scheduler's fast path.
  pool.schedule([](void) {
    do_something();
  });
 
//  Put a task into the pool, treated as if it were part of the currently running task. This is called from within a worker thread, so it takes the scheduler's fast path.
  pool.schedule_subtask([](void) {
    do_something();
  });

//  Put a task into the pool, to be executed 2 seconds after it is scheduled.
  using namespace std::chrono;
  pool.schedule_after(seconds(2),
  [](void) {
    do_something();
  });
});

//  When the thread pool is destroyed, remaining unexecuted tasks are forgotten.
```

## Authors

* **Nathaniel J. McClatchey, PhD** - *Initial work*

## License

To encourage people to use this library freely and without concern, this project is licensed under the [MIT License](LICENSE).
