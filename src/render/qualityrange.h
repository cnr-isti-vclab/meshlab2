#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

// Which scalar a view colors by or charts: the per-vertex, per-face or per-edge one.
enum class QualityElement {
    None = 0,
    Vertex,
    Face,
    Edge
};

struct RenderQualityRange {
    float minV = 0.0f;
    float maxV = 1.0f;
    bool valid = false;
};

inline RenderQualityRange fixedRenderQualityRange(float minV, float maxV)
{
    if (!std::isfinite(minV) || !std::isfinite(maxV))
        return {};
    if (minV > maxV)
        std::swap(minV, maxV);
    return { minV, maxV, true };
}

inline RenderQualityRange sampledRenderQualityRange(
    std::vector<float> values,
    bool centerOnZero,
    float percentileCrop)
{
    values.erase(
        std::remove_if(values.begin(), values.end(), [](float v) { return !std::isfinite(v); }),
        values.end());
    if (values.empty())
        return {};

    float minV = 0.0f;
    float maxV = 1.0f;
    percentileCrop = std::clamp(percentileCrop, 0.0f, 0.5f);
    if (percentileCrop > 0.0f && values.size() > 1) {
        const int lastIndex = int(values.size()) - 1;
        const int loIndex = std::clamp(
            int(std::floor(percentileCrop * float(lastIndex))),
            0,
            lastIndex);
        const int hiIndex = std::clamp(
            int(std::ceil((1.0f - percentileCrop) * float(lastIndex))),
            0,
            lastIndex);
        const size_t lo = size_t(std::min(loIndex, hiIndex));
        const size_t hi = size_t(std::max(loIndex, hiIndex));
        std::nth_element(values.begin(), values.begin() + lo, values.end());
        minV = values[lo];
        std::nth_element(values.begin(), values.begin() + hi, values.end());
        maxV = values[hi];
    } else {
        const auto minMax = std::minmax_element(values.begin(), values.end());
        minV = *minMax.first;
        maxV = *minMax.second;
    }

    if (centerOnZero) {
        const float absMax = std::max(std::abs(minV), std::abs(maxV));
        minV = -absMax;
        maxV = absMax;
    }

    return { minV, maxV, true };
}

inline float normalizedRenderQuality(float q, const RenderQualityRange &range)
{
    if (!range.valid || !std::isfinite(q))
        return 0.5f;
    const float den = range.maxV - range.minV;
    if (std::abs(den) <= 1e-12f)
        return 0.5f;
    return std::clamp((q - range.minV) / den, 0.0f, 1.0f);
}
