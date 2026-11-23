// API-neutral gamepad snapshot. Both Windows.Gaming.Input readings and XInput
// states are converted to and from this form at the hook boundary without
// losing precision (axes stay double, XInput's integer ranges round-trip).
#pragma once
#include <cstdint>

namespace bll {

struct PadState {
    uint32_t buttons = 0; // backend-specific bit mask, opaque to the latch
    double left_trigger = 0.0;  // 0..1
    double right_trigger = 0.0; // 0..1
    double left_x = 0.0, left_y = 0.0;   // -1..1
    double right_x = 0.0, right_y = 0.0; // -1..1
    uint64_t device_timestamp = 0;       // backend-specific, 0 if none
};

} // namespace bll
