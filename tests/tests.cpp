#include "../thread_pool.hpp"

#include <mutex>
#if (!defined(__MINGW32__) || defined(_GLIBCXX_HAS_GTHREADS))
#include <thread>
#include <condition_variable>
#else
#include <mingw.thread.h>
#include <mingw.mutex.h>
#include <mingw.condition_variable.h>
#endif
#include <assert.h>
#include <map>

using namespace std;

thread_local uint_fast64_t executed_tasks = 0;

std::mutex completed_mutex;
std::condition_variable completed_cv;
std::map<typename std::thread::id, uint_fast64_t> completed_tasks;

void record_executed (ThreadPool & pool)
{
  typedef std::pair<typename decltype(completed_tasks)::iterator, bool> res;
  res it_bool;
  {
    std::lock_guard<decltype(completed_mutex)> guard (completed_mutex);
    it_bool = completed_tasks.insert(std::make_pair(std::this_thread::get_id(), executed_tasks));
    executed_tasks = 0;
  }
  assert(it_bool.second);

  std::unique_lock<decltype(completed_mutex)> guard (completed_mutex);
  if (completed_tasks.size() >= pool.get_concurrency())
    completed_cv.notify_all();
  else
    completed_cv.wait(guard, [&pool](void)->bool { return completed_tasks.size() >= pool.get_concurrency(); });
}

#define LOG(fmtString,...) printf(fmtString "\n", ##__VA_ARGS__); fflush(stdout)
int main()
{
  LOG("Constructing a thread pool.");
  ThreadPool pool;
  LOG("\tDone.");
  printf("\tPool has %u worker threads.\n", pool.get_concurrency());
  for (unsigned nn = 0; nn < 2; ++nn)
  {
    LOG("Starting a test.");
    completed_tasks.clear();
    LOG("Scheduling some tasks...");
    pool.schedule([&](void)
    {
      for (unsigned i = 0; i < 10000; ++i)
      {
        pool.schedule([&](void)
        {
          for (unsigned j = 0; j < 8000; ++j)
          {
            pool.schedule_subtask([&](void)
            {
              ++executed_tasks;
  //            if (cnt.fetch_add(1, std::memory_order_relaxed) + 1 == 10000 * 8000) t = glfwGetTimerValue() - t;
            });
          }
        });
      }
    });
    LOG("\tDone.");
    LOG("Waiting a bit, to give time for completion...");
    Sleep(1000);
    LOG("Scheduling a recording operation...");
    for (unsigned i = 0; i < pool.get_concurrency(); ++i)
    {
      pool.schedule([&](void)
      {
        record_executed(pool);
      });
    }
    LOG("\tDone.");
    LOG("Waiting until all threads record something...");
    {
      std::unique_lock<decltype(completed_mutex)> guard (completed_mutex);
      completed_cv.wait(guard, [&pool](void)->bool { return completed_tasks.size() >= pool.get_concurrency(); });
      LOG("\tDone.");
    }

    uint_fast64_t balance_min, balance_max, balance_total;
    balance_min = ~static_cast<uint_fast64_t>(0);
    balance_max = balance_total = 0;
    for (auto it = completed_tasks.begin(); it != completed_tasks.end(); ++it)
    {
      if (balance_max < it->second)
        balance_max = it->second;
      if (balance_min > it->second)
        balance_min = it->second;
      balance_total += it->second;
    }
    printf("Completed %u tasks\n", balance_total);
    printf("Processor utilization min / max / mean :\t%u / %u / %u\n", balance_min, balance_max, balance_total / pool.get_concurrency());

    for (unsigned ii = 0; ii < 10; ++ii)
    {
      Sleep(1000);
      if (pool.is_idle())
      {
        LOG("Pool has idled, as expected.");
        break;
      }
      else
      {
        if (balance_total == 80000000)
        {
          LOG("Pool has not yet idled; this is probably an error.");
        }
        else
          LOG("Pool has not yet idled; this is unexpected, but might be due to remaining tasks.");
      }
    }
  }
  return 0;
}

