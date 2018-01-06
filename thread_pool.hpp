////////////////////////////////////////////////////////////////////////////////
/// \file thread_pool.hpp
/// \brief  Lightweight multitasking through thread pools.
/// \author Nathaniel J. McClatchey
/// \version  1.0.0
/// \todo Investigate delegates as a replacement for std::function:
///   <a href=https://www.codeproject.com/Articles/1170503/The-Impossibly-Fast-Cplusplus-Delegates-Fixed>"The Impossibly Fast C++ Delegates (Fixed)"</href>
/// \note Users may define the macro <tt>THREAD_POOL_FALSE_SHARING_ALIGNMENT</tt>
///   to specify L1 cache line size when compiling \ref thread_pool.cpp. Default
///   is 64 bytes, as this is typical of x86 and x64 systems.
/// \copyright Copyright (c) 2017 Nathaniel J. McClatchey.  \n
///   Licensed under the Simplified BSD 2-clause license. \n
///   You should have received a copy of the license with this software.
///
///   This header is part of a multi-tasking library that provides low-overhead
/// concurrent scheduling. This is provided through a thread pool, and uses the
/// <a href=https://en.wikipedia.org/wiki/Work_stealing>work-stealing method</a>
/// for work balancing. \n
///   This averts the overhead of creating a new thread for each task, and thus
/// makes fine-grained parallelism feasible.  \n
///   I provide a usage example below:
/// \code
/// #include "thread_pool.hpp"
///
/// //    Create a new thread pool, letting the implementation determine the
/// //  number of worker threads to use.
/// ThreadPool pool;
///
/// //    Put a task into the pool. Because this isn't called from within a
/// //  worker thread, it takes the scheduler's slow path.
/// pool.schedule([](void)
/// {
/// //    Put a task into the pool. This is called from within a worker thread,
/// //  so it takes the scheduler's fast path.
///   pool.schedule([](void) { });
///
/// //    Put a task into the pool, treated as if it were part of the currently
/// //  running task. This is called from within a worker thread, so it takes
/// //  the scheduler's fast path.
///   pool.schedule_subtask([](void) { });
/// });
///
/// //    When the thread pool is destroyed, remaining unexecuted tasks are
/// //  forgotten.
/// \endcode
////////////////////////////////////////////////////////////////////////////////

#ifndef THREAD_POOL_HPP_
#define THREAD_POOL_HPP_

//    I need some means of recording tasks. In most cases, passing arguments by
//  reference will suffice, so the fast delegate library should work as a
//  replacement, if needed.
#include <functional>

/// \brief A high-performance asynchronous task scheduler.
/// \warning  If <tt>get_concurrency()</tt> active tasks (or more)
///   simultaneously block, then all inactive tasks in the pool may be blocked.
///   To prevent deadlock, it is recommended that tasks be constructed such that
///   at least one active task makes progress.
/// \note Has a fast path and a slow path. If called by a thread in the pool,
///   <tt>schedule(task_type&&)</tt> and <tt>schedule_subtask(task_type&&)</tt>
///   take the fast path, placing the task into the thread's own queue and
///   bypassing the <tt>ThreadPool</tt> instance. If
///   <tt>schedule_later(task_type&&)</tt> is called, or any scheduling function
///   is called by a thread not in the pool, the slow path is taken, requiring
///   synchronization of the <tt>ThreadPool</tt>'s task queue. If the worker's
///   local queue is full, the slow path is taken, regardless of the thread from
///   which the function was called.
/// \note The <a href="http://en.cppreference.com/w/cpp/language/pimpl">pointer
///   to implementation idiom</a> provides no significant disadvantage. It will
///   impose a pointer lookup penalty, but only on the slow path. Moreover,
///   dynamic allocation is required regardless, and all initial allocation may
///   be combined.
class ThreadPool
{
  void * impl_;
 public:
/// \brief  Maximum number of tasks that can be efficiently scheduled by a
///   worker.
/// \note Most efficient if selected as 1 less than an integer power of 2.
///
///   To reduce contention, each worker thread keeps its own queue of tasks.
/// This constant, kWorkerQueueSize, governs the size of that local queue. If it
/// is large, more tasks may be simultaneously scheduled without taking the slow
/// path, but more memory is required. If it is small, task scheduling is more
/// likely to take the slow path, but less memory is required.
  constexpr static size_t kWorkerQueueSize = (1 << 14) - 1;

/// \brief  A Callable type, taking no arguments and returning void. Used to
///   store tasks for later execution.
  typedef std::function<void()> task_type;

/// \brief  Initializes a thread pool and starts a collection of worker threads.
/// \param[in]  workers The number of worker threads to use in the pool.
///
///   Creates a thread pool with <tt>workers</tt> worker threads. The worker
/// threads are started immediately. If <tt>workers == 0</tt>, the number of
/// worker threads is implementation-defined.
  ThreadPool (unsigned workers = 0);

/// \brief  Destroys the thread pool, terminating all of its worker threads.
///
///   Notifies all worker threads that work is to be discontinued, and blocks
/// until they terminate. Though any task that has already been started will be
/// completed, any tasks that are not active when <tt>~ThreadPool()</tt> is
/// called may be forgotten.
  ~ThreadPool (void);

//  Thread pools cannot be copied or moved.
  ThreadPool (const ThreadPool &) = delete;
  ThreadPool & operator= (const ThreadPool &) = delete;

/// \brief  Schedules a task to be run asynchronously.
/// \param[in]  task  The task to be performed.
///
///   Adds the provided task to a queue for later execution. If called by a
/// worker thread, the scheduler attempts to add the task to the worker's own
/// queue (if this can be accomplished, it will avoid contention). If the
/// worker's queue is full, the queue overflows, with any excess tasks being
/// placed in the <tt>ThreadPool</tt>'s task queue. Because resolving overflow
/// requires synchronization, one should typically attempt to avoid causing a
/// queue overflow.
  void schedule (task_type && task);

/// \brief  Schedules a task to be run asynchronously, but with a hint that the
///   task ought to be considered part of the currently-scheduled task.
///
///     Schedules a task to be performed asynchronously, but treats it as if it
///   were part of the currently scheduled task. This gives the task a better
///   chance of being performed soon after scheduling, but relaxes
///   non-starvation guarantees. In particular, if the collective subtasks fail
///   to terminate, then the original task is considered not to have terminated,
///   and later tasks may fail to run.
///     The <tt>schedule_subtask</tt> method may be used to encourage
///   depth-first execution, rather than breadth-first execution, if tasks
///   exhibit significant branching. This can reduce the odds of taking the
///   scheduler's slow path and reduce the memory devoted to scheduling tasks.
/// \param[in]  task  The task to be performed.
/// \warning  Because a subtask is considered as part of the task that spawned
///   it, no guarantees of non-starvation are made should the collective
///   subtasks not terminate.
/// \see schedule(task_type &&)
  void schedule_subtask (task_type && task);

/// \brief  Returns the number of threads in the pool.
/// \return Number of threads in the pool.
///
///   Returns the number of threads in the thread pool. That is, this function
/// returns the number of tasks that can be concurrently executed by the thread
/// pool.
  unsigned get_concurrency (void) const;

/// \brief  Returns whether the pool is currently idle.
/// \return \c true if the pool is idle, or \c false if not.
///
///   Returns whether the pool is idle. May return \c false spuriously.
  bool is_idle (void) const;
};

#endif // THREAD_POOL_HPP_
