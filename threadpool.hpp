////////////////////////////////////////////////////////////////////////////////
/// \file threadpool.hpp
/// \brief  Lightweight, fine-grained multitasking through thread pools.
///
///   This header is part of a multi-tasking library that provides low-overhead
/// concurrent scheduling. This is provided through a thread pool, and uses the
/// [work-stealing method](https://en.wikipedia.org/wiki/Work_stealing) for load
/// balancing.                                                           \n
///   This averts the overhead of creating a new thread for each task, and thus
/// makes fine-grained parallelism feasible.
/// \code
/// #include "thread_pool.hpp"
///
/// //    Create a new thread pool, letting the implementation determine the
/// //  number of worker threads to use.
/// ThreadPool pool;
///
/// //  Function pointers of type void (*) () can be passed as tasks directly.
/// void task (void)
/// {
///   //  ...
/// }
/// //    Put a task into the pool. Because this isn't called from within a
/// //  worker thread, the worker threads synchronize to avoid calling it twice.
/// pool.schedule([](void)
/// {
/// //    Put a task into the pool. This is called from within a worker thread,
/// //  so no synchronization is required.
///   pool.schedule(&task);
///
/// //    Put a task into the pool, treated as if it were part of the currently
/// //  running task. This is called from within a worker thread, so no
/// //  synchronization is required.
///   pool.schedule_subtask([](void) { });
///
/// //  Put a task into the pool, to be executed 2 seconds after it is scheduled.
///  using namespace std::chrono;
///  pool.schedule_after(seconds(2),
///  [](void) {
///    do_something();
///  });
///
/// //    Put a task into the pool, to be executed at the specified time.
///   pool.schedule_after(steady_clock::now() + seconds(2),
///   [](void) {
///     do_something();
///   });
/// });
///
/// //    When the thread pool is destroyed, remaining tasks are forgotten.
/// \endcode
/// \note Tasks assigned to the pool from within one of its worker threads will
///   not encounter contention unless the worker already has
///   `get_worker_capacity()` tasks scheduled. Tasks assigned from outside the
///   pool will encounter contention.
/// \warning  If `get_concurrency()` active tasks (or more) simultaneously
///   block, then all inactive tasks in the pool may be blocked. To prevent
///   deadlock, it is recommended that tasks be constructed such that at least
///   one active task makes progress.
/// \note Users may define the macro `THREAD_POOL_FALSE_SHARING_ALIGNMENT` to
///   specify L1 cache line size when compiling \ref `thread_pool.cpp`. Default
///   is 64 bytes, as this is typical of x86 and x64 systems.
/// \note Users may specify the capacity of each worker's fixed queue by
///   changing the definition of `kLog2Modulus` in \ref `thread_pool.cpp`.
/// \todo Allow tasks to return values, possibly using `std::packaged_task`.
/// \todo Investigate delegates as a replacement for `std::function`:
///   ["The Impossibly Fast C++ Delegates (Fixed)"](https://www.codeproject.com/Articles/1170503/The-Impossibly-Fast-Cplusplus-Delegates-Fixed)
/// \author Nathaniel J. McClatchey, PhD
/// \version  1.4.0
/// \copyright Copyright (c) 2017 Nathaniel J. McClatchey, PhD.               \n
///   [Licensed under the MIT license.](https://github.com/nmcclatchey/ThreadPool/blob/master/LICENSE)  \n
///   You should have received a copy of the license with this software.
////////////////////////////////////////////////////////////////////////////////

#ifndef THREAD_POOL_HPP_
#define THREAD_POOL_HPP_

#if !defined(__cplusplus) || (__cplusplus < 201103L)
#error  "The ThreadPool library requires C++11 or higher."
#endif

//    For a unified interface to Callable objects, I considered 3 options:
//  * Delegates (fast, but would need extra library and wouldn't allow return)
//  * std::function (universally available, but doesn't allow return)
//  * std::packaged_task (allows return, but may not be available. Eg. MinGW-w64
//  with Win32 threads).
#include <functional>
//  For std::size_t
#include <cstddef>
//  For timed waiting.
#include <chrono>
//#include <future>

/// \brief A high-performance asynchronous task scheduler.
/// \warning  If `get_concurrency()` active tasks (or more) simultaneously
///   block, then all inactive tasks in the pool may be blocked. To prevent
///   deadlock, it is recommended that tasks be constructed such that at least
///   one active task makes progress.
/// \note Has a fast path and a slow path. If called by a worker thread,
///   `schedule(const task_type &)` and `schedule_subtask(tconst task_type &)`
///   take the fast path, placing the task into the worker thread's own queue
///   and bypassing any synchronization. If any scheduling function is called by
///   a thread not in the pool or if the worker's queue is at capacity, the slow
///   path is taken, requiring synchronization of the `ThreadPool`'s central
///   queue.
/// \note If the worker's local queue is full, the slow path is taken. If one
///   compiles \ref `thread_pool.cpp` without the macro `NDEBUG` defined, a
///   warning will be printed when an over-full queue is first detected.
//    Implementer's note: The <a href="http://en.cppreference.com/w/cpp/language/pimpl">
//  pointer to implementation idiom</a> provides no significant disadvantage. It
//  will impose a pointer lookup penalty, but only on the slow path. Moreover,
//  dynamic allocation is required regardless, and all initial allocation is
//  combined into a single allocation.
struct ThreadPool
{
/// \brief  A Callable type, taking no arguments and returning void. Used to
///   store tasks for later execution.
  typedef std::function<void()> task_type;
  //typedef std::packaged_task<void()> task_type;

/// \brief  Initializes a thread pool and starts a collection of worker threads.
/// \param[in]  n The number of worker threads to use in the pool.
///
///   Creates a thread pool with *n* worker threads, which are started
/// immediately. If *n == 0*, the number of worker threads is implementation-
/// defined.
  ThreadPool (unsigned workers = 0);

/// \brief  Destroys the `ThreadPool`, terminating all of its worker threads.
///
///   Notifies all worker threads that work is to be discontinued, and blocks
/// until they terminate. Though any task that has already been started will be
/// completed, any tasks that are not active when `~ThreadPool()` is called
/// may be forgotten.
/// \warning  Using a worker thread to destroy its own `ThreadPool` results in
///   undefined behavior.
  ~ThreadPool (void);

//  Thread pools cannot be copied or moved.
  ThreadPool (const ThreadPool &) = delete;
  ThreadPool & operator= (const ThreadPool &) = delete;

/// \brief  Schedules a task to be run asynchronously.
/// \param[in]  task  The task to be performed.
///
///   Schedules a task to be performed asynchronously.
/// \par  Memory order
///   Execution of a task *synchronizes-with* (as in `std::memory_order`) the
/// call to `schedule()` that added it to the pool, using a *Release-Acquire*
/// ordering.

  void schedule (const task_type & task);
/// \overload
  void schedule (task_type && task);

/// \brief  Schedules a task to be run asynchronously after a specified wait
///   duration.
/// \param[in]  rel_time  The duration after which the task is to be run.
/// \param[in]  task  The task to be performed.
///
///   Schedules a task to be performed asynchronously, but only after waiting
/// for a duration of *rel_time*.
/// \par  Memory order
///   Execution of a task *synchronizes-with* (as in `std::memory_order`) the
/// call to `schedule_after()` that added it to the pool, using a
/// *Release-Acquire* ordering.
  template<class Rep, class Period, class Task>
  void schedule_after ( const std::chrono::duration<Rep, Period> & rel_time,
                        Task && task)
  {
    using namespace std;
    sched_impl(chrono::duration_cast<duration>(rel_time), forward<Task>(task));
  }

/// \brief  Schedules a task to be run asynchronously at (or after) a specified
///   point in time.
/// \param[in]  time  The time point after which the task is to be run.
/// \param[in]  task  The task to be performed.
///
///   Schedules a task to be performed asynchronously at a specified time point.
/// \par  Memory order
///   Execution of a task *synchronizes-with* (as in `std::memory_order`) the
/// call to `schedule_after()` that added it to the pool, using a
/// *Release-Acquire* ordering.
  template<class Clock, class Duration, class Task>
  void schedule_after ( const std::chrono::time_point<Clock, Duration> & time,
                        Task && task)
  {
    using namespace std;
    using namespace std::chrono;
    sched_impl(duration_cast<duration>(time-Clock::now()), forward<Task>(task));
  }

/// \brief  Schedules a task to be run asynchronously, but with a hint that the
///   task ought to be considered part of the currently-scheduled task.
/// \param[in]  task  The task to be performed.
/// \see `schedule(const task_type &)`
///
///     Schedules a task to be performed asynchronously, but treats it as if it
///   were part of the currently scheduled task. This gives the task a better
///   chance of being performed soon after scheduling, but relaxes
///   non-starvation guarantees. In particular, if the collective subtasks fail
///   to terminate, then the original task is considered not to have terminated,
///   and later tasks may fail to run.
///     The `schedule_subtask()` method may be used to encourage (not force)
///   depth-first execution -- rather than breadth-first execution -- if tasks
///   exhibit significant branching. This can reduce the odds of a local queue
///   overflow (the slow path) and reduce the memory needed for scheduled tasks.
/// \par  Memory order
///   Execution of a task *synchronizes-with* (as in `std::memory_order`) the
/// call to `schedule_subtask()` that added it to the pool, using a
/// *Release-Acquire* ordering.
/// \warning  Because a subtask is considered as part of the task that spawned
///   it, no guarantees of non-starvation are made should the collective
///   subtasks not terminate.
  void schedule_subtask (const task_type & task);
/// \overload
  void schedule_subtask (task_type && task);

/// \brief  Returns the number of threads in the pool.
/// \return Number of threads in the pool.
///
///   Returns the number of threads in the `ThreadPool`. That is, this function
/// returns the number of tasks that can be truly concurrently executed.
/// \note If more than `get_concurrency()` tasks block simultaneously, no
///   the entire `ThreadPool` is blocked, and no further progress will be made.
  unsigned get_concurrency (void) const;

/// \brief  Maximum number of tasks that can be efficiently scheduled by a
///   worker thread.
/// \return Returns the number of tasks that a worker thread can have scheduled
///   without contention.
///
///   To reduce contention, each worker thread keeps its own queue of tasks. The
/// queues are pre-allocated, and of constant size. The `get_worker_capacity()`
/// function returns the number of tasks that each worker can keep in its own
/// queue -- that is, the number of tasks that a worker can have scheduled
/// before contention occurs.                                                 \n
///   If the returned value is large, many tasks may be simultaneously scheduled
/// without taking the slow path, but more memory is required. If it is small,
/// task scheduling is more likely to take the slow path, but less memory is
/// required.                                                                 \n
///   To select the size of the worker queues, edit the variable `kLog2Modulus`
/// in `thread_pool.cpp`.
  static std::size_t get_worker_capacity (void);

/// \brief  Returns whether the pool is currently idle.
/// \return `true` if the pool is idle, or `false` if not.
///
///   Returns whether the pool is idle. That is, returns true if all threads in
/// the pool are simultaneously idling, or `false` if at least one thread is
/// active. If the pool is halted, the returned value is undefined. Calling this
/// from within one of the `ThreadPool`'s tasks necessarily returns `false`.
  bool is_idle (void) const;

/// \brief  Suspends execution of tasks in the `ThreadPool`.
///
///   Halts all worker threads, blocking the caller until worker threads have
/// fully halted. If `halt()` is called from within one of the pool's worker
/// threads, the calling thread is halted either until `resume()` is called or
/// until the `ThreadPool` is destroyed, whichever comes first.
/// \see  `resume()`
  void halt (void);

/// \brief  Resumes execution of tasks in the `ThreadPool` after a call to
///   `halt()`.
///
///   If execution is currently halted, re-starts all worker threads, possibly
/// blocking the caller until all worker threads have resumed their tasks.
/// \see  `halt()`
  void resume (void);

/// \brief  Returns whether the pool is currently halted.
/// \return Returns `true` if all worker threads are halted, or `false` if not.
///
///   Returns whether the pool is currently halted. Note that this function only
/// begins to return `true` once all tasks have fully halted. Calling it from
/// within one of the `ThreadPool`'s tasks necessarily returns `false`.
  bool is_halted (void) const;
 private:
  void * impl_;
  typedef std::chrono::steady_clock::duration duration;
  void sched_impl ( const duration &, const task_type &);
  void sched_impl ( const duration &, task_type && task);
};

#endif // THREAD_POOL_HPP_
