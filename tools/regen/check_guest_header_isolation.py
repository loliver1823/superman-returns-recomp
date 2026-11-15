#!/usr/bin/env python3
"""Compile a bounded header-dependency and PPC-macro equivalence regression.

Run through tools/build_slot.py. This compiles four tiny LLVM-IR probes, never
the generated game. --baseline-dir may hold the pre-change kernel/ and
tools/regen/hand/ headers; otherwise the control restores the broad include.
The optional deployed-tree check proves that every generated unit uses the
same include prefix checked by the compiler. A full game build remains a
separate integration check.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = r'''#include "ppc_context.h"
#include <type_traits>
static_assert(std::is_same_v<decltype(sr::kernel::g_guestBaseTls), uint8_t*>);
static_assert(std::is_same_v<decltype(sr::kernel::g_storeWatchOn), bool>);
static_assert(std::is_same_v<decltype(sr::kernel::g_traceCallsOn), bool>);
static_assert(std::is_same_v<decltype(sr::kernel::g_guestTraceOn), bool>);
static_assert(std::is_same_v<decltype(sr::kernel::g_physAliasAIdentity), bool>);
static_assert(std::is_same_v<decltype(sr::kernel::g_renderObjVA), std::atomic<uint32_t>>);
static_assert(std::is_same_v<decltype(sr::kernel::g_fmuiPoolQueueVA), std::atomic<uint32_t>>);
static_assert(std::is_same_v<decltype(sr::kernel::g_aptEnvVA), std::atomic<uint32_t>>);
static_assert(std::is_same_v<decltype(sr::kernel::g_worldPtrFieldVA), std::atomic<uint32_t>[4]>);
static_assert(std::is_same_v<decltype(sr::kernel::g_worldPtrWatchArmed), std::atomic<bool>>);
using Watch = void(uint32_t, uint32_t, const char*, int);
static_assert(std::is_same_v<decltype(sr::kernel::dogmaGuardWatchpoint), Watch>);
static_assert(std::is_same_v<decltype(sr::kernel::moduleGlobalWatchpoint), Watch>);
static_assert(std::is_same_v<decltype(sr::kernel::vtableWatchpoint), Watch>);
static_assert(std::is_same_v<decltype(sr::kernel::renderObjDestructWatchpoint), Watch>);
static_assert(std::is_same_v<decltype(sr::kernel::fmuiQueueWatchpoint), Watch>);
static_assert(std::is_same_v<decltype(sr::kernel::worldPtrFieldWatchpoint), Watch>);
static_assert(std::is_same_v<decltype(sr::kernel::traceIndirectCall), void(uint32_t, uint32_t)>);
static_assert(std::is_same_v<decltype(sr::kernel::emitGuestTraceEntry), void(const char*, uint32_t, uint32_t, uint32_t)>);
extern "C" PPC_FUNC(sr_header_probe) {
    PPC_FUNC_PROLOGUE();
    const uint32_t address = ctx.r3.u32;
    ctx.r4.u64 = PPC_LOAD_U8(address) + PPC_LOAD_U16(address + 2)
              + PPC_LOAD_U32(address + 4) + PPC_LOAD_U64(address + 8);
    PPC_STORE_U8(address, ctx.r4.u8);
    PPC_STORE_U16(address + 2, ctx.r4.u16);
    PPC_STORE_U32(address + 4, ctx.r4.u32);
    PPC_STORE_U64(address + 8, ctx.r4.u64);
    PPC_CALL_INDIRECT_FUNC(ctx.r5.u32);
    ctx.r3.u64 = PPC_GUEST_OFFSET(ctx.r6.u32);
}
'''


def includes(text):
    return re.findall(r'^\s*#\s*include\s+([^\r\n]+)', text, re.MULTILINE)


def generated_prefix(directory):
    units = sorted(directory.glob('ppc_recomp.[0-9]*.cpp'))
    if not units:
        raise ValueError(f'No generated guest units in {directory}')
    # A new include must be examined rather than inheriting this proof silently.
    for unit in units:
        if includes(unit.read_text(encoding='utf-8')) != ['"ppc_recomp_shared.h"']:
            raise ValueError(f'Generated include prefix changed: {unit}')
    if includes((directory / 'ppc_recomp_shared.h').read_text(encoding='utf-8')) != ['"ppc_config.h"', '"ppc_context.h"']:
        raise ValueError('Generated shared-header include prefix changed')
    return len(units)


def run(command, log, *, succeeds=True):
    result = subprocess.run(command, capture_output=True, text=True, timeout=90)
    log.write_text(result.stdout + result.stderr, encoding='utf-8')
    if (result.returncode == 0) != succeeds:
        raise ValueError(f'Unexpected compiler result {result.returncode}; see {log}')


def normalized_ir(path):
    # Only the module identifier names the output path. Instructions, attributes,
    # types, constants, TLS access and calling conventions must match verbatim.
    return '\n'.join(line for line in path.read_text(encoding='utf-8').splitlines()
                     if not line.startswith('; ModuleID ='))


def check(out, compiler, simde, baseline=None, generated=None):
    out.mkdir(parents=True, exist_ok=False)
    source_paths = [ROOT / 'kernel/guest_runtime.hpp', ROOT / 'kernel/guest_context.hpp',
                    ROOT / 'tools/regen/hand/ppc_context.h', ROOT / 'tools/regen/hand/ppc_config.h']
    source_hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in source_paths}
    candidate, control = out / 'candidate', out / 'control'
    for directory in (candidate, control):
        (directory / 'kernel').mkdir(parents=True)
        for name in ('ppc_context.h', 'ppc_config.h'):
            source = ((baseline / 'tools/regen/hand' / name) if baseline and directory == control
                      else ROOT / 'tools/regen/hand' / name)
            shutil.copyfile(source, directory / name)
    if not baseline:
        path = control / 'ppc_context.h'
        text = path.read_text(encoding='utf-8')
        expected = '#include "kernel/guest_runtime.hpp"'
        if text.count(expected) != 1:
            raise ValueError('Expected exactly one narrow runtime include')
        path.write_text(text.replace(expected, '#include "kernel/guest_context.hpp"'), encoding='utf-8')
    shutil.copyfile((baseline or ROOT) / 'kernel/guest_context.hpp', control / 'kernel/guest_context.hpp')
    shutil.copyfile(ROOT / 'kernel/guest_runtime.hpp', control / 'kernel/guest_runtime.hpp')
    control_hashes = {str(path.relative_to(control)): hashlib.sha256(path.read_bytes()).hexdigest()
                      for path in control.rglob('*.h*')}
    # A host-only header mutation must be invisible to generated macros. The
    # positive control below proves this poison is observed with the old include.
    (candidate / 'kernel/guest_context.hpp').write_text('#error host_header_reached\n', encoding='utf-8')
    source = out / 'probe.cpp'
    source.write_text(FIXTURE, encoding='utf-8')
    rows = []
    for watch in (0, 1):
        irs = []
        for label, directory in (('control', control), ('candidate', candidate)):
            dest = out / f'{label}_watch{watch}.ll'
            deps = dest.with_suffix('.d')
            command = [str(compiler), '-std=c++20', '-O2', '-mssse3', '-fno-ms-volatile', '-w',
                       f'-DSR_STORE_WATCHPOINTS={watch}', '-S', '-emit-llvm', '-MMD', '-MF', str(deps),
                       '-I', str(directory), '-I', str(ROOT), '-I', str(simde), str(source), '-o', str(dest)]
            run(command, dest.with_suffix('.log'))
            dependencies = deps.read_text(encoding='utf-8').replace('\\', '/').lower()
            broad = 'guest_context.hpp' in dependencies
            if broad != (label == 'control'):
                raise ValueError(f'Unexpected broad-host-header dependency for {label}')
            if label == 'candidate' and 'guest_runtime.hpp' not in dependencies:
                raise ValueError('Candidate did not depend on the stable runtime header')
            irs.append(normalized_ir(dest))
        if irs[0] != irs[1]:
            raise ValueError(f'PPC macro LLVM IR changed with watchpoints={watch}; inspect {out}')
        rows.append({'store_watchpoints': watch, 'identical_llvm_ir': True,
                     'llvm_ir_sha256': hashlib.sha256(irs[1].encode()).hexdigest(),
                     'candidate_depends_on_host_header': False})
    # Show the test would fail if the removed transitive include were restored.
    shutil.copyfile(candidate / 'kernel/guest_context.hpp', control / 'kernel/guest_context.hpp')
    run([str(compiler), '-std=c++20', '-w', '-fsyntax-only', '-I', str(control), '-I', str(ROOT),
         '-I', str(simde), str(source)], out / 'negative_control.log', succeeds=False)
    if 'host_header_reached' not in (out / 'negative_control.log').read_text(encoding='utf-8'):
        raise ValueError('Negative control failed for an unrelated reason')
    if source_hashes != {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in source_paths}:
        raise ValueError('Headers changed during the compiler checks')
    result = {'schema': 'sr.guest-header-isolation/1', 'compiler': str(compiler),
              'baseline_headers': str(baseline) if baseline else 'restored broad include',
              'source_sha256': source_hashes, 'control_source_sha256_before_poison': control_hashes,
              'generated_units_with_verified_common_prefix': generated_prefix(generated) if generated else None,
              'deployed_headers_match_authoritative': {
                  name: (generated / name).read_bytes() == (ROOT / 'tools/regen/hand' / name).read_bytes()
                  for name in ('ppc_context.h', 'ppc_config.h')} if generated else None,
              'checks': rows, 'host_header_poison_detected_by_control': True,
              'scope': 'Authoritative hand-header probes and deployed include-prefix census; '
                       'deployed files are unchanged, and no full generated-code build is performed'}
    (out / 'receipt.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, default=Path(shutil.which('clang++') or 'C:/Program Files/LLVM/bin/clang++.exe'))
    parser.add_argument('--simde-dir', type=Path, default=ROOT / 'tools/XenonRecomp/thirdparty/simde')
    parser.add_argument('--baseline-dir', type=Path)
    parser.add_argument('--generated-dir', type=Path, help='optional deployed guest tree to census without modifying it')
    args = parser.parse_args(argv)
    try:
        result = check(args.out.resolve(), args.compiler.resolve(), args.simde_dir.resolve(),
                       args.baseline_dir.resolve() if args.baseline_dir else None,
                       args.generated_dir.resolve() if args.generated_dir else None)
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f'guest-header-check: {error}', file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
