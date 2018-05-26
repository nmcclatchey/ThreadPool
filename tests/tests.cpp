//  Include this first to check for missed dependencies.
#include "../threadpool.hpp"

#if (!defined(__MINGW32__) || defined(_GLIBCXX_HAS_GTHREADS))
#include <thread>
#else
#include <mingw.thread.h>
#endif
#include <cassert>
#include <cstdio>
#include <atomic>

#define LOG(fmtString,...) printf(fmtString "\n", ##__VA_ARGS__); fflush(stdout)

using namespace std;

constexpr size_t kTestMaxThreads = 1024;
constexpr size_t kTestRootTasks = 10000;
constexpr size_t kTestBranchFactor = 8000;
constexpr size_t kTestTotalTasks = kTestRootTasks * kTestBranchFactor;


void perform_task (void);
void gather_statistics  (uint_fast64_t & balance_min,
                         uint_fast64_t & balance_max,
                         uint_fast64_t & balance_total);

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

int main()
{
  int test_id = 0;
  {
    LOG("Test %u:\t%s",++test_id,"Query static information");
    LOG("\tWorker queue capacity is %llu tasks.",ThreadPool::get_worker_capacity());
  }
  {
    LOG("Test %u:\t%s",++test_id,"Construct and destroy empty threadpool.");
    {
      ThreadPool pool;
      LOG("\t%s","Construct successful.");
    }
    LOG("\t%s","Destroy successful.");
  }
  int logged_errors = 0;
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
        for (unsigned i = 0; i < kTestRootTasks; ++i)
        {
          pool.schedule_after(std::chrono::seconds(nn), [&](void)
          {
            for (unsigned j = 0; j < kTestBranchFactor; ++j)
            {
              pool.schedule_subtask(&perform_task);
            }
          });
        }
      });
      LOG("\t\t%s","Done. Tasks scheduled successfully.");
      LOG("\t%s","Waiting a bit while tasks complete...");

      unsigned total_ms = 0;
      for (unsigned ii = 0; ii < 9; ++ii)
      {
        using namespace std::chrono_literals;
        unsigned sleep_ms = (100 << ii);
        std::this_thread::sleep_for(1ms * sleep_ms);
        total_ms += sleep_ms;

        LOG("\t\t%s","Checking whether tasks are completed...");
        uint_fast64_t balance_min, balance_max, balance_total;
        gather_statistics(balance_min, balance_max, balance_total);
        LOG("\t\tCompleted %llu / %llu tasks so far.", balance_total, kTestTotalTasks);
        if (pool.is_idle() && (balance_total == kTestTotalTasks))
        {
          gather_statistics(balance_min, balance_max, balance_total);
          LOG("\tPool has idled, as expected, with all %llu tasks complete.", kTestTotalTasks);
          LOG("\tProcessor utilization [min / mean / max]:\t%llu / %llu / %llu", balance_min, balance_total / pool.get_concurrency(), balance_max);
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
  LOG("%s", "Exiting...");
  return logged_errors;
}

