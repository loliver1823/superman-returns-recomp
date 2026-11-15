"""Generates tools/scenarios/devtools_skip_menu.json (iter-465 playthrough lane): the live proof of the
Dev Tools LEVEL SKIP hub - SIDE MISSIONS (a race, a Bizarro rampage), SKIP: <active mission> (menu)
and SKIP CHAPTER, all from the in-game pause menu > OPTIONS > (GAME) DEV TOOLS > LEVEL SKIP (lane E L-c: the 9-row
pause menu has no MORE OPTIONS; DEV TOOLS is the OPTIONS GAME tab's last row in a dev build).

Run (port features ON = the custom pause menu; dev tools on from the env; the Metropolis-return plan
gets the game past Warworld first, where races and rampages open):
  .\\tools\\sr_drive.ps1 -Scenario tools\\scenarios\\devtools_skip_menu.json -Out <dir> -TimeoutSeconds 2400 -Gates
    "SR_INSTANCE_ID=playthrough,SR_PORT_FEATURES=1,SR_DEVTOOLS=levelskip+devmenu,
     SR_DEVTOOLS_CMDS=<abs>\\data\\devtools\\plans\\metropolis_return_from_new_game.txt,SR_DEVTOOLS_OUT=<dir>\\devtools,
     SDL_AUDIODRIVER=disk"
"""
import json
import os

FS = r"\[SR_FSCMD\]"


def k(name, key, expect=None, ms=15000, dwell=1500, fresh=True, pulses=1, hold=250, optional=False):
    s = {"name": name, "key": key, "holdMs": hold, "maxPulses": pulses, "gapMs": 1500, "dwellMs": dwell}
    if expect:
        s["expect"] = expect
        s["expectMs"] = ms
        s["expectFresh"] = fresh
    if optional:
        s["optional"] = True
    return s


def open_levelskip(tag):
    """pause -> OPTIONS -> (GAME) DEV TOOLS -> LEVEL SKIP (the hub page; lane E L-c)."""
    return [
        {"name": tag + "-pause", "key": "ENTER", "holdMs": 2000, "holdUntilConfirmed": True, "maxPulses": 6,
         "gapMs": 8000, "expect": FS + r".*SR_Log 'pause install'", "expectFresh": True, "expectMs": 10000,
         "dwellMs": 2500},
        k(tag + "-up-to-options", "UP", pulses=2, dwell=2000),   # RESUME -> (wrap) QUIT GAME -> OPTIONS
        k(tag + "-open-options", "A", r"\[PC_SETTINGS\] shown \(IG", dwell=3000),
        k(tag + "-down-to-devtools", "DOWN", pulses=10, dwell=1500),   # the GAME tab's last row (a dev build): DEV TOOLS
        k(tag + "-open-devtools", "A", FS + r".*SR_Log 'menu show main", dwell=2500),
        k(tag + "-down-to-levelskip", "DOWN", dwell=1200),
        k(tag + "-open-levelskip", "A", FS + r".*SR_Log 'menu show levelskip", dwell=4000),
    ]


def close_to_game(tag):
    """LEVEL SKIP -> B (dev main) -> B (close: the pause menu, OPTIONS focused) -> B (resume)."""
    return [
        k(tag + "-back-main", "B", FS + r".*SR_Log 'menu show main", dwell=1500),
        k(tag + "-back-close", "B", FS + r".*SR_Log 'menu close'", dwell=2000),
        k(tag + "-resume", "B", dwell=4000),
    ]


def fly_away(tag):
    """Take off (Y) and fly forward 7 s: out of the minigame's start radius, so it does not restart."""
    return [
        {"name": tag + "-takeoff", "key": "Y", "holdMs": 200, "maxPulses": 1, "dwellMs": 600},
        {"name": tag + "-fly", "key": "KEY_W", "holdMs": 7000, "maxPulses": 1, "dwellMs": 1500},
    ]


steps = [
    {"name": "fe-alive", "await": r"\[NAT_FE\] #1 natural FE master tick", "timeoutMs": 240000},
    {"name": "settle-title", "await": r"\[NAT_FE\] #256 natural FE master tick", "timeoutMs": 240000},
    {"name": "press-start", "key": "ENTER", "holdMs": 2000, "holdUntilConfirmed": True, "maxPulses": 4,
     "gapMs": 12000, "expect": r"\[APT_SCREEN\] path='main'|\[APT_LOADMOVIE\].*\(\"main\"\)", "expectMs": 90000},
    {"name": "title-ui-tail", "await": r"\[APT_REQ\] #\d+ path='\.\./common/ButtonHints/XBox360/HintB'",
     "timeoutMs": 240000},
    {"name": "title-visible", "requireVisibleMs": 6000, "visibleTimeoutMs": 200000, "dwellMs": 3000},
    {"name": "start-bootflow", "key": "ENTER", "holdMs": 2000, "holdUntilConfirmed": True, "maxPulses": 3,
     "gapMs": 10000, "expect": FS + r".*claim LoadCreate", "expectMs": 30000},
    {"name": "menu-visible", "mouse": "move 640,560; wait 1500; move 660,580; wait 2000", "dwellMs": 2000},
    {"name": "new-game", "mouse": "moveitem 0; wait 2500; clickitem 0",
     "expect": FS + r".*answer cppId=\S+ button=0", "expectMs": 30000},
    {"name": "game-starts", "expect": r"\[APT_REQ\] #\d+ path='screens/hud/main_hud'", "expectMs": 240000},
    {"name": "metropolis-return", "purpose": "the Metropolis-return plan is done: races and rampages are open",
     "await": r"\[DEVTOOL\] levelskip \w+ at k=|\[ICALL_FAIL\]", "timeoutMs": 1500000},
    {"name": "settle", "dwellMs": 15000},
]
# Proven in roll menu2 (archive/v1_playthrough/rolls/menu2): SIDE MISSIONS -> RACE #95 started and
# completed the race; SKIP: RACE #95 (menu) completed the race the game had restarted by itself.
# This scenario: A) SIDE MISSIONS -> BIZARRO RAMPAGE #67, B) SKIP: <active> (menu), C) SKIP CHAPTER.
# Superman is flown out of a minigame's start radius after each skip by the companion harness script
# (gen_devtools_skip_menu_play.txt: player keymap W + Space), so the minigame does not restart.
steps += open_levelskip("biz")
steps += [k("biz-down-to-side", "DOWN", pulses=2, dwell=1200),
          k("biz-open-side", "A", FS + r".*SR_Log 'menu show sidemissions", dwell=3000),
          k("biz-more1-down", "DOWN", pulses=4, dwell=1200),
          k("biz-more1", "A", FS + r".*SR_Log 'menu show sidemissions:4", dwell=3000),
          k("biz-more2-down", "DOWN", pulses=4, dwell=1200),
          k("biz-more2", "A", FS + r".*SR_Log 'menu show sidemissions:8", dwell=3000),
          k("biz-down-to-67", "DOWN", dwell=1200),
          k("biz-pick-67", "A", r"\[DEVTOOL\] levelskip armed plan=.*menu_side_67", ms=10000, dwell=4000)]
steps += close_to_game("biz")
# SIDE MISSIONS STARTS the side mission (a reach plan since d73bd185, iter-465: "SIDE MISSIONS now only starts the
# side mission"; SKIP below completes it): the pick's proof is the reach, not a pass (the Ally lc4 roll 523 logged
# act=levelskip.reach args=67 PlayableBIZ, then levelskip done at k=1, and never a pass).
steps += [{"name": "biz-done", "await": r"\[DEVTOOL\] act=levelskip\.reach args=67 PlayableBIZ|\[ICALL_FAIL\]",
           "timeoutMs": 500000},
          {"name": "biz-after", "purpose": "the rampage restarts by itself (Superman is on its statue)", "dwellMs": 25000}]
steps += open_levelskip("skip")
steps += [k("skip-press", "A", r"\[DEVTOOL\] submit ticket=\d+ \"skip_mission\"", ms=10000, dwell=4000,
            optional=True)]
steps += close_to_game("skip")
steps += [{"name": "skip-done", "await": r"\[DEVTOOL\] act=levelskip\.pass args=\d+ \S+ ticket=|refused act=levelskip\.skip_mission",
           "timeoutMs": 120000, "optional": True},
          {"name": "skip-after", "purpose": "the harness flies Superman away meanwhile", "dwellMs": 30000}]
steps += open_levelskip("chapter")
steps += [k("chapter-down", "DOWN", dwell=1200),
          # SKIP CHAPTER is a tick-side controller since iter-465 (devToolsSubmit "skip_chapter"), not a plan: the press
          # submits a ticket, the controller logs start / done (the Ally lc4 roll 525: no 'armed plan' line)
          k("chapter-press", "A", r"\[DEVTOOL\] submit ticket=\d+ \"skip_chapter\"", ms=10000, dwell=4000)]
steps += close_to_game("chapter")
steps += [{"name": "chapter-done", "await": r"\[DEVTOOL\] skip_chapter ticket=\d+ done chapter|\[ICALL_FAIL\]",
           "timeoutMs": 500000},
          {"name": "chapter-after", "purpose": "the NEW CHAPTER card", "dwellMs": 40000}]

doc = {
    "name": "devtools-skip-menu",
    "purpose": "iter-465 playthrough lane: live proof of the Dev Tools LEVEL SKIP hub (generated by "
               "gen_devtools_skip_menu.py; see its docstring for the gates). Dev-tool assisted, port features on.",
    "gates": "SR_KB_PAD_OBS=1,SR_MOUSE_UI_LOG=1,SR_PRESENT_DUMP=1,SR_PRESENT_DUMP_SPARSE_EVERY=60,"
             "SR_PRESENT_DUMP_SPARSE_CAP=1500",
    "steps": steps,
}
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "devtools_skip_menu.json")
with open(out, "w", newline="\n") as f:
    json.dump(doc, f, indent=1)
print(out, len(steps))
