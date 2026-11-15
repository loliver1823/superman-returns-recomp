# OpenSAGE — Standing Reference for the Apt UI Subsystem

**What:** [OpenSAGE](https://github.com/OpenSAGE/OpenSAGE) is a free, open-source C# re-implementation of EA's **SAGE** engine (Command & Conquer: Generals, BFME, etc.). Its **Apt** subsystem is a complete, readable re-implementation of the **same EA "Apt" Flash-derived UI middleware** that Superman Returns uses for its entire frontend (`ui.AST` → 820 `Apt Data` movies). It is the single most useful external codebase for this project because it documents, in working code, the **consumer side** of the Apt format that our retail binary implements in obfuscated PPC.

**Why it matters to us:** the menu blocker (see [APT_INSTANTIATION_BLOCKER.md](_attic/APT_INSTANTIATION_BLOCKER.md)) is entirely inside the Apt load→parse→instantiate→execute chain. OpenSAGE is a reference implementation of every step of that chain. Use it to understand *what a retail function is trying to do*, *what a parsed structure should contain*, and *which steps are pure vs IO/threaded*.

> **CRITICAL CAVEAT — read before trusting any detail.** OpenSAGE is a *different-version* Apt re-implementation, not a spec. BFME/Generals Apt ≠ our `Apt Data:1:7:4` build; EA added custom bytecodes and the binary layouts differ. OpenSAGE is a **structural/semantic guide and output-validator**, NOT a bit-identical format reference. It will never substitute for reading the retail PPC; it tells you the *shape and intent* so the retail decode goes faster. Where this doc states a field/offset, that's OpenSAGE's model — verify against our binary before relying on it.

**What OpenSAGE does NOT cover:** the `.AST`/BGFA container format (that's EA Tiburon's BigFile — see [APT_MENU_EXTERNAL_RESEARCH.md](APT_MENU_EXTERNAL_RESEARCH.md) §2 / `tools/extract_ast.py`), and the **RTAM / `aptaux` runtime-asset-manager** layer (proprietary EA Tiburon, zero public docs — OpenSAGE loads `.apt` straight from disk; our game loads through RTAM/BigFileAsset). OpenSAGE covers the format *inside* the container and everything downstream of having the bytes.

---

## 1. Getting it / where things live

```
git clone https://github.com/OpenSAGE/OpenSAGE
```

Three directories hold the entire Apt subsystem:

| Path | Layer | Role |
|---|---|---|
| `src/OpenSage.Game/Data/Apt/` | **Data / parser** | Parses an `.apt` blob (+`.const`, +`.dat`) into an in-memory `AptFile`/`Movie` definition tree |
| `src/OpenSage.Game/Gui/Apt/` | **Runtime / player** | Turns a parsed `Movie` def into a running, rendering, updating instance |
| `src/OpenSage.Game/Gui/Apt/ActionScript/` | **AS VM** | AS2-style stack VM that runs the frame bytecode (this is what defines `_global.FlowMgr`) |

Docs: [opensage.readthedocs.io — APT format](https://opensage.readthedocs.io/file-formats/apt/apt.html) (format), [OpenSAGE/Docs repo](https://github.com/OpenSAGE/Docs). Origins: jonwil's `apt2xml`/`xml2apt`; parallel C++ impl [feliwir/libapt](https://github.com/feliwir/libapt).

---

## 2. Data / parser layer — `Data/Apt/`

Produces the **movie definition** the runtime instantiates. This is the output our retail `.apt` parser (def vtable `0x82087500`, currently unfindable) must produce.

| File | Defines |
|---|---|
| `AptFile.cs` | Root container/parser. Holds the `Movie`, the constant pool, image map, geometry. Entry point: `AptFile.Parse()`. |
| `Characters/` | The 9 character types (see below). `Movie` (type 9) is root; `Sprite` (type 5) is a nested timeline; `Shape`/`Text`/`Button`/`Image`/`Font` are leaves. |
| `Frame.cs` | One timeline frame: an ordered list of **FrameItems**. |
| `FrameItems/` | Per-frame commands: **PlaceObject** (instantiate/move a character on the display list), **RemoveObject**, **Action** (a block of AS bytecode to run on this frame), **FrameLabel**, **BackgroundColor**, etc. |
| `ConstantData.cs` | Parses the **`.const`** companion: the constant pool the bytecode indexes into, **plus the entry-character offset** (where parsing of the first character begins). The `.const` is mandatory to interpret an `.apt`. |
| `Import.cs` / `Export.cs` | Cross-movie symbol imports/exports (one movie referencing another's exported character by name). |
| `Geometry.cs` | Vector shape geometry (from the `.dat` companion). |
| `ImageMap.cs` | Maps image character IDs → texture resources. |

### Character types (from the format docs)
| Type | Name | Purpose |
|---|---|---|
| 1 | Shape | geometry/display data |
| 2 | Text | text field |
| 3 | Font | embedded glyph data |
| 4 | Button | hit region + AS event handlers |
| 5 | Sprite | **nested timeline container (a MovieClip def)** |
| 6 | Sound | (unused in Apt) |
| 7 | Image | references the `.dat` |
| 8 | Morph | (unused in Apt) |
| 9 | **Movie** | **root character; contains all others + the frame timeline** |

**The `Movie` def (what `AptFile.Parse` yields, conceptually):** `{ Frames[ {FrameItems: PlaceObject|Action|...} ], Characters[], screen width/height, millisecondsPerFrame, Imports[], Exports[] }`. This is the field-by-field checklist for validating our retail def-ctor's output.

---

## 3. Runtime / player layer — `Gui/Apt/`

Turns a `Movie` def into a live instance. **Everything here is synchronous, single-threaded, and free of any asset/IO/worker system** — a key fact for us (see §6).

| File | Role |
|---|---|
| `AptWindow.cs` | **The player.** Owns the `AptContext`, creates the root `SpriteItem` from `aptFile.Movie`, drives `Update`/`Render`. This is the boot blueprint (§5). |
| `AptWindowManager.cs` | Stack of windows (push/pop screens). |
| `AptContext.cs` | Shared runtime state: the AVM instance, the owning movie, msPerFrame, handler hooks. |
| `SpriteItem.cs` | **MovieClip instance.** Created from a `Sprite`/`Movie` def; advances frames, builds its display list from `PlaceObject` items, queues+runs frame `Action`s. |
| `DisplayList.cs` / `DisplayItem.cs` | The render tree; `DisplayItem` is the base for instances. |
| `ButtonItem.cs` | Button instance (hit-test + AS event dispatch). |
| `AptRenderingContext.cs` | Per-frame graphics state. |
| `AptInputMessageHandler.cs` | Routes input to interactive items. |
| `AptCallbackResolver.cs`, `AptCallbacksAttribute.cs` | Wire native C# callbacks to AS commands — **the equivalent of our retail `Function_822B62E0` ~70-command registration table** (`ChangeInterfaceState`, `PlayFMVs`, `ActivateUI`, …). |

---

## 4. ActionScript VM — `Gui/Apt/ActionScript/`

AS2-style stack VM. **This is the layer that runs the boot movie's frame-0 script and thereby defines `_global.FlowMgr`** — the thing whose absence is the proximate menu blocker.

| File | Role |
|---|---|
| `VM.cs` | The bytecode interpreter. |
| `InstructionStream.cs` / `InstructionCollection.cs` | Bytecode parsing/iteration. |
| `Opcodes/` | The instruction set (arithmetic, control flow, object/property ops, `CallMethod`, `GetMember`, `SetMember`, etc.). EA's custom opcodes live here in our build's dialect — expect deltas. |
| `Value.cs` | AS value + type coercions. |
| `ActionContext.cs` | Per-execution scope/state. |
| `ObjectContext.cs` | **Object scoping + property resolution — how `_global` and named objects like `FlowMgr` are stored and looked up.** Directly mirrors our retail resolver chain `sub_826B3998→…→sub_826B1040` (property-hash lookup on the active-context scope `[0x829A17BC]+24`). |
| `Property.cs` | Property storage/attributes. |
| `Function.cs` | Callable AS function objects. |
| `ExternObject.cs` | Bridges native objects into AS (e.g. the FlowMgr backing object). |
| `Library/` | Builtin globals / stdlib. |
| `UrlHandler.cs` | `getURL`/`loadMovie`-style URL ops. |

---

## 5. The canonical boot sequence (the blueprint)

`AptWindow.cs` constructor + `Update`, verbatim shape:

```csharp
// 1. create player + context, register native command hooks
_context = new AptContext(this);
_context.Avm.CommandHandler  = HandleCommand;   // ← "ChangeInterfaceState"/FlowMgr land here
_context.Avm.VariableHandler = HandleVariable;
_context.Avm.MovieHandler    = HandleMovie;

// 2. instantiate the ROOT clip FROM THE MOVIE DEFINITION
Root = new SpriteItem { ... };
Root.Create(aptFile.Movie, _context);           // def → live instance

// 3. per frame: advance display list, THEN run that frame's AS
void Update(...) {
    _context.Avm.UpdateIntervals(gt);
    Root.Update(gt);      // SpriteItem: advance frame, process PlaceObject items, queue Actions
    Root.RunActions(gt);  // execute queued frame bytecode  → frame-0 defines _global.FlowMgr
}
```

`SpriteItem.Create(def)` stores the def, sets frame 0, pre-indexes frame labels, `State=PLAYING`. `SpriteItem.Update` reads `_sprite.Frames[_currentFrame].FrameItems`, dispatching `PlaceObject`→place/move display items and `Action`→a queue that `RunActions` then feeds to `VM.Execute`.

---

## 6. Mapping: OpenSAGE ↔ our retail binary

| OpenSAGE concept | Our retail equivalent | DOGMA/threaded? | Our status |
|---|---|---|---|
| `AptFile.Parse()` → `Movie` def | the obfuscated `.apt` parser; def vtable `0x82087500` | **pure** | **never called** (the core gap) |
| `Root.Create(Movie def)` | `sub_826B0458` with **movieDef ≠ NULL** | **pure** | only ever called movieDef=NULL → empty clips |
| `SpriteItem.Update`/`RunActions` | level/clip vtable update (`sub_82692640`/`sub_8268EF50`) | **pure** | driven host-side (iter-197) but on an empty clip |
| `AptContext` | the Apt env block @ `0x829A19A8`+ (5 vectors); active-context `[0x829A17BC]` | — | exists, empty (counts 0) |
| `ObjectContext` resolution (`_global`, named lookup) | `sub_826B3998→sub_826B33C8→sub_826B1830→sub_826B1040` (hash via `sub_82680EB8`) | pure | resolver works; the names just aren't registered (no def) |
| `AVM.CommandHandler` table | `Function_822B62E0` ~70-command registration (`ChangeInterfaceState`, `PlayFMVs`, `ActivateUI`…) | — | registered; `[AS_INVOKE]` fires but FlowMgr undefined |
| loading the blob bytes | RtamPackage → `BigFileAsset::Load` (`sub_826E83D0`/`sub_82700F80`); generic loader `sub_822EDF10`/`sub_8238ED90` | **YES — the only contended layer** | the byte-fetch never fires for the movie |
| movie definition list/scope | currently-executing-def scope `[0x829A1630]`; def-chain via `[def+28]` | — | always null (no def executes) |

**The load-bearing takeaway:** in OpenSAGE the **only** thing touching IO/threads is getting the file bytes. Parse, instance, and frame-0 execution are all pure synchronous transforms. Our DOGMA/FMUI race lives **only** in the byte-fetch layer — so a def can in principle be built by: get bytes by any means (incl. the generic loader iter-205 dismissed) → call the parser → `sub_826B0458(movieDef=def)` → drive frame update, none of which needs FMGameRtam or the FMUI pool worker. See [APT_INSTANTIATION_BLOCKER.md](_attic/APT_INSTANTIATION_BLOCKER.md) §"iter-213 external research" for the recomposed attack.

---

## 7. How to use this reference (question → file to read)

| Question | Read in OpenSAGE |
|---|---|
| "What must our `.apt` parser output?" | `Data/Apt/AptFile.cs`, `Characters/*`, `Frame.cs`, `FrameItems/PlaceObject.cs` |
| "What does frame-0 do to define FlowMgr?" | `Gui/Apt/SpriteItem.cs` (`RunActions`), `ActionScript/VM.cs`, `ActionScript/ObjectContext.cs` |
| "How is a named object (`_global.FlowMgr`) stored/looked up?" | `ActionScript/ObjectContext.cs`, `Property.cs` |
| "How does a screen get pushed/popped?" | `Gui/Apt/AptWindowManager.cs` |
| "What's the per-frame update order?" | `Gui/Apt/AptWindow.cs` (`Update`), `SpriteItem.cs` |
| "What native commands does AS call?" | `Gui/Apt/AptCallbackResolver.cs` + grep `[AptCallback]`/`CommandHandler` |
| "How is the `.const` used?" | `Data/Apt/ConstantData.cs` |
| ".AST/BGFA container?" (NOT OpenSAGE) | our `tools/extract_ast.py`, `tools/pack_ast.py`; [APT_MENU_EXTERNAL_RESEARCH.md](APT_MENU_EXTERNAL_RESEARCH.md) |
| "RTAM/BigFileAsset/aptaux?" (NOT OpenSAGE) | retail PPC only; dev-PE symbols (`reference/dev_all_strings.txt`) |

---

## Sources
- [OpenSAGE engine (GitHub)](https://github.com/OpenSAGE/OpenSAGE) — `src/OpenSage.Game/Data/Apt/`, `src/OpenSage.Game/Gui/Apt/`, `.../Gui/Apt/ActionScript/`
- [OpenSAGE APT format docs](https://opensage.readthedocs.io/file-formats/apt/apt.html) · [OpenSAGE/Docs](https://github.com/OpenSAGE/Docs)
- [feliwir/libapt](https://github.com/feliwir/libapt) (C++ parallel impl + SWF v9 spec PDF)
- Related project docs: [APT_INSTANTIATION_BLOCKER.md](_attic/APT_INSTANTIATION_BLOCKER.md), [APT_MENU_EXTERNAL_RESEARCH.md](APT_MENU_EXTERNAL_RESEARCH.md)

_Created iter-213 (2026-06-13). Update file/method names against the live repo when reading source — OpenSAGE is actively developed._
