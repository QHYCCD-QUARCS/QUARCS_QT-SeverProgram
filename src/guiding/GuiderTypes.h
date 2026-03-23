#pragma once

#include <QString>
#include <QPointF>
#include <QMetaType>

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace guiding {

// 与 MainWindow::ControlGuide 兼容的方向编码
// 0=SOUTH, 1=NORTH, 2=EAST, 3=WEST
enum class GuideDir : int
{
    South = 0,
    North = 1,
    East  = 2,
    West  = 3,
};

enum class State
{
    Idle,
    Looping,
    Selecting,
    Calibrating,
    Guiding,
    Stopped,
    Error,
};

// GuideController 在“本帧不出脉冲”时用于诊断/日志的原因码
enum class NoPulseReason
{
    Other,
    BelowDeadband,
    BlockedByAllowedDirsRa,
    BlockedByAllowedDirsDec,
};

struct PulseCommand
{
    GuideDir dir = GuideDir::West;
    int durationMs = 0; // INDI 脉冲单位：毫秒（用户已确认）
};

struct CalibrationResult
{
    bool valid = false;

    // PHD2 风格字段（用于 UI/日志对齐）
    double cameraAngleDeg = 0.0;
    double orthoErrDeg = 0.0;

    // px/s
    double raRatePxPerSec = 0.0;
    double decRatePxPerSec = 0.0;

    // ms/px
    double raMsPerPixel = 0.0;
    double decMsPerPixel = 0.0;

    // Calibration travel and total pulse duration (for quality gating / diagnostics)
    double raTravelPx = 0.0;
    double decTravelPx = 0.0;
    int raTotalPulseMs = 0;
    int decTotalPulseMs = 0;

    // 校准步数（每轴实际发出的 pulse 次数），用于诊断/对齐 PHD2 日志
    int raStepCount = 0;
    int decStepCount = 0;

    // 校准得到的单位向量（在图像坐标系中）
    // 表示“对应轴的正向导星脉冲”会让星点沿该方向移动。
    // - RA：本实现使用 WEST 脉冲作为正向
    // - DEC：本实现使用 NORTH 脉冲作为正向
    QPointF raUnitVec{1.0, 0.0};
    QPointF decUnitVec{0.0, 1.0};

    // 其他信息
    QPointF lockPosPx{0.0, 0.0};
    qint64 timestampMs = 0;
};

struct GuidingParams
{
    int exposureMs = 1500; // 0.5s--2s，默认 1.5s（室外起步更稳）

    // ===== RA 导星算法：Hysteresis（PHD2 风格）=====
    // 说明：
    // - 将“本帧应纠正量”与“上一帧输出”做滞后融合，能显著降低 seeing 抖动导致的 RA 脉冲抖动
    // - enableRaHysteresis=true 时，RA 脉冲时长将不再直接使用纯比例（absErr*ms/px*aggr），而是使用滞后滤波后的输出
    bool enableRaHysteresis = true;
    double raHysteresis = 0.7;     // 0=不滞后（等价纯比例），1=完全沿用上一帧输出
    double raMinMovePx = 0.0;      // RA 最小移动阈值（px）。0 表示沿用 deadbandPx
    int raMaxStepMsPerFrame = 400; // 限制 RA 脉冲每帧变化幅度（ms），防止突然暴冲

    // ===== 校准质量门槛（室外强烈建议硬门槛）=====
    // orthoErrDeg 太大意味着 RA/DEC 串扰严重；ms/px 异常大通常意味着导星速率太低或脉冲无效
    double calibMaxOrthoErrDeg = 25.0;
    double calibMinAxisMovePx = 12.0;   // 每轴至少移动这么多像素才认为校准可信
    double calibMinMsPerPixel = 1.0;
    double calibMaxMsPerPixel = 500.0;

    // ===== PHD2 风格校准步长/步数计算所需参数 =====
    // 参考：PHD2 CalstepDialog::GetCalibrationStepSize
    // - guideSpeedSidereal: 导星速率（恒星时速的倍数），常见 0.5
    // - pixelScaleArcsecPerPixel 优先用于 imageScale；若为 0，则使用 pixelSizeUm/focalLengthMm/binning 推导
    // - calibDesiredSteps: 期望每轴校准步数（PHD2 默认约 12）
    // - calibDistancePx: 期望校准总位移（像素），用于计算总校准时长，再分摊到每步 pulse
    double guideSpeedSidereal = 0.5;
    int guiderBinning = 1;
    double guiderPixelSizeUm = 0.0;
    double guiderFocalLengthMm = 0.0;
    double calibAssumedDecDeg = 0.0; // 若未知可保持 0（相当于不做 cos(dec) 修正）
    int calibDesiredSteps = 12;
    double calibDistancePx = 25.0;

    // ===== 脉冲/曝光时序（PHD2 风格：pulse -> settle -> next exposure）=====
    // 说明：
    // - INDI timed guide 是异步的（下发即返回），如果我们立刻开始下一次曝光，会把星点拖影“融进”质心，
    //   室外表现为 RA/DEC 都压不住或者出现抖动。
    // - settleMsAfterPulse: 最后一个脉冲结束后额外等待的时间（给机械回弹/延迟留余量）
    // - enableMultiAxisPulses: 当 RA 与 DEC 同时需要纠正时，允许在同一帧周期内按顺序各发一个脉冲
    //   （仍然遵守 allowedRaDirs/allowedDecDirs 门控）
    int settleMsAfterPulse = 150;
    int interPulseDelayMs = 0;      // 两个脉冲之间的额外间隔（ms），一般可为 0
    bool enableMultiAxisPulses = true;

    // 单向导星：允许方向集合门控（用户补充的流程）
    // 经验上 RA 建议默认双向（E/W），DEC 可按需要单向
    std::set<GuideDir> allowedRaDirs{GuideDir::East, GuideDir::West};
    std::set<GuideDir> allowedDecDirs{GuideDir::North};

    // ===== DEC 回差（backlash）测量与补偿 =====
    // 注意：回差单位统一为 “导星脉冲时长 ms”（与 INDI guide pulse 一致）
    bool enableDecBacklashMeasure = true; // 导星开始前测一次 DEC 回差
    double decBacklashNorthTargetPx = 20.0; // NORTH 预加载目标位移（DEC轴投影，px）
    int decBacklashNorthPulseMs = 300;      // NORTH 预加载单步脉冲（ms）
    int decBacklashNorthMaxTotalMs = 8000;  // NORTH 预加载最大累计时长（ms）
    int decBacklashProbeStepMs = 100;       // SOUTH 探测单步脉冲（ms）
    int decBacklashProbeMaxTotalMs = 6000;  // SOUTH 探测最大累计时长（ms）
    double decBacklashDetectMovePx = 0.4;   // 判定“开始啮合回拉”的位移阈值（px）
    int decBacklashDetectConsecutiveFrames = 2; // 连续多少帧满足阈值才算开始运动

    bool enableDecBacklashCompensation = true; // DEC 方向反转时应用回差补偿
    bool enableDecBacklashAdaptive = true;     // 根据误差改善程度动态调整回差(ms)
    int decBacklashAdaptiveWindowFrames = 3;   // 反向后观察多少帧
    double decBacklashAdaptiveMinImprovePx = 0.15; // 最小改善阈值（px），低于认为补偿不足
    int decBacklashAdaptiveStepMs = 20;        // 自适应调整步长（ms）
    int decBacklashAdaptiveMaxMs = 2000;       // 自适应回差上限（ms）

    // ===== 自动判定 RA/DEC 漂移并推荐/自动设置单向方向 =====
    // 若启用：导星进入 Guiding 后会先运行一段短暂的"漂移检测窗口"，期间临时允许双向（以便检测真实漂移），
    // 通过线性拟合 raErrPx(t) / decErrPx(t) 得到漂移方向与速度，并推荐/自动设置 allowedRaDirs/allowedDecDirs 为反向以抵消漂移。
    // 检测完成后，自动锁定为单向（不再允许双向）。
    bool autoRaGuideDir = false;  // 默认关闭：RA 通常建议双向；需要时再开启 AUTO
    bool autoDecGuideDir = true; // 默认启用 AUTO 模式（见下方“DEC 单向策略”）

    // ===== DEC 单向策略（按用户需求的两种情况）=====
    // (1) 对极轴很准（DEC移动幅度小）：导星开始先允许 DEC 双向，收集足够数据后再锁定为单向
    // (2) 对极轴不准（DEC移动幅度大）：根据前 N 帧快速判定单向方向并锁定
    //
    // 说明：
    // - “DEC移动幅度大/小”的判定使用导星误差 decErrPx 的 RMS（像素）
    // - 锁定的“单向方向”指允许的纠正方向（North-only 或 South-only）
    int decUniCollectFrames = 30;       // case(1) 收集足够数据的阈值（帧数）
    int decUniInitialFrames = 5;        // case(2) 快速判定使用的前N帧
    double decUniLargeMoveRmsPx = 2.0;  // RMS >= 该值 → 认为“DEC移动幅度大”，走 case(2)
    double decUniMinAbsMeanPx = 0.05;   // 均值绝对值太小则认为方向不可靠（继续收集）
    int driftDetectDurationMs = 8000;        // 漂移检测窗口长度（ms）
    int driftDetectMinSamples = 6;           // 至少采样点数
    double driftDetectMinAbsSlopePxPerSec = 0.02; // 最小|斜率|阈值（px/s），低于则认为“无明显长期漂移”

    // 像素尺度（角秒/像素），用于把 px/s 换算为 arcsec/s；若为 0 则仅输出 px/s
    double pixelScaleArcsecPerPixel = 0.0;

    // ===== 误差滤波（降低 seeing 抖动导致的脉冲抖动）=====
    // EMA: ema = alpha*err + (1-alpha)*ema
    bool enableErrorEma = true;
    double errorEmaAlpha = 0.35; // 0=关闭；越大越“跟随快”，越小越“更平滑”

    double deadbandPx = 0.40;      // 误差小于此不出脉冲（室外起步更稳）
    // 分轴比例系数（P 控制）。保留旧字段 aggression 但不再作为主配置使用。
    double raAggression = 0.8;
    double decAggression = 0.8;
    double aggression = 1.0; // deprecated
    int minPulseMs = 20;
    int maxPulseMs = 1200;

    // ===== 丢星恢复 =====
    int maxConsecutiveCentroidFails = 10; // 连续多少帧质心失败就判定丢星并回退到 Selecting

    // ===== 脉冲有效性检测（室外排查“脉冲不生效/导星速率太低/驱动拒绝”）=====
    bool enablePulseEffectCheck = true;
    int pulseEffectWindowFrames = 3;      // 发脉冲后观察多少帧的误差改善
    double pulseEffectMinImprovePx = 0.08;// 改善阈值（px），低于认为“几乎无效”
    double pulseEffectMinStartAbsErrPx = 1.0; // 小于此误差不检测（避免 seeing 噪声误判）
    int pulseEffectMaxFailures = 5;       // 连续失败次数超过则进入 Error

    // ===== RMS 统计（用于室外快速判断是否稳定）=====
    int rmsWindowFrames = 60;
    int rmsEmitEveryFrames = 5;

    // ===== 两段式应急兜底（目前仅用于离线测试/预留配置；核心逻辑后续实现）=====
    // 说明：这些字段在 guiding_offline_test.cpp 中会被设置；为保证工程可编译，这里先补齐配置字段。
    bool enableEmergency = false;
    double emergencyStage1AbsErrPx = 2.0;
    double emergencyStage2AbsErrPx = 4.0;
    double emergencySafeBandPx = 1.5;
    int emergencyGrowFrames = 3;
    double emergencyGrowEpsPx = 0.1;
    double stage1MinPulseFactor = 2.0;
    int stage1MaxMs = 200;
    int maxPulseStepPerFrameMs = 400;
};

} // namespace guiding

// 允许 queued connection 传递 guiding::State
Q_DECLARE_METATYPE(guiding::State)

// queued connection 还需要注册这些类型（用于 requestPulse / calibrationResultChanged）
Q_DECLARE_METATYPE(guiding::GuideDir)
Q_DECLARE_METATYPE(guiding::PulseCommand)
Q_DECLARE_METATYPE(guiding::CalibrationResult)


