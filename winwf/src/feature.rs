//! Loads the original auto potion navigation module and routes its game hooks
//! through an on/off gate, so the feature can be switched at runtime.
//!
//! The original DLL (`payload/pottedhelp.dll`, byte-for-byte the shipped
//! `winwf.dll`) installs four vtable hooks in Minecraft 1.16.100. Three of them
//! drive the feature (screen open, input, tick); while the feature is off they
//! go straight to the game's own functions. The fourth (screen teardown) only
//! clears the module's state and always stays on.

use std::path::PathBuf;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicUsize, Ordering};
use std::time::{Duration, Instant};

use windows::Win32::Foundation::HMODULE;
use windows::Win32::System::LibraryLoader::{GetModuleHandleW, GetProcAddress, LoadLibraryW};
use windows::Win32::System::Memory::{PAGE_PROTECTION_FLAGS, PAGE_READWRITE, VirtualProtect};
use windows::core::{HSTRING, PCWSTR, s};

static PAYLOAD: &[u8] = include_bytes!("../payload/pottedhelp.dll");

/// Mirrors the original module's export: 0 starting, 1 active, negative on failure.
#[unsafe(no_mangle)]
#[allow(non_upper_case_globals)]
pub static AutoPotionNavigationStatus: AtomicI32 = AtomicI32::new(0);

pub static ENABLED: AtomicBool = AtomicBool::new(true);

pub(crate) static DLL_OPEN: AtomicUsize = AtomicUsize::new(0);
static GAME_OPEN: AtomicUsize = AtomicUsize::new(0);
static DLL_INPUT: AtomicUsize = AtomicUsize::new(0);
static GAME_INPUT: AtomicUsize = AtomicUsize::new(0);
static DLL_TICK: AtomicUsize = AtomicUsize::new(0);
static GAME_TICK: AtomicUsize = AtomicUsize::new(0);

// Pure tail jumps: registers and stack arguments reach the target untouched,
// whatever the hooked method's signature is.
macro_rules! gate {
    ($name:literal, $dll:ident, $game:ident) => {
        std::arch::global_asm!(
            concat!(".globl ", $name),
            concat!($name, ":"),
            "cmp byte ptr [rip + {en}], 0",
            "je 2f",
            "jmp qword ptr [rip + {dll}]",
            "2:",
            "jmp qword ptr [rip + {game}]",
            en = sym ENABLED,
            dll = sym $dll,
            game = sym $game,
        );
    };
}
gate!("winwf_gate_open", DLL_OPEN, GAME_OPEN);
gate!("winwf_gate_input", DLL_INPUT, GAME_INPUT);
gate!("winwf_gate_tick", DLL_TICK, GAME_TICK);

unsafe extern "C" {
    fn winwf_gate_open();
    fn winwf_gate_input();
    fn winwf_gate_tick();
}

struct Hook {
    /// vtable slot in Minecraft.Windows.exe (RVA)
    slot: usize,
    /// game method the module saved as "original" (RVA)
    game: usize,
    /// module's detour (RVA inside the payload)
    detour: usize,
    dll_cell: &'static AtomicUsize,
    game_cell: &'static AtomicUsize,
    gate: unsafe extern "C" fn(),
}

// Taken from the module's own installer (Minecraft 1.16.100, verified by it
// against the PE timestamp, image size and code signatures before hooking).
const HOOKS: [Hook; 3] = [
    Hook { slot: 0x330_1908, game: 0xC5_0D70, detour: 0x2C70, dll_cell: &DLL_OPEN, game_cell: &GAME_OPEN, gate: winwf_gate_open },
    Hook { slot: 0x330_5FF8, game: 0xC1_4120, detour: 0x2DB0, dll_cell: &DLL_INPUT, game_cell: &GAME_INPUT, gate: winwf_gate_input },
    Hook { slot: 0x330_18E8, game: 0xC5_2A70, detour: 0x2F30, dll_cell: &DLL_TICK, game_cell: &GAME_TICK, gate: winwf_gate_tick },
];

#[derive(Clone, PartialEq)]
pub enum Phase {
    Starting,
    Ready,
    Failed(String),
}

pub static PHASE: Mutex<Phase> = Mutex::new(Phase::Starting);

fn set_phase(p: Phase) {
    *PHASE.lock().unwrap_or_else(|e| e.into_inner()) = p;
}

pub fn phase() -> Phase {
    PHASE.lock().unwrap_or_else(|e| e.into_inner()).clone()
}

/// Test-only controls for the Wine tests (`--features harness`); not in release builds.
#[cfg(feature = "harness")]
mod harness {
    use super::*;

    #[unsafe(no_mangle)]
    pub extern "C" fn winwf_set_enabled(on: bool) {
        ENABLED.store(on, Ordering::Release);
    }

    /// 0 starting, 1 ready, 2 failed
    #[unsafe(no_mangle)]
    pub extern "C" fn winwf_phase() -> i32 {
        match phase() {
            Phase::Starting => 0,
            Phase::Ready => 1,
            Phase::Failed(e) => {
                eprintln!("winwf: {e}");
                2
            }
        }
    }
}

pub fn start() {
    match run() {
        Ok(()) => set_phase(Phase::Ready),
        Err(e) => set_phase(Phase::Failed(e)),
    }
}

fn run() -> Result<(), String> {
    let module = load_payload()?;
    let status = unsafe { GetProcAddress(module, s!("AutoPotionNavigationStatus")) }
        .ok_or("module has no AutoPotionNavigationStatus export")? as *const AtomicI32;
    let status = unsafe { &*status };

    let deadline = Instant::now() + Duration::from_secs(30);
    let code = loop {
        let v = status.load(Ordering::Acquire);
        if v != 0 || Instant::now() > deadline {
            break v;
        }
        std::thread::sleep(Duration::from_millis(20));
    };
    AutoPotionNavigationStatus.store(code, Ordering::Release);
    match code {
        1 => {}
        -1 => return Err("unsupported game version (needs Minecraft Bedrock 1.16.100)".into()),
        0 => return Err("feature module did not start".into()),
        c => return Err(format!("feature module could not hook the game (code {c})")),
    }
    install_gates(module.0 as usize)
}

fn load_payload() -> Result<HMODULE, String> {
    // FNV-1a: a stable file name per payload build
    let hash = PAYLOAD.iter().fold(0x811c_9dc5u32, |h, b| (h ^ *b as u32).wrapping_mul(0x0100_0193));
    let path: PathBuf = std::env::temp_dir().join(format!("winwf-core-{hash:08x}.dll"));
    let current = std::fs::read(&path).map(|b| b == PAYLOAD).unwrap_or(false);
    if !current {
        std::fs::write(&path, PAYLOAD).map_err(|e| format!("cannot write {}: {e}", path.display()))?;
    }
    unsafe { LoadLibraryW(&HSTRING::from(path.as_os_str())) }
        .map_err(|e| format!("cannot load {}: {e}", path.display()))
}

unsafe fn swap_slot(slot: *mut usize, from: usize, to: usize) -> bool {
    let mut old = PAGE_PROTECTION_FLAGS(0);
    if unsafe { VirtualProtect(slot.cast(), 8, PAGE_READWRITE, &mut old) }.is_err() {
        return false;
    }
    let ok = unsafe { AtomicUsize::from_ptr(slot) }
        .compare_exchange(from, to, Ordering::SeqCst, Ordering::SeqCst)
        .is_ok();
    let mut tmp = PAGE_PROTECTION_FLAGS(0);
    let _ = unsafe { VirtualProtect(slot.cast(), 8, old, &mut tmp) };
    ok
}

fn install_gates(payload: usize) -> Result<(), String> {
    let game = unsafe { GetModuleHandleW(PCWSTR::null()) }.map_err(|e| e.to_string())?.0 as usize;
    for (i, h) in HOOKS.iter().enumerate() {
        let detour = payload + h.detour;
        h.dll_cell.store(detour, Ordering::Release);
        h.game_cell.store(game + h.game, Ordering::Release);
        let slot = (game + h.slot) as *mut usize;
        if !unsafe { swap_slot(slot, detour, h.gate as usize) } {
            for r in &HOOKS[..i] {
                unsafe { swap_slot((game + r.slot) as *mut usize, r.gate as usize, payload + r.detour) };
            }
            return Err("game hooks are not in the expected state; the feature stays always on".into());
        }
    }
    Ok(())
}
