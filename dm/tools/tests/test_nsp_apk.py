"""NSP script for Android (scripts/build_apk_from_nsp.py): the guard over what it takes from build_ipa_from_nsp.py, the app ID and the label as aapt2 reads them, the game's manifest made from
dm/android/app/AndroidManifest.xml, the APK's package (index, stamp, the shader modules decompressed and checked against their reflection, only the shader build's own output taken), the
environment file as the phone reads it, the folders it writes in (never a repository, a link or a folder it was not given), the data folder's reuse, the rewrite stopping on a failure, the
icon (the cover fetched as untrusted data, cut or wrong images falling through to the next source, the background colour from its edge), and the Vulkan shader chain (its version against the
host's, own_compilers_vk.py taking this Mac's spirv-cross).
No game files, keys or phone needed (temporary files only in a temporary folder); the aapt2 checks run only where the Android SDK is installed, the sips checks only on macOS."""
import collections
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock
import zlib

REPO = Path(__file__).resolve().parents[3]
SCRIPT = REPO / 'scripts/build_apk_from_nsp.py'
spec = importlib.util.spec_from_file_location('build_apk_from_nsp', SCRIPT)
apk = importlib.util.module_from_spec(spec); spec.loader.exec_module(apk)
SDK = Path(os.environ.get('ANDROID_HOME') or os.path.expanduser('~/Library/Android/sdk'))
AAPT2, JAR = SDK / 'build-tools/36.1.0/aapt2', SDK / 'platforms/android-36/android.jar'
HAVE_SDK = AAPT2.exists() and JAR.exists()
HAVE_SIPS = shutil.which('sips') is not None
HAVE_GIT = shutil.which('git') is not None


def png(w, h, rgb):   # a minimal valid RGB PNG, one colour
    row = b'\0' + bytes(rgb) * w
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(row * h)) + chunk(b'IEND', b'')


def noisy_png(n, seed=1):   # an n x n PNG of noise: cut short, sips still reads and converts it (grey where it ends), unlike a flat one, so only the end marker tells
    import random; r = random.Random(seed)
    return _png_rows(n, n, [b'\0' + r.randbytes(n * 3) for _ in range(n)])


def _png_rows(w, h, rows):
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b'')


def bmp(px, top_down=True, bpp=24):   # px: rows of (r, g, b), top row first
    h, w, n = len(px), len(px[0]), bpp // 8
    stride = (w * n + 3) & ~3
    rows = [b''.join(bytes((b, g, r) + ((255,) if n == 4 else ())) for r, g, b in row).ljust(stride, b'\0') for row in px]
    data = b''.join(rows if top_down else rows[::-1])
    return b'BM' + struct.pack('<IHHI', 54 + len(data), 0, 0, 54) + struct.pack('<IiiHHIIiiII', 40, w, -h if top_down else h, 1, bpp, 0, len(data), 2835, 2835, 0, 0) + data


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.said = []
        for target, name, value in ((apk.ipa, 'say', self.said.append), (apk.ipa, 'PROG', mock.MagicMock())):
            cm = mock.patch.object(target, name, value); cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)


class Take(unittest.TestCase):   # not Base: its stand-in say() would itself trip the guard
    def fake(self):   # a module with what TAKE asks for, each function with exactly its parameters, spelled as TAKE spells them (a def's own syntax)
        m = types.SimpleNamespace()
        for name, params in apk.TAKE.items():
            if params is None: setattr(m, name, object()); continue
            ns = {}; exec(f'def f({", ".join(params)}): pass', ns); setattr(m, name, ns['f'])
        return m

    def test_real_script(self):   # build_ipa_from_nsp.py as it is in the repository
        apk.take(apk.ipa)

    def test_fake_ok(self):
        apk.take(self.fake())

    def test_missing_name(self):
        m = self.fake(); del m.extract
        with self.assertRaisesRegex(apk.BuildError, 'extract is gone'): apk.take(m)

    def test_changed_parameters(self):
        m = self.fake(); m.extract = lambda nx, keys, ex, name, base, update, cmp_dir, logs, g: None
        with self.assertRaisesRegex(apk.BuildError, 'does not begin with'): apk.take(m)

    def test_parameter_kinds(self):   # the same names, but a call by position or by keyword would break: env made keyword-only or positional-only, *items made a plain list
        for name, src in (('run', 'def f(cmd, log, *, env=None, cwd=None, on_line=None): pass'), ('run', 'def f(cmd, log, env=None, /, cwd=None, on_line=None): pass'),
                          ('fingerprint', 'def f(items, env=None): pass'), ('fingerprint', 'def f(*items, **env): pass')):
            m = self.fake(); ns = {}; exec(src, ns); setattr(m, name, ns['f'])
            with self.subTest(src=src), self.assertRaisesRegex(apk.BuildError, f'{name}.* does not begin with'): apk.take(m)

    def test_new_parameter(self):   # one without a default would break the calls; one with a default does not
        m = self.fake(); m.reused = lambda keyfile, key, strict: None
        with self.assertRaisesRegex(apk.BuildError, 'without a default'): apk.take(m)
        m.reused = lambda keyfile, key, strict=False: None
        apk.take(m)
        ns = {}; exec('def f(*items, env, strict): pass', ns); m.fingerprint = ns['f']   # keyword-only too
        with self.assertRaisesRegex(apk.BuildError, 'without a default'): apk.take(m)

    def test_not_a_function(self):
        m = self.fake(); m.run = 'run'
        with self.assertRaisesRegex(apk.BuildError, 'no longer a function'): apk.take(m)


class Names(Base):
    def test_app_id(self):   # the iOS form is not a valid Android package name: a part may not start with a digit
        self.assertEqual(apk.app_id_of(0x0100E65002BB8000), 'org.nxnative.t0100e65002bb8000')
        self.assertEqual(apk.check_app_id('org.nxnative.t0100e65002bb8000'), 'org.nxnative.t0100e65002bb8000')
        for bad in ('org.nxnative.0100E65002BB8000', 'org', 'org..x', 'org.nx-native.x', '.org.x', 'org.x.'):
            with self.assertRaises(apk.BuildError): apk.check_app_id(bad)

    def test_label_escapes(self):
        self.assertEqual(apk.label_attr('Stardew Valley'), 'Stardew Valley')
        self.assertEqual(apk.label_attr('Luigi\'s "M" & <3 \\ @x'), 'Luigi&apos;s &quot;M&quot; &amp; &lt;3 \\\\ @x')
        self.assertEqual(apk.label_attr('@home'), '\\@home')
        self.assertEqual(apk.label_attr('?attr/x'), '\\?attr/x')

    def test_label_characters(self):   # Japanese titles' spaces are kept; control and format characters and line separators are not
        for ok in ('ポケモン\u3000スカーレット', 'A\u00a0B', '  A   B  ', 'Pokémon™'):
            with self.subTest(ok=ok): self.assertEqual(apk.label_attr(ok), ok)
        for bad in ('', '   ', '\u3000', 'a\nb', 'a\tb', 'a\x7fb', 'A\u200bB', 'A\u202eB', 'A\u2028B', 'A\u2029B'):
            with self.subTest(bad=bad), self.assertRaises(apk.BuildError): apk.label_attr(bad)


class Manifest(Base):
    T = (REPO / 'dm/android/app/AndroidManifest.xml').read_text()

    def test_from_the_template(self):
        m = apk.apk_manifest(self.T, 'org.nxnative.t0100e65002bb8000', 'Stardew Valley', True)
        self.assertIn('package="org.nxnative.t0100e65002bb8000"', m)
        self.assertIn('android:label="Stardew Valley"', m)
        self.assertIn('android:icon="@mipmap/nx_icon" android:roundIcon="@mipmap/nx_icon"', m)
        self.assertNotIn('org.nxnative.host"', m); self.assertNotIn('<!--', m)
        self.assertIn('android:name="org.nxnative.host.HostActivity"', m)   # the class keeps its package
        self.assertNotIn('android:icon', apk.apk_manifest(self.T, 'org.nxnative.tx', 'X', False))

    def test_every_configuration_change_kept(self):   # a change the activity does not take recreates it, which ends the process (front_android.c onDestroy); minSdk 35 has them all
        for c in ('locale', 'orientation', 'screenSize', 'uiMode', 'density', 'fontScale', 'fontWeightAdjustment', 'grammaticalGender'):
            self.assertRegex(self.T, rf'android:configChanges="[^"]*\b{c}\b')

    def test_template_changes_stop_it(self):
        with self.assertRaisesRegex(apk.BuildError, '2 <application>'): apk.apk_manifest(self.T.replace('</manifest>', '<application/></manifest>'), 'org.nxnative.tx', 'X', False)
        with self.assertRaisesRegex(apk.BuildError, '0 android:label'): apk.apk_manifest(self.T.replace('android:label="nxnative"', ''), 'org.nxnative.tx', 'X', False)
        with self.assertRaisesRegex(apk.BuildError, 'already names an icon'): apk.apk_manifest(self.T.replace('<application ', '<application android:icon="@mipmap/x" '), 'org.nxnative.tx', 'X', True)

    @unittest.skipUnless(HAVE_SDK, 'no Android SDK (build-tools 36.1.0, platform 36)')
    def test_aapt2_reads_it(self):   # the label as the APK holds it, special characters and all, the icon resources link, and aapt2 takes the manifest's configChanges
        res = self.tmp / 'res'
        for rel, text in apk.ICON_XML.items(): (res / rel).parent.mkdir(parents=True, exist_ok=True); (res / rel).write_text(text)
        (res / 'drawable-nodpi').mkdir(); (res / 'drawable-nodpi/nx_cover.png').write_bytes(png(8, 8, (10, 20, 30)))
        (res / 'values').mkdir(); (res / 'values/nx_icon.xml').write_text('<resources><color name="nx_icon_bg">#0a141e</color></resources>\n')
        for name in ('Luigi\'s "M" & <3 \\ @x ?y Pokémon™', 'ポケモン\u3000スカーレット', ' A\u00a0B  C '):
            with self.subTest(name=name):
                man = self.tmp / 'AndroidManifest.xml'; man.write_text(apk.apk_manifest(self.T, 'org.nxnative.t0100e65002bb8000', name, True))
                subprocess.run([AAPT2, 'compile', '--dir', res, '-o', self.tmp / 'res.zip'], check=True, capture_output=True)
                subprocess.run([AAPT2, 'link', '-o', self.tmp / 'o.apk', '--manifest', man, '-I', JAR, self.tmp / 'res.zip'], check=True, capture_output=True)
                t = subprocess.run([AAPT2, 'dump', 'xmltree', '--file', 'AndroidManifest.xml', self.tmp / 'o.apk'], check=True, capture_output=True, text=True).stdout
                self.assertIn(f':label(0x01010001)="{name}"', t)
                self.assertRegex(t, r':configChanges\(0x0101001f\)=0x[0-9a-f]*[89a-f]...\b')   # grammaticalGender is 0x8000
                b = subprocess.run([AAPT2, 'dump', 'badging', self.tmp / 'o.apk'], check=True, capture_output=True, text=True).stdout
                self.assertIn("package: name='org.nxnative.t0100e65002bb8000'", b)
                self.assertRegex(b, r"application: label='.*' icon='res/[^']*nx_icon[^']*\.xml'")

    @unittest.skipUnless(HAVE_SDK, 'no Android SDK (build-tools 36.1.0, platform 36)')
    def test_aapt2_refuses_the_ios_form(self):   # why the app ID is not org.nxnative.<title ID>
        man = self.tmp / 'm.xml'; man.write_text(self.T.replace('package="org.nxnative.host"', 'package="org.nxnative.0100E65002BB8000"'))
        r = subprocess.run([AAPT2, 'link', '-o', self.tmp / 'o.apk', '--manifest', man, '-I', JAR], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0); self.assertIn('not a valid Android package name', r.stdout + r.stderr)


class Payload(Base):
    def tree(self, files):
        d = self.tmp / 'nx'; shutil.rmtree(d, ignore_errors=True)
        for rel, data in files.items(): (d / rel).parent.mkdir(parents=True, exist_ok=True); (d / rel).write_bytes(data)
        return d

    def test_stamp(self):   # the same files give the same stamp, whatever their times; any change of content or name moves it
        files = {'mods/main.patched.bin': b'\1' * 100, 'mods/main.fixups.tsv': b'f', 'vksh/k_b.spv': b's', 'env.txt': b'DM_LANG=en-US\n'}
        idx, s1 = apk.payload_index(self.tree(files))
        self.assertEqual(idx, '14\tenv.txt\n1\tmods/main.fixups.tsv\n100\tmods/main.patched.bin\n1\tvksh/k_b.spv\n')
        self.assertRegex(s1, r'^[0-9a-f]{16}$')
        d = self.tree(files); os.utime(d / 'env.txt', (1, 1)); (d / 'stamp').write_text('x'); (d / 'index.txt').write_text('y')   # its own files do not count
        self.assertEqual(apk.payload_index(d)[1], s1)
        self.assertNotEqual(apk.payload_index(self.tree({**files, 'vksh/k_b.spv': b't'}))[1], s1)
        self.assertNotEqual(apk.payload_index(self.tree({**{k: v for k, v in files.items() if k != 'env.txt'}, 'env2.txt': files['env.txt']}))[1], s1)

    def test_names_the_front_end_takes(self):   # front_android.c path_ok: [A-Za-z0-9._-] parts, none starting with '.'
        for bad in ('mods/.hidden', 'mods/a b', 'mods/ü'):
            with self.assertRaisesRegex(apk.BuildError, 'package file'): apk.payload_index(self.tree({bad: b'x'}))

    def vkdir(self, spv, chain='c0ffee', zst=False, status='ok', **over):
        d = self.tmp / 'b-vk'; shutil.rmtree(d, ignore_errors=True); d.mkdir()
        r = {'key': 'k_b', 'chain': chain, 'status': status, 'spv_bytes': len(spv), 'spv_sha256': hashlib.sha256(spv).hexdigest()[:16], **over}
        (d / 'k_b.json').write_text(json.dumps(r)); (d / '_b_build.json').write_text('{}'); (d / 'inputs.tsv').write_text('game\tX\ninputs\tY\n')
        if zst:
            z = next((z for z in map(self.importable, ('compression.zstd', 'backports.zstd')) if z), None)
            if z is None: self.skipTest('no zstd module in this Python')
            (d / 'k_b.spv.zst').write_bytes(z.compress(spv))
        else: (d / 'k_b.spv').write_bytes(spv)
        return d

    @staticmethod
    def importable(m):   # the module, or None
        try: return importlib.import_module(m)
        except ImportError: return None

    def test_vk_modules(self):
        spv = struct.pack('<5I', 0x07230203, 0x10300, 0, 1, 0)
        for zst in (False, True):
            with self.subTest(zst=zst):
                out = self.tmp / f'out{zst}'
                self.assertEqual(apk.vk_modules(self.vkdir(spv, zst=zst, status='unsupported'), out, 'c0ffee'), (1, 1))
                self.assertEqual((out / 'k_b.spv').read_bytes(), spv)
                self.assertEqual(sorted(p.name for p in out.iterdir()), ['inputs.tsv', 'k_b.json', 'k_b.spv'])

    def test_vk_modules_refused(self):   # a module that is not what its reflection says, another chain, no stamp
        spv = b'\3\2\x23\7' + b'\0' * 16
        for over, why in (({'spv_bytes': 4}, 'reflection says'), ({'spv_sha256': '0' * 16}, 'reflection says'), ({'chain': 'beef'}, 'the host takes')):
            with self.subTest(why=why), self.assertRaisesRegex(apk.BuildError, why):
                apk.vk_modules(self.vkdir(spv, **over), self.tmp / 'o', 'c0ffee')
        d = self.vkdir(spv); (d / 'inputs.tsv').unlink()
        with self.assertRaisesRegex(apk.BuildError, 'no stamp'): apk.vk_modules(d, self.tmp / 'o2', 'c0ffee')

    def test_make_payload(self):
        patched = self.tmp / 'patched'; patched.mkdir()
        for n, data in (('main.patched.bin', b'\0' * 64), ('main.fixups.tsv', b'f'), ('nro.tsv', b'n'), ('main.key', b'k')): (patched / n).write_bytes(data)   # a key file inside: not packed
        shmap, font = self.tmp / 'shader_map.tsv', self.tmp / 'nintendo_ext.ttf'; shmap.write_text('m'); font.write_bytes(b'ttf')
        spv = b'\3\2\x23\7' + b'\0' * 16
        stamp, n, size, nmod, nbad = apk.make_payload(self.tmp / 'payload', patched, self.vkdir(spv), 'c0ffee', shmap, font, 'DM_LANG=fr\n')
        nx = self.tmp / 'payload/nx'
        self.assertEqual((nx / 'stamp').read_text(), stamp + '\n')
        self.assertEqual(sorted(l.split('\t')[1] for l in (nx / 'index.txt').read_text().splitlines()),
                         ['env.txt', 'font/nintendo_ext.ttf', 'mods/main.fixups.tsv', 'mods/main.patched.bin', 'mods/nro.tsv', 'vksh/inputs.tsv', 'vksh/k_b.json', 'vksh/k_b.spv', 'vksh/shader_map.tsv'])
        self.assertFalse((nx / 'mods/main.key').exists())
        self.assertEqual((n, nmod, nbad), (9, 1, 0))
        self.assertEqual(apk.payload_index(nx)[1], stamp)
        self.assertFalse((self.tmp / 'payload.partial').exists())


class VkShaders(Base):
    """vk_shaders takes only what this run of the shader build wrote: dmsh_build_vk.py's early exits leave the last build's stamped output, which must never be packed or keyed as new"""
    GAME = '0100000000010000'

    def setUp(self):
        super().setUp()
        self.V = apk.vkn_chain(REPO); self.root = self.tmp / 'vkcache'; self.out = self.root / f'b-{self.V}-vk'; self.kf = self.root / f'b-{self.V}-vk.key'; self.builds = 0

    def stage(self, build):   # vk_shaders with build(out) standing in for the shader build (own_compilers_vk.py): it writes what it writes and returns the exit code
        def run(cmd, **kw):
            if cmd[-1] == '--version': return types.SimpleNamespace(returncode=0, stdout=self.V + '\n', stderr='')
            self.assertEqual(Path(cmd[1]).name, 'own_compilers_vk.py'); self.builds += 1
            return types.SimpleNamespace(returncode=build(self.out))
        with mock.patch.object(apk.subprocess, 'run', run), mock.patch.object(apk.ipa, 'fingerprint', lambda *a, **k: 'KEY'), mock.patch.object(apk.ipa, 'profile_files', lambda g: []):
            return apk.vk_shaders(sys.executable, self.tmp / 'pkg', self.root, 'pkg-key', {}, self.tmp / 'logs', types.SimpleNamespace(id=self.GAME), self.tmp / 'xlate/dmsh_xlate')

    def old_build(self, key='OLD'):   # the output of an earlier build, complete and stamped
        self.out.mkdir(parents=True); (self.out / 'inputs.tsv').write_text(f'game\t{self.GAME}\ninputs\told\n')
        (self.out / '_b_build.json').write_text(json.dumps([{'key': 'a', 'st': 'ok', 'err': ''}])); (self.out / 'a_b.json').write_text('{}')
        self.kf.write_text(f'{key}\n1 of 1 modules ok\n')

    @staticmethod
    def wrote(game, statuses, rc=0):
        def build(out):
            out.mkdir(parents=True, exist_ok=True); (out / '_b_build.json').write_text(json.dumps([{'key': f'k{i}', 'st': s, 'err': ''} for i, s in enumerate(statuses)]))
            if game: (out / 'inputs.tsv').write_text(f'game\t{game}\ninputs\tnew\n')
            return rc
        return build

    def test_early_exit_takes_nothing(self):   # the tool lock, spirv-val, another game's output, the answer table: exit 1, nothing written
        self.old_build()
        with self.assertRaisesRegex(apk.BuildError, 'no stamped output of this run'): self.stage(lambda out: 1)
        self.assertFalse((self.out / 'inputs.tsv').exists()); self.assertFalse((self.out / '_b_build.json').exists()); self.assertFalse(self.kf.exists())

    def test_status_list_without_stamp(self):   # not every shader built: the run's own status list, but no stamp
        self.old_build()
        with self.assertRaisesRegex(apk.BuildError, 'no stamped output of this run'): self.stage(self.wrote(None, ['ok', 'translation failed'], 1))
        self.assertFalse(self.kf.exists())

    def test_other_game(self):
        with self.assertRaisesRegex(apk.BuildError, 'stamped for game 0100000000020000'): self.stage(self.wrote('0100000000020000', ['ok']))

    def test_counts_from_the_status_list(self):   # more unsupported than ok (the summary line then begins with unsupported): the counts still come out right
        out, v = self.stage(self.wrote(self.GAME, ['unsupported', 'ok', 'unsupported'], 1))
        self.assertEqual((out, v), (self.out, self.V))
        red = [s for s in self.said if s.startswith('[RED]')]
        self.assertEqual(len(red), 1); self.assertIn('1 of 3 modules ok (2 unsupported)', red[0])
        self.assertEqual(self.kf.read_text(), 'KEY\n1 of 3 modules ok\n')

    def test_reused(self):
        self.old_build('KEY')
        self.assertEqual(self.stage(lambda out: self.fail('built again')), (self.out, self.V)); self.assertEqual(self.builds, 0)


class Rewrite(Base):
    def test_a_failure_stops_the_others(self):   # patch_nso.py failing on one module terminates the rewrites still running, as build_ipa_from_nsp.py's rewrite does
        exe = self.tmp / 'ExeFS'; exe.mkdir()
        for m in ('main', 'sdk'): (exe / m).write_bytes(b'x')
        procs = {}
        class Proc:
            def __init__(self, cmd, **kw):
                self.cmd, self.m = cmd, cmd[-1]; self.returncode = None; self.terminated = self.waited = False; procs[self.m] = self
            def poll(self):
                if self.m == 'main': self.returncode = 3
                return self.returncode
            def terminate(self): self.terminated = True; self.returncode = -15
            def wait(self): self.waited = True; return self.returncode
        g = types.SimpleNamespace(words=lambda k: ['main', 'sdk'])
        with mock.patch.object(apk.subprocess, 'Popen', Proc), mock.patch.object(apk.ipa, 'collect_nros', lambda *a: {}), mock.patch.object(apk.ipa, 'fingerprint', lambda *a, **k: 'K'), \
             mock.patch.object(apk.ipa, 'patch_files', lambda *a: []):
            with self.assertRaisesRegex(apk.BuildError, 'exit code 3 on main'): apk.rewrite_android(sys.executable, exe, self.tmp / 'android/patched_v1', {}, self.tmp / 'logs', g)
        self.assertEqual(sorted(procs), ['main', 'sdk'])
        self.assertTrue(procs['sdk'].terminated and procs['sdk'].waited); self.assertFalse(procs['main'].terminated)
        self.assertEqual(procs['main'].cmd[1:4], [str(apk.REPO / 'dm/tools/patch_nso.py'), '--target', 'android'])
        self.assertFalse((self.tmp / 'android/patched_v1').exists()); self.assertFalse((self.tmp / 'android/patched_v1.key').exists())


class EnvFile(Base):
    """The build reads an --env-file as the phone reads env.txt, and bakes only what the phone reads the same way: the phone side is front_android.c's own env_lines, compiled for this Mac
    with a stand-in setenv that prints each assignment it makes"""
    reader = None

    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'): return
        src = (REPO / 'dm/android/front/front_android.c').read_text()
        i = src.index('static char g_env_keys['); j = src.index('\n}\n', src.index('static void env_lines(')) + 3
        cls.dir = tempfile.mkdtemp(); c = Path(cls.dir) / 'env_lines.c'
        c.write_text('#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <sys/types.h>\n'
                     'static const char *const g_ctor_env[] = {"DM_NVN_LOG"};\n'
                     'static int set_and_print(const char *k, const char *v, int o) { if (strcmp(k, "DM_ENV_FILE_KEYS")) printf("%s\\x1f%s\\x1e", k, v); return setenv(k, v, o); }\n'
                     '#define setenv set_and_print\n' + src[i:j] +
                     'int main(int argc, char **argv) { FILE *f = fopen(argv[1], "rb"); if (!f) return 2; env_lines(f, "env.txt", NULL, 0); fclose(f); return 0; }\n')
        r = subprocess.run(['cc', '-o', str(c.with_suffix('')), str(c)], capture_output=True, text=True)
        if r.returncode: raise AssertionError(f'front_android.c env_lines does not compile on its own: {r.stderr[-2000:]}')
        cls.reader = c.with_suffix('')

    @classmethod
    def tearDownClass(cls):
        if cls.reader: shutil.rmtree(cls.dir, ignore_errors=True)

    def phone(self, data):   # [(name, value)] in the order the phone sets them, and its [RED] log lines; nothing set beforehand (an empty environment)
        if not self.reader: self.skipTest('no C compiler (cc)')
        f = self.tmp / 'phone_env.txt'; f.write_bytes(data.encode() if isinstance(data, str) else data)
        r = subprocess.run([str(self.reader), str(f)], env={}, capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return [tuple(x.split('\x1f', 1)) for x in r.stdout.decode().split('\x1e') if x], [l for l in r.stderr.decode().splitlines() if '[RED]' in l]

    LINES = ['# comment', 'A=1', '', 'B= two ', 'A=2', '  C=x=y', 'not a line', '=x', ' \t= x', 'DM_LANG =fr', 'D\t=tab', '  #E=1', 'F=crlf\r', 'G=  ', 'H==', 'I=a\tb',
             'J K=space inside', '\x0bL=vt', 'M=é　', 'N=x\x0cy', 'O= z', 'B=3', 'P=last line, no newline']

    def test_one_rule(self):   # the same lines, the same assignments: blanks before = cut on both sides, the first value of a name kept, lines split at \n only
        f = self.tmp / 'e.env'; f.write_text('\n'.join(self.LINES))
        pairs, again = apk.env_pairs(f)
        self.assertEqual(pairs, {'A': '1', 'B': ' two', 'C': 'x=y', 'DM_LANG': 'fr', 'D': 'tab', 'F': 'crlf', 'G': '', 'H': '=', 'I': 'a\tb', 'J K': 'space inside', '\x0bL': 'vt',
                                 'M': 'é　', 'N': 'x\x0cy', 'O': ' z', 'P': 'last line, no newline'})
        self.assertEqual(again, ['A', 'B'])
        self.assertEqual(self.phone(f.read_bytes()), (list(pairs.items()), []))   # the file as the phone reads it
        self.assertEqual(self.phone(apk.env_txt(pairs, 'en-US')), (list(pairs.items()), []))   # the APK's env.txt as the phone reads it

    def test_language(self):   # the file's DM_LANG is the one written; else the build's
        f = self.tmp / 'e.env'; f.write_text('DM_LANG=fr\nDM_LANG=de\n')
        pairs, _ = apk.env_pairs(f)
        self.assertEqual(apk.env_txt(pairs, 'en-US').count('DM_LANG'), 1)
        self.assertEqual(apk.env_txt({'A': '1'}, 'en-US').splitlines()[1:], ['A=1', 'DM_LANG=en-US'])
        self.assertEqual(apk.env_pairs(None), ({}, []))
        with self.assertRaisesRegex(apk.BuildError, 'cannot read'): apk.env_pairs(self.tmp / 'none.env')
        self.assertEqual(dict(self.phone(apk.env_txt(pairs, 'en-US'))[0])['DM_LANG'], 'fr')

    def test_long_lines(self):   # never read in pieces: a line over 4095 bytes is ignored whole on the phone, in red (a fixed buffer read its rest as a line of its own: EVIL=1), and refused here
        lines = ['A=1', '# ' + 'x' * 4100 + 'EVIL=1', 'B=' + 'y' * 4093, 'C=' + 'z' * 4094, 'D=' + 'w' * 4092 + '\r', 'E=4', 'Z=' + 'q' * 5000]
        got, log = self.phone('\n'.join(lines))
        self.assertEqual(got, [('A', '1'), ('B', 'y' * 4093), ('D', 'w' * 4092), ('E', '4')])
        self.assertEqual(log, [f'boot: [RED] env.txt line {n} is longer than 4095 bytes: ignored' for n in (2, 4, 7)])
        f = self.tmp / 'e.env'; f.write_text('\n'.join(lines))
        with self.assertRaisesRegex(apk.BuildError, r'line 2 is longer than 4095 bytes, which the phone ignores'): apk.env_pairs(f)
        f.write_text('\n'.join(lines[i] for i in (0, 2, 4, 5)))   # without the long lines: what the phone takes from the other file
        self.assertEqual(list(apk.env_pairs(f)[0].items()), got)

    def test_bytes_the_phone_ignores(self):   # a NUL byte: ignored on the phone, in red; refused here, as text that is not UTF-8
        got, log = self.phone(b'A=1\nX=a\0b\nB=2\n')
        self.assertEqual((got, log), ([('A', '1'), ('B', '2')], ['boot: [RED] env.txt line 2 has a NUL byte: ignored']))
        f = self.tmp / 'e.env'
        for data, why in ((b'A=1\nX=a\0b\n', 'line 2 has a NUL byte'), (b'A=\xff\n', 'not UTF-8')):
            f.write_bytes(data)
            with self.subTest(why=why), self.assertRaisesRegex(apk.BuildError, why): apk.env_pairs(f)


class Folders(Base):
    def test_claim(self):
        new = self.tmp / 'a/b'
        apk.claim(new, 'output', 'T1'); self.assertEqual((new / apk.MARK).read_text(), 'build_apk_from_nsp.py\toutput\tT1\n')
        apk.claim(new, 'output', 'T1')   # its own again
        (new / 'data').mkdir(); apk.claim(new, 'output', 'T1')
        with self.assertRaisesRegex(apk.BuildError, 'not the output folder of T2'): apk.claim(new, 'output', 'T2')
        with self.assertRaisesRegex(apk.BuildError, 'not the work folder of T1'): apk.claim(new, 'work', 'T1')
        empty = self.tmp / 'empty'; empty.mkdir(); (empty / '.DS_Store').write_bytes(b'')
        apk.claim(empty, 'work', 'T1'); self.assertTrue((empty / apk.MARK).is_file())

    def test_claim_refuses_a_folder_in_use(self):   # --out ~/Desktop would replace ~/Desktop/data: refused, and nothing written there
        d = self.tmp / 'Desktop'; (d / 'data').mkdir(parents=True); (d / 'data/mine.txt').write_text('keep')
        with self.assertRaisesRegex(apk.BuildError, 'not empty and this script did not make it'): apk.claim(d, 'output', 'T1')
        self.assertEqual(sorted(p.name for p in d.iterdir()), ['data']); self.assertEqual((d / 'data/mine.txt').read_text(), 'keep')
        with self.assertRaisesRegex(apk.BuildError, 'not a folder'): (self.tmp / 'f').write_text('x'); apk.claim(self.tmp / 'f', 'output', 'T1')

    def test_claim_adopts_the_default_work_folder(self):   # build_ipa_from_nsp.py's <title ID>_build, made without a mark
        d = self.tmp / 'T1_build'; (d / 'extract').mkdir(parents=True)
        apk.claim(d, 'work', 'T1', adopt=True); self.assertTrue((d / apk.MARK).is_file())
        with self.assertRaisesRegex(apk.BuildError, 'not the work folder of T2'): apk.claim(d, 'work', 'T2', adopt=True)

    def test_repo_by_identity(self):
        for d in (REPO, REPO / 'dm', REPO / 'not/made/yet', Path(str(REPO) + '/dm/../scripts')):
            with self.subTest(d=d): self.assertEqual(apk.repo_of(d), str(REPO))
        self.assertIsNone(apk.repo_of(self.tmp / 'out'))
        link = self.tmp / 'link'; link.symlink_to(REPO / 'dm')   # through a link, and a folder under it not made yet
        self.assertTrue(apk.within(link / 'out', REPO)); self.assertEqual(apk.repo_of(link / 'out'), str(REPO))
        self.assertFalse(apk.within(self.tmp, REPO)); self.assertFalse(apk.within(REPO.parent, REPO))
        with self.assertRaisesRegex(apk.BuildError, 'inside the repository'): apk.check_folder(link / 'out', 'output')

    def test_repo_in_another_case(self):   # a case-insensitive volume (macOS's default): /users/... is /Users/...
        other = Path(str(REPO).swapcase())
        if not other.exists(): self.skipTest('a case-sensitive volume')
        self.assertTrue(apk.within(other / 'out', REPO)); self.assertEqual(apk.repo_of(other / 'out'), str(REPO))
        with self.assertRaisesRegex(apk.BuildError, 'inside the repository'): apk.check_folder(other / 'out', 'work')

    def test_repo_through_the_data_volume(self):   # macOS's firmlink: /System/Volumes/Data/Users/... is /Users/...
        other = Path('/System/Volumes/Data' + str(REPO))
        if not other.exists(): self.skipTest('no /System/Volumes/Data path to the repository')
        self.assertTrue(apk.within(other / 'out', REPO)); self.assertEqual(apk.repo_of(other / 'out'), str(REPO))

    @unittest.skipUnless(HAVE_GIT, 'no git')
    def test_any_git_work_tree(self):   # another clone or worktree of the repository: any work tree is refused
        r = self.tmp / 'clone'; r.mkdir(); subprocess.run(['git', 'init', '-q', str(r)], check=True)
        self.assertEqual(os.path.realpath(apk.repo_of(r / 'x/out')), os.path.realpath(r))
        with self.assertRaisesRegex(apk.BuildError, 'inside a git work tree'): apk.check_folder(r / 'x/out', 'work')

    def test_links(self):   # a stage folder that is a link carries the writes and deletions elsewhere
        w = self.tmp / 'work'; (w / 'extract/v1/RomFS/deep').mkdir(parents=True); elsewhere = self.tmp / 'elsewhere'; elsewhere.mkdir()
        self.assertEqual(apk.links_under(w), [])
        (w / 'android').symlink_to(elsewhere); (w / 'extract/v1/ExeFS').symlink_to(elsewhere); (w / 'extract/v1/RomFS/deep/x').symlink_to(elsewhere)
        self.assertEqual(apk.links_under(w), [str(w / 'android'), str(w / 'extract/v1/ExeFS')])   # three levels down, not deeper
        with self.assertRaisesRegex(apk.BuildError, 'holds links'): apk.check_folder(w, 'work')
        top = self.tmp / 'top'; top.symlink_to(elsewhere); self.assertEqual(apk.links_under(top), [str(top)])

    def test_apart(self):
        a = self.tmp / 'a'; a.mkdir()
        self.assertTrue(apk.apart(a, self.tmp / 'b'))
        self.assertFalse(apk.apart(a, a)); self.assertFalse(apk.apart(a, a / 'w')); self.assertFalse(apk.apart(a / 'w', a))

    def test_apart_before_either_is_made(self):   # --out X --work X/data with neither made: data_folder's swap would replace X/data, the whole work folder
        x = self.tmp / 'new/X'
        for a, b in ((x, x / 'data'), (x / 'data', x), (x, x), (x / 'a/b', x / 'a'), (x, self.tmp / 'NEW/x/data'), (self.tmp / 'new/caf\u00e9', self.tmp / 'new/cafe\u0301/w'),
                     (x, Path(str(x) + '/w/../data'))):
            with self.subTest(a=a, b=b): self.assertFalse(apk.apart(a, b))
        for a, b in ((x, self.tmp / 'new/XY'), (x / 'out', x / 'work'), (x, self.tmp / 'Y/X'), (x / 'data', self.tmp / 'new/data')):
            with self.subTest(a=a, b=b): self.assertTrue(apk.apart(a, b))
        self.assertFalse((self.tmp / 'new').exists())   # nothing made by asking
        x.mkdir(parents=True)   # one made, the other not
        self.assertFalse(apk.apart(x, x / 'data')); self.assertFalse(apk.apart(x / 'data', x)); self.assertTrue(apk.apart(x / 'out', x / 'work'))
        link = self.tmp / 'link'; link.symlink_to(x)   # the made part compared by identity
        self.assertFalse(apk.apart(link / 'w', x / 'w/data')); self.assertFalse(apk.apart(x / 'w', link / 'w'))


class NszFolder(Base):
    def test_the_folder_decompress_nsz_writes(self):   # the folder checked before the NSZ step is the one it writes and deletes in (by the title ID, not the profile's ID)
        nsp = self.tmp / 'nsps'; nsp.mkdir(); src = nsp / 'Game [01000A0000BC0800][v0].nsz'; src.write_bytes(b'x'); app = 0x01000A0000BC0800
        wrote, real = [], importlib.util.find_spec
        def run(cmd, log, env=None, cwd=None, on_line=None):
            o = Path(cmd[cmd.index('-o') + 1]); (o / (src.stem + '.nsp')).write_bytes(b'n'); wrote.append(o)
        with mock.patch.object(apk.ipa, 'run', run), mock.patch.object(importlib.util, 'find_spec', lambda n, *a: object() if n == 'nsz' else real(n, *a)):
            fam = apk.ipa.decompress_nsz(nsp, app, {'base': src, 'updates': [], 'dlc': []}, self.tmp / 'keys', {})
        d = apk.nsz_folder(nsp, app)
        self.assertEqual(d, nsp / '01000A0000BC0800_build')
        self.assertEqual((fam['base'], wrote), (d / 'nsp' / (src.stem + '.nsp'), [d / 'nsp/partial']))
        self.assertEqual(sorted(p.name for p in nsp.iterdir()), ['01000A0000BC0800_build', src.name])


class DataFolder(Base):
    def test_made_again_when_its_sources_change(self):   # the same names and sizes are not enough: a new DLC extraction (its key) or a file written again makes it again
        romfs, aoc, out = self.tmp / 'ex/RomFS', self.tmp / 'aoc', self.tmp / 'out'
        (romfs / 'Content').mkdir(parents=True); (romfs / 'Content/a.xnb').write_bytes(b'1234'); (aoc / '1').mkdir(parents=True); (aoc / '1/d.bin').write_bytes(b'dlc1')
        kf = self.tmp / 'aoc.key'; kf.write_text('key-of-dlc-v1\n1 DLC\n'); out.mkdir()
        self.assertEqual(apk.data_folder(out, romfs, aoc, kf)[1], 'made')
        self.assertEqual((out / 'data/aoc/1/d.bin').read_bytes(), b'dlc1')
        self.assertEqual(apk.data_folder(out, romfs, aoc, kf)[1], 'reused')
        (aoc / '1/d.bin').write_bytes(b'dlc2'); os.utime(aoc / '1/d.bin', ns=((aoc / '1/d.bin').stat().st_atime_ns, (out / 'data/aoc/1/d.bin').stat().st_mtime_ns))
        kf.write_text('key-of-dlc-v2\n1 DLC\n')   # a replaced DLC NSP of the same size
        self.assertEqual(apk.data_folder(out, romfs, aoc, kf)[1], 'made'); self.assertEqual((out / 'data/aoc/1/d.bin').read_bytes(), b'dlc2')
        st = (romfs / 'Content/a.xnb').stat(); (romfs / 'Content/a.xnb').write_bytes(b'5678'); os.utime(romfs / 'Content/a.xnb', ns=(st.st_atime_ns, st.st_mtime_ns + 10**9))
        self.assertEqual(apk.data_folder(out, romfs, aoc, kf)[1], 'made'); self.assertEqual((out / 'data/romfs/Content/a.xnb').read_bytes(), b'5678')
        self.assertFalse((out / 'data.partial').exists())


class Icon(Base):
    def test_edge_colour(self):   # the mean of the outermost pixels, whatever the row order or depth
        px = [[(200, 100, 0)] * 4] + [[(200, 100, 0), (0, 0, 255), (0, 0, 255), (200, 100, 0)] for _ in range(2)] + [[(200, 100, 0)] * 4]
        for top, bpp in ((True, 24), (False, 24), (True, 32)):
            with self.subTest(top=top, bpp=bpp): self.assertEqual(apk.bmp_edge_colour(bmp(px, top, bpp)), '#c86400')
        with self.assertRaises(ValueError): apk.bmp_edge_colour(b'GIF89a' + b'\0' * 60)
        with self.assertRaises(ValueError): apk.bmp_edge_colour(bmp(px)[:70])

    def test_image_kind(self):
        for data, kind in ((b'\xff\xd8\xff\xe0' + b'\0' * 8, 'jpg'), (png(1, 1, (0, 0, 0)), 'png'), (b'<html>503</html>', None), (b'', None)):
            p = self.tmp / 'f'; p.write_bytes(data); self.assertEqual(apk.image_kind(p), kind)
        with mock.patch.object(Path, 'read_bytes', lambda self: self.fail('read whole')): apk.image_kind(self.tmp / 'f')   # only its first bytes are read

    def test_image_whole(self):   # sips reads a cut file without a word: the end marker says whether it is whole
        p, good = self.tmp / 'f', png(64, 64, (1, 2, 3))
        jpg = b'\xff\xd8\xff\xe0' + b'\x11' * 300 + b'\xff\xd9'
        for data, kind, whole in ((good, 'png', True), (good[:len(good) // 2], 'png', False), (good[:-1], 'png', False), (jpg, 'jpg', True), (jpg + b'\0' * 20, 'jpg', True),
                                  (jpg + b'\0' * 40, 'jpg', False), (jpg[:200], 'jpg', False), (jpg, 'png', False)):
            p.write_bytes(data)
            with self.subTest(kind=kind, n=len(data)): self.assertEqual(apk.image_whole(p, kind), whole)

    def fake_curl(self, payloads):   # stands in for curl (sips runs for real): writes the next payload to -o's file (None: exit 22, the HTTP error)
        calls, real = [], subprocess.run
        def run(cmd, **kw):
            if cmd[0] != 'curl': return real(cmd, **kw)
            self.assertIn('--max-filesize', cmd); self.assertEqual(int(cmd[cmd.index('--max-filesize') + 1]), apk.ICON_MAX)
            calls.append(cmd[-1]); data = payloads.pop(0)
            if data is None: return types.SimpleNamespace(returncode=22, stdout='', stderr='curl: (22) The requested URL returned error: 503')
            Path(cmd[cmd.index('-o') + 1]).write_bytes(data); return types.SimpleNamespace(returncode=0, stdout='', stderr='')
        return run, calls

    @unittest.skipUnless(HAVE_SIPS, 'no sips (macOS)')
    def test_fetch(self):   # a cut download (its header whole) is not an icon; the 256 fallback; the cache holds the PNG made of it; --refresh-icon
        cache, good = self.tmp / 'cache', png(256, 256, (1, 2, 3))
        cut = noisy_png(512); cut = cut[:len(cut) * 2 // 3]
        f = self.tmp / 'cut.png'; f.write_bytes(cut); self.assertEqual(apk.sips_size(f), (512, 512))   # what sips alone would take
        self.assertIn('cut short', apk.cover_png(f, self.tmp / 'o.png')); self.assertFalse((self.tmp / 'o.png').exists())
        run, calls = self.fake_curl([cut, good])
        with mock.patch.object(apk.subprocess, 'run', run): p, how = apk.fetch_icon('0100E65002BB8000', cache)
        self.assertEqual(calls, ['https://tinfoil.media/ti/0100E65002BB8000/512/512/', 'https://tinfoil.media/ti/0100E65002BB8000/256/256/'])
        self.assertEqual(p, cache / '0100E65002BB8000/icon.png'); self.assertIn('256/256', how)
        self.assertEqual((apk.image_kind(p), apk.sips_size(p)), ('png', (256, 256)))
        self.assertEqual(sorted(x.name for x in cache.iterdir()), ['0100E65002BB8000'])   # the download folder is gone
        real = subprocess.run
        with mock.patch.object(apk.subprocess, 'run', lambda cmd, **k: self.fail('fetched again') if cmd[0] == 'curl' else real(cmd, **k)):
            self.assertEqual(apk.fetch_icon('0100E65002BB8000', cache), (p, 'cached'))
        run, calls = self.fake_curl([None, b'<html>not an image</html>'])   # a refresh that fails keeps the cached cover
        with mock.patch.object(apk.subprocess, 'run', run): p2, how = apk.fetch_icon('0100E65002BB8000', cache, refresh=True)
        self.assertEqual((p2, len(calls)), (p, 2)); self.assertIn('fetching again failed', how); self.assertIn('not a JPEG or PNG', how)
        p.write_bytes(p.read_bytes()[:100])   # a cached file cut short is not taken
        run, calls = self.fake_curl([None, None])
        with mock.patch.object(apk.subprocess, 'run', run): self.assertEqual(apk.fetch_icon('0100E65002BB8000', cache)[0], None)

    @unittest.skipUnless(HAVE_SIPS, 'no sips (macOS)')
    def test_fetch_nothing(self):   # neither size: no icon, and nothing left in the cache
        run, _ = self.fake_curl([None, png(256, 200, (1, 1, 1))])
        with mock.patch.object(apk.subprocess, 'run', run): p, why = apk.fetch_icon('0100000000010000', self.tmp / 'cache')
        self.assertIsNone(p); self.assertIn('not a square', why)
        self.assertEqual(list((self.tmp / 'cache').iterdir()), [])

    @unittest.skipUnless(HAVE_SIPS, 'no sips (macOS)')
    def test_make_icon_res(self):
        src = self.tmp / 'cover.png'; src.write_bytes(png(256, 256, (12, 34, 56)))
        self.assertEqual(apk.make_icon_res(src, self.tmp / 'res'), '#0c2238')
        res = self.tmp / 'res'
        self.assertEqual(apk.image_kind(res / 'drawable-nodpi/nx_cover.png'), 'png')
        self.assertIn('#0c2238', (res / 'values/nx_icon.xml').read_text())
        self.assertIn('android:inset="16.6667%"', (res / 'drawable/nx_icon_fg.xml').read_text())
        self.assertFalse((res / 'edge.bmp').exists())

    @unittest.skipUnless(HAVE_SIPS, 'no sips (macOS)')
    def test_falls_through_to_the_next_source(self):   # an online cover cut short (cached before the check, or the cache tampered with) gives way to the extraction's icon.jpg; none, to the default
        cut = self.tmp / 'icon.png'; cut.write_bytes(noisy_png(256)[:-5000])
        good = self.tmp / 'icon.jpg'; (self.tmp / 'g.png').write_bytes(png(256, 256, (200, 10, 10)))
        subprocess.run(['sips', '-s', 'format', 'jpeg', str(self.tmp / 'g.png'), '--out', str(good)], check=True, capture_output=True)
        res = self.tmp / 'res'
        used, skipped = apk.icon_from([('online', 'online cover', cut), ('extraction', 'the extraction\'s icon.jpg', good)], res)
        self.assertEqual(used, 'extraction'); self.assertEqual(len(skipped), 1); self.assertIn('cut short', skipped[0])
        self.assertTrue((res / 'drawable-nodpi/nx_cover.png').is_file())
        self.assertTrue(any(s.startswith('icon: the extraction') for s in self.said))
        used, skipped = apk.icon_from([('online', 'online cover', cut)], res)
        self.assertIsNone(used); self.assertFalse(res.exists())


class Chain(unittest.TestCase):
    def test_host_takes_the_vk_chain(self):   # the packaging refuses to build an APK whose modules the backend would refuse; the repository should never be in that state
        r = subprocess.run([sys.executable, REPO / 'dm/gfx/tools/dmsh_build_vk.py', '--version'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), apk.vkn_chain(REPO), 'dm/gfx/vkn/vkn.h VKN_CHAIN is not dmsh_build_vk.py --version: update it with the chain change')

    def test_own_compilers_vk_keeps_the_version(self):   # the wrapper changes no chain version: the same V as dmsh_build_vk.py, so as VKN_CHAIN
        r = subprocess.run([sys.executable, REPO / 'dm/gfx/tools/own_compilers_vk.py', '--version'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr); self.assertEqual(r.stdout.strip(), apk.vkn_chain(REPO))

    def test_own_compilers_vk_takes_this_macs_spirv_cross(self):   # spirv-cross may differ from the lock (one note line); a missing one, or another translator, still stops the build
        s = importlib.util.spec_from_file_location('own_compilers_vk', REPO / 'dm/gfx/tools/own_compilers_vk.py'); w = importlib.util.module_from_spec(s); s.loader.exec_module(w)
        sc, xl, mt = 'spirv-cross: lock d9dc197f061b42a5 / actual 0123456789abcdef', 'dmsh_xlate.dll: lock 08d31903d666ea0c / actual 0000000000000000', 'metal_macosx: lock A / actual B'
        for found, kept, note in (([sc], [], True), (['spirv-cross: lock d9dc197f061b42a5 / actual missing'], ['spirv-cross: lock d9dc197f061b42a5 / actual missing'], False),
                                  ([sc, xl], [xl], False), ([mt, sc], [mt], True), ([], [], False)):
            out = io.StringIO()
            with self.subTest(found=found), mock.patch.object(w, 'strict', lambda: list(found)), contextlib.redirect_stdout(out):
                self.assertEqual(w.check_tools(), kept)
            self.assertEqual(out.getvalue().startswith("note: built with this Mac's spirv-cross"), note, out.getvalue())
            if note: self.assertIn('spirv-cross 0123456789abcdef (tested: d9dc197f061b42a5)', out.getvalue())


if __name__ == '__main__':
    unittest.main()
