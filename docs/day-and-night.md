# Day and night

Three settings change how time passes and how the light changes through the day. They are the **Day and night**
group on the F1 menu's Game tab and the `[game]` section of `xcx.ini`. All three apply at once while playing, change no
game files and save nothing but the time of day the clock reached, and need game version 2.0.0: on another version the rows stay in the menu,
grayed out with "Needs game version 2.0.0.".

| Row | Key | Values | Default |
|---|---|---|---|
| Day length | `day_length` | real minutes a game day lasts: 0 (stopped) or 1 to 1440; the menu offers 1, 2, 3, 6, 12, 24, 48, 96, 144 and Stopped | 24 (*Original*), the game's own |
| Dawn and dusk | `dawn_dusk` | `original`, `smoothed`, `extended` (*Smoothed & Extended*) | `original` (*Original*) |
| Night darkness | `dark_nights` | `original`, `darker`, `darkest` | `original` (*Original*) |

At the defaults nothing in game memory is read or written.

## Day length

How many real minutes a whole day takes on the in-game clock. The game's own clock adds a game minute each real
second, a 24-minute day. Each choice in the row shows its speed ("1 min (24x)", "48 min (0.5x)"). A value set by hand
in `xcx.ini` between two of the menu's steps works and shows in the row in its place. Over 1440 counts as 1440, and over 0 but
under 1 as 1 (the row has no entry for such a value); text, a negative value or NaN means
the game's own speed. An unknown `dawn_dusk` or `dark_nights` word means Original. Stopped
holds the clock; a time change in the game's own menu still moves it, with every setting.

Very short days need a steady frame rate: the clock can gain at most one game minute a frame on top of the game's own,
so the top speed is the frame rate plus one game minute a real second, and a 1-minute day needs about 24 frames a
second. That is why the shortest step is 1 minute.

### How it works

`runtime/day_length.c` runs once a present. It never writes the game time: it changes only the stamp the game's clock
update compares with the real time (the clock object at main + 0x1FEFC38, its update at main + 0x1e7640), so the game
adds every minute itself, with its own carries, and the time never goes back.

- **Faster:** whenever an extra game minute is due, the stamp is wound back one second (a compare-and-swap, at most
  one a frame). The game's next update counts that second and adds the minute, so the light moves smoothly.
- **Slower and Stopped:** while no minute is owed, the stamp is held ahead of the real time after each update, so the
  game counts no whole second and adds nothing. Minutes owed grow at the chosen speed; once one is owed, the stamp goes
  a second back as above. The clock's smooth between-minutes value is set from the part of the next minute owed, so
  the light moves at the slower speed and holds still at Stopped.
- **Changes the game makes** (its own time change, a save loading: backwards, or more than 2 minutes) are followed,
  and nothing owed is carried past them.
- **When the clock stops** (the title screen, loads): a stamp that hasn't moved for 2 real seconds counts as a stopped
  clock. Nothing is written, and both modes start afresh when it runs again.
- **Only on 2.0.0, and only once it checks out:** before the first write it checks once a second that the clock's
  smooth value follows its packed time. Three failed checks in a row leave the clock alone for the session, log it,
  and gray out all three rows with the note "Off for this session: the game's clock was not where expected." (Dawn and dusk and Night darkness read the time of day through the same
  check, `clock_ready`).

The arithmetic is in `runtime/day_length.h`, without game memory, so it can be tested on its own. Lost holds and
followed changes are counted in the log ("game minute(s) added past a hold, … change(s) of the time followed").

## Dawn and dusk

The game changes the light at dawn and dusk in steps: the sky jumps brighter at 18:45 and the ground dips at 05:00
(on screen a minute or two later). Those are the moments the game switches the picture's color table, and its color pass shows the new
table in one frame instead of fading to it.

- **Original:** the game's own, with the steps.
- **Smoothed:** the game's times, without the steps. The two color tables are crossfaded through the blend, the
  exposure follows the falling light instead of rising ahead of it at dusk, and the dawn light is eased so it doesn't
  rise fast first and slowly later.
- **Smoothed & Extended:** as Smoothed, and the times are stretched: dawn from 04:00 to 08:00, dusk from 17:00 to
  21:00.

## Night darkness

How dark the night is on the ground, people and things: **Darker** is about 78 % and **Darkest** about 60 % of the
game's brightness on screen. The night sky stays as the game draws it. The darkness fades in at dusk and out at dawn
with the night light, so it follows whichever Dawn and dusk timing is in use.

## How Dawn and dusk and Night darkness work

The game lights a map from three things in its map header (`map/<map>.wismhd`), all blended by time of day:

- **Light and fog keys:** looks with a start time and a blend length, blended in straight lines by the map light
  update (main + 0x982b40). Each light key has a color, an intensity and one more value.
- **Post-effect sets** (exposure and the rest), blended field by field in main + 0x3fb374 by the post manager (the object at main + 0x1fefba0), each
  with its own start and blend length in minutes.
- **The color table** (a 16x16x16 3D table) of the active post set.

`runtime/dawn_dusk.c` runs once a present and does nothing at Original + Original:

- **Color tables crossfaded in their data** (Smoothed, Smoothed & Extended, and Night darkness with Dawn and dusk at
  Original): a clean copy of each table is kept, and while a post set blends both tables get old + (new − old) × t, with the curved t below when it is on;
  the game's own fade is held at the same share so it keeps both until the blend ends. Then both get their clean data
  back.
- **The post blend curved by direction** (Smoothed, Smoothed & Extended): t + (t² − t) × k, with k = 1 into night and
  k = −0.7 into day (a set that starts at 12:00 or later counts as into night), while the active set's blend runs
  and for a minute after, so the blend's last frames still get the curve.
- **An eased dawn light** (Smoothed, Smoothed & Extended): inside the dawn window the light keys see an eased time
  instead of the real one, as an offset that is 0 at both edges of the window: 04:40 to 07:00 with a smoothstep for
  Smoothed, 04:00 to 08:00 with a smoothstep of u^1.2 for Smoothed & Extended.
- **Moved times** (Smoothed & Extended): the key and post-set start times and blend lengths are written in memory, with
  the originals kept. Another choice writes them back at once, and a map load (the header read again, the post sets
  built again) is caught within a second.
- **Night darkness:** the night light keys' intensity × 0.70 (Darker) or × 0.42 (Darkest). The fog keys (the sky) and
  the color tables are left as they are.

Nothing is written blind: every key block and moved post set keeps the values written and is checked in full once a
second, a block is written only while the game's key blend has read it in the last 2 seconds, and a color table only
while its texture object still holds the same table at the same place. Every address read from game data that is
written through is checked against `RT_ADDRESS_LIMIT`. The fixed limits of 512 timed keys a block, 256 moved post
sets and 6 color tables each say so once in the log when they are reached; with a fifth key block, the one seen
longest ago is given back and let go, and the log says so.

Four patch sets in `recomp/patches-upd.txt` give `dawn_dusk.c` what it needs. They are switched off before any game
code runs and follow the two settings from then on (their `XCX_DD_*_PATCH` keys have no effect); back at Original + Original everything is
given back and all four are off again.

| Set | Site | Does | Page slot |
|---|---|---|---|
| ddlut | main + 0x977234 (color-table request) | saves the table manager (x0) | +0x920 |
| ddcurve | main + 0x3fb390 (in the post field blend) | s8 = t + (t² − t) × k | +0x940 (k) |
| ddlight | main + 0x982b6c (the light update's time load) | time × a + b | +0x950, +0x954 |
| ddkeys | main + 0x988c80 (the light key blend's entry) | saves the key time data (x9) | +0x968 |

Key blocks that don't have the main map's timetable are left alone, so on a map with other key times both settings
leave its light as the game's. Known limitation: inside the dawn window a scene with a time of its own (a cutscene)
gets the eased dawn's offset too.
