#include "phone_screen_playback.hpp"

#include "phone_screen_buffer.hpp"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace feathercast::phone {
namespace {
using Microsoft::WRL::ComPtr;

// Status updates are few, but several can arrive between two worker wakeups
// (for example "streaming" followed by "stopped"); none of them may be lost.
constexpr std::size_t kMaxQueuedStatuses = 16;

long long SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void Check(HRESULT result) {
  if (FAILED(result)) throw std::runtime_error("Windows media operation failed.");
}

ComPtr<IMFSample> Sample(const Bytes& bytes, long long pts) {
  ComPtr<IMFMediaBuffer> buffer;
  Check(MFCreateMemoryBuffer(static_cast<DWORD>(bytes.size()), &buffer));
  BYTE* data = nullptr;
  Check(buffer->Lock(&data, nullptr, nullptr));
  std::memcpy(data, bytes.data(), bytes.size());
  Check(buffer->Unlock());
  Check(buffer->SetCurrentLength(static_cast<DWORD>(bytes.size())));
  ComPtr<IMFSample> sample;
  Check(MFCreateSample(&sample));
  Check(sample->AddBuffer(buffer.Get()));
  Check(sample->SetSampleTime(pts * 10));
  return sample;
}

ComPtr<IMFMediaType> Type(const GUID& major, const GUID& subtype) {
  ComPtr<IMFMediaType> type;
  Check(MFCreateMediaType(&type));
  Check(type->SetGUID(MF_MT_MAJOR_TYPE, major));
  Check(type->SetGUID(MF_MT_SUBTYPE, subtype));
  return type;
}

class Decoder {
 public:
  ComPtr<IMFTransform> transform;
  ComPtr<IMFMediaType> output;
  bool audio = false;

  void Start() {
    Check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    Check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
  }

  void SelectOutput() {
    output.Reset();
    for (DWORD i = 0; ; ++i) {
      ComPtr<IMFMediaType> candidate;
      if (FAILED(transform->GetOutputAvailableType(0, i, &candidate))) break;
      GUID subtype{};
      candidate->GetGUID(MF_MT_SUBTYPE, &subtype);
      if ((!audio && subtype == MFVideoFormat_NV12) || (audio && subtype == MFAudioFormat_Float)) {
        if (SUCCEEDED(transform->SetOutputType(0, candidate.Get(), 0))) {
          output = candidate;
          return;
        }
      }
    }
    throw std::runtime_error("Windows could not choose a decoder output format.");
  }

  template <typename Consumer>
  void Drain(Consumer consume) {
    for (int iteration = 0; iteration < 64; ++iteration) {
      MFT_OUTPUT_STREAM_INFO info{};
      Check(transform->GetOutputStreamInfo(0, &info));
      ComPtr<IMFSample> sample;
      if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        ComPtr<IMFMediaBuffer> buffer;
        Check(MFCreateSample(&sample));
        Check(MFCreateAlignedMemoryBuffer(std::max<DWORD>(info.cbSize, 4096), info.cbAlignment, &buffer));
        Check(sample->AddBuffer(buffer.Get()));
      }
      MFT_OUTPUT_DATA_BUFFER outputData{};
      outputData.pSample = sample.Get();
      DWORD status = 0;
      const HRESULT result = transform->ProcessOutput(0, 1, &outputData, &status);
      if (outputData.pEvents) outputData.pEvents->Release();
      if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
      if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
        if (!sample && outputData.pSample) outputData.pSample->Release();
        SelectOutput();
        continue;
      }
      Check(result);
      if (!sample) sample.Attach(outputData.pSample);
      if (sample) consume(sample.Get(), output.Get());
    }
  }

  template <typename Consumer>
  void Feed(const ScreenPacket& packet, Consumer consume) {
    if (!transform) return;
    const auto sample = Sample(packet.data, packet.ptsUs);
    if (packet.keyframe) sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    HRESULT result = transform->ProcessInput(0, sample.Get(), 0);
    if (result == MF_E_NOTACCEPTING) {
      Drain(consume);
      result = transform->ProcessInput(0, sample.Get(), 0);
    }
    Check(result);
    Drain(consume);
  }

  void ConfigureVideo(const ScreenPacket& packet) {
    audio = false;
    transform.Reset();
    Check(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform)));
    ComPtr<IMFAttributes> attributes;
    Check(transform->GetAttributes(&attributes));
    Check(attributes->SetUINT32(MF_LOW_LATENCY, TRUE));
    auto input = Type(MFMediaType_Video, MFVideoFormat_H264);
    Check(MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, packet.width, packet.height));
    Check(MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, 30, 1));
    Check(input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    Check(input->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, packet.data.data(), static_cast<UINT32>(packet.data.size())));
    Check(transform->SetInputType(0, input.Get(), 0));
    SelectOutput();
    Start();
    // Keep SPS/PPS with the first access unit too, for decoders that defer the
    // sequence header until ProcessInput.
  }

  void ConfigureAudio(const ScreenPacket& packet) {
    audio = true;
    transform.Reset();
    Check(CoCreateInstance(CLSID_CMSAACDecMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform)));
    auto input = Type(MFMediaType_Audio, MEDIASUBTYPE_RAW_AAC1);
    Check(input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000));
    Check(input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2));
    Check(input->SetBlob(MF_MT_USER_DATA, packet.data.data(), static_cast<UINT32>(packet.data.size())));
    Check(transform->SetInputType(0, input.Get(), 0));
    SelectOutput();
    Start();
  }
};

std::shared_ptr<ScreenFrame> VideoFrame(IMFSample* sample, IMFMediaType* type,
                                       int visibleWidth, int visibleHeight, int generation) {
  UINT32 width = 0, height = 0;
  Check(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height));
  if (width > 4096 || height > 4096 || width < static_cast<UINT32>(visibleWidth) ||
      height < static_cast<UINT32>(visibleHeight)) throw std::runtime_error("Invalid video dimensions.");
  ComPtr<IMFMediaBuffer> buffer;
  Check(sample->ConvertToContiguousBuffer(&buffer));
  BYTE* data = nullptr;
  DWORD length = 0;
  Check(buffer->Lock(&data, nullptr, &length));
  const auto unlock = [&] { buffer->Unlock(); };
  UINT32 rawStride = 0;
  if (FAILED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawStride))) rawStride = width;
  const LONG stride = static_cast<LONG>(rawStride);
  if (stride <= 0 || static_cast<UINT32>(stride) < width ||
      static_cast<std::uint64_t>(stride) * height * 3 / 2 > length) {
    unlock();
    throw std::runtime_error("Invalid decoded video buffer.");
  }
  auto frame = std::make_shared<ScreenFrame>();
  frame->width = visibleWidth; frame->height = visibleHeight; frame->generation = generation;
  frame->bgra.resize(static_cast<std::size_t>(visibleWidth) * visibleHeight * 4);
  UINT32 matrix = MFVideoTransferMatrix_BT709;
  type->GetUINT32(MF_MT_YUV_MATRIX, &matrix);
  const bool bt709 = matrix == MFVideoTransferMatrix_BT709;
  const BYTE* uv = data + static_cast<std::size_t>(stride) * height;
  const auto channel = [](int value) { return static_cast<BYTE>(std::clamp(value >> 8, 0, 255)); };
  for (int y = 0; y < visibleHeight; ++y) {
    const BYTE* luma = data + static_cast<std::size_t>(y) * stride;
    const BYTE* chroma = uv + static_cast<std::size_t>(y / 2) * stride;
    BYTE* dest = frame->bgra.data() + static_cast<std::size_t>(y) * visibleWidth * 4;
    for (int x = 0; x < visibleWidth; ++x) {
      const int c = std::max(0, static_cast<int>(luma[x]) - 16) * 298;
      const int d = static_cast<int>(chroma[x & ~1]) - 128;
      const int e = static_cast<int>(chroma[(x & ~1) + 1]) - 128;
      dest[x * 4] = channel(c + (bt709 ? 541 : 516) * d + 128);
      dest[x * 4 + 1] = channel(c - (bt709 ? 55 : 100) * d - (bt709 ? 136 : 208) * e + 128);
      dest[x * 4 + 2] = channel(c + (bt709 ? 459 : 409) * e + 128);
      dest[x * 4 + 3] = 255;
    }
  }
  unlock();
  return frame;
}

class AudioOutput {
 public:
  ComPtr<IAudioClient> client;
  ComPtr<IAudioRenderClient> render;
  UINT32 capacity = 0;
  std::deque<float> samples;
  ~AudioOutput() { if (client) client->Stop(); }
  void Reset() {
    if (client) client->Stop();
    render.Reset(); client.Reset(); samples.clear(); capacity = 0;
  }
  void Configure() {
    Reset();
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));
    Check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device));
    Check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf())));
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 2; format.nSamplesPerSec = 48000; format.wBitsPerSample = 32;
    format.nBlockAlign = 8; format.nAvgBytesPerSec = 384000;
    Check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        0, 0, &format, nullptr));
    Check(client->GetBufferSize(&capacity));
    Check(client->GetService(IID_PPV_ARGS(&render)));
    Check(client->Start());
  }
  void Queue(IMFSample* sample, IMFMediaType* type) {
    UINT32 channels = 0, rate = 0;
    Check(type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels));
    Check(type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate));
    if (channels != 2 || rate != 48000) throw std::runtime_error("Unsupported audio output format.");
    ComPtr<IMFMediaBuffer> buffer;
    Check(sample->ConvertToContiguousBuffer(&buffer));
    BYTE* data = nullptr;
    DWORD length = 0;
    Check(buffer->Lock(&data, nullptr, &length));
    const auto* values = reinterpret_cast<const float*>(data);
    samples.insert(samples.end(), values, values + length / sizeof(float));
    buffer->Unlock();
    constexpr std::size_t maxSamples = 48000 * 2 * 150 / 1000;
    while (samples.size() > maxSamples) samples.pop_front();
  }
  void Pump(float gain) {
    if (!client || samples.size() < 2) return;
    UINT32 padding = 0;
    Check(client->GetCurrentPadding(&padding));
    const auto frames = std::min<UINT32>(capacity - std::min(capacity, padding), static_cast<UINT32>(samples.size() / 2));
    if (!frames) return;
    BYTE* buffer = nullptr;
    Check(render->GetBuffer(frames, &buffer));
    auto* output = reinterpret_cast<float*>(buffer);
    for (UINT32 i = 0; i < frames * 2; ++i) {
      output[i] = samples.front() * gain;
      samples.pop_front();
    }
    Check(render->ReleaseBuffer(frames, 0));
  }
};
}  // namespace

struct ScreenPlayback::Impl {
  ScreenPlaybackCallbacks callbacks;
  std::mutex mutex;
  std::condition_variable_any wake;
  ScreenPacketBuffer packets;
  KeyframeThrottle keyframes;
  std::deque<ScreenPacket> statuses;
  std::string sessionId;
  bool reset = false;
  std::atomic<float> gain{1.0f};
  std::jthread worker;

  explicit Impl(ScreenPlaybackCallbacks cb) : callbacks(std::move(cb)) {
    worker = std::jthread([this](std::stop_token stop) { Run(stop); });
  }
  ~Impl() { worker.request_stop(); wake.notify_all(); if (worker.joinable()) worker.join(); }

  void Report(const std::string& id, std::string detail, bool fatal) {
    ScreenPacket packet;
    packet.sessionId = id;
    packet.state = fatal ? "error" : "audio-error";
    packet.detail = std::move(detail);
    if (callbacks.status) callbacks.status(std::move(packet));
  }

  void Run(std::stop_token stop) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HRESULT mf = SUCCEEDED(com) ? MFStartup(MF_VERSION) : E_FAIL;
    Decoder video, audio;
    AudioOutput output;
    Bytes videoConfig;
    bool firstVideo = true, audioFailed = false, videoFailed = false;
    int generation = 0, width = 0, height = 0, audioGeneration = 0;
    std::string decoderSession;
    while (!stop.stop_requested()) {
      std::optional<ScreenPacket> packet;
      std::deque<ScreenPacket> states;
      {
        std::unique_lock lock(mutex);
        const auto ready = [&] { return reset || !statuses.empty() || packets.Size() > 0; };
        if (output.client) wake.wait_for(lock, stop, std::chrono::milliseconds(3), ready);
        else wake.wait(lock, stop, ready);
        if (stop.stop_requested()) break;
        if (reset) {
          video.transform.Reset(); audio.transform.Reset(); output.Reset();
          generation = 0; audioGeneration = 0; videoFailed = false; audioFailed = false;
          firstVideo = true; videoConfig.clear(); decoderSession = sessionId; reset = false;
        }
        states.swap(statuses);
        packet = packets.Pop();
      }
      for (const auto& state : states) {
        if (callbacks.status) callbacks.status(state);
        if (state.state == "stopped" || state.state == "error") {
          output.Reset(); video.transform.Reset(); audio.transform.Reset();
          videoFailed = true; audioFailed = true;
        }
      }
      if (packet && packet->sessionId == decoderSession) {
        try {
          if (FAILED(mf)) throw std::runtime_error("Windows Media Foundation is unavailable.");
          switch (packet->kind) {
            case ScreenPacketKind::VideoConfig:
              video.ConfigureVideo(*packet);
              generation = packet->generation; width = packet->width; height = packet->height;
              videoConfig = packet->data; firstVideo = true; videoFailed = false;
              break;
            case ScreenPacketKind::Video:
              if (!videoFailed && packet->generation == generation) {
                if (firstVideo) {
                  if (!packet->keyframe) break;
                  packet->data.insert(packet->data.begin(), videoConfig.begin(), videoConfig.end());
                  firstVideo = false;
                }
                video.Feed(*packet, [&](IMFSample* sample, IMFMediaType* type) {
                  auto frame = VideoFrame(sample, type, width, height, generation);
                  frame->sessionId = decoderSession;
                  if (callbacks.frame) callbacks.frame(std::move(frame));
                });
              }
              break;
            case ScreenPacketKind::AudioConfig:
              if (!audioFailed) { audio.ConfigureAudio(*packet); output.Configure(); audioGeneration = packet->generation; }
              break;
            case ScreenPacketKind::Audio:
              if (!audioFailed && packet->generation == audioGeneration) {
                audio.Feed(*packet, [&](IMFSample* sample, IMFMediaType* type) { output.Queue(sample, type); });
              }
              break;
            case ScreenPacketKind::State: break;
          }
        } catch (const std::exception&) {
          const bool isAudio = packet->kind == ScreenPacketKind::Audio || packet->kind == ScreenPacketKind::AudioConfig;
          if (isAudio) {
            if (!audioFailed) Report(decoderSession, "Device audio is unavailable on this PC. Screen sharing and control remain active.", false);
            audioFailed = true; output.Reset(); audio.transform.Reset();
          } else {
            if (!videoFailed) Report(decoderSession, "Windows could not decode the phone screen. Install the Media Feature Pack if required, then start a new session.", true);
            videoFailed = true; video.transform.Reset(); output.Reset();
          }
        }
      }
      try { output.Pump(gain.load()); }
      catch (const std::exception&) {
        if (!audioFailed) Report(decoderSession, "The PC audio output is unavailable. Screen sharing continues without sound.", false);
        audioFailed = true; output.Reset();
      }
    }
    output.Reset(); video.transform.Reset(); audio.transform.Reset();
    if (SUCCEEDED(mf)) MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
  }
};

ScreenPlayback::ScreenPlayback(ScreenPlaybackCallbacks callbacks) : impl_(std::make_unique<Impl>(std::move(callbacks))) {}
ScreenPlayback::~ScreenPlayback() = default;

void ScreenPlayback::Begin(std::string sessionId) {
  std::lock_guard lock(impl_->mutex);
  impl_->sessionId = std::move(sessionId); impl_->packets.Clear(); impl_->statuses.clear();
  impl_->keyframes.Reset(); impl_->reset = true;
  impl_->wake.notify_all();
}

void ScreenPlayback::End() { Begin({}); }

void ScreenPlayback::OnPacket(ScreenPacket packet) {
  ScreenInput recovery;
  {
    std::lock_guard lock(impl_->mutex);
    if (packet.sessionId != impl_->sessionId || impl_->sessionId.empty()) return;
    if (packet.kind == ScreenPacketKind::State) {
      if (packet.state == "stopped" || packet.state == "error") impl_->packets.Clear();
      if (impl_->statuses.size() >= kMaxQueuedStatuses) impl_->statuses.pop_front();
      impl_->statuses.push_back(std::move(packet));
    } else {
      recovery.sessionId = packet.sessionId; recovery.generation = packet.generation; recovery.action = "keyframe";
      // Ask for a keyframe at most every 500 ms while delta frames are dropped.
      if (impl_->packets.Push(std::move(packet)) || !impl_->keyframes.Allow(SteadyMs())) recovery.sessionId.clear();
    }
  }
  impl_->wake.notify_all();
  if (!recovery.sessionId.empty() && impl_->callbacks.input) impl_->callbacks.input(std::move(recovery));
}

void ScreenPlayback::SetAudio(bool enabled, float volume) { impl_->gain = enabled ? std::clamp(volume, 0.0f, 1.0f) : 0.0f; }

}  // namespace feathercast::phone
