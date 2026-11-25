"""installer/ bookkeeping with fake steps: fresh install, resume after a failure or cancel, transient steps, update
with a newer release, xcx.ini and the event stream. No game files, no tools; stdlib only.

Run: python tests/installer_test.py
"""
import io
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from installer import ini, runner, state  # noqa: E402
from installer.events import Reporter  # noqa: E402
from installer.layout import Layout  # noqa: E402


class Fake(runner.Step):
    """A step whose output is <dir>/<id>.out holding its key; dir is the temp folder for a transient step."""

    def __init__(self, id, transient=False, always=False, previous=None):
        self.id, self.transient, self.always, self.previous = id, transient, always, previous

    def out(self, ctx):
        return (ctx.temp if self.transient else ctx.dest) / f'{self.id}.out'

    def key(self, ctx):
        upstream = ctx.keys[self.previous] if self.previous else ''
        return f'{self.id}:{ctx.options.inputs.get(self.id, "v1")}:{upstream}'

    def current(self, ctx, record):
        p = self.out(ctx)
        return p.is_file() and p.read_text() == ctx.keys[self.id]

    def verify(self, ctx, record):
        ctx.options.verified.append(self.id)
        return ctx.options.verify_ok.get(self.id, True)

    def run(self, ctx):
        o = ctx.options
        o.ran.append(self.id)
        if self.id in ctx.forced:
            o.forced.append(self.id)
        if self.id == 'check':
            ctx.verify_kept(self.id)
        failure = o.fail.pop(self.id, None)
        if failure == 'error':
            raise runner.InstallError('fake.failed', step=self.id)
        if failure == 'cancel':
            raise KeyboardInterrupt
        if self.id == 'cleanup':
            for p in ctx.temp.glob('*.out'):
                p.unlink()
            return {}
        p = self.out(ctx)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(ctx.keys[self.id])
        return {'note': self.id}


def fake_steps():
    return [Fake('check', always=True), Fake('extract'), Fake('lift', transient=True, previous='extract'),
            Fake('compile', previous='lift'), Fake('prepare', previous='compile'), Fake('cleanup', always=True)]


class Bed:
    """A dest and temp folder plus one run of the fake steps per call."""

    def __init__(self, base):
        self.dest, self.temp = base / 'dest', base / 'temp'
        self.temp.mkdir()

    def run(self, release='1.0', inputs=None, fail=None, verify_ok=None, verify=False):
        layout = Layout(root=Path('.'), dev=True, release=release, python=Path('python'), tools=Path('tools'),
                        recomp=Path('recomp'), llvm=Path('llvm'),
                        deps=Path('deps'), runtime=Path('rt'), runtime_lib=Path('lib'), ninja=Path('ninja'),
                        hactool=Path('hactool'), xlate=Path('xlate'))
        options = SimpleNamespace(inputs=inputs or {}, fail=dict(fail or {}), verify_ok=verify_ok or {}, ran=[],
                                  verified=[], forced=[])
        out = io.StringIO()
        ctx = runner.Context(layout, self.dest, self.temp, Reporter(out=out), options, state=state.load(self.dest),
                             verify=verify)
        mode = ctx.mode
        result = runner.run_steps(fake_steps(), ctx)
        events = [json.loads(line) for line in out.getvalue().splitlines()]
        return SimpleNamespace(result=result, ran=options.ran, verified=options.verified, forced=options.forced,
                               events=events, mode=mode,
                               state=state.load(self.dest))

    @staticmethod
    def skipped(r):
        return {e['step']: e['reason'] for e in r.events if e['type'] == 'step' and e['state'] == 'skipped'}


ALL = ['check', 'extract', 'lift', 'compile', 'prepare', 'cleanup']


class RunnerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.bed = Bed(Path(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def test_fresh_install_runs_everything_and_records_it(self):
        r = self.bed.run()
        self.assertEqual((r.result, r.mode, r.ran), ('ok', 'install', ALL))
        self.assertEqual(set(r.state['steps']), {'extract', 'lift', 'compile', 'prepare'})
        self.assertEqual(r.state['steps']['compile']['note'], 'compile')
        self.assertEqual(r.state['release'], '1.0')
        started = [e['step'] for e in r.events if e['type'] == 'step' and e['state'] == 'started']
        done = [e['step'] for e in r.events if e['type'] == 'step' and e['state'] == 'done']
        self.assertEqual((started, done), (ALL, ALL))

    def test_rerun_skips_finished_steps_and_unneeded_transient(self):
        self.bed.run()
        r = self.bed.run()
        self.assertEqual((r.result, r.mode, r.ran), ('ok', 'resume', ['check', 'cleanup']))
        self.assertEqual(self.bed.skipped(r), {'extract': 'current', 'lift': 'not_needed', 'compile': 'current',
                                               'prepare': 'current'})
        self.assertEqual(r.verified, [])   # same release: no full checks

    def test_resume_after_failure(self):
        r = self.bed.run(fail={'compile': 'error'})
        self.assertEqual((r.result, r.ran), ('failed', ['check', 'extract', 'lift', 'compile']))
        error = [e for e in r.events if e['type'] == 'error'][0]
        self.assertEqual((error['id'], error['step'], error['params']), ('fake.failed', 'compile', {'step': 'compile'}))
        self.assertEqual(set(r.state['steps']), {'extract', 'lift'})
        r = self.bed.run()
        # lift's output is still in the temp folder (no cleanup ran), so only compile and later run.
        self.assertEqual((r.result, r.mode, r.ran), ('ok', 'resume', ['check', 'compile', 'prepare', 'cleanup']))

    def test_resume_after_cancel_with_transient_output_gone(self):
        r = self.bed.run(fail={'prepare': 'cancel'})
        self.assertEqual(r.result, 'cancelled')
        self.assertNotIn('prepare', r.state['steps'])
        for p in self.bed.temp.glob('*.out'):
            p.unlink()
        r = self.bed.run()
        self.assertEqual(r.ran, ['check', 'prepare', 'cleanup'])
        self.assertEqual(self.bed.skipped(r)['lift'], 'not_needed')

    def test_failed_check_writes_nothing(self):
        r = self.bed.run(fail={'check': 'error'})
        self.assertEqual(r.result, 'failed')
        self.assertFalse(self.bed.dest.exists())

    def test_removed_output_runs_again(self):
        self.bed.run()
        (self.bed.dest / 'compile.out').unlink()
        r = self.bed.run()
        self.assertEqual(r.ran, ['check', 'lift', 'compile', 'cleanup'])

    def test_update_with_new_lifter_keeps_extraction(self):
        self.bed.run(release='1.0')
        r = self.bed.run(release='1.1', inputs={'lift': 'v2'})
        self.assertEqual((r.result, r.mode), ('ok', 'update'))
        self.assertEqual(r.ran, ['check', 'lift', 'compile', 'prepare', 'cleanup'])
        self.assertEqual(r.verified, ['extract'])   # kept from another release: full check
        self.assertEqual(r.state['steps']['extract']['release'], '1.0')
        self.assertEqual(r.state['steps']['compile']['release'], '1.1')
        self.assertEqual(r.state['release'], '1.1')

    def test_update_that_changes_nothing_skips_the_rebuild(self):
        self.bed.run(release='1.0')
        r = self.bed.run(release='1.1')
        self.assertEqual(r.ran, ['check', 'cleanup'])
        self.assertEqual(r.verified, ['extract', 'compile', 'prepare'])

    def test_update_with_changed_kept_files_extracts_again(self):
        self.bed.run(release='1.0')
        r = self.bed.run(release='1.1', verify_ok={'extract': False})
        self.assertEqual(r.ran, ['check', 'extract', 'cleanup'])
        self.assertEqual(r.forced, ['extract'])   # the step knows not to trust what it left behind
        warnings = [e for e in r.events if e['type'] == 'warning']
        self.assertEqual([(w['id'], w['params']) for w in warnings], [('state.output_changed', {'step': 'extract'})])

    def test_verify_option_checks_same_release(self):
        self.bed.run()
        r = self.bed.run(verify=True)
        self.assertEqual(r.verified, ['extract', 'compile', 'prepare'])

    def test_state_errors(self):
        self.bed.dest.mkdir()
        (self.bed.dest / state.NAME).write_text('{not json')
        with self.assertRaises(state.StateError) as e:
            state.load(self.bed.dest)
        self.assertEqual(e.exception.id, 'state.unreadable')
        (self.bed.dest / state.NAME).write_text(json.dumps({'format': 99, 'steps': {}}))
        with self.assertRaises(state.StateError) as e:
            state.load(self.bed.dest)
        self.assertEqual(e.exception.id, 'state.newer_format')


class IniTest(unittest.TestCase):
    EXAMPLE = '[display]\n# vk_present = mailbox\n\n[language]\n# locale = en-US\n# lang = en-US\n# x = 1\n'

    def test_example_line_is_activated(self):
        out = ini.set_language(self.EXAMPLE, 'ru')
        self.assertIn('\nlang = ru\n', out)
        self.assertNotIn('# lang', out)
        self.assertIn('# locale = en-US', out)

    def test_active_line_is_rewritten_and_the_rest_kept(self):
        text = '[display]\nvk_present = fifo\n[language]\n  LANG =  ja  \nfps60 = 0\n'
        out = ini.set_language(text, 'de')
        self.assertEqual(out, '[display]\nvk_present = fifo\n[language]\n  lang =  de\nfps60 = 0\n')
        self.assertEqual(ini.get_language(out), 'de')

    def test_missing_section_is_added(self):
        self.assertEqual(ini.set_language('fps60 = 0\n', 'fr'), 'fps60 = 0\n\n[language]\nlang = fr\n')
        self.assertEqual(ini.set_language('[language]\nlocale = de-DE\n', 'fr'),
                         '[language]\nlang = fr\nlocale = de-DE\n')

    def test_write_keeps_user_lines(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            example = d / 'xcx.ini.example'
            example.write_text(self.EXAMPLE)
            path = ini.write(d / '', 'en-GB', example)
            self.assertEqual(ini.get_language(path.read_text()), 'en-GB')
            path.write_text(path.read_text().replace('# vk_present = mailbox', 'vk_present = fifo'))
            ini.write(d, 'ja', example)
            text = path.read_text()
            self.assertIn('vk_present = fifo', text)
            self.assertEqual(ini.get_language(text), 'ja')


class EventsTest(unittest.TestCase):
    def test_lines_are_json_and_the_log_is_english(self):
        with tempfile.TemporaryDirectory() as d:
            out = io.StringIO()
            rep = Reporter(out=out, log_path=Path(d) / 'x.log')
            rep.step('check', 'started')
            rep.progress('extract', 'copy', 0, 10, 'bytes')
            rep.progress('extract', 'copy', 5, 10, 'bytes')    # throttled
            rep.progress('extract', 'copy', 10, 10, 'bytes')   # last one always goes out
            rep.error('check.disk_space', {'path': 'D:\\x', 'need_gb': 40, 'free_gb': 3}, step='check')
            rep.close()
            events = [json.loads(line) for line in out.getvalue().splitlines()]
            self.assertEqual([e['type'] for e in events], ['step', 'progress', 'progress', 'error'])
            self.assertEqual([e.get('done') for e in events[1:3]], [0, 10])
            self.assertTrue(all('t' in e for e in events))
            log = (Path(d) / 'x.log').read_text(encoding='utf-8')
            self.assertIn('40 GB needed, 3 GB free', log)
            self.assertIn('[check.disk_space]', log)

    def test_progress_lines_are_for_a_launcher_and_not_json(self):
        out = io.StringIO()
        rep = Reporter(out=out, lines=True)
        rep.emit('start', release='1', mode='install', dev=False, dest='x', log='y', steps=['check', 'extract'])
        rep.step('check', 'done', seconds=1)
        rep.progress('extract', 'copy', 5, 10, 'bytes', force=True)
        lines = out.getvalue().splitlines()
        self.assertEqual([line.split()[0] for line in lines], ['progress'] * 3)
        self.assertEqual([line.split()[1] for line in lines], ['0', '0.500', '0.750'])

    def test_progress_lines_name_a_failure_in_words(self):
        out = io.StringIO()
        rep = Reporter(out=out, lines=True)
        rep.error('check.lang_missing', {})
        self.assertTrue(out.getvalue().startswith('error: '))

    def test_catalog_has_every_id_the_code_uses(self):
        catalog = json.loads((ROOT / 'installer' / 'text' / 'en.json').read_text(encoding='utf-8'))
        used = set()
        for path in (ROOT / 'installer').glob('*.py'):
            used |= set(re.findall(r"InstallError\('([a-z_.]+)'", path.read_text()))
            used |= set(re.findall(r"warning\('([a-z_.]+)'", path.read_text()))
            used |= {f'phase.{s}.{p}' for s, p in re.findall(r"progress\('(\w+)', '(\w+)'[,)]", path.read_text())}
        self.assertEqual(sorted(i for i in used if i not in catalog), [])


class KeysTest(unittest.TestCase):
    """#158: a title.keys picked for prod.keys is named as such. The keys here are made up."""

    def inputs_error(self, text):
        from installer import steps
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            for name in ('base.nsp', 'update.nsp'):
                (d / name).write_bytes(b'')
            (d / 'keys.txt').write_text(text, encoding='utf-8')
            options = SimpleNamespace(base=d / 'base.nsp', update=d / 'update.nsp', keys=d / 'keys.txt')
            with self.assertRaises(runner.InstallError) as e:
                steps.Check().inputs(SimpleNamespace(options=options))
            return e.exception.args[0]

    def test_title_keys_is_named(self):
        title = ''.join(f'{i:032x} = {i * 7:032X}\n' for i in range(1, 4))
        self.assertEqual(self.inputs_error(title), 'check.keys_title_keys')
        self.assertEqual(self.inputs_error('\ufeff' + title.replace('\n', '\r\n')), 'check.keys_title_keys')

    def test_other_keys_without_header_key(self):
        self.assertEqual(self.inputs_error('key_area_key_application_00 = ' + '0' * 32 + '\n'),
                         'check.keys_no_header_key')
        self.assertEqual(self.inputs_error(''), 'check.keys_no_header_key')


class CliTest(unittest.TestCase):
    def test_auto_language_keeps_the_one_in_the_ini(self):
        from installer.cli import auto_language
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'xcx.ini').write_text('[language]\nlang = de\n', encoding='utf-8')
            self.assertEqual(auto_language(Path(d), {'lang': 'ja'}), 'de')

    def test_auto_language_falls_back_to_the_recorded_one(self):
        from installer.cli import auto_language
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(auto_language(Path(d), {'lang': 'ja'}), 'ja')

    def test_auto_language_never_keeps_russian(self):
        # issue #58: the game stops before the title in Russian; an older setup wrote lang = ru
        from installer import ini
        from installer.cli import auto_language
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(auto_language(Path(d), {'lang': 'ru'}), 'en-US')
            (Path(d) / 'xcx.ini').write_text('[language]\nlang = ru\n', encoding='utf-8')
            self.assertEqual(auto_language(Path(d), None), 'en-US')
        self.assertNotIn('ru', ini.PLAYABLE)
        self.assertEqual(ini.playable('nl'), 'nl')

    def test_old_chinese_codes_become_the_ones_the_game_knows(self):
        # issue #172: the game stops before the title with zh-CN or zh-TW
        from installer import ini
        from installer.cli import auto_language
        self.assertEqual(ini.playable('zh-CN'), 'zh-Hans')
        self.assertEqual(ini.playable('zh-TW'), 'zh-Hant')
        self.assertEqual(ini.playable('zh-Hans'), 'zh-Hans')
        self.assertNotIn('zh-CN', ini.PLAYABLE)
        self.assertNotIn('zh-TW', ini.PLAYABLE)
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'xcx.ini').write_text('[language]\nlang = zh-CN\n', encoding='utf-8')
            self.assertEqual(auto_language(Path(d), None), 'zh-Hans')

    def test_auto_language_on_a_fresh_folder_is_a_game_language(self):
        from installer import ini
        from installer.cli import auto_language
        with tempfile.TemporaryDirectory() as d:
            self.assertIn(auto_language(Path(d), None), ini.LANGUAGES)


class RecentTest(unittest.TestCase):
    """installer/recent.py: the paths of the last install, filled in by the next setup window (issue #155)."""

    def setUp(self):
        import os
        import shutil
        from unittest import mock
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.file = self.tmp / 'cfg' / 'setup.json'
        patch = mock.patch.dict(os.environ, {'XCX_SETUP_RECENT': str(self.file)})
        patch.start()
        self.addCleanup(patch.stop)
        self.dest = self.tmp / 'XCX'
        self.dest.mkdir()
        self.src = {}
        for name in ('base', 'update', 'dlc', 'keys'):
            f = self.tmp / f'{name}.bin'
            f.write_text('x', encoding='utf-8')
            self.src[name] = str(f)

    def installed(self, paths=None):
        data = state.empty('test')
        if paths is not None:
            data['paths'] = paths
        state.save(self.dest, data)

    def test_default_location_is_per_user(self):
        import os
        from unittest import mock
        from installer import recent
        with mock.patch.dict(os.environ, {'XCX_SETUP_RECENT': '', 'APPDATA': 'C:/Users/p/AppData/Roaming',
                                          'XDG_CONFIG_HOME': '/home/p/.config'}):
            want = (Path('C:/Users/p/AppData/Roaming/XCX Recomp/setup.json') if os.name == 'nt'
                    else Path('/home/p/.config/xcx-recomp/setup.json'))
            self.assertEqual(recent.path(), want)

    def test_nothing_recorded(self):
        from installer import recent
        self.assertEqual(recent.load(), {})

    def test_round_trip(self):
        from installer import recent
        self.installed()
        recent.save(str(self.dest), self.src)
        stored = json.loads(self.file.read_text(encoding='utf-8'))
        self.assertEqual(stored['format'], 1)
        self.assertEqual(recent.load(), {'dest': str(self.dest), **self.src})
        self.assertFalse(self.file.with_name('setup.json.tmp').exists())

    def test_keys_contents_never_stored(self):
        from installer import recent
        Path(self.src['keys']).write_text('header_key = 00112233' + chr(10), encoding='utf-8')
        recent.save(str(self.dest), self.src)
        self.assertNotIn('00112233', self.file.read_text(encoding='utf-8'))

    def test_empty_field_keeps_saved_value(self):
        from installer import recent
        self.installed()
        recent.save(str(self.dest), self.src)
        recent.save(str(self.dest), {'base': '', 'update': '  ', 'dlc': '', 'keys': self.src['keys']})
        self.assertEqual(recent.load()['base'], self.src['base'])

    def test_gone_paths_stay_empty(self):
        import os
        from installer import recent
        self.installed()
        recent.save(str(self.dest), self.src)
        os.remove(self.src['update'])
        loaded = recent.load()
        self.assertNotIn('update', loaded)
        self.assertEqual(loaded['base'], self.src['base'])

    def test_folder_without_state_is_not_filled(self):
        from installer import recent
        recent.save(str(self.dest), self.src)
        loaded = recent.load()
        self.assertNotIn('dest', loaded)
        self.assertEqual(loaded['keys'], self.src['keys'])   # the paths of a run that stopped before its first save

    def test_folder_record_wins(self):
        from installer import recent
        other = self.tmp / 'other-update.nsp'
        other.write_text('y', encoding='utf-8')
        self.installed({'update': str(other), 'dlc': [str(self.tmp / 'gone.nsp'), self.src['dlc']]})
        recent.save(str(self.dest), self.src)
        loaded = recent.load()
        self.assertEqual(loaded['update'], str(other))
        self.assertEqual(loaded['dlc'], self.src['dlc'])
        self.assertEqual(loaded['base'], self.src['base'])   # not in the folder's record: from the per-user file

    def test_from_folder(self):
        from installer import recent
        self.installed({'base': self.src['base'], 'keys': str(self.tmp / 'moved.keys')})
        self.assertEqual(recent.from_folder(str(self.dest)), {'base': self.src['base']})
        self.assertEqual(recent.from_folder(str(self.tmp / 'nothing')), {})

    def test_recorded_shaders(self):   # a rerun of the window keeps the folder's shader choice (gui fill_from_folder)
        from installer import recent
        self.assertIsNone(recent.recorded_shaders(str(self.tmp / 'nothing')))
        self.installed()
        self.assertIsNone(recent.recorded_shaders(str(self.dest)))   # a setup from before the choice
        for value, want in (('off', 'off'), ('now', 'now'), ('later', None), (5, None)):
            data = state.empty('test')
            data['steps']['prepare'] = {'key': 'k', 'shaders': value}
            state.save(self.dest, data)
            self.assertEqual(recent.recorded_shaders(str(self.dest)), want)

    def test_prepare_mode(self):   # --prepare-shaders: now, background or off; anything else is the default
        from installer.steps import Prepare
        for value, want in (('now', 'now'), ('off', 'off'), ('background', 'background'), (None, 'background'),
                            (True, 'background')):
            self.assertEqual(Prepare.mode(SimpleNamespace(options=SimpleNamespace(prepare_shaders=value))), want)
        self.assertEqual(Prepare.mode(SimpleNamespace(options=SimpleNamespace())), 'background')

    def test_prepare_again(self):   # --prepare-again: only with now or background
        from installer.steps import Prepare
        for mode, again, want in (('now', True, True), ('background', True, True), ('off', True, False),
                                  ('now', False, False), ('now', 'yes', False)):
            ctx = SimpleNamespace(options=SimpleNamespace(prepare_shaders=mode, prepare_again=again))
            self.assertEqual(Prepare.again(ctx), want)

    def test_prepared_list_against_the_shader_cache(self):   # a cache deleted by hand means: prepare again
        from installer.steps import PREPARED, prepared_intact, prepared_missing
        dest = self.tmp / 'prepared'
        dest.mkdir()
        self.assertIsNone(prepared_missing(dest))
        self.assertTrue(prepared_intact(dest))   # no list: nothing to check
        ids = [bytes([i]) * 32 for i in (1, 2, 3)]
        recs = b''.join(bytes([1, 1, 0, 0]) + i for i in ids)
        head = b'XCXSPRP' + bytes(1) + (36).to_bytes(4, 'little')
        (dest / PREPARED[0]).write_bytes(head[:8] + (1).to_bytes(4, 'little') + head[8:] + recs)
        self.assertEqual(prepared_missing(dest), (3, 3))   # no shader cache at all
        self.assertFalse(prepared_intact(dest))
        cache_in = dest / 'shader-cache' / 'in'
        cache_in.mkdir(parents=True)
        (cache_in / (ids[0].hex() + '.in')).write_bytes(b'x')
        self.assertEqual(prepared_missing(dest), (3, 2))
        self.assertFalse(prepared_intact(dest))   # most gone
        (cache_in / (ids[1].hex() + '.in')).write_bytes(b'x')
        self.assertEqual(prepared_missing(dest), (3, 1))
        self.assertTrue(prepared_intact(dest))    # one stray missing file: not a reason to prepare everything again
        (dest / PREPARED[0]).write_bytes(head[:8] + (1).to_bytes(4, 'little') + head[8:])
        self.assertFalse(prepared_intact(dest))   # a list without records, as the game's own check
        (dest / PREPARED[0]).write_bytes(head[:8] + (2).to_bytes(4, 'little') + head[8:] + recs)
        self.assertIsNone(prepared_missing(dest))   # another version: unreadable, so not intact
        self.assertFalse(prepared_intact(dest))

    def test_clear_cache_keeps_what_is_not_the_cache(self):   # Rebuild / a new translator (installer/shaders.py)
        from installer import shaders
        h = 'ab' * 32
        dest = self.tmp / 'clear'
        (dest / 'shader-cache' / 'in').mkdir(parents=True)
        (dest / 'shader-cache' / 'anything.txt').write_text('x', encoding='utf-8')
        self.assertEqual(shaders.clear_cache(dest), dest / 'shader-cache')
        self.assertFalse((dest / 'shader-cache').exists())   # the setup's own folder goes whole
        shared = dest / 'shared'
        (shared / 'in').mkdir(parents=True)
        own = [shared / f'{h}.spv', shared / f'{h}.res.json', shared / f'{h}.use', shared / f'{h}.spv.12.34.tmp',
               shared / 'in' / f'{h}.in']
        foreign = [shared / 'notes.txt', shared / 'other.tmp', shared / 'short.spv', shared / 'in' / 'x.in',
                   shared / f'{h}.spv.tmp']
        for f in own + foreign:
            f.write_bytes(b'x')
        (dest / 'xcx.ini').write_text('[performance]\nshader_cache = shared\n', encoding='utf-8')
        self.assertEqual(shaders.clear_cache(dest), shared)
        self.assertEqual([f.name for f in own if f.exists()], [])
        self.assertEqual([f.name for f in foreign if not f.exists()], [])
        (dest / 'keep.txt').write_bytes(b'x')
        (dest / f'{h}.spv').write_bytes(b'x')   # a cache-like name in the install folder itself
        for value in ('.', '..', str(dest), str(dest.parent)):
            (dest / 'xcx.ini').write_text(f'shader_cache = {value}\n', encoding='utf-8')
            self.assertIsNone(shaders.clear_cache(dest), value)
            self.assertTrue((dest / 'keep.txt').exists() and (dest / f'{h}.spv').exists(), value)

    @unittest.skipUnless(sys.platform == 'win32', 'junctions are a Windows thing')
    def test_clear_cache_through_a_junction(self):   # a default cache moved to another drive with mklink /J
        import subprocess
        from installer import shaders
        h = 'ef' * 32
        dest, target = self.tmp / 'junc', self.tmp / 'elsewhere'
        dest.mkdir()
        (target / 'in').mkdir(parents=True)
        for f in (target / f'{h}.spv', target / 'in' / f'{h}.in', target / 'notes.txt'):
            f.write_bytes(b'x')
        subprocess.run(['cmd', '/c', 'mklink', '/J', str(dest / 'shader-cache'), str(target)], check=True,
                       capture_output=True)
        try:
            self.assertEqual(shaders.clear_cache(dest), dest / 'shader-cache')
            self.assertFalse((target / f'{h}.spv').exists() or (target / 'in' / f'{h}.in').exists())
            self.assertTrue((target / 'notes.txt').exists())   # the moved folder keeps what isn't the cache's
            self.assertTrue((dest / 'shader-cache').exists())  # and the junction stays
        finally:
            subprocess.run(['cmd', '/c', 'rmdir', str(dest / 'shader-cache')], capture_output=True)

    def test_ini_value_as_the_game_reads_it(self):
        from installer import shaders
        dest = self.tmp / 'ini2'
        dest.mkdir()
        (dest / 'xcx.ini').write_text('XCX_SHADER_CACHE = "D:/c"\n', encoding='utf-8-sig')   # a BOM, the prefix
        self.assertEqual(shaders.ini_value(dest, 'shader_cache'), 'D:/c')
        (dest / 'xcx.ini').write_text('shader_cache = ' + 'x' * 1100 + '\n', encoding='utf-8')
        self.assertEqual(shaders.shader_cache(dest), dest / 'shader-cache')   # too long for the game: its default

    def test_prepare_meter(self):   # the setup's progress and time left from xcx.exe --prepare-shaders' lines
        from unittest import mock
        from installer import steps
        log = self.tmp / 'prepare.log'
        log.write_bytes(b'[host] other line\nprepare-shaders: progress 0 of 1000\n')
        meter = steps.PrepareMeter(log)
        clock = [100.0]
        with mock.patch.object(steps.time, 'monotonic', lambda: clock[0]):
            self.assertEqual(meter.read(), (0, 1000, None))
            self.assertIsNone(meter.read())   # nothing new
            clock[0] = 105.0
            with open(log, 'ab') as f:
                f.write(b'prepare-shaders: progress 100 of 1000\nprepare-shaders: progr')   # a half-written line
            self.assertEqual(meter.read(), (100, 1000, None))   # under 10 s: no estimate yet
            clock[0] = 120.0
            with open(log, 'ab') as f:
                f.write(b'ess 200 of 1000\n')
            self.assertEqual(meter.read(), (200, 1000, 80.0))   # 200 in 20 s: 800 left take 80 s
        self.assertIsNone(steps.PrepareMeter(self.tmp / 'missing.log').read())

    def test_shader_cache_folder_from_xcx_ini(self):   # the folder the game uses (runtime/cfg.c rules)
        from installer.steps import shader_cache
        dest = self.tmp / 'ini'
        dest.mkdir()
        self.assertEqual(shader_cache(dest), dest / 'shader-cache')
        other = self.tmp / 'elsewhere'
        (dest / 'xcx.ini').write_text('# shader_cache = commented\n[performance]\nSHADER_CACHE = "' + str(other)
                                      + '"\n; shader_cache = also commented\n', encoding='utf-8')
        self.assertEqual(shader_cache(dest), other)
        (dest / 'xcx.ini').write_text('shader_cache = first\nshader_cache = cache2\n', encoding='utf-8')
        self.assertEqual(shader_cache(dest), dest / 'cache2')   # the last line wins; relative to the game folder

    def test_shader_cache_folder_from_the_environment(self):   # XCX_SHADER_CACHE there wins, as in the game (cfg.c)
        import os
        from unittest import mock
        from installer.steps import shader_cache
        dest = self.tmp / 'env'
        dest.mkdir()
        (dest / 'xcx.ini').write_text('shader_cache = from-ini' + chr(10), encoding='utf-8')
        with mock.patch.dict(os.environ, {'XCX_SHADER_CACHE': 'from-env'}):
            self.assertEqual(shader_cache(dest), dest / 'from-env')
        with mock.patch.dict(os.environ, {'XCX_SHADER_CACHE': ''}):   # set but empty: the game's default folder
            self.assertEqual(shader_cache(dest), dest / 'shader-cache')
        with mock.patch.dict(os.environ):
            os.environ.pop('XCX_SHADER_CACHE', None)
            self.assertEqual(shader_cache(dest), dest / 'from-ini')

    def test_translator_identity(self):   # #223: the .id names the translator; a rebuilt exe of it is the same one
        from installer import steps
        a, b = self.tmp / 'a' / steps.XLATE, self.tmp / 'b' / steps.XLATE
        for exe, data in ((a, b'build 1'), (b, b'build 2')):
            exe.parent.mkdir()
            exe.write_bytes(data)
        self.assertNotEqual(steps.translator_identity(a), steps.translator_identity(b))   # no .id: the exe decides
        a.with_suffix('.id').write_text('ab' * 32 + chr(10), encoding='ascii')
        b.with_suffix('.id').write_text('AB' * 32, encoding='ascii')
        self.assertEqual(steps.translator_identity(a), 'ab' * 32)
        self.assertEqual(steps.translator_identity(a), steps.translator_identity(b))
        b.with_suffix('.id').write_text('not an id', encoding='ascii')
        self.assertTrue(steps.translator_identity(b).startswith('exe '))
        self.assertEqual(steps.translator_identity(self.tmp / 'none' / steps.XLATE), '')

    def test_prepare_permission_error(self):   # the game running, or a folder that can't be written
        from unittest import mock
        from installer import steps
        ctx = SimpleNamespace(dest=self.tmp / 'ro')
        for locked, want in ((True, 'compile.exe_in_use'), (False, 'prepare.no_write')):
            with mock.patch.object(steps.Prepare, 'prepare', side_effect=PermissionError(13, 'denied', 'x.bin')),                     mock.patch.object(steps, 'exe_locked', return_value=locked):
                with self.assertRaises(runner.InstallError) as e:
                    steps.Prepare().run(ctx)
            self.assertEqual(e.exception.id, want)
        self.assertFalse(steps.exe_locked(self.tmp / 'missing.exe'))

    def test_bad_values_dropped(self):
        from installer import recent
        self.installed()
        self.file.parent.mkdir(parents=True)
        for record in ({'dest': str(self.dest), 'base': 5, 'update': 'rel/x.nsp', 'keys': [1], 'dlc': None},
                       {'dest': str(self.dest), 'base': self.src['base'] + chr(0) + 'x', 'keys': str(self.tmp)}):
            self.file.write_text(json.dumps(record), encoding='utf-8')
            self.assertEqual(recent.load(), {'dest': str(self.dest)})
        for text in ('', 'not json', '[1, 2]', '{"dest": "relative"}'):
            self.file.write_text(text, encoding='utf-8')
            self.assertEqual(recent.load(), {}, text)

    def test_blocked_load_fills_nothing(self):
        import time
        from unittest import mock
        from installer import recent
        with mock.patch.object(recent, 'load', side_effect=lambda: time.sleep(3) or {'dest': 'x'}):
            t0 = time.monotonic()
            self.assertEqual(recent.load_bounded(0.2), {})
            self.assertLess(time.monotonic() - t0, 2)
        with mock.patch.object(recent, 'load', return_value={'dest': 'x'}):
            self.assertEqual(recent.load_bounded(), {'dest': 'x'})

    def test_merge_paths(self):
        from installer import recent
        given = SimpleNamespace(game=None, base=Path(self.src['base']), update=Path(self.src['update']),
                                dlc=[Path(self.src['dlc'])], keys=Path(self.src['keys']))
        paths = recent.merge_paths(None, given)
        self.assertEqual(paths, {**self.src, 'dlc': [self.src['dlc']]})
        # an update given only --dest keeps them; a file that does not exist is not recorded
        only_dest = SimpleNamespace(game=None, base=None, update=self.tmp / 'gone.nsp', dlc=None, keys=None)
        self.assertEqual(recent.merge_paths(paths, only_dest), paths)
        folder = SimpleNamespace(game=self.tmp, base=None, update=None, dlc=None, keys=None)
        self.assertEqual(recent.merge_paths(paths, folder)['base'], str(self.tmp))


if __name__ == '__main__':
    unittest.main()
