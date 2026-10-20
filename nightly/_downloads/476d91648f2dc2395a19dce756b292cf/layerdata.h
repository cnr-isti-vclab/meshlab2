#pragma once

#include <QString>

#include <cstddef>
#include <memory>

// Plugin-owned data attached to a layer.
//
// Some algorithms are naturally several filters over one shared intermediate result -- an
// abstract domain, a solver state, a computed correspondence -- and the intermediate is a
// plugin's own C++ type that Core has no business understanding. A plugin subclasses this,
// installs an instance on the layer with Document::setLayerData(), and its later filters
// fetch it back.
//
// **Instances are immutable once installed.** That is what makes undo correct rather than
// merely cheap: a snapshot holds a shared_ptr to the very object that existed at capture,
// so it cannot drift from what the plugin later does. To change its data a plugin builds a
// new instance and installs it, which is copy-on-write; Core never copies one, and no
// plugin code runs on the undo or redo path. The `const` in LayerDataPtr enforces it.
class LayerData
{
public:
    virtual ~LayerData() = default;

    // One short line for the layer panel: what this is, and enough of its size or state to
    // be worth showing. It is the only thing that tells a user the data is there at all.
    virtual QString describe() const = 0;

    // Whether the data outlives a change to its layer's geometry. Most derived data does
    // not -- an abstract domain means nothing once the vertices move -- so the default is
    // to be dropped, and a plugin has to say otherwise deliberately.
    virtual bool survivesGeometryChange() const { return false; }

    // Rough footprint, for the document's memory accounting and the undo budget. Zero
    // means "not accounted", which is honest for something small.
    virtual std::size_t approximateBytes() const { return 0; }
};

// Immutable and shared: several undo snapshots, and several layers after a duplicate, all
// point at one instance.
using LayerDataPtr = std::shared_ptr<const LayerData>;
