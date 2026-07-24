#ifndef DOODLE_BRUSH_RENDER_H
#define DOODLE_BRUSH_RENDER_H

namespace Doodle
{

static const int BRUSH_RENDER_DITHER_SHAPE = 2;
static const int BRUSH_RENDER_SPRAY_SHAPE = 6;

inline bool brushUsesSparseSampling(int shape)
{
    return shape == BRUSH_RENDER_DITHER_SHAPE ||
           shape == BRUSH_RENDER_SPRAY_SHAPE;
}

inline int brushStrokeSpacing(int sizeTenths, int shape)
{
    if (!brushUsesSparseSampling(shape))
        return 1;

    const int spacing = (sizeTenths + 19) / 20;
    return spacing < 3 ? 3 : spacing;
}

inline int brushStrokeSegmentSteps(int deltaX, int deltaY,
                                   int sizeTenths, int shape)
{
    const int absX = deltaX < 0 ? -deltaX : deltaX;
    const int absY = deltaY < 0 ? -deltaY : deltaY;
    const int distance = absX > absY ? absX : absY;
    const int spacing = brushStrokeSpacing(sizeTenths, shape);
    const int steps = (distance + spacing - 1) / spacing;
    return steps < 1 ? 1 : steps;
}

inline int fractionalBrushCoverageTenths(bool lowerContains,
                                         bool upperContains,
                                         bool lowerBoundary,
                                         int fraction,
                                         int shape,
                                         int pixelX,
                                         int pixelY,
                                         bool feather,
                                         bool centerPixel)
{
    if (lowerContains)
        return feather && lowerBoundary && !centerPixel ? 7 : 10;
    if (!upperContains || fraction <= 0)
        return 0;

    if (feather)
        return brushUsesSparseSampling(shape) && fraction > 7 ? 7 : fraction;

    static const int bayer4[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5},
    };
    const int transitionThreshold =
        (bayer4[pixelY & 3][pixelX & 3] * 10) / 16;
    return transitionThreshold < fraction ? 10 : 0;
}

inline unsigned char blendBrushChannel(unsigned char existing,
                                       unsigned char desired,
                                       int coverageTenths)
{
    if (coverageTenths <= 0)
        return existing;
    if (coverageTenths >= 10)
        return desired;
    return static_cast<unsigned char>(
        (desired * coverageTenths +
         existing * (10 - coverageTenths) + 5) / 10);
}

} // namespace Doodle

#endif
