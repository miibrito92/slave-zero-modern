#!/usr/bin/env python3
"""Build the HD texture pack for szmodern.dll from your own copy of Slave Zero.

    tools/make-texture-pack.py "<game dir>" [--scale 4] [--model realesrgan-x4plus] [--upscaler PATH]

Reads Data/textures.zip, upscales every texture with Real-ESRGAN (ncnn/Vulkan build) and writes
<game dir>/szmodern/textures/<name>.png (lower-case names; animation frames as <name>_f<N>.png).
For the player's mech it also writes the unshaded 256x256 colour maps (from *_bump1) and normal
maps (<name>_n.png) derived from the *_bump2 height maps, which the renderer lights per pixel.
Requires Pillow and NumPy. Generated files are derived from the game's art: keep them local.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

import numpy as np
from PIL import Image

PAD = 8  # wrapped border so tiling textures stay seamless after upscaling


def decode(data):
    """Return (frames: list of HxWx4 uint8 arrays, flags) for a .tex file (mip level 0 only)."""
    w, h, frames, flags = struct.unpack_from("<IIII", data)
    px = data[16:]
    mips = bool(flags & 0x8)
    if flags & 0x200:
        bpp = 4
    elif flags & 0x800:
        bpp = 1
    else:
        bpp = 2
    level0 = w * h * bpp
    if mips:
        stride = sum((w >> i) * (h >> i) for i in range(max(w, h).bit_length())) * bpp
    else:
        stride = level0
    out = []
    for f in range(max(1, frames)):
        raw = px[f * stride:f * stride + level0]
        if len(raw) < level0:
            break
        if bpp == 4:
            a = np.frombuffer(raw, dtype="<u4").reshape(h, w)
            rgba = np.stack([(a >> 16) & 255, (a >> 8) & 255, a & 255, a >> 24], -1).astype(np.uint8)
        elif bpp == 1:
            g = np.frombuffer(raw, dtype=np.uint8).reshape(h, w)
            rgba = np.stack([g, g, g, np.full_like(g, 255)], -1)
        else:
            p = np.frombuffer(raw, dtype="<u2").reshape(h, w).astype(np.uint32)
            if flags & 0x100:  # ARGB4444
                c = [(p >> 8) & 15, (p >> 4) & 15, p & 15, p >> 12]
                rgba = np.stack([(x << 4) | x for x in c], -1).astype(np.uint8)
            elif flags & 0x80:  # ARGB1555
                r, g, b = (p >> 10) & 31, (p >> 5) & 31, p & 31
                a = np.where(p & 0x8000, 255, 0)
                rgba = np.stack([(r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), a], -1).astype(np.uint8)
            else:  # RGB565
                r, g, b = p >> 11, (p >> 5) & 63, p & 31
                rgba = np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2),
                                 np.full_like(r, 255)], -1).astype(np.uint8)
        out.append(rgba)
    return out, flags


def normal_from_height(height, strength):
    """Tangent-space normal map (RGB, +Z out of the surface) from a float height map in [0,1]."""
    hp = np.pad(height, 1, mode="wrap")
    dx = (hp[1:-1, 2:] - hp[1:-1, :-2]) * 0.5
    dy = (hp[2:, 1:-1] - hp[:-2, 1:-1]) * 0.5
    n = np.stack([-dx * strength, -dy * strength, np.ones_like(height)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return ((n * 0.5 + 0.5) * 255.0 + 0.5).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game")
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--model", default="realesrgan-x4plus")
    ap.add_argument("--upscaler", default=os.path.expanduser("~/.local/opt/sz-tools/realesrgan/realesrgan-ncnn-vulkan"))
    ap.add_argument("--bump-strength", type=float, default=6.0)
    args = ap.parse_args()

    zpath = os.path.join(args.game, "Data", "textures.zip")
    out = os.path.join(args.game, "szmodern", "textures")
    os.makedirs(out, exist_ok=True)
    z = zipfile.ZipFile(zpath)
    names = [n for n in z.namelist() if n.lower().endswith(".tex")]
    lower = {n.lower(): n for n in names}

    work = tempfile.mkdtemp(prefix="sz-pack-")
    src, dst = os.path.join(work, "in"), os.path.join(work, "out")
    os.makedirs(src)
    os.makedirs(dst)
    jobs = {}  # job file stem -> (output name, alpha array or None, is1bit, w, h)
    skipped = 0
    for n in names:
        base = n[:-4].lower()
        if base.startswith("i_font") or "_bump" in base:
            skipped += 1
            continue
        frames, flags = decode(z.read(n))
        if not frames or flags & 0x800:
            skipped += 1
            continue
        # the player mech: use the unshaded high-res colour map from the bump set
        bump1 = lower.get(base + "_bump1.tex")
        if bump1:
            frames, flags = decode(z.read(bump1))
        for i, rgba in enumerate(frames):
            outname = base if len(frames) == 1 else f"{base}_f{i}"
            h, w = rgba.shape[:2]
            rgb = np.pad(rgba[..., :3], ((PAD, PAD), (PAD, PAD), (0, 0)), mode="wrap")
            stem = f"j{len(jobs):05d}"
            Image.fromarray(rgb).save(os.path.join(src, stem + ".png"))
            alpha = rgba[..., 3]
            has_alpha = alpha.min() < 255
            jobs[stem] = (outname, alpha if has_alpha else None, bool(flags & 0x80) and not flags & 0x200, w, h)

    print(f"upscaling {len(jobs)} images ({skipped} skipped) with {args.model} x{args.scale}...", flush=True)
    cmd = [args.upscaler, "-i", src, "-o", dst, "-n", args.model, "-s", str(args.scale), "-f", "png"]
    r = subprocess.run(cmd, cwd=os.path.dirname(args.upscaler), stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        sys.exit("upscaler failed: " + r.stderr[-2000:])

    s = args.scale
    for stem, (outname, alpha, onebit, w, h) in jobs.items():
        up = Image.open(os.path.join(dst, stem + ".png")).convert("RGB")
        rgb = np.asarray(up)[PAD * s:(PAD + h) * s, PAD * s:(PAD + w) * s]
        if alpha is not None:
            a = Image.fromarray(alpha).resize((w * s, h * s), Image.BILINEAR if onebit else Image.BICUBIC)
            a = np.asarray(a)
            if onebit:
                a = np.where(a >= 128, 255, 0).astype(np.uint8)
            img = Image.fromarray(np.dstack([rgb, a]), "RGBA")
        else:
            img = Image.fromarray(np.ascontiguousarray(rgb), "RGB")
        img.save(os.path.join(out, outname + ".png"), optimize=False, compress_level=6)

    # normal maps for textures that ship height maps (*_bump2)
    nmaps = 0
    for n in names:
        lo = n.lower()
        if not lo.endswith("_bump2.tex"):
            continue
        base = lo[:-len("_bump2.tex")]
        frames, _ = decode(z.read(n))
        height = frames[0][..., 0].astype(np.float32) / 255.0
        hh, hw = height.shape
        big = np.asarray(Image.fromarray((height * 255).astype(np.uint8)).resize((hw * s, hh * s), Image.BICUBIC))
        nm = normal_from_height(big.astype(np.float32) / 255.0, args.bump_strength * s)
        Image.fromarray(nm, "RGB").save(os.path.join(out, base + "_n.png"))
        nmaps += 1

    with open(os.path.join(args.game, "szmodern", "pack.txt"), "w") as f:
        f.write(f"model={args.model}\nscale={args.scale}\ntextures={len(jobs)}\nnormalmaps={nmaps}\n")
    shutil.rmtree(work, ignore_errors=True)
    print(f"wrote {len(jobs)} textures and {nmaps} normal maps to {out}")


if __name__ == "__main__":
    main()
