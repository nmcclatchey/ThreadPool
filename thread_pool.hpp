////////////////////////////////////////////////////////////////////////////////
/// \file thread_pool.hpp
/// \brief  Lightweight, fine-grained multitasking through thread pools.
///
///   This header is part of a multi-tasking library that provides low-overhead
/// concurrent scheduling. This is provided through a thread pool, and uses the
/// <a href=https://en.wikipedia.org/wiki/Work_stealing>work-stealing method</a>
/// for work balancing. \n
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
/// });
///
/// //    When the thread pool is destroyed, remaining tasks are forgotten.
/// \endcode
/// \note Tasks assigned to the pool from within one of its worker threads will
///   not require synchronization. Threads assigned from outside the pool will
///   be assigned to a single queue. Avoid this bottleneck, whenever possible.
/// \warning  If <tt>get_concurrency()</tt> active tasks (or more)
///   simultaneously block, then all inactive tasks in the pool may be blocked.
///   To prevent deadlock, it is recommended that tasks be constructed such that
///   at least one active task makes progress.
/// \note Users may define the macro <tt>THREAD_POOL_FALSE_SHARING_ALIGNMENT</tt>
///   to specify L1 cache line size when compiling \ref thread_pool.cpp. Default
///   is 64 bytes, as this is typical of x86 and x64 systems.
/// \note Users may specify the size of each worker's fixed queue by changing
///   the definition of kWorkerQueueSize and recompiling \ref thread_pool.cpp.
/// \todo Allow tasks to return values, possibly using std::packaged_task.
/// \todo Investigate delegates as a replacement for std::function:
///   <a href=https://www.codeproject.com/Articles/1170503/The-Impossibly-Fast-Cplusplus-Delegates-Fixed>"The Impossibly Fast C++ Delegates (Fixed)"</href>
/// \author Nathaniel J. McClatchey, PhD
/// \version  1.1.0
/// \copyright Copyright (c) 2017 Nathaniel J. McClatchey, PhD.  \n
///   Licensed under the MIT license. \n
///   You should have received a copy of the license with this software.
////////////////////////////////////////////////////////////////////////////////

#ifndef THREAD_POOL_HPP_
#define THREAD_POOL_HPP_

//    For a unified interface to Callable objects, I considered 3 options:
//  * Delegates (fast, but would need extra library and wouldn't allow return)
//  * std::function (universally available, but doesn't allow return)
//  * std::packaged_task (allows return, but may not be available. Eg. MinGW-w64
//  with Win32 threads).
#include <functional>
//#include <future>

/// \brief A high-performance asynchronous task scheduler.
/// \warning  If <tt>get_concurrency()</tt> active tasks (or more)
///   simultaneously block, then all inactive tasks in the pool may be blocked.
///   To prevent deadlock, it is recommended that tasks be constructed such that
///   at least one active task makes progress.
/// \note Has a fast path and a slow path. If called by a thread in the pool,
///   <tt>schedule(const task_type &)</tt> and
///   <tt>schedule_subtask(tconst task_type &)</tt> take the fast path, placing
///   the task into the thread's own queue and bypassing any synchronization. If
///   any scheduling function is called by a thread not in the pool, the slow
///   path is taken, requiring synchronization of the <tt>ThreadPool</tt>'s task
///   queue.
/// \note If the worker's local queue is full, the slow path is taken. If one
///   compiles \ref thread_pool.cpp without the macro <tt>NDEBUG</tt> defined,
///   a warning will be printed when an over-full queue is first detected.
//    Implementer's note: The <a href="http://en.cppreference.com/w/cpp/language/pimpl">
//  pointer to implementation idiom</a> provides no significant disadvantage. It
//  will impose a pointer lookup penalty, but only on the slow path. Moreover,
//  dynamic allocation is required regardless, and all initial allocation is
//  combined into a single allocation.
class ThreadPool
{
  void * impl_;
 public:
/// \brief  Maximum number of tasks that can be efficiently scheduled by a
///   worker.
///
///   To reduce contention, each worker thread keeps its own queue of tasks.
/// This constant, kWorkerQueueSize, governs the size of that local queue. If it
/// is large, more tasks may be simultaneously scheduled without taking the slow
/// path, but more memory is required. If it is small, task scheduling is more
/// likely to take the slow path, but less memory is required.
/// \note Most efficient if selected as 1 less than an integral power of 2.
  constexpr static size_t kWorkerQueueSize = (1 << 14) - 1;

/// \brief  A Callable type, taking no arguments and returning void. Used to
///   store tasks for later execution.
  typedef std::function<void()> task_type;
  //typedef std::packaged_task<void()> task_type;

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
/// worker thread, the scheduler attempts to add the task to the worker's local
/// queue (if this can be accomplished, it will avoid contention). If the
/// worker's queue is full, the queue overflows, with any excess tasks being
/// placed in the <tt>ThreadPool</tt>'s task queue. Because resolving overflow
/// requires synchronization, one should typically attempt to avoid causing a
/// queue overflow.
  void schedule (const task_type & task);
/// \overload
  void schedule (task_type && task);

/// \brief  Schedules a task to be run asynchronously, but with a hint that the
///   task ought to be considered part of the currently-scheduled task.
/// \param[in]  task  The task to be performed.
/// \see schedule(const task_type &)
///
///     Schedules a task to be performed asynchronously, but treats it as if it
///   were part of the currently scheduled task. This gives the task a better
///   chance of being performed soon after scheduling, but relaxes
///   non-starvation guarantees. In particular, if the collective subtasks fail
///   to terminate, then the original task is considered not to have terminated,
///   and later tasks may fail to run.
///     The <tt>schedule_subtask</tt> method may be used to encourage (not force)
///   depth-first execution --  rather than breadth-first execution -- if tasks
///   exhibit significant branching. This can reduce the odds of a local queue
///   overflow (the slow path) and reduce the memory needed for scheduled tasks.
/// \warning  Because a subtask is considered as part of the task that spawned
///   it, no guarantees of non-starvation are made should the collective
///   subtasks not terminate.
  void schedule_subtask (const task_type & task);
/// \overload
  void schedule_subtask (task_type && task);

/// \brief  Returns the number of threads in the pool.
/// \return Number of threads in the pool.
///
///   Returns the number of threads in the thread pool. That is, this function
/// returns the number of tasks that can be concurrently executed by the thread
/// pool.
/// \note If more than <tt>get_concurrency()</tt> tasks block simultaneously,
///   no further progress will be made.
  unsigned get_concurrency (void) const;

/// \brief  Returns whether the pool is currently idle.
/// \return \c true if the pool is idle, or \c false if not.
///
///   Returns whether the pool is idle. That is, returns true if all threads in
/// the pool are simultaneously idling, or fales if at least one thread is
/// active. May return \c false spuriously.
  bool is_idle (void) const;
};

#endif // THREAD_POOL_HPP_
