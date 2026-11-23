# winwf.dll — Auto Potion Navigation with an on/off menu

Drop-in replacement for the original `winwf.dll` (Minecraft Bedrock 1.16.100).
Inject it the same way as before.

- **Alt+G** (the G key on the English layout) opens and closes the menu.
- The menu shows **Feature: ON / OFF** and a button to switch it.
- Closing the menu (Alt+G or ×) never changes the feature state. The feature
  starts ON, like the original.

## How it works

`payload/pottedhelp.dll` is the original DLL, byte for byte. At startup the new
DLL writes it to the game's temp folder, loads it, and waits for it to verify
the game version and install its four game hooks. Three of those hooks drive
the feature (screen open, input, tick). Their vtable slots are re-pointed to
small gates that jump either to the original module (ON) or straight to the
game's own function (OFF). The fourth hook (screen teardown) only resets the
module's state and stays active. The menu is drawn with
[Overhook](https://github.com/qcountel/overhook) (egui).

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
```

Never ship a `--features harness` build: it exports test controls.
