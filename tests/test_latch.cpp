//  Include this first to check for missed dependencies.
#include "../latch.hpp"

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
#include <chrono>
#include <iostream>

#define LOG(fmtString,...) std::printf(fmtString "\n", ##__VA_ARGS__); fflush(stdout)

int main()
{
  std::atomic<int> result_code {0};
  {
    latch test_latch (4);
    for (int i = 0; i < 4; ++i)
    {
      std::thread new_thread([i,&test_latch](){
          std::this_thread::sleep_for(std::chrono::milliseconds(500 * (i + 1)));
          test_latch.count_down(1);
        });
      new_thread.detach();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    for (int i = 0; i < 4; ++i)
    {
      if (test_latch.try_wait())
      {
        LOG("Exiting far too early (probe point %d)", i);
        result_code.fetch_or(1, std::memory_order_relaxed);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!test_latch.try_wait())
    {
      LOG("%s", "Did not unlock when expected.");
      result_code.fetch_or(2, std::memory_order_relaxed);
    }
  }
  {
    latch test_latch (4);
    std::atomic<int> counter {0};
    for (int i = 0; i < 4; ++i)
    {
      std::thread new_thread([i,&test_latch, &counter, &result_code](){
          std::this_thread::sleep_for(std::chrono::milliseconds(500 * (i + 1)));
          counter.fetch_add(1, std::memory_order_relaxed);
          try
          {
            test_latch.arrive_and_wait(1);
          }
          catch(const std::system_error & e)
          {
            std::cerr << "Arrive-and-wait error code " << e.code() << ": " << e.what() << '\n';
            result_code.fetch_or(16, std::memory_order_relaxed);
          }
          if (counter.load(std::memory_order_relaxed) != 4)
            result_code.fetch_or(8, std::memory_order_relaxed);
        });
      new_thread.detach();
    }
    try
    {
      test_latch.wait();
    }
    catch(const std::system_error & e)
    {
      std::cerr << "Wait error code " << e.code() << ": " << e.what() << '\n';
      result_code.fetch_or(32, std::memory_order_relaxed);
    }
    if (counter.load(std::memory_order_relaxed) != 4)
      result_code.fetch_or(4, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  return result_code.load(std::memory_order_relaxed);
}

