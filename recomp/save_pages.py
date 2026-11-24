"""Hook sites for save slot pages (runtime/save_pages.c) in the Save/Load screen of XCX DE 2.0.0.

The screen's update function (main+0x6660a0) reads the buttons that went down this frame from the game's pad layer and
merges them into one mask (main+0x6661bc). Its next instruction (main+0x6661c0) starts the cursor tests, so a call
there sees the mask in W8 and the screen object in X19, on the path that only runs while the screen is idle: its
rows are loaded and no message is open. Every word below is checked against the pinned main.
"""

BUILD = 'ec4ed46bbc4a9ebbe7ce0af7a4a8ebbc09e5724a'
SHA256 = '0a9dbf1f3816dd0fab0a08883d538c74d9b9fd7d939cb29aaf0947d71a201026'
PAD_SITE = 0x6661c0   # mov w9, #0x1000: the first instruction after `orr w8, w0, w20` (0x6661bc)
ROWS_ENTRY = 0x6665c0   # the function that fills the rows' widgets from their loaded data; X0 is the screen object
OPEN_ENTRY = 0x6654a0   # the screen's constructor; X0 is the new screen object
UPDATE_ENTRY = 0x6660a0   # the screen's update, once per frame; X0 is the screen object
TEXT_ENTRY = 0x916d44   # the text widget's string setter every label goes through: X0 widget, X1 UTF-8 string
LABEL_ENTRY = 0x55f548   # sets the text of the child named X1 of the layout handle X0 from a message (the screen frame's title goes through it)
REQUEST_ENTRY = 0x928620   # asks for the Save/Load screen: X1 is the mode, 1 for Save and 0 for Load
WORDS = {REQUEST_ENTRY: 0xd10143ff, 0x55f548: 0xd10103ff, 0x6661bc: 0x2a140008, PAD_SITE: 0x52820009, ROWS_ENTRY: 0xd10703ff, OPEN_ENTRY: 0xd10243ff, TEXT_ENTRY: 0xd101c3ff,
         UPDATE_ENTRY: 0xa9bc7bfd}


def verified(mod):
    if mod.name != 'main' or mod.nso.build_id != BUILD:
        return False
    if mod.nso.source_sha256 != SHA256 or mod.base != 0x7000000000:
        raise ValueError('Save pages: build identity mismatch')
    for offset, word in WORDS.items():
        if mod.word(mod.base + offset) != word:
            raise ValueError(f'Save pages: instruction mismatch at main+{offset:#x}')
    return True


def site(mod, address):
    """C emitted before the instruction at address; reads registers through c->x like the other hooks."""
    if address - mod.base == PAD_SITE:
        return 'rt_save_pages_pad(c->x[19], (uint32_t)c->x[8]);'
    return ''


def entry(mod, address):
    """C emitted at the start of the function at address, after the entry profiling hook; X0..X7 are the arguments."""
    offset = address - mod.base
    if offset == ROWS_ENTRY:
        return 'rt_save_pages_rows(c);'
    if offset == UPDATE_ENTRY:
        return 'rt_save_pages_update(c);'
    if offset == OPEN_ENTRY:
        return 'rt_save_pages_open();'
    if offset == TEXT_ENTRY:
        return 'rt_save_pages_text(c);'
    if offset == LABEL_ENTRY:
        return 'rt_save_pages_label_entry(c);'
    if offset == REQUEST_ENTRY:
        return 'rt_save_pages_request((uint32_t)c->x[1]);'
    return ''
