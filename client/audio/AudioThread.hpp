#pragma once

#include <windows.h>
#include <avrt.h>

namespace vibez::audio {

// Windows 10+ throttles background processes (EcoQoS). A minimized voice app
// must keep timer resolution and execution speed untouched or the WASAPI
// callbacks are delayed until the stream underruns.
inline void EnableAudioProcessPowerThrottling() {
#if defined(PROCESS_POWER_THROTTLING_CURRENT_VERSION)
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    // Claim the execution-speed policy and leave it disabled: Windows then
    // does not place the minimized process in EcoQoS while audio is running.
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
#endif
}

// Registers the calling thread with the Windows Multimedia Class Scheduler so
// audio work keeps its share of CPU even when the window is minimized.
class AudioThreadScope {
public:
    AudioThreadScope() {
        DWORD taskIndex = 0;
        handle_ = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
    }
    ~AudioThreadScope() {
        if (handle_) AvRevertMmThreadCharacteristics(handle_);
    }
    AudioThreadScope(const AudioThreadScope&) = delete;
    AudioThreadScope& operator=(const AudioThreadScope&) = delete;

private:
    HANDLE handle_{};
};

} // namespace vibez::audio
