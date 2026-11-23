// Tells whether the GPU has finished the previously submitted frame, using a
// Direct3D 11 event query issued right before each Present. This is the
// standards-based way to observe how far the CPU is running ahead of the GPU.
//
// All methods must be called on the thread that presents (the thread that
// owns the immediate context).
#pragma once
#include <d3d11.h>

namespace bll {

class GpuSync {
public:
    ~GpuSync() { shutdown(); }

    bool init(ID3D11Device* device);
    void shutdown();
    bool ready() const { return context_ != nullptr; }

    // Call immediately before Present.
    void mark_frame_end();
    // True while the most recently marked frame is still being processed.
    // Returns false if that cannot be determined.
    bool pending();

private:
    static constexpr int kQueries = 2;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11Query* queries_[kQueries] = {};
    ID3D11Query* last_ = nullptr;
    int next_ = 0;
};

} // namespace bll
