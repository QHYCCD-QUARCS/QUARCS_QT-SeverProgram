#include "GuidingStarDetector.h"

#include "../tools.h"

#include <algorithm>
#include <cmath>

namespace guiding {

double GuidingStarDetector::maxADUForMat(const cv::Mat& img)
{
    // 以位深推断最大 ADU
    if (img.depth() == CV_8U)  return 255.0;
    if (img.depth() == CV_16U) return 65535.0;
    // 对 float 类型，后续会用实际峰值做“相对饱和”不可靠，这里仍返回 1.0
    return 1.0;
}

double GuidingStarDetector::localPeakADU(const cv::Mat& img, double x, double y, int halfSizePx)
{
    if (img.empty()) return 0.0;
    const int cx = static_cast<int>(std::llround(x));
    const int cy = static_cast<int>(std::llround(y));
    const int x0 = std::max(0, cx - halfSizePx);
    const int y0 = std::max(0, cy - halfSizePx);
    const int x1 = std::min(img.cols - 1, cx + halfSizePx);
    const int y1 = std::min(img.rows - 1, cy + halfSizePx);

    double peak = 0.0;
    for (int yy = y0; yy <= y1; ++yy)
    {
        for (int xx = x0; xx <= x1; ++xx)
        {
            double v = 0.0;
            switch (img.depth())
            {
            case CV_8U:  v = static_cast<double>(img.at<uint8_t>(yy, xx)); break;
            case CV_16U: v = static_cast<double>(img.at<uint16_t>(yy, xx)); break;
            case CV_32F: v = static_cast<double>(img.at<float>(yy, xx)); break;
            default:     v = static_cast<double>(img.at<uint16_t>(yy, xx)); break;
            }
            if (v > peak) peak = v;
        }
    }
    return peak;
}

std::optional<StarCandidate> GuidingStarDetector::selectGuideStar(const cv::Mat& image16,
                                                                  const StarSelectionParams& p,
                                                                  std::vector<StarCandidate>* outCandidates) const
{
    if (image16.empty() || image16.cols <= 0 || image16.rows <= 0)
        return std::nullopt;

    // Step0：峰值检测 + 质心（复用现有 C++ 实现）
    auto stars = Tools::DetectFocusedStars(image16, p.kSigma, p.minArea, p.maxArea, p.detectMinSNR);
    if (stars.empty())
        return std::nullopt;

    const double aduMax = maxADUForMat(image16);
    const double nearSat = aduMax * p.nearSaturationRatio;

    std::vector<StarCandidate> candidates;
    candidates.reserve(stars.size());

    for (const auto& s : stars)
    {
        StarCandidate c;
        c.x = s.x;
        c.y = s.y;
        c.snr = s.snr;
        c.hfd = s.hfr * 2.0; // HFD = 2*HFR
        c.edgeDistPx = std::min({c.x, c.y, (double)(image16.cols - 1) - c.x, (double)(image16.rows - 1) - c.y});
        c.peakADU = localPeakADU(image16, c.x, c.y, 4);
        candidates.push_back(c);
    }

    if (outCandidates)
        *outCandidates = candidates;

    // Step1：SNR 过滤（区分星点与噪点）
    std::vector<StarCandidate> s1;
    s1.reserve(candidates.size());
    for (const auto& c : candidates)
        if (c.snr >= p.minSNR)
            s1.push_back(c);

    // Step2：HFD 范围过滤（热像素/团块）
    std::vector<StarCandidate> s2;
    s2.reserve(s1.size());
    for (const auto& c : s1)
        if (c.hfd >= p.minHFD && c.hfd <= p.maxHFD)
            s2.push_back(c);

    // Step3：饱和 + 边缘过滤
    std::vector<StarCandidate> s3;
    s3.reserve(s2.size());
    for (const auto& c : s2)
    {
        if (c.edgeDistPx < p.edgeMarginPx)
            continue;
        // 仅对 8/16bit 做饱和判定（float 的 nearSat 没意义）
        if ((image16.depth() == CV_8U || image16.depth() == CV_16U) && c.peakADU >= nearSat)
            continue;
        s3.push_back(c);
    }

    if (s3.empty())
        return std::nullopt;

    // 计算图像中心
    const double centerX = image16.cols / 2.0;
    const double centerY = image16.rows / 2.0;
    const double maxCenterDist = std::sqrt(centerX * centerX + centerY * centerY);

    // 评分：优先 SNR，其次靠近中心，其次远离边缘，最后 HFD 更接近中间
    const double hfdMid = (p.minHFD + p.maxHFD) * 0.5;
    auto score = [&](const StarCandidate& c) {
        // 计算到图像中心的距离
        const double dx = c.x - centerX;
        const double dy = c.y - centerY;
        const double centerDist = std::sqrt(dx * dx + dy * dy);
        // 中心距离奖励：距离中心越近，奖励越高（归一化到 0-1）
        const double centerDistReward = (maxCenterDist > 0.0) ? (1.0 - centerDist / maxCenterDist) : 0.0;
        
        double hfdPenalty = std::abs(c.hfd - hfdMid);
        // 增加边缘距离权重，确保更严格地排除边缘星点
        // 增加中心距离奖励，优先选择靠近中心的星点
        return c.snr * 1.0 + c.edgeDistPx * 0.02 + centerDistReward * 0.5 - hfdPenalty * 0.2;
    };

    auto bestIt = std::max_element(s3.begin(), s3.end(),
                                   [&](const StarCandidate& a, const StarCandidate& b) {
                                       return score(a) < score(b);
                                   });
    return *bestIt;
}

} // namespace guiding



