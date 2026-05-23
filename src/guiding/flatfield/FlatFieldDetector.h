#pragma once

#include "../GuidingStarDetector.h"
#include <opencv2/core/core.hpp>
#include <vector>
#include <string>

namespace guiding {
namespace flatfield {

/**
 * 平场法星点检测器（C++实现）
 *
 * 核心思路：
 * 1. 用大核滤波生成自平场（self-flat-field）
 * 2. 原图减去平场，去除大尺度背景（云、光污染梯度等）
 * 3. 在扣除背景后的图像上做峰值检测
 * 4. 加权质心精化 + SNR/HFD筛选
 *
 * 对应 starDetect 项目的 FlatFieldDetector Python 实现，
 * 但使用 OpenCV 替代 SciPy，适配 QUARCS 导星模块的 StarCandidate 格式。
 */

class FlatFieldDetector
{
public:
    struct Params
    {
        // 平场生成参数
        int kernelSize = 64;           // 滤波核大小（像素），对应Python的kernel_size
        std::string method = "uniform"; // "uniform" | "gaussian" | "median"
        double gaussianSigma = 0.0;    // 高斯核sigma（method=gaussian时），0=自动(kernelSize/4)

        // 峰值检测参数
        double snrThreshold = 5.0;     // 最小SNR阈值
        double minHFD = 1.5;           // 最小HFD（像素）
        double maxHFD = 12.0;          // 最大HFD（像素）
        int minSeparation = 5;         // 最小星点间距（像素）
        double edgeMarginPx = 40.0;    // 边缘剔除距离（像素）
        double nearSaturationRatio = 0.9; // 饱和阈值比例

        // 质心参数
        int centroidHalfSize = 5;      // 质心计算半窗口大小
        double kSigma = 3.5;           // 质心阈值倍数
    };

    /**
     * 检测星点
     * @param image16 16位灰度图像 (CV_16UC1)
     * @param p 检测参数
     * @param outCandidates 输出所有候选星点（通过HFD/边缘/质心复核的）
     * @param outRejected 输出被拒绝的候选星点（调试用）
     * @return 最佳导星星点（最高SNR且满足所有条件）
     */
    std::optional<StarCandidate> detect(
        const cv::Mat& image16,
        const Params& p,
        std::vector<StarCandidate>* outCandidates = nullptr,
        std::vector<StarCandidate>* outRejected = nullptr) const;

    /**
     * 仅生成平场图像（调试/可视化用）
     */
    cv::Mat generateFlatField(const cv::Mat& image16, int kernelSize,
                              const std::string& method, double sigma) const;

    /**
     * 生成扣除平场后的图像（调试/可视化用）
     */
    cv::Mat generateFlatSubtracted(const cv::Mat& image16, int kernelSize,
                                   const std::string& method, double sigma) const;

private:
    // 内部步骤拆分
    cv::Mat _generateFlatField(const cv::Mat& imageF, int kernelSize,
                               const std::string& method, double sigma) const;
    cv::Mat _subtractFlat(const cv::Mat& imageF, const cv::Mat& flat) const;
    std::vector<StarCandidate> _findPeaks(const cv::Mat& imageF, const cv::Mat& flatSub,
                                          const Params& p) const;
    double _estimateBackground(const cv::Mat& imageF, int x, int y, int halfSize) const;
    double _computeHFD(const cv::Mat& imageF, double x, double y) const;
    double _localPeakADU(const cv::Mat& imageF, double x, double y, int halfSize) const;
    bool _refineCentroid(const cv::Mat& imageF, double& x, double& y, int halfSize) const;
};

} // namespace flatfield
} // namespace guiding
