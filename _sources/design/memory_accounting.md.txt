# Memory Accounting

See also: [Architecture](architecture.md) · [Data Model](data_model.md) · [Rendering](rendering.md) · [Preferences](preferences.md)

## Why There Are Separate Numbers

`Help > Memory Info` reports three metrics with different meanings. They must not be
added together or expected to match one another exactly:

- **OS process memory** is queried from the operating system. On macOS the primary
  value is the kernel's physical-footprint ledger, the same accounting family used by
  Activity Monitor. Linux reports resident and private mappings from `/proc`; Windows
  reports working-set and private bytes.
- **Tracked CPU allocations** are a lower-bound ownership estimate for data structures
  MeshLab can inspect directly.
- **Logical mesh GPU cache** sums QRhi buffer sizes and known RGBA8 texture payloads in
  `MeshGpuResourceCache`. It does not include per-view render targets, driver allocation
  granularity, command/staging resources, or all graphics memory charged to the process.

The dialog uses binary units (`KiB`, `MiB`, `GiB`) and provides exact bytes in tooltips.
`Copy JSON` exports the same snapshot as `org.meshlab.memory-report.v1`, including the
process id, timestamp, and the separate CPU/GPU subtotals.

## Tracked CPU Data

Live mesh accounting uses vector **capacity**, not size, for vertices, edges, faces, and
VCG optional-component vectors. It also estimates named VCG custom-attribute backing
storage from attribute element size and the owning container capacity.

Live `QImage` storage is counted for mesh texture assets, material texture assets, and
raster planes. QImage copies are implicit shares, so backing pointers are de-duplicated
before summing.

The tracked subtotal intentionally excludes Qt/plugin/Python object heaps, allocator
metadata and retained pages, thread stacks, libraries, temporary filter/import buffers,
per-view rendering allocations, and backend/driver overhead. This is why the OS physical
footprint remains the authoritative whole-process comparison value.

## Undo Memory

`DocumentUndoManager::memoryStats()` reports reclaimable history ownership rather than
adding every node's logical state:

- immutable geometry snapshots are de-duplicated by shared `VCGMesh` pointer;
- packed before/after selection-delta vector capacities are included;
- raster and texture images are de-duplicated, and image storage still shared with the
  live document is excluded because clearing history cannot release it;
- resources unique to a currently open undo operation are reported as pending memory.

Current-path rows in the dialog are explicitly marked **not additive**. Full states can
reference geometry shared with many other nodes, while selection-delta rows own their
small packed vectors directly.

## Automatic Pruning

Two preferences control automatic history release:

- `document.undoMemoryLimitMiB`: tracked undo-memory ceiling. `0` disables the byte
  budget, which is the default. Crossing a non-zero ceiling prunes toward 80% of it.
- `document.purgeUndoOnMemoryPressure`: opts into OS pressure handling. Warning pressure
  prunes only relative to the configured budget; critical pressure may clear all history.

Pruning first removes alternate branches, then advances the root through the oldest full
snapshots while preserving at least one transition when possible. Selection-delta nodes
never become roots because they do not contain a complete document state. If the newest
complete checkpoint alone exceeds an explicitly enabled budget, the history is cleared
rather than claiming that the limit was enforced. Pressure received during a filter or
other undo transaction is deferred until that operation closes. Every automatic purge is
written to the application log.

## External macOS Probe

For repeatable comparison against system tools, start MeshLab and run:

```bash
tools/memory_probe_macos.sh MeshLab
```

At each phase, open `Help > Memory Info`, click `Copy JSON`, and enter a checkpoint label
in the probe. A useful sequence is `baseline`, `one-mesh`, `two-mesh`, `after-edit`, and
`undo-cleared`. The output directory stores the internal JSON beside:

- `footprint` text and JSON, including `phys_footprint`;
- `vmmap -summary`, including mapped/dirty/fragmentation summaries;
- `heap -s -H`, when process inspection permission allows it;
- `ps` RSS/virtual-size context.

Compare **deltas between stable checkpoints**, not only absolute values. Allow rendering
uploads to settle before capture and repeat a run when establishing a tolerance. External
footprint tests are diagnostic rather than strict CI assertions because allocator state,
graphics backend, and operating-system accounting vary. Exact unit tests cover MeshLab's
deterministic ownership counters and pruning behavior in `tests/test_document.cpp`.
