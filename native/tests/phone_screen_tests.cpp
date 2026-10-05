#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "phone_screen_playback.hpp"
#include "phone_screen_ui.hpp"
#include "phone_service.hpp"
#include "test_framework.hpp"
#include "ui_event_queue.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

using namespace feathercast;
using Microsoft::WRL::ComPtr;
namespace {
void Check(HRESULT hr) { assert(SUCCEEDED(hr)); }
ComPtr<IMFMediaType> MediaType(GUID major, GUID subtype) {
  ComPtr<IMFMediaType> type; Check(MFCreateMediaType(&type));
  Check(type->SetGUID(MF_MT_MAJOR_TYPE, major)); Check(type->SetGUID(MF_MT_SUBTYPE, subtype)); return type;
}
ComPtr<IMFSample> Sample(const phone::Bytes& bytes, LONGLONG time, LONGLONG duration) {
  ComPtr<IMFMediaBuffer> buffer; Check(MFCreateMemoryBuffer(static_cast<DWORD>(bytes.size()), &buffer));
  BYTE* data = nullptr; Check(buffer->Lock(&data, nullptr, nullptr)); std::memcpy(data, bytes.data(), bytes.size());
  Check(buffer->Unlock()); Check(buffer->SetCurrentLength(static_cast<DWORD>(bytes.size())));
  ComPtr<IMFSample> sample; Check(MFCreateSample(&sample)); Check(sample->AddBuffer(buffer.Get()));
  Check(sample->SetSampleTime(time)); Check(sample->SetSampleDuration(duration)); return sample;
}
phone::Bytes Blob(IMFAttributes* attributes, const GUID& key) {
  UINT32 size = 0; Check(attributes->GetBlobSize(key, &size)); phone::Bytes data(size);
  Check(attributes->GetBlob(key, data.data(), size, nullptr)); return data;
}
phone::Bytes SampleBytes(IMFSample* sample) {
  ComPtr<IMFMediaBuffer> buffer; Check(sample->ConvertToContiguousBuffer(&buffer)); BYTE* data = nullptr; DWORD size = 0;
  Check(buffer->Lock(&data, nullptr, &size)); phone::Bytes bytes(data, data + size); Check(buffer->Unlock()); return bytes;
}

std::vector<phone::ScreenPacket> GenerateMediaFixture() {
  const auto file = std::filesystem::temp_directory_path() / (L"fc-screen-codec-" + std::to_wstring(GetCurrentProcessId()) + L".mp4");
  ComPtr<IMFSinkWriter> writer; Check(MFCreateSinkWriterFromURL(file.c_str(), nullptr, nullptr, &writer));
  auto video = MediaType(MFMediaType_Video, MFVideoFormat_H264);
  Check(video->SetUINT32(MF_MT_AVG_BITRATE, 400000)); Check(video->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
  Check(video->SetUINT32(MF_MT_MPEG2_PROFILE, 66));
  Check(MFSetAttributeSize(video.Get(), MF_MT_FRAME_SIZE, 64, 64)); Check(MFSetAttributeRatio(video.Get(), MF_MT_FRAME_RATE, 30, 1));
  Check(MFSetAttributeRatio(video.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
  DWORD videoStream = 0, audioStream = 0; Check(writer->AddStream(video.Get(), &videoStream));
  auto rgb = MediaType(MFMediaType_Video, MFVideoFormat_RGB32);
  Check(MFSetAttributeSize(rgb.Get(), MF_MT_FRAME_SIZE, 64, 64)); Check(MFSetAttributeRatio(rgb.Get(), MF_MT_FRAME_RATE, 30, 1));
  Check(rgb->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive)); Check(rgb->SetUINT32(MF_MT_DEFAULT_STRIDE, 64 * 4));
  Check(writer->SetInputMediaType(videoStream, rgb.Get(), nullptr));
  auto aac = MediaType(MFMediaType_Audio, MFAudioFormat_AAC);
  Check(aac->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000)); Check(aac->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2));
  Check(aac->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16)); Check(aac->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000));
  Check(writer->AddStream(aac.Get(), &audioStream));
  auto pcm = MediaType(MFMediaType_Audio, MFAudioFormat_PCM);
  Check(pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000)); Check(pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2));
  Check(pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16)); Check(pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4));
  Check(pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000)); Check(writer->SetInputMediaType(audioStream, pcm.Get(), nullptr));
  Check(writer->BeginWriting());
  phone::Bytes pixels(64 * 64 * 4);
  for (std::size_t i = 0; i < pixels.size(); i += 4) { pixels[i] = 8; pixels[i + 1] = 16; pixels[i + 2] = 240; pixels[i + 3] = 255; }
  phone::Bytes tone(1600 * 4);
  for (int i = 0; i < 12; ++i) {
    for (int j = 0; j < 1600; ++j) {
      const auto value = static_cast<std::int16_t>(std::sin((i * 1600 + j) * 440.0 * 6.283185307 / 48000) * 1000);
      std::memcpy(tone.data() + j * 4, &value, 2); std::memcpy(tone.data() + j * 4 + 2, &value, 2);
    }
    const auto start = static_cast<LONGLONG>(i) * 10'000'000 / 30;
    Check(writer->WriteSample(videoStream, Sample(pixels, start, 10'000'000 / 30).Get()));
    Check(writer->WriteSample(audioStream, Sample(tone, start, 10'000'000 / 30).Get()));
  }
  Check(writer->Finalize()); writer.Reset();
  ComPtr<IMFSourceReader> reader; Check(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader));
  ComPtr<IMFMediaType> nativeVideo, nativeAudio;
  Check(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &nativeVideo));
  Check(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &nativeAudio));
  Check(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, nativeVideo.Get()));
  Check(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, nativeAudio.Get()));
  phone::ScreenPacket videoConfig;
  videoConfig.kind = phone::ScreenPacketKind::VideoConfig; videoConfig.sessionId = "codec-test";
  videoConfig.generation = 1; videoConfig.width = 64; videoConfig.height = 64; videoConfig.data = Blob(nativeVideo.Get(), MF_MT_MPEG_SEQUENCE_HEADER);
  phone::ScreenPacket audioConfig; audioConfig.kind = phone::ScreenPacketKind::AudioConfig;
  audioConfig.sessionId = "codec-test"; audioConfig.generation = 1;
  audioConfig.data = Blob(nativeAudio.Get(), MF_MT_USER_DATA);
  // MFAudioFormat_AAC has the HEAACWAVEINFO extension before the ASC.
  if (audioConfig.data.size() > 12) audioConfig.data.erase(audioConfig.data.begin(), audioConfig.data.begin() + 12);
  std::vector<phone::ScreenPacket> packets{videoConfig, audioConfig};
  for (int i = 0; i < 200; ++i) {
    DWORD stream = 0, flags = 0; LONGLONG time = 0; ComPtr<IMFSample> sample;
    Check(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &stream, &flags, &time, &sample));
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    if (!sample) continue;
    phone::ScreenPacket packet; packet.sessionId = "codec-test"; packet.generation = 1; packet.ptsUs = time / 10;
    ComPtr<IMFMediaType> streamType; Check(reader->GetNativeMediaType(stream, 0, &streamType));
    GUID major{}; Check(streamType->GetGUID(MF_MT_MAJOR_TYPE, &major));
    packet.kind = major == MFMediaType_Video ? phone::ScreenPacketKind::Video : phone::ScreenPacketKind::Audio;
    UINT32 keyframe = 0; sample->GetUINT32(MFSampleExtension_CleanPoint, &keyframe); packet.keyframe = keyframe != 0;
    packet.data = SampleBytes(sample.Get());
    if (packet.kind == phone::ScreenPacketKind::Video) {
      phone::Bytes annexB; std::size_t offset = 0;
      while (offset + 4 <= packet.data.size()) {
        const auto* data = packet.data.data() + offset;
        const auto size = (static_cast<std::uint32_t>(data[0]) << 24) | (static_cast<std::uint32_t>(data[1]) << 16) | (static_cast<std::uint32_t>(data[2]) << 8) | data[3];
        if (!size || size > packet.data.size() - offset - 4) { annexB.clear(); break; }
        annexB.insert(annexB.end(), {0, 0, 0, 1});
        annexB.insert(annexB.end(), packet.data.begin() + offset + 4, packet.data.begin() + offset + 4 + size);
        offset += 4 + size;
      }
      if (!annexB.empty() && offset == packet.data.size()) packet.data = std::move(annexB);
    }
    packets.push_back(std::move(packet));
  }
  reader.Reset(); std::filesystem::remove(file);
  return packets;
}

void TestCodecs(const std::vector<phone::ScreenPacket>& packets) {
  std::mutex mutex; std::condition_variable wake; int frames = 0; std::vector<std::string> errors;
  phone::ScreenPlaybackCallbacks callbacks;
  callbacks.frame = [&](std::shared_ptr<const phone::ScreenFrame> frame) {
    assert(frame->width == 64 && frame->height == 64 && frame->bgra.size() == 64 * 64 * 4);
    const auto center = frame->bgra.data() + (32 * 64 + 32) * 4;
    assert(center[2] > 180 && center[0] < 70 && center[1] < 70);
    std::lock_guard lock(mutex); ++frames; wake.notify_all();
  };
  callbacks.status = [&](phone::ScreenPacket packet) {
    std::fprintf(stderr, "Screen playback status (%s): %s\n", packet.state.c_str(), packet.detail.c_str());
    // Headless CI hosts may have no audio endpoint. Playback deliberately
    // reports that as nonfatal and must still decode every video frame.
    if (packet.state == "audio-error") return;
    std::lock_guard lock(mutex); errors.push_back(packet.detail); wake.notify_all();
  };
  phone::ScreenPlayback playback(callbacks); playback.Begin("codec-test"); playback.SetAudio(false, 0);
  // Synchronize with decoded output, so a busy CI host does not turn a codec
  // correctness test into a queue-overflow test that discards its only IDR.
  for (const auto& packet : packets) {
    int before = 0;
    { std::lock_guard lock(mutex); before = frames; }
    playback.OnPacket(packet);
    if (packet.kind == phone::ScreenPacketKind::Video) {
      std::unique_lock lock(mutex);
      wake.wait_for(lock, std::chrono::seconds(10), [&] { return frames > before || !errors.empty(); });
      assert(errors.empty()); assert(frames > before);
    }
  }
  std::unique_lock lock(mutex);
  wake.wait_for(lock, std::chrono::seconds(5), [&] { return frames >= 3 || !errors.empty(); });
  for (const auto& error : errors) std::fprintf(stderr, "%s\n", error.c_str());
  std::fprintf(stderr, "Decoded frames: %d; encoded packets: %zu\n", frames, packets.size());
  assert(errors.empty()); assert(frames >= 3);
}

void SaveWindow(HWND window, const std::filesystem::path& path) {
  RECT rect{}; GetWindowRect(window, &rect); const int width = rect.right - rect.left, height = rect.bottom - rect.top;
  HDC source = GetWindowDC(window), memory = CreateCompatibleDC(source);
  BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
  void* pixels = nullptr; HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  const auto old = SelectObject(memory, bitmap);
  assert(PrintWindow(window, memory, 2));
  ComPtr<IWICImagingFactory> factory; Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
  ComPtr<IWICStream> stream; Check(factory->CreateStream(&stream)); Check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
  ComPtr<IWICBitmapEncoder> encoder; Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)); Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
  ComPtr<IWICBitmapFrameEncode> frame; Check(encoder->CreateNewFrame(&frame, nullptr)); Check(frame->Initialize(nullptr)); Check(frame->SetSize(width, height));
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA; Check(frame->SetPixelFormat(&format));
  Check(frame->WritePixels(height, width * 4, width * height * 4, static_cast<BYTE*>(pixels))); Check(frame->Commit()); Check(encoder->Commit());
  SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window, source);
}

int LiveHarness(const std::filesystem::path& directory) {
  std::filesystem::create_directories(directory);
  phone_ui::PhoneScreenWindow window;
  phone::PhoneService service;
  const DWORD uiThread = GetCurrentThreadId(); MSG init{}; PeekMessageW(&init, nullptr, 0, 0, PM_NOREMOVE);
  runtime::UiEventQueue<phone::Event> events([&] { PostThreadMessageW(uiThread, WM_APP + 178, 0, 0); });
  std::atomic<int> frames{0}, audioPackets{0}, width{0}, height{0}, generation{0};
  std::string screenId;
  std::ofstream log(directory / "screen.log"); std::mutex logMutex;
  phone::ScreenPlaybackCallbacks cb;
  cb.frame = [&](auto frame) { ++frames; width = frame->width; height = frame->height; generation = frame->generation; };
  cb.status = [&](phone::ScreenPacket packet) { std::lock_guard lock(logMutex); log << packet.state << ": " << packet.detail << std::endl; };
  phone::ScreenPlayback diagnostics(cb);
  phone::ServiceConfig config; config.pcName = "Screen Test PC"; config.port = 0;  // free port; pairing.txt carries it
  config.stateFile = (directory / L"phone-link.dat").wstring(); config.downloadsDir = (directory / L"downloads").wstring();
  config.onEvent = [&](phone::Event event) { events.Push(std::move(event)); };
  config.onScreenPacket = [&](phone::ScreenPacket packet) {
    if (packet.kind == phone::ScreenPacketKind::Audio) ++audioPackets;
    diagnostics.OnPacket(packet); window.OnPacket(std::move(packet));
  };
  std::string error; assert(service.Start(config, &error));
  auto uri = service.CreatePairingUri();
  const auto addresses = uri.find("&h="), next = uri.find('&', addresses + 3);
  assert(addresses != std::string::npos); uri.replace(addresses + 3, next - addresses - 3, "10.0.2.2");
  std::ofstream(directory / "pairing.txt") << uri;
  phone_ui::ScreenCallbacks screenCallbacks;
  screenCallbacks.start = [&](bool audio) { screenId = service.StartScreen(audio); diagnostics.Begin(screenId); return screenId; };
  screenCallbacks.stop = [&] { service.StopScreen(); diagnostics.End(); };
  screenCallbacks.input = [&](phone::ScreenInput input) { return service.SendScreenInput(std::move(input)); };
  window.SetCallbacks(screenCallbacks); window.SetTheme(theme::Theme{}, theme::Color{.36f, .42f, 1, 1});
  window.SetConnection(false, false, "Android screen test"); window.Show();
  const auto start = GetTickCount64(); bool connected = false, available = false, captured = false; std::string name = "Android screen test";
  ULONGLONG statsAt = 0; bool requested = false;
  while (GetTickCount64() - start < 900'000) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_APP + 178) {
        for (const auto& event : events.Drain()) {
          if (event.kind == phone::EventKind::Connected) { connected = true; name = event.deviceName; }
          if (event.kind == phone::EventKind::Disconnected) connected = false;
          if (event.kind == phone::EventKind::Status) available = std::find(event.features.begin(), event.features.end(), "screen") != event.features.end();
          window.SetConnection(connected, available, name);
          if (connected && available && !requested) { requested = true; window.Show(); }
        }
      } else if (!window.HandleMessage(message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    const auto commandPath = directory / "command.json";
    if (std::filesystem::exists(commandPath)) {
      std::ifstream source(commandPath); const std::string text((std::istreambuf_iterator<char>(source)), {});
      source.close(); std::error_code ignored; std::filesystem::remove(commandPath, ignored);
      if (const auto command = json::Parse(text)) {
        const auto op = phone::JsonString(*command, "op");
        if (op == "text_scale") window.SetTextScale(static_cast<float>(phone::JsonInt(*command, "percent", 100)) / 100);
        else if (op == "resize") SetWindowPos(window.Hwnd(), nullptr, 40, 40, static_cast<int>(phone::JsonInt(*command, "width", 760)),
            static_cast<int>(phone::JsonInt(*command, "height", 960)), SWP_NOZORDER);
        else if (op == "button") SendMessageW(window.Hwnd(), WM_COMMAND, static_cast<WPARAM>(phone::JsonInt(*command, "id")), 0);
        else if (op == "text") {
          const auto utf8 = phone::JsonString(*command, "text");
          const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
          std::wstring wide(static_cast<std::size_t>(size), 0);
          MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
          for (const auto ch : wide) SendMessageW(window.Hwnd(), WM_CHAR, ch, 0);
        } else if (op == "key") SendMessageW(window.Hwnd(), WM_KEYDOWN, static_cast<WPARAM>(phone::JsonInt(*command, "key")), 0);
        else if (op == "mouse") {
          RECT rect{}; GetClientRect(window.Hwnd(), &rect); const float scale = GetDpiForWindow(window.Hwnd()) / 96.0f;
          const auto fit = phone::FitScreenRect({24, 112, rect.right / scale - 24, rect.bottom / scale - 108}, width, height);
          const auto x = (fit.left + (fit.right - fit.left) * static_cast<float>(phone::JsonInt(*command, "x")) / 1'000'000) * scale;
          const auto y = (fit.top + (fit.bottom - fit.top) * static_cast<float>(phone::JsonInt(*command, "y")) / 1'000'000) * scale;
          const auto action = phone::JsonString(*command, "action");
          const UINT mouseMessage = action == "down" ? WM_LBUTTONDOWN : action == "up" ? WM_LBUTTONUP : WM_MOUSEMOVE;
          SendMessageW(window.Hwnd(), mouseMessage, action == "up" ? 0 : MK_LBUTTON, MAKELPARAM(static_cast<int>(x), static_cast<int>(y)));
        } else if (op == "input") service.SendScreenInput({screenId, static_cast<int>(phone::JsonInt(*command, "generation", generation)),
            phone::JsonString(*command, "action"), static_cast<int>(phone::JsonInt(*command, "x")), static_cast<int>(phone::JsonInt(*command, "y")),
            static_cast<int>(phone::JsonInt(*command, "value")), phone::JsonString(*command, "text")});
        else if (op == "file") service.SendFile(std::filesystem::path(phone::JsonString(*command, "path")).wstring());
      }
    }
    if (frames > 5 && (!captured || std::filesystem::exists(directory / "capture"))) {
      SaveWindow(window.Hwnd(), directory / "windows.png");
      captured = true;
      std::error_code ignored; std::filesystem::remove(directory / "capture", ignored);
    }
    if (GetTickCount64() - statsAt > 1000) {
      statsAt = GetTickCount64();
      RECT rect{}, client{}; GetWindowRect(window.Hwnd(), &rect); GetClientRect(window.Hwnd(), &client);
      std::ofstream(directory / "stats.txt") << "frames=" << frames << "\naudioPackets=" << audioPackets << "\ngeneration=" << generation << "\nwidth=" << width << "\nheight=" << height << '\n';
      std::ofstream(directory / "geometry.txt") << rect.left << ',' << rect.top << ',' << rect.right << ',' << rect.bottom << " client=" << client.right << ',' << client.bottom;
    }
    if (std::filesystem::exists(directory / "stop")) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  window.Close(); service.Stop(); return 0;
}
}

int wmain(int argc, wchar_t** argv) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  Check(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); Check(MFStartup(MF_VERSION));
  if (argc == 3 && std::wstring_view(argv[1]) == L"--live") return LiveHarness(argv[2]);
  const auto packets = GenerateMediaFixture(); TestCodecs(packets);
  MFShutdown(); CoUninitialize(); std::puts("phone screen codec tests passed"); return 0;
}
