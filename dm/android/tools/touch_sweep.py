#!/usr/bin/env python3
# Offline sweep of TouchPad.java's layout: layout()'s geometry (its constants, insets and bounds on u), the hit regions (lockedAt, the diamond's and the stick's grab circles in begin(), faceAt)
# and the visible shapes (onDraw: teardrops, pills, key circles, the stick's ring with the knob anywhere on it, every outline stroked 1.5 dp). For each window size and inset combination it checks:
#   (a) no two controls' hit regions meet (ZL L ZR R − + as teardrop circles and inflated pills, the diamond's and the stick's grab circles);
#   (b) no control's visible shape meets another control's hit region (and no diamond key's another key's hit circle);
#   (c) begin()'s resolver: each control's visible centre resolves to that control (a diamond key to that key), and no point on a visible outline resolves to another control.
# Modes:  derive  - u with no bounds (no h/430, no cap, no width or height bound), bisected to the largest u that passes, on a coarse grid: the room each binding pair needs, in u
#         verify  - u as layout() computes it (the constants below), the full grid: every shown layout must pass, and still pass at 1.09 x u (the margin)
#         one W H D cl ct cr cb bl bt br bb - one window in px (density D, cutout and bar insets in px): u and the checks
import math, sys, itertools, multiprocessing as mp

# TouchPad.java's constants (in u unless said)
K_ZL_X, K_ZL_Y, K_ZL_R, K_L_X, K_L_Y, K_L_R, K_DROP_TIP, K_CORNER_IN = 44, 40, 26, 44, 108, 24, 1.45, 0.5
K_FACE_X, K_PAD_Y, K_FACE_GAP, K_FACE_R, K_PILL_DX, K_PILL_Y, K_PILL_W, K_PILL_H = 81, 82, 50, 27, 52, 22, 50, 28
K_STICK_X, K_STICK_R, K_KNOB_R, K_STICK_GRAB = 81, 77, 30, 1.4
DROP_HIT, PILL_INFL, FACE_HIT, FACE_GRAB = 1.35, 8, 1.25, 1.6   # lockedAt, faceAt, begin()
K = dict(corners=400, half=180, safe_h=365, u_min_dp=0.6)         # the bounds (layout()); derive mode finds what they must exceed
LOCKED = ['ZL', 'L', 'ZR', 'R', '-', '+']                          # lockedAt's order: the first hit wins
FACE = ['A', 'B', 'X', 'Y']

def spans(w, h, cut, bars):
    l, t, r, b = cut; bl, bt, br, bb = bars
    sl, st, sr, sb = max(l, bl), max(t, bt), w - max(r, br), h - max(b, bb)
    cl, crt = max(l * K_CORNER_IN, bl), w - max(r * K_CORNER_IN, br)
    return sl, st, sr, sb, cl, crt, w / 2

def u_of(w, h, d, cut, bars, k=K):   # layout()'s u; 0 when hidden (below the minimum)
    sl, st, sr, sb, cl, crt, mid = spans(w, h, cut, bars)
    u = max(0.0, min(h / 430, 1.3 * d, (crt - cl) / k['corners'], min(sr - mid, mid - sl) / k['half'], (sb - st) / k['safe_h']))
    return 0.0 if u < k['u_min_dp'] * d else u

def layout(w, h, d, cut, bars, u):
    sl, st, sr, sb, cl, crt, mid = spans(w, h, cut, bars)
    g = dict(u=u, s=0.75 * d)   # s: half the 1.5 dp stroke
    gap, keyR = K_FACE_GAP * u, K_FACE_R * u
    fx, fy = crt - K_FACE_X * u, sb - K_PAD_Y * u
    g['face'] = (fx, fy, gap + keyR * FACE_GRAB); g['keyR'] = keyR
    g['A'], g['B'], g['X'], g['Y'] = (fx + gap, fy), (fx, fy + gap), (fx, fy - gap), (fx - gap, fy)
    g['stick'] = (cl + K_STICK_X * u, sb - K_PAD_Y * u, K_STICK_R * u * K_STICK_GRAB); g['stickR'], g['knobR'] = K_STICK_R * u, K_KNOB_R * u
    g['ZL'] = (sl + K_ZL_X * u, st + K_ZL_Y * u, K_ZL_R * u, -3 * math.pi / 4); g['L'] = (sl + K_L_X * u, st + K_L_Y * u, K_L_R * u, -3 * math.pi / 4)
    g['ZR'] = (sr - K_ZL_X * u, st + K_ZL_Y * u, K_ZL_R * u, -math.pi / 4); g['R'] = (sr - K_L_X * u, st + K_L_Y * u, K_L_R * u, -math.pi / 4)
    for k, sx in (('-', -1), ('+', 1)):
        x, y, pw, ph = mid + sx * K_PILL_DX * u, st + K_PILL_Y * u, K_PILL_W * u / 2, K_PILL_H * u / 2
        g[k] = (x - pw, y - ph, x + pw, y + ph)
    return g

# ---- Shapes: ('c', x, y, r) disks, ('r', x0, y0, x1, y1) rectangles, ('t', p0, p1, p2) triangles; a visible shape may be a union ----
def hit(g, k):
    u = g['u']
    if k in ('ZL', 'L', 'ZR', 'R'): x, y, r, _ = g[k]; return [('c', x, y, r * DROP_HIT)]
    if k in ('-', '+'): x0, y0, x1, y1 = g[k]; i = PILL_INFL * u; return [('r', x0 - i, y0 - i, x1 + i, y1 + i)]
    if k == 'diamond': return [('c',) + g['face']]
    if k == 'stick': return [('c',) + g['stick']]
    if k in FACE: x, y = g[k]; return [('c', x, y, g['keyR'] * FACE_HIT)]
def visible(g, k):
    s = g['s']
    if k in ('ZL', 'L', 'ZR', 'R'):   # the hull of the circle and the tip (its rounding only takes away), stroked: the tip moves out by s / sin(half the tip's angle)
        x, y, r, tip = g[k]; R = r + s; D = r * K_DROP_TIP; D2 = D + s * D / r; th = math.acos(R / D2) if D2 > R else 0
        T = (x + D2 * math.cos(tip), y + D2 * math.sin(tip)); P1 = (x + R * math.cos(tip + th), y + R * math.sin(tip + th)); P2 = (x + R * math.cos(tip - th), y + R * math.sin(tip - th))
        return [('c', x, y, R), ('t', T, P1, P2)]
    if k in ('-', '+'): x0, y0, x1, y1 = g[k]; return [('r', x0 - s, y0 - s, x1 + s, y1 + s)]
    if k in FACE: x, y = g[k]; return [('c', x, y, g['keyR'] + s)]
    if k == 'diamond': return [v for f in FACE for v in visible(g, f)]
    if k == 'stick': x, y, _ = g['stick']; return [('c', x, y, g['stickR'] + g['knobR'] + s)]   # the knob reaches the ring's edge

def seg_dist(p, a, b):
    ax, ay = a; bx, by = b; px, py = p; dx, dy = bx - ax, by - ay; L = dx * dx + dy * dy
    t = 0 if L == 0 else max(0, min(1, ((px - ax) * dx + (py - ay) * dy) / L))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)
def in_tri(p, a, b, c):
    def cr(o, q, r): return (q[0] - o[0]) * (r[1] - o[1]) - (q[1] - o[1]) * (r[0] - o[0])
    d1, d2, d3 = cr(a, b, p), cr(b, c, p), cr(c, a, p)
    return not ((d1 < 0 or d2 < 0 or d3 < 0) and (d1 > 0 or d2 > 0 or d3 > 0))
def poly(sh):
    if sh[0] == 'r': _, x0, y0, x1, y1 = sh; return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
    return list(sh[1:])
def sat(P, Q):   # convex polygons intersect (separating axis theorem)
    for A in (P, Q):
        for i in range(len(A)):
            (x0, y0), (x1, y1) = A[i], A[(i + 1) % len(A)]; nx, ny = y0 - y1, x1 - x0
            a = [nx * x + ny * y for x, y in P]; b = [nx * x + ny * y for x, y in Q]
            if max(a) < min(b) or max(b) < min(a): return False
    return True
def meet(a, b):   # two shapes share a point (touching counts)
    if a[0] == 'c' and b[0] == 'c': return math.hypot(a[1] - b[1], a[2] - b[2]) <= a[3] + b[3]
    if a[0] != 'c' and b[0] == 'c': a, b = b, a
    if a[0] == 'c' and b[0] == 'r':
        _, x, y, r = a; _, x0, y0, x1, y1 = b; return math.hypot(x - max(x0, min(x, x1)), y - max(y0, min(y, y1))) <= r
    if a[0] == 'c' and b[0] == 't':
        _, x, y, r = a; p, q, s = b[1:]; return in_tri((x, y), p, q, s) or min(seg_dist((x, y), p, q), seg_dist((x, y), q, s), seg_dist((x, y), s, p)) <= r
    return sat(poly(a), poly(b))
def meets(A, B): return any(meet(a, b) for a in A for b in B)

def inside(sh, x, y):
    if sh[0] == 'c': return math.hypot(x - sh[1], y - sh[2]) <= sh[3]
    _, x0, y0, x1, y1 = sh; return x0 <= x <= x1 and y0 <= y <= y1
def resolve(g, x, y):   # begin(): a locked key, else the diamond or the (free) stick by the smaller distance / grab radius, else the game (None)
    for k in LOCKED:
        if inside(hit(g, k)[0], x, y): return k
    best, kind = 1.0, None
    fx, fy, fr = g['face']; fd = math.hypot(x - fx, y - fy) / fr
    if fd <= best: best, kind = fd, 'diamond'
    sx, sy, sr = g['stick']
    if math.hypot(x - sx, y - sy) / sr < best: kind = 'stick'
    if kind == 'diamond':   # faceAt: the keys under the finger
        m = [f for f in FACE if math.hypot(x - g[f][0], y - g[f][1]) <= g['keyR'] * FACE_HIT]
        return 'diamond:' + ''.join(m)
    return kind
def outline(sh, n=32):
    if sh[0] == 'c': _, x, y, r = sh; return [(x + r * math.cos(2 * math.pi * i / n), y + r * math.sin(2 * math.pi * i / n)) for i in range(n)]
    P = poly(sh); out = []
    for i in range(len(P)):
        (x0, y0), (x1, y1) = P[i], P[(i + 1) % len(P)]; out += [(x0 + (x1 - x0) * j / 8, y0 + (y1 - y0) * j / 8) for j in range(8)]
    return out

CONTROLS = LOCKED + ['diamond', 'stick']
def check(g):   # the failed checks, as names
    bad = []
    H = {k: hit(g, k) for k in CONTROLS}; V = {k: visible(g, k) for k in CONTROLS}
    for a, b in itertools.combinations(CONTROLS, 2):
        if meets(H[a], H[b]): bad.append(f'a:hit {a} x hit {b}')
    for a in CONTROLS:
        for b in CONTROLS:
            if a != b and meets(V[a], H[b]): bad.append(f'b:shape {a} in hit {b}')
    for a in FACE:
        for b in FACE:
            if a != b and meets(visible(g, a), hit(g, b)): bad.append(f'b:shape {a} in hit {b}')
    own = {k: k for k in LOCKED}; own.update({'stick': 'stick'}); own.update({f: 'diamond:' + f for f in FACE})
    centre = {k: (g[k][0], g[k][1]) for k in ('ZL', 'L', 'ZR', 'R')}
    centre.update({k: ((g[k][0] + g[k][2]) / 2, (g[k][1] + g[k][3]) / 2) for k in ('-', '+')}); centre['stick'] = g['stick'][:2]; centre.update({f: g[f] for f in FACE})
    for k, (x, y) in centre.items():
        if resolve(g, x, y) != own[k]: bad.append(f'c:centre of {k} -> {resolve(g, x, y)}')
    for k in LOCKED + ['stick'] + FACE:
        ctl = 'diamond' if k in FACE else k
        for sh in visible(g, k):
            for x, y in outline(sh):
                r = resolve(g, x, y)
                ok = r is None or r == ctl or (ctl == 'diamond' and r in ('diamond:', own[k]))
                if not ok: bad.append(f'c:outline of {k} -> {r}'); break
    return sorted(set(bad))

# ---- The sweep's windows: dp sizes (both orientations), insets in dp: a cutout either side, a navigation bar either side or at the bottom (3-button 48, gesture 15), a status bar or none ----
CUTS = [(0, 0, 0, 0), (39, 0, 0, 0), (0, 0, 39, 0), (80, 0, 0, 0), (0, 0, 80, 0)]
NAVS = [(0, 0, 0, 0), (48, 0, 0, 0), (0, 0, 48, 0), (0, 0, 0, 48), (0, 0, 0, 15)]
TOPS = [0, 30]
def insets():
    for c, n, t in itertools.product(CUTS, NAVS, TOPS): yield c, (n[0], max(n[1], t), n[2], n[3])
def px(v, d): return tuple(int(round(x * d)) for x in v)

def verify_row(args):
    wdp, d, hs = args; out = []
    for hdp in hs:
        for c, b in insets():
            w, h, cut, bars = int(round(wdp * d)), int(round(hdp * d)), px(c, d), px(b, d)
            u = u_of(w, h, d, cut, bars)
            if u == 0: out.append(('hidden', wdp, hdp, d, c, b, None)); continue
            bad = check(layout(w, h, d, cut, bars, u)); bad2 = [] if bad else check(layout(w, h, d, cut, bars, u * 1.09))
            out.append(('ok' if not bad and not bad2 else 'FAIL', wdp, hdp, d, c, b, (u / d, bad or ['at 1.09 u: ' + x for x in bad2])))
    return out

def umax(w, h, d, cut, bars):   # the largest u that passes, by bisection (no bound on u); and what fails just above it
    lo, hi = K['u_min_dp'] * d, float(max(w, h))   # from the minimum: below it the fixed 1.5 dp stroke, not u, sets the gaps
    if check(layout(w, h, d, cut, bars, lo)): return 0.0, ['fails at the minimum u']
    for _ in range(40):
        m = (lo + hi) / 2
        if check(layout(w, h, d, cut, bars, m)): hi = m
        else: lo = m
    return lo, check(layout(w, h, d, cut, bars, hi))
def derive_row(args):
    wdp, d, hs = args; out = []
    for hdp in hs:
        for c, b in insets():
            w, h, cut, bars = int(round(wdp * d)), int(round(hdp * d)), px(c, d), px(b, d)
            sl, st, sr, sb, cl, crt, mid = spans(w, h, cut, bars)
            if min(crt - cl, sr - mid, mid - sl, sb - st) <= 0: continue
            um, why = umax(w, h, d, cut, bars)
            if um > 0: out.append((wdp, hdp, c, b, um / d, (crt - cl) / um, min(sr - mid, mid - sl) / um, (sb - st) / um, why))
    return out

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else 'verify'
    if mode == 'one':
        w, h = int(sys.argv[2]), int(sys.argv[3]); d = float(sys.argv[4]); v = [int(x) for x in sys.argv[5:13]]; cut, bars = tuple(v[:4]), tuple(v[4:])
        sl, st, sr, sb, cl, crt, mid = spans(w, h, cut, bars)
        print(f'{w}x{h} px at {d}: bounds h/430 {h / 430:.3f}, cap {1.3 * d:.3f}, corners {(crt - cl) / K["corners"]:.3f}, top row {min(sr - mid, mid - sl) / K["half"]:.3f}, '
              f'height {(sb - st) / K["safe_h"]:.3f}, minimum {K["u_min_dp"] * d:.3f} px')
        u = u_of(w, h, d, cut, bars)
        print(f'u {u:.4f} px = {u / d:.4f} dp' + (' (hidden)' if u == 0 else '') + ('' if u == 0 else f'; checks: {check(layout(w, h, d, cut, bars, u)) or "all pass"}'))
        return
    pool = mp.Pool()
    if mode == 'derive':   # a coarse grid at one density: the geometry scales with u; only the stroke (1.5 dp) does not
        rows = [(wdp, 2.625, list(range(100, 1101, 40))) for wdp in range(100, 1501, 40)]
        res = [r for rr in pool.map(derive_row, rows) for r in rr]
        print(f'derive: {len(res)} layouts that pass at the minimum u (u with no bounds, bisected upward from it)')
        by = {}
        for wdp, hdp, c, b, udp, rc, rh, rv, why in res:
            key = why[0] if why else '?'
            e = by.setdefault(key, [0, 0, 0, 0, None]); e[0] += 1
            for i, r in ((1, rc), (2, rh), (3, rv)):
                if r > e[i]: e[i] = r
            if e[4] is None: e[4] = (wdp, hdp, c, b)
        print('the first failure just above the largest u, with the most room each span needed when it was the limit (span / u):')
        for k, (n, rc, rh, rv, ex) in sorted(by.items(), key=lambda kv: -kv[1][0]):
            print(f'  {n:6d} x {k:40s} corners {rc:7.1f}  top half {rh:7.1f}  safe height {rv:7.1f}   e.g. {ex}')
        return
    rows = [(wdp, 2.625, list(range(100, 1101, 20))) for wdp in range(100, 1501, 20)]
    rows += [(wdp, d, list(range(100, 1101, 40))) for d in (1.0, 4.0) for wdp in range(100, 1501, 40)]
    res = [r for rr in pool.map(verify_row, rows) for r in rr]
    n = len(res); hid = sum(1 for r in res if r[0] == 'hidden'); fail = [r for r in res if r[0] == 'FAIL']
    print(f'verify: {n} layouts (dp sizes 100..1500 x 100..1100, densities 2.625 / 1 / 4, {len(list(insets()))} inset combinations); hidden (u under {K["u_min_dp"]} dp) {hid}; shown {n - hid}; failed {len(fail)}')
    us = [r[6][0] for r in res if r[0] != 'hidden']
    if us: print(f'  shown u from {min(us):.3f} to {max(us):.3f} dp; every shown layout also checked at 1.09 x u')
    for r in fail[:20]: print('  FAIL', r[1:6], r[6])

if __name__ == '__main__': main()
