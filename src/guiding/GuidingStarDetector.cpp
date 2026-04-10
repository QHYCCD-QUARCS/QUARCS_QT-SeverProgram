#include "GuidingStarDetector.h"

#include "../tools.h"
#include "../Logger.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace guiding {

namespace {

std::string formatCandidateBrief(const StarCandidate& c)
{
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(2);
    oss << "x=" << c.x
        << " y=" << c.y
        << " snr=" << c.snr
        << " hfd=" << c.hfd
        << " edge=" << c.edgeDistPx
        << " peak=" << c.peakADU;
    return oss.str();
}

} // namespace

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
                                                                  const QString& fitsPath,
                                                                  std::vector<StarCandidate>* outCandidates) const
{
    if (image16.empty() || image16.cols <= 0 || image16.rows <= 0)
        return std::nullopt;

    // Step0：优先使用 StellarSolver 检星；若失败则回退到旧的 FocusedCpp 检星
    constexpr const char* kLogPrefix = "[AutoGuideSelect]";
    std::vector<StarCandidate> candidates;
    bool usedStellarSolver = false;

    if (!fitsPath.isEmpty())
    {
        const QList<FITSImage::Star> ssStars = Tools::FindStarsByStellarSolverFromFile(fitsPath, true, true);
        if (!ssStars.isEmpty())
        {
            usedStellarSolver = true;
            candidates.reserve(static_cast<size_t>(ssStars.size()));
            for (const auto& s : ssStars)
            {
                StarCandidate c;
                c.x = s.x;
                c.y = s.y;
                c.hfd = std::isfinite(s.HFR) ? (s.HFR * 2.0) : 0.0;
                // FITSImage::Star 在 StellarSolver 路径不保证直接提供 SNR；用 flux/HFD 构造稳定评分值。
                const double flux = std::max(0.0, static_cast<double>(s.flux));
                const double den = std::max(0.5, c.hfd);
                c.snr = flux / den;
                c.edgeDistPx = std::min({c.x, c.y, (double)(image16.cols - 1) - c.x, (double)(image16.rows - 1) - c.y});
                c.peakADU = localPeakADU(image16, c.x, c.y, 4);
                candidates.push_back(c);
            }
            Logger::Log(std::string(kLogPrefix) +
                            " base_detect engine=StellarSolver candidates=" + std::to_string(candidates.size()) +
                            " fits=" + fitsPath.toStdString(),
                        LogLevel::INFO, DeviceType::GUIDER);
        }
    }

    if (candidates.empty())
    {
        auto stars = Tools::DetectFocusedStars(image16,
                                               p.kSigma,
                                               p.minArea,
                                               p.maxArea,
                                               p.detectMinSNR,
                                               51,
                                               1.0,
                                               true,
                                               DeviceType::GUIDER,
                                               QString::fromUtf8(kLogPrefix));
        if (!stars.empty())
        {
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
            Logger::Log(std::string(kLogPrefix) +
                            " base_detect engine=FocusedCpp candidates=" + std::to_string(candidates.size()),
                        LogLevel::INFO, DeviceType::GUIDER);
        }
    }

    if (candidates.empty())
    {
        Logger::Log(std::string(kLogPrefix) +
                        " fail_at=base_detect reason=no_candidates_from_stellarsolver_and_fallback"
                        " | possible_steps=exposure_gain_or_focus",
                    LogLevel::INFO, DeviceType::GUIDER);
        return std::nullopt;
    }

    const double aduMax = maxADUForMat(image16);
    const double nearSat = aduMax * p.nearSaturationRatio;

    if (outCandidates)
        *outCandidates = candidates;

    // Step1：SNR 过滤（区分星点与噪点）
    std::vector<StarCandidate> s1;
    s1.reserve(candidates.size());
    std::vector<StarCandidate> rejectedSNR;
    for (const auto& c : candidates)
    {
        if (c.snr >= p.minSNR)
            s1.push_back(c);
        else if (rejectedSNR.size() < 3)
            rejectedSNR.push_back(c);
    }

    // Step2：HFD 范围过滤（热像素/团块）
    std::vector<StarCandidate> s2;
    s2.reserve(s1.size());
    std::vector<StarCandidate> rejectedHFD;
    for (const auto& c : s1)
    {
        if (c.hfd >= p.minHFD && c.hfd <= p.maxHFD)
            s2.push_back(c);
        else if (rejectedHFD.size() < 3)
            rejectedHFD.push_back(c);
    }

    // Step3：饱和 + 边缘过滤
    std::vector<StarCandidate> s3;
    s3.reserve(s2.size());
    std::vector<StarCandidate> rejectedEdge;
    std::vector<StarCandidate> rejectedSaturation;
    int rejectedEdgeCount = 0;
    int rejectedSaturationCount = 0;
    for (const auto& c : s2)
    {
        if (c.edgeDistPx < p.edgeMarginPx)
        {
            ++rejectedEdgeCount;
            if (rejectedEdge.size() < 3)
                rejectedEdge.push_back(c);
            continue;
        }
        // 仅对 8/16bit 做饱和判定（float 的 nearSat 没意义）
        if ((image16.depth() == CV_8U || image16.depth() == CV_16U) && c.peakADU >= nearSat)
        {
            ++rejectedSaturationCount;
            if (rejectedSaturation.size() < 3)
                rejectedSaturation.push_back(c);
            continue;
        }
        s3.push_back(c);
    }

    Logger::Log(std::string(kLogPrefix) +
                    " summary base=" + std::to_string(candidates.size()) +
                    " pass_snr=" + std::to_string(s1.size()) +
                    " pass_hfd=" + std::to_string(s2.size()) +
                    " pass_edge_sat=" + std::to_string(s3.size()) +
                    " reject_snr=" + std::to_string(candidates.size() - s1.size()) +
                    " reject_hfd=" + std::to_string(s1.size() - s2.size()) +
                    " reject_edge=" + std::to_string(rejectedEdgeCount) +
                    " reject_sat=" + std::to_string(rejectedSaturationCount) +
                    " engine=" + std::string(usedStellarSolver ? "StellarSolver" : "FocusedCpp") +
                    " | thresholds{minSNR=" + std::to_string(p.minSNR) +
                    ", minHFD=" + std::to_string(p.minHFD) +
                    ", maxHFD=" + std::to_string(p.maxHFD) +
                    ", edgeMarginPx=" + std::to_string(p.edgeMarginPx) +
                    ", nearSatRatio=" + std::to_string(p.nearSaturationRatio) + "}",
                LogLevel::INFO, DeviceType::GUIDER);

    for (const auto& c : rejectedSNR)
    {
        Logger::Log(std::string(kLogPrefix) +
                        " fail_at=SNR " + formatCandidateBrief(c) +
                        " threshold.minSNR=" + std::to_string(p.minSNR),
                    LogLevel::INFO, DeviceType::GUIDER);
    }
    for (const auto& c : rejectedHFD)
    {
        Logger::Log(std::string(kLogPrefix) +
                        " fail_at=HFD " + formatCandidateBrief(c) +
                        " threshold.range=[" + std::to_string(p.minHFD) +
                        "," + std::to_string(p.maxHFD) + "]",
                    LogLevel::INFO, DeviceType::GUIDER);
    }
    for (const auto& c : rejectedEdge)
    {
        Logger::Log(std::string(kLogPrefix) +
                        " fail_at=edge " + formatCandidateBrief(c) +
                        " threshold.edgeMarginPx=" + std::to_string(p.edgeMarginPx),
                    LogLevel::INFO, DeviceType::GUIDER);
    }
    for (const auto& c : rejectedSaturation)
    {
        Logger::Log(std::string(kLogPrefix) +
                        " fail_at=saturation " + formatCandidateBrief(c) +
                        " threshold.nearSatADU=" + std::to_string(nearSat),
                    LogLevel::INFO, DeviceType::GUIDER);
    }

    if (s3.empty())
    {
        Logger::Log(std::string(kLogPrefix) +
                        " fail_at=final reason=no candidates after SNR/HFD/edge/saturation filtering",
                    LogLevel::INFO, DeviceType::GUIDER);
        return std::nullopt;
    }

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
    Logger::Log(std::string(kLogPrefix) +
                    " selected " + formatCandidateBrief(*bestIt),
                LogLevel::INFO, DeviceType::GUIDER);
    return *bestIt;
}

} // namespace guiding
