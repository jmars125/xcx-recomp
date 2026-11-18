"""Play from Setup (installer/play.py, gui.py --play, cli.py --play): the finished-build marker, where Setup looks,
the remembered folder (installer/recent.py), the arguments that go to the game and a fake game started and waited for. No game files, no
window; stdlib only.

Run: python tests/installer_play_test.py
"""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tests'))
from installer import cli, gui, play, recent, state  # noqa: E402
from installer_test import Bed  # noqa: E402

FAKE_GAME = 'fake-game.cmd' if os.name == 'nt' else 'fake-game'


def layout(root, release='1.0', dev=False):
    return SimpleNamespace(root=Path(root), release=release, dev=dev)


def fake_install(dest, release='1.0', complete=True, game=True):
    """A folder as a finished run leaves it, with a game that records its arguments in args.json and exits 7."""
    dest.mkdir(parents=True, exist_ok=True)
    data = state.empty(release)
    if complete:
        data['complete'] = release
    state.save(dest, data)
    if game:
        script = dest / 'fake_game.py'
        script.write_text('import json, sys\nopen("args.json", "w").write(json.dumps(sys.argv[1:]))\nsys.exit(7)\n')
        exe = dest / FAKE_GAME
        if os.name == 'nt':
            exe.write_text(f'@"{sys.executable}" fake_game.py %*\r\n@exit /b %errorlevel%\r\n')
        else:
            exe.write_text(f'#!/bin/sh\nexec "{sys.executable}" fake_game.py "$@"\n')
            exe.chmod(0o755)
    return dest


class MarkerTest(unittest.TestCase):
    """runner.run_steps records `complete` at the end of a good run and drops it before a step changes the folder."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.bed = Bed(Path(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def test_good_run_marks_complete(self):
        self.assertEqual(self.bed.run().state['complete'], '1.0')

    def test_failed_first_run_is_not_complete(self):
        r = self.bed.run(fail={'compile': 'error'})
        self.assertNotIn('complete', r.state)

    def test_failed_rerun_drops_the_marker(self):
        self.bed.run()
        (self.bed.dest / 'compile.out').unlink()
        r = self.bed.run(fail={'compile': 'error'})
        self.assertEqual(r.result, 'failed')
        self.assertNotIn('complete', r.state)

    def test_rerun_with_nothing_to_do_keeps_it(self):
        self.bed.run()
        self.assertEqual(self.bed.run().state['complete'], '1.0')

    def test_update_marks_the_new_release(self):
        self.bed.run()
        self.assertEqual(self.bed.run(release='1.1').state['complete'], '1.1')


@mock.patch.object(play, 'GAME_EXE', FAKE_GAME)
class FindTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)
        self.setup = self.base / 'XCX-Setup'
        self.setup.mkdir()
        os.environ.pop('APPIMAGE', None)

    def tearDown(self):
        self.tmp.cleanup()

    def test_playable_needs_marker_release_and_exe(self):
        good = fake_install(self.base / 'a')
        self.assertTrue(play.playable(good, '1.0'))
        self.assertFalse(play.playable(good, '1.1'))   # another release: update first
        self.assertFalse(play.playable(fake_install(self.base / 'b', complete=False), '1.0'))
        self.assertFalse(play.playable(fake_install(self.base / 'c', game=False), '1.0'))
        self.assertFalse(play.playable(self.base / 'missing', '1.0'))
        (self.base / 'd').mkdir()
        (self.base / 'd' / state.NAME).write_text('not json')
        self.assertFalse(play.playable(self.base / 'd', '1.0'))

    def remembered(self, dest):
        """What the window's recent.save leaves behind, read back the way gui.py does."""
        with mock.patch.dict(os.environ, {recent.ENV: str(self.base / 'recent.json')}):
            recent.save(str(dest), {})
            return recent.load_bounded().get('dest')

    def test_remembered_folder_with_a_ready_build_plays(self):
        dest = fake_install(self.base / 'Games' / 'XCX')
        lay = layout(self.setup)
        self.assertEqual(play.find(lay), (None, None))
        last = self.remembered(dest)
        self.assertEqual(last, str(dest.resolve()))
        self.assertEqual(play.find(lay, last=last), (Path(last), Path(last)))

    def test_folder_inside_setup_folder(self):
        dest = fake_install(self.setup / 'game')
        self.assertEqual(play.find(layout(self.setup))[0], dest)

    def test_outdated_remembered_install_is_known_not_playable(self):
        dest = fake_install(self.base / 'old', release='0.9')
        last = self.remembered(dest)
        self.assertEqual(play.find(layout(self.setup), last=last), (None, Path(last)))

    def test_given_folder_only(self):
        other = fake_install(self.setup / 'game')
        given = self.base / 'elsewhere'
        self.assertEqual(play.find(layout(self.setup), given, last=str(other)), (None, None))
        self.assertEqual(play.find(layout(self.setup), other), (other, other))

    def test_appimage_looks_beside_the_file(self):
        image = self.base / 'Apps' / 'XCX-Setup.AppImage'
        image.parent.mkdir()
        dest = fake_install(image.parent / 'XCX')
        lay = layout(self.base / 'mount')   # the read-only mount
        with mock.patch.dict(os.environ, {'APPIMAGE': str(image)}):
            self.assertEqual(play.find(lay)[0], dest)


@unittest.skipIf(os.name == 'nt', 'the Linux location of the remembered install')
class RecentLinuxTest(unittest.TestCase):
    """installer/recent.py on Linux writes nothing outside the install folder unless asked (XCX_SETUP_RECENT)."""

    def setUp(self):
        self.env = mock.patch.dict(os.environ, {recent.ENV: '', 'XDG_CONFIG_HOME': '/home/p/.config'})
        self.env.start()
        os.environ.pop(recent.ENV)
        os.environ.pop('APPIMAGE', None)

    def tearDown(self):
        self.env.stop()

    def test_appimage_keeps_no_file(self):
        with mock.patch.dict(os.environ, {'APPIMAGE': '/x/XCX-Setup.AppImage'}):
            self.assertIsNone(recent.path())
            self.assertEqual(recent.read(), {})
            recent.save('/x/XCX', {})   # nothing to write, no error

    def test_unpacked_release_keeps_it_in_its_own_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'python' / 'bin').mkdir(parents=True)
            (Path(tmp) / 'python' / 'bin' / 'python3').write_text('')
            with mock.patch.object(recent, 'SETUP_ROOT', Path(tmp)):
                self.assertEqual(recent.path(), Path(tmp) / 'setup.json')

    def test_repository_uses_the_config_folder(self):
        with mock.patch.object(recent, 'SETUP_ROOT', ROOT):
            self.assertEqual(recent.path(), Path('/home/p/.config/xcx-recomp/setup.json'))

    def test_override_wins(self):
        with mock.patch.dict(os.environ, {'APPIMAGE': '/x/a.AppImage', recent.ENV: '/data/setup.json'}):
            self.assertEqual(recent.path(), Path('/data/setup.json'))


class ArgsTest(unittest.TestCase):
    def test_window_passes_unknown_arguments(self):
        args, game = gui.parse(['--play', '--fullscreen', '--set', 'FPS_LIMIT=30', '--lang', 'en'])
        self.assertTrue(args.play)
        self.assertEqual(args.lang, 'en')
        self.assertEqual(game, ['--fullscreen', '--set', 'FPS_LIMIT=30'])

    def test_after_double_dash_all_go_to_the_game(self):
        args, game = gui.parse(['--dest', 'D', '--', '--lang', 'ru', '--set=A=1'])
        self.assertEqual((args.dest, args.lang), (Path('D'), None))
        self.assertEqual(game, ['--lang', 'ru', '--set=A=1'])

    def test_cli_refuses_unknown_without_play(self):
        with mock.patch('sys.stdout'), self.assertRaises(SystemExit) as e:
            cli.main(['--dest', 'D', '--fullscreen'])
        self.assertEqual(e.exception.code, 2)


@mock.patch.object(play, 'GAME_EXE', FAKE_GAME)
class StartTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dest = fake_install(Path(self.tmp.name) / 'XCX Game')

    def tearDown(self):
        self.tmp.cleanup()

    def args(self):
        return json.loads((self.dest / 'args.json').read_text())

    def test_run_waits_and_returns_the_games_code(self):
        self.assertEqual(play.run(self.dest, ['--fullscreen', '--set', 'FPS_LIMIT=30']), 7)
        self.assertEqual(self.args(), ['--fullscreen', '--set', 'FPS_LIMIT=30'])

    def test_missing_game(self):
        with mock.patch('sys.stderr'):
            self.assertEqual(play.run(Path(self.tmp.name) / 'none'), 127)

    def test_setup_environment_stays_out(self):
        with mock.patch.dict(os.environ, {'TCL_LIBRARY': 'x', 'APPDIR': 'y', 'XCX_KEEP': '1'}):
            env = play.game_env()
        self.assertNotIn('TCL_LIBRARY', env)
        self.assertNotIn('APPDIR', env)
        self.assertEqual(env['XCX_KEEP'], '1')

    def test_cli_play_on_a_finished_folder_starts_at_once(self):
        lay = layout(self.tmp.name)
        with mock.patch.object(cli.layouts, 'find', return_value=lay):
            code = cli.main(['--dest', str(self.dest), '--play', '--windowed', '--', '--set', 'FPS_LIMIT=40'])
        self.assertEqual(code, 7)
        self.assertEqual(self.args(), ['--windowed', '--set', 'FPS_LIMIT=40'])

    def test_window_play_without_window(self):
        lay = layout(self.tmp.name)
        with mock.patch.object(gui.layouts, 'find', return_value=lay):
            code = gui.main(['--play', '--dest', str(self.dest), '--fullscreen'])
        self.assertEqual(code, 7)
        self.assertEqual(self.args(), ['--fullscreen'])


if __name__ == '__main__':
    unittest.main()
