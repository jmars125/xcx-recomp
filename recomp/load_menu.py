"""Verified XCX DE 2.0.0 sites for the in-game main menu's Data row, which holds Save and Load.

The menu's rows are game data: a table of 0x30-byte entries, each with a member-function-pointer handler and an optional
list of child rows (docs/menu-load-game.md). The three hooks are entry hooks, so they only read and write memory and call
the game's own functions through rt_call. Another build gets no hooks and keeps the game's menu as it is.
"""

BUILD = 'ec4ed46bbc4a9ebbe7ce0af7a4a8ebbc09e5724a'
SHA256 = '0a9dbf1f3816dd0fab0a08883d538c74d9b9fd7d939cb29aaf0947d71a201026'

# The menu's per-frame state update; its first call each frame sees the row table before the list is built.
UPDATE = 0x839910
# The Save row's handler. It answers one of eight questions about its row (the op in w1), op 6 being "choose it".
SAVE_HANDLER = 0x838fd8
# A parent row's handler (answers "yes" to every op but 3); also the Intel row's.
PARENT_HANDLER = 0x838fc0
# The menu's label setter (view, level, row, {text table, message id}); the mod menu hooks the same function.
LABEL = 0x66bee8
HOOKS = {UPDATE: 'rt_load_menu_update(c->x[0]);', SAVE_HANDLER: 'if (rt_load_menu_handler(c)) return;',
         LABEL: 'if (rt_load_menu_label(c)) return;'}
WORDS = {UPDATE: 0xa9bd7bfd, SAVE_HANDLER: 0xa9be7bfd, PARENT_HANDLER: 0x71000c3f, LABEL: 0xa9bd7bfd}


def verified(mod):
    """True for the pinned 2.0.0 main, False for any other module or build; a pinned build whose words differ is an error."""
    if mod.name != 'main' or mod.nso.build_id != BUILD:
        return False
    if mod.nso.source_sha256 != SHA256 or mod.base != 0x7000000000:
        raise ValueError('Load menu: build identity mismatch')
    for offset, word in WORDS.items():
        if mod.word(mod.base + offset) != word:
            raise ValueError(f'Load menu: instruction mismatch at main+{offset:#x}')
    return True


def entry(mod, address):
    return HOOKS.get(address - mod.base, '')


def validate_patches(table):
    """An instruction patch on a hooked function's first instruction would replace what the hook sits in front of."""
    for off in table.sites.get('main', {}):
        if off in HOOKS:
            raise ValueError(f'Load menu: patch at the hooked entry main+{off:#x}')
