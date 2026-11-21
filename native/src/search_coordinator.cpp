#include "search_coordinator.hpp"

#include <windows.h>

#include <chrono>
#include <utility>

namespace feathercast::search {
namespace {
constexpr auto kIdleWorkerTimeout = std::chrono::seconds(20);
}

SearchCoordinator::SearchCoordinator(ResultSink resultSink, ErrorSink errorSink)
    : resultSink_(std::move(resultSink)), errorSink_(std::move(errorSink)) {}

SearchCoordinator::~SearchCoordinator() {
  Stop();
}

void SearchCoordinator::Start(Processor processor) {
  std::lock_guard lock(mutex_);
  if (started_) return;
  processor_ = std::move(processor);
  stopping_ = false;
  started_ = static_cast<bool>(processor_);
}

void SearchCoordinator::Stop() {
  std::jthread toJoin;
  {
    std::lock_guard lock(mutex_);
    if (!started_ && !worker_.joinable()) return;
    stopping_ = true;
    started_ = false;
    pending_.reset();
    toJoin = std::move(worker_);
    workerActive_ = false;
    if (toJoin.joinable()) toJoin.request_stop();
  }
  cv_.notify_all();
  if (toJoin.joinable()) toJoin.join();
  std::lock_guard lock(mutex_);
  processor_ = {};
  stopping_ = false;
}

bool SearchCoordinator::Query(app::QueryRequest request) {
  latestGeneration_.store(request.generation, std::memory_order_release);
  std::jthread retired;
  {
    std::lock_guard lock(mutex_);
    if (!started_ || stopping_ || !processor_) return false;
    pending_ = std::move(request);
    if (!workerActive_) {
      retired = std::move(worker_);
      workerActive_ = true;
      worker_ = std::jthread(
          [this](std::stop_token token) { WorkerLoop(token); });
    }
  }
  if (retired.joinable()) retired.join();
  cv_.notify_one();
  return true;
}

void SearchCoordinator::Invalidate(std::uint64_t generation) {
  latestGeneration_.store(generation, std::memory_order_release);
  std::lock_guard lock(mutex_);
  if (pending_ && pending_->generation != generation) pending_.reset();
}

void SearchCoordinator::WorkerLoop(std::stop_token stopToken) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  for (;;) {
    app::QueryRequest request;
    {
      std::unique_lock lock(mutex_);
      const bool woke = cv_.wait_for(lock, kIdleWorkerTimeout, [&] {
        return pending_.has_value() || stopping_ ||
               stopToken.stop_requested();
      });
      if (stopping_ || stopToken.stop_requested() ||
          (!woke && !pending_.has_value())) {
        workerActive_ = false;
        return;
      }
      request = std::move(*pending_);
      pending_.reset();
    }
    request.latestGeneration = &latestGeneration_;
    app::ResultsCollection result;
    try {
      result = processor_(request);
    } catch (...) {
      if (errorSink_) {
        try { errorSink_(std::current_exception()); } catch (...) {}
      }
      continue;
    }
    if (request.generation !=
        latestGeneration_.load(std::memory_order_acquire)) {
      continue;
    }
    if (resultSink_) {
      try {
        resultSink_(std::move(result));
      } catch (...) {
        if (errorSink_) {
          try { errorSink_(std::current_exception()); } catch (...) {}
        }
      }
    }
  }
}

SnapshotCoordinator::SnapshotCoordinator(ResultSink resultSink,
                                         ErrorSink errorSink)
    : resultSink_(std::move(resultSink)), errorSink_(std::move(errorSink)) {}

SnapshotCoordinator::~SnapshotCoordinator() {
  Stop();
}

void SnapshotCoordinator::Start(Builder builder) {
  std::lock_guard lock(mutex_);
  if (started_) return;
  builder_ = std::move(builder);
  stopping_ = false;
  started_ = static_cast<bool>(builder_);
}

void SnapshotCoordinator::Stop() {
  std::jthread toJoin;
  {
    std::lock_guard lock(mutex_);
    if (!started_ && !worker_.joinable()) return;
    stopping_ = true;
    started_ = false;
    pending_.reset();
    toJoin = std::move(worker_);
    workerActive_ = false;
    if (toJoin.joinable()) toJoin.request_stop();
  }
  cv_.notify_all();
  if (toJoin.joinable()) toJoin.join();
  std::lock_guard lock(mutex_);
  builder_ = {};
  stopping_ = false;
}

void SnapshotCoordinator::Invalidate() {
  std::lock_guard lock(mutex_);
  ++latestRevision_;
  pending_.reset();
}

bool SnapshotCoordinator::UpdateCorpus(app::SnapshotBuildRequest request) {
  latestRevision_.store(request.revision, std::memory_order_release);
  std::jthread retired;
  {
    std::lock_guard lock(mutex_);
    if (!started_ || stopping_ || !builder_) return false;
    pending_ = std::move(request);
    if (!workerActive_) {
      retired = std::move(worker_);
      workerActive_ = true;
      worker_ = std::jthread(
          [this](std::stop_token token) { WorkerLoop(token); });
    }
  }
  if (retired.joinable()) retired.join();
  cv_.notify_one();
  return true;
}

void SnapshotCoordinator::WorkerLoop(std::stop_token stopToken) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  for (;;) {
    app::SnapshotBuildRequest request;
    {
      std::unique_lock lock(mutex_);
      const bool woke = cv_.wait_for(lock, kIdleWorkerTimeout, [&] {
        return pending_.has_value() || stopping_ ||
               stopToken.stop_requested();
      });
      if (stopping_ || stopToken.stop_requested() ||
          (!woke && !pending_.has_value())) {
        workerActive_ = false;
        return;
      }
      request = std::move(*pending_);
      pending_.reset();
    }
    std::shared_ptr<const app::SearchSnapshot> snapshot;
    try {
      snapshot = builder_(request.settings);
    } catch (...) {
      if (errorSink_) {
        try { errorSink_(std::current_exception()); } catch (...) {}
      }
      continue;
    }
    if (request.revision !=
        latestRevision_.load(std::memory_order_acquire)) {
      continue;
    }
    if (resultSink_) {
      try {
        resultSink_({request.revision, std::move(snapshot)});
      } catch (...) {
        if (errorSink_) {
          try { errorSink_(std::current_exception()); } catch (...) {}
        }
      }
    }
  }
}

}  // namespace feathercast::search
