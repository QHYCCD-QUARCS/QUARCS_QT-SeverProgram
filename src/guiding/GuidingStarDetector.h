#pragma once

#include <opencv2/core/core.hpp>
#include <QString>
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
    int searchRegionPx = 15;      // PHD2 DEFAULT_SEARCH_REGION
    int autoSelDownsample = 0;    // 0=Auto（PHD2 默认语义），1=disabled，2/3=固定 downsample
    double autoSelPixelScaleArcsecPerPixel = 0.0; // 用于 Auto downsample；<=0 时回退为 1x
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
    // 返回主星；outCandidates 若提供，则返回接近 PHD2 foundStars 语义的“已验证可用候选星”：
    // 已通过 HFD / 边缘 / 质心复核，且满足最小 SNR。多星副星应只从这批候选星里选。
    std::optional<StarCandidate> selectGuideStar(const cv::Mat& image16,
                                                 const StarSelectionParams& p,
                                                 const QString& fitsPath = QString(),
                                                 std::vector<StarCandidate>* outCandidates = nullptr,
                                                 std::vector<StarCandidate>* outRejectedCandidates = nullptr) const;

private:
    static double maxADUForMat(const cv::Mat& img);
    static double localPeakADU(const cv::Mat& img, double x, double y, int halfSizePx);
};

} // namespace guiding
