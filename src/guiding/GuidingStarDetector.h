#pragma once

#include <opencv2/core/core.hpp>
#include <optional>
#include <vector>

namespace guiding {

struct StarCandidate
{
    double x = 0.0;
    double y = 0.0;
    double snr = 0.0;
    double hfd = 0.0;       // Half-Flux Diameter（像素）
    double peakADU = 0.0;   // 局部峰值（原始 ADU）
    double edgeDistPx = 0.0;
};

struct StarSelectionParams
{
    double minSNR = 10.0;
    double minHFD = 1.5;
    double maxHFD = 12.0;
    double nearSaturationRatio = 0.9; // 90% 饱和
    // 室外更稳：边缘星点更容易受畸变/抖动/裁切影响，提升默认边缘剔除门槛
    double edgeMarginPx = 40.0;

    // DetectFocusedStars 参数
    double kSigma = 3.5;
    int minArea = 3;
    int maxArea = 200;
    double detectMinSNR = 3.0;
};

class GuidingStarDetector
{
public:
    // 三遍扫描：SNR → HFD → 饱和 + 边缘，最后按评分选最佳星
    std::optional<StarCandidate> selectGuideStar(const cv::Mat& image16,
                                                 const StarSelectionParams& p,
                                                 std::vector<StarCandidate>* outCandidates = nullptr) const;

private:
    static double maxADUForMat(const cv::Mat& img);
    static double localPeakADU(const cv::Mat& img, double x, double y, int halfSizePx);
};

} // namespace guiding



