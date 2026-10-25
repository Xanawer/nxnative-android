#!/usr/bin/env python3
"""Compare two frames (docs/android/PLAN.md Phase 2: the Vulkan backend's DM_NVN_SHOT frames against the Metal backend's).

Usage: imgdiff.py A.png B.png [--threshold 8] [--diff out.png] [--json]

Reports whether the sizes match, the mean absolute error per channel (R, G, B, and A when both have alpha), the percentage of pixels whose largest channel difference exceeds the threshold,
and the bounding box of those pixels (x0, y0, x1, y1, inclusive). --diff writes an image of the difference: each pixel's largest channel difference as grey, scaled so the largest is white,
with pixels over the threshold pure red. Exit status: 0 = no pixel over the threshold (sizes equal), 1 = they differ, 2 = an input could not be read.

Frames and diff images are game imagery: keep them under ~/dm-work, never in the repository. Run with the project venv and -I (the inputs are data):
    PATH="$HOME/.venvs/nxnative/bin:$PATH" python3 -I dm/android/tools/imgdiff.py a.png b.png
"""
import argparse
import json
import sys

from PIL import Image, ImageChops


def load(path):
    im = Image.open(path)
    im.load()
    alpha = 'A' in im.getbands()
    return im.convert('RGBA' if alpha else 'RGB'), alpha


def compare(a_path, b_path, threshold=8, diff_path=None):
    a, a_alpha = load(a_path)
    b, b_alpha = load(b_path)
    res = {'a': a_path, 'b': b_path, 'size_a': list(a.size), 'size_b': list(b.size), 'size_match': a.size == b.size, 'threshold': threshold}
    if not res['size_match']:
        return res
    both_alpha = a_alpha and b_alpha
    if not both_alpha:
        a, b = a.convert('RGB'), b.convert('RGB')
    bands = ('R', 'G', 'B', 'A') if both_alpha else ('R', 'G', 'B')
    d = ImageChops.difference(a, b)
    n = a.size[0] * a.size[1]
    chans = d.split()
    res['mae'] = {name: sum(i * c for i, c in enumerate(ch.histogram())) / n for name, ch in zip(bands, chans)}
    mx = chans[0]
    for ch in chans[1:]:
        mx = ImageChops.lighter(mx, ch)   # the largest channel difference of each pixel
    hist = mx.histogram()
    over = sum(hist[threshold + 1:])
    res['pixels'] = n
    res['over'] = over
    res['over_pct'] = 100.0 * over / n
    res['max_diff'] = max((i for i, c in enumerate(hist) if c), default=0)
    mask = mx.point(lambda v: 255 if v > threshold else 0)
    res['bbox'] = list(mask.getbbox()) if over else None
    if res['bbox']:
        res['bbox'][2] -= 1   # PIL's box is exclusive at the right and bottom
        res['bbox'][3] -= 1
    if diff_path:
        top = res['max_diff'] or 1
        grey = mx.point(lambda v: min(255, v * 255 // top))
        low = ImageChops.subtract(grey, mask)   # green and blue: the grey, 0 over the threshold
        out = Image.merge('RGB', (ImageChops.lighter(grey, mask), low, low))   # over the threshold: pure red (255, 0, 0)
        out.save(diff_path)
        res['diff'] = diff_path
    return res


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--threshold', type=int, default=8, help='a pixel differs when its largest channel difference exceeds this (0-255, default 8)')
    ap.add_argument('--diff', help='write a difference image here')
    ap.add_argument('--json', action='store_true', help='print the result as JSON')
    args = ap.parse_args(argv)
    if not 0 <= args.threshold <= 255:
        ap.error('--threshold is 0-255')
    try:
        r = compare(args.a, args.b, args.threshold, args.diff)
    except (OSError, ValueError) as e:
        print(f'imgdiff: {e}', file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(r))
    elif not r['size_match']:
        print(f"{r['a']} vs {r['b']}: size mismatch {r['size_a'][0]}x{r['size_a'][1]} vs {r['size_b'][0]}x{r['size_b'][1]}")
    else:
        mae = ' '.join(f'{k}={v:.3f}' for k, v in r['mae'].items())
        box = 'none' if not r['bbox'] else '{},{} to {},{}'.format(*r['bbox'])
        print(f"{r['a']} vs {r['b']}: size {r['size_a'][0]}x{r['size_a'][1]} match; MAE {mae}; pixels over {r['threshold']}: {r['over']} of {r['pixels']} ({r['over_pct']:.4f}%); "
              f"max difference {r['max_diff']}; differing region {box}" + (f"; diff image {r['diff']}" if r.get('diff') else ''))
    return 0 if r['size_match'] and not r['over'] else 1


if __name__ == '__main__':
    sys.exit(main())
