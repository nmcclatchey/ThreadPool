# ThreadPool tests

To ensure that the ThreadPool library works properly, I have created a test of its capabilities. The file `tests.cpp` is intended to test
* Compilation
* Expected use cases
* Load-balancing

The test application will, when run, perform the following:
* Create and destroy a `ThreadPool` without assigning any tasks.
* Assign tasks to a `ThreadPool`.
* Ensure that `ThreadPool`s idle when all tasks are complete, to avoid excessive CPU use. If it fails to idle quickly enough after completion, or does not complete within a reasonable period of time, the test application returns non-zero.
* Restart an idling `ThreadPool` for a second round of tasks.
* Measure how well tasks are balanced, by counting the minimum, maximum, and average number of tasks performed by each worker thread. Given that the tasks are (mostly) homogeneous, good balance is indicated by similarity of these numbers.