#!/usr/bin/env python3
# Declares the storage image capabilities a module needs but the shader translator leaves out: StorageImageWriteWithoutFormat (56) when it writes a storage image of
# Unknown format (OpImageWrite), StorageImageReadWithoutFormat (55) when it reads one (OpImageRead). Paper Mario: The Thousand-Year Door's compute shaders write such images
# (833 of its modules failed spirv-val: "Capability StorageImageWriteWithoutFormat is required to write to storage image"). A backend enables the matching device features
# (shaderStorageImageWriteWithoutFormat / ReadWithoutFormat) and refuses a module whose feature is off (vkn: vk_dev.c, vk_shader.c cap_ok).
# DMSH_VK_NOFMT=0 adds nothing (the modules fail spirv-val as before).
# Usage: spv_add_caps.py <in.spv> <out.spv>   (prints the added capability numbers; the output is the input when nothing is added)
import os, struct, sys

OP_CAPABILITY, OP_TYPE_IMAGE, OP_LOAD, OP_IMAGE_READ, OP_IMAGE_WRITE = 17, 25, 61, 98, 99
CAP_READ_NOFMT, CAP_WRITE_NOFMT = 55, 56
DIM_SUBPASS = 6


def add(words):
    """→ (words, [added capability numbers])"""
    if len(words) < 5 or words[0] != 0x07230203:
        raise ValueError("not a SPIR-V module")
    if os.environ.get("DMSH_VK_NOFMT") == "0":
        return words, []
    caps, nofmt, load_type, need, i, last_cap = set(), set(), {}, set(), 5, 5
    while i < len(words):
        op, wc = words[i] & 0xffff, words[i] >> 16
        if wc == 0:
            raise ValueError(f"word count 0 at word {i}")
        if op == OP_CAPABILITY:
            caps.add(words[i + 1]); last_cap = i + wc
        elif op == OP_TYPE_IMAGE and wc >= 9 and words[i + 7] == 2 and words[i + 8] == 0 and words[i + 3] != DIM_SUBPASS:
            nofmt.add(words[i + 1])   # a storage image (Sampled 2) of Unknown format
        elif op == OP_LOAD and wc >= 4:
            load_type[words[i + 2]] = words[i + 1]
        elif op in (OP_IMAGE_READ, OP_IMAGE_WRITE) and nofmt:
            img = words[i + (3 if op == OP_IMAGE_READ else 1)]
            # the image operand is a loaded variable; one that is not (no OpLoad seen) is taken as format-less, which only adds a capability spirv-val would also ask for
            if img not in load_type or load_type[img] in nofmt:
                need.add(CAP_READ_NOFMT if op == OP_IMAGE_READ else CAP_WRITE_NOFMT)
        i += wc
    added = sorted(need - caps)
    if not added:
        return words, []
    return words[:last_cap] + [w for c in added for w in ((2 << 16) | OP_CAPABILITY, c)] + words[last_cap:], added


if __name__ == "__main__":
    data = open(sys.argv[1], "rb").read()
    words, added = add(list(struct.unpack(f"<{len(data) // 4}I", data)))
    open(sys.argv[2], "wb").write(struct.pack(f"<{len(words)}I", *words))
    print(" ".join(map(str, added)))
