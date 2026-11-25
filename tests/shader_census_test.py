"""The XCX shader census (tools/shader_census.py ard_shaders, spch_shaders) and the seed files of
tools/prepare_shaders.py, on archives and containers built here: no game files.

Run: python tests/shader_census_test.py
"""
import json
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import prepare_shaders  # noqa: E402
import shader_census as sc  # noqa: E402

BLOCK = sc.FULL_BLOCK


def control(stage_id, length, c1=None):
    """A 0x850-byte control block: the magic, the code bounds at 0x700 and the stage at 0x714."""
    c = bytearray(BLOCK)
    c[:4] = sc.CONTROL_MAGIC
    struct.pack_into('<II', c, 0x700, length if c1 is None else c1, length)
    struct.pack_into('<I', c, 0x714, stage_id)
    c[0x10:0x14] = struct.pack('<I', length ^ stage_id)   # differs per program
    return bytes(c)


def code(n, tag):
    return bytes((tag + i) & 0xff for i in range(n))


def container(records):
    """An HCPS container. records: list of (blocks, codes, nvsd) where nvsd = (variants, vs_code, fs_code, vs_block,
    fs_block) or None for a record without an NVSD block."""
    ctrl, xv4, recs = b'', b'', []
    for blocks, codes, nvsd in records:
        c_off, x_off = len(ctrl), len(xv4)
        ctrl += b''.join(blocks)
        xv4 += b''.join(codes)
        r = bytearray(0x100)
        r[:4] = sc.SLCT_MAGIC
        struct.pack_into('<4I', r, 0x28, c_off, len(ctrl) - c_off, x_off, len(xv4) - x_off)
        if nvsd:
            r[0xc8:0xcc] = sc.NVSD_MAGIC
            struct.pack_into('<5I', r, 0xc8 + 0x1c, *nvsd)
        recs.append(bytes(r))
    ctrl += bytes(0x30)   # the last block's 0x880-byte read runs on: the section that follows in a real container
    so_off = 48
    rec_off = so_off + 8 * len(recs)
    table = b''.join(struct.pack('<II', i * 0x100, 0) for i in range(len(recs)))
    rec_sec = b''.join(recs)
    code_off = rec_off + len(rec_sec)
    ctrl_off = code_off + len(xv4)
    head = struct.pack('<4s11I', sc.SPCH_MAGIC, sc.SPCH_VERSION, so_off, len(recs), 0, 0, rec_off, len(rec_sec),
                       code_off, len(xv4), ctrl_off, len(ctrl))
    return head + table + rec_sec + xv4 + ctrl


def xbc1(data, method=1):
    packed = zlib.compress(data) if method == 1 else sc.zstd.compress(data)
    return b'xbc1' + struct.pack('<III', method, len(data), len(packed)) + bytes(32) + packed


def romfs(tmp, entries):
    """romfs.bin + index.json holding sts.arh / sts.ard with these entries (bytes each), 16-byte aligned."""
    ard, arh = b'', bytearray(b'arh2' + struct.pack('<III', len(entries), 16, 0))
    for i, e in enumerate(entries):
        arh += struct.pack('<QII', 0x1000 + i, len(e), len(e))
        ard += e + bytes(-len(e) % 16)
    image = bytes(512) + ard + bytes(-len(ard) % 512) + bytes(arh)
    (tmp / 'romfs.bin').write_bytes(image)
    rows = [{'path': 'sts.ard', 'offset': 512, 'size': len(ard)},
            {'path': 'sts.arh', 'offset': 512 + len(ard) + (-len(ard) % 512), 'size': len(arh)}]
    (tmp / 'index.json').write_text(json.dumps({'status': 'pass', 'files': rows}), encoding='utf-8')
    return {r['path']: r for r in rows}


class SpchTest(unittest.TestCase):
    def programs(self, data, pos=0):
        counts = sc.defaultdict(int)
        return list(sc.spch_shaders(data, pos, counts)), dict(counts)

    def test_pair(self):
        vs, fs = code(0x300, 1), code(0x500, 2)
        found, counts = self.programs(container([([control(0, 0x300), control(1, 0x500, 0x400)], [vs, fs],
                                                  (1, 0x300, 0x500, BLOCK, BLOCK))]))
        self.assertEqual([s for _, s, _, _ in found], ['vs', 'ps'])
        self.assertEqual(found[0][3], vs)
        self.assertEqual(found[1][3], fs)
        self.assertEqual(len(found[0][2]), 0x880)   # the runtime's control read, into the next block
        self.assertEqual(found[0][2][BLOCK:BLOCK + 4], sc.CONTROL_MAGIC)
        self.assertEqual(found[0][0], sc.digest(found[0][2] + vs))

    def test_padded_code_follows_nvsd_sizes(self):
        # NVSD sizes larger than the control lengths: the fragment code starts after the padded vertex code
        vs, fs = code(0x200, 3) + bytes(0x100), code(0x200, 4)
        found, _ = self.programs(container([([control(0, 0x200), control(1, 0x200)], [vs, fs],
                                             (1, 0x300, 0x200, BLOCK, BLOCK))]))
        self.assertEqual(found[1][3], fs)

    def test_several_pairs_in_one_record(self):
        blocks = [control(0, 0x100), control(1, 0x200), control(0, 0x300), control(1, 0x100)]
        codes = [code(0x100, 5), code(0x200, 6), code(0x300, 7), code(0x100, 8)]
        for nvsd in ((1, 0x100, 0x200, BLOCK, BLOCK), None):   # a variant count of 1 that lies, and no NVSD
            found, _ = self.programs(container([(blocks, codes, nvsd)]))
            self.assertEqual([k for _, _, _, k in found], codes)
            self.assertEqual([s for _, s, _, _ in found], ['vs', 'ps', 'vs', 'ps'])

    def test_single_program_next_to_another_records_nvsd(self):
        # a one-block record whose NVSD search lands on numbers that are not its own sizes: the fallback reads it
        one = container([([control(0, 0x300)], [code(0x300, 7)], (274, 1, 268, 6291712, 294))])
        found, counts = self.programs(one)
        self.assertEqual([(s, k) for _, s, _, k in found], [('vs', code(0x300, 7))])
        self.assertNotIn('compact record', counts)

    def test_compact_blocks_skipped(self):
        small = [control(0, 0x100)[:0xd8], control(1, 0x100)[:0xd8]]
        found, counts = self.programs(container([(small, [code(0x100, 9), code(0x100, 10)],
                                                  (1, 0x100, 0x100, 0xd8, 0xd8))]))
        self.assertEqual(found, [])
        self.assertEqual(counts.get('compact record'), 1)

    def test_bad_bounds_and_version(self):
        found, counts = self.programs(container([([control(0, 0x900), control(1, 0x100)], [code(0x100, 1)] * 2,
                                                  (1, 0x100, 0x100, BLOCK, BLOCK))]))
        self.assertEqual(found, [])
        self.assertTrue(any(k.startswith('program bounds') for k in counts))
        data = bytearray(container([([control(0, 0x100), control(1, 0x100)], [code(0x100, 1)] * 2,
                                     (1, 0x100, 0x100, BLOCK, BLOCK))]))
        data[4] = 0x99
        self.assertEqual(self.programs(bytes(data)), ([], {'container version': 1}))

    def test_truncated_container_raises(self):
        data = container([([control(0, 0x100), control(1, 0x100)], [code(0x100, 1)] * 2,
                           (1, 0x100, 0x100, BLOCK, BLOCK))])
        with self.assertRaises(ValueError):
            self.programs(data[:len(data) // 2])


class ArchiveTest(unittest.TestCase):
    def test_archive(self):
        a = container([([control(0, 0x100), control(1, 0x200)], [code(0x100, 1), code(0x200, 2)],
                         (1, 0x100, 0x200, BLOCK, BLOCK))])
        b = container([([control(0, 0x300), control(1, 0x100)], [code(0x300, 3), code(0x100, 4)],
                         (1, 0x300, 0x100, BLOCK, BLOCK))])
        c = container([([control(0, 0x200), control(1, 0x300)], [code(0x200, 5), code(0x300, 6)],
                         (1, 0x200, 0x300, BLOCK, BLOCK))])
        entries = [
            b'plain' + a,                                             # stored as it is
            xbc1(b'one') + xbc1(b + b'tail', 3),                      # blocks back to back, the second zstd
            b'DRSM' + bytes(60) + xbc1(b'pad' + xbc1(c)) + b'xbc1junk',  # nested, and a magic that does not expand
            b'nothing here',
        ]
        with tempfile.TemporaryDirectory() as t:
            tmp = Path(t)
            rows = romfs(tmp, entries)
            shaders, counts = sc.ard_shaders(tmp / 'romfs.bin', rows['sts.arh'], rows['sts.ard'], workers=2)
        self.assertEqual(len(shaders), 6)
        self.assertEqual(sorted(s for s, _, _ in shaders.values()), ['ps'] * 3 + ['vs'] * 3)
        self.assertEqual(counts['containers'], 3)

    def test_block_that_expands_past_its_size(self):
        for method in (1, 3):
            block = bytearray(xbc1(bytes(1 << 20), method))
            struct.pack_into('<I', block, 8, 100)   # claims 100 bytes; holds 1 MB of zeros
            with self.assertRaises(ValueError):
                sc.xbc1_block(bytes(block), 0)
            self.assertEqual(list(sc.xbc1_expand(bytes(block))), [bytes(block)])   # dropped, not expanded

    def test_skipped_blocks_are_counted(self):
        deep = b'x'
        for _ in range(sc.XBC1_DEPTH + 1):
            deep = b'pad' + xbc1(deep)
        counts = sc.defaultdict(int)
        list(sc.ard_entry(deep, counts))
        self.assertEqual(counts['xbc1 past the depth limit'], 1)
        bad = bytearray(xbc1(bytes(64)))
        bad[48:50] = b'\xff\xff'   # the packed stream's header damaged
        counts = sc.defaultdict(int)
        list(sc.ard_entry(b'pad' + bytes(bad), counts))
        self.assertEqual(counts['xbc1 unreadable'], 1)
        counts = sc.defaultdict(int)
        list(sc.ard_entry(b'xbc1 in plain text', counts))   # no real header: not counted
        self.assertEqual(dict(counts), {})

    def test_program_table_longer_than_its_records(self):
        data = bytearray(container([([control(0, 0x100), control(1, 0x100)], [code(0x100, 1)] * 2,
                                     (1, 0x100, 0x100, BLOCK, BLOCK))]))
        struct.pack_into('<I', data, 12, 1000)   # so_n
        with self.assertRaises(ValueError):
            list(sc.spch_shaders(bytes(data), 0, sc.defaultdict(int)))

    def test_add_on_content(self):
        # aoc/<index>/: an archive pair of loose files, and a loose model file holding a container
        a = container([([control(0, 0x100), control(1, 0x200)], [code(0x100, 11), code(0x200, 12)],
                         (1, 0x100, 0x200, BLOCK, BLOCK))])
        b = container([([control(0, 0x300), control(1, 0x100)], [code(0x300, 13), code(0x100, 14)],
                         (1, 0x300, 0x100, BLOCK, BLOCK))])
        with tempfile.TemporaryDirectory() as t:
            aoc = Path(t) / 'aoc'
            one = aoc / '1' / 'sub'
            one.mkdir(parents=True)
            ard = xbc1(a) + bytes(-len(xbc1(a)) % 16)
            (one / 'sts.ard').write_bytes(ard)
            (one / 'sts.arh').write_bytes(b'arh2' + struct.pack('<III', 1, 16, 0) +
                                          struct.pack('<QII', 1, len(xbc1(a)), len(a)))
            (aoc / '2').mkdir()
            (aoc / '2' / 'model.bin').write_bytes(b'DRSM' + bytes(60) + xbc1(b))
            shaders, counts = sc.aoc_shaders(aoc, workers=1)
        self.assertEqual(len(shaders), 4)
        self.assertEqual(counts['add-on files read'], 1)
        self.assertEqual(counts['containers'], 2)

    def test_arh_checks(self):
        with self.assertRaises(ValueError):
            sc.arh_entries(b'arh1' + bytes(12))
        with self.assertRaises(ValueError):
            sc.arh_entries(b'arh2' + struct.pack('<III', 1, 12, 0))   # alignment not a power of two
        with self.assertRaises(ValueError):
            sc.arh_entries(b'arh2' + struct.pack('<III', 5, 16, 0))   # table runs past the file


class SeedTest(unittest.TestCase):
    def test_header_has_the_runtime_magic(self):
        # gfx_shader.c RecHeader: char magic[8] compared with its NUL
        for m in prepare_shaders.RECORD_MAGIC.values():
            self.assertEqual(len(m), 8)
            self.assertEqual(m[-1:], b'\0')
            self.assertEqual(len(m + struct.pack('<II', 1, 0)), 16)

    def test_mvprev_bit(self):
        ctl = bytes(0x880)
        self.assertEqual(prepare_shaders.answers(dict(prepare_shaders.DEFAULT, mvprev=True), 0, ctl)[-1], 0x80)
        self.assertEqual(prepare_shaders.answers(dict(prepare_shaders.DEFAULT, mvprev=True), 4, ctl)[-1], 0)
        self.assertEqual(prepare_shaders.answers(prepare_shaders.DEFAULT, 0, ctl)[-1], 0)
        self.assertEqual(len(prepare_shaders.answers(prepare_shaders.DEFAULT, 0, ctl)), 147)


if __name__ == '__main__':
    unittest.main()
