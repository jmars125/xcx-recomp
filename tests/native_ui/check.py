"""Native Online-screen hooks: validation, the guest-side adapter fixture and the Lua registration, no game window.

    python tests/native_ui/check.py [--sanitize]

Needs clang and build/lua-src (python -c "import sys; sys.path.insert(0,'tools'); import prepare_lua; prepare_lua.prepare()").
"""
from pathlib import Path
import argparse, os, subprocess, sys
ROOT = Path(__file__).resolve().parents[2]
WIN = sys.platform == 'win32'; EXE = '.exe' if WIN else ''
p = argparse.ArgumentParser(); p.add_argument('--sanitize', action='store_true'); a = p.parse_args()
out = ROOT / 'build/native_ui' / ('sanitize' if a.sanitize else 'regular'); out.mkdir(parents=True, exist_ok=True)
# The Lua driver is the shared actions driver plus the registry switches and a "native=<option>" step that confirms a
# Squad screen option the way the guest adapter does, with no overlay session.
s = (ROOT / 'tests/actions/lua_driver.c').read_text()
s = s.replace('#include "mod_internal.h"', '#include "mod_internal.h"\n#include "mod_native_ui.h"\nstatic char nt_txt[32];')
s = s.replace('mod_ui_reset(argv[2]);', 'mod_ui_reset(argv[2]);mod_native_ui_reset();mod_native_ui_support(!getenv("NATIVE_TEST_UNSUPPORTED"));')
s = s.replace('else if(sscanf(argv[i],"pump=%u",&ms)==1)', 'else if(sscanf(argv[i],"nativetext=%u:%15s",&ms,nt_txt)==2)printf("native %u %d\\n",ms,mod_native_ui_confirm_text(ms,nt_txt));else if(sscanf(argv[i],"native=%u",&ms)==1)printf("native %u %d\\n",ms,mod_native_ui_confirm(ms));else if(sscanf(argv[i],"pump=%u",&ms)==1)')
s = s.replace('mod_ui_shutdown();mod_runtime_stop(rt);', 'printf("features %u\\n",mod_native_ui_features());mod_ui_shutdown();mod_runtime_stop(rt);')
s = s.replace('ModRuntime *rt=', 'snprintf(plan->game_id,sizeof plan->game_id,"2.0.0");ModRuntime *rt=')
(out / 'driver.c').write_text(s)
flags = ['-O1', '-g', '-std=gnu11', '-DXCX_MOD_TESTING', '-Iruntime', '-Ibuild/lua-src']
if a.sanitize: flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
lua = ROOT / 'build/lua-src'
core = [x for x in sorted(lua.glob('*.c')) if x.name not in ('lua.c', 'luac.c', 'linit.c') and (not x.name.endswith('lib.c') or x.name in ('lauxlib.c', 'lbaselib.c', 'lstrlib.c', 'ltablib.c', 'lmathlib.c'))]
source = [ROOT / 'runtime' / n for n in ['mod_menu.c', 'mod_native_ui.c', 'mod_runtime.c', 'mod_provider.c', 'mod_provider_platform.c', 'mod_package.c', 'mod_settings.c', 'sha256.c']]
env = os.environ.copy(); env.update(ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1', NATIVE_TEST_EXE=str(out / ('host' + EXE)))
io = ROOT / 'tests/mods/settings_io.c'
if WIN:
    io = out / 'io.c'; io.write_text('#include <stddef.h>\nvoid plat_file_mkdir_p(const char*p){(void)p;} int plat_file_write_atomic(const char*p,const void*d,size_t n){(void)p;(void)d;(void)n;return 0;}\n')
libs = [] if WIN else ['-ldl', '-lm']
subprocess.run(['clang', *flags, out / 'driver.c', *source, io, *core, *libs, '-o', out / ('host' + EXE)], cwd=ROOT, env=env, check=True)
# Guest-side adapter: the real registry, intent queue and hook code, a scripted pad, no Lua.
(out / 'native_ui_config.h').write_text('#define NATIVE_UI_HOOKS 1\n')
subprocess.run(['clang', *flags, '-I' + str(out), ROOT / 'tests/native_ui/adapter.c', ROOT / 'runtime/native_ui_adapter.c', ROOT / 'runtime/mod_native_ui.c',
                ROOT / 'runtime/mod_settings.c', io, '-o', out / ('adapter' + EXE), *libs], cwd=ROOT, env=env, check=True)
subprocess.run([out / ('adapter' + EXE)], cwd=ROOT, env=env, check=True)
# The message table text edits; XCX_MESSAGE_TABLE=<the player's us-common_ms.bdat> also checks them on the real table.
subprocess.run(['clang', *flags, ROOT / 'tests/native_ui/message_text_test.c', '-o', out / ('message_text' + EXE)], cwd=ROOT, env=env, check=True)
subprocess.run([out / ('message_text' + EXE)], cwd=ROOT, env=env, check=True)
subprocess.run([sys.executable, '-B', '-m', 'unittest', 'discover', '-s', 'tests/native_ui', '-p', 'test_*.py', '-v'], cwd=ROOT, env=env, check=True)
