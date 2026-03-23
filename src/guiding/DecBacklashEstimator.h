#pragma once

#include "GuiderTypes.h"

#include <QPointF>
#include <optional>

namespace guiding {

// DEC 回差测量器（帧驱动）
// 思路：
// - 先用 NORTH 把星点在 DEC 轴方向推到明显偏移（>= targetPx）
// - 再用 SOUTH 小步长回拉，统计“直到星点开始明显回拉”为止累积的 SOUTH 脉冲时长，作为回差(ms)
//
// 注意：这里的“位移”全部通过校准得到的 decUnitVec 投影到 DEC 轴（像素）来判断。
class DecBacklashEstimator
{
public:
    struct Result
    {
        bool hasPulse = false;
        PulseCommand pulse{};

        bool done = false;
        bool failed = false;
        int backlashMs = 0;     // 估计的 DEC 回差（ms）
        QString errorMessage;
        QString infoMessage;
    };

    void reset();
    void start(const CalibrationResult& calib, const GuidingParams& params, const QPointF& lockPosPx);

    bool isActive() const { return m_active; }

    // 输入：本帧星点质心位置（像素，图像坐标）
    Result onFrame(const QPointF& centroidPx);

private:
    enum class Phase { PushNorth, ProbeSouth, Done };

    bool m_active = false;
    Phase m_phase = Phase::PushNorth;

    CalibrationResult m_calib{};
    GuidingParams m_params{};
    QPointF m_lockPosPx{0.0, 0.0};

    bool m_hasFirst = false;
    double m_startDecErrPx = 0.0;
    double m_extremeDecErrPx = 0.0; // NORTH预加载阶段的“极值”（离 start 最远的 decErr）
    int m_pushSign = 1;             // NORTH预加载阶段 decErr-start 的符号（+1 或 -1）

    int m_northTotalMs = 0;
    int m_southTotalMs = 0;
    int m_consecutiveMoveFrames = 0;
    int m_backlashMsEstimated = -1; // 首次检测到运动时记录
    bool m_reportedAutoMax = false; // 是否已输出过“自动放宽超时阈值”的提示
};

} // namespace guiding

