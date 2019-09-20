#ifndef SRC_THREAD_POOL_H_
#define SRC_THREAD_POOL_H_

// A simple thread pool class.
// Usage examples:
//
// {
//   ThreadPool pool(16);  // 16 worker threads.
//   for (int i = 0; i < 100; ++i) {
//     pool.Schedule([i]() {
//       DoSlowExpensiveOperation(i);
//     });
//   }
//
//   // `pool` goes out of scope here - the code will block in the ~ThreadPool
//   // destructor until all work is complete.
// }
//
// // TODO(cbraley): Add examples with std::future.

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace cb {

class ThreadPool {
 public:
  // Create a thread pool with `num_workers` dedicated worker threads.
  explicit ThreadPool(int num_workers);

  // Default construction is disallowed.
  ThreadPool() = delete;

  // Get the default thread pool size. This is implemented using
  // std::thread::hardware_concurrency().
  // https://en.cppreference.com/w/cpp/thread/thread/hardware_concurrency
  // On my machine this returns the number of logical cores.
  static unsigned int GetDefaultThreadPoolSize();

  // The `ThreadPool` destructor blocks until all outstanding work is complete.
  ~ThreadPool();

  // No copying, assigning, or std::move-ing.
  ThreadPool& operator=(const ThreadPool&) = delete;
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool(ThreadPool&&) = delete;
  ThreadPool& operator=(ThreadPool&&) = delete;

  // Add the function `func` to the thread pool. `func` will be executed at some
  // point in the future on an arbitrary thread.
  void Schedule(std::function<void(void)> func);

  // Add `func` to the thread pool, and return a std::future that can be used to
  // access the function's return value.
  //
  // *** Usage example ***
  //   Don't be alarmed by this function's tricky looking signature - this is
  //   very easy to use. Here's an example:
  //
  //   int ComputeSum(std::vector<int>& values) {
  //     int sum = 0;
  //     for (const int& v : values) {
  //       sum += v;
  //     }
  //     return sum;
  //   }
  //
  //   ThreadPool pool = ...;
  //   std::vector<int> numbers = ...;
  //
  //   std::future<int> sum_future = ScheduleAndGetFuture(
  //     []() {
  //       return ComputeSum(numbers);
  //     });
  //
  //   // Do other work...
  //
  //   std::cout << "The sum is " << sum_future.get() << std::endl;
  //
  // *** Details ***
  //   Given a callable `func` that returns a value of type `RetT`, this
  //   function returns a std::future<RetT> that can be used to access
  //   `func`'s results.
  template <typename FuncT, typename... ArgsT>
  auto ScheduleAndGetFuture(FuncT&& func, ArgsT&&... args)
      -> std::future<decltype(func(std::forward<ArgsT>(args)...))>;

  // Wait for all outstanding work to be completed.
  void Wait();

  // Return the number of outstanding functions to be executed.
  int OutstandingWorkSize() const;

  // Return the number of threads in the pool.
  int NumWorkers() const;

 private:
  void ThreadLoop();

  // Number of worker threads - fixed at construction time.
  int num_workers_;

  // The destructor sets `exit_` to true and then notifues all workers. `exit_`
  // causes each thread to break out of their work loop.
  bool exit_;

  mutable std::mutex mu_;

  // Work queue. Guarded by `mu_`.
  struct WorkItem {
    std::function<void(void)> func;
  };
  std::queue<WorkItem> work_;

  // Condition variable used to notify worker threads that new work is
  // available.
  std::condition_variable condvar_;

  // Worker threads.
  std::vector<std::thread> workers_;

  // Condition variable used to notify that all work is complete - the work
  // queue has "run dry".
  std::condition_variable work_done_condvar_;
};

namespace impl {

// This helper class simply returns a std::function that executes:
//   T x = func();
//   promise->set_value(x);
// However, this is tricky in the case where T == void. The code above won't
// compile if T == void, and neither will
//   promise->set_value(func());
// To workaround this, we have a template specialization for the case where
// void is returned. If the "regular void" proposal is accepted, we could
// simplify this:
// http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2016/p0146r1.html.

// This `FuncWrapper` handles callables that return a non-void value.
template <typename ReturnT>
struct FuncWrapper {
  template <typename FuncT, typename... ArgsT>
  std::function<void()> GetWrapped(
      FuncT&& func, std::shared_ptr<std::promise<ReturnT>> promise,
      ArgsT&&... args) {
    // TODO(cbraley): Capturing by value is inefficient. It would be more
    // efficient to move-capture everything, but we can't do this until C++14
    // generalized lambda capture is available. Can we use std::bind instead to
    // make this more efficient and still use C++11?
    //
    // The same TODO applies below as well.
    return [promise, func, args...]() mutable {
      promise->set_value(func(std::forward<ArgsT>(args)...));
    };
  }
};

// This `FuncWrapper` handles callables that return void.
template <>
struct FuncWrapper<void> {
  template <typename FuncT, typename... ArgsT>
  std::function<void()> GetWrapped(FuncT&& func,
                                   std::shared_ptr<std::promise<void>> promise,
                                   ArgsT&&... args) {
    return [promise, func, args...]() {
      func(std::forward<ArgsT>(args)...);
      promise->set_value();
    };
  }
};

}  // namespace impl

template <typename FuncT, typename... ArgsT>
auto ThreadPool::ScheduleAndGetFuture(FuncT&& func, ArgsT&&... args)
    -> std::future<decltype(func(std::forward<ArgsT>(args)...))> {
  using ReturnT = decltype(func(std::forward<ArgsT>(args)...));

  // We are only allocating this std::promise as a shared_ptr because a normal
  // std::promise is non-copyable. See:
  // https://stackoverflow.com/questions/28208948/how-to-store-non-copyable-stdfunction-into-a-container
  // for more details.
  std::shared_ptr<std::promise<ReturnT>> promise =
      std::make_shared<std::promise<ReturnT>>();
  std::future<ReturnT> ret_future = promise->get_future();

  impl::FuncWrapper<ReturnT> func_wrapper;
  std::function<void()> wrapped_func = func_wrapper.GetWrapped(
      std::move(func), std::move(promise), std::forward<ArgsT>(args)...);

  // Acquire the lock, and then push the WorkItem onto the queue.
  {
    std::lock_guard<std::mutex> scoped_lock(mu_);
    WorkItem work;
    work.func = std::move(wrapped_func);
    work_.emplace(std::move(work));
  }
  condvar_.notify_one();  // Tell one worker we are ready.
  return ret_future;
}

}  // namespace cb

#endif  // SRC_THREAD_POOL_H_