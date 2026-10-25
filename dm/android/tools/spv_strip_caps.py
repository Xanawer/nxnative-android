#!/usr/bin/env python3
# Drops the capabilities the shader translator declares on every module but a module may not use (docs/android/PLAN.md, Phase 0 P0.8):
#   Float64 when the module declares no 64-bit float type, the GroupNonUniform* family when it has no subgroup instruction and no subgroup built-in, and the
#   fragment shader interlock capabilities (with their OpExtension) when it has no interlock instruction or execution mode.
# Adreno's drivers report no shaderFloat64 and no VK_EXT_fragment_shader_interlock, and spirv-opt --trim-capabilities keeps GroupNonUniformBallot / Shuffle and the
# interlock capability, so the Vulkan shader chain strips them here.
# Usage: spv_strip_caps.py <in.spv> <out.spv>   (prints the dropped capability numbers; the output is the input when nothing is dropped)
import struct, sys

OP_EXTENSION, OP_EXECUTION_MODE, OP_CAPABILITY, OP_TYPE_FLOAT, OP_DECORATE, OP_MEMBER_DECORATE = 10, 16, 17, 22, 71, 72
CAP_FLOAT64 = 10
CAP_GNU = set(range(61, 69))                       # GroupNonUniform, Vote, Arithmetic, Ballot, Shuffle, ShuffleRelative, Clustered, Quad
GNU_OPS = set(range(333, 367)) | {4421, 4428, 4429, 4430, 4431}   # OpGroupNonUniform*, OpSubgroupBallotKHR / FirstInvocation / All / Any / AllEqual
BUILTIN_SUBGROUP = set(range(36, 42)) | set(range(4416, 4421))  # SubgroupSize, SubgroupMaxSize, NumSubgroups, NumEnqueuedSubgroups, SubgroupId, SubgroupLocalInvocationId, Subgroup{Eq,Ge,Gt,Le,Lt}Mask
DEC_BUILTIN = 11
CAP_INTERLOCK = {5363, 5372, 5378}                # FragmentShader{Sample,ShadingRate,Pixel}InterlockEXT
INTERLOCK_OPS = {5364, 5365}                      # OpBegin / OpEndInvocationInterlockEXT
INTERLOCK_MODES = set(range(5366, 5372))          # {Pixel,Sample,ShadingRate}Interlock{Ordered,Unordered}EXT
INTERLOCK_EXT = "SPV_EXT_fragment_shader_interlock"


def literal_string(w):
    b = struct.pack(f"<{len(w)}I", *w)
    return b[:b.index(0)].decode() if 0 in b else b.decode()


def strip(words):
    if len(words) < 5 or words[0] != 0x07230203:
        raise ValueError("not a SPIR-V module")
    insts, i = [], 5
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 0xFFFF
        if n == 0 or i + n > len(words):
            raise ValueError(f"bad instruction at word {i}")
        insts.append((op, words[i:i + n]))
        i += n
    f64 = any(op == OP_TYPE_FLOAT and w[2] == 64 for op, w in insts)
    gnu = any(op in GNU_OPS for op, w in insts) or any(
        (op == OP_DECORATE and w[2] == DEC_BUILTIN and w[3] in BUILTIN_SUBGROUP) or
        (op == OP_MEMBER_DECORATE and w[3] == DEC_BUILTIN and w[4] in BUILTIN_SUBGROUP) for op, w in insts)
    interlock = any(op in INTERLOCK_OPS or (op == OP_EXECUTION_MODE and w[2] in INTERLOCK_MODES) for op, w in insts)
    drop = set() if f64 else {CAP_FLOAT64}
    if not gnu:
        drop |= CAP_GNU
    if not interlock:
        drop |= CAP_INTERLOCK
    out, dropped = list(words[:5]), []
    for op, w in insts:
        if op == OP_CAPABILITY and w[1] in drop:
            dropped.append(w[1])
            continue
        if op == OP_EXTENSION and not interlock and literal_string(w[1:]) == INTERLOCK_EXT:
            continue
        out.extend(w)
    return out, dropped


def main():
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src, "rb").read()
    words = list(struct.unpack(f"<{len(data) // 4}I", data))
    out, dropped = strip(words)
    open(dst, "wb").write(struct.pack(f"<{len(out)}I", *out))
    print(" ".join(map(str, dropped)))


if __name__ == "__main__":
    main()
