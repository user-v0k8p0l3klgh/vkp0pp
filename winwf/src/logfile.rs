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
