//! Appends overhook's and winwf's log lines to `%TEMP%\winwf.log`.
//! For Minecraft (UWP) that is
//! `%LOCALAPPDATA%\Packages\Microsoft.MinecraftUWP_8wekyb3d8bbwe\AC\Temp\winwf.log`.

use std::fs::{File, OpenOptions};
use std::io::Write;
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};

struct FileLog(Mutex<File>);

impl log::Log for FileLog {
    fn enabled(&self, m: &log::Metadata) -> bool {
        m.level() <= log::Level::Debug
    }

    fn log(&self, r: &log::Record) {
        if !self.enabled(r.metadata()) {
            return;
        }
        let t = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default();
        let (s, ms) = (t.as_secs() % 86_400, t.subsec_millis());
        let mut f = self.0.lock().unwrap_or_else(|e| e.into_inner());
        let _ = writeln!(
            f,
            "{:02}:{:02}:{:02}.{ms:03} UTC [{}] {}",
            s / 3600,
            s / 60 % 60,
            s % 60,
            r.level(),
            r.args()
        );
        let _ = f.flush();
    }

    fn flush(&self) {}
}

pub fn init() {
    let path = std::env::temp_dir().join("winwf.log");
    let Ok(file) = OpenOptions::new().create(true).write(true).truncate(true).open(&path) else {
        return;
    };
    if log::set_boxed_logger(Box::new(FileLog(Mutex::new(file)))).is_ok() {
        log::set_max_level(log::LevelFilter::Debug);
        log::info!("winwf {} loaded, log: {}", env!("CARGO_PKG_VERSION"), path.display());
    }
}

/// Lists the DLLs loaded in the game: overlays and other hooking tools show up here.
pub fn log_modules() {
    use windows::Win32::Foundation::HMODULE;
    use windows::Win32::System::ProcessStatus::{K32EnumProcessModules, K32GetModuleBaseNameW};
    use windows::Win32::System::Threading::GetCurrentProcess;
    let mut mods = [HMODULE::default(); 512];
    let mut need = 0u32;
    let process = unsafe { GetCurrentProcess() };
    if !unsafe { K32EnumProcessModules(process, mods.as_mut_ptr(), size_of_val(&mods) as u32, &mut need) }.as_bool() {
        return;
    }
    let count = (need as usize / size_of::<HMODULE>()).min(mods.len());
    let names: Vec<String> = mods[..count]
        .iter()
        .map(|&m| {
            let mut buf = [0u16; 260];
            let n = unsafe { K32GetModuleBaseNameW(process, Some(m), &mut buf) } as usize;
            String::from_utf16_lossy(&buf[..n])
        })
        .collect();
    log::info!("{} modules loaded: {}", names.len(), names.join(", "));
}
