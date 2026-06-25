#include "FlatFieldDetector.h"
#include "../../Logger.h"

#include "../CentroidUtils.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <opencv2/imgproc.hpp>
#include <sstream>
#include <unordered_map>

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

std::pair<double, double> estimateGlobalBackgroundAndNoise(const cv::Mat& img)
{
    if (img.empty())
        return {0.0, 1.0};

    std::vector<double> pixels;
    pixels.reserve(img.total());
    if (img.type() == CV_32F)
    {
        for (int y = 0; y < img.rows; ++y)
        {
            const float* row = img.ptr<float>(y);
            for (int x = 0; x < img.cols; ++x)
                pixels.push_back(static_cast<double>(row[x]));
        }
    }
    else if (img.type() == CV_16UC1)
    {
        for (int y = 0; y < img.rows; ++y)
        {
            const ushort* row = img.ptr<ushort>(y);
            for (int x = 0; x < img.cols; ++x)
                pixels.push_back(static_cast<double>(row[x]));
        }
    }
    else
    {
        return {0.0, 1.0};
    }

    if (pixels.empty())
        return {0.0, 1.0};

    std::sort(pixels.begin(), pixels.end());
    const size_t lo = static_cast<size_t>(pixels.size() * 0.1);
    const size_t hi = static_cast<size_t>(pixels.size() * 0.9);
    const size_t mid = lo + std::max<size_t>(1, (hi > lo ? (hi - lo) / 2 : 0));
    const double bg = static_cast<double>(pixels[std::min(mid, pixels.size() - 1)]);

    cv::Scalar meanAll, stdAll;
    cv::meanStdDev(img, meanAll, stdAll);
    const double coarseStd = std::max(1.0, stdAll[0]);
    const double cutoff = bg + 3.0 * coarseStd;

    double sum = 0.0;
    double sumSq = 0.0;
    size_t count = 0;
    for (double v : pixels)
    {
        if (v >= cutoff)
            break;
        sum += v;
        sumSq += static_cast<double>(v) * static_cast<double>(v);
        ++count;
    }

    if (count == 0)
        return {bg, 1.0};

    const double mean = sum / static_cast<double>(count);
    double variance = sumSq / static_cast<double>(count) - mean * mean;
    if (variance < 1.0)
        variance = 1.0;
    return {bg, std::sqrt(variance)};
}

std::vector<StarCandidate> spatialDedupByPeak(const std::vector<StarCandidate>& input, double minSeparation)
{
    if (input.size() <= 1 || minSeparation <= 0.0)
        return input;

    std::vector<StarCandidate> sorted = input;
    std::sort(sorted.begin(), sorted.end(),
              [](const StarCandidate& a, const StarCandidate& b) {
                  if (a.peakADU == b.peakADU)
                      return a.snr > b.snr;
                  return a.peakADU > b.peakADU;
              });

    const double cellSize = std::max(1.0, minSeparation);
    std::unordered_map<long long, std::vector<size_t>> buckets;
    std::vector<StarCandidate> kept;
    kept.reserve(sorted.size());

    auto cellKey = [](int gx, int gy) -> long long {
        return (static_cast<long long>(gx) << 32) ^
               static_cast<unsigned long long>(static_cast<uint32_t>(gy));
    };

    for (const auto& c : sorted)
    {
        const int gx = static_cast<int>(std::floor(c.x / cellSize));
        const int gy = static_cast<int>(std::floor(c.y / cellSize));
        bool blocked = false;

        for (int dy = -1; dy <= 1 && !blocked; ++dy)
        {
            for (int dx = -1; dx <= 1 && !blocked; ++dx)
            {
                auto it = buckets.find(cellKey(gx + dx, gy + dy));
                if (it == buckets.end())
                    continue;
                for (size_t idx : it->second)
                {
                    const auto& prev = kept[idx];
                    const double ddx = prev.x - c.x;
                    const double ddy = prev.y - c.y;
                    if ((ddx * ddx + ddy * ddy) < minSeparation * minSeparation)
                    {
                        blocked = true;
                        break;
                    }
                }
            }
        }

        if (blocked)
            continue;

        const size_t newIndex = kept.size();
        kept.push_back(c);
        buckets[cellKey(gx, gy)].push_back(newIndex);
    }

    return kept;
}

std::vector<StarCandidate> dedupRejectAnyNeighbor(const std::vector<StarCandidate>& input, double minSeparation)
{
    if (input.size() <= 1 || minSeparation <= 0.0)
        return input;

    const double minSep2 = minSeparation * minSeparation;
    std::vector<char> keep(input.size(), 1);
    for (size_t i = 0; i < input.size(); ++i)
    {
        if (!keep[i])
            continue;
        for (size_t j = i + 1; j < input.size(); ++j)
        {
            if (!keep[j])
                continue;
            const double dx = input[i].x - input[j].x;
            const double dy = input[i].y - input[j].y;
            if ((dx * dx + dy * dy) < minSep2)
            {
                keep[i] = 0;
                keep[j] = 0;
            }
        }
    }

    std::vector<StarCandidate> out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i)
    {
        if (keep[i])
            out.push_back(input[i]);
    }
    return out;
}

bool weightedCentroid5x5(const cv::Mat& imageF, int x, int y, double& outX, double& outY)
{
    if (imageF.empty())
        return false;
    if (x < 2 || y < 2 || x + 2 >= imageF.cols || y + 2 >= imageF.rows)
        return false;

    double rowSum[5] = {0, 0, 0, 0, 0};
    double colSum[5] = {0, 0, 0, 0, 0};
    double total = 0.0;
    if (imageF.type() == CV_32F)
    {
        for (int dy = -2; dy <= 2; ++dy)
        {
            const float* row = imageF.ptr<float>(y + dy);
            for (int dx = -2; dx <= 2; ++dx)
            {
                const double v = static_cast<double>(row[x + dx]);
                rowSum[dy + 2] += v;
                colSum[dx + 2] += v;
                total += v;
            }
        }
    }
    else if (imageF.type() == CV_16UC1)
    {
        for (int dy = -2; dy <= 2; ++dy)
        {
            const ushort* row = imageF.ptr<ushort>(y + dy);
            for (int dx = -2; dx <= 2; ++dx)
            {
                const double v = static_cast<double>(row[x + dx]);
                rowSum[dy + 2] += v;
                colSum[dx + 2] += v;
                total += v;
            }
        }
    }
    else
    {
        return false;
    }

    if (total <= 0.0)
        return false;

    double cy = 0.0;
    double cx = 0.0;
    for (int i = 0; i < 5; ++i)
    {
        cy += static_cast<double>(i) * rowSum[i];
        cx += static_cast<double>(i) * colSum[i];
    }
    cy /= total;
    cx /= total;

    outX = static_cast<double>(x) + cx - 2.0;
    outY = static_cast<double>(y) + cy - 2.0;
    return true;
}

} // namespace

std::optional<StarCandidate> FlatFieldDetector::detect(
    const cv::Mat& image16,
    const Params& p,
    std::vector<StarCandidate>* outDedupCandidates,
    std::vector<StarCandidate>* outSnrCandidates,
    std::vector<StarCandidate>* outCandidates,
    std::vector<StarCandidate>* outRejected) const
{
    if (image16.empty() || image16.type() != CV_16UC1) {
        return std::nullopt;
    }

    // Step 1: 转换为双精度，尽量贴近验证脚本里的 float64 路径。
    cv::Mat imageF;
    image16.convertTo(imageF, CV_64F);

    // Step 2: 生成平场
    cv::Mat flat = _generateFlatField(imageF, p.kernelSize, p.method, p.gaussianSigma);

    // Step 3: 扣除平场
    cv::Mat flatSub = _subtractFlat(imageF, flat);
    cv::Mat flatSub16;
    flatSub.convertTo(flatSub16, CV_16U);

    // Step 4: 峰值检测。
    // 真实筛选顺序尽量和已验证脚本一致：局部峰值 -> SNR -> 5x5 质心 -> 距离去重。
    // 紫框仍保留为“近邻消除后的预览层”，便于肉眼查看峰值分布。
    std::vector<StarCandidate> rawPeaks = _findPeaks(flatSub16, p);
    std::vector<StarCandidate> dedupPreview = spatialDedupByPeak(rawPeaks, p.minSeparation);
    if (outDedupCandidates) *outDedupCandidates = dedupPreview;
    if (outSnrCandidates) outSnrCandidates->clear();

    if (rawPeaks.empty()) {
        if (outCandidates) outCandidates->clear();
        if (outRejected) outRejected->clear();
        return std::nullopt;
    }

    // Step 5: 严格按验证脚本做 SNR 预筛，再做 5x5 加权质心。
    std::vector<StarCandidate> rejected;
    std::vector<StarCandidate> snrPassed;
    const auto [bgMean, bgNoiseRaw] = estimateGlobalBackgroundAndNoise(flatSub16);
    const double bgNoise = std::max(1.0, bgNoiseRaw);

    for (auto c : rawPeaks) {
        const int px = std::clamp(static_cast<int>(std::llround(c.x)), 0, flatSub16.cols - 1);
        const int py = std::clamp(static_cast<int>(std::llround(c.y)), 0, flatSub16.rows - 1);
        const double peakFlatSub = static_cast<double>(flatSub16.at<ushort>(py, px));
        c.snr = (peakFlatSub - bgMean) / bgNoise;

        if (c.snr < p.snrThreshold) {
            rejected.push_back(c);
            continue;
        }

        double cx = 0.0;
        double cy = 0.0;
        if (!weightedCentroid5x5(flatSub16, px, py, cx, cy)) {
            rejected.push_back(c);
            continue;
        }

        c.x = cx;
        c.y = cy;
        c.peakADU = peakFlatSub;
        c.edgeDistPx = std::min({c.x, c.y, image16.cols - 1 - c.x, image16.rows - 1 - c.y});
        snrPassed.push_back(c);
    }

    // Step 6: 距离去重，和验证脚本的“任意近邻都剔除”语义保持一致。
    std::vector<StarCandidate> candidates = dedupRejectAnyNeighbor(snrPassed, p.minSeparation);

    if (outSnrCandidates) *outSnrCandidates = snrPassed;
    if (outCandidates) *outCandidates = candidates;
    if (outRejected) *outRejected = rejected;

    Logger::Log((QString("[FlatField] stageCounts raw=%1 dedup=%2 snrPassed=%3 final=%4 rejected=%5")
        .arg(static_cast<int>(rawPeaks.size()))
        .arg(static_cast<int>(dedupPreview.size()))
        .arg(static_cast<int>(snrPassed.size()))
        .arg(static_cast<int>(candidates.size()))
        .arg(static_cast<int>(rejected.size()))).toStdString(),
        LogLevel::INFO, DeviceType::GUIDER);

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
    image16.convertTo(imageF, CV_64F);
    return _generateFlatField(imageF, kernelSize, method, sigma);
}

cv::Mat FlatFieldDetector::generateFlatSubtracted(const cv::Mat& image16, int kernelSize,
                                                   const std::string& method, double sigma) const
{
    cv::Mat imageF;
    image16.convertTo(imageF, CV_64F);
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
        cv::GaussianBlur(imageF, flat, ksize, s, s, cv::BORDER_REFLECT_101);
    } else if (method == "median") {
        // 中值滤波核大小必须是奇数
        int ks = (kernelSize % 2 == 0) ? kernelSize + 1 : kernelSize;
        cv::medianBlur(imageF, flat, ks);
    } else {
        // 默认：均匀滤波。使用 reflect 边界更贴近 scipy.ndimage.uniform_filter。
        cv::boxFilter(imageF, flat, imageF.type(), ksize, anchor, true, cv::BORDER_REFLECT_101);
    }

    return flat;
}

cv::Mat FlatFieldDetector::_subtractFlat(const cv::Mat& imageF, const cv::Mat& flat) const
{
    cv::Mat result;
    cv::subtract(imageF, flat, result);
    // 与已验证脚本保持一致：保留一个正偏移，避免背景被压到 0 附近后把 SNR 整体抬高。
    result += 2000.0;
    cv::max(result, 0.0, result);
    return result;
}

std::vector<StarCandidate> FlatFieldDetector::_findPeaks(const cv::Mat& flatSub16,
                                                          const Params& p) const
{
    std::vector<StarCandidate> candidates;

    if (flatSub16.empty() || flatSub16.type() != CV_16UC1) return candidates;

    // 使用最大值滤波找局部峰值
    // OpenCV没有直接的maximum_filter，用dilate+kernel实现
    const int peakWindowSize = 5;
    cv::Mat localMax;
    cv::Mat kernel = cv::Mat::ones(peakWindowSize, peakWindowSize, CV_32F);
    cv::dilate(flatSub16, localMax, kernel);

    // 峰值掩码：和验证脚本保持一致，在 uint16 扣平场图上做“严格相等”的局部最大值判断。
    cv::Mat peakMask;
    cv::compare(flatSub16, localMax, peakMask, cv::CMP_EQ);

    // 遍历峰值位置。这里仅保留局部极大值与边缘过滤，不提前做 SNR，
    // 以便把“紫框=空间去重后的峰值候选”和“黄框=SNR 通过候选”分开。
    for (int y = 0; y < peakMask.rows; ++y) {
        const uchar* maskRow = peakMask.ptr<uchar>(y);
        const ushort* imgRow = flatSub16.ptr<ushort>(y);
        for (int x = 0; x < peakMask.cols; ++x) {
            if (!maskRow[x]) continue;
            if (imgRow[x] == 0) continue;
            if (x < p.edgeMarginPx || y < p.edgeMarginPx ||
                x >= flatSub16.cols - p.edgeMarginPx || y >= flatSub16.rows - p.edgeMarginPx)
                continue;

            StarCandidate c;
            c.x = static_cast<double>(x);
            c.y = static_cast<double>(y);
            c.snr = 0.0;
            c.peakADU = static_cast<double>(imgRow[x]);
            c.hfd = 0.0; // 后续计算
            c.edgeDistPx = 0.0;

            candidates.push_back(c);
        }
    }

    return spatialDedupByPeak(candidates, p.minSeparation);
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
