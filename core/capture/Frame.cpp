#include "Frame.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <vector>
#include <spdlog/spdlog.h>

std::vector<IDXGIOutput*> enumerate_outputs() {
    IDXGIFactory *DXGIFactory;
    CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void **>(&DXGIFactory));

    std::vector<IDXGIOutput*> vOutputs;

    UINT i = 0;
    UINT j = 0;
    IDXGIAdapter* pAdapter{};

    while (DXGIFactory->EnumAdapters(i, &pAdapter) != DXGI_ERROR_NOT_FOUND) {
        IDXGIOutput *pOutput;
        while (pAdapter->EnumOutputs(j, &pOutput) != DXGI_ERROR_NOT_FOUND) {
            vOutputs.push_back(pOutput);
            ++j;
        }
        ++i;
    }

    return vOutputs;
}
