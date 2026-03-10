#pragma once

#include "GuiderTypes.h"

#include <QElapsedTimer>
#include <QMutex>
#include <QPointF>
#include <QString>

#include <opencv2/core/core.hpp>

namespace guiding {

// 生成模拟导星帧（16bit FITS），用于无相机/无赤道仪的导星测试。
// - 每次 generateNextFrame() 会把一颗模拟星点绘制到噪声背景上
// - 星点位置由：基准漂移（右上）+ pending 脉冲响应 位移决定
class SimGuiderFrameSource
{
public:
    struct Params {
        // 默认用前端常见导星分辨率，避免显示端缩放/布局差异
        int width = 1920;
        int height = 1080;

        // 初始星点位置（像素）
        QPointF startPosPx{960.0, 540.0};

        // 右上漂移：x 正、y 负（像素/秒）
        // 漂移速度（像素/秒）：稍微温和一些，更接近真实导星画面
        // 漂移幅度稍小一些（更接近 PHD2 的观感）
        QPointF driftPxPerSec{0.18, -0.12};

        // 脉冲转换：ms/px（越小表示同样脉冲移动越大）
        // 调大 ms/px，降低每次校准/导星脉冲造成的像素位移
        double raMsPerPixel = 140.0;
        double decMsPerPixel = 140.0;

        // 模拟回差（单位：ms）。用于测试“DEC 回差测量/补偿”链路。
        // 语义：当 DEC 脉冲方向发生反转时，先消耗这段 ms，星点不动。
        bool enableDecBacklash = true;
        int decBacklashMs = 400;

        // 星点 PSF
        // 星点做得更“显眼”：更大 PSF + 更高峰值
        double psfSigmaPx = 2.8;
        int psfRadiusPx = 14;
        // 注意：GuidingStarDetector 会过滤 peak>=0.9*65535 的“近饱和星”
        uint16_t starPeak = 52000;

        // 背景噪声（16bit）
        // 降低背景与噪声，确保前端自动拉伸后星点仍清晰可见
        double noiseMean = 200.0;
        double noiseStd = 25.0;

        // 生成多星场：总星数（含导星星点）
        int starCount = 120;
        // 固定随机种子，保证每次运行星场一致（便于测试）
        uint32_t randomSeed = 20251224;

        // 输出路径（默认与前端约定一致）
        QString fitsPath = QStringLiteral("/dev/shm/guiding.fits");
    };

    SimGuiderFrameSource();
    explicit SimGuiderFrameSource(const Params& p);

    Params params() const;
    void setParams(const Params& p);

    // 注入导星脉冲（由 MainWindow::ControlGuide 替代）
    void injectPulse(const PulseCommand& cmd);

    // 生成下一帧并写 FITS；exposureMs 用于计算漂移 dt
    // 成功返回写入的 FITS 路径，否则返回空字符串
    QString generateNextFrame(int exposureMs);

    // 读出当前模拟星点中心（便于调试）
    QPointF currentStarPosPx() const;

private:
    struct FieldStar {
        QPointF basePosPx;
        uint16_t peak = 0;
        double sigmaPx = 1.6;
        int radiusPx = 8;
    };

    static bool writeFits16U(const QString& path, const cv::Mat& image16);
    cv::Mat renderFrame16U(double dtSec);
    void applyPendingShift();
    void initStarFieldIfNeeded();
    void drawStarGaussian(cv::Mat& img16, const QPointF& posPx, uint16_t peak, double sigmaPx, int radiusPx);

private:
    mutable QMutex m_mutex;
    Params m_p{};
    QPointF m_starPosPx{0.0, 0.0};
    QPointF m_pendingShiftPx{0.0, 0.0};
    bool m_started = false;

    // DEC 回差模型：方向反转后需先消耗的 ms
    int m_decBacklashRemainMs = 0;
    std::optional<GuideDir> m_lastDecDir;

    bool m_fieldReady = false;
    std::vector<FieldStar> m_fieldStars;
};

} // namespace guiding


