#!/usr/bin/env python3
"""iter-465 player lane: frame rate PER ERA from a roll's log(s).

    python tools/player/era_fps.py <run.log> [<run_r1.log> ...] [--json out.json]

Eras are cut at the game's own markers (in log order):
    boot     log start                         .. ScrPressStart requested (intro movies play here)
    title    path='screens/bootflow/ScrPressStart' .. claim LoadCreate (the custom MAIN MENU)
    menu     claim LoadCreate                  .. RESTART NOW / NEW GAME answer
    loading  answer cppId=... button=0         .. path='screens/hud/main_hud'
    gameplay path='screens/hud/main_hud'       .. log end
Per era, from the [HIGHFPS_PACE] 2-second windows (kernel/highfps.cpp; needs the feature on or
SR_HIGHFPS_PACE=1): median DISPLAYED fps (host_presents/s - what the screen gets), median guest
presents/s, the host present interval p50/p95 and the sim ticks/s (30 = the game's own rate);
from [PM4_GPU_TIME] (SR_PM4_GPU_TIME=1): GPU busy ms per frame and the internal / backbuffer
size; from [PM4_SWAP_TIME]: the render-thread phases (ms). [MOVIE_PACE] lines list the movies.
Windows that straddle an era boundary go to the era their END falls in.
"""
import argparse
import json
import re
import statistics
import sys

TS = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]')
PACE = re.compile(r'\[HIGHFPS_PACE\] w=\d+ ([\d.]+)s presents=\d+ \(([\d.]+)/s\) interval_ms p50=([\d.]+) p95=([\d.]+)'
                  r'.*sim_ticks/s=([\d.]+).*host_presents=\d+ \(([\d.]+)/s\) host_ms p50=([\d.]+) p95=([\d.]+) p99=([\d.]+) max=([\d.]+)')
GPU = re.compile(r'\[PM4_GPU_TIME\] avg over 120 frames: gpu busy ([\d.]+) ms \(max ([\d.]+)\), span ([\d.]+) ms.*'
                 r'present interval ([\d.]+) ms \(([\d.]+) fps\); internal (\d+)x(\d+) \(scale (\d+)x(\d+)\) -> backbuffer (\d+)x(\d+)')
SWAP = re.compile(r'\[PM4_SWAP_TIME\] avg over 120: render=([\d.]+) scanout=([\d.]+) submit=([\d.]+) present=([\d.]+) '
                  r'frameWait=([\d.]+) ww=([\d.]+)')
# iter-466 hfr: the 120 fps budget columns - the engine's own render/sim tick cost
# ([HIGHFPS_PACE] tick_cost_ms) and the CP wall split per swap ([GPU_CP_TIME], SR_PM4_DIAG_TIMING=1).
TICK = re.compile(r'render_ticks/s=([\d.]+).*tick_cost_ms render=([\d.]+) sim=([\d.]+)')
CPT = re.compile(r'\[(?:GPU_CP_TIME|NATIVE_SUBMIT_TIME)\] \S+ wall=(\d+)ms swaps=\+(\d+) \([\d.]+/s\) idle=(\d+) ib=(\d+) swap=(\d+) isr=(\d+)')
MOVIE = re.compile(r'\[MOVIE_PACE\] movie#(\d+) (\S+) wall=([\d.]+)s frames=(\d+) calls=\d+ fps=([\d.]+)')
MARKS = [
    ('title', re.compile(r"path='screens/bootflow/ScrPressStart'")),
    ('menu', re.compile(r'\[SR_FSCMD\].*claim LoadCreate')),
    ('loading', re.compile(r'\[SR_FSCMD\].*answer cppId=\S+ button=0')),
    ('gameplay', re.compile(r"path='screens/hud/main_hud'")),
]
# 'ingame' (iter-465 perf): gameplay proper = after the first movie that ENDS after the HUD
# request (a NEW GAME's opening FMV plays right after main_hud is requested).
ORDER = ['boot', 'title', 'menu', 'loading', 'gameplay', 'ingame']


def line_ts(line):
    m = TS.match(line)
    if not m:
        return None
    import datetime
    return datetime.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f').timestamp()


def med(xs):
    return round(statistics.median(xs), 2) if xs else None


def scan(path):
    eras = {e: {'pace': [], 'gpu': [], 'swap': [], 'tick': [], 'cp': []} for e in ORDER}
    movies = []
    era = 'boot'
    hud_ts = None
    with open(path, 'rb') as f:
        for raw in f:
            line = raw.decode('utf-8', 'replace')
            for name, rx in MARKS:
                if ORDER.index(name) > ORDER.index(era) and rx.search(line):
                    era = name
                    if name == 'gameplay':
                        hud_ts = line_ts(line)
            m = PACE.search(line)
            if m:
                eras[era]['pace'].append(dict(win_s=float(m.group(1)), guest_fps=float(m.group(2)),
                                              guest_p50=float(m.group(3)), sim_tps=float(m.group(5)),
                                              host_fps=float(m.group(6)), host_p50=float(m.group(7)),
                                              host_p95=float(m.group(8)), host_p99=float(m.group(9)),
                                              host_max=float(m.group(10))))
                t = TICK.search(line)
                if t:
                    eras[era]['tick'].append(dict(rtps=float(t.group(1)), render=float(t.group(2)),
                                                  sim=float(t.group(3))))
                continue
            m = CPT.search(line)
            if m and int(m.group(2)) > 0:
                n = float(m.group(2))
                eras[era]['cp'].append(dict(idle=int(m.group(3)) / n, ib=int(m.group(4)) / n,
                                            swap=int(m.group(5)) / n, isr=int(m.group(6)) / n))
                continue
            m = GPU.search(line)
            if m:
                eras[era]['gpu'].append(dict(busy=float(m.group(1)), busy_max=float(m.group(2)),
                                             fps=float(m.group(5)), internal='%sx%s' % (m.group(6), m.group(7)),
                                             backbuffer='%sx%s' % (m.group(10), m.group(11))))
                continue
            m = SWAP.search(line)
            if m:
                eras[era]['swap'].append(dict(render=float(m.group(1)), scanout=float(m.group(2)),
                                              submit=float(m.group(3)), present=float(m.group(4)),
                                              frameWait=float(m.group(5))))
                continue
            m = MOVIE.search(line)
            if m:
                # The opening FMV: a movie that STARTED after the HUD request (end - wall).
                t_end = line_ts(line)
                if era == 'gameplay' and hud_ts and t_end and t_end - float(m.group(3)) >= hud_ts - 1.0:
                    era = 'ingame'
                movies.append(dict(n=int(m.group(1)), name=m.group(2), wall_s=float(m.group(3)),
                                   frames=int(m.group(4)), fps=float(m.group(5))))
    return eras, movies


def summarize(eras):
    out = {}
    for e in ORDER:
        p, g, s = eras[e]['pace'], eras[e]['gpu'], eras[e]['swap']
        if not (p or g):
            continue
        out[e] = dict(
            windows=len(p),
            host_fps_median=med([x['host_fps'] for x in p]),
            host_fps_min=round(min(x['host_fps'] for x in p), 1) if p else None,
            guest_fps_median=med([x['guest_fps'] for x in p]),
            host_ms_p50_median=med([x['host_p50'] for x in p]),
            host_ms_p95_median=med([x['host_p95'] for x in p]),
            sim_tps_median=med([x['sim_tps'] for x in p]),
            gpu_busy_ms_median=med([x['busy'] for x in g]),
            gpu_busy_ms_max=round(max(x['busy_max'] for x in g), 2) if g else None,
            internal=g[-1]['internal'] if g else None,
            backbuffer=g[-1]['backbuffer'] if g else None,
            render_ms_median=med([x['render'] for x in s]),
            present_ms_median=med([x['present'] for x in s]),
            frame_wait_ms_median=med([x['frameWait'] for x in s]),
            scanout_ms_median=med([x['scanout'] for x in s]),
            submit_ms_median=med([x['submit'] for x in s]),
            render_ticks_per_s_median=med([x['rtps'] for x in eras[e]['tick']]),
            guest_render_tick_ms_median=med([x['render'] for x in eras[e]['tick']]),
            sim_tick_ms_median=med([x['sim'] for x in eras[e]['tick']]),
            cp_idle_ms_per_swap=med([x['idle'] for x in eras[e]['cp']]),
            cp_ib_ms_per_swap=med([x['ib'] for x in eras[e]['cp']]),
            cp_swap_ms_per_swap=med([x['swap'] for x in eras[e]['cp']]),
        )
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--json', default='')
    a = ap.parse_args()
    report = {}
    for path in a.logs:
        eras, movies = scan(path)
        s = summarize(eras)
        report[path] = dict(eras=s, movies=movies)
        print('== %s' % path)
        print('%-9s %4s %8s %7s %8s %8s %8s %6s %8s %8s %-9s %-9s %7s %7s' % (
            'era', 'win', 'shownFPS', 'minFPS', 'guestFPS', 'p50ms', 'p95ms', 'sim/s', 'gpuMs', 'gpuMax',
            'internal', 'backbuf', 'render', 'present'))
        for e, r in s.items():
            print('%-9s %4d %8s %7s %8s %8s %8s %6s %8s %8s %-9s %-9s %7s %7s' % (
                e, r['windows'], r['host_fps_median'], r['host_fps_min'], r['guest_fps_median'],
                r['host_ms_p50_median'], r['host_ms_p95_median'], r['sim_tps_median'], r['gpu_busy_ms_median'],
                r['gpu_busy_ms_max'], r['internal'], r['backbuffer'], r['render_ms_median'], r['present_ms_median']))
        print('  budget (ms/frame, medians): era | guest render tick | sim tick | render ticks/s | host RT render+scanout+submit / present / frameWait | CP idle / ib / swap per swap | GPU busy')
        for e, r in s.items():
            print('  %-9s %6s %6s %6s | %s+%s+%s / %s / %s | %s / %s / %s | %s' % (
                e, r['guest_render_tick_ms_median'], r['sim_tick_ms_median'], r['render_ticks_per_s_median'],
                r['render_ms_median'], r['scanout_ms_median'], r['submit_ms_median'], r['present_ms_median'],
                r['frame_wait_ms_median'], r['cp_idle_ms_per_swap'], r['cp_ib_ms_per_swap'],
                r['cp_swap_ms_per_swap'], r['gpu_busy_ms_median']))
        for mv in movies:
            print('  movie #%d %s: %.1f fps over %.1f s (%d frames)' % (mv['n'], mv['name'], mv['fps'], mv['wall_s'], mv['frames']))
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(report, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
