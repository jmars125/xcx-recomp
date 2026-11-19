# 05 · Native Online screens v1

A loaded Lua mod can ask the game to reuse two of its own Online screens as the front end of the mod: the **connect entry** in the Online menu, and the four-card **squad screen** behind it. The game draws, animates and navigates the screens; the mod decides what each choice does. This page is the whole contract; the mod never touches game memory.

The adapter supports only XCX DE 2.0.0 with checked generation enabled (`--native-ui-hook`; the installer passes it). On any other build, and on a 2.0.0 build lifted without the flag, `register` returns `nil,'unsupported'` and the game behaves exactly as before.

**No game text lives in this repository.** The docs, comments and tests describe the game's windows in plain words instead of quoting them, and the code finds the few game rows it borrows by table shape, row ID, length and hash (see "How the borrowed rows are found" below). Please keep it that way: describe a window, do not quote it.

## Register during init

Call after `host.settings.register`, from `init` (or the startup table the host passes to it):

```lua
local ok,reason=host.native_ui.register {
 version=1,
 online_entry=true,
 squad_select={
  create_private='host',
  play_private='join'
 }
}
if not ok then host.log('Native Online screens unavailable: '..reason) end
```

| Field | Meaning |
| --- | --- |
| `version` | Must be `1`. Anything else returns `nil,'unsupported'`. |
| `online_entry` | `true` lets the connect entry be selected before the story would normally allow it. |
| `squad_select` | Maps the squad screen's four cards to the id of a **button of your own page** (the `actions` you gave `host.settings.register`). Keys, in card order: `quick_assignment`, `play_with_friends`, `create_private`, `play_private`. An unmapped first or second card shows the game's own "not available offline" toast; an unmapped create or join card stays inert. |

At least one feature must be requested. A successful call returns `true`; expected rejection returns `nil,reason`:

| Reason | Meaning |
| --- | --- |
| `unsupported` | Unknown version, or an executable without the checked hooks |
| `full` | Another mod already holds the single native-screen registration |
| `unavailable` | Called outside init, or the host is shutting down |

Malformed values (non-boolean `online_entry`, a non-table `squad_select`, an action that is empty, longer than 64 bytes, not a string, or contains a NUL) raise a protected Lua error and fault the owner. There is one registration per process, no unregister, and it is claimed first come, first served, like the menu row.

## What happens when the player confirms a card

**Create card and join card.** The game raises its own confirmation window first (create asks to confirm making a private squad; join asks to confirm joining with a passcode). Cancel or Back closes it and nothing happens. Confirm queues the mapped button as an ordinary `ui.action` for your mod, exactly like pressing it on the Mods page, **without opening the Mods overlay**, and the squad screen waits for your answer for up to 20 seconds. The action runs on the mod worker; Lua never runs inside the game's UI thread. Answer with `host.native_ui.report`:

```lua
host.native_ui.report('ok')                          -- the screen returns to the four cards
host.native_ui.report('failed')                      -- the game shows its own failure window
host.native_ui.report('failed', 'Could not reach the host.')  -- the same window with your own words
```

A plain `failed` shows the game's own failure text for that card (create has one, join has another), each in the game's own message window. **A failure may carry the text to show:** 1 to 75 printable ASCII characters (a newline is allowed), valid only with `failed`; anything else raises a Lua error. The window is still the game's own (its frame, fonts and buttons); only the words come from the mod. The game has no way to take free text in that window, so the runtime keeps a small pool of three spare rows of the main message table (friends-list notices that no native screen of ours shows: ids `0x12ea`, `0x12eb`, `0x12ec`) and writes the text into one of them in the copy of the table the game holds in memory, in place and zero-filled, then lets the window show that row (`runtime/message_text.h`, `custom_message_id` in `runtime/native_ui_adapter.c`). Nothing is written to disk. The same text reuses its row, a new text takes the least recently used one, so up to three different texts are available at the same time and an older one is replaced when a fourth is used. The rows are looked for when the player confirms create or join (the first search can take a moment); if they are not found (another language, a modified table; at most four searches are made in a session) the game's own text is shown instead. Answering the window returns to the four cards (the game's own offline-fallback failure window is not used, because it leaves the squad screen). No answer in time is treated as `failed`. `report` returns `true` when it was accepted and `false` for any mod that does not hold the registration, or when the text is not valid for the status; a mod that faulted or stopped receives nothing and the screen goes back to the four cards.

**A toast with your own words.** While the native squad screen is open, `host.native_ui.toast(text)` shows the game's own toast (the red "not available offline" one) with the mod's text, for example while the action runs:

```lua
host.native_ui.toast('Creating a squad...')           -- the game's toast, your words
host.native_ui.toast('[ST:col p1=red ]Oops')          -- the game's own text markup passes through (this one is red)
```

The text is 1 to 46 printable ASCII characters on one line (a longer one, a newline or anything else raises a Lua error); it returns true when accepted and false for another mod or no registration. The game has no way to take free text in a toast either, so the runtime borrows the text of that offline toast itself: it writes your words over that row of the game's notice table in memory (row `0x23e` of the notice table, found like the spare rows above), raises the toast the way the game's own squad screen code does (request `0x241` on the game's message manager), and puts the original text back four seconds of real time later (the toast itself is on screen for two to three). The game reads the text when the toast is shown, not when it is requested, so the row has to stay changed for that time. Consequences: only one toast shows at a time (a request made meanwhile waits and the latest one wins); the toast comes from the squad screen hooks, so it appears only while that screen is open; pressing the first or second card during those seconds shows your text instead of the offline text; the put-back also runs from those hooks, so a press inside those four seconds shows your text (the toast itself is up for two to three; in a test run a press right after it went away already showed the offline text), and if the player leaves the squad screen inside the four seconds the row stays changed until the screen is opened again, and the other screens that raise the same offline toast (the game requests `0x241` from at least three other places) would show your text meanwhile; and if the row is not found (another language, a modified table) the request is dropped and the log says so. Nothing is written to disk.

**First card and second card.** Mapped, they run the button's action on Confirm after the game's press animation (no confirmation window, no `report`). Unmapped, or when the button cannot run, they are answered by the game's own "not available offline" toast instead of its online code, which would crash offline. Back, direction keys and every other input still use the game's handling.

Because there is no network service behind the screen, the host also answers the game's "is the online service available" questions with *yes* **only while a registration holds `squad_select`**, and only from the exact call sites that the squad screen flow uses (for the card rows, only for a card that has a mapped action and is the create or join card). Nothing else about the game's online state, the save file or the story flags is touched.

## The squad details screen (opt in)

By default a reported `ok` returns the squad screen to the four cards. A mod that registers `details` gets the game's own **squad details screen** instead (the full screen with the passcode, a timer slot and two buttons, start and leave), with the game's own busy window ("connecting, please wait") while the action runs:

```lua
host.native_ui.register {
 version=1,
 online_entry=true,
 squad_select={create_private='host',play_private='join'},
 details={start='start',leave='leave'}        -- actions (buttons of your page) for the screen's two buttons; each is optional
}
-- later, when the action succeeded:
host.native_ui.report('ok',{passcode='27015',mode='created'})   -- mode: 'created', 'joined' or 'in_squad'
```

- `report('ok', {passcode=?, mode=?})`: the passcode is 1 to 15 letters, digits or hyphens (default `000000`); `mode` picks the explanation the screen shows: `created` (a squad was created), `joined` (a squad was joined) or `in_squad` (the player is already in a squad; its first button then returns to the game and only closes the screen). Without a mode, create shows `created` and join shows `joined`. A bad value raises a Lua error; `false` is returned for another mod or a mod that did not register `details`.
- While the screen is up the squad screen is hidden (the game's own `out` animation) and takes no input; when the screen closes it comes back (`in`).
- The **start button** runs the `start` action and the **leave button** runs the `leave` action, like any other `ui.action`, after the screen has closed. The return button (the first button in the `in_squad` mode) runs nothing. The mod decides what they mean; Co-op's `leave` stops its session.
- **The start and return buttons leave the menu**, as in the game: the notice status stays at 1 and the squad screen view is sent to its state 0x11, whose own code reads the 1 and exits to the field. The leave button clears the status and the squad screen comes back, as before.
- **While `set_session(true)` is in force the game is told it is in an online squad.** The game's own queries "am I in an online squad" (`f_70000c8960`, hook 7 at every caller) and "is the online state ready" (`f_70000c8908`, hook 10) answer yes to every caller, and the game's online check (`f_70004e3ae4`, hook 1) answers yes to every caller except the squad screen card handler's own call. This enables the main menu's Online rows (change squad, the player list, the members list and the others that check these) and makes the connect entry answer that the player is already connected; the change-squad row is then how a player leaves the squad. Each distinct caller is logged once (`native ui: the session query is asked by main+0x...`). Nothing changes while no session is reported. The HUD's Online globe is not driven by these queries.
- To show the screen again for a session that is already up (the player chooses create or join again), report `ok` with `mode='in_squad'` instead of an error message.
- **The timer slot.** The screen's countdown (an idle private squad is disbanded after 48 hours in the game) is of no use to a mod, so the slot is the mod's: `report('ok', {..., label='Players:', status='1 / 4'})` replaces the label (1 to 13 printable ASCII characters) and the countdown text (1 to 8), and `host.native_ui.details_status('2 / 4')` changes the status while the screen is open (it returns `true` when accepted). The label is a row of the game's message table (found like the toast row, and put back when the screen closes); the status is written into the countdown pane with the same calls the game's own timer makes. Without a status the screen keeps its initial `00:00:00`.
- **The passcode keyboard and an existing session.** With `details` registered, the **join card** asks for the passcode with the game's own keyboard (the recomp's text entry; the game limits it to 6 characters) and hands the typed text to the mapped action as the field `text` of its `ui.action` event (an empty text or characters a passcode cannot have end as the game's own "squad not found" failure after the usual busy window). While the mod says a session is up with `host.native_ui.set_session(true)` (and `false` when it ends), the keyboard is skipped and the action runs at once with no `text`, so the mod can answer `report('ok', {mode='in_squad', ...})` and the player lands on the details screen of the session they are in.
- **The busy window** is the game's own, shown from the moment the mod's action is accepted until it answers, and for at least 0.7 seconds so a fast answer is still seen.
- The game's own request that the screen's buttons would send is not sent: there is no online session behind it. Three checked hooks (the "still online?" question, the countdown refresh and the close) answer only while the adapter opened the screen.

## How the borrowed rows are found

Three spare rows, the toast row and the timer label row are the only game rows the adapter ever changes. None of their text is stored here. The runtime scans the game's memory for the two message tables by **shape** (the BDAT magic, three columns, ten-byte rows, first ID 1, and 5,565 rows for the main table or 683 for the notice table, with the column types checked), reads each row by its **ID**, and touches it only when its string has the **length and FNV-1a hash** recorded in `runtime/message_text.h`. It copies the row's original bytes into memory when it finds the row, so it can put them back. In another language, or in a modified table, the hash differs and nothing is touched. `tests/native_ui/message_text_test.c` checks all of this on made-up tables; setting `XCX_MESSAGE_TABLE` to the path of your own message table file also checks the recorded lengths and hashes against it, without storing anything.

## What this does not do

- It does not make public matchmaking, short codes, friends lists or the first and second cards' own results work; those two cards show the game's "not available offline" toast unless you map them to an action of your own. The cards themselves are not greyed: animation names tried on them had no visible effect.
- It adds no networking. What your action does is up to the mod and its provider.
- It does not edit saved progression and sets no global unlock flag; the connect entry answer is a per-call override that disappears with the registration.
- It is not a general hook API. Eleven functions of one pinned build are checked by build ID, executable hash and original instruction words at generation time; any mismatch aborts generation instead of producing a partly hooked game.

## For maintainers

`recomp/native_ui.py` is the catalogue (hook ids, entry addresses, the guarded caller of each) and the validator. The generator inserts `if (rt_native_ui_hook(id, c)) return;` at the entry of exactly those eleven functions of the main module (the seventh, the game's message post, is guarded to the one call inside the failure state and only swaps that window's message id for a spare row holding the mod's text; the eighth to tenth belong to the details screen and answer only while the adapter opened it; the eleventh is the "online state ready" question, answered only while a mod reports a session); `runtime/native_ui_adapter.c` (compiled per install, like the menu adapter) answers only when the caller's return address is the catalogued call site and a registration holds the matching feature. The registry is `runtime/mod_native_ui.c`; the native intent path is `mod_ui_native_intent_submit` in `runtime/mod_settings.c`. Tests: `python tests/native_ui/check.py [--sanitize]`.
