#pragma once

#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace feathercast::background {

class Executor {
 public:
  using Task = std::function<void(std::stop_token)>;
  using Completion = std::function<void()>;
  using ErrorHandler = std::function<void(std::exception_ptr)>;

  Executor() = default;
  ~Executor() { Shutdown(); }

  Executor(const Executor&) = delete;
  Executor& operator=(const Executor&) = delete;

  void Start(size_t workerCount = 2, ErrorHandler errorHandler = {}) {
    std::lock_guard lock(mutex_);
    if (state_) return;
    auto state = std::make_shared<State>();
    state->errorHandler = std::move(errorHandler);
    workerCount = std::max<size_t>(1, workerCount);
    workers_.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i) {
      workers_.emplace_back([state] { WorkerLoop(state); });
    }
    {
      std::lock_guard stateLock(state->mutex);
      for (const auto& worker : workers_) state->workerIds.push_back(worker.get_id());
    }
    state_ = std::move(state);
  }

  // Returns false once shutdown has begun; such tasks are never run.
  bool Submit(Task task) {
    if (!task) return false;
    std::shared_ptr<State> state;
    {
      std::lock_guard lock(mutex_);
      state = state_;
    }
    if (!state) return false;
    {
      std::lock_guard lock(state->mutex);
      if (state->stopping) return false;
      state->tasks.push_back(std::move(task));
    }
    state->cv.notify_one();
    return true;
  }

  bool Submit(Task task, Completion completion) {
    return Submit([task = std::move(task), completion = std::move(completion)](std::stop_token stopToken) {
      task(stopToken);
      if (!stopToken.stop_requested() && completion) completion();
    });
  }

  // Stops the workers. With drainPending the queued tasks still run before
  // the workers exit; otherwise they are discarded and running tasks see a
  // stop request. Safe to call repeatedly and concurrently: a later
  // non-draining call escalates a draining one, and every caller returns only
  // after the workers have been joined. A worker thread may call it (also via
  // the destructor); it cannot join itself, so it is detached and exits on its
  // own once its current task returns. Workers never touch the Executor
  // object itself, only shared state, so that is safe.
  void Shutdown(bool drainPending = false) {
    std::shared_ptr<State> state;
    std::vector<std::jthread> workers;
    {
      std::lock_guard lock(mutex_);
      if (state_) {
        state = std::move(state_);
        workers.swap(workers_);
        retiring_ = state;
      } else {
        state = retiring_.lock();
      }
    }
    if (!state) return;

    std::deque<Task> discarded;
    {
      std::lock_guard lock(state->mutex);
      if (!state->stopping) {
        state->stopping = true;
        state->drain = drainPending;
      } else if (!drainPending) {
        state->drain = false;
      }
      if (!state->drain) {
        discarded.swap(state->tasks);
        state->stop.request_stop();
      }
    }
    state->cv.notify_all();
    // Discarded tasks may own resources; release them outside the lock.
    discarded.clear();

    const auto self = std::this_thread::get_id();
    if (workers.empty()) {
      // Another caller owns the join. Wait for it, unless this thread is one
      // of the workers being joined, which would deadlock.
      std::unique_lock lock(state->mutex);
      const auto& ids = state->workerIds;
      if (std::find(ids.begin(), ids.end(), self) != ids.end()) return;
      state->joinedCv.wait(lock, [&] { return state->joined; });
      return;
    }

    for (auto& worker : workers) {
      if (!worker.joinable()) continue;
      if (worker.get_id() == self) {
        worker.detach();
      } else {
        worker.join();
      }
    }
    {
      std::lock_guard lock(state->mutex);
      state->joined = true;
      state->errorHandler = {};
    }
    state->joinedCv.notify_all();
  }

 private:
  // Everything a worker touches lives here, shared with the workers, so a
  // worker that outlives the Executor (see Shutdown) stays valid.
  struct State {
    std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable joinedCv;
    std::deque<Task> tasks;
    ErrorHandler errorHandler;
    std::stop_source stop;
    std::vector<std::thread::id> workerIds;
    bool stopping = false;
    bool drain = false;
    bool joined = false;
  };

  static void ReportFailure(State& state, std::exception_ptr failure) noexcept {
    ErrorHandler handler;
    try {
      std::lock_guard lock(state.mutex);
      handler = state.errorHandler;
    } catch (...) {
      return;
    }
    if (!handler) return;
    try {
      handler(std::move(failure));
    } catch (...) {
      // Error reporting must never terminate a worker.
    }
  }

  static void WorkerLoop(std::shared_ptr<State> state) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const std::stop_token stopToken = state->stop.get_token();
    for (;;) {
      Task task;
      {
        std::unique_lock lock(state->mutex);
        state->cv.wait(lock, [&] {
          return state->stopping || stopToken.stop_requested() || !state->tasks.empty();
        });
        if (stopToken.stop_requested()) return;
        if (state->stopping && (!state->drain || state->tasks.empty())) return;
        task = std::move(state->tasks.front());
        state->tasks.pop_front();
      }
      try {
        task(stopToken);
      } catch (const std::exception&) {
        ReportFailure(*state, std::current_exception());
      } catch (...) {
        ReportFailure(*state, std::current_exception());
      }
    }
  }

  std::mutex mutex_;
  std::shared_ptr<State> state_;
  std::weak_ptr<State> retiring_;
  std::vector<std::jthread> workers_;
};

using BackgroundExecutor = Executor;

}  // namespace feathercast::background
