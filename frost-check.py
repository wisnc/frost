#!/usr/bin/env python3
"""
frost-check.py

Walks an AVI the same way frost's firmware does and reports where playback
would stop, what frost would skip, and whether the file is split into
multiple RIFF segments (files over ~1 GB).

  python3 frost-check.py movie.avi [more.avi ...]
  python3 frost-check.py --compare pc_copy.avi E:/Movie/sd_copy.avi
"""

import os
import struct
import sys

VBUF_SIZE = 40960
AUDIO_RING = 8192


def fcc(b):
    return b.decode("latin1") if all(32 <= c < 127 for c in b) else b.hex()


def u32(b, o=0):
    return struct.unpack_from("<I", b, o)[0]


def check(path):
    size = os.path.getsize(path)
    print(f"\n== {path}")
    print(f"   file size        {size:,} bytes")
    with open(path, "rb") as f:
        head = f.read(12)
        if len(head) < 12 or head[:4] != b"RIFF" or head[8:12] != b"AVI ":
            print("   NOT an AVI (no RIFF 'AVI ' header). frost shows: x parse failed")
            return

        # top-level RIFF segments
        segs, pos = [], 0
        while pos + 12 <= size:
            f.seek(pos)
            h = f.read(12)
            if h[:4] != b"RIFF":
                break
            ssz = u32(h, 4)
            segs.append((pos, ssz, fcc(h[8:12])))
            if ssz == 0xFFFFFFFF or ssz == 0:
                break
            pos += 8 + ssz + (ssz & 1)
        first_end = segs[0][0] + 8 + segs[0][1]
        print(f"   RIFF segments    {len(segs)}  ({', '.join(t for _, _, t in segs)})")
        if len(segs) > 1:
            print("   !! file continues in AVIX segments; frost only plays the first segment")
        if first_end > size:
            print(f"   !! first RIFF claims {first_end:,} bytes but the file has {size:,}: file is truncated")

        # header walk, same fields frost reads
        info = dict(movi=None, fps=None, frames=None, streams=[], atag=None, tags={})

        def walk(p, end, depth, last_type=[None]):
            while p + 8 <= end:
                f.seek(p)
                h = f.read(8)
                if len(h) < 8:
                    return
                cid, csz = h[:4], u32(h, 4)
                payload, nxt = p + 8, p + 8 + csz + (csz & 1)
                if nxt <= p:
                    return
                if cid == b"LIST":
                    t = f.read(4)
                    if t == b"movi":
                        if info["movi"] is None:
                            info["movi"] = (payload + 4, payload + csz)
                    elif t in (b"hdrl", b"strl", b"INFO") and depth < 4:
                        walk(payload + 4, payload + csz, depth + 1)
                elif cid == b"avih" and csz >= 20:
                    info["frames"] = u32(f.read(20), 16)
                elif cid == b"strh" and csz >= 28:
                    b = f.read(28)
                    typ = b[:4]
                    info["streams"].append(fcc(typ))
                    last_type[0] = typ
                    if typ == b"vids" and u32(b, 24):
                        info["fps"] = (u32(b, 24), u32(b, 20) or 1)
                elif cid == b"strf" and last_type[0] == b"auds" and csz >= 2:
                    info["atag"] = struct.unpack("<H", f.read(2))[0]
                    last_type[0] = None
                elif cid in (b"INAM", b"IART", b"IPRD", b"ISFT"):
                    info["tags"][fcc(cid)] = f.read(min(csz, 120)).split(b"\0")[0].decode("utf-8", "replace")
                p = nxt

        walk(12, min(first_end, size), 0)

        for k, label in (("INAM", "title"), ("IART", "artist"), ("IPRD", "album"), ("ISFT", "written by")):
            if k in info["tags"]:
                print(f"   {label:<16} {info['tags'][k]}")
        if not info["movi"]:
            print("   !! no LIST 'movi' found. frost shows: x parse failed")
            return
        ms, me = info["movi"]
        rate, scale = info["fps"] or (20, 1)
        vid_i = info["streams"].index("vids") if "vids" in info["streams"] else -1
        aud_i = info["streams"].index("auds") if "auds" in info["streams"] else -1
        print(f"   streams          {info['streams']}   fps {rate}/{scale}   header frames {info['frames']}")
        tag = info["atag"]
        print(f"   audio format     {hex(tag) if tag is not None else 'none'}"
              + ("" if tag == 0x55 else "   !! frost needs 0x55 (MP3). frost shows: x audio not mp3"))
        print(f"   movi range       {ms:,} .. {me:,}  ({(me - ms) * 100 / size:.1f}% of file)")
        if me > size:
            print(f"   !! movi claims to end at {me:,} but the file is only {size:,} bytes")
        if vid_i < 0 or aud_i < 0:
            print("   !! needs both a video and an audio stream")
            return
        vid, aud = f"{vid_i:02d}dc".encode(), f"{aud_i:02d}wb".encode()

        # chunk walk, exactly like the reader task
        p, frames, achunks, vbig, abig, vmax, amax = ms, 0, 0, 0, 0, 0, 0
        other = {}
        stop = None
        end = min(me, size)
        while True:
            if p + 8 > end:
                stop = "end of movi (normal end)" if end == me else "end of file before movi end"
                break
            f.seek(p)
            h = f.read(8)
            if len(h) != 8:
                stop = "read failed"
                break
            cid, csz = h[:4], u32(h, 4)
            if cid == b"LIST":
                p += 12
                continue
            if cid == vid:
                frames += 1
                vmax = max(vmax, csz)
                if csz > VBUF_SIZE:
                    vbig += 1
            elif cid == aud:
                achunks += 1
                amax = max(amax, csz)
                if csz > AUDIO_RING:
                    abig += 1
            else:
                k = fcc(cid)
                other[k] = other.get(k, 0) + 1
                if not (k.startswith("ix") or k in ("JUNK", "idx1")):
                    stop = f"unexpected chunk id '{k}' size {csz:,}"
                    if cid == b"\0\0\0\0":
                        stop = "zero-filled data (damaged or unwritten region)"
                    break
            p += 8 + csz + (csz & 1)

        secs = frames * scale / rate
        print(f"   walk stopped     at byte {p:,} ({p * 100 / size:.1f}% of file): {stop}")
        print(f"   playable         {frames:,} frames = {int(secs // 3600)}h{int(secs % 3600 // 60):02d}m{int(secs % 60):02d}s"
              + (f"   (header says {info['frames']:,})" if info["frames"] else ""))
        print(f"   largest chunks   video {vmax:,} B   audio {amax:,} B")
        if vbig:
            print(f"   !! {vbig} frames over {VBUF_SIZE:,} B: frost skips them (encode with a frame cap)")
        if abig:
            print(f"   !! {abig} audio chunks over {AUDIO_RING:,} B: frost skips them (audio gaps)")
        if other:
            print(f"   other chunks     {other}")
        if stop and not stop.startswith("end of movi"):
            print("   => frost will stop here. Compare against the original file with --compare.")
        elif len(segs) > 1:
            print("   => frost stops at the end of the first segment; keep files under 1 GB")
        else:
            print("   => structure OK for frost")


def compare(a, b):
    sa, sb = os.path.getsize(a), os.path.getsize(b)
    print(f"\ncomparing\n  A {a} ({sa:,} B)\n  B {b} ({sb:,} B)")
    block = 1 << 16
    regions, cur, off, zero_bytes, diff_bytes = [], None, 0, 0, 0
    with open(a, "rb") as fa, open(b, "rb") as fb:
        while True:
            x, y = fa.read(block), fb.read(block)
            if not x and not y:
                break
            n = min(len(x), len(y))
            if x[:n] != y[:n]:
                first = next(k for k in range(n) if x[k] != y[k])
                last = next(k for k in range(n - 1, -1, -1) if x[k] != y[k])
                s0, s1 = off + first, off + last
                chunk = y[first:last + 1]
                zero_bytes += chunk.count(0)
                diff_bytes += len(chunk)
                if cur and s0 - cur[1] <= block:
                    cur[1] = s1
                else:
                    if cur:
                        regions.append(cur)
                    cur = [s0, s1]
            off += max(len(x), len(y))
            if len(x) != len(y):
                break
        if cur:
            regions.append(cur)
    if sa != sb:
        print(f"  !! sizes differ by {abs(sa - sb):,} bytes ({'B is shorter' if sb < sa else 'B is longer'})")
    if not regions:
        print("  identical" if sa == sb else "  identical up to the shorter length")
        return
    total = sum(r[1] - r[0] + 1 for r in regions)
    print(f"  {len(regions)} damaged region(s), {total:,} bytes in total:")
    for r0, r1 in regions[:12]:
        print(f"    {r0:>13,} .. {r1:>13,}  ({r0 * 100 / sa:5.1f}% .. {r1 * 100 / sa:5.1f}%)  {r1 - r0 + 1:>12,} B"
              f"  starts at {r0 / 1048576:.2f} MiB")
    if len(regions) > 12:
        print(f"    ... {len(regions) - 12} more")
    zero_share = zero_bytes / max(diff_bytes, 1)
    runs_to_end = regions[-1][1] >= min(sa, sb) - block
    if len(regions) == 1 and runs_to_end:
        print("  => everything after one point is wrong: fake-capacity card, or a copy that never finished")
    else:
        print("  => isolated damaged patches: bad sectors or a filesystem fault on the card")
    if zero_share > 0.9:
        print("     damaged bytes are mostly zeros: never written (interrupted copy or card removed early)")
    else:
        print("     damaged bytes are other data: wrapped or cross-linked storage, typical of a fake or failing card")


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    if args[0] == "--compare":
        if len(args) != 3:
            sys.exit("usage: frost-check.py --compare original.avi sdcard_copy.avi")
        compare(args[1], args[2])
    else:
        for p in args:
            check(p)
