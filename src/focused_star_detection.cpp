// QfdNative 多星识别与 HFR 测量：
// 1) 在缩放后的 8bit 图上做边缘/连通结构检测，生成星点候选
// 2) 用局部 sigma-clipped 背景估计筛除伪星点
// 3) 对每颗候选星做窗口质心细化、curve-of-growth HFR、FWHM、偏心率测量
// 4) 返回多颗星；若失败则退回到旧的单峰值质心兜底实现
#include "tools.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace {

struct QfdNativeState {
    cv::Mat gray32;
    cv::Mat detect8;
    double resizeFactor = 1.0;
    double inverseResizeFactor = 1.0;
    int minStarSize = 2;
    int maxStarSize = 150;
};

struct QfdNativePixelData {
    int x = 0;
    int y = 0;
    double value = 0.0;
};

struct QfdNativeRadialSample {
    double distance = 0.0;
    double rawFlux = 0.0;
    double positiveFlux = 0.0;
    int x = 0;
    int y = 0;
};

struct QfdNativeMeasuredStar {
    double radius = 0.0;
    cv::Rect rect;
    double meanBrightness = 0.0;
    double surroundingMean = 0.0;
    double maxPixelValue = 0.0;
    cv::Point2d position;
    double hfr = std::numeric_limits<double>::quiet_NaN();
    double fwhm = std::numeric_limits<double>::quiet_NaN();
    double eccentricity = std::numeric_limits<double>::quiet_NaN();
    double averageBrightness = std::numeric_limits<double>::quiet_NaN();
    double totalFlux = 0.0;
    int supportArea = 0;
    double backgroundSigma = 0.0;
    double snr = 0.0;
};

static cv::Mat QfdNativeToSingleChannelGray(const cv::Mat& image)
{
    cv::Mat gray;
    if (image.channels() == 1) {
        gray = image;
    } else {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    }
    return gray;
}

static cv::Mat QfdNativeToGray32(const cv::Mat& image)
{
    cv::Mat gray = QfdNativeToSingleChannelGray(image);
    cv::Mat gray32;
    if (gray.depth() == CV_32F) {
        gray32 = gray.clone();
    } else {
        gray.convertTo(gray32, CV_32F);
    }
    return gray32;
}

static cv::Mat QfdNativeNormalizeTo8U(const cv::Mat& gray32)
{
    CV_Assert(gray32.type() == CV_32F);
    double minv = 0.0;
    double maxv = 0.0;
    cv::minMaxLoc(gray32, &minv, &maxv);
    cv::Mat out(gray32.size(), CV_8U, cv::Scalar(0));
    if (!(maxv > minv)) {
        return out;
    }

    cv::Mat shifted = gray32 - static_cast<float>(minv);
    shifted.convertTo(out, CV_8U, 255.0 / (maxv - minv));
    return out;
}

static double QfdNativeMedianFromSorted(const std::vector<double>& values)
{
    if (values.empty()) return 0.0;
    const size_t middle = values.size() / 2;
    if ((values.size() % 2) == 0) {
        return 0.5 * (values[middle - 1] + values[middle]);
    }
    return values[middle];
}

static double QfdNativeStandardDeviation(const std::vector<double>& values, double mean)
{
    if (values.empty()) return 0.0;
    double sumSquares = 0.0;
    for (double value : values) {
        const double delta = value - mean;
        sumSquares += delta * delta;
    }
    return std::sqrt(sumSquares / static_cast<double>(values.size()));
}

static std::pair<double, double> QfdNativeEstimateBackground(const std::vector<double>& pixelValues)
{
    constexpr int kMaxIterations = 3;
    constexpr double kSigmaThreshold = 3.0;

    if (pixelValues.empty()) {
        return {0.0, 0.0};
    }

    std::vector<double> values = pixelValues;
    for (int iter = 0; iter < kMaxIterations && values.size() > 1; ++iter) {
        std::sort(values.begin(), values.end());
        const double median = QfdNativeMedianFromSorted(values);

        std::vector<double> deviations;
        deviations.reserve(values.size());
        for (double value : values) {
            deviations.push_back(std::abs(value - median));
        }
        std::sort(deviations.begin(), deviations.end());

        const double mad = QfdNativeMedianFromSorted(deviations);
        const double sigma = mad > 0.0 ? 1.4826 * mad : QfdNativeStandardDeviation(values, median);
        if (!(sigma > 0.0)) {
            return {median, 0.0};
        }

        std::vector<double> clipped;
        clipped.reserve(values.size());
        for (double value : values) {
            if (std::abs(value - median) <= kSigmaThreshold * sigma) {
                clipped.push_back(value);
            }
        }
        if (clipped.empty() || clipped.size() == values.size()) {
            return {median, sigma};
        }
        values.swap(clipped);
    }

    std::sort(values.begin(), values.end());
    const double median = QfdNativeMedianFromSorted(values);
    return {median, QfdNativeStandardDeviation(values, median)};
}

static double QfdNativeGetMeasurementRadius(const cv::Point2d& center,
                                            const std::vector<QfdNativePixelData>& pixelData,
                                            double starRadius)
{
    if (pixelData.empty()) return 1.0;

    int minX = pixelData.front().x;
    int maxX = pixelData.front().x;
    int minY = pixelData.front().y;
    int maxY = pixelData.front().y;
    for (const auto& p : pixelData) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }

    const double availableRadius = std::min(
        std::min(center.x - minX, maxX - center.x),
        std::min(center.y - minY, maxY - center.y)) - 0.5;
    const double requestedRadius = std::max(starRadius * 1.5, 2.0);
    return std::max(1.0, std::min(requestedRadius, availableRadius));
}

static std::pair<cv::Point2d, double> QfdNativeCalculateCentroid(
    const std::vector<QfdNativePixelData>& pixelData,
    const cv::Point2d& initialCenter,
    double radius,
    double surroundingMean,
    bool iterate)
{
    constexpr int kCentroidMaxIterations = 3;
    constexpr double kCentroidConvergencePixels = 0.01;

    const double sigma = std::max(radius / 2.0, 1.0);
    cv::Point2d currentCenter = initialCenter;
    const int maxIterations = iterate ? kCentroidMaxIterations : 1;
    double lastTotalFlux = 0.0;

    for (int iteration = 0; iteration < maxIterations; ++iteration) {
        double sumWeightedFlux = 0.0;
        double sumX = 0.0;
        double sumY = 0.0;
        double sumFlux = 0.0;

        for (const auto& data : pixelData) {
            const double dx = static_cast<double>(data.x) - currentCenter.x;
            const double dy = static_cast<double>(data.y) - currentCenter.y;
            const double distanceSquared = dx * dx + dy * dy;
            if (distanceSquared > radius * radius) {
                continue;
            }

            const double flux = data.value - surroundingMean;
            if (flux <= 0.0) {
                continue;
            }

            const double window = std::exp(-0.5 * distanceSquared / (sigma * sigma));
            const double weightedFlux = flux * window;
            sumWeightedFlux += weightedFlux;
            sumX += static_cast<double>(data.x) * weightedFlux;
            sumY += static_cast<double>(data.y) * weightedFlux;
            sumFlux += flux;
        }

        if (!(sumWeightedFlux > 0.0)) {
            return {currentCenter, 0.0};
        }

        const cv::Point2d refinedCenter(sumX / sumWeightedFlux, sumY / sumWeightedFlux);
        lastTotalFlux = sumFlux;
        if (!iterate || cv::norm(refinedCenter - currentCenter) <= kCentroidConvergencePixels) {
            return {refinedCenter, lastTotalFlux};
        }

        currentCenter = refinedCenter;
    }

    return {currentCenter, lastTotalFlux};
}

static std::vector<QfdNativeRadialSample> QfdNativeCollectRadialSamples(
    const std::vector<QfdNativePixelData>& pixelData,
    const cv::Point2d& center,
    double radius,
    double surroundingMean)
{
    std::vector<QfdNativeRadialSample> samples;
    samples.reserve(pixelData.size());

    for (const auto& data : pixelData) {
        const double dx = static_cast<double>(data.x) - center.x;
        const double dy = static_cast<double>(data.y) - center.y;
        const double distance = std::sqrt(dx * dx + dy * dy);
        if (distance > radius) {
            continue;
        }

        const double flux = data.value - surroundingMean;
        samples.push_back(QfdNativeRadialSample{
            distance,
            flux,
            std::max(flux, 0.0),
            data.x,
            data.y
        });
    }

    return samples;
}

static double QfdNativeCalculateHalfFluxRadius(std::vector<QfdNativeRadialSample> samples, double totalFlux)
{
    if (!(totalFlux > 0.0) || samples.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    std::sort(samples.begin(), samples.end(),
              [](const auto& a, const auto& b) { return a.distance < b.distance; });

    const double targetFlux = totalFlux / 2.0;
    double cumulativeFlux = 0.0;
    double previousFlux = 0.0;
    double previousRadius = 0.0;

    for (const auto& sample : samples) {
        if (sample.positiveFlux <= 0.0) {
            continue;
        }

        cumulativeFlux += sample.positiveFlux;
        if (cumulativeFlux < targetFlux) {
            previousFlux = cumulativeFlux;
            previousRadius = sample.distance;
            continue;
        }

        if (cumulativeFlux == previousFlux || sample.distance <= previousRadius) {
            return sample.distance;
        }

        const double fraction = (targetFlux - previousFlux) / (cumulativeFlux - previousFlux);
        return previousRadius + fraction * (sample.distance - previousRadius);
    }

    return samples.back().distance;
}

static double QfdNativeCalculateFwhm(const std::vector<QfdNativeRadialSample>& samples,
                                     double maxPixelValue,
                                     double surroundingMean)
{
    constexpr double kRadialProfileBinWidth = 0.5;

    if (samples.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    const double peakFlux = maxPixelValue - surroundingMean;
    if (!(peakFlux > 0.0)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    const double halfMaximum = peakFlux / 2.0;
    double maxDistance = 0.0;
    for (const auto& sample : samples) {
        maxDistance = std::max(maxDistance, sample.distance);
    }
    const int binCount = static_cast<int>(std::ceil(maxDistance / kRadialProfileBinWidth)) + 1;
    std::vector<double> sums(static_cast<size_t>(binCount), 0.0);
    std::vector<double> distanceSums(static_cast<size_t>(binCount), 0.0);
    std::vector<int> counts(static_cast<size_t>(binCount), 0);

    for (const auto& sample : samples) {
        const int index = std::min(static_cast<int>(std::floor(sample.distance / kRadialProfileBinWidth)),
                                   binCount - 1);
        sums[static_cast<size_t>(index)] += sample.rawFlux;
        distanceSums[static_cast<size_t>(index)] += sample.distance;
        counts[static_cast<size_t>(index)]++;
    }

    double previousRadius = 0.0;
    double previousValue = peakFlux;
    for (int i = 0; i < binCount; ++i) {
        if (counts[static_cast<size_t>(i)] == 0) {
            continue;
        }

        const double radius = distanceSums[static_cast<size_t>(i)] / counts[static_cast<size_t>(i)];
        double value = sums[static_cast<size_t>(i)] / counts[static_cast<size_t>(i)];
        if (i > 0 && value > previousValue) {
            value = previousValue;
        }

        if (value <= halfMaximum) {
            if (std::abs(value - previousValue) < std::numeric_limits<double>::epsilon()
                || radius <= previousRadius) {
                return 2.0 * radius;
            }
            const double fraction = (halfMaximum - previousValue) / (value - previousValue);
            const double halfMaxRadius = previousRadius + fraction * (radius - previousRadius);
            return 2.0 * halfMaxRadius;
        }

        previousRadius = radius;
        previousValue = value;
    }

    return std::numeric_limits<double>::quiet_NaN();
}

static double QfdNativeCalculateMomentEccentricity(const std::vector<QfdNativeRadialSample>& samples,
                                                   const cv::Point2d& center)
{
    double momentXX = 0.0;
    double momentYY = 0.0;
    double momentXY = 0.0;
    double totalFlux = 0.0;

    for (const auto& sample : samples) {
        if (sample.positiveFlux <= 0.0) {
            continue;
        }

        const double dx = static_cast<double>(sample.x) - center.x;
        const double dy = static_cast<double>(sample.y) - center.y;
        totalFlux += sample.positiveFlux;
        momentXX += sample.positiveFlux * dx * dx;
        momentYY += sample.positiveFlux * dy * dy;
        momentXY += sample.positiveFlux * dx * dy;
    }

    if (!(totalFlux > 0.0)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    momentXX /= totalFlux;
    momentYY /= totalFlux;
    momentXY /= totalFlux;

    const double trace = momentXX + momentYY;
    const double determinantTerm =
        (momentXX - momentYY) * (momentXX - momentYY) + 4.0 * momentXY * momentXY;
    const double root = std::sqrt(std::max(0.0, determinantTerm));
    const double major = (trace + root) / 2.0;
    double minor = (trace - root) / 2.0;

    if (!(major > 0.0)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    minor = std::max(0.0, minor);
    return std::sqrt(std::max(0.0, 1.0 - (minor / major)));
}

static bool QfdNativeInsideCircle(double x, double y,
                                  double centerX, double centerY,
                                  double radius)
{
    const double dx = x - centerX;
    const double dy = y - centerY;
    return (dx * dx + dy * dy) <= (radius * radius);
}

static bool QfdNativeMeasureStar(QfdNativeMeasuredStar& star,
                                 const std::vector<QfdNativePixelData>& pixelData)
{
    star.hfr = std::numeric_limits<double>::quiet_NaN();
    star.fwhm = std::numeric_limits<double>::quiet_NaN();
    star.eccentricity = std::numeric_limits<double>::quiet_NaN();
    star.averageBrightness = std::numeric_limits<double>::quiet_NaN();
    star.totalFlux = 0.0;
    star.supportArea = 0;

    if (pixelData.empty()) {
        return false;
    }

    const double centroidRadius = QfdNativeGetMeasurementRadius(star.position, pixelData, star.radius);
    const auto centroid = QfdNativeCalculateCentroid(pixelData, star.position,
                                                     centroidRadius, star.surroundingMean, true);
    if (!(centroid.second > 0.0)) {
        return false;
    }

    star.position = centroid.first;
    const double measurementRadius =
        QfdNativeGetMeasurementRadius(star.position, pixelData, star.radius);
    auto radialSamples =
        QfdNativeCollectRadialSamples(pixelData, star.position, measurementRadius, star.surroundingMean);
    if (radialSamples.empty()) {
        return false;
    }

    double totalFlux = 0.0;
    int positiveSampleCount = 0;
    for (const auto& sample : radialSamples) {
        totalFlux += sample.positiveFlux;
        if (sample.positiveFlux > 0.0) {
            positiveSampleCount++;
        }
    }
    if (!(totalFlux > 0.0)) {
        return false;
    }

    star.totalFlux = totalFlux;
    star.supportArea = positiveSampleCount;
    star.averageBrightness = positiveSampleCount > 0
        ? totalFlux / static_cast<double>(positiveSampleCount)
        : std::numeric_limits<double>::quiet_NaN();
    star.hfr = QfdNativeCalculateHalfFluxRadius(radialSamples, totalFlux);
    star.fwhm = QfdNativeCalculateFwhm(radialSamples, star.maxPixelValue, star.surroundingMean);
    star.eccentricity = QfdNativeCalculateMomentEccentricity(radialSamples, star.position);

    return std::isfinite(star.hfr) && star.hfr > 0.0;
}

static double QfdNativeRectEccentricity(const cv::Rect& rect)
{
    const double major = static_cast<double>(std::max(rect.width, rect.height));
    const double minor = static_cast<double>(std::min(rect.width, rect.height));
    if (!(major > 0.0)) {
        return 1.0;
    }
    const double focus = std::sqrt(std::max(0.0, major * major - minor * minor));
    return focus / major;
}

static QfdNativeState QfdNativeBuildState(const cv::Mat& image16,
                                          double smoothSigma)
{
    constexpr int kMaxDetectWidth = 1552;

    QfdNativeState state;
    state.gray32 = QfdNativeToGray32(image16);
    state.resizeFactor = 1.0;
    if (state.gray32.cols > kMaxDetectWidth) {
        state.resizeFactor = static_cast<double>(kMaxDetectWidth) /
                             static_cast<double>(state.gray32.cols);
    }
    state.inverseResizeFactor = 1.0 / state.resizeFactor;
    state.minStarSize = std::max(2, static_cast<int>(std::floor(5.0 * state.resizeFactor)));
    state.maxStarSize = std::max(state.minStarSize + 1,
                                 static_cast<int>(std::ceil(150.0 * state.resizeFactor)));

    state.detect8 = QfdNativeNormalizeTo8U(state.gray32);
    if (smoothSigma > 0.0) {
        cv::GaussianBlur(state.detect8, state.detect8, cv::Size(), smoothSigma, smoothSigma);
    }
    if (state.resizeFactor != 1.0) {
        cv::resize(state.detect8, state.detect8, cv::Size(), state.resizeFactor,
                   state.resizeFactor, cv::INTER_AREA);
    }

    return state;
}

static std::vector<Tools::FocusedStar> detectPeakCentroidInternal(const cv::Mat& image16,
                                                                  int windowSize,
                                                                  bool verbose,
                                                                  DeviceType logDeviceType,
                                                                  const QString& logPrefix)
{
    std::vector<Tools::FocusedStar> stars;
    const std::string prefix = logPrefix.isEmpty()
        ? std::string("[PeakCentroid]")
        : logPrefix.toStdString();
    if (image16.empty()) return stars;

    cv::Mat gray = QfdNativeToSingleChannelGray(image16);
    cv::Mat gray32;
    if (gray.depth() == CV_32F) {
        gray32 = gray.clone();
    } else {
        gray.convertTo(gray32, CV_32F);
    }

    double maxVal = 0.0;
    cv::Point maxLoc(0, 0);
    cv::minMaxLoc(gray32, nullptr, &maxVal, nullptr, &maxLoc);
    if (!(maxVal > 0.0)) {
        if (verbose) {
            Logger::Log(prefix + " maxVal<=0, no valid peak", LogLevel::INFO, logDeviceType);
        }
        return stars;
    }

    if (windowSize < 3) windowSize = 3;
    if (windowSize % 2 != 0) windowSize += 1;
    const int half = windowSize / 2;
    const int x0 = std::max(0, maxLoc.x - half);
    const int y0 = std::max(0, maxLoc.y - half);
    const int x1 = std::min(gray32.cols, maxLoc.x + half);
    const int y1 = std::min(gray32.rows, maxLoc.y + half);
    if (x1 <= x0 || y1 <= y0) return stars;

    const cv::Rect roi(x0, y0, x1 - x0, y1 - y0);
    cv::Mat roi32 = gray32(roi).clone();

    cv::Mat bgMask(gray32.size(), CV_8U, cv::Scalar(255));
    bgMask(roi).setTo(cv::Scalar(0));
    cv::Scalar bgMeanScalar;
    cv::Scalar bgStdScalar;
    cv::meanStdDev(gray32, bgMeanScalar, bgStdScalar, bgMask);
    const double bgStd = bgStdScalar[0];

    double localMin = 0.0;
    double localMax = 0.0;
    cv::minMaxLoc(roi32, &localMin, &localMax);
    const cv::Scalar roiMeanScalar = cv::mean(roi32);
    const double localMean = roiMeanScalar[0];
    const double peakAboveMean = std::max(0.0, localMax - localMean);
    if (!(peakAboveMean > 0.0)) {
        if (verbose) {
            Logger::Log(prefix + " peakAboveMean<=0, fallback to maxLoc",
                        LogLevel::INFO, logDeviceType);
        }
        Tools::FocusedStar star{};
        star.x = static_cast<double>(maxLoc.x);
        star.y = static_cast<double>(maxLoc.y);
        star.flux = localMax;
        star.hfr = 0.0;
        star.radius = 0.0;
        star.area = 1;
        star.snr = 0.0;
        star.localMax = localMax;
        star.bgStd = bgStd;
        star.snrQuality = QStringLiteral("peak-only");
        stars.push_back(star);
        return stars;
    }

    const double threshold = localMean + peakAboveMean * 0.3;
    cv::Mat signal32 = roi32.clone();
    signal32 -= static_cast<float>(threshold);
    cv::threshold(signal32, signal32, 0.0, 0.0, cv::THRESH_TOZERO);

    double flux = 0.0;
    double sumX = 0.0;
    double sumY = 0.0;
    int supportArea = 0;
    for (int y = 0; y < signal32.rows; ++y) {
        const float* row = signal32.ptr<float>(y);
        for (int x = 0; x < signal32.cols; ++x) {
            const double w = std::max(0.0, static_cast<double>(row[x]));
            if (w <= 0.0) continue;
            flux += w;
            sumX += w * static_cast<double>(x0 + x);
            sumY += w * static_cast<double>(y0 + y);
            supportArea++;
        }
    }
    if (!(flux > 0.0)) {
        if (verbose) {
            Logger::Log(prefix + " threshold removed all support, fallback to maxLoc",
                        LogLevel::INFO, logDeviceType);
        }
        Tools::FocusedStar star{};
        star.x = static_cast<double>(maxLoc.x);
        star.y = static_cast<double>(maxLoc.y);
        star.flux = localMax;
        star.hfr = 0.0;
        star.radius = 0.0;
        star.area = 1;
        star.snr = 0.0;
        star.localMax = localMax;
        star.bgStd = bgStd;
        star.snrQuality = QStringLiteral("peak-only");
        stars.push_back(star);
        return stars;
    }

    const double cx = sumX / flux;
    const double cy = sumY / flux;
    const cv::Mat roiNorm = QfdNativeNormalizeTo8U(signal32);
    cv::Mat roiNorm32;
    roiNorm.convertTo(roiNorm32, CV_32F, 1.0 / 255.0);

    std::vector<float> vals;
    std::vector<float> rs;
    vals.reserve(static_cast<size_t>(roiNorm32.total()));
    rs.reserve(static_cast<size_t>(roiNorm32.total()));
    for (int y = 0; y < roiNorm32.rows; ++y) {
        const float* row = roiNorm32.ptr<float>(y);
        for (int x = 0; x < roiNorm32.cols; ++x) {
            const float v = row[x];
            if (v <= 0.0f) continue;
            vals.push_back(v);
            const double dx = static_cast<double>(x) - (cx - x0);
            const double dy = static_cast<double>(y) - (cy - y0);
            rs.push_back(static_cast<float>(std::sqrt(dx * dx + dy * dy)));
        }
    }

    double hfr = 0.0;
    if (!vals.empty()) {
        const double totalFlux = std::accumulate(vals.begin(), vals.end(), 0.0);
        if (totalFlux > 0.0) {
            std::vector<int> order(vals.size());
            std::iota(order.begin(), order.end(), 0);
            std::sort(order.begin(), order.end(),
                      [&](int a, int b) { return rs[a] < rs[b]; });
            const double halfFlux = totalFlux * 0.5;
            double acc = 0.0;
            for (int idx : order) {
                acc += vals[static_cast<size_t>(idx)];
                if (acc >= halfFlux) {
                    hfr = static_cast<double>(rs[static_cast<size_t>(idx)]);
                    break;
                }
            }
        }
    }
    const double radius = std::sqrt(static_cast<double>(std::max(1, supportArea)) / CV_PI);

    Tools::FocusedStar star{};
    star.x = cx;
    star.y = cy;
    star.flux = flux;
    star.hfr = hfr;
    star.radius = radius;
    star.area = supportArea;
    star.snr = (bgStd > 0.0) ? (localMax / bgStd) : 0.0;
    star.localMax = localMax;
    star.bgStd = bgStd;
    star.snrQuality = QStringLiteral("peak-centroid");
    stars.push_back(star);

    if (verbose) {
        Logger::Log(prefix + " maxLoc=(" + std::to_string(maxLoc.x) + "," + std::to_string(maxLoc.y) +
                        "), roi=(" + std::to_string(x0) + "," + std::to_string(y0) + "," +
                        std::to_string(roi.width) + "x" + std::to_string(roi.height) + ")" +
                        ", localMin=" + std::to_string(localMin) +
                        ", localMean=" + std::to_string(localMean) +
                        ", localMax=" + std::to_string(localMax) +
                        ", bgStd=" + std::to_string(bgStd) +
                        ", threshold=" + std::to_string(threshold) +
                        ", supportArea=" + std::to_string(supportArea) +
                        ", centroid=(" + std::to_string(cx) + "," + std::to_string(cy) + ")" +
                        ", flux=" + std::to_string(flux) +
                        ", hfr=" + std::to_string(hfr) +
                        ", snr=" + std::to_string(star.snr),
                    LogLevel::INFO, logDeviceType);
    }

    return stars;
}

static std::vector<Tools::FocusedStar> QfdNativeDetectFocusedStarsInternal(const cv::Mat& image16,
                                                                           double minSNR,
                                                                           double smoothSigma,
                                                                           bool verbose,
                                                                           DeviceType logDeviceType,
                                                                           const QString& logPrefix)
{
    std::vector<Tools::FocusedStar> out;
    const std::string prefix = logPrefix.isEmpty()
        ? std::string("[QfdNative]")
        : logPrefix.toStdString() + " [QfdNative]";

    if (image16.empty()) {
        return out;
    }

    const QfdNativeState state = QfdNativeBuildState(image16, smoothSigma);
    if (state.detect8.empty() || state.gray32.empty()) {
        return out;
    }

    cv::Mat edges;
    cv::Canny(state.detect8, edges, 10, 80);
    cv::Mat binary;
    cv::dilate(edges, binary, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3)));

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(binary, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    std::vector<QfdNativeMeasuredStar> measuredStars;
    measuredStars.reserve(contours.size());

    double sumRadius = 0.0;
    double sumSquares = 0.0;
    const int minimumNumberOfPixels =
        static_cast<int>(std::ceil(std::max(state.gray32.cols, state.gray32.rows) / 1000.0));

    for (const auto& contour : contours) {
        if (contour.empty()) {
            continue;
        }

        const cv::Rect blobRect = cv::boundingRect(contour);
        if (blobRect.width > state.maxStarSize
            || blobRect.height > state.maxStarSize
            || blobRect.width < state.minStarSize
            || blobRect.height < state.minStarSize) {
            continue;
        }

        cv::Point2f centerPoint;
        float radius = 0.0f;
        cv::minEnclosingCircle(contour, centerPoint, radius);

        QfdNativeMeasuredStar star;
        const int rectX =
            std::max(0, static_cast<int>(std::floor(blobRect.x * state.inverseResizeFactor)));
        const int rectY =
            std::max(0, static_cast<int>(std::floor(blobRect.y * state.inverseResizeFactor)));
        const int rectW = std::min(
            state.gray32.cols - rectX,
            static_cast<int>(std::ceil(blobRect.width * state.inverseResizeFactor)));
        const int rectH = std::min(
            state.gray32.rows - rectY,
            static_cast<int>(std::ceil(blobRect.height * state.inverseResizeFactor)));
        const cv::Rect rect(rectX, rectY, rectW, rectH);
        if (rect.width <= 0 || rect.height <= 0) {
            continue;
        }

        const double shapeEccentricity = QfdNativeRectEccentricity(rect);
        if (shapeEccentricity > 0.8) {
            continue;
        }

        star.rect = rect;
        star.position = cv::Point2d(centerPoint.x * state.inverseResizeFactor,
                                    centerPoint.y * state.inverseResizeFactor);
        star.radius = radius * state.inverseResizeFactor;
        if (!(star.radius > 0.0)) {
            star.radius = std::max(rect.width, rect.height) / 2.0;
        }

        const int largeRectXPos = std::max(rect.x - rect.width, 0);
        const int largeRectYPos = std::max(rect.y - rect.height, 0);
        const int largeRectWidth = std::min(state.gray32.cols - largeRectXPos, rect.width * 3);
        const int largeRectHeight = std::min(state.gray32.rows - largeRectYPos, rect.height * 3);
        if (largeRectWidth <= 0 || largeRectHeight <= 0) {
            continue;
        }
        const cv::Rect largeRect(largeRectXPos, largeRectYPos, largeRectWidth, largeRectHeight);

        std::vector<QfdNativePixelData> pixelData;
        pixelData.reserve(static_cast<size_t>(largeRect.area()));
        std::vector<double> backgroundPixelValues;
        std::vector<double> innerStarPixelValues;
        backgroundPixelValues.reserve(static_cast<size_t>(largeRect.area()));
        innerStarPixelValues.reserve(static_cast<size_t>(rect.area()));

        double starPixelSum = 0.0;
        int starPixelCount = 0;

        for (int y = largeRect.y; y < largeRect.y + largeRect.height; ++y) {
            const float* row = state.gray32.ptr<float>(y);
            for (int x = largeRect.x; x < largeRect.x + largeRect.width; ++x) {
                const double pixelValue = static_cast<double>(row[x]);
                if (x >= star.rect.x && x < star.rect.x + star.rect.width &&
                    y >= star.rect.y && y < star.rect.y + star.rect.height) {
                    if (QfdNativeInsideCircle(static_cast<double>(x), static_cast<double>(y),
                                              star.position.x, star.position.y, star.radius)) {
                        starPixelSum += pixelValue;
                        starPixelCount++;
                        innerStarPixelValues.push_back(pixelValue);
                        star.maxPixelValue = std::max(star.maxPixelValue, pixelValue);
                    }
                } else {
                    backgroundPixelValues.push_back(pixelValue);
                }
                pixelData.push_back(QfdNativePixelData{x, y, pixelValue});
            }
        }

        if (starPixelCount == 0 || backgroundPixelValues.empty()) {
            continue;
        }

        star.meanBrightness = starPixelSum / static_cast<double>(starPixelCount);
        const auto backgroundStats = QfdNativeEstimateBackground(backgroundPixelValues);
        star.surroundingMean = backgroundStats.first;
        star.backgroundSigma = backgroundStats.second;

        int brightPixelCount = 0;
        const double brightThreshold = star.surroundingMean + 1.5 * star.backgroundSigma;
        for (double value : innerStarPixelValues) {
            if (value > brightThreshold) {
                brightPixelCount++;
            }
        }

        if (star.meanBrightness >= star.surroundingMean + std::min(0.1 * star.surroundingMean,
                                                                    star.backgroundSigma)
            && brightPixelCount > minimumNumberOfPixels
            && QfdNativeMeasureStar(star, pixelData)) {
            if (star.position.x > (star.rect.x + 1)
                && star.position.y > (star.rect.y + 1)
                && star.position.x < (star.rect.x + star.rect.width - 2)
                && star.position.y < (star.rect.y + star.rect.height - 2)) {
                star.snr = star.backgroundSigma > 0.0
                    ? (star.maxPixelValue - star.surroundingMean) / star.backgroundSigma
                    : 0.0;
                if (star.snr >= minSNR) {
                    sumRadius += star.radius;
                    sumSquares += star.radius * star.radius;
                    measuredStars.push_back(star);
                }
            }
        }
    }

    if (measuredStars.empty()) {
        if (verbose) {
            Logger::Log(prefix + " no valid stars from native pipeline",
                        LogLevel::INFO, logDeviceType);
        }
        return out;
    }

    const double avgRadius = sumRadius / static_cast<double>(measuredStars.size());
    const double radiusStdev = std::sqrt(std::max(
        0.0,
        (sumSquares - measuredStars.size() * avgRadius * avgRadius) /
            static_cast<double>(measuredStars.size())));

    std::vector<QfdNativeMeasuredStar> filteredStars;
    filteredStars.reserve(measuredStars.size());
    for (const auto& star : measuredStars) {
        if (star.radius <= avgRadius + 1.5 * radiusStdev
            && star.radius >= avgRadius - 1.5 * radiusStdev) {
            filteredStars.push_back(star);
        }
    }
    if (filteredStars.empty()) {
        filteredStars = measuredStars;
    }

    std::sort(filteredStars.begin(), filteredStars.end(),
              [](const auto& a, const auto& b) {
                  if (a.maxPixelValue != b.maxPixelValue) {
                      return a.maxPixelValue > b.maxPixelValue;
                  }
                  return a.totalFlux > b.totalFlux;
              });

    out.reserve(filteredStars.size());
    for (const auto& star : filteredStars) {
        Tools::FocusedStar focusedStar{};
        focusedStar.x = star.position.x;
        focusedStar.y = star.position.y;
        focusedStar.flux = star.totalFlux;
        focusedStar.hfr = std::isfinite(star.hfr) ? star.hfr : 0.0;
        focusedStar.radius = star.radius;
        focusedStar.area = std::max(1, star.supportArea);
        focusedStar.snr = star.snr;
        focusedStar.localMax = star.maxPixelValue;
        focusedStar.bgStd = star.backgroundSigma;
        focusedStar.snrQuality = QStringLiteral("qfd-native");
        out.push_back(focusedStar);
    }

    if (verbose) {
        const auto& best = filteredStars.front();
        Logger::Log(prefix +
                        " detectedStars=" + std::to_string(out.size()) +
                        ", resizeFactor=" + std::to_string(state.resizeFactor) +
                        ", best=(" + std::to_string(best.position.x) + "," + std::to_string(best.position.y) + ")" +
                        ", bestHFR=" + std::to_string(best.hfr) +
                        ", bestFWHM=" + std::to_string(best.fwhm) +
                        ", bestEcc=" + std::to_string(best.eccentricity) +
                        ", bestSNR=" + std::to_string(best.snr),
                    LogLevel::INFO, logDeviceType);
    }

    return out;
}

} // namespace

std::vector<Tools::FocusedStar> Tools::DetectFocusedStars(const cv::Mat& image16,
                                                          double kSigma,
                                                          int minArea,
                                                          int maxArea,
                                                          double minSNR,
                                                          int bgKsize,
                                                          double smoothSigma,
                                                          bool verbose,
                                                          DeviceType logDeviceType,
                                                          const QString& logPrefix)
{
    Q_UNUSED(kSigma);
    Q_UNUSED(minArea);
    Q_UNUSED(maxArea);
    Q_UNUSED(bgKsize);

    auto stars = QfdNativeDetectFocusedStarsInternal(image16,
                                                     minSNR,
                                                     smoothSigma,
                                                     verbose,
                                                     logDeviceType,
                                                     logPrefix);
    if (!stars.empty()) {
        return stars;
    }
    return detectPeakCentroidInternal(image16, 50, verbose, logDeviceType, logPrefix);
}

int Tools::DetectFocusedStarsFromFITS(const char* fileName,
                                      std::vector<Tools::FocusedStar>& outStars,
                                      bool verbose)
{
    outStars.clear();
    cv::Mat img;
    const int status = Tools::readFits(fileName, img);
    if (status != 0) return status;
    outStars = Tools::DetectFocusedStars(img,
                                         3.5,
                                         3,
                                         200,
                                         3.0,
                                         51,
                                         1.0,
                                         verbose,
                                         DeviceType::MAIN,
                                         QStringLiteral("[FocusedStarsFromFITS]"));
    return 0;
}

double Tools::MedianHFR(const std::vector<Tools::FocusedStar>& stars)
{
    if (stars.empty()) return 0.0;
    std::vector<double> hfrs;
    hfrs.reserve(stars.size());
    for (const auto& s : stars) {
        if (s.hfr > 0.0 && std::isfinite(s.hfr)) hfrs.push_back(s.hfr);
    }
    if (hfrs.empty()) return 0.0;
    std::sort(hfrs.begin(), hfrs.end());
    const size_t n = hfrs.size();
    if ((n % 2) == 1) return hfrs[n / 2];
    return 0.5 * (hfrs[n / 2 - 1] + hfrs[n / 2]);
}
