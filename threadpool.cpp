/// \file   threadpool.cpp
/// \brief  Implements \ref `threadpool.hpp`.
/// \author Nathaniel J. McClatchey, PhD
/// \copyright Copyright (c) 2017 Nathaniel J. McClatchey, PhD.               \n
///   Licensed under the MIT license.                                         \n
///   You should have received a copy of the license with this software.
/// \note   To compile for MinGW-w64 without linking against the winpthreads
/// library, use the <a href=https://github.com/nmcclatchey/mingw-std-threads>
/// MinGW Windows STD Threads library</a>.
#include "threadpool.hpp"

#if !defined(__cplusplus) || (__cplusplus < 201103L)
#error The implementation of ThreadPool requires C++11 or higher.
#endif

#include <atomic>             //  For atomic indexes, etc.
#include <queue>              //  For central task queue

#if (!defined(__MINGW32__) || defined(_GLIBCXX_HAS_GTHREADS))
#include <thread>             //  For threads. Duh.
#include <mutex>              //  For locking of central queue.
#include <condition_variable> //  Let threads sleep instead of spin when idle.
#else
//    This toolchain-specific workaround allows ThreadPool to be used with
//  MinGW-w64 even without linking the winpthreads library. If you lack these
//  headers, you can find them at https://github.com/nmcclatchey/mingw-std-threads .
#include "mingw.thread.h"
#include "mingw.mutex.h"
#include "mingw.condition_variable.h"
#endif

#include <cstdlib>            //  For std::malloc and std::free
#include <memory>             //  For std::align
#include <climits>            //  For CHAR_BIT

#include <cassert>            //  For debugging: Fail deadly.
#ifndef NDEBUG
#include <cstdio>             //  For debugging: Warn on task queue overflow.
#endif

#ifndef THREAD_POOL_FALSE_SHARING_ALIGNMENT
#if false && (__cplusplus >= 201703L)
//  Note: Not yet in GCC.
#include <new>                //  For alignment purposes.
#define THREAD_POOL_FALSE_SHARING_ALIGNMENT std::hardware_destructive_interference_size
#else
#define THREAD_POOL_FALSE_SHARING_ALIGNMENT 64
#endif
#endif

namespace {
//  Forward-declarations
struct Worker;
struct ThreadPoolImpl;

/// \brief  Determines the capacity of each `Worker`'s queue. Larger values take
///   more memory, but less processing power. The reverse holds for smaller
///   values.
/// \note Must be positive.
constexpr std::size_t kLog2Modulus = 14;

static_assert(kLog2Modulus > 0, "Worker thread capacity must be positive.");

constexpr std::size_t kModulus = 1 << kLog2Modulus;

/// \brief  Provides O(1) access to the Worker that is handling the current
///   function (if any). Used to provide a fast path for scheduling within the
///   ThreadPool.
thread_local Worker * current_worker = nullptr;

struct ThreadPoolImpl
{
  typedef typename ThreadPool::task_type task_type;
  typedef std::chrono::steady_clock clock;
  typedef std::pair<clock::time_point, task_type> timed_task;

  ThreadPoolImpl (Worker *, unsigned);
  ~ThreadPoolImpl (void);

  template<typename Task>
  void schedule_overflow (Task &&);

  template<typename Task>
  void schedule_after (const clock::duration &, Task &&);

  unsigned get_concurrency (void) const;

  std::mutex mutex_;
 private:
  ThreadPoolImpl (const ThreadPoolImpl &) = delete;
  ThreadPoolImpl & operator= (const ThreadPoolImpl &) = delete;

  std::condition_variable cv_;
  std::queue<task_type> queue_;

  struct TaskOrder {
    inline bool operator() (const timed_task & lhs, const timed_task & rhs) const
    {
      return lhs.first > rhs.first;
    }
  };
  std::priority_queue<timed_task, std::vector<timed_task>, TaskOrder> time_queue_;
 public:
  void notify_if_idle (void)
  {
    if (idle_ > 0)
      cv_.notify_one();
  }
  bool might_have_task (void) const
  {
    return !queue_.empty();
  }
//  Note: Does no synchronization of its own.
  bool has_task (void) const
  {
    return !queue_.empty();
  }
//  Note: Does no synchronization of its own.
  std::size_t size (void) const
  {
    return queue_.size();
  }
//  Note: Does no synchronization of its own.
  void update_tasks (void)
  {
    while ((!time_queue_.empty()) && (clock::now() >= time_queue_.top().first))
    {
      queue_.emplace(std::move(time_queue_.top().second));
      time_queue_.pop();
    }
  }
//  Note: Does no synchronization of its own.
  task_type extract_task (void)
  {
    assert(!queue_.empty());
    task_type result = std::move(queue_.front());
    queue_.pop();
    return std::move(result);
  }

  template<typename Task>
  void push (Task && task)
  {
    queue_.push(std::forward<Task>(task));
  }

  template<typename Task>
  void push_at (const clock::time_point & tp, Task && task)
  {
    time_queue_.push(timed_task{tp, std::forward<Task>(task)});
  }


  void wait_for_task (std::unique_lock<decltype(mutex_)> & lk)
  {
    assert(lk.mutex() == &mutex_);
    if (time_queue_.empty())
      cv_.wait(lk);
    else
      cv_.wait_until(lk, time_queue_.top().first);
  }

  unsigned idle_, threads_;
  std::atomic<unsigned> living_;
  std::atomic<bool> stop_;

  Worker * workers_;

  friend struct Worker;
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

  Worker (ThreadPoolImpl &);
  ~Worker (void);

  void operator() (void);
  void kill_thread (void)
  {
    thread_.join();
  }

  inline bool belongs_to (ThreadPoolImpl * ptr) const
  {
    return &pool_ == ptr;
  }

  template<typename Task>
  bool push (Task && tasks);
  template<typename Task>
  bool push_front (Task && tasks);

 private:
  Worker (const Worker &) = delete;
  Worker & operator= (const Worker &) = delete;

  typedef uint_fast32_t index_type;

  //constexpr static std::size_t kModulus = 1 << kLog2Modulus;
  constexpr static std::size_t kValidShift = CHAR_BIT * sizeof(index_type) / 2;
  constexpr static index_type kWriteMask = ~(~static_cast<index_type>(0) << kValidShift);
  static_assert(kLog2Modulus <= kValidShift, "ThreadPool's local task queue \
size exceeds limit of selected index type.");

  inline static constexpr index_type get_distance (index_type, index_type);
  //inline static constexpr index_type get_writeable (index_type, index_type);
  inline static constexpr index_type get_valid (index_type);
  inline static constexpr index_type get_write (index_type);
  inline static constexpr index_type make_back (index_type, index_type);
  inline static constexpr index_type make_back (index_type);


  unsigned steal (void);
  unsigned steal_from (Worker & source, unsigned divisor);
  bool pop (task_type & task);
  unsigned push_front(ThreadPoolImpl &, unsigned number);
  bool execute (void);
  index_type count_tasks (void) const;
  void refresh_tasks (ThreadPoolImpl &, unsigned number);

  template<typename Task>
  void place_task (index_type location, Task && task)
  {
    task_type * task_array = reinterpret_cast<task_type *>(tasks_);
    new(task_array + location) task_type(std::forward<Task>(task));
  }
  task_type remove_task (index_type location)
  {
    task_type * task_array = reinterpret_cast<task_type *>(tasks_);
    assert(task_array[location] != nullptr);
    task_type result = std::move(task_array[location]);
    task_array[location].~task_type();
    return std::move(result);
  }

  template<typename Func>
  void remove_all_and (Func && func);

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
//  task_type tasks_ [kModulus];
  char tasks_ [kModulus * sizeof(task_type)];
};

Worker::Worker (ThreadPoolImpl & pool)
  : front_(0), back_(0), countdown_(2), front_invalid_(false), pool_(pool),
    thread_(std::reference_wrapper<Worker>(*this))
{
}

//    Removes each task from a Worker and applies func to it. Note: Must
//  not be called before the Worker's thread is fully stopped.
template<typename Func>
void Worker::remove_all_and (Func && func)
{
  index_type front = front_.load(std::memory_order_relaxed);
  index_type back = back_.load(std::memory_order_relaxed);

//    For safety, block stealing during this. Note: Won't block the worker that
//  is being destroyed.
  do {
    back = make_back(get_valid(back), get_valid(back));
    if (back_.compare_exchange_weak(back, make_back(1, 0),
                                    std::memory_order_acquire,
                                    std::memory_order_relaxed))
     break;
  } while (true);

//  If the worker is running a task, something is VERY wrong.
  assert(!front_invalid_);

  back = get_valid(back);

  front = front_.load(std::memory_order_acquire);
  while (front != back) {
    back = (back - 1 + kModulus) % kModulus;
    std::forward<Func>(func)(remove_task(back));
  }
  back_.store(make_back(front,front), std::memory_order_release);
}

//  Only called after all workers have stopped.
Worker::~Worker (void)
{
//    If this assert fails, either synchronization wasn't performed, or a task
//  is actively running. Either way, the code would need a fix.
  assert(!front_invalid_);

//    Remove tasks without using them in any way.
  remove_all_and([](task_type&&){});
}

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

//  Maximum spin count before giving up.
  uint_fast8_t spins = 64;
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

    if (source.back_.compare_exchange_weak(source_back,
                                           make_back(source_write, source_valid),
                                           std::memory_order_acq_rel,
                                           std::memory_order_relaxed))
      break;
//  Spun too long. Better to try a different victim than lock forever.
    if (--spins == 0)
      return 0;
  } while (true);
//    Now that the lock has been acquired, read may advance at most one more
//  time. That is, simply ensuring that READ < WRITE will suffice to ensure
//  correct behavior. Unfortunately, the READ may already be in the claim. Only
//  READ <= VALID is certain until we enforce it.
//    Note that by including the one-increment error margin, the following
//  adjustment needs to be run at most once.
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
//    Thief's number of held tasks can only be reduced since last check, so
//  there is no reason to double-check whether thief can hold the tasks.
      source_write = (source_valid - stolen + kModulus) % kModulus;
//    This store is optional. It allows the victim queue to keep executing
//  while memory is copied.
      source.back_.store(make_back(source_write, source_valid),
                         std::memory_order_relaxed);
    }
  }

#ifndef NDEBUG
  assert(source_write != source_valid);
  source_front = source.front_.load(std::memory_order_relaxed);
  assert(get_distance(source_front, source_write) <= get_distance(source_front, source_valid));
#endif
  do {
    source_valid = (source_valid - 1 + kModulus) % kModulus;
    this_front = (this_front - 1 + kModulus) % kModulus;
    place_task(this_front, source.remove_task(source_valid));
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
  task = remove_task(front);

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

  task_type task = remove_task(front);

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
void Worker::refresh_tasks (ThreadPoolImpl & tasks, unsigned number)
{
  unsigned pushed = push_front(tasks, number);
  if (pushed == 0)
  {
    auto cnt = tasks.size();
    if (number > cnt)
      number = cnt;
    task_type task;

    for (; number && pop(task); ++pushed, --number)
      tasks.push(std::move(task));
    push_front(tasks, pushed);
  }
}

//    Pushes a task onto the back of the queue, if possible. If the back of the
//  queue is in contention, (eg. because of work stealing), pushes onto the
//  front of the queue instead.
//    Note: Only evaluates the task reference if there is room to insert the
//  task.
template<typename Task>
bool Worker::push (Task && task)
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
    place_task(write, std::forward<Task>(task));
    back_.store(make_back(new_back), std::memory_order_release);
  }
  else
  {
    write = front;
    front = (front - 1 + kModulus) % kModulus;
    if (!front_invalid_)
      write = front;
    place_task(write, std::forward<Task>(task));
    front_.store(front, std::memory_order_release);
  }

  pool_.notify_if_idle();
  return true;
}

//    Places a new task at the front of the queue. Note that this skirts anti-
//  starvation precautions.
//    Note: Only evaluates the task reference if there is room to insert the
//  task.
template<typename Task>
bool Worker::push_front (Task && task)
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
  place_task(write, std::forward<Task>(task));
  front_.store(front, std::memory_order_release);

//    Delay lower-level (central) queue from being accessed, to fully support
//  depth-first traversal of task tree.
  ++countdown_;
  pool_.notify_if_idle();
  return true;
}

//    Places multiple new tasks at the front of the queue. Note that this skirts
//  anti-starvation precautions.
unsigned Worker::push_front (ThreadPoolImpl & tasks, unsigned number)
{
  index_type front, back, written, n;

//  Worker::push_front may only be called from the Worker's owned thread.
  assert(std::this_thread::get_id() == thread_.get_id());
  if (!tasks.has_task())
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
    //assert(tasks.front() != nullptr);
    place_task(front, std::move(tasks.extract_task()));
    if (!tasks.has_task())
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

//  Attempts to steal work from other worker threads in the same pool.
unsigned Worker::steal (void)
{
  //return 0;
  unsigned source = front_.load(std::memory_order_relaxed);
  std::hash<typename std::thread::id> hasher;
  source += hasher(thread_.get_id());
  auto num_threads = pool_.get_concurrency();
  unsigned count = 0;
  for (auto n = num_threads; n--;) {
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
  static constexpr std::size_t kPullFromQueue = (kModulus + 31) / 32;
  index_type last_size = 0;
//    This thread-local variable allows O(1) scheduling (allows pushing directly
//  to the local task queue).
  current_worker = this;
  pool_.living_.fetch_add(1, std::memory_order_relaxed);
  {
    std::unique_lock<decltype(pool_.mutex_)> guard(pool_.mutex_);
    pool_.cv_.notify_all();
    ++pool_.idle_;
    ThreadPoolImpl * ptr = &pool_;
    pool_.cv_.wait(guard, [ptr] (void) -> bool {
      return (ptr->living_.load(std::memory_order_relaxed) == ptr->threads_) ||
              ptr->stop_.load(std::memory_order_relaxed);
    });
    --pool_.idle_;
  }
  while (true)
  {
    if (--countdown_ == 0)
    {
      typedef decltype(pool_.mutex_) mutex_type;
      mutex_type & mutex = pool_.mutex_;

      index_type size = count_tasks();
      if (size >= kModulus)
        size = kModulus - 1;
      countdown_ = size + 2;

//    Periodically check whether the program is trying to destroy the pool.
      if (pool_.stop_.load(std::memory_order_relaxed))
        goto kill;

      if (mutex.try_lock())
      {
        std::lock_guard<mutex_type> guard (mutex, std::adopt_lock);
        pool_.update_tasks();
        if (!pool_.has_task())
        {
//    If the queue size has stabilized, it's likely that all tasks are waiting
//  on something (and thus continually re-adding themselves). Shake things up a
//  bit by re-shuffling tasks.
          if (size == last_size)
            size += steal();
          last_size = size;
          continue;
        }
        refresh_tasks(pool_, (kPullFromQueue + 3) / 4);
        countdown_ += kPullFromQueue / 2;
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
//  Make sure we don't exhaust the full queue when an exit is desired.
    if (pool_.stop_.load(std::memory_order_acquire))
      goto kill;
//    Third, check whether there are common tasks available. This will also
//  serve to jump-start the worker.
    typedef decltype(pool_.mutex_) mutex_type;
    mutex_type & mutex = pool_.mutex_;
//    Testing whether the task queue is empty may give an incorrect result,
//  due to lack of synchronization, but is still a fast and easy test.
    if (pool_.might_have_task() && mutex.try_lock())
    {
      std::lock_guard<mutex_type> guard (mutex, std::adopt_lock);
      pool_.update_tasks();
      unsigned count = push_front(pool_, kPullFromQueue);
      if (count > 0)
      {
//  If our new tasks are already from the queue, no need to refresh.
        countdown_ += kPullFromQueue;//count;
        continue;
      }
    }
//    Fourth, try work stealing.
    if (steal() > 0)
      continue;

//  Fifth, wait a bit for something to change...
    auto num_threads = pool_.get_concurrency();
    bool should_idle = (count_tasks() == 0);
    for (auto n = num_threads; n && should_idle; --n)
    {
      Worker * victim = pool_.workers_ + n;
      should_idle = (victim->count_tasks() < 2);
    }
    if (should_idle && mutex.try_lock())
    {
      std::unique_lock<mutex_type> guard (mutex, std::adopt_lock);
      if (pool_.stop_.load(std::memory_order_acquire))
        goto kill;
      pool_.update_tasks();
      if (!pool_.has_task())
      {
        ++pool_.idle_;
        pool_.wait_for_task(guard);
        --pool_.idle_;
        if (pool_.stop_.load(std::memory_order_relaxed))
          goto kill;
        pool_.update_tasks();
      }
      push_front(pool_, kPullFromQueue);
//  If our new tasks are already from the queue, no need to refresh.
      countdown_ += kPullFromQueue;
    }
    else
      std::this_thread::yield();
  }
kill:
  current_worker = nullptr;
  pool_.living_.fetch_sub(1, std::memory_order_acq_rel);
}



ThreadPoolImpl::ThreadPoolImpl (Worker * workers, unsigned threads)
  : mutex_(), cv_(), queue_(), time_queue_(),
    idle_(0), threads_(threads),
    living_(0), stop_(false),
    workers_(workers)
{
  for (unsigned i = 0; i < threads_; ++i)
    new(workers + i) Worker(*this);
//  Wait for the pool to be fully populated to ensure no weird behaviors.
  {
    std::unique_lock<decltype(mutex_)> guard (mutex_);
    cv_.wait(guard, [this](void)->bool {
      return (living_.load(std::memory_order_relaxed) == threads_) ||          \
             stop_.load(std::memory_order_relaxed);
    });
  }
}

ThreadPoolImpl::~ThreadPoolImpl (void)
{
  stop_.store(true, std::memory_order_release);
//    Wake all worker threads. Do so while holding the lock to ensure that all
//  potentially-idling threads are woken.
  {
    std::lock_guard<decltype(mutex_)> guard (mutex_);
    if (idle_ > 0)
      cv_.notify_all();
  }
  for (unsigned i = 0; i < threads_; ++i)
    workers_[i].kill_thread();
  for (unsigned i = 0; i < threads_; ++i)
    workers_[i].~Worker();
}

unsigned ThreadPoolImpl::get_concurrency(void) const
{
  return threads_;
}

template<typename Task>
void ThreadPoolImpl::schedule_overflow (Task && task)
{
  std::lock_guard<decltype(mutex_)> guard (mutex_);
  //queue_.emplace(std::forward<Task>(task));
  push(std::forward<Task>(task));
  notify_if_idle();
}

template<typename Task>
void ThreadPoolImpl::schedule_after (const clock::duration & dur, Task && task)
{
  std::lock_guard<decltype(mutex_)> guard (mutex_);
  push_at(clock::now() + dur, std::forward<Task>(task));
//    Wake the waiters, just in case the scheduled time is earlier than that for
//  which they were waiting.
  notify_if_idle();
}

} //  Namespace [Anonymous]

unsigned ThreadPool::get_concurrency(void) const
{
  return static_cast<ThreadPoolImpl*>(impl_)->threads_;
}

bool ThreadPool::is_idle (void) const
{
  ThreadPoolImpl * impl = static_cast<ThreadPoolImpl*>(impl_);
  std::lock_guard<decltype(impl->mutex_)> guard (impl->mutex_);
  return impl->idle_ > 0;
}

namespace {
#ifndef NDEBUG
std::atomic_flag overflow_warning_given = ATOMIC_FLAG_INIT;
void debug_warn_overflow (void)
{
  if (!overflow_warning_given.test_and_set())
    std::printf("Task queue overflow (more than %llu tasks in a single worker's \
queue). May impact performance.", ThreadPool::get_worker_capacity());
}
#else
inline void debug_warn_overflow (void) { }
#endif

template<typename Task>
void impl_schedule (Task && task, ThreadPoolImpl * impl)
{
  Worker * worker = current_worker;
//  If a thread is attempting to schedule in its own pool...
  if ((worker != nullptr) && worker->belongs_to(impl))
  {
    if (worker->push(std::forward<Task>(task)))
      return;
    else
      debug_warn_overflow();
  }
  impl->schedule_overflow<Task>(std::forward<Task>(task));
}

//  Schedule at the front of the queue, if in fast path.
template<typename Task>
void impl_schedule_subtask (Task && task, ThreadPoolImpl * impl)
{
  Worker * worker = current_worker;
//  If a thread is attempting to schedule in its own pool, take the fast path.
  if ((worker != nullptr) && worker->belongs_to(impl))
  {
    if (worker->push_front(std::forward<Task>(task)))
      return;
    else
      debug_warn_overflow();
  }
  impl->schedule_overflow(std::forward<Task>(task));
}

template<typename Task>
void impl_schedule_after (const std::chrono::steady_clock::duration & dur,
                       Task && task, ThreadPoolImpl * impl)
{
  if (dur <= std::chrono::steady_clock::duration(0))
    impl_schedule(task, impl);
  else
    impl->schedule_after<Task>(dur, std::forward<Task>(task));
}
}

//  Schedules a task normally, at the back of the queue.
void ThreadPool::schedule (const task_type & task)
{
  impl_schedule(task, static_cast<ThreadPoolImpl*>(impl_));
}
void ThreadPool::schedule (task_type && task)
{
  impl_schedule(task, static_cast<ThreadPoolImpl*>(impl_));
}

//  Schedules a task normally, at the back of the queue.
void ThreadPool::sched_impl(const duration & dur, const task_type & task)
{
  impl_schedule_after(dur, task, static_cast<ThreadPoolImpl*>(impl_));
}
void ThreadPool::sched_impl(const duration & dur, task_type && task)
{
  impl_schedule_after(dur, task, static_cast<ThreadPoolImpl*>(impl_));
}

//  Schedule at the front of the queue, if in fast path.
void ThreadPool::schedule_subtask (const task_type & task)
{
  impl_schedule_subtask(task, static_cast<ThreadPoolImpl*>(impl_));
}
void ThreadPool::schedule_subtask (task_type && task)
{
  impl_schedule_subtask(task, static_cast<ThreadPoolImpl*>(impl_));
}

std::size_t ThreadPool::get_worker_capacity (void)
{
  return kModulus - 1;
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
  std::size_t space = sizeof(ThreadPoolImpl) + threads * sizeof(Worker) +      \
                 alignof(ThreadPoolImpl) + alignof(Worker) + sizeof(void**);
  void * memory = std::malloc(space);
  if (memory == nullptr)
    goto fail_l1;
  {
    using std::align;
    void * ptr = memory;

//  Allocate space for a block of worker threads
    if (!align(alignof(Worker), threads * sizeof(Worker), ptr, space))
      goto fail_l2;
    Worker * workers = static_cast<Worker*>(ptr);
    ptr = workers + threads;

//  Allocate space for the controller.
    if (!align(alignof(ThreadPoolImpl), sizeof(ThreadPoolImpl), ptr, space))
      goto fail_l2;
    ThreadPoolImpl * impl = static_cast<ThreadPoolImpl*>(ptr);
    ptr = impl + 1;

    impl_ = impl;
    new(impl) ThreadPoolImpl(workers, threads);

    *reinterpret_cast<void**>(ptr) = memory;
  }
  return;
fail_l2:
  std::free(memory);
fail_l1:
  throw std::bad_alloc();
}

ThreadPool::~ThreadPool (void)
{
  ThreadPoolImpl * impl = static_cast<ThreadPoolImpl*>(impl_);
  void * memory = *reinterpret_cast<void**>(impl->workers_ + impl->threads_);
  impl->~ThreadPoolImpl();
  std::free(memory);
}
