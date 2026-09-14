# Vendored PoissonRecon Provenance

This document records where the vendored `PoissonRecon` source subtree
came from, which vendored files were modified locally, and how to refresh it.

## Upstream source

- Upstream repository: `https://github.com/mkazhdan/PoissonRecon`
- Local reference clone used for comparison: `.reference/PoissonRecon`
- Vendored subtree in MeshLab: `plugins/filter_screened_poisson/Src`
- Current reference commit:

```text
cd6dc7d33f028b2e6496f5cd999c25cecd56aff2
```

The vendored subtree was copied from `.reference/PoissonRecon/Src` and then kept
as close as possible to the reference source. MeshLab-specific integration code is kept
outside the vendored subtree.

## MeshLab integration files outside the vendored subtree

These files are intentionally separate from `Src` so future refreshes
remain easy to review:

- `plugins/filter_screened_poisson/poissonrecon_backend.h`
- `plugins/filter_screened_poisson/poissonrecon_backend.cpp`
- `plugins/filter_screened_poisson/poissonrecon_adapter.h`
- `plugins/filter_screened_poisson/poissonrecon_adapter.cpp`
- `plugins/filter_screened_poisson/screenedpoissonfilterplugin.cpp`
- `plugins/filter_screened_poisson/CMakeLists.txt`

## Vendored files modified locally

At the moment, two files inside `Src` differ from the reference clone:

- `plugins/filter_screened_poisson/Src/MultiThreading.h`
- `plugins/filter_screened_poisson/Src/FEMTree.h`

### Thread-count setter (`MultiThreading.h`)

Purpose: expose a small setter for the PoissonRecon thread-count static so MeshLab
can control the solver thread pool from the filter parameter/UI.

Current local change:

```diff
+ static void SetNumThreads( unsigned int n ){ _NumThreads = n>0 ? n : 1; }
```

Rationale:

- MeshLab needs to map the filter `threads` parameter to the PoissonRecon backend.
- Keeping this as a one-line vendored patch is simpler and easier to reapply than
  moving thread-pool control into a larger fork.

### MSVC template-parser compatibility (`FEMTree.h`)

Purpose: allow the vendored source to compile with the conforming MSVC mode
propagated by Qt (`/permissive-`).

Current local change: parenthesize the four recursive SFINAE conditions that use
`D<Dim`, for example:

```diff
- typename std::enable_if< D< Dim >::type
+ typename std::enable_if< (D<Dim) >::type
```

Rationale:

- MSVC 14.44 otherwise reports `C2988`/`C2059` while parsing these member-template
  overloads.
- This is a syntax-only disambiguation; it does not alter the recursion or runtime
  behavior.
- The form matches existing parenthesized conditions elsewhere in the same upstream
  file and keeps MeshLab's normal conforming compiler mode enabled.

## How to verify current local differences

From the MeshLab repo root:

```bash
diff -rq .reference/PoissonRecon/Src plugins/filter_screened_poisson/Src
```

For full patches of the locally modified vendored files:

```bash
diff -u .reference/PoissonRecon/Src/MultiThreading.h \
  plugins/filter_screened_poisson/Src/MultiThreading.h
diff -u .reference/PoissonRecon/Src/FEMTree.h \
  plugins/filter_screened_poisson/Src/FEMTree.h
```

Or use the helper script:

```bash
plugins/filter_screened_poisson/check_upstream_status.sh
```

## Recommended update workflow

When a newer `PoissonRecon` version needs to be integrated:

1. Update the local reference clone:

```bash
git -C .reference/PoissonRecon fetch
git -C .reference/PoissonRecon checkout <new-commit-or-tag>
```

2. Compare the current vendored subtree against the updated reference:

```bash
plugins/filter_screened_poisson/check_upstream_status.sh
```

3. Refresh the vendored subtree from the new reference copy.
   - Prefer copying `Src` cleanly from `.reference/PoissonRecon/Src`
   - Reapply the minimal local vendored patches in `Src/MultiThreading.h` and
     `Src/FEMTree.h`

4. Rebuild MeshLab and retest:

```bash
cmake --build build-release --target MeshLab2 -j4
```

5. Update this file:
   - replace the reference commit hash
   - confirm the list of locally modified vendored files is still correct
   - adjust the patch summary if needed

## Maintenance rule

Try to keep all future MeshLab-specific changes out of `Src` unless a
small vendored patch is clearly preferable. When a vendored patch is necessary,
record it here immediately.
