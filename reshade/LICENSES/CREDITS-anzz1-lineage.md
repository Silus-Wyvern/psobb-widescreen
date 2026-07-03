# Credits & licenses — PSOBB ReShade package

This package ships ReShade-FX ports of the post-processing shaders from anzz1's
`psobb_patches/psobb_widescreen`, plus install tooling. The shader math and
default parameters are preserved 1:1 from the anzz1 sources; only the runtime
plumbing (depth access, pass declarations) was adapted to ReShade FX.

## Bundled in this repo

| File | Effect lineage | License / status |
|------|----------------|------------------|
| `shaders/Shaders/PSO_SSAO.fx` | SSAO by **Arkano22**, assembled/optimized by **Martins Upitis (martinsh)**; PSOBB .fx packaging by **anzz1** | free / public technique; attribution in header |
| `shaders/Shaders/PSO_DOF.fx` | DoF by **WrinklyNinja** (v7); packaging by **anzz1** | free; attribution in header |
| `shaders/Shaders/PSO_CelShader.fx` | Cel shading from **GSFx Shader Suite by Asmodean**; packaging by **anzz1** | GSFx has no formal OSS license — bundling is owner go/no-go |
| `shaders/Shaders/PSO_HDRToneMap.fx` | HDR tonemap from **GSFx Shader Suite by Asmodean**; packaging by **anzz1** | GSFx has no formal OSS license — bundling is owner go/no-go |
| `shaders/Shaders/DisplayDepth.fx` | depth acceptance test; reduced standalone variant of crosire's DisplayDepth | self-authored; mirrors CC0-1.0 upstream |
| `shaders/Shaders/ReShade.fxh` | common ReShade FX interface | self-authored re-implementation; upstream is CC0-1.0 |
| `shaders/Shaders/ReShadeUI.fxh` | UI macro compatibility stub | self-authored; upstream is CC0-1.0 |

## Fetched at install time (NOT committed)

| What | Source | License | Fetched by |
|------|--------|---------|------------|
| ReShade (Addon edition) DLL | https://reshade.me (pinned 6.5.1) | BSD-3-Clause | `install_*.ps1` |
| `SMAA.fx` + `SMAA.fxh` + `AreaTex.dds` + `SearchTex.dds` | crosire/reshade-shaders (pinned commit) | BSD-3-Clause (Iryoku MIT core) | `fetch_smaa.ps1` |
| `qUINT_mxao.fx` (optional) | martymcmodding/qUINT | **All rights reserved — NOT redistributable** | `fetch_mxao.ps1` (opt-in) |

## Notes

- **Asmodean GSFx (Cel + HDR):** no formal open-source license. If the owner
  declines to bundle these, the MIT fallbacks are SweetFX `Cartoon.fx` (cel) and
  SweetFX `Tonemap.fx` + `LevelsPlus.fx` (HDR) — different look, clean license.
- **qUINT MXAO:** never committed (a PSOBB preset repo was DMCA'd in 2025 for
  bundling it). `fetch_mxao.ps1` downloads it on explicit consent for personal use.
- **SMAA:** the same Iryoku MIT-core implementation anzz1 used; fetched from
  crosire (BSD-3) rather than hand-transcribed, to guarantee it compiles.
