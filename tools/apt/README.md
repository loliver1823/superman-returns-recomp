# tools/apt - EA Apt (ui.AST) container, movie and bytecode toolchain

These tools read, dump, assemble, patch and re-pack the game's Apt UI movies. They need
Python 3 and nothing else. Everything reads the user's own `Superman Returns (USA)/DATA/*.AST`,
and that folder is **read-only**: nothing here writes into the game folder. The code was
promoted from the prototype in `archive/v1_frontend/apt_format/` (which stays there
untouched) for `docs/CUSTOM_FRONTEND.md` step 0. Step 3 added the assembler, the patcher and
the mod build. Run every command from the repo root.

**Select movies by their ui.AST TOC name** (`bgfa.movie("SCRPRESSSTART")`,
`apt2txt.py SCRPRESSSTART`), never by the carve numbers in `data/ui_ast_extracted/`. Carve
*n* holds TOC record *n+2*, so every carve name is wrong. For example, `0038_ICONMENU` is
MAIN, `0040_MAIN` is PARTHIGHLIGHTBAR, and `0045_SAVELOADSLOTS` is SCRPRESSSTART
(`archive/v1_frontend/tooling/carve_evidence.py` checks 1640/1640).

## Files

| File | What it does |
|---|---|
| `bgfa.py` | Reads and writes BGFA1.05 containers: the outer ui.AST, the nested per-movie containers, and text.AST. Field widths and the offset shift come from the header. `rebuild()` is byte-exact. It also provides `fnv1a64()` (the outer TOC id is FNV-1a-64 of the UPPER name), `movies()`, `movie(name)`, `DEFAULT_UI` and a TOC lister. |
| `apt_model.py` | Walks `.apt` (`Apt Data:1:7:4`) and `.const` (`Apt constant file`) files. Its pointer map is the game's own load-time relocation set: AptParseReloc_826AE6D0, AptRegistrar_826ADD40, AptSpriteFramesRelocate_826A39A0 and AptActionBytecodeRelocate_8269F3A0. |
| `apt_asm.py` | ActionScript bytecode disassembler (`disasm`, `fmt`, `listing`) plus `logical_view()`, the offset-independent view that the round-trip tests compare. |
| `apt_graph.py` | The editable graph: `from_apt(Apt)` gives a `Graph` of blocks with symbolic pointers. `Graph.emit()` lays the blocks out again, re-assembles and re-links. `Graph.gc()` drops orphan blocks and unreferenced `.const` items and renumbers the index arrays; it is a no-op on an unedited movie. `Graph.renumber_walk_order()` (after `gc()`, before `emit()`; `patch_movie` runs it for every patched movie) stores the `.const` items in the runtime's unload-walk order, one item per index slot (see the invariants below). Every edit goes through this module. |
| `apt_unload.py` | The runtime's **unload contract**, checked on the bytes and independent of `apt_graph`: simulates the unload walk (AptMovieUnrelocate_sub_826A3180 -> sub_826A0758 -> sub_8269F3A0 with a3 == 0) and asserts index == counter for every Push/ConstantPool slot, slots == items, every slot walked, and that a reload after the simulated unload resolves the first load's constants. `patch_movie.verify()` and `build_custom_frontend` (every output movie) fail on a violation; the CLI checks whole containers. |
| `apt_write.py` | An independent second writer (`relayout`), used only by `rt_test.py`. |
| `apt2txt.py` | Human-readable dump of one movie: header, imports/exports, characters, every timeline and all AS bytecode. |
| `aptasm.py` | **Text assembler** plus canonical disassembler (step 3; syntax below). `render(lower(g, key))` gives the `.aptasm` text of a block and `parse()` reads it back. `materialize_new()` builds fresh graph code from the text, and `bind()` re-assembles a block in place. |
| `patch_movie.py` | **Patcher** (step 3): applies a `.patch` file to movies, then re-serialises and verifies them by re-parsing (patch language below). |
| `build_custom_frontend.py` | **Mod build** (steps 2-3). It reads ui.AST, applies `mods/<mod>/*.patch` (default `custom_frontend`), verifies the final bytes, and writes `<content root>/mods/<mod>/DATA/ui.AST` (form A, the default; gitignored `/mods/`). `--form b` writes per-movie `_workinprogress/<MOVIE>.ast` (REJECTED by E1, kept for follow-up). `--enable` adds the mod to `mods/enabled.txt`, the switch the exe reads at boot (docs/MODS.md). |
| `mods/custom_frontend/` | Tracked patch sources for the custom front end: the step-4 menu (`10_menu.patch` + three `.aptasm` bodies on SCRPRESSSTART; its README lists them). |
| `rt_test.py`, `rt_graph.py`, `rt_asm.py`, `rt_patch.py` | Regression tests (below). |

## Commands

```powershell
python tools/apt/rt_test.py                  # container + movie round trips        exit 0 = all pass
python tools/apt/rt_graph.py                 # editable-graph round trips            exit 0 = all pass
python tools/apt/rt_asm.py                   # assembler round trips + gc            exit 0 = all pass
python tools/apt/rt_patch.py                 # every patch op + the mod build        exit 0 = all pass
python tools/apt/apt_unload.py [UI.AST ...]  # unload contract per movie (default: retail + the built mod)  exit 0 = all pass
python tools/apt/apt2txt.py --list           # TOC names of the 820 movies
python tools/apt/apt2txt.py SCRPRESSSTART > archive/<somewhere>/dump_SCRPRESSSTART.txt
python tools/apt/aptasm.py dis SCRPRESSSTART [--block 0x41e4]   # canonical .aptasm of a movie
python tools/apt/aptasm.py check my.aptasm   # parse one block, print its canonical form
python tools/apt/patch_movie.py my.patch [--out archive/<somewhere>]   # apply + verify; --out writes .apt/.const + changed blocks (refuses the game folder)
python tools/apt/build_custom_frontend.py --check                     # build + verify in memory
python tools/apt/build_custom_frontend.py [--mod NAME] [--form a|b] [--enable] [--patches DIR] [--content-root DIR]   # write the mod
python tools/apt/bgfa.py                     # ui.AST TOC: index, type, offset, size, id, name
```

The tests also take an explicit container path (`rt_test.py [ui.AST] [text.AST]`, the others
`[ui.AST]`). Dumps and other generated output go under `archive/` or the session scratchpad,
never the repo root. `rt_patch.py` writes its two 89.6 MB build outputs to a system temp
directory and deletes them afterwards.

Expected output on the retail ui.AST (2026-09-30):

```
rt_test.py:
[PASS] ui.AST TOC ids == FNV-1a-64(UPPER name): 828/828
[PASS] ui.AST bgfa rebuild byte-identical (89595420 B): 1/1
[PASS] nested BGFA containers rebuild byte-identical: 821/821
[PASS] text.AST bgfa rebuild byte-identical (233532 B): 1/1
[PASS] movies parsed with 0 uncovered non-zero bytes: 820/820
[PASS] movies re-serialised byte-exact (relayout identity, .apt AND .const): 820/820
[PASS] movies shifted +0x40, re-parsed, logically equal: 820/820
rt_graph.py:
[PASS] graph identity byte-exact (.apt AND .const): 820/820
[PASS] graph shifted +0x40, re-parsed, logically equal: 820/820
[PASS] retail is in unload-walk order: renumber_walk_order moves nothing and emits byte-exact, apt_unload.check passes: 820/820
rt_asm.py:
[PASS] canonical .aptasm -> assemble -> movie byte-exact (.apt AND .const): 820/820 movies
[PASS] canonical text re-parses to the block's exact values: 598/598 blocks (the bytecode of 112 movies; 708 movies have none)
[PASS] apt2txt listing syntax -> assemble -> movie byte-exact: 820/820 movies
[PASS] apt2txt listing re-parses to the block's exact values: 598/598 blocks
[PASS] fresh re-assembly of every block + gc(), re-parsed, same text, 0 uncovered: 820/820 movies
[PASS] gc() on an unedited movie removes nothing, emit byte-exact: 820/820 movies
[PASS] single-token mutations of a real block change the bytes or are rejected: 8/8
[PASS] malformed sources rejected by the assembler: 8/8
rt_patch.py: 32 [PASS] lines, "ALL PASS: 32/32 checks" (section Q = the unload contract, with mutations)
apt_unload.py: retail 820/820, the built custom_frontend 820/820
```

"Logically equal" means `apt_asm.logical_view()` is identical: the same blocks in the same
order, every non-pointer byte equal, every pointer naming the same (block, delta) target, the
same disassembly and the same `.const` items. The step-0 tests were mutation-checked
(`archive/v1_frontend/tooling/mutation_check.py`, 6/6). The step-3 tests were too
(`archive/v1_frontend/tooling/step3/mutation_check3.py`): each of 12 injected defects makes
the relevant test exit non-zero. The defects include a wrong splice retarget, an ungrown
pool, an unbumped frame count, pool names never reused, dropped function flags, `bind`
skipping the `.const` write, no renumbering in `gc`, a wrong raw-offset base, 0xFF padding,
and spriteId 0.

## aptasm syntax (`aptasm.py` docstring has the full list)

The syntax is apt2txt's own instruction syntax, so a pasted apt2txt listing (address column,
numeric branch offsets, `DefineFunction2 ... w=[...]`) assembles byte-exact. It adds:

```
.const KEY = reg3                     ; assembler-time alias, used as $KEY
.pool 'SRFrontEnd', 'Install'         ; = ConstantPool [...]; names used later are appended
    Push [$KEY, reg1]                 ; Push / ConstantPool values: 'str', regN, ints, ...
    EA_GetNamedMember 'SRFrontEnd'    ; pool ops take a name (resolved in the pool in effect)
    EA_GetNamedMember pool[3]='x'     ; ... or apt2txt's index form (the name is checked)
    If skip                           ; branches name labels
    .func2 ''(r3 'keyCode') flags=0x60129   ; DefineFunction2 (flags required) ... .end
        Return
    .end
    GetURL 'FSCommand:SR_Quit', ''    ; = fscommand("SR_Quit", "")
skip:
```

A new block that names pool entries but has no ConstantPool gets one at index 0. `End` is
appended when missing (add-* bodies). The only aliases accepted beyond `apt_asm.NAMES` are
`EA_PushZero` (0x59) and `EA_PushOne` (0x5A), both IDA-verified, and `EA_PushConstByte`
without the `?`. Output keeps the `apt_asm.NAMES` spellings (e.g. `op_71`).

## Patch language (`patch_movie.py` docstring has the full list)

```
movie SCRGAMEPLAYOPT                          # select by TOC name
replace-const 'OLD' -> 'NEW' [count=N]
prepend-to function HandleInput in ScrGameplayOpt [nth=K]     # body inline up to 'end',
append-to  function InitGui in ScrPressStart                   #   or file=NAME.aptasm
prepend-to action initaction:ScrPressStart | doaction:0/0 | bytecode@0x41e4
append-to  action ...
add-initaction [sprite=auto|N] [char=C] [frame=F] [position=last|first]
add-doaction   [char=C] [frame=F] [position=last|first]
clone-place NAME as NEW char=C [frame=F] depth=D [x=X] [y=Y] [tabindex=N]   # iter-464
edit-place  NAME char=C [frame=F] [x=X] [y=Y] [tabindex=N]                  # iter-464
```

`clone-place` copies a PlaceObject (a component instance such as a BasicButton, with its
clip-action parameters) under a new name and depth, right after the original in the frame;
the copy owns copies of its name, clip-action records, their bytecode and every string they
point at, so no block is shared. `edit-place` moves a PlaceObject (matrix tx/ty) and/or
rewrites the value its clip action assigns to `m_tabIndex` (the focus order of the game's
focus manager). `custom_frontend/40_pause.patch` uses both for the pause menu's 10th row.

Prepend and append bodies are snippets: no `End` and no ConstantPool. Their pool names
resolve in the host block's pool, and missing names are appended to it. `append-to` also
catches branches inside the body that target the body end, and it refuses a body whose final
Return/Jump would leave the code unreachable. A name in `in` / `initaction:` first matches
AS2 class exports (`__Packages.X`) and only then plain linkage exports. Every apply is
verified before it returns. The checks are:

**The first-registrar rule (lane E, step 5 E3-E6 r3).** Every AS2 class's InitAction is guarded by
`if (!_global.X)`, so the first movie that registers X wins and every later copy never runs. MAIN
(_level0, loaded first on every boot) exports 40 classes as `__Packages.*`, among them ScrGenPop,
ButtonIcon, ButtonHints, DisplayWell and PartText (`docs/ENGINE_MAP.md`). `patch_container` (every
build) and `patch_movie.py` refuse a `function` op that lands in such a class in any movie but
MAIN, naming the class: patch it in `movie MAIN`. Only a function defined INSIDE the guarded body
counts (by position, E3-E6 r4; the guard is found on the pristine block and tracked through every
op, so code prepended to the InitAction does not hide the body, E3-E6 r5). An `action` op on an InitAction block is live (it runs whether
or not the guard skips the body) and is not refused, nor is a function defined outside the body. `rt_patch.py` covers both
and a mutation without the check. The E4 BACK hint and the E5 hint ids were first patched into
SCRGENPOP / BUTTONICON and never ran (the Ally roll of 2026-10-23); no other shipped patch was
caught.

- 0 uncovered non-zero bytes;
- the re-parsed movie is isomorphic to the edited graph;
- the edited movie round-trips byte-exact;
- every untouched original block and `.const` item is unchanged.

`append-to function NAME in CLASS after-call=METHOD` inserts immediately after
the unique own-level `PushConst METHOD / CallMethod` pair instead of at the body
end. Existing branches retain their original targets, including branches that
skip that call. Missing or ambiguous calls fail the build. The native costume
extension uses this to append records after the retail getter without replacing
`RecordAdapter.GetCostumeData`. `python tools/apt/test_costume_patch.py` checks the
emitted splice, retail selection preservation, teardown and negative controls.

## Writer invariants

These come from `archive/v1_frontend/apt_format/NOTES.txt` and the 2026-09-30 corpus census.

- Every block is 4-aligned.
- Every Push (0x96) and ConstantPool (0x88) has its **own** index array. The loader
  rewrites them in place (12,992/12,992 are unshared in retail), so never share one.
- Every `.const` item belongs to exactly **one** array element: 19,812/19,812, with 0 shared
  and 0 unreferenced. That is why new code gets fresh items, and why there is no dedupe.
  Re-assembling every block with fresh items reproduces the retail `.const` byte for byte
  (820/820).
- **Items are stored in the runtime's UNLOAD-walk order (a hard rule, iter-463 QUIT GAME
  soft-lock).** On unload the runtime rewrites every Push/ConstantPool slot as a running
  counter (`slot = counter++`, AptActionBytecodeRelocate_sub_8269F3A0 0x8269F6BC; the
  original index is not restored) while it walks chars in index order (types 5/9, imports
  skipped) -> frames -> items (type 1 Action, type 3 PlaceObject clip actions, type 8
  InitAction) -> a linear bytecode scan to the first End. A reload reuses those bytes. So
  slot k must hold k and #items == #slots, as in all 820 retail movies. Edits append new
  items at the end (`aptasm`, `patch_movie._grow_pool`, `add_const_string`) and `gc()` keeps
  that order; `renumber_walk_order()` fixes it before every patched emit, and
  `apt_unload.py` checks it on the output. Without it the 5 patched custom_frontend movies
  loaded once and reloaded with wrong constants (SCRPRESSSTART's registerClass pool read
  [reg2, reg2, reg2]: class never bound, no onLoad / InitGui).
- Each block has at most one ConstantPool, and it is always the first instruction. Pool ops
  take 1-byte indices, except `EA_PushConstWord` (0xA3), which retail uses only at indices of
  256 and above.
- An empty DefineFunction(2) params pointer stays non-null (926/926). `gc()` treats it as a
  weak reference and retargets it when its target goes away.
- Branch offsets (`next_pc + i32`) and DefineFunction(2) body sizes are recomputed at emit.
- `.const` strings are re-emitted in item order, 4-aligned.
- Nested TOC ids stay sorted ascending, because the lookup is a bsearch (sub_827066D8).
- TOC field widths are honoured, and `rebuild()` asserts on overflow. A TOC-first (nested)
  container cannot gain entries, because its TOC region is kept in place. A movie whose
  `.apt` outgrows a 2-byte size field would fail that assert (not handled).

## Not done yet

- Edited movies run in the game: E1 (`docs/CUSTOM_FRONTEND.md` §7.2, a `replace-const`)
  and the step-4 menu (§5.3: `append-to` / `prepend-to` code with 12 new functions,
  GetURL / GetURL2, setInterval). `add-initaction` has still not run in the game (the menu
  appends to an existing InitAction instead).
- Form B (per-movie `DATA/_workinprogress/*.ast`) was rejected by E1 for `<TOC NAME>.ast`;
  other loose names are untested.
- A whole-binary search for other readers of the InitAction spriteId (`+4`) has not been
  done. The four frame-item walkers checked read it only as a sign flag.
- There is no op that replaces or deletes existing code, and no `replace-string` for `.apt`
  strings. `gc()` is ready for them.
- `Try` (0x8F) assembles only from raw bytes. It has 0 retail uses.
