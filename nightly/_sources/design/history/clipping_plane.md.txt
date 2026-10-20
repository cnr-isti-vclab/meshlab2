# Clipping plane, applied 2026-09-24

Until this, the only way to cut into an object was `view.nearClipRatio` on `Ctrl`+wheel,
which slid the projection's near plane towards the eye. The replacement is a real plane in
the shaders. How it works is in [rendering.md](../rendering.md); this records the decisions
and the measurements behind them, including the two I got wrong on the way.

## The near plane was never a cutting tool

Three things it could not do, and one it was blamed for that it did not do.

It is **perpendicular to the view by construction**, so you cannot cut along an anatomical
axis or a fitted plane, and the cut turns with the camera. It is **invisible**: nothing says
where it is, so you find it by overshooting. And it is **the depth buffer's own control**.

That last one produced the session's first real error. The proposal claimed cutting in
"degrades the depth buffer everywhere". It is the opposite. Depth resolution at distance Z
goes as Z²/near, so a larger near is better. Measured at the default framing — object radius
1, camera 3 away, 24-bit depth, resolution at the object centre:

| `nearClipRatio` | near | resolution |
|---|---|---|
| 1e-5 (the floor) | 0.00003 | 1.8e-2 r |
| 0.00333 (default) | 0.010 | 5.4e-5 r |
| 0.1 | 0.30 | 1.7e-6 r |
| 0.66 (cutting into the object) | 1.98 | 1.9e-7 r |

Cutting **in** improves precision 275×. Scrubbing back **out** toward the floor collapses it
to two percent of the object's radius, which is where a wireframe starts to shimmer against
its fill. That is the flicker that prompted the work, and it comes from the direction nobody
thinks about.

So the clipping plane is **not a depth-precision win** and must not be sold as one: holding
near at its default is much better than scrubbing it to the floor and worse than a deep
near-plane cut. What is gained is a cut that faces any way, stays put, says where it is, and
is obeyed by picking, decorators and the selection overlay.

## Rulings

**The OpenGL floor moves to 3.2 core.** Measured against this tree's `qsb` (Qt 6.11.2): a
one-line vertex shader writing `gl_ClipDistance[0]` compiles cleanly for SPIR-V, GLSL 150,
HLSL 50 (`SV_ClipDistance0`) and MSL 12 (`[[clip_distance]]`), and produces `#version 120`
… `out float gl_ClipDistance[1];` for the two legacy GLSL profiles Qt bakes by default,
which reject a global `out`. SPIRV-Cross downgrades ordinary varyings correctly for those
profiles — the pre-existing `fill_smooth.vert` proves it — but there is no legacy lowering
for a clip distance, because none exists. **`qsb` exits 0 and warns about nothing**, so the
breakage would have surfaced only when the OpenGL backend compiled the shader at run time,
on the machines least able to report why. `GLSL "150"` on `qt_add_shaders` is the whole fix.
This closes the door on a WebGL build without further work — WebGL 1 *is* GLSL 100 es and
WebGL 2 needs `EXT_clip_cull_distance`, which browsers do not all expose — but nothing in
this tree targets WebAssembly, and the README advertises Metal, Vulkan and Direct3D 12.

**The plane rides in the per-mesh uniform slice, not a new binding.** The first design put it
in a second block at binding 6, because the 352-byte main block is declared as a *prefix* by
the shaders that use it — `fill_wire.vert` declared 3 of its 15 fields, `selection_mask.vert`
one — and appending forces each of them to spell out the whole prefix. Implementing it showed
the trade the other way round: padding is nine mechanical copy-pastes, while binding 6 needs
a new buffer, a second dynamic offset threaded through every `setShaderResources` call, and
twelve SRBs rebuilt. The clincher was conceptual and had been under-weighted: the plane is
transformed into *mesh-local* space, so it is per-mesh data with exactly the lifetime and
cadence of `mvp`, and belongs in the slice beside it.

**No per-layer exemption, no raster mode, no numeric custom normal, no stencil cap.** The
first two were asked and declined; the third was offered and declined once `Freeze to View`
and `Alt`+drag existed. A stencil cap is exact for a closed manifold and garbage for
anything else, which is a poor default for an archive of scans and open surfaces — MeshLab
renders two-sided, so a cut shows the lit interior of the far shell, and the rim marks where
the plane is. A slab (two parallel planes) is `gl_ClipDistance[1]` and one more `vec4` if
anyone wants it.

**`Alt`+drag, not `Ctrl`+drag.** `Ctrl` would have read better beside `Ctrl`+wheel, but it
collides twice: macOS turns `Ctrl`+click into a right-click before Qt sees it, and
`ViewTrackball::mousePress` already treats `Ctrl`+Left as pan.

## Two bugs the tests caught, and one they did not

**The flip moved the cut.** Folding `clipPlaneFlipped` into the normal before applying the
offset mirrored the plane's position as well as swapping sides, so asking for the other half
of an object moved the cut off it. The offset is now measured along the unflipped normal and
the flip negates the finished plane. Caught by a unit test written before the behaviour was
tried by hand, and confirmed through the render path.

**The wheel wasted eight notches.** Enabling clipping at "half a diagonal back" is clear of
any scene, because a box's diagonal exceeds its extent along any one direction — which is
exactly why it is the wrong place to start: the gesture did nothing until the plane reached
the object. `ClipPlane::offsetClearOfScene()` computes where the plane actually touches.

**The rim's band spreads at tangency**, because marking a surface by its distance to a plane
inherently catches a large patch where the two are nearly parallel. It is not an `fwidth`
artefact and was not fixed: measured at the offset where the plane is about to touch a framed
sphere, it is **four pixels out of 57600**. A uniform to bound it would cost more than it
buys.

## How it was verified

The render path cannot be linked from a Qt test, so `tests/render_probes/clip_plane/` drives
it through the application — 19 checks on a rendered sphere. `tests/test_clipplane.cpp` pins
the arithmetic in 21 more.

Both probe failures on the final run were the probe, not the code: the rim legitimately
changes pixels in two cases that had been asserted pixel-identical (4 and 76 of them), and
the magenta detector looked for the pure rim colour when the rim is *blended* over grey —
peak excess 175, but green never falls below 90. The geometry checks now run with the rim
off and the rim is checked on its own.
