#include <benchmark/benchmark.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>

#include "src/thread_pool.h"

// Default number of iterations when simulating a CPU bound task.
constexpr uint64_t kNumIterations = 50000;

// When comparing std::async to launching using a thread pool, we spawn this
// many tasks per benchmark iteration.
constexpr int kNumTasks = 10000;

// Synthetic CPU bound task that applies std::cos repeatedly.
// This computes http://mathworld.wolfram.com/DottieNumber.html
void CpuTask(uint64_t n = kNumIterations) {
  constexpr double kStartValue = 1.24;
  double curr_value = kStartValue;
  for (uint64_t i = 0; i < n; ++i) {
    curr_value = std::cos(curr_value);
  }
  benchmark::DoNotOptimize(curr_value);
}

static void BM_CpuTask(benchmark::State& state) {
  for (auto _ : state) {
    CpuTask();
  }
}
BENCHMARK(BM_CpuTask);  // Register the benchmark.

static void BM_ThreadPoolUsage(benchmark::State& state) {
  const int num_threads = state.range(0);
  cb::ThreadPool pool(num_threads);
  for (auto _ : state) {
    std::array<std::future<void>, kNumTasks> futures;
    for (std::size_t i = 0; i < futures.size(); ++i) {
      futures[i] = pool.ScheduleAndGetFuture([]() { CpuTask(); });
    }
    for (auto& future : futures) {
      future.wait();
    }
  }
  state.SetItemsProcessed(state.iterations() * kNumTasks);
}
BENCHMARK(BM_ThreadPoolUsage)
    ->UseRealTime()
    ->RangeMultiplier(2)
    ->Range(1, 128)
    ->Arg(1000);

static void BM_AsyncUsage(benchmark::State& state) {
  for (auto _ : state) {
    std::array<std::future<void>, kNumTasks> futures;
    for (std::size_t i = 0; i < futures.size(); ++i) {
      futures[i] = std::async(std::launch::async, &CpuTask, kNumIterations);
    }
    for (auto& future : futures) {
      future.wait();
    }
  }
  state.SetItemsProcessed(state.iterations() * kNumTasks);
}
BENCHMARK(BM_AsyncUsage)
  ->UseRealTime();

// Benchmark the overhead of waiting for a single a "no-op" function
// executed via std::async.
static void BM_AsyncOverhead(benchmark::State& state) {
  for (auto _ : state) {
    std::async(std::launch::async, []() {}).wait();
  }
}
BENCHMARK(BM_AsyncOverhead)->UseRealTime();

// Benchmark the overhead of waiting for a single a "no-op" function
// executed on a thread pool.
static void BM_ThreadpoolOverhead(benchmark::State& state) {
  // Number of threads to use in thread pool.
  constexpr int kNumThreads = 4;
  cb::ThreadPool pool(kNumThreads);
  for (auto _ : state) {
    pool.ScheduleAndGetFuture([]() {}).wait();
  }
}
BENCHMARK(BM_ThreadpoolOverhead)->UseRealTime();

// TODO(cbraley): Add a benchmark that demonstrates the issues I am saw with
// std::async(std::launch_async, ...) and thread-local storage. Each std::async
// call generates a new thread, and all TLS variables and destructed whenever 
// a thread is destroyed. This generates a lot of overhead for applications like 
// multi-threaded logging, where you may want to use TLS to store a thread-local
// log buffer that is periodically merged with the main global log.

int main(int argc, char** argv) {
  ::benchmark::Initialize(&argc, argv);
  ::benchmark::RunSpecifiedBenchmarks();
  return EXIT_SUCCESS;
}