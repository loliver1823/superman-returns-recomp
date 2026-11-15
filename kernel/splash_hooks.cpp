// Splash-screen host-side hooks.
//
// The splash decoder/uploader/draw functions are absent from this XEX build
// (the trigger functions tail-call into __savegprlr compiler helpers). The
// game's PSM advances the splash filename at psm+0x18 each frame via
// FUN_822B7F38 (sub_822B7F38): strcpy(psm+0x18, filename); call trigger.
// We intercept sub_822B7F38 to observe the filename sequence
// (EA_HD_logo / EA_ID_OLD / DC_logo / WBIE_logo / DC_WB_legal / 0100) and,
// later, blit a host-side overlay of the matching asset through the D3D12
// backend.
//
// Mechanism: the recomp declares sub_822B7F38 as a weak alias to the strong
// __imp__sub_822B7F38. Providing a non-weak sub_822B7F38 here causes the
// linker to pick this override for BOTH direct callers (ppc_recomp.32.cpp)
// and any dispatch-table lookup.

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "export_registry.hpp"
#include "guest_call.hpp"  // iter-295g: drive-authoring helper (compile check; new drives use it)
#include "../app/log.hpp"
#include "../gfx/d3d12_backend.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

// iter-245d: one-shot .apt blob scanner, defined in apt_parse_trace.cpp.
extern "C" void sr_find_apt_blob(uint8_t* base);
// iter-251 / iter-295 embedded two blobs carved from the disc (ScrPressStart.AST's _level0, ui.AST's MAIN .apt +
// .const) for the retired SR_DRIVE_APTARM drive and the def-register decode. Nothing referenced them any more; the
// headers were deleted 2026-10-13 (C1 composition audit: game bytes never ship, not even in the source tree's build).
// The decode itself stays documented: the register entry sub_826AECC8 takes r5 = the raw .apt bytes and r6 = the raw
// .const bytes ("defHdr") - const+0x14 holds the movie offset into the .apt (0x98 for MAIN), const+0x1c the const table.
// iter-245i: the per-frame Apt EXECUTOR -- walks _level0's pending work-lists and
// builds queued movie loads (sub_826AF688). Never driven before; loadMovie queues a
// request on _level0 that nothing processes.
extern "C" void __imp__sub_826BF630(PPCContext& __restrict ctx, uint8_t* base);
// iter-245e: shared one-shot latch so the in-branch and decoupled loadMovie drives
// never both dispatch (two concurrent sub_826B8A90 deadlock the Apt string CS).
static std::atomic<bool> g_lmDispatched{false};

// Reach the original lifted body. __imp__* names are C-linkage in recomp.
extern "C" void __imp__sub_822B7F38(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_822A8C68(PPCContext& __restrict ctx, uint8_t* base);
// iter-340 FMV CAMPAIGN: the FE/FMV state machine per-frame update
// sub_8235C8D0(sm) — called by FeMasterTick_821494C0. State at sm+4:
// 0=intro-movie area/idle, 2=playing, 3/5=end, 4=menu. Probe logs whether
// this ticks at all (is FeMasterTick running?) and every state transition.
extern "C" void __imp__sub_8235C8D0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8235C780(PPCContext& __restrict ctx, uint8_t* base);
// iter-449t: the ev187 kill-switch dispatch (observe). IDA chain, this
// session: the ladder climber sets PSM+553=1 exactly when queueing movie
// "0100" (the title-card/starfield FMV); MoviePlayerOpen bulk-copies 552B
// from PSM+8 to SM+8 so SM+553 inherits the flag; the SM tick's case-2 then
// broadcasts event 187 EVERY TICK for that movie's whole life, and the
// movie dies the moment a slot ACCEPTS (observed 2.2s in — the visible
// starfield freeze). This probe logs each 187 attempt: the suppression
// vector's contents and the accept edge — naming which side times the kill.
extern "C" void __imp__sub_82181860(PPCContext& __restrict ctx, uint8_t* base);
// iter-31: cross-thread invocation of the PSM dispatcher from the kernel-side
// fabricator thread (the host fallback loop frequently hangs in a CS spin
// before it can drive sub_822AB748 itself, so we drive it from here too).
extern "C" void __imp__sub_822AB748(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_822A2590(PPCContext& __restrict ctx, uint8_t* base);  // iter-192: ActivateUI flow cmd
extern "C" void __imp__sub_826B05F0(PPCContext& __restrict ctx, uint8_t* base);  // iter-194: Apt service/update pass
extern "C" void __imp__sub_821516E0(PPCContext& __restrict ctx, uint8_t* base);  // iter-194g: FMUI ctor (lottery ender)
extern "C" void __imp__sub_828DBC88(PPCContext& __restrict ctx, uint8_t* base);  // iter-292h: FMUI static ctor — sets *(0x8295FC8C) vtable 0x820033E4 + FM-list registration
extern "C" void __imp__sub_8214BC88(PPCContext& __restrict ctx, uint8_t* base);  // iter-293: FMGameRtamIni — Function_8218F5F8 populates DAT_8296d8c8 (the menu-loader's arg)
extern "C" void __imp__sub_8218D128(PPCContext& __restrict ctx, uint8_t* base);  // iter-293: GOLDEN menu/UI asset-package loader (r3=DAT_8296d8c8) — sub_82182E58 x9 -> BigFileAsset::Load x16 (loads ui.AST). L9 golden path; sub_826D8A10/RtamPackage = 0 hits in golden.
extern "C" void __imp__sub_826D83F8(PPCContext& __restrict ctx, uint8_t* base);  // iter-293: RtamUtil::DefaultCreate — resolve a named sub-asset (r3=name C-str) from the registered ui.AST registry (magic 0x82088a34) -> returns BigFileAsset* in r3
extern "C" void __imp__sub_826D8480(PPCContext& __restrict ctx, uint8_t* base);  // iter-293: per-asset load driver (r3=asset) — calls asset->vt[3] (BigFileAsset::Load) init+real to fetch content bytes
extern "C" void __imp__sub_826E0130(PPCContext& __restrict ctx, uint8_t* base);  // iter-194h: Apt one-shot init (sets 0x829A2410)
extern "C" void __imp__sub_822A25E0(PPCContext& __restrict ctx, uint8_t* base);  // iter-196: FE command — ChangeInterfaceState(0,"FE") via g3->vt[7]
extern "C" void __imp__sub_826D2718(PPCContext& __restrict ctx, uint8_t* base);  // iter-196: client vt[3] = per-frame movie update (frame advance → AS exec)
extern "C" void __imp__sub_8271F848(PPCContext& __restrict ctx, uint8_t* base);  // iter-245m: resource-streaming I/O pump (zero-arg, self-pumping, never driven) — completes the .apt fetch
extern "C" void __imp__sub_826E83D0(PPCContext& __restrict ctx, uint8_t* base);  // iter-245o: BigFileAsset::Load — loose path (r5=0) opens+reads+mounts "<name>.AST"
extern "C" void __imp__sub_826E9650(PPCContext& __restrict ctx, uint8_t* base);  // iter-257: REAL BigFileAsset ctor (r3=this 0x230, r4=nameStr, r5/r6/r7=flags) — builds a PROPER asset for BFAL
extern "C" void __imp__sub_826AF460(PPCContext& __restrict ctx, uint8_t* base);  // iter-262: THE Apt ctx-graph builder (r3=ctx 0x30, r4=dims6) — allocs+links ctx+0x18 inst / +0x1c scope / +0x20 clip mgr
extern "C" void __imp__sub_826AECC8(PPCContext& __restrict ctx, uint8_t* base);  // iter-265: parse-entry (r3=ctx, r4=&holder, r5=blob-base, r6=DEF-HDR-PTR [iter-294: NOT a length!], r7=movieObj) — sets DAT_829A17BC=r3 + reloc(def+0x14 by blob)+REGISTER exports into ctx+0x1c. Golden call site = pump drain sub_826D23A0.
extern "C" void __imp__sub_826AF688(PPCContext& __restrict ctx, uint8_t* base);  // iter-295b: movie-build (r3=new 0x2c obj, r4=holder-slot host obj, r5=&holderPtr) — installs runtime-movie vtable 0x82087738, stores holder at +0x28.
extern "C" void __imp__sub_826B0358(PPCContext& __restrict ctx, uint8_t* base);  // iter-295b: def-install (r3=clip node, r4=movieObj, r5=1) — writes clip+0x20 (the movieDef slot) + registers clip into *(DAT_829A17BC+0x2c).
extern "C" void __imp__sub_826811C8(PPCContext& __restrict ctx, uint8_t* base);  // iter-295c: clip activate (r3=clip, r4=1) — sets active bit 0x08000000 in clip+4 under the 0x829a19a8 spinlock.
extern "C" void __imp__sub_826807F8(PPCContext& __restrict ctx, uint8_t* base);  // iter-295c: mark-for-exec (r3=clip, r4=1, r5=1) — sets clip+0xc |= 0x2000000 + propagates up the parent chain; requires wrapper+8 type + clip+4 low7 != 0x25.
extern "C" void __imp__sub_826804C0(PPCContext& __restrict ctx, uint8_t* base);  // iter-297d: has-event-handler check (r3=clip, r4=event mask; 2=enterFrame) — reads the AS-binding's event mask at obj+0x10, walks parent chain.
extern "C" void __imp__sub_826B2490(PPCContext& __restrict ctx, uint8_t* base);  // iter-297d: event-mask REFRESH (r3=clip) — scans the clip's AS properties against the event-name table 0x82931184 and ORs mask bits into binding+0x10; the engine's own post-classbind wiring (sole natural caller sub_826B8040). Returns 1 if enterFrame/frame-family found.
// iter-297d: sub_82688B90 (AS interpreter; strong def = apt_as_watch override, keeps
// [AS_EXEC] counting) is already declared by the recomp headers — call it directly.
extern "C" void __imp__sub_826B1040(PPCContext& __restrict ctx, uint8_t* base);  // iter-297d: AS name RESOLVER (r3=clip/scope, r4=&nameHolder, r5=altScope|0) — builtins, scope chain (clip->vt[2] binding + parents), then _global (DAT_829a1584). Returns property obj or 0.
extern "C" void __imp__sub_8268FB58(PPCContext& __restrict ctx, uint8_t* base);  // iter-297d: CALL AS FUNCTION OBJECT (r3=vm 0x829A19B0, r4=thisObj/clip, r5=fnObj, r6=argc, r7=argv) — used by class-bind sub_826B8040 + the pump's type-2 records.
extern "C" void __imp__sub_8229FD48(PPCContext& __restrict ctx, uint8_t* base);  // iter-297d: name-holder release (r3=&holder) — pairs with sub_8268F3E8.
extern "C" void __imp__sub_8268C198(PPCContext& __restrict ctx, uint8_t* base);  // iter-297d: NATURAL type-1 enqueuer (r3=frame-table base *(defObj+4)+0x10, r4=clip, r5=frameIdx) — walks the frame's items, enqueues each type-1 script into the exec-ctx ring *(17BC+0x18)+0xc with the ENGINE's own invocation setup (proper this/scope). Natural caller: ClipFrameAdvance.
extern "C" void __imp__sub_8214D950(PPCContext& __restrict ctx, uint8_t* base);  // iter-297f: FMGameRender INIT (descriptor 0x82003580 {.., 8214D950 init, 8214DB40 shutdown, "FMGameRender"}) — zero-arg, idempotent: builds the 0x1D-field render config + constructs the RENDERER into DAT_829A9384 (via sub_8278D258 pool alloc on heap DAT_8296D894 + ctor sub_827761F0). The buffer-replay consumer sub_821491A0 needs this object.
extern "C" void __imp__sub_82787650(PPCContext& __restrict ctx, uint8_t* base);  // iter-297f: RenderFrameBegin (r3 = ring ctx from DAT_829A93C8) — picks/opens a command buffer for the frame (agent map).
extern "C" void __imp__sub_82787730(PPCContext& __restrict ctx, uint8_t* base);  // iter-297f: RenderFrameEnd (r3 = ring ctx) — stamps/closes the buffer for the consumer.
extern "C" void __imp__sub_826A9978(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82697B30(PPCContext& __restrict ctx, uint8_t* base);  // iter-297j: render-node DIRTY MARK (r3=node hdr from Function_8268AF90, r4=0, r5=0)
extern "C" void __imp__sub_8268C868(PPCContext& __restrict ctx, uint8_t* base);  // iter-297j: render-node frame STAMP (r3=hdr, r4=frame)
extern "C" void __imp__sub_826866F0(PPCContext& __restrict ctx, uint8_t* base);  // iter-297k: SetCurrentAptContext (spinlocked; sets 0x829A17BC logic + 0x829A17C0 render ctx)
extern "C" void __imp__sub_82698370(PPCContext& __restrict ctx, uint8_t* base);  // iter-297k: commit one live object's render-node links (r3=*(ctx+0x2c) root handle slot, r4=live obj, r5=frame) — sets parent child (+0x30 via Function_82685B20), prev/next sibling (+0x2c via Function_82685950), or registry root (sub_826919F0)
extern "C" void __imp__sub_8269BEC8(PPCContext& __restrict ctx, uint8_t* base);  // iter-297k: render-node FACTORY (r3=charDef or 0, r4=frame) — NULL def = generic container node (0x34 bytes, vt 0x82086D84, flags bit31|0x3C0000, depth 0xFFFF). Handle ctor Function_826A04E8 only calls this when DAT_829A17BC published — the type-9 hdl+4==0 root cause.
extern "C" void __imp__sub_8268AF90(PPCContext& __restrict ctx, uint8_t* base);  // iter-297j: wrapper -> render-node hdr accessor  // iter-297j: AS EVENT DISPATCH (r3=clip, r4=event bit [2=enterFrame], r5=*(0x829A1764) arg obj, r6=1) — mask check + per-listener enqueue. The transition closures (onEnterFrame fades) only run when this fires per frame.
extern "C" void __imp__sub_82680EB8(PPCContext& __restrict ctx, uint8_t* base);  // iter-297j: AS builtin-name matcher (r3=chars, r4=len) -> descriptor {strPtr,id} in the table at 0x829380B8 ('_global' -> id 0x13) or 0.
extern "C" void __imp__sub_826B3998(PPCContext& __restrict ctx, uint8_t* base);  // iter-297j: dotted-path resolver (r3=vm 0x829A19B0, r4=root movie/clip, r5=0, r6=&nameHolder, r7=1, r8=1, r9=0) — the wrapper AsInvokeByPath_sub_826B8DC0 resolves "_global.FlowMgr" through this. PURE lookup (iter-193).
extern "C" void __imp__sub_8269B0B0(PPCContext& __restrict ctx, uint8_t* base);  // iter-297j: LEVEL-LIST register (r3 = list holder *(execCtx+0x20), r4 = level number, r5 = clip). Natural callers: sub_826B05F0 (lazy stand-in create) + sub_826B6F00 (loadMovieNum).
extern "C" void __imp__sub_82696FD0(PPCContext& __restrict ctx, uint8_t* base);  // iter-297j: LEVEL-LIST unlink (r3 = clip) — sub_826B6F00's re-level path calls this before re-registering.
extern "C" void __imp__sub_8214FF48(PPCContext& __restrict ctx, uint8_t* base);  // iter-297h: FMGameEntity INIT (descriptor 0x82002D40 {8271F428, 8214B298 custom-middle, 82196960, 8214FF48 init, 8214B2A0 shutdown, "FMGameEntity"}) — the SCREEN SYSTEM builder: creates the screen registry map DAT_8296DD0C (via sub_821C2578), screen-list managers DAT_8296DD30/DD2C (sub_821D0A00), a dozen pool globals DAT_8296DCxx/DFxx, + sub-inits. NEVER runs on our boot: the custom middle-triplet slot (8214B298 vs shared 8231C140) made it INVISIBLE to the fm_module_walker's triplet scan. r3 = module obj — only writes/reads +0x48 (pool wrapper); a zeroed scratch object suffices.
extern "C" void __imp__sub_821745D0(PPCContext& __restrict ctx, uint8_t* base);  // iter-297g: SetInterfaceState(r3=AUI mod, r4=stateId, r5=mode) — THE FE interface state machine setter. Selector table state→client: 0=+0x220 base, 1=+0x7F0, 2=+0xD60, 3=+0xEA0, 4=+0xF50 (Apt render client, GOLDEN menu state), 5=+0xF70, 6=+0x1060, 7=+0x1150, 8=+0x1220 transition, 9=NULL movie. Writes +0x1310 selector AND +0x1320 state, then client->vt[0x14] activate (SKIPPED when mode==3). Guard: *(mod+0x10) must be 0 or ==mode, else request pends at +0x1328.
// sub_821808F8 (AUI render dispatch; strong def = apt_render_watch override; f1 = dt double,
// r4 = AUI module) is already declared by the recomp headers — call it directly.
extern "C" void __imp__sub_826BE8C0(PPCContext& __restrict ctx, uint8_t* base);  // iter-295c: per-clip FRAME-ADVANCE (r3=clip) — gated on clip+0xc&0x2000000; type(wrapper+8>>26) must be 5/9; executes frame items via sub_826BE100 from the def at wrapper+4.
extern "C" void __imp__sub_826BF630(PPCContext& __restrict ctx, uint8_t* base);  // iter-295d: THE EXECUTOR (r3=*(ctx+0x20) clip manager) — pending-load matching + first-frame kicks + tail screen pump; natural caller sub_826C04F8 passes *(DAT_829a17bc+0x20).
extern "C" void __imp__sub_826BF268(PPCContext& __restrict ctx, uint8_t* base);  // iter-295d: SCREEN/EXEC pump (r3=*(ctx+0x18) exec ctx) — drains the queued work ring, pushes/pops the Apt screen stack 0x829A19B0/19B8 (aptCount/aptBase), first-frame kick sub_826BF198.
extern "C" void __imp__sub_821494C0(PPCContext& __restrict ctx, uint8_t* base);  // iter-295e: THE MASTER PER-FRAME FE TICK (FSGameSimulation update slot, descriptor vtable 0x82001888 idx5; ctor sub_821492E8 makes obj {vt,+4=1}). Runs the whole natural cascade: FE state machine sub_8235C8D0->sub_8235C780, PSM tick, resMgr pump (*DAT_8296ec4c+0x1c)(), flow-client dispatch (*EB80+0xc)(EB80,-1) -> ChangeInterfaceState -> screens. NEVER dispatched on our boot (game loop only drains the worker queue).
extern "C" void __imp__sub_82700238(PPCContext& __restrict ctx, uint8_t* base);  // iter-294: BigFileAsset content READ+parse (r3=defObj asset+0x208, r4=*(defObj+0x110), r5=mode[!=1], r6=1) — Function_826FF908 opens stream +0x114, memcpy-reads blob (base=defObj+0x100 pkg offset) + finalizes +0x124 descriptor. Deferred by the mode-0 registry path; drive it to load content.
extern "C" void __imp__sub_826D2690(PPCContext& __restrict ctx, uint8_t* base);  // iter-283: NATIVE load-by-name (r3=name C-str, r4=&holder) — derives mgr from resMgr DAT_829A23E0+0xd4 -> sub_826D91F0 -> async enqueue->pump->poster->drain->REGISTER. The clean trigger Xenia uses (golden: 1 call, from sub_826D220C).
extern "C" void __imp__sub_826D4AB0(PPCContext& __restrict ctx, uint8_t* base);  // iter-285: scopeCreate (resMgr->vt[0x10]); allocs 0xe8 scope + Function_826D3FD0(scope,ctx) whose tail = KeTlsSetValue(*(resMgr+0xd4) index, scope). resMgr+0xd4 is a TLS slot, so drive on the CONSUME thread.
extern "C" void __imp__sub_826D7850(PPCContext& __restrict ctx, uint8_t* base);  // iter-286: POSTER (loader, subAsset, status>=0) — stash sub-asset, dec refcount loader+0x18; on 0 post node to *(loader+0x10)+0x7c
extern "C" void __imp__sub_825F70C0(PPCContext& __restrict ctx, uint8_t* base);  // iter-297c: get-or-create the per-thread AS-executor (TLS slot *(resMgr+0xd4))
extern "C" void __imp__sub_826D2718(PPCContext& __restrict ctx, uint8_t* base);  // iter-286: pump (mgr) -> drain mgr+0x7c -> register into scope
// iter-286: captured during the native enqueue (sub_826D5FE8 override in apt_parse_trace.cpp) so the
// LOADPKG poster-drive can complete the 2-part load without the deadlocking streaming pump.
namespace sr { namespace kernel { std::atomic<uint32_t> g_enqLoaderVA{0}; std::atomic<uint32_t> g_enqAssetVA{0}; std::atomic<uint32_t> g_menuCtx{0}; std::atomic<uint32_t> g_lastClipNode{0}; std::atomic<uint32_t> g_fabMgrVA{0}; std::atomic<uint32_t> g_ovlClipLatch{0}; uint32_t aptAsRunCount(); void aptRenderWatchLine(char*, size_t); extern std::atomic<uint32_t> g_liveInsertRing[256]; extern std::atomic<uint32_t> g_liveInsertN; } }  // aptAsRunCount: apt_as_watch.cpp; aptRenderWatchLine + g_liveInsert*: apt_render_watch.cpp

// iter-463 charter: the dead FMUI static-ctor/GlobalInit fab-drive (fireFmuiCtorDrive,
// srT2Skip, the g_fmuiDrive* state, fmuiMgrReal) was deleted. It had no callers since
// the iter-378 fabricator removal and would have host-called the FMUI ctor and zeroed
// guest spinlocks 0x829A1A20/0x829A19A8. Natural module init runs sub_821516E0 itself.
extern "C" void __imp__sub_8268F3E8(PPCContext& __restrict ctx, uint8_t* base);  // iter-245x: intern raw char* -> name-object holder
extern "C" void __imp__sub_82682C08(PPCContext& __restrict ctx, uint8_t* base);  // iter-251: SOLE arm of the Apt bytestream globals DAT_829A0A90/_A94/_A98 (boot-asset stream,len)
extern "C" void __imp__sub_826C0888(PPCContext& __restrict ctx, uint8_t* base);  // iter-252: per-frame Apt-stream PUMP; armed (DAT_829A0A90!=0) -> sub_826C04F8 interprets the stream
extern "C" void __imp__sub_8269B658(PPCContext& __restrict ctx, uint8_t* base);  // iter-245x: get-or-create resource by name + register in ctx table (2nd fetch-enqueue path)
extern "C" void __imp__sub_82692640(PPCContext& __restrict ctx, uint8_t* base);  // iter-197: Level::Update (level vtable slot 1 → frame exec sub_8268EF50)
extern "C" void __imp__sub_82680778(PPCContext& __restrict ctx, uint8_t* base);  // iter-194d: Apt manager service (mgr non-null branch)
extern "C" void __imp__sub_8236DD90(PPCContext& __restrict ctx, uint8_t* base);  // iter-198: package mounter — mounts next pending pkg on the streamer
extern "C" void __imp__sub_826B8A90(PPCContext& __restrict ctx, uint8_t* base);  // iter-198: loadMovie(name, level) — raw char* args, queues on [[0x829A17BC]+32]
// Forward decl of the fake stub (C++ linkage, defined in ppc_missing_recomp_stubs.cpp).

// /loop iter-9 (iter-352 CHARTER-GATED): the REAL sub_827759B8 returns *(r3+0x66)
// (the renderer-ready byte); sub_822A8C68's splash-advance short-circuits when it
// is nonzero. Faking 0 UNCONDITIONALLY forced the splash gate open = FORCING (it
// makes the guest's decision) and it was UNGATED — active on the default boot,
// which contaminated the "natural" baseline and drove FUN_822b7f38 (a movie-play).
// Default now runs the REAL accessor so the game decides; the force-open is opt-in
// diagnostic behind the master gate. force -> diagnose -> root-fix -> confirm -> delete.
extern "C" void __imp__sub_827759B8(PPCContext& __restrict ctx, uint8_t* base);
void sub_827759B8(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool s_force = [] {
    if (!sr::kernel::forcingAllowed()) return false;
    const char* e = std::getenv("SR_FORCE_SPLASH_GATE");
    return e && e[0] == '1';
  }();
  if (s_force) { ctx.r3.u32 = 0; return; }   // diagnostic: force the splash gate open
  __imp__sub_827759B8(ctx, base);            // natural: return the real renderer-ready byte
}

// iter-DOGMA-RE-14: stop the splash from cycling.
//
// sub_822A8C68 is the splash state's per-tick handler. When the counter at
// psm+0x230 exceeds 5, it sets done-flag at psm+0x234 = 1, resets counter
// to -1, and calls Function_82470700 (a Xam IORequest, not the actual menu
// state transition).
//
// The PROPER PSM dispatcher (sub_822AB748) then routes to the next state
// because counter < 0. But our host fallback in ppc_recomp.5.cpp calls
// sub_822A8C68 directly, bypassing the dispatcher's check — every call
// bumps counter from -1 → 0 → 1 → loads "EA_HD_logo" → cycle restart.
// Observed: 8 splash hits in iter-11b (5 logos + "0100" + 2 from cycle).
//
// Strong-override: when done flag is already set, no-op. Breaks the cycle
// regardless of caller (host fallback OR dispatcher). The proper dispatcher
// path then handles the state transition naturally.
void sub_822A8C68(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t psmVA = ctx.r3.u32;
  // Read done flag at psm+0x234 directly (avoid the FmGamePlayStateMgr
  // singleton lookup — caller passed the instance in r3 already).
  uint8_t done = 0;
  if (uint8_t* host = sr::kernel::guestVAToHost(psmVA + 0x234)) {
    done = *host;
  }
  // iter-318d: the old blanket done=1 skip BLOCKED THE FE TRANSITION: the
  // fabricator sets done=1 at its tick-1, and every NATURAL stepper call
  // thereafter was skipped — the counter could never pass 5, so the
  // splash-done transition sub_82470700 → sub_824704B0(2) (the REAL FE
  // state-2 sender; this fn's old comment dismissing it was wrong) never
  // fired even after the XamGetSystemVersion root fix. Keep the guard ONLY
  // for the restart case it was written for (counter already reset to -1);
  // let a live climb (0..5) through. SR_SPLASH_SKIP_ALL=1 restores blanket.
  if (done) {
    static const bool s_blanket = [] {
      const char* e = std::getenv("SR_SPLASH_SKIP_ALL");
      return e && e[0] == '1';
    }();
    uint32_t counter = 0xFFFFFFFFu;
    if (uint8_t* h = sr::kernel::guestVAToHost(psmVA + 0x230)) {
      counter = (uint32_t(h[0]) << 24) | (uint32_t(h[1]) << 16) |
                (uint32_t(h[2]) << 8) | uint32_t(h[3]);
    }
    if (s_blanket || counter > 5u) {   // -1/garbage = post-done restart
      static std::atomic<unsigned> s_skipCount{0};
      unsigned n = s_skipCount.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 4 || (n & (n - 1)) == 0) {
        SR_LOG_BOOT(sr::LogSeverity::Info,
          "[SPLASH] sub_822A8C68 SKIP #%u psm=0x%08X done=1 counter=%08X — preventing cycle restart",
          n, psmVA, counter);
      }
      return;
    }
    static std::atomic<unsigned> s_passCount{0};
    unsigned p = s_passCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (p <= 8)
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[SPLASH] sub_822A8C68 PASS-THROUGH #%u (done=1 but counter=%u climbing)",
        p, counter);
  }
  __imp__sub_822A8C68(ctx, base);
}

namespace sr::splash {

// Most-recently-observed splash basename. Populated by the hook, read by
// future D3D12 overlay path.
std::atomic<uint32_t> g_currentFilenameGuestVA{0};

static void readGuestCString(uint32_t guestVA, char* dst, size_t dstCap) {
  if (dstCap == 0) return;
  dst[0] = '\0';
  uint8_t* host = sr::kernel::guestVAToHost(guestVA);
  if (!host) return;
  size_t i = 0;
  for (; i + 1 < dstCap; ++i) {
    char c = static_cast<char>(host[i]);
    if (c == '\0') break;
    dst[i] = c;
  }
  dst[i] = '\0';
}

}  // namespace sr::splash

// Strong override of sub_822B7F38. The original would do strcpy(psm+0x18, r4)
// then call the trigger. Since r4, the splash array, AND the .rodata strings
// at 0x8201BBxx all get clobbered post-init in this build, we (a) supply the
// filename string ourselves and (b) bypass __imp__'s strcpy by writing the
// chosen filename directly into psm+0x18, then jump straight to the trigger.
//
// This is the smallest change that actually delivers the correct splash name
// to downstream consumers (Function_8235F2D0 which reads psm+0x18).
void sub_822B7F38(PPCContext& __restrict ctx, uint8_t* base) {
  static constexpr const char* kNames[6] = {
    "EA_HD_logo", "EA_ID_OLD", "DC_logo", "WBIE_logo", "DC_WB_legal", "0100"
  };
  // Per-logo screen tints — the original assets aren't in the XEX, so the
  // D3D12 default-clear path picks up these RGBA values via
  // sr::gfx::setSplashOverlayColor() to give a visible boot-time signal.
  // EA_HD_logo  → EA blue;     EA_ID_OLD → orange (Insomniac/Dev brand);
  // DC_logo     → DC red;      WBIE_logo → WB shield gold;
  // DC_WB_legal → grey;        "0100"     → 0 (overlay off, end of seq).
  static constexpr uint32_t kColors[6] = {
    0x143FB4FFu,  // EA_HD_logo:   EA blue (#143FB4)
    0xE07020FFu,  // EA_ID_OLD:    orange (#E07020)
    0xB31818FFu,  // DC_logo:      DC red (#B31818)
    0xE6C429FFu,  // WBIE_logo:    WB shield gold (#E6C429)
    0x666666FFu,  // DC_WB_legal:  grey (#666666)
    0x00000000u,  // "0100":       overlay off
  };
  static std::atomic<unsigned> s_callIdx{0};
  unsigned idx = s_callIdx.fetch_add(1, std::memory_order_relaxed);
  unsigned slot = idx % 6;
  const char* name = kNames[slot];
  const uint32_t psmVA = ctx.r3.u32;

  // iter-340c: the fake splash is a band-aid from the flaky-walker era. Now the
  // real native presentation SM runs (state 0->1 proven), so the name-
  // substitution here is actively injecting fake logo names into the real movie
  // queue (sub_822B7F38 -> Function_8235F2D0 reads psm+0x18). SR_FAKE_SPLASH=0
  // retires it: log the game's OWN intended name (arrives at r4=param_2), do NOT
  // substitute, do NOT paint the color overlay — let the natural sequence and
  // its real movie name flow through so the decoder gets correct input and we
  // can see whatever the real render path draws.
  // 2026-07-13 PRIME DIRECTIVE: name substitution is FORCING — requires the
  // master gate AND the legacy knob. Default = pass-through (natural).
  static const bool s_fakeSplash = [] {
    const char* e = std::getenv("SR_FAKE_SPLASH");
    return sr::kernel::forcingAllowed() && (!e || e[0] != '0');
  }();
  if (!s_fakeSplash) {
    char real[64] = {0};
    if (uint8_t* src = sr::kernel::guestVAToHost(ctx.r4.u32)) {
      for (int i = 0; i < 63 && src[i] != '\0'; ++i) {
        real[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : '?';
      }
    }
    if (idx < 50) {
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[SPLASH_REAL] sub_822B7F38 hit#%u PASS-THROUGH game-name=\"%s\" psm=0x%08X r4=0x%08X",
        idx + 1, real, psmVA, ctx.r4.u32);
    }
    __imp__sub_822B7F38(ctx, base);
    return;
  }

  // Write the name into psm+0x18 directly. Avoids the corrupt-string strcpy.
  if (uint8_t* dst = sr::kernel::guestVAToHost(psmVA + 0x18)) {
    size_t n = 0;
    while (name[n] != '\0' && n < 62) { dst[n] = static_cast<uint8_t>(name[n]); ++n; }
    dst[n] = '\0';
  }
  sr::splash::g_currentFilenameGuestVA.store(psmVA + 0x18, std::memory_order_relaxed);
  sr::gfx::setSplashOverlayColor(kColors[slot]);

  if (idx < 50) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[SPLASH] sub_822B7F38 hit#%u WROTE_INPLACE name=\"%s\" psm=0x%08X overlay=0x%08X",
      idx + 1, name, psmVA, kColors[slot]);
  }

  // Still call the original — it does state transitions beyond the strcpy
  // (sets +0x229 strcmp-with-"0100" flag, calls Function_8235F2D0). Our
  // pre-write of psm+0x18 ensures the caller's strcpy reads our string.
  ctx.r4.u32 = psmVA + 0x18;  // make r4 point at our pre-written name
  __imp__sub_822B7F38(ctx, base);
}

// iter-340 FMV CAMPAIGN probe: FE/FMV state-machine per-frame update.
// Answers the two open runtime questions with zero guesswork:
//   (1) does sub_8235C8D0 tick at all? (i.e. is FeMasterTick running the
//       FMV SM update, or is the whole intro path dark?)
//   (2) what states does sm+4 pass through, and does it ever reach case 4
//       (sub_8235C780 decode/present)?
// Low-noise: logs the first few calls, then only on state CHANGE. Gate
// SR_FMV_PROBE=0 to silence.
static uint32_t sr_fmv_be32(uint8_t* base, uint32_t va) {
  if (uint8_t* p = sr::kernel::guestVAToHost(va)) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  }
  return 0xFFFFFFFFu;
}
void sub_82181860(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t ev = ctx.r5.u32;
  if (ev != 187u) { __imp__sub_82181860(ctx, base); return; }
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t slot = ctx.r4.u32;
  // Suppression vector: obj at *(0x8296D888), begin/end pointers at +0/+4.
  uint32_t ids[8] = {0};
  uint32_t cnt = 0;
  const uint32_t vec = sr_fmv_be32(base, 0x8296D888u);
  if (vec >= 0x1000u) {
    const uint32_t b = sr_fmv_be32(base, vec);
    const uint32_t e = sr_fmv_be32(base, vec + 4u);
    if (e >= b && e - b <= 0x100u) {
      cnt = (e - b) / 4u;
      for (uint32_t i = 0; i < cnt && i < 8u; ++i)
        ids[i] = sr_fmv_be32(base, b + 4u * i);
    }
  }
  __imp__sub_82181860(ctx, base);
  const uint32_t rc = ctx.r3.u32;
  // Log the first attempts and EVERY accept (the kill edge).
  if (n <= 40 || (n & (n - 1)) == 0 || (rc & 0xFFu)) {
    const uint32_t sm = sr_fmv_be32(base, 0x829761E0u);
    const uint32_t psm = sr_fmv_be32(base, 0x8296EDACu);
    const uint8_t* sm553 = sr::kernel::guestVAToHost(sm + 553u);
    const uint8_t* psm553 = sr::kernel::guestVAToHost(psm + 553u);
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[EV187] #%u slot=%u rc=%02X %s | suppressed[%u]=%u,%u,%u,%u,%u,%u,%u,%u "
        "| sm553=%u psm553=%u cursor=%d",
        n, slot, rc & 0xFFu, (rc & 0xFFu) ? "ACCEPTED-KILL" : "declined", cnt,
        ids[0], ids[1], ids[2], ids[3], ids[4], ids[5], ids[6], ids[7],
        sm553 ? *sm553 : 0u, psm553 ? *psm553 : 0u,
        (int)sr_fmv_be32(base, psm + 560u));
  }
}

void sub_8235C8D0(PPCContext& __restrict ctx, uint8_t* base) {
  // iter-365m SR_FMV_HOLD=<sec> (opt-in, default off): stretch the intro-FMV
  // window toward console duration. On console the EA/WB intro movies PLAY for
  // tens of seconds and the world-preload conveyor runs inside that window. Our
  // XMV create fails instantly (hr=8000FFFF, the demuxer wall), the SM bails
  // ~8s in, and the bail-path teardown collides with the still-mid-flight paced
  // preload — iter-365m traced BOTH stall shapes to this collision (the
  // 826F4740 drain-park and the freed-iterator flat run). Hold = defer this
  // outer SM tick while the FMV phase (0x829761E0+4) is still 0 (intro), for at
  // most N wall-clock seconds; then ticking resumes and the SM runs its natural
  // bail. Same timing-lever family as SR_CONVEYOR_PACE; retired by real XMV
  // playback (docs/BANDAIDS.md).
  static const unsigned s_holdSec = [] {
    const char* e = std::getenv("SR_FMV_HOLD");
    return e ? (unsigned)strtoul(e, nullptr, 10) : 0u;
  }();
  if (s_holdSec) {
    const uint32_t phase = sr_fmv_be32(base, 0x829761E4u);
    if (phase == 0u) {
      static const auto s_t0 = std::chrono::steady_clock::now();
      const auto held = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - s_t0).count();
      if ((unsigned)held < s_holdSec) {
        static std::atomic<unsigned> s_held{0};
        unsigned h = s_held.fetch_add(1, std::memory_order_relaxed);
        if (h == 0 || (h & (h - 1)) == 0)
          SR_LOG_BOOT(sr::LogSeverity::Info,
              "[FMV_HOLD] deferring intro SM tick #%u (%llds/%us)", h + 1,
              (long long)held, s_holdSec);
        return;  // movie "still playing" — tick deferred, nothing fabricated
      }
      static std::atomic<bool> s_expired{false};
      bool exp = false;
      if (s_expired.compare_exchange_strong(exp, true))
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[FMV_HOLD] window expired (%us) — resuming natural SM ticks", s_holdSec);
    }
  }
  static const bool s_probe = [] {
    const char* e = std::getenv("SR_FMV_PROBE");
    return !e || e[0] != '0';
  }();
  if (s_probe) {
    const uint32_t sm = ctx.r3.u32;
    const uint32_t state = sr_fmv_be32(base, sm + 4u);
    static std::atomic<uint32_t> s_lastState{0xDEADBEEFu};
    static std::atomic<unsigned> s_calls{0};
    unsigned n = s_calls.fetch_add(1, std::memory_order_relaxed);
    uint32_t prev = s_lastState.exchange(state, std::memory_order_relaxed);
    if (n < 4 || prev != state) {
      // sm+8 holds the queued movie descriptor (name at sm+8..). Peek the
      // first bytes of the pending movie name for context.
      char nm[24] = {0};
      if (uint8_t* p = sr::kernel::guestVAToHost(sm + 8u)) {
        for (int i = 0; i < 23 && p[i] >= 0x20 && p[i] < 0x7F; ++i) nm[i] = (char)p[i];
      }
      // iter-440k: the chain advances via EVENT 187 to listeners of
      // mgr=*(0x8296D884) (count at mgr+28, getter sub_82162FC0). count=0 on
      // an idle roll = registration race; >0 = listener declines.
      const uint32_t evMgr = sr_fmv_be32(base, 0x8296D884u);
      const uint32_t evListeners =
          evMgr ? sr_fmv_be32(base, evMgr + 28u) : 0xFFFFFFFFu;
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FMV_SM] call#%u sm=0x%08X state=%u (prev=%u) pendName=\"%s\" "
        "ev187mgr=%08X listeners=%u finFlag=%u contCb=%08X "
        "fe=%08X cursor=%d doneFlag=%u",
        n + 1, sm, state, prev, nm, evMgr, evListeners,
        sr::kernel::guestVAToHost(sm + 553u)
            ? *sr::kernel::guestVAToHost(sm + 553u) : 0u,
        sr_fmv_be32(base, sm + 556u),
        // iter-440l: the SEQUENCER state - cursor (-1 = not running) and the
        // done flag. fe = *(0x8296EDAC); cursor @fe+560, done @fe+564.
        sr_fmv_be32(base, 0x8296EDACu),
        (int32_t)sr_fmv_be32(base, sr_fmv_be32(base, 0x8296EDACu) + 560u),
        sr::kernel::guestVAToHost(sr_fmv_be32(base, 0x8296EDACu) + 564u)
            ? *sr::kernel::guestVAToHost(sr_fmv_be32(base, 0x8296EDACu) + 564u) : 0u);
    }
  }
  __imp__sub_8235C8D0(ctx, base);
}

// iter-340: the decode/present worker (state-4 case of the SM update). If
// this fires, the SM reached the frame-work stage — decode is being attempted.
void sub_8235C780(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool s_probe = [] {
    const char* e = std::getenv("SR_FMV_PROBE");
    return !e || e[0] != '0';
  }();
  if (s_probe) {
    static std::atomic<unsigned> s_calls{0};
    unsigned n = s_calls.fetch_add(1, std::memory_order_relaxed);
    if (n < 8 || (n & (n - 1)) == 0) {
      const uint32_t sm = ctx.r3.u32;
      const uint32_t sub = sr_fmv_be32(base, sm + 4u);
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FMV_DECODE] sub_8235C780 call#%u sm=0x%08X substate=%u — decode/present worker reached",
        n + 1, sm, sub);
    }
  }
  __imp__sub_8235C780(ctx, base);
}

// iter-37 (roadmap pivot — visible splash regardless of walker state).
//
// Walker is race-flakey ~2/3 — when it hangs in some FM module CTOR, the
// guest splash state machine never advances and we never see the overlay.
// iter-36 tried driving advance from inside the walker loop but the loop is
// bursty (29 fast ctors then a long hang), so only 1 advance fires before
// the hang.
//
// Pivot: a kernel-side host thread drives `setSplashOverlayColor()` directly
// every 2.5s, cycling EA blue → orange → DC red → WB gold → grey → off.
// Bypasses the guest splash state machine entirely; the user sees the
// expected color sequence on screen even on flakey walker runs.
//
// Tradeoff: this is purely VISUAL — the guest's splash counter at
// PSM+0x230 doesn't actually advance (so Function_82470700 / post-splash
// transition won't fire from this path). For "see splash on screen" it's
// sufficient. For "drive game past splash into menu" we still need the
// guest path; this thread just provides the visible overlay regardless.
//
// Started from kernel harness on first sub_822B7F38 hit OR after walker
// completes — see startKernelSplashOverlayThread().
namespace {
std::atomic<bool> g_kernelSplashThreadStarted{false};
}

void startKernelSplashOverlayThread() {
  bool already = g_kernelSplashThreadStarted.exchange(true, std::memory_order_acq_rel);
  if (already) return;
  // iter-340c: SR_FAKE_SPLASH=0 retires ONLY the fake color-overlay thread (a
  // purely-visual band-aid that paints over the real render path). The
  // fabricator/dispatcher thread further down is the real presentation DRIVER
  // and MUST still run — an earlier version of this gate wrongly early-returned
  // and killed the driver too, regressing the FMV SM to state 0.
  // 2026-07-13 PRIME DIRECTIVE: the overlay is FORCING (fakes visible output) —
  // requires the master gate too. Default = no overlay.
  static const bool s_fakeSplash = [] {
    const char* e = std::getenv("SR_FAKE_SPLASH");
    return sr::kernel::forcingAllowed() && (!e || e[0] != '0');
  }();
  if (!s_fakeSplash) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[SPLASH] fake color-overlay OFF (master-gated; all splash fab/driver paths "
      "in this file are forcingAllowed()-gated and inert on the default exe)");
  }
  if (s_fakeSplash) std::thread([]() {
    static constexpr uint32_t kColors[5] = {
      0x143FB4FFu,  // EA_HD_logo:   EA blue
      0xE07020FFu,  // EA_ID_OLD:    orange
      0xB31818FFu,  // DC_logo:      DC red
      0xE6C429FFu,  // WBIE_logo:    WB shield gold
      0x666666FFu,  // DC_WB_legal:  grey (held after sequence)
    };
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[SPLASH] kernel-side overlay thread started — driving visual splash regardless of walker state");
    for (size_t slot = 0; slot < 5; ++slot) {
      sr::gfx::setSplashOverlayColor(kColors[slot]);
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[SPLASH] kernel-thread step %zu/5 overlay=0x%08X",
        slot + 1, kColors[slot]);
      std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    }
    // iter-273: after the intro logo sequence, show the host-rendered REAL
    // press-start screen (decoded offline from ScrPressStart.AST -> .apt parse +
    // X360 DXT5 textures, composited at the real display-list positions). Loads
    // press_start.rgba (1280x720 RGBA) next to the exe. Gated SR_SHOW_PRESS_START.
    const char* showPS = std::getenv("SR_SHOW_PRESS_START");
    if (showPS && showPS[0] == '1') {
      std::ifstream f("press_start.rgba", std::ios::binary);
      if (f) {
        std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
        const size_t need = static_cast<size_t>(1280) * 720 * 4;
        if (img.size() >= need) {
          sr::gfx::setHostPresentImage(img.data(), 1280, 720);
          SR_LOG_BOOT(sr::LogSeverity::Info,
            "[PRESS_START] host present-image set: REAL ScrPressStart screen (%zu bytes, 1280x720)",
            img.size());
        } else {
          SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[PRESS_START] press_start.rgba too small (%zu < %zu)", img.size(), need);
        }
      } else {
        SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[PRESS_START] press_start.rgba not found next to exe — run from build/");
      }
    } else {
      // Leave overlay at grey (kColors[4]) — don't clear, so the user sees
      // the screen tinted instead of going back to dark blue.
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[SPLASH] kernel-thread sequence complete; overlay held at grey 0x666666FF");
    }
  }).detach();

  // ========== iter-31 kernel fabricator + dispatcher driver - DELETED (iter-378) ==========
  // ~4,900 lines of master-gated forcing lived here: minted the fake dispatcher
  // globals (DAT_8296EDF8/EDFC/EB80 + a 16-slot vtable pointing at the deleted
  // sub_8290E000), then hand-drove the presentation (splash-advance, FMUI ctor,
  // ActivateUI, movieGate). The natural path reaches the splash and menu chain
  // itself (2026-07-26 deep rolls: defExec=666 movInst=451 feFrames>3500), so
  // per the charter cycle (force -> diagnose -> root-fix -> confirm -> DELETE)
  // the diagnostic is retired. git history has the full machine if a future
  // investigation needs to read what it once compensated for.
}
