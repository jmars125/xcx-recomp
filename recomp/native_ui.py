"""Validated native UI hook points for the inspected XCX DE 2.0.0 image.

Each hook is one guest function entry that the generator wraps with `if (rt_native_ui_hook(id, c)) return;`. The runtime
answers a hook only when its caller matches (`caller` is the return address inside the one guarded call), so a hook never
changes any other use of the same function. `validate` checks the pinned executable and the original instruction words
at every entry and at every guarded `bl`, like recomp/mod_menu.py does for the Online row.

The runtime side is runtime/native_ui_adapter.c; the mod-facing API is documented in docs/mods/05-native-ui-hooks.md.
"""
import hashlib

BUILD_ID = 'ec4ed46bbc4a9ebbe7ce0af7a4a8ebbc09e5724a'
NSO_SHA256 = '0a9dbf1f3816dd0fab0a08883d538c74d9b9fd7d939cb29aaf0947d71a201026'
BASE = 0x7000000000

# (id, name, function entry, return address of the guarded call), offsets are main-relative.
HOOKS = (
    (0, 'connect.story', 0xc89f0, 0x8383e8),    # Connect entry's story condition 0x2280
    (1, 'connect.online', 0x4e3ae4, 0x78b8e4),  # the connect state machine's online query (state 14)
    (2, 'connect.flag', 0x54ace0, 0x78bab4),    # the connect state machine's named-flag check (state 18)
    (3, 'squad.idle_online', 0xc8860, 0x71632c),  # the squad screen view's idle connection query
    (4, 'squad.input', 0x717460, 0x716338),     # the squad screen view's idle input handler
    (5, 'squad.state', 0x716370, 0x71631c),     # the squad screen view's state handler (dialog answers and waits)
    (6, 'squad.message', 0x9262a0, 0x716cb8),   # the message post inside the create-failure state (0x17)
    # The squad details screen (opened by the adapter after a mod reports success). Each hook answers only while the
    # adapter has opened that screen, and only at its one guarded call site.
    (7, 'details.online', 0xc8960, 0x715c84),   # the Details update's "still online?" query; offline it would raise 0x15bb and close
    (8, 'details.timer', 0x715e84, 0x715ca4),   # the Details update's countdown refresh, which reads the online session manager
    (9, 'details.close', 0x8f0218, 0x715d78),   # the Details screen closing after a button press (logged, never answered)
    # The game's "online state ready" query (the online parameter object's state), asked by each main-menu Online row's validator
    # right after the squad-session query. Answered for every caller while a mod reports a session; `caller` is one of its sites.
    (10, 'online.ready', 0xc8908, 0x838448),
)
NAMES = {name: hook_id for hook_id, name, _, _ in HOOKS}
ENTRIES = {BASE + entry: hook_id for hook_id, _, entry, _ in HOOKS}

# The online query (hook 1) is also called by the squad screen input handler for every option; that second call site
# (return address) is answered for the create and join card options only, so the game's own confirmation
# dialog appears. The runtime tells the two call sites apart by the caller.
SQUAD_INPUT_ONLINE = 0x7174a4

# Original instruction words: every hooked function entry, the guarded bl before each return address, and the guest
# functions the squad screen input gate calls (the pad poll, the button animation, the dialog answer queries).
WORDS = {
    0xc89f0: 0xd10203ff, 0x4e3ae4: 0xa9be7bfd, 0x54ace0: 0xd100c3ff, 0xc8860: 0xa9be7bfd, 0x717460: 0xa9ba7bfd,
    0x40b93c: 0xa9bd7bfd, 0x55faf4: 0xa9bd7bfd, 0x716370: 0xa9ba7bfd,
    0x8383e4: 0x97e24183, 0x78b8e0: 0x97f56081, 0x78bab0: 0x97f6fc8c, 0x716328: 0x97e6c94e, 0x716334: 0x9400044b, 0x716318: 0x94000016,
    0x7174a0: 0x97f73191, 0x9262a0: 0xa9be7bfd, 0x716cb4: 0x94083d7b,
    0xc8960: 0x9000ea28, 0x715e84: 0xd101c3ff, 0x8f0218: 0xa9be7bfd,
    0x715c80: 0x97e6cb38, 0x715ca0: 0x94000079, 0x715d74: 0x94076929,
    0xc8908: 0xa9be7bfd, 0x838444: 0x97e24131,
}


def validate(mod, raw):
    if mod.name != 'main' or mod.base != BASE or mod.nso.build_id != BUILD_ID:
        raise ValueError('Native UI hooks: unsupported module, layout or build ID')
    if hashlib.sha256(raw).hexdigest() != NSO_SHA256:
        raise ValueError('Native UI hooks: unsupported main NSO hash')
    for off, word in WORDS.items():
        if mod.word(BASE + off) != word:
            raise ValueError(f'Native UI hooks: original instruction mismatch at main+{off:#x}')


def validate_patches(table):
    """A code patch inside a hooked function's first instruction would change what the hook was checked against."""
    for off in table.sites.get('main', {}):
        if off in WORDS or any(entry <= off < entry + 4 for _, _, entry, _ in HOOKS):
            raise ValueError('Native UI hooks: overlapping instruction patch')


def entry_hook(entry):
    """The line the generator adds right after FN_ENTER of a hooked function, or None."""
    hook_id = ENTRIES.get(entry)
    return None if hook_id is None else f'if (rt_native_ui_hook({hook_id}, c)) return;'
