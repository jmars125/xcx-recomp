"""host.native_ui.register through the production Lua runtime, and the native intent a registered option runs."""
from pathlib import Path
import os, subprocess, tempfile, unittest

REGISTER = ('host.native_ui.register{version=1,online_entry=true,squad_select={create_private="host",play_private="join"}}')

class Registration(unittest.TestCase):
    def run_mod(self, code, steps=(), unsupported=False, event='', second=''):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); mods = root / 'mods'; settings = root / 'settings'; mods.mkdir(); settings.mkdir()
            (mods / 'enabled.txt').write_text('alpha\n' + ('beta\n' if second else ''))
            for owner, extra in [('alpha', code), ('beta', second)]:
                if owner == 'beta' and not second: continue
                p = mods / owner; p.mkdir()
                (p / 'mod.ini').write_text(f'id={owner}\nname={owner}\nversion=0.1.0\napi=1\nlanguage=lua55-basic-v1\nrequires=\n')
                (p / 'main.lua').write_text(
                    "return {init=function() host.settings.register{version=1,id='page',title='Page',fields={{id='flag',label='Flag',type='bool',default=false}},"
                    "actions={{id='host',label='Host'},{id='join',label='Join'}}};" + extra + " end,"
                    "events={['host.started']=function() " + event + " end,['ui.action']=function(e) host.ui.status('ran '..e.action..(e.text and (' with '..e.text) or '')) end},shutdown=function() end}")
            env = os.environ.copy()
            if unsupported: env['NATIVE_TEST_UNSUPPORTED'] = '1'
            r = subprocess.run([os.environ['NATIVE_TEST_EXE'], str(mods), str(settings), *steps], env=env, capture_output=True, text=True, timeout=6)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr); self.assertEqual(r.stderr, '')
            self.assertIn('alpha closed bytes=0 callbacks=0', r.stdout)
            return r.stdout
    def test_registration_runs_option_action_without_overlay(self):
        out = self.run_mod('assert(' + REGISTER + ')', steps=('close', 'native=2', 'pump=60', 'native=3', 'pump=60'))
        self.assertIn('native 2 1', out); self.assertIn('native 3 1', out); self.assertIn('features 3', out)
        self.assertIn('text=ran join', out); self.assertNotIn(' error ', out)
    def test_unbound_options_are_refused(self):
        out = self.run_mod('assert(' + REGISTER + ')', steps=('close', 'native=0', 'native=1', 'native=4', 'pump=40'))
        self.assertIn('native 0 0', out); self.assertIn('native 1 0', out); self.assertIn('native 4 0', out); self.assertNotIn('ran ', out)
    def test_online_entry_only(self):
        out = self.run_mod('assert(host.native_ui.register{version=1,online_entry=true})', steps=('native=2',))
        self.assertIn('features 1', out); self.assertIn('native 2 0', out)
    def test_unknown_version_and_empty_request(self):
        out = self.run_mod('local ok,why=host.native_ui.register{version=2,online_entry=true};assert(not ok and why=="unsupported")')
        self.assertIn('features 0', out)
        out = self.run_mod('host.native_ui.register{version=1}'); self.assertIn('alpha error init', out); self.assertIn('features 0', out)
    def test_invalid_values(self):
        for call in ['online_entry="yes"', 'squad_select=3', 'squad_select={play_private=3}', 'squad_select={play_private=""}',
                     'squad_select={play_private="' + 'x' * 65 + '"}', 'squad_select={play_private="a\\0b"}']:
            out = self.run_mod('host.native_ui.register{version=1,' + call + '}')
            self.assertIn('alpha error init', out, call); self.assertIn('features 0', out, call)
    def test_unsupported_build(self):
        out = self.run_mod('local ok,why=' + REGISTER + ';assert(not ok and why=="unsupported")', unsupported=True)
        self.assertNotIn(' error ', out); self.assertIn('features 0', out)
    def test_only_one_owner(self):
        out = self.run_mod('assert(' + REGISTER + ')', second='local ok,why=' + REGISTER + ';assert(not ok and why=="full")')
        self.assertNotIn(' error ', out); self.assertIn('features 3', out)
    def test_registration_after_init_unavailable(self):
        out = self.run_mod('', event='local ok,why=' + REGISTER + ';assert(not ok and why=="unavailable")')
        self.assertNotIn(' error ', out); self.assertIn('features 0', out)
    def test_faulting_owner_gets_no_actions(self):
        out = self.run_mod('assert(' + REGISTER + ')', event='error("owner failed")', steps=('close', 'pump=40', 'native=2', 'pump=40'))
        self.assertIn('native 2 0', out); self.assertNotIn('ran ', out)
    def test_documented_snippet_registers(self):
        import re, shutil
        root = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp); mods = t / 'mods'; shutil.copytree(root / 'examples/mods/hello', mods / 'hello'); (mods / 'enabled.txt').write_text('hello\n'); settings = t / 'settings'; settings.mkdir()
            snippet = re.search(r'```lua\n(.*?)```', (root / 'docs/mods/05-native-ui-hooks.md').read_text(), re.S)[1]
            source = mods / 'hello/main.lua'; source.write_text(source.read_text().replace("host.ui.status('Choose Say hello.')", snippet + "\n  host.ui.status('Choose Say hello.')"))
            r = subprocess.run([os.environ['NATIVE_TEST_EXE'], str(mods), str(settings)], capture_output=True, text=True, timeout=6)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr); self.assertEqual(r.stderr, ''); self.assertNotIn(' error ', r.stdout); self.assertIn('features 3', r.stdout)
    def test_owner_reports_the_session_answer(self):
        out = self.run_mod('assert(' + REGISTER + ')', event='assert(host.native_ui.report("ok")==true);assert(host.native_ui.report("failed")==true)')
        self.assertNotIn(' error ', out)
    def test_report_refuses_other_mods(self):
        out = self.run_mod('assert(' + REGISTER + ')', second='assert(host.native_ui.report("ok")==false)')
        self.assertNotIn(' error ', out)
        out = self.run_mod('assert(host.native_ui.report("ok")==false)')  # no registration: nothing to answer
        self.assertNotIn(' error ', out)
    def test_report_rejects_bad_values(self):
        for value in ('"maybe"', '3', 'nil'):
            out = self.run_mod('assert(' + REGISTER + ')', event='host.native_ui.report(' + value + ')')
            self.assertIn('alpha error event', out, value)
        # A failure may carry the text of the message window: 1 to 75 printable ASCII characters (a newline is allowed), only with "failed".
        for call in ('report("failed", "")', 'report("ok", "text")', 'report("failed", 7)', 'report("failed", "bad\\1text")',
                     'report("failed", "' + 'a' * 76 + '")'):
            out = self.run_mod('assert(' + REGISTER + ')', event='host.native_ui.' + call)
            self.assertIn('alpha error event', out, call)
        for call in ('report("failed", "Second test text.")', 'report("failed", "Line one\\nLine two")', 'report("failed", "' + 'a' * 75 + '")'):
            out = self.run_mod('assert(' + REGISTER + ')', event='host.native_ui.' + call)
            self.assertNotIn('alpha error event', out, call)
    def test_native_intent_carries_the_typed_text(self):
        # The Join passcode typed on the game's keyboard reaches the mapped action as the event field `text`; other intents have none.
        out = self.run_mod('assert(' + REGISTER + ')', steps=('close', 'nativetext=3:27015', 'pump=60'))
        self.assertIn('native 3 1', out); self.assertIn('text=ran join with 27015', out); self.assertNotIn(' error ', out)
        out = self.run_mod('assert(' + REGISTER + ')', steps=('close', 'native=3', 'pump=60'))
        self.assertIn('text=ran join', out); self.assertNotIn(' with ', out)
        out = self.run_mod('assert(' + REGISTER + ')', steps=('close', 'nativetext=3:a-B9', 'pump=60', 'native=2', 'pump=60'))
        self.assertIn('text=ran host', out); self.assertNotIn('host with', out)   # the text belongs to one intent
    def test_details_opt_in_names_the_button_actions(self):
        # details={start=?, leave=?}: registering the table opts in to the squad details screen; its buttons run the named actions.
        both = 'host.native_ui.register{version=1,online_entry=true,squad_select={create_private="host"},details={start="host",leave="join"}}'
        out = self.run_mod('assert(' + both + ')', steps=('close', 'native=4', 'pump=60', 'native=5', 'pump=60'))
        self.assertIn('features 7', out); self.assertIn('native 4 1', out); self.assertIn('native 5 1', out); self.assertIn('text=ran join', out)
        out = self.run_mod('assert(host.native_ui.register{version=1,details={}})', steps=('native=4', 'native=5', 'native=2'))
        self.assertIn('features 4', out); self.assertIn('native 4 0', out); self.assertIn('native 5 0', out); self.assertIn('native 2 0', out)  # no action named, no squad screen
        out = self.run_mod('assert(' + REGISTER + ')', steps=('native=4', 'native=5'))   # not opted in: the Details buttons are refused
        self.assertIn('native 4 0', out); self.assertIn('native 5 0', out)
        for call in ('details=3', 'details={start=3}', 'details={leave=""}', 'details={start="' + 'x' * 65 + '"}', 'details={leave="a\\0b"}'):
            out = self.run_mod('host.native_ui.register{version=1,online_entry=true,' + call + '}')
            self.assertIn('alpha error init', out, call); self.assertIn('features 0', out, call)
    def test_report_ok_carries_passcode_and_mode(self):
        both = 'assert(host.native_ui.register{version=1,details={start="host"}})'
        for call in ('report("ok", {passcode="27015"})', 'report("ok", {passcode="AB-12", mode="created"})', 'report("ok", {mode="joined"})',
                     'report("ok", {mode="in_squad"})', 'report("ok", {})', 'report("ok", {passcode="' + 'a' * 15 + '"})',
                     'report("ok", {passcode="27015", label="Players:", status="1 / 4"})', 'report("ok", {label="' + 'a' * 13 + '", status="' + 'b' * 8 + '"})'):
            out = self.run_mod(both, event='assert(host.native_ui.' + call + '==true)')
            self.assertNotIn('alpha error event', out, call)
        for call in ('report("ok", {passcode=""})', 'report("ok", {passcode="bad code"})', 'report("ok", {passcode="' + 'a' * 16 + '"})',
                     'report("ok", {passcode=7})', 'report("ok", {mode="nope"})', 'report("ok", {mode=1})', 'report("failed", {passcode="1"})',
                     'report("ok", {label=""})', 'report("ok", {label="' + 'a' * 14 + '"})', 'report("ok", {label=7})', 'report("ok", {status=""})',
                     'report("ok", {status="' + 'b' * 9 + '"})', 'report("ok", {status=7})', 'report("ok", {status="bad\\1"})'):
            out = self.run_mod(both, event='host.native_ui.' + call)
            self.assertIn('alpha error event', out, call)
        # Without the opt-in the details are refused (false, not an error), and another mod gets false too.
        out = self.run_mod('assert(' + REGISTER + ')', event='assert(host.native_ui.report("ok", {passcode="27015"})==false)')
        self.assertNotIn('alpha error event', out)
        out = self.run_mod(both, second='assert(host.native_ui.report("ok", {passcode="27015"})==false)')
        self.assertNotIn(' error ', out)
    def test_details_status_changes_the_timer_text(self):
        both = 'assert(host.native_ui.register{version=1,details={start="host"}})'
        for call in ('details_status("2 / 4")', 'details_status("1")', 'details_status("' + 'a' * 8 + '")'):
            out = self.run_mod(both, event='assert(host.native_ui.' + call + '==true)')
            self.assertNotIn('alpha error event', out, call)
        for call in ('details_status("")', 'details_status(7)', 'details_status("' + 'a' * 9 + '")', 'details_status("bad\\1")', 'details_status()'):
            out = self.run_mod(both, event='host.native_ui.' + call)
            self.assertIn('alpha error event', out, call)
        out = self.run_mod('assert(' + REGISTER + ')', event='assert(host.native_ui.details_status("2 / 4")==false)')   # no opt-in
        self.assertNotIn('alpha error event', out)
        out = self.run_mod(both, second='assert(host.native_ui.details_status("2 / 4")==false)')                         # another mod
        self.assertNotIn(' error ', out)
    def test_set_session_tells_the_host_a_session_is_up(self):
        both = 'assert(host.native_ui.register{version=1,details={start="host"}})'
        for call in ('set_session(true)', 'set_session(false)'):
            out = self.run_mod(both, event='assert(host.native_ui.' + call + '==true)')
            self.assertNotIn('alpha error event', out, call)
        for call in ('set_session(1)', 'set_session("yes")', 'set_session()'):
            out = self.run_mod(both, event='host.native_ui.' + call)
            self.assertIn('alpha error event', out, call)
        out = self.run_mod('assert(' + REGISTER + ')', event='assert(host.native_ui.set_session(true)==false)')   # no opt-in
        self.assertNotIn('alpha error event', out)
        out = self.run_mod(both, second='assert(host.native_ui.set_session(true)==false)')                         # another mod
        self.assertNotIn(' error ', out)
    def test_toast_validates_text_and_owner(self):
        # host.native_ui.toast(text): 1 to 46 printable ASCII characters on one line; a bad text is a Lua error.
        for call in ('toast("")', 'toast(7)', 'toast("two\\nlines")', 'toast("bad\\1text")', 'toast("' + 'a' * 47 + '")'):
            out = self.run_mod('assert(' + REGISTER + ')', event='host.native_ui.' + call)
            self.assertIn('alpha error event', out, call)
        for call in ('toast("Starting a squad...")', 'toast("[ST:col p1=red ]Oops")', 'toast("' + 'a' * 46 + '")'):
            out = self.run_mod('assert(' + REGISTER + ')', event='host.native_ui.' + call)
            self.assertNotIn('alpha error event', out, call)
    def test_cancellation_withdraws(self):
        out = self.run_mod('assert(' + REGISTER + ')', steps=('cancel', 'native=2'))
        self.assertNotIn('ran ', out)

if __name__ == '__main__': unittest.main()
