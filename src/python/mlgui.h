#pragma once

#include <nanobind/nanobind.h>

#include <string>

class RenderWidget;

// Desktop GUI access object exposed to the embedded Python console as `mlgui`.
// Provides view state, camera, and rendering methods that are only meaningful
// in the context of a running MeshLab application with a visible viewport.
//
// In headless pymeshlab this object does not exist — only in the desktop app's
// Python console (where QApplication + MainWindow + RenderWidget are alive).
class MlGui
{
public:
    explicit MlGui(RenderWidget *view = nullptr);
    ~MlGui() = default;

    // View state access
    std::string cameraStateJson() const;
    std::string renderStateJson() const;
    bool applyCameraStateJson(const std::string &json, std::string *error = nullptr);
    bool applyRenderStateJson(const std::string &json, std::string *error = nullptr);

    // Offscreen render using the live RenderWidget.
    nanobind::bytes renderSnapshot(const std::string &renderStateJson,
                                   int width, int height);

    // Convenience: render and save directly to a PNG file.
    // If renderStateJson is empty, the current view state is used.
    void saveSnapshot(const std::string &path,
                      int width, int height,
                      const std::string &renderStateJson = {});

    // View lifecycle
    void setRenderWidget(RenderWidget *view);
    RenderWidget *renderWidget() const { return m_view; }
    bool hasValidView() const { return m_view != nullptr; }

private:
    RenderWidget *m_view = nullptr;
};
