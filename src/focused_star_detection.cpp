// 简化版 ROI 星点检测：
// 1) 在整幅 ROI 图上寻找全局峰值
// 2) 在峰值附近固定窗口内，以像素强度做加权质心
// 3) 输出单颗星点结果
#include "tools.h"

#include <algorithm>
#include <limits>
#include <numeric>

namespace {

static cv::Mat toSingleChannelGray(const cv::Mat& image)
{
    cv::Mat gray;
    if (image.channels() == 1) {
        gray = image;
    } else {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    }
    return gray;
}

static cv::Mat toFloat32Normalized(const cv::Mat& image)
{
    cv::Mat gray = toSingleChannelGray(image);
    cv::Mat f32;
    if (gray.depth() == CV_32F) {
        f32 = gray.clone();
    } else {
        gray.convertTo(f32, CV_32F);
    }
    double minv = 0.0;
    double maxv = 0.0;
    cv::minMaxLoc(f32, &minv, &maxv);
    if (maxv > 0.0) {
        f32 /= static_cast<float>(maxv);
    }
    return f32;
}

static double computeHFR(const cv::Mat& roiNorm, double cx, double cy)
{
    CV_Assert(roiNorm.type() == CV_32F);
    std::vector<float> vals;
    std::vector<float> rs;
    vals.reserve(static_cast<size_t>(roiNorm.total()));
    rs.reserve(static_cast<size_t>(roiNorm.total()));

    for (int y = 0; y < roiNorm.rows; ++y) {
        const float* row = roiNorm.ptr<float>(y);
        for (int x = 0; x < roiNorm.cols; ++x) {
            const float v = row[x];
            if (v <= 0.0f) continue;
            vals.push_back(v);
            const double dx = static_cast<double>(x) - cx;
            const double dy = static_cast<double>(y) - cy;
            rs.push_back(static_cast<float>(std::sqrt(dx * dx + dy * dy)));
        }
    }
    if (vals.empty()) return 0.0;

    const double totalFlux = std::accumulate(vals.begin(), vals.end(), 0.0);
    if (totalFlux <= 0.0) return 0.0;

    std::vector<int> order(vals.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return rs[a] < rs[b]; });

    const double halfFlux = totalFlux * 0.5;
    double acc = 0.0;
    for (int idx : order) {
        acc += vals[idx];
        if (acc >= halfFlux) {
            return static_cast<double>(rs[idx]);
        }
    }
    return static_cast<double>(rs[order.back()]);
}

static std::vector<Tools::FocusedStar> detectPeakCentroidInternal(const cv::Mat& image16,
                                                                  int windowSize,
                                                                  bool verbose)
{
    std::vector<Tools::FocusedStar> stars;
    if (image16.empty()) return stars;

    cv::Mat gray = toSingleChannelGray(image16);
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
            Logger::Log("[PeakCentroid] maxVal<=0, no valid peak", LogLevel::INFO, DeviceType::MAIN);
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
            Logger::Log("[PeakCentroid] peakAboveMean<=0, fallback to maxLoc",
                        LogLevel::INFO, DeviceType::MAIN);
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

    // 天文图像里窗口大部分通常是本底，用区域均值近似本底，
    // 再取 (峰值-均值)*0.3 作为阈值，较半高宽更宽松一些，减少均值被星点抬高后的偏差。
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
            Logger::Log("[PeakCentroid] threshold removed all support, fallback to maxLoc",
                        LogLevel::INFO, DeviceType::MAIN);
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
    const cv::Mat roiNorm = toFloat32Normalized(signal32);
    const double hfr = computeHFR(roiNorm, cx - x0, cy - y0);
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
        Logger::Log("[PeakCentroid] maxLoc=(" + std::to_string(maxLoc.x) + "," + std::to_string(maxLoc.y) +
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
                    LogLevel::INFO, DeviceType::MAIN);
    }

    return stars;
}

} // namespace

std::vector<Tools::FocusedStar> Tools::DetectFocusedStars(const cv::Mat& image16,
                                                          double kSigma,
                                                          int minArea,
                                                          int maxArea,
                                                          double minSNR,
                                                          int bgKsize,
                                                          double smoothSigma,
                                                          bool verbose)
{
    Q_UNUSED(kSigma);
    Q_UNUSED(minArea);
    Q_UNUSED(maxArea);
    Q_UNUSED(minSNR);
    Q_UNUSED(bgKsize);
    Q_UNUSED(smoothSigma);
    return detectPeakCentroidInternal(image16, 50, verbose);
}

int Tools::DetectFocusedStarsFromFITS(const char* fileName,
                                      std::vector<Tools::FocusedStar>& outStars,
                                      bool verbose)
{
    outStars.clear();
    cv::Mat img;
    const int status = Tools::readFits(fileName, img);
    if (status != 0) return status;
    outStars = detectPeakCentroidInternal(img, 50, verbose);
    return 0;
}

double Tools::MedianHFR(const std::vector<Tools::FocusedStar>& stars)
{
    if (stars.empty()) return 0.0;
    std::vector<double> hfrs;
    hfrs.reserve(stars.size());
    for (const auto& s : stars) {
        if (s.hfr > 0.0) hfrs.push_back(s.hfr);
    }
    if (hfrs.empty()) return 0.0;
    std::sort(hfrs.begin(), hfrs.end());
    const size_t n = hfrs.size();
    if (n % 2 == 1) return hfrs[n / 2];
    return 0.5 * (hfrs[n / 2 - 1] + hfrs[n / 2]);
}
