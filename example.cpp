//  Include this first to check for missed dependencies.
#include "threadpool.hpp"
#include "latch.hpp"

#if (!defined(__MINGW32__) || defined(_GLIBCXX_HAS_GTHREADS))
#include <thread>
#else
#include <mingw.thread.h>
#endif
#include <iostream>
#include <atomic>
#include <chrono>

int main()
{
  ThreadPool pool;
  latch continuation_guard (1024);
  std::atomic<int> counter {0};

  //    To wait without blocking threads, define a continuation function, to be
  //  executed after all other tasks complete.
  std::function<void(void)> continuation = [&]()
    {
      //    Check whether all other subtasks are complete. If not, push another
      //  call to this continuation function into the pool.
      if (!continuation_guard.try_wait())
      {
        pool.schedule(continuation);
        return;
      }
      if (counter.load(std::memory_order_relaxed) == 1024)
        std::cout << "SUCCESS\n";
      else
        std::cout << "FAILED\n";
    };

  pool.schedule([&](){
      for (int j = 0; j < 1024; ++j)
      {
        pool.schedule_subtask([&](){
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            counter.fetch_add(1, std::memory_order_relaxed);
            continuation_guard.count_down(1);
          });
      }
      pool.schedule(continuation);
    });
  //    Threads outside the pool can take a simpler approach, using the OS's
  //  preemptive scheduling or other waiting mechanisms.
  continuation_guard.wait();
  std::cout << "Finishing.\n";
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  return 0;
}

