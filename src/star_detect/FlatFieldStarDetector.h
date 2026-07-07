#pragma once

#include "StarDetectParams.h"

#include <opencv2/core/core.hpp>

namespace star_detect {

class FlatFieldStarDetector
{
public:
    DetectionResult detect(const cv::Mat& image16,
                           const ImageContext& imageContext,
                           const DetectParams& detectParams,
                           const SearchRegion& searchRegion = {}) const;

    cv::Mat generateFlatField(const cv::Mat& image16,
                              int kernelSize,
                              const std::string& method,
                              double sigma) const;

    cv::Mat generateFlatSubtracted(const cv::Mat& image16,
                                   int kernelSize,
                                   const std::string& method,
                                   double sigma) const;
};

} // namespace star_detect
