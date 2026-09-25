# Preferences

Application-wide settings reuse the filter-parameter machinery end to end: they are
**declared** with the same JSON schema, **rendered** by the same widget builder, and
**read** through one registry.

```
resources/preferences.json   declaration (same schema as any filters.json "parameters")
        │
        ├─ FilterDescriptorLoader::loadParameters()  →  MeshFilterParameterDescriptor
        │
        ├─ Preferences (src/core)      values + QSettings persistence + changed() signal
        │
        └─ ParameterFormBuilder (src/ui)   descriptors → editors, shared with the filter panel
                    │
                    └─ PreferencesDialog     ~60 lines; owns no widget knowledge
```

Adding a preference means adding a JSON entry and reading it back. There is no UI code
to write.

Current implementation status: `resources/preferences.json` declares preferences across
`view`, `input`, `render`, `scalar`, `log`, `document`, and `advanced`.

The two memory-related document preferences are intentionally conservative:
`document.undoMemoryLimitMiB` defaults to `0` (disabled), and
`document.purgeUndoOnMemoryPressure` defaults to `false`. See
[Memory Accounting](memory_accounting.md) for their pruning semantics.

## Declaring one

Entries in `resources/preferences.json` use exactly the parameter schema documented in
[Adding a Filter](adding_a_filter.md) — `id`, `label`, `help`, `group`, `type`,
`default`, `min`/`max`, `enumOptions`. Ids are dotted and namespaced by group
(`view.fieldOfView`), because the id doubles as the QSettings key.

## Reading one

```cpp
#include "preferences.h"

const int chunks = Preferences::instance().intValue(QStringLiteral("advanced.rayCallbackChunks"));
```

A read falls back to the declared default when the value has never been set, so a call
site reads as a drop-in replacement for the constant it replaces. `Preferences::changed`
fires after a new value is stored, for consumers that need to react live.

Values are written to QSettings the moment they change — there is no OK/Cancel — and
stored values are only adopted for ids that are still declared, so deleting an entry
from the JSON leaves no stale key behind.

## What belongs here

This is the part that matters, because most constants in the codebase should **not**
become preferences. Four categories, three of which have a different fix:

| Kind | Example | Where it belongs |
|---|---|---|
| Structural invariant | `kUbufSize`, vertex strides, UBO offsets | stays a constant — exposing it only lets a user corrupt the renderer |
| Should be *derived* | the Embree ray epsilon | compute it from the data; see [embree ray epsilon](../../vcglib/wrap/embree/EmbreeAdaptor.h). Exposing it as a knob would have shipped a bug with a dial on it |
| Per-invocation algorithm knob | xatlas placement attempts, texture-defrag permutation limit | the owning filter's `filters.json` |
| Genuine user preference | field of view, gizmo size, default color map | **here** |

If a value must be right rather than chosen, it is not a preference.

## Related

`ParameterFormBuilder` is the shared piece and is independently testable
(`tests/test_parameterform.cpp`) precisely because it depends on nothing but
`Document` and Qt Widgets. Its `Context` is optional: a caller with no document — this
dialog — gets a working form, and the document-coupled parameter types (mesh, texture,
camera/render state) are skipped rather than built half-working.

The obvious next step is migrating `RenderOverlayPanel`'s hand-rolled controls onto the
same builder; that is where the remaining duplication lives.
