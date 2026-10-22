#include "interactivetool.h"

#include "tools/rubberbandselecttool.h"
#include "tools/measuretool.h"
#include "tools/transformtool.h"

std::vector<std::unique_ptr<InteractiveTool>> createBuiltinInteractiveTools()
{
    std::vector<std::unique_ptr<InteractiveTool>> tools;
    tools.push_back(std::make_unique<RubberBandSelectTool>());
    tools.push_back(std::make_unique<MeasureTool>());
    tools.push_back(std::make_unique<TransformTool>());
    return tools;
}
