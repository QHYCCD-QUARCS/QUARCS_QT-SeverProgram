#include "FlatFieldDetector.h"

#include "../CentroidUtils.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <opencv2/imgproc.hpp>
#include <sstream>

namespace guiding {
namespace flatfield {

namespace {

// 格式化工具
std::string fmtCandidate(const StarCandidate& c)
{
    std::ostringstream oss;
    oss << "Star(x=" << std::fixed << std::setprecision(2) << c.x
        << ", y=" << c.y
        << ", SNR=" << c.snr
        << ", HFD=" << c.hfd
        << ", peakADU=" << c.peakADU << ")";
    return oss.str();
}

// MAD 噪声估计（Median Absolute Deviation）
double madStdDev(const cv::Mat& img)
{
    cv::Mat f;
    img.convertTo(f, CV_32F);
    cv::Scalar mean, stddev;
    cv::meanStdDev(f, mean, stddev);
    // 用 MAD 更鲁棒
    cv::Mat absDiff;
    cv::absdiff(f, mean[0], absDiff);
    // 手动计算中位数（cv::medianValue 在旧版OpenCV不可用）
    std::vector<float> vals(absDiff.total());
    for (int i = 0; i < absDiff.rows; ++i) {
        const float* row = absDiff.ptr<float>(i);
        for (int j = 0; j < absDiff.cols; ++j) {
            vals[i * absDiff.cols + j] = row[j];
        }
    }
    std::sort(vals.begin(), vals.end());
    double median = vals[vals.size() / 2];
    return median * 1.4826; // MAD → σ 转换因子
}

// 环形区域背景估计（用于SNR计算）
double ringBackground(const cv::Mat& img, int cx, int cy, int innerR, int outerR,
                      double& outMean, double& outStd)
{
    if (img.empty()) return 0.0;
    const int rows = img.rows, cols = img.cols;

    std::vector<float> vals;
    vals.reserve((outerR * outerR - innerR * innerR) * 4);

    for (int y = std::max(0, cy - outerR); y <= std::min(rows - 1, cy + outerR); ++y)
    {
        const ushort* row = img.ptr<ushort>(y);
        for (int x = std::max(0, cx - outerR); x <= std::min(cols - 1, cx + outerR); ++x)
        {
            int dx = x - cx, dy = y - cy;
            int r2 = dx * dx + dy * dy;
            if (r2 >= innerR * innerR && r2 <= outerR * outerR)
            {
                vals.push_back(static_cast<float>(row[x]));
            }
        }
    }

    if (vals.empty()) {
        outMean = 0.0;
        outStd = 1.0;
        return 0.0;
    }

    double sum = 0.0;
    for (float v : vals) sum += v;
    outMean = sum / vals.size();

    double sumSq = 0.0;
    for (float v : vals) {
        double d = v - outMean;
        sumSq += d * d;
    }
    outStd = std::sqrt(sumSq / vals.size());
    if (outStd < 1.0) outStd = 1.0; // 防止除零

    return outMean;
}

// HFD 计算（Half-Flux Diameter）
double computeHFD(const cv::Mat& img, double cx, double cy, int halfSize)
{
    if (img.empty() || halfSize < 1) return 0.0;

    const int x0 = std::max(0, static_cast<int>(std::llround(cx)) - halfSize);
    const int y0 = std::max(0, static_cast<int>(std::llround(cy)) - halfSize);
    const int x1 = std::min(img.cols - 1, static_cast<int>(std::llround(cx)) + halfSize);
    const int y1 = std::min(img.rows - 1, static_cast<int>(std::llround(cy)) + halfSize);

    // 收集像素值并排序
    std::vector<double> pixels;
    for (int y = y0; y <= y1; ++y) {
        const ushort* row = img.ptr<ushort>(y);
        for (int x = x0; x <= x1; ++x) {
            pixels.push_back(row[x]);
        }
    }
    std::sort(pixels.begin(), pixels.end());

    if (pixels.empty()) return 0.0;

    // 累积流量
    double totalFlux = 0.0;
    for (double v : pixels) totalFlux += v;
    double halfFlux = totalFlux * 0.5;

    double cumFlux = 0.0;
    double hfd = 0.0;
    for (double v : pixels) {
        cumFlux += v;
        if (cumFlux >= halfFlux) {
            hfd = v;
            break;
        }
    }

    // HFD = 包含50%流量的最小圆的直径（近似用像素值表示）
    // 更精确的实现需要按距离排序，这里用简化版
    return hfd > 0 ? hfd : 0.0;
}

// 更精确的HFD：按距离中心排序
double computeHFDByDistance(const cv::Mat& img, double cx, double cy, int halfSize)
{
    if (img.empty() || halfSize < 1) return 0.0;

    const int icx = static_cast<int>(std::llround(cx));
    const int icy = static_cast<int>(std::llround(cy));

    struct PixelDist {
        double dist;
        double value;
    };
    std::vector<PixelDist> pixels;
    pixels.reserve((halfSize * 2 + 1) * (halfSize * 2 + 1));

    for (int dy = -halfSize; dy <= halfSize; ++dy) {
        int y = icy + dy;
        if (y < 0 || y >= img.rows) continue;
        const ushort* row = img.ptr<ushort>(y);
        for (int dx = -halfSize; dx <= halfSize; ++dx) {
            int x = icx + dx;
            if (x < 0 || x >= img.cols) continue;
            double dist = std::sqrt(static_cast<double>(dx * dx + dy * dy));
            pixels.push_back({dist, static_cast<double>(row[x])});
        }
    }

    // 按距离排序
    std::sort(pixels.begin(), pixels.end(),
              [](const PixelDist& a, const PixelDist& b) { return a.dist < b.dist; });

    if (pixels.empty()) return 0.0;

    double totalFlux = 0.0;
    for (const auto& p : pixels) totalFlux += p.value;
    double halfFlux = totalFlux * 0.5;

    double cumFlux = 0.0;
    for (const auto& p : pixels) {
        cumFlux += p.value;
        if (cumFlux >= halfFlux) {
            return p.dist * 2.0; // 直径
        }
    }

    return pixels.back().dist * 2.0;
}

} // namespace

std::optional<StarCandidate> FlatFieldDetector::detect(
    const cv::Mat& image16,
    const Params& p,
    std::vector<StarCandidate>* outCandidates,
    std::vector<StarCandidate>* outRejected) const
{
    if (image16.empty() || image16.type() != CV_16UC1) {
        return std::nullopt;
    }

    // Step 1: 转换为浮点
    cv::Mat imageF;
    image16.convertTo(imageF, CV_32F);

    // Step 2: 生成平场
    cv::Mat flat = _generateFlatField(imageF, p.kernelSize, p.method, p.gaussianSigma);

    // Step 3: 扣除平场
    cv::Mat flatSub = _subtractFlat(imageF, flat);

    // Step 4: 峰值检测
    std::vector<StarCandidate> allCandidates = _findPeaks(imageF, flatSub, p);

    if (allCandidates.empty()) {
        return std::nullopt;
    }

    // Step 5: 筛选候选星
    double maxADU = 65535.0;
    if (image16.depth() == CV_8U) maxADU = 255.0;
    double satThreshold = maxADU * p.nearSaturationRatio;

    std::vector<StarCandidate> candidates;
    std::vector<StarCandidate> rejected;

    for (auto& c : allCandidates) {
        // 边缘剔除
        double edgeDist = std::min({c.x, c.y, image16.cols - 1 - c.x, image16.rows - 1 - c.y});
        c.edgeDistPx = edgeDist;
        if (edgeDist < p.edgeMarginPx) {
            rejected.push_back(c);
            continue;
        }

        // 饱和检查
        if (c.peakADU > satThreshold) {
            rejected.push_back(c);
            continue;
        }

        // HFD 计算与筛选
        c.hfd = computeHFDByDistance(image16, c.x, c.y, std::max(p.centroidHalfSize, 5));
        if (c.hfd < p.minHFD || c.hfd > p.maxHFD) {
            rejected.push_back(c);
            continue;
        }

        // SNR 筛选
        if (c.snr < p.snrThreshold) {
            rejected.push_back(c);
            continue;
        }

        candidates.push_back(c);
    }

    if (outCandidates) *outCandidates = candidates;
    if (outRejected) *outRejected = rejected;

    if (candidates.empty()) {
        return std::nullopt;
    }

    // 选择最佳星（最高SNR）
    StarCandidate best = *std::max_element(candidates.begin(), candidates.end(),
        [](const StarCandidate& a, const StarCandidate& b) {
            return a.snr < b.snr;
        });

    return best;
}

cv::Mat FlatFieldDetector::generateFlatField(const cv::Mat& image16, int kernelSize,
                                              const std::string& method, double sigma) const
{
    cv::Mat imageF;
    image16.convertTo(imageF, CV_32F);
    return _generateFlatField(imageF, kernelSize, method, sigma);
}

cv::Mat FlatFieldDetector::generateFlatSubtracted(const cv::Mat& image16, int kernelSize,
                                                   const std::string& method, double sigma) const
{
    cv::Mat imageF;
    image16.convertTo(imageF, CV_32F);
    cv::Mat flat = _generateFlatField(imageF, kernelSize, method, sigma);
    return _subtractFlat(imageF, flat);
}

cv::Mat FlatFieldDetector::_generateFlatField(const cv::Mat& imageF, int kernelSize,
                                               const std::string& method, double sigma) const
{
    cv::Mat flat;
    cv::Size ksize(kernelSize, kernelSize);
    cv::Point anchor(-1, -1);

    if (method == "gaussian") {
        double s = (sigma > 0) ? sigma : (kernelSize / 4.0);
        cv::GaussianBlur(imageF, flat, ksize, s, s, cv::BORDER_REPLICATE);
    } else if (method == "median") {
        // 中值滤波核大小必须是奇数
        int ks = (kernelSize % 2 == 0) ? kernelSize + 1 : kernelSize;
        cv::medianBlur(imageF, flat, ks);
    } else {
        // 默认：均匀滤波
        cv::blur(imageF, flat, ksize, anchor, cv::BORDER_REPLICATE);
    }

    return flat;
}

cv::Mat FlatFieldDetector::_subtractFlat(const cv::Mat& imageF, const cv::Mat& flat) const
{
    cv::Mat result;
    cv::subtract(imageF, flat, result);
    // 确保非负
    cv::max(result, 0.0, result);
    return result;
}

std::vector<StarCandidate> FlatFieldDetector::_findPeaks(const cv::Mat& imageF,
                                                          const cv::Mat& flatSub,
                                                          const Params& p) const
{
    std::vector<StarCandidate> candidates;

    if (flatSub.empty()) return candidates;

    // 使用最大值滤波找局部峰值
    // OpenCV没有直接的maximum_filter，用dilate+kernel实现
    int peakWindowSize = std::max(5, p.minSeparation * 2 + 1);
    // 确保奇数
    if (peakWindowSize % 2 == 0) peakWindowSize++;
    cv::Mat localMax;
    cv::Mat kernel = cv::Mat::ones(peakWindowSize, peakWindowSize, CV_32F);
    cv::dilate(flatSub, localMax, kernel);

    // 峰值掩码：原图 == 局部最大值（差值 < 0.01）
    cv::Mat peakMask;
    cv::Mat diff;
    cv::absdiff(flatSub, localMax, diff);
    cv::threshold(diff, peakMask, 0.01, 255, cv::THRESH_BINARY_INV);

    // 遍历峰值位置
    for (int y = 0; y < peakMask.rows; ++y) {
        const uchar* maskRow = peakMask.ptr<uchar>(y);
        const float* imgRow = flatSub.ptr<float>(y);
        for (int x = 0; x < peakMask.cols; ++x) {
            if (!maskRow[x]) continue;

            // 估计背景（环形区域）
            double bgMean, bgStd;
            ringBackground(imageF, x, y, 5, 15, bgMean, bgStd);

            double signal = imgRow[x] - bgMean;
            double snr = (bgStd > 0) ? (signal / bgStd) : 0.0;

            if (snr < p.snrThreshold * 0.5) continue; // 预筛选

            // 质心精化
            double cx = static_cast<double>(x);
            double cy = static_cast<double>(y);
            _refineCentroid(imageF, cx, cy, p.centroidHalfSize);

            // 计算峰值ADU
            double peakADU = _localPeakADU(imageF, cx, cy, p.centroidHalfSize);

            StarCandidate c;
            c.x = cx;
            c.y = cy;
            c.snr = snr;
            c.peakADU = peakADU;
            c.hfd = 0.0; // 后续计算
            c.edgeDistPx = 0.0;

            candidates.push_back(c);
        }
    }

    // 距离去重
    if (candidates.size() > 1) {
        std::sort(candidates.begin(), candidates.end(),
            [](const StarCandidate& a, const StarCandidate& b) {
                return a.snr > b.snr; // SNR高的优先
            });

        std::vector<StarCandidate> deduped;
        std::vector<bool> used(candidates.size(), false);

        for (size_t i = 0; i < candidates.size(); ++i) {
            if (used[i]) continue;
            deduped.push_back(candidates[i]);
            used[i] = true;

            for (size_t j = i + 1; j < candidates.size(); ++j) {
                if (used[j]) continue;
                double dx = candidates[i].x - candidates[j].x;
                double dy = candidates[i].y - candidates[j].y;
                double dist = std::sqrt(dx * dx + dy * dy);
                if (dist < p.minSeparation) {
                    used[j] = true;
                }
            }
        }
        candidates = std::move(deduped);
    }

    return candidates;
}

double FlatFieldDetector::_estimateBackground(const cv::Mat& imageF, int x, int y,
                                               int halfSize) const
{
    double bgMean, bgStd;
    ringBackground(imageF, x, y, halfSize / 2, halfSize, bgMean, bgStd);
    return bgMean;
}

double FlatFieldDetector::_computeHFD(const cv::Mat& imageF, double x, double y) const
{
    return computeHFDByDistance(imageF, x, y, 8);
}

double FlatFieldDetector::_localPeakADU(const cv::Mat& imageF, double x, double y,
                                         int halfSize) const
{
    int icx = static_cast<int>(std::llround(x));
    int icy = static_cast<int>(std::llround(y));

    float peak = 0.0f;
    for (int dy = -halfSize; dy <= halfSize; ++dy) {
        int yy = icy + dy;
        if (yy < 0 || yy >= imageF.rows) continue;
        const float* row = imageF.ptr<float>(yy);
        for (int dx = -halfSize; dx <= halfSize; ++dx) {
            int xx = icx + dx;
            if (xx < 0 || xx >= imageF.cols) continue;
            if (row[xx] > peak) peak = row[xx];
        }
    }
    return static_cast<double>(peak);
}

bool FlatFieldDetector::_refineCentroid(const cv::Mat& imageF, double& x, double& y,
                                         int halfSize) const
{
    // 使用加权质心（复用 CentroidUtils）
    QPointF approx(x, y);
    QPointF out;
    if (FindCentroidWeighted(imageF, approx, halfSize, out, 2.0)) {
        x = out.x();
        y = out.y();
        return true;
    }
    return false;
}

} // namespace flatfield
} // namespace guiding
