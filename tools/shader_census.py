"""Census of every Maxwell shader in the extracted XCX RomFS and ExeFS, and an offline translation pass.

Port of MacNativeGames games/totk/scripts/prepare_shader_seed.py (inventory, bnsh_shaders, sharcb_shaders, sarc) and the
package walk of locate_shaders.py @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames contributors). Differences: no
shared shader list, no spirv-cross, no bind-state variants; the module scan reads the NSO files under exefs/ directly;
a control section must start with CONTROL_MAGIC (sharcb_shaders says why).

Reads build/extracted/<version>/ (RomFS image plus index.json as tools/romfs_index.py reads them, and exefs/). XCX keeps
its shaders inside its data archive (sts.arh index, sts.ard data): ard_shaders opens it and reads the game's shader
containers (spch_shaders). --played takes the shaders from build/<version>/shader-programs.bin instead, the shaders a
build met in play (runtime/gfx_shader.c), which is what the runtime probes need. Writes under build/<version>/shaders/
only:
    code/<key>.code     translator input (graphics: SPH + instructions, compute: instructions)
    jobs.json           key -> job, source path, origin (bnsh|sharcb|main|ard)
    spv/<key>.spv       with --translate
    failures.txt        with --translate, when something failed: groups by message

The key is sha256(control[:0x880] + code), as in MNG. Needs Python 3.14 (compression.zstd), so run in .venv-py314:

    .venv-py314\\Scripts\\python tools\\shader_census.py --version upd --played [--translate]
"""
import argparse
import compression.zstd as zstd
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib
from collections import defaultdict
from pathlib import Path

import lz4.block

ROOT = Path(__file__).resolve().parent.parent
XLATE = ROOT / 'build' / 'xlate' / ('xcx_xlate.exe' if os.name == 'nt' else 'xcx_xlate')
SPIRV_VAL = Path(r'C:\msys64\ucrt64\bin\spirv-val.exe') if os.name == 'nt' else Path(shutil.which('spirv-val') or '/usr/bin/spirv-val')
ZSTD = b'\x28\xb5\x2f\xfd'
# NVN program control section magic, u32 0x98761234 (d2r-mobile dm/gfx/tools/shader_map.py)
CONTROL_MAGIC = b'\x34\x12\x76\x98'
STAGE_IDS = {0: 'vs', 1: 'ps', 5: 'cs'}
MAX_WORKERS = 20


def digest(data):
    return hashlib.sha256(data).hexdigest()


def checked(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError('shader container range outside file')
    return data[offset:offset + size]


def sarc(data):
    if data[:4] != b'SARC':
        raise ValueError('expected SARC')
    endian = '<' if data[6:8] == b'\xff\xfe' else '>'
    header, size, start = struct.unpack_from(endian + 'HxxII', data, 4)
    if size != len(data) or header < 20 or checked(data, header, 4) != b'SFAT':
        raise ValueError('invalid SARC header')
    count = struct.unpack_from(endian + 'H', data, header + 6)[0]
    names = header + 12 + count * 16
    if checked(data, names, 4) != b'SFNT':
        raise ValueError('invalid SARC string table')
    names += 8
    result = {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from(endian + '4I', checked(data, header + 12 + i * 16, 16))
        off = names + (attribute & 0xffffff) * 4
        if attribute >> 24 != 1 or not names <= off < start or hi < lo or start + lo < start:
            raise ValueError('invalid SARC member')
        name = data[off:data.index(0, off, start)].decode('utf-8')
        if name.startswith('/') or '\\' in name or any(p in ('', '.', '..') for p in name.split('/')) or name in result:
            raise ValueError('unsafe or duplicate SARC name')
        result[name] = checked(data, start + lo, hi - lo)
    return result


PLAYED_STAGES = {0: 'vs', 4: 'ps', 5: 'cs'}   # GFX_STAGE_ of runtime/gfx.h; the stages the translator takes


def bnsh_shaders(data):
    """BNSH absolute offsets are relative to each embedded BNSH, not BFSHA."""
    if data[:4] != b'BNSH' or checked(data, 12, 2) != b'\xff\xfe' or checked(data, 96, 4) != b'grsc':
        raise ValueError('unsupported BNSH header')
    size = struct.unpack_from('<I', data, 28)[0]
    data = checked(data, 0, size)
    count, variations = struct.unpack_from('<IQ', data, 124)
    checked(data, variations, count * 64)
    for i in range(count):
        program = struct.unpack_from('<Q', data, variations + i * 64 + 16)[0]
        checked(data, program, 56)
        for stage, relative in [('vs', 8), ('ps', 40), ('cs', 48)]:
            offset = struct.unpack_from('<Q', data, program + relative)[0]
            if not offset:
                continue
            control_off, code_off, code_size, control_size = struct.unpack_from('<QQII', checked(data, offset + 8, 24))
            control = checked(data, control_off, control_size)
            code = checked(data, code_off, code_size)
            if len(control) < 0x880:
                continue   # the SDK utility BNSH uses an older, shorter control layout
            control = control[:0x880]   # some are 0x88c; NVN and the runtime's key read 0x880
            if control[:4] != CONTROL_MAGIC:
                raise ValueError('NVN control section without its magic')
            c1, length = struct.unpack_from('<II', control, 0x700)
            actual = struct.unpack_from('<I', control, 0x714)[0]
            if actual != {'vs': 0, 'ps': 1, 'cs': 5}[stage] or not 0 <= c1 <= length <= len(code) or not length:
                raise ValueError('invalid NVN shader bounds or stage')
            yield digest(control + code[:length]), stage, control, code[:length]


def sharcb_shaders(data, rejected):
    """agl shader packages (.sharcb, magic BAHS): records <u64 code offset in the pool, u32 control size (0x880 or
    0x88c), i32 offset of the control from the record>; a record may reuse an earlier record's control.

    The scan finds records by their size word, so it also meets 16-byte groups that only look like one. Without the
    magic check, 1.2.1 yields 87 such keys: each has control offset 0 (the "control" starts at the record itself, so its
    first 8 bytes are the code offset) and no CONTROL_MAGIC. The game hands NVN real control sections and the runtime
    key hashes those, so no draw can ever produce these keys. Their code is cut from the wrong place: they were all 67
    translation failures of 1.2.1 (IndexOutOfRange in ShaderHeader.ctor and ArgumentOutOfRange in Decoder.FillBlock,
    on 0 to 4,718 bytes of code). MNG's scan (prepare_shader_seed.py @ b75be8cf) keeps them; the 7 that translate are
    V rows in MNG's list only because shader_list.py add_cache() folds the seed's own .variants files back in. A
    candidate without CONTROL_MAGIC is skipped and counted in rejected[0]."""
    if len(data) < 32 or data[:4] != b'BAHS':
        raise ValueError('expected an agl shader package')
    start, pool_size, pool = struct.unpack_from('<3I', data, 20)
    if not 32 <= start <= pool or pool + pool_size > len(data):
        raise ValueError('agl shader package layout outside the file')
    seen = set()
    for size in (b'\x80\x08\x00\x00', b'\x8c\x08\x00\x00'):
        at = start
        while (at := data.find(size, at, pool)) >= 0:
            record, at = at - 8, at + 4
            if record < start or record % 4:
                continue
            code_off = struct.unpack_from('<Q', data, record)[0]
            control_at = record + struct.unpack_from('<i', data, record + 12)[0]
            if not start <= control_at <= pool - 0x880 or code_off >= pool_size:
                continue
            control = data[control_at:control_at + 0x880]
            if control[:4] != CONTROL_MAGIC:
                rejected[0] += 1
                continue
            c1, length = struct.unpack_from('<II', control, 0x700)
            stage = STAGE_IDS.get(struct.unpack_from('<I', control, 0x714)[0])
            if not stage or not 0 < length <= pool_size - code_off or c1 > length:
                continue
            code = data[pool + code_off:pool + code_off + length]
            key = digest(control + code)
            if key not in seen:
                seen.add(key)
                yield key, stage, control, code


def nso_image(path):
    """The decompressed text, rodata and data segments of an NSO, concatenated (enough to find embedded BNSH files)."""
    raw = path.read_bytes()
    if raw[:4] != b'NSO0':
        raise ValueError(f'{path} is not an NSO')
    flags = struct.unpack_from('<I', raw, 0xC)[0]
    parts = []
    for i in range(3):
        file_off, _, dec_size = struct.unpack_from('<III', raw, 0x10 + i * 0x10)
        comp_size = struct.unpack_from('<I', raw, 0x60 + i * 4)[0]
        data = raw[file_off:file_off + comp_size]
        parts.append(lz4.block.decompress(data, uncompressed_size=dec_size) if flags & (1 << i) else data)
    return b''.join(parts)


# ---- XCX's data archive
# sts.arh ("arh2"): entry count and alignment after the magic, then 16 bytes per entry: the xxh64 of its path, its size
# in sts.ard and its expanded size. The entries lie in sts.ard in table order, each one starting aligned. An entry is
# stored as it is (model streams, magic DRSM) or as one or more xbc1 blocks back to back (map files hold many); a block
# is a 48-byte header (magic, method 1 zlib or 3 zstd, expanded size, packed size, a hash and a name) and the packed
# bytes. Model streams hold further xbc1 blocks, and the shader containers sit inside the expanded data.
ARCHIVE = ('sts.arh', 'sts.ard')                           # RomFS paths of the index and the data
ARH_MAGIC = b'arh2'
XBC1 = b'xbc1'
XBC1_MAX = 512 << 20                                       # bigger than any block of the game; a sanity bound
XBC1_DEPTH = 3                                             # nesting seen: entry, model stream, one more
XBC1_ERRORS = (ValueError, struct.error, zlib.error, zstd.ZstdError)


def arh_entries(arh):
    """(offset in sts.ard, size) of every entry of an arh2 index, in table order."""
    if arh[:4] != ARH_MAGIC:
        raise ValueError('expected an arh2 archive index')
    count, align = struct.unpack_from('<II', arh, 4)
    if not align or align & (align - 1):
        raise ValueError('arh2 alignment is not a power of two')
    checked(arh, 16, count * 16)
    entries, offset = [], 0
    for i in range(count):
        size = struct.unpack_from('<I', arh, 16 + 16 * i + 8)[0]
        entries.append((offset, size))
        offset += (size + align - 1) & -align
    return entries


def xbc1_block(data, pos):
    """The expanded bytes of the xbc1 block at pos."""
    method, size, packed = struct.unpack_from('<III', checked(data, pos + 4, 12))
    if size > XBC1_MAX:
        raise ValueError('xbc1 block too big')
    body = checked(data, pos + 48, packed)
    # at most one byte more than the header says, so a damaged block (or a match of the magic inside other bytes) can
    # never expand past XBC1_MAX before the size check rejects it
    if method == 3:
        out = zstd.ZstdDecompressor().decompress(body, max_length=size + 1)
    elif method == 1:
        out = zlib.decompressobj().decompress(body, size + 1)
    else:
        raise ValueError(f'xbc1 method {method}')
    if len(out) != size:
        raise ValueError('xbc1 block expands to another size')
    return out


def xbc1_header_fits(data, pos):
    """The xbc1 header at pos names a known method and packed bytes that fit: a block, not the magic inside other bytes."""
    if pos + 48 > len(data):
        return False
    method, _, packed = struct.unpack_from('<III', data, pos + 4)
    return method in (1, 3) and pos + 48 + packed <= len(data)


def xbc1_expand(data, depth=0, counts=None):
    """data and the expanded bytes of every xbc1 block inside it, recursively. The magic is searched for, so a match
    inside other bytes is tried and dropped when it does not expand. counts (optional) gets the blocks that look real
    but could not be expanded, and those left closed at XBC1_DEPTH, so a change in the archive that hides shaders
    shows in the census line instead of passing silently."""
    yield data
    pos = 0
    while (pos := data.find(XBC1, pos)) >= 0:
        if depth >= XBC1_DEPTH:
            if counts is not None and xbc1_header_fits(data, pos):
                counts['xbc1 past the depth limit'] += 1
            pos += 4
            continue
        try:
            inner = xbc1_block(data, pos)
        except XBC1_ERRORS:
            if counts is not None and xbc1_header_fits(data, pos):
                counts['xbc1 unreadable'] += 1
            pos += 4
            continue
        yield from xbc1_expand(inner, depth + 1, counts)
        pos += 48 + struct.unpack_from('<I', data, pos + 12)[0]


def ard_entry(raw, counts=None):
    """The expanded pieces of one archive entry: each top-level xbc1 block (or the entry itself) and what nests in it.
    An entry made of xbc1 blocks is packed data itself, so it is left out."""
    pieces = xbc1_expand(raw, 0, counts)
    if raw[:4] == XBC1:
        next(pieces)
    yield from pieces


# A shader container (magic HCPS, version 0x3f2), the layout of the game's model and effect shaders. Header words after
# magic and version: the program table's offset and count (8 bytes a program: the offset of its SLCT record in the
# record section, then zero), a second table (offset, count; not needed here), then offset and size of the record
# section, the code section and the control section, all from the container start. A SLCT record holds at +0x28 the
# offset and size of its control blocks in the control section and of its code in the code section; from NVSD +0x1c,
# its NVSD block holds the variant count, the vertex and fragment code sizes and their control block sizes (u32 each).
# The vertex block comes first, then the fragment block, and their code follows the same order. Full blocks are 0x850
# bytes. The game hands NVN pointers into the container as loaded, so a program's control section is the 0x880 bytes
# from its block on, into the next block: that is what the runtime reads and hashes (gfx_shader.c build_code_hash).
# Checked against the 4,663 programs a long run of the game recorded (shader-programs.bin): all but 2 are found with
# the same code hash, and those 2 are nowhere in the archive.
SPCH_MAGIC, SPCH_VERSION = b'HCPS', 0x3f2
SLCT_MAGIC, NVSD_MAGIC = b'SLCT', b'NVSD'
NVN_STAGES = {0: 'vs', 1: 'ps', 5: 'cs'}                   # control +0x714
FULL_BLOCK = 0x850


def spch_shaders(data, pos, counts):
    """(key, stage, control, code) of every program of the container at data[pos:]. counts gathers what was skipped."""
    head = struct.unpack_from('<12I', checked(data, pos, 48))
    if head[1] != SPCH_VERSION:
        counts['container version'] += 1
        return
    so_off, so_n, _, _, rec_off, rec_len, code_off, code_len, ctrl_off, ctrl_len = head[2:12]
    # offsets from the container start, read in place: a map piece holds many containers, and a copy of the rest of the
    # piece for each would cost more than the census itself
    so_off, rec_off, code_off, ctrl_off = pos + so_off, pos + rec_off, pos + code_off, pos + ctrl_off
    s = data
    if so_n * 0x30 > rec_len:   # more programs than records fit: a damaged table that would hash the same ones over
        raise ValueError('program table longer than its records')
    checked(s, so_off, so_n * 8), checked(s, rec_off, rec_len), checked(s, code_off, code_len)
    checked(s, ctrl_off, ctrl_len)
    for i in range(so_n):
        rec = rec_off + struct.unpack_from('<I', s, so_off + 8 * i)[0]
        if checked(s, rec, 4) != SLCT_MAGIC:
            counts['not a SLCT record'] += 1
            continue
        c_off, c_len, x_off, x_len = struct.unpack_from('<4I', checked(s, rec + 0x28, 16))
        nvsd = s.find(NVSD_MAGIC, rec, rec + 0x400)   # big records keep theirs further on; they take the fallback
        variants, vs_code, fs_code, vs_block, fs_block = \
            struct.unpack_from('<5I', checked(s, nvsd + 0x1c, 20)) if nvsd >= 0 else (0, 0, 0, 0, 0)
        if c_off + c_len > ctrl_len or x_off + x_len > code_len:
            counts['record ranges'] += 1
            continue
        if variants and vs_block < FULL_BLOCK and c_len == vs_block + fs_block:
            # Compact blocks (0xd8 bytes) hold no NVN control section: read as one, the 0x880 bytes run into the next
            # blocks, every fragment program fails the checks below and no vertex program matches one a run met.
            # Only when the record's own sizes say so: the NVSD search can land on another record's block (a record
            # of one program has none of its own nearby), and such a record still takes the fallback below.
            counts['compact record'] += 1
            continue
        if variants == 1 and c_len == vs_block + fs_block and x_len == vs_code + fs_code:
            programs = (('vs', 0, vs_code, vs_block), ('ps', vs_block, fs_code, fs_block))
        elif c_len and c_len % FULL_BLOCK == 0:
            # Full blocks one after another with their code in the same order, each program's code as long as its
            # control section says: single programs, and records that hold many (some with a variant count of 1).
            # These take each program's stage from its control section.
            programs = tuple((None, k * FULL_BLOCK, None, FULL_BLOCK) for k in range(c_len // FULL_BLOCK))
        else:
            counts['record layout'] += 1
            continue
        cursor = code_off + x_off
        for want, block, size, block_size in programs:
            control = checked(s, ctrl_off + c_off + block, 0x880)
            c1, length = struct.unpack_from('<II', control, 0x700)
            size = length if size is None else size
            code = checked(s, cursor, size)
            cursor += size
            stage = NVN_STAGES.get(struct.unpack_from('<I', control, 0x714)[0])
            if control[:4] != CONTROL_MAGIC or not stage or want not in (None, stage) or \
                    not 0 <= c1 <= length <= size or not length or cursor > code_off + x_off + x_len:
                counts[f'program bounds, block {block_size:#x}'] += 1
                break
            counts[f'programs, block {block_size:#x}'] += 1
            yield digest(control + code[:length]), stage, control, code[:length]


def ard_scan(job):
    """Worker: the shaders of a list of (offset, size) entries of sts.ard, which starts at base in romfs.bin."""
    path, base, entries = job
    found, counts = {}, defaultdict(int)   # one copy per key: the same program is in many model and map containers
    with open(path, 'rb') as stream:
        for offset, size in entries:
            stream.seek(base + offset)
            raw = stream.read(size)
            if len(raw) != size:
                raise ValueError('short read in the data archive')
            entry_shaders(raw, found, counts)
    return found, dict(counts)


def entry_shaders(raw, found, counts):
    """The programs of every shader container in one archive entry or loose file, into found (key -> stage, control,
    code; the first copy of each key stays)."""
    for piece in ard_entry(raw, counts):
        pos = 0
        while (pos := piece.find(SPCH_MAGIC, pos)) >= 0:
            try:
                for key, stage, control, code in spch_shaders(piece, pos, counts):
                    if key not in found:
                        found[key] = (stage, control, code)
                counts['containers'] += 1
            except (ValueError, struct.error):
                counts['unreadable container'] += 1
            pos += 4


def ard_shaders(romfs_bin, arh_row, ard_row, workers=None):
    """Every program of every shader container in sts.ard as key -> (stage, control, code), and the skip counts. The
    entries go to worker processes in interleaved slices, so the big map files spread over them. The rows give each
    file's offset and size inside romfs_bin; for loose files (add-on content) both offsets are 0 and the paths differ:
    arh_row and ard_row may then carry their own 'file'."""
    with open(arh_row.get('file', romfs_bin), 'rb') as stream:
        stream.seek(arh_row['offset'])
        arh = stream.read(arh_row['size'])
    entries = arh_entries(arh)
    if entries and entries[-1][0] + entries[-1][1] > ard_row['size']:
        raise ValueError('arh2 index does not fit its data archive')
    n = max(1, min(workers or os.cpu_count() or 1, MAX_WORKERS))
    jobs = [(str(ard_row.get('file', romfs_bin)), ard_row['offset'], entries[i::n * 8]) for i in range(n * 8)]
    shaders, counts = {}, defaultdict(int)
    with concurrent.futures.ProcessPoolExecutor(n) as pool:
        for found, c in pool.map(ard_scan, jobs):
            for key, v in found.items():
                shaders.setdefault(key, v)
            for k, v in c.items():
                counts[k] += v
    return shaders, dict(counts)


AOC_FILE_MAX = 1 << 30   # a loose add-on file larger than this is not read whole (the archive pair goes by entries)


def aoc_shaders(aoc, workers=None):
    """The shader programs of the add-on content extracted next to the RomFS (<install>/aoc/<index>/, installer/steps.py
    Dlc): an archive pair (sts.arh + sts.ard) read like the base game's, and any other file read whole for shader
    containers, the way model files hold them. key -> (stage, control, code), and the counts."""
    shaders, counts = {}, defaultdict(int)
    for d in sorted(p for p in aoc.iterdir() if p.is_dir()) if aoc.is_dir() else []:
        files = sorted(p for p in d.rglob('*') if p.is_file())
        # the pair's partner in the same folder whatever its case (Linux file names are case-sensitive)
        by_name = {(f.parent, f.name.lower()): f for f in files}
        for path in files:
            name = path.name.lower()
            if name == ARCHIVE[1] and (path.parent, ARCHIVE[0]) in by_name:
                continue   # sts.ard: read through its index below
            if name == ARCHIVE[0] and (path.parent, ARCHIVE[1]) in by_name:
                ard = by_name[(path.parent, ARCHIVE[1])]
                found, c = ard_shaders(None, {'file': path, 'offset': 0, 'size': path.stat().st_size},
                                       {'file': ard, 'offset': 0, 'size': ard.stat().st_size}, workers)
                for k, v in found.items():
                    shaders.setdefault(k, v)
                for k, v in c.items():
                    counts[k] += v
                continue
            if path.stat().st_size > AOC_FILE_MAX:
                counts['add-on file too big to read whole'] += 1
                continue
            found = {}
            entry_shaders(path.read_bytes(), found, counts)
            for k, v in found.items():
                shaders.setdefault(k, v)
            counts['add-on files read'] += 1
    return shaders, dict(counts)


def startup_package(name):
    """The RomFS files MNG's startup shaders come from; effects and UI layouts carry more."""
    return (name == 'Lib/NintendoSDK/TextureShader.bnsh' or name == 'Pack/Bootup.Nin_NX_NVN.pack.zs' or
            name.startswith('Shader/') and name.endswith('.zs'))


def inventory(romfs, exefs):
    """Every unique shader: key -> dict(stage, control, code, source, origin, startup). As in MNG, a shader is a startup
    shader when it comes from an agl package, a game module, or a startup_package() RomFS file; the others come from
    effect and UI layout resources, which MNG translates only for bind states players have met. Also returns the number
    of .sharcb record candidates rejected for a missing control magic (see sharcb_shaders)."""
    index = json.loads((romfs / 'index.json').read_text(encoding='utf-8'))
    if index.get('status') != 'pass':
        raise ValueError('RomFS has not passed integrity verification')
    entries = {r['path']: r for r in index['files']}
    shaders = {}
    rejected = [0]

    def add(origin, name, found, startup):
        for key, stage, control, code in found:
            s = shaders.setdefault(key, dict(stage=stage, control=control, code=code, source=name, origin=origin,
                                             startup=startup))
            if startup and not s['startup']:
                s.update(source=name, origin=origin, startup=True)

    with (romfs / 'romfs.bin').open('rb') as stream:
        def read(name):
            row = entries[name]
            stream.seek(row['offset'])
            raw = stream.read(row['size'])
            if len(raw) != row['size']:
                raise ValueError('short RomFS read')
            return raw

        dictionaries = {}
        if 'Pack/ZsDic.pack.zs' in entries:   # TotK's shared dictionaries; XCX's RomFS has none
            for raw in sarc(zstd.decompress(read('Pack/ZsDic.pack.zs'))).values():
                d = zstd.ZstdDict(raw)
                dictionaries[d.dict_id] = d

        def decode(raw):
            info = zstd.get_frame_info(raw)
            if info.decompressed_size is None or info.decompressed_size > 512 << 20:
                raise ValueError('unbounded shader package')
            return zstd.decompress(raw, zstd_dict=dictionaries[info.dictionary_id] if info.dictionary_id else None)

        def visit(origin, name, data, startup, depth=0):
            if data[:4] == b'BAHS':
                add('sharcb', name, sharcb_shaders(data, rejected), True)
                return
            if data[:4] == b'SARC':
                if depth > 4:
                    return
                for member, raw in sarc(data).items():
                    visit(origin, name + '/' + member, decode(raw) if raw[:4] == ZSTD else raw, startup, depth + 1)
                return
            pos = 0   # BFSHA and BNSH files, effect and layout resources: complete, independently addressed BNSH files
            while (pos := data.find(b'BNSH\0\0\0\0', pos)) >= 0:
                size = struct.unpack_from('<I', checked(data, pos + 28, 4))[0]
                add(origin, name, bnsh_shaders(checked(data, pos, size)), startup)
                pos += max(size, 8)

        for name in sorted(entries):
            if name in ARCHIVE:
                continue   # ard_shaders below; read whole, sts.ard would take 10 GB of memory
            raw = read(name)
            if raw[:4] not in (ZSTD, b'SARC', b'FSHA', b'BNSH', b'BAHS') and b'BNSH\0\0\0\0' not in raw:
                continue
            visit('bnsh', name, decode(raw) if raw[:4] == ZSTD else raw, startup_package(name))
    if all(name in entries for name in ARCHIVE):
        found, counts = ard_shaders(romfs / 'romfs.bin', entries[ARCHIVE[0]], entries[ARCHIVE[1]])
        # what was read and skipped, so a game update that changes the containers shows as a drop here
        print(f'{ARCHIVE[1]}: {len(found)} unique programs; ' + ', '.join(f'{k} {v}' for k, v in sorted(counts.items())),
              flush=True)
        # The game's own model, map and effect shaders: what play draws with, so they count as startup shaders.
        add('ard', ARCHIVE[1], ((key, *v) for key, v in found.items()), True)
    aoc = romfs.parent / 'aoc'   # the setup's add-on content, beside romfs/ in the install folder
    if aoc.is_dir():
        found, counts = aoc_shaders(aoc)
        print(f'aoc: {len(found)} unique programs; ' + ', '.join(f'{k} {v}' for k, v in sorted(counts.items())), flush=True)
        add('ard', 'aoc', ((key, *v) for key, v in found.items()), True)
    for module in sorted(exefs.iterdir()):
        if module.name in ('main', 'sdk', 'subsdk0', 'rtld'):
            visit('main', 'module:' + module.name, nso_image(module), True)
    return shaders, rejected[0]


def played_shaders(path):
    """Every record of a shader-programs.bin (layout: runtime/gfx_shader.c), in the shape of inventory(). A record's
    vertex shader opens a group its fragment shader joins, as the game wrote each pair; the probes pair shaders by source.
    One shader per stage and code: the probes rebuild the control section from the job, so a record that differs only
    there would be the same shader to them."""
    data = path.read_bytes()
    if data[:7] != b'XCXSPRG':
        raise ValueError(f'{path}: not a shader-programs.bin')
    shaders, seen, pos, group = {}, set(), 16, 0
    while pos + 44 + 0x880 <= len(data):
        stage, length, c1 = struct.unpack_from('<III', data, pos)
        control = data[pos + 44:pos + 44 + 0x880]
        code = data[pos + 44 + 0x880:][:length]
        pos += 44 + 0x880 + length + c1
        name = PLAYED_STAGES.get(stage)
        if name and (name, code) not in seen:
            seen.add((name, code))
            group += name == 'vs'
            key = hashlib.sha256(control + code).hexdigest()
            shaders[key] = dict(stage=name, control=control, code=code, source=f'played/{group}', origin='played',
                                startup=True)
    return shaders


def write_shaders(shaders, out, everything):
    """Write code files and jobs.json; returns {key: dict(job, source, origin)}."""
    (out / 'code').mkdir(parents=True, exist_ok=True)
    jobs = {}
    for key, s in sorted(shaders.items()):
        if not s['startup'] and not everything:
            continue
        control, code, stage = s['control'], s['code'], s['stage']
        c1, length = struct.unpack_from('<II', control, 0x700)
        body = code[0 if stage == 'cs' else 0x30:length]
        path = out / 'code' / f'{key}.code'
        if not path.is_file() or path.read_bytes() != body:
            path.write_bytes(body)
        job = dict(key=key, stage=stage, code=str(path), c1=code[c1:length].hex(), debug=False)
        if stage == 'cs':
            job['local'] = list(struct.unpack_from('<III', control, 0x748))
        jobs[key] = dict(job=job, source=s['source'], origin=s['origin'], startup=s['startup'])
    (out / 'jobs.json').write_text(json.dumps(jobs, indent=0), encoding='utf-8')
    return jobs


def worker(keys, jobs, spv_dir):
    """One --serve process for a slice of keys. Returns (translated, failures {key: message}, errors {key: message})."""
    failures, errors, ok = {}, {}, 0
    process = subprocess.Popen([str(XLATE), '--serve'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, text=True)
    try:
        for key in keys:
            process.stdin.write(json.dumps({'job': jobs[key]['job'], 'out': str(spv_dir)}) + '\n')
            process.stdin.flush()
            reply = process.stdout.readline().rstrip('\n')
            if not reply:
                failures[key] = 'translator service exited'
                break
            if not reply.startswith('ok '):
                failures[key] = reply.split(' ', 2)[2] if reply.count(' ') >= 2 else reply
                continue
            v = subprocess.run([str(SPIRV_VAL), '--target-env', 'vulkan1.3', str(spv_dir / f'{key}.spv')],
                               capture_output=True, text=True)
            if v.returncode:
                text = (v.stderr or v.stdout).strip()
                errors[key] = text.splitlines()[0] if text else 'spirv-val failed'
            else:
                ok += 1
    finally:
        process.stdin.close()
        process.wait()
    return ok, failures, errors


def normalize(message):
    return re.sub(r'\b\d+\b|0x[0-9a-fA-F]+', 'N', message)[:300]


def translate(jobs, out):
    spv_dir = out / 'spv'
    spv_dir.mkdir(exist_ok=True)
    keys = sorted(jobs)
    count = max(1, min(MAX_WORKERS, len(keys)))
    slices = [keys[i::count] for i in range(count)]
    ok, failures, errors = 0, {}, {}
    with concurrent.futures.ThreadPoolExecutor(count) as pool:
        for n, f, e in pool.map(lambda s: worker(s, jobs, spv_dir), slices):
            ok += n
            failures.update(f)
            errors.update(e)
    if failures or errors:
        lines = []
        for title, table in (('translate failures', failures), ('spirv-val errors', errors)):
            groups = defaultdict(list)
            for key, message in table.items():
                groups[normalize(message)].append((key, message))
            lines.append(f'== {title}: {len(table)} in {len(groups)} groups')
            for _, members in sorted(groups.items(), key=lambda g: -len(g[1])):
                lines.append(f'{len(members)}\t{members[0][1][:300]}')
                lines.extend(f'\texample {key}' for key, _ in sorted(members)[:2])
        (out / 'failures.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    else:
        (out / 'failures.txt').unlink(missing_ok=True)
    return ok, len(failures), len(errors)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--version', required=True, help='upd (build/extracted/<version>)')
    ap.add_argument('--played', action='store_true', help='take the shaders from build/<version>/shader-programs.bin')
    ap.add_argument('--all', action='store_true', help='also the layout and effect shaders MNG leaves to bind-state variants')
    ap.add_argument('--translate', action='store_true', help='translate every shader and run spirv-val')
    args = ap.parse_args()
    t0 = time.monotonic()
    extracted = ROOT / 'build' / 'extracted' / args.version
    out = ROOT / 'build' / args.version / 'shaders'
    out.mkdir(parents=True, exist_ok=True)
    shaders, rejected = (played_shaders(out.parent / 'shader-programs.bin'), 0) if args.played else         inventory(extracted / 'romfs', extracted / 'exefs')
    jobs = write_shaders(shaders, out, args.all)
    a, b, c, d = (sum(1 for j in jobs.values() if j['origin'] == o) for o in ('bnsh', 'sharcb', 'main', 'ard'))
    print(f'{len(shaders)} unique shaders found in all, {len(jobs)} selected; {rejected} .sharcb record candidates '
          f'without a control section rejected', flush=True)
    summary = f'shaders {len(jobs)} (bnsh {a}, sharcb {b}, main {c}, archive {d})'
    print(f'extracted {summary} in {time.monotonic() - t0:.1f} s', flush=True)
    if not args.translate:
        print(f'{summary}, total {time.monotonic() - t0:.1f} s')
        return 0
    if not XLATE.is_file() or not SPIRV_VAL.is_file():
        sys.exit(f'missing {XLATE} or {SPIRV_VAL}')
    ok, failed, bad = translate(jobs, out)
    print(f'{summary}, translated {ok}, failed {failed}, spirv-val errors {bad}, total {time.monotonic() - t0:.1f} s')
    return 0


if __name__ == '__main__':
    sys.exit(main())
