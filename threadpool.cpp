/// \file   threadpool.cpp
/// \brief  Implements `threadpool.hpp`.
/// \author Nathaniel J. McClatchey, PhD
/// \copyright Copyright (c) 2017-2019 Nathaniel J. McClatchey, PhD.          \n
///   Licensed under the MIT license.                                         \n
///   You should have received a copy of the license with this software.
/// \note   To compile for MinGW-w64 without linking against the *winpthreads*
/// library, use the [*MinGW Windows STD Threads* library](https://github.com/meganz/mingw-std-threads "MinGW STD Threads").
#include "threadpool.hpp"

#if !defined(__cplusplus) || (__cplusplus < 201103L)
#error The implementation of ThreadPool requires C++11 or higher.
#endif

//  Debugging:
#include <cassert>            //  Fail deadly on internal library error.
#ifndef NDEBUG
#include <cstdio>             //  Warn on task queue overflow.
#endif
//  Memory management (for allocate-once approach):
#include <cstdlib>            //  For std::malloc and std::free.
#include <memory>             //  For std::align and std::unique_ptr.
#if (__cplusplus >= 201703L) && !defined(THREAD_POOL_FALSE_SHARING_ALIGNMENT)
#include <new>                //  Used to detect cache size.
#endif
//  Integers:
#include <cstdint>            //  Fixed-width integer types.
#include <atomic>             //  Relaxed memory orderings, for efficiency.
#include <limits>             //  Type sizes and maximum values.
//  Central queue management:
#include <algorithm>          //  Delayed-task sorting.
#include <vector>             //  Delayed-task storage.
#include <deque>              //  For central task queue.
//  Miscellaneous type information:
#include <type_traits>        //  Detect conditions needed for noexcept.
#include <utility>            //  For std::declval

//  Threading facilities:
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

namespace {
#ifdef THREAD_POOL_FALSE_SHARING_ALIGNMENT
//  If a user has supplied a false-sharing alignment, use it.
constexpr std::size_t kFalseSharingAlignment = THREAD_POOL_FALSE_SHARING_ALIGNMENT;
#elif defined(__cpp_lib_hardware_interference_size) && (__cpp_lib_hardware_interference_size >= 201703L)
constexpr std::size_t kFalseSharingAlignment = std::hardware_destructive_interference_size;
#else
//  No hints? Use a typical cache line size.
constexpr std::size_t kFalseSharingAlignment = 64;
#endif
//  Forward-declarations
struct Worker;
struct ThreadPoolImpl;

/// \brief  Determines the capacity of each `Worker`'s queue. Larger values take
///   more memory, but less processing power. The reverse holds for smaller
///   values.
/// \note Must be positive.
constexpr std::uint_fast8_t kLog2Modulus = 12u;

static_assert(kLog2Modulus > 0, "Worker thread capacity must be positive.");

constexpr std::uint_fast32_t kModulus = 1ull << kLog2Modulus;

static_assert(kLog2Modulus < std::numeric_limits<decltype(kModulus)>::digits, "Worker thread capacity must not be excessive.");

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
constexpr bool is_pow2 (Integer x) noexcept
{
  return ((x - 1) & x) == 0;
}

/// \brief  Checks whether (n1 > n2) || (n1 == 0). Clang optimizes this, while
///   GCC does not (even in 9.0)
template<class Integer>
inline constexpr bool greater_or_zero (Integer n1, Integer n2) noexcept
{
  //  return (n1 > n2) || (n1 == 0);
  static_assert(std::numeric_limits<Integer>::is_signed == false,
                "This optimization depends on using unsigned comparison.");
  return (n1 - 1u >= n2);
}

static_assert(is_pow2(kFalseSharingAlignment),
              "Alignments must be integer powers of 2.");

/// \brief  Exactly what it says on the tin. I'd use `std::min`, but that's not
///   `constexpr` until C++14.
template<class In1, class In2>
constexpr typename std::common_type<In1, In2>::type min (In1 x, In2 y) noexcept
{
  using result_type = decltype(min(x,y));
  return (static_cast<result_type>(x) < static_cast<result_type>(y)) ? x : y;
}

/// \brief  Exactly what it says on the tin. I'd use `std::max`, but that's not
///   `constexpr` until C++14.
template<class In1, class In2>
constexpr typename std::common_type<In1, In2>::type max (In1 x, In2 y) noexcept
{
  using result_type = decltype(max(x,y));
  return (static_cast<result_type>(x) < static_cast<result_type>(y)) ? y : x;
}

/// \brief  Determines an alignment that minimizes the number of times that a
///   densely-packed array of `T` would have an instance of `T` straddling a
///   cache-line border.
template<class T>
constexpr std::size_t get_align (void)
{
  return max(alignof(T), min(lsb(sizeof(T)), kFalseSharingAlignment));
}

/// \brief  Destructor that allows `std::unique_ptr` to be used with memory
///   acquired using `malloc`.
struct RawDeleter
{
  void operator() (void * ptr) const
  {
    std::free(ptr);
  }
};

/// \brief  Provides O(1) access to the Worker that is handling the current
///   function (if any). Used to provide a fast path for scheduling within the
///   ThreadPool.
thread_local Worker * current_worker = nullptr;

struct ThreadPoolImpl
{
  using task_type   = typename ThreadPool::task_type;
  using clock       = std::chrono::steady_clock;
  using timed_task  = std::pair<clock::time_point, task_type>;
  using index_type  = std::uint_fast16_t;

  ThreadPoolImpl (Worker *, index_type);
  ~ThreadPoolImpl (void);

//  Returns number of allocated Workers (may differ from active workers later)
  inline index_type get_capacity (void) const noexcept
  {
    return num_workers_;
  }

  inline index_type get_concurrency (void) const noexcept
  {
    return num_threads_.load(std::memory_order_relaxed);
  }

  void halt (void);
  void resume (void);
  bool is_halted (void) const;

  template<typename Task>
  void schedule_overflow (Task &&);

  template<typename Task>
  void schedule_after (clock::duration const &, Task &&);

  bool is_idle (void) const;

  inline bool should_stop (void) const noexcept
  {
    return stop_.load(std::memory_order_relaxed) & 0x01;
  }


  inline void notify_if_idle (void) noexcept
  {
    if (idle_ > 0)
      cv_.notify_one();
  }
  inline bool might_have_task (void) const noexcept
  {
    return !queue_.empty();
  }
//  Note: Does no synchronization of its own.
  inline bool has_task (void) const noexcept
  {
    return !queue_.empty();
  }
//  Note: Does no synchronization of its own.
  inline std::size_t size (void) const noexcept
  {
    return queue_.size();
  }
//  Note: Does no synchronization of its own.
  void update_tasks (void)
  {
    if (time_queue_.empty())
      return;
    auto time_now = clock::now();

    while (time_now >= time_queue_.front().first)
    {
//    If an exception was thrown, it was thrown in `push`. Because of the strong
//  exception-safety guarantee, nothing actually happens.
      try {
        push(std::move(time_queue_.front().second));
      } catch (std::bad_alloc &) {
        return;
      }
//  The pop_back method for a vector should be non-throwing.
      std::pop_heap(time_queue_.begin(), time_queue_.end(), TaskOrder{});
      time_queue_.pop_back();

      if (time_queue_.empty())
        break;
    }
  }
//  Note: Does no synchronization of its own.
  task_type extract_task (void)
  {
    assert(!queue_.empty() && "Cannot retrieve a task from an empty queue.");
    task_type result = std::move(queue_.front());
    queue_.pop_front();
    return result;
  }

/// \par  Exception safety
///   Provides the strong (rollback) guarantee, even with move semantics.
  template<typename Task>
  inline void push (Task && task)
  {
    queue_.push_back(std::forward<Task>(task));
  }

/// \par  Exception safety
///   Provides the strong (rollback) guarantee unless the task can only be moved
/// and has a throwing move constructor.
  template<typename Task>
  inline void push_at (clock::time_point const & tp, Task && task)
  {
    //time_queue_.push(timed_task{tp, std::forward<Task>(task)});
    time_queue_.push_back(timed_task{tp, std::forward<Task>(task)});
    TaskOrder comp;
    std::push_heap(time_queue_.begin(), time_queue_.end(), comp);
  }

//  Note: wait and wait_until don't throw in C++14 and later.
  void wait_for_task (std::unique_lock<std::mutex> & lk)
  {
    assert(lk.mutex() == &mutex_ &&"Incorrect mutex used for synchronization.");
    if (time_queue_.empty())
      cv_.wait(lk);
    else
      cv_.wait_until(lk, time_queue_.front().first);
  }

  inline Worker * data (void) noexcept
  {
    return workers_;
  }
 private:
  struct TaskOrder {
    inline bool operator() (timed_task const & lhs, timed_task const & rhs) const
    {
      return lhs.first > rhs.first;
    }
  };

  std::condition_variable cv_ {};
  mutable std::mutex mutex_ {};

  std::deque<task_type> queue_ {};
  std::vector<timed_task> time_queue_ {};

  Worker * const workers_;

  index_type num_workers_ {0},
              living_ {0}, idle_ {0}, paused_ {0};
  std::atomic<index_type> num_threads_ {0};

  std::atomic<std::uint_fast8_t> stop_ {0x00};

  ThreadPoolImpl (ThreadPoolImpl const &) = delete;
  ThreadPoolImpl & operator= (ThreadPoolImpl const &) = delete;

  void stop_threads (std::unique_lock<std::mutex>&);

  friend struct Worker;
};

//  Notes:
//  -   "front_" is always claimed for the worker.
//  -   "back_"  stores past-the-end markers both for writing and validity. If
//    they are unequal, the back is locked.
//  -   For various reasons, it is possible for the front marker to be between
//    the write and valid pte markers. In such a case, the already-claimed task
//    may be read, but no further tasks will be read, even if claimed.
struct alignas(kFalseSharingAlignment) Worker
{
  using task_type = typename ThreadPool::task_type;
  using index_type = std::uint_fast32_t;

  Worker (ThreadPoolImpl &) noexcept;
  ~Worker (void);

  void operator() (void);

  bool is_alive (void) const noexcept
  {
    return thread_.joinable();
  }

  void restart_thread (void)
  {
    assert(!pool_.should_stop() && "Start or stop new threads. Not both.");
    if (!thread_.joinable())  //  noexcept
    {
      thread_ = std::thread(std::reference_wrapper<Worker>(*this));
      pool_.num_threads_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  void stop_thread (void)
  {
    assert(pool_.should_stop() && "Spurious thread-stopping detected.");
    if (thread_.joinable()) //  noexcept
    {
      thread_.join();
      pool_.num_threads_.fetch_sub(1, std::memory_order_relaxed);
    }
  }

  inline bool belongs_to (ThreadPoolImpl const * ptr) const noexcept
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

  index_type count_tasks (void) const noexcept;

  void canibalize (ThreadPoolImpl &);

 private:
  Worker (Worker const &) = delete;
  Worker & operator= (Worker const &) = delete;

  constexpr static std::size_t kValidShift = std::numeric_limits<index_type>::digits / 2;
  constexpr static index_type kWriteMask = ~(~static_cast<index_type>(0) << kValidShift);
  static_assert(kLog2Modulus <= kValidShift,                                   \
    "ThreadPool's local task queue size exceeds limit of selected index type.");

  inline static constexpr
    index_type get_distance (index_type left, index_type right) noexcept
  {
    return (right - left + kModulus) % kModulus;
  }

  inline static constexpr index_type get_valid (index_type b) noexcept
  {
    return b >> kValidShift;
  }

  inline static constexpr index_type get_write (index_type b) noexcept
  {
    static_assert((kWriteMask >> kValidShift) == 0, "WRITE and VALID regions must not intersect.");
    return b & kWriteMask;
  }

  inline static constexpr
    index_type make_back (index_type write, index_type valid) noexcept
  {
    return write | (valid << kValidShift);
  }

  inline static constexpr index_type make_back (index_type write) noexcept
  {
    return write | (write << kValidShift);
  }


  unsigned steal (void);
  unsigned steal_from (Worker & source) noexcept(std::is_nothrow_destructible<task_type>::value && std::is_nothrow_move_constructible<task_type>::value);
  bool pop (task_type & task)           noexcept(std::is_nothrow_destructible<task_type>::value && std::is_nothrow_move_assignable<task_type>::value);
  unsigned push_front(ThreadPoolImpl &, unsigned number);
  bool execute (void);
  void refresh_tasks (ThreadPoolImpl &, unsigned number);

/// \brief  Activates a task slot within the queue, and fills it appropriately.
  template<typename Task>
  void place_task (index_type location, Task && task)
    noexcept(std::is_nothrow_constructible<task_type, Task &&>::value)
  {
    static_assert(std::is_trivially_destructible<OptionalTask::Empty>::value,
              "Implicit destruction is used here, and thus is required here.");
    new(std::addressof(tasks_[location].task_)) task_type(std::forward<Task>(task));
  }
/// \brief  Deactivates a task slot, and returns what was inside before the
///   deactivation.
  task_type remove_task (index_type location)
    noexcept(std::is_nothrow_destructible<task_type>::value)
  {
    task_type result = std::move(tasks_[location].task_);
    tasks_[location].task_.~task_type();
//  Set the new active member of the union. Should be a no-op.
    static_assert(std::is_trivial<OptionalTask::Empty>::value,
            "The default value for implicit optional values must be trivial.");
    tasks_[location].empty_ = OptionalTask::Empty();
    return result;
  }

  template<typename Func>
  void remove_all_and (Func const &);

//  These store information about the current state of the deque.
//  -   front_ is modified only by the Worker's own thread. Reads and writes
//    must be atomic, however, to avoid torn writes.
//  -   back_ is potentially modified by all threads. The top and bottom halves
//    store a past-the-end (PTE) marker for the occupied slots, and a PTE marker
//    for the slots this Worker is permitted to read, respectively.
  std::atomic<index_type> front_ {0}, back_ {0};
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
  std::uint_fast32_t  countdown_;
//    While a task is being executed, the front_ marker is not incremented. This
//  avoids early claiming of a new task (which would prevent that task from
//  being stolen), but makes the push-to-front process a bit more complicated.
//  In particular, the push-to-front should overwrite the front when first
//  called during an execution, but not afterward.
  bool front_invalid_;
  bool paused_;
//    Need to keep the thread's handle for later joining. I could work around
//  this, but the workaround would be less efficient.
  std::thread thread_ {};
//    Task queue. When information about the cache is available, allocate so
//  that tasks aren't split across cache lines. Note: If splitting is
//  inevitable, make a best-effort attempt to reduce it.
  union OptionalTask {
    struct Empty {} empty_;
    task_type task_;

    OptionalTask (void) noexcept : empty_() {}
    ~OptionalTask (void) noexcept {}
  };
  alignas(get_align<task_type>()) OptionalTask tasks_ [kModulus];
};

Worker::Worker (ThreadPoolImpl & pool) noexcept
  : pool_(pool),
    countdown_(2), front_invalid_(false), paused_(false)
{
}

//  Only called after all workers have stopped.
Worker::~Worker (void)
{
//    If this assert fails, either synchronization wasn't performed, or a task
//  is actively running. Either way, the code would need a fix.
  assert(!front_invalid_ && "Attempting to destroy a running worker!");

//    Remove tasks without using them in any way.
  remove_all_and([](task_type&&){});
}

//    Removes each task from a Worker and applies func to it. Note: Must
//  not be called before the Worker's thread is fully stopped.
/// \note Has exactly one possibly-throwing statement.
template<class Func>
void Worker::remove_all_and (Func const & func)
{
  index_type back = back_.load(std::memory_order_relaxed);

//    For safety, block stealing during this. Note: Won't block the worker that
//  is being destroyed.
  do {
    back = make_back(get_valid(back));
  } while (!back_.compare_exchange_weak(back, make_back(1, 0),
                    std::memory_order_acquire, std::memory_order_relaxed));

//  If the worker is running a task, something is VERY wrong.
  assert(!front_invalid_ && "The worker is still running a task!");

  back = get_valid(back);

  index_type front = front_.load(std::memory_order_acquire);
//  Ensure a consistent state, in the event of an exception.
  struct RAIIHelper
  {
    decltype(back_) & back_ref;
    index_type value;
    ~RAIIHelper (void)
    {
      back_ref.store(value, std::memory_order_release);
    }
  } raii_helper { back_, back };
  while (front != raii_helper.value)
  {
    raii_helper.value = (raii_helper.value - 1 + kModulus) % kModulus;
//  Possibly-throwing:
    func(remove_task(raii_helper.value));
  }
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
/// \note noexcept if place_task and remove_task are both noexcept.
unsigned Worker::steal_from (Worker & source)
    noexcept(std::is_nothrow_destructible<task_type>::value && std::is_nothrow_move_constructible<task_type>::value)
{
  static constexpr unsigned kDivisor = 4;
  index_type this_front,    this_back,    writeable, stolen,
              source_front, source_back,  source_valid, source_write;
//  Worker::steal_from may only be called from the Worker's owned thread.
  assert(std::this_thread::get_id() == thread_.get_id() && "Worker::steal_from may only be called from the Worker's own thread.");
  assert(this != &source && "Worker may not steal from itself.");
  assert(!front_invalid_ && "Worker cannot steal while it is performing a task.");

  this_front = front_.load(std::memory_order_relaxed);
  this_back = back_.load(std::memory_order_acquire);

  writeable = get_distance(get_valid(this_back), this_front - 1);
  if (writeable == 0)
    return 0;

//  Maximum number of times to attempt to lock the victim before giving up.
  std::uint_fast8_t spins = 64;
//  Lock the source queue, reserving several tasks to steal.
  source_back = source.back_.load(std::memory_order_relaxed);
  do {
    source_valid = get_valid(source_back);
//  Already locked. Better to give up immediately, and try a different victim.
    if (source_valid != get_write(source_back))
      return 0;
    source_front = source.front_.load(std::memory_order_relaxed);
//  Stolen is actually from WRITE, but WRITE and VALID are identical.
    index_type valid = get_distance(source_front, source_valid);
//    Must not attempt to claim the current front pointer, so require at least 2
//  items in source queue.
    if (valid < 2)
      return 0;
    stolen = min((valid + kDivisor - 2) / kDivisor, writeable);
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
    if (greater_or_zero(readable, valid))
    {
      stolen = (valid + kDivisor - 2) / kDivisor;
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
  auto test_front = source.front_.load(std::memory_order_relaxed);
  assert(get_distance(test_front, source_write) <= get_distance(test_front, source_valid));
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
  noexcept(std::is_nothrow_destructible<task_type>::value &&
           std::is_nothrow_move_assignable<task_type>::value)
{
  //assert(std::this_thread::get_id() == thread_.get_id() && "Worker::pop may only be called from the Worker's own thread.");

  auto front = front_.load(std::memory_order_relaxed);
  auto back = back_.load(std::memory_order_acquire);

  auto readable = get_distance(front, get_write(back));
//    Two circumstances can prevent reading: Either there is nothing to read, or
//  the current location is claimed. Even once the claim is resolved, there may
//  or may not be something to read.
  if (greater_or_zero(readable, get_distance(front, get_valid(back))))
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
  assert(!front_invalid_ && "Can't execute a task while already executing a different task.");
  assert(std::this_thread::get_id() == thread_.get_id() && "Worker::execute may only be called from the Worker's own thread.");

  auto front = front_.load(std::memory_order_relaxed);
  auto back = back_.load(std::memory_order_acquire);

  auto readable = get_distance(front, get_write(back));
//    Two circumstances can prevent reading: Either there is nothing to read, or
//  the current location is claimed. Even once the claim is resolved, there may
//  or may not be something to read.
  if (greater_or_zero(readable, get_distance(front, get_valid(back))))
    return false;

//    Will ensure that the queue is restored to validity, even in the event of
//  an exception.
  struct Reservation
  {
    Reservation (Worker & worker) noexcept
      : worker_(worker)
    {
      worker_.front_invalid_ = true;
    }
    ~Reservation (void)
    {
      if (worker_.front_invalid_)
      {
        worker_.front_invalid_ = false;
        auto new_front = worker_.front_.load(std::memory_order_relaxed);
        worker_.front_.store((new_front+1)%kModulus, std::memory_order_relaxed);
//  I need to release back_ so that the write to front_ is visible to thieves.
        worker_.back_.fetch_or(0, std::memory_order_release);
      }
    }
    Reservation (Reservation const &) = delete;
    Reservation & operator= (Reservation const &) = delete;
   private:
    Worker & worker_;
  } reservation {*this};
//  Potentially-throwing.
  task_type task = remove_task(front);
//  Potentially-throwing.
  task();
/// \todo Find a good way to unify this with the other validation.
//    If the slot was not already overwritten (eg. by the task pushing to the
//  task-queue), need to adjust the queue size.
  return true;
}

//    Pulls some tasks into the local queue from the central queue, and returns
//  others.
void Worker::refresh_tasks (ThreadPoolImpl & tasks, unsigned number)
{
  unsigned num_pushed = push_front(tasks, number);
  if (num_pushed == 0)
  {
    auto cnt = tasks.size();
    if (number > cnt)
      number = static_cast<unsigned>(cnt);
    task_type task;

    for (; number && pop(task); ++num_pushed, --number)
      tasks.push(std::move(task));
    push_front(tasks, num_pushed);
  }
}

//  Feeds all existing tasks to the ThreadPool. Used as a last resort.
void Worker::canibalize (ThreadPoolImpl & tasks)
{
  do {
    task_type task;
    if (pop(task))
      tasks.push(std::move(task));
    else
    {
      auto front = front_.load(std::memory_order_relaxed);
      auto back = back_.load(std::memory_order_relaxed);
//    If the queue is fully-depleted, our job is done. Otherwise, we need to
//  keep trying.
      if ((get_write(back) == get_valid(back)) && (get_valid(back) == front))
        break;
      else
        std::this_thread::yield();
    }
  } while (true);
}

//    Pushes a task onto the back of the queue, if possible. If the back of the
//  queue is in contention, (eg. because of work stealing), pushes onto the
//  front of the queue instead.
//    Note: Only evaluates the task reference if there is room to insert the
//  task.
/// \par  Exception safety
///   *Strong*: If an exception is thrown, the function has no effect.
///   Applies only if `place_task()` also provides the strong guarantee.
template<typename Task>
bool Worker::push (Task && task)
{
  assert(std::this_thread::get_id() == thread_.get_id() && "Worker::push may only be called from the Worker's owned thread.");

  auto front = front_.load(std::memory_order_relaxed);
  auto back = back_.load(std::memory_order_acquire);

  auto valid = get_valid(back);
  if (((front - valid + kModulus) % kModulus) == 1)
    return false;

  index_type write     = get_write(back);
  index_type new_back  = (write + 1) % kModulus;
  index_type expected  = make_back(write);
  if (back_.compare_exchange_strong(expected, make_back(write, new_back),
                                    std::memory_order_acquire,
                                    std::memory_order_relaxed))
  {
    struct RAIIHelper
    {
      decltype(back_) & back_ref;
      index_type value;
      ~RAIIHelper (void)
      {
        back_ref.store(value, std::memory_order_release);
      }
    } raii_helper { back_, back };
    place_task(write, std::forward<Task>(task));  //  May throw.
    raii_helper.value = make_back(new_back);
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
/// \par  Exception safety
///   *Strong*: If an exception is thrown, the function has no effect.
///   Applies only if `place_task()` also provides the strong guarantee.
template<typename Task>
bool Worker::push_front (Task && task)
{
  assert(std::this_thread::get_id() == thread_.get_id() && "Worker::push_front may only be called from the Worker's owned thread.");

  index_type front = front_.load(std::memory_order_relaxed);
  index_type back = back_.load(std::memory_order_acquire);

  if ((front - get_valid(back) + kModulus) % kModulus == 1)
    return false;
  index_type write = front;
  front = (front - 1 + kModulus) % kModulus;

//  Potentially-throwing
  place_task(front_invalid_ ? write : front, std::forward<Task>(task));

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
  assert(std::this_thread::get_id() == thread_.get_id() && "Worker::push_front may only be called from the Worker's owned thread.");
  if (!tasks.has_task())
    return 0;

  index_type front = front_.load(std::memory_order_relaxed);
  index_type back = back_.load(std::memory_order_acquire);

  auto written = (front - get_valid(back) - 1 + kModulus) % kModulus;
  if (number < written)
    written = number;
  if (written == 0)
    return 0;

//  In C++, bool converts implicitly to 0 (false) or 1 (true).
  front += front_invalid_;
  auto n = written;
  do {
    front = (front - 1 + kModulus) % kModulus;
    place_task(front, tasks.extract_task());
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
typename Worker::index_type Worker::count_tasks (void) const noexcept
{
  index_type front = front_.load(std::memory_order_relaxed);
  index_type back = back_.load(std::memory_order_relaxed);
  return get_distance(front, get_valid(back));
}

//  Attempts to steal work from other worker threads in the same pool.
unsigned Worker::steal (void)
{
  unsigned num_workers = pool_.get_capacity();
  auto randomizer = front_.load(std::memory_order_relaxed);
  unsigned source = static_cast<unsigned>(randomizer);
  unsigned stolen_count = 0;
  for (auto n = num_workers; n--;) {
    source = (source + 1) % num_workers;
    Worker * victim = pool_.data() + source;
    if (victim == this)
      continue;
    stolen_count += steal_from(*victim);
    if (stolen_count > 0)
      break;
  }
  return stolen_count;
}

//    Performs a loop of the form execute-steal-check_central_queue-repeat.
//  Sleeps if no work is available in this and other queues.
void Worker::operator() (void)
{
  static constexpr std::uint_fast32_t kPullFromQueue = 1 + (kModulus - 1) / 32;
  index_type last_size = 0;
//    This thread-local variable allows O(1) scheduling (allows pushing directly
//  to the local task queue).
  current_worker = this;
  using mutex_type = decltype(pool_.mutex_);
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
    auto num_workers = pool_.get_capacity();
    bool should_idle = (count_tasks() == 0);
    for (auto n = num_workers; n-- && should_idle;)
      should_idle = (pool_.workers_[n].count_tasks() < 2);
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

ThreadPoolImpl::ThreadPoolImpl (Worker * workers, index_type num_workers)
  : workers_(workers), num_workers_(num_workers)
{
  assert(num_workers > 0);
  std::unique_lock<decltype(mutex_)> guard (mutex_);

//  Construct the workers, after some safety-checks.
  static_assert(std::is_nothrow_constructible<Worker, ThreadPoolImpl &>::value,\
    "This loop is only exception-safe if Worker construction is non-throwing");
  for (index_type i = 0; i < get_capacity(); ++i)
    new(workers_ + i) Worker(*this);
//    Start the threads only after all initialization is complete. The Worker's
//  loop will need no further synchronization for safe use.
//    Note that a worker without an initialized thread will simply do nothing,
//  because the threads are responsible for populating themselves with tasks.
  std::exception_ptr eptr;
  for (index_type i = 0; i < get_capacity(); ++i)
  {
    try {
      workers_[i].restart_thread();
    } catch (std::system_error &) {
      eptr = std::current_exception();
    }
  }
//    If no threads were able to start, give a meaningful error regarding why.
//  However, if at least one thread was able to start, the ThreadPool will
//  function properly.
  if (get_concurrency() == 0)
    std::rethrow_exception(eptr);
//  Wait for the pool to be fully populated to ensure no weird behaviors.
  cv_.wait(guard, [this](void)->bool {
    return (living_ == get_concurrency()) || should_stop();
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
    for (auto i = get_capacity(); i--;)
      workers_[i].stop_thread();
  } else
    stop_threads(guard);

  for (auto i = get_capacity(); i--;)
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
    for (auto i = get_capacity(); i--;)
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
  cv_.notify_all(); //  noexcept

  std::exception_ptr eptr;
  for (unsigned i = 0; i < get_capacity(); ++i)
  {
    try {
      workers_[i].restart_thread();
    } catch (std::system_error &) {
//    Whenever a thread fails to start, remove all the tasks it would otherwise
//  need to consume. This will prevent those tasks from becoming unreachable.
      if (!workers_[i].is_alive())
        workers_[i].canibalize(*this);
      eptr = std::current_exception();
    }
  }
  if (get_concurrency() == 0)
    std::rethrow_exception(eptr);

  cv_.wait(guard, [this](void)->bool {
    return (living_ >= get_concurrency()) || should_stop();
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

/// \par Exception safety
///   Provides the strong (rollback) guarantee.
template<typename Task>
void ThreadPoolImpl::schedule_overflow (Task && task)
{
  std::lock_guard<decltype(mutex_)> guard (mutex_);
  push(std::forward<Task>(task)); // < Strong exception-safety guarantee.
  notify_if_idle();
}

/// \par Exception safety
///   Provides the strong (rollback) guarantee.
template<typename Task>
void ThreadPoolImpl::schedule_after (clock::duration const & dur, Task && task)
{
  std::lock_guard<decltype(mutex_)> guard (mutex_);
  push_at(clock::now() + dur, std::forward<Task>(task));
//    Wake the waiters, just in case the scheduled time is earlier than that for
//  which they were waiting.
  notify_if_idle();
}



#ifndef NDEBUG
void debug_warn_overflow (void) noexcept
{
  static std::atomic_flag overflow_warning_given = ATOMIC_FLAG_INIT;
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
void impl_schedule_after (std::chrono::steady_clock::duration const & dur,
                          Task && task, ThreadPoolImpl * impl)
{
  if (dur <= std::chrono::steady_clock::duration(0))
    impl_schedule(std::forward<Task>(task), impl);
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
} //  Namespace [anonymous]





////////////////////////////////////////////////////////////////////////////////
//                                ThreadPool                                  //
////////////////////////////////////////////////////////////////////////////////

ThreadPool::ThreadPool (unsigned threads)
  : impl_(nullptr)
{
  if (threads == 0)
  {
//    Hardware concurrency of 0 indicates that it is unknown. Make sure we have
//  a few threads running.
    threads = max(2, std::thread::hardware_concurrency());
  }
  using thread_counter_type = decltype(std::declval<ThreadPoolImpl>().get_concurrency());
  threads = min(threads, min(std::numeric_limits<thread_counter_type>::max(),
                             std::numeric_limits<unsigned>::max()));
//    Alignment change during Worker allocation is an integer multiple of
//  alignof(Worker). If (alignof(Worker) >= alignof(ThreadPoolImpl)), then
//  the second align will not do anything, and the problem is solved. Otherwise,
//  Alignment is off by at most alignof(ThreadPoolImpl) - alignof(Worker).
//    Total alignment is off by at most the greater of the alignments.
  std::size_t space = sizeof(ThreadPoolImpl) + threads * sizeof(Worker) +      \
                 max(alignof(ThreadPoolImpl), alignof(Worker)) + sizeof(void**);

  std::unique_ptr<void, RawDeleter> memory { std::malloc(space) };
  if (memory == nullptr)
    throw std::bad_alloc();
  void * ptr = memory.get();

  using std::align;
//  Allocate space for a block of worker threads
  if (!align(alignof(Worker), threads * sizeof(Worker), ptr, space))
    throw std::bad_alloc();
  Worker * workers = static_cast<Worker*>(ptr);
  ptr = workers + threads;

//  Allocate space for the controller.
  if (!align(alignof(ThreadPoolImpl), sizeof(ThreadPoolImpl), ptr, space))
    throw std::bad_alloc();
  ThreadPoolImpl * impl = static_cast<ThreadPoolImpl*>(ptr);
  ptr = impl + 1;

  new(impl) ThreadPoolImpl(workers, static_cast<thread_counter_type>(threads));

  impl_ = impl;
  *reinterpret_cast<void**>(ptr) = memory.release();
}

ThreadPool::~ThreadPool (void)
{
  ThreadPoolImpl * impl = static_cast<ThreadPoolImpl*>(impl_);
  std::unique_ptr<void,RawDeleter> memory {*reinterpret_cast<void**>(impl + 1)};
  impl->~ThreadPoolImpl();
}

unsigned ThreadPool::get_concurrency(void) const noexcept
{
  return static_cast<ThreadPoolImpl const*>(impl_)->get_concurrency();
}

bool ThreadPool::is_idle (void) const
{
  return static_cast<ThreadPoolImpl const*>(impl_)->is_idle();
}

//  Schedules a task normally, at the back of the queue.
void ThreadPool::schedule (task_type const & task)
{
  impl_schedule(task, static_cast<ThreadPoolImpl*>(impl_));
}
void ThreadPool::schedule (task_type && task)
{
  impl_schedule(std::move(task), static_cast<ThreadPoolImpl*>(impl_));
}

//  Schedules a task normally, at the back of the queue.
void ThreadPool::sched_impl(duration const & dur, task_type const & task)
{
  impl_schedule_after(dur, task, static_cast<ThreadPoolImpl*>(impl_));
}
void ThreadPool::sched_impl(duration const & dur, task_type && task)
{
  impl_schedule_after(dur, std::move(task),static_cast<ThreadPoolImpl*>(impl_));
}

//  Schedule at the front of the queue, if in fast path.
void ThreadPool::schedule_subtask (task_type const & task)
{
  impl_schedule_subtask(task, static_cast<ThreadPoolImpl*>(impl_));
}
void ThreadPool::schedule_subtask (task_type && task)
{
  impl_schedule_subtask(std::move(task), static_cast<ThreadPoolImpl*>(impl_));
}

std::size_t ThreadPool::get_worker_capacity (void) noexcept
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
  return static_cast<ThreadPoolImpl const*>(impl_)->is_halted();
}
