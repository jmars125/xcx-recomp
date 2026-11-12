# Recomp Achievements

When [RecompLauncher](https://recomplauncher.com) starts the game, `xcx.exe` reports achievement events to it over the
local channel of [Recomp Achievements](https://github.com/elripalda/RecompAchievements) (its `docs/SPEC.md`, version
1). The launcher shows the toast, keeps the unlocks and decides what counts as Ranked. Started any other way, the game
does none of this. The client is our own code, `runtime/achievements.c`; the library's license lets any game use the
list format and the channel, and nothing of the library is copied.

## Events

The list is an `events` list: RecompLauncher hosts it and maps each event name to an achievement, so achievements can
be added or changed without a game update, and an event no list names does nothing. A thread reads the game's own data
twice a second (no work on the game's threads):

| Event | When |
| --- | --- |
| `ach_1` … `ach_843` | The game itself awards its own achievement with that ID (below) |
| `level_10`, `level_20`, `level_30`, `level_40`, `level_50`, `level_60` | Any character reaches that level |
| `funds_1m`, `funds_10m` | Funds reach 1,000,000 or 10,000,000 |
| `funds_max` | Funds reach the cap, 99,999,999 |
| `tickets_max` | Tickets reach the cap, 9,999 |
| `prologue` | The prologue is over (chapter 1 starts) |
| `chapter_1` … `chapter_12` | That chapter is cleared |
| `chapter_13_act_1`, `chapter_13_act_2` | Chapter 13 (new in the Definitive Edition): that act is cleared |
| `chapter_13`, `story_cleared` | Chapter 13, act 3 is cleared: the end of the story |

An event fires only when the game is seen without it first, with the values settled for 10 s after the last save load. A
save that already has an achievement, a level, the Funds or a chapter never fires anything when it loads. The game data
object is filled with defaults for the first moments after boot and then rewritten in place by the save the game reads
at start, or by a load from the menu. A load is seen when 3 or more characters' levels change in one look, a level or
the scenario number goes down, the scenario number skips a chapter (play only ever moves it one step, or from one
chapter's last step to the next chapter's first), an earned achievement turns unearned, or more than 8 achievements are
earned in one look (half a second). A load that changes only Funds or Tickets, moves the scenario by less than that and
adds 8 or fewer achievements is not seen, so their events could fire on it.

What the game reads, build `EC4ED46BBC4A9EBB` of update 2.0.0: the game data object's pointer at main + `0x1DC1408`;
Funds a `u32` at `+0x1B5CC`, Tickets a `u32` at `+0x1B5D4`; 23 character slots `0x7F8` apart from the object, the level
a `u16` at `+0x172` in each; the scenario number a `u16` at `+0x7867A`; the game's achievement records from
`+0x91528`. `XCX_ACH_TRACE=1` logs the values read (and how many of the game's achievements are earned) when they
change, and each load.

### The game's own achievements

The game keeps its own list of achievements (the in-game Achievements menu): the table `SCL_AchiveList` (sic), 844
rows, IDs 1 to 843 and a last empty row `ACHIVE_END`. The event for one is `ach_` and the row's ID, so the list for
RecompLauncher can name any of them. Each row names a statistic (`StatsID`) and a goal (`Count`); when the game counts
that statistic up to the goal, it awards the achievement (setting the statistic by other means, a cheat for one, awards
nothing: the game checks only when it counts). The row's `Disp` is 1 for the 741 the menu lists; the others are kept
but never shown.

The result is a 16-byte record per ID at game data object + `0x91528` + ID × 16 (record 0 unused). The function the
menu asks, main + `0x4fa03c`, reads it at `0x17AB0` + ID × 16 into the block main + `0x4e4054` returns, which is the
object + `0x79A78`. The first `u32` holds flags: bit 1 is set when the achievement is awarded (and bit 0 with it), bit
2 once the menu has shown it as new, so an earned record reads 3 or 7. The third `u32` is when it was awarded, in
seconds since 1970. The records are saved with the rest of the object. A record with any other flag bit set means the
offsets do not fit the build, and the thread reads nothing.

The row IDs group by kind, from the row names: `ACHIVE_EV…` (1–54) the story parts of the prologue and chapters 1–12,
then `ACHIVE_QE…`, `ACHIVE_FLD…`, `ACHIVE_BTL…` and the rest, and `ACHIVE_STS_…` (790–843) the Definitive Edition's:
chapter 13's acts, the new party members and more. The story ones are one per part of a chapter, numbered as the menu
shows them:

| Part | IDs, in order | Last part |
| --- | --- | --- |
| Prologue | 1–5 | 5 |
| Chapter 1 | 6–10 | 10 |
| Chapter 2 | 11–14 | 14 |
| Chapter 3 | 15–18 | 18 |
| Chapter 4 | 19–22 | 22 |
| Chapter 5 | 23–27 | 27 |
| Chapter 6 | 28–31 | 31 |
| Chapter 7 | 32–35 | 35 |
| Chapter 8 | 36–38 | 38 |
| Chapter 9 | 39–42 | 42 |
| Chapter 10 | 43–45 | 45 |
| Chapter 11 | 46–49 | 49 |
| Chapter 12 | 50–54 | 54 |
| Chapter 13, act 1 | 790–796 | 796 |
| Chapter 13, act 2 | 797–805 | 805 |
| Chapter 13, act 3 | 806–811 | 811 |

Titles and descriptions are game text and stay out of this repository; the launcher's list carries its own. To read the
game's: the row's `Title` and `Description` columns are row numbers in the text table `MNU_Achievement_ms` (its `name`
column), and `Title_replace_ms`/`_id` and `Desc_replace_ms`/`_id` name another text when the row has one. Both tables
are BDAT version 4 in the game's data (in memory once the game has loaded them, each starting with `BDAT`, `0x04`); a
row's name, such as `ACHIVE_EV000`, is stored in its `ID` column as the murmur3 hash (seed 0) of the name. Seen on
saves: a New Los Angeles test save has 15 earned, IDs 1–6 among them.

### The scenario number

The story's progress is one number, the one the game's scripts read with `getScenario` and write with `setScenario`
(their natives at main + `0x3ed320` and `0x3ed370` call the flag store's get and set, main + `0xc96d0` and `0xc94a0`,
for 16-bit value 1). The flag store sits inside the game data object at `+0x72E78`; its 16-bit values start `0x5800`
into it, so value 1 is at `+0x7867A`. It is saved with the rest of the object.

It counts chapter × 1000 + step. The game's own conditions (the table `FLD_ConditionList_Scenario`, read from memory;
its row names are the murmur3 hashes in its ID column) give the steps:

| Part | Runs | Cleared at (condition) |
| --- | --- | --- |
| Prologue | 1–8 | 1001, chapter 1 starts (`COND_SCEN_START_M01`) |
| Chapter 1 | 1001–1011 | 1012 (`COND_SCEN_CLEAR_M01`) |
| Chapter 2 | 2001–2006 | 2007 (`CLEAR_M02`) |
| Chapter 3 | 3001–3009 | 3010 (`CLEAR_M03`) |
| Chapter 4 | 4001–4011 | 4012 (`CLEAR_M04`) |
| Chapter 5 | 5001–5014 | 5015 (`CLEAR_M05`) |
| Chapter 6 | 6002–6014 | 6015 (`CLEAR_M06`) |
| Chapter 7 | 7001–7007 | 7008 (`CLEAR_M07`) |
| Chapter 8 | 8001–8022 | 8023 (`CLEAR_M08`) |
| Chapter 9 | 9001–9011 | 9012 (`CLEAR_M09`) |
| Chapter 10 | 10001–10009 | 10010 (`CLEAR_M10`) |
| Chapter 11 | 11001–11010 | 11011 (`CLEAR_M11`) |
| Chapter 12 | 12001–12022 | 12023 (`CLEAR_M12`) |
| Chapter 13, act 1 (`M31`) | 13002–13020 | 13021 (`COND_SCEN_0009_CLEAR_M31`) |
| Chapter 13, act 2 (`M32`) | 13021–14036 | 14037 (`COND_SCEN_0010_CLEAR_M32`) |
| Chapter 13, act 3 (`M33`) | 14037–15025 | 15026 (`COND_SCEN_0011_CLEAR_M33`); 15027 is the last value |

Chapters 1–12 match the Wii U game's table. The chapter menu (`MNU_ChapterList`) names M31–M33 "Chapter 13, Act
1–3". Seen on saves: the prologue test saves read 8, the New Los Angeles ones 1001 and 1007.

Also sent: `hello` on connect (`console` `switch`, `game` `xcx-recomp`, the port's version); `save_loaded` at each load
after the object first appears; `casual` with reason `cheats` while a cheat is switched on (F1 > Cheats), so those
unlocks are kept as Casual. The launcher's `peek` (its dev tools' memory viewer) is answered from guest memory at the
guest address it names (main is at `0x7000000000`). `list` and `quiet` need nothing from an events list.

## The list for RecompLauncher

A starting point to give the RecompLauncher team (they host and check the list; titles, descriptions and points are
theirs to set, from the game's own texts as above). It names the game's story achievements: the last part of each
chapter is the spec's `progression` type and the last part of chapter 13, act 3 is `win`, which Beaten needs. Any other
of the 843 can be added the same way, as `ach_<ID>`.

```json
{
  "format": 1,
  "game": "xcx-recomp",
  "source": "events",
  "versions": ">=0.1.22",
  "voids": [],
  "achievements": [
    {"id": 1, "title": "Prologue", "description": "Clear the prologue", "points": 10, "type": "progression", "event": "ach_5"},
    {"id": 2, "title": "Chapter 1", "description": "Clear Chapter 1", "points": 10, "type": "progression", "event": "ach_10"},
    {"id": 3, "title": "Chapter 2", "description": "Clear Chapter 2", "points": 10, "type": "progression", "event": "ach_14"},
    {"id": 4, "title": "Chapter 3", "description": "Clear Chapter 3", "points": 10, "type": "progression", "event": "ach_18"},
    {"id": 5, "title": "Chapter 4", "description": "Clear Chapter 4", "points": 10, "type": "progression", "event": "ach_22"},
    {"id": 6, "title": "Chapter 5", "description": "Clear Chapter 5", "points": 10, "type": "progression", "event": "ach_27"},
    {"id": 7, "title": "Chapter 6", "description": "Clear Chapter 6", "points": 10, "type": "progression", "event": "ach_31"},
    {"id": 8, "title": "Chapter 7", "description": "Clear Chapter 7", "points": 10, "type": "progression", "event": "ach_35"},
    {"id": 9, "title": "Chapter 8", "description": "Clear Chapter 8", "points": 10, "type": "progression", "event": "ach_38"},
    {"id": 10, "title": "Chapter 9", "description": "Clear Chapter 9", "points": 10, "type": "progression", "event": "ach_42"},
    {"id": 11, "title": "Chapter 10", "description": "Clear Chapter 10", "points": 10, "type": "progression", "event": "ach_45"},
    {"id": 12, "title": "Chapter 11", "description": "Clear Chapter 11", "points": 10, "type": "progression", "event": "ach_49"},
    {"id": 13, "title": "Chapter 12", "description": "Clear Chapter 12", "points": 10, "type": "progression", "event": "ach_54"},
    {"id": 14, "title": "Chapter 13, Act 1", "description": "Clear Chapter 13, Act 1", "points": 10, "type": "progression", "event": "ach_796"},
    {"id": 15, "title": "Chapter 13, Act 2", "description": "Clear Chapter 13, Act 2", "points": 10, "type": "progression", "event": "ach_805"},
    {"id": 16, "title": "Chapter 13, Act 3", "description": "Clear Chapter 13 and finish the story", "points": 50, "type": "win", "event": "ach_811"}
  ]
}
```

The `level_…`, `funds_…`, `tickets_max` and story events above are ours, from before the game's own achievements were
read; the game has its own for levels, credits and the story too.

## Testing without the launcher

`tests/achievements_fake_launcher.py` plays the launcher's side on Windows: it creates the pipe, starts the game with
`RL_SESSION` naming it and prints every line the game sends.

```sh
python tests/achievements_fake_launcher.py --seconds 120 -- <install>/xcx.exe --seconds 100
```

With `XCX_ACH_TRACE=1` and a cheat that sets Funds while a button is held (a mod's `cheats/EC4ED46BBC4A9EBB.txt`,
pressed by `XCX_INPUT_SCRIPT`), `funds_1m`, `funds_10m` and `funds_max` fire once each, after `casual`.

The story events the same way, with cheats that write the scenario number while a button is held (`58000000 01DC1408`,
`78000000 0007867A`, `62000000 00000000 <value>`), on a copy of a New Los Angeles save (scenario 1001): 1012 then
2007 fire `chapter_1` and `chapter_2` (`prologue` does not: the save already had it); 15026 from there skips chapters,
so it is a load (`save_loaded`, nothing fires); 14037 then 15026 fire `chapter_13` and `story_cleared`.

The game's own achievements the same way, with a cheat that writes a record's flags while a button is held
(`58000000 01DC1408`, `78000000 <0x91528 + ID * 16>`, `64000000 00000000 00000003`): on a copy of a New Los Angeles
save (15 earned), the load sends nothing for those 15 and the cheat sends `ach_<ID>` once.
