#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>

namespace sr::gfx {
// Failure-only host diagnostics. Preserves the caller's HRESULT and performs no
// GPU work, resource retirement, retries, or policy changes. Missing observations
// are logged as unavailable. DRED traversal and logger drain are bounded.
void reportNativeGpuFailure(ID3D12Device* device, IDXGIFactory4* factory,
                            const char* where, HRESULT original) noexcept;
}
