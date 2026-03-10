#include "CentroidUtils.h"

#include <algorithm>
#include <cmath>

namespace guiding {

static bool findCentroidWeightedImpl(const cv::Mat& image16,
                                     const QPointF& approx,
                                     int halfSize,
                                     QPointF& out,
                                     double kSigma,
                                     bool strict)
{
    if (image16.empty()) return false;

    const int cx = static_cast<int>(std::llround(approx.x()));
    const int cy = static_cast<int>(std::llround(approx.y()));
    const int x0 = std::max(0, cx - halfSize);
    const int y0 = std::max(0, cy - halfSize);
    const int x1 = std::min(image16.cols - 1, cx + halfSize);
    const int y1 = std::min(image16.rows - 1, cy + halfSize);
    if (x1 <= x0 || y1 <= y0) return false;

    cv::Mat roi = image16(cv::Rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1));
    cv::Mat f;
    roi.convertTo(f, CV_32F);
    cv::Scalar mean, stddev;
    cv::meanStdDev(f, mean, stddev);

    const double thr = mean[0] + kSigma * stddev[0];

    double sumW = 0.0;
    double sumX = 0.0;
    double sumY = 0.0;
    float peak = 0.0f;
    int peakX = 0, peakY = 0;

    for (int yy = 0; yy < f.rows; ++yy)
    {
        const float* row = f.ptr<float>(yy);
        for (int xx = 0; xx < f.cols; ++xx)
        {
            const float v = row[xx];
            if (v > peak) { peak = v; peakX = xx; peakY = yy; }
            if (v < thr) continue;
            const double w = std::max(0.0, static_cast<double>(v) - static_cast<double>(mean[0]));
            sumW += w;
            sumX += (x0 + xx) * w;
            sumY += (y0 + yy) * w;
        }
    }

    if (sumW > 0.0)
    {
        out = QPointF(sumX / sumW, sumY / sumW);
        return true;
    }

    if (strict)
        return false;

    out = QPointF(x0 + peakX, y0 + peakY);
    return true;
}

bool FindCentroidWeighted(const cv::Mat& image16,
                          const QPointF& approx,
                          int halfSize,
                          QPointF& out,
                          double kSigma)
{
    return findCentroidWeightedImpl(image16, approx, halfSize, out, kSigma, false /*strict*/);
}

bool FindCentroidWeightedStrict(const cv::Mat& image16,
                                const QPointF& approx,
                                int halfSize,
                                QPointF& out,
                                double kSigma)
{
    return findCentroidWeightedImpl(image16, approx, halfSize, out, kSigma, true /*strict*/);
}

} // namespace guiding

