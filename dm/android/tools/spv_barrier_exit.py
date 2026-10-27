#!/usr/bin/env python3
# Keeps every invocation of a compute shader at its workgroup barriers. Maxwell leaves invocations that returned out of a barrier; Vulkan does not, and Mali GPUs wait at a barrier
# for invocations that returned until the driver's watchdog loses the device. Paper Mario: The Thousand-Year Door's mip chain pass (b07b6080aab0ae75, 8x8 invocations, five
# barriers) lets the invocations a level no longer needs return between them: VK_ERROR_DEVICE_LOST on the RG 476H shortly after the title's first frames.
# In the entry point of a GLCompute module with OpControlBarrier, each OpReturn before the function's last block, in a block a selection branches to, becomes: store true in a
# function-local flag, branch to that selection's merge block. Every later side effect (OpImageWrite, OpStore through a Workgroup or StorageBuffer pointer, an atomic) is then
# wrapped in a selection on "not flagged", so a finished invocation reaches the barriers but writes nothing. Blocks with OpLoopMerge are not split (their stores stay as they are).
# DMSH_VK_BARRIER_EXIT=0 changes nothing.
# Usage: spv_barrier_exit.py <in.spv> <out.spv>   (prints the number of returns rewritten and stores guarded)
import os, struct, sys

OP_ENTRY_POINT, OP_EXECUTION_MODEL_GLCOMPUTE = 15, 5
OP_TYPE_BOOL, OP_TYPE_POINTER, OP_CONSTANT_TRUE, OP_CONSTANT_FALSE = 20, 32, 41, 42
OP_FUNCTION, OP_FUNCTION_END, OP_VARIABLE, OP_LOAD, OP_STORE, OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN = 54, 56, 59, 61, 62, 65, 66
OP_IMAGE_WRITE, OP_LOGICAL_NOT, OP_PHI, OP_LOOP_MERGE, OP_SELECTION_MERGE, OP_LABEL = 99, 168, 245, 246, 247, 248
OP_BRANCH, OP_BRANCH_CONDITIONAL, OP_SWITCH, OP_RETURN, OP_RETURN_VALUE, OP_KILL, OP_UNREACHABLE = 249, 250, 251, 253, 254, 252, 255
OP_CONTROL_BARRIER = 224
ATOMICS = set(range(227, 243))  # OpAtomicLoad .. OpAtomicXor (OpAtomicLoad included: harmless to guard)
SC_FUNCTION, SC_WORKGROUP, SC_STORAGE_BUFFER = 7, 4, 12
TERMINATORS = {OP_BRANCH, OP_BRANCH_CONDITIONAL, OP_SWITCH, OP_RETURN, OP_RETURN_VALUE, OP_KILL, OP_UNREACHABLE}


def ins(op, *ops):
    return [((len(ops) + 1) << 16) | op, *ops]


def fix(words):
    """→ (words, returns rewritten, stores guarded)"""
    if len(words) < 5 or words[0] != 0x07230203:
        raise ValueError("not a SPIR-V module")
    if os.environ.get("DMSH_VK_BARRIER_EXIT") == "0":
        return words, 0, 0
    I, i = [], 5
    while i < len(words):
        wc = words[i] >> 16
        if wc == 0:
            raise ValueError(f"word count 0 at word {i}")
        I.append(list(words[i:i + wc])); i += wc
    op = lambda x: x[0] & 0xffff
    ep = [x for x in I if op(x) == OP_ENTRY_POINT and x[1] == OP_EXECUTION_MODEL_GLCOMPUTE]
    if len(ep) != 1:
        return words, 0, 0
    main = ep[0][2]
    f0 = next((k for k, x in enumerate(I) if op(x) == OP_FUNCTION and x[2] == main), None)
    if f0 is None:
        return words, 0, 0
    f1 = next(k for k in range(f0, len(I)) if op(I[k]) == OP_FUNCTION_END)
    body = I[f0 + 1:f1]
    bars = [k for k, x in enumerate(body) if op(x) == OP_CONTROL_BARRIER]
    if not bars:
        return words, 0, 0
    labels = [k for k, x in enumerate(body) if op(x) == OP_LABEL]
    last_label = labels[-1]
    # the early returns: an OpReturn before the function's last block and before its last barrier, whose block a selection header branches to
    blk_of = {}
    cur = None
    for k, x in enumerate(body):
        if op(x) == OP_LABEL: cur = x[1]
        blk_of[k] = cur
    merge_for = {}   # block label → the merge block of the selection that branches to it
    for k, x in enumerate(body):
        if op(x) == OP_SELECTION_MERGE and k + 1 < len(body) and op(body[k + 1]) == OP_BRANCH_CONDITIONAL:
            for t in body[k + 1][2:4]: merge_for.setdefault(t, x[1])
    rets = [k for k, x in enumerate(body) if op(x) == OP_RETURN and k < last_label and k < bars[-1] and blk_of[k] in merge_for]
    if not rets:
        return words, 0, 0
    bound = words[3]
    def new():
        nonlocal bound
        bound += 1; return bound - 1
    # types and constants (added before the first function when missing)
    glob_end = next(k for k, x in enumerate(I) if op(x) == OP_FUNCTION)
    add = []
    t_bool = next((x[1] for x in I[:glob_end] if op(x) == OP_TYPE_BOOL), None)
    if t_bool is None: t_bool = new(); add.append(ins(OP_TYPE_BOOL, t_bool))
    t_ptr = next((x[1] for x in I[:glob_end] if op(x) == OP_TYPE_POINTER and x[2] == SC_FUNCTION and x[3] == t_bool), None)
    if t_ptr is None: t_ptr = new(); add.append(ins(OP_TYPE_POINTER, t_ptr, SC_FUNCTION, t_bool))
    c_true = next((x[2] for x in I[:glob_end] if op(x) == OP_CONSTANT_TRUE and x[1] == t_bool), None)
    if c_true is None: c_true = new(); add.append(ins(OP_CONSTANT_TRUE, t_bool, c_true))
    c_false = next((x[2] for x in I[:glob_end] if op(x) == OP_CONSTANT_FALSE and x[1] == t_bool), None)
    if c_false is None: c_false = new(); add.append(ins(OP_CONSTANT_FALSE, t_bool, c_false))
    # pointers into Workgroup / StorageBuffer memory: their types, then the variables and access chains of those types
    ptr_t = {x[1] for x in I if op(x) == OP_TYPE_POINTER and x[2] in (SC_WORKGROUP, SC_STORAGE_BUFFER)}
    shared = {x[2] for x in I if op(x) in (OP_VARIABLE, OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN) and x[1] in ptr_t}
    flag = new()
    out, moved, nret, nguard = [], {}, 0, 0
    first_ret = rets[0]
    loop_hdr = {blk_of[k] for k, x in enumerate(body) if op(x) == OP_LOOP_MERGE}
    cur, in_loop_header = None, False
    for k, x in enumerate(body):
        o = op(x)
        if o == OP_LABEL:
            cur = x[1]; in_loop_header = cur in loop_hdr
        if k in rets:
            out.append(ins(OP_STORE, flag, c_true)); out.append(ins(OP_BRANCH, merge_for[blk_of[k]])); nret += 1
            continue
        side = o == OP_IMAGE_WRITE or (o == OP_STORE and x[1] in shared) or o in ATOMICS
        if k > first_ret and side and not in_loop_header:
            a, n, w, m = new(), new(), new(), new()
            out += [ins(OP_LOAD, t_bool, a, flag), ins(OP_LOGICAL_NOT, t_bool, n, a), ins(OP_SELECTION_MERGE, m, 0), ins(OP_BRANCH_CONDITIONAL, n, w, m),
                    ins(OP_LABEL, w), x, ins(OP_BRANCH, m), ins(OP_LABEL, m)]
            moved[cur] = m; cur = m; nguard += 1
            continue
        out.append(x)
    # the flag: a variable of the first block, cleared at the start
    first_blk = next(k for k, x in enumerate(out) if op(x) == OP_LABEL)
    vend = first_blk + 1
    while vend < len(out) and op(out[vend]) == OP_VARIABLE: vend += 1
    out[vend:vend] = [ins(OP_VARIABLE, t_ptr, flag, SC_FUNCTION, c_false)]
    # an OpPhi naming a split block as the predecessor names the block its terminator moved to
    for x in out:
        if op(x) == OP_PHI:
            for p in range(4, len(x), 2): x[p] = moved.get(x[p], x[p])
    I = I[:glob_end] + add + I[glob_end:f0 + 1] + out + I[f1:]
    hdr = list(words[:5]); hdr[3] = bound
    return hdr + [w for x in I for w in x], nret, nguard


if __name__ == "__main__":
    data = open(sys.argv[1], "rb").read()
    words, nr, ng = fix(list(struct.unpack(f"<{len(data) // 4}I", data)))
    open(sys.argv[2], "wb").write(struct.pack(f"<{len(words)}I", *words))
    print(nr, ng)
