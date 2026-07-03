# Licenses & Attribution

`psobb-widescreen` stands on a lot of prior community work and a few open-source
libraries. This file lists every third-party component it bundles, links against, or was
reverse-engineered from — with its author, license, and where the license text lives.

The mod's own source (`asi/`) is © the psobb-widescreen author. The sections below cover
**third-party** code and references only.

---

## Bundled / linked code (formal licenses)

| Component | Used for | Author | License | Where the license lives |
|---|---|---|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) | the inline API-hook engine every detour is built on | Tsuda Kageyu | **BSD 2-Clause** | vendored — [`_shared/minhook/LICENSE.txt`](_shared/minhook/LICENSE.txt) |
| [ReShade](https://github.com/crosire/reshade) | optional post-FX proxy (SMAA, SSAO, Cel, DOF, HDR) | crosire | **BSD 3-Clause** | [`reshade/LICENSES/BSD3-ReShade.txt`](reshade/LICENSES/BSD3-ReShade.txt) |
| [SMAA](https://github.com/iryoku/smaa) shader | anti-aliasing in the ReShade pack | Jorge Jimenez et al. | **MIT** | [`reshade/LICENSES/MIT-SMAA.txt`](reshade/LICENSES/MIT-SMAA.txt) |
| [crosire d3d8to9](https://github.com/crosire/d3d8to9) | recommended d3d8 wrapper (user-installed, not bundled) | crosire | **BSD 3-Clause** | see upstream repo |
| [dgVoodoo2](http://dege.fw.hu/dgVoodoo2/) | recommended d3d8 wrapper (bundled in the `-dgvoodoo` release) | Dege | **Freeware** (Dege's EULA) | see [dege.fw.hu/dgVoodoo2](http://dege.fw.hu/dgVoodoo2/) |

The custom ReShade effects shipped in `reshade/shaders/Shaders/` (`PSO_CelShader.fx`,
`PSO_DOF.fx`, `PSO_HDRToneMap.fx`, `PSO_SSAO.fx`, `DisplayDepth.fx`) are authored for this
package and released under the same terms as the mod.

---

## Reverse-engineering lineage (community work — no formal license)

The widescreen anchor tables are grown from community reverse-engineering that predates
this repo. These are credited by attribution; where no upstream license exists, none is
claimed:

- **anzz1** — the PSOBB widescreen this mod is built on; the anchor tables here are grown
  from anzz1's work. This mod is that widescreen pulled out into a standalone,
  wrapper-agnostic ASI.
- **tofuman** — the `WideScreen.c` reference used to cross-check anchor math.
- **Trinity DLL** — the static widescreen patch set used to name and cross-check every
  anchor VA.
- **llama-bob** — the clean-C reconstruction of the widescreen engine:
  [github.com/llama-bob/psobb-ephinea-re](https://github.com/llama-bob/psobb-ephinea-re).
- **Ephinea** — the layout was reverse-engineered using their client as the reference
  (a separate community server, unaffiliated with this mod). The full RE lives in the
  companion [therealpixelated/psobb-ephinea-re](https://github.com/therealpixelated/psobb-ephinea-re).

---

## Game assets

*Phantasy Star Online* and *Phantasy Star Online: Blue Burst*, and all of their code,
art, audio, and data, are **© SEGA / Sonic Team**. None of that game data is included in
or redistributed by this repository — the mod patches the client in memory at load and
touches nothing on disk. Bring your own legally-obtained client.
