#pragma once

#include <QString>
#include <QVariantMap>
#include <Qt>

#include <optional>

// The selection tool's editing keys: one press runs one ordinary selection filter, so each is
// a single undo step and is recorded as a script call like any other filter run.
//
//   A  select all          N  select none        I  invert the selection
//   +  grow (dilate)       -  shrink (erode)     X  delete the selection
//
// Every one acts on the kind of element the tool is currently picking -- faces, vertices or
// edges -- which is why it is an argument and not left to the filter's defaults: those act on
// every kind, polyline edges included (select all and select none), act on whichever kind
// already has something selected (invert, which would do nothing to an empty selection), or
// work from faces and clear a vertex selection (dilate and erode, before they took an
// `element`).
//
// Kept in a header of its own, free of any widget, so a test can run exactly what the tool
// would send without a view to send it from.
// What the selection tool picks. Edges are both kinds: polyline edges and triangle sides.
enum class SelectionElement { Face, Vertex, Edge };

struct SelectionShortcut
{
    QString filterId;     // in the select plugin: "meshlab2.filter.select::" + filterId
    QVariantMap params;
};

inline std::optional<SelectionShortcut> selectionShortcutFor(int key, SelectionElement picked)
{
    const bool faces = picked == SelectionElement::Face;
    const bool edges = picked == SelectionElement::Edge;
    const QString element = faces ? QStringLiteral("face")
                            : edges ? QStringLiteral("edge")
                                    : QStringLiteral("vertex");
    // In edge mode every key but X reaches polyline edges only: the set-operation filters
    // have no option for triangle sides. Deleting has no edge form at all.
    if (edges && key == Qt::Key_X)
        return std::nullopt;
    switch (key) {
    case Qt::Key_A:
        return SelectionShortcut{ QStringLiteral("select_all"),
            { { QStringLiteral("allFaces"), faces }, { QStringLiteral("allVerts"), !faces && !edges },
              { QStringLiteral("allEdges"), edges } } };
    case Qt::Key_N:
        return SelectionShortcut{ QStringLiteral("select_none"),
            { { QStringLiteral("allFaces"), faces }, { QStringLiteral("allVerts"), !faces && !edges },
              { QStringLiteral("allEdges"), edges } } };
    case Qt::Key_I:
        return SelectionShortcut{ QStringLiteral("invert_selection"),
            { { QStringLiteral("InvFaces"), faces }, { QStringLiteral("InvVerts"), !faces && !edges },
              { QStringLiteral("InvEdges"), edges } } };
    case Qt::Key_Plus:
    case Qt::Key_Equal:   // the same key as + on a layout where + needs Shift
        return SelectionShortcut{ QStringLiteral("dilate_selection"),
            { { QStringLiteral("element"), element } } };
    case Qt::Key_Minus:
        return SelectionShortcut{ QStringLiteral("erode_selection"),
            { { QStringLiteral("element"), element } } };
    case Qt::Key_X:
        // Deleting faces also removes the vertices that no face uses any more, and that
        // filter works the vertex set out for itself, so a stale vertex selection cannot
        // make it delete the wrong ones. Deleting vertices takes their faces with them.
        return SelectionShortcut{
            faces ? QStringLiteral("remove_selected_faces_and_vertices")
                  : QStringLiteral("remove_selected_vertices"),
            {} };
    default:
        return std::nullopt;
    }
}
