#!/usr/bin/env python3
"""residue_census.py - the p2.transport RESIDUE producers' packets by KIND, from two census rolls (a DIAGNOSTIC).

    python tools/perf/residue_census.py <base roll dir or run.log> <target roll dir or run.log> [--json out.json]

Both rolls run SR_NATIVE_BRACKETS=1 (which implies SR_PM4_COVERAGE): the base on the default (segmentless) transport
with the residue gates off, the target with SR_NATIVE_IDLE_WAIT / SR_NATIVE_INVAL_RANGE / SR_NATIVE_LOCK_COHER on.

The residue plan grades the producers by the producer census instead (native_proof producer_census_item: the
[PM4_COVMAP] inner= lines, each converted producer present in the base roll and absent from the gated one). This tool
stays as an extra diagnostic: the CP's per-frame packet counts of the residue's own packet kinds, from each roll's last
[PM4_COV] CUM line - type-0 packets in register page 0x0A (COHER_STATUS_HOST 0xA31 and COHER_SIZE/BASE_HOST
0xA2F..0xA30: two packets per owner COHER block), WAIT_REG_MEM 0x3C (one per block) and page 0x05 (WAIT_UNTIL 0x5C8:
one per BlockUntilIdle / owner InvalidateGpuCache) - against what the target's own residue counters say it converted:
`lock` + `inval` record spans (each one block that is no longer PM4) and `idle` + `inval` (each one WAIT_UNTIL).
    observed drop per frame = base / frame - target / frame
    expected drop per frame = 2 * (lock + inval) / target frames   (page 0x0A)
                              (lock + inval) / target frames       (0x3C)
                              (idle + inval) / target frames       (page 0x05)
    ratio = observed / expected  (1.0: exactly the converted blocks left the PM4 stream; 0: none did)
Other COHER producers (the SDK Z-pass kick's coherency block 820FD3B8, guest-mode closes) are in both rolls alike.

STRICT: the last CUM line of each roll and the target's last residue line must parse in full - the CUM line's ops= /
regs= lists in gfx/pm4_coverage's exact syntax (two upper-case hex digits per key, keys strictly ascending, counts > 0,
type-0 dwords >= packets), adding up to the line's own totals (ops = t3 + pred, regs packets = t0, regs dwords = t0dw),
every field once; the residue line field for field. A key absent from a well-formed list is a true zero; a malformed
or truncated line is never read as zeros.

Exit 0 with the JSON written; 2 when a roll lacks its census or the target its residue line, or a line is malformed.

--require-terminal (tools/graceful-quit, docs/TOOLKIT.md 1.8m): read each roll's TERMINAL lines - the CUM and residue
lines its graceful quit printed after the CP drained (why token `terminal`, inside a complete `[QUIT] terminal snapshot`
with the quit receipt of a clean exit 0 beside the log, tools/perf/terminal_snapshot.py) - so the per-frame counts cover
the whole run; a roll without a complete snapshot is an
error (exit 2). The JSON then carries "terminal" (each roll's snapshot status and frames). Without the option the last
lines are read as before (on a graceful-quit roll those ARE the terminal lines).
"""
import argparse
import json
import os
import re
import sys

import terminal_snapshot as ts  # tools/graceful-quit: the [QUIT] terminal snapshot reader (same folder)

CUM_TAG = "[PM4_COV] CUM "
CUM = re.compile(r"\[PM4_COV\] CUM (\w+) frames=(\d+) pk=(\d+) pk/f mean=[0-9.]+ min=\d+ max=\d+ t0=(\d+) t0dw=(\d+) "
                 r"t3=(\d+) t3dw=\d+ pred=(\d+) ringIb=\d+ zero=\d+ trunc=\d+ chainTail=\d+ t1=\d+ t2=\d+ t1skip=\d+ "
                 r"acc=(\d+) unst=(\d+)(?: chain=\d+)? setConst=\d+/\d+/\d+/\d+/\d+ loadConst=\d+/\d+/\d+/\d+ snapOverflow=\d+ "
                 r"ops=(\S*) regs=(\S*)\s*$")
OPS = re.compile(r"(?:[0-9A-F]{2}:\d+(?:,[0-9A-F]{2}:\d+)*)?")
REGS = re.compile(r"(?:[0-9A-F]{2}:\d+/\d+(?:,[0-9A-F]{2}:\d+/\d+)*)?")
RESIDUE_TAG = re.compile(r"\[NATIVE_FLUSH\] \w+ residue ")
DECL = r"decl ab=\d+ seg=\d+ mem=\d+ mode=\d+ entry=\d+ owner=\d+ noPk=\d+"
RESIDUE = re.compile(r"\[NATIVE_FLUSH\] (\w+) residue idle=(\d+) late=(\d+) " + DECL +
                     r" \| inval=(\d+) late=(\d+) merge=(\d+) " + DECL +
                     r" \| lock=(\d+) late=(\d+) merge=(\d+) host=(\d+) " + DECL +
                     r" \| pk=(\d+) litPk=(\d+) \| hostApplied=(\d+)\s*$")


class Malformed(Exception):
    pass


def run_log(p):
    if os.path.isdir(p):
        for cand in (os.path.join(p, "drive", "attempt1", "run.log"), os.path.join(p, "run.log")):
            if os.path.exists(cand):
                return cand
    return p


def last(path, tag):
    """The last line holding `tag` (a substring or a compiled pattern), or None."""
    hit = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if (tag.search(line) if hasattr(tag, "search") else tag in line):
                hit = line
    return hit


def keyed(text, pattern, what, pair):
    """A strict ops= / regs= list: {key: count} or {key: (packets, dwords)}; keys strictly ascending."""
    if not pattern.fullmatch(text):
        raise Malformed("%s list is not in the census syntax: %r" % (what, text[:120]))
    out, prev = {}, -1
    for tok in (text.split(",") if text else []):
        k, v = tok.split(":")
        kv = int(k, 16)
        if kv <= prev:
            raise Malformed("%s list keys not strictly ascending at %s" % (what, tok))
        prev = kv
        if pair:
            pk, dw = (int(x) for x in v.split("/"))
            if pk <= 0 or dw < pk:
                raise Malformed("%s list entry %s: packets must be > 0 and dwords >= packets" % (what, tok))
            out[k] = (pk, dw)
        else:
            if int(v) <= 0:
                raise Malformed("%s list entry %s: count must be > 0" % (what, tok))
            out[k] = int(v)
    return out


def census(line):
    m = CUM.search(line)
    if not m:
        raise Malformed("CUM line does not parse in full: %r" % line.strip()[:200])
    frames, pk, t0, t0dw, t3, pred, acc, unst = (int(m.group(i)) for i in (2, 3, 4, 5, 6, 7, 8, 9))
    ops = keyed(m.group(10), OPS, "ops", False)
    regs = keyed(m.group(11), REGS, "regs", True)
    if frames <= 0:
        raise Malformed("CUM line with frames=%d" % frames)
    if sum(ops.values()) != t3 + pred:
        raise Malformed("ops list adds up to %d, the line says t3 + pred = %d" % (sum(ops.values()), t3 + pred))
    if sum(p for p, _ in regs.values()) != t0 or sum(d for _, d in regs.values()) != t0dw:
        raise Malformed("regs list adds up to %d / %d, the line says t0 = %d, t0dw = %d" % (
            sum(p for p, _ in regs.values()), sum(d for _, d in regs.values()), t0, t0dw))
    per = {"coher_reg_pk": regs.get("0A", (0, 0))[0] / frames, "wait_reg_mem_pk": ops.get("3C", 0) / frames,
           "wait_until_page_pk": regs.get("05", (0, 0))[0] / frames}
    return {"frames": frames, "packets": pk, "accounted": acc, "unstamped": unst, "per_frame": per}


def compute(base_log, target_log, require_terminal=False, stranded_policy="allow"):
    """stranded_policy (native/quit-drain round 2; terminal_snapshot.STRANDED_POLICIES), with require_terminal: "allow" -
    a snapshot with stranded threads is complete (the counts close at the ring cut); "forbid" - it is not. Either way
    out["terminal"] carries each side's stranded / stranded_threads / stranded_mismatch and the policy."""
    if stranded_policy not in ts.STRANDED_POLICIES:
        raise ValueError("stranded_policy must be one of %s" % (ts.STRANDED_POLICIES,))
    out = {"base": {"log": base_log}, "target": {"log": target_log}}
    if require_terminal:  # tools/graceful-quit: each roll's terminal lines, inside a complete snapshot
        lines = {"base": ts.read_lines(base_log), "target": ts.read_lines(target_log)}
        # the exit evidence too (Astra round 1, P1-3): each log's quit receipt, found beside it
        logs = {"base": base_log, "target": target_log}
        snaps = {k: ts.read(v, ts.find_receipt(logs[k])) for k, v in lines.items()}
        out["terminal"] = {k: dict({"status": s["status"], "frames": s["frames"], "tail": s["tail"],
                                    "exit_reason": s["exit_reason"]}, **ts.stranded_view(s, stranded_policy))
                           for k, s in snaps.items()}
        absent = [k for k, s in snaps.items() if not ts.accept(s, stranded_policy)]
        if absent:
            out["error"] = "no complete terminal snapshot: " + ", ".join(
                "%s (%s%s)" % (k, snaps[k]["status"],
                               ", stranded %s / %d record(s), policy %s" % (
                                   snaps[k].get("stranded"), len(snaps[k].get("stranded_threads") or []),
                                   stranded_policy) if snaps[k].get("stranded") else "") for k in absent)
            return out
        cum_rx = re.compile(re.escape(CUM_TAG))

        def pick(path, tag):
            side = "base" if path is base_log else "target"
            return ts.last_line(lines[side], cum_rx if tag is CUM_TAG else tag, True, snaps[side])
    else:
        pick = last
    bl, tl = pick(base_log, CUM_TAG), pick(target_log, CUM_TAG)
    rl = pick(target_log, RESIDUE_TAG)
    if not bl or not tl or not rl:
        out["error"] = "missing: " + ", ".join(n for n, v in (("base census", bl), ("target census", tl),
                                                               ("target residue line", rl)) if not v)
        return out
    try:
        out["base"].update(census(bl))
        out["target"].update(census(tl))
        m = RESIDUE.search(rl)
        if not m:
            raise Malformed("residue line does not parse in full: %r" % rl.strip()[:200])
        brl = pick(base_log, RESIDUE_TAG)
        if brl and not RESIDUE.search(brl):
            raise Malformed("base residue line does not parse in full: %r" % brl.strip()[:200])
    except Malformed as e:
        out["error"] = "malformed: %s" % e
        return out
    out["base"]["residue_line"] = brl is not None  # the base runs the gates off: no line
    g = [int(x) for x in m.groups()[1:]]
    res = {"idle": g[0], "idle_late": g[1], "inval": g[2], "inval_late": g[3], "inval_merge": g[4], "lock": g[5],
           "lock_late": g[6], "lock_merge": g[7], "host": g[8], "pk": g[9], "litPk": g[10], "hostApplied": g[11]}
    out["target"]["residue"] = res
    tf = out["target"]["frames"]
    blocks = (res["lock"] + res["inval"]) / tf
    waits = (res["idle"] + res["inval"]) / tf
    out["converted_blocks_per_frame"] = blocks
    out["converted_wait_until_per_frame"] = waits
    exp = {"coher_reg_pk": 2.0 * blocks, "wait_reg_mem_pk": blocks, "wait_until_page_pk": waits}
    bp, tp = out["base"]["per_frame"], out["target"]["per_frame"]
    obs = {k: bp[k] - tp[k] for k in exp}
    out["expected_drop_per_frame"] = exp
    out["observed_drop_per_frame"] = obs
    out["ratio"] = {"coher_reg": obs["coher_reg_pk"] / exp["coher_reg_pk"] if exp["coher_reg_pk"] else None,
                    "wait_reg_mem": obs["wait_reg_mem_pk"] / exp["wait_reg_mem_pk"] if exp["wait_reg_mem_pk"] else None,
                    "wait_until": obs["wait_until_page_pk"] / exp["wait_until_page_pk"] if exp["wait_until_page_pk"] else None}
    # the base census saw at least the packets the target converted (its COHER / WAIT_REG_MEM PM4 is there to remove)
    out["base_holds_converted"] = bool(bp["coher_reg_pk"] >= exp["coher_reg_pk"] and bp["wait_reg_mem_pk"] >= exp["wait_reg_mem_pk"])
    for side in ("base", "target"):
        s = out[side]
        s["unstamped_share"] = s["unstamped"] / s["packets"] if s["packets"] else None
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("base")
    ap.add_argument("target")
    ap.add_argument("--json")
    ap.add_argument("--require-terminal", action="store_true",
                    help="read each roll's graceful-quit terminal lines (the whole run); a roll without them is an error")
    ap.add_argument("--stranded-policy", choices=ts.STRANDED_POLICIES, default="allow",
                    help="with --require-terminal: a snapshot with stranded threads counts (allow) or not (forbid); "
                         "the count is in the JSON either way")
    a = ap.parse_args()
    out = compute(run_log(a.base), run_log(a.target), require_terminal=a.require_terminal,
                  stranded_policy=a.stranded_policy)
    if a.require_terminal and "terminal" in out:
        print("residue_census terminal: base stranded %s, target stranded %s (policy %s)" % (
            out["terminal"]["base"]["stranded"], out["terminal"]["target"]["stranded"], a.stranded_policy))
    text = json.dumps(out, indent=1)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    if "error" in out:
        print("residue_census: %s" % out["error"], file=sys.stderr)
        return 2
    r, e, o = out["ratio"], out["expected_drop_per_frame"], out["observed_drop_per_frame"]
    print("converted per frame: %.3f blocks, %.3f WAIT_UNTIL" % (out["converted_blocks_per_frame"],
                                                                  out["converted_wait_until_per_frame"]))
    for k, rk in (("coher_reg_pk", "coher_reg"), ("wait_reg_mem_pk", "wait_reg_mem"), ("wait_until_page_pk", "wait_until")):
        print("%-20s base %.3f target %.3f drop %.3f expected %.3f ratio %s" % (
            k, out["base"]["per_frame"][k], out["target"]["per_frame"][k], o[k], e[k],
            "%.3f" % r[rk] if r[rk] is not None else "-"))
    print("unstamped share: base %.3f target %.3f" % (out["base"]["unstamped_share"], out["target"]["unstamped_share"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
