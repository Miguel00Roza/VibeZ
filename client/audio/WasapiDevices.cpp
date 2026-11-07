#include "client/audio/WasapiDevices.hpp"

#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <utility>

using Microsoft::WRL::ComPtr;

namespace vibez::audio {
namespace {
class ComScope {
public:
    ComScope() {
        result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owns_ = result_ == S_OK || result_ == S_FALSE;
    }
    ~ComScope() { if (owns_) CoUninitialize(); }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    [[nodiscard]] bool Usable() const noexcept { return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT result_{};
    bool owns_{};
};

void SetError(std::wstring* error, const wchar_t* text) {
    if (error) *error = text;
}

std::wstring DeviceName(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, properties.GetAddressOf()))) return L"Unknown device";
    PROPVARIANT value{};
    PropVariantInit(&value);
    std::wstring name = L"Unknown device";
    if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

std::wstring DefaultId(IMMDeviceEnumerator* enumerator, const EDataFlow flow) {
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, device.GetAddressOf()))) return {};
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || !id) return {};
    std::wstring result(id);
    CoTaskMemFree(id);
    return result;
}
} // namespace

std::vector<AudioDeviceInfo> EnumerateAudioDevices(const AudioDeviceFlow flow, std::wstring* error) {
    if (error) error->clear();
    ComScope com;
    if (!com.Usable()) {
        SetError(error, L"COM initialization failed");
        return {};
    }
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.GetAddressOf()));
    if (FAILED(hr)) {
        SetError(error, L"Could not create the audio device enumerator");
        return {};
    }
    const EDataFlow dataFlow = flow == AudioDeviceFlow::Capture ? eCapture : eRender;
    const auto defaultId = DefaultId(enumerator.Get(), dataFlow);
    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(dataFlow, DEVICE_STATE_ACTIVE, collection.GetAddressOf());
    if (FAILED(hr)) {
        SetError(error, L"Could not enumerate active audio devices");
        return {};
    }
    UINT count{};
    if (FAILED(collection->GetCount(&count))) {
        SetError(error, L"Could not read the audio device count");
        return {};
    }
    std::vector<AudioDeviceInfo> devices;
    devices.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.GetAddressOf()))) continue;
        LPWSTR rawId = nullptr;
        if (FAILED(device->GetId(&rawId)) || !rawId) continue;
        AudioDeviceInfo info;
        info.id = rawId;
        CoTaskMemFree(rawId);
        info.name = DeviceName(device.Get());
        info.isDefault = !defaultId.empty() && info.id == defaultId;
        devices.push_back(std::move(info));
    }
    return devices;
}

std::wstring DefaultAudioDeviceId(const AudioDeviceFlow flow) {
    ComScope com;
    if (!com.Usable()) return {};
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(enumerator.GetAddressOf())))) return {};
    return DefaultId(enumerator.Get(), flow == AudioDeviceFlow::Capture ? eCapture : eRender);
}

} // namespace vibez::audio
