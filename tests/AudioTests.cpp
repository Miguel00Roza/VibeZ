#include "client/audio/AudioFormat.hpp"
#include "client/audio/AudioMixer.hpp"
#include "client/audio/BoundedQueue.hpp"
#include "client/audio/JitterBuffer.hpp"
#include "client/audio/OpusCodec.hpp"
#include "tests/TestSupport.hpp"

#include <chrono>
#include <cmath>
#include <vector>

int main() {
    using namespace vibez::audio;

    std::vector<std::int16_t> voice(kFrameSamples);
    for (std::size_t i = 0; i < voice.size(); ++i) voice[i] = static_cast<std::int16_t>(12000.0 * std::sin(static_cast<double>(i) * 0.12));
    VoiceEncoder encoder;
    VoiceDecoder decoder;
    const auto encodedVoice = encoder.Encode(voice);
    const auto decodedVoice = decoder.Decode(encodedVoice);
    CHECK(!encodedVoice.empty());
    CHECK(decodedVoice.size() == static_cast<std::size_t>(kFrameSamples));
    const auto concealedVoice = decoder.Decode({});
    CHECK(concealedVoice.size() == static_cast<std::size_t>(kFrameSamples));

    JitterBuffer jitter(4);
    const std::vector<std::uint8_t> firstPacket{1, 2};
    const std::vector<std::uint8_t> thirdPacket{5, 6};
    CHECK(jitter.Push(10, firstPacket));
    CHECK(jitter.Push(12, thirdPacket));
    CHECK(!jitter.Push(12, thirdPacket));
    CHECK(jitter.PopNext() == firstPacket);
    const auto lostPacket = jitter.PopNext();
    CHECK(lostPacket && lostPacket->empty());
    CHECK(jitter.PopNext() == thirdPacket);
    CHECK(!jitter.PopNext());
    CHECK(!jitter.Push(20, firstPacket));

    JitterBuffer wrappingJitter(4);
    CHECK(wrappingJitter.Push(UINT32_MAX - 1, firstPacket));
    CHECK(wrappingJitter.Push(0, thirdPacket));
    CHECK(wrappingJitter.PopNext() == firstPacket);
    const auto wrappedLoss = wrappingJitter.PopNext();
    CHECK(wrappedLoss && wrappedLoss->empty());
    CHECK(wrappingJitter.PopNext() == thirdPacket);

    BoundedQueue<int> queue(2);
    CHECK(queue.TryPush(1) && queue.TryPush(2) && !queue.TryPush(3));
    CHECK(queue.TryPop() == 1 && queue.TryPop() == 2 && !queue.TryPop());

    BlockingQueue<int> blocking(2);
    CHECK(blocking.TryPush(7));
    int value{};
    CHECK(blocking.WaitPop(value, std::chrono::milliseconds(10)) && value == 7);
    blocking.Close();
    CHECK(!blocking.TryPush(8));
    CHECK(!blocking.WaitPop(value, std::chrono::milliseconds(10)));

    // The streaming queues drop the oldest sample/frame when full so live
    // audio and paced video never block the producer.
    BlockingQueue<int> dropping(2);
    CHECK(dropping.TryPush(1));
    CHECK(dropping.TryPush(2));
    CHECK(dropping.TryPush(3));
    CHECK(dropping.WaitPop(value, std::chrono::milliseconds(10)) && value == 2);
    CHECK(dropping.WaitPop(value, std::chrono::milliseconds(10)) && value == 3);

    LinearResampler down;
    down.Configure(48000, 24000);
    std::vector<float> input(4800, 1.0F);
    down.Push(input);
    CHECK(down.Available() >= 2399);
    std::vector<float> output(2400);
    down.Pull(output.size(), output);
    CHECK(std::abs(output.front() - 1.0F) < 0.001F);
    CHECK(std::abs(output.back() - 1.0F) < 0.001F);

    LinearResampler up;
    up.Configure(24000, 48000);
    std::vector<float> shortInput(2400, 0.5F);
    up.Push(shortInput);
    std::vector<float> longOutput(2400);
    up.Pull(longOutput.size(), longOutput);
    CHECK(std::abs(longOutput.front() - 0.5F) < 0.001F);

    PcmFramer framer;
    std::size_t frames{};
    std::vector<std::int16_t> first(900, 100);
    std::vector<std::int16_t> second(120, 200);
    framer.Push(first, [&](std::span<const std::int16_t> frame) { CHECK(frame.size() == static_cast<std::size_t>(kFrameSamples)); ++frames; });
    CHECK(frames == 0);
    framer.Push(second, [&](std::span<const std::int16_t> frame) { CHECK(frame.size() == static_cast<std::size_t>(kFrameSamples)); ++frames; });
    CHECK(frames == 1);
    CHECK(framer.Pending() == 60);

    CHECK(RmsLevel(std::vector<std::int16_t>(960, 0)) == 0.0F);
    CHECK(RmsLevel(std::vector<std::int16_t>(960, 16384)) > 0.4F);

    AudioMixer mixer;
    mixer.SetUserVolume(1, 0.5F);
    mixer.PushSamples(1, std::vector<std::int16_t>(960, 1000));
    std::vector<std::int16_t> mixed(960);
    mixer.Mix(mixed);
    CHECK(mixed.front() == 500);
    CHECK(mixer.PeakLevel(1) > 0.0F);
    mixer.SetUserMuted(1, true);
    mixer.Mix(mixed);
    CHECK(mixed.front() == 0);
    mixer.SetUserMuted(1, false);
    mixer.PushSamples(1, std::vector<std::int16_t>(960, 1000));
    mixer.SetMasterVolume(0.0F);
    mixer.Mix(mixed);
    CHECK(mixed.front() == 0);

    // Voice and shared-screen audio are independent streams: each has its own
    // gain, mute state and peak level so the UI can control them separately.
    AudioMixer separate;
    separate.SetUserVolume(2, 0.5F);
    separate.SetUserVolume(2, true, 1.0F);
    separate.PushSamples(2, std::vector<std::int16_t>(960, 1000));
    separate.PushSamples(2, true, std::vector<std::int16_t>(960, 2000));
    separate.Mix(mixed);
    CHECK(mixed.front() == 2500);
    CHECK(separate.PeakLevel(2) > 0.0F);
    CHECK(separate.PeakLevel(2, true) > 0.0F);
    CHECK(separate.PeakLevel(3, true) == 0.0F);

    separate.SetUserMuted(2, true); // Mutes only the microphone stream.
    separate.PushSamples(2, true, std::vector<std::int16_t>(960, 2000));
    separate.Mix(mixed);
    CHECK(mixed.front() == 2000);
    CHECK(separate.PeakLevel(2) == 0.0F);
    CHECK(separate.PeakLevel(2, true) > 0.0F);

    separate.RemoveUser(2);
    CHECK(separate.PeakLevel(2, true) == 0.0F);

    return vibez::test::Finish("Audio tests");
}
