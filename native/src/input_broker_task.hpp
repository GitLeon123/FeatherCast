#pragma once

#include <string>

namespace feathercast::input_broker {

std::wstring CurrentUserSid();
std::wstring PipeName();
std::wstring MutexName();
bool StartScheduledBroker(const std::wstring& brokerPath);

}  // namespace feathercast::input_broker
