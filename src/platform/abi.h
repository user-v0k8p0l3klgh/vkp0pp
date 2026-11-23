// Binary-interface facts taken from the Windows SDK headers: COM vtable slot
// indices and the WinRT gamepad reading layout. The values are computed by the
// C compiler from the header declarations (abi.c), never hand-counted.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const unsigned bll_slot_release;                      // IUnknown::Release
extern const unsigned bll_slot_factory_create_for_hwnd;      // IDXGIFactory2::CreateSwapChainForHwnd
extern const unsigned bll_slot_factory_create_for_core_window; // IDXGIFactory2::CreateSwapChainForCoreWindow
extern const unsigned bll_slot_factory_create_for_composition; // IDXGIFactory2::CreateSwapChainForComposition
extern const unsigned bll_slot_swapchain_present;            // IDXGISwapChain::Present
extern const unsigned bll_slot_swapchain_resize_buffers;     // IDXGISwapChain::ResizeBuffers
extern const unsigned bll_slot_swapchain_present1;           // IDXGISwapChain1::Present1
extern const unsigned bll_slot_gamepad_get_current_reading;  // Windows.Gaming.Input.IGamepad::GetCurrentReading

// Windows.Gaming.Input.GamepadReading.
typedef struct BllGamepadReading {
    uint64_t timestamp;
    uint32_t buttons;
    double left_trigger;
    double right_trigger;
    double left_thumbstick_x;
    double left_thumbstick_y;
    double right_thumbstick_x;
    double right_thumbstick_y;
} BllGamepadReading;

// Returns an AddRef'd Windows.Gaming.Input.IGamepad* for the first connected
// gamepad, or NULL if none is connected or the API is unavailable. The calling
// thread must have initialised the Windows Runtime.
void* bll_wgi_first_gamepad(void);
void bll_release(void* unknown);

#ifdef __cplusplus
}
#endif
