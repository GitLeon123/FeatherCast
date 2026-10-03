#include "input_broker_task.hpp"

#include <windows.h>
#include <sddl.h>
#include <taskschd.h>
#include <wrl/client.h>
#include <filesystem>
#include <vector>

namespace feathercast::input_broker {
namespace {
using Microsoft::WRL::ComPtr;

struct Bstr {
  explicit Bstr(const wchar_t* text) : value(SysAllocString(text)) {}
  ~Bstr() { SysFreeString(value); }
  Bstr(const Bstr&) = delete;
  Bstr& operator=(const Bstr&) = delete;
  BSTR value;
};

std::wstring SessionSuffix() {
  DWORD session = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &session);
  return CurrentUserSid() + L"-" + std::to_wstring(session);
}
}  // namespace

std::wstring CurrentUserSid() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<BYTE> buffer(size);
  const bool valid = size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
  CloseHandle(token);
  if (!valid) return {};
  LPWSTR sid = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) return {};
  const std::wstring result(sid);
  LocalFree(sid);
  return result;
}

std::wstring PipeName() {
  return L"\\\\.\\pipe\\FeatherCastInputBroker-" + SessionSuffix();
}

std::wstring MutexName() {
  return L"Local\\FeatherCastInputBroker-" + SessionSuffix();
}

bool StartScheduledBroker(const std::wstring& brokerPath) {
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initialized)) return false;
  const bool started = [&] {
    ComPtr<ITaskService> service;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&service)))) return false;
    VARIANT empty{};
    if (FAILED(service->Connect(empty, empty, empty, empty))) return false;
    ComPtr<ITaskFolder> root;
    const Bstr rootName(L"\\");
    if (FAILED(service->GetFolder(rootName.value, &root))) return false;
    const auto sid = CurrentUserSid();
    if (sid.empty()) return false;
    const Bstr taskName((L"FeatherCast Input Broker-" + sid).c_str());
    ComPtr<IRegisteredTask> task;
    if (FAILED(root->GetTask(taskName.value, &task))) return false;

    // A development/portable copy must not start another installation's broker.
    ComPtr<ITaskDefinition> definition;
    ComPtr<IActionCollection> actions;
    ComPtr<IAction> action;
    ComPtr<IExecAction> executable;
    if (FAILED(task->get_Definition(&definition)) ||
        FAILED(definition->get_Actions(&actions)) ||
        FAILED(actions->get_Item(1, &action)) || FAILED(action.As(&executable))) return false;
    BSTR path = nullptr;
    if (FAILED(executable->get_Path(&path))) return false;
    std::error_code error;
    const bool samePath = path && std::filesystem::equivalent(path, brokerPath, error);
    SysFreeString(path);
    if (!samePath || error) return false;

    ComPtr<IRunningTask> running;
    return SUCCEEDED(task->Run(empty, &running));
  }();
  CoUninitialize();
  return started;
}

}  // namespace feathercast::input_broker
