# Pinned libarchive subset

`pristine/libarchive/` holds the files of libarchive 3.8.11 that its RAR and RAR5 readers need, copied unmodified from
`libarchive-3.8.11.tar.xz` (SHA-256 `a3dc0872b1b39da34dc18ef1b944f0f11ffd37425d70ae04be679279f1cf43d2`, from the
[v3.8.11 release](https://github.com/libarchive/libarchive/releases/tag/v3.8.11)), and `pristine/libarchive/test/`
eight of its uuencoded RAR test archives. Every file is pinned by SHA-256 in `pins.json`. `COPYING` is the
distribution's licence: BSD-2-Clause for the C sources; the BLAKE2s reference files are CC0-1.0, OpenSSL or Apache-2.0.

`config.h` is ours: it configures that subset for the two targets the setup builds with its bundled clang
(x86_64-w64-mingw32 and x86_64-linux-gnu), with no compression libraries, crypto, ACLs or xattrs.

`tools/build_unpack.py` checks every pin, then compiles `installer/unpack.c` with the units it lists into
`xcx-unpack`, the mod manager's RAR reader (`installer/archive.py`). The release scripts put it in `bin/` and run it
on the test archives (`build_unpack.smoke`); `tests/mods_archive_test.py` does the same and more.

```sh
python3 tools/build_unpack.py --cc clang
python3 tests/mods_archive_test.py
```

Why libarchive: it reads RAR 4 and 5 under a BSD licence. The RAR reader of 7-Zip and the UnRAR source carry
RarLab's licence, which forbids using the code to recreate the RAR compression algorithm and is not open source.

`tools/check_libarchive_pins.py` downloads the release archive, checks it against `archive_sha256`, and compares every
pinned file with it byte for byte (`--archive FILE` for one on disk).

To move to a newer libarchive: `python tools/check_libarchive_pins.py --update VERSION --sha256 HASH` (the hash from the
release page) copies the same files from its release archive and rewrites `pins.json`; then rebuild and run the tests
above. Do not edit pristine files; anything the build needs changed goes
in `config.h` or `installer/unpack.c`. libarchive's RAR readers have had security fixes in most releases, so keep the
pin current.
