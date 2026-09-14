# Vendored: bernhardmgruber/bpa

Source: <https://github.com/bernhardmgruber/bpa>, commit `6b004ac` (2022-09-05).
Licence: Boost Software License 1.0, `upstream/LICENSE.txt`.

Vendored files: `src/lib/bpa.h`, `src/lib/bpa.cpp`, `src/lib/IO.h`, `LICENSE.txt`.
The CLI driver and the Catch2 tests are not vendored.

**The tree is unpatched.** Everything MeshLab needs is done on the outside, in
`bpafilterplugin.cpp`.

## Why this one, next to vcglib's

vcglib's `BallPivoting` is the elaborated implementation: it re-seeds so disconnected
components are all covered, guesses a radius, and takes a clustering radius and a crease
angle. This one is a direct, unelaborated reading of Bernardini et al. 1999 -- one seed, one
radius, no clustering, no crease handling -- and the two behave very differently on the same
cloud, which is the point of having both. Read the differences off the two filters' help
text; neither is a drop-in for the other.

## Notes for anyone touching this

**C++20.** `bpa.cpp` uses `std::numbers::pi_v` in exactly one place (line 248), which is the
tree's only requirement above C++17. Rather than patch it, the CMake target asks for
`cxx_std_20`; nothing else in MeshLab is affected, and the vendored file stays pristine.

**It hands back loose triangles.** `reconstruct()` returns `std::vector<Triangle>`, three
bare `glm::vec3` each -- no shared vertices and no indices. Welding those by position would
throw away the correspondence to the input points, which is the one thing ball pivoting is
valued for. The plugin instead maps each returned position back to its input vertex by exact
float equality, which is sound because the positions are copied through the algorithm
verbatim, never computed. The filter reports it if any position fails to map, which would
mean that assumption had broken.

**It writes to the console.** `reconstruct()` prints "No seed triangle found" to `stderr`
when it fails to start. The plugin detects the empty result and reports it properly, so the
stray line is cosmetic.

**It has a debug switch that writes files to the working directory.** `constexpr auto debug`
in `bpa.cpp` is `false`, and the `saveTriangles`/`savePoints` calls behind it are ordinary
`if` statements, so they compile but never run. Do not turn it on in a build that ships:
those calls drop `seed.stl`, `current_mesh.stl`, `boundaryEdges.stl` and similar into
whatever directory the process happens to be in.

**glm.** The vendored headers are written against glm, which is declared in `vcpkg.json` for
this plugin alone.
