#!/usr/bin/env python3
# P0.8 shader probe (docs/android/PLAN.md, Phase 0; results in docs/android/probes-shader.md): writes the manifest shader_probe.c reads.
# Embeds no game data: every key, binding and pair comes from the files given on the command line, and the output stays outside the repository.
#
# Usage: python3 -I gen_manifest.py --spv <dir with <key>.spv> [--res <dir with <key>.res.json> | --vk <b-<V>-vk dir>] [--pso <pso_manifest.tsv>] [--main <ExeFS/main>]
#            [--romfs <RomFS dir>] --out <dir>
#   --spv      the modules the manifest describes (reflection is taken from these; strip them first, spv_strip_caps.py only drops capabilities)
#   --res      dmsh_xlate's reflection, cross-checked against the SPIR-V (default: the --spv dir)
#   --vk       dmsh_build_vk.py's output (--spv: its <key>_b.spv.zst decompressed to <key>.spv): the manifest's module, bind and interface lines are written from each <key>_b.json
#              (what a backend reads), after checking them against the SPIR-V (stage, entry, set, binding, type, count, interface: each location's base type, components, interpolation
#              and its variable's first location and array lengths); any difference, a missing <key>_b.json or a binding
#              array sized by a specialization constant stops generation (exit 1, no manifest.txt / pairs.tsv left behind). A module whose status is not ok is left out (logged).
#              Its res (dmsh_xlate's) replaces --res
#   --pso      the game's prewarm list (dm/games/<title>/pso_manifest.tsv): its R rows name pipelines the game really built, the strongest pairing evidence
#   --main     the game's main NSO: MonoGame effects (MGFX) in it name a vertex and a pixel shader per pass, the next strongest evidence
#   --romfs    the game's RomFS: MGFX effects in LZ4-compressed XNB files (Content/**/*.xnb)
# Output (in --out): manifest.txt (read by shader_probe.c), pairs.tsv, analysis.txt (stage interfaces, descriptor collisions, min/max/clamp use, position Y and Z,
# support buffer reads, res.json cross-check).
#
# manifest.txt, one record per line, fields separated by single spaces:
#   module <key> <vert|frag> <entry>
#   bind <key> <set> <binding> <VkDescriptorType number> <count> <name>       <count>: 0 for a runtime array
#   vin <key> <location> <f|i|u> <components>                 vertex shader input (built-ins left out)
#   vout <key> <location> <f|i|u> <components> <interp>       vertex shader output; <interp> is - or a comma list of flat, noperspective, centroid, sample
#   fin <key> <location> <f|i|u> <components> <interp>        pixel shader input
#   fout <key> <location> <f|i|u> <components>                pixel shader colour output
#   pair <id> <vs key> <ps key> <source> <cover>              <cover>: ok, or missing:<loc>[,<loc>...] (pixel shader inputs the vertex shader does not write)
import hashlib, json, os, re, shutil, struct, subprocess, sys
from collections import defaultdict, OrderedDict

# ---------------------------------------------------------------- SPIR-V
OP = dict(Name=5, MemberName=6, ExtInstImport=11, ExtInst=12, EntryPoint=15, ExecutionMode=16, Capability=17, TypeVoid=19, TypeBool=20, TypeInt=21,
          TypeFloat=22, TypeVector=23, TypeMatrix=24, TypeImage=25, TypeSampler=26, TypeSampledImage=27, TypeArray=28, TypeRuntimeArray=29, TypeStruct=30,
          TypePointer=32, Constant=43, Function=54, FunctionEnd=56, Variable=59, Load=61, Store=62, AccessChain=65, InBoundsAccessChain=66, Decorate=71,
          MemberDecorate=72, FNegate=127, Label=248, BranchConditional=250, Switch=251)
SC = {0: 'UniformConstant', 1: 'Input', 2: 'Uniform', 3: 'Output', 6: 'Private', 7: 'Function', 9: 'PushConstant', 12: 'StorageBuffer'}
DEC_BLOCK, DEC_BUFFERBLOCK, DEC_BUILTIN, DEC_NOPERSP, DEC_FLAT, DEC_CENTROID, DEC_SAMPLE, DEC_LOCATION, DEC_COMPONENT, DEC_BINDING, DEC_SET = 2, 3, 11, 13, 14, 16, 17, 30, 31, 33, 34
OPNAME = {v: k for k, v in OP.items()}
INTERP = {DEC_FLAT: 'flat', DEC_NOPERSP: 'noperspective', DEC_CENTROID: 'centroid', DEC_SAMPLE: 'sample'}
BUILTIN = {0: 'Position', 1: 'PointSize', 3: 'ClipDistance', 4: 'CullDistance', 5: 'VertexId', 6: 'InstanceId', 15: 'FragCoord', 17: 'FrontFacing', 22: 'FragDepth',
           42: 'VertexIndex', 43: 'InstanceIndex', 4424: 'BaseVertex', 4425: 'BaseInstance', 4426: 'DrawIndex', 9: 'Layer', 10: 'ViewportIndex', 20: 'SampleId', 19: 'SampleMask'}
EXEC_MODEL = {0: 'vert', 4: 'frag', 5: 'comp', 3: 'geom', 1: 'tesc', 2: 'tese'}
GLSL_EXT = {1: 'Round', 2: 'RoundEven', 3: 'Trunc', 4: 'FAbs', 6: 'FSign', 8: 'Floor', 9: 'Ceil', 10: 'Fract', 13: 'Sin', 14: 'Cos', 26: 'Pow', 27: 'Exp', 28: 'Log',
            29: 'Exp2', 30: 'Log2', 31: 'Sqrt', 32: 'InverseSqrt', 37: 'FMin', 38: 'UMin', 39: 'SMin', 40: 'FMax', 41: 'UMax', 42: 'SMax', 43: 'FClamp', 44: 'UClamp',
            45: 'SClamp', 46: 'FMix', 48: 'Step', 49: 'SmoothStep', 50: 'Fma', 66: 'Length', 69: 'Normalize', 79: 'NMin', 80: 'NMax', 81: 'NClamp'}
OPNAME_EXTRA = {126: 'SNegate', 129: 'FAdd', 131: 'FSub', 133: 'FMul', 136: 'FDiv', 141: 'FMod', 142: 'VectorTimesScalar', 148: 'Dot', 169: 'Select', 80: 'CompositeConstruct',
                81: 'CompositeExtract', 79: 'VectorShuffle', 124: 'Bitcast', 112: 'ConvertUToF', 111: 'ConvertSToF', 180: 'FOrdEqual', 184: 'FOrdLessThan',
                186: 'FOrdGreaterThan', 188: 'FOrdLessThanEqual', 190: 'FOrdGreaterThanEqual', 194: 'ShiftRightLogical', 197: 'BitwiseOr', 199: 'BitwiseAnd'}
# VkDescriptorType
VK_SAMPLER, VK_CIS, VK_SAMPLED_IMAGE, VK_STORAGE_IMAGE, VK_UTB, VK_STB, VK_UBO, VK_SSBO, VK_INPUT_ATT = 0, 1, 2, 3, 4, 5, 6, 7, 10
VKNAME = {0: 'SAMPLER', 1: 'COMBINED_IMAGE_SAMPLER', 2: 'SAMPLED_IMAGE', 3: 'STORAGE_IMAGE', 4: 'UNIFORM_TEXEL_BUFFER', 5: 'STORAGE_TEXEL_BUFFER', 6: 'UNIFORM_BUFFER',
          7: 'STORAGE_BUFFER', 10: 'INPUT_ATTACHMENT'}
DIM = {0: '1D', 1: '2D', 2: '3D', 3: 'Cube', 4: 'Rect', 5: 'Buffer', 6: 'SubpassData'}


OPNAME.update(OPNAME_EXTRA)


def fmt(v): return f'{v:g}' if isinstance(v, float) else str(v)


def lit_str(words):
    b = struct.pack(f'<{len(words)}I', *words)
    return b[:b.index(0)].decode() if 0 in b else b.decode()


class Spv:
    def __init__(s, path):
        d = open(path, 'rb').read()
        w = struct.unpack(f'<{len(d) // 4}I', d)
        if w[0] != 0x07230203: raise ValueError(f'{path}: not SPIR-V')
        s.insts = []
        i = 5
        while i < len(w):
            n, op = w[i] >> 16, w[i] & 0xffff
            if n == 0 or i + n > len(w): raise ValueError(f'{path}: bad instruction at word {i}')
            s.insts.append((op, w[i + 1:i + n]))
            i += n
        s.names, s.dec, s.mdec, s.types, s.consts, s.vars, s.caps, s.ext_sets, s.exec_modes = {}, defaultdict(list), defaultdict(list), {}, {}, {}, [], {}, []
        s.entry = None
        for op, a in s.insts:
            if op == OP['Name']: s.names[a[0]] = lit_str(a[1:])
            elif op == OP['Decorate']: s.dec[a[0]].append(tuple(a[1:]))
            elif op == OP['MemberDecorate']: s.mdec[a[0]].append(tuple(a[1:]))
            elif op == OP['Capability']: s.caps.append(a[0])
            elif op == OP['ExtInstImport']: s.ext_sets[a[0]] = lit_str(a[1:])
            elif op == OP['ExecutionMode']: s.exec_modes.append(tuple(a))
            elif op == OP['EntryPoint']:
                name = lit_str(a[2:]); nw = (len(name.encode()) + 4) // 4
                s.entry = (EXEC_MODEL.get(a[0], str(a[0])), a[1], name, list(a[2 + nw:]))
            elif op in (OP['TypeVoid'], OP['TypeBool'], OP['TypeInt'], OP['TypeFloat'], OP['TypeVector'], OP['TypeMatrix'], OP['TypeImage'], OP['TypeSampler'],
                        OP['TypeSampledImage'], OP['TypeArray'], OP['TypeRuntimeArray'], OP['TypeStruct'], OP['TypePointer']) or 19 <= op <= 39:
                s.types[a[0]] = (op, a[1:])
            elif op == OP['Constant']: s.consts[a[1]] = (a[0], a[2:])
            elif op == OP['Variable'] and a[2] != 7: s.vars[a[1]] = (a[0], a[2])   # global variables (Function ones are inside functions)
        if not s.entry: raise ValueError(f'{path}: no entry point')

    def decs(s, i, d): return [x[1:] for x in s.dec.get(i, []) if x[0] == d]
    def has(s, i, d): return any(x[0] == d for x in s.dec.get(i, []))
    def const_u(s, i):
        c = s.consts.get(i); return c[1][0] if c else None
    def const_f(s, i):
        c = s.consts.get(i)
        if not c: return None
        t = s.types.get(c[0])
        return struct.unpack('<f', struct.pack('<I', c[1][0]))[0] if t and t[0] == OP['TypeFloat'] else c[1][0]

    def scalar(s, t):   # → ('f'|'i'|'u', components, locations) for scalar / vector / array of them, None otherwise
        op, a = s.types[t]
        if op == OP['TypeFloat']: return ('f' if a[0] == 32 else f'f{a[0]}', 1, 1)
        if op == OP['TypeInt']: return ('i' if a[1] else 'u') + ('' if a[0] == 32 else str(a[0])), 1, 1
        if op == OP['TypeVector']:
            b = s.scalar(a[0]); return (b[0], a[1], 1) if b else None
        if op == OP['TypeArray']:
            b = s.scalar(a[0]); n = s.const_u(a[1])
            return (b[0], b[1], b[2] * n) if b and n else None
        return None

    def strip_arrays(s, t):   # → (element type, count): 0 for a runtime array, None for one sized by a specialization constant (not an OpConstant)
        count = 1
        while s.types[t][0] in (OP['TypeArray'], OP['TypeRuntimeArray']):
            op, a = s.types[t]
            n = s.const_u(a[1]) if op == OP['TypeArray'] else 0
            count = None if count is None or n is None else count * n
            t = a[0]
        return t, count

    def descriptor(s, vid):   # → (VkDescriptorType, count, detail) or None
        ptr, sc = s.vars[vid]
        t = s.types[ptr][1][1]
        if sc not in (0, 2, 12): return None
        base, count = s.strip_arrays(t)   # every kind: an array of descriptors is one binding of count n (0 runtime, None specialization-sized), as dmsh_build_vk.py and shader_probe.c spv_binds
        if sc == 12: return VK_SSBO, count, 'storage buffer'
        if sc == 2:
            if s.has(base, DEC_BUFFERBLOCK): return VK_SSBO, count, 'storage buffer (BufferBlock)'
            return VK_UBO, count, 'uniform buffer'
        op, a = s.types[base]
        if op == OP['TypeSampler']: return VK_SAMPLER, count, 'sampler'
        img = None
        if op == OP['TypeSampledImage']: img = s.types[a[0]][1]
        elif op == OP['TypeImage']: img = a
        else: return None
        dim, sampled = img[1], img[5]
        detail = f'{DIM.get(dim, dim)}{" array" if img[3] else ""}{" ms" if img[4] else ""}{" depth" if img[2] == 1 else ""}'
        if op == OP['TypeSampledImage']: return (VK_UTB if dim == 5 else VK_CIS), count, detail
        if dim == 6: return VK_INPUT_ATT, count, detail
        if sampled == 2: return (VK_STB if dim == 5 else VK_STORAGE_IMAGE), count, detail
        return (VK_UTB if dim == 5 else VK_SAMPLED_IMAGE), count, detail

    def builtin_of(s, vid):
        ptr = s.vars[vid][0]; t = s.types[ptr][1][1]
        b = s.decs(vid, DEC_BUILTIN)
        if b: return [BUILTIN.get(b[0][0], str(b[0][0]))]
        t, _ = s.strip_arrays(t)
        if s.types[t][0] == OP['TypeStruct']:
            m = [BUILTIN.get(x[2], str(x[2])) for x in s.mdec.get(t, []) if x[1] == DEC_BUILTIN]
            if m: return m
        return None

    def interface(s):   # → {'in': [...], 'out': [...], 'builtin_in': [...], 'builtin_out': [...]}
        r = {'in': [], 'out': [], 'builtin_in': [], 'builtin_out': [], 'bad': []}
        for vid in s.entry[3]:
            if vid not in s.vars: continue
            ptr, sc = s.vars[vid]
            if sc not in (1, 3): continue
            side = 'in' if sc == 1 else 'out'
            b = s.builtin_of(vid)
            if b: r['builtin_' + side] += b; continue
            loc = s.decs(vid, DEC_LOCATION)
            ty = s.scalar(s.types[ptr][1][1])
            if not loc or not ty: r['bad'].append(f'{side} %{vid} ({s.names.get(vid, "")}): no location or not a scalar/vector'); continue
            comp = s.decs(vid, DEC_COMPONENT)
            interp = [INTERP[d] for d in INTERP if s.has(vid, d)]
            t, dims = s.types[ptr][1][1], []
            while s.types[t][0] == OP['TypeArray']: dims.append(s.const_u(s.types[t][1][1])); t = s.types[t][1][0]
            arr = (loc[0][0], tuple(dims)) if dims else ()   # the variable an entry belongs to: its first location and array lengths (a stage interface matches variables)
            for k in range(ty[2]):
                r[side].append(dict(loc=loc[0][0] + k, comp=comp[0][0] if comp else 0, type=ty[0], n=ty[1], interp=interp, arr=arr, name=s.names.get(vid, f'%{vid}')))
        for side in ('in', 'out'): r[side].sort(key=lambda x: x['loc'])
        return r

    def bindings(s):
        out = []
        for vid, (ptr, sc) in s.vars.items():
            if sc not in (0, 2, 12): continue
            d = s.descriptor(vid)
            if not d: continue
            st, b = s.decs(vid, DEC_SET), s.decs(vid, DEC_BINDING)
            out.append(dict(set=st[0][0] if st else 0, binding=b[0][0] if b else 0, type=d[0], count=d[1], detail=d[2], name=s.names.get(vid, f'%{vid}'), id=vid))
        pc = [vid for vid, (ptr, sc) in s.vars.items() if sc == 9]
        return sorted(out, key=lambda x: (x['set'], x['binding'])), pc

    def ext_counts(s):   # GLSL.std.450 instructions → {name: [count, operand forms]}
        glsl = [k for k, v in s.ext_sets.items() if v == 'GLSL.std.450']
        out = defaultdict(lambda: [0, defaultdict(int)])
        for op, a in s.insts:
            if op == OP['ExtInst'] and a[2] in glsl:
                name = GLSL_EXT.get(a[3], f'#{a[3]}')
                e = out[name]; e[0] += 1
                if a[3] in (37, 40, 43, 79, 80, 81):
                    form = ','.join('c' + fmt(s.const_f(x)) if x in s.consts else 'x' for x in a[4:])
                    e[1][form] += 1
        return out

    # The value written to gl_Position.<c>, traced back through function-local variables (last store before the load, in instruction order: exact for
    # straight-line code, which the translator's vertex shaders are; 'branches' says when a function has control flow) → summary dict
    def position_slice(s):
        fn_start = next(i for i, (op, a) in enumerate(s.insts) if op == OP['Function'] and a[1] == s.entry[1])
        fn_end = next(i for i in range(fn_start, len(s.insts)) if s.insts[i][0] == OP['FunctionEnd'])
        body = s.insts[fn_start:fn_end]
        branches = any(op in (OP['BranchConditional'], OP['Switch']) for op, a in body)
        defs, fvars, chains = {}, set(), {}
        for pos, (op, a) in enumerate(body):
            if op == OP['Variable']: fvars.add(a[1])
            if op in (OP['AccessChain'], OP['InBoundsAccessChain']): chains[a[1]] = (a[2], list(a[3:]))
            # result id position for most value instructions: a[1] (a[0] = result type)
            if op not in (OP['Store'], OP['Label'], OP['Function'], OP['FunctionEnd']) and len(a) >= 2: defs[a[1]] = (pos, op, a)
        pv = [vid for vid in s.entry[3] if vid in s.vars and s.vars[vid][1] == 3 and (s.builtin_of(vid) or [None])[0] in ('Position', 'PointSize', 'ClipDistance', 'CullDistance')]
        stores = {}   # component → (pos, value id)
        for pos, (op, a) in enumerate(body):
            if op != OP['Store']: continue
            p = a[0]
            if p in chains and chains[p][0] in pv:
                idx = [s.const_u(x) for x in chains[p][1]]
                if idx[:1] == [0] and len(idx) == 2: stores[idx[1]] = (pos, a[1])
                elif idx == [0]: stores['vec'] = (pos, a[1])
        def base_of(p):
            while p in chains: p = chains[p][0]
            return p
        def last_store(ptr, before):
            b = base_of(ptr)
            for pos in range(before - 1, -1, -1):
                op, a = body[pos]
                if op == OP['Store'] and (a[0] == ptr or (base_of(a[0]) == b and a[0] not in chains and ptr not in chains)): return pos, a[1]
            return None
        def trace(vid, pos, acc, depth=0):
            if depth > 4000 or vid in acc['seen']: return
            acc['seen'].add(vid)
            if vid in s.consts: acc['const'] += 1; return
            if vid not in defs: return
            dpos, op, a = defs[vid]
            if op == OP['Load']:
                ptr = a[2]; b = base_of(ptr)
                if b in fvars:
                    ls = last_store(ptr, dpos)
                    if ls: trace(ls[1], ls[0], acc, depth + 1)
                    return
                if b in s.vars:
                    nm = s.names.get(b) or (f'loc{s.decs(b, DEC_LOCATION)[0][0]}' if s.decs(b, DEC_LOCATION) else f'%{b}')
                    idx = [s.const_u(x) if x in s.consts else '?' for x in (chains[ptr][1] if ptr in chains else [])]
                    acc['leaves'].add(f'{SC.get(s.vars[b][1], s.vars[b][1])}:{nm}{idx}')
                return
            acc['ops'][OPNAME.get(op, GLSL_EXT.get(a[3], f'ext{a[3]}') if op == OP['ExtInst'] else f'op{op}')] += 1
            if op == OP['FNegate']: acc['fneg'] += 1
            for x in a[2:] if op != OP['ExtInst'] else a[4:]:
                if x in defs or x in s.consts: trace(x, dpos, acc, depth + 1)
        def top(vid):   # the operation that produces the stored value, through loads of local variables
            for _ in range(64):
                if vid not in defs: return 'const' if vid in s.consts else '?'
                dpos, op, a = defs[vid]
                if op == OP['Load'] and base_of(a[2]) in fvars:
                    ls = last_store(a[2], dpos)
                    if not ls: return 'load-uninit'
                    vid = ls[1]; continue
                if op == OP['Load']: return f'load {s.names.get(base_of(a[2]), "")}'
                return GLSL_EXT.get(a[3], f'ext{a[3]}') if op == OP['ExtInst'] else OPNAME.get(op, f'op{op}')
            return '?'
        out = {'branches': branches}
        for c, nm in ((0, 'x'), (1, 'y'), (2, 'z'), (3, 'w'), ('vec', 'vec4')):
            if c not in stores: continue
            acc = {'seen': set(), 'leaves': set(), 'ops': defaultdict(int), 'fneg': 0, 'const': 0}
            trace(stores[c][1], stores[c][0], acc)
            out[nm] = dict(top=top(stores[c][1]), fneg=acc['fneg'], support=sorted(l for l in acc['leaves'] if 'support_buffer' in l),
                           uniforms=sorted(l for l in acc['leaves'] if l.startswith('Uniform:') and 'support_buffer' not in l),
                           inputs=sorted(l for l in acc['leaves'] if l.startswith('Input:')), ops=dict(acc['ops']))
        return out

    def pervertex(s):   # gl_PerVertex output block: its members (built-in, array size) and the members the shader writes
        out = {}
        for vid in s.entry[3]:
            if vid not in s.vars or s.vars[vid][1] != 3: continue
            t, _ = s.strip_arrays(s.types[s.vars[vid][0]][1][1])
            if s.types[t][0] != OP['TypeStruct']: continue
            bi = {x[0]: BUILTIN.get(x[2], str(x[2])) for x in s.mdec.get(t, []) if x[1] == DEC_BUILTIN}
            if not bi: continue
            sizes = {}
            for i, mt in enumerate(s.types[t][1]):
                op, a = s.types[mt]
                sizes[bi.get(i, i)] = s.const_u(a[1]) if op == OP['TypeArray'] else 1
            written = set()
            for op, a in s.insts:
                if op in (OP['AccessChain'], OP['InBoundsAccessChain']) and a[2] == vid and len(a) > 3:
                    m = s.const_u(a[3])
                    if m is not None and any(op2 == OP['Store'] and a2[0] == a[1] for op2, a2 in s.insts): written.add(bi.get(m, m))
            out = dict(members=sizes, written=sorted(written))
        return out

    def support_reads(s):   # support_buffer member indices read anywhere
        sb = [v for v in s.vars if s.names.get(v) == 'support_buffer']
        m = set()
        for op, a in s.insts:
            if op in (OP['AccessChain'], OP['InBoundsAccessChain']) and a[2] in sb and len(a) > 3: m.add(s.const_u(a[3]))
        return sorted(x for x in m if x is not None)


# ---------------------------------------------------------------- MonoGame effects (pairing evidence)
def lz4_block(src, size):
    dst = bytearray(); n = len(src); p = 0
    while p < n:
        tok = src[p]; p += 1
        ll = tok >> 4
        if ll == 15:
            while True:
                b = src[p]; p += 1; ll += b
                if b != 255: break
        dst += src[p:p + ll]; p += ll
        if p >= n: break
        off = src[p] | src[p + 1] << 8; p += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[p]; p += 1; ml += b
                if b != 255: break
        ml += 4; start = len(dst) - off
        if off <= 0 or start < 0: raise ValueError('lz4: bad offset')
        if off >= ml: dst += dst[start:start + ml]
        else:
            while ml > 0:
                c = min(ml, off); dst += dst[start:start + c]; start += c; ml -= c
    if len(dst) != size: raise ValueError(f'lz4: {len(dst)} bytes, expected {size}')
    return bytes(dst)


def nso_segments(path):   # → [(segment index, memory offset, bytes)] for rodata and data (MGFX effects are data)
    d = open(path, 'rb').read()
    if d[:4] != b'NSO0': raise ValueError(f'{path}: not an NSO')
    flags = struct.unpack_from('<I', d, 0xc)[0]
    segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]; csz = struct.unpack_from('<III', d, 0x60)
    out = []
    for i in (1, 2):
        fo, mo, sz = segs[i]
        raw = d[fo:fo + csz[i]]
        out.append((i, mo, lz4_block(raw, sz) if flags >> i & 1 else raw[:sz]))
    return out


class Rd:   # little-endian reader with .NET BinaryReader strings
    def __init__(s, d, p): s.d, s.p = d, p
    def u8(s): s.p += 1; return s.d[s.p - 1]
    def u32(s): s.p += 4; return struct.unpack_from('<I', s.d, s.p - 4)[0]
    def raw(s, n): s.p += n; return s.d[s.p - n:s.p]
    def str(s):
        n = sh = 0
        while True:
            b = s.u8(); n |= (b & 0x7f) << sh; sh += 7
            if b < 0x80: return s.raw(n).decode('utf-8', 'replace')


CTL_MAGIC = bytes.fromhex('34127698')
def shader_key(ctl, code):   # dm/gfx/tools/shader_split.py skey: sha256 of the control section (its own length when 0x760..0x87f, else 0x880 bytes) + the code area
    n = struct.unpack_from('<I', ctl, 0x20)[0]
    n = n if 0x760 <= n < 0x880 else 0x880
    return hashlib.sha256(ctl[:n] + code).hexdigest()[:16]


def mgfx_effect(d, p, label):
    """The MGFX version 8 effect at d[p:] (layout as dm/gfx/tools/shader_split.py mgfx_effect) → (shaders [(index, stage, key)], passes [(technique, pass, vs index, ps index)])
    or None when it is not one; raises ValueError when the walk does not end cleanly"""
    if d[p + 4:p + 5] != b'\x08': return None
    r = Rd(d, p + 10)
    try:
        for _ in range(r.u8()): r.str(); r.raw(2); r.raw(3 * r.u8())   # constant buffers
        if d[r.p + 10:r.p + 14] != CTL_MAGIC: return None
    except (IndexError, struct.error): return None
    shaders = []
    for i in range(r.u8()):
        vs, n = r.u8(), r.u32()
        if r.p + n > len(d): return None
        bc = r.raw(n)
        nctl = struct.unpack_from('<I', bc, 0)[0]
        ncode = struct.unpack_from('<I', bc, 4 + nctl)[0] if 8 + nctl <= n else -1
        if vs > 1 or 8 + nctl + ncode != n: raise ValueError(f'{label} MGFX @{p:#x} shader {i}: not a control section and a code area')
        shaders.append((i, 'vs' if vs else 'ps', shader_key(bc[4:4 + nctl], bc[8 + nctl:])))
        for _ in range(r.u8()):   # samplers
            r.raw(3)
            if r.u8(): r.raw(20)
            r.str(); r.u8()
        r.raw(r.u8())             # constant buffer indices
        for _ in range(r.u8()): r.str(); r.raw(4)   # attributes
    def params():
        n = r.u8()
        for _ in range(n):
            typ = r.raw(2)[1]; r.str(); r.str()
            if r.u8(): raise ValueError(f'{label} MGFX @{p:#x}: annotations')
            rows, cols = r.u8(), r.u8()
            if params() + params(): continue
            if typ in (1, 2, 3): r.raw(4 * rows * cols)
            elif typ == 4: r.str()
        return n
    params()
    passes = []
    for _ in range(r.u8()):
        tech = r.str()
        if r.u8(): raise ValueError(f'{label} MGFX @{p:#x}: technique annotations')
        for _ in range(r.u8()):
            pname = r.str()
            if r.u8(): raise ValueError(f'{label} MGFX @{p:#x}: pass annotations')
            v, f = r.u8(), r.u8()
            if (v != 255 and v >= len(shaders)) or (f != 255 and f >= len(shaders)): raise ValueError(f'{label} MGFX @{p:#x}: a pass names shader {v} / {f} of {len(shaders)}')
            for n in (18, 25, 12):
                if r.u8(): r.raw(n)
            passes.append((tech, pname, v, f))
    if r.p > len(d): raise ValueError(f'{label} MGFX @{p:#x}: runs past the data')
    return shaders, passes, r.p - p


def xnb_content(d):
    if d[:3] != b'XNB': return None
    if d[5] & 0x80: return 'lzx'   # LZX not decoded here
    if d[5] & 0x40: return lz4_block(d[14:struct.unpack_from('<I', d, 6)[0]], struct.unpack_from('<I', d, 10)[0])
    return d[10:]


def mgfx_effects(main, romfs, log):
    effects = []   # (label, shaders, passes)
    sources = []
    if main:
        for i, mo, data in nso_segments(main): sources.append((f'main seg{i}', mo, data))
    if romfs:
        for root, _, files in os.walk(romfs):
            for fn in sorted(files):
                if not fn.lower().endswith('.xnb'): continue
                path = os.path.join(root, fn)
                c = xnb_content(open(path, 'rb').read())
                if c == 'lzx': log(f'  {os.path.relpath(path, romfs)}: LZX-compressed XNB, not read'); continue
                if c and b'MGFX' in c: sources.append((os.path.relpath(path, romfs), 0, c))
    for label, base, data in sources:
        for m in re.finditer(b'MGFX', data):
            try: e = mgfx_effect(data, m.start(), label)
            except ValueError as ex: log(f'  [warn] {ex}'); continue
            if e: effects.append((f'{label}@{base + m.start():#x}', e[0], e[1]))
    return effects


# ---------------------------------------------------------------- main
def jio(vj, field):   # a <key>_b.json interface list as the SPIR-V side's (location, f|i|u, components, interp, (first location, array lengths) or ()), sorted
    T = {'float': 'f', 'sint': 'i', 'uint': 'u'}
    arr = lambda a: (a.get('location'), tuple(a.get('dims') or ())) if isinstance(a, dict) else ()   # a malformed array entry still compares unequal
    return sorted(((e['location'], T.get(e['type'], e['type']), e['components'], e.get('interp', []), arr(e['array']) if 'array' in e else ()) for e in vj.get(field, [])), key=repr)


def arg(name, default=None):
    a = sys.argv
    return a[a.index(name) + 1] if name in a and a.index(name) + 1 < len(a) else default


def main():
    spv_dir, out_dir = arg('--spv'), arg('--out')
    if not spv_dir or not out_dir: sys.exit(__doc__ or 'usage: see the header of gen_manifest.py')
    res_dir, pso, nso, romfs, vk_dir = arg('--res', spv_dir), arg('--pso'), arg('--main'), arg('--romfs'), arg('--vk')
    os.makedirs(out_dir, exist_ok=True)
    A = []   # analysis lines
    def log(x=''): A.append(x)

    keys = sorted(f[:-4] for f in os.listdir(spv_dir) if f.endswith('.spv') and re.fullmatch(r'[0-9a-f]{16}\.spv', f))
    M, VJ, E, left = OrderedDict(), {}, [], []   # VJ: the <key>_b.json of each module (--vk); E: errors that stop generation
    for k in keys:
        if vk_dir:
            jp = os.path.join(vk_dir, k + '_b.json')
            if not os.path.exists(jp): E.append(f'{k}: no {k}_b.json in {vk_dir}'); continue
            VJ[k] = json.load(open(jp))
            if VJ[k].get('status') != 'ok': left.append(f'  {k}: status {VJ[k].get("status")} ({"; ".join(VJ[k].get("unsupported", []))[:200]}): left out'); del VJ[k]; continue
        s = Spv(os.path.join(spv_dir, k + '.spv'))
        b, pc = s.bindings()
        M[k] = dict(spv=s, stage=s.entry[0], entry=s.entry[2], io=s.interface(), binds=b, pc=pc)
        E += [f'{k}: binding {x["set"]}.{x["binding"]} ({x["name"]}): an array sized by a specialization constant' for x in b if x['count'] is None]
    vs = [k for k in M if M[k]['stage'] == 'vert']; ps = [k for k in M if M[k]['stage'] == 'frag']
    log(f'modules: {len(M)} ({len(vs)} vertex, {len(ps)} pixel, {len(M) - len(vs) - len(ps)} other) from {spv_dir}' + (f'; {len(left)} not ok in {vk_dir}, left out:' if left else ''))
    for x in left: log(x)

    # ---- cross-checks: spirv-cross --reflect and dmsh_xlate's res.json
    log('\n== cross-check against spirv-cross --reflect and res.json (empty = all agree)')
    sc = shutil.which('spirv-cross')
    if not sc: log('  spirv-cross not found: reflection cross-check skipped')
    RES_TYPE = {1: '1D', 2: 'Buffer', 3: '2D', 4: '3D', 5: 'Cube'}
    for k, m in M.items():
        if sc:
            r = json.loads(subprocess.run([sc, os.path.join(spv_dir, k + '.spv'), '--reflect'], capture_output=True, text=True, check=True).stdout)
            mine = {(b['set'], b['binding']) for b in m['binds']}
            theirs = {(u.get('set', 0), u['binding']) for kind in ('ubos', 'ssbos', 'textures', 'separate_images', 'separate_samplers', 'images') for u in r.get(kind, [])}
            if mine != theirs: log(f'  {k}: bindings differ: mine {sorted(mine)} spirv-cross {sorted(theirs)}')
            for side, kind in (('in', 'inputs'), ('out', 'outputs')):
                a = sorted(x['loc'] for x in m['io'][side]); b = sorted(x['location'] for x in r.get(kind, []) if 'location' in x)
                if a != b: log(f'  {k}: {side} locations differ: mine {a} spirv-cross {b}')
        if vk_dir:   # the vk chain's reflection is what a backend reads, and what the manifest is written from: it has to say what the SPIR-V says
            vj = VJ[k]; rj = vj['res']; srt = lambda l: sorted(l, key=lambda t: tuple(-1 if v is None else v for v in t))
            a = srt((b['set'], b['binding'], b['type'], b['count']) for b in m['binds'])
            b = srt((x['set'], x['binding'], x['type'], 0 if x.get('runtime') else x['count']) for x in vj['bindings'])   # a runtime array: count 0 in the SPIR-V and the manifest
            if a != b: E.append(f'{k}: bindings (set, binding, type, count): SPIR-V {a}, {k}_b.json {b}')
            if any(x['set'] != vj['set'] for x in vj['bindings']): E.append(f'{k}: {k}_b.json bindings outside its set {vj["set"]}')
            if (vj.get('exec_model'), vj.get('entry')) != (m['stage'], m['entry']): E.append(f'{k}: stage / entry: SPIR-V {m["stage"]} {m["entry"]}, {k}_b.json {vj.get("exec_model")} {vj.get("entry")}')
            for side, field in (('in', 'vertex_inputs' if m['stage'] == 'vert' else 'inputs'), ('out', 'outputs')):
                a = sorted(((x['loc'], x['type'], x['n'], x['interp'], x['arr']) for x in m['io'][side]), key=repr)   # per location with its variable's shape; one order (repr: whatever the JSON holds)
                b = jio(vj, field)
                if a != b: E.append(f'{k}: {side} interface: SPIR-V {a}, {k}_b.json {field} {b}')
        else:
            rp = os.path.join(res_dir, k + '.res.json')
            if not os.path.exists(rp): log(f'  {k}: no res.json'); continue
            rj = json.load(open(rp))
        if rj['stage'] != {'vert': 'vs', 'frag': 'ps'}.get(m['stage'], m['stage']): log(f'  {k}: res.json stage {rj["stage"]}, SPIR-V {m["stage"]}')
        ubo = {b['binding'] for b in m['binds'] if b['type'] == VK_UBO and b['name'] != 'support_buffer'}
        if ubo != {c['binding'] for c in rj['cbufs']}: log(f'  {k}: res.json cbuf bindings {[c["binding"] for c in rj["cbufs"]]}, SPIR-V uniform buffers {sorted(ubo)}')
        if 0 in {c['binding'] for c in rj['cbufs']}: log(f'  {k}: res.json puts a cbuf at binding 0')
        tex = {b['binding']: b for b in m['binds'] if b['type'] in (VK_CIS, VK_SAMPLED_IMAGE, VK_UTB)}
        for t in rj['textures']:
            b = tex.get(t['binding'])
            want = RES_TYPE.get(t['type'] & 0xff, '?') + (' array' if t['type'] & 0x100 else '')
            if not b: log(f'  {k}: res.json texture at binding {t["binding"]} missing in the SPIR-V'); continue
            if not b['detail'].startswith(want): log(f'  {k}: texture binding {t["binding"]}: res.json type {t["type"]} ({want}), SPIR-V {b["detail"]}')
        if len(rj['textures']) != len(tex): log(f'  {k}: res.json has {len(rj["textures"])} textures, SPIR-V {len(tex)}')
        for kind in ('sbufs', 'images'):
            if rj[kind]: log(f'  {k}: res.json {kind}: {rj[kind]} (the probe does not exercise these)')
        if rj.get('notes'): log(f'  {k}: notes {rj["notes"]}')
        if m['pc']: log(f'  {k}: push constants declared')
        if m['io']['bad']: log(f'  {k}: interface not understood: {m["io"]["bad"]}')

    if E:   # a reflection that does not say what its module says, or a module the manifest cannot describe: no manifest (a stale one removed, so nothing runs on it)
        log('\n== errors: no manifest written\n' + '\n'.join('  ' + e for e in E))
        for f in ('manifest.txt', 'pairs.tsv'):
            if os.path.exists(os.path.join(out_dir, f)): os.unlink(os.path.join(out_dir, f))
        open(os.path.join(out_dir, 'analysis.txt'), 'w').write('\n'.join(A) + '\n')
        sys.exit(f'gen_manifest: {len(E)} errors, no manifest written (analysis.txt lists them):\n' + '\n'.join('  ' + e for e in E[:20]))

    # ---- pairs
    pairs = OrderedDict()   # (vs, ps) → set of sources
    def add(v, p, src):
        if v in M and p in M and M[v]['stage'] == 'vert' and M[p]['stage'] == 'frag': pairs.setdefault((v, p), []).append(src); return True
        return False
    pso_vs = []
    if pso:
        n = 0
        for line in open(pso):
            f = line.rstrip('\n').split('\t')
            if len(f) < 2 or f[0] != 'R': continue
            raw = bytes.fromhex(f[1])
            v = raw[:24].split(b'\0')[0].decode(); p = raw[24:48].split(b'\0')[0].decode()
            v, p = v.split('_')[0], p.split('_')[0]   # "<key>_b": the Metal chain's variant suffix
            n += 1
            if add(v, p, 'pso'): pso_vs.append(v) if v not in pso_vs else None
            else: log(f'  pso row with keys not among the modules: {v} {p}')
        log(f'\n== pairing evidence\npso_manifest.tsv: {n} render pipeline rows -> {len({k for k, s in pairs.items() if "pso" in s})} distinct shader pairs')
    if nso or romfs:
        effects = mgfx_effects(nso, romfs, log)
        np_ = sum(len(e[2]) for e in effects); nk = sum(1 for e in effects for s in e[1] if s[2] in M)
        log(f'MGFX effects: {len(effects)} ({np_} passes; {sum(len(e[1]) for e in effects)} shaders, {nk} of them among the modules)')
        for label, shaders, passes in effects:
            idx = {i: (st, key) for i, st, key in shaders}
            log(f'  {label}: {len(shaders)} shaders, {len(passes)} passes')
            for tech, pname, v, f in passes:
                vk = idx[v][1] if v != 255 else None; fk = idx[f][1] if f != 255 else None
                note = ''
                if fk and fk not in M: note = ' (pixel shader not among the modules)'
                elif vk and vk not in M: note = ' (vertex shader not among the modules)'
                elif fk is None: note = ' (no pixel shader)'
                elif vk: add(vk, fk, 'mgfx')
                elif pso_vs:   # 255: the pass keeps the bound vertex shader; SpriteBatch's, the one the game's prewarm list pairs
                    for pv in pso_vs: add(pv, fk, 'mgfx-bound-vs')
                    note = f' (vertex shader 255: the bound one; taken as the prewarm list\'s {",".join(pso_vs)})'
                else: note = ' (vertex shader 255 and no prewarm list to resolve it: left to interface pairing)'
                log(f'    {tech}/{pname}: vs {vk or "bound(255)"} ps {fk or "-"}{note}')

    def cover(v, p):   # pixel shader inputs the vertex shader does not write with the same component type and at least the components
        outs = {o['loc']: o for o in M[v]['io']['out']}
        miss = []
        for i in M[p]['io']['in']:
            o = outs.get(i['loc'])
            if not o or o['type'] != i['type'] or o['n'] < i['n']: miss.append(i['loc'])
        return miss
    def extra(v, p): return len({o['loc'] for o in M[v]['io']['out']} - {i['loc'] for i in M[p]['io']['in']})
    log('\n== interface pairing (pixel shaders without a pair from the evidence above; then every vertex shader still without one)')
    log('rule: a vertex shader whose outputs cover the pixel shader inputs (same location, same component type, at least as many components); among those, the one with the')
    log('fewest outputs the pixel shader does not read, then the smaller key. For a leftover vertex shader: the covered pixel shader with the most inputs, then the smaller key.')
    no_vs = []
    for p in ps:
        if any(pp == p for _, pp in pairs): continue
        cand = sorted((extra(v, p), v) for v in vs if not cover(v, p))
        if not cand: no_vs.append(p); log(f'  {p}: NO compatible vertex shader'); continue
        add(cand[0][1], p, 'iface'); log(f'  {p} <- {cand[0][1]} ({len(cand)} candidates; {cand[0][0]} unread outputs)')
    for v in vs:
        if any(vv == v for vv, _ in pairs): continue
        cand = sorted((-len(M[p]['io']['in']), p) for p in ps if not cover(v, p))
        if not cand: log(f'  vertex shader {v}: covers no pixel shader'); continue
        add(v, cand[0][1], 'vs-cover'); log(f'  vertex shader {v} -> {cand[0][1]} ({len(cand)} candidates)')
    if no_vs: log(f'pixel shaders with no compatible vertex shader: {" ".join(no_vs)}')
    else: log('every pixel shader has a compatible vertex shader')
    # how many vertex shaders fit each pixel shader
    log('\ncompatible vertex shaders per pixel shader (inputs -> count of covering vertex shaders):')
    for p in ps: log(f'  {p} in {[i["loc"] for i in M[p]["io"]["in"]]} -> {sum(1 for v in vs if not cover(v, p))}')

    # ---- descriptor collisions: a set and binding both stages of a pair use (the translator's modules: both stages in set 0, bindings numbered from 0 independently)
    sets = defaultdict(lambda: defaultdict(int))
    for m in M.values(): sets[m['stage']][','.join(str(x) for x in sorted({b['set'] for b in m['binds']})) or 'none'] += 1
    log('\n== descriptor bindings: modules per stage by the sets they use: ' + '; '.join(f'{st} {dict(c)}' for st, c in sets.items()))
    nt = nr = ns = 0
    for (v, p), src in pairs.items():
        bv = {(b['set'], b['binding']): b for b in M[v]['binds']}; bp = {(b['set'], b['binding']): b for b in M[p]['binds']}
        t = [b for b in bp if b in bv and bv[b]['type'] != bp[b]['type']]
        r = [b for b in bp if b in bv and bv[b]['type'] == bp[b]['type'] and bv[b]['name'] != bp[b]['name']]
        nt += bool(t); nr += bool(r) and not t; ns += bool({b[0] for b in bv} & {b[0] for b in bp})
        sb = lambda l: [f'{x[0]}.{x[1]}' for x in l]
        if t or r: log(f'  {v}+{p}: type collisions at set.binding {sb(t)} ({", ".join(VKNAME[bv[b]["type"]] + "/" + VKNAME[bp[b]["type"]] for b in t)}); same type, different resource at {sb(r)}')
    log(f'pairs with a type collision: {nt} of {len(pairs)}; with only same-type collisions on different resources: {nr}; with both stages in one descriptor set: {ns}')
    shared0 = all(any(b['binding'] == 0 and b['name'] == 'support_buffer' for b in M[k]['binds']) for k in M)
    log(f'binding 0 is support_buffer in every module: {shared0}')

    # ---- min / max / clamp, capabilities, position, support buffer
    log('\n== GLSL.std.450 min / max / clamp (operand forms: c<value> constant, x computed)')
    tot = defaultdict(int); forms = defaultdict(lambda: defaultdict(int))
    for k, m in M.items():
        for name, (n, f) in m['spv'].ext_counts().items():
            tot[name] += n
            for fm, c in f.items(): forms[name][fm] += c
    for name in ('FMin', 'FMax', 'FClamp', 'NMin', 'NMax', 'NClamp'):
        log(f'  {name}: {tot.get(name, 0)}' + (f'  forms {dict(forms[name])}' if forms.get(name) else ''))
    log('  all GLSL.std.450: ' + ', '.join(f'{k} {v}' for k, v in sorted(tot.items(), key=lambda x: -x[1])))
    caps = defaultdict(int)
    for m in M.values():
        for c in m['spv'].caps: caps[c] += 1
    log(f'capabilities (number: modules): {dict(sorted(caps.items()))}')
    em = defaultdict(int)
    for m in M.values():
        for e in m['spv'].exec_modes: em[e[1]] += 1
    log(f'execution modes (number: modules): {dict(em)}')
    log('\n== gl_Position per vertex shader: the operation writing each component, negations in its slice, support_buffer members and uniforms feeding it')
    ysum = defaultdict(int)
    for k in vs:
        p = M[k]['spv'].position_slice()
        y = p.get('y'); x = p.get('x'); z = p.get('z'); w = p.get('w')
        if not y: log(f'  {k}: no store to gl_Position.y found (stores: {[c for c in p if c != "branches"]})'); ysum['no y store'] += 1; continue
        kind = 'negated' if y['top'] == 'FNegate' else ('support-buffer' if y['support'] else 'plain')
        ysum[kind] += 1
        log(f'  {k}: y={y["top"]} (FNegate in slice {y["fneg"]}, support {y["support"] or "-"}, uniforms {",".join(u.split(":", 1)[1] for u in y["uniforms"])}, inputs {",".join(i.split(":", 1)[1] for i in y["inputs"])}); '
            f'x={x["top"] if x else "?"} (FNegate {x["fneg"] if x else "?"}, uniforms {",".join(u.split(":", 1)[1] for u in x["uniforms"]) if x else "?"}); z={z["top"] if z else "?"} (support {z["support"] if z else "?"}, ops {sorted(z["ops"]) if z else "?"}); '
            f'w={w["top"] if w else "?"}{"; has branches" if p["branches"] else ""}')
    log(f'position y summary: {dict(ysum)}')
    pvs = defaultdict(list)
    for k in vs:
        pv = M[k]['spv'].pervertex()
        pvs[(tuple(sorted(pv.get('members', {}).items())), tuple(pv.get('written', [])))].append(k)
    log('gl_PerVertex declarations (members with array sizes) and the members written:')
    for (mem, wr), ks in pvs.items(): log(f'  {dict(mem)}; written {list(wr)}: {len(ks)} vertex shaders')
    log('\n== support_buffer members read (0 alpha test, 1 is-BGRA[8], 2 viewport inverse, 3 viewport size, 4 render scale count, 5 render scale[73], 6 TFE offset, 7 TFE vertex count)')
    for k, m in M.items():
        r = m['spv'].support_reads()
        if r: log(f'  {k} ({m["stage"]}): {r}')
    if not any(m['spv'].support_reads() for m in M.values()): log('  none: every module declares support_buffer at binding 0, and none reads it')
    else: log('  (modules not listed read none)')
    interp = sorted({f'{k}:{i["loc"]}:{",".join(i["interp"])}' for k, m in M.items() for side in ('in', 'out') for i in m['io'][side] if i['interp']})
    log(f'\ninterpolation decorations on user varyings: {interp or "none"}')
    log('built-ins: ' + ', '.join(f'{k}({m["stage"]}) in {m["io"]["builtin_in"]} out {m["io"]["builtin_out"]}' for k, m in M.items() if m['io']['builtin_in'] or m['stage'] == 'frag' and m['io']['builtin_out']))

    # ---- write
    with open(os.path.join(out_dir, 'manifest.txt'), 'w') as f:
        f.write(f'# P0.8 shader manifest v1, generated by gen_manifest.py from {spv_dir}; game-derived: keep out of the repository\n')
        for k, m in M.items():
            f.write(f'module {k} {m["stage"]} {m["entry"]}\n')
            binds = [dict(x, count=0 if x.get('runtime') else x['count']) for x in VJ[k]['bindings']] if vk_dir else m['binds']   # --vk: from <key>_b.json (checked equal above)
            for b in binds: f.write(f'bind {k} {b["set"]} {b["binding"]} {b["type"]} {b["count"]} {b["name"]}\n')
            io = m['io'] if not vk_dir else {side: [dict(zip(('loc', 'type', 'n', 'interp'), e)) for e in jio(VJ[k], field)]
                                              for side, field in (('in', 'vertex_inputs' if m['stage'] == 'vert' else 'inputs'), ('out', 'outputs'))}
            if m['stage'] == 'vert':
                for i in io['in']: f.write(f'vin {k} {i["loc"]} {i["type"]} {i["n"]}\n')
                for o in io['out']: f.write(f'vout {k} {o["loc"]} {o["type"]} {o["n"]} {",".join(o["interp"]) or "-"}\n')
            elif m['stage'] == 'frag':
                for i in io['in']: f.write(f'fin {k} {i["loc"]} {i["type"]} {i["n"]} {",".join(i["interp"]) or "-"}\n')
                for o in io['out']: f.write(f'fout {k} {o["loc"]} {o["type"]} {o["n"]}\n')
        for n, ((v, p), src) in enumerate(pairs.items()):
            miss = cover(v, p)
            f.write(f'pair {n} {v} {p} {"+".join(src)} {"ok" if not miss else "missing:" + ",".join(map(str, miss))}\n')
    with open(os.path.join(out_dir, 'pairs.tsv'), 'w') as f:
        f.write('id\tvs\tps\tsource\tcover\tvs_out\tps_in\n')
        for n, ((v, p), src) in enumerate(pairs.items()):
            miss = cover(v, p)
            f.write(f'{n}\t{v}\t{p}\t{"+".join(src)}\t{"ok" if not miss else "missing:" + ",".join(map(str, miss))}\t'
                    f'{[o["loc"] for o in M[v]["io"]["out"]]}\t{[i["loc"] for i in M[p]["io"]["in"]]}\n')
    by = defaultdict(int)
    for src in pairs.values(): by['+'.join(src)] += 1
    bad = sum(1 for (v, p) in pairs if cover(v, p))
    log(f'\n== pairs: {len(pairs)} ({dict(by)}); {bad} with uncovered pixel shader inputs; pixel shaders covered {len({p for _, p in pairs})}/{len(ps)}, '
        f'vertex shaders covered {len({v for v, _ in pairs})}/{len(vs)}')
    open(os.path.join(out_dir, 'analysis.txt'), 'w').write('\n'.join(A) + '\n')
    print('\n'.join(A[-1:]))
    print(f'wrote {out_dir}/manifest.txt, pairs.tsv, analysis.txt')


if __name__ == '__main__':
    main()
