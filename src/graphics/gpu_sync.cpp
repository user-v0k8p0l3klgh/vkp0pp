#include "graphics/gpu_sync.h"

#include "core/log.h"

namespace bll {

bool GpuSync::init(ID3D11Device* device) {
    shutdown();
    if (!device) return false;
    D3D11_QUERY_DESC desc{};
    desc.Query = D3D11_QUERY_EVENT;
    for (ID3D11Query*& q : queries_) {
        const HRESULT hr = device->CreateQuery(&desc, &q);
        if (FAILED(hr)) {
            BLL_WARN("gpu sync: CreateQuery failed (0x%08lX)", static_cast<unsigned long>(hr));
            shutdown();
            return false;
        }
    }
    device->GetImmediateContext(&context_);
    return context_ != nullptr;
}

void GpuSync::shutdown() {
    for (ID3D11Query*& q : queries_) {
        if (q) q->Release();
        q = nullptr;
    }
    if (context_) context_->Release();
    context_ = nullptr;
    last_ = nullptr;
}

void GpuSync::mark_frame_end() {
    if (!context_) return;
    // Event queries have no Begin; End marks the point in the command stream.
    last_ = queries_[next_];
    next_ = (next_ + 1) % kQueries;
    context_->End(last_);
}

bool GpuSync::pending() {
    if (!context_ || !last_) return false;
    BOOL done = FALSE;
    // Present already flushed the command stream, so no flush is forced here.
    const HRESULT hr = context_->GetData(last_, &done, sizeof done, D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (hr == S_FALSE) return true;
    if (FAILED(hr)) {
        // Device removed or query lost: stop reporting, never block on it.
        last_ = nullptr;
        return false;
    }
    return !done;
}

} // namespace bll
