/// \file   threadpool.cpp
/// \brief  Implements \ref `threadpool.hpp`.
/// \author Nathaniel J. McClatchey, PhD
/// \copyright Copyright (c) 2017 Nathaniel J. McClatchey, PhD.               \n
///   Licensed under the MIT license.                                         \n
///   You should have received a copy of the license with this software.
/// \note   To compile for MinGW-w64 without linking against the *winpthreads*
/// library, use the [*MinGW Windows STD Threads* library](https://github.com/nmcclatchey/mingw-std-threads).
#include "threadpool.hpp"

#if !defined(__cplusplus) || (__cplusplus < 201103L)
#error The implementation of ThreadPool requires C++11 or higher.
#endif

#include <atomic>             //  For atomic indexes, etc.
#include <queue>              //  For central task queue
#include <cstdint>            //  Fixed-width integer types.

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
#include <limits>             //  For std::numeric_limits

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

/// \brief  Least-significant bit of an integer. Useful for alignment of arrays,
///   because an alignment greater than the L.S.B. of the size of an element
///   will be ruined on increment.
template<class Integer>
constexpr Integer lsb (Integer x) noexcept
{
  return ((x - 1) & x) ^ x;
}

/// \brief  Checks whether and integer is a power-of-2. Useful for alignment
///   debugging.
template<class Integer>
constexpr Integer is_pow2 (Integer x) noexcept
{
  return ((x - 1) & x) == 0;
}

static_assert(is_pow2(THREAD_POOL_FALSE_SHARING_ALIGNMENT), "Alignments must be\
 integer powers of 2.");

/// \brief  Exactly what it says on the tin. I'd use `std::min`, but that's not
///   `constexpr` until C++14.
template<class Integer1, class Integer2>
constexpr typename std::common_type<Integer1, Integer2>::type min (Integer1 x, Integer2 y) noexcept
{
  typedef typename std::common_type<Integer1, Integer2>::type result_type;
  return (static_cast<result_type>(x) < static_cast<result_type>(y)) ? x : y;
}

/// \brief  Exactly what it says on the tin. I'd use `std::max`, but that's not
///   `constexpr` until C++14.
template<class Integer1, class Integer2>
constexpr typename std::common_type<Integer1, Integer2>::type max (Integer1 x, Integer2 y) noexcept
{
  typedef typename std::common_type<Integer1, Integer2>::type result_type;
  return (static_cast<result_type>(x) < static_cast<result_type>(y)) ? y : x;
}

/// \brief  Provides O(1) access to the Worker that is handling the current
///   function (if any). Used to provide a fast path for scheduling within the
///   ThreadPool.
thread_local Worker * current_worker = nullptr;

struct ThreadPoolImpl
{
  typedef typename ThreadPool::task_type task_type;
  typedef std::chrono::steady_clock clock;
  typedef std::pair<clock::time_point, task_type> timed_task;
  typedef std::uint_fast16_t index_type;

  ThreadPoolImpl (Worker *, index_type);
  ~ThreadPoolImpl (void);

//  Returns number of allocated Workers (may differ from active workers later)
  inline index_type get_capacity (void) const noexcept
  {
    return threads_;
  }

  inline index_type get_concurrency (void) const noexcept
  {
    return threads_;
  }

  void halt (void);
  void resume (void);
  bool is_halted (void) const;

  template<typename Task>
  void schedule_overflow (Task &&);

  template<typename Task>
  void schedule_after (const clock::duration &, Task &&);

  bool is_idle (void) const;

  inline bool should_stop (void) const noexcept
  {
    return stop_.load(std::memory_order_relaxed) & 0x01;
  }


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


  void wait_for_task (std::unique_lock<std::mutex> & lk)
  {
    assert(lk.mutex() == &mutex_);
    if (time_queue_.empty())
      cv_.wait(lk);
    else
      cv_.wait_until(lk, time_queue_.top().first);
  }

  inline Worker * data (void) noexcept
  {
    return workers_;
  }
 private:
  struct TaskOrder {
    inline bool operator() (const timed_task & lhs, const timed_task & rhs) const
    {
      return lhs.first > rhs.first;
    }
  };

  std::condition_variable cv_;
  mutable std::mutex mutex_;

  std::queue<task_type> queue_;
  std::priority_queue<timed_task, std::vector<timed_task>, TaskOrder> time_queue_;

  Worker * const workers_;

  index_type threads_, living_, idle_, paused_;

  std::atomic<std::uint_fast8_t> stop_;

  ThreadPoolImpl (const ThreadPoolImpl &) = delete;
  ThreadPoolImpl & operator= (const ThreadPoolImpl &) = delete;

  void stop_threads (std::unique_lock<std::mutex>&);

  friend struct Worker;
//  friend struct ThreadPool;
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

  Worker (ThreadPoolImpl &) noexcept;
  ~Worker (void);

  void operator() (void);

  void restart_thread (void)
  {
    assert(!pool_.should_stop());
    if (!thread_.joinable())
      thread_ = std::thread(std::reference_wrapper<Worker>(*this));
  }
  void stop_thread (void)
  {
    assert(pool_.should_stop());
    if (thread_.joinable())
      thread_.join();
  }

  inline bool belongs_to (ThreadPoolImpl * ptr) const noexcept
  {
    return &pool_ == ptr;
  }

  inline bool get_paused (void) const noexcept
  {
    return paused_;
  }

  inline void set_paused (bool val) noexcept
  {
    paused_ = val;
  }

  template<typename Task>
  bool push (Task && tasks);
  template<typename Task>
  bool push_front (Task && tasks);

 private:
  Worker (const Worker &) = delete;
  Worker & operator= (const Worker &) = delete;

  typedef std::uint_fast32_t index_type;

  //constexpr static std::size_t kModulus = 1 << kLog2Modulus;
  constexpr static std::size_t kValidShift = std::numeric_limits<index_type>::digits / 2;
  constexpr static index_type kWriteMask = ~(~static_cast<index_type>(0) << kValidShift);
  static_assert(kLog2Modulus <= kValidShift,                                   \
    "ThreadPool's local task queue size exceeds limit of selected index type.");

  inline static index_type get_distance (index_type, index_type) noexcept;
  //inline static constexpr index_type get_writeable (index_type, index_type);
  inline static index_type get_valid (index_type) noexcept;
  inline static index_type get_write (index_type) noexcept;
  inline static index_type make_back (index_type, index_type) noexcept;

  inline static index_type make_back (index_type) noexcept;


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
//    When this Worker runs out of tasks, it will search for more. A central
//  ThreadPool object will serve to coordinate work-stealing (that is, store the
//  addresses of other Workers), provide new tasks, and capture overflow should
//  a Worker generate more tasks than can fit in its deque.
  ThreadPoolImpl & pool_;
//    To avoid starvation for tasks in the overflow queue, I pull in its tasks
//  once every time a worker finishes a batch of tasks. The variable countdown_
//  records the remaining size of the batch. A successfully scheduled subtask
//  will increment this to ensure the originally scheduled tasks are completed
//  as part of the batch.
  static_assert(kLog2Modulus < std::numeric_limits<std::uint_fast32_t>::digits - 2,
"The behavior of the worker queue's starvation-avoidance algorithm has not yet \
been examined in the case that the countdown variable is small relative to the \
task-queue.");
  std::uint_fast32_t  countdown_ : (std::numeric_limits<std::uint_fast32_t>::digits - 2),
//    While a task is being executed, the front_ marker is not incremented. This
//  avoids early claiming of a new task (which would prevent that task from
//  being stolen), but makes the push-to-front process a bit more complicated.
//  In particular, the push-to-front should overwrite the front when first
//  called during an execution, but not afterward.
                      front_invalid_ : 1,
                      paused_ : 1;
//    Need to keep the thread's handle for later joining. I could work around
//  this, but the workaround would be less efficient.
  std::thread thread_;
//    Task queue. When information about the cache is available, allocate so
//  that tasks aren't split across cache lines. Note: If splitting is
//  inevitable, make a best-effort attempt to reduce it.
  alignas(max(alignof(task_type), min(lsb(sizeof(task_type)),\
THREAD_POOL_FALSE_SHARING_ALIGNMENT))) char tasks_ [kModulus*sizeof(task_type)];
};

Worker::Worker (ThreadPoolImpl & pool) noexcept
  : front_(0), back_(0),
    pool_(pool),
    countdown_(2), front_invalid_(false), paused_(false),
    thread_()
{
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

typename Worker::index_type Worker::get_distance (index_type left, index_type right) noexcept
{
  return (right - left + kModulus) % kModulus;
}

typename Worker::index_type Worker::get_valid (index_type b) noexcept
{
  return b >> kValidShift;
}

typename Worker::index_type Worker::get_write (index_type b) noexcept
{
  static_assert((kWriteMask >> kValidShift) == 0, "WRITE and VALID regions must not intersect.");
  return b & kWriteMask;
}

typename Worker::index_type Worker::make_back (index_type write, index_type valid) noexcept
{
  return write | (valid << kValidShift);
}

typename Worker::index_type Worker::make_back (index_type write) noexcept
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
  std::uint_fast8_t spins = 64;
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
    Worker * victim = pool_.data() + source;
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
  typedef decltype(pool_.mutex_) mutex_type;
  mutex_type & mutex = pool_.mutex_;

  {
    std::unique_lock<mutex_type> guard(mutex);
    ++pool_.living_;
    guard.unlock();
    pool_.cv_.notify_all();
  }
//  The thread is started after all workers are initialized; no need to wait.

  while (true)
  {
    if (--countdown_ == 0)
    {
      index_type size = count_tasks();
      if (size >= kModulus)
        size = kModulus - 1;
      countdown_ = size + 2;

//    Periodically check whether the program is trying to destroy the pool.
      if (pool_.should_stop())
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
    if (pool_.should_stop())
      goto kill;
//    Third, check whether there are common tasks available. This will also
//  serve to jump-start the worker.
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
    for (auto n = num_threads; n-- && should_idle;)
    {
      Worker * victim = pool_.workers_ + n;
      should_idle = (victim->count_tasks() < 2);
    }
    if (should_idle && mutex.try_lock())
    {
      std::unique_lock<mutex_type> guard (mutex, std::adopt_lock);
      if (pool_.should_stop())
        goto kill;
      pool_.update_tasks();
      if (!pool_.has_task())
      {
        ++pool_.idle_;
        pool_.wait_for_task(guard);
        --pool_.idle_;
        if (pool_.should_stop())
          goto kill;
        pool_.update_tasks();
      }
      push_front(pool_, kPullFromQueue);
//  If our new tasks are already from the queue, no need to refresh.
      countdown_ += kPullFromQueue;
    }
  }
kill:
  current_worker = nullptr;
  {
    std::unique_lock<mutex_type> guard (mutex);
    --pool_.living_;
    guard.unlock();
    pool_.cv_.notify_all();
  }
}



////////////////////////////////////////////////////////////////////////////////
//                              ThreadPoolImpl                                //
////////////////////////////////////////////////////////////////////////////////

ThreadPoolImpl::ThreadPoolImpl (Worker * workers, index_type threads)
  : cv_(), mutex_(),
    queue_(), time_queue_(),
    workers_(workers),
    threads_(threads), living_(0), idle_(0), paused_(0),
    stop_(0x00)
{
  std::unique_lock<decltype(mutex_)> guard (mutex_);
  for (index_type i = 0; i < threads_; ++i)
    new(workers + i) Worker(*this);
//    Start the threads only after all initialization is complete. The Worker's
//  loop will need no further synchronization for safe use.
  for (index_type i = 0; i < threads_; ++i)
    workers_[i].restart_thread();
//  Wait for the pool to be fully populated to ensure no weird behaviors.
  cv_.wait(guard, [this](void)->bool {
    return (living_ == threads_) || should_stop();
  });
}

ThreadPoolImpl::~ThreadPoolImpl (void)
{
#ifndef NDEBUG
  if ((current_worker != nullptr) && current_worker->belongs_to(this))
  {
    std::printf("ERROR!\tA worker thread may not destroy the ThreadPool to \
which it belongs.\n");
    std::abort();
  }
#endif
  std::unique_lock<decltype(mutex_)> guard (mutex_);
  stop_.store(0x05, std::memory_order_relaxed);
  if (paused_ > 0)
  {
//    If the pool is in a "paused" state, it might be the case that one thread
//  is still alive (and waiting for an "unpause" signal). Wake it up...
    cv_.notify_all();
    cv_.wait(guard, [this](void)->bool {
      return (living_ == 0);
    });
    for (unsigned i = 0; i < threads_; ++i)
      workers_[i].stop_thread();
  } else
    stop_threads(guard);

  for (unsigned i = 0; i < threads_; ++i)
    workers_[i].~Worker();
}

//  Note: Because of the mutex, can be called from any thread at any time.
void ThreadPoolImpl::stop_threads (std::unique_lock<decltype(mutex_)> & guard)
{
  if (idle_ > 0)
    cv_.notify_all();
  cv_.wait(guard, [this](void)->bool {
    return (living_ == paused_) || !should_stop();
  });
  if (should_stop()) {
//    At this point, all threads are either dead (need to be joined) or paused
//  (must not be joined). Take action appropriately.
//    Note that if multiple threads are paused simultaneously, they all reach
//  this point (one at a time, though)
    for (unsigned i = 0; i < threads_; ++i)
    {
      if (!workers_[i].get_paused())
        workers_[i].stop_thread();
    }
  }
}

void ThreadPoolImpl::halt (void)
{
  std::unique_lock<decltype(mutex_)> guard (mutex_);
//    Note: Bit 0x04 is used to indicate that the destructor is ongoing. Do not
//  interfere with it.
  if (stop_.load(std::memory_order_relaxed) & 0x04)
    return;
  stop_.store(0x03, std::memory_order_relaxed);
  if ((current_worker != nullptr) && current_worker->belongs_to(this))
  {
    current_worker->set_paused(true);
    ++paused_;
  }
  stop_threads(guard);
//  If the caller is part of the pool, block execution until unpaused.
  if ((current_worker != nullptr) && current_worker->belongs_to(this))
  {
    cv_.wait(guard, [this] (void) -> bool {
      return (stop_.load(std::memory_order_relaxed) & 0x02) == 0;
    });
    current_worker->set_paused(false);
    --paused_;
  }
}

//  Note: Because of the mutex, can call from any thread at any time.
void ThreadPoolImpl::resume (void)
{
  std::unique_lock<decltype(mutex_)> guard (mutex_);

  assert(living_ >= paused_);
//    Note: Bit 0x04 will be used to indicate attempted destruction. Do not
//  interfere.
  if (stop_.load(std::memory_order_relaxed) & 0x04)
    return;

  stop_.store(0x00, std::memory_order_relaxed);
  cv_.notify_all();

  for (unsigned i = 0; i < threads_; ++i)
    workers_[i].restart_thread();

  cv_.wait(guard, [this](void)->bool {
    return (living_ == threads_) || should_stop();
  });
}

bool ThreadPoolImpl::is_halted (void) const
{
  std::lock_guard<decltype(mutex_)> guard (mutex_);
//  Include paused tasks to give more consistent behavior.
  return (stop_.load(std::memory_order_relaxed) & 0x02) && (paused_ == living_);
}

bool ThreadPoolImpl::is_idle (void) const
{
  std::lock_guard<decltype(mutex_)> guard (mutex_);
//  Include paused tasks to give more consistent behavior.
  return (idle_ + paused_) == living_;
}

template<typename Task>
void ThreadPoolImpl::schedule_overflow (Task && task)
{
  std::lock_guard<decltype(mutex_)> guard (mutex_);
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

namespace {
#ifndef NDEBUG
std::atomic_flag overflow_warning_given = ATOMIC_FLAG_INIT;
void debug_warn_overflow (void)
{
  if (!overflow_warning_given.test_and_set())
    std::printf("Task queue overflow (more than %zu tasks in a single worker's \
queue). May impact performance.", ThreadPool::get_worker_capacity());
}
#endif

template<typename Task>
void impl_schedule (Task && task, ThreadPoolImpl * impl)
{
#ifndef NDEBUG
//    If a NULL task is passed, place the error message as close as possible to
//  the error itself.
  if (task == nullptr)
    throw std::bad_function_call();
#endif
  Worker * worker = current_worker;
//  If a thread is attempting to schedule in its own pool...
  if ((worker != nullptr) && worker->belongs_to(impl))
  {
    if (worker->push(std::forward<Task>(task)))
      return;
#ifndef NDEBUG
    else
      debug_warn_overflow();
#endif
  }
  impl->schedule_overflow<Task>(std::forward<Task>(task));
}

//  Schedule at the front of the queue, if in fast path.
template<typename Task>
void impl_schedule_subtask (Task && task, ThreadPoolImpl * impl)
{
#ifndef NDEBUG
//    If a NULL task is passed, place the error message as close as possible to
//  the error itself.
  if (task == nullptr)
    throw std::bad_function_call();
#endif
  Worker * worker = current_worker;
//  If a thread is attempting to schedule in its own pool, take the fast path.
  if ((worker != nullptr) && worker->belongs_to(impl))
  {
    if (worker->push_front(std::forward<Task>(task)))
      return;
#ifndef NDEBUG
    else
      debug_warn_overflow();
#endif
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
  {
#ifndef NDEBUG
//    If a NULL task is passed, place the error message as close as possible to
//  the error itself.
    if (task == nullptr)
      throw std::bad_function_call();
#endif
    impl->schedule_after<Task>(dur, std::forward<Task>(task));
  }
}
}





////////////////////////////////////////////////////////////////////////////////
//                                ThreadPool                                  //
////////////////////////////////////////////////////////////////////////////////

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
  typedef decltype(static_cast<ThreadPoolImpl*>(impl_)->get_concurrency()) thread_counter_type;
  if (threads > std::numeric_limits<thread_counter_type>::max())
    threads = std::numeric_limits<thread_counter_type>::max();
//    Alignment change during Worker allocation is an integer multiple of
//  alignof(Worker). If (alignof(Worker) >= alignof(ThreadPoolImpl)), then
//  the second align will not do anything, and the problem is solved. Otherwise,
//  Alignment is off by at most alignof(ThreadPoolImpl) - alignof(Worker).
//    Total alignment is off by at most the greater of the alignments.
  std::size_t space = sizeof(ThreadPoolImpl) + threads * sizeof(Worker) +      \
                 max(alignof(ThreadPoolImpl), alignof(Worker)) + sizeof(void**);
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
  void * memory = *reinterpret_cast<void**>(impl + 1);
  impl->~ThreadPoolImpl();
  std::free(memory);
}

unsigned ThreadPool::get_concurrency(void) const
{
  return static_cast<const ThreadPoolImpl *>(impl_)->get_concurrency();
}

bool ThreadPool::is_idle (void) const
{
  return static_cast<const ThreadPoolImpl *>(impl_)->is_idle();
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

void ThreadPool::halt (void)
{
  static_cast<ThreadPoolImpl*>(impl_)->halt();
}
void ThreadPool::resume (void)
{
  static_cast<ThreadPoolImpl*>(impl_)->resume();
}
bool ThreadPool::is_halted (void) const
{
  return static_cast<const ThreadPoolImpl *>(impl_)->is_halted();
}
