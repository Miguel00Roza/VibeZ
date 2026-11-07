#include "client/audio/AudioFormat.hpp"
#include "client/audio/OpusCodec.hpp"
#include "client/audio/WasapiCapture.hpp"
#include "client/audio/WasapiDevices.hpp"
#include "client/audio/WasapiPlayback.hpp"
#include "tests/TestSupport.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    using namespace vibez::audio;

    std::wstring error;
    const auto captures = EnumerateAudioDevices(AudioDeviceFlow::Capture, &error);
    const auto renders = EnumerateAudioDevices(AudioDeviceFlow::Render, &error);
    std::wcout << L"Capture devices: " << captures.size() << L", render devices: " << renders.size() << L"\n";
    if (captures.empty() || renders.empty()) {
        std::cout << "Audio device test skipped: no active endpoint\n";
        return vibez::test::Finish("Audio device tests");
    }

    WasapiPlayback playback;
    std::string startError;
    CHECK(playback.Start(L"", [&](const std::span<std::int16_t> samples) {
        std::fill(samples.begin(), samples.end(), std::int16_t{0});
    }, startError));
    if (!startError.empty()) std::cerr << "Playback start: " << startError << '\n';
    CHECK(playback.IsRunning());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    playback.Stop();
    CHECK(!playback.IsRunning());

    WasapiCapture capture;
    std::atomic<std::size_t> frames{};
    std::atomic<std::size_t> samples{};
    CHECK(capture.Start(L"", [&](const std::span<const std::int16_t> frame, const std::uint64_t) {
        frames.fetch_add(1);
        samples.fetch_add(frame.size());
    }, startError));
    if (!startError.empty()) std::cerr << "Capture start: " << startError << '\n';
    CHECK(capture.IsRunning());
    for (int i = 0; i < 25 && frames.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    capture.Stop();
    std::cout << "Captured frames: " << frames.load() << ", samples: " << samples.load() << '\n';
    CHECK(frames.load() >= 20);
    CHECK(samples.load() >= static_cast<std::size_t>(kFrameSamples) * 20);

    return vibez::test::Finish("Audio device tests");
}
