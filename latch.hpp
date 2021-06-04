/// \file
/// \brief  Provides a `latch` class for synchronization, roughly equivalent to
///   C++20's `std::latch`.

#ifndef LATCH_HPP_
#define LATCH_HPP_

#if __cplusplus < 202002L
#include <atomic>
#include <cassert>
#include <limits>
#if defined(__MINGW32__) && !defined(_GLIBCXX_HAS_GTHREADS)
#include <mingw.condition_variable.h>
#include <mingw.mutex.h>
#else
#include <condition_variable>
#include <mutex>
#endif

/// \brief  Allow threads to wait until a selection of tasks is completed by
///   other threads.
///
///   `latch`es allow threads to wait for multiple tasks to be completed by
/// other threads. This is vital for applying the *fork-join* paradigm to
/// concurrency models that do not naturally supply a means of joining, such as
/// thread pools, and removes the need to store a tree of forked threads if one
/// employs a large number of worker threads.                                 \n
///   In a typical use-case, a `latch` is locked *n* times by thread *0*,
/// which then spawns worker threads *1, 2, ... n* and waits on the `latch`.
/// Each worker thread completes its task, then unlocks the `latch`. In this
/// example, thread *0* only progresses past the `latch` once all workers
/// complete their tasks.
class latch
{
  mutable std::condition_variable cv_ {};
  mutable std::mutex mutex_ {};
  std::atomic<int> wait_for_ {0};
 public:
/// \brief  Constructs a `latch`. Note: Diverges from `std::latch` in that it
///   is not constexpr.
  latch (std::ptrdiff_t expected = 1)
    : wait_for_(expected)
  {
  }

  ~latch (void)
  {
  }

  latch (const latch &) = delete;
  latch & operator= (const latch &) = delete;

/// \brief  Decreases the number of tasks remaining, and unlocks the `Barrier`
///   if no tasks remain.
  void count_down (std::ptrdiff_t n = 1)
  {
    assert(n >= 0);
    auto previously_waiting = wait_for_.fetch_sub(n, std::memory_order_release);
    assert(previously_waiting >= n);
    if (previously_waiting <= n)
    {
//    Using this mutex synchronizes with the awakened thread, ensuring that the
//  barrier is seen to be open.
      std::lock_guard<decltype(mutex_)> guard(mutex_);
      cv_.notify_all();
    }
  }

/// \brief  Returns `true` if tasks remain incomplete.
  inline bool try_wait (void) const noexcept
  {
    return wait_for_.load(std::memory_order_acquire) == 0;
  }

/// \brief  Blocks until no tasks remain incomplete.
  void wait (void) const
  {
    std::unique_lock<decltype(mutex_)> lck (mutex_);
    cv_.wait(lck, [this]()->bool { return try_wait(); });
  }

/// \brief  Counts down, then waits until no tasks remain.
  void arrive_and_wait (std::ptrdiff_t n = 1)
  {
    assert(n >= 0);
    auto previously_waiting = wait_for_.fetch_sub(n, std::memory_order_acq_rel);
    assert(previously_waiting >= n);
    std::unique_lock<decltype(mutex_)> lck(mutex_);
    if (previously_waiting <= n)
      cv_.notify_all();
    else
      cv_.wait(lck, [this]()->bool { return try_wait(); });
  }

  static constexpr std::ptrdiff_t max (void) noexcept
  {
    return std::numeric_limits<int>::max();
  }
};
#else
#include <latch>
using std::latch;
#endif

#endif // LATCH_HPP_
