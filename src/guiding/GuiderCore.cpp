#include "GuiderCore.h"

#include "../tools.h"

#include "CentroidUtils.h"

#include <QDateTime>
#include <limits>

namespace {
// 最小二乘线性拟合 y = a*t + b，返回 a（斜率）。若不可拟合返回 NaN。
// 线性回归拟合斜率（支持 RA 或 DEC）
static double fitSlopeLeastSquares(const std::vector<GuiderCore::DriftSample>& samples, bool useRa)
{
    if (samples.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    double sumT = 0.0, sumY = 0.0, sumTT = 0.0, sumTY = 0.0;
    for (const auto& s : samples)
    {
        const double y = useRa ? s.raErrPx : s.decErrPx;
        sumT += s.tSec;
        sumY += y;
        sumTT += s.tSec * s.tSec;
        sumTY += s.tSec * y;
    }
    const double n = static_cast<double>(samples.size());
    const double denom = (n * sumTT - sumT * sumT);
    if (std::abs(denom) < 1e-12)
        return std::numeric_limits<double>::quiet_NaN();
    return (n * sumTY - sumT * sumY) / denom;
}

static inline QString dirToStr(guiding::GuideDir d)
{
    return (d == guiding::GuideDir::North) ? "NORTH" :
           (d == guiding::GuideDir::South) ? "SOUTH" :
           (d == guiding::GuideDir::East)  ? "EAST"  :
           (d == guiding::GuideDir::West)  ? "WEST"  : "UNK";
}

static inline int roundUpTo50ms(double ms)
{
    if (!std::isfinite(ms) || ms <= 0.0)
        return 0;
    const int msi = (int)std::ceil(ms / 50.0) * 50;
    return std::max(50, msi);
}

// PHD2 CalstepDialog::GetCalibrationStepSize 的核心公式（简化版）
// - imageScale: arcsec/px
// - totalDurationSec = distancePx * imageScale / (15 * guideSpeed)
// - pulseMs = totalDurationSec/desiredSteps * 1000
// - pulseMs = min(totalDurationSec/MIN_STEPS*1000, pulseMs / cos(dec))
static double computeImageScaleArcsecPerPixel(const guiding::GuidingParams& p)
{
    if (p.pixelScaleArcsecPerPixel > 0.0)
        return p.pixelScaleArcsecPerPixel;
    if (p.guiderPixelSizeUm > 0.0 && p.guiderFocalLengthMm > 0.0)
    {
        const double bin = std::max(1, p.guiderBinning);
        // 206.265 * (pixelSizeUm / focalLengthMm)  [arcsec/pixel]，并乘以 binning
        return 206.265 * (p.guiderPixelSizeUm * bin) / p.guiderFocalLengthMm;
    }
    return 0.0;
}

static int computePHD2StyleCalibPulseMs(const guiding::GuidingParams& p)
{
    constexpr double MIN_STEPS = 6.0; // PHD2: #define MIN_STEPS 6.0

    const double guideSpeed = p.guideSpeedSidereal;
    const int desiredSteps = std::max(1, p.calibDesiredSteps);
    const double distancePx = std::max(1.0, p.calibDistancePx);
    const double imageScale = computeImageScaleArcsecPerPixel(p);
    if (guideSpeed <= 0.0 || imageScale <= 0.0)
        return 0;

    const double totalDurationSec = (distancePx * imageScale) / (15.0 * guideSpeed);
    double pulseMs = (totalDurationSec / (double)desiredSteps) * 1000.0;
    const double maxPulseMs = (totalDurationSec / MIN_STEPS) * 1000.0;

    // cos(dec) 修正（未知时 dec=0，相当于不修正）
    const double decDeg = p.calibAssumedDecDeg;
    const double c = std::cos(decDeg * M_PI / 180.0);
    if (std::isfinite(c) && c > 1e-6)
        pulseMs = pulseMs / c;

    pulseMs = std::min(maxPulseMs, pulseMs);
    return roundUpTo50ms(pulseMs);
}
} // namespace

double GuiderCore::RmsWindow::raRms() const
{
    if (ra2.empty()) return 0.0;
    double s = 0.0;
    for (double v : ra2) s += v;
    return std::sqrt(std::max(0.0, s / (double)ra2.size()));
}

double GuiderCore::RmsWindow::decRms() const
{
    if (dec2.empty()) return 0.0;
    double s = 0.0;
    for (double v : dec2) s += v;
    return std::sqrt(std::max(0.0, s / (double)dec2.size()));
}

double GuiderCore::RmsWindow::totalRms() const
{
    if (ra2.empty() || dec2.empty()) return 0.0;
    // sqrt( mean(ra^2 + dec^2) )
    const size_t n = std::min(ra2.size(), dec2.size());
    double s = 0.0;
    for (size_t i = 0; i < n; ++i)
        s += ra2[i] + dec2[i];
    return std::sqrt(std::max(0.0, s / (double)n));
}

GuiderCore::GuiderCore(QObject* parent) : QObject(parent)
{
    m_rms.reset(m_params.rmsWindowFrames);
}

guiding::GuidingParams GuiderCore::sanitizeParams(const guiding::GuidingParams& in) const
{
    guiding::GuidingParams out = in;

    // ===== 选项A：RA 永远双向 =====
    // - 禁止任何 AUTO/单向配置把 RA 锁死
    // - 允许前端仍然发送 GuiderRaGuideDir，但这里只会被强制为双向
    out.autoRaGuideDir = false;
    out.allowedRaDirs.clear();
    out.allowedRaDirs.insert(guiding::GuideDir::East);
    out.allowedRaDirs.insert(guiding::GuideDir::West);

    // 基本防御：数值范围
    if (out.exposureMs < 50) out.exposureMs = 50;
    if (out.minPulseMs < 0) out.minPulseMs = 0;
    if (out.maxPulseMs < out.minPulseMs) out.maxPulseMs = out.minPulseMs;

    // EMA 参数
    if (out.errorEmaAlpha < 0.0) out.errorEmaAlpha = 0.0;
    if (out.errorEmaAlpha > 1.0) out.errorEmaAlpha = 1.0;

    // DEC 单向策略参数
    if (out.decUniCollectFrames < 1) out.decUniCollectFrames = 1;
    if (out.decUniInitialFrames < 1) out.decUniInitialFrames = 1;
    if (out.decUniLargeMoveRmsPx < 0.0) out.decUniLargeMoveRmsPx = 0.0;
    if (out.decUniMinAbsMeanPx < 0.0) out.decUniMinAbsMeanPx = 0.0;

    // RA hysteresis 参数
    if (out.raHysteresis < 0.0) out.raHysteresis = 0.0;
    if (out.raHysteresis > 1.0) out.raHysteresis = 1.0;
    if (out.raMinMovePx < 0.0) out.raMinMovePx = 0.0;
    if (out.raMaxStepMsPerFrame < 0) out.raMaxStepMsPerFrame = 0;

    // calibration quality thresholds
    if (out.calibMaxOrthoErrDeg < 0.0) out.calibMaxOrthoErrDeg = 0.0;
    if (out.calibMinAxisMovePx < 0.0) out.calibMinAxisMovePx = 0.0;
    if (out.calibMinMsPerPixel < 0.0) out.calibMinMsPerPixel = 0.0;
    if (out.calibMaxMsPerPixel < out.calibMinMsPerPixel) out.calibMaxMsPerPixel = out.calibMinMsPerPixel;

    // lost-star / pulse-effect / RMS params
    if (out.maxConsecutiveCentroidFails < 1) out.maxConsecutiveCentroidFails = 1;
    if (out.pulseEffectWindowFrames < 1) out.pulseEffectWindowFrames = 1;
    if (out.pulseEffectMinImprovePx < 0.0) out.pulseEffectMinImprovePx = 0.0;
    if (out.pulseEffectMinStartAbsErrPx < 0.0) out.pulseEffectMinStartAbsErrPx = 0.0;
    if (out.pulseEffectMaxFailures < 1) out.pulseEffectMaxFailures = 1;
    if (out.rmsWindowFrames < 5) out.rmsWindowFrames = 5;
    if (out.rmsEmitEveryFrames < 1) out.rmsEmitEveryFrames = 1;

    // pulse/settle 时序参数
    if (out.settleMsAfterPulse < 0) out.settleMsAfterPulse = 0;
    if (out.interPulseDelayMs < 0) out.interPulseDelayMs = 0;

    return out;
}

void GuiderCore::setParams(const guiding::GuidingParams& p)
{
    const auto sanitized = sanitizeParams(p);
    m_params = sanitized;
    // keep RMS window in sync with params
    m_rms.reset(m_params.rmsWindowFrames);
    emit paramsChanged();
}

void GuiderCore::setState(guiding::State s)
{
    if (m_state == s) return;
    m_state = s;
    emit stateChanged(m_state);
}

void GuiderCore::scheduleNextExposure(int delayMs)
{
    if (!m_loopActive)
        return;

    const quint64 token = m_schedSeq;
    const int ms = std::max(0, delayMs);

    if (ms <= 0)
    {
        emit requestExposure(m_params.exposureMs);
        return;
    }

    QTimer::singleShot(ms, this, [this, token]() {
        if (!m_loopActive)
            return;
        if (token != m_schedSeq)
            return; // cancelled by newer frame/stop
        emit requestExposure(m_params.exposureMs);
    });
}

void GuiderCore::startLoop()
{
    if (m_state == guiding::State::Calibrating || m_state == guiding::State::Guiding)
    {
        emit infoMessage(QStringLiteral("导星正在运行/校准中，忽略 startLoop。"));
        return;
    }

    m_loopActive = true;
    m_loopTimer.restart();
    setState(guiding::State::Looping);
    emit infoMessage(QStringLiteral("导星循环曝光启动。"));
    emit requestExposure(m_params.exposureMs);
}

void GuiderCore::stopLoop()
{
    m_loopActive = false;
    // cancel any pending pulse/exposure timers
    m_schedSeq++;
    if (m_state == guiding::State::Looping || m_state == guiding::State::Selecting)
        setState(guiding::State::Stopped);
    emit infoMessage(QStringLiteral("导星循环曝光停止。"));
}

void GuiderCore::startGuiding()
{
    // 后续：Selecting → Calibrating → Guiding
    if (m_state == guiding::State::Guiding)
        return;
    if (!m_loopActive)
        startLoop();

    // 若用户在 Looping 下已手动选星，则保留锁点并直接进入校准（跳过自动选星）
    const bool preserveManualLock = m_hasLock && !m_lockPosPx.isNull() && (m_state == guiding::State::Looping);

    // 每次点击“导星”都从当前画面重新开始：清理上次残留（注意：可按规则保留手动锁点）
    if (!preserveManualLock)
    {
        m_hasLock = false;
        m_lockPosPx = QPointF(0.0, 0.0);
    }
    m_lastGuideCentroid = QPointF(0.0, 0.0);
    m_phd2Calib.reset();
    m_calibResult = guiding::CalibrationResult{};
    m_decBacklash.reset();
    m_decBacklashMeasureActive = false;
    m_decBacklashMsBase = 0;
    m_decBacklashMsRuntime = 0;
    m_lastDecPulseDir.reset();
    m_decBacklashAdaptActive = false;
    m_decBacklashAdaptFramesLeft = 0;
    m_errEmaInit = false;
    m_raErrEma = 0.0;
    m_decErrEma = 0.0;
    m_decDriftDetectActive = false;
    m_decDriftSamples.clear();
    m_decUniPolicyActive = false;
    m_decUniPolicyDecided = false;
    m_decUniLargeMove = false;
    m_decUniFrames = 0;
    m_decUniSum = 0.0;
    m_decUniSumSq = 0.0;
    m_guidingFrameCount = 0;
    m_centroidFailCount = 0;
    m_guidingDiagTimer.invalidate();

    // emergency state
    m_emergencyStage2Active = false;
    m_emergencyHasLastAbs = false;
    m_emergencyLastAbsDecPx = 0.0;
    m_emergencyGrowHit = 0;
    m_emergencySavedAllowed = false;
    m_emergencySavedAllowedDecDirs.clear();

    // RA hysteresis state
    m_raHysInit = false;
    m_raHysPrevSignedMs = 0.0;
    m_lastRaPulseMs = 0;
    m_lastDecPulseMs = 0;

    setState(guiding::State::Selecting);
    if (preserveManualLock)
    {
        // 关键：Selecting 阶段只会在 !m_hasLock 时自动选星。
        // 若保留了手动锁点，则直接进入校准，避免卡在 Selecting。
        m_lastGuideCentroid = m_lockPosPx;
        emit lockPositionChanged(m_lockPosPx);
        emit lockStarSelected(m_lockPosPx.x(), m_lockPosPx.y(), 0.0, 0.0);
        startGuidingFromLock(true);
        return; // 校准由后续帧驱动
    }
    QString reuseReason;
    if (m_forceCalibrateNextStart)
    {
        emit infoMessage(QStringLiteral("开始导星：进入自动选星阶段。本次为强制重新校准。"));
    }
    else if (canReuseStartupSnapshot(&reuseReason))
    {
        emit infoMessage(QStringLiteral("开始导星：进入自动选星阶段。选星成功后将复用上次校准与DEC回差（%1）并直接开始导星。")
                         .arg(reuseReason));
    }
    else
    {
        emit infoMessage(QStringLiteral("开始导星：进入自动选星阶段。"));
    }
}

void GuiderCore::startGuidingForceCalibrate()
{
    clearCachedCalibration();
    m_forceCalibrateNextStart = true;
    startGuiding();
}

void GuiderCore::clearCachedCalibration()
{
    m_hasLastCalibration = false;
    m_lastCalibration = guiding::CalibrationResult{};
    m_lastCalibrationCtx = CalibrationContext{};
    m_hasLastBacklash = false;
    m_lastBacklashMsBase = 0;
    m_lastBacklashMsRuntime = 0;
}

void GuiderCore::stopGuiding()
{
    m_loopActive = false;
    if (m_state == guiding::State::Guiding || m_state == guiding::State::Calibrating || m_state == guiding::State::Selecting)
        setState(guiding::State::Stopped);
    // cancel any pending pulse/exposure timers
    m_schedSeq++;
    // 停止导星时清理锁星/质心/活动校准状态机；但保留最近一次成功校准快照用于“换星复用”
    m_hasLock = false;
    m_lockPosPx = QPointF(0.0, 0.0);
    m_lastGuideCentroid = QPointF(0.0, 0.0);
    m_phd2Calib.reset();
    m_calibResult = guiding::CalibrationResult{}; // 当前会话清空；最近一次成功校准在 m_lastCalibration 中保留
    m_decBacklash.reset();
    m_decBacklashMeasureActive = false;
    m_decBacklashMsBase = 0;
    m_decBacklashMsRuntime = 0;
    m_lastDecPulseDir.reset();
    m_decBacklashAdaptActive = false;
    m_decBacklashAdaptFramesLeft = 0;
    m_errEmaInit = false;
    m_raErrEma = 0.0;
    m_decErrEma = 0.0;
    m_decDriftDetectActive = false;
    m_decDriftSamples.clear();
    m_decUniPolicyActive = false;
    m_decUniPolicyDecided = false;
    m_decUniLargeMove = false;
    m_decUniFrames = 0;
    m_decUniSum = 0.0;
    m_decUniSumSq = 0.0;
    m_guidingFrameCount = 0;
    m_centroidFailCount = 0;
    m_guidingDiagTimer.invalidate();

    // emergency state
    m_emergencyStage2Active = false;
    m_emergencyHasLastAbs = false;
    m_emergencyLastAbsDecPx = 0.0;
    m_emergencyGrowHit = 0;
    m_emergencySavedAllowed = false;
    m_emergencySavedAllowedDecDirs.clear();

    // RA hysteresis state
    m_raHysInit = false;
    m_raHysPrevSignedMs = 0.0;
    m_lastRaPulseMs = 0;
    m_lastDecPulseMs = 0;
    emit infoMessage(QStringLiteral("停止导星。"));
}

void GuiderCore::setManualLock(double xPx, double yPx)
{
    // 规则：只允许在 Looping 状态下手动选星
    if (m_state != guiding::State::Looping)
        return;

    m_hasLock = true;
    m_lockPosPx = QPointF(xPx, yPx);
    m_lastGuideCentroid = m_lockPosPx;
    emit lockPositionChanged(m_lockPosPx);
    // 点击即用户确认：不做 SNR/HFD 评估
    emit lockStarSelected(xPx, yPx, 0.0, 0.0);
    emit infoMessage(QStringLiteral("手动选星：x=%1 y=%2（请点击开始导星继续）")
                         .arg(xPx, 0, 'f', 2)
                         .arg(yPx, 0, 'f', 2));
}

void GuiderCore::clearManualLock()
{
    // 仅清内部锁点；UI 清理由 MainWindow（PHD2StarBoxView:false 等）控制
    m_hasLock = false;
    m_lockPosPx = QPointF(0.0, 0.0);
    m_lastGuideCentroid = QPointF(0.0, 0.0);
}

bool GuiderCore::canReuseLastCalibration(QString* reason) const
{
    if (!m_hasLastCalibration || !m_lastCalibration.valid)
    {
        if (reason) *reason = QStringLiteral("无历史校准");
        return false;
    }

    // 过旧门槛：默认 2 小时
    constexpr qint64 kMaxAgeMs = 2LL * 60LL * 60LL * 1000LL;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastCalibration.timestampMs <= 0 || now - m_lastCalibration.timestampMs > kMaxAgeMs)
    {
        if (reason) *reason = QStringLiteral("校准过旧");
        return false;
    }

    // binning 变化
    const int curBin = std::max(1, m_params.guiderBinning);
    if (m_lastCalibrationCtx.guiderBinning != curBin)
    {
        if (reason) *reason = QStringLiteral("binning变化(%1->%2)").arg(m_lastCalibrationCtx.guiderBinning).arg(curBin);
        return false;
    }

    // guide speed 变化（相对差异 >20%）
    const double oldSpeed = m_lastCalibrationCtx.guideSpeedSidereal;
    const double curSpeed = m_params.guideSpeedSidereal;
    if (oldSpeed > 0.0 && curSpeed > 0.0)
    {
        const double rel = std::abs(curSpeed - oldSpeed) / oldSpeed;
        if (rel > 0.20)
        {
            if (reason) *reason = QStringLiteral("导星速率变化(%1->%2)").arg(oldSpeed, 0, 'f', 3).arg(curSpeed, 0, 'f', 3);
            return false;
        }
    }

    // image scale 变化（若可获取；相对差异 >20%）
    const double curScale = computeImageScaleArcsecPerPixel(m_params);
    const double oldScale = m_lastCalibrationCtx.imageScaleArcsecPerPixel;
    if (oldScale > 0.0 && curScale > 0.0)
    {
        const double rel = std::abs(curScale - oldScale) / oldScale;
        if (rel > 0.20)
        {
            if (reason) *reason = QStringLiteral("像素尺度变化(%1->%2\"/px)").arg(oldScale, 0, 'f', 4).arg(curScale, 0, 'f', 4);
            return false;
        }
    }

    if (reason) *reason = QStringLiteral("有效且未过期");
    return true;
}

bool GuiderCore::canReuseStartupSnapshot(QString* reason) const
{
    QString calibReason;
    if (!canReuseLastCalibration(&calibReason))
    {
        if (reason) *reason = calibReason;
        return false;
    }

    if (m_params.enableDecBacklashMeasure && !m_hasLastBacklash)
    {
        if (reason) *reason = QStringLiteral("无历史DEC回差");
        return false;
    }

    if (reason) *reason = calibReason;
    return true;
}

void GuiderCore::startGuidingFromLock(bool isManualLock)
{
    if (!m_hasLock)
    {
        emit errorOccurred(QStringLiteral("StartGuidingFromLockFailed:NoLock"));
        setState(guiding::State::Error);
        return;
    }

    const bool forceRecalibrate = m_forceCalibrateNextStart;
    if (forceRecalibrate)
    {
        m_forceCalibrateNextStart = false;
        emit infoMessage(QStringLiteral("本次为强制重新校准。"));
        emit infoMessage(QStringLiteral("历史快照已清除，将重新校准。"));
    }
    else
    {
        QString reason;
        if (canReuseStartupSnapshot(&reason))
        {
            m_calibResult = m_lastCalibration;
            m_calibResult.lockPosPx = m_lockPosPx;
            m_calibResult.timestampMs = QDateTime::currentMSecsSinceEpoch();
            emit calibrationResultChanged(m_calibResult);

            if (m_hasLastBacklash)
            {
                m_decBacklashMsBase = m_lastBacklashMsBase;
                m_decBacklashMsRuntime = m_lastBacklashMsRuntime;
                emit infoMessage(QStringLiteral("已复用上次校准与DEC回差（%1），直接开始导星。").arg(reason));
            }
            else
            {
                emit infoMessage(QStringLiteral("已复用上次校准（%1），直接开始导星。").arg(reason));
            }

            enterGuidingState();
            return;
        }

        emit infoMessage(QStringLiteral("历史快照不可复用（%1），将重新校准。").arg(reason));
    }

    emit infoMessage(isManualLock
                         ? QStringLiteral("开始导星：使用手动选星点，进入校准阶段。")
                         : QStringLiteral("开始导星：自动选星完成，历史快照不可复用，进入校准阶段。"));
    beginCalibrationFromLock();
}

void GuiderCore::enterGuidingState()
{
    if (m_params.autoDecGuideDir)
    {
        m_decUniPolicyActive = true;
        m_decUniPolicyDecided = false;
        m_decUniLargeMove = false;
        m_decUniFrames = 0;
        m_decUniSum = 0.0;
        m_decUniSumSq = 0.0;
        m_decDriftSamples.clear();
        m_decDriftTimer.restart();

        m_quickDirectionDetectActive = true;
        emit directionDetectionStateChanged(true);

        auto newParams = m_params;
        newParams.allowedDecDirs.clear();
        newParams.allowedDecDirs.insert(guiding::GuideDir::North);
        newParams.allowedDecDirs.insert(guiding::GuideDir::South);
        m_params = newParams;
        emit paramsChanged();

        emit infoMessage(QStringLiteral("DEC 单向方向判定中：先双向收集数据，小漂移等 %1 帧后锁定；大漂移前 %2 帧快速锁定。")
                         .arg(m_params.decUniCollectFrames)
                         .arg(m_params.decUniInitialFrames));
    }
    else
    {
        emit paramsChanged();
    }

    setState(guiding::State::Guiding);
    m_lastGuideCentroid = m_lockPosPx;
    m_guidingFrameCount = 0;
    m_centroidFailCount = 0;
    m_guidingDiagTimer.restart();
    m_raGatedCount = 0;
    m_decGatedCount = 0;
    m_reselectAfterLostStar = false;

    m_pulseEffActive = false;
    m_pulseEffFramesLeft = 0;
    m_pulseEffFailStreak = 0;
    m_rms.reset(m_params.rmsWindowFrames);
    m_rmsEmitCounter = 0;
    m_decDriftDetectActive = false;
}

void GuiderCore::beginCalibrationFromLock()
{
    // 前置条件：已锁星
    if (!m_hasLock)
    {
        emit errorOccurred(QStringLiteral("BeginCalibrationFailed:NoLock"));
        setState(guiding::State::Error);
        return;
    }

    setState(guiding::State::Calibrating);

    // PHD2 风格：按像素尺度/导星速率/期望步数计算校准步长（ms）
    const double targetMovePx = std::max(8.0, m_params.calibDistancePx);
    const int phd2PulseMs = computePHD2StyleCalibPulseMs(m_params);
    const int pulseMs = (phd2PulseMs > 0) ? phd2PulseMs : 500;

    // PHD2 校准状态机（scope.cpp）：GoWest -> GoEast -> ClearBacklash -> GoNorth -> GoSouth
    m_phd2Calib.start(m_lockPosPx, targetMovePx, pulseMs);

    const double imgScale = computeImageScaleArcsecPerPixel(m_params);
    if (imgScale > 0.0 && m_params.guideSpeedSidereal > 0.0)
    {
        emit infoMessage(QStringLiteral("进入校准阶段：distance=%1px steps=%2 guideSpeed=%3 imageScale=%4\"/px -> pulse=%5ms")
                             .arg(targetMovePx, 0, 'f', 1)
                             .arg(m_params.calibDesiredSteps)
                             .arg(m_params.guideSpeedSidereal, 0, 'f', 3)
                             .arg(imgScale, 0, 'f', 4)
                             .arg(pulseMs));
    }
    else
    {
        // 按你的要求：读不到焦距/像元就降级，不阻断流程；UI 明确提示“只显示 px”
        emit infoMessage(QStringLiteral("当前未获取导星焦距，误差以像素显示"));
        emit infoMessage(QStringLiteral("若提供焦距，可显示 arcsec RMS"));
        emit infoMessage(QStringLiteral("进入校准阶段：distance=%1px steps=%2 -> pulse=%3ms（未提供 imageScale/guideSpeed，使用默认/回退值）")
                             .arg(targetMovePx, 0, 'f', 1)
                             .arg(m_params.calibDesiredSteps)
                             .arg(pulseMs));
    }
}

void GuiderCore::startGuidingWithManualLock(double xPx, double yPx)
{
    // 统一入口：确保从 Selecting 开始并清理上次残留
    startGuiding();

    // 设置锁星点（不做 SNR/HFD 评估：点击即用户确认）
    m_hasLock = true;
    m_lockPosPx = QPointF(xPx, yPx);
    m_lastGuideCentroid = m_lockPosPx;
    emit lockPositionChanged(m_lockPosPx);
    emit lockStarSelected(xPx, yPx, 0.0, 0.0);
    emit infoMessage(QStringLiteral("手动选星：x=%1 y=%2").arg(xPx, 0, 'f', 2).arg(yPx, 0, 'f', 2));
    startGuidingFromLock(true);
}

void GuiderCore::onNewFrame(const QString& fitsPath)
{
    if (!m_loopActive)
        return;

    // 先按你的要求：每帧都固定命名并落盘（MainWindow 实现）
    emit requestPersistGuidingFits(fitsPath);

    // New frame cancels any pending pulse/exposure scheduling from the previous frame
    m_schedSeq++;
    int nextExposureDelayMs = 0;

    if (m_state == guiding::State::Selecting && !m_hasLock)
    {
        cv::Mat img16;
        if (Tools::readFits(fitsPath.toUtf8().constData(), img16) == 0 && !img16.empty())
        {
            guiding::StarSelectionParams sp;
            auto best = m_detector.selectGuideStar(img16, sp, nullptr);
            if (best.has_value())
            {
                m_hasLock = true;
                m_lockPosPx = QPointF(best->x, best->y);
                // 关键：确保后续校准/导星的“质心跟踪”从本次新锁点开始，而不是沿用上次的 centroid
                m_lastGuideCentroid = m_lockPosPx;
                emit lockPositionChanged(m_lockPosPx);
                emit lockStarSelected(best->x, best->y, best->snr, best->hfd);
                emit infoMessage(QStringLiteral("选星成功：x=%1 y=%2 SNR=%3 HFD=%4")
                                 .arg(best->x, 0, 'f', 2)
                                 .arg(best->y, 0, 'f', 2)
                                 .arg(best->snr, 0, 'f', 1)
                                 .arg(best->hfd, 0, 'f', 2));
                startGuidingFromLock(false);
            }
        }
    }
    else if (m_state == guiding::State::Calibrating && m_phd2Calib.isActive())
    {
        cv::Mat img16;
        if (Tools::readFits(fitsPath.toUtf8().constData(), img16) == 0 && !img16.empty())
        {
            // 校准阶段：方框跟随“当前星点质心”，十字线保持锁点不动
            if (m_lastGuideCentroid.isNull())
                m_lastGuideCentroid = m_lockPosPx;
            QPointF calibCentroid;
            if (guiding::FindCentroidWeightedStrict(img16, m_lastGuideCentroid, 8, calibCentroid, 2.0)
                || guiding::FindCentroidWeighted(img16, m_lastGuideCentroid, 8, calibCentroid, 2.0))
            {
                m_lastGuideCentroid = calibCentroid;
                emit guideStarCentroidChanged(calibCentroid);
            }

            if (m_lastGuideCentroid.isNull())
            {
                auto step = guiding::phd2::CalibrationStepResult{};
                step.failed = true;
                step.done = true;
                step.errorMessage = QStringLiteral("CalibrationFailed:LostStar");
                // fall through to failure handling below
                if (step.failed || (step.done && !step.result.valid))
                {
                    m_phd2Calib.reset();
                    m_calibResult = guiding::CalibrationResult{};
                    const QString reason = step.errorMessage;
                    emit infoMessage(QStringLiteral("校准失败：%1").arg(reason));
                    emit errorOccurred(reason);
                    setState(guiding::State::Error);
                    scheduleNextExposure(0);
                    return;
                }
            }

            auto step = m_phd2Calib.onCentroid(m_lastGuideCentroid);
            if (!step.infoMessage.isEmpty())
                emit infoMessage(step.infoMessage);
            if (step.hasPulse)
            {
                // 关键：脉冲与下一次曝光必须分离（否则星点在曝光内拖影，质心会偏）
                emit requestPulse(step.pulse);
                nextExposureDelayMs = std::max(nextExposureDelayMs,
                                               std::max(0, step.pulse.durationMs) + std::max(0, m_params.settleMsAfterPulse));
                // 为了让 Qt 端日志/前端协议与导星阶段一致：校准阶段也上报 pulse issued。
                // 校准阶段没有 RA/DEC error 概念，这里用 NaN 表示 N/A。
                emit guidePulseIssued(step.pulse,
                                      std::numeric_limits<double>::quiet_NaN(),
                                      std::numeric_limits<double>::quiet_NaN());
            }
            if (step.failed || (step.done && !step.result.valid))
            {
                // 校准失败：进入 Error 并上报（但保持曝光继续，以免前端停帧误判为“卡住”）
                m_phd2Calib.reset();
                m_calibResult = guiding::CalibrationResult{};

                const QString reason = !step.errorMessage.isEmpty() ? step.errorMessage : QStringLiteral("CalibrationFailed");
                emit infoMessage(QStringLiteral("校准失败：%1").arg(reason));
                emit errorOccurred(reason);
                setState(guiding::State::Error);
                // 保持预览不断：若导星相机是单帧触发模式，需要继续曝光
                scheduleNextExposure(0);
                return;
            }
            if (step.done && step.result.valid)
            {
                m_calibResult = step.result;
                // 补齐元数据（用于复用校准/诊断）
                m_calibResult.lockPosPx = m_lockPosPx;
                m_calibResult.timestampMs = QDateTime::currentMSecsSinceEpoch();
                emit calibrationResultChanged(m_calibResult);
                emit infoMessage(QStringLiteral("校准完成：camAngle=%1deg orthoErr=%2deg raMs/px=%3 decMs/px=%4 raSteps=%5 decSteps=%6")
                                 .arg(m_calibResult.cameraAngleDeg, 0, 'f', 2)
                                 .arg(m_calibResult.orthoErrDeg, 0, 'f', 2)
                                 .arg(m_calibResult.raMsPerPixel, 0, 'f', 2)
                                 .arg(m_calibResult.decMsPerPixel, 0, 'f', 2)
                                 .arg(m_calibResult.raStepCount)
                                 .arg(m_calibResult.decStepCount));

                // 保存最近一次成功校准快照（供后续换星复用）
                m_hasLastCalibration = true;
                m_lastCalibration = m_calibResult;
                m_lastCalibrationCtx.guiderBinning = std::max(1, m_params.guiderBinning);
                m_lastCalibrationCtx.guideSpeedSidereal = m_params.guideSpeedSidereal;
                m_lastCalibrationCtx.imageScaleArcsecPerPixel = computeImageScaleArcsecPerPixel(m_params);
                m_hasLastBacklash = false;
                m_lastBacklashMsBase = 0;
                m_lastBacklashMsRuntime = 0;

                // ===== 校准质量硬门槛（不达标直接失败，室外更安全）=====
                {
                    // NOTE: ms/px 很容易受“导星速率/步长设定/轴响应”影响，直接硬失败会造成“看起来一直校准失败”。
                    // 我们把 ms/px 越界降级为 Warning（仍允许进入 Guiding），但保留真正的硬失败项：
                    // - orthoErr 过大（轴不正交/串扰严重）
                    // - 轴位移过小（噪声主导，校准不可信）
                    QStringList hardReasons;
                    QStringList warnReasons;

                    if (m_calibResult.orthoErrDeg > m_params.calibMaxOrthoErrDeg)
                        hardReasons << QString("orthoErr=%1deg>%2deg").arg(m_calibResult.orthoErrDeg, 0, 'f', 2).arg(m_params.calibMaxOrthoErrDeg, 0, 'f', 2);
                    if (m_calibResult.raTravelPx > 0.0 && m_calibResult.raTravelPx < m_params.calibMinAxisMovePx)
                        hardReasons << QString("raMove=%1px<%2px").arg(m_calibResult.raTravelPx, 0, 'f', 2).arg(m_params.calibMinAxisMovePx, 0, 'f', 2);
                    if (m_calibResult.decTravelPx > 0.0 && m_calibResult.decTravelPx < m_params.calibMinAxisMovePx)
                        hardReasons << QString("decMove=%1px<%2px").arg(m_calibResult.decTravelPx, 0, 'f', 2).arg(m_params.calibMinAxisMovePx, 0, 'f', 2);

                    if (m_calibResult.raMsPerPixel < m_params.calibMinMsPerPixel || m_calibResult.raMsPerPixel > m_params.calibMaxMsPerPixel)
                        warnReasons << QString("raMsPerPx=%1 outOf[%2,%3]").arg(m_calibResult.raMsPerPixel, 0, 'f', 2)
                                        .arg(m_params.calibMinMsPerPixel, 0, 'f', 2)
                                        .arg(m_params.calibMaxMsPerPixel, 0, 'f', 2);
                    if (m_calibResult.decMsPerPixel < m_params.calibMinMsPerPixel || m_calibResult.decMsPerPixel > m_params.calibMaxMsPerPixel)
                        warnReasons << QString("decMsPerPx=%1 outOf[%2,%3]").arg(m_calibResult.decMsPerPixel, 0, 'f', 2)
                                        .arg(m_params.calibMinMsPerPixel, 0, 'f', 2)
                                        .arg(m_params.calibMaxMsPerPixel, 0, 'f', 2);

                    if (!hardReasons.isEmpty())
                    {
                        const QString msg = QStringLiteral("CalibrationQualityFailed:") + hardReasons.join(",");
                        emit infoMessage(QStringLiteral("校准质量不达标（硬失败）：%1").arg(hardReasons.join(" | ")));
                        emit errorOccurred(msg);
                        setState(guiding::State::Error);
                        scheduleNextExposure(0);
                        return;
                    }

                    if (!warnReasons.isEmpty())
                    {
                        QString extra;
                        const double imgScale = computeImageScaleArcsecPerPixel(m_params);
                        if (imgScale > 0.0 && m_params.guideSpeedSidereal > 0.0)
                        {
                            // 期望 ms/px：1000*imageScale/(15*guideSpeed)
                            const double expected = (1000.0 * imgScale) / (15.0 * m_params.guideSpeedSidereal);
                            extra = QStringLiteral("（预计≈%1 ms/px）").arg(expected, 0, 'f', 1);
                        }
                        emit infoMessage(QStringLiteral("校准质量警告（继续导星）：%1%2。建议检查 GUIDE_RATE/导星速率、校准步长与脉冲是否生效。")
                                             .arg(warnReasons.join(" | "))
                                             .arg(extra));
                    }
                }

                // 进入导星前：把十字线（lock target）移动到“当前质心位置”，然后再开始导星
                if (!m_lastGuideCentroid.isNull())
                {
                    m_lockPosPx = m_lastGuideCentroid;
                    emit lockPositionChanged(m_lockPosPx);
                }

                // ===== 新增：导星前 DEC 回差测量（单位：ms）=====
                // 测量会在 Calibrating 状态内继续进行；完成后再进入 Guiding。
                if (m_params.enableDecBacklashMeasure)
                {
                    m_decBacklashMeasureActive = true;
                    m_decBacklash.start(m_calibResult, m_params, m_lockPosPx);
                    emit infoMessage(QStringLiteral("DEC 回差测量开始：先 NORTH 预加载到 %1px，再 SOUTH 探测回差...")
                                         .arg(m_params.decBacklashNorthTargetPx, 0, 'f', 1));
                    // 关键：必须继续触发下一帧曝光，否则回差测量阶段没有新帧驱动就会“卡住”
                    scheduleNextExposure(0);
                    return;
                }

                enterGuidingState();
            }
        }
    }
    else if (m_state == guiding::State::Calibrating && m_decBacklashMeasureActive && m_calibResult.valid)
    {
        // 回差测量阶段：复用导星阶段的“锁星附近质心”跟踪
        cv::Mat img16;
        if (Tools::readFits(fitsPath.toUtf8().constData(), img16) == 0 && !img16.empty())
        {
            if (m_lastGuideCentroid.isNull())
                m_lastGuideCentroid = m_lockPosPx;

            QPointF centroid;
            bool gotCentroid = guiding::FindCentroidWeightedStrict(img16, m_lastGuideCentroid, 8, centroid, 2.0);
            if (!gotCentroid)
            {
                gotCentroid = guiding::FindCentroidWeighted(img16, m_lastGuideCentroid, 16, centroid, 2.0) ||
                              guiding::FindCentroidWeighted(img16, m_lockPosPx,        16, centroid, 2.0) ||
                              guiding::FindCentroidWeighted(img16, m_lockPosPx,        24, centroid, 2.0);
            }

            if (gotCentroid)
            {
                m_lastGuideCentroid = centroid;
                emit guideStarCentroidChanged(centroid);

                auto r = m_decBacklash.onFrame(centroid);
                if (!r.infoMessage.isEmpty())
                    emit infoMessage(r.infoMessage);

                if (r.hasPulse)
                {
                    emit requestPulse(r.pulse);
                    nextExposureDelayMs = std::max(nextExposureDelayMs,
                                                   std::max(0, r.pulse.durationMs) + std::max(0, m_params.settleMsAfterPulse));
                    emit guidePulseIssued(r.pulse,
                                          std::numeric_limits<double>::quiet_NaN(),
                                          std::numeric_limits<double>::quiet_NaN());
                }

                if (r.done)
                {
                    m_decBacklashMeasureActive = false;
                    if (r.failed)
                    {
                        const QString reason = !r.errorMessage.isEmpty() ? r.errorMessage : QStringLiteral("DecBacklashMeasureFailed");
                        // Backlash measurement failure should not be fatal for guiding.
                        // We'll continue guiding, but disable DEC backlash compensation.
                        m_decBacklashMsBase = 0;
                        m_decBacklashMsRuntime = 0;
                        emit infoMessage(QStringLiteral("DEC 回差测量失败（将继续导星，不使用回差补偿）：%1").arg(reason));
                    }
                    else
                    {
                        m_decBacklashMsBase = std::max(0, r.backlashMs);
                        m_decBacklashMsRuntime = m_decBacklashMsBase;
                        m_hasLastBacklash = true;
                        m_lastBacklashMsBase = m_decBacklashMsBase;
                        m_lastBacklashMsRuntime = m_decBacklashMsRuntime;
                        emit infoMessage(QStringLiteral("DEC 回差测量结果：backlash=%1ms（后续导星将使用/可自适应调整）")
                                             .arg(m_decBacklashMsBase));
                    }

                    // 关键修复：回差测量阶段会用脉冲把星点推离 lockPos。
                    // 为避免“导星一开始误差巨大”，在进入 Guiding 前把 lockPos 重新对齐到当前质心位置。
                    m_lockPosPx = m_lastGuideCentroid;
                    emit lockPositionChanged(m_lockPosPx);

                    enterGuidingState();
                }
            }
            else
            {
                emit infoMessage(QStringLiteral("DEC 回差测量：质心跟踪失败（可能丢星/过曝/云层）。将继续尝试。"));
                m_lastGuideCentroid = m_lockPosPx;
            }
        }
    }
    else if (m_state == guiding::State::Guiding && m_calibResult.valid)
    {
        cv::Mat img16;
        if (Tools::readFits(fitsPath.toUtf8().constData(), img16) == 0 && !img16.empty())
        {
            m_guidingFrameCount++;
            if (!m_guidingDiagTimer.isValid())
                m_guidingDiagTimer.restart();

            // 跟踪锁定星点（在锁点附近找质心）
            if (m_lastGuideCentroid.isNull())
                m_lastGuideCentroid = m_lockPosPx;

            QPointF centroid;
            bool gotCentroid = guiding::FindCentroidWeightedStrict(img16, m_lastGuideCentroid, 8, centroid, 2.0);
            if (!gotCentroid)
            {
                // Guiding stage: keep strict behavior so \"lost star\" can be detected reliably.
                // If we fall back to the peak pixel on a blank/noisy frame, we would wrongly think we still have a star.
                gotCentroid = guiding::FindCentroidWeightedStrict(img16, m_lastGuideCentroid, 16, centroid, 2.0) ||
                              guiding::FindCentroidWeightedStrict(img16, m_lockPosPx,        16, centroid, 2.0) ||
                              guiding::FindCentroidWeightedStrict(img16, m_lockPosPx,        24, centroid, 2.0);
                // If strict failed but we still have a strong peak near lock position, allow non-strict fallback.
                // This avoids rare false-negative thresholding while still failing on blank frames (peak ~ 0).
                if (!gotCentroid)
                {
                    const int cx = static_cast<int>(std::llround(m_lockPosPx.x()));
                    const int cy = static_cast<int>(std::llround(m_lockPosPx.y()));
                    const int half = 24;
                    const int x0 = std::max(0, cx - half);
                    const int y0 = std::max(0, cy - half);
                    const int x1 = std::min(img16.cols - 1, cx + half);
                    const int y1 = std::min(img16.rows - 1, cy + half);
                    if (x1 > x0 && y1 > y0)
                    {
                        cv::Mat roi = img16(cv::Rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1));
                        double minV = 0.0, maxV = 0.0;
                        cv::minMaxLoc(roi, &minV, &maxV);
                        if (maxV >= 1000.0)
                        {
                            gotCentroid = guiding::FindCentroidWeighted(img16, m_lastGuideCentroid, 16, centroid, 2.0) ||
                                          guiding::FindCentroidWeighted(img16, m_lockPosPx,        16, centroid, 2.0) ||
                                          guiding::FindCentroidWeighted(img16, m_lockPosPx,        24, centroid, 2.0);
                        }
                    }
                }
            }

            if (gotCentroid)
            {
                m_centroidFailCount = 0;
                m_lastGuideCentroid = centroid;
                emit guideStarCentroidChanged(centroid);

                // ===== PHD2 对齐路径：使用 PHD2 同款算法结构（RA Hysteresis + DEC ResistSwitch） =====
                // 先在这里“短路”旧的自定义导星逻辑，确保导星闭环行为以 PHD2 为准。
                // 后续会把下方旧逻辑整体删掉（当前先保证功能与效果对齐、构建通过）。
                {
                    auto out2 = m_phd2Guiding.compute(m_calibResult, m_params, m_lockPosPx, centroid);

                    // ===== 关键修复：DEC 单向锁定后，若“需要纠偏但被门控”持续发生，则自动翻向 =====
                    // 现象：DEC 锁错方向时，RA 仍会不断发脉冲，导致 DEC 长期得不到纠正而持续漂离。
                    // 处理：连续若干帧检测到 decNeed=true 但 decAllowed=false，则将 allowedDecDirs 翻到期望方向。
                    if (m_params.autoDecGuideDir && out2.decNeed && !out2.decAllowed)
                    {
                        m_decGatedCount++;
                        if (m_decGatedCount >= AUTO_ADJUST_DEC_THRESHOLD)
                        {
                            auto newParams = m_params;
                            newParams.allowedDecDirs.clear();
                            newParams.allowedDecDirs.insert(out2.decDir);
                            m_params = newParams;
                            emit paramsChanged();

                            emit infoMessage(QStringLiteral("自动调整 DEC 单向方向：%1（检测到 DEC 需要纠正但被门控，已连续%2帧）")
                                                 .arg(dirToStr(out2.decDir))
                                                 .arg(AUTO_ADJUST_DEC_THRESHOLD));
                            m_decGatedCount = 0;

                            // 方向翻转后立即重算一次，让本帧就有机会发出 DEC 纠偏脉冲
                            out2 = m_phd2Guiding.compute(m_calibResult, m_params, m_lockPosPx, centroid);
                        }
                    }
                    else
                    {
                        // 一旦 DEC 不再被门控（或 AUTO 关闭），清零计数，避免误触发
                        m_decGatedCount = 0;
                    }

                    // ===== pulse effectiveness check update (uses current errors, before issuing a new pulse) =====
                    if (m_params.enablePulseEffectCheck && m_pulseEffActive && m_pulseEffFramesLeft > 0)
                    {
                        const double aRa = std::abs(out2.raErrPx);
                        const double aDec = std::abs(out2.decErrPx);
                        if (aRa < m_pulseEffBestAbsRa) m_pulseEffBestAbsRa = aRa;
                        if (aDec < m_pulseEffBestAbsDec) m_pulseEffBestAbsDec = aDec;
                        m_pulseEffFramesLeft--;
                        if (m_pulseEffFramesLeft <= 0)
                        {
                            const double improveRa = m_pulseEffStartAbsRa - m_pulseEffBestAbsRa;
                            const double improveDec = m_pulseEffStartAbsDec - m_pulseEffBestAbsDec;
                            const double minImprove = std::max(0.0, m_params.pulseEffectMinImprovePx);

                            const bool ok = (improveRa >= minImprove) || (improveDec >= minImprove);
                            if (!ok)
                            {
                                m_pulseEffFailStreak++;
                                emit infoMessage(QStringLiteral("PulseEffect: weak/no improvement (ra:%1→%2, dec:%3→%4, streak=%5/%6)")
                                                     .arg(m_pulseEffStartAbsRa, 0, 'f', 3)
                                                     .arg(m_pulseEffBestAbsRa, 0, 'f', 3)
                                                     .arg(m_pulseEffStartAbsDec, 0, 'f', 3)
                                                     .arg(m_pulseEffBestAbsDec, 0, 'f', 3)
                                                     .arg(m_pulseEffFailStreak)
                                                     .arg(m_params.pulseEffectMaxFailures));
                                if (m_pulseEffFailStreak >= std::max(1, m_params.pulseEffectMaxFailures))
                                {
                                    const QString reason = QStringLiteral("PulseNoEffect:CheckGuideRateOrMountDriver");
                                    emit errorOccurred(reason);
                                    setState(guiding::State::Error);
                                    scheduleNextExposure(0);
                                    return;
                                }
                            }
                            else
                            {
                                m_pulseEffFailStreak = 0;
                            }
                            m_pulseEffActive = false;
                        }
                    }

                    // ===== DEC 单向策略：收集/判定/锁定（在 PHD2 路径中同样生效）=====
                    if (m_params.autoDecGuideDir && m_decUniPolicyActive && !m_decUniPolicyDecided)
                    {
                        const double decErr = out2.decErrPx;
                        m_decUniFrames++;
                        m_decUniSum += decErr;
                        m_decUniSumSq += decErr * decErr;

                        const double rms = std::sqrt(m_decUniSumSq / std::max(1, m_decUniFrames));
                        if (!m_decUniLargeMove && rms >= std::max(0.0, m_params.decUniLargeMoveRmsPx))
                            m_decUniLargeMove = true;

                        // 采集漂移样本（用于 case(1) 的 slope 判定）
                        GuiderCore::DriftSample s;
                        s.tSec = m_decDriftTimer.elapsed() / 1000.0;
                        s.raErrPx = out2.raErrPx;
                        s.decErrPx = out2.decErrPx;
                        m_decDriftSamples.push_back(s);

                        const int needFrames = m_decUniLargeMove ? std::max(1, m_params.decUniInitialFrames)
                                                                 : std::max(1, m_params.decUniCollectFrames);
                        if (m_decUniFrames >= needFrames)
                        {
                            guiding::GuideDir chosenDir = guiding::GuideDir::North;
                            if (m_decUniLargeMove)
                            {
                                // case(2)：用均值趋势快速判定
                                const double mean = m_decUniSum / m_decUniFrames;
                                if (std::abs(mean) >= std::max(0.0, m_params.decUniMinAbsMeanPx))
                                {
                                    chosenDir = (mean > 0.0) ? guiding::GuideDir::South : guiding::GuideDir::North;
                                    m_decUniPolicyDecided = true;
                                }
                            }
                            else
                            {
                                // case(1)：小漂移，尽量用 slope 判定长期趋势
                                const double slope = fitSlopeLeastSquares(m_decDriftSamples, false /*useRa*/);
                                if (std::isfinite(slope) && std::abs(slope) >= m_params.driftDetectMinAbsSlopePxPerSec)
                                {
                                    chosenDir = (slope > 0.0) ? guiding::GuideDir::South : guiding::GuideDir::North;
                                    m_decUniPolicyDecided = true;
                                }
                                else
                                {
                                    // 斜率太小/不可靠：退化为“当前趋势”
                                    chosenDir = (out2.decErrPx > 0.0) ? guiding::GuideDir::South : guiding::GuideDir::North;
                                    m_decUniPolicyDecided = true;
                                }
                            }

                            if (m_decUniPolicyDecided)
                            {
                                auto newParams = m_params;
                                newParams.allowedDecDirs.clear();
                                newParams.allowedDecDirs.insert(chosenDir);
                                m_params = newParams;
                                m_decUniPolicyActive = false;
                                m_quickDirectionDetectActive = false;
                                emit directionDetectionStateChanged(false);
                                emit paramsChanged();

                                emit infoMessage(QStringLiteral("DEC 单向方向已锁定为：%1（frames=%2 rms=%3px mode=%4）")
                                                     .arg(dirToStr(chosenDir))
                                                     .arg(m_decUniFrames)
                                                     .arg(rms, 0, 'f', 3)
                                                     .arg(m_decUniLargeMove ? "FAST" : "COLLECT"));
                            }
                        }
                    }

                    // 连续误差曲线
                    emit guideErrorUpdated(out2.raErrPx, out2.decErrPx);

                    // rolling RMS
                    m_rms.push(out2.raErrPx, out2.decErrPx);
                    m_rmsEmitCounter++;
                    if (m_rms.ready() && (m_rmsEmitCounter % std::max(1, m_params.rmsEmitEveryFrames) == 0))
                    {
                        const double raR = m_rms.raRms();
                        const double decR = m_rms.decRms();
                        const double totR = m_rms.totalRms();
                        if (m_params.pixelScaleArcsecPerPixel > 0.0)
                        {
                            const double raA = raR * m_params.pixelScaleArcsecPerPixel;
                            const double decA = decR * m_params.pixelScaleArcsecPerPixel;
                            const double totA = totR * m_params.pixelScaleArcsecPerPixel;
                            emit infoMessage(QStringLiteral("RMS[%1]: ra=%2px(%3\") dec=%4px(%5\") tot=%6px(%7\")")
                                                 .arg((int)m_rms.ra2.size())
                                                 .arg(raR, 0, 'f', 3).arg(raA, 0, 'f', 2)
                                                 .arg(decR, 0, 'f', 3).arg(decA, 0, 'f', 2)
                                                 .arg(totR, 0, 'f', 3).arg(totA, 0, 'f', 2));
                        }
                        else
                        {
                            emit infoMessage(QStringLiteral("RMS[%1]: ra=%2px dec=%3px tot=%4px")
                                                 .arg((int)m_rms.ra2.size())
                                                 .arg(raR, 0, 'f', 3)
                                                 .arg(decR, 0, 'f', 3)
                                                 .arg(totR, 0, 'f', 3));
                        }
                    }

                    int delayMs = 0;
                    if (out2.pulse.has_value() && out2.pulse->durationMs > 0)
                    {
                        guiding::PulseCommand cmd = *out2.pulse;

                        // PHD2-like DEC backlash compensation: when DEC direction reverses, add a one-shot extra pulse.
                        if (m_params.enableDecBacklashCompensation
                            && (cmd.dir == guiding::GuideDir::North || cmd.dir == guiding::GuideDir::South)
                            && m_decBacklashMsRuntime > 0)
                        {
                            if (m_lastDecPulseDir.has_value() && *m_lastDecPulseDir != cmd.dir)
                            {
                                const int compMs = std::min(m_params.maxPulseMs, std::max(0, m_decBacklashMsRuntime));
                                const int newMs = std::min(m_params.maxPulseMs, std::max(0, cmd.durationMs) + compMs);
                                if (newMs != cmd.durationMs)
                                {
                                    cmd.durationMs = newMs;
                                    emit infoMessage(QStringLiteral("DEC 回差补偿：dir=%1 added=%2ms total=%3ms")
                                                         .arg(dirToStr(cmd.dir))
                                                         .arg(compMs)
                                                         .arg(cmd.durationMs));
                                }
                            }
                            m_lastDecPulseDir = cmd.dir;
                        }

                        // ===== pulse effectiveness check: arm window after issuing a pulse =====
                        if (m_params.enablePulseEffectCheck
                            && (std::max(std::abs(out2.raErrPx), std::abs(out2.decErrPx)) >= m_params.pulseEffectMinStartAbsErrPx))
                        {
                            m_pulseEffActive = true;
                            m_pulseEffFramesLeft = std::max(1, m_params.pulseEffectWindowFrames);
                            m_pulseEffStartAbsRa = std::abs(out2.raErrPx);
                            m_pulseEffStartAbsDec = std::abs(out2.decErrPx);
                            m_pulseEffBestAbsRa = m_pulseEffStartAbsRa;
                            m_pulseEffBestAbsDec = m_pulseEffStartAbsDec;
                        }

                        emit requestPulse(cmd);
                        emit guidePulseIssued(cmd, out2.raErrPx, out2.decErrPx);
                        delayMs = std::max(0, cmd.durationMs) + std::max(0, m_params.settleMsAfterPulse);
                    }
                    else
                    {
                        if (m_guidingDiagTimer.elapsed() >= 3000)
                        {
                            m_guidingDiagTimer.restart();
                            emit infoMessage(QStringLiteral("导星中(PHD2)：frame=%1 raErrPx=%2 decErrPx=%3 | no-pulse")
                                                 .arg(m_guidingFrameCount)
                                                 .arg(out2.raErrPx, 0, 'f', 3)
                                                 .arg(out2.decErrPx, 0, 'f', 3));
                        }
                    }

                    scheduleNextExposure(delayMs);
                    return;
                }

#if 0
                // ===== 旧的自定义导星逻辑（已废弃）=====
                // 说明：为了达到“与 PHD2 一致”的效果，我们已在上面使用 phd2::MountGuiding 接管导星闭环。
                // 这里保留旧逻辑仅用于对照，后续会整体删除。

                // ===== DEC 回差自适应：在反向后观测若干帧误差改善程度，调整回差(ms) =====
                // 注意：这里使用“当前帧误差”（未发本帧脉冲之前）来更新观测窗口。
                // 误差值来自后续 compute，因此先暂存 centroid，等 out 得到后再更新窗口。

                // 导星计算：DEC 单向策略判定期间，允许 DEC 双向；判定结束后锁定单向
                guiding::GuidingParams paramsForCompute = m_params;

                // 先用 raw error 计算（用于上报/漂移检测/诊断），脉冲再由“滤波后的误差”决定
                auto out = m_controller.compute(m_calibResult, paramsForCompute, m_lockPosPx, centroid);

                // ===== pulse effectiveness check update (uses raw errors, before new pulse) =====
                if (m_params.enablePulseEffectCheck && m_pulseEffActive && m_pulseEffFramesLeft > 0)
                {
                    const double aRa = std::abs(out.raErrPx);
                    const double aDec = std::abs(out.decErrPx);
                    if (aRa < m_pulseEffBestAbsRa) m_pulseEffBestAbsRa = aRa;
                    if (aDec < m_pulseEffBestAbsDec) m_pulseEffBestAbsDec = aDec;
                    m_pulseEffFramesLeft--;
                    if (m_pulseEffFramesLeft <= 0)
                    {
                        const double improveRa = m_pulseEffStartAbsRa - m_pulseEffBestAbsRa;
                        const double improveDec = m_pulseEffStartAbsDec - m_pulseEffBestAbsDec;
                        const double minImprove = std::max(0.0, m_params.pulseEffectMinImprovePx);

                        const bool ok =
                            (improveRa >= minImprove) || (improveDec >= minImprove);

                        if (!ok)
                        {
                            m_pulseEffFailStreak++;
                            emit infoMessage(QStringLiteral("PulseEffect: weak/no improvement (ra:%1→%2, dec:%3→%4, streak=%5/%6)")
                                                 .arg(m_pulseEffStartAbsRa, 0, 'f', 3)
                                                 .arg(m_pulseEffBestAbsRa, 0, 'f', 3)
                                                 .arg(m_pulseEffStartAbsDec, 0, 'f', 3)
                                                 .arg(m_pulseEffBestAbsDec, 0, 'f', 3)
                                                 .arg(m_pulseEffFailStreak)
                                                 .arg(m_params.pulseEffectMaxFailures));
                            if (m_pulseEffFailStreak >= std::max(1, m_params.pulseEffectMaxFailures))
                            {
                                const QString reason = QStringLiteral("PulseNoEffect:CheckGuideRateOrMountDriver");
                                emit errorOccurred(reason);
                                setState(guiding::State::Error);
                                scheduleNextExposure(0);
                                return;
                            }
                        }
                        else
                        {
                            m_pulseEffFailStreak = 0;
                        }

                        m_pulseEffActive = false;
                    }
                }

                // ===== DEC 单向策略：收集/判定/锁定 =====
                if (m_params.autoDecGuideDir && m_decUniPolicyActive && !m_decUniPolicyDecided)
                {
                    const double decErr = out.decErrPx;
                    m_decUniFrames++;
                    m_decUniSum += decErr;
                    m_decUniSumSq += decErr * decErr;

                    const double rms = std::sqrt(m_decUniSumSq / std::max(1, m_decUniFrames));
                    if (!m_decUniLargeMove && rms >= std::max(0.0, m_params.decUniLargeMoveRmsPx))
                        m_decUniLargeMove = true;

                    // 采集漂移样本（用于 case(1) 的 slope 判定）
                    GuiderCore::DriftSample s;
                    s.tSec = m_decDriftTimer.elapsed() / 1000.0;
                    s.raErrPx = out.raErrPx;
                    s.decErrPx = out.decErrPx;
                    m_decDriftSamples.push_back(s);

                    const int needFrames = m_decUniLargeMove ? std::max(1, m_params.decUniInitialFrames)
                                                             : std::max(1, m_params.decUniCollectFrames);

                    if (m_decUniFrames >= needFrames)
                    {
                        guiding::GuideDir chosenDir = guiding::GuideDir::North;

                        if (m_decUniLargeMove)
                        {
                            // case(2)：用均值趋势快速判定
                            const double mean = m_decUniSum / m_decUniFrames;
                            if (std::abs(mean) < std::max(0.0, m_params.decUniMinAbsMeanPx))
                            {
                                // 趋势不明显：再多收集几帧
                                // （不改变 needFrames，下一帧会再次进入判定）
                            }
                            else
                            {
                                chosenDir = (mean > 0.0) ? guiding::GuideDir::South : guiding::GuideDir::North;
                                m_decUniPolicyDecided = true;
                            }
                        }
                        else
                        {
                            // case(1)：小漂移，尽量用 slope 判定长期趋势
                            const double slope = fitSlopeLeastSquares(m_decDriftSamples, false /*useRa*/);
                            if (std::isfinite(slope) && std::abs(slope) >= m_params.driftDetectMinAbsSlopePxPerSec)
                            {
                                chosenDir = (slope > 0.0) ? guiding::GuideDir::South : guiding::GuideDir::North;
                                m_decUniPolicyDecided = true;
                            }
                            else
                            {
                                // 斜率太小/不可靠：退化为“当前趋势”以满足“收集足够数据后锁单向”的需求
                                const guiding::GuideDir currentTrend = (out.decErrPx > 0.0) ? guiding::GuideDir::South : guiding::GuideDir::North;
                                chosenDir = currentTrend;
                                m_decUniPolicyDecided = true;
                            }
                        }

                        if (m_decUniPolicyDecided)
                        {
                            auto newParams = m_params;
                            newParams.allowedDecDirs.clear();
                            newParams.allowedDecDirs.insert(chosenDir);
                            m_params = newParams;
                            m_decUniPolicyActive = false;

                            m_quickDirectionDetectActive = false;
                            emit directionDetectionStateChanged(false);
                            emit paramsChanged();

                            emit infoMessage(QStringLiteral("DEC 单向方向已锁定为：%1（frames=%2 rms=%3px mode=%4）")
                                             .arg(dirToStr(chosenDir))
                                             .arg(m_decUniFrames)
                                             .arg(rms, 0, 'f', 3)
                                             .arg(m_decUniLargeMove ? "FAST" : "COLLECT"));
                        }
                    }
                }

                // EMA滤波（只影响控制，不影响上报曲线）
                double raCtrlErrPx = out.raErrPx;
                double decCtrlErrPx = out.decErrPx;
                if (m_params.enableErrorEma && m_params.errorEmaAlpha > 0.0 && m_params.errorEmaAlpha <= 1.0)
                {
                    const double a = m_params.errorEmaAlpha;
                    if (!m_errEmaInit)
                    {
                        m_errEmaInit = true;
                        m_raErrEma = out.raErrPx;
                        m_decErrEma = out.decErrPx;
                    }
                    else
                    {
                        m_raErrEma = a * out.raErrPx + (1.0 - a) * m_raErrEma;
                        m_decErrEma = a * out.decErrPx + (1.0 - a) * m_decErrEma;
                    }
                    raCtrlErrPx = m_raErrEma;
                    decCtrlErrPx = m_decErrEma;
                }

                // 用“控制误差”重新决定本帧脉冲（覆盖 compute() 的默认决策）
                // 注意：后续还需要用 decision 里的诊断信息来处理“被单向门控但未发出脉冲”的场景，
                // 所以这里统一走 computePulseDecision（不要只拿 pulse）
                guiding::PulseDecision decision =
                    m_controller.computePulseDecision(m_calibResult, paramsForCompute, raCtrlErrPx, decCtrlErrPx);
                out.pulse = decision.pulse;

                // 无论是否发脉冲，都上报本帧误差（用于前端连续曲线）
                emit guideErrorUpdated(out.raErrPx, out.decErrPx);

                // Helper: apply RA hysteresis to the current pulse decision.
                // We may recompute decision inside Emergency Stage-2 enter/exit, so keep this as a reusable block.
                auto applyRaHysteresis = [&]() {
                    // 定义 signed ms：West 为正，East 为负。signedMs = -raErrPx * ms/px * aggression
                    // - raErrPx>0 代表星点沿 RA 正向（West 脉冲正向）偏离，应发 East（负号），因此 signed 为负
                    if (!m_params.enableRaHysteresis || m_calibResult.raMsPerPixel <= 0.0)
                        return;

                    const double minMove = (m_params.raMinMovePx > 0.0) ? m_params.raMinMovePx : m_params.deadbandPx;
                    const bool need = std::abs(raCtrlErrPx) >= std::max(0.0, minMove);
                    if (!need)
                    {
                        if (m_raHysInit)
                            m_raHysPrevSignedMs *= 0.5;
                        return;
                    }

                    const double signedRaw =
                        (-raCtrlErrPx) * m_calibResult.raMsPerPixel * std::max(0.0, m_params.raAggression);

                    const double h = m_params.raHysteresis;
                    const double signedFiltered = (!m_raHysInit)
                        ? signedRaw
                        : (h * m_raHysPrevSignedMs + (1.0 - h) * signedRaw);
                    m_raHysInit = true;
                    m_raHysPrevSignedMs = signedFiltered;

                    guiding::GuideDir dir = (signedFiltered >= 0.0) ? guiding::GuideDir::West : guiding::GuideDir::East;
                    const double absMs = std::abs(signedFiltered);

                    // clamp to global min/max
                    int ms = static_cast<int>(std::llround(absMs));
                    ms = std::max(m_params.minPulseMs, ms);
                    ms = std::min(m_params.maxPulseMs, ms);

                    // per-frame step limiting (magnitude)
                    int step = std::max(0, m_params.raMaxStepMsPerFrame);
                    if (m_params.maxPulseStepPerFrameMs > 0)
                        step = (step > 0) ? std::min(step, m_params.maxPulseStepPerFrameMs) : m_params.maxPulseStepPerFrameMs;

                    if (step > 0 && m_lastRaPulseMs > 0)
                    {
                        const int delta = ms - m_lastRaPulseMs;
                        if (delta > step) ms = m_lastRaPulseMs + step;
                        else if (delta < -step) ms = std::max(m_params.minPulseMs, m_lastRaPulseMs - step);
                    }

                    // update decision fields so downstream logic (multi-axis, gating logs) uses hysteresis result
                    decision.raNeed = true;
                    decision.desiredRaDir = dir;
                    decision.rawRaMs = absMs;
                    decision.clampedRaMs = ms;
                    decision.raAllowed = (m_params.allowedRaDirs.find(dir) != m_params.allowedRaDirs.end());

                    // Ensure decision.pulse is consistent with updated RA.
                    if (decision.raAllowed && (!decision.decAllowed || std::abs(raCtrlErrPx) >= std::abs(decCtrlErrPx)))
                    {
                        guiding::PulseCommand cmd;
                        cmd.dir = dir;
                        cmd.durationMs = ms;
                        decision.pulse = cmd;
                    }
                    else if (!decision.decAllowed && !decision.raAllowed)
                    {
                        decision.pulse.reset();
                    }
                };

                // Apply once now; if Emergency recomputes decision, we'll apply again after that.
                applyRaHysteresis();

                // ===== 两段式应急兜底（PHD2 思路：卡住/越修越偏时临时放宽策略）=====
                // 目标：当 DEC 被单向门控卡死、误差持续增大时：
                // - Stage-1：允许一次短“反向”脉冲解卡（即使被 allowedDecDirs 门控）
                // - Stage-2：若继续变坏，临时允许 DEC 双向直到回到安全带
                std::optional<guiding::PulseCommand> emergencyDecPulse;
                if (m_params.enableEmergency)
                {
                    const double absDec = std::abs(out.decErrPx);
                    const double eps = std::max(0.0, m_params.emergencyGrowEpsPx);

                    if (!m_emergencyHasLastAbs)
                    {
                        m_emergencyHasLastAbs = true;
                        m_emergencyLastAbsDecPx = absDec;
                        m_emergencyGrowHit = 0;
                    }
                    else
                    {
                        if (absDec >= m_emergencyLastAbsDecPx + eps)
                            m_emergencyGrowHit++;
                        else
                            m_emergencyGrowHit = 0;
                        m_emergencyLastAbsDecPx = absDec;
                    }

                    // Stage-2 exit: back inside safe band
                    if (m_emergencyStage2Active && absDec <= std::max(0.0, m_params.emergencySafeBandPx))
                    {
                        if (m_emergencySavedAllowed)
                        {
                            auto newParams = m_params;
                            newParams.allowedDecDirs = m_emergencySavedAllowedDecDirs;
                            m_params = newParams;
                            emit paramsChanged();
                        }
                        m_emergencyStage2Active = false;
                        m_emergencyGrowHit = 0;
                        emit infoMessage(QStringLiteral("Emergency Stage-2 exit (absDec=%1px <= safeBand=%2px)")
                                             .arg(absDec, 0, 'f', 3)
                                             .arg(m_params.emergencySafeBandPx, 0, 'f', 3));

                        paramsForCompute = m_params;
                        decision = m_controller.computePulseDecision(m_calibResult, paramsForCompute, raCtrlErrPx, decCtrlErrPx);
                        applyRaHysteresis();
                    }

                    // Stage-2 enter: large error + worsening trend
                    if (!m_emergencyStage2Active
                        && absDec >= std::max(0.0, m_params.emergencyStage2AbsErrPx)
                        && m_emergencyGrowHit >= std::max(1, m_params.emergencyGrowFrames))
                    {
                        if (!m_emergencySavedAllowed)
                        {
                            m_emergencySavedAllowed = true;
                            m_emergencySavedAllowedDecDirs = m_params.allowedDecDirs;
                        }

                        auto newParams = m_params;
                        newParams.allowedDecDirs.clear();
                        newParams.allowedDecDirs.insert(guiding::GuideDir::North);
                        newParams.allowedDecDirs.insert(guiding::GuideDir::South);
                        m_params = newParams;
                        emit paramsChanged();

                        m_emergencyStage2Active = true;
                        emit infoMessage(QStringLiteral("Emergency Stage-2 enter (absDec=%1px >= %2px, growHit=%3/%4): allow DEC both dirs temporarily")
                                             .arg(absDec, 0, 'f', 3)
                                             .arg(m_params.emergencyStage2AbsErrPx, 0, 'f', 3)
                                             .arg(m_emergencyGrowHit)
                                             .arg(m_params.emergencyGrowFrames));

                        paramsForCompute = m_params;
                        decision = m_controller.computePulseDecision(m_calibResult, paramsForCompute, raCtrlErrPx, decCtrlErrPx);
                        applyRaHysteresis();
                    }

                    // Stage-1: blocked by DEC gating but error already large -> allow one short unblock pulse
                    if (!m_emergencyStage2Active
                        && decision.decNeed
                        && !decision.decAllowed
                        && absDec >= std::max(0.0, m_params.emergencyStage1AbsErrPx))
                    {
                        const int base = std::max(0, m_params.minPulseMs);
                        const int want = static_cast<int>(std::llround(base * std::max(0.0, m_params.stage1MinPulseFactor)));
                        const int ms = std::min(std::max(0, m_params.maxPulseMs),
                                                std::min(std::max(0, m_params.stage1MaxMs),
                                                         std::max(base, want)));

                        guiding::PulseCommand c;
                        c.dir = decision.desiredDecDir; // intentionally override gating
                        c.durationMs = ms;
                        emergencyDecPulse = c;

                        emit infoMessage(QStringLiteral("Emergency Stage-1 unblock: issue %1 %2ms (absDec=%3px, allowedDEC gate active)")
                                             .arg(dirToStr(c.dir))
                                             .arg(c.durationMs)
                                             .arg(absDec, 0, 'f', 3));
                    }
                }

                // 更新 DEC 回差自适应观测窗口（使用本帧 out.decErrPx）
                if (m_decBacklashAdaptActive)
                {
                    const double absErr = std::abs(out.decErrPx);
                    if (absErr < m_decBacklashAdaptMinAbsErrPx)
                        m_decBacklashAdaptMinAbsErrPx = absErr;
                    m_decBacklashAdaptFramesLeft--;

                    if (m_decBacklashAdaptFramesLeft <= 0)
                    {
                        const double improve = m_decBacklashAdaptStartAbsErrPx - m_decBacklashAdaptMinAbsErrPx;
                        const int stepMs = std::max(1, m_params.decBacklashAdaptiveStepMs);
                        const int maxMs = std::max(0, std::min(m_params.decBacklashAdaptiveMaxMs, m_params.maxPulseMs));

                        bool adjusted = false;
                        int old = m_decBacklashMsRuntime;

                        // overshoot：误差符号反转且仍明显超出 deadband
                        const bool overshoot = (m_decBacklashAdaptStartSignedErrPx * out.decErrPx < 0.0)
                                               && (std::abs(out.decErrPx) >= m_params.deadbandPx);

                        if (overshoot && m_params.enableDecBacklashAdaptive)
                        {
                            m_decBacklashMsRuntime = std::max(0, m_decBacklashMsRuntime - stepMs);
                            adjusted = (m_decBacklashMsRuntime != old);
                        }
                        else if (m_params.enableDecBacklashAdaptive
                                 && improve < std::max(0.0, m_params.decBacklashAdaptiveMinImprovePx))
                        {
                            m_decBacklashMsRuntime = std::min(maxMs, m_decBacklashMsRuntime + stepMs);
                            adjusted = (m_decBacklashMsRuntime != old);
                        }

                        if (adjusted)
                        {
                            emit infoMessage(QStringLiteral("DEC 回差自适应：runtime %1ms → %2ms（improve=%3px, minAbs=%4px, overshoot=%5）")
                                             .arg(old)
                                             .arg(m_decBacklashMsRuntime)
                                             .arg(improve, 0, 'f', 3)
                                             .arg(m_decBacklashAdaptMinAbsErrPx, 0, 'f', 3)
                                             .arg(overshoot ? "1" : "0"));
                        }

                        m_decBacklashAdaptActive = false;
                    }
                }

                // 采集 RA 漂移样本
                if (m_raDriftDetectActive)
                {
                    const double tSec = std::max(0.0, m_raDriftTimer.elapsed() / 1000.0);
                    m_raDriftSamples.push_back(DriftSample{tSec, out.raErrPx, out.decErrPx});

                    const bool enoughSamples = static_cast<int>(m_raDriftSamples.size()) >= std::max(2, m_params.driftDetectMinSamples);
                    const bool enoughTime = m_raDriftTimer.elapsed() >= std::max(1000, m_params.driftDetectDurationMs);
                    if (enoughSamples && enoughTime)
                    {
                        const double slopePxPerSec = fitSlopeLeastSquares(m_raDriftSamples, true);
                        m_raDriftDetectActive = false;

                        if (std::isfinite(slopePxPerSec) && std::abs(slopePxPerSec) >= m_params.driftDetectMinAbsSlopePxPerSec)
                        {
                            const QString driftTrend = (slopePxPerSec > 0.0) ? "EAST" : "WEST";
                            const guiding::GuideDir recommendDir = (slopePxPerSec > 0.0) ? guiding::GuideDir::West : guiding::GuideDir::East;

                            QString rateStr;
                            QString rateDbg;
                            if (m_params.pixelScaleArcsecPerPixel > 0.0)
                            {
                                const double arcsecPerSec = slopePxPerSec * m_params.pixelScaleArcsecPerPixel;
                                rateStr = QString("%1 arcsec/s").arg(std::abs(arcsecPerSec), 0, 'f', 3);
                                rateDbg = QString("slope=%1 px/s (%2 arcsec/s)")
                                              .arg(slopePxPerSec, 0, 'f', 6)
                                              .arg(arcsecPerSec, 0, 'f', 6);
                            }
                            else
                            {
                                rateStr = QString("%1 px/s").arg(std::abs(slopePxPerSec), 0, 'f', 4);
                                rateDbg = QString("slope=%1 px/s").arg(slopePxPerSec, 0, 'f', 6);
                            }

                            // 选项A：RA 永远双向 —— 只做趋势提示，不自动锁定方向
                            emit infoMessage(QStringLiteral("检测到 RA 长期漂移趋势为：%1（%2），提示：可通过机械 east-heavy/配重方式抵消（RA保持双向）")
                                             .arg(driftTrend)
                                             .arg(rateStr));
                            emit infoMessage(QStringLiteral("RA 漂移判定细节：samples=%1 window=%2s %3 pixScale=%4 arcsec/px")
                                             .arg(static_cast<int>(m_raDriftSamples.size()))
                                             .arg(m_params.driftDetectDurationMs / 1000.0, 0, 'f', 1)
                                             .arg(rateDbg)
                                             .arg(m_params.pixelScaleArcsecPerPixel, 0, 'f', 6));
                        }
                        else
                        {
                            // 未发现明显漂移：保持当前设置（如果当前是双向，则保持双向；如果是单向，则保持单向）
                            emit infoMessage(QStringLiteral("RA 漂移检测完成：未发现明显长期漂移（|slope|<%1 px/s），保持当前 RA 方向设置。")
                                             .arg(m_params.driftDetectMinAbsSlopePxPerSec, 0, 'f', 3));
                            if (std::isfinite(slopePxPerSec))
                            {
                                emit infoMessage(QStringLiteral("RA 漂移判定细节：samples=%1 window=%2s slope=%3 px/s")
                                                 .arg(static_cast<int>(m_raDriftSamples.size()))
                                                 .arg(m_params.driftDetectDurationMs / 1000.0, 0, 'f', 1)
                                                 .arg(slopePxPerSec, 0, 'f', 6));
                            }
                            // 选项A：RA 永远双向 —— 不做“锁单向”
                        }
                    }
                }

                // 采集 DEC 漂移样本
                if (m_decDriftDetectActive)
                {
                    const double tSec = std::max(0.0, m_decDriftTimer.elapsed() / 1000.0);
                    m_decDriftSamples.push_back(DriftSample{tSec, out.raErrPx, out.decErrPx});

                    const bool enoughSamples = static_cast<int>(m_decDriftSamples.size()) >= std::max(2, m_params.driftDetectMinSamples);
                    const bool enoughTime = m_decDriftTimer.elapsed() >= std::max(1000, m_params.driftDetectDurationMs);
                    if (enoughSamples && enoughTime)
                    {
                        const double slopePxPerSec = fitSlopeLeastSquares(m_decDriftSamples, false);
                        m_decDriftDetectActive = false;

                        if (std::isfinite(slopePxPerSec) && std::abs(slopePxPerSec) >= m_params.driftDetectMinAbsSlopePxPerSec)
                        {
                            const QString driftTrend = (slopePxPerSec > 0.0) ? "NORTH" : "SOUTH";
                            const guiding::GuideDir recommendDir = (slopePxPerSec > 0.0) ? guiding::GuideDir::South : guiding::GuideDir::North;

                            QString rateStr;
                            QString rateDbg;
                            if (m_params.pixelScaleArcsecPerPixel > 0.0)
                            {
                                const double arcsecPerSec = slopePxPerSec * m_params.pixelScaleArcsecPerPixel;
                                rateStr = QString("%1 arcsec/s").arg(std::abs(arcsecPerSec), 0, 'f', 3);
                                rateDbg = QString("slope=%1 px/s (%2 arcsec/s)")
                                              .arg(slopePxPerSec, 0, 'f', 6)
                                              .arg(arcsecPerSec, 0, 'f', 6);
                            }
                            else
                            {
                                rateStr = QString("%1 px/s").arg(std::abs(slopePxPerSec), 0, 'f', 4);
                                rateDbg = QString("slope=%1 px/s").arg(slopePxPerSec, 0, 'f', 6);
                            }

                            // 自动设置 DEC 单向方向为"漂移反向"并锁定（仅在 AUTO 模式下）
                            if (m_params.autoDecGuideDir)
                            {
                                auto newParams = m_params;
                                newParams.allowedDecDirs.clear();
                                newParams.allowedDecDirs.insert(recommendDir);
                                m_params = newParams;
                                emit paramsChanged();

                                emit infoMessage(QStringLiteral("检测到 DEC 长期漂移趋势为：%1（%2），已自动设置 DEC 单向方向为：%3（用于抵消漂移）")
                                                 .arg(driftTrend)
                                                 .arg(rateStr)
                                                 .arg(dirToStr(recommendDir)));
                                emit infoMessage(QStringLiteral("DEC 漂移判定细节：samples=%1 window=%2s %3 pixScale=%4 arcsec/px")
                                                 .arg(static_cast<int>(m_decDriftSamples.size()))
                                                 .arg(m_params.driftDetectDurationMs / 1000.0, 0, 'f', 1)
                                                 .arg(rateDbg)
                                                 .arg(m_params.pixelScaleArcsecPerPixel, 0, 'f', 6));
                            }
                        }
                        else
                        {
                            // 未发现明显漂移：保持当前设置（如果当前是双向，则保持双向；如果是单向，则保持单向）
                            emit infoMessage(QStringLiteral("DEC 漂移检测完成：未发现明显长期漂移（|slope|<%1 px/s），保持当前 DEC 方向设置。")
                                             .arg(m_params.driftDetectMinAbsSlopePxPerSec, 0, 'f', 3));
                            if (std::isfinite(slopePxPerSec))
                            {
                                emit infoMessage(QStringLiteral("DEC 漂移判定细节：samples=%1 window=%2s slope=%3 px/s")
                                                 .arg(static_cast<int>(m_decDriftSamples.size()))
                                                 .arg(m_params.driftDetectDurationMs / 1000.0, 0, 'f', 1)
                                                 .arg(slopePxPerSec, 0, 'f', 6));
                            }
                            // 如果当前是双向，需要锁定为单向（仅在 AUTO 模式下，根据当前误差方向决定）
                            if (m_params.autoDecGuideDir && m_params.allowedDecDirs.size() > 1)
                            {
                                const guiding::GuideDir currentTrend = (out.decErrPx > 0.0) ? guiding::GuideDir::South : guiding::GuideDir::North;
                                auto newParams = m_params;
                                newParams.allowedDecDirs.clear();
                                newParams.allowedDecDirs.insert(currentTrend);
                                m_params = newParams;
                                emit paramsChanged();
                                emit infoMessage(QStringLiteral("DEC 已锁定为单向：%1（根据当前误差方向）").arg(dirToStr(currentTrend)));
                            }
                        }
                    }
                }

                // ===== 关键修复：即使本帧发出了 RA 脉冲，也要检测 DEC 是否“需要但被门控” =====
                // 旧逻辑只在 out.pulse 为空时才会进入门控自纠正；若 RA 一直占用脉冲且 DEC 被门控，
                // 则 DEC 会长期得不到纠正，看起来像“DEC 明显偏了但完全不发脉冲”。
                if (m_params.autoDecGuideDir && decision.decNeed && !decision.decAllowed)
                {
                    m_decGatedCount++;
                    if (m_decGatedCount >= AUTO_ADJUST_DEC_THRESHOLD)
                    {
                        auto newParams = m_params;
                        newParams.allowedDecDirs.clear();
                        newParams.allowedDecDirs.insert(decision.desiredDecDir);
                        m_params = newParams;
                        emit paramsChanged();
                        emit infoMessage(QStringLiteral("自动调整 DEC 单向方向：%1（检测到 DEC 需要纠正但被门控，已连续%2帧）")
                                             .arg(dirToStr(decision.desiredDecDir))
                                             .arg(AUTO_ADJUST_DEC_THRESHOLD));
                        m_decGatedCount = 0;
                    }
                }
                else
                {
                    // 一旦 DEC 不再被门控（或 AUTO 关闭），就清零计数，避免误触发
                    m_decGatedCount = 0;
                }

                // ===== Rolling RMS（室外诊断关键）=====
                m_rms.push(out.raErrPx, out.decErrPx);
                m_rmsEmitCounter++;
                if (m_rms.ready() && (m_rmsEmitCounter % std::max(1, m_params.rmsEmitEveryFrames) == 0))
                {
                    const double raR = m_rms.raRms();
                    const double decR = m_rms.decRms();
                    const double totR = m_rms.totalRms();
                    if (m_params.pixelScaleArcsecPerPixel > 0.0)
                    {
                        const double raA = raR * m_params.pixelScaleArcsecPerPixel;
                        const double decA = decR * m_params.pixelScaleArcsecPerPixel;
                        const double totA = totR * m_params.pixelScaleArcsecPerPixel;
                        emit infoMessage(QStringLiteral("RMS[%1]: ra=%2px(%3\") dec=%4px(%5\") tot=%6px(%7\")")
                                             .arg((int)m_rms.ra2.size())
                                             .arg(raR, 0, 'f', 3).arg(raA, 0, 'f', 2)
                                             .arg(decR, 0, 'f', 3).arg(decA, 0, 'f', 2)
                                             .arg(totR, 0, 'f', 3).arg(totA, 0, 'f', 2));
                    }
                    else
                    {
                        emit infoMessage(QStringLiteral("RMS[%1]: ra=%2px dec=%3px tot=%4px")
                                             .arg((int)m_rms.ra2.size())
                                             .arg(raR, 0, 'f', 3)
                                             .arg(decR, 0, 'f', 3)
                                             .arg(totR, 0, 'f', 3));
                    }
                }

                // ===== 生成本帧要发送的脉冲序列（可选：同一周期 RA+DEC 各一次）=====
                QVector<guiding::PulseCommand> basePulses;
                basePulses.reserve(2);

                auto pushIfValid = [&](guiding::GuideDir dir, int ms) {
                    const int d = std::max(0, ms);
                    if (d <= 0)
                        return;
                    guiding::PulseCommand c;
                    c.dir = dir;
                    c.durationMs = d;
                    basePulses.push_back(c);
                };

                // Global per-axis step limiting (applies even when RA hysteresis is off).
                auto stepLimit = [&](int ms, int lastMs) -> int {
                    const int step = std::max(0, m_params.maxPulseStepPerFrameMs);
                    if (step <= 0 || lastMs <= 0) return ms;
                    const int delta = ms - lastMs;
                    if (delta > step) return lastMs + step;
                    if (delta < -step) return std::max(m_params.minPulseMs, lastMs - step);
                    return ms;
                };

                if (m_params.maxPulseStepPerFrameMs > 0)
                {
                    // Apply to both axes
                    if (decision.clampedRaMs > 0)
                        decision.clampedRaMs = stepLimit(decision.clampedRaMs, m_lastRaPulseMs);
                    if (decision.clampedDecMs > 0)
                        decision.clampedDecMs = stepLimit(decision.clampedDecMs, m_lastDecPulseMs);
                    // If we changed clamped values, keep decision.pulse coherent (when controller picked a single axis)
                    if (decision.pulse.has_value())
                    {
                        if (decision.pulse->dir == guiding::GuideDir::East || decision.pulse->dir == guiding::GuideDir::West)
                            decision.pulse->durationMs = decision.clampedRaMs;
                        else if (decision.pulse->dir == guiding::GuideDir::North || decision.pulse->dir == guiding::GuideDir::South)
                            decision.pulse->durationMs = decision.clampedDecMs;
                    }
                }

                if (m_params.enableMultiAxisPulses && decision.raAllowed && decision.decAllowed)
                {
                    // RA 优先（更常见的周期误差主轴），DEC 紧随其后
                    pushIfValid(decision.desiredRaDir, decision.clampedRaMs);
                    pushIfValid(decision.desiredDecDir, decision.clampedDecMs);
                }
                else if (decision.pulse.has_value())
                {
                    pushIfValid(decision.pulse->dir, decision.pulse->durationMs);
                }

                // Emergency Stage-1: if we didn't schedule any DEC correction due to gating, inject one.
                if (emergencyDecPulse.has_value())
                {
                    bool hasDecAlready = false;
                    for (const auto& p : basePulses)
                    {
                        if (p.dir == guiding::GuideDir::North || p.dir == guiding::GuideDir::South)
                        {
                            hasDecAlready = true;
                            break;
                        }
                    }
                    if (!hasDecAlready)
                        basePulses.push_back(*emergencyDecPulse);
                }

                // Drift detection decontamination: suppress pulses on the axis being measured.
                QVector<guiding::PulseCommand> pulsesToSend;
                pulsesToSend.reserve(basePulses.size() + 1);
                for (const auto& p : basePulses)
                {
                    const bool isDecPulse = (p.dir == guiding::GuideDir::North || p.dir == guiding::GuideDir::South);
                    const bool isRaPulse  = (p.dir == guiding::GuideDir::East  || p.dir == guiding::GuideDir::West);
                    if ((isRaPulse && m_raDriftDetectActive) || (isDecPulse && m_decDriftDetectActive))
                        continue;
                    pulsesToSend.push_back(p);
                }

                // Insert DEC backlash compensation pulse when DEC direction reverses.
                // Note: compensation is only meaningful for DEC pulses.
                if (!pulsesToSend.isEmpty())
                {
                    QVector<guiding::PulseCommand> expanded;
                    expanded.reserve(pulsesToSend.size());

                    for (const auto& cmd : pulsesToSend)
                    {
                        const bool isDecPulse = (cmd.dir == guiding::GuideDir::North || cmd.dir == guiding::GuideDir::South);
                        if (isDecPulse
                            && m_params.enableDecBacklashCompensation
                            && m_decBacklashMsRuntime > 0
                            && m_lastDecPulseDir.has_value()
                            && *m_lastDecPulseDir != cmd.dir)
                        {
                            const int compMs = std::min(m_params.maxPulseMs, std::max(0, m_decBacklashMsRuntime));
                            guiding::PulseCommand merged = cmd;
                            merged.durationMs = std::min(m_params.maxPulseMs,
                                                         std::max(0, merged.durationMs) + std::max(0, compMs));

                            emit infoMessage(QStringLiteral("DEC 方向反转：插入回差补偿 %1ms（base=%2ms runtime=%3ms）")
                                                 .arg(compMs)
                                                 .arg(m_decBacklashMsBase)
                                                 .arg(m_decBacklashMsRuntime));

                            // 启动自适应观测窗口
                            if (m_params.enableDecBacklashAdaptive)
                            {
                                m_decBacklashAdaptActive = true;
                                m_decBacklashAdaptFramesLeft = std::max(1, m_params.decBacklashAdaptiveWindowFrames);
                                m_decBacklashAdaptStartAbsErrPx = std::abs(out.decErrPx);
                                m_decBacklashAdaptMinAbsErrPx = m_decBacklashAdaptStartAbsErrPx;
                                m_decBacklashAdaptStartSignedErrPx = out.decErrPx;
                            }

                            expanded.push_back(merged);
                            // 更新 lastDecPulseDir（以“计划发出”为准）
                            m_lastDecPulseDir = merged.dir;
                            continue;
                        }

                        expanded.push_back(cmd);
                        // 更新 lastDecPulseDir（以“计划发出”为准）
                        if (isDecPulse)
                            m_lastDecPulseDir = cmd.dir;
                    }

                    pulsesToSend = expanded;
                }

                if (!pulsesToSend.isEmpty())
                {
                    // Schedule pulses sequentially, then delay next exposure until all pulses finish + settle.
                    for (const auto& cmd : pulsesToSend)
                    {
                        emit requestPulse(cmd);
                        // update per-axis last-pulse magnitude for step limiting
                        if (cmd.dir == guiding::GuideDir::East || cmd.dir == guiding::GuideDir::West)
                            m_lastRaPulseMs = std::max(0, cmd.durationMs);
                        else if (cmd.dir == guiding::GuideDir::North || cmd.dir == guiding::GuideDir::South)
                            m_lastDecPulseMs = std::max(0, cmd.durationMs);
                    }
                    for (const auto& cmd : pulsesToSend)
                        emit guidePulseIssued(cmd, out.raErrPx, out.decErrPx);

                    // ===== pulse effectiveness check =====
                    if (m_params.enablePulseEffectCheck
                        && (std::max(std::abs(out.raErrPx), std::abs(out.decErrPx)) >= m_params.pulseEffectMinStartAbsErrPx))
                    {
                        m_pulseEffActive = true;
                        m_pulseEffFramesLeft = std::max(1, m_params.pulseEffectWindowFrames);
                        m_pulseEffStartAbsRa = std::abs(out.raErrPx);
                        m_pulseEffStartAbsDec = std::abs(out.decErrPx);
                        m_pulseEffBestAbsRa = m_pulseEffStartAbsRa;
                        m_pulseEffBestAbsDec = m_pulseEffStartAbsDec;
                    }

                    int totalMs = 0;
                    for (int i = 0; i < pulsesToSend.size(); ++i)
                    {
                        totalMs += std::max(0, pulsesToSend[i].durationMs);
                        if (i != pulsesToSend.size() - 1)
                            totalMs += std::max(0, m_params.interPulseDelayMs);
                    }
                    totalMs += std::max(0, m_params.settleMsAfterPulse);
                    nextExposureDelayMs = std::max(nextExposureDelayMs, totalMs);
                }
                else
                {
                    // 每隔一段时间输出一次"无脉冲"诊断
                    if (m_guidingDiagTimer.elapsed() >= 3000)
                    {
                        m_guidingDiagTimer.restart();
                        QString why;
                        if (!decision.raNeed && !decision.decNeed)
                            why = QStringLiteral("原因：误差在 deadband 内");
                        else
                            why = QStringLiteral("原因：compute 未选择脉冲（轴优先级/限幅等）");

                        emit infoMessage(QStringLiteral("导星中：frame=%1 raErrPx=%2 decErrPx=%3 | %4 | allowedRA=%5 allowedDEC=%6")
                                         .arg(m_guidingFrameCount)
                                         .arg(out.raErrPx, 0, 'f', 3)
                                         .arg(out.decErrPx, 0, 'f', 3)
                                         .arg(why)
                                         .arg(QString("%1/%2")
                                                  .arg(m_params.allowedRaDirs.count(guiding::GuideDir::East) ? "EAST" : "-")
                                                  .arg(m_params.allowedRaDirs.count(guiding::GuideDir::West) ? "WEST" : "-"))
                                         .arg(QString("%1/%2")
                                                  .arg(m_params.allowedDecDirs.count(guiding::GuideDir::North) ? "NORTH" : "-")
                                                  .arg(m_params.allowedDecDirs.count(guiding::GuideDir::South) ? "SOUTH" : "-")));
                    }
                }
#endif
            }
            else
            {
                m_centroidFailCount++;
                // 丢星硬恢复：连续失败太多则回退到 Selecting 重新选星/重新校准
                if (m_centroidFailCount >= std::max(1, m_params.maxConsecutiveCentroidFails))
                {
                    emit infoMessage(QStringLiteral("丢星恢复：连续%1帧质心失败，回退到选星（将重新选星→校准→导星）。")
                                         .arg(m_centroidFailCount));
                    // 清理导星状态并回到 Selecting
                    m_hasLock = false;
                    m_lockPosPx = QPointF(0.0, 0.0);
                    m_lastGuideCentroid = QPointF(0.0, 0.0);
                    m_phd2Calib.reset();
                    m_calibResult = guiding::CalibrationResult{};
                    m_centroidFailCount = 0;
                    setState(guiding::State::Selecting);
                    scheduleNextExposure(0);
                    return;
                }
                if (m_guidingDiagTimer.elapsed() >= 2000 || m_centroidFailCount == 1 || m_centroidFailCount % 5 == 0)
                {
                    // 不要每帧刷屏：2s 或每 5 次输出一次
                    emit infoMessage(QStringLiteral("导星质心跟踪失败：frame=%1 failCount=%2（可能丢星/过曝/云层/搜索窗口过小）。将继续尝试扩大搜索窗口。")
                                     .arg(m_guidingFrameCount)
                                     .arg(m_centroidFailCount));
                }
                // 尝试复位搜索中心到 lock 点，避免越漂越远
                m_lastGuideCentroid = m_lockPosPx;
            }
        }
    }

    // 继续下一帧曝光（若本帧发了导星脉冲，则按 PHD2 风格延后：pulse->settle->next exposure）
    scheduleNextExposure(nextExposureDelayMs);
}


