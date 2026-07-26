// pso_widescreen.c — stack-agnostic widescreen / resolution-override ASI
// for PSOBB.
//
// Originally Sodaboy's d3d8.dll wrapper bundled this with his
// post-FX (MSAA/SMAA/SSAO/etc.) — extracting just the widescreen + scale
// part means anyone can pick whichever d3d8.dll they want (Sodaboy's,
// d3d8to11, dgVoodoo, native, etc.) and still get a non-4:3 backbuffer.
//
// Hook strategy (chosen to be d3d8-implementation-agnostic):
//
// 1. IAT-patch psobb.exe's import of `d3d8.dll!Direct3DCreate8`. The
// patch lives in PSOBB's import table, so it fires regardless of
// which DLL is currently resolving the d3d8.dll name — Sodaboy's
// shim, d3d8to11, native d3d8, etc.
//
// 2. When PSOBB calls our hooked Direct3DCreate8, we forward to the
// real one, get an IDirect3D8 *factory back, then vtable-patch
// its `CreateDevice` slot (slot 15 in the d3d8 vtable). The
// vtable patch is once-per-factory; subsequent CreateDevice
// calls on the same instance flow through us.
//
// 3. Our CreateDevice hook rewrites the user's D3DPRESENT_PARAMETERS
// to substitute the configured BackBufferWidth/Height and the
// windowed flag, then forwards to the real CreateDevice. The
// engine still queries device caps + creates its swapchain
// against the new resolution.
//
// What we deliberately do NOT do (yet):
// - HUD scale — Sodaboy's HUDScale knob requires hooking the engine's
// HUD draw path (DrawPrimitiveUP with screen-space coords). That
// belongs in a separate ASI / a follow-up patch.
// - FOV / aspect adjustment — PSOBB's projection matrix is built
// internally; getting "hor+" (more world horizontally instead of
// vertical squish) needs a SetTransform(D3DTS_PROJECTION) hook.
// Also a follow-up.
// - Post-FX (MSAA/SMAA/SSAO/CelShader/DOF/HDR) — those were Sodaboy's
// d3d8.dll post-pipeline. Now dgVoodoo is out of the chain too,
// ReShade is the right home for those (already installed at
// <install>\reshade-shaders).
//
// Config: <psobb.exe dir>\widescreen.cfg, simple Key=Value lines, no
// sections. Same format Sodaboy uses so an existing user's file just
// works. We only consume Windowed / Width / Height / DebugLogsEnabled
// — keys we don't recognise are ignored without complaint.

#include <Windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <MinHook.h>          // MinHook detours (char-create composer hooks).
#include "asset_registry.h"
#include "pso_widescreen.h"

// ---- Minimal D3D8 ABI we need ----
//
// We don't include d3d8.h because it's not always present in modern SDKs
// and we only need vtable layouts, not the full type machinery. The
// IDirect3D8 vtable indices are stable since DirectX 8.0 (2000).

typedef struct IDirect3D8         IDirect3D8;
typedef struct IDirect3DDevice8   IDirect3DDevice8;

// D3DPRESENT_PARAMETERS as of d3d8: layout is critical, the engine
// passes a pointer to one of these and we mutate fields by offset.
typedef struct {
    UINT  BackBufferWidth;
    UINT  BackBufferHeight;
    DWORD BackBufferFormat;
    UINT  BackBufferCount;
    DWORD MultiSampleType;
    DWORD SwapEffect;
    HWND  hDeviceWindow;
    BOOL  Windowed;
    BOOL  EnableAutoDepthStencil;
    DWORD AutoDepthStencilFormat;
    DWORD Flags;
    UINT  FullScreen_RefreshRateInHz;
    UINT  FullScreen_PresentationInterval;
} D3DPRESENT_PARAMETERS_X;

// IDirect3D8::CreateDevice signature — same calling convention as DX8.
typedef HRESULT (STDMETHODCALLTYPE *CreateDevice_t)(
    IDirect3D8 *self,
    UINT Adapter,
    DWORD DeviceType,
    HWND hFocusWindow,
    DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS_X *pPresentationParameters,
    IDirect3DDevice8 **ppReturnedDeviceInterface);

// IDirect3DDevice8::SetTransform — slot 37 in the d3d8 device vtable.
// We intercept D3DTS_PROJECTION (transform type 3) to apply hor+ FOV
// correction, otherwise pass through unmodified.
typedef struct {
    float _11, _12, _13, _14;
    float _21, _22, _23, _24;
    float _31, _32, _33, _34;
    float _41, _42, _43, _44;
} D3DMATRIX_X;

// IDirect3D8::GetAdapterCount - slot 4 in the same vtable (after
// QueryInterface/AddRef/Release at 0..2 and RegisterSoftwareDevice at 3).
// Used only to sanity-check a configured Adapter ordinal before we hand it
// to CreateDevice.
typedef UINT (STDMETHODCALLTYPE *GetAdapterCount_t)(IDirect3D8 *self);

// IDirect3D8::GetAdapterIdentifier - slot 5, next to GetAdapterCount. Used
// purely for diagnostics: naming the adapters makes it obvious whether the
// game is talking to real hardware or to a wrapper's virtual card (dgVoodoo2
// presents one "internal3D" adapter regardless of how many GPUs are fitted).
#define WS_ADAPTER_ID_STRLEN 512
typedef struct {
    char          Driver[WS_ADAPTER_ID_STRLEN];
    char          Description[WS_ADAPTER_ID_STRLEN];
    LARGE_INTEGER DriverVersion;
    DWORD         VendorId;
    DWORD         DeviceId;
    DWORD         SubSysId;
    DWORD         Revision;
    GUID          DeviceIdentifier;
    DWORD         WHQLLevel;
} D3DADAPTER_IDENTIFIER8_X;

typedef HRESULT (STDMETHODCALLTYPE *GetAdapterIdentifier_t)(
    IDirect3D8 *self, UINT Adapter, DWORD Flags,
    D3DADAPTER_IDENTIFIER8_X *pIdentifier);

typedef HRESULT (STDMETHODCALLTYPE *SetTransform_t)(
    IDirect3DDevice8 *self, DWORD State, const D3DMATRIX_X *pMatrix);

#define D3DTS_VIEW       2
#define D3DTS_PROJECTION 3

// IDirect3DDevice8::SetViewport — slot 40 in the d3d8 device vtable.
// PSOBB calls SetViewport(0, 0, engine_w, engine_h) — usually the
// launcher-selected resolution like 1024x768 — once the device is up,
// even though we forced a 3840x2160 backbuffer. The result without a
// hook is "engine renders to upper-left 1024x768 region of a 4K
// backbuffer". We override X/Y/Width/Height to fill the full
// configured backbuffer (MinZ/MaxZ pass through unchanged).
typedef struct {
    DWORD X;
    DWORD Y;
    DWORD Width;
    DWORD Height;
    float MinZ;
    float MaxZ;
} D3DVIEWPORT8_X;

typedef HRESULT (STDMETHODCALLTYPE *SetViewport_t)(
    IDirect3DDevice8 *self, const D3DVIEWPORT8_X *pViewport);

// d3d8.dll!Direct3DCreate8 signature.
typedef IDirect3D8 *(WINAPI *Direct3DCreate8_t)(UINT SDKVersion);

// ---- Config ----
//
// We read these once at DllMain attach. There's no live-reload because
// CreateDevice fires very early in PSOBB startup (before any user could
// have edited the config) and changing resolution mid-flight requires
// IDirect3DDevice8::Reset. (We DO now intercept Reset — see Hook_Reset — but
// only for lost-device resource discipline on alt-tab, NOT to re-read cfg or
// change resolution; the present-params are left exactly as the engine built
// them.)

static struct {
    int   enabled;            // master switch — default 0, set to 1 in cfg
    int   windowed;           // 0=fullscreen, 1=windowed, 2=borderless windowed
    int   width;              // target dimensions (window + optional backbuffer)
    int   height;
    // Which D3D8 adapter (GPU) to create the device on. -1 = leave the
    // engine's own choice alone (default, previous behaviour). Otherwise the
    // ordinal is validated against GetAdapterCount() at CreateDevice time and
    // ignored if out of range, so a stale value can never stop the game
    // starting. INI key Adapter; widescreen.cfg Adapter is PRIMARY.
    int   adapter;
    // Sodaboy convention — engine canvas is FIXED at logical
    // 1920x1080 regardless of physical Width/Height. The d3d8 wrapper
    // (Sodaboy or dgVoodoo) handles upscaling 1920x1080 to physical.
    // Patching engine internals (res_table, anchors) with PHYSICAL Width/
    // Height makes downstream coord math 2x too big at 4K, which is what
    // crippled our 2D UI in the baseline diff. Default 1920/1080;
    // INI keys LogicalWidth / LogicalHeight override.
    int   logical_width;
    int   logical_height;
    int   override_backbuffer; // 1 = rewrite pp->BackBufferWidth/Height to width/height.
                               // Default 0 (stretch mode): leave engine's native
                               // backbuffer alone and let the swap chain stretch
                               // to the borderless window. Setting to 1 selects
                               // the engine-native render path which requires
                               // matching memory patches in psobb.exe to avoid
                               // RHW HUD verts being mapped to the upper-left.
    int   override_viewport;   // 1 = rewrite SetViewport to backbuffer dims. Only
                               // useful with override_backbuffer=1. Default 0.
    int   integer_scale;       // 1 = "bump the base render": render the backbuffer at a
                               // CLEAN integer divisor of the physical Width/Height
                               // (capped ~1080p) so the present upscale is a clean Nx
                               // (4K -> render 1920x1080, present 2x) instead of an
                               // ugly non-integer/aspect-mismatched stretch (1280x800
                               // 16:10 -> 4K 16:9). Fixes the Wine/wined3d 4K present
                               // perf cliff w/o dgVoodoo. No-op <=1440p. Forces
                               // override_backbuffer + override_viewport. Default 0.
    int   fov_correction;      // 1 = scale projection m11 for hor+ widescreen
    float game_aspect;         // engine-assumed aspect ratio (default 4/3)
    int   patch_splash_phase2; // 1 = widen the SEGA/boot splash sprite-quad
                               // table at .data 0x009A3420 (4 entries × 0x20
                               // stride; x1@+0x00, x2@+0x08). D3 
                               // now ALWAYS-ON on the default path (folded into
                               // apply_static_patches), de-conflicted against
                               // anzz1 — the 2 X-coords anzz1 owns
                               // (0x9A3468/0x9A3488, in listHUDWidth) are SKIPPED
                               // so anzz1's horizontal extent A wins there.
                               // Default 1; per-feature knob (D1).
    float hud_scale;           // master UI mem-patch scale (mults the patched
                               // patched UI offsets). 1.0 = canonical widescreen
                               // positioning; >1 = HUD spread further toward
                               // edges; <1 = HUD shrinks toward 4:3 anchor (loses
                               // widescreen positioning). Default 1.0.
    // HANGAME-TITLE-MENU FIX (proven via live RE). The hangame
    // title "Login / Start Game / Exit Game" ListWindow is constructed by
    // sub_00761c48 (gated behind the hangame getter 0x0082D2F8) from an
    // in-image layout struct at 0x00974E08 = { X(+0x00)=210.0, Y(+0x04)=340.0,
    // W(+0x08)=220.0, ... } passed to CreateListWindowObject_0073566c. There
    // is exactly ONE xref to 0x00974E08 (the push at 0x00761C63), so the
    // struct is private to this menu — patching it cannot affect any in-game
    // list window. The container copies X/Y from the struct AT CONSTRUCTION,
    // so a live data-patch after the title is up does nothing; we must patch
    // the in-image struct at ASI-load (before the title screen builds). At the
    // global 1.5x UI matrix, design X=210 lands the box left-of-center (~480px,
    // behind the logo). Setting X=530 centers the 220-wide box (center 640 ->
    // 960px); Y=560 drops it to the lower-center over "PRESS ENTER KEY".
    // LIVE-VALIDATED by writing the constructed container's X/Y and
    // confirming on screen.
    // RUNE-EMBLEM SCALE. Title circular rune seal (TitleEP4_11/_12,
    // the ring + 3 spinning orbs). Scales with the MASTER UI scale (hud_scale),
    // stock values read live from the binary. INI: PatchRuneScale (on/off).
    int   patch_rune_scale;    // 1 = scale the title rune emblem with the master UI scale. Default 1.
    // PHOTON-STREAK SCALE FIX. The streak emitter reads its viewport-space
    // spawn position as `[0x96E27C] * affine` (X) and `[0x96E278] * affine` (Y), where
    // affine = viewport_w / design_w = 1920 / (853.333 * hud_scale). At hud_scale > 1.0
    // the affine shrinks (e.g. 1.125 at hs2.0) and the streak collapses toward the upper-
    // left corner. Ephinea at hs2.0 uses 330.0/402.0 to compensate. The correct fix is to
    // bake scale-compensated constants at ASI load: constant = target_px / affine =
    // target_px * hud_scale / 2.25, giving the same pixel position at any HudScale.
    // Target matches Ephinea's hs2.0 pixel position: X_px=371.25, Y_px=452.25.
    // 0x96E27C_new = 165.0 * hud_scale  (165 * 2.0 = 330 = Ephinea at hs2.0)
    // 0x96E278_new = 201.0 * hud_scale  (201 * 2.0 = 402 = Ephinea at hs2.0)
    // INI: PatchStreakScale.
    int   patch_streak_scale;  // 1 = fix streak position scale. Default 1.
    // CHAR-SELECT 16:9 layout (info panel / char list / legend).
    int   patch_charselect;    // 1 = reposition char-select UI for 16:9. Default 1.
    // CHAR-CREATE convenience: ESC at the TOP (category/class-select)
    // page transitions back to char-select. Stock ESC at the top does nothing
    // (deeper pages already back up one step in the engine). Additionally gated on
    // patch_charselect (the char-create escape hatch). Default 1.
    // Char-create renders via the static apply_startup_bakes() pass + stock; the
    // runtime-only class-select content rows are intentionally not strided (no
    // static source exists to bake).
    // MINIMAP top-right anchor (CUSTOM 3). patch_minimap gates the new absolute
    // corner anchor in apply_special(): both minimap layers (viewport map graphic +
    // gray background box) are re-derived every pass from native 640x480 fractions
    // scaled to the live design space, sized by MinimapSizeScale * HudScale, and
    // pinned MinimapCornerRightPx / MinimapCornerTopPx from the design top-right.
    int   patch_minimap;       // 1 = apply the minimap top-right anchor. Default 1.
    int   no_vignette;         // 1 = NOP the 3 F12/in-game-MENU vignette sites (the dark menu-dim scrim). NOT mod_vignette (status FX). Default 1.
    // CUSTOM C3 — minimap SIZE multiplier, applied ON TOP of HudScale. The engine
    // multiplies every minimap field by render/design before drawing, so the W/H we
    // write (128*MinimapSizeScale*HudScale, design space) scales with both the knob
    // and HudScale. 1.0 = stock size at hs1.0. INI: MinimapSizeScale.
    float minimap_size_scale;  // default 1.30 (bigger minimap). 1.0 = stock.
    // CUSTOM C3 — design-space top-right margins for the absolute corner anchor.
    // The X anchor lands the box's RIGHT edge cr px in from the native right edge,
    // the Y anchor lands its TOP edge ct px down from the native top. INI:
    // MinimapCornerRightPx / MinimapCornerTopPx.
    float minimap_corner_right_px; // default 0.0 (no extra right margin).
    float minimap_corner_top_px;   // default 0.0 (no extra top margin).
    // CUSTOM C3 — compass (minimap direction icon) position from the BOTTOM-RIGHT screen
    // corner, in design px. Bigger = further from the corner. INI: CompassRightPx /
    // CompassBottomPx.
    float compass_right_px;        // default 60.0 (in from the right edge).
    float compass_bottom_px;       // default 70.0 (up from the bottom edge).
    // CUSTOM C3 — minimap ZOOM (ported from the standalone pso_minimap.asi). A single
    // float imm32 in the engine's .text at 0x00804A5D, pushed into the minimap
    // projection setup at 0x00804A65. SMALLER = zoomed OUT (wider minimap field of
    // view). Stock = 1.133799. Resolution-INDEPENDENT flat constant divisor — do NOT
    // entangle it with HudScale or the s->A/C scale ctx. Disjoint from the corner-pin
    // .data slots above. INI: MinimapZoomEnable / MinimapZoom.
    int   patch_minimap_zoom;  // 1 = overwrite the .text zoom divisor. Default 1.
    float minimap_zoom;        // default 0.60 (matches the prior pso_minimap default). Clamp [0.1,3.0].
    int   debug_log;
    int   debug_log_verbose;   // 1 = firehose mode: emits the extra per-feature
                               // diagnostic lines (e.g. the char-create backdrop
                               // column-fill trace). Default 0. DebugLogsEnabled
                               // must ALSO be 1 for the verbose lines to land
                               // in pso_widescreen.log (verbose is purely a
                               // gating extension on top of debug_log).

    // ---- Boot poster (mod_boot_poster.c) ----
    // Centered PNG overlay during the very first "NOW LOADING" black
    // screen. Auto-disables once SEGA splash atlas appears, the engine
    // loads a real floor, or MaxSeconds elapses. Lives in pso_widescreen
    // because it's pure visual / D3D8 plumbing — same architectural rule
    // as the splash-phase patches above.
    int   boot_poster_enabled;
    int   boot_poster_disable_after_floor;
    int   boot_poster_disable_after_splash;
    int   boot_poster_max_seconds;
    float boot_poster_max_screen_pct;
    char  boot_poster_path[MAX_PATH];

    // ---- P3 video player (mod_video.c) — Stage 1 ----
    // Skippable FFmpeg-decoded video, blitted via the same Present quad as
    // the boot poster. Default OFF (VideoEnable=0) => mod_video stays fully
    // dormant and the build is byte-identical in behavior to today.
    int   video_enabled;           // VideoEnable        (0 = OFF, default)
    int   video_skippable;         // VideoSkippable     (1 = Enter/Esc skip)
    int   video_max_seconds;       // VideoMaxSeconds    (per-playback watchdog)
    int   video_skip_debounce_ms;  // VideoSkipDebounceMs(release-then-press arm)
    int   video_audio;             // VideoAudio         (sibling .wav via mci)
    int   video_diag;              // VideoDiag          (1 = red half-quad diag)
    int   video_trigger;           // VideoTrigger       (VID_TRIGGER_OFF/BOOT/CHARCREATE)
    int   video_decoder;           // VideoDecoder       (VID_DECODER_MF/FFMPEG)
    char  video_path[MAX_PATH];      // VideoPath        (CHAR-CREATE source .mp4)
    char  video_boot_path[MAX_PATH]; // VideoBootPath    (BOOT source; trigger=both)
    char  video_ffmpeg[MAX_PATH];    // VideoFfmpeg      ("" => "ffmpeg" on PATH)

    char  log_path[MAX_PATH];
} g_cfg;

// ---- ws_scale_ctx — the ONE universal scaling gate ----
//
// A single computed struct, derived once at the end of load_config() via
// ws_compute_scale(). The master gate everything consults is `g_scale.enabled`
// (no widescreen_engine selector branch anywhere). front-end-native is the
// VALUE hud_scale==1.0, not a branch. The A/B/C/D anzz1 extents AND the
// render_w/render_h derivation are computed HERE, in exactly one place, then
// passed to the anzz1 bake (MINOR-3).
typedef struct {
    int   enabled;             // master: g_cfg.enabled. The ONE gate.
    float hud_scale;           // UI magnitude. 1.0 == front-end native.
    float game_aspect;         // AR feeding the anzz1 extents.
    int   render_w, render_h;  // physical render res for the 0x009006F4 table.
    float A, B, C;             // anzz1 horizontal/atlas/vertical extents.
    DWORD D;                   // anzz1 integer tile-height unit.
} ws_scale_ctx;

static ws_scale_ctx g_scale;   // populated at end of load_config().
static void ws_compute_scale(ws_scale_ctx *s);  // defined after load_config().

// ---- Boot poster bridge (defined in mod_boot_poster.c) ----
extern void boot_poster_init_from_cfg(const char *path, int enabled,
                                      int disable_after_floor,
                                      int disable_after_splash_atlas,
                                      int max_seconds, float max_screen_pct);
extern void boot_poster_on_present(void *device, int viewport_w, int viewport_h);
// Lost-device bridges (alt-tab / dgVoodoo2 Reset) — called from Hook_Reset.
extern void boot_poster_on_device_lost(void);
extern void boot_poster_on_device_reset(void);
extern void boot_poster_log_summary(void);

// ---- P3 video bridge (defined in mod_video.c) ----
// VideoTrigger modes — MUST mirror the VID_TRIGGER_* macros in mod_video.h
// (kept inline here to match this file's extern-bridge style; mod_video.c is
// the consumer of the value and owns the authoritative definitions).
#define VID_TRIGGER_OFF         0
#define VID_TRIGGER_BOOT        1
#define VID_TRIGGER_CHARCREATE  2
#define VID_TRIGGER_BOTH        3
// VideoDecoder modes — MUST mirror the VID_DECODER_* macros in mod_video.h.
#define VID_DECODER_MF          0
#define VID_DECODER_FFMPEG      1
extern void mod_video_init(const char *path, const char *boot_path, int enabled,
                           int skippable, const char *ffmpeg_path, int max_seconds,
                           int skip_debounce_ms, int audio, int diag,
                           int trigger, int decoder);
extern void mod_video_on_present(void *device, int viewport_w, int viewport_h);
// Lost-device bridges (alt-tab / dgVoodoo2 Reset) — called from Hook_Reset.
extern void mod_video_on_device_lost(void);
extern void mod_video_on_device_reset(void);
extern void mod_video_log_summary(void);

// ---- Logging ----
//
// File log next to the EXE, gated on DebugLogsEnabled=1. We write via
// CreateFile/WriteFile to keep CRT use minimal — DllMain is a fragile
// place to call into the C runtime.

static void resolve_log_path(void)
{
    char exe[MAX_PATH] = {0};
    if (GetModuleFileNameA(NULL, exe, MAX_PATH) == 0) return;
    char *slash = strrchr(exe, '\\');
    if (!slash) return;
    *(slash + 1) = 0;
    _snprintf_s(g_cfg.log_path, MAX_PATH, _TRUNCATE,
                "%spso_widescreen.log", exe);
}

// Persistent log file handle. Opened lazily on first emit, kept until
// DLL_PROCESS_DETACH. Avoids the syscall storm of CreateFile+CloseHandle
// per call when verbose mode emits thousands of lines per frame.
static HANDLE g_log_handle = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_log_cs;
static int  g_log_cs_init = 0;

// External-linkage so mod_boot_poster.c (separate translation unit) can
// route its log lines through the same handle / file.
void log_line(const char *fmt, ...)
{
    if (!g_cfg.debug_log) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(buf);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
    if (!g_cfg.log_path[0]) return;
    if (!g_log_cs_init) {
        // First-ever call: init lock + open file. Race here is benign
        // because DllMain is serialized by the loader; the very first
        // log_line() call comes from DllMain.
        InitializeCriticalSection(&g_log_cs);
        g_log_cs_init = 1;
    }
    EnterCriticalSection(&g_log_cs);
    if (g_log_handle == INVALID_HANDLE_VALUE) {
        g_log_handle = CreateFileA(g_cfg.log_path, FILE_APPEND_DATA,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_log_handle != INVALID_HANDLE_VALUE) {
            SetFilePointer(g_log_handle, 0, NULL, FILE_END);
        }
    }
    if (g_log_handle != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(g_log_handle, buf, (DWORD)n, &wrote, NULL);
        WriteFile(g_log_handle, "\r\n", 2, &wrote, NULL);
        // Flush per line so a scene transition or a char-create crash can
        // never lose buffered lines. The kernel-mode FlushFileBuffers commits
        // the WriteFile to disk before we return.
        FlushFileBuffers(g_log_handle);
    }
    LeaveCriticalSection(&g_log_cs);
}

static void load_config(void)
{
    g_cfg.enabled            = 0;
    g_cfg.windowed           = 1;
    g_cfg.width              = 0;
    g_cfg.height             = 0;
    g_cfg.adapter            = -1;     // -1 = don't touch the engine's choice
    g_cfg.logical_width      = 1920;   // Sodaboy-convention engine canvas
    g_cfg.logical_height     = 1080;
    g_cfg.override_backbuffer = 0; // stretch mode by default
    g_cfg.override_viewport   = 0;
    g_cfg.integer_scale       = 0; // "bump base render" to a clean divisor; opt-in
    g_cfg.fov_correction      = 0;
    g_cfg.game_aspect         = 16.0f / 9.0f;  // 16:9 (baked, no INI key)
    g_cfg.patch_splash_phase2 = 0;  // default 0. On
                                    // the default path (folded into
                                    // apply_static_patches). 0x009A3420 quad
                                    // table; de-conflicted vs anzz1 (skips the
                                    // 2 X-coords anzz1 owns). Per-feature knob.
    g_cfg.hud_scale           = 1.0f;  // mem patches: canonical widescreen
    g_cfg.patch_rune_scale    = 1;     // scale rune emblem with master UI scale.
    g_cfg.patch_charselect    = 1;     // Char-select 16:9 layout (info panel,
                                       // char list, frames, legend buttons +
                                       // footer Enter/Cancel stride pokes).
                                       // Model pan runs unconditionally via
                                       // patch_charselect_model_pan() (orthogonal world-space).
    // Char-create is owned by the static apply_startup_bakes() pass; no per-frame
    // cfg drives it.
    g_cfg.patch_streak_scale  = 1;     // fix streak position at any HudScale.
    // minimap top-right anchor (CUSTOM 3). patch_minimap gates the size/anchor knobs below.
    g_cfg.patch_minimap        = 1;
    g_cfg.no_vignette          = 1;     // F12/in-game-MENU vignette OFF (clean view). NOT mod_vignette (status FX).
    g_cfg.minimap_size_scale   =  1.30f;  // C3: minimap size multiplier ON TOP of HudScale.
    g_cfg.minimap_corner_right_px = 0.0f; // C3: design-space right margin from the top-right corner.
    g_cfg.minimap_corner_top_px   = 0.0f; // C3: design-space top margin from the top-right corner.
    g_cfg.compass_right_px        = 60.0f; // C3: compass in from the bottom-right corner (design px).
    g_cfg.compass_bottom_px       = 70.0f; // C3: compass up from the bottom-right corner (design px).
    g_cfg.patch_minimap_zoom  = 1;        // C3: overwrite the .text minimap zoom divisor.
    g_cfg.minimap_zoom        = 0.60f;    // C3: zoomed-out default (stock 1.1338). Smaller = wider FOV.
    g_cfg.debug_log           = 0;
    g_cfg.debug_log_verbose   = 0;
    // Boot poster defaults — feature ON by default, sensible auto-disable.
    g_cfg.boot_poster_enabled              = 1;
    g_cfg.boot_poster_disable_after_floor  = 1;
    g_cfg.boot_poster_disable_after_splash = 1;
    g_cfg.boot_poster_max_seconds          = 30;
    g_cfg.boot_poster_max_screen_pct       = 80.0f;
    _snprintf_s(g_cfg.boot_poster_path, MAX_PATH, _TRUNCATE,
                "patches\\psobb_boot_poster.png");
    // P3 video defaults — BAKED ON (the char-create intro is a shipped feature,
    // not a user tweak). Plays in-process via Media Foundation, skippable with
    // Enter, no watchdog cap (plays to EOF or skip). Asset ships in the install
    // at patches\video\ (relative; vid_resolve_path resolves vs the game dir).
    g_cfg.video_enabled          = 1;
    g_cfg.video_skippable        = 1;
    g_cfg.video_max_seconds      = 0;   // 0 = no watchdog: play to EOF or skip
    g_cfg.video_skip_debounce_ms = 500;
    g_cfg.video_audio            = 1;   // BAKED ON. Our MF->XAudio2 AAC
                                        // track plays with the intro cover. Engine
                                        // intro audio is suppressed in parallel by
                                        // the play-by-id gate (0x00828E4C) + STOP-ALL
                                        // (0x00829AD0) in mod_video.c.
                                        // Compile-time default; no INI key.
    g_cfg.video_diag             = 0;   // VideoDiag OFF by default
    g_cfg.video_trigger          = VID_TRIGGER_BOTH;  // boot meteor + char-create intro
    g_cfg.video_decoder          = VID_DECODER_MF;   // in-process MF by default
    _snprintf_s(g_cfg.video_path, MAX_PATH, _TRUNCATE,
                "patches\\video\\pso_intros_16x9.mp4");
    _snprintf_s(g_cfg.video_boot_path, MAX_PATH, _TRUNCATE,
                "patches\\video\\pso_bootseq_16x9.mp4");
    _snprintf_s(g_cfg.video_ffmpeg, MAX_PATH, _TRUNCATE,
                "patches\\video\\ffmpeg.exe");
    char exe[MAX_PATH] = {0};
    if (GetModuleFileNameA(NULL, exe, MAX_PATH) == 0) return;
    char *slash = strrchr(exe, '\\');
    if (!slash) return;
    *(slash + 1) = 0;
    // OUR ASI reads ONLY from `patches\pso_widescreen.ini` (next to the .asi),
    // NOT Sodaboy's widescreen.cfg: sharing a config file caused real confusion
    // (an `Enabled=0` line in widescreen.cfg looks like a global widescreen
    // kill-switch but only ever gated OUR ASI; Sodaboy's widescreen is gated
    // solely by the presence of Width / Height keys). Two ASIs, two config
    // files; no overlap.
    //
    // If `pso_widescreen.ini` is missing we keep the in-source
    // defaults set above (which are intentionally `Enabled=0` so a
    // bare install doesn't fight Sodaboy).
    char path[MAX_PATH];
    _snprintf_s(path, MAX_PATH, _TRUNCATE, "%spatches\\pso_widescreen.ini", exe);
    FILE *f = NULL; fopen_s(&f, path, "rb");
    if (!f) return;
    char line[256];
    // AUTO-ENABLE: track whether the INI carried an explicit
    // Enabled= line. If it did, that's a dev escape hatch and FORCE-overrides
    // the resolution-aspect auto-detect below (for A/B testing). If it didn't,
    // the resolution's aspect ratio is the SINGLE source of truth.
    int enabled_explicit = 0;
    // WINDOW MODE: track whether the INI carried an explicit
    // Windowed= line. If it did, the INI wins; otherwise the launcher's
    // widescreen.cfg Windowed= (read below) is the source of truth. Without
    // this, the launcher's "virtual widescreen" (Windowed=2) was ignored and
    // g_cfg.windowed stayed at its default of 1 -> the mod ran windowed.
    int windowed_explicit = 0;
    // ASPECT VIDEO: track whether the INI carried an explicit
    // VideoPath= line. If it did, the user's path wins and the aspect-based
    // intro-video auto-select below is skipped. If it didn't, the resolved
    // resolution's aspect picks 16:9 vs 4:3 (same threshold as enable).
    int video_path_explicit = 0;
    int video_boot_path_explicit = 0;
    while (fgets(line, sizeof(line), f)) {
        // Trim trailing \r\n + leading whitespace.
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == ';' || *p == 0 || *p == '\r' || *p == '\n')
            continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = p, *val = eq + 1;
        // Right-trim key + value.
        for (char *t = key + strlen(key); t > key && (t[-1]==' '||t[-1]=='\t'); --t) t[-1]=0;
        while (*val == ' ' || *val == '\t') ++val;
        for (char *t = val + strlen(val); t > val && (t[-1]==' '||t[-1]=='\t'||t[-1]=='\r'||t[-1]=='\n'); --t) t[-1]=0;
        // Enabled is OUR knob — Sodaboy's widescreen.cfg doesn't have it,
        // so a stock cfg leaves us off (no fight with his wrapper). Set
        // Enabled=1 explicitly when you've swapped to a different d3d8.dll
        // (e.g. d3d8to11 / native) and want this ASI to take over.
        if      (_stricmp(key, "Enabled")           == 0) { g_cfg.enabled = atoi(val); enabled_explicit = 1; }
        else if (_stricmp(key, "Windowed")          == 0) { g_cfg.windowed = atoi(val); windowed_explicit = 1; }
        else if (_stricmp(key, "Width")             == 0) g_cfg.width          = atoi(val);
        else if (_stricmp(key, "Height")            == 0) g_cfg.height         = atoi(val);
        else if (_stricmp(key, "Adapter")           == 0) g_cfg.adapter        = atoi(val);
        else if (_stricmp(key, "LogicalWidth")      == 0) {
            int v = atoi(val);
            if (v >= 320 && v <= 8192) g_cfg.logical_width = v;
        }
        else if (_stricmp(key, "LogicalHeight")     == 0) {
            int v = atoi(val);
            if (v >= 240 && v <= 8192) g_cfg.logical_height = v;
        }
        else if (_stricmp(key, "FovCorrection")     == 0) g_cfg.fov_correction      = atoi(val);
        else if (_stricmp(key, "OverrideBackBuffer")== 0) g_cfg.override_backbuffer = atoi(val);
        else if (_stricmp(key, "OverrideViewport")  == 0) g_cfg.override_viewport   = atoi(val);
        else if (_stricmp(key, "IntegerScale")      == 0) g_cfg.integer_scale       = atoi(val);
        else if (_stricmp(key, "PatchSplashPhase2") == 0) g_cfg.patch_splash_phase2 = atoi(val);
        else if (_stricmp(key, "HUDScale")          == 0) {
            float s = (float)atof(val);
            if (s > 0.1f && s < 10.0f) g_cfg.hud_scale = s;
        }
        else if (_stricmp(key, "PatchRuneScale")    == 0) g_cfg.patch_rune_scale = atoi(val);
        else if (_stricmp(key, "PatchCharSelect")   == 0) g_cfg.patch_charselect = atoi(val);
        else if (_stricmp(key, "PatchStreakScale")  == 0) g_cfg.patch_streak_scale = atoi(val);
        else if (_stricmp(key, "PatchMinimap")      == 0) g_cfg.patch_minimap = atoi(val);
        else if (_stricmp(key, "NoVignette")        == 0) g_cfg.no_vignette = atoi(val);
        else if (_stricmp(key, "MinimapSizeScale")  == 0) g_cfg.minimap_size_scale = (float)atof(val);
        else if (_stricmp(key, "MinimapCornerRightPx") == 0) g_cfg.minimap_corner_right_px = (float)atof(val);
        else if (_stricmp(key, "MinimapCornerTopPx")   == 0) g_cfg.minimap_corner_top_px   = (float)atof(val);
        else if (_stricmp(key, "CompassRightPx")       == 0) g_cfg.compass_right_px         = (float)atof(val);
        else if (_stricmp(key, "CompassBottomPx")      == 0) g_cfg.compass_bottom_px        = (float)atof(val);
        else if (_stricmp(key, "MinimapZoomEnable") == 0) g_cfg.patch_minimap_zoom = atoi(val);
        else if (_stricmp(key, "MinimapZoom")       == 0) {
            float z = (float)atof(val);
            if (z < 0.1f) z = 0.1f;     /* clamp [0.1,3.0]: smaller=zoomed out, larger=zoomed in */
            if (z > 3.0f) z = 3.0f;
            g_cfg.minimap_zoom = z;
        }
        else if (_stricmp(key, "GameAspect")        == 0) {
            // Allow override for unusual builds. Either bare float
            // ("1.333") or "W:H" form ("4:3", "16:10"). Default 16:9.
            float a = (float)atof(val);
            const char *colon = strchr(val, ':');
            if (colon) {
                float w = (float)atof(val);
                float h = (float)atof(colon + 1);
                if (w > 0 && h > 0) a = w / h;
            }
            if (a > 0.1f && a < 10.0f) g_cfg.game_aspect = a;
        }
        else if (_stricmp(key, "WidescreenEngine")  == 0) {
            // anzz1 is the only engine; this selector key is obsolete. Accept-and-warn
            // so existing INIs don't appear broken; the value is otherwise ignored.
            log_line("[pso_widescreen] WidescreenEngine='%s' is OBSOLETE; anzz1 is now the only engine (key ignored)", val);
        }
        else if (_stricmp(key, "DebugLogsEnabled")  == 0) g_cfg.debug_log = atoi(val);
        else if (_stricmp(key, "DebugLogVerbose")   == 0) g_cfg.debug_log_verbose = atoi(val);
        // Boot poster knobs — see [boot_poster] block in widescreen.cfg.sample.
        else if (_stricmp(key, "BootPosterEnabled")           == 0) g_cfg.boot_poster_enabled              = atoi(val);
        else if (_stricmp(key, "BootPosterPath")              == 0) {
            _snprintf_s(g_cfg.boot_poster_path, MAX_PATH, _TRUNCATE, "%s", val);
        }
        else if (_stricmp(key, "BootPosterMaxScreenPct")      == 0) {
            float p = (float)atof(val);
            if (p > 1.0f && p <= 100.0f) g_cfg.boot_poster_max_screen_pct = p;
        }
        else if (_stricmp(key, "BootPosterDisableAfterFloor") == 0) g_cfg.boot_poster_disable_after_floor  = atoi(val);
        else if (_stricmp(key, "BootPosterDisableAfterSplashAtlas") == 0) g_cfg.boot_poster_disable_after_splash = atoi(val);
        else if (_stricmp(key, "BootPosterMaxSeconds")        == 0) {
            int s = atoi(val);
            if (s >= 0 && s <= 600) g_cfg.boot_poster_max_seconds = s;
        }
        // P3 video knobs (Stage 1). VideoEnable=0 (default) keeps mod_video
        // dormant and the binary byte-identical-in-behavior to today.
        else if (_stricmp(key, "VideoEnable")  == 0) g_cfg.video_enabled   = atoi(val);
        else if (_stricmp(key, "VideoPath")    == 0) {
            _snprintf_s(g_cfg.video_path, MAX_PATH, _TRUNCATE, "%s", val);
            video_path_explicit = 1;
        }
        else if (_stricmp(key, "VideoBootPath") == 0) {
            _snprintf_s(g_cfg.video_boot_path, MAX_PATH, _TRUNCATE, "%s", val);
            video_boot_path_explicit = 1;
        }
        else if (_stricmp(key, "VideoSkippable") == 0) g_cfg.video_skippable = atoi(val);
        else if (_stricmp(key, "VideoFfmpeg")  == 0) {
            _snprintf_s(g_cfg.video_ffmpeg, MAX_PATH, _TRUNCATE, "%s", val);
        }
        // video_audio is a compile-time default (BAKED ON in load_config) — our
        // XAudio2 AAC track always plays with the intro cover and the engine intro
        // audio is muted in parallel. No INI knob.
        else if (_stricmp(key, "VideoMaxSeconds") == 0) {
            int s = atoi(val);
            if (s >= 0 && s <= 600) g_cfg.video_max_seconds = s;
        }
        else if (_stricmp(key, "VideoSkipDebounceMs") == 0) {
            int d = atoi(val);
            if (d >= 0 && d <= 10000) g_cfg.video_skip_debounce_ms = d;
        }
        else if (_stricmp(key, "VideoDiag")   == 0) g_cfg.video_diag = atoi(val);
        else if (_stricmp(key, "VideoTrigger") == 0) {
            // String-valued: which event auto-starts playback.
            // off        — never (default)
            // boot       — once at boot/title (covers the boot splash)
            // charcreate — when the char-create scene becomes active (covers
            // the scripted starfield intro), per Stage 2.
            if      (_stricmp(val, "charcreate") == 0) g_cfg.video_trigger = VID_TRIGGER_CHARCREATE;
            else if (_stricmp(val, "boot")       == 0) g_cfg.video_trigger = VID_TRIGGER_BOOT;
            else if (_stricmp(val, "both")       == 0) g_cfg.video_trigger = VID_TRIGGER_BOTH;
            else                                       g_cfg.video_trigger = VID_TRIGGER_OFF;
        }
        else if (_stricmp(key, "VideoDecoder") == 0) {
            // String-valued: which decode backend produces frames.
            // mf                 — in-process Media Foundation (default)
            // ffmpeg | external  — external ffmpeg.exe -> rawvideo BGRA pipe
            g_cfg.video_decoder =
                (_stricmp(val, "ffmpeg") == 0 || _stricmp(val, "external") == 0)
                    ? VID_DECODER_FFMPEG : VID_DECODER_MF;
        }
        // Sodaboy keys we deliberately ignore: MSAA / SMAA / SSAO /
        // CelShader / DOF / HDR / HUDScale. ReShade owns post-FX now
        // and HUDScale needs an engine-side hook (TODO follow-up).
    }
    fclose(f);
    // The INI (Enabled + HUDScale) is the SINGLE source of truth for scale — no
    // environment variable overrides it. Harness A/B scale testing edits the INI.
    //
    // RESOLUTION SOURCE = launcher's widescreen.cfg. The launcher
    // writes the player's chosen resolution to widescreen.cfg in the exe ROOT
    // dir (NOT patches\). Read it as the PRIMARY resolution source when the INI
    // didn't pin Width/Height. Precedence: INI Width/Height > widescreen.cfg >
    // monitor (GetSystemMetrics, below). This feeds the aspect auto-detect.
    // also read the launcher's WINDOW MODE here. The launcher writes the player's
    // chosen mode (0=fullscreen, 1=windowed, 2=virtual/borderless fullscreen —
    // Sodaboy convention, identical to ours; see g_cfg.windowed doc) into
    // widescreen.cfg next to Width/Height. Read the cfg UNCONDITIONALLY (not only
    // when Width/Height are unset) so the Windowed value is always picked up; an
    // explicit Windowed= / Width= / Height= in pso_widescreen.ini still wins
    // (windowed_explicit / nonzero g_cfg.width/height).
    {
        char cfgpath[MAX_PATH];
        _snprintf_s(cfgpath, MAX_PATH, _TRUNCATE, "%swidescreen.cfg", exe);
        FILE *cf = NULL; fopen_s(&cf, cfgpath, "rb");
        if (cf) {
            int cw = 0, ch = 0, cwin = -1, cadapter = -1;
            float chud = -1.0f;   // launcher's HUDScale (primary; overrides the ini fallback)
            char cline[256];
            while (fgets(cline, sizeof(cline), cf)) {
                char *q = cline;
                while (*q == ' ' || *q == '\t') ++q;
                if (*q == '#' || *q == ';' || *q == 0 || *q == '\r' || *q == '\n')
                    continue;
                char *ceq = strchr(q, '=');
                if (!ceq) continue;
                *ceq = 0;
                char *ckey = q, *cval = ceq + 1;
                for (char *t = ckey + strlen(ckey); t > ckey && (t[-1]==' '||t[-1]=='\t'); --t) t[-1]=0;
                while (*cval == ' ' || *cval == '\t') ++cval;
                for (char *t = cval + strlen(cval); t > cval && (t[-1]==' '||t[-1]=='\t'||t[-1]=='\r'||t[-1]=='\n'); --t) t[-1]=0;
                if      (_stricmp(ckey, "Width")    == 0) cw   = atoi(cval);
                else if (_stricmp(ckey, "Height")   == 0) ch   = atoi(cval);
                else if (_stricmp(ckey, "Windowed") == 0) cwin = atoi(cval);
                else if (_stricmp(ckey, "HUDScale") == 0) chud = (float)atof(cval);
                else if (_stricmp(ckey, "Adapter")  == 0) cadapter = atoi(cval);
            }
            fclose(cf);
            // Resolution: launcher cfg fills it only if the INI didn't pin it.
            if ((g_cfg.width <= 0 || g_cfg.height <= 0) && cw >= 320 && ch >= 240) {
                g_cfg.width = cw; g_cfg.height = ch;
            }
            // Adapter (GPU): launcher cfg is PRIMARY, same convention as
            // HUDScale - it overrides the ini. Only the sign is checked here;
            // the ordinal is validated against the real adapter count in
            // Hook_CreateDevice, the only place that number is knowable.
            if (cadapter >= 0) {
                g_cfg.adapter = cadapter;
                log_line("[pso_widescreen] adapter from widescreen.cfg: "
                         "Adapter=%d (launcher PRIMARY; overrides ini)", cadapter);
            }
            // Window mode: launcher cfg is the source of truth unless the INI
            // set Windowed explicitly. Range-checked to our 0/1/2 semantics.
            if (!windowed_explicit && cwin >= 0 && cwin <= 2) {
                g_cfg.windowed = cwin;
                log_line("[pso_widescreen] window mode from widescreen.cfg: Windowed=%d "
                         "(0=fullscreen,1=windowed,2=virtual/borderless)", cwin);
            }
            // HUDScale: the launcher's widescreen.cfg is the PRIMARY scale source (the
            // player's chosen HudScale, written by the psobb.io launcher). It OVERRIDES
            // the pso_widescreen.ini HUDScale, which is now only the FALLBACK used when
            // the launcher didn't write one. (Read AFTER the ini parse above, so the cfg
            // value wins when present.)
            if (chud > 0.1f && chud < 10.0f) {
                g_cfg.hud_scale = chud;
                log_line("[pso_widescreen] HUDScale from widescreen.cfg: %.4f (launcher PRIMARY; overrides ini)", chud);
            }
        }
    }
    // Resolution AUTO-DETECT: the shipped INI carries NO Width/Height (only
    // Enabled + HUDScale), so default them to the primary monitor's resolution.
    // Without this, width/height stay 0 and the render_w fallback (=(int)A=853,
    // the design width) breaks the widescreen render path / hangs the device.
    // An explicit Width/Height in the INI (dev override) still wins; the
    // widescreen.cfg read above already filled them if the launcher set them.
    // For borderless-fullscreen (Windowed=2), render at the ACTUAL desktop
    // resolution — super-resolution / DSR aware — and FORCE the backbuffer to
    // it, so the game renders natively at whatever the desktop is set to (1440p,
    // 4K, ...) instead of a stale widescreen.cfg value the launcher wrote. The
    // render res, the 2D affine (render_w/design_w) and the engine resolution
    // table all derive from this, so the whole layout follows automatically.
    if (g_cfg.windowed == 2) {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        if (sw >= 320 && sh >= 240) {
            g_cfg.width  = sw;  g_cfg.height = sh;
            g_cfg.logical_width = sw;  g_cfg.logical_height = sh;
            g_cfg.override_backbuffer = 1;   // CreateDevice forces BB = desktop res
        }
    }
    // Fallback for other window modes / unset: default to the primary monitor.
    if (g_cfg.width <= 0 || g_cfg.height <= 0) {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        if (sw >= 320 && sh >= 240) { g_cfg.width = sw; g_cfg.height = sh; }
    }
    // Auto-derive game aspect from Width/Height. Without this, the default
    // 4:3 game_aspect means the anzz1 widescreen path computes 640-wide
    // engine coords for a 1920-wide backbuffer -> 3.0x horizontal stretch
    // instead of the correct 2.25x. INI GameAspect (if set above) wins.
    if (g_cfg.width > 0 && g_cfg.height > 0) {
        float derived = (float)g_cfg.width / (float)g_cfg.height;
        // Only auto-set when GameAspect wasn't explicitly provided. The
        // default 4/3 is the sentinel; anything else means user set it.
        const float kStockAspect = 4.0f / 3.0f;
        if (g_cfg.game_aspect > kStockAspect - 0.001f
            && g_cfg.game_aspect < kStockAspect + 0.001f
            && derived > 0.1f && derived < 10.0f) {
            g_cfg.game_aspect = derived;
        }
    }
    // ASPECT AUTO-ENABLE: the resolution's aspect
    // ratio decides whether widescreen applies. 4:3 (or narrower) -> stock
    // client (OFF); anything wider than 4:3 (16:10, 16:9, 21:9, 32:9, ...) ->
    // widescreen (ON). The +0.01 epsilon keeps true 4:3 modes OFF while letting
    // 16:10 (1.6) and up turn ON. An explicit Enabled= in the INI force-
    // overrides this (dev A/B escape hatch).
    if (g_cfg.width > 0 && g_cfg.height > 0) {
        float ar = (float)g_cfg.width / (float)g_cfg.height;
        int auto_on = (ar > 4.0f / 3.0f + 0.01f) ? 1 : 0;
        if (!enabled_explicit) g_cfg.enabled = auto_on;
        if (enabled_explicit) {
            log_line("[pso_widescreen] aspect auto-detect: %dx%d AR=%.4f -> widescreen %s (ini Enabled=%d overrides -> %s)",
                     g_cfg.width, g_cfg.height, ar, auto_on ? "ON" : "OFF",
                     g_cfg.enabled, g_cfg.enabled ? "ON" : "OFF");
        } else {
            log_line("[pso_widescreen] aspect auto-detect: %dx%d AR=%.4f -> widescreen %s",
                     g_cfg.width, g_cfg.height, ar, auto_on ? "ON" : "OFF");
        }
    }
    // INTEGER-SCALE ("bump the base render"): on a high-DPI borderless
    // screen, render the backbuffer at a CLEAN integer divisor of the physical
    // Width/Height (capped ~1080p) so the d3d8 present upscales by a clean integer
    // factor. WHY: a 1280x800 (16:10) backbuffer stretched to a 4K (16:9) window is a
    // 3.0x/2.7x non-integer + aspect-mismatched blit — the Wine/wined3d perf cliff
    // (~20 FPS) the tester hit without dgVoodoo. Rendering 1920x1080 (= 4K/2) and
    // presenting a clean 2x is far faster (and sharper). N = ceil(height/1440) keeps
    // the render in (720..1440]; 4K->2 (1920x1080), 5K->2 (1440), 8K->3 (1440),
    // <=1440p->1 (native, no-op). Sets logical=physical/N and forces the engine-native
    // render path; ws_compute_scale's render_w follows logical so the engine res table
    // matches the real BB (no "2x too big at 4K" coord bug).
    if (g_cfg.integer_scale && g_cfg.windowed == 2 &&
        g_cfg.width >= 1280 && g_cfg.height >= 720) {
        int N = (g_cfg.height + 1439) / 1440;          // ceil(height / 1440)
        if (N < 1) N = 1;
        if (N > 1) {                                   // <=1440p: leave native (N==1 no-op)
            g_cfg.logical_width       = g_cfg.width  / N;
            g_cfg.logical_height      = g_cfg.height / N;
            g_cfg.override_backbuffer = 1;
            g_cfg.override_viewport   = 1;
            log_line("[pso_widescreen] integer-scale: physical %dx%d -> render %dx%d "
                     "(clean %dx present upscale)",
                     g_cfg.width, g_cfg.height,
                     g_cfg.logical_width, g_cfg.logical_height, N);
        } else {
            log_line("[pso_widescreen] integer-scale: physical %dx%d <=1440p -> native (no-op)",
                     g_cfg.width, g_cfg.height);
        }
    }
    // ASPECT VIDEO SELECT: pick the aspect-appropriate intro video
    // from the SAME resolved resolution the enable auto-detect used, so the two
    // decisions always agree (16:9 -> widescreen ON + 16:9 video; 4:3 ->
    // widescreen OFF + 4:3 video). An explicit VideoPath= in the INI wins.
    if (!video_path_explicit && g_cfg.width > 0 && g_cfg.height > 0) {
        float ar = (float)g_cfg.width / (float)g_cfg.height;
        if (ar > 4.0f / 3.0f + 0.01f)
            _snprintf_s(g_cfg.video_path, MAX_PATH, _TRUNCATE, "patches\\video\\pso_intros_16x9.mp4");
        else
            _snprintf_s(g_cfg.video_path, MAX_PATH, _TRUNCATE, "patches\\video\\pso_intros_4x3.mp4");
        log_line("[pso_widescreen] intro video: %s (AR=%.4f)", g_cfg.video_path, (double)ar);
    }
    // BOOT video aspect-select (same threshold), for trigger=both / boot.
    if (!video_boot_path_explicit && g_cfg.width > 0 && g_cfg.height > 0) {
        float ar = (float)g_cfg.width / (float)g_cfg.height;
        if (ar > 4.0f / 3.0f + 0.01f)
            _snprintf_s(g_cfg.video_boot_path, MAX_PATH, _TRUNCATE, "patches\\video\\pso_bootseq_16x9.mp4");
        else
            _snprintf_s(g_cfg.video_boot_path, MAX_PATH, _TRUNCATE, "patches\\video\\pso_bootseq_4x3.mp4");
        log_line("[pso_widescreen] boot video: %s (AR=%.4f)", g_cfg.video_boot_path, (double)ar);
    }
    // ---- io client's psobb.cfg: honor SkipIntro for OUR boot video ----
    // The io launcher writes psobb.cfg next to psobb.exe (same dir as
    // widescreen.cfg above). It's plain Key=Value, no sections. We read it
    // best-effort: if absent/unreadable we leave OUR trigger untouched.
    //   * SkipIntro=1 corresponds to the BB Patch Project PATCH_SKIP_INTRO_CREDITS
    //     (*(u8*)0x007A645E=2) which skips the NATIVE intro. We mirror that user
    //     intent for OUR custom boot meteor video: drop ONLY the boot leg. The
    //     char-create intro STILL plays (owner explicitly wants it).
    //   * DisableIME is parsed for awareness ONLY — our born-borderless window
    //     keeps the IME context attached without touching the IME enable state,
    //     so no behavior change is needed here (it's already compatible).
    // This runs AFTER video_trigger is finalized (compile default OR ini), so
    // the SkipIntro override applies cleanly regardless of how it was set.
    {
        int skip_intro = 0, disable_ime = 0;
        int have_skip_intro = 0, have_disable_ime = 0;
        char cfgpath[MAX_PATH];
        _snprintf_s(cfgpath, MAX_PATH, _TRUNCATE, "%spsobb.cfg", exe);
        FILE *pf = NULL; fopen_s(&pf, cfgpath, "rb");
        if (pf) {
            char cline[256];
            while (fgets(cline, sizeof(cline), pf)) {
                char *q = cline;
                while (*q == ' ' || *q == '\t') ++q;
                if (*q == '#' || *q == ';' || *q == 0 || *q == '\r' || *q == '\n')
                    continue;
                char *ceq = strchr(q, '=');
                if (!ceq) continue;
                *ceq = 0;
                char *ckey = q, *cval = ceq + 1;
                for (char *t = ckey + strlen(ckey); t > ckey && (t[-1]==' '||t[-1]=='\t'); --t) t[-1]=0;
                while (*cval == ' ' || *cval == '\t') ++cval;
                for (char *t = cval + strlen(cval); t > cval && (t[-1]==' '||t[-1]=='\t'||t[-1]=='\r'||t[-1]=='\n'); --t) t[-1]=0;
                if      (_stricmp(ckey, "SkipIntro")  == 0) { skip_intro  = atoi(cval); have_skip_intro  = 1; }
                else if (_stricmp(ckey, "DisableIME") == 0) { disable_ime = atoi(cval); have_disable_ime = 1; }
            }
            fclose(pf);
            // DisableIME: log-only (our borderless window is IME-compatible).
            if (have_disable_ime)
                log_line("[pso_widescreen] psobb.cfg: DisableIME=%d (no behavior change; "
                         "borderless window keeps IME context attached)", disable_ime);
            // SkipIntro: drop ONLY the boot leg. BOTH->CHARCREATE, BOOT->OFF;
            // CHARCREATE/OFF unchanged (no boot leg to drop).
            if (have_skip_intro && skip_intro == 1) {
                int prev = g_cfg.video_trigger;
                if (g_cfg.video_trigger == VID_TRIGGER_BOTH) {
                    g_cfg.video_trigger = VID_TRIGGER_CHARCREATE;
                    log_line("[pso_widescreen] psobb.cfg: SkipIntro=1 -> boot video gated off "
                             "(trigger BOTH->CHARCREATE; char-create intro still plays)");
                } else if (g_cfg.video_trigger == VID_TRIGGER_BOOT) {
                    g_cfg.video_trigger = VID_TRIGGER_OFF;
                    log_line("[pso_widescreen] psobb.cfg: SkipIntro=1 -> boot video gated off "
                             "(trigger BOOT->OFF)");
                } else {
                    log_line("[pso_widescreen] psobb.cfg: SkipIntro=1 -> no boot leg to drop "
                             "(trigger %d unchanged)", prev);
                }
            } else {
                log_line("[pso_widescreen] psobb.cfg: SkipIntro=%d -> boot video kept "
                         "(trigger %d unchanged)",
                         have_skip_intro ? skip_intro : 0, g_cfg.video_trigger);
            }
        } else {
            log_line("[pso_widescreen] psobb.cfg: not found at %s -> boot video kept "
                     "(trigger %d unchanged)", cfgpath, g_cfg.video_trigger);
        }
    }
    // Record which cfg file we ended up reading so the log makes the
    // path-resolution explicit (helps users diagnose "why aren't my
    // knobs taking effect" when both files exist).
    log_line("[pso_widescreen] cfg loaded from: %s", path);

    // ---- THE ONE SCALING GATE: compute g_scale ONCE, here. ----
    ws_compute_scale(&g_scale);
}

// ws_compute_scale — derive the single ws_scale_ctx from g_cfg, ONCE, at the
// end of load_config. The anzz1 A/B/C/D extents AND the render_w/render_h
// derivation are computed HERE so the magnitude lives in exactly one place; the
// resolved values are then handed to apply_anzz1_widescreen. front-end-native is
// the VALUE hud_scale==1.0 — no branch.
static void ws_compute_scale(ws_scale_ctx *s)
{
    s->enabled      = g_cfg.enabled;
    s->hud_scale    = g_cfg.hud_scale;
    s->game_aspect  = g_cfg.game_aspect;

    const float AR = g_cfg.game_aspect;
    float A = (AR / (4.0f / 3.0f)) * 640.0f;
    float B = (AR / (4.0f / 3.0f)) * 128.0f;
    float C = 480.0f;
    DWORD D = 128;
    A *= g_cfg.hud_scale;
    B *= g_cfg.hud_scale;
    C *= g_cfg.hud_scale;
    D = (DWORD)((float)D * g_cfg.hud_scale);
    s->A = A; s->B = B; s->C = C; s->D = D;

    // 6-entry resolution table = engine RENDER resolution (must equal the D3D
    // backbuffer the CreateDevice hook sets). In override_backbuffer mode the BB is the
    // LOGICAL canvas (e.g. integer-scale's physical/N = 1920x1080 @ 4K); in stretch mode
    // the BB is the engine-native backbuffer (== physical Width/Height). Pick the one
    // that matches the actual BB, else the res table is sized wrong (coords 2x too big at
    // 4K). Fall back to the A/C coordinate extents only if unset (same rule as anzz1).
    const int rw_src = (g_cfg.override_backbuffer && g_cfg.logical_width  > 0)
                       ? g_cfg.logical_width  : g_cfg.width;
    const int rh_src = (g_cfg.override_backbuffer && g_cfg.logical_height > 0)
                       ? g_cfg.logical_height : g_cfg.height;
    s->render_w = (rw_src > 0) ? rw_src : (int)A;
    s->render_h = (rh_src > 0) ? rh_src : (int)C;

    log_line("[pso_widescreen] ws_compute_scale: enabled=%d aspect=%.5f hud_scale=%.4f "
             "-> A=%.2f B=%.2f C=%.2f D=%lu render=%dx%d",
             s->enabled, s->game_aspect, s->hud_scale,
             s->A, s->B, s->C, (unsigned long)s->D, s->render_w, s->render_h);
}

// ---- Hook plumbing ----

// IDirect3DDevice8::Present — slot 15. We intercept it to call the
// boot-poster overlay BEFORE the engine flips the backbuffer, so the
// custom poster lands in the same frame as the engine's "NOW LOADING"
// black screen. Once the poster auto-disables (splash atlas appears,
// floor changes, MaxSeconds elapse), this hook becomes a near no-op.
typedef HRESULT (STDMETHODCALLTYPE *Present_t)(
    IDirect3DDevice8 *self, const RECT *pSrc, const RECT *pDst,
    HWND hOverride, void *pDirtyRegion);

// IDirect3DDevice8::Reset — slot 14 (the slot immediately before Present=15 in
// the canonical d3d8 IDirect3DDevice8 vtable; consistent with this file's
// "slot 15 = Present"). PSOBB recreates the device's swap chain on a
// resolution / fullscreen change — and, critically, dgVoodoo2 issues a Reset on
// alt-tab in virtual fullscreen (device-lost recovery). A Reset INVALIDATES
// every D3DPOOL_DEFAULT resource and every device-state object (state blocks),
// so our overlay modules must drop their device-bound resources before it and
// recreate them after. Signature mirrors CreateDevice's pp pointer type.
typedef HRESULT (STDMETHODCALLTYPE *Reset_t)(
    IDirect3DDevice8 *self, D3DPRESENT_PARAMETERS_X *pPresentationParameters);

// IDirect3DDevice8::TestCooperativeLevel — slot 3 (canonical: after
// QueryInterface/AddRef/Release at 0..2). Returns D3D_OK when the device is
// usable, D3DERR_DEVICELOST while it is lost (cannot render — e.g. mid alt-tab),
// or D3DERR_DEVICENOTRESET once it is recoverable but Reset has not run yet. We
// poll it in Hook_Present to skip our overlay blits on a non-usable device.
typedef HRESULT (STDMETHODCALLTYPE *TestCooperativeLevel_t)(
    IDirect3DDevice8 *self);

// d3d8 device-lost HRESULTs (D3DERR_* from d3d8.h; stable since DX8).
#ifndef D3DERR_DEVICELOST
#define D3DERR_DEVICELOST     0x88760868u
#endif
#ifndef D3DERR_DEVICENOTRESET
#define D3DERR_DEVICENOTRESET 0x88760869u
#endif

static Direct3DCreate8_t real_Direct3DCreate8 = NULL;
static CreateDevice_t    real_CreateDevice    = NULL;
static SetTransform_t    real_SetTransform    = NULL;
static SetViewport_t     real_SetViewport     = NULL;
static Present_t         real_Present         = NULL;
static Reset_t           real_Reset           = NULL;
static int               g_last_vp_w          = 0;  // last SetViewport-observed dims
static int               g_last_vp_h          = 0;
static int               g_vtable_patched     = 0;
static int               g_device_patched     = 0;
static int               g_logged_first_vp    = 0;
static int               g_logged_present_geom = 0;  // black-quadrant diag one-shot

// Borderless-windowed style normalisation: PSOBB's window starts as a
// regular WS_OVERLAPPEDWINDOW. To get borderless we strip the caption /
// thick-frame / system-menu bits and let the OS treat it as a popup.
// Called once on the first CreateDevice when Windowed=2.
static void make_borderless(HWND hwnd, int w, int h)
{
    if (!hwnd) return;
    // WS_CHILD GUARD: when the harness/editor reparents the game as a
    // WS_CHILD into its own pane, the host owns the window geometry — a child must
    // NEVER strip its caption or resize/maximize itself. Bail so the embedded A/B
    // path is unaffected (it also forces Windowed=1, so this is belt-and-braces).
    if (GetWindowLongPtrA(hwnd, GWL_STYLE) & WS_CHILD) {
        log_line("[pso_widescreen] make_borderless: window is WS_CHILD (embedded) — skipping borderless");
        return;
    }
    LONG_PTR style = GetWindowLongPtrA(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
               WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME);
    style |= WS_POPUP;
    SetWindowLongPtrA(hwnd, GWL_STYLE, style);
    LONG_PTR ex = GetWindowLongPtrA(hwnd, GWL_EXSTYLE);
    ex &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
    SetWindowLongPtrA(hwnd, GWL_EXSTYLE, ex);
    // MONITOR-AWARE FULL-BLEED: borderless must cover the monitor the
    // game window is ACTUALLY on — the owner's virtual widescreen display — NOT
    // the Windows PRIMARY monitor. The old SM_CXSCREEN/CYSCREEN path centered on
    // primary, so on a multi-mon setup (physical 4:3 primary + virtual widescreen)
    // the window landed on the wrong screen. Query the window's nearest monitor and
    // fill its full rect (true borderless fullscreen, like Trinity's strip-caption
    // + maximize). Fall back to the primary-centered w x h path if the query fails.
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoA(mon, &mi)) {
        int mx = mi.rcMonitor.left;
        int my = mi.rcMonitor.top;
        int mw = mi.rcMonitor.right  - mi.rcMonitor.left;
        int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        SetWindowPos(hwnd, NULL, mx, my, mw, mh,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
        log_line("[pso_widescreen] make_borderless: full-bleed on monitor rect "
                 "%d,%d %dx%d (window's own display)", mx, my, mw, mh);
        return;
    }
    // Fallback: center the requested w x h on the primary monitor.
    if (w > 0 && h > 0) {
        RECT scr; scr.left = scr.top = 0;
        scr.right  = GetSystemMetrics(SM_CXSCREEN);
        scr.bottom = GetSystemMetrics(SM_CYSCREEN);
        int x = (scr.right  - w) / 2; if (x < 0) x = 0;
        int y = (scr.bottom - h) / 2; if (y < 0) y = 0;
        SetWindowPos(hwnd, NULL, x, y, w, h,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
        log_line("[pso_widescreen] make_borderless: monitor query failed — "
                 "centered %dx%d on primary (fallback)", w, h);
    } else {
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_NOMOVE | SWP_NOSIZE);
    }
}

// Hor+ FOV correction: scale m._11 of the projection matrix by
// game_aspect / display_aspect. The engine builds its projection
// assuming a 4:3 frustum; multiplying the X term tightens the
// horizontal frustum so a wider backbuffer shows MORE world
// horizontally instead of horizontally-stretching the same FOV.
//
// Math:
// m._11_eng = cot(fovY/2) / game_aspect      (engine writes this)
// m._11_we_want = cot(fovY/2) / display_aspect
// factor = (game_aspect / display_aspect)    (multiply by this)
//
// At 4:3 → 16:9 (1.778) factor = 0.75; the X frustum gets ~33% wider.
// At 4:3 → 21:9 (2.333) factor ≈ 0.571; same idea, more aggressive.

static HRESULT STDMETHODCALLTYPE Hook_SetTransform(
    IDirect3DDevice8 *self, DWORD State, const D3DMATRIX_X *pMatrix)
{
    if (g_cfg.fov_correction && State == D3DTS_PROJECTION && pMatrix &&
        g_cfg.width > 0 && g_cfg.height > 0) {
        const float disp_aspect =
            (float)g_cfg.width / (float)g_cfg.height;
        if (disp_aspect > 0.0f) {
            const float factor = g_cfg.game_aspect / disp_aspect;
            D3DMATRIX_X corrected = *pMatrix;
            corrected._11 *= factor;
            return real_SetTransform(self, State, &corrected);
        }
    }
    return real_SetTransform(self, State, pMatrix);
}

// Force the engine's draws to fill the full backbuffer. PSOBB explicitly
// SetViewports its launcher-selected resolution after CreateDevice; if
// we let that through verbatim, the engine renders into a (e.g.) 1024x768
// rectangle in the upper-left of our 4K backbuffer. Rewriting the
// viewport to match the configured backbuffer fixes the corner-render
// without touching any engine code. MinZ/MaxZ pass through.
static HRESULT STDMETHODCALLTYPE Hook_SetViewport(
    IDirect3DDevice8 *self, const D3DVIEWPORT8_X *pViewport)
{
    if (g_cfg.override_viewport && pViewport &&
        g_cfg.width > 0 && g_cfg.height > 0) {
        // Viewport sized to LOGICAL backbuffer (matches CreateDevice's BB
        // override . Engine RHW coords land 1:1 on the BB pixels.
        //
        // ONLY override if the incoming viewport is clearly the "main scene"
        // — i.e. close to full BB size. The engine also calls SetViewport with
        // small rects for sub-renders (minimap content, HUD micro-viewports,
        // render targets); an unconditional override clobbers those, rendering
        // the minimap content at 1920x1080 instead of its tiny corner rect.
        // Heuristic: require incoming Width >= 60% of logical BB to be a
        // "main scene" viewport.
        const int lw = g_cfg.logical_width  > 0 ? g_cfg.logical_width  : 1920;
        const int lh = g_cfg.logical_height > 0 ? g_cfg.logical_height : 1080;
        const DWORD threshold_w = (DWORD)(lw * 6 / 10);
        const DWORD threshold_h = (DWORD)(lh * 6 / 10);
        if (pViewport->Width < threshold_w || pViewport->Height < threshold_h) {
            // Sub-render viewport (minimap, HUD piece, RT) — pass through.
            g_last_vp_w = (int)pViewport->Width;
            g_last_vp_h = (int)pViewport->Height;
            return real_SetViewport(self, pViewport);
        }
        D3DVIEWPORT8_X vp = *pViewport;
        const DWORD orig_w = vp.Width;
        const DWORD orig_h = vp.Height;
        vp.X = 0;
        vp.Y = 0;
        vp.Width  = (DWORD)lw;
        vp.Height = (DWORD)lh;
        if (!g_logged_first_vp) {
            g_logged_first_vp = 1;
            log_line("[pso_widescreen] SetViewport override: %lux%lu -> %lux%lu (logical; threshold=%lux%lu)",
                     (unsigned long)orig_w, (unsigned long)orig_h,
                     (unsigned long)vp.Width, (unsigned long)vp.Height,
                     (unsigned long)threshold_w, (unsigned long)threshold_h);
        }
        // Cache for boot poster (Hook_Present uses these dims to size the
        // centered overlay rect).
        g_last_vp_w = (int)vp.Width;
        g_last_vp_h = (int)vp.Height;
        return real_SetViewport(self, &vp);
    }
    if (pViewport) {
        g_last_vp_w = (int)pViewport->Width;
        g_last_vp_h = (int)pViewport->Height;
    }
    return real_SetViewport(self, pViewport);
}

// In-game re-assert, fired once per FE->IG transition (defined below).
static void ingame_reassert_on_transition(void);

// Char-select SCALES with HudScale exactly like Ephinea (global affine
// 0x00ACC0E8 = 2.25/hud_scale, design_w 0x0098A4B8 = 853.333*hud_scale) — NO pin,
// NO boot design_w/h capture. patch_charselect_vertical() bakes the Ephinea-exact
// hs-scaled layout.

// Returns 1 if the device is currently in a usable (renderable) state, 0 if it
// is lost / not-yet-reset (mid alt-tab under dgVoodoo2). We poll
// TestCooperativeLevel (slot 3) so our overlay blits never run against a lost
// device. SEH-firewalled and fail-OPEN: if the call itself faults or
// real_Present hasn't been captured yet we assume usable (1) — the engine's own
// Present + the overlay modules' internal SEH still backstop us, and we must
// never wedge the normal-case overlay just because a wrapper returned something
// unexpected from TestCooperativeLevel.
static int device_is_usable(IDirect3DDevice8 *self)
{
    if (!self) return 0;
    int usable = 1;
    __try {
        void **vt = *(void ***)self;
        TestCooperativeLevel_t fnTCL = (TestCooperativeLevel_t)vt[3];
        HRESULT hr = fnTCL(self);
        if ((DWORD)hr == D3DERR_DEVICELOST || (DWORD)hr == D3DERR_DEVICENOTRESET)
            usable = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        usable = 1;   // fail open
    }
    return usable;
}

// IDirect3DDevice8::Reset (slot 14) — the missing lost-device hook. PSOBB (and
// dgVoodoo2 on alt-tab in virtual fullscreen) Resets the device on a
// resolution / fullscreen change; a Reset invalidates every D3DPOOL_DEFAULT
// resource and every device-state object. Discipline:
//   1. BEFORE forwarding, tell each overlay module to drop its device-bound
//      resources (state block, frame texture) via *_on_device_lost().
//   2. Forward to the real Reset.
//   3. On SUCCESS, let each module know it can recreate (lazily) via
//      *_on_device_reset(). On FAILURE (device still lost — e.g. still
//      minimized) we leave resources released; the next successful Reset (or
//      lazy recreate on the next usable Present) brings them back.
// SEH-firewalled the same way as the other device hooks: a wrapper quirk here
// must never poison the engine's device recovery.
static HRESULT STDMETHODCALLTYPE Hook_Reset(
    IDirect3DDevice8 *self, D3DPRESENT_PARAMETERS_X *pp)
{
    log_line("[pso_widescreen] Reset: releasing overlay device resources "
             "(BB %ux%u, windowed=%d)",
             pp ? (unsigned)pp->BackBufferWidth  : 0u,
             pp ? (unsigned)pp->BackBufferHeight : 0u,
             pp ? (int)pp->Windowed : -1);
    __try {
        boot_poster_on_device_lost();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try {
        mod_video_on_device_lost();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    HRESULT hr = real_Reset ? real_Reset(self, pp) : E_FAIL;

    if (SUCCEEDED(hr)) {
        log_line("[pso_widescreen] Reset OK — overlay resources will recreate lazily");
        __try {
            boot_poster_on_device_reset();
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        __try {
            mod_video_on_device_reset();
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    } else {
        log_line("[pso_widescreen] Reset returned 0x%08lx — device still lost; "
                 "leaving overlay resources released", (unsigned long)hr);
    }
    return hr;
}

// Present (slot 15) — render the boot poster ONTO the current backbuffer
// before the engine flips it. The boot_poster module checks its own
// auto-disable conditions on each call, so once any condition trips this
// hook becomes a single integer compare + branch.
static HRESULT STDMETHODCALLTYPE Hook_Present(
    IDirect3DDevice8 *self, const RECT *pSrc, const RECT *pDst,
    HWND hOverride, void *pDirtyRegion)
{
    // LOST-DEVICE GUARD: on alt-tab in virtual fullscreen, dgVoodoo2 drops the
    // device (TestCooperativeLevel -> D3DERR_DEVICELOST / DEVICENOTRESET) and
    // our overlay resources are released until the Reset hook recreates them.
    // Blitting against a lost device (or a just-released/null texture) is the
    // classic AV. Skip BOTH overlays while the device is not usable and pass
    // straight through to the engine's Present, which itself returns DEVICELOST
    // harmlessly. The overlays resume automatically once the device is reset.
    if (device_is_usable(self)) {
        // The boot cover is drawn by mod_video's boot leg (through
        // vid_draw_overlay's state capture/restore), not by the boot poster.
        // boot_poster_on_present is intentionally not called here; the deployed
        // INI also sets BootPosterEnabled=0. mod_boot_poster.c stays
        // compiled-but-unused.
        //
        // mod_video on_present: cheap `if(!enabled) return;` no-op when disabled,
        // so VideoEnable=0 (default) is byte-identical to no module. SEH-firewalled
        // (mod_video also wraps internally).
        __try {
            mod_video_on_present((void *)self, g_last_vp_w, g_last_vp_h);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    // black-quadrant diagnostic (one-shot): the reparent wall
    // renders each quadrant's content into a top-left sub-rect with black
    // right/bottom margins. Capture the actual present geometry — Present's
    // own src/dst rects, the engine window client rect, and the cached
    // viewport — so we can see WHICH of these is smaller than the window.
    if (g_cfg.debug_log && !g_logged_present_geom) {
        g_logged_present_geom = 1;
        __try {
            RECT cr = {0,0,0,0};
            // Engine HWND global (same VA the cursor-warp fix uses as
            // kEngineHwndVA, spelled literally here because that #define
            // lives later in the file).
            HWND hwnd = *(HWND volatile *)0x00ACBED8u;
            if (hwnd) GetClientRect(hwnd, &cr);
            char srcbuf[64] = "NULL", dstbuf[64] = "NULL";
            if (pSrc) _snprintf_s(srcbuf, sizeof(srcbuf), _TRUNCATE,
                                  "(%ld,%ld)-(%ld,%ld)", pSrc->left, pSrc->top,
                                  pSrc->right, pSrc->bottom);
            if (pDst) _snprintf_s(dstbuf, sizeof(dstbuf), _TRUNCATE,
                                  "(%ld,%ld)-(%ld,%ld)", pDst->left, pDst->top,
                                  pDst->right, pDst->bottom);
            log_line("[pso_widescreen] PRESENT-GEOM: client=%ldx%ld pSrc=%s "
                     "pDst=%s hOverride=%p vp=%dx%d cfg=%dx%d logical=%dx%d",
                     cr.right - cr.left, cr.bottom - cr.top, srcbuf, dstbuf,
                     (void *)hOverride, g_last_vp_w, g_last_vp_h,
                     g_cfg.width, g_cfg.height, g_cfg.logical_width,
                     g_cfg.logical_height);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    ingame_reassert_on_transition();   // edge-driven; no per-frame work
    // char-select scales with HudScale like Ephinea — no per-frame design-divisor
    // pin from Present.
    return real_Present(self, pSrc, pDst, hOverride, pDirtyRegion);
}

// Patch the device's vtable once the first device comes back from
// CreateDevice. The vtable is shared across all IDirect3DDevice8
// instances created by the same IDirect3D8 factory, so one patch
// covers the whole process. We patch:
// slot 14 = Reset                  (lost-device discipline: drop+recreate the
//                                   overlay's device-bound resources on alt-tab)
// slot 15 = Present                (boot poster pre-flip overlay)
// slot 37 = SetTransform           (FOV / aspect correction)
// slot 40 = SetViewport            (full-backbuffer override)
// slot 72 = DrawPrimitiveUP        (HUDScale on RHW 2D draws)
// slot 73 = DrawIndexedPrimitiveUP (HUDScale on RHW 2D draws)
// slot 76 = SetVertexShader        (FVF tracking for HUDScale)
static void patch_device_vtable(IDirect3DDevice8 *dev)
{
    if (!dev || g_device_patched) return;
    void **vt = *(void ***)dev;
    DWORD old_prot = 0;
    // Present hook — armed when the boot poster OR the P3 video player is
    // enabled in cfg, since those are the features that need a pre-flip
    // overlay. Guarding the install keeps wrapper-startup paths clean for
    // users who disable both.
    if (g_cfg.boot_poster_enabled || g_cfg.video_enabled) {
        if (VirtualProtect(&vt[15], sizeof(void*), PAGE_READWRITE, &old_prot)) {
            real_Present = (Present_t)vt[15];
            vt[15] = (void *)&Hook_Present;
            DWORD tmp = 0;
            VirtualProtect(&vt[15], sizeof(void*), old_prot, &tmp);
            log_line("[pso_widescreen] device vtable[15] patched: real Present=0x%p",
                     (void*)real_Present);
        } else {
            log_line("[pso_widescreen] device vtable[15] VirtualProtect FAILED err=%lu",
                     GetLastError());
        }
        // Reset (slot 14) — paired with the Present overlay lifecycle. Only the
        // overlay modules hold device-bound resources that a Reset invalidates,
        // so the Reset hook is armed under the same gate. Without this hook an
        // alt-tab under dgVoodoo2 Resets the device while our state block /
        // textures stay stale -> the AddRef/blit AVs in the wrapper.
        if (VirtualProtect(&vt[14], sizeof(void*), PAGE_READWRITE, &old_prot)) {
            real_Reset = (Reset_t)vt[14];
            vt[14] = (void *)&Hook_Reset;
            DWORD tmp = 0;
            VirtualProtect(&vt[14], sizeof(void*), old_prot, &tmp);
            log_line("[pso_widescreen] device vtable[14] patched: real Reset=0x%p",
                     (void*)real_Reset);
        } else {
            log_line("[pso_widescreen] device vtable[14] VirtualProtect FAILED err=%lu",
                     GetLastError());
        }
    }
    if (VirtualProtect(&vt[37], sizeof(void*), PAGE_READWRITE, &old_prot)) {
        real_SetTransform = (SetTransform_t)vt[37];
        vt[37] = (void *)&Hook_SetTransform;
        DWORD tmp = 0;
        VirtualProtect(&vt[37], sizeof(void*), old_prot, &tmp);
        log_line("[pso_widescreen] device vtable[37] patched: real SetTransform=0x%p",
                 (void*)real_SetTransform);
    } else {
        log_line("[pso_widescreen] device vtable[37] VirtualProtect FAILED err=%lu",
                 GetLastError());
    }
    if (VirtualProtect(&vt[40], sizeof(void*), PAGE_READWRITE, &old_prot)) {
        real_SetViewport = (SetViewport_t)vt[40];
        vt[40] = (void *)&Hook_SetViewport;
        DWORD tmp = 0;
        VirtualProtect(&vt[40], sizeof(void*), old_prot, &tmp);
        log_line("[pso_widescreen] device vtable[40] patched: real SetViewport=0x%p",
                 (void*)real_SetViewport);
    } else {
        log_line("[pso_widescreen] device vtable[40] VirtualProtect FAILED err=%lu",
                 GetLastError());
    }
    g_device_patched = 1;
}

// One-shot diagnostic: list every adapter d3d8 reports, with its name. Called
// from the first CreateDevice, where a live IDirect3D8 is guaranteed.
//
// Fastest way to tell whether "only 1 adapter" means the machine really has one
// usable GPU, or that a wrapper is presenting a single virtual card in front of
// several. Costs one call per adapter, once.
static void log_adapter_inventory(IDirect3D8 *d3d, UINT count)
{
    void **vt = *(void ***)d3d;
    GetAdapterIdentifier_t get_id = (GetAdapterIdentifier_t)vt[5];
    if (!get_id) {
        log_line("[pso_widescreen] adapters: %u present "
                 "(GetAdapterIdentifier unavailable)", (unsigned)count);
        return;
    }
    for (UINT i = 0; i < count; ++i) {
        D3DADAPTER_IDENTIFIER8_X id;
        memset(&id, 0, sizeof(id));
        if (SUCCEEDED(get_id(d3d, i, 0, &id))) {
            id.Description[WS_ADAPTER_ID_STRLEN - 1] = 0;
            id.Driver[WS_ADAPTER_ID_STRLEN - 1] = 0;
            log_line("[pso_widescreen] adapter %u: '%s' (driver '%s' "
                     "vendor=0x%04X device=0x%04X)",
                     (unsigned)i, id.Description, id.Driver,
                     (unsigned)id.VendorId, (unsigned)id.DeviceId);
        } else {
            log_line("[pso_widescreen] adapter %u: identifier query failed",
                     (unsigned)i);
        }
    }
}


static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(
    IDirect3D8 *self,
    UINT Adapter,
    DWORD DeviceType,
    HWND hFocusWindow,
    DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS_X *pp,
    IDirect3DDevice8 **out)
{
    if (pp && g_cfg.enabled) {
        // GATE on g_cfg.enabled when Enabled=0 we are in
        // pure passthrough — Sodaboy's d3d8.dll has already configured
        // the backbuffer + windowed flag the way IT wants for its own
        // widescreen recipe, and overriding either here clobbers his
        // calculations and produces visible artifacts (giant central
        // oval over the minimap). With Enabled=1 (modernize / no-
        // Sodaboy chain) we own the widescreen end-to-end and
        // OverrideBackBuffer / Windowed / borderless-resize all run.
        const UINT orig_w = pp->BackBufferWidth;
        const UINT orig_h = pp->BackBufferHeight;
        const BOOL orig_w_flag = pp->Windowed;
        // BackBuffer override is opt-in via OverrideBackBuffer=1. Sodaboy
        // convention (corrected after the SEGA splash regression):
        // backbuffer dims are LOGICAL canvas (1920x1080 default) so the
        // engine renders to a surface that matches its res_table view of
        // the world, while the OS window sizes to PHYSICAL Width/Height
        // and the d3d8 wrapper handles BB->window upscaling at present.
        if (g_cfg.override_backbuffer) {
            const int lw = g_cfg.logical_width  > 0 ? g_cfg.logical_width  : 1920;
            const int lh = g_cfg.logical_height > 0 ? g_cfg.logical_height : 1080;
            pp->BackBufferWidth  = (UINT)lw;
            pp->BackBufferHeight = (UINT)lh;
        }
        // Windowed mode: 0=fullscreen, 1=windowed, 2=borderless windowed.
        // For 1 and 2, the d3d8 'Windowed' flag is TRUE. The borderless
        // distinction lives in the OS window style, applied below.
        pp->Windowed = (g_cfg.windowed != 0) ? TRUE : FALSE;
        if (g_cfg.windowed == 2) {
            // The borderless window always sizes to PHYSICAL Width/Height
            // (the user's actual screen resolution) so the d3d8 wrapper
            // has somewhere to stretch the LOGICAL backbuffer to. If
            // Width/Height were unset, fall back to BB dims (= logical).
            int win_w = g_cfg.width  > 0 ? g_cfg.width  : (int)pp->BackBufferWidth;
            int win_h = g_cfg.height > 0 ? g_cfg.height : (int)pp->BackBufferHeight;
            make_borderless(pp->hDeviceWindow ? pp->hDeviceWindow : hFocusWindow,
                            win_w, win_h);
            log_line("[pso_widescreen] borderless window sized to %dx%d (physical)", win_w, win_h);
        }
        log_line("[pso_widescreen] CreateDevice: BackBuffer %ux%u -> %ux%u (logical), "
                 "window=%dx%d (physical), windowed %d -> %d (mode=%d, override_bb=%d)",
                 (unsigned)orig_w, (unsigned)orig_h,
                 (unsigned)pp->BackBufferWidth, (unsigned)pp->BackBufferHeight,
                 g_cfg.width, g_cfg.height,
                 (int)orig_w_flag, (int)pp->Windowed, g_cfg.windowed,
                 g_cfg.override_backbuffer);
    } else if (pp) {
        log_line("[pso_widescreen] CreateDevice: passthrough "
                 "(Enabled=0; Sodaboy or other wrapper owns BB %ux%u, windowed=%d)",
                 (unsigned)pp->BackBufferWidth, (unsigned)pp->BackBufferHeight,
                 (int)pp->Windowed);
    }
    // Adapter inventory: logged once, on the first CreateDevice. Runs whether
    // or not an Adapter is configured, because knowing WHAT d3d8 can see is the
    // diagnostic - a single "internal3D"-style entry means a wrapper is
    // presenting a virtual card and no ordinal will change which GPU is used.
    {
        static int logged_inventory = 0;
        if (!logged_inventory) {
            logged_inventory = 1;
            void **ivt = *(void ***)self;
            GetAdapterCount_t icount = (GetAdapterCount_t)ivt[4];
            log_adapter_inventory(self, icount ? icount(self) : 0);
        }
    }
    // Adapter (GPU) override. Default g_cfg.adapter = -1 leaves the engine's
    // own choice untouched, so behaviour is unchanged unless a launcher or ini
    // explicitly asks for a specific GPU.
    //
    // The ordinal is validated HERE rather than at parse time because the
    // adapter count is only knowable once we have a live IDirect3D8. A stale
    // or bogus value is ignored rather than passed through: an out-of-range
    // ordinal makes CreateDevice fail, and PSOBB reacts to that by dying at
    // startup with no useful message - which is exactly the failure mode this
    // feature exists to avoid.
    UINT use_adapter = Adapter;
    if (g_cfg.adapter >= 0) {
        UINT count = 0;
        void **vt = *(void ***)self;
        GetAdapterCount_t get_count = (GetAdapterCount_t)vt[4];
        if (get_count) count = get_count(self);
        if ((UINT)g_cfg.adapter < count) {
            use_adapter = (UINT)g_cfg.adapter;
            log_line("[pso_widescreen] CreateDevice: adapter %u -> %u "
                     "(configured; %u adapters present)",
                     (unsigned)Adapter, (unsigned)use_adapter, (unsigned)count);
        } else {
            log_line("[pso_widescreen] CreateDevice: configured adapter %d is "
                     "out of range (%u adapters present) - keeping engine's %u",
                     g_cfg.adapter, (unsigned)count, (unsigned)Adapter);
        }
    }

    HRESULT hr = real_CreateDevice(self, use_adapter, DeviceType, hFocusWindow,
                                   BehaviorFlags, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        // Patch unconditionally when we successfully created a device —
        // the SetViewport hook is needed any time we override the
        // backbuffer size, otherwise the engine's launcher-resolution
        // SetViewport leaves us drawing in a corner. SetTransform's
        // FOV path is gated inside Hook_SetTransform on fov_correction.
        patch_device_vtable(*out);
    }
    return hr;
}

static IDirect3D8 *WINAPI Hook_Direct3DCreate8(UINT SDKVersion)
{
    IDirect3D8 *d3d = real_Direct3DCreate8 ? real_Direct3DCreate8(SDKVersion) : NULL;
    if (!d3d) {
        log_line("[pso_widescreen] real Direct3DCreate8(%u) returned NULL", SDKVersion);
        return d3d;
    }
    if (!g_vtable_patched) {
        // Vtable lives at *(void***)d3d. CreateDevice is slot 15 in the
        // d3d8 IDirect3D8 vtable (post QueryInterface/AddRef/Release at
        // 0..2 then 12 enum/cap query methods).
        void **vt = *(void ***)d3d;
        DWORD old_prot = 0;
        if (VirtualProtect(&vt[15], sizeof(void*), PAGE_READWRITE, &old_prot)) {
            real_CreateDevice = (CreateDevice_t)vt[15];
            vt[15] = (void *)&Hook_CreateDevice;
            DWORD tmp = 0;
            VirtualProtect(&vt[15], sizeof(void*), old_prot, &tmp);
            g_vtable_patched = 1;
            log_line("[pso_widescreen] vtable[15] patched: real CreateDevice=0x%p",
                     (void*)real_CreateDevice);
        } else {
            log_line("[pso_widescreen] vtable[15] VirtualProtect FAILED err=%lu",
                     GetLastError());
        }
    }
    return d3d;
}

// ---- IAT patch on psobb.exe's d3d8.dll!Direct3DCreate8 slot ----
//
// We do this rather than inline-hooking the export because IAT lives in
// psobb.exe's own image — independent of which d3d8.dll resolved at
// load time. If the user later swaps d3d8.dll for any other variant
// the IAT slot is already redirected to us.

static int patch_iat(HMODULE mod, const char *target_dll, const char *target_fn,
                     void *new_fn, void **old_fn_out)
{
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    DWORD imp_rva = nt->OptionalHeader.DataDirectory[
        IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!imp_rva) return 0;
    PIMAGE_IMPORT_DESCRIPTOR imp =
        (PIMAGE_IMPORT_DESCRIPTOR)((BYTE*)mod + imp_rva);
    for (; imp->Name; ++imp) {
        const char *dll = (const char *)((BYTE*)mod + imp->Name);
        if (_stricmp(dll, target_dll) != 0) continue;
        PIMAGE_THUNK_DATA orig = imp->OriginalFirstThunk
            ? (PIMAGE_THUNK_DATA)((BYTE*)mod + imp->OriginalFirstThunk)
            : (PIMAGE_THUNK_DATA)((BYTE*)mod + imp->FirstThunk);
        PIMAGE_THUNK_DATA cur  = (PIMAGE_THUNK_DATA)((BYTE*)mod + imp->FirstThunk);
        for (; orig->u1.AddressOfData; ++orig, ++cur) {
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            PIMAGE_IMPORT_BY_NAME ibn =
                (PIMAGE_IMPORT_BY_NAME)((BYTE*)mod + orig->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, target_fn) != 0) continue;
            DWORD old_prot = 0;
            if (!VirtualProtect(&cur->u1.Function, sizeof(void*),
                                PAGE_READWRITE, &old_prot)) {
                return 0;
            }
            if (old_fn_out) *old_fn_out = (void *)cur->u1.Function;
            cur->u1.Function = (DWORD_PTR)new_fn;
            DWORD tmp = 0;
            VirtualProtect(&cur->u1.Function, sizeof(void*), old_prot, &tmp);
            return 1;
        }
    }
    return 0;
}

// ---- Sodaboy-replication memory patches ----
//
// Writes the same psobb.exe data-section modifications that Sodaboy's
// d3d8.dll wrapper applies. Captured live via psobbdbg from a side-by-
// side run (full diff in memory: sodaboy_widescreen_patches.md).
// Without these the engine's RHW HUD verts assume the launcher's chosen
// resolution (e.g. 1024x768) and end up squashed into the upper-left of
// any larger backbuffer.
//
// Three classes of patch:
// 1. NOP 32 bytes at psobb.exe!0x00482e20..0x00482e3f.
// Original: a 10-byte `mov dword [0x00a46c72], 0` followed by
// `push "WINDOW_MODE"` + `call ini_lookup` + `cmp eax, 4` validation
// block that REJECTS any non-tabled resolution. NOP'ing it lets the
// engine accept arbitrary widths/heights.
// 2. Overwrite the 6-entry resolution table at psobb.exe!0x009006f4
// (originally 640x480/800x600/1024x768/1280x960/1280x1024/1600x1200,
// 48 bytes total) with 6 copies of the desired (W,H) so every
// table-lookup picks our resolution.
// 3. Overwrite the 8-row HUD-rect table at psobb.exe!0x009a3840
// (40 dwords / 160 bytes) with values scaled to the new aspect.
// For 1920x1080: { (1023,576,255,192), (1023,384,255,192), ...,
// (511,384,511,384), (511,0,511,384) }. Hardcoded for 1080p only;
// other resolutions need their own table (TODO: derive from cfg).

// Sanity check Sodaboy uses to verify it's running against psobb.exe
// (these are stable code/data signatures of the supported build).
#define PSOBB_SIG_82D1D0 0x246c8b50u  // bytes "P\x8bl$" at code 0x0082d1d0
#define PSOBB_SIG_82D1D4 0x15ff5568u  // bytes "hU\xff\x15"

static int sig_check(uintptr_t addr, uint32_t expected, const char *label)
{
    uint32_t got = *(volatile uint32_t *)addr;
    if (got == expected) return 1;
    log_line("[pso_widescreen] sig MISMATCH %s @ 0x%p: expected 0x%08x got 0x%08x",
             label, (void *)addr, expected, got);
    return 0;
}

static int patch_write(uintptr_t addr, const void *src, size_t len, const char *label)
{
    DWORD old_prot = 0;
    if (!VirtualProtect((LPVOID)addr, len, PAGE_EXECUTE_READWRITE, &old_prot)) {
        log_line("[pso_widescreen] VirtualProtect FAILED %s @ 0x%p err=%lu",
                 label, (void *)addr, GetLastError());
        return 0;
    }
    memcpy((void *)addr, src, len);
    DWORD tmp = 0;
    VirtualProtect((LPVOID)addr, len, old_prot, &tmp);
    // icache-flush hygiene if the target is a .text instruction
    // immediate (VA below the .data boundary 0x008F8000), flush the instruction
    // cache so the CPU never prefetches/executes stale bytes. Harmless for .data
    // VAs (skipped). Matches the convention in patch_frontend_vertical /
    // patch_patch_status_screen, which flush .text imms the same way. These
    // writes all run at ASI attach (long before the target code executes), so on
    // x86 this is correctness-by-construction rather than a live-coherency need —
    // but it makes the shared helper match the file's own convention.
    if (addr < 0x008F8000u) {
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)addr, len);
    }
    log_line("[pso_widescreen] patched %s @ 0x%p (%zu bytes)",
             label, (void *)addr, len);
    return 1;
}

// Rewrite the rel32 displacement of a CALL (E8 imm32) at `site` to target
// `shim` instead of `expect_target`. Validates the existing call: first byte
// must be 0xE8 AND the current call destination must equal `expect_target`,
// so a non-matching build is left untouched. Used for one-call-site shims
// (e.g. the char-select scene's ResetNPCCamera call) — finer-grain than a
// MinHook detour on the target function (which would catch every caller).
static int redirect_call(uint32_t site, uint32_t expect_target, void *shim)
{
    uint8_t *p = (uint8_t *)(uintptr_t)site;
    if (p[0] != 0xE8) {
        log_line("[pso_widescreen] redirect_call: not a CALL @ 0x%08x (byte=0x%02x)", site, p[0]);
        return 0;
    }
    uint32_t cur_tgt = (uint32_t)(site + 5 + *(int32_t *)(p + 1));
    if (cur_tgt != expect_target) {
        log_line("[pso_widescreen] redirect_call: target mismatch @ 0x%08x (expected 0x%08x got 0x%08x)",
                 site, expect_target, cur_tgt);
        return 0;
    }
    int32_t rel = (int32_t)((uintptr_t)shim - (uintptr_t)(site + 5));
    DWORD old;
    if (!VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *(int32_t *)(p + 1) = rel;
    DWORD t; VirtualProtect(p + 1, 4, old, &t);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    log_line("[pso_widescreen] redirect_call: 0x%08x: 0x%08x -> %p", site, expect_target, shim);
    return 1;
}

// SCALE-AWARE UI COORDS. The engine's 2D UI logical canvas scales INVERSELY with
// HUDScale: logical_W = 853.33 * hud_scale, so the design->screen matrix
// [0xACC0E8] = 1920 / (853.33 * hud_scale) = 2.25 / hud_scale. (Verified live:
// HUDScale 1.0 -> canvas 853x480, matrix 2.25; HUDScale 1.5 -> 1280x720, matrix
// 1.5.) Every UI design coord lives in ONE native frame dialed at the hs=1.0
// canvas; ui_coord(x) = x * hud_scale (uniform multiplier) holds the same
// on-screen position at any HUDScale, keeping the widescreen LAYOUT fixed
// independent of the HUD-scale knob.
static float ui_coord(float design)
{
    float hs = g_cfg.hud_scale;
    if (!(hs > 0.1f && hs < 10.0f)) hs = 1.0f;
    return design * hs;
}

// ============================================================
// CHAR-CREATE HEX BACKDROP 16:9 FILL (Ephinea-faithful).
// ============================================================
// The char-create dressing-room backdrop is the 24 ×(renderH/480 == ×hud_scale)
// .data tiles (12 vec2f positions @0x0091DBxx + 12 u16 W/H shorts @0x009B77xx),
// declared as SRC_EPHINEA / GATE_CHARSELECT rows in kBakes[]. Ephinea uses no
// per-frame column-emit splice — see the "SRC_EPHINEA char-create dressing-room
// tiles" block in kBakes[].



// CHAR-SELECT 3D MODEL pan — shim on the char-select scene's call to
// ResetNPCCamera at 0x004138A3. Runs the ORIGINAL reset (0x0081E1F4), then pans
// the ACTIVE preview camera +g_model_dx in world X so the 3D character shifts
// LEFT (centered between the CHARACTERS panel and the info box). Char-select-
// EXCLUSIVE: only this ONE call site is redirected — other menu preview scenes
// that share ResetNPCCamera are untouched. Rides the engine's existing per-frame
// camera reset (no new per-frame code of ours). World-space offset = orthogonal
// to UI / HUDScale.
static float g_model_dx = 8.0f;
static __declspec(naked) void rb_charsel_camera(void)
{
    __asm {
        push ecx                                       /* preserve this (thiscall) */
        mov  eax, 0x0081E1F4                           /* ResetNPCCamera           */
        call eax
        pop  ecx
        mov  eax, dword ptr ds:[0x00A489F4]            /* active camera index      */
        mov  eax, dword ptr [eax*4 + 0x00A48A00]       /* camera object ptr        */
        test eax, eax
        je   done
        fld  dword ptr [eax+0x3C]                      /* target.X += g_model_dx   */
        fadd dword ptr ds:[g_model_dx]
        fstp dword ptr [eax+0x3C]
        fld  dword ptr [eax+0x48]                      /* source.X += g_model_dx   */
        fadd dword ptr ds:[g_model_dx]
        fstp dword ptr [eax+0x48]
    done:
        ret
    }
}

// ============================================================
// EFFECT-DEANCHOR (in-game 16:9 wide-fill)
// ============================================================
// The in-game 2D affine (0x00ACC0E8/EC) is forced WIDE (2.25 @hs1.0 / 1.5 @hs1.5)
// by the F_VPRESET source bakes in apply_startup_bakes so the HUD fills 16:9. But
// that affine is GLOBAL — it is multiplied into 2D verts inside the shared submit
// helper 0x0082B158 (-> emitter 0x00836D04), which also carries SCREEN-SPACE combat
// effects (photon blasts / billboards). Three effect-draw CALL sites route those
// screen-space effects through 0x0082B158; left alone they would inherit the wide
// affine and STRETCH. We bracket each: save the two affine floats -> write 1.0f ->
// run the real submit -> restore. Net: HUD wide, effects native.
//
// The strip value is literally 1.0f regardless of HudScale (we render those effects
// in native 2D space, then restore whatever wide value the HUD uses).
//
// These three CALL redirects are CODE-FLOW detours and are WIPED by the FE->IG
// transition (unlike the immediate .text affine bakes which survive). So they are
// (re)installed from reassert_ingame_hooks(), called every in-game worker tick —
// including the hud_scale==1.0 wide-fill lever (the affine is wide at 1.0 too).
static const uint32_t kStripSites[3] = { 0x004A9C0Cu, 0x004A9D28u, 0x005474D4u }; // all `call 0x0082B158`

// Static save slots for the deanchor wrap. Render-thread-only and NON-RECURSIVE
// (the screen-space effect submit does not re-enter one of the 3 strip sites), so
// a static save area is safe and keeps the args stack pristine for forwarding.
static volatile uint32_t g_deanchor_save_x = 0;   // saved 0x00ACC0E8 (raw bits)
static volatile uint32_t g_deanchor_save_y = 0;   // saved 0x00ACC0EC (raw bits)
static volatile uint32_t g_deanchor_ret    = 0;   // stashed real caller return addr

// rb_deanchor_shim — transparent cdecl wrapper around 0x0082B158.
// Mechanism (no arg-count knowledge, forwards args UNTOUCHED, EAX return flows
// through, non-volatiles untouched): save+override the affine, then SWAP the
// caller's return address for our after-stub and JMP (not CALL) into the real
// function so the args stay exactly where the caller laid them. The function
// returns into rb_deanchor_after, where we restore the affine and JMP to the real
// caller return (esp still at the args -> the cdecl caller cleans them, as it would
// have after the original call). flags/scratch regs: cdecl callers don't rely on
// EFLAGS/ECX/EDX across a call; EAX (return value) is preserved across the restore.
__declspec(naked) static void rb_deanchor_shim(void)
{
    __asm {
        // entry: [esp]=caller_ret, [esp+4..]=cdecl args
        mov  eax, dword ptr ds:[0x00ACC0E8]            // save SCALE_X
        mov  dword ptr ds:[g_deanchor_save_x], eax
        mov  eax, dword ptr ds:[0x00ACC0EC]            // save SCALE_Y
        mov  dword ptr ds:[g_deanchor_save_y], eax
        mov  dword ptr ds:[0x00ACC0E8], 0x3F800000     // SCALE_X <- 1.0f
        mov  dword ptr ds:[0x00ACC0EC], 0x3F800000     // SCALE_Y <- 1.0f
        mov  eax, dword ptr [esp]                       // eax = real caller return addr
        mov  dword ptr ds:[g_deanchor_ret], eax         // stash it
        mov  eax, offset rb_deanchor_after
        mov  dword ptr [esp], eax                        // redirect return to our after-stub
        mov  eax, 0x0082B158
        jmp  eax                                         // tail-enter real submit; args intact, [esp]=after-stub
    rb_deanchor_after:
        // real submit returned: cdecl args still on stack, eax = return value
        push eax                                        // preserve return value
        mov  eax, dword ptr ds:[g_deanchor_save_x]
        mov  dword ptr ds:[0x00ACC0E8], eax             // restore SCALE_X
        mov  eax, dword ptr ds:[g_deanchor_save_y]
        mov  dword ptr ds:[0x00ACC0EC], eax             // restore SCALE_Y
        pop  eax                                        // restore return value
        jmp  dword ptr ds:[g_deanchor_ret]              // -> real caller (esp at args)
    }
}

// ============================================================
// KILL-SCREEN HEXAGON-LAYER ASPECT-FIT PRE-PASS (Ephinea FUN_52da6e50 port)
// ============================================================
// Site: 0x0067C4ED `call 0x0082B558` inside the boss kill-screen hexagon-layer
// fullscreen 2D-overlay render. Ephinea installs a 5-byte detour of the CALL
// (FUN_52dc3240(FUN_52da6e50, 0x67c4ed, 5)) that runs an aspect-fit / recenter
// PRE-PASS over the packed (x,y) f32 vertex array immediately before the submit
// helper 0x0082B558, so the fullscreen layer fills the (taller/wider) physical
// viewport height-fit + centered instead of being left-anchored X-stretched.
//
// Descriptor (built at 0x67C4B2.., arg0 = ecx = lea[esp+0x1C4]):
// [+0x00] vertPtr   (lea[esp+0x44])      — packed f32 pairs (x,y), stride 8
// [+0x04] colorPtr  (lea[esp+0x144])
// [+0x08] 0
// [+0x0C] vertCount (ebx)
// The vert array is FRESHLY BUILT per call into a stack buffer (the 0x67C42B
// loop writes [esp+0x44..]), so the in-place `*=k; +=off` does NOT accumulate
// across frames (no idempotency hazard).
//
// MATH (faithful to FUN_52da6e50, mapped to OUR globals; renderW/renderH = the
// DESIGN dims design_w[0x0098A4B8] / design_h[0x0098A4B4] = 853.33*hud_scale /
// 480*hud_scale in-game, == Ephinea _DAT_5318deec / _DAT_5318d124). These are
// DESIGN dims, NOT the physical backbuffer (g_scale.render_w/h) — the earlier
// physical-dims build double-scaled (k=1080/480=2.25 then ANOTHER 2.25 from the
// affine = ~5x overscale). With design dims the 16:9 else-branch gives k=design_h/
// 480 = hud_scale and off = (design_w-640*k)/2; at design 640x480 -> k=1,off=0
// (4:3 identity), and we also install-gate on game_aspect>4:3.
//
// COMPOSITION: this pre-pass writes DESIGN-space coords (height-fit + centered);
// 0x0082B558 THEN multiplies by the global affine 0x00ACC0E8 (=render_w/design_w
// = 2.25/hud_scale). k=hud_scale and affine=2.25/hud_scale CANCEL -> the on-screen
// result is hud_scale-INVARIANT (full-height, centered, margins 240px @1920). That
// is EXACTLY Ephinea's compose (its submit applies the same affine after the
// pre-pass). Do NOT also deanchor/strip the affine at this site (Ephinea lets it
// apply); this is NOT a double-bake.
//
// INSTALL: boot .text CALL-site redirect (one-shot, persistent) — this is a
// render routine NOT in the kStripSites effect-deanchor set, so it is NOT wiped
// by the FE->IG transition (it behaves like the persistent .text affine bakes).
// Installed once in apply_static_patches via redirect_call.
//
// C helper: in-place aspect-fit the packed (x,y) f32 vert array. renderW/renderH
// are the LIVE DESIGN dims (design_w 0x0098A4B8 / design_h 0x0098A4B4), read
// PER-CALL via hs_peek_f32 so they track the FE<->IG flip + hud_scale (a boot-
// captured g_scale would be both the wrong magnitude AND stale). Faithful port of
// FUN_52da6e50: pillarbox (renderW/renderH < 4/3) scales by width + centers
// vertically; else (16:9-wide, our case) scales by height + centers horizontally.
// hs_peek_f32 is defined far below; forward-declare it here (first use in the file)
// so the design_w/h reads aren't implicit-int. (Mirrors the decl before apply_bakes.)
static float hs_peek_f32(uint32_t va);
static void __cdecl ks_hexfit_transform(float *vptr, int n)
{
    if (!vptr || n <= 0) return;
    const float renderW = hs_peek_f32(0x0098A4B8u);         // design_w (853.33*hud_scale in-game)
    const float renderH = hs_peek_f32(0x0098A4B4u);         // design_h (480*hud_scale in-game)
    if (!(renderW > 1.0f) || !(renderH > 1.0f)) return;     // unset/front-end -> bail (no transform)
    const int   pillarbox = (renderW / renderH) < (4.0f / 3.0f);
    float k, off;
    if (pillarbox) { k = renderW / 640.0f; off = renderH / 2.0f - (480.0f * k) / 2.0f; }
    else           { k = renderH / 480.0f; off = renderW / 2.0f - (640.0f * k) / 2.0f; }
    for (int i = 0; i < n; i++) {
        vptr[2 * i]     *= k;
        vptr[2 * i + 1] *= k;
        if (pillarbox) vptr[2 * i + 1] += off;
        else           vptr[2 * i]     += off;
    }
}

// ks_hexfit_shim — naked CALL-site redirect for 0x0067C4ED. On entry the cdecl
// args are already on the stack: [esp]=caller_ret(0x67C4F2), [esp+4]=descPtr,
// [esp+8]=count, [esp+0xC]=0xC2EA0000, [esp+0x10]=0x42. We read descPtr=[esp+4],
// pull vptr=[descPtr+0] and n=[descPtr+0xC], run the transform, then tail-jmp to
// 0x0082B558 with ALL args UNTOUCHED (the cdecl caller at 0x67C4F2 does its
// `add esp,0x1E4` cleanup as before). No after-stub: nothing to restore.
__declspec(naked) static void ks_hexfit_shim(void)
{
    __asm {
        // entry: [esp]=caller_ret, [esp+4]=descPtr, [esp+8]=count, ...
        pushad                                          // preserve ALL caller regs
        mov  eax, dword ptr [esp + 0x20 + 4]            // descPtr (pushad pushed 0x20 bytes)
        mov  edx, dword ptr [eax + 0x0C]                // n = desc[+0x0C] (vertCount)
        mov  eax, dword ptr [eax + 0x00]                // vptr = desc[+0x00]
        push edx                                        // arg2: n
        push eax                                        // arg1: vptr
        call ks_hexfit_transform                        // cdecl
        add  esp, 8
        popad                                           // restore caller regs
        mov  eax, 0x0082B558
        jmp  eax                                        // tail-enter real submit; args intact
    }
}

// ks_hexfit_install — boot .text CALL-site redirect of 0x0067C4ED to our shim.
// One-shot persistent (NOT a kStripSites/effect-deanchor detour -> the FE->IG
// transition does not wipe it). Gated by caller on g_cfg.enabled && aspect>4:3.
static int ks_hexfit_install(void)
{
    if (!redirect_call(0x0067C4EDu, 0x0082B558u, (void *)&ks_hexfit_shim)) {
        log_line("[pso_widescreen] killscreen hexfit: SKIP (redirect failed)");
        return 0;
    }
    log_line("[pso_widescreen] killscreen hexfit: aspect-fit pre-pass installed (render %dx%d)",
             g_scale.render_w, g_scale.render_h);
    return 1;
}

// ============================================================
// IN-GAME LIST-WINDOW BOTTOM-ANCHOR (Ephinea FUN_52d8cb90 port)
// ============================================================
// Site: 0x0073FF92 `call 0x00737F80` — the [0xA48A3C]==1 (single-player) arm of
// ListWindowObject_Create_0073ff34. The case pushes 0x972DEC
// (item_bank_and_quest_list_dimensions = design {385.0, 184.0}) then [ebp+0xc]
// (the id), then calls ListWindowObject_InitBase_00737f80(xy*, id), which stores
// xy[0]->[0x9F3130] (base X), xy[1]->[0x9F3134] (base Y), id->[0x9F3128]. That
// {X,Y} is the CONSTRUCTION-TIME base position of the in-game item-bank / quest-
// counter / shop LIST WINDOW.
//
// Ephinea (FUN_52d8cb90) replaces the orig xy with a scratch {liveX, Y'} where
// Y' = (renderH-480)+Yoffset, bottom-anchoring the window to the taller widescreen
// viewport, plus a live-screen-bounds clamp and X=*liveX substitution. Ephinea's
// renderH (_DAT_5318d124 = 480*scale) is the DESIGN height = our design_h
// [0x0098A4B4] (480 native / 720 @hs1.5), NOT physical render_h.
//
// FAITHFUL-EFFECT PORT (not byte-faithful): we port the DOMINANT effect = the
// base-Y bottom anchor `Y = orig_xy[1] + (design_h - 480.0f)` and KEEP X =
// orig_xy[0]. Ephinea's clamp branch and X=*liveX substitution dereference DLL-
// private live-screen-rect pointers (Yoffset/PAD/p108/p10c/p114) that are NOT
// resolvable to engine VAs, so we deliberately drop them (the clamp only mattered
// at extreme aspect ratios to keep the window on-screen). Gate: design_h>480 (==
// hs>1.0); at design_h==480 the term is 0 -> byte-identical no-op. We do NOT gate
// on Ephinea's private flag [0x00A95EDC] (we never set it).
//
// COMPOSITION: this sets the window's DESIGN-space base Y before the global 2D
// affine 0x00ACC0E8/EC scales it -> the (design_h-480) term is in design units
// and the affine maps it to screen correctly. It self-scales via design_h (which
// already encodes hud_scale), so it is correct at all hud_scales.
//
// INSTALL: boot .text redirect of `call 0x737f80` @0x0073FF92 to our naked stub
// (stub tail-jmps to 0x00737F80). RE-ASSERT REQUIRED: ListWindowObject_Create is
// a CODE-FLOW detour reachable both front-end (game-list) and in-game (bank/shop/
// quest); the FE->IG transition WIPES .text code-flow hooks, so we re-install it
// every in-game worker tick (reassert_ingame_hooks) if reverted to stock.
// Ephinea hooks ONLY the ==1 arm; we do the same (do NOT touch the ==2 / ==3-4
// arms at 0x0073FF73 / 0x0073FFF9) for a faithful single-bake port.
static volatile uint32_t g_lwy_scratch[2];   // {X, Y'} substituted vec2f arg

// lw_yanchor_shim — naked cdecl wrapper of ListWindowObject_InitBase (0x00737F80)
// at the ==1-arm call 0x0073FF92. On entry (caller pushed `push [ebp+0xc]` then
// `push 0x972DEC`): [esp]=caller_ret(0x73FF97), [esp+4]=xy* (=0x972DEC), [esp+8]=id.
// We compute scratch.x = xy[0]; scratch.y = xy[1] + (design_h - 480); rewrite the
// on-stack xy arg to &g_lwy_scratch, then tail-jmp to 0x00737F80 (args otherwise
// intact -> the cdecl caller's `add esp,8` cleans up as before). No after-stub.
static void __cdecl lw_yanchor_compute(const float *xy)
{
    // This init_data slot is NOT a Y position — it is the {w,h} dimension pair's
    // HEIGHT (InitBase init_data[1] -> MenuListItem +0x3c source.height -> +0x4c
    // render.height). Writing a design_h-derived value here INFLATES the list-frame
    // height (so the Hunter's-Guild / bank / shop / game-list box renders ~80% tall
    // regardless of entry count) and regresses 4:3. Pass the stock height through;
    // the existing global 2D affine 0x00ACC0E8/EC scales it exactly like stock. (A
    // genuine bottom-anchor, if wanted, must target the POSITION arg / object +0x34,
    // never this dim pair.)
    g_lwy_scratch[1] = *(const volatile uint32_t *)&xy[1];   // HEIGHT pass-through = stock 184

    // X: KEEP STOCK 385. 0x00973CEC is NOT a coordinate — it is entry +0x2C of a
    // 24-slot CODE-ADDRESS table @0x00973CC0 (all 0x0075xxxx), byte-identical in
    // Ephinea and ours at the front-end. Derefing it yields a ~1e-38 denormal -> X~=0
    // -> the bank/shop window snaps far-left, which the `lx>-3000 && lx<5000` guard
    // does NOT reject. Ephinea does track the widescreen left edge, but the true
    // viewport-left global is not yet identified (needs a live in-game bank/shop
    // read); stock 385 never regresses.
    g_lwy_scratch[0] = *(const volatile uint32_t *)&xy[0];   // X = stock design 385 (safe)
}

__declspec(naked) static void lw_yanchor_shim(void)
{
    __asm {
        pushad                                          // preserve ALL caller regs
        mov  eax, dword ptr [esp + 0x20 + 4]            // xy* (pushad pushed 0x20 bytes)
        push eax                                        // arg: const float* xy
        call lw_yanchor_compute                         // fills g_lwy_scratch
        add  esp, 4
        popad
        // rewrite the on-stack xy arg ([esp+4]) to point at our scratch pair.
        mov  dword ptr [esp + 4], offset g_lwy_scratch
        mov  eax, 0x00737F80
        jmp  eax                                        // tail-enter InitBase; id arg intact
    }
}

// lw_yanchor_redirect — install/re-install the call-redirect at 0x0073FF92.
// Idempotent + steady-state quiet: only attempt the redirect when the site is
// STOCK (`call 0x00737F80`); a site already pointing at our shim is silently
// skipped (mirrors reassert_ingame_hooks' deanchor pattern). Returns 1 iff we
// (re)installed this call.
static int lw_yanchor_redirect(void)
{
    const uint32_t site = 0x0073FF92u;
    uint8_t *p = (uint8_t *)(uintptr_t)site;
    __try {
        if (p[0] != 0xE8) return 0;                                  // not a CALL -> leave
        uint32_t cur = (uint32_t)(site + 5 + *(int32_t *)(p + 1));
        if (cur != 0x00737F80u) return 0;                            // already ours / foreign -> skip
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return redirect_call(site, 0x00737F80u, (void *)&lw_yanchor_shim);
}

// ============================================================
// FIX B — QUEST/AREA-TRANSITION "NOW LOADING" RING + RUNE-HEIGHT (Shape A).
// ============================================================
// The green seal-ring + 3 spinning rune orbs on the quest/area-transition loading
// screen are drawn by RenderGameListBackgroundAndChatBubbles_006f475c (vtable
// 0x00B3E9B0 slot +8 = 0x00B3E9B8; this=ecx; gated internally by g_IsRenderingActive
// [0x00A16380]). Its 2D quads go through composer 0x0082BB74 which applies the GLOBAL
// 2D affine asymmetrically: screenX = desc[0]*SCALE_X[0xACC0E8] + OFFSET_X[0xACC0F0],
// screenY = desc[4]*SCALE_Y[0xACC0EC] + OFFSET_Y[0xACC0F4]. During the transition there
// is NO live player, so worker_scale_poke's symmetric in-game affine pin never fires and
// the loading scene's own setter governs with SCALE_X != SCALE_Y -> the ring squashes
// (bug a: "should be original 1.0") and the orbs come out right-width-wrong-height (bug b).
//
// SHAPE A: wrap 0x006F475C and, FOR THE LOADING SUB-STATE ONLY, force a SQUARE + centered
// affine for the duration of its render, then restore. One wrap fixes BOTH bugs without
// touching the global affine for any other pass (lobby/in-game stay wide).
//
// SCOPING (critical): 0x006F475C is RenderGameListBackgroundAndChatBubbles — it ALSO draws
// the LOBBY game-list background. The override is gated on the loading sub-state
// (g_is_quest_loading [0x00AAB378] != 0). When NOT loading (lobby game-list browsing) the
// shim is a transparent pass-through -> the shared lobby path is byte-identical to stock.
//
// MECHANISM: there is NO `E8 call 0x006F475C` site in the image (the render is dispatched
// virtually via `call [vtable+8]`), so the ks_hexfit/lw_yanchor E8 redirect_call does not
// apply. Instead we redirect the .data VTABLE SLOT 0x00B3E9B8 (stock = 0x006F475C) to our
// shim. A .data pointer write is idempotent + value-guarded + sig-guarded, and (being .data,
// like the immediate affine bakes) SURVIVES the FE->IG/loading transition that wipes .text
// code-flow detours. We still re-assert it (stock-guarded, steady-state quiet) every
// transition tick for safety, exactly like lw_yanchor.
//
// SQUARE BASIS (TUNABLE / UNVERIFIED-LIVE): we set SCALE_X = SCALE_Y = min(live SCALE_X,
// live SCALE_Y) so the ring keeps its original aspect (un-stretched, full size) and the orbs
// get equal X/Y scale (right height). We re-center horizontally so the design-space ring
// center (320,220) maps to (render_w/2, unchanged): OFFSET_X' = render_w/2 - 320*s. The main
// agent must confirm live: read [0xACC0E8] vs [0xACC0EC] during the loading screen at 16:9 to
// confirm the asymmetry + that min() (height-fit) is the right basis (vs max / a fixed ratio);
// FIXB_RING_CENTER_DESIGN_X is the design-space ring center X (320 per the static 0x006F4915
// init _DAT_00a96918=320.0). At true 4:3 the affine is already square (SCALE_X==SCALE_Y) and
// centered, so save==restore writes the same bits (value-guarded) -> byte-identical no-op.
#define FIXB_RING_VTABLE_SLOT_VA   0x00B3E9B8u   // vtable 0x00B3E9B0 slot +8
#define FIXB_RING_RENDER_VA        0x006F475Cu   // RenderGameListBackgroundAndChatBubbles
#define FIXB_QUEST_LOAD_VA         0x00AAB378u   // g_is_quest_loading (!=0 in the loading sub-state)
#define FIXB_AFFINE_SX_VA          0x00ACC0E8u   // SCALE_X
#define FIXB_AFFINE_SY_VA          0x00ACC0ECu   // SCALE_Y
#define FIXB_AFFINE_OX_VA          0x00ACC0F0u   // OFFSET_X
#define FIXB_AFFINE_OY_VA          0x00ACC0F4u   // OFFSET_Y
#define FIXB_RING_CENTER_X_VA      0x00A96918u   // _DAT_00a96918 = live ring center X (design space)
// NB the existing anzz1 row 0x006F4922 rewrites the center-X INIT immediate from 320.0 to
// design_w*0.5 (and 0x006F4936: 220.0 -> design_h*0.5), so at runtime the ring center X is
// design_w/2 (640 @hs1.0 native... =320 @4:3 / 640 @1280-canvas), NOT a fixed 320. We READ
// the live center from this VA rather than hardcoding, so FIX B composes with that rewrite.

// hs_poke_f32 (value-guarded .data float store) is defined far below; forward-declare it
// here (first use in the file) so the affine writes below aren't implicit-int. Mirrors the
// hs_peek_f32 forward decl before ks_hexfit_transform.
static void hs_poke_f32(uint32_t va, float v);

// non-recursive render-thread-only save slots (the loading ring does not re-enter itself)
static volatile uint32_t g_ldring_this    = 0;   // saved this (ecx) across the C-helper calls
static volatile uint32_t g_ldring_active  = 0;   // 1 = we overrode the affine this render (restore in post)
static volatile uint32_t g_ldring_sx      = 0;   // saved SCALE_X bits
static volatile uint32_t g_ldring_sy      = 0;   // saved SCALE_Y bits
static volatile uint32_t g_ldring_ox      = 0;   // saved OFFSET_X bits
static volatile uint32_t g_ldring_oy      = 0;   // saved OFFSET_Y bits

// ld_ring_pre — runs BEFORE the real render. Returns 1 (and overrides the affine) iff
// the loading sub-state is active; 0 otherwise (lobby game-list path => no override).
// SEH-guarded; on any fault returns 0 (transparent pass-through).
static int __cdecl ld_ring_pre(void)
{
    __try {
        if (!g_cfg.enabled) return 0;
        if (*(volatile uint32_t *)(uintptr_t)FIXB_QUEST_LOAD_VA == 0u) return 0;  // not loading => pass through (lobby)

        float sx = *(volatile float *)(uintptr_t)FIXB_AFFINE_SX_VA;
        float sy = *(volatile float *)(uintptr_t)FIXB_AFFINE_SY_VA;
        if (!(sx > 0.0001f) || !(sy > 0.0001f)) return 0;                         // unset affine => bail

        float s = (sx < sy) ? sx : sy;                                            // square = height-fit (min)
        /* re-center horizontally: the live design-space ring center X must keep landing at
           render_w/2 under the square scale (so shrinking SCALE_X to the square value does not
           drift the ring off-center). center_x = _DAT_00a96918 (design_w/2 after the anzz1
           center-rewrite, or 320 stock). render_w = the live backbuffer extent. */
        float center_x = *(volatile float *)(uintptr_t)FIXB_RING_CENTER_X_VA;
        if (!(center_x > 0.0f)) center_x = 320.0f;                                // stock fallback
        float rw = (float)g_scale.render_w;
        if (!(rw > 1.0f)) rw = sx * 853.333f;                                     // fallback: SCALE_X*design_w_native
        float new_ox = rw * 0.5f - center_x * s;

        /* save the live affine, then write the square + centered override (value-guarded). */
        g_ldring_sx = *(volatile uint32_t *)(uintptr_t)FIXB_AFFINE_SX_VA;
        g_ldring_sy = *(volatile uint32_t *)(uintptr_t)FIXB_AFFINE_SY_VA;
        g_ldring_ox = *(volatile uint32_t *)(uintptr_t)FIXB_AFFINE_OX_VA;
        /* OFFSET_Y is left UNCHANGED (Shape-A re-centers X only; the loading screen is full-
           height so Y follows the height-fit scale). UNVERIFIED-LIVE: if s != live SCALE_Y the
           vertical center can drift; if the main agent sees the ring sit too high/low, mirror
           the X recenter on Y here: new_oy = render_h/2 - center_y(_DAT_00a9691c)*s. Saved for
           restore symmetry. */
        g_ldring_oy = *(volatile uint32_t *)(uintptr_t)FIXB_AFFINE_OY_VA;
        hs_poke_f32(FIXB_AFFINE_SX_VA, s);
        hs_poke_f32(FIXB_AFFINE_SY_VA, s);
        hs_poke_f32(FIXB_AFFINE_OX_VA, new_ox);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ld_ring_post — runs AFTER the real render. Restores the saved affine iff ld_ring_pre
// overrode it (g_ldring_active). Value-guarded writes (idempotent).
static void __cdecl ld_ring_post(int overrode)
{
    if (!overrode) return;
    __try {
        hs_poke_f32(FIXB_AFFINE_SX_VA, *(volatile float *)&g_ldring_sx);
        hs_poke_f32(FIXB_AFFINE_SY_VA, *(volatile float *)&g_ldring_sy);
        hs_poke_f32(FIXB_AFFINE_OX_VA, *(volatile float *)&g_ldring_ox);
        hs_poke_f32(FIXB_AFFINE_OY_VA, *(volatile float *)&g_ldring_oy);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// ld_ring_shim — __thiscall wrapper invoked via the redirected vtable slot (ecx=this,
// [esp]=dispatcher return). The real render takes only `this` (no stack args), so this is a
// clean prologue/epilogue: stash this, run pre (may override affine), reload this, CALL the
// real render, run post (restore), return to the dispatcher.
__declspec(naked) static void ld_ring_shim(void)
{
    __asm {
        mov  dword ptr ds:[g_ldring_this], ecx          // save this
        call ld_ring_pre                                // cdecl -> eax = overrode?
        mov  dword ptr ds:[g_ldring_active], eax
        mov  ecx, dword ptr ds:[g_ldring_this]          // restore this for the thiscall
        mov  eax, FIXB_RING_RENDER_VA
        call eax                                        // real RenderGameListBackgroundAndChatBubbles (ecx=this)
        mov  eax, dword ptr ds:[g_ldring_active]
        push eax                                        // arg: overrode
        call ld_ring_post                               // cdecl
        add  esp, 4
        ret                                             // back to the vtable dispatcher
    }
}

// ld_ring_redirect — install/re-install the loading-ring affine wrap by redirecting the
// .data vtable slot 0x00B3E9B8 from 0x006F475C to our shim. Idempotent + steady-state quiet:
// only writes when the slot is STOCK (== 0x006F475C); a slot already pointing at our shim, or
// at a foreign value, is left untouched. Returns 1 iff we (re)installed this call. The slot is
// .data (no icache flush needed); we VirtualProtect it for the write. Mirrors the
// lw_yanchor_redirect stock-guard so re-assertion from the transition tick is silent.
static int ld_ring_redirect(void)
{
    volatile uint32_t *slot = (volatile uint32_t *)(uintptr_t)FIXB_RING_VTABLE_SLOT_VA;
    __try {
        uint32_t cur = *slot;
        if (cur == (uint32_t)(uintptr_t)&ld_ring_shim) return 0;   // already ours -> skip quietly
        if (cur != FIXB_RING_RENDER_VA) return 0;                  // foreign / not stock -> leave
        DWORD old, tmp;
        if (!VirtualProtect((LPVOID)(uintptr_t)FIXB_RING_VTABLE_SLOT_VA, 4, PAGE_EXECUTE_READWRITE, &old))
            return 0;
        *slot = (uint32_t)(uintptr_t)&ld_ring_shim;
        VirtualProtect((LPVOID)(uintptr_t)FIXB_RING_VTABLE_SLOT_VA, 4, old, &tmp);
        log_line("[pso_widescreen] loading-ring affine wrap: vtable slot 0x%08x 0x%08x -> %p",
                 FIXB_RING_VTABLE_SLOT_VA, FIXB_RING_RENDER_VA, (void *)&ld_ring_shim);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ====================================================================
// char-select SCALES with HudScale (Ephinea-style): the .data anchors below
// stride in the LIVE hud-scaled frame (s->A / s->C).


// Char-create is owned entirely by the static apply_startup_bakes() pass (.text
// imm32 + .rdata float rewrites, value-guarded) plus stock geometry; there is no
// per-frame char-create render-thread machinery. The runtime-only class-select
// content rows are intentionally not strided.

// Char-select 3D MODEL pan — SEPARATE from patch_charselect_layout so it survives
// PatchCharSelect=0 (the 2D-UI patch overstretches at HUDScale 1.0 and is gated
// off; the model pan is HUDScale-independent world-space camera offset). Redirects
// the scene's ResetNPCCamera call (the per-frame menu-camera reset) to our shim.
static int patch_charselect_model_pan(void)
{
    if (!redirect_call(0x004138A3u, 0x0081E1F4u, (void *)&rb_charsel_camera)) {
        log_line("[pso_widescreen] charselect: model-pan SKIP (redirect failed)");
        return 0;
    }
    log_line("[pso_widescreen] charselect: model pan +%.1f world-X installed", g_model_dx);
    return 1;
}


// Char-create scaling tuner globals — kept as a dllexport ABI for the external
// mod_widescreen_tuner (resolved by name via GetProcAddress on pso_widescreen.asi).

__declspec(dllexport) volatile float pso_widescreen_charscreen_stretch = 1.65f;
__declspec(dllexport) volatile float pso_widescreen_charscreen_shift_x = 0.0f;
__declspec(dllexport) volatile float pso_widescreen_charscreen_shift_y = 0.0f;


// SEGA-splash widening: Ephinea widens ONLY the two right-quad x2 coords
// (0x009A3468/0x009A3488), kept via the anzz1 SRC_ANZZ1 "hud.w" kBakes rows. The
// g_cfg patch_splash_phase2 field is retained for cfg-ABI compatibility but no
// longer widens anything.

// ============================================================
// d3d8 wrapper detection
// ============================================================
// Detect which d3d8 implementation is providing Direct3DCreate8 at attach, so
// downstream gates can adapt. We do NOT push a logical-canvas divisor hint to
// the wrapper — a single global RHW divisor cannot map both engine-stretched
// (physical-pixel) and 4:3-reference (640x480 logical) widget populations onto
// one screen, so we gate behavior on the detected stack instead.
//
// Wrapper detection contract:
// - Export `d3d8to11_get_wrapper_info(unsigned int *out_version)`.
// - Returns 0xD3D81011u to confirm "I am d3d8to11".
// - `out_version` (optional) receives a packed YYYYMMDD int.
// - Symbol absent -> wrapper is Sodaboy / native d3d8 / d3d8to9 /
//   dgVoodoo / DXVK-d3d8 / unknown. Use safe defaults.
typedef unsigned int (__cdecl *PFN_get_wrapper_info)(unsigned int *out_version);

// Detected stack (set in detect_d3d8_stack). Values:
// 0 = unknown / not yet detected (assume Sodaboy-compatible)
// 1 = d3d8to11 (RHW divide pinned to physical viewport in HLSL)
static int  g_detected_stack       = 0;
static char g_detected_dll_path[MAX_PATH];
static unsigned int g_detected_d3d8to11_version = 0;

static const char *detected_stack_name(void)
{
    switch (g_detected_stack) {
        case 1:  return "d3d8to11";
        default: return "unknown(Sodaboy/native/d3d8to9/etc.)";
    }
}

// detect_d3d8_stack: probe d3d8.dll at attach time, classify which
// wrapper is providing Direct3DCreate8. Result drives downstream gates.
static void detect_d3d8_stack(void)
{
    HMODULE d3d8 = GetModuleHandleA("d3d8.dll");
    if (!d3d8) {
        log_line("[pso_widescreen] detect_d3d8_stack: d3d8.dll not loaded at attach (deferred resolve?)");
        return;
    }
    GetModuleFileNameA(d3d8, g_detected_dll_path, sizeof(g_detected_dll_path));
    PFN_get_wrapper_info gwi = (PFN_get_wrapper_info)
        (void *)GetProcAddress(d3d8, "d3d8to11_get_wrapper_info");
    if (gwi) {
        unsigned int ver = 0;
        unsigned int magic = gwi(&ver);
        if (magic == 0xD3D81011u) {
            g_detected_stack = 1;
            g_detected_d3d8to11_version = ver;
            log_line("[pso_widescreen] detect_d3d8_stack: d3d8to11 (rhw-fix-v=%u) at %s",
                     ver, g_detected_dll_path);
            return;
        }
        log_line("[pso_widescreen] detect_d3d8_stack: gwi present but bad magic 0x%08X at %s",
                 magic, g_detected_dll_path);
        return;
    }
    // Legacy probe: did the older d3d8to11 export `d3d8to11_set_logical_canvas`
    // but no `_get_wrapper_info`? Treat as d3d8to11-old (canvas-hint era).
    if (GetProcAddress(d3d8, "d3d8to11_set_logical_canvas")) {
        g_detected_stack = 1;
        log_line("[pso_widescreen] detect_d3d8_stack: d3d8to11 (legacy, pre-rhw-fix) at %s",
                 g_detected_dll_path);
        return;
    }
    log_line("[pso_widescreen] detect_d3d8_stack: %s at %s",
             detected_stack_name(), g_detected_dll_path);
}

// ============================================================
// BORN-BORDERLESS WINDOW HOOK (anzz1 CreateWindowExA arg-override)
// ============================================================
// Replaces the old make_borderless() post-creation restyle. anzz1's approach:
// redirect psobb.exe's OWN CreateWindowExA call so the window is BORN as a
// full-monitor WS_POPUP — no caption ever exists, no SetWindowLongPtr/
// SWP_FRAMECHANGED churn, and the window's IME context is never detached.
//
// Engine call site (disasm-verified against the on-disk psobb.exe, md5 e5bdb901):
//   0x0082d1d6   ff 15 90 83 8f 00   call dword [0x008F8390]   ; user32!CreateWindowExA IAT
//   0x0082d1dc   a3 d8 be ac 00      mov  [0x00ACBED8], eax    ; store HWND -> g_hWnd
// The 4-byte operand (pointer-to-IAT-slot) lives at 0x0082D1D8. We rewrite it
// from &[0x008F8390] to &g_pCreateWindowExA (a variable holding our hook), so
// the engine's double-indirect `call dword [...]` lands on myCreateWindowExA
// with the original 12 args, which we rewrite then forward to the real export.
//
// Wrapper-agnostic: this is a plain Win32 .text operand patch, independent of
// crosire d3d8to9 / dgVoodoo / native d3d8 — none of them sit in the user32
// window-create path. One 4-byte write, applied once, value-guarded.

#define kCreateWindowExA_OperandVA  0x0082D1D8u  // operand of `call dword [...]` @0x0082D1D6
#define kCreateWindowExA_IATSlotVA  0x008F8390u  // expected current operand (user32!CreateWindowExA IAT)

typedef HWND (WINAPI *CreateWindowExA_t)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int,
                                         HWND, HMENU, HINSTANCE, LPVOID);

// Monitor selection (R1): the owner runs a virtual widescreen display that is
// NOT necessarily the Windows PRIMARY monitor, so we must NOT copy anzz1's
// MonitorFromWindow(GetDesktopWindow()) primary capture. Pick the monitor whose
// device-pixel rect matches the configured g_cfg Width/Height; fall back to the
// primary monitor rect if no match (or no cfg dims).
typedef struct {
    int   want_w, want_h;
    RECT  match;       // chosen monitor rect (valid if found)
    RECT  primary;     // primary monitor rect (always captured)
    int   found;
    int   have_primary;
} mon_pick_t;

static BOOL CALLBACK mon_pick_cb(HMONITOR hMon, HDC hdc, LPRECT lprc, LPARAM data)
{
    (void)hdc; (void)lprc;
    mon_pick_t *mp = (mon_pick_t *)data;
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(hMon, &mi)) return TRUE;
    const int mw = mi.rcMonitor.right  - mi.rcMonitor.left;
    const int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (mi.dwFlags & MONITORINFOF_PRIMARY) {
        mp->primary = mi.rcMonitor;
        mp->have_primary = 1;
    }
    if (!mp->found && mp->want_w > 0 && mp->want_h > 0 &&
        mw == mp->want_w && mh == mp->want_h) {
        mp->match = mi.rcMonitor;
        mp->found = 1;
    }
    return TRUE;  // keep enumerating so we always capture the primary too
}

// Resolve the target full-monitor rect. Returns 1 + fills *out on success.
static int resolve_target_monitor_rect(RECT *out)
{
    mon_pick_t mp; memset(&mp, 0, sizeof(mp));
    mp.want_w = g_cfg.width;
    mp.want_h = g_cfg.height;
    EnumDisplayMonitors(NULL, NULL, mon_pick_cb, (LPARAM)&mp);
    if (mp.found) {
        *out = mp.match;
        return 1;
    }
    if (mp.have_primary) {
        *out = mp.primary;
        return 1;
    }
    // Last resort: virtual-screen origin + SM_CXSCREEN/CYSCREEN.
    out->left = 0; out->top = 0;
    out->right  = GetSystemMetrics(SM_CXSCREEN);
    out->bottom = GetSystemMetrics(SM_CYSCREEN);
    return (out->right > 0 && out->bottom > 0);
}

// The engine's call operand is rewritten to &g_pCreateWindowExA; g_pCreateWindowExA
// holds the address of myCreateWindowExA (anzz1's exact double-indirection).
static HWND WINAPI myCreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style,
                                     int x, int y, int w, int h,
                                     HWND parent, HMENU menu, HINSTANCE inst, LPVOID param);
static CreateWindowExA_t g_pCreateWindowExA = &myCreateWindowExA;

static HWND WINAPI myCreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style,
                                     int x, int y, int w, int h,
                                     HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    // Always resolve the REAL export fresh (the IAT slot we redirected no longer
    // points at it). user32 is loaded long before the engine creates its window.
    CreateWindowExA_t real = (CreateWindowExA_t)GetProcAddress(
        GetModuleHandleA("user32.dll"), "CreateWindowExA");
    if (!real) {
        // Should never happen; degrade gracefully to the public API symbol.
        return CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    }

    // Item 6: WS_CHILD / embedded (harness/editor reparent) — pass through
    // untouched. A child must NEVER become a full-monitor WS_POPUP.
    // Item 7: gate — override only when widescreen is enabled AND windowed is
    // 0 (fullscreen) or 2 (borderless). windowed==1 (plain windowed) passes through.
    if (parent != NULL || !g_cfg.enabled ||
        (g_cfg.windowed != 0 && g_cfg.windowed != 2)) {
        return real(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    }

    RECT r;
    if (resolve_target_monitor_rect(&r)) {
        x = r.left;
        y = r.top;
        w = r.right  - r.left;
        h = r.bottom - r.top;
    }
    style = WS_POPUP;                 // born borderless — no caption ever
    ex    = WS_EX_APPWINDOW;          // show in taskbar / alt-tab (matches anzz1)
    log_line("[pso_widescreen] born-borderless: WS_POPUP %d,%d %dx%d (cls=%s windowed=%d)",
             x, y, w, h, cls ? cls : "(null)", g_cfg.windowed);
    return real(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
}

// Install: one-shot 4-byte operand patch at 0x0082D1D8 (verify == 0x008F8390
// before writing). Runs at ASI attach, before the engine creates its window.
static void install_window_hook(void)
{
    if (!g_cfg.enabled) {
        log_line("[pso_widescreen] born-borderless: SKIP (Enabled=0)");
        return;
    }
    DWORD *operand = (DWORD *)kCreateWindowExA_OperandVA;
    DWORD old;
    __try {
        if (VirtualProtect(operand, sizeof(DWORD), PAGE_EXECUTE_READWRITE, &old)) {
            if (*operand == kCreateWindowExA_IATSlotVA) {      // R5: verify before patch
                g_pCreateWindowExA = &myCreateWindowExA;
                *operand = (DWORD)(ULONG_PTR)&g_pCreateWindowExA;
                log_line("[pso_widescreen] born-borderless: CreateWindowExA operand "
                         "@0x%08X redirected (was 0x%08X)",
                         kCreateWindowExA_OperandVA, kCreateWindowExA_IATSlotVA);
            } else {
                log_line("[pso_widescreen] born-borderless: WARN operand @0x%08X = "
                         "0x%08X != 0x%08X — NOT patching",
                         kCreateWindowExA_OperandVA, *operand, kCreateWindowExA_IATSlotVA);
            }
            DWORD tmp; VirtualProtect(operand, sizeof(DWORD), old, &tmp);
        } else {
            log_line("[pso_widescreen] born-borderless: VirtualProtect FAILED @0x%08X err=%lu",
                     kCreateWindowExA_OperandVA, GetLastError());
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_line("[pso_widescreen] born-borderless: exception during install — skipped");
    }
}

// ============================================================
// CHAR-CREATE 16:9 — AUTHORITATIVE-SOURCE OWNERSHIP
// ============================================================
// Replaces the per-frame char-create frame-patch (cc_quad_fix_c @0x0082BB74 +
// cc_text_obs_c @0x0082B440 + the shared negative gate cc_scene_active) with full
// authoritative-source ownership. Every char-create content element the frame-patch
// moved is relocated at its TRUE static source (a .text bake immediate or a .rdata
// float), keyed so 4:3 (design_w==640) is a byte-identical no-op. 0x0082BB74 and
// 0x0082B440 are byte-stock (no char-create render-thread hooks), so ship-select
// cannot leak.
//
// All shifts derive from one quantity, evaluated once at apply time AFTER
// apply_anzz1_widescreen (anzz1 owns design_w, the backdrop table, and the
// wheel/info immediates):
// dw       = hs_peek_f32(0x0098A4B8)   design_w: 853.33 @16:9 hs1.0, 640 @4:3, 1280 @hs1.5
// span     = dw - 640.0                 213.33 @16:9 ; 0.0 @4:3  (HARD no-op key)
// stride_R = 0.75 * span                160.0 @16:9 hs1.0 (reproduces old cc_stride_right)
// At design_w==640, span==0 => stride_R==0 => every new value == stock => no write
// fires (cc_poke_imm_f32 is value-guarded). See _charcreate_source_ownership_spec.md.

// Forward decl: hs_peek_f32 is defined later (in the P5 D3 primitives block); the
// source bake below reads design_w through it. Without this prototype the call
// implicit-declares int-returning hs_peek_f32 (C4013) and dw reads garbage.
static float hs_peek_f32(uint32_t va);

// Value-guarded imm32 .text rewrite (mirrors hs_poke_f32 but for a code immediate /
// .rdata float). Idempotent: writes only on change; SEH-guarded; restores page
// protection. The "*p == stock" precondition means a re-run or a relocated reader
// can never corrupt a non-stock site.
static void cc_poke_imm_f32(uint32_t imm_va, float stock, float v)
{
    volatile float *p = (volatile float *)(uintptr_t)imm_va;
    __try {
        if (*p != stock) return;        // safety: only patch a site still at its stock imm
        if (*p == v) return;            // value-guard (no-op at 4:3 where v==stock)
        DWORD old;
        if (!VirtualProtect((LPVOID)(uintptr_t)imm_va, 4, PAGE_EXECUTE_READWRITE, &old)) return;
        *p = v;
        DWORD tmp; VirtualProtect((LPVOID)(uintptr_t)imm_va, 4, old, &tmp);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// apply_startup_bakes — THE one-shot front-end startup bake pass.
// ONE linear sweep of value-guarded source-immediate
// rewrites that own the front-end scenes' 16:9 horizontal layout, with NO scene
// gate and NO frame-patch: the engine re-bakes each cell from its source imm on
// every scene-build, and W1 (0x0082F309) is dead in-game, so a .text/.rdata
// immediate edit survives the front-end->in-game transition where a code-flow
// hook would be wiped.
//
// Called from apply_static_patches IMMEDIATELY AFTER apply_anzz1_widescreen
// (anzz1 owns design_w 0x0098A4B8 -> 853.333 @16:9 hs1.0 by the time we run).
// Gated on g_cfg.patch_charselect (the old PatchCharSelect=0 escape hatch is
// preserved, same gate the folded char-create + login writes used).
//
// 4:3 NO-OP BY CONSTRUCTION: at design_w==640 -> span==0 -> stride_R==0 and
// a_plus_10==650 -> every v collapses to its stock -> cc_poke_imm_f32's
// `if(*p==v)return` value-guard fires -> byte-identical stock at 640x480.
//
// GATED/owed items (selectgamen plates, char-create X1 0x0096E52C, char-select
// 0x004137DE final class, stats panel) are deliberately NOT included in this pass.
static void apply_startup_bakes(int render_w)
{
    if (!g_cfg.patch_charselect) {
        log_line("[pso_widescreen] startup-bakes: SKIP (PatchCharSelect=0)");
        return;
    }
    const float dw       = hs_peek_f32(0x0098A4B8u);   // design_w  (anzz1-owned, set first)
    // (char-create content-row stride is RUNTIME-ONLY with no static source -> not
    // baked/strided. See the Scene-09 note below.)



    // ---- Scene 09 char-create class-select CONTENT ROWS: runtime-only (NO bake) ----
    // The right-pane CONTENT rows (class header, description, 9-slice border, per-row
    // stat bars) have NO value-guard-bakeable static source — they are emitted by
    // shared generic widget renderers (e.g. sub_0071ff10 @0x0071FF10) whose design X
    // is a runtime heap field + offset, never an immediate. They are intentionally NOT
    // strided (no static source to bake); the class-select screen renders correctly.

    // ---- Scene 09 char-create class-select INFO BOX (the blue 9-slice panel): no
    // box-shift. Ephinea does NOT slide the class-info box for char-create; the box
    // rides design_w via the static `hud.w` widenings only.


    // ---- Lobby game-list "NN-NN" bar 4:3 right-ceiling -> design_w (16:9 full width) ----
    // (static RE, disasm-verified). The lobby NN-NN/game-list bar is emitted
    // by sub_00722010 -> sub_0082b558 -> sub_00836dc8. CONTRARY to the old "0x82b558
    // bypasses the affine" belief, the affine LOOP at 0x00836e02 (fmul [0x00ACC0E8])
    // DOES run for this quad: sub_0082b558 forwards a vertex DESCRIPTOR whose +0xC dword
    // is a vertex COUNT (the caller hard-sets it to 2: `mov eax,2; mov [esp+0x74],eax`
    // @0x721fa5/0x721fcf), and 0x836dc8 gates the affine on `cmp [eax+0xC],1; jle` =
    // count>1 -> runs. So the bar is NOT a bypass — it floats because its right-edge
    // vertex X is the literal 4:3 ceiling 640.0 (0x44200000 imm @0x00721FC0): 640*2.25
    // = 1440 of 1920 -> ~480px right gap. Raise that one imm to design_w; the SAME
    // in-game affine then maps it to render_w (dw*(render_w/dw)=render_w, so this is
    // HudScale-robust — the F_VPRESET affine just baked above and this share `dw`).
    // Patches ONE mov-imm operand (no shared cell -> leak-safe); value-guarded so 4:3
    // (dw==640) is a byte-identical no-op. The bar's left vertex X stays 0 (left edge).
    // VISUAL-PENDING: confirm at a live 16:9 lobby that the bar reaches the far-right
    // edge with no overshoot (server login was blocked when this shipped).
    cc_poke_imm_f32(0x00721FC0u, 640.0f, dw);     // NN-NN bar right-edge vertex X (853.33 @16:9 hs1.0)

    log_line("[pso_widescreen] startup-bakes: done (dw=%.3f; nnbar=dw)", dw);
}


// =====================================================================
// AD-SCREEN (patch-server news / status) LAYOUT — port of Trinity's
// AdDrawLineTask / PatchAdDrawLineTask (TrinityDLL resolution.cpp:39-69,
// resolution.h adDrawLineTaskOffsets + addrAdDrawLineTaskI 0x00408C9D).
//
// The psobb.io "patch server" news screen (MOTD box + the "Current Status" /
// "All Status" labels + the blue/red status bars) is laid out by a per-
// construction object. Its layout floats live at (obj - 0x5C04) + offset,
// authored in 4:3 design space. Stock leaves the status labels + bars at
// 640/480 anchors; in widescreen the labels stay top/left-anchored while
// the bars move -> "Current Status"/"All Status" end up UP in the news box
// instead of below their bars. We were MISSING this hook entirely (it touches
// no VA any other bake covers).
//
// Trinity hooks the ctor's `mov [0x00A3AD14], eax` (5 bytes a3 14 ad a3 00)
// and re-anchors each tagged float: axis 0 (X) = A-(640-f); axis 1 (Y) =
// C-(480-f). We mirror it with our design extents (A=design_w, C=design_h),
// held in g_ad_A/g_ad_C globals so the anchor space is LIVE-tunable
// (write_memory) without a rebuild while dialing in 16:9 hs2.0 parity. The
// hook site is a SEH-prologue (the ctor) => fires once per construction =>
// the in-place re-anchor never compounds. 4:3 / disabled => not installed.
// =====================================================================
#define AD_HOOK_VA       0x00408C9Du   // mov [0x00A3AD14], eax  (a3 14 ad a3 00)
#define AD_HOOK_LEN      5
#define AD_HOOK_RESUME   0x00408CA2u
#define AD_OBJ_BASE_OFF  0x5C04        // base = *0x00A3AD14 - 0x5C04 (Trinity)

// (offset_from_base, axis): axis 0 = X right-anchor, 1 = Y bottom-anchor.
static const struct { unsigned int off; unsigned char axis; } g_ad_offsets[] = {
    {0x0024,0},{0x0028,1},{0x0040,0},{0x0054,1},{0x2498,0},{0x251C,0},{0x25A0,0},
    {0x2600,1},{0x2610,1},{0x262C,1},{0x263C,1},{0x2658,1},{0x2664,0},{0x2668,1},
    {0x2684,1},{0x2694,1},{0x26B0,1},{0x26C0,1},{0x26DC,1},{0x26E8,0},{0x26EC,1},
    {0x34F4,1},{0x3608,0},{0x3628,1},{0x3638,1},{0x3654,1},{0x3664,1},{0x3680,1},
    {0x368C,0},{0x3690,1},{0x40D8,1},{0x40F4,1},{0x420C,1},{0x4228,1},
};
static float g_ad_A = 640.0f;          // design_w anchor space (set at install; live-tunable)
static float g_ad_C = 480.0f;          // design_h anchor space (set at install; live-tunable)
static void *g_ad_resume    = (void *)AD_HOOK_RESUME;
static int   g_ad_installed = 0;
static uint8_t g_ad_orig[AD_HOOK_LEN] = {0};

// C helper: re-anchor the freshly-constructed ad-screen object's layout floats.
static void ad_draw_line_patch(void)
{
    uint32_t objp = *(volatile uint32_t *)(uintptr_t)0x00A3AD14u;
    if (!objp) return;
    uint8_t *base = (uint8_t *)(uintptr_t)(objp - AD_OBJ_BASE_OFF);
    const float A = g_ad_A, C = g_ad_C;
    const int n = (int)(sizeof(g_ad_offsets) / sizeof(g_ad_offsets[0]));
    for (int i = 0; i < n; ++i) {
        float *p = (float *)(base + g_ad_offsets[i].off);
        __try {
            float f = *p;
            *p = (g_ad_offsets[i].axis == 0) ? (A - (640.0f - f))
                                             : (C - (480.0f - f));
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* skip on a mismatched build */ }
    }
}

// Naked stub: re-issue the hooked `mov [0x00A3AD14], eax`, re-anchor, resume.
__declspec(naked) static void ad_draw_line_stub(void)
{
    __asm {
        mov   dword ptr ds:[0x00A3AD14], eax     // original instruction (store obj ptr)
        pushad
        call  ad_draw_line_patch
        popad
        jmp   dword ptr ds:[g_ad_resume]          // -> 0x00408CA2
    }
}

// Install the ad-screen hook. Sig-guarded + idempotent; 4:3 / disabled no-op.
static void patch_ad_draw_line(const ws_scale_ctx *s)
{
    if (g_ad_installed) return;
    if (!s->enabled || s->A <= 640.0f) {
        log_line("[pso_widescreen] ad-screen: SKIP (4:3 / disabled)");
        return;
    }
    static const uint8_t kExpected[AD_HOOK_LEN] = { 0xA3, 0x14, 0xAD, 0xA3, 0x00 };
    const uint8_t *p = (const uint8_t *)(uintptr_t)AD_HOOK_VA;
    for (int i = 0; i < AD_HOOK_LEN; ++i) {
        if (p[i] != kExpected[i]) {
            log_line("[pso_widescreen] ad-screen: sig mismatch byte %d got 0x%02x "
                     "expected 0x%02x — refusing to hook", i, p[i], kExpected[i]);
            return;
        }
        g_ad_orig[i] = p[i];
    }
    g_ad_A = s->A;   // design_w (1706.67 @hs2.0); live-tunable via write_memory(&g_ad_A)
    g_ad_C = s->C;   // design_h (960 @hs2.0)
    DWORD old = 0;
    if (!VirtualProtect((LPVOID)(uintptr_t)AD_HOOK_VA, AD_HOOK_LEN,
                        PAGE_EXECUTE_READWRITE, &old)) {
        log_line("[pso_widescreen] ad-screen: VirtualProtect FAILED err=%lu", GetLastError());
        return;
    }
    uint8_t patch[AD_HOOK_LEN];
    patch[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)&ad_draw_line_stub - (AD_HOOK_VA + 5));
    memcpy(patch + 1, &rel, 4);
    memcpy((void *)(uintptr_t)AD_HOOK_VA, patch, AD_HOOK_LEN);
    DWORD tmp = 0;
    VirtualProtect((LPVOID)(uintptr_t)AD_HOOK_VA, AD_HOOK_LEN, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)(uintptr_t)AD_HOOK_VA, AD_HOOK_LEN);
    g_ad_installed = 1;
    log_line("[pso_widescreen] ad-screen: AdDrawLineTask hooked @0x00408C9D "
             "(A=%.1f C=%.1f, %d offsets)", (double)g_ad_A, (double)g_ad_C,
             (int)(sizeof(g_ad_offsets) / sizeof(g_ad_offsets[0])));
}


// ============================================================
// THE TWO APPLY HELPERS
// ============================================================
// ONE universal scaling gate (g_scale.enabled, checked by the caller), ONE
// static-patch helper, ONE engine-patch helper. No widescreen_engine selector
// branch anywhere. Per-feature knobs survive as internal early-return guards.

// apply_static_patches — every .data / .text-immediate / NOP write, in ONE
// linear pass, each idempotent + value/sig-guarded. ORDER: (1) the anzz1
// coordinate bake FIRST (the source of truth; one-shot `applied`-guarded),
// then (2) patch_minimap_layout (bake-on-top of anzz1's 0xA11324/E8), then the
// engine-independent companions (each keeps its per-feature knob). splash-phase2
// is de-conflicted vs anzz1 inside the function (skips the 2 shared x2 VAs).
/* ============================================================================
 *  UNIFIED kBakes[] TABLE + CORE
 *  ----------------------------------------------------------------------------
 *  ui_coord() is defined far above (~L2019); no forward decl needed.
 *  apply_special() is defined just below apply_bakes() but called by it. */
static void apply_special(const ws_scale_ctx *s);   /* fwd: apply_bakes calls it */

/* FIX A — cutscene/dialogue letterbox origin-overscan magnitude (design-space px).
   The 4 un-baked corner cells (TOP.X/TOP.Y/BOT.X + TOP.H) are pushed this many px
   PAST the top/left canvas edge so the in-game affine's positive/half-pixel-rounded
   OFFSET_X/Y can't leave an uncovered strip where the 3D scene leaks through. The
   bar interiors are pure black, so over-extending off-canvas is harmless.
   TUNABLE: UNVERIFIED-LIVE — the main agent must read [0xACC0F0]/[0xACC0F4] (OFFSET_X/Y)
   during an in-game cutscene at 16:9 and size this to >= max(OFFSET_X,OFFSET_Y); 32 is a
   safe guess. If the leak persists, bump it; if a bottom/right strip ever appears, this
   is NOT the cause (those edges over-cover via the existing hud.w / BOT.Y-deanchor rows). */
#define FIXA_LETTERBOX_OVERSCAN_PX  32.0f

static const bake_t kBakes[] = {
/* ===== kBakes[] — 780 authoritative rows (anzz1 folded + patch_* extracted) =====
   (ANZZ1 565 + EPHINEA 148 + TRINITY 56 + OURS 11; the per-source subheaders below
   predate recent additions and may read low — the ARRAYLEN macro is authoritative.) */
/* fields: va, kind, base, coeff, offset, src, gate, stock, note, base2 */
  /* ---- SRC_ANZZ1 : 566 rows ---- */
  { 0x004011C0, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x009A3844, K_U32, B_D, 3.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A384C, K_U32, B_D, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3858, K_U32, B_D, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3860, K_U32, B_D, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A386C, K_U32, B_D, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3874, K_U32, B_D, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3888, K_U32, B_D, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3894, K_U32, B_D, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A389C, K_U32, B_D, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A38B0, K_U32, B_D, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A38BC, K_U32, B_D, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A38C4, K_U32, B_D, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A38D8, K_U32, B_D, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x004011D2, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x004011DD, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x004011EF, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00407F9D, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00409F4B, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0040A016, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0040A02B, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0040A036, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0040A04B, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0040C445, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0040C469, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0040C48B, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00453809, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0045381D, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00453827, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0045383B, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00483278, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00489E53, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x004EB4AA, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x004EB4B4, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x004EB4F0, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x004EB4FA, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x004EC2D1, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x004EC2E9, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x004EF592, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x004EF59C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00502D7D, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00502D85, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x005BC90E, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x005BD783, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x005BDB89, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0066DFEF, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x0066DFFA, K_ADD, B_C, 0.5f, -240.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.vcenter", B_LIT },
  { 0x006F4922, K_SET, B_A, 0.5f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x006F4936, K_SET, B_C, 0.5f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x006F4CF2, K_SET, B_B, 0.5f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x006F4CFA, K_SET, B_D, 0.4765625f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x006F4D02, K_SET, B_B, 2.5f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x006F4D0A, K_SET, B_D, 2.4765625f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x006F4D4E, K_SET, B_B, 2.5f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x006F4D56, K_SET, B_D, 0.4765625f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x006F4D5E, K_SET, B_B, 4.5f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x006F4D66, K_SET, B_D, 2.4765625f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x006F4D9F, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x006FFCB8, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x006FFCE0, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x006FFCF4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x006FFD12, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x006FFD1C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x006FFD58, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x006FFD76, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D2CE, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D2EC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D2F6, K_SET, B_A, 1.0f, -384.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x0070D30A, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D328, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D346, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D350, K_SET, B_A, 1.0f, -384.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x0070D364, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D382, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D3A0, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D3B4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D3D2, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D3F0, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x0070D404, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom.movs", B_LIT },
  { 0x007126A5, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x007126AC, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x007165B8, K_SET, B_A, 1.0f, -16.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x00719AB1, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00719ACE, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00719AD9, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00719AF6, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00719C08, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00719C16, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00719C5C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00719C6B, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00719C79, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00719C8A, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00719D44, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00719D53, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00719E84, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0071A21F, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0071A226, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0071A8F8, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0071A905, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0071A988, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0071A995, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00721E6C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00721E7A, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00721F2A, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x007403F6, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0074467D, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0074468D, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x007446D8, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x007446E8, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00744723, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00744733, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0074477E, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0074478E, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x007583C8, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x007583E3, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x007583FE, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00758412, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x007584EE, K_SET, B_A, 1.0f, 10.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x007584F5, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0075850F, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0075851F, K_SET, B_A, 1.0f, 10.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x00758526, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0075853F, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00761CC7, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00761CCC, K_SET, B_A, 0.5f, -110.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x00761CD9, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00761CDE, K_SET, B_A, 0.5f, -110.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x00762367, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0076236C, K_SET, B_A, 0.5f, -180.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x007625F8, K_SET, B_A, 0.5f, -4.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x00762602, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x007627CF, K_SET, B_A, 0.5f, -4.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x007627DF, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0077F2ED, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0077F30D, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0077F386, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0077F3A7, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0077F3B2, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0077F3D2, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00783A92, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x00783AAA, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00785641, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00785648, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00785680, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x007856B1, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x007856B8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00785705, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00785BD2, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00785BD9, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00785C11, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00785C49, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x007888A6, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x007888C6, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x007888D1, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x007888F1, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00788EA6, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00788EC6, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00788ED1, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00788EF1, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00791C7A, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00792366, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00804EE0, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00804EF4, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x008051BA, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008051CE, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0082EC74, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x0082ED43, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x0082EF4C, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x0082F018, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x0082F700, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x0082F7DB, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x008F8494, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F853C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F854C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F87D4, K_SET, B_A, 1.0f, -63.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x008F87D8, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x008F87E0, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x008F87E4, K_SET, B_A, 1.0f, -63.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x008F8EAC, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F8EBC, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F8F50, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F9A94, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F9A98, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x008F9AA4, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F9AC4, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F9AC8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x008F9D38, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F9D68, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F9E8C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F9EF8, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F9F2C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008F9F34, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  /* ---- FIX A — cutscene/dialogue letterbox origin overscan (kills the top/left 3D sliver) ----
     QuestLoadingScreenBlackBarObject_Render 0x0040C1E8 (mode [this+0x1c]==1) reads the two-bar
     design rect from the SoA at 0x008F9F2C. The 4 ORIGIN cells (TOP.X/TOP.Y/BOT.X + TOP.H) were
     un-baked, so the affine maps the top-left bar corner to (OFFSET_X,OFFSET_Y) and any positive /
     fistp-rounded offset leaves an uncovered top/left strip. Push the origins -OVERSCAN px and grow
     TOP.H by OVERSCAN so the bar's INNER edge (design y=64) is unchanged — the visible band
     thickness is identical, only the off-canvas overhang grows. GATE_ALWAYS; pure-black interior
     makes the off-canvas overhang harmless. The W rows above already widen to design_w (>704),
     so the left -OVERSCAN does NOT uncover the right (it still over-covers). BOT.Y origin is the
     separate eph.92va.l5.hadd.raw deanchor row (0x008F9F48, different VA — no conflict).
     stock=0x00000000 on the 3 origin cells == their literal 0.0f stock => sig-guard unchecked,
     idempotent-guarded (live==want skip). TOP.H stock=0x42800000 (64.0) IS sig-guarded.
     NOTE (4:3): at 640x480 the affine is identity so -OVERSCAN maps to screen <0 (off-screen) and
     the visible band 0..64 is identical to stock, even though the design cell value differs. */
  { 0x008F9F3C, K_SET, B_LIT, 0.0f, -FIXA_LETTERBOX_OVERSCAN_PX,        SRC_OURS, GATE_ALWAYS, 0x00000000, "letterbox.TOP.X overscan", B_LIT },
  { 0x008F9F40, K_SET, B_LIT, 0.0f, -FIXA_LETTERBOX_OVERSCAN_PX,        SRC_OURS, GATE_ALWAYS, 0x00000000, "letterbox.TOP.Y overscan", B_LIT },
  { 0x008F9F44, K_SET, B_LIT, 0.0f, -FIXA_LETTERBOX_OVERSCAN_PX,        SRC_OURS, GATE_ALWAYS, 0x00000000, "letterbox.BOT.X overscan", B_LIT },
  { 0x008F9F30, K_SET, B_LIT, 0.0f,  64.0f + FIXA_LETTERBOX_OVERSCAN_PX, SRC_OURS, GATE_ALWAYS, 0x42800000, "letterbox.TOP.H 64->64+overscan (origin moved up)", B_LIT },
  { 0x008F9F58, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008FA8A8, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008FAE50, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008FAE90, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x008FFB80, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x008FFB84, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x009006F4, K_U32, B_RW, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.w", B_LIT },
  { 0x009006F8, K_U32, B_RH, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.h", B_LIT },
  { 0x009006FC, K_U32, B_RW, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.w", B_LIT },
  { 0x00900700, K_U32, B_RH, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.h", B_LIT },
  { 0x00900704, K_U32, B_RW, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.w", B_LIT },
  { 0x00900708, K_U32, B_RH, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.h", B_LIT },
  { 0x0090070C, K_U32, B_RW, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.w", B_LIT },
  { 0x00900710, K_U32, B_RH, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.h", B_LIT },
  { 0x00900714, K_U32, B_RW, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.w", B_LIT },
  { 0x00900718, K_U32, B_RH, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.h", B_LIT },
  { 0x0090071C, K_U32, B_RW, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.w", B_LIT },
  { 0x00900720, K_U32, B_RH, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "res.h", B_LIT },
  { 0x009007B8, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00901288, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x00909164, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00909168, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00909D70, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00909D84, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00909DA8, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0091D8B8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0091D8BC, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0091DA7C, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0091DA80, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0091E19C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0091FBEC, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0091FBF0, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0091FC50, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0091FC54, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0091FC70, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0091FC74, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0091FF90, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0091FF94, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  /* StageC_tail — the in-game menu-dim / letterbox-bar GEOMETRY EXTENT (Ephinea
     FUN_52da7ff0 edge/frame). These 4 .data floats are the right/bottom corners of
     the fullscreen dark quad, expressed in DISPLAY pixels (render_w/h), NOT 4:3 design:
       0x0091FF30 / 0x0091FF00 : displayW + 100  (stock 740 = 640+100 @4:3 -> no-op)
       0x0091FF2C / 0x0091FEFC : displayH + 100  (stock 580 = 480+100 @4:3 -> no-op)
     With StageC_color (above) this is THE bottom-bar fix: the bar's extent now reaches
     the true monitor edge instead of being sized off a 4:3 ceiling (over-tall black box).
     B_RW/B_RH = render px (the logical/physical canvas == Ephinea's displayW/H). */
  { 0x0091FF30, K_SET, B_RW, 1.0f, 100.0f, SRC_EPHINEA, GATE_ALWAYS, 0x44390000, "StageC_tail.rect.r", B_LIT },
  { 0x0091FF00, K_SET, B_RW, 1.0f, 100.0f, SRC_EPHINEA, GATE_ALWAYS, 0x44390000, "StageC_tail.rect.r", B_LIT },
  { 0x0091FF2C, K_SET, B_RH, 1.0f, 100.0f, SRC_EPHINEA, GATE_ALWAYS, 0x44110000, "StageC_tail.rect.b", B_LIT },
  { 0x0091FEFC, K_SET, B_RH, 1.0f, 100.0f, SRC_EPHINEA, GATE_ALWAYS, 0x44110000, "StageC_tail.rect.b", B_LIT },
  { 0x00920050, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00920104, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00920108, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00920160, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00920164, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0092B6EC, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0092B71C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0092B740, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0092B780, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0096E534, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0096E53C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0096E54C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0096E574, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0096E5FC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0096E660, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0096F5D8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0096F5E0, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0096F694, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0096F698, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0096FC44, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0096FC54, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0096FE84, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0096FF5C, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0096FF60, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0096FF9C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0096FFA8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0096FFBC, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0096FFC0, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x009701EC, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x009701F0, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097061C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00970638, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00970640, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00970648, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00970660, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097066C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009706C0, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x009706C8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x009707F4, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x009707F8, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00970808, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097080C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x009710BC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F44, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F4C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F54, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F5C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F64, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F6C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F74, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F7C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F84, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F8C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F94, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971F9C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FA4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FAC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FBC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FC4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FCC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FD4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FDC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FE4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FEC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FF4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00971FFC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972000, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972004, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097200C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972010, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x00972014, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972024, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972040, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972048, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972050, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x00972054, K_ADD, B_C, 0.5f, -240.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.vcenter", B_LIT },
  { 0x00972058, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x0097205C, K_ADD, B_C, 0.5f, -240.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.vcenter", B_LIT },
  { 0x00972060, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x0097206C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972078, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x00972094, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097209C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009720A4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009720A8, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x009720B0, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x009720B4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009720B8, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x009720CC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009720E0, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x009720E4, K_ADD, B_C, 0.5f, -240.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.vcenter", B_LIT },
  { 0x009720E8, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x009720EC, K_ADD, B_C, 0.5f, -240.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.vcenter", B_LIT },
  { 0x009720F0, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x009720F4, K_ADD, B_C, 0.5f, -240.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.vcenter", B_LIT },
  { 0x009720F8, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x009720FC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972100, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972104, K_ADD, B_C, 0.5f, -240.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.vcenter", B_LIT },
  { 0x00972188, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972224, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097222C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972234, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097223C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972244, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097224C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972254, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097225C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972264, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097227C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972284, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009722DC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009722E4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009722EC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009722F4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009722FC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972304, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097230C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972314, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097231C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972324, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097232C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972334, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097233C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972344, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097234C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972354, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097235C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972364, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097236C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972374, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097237C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972384, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097238C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972394, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097239C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723A4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723AC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723B4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723BC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723C4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723CC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723D4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723DC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723E4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723EC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723F4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009723FC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972404, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097240C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972414, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097241C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972424, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097242C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972434, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097243C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972444, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097244C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972454, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097245C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972464, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097246C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972474, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097247C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972484, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097248C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972494, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097249C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724A4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724AC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724B4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724BC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724C4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724CC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724D4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724DC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724E4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724EC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724F4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009724FC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972504, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972508, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972518, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972530, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972540, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x00972548, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972550, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972554, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097255C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972564, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097256C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972570, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972578, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972580, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x0097258C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972594, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097259C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009725A4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009725AC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009725B4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009725BC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009725C4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009725CC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009725D0, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x009725D4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009725E4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009725E8, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x009725EC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972610, K_ADD, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.full", B_LIT },
  { 0x00972614, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x0097261C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00972620, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972698, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00972700, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  /* HANGAME title-menu (Login/Start Game/Exit Game) position struct {X@+0,Y@+4}.
     At stock 4:3 (210/340) the menu sits left-of-center behind the logo, so we
     center-anchor X / bottom-anchor Y — identical transform to the live-verified
     E3C/E40 siblings below. Single private xref (push @0x00761C63), so GATE_ALWAYS is
     a dead write on non-hangame boots; 4:3-safe (deltas are 0 at design 640x480).
     (Ephinea's byte-delta has no row here — it never uses the Korean Hangame login.) */
  { 0x00974E08, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hangame.menu.x", B_LIT },
  { 0x00974E0C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hangame.menu.y", B_LIT },
  { 0x00974E3C, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00974E40, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00974E44, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  { 0x00974E48, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00978454, K_SET, B_A, 1.0f, -640.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x00978458, K_SET, B_A, 1.0f, -860.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009796EC, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x009796F4, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00979A28, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00979A34, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00979A70, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00979A7C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00979A8C, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00979A9C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00979DC4, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x00979DC8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00979DFC, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00979E00, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097A16C, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097A170, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097A1A8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097A1AC, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097A1B8, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097A1BC, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097A6F0, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097A6F4, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097A73C, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097A740, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097BB0C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097BB58, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097BBB0, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097E9A8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097E9D8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097E9E4, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097E9F0, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097EA00, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097EBB8, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097EBBC, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097EC18, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097EC1C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0097EC48, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0097EC4C, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0098A15C, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0098A160, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0098A494, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x0098A4A4, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0098A4A8, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0098A4B4, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x0098A4B8, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x0098A4DC, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x0098A500, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x009A3468, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x009A3488, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x009A3840, K_U32, B_BCEIL, 4.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3848, K_U32, B_BCEIL, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3854, K_U32, B_BCEIL, 4.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A385C, K_U32, B_BCEIL, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3868, K_U32, B_BCEIL, 4.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3870, K_U32, B_BCEIL, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A387C, K_U32, B_BCEIL, 4.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3884, K_U32, B_BCEIL, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A3898, K_U32, B_BCEIL, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A38AC, K_U32, B_BCEIL, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A38B8, K_U32, B_BCEIL, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A38C0, K_U32, B_BCEIL, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A38CC, K_U32, B_BCEIL, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A38D4, K_U32, B_BCEIL, 2.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "atlas", B_LIT },
  { 0x009A6900, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x009A6908, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x009A690C, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x009A6914, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x009B8D0C, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x009B8D54, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  /* 0x009B8DC4 = char-select right-gradient width (Ephinea csel.grad.w). Static .data
     float, stock 0x44200000 (640.0); widen to design_w. Was an anzz1 GATE_ALWAYS "hud.w"
     row writing the identical value — re-attributed/gated to Ephinea's char-select delta
     (same written value, now stock-guarded). 0x009B8D08/D50 are NOT here: they're written
     at runtime by the .text imms at 0x004EB4AA/0x004EB4F0 (already in kBakes as hud.w). */
  { 0x009B8DC4, K_SET, B_A, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x44200000, "csel.grad.w", B_LIT },
  /* StageC_color — the in-game menu-dim / letterbox LETTERBOX-BAR color (Ephinea
     FUN_52da7ff0 writes these 6 .data DWORDs). ARGB (D3DCOLOR 0xAARRGGBB). RGB is
     ALREADY dark-gray 0x181818 in stock; Ephinea only raises/normalizes the ALPHA:
       0x009B8DB4 / 0x009B8DC0 : 0xEC181818 -> 0xFF181818  (fully opaque)
       0x009B8DB8 / DBC / DE8 / DEC : 0xFF181818 -> 0xFA181818 (alpha 250)
     GATE_ALWAYS (StageC runs in-game) — this is the row that turns our pure-black
     over-scaled bottom bar into Ephinea's correct dark-gray letterbox. K_U32 literal
     SET (offset == the exact target u32; round-trips bit-exact through float because the
     low 0x181818 fits the f32 mantissa at this magnitude — verified). stock = the stock
     u32 for the sig-guard. These SUPERSEDE the 6 prior GATE_CHARSELECT alpha-byte pokes
     (0x009B8DB7/DBB/DBF/DC3/DEB/DEF) which touched only the alpha of these same dwords. */
  { 0x009B8DB4, K_U32, B_LIT, 0.0f, 4279769112.0f, SRC_EPHINEA, GATE_ALWAYS, 0xEC181818, "StageC_color", B_LIT },  /* ->0xFF181818 */
  { 0x009B8DC0, K_U32, B_LIT, 0.0f, 4279769112.0f, SRC_EPHINEA, GATE_ALWAYS, 0xEC181818, "StageC_color", B_LIT },  /* ->0xFF181818 */
  { 0x009B8DB8, K_U32, B_LIT, 0.0f, 4195883032.0f, SRC_EPHINEA, GATE_ALWAYS, 0xFF181818, "StageC_color", B_LIT },  /* ->0xFA181818 */
  { 0x009B8DBC, K_U32, B_LIT, 0.0f, 4195883032.0f, SRC_EPHINEA, GATE_ALWAYS, 0xFF181818, "StageC_color", B_LIT },  /* ->0xFA181818 */
  { 0x009B8DE8, K_U32, B_LIT, 0.0f, 4195883032.0f, SRC_EPHINEA, GATE_ALWAYS, 0xFF181818, "StageC_color", B_LIT },  /* ->0xFA181818 */
  { 0x009B8DEC, K_U32, B_LIT, 0.0f, 4195883032.0f, SRC_EPHINEA, GATE_ALWAYS, 0xFF181818, "StageC_color", B_LIT },  /* ->0xFA181818 */
  { 0x009B8DDC, K_SET, B_A, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.w", B_LIT },
  { 0x009D0040, K_SET, B_A, 0.5f, -128.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009D0044, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009F0ADC, K_SET, B_A, 1.0f, -157.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009F0AF4, K_SET, B_A, 1.0f, -288.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009F0B0C, K_SET, B_A, 1.0f, -157.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009F0B24, K_SET, B_A, 1.0f, -288.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009F0B3C, K_SET, B_A, 1.0f, -128.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009F0B5C, K_SET, B_A, 1.0f, -128.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009FF080, K_SET, B_A, 1.0f, -144.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009FF084, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009FF098, K_SET, B_A, 1.0f, -144.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009FF09C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009FF0B0, K_SET, B_A, 1.0f, -16.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009FF0B4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009FF0C8, K_SET, B_A, 1.0f, -16.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009FF0CC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009FF0E0, K_SET, B_A, 1.0f, -272.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009FF0E4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009FF0F8, K_SET, B_A, 1.0f, -272.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009FF0FC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009FF110, K_SET, B_A, 1.0f, -143.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009FF114, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009FF128, K_SET, B_A, 1.0f, -143.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009FF12C, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009FF1A8, K_SET, B_A, 1.0f, -144.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "anzz1.hard", B_LIT },
  { 0x009FF1AC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x009FF50C, K_ADD, B_A, 0.5f, -320.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.half", B_LIT },
  /* minimap viewport (idx0) X/Y are NOT deanchored here — apply_special() re-derives
     an ABSOLUTE corner anchor from native fractions every pass (size + position). */
  /* type-0 compass/player icon off-X (0x00A11390 / 0x00A11398) are NOT deanchored
     here either: the old "deanchor.full" rows only ever moved X (never Y), so the
     icons could never TRACK a relocated box. apply_special() now owns all four
     type-0 X/Y icon slots (0x00A11390/94/98/9C) as the SINGLE writer, deriving them
     from the box origin so the compass/player stay glued to the map. Removing these
     two K_ADD rows eliminates the apply_bakes-vs-apply_special write fight. */
  { 0x00A113A4, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  { 0x00A113AC, K_ADD, B_C, 1.0f, -480.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "deanchor.bottom", B_LIT },
  /* minimap background (idx0) X/Y are NOT deanchored here — see apply_special(). */
  { 0x00A33CD0, K_AR, B_AR, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "ar", B_LIT },
  { 0x00A98498, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00A984B4, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00A984D0, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00A984EC, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00A98508, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  { 0x00A98524, K_SET, B_C, 1.0f, 0.0f, SRC_ANZZ1, GATE_ALWAYS, 0x00000000, "hud.h", B_LIT },
  /* ---- SRC_EPHINEA : 81 rows ---- */
  { 0x004137D0, K_SET, B_A, 0.5f, -21.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x00000000, "csel.footer.cancel.x (.text imm; stock299 + (A-640)/2)", B_LIT },
  { 0x008F9F18, K_SET, B_A, 0.5f, -289.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x41F80000, "csel.banner.x (SELECT CHARACTER origin; stock31 + (A-6", B_LIT },
  { 0x008FA024, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row0 (1.0->hud)", B_LIT },
  { 0x008FA02C, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row1 (1.0->hud)", B_LIT },
  { 0x008FA030, K_SET, B_A, 0.5f, 275.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4414C000, "csel.xhalf 595+half (=595+A/2-320)", B_LIT },
  { 0x008FA034, K_SET, B_C, 0.5f, 1.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43710000, "csel.yhalfh 241+dh/2 (=241+C/2-240)", B_LIT },
  { 0x008FA038, K_SET, B_A, 0.5f, 275.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4414C000, "csel.xhalf 595+half", B_LIT },
  { 0x008FA03C, K_SET, B_C, 0.5f, 78.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x439F0000, "csel.yhalfh 318+dh/2 (=318+C/2-240)", B_LIT },
  { 0x008FA044, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row2 (1.0->hud)", B_LIT },
  { 0x008FA04C, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row3 (1.0->hud)", B_LIT },
  { 0x008FA050, K_SET, B_A, 0.5f, 223.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4407C000, "csel.xhalf 543+half (=543+A/2-320)", B_LIT },
  { 0x008FA054, K_SET, B_C, 0.5f, 1.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43710000, "csel.yhalfh 241+dh/2", B_LIT },
  { 0x008FA058, K_SET, B_A, 0.5f, 223.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4407C000, "csel.xhalf 543+half", B_LIT },
  { 0x008FA05C, K_SET, B_C, 0.5f, 78.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x439F0000, "csel.yhalfh 318+dh/2", B_LIT },
  { 0x008FA064, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row4 (1.0->hud)", B_LIT },
  { 0x008FA068, K_SET, B_KX, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.wmul.kx row0 (1.0->kx)", B_LIT },
  { 0x008FA074, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row5 (1.0->hud)", B_LIT },
  { 0x008FA078, K_SET, B_A, 0.5f, 275.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4414C000, "csel.xhalf 595+half", B_LIT },
  { 0x008FA07C, K_SET, B_C, 0.5f, -167.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42920000, "csel.yhalfh 73+dh/2 (=73+C/2-240)", B_LIT },
  { 0x008FA080, K_SET, B_A, 0.5f, 295.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4419C000, "csel.xhalf 615+half (=615+A/2-320)", B_LIT },
  { 0x008FA084, K_SET, B_C, 0.5f, -167.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42920000, "csel.yhalfh 73+dh/2", B_LIT },
  { 0x008FA088, K_SET, B_A, 0.5f, 275.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4414C000, "csel.xhalf 595+half", B_LIT },
  { 0x008FA08C, K_SET, B_C, 0.5f, 165.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43CA8000, "csel.yhalfh 405+dh/2 (=405+C/2-240)", B_LIT },
  { 0x008FA094, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row6 (1.0->hud)", B_LIT },
  { 0x008FA098, K_SET, B_KX, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.wmul.kx row1 (1.0->kx)", B_LIT },
  { 0x008FA0A4, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row7 (1.0->hud)", B_LIT },
  { 0x008FA0A8, K_SET, B_A, 0.5f, 223.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4407C000, "csel.xhalf 543+half", B_LIT },
  { 0x008FA0AC, K_SET, B_C, 0.5f, -167.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42920000, "csel.yhalfh 73+dh/2", B_LIT },
  { 0x008FA0B0, K_SET, B_A, 0.5f, 223.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4407C000, "csel.xhalf 543+half", B_LIT },
  { 0x008FA0B4, K_SET, B_C, 0.5f, -167.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42920000, "csel.yhalfh 73+dh/2", B_LIT },
  { 0x008FA0B8, K_SET, B_A, 0.5f, 223.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4407C000, "csel.xhalf 543+half", B_LIT },
  { 0x008FA0BC, K_SET, B_C, 0.5f, 165.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43CA8000, "csel.yhalfh 405+dh/2", B_LIT },
  { 0x008FA0C4, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row8 (1.0->hud)", B_LIT },
  { 0x008FA0C8, K_SET, B_KX, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.wmul.kx row2 (1.0->kx)", B_LIT },
  { 0x008FA0D4, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row9 (1.0->hud)", B_LIT },
  { 0x008FA0D8, K_SET, B_A, 0.5f, 139.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43E58000, "csel.panel.center.x (stock459 + half)", B_LIT },
  { 0x008FA0DC, K_SET, B_C, 0.5f, -167.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42920000, "csel.yhalfh 73+dh/2", B_LIT },
  { 0x008FA0E0, K_SET, B_A, 0.5f, 166.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43F30000, "csel.panel.center.x (stock486 + half)", B_LIT },
  { 0x008FA0E4, K_SET, B_C, 0.5f, -167.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42920000, "csel.yhalfh 73+dh/2", B_LIT },
  { 0x008FA0E8, K_SET, B_A, 0.5f, 139.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43E58000, "csel.panel.center.x (stock459 + half)", B_LIT },
  { 0x008FA0EC, K_SET, B_C, 0.5f, 165.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43CA8000, "csel.yhalfh 405+dh/2", B_LIT },
  { 0x008FA0F4, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row10 (1.0->hud)", B_LIT },
  { 0x008FA0F8, K_SET, B_KX, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.wmul.kx row3 (1.0->kx)", B_LIT },
  { 0x008FA104, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row11 (1.0->hud)", B_LIT },
  { 0x008FA108, K_SET, B_A, 0.5f, -289.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x41F80000, "csel.banner.x (stock31 + half)", B_LIT },
  { 0x008FA10C, K_SET, B_C, 0.5f, -167.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42920000, "csel.yhalfh 73+dh/2", B_LIT },
  { 0x008FA110, K_SET, B_A, 0.5f, -289.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x41F80000, "csel.banner.x (stock31 + half)", B_LIT },
  { 0x008FA114, K_SET, B_C, 0.5f, -167.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42920000, "csel.yhalfh 73+dh/2", B_LIT },
  { 0x008FA118, K_SET, B_A, 0.5f, -289.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x41F80000, "csel.banner.x (stock31 + half)", B_LIT },
  { 0x008FA11C, K_SET, B_C, 0.5f, 165.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43CA8000, "csel.yhalfh 405+dh/2", B_LIT },
  { 0x008FA124, K_SET, B_KX, 410.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43CD0000, "csel.xkx 410*kx", B_LIT },
  { 0x008FA128, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row12 (1.0->hud)", B_LIT },
  { 0x008FA12C, K_SET, B_KX, 214.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43560000, "csel.xkx 214*kx", B_LIT },
  { 0x008FA130, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row13 (1.0->hud)", B_LIT },
  { 0x008FA138, K_SET, B_C, 1.0f, -24.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43E40000, "csel.ydh 456+dh (=456+C-480)", B_LIT },
  { 0x008FA13C, K_SET, B_KX, 426.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43D50000, "csel.xkx 426*kx", B_LIT },
  { 0x008FA140, K_SET, B_C, 1.0f, -55.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43D48000, "csel.ydh 425+dh (=425+C-480)", B_LIT },
  { 0x008FA144, K_SET, B_KX, 213.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43550000, "csel.xkx 213*kx", B_LIT },
  { 0x008FA148, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row14 (1.0->hud)", B_LIT },
  { 0x008FA14C, K_SET, B_KX, 411.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43CD8000, "csel.xkx 411*kx", B_LIT },
  { 0x008FA150, K_SET, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x3F800000, "csel.hmul row15 (1.0->hud)", B_LIT },
  { 0x008FA15C, K_SET, B_KX, 229.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43650000, "csel.xkx 229*kx", B_LIT },
  { 0x008FA1A4, K_SET, B_KX, 15.6f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x4179999A, "csel.xkx 15.6*kx", B_LIT },
  { 0x008FA1AC, K_SET, B_C, 0.5f, -168.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42900000, "csel.yhay (72+dh/2)*affine_y — want=(72+(C-480)/2)*aff", B_AFFINEY },
  { 0x008FA1C0, K_SET, B_KX, 462.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43E70000, "csel.xkxay 462*kx*affine_y — TWO runtime bases; want=4", B_AFFINEY },
  { 0x008FA1C4, K_SET, B_AFFINEY, 50.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42480000, "csel.ay 50*affine_y", B_LIT },
  { 0x008FA1CC, K_SET, B_AFFINEY, 40.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42200000, "csel.ay 40*affine_y", B_LIT },
  { 0x008FA1D0, K_SET, B_C, 0.5f, -141.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42C60000, "csel.yhay (99+dh/2)*affine_y — want=(99+(C-480)/2)*aff", B_AFFINEY },
  { 0x008FA1D4, K_SET, B_AFFINEY, 16.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x41800000, "csel.ay 16*affine_y", B_LIT },
  { 0x008FA1D8, K_SET, B_A, 0.5f, 235.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x440AC000, "csel.xhay (555+half)*affine_y — want=(555+(A-640)/2)*a", B_AFFINEY },
  { 0x008FA528, K_SET, B_A, 1.0f, -214.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43D50000, "csel.fgright.questslider.x (stock426 + (A-640)); hs1.0", B_LIT },
  { 0x008FA52C, K_SET, B_C, 0.5f, 10.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x437A0000, "csel.details.pane.y 250+(C-480)/2 (=250+C/2-240) MOD_Y", B_LIT },
  { 0x008FABF8, K_SET, B_A, 1.0f, -208.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43D80000, "csel.fgright.dressroom-confirm.x (stock432 + (A-640));", B_LIT },
  { 0x008FAC00, K_SET, B_A, 1.0f, -212.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43D60000, "csel.fgright.sibling.x (stock428 + (A-640)); hs1.0 dR=", B_LIT },
  { 0x008FAC08, K_SET, B_A, 1.0f, -212.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43D60000, "csel.fgright.recreate-confirm.x (stock428 + (A-640)); ", B_LIT },
  { 0x008FADC8, K_SET, B_A, 0.5f, -256.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42800000, "csel.banner.x (stock64 + half)", B_LIT },
  { 0x0091D988, K_ADD, B_A, 1.0f, -640.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x44178000, "dr.toprow.ok_back.x  WRITTEN = stock(606.0)+(s->A-640); B_A(=gameRenderW)=Ephinea Loop4_Wadd_full; B_ANATIVE(853)=under-move bug", B_LIT },
  { 0x0091DC74, K_ADD, B_A, 1.0f, -640.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x44170000, "dr.charname.field.x  WRITTEN = stock(604.0)+(s->A-640); B_A=Ephinea Loop4_Wadd_full", B_LIT },
  { 0x0091DD1C, K_ADD, B_A, 1.0f, -640.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x44198000, "dr.botrow.ok_back.x  WRITTEN = stock(614.0)+(s->A-640); B_A=Ephinea Loop4_Wadd_full", B_LIT },
  { 0x0096E27C, K_SET, B_HUDSCALE, 165.0f, 0.0f, SRC_EPHINEA, GATE_STREAK_SCALE, 0x42F80000, "streak.scale.x = 165*hud_scale (cancels 2.25/hud_scale front-end affine to ~371px; =165 @hs1.0 so 0x42F80000 stock-guard holds, 4:3 no-op)", B_LIT },
  { 0x0096E278, K_SET, B_HUDSCALE, 201.0f, 0.0f, SRC_EPHINEA, GATE_STREAK_SCALE, 0x43490000, "streak.scale.y = 201*hud_scale (stock 0x43490000=201.0 from on-disk exe; =201 @hs1.0 guarded no-op, engages only hs>1)", B_LIT },
  { 0x00972148, K_SET, B_A, 0.5f, -233.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x42AE0000, "csel.banner.x (stock87 + half)", B_LIT },
  /* ---- SRC_TRINITY : 58 rows ---- */
  { 0x004013A7, K_SET, B_A, 1.0f, -1.0f, SRC_TRINITY, GATE_CHARSELECT, 0x441FC000, "csel.backdrop.w (far-corner X; MOD_X_R; 639->design_w-", B_LIT },
  { 0x004013AF, K_SET, B_C, 1.0f, -1.0f, SRC_TRINITY, GATE_CHARSELECT, 0x43EF8000, "csel.backdrop.h (far-corner Y; MOD_Y_B; 479->design_h-", B_LIT },
  { 0x00403114, K_SET, B_C, 1.0f, -130.0f, SRC_TRINITY, GATE_ALWAYS, 0x43AF0000, "fe.login.connstr.y MOD_Y_B (stock 350)", B_LIT },
  { 0x0041061C, K_SET, B_A, 1.0f, -208.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43D80000, "csel.tabdetails.x", B_LIT },
  { 0x00410628, K_SET, B_C, 1.0f, -59.0f, SRC_TRINITY, GATE_ALWAYS, 0x43D28000, "csel.tab.details_on.y MOD_Y_B (stock 421)", B_LIT },
  { 0x00410632, K_SET, B_A, 1.0f, -208.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43D80000, "csel.tabdetails.x", B_LIT },
  { 0x0041066B, K_SET, B_A, 1.0f, -208.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43D80000, "csel.tabdetails.x", B_LIT },
  { 0x00410672, K_SET, B_C, 1.0f, -59.0f, SRC_TRINITY, GATE_ALWAYS, 0x43D28000, "csel.tab.details_off.y MOD_Y_B (stock 421)", B_LIT },
  /* 0x004137C2 csel.footer.enter.x: Ephinea does NOT patch this (absent from delta) — it is left at stock 156 so it rides to the left edge in widescreen (Ephinea footer spread = Enter-left / Cancel-center / Tab-right). */
  { 0x004137C9, K_SET, B_C, 1.0f, -59.0f, SRC_TRINITY, GATE_ALWAYS, 0x43D28000, "csel.enterkey.y MOD_Y_B (stock 421)", B_LIT },
  { 0x004137D7, K_SET, B_C, 1.0f, -59.0f, SRC_TRINITY, GATE_ALWAYS, 0x43D28000, "csel.esckey.y MOD_Y_B (stock 421)", B_LIT },
  /* 0x004137DE csel.footer.details.x: orphan imm + absent from the Ephinea delta — left at stock (no SRC_TRINITY patch, to match Ephinea). */
  { 0x004ED0BF, K_SET, B_C, 1.0f, 120.0f, SRC_TRINITY, GATE_ALWAYS, 0x44160000, "dr.leftgrad.botL.y MOD_Y_B (stock 600)", B_LIT },
  { 0x004ED0D3, K_SET, B_C, 1.0f, 120.0f, SRC_TRINITY, GATE_ALWAYS, 0x44160000, "dr.leftgrad.botR.y MOD_Y_B (stock 600)", B_LIT },
  { 0x004ED144, K_SET, B_C, 1.0f, 120.0f, SRC_TRINITY, GATE_ALWAYS, 0x44160000, "dr.rightgrad.botL.y MOD_Y_B (stock 600)", B_LIT },
  { 0x004ED14E, K_SET, B_C, 1.0f, 120.0f, SRC_TRINITY, GATE_ALWAYS, 0x44160000, "dr.rightgrad.botR.y MOD_Y_B (stock 600)", B_LIT },
  { 0x0070D422, K_SET, B_C, 1.0f, -220.0f, SRC_TRINITY, GATE_ALWAYS, 0x43820000, "patch.curstatus.bar.y MOD_Y_B (stock 260)", B_LIT },
  { 0x0070D47C, K_SET, B_C, 1.0f, -116.0f, SRC_TRINITY, GATE_ALWAYS, 0x43B60000, "patch.allstatus.bar.y MOD_Y_B (stock 364)", B_LIT },
  { 0x0070D4D6, K_SET, B_C, 1.0f, -250.0f, SRC_TRINITY, GATE_CHARSELECT, 0x43660000, "patch.current-status.title.y (MOD_Y_B: C-(480-230)); .", B_LIT },
  { 0x0070D4E0, K_SET, B_C, 1.0f, -200.0f, SRC_TRINITY, GATE_CHARSELECT, 0x438C0000, "patch.current-status.progress.y (MOD_Y_B: C-(480-280))", B_LIT },
  { 0x0070D4F4, K_SET, B_C, 1.0f, -146.0f, SRC_TRINITY, GATE_CHARSELECT, 0x43A70000, "patch.all-status.title.y (MOD_Y_B: C-(480-334)); .text", B_LIT },
  { 0x0070D508, K_SET, B_C, 1.0f, -96.0f, SRC_TRINITY, GATE_CHARSELECT, 0x43C00000, "patch.all-status.progress.y (MOD_Y_B: C-(480-384)); .t", B_LIT },
  { 0x008F9E80, K_SET, B_C, 1.0f, -110.0f, SRC_TRINITY, GATE_ALWAYS, 0x43B90000, "csel.pleaseselect.y MOD_Y_B (stock 370)", B_LIT },
  { 0x0091DAE8, K_SET, B_HUDSCALE, 560.0f, 0.0f, SRC_OURS, GATE_CHARSELECT, 0x440C0000, "csel.honeycomb.frame.h", B_LIT },  /* fld[0x91dae8]+base @0x004ec981 = frame bottom-Y; scale 560*hud_scale (1120 @hs2.0) fills HudScale-scaled design_h. stock 0x440C0000=560.0 verified on-disk; prior 0x440EE000/571.5 label+guard was wrong. .data >0x008F8000 => no icache flush. */
  { 0x0091DC70, K_SET, B_C, 1.0f, -84.0f, SRC_TRINITY, GATE_ALWAYS, 0x43C60000, "dr.charname.field.y MOD_Y_B (stock 396)", B_LIT },
  { 0x0091DD20, K_SET, B_C, 1.0f, -46.0f, SRC_TRINITY, GATE_ALWAYS, 0x43D90000, "dr.bottom.okback.y MOD_Y_B (stock 434)", B_LIT },
  { 0x0091E194, K_SET, B_C, 1.0f, -64.0f, SRC_TRINITY, GATE_ALWAYS, 0x43D00000, "dr.grayline.y MOD_Y_B (stock 416)", B_LIT },
  { 0x0096FFFC, K_SET, B_A, 0.5f, 0.0f, SRC_TRINITY, GATE_ALWAYS, 0x43A00000, "fe.f1help.x MOD_X_C +half", B_LIT },
  { 0x00972128, K_SET, B_A, 1.0f, -252.0f, SRC_TRINITY, GATE_ALWAYS, 0x43C20000, "ig.cmode_records.x (MOD_X_R)", B_LIT },
  { 0x00972138, K_SET, B_A, 0.5f, -50.0f, SRC_TRINITY, GATE_ALWAYS, 0x43870000, "ig.cmode_area_number_popup.x (MOD_X_C)", B_LIT },
  /* ---- SRC_OURS : 11 rows ---- */
  { 0x004EC951, K_SET, B_A, 1.0625f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x442A0000, "csel.honeycomb.frame", B_LIT },  /* 1.0625=680/640; res-scales design_w like Ephinea (1.0625*853.33~906). .text imm32 <0x008F8000 => icache flush. */
  { 0x006F49FD, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_OURS, GATE_RUNE, 0x00000000, "rune.seal.outer.mag (code imm32 of `push 430.0`; ONE-S", B_LIT },
  { 0x006F4A57, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_OURS, GATE_RUNE, 0x00000000, "rune.seal.inner.mag (code imm32 of `push 178.0`; ONE-S", B_LIT },
  /* OWNER-MANDATED PERMANENT NOPs (a standing custom — DO NOT REMOVE). The two NOP'd
     `call 0x0082b558` sites draw the two CYAN HUD menu lines (color built @0x00719F5C
     `or ebp,0xedff` = 0xedff, depth -10.0f); suppressing them is the owner's standing
     directive ("the nops are for the thin cyan lines"). NOTE: the real F12 black BARS
     are the four render_triangle_fan(0xff000000) 640x480 quads @0x0082b5d8 in
     render_menu_hud — fixed by the in-game AFFINE (SHAPE A), not a NOP. */
  { 0x00719F96, K_NOP, B_LIT, 0.0f, 0.0f, SRC_OURS, GATE_ALWAYS, 0x1115BDE8, "cyan HUD menu line .1 NOP call 0x82b558 (color edff; OWNER PERMA)", B_LIT },
  { 0x00719FD4, K_NOP, B_LIT, 0.0f, 0.0f, SRC_OURS, GATE_ALWAYS, 0x11157FE8, "cyan HUD menu line .2 NOP call 0x82b558 (color edff; OWNER PERMA)", B_LIT },
  { 0x00721FC0, K_SET, B_A, 1.0f, 0.0f, SRC_OURS, GATE_ALWAYS, 0x44200000, "lobby.nnbar.rightEdgeX (= dw; 640 4:3-ceiling raised t", B_LIT },
  /* Lost rows recovered by the signature sweep (dropped in the kBakes recode): two
     screen-space center-X-320 elements (a photon particle + the loading-bar quad) that
     re-center to design_w/2 in widescreen. 4:3 no-op via the 320.0 stock-guard. */
  { 0x009F986C, K_SET, B_A, 0.5f, 0.0f, SRC_OURS, GATE_ALWAYS, 0x43A00000, "effect.center.particle.x (re-anchor 320->design_w/2)", B_LIT },
  { 0x00979BCC, K_SET, B_A, 0.5f, 0.0f, SRC_OURS, GATE_ALWAYS, 0x43A00000, "effect.center.loadingbar.x (re-anchor 320->design_w/2)", B_LIT },
  /* Leftover 4:3 menus the table missed (sweep): list-windows HARDCODED at (320,240)
     design-center instead of using the table-driven deanchor — help menu + lobby
     Join/Create-Party submenu. Same {K_ADD B_A 0.5 -320} deanchor.half used 60x above;
     4:3 no-op via the 320/240 stock-guards. */
  { 0x0071B1A8, K_ADD, B_A, 0.5f, -320.0f, SRC_OURS, GATE_ALWAYS, 0x43A00000, "helpmenu.list.center.x", B_LIT },
  { 0x0071B1B3, K_ADD, B_C, 0.5f, -240.0f, SRC_OURS, GATE_ALWAYS, 0x43700000, "helpmenu.list.center.y", B_LIT },
  { 0x007989DA, K_ADD, B_A, 0.5f, -320.0f, SRC_OURS, GATE_ALWAYS, 0x43A00000, "lobby.joinparty.list.center.x", B_LIT },
  { 0x007989E6, K_ADD, B_C, 0.5f, -240.0f, SRC_OURS, GATE_ALWAYS, 0x43700000, "lobby.joinparty.list.center.y", B_LIT },
  /* Connecting cyan photon (patch->login/ship): standard 16:9 re-anchor like every other
     front-end element. Lives bottom-right (X 545, Y 390) -> X right-anchored (design_w-95),
     Y bottom-anchored (design_h-90). 4:3 no-op via the 545/390 stock-guards. Read by
     0x007bc508 via fadd [0x0097E468] / [0x0097E458]. KEPT, just re-anchored. */
  { 0x0097E468, K_SET, B_A, 1.0f, -95.0f, SRC_OURS, GATE_ALWAYS, 0x44084000, "effect.connecting.photon.x (545 -> design_w-95)", B_LIT },
  { 0x0097E458, K_SET, B_C, 1.0f, -90.0f, SRC_OURS, GATE_ALWAYS, 0x43C30000, "effect.connecting.photon.y (390 -> design_h-90)", B_LIT },
  { 0x0096E114, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_OURS, GATE_RUNE, 0x00000000, "rune.orb.size (live read*hud_scale, ONE-SHOT; no fixed", B_LIT },
  { 0x0096E168, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_OURS, GATE_RUNE, 0x00000000, "rune.orb.ofs[0] (table -137,-79,137,-79,0,156; this fl", B_LIT },
  { 0x0096E16C, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_OURS, GATE_RUNE, 0x00000000, "rune.orb.ofs[1] (stock -79; ONE-SHOT live*hud_scale)", B_LIT },
  { 0x0096E170, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_OURS, GATE_RUNE, 0x00000000, "rune.orb.ofs[2] (stock 137; ONE-SHOT live*hud_scale)", B_LIT },
  { 0x0096E174, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_OURS, GATE_RUNE, 0x00000000, "rune.orb.ofs[3] (stock -79; ONE-SHOT live*hud_scale)", B_LIT },
  { 0x0096E178, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_OURS, GATE_RUNE, 0x00000000, "rune.orb.ofs[4] (stock 0; ONE-SHOT live*hud_scale)", B_LIT },
  { 0x0096E17C, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_OURS, GATE_RUNE, 0x00000000, "rune.orb.ofs[5] (stock 156; ONE-SHOT live*hud_scale)", B_LIT },
  /* SEGA-splash: Ephinea widens ONLY the two right-quad x2 coords (0x009A3468/
     0x009A3488), kept via the anzz1 SRC_ANZZ1 "hud.w" rows above. The six left/seam
     coords (0x009A3420/3428/3440/3448/3460/3480) stay stock — scaling them
     over-stretches "SONIC TEAM(TM)". */
  /* ---- SRC_EPHINEA : 31 X-axis widens the recode MISSED ----
     Cross-checking the FULL byte-delta vs this table:
     Ephinea widens these VAs but our table never referenced them, so they sat at
     stock 4:3 ("left at 4:3 / unchanged resolution math"). 24 WIDTH = stock*(A/640)
     via K_MUL B_WIDENX; 6 CENTER = stock+(A-640)/2 and 1 RIGHT = stock+(A-640) via
     K_ADD B_A. All are bit-exact no-ops at 4:3 and reproduce Ephinea's live value at
     16:9 (31/31). Clusters: front-end menus 0x0040xx, ending/credits viewport
     0x00785xx (InitializeEndingViewport), char-select anchor block 0x008F9E-B1
     (FLOAT_008f9xxx, beside the 92-VA block), congrats/results name-X 0x009CA5
     (Trinity-confirmed center), plus align-list 0x009721 + misc fe floats. */
  { 0x0040CA07, K_MUL, B_WIDENX, 1.0f,    0.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43540000, "fe.menu040c.w", B_LIT },
  { 0x0040CA40, K_MUL, B_WIDENX, 1.0f,    0.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43540000, "fe.menu040c.w", B_LIT },
  { 0x0040CA66, K_MUL, B_WIDENX, 1.0f,    0.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43650000, "fe.menu040c.w", B_LIT },
  { 0x0040CA8C, K_MUL, B_WIDENX, 1.0f,    0.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43650000, "fe.menu040c.w", B_LIT },
  { 0x0040CAE2, K_MUL, B_WIDENX, 1.0f,    0.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43CC8000, "fe.menu040c.w", B_LIT },
  { 0x0040CB1E, K_MUL, B_WIDENX, 1.0f,    0.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43CC8000, "fe.menu040c.w", B_LIT },
  { 0x008F9EC0, K_MUL, B_WIDENX, 1.0f,    0.0f, SRC_EPHINEA, GATE_ALWAYS, 0x42C80000, "csel.block.w", B_LIT },
  { 0x008F9F20, K_ADD, B_A,      0.5f, -320.0f, SRC_EPHINEA, GATE_ALWAYS, 0x4407C000, "csel.block.cx", B_LIT },
  { 0x008FB114, K_MUL, B_WIDENX, 1.0f,    0.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43A00000, "csel.block.w", B_LIT },
  { 0x0091EEB8, K_MUL, B_WIDENX, 1.0f,    0.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43A00000, "fe.91ee.w", B_LIT },
  { 0x00920010, K_ADD, B_A,      1.0f, -640.0f, SRC_EPHINEA, GATE_ALWAYS, 0x44228000, "fe.9200.rx", B_LIT },
  { 0x00972120, K_ADD, B_A,      0.5f, -320.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43898000, "align.9721.cx", B_LIT },
  { 0x00972130, K_ADD, B_A,      1.0f, -640.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43C80000, "align.9721.rx", B_LIT },
  { 0x009C94A0, K_MUL, B_WIDENX, 1.0f,    0.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43A00000, "fe.9c94.w", B_LIT },
  { 0x009CA53C, K_ADD, B_A,      0.5f, -320.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43800000, "congrats.name.cx", B_LIT },
  { 0x009CA544, K_ADD, B_A,      0.5f, -320.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43C00000, "congrats.name.cx", B_LIT },
  { 0x009CA54C, K_ADD, B_A,      0.5f, -320.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43000000, "congrats.name.cx", B_LIT },
  { 0x009CA554, K_ADD, B_A,      0.5f, -320.0f, SRC_EPHINEA, GATE_ALWAYS, 0x44008000, "congrats.name.cx", B_LIT },
  /* ---- SRC_TRINITY : 3 Y-axis anchors the byte-delta could NOT see ----
     Trinity MOD_Y_* anchors are bit-exact no-ops at the standard HUD (hudResY==480
     = the state Ephinea was dumped in), so they never appeared in _ephinea_delta.csv.
     Cross-checking Trinity's same-build hudElements[] surfaced ~63 Y-anchor candidates;
     an exhaustive RE confirmed Ephinea
     deliberately leaves 52 of them to the in-game 2D affine + the FUN_52d8cb90
     ListWindow detour (static-patching them = DOUBLE scale), and that ONLY these 3
     have a confirmed Ephinea cascade write (Loop6 half-height table / direct renderH
     write in the unpacked DLL) AND fold to bit-exact 4:3 no-ops. They compose with the
     affine exactly like the already-shipped deanchor.vcenter rows. */
  { 0x0096FFF4, K_ADD, B_C, 0.5f, -240.0f, SRC_TRINITY, GATE_ALWAYS, 0x43700000, "hud.f1help.y MOD_Y_C", B_LIT },
  { 0x009712EC, K_ADD, B_C, 1.0f, -480.0f, SRC_TRINITY, GATE_ALWAYS, 0x43F00000, "hud.f12bg.bottom MOD_Y_B (=renderH)", B_LIT },
  { 0x0097213C, K_SET, B_C, 0.5f,  -25.0f, SRC_TRINITY, GATE_ALWAYS, 0x43570000, "ig.cmode_area_popup.y MOD_Y_C (st215)", B_LIT },

  /* ===== SRC_EPHINEA : 22 cascade VAs the table was MISSING =====
     Full cross-check of OUR table vs Ephinea's complete unpacked-DLL patchset
     (_eph_patchset.json, 752 VAs) found 63 VAs Ephinea patches that we never
     referenced; 22 map 1:1 to our row kinds and are added here (the other 41 are
     Stage-C frame geometry / SSAA-inline / co-op PlayerScale / code-patches that
     need individual RE — NOT auto-baked). All are 4:3-safe (B_C->480, B_WIDENX->1.0)
     and present in Ephinea's patchset, so they compose with the now-stock affine
     exactly as Ephinea does (no double-scale). Includes the char-select Tab-button
     3rd Y-state 0x00410639 (Loop5_Hadd_full) whose ABSENCE caused the Tab button to
     animate through the un-patched stock Y on load = the "crawl-down" the owner saw. */
  { 0x0040CB07, K_ADD, B_C,       1.0f,  -480.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43E40000, "eph.92va.l5.hadd.raw", B_LIT },
  { 0x0040CB2C, K_ADD, B_C,       1.0f,  -480.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43E48000, "eph.92va.l5.hadd.raw", B_LIT },
  { 0x0040CB44, K_MUL, B_WIDENX,  1.0f,     0.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43D50000, "eph.92va.l1.wmul", B_LIT },
  { 0x0040CB52, K_ADD, B_C,       1.0f,  -480.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43D50000, "eph.92va.l5.hadd.raw", B_LIT },
  { 0x0040CB6A, K_MUL, B_WIDENX,  1.0f,     0.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43D50000, "eph.92va.l1.wmul", B_LIT },
  { 0x0040CB78, K_ADD, B_C,       1.0f,  -480.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43D48000, "eph.92va.l5.hadd.raw", B_LIT },
  { 0x00410639, K_ADD, B_C,       1.0f,  -480.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43D28000, "csel.tab.details_mid.y (Tab crawl fix)", B_LIT },
  { 0x0070D440, K_ADD, B_C,       1.0f,  -480.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43820000, "eph.loop5.hadd.full", B_LIT },
  { 0x0070D45E, K_ADD, B_C,       1.0f,  -480.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43820000, "eph.loop5.hadd.full", B_LIT },
  { 0x0075A8C5, K_ADD, B_C,       1.0f,  -480.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43AC0000, "eph.loop5.hadd.full", B_LIT },
  { 0x008F9EFC, K_MUL, B_HUDSCALE, 1.0f,    0.0f, SRC_EPHINEA, GATE_ALWAYS, 0x41000000, "eph.92va.l2.hmul", B_LIT },
  { 0x008F9F1C, K_ADD, B_C,       0.5f,  -240.0f, SRC_EPHINEA, GATE_ALWAYS, 0x42920000, "eph.92va.l4.hadd", B_LIT },
  { 0x008F9F24, K_ADD, B_C,       0.5f,  -240.0f, SRC_EPHINEA, GATE_ALWAYS, 0x42960000, "eph.92va.l4.hadd", B_LIT },
  { 0x008F9F48, K_ADD, B_C,       1.0f,  -480.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43D00000, "eph.92va.l5.hadd.raw", B_LIT },
  { 0x008FA134, K_MUL, B_WIDENX,  1.0f,     0.0f, SRC_EPHINEA, GATE_ALWAYS, 0x00000000, "eph.92va.l1.wmul (st0)", B_LIT },
  { 0x008FA154, K_MUL, B_WIDENX,  1.0f,     0.0f, SRC_EPHINEA, GATE_ALWAYS, 0x00000000, "eph.92va.l1.wmul (st0)", B_LIT },
  { 0x0091EEB4, K_ADD, B_C,       0.5f,  -240.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43700000, "eph.loop6.hadd.half", B_LIT },
  { 0x00920008, K_ADD, B_C,       1.0f,  -480.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43F50000, "eph.loop5.hadd.full", B_LIT },
  { 0x00972124, K_ADD, B_C,       0.5f,  -240.0f, SRC_EPHINEA, GATE_ALWAYS, 0x432F0000, "eph.loop6.hadd.half", B_LIT },
  { 0x00972134, K_ADD, B_C,       1.0f,  -480.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43AA0000, "eph.loop5.hadd.full", B_LIT },
  { 0x0097214C, K_ADD, B_C,       1.0f,  -480.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43B20000, "eph.loop5.hadd.full", B_LIT },
  { 0x009C94A4, K_ADD, B_C,       0.5f,  -240.0f, SRC_EPHINEA, GATE_ALWAYS, 0x43480000, "eph.loop6.hadd.half", B_LIT },

  /* ===== SRC_EPHINEA : char-create dressing-room hex-tile backdrop =====
     Ephinea scales PSOBB's char-create dressing-room hex-tile backdrop by
     ×(renderH/480) == ×hud_scale, gated to char-create (the float/short H-loops in
     the unpacked DLL; confirmed vs Trinity resolution.cpp + the stock decompile
     RenderChallengePanelBackground_004ed008 / RenderUIPiece_0082b6bc). 24 .data VAs:
     12 vec2f positions (K_MUL B_HUDSCALE, ×hud_scale on the live stock float) + 12
     u16 W/H sizes (K_U16 B_HUDSCALE, 16-bit RMW ×hud_scale, round-to-nearest). Both
     are bit-exact no-ops at HudScale 1.0 (4:3) and 2× at HudScale 2.0. Ephinea has
     no per-frame column-emit splice for these. */
  /* dressing-room tile X/Y positions — Ephinea float H-loop (×renderH/480 = ×hud_scale) */
  { 0x0091DBD0, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43C00000, "cc.tile.id12.X (384)",  B_LIT },
  { 0x0091DBD8, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43C00000, "cc.tile.id13.X (384)",  B_LIT },
  { 0x0091DBDC, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43800000, "cc.tile.id13.Y (256)",  B_LIT },
  { 0x0091DBE8, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x00000000, "cc.tile.id14.X (0)",    B_LIT },
  { 0x0091DBEC, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43800000, "cc.tile.id14.Y (256)",  B_LIT },
  { 0x0091DBF0, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43800000, "cc.tile.id15.X (256)",  B_LIT },
  { 0x0091DBF8, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43800000, "cc.tile.id16.X (256)",  B_LIT },
  { 0x0091DBFC, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43000000, "cc.tile.id16.Y (128)",  B_LIT },
  { 0x0091DC00, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43800000, "cc.tile.id17.X (256)",  B_LIT },
  { 0x0091DC04, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43800000, "cc.tile.id17.Y (256)",  B_LIT },
  { 0x0091DC08, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43800000, "cc.tile.id18.X (256)",  B_LIT },
  { 0x0091DC0C, K_MUL, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x43C00000, "cc.tile.id18.Y (384)",  B_LIT },
  /* dressing-room tile W/H sizes — Ephinea short H-loop (×renderH/480 = ×hud_scale), RMW u16 */
  { 0x009B77D0, K_U16, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x00000100, "cc.tile.id12.W (256)",  B_LIT },
  { 0x009B77D2, K_U16, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x00000100, "cc.tile.id12.H (256)",  B_LIT },
  { 0x009B77E4, K_U16, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x00000100, "cc.tile.id13.W (256)",  B_LIT },
  { 0x009B77E6, K_U16, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x00000100, "cc.tile.id13.H (256)",  B_LIT },
  { 0x009B77F8, K_U16, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x00000080, "cc.tile.id14.W (128)",  B_LIT },
  { 0x009B77FA, K_U16, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x00000080, "cc.tile.id14.H (128)",  B_LIT },
  { 0x009B780C, K_U16, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x00000080, "cc.tile.id15.W (128)",  B_LIT },
  { 0x009B780E, K_U16, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x00000080, "cc.tile.id15.H (128)",  B_LIT },
  { 0x009B7820, K_U16, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x00000080, "cc.tile.id16.W (128)",  B_LIT },
  { 0x009B7822, K_U16, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x00000080, "cc.tile.id16.H (128)",  B_LIT },
  { 0x009B7834, K_U16, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x00000080, "cc.tile.id17.W (128)",  B_LIT },
  { 0x009B7836, K_U16, B_HUDSCALE, 1.0f, 0.0f, SRC_EPHINEA, GATE_CHARSELECT, 0x00000080, "cc.tile.id17.H (128)",  B_LIT },
/* The FULL 61-row Trinity gap-fill — the state with the in-game nameplate, minimap
   box, and title corner element anchored correctly. A subset of these rows left the
   nameplate riding up and the minimap box off-screen, so we keep the full set. */
#include "_trinity_gapfill.inc"
};


/* ============================================================================
 * RECODE CORE (staging) — resolve_base / gate_on / apply_bakes.
 * Folds into pso_widescreen.c after kBakes[]. Assumes: g_cfg (config struct),
 * ws_scale_ctx, log_line, kBakes[]. No <math.h> (B_BCEIL uses anzz1's +0.9999
 * integer ceil, matching its Bi).
 * ========================================================================== */

#define ARRAYLEN(a) (sizeof(a) / sizeof((a)[0]))

static uint32_t f32_bits(float v) { uint32_t u; memcpy(&u, &v, 4); return u; }

/* resolve a row's value base from the live scale ctx.
   At 4:3 (hud_scale derives A=640, C=480, B=128, D=128, AR=4/3) every base
   returns its stock-equivalent, so coeff*base+offset == the stock value and the
   value-guard turns the write into a no-op — the whole table is 4:3-identical. */
static float resolve_base(uint8_t base, const ws_scale_ctx *s)
{
    switch (base) {
        case B_A:        return s->A;
        case B_C:        return s->C;
        case B_B:        return s->B;
        case B_BCEIL:    return (float)(DWORD)(s->B + 0.9999f);  /* == anzz1 Bi */
        case B_D:        return (float)s->D;
        case B_AR:       return s->game_aspect;
        case B_RW:       return (float)s->render_w;
        case B_RH:       return (float)s->render_h;
        case B_HUDSCALE: return s->hud_scale;
        case B_KX:       return s->hud_scale * ((16.0f / 9.0f) / (4.0f / 3.0f));
        /* affine_y = render_h / design_h, where design_h is the HudScale-scaled
           height C (=480*HudScale), NOT 480 — matches the original
           patch_charselect_vertical (render_h / s->C). Using /480 over-scaled the
           7 char-select details-pane rows by exactly HudScale (caught in verify). */
        case B_AFFINEY:  return (s->C > 1.0f) ? ((float)s->render_h / s->C) : 1.0f;
        case B_ANATIVE:  return (s->hud_scale > 0.01f) ? (s->A / s->hud_scale) : s->A;
        case B_CNATIVE:  return (s->hud_scale > 0.01f) ? (s->C / s->hud_scale) : s->C;
        case B_WIDENX:   return s->A / 640.0f;   /* width-widen factor; 1.0 @4:3 */
        case B_LIT:
        default:         return 1.0f;
    }
}

/* a row's gate bit maps to one cfg toggle; GATE_ALWAYS is unconditional. */
static int gate_on(uint8_t gate)
{
    switch (gate) {
        case GATE_ALWAYS:       return 1;
        case GATE_CHARSELECT:   return g_cfg.patch_charselect;
        case GATE_MINIMAP:      return g_cfg.patch_minimap;
        case GATE_RUNE:         return g_cfg.patch_rune_scale;
        case GATE_STREAK_SCALE: return g_cfg.patch_streak_scale;
        case GATE_SPLASH:       return g_cfg.patch_splash_phase2;
        default:                return 1;
    }
}

/* ADD/MUL rows are RMW and would double on re-run; we snapshot each one's stock
   on the FIRST apply, so the written value is the absolute stock+delta / stock*f.
   That makes the WHOLE table idempotent and lets the overwrite-guard re-run it. */
static float   g_stock_cache[ARRAYLEN(kBakes)];
static uint8_t g_stock_valid[ARRAYLEN(kBakes)];

/* apply_bakes — the ONE value-guarded pass over kBakes[]. Safe to re-run. */
static void apply_bakes(const ws_scale_ctx *s)
{
    int n_set = 0, n_delta = 0, n_skip = 0, n_guard = 0;
    for (size_t i = 0; i < ARRAYLEN(kBakes); i++) {
        const bake_t *b = &kBakes[i];
        if (!gate_on(b->gate)) continue;

        __try {
            float expr = (b->coeff * resolve_base(b->base, s) + b->offset)
                       * resolve_base(b->base2, s);   /* base2 == B_LIT (1.0) for most rows */
            uint32_t want;

            /* K_U16 — 16-bit RMW tile W/H short. Fully self-contained: the shared
               path below reads/writes a uint32 (would clobber the adjacent 16-bit
               field), so this branch does its own read/guard/write and continues. */
            if (b->kind == K_U16) {
                if (!g_stock_valid[i]) {
                    /* first sight: snapshot the stock short as a float */
                    g_stock_cache[i] = (float)*(volatile uint16_t *)(uintptr_t)b->va;
                    g_stock_valid[i] = 1;
                }
                uint16_t want16 = (uint16_t)(g_stock_cache[i] * expr + 0.5f); /* round-to-nearest, matches Trinity round() */
                uint16_t live16 = *(volatile uint16_t *)(uintptr_t)b->va;
                if (live16 == want16) { n_skip++; continue; }                 /* idempotent */
                DWORD old16, tmp16;
                if (!VirtualProtect((LPVOID)(uintptr_t)b->va, 2, PAGE_EXECUTE_READWRITE, &old16)) continue;
                *(volatile uint16_t *)(uintptr_t)b->va = want16;
                VirtualProtect((LPVOID)(uintptr_t)b->va, 2, old16, &tmp16);
                if (b->va < 0x008F8000u)  /* these tile VAs are >0x008F8000 => no icache flush */
                    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)(uintptr_t)b->va, 2);
                n_delta++;
                continue;
            }

            /* K_U8 — 1-byte literal write (char-select gradient colors). Self-contained
               like K_U16 (the shared uint32 path would clobber 3 neighbouring bytes).
               Colors don't scale: expr == coeff (base==B_LIT, offset==0) == the target
               byte. Sig-guard against the live byte == (uint8_t)b->stock (the stock color)
               so a foreign/already-changed site is refused; idempotent if already ours.
               All these VAs are >0x008F8000 => no icache flush. */
            if (b->kind == K_U8) {
                uint8_t want8 = (uint8_t)expr;
                uint8_t live8 = *(volatile uint8_t *)(uintptr_t)b->va;
                if (live8 == want8) { n_skip++; continue; }                    /* idempotent */
                if (b->stock && live8 != (uint8_t)b->stock) { n_guard++; continue; }  /* foreign site */
                DWORD old8, tmp8;
                if (!VirtualProtect((LPVOID)(uintptr_t)b->va, 1, PAGE_EXECUTE_READWRITE, &old8)) continue;
                *(volatile uint8_t *)(uintptr_t)b->va = want8;
                VirtualProtect((LPVOID)(uintptr_t)b->va, 1, old8, &tmp8);
                if (b->va < 0x008F8000u)  /* these color VAs are >0x008F8000 => no icache flush */
                    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)(uintptr_t)b->va, 1);
                n_set++;
                continue;
            }

            if (b->kind == K_NOP) {
                want = 0;  /* sentinel; handled below */
            } else if (b->kind == K_U32) {
                want = (uint32_t)expr;
            } else if (b->kind == K_ADD || b->kind == K_MUL) {
                if (!g_stock_valid[i]) {
                    /* first sight: optional sig-guard, then snapshot stock */
                    uint32_t live = *(volatile uint32_t *)(uintptr_t)b->va;
                    if (b->stock && live != b->stock) { n_guard++; continue; }
                    memcpy(&g_stock_cache[i], &live, 4);
                    g_stock_valid[i] = 1;
                }
                float stock = g_stock_cache[i];
                float v = (b->kind == K_ADD) ? (stock + expr) : (stock * expr);
                want = f32_bits(v);
            } else { /* K_SET / K_AR */
                want = f32_bits(expr);
            }

            uint32_t live = *(volatile uint32_t *)(uintptr_t)b->va;
            if (b->kind != K_NOP && live == want) { n_skip++; continue; }  /* idempotent */
            /* SET-class sig-guard: refuse a foreign site (neither stock nor ours) */
            if (b->stock && (b->kind == K_SET || b->kind == K_U32) &&
                live != b->stock && live != want) { n_guard++; continue; }

            DWORD old, tmp;
            size_t len = (b->kind == K_NOP) ? 5 : 4;
            if (!VirtualProtect((LPVOID)(uintptr_t)b->va, len, PAGE_EXECUTE_READWRITE, &old)) continue;
            if (b->kind == K_NOP) memset((void *)(uintptr_t)b->va, 0x90, 5);
            else                  *(volatile uint32_t *)(uintptr_t)b->va = want;
            VirtualProtect((LPVOID)(uintptr_t)b->va, len, old, &tmp);
            if (b->va < 0x008F8000u)  /* .text => flush icache (anzz1's split) */
                FlushInstructionCache(GetCurrentProcess(), (LPCVOID)(uintptr_t)b->va, len);

            if (b->kind == K_ADD || b->kind == K_MUL) n_delta++; else n_set++;
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    log_line("[ws] apply_bakes: %d set, %d delta, %d skip, %d guard (of %d rows)",
             n_set, n_delta, n_skip, n_guard, (int)ARRAYLEN(kBakes));
    apply_special(s);
}

/* sp_write_f32 — value-guarded float write (the apply_special companion). */
static void sp_write_f32(uint32_t va, float v)
{
    DWORD old, tmp;
    __try {
        if (*(volatile uint32_t *)(uintptr_t)va == f32_bits(v)) return;
        if (!VirtualProtect((LPVOID)(uintptr_t)va, 4, PAGE_EXECUTE_READWRITE, &old)) return;
        *(volatile float *)(uintptr_t)va = v;
        VirtualProtect((LPVOID)(uintptr_t)va, 4, old, &tmp);
        if (va < 0x008F8000u)
            FlushInstructionCache(GetCurrentProcess(), (LPCVOID)(uintptr_t)va, 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

/* ============================================================================
 * CUSTOM C3 — minimap top-right anchor (size + position), absolute re-derive.
 *
 * Both minimap rects are HUD index 0; the engine multiplies EVERY field by
 * render/design before drawing, so all fields live in DESIGN space and we write
 * design-space coords directly. Two layers, both written every pass:
 *   viewport (map graphic), projected by 0x00804844:
 *     X=0x00A11324 Y=0x00A11328 W=0x00A1132C H=0x00A11330  stock(544,128,128,128)
 *   background (gray box / silhouette), drawn by 0x008046B0:
 *     X=0x00A113E8 Y=0x00A113EC W=0x00A11400 H=0x00A11404  stock(480,64,128,128)
 *
 * SPEC: keep the native 640x480 placement fractions; size grows with
 * MinimapSizeScale * HudScale; pin the box's right/top edges into the design
 * top-right by MinimapCornerRightPx / MinimapCornerTopPx. Each pass re-derives
 * ABSOLUTE targets from literal native fractions (NOT live slots / snapshots), so
 * value-guarded re-runs are idempotent and track both HudScale and the
 * front-end<->in-game flip. NO screen-constant re-projection.
 *
 * IDENTITY AT hs=1.0, MinimapSizeScale=1.0 (S=1, dw=640, dh=480, cr=ct=0):
 *   The X anchor pins the box's RIGHT edge: X = frac*dw - cr - W (W=128*S=128).
 *     bg X = (480/640)*640 - 0 - 128 = 480 (stock 480) ; bg Y = (64/480)*480 = 64
 *            bg W = 128 (stock) ; bg H = 128 (stock)  -> stock(480,64,128,128) EXACT
 *     vp X = (544/640)*640 - 0 - 128 = 416 ; vp Y = (128/480)*480 = 128
 *            vp W = 128 (stock) ; vp H = 128 (stock)
 *   bg reproduces stock(480,64,128,128) EXACTLY. vp W/H/Y reproduce stock; vp X
 *   shifts the map graphic 128px left of stock (544->416) so it sits inside the
 *   gray box instead of off its right edge.
 *   At any hs the W=H grow by S and X re-pins the right edge -> box stays in the
 *   corner and never walks off-screen. Value-guarded via sp_write_f32.
 *
 * minimap_zoom_apply — CUSTOM C3, ported from the standalone pso_minimap.asi.
 * Overwrites the single float imm32 at .text 0x00804A5D (the minimap zoom/FOV
 * divisor pushed into the projection setup at 0x00804A65). Stock = 1.133799f;
 * SMALLER = zoomed OUT (wider minimap view). Flat constant — NOT entangled with
 * HudScale or the s->A/C scale ctx.
 *
 * This is a .TEXT write, so unlike sp_write_f32 (data) it does the full code-poke
 * idiom: VirtualProtect(PAGE_EXECUTE_READWRITE) -> write -> restore protection ->
 * FlushInstructionCache so the next fetch reloads the new immediate. Value-guarded
 * (skip if the 4 bytes already equal the target) so re-running apply_special()
 * across the front-end<->in-game flip is cheap and idempotent. SEH-wrapped. */
static void minimap_zoom_apply(void)
{
    const uint32_t va = 0x00804A5Du;
    float z = g_cfg.minimap_zoom;
    if (z < 0.1f) z = 0.1f;                 /* defensive re-clamp (INI parse already clamps) */
    if (z > 3.0f) z = 3.0f;

    volatile float *p = (volatile float *)(uintptr_t)va;
    static int s_logged = 0;
    __try {
        if (*p == z) return;                /* value-guard: idempotent re-assert is free */
        DWORD old;
        if (!VirtualProtect((LPVOID)(uintptr_t)va, 4, PAGE_EXECUTE_READWRITE, &old)) return;
        *p = z;
        DWORD tmp; VirtualProtect((LPVOID)(uintptr_t)va, 4, old, &tmp);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)(uintptr_t)va, 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    if (!s_logged) {
        s_logged = 1;
        log_line("[pso_widescreen] minimap zoom: 0x00804A5D <- %.4f (stock 1.1338)", z);
    }
}

/* apply_special — the special rows that aren't (coeff*base+offset)*base2. After the
   Ephinea-fidelity pass this is ONLY custom C3: the minimap top-right anchor. Both
   layers (viewport 0x00A11324.. + background 0x00A113E8..) get an ABSOLUTE size +
   position re-derived from native 640x480 fractions every pass — no snapshot, no
   deanchor reuse — so it tracks HudScale + the front-end<->in-game flip and is
   idempotent under value-guarded sp_write_f32. */
static void apply_special(const ws_scale_ctx *s)
{
    /* C3 (ours): minimap size + top-right anchor, with the box and map LOCKED together.
       The engine draws the gray BOX as a 2D quad at design-space (bg_X,bg_Y,bg_W,bg_H)
       through the UI scale, but renders the MAP into a D3D viewport whose screen X/Y the
       projection computes as (vp_X*scale - 0.5*vp_W*scale) — a baked -0.5*width re-center
       term (_DAT_0097ec84 = -0.5, disasm-confirmed) that the box never gets. Stock lands
       the map on the box by setting vp_X = bg_X + bg_W/2 and vp_Y = bg_Y + bg_H/2 (map
       CENTER == box CENTER). So we anchor the BOX into the design top-right, then DERIVE
       the viewport from the box center -> they stay locked at every HudScale/resolution.
       Size grows with MinimapSizeScale*HudScale; MinimapCornerRightPx/TopPx nudge the box
       further into the corner. Absolute re-derive each pass -> idempotent, flip-aware.
       Identity at hs1.0,size1.0: box (480,64,128,128) -> vp (544,128,128,128) = stock. */
    if (g_cfg.patch_minimap) {
        float hs = g_cfg.hud_scale; if (!(hs > 0.1f && hs < 10.0f)) hs = 1.0f;
        float S  = g_cfg.minimap_size_scale * hs;     /* size grows with HudScale */
        float dw = s->A, dh = s->C;                   /* live design_w / design_h */
        float cr = g_cfg.minimap_corner_right_px;
        float ct = g_cfg.minimap_corner_top_px;
        /* Box and map sizes are LOCKED (both 128*S) so the map exactly fills the box. */
        float bgW = 128.0f*S, bgH = 128.0f*S;
        float vpW = bgW, vpH = bgH;
        /* BACKGROUND (gray box): top-right corner anchor in design space. Right edge at
           the native 0.95 frac of design_w (608/640), top edge at 64/480 of design_h. */
        float bgX = (608.0f/640.0f)*dw - cr - bgW;
        float bgY = ( 64.0f/480.0f)*dh + ct;
        /* VIEWPORT (map): DERIVE from the box CENTER so the engine's -0.5*vpW re-center
           term drops the map onto the box. vp_X = bg_X + bg_W/2, vp_Y = bg_Y + bg_H/2. */
        float vpX = bgX + bgW*0.5f;
        float vpY = bgY + bgH*0.5f;
        sp_write_f32(0x00A1132Cu, vpW);
        sp_write_f32(0x00A11330u, vpH);
        sp_write_f32(0x00A11324u, vpX);
        sp_write_f32(0x00A11328u, vpY);
        sp_write_f32(0x00A11400u, bgW);
        sp_write_f32(0x00A11404u, bgH);
        sp_write_f32(0x00A113E8u, bgX);
        sp_write_f32(0x00A113ECu, bgY);
        /* COMPASS + PLAYER ICONS track the box. The engine draws each icon at
           screen = base_short + offset_f32 in the SAME DESIGN space as the box (both
           go through the 2D affine at RenderUIQuad 0x0082B440). We pin the icon's
           design-space position to box_origin + frac*box_size (frac from the native
           640x480 layout), then back out the offset_f32 the engine adds:
               offset = target - base_short.
           Stock layout: box (480,64,128,128); compass screen (591,175); player (591,176).
             compass dx=dy = 111/128 of box size ; player dx = 111/128, dy = 112/128.
           These fractions are size-invariant -> the icons sit at the same visual spot
           on the map at any S, and at S=1,dw=640,dh=480,cr=ct=0 reproduce the stock
           offsets exactly. Single writer (the two old deanchor.full rows were removed),
           absolute re-derive each pass -> idempotent + flip-aware under sp_write_f32. */
        float cmpX = bgX + (111.0f/128.0f)*bgW;   /* 591 @identity */
        float cmpY = bgY + (111.0f/128.0f)*bgH;   /* 175 @identity */
        float plrX = bgX + (111.0f/128.0f)*bgW;   /* 591 @identity */
        float plrY = bgY + (112.0f/128.0f)*bgH;   /* 176 @identity */
        sp_write_f32(0x00A11398u, cmpX - 16.0f);  /* compass off X -> 575 @identity (base 16) */
        sp_write_f32(0x00A1139Cu, cmpY - 16.0f);  /* compass off Y -> 159 @identity (base 16) */
        sp_write_f32(0x00A11390u, plrX - 16.0f);  /* player  off X -> 575 @identity (base 16) */
        sp_write_f32(0x00A11394u, plrY - 27.0f);  /* player  off Y -> 149 @identity (base 27) */
        /* NOTE: CompassRightPx/CompassBottomPx ini knobs are currently INERT — the
           icons now follow the box's own corner anchor (MinimapCornerRightPx/TopPx),
           so a separate compass nudge would only desync the icons from the map. Left
           in the cfg (harmless defaults) in case a future per-icon offset is wanted. */
    }

    /* C3 (ours, ported from pso_minimap.asi): minimap ZOOM. A flat .text imm32 divisor
       at 0x00804A5D — DISJOINT from the anchor .data slots above (no cross-coupling).
       Re-asserts here so it survives the front-end<->in-game flip; value-guarded so the
       re-run is cheap. */
    if (g_cfg.patch_minimap_zoom)
        minimap_zoom_apply();
}


static void apply_static_patches(const ws_scale_ctx *s)
{
    // (1) front-end startup source bakes — the char-create description-TEXT
    // bake (not a flat write). Runs FIRST; apply_bakes overwrites any overlap.
    apply_startup_bakes(s->render_w);

    // (2) THE unified table — authoritative coordinate source of truth (folds
    // anzz1's 6 lists + the ~20 patch_* flat-write helpers). Value-guarded,
    // 4:3-identity by construction; calls apply_special() internally.
    apply_bakes(s);

    // (3) Char-create class-info-box + hex-backdrop: positioned entirely by kBakes[]
    // (applied above), NOT a per-draw splice. Ephinea does NOT splice
    // RenderChallengePanelElement and does NOT scale char-create tiles; its delta is
    // the static design-width widenings + honeycomb frame + right-gradient
    // widths/colors.

    // (4) psobb.io patch-server news/status screen (Trinity AdDrawLineTask
    // @0x00408C9D): right/bottom-anchor the MOTD box geometry.
    patch_ad_draw_line(s);

    // (Char-create class-select layout is an authoritative-source bake now —
    // folded into apply_startup_bakes() above (called right after the anzz1 bake),
    // which rewrites the 2 .text scene-init immediates + 2 .rdata portrait floats
    // alongside the Scene-02 login seeds. No render-thread hook: 0x0082BB74 /
    // 0x0082B440 are byte-stock.)

    // (4) Ephinea in-game detour-handler ports (FUN_52da6e50 / FUN_52d8cb90),
    // . Default-ON with widescreen; value/byte-guarded 4:3 no-op.
    //
    // (4a) KILL-SCREEN HEXAGON-LAYER aspect-fit pre-pass (Ephinea FUN_52da6e50) —
    // one-shot boot CALL-site redirect of 0x0067C4ED (NOT a kStripSites
    // detour, so NOT wiped by FE->IG). Identity at true 4:3 (else-branch
    // k=1,off=0), so install only when the engine aspect is wider than 4:3.
    if (s->enabled && s->game_aspect > (4.0f / 3.0f) + 0.001f) {
        ks_hexfit_install();
    } else {
        log_line("[pso_widescreen] killscreen hexfit: SKIP (disabled or 4:3 identity)");
    }

    // (4b) IN-GAME LIST-WINDOW bottom-anchor (Ephinea FUN_52d8cb90) — boot CALL-
    // site redirect of 0x0073FF92 (`call 0x00737F80`, ==1 arm). Self-no-ops
    // at design_h<=480 (the Y term is 0), so it is safe to install always;
    // it bottom-anchors only when design_h>480 (in-game @hs>1.0). RE-ASSERTED
    // every in-game worker tick (reassert_ingame_hooks) since this is a
    // code-flow detour the FE->IG transition wipes.
    if (s->enabled) {
        if (lw_yanchor_redirect())
            log_line("[pso_widescreen] listwindow y-anchor: installed @0x0073FF92");
    } else {
        log_line("[pso_widescreen] listwindow y-anchor: SKIP (disabled)");
    }

    // (4c) FIX B — quest/area-transition loading-RING affine wrap. Redirects the
    // .data vtable slot 0x00B3E9B8 (0x006F475C) to ld_ring_shim, which forces a
    // SQUARE + centered affine for the loading sub-state only (g_is_quest_loading
    // [0x00AAB378]!=0); the shared LOBBY game-list path passes through untouched.
    // The slot is .data so it survives the FE->IG transition, but we re-assert it
    // (stock-guarded, steady-state quiet) every transition tick for safety.
    if (s->enabled) {
        if (ld_ring_redirect())
            log_line("[pso_widescreen] loading-ring affine wrap: installed (vtable slot 0x00B3E9B8)");
    } else {
        log_line("[pso_widescreen] loading-ring affine wrap: SKIP (disabled)");
    }

    // (5) NO-VIGNETTE — NOP the 3 F12 / in-game-MENU vignette-draw sites (the dark
    // dimming scrim drawn behind the in-game menu) for a clean view. This is the
    // MENU vignette, NOT the status-effect vignette (mod_vignette.asi is separate).
    // Absolute VAs (no-ASLR base 0x00400000), patched via the
    // shared patch_write() helper (VirtualProtect EXECUTE_READWRITE -> memcpy ->
    // restore -> FlushInstructionCache). Stock-byte guarded BEFORE each write so
    // re-running is a no-op AND a non-matching build is left untouched.
    //   719B54: 77 0b (ja  0x00719B61)              -> 90 90   (two NOPs)
    //   733BA7: 75 18 (jne 0x00733BC1)              -> 90 90   (two NOPs)
    //   733A0E: 0f 84 9a 00 00 00 (je 0x00733AAE)   -> 90 e9 .. (FIRST 2 BYTES ONLY:
    //           nop + the rel32 jmp opcode; the trailing 4 displacement bytes
    //           9a 00 00 00 are LEFT INTACT and reused, so the jmp lands on the
    //           same 0x00733AAE -> unconditionally skips the vignette-draw block).
    if (g_cfg.no_vignette) {
        static const uint8_t k_nop2[2]   = { 0x90, 0x90 };  // 719B54 / 733BA7
        static const uint8_t k_nopjmp[2] = { 0x90, 0xE9 };  // 733A0E first 2 bytes
        int n = 0;
        __try {
            if (((volatile uint8_t *)0x00719B54u)[0] == 0x77 &&
                ((volatile uint8_t *)0x00719B54u)[1] == 0x0B) {
                if (patch_write(0x00719B54u, k_nop2, 2, "no-vignette 719B54")) n++;
            }
            if (((volatile uint8_t *)0x00733BA7u)[0] == 0x75 &&
                ((volatile uint8_t *)0x00733BA7u)[1] == 0x18) {
                if (patch_write(0x00733BA7u, k_nop2, 2, "no-vignette 733BA7")) n++;
            }
            if (((volatile uint8_t *)0x00733A0Eu)[0] == 0x0F &&
                ((volatile uint8_t *)0x00733A0Eu)[1] == 0x84) {
                if (patch_write(0x00733A0Eu, k_nopjmp, 2, "no-vignette 733A0E")) n++;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        log_line("[pso_widescreen] no-vignette: %d sites patched (719B54/733BA7/733A0E)", n);
    }

}

// ---- In-game scale + hook primitives ----------------------------------
static float hs_peek_f32(uint32_t va)
{
    return *(volatile float *)(uintptr_t)va;
}

// Value-guarded float store to .data: writes only on change (idempotent),
// a single aligned 4-byte store (atomic on x86), VirtualProtect-wrapped.
static void hs_poke_f32(uint32_t va, float v)
{
    volatile float *p = (volatile float *)(uintptr_t)va;
    __try {
        if (*p == v) return;                       // value-guard (idempotent)
        DWORD old;
        if (!VirtualProtect((LPVOID)(uintptr_t)va, 4, PAGE_EXECUTE_READWRITE, &old))
            return;
        *p = v;
        DWORD tmp; VirtualProtect((LPVOID)(uintptr_t)va, 4, old, &tmp);
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// ---- Scene/gate globals ------------------------------
#define HS_VA_PLAYER_ARRAY 0x00A94254u   // 12-slot in-game player array head
#define HS_VA_SCENE_IDX    0x00AAFC9Cu   // G_SCENE_IDX (0 = front-end)
#define HS_VA_QUEST_LOAD   0x00AAB378u   // quest/area-loading flag (!=0 while loading)
#define HS_VA_DESIGN_W     0x0098A4B8u   // design_w  (853.33 native / 1280 @hs1.5)
#define HS_VA_DESIGN_H     0x0098A4B4u   // design_h  (480 native / 720 @hs1.5)
#define HS_VA_AFFINE_X     0x00ACC0E8u   // 2D affine SCALE_X
#define HS_VA_AFFINE_Y     0x00ACC0ECu   // 2D affine SCALE_Y
#define HS_VA_UIQUAD       0x0082B440u   // RenderUIQuad entry (stock first byte 0x83)
#define HS_UIQUAD_STOCK_B0 0x83u

// hs_ingame() — true iff we are in a live lobby/area (player object exists).
// 12-slot scan of the player array 0x00A94254: any slot holding a heap-range
// ptr [0x00400000,0x40000000) means a live player object exists -> in-game.
// Reads are SEH-guarded (the array head can be garbage very early in a scene).
static int hs_ingame(void)
{
    __try {
        const volatile uint32_t *pa = (const volatile uint32_t *)(uintptr_t)HS_VA_PLAYER_ARRAY;
        int i;
        for (i = 0; i < 12; i++) {
            uint32_t p = pa[i];
            if (p >= 0x00400000u && p < 0x40000000u)   /* live player object ptr */
                return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
    return 0;
}

// Data-only in-game scale pin: writes design_w/h + 2D affine derived from
// HudScale, value-guarded. Called once per FE->IG transition, and only when
// hud_scale != 1.0 (the front-end is owned by the static bake).
static void worker_scale_poke(int in_game)
{
    if (!in_game) return;                          // front-end: leave to W1 + static bake

    // Canonical design dims straight from the scale ctx — they already carry game_aspect AND
    // hud_scale (= ws_compute_scale s->A/s->C = the front-end bake = Ephinea Stage-B). NOT a
    // local 16:9 recompute (which mis-scaled real 4:3), and NOT the stale in-game globals (the
    // FE->IG transition resets [0x0098A4B8]/[0x00ACC0E8] toward stock 640 / identity 1.0).
    float dw = g_scale.A;                                 // 853.333*hud_scale @16:9
    float dh = g_scale.C;                                 // 480*hud_scale
    if (!(dw > 1.0f && dh > 1.0f)) return;

    // affine = render_w/design_w — the LIVE backbuffer extent (== the VPEXT numerator the
    // engine's own front-end setter @0x0082F49A divides by), so in-game reproduces the
    // front-end affine and FILLS exactly at EVERY HudScale (design*affine == render). Reading
    // the real backbuffer (not a hardcoded 1920) is what fixes the >1080p under-fill.
    float rw = (float)g_scale.render_w; if (!(rw > 1.0f)) rw = 1920.0f;
    float rh = (float)g_scale.render_h; if (!(rh > 1.0f)) rh = rw * (dh / dw);
    float aff_x = rw / dw;
    float aff_y = rh / dh;
    hs_poke_f32(HS_VA_DESIGN_W, dw);
    hs_poke_f32(HS_VA_DESIGN_H, dh);
    hs_poke_f32(HS_VA_AFFINE_X, aff_x);
    hs_poke_f32(HS_VA_AFFINE_Y, aff_y);
}

// ---- reassert_ingame_hooks — re-install wiped detours ------
// Re-installs the effect-deanchor CALL redirects (kStripSites -> rb_deanchor_shim)
// that the FE->IG transition WIPES back to stock (`call 0x0082B158`). Idempotent +
// steady-state quiet: we only attempt the redirect on a site that has been wiped
// BACK to the stock target (current call dest == 0x0082B158); a site already
// pointing at our shim is silently skipped (no redirect_call mismatch-log spam).
// Returns a bitmask of the sites (re)installed THIS call (0 in steady state).
//
// This is the missing-piece for the in-game 16:9 wide-fill: the affine source
// bakes (apply_startup_bakes) make the HUD wide and survive the transition, but
// the deanchor that keeps photon/screen-space effects from inheriting that wide
// affine is a code-flow detour the transition reverts — so it must be re-asserted
// from a context the transition can't kill (the worker), every in-game tick.
static unsigned reassert_ingame_hooks(void)
{
    unsigned did = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t site = kStripSites[i];
        uint8_t *p = (uint8_t *)(uintptr_t)site;
        __try {
            if (p[0] != 0xE8) continue;                         // not a CALL (unexpected) -> leave
            uint32_t cur = (uint32_t)(site + 5 + *(int32_t *)(p + 1));
            if (cur != 0x0082B158u) continue;                   // already ours (or foreign) -> skip quietly
        } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        // site is stock (`call 0x0082B158`): (re)install our deanchor wrap.
        if (redirect_call(site, 0x0082B158u, (void *)&rb_deanchor_shim))
            did |= (1u << i);
    }
    // ALSO re-assert the in-game list-window bottom-anchor (Ephinea FUN_52d8cb90
    // port @0x0073FF92) — it is a code-flow detour wiped by the FE->IG transition.
    // lw_yanchor_redirect() is itself stock-guarded + steady-state quiet, and the
    // shim self-no-ops at design_h<=480 (so it is harmless when re-asserted at
    // hud_scale 1.0 in-game). bit 3 reports a (re)install this tick.
    if (g_cfg.enabled && lw_yanchor_redirect())
        did |= (1u << 3);
    return did;
}

// In-game re-assert. The FE->IG scene transition reverts our deanchor + ListWindow-Y
// CALL-site redirects to stock and resets the in-game design_w/h + affine globals.
// Re-applies them ONCE per transition: armed on the FE->IG rising edge, fired after
// the area-load flag clears. No polling, no per-frame writes.
static void ingame_reassert_on_transition(void)
{
    static int s_prev_ig = 0;
    static int s_pending = 0;
    __try {
        int ig      = hs_ingame();
        int loading = (*(volatile uint32_t *)(uintptr_t)HS_VA_QUEST_LOAD != 0u);
        if (ig && !s_prev_ig) s_pending = 1;   // FE->IG rising edge: arm
        if (!ig)              s_pending = 0;    // back to front-end: disarm
        if (s_pending && ig && !loading) {      // fire once, after the area loads
            reassert_ingame_hooks();            // deanchor + ListWindow-Y redirects
            if (g_scale.enabled)
                worker_scale_poke(1);           // design_w/h + affine pin (runs at hs==1.0 too:
                                                // the FE->IG transition resets the in-game affine
                                                // to identity, so 1.0 STILL needs re-pinning to fill)
            s_pending = 0;
        }
        s_prev_ig = ig;

        /* FIX B re-assert: the loading-ring affine wrap (vtable slot 0x00B3E9B8) must be
           present DURING the loading sub-state, which is exactly the FE->IG/area transition
           window (reassert_ingame_hooks only fires on the !loading post-transition edge, too
           late for the ring). The slot is .data and survives the transition, but a wrapper /
           another ASI could revert it; ld_ring_redirect is stock-guarded + steady-state quiet,
           so this is a cheap no-op once installed. Run it every frame, unconditional of the
           transition state machine above. */
        if (g_scale.enabled) ld_ring_redirect();

        /* clobber re-assert: a d3d8 wrapper or another ASI may re-init .data and
           revert our design_w (0x0098A4B8) to stock 640.0. One read/frame; re-run
           the whole table only on an actual revert (apply_bakes is idempotent). */
        if (g_scale.enabled && g_scale.A != 640.0f &&
            *(volatile float *)0x0098A4B8 == 640.0f) {
            log_line("[ws] clobber: design_w reverted to 640 -> re-asserting kBakes");
            apply_bakes(&g_scale);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// apply_engine_patches — every code-flow detour (CALL-site rewrite, IAT thunk,
// MinHook). Mechanism-classified, NOT by any anzz1/sodaboy label.
static void apply_engine_patches(const ws_scale_ctx *s)
{
    (void)s;  // reads g_cfg.hud_scale live inside the installers
    // Char-select 3D model pan: E8 redirect of the scene's ResetNPCCamera call.
    // World-space; survives PatchCharSelect=0 (orthogonal to the 2D layout).
    patch_charselect_model_pan();
    // Char-create needs no code-flow detour here: it is owned entirely by the static
    // apply_startup_bakes() pass (run from apply_static_patches, right after
    // apply_anzz1_widescreen) plus stock geometry — no per-frame hook (0x0082BB74 /
    // 0x0082B440 are byte-stock). The class-select right-pane CONTENT rows are
    // runtime-only widget data with no static source, so they are intentionally not
    // strided.
}

// ---- Entry ----

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        load_config();
        // FORCE file logging ON, regardless of INI/DebugLogsEnabled, so the
        // char-create intro audio/scene lifecycle is always captured + flushed
        // per line.
        g_cfg.debug_log = 1;
        resolve_log_path();
        // Truncate log on each launch so we don't accumulate noise.
        if (g_cfg.debug_log && g_cfg.log_path[0]) {
            DeleteFileA(g_cfg.log_path);
        }
        log_line("[pso_widescreen] attach pid=%lu cfg enabled=%d windowed=%d %dx%d debug=%d verbose=%d",
                 GetCurrentProcessId(), g_cfg.enabled, g_cfg.windowed,
                 g_cfg.width, g_cfg.height, g_cfg.debug_log,
                 g_cfg.debug_log_verbose);

        // Stack detection: classify which d3d8.dll is currently providing
        // Direct3DCreate8 so downstream behavior can adapt. The d3d8to11 wrapper
        // handles RHW-vert NDC mapping via its physical-viewport divide; Sodaboy /
        // native / d3d8to9 wrappers expect their own submission-space rules, which
        // we don't try to second-guess.
        detect_d3d8_stack();

        // MASTER GATE: Enabled=0 means STOCK CLIENT. Nothing past this runs —
        // no widescreen, no borderless/backbuffer rewrite, no boot poster, no
        // video, no device/IAT hooks. Leaving the device machinery live at
        // Enabled=0 (it used to install whenever VideoEnable/BootPoster were on)
        // was the Enabled=0 + Windowed=2 graphics corruption. A 4:3 display that
        // auto-disables widescreen also takes this path (fully stock).
        if (!g_cfg.enabled) {
            log_line("[pso_widescreen] Enabled=0 -> fully dormant (stock client)");
            return TRUE;
        }

        // PNG decode happens here (before any D3D init) so the texture upload
        // on first Present is just a CreateTexture+memcpy.
        boot_poster_init_from_cfg(g_cfg.boot_poster_path,
                                  g_cfg.boot_poster_enabled,
                                  g_cfg.boot_poster_disable_after_floor,
                                  g_cfg.boot_poster_disable_after_splash,
                                  g_cfg.boot_poster_max_seconds,
                                  g_cfg.boot_poster_max_screen_pct);

        // P3 video init (Stage 1). Independent of the widescreen master
        // switch, same as the boot poster. VideoEnable=0 (default) =>
        // mod_video_init early-returns dormant and on_present is a no-op.
        mod_video_init(g_cfg.video_path,
                       g_cfg.video_boot_path,
                       g_cfg.video_enabled,
                       g_cfg.video_skippable,
                       g_cfg.video_ffmpeg,
                       g_cfg.video_max_seconds,
                       g_cfg.video_skip_debounce_ms,
                       g_cfg.video_audio,
                       g_cfg.video_diag,
                       g_cfg.video_trigger,
                       g_cfg.video_decoder);

        const int needs_iat_hook =
            g_cfg.enabled || g_cfg.boot_poster_enabled || g_cfg.video_enabled;

        if (!g_cfg.enabled && !g_cfg.boot_poster_enabled && !g_cfg.video_enabled) {
            // All overlay features off — leave the IAT alone entirely.
            log_line("[pso_widescreen] Enabled=0, BootPosterEnabled=0, VideoEnable=0, staying dormant");
            return TRUE;
        }
        if (!g_cfg.enabled) {
            // Widescreen master off but boot poster on — IAT hook still
            // needed so we can patch the device vtable Present slot.
            log_line("[pso_widescreen] Enabled=0; boot poster only path");
        }
        else if (g_cfg.width <= 0 && g_cfg.height <= 0 && g_cfg.windowed == 1) {
            // Enabled=1 but no actual override values configured. Boot
            // poster / video may still want the IAT hook to install Present.
            if (!g_cfg.boot_poster_enabled && !g_cfg.video_enabled) {
                log_line("[pso_widescreen] no overrides configured, idle");
                return TRUE;
            }
            log_line("[pso_widescreen] no widescreen overrides; running for boot poster / video only");
        }
        (void)needs_iat_hook;
        // ---- THE ONE GATE ----
        // ONE master gate (g_scale.enabled, == g_cfg.enabled). No
        // widescreen_engine selector, no else branch. Everything inside runs
        // TOGETHER, de-conflicted: apply_static_patches (anzz1 bake one-shot ->
        // minimap bake-on-top -> engine-independent static companions, each
        // keeping its per-feature knob), then apply_engine_patches (cursor-warp
        // IAT, model-pan E8 redirect; the char-create composer MinHooks ride
        // along via the static anchor pass). detect_d3d8_stack() above logged
        // the wrapper for diagnostics; no behavior gates on it (wrapper-agnostic).
        if (g_scale.enabled) {
            log_line("[pso_widescreen] apply: anzz1 authoritative coords + companions (one gate, no engine selector)");
            apply_static_patches(&g_scale);
            apply_engine_patches(&g_scale);
        }
        // d3d8.dll might already be loaded (pulled in via the static
        // import on psobb.exe by the time DllMain runs for an ASI),
        // but we don't actually need its handle — we patch the IAT slot
        // in psobb.exe's image, which already points into d3d8.dll.
        HMODULE psobb = GetModuleHandleA(NULL);  // = psobb.exe base
        if (patch_iat(psobb, "d3d8.dll", "Direct3DCreate8",
                      (void *)&Hook_Direct3DCreate8,
                      (void **)&real_Direct3DCreate8)) {
            log_line("[pso_widescreen] IAT patched: real=0x%p",
                     (void *)real_Direct3DCreate8);
        } else {
            log_line("[pso_widescreen] IAT patch FAILED — d3d8.dll!Direct3DCreate8 not found in psobb.exe import table");
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_cfg.boot_poster_enabled) {
            boot_poster_log_summary();
        }
        if (g_cfg.video_enabled) {
            // Logs diagnostics AND tears down any live ffmpeg child so we
            // never leave a zombie decoder on exit (Job KILL_ON_JOB_CLOSE
            // is the backstop; this is the clean path).
            mod_video_log_summary();
        }
    }
    return TRUE;
}
