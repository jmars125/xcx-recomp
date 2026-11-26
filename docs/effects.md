# Effects

Four settings change effects the game draws. They are the **Effects** group on the F1 menu's Graphics tab and the
`[graphics]` section of `xcx.ini`. All four apply at once while playing, change no game files and save nothing, and
need game version 2.0.0: on another version the rows stay in the menu, grayed out with "Needs game version 2.0.0.".

| Row | Key | Values | Default |
|---|---|---|---|
| Ambient occlusion | `ao` | `original`, `light`, `medium`, `strong` | `original` (*Original*) |
| Bloom | `bloom` | `off`, `soft`, `original`, `strong`, `intense` | `original` (*Original*) |
| Camera rain droplets | `rain_droplets` | `0` (*Off*), `1` (*On*) | `1` |
| Depth of field | `bokeh` | `off`, `horizon`, `vista`, `portrait` | `off` |
| ↳ Sky (while on) | `bokeh_sky` | `0` (*Sharp*), `1` (*Soft*) | `0` |
| ↳ Clouds (while on) | `bokeh_clouds` | `0` (*Sharp*), `1` (*Soft*) | `0` |

At the defaults none of these run: no pass, no copy, no skipped draw. An unknown word means the default; the words
are lowercase, as above.

The game's draws these settings change are recognized by a hash of their guest shader code, so they are found the
same way on every continent, indoors and in cities. On a game version whose shaders differ, nothing is found and the
game's own picture stays.

## Ambient occlusion

The soft shading where surfaces meet: in corners, under ledges, between grass clumps and in leaves.

- **Original:** the game's own. It samples only six depth points per pixel and blurs across edges, which gives the
  soft dark halos around characters and grass and a shimmer at a distance.
- **Light:** our own occlusion with a 1 m radius: sharper, in creases only.
- **Medium:** a 4 m radius on half the samples and 1 m on the rest: broad shade under bushes and foliage, with the
  creases kept.
- **Strong:** the 4 m radius on every sample, at the full scene size instead of half: the deepest shade.

There is no Off: the game lets its occlusion take at most about half of the ambient light, so without it the picture
looks the same as Original.

### How it works

`runtime/vk_ao.c`. After the G-buffer, the game draws its occlusion at half size into a two-channel target and blurs
it twice; the lighting pass then reads it. With Light, Medium or Strong, those three draws are skipped and our compute
passes take their place: 16 samples on a spiral disc from the scene depth, oriented by the surface normal
(`shaders/ao_main.comp`), then a depth-aware blur across and down (`shaders/ao_blur.comp`) so the shading doesn't bleed
over edges. Light and Medium write the result into the game's target, where the lighting reads it as before. Strong
works at the scene size, and the lighting pass's binding for that target is pointed at the full-size image instead.

Because the game caps occlusion at about half of the ambient light, a better occlusion alone barely shows. Light,
Medium and Strong also halve that cap in the lighting programs (a copy of their constants, bound in place of the
game's), which is what makes bushes, plants and corners read darker. In direct sunlight the change stays small, since
occlusion only darkens ambient light.

## Bloom

The glow around bright light: the sun, lamps, glowing parts. The game blurs the bright parts of the picture at three
sizes and adds them up with fixed weights before adding the glow to the scene.

- **Off:** no glow.
- **Soft:** half the game's glow, for a clearer picture (above all with HDR on).
- **Original:** the game's own.
- **Strong:** half again as much.
- **Intense:** twice the game's: big halos at night, and it can wash out a bright sunset sky.

### How it works

The draw that sums the three blur sizes reads their weights from its constants (0.2, 0.1, 0.1 in the field). For any
level other than Original, a copy of those constants with the weights scaled (0, 0.5, 1.5 or 2) is bound in place of
the game's for that draw (`bloom_scale` and `bloom_copy` in `runtime/vk_exec_draw.c`). No pass is added.

## Camera rain droplets

In rain the game draws round droplets in a small space just in front of the camera. They follow the view, and on
screen they land on distant ledges and cliffs as big blobs. **Off** leaves those draws out; the falling rain and the
splashes on the ground near you stay. A New LA effect that uses the same shader with a different texture is not
affected.

## Depth of field

Softens what is far away while you play, with disc-shaped bokeh on far lights, so the background feels further away.

| Step | Starts softening at | Fully soft from |
|---|---|---|
| Horizon | about 800 m | about 3000 m |
| Vista | about 300 m | about 1500 m |
| Portrait | about 100 m | about 600 m |

- **Sky:** Sharp (default) keeps the sky (space, the moons, the stars) as the game draws it while the far land softens
  into it; Soft blurs it too.
- **Clouds:** Sharp (default) keeps the clouds sharp, also over far land; Soft blurs them.

The interface is drawn after this, so menus, the map and the HUD are never blurred; markers the game draws into the
scene itself are. Cutscenes get it too, on top of the game's own depth of field there, which is left as it is.

### How it works

`runtime/vk_bokeh.c`, at the point in the frame where DLSS runs: the light target holds the finished picture and the
depth buffer still holds the frame's scene. Three compute passes: the picture at half size in linear light, with each
pixel's blur radius from its distance (`shaders/bokeh_down.comp`); a disc gather at half size that keeps sharp near
objects out of the blur behind them (`shaders/bokeh_blur.comp`); back to full size, keeping the game's own pixel wherever
the radius is under half a pixel (`shaders/bokeh_comp.comp`). The radius follows the scene height, so the look is the
same at every resolution, render scale and console mode.

The game projects with an infinite far plane, so the sky is where nothing wrote depth. The clouds are one shader drawn
in their own pass after the sky; with Sky or Clouds at Sharp, the light target is copied just before and after it to
keep the clouds apart.

## Cost

Measured on an RTX 4090 at 2560x1440 with a 144 fps limit, the GPU's own time per frame (timestamps), each level
against the default on the same save:

| Setting | GPU time per frame |
|---|---|
| Ambient occlusion Light, Medium | +0.13 ms (3.52 → 3.65 ms) |
| Ambient occlusion Strong | +0.62 ms (3.52 → 4.14 ms, about +18 %) |
| Depth of field Horizon, Vista | +0.11 to +0.12 ms (3.11 → 3.22-3.23 ms) |
| Depth of field Portrait | +0.17 ms (3.11 → 3.28 ms) |
| Bloom (any level) | none: three constants of a draw the game makes anyway |
| Camera rain droplets Off | none (draws left out) |

All of them held 144 fps. At the game's 60 fps cap none of them changed the frame rate or the frame time. Depth of
field's work images (at 1440p about 83 MB, or 44 MB with Sky and Clouds both Soft) and ambient occlusion's (about 11 MB at Light and Medium, 44 MB at Strong) are freed when they are turned off.

## Limits

- **Game version 2.0.0 only.** The shaders are found by their guest code; on another version nothing is found and the
  game's own picture stays.
- **Ambient occlusion** shows most on vegetation and in shade; in sunlit streets and open vistas it changes little.
- **Depth of field** also softens markers the game draws into the scene (not the HUD).
