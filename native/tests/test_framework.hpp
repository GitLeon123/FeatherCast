#pragma once

#include <crtdbg.h>
#include <cstdio>
#include <cstdlib>

namespace feathercast::test {

// A failed check or CRT assertion must end the test with an error code right
// away. The default abort()/assert handlers open a modal dialog (Debug) or a
// Windows Error Report (Release), which leaves CTest waiting for its timeout.
inline bool ConfigureFailureReporting() {
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  (void)_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  (void)_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
  (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  (void)_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  return true;
}

inline const bool kFailureReportingConfigured = ConfigureFailureReporting();

[[noreturn]] inline void Fail(const char* expression, const char* file, int line) {
  std::fflush(stdout);
  std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", expression, file, line);
  std::fflush(stderr);
  std::abort();
}

// Wall-clock budgets are tuned for an idle developer machine. Shared CI
// runners are noisier, so CI widens them 2x; FEATHERCAST_PERF_BUDGET_SCALE
// overrides the factor (clamped to 1..10, so budgets never get stricter).
inline double TimingBudgetScale() {
  double scale = 1.0;
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, "FEATHERCAST_PERF_BUDGET_SCALE") == 0 && value) {
    scale = std::strtod(value, nullptr);
    std::free(value);
  } else if (_dupenv_s(&value, &length, "CI") == 0 && value) {
    std::free(value);
    scale = 2.0;
  }
  if (!(scale >= 1.0)) scale = 1.0;
  return scale > 10.0 ? 10.0 : scale;
}

}  // namespace feathercast::test

// The existing suites historically used assert(), which disappears in Release
// builds. Keep the call sites readable while making every check unconditional.
#ifdef assert
#undef assert
#endif
#define assert(expression) \
  ((expression) ? static_cast<void>(0) : ::feathercast::test::Fail(#expression, __FILE__, __LINE__))
