#!/usr/bin/env python3
"""
frost-convert.py

Converts every .mp4 and .mkv in the folder this script sits in into an AVI that
the frost firmware plays: MJPEG video scaled to 240x135 with letterbox/pillarbox
bars, MP3 audio, and title | artist/director | album/studio written into the AVI INFO list so frost
shows clean names.

Requires ffmpeg and ffprobe on PATH.

running:
python3 frost-convert.py
"""

import os
import sys
import subprocess
import shutil

W, H = 240, 135
OUT_DIR = "frost-out"
SRC_EXT = (".mp4", ".mkv")


def need(tool):
    if shutil.which(tool) is None:
        sys.exit(f"'{tool}' not found on PATH. Install ffmpeg (it ships ffprobe too).")


def ask(prompt, default=""):
    d = f" [{default}]" if default else ""
    ans = input(f"{prompt}{d}: ").strip()
    return ans if ans else default


def ask_int(prompt, default, allow_zero=False):
    lowest = 0 if allow_zero else 1
    while True:
        ans = input(f"{prompt} [{default}]: ").strip()
        if not ans:
            return default
        if ans.isdigit() and int(ans) >= lowest:
            return int(ans)
        print(f"  enter a whole number, {lowest} or more")


def has_audio(path):
    r = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "a",
         "-show_entries", "stream=index", "-of", "csv=p=0", path],
        capture_output=True, text=True)
    return bool(r.stdout.strip())


def convert(src, out, fps, quality, cap_kb, title, artist, album):
    vf = (f"scale={W}:{H}:force_original_aspect_ratio=decrease,format=rgb24,"
          f"pad={W}:{H}:(ow-iw)/2:(oh-ih)/2:color=black,setsar=1")
    cmd = ["ffmpeg", "-y", "-i", src, "-map", "0:v:0"]
    audio = has_audio(src)
    if audio:
        cmd += ["-map", "0:a:0"]
    cmd += ["-sn", "-dn",
            "-vf", vf,
            "-r", str(fps), "-fps_mode", "cfr",
            "-c:v", "mjpeg", "-pix_fmt", "yuvj420p", "-huffman", "optimal"]
    if cap_kb > 0:
        frame_bits = cap_kb * 1000 * 8
        peak = frame_bits * fps
        cmd += ["-b:v", str(peak * 3 // 4), "-maxrate", str(peak), "-bufsize", str(frame_bits),
                "-qmin", str(quality), "-qmax", "31"]
    else:
        cmd += ["-q:v", str(quality)]
    if audio:
        cmd += ["-c:a", "libmp3lame", "-b:a", "96k", "-ar", "44100", "-ac", "2"]
    if title:
        cmd += ["-metadata", f"title={title}"]
    if artist:
        cmd += ["-metadata", f"artist={artist}"]
    if album:
        cmd += ["-metadata", f"album={album}"]
    cmd += [out]

    r = subprocess.run(cmd)
    return r.returncode == 0, audio


def main():
    need("ffmpeg")
    need("ffprobe")

    here = os.path.dirname(os.path.abspath(__file__))
    sources = sorted(
        f for f in os.listdir(here)
        if f.lower().endswith(SRC_EXT) and os.path.isfile(os.path.join(here, f))
    )
    if not sources:
        sys.exit("No .mp4 or .mkv files found next to this script.")

    print(f"Found {len(sources)} file(s):")
    for f in sources:
        print(f"  {f}")
    print()

    fps = ask_int("Frame rate", 20)
    quality = ask_int("JPEG quality (2 best/large .. 8 small/blocky)", 6)
    cap_kb = ask_int("Max frame size in KB, heavy scenes compress harder (0 = no cap)", 8, allow_zero=True)
    same_meta = ask("Apply one artist/album to all files? (y/N)", "N").lower().startswith("y")

    common_artist = common_album = ""
    if same_meta:
        common_artist = ask("Artist / studio for all")
        common_album = ask("Album / series for all (blank to skip)")

    outdir = os.path.join(here, OUT_DIR)
    os.makedirs(outdir, exist_ok=True)

    ok_count = 0
    for f in sources:
        src = os.path.join(here, f)
        stem = os.path.splitext(f)[0]
        print("\n" + "=" * 50)
        print(f"  {f}")
        print("=" * 50)

        title = ask("Title", stem)
        if same_meta:
            artist, album = common_artist, common_album
        else:
            artist = ask("Artist / studio")
            album = ask("Album / series (blank to skip)")

        out = os.path.join(outdir, stem + ".avi")
        ok, had_audio = convert(src, out, fps, quality, cap_kb, title, artist, album)
        if ok:
            ok_count += 1
            note = "" if had_audio else "  (no audio track found; video only)"
            print(f"  -> {os.path.relpath(out, here)}{note}")
        else:
            print(f"  FAILED: {f}")

    print("\n" + "=" * 50)
    print(f"Done: {ok_count}/{len(sources)} converted into '{OUT_DIR}/'.")
    print(f"Copy that folder's .avi files to your SD card's movie directory (default /Movie).")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nCancelled.")
