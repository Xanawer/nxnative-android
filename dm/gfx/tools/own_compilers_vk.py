#!/usr/bin/env python3
# dmsh_build_vk.py with this Mac's own spirv-cross, as own_compilers.py runs dmsh_build.py for the iOS libraries: the Android NSP script (scripts/build_apk_from_nsp.py) builds the Vulkan
# shader modules through this file. dmsh_build_vk.py stops when a chain tool differs from dmsh_chain.lock; here spirv-cross may differ (a Homebrew upgrade), with one note line naming it,
# while the translator (dmsh_xlate_bin) must still be the locked build and a missing spirv-cross still stops the build. The Metal compiler is not used by the Vulkan chain (dmsh_build_vk.py
# ignores it already). Neither the chain version (dmsh_build_vk.py --version, which dm/gfx/vkn/vkn.h VKN_CHAIN must equal) nor any repository file changes: the lock stays the record of the
# spirv-cross the modules were tested with, and on the maintainer's Mac dmsh_build_vk.py keeps the strict check. The NSP script's stage key takes spirv-cross --revision, so the modules are
# built again when spirv-cross changes.
# Usage: own_compilers_vk.py <dmsh_build_vk.py arguments>   (the environment as for dmsh_build_vk.py)
import os, re, sys, runpy
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE); import dmsh_chain
OWN = re.compile(r'(spirv-cross): lock (.*) / actual (?!missing$)(.*)')   # a missing spirv-cross still stops the build
strict = dmsh_chain.check_tools


def check_tools():
    bad, own = [], []
    for b in strict(): (own if OWN.fullmatch(b) else bad).append(b)
    if own and not [b for b in bad if not b.startswith('metal_')]:   # dmsh_build_vk.py drops the Metal compiler's lines itself
        print('note: built with this Mac\'s spirv-cross, not the one the shaders were tested with (dmsh_chain.lock): ' +
              '; '.join(f'{m[1]} {m[3]} (tested: {m[2]})' for m in map(OWN.fullmatch, own)), flush=True)
    return bad


if __name__ == '__main__':
    dmsh_chain.check_tools = check_tools   # dmsh_build_vk.py imports the same module object and calls it through the module
    sys.argv = [os.path.join(HERE, 'dmsh_build_vk.py'), *sys.argv[1:]]
    runpy.run_path(sys.argv[0], run_name='__main__')
