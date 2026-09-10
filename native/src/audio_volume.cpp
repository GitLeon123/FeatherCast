#include "audio_volume.hpp"

#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

namespace feathercast::audio {
namespace {

using Microsoft::WRL::ComPtr;

ComPtr<IAudioEndpointVolume> cachedEndpoint;
bool endpointCacheEnabled = false;

ComPtr<IAudioEndpointVolume> DefaultOutputEndpoint() {
  if (endpointCacheEnabled && cachedEndpoint) return cachedEndpoint;
  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(enumerator.GetAddressOf())))) {
    return nullptr;
  }

  ComPtr<IMMDevice> device;
  if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                 device.GetAddressOf()))) {
    return nullptr;
  }

  ComPtr<IAudioEndpointVolume> volume;
  if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                              nullptr, reinterpret_cast<void**>(
                                           volume.GetAddressOf())))) {
    return nullptr;
  }
  if (endpointCacheEnabled) cachedEndpoint = volume;
  return volume;
}

}  // namespace

std::optional<int> ReadDefaultOutputVolumePercent() {
  const auto volume = DefaultOutputEndpoint();
  if (!volume) return std::nullopt;

  float scalar = 0.0f;
  if (FAILED(volume->GetMasterVolumeLevelScalar(&scalar))) {
    cachedEndpoint.Reset();
    return std::nullopt;
  }
  return ClampPercent(static_cast<int>(std::lround(scalar * 100.0f)));
}

bool SetDefaultOutputVolumePercent(int percent) {
  const auto volume = DefaultOutputEndpoint();
  if (!volume) return false;
  const float scalar = static_cast<float>(ClampPercent(percent)) / 100.0f;
  const bool succeeded = SUCCEEDED(volume->SetMasterVolumeLevelScalar(scalar, nullptr));
  if (!succeeded) cachedEndpoint.Reset();
  return succeeded;
}

bool StepDefaultOutputVolume(bool increase) {
  const auto volume = DefaultOutputEndpoint();
  if (!volume) return false;
  const bool succeeded = SUCCEEDED(increase ? volume->VolumeStepUp(nullptr)
                                            : volume->VolumeStepDown(nullptr));
  if (!succeeded) cachedEndpoint.Reset();
  return succeeded;
}

bool ToggleDefaultOutputMute() {
  const auto volume = DefaultOutputEndpoint();
  if (!volume) return false;

  BOOL muted = FALSE;
  if (FAILED(volume->GetMute(&muted))) {
    cachedEndpoint.Reset();
    return false;
  }
  const bool succeeded = SUCCEEDED(volume->SetMute(!muted, nullptr));
  if (!succeeded) cachedEndpoint.Reset();
  return succeeded;
}

void SetDefaultOutputEndpointCacheEnabled(bool enabled) {
  endpointCacheEnabled = enabled;
  if (!enabled) cachedEndpoint.Reset();
}

}  // namespace feathercast::audio
