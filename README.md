# Pull-from-bambu-studio

Phase-1 port of the **"one tool-change per layer" image/texture color printing**
feature from
[OrcaSlicer-ImageMap](https://github.com/sentientstardust-dev/OrcaSlicer-ImageMap)
into [Bambu Studio](https://github.com/bambulab/BambuStudio).

* **Read [`PORT_SUMMARY.md`](PORT_SUMMARY.md) first** — files changed,
  provenance, the default-off toggle, low-confidence flags, and the
  disabled-state / H2C audit.
* `bambustudio/` — newly added files in their in-tree locations (the vendored
  ColorSolver / Pigment Painter / prusa-fdm-mixer libraries and the ported
  `ImageMapPerLayerColor` module).
* `patches/imagemap-port-modified-files.patch` — the diff to existing Bambu
  Studio files (against `bambulab/BambuStudio` commit
  `926a7192574bcb9b3a732e1ec59a46d79cb45466`, v02.08.02.61).
* `apply.sh /path/to/BambuStudio` — copies the new files and applies the patch
  (verified to reproduce the ported tree exactly on a clean base checkout).

The feature ships **off by default** behind the
`image_map_per_layer_color_rotation` print setting.

## How to use it

1. Load the filaments you want to mix from into the AMS, e.g. cyan, magenta,
   yellow and white in slots 1–4.
2. Paint the model with any colours you like (Bambu's normal colour painting,
   or an imported colour OBJ/3MF). Painted colours do **not** have to be
   loaded: add them as extra filaments in the project's filament list.
3. Switch the settings panel to **Advanced**, open **Process > Others >
   Image map per-layer color (experimental)** and:
   * tick **Image-map per-layer color rotation**;
   * set **Rotation filaments** to the loaded slots, in order, e.g. `1,2,3,4`
     (leave empty to rotate through every painted filament instead);
   * pick the wall map: **Offset outer wall surface** (default, safest),
     **Vary outer wall line width**, or **Combined (preset)** for the
     strongest effect.
4. Slice. Every layer is printed in one rotation filament (one tool change per
   layer); the outer wall moves in and out per layer so each painted region
   reads as its colour from the side.

For the original OrcaSlicer-ImageMap geometry (a 0.95 mm outer wall narrowed
to 0.32 mm on layers whose filament isn't wanted), set **Outer wall line
width** to 0.95 mm and use **Vary outer wall line width** with minimum
0.32 mm, widening off.

What is *not* ported (see `PORT_SUMMARY.md`, "Out of scope"): image/texture
import and projection, per-pixel texture sampling and dithering, gradients,
top-surface image printing, and prime-tower images. Colours come from
painted regions, one colour per region.
