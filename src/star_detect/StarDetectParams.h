#pragma once

#include "StarTypes.h"

#include <opencv2/core/core.hpp>

#include <optional>
#include <string>
#include <vector>

namespace star_detect {

struct ImageContext
{
    int imageWidth = 0;
    int imageHeight = 0;
    int bitDepth = 16;
    int channels = 1;

    int binningX = 1;
    int binningY = 1;

    int roiOriginX = 0;
    int roiOriginY = 0;

    double pixelScaleArcsecPerPixel = 0.0;
    bool isColor = false;
};

struct SearchRegion
{
    std::optional<cv::Rect> includeRect;
    std::vector<cv::Rect> excludeRects;
};

struct DetectParams
{
    int kernelSize = 64;
    std::string flatMethod = "uniform";
    double gaussianSigma = 0.0;

    double snrThreshold = 5.0;
    double minHFD = 1.5;
    double maxHFD = 6.0;
    double minHFR = 0.0;
    double maxHFR = 0.0;
    int minSeparationPx = 5;
    double edgeMarginPx = 40.0;
    double nearSaturationRatio = 0.9;

    int centroidHalfSize = 5;
    double centroidKSigma = 3.5;

    int maxCandidates = 100;
    bool computeHfr = false;
};

struct SelectionPolicy
{
    SelectionMode mode = SelectionMode::HighestSnr;

    double minSnr = 0.0;
    double maxHfd = 0.0;
    double maxHfr = 0.0;
    bool avoidSaturated = true;
    double minEdgeDistPx = 0.0;

    double preferredX = 0.0;
    double preferredY = 0.0;
    bool hasPreferredPoint = false;

    bool preferCentralStar = false;
    double centerWeight = 1.0;
    double snrWeight = 1.0;
    double hfrWeight = 1.0;
    double peakWeight = 0.0;
};

} // namespace star_detect
