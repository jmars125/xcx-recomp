# World detail

Three settings change how much of the game world is loaded and shown around the player. They are the **World detail**
group at the bottom of the F1 menu's Game tab, and keys in the `[display]` section of `xcx.ini`. All of them apply at
once while playing and change no game files. They work only on game version 2.0.0: on another version their patch
sets or code checks are missing, and the menu leaves their rows out.

| Row | Key | Values | Default |
|---|---|---|---|
| Grass distance | `grass_distance` | 1, 1.5, 2, 3 (times the game's ranges) | 1 (*Original*), the game's |
| Object detail range | `object_lod_range` | 0 to 1000 meters added; the menu steps are +10, +20, +30, +50, +75, +100, +150, +200, +300, +500, +750 and +1000 m | 0 (*Original*), the game's |
| Adjust range to frame rate | `object_lod_mode` | `auto`, `fixed` | `auto` |
| Crowd distance | `crowd_distance` | 1, 1.5, 2, 3, 4 (times the game's distances) | 1 (*Original*), the game's |

At the defaults the game runs its own code: the `grass` and `crowd` patch sets are off and Object detail range writes
nothing. Only `grasspool`, the larger grass heap below, is always on.

## Grass distance

Grass pops in close to the character because each grass layer loads only within a fixed range of the camera: 10 to
90 m depending on the layer, most of them 60 to 80 m (in Primordia's fields). Grass distance multiplies those ranges by 1.5, 2 or 3.

How it works (`runtime/grass.c`, patch sets `grass` and `grasspool` in `recomp/patches-upd.txt`, which lists each
site and the registers it writes):

- Grass loads per layer from the game's layer table (+8 of each 12-byte entry is its range). Each frame the grass
  update (main + 0x9fd830) loads the cells within the range of the camera, and the instance builders cull each grass
  placement against rings at 1.7, 2.5 and 3.3 times a per-type distance. Set `grass` multiplies the two range loads and
  the two ring distances by a float on the patch page (+0x100). The loading range is what limits grass, so scaling the
  rings alone would change nothing.
- **The fine window:** the finest cells are kept within 30 m of the camera and rebuilt as it moves, so a second,
  denser layer would still pop in 30 m away at every setting. Those sites read the window from page +0x104.
- **The minimum range:** the shortest layer (10 m, the low grass right around the character) would still end 30 m out
  at 3x. The range loads raise every scaled range to at least page +0x108.
- The window and the minimum are both 30 m times the setting (45, 60 or 90 m), so 1 keeps the game's 30 m.
- **Memory:** above 2x the cells no longer fit the grass system's cell heap. Set `grasspool` makes that heap 4 MiB larger
  (from 0x1c0000 to 0x5c0000 bytes). It is always on, because the heap is made once when the grass system starts and
  must already be large when the setting is raised later. At 3x about 4 of the 6 MB are used.

`xcx.ini` takes any value from 1 to 3; under 1, text that isn't a number, and NaN count as 1, more than 3 as 3. The
setting stops at 3 because at 4x grass stops loading and the game then crashes in its cell list code
(main + 0x993ad0), even with a much larger heap, so some other fixed structure of the loader is the limit.

The new ranges apply to the grass the game loads after the change, so with the camera standing still the change
shows once the camera moves.

## Object detail range

Trees, bushes, rocks and other map objects switch from a low-detail to a detailed model at a distance set per model,
from under 5 m to about 50 m. Object detail range adds the same number of meters to every one of those distances, so
the detailed models show from further away. Added meters move every object by the same amount, where a multiplier
would leave the shortest ones short.

How it works (`runtime/objlod.c`, no code patched):

- The map object renderer (main + 0x9ad0a0) walks a spatial tree (main + 0x9fa400) and picks each object's level of
  detail (main + 0x9fa5a0): the first of the model's LOD records whose switch distance covers the camera distance
  wins, and past the last one the object is hidden. The walk and the pick both add a margin to every distance, a float
  at +0x9ec of the map object settings (pointer at main + 0x27681e0). The game only ever sets it to 0.
  `objlod.c` writes the meters there at every present.
- Before it writes, it checks the four instructions it relies on. If one differs it logs which, the setting stays
  off, and the row is left out.
- `xcx.ini` takes any value from 0 to 1000, and the row shows one between two steps in its place; under 0, text and
  NaN count as 0, more than 1000 as 1000. Any `object_lod_mode` other than `fixed` counts as `auto`.
- It doesn't cover people, vehicles or Skells (they appear at their normal distance through another system), or a
  few plants that switch models elsewhere.

The cost is CPU time: more objects at their detailed level means more draws to record. Out on the continents
+1000 m costs little; in a busy part of New Los Angeles it can cost around 20 fps. GPU time and video memory stay
about the same.

**Adjust range to frame rate** is an indented row under it, shown while the range is above Original. **Auto** (the
default) treats the range as a ceiling and moves the range in effect between 0 and it so frames fit the frame budget: the frame limit, or
the display's refresh rate without one. The row then shows the range in effect ("now +N m"). **Fixed** always uses the
range as set. Auto takes each frame's busy time (its interval minus the time it waited for pacing, `perf.c`
`perf_last_frame`) as a median over half a second:

- Over 103% of the budget for a second, or over 125% for half a second: the range goes down by 8% plus one and a half
  times the overrun, at most 70% and at least 10 m. It doesn't go up again for a second, nor back up to the value that
  was too much for 5 seconds. The half second after a cut isn't judged, since its frames mostly came before the cut.
- Under 92% of the budget: up by 8% to 30% of the range in effect, at least 10 m, more the more time is spare. So it
  creeps near 0 and near the edge of the budget, and strides when there is plenty of room.
- Half seconds whose frames take 100 ms or more (loading, a pause) are skipped.

## Crowd distance

How far from the player the crowd people (the walkers in New Los Angeles) appear and fade out: 1.5, 2, 3 or 4 times
the game's distances, which are about 25 m in New Los Angeles.

How it works (`runtime/crowd.c`, patch set `crowd`):

- The crowd people come in groups placed by the map. Each group spawns its people while the player is within its
  spawn distance and fades them out past its leave distance. A group stores both as halfwords when it is made, and
  every update a spawn check (main + 0x3982d8) and a fade check (main + 0x39773c) compare them with the player's
  distance. Set `crowd` multiplies the stored distance by a float on the patch page (+0x120) in both checks, just
  before the compare. Because it acts where the distances are compared, a change reaches every group at its next
  check, including groups made before it. A distance of 0 (no limit) stays unlimited.
- `xcx.ini` takes any value from 1 to 4; under 1, text and NaN count as 1, more than 4 as 4.

Crowds also get denser, quickly with the step. No group gets more people, but many more groups are active at once:
the area within the spawn distance grows with its square (16 times at 4x), and walkers stay until they pass the
larger leave distance.

The cost is CPU time for the extra people's draws, in busy places: in the Commercial District at 4x a frame-limited
90 fps dropped to about 66 fps with the replay on the game's thread. With **GPU frame prep on its own thread**
(Advanced tab, `replay_async = 1` in `[performance]`, applies after a restart) the same spot held 88 to 90 fps, so turn that on before raising Crowd distance.
With it, brief vertex explosions at the start of camera pans can show at the higher steps; **Snapshot GPU data at
submit**, a row that shows under it once it is on (`mirror_cow = 1`, also after a restart; see [architecture.md](architecture.md#4-graphics)) stops them.

The setting stops at 4x: at 6x and 8x the game crashed after a minute or so in the job that steps the characters'
physics bodies, where past some number of bodies the extra walkers don't get all their parts.
