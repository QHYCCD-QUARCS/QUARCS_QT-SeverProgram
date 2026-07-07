#include "FlatFieldStarDetector.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <sstream>
#include <unordered_map>

#include <opencv2/imgproc.hpp>

namespace star_detect {

namespace {

cv::Mat generateFlatFieldImpl(const cv::Mat& imageF, int kernelSize,
                              const std::string& method, double sigma)
{
    cv::Mat flat;
    cv::Size ksize(kernelSize, kernelSize);
    cv::Point anchor(-1, -1);

    if (method == "gaussian")
    {
        const double s = (sigma > 0.0) ? sigma : (kernelSize / 4.0);
        cv::GaussianBlur(imageF, flat, ksize, s, s, cv::BORDER_REFLECT_101);
    }
    else if (method == "median")
    {
        const int ks = (kernelSize % 2 == 0) ? kernelSize + 1 : kernelSize;
        cv::medianBlur(imageF, flat, ks);
    }
    else
    {
        cv::boxFilter(imageF, flat, imageF.type(), ksize, anchor, true, cv::BORDER_REFLECT_101);
    }

    return flat;
}

cv::Mat subtractFlatImpl(const cv::Mat& imageF, const cv::Mat& flat)
{
    cv::Mat result;
    cv::subtract(imageF, flat, result);
    result += 2000.0;
    cv::max(result, 0.0, result);
    return result;
}

std::pair<double, double> estimateGlobalBackgroundAndNoise(const cv::Mat& img)
{
    if (img.empty())
        return {0.0, 1.0};

    std::vector<double> pixels;
    pixels.reserve(img.total());

    if (img.type() == CV_16UC1)
    {
        for (int y = 0; y < img.rows; ++y)
        {
            const ushort* row = img.ptr<ushort>(y);
            for (int x = 0; x < img.cols; ++x)
                pixels.push_back(static_cast<double>(row[x]));
        }
    }
    else if (img.type() == CV_32F)
    {
        for (int y = 0; y < img.rows; ++y)
        {
            const float* row = img.ptr<float>(y);
            for (int x = 0; x < img.cols; ++x)
                pixels.push_back(static_cast<double>(row[x]));
        }
    }
    else if (img.type() == CV_64F)
    {
        for (int y = 0; y < img.rows; ++y)
        {
            const double* row = img.ptr<double>(y);
            for (int x = 0; x < img.cols; ++x)
                pixels.push_back(row[x]);
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
    const double bg = pixels[std::min(mid, pixels.size() - 1)];

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
        sumSq += v * v;
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

bool isInsideSearchRegion(const SearchRegion& region, double x, double y)
{
    const auto contains = [](const cv::Rect& rect, double px, double py) {
        return px >= static_cast<double>(rect.x) &&
               py >= static_cast<double>(rect.y) &&
               px < static_cast<double>(rect.x + rect.width) &&
               py < static_cast<double>(rect.y + rect.height);
    };

    if (region.includeRect.has_value() && !contains(*region.includeRect, x, y))
        return false;
    for (const auto& rect : region.excludeRects)
    {
        if (contains(rect, x, y))
            return false;
    }
    return true;
}

std::vector<DetectedStar> spatialDedupByPeak(const std::vector<DetectedStar>& input, double minSeparation)
{
    if (input.size() <= 1 || minSeparation <= 0.0)
        return input;

    std::vector<DetectedStar> sorted = input;
    std::sort(sorted.begin(), sorted.end(),
              [](const DetectedStar& a, const DetectedStar& b) {
                  if (a.peakADU == b.peakADU)
                      return a.snr > b.snr;
                  return a.peakADU > b.peakADU;
              });

    const double cellSize = std::max(1.0, minSeparation);
    std::unordered_map<long long, std::vector<size_t>> buckets;
    std::vector<DetectedStar> kept;
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

struct DedupOutcome
{
    std::vector<DetectedStar> kept;
    std::vector<RejectedStar> rejected;
};

DedupOutcome dedupRejectAnyNeighbor(const std::vector<DetectedStar>& input, double minSeparation)
{
    DedupOutcome outcome;
    if (input.empty())
        return outcome;
    if (input.size() == 1 || minSeparation <= 0.0)
    {
        outcome.kept = input;
        return outcome;
    }

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

    outcome.kept.reserve(input.size());
    outcome.rejected.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i)
    {
        if (keep[i])
        {
            outcome.kept.push_back(input[i]);
            continue;
        }

        RejectedStar rejected;
        rejected.star = input[i];
        rejected.reason = RejectReason::Duplicate;
        rejected.detail = "rejected_by_min_separation";
        outcome.rejected.push_back(rejected);
    }

    return outcome;
}

bool weightedCentroid5x5(const cv::Mat& image, int x, int y, double& outX, double& outY)
{
    if (image.empty())
        return false;
    if (x < 2 || y < 2 || x + 2 >= image.cols || y + 2 >= image.rows)
        return false;

    double rowSum[5] = {0, 0, 0, 0, 0};
    double colSum[5] = {0, 0, 0, 0, 0};
    double total = 0.0;

    for (int dy = -2; dy <= 2; ++dy)
    {
        const ushort* row = image.ptr<ushort>(y + dy);
        for (int dx = -2; dx <= 2; ++dx)
        {
            const double v = static_cast<double>(row[x + dx]);
            rowSum[dy + 2] += v;
            colSum[dx + 2] += v;
            total += v;
        }
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

double computeHFDByDistance(const cv::Mat& img, double cx, double cy, int halfSize)
{
    if (img.empty() || halfSize < 1)
        return 0.0;

    const int icx = static_cast<int>(std::llround(cx));
    const int icy = static_cast<int>(std::llround(cy));

    struct PixelDist
    {
        double dist;
        double value;
    };

    std::vector<PixelDist> pixels;
    pixels.reserve((halfSize * 2 + 1) * (halfSize * 2 + 1));

    for (int dy = -halfSize; dy <= halfSize; ++dy)
    {
        const int y = icy + dy;
        if (y < 0 || y >= img.rows)
            continue;
        const ushort* row = img.ptr<ushort>(y);
        for (int dx = -halfSize; dx <= halfSize; ++dx)
        {
            const int x = icx + dx;
            if (x < 0 || x >= img.cols)
                continue;
            const double dist = std::sqrt(static_cast<double>(dx * dx + dy * dy));
            pixels.push_back({dist, static_cast<double>(row[x])});
        }
    }

    std::sort(pixels.begin(), pixels.end(),
              [](const PixelDist& a, const PixelDist& b) { return a.dist < b.dist; });

    if (pixels.empty())
        return 0.0;

    double totalFlux = 0.0;
    for (const auto& pixel : pixels)
        totalFlux += pixel.value;
    const double halfFlux = totalFlux * 0.5;

    double cumFlux = 0.0;
    for (const auto& pixel : pixels)
    {
        cumFlux += pixel.value;
        if (cumFlux >= halfFlux)
            return pixel.dist * 2.0;
    }

    return pixels.back().dist * 2.0;
}

std::vector<DetectedStar> findPeaks(const cv::Mat& flatSub16,
                                    const DetectParams& params,
                                    const ImageContext& imageContext,
                                    const SearchRegion& searchRegion)
{
    std::vector<DetectedStar> candidates;
    if (flatSub16.empty() || flatSub16.type() != CV_16UC1)
        return candidates;

    const int peakWindowSize = 5;
    cv::Mat localMax;
    cv::Mat kernel = cv::Mat::ones(peakWindowSize, peakWindowSize, CV_32F);
    cv::dilate(flatSub16, localMax, kernel);

    cv::Mat peakMask;
    cv::compare(flatSub16, localMax, peakMask, cv::CMP_EQ);

    for (int y = 0; y < peakMask.rows; ++y)
    {
        const uchar* maskRow = peakMask.ptr<uchar>(y);
        const ushort* imgRow = flatSub16.ptr<ushort>(y);
        for (int x = 0; x < peakMask.cols; ++x)
        {
            if (!maskRow[x] || imgRow[x] == 0)
                continue;
            if (x < params.edgeMarginPx || y < params.edgeMarginPx ||
                x >= flatSub16.cols - params.edgeMarginPx ||
                y >= flatSub16.rows - params.edgeMarginPx)
                continue;
            if (!isInsideSearchRegion(searchRegion, static_cast<double>(x), static_cast<double>(y)))
                continue;

            DetectedStar star;
            star.x = static_cast<double>(x);
            star.y = static_cast<double>(y);
            star.fullX = static_cast<double>(imageContext.roiOriginX) + star.x;
            star.fullY = static_cast<double>(imageContext.roiOriginY) + star.y;
            star.peakADU = static_cast<double>(imgRow[x]);
            candidates.push_back(star);
        }
    }

    return spatialDedupByPeak(candidates, params.minSeparationPx);
}

std::string buildSummary(const DetectionDebugInfo& debug)
{
    std::ostringstream oss;
    oss << "raw=" << debug.rawPeakCount
        << " dedup=" << debug.dedupCount
        << " snrPassed=" << debug.snrPassedCount
        << " valid=" << debug.validCount
        << " rejected=" << debug.rejectedCount;
    return oss.str();
}

} // namespace

DetectionResult FlatFieldStarDetector::detect(const cv::Mat& image16,
                                              const ImageContext& imageContext,
                                              const DetectParams& detectParams,
                                              const SearchRegion& searchRegion) const
{
    DetectionResult result;
    if (image16.empty() || image16.type() != CV_16UC1)
    {
        result.debug.summary = "invalid_input";
        return result;
    }

    cv::Mat imageF;
    image16.convertTo(imageF, CV_64F);

    const cv::Mat flat = generateFlatFieldImpl(imageF, detectParams.kernelSize,
                                               detectParams.flatMethod, detectParams.gaussianSigma);
    const cv::Mat flatSub = subtractFlatImpl(imageF, flat);

    cv::Mat flatSub16;
    flatSub.convertTo(flatSub16, CV_16U);

    std::vector<DetectedStar> rawPeaks = findPeaks(flatSub16, detectParams, imageContext, searchRegion);
    std::vector<DetectedStar> dedupPreview = spatialDedupByPeak(rawPeaks, detectParams.minSeparationPx);

    result.allCandidates = rawPeaks;
    result.dedupCandidates = dedupPreview;

    result.debug.rawPeakCount = static_cast<int>(rawPeaks.size());
    result.debug.dedupCount = static_cast<int>(dedupPreview.size());

    if (rawPeaks.empty())
    {
        result.debug.summary = buildSummary(result.debug);
        return result;
    }

    const auto [bgMean, bgNoiseRaw] = estimateGlobalBackgroundAndNoise(flatSub16);
    const double bgNoise = std::max(1.0, bgNoiseRaw);

    std::vector<DetectedStar> snrPassed;
    std::vector<RejectedStar> rejected;
    snrPassed.reserve(rawPeaks.size());
    rejected.reserve(rawPeaks.size());

    for (auto star : rawPeaks)
    {
        const int px = std::clamp(static_cast<int>(std::llround(star.x)), 0, flatSub16.cols - 1);
        const int py = std::clamp(static_cast<int>(std::llround(star.y)), 0, flatSub16.rows - 1);
        const double peakFlatSub = static_cast<double>(flatSub16.at<ushort>(py, px));

        star.snr = (peakFlatSub - bgMean) / bgNoise;
        star.peakADU = peakFlatSub;
        star.bgMean = bgMean;
        star.bgStd = bgNoise;
        star.edgeDistPx = std::min({star.x, star.y,
                                    static_cast<double>(image16.cols - 1) - star.x,
                                    static_cast<double>(image16.rows - 1) - star.y});
        star.saturated = (peakFlatSub >= detectParams.nearSaturationRatio * 65535.0);

        if (star.snr < detectParams.snrThreshold)
        {
            rejected.push_back({star, RejectReason::LowSnr, "below_snr_threshold"});
            continue;
        }

        double cx = 0.0;
        double cy = 0.0;
        if (!weightedCentroid5x5(flatSub16, px, py, cx, cy))
        {
            rejected.push_back({star, RejectReason::CentroidFailed, "weighted_centroid_failed"});
            continue;
        }

        star.x = cx;
        star.y = cy;
        star.fullX = static_cast<double>(imageContext.roiOriginX) + star.x;
        star.fullY = static_cast<double>(imageContext.roiOriginY) + star.y;
        star.edgeDistPx = std::min({star.x, star.y,
                                    static_cast<double>(image16.cols - 1) - star.x,
                                    static_cast<double>(image16.rows - 1) - star.y});
        star.centroidRefined = true;
        star.hfd = computeHFDByDistance(image16, star.x, star.y, 8);
        if (detectParams.computeHfr)
            star.hfr = star.hfd * 0.5;
        snrPassed.push_back(star);
    }

    DedupOutcome dedupOutcome = dedupRejectAnyNeighbor(snrPassed, detectParams.minSeparationPx);

    result.snrCandidates = snrPassed;
    result.validCandidates = dedupOutcome.kept;
    result.rejectedCandidates = std::move(rejected);
    result.rejectedCandidates.insert(result.rejectedCandidates.end(),
                                     dedupOutcome.rejected.begin(), dedupOutcome.rejected.end());

    result.debug.snrPassedCount = static_cast<int>(snrPassed.size());
    result.debug.validCount = static_cast<int>(result.validCandidates.size());
    result.debug.rejectedCount = static_cast<int>(result.rejectedCandidates.size());
    result.debug.summary = buildSummary(result.debug);
    return result;
}

cv::Mat FlatFieldStarDetector::generateFlatField(const cv::Mat& image16,
                                                 int kernelSize,
                                                 const std::string& method,
                                                 double sigma) const
{
    cv::Mat imageF;
    image16.convertTo(imageF, CV_64F);
    return generateFlatFieldImpl(imageF, kernelSize, method, sigma);
}

cv::Mat FlatFieldStarDetector::generateFlatSubtracted(const cv::Mat& image16,
                                                      int kernelSize,
                                                      const std::string& method,
                                                      double sigma) const
{
    cv::Mat imageF;
    image16.convertTo(imageF, CV_64F);
    const cv::Mat flat = generateFlatFieldImpl(imageF, kernelSize, method, sigma);
    return subtractFlatImpl(imageF, flat);
}

} // namespace star_detect
