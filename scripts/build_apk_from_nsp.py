#!/usr/bin/env python3
# Android APK plus a game-data folder straight from the NSPs (docs/android/PLAN.md Phase 3): identify the game (title ID → game profile dm/games/<id>/) → extract (nx_extract) → rewrite the code
#   modules for Android (patch_nso.py --target android) → split out the shaders (shader_split.py) → the Vulkan shader modules (dm/gfx/tools/dmsh_build_vk.py through own_compilers_vk.py:
#   this Mac's spirv-cross) → the APK's package (modules, shaders,
#   font) → icon and label → the APK (dm/android/app/build_app.sh --gpu vkn: libdmhost.so with the Vulkan backend, libadrenotools and its hooks) → the game-data folder (RomFS, DLC).
# The stages this shares with the iOS script are taken from scripts/build_ipa_from_nsp.py by name (TAKE, checked before anything runs), as dmsh_build_vk.py takes the Metal chain's steps from
#   dmsh_build.py: an upstream fix to them reaches this script with no merge work, and a name that disappears or whose parameters change stops it with a [RED] line. That file is not changed.
# Usage: python3 -I scripts/build_apk_from_nsp.py <NSP folder> -k <prod.keys> [--work DIR] [--out DIR] [--app-name NAME] [--app-id ID] [-l LANGUAGE] [--env-file F] [--validation-layer SO]
#   [--refresh-icon | --no-online-icon] [-p VERSION] [--allow-damaged] [--progress MODE]; -h lists them. Exit code 0 on success, 1 on error, 130 when interrupted. Key values are never printed.
# Output (--out, default <NSP folder>/<title ID>_android/). The output and work folders must be new or empty, or marked by this script (.nxnative-apk: its use and the title), since it
#   replaces and deletes folders in them, and neither may be in the other (compared before either is made); a folder inside this repository or any git work tree (by file identity: another
#   case, /System/Volumes/Data, a link) or holding links is refused, and .gitignore covers the default names and *.apk besides. NSZ files are converted in build_ipa_from_nsp.py's
#   <NSP folder>/<title ID>_build/nsp/, shared with it (nsz_folder); --env-file is read by the phone's rule for env.txt (env_pairs). The output folder holds:
#   <name>.apk  org.nxnative.t<title ID in lowercase> (app_id_of: Android wants every part of a package name to start with a letter, so the iOS form org.nxnative.<title ID> is refused),
#               labelled with the game's own name (control data) and its cover as the icon. It contains the game's code: never share it.
#     lib/arm64-v8a/  libdmhost.so (Vulkan backend), libadrenotools.so and its hook libraries, and with --validation-layer the Khronos layer
#     assets/nx/      the package front_android.c unpacks into the app's files/pkg/ at the first start of a new stamp: mods/ (the rewritten modules and their tables), vksh/ (<key>_b.spv
#                     decompressed beside <key>_b.json, inputs.tsv, shader_map.tsv; DM_VK_SHADERS points there), font/nintendo_ext.ttf (make_ext_font.py; DM_FONT_DIR), env.txt (DM_LANG and
#                     --env-file: the build's defaults, below the phone's files/env.txt), index.txt and stamp
#     res/            the icon: an adaptive icon whose foreground is the cover over its 72dp visible area and whose background is a colour taken from the cover's edge
#   data/       the game data, kept out of the APK: romfs/ and aoc/<index>/ (DLC), APFS clones of the extraction where the volume has them. The last lines printed are the adb commands that put it
#               into the app's external files directory (/sdcard/Android/data/<app ID>/files/), where front_android.c looks (romfs_locate)
# Work folder (--work, default <NSP folder>/<title ID>_build/, the iOS script's, so an extraction and a shader split made by either are reused): extract/, shader/pkg/, aoc/, tools/, and android/ for
#   what only this script makes (patched_<version>/ with --target android, vkcache/b-<V>-vk/, payload/, res/, icon-cache/, logs/). Each stage is reused when its inputs are as at its last run.
# The icon: the cover from https://tinfoil.media/ti/<title ID>/512/512/ (else /256/256/), checked to be a whole square JPEG or PNG (at most 4 MB) and cached as
#   the PNG made of it in android/icon-cache/<title ID>/icon.png (--refresh-icon fetches it again, --no-online-icon skips the fetch); else the extraction's icon.jpg (the packed version's, then the
#   base game's), checked the same way; else none, said in red: the system's default icon.
# Only game profiles in dm/games and the versions they register: the Android host's CMake build reads dm/games/<id>/game.tsv, so a provisional or derived profile (build_ipa_from_nsp.py's, outside
#   the repository) is refused, as are games whose graphics API is not NVN (the Vulkan backend replays NVN). Stages that boot the Mac host (run-time shader capture, Unreal shader decoding, Oodle
#   packs) run as in the iOS script, on modules rewritten for the Mac.
# Needs: the Python packages of the iOS script (lz4, capstone, fonttools; zstd from Python 3.14 or backports.zstd, else the zstd tool), .NET SDK 10, spirv-cross, spirv-val, curl, sips, cmake, ninja,
#   the Android SDK (build-tools 36.1.0, platform 36, NDK 27.1; ANDROID_HOME), a JDK 17 or later (JAVA_HOME, else /usr/libexec/java_home), libadrenotools built as for the P0.7 probe
#   (ADRENOTOOLS_BUILD) and a signing keystore (APP_KEYSTORE, default the SDK's debug keystore: build_app.sh).
import argparse, collections, hashlib, importlib.util, inspect, json, os, re, shlex, shutil, struct, subprocess, sys, tempfile, time, unicodedata
from pathlib import Path
from xml.sax.saxutils import escape as xml_escape

REPO = Path(__file__).resolve().parents[1]
IPA_SCRIPT = REPO / 'scripts/build_ipa_from_nsp.py'


def load_ipa(path=IPA_SCRIPT):   # the iOS script as a module (its main() does not run on import)
    spec = importlib.util.spec_from_file_location('build_ipa_from_nsp', path)
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
    return mod


ipa = load_ipa()
BuildError = getattr(ipa, 'BuildError', RuntimeError)

# What this script takes from build_ipa_from_nsp.py: name → the parameters it must begin with, spelled as in the def (a plain name positional-or-keyword, '*name' the *args, a name after
# it keyword-only; None: not a function). A parameter added after them must have a default.
TAKE = {
    'BuildError': None, 'PROG': None, 'TOOL_PATHS': None, 'ALLOW_DAMAGED': None, 'COMPRESSED': None,
    'say': ('msg',), 'run': ('cmd', 'log', 'env', 'cwd', 'on_line'), 'rmtree': ('p',), 'swap_in': ('tmp', 'dst'), 'free_gb': ('p',), 'progress_mode': ('want',),
    'scan_nsps': ('nsp_dir',), 'decompress_nsz': ('nsp_dir', 'app', 'fam', 'keys', 'env'), 'vnum': ('ver',), 'nsp_versions': ('fam', 'builds'), 'pick_nsps': ('fam', 'have', 'versions'),
    'can_clone': ('d',), 'build_nx_extract': ('repo', 'work', 'logs', 'env'), 'extract': ('nx', 'keys', 'ex', 'name', 'base', 'upd', 'cmp_dir', 'logs', 'g'),
    'build_mismatch': ('d', 'name', 'g'), 'extract_aoc': ('nx', 'keys', 'repo', 'work', 'app', 'fam', 'logs'), 'collect_nros': ('py', 'repo', 'romfs', 'dst', 'env', 'logs'),
    'patch_files': ('repo', 'exe_dir', 'mods'), 'rewrite': ('py', 'repo', 'exe_dir', 'out', 'env', 'logs', 'g'), 'fingerprint': ('*items', 'env'), 'reused': ('keyfile', 'key'),
    'files_under': ('d',), 'package_key': ('pkg',), 'profile_files': ('g',), 'chain_header': ('py', 'repo', 'env', 'logs'),
    'split_shaders': ('py', 'repo', 'roms', 'mains', 'pkg', 'env', 'logs', 'g', 'provisional', 'oodle'), 'build_mac_host': ('repo', 'out', 'env', 'logs', 'g'),
    'capture_glslc': ('repo', 'dirs', 'pkg_ver', 'patched', 'work', 'env', 'logs', 'g'), 'decode_ue_shaders': ('repo', 'dirs', 'pkg_ver', 'patched', 'work', 'env', 'logs', 'g'),
    'language_code': ('s',), 'game_language': ('want', 'asked', 'have'), 'language_list': ('codes',), 'amount': ('v', 'unit'),
}


def take(mod, spec=TAKE):
    """Check that mod (build_ipa_from_nsp.py) still has every name this script uses, functions with the parameters it calls them with; stop with why otherwise"""
    P, bad = inspect.Parameter, []
    for name, params in spec.items():
        if not hasattr(mod, name): bad.append(f'{name} is gone'); continue
        if params is None: continue
        f = getattr(mod, name)
        if not callable(f): bad.append(f'{name} is no longer a function'); continue
        want, kw = [], False   # (name, kind) as the spec spells them: this script calls some by position and some by keyword (run(..., env=)), so the kind matters as much as the name
        for s in params:
            if s == '*': kw = True
            elif s.startswith('*'): want.append((s[1:], P.VAR_POSITIONAL)); kw = True
            else: want.append((s, P.KEYWORD_ONLY if kw else P.POSITIONAL_OR_KEYWORD))
        ps = list(inspect.signature(f).parameters.values())
        if [(p.name, p.kind) for p in ps[:len(want)]] != want: bad.append(f'{name}{inspect.signature(f)} does not begin with ({", ".join(params)})')
        elif any(p.default is p.empty and p.kind not in (p.VAR_POSITIONAL, p.VAR_KEYWORD) for p in ps[len(want):]): bad.append(f'{name} has a new parameter without a default')
    if bad: raise BuildError('what build_apk_from_nsp.py takes from build_ipa_from_nsp.py has changed: ' + '; '.join(bad) + ' (follow the change in TAKE and the calls)')


def say(msg): ipa.say(msg)


# ---- Names ----
PKG_RE = re.compile(r'[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+')


def app_id_of(title):   # the app ID: org.nxnative.t<title ID in lowercase>. iOS's org.nxnative.<title ID> is not a valid Android package name (a part may not start with a digit)
    return f'org.nxnative.t{title:016x}'


def check_app_id(s):
    if not PKG_RE.fullmatch(s): raise BuildError(f'app ID {s!r} is not a valid Android package name: two or more parts, each a letter then letters, digits or _')
    if not s.startswith('org.nxnative.'): say(f'[RED] app ID {s}: dm/android/app/run_app.sh and the phone rules of this project take only org.nxnative.* packages')
    return s


def label_attr(name):
    """The app name as the value of android:label: aapt2 reads a backslash as an escape and a leading @ or ? as a reference, so those are escaped, then the XML specials. Only control and
    format characters (Unicode Cc: newlines, tabs; Cf: zero-width and direction marks) and line / paragraph separators are refused; every other character is kept as it is, U+3000 and
    U+00A0 of Japanese titles among them (aapt2 keeps them, and runs of spaces, as written)"""
    bad = sorted({f'U+{ord(c):04X}' for c in name if unicodedata.category(c) in ('Cc', 'Cf', 'Zl', 'Zp')})
    if not name.strip() or bad: raise BuildError(f'the app name {name!r} is empty or has control or format characters ({", ".join(bad) or "nothing but spaces"}) (--app-name)')
    s = name.replace('\\', '\\\\')
    if s[0] in '@?': s = '\\' + s
    return xml_escape(s, {'"': '&quot;', "'": '&apos;'})


def apk_manifest(template, app_id, label, icon):
    """A game's manifest from dm/android/app/AndroidManifest.xml (the development APK's): its comments dropped, package and android:label replaced (each once, in their own tags),
    and with icon the adaptive icon (make_icon_res) as android:icon and android:roundIcon"""
    t = re.sub(r'<!--.*?-->\s*', '', template, flags=re.S)
    def sub(tag, attr, value, t):
        m = [x for x in re.finditer(rf'<{tag}\b[^>]*>', t)]
        if len(m) != 1: raise BuildError(f'the manifest template has {len(m)} <{tag}> tags, not one')
        head = m[0].group(0); n = len(re.findall(rf'\s{attr}="[^"]*"', head))
        if n != 1: raise BuildError(f'the manifest template\'s <{tag}> has {n} {attr} attributes, not one')
        return t[:m[0].start()] + re.sub(rf'(\s){attr}="[^"]*"', lambda x: f'{x.group(1)}{attr}="{value}"', head) + t[m[0].end():]
    t = sub('manifest', 'package', check_app_id(app_id), t)
    t = sub('application', 'android:label', label_attr(label), t)
    if icon:
        if re.search(r'<application\b[^>]*\sandroid:(round)?[iI]con=', t): raise BuildError('the manifest template already names an icon')
        t = re.sub(r'<application\b', '<application android:icon="@mipmap/nx_icon" android:roundIcon="@mipmap/nx_icon"', t, count=1)
    return t


# ---- Icon ----
ICON_URL = 'https://tinfoil.media/ti/{tid}/{n}/{n}/'   # the title's cover by title ID, square; 512 when it serves it, else 256


ICON_MAX = 4 << 20   # a cover is some 200 KB: curl stops past this (--max-filesize), and a file over it is not taken


def image_kind(p):   # 'jpg' / 'png' by the file's magic number (its first bytes only), None for anything else (an HTML error page)
    try:
        with open(p, 'rb') as f: b = f.read(8)
    except OSError: return None
    return 'jpg' if b[:3] == b'\xff\xd8\xff' else 'png' if b == b'\x89PNG\r\n\x1a\n' else None


def image_whole(p, kind):
    """Whether the file ends as its format ends: a JPEG with its end-of-image marker (FF D9, at most 32 bytes of padding after it), a PNG with its IEND chunk. sips reads a cut file
    without a word, the missing part grey, so a download cut short would pass every other check"""
    try:
        with open(p, 'rb') as f: f.seek(0, 2); n = f.tell(); f.seek(max(0, n - 64)); t = f.read()
    except OSError: return False
    if kind == 'png': return t.endswith(b'\0\0\0\0IEND\xaeB`\x82')
    i = t.rfind(b'\xff\xd9')
    return kind == 'jpg' and i >= 0 and len(t) - i - 2 <= 32


def sips_size(p):   # (width, height) as sips reads the image, None when it cannot
    r = subprocess.run(['sips', '-g', 'pixelWidth', '-g', 'pixelHeight', str(p)], capture_output=True, text=True)
    w, h = re.search(r'pixelWidth: (\d+)', r.stdout), re.search(r'pixelHeight: (\d+)', r.stdout)
    return (int(w.group(1)), int(h.group(1))) if not r.returncode and w and h else None


def cover_why(p):   # None when p is a whole JPEG or PNG, square, of 128 pixels or more and at most ICON_MAX bytes; else why not
    k = image_kind(p)
    if not k: return 'not a JPEG or PNG'
    if Path(p).stat().st_size > ICON_MAX: return f'larger than {ICON_MAX >> 20} MB'
    if not image_whole(p, k): return f'a {k.upper()} cut short (no end marker)'
    wh = sips_size(p)
    if not wh or wh[0] != wh[1] or wh[0] < 128: return f'not a square of 128 pixels or more ({"x".join(map(str, wh)) if wh else "sips cannot read it"})'
    return None


def cover_png(src, dst):
    """The cover src checked (cover_why) and made a PNG of at most 512 pixels in dst with sips, which is checked in turn: what makes an untrusted download, or an extraction's icon.jpg,
    an icon. None when done, else why not (dst then removed)"""
    if (why := cover_why(src)): return why
    ipa.rmtree(dst)
    r = subprocess.run(['sips', '-s', 'format', 'png'] + (['-Z', '512'] if sips_size(src)[0] > 512 else []) + [str(src), '--out', str(dst)], capture_output=True, text=True)
    if r.returncode or image_kind(dst) != 'png' or not image_whole(dst, 'png'):
        ipa.rmtree(dst); return f'sips cannot make a PNG of it ({(r.stderr.strip().splitlines() or ["no message"])[-1][:120]})'
    return None


def fetch_icon(tid, cache, refresh=False):
    """The cover from ICON_URL, cached in cache/<title ID>/icon.png. A download is untrusted data: it goes into a new empty folder of its own (curl stops past ICON_MAX), and is kept only as
    the PNG cover_png makes of it (an HTTP 200 alone proves nothing, nor does a header sips reads). Returns (path, how) or (None, why)"""
    d = cache / tid; have = d / 'icon.png'
    ok = have.is_file() and cover_why(have) is None
    if ok and not refresh: return have, 'cached'
    cache.mkdir(parents=True, exist_ok=True)
    tmp, why = Path(tempfile.mkdtemp(prefix=f'.{tid}-', dir=cache)), 'not fetched'
    try:
        for n in (512, 256):
            f, png, url = tmp / 'download', tmp / 'icon.png', ICON_URL.format(tid=tid, n=n)
            ipa.rmtree(f); ipa.rmtree(png)
            r = subprocess.run(['curl', '-fsSL', '--max-time', '20', '--max-filesize', str(ICON_MAX), '-o', str(f), url], capture_output=True, text=True)
            if r.returncode: why = f'{url}: curl exit {r.returncode} ({(r.stderr.strip().splitlines() or [""])[-1][:120]})'; continue
            if (no := cover_png(f, png)): why = f'{url}: {no}'; continue
            ipa.rmtree(d); d.mkdir(parents=True); png.rename(have)
            return have, f'fetched {url}'
    finally: ipa.rmtree(tmp)
    if ok: return have, f'cached (fetching again failed: {why})'
    return None, why


def bmp_edge_colour(b):
    """The mean colour of a BMP's outermost pixels, '#rrggbb' (sips writes 24-bit BI_RGB, top-down; 32-bit and bottom-up are read too)"""
    if b[:2] != b'BM' or len(b) < 54: raise ValueError('not a BMP')
    off, = struct.unpack_from('<I', b, 10); w, h = struct.unpack_from('<ii', b, 18); bpp, comp = struct.unpack_from('<HI', b, 28)
    if bpp not in (24, 32) or comp not in (0, 3) or w <= 0 or h == 0: raise ValueError(f'BMP of {bpp} bits, compression {comp}')
    H, n, stride = abs(h), bpp // 8, (w * bpp // 8 + 3) & ~3
    if off + stride * H > len(b): raise ValueError('BMP shorter than its header says')
    pts = {(x, y) for x in range(w) for y in (0, H - 1)} | {(x, y) for y in range(H) for x in (0, w - 1)}
    s = [0, 0, 0]
    for x, y in pts:
        o = off + (y if h < 0 else H - 1 - y) * stride + x * n
        for i in range(3): s[i] += b[o + 2 - i]   # stored B, G, R
    return '#' + ''.join(f'{round(v / len(pts)):02x}' for v in s)


ICON_XML = {
    'mipmap-anydpi-v26/nx_icon.xml': '<?xml version="1.0" encoding="utf-8"?>\n<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
                                     '    <background android:drawable="@color/nx_icon_bg"/>\n    <foreground android:drawable="@drawable/nx_icon_fg"/>\n</adaptive-icon>\n',
    # the cover over the 72dp a launcher shows of the 108dp layers (18dp each side): its mask rounds the corners and cuts nothing else; the background fills what the mask leaves
    'drawable/nx_icon_fg.xml': '<?xml version="1.0" encoding="utf-8"?>\n<inset xmlns:android="http://schemas.android.com/apk/res/android" android:drawable="@drawable/nx_cover" android:inset="16.6667%"/>\n',
}


def make_icon_res(src, res):
    """The Android resources of the icon from the cover src: res/drawable-nodpi/nx_cover.png (cover_png: checked, at most 512 pixels), the adaptive icon (ICON_XML) and its background colour
    (values/). A cover that is not one stops with BuildError and leaves res as it was, so the caller can take the next source"""
    tmp = res.with_name(res.name + '.partial'); ipa.rmtree(tmp)
    (tmp / 'drawable-nodpi').mkdir(parents=True); (tmp / 'values').mkdir()
    cover = tmp / 'drawable-nodpi/nx_cover.png'
    if (why := cover_png(src, cover)): ipa.rmtree(tmp); raise BuildError(f'{src}: {why}')
    bmp = tmp / 'edge.bmp'
    r = subprocess.run(['sips', '-s', 'format', 'bmp', '-z', '32', '32', str(cover), '--out', str(bmp)], capture_output=True, text=True)
    try: bg = bmp_edge_colour(bmp.read_bytes()) if not r.returncode else '#000000'
    except (OSError, ValueError): bg = '#000000'
    ipa.rmtree(bmp)
    (tmp / 'values/nx_icon.xml').write_text(f'<?xml version="1.0" encoding="utf-8"?>\n<resources>\n    <color name="nx_icon_bg">{bg}</color>\n</resources>\n')
    for rel, text in ICON_XML.items():
        (tmp / rel).parent.mkdir(parents=True, exist_ok=True); (tmp / rel).write_text(text)
    ipa.swap_in(tmp, res)
    return bg


def icon_from(srcs, res):
    """The icon resources (make_icon_res) from the first of srcs, [(kind, what, path)] in order, that is a cover: one that is not (cut short, not square, not an image) falls through to the
    next. Says which was taken. Returns (its kind or None, [why each one before it was not taken])"""
    ipa.rmtree(res); skipped = []
    for kind, what, p in srcs:
        try: bg = make_icon_res(p, res)
        except BuildError as e: skipped.append(f'{what}: {e}'); continue
        say(f'icon: {what}: {p}; background {bg}' + (f' (not taken: {"; ".join(skipped)})' if skipped else ''))
        return kind, skipped
    return None, skipped


# ---- The APK's package (assets/nx) ----
NAME_RE = re.compile(r'[A-Za-z0-9_-][A-Za-z0-9._-]*')   # each part of a package path (front_android.c path_ok takes no others)


def payload_index(nx):
    """index.txt's text ("<size>\t<path>" per file, sorted) and the stamp: 16 hex digits of a SHA-256 over every path and its content's SHA-256, so the same files give the same stamp"""
    rows, h = [], hashlib.sha256(b'nxpkg 1\n')
    for rel, st in ipa.files_under(nx):
        if rel in ('index.txt', 'stamp'): continue
        if not all(NAME_RE.fullmatch(p) for p in rel.split('/')): raise BuildError(f'package file {rel}: a name with characters other than A-Z a-z 0-9 . _ - (or starting with .)')
        d = hashlib.sha256()
        with open(nx / rel, 'rb') as f:
            for blk in iter(lambda: f.read(1 << 20), b''): d.update(blk)
        rows.append(f'{st.st_size}\t{rel}'); h.update(f'{rel}\0{st.st_size}\0{d.hexdigest()}\n'.encode())
    return ''.join(r + '\n' for r in rows), h.hexdigest()[:16]


def clone_file(src, dst):   # an APFS clone where the volume has them, else a copy
    if sys.platform == 'darwin' and not subprocess.run(['cp', '-c', str(src), str(dst)], capture_output=True).returncode: return
    shutil.copy2(src, dst)


def clone_tree(src, dst):
    if sys.platform == 'darwin' and not subprocess.run(['cp', '-cR', str(src), str(dst)], capture_output=True).returncode: return
    ipa.rmtree(dst); shutil.copytree(src, dst)


def zstd_decompress(data):
    for m in ('compression.zstd', 'backports.zstd'):
        try: return importlib.import_module(m).decompress(data)
        except ImportError: continue
    r = subprocess.run(['zstd', '-q', '-d', '-c'], input=data, capture_output=True)
    if r.returncode: raise BuildError(f'zstd -d failed: {r.stderr.decode(errors="replace")[-200:]}')
    return r.stdout


def vk_modules(src, dst, chain):
    """dmsh_build_vk.py's output src (b-<V>-vk/) → dst as the backend reads it: each <key>_b.spv decompressed beside its <key>_b.json, checked against the reflection's chain, spv_bytes and
    spv_sha256 (a module that does not match stops the build), and the stamp inputs.tsv. Returns (modules, of which not ok)"""
    dst.mkdir(parents=True, exist_ok=True); n = bad = 0
    for j in sorted(src.glob('*_b.json')):
        if j.name.startswith('_'): continue   # _b_build.json, the run's status list
        r = json.loads(j.read_text()); k = j.name[:-5]
        if r.get('chain') != chain: raise BuildError(f'shader {k}: chain {r.get("chain")}, the host takes {chain}')
        z, raw = src / f'{k}.spv.zst', src / f'{k}.spv'
        spv = zstd_decompress(z.read_bytes()) if z.is_file() else raw.read_bytes() if raw.is_file() else None
        if spv is None: raise BuildError(f'shader {k}: no {z.name} or {raw.name} in {src}')
        if len(spv) != r.get('spv_bytes') or hashlib.sha256(spv).hexdigest()[:16] != r.get('spv_sha256'):
            raise BuildError(f'shader {k}: {len(spv)} bytes, SHA-256 {hashlib.sha256(spv).hexdigest()[:16]}; its reflection says {r.get("spv_bytes")}, {r.get("spv_sha256")}')
        (dst / f'{k}.spv').write_bytes(spv); shutil.copyfile(j, dst / j.name)
        n += 1; bad += r.get('status') != 'ok'
    if not (src / 'inputs.tsv').is_file(): raise BuildError(f'{src} has no stamp (inputs.tsv): dmsh_build_vk.py did not finish a full build')
    shutil.copyfile(src / 'inputs.tsv', dst / 'inputs.tsv')
    return n, bad


def make_payload(dst, patched, vkdir, chain, shmap, font, env_text):
    """assets/nx of the APK, built in dst.partial and swapped in: mods/ (every file of the rewrite, as run_app.sh pushes them), vksh/ (vk_modules, plus the profile's shader map), font/, env.txt,
    then index.txt and stamp. Large files are APFS clones. Returns (stamp, files, bytes, shader modules, of which not ok)"""
    tmp = dst.with_name(dst.name + '.partial'); ipa.rmtree(tmp); nx = tmp / 'nx'
    (nx / 'mods').mkdir(parents=True); (nx / 'font').mkdir()
    for p in sorted(patched.iterdir()):
        if p.is_file() and not p.name.endswith('.key'): clone_file(p, nx / 'mods' / p.name)
    n, bad = vk_modules(vkdir, nx / 'vksh', chain)
    shutil.copyfile(shmap, nx / 'vksh/shader_map.tsv')
    shutil.copyfile(font, nx / 'font' / font.name)
    (nx / 'env.txt').write_text(env_text)
    index, stamp = payload_index(nx)
    (nx / 'index.txt').write_text(index); (nx / 'stamp').write_text(stamp + '\n')
    ipa.swap_in(tmp, dst)
    return stamp, index.count('\n'), sum(int(l.split('\t')[0]) for l in index.splitlines()), n, bad


# ---- Stages ----
def vkn_chain(repo):   # the shader chain the Vulkan backend takes (dm/gfx/vkn/vkn.h VKN_CHAIN)
    m = re.search(r'#define VKN_CHAIN "([0-9a-f]+)"', (repo / 'dm/gfx/vkn/vkn.h').read_text())
    if not m: raise BuildError('no VKN_CHAIN in dm/gfx/vkn/vkn.h')
    return m.group(1)


def android_sdk(env):
    """The Android SDK pieces build_app.sh uses, the JDK (env JAVA_HOME is set) and libadrenotools, checked up front. Returns ANDROID_HOME"""
    home = Path(env.get('ANDROID_HOME') or os.path.expanduser('~/Library/Android/sdk'))
    bt, ndk = home / 'build-tools/36.1.0', Path(env.get('ANDROID_NDK_HOME') or home / 'ndk/27.1.12297006')
    need = [bt / t for t in ('aapt2', 'zipalign', 'apksigner', 'd8')] + [home / 'platforms/android-36/android.jar', ndk / 'build/cmake/android.toolchain.cmake']
    at = Path(env.get('ADRENOTOOLS_BUILD') or os.path.expanduser('~/dm-work/android/src/libadrenotools-build'))
    need += [at / 'libadrenotools.so'] + [at / 'src/hook' / f'lib{h}.so' for h in ('hook_impl', 'main_hook', 'file_redirect_hook', 'gsl_alloc_hook')]
    gone = [str(p) for p in need if not p.exists()]
    sdkcm = sorted(home.glob('cmake/*/bin'))   # build.sh: cmake and ninja from PATH, else the SDK's cmake package
    gone += [f'{t} (brew install {t}, or the SDK\'s cmake package)' for t in ('cmake', 'ninja') if not shutil.which(t, path=env['PATH']) and not (sdkcm and (sdkcm[-1] / t).exists())]
    if gone: raise BuildError('missing: ' + ', '.join(gone) + ' (the Android SDK, NDK 27.1 and libadrenotools: docs/android/probes-gpu.md, "Loading the drivers")')
    ks = Path(env.get('APP_KEYSTORE') or os.path.expanduser('~/.android/debug.keystore'))
    if not ks.exists(): raise BuildError(f'no signing keystore at {ks} (APP_KEYSTORE)')   # only whether it is there: never opened here
    jh = env.get('JAVA_HOME')
    if not (jh and (Path(jh) / 'bin/javac').exists()):
        r = subprocess.run(['/usr/libexec/java_home', '-v', '17+'], capture_output=True, text=True) if Path('/usr/libexec/java_home').exists() else None
        cands = ([r.stdout.strip()] if r and not r.returncode else []) + ['/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home', '/opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home']
        jh = next((c for c in cands if c and (Path(c) / 'bin/javac').exists()), None)
        if not jh: raise BuildError('no JDK 17 or later (javac): set JAVA_HOME')
    env['JAVA_HOME'], env['ANDROID_HOME'] = jh, str(home)
    return home


def check_prereqs(env, xlate):
    missing = [m for m in ('lz4.block', 'capstone', 'fontTools') if not importlib.util.find_spec(m.split('.')[0])]
    if missing: raise BuildError(f'the current Python ({sys.executable}) is missing modules: {", ".join(missing)} (pip install lz4 capstone fonttools)')
    tools = {'dotnet': '.NET SDK 10', 'spirv-cross': 'brew install spirv-cross', 'spirv-val': 'brew install spirv-tools', 'curl': 'curl', 'sips': 'macOS'}
    gone = [f'{t} ({how})' for t, how in tools.items() if not shutil.which(t, path=env['PATH'])]
    if not any(_importable(m) for m in ('compression.zstd', 'backports.zstd')) and not shutil.which('zstd', path=env['PATH']): gone.append('zstd (brew install zstd)')
    if gone: raise BuildError('tools not found: ' + ', '.join(gone))
    if not env.get('DOTNET_ROOT'):   # nx_extract is an apphost executable: it finds the runtime through DOTNET_ROOT or /usr/local/share/dotnet only, and a per-user SDK (~/.dotnet) sets it in the login profile, which a non-login shell or a GUI app does not read
        env['DOTNET_ROOT'] = os.environ['DOTNET_ROOT'] = str(Path(shutil.which('dotnet', path=env['PATH'])).resolve().parent)   # also this process's: build_ipa_from_nsp.py's extract runs nx_extract without env=
    if not xlate.exists(): raise BuildError(f'the shader translator is missing from the repository: {xlate}')
    return android_sdk(env)


def _importable(m):
    try: importlib.import_module(m); return True
    except ImportError: return False


def rewrite_android(py, exe_dir, out, env, logs, g):
    """patch_nso.py --target android on every code module and the NROs the game registers, as build_ipa_from_nsp.py's rewrite does for the Mac and iOS (the target is in the stage's key and
    on each command line); reused when its inputs are as at the last successful run"""
    import capstone
    PROG = ipa.PROG; mods = g.words('modules')
    PROG.stage('rewrite for Android', step='checking the inputs')
    nro_dir = out.with_name('nro_' + out.name.removeprefix('patched_'))
    nros = ipa.collect_nros(py, REPO, exe_dir.parent / 'RomFS', nro_dir, env, logs)
    manifest = nro_dir / 'nro.tsv'
    src = {m: exe_dir / m for m in mods} | nros
    kf = out.with_name(out.name + '.key')
    key = ipa.fingerprint(('files', [REPO / 'dm/tools' / f for f in ('patch_nso.py', 'nso_code.py', 'nso_asm.py')] + ipa.patch_files(REPO, exe_dir, mods) + list(src.values())
                           + ([manifest] if manifest.is_file() else [])), list(src), capstone.__version__, {k: env.get(k) for k in ('DM_PATCH_FRONT',)}, 'target android')
    what = ', '.join(mods) + (f' and {len(nros)} NROs' if nros else '')
    if all((out / f'{m}.patched.bin').is_file() for m in src) and ipa.reused(kf, key) is not None:
        PROG.end(f'{what}, reused (inputs unchanged)'); return
    tmp = out.with_name(out.name + '.partial'); ipa.rmtree(tmp); tmp.mkdir(parents=True); logs.mkdir(parents=True, exist_ok=True)
    if manifest.is_file(): shutil.copyfile(manifest, tmp / 'nro.tsv')
    PROG.set(step=what, done=0, total=len(src), unit='modules')
    procs, logf, todo, cores = {}, {}, list(src), max(2, os.cpu_count() or 4)   # as build_ipa_from_nsp.py's rewrite: as many at once as the Mac has cores
    try:
        while todo or procs:
            while todo and len(procs) < cores:
                m = todo.pop(0); logf[m] = open(logs / f'rewrite_android_{m}.log', 'w')
                procs[m] = subprocess.Popen([str(py), str(REPO / 'dm/tools/patch_nso.py'), '--target', 'android', str(src[m]), str(tmp), m], env=env, stdout=logf[m], stderr=subprocess.STDOUT)
            for m in [m for m in procs if procs[m].poll() is not None]:
                p = procs.pop(m); logf[m].close(); PROG.set(done=len(src) - len(todo) - len(procs))
                if p.returncode:
                    log = logs / f'rewrite_android_{m}.log'
                    PROG.end(ok=False)
                    with open(log, errors='replace') as lf: PROG.say(f'---- last lines of {log} ----\n' + ''.join(collections.deque(lf, maxlen=40)).rstrip('\n'))
                    raise BuildError(f'patch_nso.py --target android exit code {p.returncode} on {m} (full output: {log})')
            time.sleep(0.2)
    finally:   # an error or an interrupt stops the other rewrites (Monster Hunter Rise's main alone takes minutes)
        for p in procs.values():
            if p.poll() is None: p.terminate(); p.wait()
        for f in logf.values(): f.close()
    ipa.rmtree(kf); ipa.swap_in(tmp, out); kf.write_text(f'{key}\n')
    PROG.end(what)


def vk_stamp(out):   # dmsh_build_vk.py's stamp (inputs.tsv: game, inputs) as a dict, {} when there is none
    try: return dict(l.rstrip('\n').split('\t', 1) for l in (out / 'inputs.tsv').read_text().splitlines() if '\t' in l and not l.startswith('#'))
    except OSError: return {}


def vk_build_status(out):   # the run's status per shader (dmsh_build_vk.py's _b_build.json: [{key, st, err}]) counted, None when there is none
    try: rows = json.loads((out / '_b_build.json').read_text())
    except (OSError, ValueError): return None
    return collections.Counter(r.get('st') for r in rows if isinstance(r, dict)) if isinstance(rows, list) and rows else None


def vk_shaders(py, pkg, root, pkg_key, env, logs, g, xlate):
    """The Vulkan shader modules (dm/gfx/tools/dmsh_build_vk.py, through own_compilers_vk.py: this Mac's spirv-cross may differ from the lock) of the split package pkg into root/b-<V>-vk/.
    V must be the chain the host takes (vkn.h), or the APK would draw nothing. Reused when the package's content, the profile, the translator, spirv-val and spirv-cross are as at the last full
    build; otherwise built again, and only this run's output is taken: its stamp (inputs.tsv) and status list (_b_build.json) are removed first, because dmsh_build_vk.py's early exits (the
    tool lock, no spirv-val, another game's output, no answer table) leave the last build's stamped output in place. A module the chain marks unsupported is packed (the backend refuses it and
    says so); a shader with no output at all stops the build. Returns (output folder, V)"""
    PROG = ipa.PROG
    PROG.stage('Vulkan shaders', step='checking the inputs')
    r = subprocess.run([py, REPO / 'dm/gfx/tools/dmsh_build_vk.py', '--version'], env=env, capture_output=True, text=True)
    V, host = r.stdout.strip(), vkn_chain(REPO)
    if r.returncode or not V: raise BuildError(f'dmsh_build_vk.py --version: exit {r.returncode} {r.stderr.strip()[-300:]}')
    if V != host: raise BuildError(f'the Vulkan shader chain is {V} (dmsh_build_vk.py --version) but the host takes {host} (dm/gfx/vkn/vkn.h VKN_CHAIN): the backend would refuse every module; update VKN_CHAIN with the chain change')
    out, kf, log = root / f'b-{V}-vk', root / f'b-{V}-vk.key', logs / 'dmsh_build_vk.log'
    key = ipa.fingerprint(pkg_key, ('files', ipa.profile_files(g)), ('stat', xlate.parent), ('cmd', ['spirv-val', '--version']), ('cmd', ['spirv-cross', '--revision']), env=env) if pkg_key else None
    if key and (out / 'inputs.tsv').is_file() and (note := ipa.reused(kf, key)) is not None:
        PROG.end(f'{note}, reused (inputs unchanged)'); return out, V
    ipa.rmtree(kf); root.mkdir(parents=True, exist_ok=True); log.parent.mkdir(parents=True, exist_ok=True)
    for f in ('inputs.tsv', '_b_build.json'): ipa.rmtree(out / f)   # what the last build left: only this run's may be taken
    PROG.set(step='translating')
    with open(log, 'w') as f: rc = subprocess.run([py, REPO / 'dm/gfx/tools/own_compilers_vk.py', pkg, root], env=env, stdout=f, stderr=subprocess.STDOUT).returncode
    text = log.read_text(errors='replace'); st = vk_build_status(out)
    if not (out / 'inputs.tsv').is_file() or not st:
        PROG.end(ok=False); PROG.say(f'---- last lines of {log} ----\n' + '\n'.join(text.splitlines()[-30:]))
        raise BuildError(f'dmsh_build_vk.py exit code {rc}: no stamped output of this run in {out} (a shader failed, or the tool stopped before building; full output: {log})')
    if (sg := vk_stamp(out).get('game')) != g.id: raise BuildError(f'{out} is stamped for game {sg}, not {g.id} (full output: {log})')
    if own := re.search(r"^note: (built with this Mac's spirv-cross.*)$", text, re.M): say(own.group(1))
    total, ok = sum(st.values()), st.get('ok', 0)
    note = f'{ok:,} of {total:,} modules ok'
    if rc or ok < total: say(f'[RED] Vulkan shaders: {note} ({", ".join(f"{n} {s}" for s, n in st.most_common() if s != "ok") or f"exit code {rc}"}); the backend refuses the others and says so at their first draw (reasons in {log})')
    if key: kf.write_text(f'{key}\n{note}\n')
    PROG.end(note)
    return out, V


def ext_font(py, dst, env, logs):   # the button-icon font the host gives for the Nintendo extension shared font (make_ext_font.py; not made from game files), made again when the tool changed
    ttf, kf = dst / 'nintendo_ext.ttf', dst / 'nintendo_ext.key'
    key = ipa.fingerprint(('files', [REPO / 'dm/tools/make_ext_font.py']))
    if not (ttf.is_file() and ipa.reused(kf, key) is not None):
        ipa.rmtree(kf); dst.mkdir(parents=True, exist_ok=True)
        ipa.run([py, REPO / 'dm/tools/make_ext_font.py', ttf], logs / 'make_ext_font.log', env=env)
        kf.write_text(f'{key}\n')
    return ttf


def data_folder(out, romfs, aoc, aoc_key=None):
    """The game data the APK leaves out, as it goes into the app's external files directory: out/data/romfs (and aoc/), APFS clones of the extraction where the volume has them, else copies.
    Made again when the RomFS or the DLC changed: every file's path, size and modification time (an extraction made again writes new files), and the DLC extraction's own key (aoc_key,
    build_ipa_from_nsp.py's work/aoc.key: the DLC NSPs' names, sizes and times), so a DLC replaced by one of the same names and sizes is not taken for the old one"""
    d, kf = out / 'data', out / 'data.key'
    key = ipa.fingerprint(('stat', romfs), *([('stat', aoc), ('files', [aoc_key or aoc.with_name(aoc.name + '.key')])] if aoc else []), str(romfs), str(aoc))
    if d.is_dir() and ipa.reused(kf, key) is not None: return d, 'reused'
    tmp = out / 'data.partial'; ipa.rmtree(tmp); tmp.mkdir(parents=True); ipa.rmtree(kf)
    clone_tree(romfs, tmp / 'romfs')
    if aoc: clone_tree(aoc, tmp / 'aoc')
    ipa.swap_in(tmp, d); kf.write_text(f'{key}\n')
    return d, 'made'


# ---- Folders: where game files may go ----
MARK = '.nxnative-apk'   # in each output and work folder this script made or took: its use and the title ID, so it never clears a folder it was not given for that


def made_part(d):   # (d's nearest existing folder, [the names below it, not made yet])
    p, rest = Path(os.path.abspath(d)), []
    while not p.exists() and p != p.parent: rest.insert(0, p.name); p = p.parent
    return p, rest


def same_file(a, b):
    try: return os.path.samefile(a, b)
    except OSError: return False


def same_name(a, b):   # as macOS's default volume compares names: without regard to case or Unicode normalization (on a case-sensitive volume this errs on the safe side: a refusal)
    f = lambda s: unicodedata.normalize('NFC', s).casefold()
    return f(a) == f(b)


def within(d, root):
    """Whether d is root or under it, by file identity for the part of each path that exists (os.path.samefile on d's nearest existing folder and each folder above it, as written and with
    links resolved), so that a path spelling root in another case, through /System/Volumes/Data or through a link is the same folder. When root is not made yet, nothing under it exists
    either: d is under it when d's nearest existing folder is root's and d's further names begin with root's (same_name), so --out X --work X/data are one in the other before either is made"""
    p, rest = made_part(d); q, qrest = made_part(root)
    if qrest: return len(rest) >= len(qrest) and same_file(p, q) and all(map(same_name, rest, qrest))
    r = Path(os.path.realpath(p))
    return any(same_file(a, root) for a in dict.fromkeys([p, *p.parents, r, *r.parents]))


def repo_of(d):
    """The repository d is in (or would be made in), or None: this one by file identity (within), or any git work tree (git rev-parse --show-toplevel: another clone or worktree of this
    one). Game files must go into neither: the fork is public, and the APK holds the game's code"""
    if within(d, REPO): return str(REPO)
    p = Path(os.path.abspath(d))
    while not p.exists() and p != p.parent: p = p.parent
    try: r = subprocess.run(['git', '-C', str(p), 'rev-parse', '--show-toplevel'], capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired): return None
    return (r.stdout.strip() or None) if r.returncode == 0 else None


def links_under(d, depth=3):
    """The symbolic links at d and down to depth levels under it (not followed): a stage folder that is a link (work/android, work/extract, out/data ...) would carry this script's writes and
    deletions somewhere else, past every check of d itself"""
    found = [str(d)] if Path(d).is_symlink() else []
    todo = [] if found else [(str(d), 0)]
    while todo:
        top, n = todo.pop()
        try: es = list(os.scandir(top))
        except OSError: continue
        for e in es:
            if e.is_symlink(): found.append(e.path)
            elif n + 1 < depth and e.is_dir(follow_symlinks=False): todo.append((e.path, n + 1))
    return sorted(found)


def nsz_folder(nsp_dir, app):
    """The folder build_ipa_from_nsp.py's decompress_nsz writes (nsp/ with the converted NSPs, its temporary nsp/partial/ and logs/), whatever --work says: the iOS script's
    <title ID>_build/, named by the title ID (the default work folder is named by the profile's ID, which may differ). Both scripts convert there with the same function, so they share its
    nsp/: this script takes the folder as it is (checked as a work folder is, not marked), as it takes the default work folder; what the step deletes there is its own (nsp/partial/, and a
    conversion whose source changed). dm/tools/tests/test_nsp_apk.py checks the name against decompress_nsz"""
    return nsp_dir / f'{app:016X}_build'


def check_folder(d, what):   # a folder this script writes (and deletes) in: refused inside a repository, or with links in it
    if (r := repo_of(d)): raise BuildError(f'the {what} folder {d} is inside {"the repository" if r == str(REPO) else "a git work tree"} ({r}): game files must not go there; give a folder outside it')
    if (ls := links_under(d)): raise BuildError(f'the {what} folder {d} holds links ({", ".join(ls[:3])}{" ..." if len(ls) > 3 else ""}): this script writes and deletes inside it; give a folder without them')


def claim(d, what, tid, adopt=False):
    """Take d as the output or work folder (what) of title tid: this script replaces and deletes whole folders in it (out/data, work/tmp, the stages' outputs), so d must be new or empty, or
    carry the mark this script left there for the same use and title. adopt: an existing folder taken as it is (the default work folder, build_ipa_from_nsp.py's <title ID>_build, which shares
    its stages). The mark is written before anything else"""
    m, want = d / MARK, f'build_apk_from_nsp.py\t{what}\t{tid}\n'
    if m.is_file():
        have = m.read_text(errors='replace')
        if have != want: raise BuildError(f'the {what} folder {d} is this script\'s {" ".join(have.split(chr(9))[1:]).strip() or "(unreadable mark)"} folder, not the {what} folder of {tid}: give another folder')
        return
    if d.exists():
        if not d.is_dir(): raise BuildError(f'the {what} folder {d} is not a folder')
        if not adopt and [e for e in d.iterdir() if e.name != '.DS_Store']:
            raise BuildError(f'the {what} folder {d} is not empty and this script did not make it ({MARK} is not there): it replaces and deletes folders in it ({"data/" if what == "output" else "tmp/, aoc/, tools/, shader/pkg/"} ...); give a new or empty folder')
    d.mkdir(parents=True, exist_ok=True); m.write_text(want)


def apart(a, b):   # neither folder is the other or under it (within: by identity, and by name below what exists)
    return not (within(a, b) or within(b, a))


ENV_LINE_MAX = 4095   # front_android.c ENV_LINE_MAX: the phone ignores a longer line, in red


def env_pairs(path):
    """An --env-file's switches read by the phone's rule (front_android.c env_lines; dm/tools/tests/test_nsp_apk.py runs that function on the same lines): lines end at \n only; spaces and
    tabs are cut at a line's start, at its end (with \r) and before the first =; a line starting with # is a comment; a line without = or without a name is skipped; a name set twice keeps
    its first value (the phone skips a name already set). A line over ENV_LINE_MAX bytes, a NUL byte or text that is not UTF-8 stops the build: the phone would ignore such a line, so the
    APK never carries one. Returns ({name: value} in file order, [names set again])"""
    pairs, again = {}, []
    if not path: return pairs, again
    p = Path(os.path.expanduser(path))
    try: b = p.read_bytes()
    except OSError as e: raise BuildError(f'cannot read the environment file (--env-file): {e}') from e
    for n, raw in enumerate(b.split(b'\n'), 1):
        why = f'is longer than {ENV_LINE_MAX} bytes' if len(raw) > ENV_LINE_MAX else 'has a NUL byte' if b'\0' in raw else None
        if why: raise BuildError(f'the environment file {p}: line {n} {why}, which the phone ignores (front_android.c env_lines): shorten or remove it')
    try: text = b.decode('utf-8')
    except UnicodeDecodeError as e: raise BuildError(f'the environment file {p} is not UTF-8 text ({e})') from e
    for ln in text.split('\n'):
        s = ln.lstrip(' \t').rstrip(' \t\r')
        k, eq, v = s.partition('=')
        k = k.rstrip(' \t')
        if not s or s.startswith('#') or not eq or not k: continue
        if k in pairs: again.append(k)
        else: pairs[k] = v
    return pairs, again


def env_txt(pairs, lang):   # the APK's env.txt: the --env-file switches, then the game language unless they set it
    return ('# the build\'s defaults (scripts/build_apk_from_nsp.py); the phone\'s files/env.txt overrides them\n' + ''.join(f'{k}={v}\n' for k, v in pairs.items())
            + ('' if 'DM_LANG' in pairs else f'DM_LANG={lang}\n'))


def parse_args(argv):
    ap = argparse.ArgumentParser(prog='build_apk_from_nsp.py', description='Build an Android APK and a game-data folder from a game\'s NSPs (docs/android/PLAN.md Phase 3).',
                                 epilog='example: python3 -I scripts/build_apk_from_nsp.py ~/Games/StardewValley -k ~/keys/prod.keys')
    ap.add_argument('nsp_dir', help='folder holding one game\'s NSPs (the base game, an update, DLC)')
    ap.add_argument('-k', '--keys', required=True, help='path of prod.keys (never printed or copied)')
    ap.add_argument('-p', '--package', help='version to pack, e.g. v1310720 (default: the newest in the folder; it must be one the game profile registers)')
    ap.add_argument('--work', help='work folder (default: <NSP folder>/<title ID>_build, the iOS script\'s; this script\'s own outputs go to its android/)')
    ap.add_argument('-o', '--out', help='output folder for the APK and the game-data folder (default: <NSP folder>/<title ID>_android)')
    ap.add_argument('--app-name', help='the app\'s label (default: the game name in the base game\'s control data)')
    ap.add_argument('--app-id', help='the app ID (default: org.nxnative.t<title ID in lowercase>)')
    ap.add_argument('-l', '--language', help='game language, a code or a name (default: English); written to the APK\'s env.txt as DM_LANG, which the phone\'s files/env.txt overrides')
    ap.add_argument('--env-file', help='environment switches baked into the APK\'s env.txt (one KEY=VALUE per line; the phone\'s files/env.txt overrides them)')
    ap.add_argument('--validation-layer', default=os.environ.get('VALIDATION_LAYER'), help='an arm64 libVkLayer_khronos_validation.so to pack (default: VALIDATION_LAYER; DM_VK_VALIDATION=1 turns it on)')
    ap.add_argument('--refresh-icon', action='store_true', help='fetch the online cover again instead of the cached one')
    ap.add_argument('--no-online-icon', action='store_true', help='do not fetch the cover online: the extraction\'s icon.jpg')
    ap.add_argument('--allow-damaged', action='store_true', help='a damaged NSP: write the files that fail the NCA hash check as read and go on')
    ap.add_argument('--progress', choices=('auto', 'bar', 'lines', 'json', 'off'), help='stage lines on the console, as build_ipa_from_nsp.py\'s (default auto)')
    return ap.parse_args(argv)


def main(args):
    take(ipa)
    PROG = ipa.PROG; PROG.mode = ipa.progress_mode(args.progress or 'auto')
    ipa.ALLOW_DAMAGED = args.allow_damaged or ipa.ALLOW_DAMAGED
    nsp_dir, keys = Path(os.path.expanduser(args.nsp_dir)).resolve(), Path(os.path.expanduser(args.keys)).resolve()
    if not nsp_dir.is_dir(): raise BuildError(f'not a folder: {nsp_dir}')
    if not keys.is_file(): raise BuildError(f'no such file: {keys}')
    sys.path.insert(0, str(REPO / 'dm/games')); import game as gamemod, profile_guess as pg
    sys.path.insert(0, str(REPO / 'dm/tools')); import nacp
    ipa.pg, ipa.nacp = pg, nacp   # the module globals the iOS script's main sets for the stages that read them
    want_lang = ipa.language_code(args.language) if args.language else 'en-US'
    if not want_lang: raise BuildError(f'unknown game language {args.language!r}; give one of {ipa.language_list([c for c, _ in nacp.LANGUAGES])}, by code or by name')
    env = dict(os.environ, PATH=os.pathsep.join([str(Path(sys.executable).parent)] + ipa.TOOL_PATHS + [os.environ.get('PATH', '/usr/bin:/bin:/usr/sbin:/sbin')]))   # python3 = this Python for build.sh
    env.setdefault('J', str(os.cpu_count() or 8))
    py = sys.executable
    envs, again = env_pairs(args.env_file)
    if args.language and envs.get('DM_LANG', want_lang) != want_lang: raise BuildError(f'-l asks for {want_lang} and the environment file sets DM_LANG={envs["DM_LANG"]}: give one of them')
    app, fam = ipa.scan_nsps(nsp_dir)
    g = gamemod.by_title(app)
    if g is None: raise BuildError(f'no game profile for {app:016X} in dm/games: the Android build takes only registered profiles for now (the host\'s CMake build reads dm/games/<id>/game.tsv); '
                                   'build_ipa_from_nsp.py surveys a new game and makes a provisional profile')
    if g.dir.resolve() != (REPO / 'dm/games' / g.id).resolve(): raise BuildError(f'game profile {g.id} is at {g.dir}, outside dm/games (DM_GAMES_DIR): the Android host\'s CMake build reads dm/games only')
    say(f'game profile: {g.id} ({g.get("name", "")})')
    tid = f'{app:016X}'
    # the folders this script writes and deletes in, checked before anything is written: none in a repository or holding links, out and work apart, each new or empty or marked by this script
    work = Path(os.path.expanduser(args.work)).resolve() if args.work else nsp_dir / f'{g.id}_build'   # the iOS script's (named by the profile; its NSZ folder by the title)
    out = Path(os.path.expanduser(args.out)).resolve() if args.out else nsp_dir / f'{g.id}_android'
    nsz = [p for p in [fam['base']] + [p for _, p in fam['updates']] + [p for _, _, p in fam['dlc']] if p.suffix in ipa.COMPRESSED]
    for what, d in [('output', out), ('work', work)] + ([('NSZ conversion', nsz_folder(nsp_dir, app))] if nsz else []):   # the NSZ folder is shared with the iOS script (nsz_folder)
        check_folder(d, what)
    if not apart(out, work): raise BuildError(f'the output folder {out} and the work folder {work} must not be one in the other: give two separate folders')
    claim(out, 'output', g.id); claim(work, 'work', g.id, adopt=not args.work)
    fam = ipa.decompress_nsz(nsp_dir, app, fam, keys, env)
    builds, need = g.builds(), g.words('shader_versions')
    have = ipa.nsp_versions(fam, builds)
    pkg_ver = args.package or max(have, key=ipa.vnum)
    if pkg_ver not in have: raise BuildError(f'the version to pack (-p) is not in the folder, which holds {", ".join(sorted(have, key=ipa.vnum))}')
    if pkg_ver not in builds or not all(v in have for v in need):
        raise BuildError(f'{pkg_ver} is not a version game profile {g.id} registers with all of its shader versions ({", ".join(need)}) in the folder: the iOS script packs such a version from a profile derived '
                         f'outside the repository, which the Android host\'s CMake build cannot read yet; pack one of {", ".join(v for v in builds if v in have and all(n in have for n in need)) or "none here"}')
    versions = need + ([pkg_ver] if pkg_ver not in need else [])
    app_id = check_app_id(args.app_id or app_id_of(app))
    aw, logs = work / 'android', work / 'android/logs'
    say(f'app ID: {app_id}; output: {out}; work folder: {work}')
    xlate = REPO / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'
    env.update(DM_GAME=g.id, DM_ROOT=str(work), WORK=str(work / 'tmp'))
    sdk = check_prereqs(env, xlate)
    vl = Path(os.path.expanduser(args.validation_layer)).resolve() if args.validation_layer else None
    if vl and not vl.is_file(): raise BuildError(f'no validation layer at {vl}')
    picks = ipa.pick_nsps(fam, have, versions)
    ex = work / 'extract'; ex.mkdir(parents=True, exist_ok=True); (work / 'tmp').mkdir(exist_ok=True); logs.mkdir(parents=True, exist_ok=True)
    gb = sum({p.stat().st_size for b, u in picks.values() for p in (b, u) if p}) / 2**30 + 3   # extraction + rewrite + APK + slack (the data folder is clones)
    if ipa.free_gb(work) < gb and not all((ex / v).exists() for v in versions): raise BuildError(f'less than {gb:.0f} GB free on the volume of {work}')
    PROG.plan(1 + len(versions) + bool(fam['dlc']) + 7)
    PROG.stage('build nx_extract'); nx = ipa.build_nx_extract(REPO, work, logs, env); PROG.end()
    dirs, clone = {}, None
    for ver in versions:   # the base game is extracted alone first; later updates clone against its RomFS
        b, u = picks[ver]
        dirs[ver] = ipa.extract(nx, keys, ex, ver, b, u, clone, logs, g)
        if u is None and clone is None and ipa.can_clone(ex): clone = dirs[ver] / 'RomFS'
    bad = [f'{v}: {x}' for v in versions for x in ipa.build_mismatch(dirs[v], v, g)]
    if bad: raise BuildError(f'not the builds game profile {g.id} registers ({"; ".join(bad)}): the iOS script packs them from a derived profile, which the Android build cannot read yet')
    game = dirs[pkg_ver]
    src = pg.build_sources(game / 'ExeFS', game / 'RomFS', g.words('modules'))
    if src['graphics'][0] != 'nvn': raise BuildError(f'{src["graphics"][1]}: the Android host replays NVN on Vulkan; Vulkan and OpenGL games are Phase 4 (docs/android/PLAN.md)')
    glslc, uedec, oodle_on = src['glslc_capture'][0], src['ue_decode'][0], src['oodle_packs'][0] or src['ue_legacy'][0]
    for on, k in ((glslc, 'glslc_capture'), (uedec, 'ue_decode'), (src['oodle_packs'][0], 'oodle_packs'), (src['ue_legacy'][0], 'ue_legacy')):
        if on: say(src[k][1])
    aoc = ipa.extract_aoc(nx, keys, REPO, work, app, fam, logs)
    base = next((dirs[v] for v in versions if picks[v][1] is None), dirs[versions[0]])
    app_name = args.app_name or nacp.name(base / 'control.nacp')
    if not app_name: raise BuildError(f'no game name in {base / "control.nacp"} (an older extraction did not save it: delete extract/ and run again; or the name has a character nacp.py does not take, '
                                      'such as U+3000 or U+00A0: give --app-name)')
    app_version = nacp.version(game / 'control.nacp') or nacp.version(base / 'control.nacp') or '1.0'
    langs = nacp.languages(game / 'control.nacp') or nacp.languages(base / 'control.nacp')
    if 'DM_LANG' in envs: lang = envs['DM_LANG']; lang_note = f'{lang} (DM_LANG of the environment file)'
    else:
        lang = ipa.game_language(want_lang, bool(args.language), langs)
        lang_note = f'{lang} ({dict(nacp.LANGUAGES)[lang]})' + (f', the game has no {want_lang}' if lang != want_lang else '')
    say(f'app name: {app_name}; version: {app_version}; game language: {lang_note} (DM_LANG=<code> in the phone\'s files/env.txt changes it)')
    if again: say(f'the environment file sets {", ".join(dict.fromkeys(again))} more than once: the first value is kept, as the phone reads env.txt')

    patched = aw / f'patched_{pkg_ver}'
    rewrite_android(py, game / 'ExeFS', patched, env, logs, g)
    ipa.chain_header(py, REPO, env, logs)
    mac_patched = work / f'patched_{pkg_ver}'   # the Mac host's modules, for the stages that boot it (the iOS script's rewrite and its folder, so either script reuses the other's)
    if glslc or uedec or oodle_on: PROG.add(1); ipa.rewrite(py, REPO, game / 'ExeFS', mac_patched, env, logs, g)
    if glslc: PROG.add(1); ipa.capture_glslc(REPO, {v: dirs[v] for v in need}, pkg_ver, mac_patched, work, env, logs, g)
    if uedec: PROG.add(1); ipa.decode_ue_shaders(REPO, {v: dirs[v] for v in need}, pkg_ver, mac_patched, work, env, logs, g)
    oodle = (lambda: {'DM_OODLE_HOST': str(ipa.build_mac_host(REPO, work / 'tools/dmhost', env, logs, g)), 'DM_OODLE_MODULES': str(mac_patched)}) if oodle_on else None
    sh = work / 'shader'
    pkey = ipa.split_shaders(py, REPO, [dirs[v] / 'RomFS' for v in need], [dirs[v] / 'ExeFS/main' for v in need], sh / 'pkg', env, logs, g, False, oodle)
    vkdir, chain = vk_shaders(py, sh / 'pkg', aw / 'vkcache', pkey, env, logs, g, xlate)

    PROG.stage('package', step='font')
    font = ext_font(py, aw / 'font', env, logs)
    PROG.set(step='modules and shaders')
    stamp, nfiles, nbytes, nmod, nbad = make_payload(aw / 'payload', patched, vkdir, chain, g.table('shader_map.tsv'), font, env_txt(envs, lang))
    PROG.end(f'stamp {stamp}: {nfiles} files, {ipa.amount(nbytes, "B")}; {nmod} shader modules' + (f' ({nbad} not ok)' if nbad else ''))

    PROG.stage('icon and label', step='cover')
    online, how = (None, 'not fetched (--no-online-icon)') if args.no_online_icon else fetch_icon(tid, aw / 'icon-cache', args.refresh_icon)
    # the sources in order, each checked as it is made into the icon (make_icon_res: a cover that is not a whole square JPEG or PNG falls through to the next): the online cover, then the
    # extraction's icon.jpg (the packed version's, then the base game's), then none (the system's default icon)
    srcs = ([('online', f'online cover ({how})', online)] if online else []) + [('extraction', f'the extraction\'s icon.jpg ({d.name})', d / 'icon.jpg') for d in dict.fromkeys((game, base)) if (d / 'icon.jpg').is_file()]
    res = aw / 'res'
    used, skipped = icon_from(srcs, res)
    if not online: skipped.insert(0, f'online: {how}')
    if not used: say(f'[RED] icon: no cover ({"; ".join(skipped) or "none online and no icon.jpg in the extraction"}; an older extraction did not save icon.jpg: delete extract/ and run again): the app gets the system\'s default icon')
    man = aw / 'AndroidManifest.xml'
    man.write_text(apk_manifest((REPO / 'dm/android/app/AndroidManifest.xml').read_text(), app_id, app_name, bool(used)))
    PROG.end(f'label "{app_name}", ' + (f'icon from {"online" if used == "online" else "the extraction"}' if used else 'default icon'))

    PROG.stage('APK', step='build_app.sh (host build, javac, aapt2, zip, sign)')
    if ipa.free_gb(out) < 2: raise BuildError(f'less than 2 GB free on the volume of {out}')
    out.mkdir(parents=True, exist_ok=True)
    apk = out / (Path(g.get('ipa') or f'{tid}.ipa').stem + '.apk')
    benv = dict(env)
    if vl: benv['VALIDATION_LAYER'] = str(vl)
    else: benv.pop('VALIDATION_LAYER', None)
    cmd = [REPO / 'dm/android/app/build_app.sh', g.id, '--gpu', 'vkn', '--manifest', man, '--assets', aw / 'payload', '--out', apk,
           '--version-code', str(max(1, ipa.vnum(pkg_ver))), '--version-name', app_version] + (['--res', res] if used else [])
    ipa.run(['bash'] + cmd, logs / 'build_app.log', env=benv)
    if not apk.is_file(): raise BuildError(f'no APK at {apk}')
    PROG.end(f'{apk.name}, {ipa.amount(apk.stat().st_size, "B")}' + (f'; validation layer {vl.name}' if vl else ''))

    PROG.stage('game data')
    data, how = data_folder(out, game / 'RomFS', aoc, work / 'aoc.key')
    PROG.end(f'{data} ({how})')
    ipa.rmtree(work / 'tmp')
    q = lambda v: shlex.quote(str(v))
    ext = f'/sdcard/Android/data/{app_id}/files/'
    say(f'done: {apk} ({apk.stat().st_size / 2**20:.0f} MB), app ID {app_id}, label "{app_name}". It contains the game\'s code: never share or upload it.')
    say(f'game data (not in the APK): {data}. With the phone attached (adb), install the APK, start it once so that it makes its external files directory, then push the data:')
    for c in [f'adb install -r {q(apk)}', f'adb shell am start -W -n {app_id}/org.nxnative.host.HostActivity', f'adb shell am force-stop {app_id}',
              f'adb push --sync {q(data / "romfs")} {ext}'] + ([f'adb push --sync {q(data / "aoc")} {ext}'] if aoc else []):
        PROG.say('  ' + c)
    say(f'the APK\'s version code is {max(1, ipa.vnum(pkg_ver))} ({pkg_ver}): when the phone has a later one (another version packed), adb install -r refuses it as a downgrade; '
        'adb install -r -d takes it (the app is debuggable) and keeps the app\'s data')
    say(f'then start "{app_name}" on the phone. The first start unpacks the package ({ipa.amount(nbytes, "B")}) into the app\'s files/pkg; files/env.txt (adb shell run-as {app_id}) takes switches such as DM_VK_DRIVER=842')


def cli(argv):   # the exit code: 0 done, 1 error, 130 interrupted
    PROG = ipa.PROG
    try: main(parse_args(argv))
    except BuildError as e: PROG.close(ok=False); PROG.say(f'[RED] {e}'); return 1
    except KeyboardInterrupt: PROG.close(ok=False); PROG.say('[RED] interrupted'); return 130
    finally: PROG.close(ok=False)
    return 0


if __name__ == '__main__':
    sys.exit(cli(sys.argv[1:]))
