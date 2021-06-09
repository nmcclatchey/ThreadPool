project(threadpool)
cmake_minimum_required(VERSION 3.1)

include(CTest)

set(CMAKE_CXX_STANDARD 11)

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)


add_definitions(-Wall -Wextra)
add_library(threadpool STATIC ${CMAKE_CURRENT_SOURCE_DIR}/threadpool.cpp)
set_target_properties(threadpool PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(threadpool Threads::Threads)

if (BUILD_TESTING)
    set(CMAKE_CXX_FLAGS "-g -O0 -Wall -fprofile-arcs -ftest-coverage")
    add_executable(check ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pool.cpp)
    target_link_libraries(check threadpool)
    add_test(NAME threadpool_standalone_tests COMMAND check)

    add_executable(check_latch ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_latch.cpp)
    target_link_libraries(check_latch Threads::Threads)
    add_test(NAME latch_standalone_tests COMMAND check_latch)

    add_executable(example_latchpool ${CMAKE_CURRENT_SOURCE_DIR}/example.cpp)
    target_link_libraries(example_latchpool threadpool Threads::Threads)
    add_test(NAME latch_pool_example COMMAND example_latchpool)
endif (BUILD_TESTING)