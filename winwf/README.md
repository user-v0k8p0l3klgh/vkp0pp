# winwf.dll — Auto Potion Navigation with an on/off menu

Drop-in replacement for the original `winwf.dll` (Minecraft Bedrock 1.16.100).
Inject it the same way as before.

- **Alt+G** (the G key on the English layout) opens and closes the menu.
- The menu shows **Feature: ON / OFF** and a button to switch it.
- Closing the menu (Alt+G or ×) never changes the feature state. The feature
  starts ON, like the original.
- The menu is a small window drawn over the game, near the top-left corner.
  While it is open the game gets no mouse or keyboard input.

## Troubleshooting

Everything is logged to `winwf.log` in the game's temp folder. For Minecraft
that is
`%LOCALAPPDATA%\Packages\Microsoft.MinecraftUWP_8wekyb3d8bbwe\AC\Temp\winwf.log`.
Healthy lines: `feature: ready`, `menu overlay installed`,
`vtable entries redirected`, `Alt+G: listening on the game window (UWP)`,
`first Present call`, and after Alt+G `menu shown`, `Dx11 renderer ready`
(or Dx12) and `menu check: OK`. When the menu does not show, the
`menu check` line says whether the game's frames reach the overlay at all,
and `was overwritten` / `changed by something else` lines name hooks that
other software replaced. The `modules loaded` line lists overlays and other
tools running inside the game.

## How it works

`payload/pottedhelp.dll` is the original DLL, byte for byte. At startup the new
DLL writes it to the game's temp folder, loads it, and waits for it to verify
the game version and install its four game hooks. Three of those hooks drive
the feature (screen open, input, tick). Their vtable slots are re-pointed to
small gates that jump either to the original module (ON) or straight to the
game's own function (OFF). The fourth hook (screen teardown) only resets the
module's state and stays active. The menu is drawn with
[Overhook](https://github.com/qcountel/overhook) (egui), vendored in
`vendor/overhook` at 722c43f with these changes:

- The original Present is stored before the hook is enabled (it was stored
  after, so a frame drawn in between jumped to address 0 and crashed the game).
- Besides the inline hook on `IDXGISwapChain::Present` / `Present1` /
  `ResizeBuffers[1]`, the matching entries of the swap-chain vtables in
  `dxgi.dll` are redirected too. If other software overwrites the inline hook,
  the game's calls through the vtable still reach the overlay (detours call
  the MinHook trampoline, so nothing runs twice).
- The swap chains are probed in more variants (blt, flip, factory2, composition,
  every IDXGISwapChainN interface) and every distinct entry is hooked.
- Renderer creation is retried after a failure, DX11 shaders use SM 4.0, and
  each step is logged (first Present, renderer, first overlay frame), plus a
  watchdog for overwritten hooks.

Minecraft is a UWP app, so Alt+G is caught on the game window's own UI thread
(`CoreDispatcher.AcceleratorKeyActivated`, see `src/hotkey.rs`); a
`GetAsyncKeyState` poller covers normal Win32 hosts.

## Build (Linux)

```sh
apt install mingw-w64
rustup target add x86_64-pc-windows-gnu
cargo build --release          # target/x86_64-pc-windows-gnu/release/winwf.dll
```

## Tests (Wine)

`tests/fakemc.h` makes a test .exe look like Minecraft 1.16.100 to the
original module, so the real hook code runs.

```sh
cargo build --release --features harness --target-dir /tmp/wt
cp /tmp/wt/x86_64-pc-windows-gnu/release/winwf.dll /tmp/run/winwf_test.dll
x86_64-w64-mingw32-gcc -O1 -o /tmp/run/harness.exe tests/harness.c -lpsapi
x86_64-w64-mingw32-gcc -O1 -o /tmp/run/d3dhost.exe tests/d3dhost.c -ld3d11 -ldxgi -luser32 -lgdi32
cd /tmp/run && wine harness.exe        # hook routing ON / OFF / ON
DISPLAY=:99 wine d3dhost.exe           # menu, Alt+G, button click (screenshot with markers)
DISPLAY=:99 wine d3dhost.exe clobber   # same, after another "tool" overwrites the Present hook
DISPLAY=:99 wine d3dhost.exe mm winwf_test.dll clobber   # injected by manual mapping
```

Never ship a `--features harness` build: it exports test controls.
