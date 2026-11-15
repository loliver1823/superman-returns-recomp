#!/usr/bin/env python3
"""ea_snr_xma.py - carve the game's EA-XMA sounds (EAAC SNR/SNS, codec 3) out of sound.AST.

How EA stores XMA on the disc (iter-463, verified by finding the Xenia oracle's live XMA input
packets byte-for-byte in sound.AST - tools/xma_golden_extract.py + this tool):

  sound.AST (BGFA1.05) holds ~1.8K asset records, each starting with an "rbas" property chunk
  (cmun / mlov / llaf / ... little-endian). Inside, EA Audio Core "SNR" headers (big-endian):
    h1 = be32: version (>>28), codec (>>24 & 0xF; 3 = EA-XMA), channel_config (>>18 & 0x3F;
         channels = +1), sample_rate (& 0x3FFFF)
    h2 = be32: type (>>30; 0 = RAM, 1 = stream), loop flag (>>29 & 1), num_samples (& 0x1FFFFFFF)
    [be32 loop_start]  when the loop flag is set
  followed directly (RAM sounds) by "SNS" blocks:
    be32 (flag << 24 | block_size), be32 num_samples, then per sub-stream a be32 header whose low
    12 bits are 0x012, then 2048-byte XMA packets (XMA1-style headers, metadata 0) - STORED RAW,
    not compressed. (The design pass's "RefPack/zlib-compressed, carving not viable" was wrong:
    those 2-byte "markers" occur at the random rate in 300 MB of entropy-coded audio.)
  The last block has flag 0x80.

Outputs to <out>/: sNNNN_<fileoff>.xma (the packets, concatenated), index.json.
Optional --wav-wrap writes sNNNN.xma1.wav: the same packets in a RIFF WAVE_FORMAT_XMA (0x0165)
container so stock FFmpeg's xma1 decoder can decode them for a cross-check.

Usage: python tools/ea_snr_xma.py [AST] [--out DIR] [--limit N] [--wav-wrap]
Read-only over the AST.
"""
import argparse
import json
import mmap
import os
import re
import struct

RATES = {11025, 16000, 22050, 24000, 32000, 44100, 48000}


REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def plausible_packet(b):
    if len(b) < 2048:
        return False
    fo = (((b[0] & 3) << 13) | (b[1] << 5) | (b[2] >> 3)) + 32
    return 32 <= fo <= 16384


def parse_snr(m, o):
    if o + 8 > len(m):
        return None
    h1, h2 = struct.unpack_from(">II", m, o)
    ver, codec = h1 >> 28, (h1 >> 24) & 0xF
    ch, rate = ((h1 >> 18) & 0x3F) + 1, h1 & 0x3FFFF
    typ, loop, ns = h2 >> 30, (h2 >> 29) & 1, h2 & 0x1FFFFFFF
    if ver != 0 or codec != 3 or rate not in RATES or ch > 8 or typ > 1 or ns == 0:
        return None
    hdr = 8
    loop_start = None
    if loop:
        loop_start = struct.unpack_from(">I", m, o + 8)[0]
        hdr = 12
    return dict(off=o, channels=ch, rate=rate, type=typ, loop=loop, loop_start=loop_start,
                num_samples=ns, sns=o + hdr)


def walk_sns(m, o, channels, num_samples):
    """Returns (packet bytes, blocks, ok, streams) for SNS blocks at o. A RAM sound is ONE block
    without the 0x80 flag; blocks are walked until their sample counts reach num_samples."""
    nsub = (channels + 1) // 2
    out = []
    blocks = 0
    got = 0
    while o + 8 <= len(m):
        bh, bsamples = struct.unpack_from(">II", m, o)
        flag, size = bh >> 24, bh & 0xFFFFFF
        if flag not in (0x00, 0x80) or size < 12 or o + size > len(m):
            return b"".join(out), blocks, False, nsub
        payload = m[o + 8:o + size]
        if nsub == 1:
            sub = struct.unpack_from(">I", payload, 0)[0]
            pk = payload[4:]
            if (sub & 0xFFF) != 0x012 or len(pk) % 2048 or not plausible_packet(pk):
                return b"".join(out), blocks, False, nsub
            out.append(pk)
        else:
            return b"".join(out), blocks, False, nsub   # multi-substream: not carved here
        blocks += 1
        o += size
        got += bsamples
        if flag == 0x80 or got >= num_samples:
            return b"".join(out), blocks, True, nsub
    return b"".join(out), blocks, False, nsub


def xma1_wav(packets, rate, channels):
    """RIFF WAVE_FORMAT_XMA (0x0165), XMAWAVEFORMAT with one stream (xma2defs.h layout)."""
    # XMAWAVEFORMAT: wFormatTag, wBitsPerSample, wEncodeOptions, wLargestSkip, wNumStreams,
    # bLoopCount, bVersion, then XMASTREAMFORMAT[n]: PsuedoBytesPerSec(u32), SampleRate(u32),
    # LoopStart(u32), LoopEnd(u32), SubframeData(u8), Channels(u8), ChannelMask(u16).
    fmt = struct.pack("<HHHHHBB", 0x0165, 16, 0, 0, 1, 0, 2)
    fmt += struct.pack("<IIIIBBH", rate * channels * 2, rate, 0, 0, 0, channels,
                       3 if channels == 2 else 4)
    riff = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    riff += b"data" + struct.pack("<I", len(packets)) + packets
    return b"RIFF" + struct.pack("<I", 4 + len(riff)) + b"WAVE" + riff


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ast", nargs="?",
                    default=os.path.join(REPO, "Superman Returns (USA)", "DATA", "sound.AST"))
    ap.add_argument("--out", default=os.path.join(REPO, "archive", "v1_audio", "xma_offline", "disc"))
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--wav-wrap", action="store_true")
    a = ap.parse_args()
    f = open(a.ast, "rb")
    m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    os.makedirs(a.out, exist_ok=True)
    idx = []
    seen = set()
    rbas = [x.start() for x in re.finditer(b"rbas", m)]
    for r in rbas:
        # The SNR header sits inside the record, after the rbas property chunk: search a window.
        for o in range(r + 8, min(r + 0x400, len(m) - 8), 4):
            snr = parse_snr(m, o)
            if not snr or o in seen:
                continue
            pk, blocks, ok, nsub = walk_sns(m, snr["sns"], snr["channels"], snr["num_samples"])
            if not ok or not pk:
                continue
            seen.add(o)
            n = len(idx)
            name = f"s{n:04d}_{o:08X}"
            open(os.path.join(a.out, name + ".xma"), "wb").write(pk)
            if a.wav_wrap:
                open(os.path.join(a.out, name + ".xma1.wav"), "wb").write(
                    xma1_wav(pk, snr["rate"], snr["channels"]))
            snr.update(name=name, packets=len(pk) // 2048, blocks=blocks, rbas=r,
                       packet_file_off=snr["sns"] + 12)
            idx.append(snr)
            break
        if a.limit and len(idx) >= a.limit:
            break
    json.dump({"ast": a.ast, "sounds": idx}, open(os.path.join(a.out, "index.json"), "w"), indent=1)
    tot = sum(s["num_samples"] for s in idx)
    print(f"{len(rbas)} rbas records; carved {len(idx)} EA-XMA sounds "
          f"({sum(s['packets'] for s in idx)} packets, {tot} samples) -> {a.out}")


if __name__ == "__main__":
    main()
