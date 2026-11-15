# Contributing

> Drafted with AI assistance (Claude) for the project owner.

**Development is paused** (see the [README](README.md)). Issues and pull requests may not be read or
answered. You are welcome to fork the project and build on it under its licence; the rules below are how
the project worked, and what a pull request here would have to meet.

**AI use.** Nearly all of the code and documentation in this repository was written by AI agents, working under the direction of the project owner, Lachlan Oliver ([@loliver1823](https://github.com/loliver1823)). Most of it was written by Anthropic's Claude models, run through Claude Code; OpenAI's Codex wrote a smaller part (mainly integration, native-rendering and release-build work in October 2026) and gave second opinions on results. The owner set the goals and priorities, made the product and direction decisions, decided what ships, and judged the work by building, running and playing the game. He did not review most of the code line by line. Correctness is checked by machines instead: the Xenia emulator as a reference oracle, differential tests and the other checks in [`docs/VERIFICATION.md`](/docs/VERIFICATION.md). [`docs/HOW_IT_WAS_BUILT.md`](/docs/HOW_IT_WAS_BUILT.md) describes the process, including the mistakes.

## Rules for every contribution

### 1. Disclose AI use

Say in the pull request whether AI tools wrote or changed any of it, which tools, and what you
checked yourself. Undisclosed AI-written contributions are closed.

### 2. Clean room

- No code, headers, documentation or binaries from a leaked or unlicensed source: no Xbox 360
  SDK material and no leaked game builds.
- No decompiled game code pasted in as source. The game's code reaches the build only through the
  recompiler, from the builder's own `default.xex`.
- No game data in the repository: no extracted files, shaders, textures, audio, GPU captures,
  memory dumps or screenshots. `.gitignore` covers the usual paths.
- Code from another project is allowed only under a permissive licence compatible with MIT (MIT, BSD,
  zlib, Apache-2.0, public domain); no GPL or LGPL code is copied in. Name the
  source in the file's header and add a `data/credits.json` entry in the same commit;
  `python scripts/check_credits.py` checks this.

### 3. No forcing

The game's own code makes every decision. A change may implement something the console provided (a
platform fix). It may not call game functions the game did not call, write state the game did not
write, or fake return values to make the game progress. Read
[`docs/NATURAL_PATH_CHARTER.md`](docs/NATURAL_PATH_CHARTER.md). Port features that act on a
player's explicit input (for example a key binding) call the same function with the same arguments
the game's own path would, and are registered in `docs/OVERRIDES.json`.

### 4. Hooks, not edits of generated code

Never edit `guest/ppc/` by hand. Override a function in host code and call the original
(`__imp__sub_XXXXXXXX`), as shown in [`docs/BUILDING.md`](docs/BUILDING.md#how-changes-are-made), then
register it: `python scripts/gen_override_manifest.py`, and `python scripts/audit_forcing.py` must
pass.

### 5. Verify against the reference

- Compare against Xenia, not against this port's earlier output. "Better than before" is not
  "correct".
- Renderer changes: replay the GPU traces first (`tools/replay_ab.ps1`), then run the flicker gate
  (`python tools/perf/flicker_gate.py`) for any change to rendering, draw timing, caching or
  threading.
- Suspected recompiler bugs: prove them with the differential test before claiming them
  ([`docs/DIFFTEST_DESIGN.md`](docs/DIFFTEST_DESIGN.md)).
- In the pull request, state what you ran, the result, and what is still broken or unchecked.

### 6. Upstream projects

Report bugs you find in XenonRecomp, XenosRecomp, Xenia or any other upstream project as issues
there, with a minimal reproduction written and checked by you. Do not send AI-written pull requests
to upstream projects. Their authors did not ask to review AI output, and some of them have said
publicly that they do not want it.

## Practical notes

- Commit messages: what changed, why, and the evidence (commands and results).
- New behaviour goes behind an `SR_*` environment switch until it is proven, then becomes the
  default.
- Keep the repository root clean: run outputs go to `archive/` (ignored).

## Licence of contributions

Contributions are licensed under the MIT licence, the licence of this project ([`LICENSE`](LICENSE)).
