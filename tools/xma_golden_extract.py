#!/usr/bin/env python3
"""xma_golden_extract.py - pull the game's REAL XMA packets out of the Xenia oracle's golden dumps.

The oracle (xenia-canary canary_experimental@0bd090dbe, xma_decoder="new") keeps its XMA
context array in guest memory (VA 0xFFCA9000, 320 x 64 B; findings.json). Each active EA
context names its input buffers and its output ring by PHYSICAL address. Xenia's physical
memory lives at host physical_membase = mapping_base + 0x100000000 = 0x200000000 in these
dumps (memory.cc: physical_membase_ = mapping_base_ + 0x100000000ull), so phys p is host
0x200000000 + p.

For every context with a non-zero input buffer it writes, under <out>/<dump>/ctxNNN/:
  ctx.bin     the 64-byte context (big-endian, as in guest memory)
  ctx.json    decoded fields (xma_context.h bitfield layout)
  in0.bin / in1.bin   the input buffers (2048 B x packet_count)
  out.bin     the output ring (256 B x block_count), the PCM Xenia last wrote (BE int16)

Usage: python tools/xma_golden_extract.py [--dump sound_gameplay|sound_menu] [--out DIR]
Read-only over the dump. Needs tools/golden.py from the main tree (or this tree).
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
for cand in (HERE, os.path.join(REPO, "tools")):
    if os.path.exists(os.path.join(cand, "golden.py")):
        sys.path.insert(0, cand)
        break
import golden  # noqa: E402

CTX_ARRAY_VA = 0xFFCA9000
CTX_COUNT = 320
PHYS_HOST_BASE = 0x200000000


def bits(w, lo, n):
    return (w >> lo) & ((1 << n) - 1)


def decode_ctx(raw):
    w = struct.unpack(">16I", raw)
    return {
        "input_buffer_0_packet_count": bits(w[0], 0, 12),
        "loop_count": bits(w[0], 12, 8),
        "input_buffer_0_valid": bits(w[0], 20, 1),
        "input_buffer_1_valid": bits(w[0], 21, 1),
        "output_buffer_block_count": bits(w[0], 22, 5),
        "output_buffer_write_offset": bits(w[0], 27, 5),
        "input_buffer_1_packet_count": bits(w[1], 0, 12),
        "loop_subframe_start": bits(w[1], 12, 2),
        "loop_subframe_end": bits(w[1], 14, 3),
        "loop_subframe_skip": bits(w[1], 17, 3),
        "subframe_decode_count": bits(w[1], 20, 4),
        "output_buffer_padding": bits(w[1], 24, 3),
        "sample_rate": bits(w[1], 27, 2),
        "is_stereo": bits(w[1], 29, 1),
        "unk_dword_1_c": bits(w[1], 30, 1),
        "output_buffer_valid": bits(w[1], 31, 1),
        "input_buffer_read_offset": bits(w[2], 0, 26),
        "error_status": bits(w[2], 26, 5),
        "error_set": bits(w[2], 31, 1),
        "loop_start": bits(w[3], 0, 26),
        "loop_end": bits(w[4], 0, 26),
        "packet_metadata": bits(w[4], 26, 5),
        "current_buffer": bits(w[4], 31, 1),
        "input_buffer_0_ptr": w[5],
        "input_buffer_1_ptr": w[6],
        "output_buffer_ptr": w[7],
        "work_buffer_ptr": w[8],
        "output_buffer_read_offset": bits(w[9], 0, 5),
        "stop_when_done": bits(w[9], 30, 1),
        "interrupt_when_done": bits(w[9], 31, 1),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", default="sound_gameplay")
    ap.add_argument("--out", default=os.path.join(REPO, "archive", "v1_audio", "xma_offline", "golden"))
    a = ap.parse_args()
    path = a.dump
    if not os.path.exists(path):
        names = {"sound_gameplay": "xenia_canary_sound_gameplay.DMP",
                 "sound_menu": "xenia_canary_sound_menu.DMP", "nosound": "xenia_canary.DMP"}
        path = os.path.join(REPO, "data", names.get(a.dump, a.dump))
    d = golden.GoldenDump(path)
    outdir = os.path.join(a.out, a.dump)
    os.makedirs(outdir, exist_ok=True)
    arr = d.read(CTX_ARRAY_VA, CTX_COUNT * 64)
    if len(arr) != CTX_COUNT * 64:
        raise SystemExit("context array not readable")
    summary = []
    for i in range(CTX_COUNT):
        raw = arr[i * 64:(i + 1) * 64]
        if raw == bytes(64):
            continue
        f = decode_ctx(raw)
        if not (f["input_buffer_0_ptr"] or f["input_buffer_1_ptr"]):
            continue
        cdir = os.path.join(outdir, f"ctx{i:03d}")
        os.makedirs(cdir, exist_ok=True)
        open(os.path.join(cdir, "ctx.bin"), "wb").write(raw)
        got = {}
        for name, ptr, cnt in (("in0", f["input_buffer_0_ptr"], f["input_buffer_0_packet_count"]),
                               ("in1", f["input_buffer_1_ptr"], f["input_buffer_1_packet_count"])):
            if ptr and cnt:
                b = d.read_host(PHYS_HOST_BASE + ptr, cnt * 2048)
                open(os.path.join(cdir, name + ".bin"), "wb").write(b)
                got[name] = len(b)
        if f["output_buffer_ptr"] and f["output_buffer_block_count"]:
            b = d.read_host(PHYS_HOST_BASE + f["output_buffer_ptr"], f["output_buffer_block_count"] * 256)
            open(os.path.join(cdir, "out.bin"), "wb").write(b)
            got["out"] = len(b)
        f["hw_index"] = i
        f["bytes_read"] = got
        json.dump(f, open(os.path.join(cdir, "ctx.json"), "w"), indent=1)
        summary.append(f)
        print(f"ctx{i:03d} in0={f['input_buffer_0_ptr']:08X}x{f['input_buffer_0_packet_count']}"
              f"(v{f['input_buffer_0_valid']}) in1={f['input_buffer_1_ptr']:08X}x{f['input_buffer_1_packet_count']}"
              f"(v{f['input_buffer_1_valid']}) out={f['output_buffer_ptr']:08X}x{f['output_buffer_block_count']}"
              f" rate={f['sample_rate']} st={f['is_stereo']} sdc={f['subframe_decode_count']}"
              f" rd={f['input_buffer_read_offset']} cur={f['current_buffer']} ow={f['output_buffer_write_offset']}"
              f" or={f['output_buffer_read_offset']} ov={f['output_buffer_valid']} loop={f['loop_count']}"
              f" got={got}")
    json.dump(summary, open(os.path.join(outdir, "summary.json"), "w"), indent=1)
    print(f"{len(summary)} contexts with input buffers -> {outdir}")


if __name__ == "__main__":
    main()
