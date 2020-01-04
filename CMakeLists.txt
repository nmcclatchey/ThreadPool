project(threadpool)
cmake_minimum_required(VERSION 3.1)

include(CTest)

set(CMAKE_CXX_STANDARD 11)

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)


add_definitions(-Wall -Wextra)
add_library(threadpool STATIC ${CMAKE_CURRENT_SOURCE_DIR}/threadpool.cpp)
target_link_libraries(threadpool Threads::Threads)

if (BUILD_TESTING)
    set(CMAKE_CXX_FLAGS "-g -O0 -Wall -fprofile-arcs -ftest-coverage")
    add_executable(check ${CMAKE_CURRENT_SOURCE_DIR}/tests/tests.cpp)
    target_link_libraries(check threadpool)
    add_test(NAME threadpool_standalone_tests COMMAND check)

    find_package(Boost 1.60.0)
    if (Boost_FOUND)
        include_directories(${Boost_INCLUDE_DIRS})
        add_library(boost_test STATIC ${CMAKE_CURRENT_SOURCE_DIR}/tests/boost_test_main.cpp)
        target_link_libraries(boost_test ${Boost_LIBRARIES})
        add_executable(${PROJECT_NAME}_unit_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit_tests.cpp)
        target_link_libraries(${PROJECT_NAME}_unit_tests threadpool boost_test)
        add_test(NAME threadpool_boost_tests COMMAND ${PROJECT_NAME}_unit_tests)
    endif (Boost_FOUND)
endif (BUILD_TESTING)