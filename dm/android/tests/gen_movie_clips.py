#!/usr/bin/env python3
"""Synthetic clips for t_movie_mc (the movie decoders on MediaCodec): gen_movie_clips.py <out dir> [clip ...] (only those clips, their lines in an existing manifest replaced).

Made on the Mac with ffmpeg (libx264, the native AAC encoder, AudioToolbox's aac_at) and afconvert, from ffmpeg's test patterns: nothing comes from a game.
Each clip is written the way hle_movie.c receives it, in a small container t_movie_mc reads (<name>.nxc, little-endian):
  "NXMC", version 1, kind (1 video, 2 audio)
  video: width, height, access units; SPS, PPS and the avcC record (each a u32 length and the bytes); then per access unit: presentation time (i64 µs), key flag, length, Annex B bytes
         (4-byte start codes, no SPS / PPS in the stream: they come only out of band, as from Configure)
  audio: Configure's rate and channels (as the MP4 sample entry gives them), packets; the AudioSpecificConfig; then per packet: time (i64 µs), length, raw AAC bytes
Beside it the reference decode by ffmpeg: <name>.ref, the NV12 pictures in presentation order (each a u32 length and zlib data), or <name>.<rate>.pcm (s16le, every sample of every packet,
the encoder's priming included). manifest.txt lists the clips with what the test expects of them.
a960, AAC-LC with 960-sample frames (frameLengthFlag): no encoder here makes them, so it is the native encoder's 1,024-sample stream (no TNS, PNS or intensity stereo, cut at 18 kHz)
declared as 960: below the last scale factor band the 48 kHz band tables of both lengths are the same, so its bands parse alike and it is a valid 960-sample stream (its tones come out
6.7 % higher and faster). Its reference is ffmpeg's decode of the same packets in LOAS, which carries the AudioSpecificConfig.
"""
import json, os, struct, subprocess, sys, zlib

FF = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y']

def run(cmd, **kw):
    return subprocess.run(cmd, check=True, stdout=subprocess.PIPE, **kw).stdout

def boxes(b, start, end):
    """the MP4 boxes in b[start:end]: (type, payload start, end)"""
    i = start
    while i + 8 <= end:
        n, t = struct.unpack('>I4s', b[i:i + 8])
        h = 8
        if n == 1: n = struct.unpack('>Q', b[i + 8:i + 16])[0]; h = 16
        if n < h or i + n > end: raise ValueError('bad box at %d' % i)
        yield t.decode('latin-1'), i + h, i + n
        i += n

def find_box(b, path, start=0, end=None):
    """the payload range of the first box along path (container boxes walked; sample entries skipped by their fixed headers)"""
    end = len(b) if end is None else end
    skip = {'stsd': 8, 'avc1': 78, 'mp4a': 28}
    for t, s, e in boxes(b, start, end):
        if t == path[0]:
            if len(path) == 1: return s, e
            r = find_box(b, path[1:], s + skip.get(t, 0), e)
            if r: return r
    return None

def esds_asc(b, s, e):
    """the AudioSpecificConfig (DecoderSpecificInfo, tag 5) of an esds box"""
    p = s + 4
    while p < e:
        tag = b[p]; p += 1; n = 0
        for _ in range(4):
            c = b[p]; p += 1; n = n << 7 | (c & 0x7f)
            if not c & 0x80: break
        if tag == 5: return b[p:p + n]
        p += {3: 3, 4: 13}.get(tag, n)
    raise ValueError('no AudioSpecificConfig')

def packets(path):
    out = run(['ffprobe', '-v', 'error', '-ignore_editlist', '1', '-show_entries', 'stream=time_base:packet=pts,dts,pos,size,flags', '-of', 'json', path])
    j = json.loads(out)
    num, den = map(int, j['streams'][0]['time_base'].split('/'))
    return [(int(p['pts']) * num * 1000000 // den, int(p['pos']), int(p['size']), 'K' in p['flags']) for p in j['packets']], (num, den)

def nal_types(au):
    i, t = 0, []
    while i + 4 <= len(au):
        n = struct.unpack('>I', au[i:i + 4])[0]; t.append(au[i + 4] & 31); i += 4 + n
    return t

def video(out, name, w, h, frames, profile, man):
    mp4 = os.path.join(out, name + '.mp4')
    bf = ['-bf', '0'] if profile == 'baseline' else ['-bf', '3', '-x264-params', 'b-adapt=0']
    run(FF + ['-f', 'lavfi', '-i', 'testsrc2=size=%dx%d:rate=30:duration=%f' % (w, h, frames / 30), '-c:v', 'libx264', '-preset', 'medium', '-profile:v', profile] + bf +
        ['-g', '24', '-keyint_min', '24', '-sc_threshold', '0', '-crf', '20', '-pix_fmt', 'yuv420p', '-an', mp4])
    b = open(mp4, 'rb').read()
    s, e = find_box(b, ['moov', 'trak', 'mdia', 'minf', 'stbl', 'stsd', 'avc1', 'avcC'])
    avcc = b[s:e]
    assert avcc[0] == 1 and avcc[4] & 3 == 3, 'avcC with 4-byte lengths expected'
    i = 6; sps = b''; pps = b''
    n = struct.unpack('>H', avcc[i:i + 2])[0]; sps = avcc[i + 2:i + 2 + n]; i += 2 + n
    i += 1; n = struct.unpack('>H', avcc[i:i + 2])[0]; pps = avcc[i + 2:i + 2 + n]
    assert avcc[5] & 31 == 1 and avcc[6 + 2 + len(sps)] == 1, 'one SPS and one PPS expected'
    pk, _ = packets(mp4)
    aus = []
    for pts, pos, size, key in pk:
        au = b[pos:pos + size]
        t = nal_types(au)
        assert 7 not in t and 8 not in t, '%s: SPS / PPS inside the stream' % name
        annexb = b''; i = 0
        while i < len(au):
            n = struct.unpack('>I', au[i:i + 4])[0]; annexb += b'\0\0\0\1' + au[i + 4:i + 4 + n]; i += 4 + n
        aus.append((pts, key, annexb))
    reorder = any(aus[k][0] < max(a[0] for a in aus[:k]) for k in range(1, len(aus)))
    assert reorder == (profile != 'baseline'), '%s: B-frames expected only outside Baseline' % name
    with open(os.path.join(out, name + '.nxc'), 'wb') as f:
        f.write(b'NXMC' + struct.pack('<IIIII', 1, 1, w, h, len(aus)))
        for x in (sps, pps, avcc): f.write(struct.pack('<I', len(x)) + x)
        for pts, key, d in aus: f.write(struct.pack('<qII', pts, int(key), len(d)) + d)
    raw = run(FF + ['-ignore_editlist', '1', '-i', mp4, '-fps_mode', 'passthrough', '-f', 'rawvideo', '-pix_fmt', 'nv12', '-'])
    fs = w * h * 3 // 2
    assert len(raw) == fs * len(aus), '%s: %d reference pictures for %d access units' % (name, len(raw) // fs, len(aus))
    with open(os.path.join(out, name + '.ref'), 'wb') as f:
        for k in range(len(aus)):
            z = zlib.compress(raw[k * fs:(k + 1) * fs], 6); f.write(struct.pack('<I', len(z)) + z)
    keys = [k for k, a in enumerate(aus) if a[1]]
    man.append('video %s %d %d %d %s %d' % (name, w, h, len(aus), profile, keys[1] if len(keys) > 1 else 0))
    print('%s: %d×%d %s, %d access units (%d B-frame reorders), keys at %s, SPS %d PPS %d bytes' % (name, w, h, profile, len(aus), sum(aus[k][0] < aus[k - 1][0] for k in range(1, len(aus))), keys, len(sps), len(pps)))

def explicit_asc(asc):
    """the same stream's configuration with SBR (and parametric stereo) signalled by object type 5 / 29, from a backward-compatible one (AAC-LC + sync extension 0x2b7 / 0x548)"""
    bits = ''.join('{:08b}'.format(c) for c in asc)
    aot, sfi, cc = int(bits[0:5], 2), int(bits[5:9], 2), int(bits[9:13], 2)
    assert aot == 2 and bits[16:27] == '{:011b}'.format(0x2b7) and int(bits[27:32], 2) == 5 and bits[32] == '1', 'backward-compatible SBR signalling expected'
    esfi = int(bits[33:37], 2); ps = len(bits) >= 49 and bits[37:48] == '{:011b}'.format(0x548) and bits[48] == '1'
    o = '{:05b}{:04b}{:04b}{:04b}{:05b}000'.format(29 if ps else 5, sfi, cc, esfi, 2)
    o += '0' * (-len(o) % 8)
    return bytes(int(o[i:i + 8], 2) for i in range(0, len(o), 8))

def audio(out, name, m4a, man, exp, asc_override=None, ref_from=None):
    b = open(m4a, 'rb').read()
    s, e = find_box(b, ['moov', 'trak', 'mdia', 'minf', 'stbl', 'stsd', 'mp4a', 'esds'])
    asc = esds_asc(b, s, e)
    ms, _ = find_box(b, ['moov', 'trak', 'mdia', 'minf', 'stbl', 'stsd', 'mp4a'])
    ch, rate = struct.unpack('>H', b[ms + 16:ms + 18])[0], struct.unpack('>I', b[ms + 24:ms + 28])[0] >> 16   # the sample entry: what a demuxer gives Configure
    if asc_override: asc = asc_override(asc)
    pk, _ = packets(m4a)
    with open(os.path.join(out, name + '.nxc'), 'wb') as f:
        f.write(b'NXMC' + struct.pack('<IIIII', 1, 2, rate, ch, len(pk)) + struct.pack('<I', len(asc)) + asc)
        for pts, pos, size, key in pk: f.write(struct.pack('<qI', pts, size) + b[pos:pos + size])
    src = ref_from or m4a
    refs = []
    for r in sorted({48000, exp[0]}):   # the decoder's own output and, when the host halves it, at the host's rate too
        pcm = run(FF + ['-flags2', '+skip_manual', '-ignore_editlist', '1', '-i', src, '-ac', '2', '-ar', str(r), '-f', 's16le', '-'])
        open(os.path.join(out, '%s.%d.pcm' % (name, r)), 'wb').write(pcm); refs.append((r, len(pcm) // 4))
    man.append('audio %s %d %d %d %d %d' % (name, rate, ch, exp[0], exp[1], exp[2]))
    print('%s: AudioSpecificConfig %s, Configure %d Hz %d ch, %d packets; expect %d Hz %d ch %d frames per packet; references %s' % (name, asc.hex(), rate, ch, len(pk), exp[0], exp[1], exp[2], refs))

def bitstr(b): return ''.join('{:08b}'.format(c) for c in b)
def frombits(s): return bytes(int(s[i:i + 8], 2) for i in range(0, len(s), 8))

def loas(asc, payloads):
    """an AudioSyncStream (ISO 14496-3 1.7): per payload a frame with its StreamMuxConfig (audioMuxVersion 0, one program, one layer, the AudioSpecificConfig, frameLengthType 0) and the raw AAC bytes"""
    out = bytearray()
    for p in payloads:
        b = '0' + '0' + '1' + '000000' + '0000' + '000' + bitstr(asc) + '000' + '11111111' + '0' + '0'   # useSameStreamMux, version, same time framing, subframes, programs, layers, ASC, ...
        n = len(p)
        while n >= 255: b += '11111111'; n -= 255
        b += '{:08b}'.format(n) + bitstr(p)
        body = frombits(b + '0' * (-len(b) % 8))
        out += struct.pack('>I', 0x2b7 << 13 | len(body))[1:] + body
    return bytes(out)

def audio960(out, name, m4a, man):
    """AAC-LC packets of a 1,024-sample stream declared as 960-sample frames (frameLengthFlag set), 20 ms apart; the reference: ffmpeg's decode of them in LOAS"""
    b = open(m4a, 'rb').read()
    s, e = find_box(b, ['moov', 'trak', 'mdia', 'minf', 'stbl', 'stsd', 'mp4a', 'esds'])
    asc = esds_asc(b, s, e)
    bits = bitstr(asc)
    assert len(asc) >= 2 and int(bits[0:5], 2) == 2 and bits[13:16] == '000', 'an AAC-LC configuration expected'
    asc = frombits(bits[:13] + '1' + bits[14:16])   # its first 16 bits (object type, rate, channels, GASpecificConfig) with frameLengthFlag; the encoder's "no SBR" extension left out
    ms, _ = find_box(b, ['moov', 'trak', 'mdia', 'minf', 'stbl', 'stsd', 'mp4a'])
    ch, rate = struct.unpack('>H', b[ms + 16:ms + 18])[0], struct.unpack('>I', b[ms + 24:ms + 28])[0] >> 16
    pk, _ = packets(m4a)
    pays = [b[pos:pos + size] for _, pos, size, _ in pk]
    with open(os.path.join(out, name + '.nxc'), 'wb') as f:
        f.write(b'NXMC' + struct.pack('<IIIII', 1, 2, rate, ch, len(pk)) + struct.pack('<I', len(asc)) + asc)
        for k, d in enumerate(pays): f.write(struct.pack('<qI', k * 960 * 1000000 // rate, len(d)) + d)
    lo = os.path.join(out, name + '.loas'); open(lo, 'wb').write(loas(asc, pays))
    p = subprocess.run(FF[:-1] + ['-loglevel', 'warning', '-y', '-f', 'loas', '-i', lo, '-ac', '2', '-ar', str(rate), '-f', 's16le', '-'], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert not p.stderr.strip(), '%s: ffmpeg: %s' % (name, p.stderr.decode(errors='replace'))
    assert len(p.stdout) == len(pk) * 960 * 4, '%s: %d frames decoded for %d packets of 960' % (name, len(p.stdout) // 4, len(pk))
    open(os.path.join(out, '%s.%d.pcm' % (name, rate)), 'wb').write(p.stdout); os.remove(lo)
    man.append('audio %s %d %d %d %d %d' % (name, rate, ch, rate, ch, 960))
    print('%s: AudioSpecificConfig %s (960-sample frames), Configure %d Hz %d ch, %d packets; expect %d Hz %d ch 960 frames per packet; reference %d frames' % (name, asc.hex(), rate, ch, len(pk), rate, ch, len(p.stdout) // 4))

def main():
    out = os.path.abspath(sys.argv[1]); os.makedirs(out, exist_ok=True)
    only = set(sys.argv[2:]); want = lambda n: not only or n in only
    man = []
    for name, w, h, n, prof in (('v720', 1280, 720, 48, 'high'), ('v1080', 1920, 1080, 90, 'high'), ('v480', 854, 480, 48, 'high'), ('v360', 640, 360, 48, 'high'), ('vbase', 640, 480, 48, 'baseline')):
        if want(name): video(out, name, w, h, n, prof, man)
    wav = os.path.join(out, 'src.wav')   # two chirps, one per channel (no lag repeats itself), 3 s
    run(FF + ['-f', 'lavfi', '-i', 'aevalsrc=0.4*sin(2*PI*(200+400*t)*t)|0.4*sin(2*PI*(300+500*t)*t):s=48000:d=3', '-c:a', 'pcm_s16le', wav])
    lc, he, he_ff, he2, lc960 = (os.path.join(out, x) for x in ('lc.m4a', 'he.m4a', 'he_ff.m4a', 'he2.m4a', 'lc960.m4a'))
    if want('alc'): run(FF + ['-i', wav, '-c:a', 'aac', '-b:a', '128k', lc])
    if want('ahe'): subprocess.run(['afconvert', '-d', 'aach', '-f', 'm4af', '-b', '64000', wav, he], check=True)
    if want('ahex'): run(FF + ['-i', wav, '-c:a', 'aac_at', '-profile:a', '4', '-b:a', '64k', he_ff])     # HE-AAC
    if want('ahe2') or want('ahe2x'): run(FF + ['-i', wav, '-c:a', 'aac_at', '-profile:a', '28', '-b:a', '32k', he2])      # HE-AAC v2 (parametric stereo: a mono core)
    if want('a960'): run(FF + ['-i', wav, '-c:a', 'aac', '-b:a', '128k', '-aac_tns', '0', '-aac_pns', '0', '-aac_is', '0', '-cutoff', '18000', lc960])
    # expectations (movie_aac_mc.c): LC 48 kHz, 1,024 per packet; SBR by extension (Apple's encoders) at the 24 kHz core rate, 1,024; SBR by object type at 48 kHz, 2,048; stereo throughout;
    # 960-sample LC at 48 kHz, 960 per packet
    if want('alc'): audio(out, 'alc', lc, man, (48000, 2, 1024))
    if want('ahe'): audio(out, 'ahe', he, man, (24000, 2, 1024))
    if want('ahex'): audio(out, 'ahex', he_ff, man, (48000, 2, 2048), explicit_asc)
    if want('ahe2'): audio(out, 'ahe2', he2, man, (24000, 2, 1024))
    if want('ahe2x'): audio(out, 'ahe2x', he2, man, (48000, 2, 2048), explicit_asc)
    if want('a960'): audio960(out, 'a960', lc960, man)
    for f in (wav,): os.remove(f)
    mf = os.path.join(out, 'manifest.txt')
    if only and os.path.exists(mf):   # the other clips' lines stay
        made = {l.split()[1] for l in man}
        man = [l for l in open(mf).read().splitlines() if l.split() and l.split()[1] not in made] + man
    open(mf, 'w').write('\n'.join(man) + '\n')
    print('manifest:\n' + '\n'.join(man))

if __name__ == '__main__':
    main()
