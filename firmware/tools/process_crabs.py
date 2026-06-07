#!/usr/bin/env python3
"""Turn the Gemini crab PNGs into transparent square sprites for the round panel.

- removes the flat navy background via a connected-component flood from the image
  borders (so the crab's *interior* dark outlines/eyes survive — only background
  navy that touches an edge is cleared)
- crops to the crab's bounding box (which also drops the corner watermark), pads
  to a square, resizes to SIZE for the device sprite
"""
import os
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

MAPPING = {
    "idle":      "Gemini_Generated_Image_9mhyo19mhyo19mhy.png",  # sleeping, zzz
    "listening": "Gemini_Generated_Image_ljmj8sljmj8sljmj.png",  # calm, attentive
    "thinking":  "Gemini_Generated_Image_pt1odfpt1odfpt1o.png",  # thought bubble
    "speaking":  "Gemini_Generated_Image_h24npvh24npvh24n.png",  # excited, confetti
    "error":     "Gemini_Generated_Image_1yd8pp1yd8pp1yd8.png",  # X eyes, warning
}
SIZE = 280       # output sprite, px (sits centered on the 360 colour ring)
PAD = 0.08       # padding fraction around the content bbox
MIN_FRAC = 0.03  # drop components smaller than this fraction of the crab (confetti/sparkles)
TOL = 60         # navy-background colour match tolerance (Euclidean in RGB)


def remove_bg(im, tol=TOL):
    rgb = np.asarray(im.convert("RGB")).astype(np.int16)
    h, w, _ = rgb.shape
    corners = np.concatenate([
        rgb[0:24, 0:24].reshape(-1, 3), rgb[0:24, w - 24:w].reshape(-1, 3),
        rgb[h - 24:h, 0:24].reshape(-1, 3), rgb[h - 24:h, w - 24:w].reshape(-1, 3)])
    bg = np.median(corners, axis=0)
    dist = np.sqrt(((rgb - bg) ** 2).sum(axis=2))
    is_bg = dist < tol
    labeled, _ = ndimage.label(is_bg)
    border = (set(labeled[0, :]) | set(labeled[-1, :]) |
              set(labeled[:, 0]) | set(labeled[:, -1]))
    border.discard(0)
    bg_mask = np.isin(labeled, list(border))
    out = np.asarray(im.convert("RGBA")).copy()
    out[..., 3] = np.where(bg_mask, 0, out[..., 3]).astype(np.uint8)
    return Image.fromarray(out, "RGBA"), tuple(int(c) for c in bg)


def isolate_and_crop(im, drop_stray=True):
    """Square-crop the crab, centered, with a little padding. With drop_stray
    (auto-removed sources) tiny components are erased first — confetti/sparkles —
    then the frame is taken over what remains. For pre-cut art we keep everything
    (the crab may be many disconnected pieces: outline-less body + legs + zzz),
    so nothing gets dropped or clipped."""
    arr = np.asarray(im).copy()
    mask = arr[..., 3] > 16
    labeled, n = ndimage.label(mask)
    if n == 0:
        return im
    sizes = ndimage.sum(mask, labeled, range(1, n + 1))
    if drop_stray:
        crab_area = sizes[int(np.argmax(sizes))]
        for lbl in range(1, n + 1):
            if sizes[lbl - 1] < MIN_FRAC * crab_area:
                arr[..., 3][labeled == lbl] = 0  # erase confetti/sparkles
    ys, xs = np.where(arr[..., 3] > 16)  # frame over all remaining content
    cx, cy = (xs.min() + xs.max()) / 2, (ys.min() + ys.max()) / 2
    half = max(xs.max() - xs.min(), ys.max() - ys.min()) / 2 * (1 + PAD)
    box = (round(cx - half), round(cy - half), round(cx + half), round(cy + half))
    return Image.fromarray(arr, "RGBA").crop(box)  # PIL pads OOB transparent


def already_transparent(im):
    """True if the image arrives pre-cut (has a real transparent region), so we
    leave its background alone and only reframe."""
    if im.mode != "RGBA":
        return False
    a = np.asarray(im)[..., 3]
    return float((a < 16).mean()) > 0.02


# Hand-cut PNGs may be named by mood rather than by state.
CUT_ALIASES = {"idle": ["idle", "sleeping"], "speaking": ["speaking", "happy"]}


def source_for(state, indir):
    """Prefer a hand-cut PNG (by state name or a mood alias) in indir; else fall
    back to the original Gemini export named in MAPPING."""
    for name in CUT_ALIASES.get(state, [state]):
        p = os.path.join(indir, name + ".png")
        if os.path.exists(p):
            return p
    return os.path.join(indir, MAPPING[state])


def main():
    indir, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    for state in MAPPING:
        im = Image.open(source_for(state, indir)).convert("RGBA")
        pre_cut = already_transparent(im)
        if pre_cut:
            note = "pre-cut"          # hand-removed background: leave it alone
        else:
            im, bg = remove_bg(im)
            note = f"bg={bg}"
        sq = isolate_and_crop(im, drop_stray=not pre_cut).resize((SIZE, SIZE), Image.LANCZOS)
        sq.save(os.path.join(outdir, state + ".png"))
        print(f"{state:10s} {note} -> {state}.png  {sq.size}")


if __name__ == "__main__":
    main()
