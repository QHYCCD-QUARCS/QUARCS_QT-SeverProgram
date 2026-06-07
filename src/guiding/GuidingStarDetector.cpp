#include "GuidingStarDetector.h"
#include "CentroidUtils.h"

#include "../tools.h"
#include "../Logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <set>
#include <sstream>
#include <tuple>

namespace guiding {

namespace {

struct Peak
{
    int x = 0;
    int y = 0;
    float val = 0.0f;
};

struct R2M
{
    double r2 = 0.0;
    QPoint p;
    double m = 0.0;
};

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

QPointF refineCandidateCentroid(const cv::Mat& image16, const StarCandidate& c, bool* ok)
{
    if (ok) *ok = false;
    if (image16.empty())
        return QPointF(c.x, c.y);

    const QPointF approx(c.x, c.y);
    QPointF centroid;
    const int strictHalf = std::max(8, static_cast<int>(std::lround(std::clamp(c.hfd, 2.0, 8.0))));
    const int relaxedHalf = std::max(16, strictHalf * 2);

    const bool found =
        guiding::FindCentroidWeightedStrict(image16, approx, strictHalf, centroid, 2.0) ||
        guiding::FindCentroidWeighted(image16,       approx, strictHalf, centroid, 2.0) ||
        guiding::FindCentroidWeighted(image16,       approx, relaxedHalf, centroid, 2.0);

    if (ok) *ok = found;
    return found ? centroid : approx;
}

bool isCloseDuplicate(const StarCandidate& a, const StarCandidate& b)
{
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    return (dx * dx + dy * dy) < (25.0 * 25.0);
}

double computeHfr(std::vector<R2M>& vec, double cx, double cy, double mass)
{
    if (vec.size() == 1)
        return 0.25;

    for (auto& it : vec)
    {
        const double dx = static_cast<double>(it.p.x()) - cx;
        const double dy = static_cast<double>(it.p.y()) - cy;
        it.r2 = dx * dx + dy * dy;
    }
    std::sort(vec.begin(), vec.end(), [](const R2M& a, const R2M& b) { return a.r2 < b.r2; });

    double r20 = 0.0, r21 = 0.0, m0 = 0.0, m1 = 0.0;
    const double halfm = 0.5 * mass;
    for (const auto& rm : vec)
    {
        r20 = r21;
        m0 = m1;
        r21 = rm.r2;
        m1 += rm.m;
        if (m1 > halfm)
            break;
    }

    if (m1 > m0)
    {
        const double r0 = std::sqrt(r20);
        const double r1 = std::sqrt(r21);
        const double s = (r1 - r0) / (m1 - m0);
        return r0 + s * (halfm - m0);
    }
    return 0.25;
}

double localPeakADUAt(const cv::Mat& img, double x, double y, int halfSizePx)
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

cv::Mat toFloat32(const cv::Mat& image)
{
    cv::Mat f;
    image.convertTo(f, CV_32F);
    return f;
}

cv::Mat phd2Downsample(const cv::Mat& src32f, int downsample)
{
    if (downsample <= 1)
        return src32f;

    const int dw = src32f.cols / downsample;
    const int dh = src32f.rows / downsample;
    if (dw <= 0 || dh <= 0)
        return src32f;

    cv::Mat dst(dh, dw, CV_32F, cv::Scalar(0));
    const float area = static_cast<float>(downsample * downsample);
    for (int yy = 0; yy < dh; ++yy)
    {
        float* out = dst.ptr<float>(yy);
        for (int xx = 0; xx < dw; ++xx)
        {
            float sum = 0.0f;
            for (int j = 0; j < downsample; ++j)
            {
                const float* row = src32f.ptr<float>(yy * downsample + j);
                for (int i = 0; i < downsample; ++i)
                    sum += row[xx * downsample + i];
            }
            out[xx] = sum / area;
        }
    }
    return dst;
}

cv::Mat phd2PsfConvolution(const cv::Mat& src32f)
{
    cv::Mat dst(src32f.size(), CV_32F, cv::Scalar(0));
    static const double PSF[] = { 0.906, 0.584, 0.365, 0.117, 0.049, -0.05, -0.064, -0.074, -0.094 };
    constexpr int psfSize = 4;

    for (int y = psfSize; y < src32f.rows - psfSize; ++y)
    {
        for (int x = psfSize; x < src32f.cols - psfSize; ++x)
        {
            auto px = [&](int dx, int dy) -> float { return src32f.at<float>(y + dy, x + dx); };

            const double A = px(0, 0);
            const double B1 = px(0, -1) + px(0, 1) + px(1, 0) + px(-1, 0);
            const double B2 = px(-1, -1) + px(1, -1) + px(-1, 1) + px(1, 1);
            const double C1 = px(0, -2) + px(-2, 0) + px(2, 0) + px(0, 2);
            const double C2 = px(-1, -2) + px(1, -2) + px(-2, -1) + px(2, -1) + px(-2, 1) + px(2, 1) + px(-1, 2) + px(1, 2);
            const double C3 = px(-2, -2) + px(2, -2) + px(-2, 2) + px(2, 2);
            const double D1 = px(0, -3) + px(-3, 0) + px(3, 0) + px(0, 3);
            const double D2 = px(-1, -3) + px(1, -3) + px(-3, -1) + px(3, -1) + px(-3, 1) + px(3, 1) + px(-1, 3) + px(1, 3);
            double D3 = px(-4, -2) + px(-3, -2) + px(3, -2) + px(4, -2) + px(-4, -1) + px(4, -1) +
                        px(-4, 0) + px(4, 0) + px(-4, 1) + px(4, 1) + px(-4, 2) + px(-3, 2) + px(3, 2) + px(4, 2);

            for (int i = -4; i <= 4; ++i)
            {
                D3 += px(i, -4);
                D3 += px(i, 4);
            }
            for (int i = -4; i <= -2; ++i)
            {
                D3 += px(i, -3);
                D3 += px(i, 3);
            }
            for (int i = 2; i <= 4; ++i)
            {
                D3 += px(i, -3);
                D3 += px(i, 3);
            }

            const double mean = (A + B1 + B2 + C1 + C2 + C3 + D1 + D2 + D3) / 81.0;
            const double fit =
                PSF[0] * (A - mean) +
                PSF[1] * (B1 - 4.0 * mean) +
                PSF[2] * (B2 - 4.0 * mean) +
                PSF[3] * (C1 - 4.0 * mean) +
                PSF[4] * (C2 - 8.0 * mean) +
                PSF[5] * (C3 - 4.0 * mean) +
                PSF[6] * (D1 - 4.0 * mean) +
                PSF[7] * (D2 - 8.0 * mean) +
                PSF[8] * (D3 - 44.0 * mean);
            dst.at<float>(y, x) = static_cast<float>(fit);
        }
    }
    return dst;
}

std::pair<double, double> rectStats(const cv::Mat& img32f, const cv::Rect& rect)
{
    cv::Scalar mean, stddev;
    cv::meanStdDev(img32f(rect), mean, stddev);
    return {mean[0], stddev[0]};
}

int resolveAutoSelDownsample(const StarSelectionParams& p)
{
    if (p.autoSelDownsample > 0)
        return p.autoSelDownsample;

    constexpr double kDownsampleScaleThresh = 0.6; // PHD2 DOWNSAMPLE_SCALE_THRESH
    if (p.autoSelPixelScaleArcsecPerPixel > 0.0 && p.autoSelPixelScaleArcsecPerPixel <= kDownsampleScaleThresh)
        return 2;
    return 1;
}

std::vector<Peak> detectPHD2StylePeaks(const cv::Mat& image16, int searchRegionPx, int downsample)
{
    std::vector<Peak> stars;
    if (image16.empty())
        return stars;

    cv::Mat medianed;
    cv::medianBlur(image16, medianed, 3);
    cv::Mat conv = toFloat32(medianed);
    conv = phd2Downsample(conv, std::max(1, downsample));
    conv = phd2PsfConvolution(conv);

    constexpr int convRadius = 4;
    const cv::Rect convRect(convRadius, convRadius, conv.cols - 2 * convRadius, conv.rows - 2 * convRadius);
    if (convRect.width <= 2 * convRadius || convRect.height <= 2 * convRadius)
        return stars;

    const auto [globalMean, globalStd] = rectStats(conv, convRect);
    (void)globalMean;
    if (!(globalStd > 0.0))
        return stars;

    constexpr double threshold = 0.1;
    constexpr int topN = 100;
    constexpr int srch = 4;
    const int local = 7;

    for (int y = convRect.y + srch; y < convRect.y + convRect.height - srch; ++y)
    {
        for (int x = convRect.x + srch; x < convRect.x + convRect.width - srch; ++x)
        {
            const float val = conv.at<float>(y, x);
            if (!(val > 0.0f))
                continue;

            bool ismax = true;
            for (int j = -srch; j <= srch && ismax; ++j)
            {
                for (int i = -srch; i <= srch; ++i)
                {
                    if (i == 0 && j == 0)
                        continue;
                    if (conv.at<float>(y + j, x + i) > val)
                    {
                        ismax = false;
                        break;
                    }
                }
            }
            if (!ismax)
                continue;

            cv::Rect localRect(x - local, y - local, 2 * local + 1, 2 * local + 1);
            localRect &= convRect;
            const auto [localMean, localStd] = rectStats(conv, localRect);
            (void)localStd;
            const double h = (val - localMean) / globalStd;
            if (h < threshold)
                continue;

            const int imgx = x * std::max(1, downsample) + std::max(1, downsample) / 2;
            const int imgy = y * std::max(1, downsample) + std::max(1, downsample) / 2;
            stars.push_back(Peak{imgx, imgy, static_cast<float>(h)});
        }
    }

    std::sort(stars.begin(), stars.end(), [](const Peak& a, const Peak& b) { return a.val > b.val; });
    if (static_cast<int>(stars.size()) > topN)
        stars.resize(topN);

    for (size_t i = 0; i < stars.size();)
    {
        bool erased = false;
        for (size_t j = i + 1; j < stars.size(); ++j)
        {
            const int dx = stars[i].x - stars[j].x;
            const int dy = stars[i].y - stars[j].y;
            if (dx * dx + dy * dy < 25)
            {
                stars.erase(stars.begin() + static_cast<long>(j));
                erased = true;
                break;
            }
        }
        if (!erased)
            ++i;
    }

    {
        const int extra = 5;
        const int fullw = searchRegionPx + extra;
        std::vector<bool> erase(stars.size(), false);
        for (size_t i = 0; i < stars.size(); ++i)
        {
            for (size_t j = i + 1; j < stars.size(); ++j)
            {
                const int dx = std::abs(stars[i].x - stars[j].x);
                const int dy = std::abs(stars[i].y - stars[j].y);
                if (dx <= fullw && dy <= fullw)
                {
                    // PHD2 iterates these peaks from dim to bright; this port keeps them
                    // sorted bright to dim, so the close-pair ratio must be inverted here.
                    // Otherwise a dim neighbor will incorrectly eliminate a bright star.
                    if (stars[j].val > 0.0f && (stars[i].val / stars[j].val) < 5.0f)
                    {
                        erase[i] = true;
                        erase[j] = true;
                    }
                }
            }
        }
        std::vector<Peak> filtered;
        filtered.reserve(stars.size());
        for (size_t i = 0; i < stars.size(); ++i)
            if (!erase[i])
                filtered.push_back(stars[i]);
        stars.swap(filtered);
    }

    {
        const int edgeDist = searchRegionPx;
        std::vector<Peak> filtered;
        filtered.reserve(stars.size());
        for (const auto& s : stars)
        {
            if (s.x <= edgeDist || s.x >= image16.cols - edgeDist || s.y <= edgeDist || s.y >= image16.rows - edgeDist)
                continue;
            filtered.push_back(s);
        }
        stars.swap(filtered);
    }

    return stars;
}

bool measureStarCandidate(const cv::Mat& image16, const QPointF& approx, int searchRegionPx, StarCandidate& out)
{
    const int minx = 0;
    const int miny = 0;
    const int maxx = image16.cols - 1;
    const int maxy = image16.rows - 1;

    const int baseX = static_cast<int>(std::llround(approx.x()));
    const int baseY = static_cast<int>(std::llround(approx.y()));
    int startX = std::max(baseX - searchRegionPx, minx);
    int endX = std::min(baseX + searchRegionPx, maxx);
    int startY = std::max(baseY - searchRegionPx, miny);
    int endY = std::min(baseY + searchRegionPx, maxy);
    if (endX <= startX || endY <= startY)
        return false;

    int peakX = 0;
    int peakY = 0;
    unsigned int peakVal = 0;
    unsigned short max3[3] = {0, 0, 0};

    for (int y = startY + 1; y <= endY - 1; ++y)
    {
        for (int x = startX + 1; x <= endX - 1; ++x)
        {
            const unsigned short p = image16.at<uint16_t>(y, x);
            const unsigned int val =
                4U * static_cast<unsigned int>(p) +
                image16.at<uint16_t>(y - 1, x - 1) +
                image16.at<uint16_t>(y - 1, x + 1) +
                image16.at<uint16_t>(y + 1, x - 1) +
                image16.at<uint16_t>(y + 1, x + 1) +
                2U * image16.at<uint16_t>(y - 1, x) +
                2U * image16.at<uint16_t>(y, x - 1) +
                2U * image16.at<uint16_t>(y, x + 1) +
                2U * image16.at<uint16_t>(y + 1, x);

            if (val > peakVal)
            {
                peakVal = val;
                peakX = x;
                peakY = y;
            }

            unsigned short q = p;
            if (q > max3[0]) std::swap(q, max3[0]);
            if (q > max3[1]) std::swap(q, max3[1]);
            if (q > max3[2]) std::swap(q, max3[2]);
        }
    }

    const unsigned short rawPeak = max3[0];
    peakVal /= 16U;
    if (peakVal == 0)
        return false;

    constexpr int A = 7;
    constexpr int B = 12;
    constexpr int A2 = A * A;
    constexpr int B2 = B * B;

    startX = std::max(peakX - B, minx);
    endX = std::min(peakX + B, maxx);
    startY = std::max(peakY - B, miny);
    endY = std::min(peakY + B, maxy);

    unsigned int nbg = 0;
    double meanBg = 0.0;
    double prevMeanBg = 0.0;
    double sigma2Bg = 0.0;
    double sigmaBg = 0.0;
    for (int iter = 0; iter < 9; ++iter)
    {
        double sum = 0.0;
        double a = 0.0;
        double q = 0.0;
        nbg = 0;

        for (int y = startY; y <= endY; ++y)
        {
            const int dy = y - peakY;
            const int dy2 = dy * dy;
            for (int x = startX; x <= endX; ++x)
            {
                const int dx = x - peakX;
                const int r2 = dx * dx + dy2;
                if (r2 <= A2 || r2 > B2)
                    continue;

                const double val = static_cast<double>(image16.at<uint16_t>(y, x));
                if (iter > 0 && (val < meanBg - 2.0 * sigmaBg || val > meanBg + 2.0 * sigmaBg))
                    continue;

                sum += val;
                ++nbg;
                const double k = static_cast<double>(nbg);
                const double a0 = a;
                a += (val - a) / k;
                q += (val - a0) * (val - a);
            }
        }

        if (nbg < 10)
            break;

        prevMeanBg = meanBg;
        meanBg = sum / static_cast<double>(nbg);
        sigma2Bg = q / static_cast<double>(nbg - 1);
        sigmaBg = std::sqrt(std::max(0.0, sigma2Bg));
        if (iter > 0 && std::fabs(meanBg - prevMeanBg) < 0.5)
            break;
    }

    const unsigned short thresh = static_cast<unsigned short>(meanBg + 3.0 * sigmaBg + 0.5);
    startX = std::max(peakX - A, minx);
    endX = std::min(peakX + A, maxx);
    startY = std::max(peakY - A, miny);
    endY = std::min(peakY + A, maxy);

    double cx = 0.0;
    double cy = 0.0;
    double mass = 0.0;
    unsigned int n = 0;
    std::vector<R2M> hfrvec;
    for (int y = startY; y <= endY; ++y)
    {
        const int dy = y - peakY;
        const int dy2 = dy * dy;
        if (dy2 > A2)
            continue;
        for (int x = startX; x <= endX; ++x)
        {
            const int dx = x - peakX;
            if (dx * dx + dy2 > A2)
                continue;

            const unsigned short val = image16.at<uint16_t>(y, x);
            if (val < thresh)
                continue;

            const double d = static_cast<double>(val) - meanBg;
            cx += dx * d;
            cy += dy * d;
            mass += d;
            ++n;
            hfrvec.push_back(R2M{0.0, QPoint(x, y), d});
        }
    }

    constexpr double gain = 0.5;
    constexpr double lowSnr = 3.0;
    const double snr = (n > 0 && nbg > 0)
        ? mass / std::sqrt(std::max(1e-9, mass / gain + sigma2Bg * static_cast<double>(n) * (1.0 + 1.0 / static_cast<double>(nbg))))
        : 0.0;

    double effectiveSnr = snr;
    if (peakVal <= thresh && effectiveSnr >= lowSnr)
        effectiveSnr = lowSnr - 0.1;

    if (mass < 10.0 || effectiveSnr < lowSnr || hfrvec.empty())
        return false;

    const double newX = peakX + cx / mass;
    const double newY = peakY + cy / mass;
    const double hfd = 2.0 * computeHfr(hfrvec, newX, newY, mass);

    bool saturated = false;
    if (image16.depth() == CV_16U)
    {
        const unsigned int maxAdu = 65535U;
        if (rawPeak >= maxAdu)
            saturated = true;
        else
        {
            const unsigned int mx = rawPeak;
            const unsigned int d = static_cast<unsigned int>(max3[0] - max3[2]);
            if (d * 65535U < 32U * std::max(1U, mx))
                saturated = true;
        }
    }

    if (!std::isfinite(hfd) || hfd <= 0.0)
        return false;

    out.x = newX;
    out.y = newY;
    out.snr = effectiveSnr;
    out.hfd = hfd;
    out.peakADU = rawPeak;
    out.edgeDistPx = std::min({out.x, out.y, (double)(image16.cols - 1) - out.x, (double)(image16.rows - 1) - out.y});

    if (saturated)
    {
        // Preserve the star for later pass handling; caller decides whether near-saturated stars are acceptable.
        out.peakADU = std::max(out.peakADU, 65535.0);
    }

    return true;
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


// ============================================================
// Flat-Field Star Detection
// ============================================================

std::vector<StarCandidate> detectFlatFieldPeaks(const cv::Mat& image16,
                                                 const StarSelectionParams& p)
{
    std::vector<StarCandidate> candidates;
    if (image16.empty()) return candidates;

    const int rows = image16.rows;
    const int cols = image16.cols;

    // Step 1: Convert to float
    cv::Mat image32f;
    image16.convertTo(image32f, CV_32F);

    // Step 2: Generate flat-field with boxFilter
    int kernelSize = p.flatKernelSize;
    if (kernelSize < 8) kernelSize = 64;
    cv::Mat flat;
    cv::boxFilter(image32f, flat, CV_32F, cv::Size(kernelSize, kernelSize));

    // Step 3: Normalize flat-field
    cv::Scalar flatMean = cv::mean(flat);
    double flatMeanVal = flatMean[0];
    if (flatMeanVal < 1.0) flatMeanVal = 1.0;
    flat /= flatMeanVal;

    // Step 4: Divide image by flat-field (correction)
    cv::Mat corrected;
    cv::divide(image32f, flat, corrected);

    // Step 5: Clip negative values
    cv::max(corrected, 0.0, corrected);

    // Step 6: Compute background statistics from corrected image
    cv::Mat bgFiltered;
    cv::boxFilter(corrected, bgFiltered, CV_32F, cv::Size(kernelSize, kernelSize));

    // Step 7: 3x3 local peak detection on corrected image
    std::vector<std::tuple<double, int, int>> peaks; // (value, x, y)
    for (int y = 1; y < rows - 1; ++y)
    {
        const float* ptr = corrected.ptr<float>(y);
        const float* ptrUp = corrected.ptr<float>(y - 1);
        const float* ptrDown = corrected.ptr<float>(y + 1);
        for (int x = 1; x < cols - 1; ++x)
        {
            float v = ptr[x];
            if (v >= ptrUp[x-1] && v >= ptrUp[x] && v >= ptrUp[x+1] &&
                v >= ptr[x-1]   && v >= ptr[x+1] &&
                v >= ptrDown[x-1] && v >= ptrDown[x] && v >= ptrDown[x+1])
            {
                peaks.emplace_back(v, x, y);
            }
        }
    }

    // Step 8: Sort by value descending
    std::sort(peaks.begin(), peaks.end(),
              [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });

    // Step 9: Deduplicate (5x5 window, keep strongest)
    std::set<std::pair<int, int>> used;
    const int dedupHalf = 2; // 5x5 window
    for (const auto& peak : peaks)
    {
        int px = std::get<1>(peak);
        int py = std::get<2>(peak);
        bool skip = false;
        for (int dy = -dedupHalf; dy <= dedupHalf && !skip; ++dy)
        {
            for (int dx = -dedupHalf; dx <= dedupHalf && !skip; ++dx)
            {
                if (used.count({px + dx, py + dy}))
                {
                    skip = true;
                }
            }
        }
        if (!skip)
        {
            used.insert({px, py});

            // Compute SNR using local background statistics
            double val = std::get<0>(peak);
            double bgMean = bgFiltered.at<float>(py, px);

            // Compute local std from a 15x15 window
            double sum = 0.0, sumSq = 0.0, count = 0;
            int halfWin = 7;
            for (int dy = -halfWin; dy <= halfWin; ++dy)
            {
                int yy = py + dy;
                if (yy < 1 || yy >= rows - 1) continue;
                const float* rPtr = corrected.ptr<float>(yy);
                for (int dx = -halfWin; dx <= halfWin; ++dx)
                {
                    int xx = px + dx;
                    if (xx < 1 || xx >= cols - 1) continue;
                    // Skip the peak itself
                    if (xx == px && yy == py) continue;
                    double v = rPtr[xx];
                    sum += v;
                    sumSq += v * v;
                    count++;
                }
            }
            if (count < 4) continue;
            double localMean = sum / count;
            double localVar = sumSq / count - localMean * localMean;
            if (localVar < 0.0) localVar = 0.0;
            double localStd = std::sqrt(localVar);
            if (localStd < 1.0) localStd = 1.0;

            double snr = (val - localMean) / localStd;

            // SNR filter
            if (snr < p.minSNR) continue;

            // Compute FWHM for HFD estimation
            // Use 3x3 window around peak
            double totalFlux = 0.0, halfFluxR2 = 0.0;
            int fluxCount = 0;
            int fwhmHalf = 4;
            for (int dy = -fwhmHalf; dy <= fwhmHalf; ++dy)
            {
                int yy = py + dy;
                if (yy < 0 || yy >= rows) continue;
                const float* rPtr = corrected.ptr<float>(yy);
                for (int dx = -fwhmHalf; dx <= fwhmHalf; ++dx)
                {
                    int xx = px + dx;
                    if (xx < 0 || xx >= cols) continue;
                    double v = rPtr[xx];
                    totalFlux += v;
                    double r2 = dx * dx + dy * dy;
                    halfFluxR2 += v * r2;
                    fluxCount++;
                }
            }
            double hfd = (totalFlux > 0) ? std::sqrt(halfFluxR2 / totalFlux) : 0.0;

            // HFD filter
            if (hfd < p.minHFD || hfd > p.maxHFD) continue;

            StarCandidate c;
            c.x = px;
            c.y = py;
            c.snr = snr;
            c.hfd = hfd;
            c.peakADU = val;
            c.edgeDistPx = std::min({(double)px, (double)py,
                                     (double)(cols - 1 - px), (double)(rows - 1 - py)});
            candidates.push_back(c);
        }
    }

    return candidates;
}


std::optional<StarCandidate> GuidingStarDetector::selectGuideStar(const cv::Mat& image16,
                                                                  const StarSelectionParams& p,
                                                                  const QString& fitsPath,
                                                                  std::vector<StarCandidate>* outCandidates,
                                                                  std::vector<StarCandidate>* outRejectedCandidates) const
{
    if (image16.empty() || image16.cols <= 0 || image16.rows <= 0)
        return std::nullopt;

    constexpr const char* kLogPrefix = "[AutoGuideSelect]";
    const auto t0 = std::chrono::steady_clock::now();
    const int resolvedDownsample = resolveAutoSelDownsample(p);

    std::vector<StarCandidate> candidates;
    const auto tDetectStart = std::chrono::steady_clock::now();

    if (p.useFlatField)
    {
        // Flat-field method
        candidates = detectFlatFieldPeaks(image16, p);
    }
    else
    {
        // PHD2Style method (original)
        const auto peaks = detectPHD2StylePeaks(image16, p.searchRegionPx, resolvedDownsample);
        candidates.reserve(peaks.size());
        for (const auto& peak : peaks)
        {
            StarCandidate c;
            if (measureStarCandidate(image16, QPointF(peak.x, peak.y), p.searchRegionPx, c))
                candidates.push_back(c);
        }
    }

    const auto tDetectDone = std::chrono::steady_clock::now();
    const auto tMeasureDone = tDetectDone;

    const auto detectMs = std::chrono::duration_cast<std::chrono::milliseconds>(tDetectDone - t0).count();
    const auto measureMs = std::chrono::duration_cast<std::chrono::milliseconds>(tMeasureDone - tDetectDone).count();
    Logger::Log(std::string(kLogPrefix) +
                    " base_detect engine=" + std::string(p.useFlatField ? "FlatField" : "PHD2Style") + " candidates=" + std::to_string(candidates.size()) +
                    " measured=" + std::to_string(candidates.size()) +
                    " downsample=" + std::to_string(resolvedDownsample) +
                    " pixelScale=" + std::to_string(p.autoSelPixelScaleArcsecPerPixel) +
                    " searchRegionPx=" + std::to_string(p.searchRegionPx) +
                    " detectMs=" + std::to_string(detectMs) +
                    " measureMs=" + std::to_string(measureMs) +
                    (!fitsPath.isEmpty() ? " fits=" + fitsPath.toStdString() : std::string()),
                LogLevel::INFO, DeviceType::GUIDER);

    if (candidates.empty())
    {
        if (outCandidates)
            outCandidates->clear();
        if (outRejectedCandidates)
            outRejectedCandidates->clear();
        Logger::Log(std::string(kLogPrefix) +
                        " fail_at=base_detect reason=no_candidates_from_phd2_style_autofind"
                        " | possible_steps=exposure_gain_or_focus",
                    LogLevel::INFO, DeviceType::GUIDER);
        return std::nullopt;
    }

    const double aduMax = maxADUForMat(image16);
    const double nearSat = aduMax * p.nearSaturationRatio;

    // PHD2-like split:
    // 1) Build a validated candidate list ("foundStars" equivalent) from stars that pass
    //    HFD / edge / centroid checks and meet the minimum SNR.
    // 2) Choose the primary from a 3-pass brightness-first search, where pass1 rejects
    //    near-saturated stars, pass2 allows them, and pass3 falls back to the brightest
    //    viable star even if it is below minSNR.
    std::vector<StarCandidate> viable;
    viable.reserve(candidates.size());
    std::vector<StarCandidate> validated;
    validated.reserve(candidates.size());

    std::vector<StarCandidate> rejectedSNR;
    std::vector<StarCandidate> rejectedHFD;
    std::vector<StarCandidate> rejectedEdge;
    std::vector<StarCandidate> rejectedCentroid;
    std::vector<StarCandidate> rejectedDuplicate;
    std::vector<StarCandidate> rejectedNearSaturation;
    int rejectedSNRCount = 0;
    int rejectedHFDCount = 0;
    int rejectedEdgeCount = 0;
    int rejectedCentroidCount = 0;
    int rejectedDuplicateCount = 0;
    int nearSaturatedCount = 0;

    for (auto c : candidates)
    {
        if (c.hfd < p.minHFD || c.hfd > p.maxHFD)
        {
            ++rejectedHFDCount;
            if (rejectedHFD.size() < 3)
                rejectedHFD.push_back(c);
            continue;
        }

        if (c.edgeDistPx < p.edgeMarginPx)
        {
            ++rejectedEdgeCount;
            if (rejectedEdge.size() < 3)
                rejectedEdge.push_back(c);
            continue;
        }

        bool refined = false;
        const QPointF centroid = refineCandidateCentroid(image16, c, &refined);
        if (!refined)
        {
            ++rejectedCentroidCount;
            if (rejectedCentroid.size() < 3)
                rejectedCentroid.push_back(c);
            continue;
        }

        c.x = centroid.x();
        c.y = centroid.y();
        c.edgeDistPx = std::min({c.x, c.y, (double)(image16.cols - 1) - c.x, (double)(image16.rows - 1) - c.y});
        c.peakADU = localPeakADU(image16, c.x, c.y, 4);

        if (std::any_of(viable.begin(), viable.end(), [&](const StarCandidate& other) { return isCloseDuplicate(c, other); }))
        {
            ++rejectedDuplicateCount;
            if (rejectedDuplicate.size() < 3)
                rejectedDuplicate.push_back(c);
            continue;
        }

        viable.push_back(c);

        const bool nearSaturated =
            (image16.depth() == CV_8U || image16.depth() == CV_16U) && c.peakADU >= nearSat;
        if (nearSaturated)
        {
            ++nearSaturatedCount;
            if (rejectedNearSaturation.size() < 3)
                rejectedNearSaturation.push_back(c);
        }

        if (c.snr >= p.minSNR)
        {
            validated.push_back(c);
        }
        else
        {
            ++rejectedSNRCount;
            if (rejectedSNR.size() < 3)
                rejectedSNR.push_back(c);
        }
    }
    const auto tFilterDone = std::chrono::steady_clock::now();

    auto brightnessOrder = [](const StarCandidate& a, const StarCandidate& b) {
        if (a.peakADU == b.peakADU)
            return a.snr > b.snr;
        return a.peakADU > b.peakADU;
    };

    std::stable_sort(viable.begin(), viable.end(), brightnessOrder);
    std::stable_sort(validated.begin(), validated.end(), brightnessOrder);

    if (outCandidates)
        *outCandidates = validated;
    if (outRejectedCandidates)
    {
        outRejectedCandidates->clear();
        outRejectedCandidates->reserve(rejectedSNR.size() + rejectedHFD.size() + rejectedEdge.size() +
                                       rejectedCentroid.size() + rejectedDuplicate.size());
        outRejectedCandidates->insert(outRejectedCandidates->end(), rejectedSNR.begin(), rejectedSNR.end());
        outRejectedCandidates->insert(outRejectedCandidates->end(), rejectedHFD.begin(), rejectedHFD.end());
        outRejectedCandidates->insert(outRejectedCandidates->end(), rejectedEdge.begin(), rejectedEdge.end());
        outRejectedCandidates->insert(outRejectedCandidates->end(), rejectedCentroid.begin(), rejectedCentroid.end());
        outRejectedCandidates->insert(outRejectedCandidates->end(), rejectedDuplicate.begin(), rejectedDuplicate.end());
    }

    Logger::Log(std::string(kLogPrefix) +
                    " summary base=" + std::to_string(candidates.size()) +
                    " viable=" + std::to_string(viable.size()) +
                    " validated=" + std::to_string(validated.size()) +
                    " reject_snr=" + std::to_string(rejectedSNRCount) +
                    " reject_hfd=" + std::to_string(rejectedHFDCount) +
                    " reject_edge=" + std::to_string(rejectedEdgeCount) +
                    " reject_centroid=" + std::to_string(rejectedCentroidCount) +
                    " reject_duplicate=" + std::to_string(rejectedDuplicateCount) +
                    " near_sat=" + std::to_string(nearSaturatedCount) +
                    " engine=PHD2Style" +
                    " | thresholds{minSNR=" + std::to_string(p.minSNR) +
                    ", searchRegionPx=" + std::to_string(p.searchRegionPx) +
                    ", minHFD=" + std::to_string(p.minHFD) +
                    ", maxHFD=" + std::to_string(p.maxHFD) +
                    ", edgeMarginPx=" + std::to_string(p.edgeMarginPx) +
                    ", nearSatRatio=" + std::to_string(p.nearSaturationRatio) +
                    ", autoDownsample=" + std::to_string(resolvedDownsample) + "}" +
                    " | timings{detectMs=" + std::to_string(detectMs) +
                    ", measureMs=" + std::to_string(measureMs) +
                    ", filterMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(tFilterDone - tMeasureDone).count()) +
                    ", totalMs=" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(tFilterDone - t0).count()) + "}",
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
    for (const auto& c : rejectedCentroid)
    {
        Logger::Log(std::string(kLogPrefix) +
                        " reject_at=centroid " + formatCandidateBrief(c),
                    LogLevel::INFO, DeviceType::GUIDER);
    }
    for (const auto& c : rejectedDuplicate)
    {
        Logger::Log(std::string(kLogPrefix) +
                        " fail_at=duplicate " + formatCandidateBrief(c),
                    LogLevel::INFO, DeviceType::GUIDER);
    }
    for (const auto& c : rejectedNearSaturation)
    {
        Logger::Log(std::string(kLogPrefix) +
                        " note_at=near_saturation " + formatCandidateBrief(c) +
                        " threshold.nearSatADU=" + std::to_string(nearSat),
                    LogLevel::INFO, DeviceType::GUIDER);
    }

    if (viable.empty())
    {
        Logger::Log(std::string(kLogPrefix) +
                        " fail_at=final reason=no candidates after HFD/edge/centroid filtering",
                    LogLevel::INFO, DeviceType::GUIDER);
        return std::nullopt;
    }

    for (int pass = 1; pass <= 3; ++pass)
    {
        for (const auto& c : viable)
        {
            const bool nearSaturated =
                (image16.depth() == CV_8U || image16.depth() == CV_16U) && c.peakADU >= nearSat;

            if (pass == 1)
            {
                if (nearSaturated || c.snr < p.minSNR)
                    continue;
            }
            else if (pass == 2)
            {
                if (c.snr < p.minSNR)
                    continue;
            }

            Logger::Log(std::string(kLogPrefix) +
                            " selected pass=" + std::to_string(pass) + " " + formatCandidateBrief(c),
                        LogLevel::INFO, DeviceType::GUIDER);
            return c;
        }
    }

    Logger::Log(std::string(kLogPrefix) +
                    " fail_at=final reason=no star selected after primary passes despite viable candidates",
                LogLevel::WARNING, DeviceType::GUIDER);
    return std::nullopt;
}

} // namespace guiding


