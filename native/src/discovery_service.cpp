#include "discovery_service.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <utility>

namespace feathercast::discovery_runtime {

DiscoveryService::DiscoveryService(ResultSink resultSink, ErrorSink errorSink,
                                   RetryPolicy retry)
    : resultSink_(std::move(resultSink)),
      errorSink_(std::move(errorSink)),
      retry_(retry) {}

DiscoveryService::~DiscoveryService() {
  Stop();
}

void DiscoveryService::Start(Worker worker) {
  std::lock_guard lock(mutex_);
  if (worker_.joinable()) return;
  workerFunction_ = std::move(worker);
  stopping_ = false;
  worker_ = std::jthread(
      [this](std::stop_token token) { WorkerLoop(token); });
}

void DiscoveryService::Stop() {
  {
    std::lock_guard lock(mutex_);
    if (!worker_.joinable()) return;
    stopping_ = true;
    pending_.reset();
    latestGeneration_.fetch_add(1, std::memory_order_acq_rel);
    worker_.request_stop();
  }
  cv_.notify_all();
  worker_.join();
  std::lock_guard lock(mutex_);
  workerFunction_ = {};
  stopping_ = false;
}

bool DiscoveryService::Refresh(app::DiscoveryRequest request) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_ || !worker_.joinable() || !workerFunction_) return false;
    latestGeneration_.store(request.generation, std::memory_order_release);
    pending_ = std::move(request);
  }
  cv_.notify_one();
  return true;
}

std::uint64_t DiscoveryService::Cancel() {
  std::uint64_t generation;
  {
    std::lock_guard lock(mutex_);
    generation = latestGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
    pending_.reset();
  }
  cv_.notify_all();
  return generation;
}

bool DiscoveryService::IsCurrent(std::uint64_t generation) const {
  return latestGeneration_.load(std::memory_order_acquire) == generation;
}

void DiscoveryService::WorkerLoop(std::stop_token stopToken) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  std::uint64_t retryGeneration = 0;
  int failedAttempts = 0;
  for (;;) {
    app::DiscoveryRequest request;
    Worker worker;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] {
        return pending_.has_value() || stopping_ ||
               stopToken.stop_requested();
      });
      if (stopping_ || stopToken.stop_requested()) return;
      request = std::move(*pending_);
      pending_.reset();
      worker = workerFunction_;
    }
    if (retryGeneration != request.generation) {
      retryGeneration = request.generation;
      failedAttempts = 0;
    }
    std::optional<app::DiscoveryResult> result;
    try {
      result = worker(request, stopToken);
    } catch (...) {
      ++failedAttempts;
      if (!IsCurrent(request.generation)) continue;
      if (failedAttempts >= retry_.maxAttempts) {
        // Give up until the next explicit refresh instead of waking forever.
        if (errorSink_) {
          try { errorSink_(std::current_exception()); } catch (...) {}
        }
        continue;
      }
      // The shell may not be ready at login. Retry without requiring another
      // overlay open; a newer request, cancellation or shutdown interrupts us.
      auto retryDelay = retry_.firstDelay;
      for (int i = 1; i < failedAttempts && retryDelay < retry_.maxDelay; ++i) {
        retryDelay *= 2;
      }
      retryDelay = std::min(retryDelay, retry_.maxDelay);
      std::unique_lock lock(mutex_);
      cv_.wait_for(lock, retryDelay, [&] {
        return pending_.has_value() || stopping_ ||
               stopToken.stop_requested() || !IsCurrent(request.generation);
      });
      if (stopping_ || stopToken.stop_requested()) return;
      if (!pending_ && IsCurrent(request.generation)) {
        request.allowCache = false;
        pending_ = std::move(request);
      }
      continue;
    }
    if (!result || stopToken.stop_requested() ||
        !IsCurrent(request.generation)) {
      continue;
    }
    if (resultSink_) {
      try {
        resultSink_(std::move(*result));
      } catch (...) {
        if (errorSink_) {
          try { errorSink_(std::current_exception()); } catch (...) {}
        }
      }
    }
  }
}

}  // namespace feathercast::discovery_runtime
