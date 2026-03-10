#pragma once

#include <QPointF>
#include <opencv2/core/core.hpp>

namespace guiding {

// 在 approx 附近的 ROI 内做加权质心：
// - 先估计 ROI 的 mean/std
// - 以 thr = mean + kSigma*std 做阈值
// - 对超过阈值的像素按 (v-mean) 加权
// - 若没有像素超过阈值，则 fallback 到峰值像素位置
bool FindCentroidWeighted(const cv::Mat& image16,
                          const QPointF& approx,
                          int halfSize,
                          QPointF& out,
                          double kSigma = 2.0);

// Strict variant:
// - Same weighted centroid algorithm
// - Returns false when no pixels pass the threshold (sumW==0), instead of falling back to the peak pixel.
// This is used to reliably detect \"lost star\" conditions.
bool FindCentroidWeightedStrict(const cv::Mat& image16,
                                const QPointF& approx,
                                int halfSize,
                                QPointF& out,
                                double kSigma = 2.0);

} // namespace guiding

