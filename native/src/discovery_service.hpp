#pragma once

#include "app_types.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace feathercast::discovery_runtime {

// A failed scan is retried with exponential backoff because the shell may
// still be starting at login. The error sink only hears about a generation
// once every attempt has failed, so a slow login never raises an alert.
struct RetryPolicy {
  std::chrono::milliseconds firstDelay{1000};
  std::chrono::milliseconds maxDelay{30000};
  int maxAttempts = 6;
};

class DiscoveryService {
 public:
  using Worker = std::function<std::optional<app::DiscoveryResult>(
      const app::DiscoveryRequest&, std::stop_token)>;
  using ResultSink = std::function<void(app::DiscoveryResult)>;
  using ErrorSink = std::function<void(std::exception_ptr)>;

  explicit DiscoveryService(ResultSink resultSink = {}, ErrorSink errorSink = {},
                            RetryPolicy retry = {});
  ~DiscoveryService();

  DiscoveryService(const DiscoveryService&) = delete;
  DiscoveryService& operator=(const DiscoveryService&) = delete;

  void Start(Worker worker);
  void Stop();
  bool Refresh(app::DiscoveryRequest request);
  std::uint64_t Cancel();
  bool IsCurrent(std::uint64_t generation) const;

 private:
  void WorkerLoop(std::stop_token stopToken);

  ResultSink resultSink_;
  ErrorSink errorSink_;
  RetryPolicy retry_;
  Worker workerFunction_;
  std::jthread worker_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<app::DiscoveryRequest> pending_;
  std::atomic<std::uint64_t> latestGeneration_ = 0;
  bool stopping_ = false;
};

}  // namespace feathercast::discovery_runtime
