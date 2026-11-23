// Gamepad integration. Both controller APIs a UWP title can use are covered:
// Windows.Gaming.Input (IGamepad::GetCurrentReading, via vtable) and XInput
// (XInputGetState, via the import table). Which one the game really calls is
// reported in the log from the observed call counts.
//
// The hooks do two things:
//   - timestamp every poll made by the game, which yields the input-age and
//     polls-per-frame statistics;
//   - optionally run the transition latch so that button and trigger taps
//     shorter than the game's poll interval are not lost.
// Stick values always come straight from the device read made inside the
// game's own call; they are never buffered, smoothed or delayed.
#include <windows.h>
#include <roapi.h>
#include <xinput.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>

#include "core/log.h"
#include "input/latched_pad.h"
#include "platform/abi.h"
#include "platform/hook.h"
#include "runtime.h"

namespace bll {

namespace {

using GetCurrentReadingFn = HRESULT(STDMETHODCALLTYPE*)(void*, BllGamepadReading*);
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

enum class PadKind { None, Wgi, XInput };

struct PadSlot {
    std::atomic<void*> key{nullptr}; // IGamepad* or an XInput user index tag
    PadKind kind = PadKind::None;
    std::unique_ptr<LatchedPad> pad;
};

constexpr int kSlots = 8;
constexpr Ticks kSlotStale = 5 * kNsPerSec; // a slot not polled for this long may be reused
constexpr Ticks kSamplerIdle = kNsPerSec;   // stop sampling a pad the game stopped polling
constexpr uintptr_t kXInputTag = 0x1000;

VtableHook g_hook_reading;
void* g_xinput_original = nullptr;
size_t g_xinput_slots = 0;
PadSlot g_pads[kSlots];
std::mutex g_pads_mutex; // slot table changes and sampler passes
std::atomic<bool> g_latch_enabled{false};
std::atomic<bool> g_stop{false};
std::atomic<uint64_t> g_wgi_polls{0}, g_xinput_polls{0};
std::thread g_service;

PadState from_wgi(const BllGamepadReading& r) {
    PadState s;
    s.buttons = r.buttons;
    s.left_trigger = r.left_trigger;
    s.right_trigger = r.right_trigger;
    s.left_x = r.left_thumbstick_x;
    s.left_y = r.left_thumbstick_y;
    s.right_x = r.right_thumbstick_x;
    s.right_y = r.right_thumbstick_y;
    s.device_timestamp = r.timestamp;
    return s;
}

PadState from_xinput(const XINPUT_STATE& x) {
    PadState s;
    s.buttons = x.Gamepad.wButtons;
    s.left_trigger = x.Gamepad.bLeftTrigger / 255.0;
    s.right_trigger = x.Gamepad.bRightTrigger / 255.0;
    s.device_timestamp = x.dwPacketNumber;
    return s;
}

PadSlot* find_slot(void* key) {
    for (PadSlot& slot : g_pads)
        if (slot.key.load(std::memory_order_acquire) == key) return &slot;
    return nullptr;
}

// Slow path, first poll of a pad only.
PadSlot* add_slot(void* key, PadKind kind, Ticks now) {
    std::lock_guard lock(g_pads_mutex);
    if (PadSlot* existing = find_slot(key)) return existing;
    for (PadSlot& slot : g_pads) {
        void* const old = slot.key.load();
        if (old != nullptr && now - slot.pad->last_game_poll() < kSlotStale) continue;
        if (old != nullptr && slot.kind == PadKind::Wgi) bll_release(old);
        slot.pad->sample([](PadState&) { return false; }); // reset the latch
        // The sampler thread reads through this pointer, so keep the object alive.
        if (kind == PadKind::Wgi) static_cast<IUnknown*>(key)->AddRef();
        slot.kind = kind;
        slot.key.store(key, std::memory_order_release);
        return &slot;
    }
    return nullptr; // more pads than slots: these simply are not latched
}

HRESULT STDMETHODCALLTYPE hk_get_current_reading(void* self, BllGamepadReading* value) {
    const auto original = g_hook_reading.original<GetCurrentReadingFn>();
    Runtime* rt = runtime();
    if (!rt || !value) return original(self, value);

    const Ticks now = now_ns();
    rt->governor.note_input_poll(now);
    g_wgi_polls.fetch_add(1, std::memory_order_relaxed);
    if (!g_latch_enabled.load(std::memory_order_relaxed)) return original(self, value);

    PadSlot* slot = find_slot(self);
    if (!slot) slot = add_slot(self, PadKind::Wgi, now);
    if (!slot) return original(self, value);

    HRESULT hr = S_OK;
    BllGamepadReading raw{};
    PadState out;
    const bool ok = slot->pad->poll(
        [&](PadState& s) {
            hr = original(self, &raw);
            if (FAILED(hr)) return false;
            s = from_wgi(raw);
            return true;
        },
        now, out);
    if (!ok) return hr;
    *value = raw; // sticks and timestamp exactly as read
    value->buttons = out.buttons;
    value->left_trigger = out.left_trigger;
    value->right_trigger = out.right_trigger;
    return hr;
}

DWORD WINAPI hk_xinput_get_state(DWORD index, XINPUT_STATE* state) {
    const auto original = reinterpret_cast<XInputGetStateFn>(g_xinput_original);
    Runtime* rt = runtime();
    if (!rt || !state) return original(index, state);

    const Ticks now = now_ns();
    if (!g_latch_enabled.load(std::memory_order_relaxed)) {
        const DWORD rc = original(index, state);
        // Games poll all four user indices; only connected pads count as input polls.
        if (rc == ERROR_SUCCESS) {
            rt->governor.note_input_poll(now);
            g_xinput_polls.fetch_add(1, std::memory_order_relaxed);
        }
        return rc;
    }

    void* const key = reinterpret_cast<void*>(kXInputTag + index);
    PadSlot* slot = find_slot(key);
    if (!slot) slot = add_slot(key, PadKind::XInput, now);
    if (!slot) return original(index, state);

    DWORD rc = ERROR_SUCCESS;
    XINPUT_STATE raw{};
    PadState in, out;
    const bool ok = slot->pad->poll(
        [&](PadState& s) {
            rc = original(index, &raw);
            if (rc != ERROR_SUCCESS) return false;
            s = in = from_xinput(raw);
            return true;
        },
        now, out);
    if (!ok) return rc;
    rt->governor.note_input_poll(now);
    g_xinput_polls.fetch_add(1, std::memory_order_relaxed);
    *state = raw; // sticks and packet number exactly as read
    state->Gamepad.wButtons = static_cast<WORD>(out.buttons);
    // Trigger bytes are only rewritten while a missed pull is being replayed.
    if (out.left_trigger != in.left_trigger)
        state->Gamepad.bLeftTrigger = static_cast<BYTE>(std::lround(out.left_trigger * 255.0));
    if (out.right_trigger != in.right_trigger)
        state->Gamepad.bRightTrigger = static_cast<BYTE>(std::lround(out.right_trigger * 255.0));
    return rc;
}

// One sampler pass over the pads the game is currently polling.
bool sample_active_pads(Ticks now) {
    bool any = false;
    std::lock_guard lock(g_pads_mutex);
    for (PadSlot& slot : g_pads) {
        void* const key = slot.key.load(std::memory_order_acquire);
        if (!key || now - slot.pad->last_game_poll() > kSamplerIdle) continue;
        any = true;
        if (slot.kind == PadKind::Wgi) {
            const auto original = g_hook_reading.original<GetCurrentReadingFn>();
            slot.pad->sample([&](PadState& s) {
                BllGamepadReading raw{};
                if (FAILED(original(key, &raw))) return false;
                s = from_wgi(raw);
                return true;
            });
        } else {
            const auto original = reinterpret_cast<XInputGetStateFn>(g_xinput_original);
            const DWORD index = static_cast<DWORD>(reinterpret_cast<uintptr_t>(key) - kXInputTag);
            slot.pad->sample([&](PadState& s) {
                XINPUT_STATE raw{};
                if (original(index, &raw) != ERROR_SUCCESS) return false;
                s = from_xinput(raw);
                return true;
            });
        }
    }
    return any;
}

// Finds a connected gamepad to reach the IGamepad vtable. Gamepads can be
// connected at any time, so this is retried until it succeeds.
bool try_hook_wgi() {
    void* const pad = bll_wgi_first_gamepad();
    if (!pad) return false;
    const bool ok = g_hook_reading.install(pad, bll_slot_gamepad_get_current_reading,
                                           reinterpret_cast<void*>(&hk_get_current_reading));
    bll_release(pad);
    BLL_INFO("Windows.Gaming.Input: IGamepad::GetCurrentReading hook %s", ok ? "installed" : "FAILED");
    return ok;
}

void service_thread(int sampler_hz) {
    const HRESULT ro = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(ro)) BLL_WARN("RoInitialize failed (0x%08lX); Windows.Gaming.Input hook unavailable", static_cast<unsigned long>(ro));

    WaitBackend& backend = system_wait_backend();
    const Ticks period = kNsPerSec / sampler_hz;
    bool wgi_hooked = false;
    bool boosted = false;
    Ticks next_wgi_try = 0;
    Ticks next_sample = now_ns();

    while (!g_stop.load(std::memory_order_relaxed)) {
        Ticks now = now_ns();
        if (!wgi_hooked && SUCCEEDED(ro) && now >= next_wgi_try) {
            wgi_hooked = try_hook_wgi();
            next_wgi_try = now + 500 * kNsPerMs;
        }
        const bool latch = g_latch_enabled.load(std::memory_order_relaxed);
        if (wgi_hooked && !latch) break; // nothing left to do on this thread

        if (latch && !boosted) {
            // The sampler does microseconds of work per millisecond but must not
            // be starved by render workers: a tap it sleeps through is lost.
            // (Measured: on a CPU-saturated machine it missed 6 % of 5 ms taps
            // at normal priority.)
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            boosted = true;
        }
        if (latch && sample_active_pads(now)) {
            next_sample += period;
            now = now_ns();
            if (next_sample < now) next_sample = now; // fell behind: do not burst
            backend.sleep_until(next_sample);         // timer only, no busy-wait
        } else {
            Sleep(100); // game not polling (menu without pad, suspended): stay idle
            next_sample = now_ns();
        }
    }
    if (SUCCEEDED(ro)) RoUninitialize();
}

} // namespace

void install_gamepad_hooks() {
    Runtime* rt = runtime();
    if (!rt || !rt->cfg.optimize_gamepad) {
        BLL_INFO("gamepad hooks disabled by configuration");
        return;
    }
    for (PadSlot& slot : g_pads)
        slot.pad = std::make_unique<LatchedPad>(rt->cfg.trigger_press_threshold, rt->cfg.trigger_release_threshold);
    g_latch_enabled.store(rt->active && rt->cfg.buffer_button_transitions);

    g_xinput_slots = patch_imports("xinput", "XInputGetState", reinterpret_cast<void*>(&hk_xinput_get_state),
                                   &g_xinput_original);
    BLL_INFO("XInput: %zu XInputGetState import slot(s) redirected%s", g_xinput_slots,
             g_xinput_slots ? "" : " (the executable does not import it)");
    BLL_INFO("button transition latch: %s (sampler %d Hz)", g_latch_enabled.load() ? "on" : "off", rt->cfg.sampler_hz);

    g_stop.store(false);
    g_service = std::thread(service_thread, rt->cfg.sampler_hz);
    g_service.detach();
}

void remove_gamepad_hooks() {
    g_stop.store(true);
    g_latch_enabled.store(false);
    g_hook_reading.remove();
    unpatch_imports();
}

std::string gamepad_status() {
    TransitionLatch::Stats total;
    for (PadSlot& slot : g_pads) {
        if (!slot.pad || !slot.key.load()) continue;
        const TransitionLatch::Stats s = slot.pad->stats();
        total.replayed += s.replayed;
        total.collapsed += s.collapsed;
        total.resets += s.resets;
    }
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "pad: wgi_hooked=%d wgi_polls=%llu xinput_slots=%zu xinput_polls=%llu latch=%d taps_preserved=%llu "
                  "collapsed=%llu stale_resets=%llu",
                  g_hook_reading.installed(), static_cast<unsigned long long>(g_wgi_polls.load()), g_xinput_slots,
                  static_cast<unsigned long long>(g_xinput_polls.load()), g_latch_enabled.load(),
                  static_cast<unsigned long long>(total.replayed), static_cast<unsigned long long>(total.collapsed),
                  static_cast<unsigned long long>(total.resets));
    return buf;
}

} // namespace bll
