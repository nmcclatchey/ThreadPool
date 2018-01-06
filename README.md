# ThreadPool

Provides low-overhead concurrent scheduling in C++11 through [thread pools](https://en.wikipedia.org/wiki/Thread_pool "Wikipedia: Thread pool") and [work stealing](https://en.wikipedia.org/wiki/Work_stealing "Wikipedia: Work stealing"). The thread pool approach allows fine-grained parallelism by minimizing the overhead involved in scheduling a task. The work stealing approach allows efficient balancing of scheduled tasks across available threads.

## Getting Started

This library consists of a single header and source file. One may compile the source file either as part of one's own project, or as a static library.

### Prerequisites

You will require a C++11 compiler. If you are using MinGW-w64, you may require [MinGW STD Threads](https://github.com/nmcclatchey/mingw-std-threads "MinGW STD Threads") to supply std::thread and similar.

### Installing

Either compile thread_pool.cpp as part of your project, or [compile it as a static library](https://en.wikipedia.org/wiki/Static_library "Wikipedia: Static library").

### Using the library

Full documentation for this library may be generated using Doxygen. A simple example of how to the library follows:
```
#include "thread_pool.hpp"

//  Create a new thread pool, letting the implementation determine the number of worker threads to use.
ThreadPool pool;

//  Put a task into the pool. Because this isn't called from within a worker thread, it takes the scheduler's slow path.
pool.schedule([](void)
{
//  Put a task into the pool. This is called from within a worker thread, so it takes the scheduler's fast path.
  pool.schedule([](void) { });
 
//  Put a task into the pool, treated as if it were part of the currently running task. This is called from within a worker thread, so it takes the scheduler's fast path.
  pool.schedule_subtask([](void) { });
});

//  When the thread pool is destroyed, remaining unexecuted tasks are forgotten.
```

## Running the tests

Compile tests.cpp, then link with the compiled object from thread_pool.cpp. Run the resulting executable. The test should complete some or all of the 80000000 simple tasks within the time allotted; tasks remaining in the pool when the pool is destroyed are ignored. The test will then wait for 10 seconds, to test idle performance, followed by a repeat of the previous test. As a point of interest, the test will state the minimum and maximum number of tasks executed by workers; this can be used to estimate how well the scheduler is balancing work across threads.

## Authors

* **Nathaniel J. McClatchey** - *Initial work*

## License

This project is licensed under the [Simplified BSD 2-Clause License](LICENSE).
