//! Alt+G detection.
//!
//! Minecraft Bedrock is a UWP app: its keyboard goes to a `CoreWindow`, so
//! `GetAsyncKeyState` sees nothing, and overhook's `CoreWindow` polling only
//! works when the game presents on the window's UI thread. To not depend on
//! that, a handler is registered on the window's `CoreDispatcher` (from the UI
//! thread, via `RunAsync`) and overhook's key polling is pumped on that thread
//! too. Alt+<key> arrives there as `SystemKeyDown`. Win32 games keep working
//! through the `GetAsyncKeyState` poller in `lib.rs`.

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::time::{Duration, Instant};

use windows::ApplicationModel::Core::CoreApplication;
use windows::Foundation::TypedEventHandler;
use windows::System::VirtualKey;
use windows::UI::Core::{AcceleratorKeyEventArgs, CoreAcceleratorKeyEventType, CoreDispatcher, CoreDispatcherPriority, DispatchedHandler};

/// Scan code of the key labelled G on a US/English keyboard.
const SCAN_G: u32 = 0x22;

static START: std::sync::OnceLock<Instant> = std::sync::OnceLock::new();
static LAST_TOGGLE_MS: AtomicU64 = AtomicU64::new(0);
pub static UWP_ACTIVE: AtomicBool = AtomicBool::new(false);

fn now_ms() -> u64 {
    START.get_or_init(Instant::now).elapsed().as_millis() as u64 + 1_000
}

/// Shows / hides the menu. Both detectors may see the same press, so presses
/// closer than 250 ms count once.
pub fn toggle_menu(source: &str) {
    let now = now_ms();
    let last = LAST_TOGGLE_MS.load(Ordering::Acquire);
    if now.saturating_sub(last) < 250
        || LAST_TOGGLE_MS.compare_exchange(last, now, Ordering::AcqRel, Ordering::Acquire).is_err()
    {
        return;
    }
    overhook::toggle();
    let shown = overhook::is_visible();
    log::info!(
        "Alt+G ({source}): menu {} (game frames seen: {}, overlay frames drawn: {}, api: {:?})",
        if shown { "shown" } else { "hidden" },
        overhook::present_count(),
        overhook::drawn_frames(),
        overhook::active_api()
    );
    if shown {
        // one follow-up line answers "why is nothing on screen"
        std::thread::spawn(|| {
            let (p0, d0) = (overhook::present_count(), overhook::drawn_frames());
            std::thread::sleep(Duration::from_secs(2));
            let (p, d) = (overhook::present_count() - p0, overhook::drawn_frames() - d0);
            if !overhook::is_visible() {
                return;
            }
            if p == 0 {
                log::error!("menu check: the game presented no frames through the hook in 2 s (Present hook not firing)");
            } else if d == 0 {
                log::error!("menu check: {p} game frames in 2 s but no overlay frame drawn (renderer problem, see lines above)");
            } else {
                log::info!("menu check: OK, {d} overlay frames drawn in 2 s ({p} game frames)");
            }
        });
    }
}

fn on_key(args: &AcceleratorKeyEventArgs) -> windows::core::Result<()> {
    let kind = args.EventType()?;
    if kind != CoreAcceleratorKeyEventType::SystemKeyDown && kind != CoreAcceleratorKeyEventType::KeyDown {
        return Ok(());
    }
    let status = args.KeyStatus()?;
    let is_g = args.VirtualKey()? == VirtualKey::G || (status.ScanCode == SCAN_G && !status.IsExtendedKey);
    if is_g && status.IsMenuKeyDown && !status.WasKeyDown {
        toggle_menu("UWP");
        // keep the G press away from the game
        let _ = args.SetHandled(true);
    }
    Ok(())
}

fn attach() -> windows::core::Result<CoreDispatcher> {
    let dispatcher = CoreApplication::MainView()?.Dispatcher()?;
    let d = dispatcher.clone();
    let register = DispatchedHandler::new(move || {
        let handler = TypedEventHandler::<CoreDispatcher, AcceleratorKeyEventArgs>::new(|_, args| match args.as_ref() {
            Some(a) => on_key(a),
            None => Ok(()),
        });
        match d.AcceleratorKeyActivated(&handler) {
            Ok(_) => {
                UWP_ACTIVE.store(true, Ordering::Release);
                log::info!("Alt+G: listening on the game window (UWP)");
            }
            Err(e) => log::warn!("Alt+G: cannot listen on the game window: {e}"),
        }
        Ok(())
    });
    dispatcher.RunAsync(CoreDispatcherPriority::Normal, &register)?;
    Ok(dispatcher)
}

/// Runs forever on its own thread. Returns quietly for non-UWP processes.
pub fn run_uwp() {
    let mut dispatcher = None;
    for attempt in 0..120 {
        match attach() {
            Ok(d) => {
                dispatcher = Some(d);
                break;
            }
            Err(e) => {
                if attempt == 0 {
                    log::info!("Alt+G: no UWP window yet ({e}); retrying");
                }
                std::thread::sleep(Duration::from_millis(500));
            }
        }
    }
    let Some(dispatcher) = dispatcher else {
        log::info!("Alt+G: not a UWP app, using key polling only");
        return;
    };

    // overhook reads UWP keys only on the UI thread; feed it from there
    let poll = DispatchedHandler::new(|| {
        overhook::input::poll_core_keys();
        Ok(())
    });
    loop {
        let visible = overhook::is_visible();
        let _ = dispatcher.RunAsync(CoreDispatcherPriority::Low, &poll);
        std::thread::sleep(Duration::from_millis(if visible { 8 } else { 30 }));
    }
}
