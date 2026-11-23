//! Locating and hooking `IDXGISwapChain::Present / Present1 / ResizeBuffers /
//! ResizeBuffers1` and `ID3D12CommandQueue::ExecuteCommandLists`.
//!
//! The addresses are read from the vtables of throw-away objects created on a
//! hidden window (DX11 / DX12 devices + several kinds of swap chains). A swap
//! chain may reach `Present` through different functions depending on the
//! interface it is called through (IDXGISwapChain vs IDXGISwapChain1..4) and
//! the kind of swap chain (blt / flip / composition), so every distinct
//! address seen is hooked, each with its own const-generic detour slot.

use crate::error::{Error, Result};
use std::ffi::c_void;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use windows::Win32::Foundation::{HMODULE, HWND, LPARAM, LRESULT, WPARAM};
use windows::Win32::Graphics::Dxgi::*;
use windows::Win32::UI::WindowsAndMessaging::*;
use windows::core::{HRESULT, Interface, PCWSTR, w};

const SLOTS: usize = 8;
static PRESENT: [AtomicUsize; SLOTS] = [const { AtomicUsize::new(0) }; SLOTS];
static PRESENT1: [AtomicUsize; SLOTS] = [const { AtomicUsize::new(0) }; SLOTS];
static RESIZE: [AtomicUsize; SLOTS] = [const { AtomicUsize::new(0) }; SLOTS];
static RESIZE1: [AtomicUsize; SLOTS] = [const { AtomicUsize::new(0) }; SLOTS];
#[cfg(feature = "dx12")]
static EXECUTE: AtomicUsize = AtomicUsize::new(0);

type PresentFn = unsafe extern "system" fn(*mut c_void, u32, DXGI_PRESENT) -> HRESULT;
type Present1Fn = unsafe extern "system" fn(*mut c_void, u32, DXGI_PRESENT, *const DXGI_PRESENT_PARAMETERS) -> HRESULT;
type ResizeFn = unsafe extern "system" fn(*mut c_void, u32, u32, u32, i32, u32) -> HRESULT;
type Resize1Fn =
    unsafe extern "system" fn(*mut c_void, u32, u32, u32, i32, u32, *const u32, *const *mut c_void) -> HRESULT;
#[cfg(feature = "dx12")]
type ExecuteFn = unsafe extern "system" fn(*mut c_void, u32, *const *mut c_void);

/// Distinct function addresses to hook.
#[derive(Default, Debug)]
pub(crate) struct Targets {
    pub present: Vec<usize>,
    pub present1: Vec<usize>,
    pub resize: Vec<usize>,
    pub resize1: Vec<usize>,
    #[cfg_attr(not(feature = "dx12"), allow(dead_code))]
    pub execute_command_lists: usize,
    /// (vtable address, interface level 0..=4) of every probe interface
    pub vtables: Vec<(usize, u8)>,
}

impl Targets {
    fn is_empty(&self) -> bool {
        self.present.is_empty() && self.present1.is_empty()
    }

    fn add(list: &mut Vec<usize>, addr: usize) {
        if addr != 0 && !list.contains(&addr) {
            list.push(addr);
        }
    }

    /// Reads the swap-chain methods through every interface the object
    /// exposes: their vtables need not share the same entries.
    fn add_vtable(&mut self, obj: *mut c_void, level: u8) {
        let vt = unsafe { *(obj as *const usize) };
        match self.vtables.iter_mut().find(|v| v.0 == vt) {
            Some(v) => v.1 = v.1.max(level),
            None => self.vtables.push((vt, level)),
        }
    }

    fn add_swap_chain(&mut self, what: &str, sc: &IDXGISwapChain) {
        let mut seen = Vec::new();
        self.add_vtable(sc.as_raw(), 0);
        Self::add(&mut self.present, sc.vtable().Present as usize);
        Self::add(&mut self.resize, sc.vtable().ResizeBuffers as usize);
        seen.push(format!("SC0 present={} resize={}", describe(sc.vtable().Present as usize), describe(sc.vtable().ResizeBuffers as usize)));
        if let Ok(s) = sc.cast::<IDXGISwapChain1>() {
            self.add_vtable(s.as_raw(), 1);
            let v = s.vtable();
            Self::add(&mut self.present, v.base__.Present as usize);
            Self::add(&mut self.resize, v.base__.ResizeBuffers as usize);
            Self::add(&mut self.present1, v.Present1 as usize);
            seen.push(format!("SC1 present={} present1={}", describe(v.base__.Present as usize), describe(v.Present1 as usize)));
        }
        if let Ok(s) = sc.cast::<IDXGISwapChain2>() {
            self.add_vtable(s.as_raw(), 2);
            let v = s.vtable();
            Self::add(&mut self.present, v.base__.base__.Present as usize);
            Self::add(&mut self.resize, v.base__.base__.ResizeBuffers as usize);
            Self::add(&mut self.present1, v.base__.Present1 as usize);
            seen.push(format!("SC2 present={} present1={}", describe(v.base__.base__.Present as usize), describe(v.base__.Present1 as usize)));
        }
        if let Ok(s) = sc.cast::<IDXGISwapChain3>() {
            self.add_vtable(s.as_raw(), 3);
            let v = s.vtable();
            Self::add(&mut self.present, v.base__.base__.base__.Present as usize);
            Self::add(&mut self.resize, v.base__.base__.base__.ResizeBuffers as usize);
            Self::add(&mut self.present1, v.base__.base__.Present1 as usize);
            Self::add(&mut self.resize1, v.ResizeBuffers1 as usize);
            seen.push(format!("SC3 present={} resize1={}", describe(v.base__.base__.base__.Present as usize), describe(v.ResizeBuffers1 as usize)));
        }
        if let Ok(s) = sc.cast::<IDXGISwapChain4>() {
            self.add_vtable(s.as_raw(), 4);
            let v = s.vtable();
            Self::add(&mut self.present, v.base__.base__.base__.base__.Present as usize);
            Self::add(&mut self.present1, v.base__.base__.base__.Present1 as usize);
            Self::add(&mut self.resize1, v.base__.ResizeBuffers1 as usize);
            seen.push(format!("SC4 present={}", describe(v.base__.base__.base__.base__.Present as usize)));
        }
        log::debug!("overhook: probe {what}: {}", seen.join("; "));
    }
}

/// `module+0xRVA [first bytes]` for logs.
pub(crate) fn describe(addr: usize) -> String {
    use windows::Win32::System::LibraryLoader::{
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, GetModuleFileNameW,
        GetModuleHandleExW,
    };
    if addr == 0 {
        return "-".into();
    }
    let mut module = HMODULE::default();
    let name = unsafe {
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            PCWSTR(addr as *const u16),
            &mut module,
        )
    }
    .ok()
    .map(|_| {
        let mut buf = [0u16; 260];
        let n = unsafe { GetModuleFileNameW(Some(module), &mut buf) } as usize;
        let full = String::from_utf16_lossy(&buf[..n]);
        full.rsplit(['\\', '/']).next().unwrap_or("").to_owned()
    });
    let bytes = unsafe { std::slice::from_raw_parts(addr as *const u8, 6) };
    let hex: Vec<String> = bytes.iter().map(|b| format!("{b:02x}")).collect();
    match name {
        Some(n) => format!("{n}+{:#x}[{}]", addr - module.0 as usize, hex.join(" ")),
        None => format!("{addr:#x}[{}]", hex.join(" ")),
    }
}

// ---------------------------------------------------------------------------
// detours
// ---------------------------------------------------------------------------

/// Logs the first call of each detour: proves which route the game uses.
fn first_call(kind: usize, n: usize, this: *mut c_void, slot: usize) {
    static SEEN: [AtomicBool; 4 * SLOTS] = [const { AtomicBool::new(false) }; 4 * SLOTS];
    if SEEN[kind * SLOTS + n].swap(true, Ordering::Relaxed) {
        return;
    }
    let names = ["Present", "Present1", "ResizeBuffers", "ResizeBuffers1"];
    let entry = if this.is_null() { 0 } else { unsafe { *(*(this as *const *const usize)).add(slot) } };
    log::info!(
        "overhook: first {} call (variant {n}) on {this:?}; its vtable entry -> {}",
        names[kind],
        describe(entry)
    );
}

unsafe extern "system" fn present_detour<const N: usize>(this: *mut c_void, sync: u32, flags: DXGI_PRESENT) -> HRESULT {
    let _f = super::InFlight::new();
    first_call(0, N, this, 8);
    if flags.0 & DXGI_PRESENT_TEST.0 == 0 {
        crate::overlay::on_present(this);
    }
    let orig: PresentFn = unsafe { std::mem::transmute(PRESENT[N].load(Ordering::Acquire)) };
    unsafe { orig(this, sync, flags) }
}

unsafe extern "system" fn present1_detour<const N: usize>(
    this: *mut c_void,
    sync: u32,
    flags: DXGI_PRESENT,
    params: *const DXGI_PRESENT_PARAMETERS,
) -> HRESULT {
    let _f = super::InFlight::new();
    first_call(1, N, this, 22);
    if flags.0 & DXGI_PRESENT_TEST.0 == 0 {
        crate::overlay::on_present(this);
    }
    let orig: Present1Fn = unsafe { std::mem::transmute(PRESENT1[N].load(Ordering::Acquire)) };
    unsafe { orig(this, sync, flags, params) }
}

unsafe extern "system" fn resize_detour<const N: usize>(
    this: *mut c_void,
    count: u32,
    w: u32,
    h: u32,
    fmt: i32,
    flags: u32,
) -> HRESULT {
    let _f = super::InFlight::new();
    first_call(2, N, this, 13);
    crate::overlay::on_resize(this);
    let orig: ResizeFn = unsafe { std::mem::transmute(RESIZE[N].load(Ordering::Acquire)) };
    unsafe { orig(this, count, w, h, fmt, flags) }
}

#[allow(clippy::too_many_arguments)]
unsafe extern "system" fn resize1_detour<const N: usize>(
    this: *mut c_void,
    count: u32,
    w: u32,
    h: u32,
    fmt: i32,
    flags: u32,
    node_mask: *const u32,
    queues: *const *mut c_void,
) -> HRESULT {
    let _f = super::InFlight::new();
    first_call(3, N, this, 39);
    crate::overlay::on_resize(this);
    let orig: Resize1Fn = unsafe { std::mem::transmute(RESIZE1[N].load(Ordering::Acquire)) };
    unsafe { orig(this, count, w, h, fmt, flags, node_mask, queues) }
}

#[cfg(feature = "dx12")]
unsafe extern "system" fn execute_detour(this: *mut c_void, count: u32, lists: *const *mut c_void) {
    let _f = super::InFlight::new();
    static ONCE: AtomicBool = AtomicBool::new(false);
    if !ONCE.swap(true, Ordering::Relaxed) {
        log::info!("overhook: first ExecuteCommandLists call (the game uses D3D12)");
    }
    crate::renderer::dx12::observe_queue(this);
    let orig: ExecuteFn = unsafe { std::mem::transmute(EXECUTE.load(Ordering::Acquire)) };
    unsafe { orig(this, count, lists) }
}


// ---------------------------------------------------------------------------
// install
// ---------------------------------------------------------------------------

macro_rules! pick {
    ($detour:ident, $n:expr) => {
        match $n {
            0 => $detour::<0> as *const () as usize,
            1 => $detour::<1> as *const () as usize,
            2 => $detour::<2> as *const () as usize,
            3 => $detour::<3> as *const () as usize,
            4 => $detour::<4> as *const () as usize,
            5 => $detour::<5> as *const () as usize,
            6 => $detour::<6> as *const () as usize,
            _ => $detour::<7> as *const () as usize,
        }
    };
}

fn hook_all(addrs: &[usize], store: &'static [AtomicUsize; SLOTS], detours: &dyn Fn(usize) -> usize, name: &str) -> usize {
    let mut ok = 0;
    for (n, &addr) in addrs.iter().take(SLOTS).enumerate() {
        match unsafe { super::hook(addr, detours(n), name, &store[n]) } {
            Ok(_) => ok += 1,
            Err(e) => log::warn!("overhook: {e}"),
        }
    }
    if addrs.len() > SLOTS {
        log::warn!("overhook: {} {name} variants, only {SLOTS} hooked", addrs.len());
    }
    ok
}

pub(crate) fn install(targets: &Targets) -> Result<()> {
    let presents = hook_all(&targets.present, &PRESENT, &|n| pick!(present_detour, n), "IDXGISwapChain::Present")
        + hook_all(&targets.present1, &PRESENT1, &|n| pick!(present1_detour, n), "IDXGISwapChain1::Present1");
    hook_all(&targets.resize, &RESIZE, &|n| pick!(resize_detour, n), "IDXGISwapChain::ResizeBuffers");
    hook_all(&targets.resize1, &RESIZE1, &|n| pick!(resize1_detour, n), "IDXGISwapChain3::ResizeBuffers1");
    #[cfg(feature = "dx12")]
    if targets.execute_command_lists != 0 {
        match unsafe {
            super::hook(
                targets.execute_command_lists,
                execute_detour as *const () as usize,
                "ID3D12CommandQueue::ExecuteCommandLists",
                &EXECUTE,
            )
        } {
            Ok(_) => {}
            Err(e) => log::warn!("overhook: {e} - the DX12 renderer will not be available"),
        }
    }
    if presents == 0 {
        return Err(Error::NoGraphicsApi);
    }
    patch_vtables(targets);
    Ok(())
}

/// vtable slots we redirected: (slot address, original entry, our detour).
static VT_PATCHES: Mutex<Vec<(usize, usize, usize)>> = Mutex::new(Vec::new());

unsafe fn write_slot(slot: usize, value: usize) -> bool {
    use windows::Win32::System::Memory::{PAGE_PROTECTION_FLAGS, PAGE_READWRITE, VirtualProtect};
    let mut old = PAGE_PROTECTION_FLAGS(0);
    if unsafe { VirtualProtect(slot as *const c_void, 8, PAGE_READWRITE, &mut old) }.is_err() {
        return false;
    }
    unsafe { AtomicUsize::from_ptr(slot as *mut usize) }.store(value, Ordering::SeqCst);
    let mut tmp = PAGE_PROTECTION_FLAGS(0);
    let _ = unsafe { VirtualProtect(slot as *const c_void, 8, old, &mut tmp) };
    true
}

/// Also redirects the swap-chain vtables (they live in dxgi.dll's read-only
/// data and are shared by every swap chain of that class, the game's
/// included). Calls through the vtable then reach our detour even if another
/// tool overwrites the inline hook. The detours call the MinHook trampoline,
/// so a call is never handled twice.
fn patch_vtables(t: &Targets) {
    let mut patches = VT_PATCHES.lock().unwrap_or_else(|e| e.into_inner());
    for &(vt, level) in &t.vtables {
        if !describe(vt).contains(".dll+") {
            continue; // only static vtables inside a module image
        }
        let kinds: [(usize, u8, &Vec<usize>, &[AtomicUsize; SLOTS], fn(usize) -> usize); 4] = [
            (8, 0, &t.present, &PRESENT, |n| pick!(present_detour, n)),
            (13, 0, &t.resize, &RESIZE, |n| pick!(resize_detour, n)),
            (22, 1, &t.present1, &PRESENT1, |n| pick!(present1_detour, n)),
            (39, 3, &t.resize1, &RESIZE1, |n| pick!(resize1_detour, n)),
        ];
        for (slot_index, min_level, list, store, detour) in kinds {
            if level < min_level {
                continue;
            }
            let slot = vt + slot_index * 8;
            let cur = unsafe { *(slot as *const usize) };
            let Some(n) = list.iter().position(|&a| a == cur) else { continue };
            if n >= SLOTS || store[n].load(Ordering::Acquire) == 0 {
                continue; // inline hook missing: no trampoline to call
            }
            let d = detour(n);
            if unsafe { write_slot(slot, d) } {
                patches.push((slot, cur, d));
            }
        }
    }
    log::info!("overhook: {} vtable entries redirected in {} vtables", patches.len(), t.vtables.len());
}

pub(crate) fn restore_vtables() {
    for (slot, orig, _) in std::mem::take(&mut *VT_PATCHES.lock().unwrap_or_else(|e| e.into_inner())) {
        unsafe { write_slot(slot, orig) };
    }
}

/// Logs (once per entry) vtable redirects that something else undid.
pub(crate) fn check_vtables() {
    static REPORTED: Mutex<Vec<usize>> = Mutex::new(Vec::new());
    let patches = VT_PATCHES.lock().unwrap_or_else(|e| e.into_inner());
    let mut reported = REPORTED.lock().unwrap_or_else(|e| e.into_inner());
    for &(slot, _, ours) in patches.iter() {
        let now = unsafe { *(slot as *const usize) };
        if now != ours && !reported.contains(&slot) {
            reported.push(slot);
            log::warn!("overhook: vtable entry {} was changed by something else -> {}", describe(slot), describe(now));
        }
    }
}

// ---------------------------------------------------------------------------
// address discovery
// ---------------------------------------------------------------------------

struct DummyWindow {
    hwnd: HWND,
    class: PCWSTR,
    instance: HMODULE,
}

unsafe extern "system" fn dummy_wndproc(h: HWND, m: u32, w: WPARAM, l: LPARAM) -> LRESULT {
    unsafe { DefWindowProcW(h, m, w, l) }
}

impl DummyWindow {
    fn new() -> Result<Self> {
        unsafe {
            let instance: HMODULE = windows::Win32::System::LibraryLoader::GetModuleHandleW(None)?;
            let class = w!("overhook_dummy_window");
            let wc = WNDCLASSEXW {
                cbSize: size_of::<WNDCLASSEXW>() as u32,
                lpfnWndProc: Some(dummy_wndproc),
                hInstance: instance.into(),
                lpszClassName: class,
                ..Default::default()
            };
            // may already be registered (re-injection): ignore the error
            RegisterClassExW(&wc);
            let hwnd = CreateWindowExW(
                WINDOW_EX_STYLE::default(),
                class,
                w!("overhook"),
                WS_OVERLAPPEDWINDOW,
                0,
                0,
                100,
                100,
                None,
                None,
                Some(instance.into()),
                None,
            )?;
            Ok(DummyWindow { hwnd, class, instance })
        }
    }
}

impl Drop for DummyWindow {
    fn drop(&mut self) {
        unsafe {
            let _ = DestroyWindow(self.hwnd);
            let _ = UnregisterClassW(self.class, Some(self.instance.into()));
        }
    }
}


/// Creates the throw-away objects and returns the function addresses.
pub(crate) fn discover(dx11: bool, dx12: bool) -> Result<Targets> {
    let window = DummyWindow::new()?;
    let mut targets = Targets::default();

    #[cfg(feature = "dx11")]
    if dx11 {
        use windows::Win32::Graphics::Dxgi::Common::*;
        for (what, effect) in [("dx11 hwnd flip", DXGI_SWAP_EFFECT_FLIP_DISCARD), ("dx11 hwnd blt", DXGI_SWAP_EFFECT_DISCARD)] {
            match crate::renderer::dx11::dummy_swap_chain(window.hwnd, effect) {
                Ok(sc) => targets.add_swap_chain(what, &sc),
                Err(e) => log::warn!("overhook: {what} probe failed: {e}"),
            }
        }
        // what UWP games use: flip-sequential swap chains made through
        // IDXGIFactory2 (CoreWindow / composition)
        match crate::renderer::dx11::dummy_factory2_swap_chains(window.hwnd) {
            Ok(list) => {
                for (what, sc) in list {
                    targets.add_swap_chain(what, &sc);
                }
            }
            Err(e) => log::warn!("overhook: dx11 factory2 probe failed: {e}"),
        }
        let _ = DXGI_FORMAT_UNKNOWN;
    }
    #[cfg(feature = "dx12")]
    if dx12 {
        match crate::renderer::dx12::dummy_objects(window.hwnd) {
            Ok((sc, queue)) => {
                targets.add_swap_chain("dx12 hwnd flip", &sc);
                targets.execute_command_lists = queue.vtable().ExecuteCommandLists as usize;
            }
            Err(e) => log::warn!("overhook: DX12 probe failed: {e}"),
        }
    }
    let _ = (dx11, dx12);
    drop(window);
    let list = |v: &[usize]| v.iter().map(|&a| describe(a)).collect::<Vec<_>>().join(", ");
    log::info!(
        "overhook: hook targets: Present [{}]; Present1 [{}]; ResizeBuffers [{}]; ResizeBuffers1 [{}]",
        list(&targets.present),
        list(&targets.present1),
        list(&targets.resize),
        list(&targets.resize1)
    );
    if targets.is_empty() { Err(Error::NoGraphicsApi) } else { Ok(targets) }
}
