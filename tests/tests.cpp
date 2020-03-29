//  Include this first to check for missed dependencies.
#include "../threadpool.hpp"

#if (!defined(__MINGW32__) || defined(_GLIBCXX_HAS_GTHREADS))
#include <thread>
#include <mutex>
#include <condition_variable>
#else
#include <mingw.thread.h>
#include <mingw.mutex.h>
#include <mingw.condition_variable.h>
#endif
#include <cassert>
#include <cstdio>
#include <atomic>
#include <cstdint>

#define LOG(fmtString,...) printf(fmtString "\n", ##__VA_ARGS__); fflush(stdout)

using namespace std;

namespace
{
constexpr size_t kTestMaxThreads = 1024;
constexpr size_t kTestRootTasks = 1000;
constexpr size_t kTestBranchFactor = 800;
constexpr uint_fast64_t kTestTotalTasks = kTestRootTasks * kTestBranchFactor;

thread_local std::atomic<uint_fast64_t> * task_slot_local = nullptr;
std::atomic<uint_fast32_t> task_slot_next(0);
std::atomic<uint_fast64_t> executed_tasks [kTestMaxThreads * 64];

void perform_task (void)
{
  if (task_slot_local == nullptr)
  {
    auto n = task_slot_next.fetch_add(1, std::memory_order_relaxed);
    assert(n < kTestMaxThreads);
    task_slot_local = executed_tasks + (n * 64 / sizeof(*executed_tasks));
  }
  task_slot_local->fetch_add(1, std::memory_order_release);
}

std::condition_variable cv;
std::mutex mtx;

bool one_is_active = false;
size_t alive_count = 0;
void stay_active (ThreadPool & pool)
{
  {
    std::lock_guard<decltype(mtx)> lk (mtx);
    one_is_active = true;
    /*++alive_count;
    if ((alive_count & (alive_count + 1)) == 0)
      std::printf("Alive, %llu\n", alive_count);*/
    cv.notify_all();
  }
  pool.schedule([&pool](void){ stay_active(pool); });
}

void gather_statistics  (uint_fast64_t & balance_min,
                         uint_fast64_t & balance_max,
                         uint_fast64_t & balance_total)
{
  balance_min = ~static_cast<uint_fast64_t>(0);
  balance_max = balance_total = 0;
  for (uint_fast32_t n = 0; n < task_slot_next.load(std::memory_order_acquire); ++n)
  {
    auto it = executed_tasks[n * 8].load(std::memory_order_relaxed);
    if (balance_max < it)
      balance_max = it;
    if (balance_min > it)
      balance_min = it;
    balance_total += it;
  }
}
}

int main()
{
  using namespace std::literals::chrono_literals;
  int test_id = 0;
  {
    LOG("Test %u:\t%s",++test_id,"Query static information");
    LOG("\tWorker queue capacity is %zu tasks.",ThreadPool::get_worker_capacity());
  }
  {
    LOG("Test %u:\t%s",++test_id,"Construct and destroy empty threadpool.");
    {
      ThreadPool pool;
      LOG("\t%s","Construct successful.");
    }
    LOG("\t%s","Destroy successful.");
  }
  std::atomic<int> logged_errors {0};
  {
    LOG("Test %u:\t%s", ++test_id, "Disallow null function pointers.");
    LOG("\t%s","Constructing a thread pool.");
    ThreadPool pool;
    LOG("\t\tDone.\tNote: Pool has %u worker threads.", pool.get_concurrency());

    pool.schedule([&](void)
    {
      try {
        std::function<void()> null_func;
        pool.schedule_subtask(null_func);
        logged_errors |= 8;
      } catch (std::bad_function_call &) {}
      try {
        pool.schedule_subtask(std::function<void()>());
        logged_errors |= 8;
      } catch (std::bad_function_call &) {}
    });
    try {
      std::function<void()> null_func;
      pool.schedule(null_func);
      logged_errors |= 8;
    } catch (std::bad_function_call &) {}
    try {
      pool.schedule(std::function<void()>());
      logged_errors |= 8;
    } catch (std::bad_function_call &) {}
    try {
      std::function<void()> null_func;
      pool.schedule_after(1s, null_func);
      logged_errors |= 8;
    } catch (std::bad_function_call &) {}
    try {
      pool.schedule_after(1s, std::function<void()>());
      logged_errors |= 8;
    } catch (std::bad_function_call &) {}
    LOG("\t%s", "Destroying the thread pool.");
  }

  {
    LOG("Test %u:\t%s",++test_id,"Use threadpool for tasks.");
    LOG("\t%s","Constructing a thread pool.");
    ThreadPool pool;
    LOG("\t\tDone.\tNote: Pool has %u worker threads.", pool.get_concurrency());
    for (unsigned nn = 0; nn < 2; ++nn)
    {
      LOG("\t%s","Resetting task-recording utilities...");
      for (unsigned i = 0; i < 8 * 64; ++i)
        executed_tasks[i].store(0, std::memory_order_release);
      bool already_idling = false;

      LOG("\tScheduling some %s tasks...", (nn == 0) ? "immediate" : "delayed");
      pool.schedule([&](void)
      {
        for (unsigned i = 0; i < kTestRootTasks / 2; ++i)
        {
          pool.schedule_after(std::chrono::seconds(nn), [&](void)
          {
            for (unsigned j = 0; j < kTestBranchFactor; ++j)
            {
              pool.schedule_subtask(&perform_task);
            }
          });
        }
        for (unsigned i = kTestRootTasks / 2; i < kTestRootTasks; ++i)
        {
          std::function<void(void)> lvalue_task ( [&](void)
          {
            for (unsigned j = 0; j < kTestBranchFactor; ++j)
            {
              pool.schedule_subtask(&perform_task);
            }
          } );
          pool.schedule_after(std::chrono::seconds(nn), lvalue_task);
        }
      });
      LOG("\t\t%s","Done. Tasks scheduled successfully.");
      LOG("\t%s","Waiting a bit while tasks complete...");

      unsigned total_ms = 0;
      for (unsigned ii = 0; ii < 9; ++ii)
      {
        using namespace std::chrono;
        unsigned sleep_ms = (100u << ii);
        std::this_thread::sleep_for(milliseconds(sleep_ms));
        total_ms += sleep_ms;

        LOG("\t\t%s","Checking whether tasks are completed...");
        uint_fast64_t balance_min, balance_max, balance_total;
        gather_statistics(balance_min, balance_max, balance_total);
        LOG("\t\tCompleted %llu / %llu tasks so far.", static_cast<long long unsigned>(balance_total), static_cast<long long unsigned>(kTestTotalTasks));
        if (pool.is_idle() && (balance_total == kTestTotalTasks))
        {
          gather_statistics(balance_min, balance_max, balance_total);
          LOG("\tPool has idled, as expected, with all %llu tasks complete.", static_cast<long long unsigned>(kTestTotalTasks));
          LOG("\tProcessor utilization [min / mean / max]:\t%llu / %llu / %llu", static_cast<long long unsigned>(balance_min), static_cast<long long unsigned>(balance_total / pool.get_concurrency()), static_cast<long long unsigned>(balance_max));
          break;
        }
        else if (balance_total == kTestTotalTasks)
        {
          if (already_idling)
          {
            LOG("\t%s","Pool has not yet idled, despite all tasks being complete; this is probably an error.");
            logged_errors |= 1;
          } else
            already_idling = true;
        }
      }
      if (!pool.is_idle())
      {
        LOG("\t\tPool failed to complete all tasks after %u seconds. There is probably an error.", total_ms / 1000);
        logged_errors |= 2;
      }
    }
    LOG("\t%s", "Destroying the thread pool.");
  }

  {
    LOG("Test %u:\t%s",++test_id,"Destroy a ThreadPool with running task-chains.");
    LOG("\t%s","Constructing a thread pool.");
    ThreadPool pool (2);
    LOG("\t\tDone.\tNote: Pool has %u worker threads.", pool.get_concurrency());
    LOG("\t%s", "Scheduling several undying tasks...");
    {
      std::unique_lock<decltype(mtx)> guard (mtx);
      one_is_active = false;
      for (unsigned n = 0; n < 16; ++n)
        pool.schedule([&pool](void) { stay_active(pool); });
      cv.wait(guard, [](void)->bool { return one_is_active; });
    }
    LOG("\t\t%s","Done. Tasks are running.");
    LOG("\t%s", "Destroying the thread pool.");
  }

  {
    LOG("Test %u:\t%s",++test_id,"Pause and resume a ThreadPool with running task-chains.");
    LOG("\t%s","Constructing a thread pool.");
    ThreadPool pool (3);
    LOG("\t\tDone.\tNote: Pool has %u worker threads.", pool.get_concurrency());
    LOG("\t%s", "Scheduling several undying tasks...");
    {
      std::unique_lock<decltype(mtx)> guard (mtx);
      one_is_active = false;
      alive_count = 0;
      for (unsigned n = 0; n < 16; ++n)
        pool.schedule([&pool](void) { stay_active(pool); });
      cv.wait(guard, [](void)->bool { return one_is_active; });
    }
    LOG("\t\t%s","Done. Tasks scheduled successfully.");
    LOG("\t%s","Pausing...");
    pool.halt();
    LOG("\t%s","Waiting for a bit...");
    std::this_thread::sleep_for(250ms);
    if (pool.is_halted())
    {
      LOG("\t%s", "Pool did pause.");
    }
    else
    {
      LOG("\t%s", "Pool did not pause. This is most unusual!");
      logged_errors |= 4;
    }
    LOG("\t%s","Unpausing...");
    pool.resume();
    LOG("\t%s","Waiting for 0.3 seconds...");
    std::this_thread::sleep_for(300ms);
    LOG("\t%s", "Destroying the thread pool.");
  }

  {
    LOG("Test %u:\t%s",++test_id,"Destroy a paused Threadpool.");
    LOG("\t%s","Constructing a thread pool.");
    ThreadPool pool (5);
    LOG("\t\tDone.\tNote: Pool has %u worker threads.", pool.get_concurrency());
    LOG("\t%s", "Scheduling several undying tasks...");
    {
      std::unique_lock<decltype(mtx)> guard (mtx);
      one_is_active = false;
      alive_count = 0;
      for (unsigned n = 0; n < 16; ++n)
        pool.schedule([&pool](void) { stay_active(pool); });
      cv.wait(guard, [](void)->bool { return one_is_active; });
    }
    LOG("\t\t%s","Done. Tasks scheduled successfully.");
    LOG("\t%s","Pausing...");
    pool.halt();
    while (!pool.is_halted())
      std::this_thread::sleep_for(50ms);
    LOG("\t%s", "Destroying the thread pool.");
  }

  {
    LOG("Test %u:\t%s",++test_id,"Attempt to pause from within a worker thread.");
    LOG("\t%s","Constructing a thread pool.");
    ThreadPool pool;
    LOG("\t\tDone.\tNote: Pool has %u worker threads.", pool.get_concurrency());
    LOG("\t%s", "Scheduling a few tasks, including a pausing task.");
    {
      std::unique_lock<decltype(mtx)> guard (mtx);
      one_is_active = false;
      alive_count = 0;
      for (unsigned n = 0; n < 16; ++n)
        pool.schedule([&pool](void) { stay_active(pool); });
      pool.schedule([&pool](void) { stay_active(pool); pool.halt(); });
      cv.wait(guard, [](void)->bool { return one_is_active; });
    }
    LOG("\t\t%s","Done. Tasks scheduled successfully.");
    LOG("\t\t%s","Done. Waiting for a bit...");
    std::this_thread::sleep_for(250ms);
    LOG("\t%s","Unpausing...");
    pool.resume();
    LOG("\t\t%s","Done. Waiting for a bit...");
    std::this_thread::sleep_for(250ms);
    LOG("\t%s", "Destroying the thread pool.");
  }

  {
    LOG("Test %u:\t%s",++test_id,"Attempt to pause from within a worker thread, and then destroy the pool.");
    LOG("\t%s","Constructing a thread pool.");
    ThreadPool pool;
    LOG("\t\tDone.\tNote: Pool has %u worker threads.", pool.get_concurrency());
    LOG("\t%s", "Scheduling a few tasks, including a pausing task.");
    {
      std::unique_lock<decltype(mtx)> guard (mtx);
      one_is_active = false;
      alive_count = 0;
      for (unsigned n = 0; n < 16; ++n)
        pool.schedule([&pool](void) { stay_active(pool); });
      pool.schedule([&pool](void) { stay_active(pool); pool.halt(); pool.halt(); pool.halt(); });
      cv.wait(guard, [](void)->bool { return one_is_active; });
    }
    LOG("\t\t%s","Done. Tasks scheduled successfully.");
    LOG("\t\t%s","Done. Waiting for a bit second...");
    std::this_thread::sleep_for(250ms);
    LOG("\t%s", "Destroying the thread pool.");
  }
  LOG("%s", "Exiting...");
  return logged_errors;
}

