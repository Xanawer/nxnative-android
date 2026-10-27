#!/usr/bin/env python3
# Route B offline build, Vulkan target (docs/android/PLAN.md 2.4): original Switch shaders → SPIR-V (dmsh_xlate, as for Metal) → the SPIR-V passes below → <key>_b.spv.zst + reflection
# <key>_b.json, for the Vulkan replay backend (dm/gfx/vkn). A sibling of dmsh_build.py, not a --target flag inside it: every byte of dmsh_build.py is in the Metal chain version
# (dmsh_chain.FILES), so editing it would move every Metal library to a new b-<V>/ and the host's dmsh_chain.h with it. The steps the two targets share are taken from dmsh_build.py by
# name (metal_chain), so an upstream fix to them reaches this target with no merge work; a name that disappears stops the build, as does any other statement that binds or changes a name
# those take (XC += ..., XC.append(...), IMG_BASE of TEX_BASE's tuple, a def / import / assignment under an if; outside function bodies, any store, method call or comprehension
# target naming one: mutates), which metal_chain would not run.
# Usage: dmsh_build_vk.py <Switch package dir> <output cache dir> [key...]   (no keys = all; as dmsh_build.py)   dmsh_build_vk.py --version   (print V)
#   Output in <output cache dir>/b-<V>-vk/. V folds the target into the chain version: sha256 of "vk", the Metal chain version (its tools, translator and options) and this target's
#   files (VK_FILES)[:12]. The vk directory can share a cache root with the Metal b-<V>/ (dmsh_build.py's prefill skips a folder without _b.metal files) but must not be named shader/vk/,
#   vk_shaders.py's Metal library for Vulkan games. A full build stamps the directory with the game and its profile inputs (inputs.tsv, dmsh_chain.write_stamp); unlike the Metal build it
#   writes no c1_full.tsv / cb_extent.tsv into the profile: those are proven on the Metal outputs, and the host takes them only beside a Metal library's stamp.
#   Environment: DM_GAME (required), XLATE (translator, default the in-repo dmsh_xlate_bin), J (concurrency, default 8), WORK (temp dir parent), DMSH_KEEP_WORK=1 (keep it: the translator's
#   modules before the passes, for comparing with the Metal chain's). No IOSOUT, DMSH_CAS or DMSH_PREFILL:
#   there is no per-shader cache yet (Stardew's 65 shaders build in seconds).
# Steps, per shader (docs/android/PLAN.md 2.4 is the list; vk_module runs 2-10 on a translated module, finish around it):
#   0. structure: a module the passes and the reflection do not follow (more than one entry point, decoration groups, an array sized by a specialization constant, a user interface
#      variable other than a 32-bit scalar, vector or array of them with a Location and no Component: a struct / block, a matrix, a Component split) skips steps 2-8 and is written as
#      translated with status unsupported (step 10). The translator writes none of these; Stardew has none
#   1. dmsh_xlate as for Metal (vertex_ab, the softfloat64 retry and VertexCounterLowering are in the translator / job), then for a vertex shader spv_single_lane and the integer-input
#      retranslation (int_inputs on spirv-cross's MSL, the same decision the Metal chain makes, so both backends give an input the same vertex format)
#   2. select_fix: spv_single_lane writes a ballot as OpSelect(scalar bool, uvec4, uvec4), legal only from SPIR-V 1.4; the translator writes 1.3, so the condition is splatted to a bvec
#   3. nminmax: GLSL.std.450 FMin / FMax / FClamp → NMin / NMax / NClamp. The F forms leave a NaN operand's result undefined; N gives the other operand, and NClamp(NaN, 0, 1) = 0, Maxwell's
#      FMNMX and .SAT (the Metal chain's precise() for the same reason)
#   4. pervertex: the translator declares ClipDistance[8] + CullDistance[1] in every gl_PerVertex, over maxCombinedClipAndCullDistances (8 on every driver here: VUID-...-00710). Each such
#      member is cut to what the shader indexes (one never indexed is removed and the later members' access chains renumbered, one indexed only by constants is sized to the highest index
#      + 1, a dynamic index keeps the size): every declared clip distance clips, so an unwritten one would clip by an undefined value. Only in the output block of the last stage before
#      rasterization (cut_reason: a vertex shader of a game with no gs / hs / ds, a geometry shader, a domain shader of a game with no gs), since linked stages' blocks must match
#      (VUID-RuntimeSpirv-OpVariable-08746); every other block stays as translated, Clip[8] + Cull[1], over the limit. gs / hs / ds are refused (step 10), but a vertex shader of a
#      game that has any of them is ok and keeps its block: every pipeline with it, a plain vertex + pixel one too, raises VUID-...-00710 as the translator's modules do (accepted by
#      every driver here, by leniency) until a pipeline list says which vertex shaders feed which stage
#   5. unused: descriptor variables no function references are removed (support_buffer in every Stardew module), so a set layout holds only what is bound
#   6. sets: one descriptor set per stage (STAGE_SET). The translator numbers every stage from binding 1 in set 0, so a vertex and a pixel shader collide in set 0 (59 of Stardew's 73 pipelines);
#      binding numbers stay the translator's, which res.json records
#   7. pad + init (vertex shaders): Vulkan matches each pixel shader input variable with the output at its Location by an equivalent type (base type, components, array lengths; a
#      wider vector only under maintenance4, not asked for), interpolation decorations excepted. pair_inputs takes the paired pixel shaders' (shader_pairs.tsv; only those built ok)
#      input variables: two reading one location as different variables, or an output there that is not the same variable, make the vertex shader unsupported; flat and smooth readers
#      share one. A variable it writes at none of its locations is added as declared there, an array as one array, with no interpolation decoration (Metal's vs_pad), then every user
#      output, every element of an array, is stored (0,0,0,1) before the body runs (Metal's vs_init: an unwritten component reads so on Maxwell; in Vulkan it is undefined). After the
#      build every pair is checked again on the reflections, by variable, and a vertex shader that does not match a pixel shader input has its reflection rewritten unsupported (a backstop)
#   8. spv_strip_caps (dm/android/tools): Float64, the unused GroupNonUniform* and the interlock capability with its extension (one that stays is refused, step 10);
#      spv_add_caps: StorageImage{Read,Write}WithoutFormat where the module reads or writes a storage image of Unknown format (DMSH_VK_NOFMT=0: not added);
#      spv_barrier_exit: a compute shader's early returns before a workgroup barrier keep the invocation at the barriers, its later writes skipped (DMSH_VK_BARRIER_EXIT=0: as translated)
#   9. spirv-val --target-env vulkan1.3: a module with any message is not written
#  10. refusals: a valid module that must not be used is written with status "unsupported" and its reasons, one [RED] line each, so a backend refuses the pipeline instead of drawing
#      wrongly: every geometry, hull and domain shader (the translator fixes the input topology and the tessellation state that Maxwell takes from the draw; Metal builds variants in
#      gs_slice / geometry_finish, none are built here yet), a capability or extension outside the allowlists of what every target driver supports (CAP_OK / EXT_OK: Float64, the
#      interlock, NVIDIA's and NVX's among those left out), subgroup operations outside fragment and compute shaders (the only stages every target driver runs them in), push constants
#      (not in the reflection), and steps 0 and 7's reasons. Stardew has none of these
#   Metal-only steps not here: precise (step 3), vs_depth (VK_EXT_depth_clip_control per pipeline), vs_layer (Vulkan allows layer + point size), gs_slice / geometry_finish (step 10),
#   dmsh_fetch / dmsh_vin (off by default upstream), c1_guard / cb_extent (the conservative paths). vs_tf (transform feedback) and fs_uint (integer colour outputs) have no Vulkan form
#   yet: such a shader is reported [RED] and flagged in its reflection.
# Output files: <key>_b.spv.zst (one zstd frame with the content size; <key>_b.spv when this Python has no compression.zstd / backports.zstd), <key>_b.json, _b_build.json (status per key:
#   ok, unsupported, or a failure, which writes nothing). Exit status 0 only when every shader is ok. The stamp is removed before the directory changes and written again only when every
#   shader has its output (ok or unsupported): by a full run, or by a partial one into a directory that was complete for the same inputs.
# Reflection <key>_b.json (field names follow dmsh_build.py's Metal reflection where the meaning is the same):
#   key "<key>_b", route "B", target "vk", chain V, status (ok, or unsupported: a backend must not use the module), unsupported (the reasons, step 10), dm_ctl, source, pc_paired,
#   stage (vs ps gs hs ds cs), exec_model (vert frag geom tesc tese comp), entry (the SPIR-V entry point name),
#   spv (file name), spv_bytes / spv_sha256 (uncompressed size and sha256[:16], checked after decompression), zstd (library version, or null for a raw module),
#   set: this stage's descriptor set (every binding of the module is in it; a pipeline layout puts the vertex shader's at 0, the pixel shader's at 1, empty set layouts filling any gap;
#   null when step 0 skipped the passes: the bindings keep the translator's sets),
#   bindings: [{set, binding, type (VkDescriptorType value), type_name (UNIFORM_BUFFER ...), count (1, n for an array; null with runtime: true for a runtime array, the layout's
#              variable count, or spec_sized: true for one sized by a specialization constant, which step 0 refuses), name (SPIR-V name),
#              image {dim (1D 2D 3D Cube Rect Buffer SubpassData), arrayed, ms, depth (0 1 2), sampled (1 sampled, 2 storage), comp (float sint uint), format (SPIR-V ImageFormat)} for image types,
#              then the Metal entry's NVN fields (nvn_fields: dmsh_build.py reflect() by the same name): kind (cbuf c1 drvcb sbuf ubo_ptr sbuf_ubo tex tbuf img tex_ubo tex_c2a support geometry ?),
#              reg / off / bank / index / rw / fetch1d / fetch1d_size as there, and for a combined image sampler the fields of Metal's <name>Smplr entry under sampler
#              (kind samp_tex samp samp_id samp_ubo samp_c2a, reg / off)}], statically used only, by binding,
#   res: the translator's res.json (its binding numbers are the bindings' binding: uniform slots, storage buffer c0 offsets, texture handles, types and formats, fetch1d, notes),
#   vertex_inputs [{location, type (float sint uint), components}] (vertex shaders: a uint input takes a _UINT vertex format), inputs / outputs [{location, type, components, interp,
#   array (only for an array variable: {location (its first), dims (lengths, outermost first)}: a stage interface matches variables, not locations), patch (only when true)}] the user
#   interface, one entry per location; a pixel shader's outputs are its colours, each with value (out_values: float, uint (written from integer bits: an integer attachment takes
#   them bit for bit), const (only constants), unknown (no MSL)), so a backend can choose attachment formats; vs_pad (locations padded), init (locations initialised), c1 (hex),
#   threadgroup_size, notes,
#   drv_reads / drv_unfillable (as Metal's, but read from the SPIR-V for every stage: Metal's MSL regex sees only pixel shaders' fp_c0 / fp_c2), raster (dmsh_build.py raster_facts
#   on spirv-cross's MSL of the module before the passes: the vertex facts without Metal's depth rewrite, which Vulkan does not have), tf / tf_strides (transform feedback layout, no
#   capture emitted yet),
#   int_writer (pixel shader Metal gives main0_u), uint_mask_m (as Metal's: the colour outputs written from integer bits when the others are not, bit N = location N), passes {single_lane,
#   select_fix, nminmax, pervertex (what was cut, or "kept <block>: why"), unused, caps_dropped; or skipped (step 0's reasons)}
import ast, collections, concurrent.futures as cf, hashlib, json, os, re, shutil, struct, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import dmsh_chain, dmsh_geometry
sys.path.insert(0, os.path.join(HERE, '../../android/tools')); import spv_strip_caps, spv_add_caps, spv_barrier_exit
BUILD = os.path.join(HERE, 'dmsh_build.py')
VK_FILES = ['tools/dmsh_build_vk.py', '../android/tools/spv_strip_caps.py', '../android/tools/spv_add_caps.py', '../android/tools/spv_barrier_exit.py']   # relative to dm/gfx, as dmsh_chain.FILES
STAGE_SET = {'vs': 0, 'ps': 1, 'gs': 2, 'hs': 3, 'ds': 4, 'cs': 0}   # five sets for a full tessellation + geometry pipeline: above the guaranteed maxBoundDescriptorSets of 4
EXEC_MODEL = {'vs': 'vert', 'ps': 'frag', 'gs': 'geom', 'hs': 'tesc', 'ds': 'tese', 'cs': 'comp'}
try: from compression import zstd   # Python 3.14; the backport before, as smash_arc.py
except ImportError:
    try: from backports import zstd
    except ImportError: zstd = None

def version():
    h = hashlib.sha256(b'target\0vk\0metal\0' + dmsh_chain.version().encode() + b'\0')
    for f in VK_FILES: h.update(f.encode() + b'\0' + open(os.path.join(HERE, '..', f), 'rb').read() + b'\0')
    return h.hexdigest()[:12]

# ---------------------------------------------------------------- what the Metal chain shares
METAL_NAMES = ['TEX_BASE', 'SSBO', 'XC', 'precise', 'records', 'vertex_ab', 'job_for', 'tf_layout', 'raster_facts', 'decode_tex', 'const_sampler', 'decode_ubo_tex', 'c2_array',
               'DRV_FILL', 'unfillable', '_CAST', 'int_inputs', 'fs_uint', 'SG_OPS', 'SG_BUILTINS', 'spv_single_lane', '_ep_drop']
MUTATORS = {'append', 'extend', 'insert', 'remove', 'pop', 'popitem', 'clear', 'update', 'setdefault', 'add', 'discard', 'sort', 'reverse', 'difference_update', 'intersection_update',
            'symmetric_difference_update', '__setitem__', '__delitem__', '__iadd__', '__ior__'}
SCOPES = (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)
def scope_walk(roots, classes=True):   # the nodes evaluated in one scope: a function / lambda is yielded (its name binds here) with its decorators and defaults, not its body; a class
    st = list(roots)                     # body is walked into when classes, else left to the caller; a comprehension's target is not walked (mutates sees it on the comprehension)
    while st:
        n = st.pop(); yield n
        if isinstance(n, SCOPES): st += getattr(n, 'decorator_list', []) + n.args.defaults + [d for d in n.args.kw_defaults if d]
        elif isinstance(n, ast.ClassDef): st += n.decorator_list + n.bases + [k.value for k in n.keywords] + (n.body if classes else [])
        elif isinstance(n, ast.comprehension): st += [n.iter] + n.ifs
        else: st += ast.iter_child_nodes(n)
def binds(n):   # the names one node binds in its scope ('*' for a star import)
    if isinstance(n, ast.Name) and isinstance(n.ctx, (ast.Store, ast.Del)): return [n.id]
    if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)): return [n.name]
    if isinstance(n, (ast.Import, ast.ImportFrom)): return [a.asname or a.name.split('.')[0] for a in n.names]
    if isinstance(n, ast.ExceptHandler) or type(n).__name__ in ('MatchAs', 'MatchStar'): return [n.name] if n.name else []
    if type(n).__name__ == 'MatchMapping': return [n.rest] if n.rest else []
    return []
def mutates(node, watch):
    """The names in watch (what metal_chain's statements bind) that a top-level statement it does not run may bind or change. Conservative outside function bodies (module level and
    class bodies anywhere, comprehensions and all): any mention of one stored or deleted (an assignment, AugAssign or walrus, a def / class / import / for / with / except / match
    target, under any if / for / while / try / with), a subscript or attribute of one stored or deleted, any method called on one, or one in a comprehension target. Function bodies
    get scope analysis: a name the function binds is its own (or its closure's) unless declared global; a module one stored or deleted, its subscript or attribute stored or
    deleted, or a mutating method called on it counts; any global declaration of one counts, as does a comprehension target naming one anywhere. → set of names"""
    def base(x):
        while isinstance(x, (ast.Subscript, ast.Attribute)): x = x.value
        return x.id if isinstance(x, ast.Name) else None
    hit = set()
    def check(n, mod, calls):   # mod(x): x is the module's name here; calls: method names that count (None: any)
        hit.update(x for x in binds(n) if (x in watch or x == '*') and mod(x))
        if isinstance(n, ast.Global): hit.update(x for x in n.names if x in watch)
        if isinstance(n, (ast.Subscript, ast.Attribute)) and isinstance(n.ctx, (ast.Store, ast.Del)) and base(n) in watch and mod(base(n)): hit.add(base(n))
        if isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute) and (calls is None or n.func.attr in calls) and base(n.func) in watch and mod(base(n.func)): hit.add(base(n.func))
        if isinstance(n, ast.comprehension): hit.update(x.id for x in ast.walk(n.target) if isinstance(x, ast.Name) and x.id in watch)
    def plain(roots, outer):   # code outside function bodies: every mention counts
        for n in scope_walk(roots):
            check(n, lambda x: True, None)
            if isinstance(n, SCOPES): function(n, outer)
    def function(f, outer):   # outer: names the enclosing function scopes bind (a closure's, not the module's)
        ns = list(scope_walk(f.body if isinstance(f.body, list) else [f.body], classes=False))
        glob = {x for n in ns if isinstance(n, ast.Global) for x in n.names}; nonl = {x for n in ns if isinstance(n, ast.Nonlocal) for x in n.names}
        loc = ({a.arg for a in ast.walk(f.args) if isinstance(a, ast.arg)} | {x for n in ns for x in binds(n)}) - glob - nonl
        mod = lambda x: x in glob or (x not in loc and x not in nonl and x not in outer)
        for n in ns:
            check(n, mod, MUTATORS)
            if isinstance(n, SCOPES): function(n, outer | loc)
            elif isinstance(n, ast.ClassDef): plain(n.body, outer | loc)
    plain([node], set())
    return hit

def metal_chain(facts):
    """The functions and constants named in METAL_NAMES, compiled from dmsh_build.py's own source (which runs a whole build when imported) into one namespace; the assignment
    statements come along in file order, so a constant built from others (DRV_FILL) sees them"""
    tree = ast.parse(open(BUILD).read(), BUILD); want = set(METAL_NAMES)
    def names(n): return {t.id for x in n.targets for t in (x.elts if isinstance(x, ast.Tuple) else [x]) if isinstance(t, ast.Name)}
    body = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name in want) or (isinstance(n, ast.Assign) and names(n) & want)]
    watch = {x for n in body for s in scope_walk([n]) for x in binds(s)}   # every name these statements bind (TEX_BASE's tuple binds IMG_BASE too): all are in the namespace
    bad = [f'line {n.lineno}: {", ".join(sorted(h))}' for n in tree.body if n not in body for h in [mutates(n, watch)] if h]
    if bad: sys.exit(f'[RED] dmsh_build.py binds or changes what the Vulkan target takes from it outside the plain top-level assignments and definitions that bind it ({"; ".join(bad)}): '
                     f'metal_chain runs only those, so the change would not reach this target; move it into the assignment, or teach metal_chain')
    import dmsh_facts
    ns = {'re': re, 'struct': struct, 'os': os, 'sys': sys, 'json': json, 'hashlib': hashlib, 'dmsh_geometry': dmsh_geometry, 'dmsh_facts': dmsh_facts, 'FACTS': facts}
    try: exec(compile(ast.Module(body=body, type_ignores=[]), BUILD, 'exec'), ns)
    except Exception as e: sys.exit(f'[RED] what the Vulkan target takes from dmsh_build.py does not run on its own ({type(e).__name__}: {e}): a statement needs a name metal_chain does not take')
    gone = [n for n in METAL_NAMES if n not in ns]
    if gone: sys.exit(f'[RED] dmsh_build.py no longer defines {", ".join(gone)}, which the Vulkan target takes from it (dmsh_build_vk.py METAL_NAMES)')
    return ns

# ---------------------------------------------------------------- SPIR-V
OP_NAME, OP_MEMBER_NAME, OP_EXT_IMPORT, OP_EXT_INST, OP_ENTRY_POINT, OP_CAPABILITY = 5, 6, 11, 12, 15, 17
OP_TYPE_BOOL, OP_TYPE_INT, OP_TYPE_FLOAT, OP_TYPE_VECTOR, OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_SAMPLED_IMAGE, OP_TYPE_ARRAY, OP_TYPE_RTARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER = 20, 21, 22, 23, 25, 26, 27, 28, 29, 30, 32
OP_CONSTANT, OP_CONSTANT_COMPOSITE, OP_FUNCTION, OP_FUNCTION_END, OP_VARIABLE, OP_LOAD, OP_STORE, OP_ACCESS_CHAIN, OP_IB_ACCESS_CHAIN = 43, 44, 54, 56, 59, 61, 62, 65, 66
OP_DECORATE, OP_MEMBER_DECORATE, OP_COMPOSITE_CONSTRUCT, OP_IMAGE, OP_QUERY_SIZE_LOD, OP_QUERY_SIZE, OP_SELECT, OP_LABEL, OP_LINE, OP_NO_LINE = 71, 72, 80, 100, 103, 104, 169, 248, 8, 317
ANNOTATION_OPS = {71, 72, 73, 74, 75, 332, 5632, 5633}
DEC_BUFFERBLOCK, DEC_BUILTIN, DEC_NOPERSP, DEC_FLAT, DEC_CENTROID, DEC_SAMPLE, DEC_LOCATION, DEC_BINDING, DEC_SET = 3, 11, 13, 14, 16, 17, 30, 33, 34
INTERP = {DEC_FLAT: 'flat', DEC_NOPERSP: 'noperspective', DEC_CENTROID: 'centroid', DEC_SAMPLE: 'sample'}
BI_CLIP, BI_CULL = 3, 4
SC_UNIFORM_CONSTANT, SC_INPUT, SC_UNIFORM, SC_OUTPUT, SC_STORAGE_BUFFER = 0, 1, 2, 3, 12
VK_DESC = {0: 'SAMPLER', 1: 'COMBINED_IMAGE_SAMPLER', 2: 'SAMPLED_IMAGE', 3: 'STORAGE_IMAGE', 4: 'UNIFORM_TEXEL_BUFFER', 5: 'STORAGE_TEXEL_BUFFER', 6: 'UNIFORM_BUFFER', 7: 'STORAGE_BUFFER', 10: 'INPUT_ATTACHMENT'}
DIM = {0: '1D', 1: '2D', 2: '3D', 3: 'Cube', 4: 'Rect', 5: 'Buffer', 6: 'SubpassData'}
GLSL_N = {37: 79, 40: 80, 43: 81}   # FMin / FMax / FClamp → NMin / NMax / NClamp
OP_EXTENSION, DEC_PATCH, DEC_COMPONENT = 10, 15, 31
OP_TYPE_MATRIX, OP_DECORATION_GROUP, OP_GROUP_DECORATE, OP_GROUP_MEMBER_DECORATE, SPEC_CONSTANT_OPS = 24, 73, 74, 75, {48, 49, 50, 51, 52}
# The SPIR-V capabilities and extensions every target driver supports (step 10 refuses a module declaring anything else): vk.xml 1.4.363's <spirvcapabilities> / <spirvextensions>, each <enable>
# evaluated against the stock driver (Vulkan 1.3.284), 842.6, Turnip and KosmicKrisp as the P0.4 gpu probe (features / properties / extensions.txt) and vulkaninfo dumped them, versions capped
# at 1.3 (the spirv-val environment); by number, so an alias counts (5254 is ShaderViewportIndexLayerEXT and NV). A backend still enables the feature each one names (DrawParameters:
# shaderDrawParameters). Regenerate when a target driver changes: ~/dm-work/android/vk-chain/allow/gen_allow.py <vk.xml> <spirv.core.grammar.json> <dm-work/android> (outside the repository)
CAP_OK = {0: 'Matrix', 1: 'Shader', 3: 'Tessellation', 9: 'Float16', 11: 'Int64', 22: 'Int16', 23: 'TessellationPointSize', 24: 'GeometryPointSize', 25: 'ImageGatherExtended',
          28: 'UniformBufferArrayDynamicIndexing', 29: 'SampledImageArrayDynamicIndexing', 30: 'StorageBufferArrayDynamicIndexing', 31: 'StorageImageArrayDynamicIndexing', 32: 'ClipDistance',
          33: 'CullDistance', 34: 'ImageCubeArray', 35: 'SampleRateShading', 39: 'Int8', 40: 'InputAttachment', 43: 'Sampled1D', 44: 'Image1D', 45: 'SampledCubeArray', 46: 'SampledBuffer',
          47: 'ImageBuffer', 49: 'StorageImageExtendedFormats', 50: 'ImageQuery', 51: 'DerivativeControl', 52: 'InterpolationFunction', 55: 'StorageImageReadWithoutFormat',
          56: 'StorageImageWriteWithoutFormat', 57: 'MultiViewport', 61: 'GroupNonUniform', 62: 'GroupNonUniformVote', 63: 'GroupNonUniformArithmetic', 64: 'GroupNonUniformBallot',
          65: 'GroupNonUniformShuffle', 66: 'GroupNonUniformShuffleRelative', 68: 'GroupNonUniformQuad', 69: 'ShaderLayer', 70: 'ShaderViewportIndex', 4427: 'DrawParameters',
          4428: 'WorkgroupMemoryExplicitLayoutKHR', 4430: 'WorkgroupMemoryExplicitLayout16BitAccessKHR', 4433: 'StorageBuffer16BitAccess', 4437: 'DeviceGroup', 4439: 'MultiView',
          4441: 'VariablePointersStorageBuffer', 4442: 'VariablePointers', 4448: 'StorageBuffer8BitAccess', 4466: 'SignedZeroInfNanPreserve', 4467: 'RoundingModeRTE', 5013: 'StencilExportEXT',
          5254: 'ShaderViewportIndexLayerEXT', 5301: 'ShaderNonUniform', 5302: 'RuntimeDescriptorArray', 5304: 'UniformTexelBufferArrayDynamicIndexing',
          5305: 'StorageTexelBufferArrayDynamicIndexing', 5306: 'UniformBufferArrayNonUniformIndexing', 5307: 'SampledImageArrayNonUniformIndexing', 5308: 'StorageBufferArrayNonUniformIndexing',
          5309: 'StorageImageArrayNonUniformIndexing', 5311: 'UniformTexelBufferArrayNonUniformIndexing', 5312: 'StorageTexelBufferArrayNonUniformIndexing', 5345: 'VulkanMemoryModel',
          5346: 'VulkanMemoryModelDeviceScope', 5347: 'PhysicalStorageBufferAddresses', 5379: 'DemoteToHelperInvocation', 6016: 'DotProductInputAll', 6017: 'DotProductInput4x8Bit',
          6018: 'DotProductInput4x8BitPacked', 6019: 'DotProduct'}
EXT_OK = {'SPV_EXT_demote_to_helper_invocation', 'SPV_EXT_descriptor_indexing', 'SPV_EXT_shader_atomic_float_add', 'SPV_EXT_shader_stencil_export', 'SPV_EXT_shader_viewport_index_layer',
          'SPV_KHR_16bit_storage', 'SPV_KHR_8bit_storage', 'SPV_KHR_device_group', 'SPV_KHR_float_controls', 'SPV_KHR_integer_dot_product', 'SPV_KHR_multiview', 'SPV_KHR_non_semantic_info',
          'SPV_KHR_physical_storage_buffer', 'SPV_KHR_shader_draw_parameters', 'SPV_KHR_storage_buffer_storage_class', 'SPV_KHR_subgroup_uniform_control_flow', 'SPV_KHR_terminate_invocation',
          'SPV_KHR_variable_pointers', 'SPV_KHR_vulkan_memory_model', 'SPV_KHR_workgroup_memory_explicit_layout'}
CAP_NAME = {2: 'Geometry', 10: 'Float64', 53: 'TransformFeedback', 5251: 'GeometryShaderPassthroughNV', 5255: 'ShaderViewportMaskNV', 5260: 'PerViewAttributesNV',
            5363: 'FragmentShaderSampleInterlockEXT', 5372: 'FragmentShaderPixelInterlockEXT', 5378: 'FragmentShaderShadingRateInterlockEXT'}   # names for refusal messages beyond CAP_OK
CAP_SUBGROUP = set(range(61, 69)) | {4423, 4431}   # GroupNonUniform* and the KHR ballot / vote
SG_STAGES = {'ps', 'cs'}   # subgroupSupportedStages: stock and KosmicKrisp 0x30, 842.6 0xf0, Turnip 0x3f: fragment and compute on every target driver, nothing else
FIXED_STATE = {   # what the translator fixes that Maxwell sets per draw (dmsh_xlate Program.cs, no geometry_topology / tessellation state in the job): Metal builds variants (geometry_finish)
    'gs': 'geometry shader translated for one input topology (triangles: OpExecutionMode Triangles, gl_in[3]); Maxwell takes it from the draw, and no per-topology variants are built for Vulkan yet',
    'hs': 'tessellation-control shader translated with a fixed tessellation state (triangles, equal spacing, the translator\'s winding); no per-state variants are built for Vulkan yet',
    'ds': 'tessellation-evaluation shader translated with a fixed tessellation state (triangles, equal spacing, the translator\'s winding); no per-state variants are built for Vulkan yet'}

def lit_str(w):
    b = struct.pack(f'<{len(w)}I', *w); return b[:b.index(0)].decode() if 0 in b else b.decode()
def str_words(s):
    b = s.encode() + b'\0'; b += b'\0' * (-len(b) % 4); return list(struct.unpack(f'<{len(b) // 4}I', b))

class Spv:
    """A module as a list of instructions (each a list of words, opcode word first); index() rebuilds the lookups after a change"""
    def __init__(s, data):
        w = list(struct.unpack(f'<{len(data) // 4}I', data))
        if len(w) < 5 or w[0] != 0x07230203: raise ValueError('not a SPIR-V module')
        s.hdr, s.ins, i = w[:5], [], 5
        while i < len(w):
            n = w[i] >> 16
            if not n or i + n > len(w): raise ValueError(f'bad instruction at word {i}')
            s.ins.append(w[i:i + n]); i += n
        s.index()
    def bytes(s):
        flat = s.hdr + [v for x in s.ins for v in x]; return struct.pack(f'<{len(flat)}I', *flat)
    def version(s): return (s.hdr[1] >> 16) & 0xff, (s.hdr[1] >> 8) & 0xff
    def new_id(s): s.hdr[3] += 1; return s.hdr[3] - 1
    def index(s):
        s.names, s.types, s.consts, s.vars, s.dec, s.mdec, s.entries = {}, {}, {}, {}, collections.defaultdict(list), collections.defaultdict(list), []
        s.groups, s.spec, infn = set(), set(), False
        for x in s.ins:
            o = x[0] & 0xffff
            if o == OP_FUNCTION: infn = True
            elif o == OP_FUNCTION_END: infn = False
            elif o == OP_NAME: s.names[x[1]] = lit_str(x[2:])
            elif o == OP_DECORATE: s.dec[x[1]].append(x[2:])
            elif o == OP_MEMBER_DECORATE: s.mdec[x[1]].append(x[2:])
            elif o == OP_DECORATION_GROUP: s.groups.add(x[1])
            elif o == OP_GROUP_DECORATE:   # read through, so the reflection of a module refused for its groups (structure) is still right; the passes never edit one
                for t in x[2:]: s.dec[t].extend(s.dec.get(x[1], []))
            elif o == OP_GROUP_MEMBER_DECORATE:
                for t, k in zip(x[2::2], x[3::2]): s.mdec[t].extend([k] + d for d in s.dec.get(x[1], []))
            elif o == OP_ENTRY_POINT:
                nw = len(lit_str(x[3:]).encode()) // 4 + 1; s.entries.append((x[1], x[2], lit_str(x[3:]), 3 + nw))   # model, function, name, first interface word
            elif 19 <= o <= 39 or o in (322, 4472, 5341): s.types[x[1]] = x
            elif o == OP_CONSTANT: s.consts[x[2]] = (x[1], x[3:])
            elif o in SPEC_CONSTANT_OPS: s.spec.add(x[2])
            elif o == OP_VARIABLE and not infn: s.vars[x[2]] = (x[1], x[3])
        s.entry = s.entries[0] if s.entries else None   # the first: a module with more than one is refused (structure)
    def op(s, x): return x[0] & 0xffff
    def first_fn(s): return next(i for i, x in enumerate(s.ins) if s.op(x) == OP_FUNCTION)
    def interface(s): return [x for x in s.ins if s.op(x) == OP_ENTRY_POINT][0][s.entry[3]:]
    def const_u(s, i): c = s.consts.get(i); return c[1][0] if c and s.types[c[0]][0] & 0xffff == OP_TYPE_INT else None
    def has_dec(s, i, d): return any(x[0] == d for x in s.dec.get(i, []))
    def dec_val(s, i, d): return next((x[1] for x in s.dec.get(i, []) if x[0] == d and len(x) > 1), None)
    def pointee(s, v): return s.types[s.vars[v][0]][3]
    def strip_arrays(s, t):   # → (element type, count, None | 'runtime' | 'spec'): count None for a runtime array or one sized by a specialization constant
        count, kind = 1, None
        while s.op(s.types[t]) in (OP_TYPE_ARRAY, OP_TYPE_RTARRAY):
            x = s.types[t]; t = x[2]
            if s.op(x) == OP_TYPE_RTARRAY: kind = kind or 'runtime'
            elif x[3] in s.spec or s.const_u(x[3]) is None: kind = 'spec'
            else: count *= s.const_u(x[3])
        return t, (None if kind else count), kind
    def global_insert(s, x): s.ins.insert(s.first_fn(), x)
    def annotation_insert(s, x):
        last = max((i for i, y in enumerate(s.ins) if s.op(y) in ANNOTATION_OPS), default=None)
        s.ins.insert(last + 1 if last is not None else s.first_fn(), x)
    def name_insert(s, x):
        last = max((i for i, y in enumerate(s.ins) if s.op(y) in (OP_NAME, OP_MEMBER_NAME)), default=None)
        if last is not None: s.ins.insert(last + 1, x)
    def type_id(s, op, *args):   # an existing type declared with these operands, else a new one (non-aggregate types may be declared only once)
        for x in s.ins:
            if s.op(x) == op and x[2:] == list(args): return x[1]
        t = s.new_id(); s.global_insert([op | (2 + len(args)) << 16, t, *args]); s.types[t] = s.ins[s.first_fn() - 1]; return t
    def const_id(s, ty, *words):
        for x in s.ins:
            if s.op(x) == OP_CONSTANT and x[1] == ty and x[3:] == list(words): return x[2]
        c = s.new_id(); s.global_insert([OP_CONSTANT | (3 + len(words)) << 16, ty, c, *words]); s.consts[c] = (ty, list(words)); return c
    def composite_id(s, ty, *parts):
        for x in s.ins:
            if s.op(x) == OP_CONSTANT_COMPOSITE and x[1] == ty and x[3:] == list(parts): return x[2]
        c = s.new_id(); s.global_insert([OP_CONSTANT_COMPOSITE | (3 + len(parts)) << 16, ty, c, *parts]); return c
    def scalar(s, t):   # → (float|sint|uint, components, locations) of a scalar / vector / array of them, None otherwise
        x = s.types.get(t); o = s.op(x) if x else None
        if o == OP_TYPE_FLOAT: return ('float' if x[2] == 32 else f'float{x[2]}', 1, 1)
        if o == OP_TYPE_INT: return (('sint' if x[3] else 'uint') + ('' if x[2] == 32 else str(x[2])), 1, 1)
        if o == OP_TYPE_VECTOR: b = s.scalar(x[2]); return (b[0], x[3], 1) if b else None
        if o == OP_TYPE_ARRAY: b = s.scalar(x[2]); n = s.const_u(x[3]); return (b[0], b[1], b[2] * n) if b and n else None
        return None
    def dims(s, t):   # an array type's lengths, outermost first ([] for a scalar / vector)
        out = []
        while s.op(s.types[t]) == OP_TYPE_ARRAY: out.append(s.const_u(s.types[t][3])); t = s.types[t][2]
        return out
    def io(s, sc):   # user (non-built-in) interface variables of one storage class, one entry per location → [{location, type, components, interp, id, array (an array's {location (its first), dims})}]
        out = []
        for v in s.interface():
            if v not in s.vars or s.vars[v][1] != sc or s.has_dec(v, DEC_BUILTIN): continue
            t = s.pointee(v)
            if s.op(s.types[t]) == OP_TYPE_STRUCT and any(x[1] == DEC_BUILTIN for x in s.mdec.get(t, [])): continue   # gl_PerVertex
            patch, ex = s.has_dec(v, DEC_PATCH), EXEC_OF.get(s.entry[0])
            if not patch and s.op(s.types[t]) == OP_TYPE_ARRAY and (ex in ('geom', 'tesc', 'tese') if sc == SC_INPUT else ex == 'tesc'): t = s.types[t][2]   # per-vertex arrays (a Patch one is per patch)
            d, loc = s.scalar(t), s.dec_val(v, DEC_LOCATION)
            if d is None or loc is None: continue
            arr = {'array': {'location': loc, 'dims': s.dims(t)}} if s.dims(t) else {}   # Vulkan matches variables, not locations: a vec4[2] is not two vec4s (pair_inputs)
            for k in range(d[2]): out.append(dict(location=loc + k, type=d[0], components=d[1], interp=[INTERP[i] for i in INTERP if s.has_dec(v, i)], id=v, **arr, **({'patch': True} if patch else {})))
        return sorted(out, key=lambda e: e['location'])
    def fn_refs(s):   # every word inside function bodies, for "is this global referenced" (a literal that equals an ID only makes a pass more careful)
        refs, infn = collections.Counter(), False
        for x in s.ins:
            o = s.op(x)
            if o == OP_FUNCTION: infn = True
            if infn: refs.update(x[1:])
            if o == OP_FUNCTION_END: infn = False
        return refs
EXEC_OF = {0: 'vert', 1: 'tesc', 2: 'tese', 3: 'geom', 4: 'frag', 5: 'comp'}

def descriptor(m, v):   # → (VkDescriptorType, count, image facts or None, None | 'runtime' | 'spec') for a resource variable, None for anything else
    ptr, sc = m.vars[v]; base, count, kind = m.strip_arrays(m.types[ptr][3]); x = m.types[base]; o = m.op(x)
    if sc == SC_STORAGE_BUFFER: return 7, count, None, kind
    if sc == SC_UNIFORM: return (7 if m.has_dec(base, DEC_BUFFERBLOCK) else 6), count, None, kind
    if sc != SC_UNIFORM_CONSTANT: return None
    if o == OP_TYPE_SAMPLER: return 0, count, None, kind
    img = m.types[x[2]] if o == OP_TYPE_SAMPLED_IMAGE else x if o == OP_TYPE_IMAGE else None
    if img is None: return None
    st = m.scalar(img[2]); info = dict(dim=DIM.get(img[3], img[3]), arrayed=bool(img[5]), ms=bool(img[6]), depth=img[4], sampled=img[7], comp=st[0] if st else '?', format=img[8])
    buf = img[3] == 5
    if o == OP_TYPE_SAMPLED_IMAGE: return (4 if buf else 1), count, info, kind
    if img[3] == 6: return 10, count, info, kind
    return ((5 if buf else 3) if img[7] == 2 else (4 if buf else 2)), count, info, kind

def structure(m, stage):
    """What the passes and the reflection do not follow, checked on the translator's module before any pass: more than one entry point (the interface, init and the reflection take one),
    decoration groups (the passes edit decorations one OpDecorate at a time: a grouped Binding would be dropped, a removed variable left in OpGroupDecorate), an array sized by a
    specialization constant (its size is the pipeline's), and a user interface variable other than a 32-bit scalar, vector or array of them with a Location and no Component (pad, init and
    the pair coverage track whole locations). → reasons, [] for none; a module with any is not passed through the passes and is written unsupported (step 10)"""
    why = []
    if len(m.entries) != 1: why.append(f'{len(m.entries)} entry points ({", ".join(e[2] for e in m.entries)}): the passes and the reflection follow one')
    if m.groups: why.append(f'decoration groups (OpDecorationGroup {sorted(m.groups)}): the passes edit decorations one OpDecorate at a time')
    sp = sorted(m.names.get(v, f'%{v}') for v in m.vars if m.strip_arrays(m.pointee(v))[2] == 'spec') or sorted(f'%{t}' for t, x in m.types.items() if m.op(x) == OP_TYPE_ARRAY and x[3] in m.spec)
    if sp: why.append(f'arrays sized by a specialization constant ({", ".join(sp)}): their size is the pipeline\'s, not the module\'s')
    if not m.entry: return why
    ex = EXEC_OF.get(m.entry[0])
    for v in m.interface():
        if v not in m.vars or m.vars[v][1] not in (SC_INPUT, SC_OUTPUT) or m.has_dec(v, DEC_BUILTIN): continue
        sc, t, nm = m.vars[v][1], m.pointee(v), m.names.get(v, f'%{v}')
        if not m.has_dec(v, DEC_PATCH) and m.op(m.types[t]) == OP_TYPE_ARRAY and (ex in ('geom', 'tesc', 'tese') if sc == SC_INPUT else ex == 'tesc'): t = m.types[t][2]   # per-vertex arrays, as io()
        base, side = m.strip_arrays(t)[0], 'input' if sc == SC_INPUT else 'output'
        if m.op(m.types[base]) == OP_TYPE_STRUCT:
            bi = {d[0] for d in m.mdec.get(base, []) if d[1] == DEC_BUILTIN}
            if len(bi) == len(m.types[base]) - 2: continue   # a built-in block (gl_PerVertex)
            why.append(f'{side} {nm}: a struct / block interface variable (pad, init and the reflection take scalars, vectors and arrays of them)'); continue
        d = m.scalar(t)
        if d is None or d[0] not in ('float', 'sint', 'uint'):
            why.append(f'{side} {nm}: a {"matrix / " if m.op(m.types[base]) == OP_TYPE_MATRIX else ""}type other than a 32-bit scalar, vector or array of them'); continue
        if m.dec_val(v, DEC_LOCATION) is None: why.append(f'{side} {nm}: no Location')
        if m.has_dec(v, DEC_COMPONENT): why.append(f'{side} {nm}: a Component decoration (pad, init and the pair coverage track whole locations)')
    return why

# ---------------------------------------------------------------- passes
NO_RESULT = {0, 8, 56, 62, 63, 64, 99, 218, 219, 220, 221, 224, 225, 228, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255, 256, 317, 4416, 5380}   # body opcodes without result type + ID
def select_fix(m):
    """OpSelect with a scalar condition and a vector result (spv_single_lane's ballot) is valid only from SPIR-V 1.4: before it, splat the condition to a bool vector"""
    boolt = next((x[1] for x in m.ins if m.op(x) == OP_TYPE_BOOL), None)
    if m.version() >= (1, 4) or boolt is None: return 0
    rtype, infn = {}, False
    for x in m.ins:
        o = m.op(x)
        if o == OP_FUNCTION: infn = True
        if len(x) > 2 and ((infn and o not in NO_RESULT) or o in (41, 42, 43, 44, 45, 46, 48, 49, 50, 51, 52)): rtype[x[2]] = x[1]
        if o == OP_FUNCTION_END: infn = False
    fix = [x for x in m.ins if m.op(x) == OP_SELECT and m.op(m.types.get(x[1], [0])) == OP_TYPE_VECTOR and rtype.get(x[3]) == boolt]
    for x in fix:
        k = m.types[x[1]][3]; bv = m.type_id(OP_TYPE_VECTOR, boolt, k); c = m.new_id()
        i = next(i for i, y in enumerate(m.ins) if y is x); m.ins.insert(i, [OP_COMPOSITE_CONSTRUCT | (3 + k) << 16, bv, c] + [x[3]] * k); x[3] = c
    if fix: m.index()
    return len(fix)

def nminmax(m):
    glsl = {x[1] for x in m.ins if m.op(x) == OP_EXT_IMPORT and lit_str(x[2:]) == 'GLSL.std.450'}; n = collections.Counter()
    for x in m.ins:
        if m.op(x) == OP_EXT_INST and x[3] in glsl and x[4] in GLSL_N: n[x[4]] += 1; x[4] = GLSL_N[x[4]]
    return {'FMin': n[37], 'FMax': n[40], 'FClamp': n[43]}

def pervertex(m, keep=None):
    """Cut the ClipDistance / CullDistance members of each built-in output block (gl_PerVertex; the per-vertex arrays of geometry and tessellation stages too) to what the module indexes:
    a member never indexed is removed (the later members' access chains renumbered), one indexed only by constants is sized to the highest + 1, a dynamic index keeps the size.
    Linked stages' blocks must match (VUID-RuntimeSpirv-OpVariable-08746), so an input block is never cut, and an output block only when keep is None (cut_reason: the last stage before
    rasterization). → description of the change, '' for none; a block left as it is says why ('kept ...'), as does one reached other than by access chains with constant member indices"""
    out = []
    for st in sorted({t for t, ds in m.mdec.items() if any(d[1] == DEC_BUILTIN for d in ds)}):
        bi = {d[0]: d[2] for d in m.mdec[st] if d[1] == DEC_BUILTIN}; members = m.types[st][2:]
        cut = {i: m.const_u(m.types[members[i]][3]) for i in range(len(members)) if bi.get(i) in (BI_CLIP, BI_CULL) and m.op(m.types[members[i]]) == OP_TYPE_ARRAY}
        if not cut: continue
        vs = {v: (0 if m.pointee(v) == st else 1) for v in m.vars if m.strip_arrays(m.pointee(v))[0] == st}
        if not vs: continue
        if any(m.vars[v][1] != SC_OUTPUT for v in vs): out.append(f'kept {m.names.get(st, st)}: an input block (it matches the previous stage\'s output)'); continue
        if keep: out.append(f'kept {m.names.get(st, st)}: {keep}'); continue
        need, why, infn, chains = {i: 0 for i in cut}, None, False, []
        for x in m.ins:
            if m.op(x) == OP_FUNCTION: infn = True
            elif m.op(x) == OP_FUNCTION_END: infn = False
            if not infn: continue
            if m.op(x) in (OP_ACCESS_CHAIN, OP_IB_ACCESS_CHAIN) and x[3] in vs:
                idx, p = x[4:], vs[x[3]]
                if len(idx) <= p or m.const_u(idx[p]) is None: why = 'access chain without a constant member index'; break
                k = m.const_u(idx[p]); chains.append((x, 4 + p, k))
                if k in need: need[k] = max(need[k], m.const_u(idx[p + 1]) + 1) if len(idx) > p + 1 and m.const_u(idx[p + 1]) is not None else cut[k]
            elif any(v in x[1:] for v in vs): why = f'used by opcode {m.op(x)}'; break
        if why: out.append(f'kept {m.names.get(st, st)}: {why}'); continue
        if all(need[k] >= cut[k] for k in cut): continue
        kept = [i for i in range(len(members)) if not (i in cut and need[i] == 0)]; nidx = {o: n for n, o in enumerate(kept)}
        for x, at, k in chains:   # a removed member's index is never in a chain (need 0); a later member's moves down
            if nidx[k] != k: x[at] = m.const_id(m.consts[x[at]][0], nidx[k])
        x = m.types[st]; i = next(i for i, y in enumerate(m.ins) if y is x); new = []
        for k in kept:
            if k in cut and need[k] < cut[k]:   # a new array type and length, declared just before the block
                old = m.types[members[k]]; ln, at = m.new_id(), m.new_id()
                m.ins[i:i] = [[OP_CONSTANT | 4 << 16, m.consts[old[3]][0], ln, need[k]], [OP_TYPE_ARRAY | 4 << 16, at, old[2], ln]]; i += 2; new.append(at)
            else: new.append(members[k])
        x[:] = [OP_TYPE_STRUCT | (2 + len(new)) << 16, st] + new
        m.ins = [y for y in m.ins if not (m.op(y) in (OP_MEMBER_NAME, OP_MEMBER_DECORATE) and y[1] == st and y[2] not in nidx)]
        for y in m.ins:
            if m.op(y) in (OP_MEMBER_NAME, OP_MEMBER_DECORATE) and y[1] == st: y[2] = nidx[y[2]]
        out.append(f'{m.names.get(st, st)}: ' + ', '.join(f'{"ClipDistance" if bi[k] == BI_CLIP else "CullDistance"}[{cut[k]}] → ' + ('removed' if not need[k] else f'[{need[k]}]') for k in sorted(cut)))
        m.index()
    return '; '.join(out)

def unused(m):   # descriptor variables no function references → removed, with their names and decorations; → their names
    refs = m.fn_refs(); gone = {v for v, (p, sc) in m.vars.items() if sc in (SC_UNIFORM_CONSTANT, SC_UNIFORM, SC_STORAGE_BUFFER) and m.has_dec(v, DEC_BINDING) and not refs[v]}
    if not gone: return []
    names = sorted(m.names.get(v, f'%{v}') for v in gone)
    m.ins = [x for x in m.ins if not ((m.op(x) == OP_VARIABLE and x[2] in gone) or (m.op(x) in (OP_NAME, OP_DECORATE) and x[1] in gone))]
    for x in m.ins:
        if m.op(x) == OP_ENTRY_POINT and any(v in gone for v in x[m.entry[3]:]):   # SPIR-V 1.4+ lists every global in the interface
            x[:] = x[:m.entry[3]] + [v for v in x[m.entry[3]:] if v not in gone]; x[0] = len(x) << 16 | OP_ENTRY_POINT
    m.index(); return names

def sets(m, stage):
    s = STAGE_SET[stage]
    for x in m.ins:
        if m.op(x) == OP_DECORATE and x[2] == DEC_SET: x[3] = s
    for v in m.vars:
        if m.has_dec(v, DEC_BINDING) and not m.has_dec(v, DEC_SET): m.annotation_insert([OP_DECORATE | 4 << 16, v, DEC_SET, s])
    m.index()

def scalar_type(m, ty, n):   # 'float' / 'sint' / 'uint' and a component count → the type id (declared when missing)
    c = m.type_id(OP_TYPE_FLOAT, 32) if ty == 'float' else m.type_id(OP_TYPE_INT, 32, int(ty == 'sint'))
    return c, (m.type_id(OP_TYPE_VECTOR, c, n) if n > 1 else c)

def pad(m, want):
    """An output for each pixel shader input variable the vertex shader writes at none of its locations (want: first location → entry, pair_inputs), declared as the pixel shader
    declares it (an array stays one array: Vulkan matches variables, not locations) and with no interpolation decoration (it takes no part in matching and has no effect on a vertex
    output: the pixel shader's own decide; an integer input's Flat is on the pixel shader's side, VUID-StandaloneSpirv-Flat-04744), as Metal's vs_pad; → the locations padded"""
    ep, done = next(x for x in m.ins if m.op(x) == OP_ENTRY_POINT), []
    for l, e in sorted(want.items()):
        _, t = scalar_type(m, e['type'], e['components'])
        for n in reversed(e['array']['dims'] if e.get('array') else []): t = m.type_id(OP_TYPE_ARRAY, t, m.const_id(m.type_id(OP_TYPE_INT, 32, 0), n))
        p = m.type_id(OP_TYPE_POINTER, SC_OUTPUT, t); v = m.new_id()
        m.global_insert([OP_VARIABLE | 4 << 16, p, v, SC_OUTPUT]); m.annotation_insert([OP_DECORATE | 4 << 16, v, DEC_LOCATION, l])
        m.name_insert([OP_NAME | (2 + len(str_words(f'dm_pad{l}'))) << 16, v] + str_words(f'dm_pad{l}'))
        ep.append(v); ep[0] = len(ep) << 16 | OP_ENTRY_POINT; done += e['locs']
    m.index(); return sorted(done)

def init_value(m, t):   # the constant (0,0,0,1) of a 32-bit scalar / vector type, or of an array of them element by element (declared when missing); None for another type
    x = m.types[t]; o = m.op(x)
    if o == OP_TYPE_ARRAY:
        e = init_value(m, x[2]); n = m.const_u(x[3])
        return m.composite_id(t, *[e] * n) if e is not None and n else None
    c, n = (x[2], x[3]) if o == OP_TYPE_VECTOR else (t, 1)
    if m.op(m.types[c]) not in (OP_TYPE_FLOAT, OP_TYPE_INT) or m.types[c][2] != 32 or not 1 <= n <= 4: return None
    cs = [m.const_id(c, 0x3f800000 if (k and m.op(m.types[c]) == OP_TYPE_FLOAT) else k) for k in [0, 0, 0, 1][:n]]
    return m.composite_id(t, *cs) if n > 1 else cs[0]

def init(m):
    """Every user output of a vertex shader stored (0,0,0,1) at the top of the entry function, before the body (Metal's vs_init), an array output in every element; → the locations"""
    outs, stores = m.io(SC_OUTPUT), []
    for e in {e['id']: e for e in outs}.values():
        c = init_value(m, m.pointee(e['id']))
        if c is not None: stores.append([OP_STORE | 3 << 16, e['id'], c])
    if not stores: return []
    f = next(i for i, x in enumerate(m.ins) if m.op(x) == OP_FUNCTION and x[2] == m.entry[1])
    i = next(i for i in range(f, len(m.ins)) if m.op(m.ins[i]) == OP_LABEL) + 1
    while m.op(m.ins[i]) in (OP_VARIABLE, OP_LINE, OP_NO_LINE): i += 1
    m.ins[i:i] = stores; m.index()
    return sorted({e['location'] for e in outs if any(s[1] == e['id'] for s in stores)})

# ---------------------------------------------------------------- reflection
def nvn_fields(M, stage, name, desc, arrayed, binding, res):
    """dmsh_build.py reflect() for one binding, decided from its name as there (spirv-cross names the MSL members after the SPIR-V names), with the same branch order; a combined image
    sampler gets the fields of Metal's <name>Smplr entry under 'sampler'. Storage buffers match res.json by binding number (Metal goes by the name's index into the sorted list)"""
    texbuf, e, smp = desc in (4, 5), {}, None
    def samp_of(nm):
        u = M['decode_ubo_tex'](nm)
        if u: return dict(kind='samp_id', reg=u[1][1]) if u[1][0] == 'id' else dict(kind='samp_ubo', reg=u[1][0], off=u[1][1])
        if M['c2_array'](nm, 'array<' if arrayed else '') is not None: return dict(kind='samp_c2a', off=M['c2_array'](nm, 'array<'))
        d = M['decode_tex'](nm)
        if not d: return dict(kind='?')
        t, s, ty = d; return dict(kind='samp_id', reg=M['const_sampler'](nm)) if ty == 3 else dict(kind='samp', reg=s) if ty else dict(kind='samp_tex', reg=t)
    if desc == 0: return {'sampler': samp_of(name[:-5] if name.endswith('Smplr') else name)}   # a separate sampler (no Stardew shader has one)
    if name in dmsh_geometry.HELPERS: e.update(kind='geometry', reg=dmsh_geometry.HELPERS[name])
    elif name == 'support_buffer': e.update(kind='support')
    elif re.fullmatch(r'[a-z]+_c(\d+)', name):
        n = int(re.fullmatch(r'[a-z]+_c(\d+)', name).group(1)); e.update(kind='cbuf', reg=n - 3) if 3 <= n <= 16 else e.update(kind='c1' if n == 1 else 'drvcb', bank=n)
    elif re.fullmatch(r'[a-z]+_s(\d+)', name):
        e.update(kind='sbuf', index=int(re.fullmatch(r'[a-z]+_s(\d+)', name).group(1)))
        b = next((x for x in res['sbufs'] if x['binding'] == binding), None); base = M['SSBO'].get(stage)
        off = b['cb_offset'] * 4 if b and b['cb_slot'] == 0 else None
        if stage == 'cs' and off is not None and 0x210 <= off < 0x310 and (off - 0x210) % 16 == 0: e.update(kind='ubo_ptr', reg=(off - 0x210) // 16)
        elif b and b['cb_slot'] >= 3: e.update(kind='sbuf_ubo', reg=b['cb_slot'] - 3, off=b['cb_offset'] * 4)
        else: e['reg'] = (off - base) // 16 if off is not None and base is not None and 0 <= off - base < 0x100 and (off - base) % 16 == 0 else None
        e['rw'] = bool(b and b['flags'] & 2)
    elif M['decode_ubo_tex'](name):
        ut, _ = M['decode_ubo_tex'](name); e.update(kind='?') if texbuf else e.update(kind='tex_ubo', reg=ut[0], off=ut[1])
    elif M['c2_array'](name, 'array<' if arrayed else '') is not None: e.update(kind='tex_c2a', off=M['c2_array'](name, 'array<'))
    else:
        d = M['decode_tex'](name)
        if not d: e.update(kind='?')
        else: e.update(kind='img' if re.match(r'[a-z]+_i', name) else ('tbuf' if texbuf else 'tex'), reg=d[0])
        if e.get('kind') == 'tex' and e.get('reg') in res.get('fetch1d', ()): e['fetch1d'] = True
    if desc == 1: smp = samp_of(name)   # a texel buffer has no sampler (spirv-cross writes no <name>Smplr for it)
    return dict(e, **({'sampler': smp} if smp else {}))

def size_queried(m, v):   # OpImageQuerySize(Lod) on an image loaded from v (Metal: <name>.get_width( in the MSL)
    ids = {v}
    for x in m.ins:
        o = m.op(x)
        if (o in (OP_ACCESS_CHAIN, OP_IB_ACCESS_CHAIN, OP_LOAD, OP_IMAGE)) and x[3] in ids: ids.add(x[2])
        elif o in (OP_QUERY_SIZE, OP_QUERY_SIZE_LOD) and x[3] in ids: return True
    return False

def drv_reads(m):   # as dmsh_build.py drv_reads, from the SPIR-V and for every stage: the vec4 rows read from the driver segments c0 / c2 (<stage>_c0 / _c2), or dynamic
    out = {}
    for v, nm in m.names.items():
        b = re.fullmatch(r'[a-z]+_c([02])', nm)
        if not b or v not in m.vars: continue
        offs, dyn = set(), False
        for x in m.ins:
            if m.op(x) in (OP_ACCESS_CHAIN, OP_IB_ACCESS_CHAIN) and x[3] == v:
                k = m.const_u(x[5]) if len(x) > 5 else None
                if k is None: dyn = True
                else: offs.add(k * 16)
        if offs or dyn: out[b.group(1)] = {'vec4_offsets': sorted(offs), 'dynamic': dyn}
    return out

def reflect(M, m, stage, res):
    binds = []
    for v in m.vars:
        d = descriptor(m, v)
        if d is None or not m.has_dec(v, DEC_BINDING): continue
        ty, count, info, kind = d; name = m.names.get(v, f'%{v}'); b = m.dec_val(v, DEC_BINDING)
        e = dict(set=m.dec_val(v, DEC_SET), binding=b, type=ty, type_name=VK_DESC[ty], count=count, **({'runtime': True} if kind == 'runtime' else {'spec_sized': True} if kind else {}),
                 name=name, **({'image': info} if info else {}))
        e.update(nvn_fields(M, stage, name, ty, m.op(m.types[m.pointee(v)]) in (OP_TYPE_ARRAY, OP_TYPE_RTARRAY), b, res))
        if e.get('fetch1d') and size_queried(m, v): e['fetch1d_size'] = True
        binds.append(e)
    return sorted(binds, key=lambda e: (-1 if e['set'] is None else e['set'], e['binding']))

def io_list(m, sc): return [{**{k: e[k] for k in ('location', 'type', 'components', 'interp', 'array') if k in e}, **({'patch': True} if e.get('patch') else {})} for e in m.io(sc)]

def cut_reason(stage, pre):
    """None when this stage's gl_PerVertex output block may be cut (the last stage before rasterization: nothing reads the block but the fixed function), else why it stays full.
    pre: the game's gs / hs / ds stages (no pipeline list says which vertex shader feeds which, so a vertex shader of a game with any of them keeps its block)"""
    if stage == 'vs': return None if not pre else f'the game has {"/".join(sorted(pre))} shaders: a vertex shader may feed one, whose input block must match its output block'
    if stage == 'gs': return None
    if stage == 'ds': return None if 'gs' not in pre else 'the game has geometry shaders: one may read this block'
    if stage == 'hs': return 'a tessellation-control output block feeds the tessellation-evaluation input block'
    return 'not a pre-rasterization stage'

def refusals(m, stage):
    """Why the module, valid as it is, must not be used: a stage built for one fixed draw state, a capability or extension not every target driver supports (CAP_OK / EXT_OK: Float64,
    the interlock, NVIDIA's and NVX's among them), subgroup operations in a stage no target driver runs them in. → reasons, [] for none. A backend refuses a module with any (its
    reflection's status), so a pipeline with it fails instead of drawing wrongly"""
    caps = [x[1] for x in m.ins if m.op(x) == OP_CAPABILITY]; exts = [lit_str(x[1:]) for x in m.ins if m.op(x) == OP_EXTENSION]; why = []
    if stage in FIXED_STATE: why.append(FIXED_STATE[stage])
    bad = [f'{CAP_NAME.get(c, "capability")} ({c})' for c in caps if c not in CAP_OK]
    if bad: why.append(f'capabilities not every target driver supports: {", ".join(bad)}')
    bad = [e for e in exts if e not in EXT_OK]
    if bad: why.append(f'extensions not every target driver supports: {", ".join(bad)}')
    if any(sc == 9 for _, sc in m.vars.values()): why.append('push constants: the reflection has no push-constant range')
    sg = [c for c in caps if c in CAP_SUBGROUP]
    if sg and stage not in SG_STAGES: why.append(f'subgroup capabilities {sg} in a {stage} shader: the target drivers run subgroup operations only in fragment and compute shaders'
                                                 + (' (spv_single_lane met an operation it does not cover)' if stage == 'vs' else ''))
    return why

def first(e): return e['array']['location'] if e.get('array') else e['location']   # an io() entry's variable: its first location and its type (base, components, array lengths)
def shape(e): return e['type'], e['components'], tuple(e['array']['dims']) if e.get('array') else ()
def pair_inputs(m, pss):
    """The outputs a vertex shader must add for its paired pixel shaders (pss: [(key, their io(SC_INPUT) entries)]), and why it cannot serve them all. Vulkan matches a pixel shader input
    variable with the output at its Location by an equivalent type (Interface Matching: not a vec4[2] with two vec4s, not a vec2 with a vec4 unless the pipeline enables maintenance4,
    which nothing asks a backend to do), interpolation decorations excepted (graphics pipeline libraries aside). So per input variable (first location, base type, components, array
    lengths): pixel shaders reading one location as different variables, or a vertex output there that is not the same variable, make it unsupported (flat and smooth readers share an
    output); a variable the vertex shader writes at none of its locations is added (pad). → ({first location: entry with its locations, for pad}, reasons)"""
    want, at, why = {}, {}, []   # at: location → the first location of the wanted variable on it
    desc = lambda e: f'{e["type"]} x{e["components"]}' + ''.join(f'[{n}]' for n in shape(e)[2]) + f' at location {first(e)}'
    for ps, ins in pss:
        for f, e in {first(e): e for e in ins}.items():
            locs = sorted(x['location'] for x in ins if first(x) == f); w = next((want[at[l]] for l in locs if l in at), None)
            if w is None: want[f] = dict(e, location=f, src=[ps], locs=locs); at.update((l, f) for l in locs); continue
            if first(w) != f or shape(w) != shape(e): why.append(f'pixel shader {w["src"][0]} reads {desc(w)}, {ps} {desc(e)}: one vertex output cannot match both'); continue
            w['src'].append(ps)
    have, pads = {e['location']: e for e in m.io(SC_OUTPUT)}, {}
    for f, w in sorted(want.items()):
        os_ = [have[l] for l in w['locs'] if l in have]
        if not os_: pads[f] = w
        elif len(os_) != len(w['locs']) or any(first(o) != f or shape(o) != shape(w) for o in os_):
            why.append(f'output {desc(os_[0])}, where pixel shader {",".join(w["src"])} reads {desc(w)}: not an equivalent type (Vulkan interface matching)')
    return pads, why

def vk_module(m, stage, pre, single, pss, tmp):
    """The Vulkan steps for one translated module, after the translator and spv_single_lane (finish() runs it per shader, the self-tests on hand-written modules): structure, the passes,
    spv_strip_caps, spirv-val, refusals. pss: a vertex shader's paired pixel shaders' inputs (pair_inputs). tmp: the path spirv-val reads.
    → (module, passes, vs_pad, init, reasons it is unsupported (step 10), spirv-val's messages ('' for none: a module with any is not written))"""
    skip = structure(m, stage); vpad, vinit, why = [], [], list(skip)
    if skip: passes = {'skipped': 'structure: ' + '; '.join(skip)}   # written as translated, not passed through what it would break
    else:
        passes = {'single_lane': single, 'select_fix': select_fix(m) if single else 0}
        passes['nminmax'] = nminmax(m); passes['pervertex'] = pervertex(m, cut_reason(stage, pre)); passes['unused'] = unused(m); sets(m, stage)
        if stage == 'vs':
            want, conflict = pair_inputs(m, pss or []); why += conflict
            vpad = pad(m, want) if want else []; vinit = init(m)
        words = list(struct.unpack(f'<{len(m.bytes()) // 4}I', m.bytes())); words, passes['caps_dropped'] = spv_strip_caps.strip(words)
        words, passes['caps_added'] = spv_add_caps.add(words)
        words, passes['barrier_exits'], passes['barrier_guards'] = spv_barrier_exit.fix(words)
        m = Spv(struct.pack(f'<{len(words)}I', *words))
    open(tmp, 'wb').write(m.bytes())
    r = subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', tmp], capture_output=True, text=True)
    val = (r.stdout + r.stderr).strip() or (f'spirv-val exit {r.returncode}' if r.returncode else '')
    return m, passes, vpad, vinit, why + refusals(m, stage), val

def spv_reflection(M, m, stage, res, ran):   # the reflection fields read from the module written; ran: the passes ran (else set is null: the module keeps the translator's sets)
    binds = reflect(M, m, stage, res)
    return {'set': STAGE_SET[stage] if ran else None, 'bindings': binds,
            **({'vertex_inputs': io_list(m, SC_INPUT)} if stage == 'vs' else {'inputs': io_list(m, SC_INPUT)} if stage != 'cs' else {}),
            **({'outputs': io_list(m, SC_OUTPUT)} if stage != 'cs' else {})}

def out_values(M, msl, outs):
    """What a pixel shader writes to each colour output, from spirv-cross's MSL as Metal decides it (fs_uint; the translator declares every output float): 'float' a float value, 'uint' the
    bits of an integer (Metal's main0_u / main0_m return them as the uint vector of the same bits: an integer attachment takes them bit for bit), 'const' only constants (Metal gives a
    main0_u: either kind of attachment), 'unknown' (no MSL). → ({location: value}, Metal's uint_mask_m (colour N = location N) or 0)"""
    if not msl: return {e['location']: 'unknown' for e in outs}, 0
    src, mask = M['fs_uint'](msl)
    if src is None: return {e['location']: 'float' for e in outs}, 0
    if mask: return {e['location']: 'uint' if mask >> e['location'] & 1 else 'float' for e in outs}, mask
    st = re.search(r'struct main0_out\s*\{(.*?)\};', msl, re.S)   # fs_uint's from_int (its colour members, one written from integer bits): all of them; else it took the shader for constants
    cols = [nm for ty, nm, at in re.findall(r'^\s*(\w+)\s+(\w+)\s*(\[\[[^\]]*\]\])?;', st.group(1) if st else '', re.M) if 'color(' in at and ty.startswith('float')]
    bits = any(re.search(r'\bout\.%s(?:\.\w+)? = as_type<float\d?>\(' % nm, msl) for nm in cols)
    return {e['location']: 'uint' if bits else 'const' for e in outs}, 0

# ---------------------------------------------------------------- build
def main():
    a = sys.argv[1:]
    if a == ['--version']: print(version()); return
    if len(a) < 2: sys.exit('usage: dmsh_build_vk.py <Switch package dir> <output cache dir> [key...]   |   dmsh_build_vk.py --version')
    dm_dir, root, want = a[0], a[1], set(a[2:])
    bad = [b for b in dmsh_chain.check_tools() if not b.startswith('metal_')]   # the Metal compiler is not used here (its version is still in V, through the lock)
    if bad: sys.exit('\n'.join('[RED] translation chain tools do not match the lock file: ' + b for b in bad))
    if not shutil.which('spirv-val'): sys.exit('[RED] no spirv-val (Homebrew spirv-tools): every module is validated before it is written')
    V = version(); out = os.path.join(root, f'b-{V}-vk'); GAME = dmsh_chain.load_game(); INPUTS = dmsh_chain.inputs(GAME)
    g = dmsh_chain.read_stamp(out).get('game') if os.path.isdir(out) else None
    if g and g != GAME.id: sys.exit(f'[RED] {out} holds the shader modules of game {g}, not {GAME.id}: give each game its own output directory')
    import dmsh_facts
    if not os.path.exists(dmsh_facts.TABLE): sys.exit(f'[RED] no answer table {dmsh_facts.TABLE} (it comes with the game profile)')
    M = metal_chain(dmsh_facts.load()); XLATE = os.environ.get('XLATE', os.path.join(dmsh_chain.XLATE_DIR, 'dmsh_xlate'))
    MAP = str(GAME.table('shader_map.tsv')); loc, pc_of = {}, {}
    for ln in open(MAP):   # as dmsh_build.py reads the map and its conflicts table
        c = ln.rstrip('\n').split('\t')
        if len(c) > 4 and c[0].endswith('.bin'): pc_of.setdefault(c[3], c[4]); loc.setdefault(c[3], (c[0], int(c[1])))
    for ln in open(MAP + '.conflicts.tsv'):
        c = ln.rstrip('\n').split('\t')
        if len(c) > 1 and not ln.startswith(('#', 'dm_')) and c[0] in pc_of: pc_of[c[0]] = c[1]
    PRE = {n[:2] for n, _ in loc.values()} & set(FIXED_STATE)   # the game's gs / hs / ds stages, from every shader in the map: a partial build cuts gl_PerVertex as a full one
    PAIRS = collections.defaultdict(set)
    for ln in open(GAME.table('shader_pairs.tsv')):
        if ln.strip() and not ln.startswith('#'): v_, p_ = ln.split(); PAIRS[v_].add(p_)
    keys = sorted(want) if want else sorted(loc)
    os.makedirs(out, exist_ok=True); stamp_before = dmsh_chain.read_stamp(out).get('inputs')
    if os.path.exists(os.path.join(out, dmsh_chain.STAMP)): os.unlink(os.path.join(out, dmsh_chain.STAMP))   # before anything changes: a run that fails part way leaves no stamp
    for n in os.listdir(out):   # as the Metal build: what a run leaves is what it built
        mk = re.match(r'([0-9a-f]{16})_b', n)
        if mk and (not want or mk.group(1) in want): os.unlink(os.path.join(out, n))
    w = tempfile.mkdtemp(prefix='dmsh_vk_', dir=os.environ.get('WORK')); packs, jobs, c1s = {}, {}, {}
    for key in keys:
        name, k = loc[key]
        if name not in packs: packs[name] = M['records'](open(os.path.join(dm_dir, name), 'rb').read())
        jobs[key], c1s[key] = M['job_for'](key, name, packs[name][k], w)
    NJ = int(os.environ.get('J', '8')); chunks = [c for c in (keys[i::NJ] for i in range(NJ)) if c]
    def xl(ch):
        jp = os.path.join(w, f'job_{ch[0]}.json'); json.dump([jobs[k] for k in ch], open(jp, 'w'))
        return subprocess.run([XLATE, jp, w], capture_output=True, text=True)
    with cf.ThreadPoolExecutor(NJ) as ex: xr = list(ex.map(xl, chunks))
    xerr = {mm.group(1): mm.group(2)[:160] for r in xr for ln in r.stderr.splitlines() for mm in [re.match(r'\[RED\] (\w+): (.*)', ln)] if mm}

    def analysis_msl(p):   # spirv-cross's MSL with the Metal chain's options: int_inputs and raster_facts read it, as there
        r = subprocess.run(['spirv-cross', *M['XC'], p], capture_output=True, text=True)
        return (M['precise'](r.stdout), '') if not r.returncode else (None, r.stderr.strip()[:160])

    def finish(key):
        j, stage, p = jobs[key], jobs[key]['stage'], os.path.join(w, key + '.spv'); notes = []
        single = M['spv_single_lane'](p) if stage == 'vs' else False
        msl, why = analysis_msl(p)
        if stage == 'vs' and msl:   # the integer-input retranslation (dmsh_build.py finish, int_inputs)
            extra = {l: t for l, t in M['int_inputs'](msl).items() if l not in j['attr']}
            if extra:
                j['attr'].update(extra); jp = os.path.join(w, f'job1_{key}.json'); json.dump([j], open(jp, 'w'))
                rx = subprocess.run([XLATE, jp, w], capture_output=True, text=True)
                if rx.returncode or not os.path.exists(p): return 'translation failed', ('integer-input retranslation: ' + rx.stderr.strip())[:160], None
                single = M['spv_single_lane'](p) or single; msl, why = analysis_msl(p)
        if not msl: notes.append(f'no MSL for the analysis ({why}): integer inputs, raster facts and the integer-output check use defaults')
        res = json.load(open(os.path.join(w, key + '.res.json'))); m = Spv(open(p, 'rb').read()); pss = []
        if stage == 'vs' and PRE: notes.append(f'pad / init assume this vertex shader feeds the pixel shader directly; in a pipeline with a {"/".join(sorted(PRE))} shader that shader is refused (its status)')
        for ps in sorted(PAIRS.get(key, ())) if stage == 'vs' else ():   # the paired pixel shaders' inputs, from their written modules (they are built first)
            f = os.path.join(out, ps + '_b.spv' + ('.zst' if zstd else ''))
            if not os.path.exists(f): notes.append(f'paired pixel shader {ps} not built: not padded for it'); continue
            if json.load(open(os.path.join(out, ps + '_b.json')))['status'] != 'ok': notes.append(f'paired pixel shader {ps} unsupported: not padded for it'); continue
            pss.append((ps, Spv(zstd.decompress(open(f, 'rb').read()) if zstd else open(f, 'rb').read()).io(SC_INPUT)))
        m, passes, vpad, vinit, refuse, val = vk_module(m, stage, PRE, single, pss, os.path.join(w, key + '.vk.spv'))
        if val: return 'spirv-val failed', val.replace('\n', ' ')[:160], None
        if refuse: print(f'[RED] {key} ({stage}): unsupported on Vulkan: {"; ".join(refuse)}', flush=True)   # valid, but not usable as built: a backend refuses it instead of drawing wrongly
        ints = M['fs_uint'](msl)[0] is not None if stage == 'ps' and msl else False
        if ints: print(f'[RED] {key}: writes colour outputs from integer bits (Metal main0_u): no Vulkan variant yet, an integer attachment gets float outputs', flush=True)
        if j.get('tf'): print(f'[RED] {key}: transform feedback (layout {j["tf"]}): no Xfb decorations emitted yet', flush=True)
        data = m.bytes(); dr = drv_reads(m); name, k = loc[key]; fn = key + '_b.spv' + ('.zst' if zstd else ''); sr = spv_reflection(M, m, stage, res, 'skipped' not in passes); umask = 0
        if stage == 'ps':
            vals, umask = out_values(M, msl, sr['outputs'])
            for e in sr['outputs']: e['value'] = vals[e['location']]
        raster = M['raster_facts'](stage, msl) if msl else ({'pos_attr': -1} if stage == 'vs' else {'discard': 1, 'sample_mask': 1, 'colors': []} if stage == 'ps' else {})
        refl = {'key': key + '_b', 'route': 'B', 'target': 'vk', 'chain': V, 'status': 'unsupported' if refuse else 'ok', 'unsupported': refuse, 'dm_ctl': key, 'source': f'{name}#{k}', 'pc_paired': pc_of[key], 'stage': stage, 'exec_model': EXEC_MODEL.get(stage, stage),
                'entry': m.entry[2], 'spv': fn, 'spv_bytes': len(data), 'spv_sha256': hashlib.sha256(data).hexdigest()[:16], 'zstd': '.'.join(map(str, zstd.zstd_version_info)) if zstd else None,
                **sr, 'res': res, 'vs_pad': vpad, 'init': vinit,
                'c1': c1s[key].hex(), 'threadgroup_size': j.get('local'), 'notes': res['notes'] + (['not in the answer table, queries use defaults'] if j.get('no_facts') else []) + notes,
                'drv_reads': dr, 'drv_unfillable': M['unfillable'](dr), 'raster': raster,
                **({'tf': j['tf'], 'tf_strides': [e[2] if e else 0 for e in j['tf']]} if j.get('tf') else {}), **({'int_writer': True} if ints else {}), **({'uint_mask_m': umask} if umask else {}),
                'passes': passes}
        blob = zstd.compress(data, level=19) if zstd else data
        tmp = os.path.join(out, fn + '.tmp'); open(tmp, 'wb').write(blob); os.replace(tmp, os.path.join(out, fn))
        tmp = os.path.join(out, key + '_b.json.tmp'); json.dump(refl, open(tmp, 'w'), ensure_ascii=False, indent=1); os.replace(tmp, os.path.join(out, key + '_b.json'))
        return ('unsupported', '; '.join(refuse)[:160], refl) if refuse else ('ok', '', refl)

    def fin(key):
        if key in xerr or not os.path.exists(os.path.join(w, key + '.spv')): return key, 'translation failed', xerr.get(key, 'no spv'), None
        try: return (key,) + finish(key)
        except (ValueError, KeyError, IndexError, StopIteration, TypeError, struct.error) as e: return key, 'pass failed', f'{type(e).__name__}: {e}'[:160], None
    with cf.ThreadPoolExecutor(NJ) as ex:   # two phases, as the Metal build: a paired vertex shader reads its pixel shaders' modules (pad)
        res = list(ex.map(fin, [k for k in keys if k not in PAIRS])) + list(ex.map(fin, [k for k in keys if k in PAIRS]))
    if os.environ.get('DMSH_KEEP_WORK') == '1': print(f'work directory kept: {w} (<key>.spv is the translator output before the passes, <key>.vk.spv the module written)')
    else: shutil.rmtree(w, ignore_errors=True)
    res, unc = [list(r) for r in res], []; built = {r[0]: r for r in res if r[1] == 'ok'}
    for v_ in sorted(PAIRS):   # every pixel shader input matched by a vertex output of an equivalent type (Interface Matching; VUID-RuntimeSpirv-OpEntryPoint-08743 / -07754), from the reflections
        for p_ in sorted(PAIRS[v_]):
            if v_ in built and p_ in built:
                o = {e['location']: e for e in built[v_][3]['outputs']}
                miss = [e['location'] for e in built[p_][3]['inputs'] if e['location'] not in o or (first(o[e['location']]), shape(o[e['location']])) != (first(e), shape(e))]
                if miss: unc.append((v_, p_, miss))
    npairs = sum(1 for v_ in PAIRS for p_ in PAIRS[v_] if v_ in built and p_ in built)
    for v_, p_, miss in unc:   # a backstop (pair_inputs refuses what it finds): the vertex shader is refused, its reflection rewritten
        r = built[v_]; why = f'paired pixel shader {p_} reads locations {miss}, which this vertex shader does not write as the same variable (base type, components, array lengths)'
        r[3]['status'] = 'unsupported'; r[3]['unsupported'].append(why); r[1], r[2] = 'unsupported', why[:160]; print(f'[RED] {v_} (vs): unsupported on Vulkan: {why}', flush=True)
        tmp = os.path.join(out, v_ + '_b.json.tmp'); json.dump(r[3], open(tmp, 'w'), ensure_ascii=False, indent=1); os.replace(tmp, os.path.join(out, v_ + '_b.json'))
    st = collections.Counter(r[1] for r in res); ok = [r[3] for r in res if r[1] == 'ok']; done = ok + [r[3] for r in res if r[1] == 'unsupported']
    print(f'vk chain {V}: shaders {len(res)}: ' + ', '.join(f'{k} {v}' for k, v in st.most_common()) + f'; output {out} ({"zstd " + ".".join(map(str, zstd.zstd_version_info)) if zstd else "raw SPIR-V: this Python has no compression.zstd"})')
    for lvl in [k for k in st if k != 'ok']:
        for e, n in collections.Counter(r[2] for r in res if r[1] == lvl).most_common(10): print(f'  [{lvl}] {n:5d}  {e}')
    P = [x['passes'] for x in done if 'skipped' not in x['passes']]; mm = collections.Counter()   # what the passes did, in every module they ran on (an unsupported one included)
    for x in P: mm.update(x['nminmax'])
    cutn = sum(any(p and not p.startswith('kept') for p in x['pervertex'].split('; ')) for x in P)
    kept = collections.Counter(p.split(': ', 1)[1] for x in P for p in x['pervertex'].split('; ') if p.startswith('kept'))
    print(f'single-lane rewrites {sum(bool(x["single_lane"]) for x in P)} (ballot selects splatted {sum(x["select_fix"] for x in P)}); FMin/FMax/FClamp → N* {mm["FMin"]}/{mm["FMax"]}/{mm["FClamp"]}; '
          f'gl_PerVertex cut in {cutn}' + ''.join(f'\n  [kept] {n:5d}  {k}' for k, n in kept.most_common()))
    print('unused descriptors removed: ' + (', '.join(f'{k} {v}' for k, v in collections.Counter(n for x in P for n in x['unused']).most_common()) or 'none') +
          '; capabilities dropped: ' + (', '.join(f'{k} in {v}' for k, v in sorted(collections.Counter(c for x in P for c in x['caps_dropped']).items())) or 'none') +
          f'; passes skipped (structure) {len(done) - len(P)}')
    vs_ok = [x for x in ok if x['stage'] == 'vs']
    print(f'vertex shaders: {sum(bool(x["vs_pad"]) for x in vs_ok)} padded ({collections.Counter(str(x["vs_pad"]) for x in vs_ok if x["vs_pad"])}), {sum(bool(x["init"]) for x in vs_ok)} with outputs initialised (0,0,0,1)')
    print(f'shader_pairs.tsv pairs with both shaders built {npairs}: {len(unc)} with a pixel shader input no vertex output matches' + (' (those vertex shaders refused)' if unc else ''))
    ov = collections.Counter(e.get('value') for x in ok if x['stage'] == 'ps' for e in x['outputs'])
    print(f'shaders reading driver segments {sum(bool(x["drv_reads"]) for x in ok)}; reading what the replay layer cannot fill {sum(bool(x["drv_unfillable"]) for x in ok)}; '
          f'bindings of unknown kind {sum(1 for x in ok for b in x["bindings"] if b.get("kind") == "?" or b.get("sampler", {}).get("kind") == "?")}; pixel shader colour outputs by value {dict(ov)}')
    json.dump([{'key': r[0], 'st': r[1], 'err': r[2]} for r in res], open(os.path.join(out, '_b_build.json'), 'w'), ensure_ascii=False)
    if st['unsupported']: print(f'unsupported on Vulkan {st["unsupported"]} (written with status unsupported: a backend refuses them): ' + ', '.join(f'{k} {v}' for k, v in collections.Counter(x['stage'] for x in done if x['status'] != 'ok').most_common()))
    if len(done) == len(res) and (not want or stamp_before == INPUTS):   # every shader has its output (usable or refused), and a partial run replaced part of a complete directory
        dmsh_chain.write_stamp(out, GAME.id, INPUTS); print(f'stamp: game {GAME.id}, inputs {INPUTS}')
    else: print('no stamp: ' + ('not every shader built' if len(done) != len(res) else 'a partial run into a directory that was not complete for these inputs'))
    return 0 if st['ok'] == len(res) else 1

if __name__ == '__main__': sys.exit(main())
