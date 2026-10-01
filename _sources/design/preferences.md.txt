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
                    └─ PreferencesDialog     ~75 lines; owns no widget knowledge
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

Write the `help` for a reader of the dialog: it is shown there as text under the row,
rendered as Markdown, and not only as a tooltip. Every preference declares one.

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

**Only overrides are stored.** A preference at its default has no QSettings key: setting
a value equal to the default, `resetToDefault(id)`, and `resetToDefaults()` all remove
the key rather than write the default into it, and a stored copy of the default found at
load is dropped. That keeps a user following the declared default, including a later
change to it. The old Restore Defaults wrote the defaults in, which pinned them: a user
who had once pressed it kept Gray as the default color map after the declared default
became Rainbow.

## The dialog

`PreferencesDialog` turns on three `ParameterFormBuilder` options that the filter panel
leaves off.

With **inline help**, each row's help is shown as text under it, in place of the tooltip
it gets in the filter panel. *Show help* hides and shows it (`setInlineHelpVisible`). The
choice is kept in QSettings as `preferencesDialog/showHelp`, outside the `preferences`
group: it is how the dialog looks, not a preference, so *Restore All Defaults* leaves it
alone.

With **reset buttons**, a button appears at the end of a row once its value differs from
the default, and puts that one value back; *Restore All Defaults* resets them all. When an
edit or a reset leaves a row at its default, the dialog calls `resetToDefault(id)` instead
of storing the value. The builder's `isDefault()` makes that call, with the same numeric
tolerance as the store (`sameParameterValue`), because an editor may round.

**Larger group headings** (`setGroupHeadingScale`) make the sections read as titles, since
they are the dialog's top level. In the filter panel the headings stay at the rows' size,
below the filter's bold title.

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
