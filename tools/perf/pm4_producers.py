#!/usr/bin/env python3
"""pm4_producers.py <roll dir> - the P0 PM4 producer census (docs/NATIVE_ROADMAP.md RE-PLAN 2026-10-08).

Reads a perf_run roll made with SR_NATIVE_BRACKETS=1: the [PM4_COVMAP] owner lines (cumulative
packets / dwords / op keys per XDK entry that wrote them, plus UNSTAMPED = no bracket = an inlined
site) and the [PM4_COV] CUM frame counter, differenced across the roll's INGAME era (report.json),
and prints producers by PM4 dwords per frame with their packet mix.

Keys: XX = type-3 opcode, rXX = type-0 register page (reg >> 8... as pm4_coverage keys it), oN = other.
"""
import datetime as dt
import json
import os
import re
import sys

TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]")
CUM = re.compile(r"\[PM4_COV\] CUM (\S+) frames=(\d+) pk=(\d+)")
MAP = re.compile(r"\[PM4_COVMAP\] (\S+) owner=([0-9A-F]+) name=(\S+) pk=(\d+) dw=(\d+) keys=(\S*)")

T3 = {0x10: "NOP", 0x22: "DRAW_INDX", 0x36: "DRAW_INDX_2", 0x23: "VIZ_QUERY", 0x25: "SET_STATE",
      0x2D: "SET_CONSTANT", 0x55: "SET_CONSTANT2", 0x56: "SET_SHADER_CONSTANTS", 0x2F: "LOAD_ALU_CONSTANT",
      0x2E: "LOAD_CONSTANT_CONTEXT", 0x27: "IM_LOAD", 0x2B: "IM_LOAD_IMMEDIATE", 0x3B: "INVALIDATE_STATE",
      0x26: "WAIT_FOR_IDLE", 0x3C: "WAIT_REG_MEM", 0x52: "WAIT_REG_EQ", 0x53: "WAIT_REG_GTE",
      0x21: "REG_RMW", 0x3E: "REG_TO_MEM", 0x3D: "MEM_WRITE", 0x45: "COND_WRITE", 0x44: "COND_EXEC",
      0x46: "EVENT_WRITE", 0x58: "EVENT_WRITE_SHD", 0x59: "EVENT_WRITE_CFL", 0x5A: "EVENT_WRITE_EXT",
      0x5B: "EVENT_WRITE_ZPD", 0x3F: "INDIRECT_BUFFER", 0x37: "INDIRECT_BUFFER_PFD", 0x48: "ME_INIT",
      0x50: "SET_BIN_MASK", 0x51: "SET_BIN_SELECT", 0x60: "SET_BIN_MASK_LO", 0x61: "SET_BIN_MASK_HI",
      0x62: "SET_BIN_SELECT_LO", 0x63: "SET_BIN_SELECT_HI", 0x4A: "SET_SHADER_BASES", 0x5E: "CONTEXT_UPDATE",
      0x54: "INTERRUPT", 0x64: "XE_SWAP", 0x01: "SET_CONFIG_REG"}


def ts(line):
    m = TS.match(line)
    return dt.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp() if m else None


def keyname(k):
    if k.startswith("r"):
        return "type0:" + k
    if k.startswith("o"):
        return "other:" + k
    v = int(k, 16)
    return T3.get(v, "t3:%02X" % v)


def main():
    d = sys.argv[1]
    log = os.path.join(d, "drive", "attempt1", "run.log")
    rep = json.load(open(os.path.join(d, "report.json"), encoding="utf-8"))
    ing = [e for e in rep["eras"] if e.get("name") == "ingame" or e.get("era") == "ingame"][0]
    groups = []  # (t, frames, {owner: (name, pk, dw, keys)})
    cur = None
    t_first = None
    with open(log, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            t = ts(line)
            if t_first is None and t is not None:
                t_first = t
            m = CUM.search(line)
            if m:
                cur = (t, int(m.group(2)), {})
                groups.append(cur)
                continue
            m = MAP.search(line)
            if m and cur is not None and "owner=" in line:
                keys = {}
                for kv in m.group(6).split(","):
                    if ":" in kv:
                        k, v = kv.split(":")
                        keys[k] = int(v)
                cur[2][m.group(2)] = (m.group(3), int(m.group(4)), int(m.group(5)), keys)
    w0 = t_first + ing["t0_s"]
    w1 = w0 + ing["dur_s"]
    inside = [g for g in groups if w0 <= g[0] <= w1 and g[2]]
    if len(inside) < 2:
        sys.exit("fewer than two [PM4_COVMAP] reports inside the ingame era (%d groups total)" % len(groups))
    a, b = inside[0], inside[-1]
    frames = b[1] - a[1]
    rows = []
    tot_dw = tot_pk = 0
    for o, (name, pk, dw, keys) in b[2].items():
        pa = a[2].get(o, (name, 0, 0, {}))
        dpk, ddw = pk - pa[1], dw - pa[2]
        if dpk <= 0:
            continue
        dk = {k: v - pa[3].get(k, 0) for k, v in keys.items() if v - pa[3].get(k, 0) > 0}
        rows.append((ddw, dpk, o, name, dk))
        tot_dw += ddw
        tot_pk += dpk
    rows.sort(reverse=True)
    print("P0 producer census: %s, ingame %.1f-%.1f s, %d frames (reports %.0f s apart)" % (
        d, ing["t0_s"], ing["t0_s"] + ing["dur_s"], frames, b[0] - a[0]))
    print("total: %.0f packets/frame, %.0f dwords/frame (%.1f KB/frame)" % (
        tot_pk / frames, tot_dw / frames, tot_dw * 4 / frames / 1024))
    print("%-9s %-44s %9s %9s %6s  %s" % ("owner", "name", "pk/frame", "dw/frame", "dw%", "top packet kinds (pk/frame)"))
    for ddw, dpk, o, name, dk in rows:
        top = sorted(dk.items(), key=lambda kv: -kv[1])[:5]
        print("%-9s %-44s %9.1f %9.1f %5.1f%%  %s" % (
            o, name[:44], dpk / frames, ddw / frames, 100.0 * ddw / max(1, tot_dw),
            ", ".join("%s %.1f" % (keyname(k), v / frames) for k, v in top)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
