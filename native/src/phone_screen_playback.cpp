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

#include <emmintrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

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
  ComPtr<IMFSample> outputSample;
  ComPtr<IMFMediaBuffer> outputBuffer;
  DWORD outputBufferCapacity = 0;
  DWORD outputBufferAlignment = 0;
  bool audio = false;

  void Reset() {
    transform.Reset();
    output.Reset();
    outputSample.Reset();
    outputBuffer.Reset();
    outputBufferCapacity = 0;
    outputBufferAlignment = 0;
  }

  void Start() {
    Check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    Check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
  }

  void SelectOutput() {
    output.Reset();
    outputSample.Reset();
    outputBuffer.Reset();
    outputBufferCapacity = 0;
    outputBufferAlignment = 0;
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
        const DWORD needed = std::max<DWORD>(info.cbSize, 4096);
        if (!outputSample || !outputBuffer || outputBufferCapacity < needed ||
            outputBufferAlignment != info.cbAlignment) {
          outputSample.Reset();
          outputBuffer.Reset();
          Check(MFCreateSample(&outputSample));
          Check(MFCreateAlignedMemoryBuffer(needed, info.cbAlignment, &outputBuffer));
          Check(outputSample->AddBuffer(outputBuffer.Get()));
          outputBufferCapacity = needed;
          outputBufferAlignment = info.cbAlignment;
        } else {
          Check(outputBuffer->SetCurrentLength(0));
        }
        sample = outputSample;
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
  const int kBd = bt709 ? 541 : 516;
  const int kGd = bt709 ? -55 : -100;
  const int kGe = bt709 ? -136 : -208;
  const int kRe = bt709 ? 459 : 409;
  const BYTE* uv = data + static_cast<std::size_t>(stride) * height;
  const auto channel = [](int value) { return static_cast<BYTE>(std::clamp(value >> 8, 0, 255)); };
  const __m128i v16 = _mm_set1_epi16(16);
  const __m128i v128_32 = _mm_set1_epi32(128);
  const __m128i v298 = _mm_setr_epi16(298, 0, 298, 0, 298, 0, 298, 0);
  const __m128i vBd = _mm_setr_epi16(static_cast<short>(kBd), 0, static_cast<short>(kBd), 0,
                                     static_cast<short>(kBd), 0, static_cast<short>(kBd), 0);
  const __m128i vGde = _mm_setr_epi16(static_cast<short>(kGd), static_cast<short>(kGe),
                                      static_cast<short>(kGd), static_cast<short>(kGe),
                                      static_cast<short>(kGd), static_cast<short>(kGe),
                                      static_cast<short>(kGd), static_cast<short>(kGe));
  const __m128i vRe = _mm_setr_epi16(0, static_cast<short>(kRe), 0, static_cast<short>(kRe),
                                     0, static_cast<short>(kRe), 0, static_cast<short>(kRe));
  const __m128i vAlpha = _mm_set1_epi8(static_cast<char>(0xFF));
  const __m128i vZero = _mm_setzero_si128();
  const __m128i v128_16 = _mm_set1_epi16(128);
  for (int y = 0; y < visibleHeight; ++y) {
    const BYTE* luma = data + static_cast<std::size_t>(y) * stride;
    const BYTE* chroma = uv + static_cast<std::size_t>(y / 2) * stride;
    BYTE* dest = frame->bgra.data() + static_cast<std::size_t>(y) * visibleWidth * 4;
    int x = 0;
    for (; x + 4 <= visibleWidth; x += 4) {
      // Load 4 luma bytes [y0 y1 y2 y3] -> 16-bit max(0, y - 16) -> 32-bit c = (y-16)*298.
      const int y4 = *reinterpret_cast<const int*>(luma + x);
      const __m128i y16 = _mm_subs_epu16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(y4), vZero), v16);
      const __m128i c32 = _mm_madd_epi16(_mm_unpacklo_epi16(y16, vZero), v298);
      // Load 2 chroma pairs [u0 v0 u1 v1] -> 16-bit signed [d0 e0 d1 e1] -> duplicate pairs
      // to [d0 e0 d0 e0 d1 e1 d1 e1].
      const int uv4 = *reinterpret_cast<const int*>(chroma + x);
      const __m128i uv16 = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(uv4), vZero), v128_16);
      const __m128i de = _mm_unpacklo_epi32(uv16, uv16);
      const __m128i b32 = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(c32, _mm_madd_epi16(de, vBd)), v128_32), 8);
      const __m128i g32 = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(c32, _mm_madd_epi16(de, vGde)), v128_32), 8);
      const __m128i r32 = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(c32, _mm_madd_epi16(de, vRe)), v128_32), 8);
      const __m128i b8 = _mm_packus_epi16(_mm_packs_epi32(b32, b32), vZero);
      const __m128i g8 = _mm_packus_epi16(_mm_packs_epi32(g32, g32), vZero);
      const __m128i r8 = _mm_packus_epi16(_mm_packs_epi32(r32, r32), vZero);
      const __m128i bg = _mm_unpacklo_epi8(b8, g8);
      const __m128i ra = _mm_unpacklo_epi8(r8, vAlpha);
      _mm_storeu_si128(reinterpret_cast<__m128i*>(dest + x * 4), _mm_unpacklo_epi16(bg, ra));
    }
    for (; x < visibleWidth; ++x) {
      const int c = std::max(0, static_cast<int>(luma[x]) - 16) * 298;
      const int d = static_cast<int>(chroma[x & ~1]) - 128;
      const int e = static_cast<int>(chroma[(x & ~1) + 1]) - 128;
      dest[x * 4] = channel(c + kBd * d + 128);
      dest[x * 4 + 1] = channel(c + kGd * d + kGe * e + 128);
      dest[x * 4 + 2] = channel(c + kRe * e + 128);
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
  std::vector<float> samples;
  std::size_t readPos = 0;
  ~AudioOutput() { if (client) client->Stop(); }
  bool HasSamples() const noexcept { return samples.size() - readPos >= 2; }
  void Reset() {
    if (client) client->Stop();
    render.Reset(); client.Reset(); samples.clear(); samples.shrink_to_fit(); readPos = 0; capacity = 0;
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
    const std::size_t queued = samples.size() - readPos;
    if (queued > maxSamples) readPos += queued - maxSamples;
    if (readPos >= 8192) {
      samples.erase(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(readPos));
      readPos = 0;
    }
  }
  void Pump(float gain) {
    if (!client || !HasSamples()) return;
    UINT32 padding = 0;
    Check(client->GetCurrentPadding(&padding));
    const std::size_t availableFrames = (samples.size() - readPos) / 2;
    const auto frames = std::min<UINT32>(capacity - std::min(capacity, padding), static_cast<UINT32>(availableFrames));
    if (!frames) return;
    BYTE* buffer = nullptr;
    Check(render->GetBuffer(frames, &buffer));
    auto* output = reinterpret_cast<float*>(buffer);
    const float* src = samples.data() + readPos;
    const UINT32 count = frames * 2;
    for (UINT32 i = 0; i < count; ++i) output[i] = src[i] * gain;
    readPos += count;
    if (readPos == samples.size()) {
      samples.clear();
      readPos = 0;
    } else if (readPos >= 8192) {
      samples.erase(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(readPos));
      readPos = 0;
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
  bool workerExited = false;
  std::atomic<float> gain{1.0f};
  std::jthread worker;

  explicit Impl(ScreenPlaybackCallbacks cb) : callbacks(std::move(cb)) {}
  ~Impl() { StopWorker(); }

  void StopWorker() {
    std::jthread old;
    {
      std::lock_guard lock(mutex);
      sessionId.clear();
      packets.Clear();
      statuses.clear();
      reset = true;
      workerExited = false;
      old = std::move(worker);
    }
    if (old.joinable()) {
      old.request_stop();
      wake.notify_all();
      old.join();
    }
  }

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
        if (output.client && output.HasSamples()) {
          wake.wait_for(lock, stop, std::chrono::milliseconds(8), ready);
        } else {
          wake.wait(lock, stop, ready);
        }
        if (stop.stop_requested()) break;
        if (reset) {
          video.Reset(); audio.Reset(); output.Reset();
          generation = 0; audioGeneration = 0; videoFailed = false; audioFailed = false;
          firstVideo = true; videoConfig.clear(); decoderSession = sessionId; reset = false;
          if (decoderSession.empty() && statuses.empty() && packets.Size() == 0) {
            workerExited = true;
            break;
          }
        }
        states.swap(statuses);
        packet = packets.Pop();
      }
      bool ended = false;
      for (const auto& state : states) {
        if (callbacks.status) callbacks.status(state);
        if (state.state == "stopped" || state.state == "error") {
          output.Reset(); video.Reset(); audio.Reset();
          videoFailed = true; audioFailed = true;
          ended = true;
        }
      }
      if (ended) {
        std::lock_guard lock(mutex);
        if (!reset && statuses.empty() && packets.Size() == 0 && sessionId == decoderSession) {
          workerExited = true;
          break;
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
            audioFailed = true; output.Reset(); audio.Reset();
          } else {
            if (!videoFailed) Report(decoderSession, "Windows could not decode the phone screen. Install the Media Feature Pack if required, then start a new session.", true);
            videoFailed = true; video.Reset(); output.Reset();
          }
        }
      }
      try { output.Pump(gain.load()); }
      catch (const std::exception&) {
        if (!audioFailed) Report(decoderSession, "The PC audio output is unavailable. Screen sharing continues without sound.", false);
        audioFailed = true; output.Reset();
      }
    }
    output.Reset(); video.Reset(); audio.Reset();
    if (SUCCEEDED(mf)) MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
  }
};

ScreenPlayback::ScreenPlayback(ScreenPlaybackCallbacks callbacks) : impl_(std::make_unique<Impl>(std::move(callbacks))) {}
ScreenPlayback::~ScreenPlayback() = default;

void ScreenPlayback::Begin(std::string sessionId) {
  if (sessionId.empty()) {
    impl_->StopWorker();
    return;
  }
  std::jthread finishedWorker;
  bool needSpawn = false;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->sessionId = std::move(sessionId);
    impl_->packets.Clear();
    impl_->statuses.clear();
    impl_->keyframes.Reset();
    impl_->reset = true;
    if (!impl_->worker.joinable() || impl_->workerExited) {
      finishedWorker = std::move(impl_->worker);
      impl_->workerExited = false;
      needSpawn = true;
    }
  }
  // Join any previously exited worker outside mutex before spawning a new one.
  if (finishedWorker.joinable()) finishedWorker.join();
  if (needSpawn) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->sessionId.empty() && !impl_->worker.joinable()) {
      impl_->worker = std::jthread([impl = impl_.get()](std::stop_token stop) { impl->Run(stop); });
    }
  }
  impl_->wake.notify_all();
}

void ScreenPlayback::End() { impl_->StopWorker(); }

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
