"""Pinned, phase-separated execution of the existing ZPD bake. No ledger writes.

prepare seals current tools, binaries, seed and tapes into a fresh archive directory.
run is a dry run unless --execute is supplied. The caller owns machine scheduling.
"""
from pathlib import Path
import argparse
import copy
import hashlib
import importlib.util
import json
import os
import shutil
import sys

ROOT = Path(__file__).resolve().parents[2]
ARCHIVE = ROOT / 'archive/milestone2_20261016'
PACE_TAGS = ('pace0', 'pace0_trace', 'pace1', 'pace1_trace', 'pace1_zpd',
             'pace0b', 'pace0b_trace', 'pacing')
SEED = ROOT / 'archive/saves/12_ch12_after_bizarro1'
OWNER_SCOPE = ('Owner 2026-10-16 accepts natural city destruction in this rendering/query workload. '
               'Retain raw MENU-HEAVY classifications and original verdicts. A separate reviewed assessment '
               'may identify the natural destruction outcome; ordinary pause menus remain excluded.')


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def pacing_runner(base):
    """Stop on completed prerequisite failures; pending FAIL rows remain durable."""
    class BoundedRunner(base):
        def record(self, step, verdict, detail, paths=None, pending=False, refused=False):
            result = super().record(step, verdict, detail, paths, pending, refused)
            if not pending and step.kind in ('p2trace', 'zpdwin') and verdict not in ('PASS', 'DRY', 'SKIP'):
                self.abort = 'pacing prerequisite failed: ' + step.tag
            return result

        def run(self):
            result = super().run()
            rows = {row['step']: row for row in self.results}
            missing = [tag for tag in PACE_TAGS if rows.get(tag, {}).get('verdict') not in ('PASS', 'DONE', 'DRY')
                       or rows.get(tag, {}).get('pending')]
            if self.abort or missing:
                self.log('pacing incomplete: ' + (self.abort or 'required steps missing: ' + ', '.join(missing)))
                return result or 1
            return result
    return BoundedRunner


def inventory(root):
    """Exact path/content inventory; reject linked inputs instead of traversing them."""
    root = Path(root)
    if not root.is_dir():
        raise ValueError('missing directory: ' + str(root))
    result = {}
    for current, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            path = Path(current) / name
            if path.is_symlink() or getattr(path.lstat(), 'st_file_attributes', 0) & 0x400:
                raise ValueError('linked input: ' + str(path))
        for name in files:
            path = Path(current) / name
            result[path.relative_to(root).as_posix()] = digest(path)
    if not result:
        raise ValueError('empty input directory: ' + str(root))
    return dict(sorted(result.items()))


def tool_paths(root):
    # Pin the complete perf module set, and the shared launcher/replay dependencies.
    files = list((root / 'tools/perf').glob('*.py'))
    files += list((root / 'tools').glob('*.ps1'))
    files += list((root / 'tools').glob('*.py'))
    files += list((root / 'tools/controls').glob('*.py'))
    files += list((root / 'tools/scenarios').glob('*.json'))
    return sorted(set(files))


def tool_issues(root):
    required = {
        'tools/perf/native_proof.py': ('"zpd_bake":', 'zpd_window_item'),
        'tools/perf/zpd_activity.py': ('every_quarter_active',),
        'tools/perf/run_health.py': ('THREAD_FATAL',),
        'tools/perf/perf_run.py': ('from run_health import', 'portFeaturesFor'),
        'tools/replay_batch.py': ('-ReceiptPath', 'run_health.scan_log'),
        'tools/run_recomp_x.ps1': ('SR_PYTHON', 'Get-SrRunLogHealth'),
    }
    issues = []
    for name, markers in required.items():
        path = root / name
        body = path.read_text(encoding='utf-8') if path.is_file() else ''
        for marker in markers:
            if marker not in body:
                issues.append(name + ': missing prerequisite ' + marker)
    return issues


def load_proof(root):
    # One complete checkout supplies the plan, graders, launchers and replay code.
    sys.path.insert(0, str(root / 'tools'))
    sys.path.insert(0, str(root / 'tools/perf'))
    spec = importlib.util.spec_from_file_location('zpd_full_proof', root / 'tools/perf/native_proof.py')
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def tape(cycles):
    lines = [
        '# Fixed view, ordinary legacy Up power-selection input; no flight or mouse.',
        '# Measured inputs remain inputs, not a guarantee of query activity or idle state.',
        r'await \[SR_FSCMD\].*answer cppId=\S+ button=1 1500',
        r"await \[APT_(REQ|SCREEN)\].*path='screens/hud/main_hud' 900",
        r'await \[PC_CONTROLS\] context gameplay 15',
        'wait 7000', 'releaseall', 'keymap legacy', 'sample stationary-start', 'mark dump']
    for i in range(cycles):
        lines += [f'press Up 100 active-{i + 1:03d}', 'wait 2400']
    return '\n'.join(lines + ['releaseall', 'sample stationary-complete']) + '\n'


def make_plan(proof, manifest, phase):
    plan = copy.deepcopy(proof.plans()['zpd_bake'])
    original = copy.deepcopy(plan)
    plan['prefix'] = 'zfb'
    plan['about'] = 'Pinned full-bake ' + phase + ' phase; original obligations retained; ' + OWNER_SCOPE
    # A phase cannot close the full bake, and this helper never applies the ledger.
    plan['resolves'] = []
    plan['gates'] = proof.genv(plan['gates'], 'SR_NB_AOT_DIR=' + manifest['nb_dir'])
    for step in plan['steps']:
        if step.kind not in ('roll', 'gate'):
            continue
        step.gates = proof.genv(step.gates, 'SR_NB_AOT_DIR=' + manifest['nb_dir'])
        step.from_save = manifest['seed']
        if step.tag == 'gate':
            step.play = manifest['tapes']['gate']
        elif step.tag in ('abp', 'cppm4'):
            step.play, step.dwell, step.seconds = manifest['tapes']['freeze'], 600, 1100
        elif step.tag.startswith('pace'):
            step.play, step.dwell, step.seconds = manifest['tapes']['pacing'], 1200, 2100
        else:
            step.play = manifest['tapes']['short']
    # The live arms keep the original pace0 -> pace1 -> pace0b order. Read the
    # default's window immediately, before spending another calibration roll.
    by_tag = {s.tag: s for s in plan['steps']}
    plan['steps'] = ([by_tag[tag] for tag in PACE_TAGS] if phase == 'pacing' else
                     [s for s in plan['steps'] if s.tag not in PACE_TAGS])
    old_by_tag = {s.tag: s for s in original['steps']}
    for step in plan['steps']:
        before = old_by_tag[step.tag]
        # No acceptance predicate, comparison target, per-step gate, or window changes.
        expected_gates = (proof.genv(before.gates, 'SR_NB_AOT_DIR=' + manifest['nb_dir'])
                          if step.kind in ('roll', 'gate') else before.gates)
        if step.gates != expected_gates:
            raise ValueError('unexpected gate change: ' + step.tag)
        for field in ('ledger', 'mode', 'base', 'target', 'calib', 'window', 'require'):
            if getattr(step, field) != getattr(before, field):
                raise ValueError('unexpected obligation change: ' + step.tag + '.' + field)
    return plan


def prepare(a):
    tools = Path(a.tools).resolve()
    exe, base = Path(a.exe).resolve(), Path(a.base).resolve()
    issues = tool_issues(tools)
    if issues:
        raise ValueError('complete current tool checkout required:\n' + '\n'.join(issues))
    if digest(exe) != a.exe_sha256.lower() or digest(base) != a.base_sha256.lower():
        raise ValueError('executable hash mismatch')
    if exe == base or a.exe_sha256.lower() == a.base_sha256.lower():
        raise ValueError('pre-bake reference must be a distinct pinned executable')
    out = Path(a.out).resolve()
    if out.parent != ARCHIVE or not out.name.startswith('zpd_full_bake_') or out.exists():
        raise ValueError('use a fresh zpd_full_bake_* directory directly under ' + str(ARCHIVE))
    nb = exe.parent / 'data/shader_cache/nb'
    base_nb = base.parent / 'data/shader_cache/nb'
    # Read before creating the preparation folder: bad inputs leave no runnable plan.
    seed_files, nb_files, base_nb_files = inventory(SEED), inventory(nb), inventory(base_nb)
    package_files, base_package_files = inventory(exe.parent), inventory(base.parent)
    pins = {str(p): digest(p) for p in tool_paths(tools)}
    pins[str(Path(__file__).resolve())] = digest(__file__)
    out.mkdir()
    tapes = {}
    for name, cycles in (('gate', 52), ('short', 98), ('freeze', 235), ('pacing', 475)):
        path = out / ('zpd_full_bake_' + name + '.play')
        path.write_text(tape(cycles), encoding='utf-8')
        tapes[name] = str(path)
        pins[str(path)] = digest(path)
    # Retain the literal accepted 600-second input schedule (only comments differ).
    accepted = ARCHIVE / 'zpd_stationary_freeze_600_play.txt'
    commands = lambda p: [s.strip() for s in Path(p).read_text().splitlines()
                          if s.strip() and not s.lstrip().startswith('#')]
    if commands(accepted) != commands(tapes['freeze']):
        raise ValueError('freeze tape no longer matches accepted 600-second pilot')
    manifest = dict(schema='zpd-full-bake/1', tool_root=str(tools), exe=str(exe), exe_sha256=a.exe_sha256.lower(),
                    base=str(base), base_sha256=a.base_sha256.lower(), nb_dir=str(nb), nb_files=nb_files,
                    base_nb_dir=str(base_nb), base_nb_files=base_nb_files,
                    package_files=package_files, base_package_files=base_package_files,
                    seed=str(SEED), seed_files=seed_files, stage=str(Path(a.stage).resolve()), tapes=tapes,
                    pins=pins, owner_scope=OWNER_SCOPE, baseline_provenance=a.baseline_provenance,
                    full_acceptance='PENDING: both phases, raw/scoped review, all item predicates, offline and Xenia proof')
    proof = load_proof(tools)
    import pm4_replay_score as score
    score.REPO = str(ROOT)
    fixtures = {str(Path(score._fixture(({**score.TRACES, **score.EXTRA})[tag][0]))):
                digest(score._fixture(({**score.TRACES, **score.EXTRA})[tag][0])) for tag in proof.TRACES.split(',')}
    manifest['fixtures'] = fixtures
    manifest['fixture_root'] = str(ROOT)
    manifest['plans'] = {phase: [vars(s) for s in make_plan(proof, manifest, phase)['steps']]
                         for phase in ('correctness', 'pacing')}
    path = out / 'zpd_full_bake_manifest.json'
    path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(path)
    return 0


def run(a):
    manifest_path = Path(a.manifest).resolve()
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    if manifest.get('schema') != 'zpd-full-bake/1':
        raise ValueError('unsupported manifest')
    tools = Path(manifest['tool_root'])
    for path, expected in manifest['pins'].items():
        if digest(path) != expected:
            raise ValueError('pinned input changed: ' + path)
    for path, expected in manifest['fixtures'].items():
        if digest(path) != expected:
            raise ValueError('replay input changed: ' + path)
    for name in ('exe', 'base'):
        if digest(manifest[name]) != manifest[name + '_sha256']:
            raise ValueError('binary changed: ' + name)
    if (inventory(manifest['seed']) != manifest['seed_files'] or
            inventory(manifest['nb_dir']) != manifest['nb_files'] or
            inventory(manifest['base_nb_dir']) != manifest['base_nb_files']):
        raise ValueError('seed or matching native shader package changed')
    if (inventory(Path(manifest['exe']).parent) != manifest['package_files'] or
            inventory(Path(manifest['base']).parent) != manifest['base_package_files']):
        raise ValueError('packaged executable dependencies changed')
    if tool_issues(tools):
        raise ValueError('tool prerequisites missing')
    if a.execute and a.phase == 'pacing' and not a.timing_isolated:
        raise ValueError('pacing requires --timing-isolated after owner schedules an idle machine')
    proof = load_proof(tools)
    plan = make_plan(proof, manifest, a.phase)
    if json.loads(json.dumps([vars(s) for s in plan['steps']])) != manifest['plans'][a.phase]:
        raise ValueError('resolved plan differs from sealed manifest')
    proof.plans = lambda: {'zpd_full_bake_' + a.phase: plan}
    if a.phase == 'pacing':
        proof.Runner = pacing_runner(proof.Runner)
    # Replay bypasses stage_roll. Bind each replay arm to its OWN packaged kernels.
    import replay_batch
    replay_batch.score.REPO = manifest['fixture_root']
    original_replay_job = replay_batch.run_job

    def pinned_replay_job(job, out):
        job = dict(job)
        identity = str(Path(job['exe']).resolve())
        if identity == manifest['base']:
            nb = manifest['base_nb_dir']
            arm = 'base'
        elif identity == manifest['exe']:
            nb = manifest['nb_dir']
            arm = 'oracle'
        else:
            raise ValueError('unexpected replay executable')
        job['gates'] = proof.genv(job.get('gates', ''), 'SR_NB_AOT_DIR=' + nb)
        # Shader cache may be extended by the runtime. Keep the pinned package
        # immutable and give each replay arm its own fresh copy of shipped DXIL.
        cache = Path(out) / ('cache_' + arm)
        shipped = Path(job['exe']).parent / 'data/shader_cache/dxil'
        shutil.copytree(shipped, cache)
        job['gates'] = proof.genv(job['gates'], 'SR_SHADER_CACHE_DIR=' + str(cache))
        return original_replay_job(job, out)

    replay_batch.run_job = pinned_replay_job
    # Explicit environment for this owned child; do not modify the caller's shell.
    for key in list(os.environ):
        if key.upper().startswith('SR_'):
            del os.environ[key]
    os.environ.update(SR_WINDOW_BACKGROUND='1', SR_HARNESS_INPUT='1', SR_HARNESS_REAL_INPUT='0',
                      SR_PORT_FEATURES='0', SR_DEVTOOLS='0', SR_ALLOW_FORCING='0', SR_PYTHON=sys.executable)
    out = manifest_path.parent / ('zpd_full_bake_' + a.phase)
    args = ['--plan', 'zpd_full_bake_' + a.phase, '--exe', manifest['exe'], '--base', manifest['base'],
            '--repo', str(tools), '--stage-source', str(ROOT), '--stage', manifest['stage'],
            '--out', str(out), '--owner', a.owner, '--graceful-quit']
    if a.slot_held:
        args.append('--slot-held')
    if not a.execute:
        args.append('--dry-run')
    # Current tools, complete checkout, and explicit disc source remain distinct.
    os.chdir(tools)
    return proof.main(args)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('prepare')
    for name in ('tools', 'exe', 'exe-sha256', 'base', 'base-sha256', 'baseline-provenance', 'out'):
        p.add_argument('--' + name, required=True)
    p.add_argument('--stage', default='J:/sr_stage')
    r = sub.add_parser('run')
    r.add_argument('--manifest', required=True)
    r.add_argument('--phase', choices=('correctness', 'pacing'), required=True)
    r.add_argument('--owner', default='m2-zpd-full-bake')
    r.add_argument('--execute', action='store_true')
    r.add_argument('--slot-held', action='store_true')
    r.add_argument('--timing-isolated', action='store_true')
    a = parser.parse_args()
    try:
        return prepare(a) if a.command == 'prepare' else run(a)
    except (OSError, ValueError) as error:
        print('REFUSED: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
