/// \file   thread_pool.cpp
/// \brief  Implements \ref thread_pool.hpp.
/// \author Nathaniel J. McClatchey
/// \note   To compile for MinGW-w64 without linking against the winpthreads
/// library, use the <a href=https://github.com/nmcclatchey/mingw-std-threads>
/// MinGW Windows STD Threads library</a>.
/// \copyright Copyright (c) 2017 Nathaniel J. McClatchey.  \n
///   Licensed under the Simplified BSD 2-clause license. \n
///   You should have received a copy of the license with this software.
#include "thread_pool.hpp"

#if !defined(__cplusplus) || (__cplusplus < 201103L)
#error The implementation of ThreadPool requires C++11 or higher.
#endif

#include <atomic>             //  For atomic indexes, etc.
#include <queue>              //  For central task queue

#include <mutex>              //  For locking of central queue.

#if (!defined(__MINGW32__) || defined(_GLIBCXX_HAS_GTHREADS))
#include <thread>             //  For threads. Duh.
#include <condition_variable> //  Let threads sleep instead of spin when idle.
#else
//    This toolchain-specific workaround allows ThreadPool to be used with
//  MinGW-w64 even without linking the winpthreads library. If you lack these
//  headers, you can find them at https://github.com/nmcclatchey/mingw-std-threads .
#pragma message "Using native WIN32 threads for thread pools (" __FILE__ ")."
#include "mingw.thread.h"
#include "mingw.mutex.h"
#include "mingw.condition_variable.h"
#endif

#include <memory>             //  For std::align
#include <climits>            //  For CHAR_BIT

#include <assert.h>           //  For debugging: Fail deadly.
#ifndef NDEBUG
#include <iostream>           //  For debugging: Warn on task queue overflow.
#endif

#ifndef THREAD_POOL_FALSE_SHARING_ALIGNMENT
#if (__cplusplus >= 201703L)
#define THREAD_POOL_FALSE_SHARING_ALIGNMENT std::hardware_destructive_interference_size
#else
#define THREAD_POOL_FALSE_SHARING_ALIGNMENT 64
#endif
#endif
#if (__cplusplus >= 201703L)
#include <new>                //  For alignment purposes.
#endif

namespace {
struct Worker;
struct ThreadPoolImpl;

thread_local Worker * current_worker = nullptr;


struct ThreadPoolImpl
{
  typedef typename ThreadPool::task_type task_type;

  ThreadPoolImpl (Worker *, unsigned);
  ~ThreadPoolImpl (void);
  ThreadPoolImpl (const ThreadPoolImpl &) = delete;
  ThreadPoolImpl & operator= (const ThreadPoolImpl &) = delete;

  void schedule_overflow (task_type &&);

  unsigned get_concurrency (void) const;
// private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<task_type> queue_;

  std::atomic<bool> stop_;

  unsigned threads_, idle_;
  Worker * workers_;

  friend struct Worker;
  friend struct ::ThreadPool;
};

//  Notes:
//  -   "front_" is always claimed for the worker.
//  -   "back_"  stores past-the-end markers both for writing and validity. If
//    they are unequal, the back is locked.
//  -   For various reasons, it is possible for the front marker to be between
//    the write and valid pte markers. In such a case, the already-claimed task
//    may be read, but no further tasks will be read, even if claimed.
struct alignas(THREAD_POOL_FALSE_SHARING_ALIGNMENT) Worker
{
  typedef typename ThreadPool::task_type task_type;
  ~Worker (void) =default;

  Worker (const Worker &) = delete;
  Worker & operator= (const Worker &) = delete;
  void operator() (void);
  void kill_thread (void)
  {
    thread_.join();
  }
// private:
  friend struct ThreadPoolImpl;
  friend struct ::ThreadPool;
  Worker (ThreadPoolImpl &);

  typedef uint_fast32_t index_type;

  constexpr static size_t kModulus = ThreadPool::kWorkerQueueSize + 1;
  constexpr static size_t kValidShift = (CHAR_BIT * sizeof(index_type)) / 2;
  constexpr static index_type kWriteMask = ~(~static_cast<index_type>(0) << kValidShift);
  static_assert(ThreadPool::kWorkerQueueSize < (1 << kValidShift), "Thread \
pool's local task queue size exceeds limit of selected index type.");
  static_assert(ThreadPool::kWorkerQueueSize > 1, "Invalid worker thread queue \
size.");

  inline static constexpr index_type get_distance (index_type, index_type);
  //inline static constexpr index_type get_writeable (index_type, index_type);
  inline static constexpr index_type get_valid (index_type);
  inline static constexpr index_type get_write (index_type);
  inline static constexpr index_type make_back (index_type, index_type);
  inline static constexpr index_type make_back (index_type);


  unsigned steal (void);
  unsigned steal_from (Worker & source, unsigned divisor);
  bool pop (task_type & task);
  unsigned push_front(std::queue<task_type> &, unsigned number);
  bool push (task_type && tasks);
  bool push_front (task_type && tasks);
  bool execute (void);
  index_type count_tasks (void) const;
  void refresh_tasks (std::queue<task_type> &, unsigned number);

//  These store information about the current state of the deque.
//  -   front_ is modified only by the Worker's own thread. Reads and writes
//    must be atomic, however, to avoid torn writes.
//  -   back_ is potentially modified by all threads. The top and bottom halves
//    store a past-the-end (PTE) marker for the occupied slots, and a PTE marker
//    for the slots this Worker is permitted to read, respectively.
  std::atomic<index_type> front_, back_;
//    To avoid starvation for tasks in the overflow queue, I pull in its tasks
//  once every time a worker finishes a batch of tasks. The variable countdown_
//  records the remaining size of the batch. A successfully scheduled subtask
//  will increment this to ensure the originally scheduled tasks are completed
//  as part of the batch.
  uint_fast32_t countdown_ : kValidShift,
//    While a task is being executed, the front_ marker is not incremented. This
//  avoids early claiming of a new task (which would prevent that task from
//  being stolen), but makes the push-to-front process a bit more complicated.
//  In particular, the push-to-front should overwrite the front when first
//  called during an execution, but not afterward.
                front_invalid_ : 1;
//    When this Worker runs out of tasks, it will search for more. A central
//  ThreadPool object will serve to coordinate work-stealing (that is, store the
//  addresses of other Workers), provide new tasks, and capture overflow should
//  a Worker generate more tasks than can fit in its deque.
  ThreadPoolImpl & pool_;
//    Need to keep the thread's handle for later joining. I could work around
//  this, but the workaround would be less efficient.
  std::thread thread_;
//  Task queue.
  task_type tasks_ [kModulus];
};

constexpr typename Worker::index_type Worker::get_distance (index_type left, index_type right)
{
  return (right - left + kModulus) % kModulus;
}

constexpr typename Worker::index_type Worker::get_valid (index_type b)
{
  return b >> kValidShift;
}

constexpr typename Worker::index_type Worker::get_write (index_type b)
{
  static_assert((kWriteMask >> kValidShift) == 0, "WRITE and VALID regions must not intersect.");
  return b & kWriteMask;
}

constexpr typename Worker::index_type Worker::make_back (index_type write, index_type valid)
{
  return write | (valid << kValidShift);
}

constexpr typename Worker::index_type Worker::make_back (index_type write)
{
  return write | (write << kValidShift);
}

//  Work-stealing will occur as follows:
//  1.  Determine the exact number of tasks that can be added to this queue.
//    -   Note: Though stealing only occurs when the queue is empty, it can be
//      empty because of another process performing work-stealing.
//    -   Note: This value need not be refreshed, as it can only decrease. This
//      is because only the Worker's thread will be allowed to add items to its
//      queue.
//  2.  Estimate the number of items in the the source queue.
//    -   If the queue is already being edited, giving up is an option. The
//      worker will come back later or try a different source queue, effectively
//      creating a spin-lock.
//  3.  Set source's write to write - (items - stolen). Do not change valid.
//    -   When write != valid, the write-head is locked. Moreover, reading
//      should not occur if read is in the interval [write, valid].
//  4.  Check whether source's read is in the interval [write, valid]. If it is,
//    then the current interval is contended. Go to step 2.
//  5.  Now that the write interval is locked, copy to the front (reading side)
//    of this thread's queue. This is safe because only this thread affects this
//    part of the queue.
//  6.  Set source's VALID equal to its WRITE to unlock that part of the queue.

//    Steals approximately [available] / [divisor] tasks from source, if
//  possible. Returns number of successfully stolen tasks.
unsigned Worker::steal_from (Worker & source, unsigned divisor)
{
  index_type this_front,    this_back,    writeable, stolen,
              source_front, source_back,  source_valid, source_write;
//  Worker::steal_from may only be called from the Worker's owned thread.
  assert(std::this_thread::get_id() == thread_.get_id());
  assert(this != &source);
  assert(!front_invalid_);

  this_front = front_.load(std::memory_order_relaxed);
  this_back = back_.load(std::memory_order_acquire);

  writeable = get_distance(get_valid(this_back), this_front - 1);
  if (writeable == 0)
    return 0;

//  Lock the source queue, reserving several tasks to steal.
  source_back = source.back_.load(std::memory_order_relaxed);
  do {
    source_valid = get_valid(source_back);
    if (source_valid != get_write(source_back))
      return 0;
    source_front = source.front_.load(std::memory_order_relaxed);
//  Stolen is actually from WRITE, but WRITE and VALID are identical.
    index_type valid = get_distance(source_front, source_valid);
//    Must not attempt to claim the current front pointer, so require at least 2
//  items in source queue.
    if (valid < 2)
      return 0;
    stolen = (valid + divisor - 2) / divisor;
    if (writeable < stolen)
      stolen = writeable;
    source_write = (source_valid - stolen + kModulus) % kModulus;
    //assert(get_distance(source_front, source_write) <= get_distance(source_front, source_valid));
  } while (!source.back_.compare_exchange_weak(source_back,
                                               make_back(source_write, source_valid),
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed));
//    Now that the lock has been acquired, read may advance at most one more
//  time. That is, simply ensuring that READ < WRITE will suffice to ensure
//  correct behavior. Unfortunately, the READ may already be in the claim. Only
//  READ <= VALID is certain until we enforce it.
//    Note that by including the one-increment error margin, the following
//  adjustment needs to be run at most once.
  //std::atomic_thread_fence(std::memory_order_acquire);
  {
    source_front = source.front_.load(std::memory_order_acquire);
    index_type valid = get_distance(source_front, source_valid);
    if (valid < 2)  //  Unlock. There aren't any unclaimed tasks to steal.
    {
      source.back_.store(make_back(source_valid));
      return 0;
    }

    index_type readable = get_distance(source_front, source_write);
//    Even if READ <= VALID, (that is, normal behavior), if READ == WRITE then
//  we must increment WRITE as READ may be incremented during the write phase.
    if ((readable > valid) || (readable == 0))
    {
      stolen = (valid + divisor - 2) / divisor;
//    Number of valid tasks can only have been reduced since last time, so there
//  is no reason to double-check whether dest queue can accept the tasks.
      source_write = (source_valid - stolen + kModulus) % kModulus;
      source.back_.store(make_back(source_write, source_valid),
                         std::memory_order_relaxed);
    }
  }

#ifndef NDEBUG
  assert(source_write != source_valid);
  source_front = source.front_.load(std::memory_order_relaxed);
  assert(get_distance(source_front, source_write) <= get_distance(source_front, source_valid));
#endif

  assert(source_write != source_valid);
  do {
    source_valid = (source_valid - 1 + kModulus) % kModulus;
    this_front = (this_front - 1 + kModulus) % kModulus;
    assert(tasks_[this_front] == nullptr);
    tasks_[this_front] = std::move(source.tasks_[source_valid]);
  } while (source_valid != source_write);

  front_.store(this_front, std::memory_order_release);
  source.back_.store(make_back(source_valid), std::memory_order_release);
  return stolen;
}

//    Removes a task from the front of the queue, if possible. Returns true or
//  false for success or failure, respectively.
bool Worker::pop (task_type & task)
{
  index_type front, back, readable;

//  Worker::pop may only be called from the Worker's owned thread.
  assert(std::this_thread::get_id() == thread_.get_id());

  front = front_.load(std::memory_order_relaxed);
  back = back_.load(std::memory_order_acquire);

  readable = get_distance(front, get_write(back));
//    Two circumstances can prevent reading: Either there is nothing to read, or
//  the current location is claimed. Even once the claim is resolved, there may
//  or may not be something to read.
  if ((readable == 0) || (readable > get_distance(front, get_valid(back))))
    return false;
  task = std::move(tasks_[front]);

  front_.store((front + 1) % kModulus, std::memory_order_relaxed);
//  I need to release back_ so that the write to front_ is visible to thieves.
  back_.fetch_or(0, std::memory_order_release);
  return true;
}

//    Removes, then performs the task at the front of the queue. Claims the next
//  task after performing the current one.
bool Worker::execute (void)
{
  index_type front, back, readable;

  assert(!front_invalid_);

//  Worker::execute may only be called from the Worker's owned thread.
  assert(std::this_thread::get_id() == thread_.get_id());

  front = front_.load(std::memory_order_relaxed);
  back = back_.load(std::memory_order_acquire);

  readable = get_distance(front, get_write(back));
//    Two circumstances can prevent reading: Either there is nothing to read, or
//  the current location is claimed. Even once the claim is resolved, there may
//  or may not be something to read.
  if ((readable == 0) || (readable > get_distance(front, get_valid(back))))
    return false;

  task_type task = std::move(tasks_[front]);
  assert(task != nullptr);
  front_invalid_ = true;
  task();

  if (front_invalid_)
  {
    front_invalid_ = false;
    front = front_.load(std::memory_order_relaxed);
    front_.store((front + 1) % kModulus, std::memory_order_relaxed);
//  I need to release back_ so that the write to front_ is visible to thieves.
    back_.fetch_or(0, std::memory_order_release);
  }
  return true;
}

//  Pulls some tasks into the local queue from the central queue
void Worker::refresh_tasks (std::queue<task_type> & tasks, unsigned number)
{
  unsigned pushed = push_front(tasks, number);
  if (pushed == 0)
  {
    if (tasks.size() < number)
      number = tasks.size();
    task_type task;

    while (number && pop(task))
    {
      tasks.emplace(std::move(task));
      ++pushed;
      --number;
    }
    push_front(tasks, pushed);
  }
}

//    Pushes a task onto the back of the queue, if possible. If the back of the
//  queue is in contention, (eg. because of work stealing), pushes onto the
//  front of the queue instead.
bool Worker::push (task_type && task)
{
//  Worker::push may only be called from the Worker's owned thread.
  assert(std::this_thread::get_id() == thread_.get_id());

  index_type front, back, valid;

  front = front_.load(std::memory_order_relaxed);
  back = back_.load(std::memory_order_acquire);

  valid = get_valid(back);
  if (((front - valid + kModulus) % kModulus) == 1)
    return false;
  index_type write, expected, new_back;

  write     = get_write(back);
  new_back  = (write + 1) % kModulus;
  expected  = make_back(write);
  if (back_.compare_exchange_strong(expected, make_back(write, new_back),
                                    std::memory_order_acquire,
                                    std::memory_order_relaxed))
  {
    assert(tasks_[write] == nullptr);
    tasks_[write] = std::move(task);
    back_.store(make_back(new_back), std::memory_order_release);
  }
  else
  {
    index_type write = front;
    front = (front - 1 + kModulus) % kModulus;
    if (!front_invalid_)
      write = front;
    assert(tasks_[write] == nullptr);
    tasks_[write] = std::move(task);
    front_.store(front, std::memory_order_release);
  }
  return true;
}

//    Places a new task at the front of the queue. Note that this skirts anti-
//  starvation precautions.
bool Worker::push_front (task_type && task)
{
  index_type front, back;

//  Worker::push_front may only be called from the Worker's owned thread.
  assert(std::this_thread::get_id() == thread_.get_id());

  front = front_.load(std::memory_order_relaxed);
  back = back_.load(std::memory_order_acquire);

  if ((front - get_valid(back) + kModulus) % kModulus == 1)
    return false;
  index_type write = front;
  front = (front - 1 + kModulus) % kModulus;
  if (!front_invalid_)
    write = front;
  assert(tasks_[write] == nullptr);
  tasks_[write] = std::move(task);
  front_.store(front, std::memory_order_release);
  return true;
}

//    Places multiple new tasks at the front of the queue. Note that this skirts
//  anti-starvation precautions.
unsigned Worker::push_front (std::queue<task_type> & tasks, unsigned number)
{
  index_type front, back, written, n;

//  Worker::push_front may only be called from the Worker's owned thread.
  assert(std::this_thread::get_id() == thread_.get_id());
  if (tasks.empty())
    return 0;

  front = front_.load(std::memory_order_relaxed);
  back = back_.load(std::memory_order_acquire);

  written = (front - get_valid(back) - 1 + kModulus) % kModulus;
  if (number < written)
    written = number;
  if (written == 0)
    return 0;

//  In C++, bool converts implicitly to 0 (false) or 1 (true).
  front += front_invalid_;
  n = written;
  do {
    front = (front - 1 + kModulus) % kModulus;
    assert(tasks_[front] == nullptr);
    tasks_[front] = std::move(tasks.front());
    assert(tasks_[front] != nullptr);
    tasks.pop();
    if (tasks.empty())
    {
      written -= n - 1;
      break;
    }
  } while (--n);
  front = (front - front_invalid_ + kModulus) % kModulus;
  front_.store(front, std::memory_order_release);
  return written;
}

//  Returns an estimate of the number of tasks currently in the queue.
typename Worker::index_type Worker::count_tasks (void) const
{
  index_type front = front_.load(std::memory_order_relaxed);
  index_type back = back_.load(std::memory_order_relaxed);
  return get_distance(front, get_valid(back));
}

Worker::Worker (ThreadPoolImpl & pool)
  : front_(0), back_(0), countdown_(2), front_invalid_(false), pool_(pool),
    thread_(std::reference_wrapper<Worker>(*this))
{
}

//  Attempts to steal work from other worker threads in the same pool.
unsigned Worker::steal (void)
{
  //return 0;
  std::hash<typename std::thread::id> hasher;
  size_t source = hasher(thread_.get_id()) + front_.load(std::memory_order_relaxed);// + time(nullptr);
  size_t num_threads = pool_.get_concurrency();
  unsigned count = 0;
  for (size_t n = num_threads; n; --n) {
    source = (source + 1) % num_threads;
    Worker * victim = pool_.workers_ + source;
    if (victim == this)
      continue;
    count += steal_from(*victim, num_threads);
    if (count > 0)
      break;
  }
  return count;
}

//    Performs a loop of the form execute-steal-check_central_queue-repeat.
//  Sleeps if no work is available in this and other queues.
void Worker::operator() (void)
{
  index_type last_size = 0;
//    This thread-local variable allows O(1) scheduling (allows pushing directly
//  to the local task queue).
  current_worker = this;
  while (true)
  {
    if (--countdown_ == 0)
    {
//    Periodically check whether the program is trying to destroy the pool.
      if (pool_.stop_.load(std::memory_order_relaxed))
        return;
      typedef decltype(pool_.mutex_) mutex_type;
      mutex_type & mutex = pool_.mutex_;

      index_type size = count_tasks();
      countdown_ = size + 2;
      if (pool_.queue_.empty())
      {
//    If the queue size has stabilized, it's likely that all tasks are waiting
//  on something (and thus continually re-adding themselves). Shake things up a
//  bit by re-shuffling tasks.
        if (size == last_size)
          size += steal();
        last_size = size;
        continue;
      }
      if (mutex.try_lock())
      {
        std::lock_guard<mutex_type> guard (mutex, std::adopt_lock);
        if (!pool_.queue_.empty())
          refresh_tasks(pool_.queue_, 32);
        countdown_ += 32;
      }
      else
      {
//  If the queue size has stabilized, it's probably full of infinite loops.
        if (size == last_size)
          countdown_ = 4;
      }
      last_size = size;
    }
//    Second, check for (and perform) any tasks in this thread's queue.
    if (execute())
      continue;
//    Third, check whether there are common tasks available. This will also
//  serve to jump-start the worker.
    unsigned count = 0;
    typedef decltype(pool_.mutex_) mutex_type;
    mutex_type & mutex = pool_.mutex_;
    decltype(pool_.queue_) & queue = pool_.queue_;
    if ((!queue.empty()) && mutex.try_lock())
    {
      std::lock_guard<mutex_type> guard (mutex, std::adopt_lock);
      count += push_front(queue, 32);
    }
    if (count > 0)
      continue;
//    Fourth, try work stealing.
    count += steal();
    if (count > 0)
      continue;
//  Fifth, wait a bit for something to change...
    size_t num_threads = pool_.get_concurrency();
    bool should_idle = (count_tasks() == 0);
    for (size_t n = num_threads; n && should_idle; --n)
    {
      Worker * victim = pool_.workers_ + n;
      should_idle = (victim->count_tasks() < 2);
    }
    if (should_idle)
    {
      std::unique_lock<mutex_type> guard (mutex);
      if (queue.empty())
      {
        if (pool_.stop_.load(std::memory_order_relaxed))
          return;
        ++pool_.idle_;
        pool_.cv_.wait(guard);
        --pool_.idle_;
        if (pool_.stop_.load(std::memory_order_relaxed))
          return;
      }
      push_front(queue, 32);
    }
    else
      std::this_thread::yield();
  }
}









ThreadPoolImpl::ThreadPoolImpl (Worker * workers, unsigned threads)
  : mutex_(), cv_(), queue_(), stop_(false),
    threads_(threads), idle_(0),
    workers_(workers)
{
  for (unsigned i = 0; i < threads_; ++i)
    new(workers + i) Worker(*this);
}

ThreadPoolImpl::~ThreadPoolImpl (void)
{
  {
    std::lock_guard<decltype(mutex_)> guard (mutex_);
    stop_.store(true, std::memory_order_relaxed);
  }
  cv_.notify_all();
  for (unsigned i = 0; i < threads_; ++i)
    workers_[i].kill_thread();
  for (unsigned i = 0; i < threads_; ++i)
    workers_[i].~Worker();
}

unsigned ThreadPoolImpl::get_concurrency(void) const
{
  return threads_;
}

void ThreadPoolImpl::schedule_overflow (task_type && task)
{
  {
    std::lock_guard<decltype(mutex_)> guard (mutex_);
    queue_.emplace(std::move(task));
  }
  cv_.notify_one();
}

} //  Namespace [Anonymous]

unsigned ThreadPool::get_concurrency(void) const
{
  return static_cast<ThreadPoolImpl*>(impl_)->threads_;
}

#ifndef NDEBUG
namespace {
  std::atomic_flag overflow_warning_given = ATOMIC_FLAG_INIT;
}
#endif

//  Schedules a task normally, at the back of the queue.
void ThreadPool::schedule (task_type && task)
{
  Worker * worker = current_worker;
//  If a thread is attempting to schedule in its own pool...
  if ((worker != nullptr) && (&(worker->pool_) == impl_))
  {
    if (worker->push(std::move(task)))
    {
      if (worker->pool_.idle_ > 0)
        worker->pool_.cv_.notify_one();
      return;
    }
#ifndef NDEBUG
    else if (!overflow_warning_given.test_and_set())
      std::cerr << "Task queue overflow. May impact performance." << std::endl;
#endif
  }
  static_cast<ThreadPoolImpl*>(impl_)->schedule_overflow(std::move(task));
}

//  Schedule at the front of the queue, if in fast path.
void ThreadPool::schedule_subtask (task_type && task)
{
  Worker * worker = current_worker;
//  If a thread is attempting to schedule in its own pool, take the fast path.
  if ((worker != nullptr) && (&(worker->pool_) == impl_))
  {
    if (worker->push_front(std::move(task)))
    {
//    Delay lower-level (central) queue from being accessed, to fully support
//  depth-first traversal of task tree.
      ++worker->countdown_;
//  Wake up the rest of the pool, if necessary.
      if (worker->pool_.idle_ > 0)
        worker->pool_.cv_.notify_one();
      return;
    }
#ifndef NDEBUG
    else if (!overflow_warning_given.test_and_set())
      std::cerr << "Task queue overflow. May impact performance." << std::endl;
#endif
  }
  static_cast<ThreadPoolImpl*>(impl_)->schedule_overflow(std::move(task));
}

ThreadPool::ThreadPool (unsigned threads)
  : impl_(nullptr)
{
  if (threads == 0)
  {
    threads = std::thread::hardware_concurrency();
//  Only adjust the default if a hint is given.
    if (threads < 2)
      threads = 2;
  }
  size_t space = sizeof(ThreadPoolImpl) + threads * sizeof(Worker) +           \
                 alignof(ThreadPoolImpl) + alignof(Worker) + sizeof(void**);
  void * memory = malloc(space);
  if (memory == nullptr)
    goto fail_l1;
  {
    using std::align;
    void * ptr = memory;
    if (!align(alignof(ThreadPoolImpl), sizeof(ThreadPoolImpl), ptr, space))
      goto fail_l2;
    impl_ = ptr;
    ThreadPoolImpl * impl = static_cast<ThreadPoolImpl*>(impl_);
    ptr = impl + 1;
    if (!align(alignof(Worker), threads * sizeof(Worker), ptr, space))
      goto fail_l2;
    Worker * workers = static_cast<Worker*>(ptr);
    new(impl) ThreadPoolImpl(workers, threads);

    //std::cerr << "Storing " << memory_ << "\n";
    *reinterpret_cast<void**>(workers + threads) = memory;
  }
  return;
fail_l2:
  free(memory);
fail_l1:
  throw std::bad_alloc();
}

ThreadPool::~ThreadPool (void)
{
  ThreadPoolImpl * impl = static_cast<ThreadPoolImpl*>(impl_);
  void * memory = *reinterpret_cast<void**>(impl->workers_ + impl->threads_);
  impl->~ThreadPoolImpl();
  free(memory);
}
