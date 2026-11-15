# Documentation

> Drafted with AI assistance (Claude) for the project owner.

The pages below are the project's working documents. Most were written during development by the
AI agents that did the work, for themselves and each other: they are dense, dated and refer to
internal run names and to development documents that are not published. Where a page and the code
disagree, the code is right. Start with the first group.

## Start here

- [`../README.md`](../README.md): what this is, where it stopped, how to build it, features, known issues.
- [`HOW_IT_WAS_BUILT.md`](HOW_IT_WAS_BUILT.md): the process, who did what, the mistakes.
- [`VERIFICATION.md`](VERIFICATION.md): every check, how to run it, its latest result.
- [`BUILDING.md`](BUILDING.md), [`DUMPING.md`](DUMPING.md), [`RELEASING.md`](RELEASING.md).
- [`../CONTRIBUTING.md`](../CONTRIBUTING.md), [`../SECURITY.md`](../SECURITY.md).

## Rules and records

- [`NATURAL_PATH_CHARTER.md`](NATURAL_PATH_CHARTER.md): the no-forcing rule.
- [`NATURAL_BASELINE.md`](NATURAL_BASELINE.md): how far the unmodified game gets, with repro commands.
- [`PLAYTHROUGH.md`](PLAYTHROUGH.md): the automated whole-game runs.
- [`OVERRIDES.json`](OVERRIDES.json): every host override of a game function, classified.
- [`release_composition.json`](release_composition.json): what the release build may contain, file by
  file (the composition audit's classification).
- [`BANDAIDS.md`](BANDAIDS.md), [`BANDAID_BURNDOWN.md`](BANDAID_BURNDOWN.md): workarounds still in
  place and their retirement plan. [`forcing_audit_baseline.txt`](forcing_audit_baseline.txt).

## Engine and platform

- [`ENGINE_MAP.md`](ENGINE_MAP.md): verified facts about the game's engine (objects, functions,
  globals).
- [`MEMORY_MODEL_PLAN.md`](MEMORY_MODEL_PLAN.md), [`HOST_THREADS.json`](HOST_THREADS.json).
- [`APT_MENU_EXTERNAL_RESEARCH.md`](APT_MENU_EXTERNAL_RESEARCH.md),
  [`OPENSAGE_REFERENCE.md`](OPENSAGE_REFERENCE.md): EA's Apt UI format.

## Renderer and the native work

- [`RENDERER_PM4_HYBRID.md`](RENDERER_PM4_HYBRID.md): the GPU command-stream renderer (the test oracle
  the native path was checked against).
- [`NATIVE_ROADMAP.md`](NATIVE_ROADMAP.md), [`NATIVE_XDK_SPEC.md`](NATIVE_XDK_SPEC.md): removing the
  emulation layer.
- [`MIGRATION_API_HLE.md`](MIGRATION_API_HLE.md): the API-boundary observe layer.
- [`XENIA_EDRAM_CONTRACT.md`](XENIA_EDRAM_CONTRACT.md),
  [`CUBE_CONFORMANCE_READING.md`](CUBE_CONFORMANCE_READING.md),
  [`DESIGN_TEXCONTENT_V2.md`](DESIGN_TEXCONTENT_V2.md),
  [`d3d_renderstate_table.csv`](d3d_renderstate_table.csv).

## Port features

- [`CUSTOM_FRONTEND.md`](CUSTOM_FRONTEND.md), [`MAP.md`](MAP.md), [`SAVES.md`](SAVES.md).
- [`PC_CONTROLS.md`](PC_CONTROLS.md), [`MOUSE_CURSOR.md`](MOUSE_CURSOR.md),
  [`INPUT_GLYPHS.md`](INPUT_GLYPHS.md).
- [`HIGH_FRAMERATE.md`](HIGH_FRAMERATE.md), [`RESOLUTION.md`](RESOLUTION.md),
  [`GRAPHICS_SETTINGS.md`](GRAPHICS_SETTINGS.md).
- [`MODS.md`](MODS.md), [`DEVTOOLS.md`](DEVTOOLS.md) ([`DEVTOOLS.json`](DEVTOOLS.json)).

## Performance

- [`PERFORMANCE.md`](PERFORMANCE.md), [`PERFORMANCE_CODEGEN.md`](PERFORMANCE_CODEGEN.md),
  [`RECOMP_PERF_REFERENCE.md`](RECOMP_PERF_REFERENCE.md).
- [`CANARY_COMPARISON.md`](CANARY_COMPARISON.md): the measured comparison with Xenia Canary.

## Tools and method

- [`TOOLKIT.md`](TOOLKIT.md): the debugging toolkit and pipelines.
- [`TOOLS_INDEX.md`](TOOLS_INDEX.md), [`TOOLS.md`](TOOLS.md): where each tool lives.
- [`DIFFTEST_DESIGN.md`](DIFFTEST_DESIGN.md), [`L9_DIFFERENTIAL_ORACLE.md`](L9_DIFFERENTIAL_ORACLE.md),
  [`REXGLUE_BUILD.md`](REXGLUE_BUILD.md): differential testing of the recompiled code.
- [`OTHER_RECOMPS_LESSONS.md`](OTHER_RECOMPS_LESSONS.md): lessons from other Xbox 360 recompilations.
- [`THIRD_PARTY.md`](THIRD_PARTY.md): how the third-party licences are met in the build.
