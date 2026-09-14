#include "audio_volume.hpp"

#include <windows.h>
#include <endpointvolume.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <wrl/client.h>

#include <utility>

namespace feathercast::audio {
namespace {

using Microsoft::WRL::ComPtr;

ComPtr<IAudioEndpointVolume> cachedEndpoint;
ComPtr<IMMDevice> cachedDevice;
ComPtr<IMMDeviceEnumerator> cachedEnumerator;
std::wstring cachedEndpointId;
bool endpointCacheEnabled = false;

struct EndpointInfo {
  ComPtr<IMMDeviceEnumerator> enumerator;
  ComPtr<IMMDevice> device;
  ComPtr<IAudioEndpointVolume> volume;
  std::wstring id;
};

void ResetCachedEndpoint() {
  cachedEndpoint.Reset();
  cachedDevice.Reset();
  cachedEnumerator.Reset();
  cachedEndpointId.clear();
}

std::optional<EndpointInfo> DefaultOutputEndpointInfo() {
  ComPtr<IMMDeviceEnumerator> enumerator;
  if (endpointCacheEnabled && cachedEnumerator) {
    enumerator = cachedEnumerator;
  } else if (FAILED(CoCreateInstance(
                 __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                 IID_PPV_ARGS(enumerator.GetAddressOf())))) {
    return std::nullopt;
  }

  ComPtr<IMMDevice> device;
  if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                 device.GetAddressOf()))) {
    return std::nullopt;
  }

  std::wstring id;
  LPWSTR rawId = nullptr;
  if (SUCCEEDED(device->GetId(&rawId)) && rawId) {
    id = rawId;
  }
  CoTaskMemFree(rawId);
  if (endpointCacheEnabled && cachedEndpoint && cachedDevice &&
      !id.empty() && id == cachedEndpointId) {
    return EndpointInfo{enumerator, cachedDevice, cachedEndpoint, id};
  }

  ComPtr<IAudioEndpointVolume> volume;
  if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                              nullptr, reinterpret_cast<void**>(
                                           volume.GetAddressOf())))) {
    return std::nullopt;
  }
  if (endpointCacheEnabled) {
    cachedEnumerator = enumerator;
    cachedDevice = device;
    cachedEndpoint = volume;
    cachedEndpointId = id;
  }
  return EndpointInfo{std::move(enumerator), std::move(device),
                       std::move(volume), std::move(id)};
}

std::wstring DeviceFriendlyName(IMMDevice* device) {
  if (!device) return {};
  ComPtr<IPropertyStore> store;
  if (FAILED(device->OpenPropertyStore(STGM_READ, store.GetAddressOf()))) {
    return {};
  }

  PROPVARIANT value;
  PropVariantInit(&value);
  std::wstring name;
  if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value))) {
    if (value.vt == VT_LPWSTR && value.pwszVal) {
      name = value.pwszVal;
    } else if (value.vt == VT_BSTR && value.bstrVal) {
      name = value.bstrVal;
    }
  }
  PropVariantClear(&value);
  return name;
}

}  // namespace

std::optional<DefaultOutputState> ReadDefaultOutputState() {
  const auto endpoint = DefaultOutputEndpointInfo();
  if (!endpoint) return std::nullopt;

  float scalar = 0.0f;
  BOOL muted = FALSE;
  if (FAILED(endpoint->volume->GetMasterVolumeLevelScalar(&scalar)) ||
      FAILED(endpoint->volume->GetMute(&muted))) {
    ResetCachedEndpoint();
    return std::nullopt;
  }
  return DefaultOutputState{
      ClampPercent(static_cast<int>(std::lround(scalar * 100.0f))),
      muted != FALSE, DeviceFriendlyName(endpoint->device.Get())};
}

std::optional<int> ReadDefaultOutputVolumePercent() {
  const auto endpoint = DefaultOutputEndpointInfo();
  if (!endpoint) return std::nullopt;

  float scalar = 0.0f;
  if (FAILED(endpoint->volume->GetMasterVolumeLevelScalar(&scalar))) {
    ResetCachedEndpoint();
    return std::nullopt;
  }
  return ClampPercent(static_cast<int>(std::lround(scalar * 100.0f)));
}

bool SetDefaultOutputVolumePercent(int percent) {
  const auto endpoint = DefaultOutputEndpointInfo();
  if (!endpoint) return false;
  const float scalar = static_cast<float>(ClampPercent(percent)) / 100.0f;
  const bool succeeded = SUCCEEDED(
      endpoint->volume->SetMasterVolumeLevelScalar(scalar, nullptr));
  if (!succeeded) ResetCachedEndpoint();
  return succeeded;
}

bool StepDefaultOutputVolume(bool increase) {
  const auto endpoint = DefaultOutputEndpointInfo();
  if (!endpoint) return false;
  const bool succeeded = SUCCEEDED(
      increase ? endpoint->volume->VolumeStepUp(nullptr)
               : endpoint->volume->VolumeStepDown(nullptr));
  if (!succeeded) ResetCachedEndpoint();
  return succeeded;
}

bool SetDefaultOutputMute(bool muted) {
  const auto endpoint = DefaultOutputEndpointInfo();
  if (!endpoint) return false;
  const bool succeeded = SUCCEEDED(
      endpoint->volume->SetMute(muted ? TRUE : FALSE, nullptr));
  if (!succeeded) ResetCachedEndpoint();
  return succeeded;
}

bool ToggleDefaultOutputMute() {
  const auto endpoint = DefaultOutputEndpointInfo();
  if (!endpoint) return false;

  BOOL muted = FALSE;
  if (FAILED(endpoint->volume->GetMute(&muted))) {
    ResetCachedEndpoint();
    return false;
  }
  const bool succeeded =
      SUCCEEDED(endpoint->volume->SetMute(muted == FALSE, nullptr));
  if (!succeeded) ResetCachedEndpoint();
  return succeeded;
}

void SetDefaultOutputEndpointCacheEnabled(bool enabled) {
  endpointCacheEnabled = enabled;
  if (!enabled) ResetCachedEndpoint();
}

}  // namespace feathercast::audio
