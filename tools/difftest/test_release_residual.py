"""Owned VM proof of release legacy-path cuts after building game + difftest.

Runs the production direct-loader selector in both compile configurations,
including an old-behavior mutant. Inspects real objects and executable bytes;
this does not establish GPU output equivalence or full composition acceptance.
"""
from pathlib import Path
import argparse
import json
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from dev_loop import ROOT, source_identity, worker_root
from set_runner import atomic, digest, no_links, run_child


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build-dir', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--objects-only', action='store_true',
                    help='defer linked executable checks to the later combined GAME build')
    args = ap.parse_args()
    if not worker_root(ROOT):
        ap.error('compile/runtime proof belongs on the cloud worker')
    build, out = args.build_dir.resolve(), args.out.resolve()
    no_links(build)
    no_links(out)
    out.mkdir()
    (out / 'temp').mkdir()
    state = {'source': source_identity(), 'status': 'running', 'steps': {},
             'scope': 'compiled selector controls and linked-object boundary; no GPU acceptance'}
    spec = {'cwd': str(ROOT), 'quiet_window': str(ROOT / 'archive/QUIET_WINDOW'),
            'memory_limit_mb': 4096, 'process_limit': 16, 'env': {}}

    def run(name, argv, expected=0, env=None):
        step = out / name
        step.mkdir()
        result = run_child([str(a) for a in argv], step, dict(spec, env=env or {}), out, 120)
        state['steps'][name] = dict(result, argv=[str(a) for a in argv], expected=expected)
        atomic(out / 'receipt.json', state)
        if result['exit_code'] != expected or result['reason'] or not result['tree_empty']:
            raise AssertionError(name + ': ' + json.dumps(result))
        return ''.join((step / f).read_text(encoding='utf-8', errors='replace')
                       for f in ('stdout.txt', 'stderr.txt'))

    try:
        cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
        assert 'SR_RELEASE_COMPOSITION:BOOL=ON' in cache, 'release build required'
        production = ROOT / 'gfx/tex_load_direct.inc'
        text = production.read_text(encoding='utf-8')
        begin = text.index('bool texLoadDirectEnabled() {')
        end = text.index('\n}\n\n}  // namespace', begin) + 2
        function = text[begin:end]
        state['selector_source'] = digest(production)
        harness = ('#include <cstdlib>\n#include <cstdio>\n' + function + '\n'
                   'int main(int argc, char** argv) {\n'
                   '  if (argc != 2) return 2;\n'
                   '  const bool want = argv[1][0] == \'1\';\n'
                   '  const bool got = texLoadDirectEnabled();\n'
                   '  std::printf("selector got=%d expected=%d\\n", got, want);\n'
                   '  return got == want ? 0 : 1;\n}\n')
        compiler = 'C:/Program Files/LLVM/bin/clang++.exe'
        for oracle in (0, 1):
            src, exe = out / f'selector_{oracle}.cpp', out / f'selector_{oracle}.exe'
            src.write_text(harness, encoding='utf-8')
            run(f'compile_{oracle}', [compiler, '-std=c++20', '-O2',
                                     f'-DSR_ORACLE={oracle}', src, '-o', exe])
            for gate in ('0', '1'):
                run(f'selector_{oracle}_{gate}', [exe, gate if oracle else '1'],
                    env={'SR_TEX_LOAD_DIRECT': gate})
        # Restore the removed selector into the actual release arm. With =0 it
        # must fail the same always-ON assertion that the production code passed.
        mutant = harness.replace('return true;  // Shipping policy;',
                                 'const char* e = std::getenv("SR_TEX_LOAD_DIRECT"); '
                                 'return !e || e[0] != \'0\';  // Shipping policy;')
        assert mutant != harness, 'selector mutant anchor changed'
        src, exe = out / 'selector_mutant.cpp', out / 'selector_mutant.exe'
        src.write_text(mutant, encoding='utf-8')
        run('compile_mutant', [compiler, '-std=c++20', '-O2', '-DSR_ORACLE=0', src, '-o', exe])
        raw = run('selector_mutant', [exe, '1'], expected=1, env={'SR_TEX_LOAD_DIRECT': '0'})
        assert 'selector got=0 expected=1' in raw

        nm = 'C:/Program Files/LLVM/bin/llvm-nm.exe'
        removed = ('renderGuestGeometry(', 'ensureResolveTex(', 'snapshotResolveTexture(',
                   'copyRenderTargetToGuestFramebuffer(', 'recordPendingResolve(',
                   'completeInFlightResolves(', 'flushPendingResolves(')
        for target in ('SupermanReturns', 'difftest'):
            obj = build / f'CMakeFiles/{target}.dir/gfx/d3d12_backend.cpp.obj'
            raw = run(target + '_backend_symbols', [nm, '--demangle', obj])
            for name in removed:
                assert (name in raw) == (target == 'difftest'), (target, name)
            assert 'drainDrawQueue(' in raw and 'dumpBackBuffer(' in raw, target
        for unit in ('gfx/d3d12_frame_render.cpp', 'kernel/native_leaves.cpp'):
            obj = build / f'CMakeFiles/SupermanReturns.dir/{unit}.obj'
            raw = run(Path(unit).stem + '_symbols', [nm, '--demangle', obj])
            for name in ('sr::kernel::gww::', 'SkipState', 'applyFSkip(', 'IbConv'):
                assert name not in raw, (unit, name)
        markers = ('SR_NATIVE_CONST_SKIP', 'SR_CP_IB_CACHE', 'SR_GAMMA_OBS',
                   'SR_TEX_LOAD_DIRECT', 'SR_RESOLVE_TEXSNAP', 'SR_RESOLVE_SRCRECT',
                   'SR_RESOLVE_ASYNC', 'SR_RESOLVE_TILED', 'SR_RESOLVE_NOWRITE')
        state['linked_binary_proof'] = 'pending' if args.objects_only else 'checked'
        if not args.objects_only:
            release = (build / 'SupermanReturns.exe').read_bytes()
            oracle = (build / 'difftest.exe').read_bytes()
            for marker in markers:
                needle = marker.encode('ascii') + b'\0'
                assert needle not in release, ('release marker', marker)
                assert needle in oracle, ('Oracle marker missing', marker)
            state['binaries'] = {name: digest(build / name)
                                 for name in ('SupermanReturns.exe', 'difftest.exe')}
        state['source_after'] = source_identity()
        assert state['source'] == state['source_after'], 'source changed during proof'
        state['status'] = 'passed'
    except Exception as error:
        state['error'] = repr(error)
        state['status'] = 'failed'
    atomic(out / 'receipt.json', state)
    print(json.dumps(state, indent=2))
    return 0 if state['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
