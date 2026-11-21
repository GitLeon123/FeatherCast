#pragma once

#include <windows.h>

#include <algorithm>
#include <chrono>
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
  using IdleHandler = std::function<void()>;

  Executor() = default;
  ~Executor() { Shutdown(); }

  Executor(const Executor&) = delete;
  Executor& operator=(const Executor&) = delete;

  // Workers are spawned lazily on Submit() and exit automatically after
  // idleTimeout of inactivity so hidden-idle processes keep 0 parked threads.
  void Start(size_t workerCount = 2, ErrorHandler errorHandler = {},
             IdleHandler idleHandler = {},
             std::chrono::milliseconds idleTimeout = std::chrono::seconds(30)) {
    std::lock_guard lock(mutex_);
    if (state_) return;
    auto state = std::make_shared<State>();
    state->maxWorkers = std::max<size_t>(1, workerCount);
    state->errorHandler = std::move(errorHandler);
    state->idleHandler = std::move(idleHandler);
    state->idleTimeout = idleTimeout.count() > 0 ? idleTimeout : std::chrono::seconds(30);
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

    std::vector<std::shared_ptr<WorkerSlot>> exitedSlots;
    bool notify = false;
    {
      std::lock_guard lock(state->mutex);
      if (state->stopping) return false;
      ReapExitedLocked(*state, exitedSlots);
      state->tasks.push_back(std::move(task));
      if (state->idleWorkers == 0 && state->liveWorkers < state->maxWorkers) {
        auto slot = std::make_shared<WorkerSlot>();
        ++state->liveWorkers;
        slot->thread = std::jthread([state, slot] { WorkerLoop(state, slot); });
        slot->id = slot->thread.get_id();
        state->slots.push_back(std::move(slot));
      } else {
        notify = true;
      }
    }
    if (notify) state->cv.notify_one();
    JoinSlots(exitedSlots, std::this_thread::get_id());
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
    bool owner = false;
    {
      std::lock_guard lock(mutex_);
      if (state_) {
        state = std::move(state_);
        retiring_ = state;
        owner = true;
      } else {
        state = retiring_.lock();
      }
    }
    if (!state) return;

    std::deque<Task> discarded;
    std::vector<std::shared_ptr<WorkerSlot>> slotsToJoin;
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
      if (owner) {
        slotsToJoin.swap(state->slots);
      }
    }
    state->cv.notify_all();
    // Discarded tasks may own resources; release them outside the lock.
    discarded.clear();

    const auto self = std::this_thread::get_id();
    if (!owner) {
      // Another caller owns the join. Wait for it, unless this thread is one
      // of the workers being joined, which would deadlock.
      std::unique_lock lock(state->mutex);
      const auto& ids = state->activeIds;
      if (std::find(ids.begin(), ids.end(), self) != ids.end()) return;
      state->joinedCv.wait(lock, [&] { return state->joined; });
      return;
    }

    JoinSlots(slotsToJoin, self);
    {
      std::lock_guard lock(state->mutex);
      state->joined = true;
      state->errorHandler = {};
      state->idleHandler = {};
      state->activeIds.clear();
    }
    state->joinedCv.notify_all();
  }

 private:
  struct WorkerSlot {
    std::jthread thread;
    std::thread::id id;
    bool exited = false;
  };

  // Everything a worker touches lives here, shared with the workers, so a
  // worker that outlives the Executor (see Shutdown) stays valid.
  struct State {
    std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable joinedCv;
    std::deque<Task> tasks;
    ErrorHandler errorHandler;
    IdleHandler idleHandler;
    std::chrono::milliseconds idleTimeout{std::chrono::seconds(30)};
    std::stop_source stop;
    std::vector<std::shared_ptr<WorkerSlot>> slots;
    std::vector<std::thread::id> activeIds;
    size_t maxWorkers = 1;
    size_t liveWorkers = 0;
    size_t idleWorkers = 0;
    bool stopping = false;
    bool drain = false;
    bool joined = false;
  };

  static void ReapExitedLocked(
      State& state, std::vector<std::shared_ptr<WorkerSlot>>& out) {
    auto it = state.slots.begin();
    while (it != state.slots.end()) {
      if ((*it)->exited) {
        out.push_back(std::move(*it));
        it = state.slots.erase(it);
      } else {
        ++it;
      }
    }
  }

  static void JoinSlots(std::vector<std::shared_ptr<WorkerSlot>>& slots,
                        std::thread::id self) {
    for (auto& slot : slots) {
      if (!slot || !slot->thread.joinable()) continue;
      if (slot->thread.get_id() == self) {
        slot->thread.detach();
      } else {
        slot->thread.join();
      }
    }
  }

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

  static void WorkerLoop(std::shared_ptr<State> state,
                         std::shared_ptr<WorkerSlot> slot) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const std::thread::id selfId = std::this_thread::get_id();
    const auto finishLocked = [&]() {
      if (state->liveWorkers > 0) --state->liveWorkers;
      auto it = std::find(state->activeIds.begin(), state->activeIds.end(), selfId);
      if (it != state->activeIds.end()) state->activeIds.erase(it);
      slot->exited = true;
    };
    {
      std::lock_guard lock(state->mutex);
      state->activeIds.push_back(selfId);
    }
    const std::stop_token stopToken = state->stop.get_token();
    bool ranTask = false;
    for (;;) {
      Task task;
      IdleHandler idleCallback;
      bool timedOut = false;
      {
        std::unique_lock lock(state->mutex);
        ++state->idleWorkers;
        const bool woke = state->cv.wait_for(lock, state->idleTimeout, [&] {
          return state->stopping || stopToken.stop_requested() || !state->tasks.empty();
        });
        --state->idleWorkers;
        if (stopToken.stop_requested() ||
            (state->stopping && (!state->drain || state->tasks.empty()))) {
          finishLocked();
          return;
        }
        if (!woke && state->tasks.empty()) {
          timedOut = true;
          if (ranTask && state->liveWorkers == 1 && state->idleHandler) {
            idleCallback = state->idleHandler;
          } else {
            finishLocked();
            return;
          }
        } else {
          task = std::move(state->tasks.front());
          state->tasks.pop_front();
        }
      }
      if (timedOut) {
        try {
          if (idleCallback) idleCallback();
        } catch (...) {
          ReportFailure(*state, std::current_exception());
        }
        ranTask = false;
        std::lock_guard lock(state->mutex);
        if (!state->tasks.empty() && !stopToken.stop_requested() &&
            (!state->stopping || state->drain)) {
          continue;
        }
        finishLocked();
        return;
      }
      ranTask = true;
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
};

using BackgroundExecutor = Executor;

}  // namespace feathercast::background
