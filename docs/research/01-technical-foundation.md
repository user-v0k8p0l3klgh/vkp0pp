# Stage 1 — Technical Foundation Research (Bedrock 1.16.100)

Status: **draft for discussion** · Researched: 2026-11-23

Confidence legend used throughout:

- **[V]** Verified — primary source or direct check, linked.
- **[B]** Believed — strong inference or a single secondary source; needs confirmation.
- **[?]** Unknown — must be answered by the feasibility spike (section 9) before we design around it.

---

## 1. The target build

| Fact | Value | Conf. |
|---|---|---|
| Release | 17 Nov 2020 (Switch: 16 Nov) | [V] [wiki][wiki-1.16.100] |
| Protocol version | 419 | [V] [wiki][wiki-1.16.100] |
| Windows internal version | 1.16.10004.0 (build 4919810) | [V] [wiki][wiki-1.16.100] |
| Windows packaging | UWP (the move to GDK packaging happened in 1.21.120) | [V] [native-modding wiki][bnm-uwp] |
| Renderer | **Last Windows release before RenderDragon.** RenderDragon reached Windows 10 in 1.16.200 | [V] [1.16.200 changelog][cl-1.16.200] |
| Graphics API of the pre-RenderDragon Windows renderer | Probably Direct3D 11 | [B] — confirm by inspecting the binary's imports |
| Official dedicated server (BDS) 1.16.100.04 | Still downloadable from minecraft.net; **the Windows zip includes `bedrock_server.pdb` (226 MB)**; the Linux binary has no static symbol table but exports ~924k dynamic symbols | [V] downloaded and inspected 2026-11-23 |

Why the community prefers this build (community claims, **not** verified):
hit-registration and aim "feel", and third-party shaders that stopped working when RenderDragon arrived
([Hive forum thread][hive-116]).

## 2. Ecosystem state in late 2026: the biggest risk

1. **Major networks dropped 1.16 years ago.** CubeCraft removed all 1.16 versions on 30 Nov 2021
   ([notice][cubecraft-116]). Hive declined to restore 1.16.100, saying fewer than 1% of its players used it
   ([thread][hive-116]). [V]
2. **Networking change.** Since 26.60 (27 Oct 2026) the current client connects only over NetherNet, and RakNet
   is deprecated ([Minecraft Help][mc-help-nethernet], [GeyserMC][geyser-nethernet]). [V]
   1.16.100 predates NetherNet and speaks RakNet only [B]. A server must therefore *keep a RakNet listener*
   **and** translate protocol 419 for a 1.16.100 client to connect at all.
3. **Multiversion server software that targets 419 exists, but it is niche:** `helix-bedrock/multiversion`
   (in development), archived PocketMine multiversion plugins, and Nukkit forks ([search evidence][helix-mv]). [V]
   Whether any *active PvP server* accepts 1.16.100 today is **[?]**. A web search on 2026-11-23 found no public
   server advertising 1.16.100 support; server lists show current versions only (e.g. 26.63). That does not prove
   none exist, because 1.16.100 communities often advertise only on Discord.
4. **Xbox Live sign-in from a 2020 client in 2026** is **[?]**. Servers with `online-mode` require an
   Xbox-signed login chain.

**Implication:** a client's value depends on having somewhere to use it. This must be settled before
anything else (see the open question at the end).

## 3. Platform options

| Platform | How a client would work | Verdict |
|---|---|---|
| **Windows 10/11 x64, UWP 1.16.100** | Install side-by-side with [MCLauncher][mclauncher] (needs a Microsoft account that owns the game, Developer Mode, admin). Inject a native DLL; the DLL needs the `ALL_APPLICATION_PACKAGES` ACL because of the UWP sandbox [V] [native-modding wiki][bnm-uwp]. MCLauncher's README warns it has been "less reliable since the GDK migration" [V]. | **Recommended primary target** |
| Android | Must obtain the exact 1.16.100 APK legitimately [?], then inject a native `.so`, usually by repackaging and re-signing the APK. Smaller reference ecosystem, harder debugging. | Possible, much higher friction |
| Linux/macOS via [mcpelauncher][mcpelauncher] | Runs the Android build through a compatibility layer and has its own mod loading. Version support for 1.16.100 [?]. | Niche; not a first target |
| iOS, Xbox, PlayStation, Switch | No legitimate way to run our code (signed, locked platforms). | Not feasible |

## 4. Modification methods (Windows)

| Method | Can do | Can't do | Verdict |
|---|---|---|---|
| **A. Resource packs / JSON UI** | Restyle vanilla HUD and screens, textures, crosshair, layout. Works on every server, no injection. | Compute new data (CPS, timers), add menus with logic, change input. | Complementary layer |
| **B. Add-on scripting (1.16-era, experimental)** | World/behaviour logic in worlds you host. | Run on someone else's server. | Not useful for a PvP client [B] |
| **C. External overlay process** | Draw a separate transparent window over the game. | Read game state without memory reading; reliable in fullscreen; feel native. | Weak |
| **D. Injected native DLL** | Hook rendering, read game state, intercept input, draw in-game UI. This is how every Windows Bedrock client works (e.g. Onix, and Horion's archived 1.16-era source [V] [Horion][horion]). | — (cost is reverse engineering) | **Recommended core** |
| **E. Network proxy (MITM)** | See packets (ping, server data). | Render in-game; must handle Xbox auth; servers may object. | Not core |

**Key structural advantage of a frozen target:** Bedrock clients normally spend much of their effort re-finding
functions after every game update. Our build never changes, so function addresses and layouts are resolved once
and stay valid forever. That makes a native DLL far cheaper to maintain here than for current-version clients.

**Reverse-engineering support:** the 1.16.100 Windows BDS PDB [V] names the server-side classes shared with the client
(actors, items, effects, inventory, attributes). Client-only systems (UI rendering, input mapping, screens) are not
in BDS and must be found by hand, with Horion's archived 1.16 source as a *reference for where things are*.
That source is cheat-oriented and under a non-standard license, so we read it but do not copy code.

## 5. Stack implications (preliminary — decided in Stage 3)

- **C++20 with MSVC** is the front-runner. The game is MSVC-compiled C++, so matching its ABI and the Microsoft STL
  layouts (`std::string`, `std::vector`, `std::shared_ptr`) is natural. Mature hooking (MinHook, Detours) and nearly
  all Bedrock reference material are C++.
- **Rust** is viable (detour crates, `windows-rs`), but every game structure containing MSVC STL types needs
  hand-written layouts, and almost no Bedrock reference material is in Rust. That means more friction for no
  product benefit.
- **Launcher/injector:** small, separate; C++ or C#. Low risk either way.

Two candidate in-game rendering approaches, to be compared in the spike:

1. **Game's own UI render context** (Horion used it on 1.16): native Minecraft font and look, follows GUI scale,
   but depends on more reverse-engineered internals.
2. **Own renderer on the swap chain** (D3D `Present` hook + Dear ImGui or a custom renderer): independent of
   game internals and rich widgets, but a non-Minecraft look unless we skin it, and we must handle resize and fullscreen.

## 6. Controller input

- Vanilla Bedrock has a Controller settings tab with button remapping; its exact capabilities in 1.16.100 are [B].
  Our system must add value beyond it: profiles, layers, client bindings, and menu navigation.
- **Interception points:**
  1. **OS-level gamepad API** used by the game (Windows.Gaming.Input vs XInput: **[?]**, check the imports).
     Hooking the "read current state" call gives a game-agnostic layer that can remap, apply modifier layers,
     and *consume* buttons for client functions before the game sees them.
  2. **Game-internal input mapping** (button → game action). More semantic, needs reverse engineering. Use it
     only where (1) is insufficient.
- **Fair-play line:**
  - **Allowed:** 1:1 remapping; modifier layers (hold a button to shift the pad to a second layout); per-profile
    layouts; toggle sprint/sneak (accessibility; vanilla offers toggles); client-menu navigation; rumble feedback.
  - **Needs discussion:** dead-zone and response-curve tuning. These are standard accessibility settings, but they touch aim feel.
  - **Excluded:** turbo/rapid fire, auto-clickers, multi-action macros (e.g. one press = swap + throw pearl),
    any aim or movement automation.

## 7. Feature candidates (preliminary; Stage 2 will evaluate properly)

| Feature | Problem solved | Data source | Fair-play |
|---|---|---|---|
| HUD editor (controller-driven placement/scale/opacity) | Vanilla HUD is fixed | Client UI | Usability |
| FPS / ping / CPS (own inputs) | Performance and self-feedback | Renderer / network / own input | Usability |
| Controller input display | Streaming, practice, debugging binds | Input layer | Usability |
| Armor and held-item durability | Hidden in inventory | Own inventory | Usability |
| Potion/effect timers | Vanilla display is small or hidden | Own effects | Usability |
| Item counters (pearls, pots, blocks, arrows) | Counting mid-fight | Own inventory | Usability |
| Crosshair customization | Visibility, accessibility | Rendering | Usability |
| Toggle sprint/sneak, FOV control | Hand strain, comfort | Input / settings | Usability |
| **Contested — decide explicitly:** zoom, fullbright, freelook, hit-distance readout | — | — | Many servers restrict these |
| **Excluded:** anything in the project's fair-play list | — | — | — |

## 8. Risk register

| # | Risk | Severity | Mitigation |
|---|---|---|---|
| 1 | No servers to play on with 1.16.100 | **Critical** | Identify target servers/community first |
| 2 | Xbox Live sign-in fails on a 2020 client | **Critical** | Test on day one of the spike |
| 3 | 1.16.100 cannot be installed (MCLauncher post-GDK reliability, licensing) | High | Test on day one; document the install path |
| 4 | Servers forbid injected clients regardless of features | Medium | Check target servers' rules |
| 5 | Client-only systems are hard to reverse engineer | Medium | Frozen target, Horion as a map, BDS PDB for shared code |
| 6 | Antivirus flags the injector | Low | Signed builds, minimal injector |

## 9. Proposed next step: feasibility spike (throwaway code, Windows machine required)

Each step answers a [?] above:

1. Install UWP 1.16.100 via MCLauncher on Windows 10/11; record the exact build.
2. Sign in to Xbox Live; join each target server.
3. Inspect `Minecraft.Windows.exe` imports: `d3d11`/`d3d12`/`dxgi`; `Windows.Gaming.Input` vs `xinput`.
4. Inject a minimal DLL that logs, draws one rectangle through both rendering approaches, and reads and remaps one
   gamepad button.
5. Map 5–10 needed functions using the BDS PDB plus manual reverse engineering, to calibrate effort.

The spike needs a Windows PC with the game owned. This Linux workspace cannot run the game; it can only do static
analysis (e.g. of BDS binaries).

[wiki-1.16.100]: https://minecraft.wiki/w/Bedrock_Edition_1.16.100
[cl-1.16.200]: https://feedback.minecraft.net/hc/en-us/articles/360052712132-Minecraft-1-16-200-Bedrock
[hive-116]: https://forum.playhive.com/t/why-1-16-100-support-shouldnt-be-removed/52628
[cubecraft-116]: https://www.cubecraft.net/threads/%E2%9A%A0%EF%B8%8F-notice-removal-of-minecraft-1-16-bedrock-versions.299580/
[mc-help-nethernet]: https://help.minecraft.net/hc/en-us/articles/49914677714957
[geyser-nethernet]: https://geysermc.org/blog/preparing-for-nethernet/
[helix-mv]: https://github.com/helix-bedrock/multiversion
[mclauncher]: https://github.com/MCMrARM/mc-w10-version-launcher
[bnm-uwp]: https://bedrock-native-modding.github.io/wiki/platforms/uwp.html
[mcpelauncher]: https://github.com/minecraft-linux/mcpelauncher-manifest
[horion]: https://github.com/horionclient/Horion
