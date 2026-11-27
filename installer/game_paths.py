"""Which paths are files of the installed game, for the mod manager's layout check (installer/mod_layout.py).

The game's files are the loose ones of the RomFS index (<dest>/romfs/index.json, tools/extract.py) and the entries of
the sts.arh/sts.ard archive inside it. sts.arh holds no names, only the XXH64 of each path with a leading '/'
(the runtime looks them up the same way), so a path is checked by hashing it, and the game's names never have to be
listed anywhere. A path is lowercase with '/' between its parts and no leading '/', as runtime/romfs_mods.c keys it.
"""
import json
import struct
from pathlib import Path

M64 = (1 << 64) - 1
P1, P2, P3, P4, P5 = (0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9, 0x85EBCA77C2B2AE63,
                      0x27D4EB2F165667C5)
ARH_LIMIT = 64 << 20   # bytes of sts.arh this reads at most


def _rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & M64


def _round(acc, lane):
    return (_rotl((acc + lane * P2) & M64, 31) * P1) & M64


def xxh64(data, seed=0):
    """XXH64 of data (bytes), as the reference implementation (github.com/Cyan4973/xxHash) defines it."""
    n, i = len(data), 0
    if n >= 32:
        v = [(seed + P1 + P2) & M64, (seed + P2) & M64, seed & M64, (seed - P1) & M64]
        while i + 32 <= n:
            for k in range(4):
                v[k] = _round(v[k], struct.unpack_from('<Q', data, i + 8 * k)[0])
            i += 32
        h = (_rotl(v[0], 1) + _rotl(v[1], 7) + _rotl(v[2], 12) + _rotl(v[3], 18)) & M64
        for k in range(4):
            h = ((h ^ _round(0, v[k])) * P1 + P4) & M64
    else:
        h = (seed + P5) & M64
    h = (h + n) & M64
    while i + 8 <= n:
        h ^= _round(0, struct.unpack_from('<Q', data, i)[0])
        h = (_rotl(h, 27) * P1 + P4) & M64
        i += 8
    if i + 4 <= n:
        h ^= (struct.unpack_from('<I', data, i)[0] * P1) & M64
        h = (_rotl(h, 23) * P2 + P3) & M64
        i += 4
    while i < n:
        h ^= (data[i] * P5) & M64
        h = (_rotl(h, 11) * P1) & M64
        i += 1
    h ^= h >> 33
    h = (h * P2) & M64
    h ^= h >> 29
    h = (h * P3) & M64
    h ^= h >> 32
    return h


class GamePaths:
    """The game's files: loose names and sts.arh hashes. Empty (knows nothing) when the install has no RomFS."""

    def __init__(self, loose=(), hashes=()):
        self.loose = {p.lower() for p in loose}
        self.hashes = set(hashes)

    def __bool__(self):
        return bool(self.loose or self.hashes)

    def __contains__(self, path):
        key = path.replace('\\', '/').strip('/').lower()
        return key in self.loose or xxh64(('/' + key).encode('utf-8')) in self.hashes

    @classmethod
    def of_install(cls, dest):
        """The game files of an install folder; an empty GamePaths when they cannot be read (no game set up yet)."""
        romfs = Path(dest) / 'romfs'
        try:
            files = json.loads((romfs / 'index.json').read_text(encoding='utf-8'))['files']
            loose = [f['path'] for f in files]
            arh = next((f for f in files if f['path'].lower() == 'sts.arh'), None)
            hashes = ()
            if arh and 16 <= arh['size'] <= ARH_LIMIT:
                with open(romfs / 'romfs.bin', 'rb') as f:
                    f.seek(arh['offset'])
                    hashes = arh_hashes(f.read(arh['size']))
            return cls([p for p in loose if p.lower() not in ('sts.arh', 'sts.ard')], hashes)
        except (OSError, ValueError, KeyError, TypeError, struct.error):
            return cls()


def arh_hashes(data):
    """The path hashes of an sts.arh: a 16-byte header (magic, count, alignment, reserved), then count rows of
    (XXH64 of the path, stored size, size)."""
    _magic, count, _align, _ = struct.unpack_from('<4sIII', data, 0)
    if 16 + 16 * count > len(data):
        raise ValueError('sts.arh is shorter than its count')
    return {struct.unpack_from('<Q', data, 16 + 16 * k)[0] for k in range(count)}
