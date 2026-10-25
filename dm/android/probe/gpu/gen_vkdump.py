#!/usr/bin/env python3
# Generates vkdump_gen.h from vulkan_core.h: one dump function per VkPhysicalDevice*Features / *Properties struct that extends
# VkPhysicalDeviceFeatures2 / VkPhysicalDeviceProperties2, plus a table saying which core version or extension makes each one valid to query.
# Usage: gen_vkdump.py <vulkan_core.h> <out.h>   (run with python3 -I)
import re, sys

src = open(sys.argv[1]).read().splitlines()
guard = None
structs = {}       # name -> list of (type, member, array_expr or None, is_ptr)
struct_guard = {}  # name -> guard it was defined under
aliases = {}       # alias -> base
alias_guard = {}   # alias -> guard
enums, flags32, flags64 = set(), set(), set()
stypes = {}        # normalized -> enum name

re_guard = re.compile(r'^#define (VK_VERSION_\d_\d|VK_[A-Z]+_[a-z0-9_]+) 1$')
re_sstart = re.compile(r'^typedef struct (Vk\w+) \{$')
re_alias = re.compile(r'^typedef (Vk\w+) (Vk\w+);$')
re_member = re.compile(r'^\s+(const\s+)?([\w ]+?)\s*(\*?)\s*(\w+)(\[(\w+)\])?(\[(\w+)\])?;$')

i = 0
while i < len(src):
    l = src[i]
    m = re_guard.match(l)
    if m: guard = m.group(1)
    m = re.match(r'^typedef VkFlags (\w+);', l)
    if m: flags32.add(m.group(1))
    m = re.match(r'^typedef VkFlags64 (\w+);', l)
    if m: flags64.add(m.group(1))
    m = re.match(r'^typedef enum (\w+) \{', l)
    if m: enums.add(m.group(1))
    m = re.match(r'^\s+VK_STRUCTURE_TYPE_(\w+) = ', l)
    if m: stypes.setdefault(m.group(1).replace('_', ''), 'VK_STRUCTURE_TYPE_' + m.group(1))
    m = re_sstart.match(l)
    if m:
        name = m.group(1); mem = []; i += 1
        while not src[i].startswith('}'):
            mm = re_member.match(src[i])
            if mm:
                ty = mm.group(2).strip(); ptr = bool(mm.group(3)) or ty.endswith('*')
                arr = mm.group(6)
                if mm.group(8): arr = None; ptr = True   # 2-D arrays: skipped
                mem.append((ty, mm.group(4), arr, ptr))
            i += 1
        structs[name] = mem; struct_guard[name] = guard
    m = re_alias.match(l)
    if m and guard:
        aliases[m.group(2)] = m.group(1); alias_guard[m.group(2)] = guard
    i += 1

blacklist = {'VkPhysicalDeviceFeatures2', 'VkPhysicalDeviceProperties2', 'VkPhysicalDeviceGroupProperties', 'VkPhysicalDeviceToolProperties',
             'VkPhysicalDeviceMemoryProperties', 'VkPhysicalDeviceMemoryProperties2', 'VkPhysicalDeviceMemoryBudgetPropertiesEXT',
             'VkPhysicalDeviceLayeredApiPropertiesKHR', 'VkPhysicalDeviceLayeredApiVulkanPropertiesKHR', 'VkPhysicalDeviceLayeredApiPropertiesListKHR',
             'VkPhysicalDeviceFeatures', 'VkPhysicalDeviceProperties', 'VkPhysicalDeviceLimits', 'VkPhysicalDeviceSparseProperties'}
want = re.compile(r'^VkPhysicalDevice\w*(Features|Properties)[A-Z]*$')
sel = [n for n in structs if want.match(n) and n not in blacklist and structs[n] and structs[n][0][1] == 'sType' and not n.startswith('VkPhysicalDeviceVideo')]

INT_U = {'uint32_t', 'uint16_t', 'uint8_t', 'VkBool32', 'VkSampleCountFlags'}
INT_64 = {'uint64_t', 'VkDeviceSize', 'VkDeviceAddress'}
out = []
emitted = set()

def emit_member(lines, ty, nm, arr, ptr, acc):
    if ptr: return
    if ty == 'char' and arr:
        lines.append(f'    fprintf(f, "%s{nm}=%s\\n", pfx, {acc}{nm});'); return
    if ty == 'uint8_t' and arr:
        lines.append(f'    fprintf(f, "%s{nm}=", pfx); for (size_t k = 0; k < sizeof {acc}{nm}; k++) fprintf(f, "%02x", {acc}{nm}[k]); fputc(\'\\n\', f);'); return
    if ty in structs:
        if ty not in emitted: emit_struct(ty)
        if arr: return
        lines.append(f'    {{ char p2[160]; snprintf(p2, sizeof p2, "%s{nm}.", pfx); vkdump_{ty}(f, p2, &{acc}{nm}); }}'); return
    if ty in INT_U or ty in flags32 or ty == 'int32_t' or ty in enums:
        fmt, cast = ('0x%x', '(unsigned)') if (ty in flags32 or ty.endswith('Flags')) else (('%d', '(int)') if (ty == 'int32_t' or ty in enums) else ('%u', '(unsigned)'))
    elif ty in INT_64 or ty in flags64:
        fmt, cast = ('0x%llx', '(unsigned long long)') if ty in flags64 else ('%llu', '(unsigned long long)')
    elif ty == 'size_t': fmt, cast = '%zu', '(size_t)'
    elif ty == 'float': fmt, cast = '%g', '(double)'
    elif ty == 'int64_t': fmt, cast = '%lld', '(long long)'
    else:
        lines.append(f'    /* skipped {ty} {nm} */'); return
    if arr:
        lines.append(f'    fprintf(f, "%s{nm}=", pfx); for (size_t k = 0; k < sizeof {acc}{nm} / sizeof {acc}{nm}[0]; k++) fprintf(f, "%s{fmt}", k ? "," : "", {cast}{acc}{nm}[k]); fputc(\'\\n\', f);')
    else:
        lines.append(f'    fprintf(f, "%s{nm}={fmt}\\n", pfx, {cast}{acc}{nm});')

def emit_struct(name):
    emitted.add(name)
    lines = [f'static void vkdump_{name}(FILE *f, const char *pfx, const void *vp) {{', f'    const {name} *s = (const {name} *)vp; (void)s;']
    for ty, nm, arr, ptr in structs[name]:
        if nm in ('sType', 'pNext'): continue
        emit_member(lines, ty, nm, arr, ptr, 's->')
    lines.append('}')
    out.append('\n'.join(lines))

for n in ['VkPhysicalDeviceFeatures', 'VkPhysicalDeviceLimits', 'VkPhysicalDeviceSparseProperties', 'VkConformanceVersion']:
    emit_struct(n)
table = []
for n in sel:
    norm = n[2:].upper()
    st = stypes.get(norm)
    if not st: continue
    en = [struct_guard[n]] + [alias_guard[a] for a, b in aliases.items() if b == n and alias_guard[a] != struct_guard[n]]
    emit_struct(n)
    kind = 0 if re.search(r'Features[A-Z]*$', n) else 1
    table.append(f'    {{"{n}", {st}, sizeof({n}), "{"|".join(en)}", {kind}, vkdump_{n}}},')

hdr = ['// Generated by gen_vkdump.py from vulkan_core.h; do not edit.', '#pragma once', '#include <stdio.h>', '#include <stddef.h>']
hdr += out
hdr.append('typedef struct { const char *name; VkStructureType stype; size_t size; const char *enablers; int is_props; void (*dump)(FILE *, const char *, const void *); } vkdump_struct_t;')
hdr.append('static const vkdump_struct_t vkdump_structs[] = {')
hdr += table
hdr.append('};')
hdr.append(f'#define VKDUMP_NSTRUCTS {len(table)}')
open(sys.argv[2], 'w').write('\n'.join(hdr) + '\n')
print(f'gen_vkdump: {len(table)} structs', file=sys.stderr)
