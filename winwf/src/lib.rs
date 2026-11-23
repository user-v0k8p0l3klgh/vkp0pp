//! winwf.dll: auto potion navigation for Minecraft Bedrock 1.16.100 with an
//! in-game on/off menu (Overhook + egui). Alt+G opens and closes the menu.

mod feature;
mod hotkey;
mod logfile;

use std::sync::atomic::Ordering;
use std::time::Duration;

use feature::{ENABLED, Phase};
use overhook::egui::{self, Color32, RichText};
use overhook::{InputBlocking, Overlay};

const VK_G: u16 = 0x47;
const VK_MENU: u16 = 0x12;
const VK_LMENU: u16 = 0xA4;
const VK_RMENU: u16 = 0xA5;

const GREEN: Color32 = Color32::from_rgb(80, 220, 100);
const RED: Color32 = Color32::from_rgb(235, 80, 80);

struct Menu {
    open: bool,
}

impl overhook::backends::egui::EguiApp for Menu {
    fn setup(&mut self, ctx: &egui::Context) {
        ctx.set_visuals(egui::Visuals::dark());
    }

    fn ui(&mut self, ctx: &egui::Context) {
        egui::Window::new("Auto Potion Navigation")
            .open(&mut self.open)
            .collapsible(false)
            .resizable(false)
            .default_pos([40.0, 40.0])
            .show(ctx, |ui| {
                let phase = feature::phase();
                let on = ENABLED.load(Ordering::Acquire);
                // the module may be running even if the gate could not be installed
                let running = feature::AutoPotionNavigationStatus.load(Ordering::Acquire) == 1;
                let (state, color) = match &phase {
                    Phase::Ready if on => ("ON", GREEN),
                    Phase::Ready => ("OFF", RED),
                    Phase::Starting => ("STARTING", Color32::GRAY),
                    Phase::Failed(_) if running => ("ON (cannot be turned off)", GREEN),
                    Phase::Failed(_) => ("UNAVAILABLE", RED),
                };

                ui.horizontal(|ui| {
                    ui.label(RichText::new("Feature:").size(18.0));
                    ui.label(RichText::new(state).size(22.0).strong().color(color));
                });
                ui.add_space(6.0);

                let label = if on { "Turn OFF" } else { "Turn ON" };
                let button = egui::Button::new(RichText::new(label).size(16.0)).min_size(egui::vec2(220.0, 34.0));
                if ui.add_enabled(phase == Phase::Ready, button).clicked() {
                    ENABLED.store(!on, Ordering::Release);
                }

                ui.add_space(6.0);
                match &phase {
                    Phase::Starting => {
                        ui.label("Starting...");
                    }
                    Phase::Ready => {}
                    Phase::Failed(e) => {
                        ui.colored_label(RED, format!("Not available: {e}"));
                    }
                }
                ui.separator();
                ui.weak("Alt+G: show / hide this menu");
            });
        // closing the window only hides the menu; the feature keeps its state
        if !self.open {
            self.open = true;
            overhook::set_visible(false);
        }
    }
}

fn alt_down() -> bool {
    use overhook::input::is_key_down;
    is_key_down(VK_MENU) || is_key_down(VK_LMENU) || is_key_down(VK_RMENU)
}

fn main_thread(_module: overhook::util::Module) {
    logfile::init();
    std::thread::spawn(feature::start);

    if let Err(e) = Overlay::builder()
        .visible(false)
        .input_blocking(InputBlocking::WhenVisible)
        .egui(Menu { open: true })
        .install()
    {
        log::error!("menu overlay could not be installed: {e}");
        return;
    }
    log::info!("menu overlay installed; press Alt+G in game");
    logfile::log_modules();
    std::thread::spawn(hotkey::run_uwp);

    // Win32 path (and backup for UWP once overhook's key table is fed).
    // Edge-triggered so holding the keys toggles once.
    let mut held = false;
    let mut ticks: u32 = 0;
    loop {
        std::thread::sleep(Duration::from_millis(10));
        ticks = ticks.wrapping_add(1);
        if ticks % 300 == 0 {
            overhook::check_hooks(); // every ~3 s
        }
        let combo = overhook::input::is_key_down(VK_G) && alt_down();
        if combo && !held {
            hotkey::toggle_menu("keys");
        }
        held = combo;
    }
}

overhook::entry!(main_thread);
