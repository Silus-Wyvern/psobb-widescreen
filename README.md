# psobb-widescreen

**Real widescreen + HudScale for *Phantasy Star Online: Blue Burst*.** One drop-in `.asi`
that scales the entire UI to 16:9 — or wider — and then gets out of the way. Bring your
own d3d8 wrapper; nothing on the client is touched on disk.

> **Tested on Windows 11 at 16:9 only.** Wider ratios (ultrawide) auto-detect but are
> unverified, as are Wine / Proton (Steam Deck) and macOS — reports welcome.

---

## Features

- **True widescreen / any resolution** — auto-detected from your display; 4:3 stays
  byte-for-byte stock. Verified at 16:9; 16:10 and ultrawide (21:9, 32:9) auto-detect too
  but are currently untested.
- **The whole UI** — title, login, ship / char select, char create, lobby, in-game HUD,
  minimap, menus and effects, not just the parts everyone notices.
- **HudScale** — one knob for the entire UI.
- **Quality-of-life customs** — minimap corner-pin, title rune-emblem scaling, photon-
  streak position fix, char-select 3D model pan, optional NoVignette.
- **Born-borderless virtual fullscreen** — no title bar, no IME / text-input breakage.
- **Optional ReShade post-FX** — SMAA, SSAO, Cel, DOF, HDR, with one-click installers.

---

## What makes it different

- **One `.asi`, any wrapper.** The widescreen logic is a standalone ASI, not welded into
  one specific d3d8 wrapper. Run it on crosire d3d8to9, dgVoodoo2, d3d8to11 or native d3d8
  and keep whatever wrapper you already like.
- **Declarative and self-healing.** Every coordinate the mod changes is one auditable
  table row with attribution; an overwrite guard re-asserts the table if another mod or
  wrapper stomps a site. Applied once at load — no per-frame patching, no flicker.
- **Actually complete.** Front-end through in-game, char-create included — and the rough
  edges other widescreen patches leave behind (the trapped char-create screen, squashed
  photon streaks, a drifting minimap) are fixed, not ignored.
- **Zero fiddling.** Aspect auto-detect, a true 4:3 no-op, sane defaults. Drop it in and
  launch.
- **Batteries included.** A curated ReShade post-FX package with per-wrapper installers
  ships in the repo (`reshade/`).

---

## Install — Windows

1. Grab the latest release `.zip` (`-dgvoodoo`, `-crosire`, or `-asi-only` — see
   "Recommended d3d8 wrapper" below).
2. Extract it straight into your PSOBB client folder — the one with `psobb.exe` — and
   let it merge: the mod lands in `patches\`, the wrapper sits next to `psobb.exe`, and
   the ReShade pack drops alongside.
3. Launch. Widescreen auto-enables from your display aspect; set your resolution in the
   launcher as usual.

The mod patches in memory at load — nothing in the client is changed on disk.

---

## Recommended d3d8 wrapper

The mod hooks Direct3D 8, so the client needs a `d3d8.dll` wrapper next to `psobb.exe`.
Either of these works — the mod doesn't care which:

**dgVoodoo2** — author's choice; robust against power interruptions and unclean shutdowns.
[dege.fw.hu/dgVoodoo2](http://dege.fw.hu/dgVoodoo2/), or grab the `-dgvoodoo` release zip
(bundled and preconfigured).

**crosire d3d8to9** — a lighter-weight alternative.
[github.com/crosire/d3d8to9](https://github.com/crosire/d3d8to9), or grab the `-crosire`
release zip. Needs the DirectX 9 runtime (`d3dx9_43.dll`) installed.

The `-asi-only` zip is the mod by itself if you already run a wrapper.

---

## Install — Steam Deck / Linux (Proton / Wine)

ASI mods load under Proton the same way they do on Windows once the loader is in place.
From the unzipped release:

```bash
./install_steamdeck.sh /path/to/your/PSOBB.IO
```

It copies the mod into `<gamedir>/patches/` (backing up anything it would overwrite) and
prints the Steam launch-option line to add if your `.asi` files aren't auto-loading under
Proton yet. (Untested by us — Deck / Wine reports welcome.)

---

## Config — `pso_widescreen.ini`

Widescreen auto-enables from your display aspect (4:3 stays stock), so most people never
need to touch this. **HudScale is set in your launcher** — it writes `HUDScale` to
`widescreen.cfg`, which the mod reads as the primary value (the ini's `HUDScale` is only a
fallback for non-launcher setups).

The keys most people tune are below; the ini itself documents **every** key, with a fenced
`ADVANCED` block for the auto-derived ones you shouldn't normally touch.

| Key | Meaning |
|---|---|
| `VideoEnable` | `1` (default) plays the bundled boot + char-create clips; `0` = stock (no clips). |
| `VideoTrigger` | `both` (default) = boot clip + char-create clip; or `boot` / `charcreate` for just one. |
| `VideoSkippable` | `1` (default) = **Enter** / Esc skips a clip; `0` = it plays to the end. |
| `MinimapSizeScale` | Minimap size, multiplied by HudScale. Lower it to shrink the minimap. |
| `MinimapCornerRightPx` / `…TopPx` | Nudge the minimap into the top-right corner (negative = further right / up). |
| `MinimapZoomEnable` / `MinimapZoom` | Map zoom; smaller `MinimapZoom` = zoomed out (see more around you). |
| `NoVignette` | `1` (default) removes the dark dim scrim behind the F12 / in-game menu; `0` = stock. |
| `IntegerScale` | `1` renders at a clean integer divisor of your screen then upscales sharply — helps high-DPI / 4K and the Wine-without-dgVoodoo FPS cliff. No-op at ≤1440p. Default `0`. |
| `BootPosterEnabled` | `1` (default) shows the logo PNG over the first loading screen; `0` = off. |

Auto-derived / advanced keys (`Enabled`, `Windowed`, `GameAspect`, `VideoDecoder`, the
`Patch*` toggles, video paths, …) live in the ini's `ADVANCED` block — leave them unless
you know what they do.

### Make it yours — boot splash & clips

These ship inside `patches\` and are meant to be swapped:

- **Boot splash** — `patches\psobb_boot_poster.png`. Drop in any PNG to change the image
  shown before the boot clip; delete it to skip straight to the clip.
- **Clips** — `patches\video\pso_bootseq_16x9.mp4` / `_4x3.mp4` (boot) and
  `pso_intros_16x9.mp4` / `_4x3.mp4` (char-create). Replace the matching aspect, or point
  `VideoBootPath` / `VideoPath` at your own file. **Enter** skips a clip.

Everything else (layout, resolution auto-detect, the QoL customs) is baked in and needs no
tuning.

---

## ReShade post-FX (optional)

The `reshade/` folder adds **SMAA, SSAO, Cel Shading, Depth of Field and HDR tone
mapping** on top of the rendered frame — composited by ReShade, with nothing baked into
the game or the widescreen ASI. It installs ReShade as the proxy DLL your d3d8 wrapper
already chains through.

**Windows:** run `reshade\install\install.bat` and pick your base wrapper (crosire
d3d8to9 or dgVoodoo2). It calls the matching PowerShell installer, downloads ReShade
(pinned version, printing the URL + SHA256), drops the curated shaders, and generates the
preset.

**Manual / advanced:**
```powershell
pwsh -File reshade\install\detect_wrapper.ps1      # which wrapper do I have?
pwsh -File reshade\install\install_dgvoodoo.ps1     # dgVoodoo2 base
pwsh -File reshade\install\install_crosire.ps1      # crosire d3d8to9 base
#   -GameDir <path>   -DryRun   -NoDepth   -Force
```

Idempotent and non-destructive (overwrites saved to `*.reshade-bak`). The depth-buffer
acceptance gate and per-effect notes are in [`reshade/README.md`](reshade/README.md).

---

## How it works

PSOBB lays its entire 2D UI out in a **640×480 design space** and multiplies it through
one affine at draw time. The mod widens that design space
(`design_w = 853.33 × HudScale`, `design_h = 480 × HudScale`), re-anchors each element to
the correct edge / center of the wider canvas, and keeps screen-space effects off the
wide affine.

The way it does that is the interesting part: every engine-memory coordinate write is one
row of a declarative table (`kBakes[]`) — `value = (coeff·base + offset)·base2`, resolved
from the live scale so 4:3 is a structural no-op — applied once by a value-guarded pass,
with a handful of per-draw detours and an overwrite guard that re-asserts the table if
another wrapper or mod clobbers a site. The whole thing is one `pso_widescreen.c` +
`pso_widescreen.h`.

---

## Build

MSVC toolchain (32-bit). From `asi/`:

```
build.bat
```

→ `pso_widescreen.asi`. Drop it + `pso_widescreen.ini` into `<client>\patches\`.

---

## Shoutouts

This builds on a lot of prior work, and wouldn't exist without it:

- **anzz1** — the PSOBB widescreen this mod is built on. This mod is that widescreen,
  pulled out into a standalone, wrapper-agnostic ASI.
- **Trinity DLL** — the static widescreen patch set used to name and cross-check every
  anchor VA.
- **llama-bob** — the clean-C reconstruction of the widescreen engine
  ([github.com/llama-bob/psobb-ephinea-re](https://github.com/llama-bob/psobb-ephinea-re)).
- **Ephinea** — the widescreen layout was reverse-engineered using their client as the
  reference; big respect to that project. (Ephinea is a separate community server, not
  affiliated with this mod.) The full RE lives in the companion
  [psobb-ephinea-re](https://github.com/therealpixelated/psobb-ephinea-re) repo.
- **MinHook** by Tsuda Kageyu — the inline-hook engine every detour is built on
  ([github.com/TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook), BSD 2-Clause;
  vendored under `_shared/minhook/`).
- **ReShade** by crosire, and the SMAA and shader authors — the optional post-FX layer
  ([github.com/crosire/reshade](https://github.com/crosire/reshade)).

Full third-party licenses, repos and RE lineage: **[LICENSES.md](LICENSES.md)**.

*Phantasy Star Online* and its assets are © SEGA.
