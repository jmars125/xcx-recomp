"""Check a packed release as a player receives it, on a machine that did not build it.

    python tools/check_release.py XCX-Setup-<version>-windows.zip
    python tools/check_release.py XCX-Setup-<version>-linux.AppImage
    python tools/check_release.py XCX-Setup-<version>-linux.tar.gz

Unpacks the package into build/release-verify (the tarball with the system's tar, as a launcher would, keeping modes
and symlinks; it must hold the one folder XCX-Setup/), then runs the checks of tools/make_release.py and
tools/make_release_linux.py over the result: check_content over every file (and the glibc floor on Linux), then the
check-only installer pass, the window's --selftest and --play on a fake build (through AppRun and the start script
xcx-setup on Linux) with the package's own python. No game files are involved. On
Linux the window needs a display (xvfb-run). The host check needs a Vulkan 1.3 device on both, as on a player's machine.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_release as mr  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('package', type=Path, help='the .zip, .AppImage or .tar.gz')
    package = ap.parse_args().package.resolve()
    work = mr.MAIN_ROOT / 'build' / 'release-verify'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    if package.suffix == '.zip':
        with zipfile.ZipFile(package) as z:
            z.extractall(work)
        out = work / 'XCX-Setup'
        mr.check_folder(out)
        mr.check_bundle(out)
    elif package.name.endswith('.tar.gz'):
        import make_release_linux as mrl
        with tarfile.open(package) as t:
            tops = {m.name.split('/')[0] for m in t.getmembers()}
        if tops != {mrl.TAR_ROOT}:
            sys.exit(f'{package.name}: top folders {sorted(tops)}, expected only {mrl.TAR_ROOT}/')
        subprocess.run(['tar', '-xzf', str(package), '-C', str(work)], check=True)
        out = work / mrl.TAR_ROOT
        for entry in ('AppRun', 'xcx-setup', 'python/bin/python3'):
            if not os.access(out / entry, os.X_OK):
                sys.exit(f'{package.name}: {entry} is not executable after unpacking')
        mr.check_folder(out)
        mrl.check_glibc(out)
        mrl.check_bundle(out)
    else:
        import make_release_linux as mrl
        package.chmod(0o755)
        subprocess.run([str(package), '--appimage-extract'], cwd=work, check=True, stdout=subprocess.DEVNULL,
                       env=dict(os.environ, APPIMAGE_EXTRACT_AND_RUN='1'))
        out = work / 'squashfs-root'
        mr.check_folder(out)
        mrl.check_glibc(out)
        mrl.check_bundle(out)


if __name__ == '__main__':
    main()
