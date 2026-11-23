#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <stddef.h>

#include <dxgi1_5.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.gaming.input.h>

#include "platform/abi.h"

#define SLOT(type, member) ((unsigned)(offsetof(type##Vtbl, member) / sizeof(void *)))

typedef __x_ABI_CWindows_CGaming_CInput_CIGamepad WgiGamepad;
typedef __x_ABI_CWindows_CGaming_CInput_CIGamepadStatics WgiGamepadStatics;
typedef __x_ABI_CWindows_CGaming_CInput_CGamepadReading WgiReading;

const unsigned bll_slot_release = SLOT(IUnknown, Release);
const unsigned bll_slot_factory_create_for_hwnd = SLOT(IDXGIFactory2, CreateSwapChainForHwnd);
const unsigned bll_slot_factory_create_for_core_window = SLOT(IDXGIFactory2, CreateSwapChainForCoreWindow);
const unsigned bll_slot_factory_create_for_composition = SLOT(IDXGIFactory2, CreateSwapChainForComposition);
const unsigned bll_slot_swapchain_present = SLOT(IDXGISwapChain1, Present);
const unsigned bll_slot_swapchain_resize_buffers = SLOT(IDXGISwapChain1, ResizeBuffers);
const unsigned bll_slot_swapchain_present1 = SLOT(IDXGISwapChain1, Present1);
const unsigned bll_slot_gamepad_get_current_reading = SLOT(__x_ABI_CWindows_CGaming_CInput_CIGamepad, GetCurrentReading);

/* Cross-check against the indices documented by other projects (Present = 8, Present1 = 22). */
_Static_assert(offsetof(IDXGISwapChain1Vtbl, Present) / sizeof(void *) == 8, "Present slot");
_Static_assert(offsetof(IDXGISwapChain1Vtbl, ResizeBuffers) / sizeof(void *) == 13, "ResizeBuffers slot");
_Static_assert(offsetof(IDXGISwapChain1Vtbl, Present1) / sizeof(void *) == 22, "Present1 slot");
_Static_assert(offsetof(IDXGIFactory2Vtbl, CreateSwapChainForCoreWindow) / sizeof(void *) == 16, "factory slot");
/* The slot is the same whether reached through IDXGISwapChain or any later revision. */
_Static_assert(offsetof(IDXGISwapChain4Vtbl, Present) == offsetof(IDXGISwapChainVtbl, Present), "vtable prefix");

_Static_assert(sizeof(BllGamepadReading) == sizeof(WgiReading), "GamepadReading size");
_Static_assert(offsetof(BllGamepadReading, buttons) == offsetof(WgiReading, Buttons), "Buttons");
_Static_assert(offsetof(BllGamepadReading, left_trigger) == offsetof(WgiReading, LeftTrigger), "LeftTrigger");
_Static_assert(offsetof(BllGamepadReading, right_thumbstick_y) == offsetof(WgiReading, RightThumbstickY), "RightThumbstickY");

void bll_release(void *unknown) {
    if (unknown) IUnknown_Release((IUnknown *)unknown);
}

void *bll_wgi_first_gamepad(void) {
    /* IID of Windows.Gaming.Input.IGamepadStatics, {8BBCE529-D49C-39E9-9560-E47DDE96B7C8}. */
    static const GUID iid_statics = {0x8bbce529, 0xd49c, 0x39e9, {0x95, 0x60, 0xe4, 0x7d, 0xde, 0x96, 0xb7, 0xc8}};
    static const WCHAR class_name[] = L"Windows.Gaming.Input.Gamepad";
    HSTRING_HEADER header;
    HSTRING name = NULL;
    WgiGamepadStatics *statics = NULL;
    __FIVectorView_1_Windows__CGaming__CInput__CGamepad *pads = NULL;
    WgiGamepad *pad = NULL;
    UINT32 count = 0;

    if (FAILED(WindowsCreateStringReference(class_name, ARRAYSIZE(class_name) - 1, &header, &name))) return NULL;
    if (FAILED(RoGetActivationFactory(name, &iid_statics, (void **)&statics)))
        return NULL;
    if (SUCCEEDED(__x_ABI_CWindows_CGaming_CInput_CIGamepadStatics_get_Gamepads(statics, &pads)) && pads) {
        if (SUCCEEDED(__FIVectorView_1_Windows__CGaming__CInput__CGamepad_get_Size(pads, &count)) && count > 0)
            __FIVectorView_1_Windows__CGaming__CInput__CGamepad_GetAt(pads, 0, &pad);
        __FIVectorView_1_Windows__CGaming__CInput__CGamepad_Release(pads);
    }
    __x_ABI_CWindows_CGaming_CInput_CIGamepadStatics_Release(statics);
    return pad;
}
