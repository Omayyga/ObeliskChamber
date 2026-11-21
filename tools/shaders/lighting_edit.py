#!/usr/bin/env python3
"""Set up's optional "decompiled lighting shader" step (experimental).

The game's deferred lighting pass is one large fragment shader. On PC it is the most expensive single draw of a frame
(about 17% of the GPU frame at 4K in the 0.3.1 traces). This tool takes the player's own translated copy of that shader
from the shader cache, decompiles it to GLSL with spirv-cross, makes two edits to the text, compiles it again with
glslang and writes the result to a folder beside the cache. The runtime uses a module from that folder in place of the
translated one ([nvn] edited_shaders). The translated shader is never changed, and nothing here contains game code:
the edits are rules applied to whatever the player's shader decompiles to.

The edits:
  probes       The loop over reflection probes reads two cube maps for every probe near the pixel, also when the probe's
               weight at the pixel is 0. Such a probe adds exactly nothing (weight * log2(colour)), so its box projection
               and cube-map reads go behind `if (weight > 0.0)`. The result is the same.
  sun_shadows  The sun shadow filter reads 25 samples per pixel. Five spread-out samples are read first; the full 25 are
               read only where those five disagree (the soft edge of a shadow). Not exact: a shadow thinner than the
               filter that all five miss would change.

A shader this was not written for is left alone: the edit is applied only to a module whose SHA-256 is in TESTED (unless
--allow-untested), and only when every anchor it expects is found. Measured on an RTX 3090 Ti at 4K DLAA: the pass went
from 1.90 ms to 1.36 ms and the route from 89.8 to 92.0 fps (docs: claude/Lighting_Shader_Edit_AB_2026-11-21.md in the
project notes; PR #159 comments).

usage: lighting_edit.py --spirv <shader cache>/spirv --out <shader cache>/edited [--spirv-cross exe] [--glslang exe]
       lighting_edit.py --self-test
Exit code 0: done (a shader left alone is not an error; the lines say what happened). 1: bad arguments or missing tools.
"""
import argparse
import hashlib
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

TOOL_VERSION = 2

# hash of the game's shader (the cache's file name) -> the edits to make, in order
TARGETS = {
    "6385c299b541c5be": ("sun_shadows", "probes"),
}

# SHA-256 of the translated modules (shader_cache/spirv/<hash>.spv) the edits were measured on
TESTED = {
    "6385c299b541c5be": {
        "46d146908da5b40a570c9651fee963b532a94733724a86ce572919d5d6257ebd",
    },
}

SUN_PROBES = 5


class Skip(Exception):
    """The shader is not the shape this edit was written for; it is left alone."""


def _need(cond, what):
    if not cond:
        raise Skip(what)


# ---------------------------------------------------------------------------------------------------------------------
# edit: sun shadow filter

_TAP_A = re.compile(r'^(\s*)bool (_\d+) = texture\((fp_t_\w+), (vec2\(.*\))\)\.x >= (_\d+);$')
_TAP_B = re.compile(r'^(\s*)if \(!\(texture\((fp_t_\w+), (vec2\(.*\))\)\.x >= (_\d+)\)\)$')
_TAP_ANY = re.compile(r'texture\(fp_t_\w+, vec2\(.*\)\)\.x >= _\d+')
_DECL = re.compile(r'^(\s*)(precise float|float|int|bool|uint|vec2|vec3|vec4) (_\d+) = (.*);$')
_PRECISE = re.compile(r'^\s*precise float (_\d+) = (.*);$')
_TAP_UV = re.compile(r'vec2\(fma\(fma\((_\d+), (-?[\d.e+-]+), (-?_\d+)\), (_\d+), (_\d+)\), '
                     r'fma\(fma\((_\d+), (-?[\d.e+-]+), (-?_\d+)\), (_\d+), (_\d+)\)\)$')


def _hoist(body, ind):
    """Declarations inside `body` become plain assignments; returns (declarations to put before it, the new body)."""
    decls, real = [], []
    for l in body:
        m = _DECL.match(l)
        if m:
            decls.append(f"{ind}{m[2]} {m[3]};")
            real.append(f"{m[1]}{m[3]} = {m[4]};")
        else:
            real.append(l)
    return decls, real


def edit_sun_shadows(src, probes=SUN_PROBES):
    """Each run of 20 or more shadow-map compares against one reference becomes
    `if (the probe samples disagree) { the run as it was } else { the run with every compare a constant }`."""
    defs = {m[1]: m[2] for m in (_PRECISE.match(l) for l in src) if m}
    taps = []
    for i, l in enumerate(src):
        m = _TAP_A.match(l)
        if m:
            taps.append(dict(i=i, end=i, ind=m[1], tex=m[3], uv=m[4], ref=m[5]))
            continue
        m = _TAP_B.match(l)
        if m:
            _need(i + 3 < len(src) and src[i + 1].strip() == '{' and src[i + 3].strip() == '}', "a shadow compare with an unexpected body")
            taps.append(dict(i=i, end=i + 3, ind=m[1], tex=m[2], uv=m[3], ref=m[4]))
    groups = {}
    for t in taps:
        groups.setdefault((t['tex'], t['ref']), []).append(t)
    blocks = [(k, g) for k, g in groups.items() if len(g) >= 20]
    _need(blocks, "no 20-sample shadow filter found")
    rep, n = {}, 0
    for (tex, ref), g in blocks:
        n += 1
        pts = []
        for t in g:
            m = _TAP_UV.match(t['uv'])
            _need(m, "a shadow sample position of an unexpected form")
            a, b = m[3].lstrip('-'), m[8].lstrip('-')
            _need(a in defs and b in defs, "a shadow sample offset is not a simple product")
            oy = re.search(r'\(?(-?[\d.e+-]+)\)?$', defs[a])
            _need(oy, "a shadow sample offset has no constant")
            inl = t['uv'].replace(a, '(' + defs[a] + ')')
            if b != a:
                inl = inl.replace(b, '(' + defs[b] + ')')
            t['inl'] = inl
            pts.append((float(m[2]), float(oy[1])))
        # the probes: the sample nearest the centre, then each time the sample farthest from those chosen
        chosen = [min(range(len(g)), key=lambda j: math.hypot(*pts[j]))]
        while len(chosen) < probes:
            chosen.append(max((j for j in range(len(g)) if j not in chosen),
                              key=lambda j: min(math.hypot(pts[j][0] - pts[c][0], pts[j][1] - pts[c][1]) for c in chosen)))
        first, last, ind = g[0]['i'], g[-1]['end'], g[0]['ind']
        body = src[first:last + 1]
        _need(sum(l.count('{') - l.count('}') for l in body) == 0, "a shadow filter that is not one block")
        decls, real = _hoist(body, ind)
        const = [_TAP_ANY.sub(f'sh{n}_lit', l) for l in real]
        _need(not any('texture(' in l for l in const), "a texture read in the shadow filter that is not a compare")
        pre = decls + [f"{ind}bool sh{n}_p{k} = texture({tex}, {g[j]['inl']}).x >= {ref};" for k, j in enumerate(chosen)]
        pre.append(f"{ind}bool sh{n}_lit = " + ' && '.join(f'sh{n}_p{k}' for k in range(probes)) + ';')
        pre.append(f"{ind}bool sh{n}_mixed = !sh{n}_lit && (" + ' || '.join(f'sh{n}_p{k}' for k in range(probes)) + ');')
        rep[first] = pre + [f"{ind}if (sh{n}_mixed)", f"{ind}{{"] + real + [f"{ind}}}", f"{ind}else", f"{ind}{{"] + const + [f"{ind}}}"]
        for i in range(first + 1, last + 1):
            rep[i] = []
    out = []
    for i, l in enumerate(src):
        out += rep[i] if i in rep else [l]
    return out, f"{n} shadow filter(s) of {len(blocks[0][1])} samples behind {probes} probes"


# ---------------------------------------------------------------------------------------------------------------------
# edit: reflection probe loop

# Names in the decompiled text are the module's own result ids, so they are the same wherever the translator made the same
# module (TESTED). weight; the accumulators' new values; their old values, in the same order.
_PROBE_WEIGHT = '_2220'
_PROBE_NEW = ('_2302', '_2304', '_2306', '_2308', '_2310', '_2312')
_PROBE_OLD = ('_2140', '_2142', '_2144', '_2146', '_2148', '_2150')


def edit_probes(src):
    """In the probe loop, the lines between the weight and the accumulators run only when the weight is above 0."""
    starts = [i for i, l in enumerate(src) if l.strip() == 'float _2232 = _2224;']
    _need(len(starts) == 1, "the probe loop's start was not found")
    a = starts[0] + 1
    ends = [i for i in range(a, min(a + 140, len(src))) if src[i].strip().startswith(f'float {_PROBE_NEW[-1]} = fma({_PROBE_WEIGHT},')]
    _need(len(ends) == 1, "the probe loop's end was not found")
    b = ends[0]
    body = src[a:b + 1]
    _need(src[a].strip().startswith('if ('), "the probe loop does not start with the box test")
    _need(any(l.strip().startswith(f'float {_PROBE_WEIGHT} = ') for l in src[:a]), "the probe weight is not set before the reads")
    _need(sum(l.count('{') - l.count('}') for l in body) == 0, "the probe reads are not one block")
    _need(sum('textureLod(' in l and 'samplerCube' in l for l in body) == 2, "the probe loop does not read two cube maps")
    _need(not any(re.search(r'\._m\d+\[.*\] = ', l) for l in body), "the probe loop writes to a buffer")
    joined = '\n'.join(body)
    for new, old in zip(_PROBE_NEW, _PROBE_OLD):
        # every accumulator is  new = fma(weight, log2(...), old)
        _need(re.search(rf'{new} = fma\({_PROBE_WEIGHT}, log2\(.*\), {old}\);', joined), f"accumulator {new} is not weight * log2(colour) + {old}")
    ind = src[a][:len(src[a]) - len(src[a].lstrip())]
    decls, real = _hoist(body, ind)
    skip = [f"{ind}    {new} = {old};" for new, old in zip(_PROBE_NEW, _PROBE_OLD)]
    new_body = decls + [f"{ind}if ({_PROBE_WEIGHT} > 0.0)", f"{ind}{{"] + real + [f"{ind}}}", f"{ind}else", f"{ind}{{"] + skip + [f"{ind}}}"]
    return src[:a] + new_body + src[b + 1:], "probes with weight 0 skipped"


EDITS = {"sun_shadows": edit_sun_shadows, "probes": edit_probes}


# ---------------------------------------------------------------------------------------------------------------------
# names

_BLOCK_OPEN = re.compile(r'^(layout\(.*\) (?:uniform|buffer) )(\w+)$')
_BLOCK_CLOSE = re.compile(r'^\} (\w+);$')


def keep_block_names(src):
    """The runtime finds out what a uniform or storage block is from its variable's name (support_buffer, fp_c5, fp_s0, ...). In the
    translated module the block type and its variable share that name, which GLSL cannot say, so spirv-cross calls the variable
    <name>_1. Here the type becomes <name>_t and the variable gets the name back, so the compiled module names it as before."""
    out = list(src)
    renames = {}
    i = 0
    while i < len(out):
        m = _BLOCK_OPEN.match(out[i])
        if m and i + 1 < len(out) and out[i + 1].strip() == '{':
            j = i + 2
            while j < len(out) and not out[j].startswith('}'):
                j += 1
            c = _BLOCK_CLOSE.match(out[j]) if j < len(out) else None
            if c and c[1] == m[2] + '_1':
                out[i] = m[1] + m[2] + '_t'
                out[j] = '} ' + m[2] + ';'
                renames[c[1]] = m[2]
            i = j
        i += 1
    if not renames:
        return out
    pat = re.compile(r'\b(' + '|'.join(re.escape(k) for k in renames) + r')\b')
    return [pat.sub(lambda x: renames[x[1]], l) for l in out]


# ---------------------------------------------------------------------------------------------------------------------
# SPIR-V checks (no tools needed)

def _words(data):
    if len(data) < 20 or len(data) % 4:
        raise Skip("not a SPIR-V module")
    w = struct.unpack(f'<{len(data) // 4}I', data)
    if w[0] != 0x07230203:
        raise Skip("not a SPIR-V module")
    return w


def _string(w, i, n):
    b = b''.join(struct.pack('<I', x) for x in w[i:i + n])
    return b.split(b'\0', 1)[0].decode('utf-8', 'replace')


def interface(data):
    """(version, {(set, binding): variable name}, {(storage class, location)}, execution modes) of a module."""
    w = _words(data)
    sets, bindings, locs, storage, modes, names = {}, {}, {}, {}, set(), {}
    i = 5
    while i < len(w):
        op, n = w[i] & 0xffff, w[i] >> 16
        if n == 0:
            raise Skip("a damaged SPIR-V module")
        if op == 5 and n >= 3:       # OpName
            names[w[i + 1]] = _string(w, i + 2, n - 2)
        elif op == 71 and n >= 4:    # OpDecorate
            if w[i + 2] == 34: sets[w[i + 1]] = w[i + 3]
            if w[i + 2] == 33: bindings[w[i + 1]] = w[i + 3]
            if w[i + 2] == 30: locs[w[i + 1]] = w[i + 3]
        elif op == 59 and n >= 4:    # OpVariable
            storage[w[i + 2]] = w[i + 3]
        elif op == 16 and n >= 3:    # OpExecutionMode
            modes.add(w[i + 2])
        i += n
    return (w[1], {(sets[v], bindings[v]): names.get(v, '') for v in sets if v in bindings},
            {(storage.get(v), l) for v, l in locs.items()}, modes)


def check_same_interface(before, after):
    v0, b0, l0, m0 = interface(before)
    v1, b1, l1, m1 = interface(after)
    _need(v1 >= 0x10300, "the compiled module is older than SPIR-V 1.3")
    _need(set(b1) <= set(b0), f"the compiled module binds something the shader does not: {sorted(set(b1) - set(b0))}")
    # the runtime reads what a binding is from its variable's name
    wrong = sorted(f"{k}: {b0[k]!r} became {b1[k]!r}" for k in b1 if b1[k] != b0[k])
    _need(not wrong, "the compiled module names its bindings differently (" + '; '.join(wrong[:4]) + ")")
    _need(l1 == l0, "the compiled module's inputs and outputs differ")
    _need(m1 == m0, "the compiled module's execution modes differ")


# ---------------------------------------------------------------------------------------------------------------------

def find_tool(given, names):
    if given:
        return given if os.path.isfile(given) else None
    dirs = []
    here = os.path.dirname(os.path.abspath(__file__))
    dirs.append(os.path.join(here, 'bin'))                      # the release package: tools/shaders/bin
    if os.environ.get('VULKAN_SDK'):
        dirs.append(os.path.join(os.environ['VULKAN_SDK'], 'Bin'))
        dirs.append(os.path.join(os.environ['VULKAN_SDK'], 'bin'))
    for d in dirs:
        for n in names:
            for ext in ('.exe', ''):
                p = os.path.join(d, n + ext)
                if os.path.isfile(p):
                    return p
    for n in names:
        p = shutil.which(n)
        if p:
            return p
    return None


def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace',
                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    return r.returncode, r.stdout.strip()


def edit_text(text, edits):
    src = keep_block_names(text.split('\n'))
    notes = []
    for name in edits:
        src, note = EDITS[name](src)
        notes.append(note)
    return '\n'.join(src), notes


def edit_one(h, spirv_dir, out_dir, cross, glslang, allow_untested, keep):
    path = os.path.join(spirv_dir, h + '.spv')
    if not os.path.isfile(path):
        return f"{h}: not in this shader cache (nothing to do)"
    data = open(path, 'rb').read()
    sha = hashlib.sha256(data).hexdigest()
    tested = sha in TESTED.get(h, ())
    if not tested and not allow_untested:
        return (f"{h}: left alone. Your translated shader (sha256 {sha[:16]}) is not one this edit was measured on; "
                f"the game uses the translated shader as before.")
    tmp = tempfile.mkdtemp(prefix='lighting_edit_')
    try:
        frag, edited, spv = (os.path.join(tmp, n) for n in ('in.frag', 'edited.frag', 'out.spv'))
        rc, out = run([cross, path, '--version', '450', '--vulkan-semantics', '--output', frag])
        if rc != 0 or not os.path.isfile(frag):
            return f"{h}: left alone (spirv-cross could not decompile it: {out.splitlines()[-1] if out else rc})"
        try:
            text, notes = edit_text(open(frag, encoding='utf-8').read(), TARGETS[h])
        except Skip as e:
            return f"{h}: left alone ({e}); the game uses the translated shader as before."
        open(edited, 'w', encoding='utf-8', newline='\n').write(text)
        rc, out = run([glslang, '-V', '--target-env', 'vulkan1.1', '-S', 'frag', edited, '-o', spv])
        if rc != 0 or not os.path.isfile(spv):
            return f"{h}: left alone (glslang could not compile the edited shader: {out.splitlines()[-1] if out else rc})"
        new = open(spv, 'rb').read()
        try:
            check_same_interface(data, new)
        except Skip as e:
            return f"{h}: left alone ({e}); the game uses the translated shader as before."
        os.makedirs(out_dir, exist_ok=True)
        dst = os.path.join(out_dir, h + '.spv')
        with open(dst + '.tmp', 'wb') as f:
            f.write(new)
        os.replace(dst + '.tmp', dst)
        if keep:
            shutil.copy(edited, os.path.join(keep, h + '.edited.frag'))
        return (f"{h}: edited ({'; '.join(notes)}). {len(data) // 4} words in, {len(new) // 4} out"
                f"{'' if tested else ' (a shader this edit was not measured on: --allow-untested)'}", sha, hashlib.sha256(new).hexdigest())
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def self_test():
    """The two edits on a small made-up shader body (no game code): checks the shape of what they write."""
    def tap(i, form):
        ox, oy = math.cos(i * 2.4) * (0.2 + 0.03 * i), math.sin(i * 2.4) * (0.2 + 0.03 * i)
        a = [f"    precise float _9{i:02d}0 = _20 * {oy!r};", f"    precise float _9{i:02d}1 = _21 * {oy!r};"]
        uv = f"vec2(fma(fma(_21, {ox!r}, -_9{i:02d}0), _30, _31), fma(fma(_20, {ox!r}, _9{i:02d}1), _30, _32))"
        if form == 'a':
            return a + [f"    bool _5{i:02d} = texture(fp_t_cb7_E, {uv}).x >= _33;", f"    int _6{i:02d} = _5{i:02d} ? 1 : 0;"]
        return a + [f"    int _6{i:02d} = 1;", f"    if (!(texture(fp_t_cb7_E, {uv}).x >= _33))", "    {", f"        _6{i:02d} = 0;", "    }"]
    src = ["void main()", "{", "    float _20 = 0.5;", "    float _21 = 0.25;", "    float _30 = 0.01;", "    float _31 = 0.5;", "    float _32 = 0.5;", "    float _33 = 0.5;"]
    for i in range(25):
        src += tap(i, 'a' if i in (0, 20) else 'b')
    src += ["    float _700 = float(" + ' + '.join(f"_6{i:02d}" for i in range(25)) + ") * 0.04;", "}"]
    out, note = edit_sun_shadows(src)
    text = '\n'.join(out)
    assert note.startswith("1 shadow filter(s) of 25 samples"), note
    assert text.count('texture(fp_t_cb7_E') == 25 + SUN_PROBES, text.count('texture(fp_t_cb7_E')
    assert 'if (sh1_mixed)' in text and text.count('sh1_lit') >= 25
    assert text.index('int _600;') < text.index('if (sh1_mixed)')          # declarations moved out of the branch
    assert sum(l.count('{') - l.count('}') for l in out) == 0

    w, new, old = _PROBE_WEIGHT, _PROBE_NEW, _PROBE_OLD
    body = ["    float _2220 = _1;", "    float _2224 = _2;", "    float _2232 = _2224;", "    if (!(!_2198))", "    {", "        _2232 = _3;", "    }",
            "    vec3 _2286 = textureLod(samplerCube(a, s), vec3(_2232), 0.0).xyz;", "    vec3 _2294 = textureLod(samplerCube(b, s), vec3(_2232), 1.0).xyz;"]
    for k, (n_, o_) in enumerate(zip(new, old)):
        body.append(f"    {'' if n_ in ('_2302', '_2308') else 'float '}{n_} = fma({w}, log2(max(_2286.x, 0.001)), {o_});")
    out, note = edit_probes(["void main()", "{"] + body + ["}"])
    text = '\n'.join(out)
    assert 'if (_2220 > 0.0)' in text and '_2312 = _2150;' in text and text.count('textureLod(') == 2
    assert text.index('vec3 _2286;') < text.index('if (_2220 > 0.0)')
    try:
        edit_probes(["void main()", "{", "}"])
        raise AssertionError("an unknown shader was edited")
    except Skip:
        pass
    blk = ["layout(set = 0, binding = 0, std140) uniform support_buffer", "{", "    float _m5[73];", "} support_buffer_1;", "",
           "layout(set = 1, binding = 16, std430) buffer fp_s0", "{", "    uint _m0[];", "} fp_s0_1;", "",
           "void main()", "{", "    float a = support_buffer_1._m5[0] + float(fp_s0_1._m0[1]);", "}"]
    text = '\n'.join(keep_block_names(blk))
    assert "uniform support_buffer_t" in text and "} support_buffer;" in text and "support_buffer._m5[0]" in text and "fp_s0._m0[1]" in text and "_1." not in text, text
    print("lighting_edit self-test: ok")


def main():
    ap = argparse.ArgumentParser(description="Set up's optional decompiled lighting shader step (experimental)")
    ap.add_argument('--spirv', help="the shader cache's spirv folder (holds <hash>.spv)")
    ap.add_argument('--out', help="where the edited modules go (the runtime reads <shader cache>/edited)")
    ap.add_argument('--spirv-cross', dest='cross')
    ap.add_argument('--glslang')
    ap.add_argument('--allow-untested', action='store_true', help="also edit a module whose SHA-256 is not in TESTED (for developers)")
    ap.add_argument('--keep', help="folder to keep the edited GLSL in (for developers; it is derived from the game's shader, do not share it)")
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    if a.self_test:
        self_test()
        return 0
    if not a.spirv or not a.out:
        ap.error("--spirv and --out are needed")
    cross = find_tool(a.cross, ('spirv-cross',))
    glslang = find_tool(a.glslang, ('glslang', 'glslangValidator'))
    if not cross or not glslang:
        print("lighting shader edit: " + ' and '.join(n for n, t in (('spirv-cross', cross), ('glslang', glslang)) if not t)
              + " not found (tools/shaders/bin, the Vulkan SDK's Bin folder, or PATH). Nothing was changed.")
        return 1
    if a.keep:
        os.makedirs(a.keep, exist_ok=True)
    done = {}
    for h in TARGETS:
        r = edit_one(h, a.spirv, a.out, cross, glslang, a.allow_untested, a.keep)
        if isinstance(r, tuple):
            print("lighting shader edit: " + r[0])
            done[h] = {"input_sha256": r[1], "output_sha256": r[2], "edits": list(TARGETS[h])}
        else:
            print("lighting shader edit: " + r)
            stale = os.path.join(a.out, h + '.spv')   # an edit from an earlier run must not outlive a shader it no longer fits
            if os.path.isfile(stale):
                os.remove(stale)
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, 'edit.json'), 'w', encoding='utf-8') as f:
        json.dump({"tool_version": TOOL_VERSION, "shaders": done}, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
