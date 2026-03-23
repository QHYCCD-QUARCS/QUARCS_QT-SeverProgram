#pragma once

#include "GuiderTypes.h"

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <QPointF>
#include <QVector>
#include <optional>
#include <vector>
#include <deque>

#include "GuidingStarDetector.h"
#include "phd2/Phd2MountCalibration.h"
#include "phd2/Phd2MountGuiding.h"
#include "DecBacklashEstimator.h"

// GuiderCore：内置导星核心状态机
// - 负责：循环曝光控制、接收新帧事件、状态上报、后续选星/校准/导星闭环的调度
// - 不直接依赖 INDI；由 MainWindow 注入回调去执行“拍摄”和“发脉冲”

class GuiderCore : public QObject
{
    Q_OBJECT
public:
    explicit GuiderCore(QObject* parent = nullptr);

    // 仅用于内部导星流程中的漂移检测采样（放在 public 以便 .cpp 的辅助函数使用类型）
    struct DriftSample
    {
        double tSec = 0.0;
        double raErrPx = 0.0;   // RA 误差（像素）
        double decErrPx = 0.0;  // DEC 误差（像素）
    };

    guiding::State state() const { return m_state; }
    guiding::GuidingParams params() const { return m_params; }
    void setParams(const guiding::GuidingParams& p);
    bool isQuickDirectionDetecting() const { return m_quickDirectionDetectActive; }

    // 由 MainWindow 调用：开始/停止循环曝光
    // 注意：真正的 takeExposure 由外部通过 requestExposure 信号完成
    Q_INVOKABLE void startLoop();
    Q_INVOKABLE void stopLoop();

    // 导星流程入口（后续完善：选星→校准→导星）
    Q_INVOKABLE void startGuiding();
    // 强制重新校准：忽略历史校准复用（用于“长按开始导星强制校准”）
    Q_INVOKABLE void startGuidingForceCalibrate();
    Q_INVOKABLE void clearCachedCalibration();
    Q_INVOKABLE void stopGuiding();

    // 手动选星：来自前端画布点击（替代外部 PHD2 的 StarClick）
    // 规则：只允许在 Looping 状态下调用。点击仅“选择星点”，不启动校准/导星；
    // 用户需再点击“开始导星”。
    Q_INVOKABLE void setManualLock(double xPx, double yPx);
    Q_INVOKABLE void clearManualLock();

    // 兼容旧入口：点击即启动校准/导星（当前已不再由前端调用，但保留用于测试/回滚）
    Q_INVOKABLE void startGuidingWithManualLock(double xPx, double yPx);

    // 由 MainWindow 在每次获得导星 FITS 后调用
    Q_INVOKABLE void onNewFrame(const QString& fitsPath);

signals:
    // 让外部执行一次曝光（dpGuider）
    void requestExposure(int exposureMs);

    // 让外部保存导星 FITS 到主相机同规则目录（固定 guiding.fits）
    void requestPersistGuidingFits(const QString& sourceFitsPath);

    // 发出导星脉冲指令（后续导星闭环会 emit）
    void requestPulse(const guiding::PulseCommand& cmd);

    // 状态/日志
    void stateChanged(guiding::State state);
    void errorOccurred(const QString& msg);
    void infoMessage(const QString& msg);
    void paramsChanged();

private:
    void setState(guiding::State s);
    guiding::GuidingParams sanitizeParams(const guiding::GuidingParams& in) const;
    void scheduleNextExposure(int delayMs);
    void beginCalibrationFromLock();
    bool canReuseLastCalibration(QString* reason = nullptr) const;
    bool canReuseStartupSnapshot(QString* reason = nullptr) const;
    void startGuidingFromLock(bool isManualLock);
    void enterGuidingState();

private:
    guiding::State m_state = guiding::State::Idle;
    guiding::GuidingParams m_params{};

    QElapsedTimer m_loopTimer;
    bool m_loopActive = false;

    // 选星/锁星
    bool m_hasLock = false;
    QPointF m_lockPosPx{0.0, 0.0};
    guiding::GuidingStarDetector m_detector{};

    // 校准
    guiding::phd2::MountCalibration m_phd2Calib{};
    guiding::CalibrationResult m_calibResult{};

    // 最近一次成功校准快照：用于“换星/停止后再开始”时尝试复用
    struct CalibrationContext
    {
        int guiderBinning = 1;
        double guideSpeedSidereal = 0.0;
        double imageScaleArcsecPerPixel = 0.0;
    };
    bool m_hasLastCalibration = false;
    guiding::CalibrationResult m_lastCalibration{};
    CalibrationContext m_lastCalibrationCtx{};
    bool m_hasLastBacklash = false;
    int m_lastBacklashMsBase = 0;
    int m_lastBacklashMsRuntime = 0;
    bool m_forceCalibrateNextStart = false;

    // 导星闭环
    guiding::phd2::MountGuiding m_phd2Guiding{};
    QPointF m_lastGuideCentroid{0.0, 0.0};

    // 误差EMA滤波（用于控制，不影响上报的 raw error 曲线）
    bool m_errEmaInit = false;
    double m_raErrEma = 0.0;
    double m_decErrEma = 0.0;

    // ===== DEC 回差测量（导星前）=====
    guiding::DecBacklashEstimator m_decBacklash{};
    bool m_decBacklashMeasureActive = false;
    int m_decBacklashMsBase = 0;     // 测得的回差（ms）
    int m_decBacklashMsRuntime = 0;  // 运行时可调整的回差（ms），后续用于补偿
    std::optional<guiding::GuideDir> m_lastDecPulseDir; // 记录上一次实际发出的 DEC 脉冲方向

    // 自适应：DEC 方向反转后观测误差改善，调整 m_decBacklashMsRuntime
    bool m_decBacklashAdaptActive = false;
    int m_decBacklashAdaptFramesLeft = 0;
    double m_decBacklashAdaptStartAbsErrPx = 0.0;
    double m_decBacklashAdaptMinAbsErrPx = 0.0;
    double m_decBacklashAdaptStartSignedErrPx = 0.0;

    // ===== RA/DEC 漂移自动判定（单向导星辅助）=====
    bool m_raDriftDetectActive = false;
    QElapsedTimer m_raDriftTimer;
    std::vector<DriftSample> m_raDriftSamples;

    bool m_decDriftDetectActive = false;
    QElapsedTimer m_decDriftTimer;
    std::vector<DriftSample> m_decDriftSamples;

    // ===== DEC 单向策略（用户需求的两种情况）=====
    bool m_decUniPolicyActive = false;
    bool m_decUniPolicyDecided = false;
    bool m_decUniLargeMove = false;  // 已判定为“移动幅度大”
    int m_decUniFrames = 0;
    double m_decUniSum = 0.0;
    double m_decUniSumSq = 0.0;

    // ===== Guiding 诊断（防"卡住"难排查）=====
    int m_guidingFrameCount = 0;
    int m_centroidFailCount = 0;
    QElapsedTimer m_guidingDiagTimer;

    // ===== pulse/settle/exposure 调度（避免脉冲与曝光重叠）=====
    quint64 m_schedSeq = 0;

    // ===== 丢星恢复 =====
    bool m_reselectAfterLostStar = false;

    // ===== pulse effectiveness check (detect \"pulse has no effect\") =====
    bool m_pulseEffActive = false;
    int m_pulseEffFramesLeft = 0;
    double m_pulseEffStartAbsRa = 0.0;
    double m_pulseEffStartAbsDec = 0.0;
    double m_pulseEffBestAbsRa = 0.0;
    double m_pulseEffBestAbsDec = 0.0;
    int m_pulseEffFailStreak = 0;

    // ===== Rolling RMS diagnostics =====
    struct RmsWindow
    {
        std::deque<double> ra2;
        std::deque<double> dec2;
        int maxN = 60;

        void reset(int n)
        {
            maxN = std::max(1, n);
            ra2.clear();
            dec2.clear();
        }
        void push(double raErrPx, double decErrPx)
        {
            ra2.push_back(raErrPx * raErrPx);
            dec2.push_back(decErrPx * decErrPx);
            while ((int)ra2.size() > maxN) ra2.pop_front();
            while ((int)dec2.size() > maxN) dec2.pop_front();
        }
        bool ready() const { return !ra2.empty() && ra2.size() == dec2.size(); }
        double raRms() const;
        double decRms() const;
        double totalRms() const;
    };
    RmsWindow m_rms;
    int m_rmsEmitCounter = 0;

    // ===== 智能方向自动调整（避免"被单向门控"持续出现）=====
    int m_raGatedCount = 0;      // RA 被门控的连续次数
    int m_decGatedCount = 0;     // DEC 被门控的连续次数
    static constexpr int AUTO_ADJUST_RA_THRESHOLD = 12;  // RA：更保守，避免频繁翻向
    static constexpr int AUTO_ADJUST_DEC_THRESHOLD = 5;  // DEC：相对积极（但默认仍单向）

    // ===== 两段式应急兜底（解卡/临时放宽门控）=====
    bool m_emergencyStage2Active = false;
    bool m_emergencyHasLastAbs = false;
    double m_emergencyLastAbsDecPx = 0.0;
    int m_emergencyGrowHit = 0;
    bool m_emergencySavedAllowed = false;
    std::set<guiding::GuideDir> m_emergencySavedAllowedDecDirs;

    // ===== RA Hysteresis（滞后）状态 =====
    bool m_raHysInit = false;
    double m_raHysPrevSignedMs = 0.0; // signed ms, West positive, East negative
    int m_lastRaPulseMs = 0;          // last RA pulse magnitude (ms), for step limiting
    int m_lastDecPulseMs = 0;         // last DEC pulse magnitude (ms), for step limiting

    // ===== 快速方向检测（旧逻辑已由 DEC 单向策略替代）=====
    // 保留接口：前端可能仍会调用 isQuickDirectionDetecting() 显示状态
    bool m_quickDirectionDetectActive = false;

signals:
    void calibrationResultChanged(const guiding::CalibrationResult& r);
    void guidePulseIssued(const guiding::PulseCommand& cmd, double raErrPx, double decErrPx);
    // 每帧更新误差（即使没有发脉冲也会发），用于前端连续曲线绘制
    void guideErrorUpdated(double raErrPx, double decErrPx);
    // 选星/锁星：用于前端在导星画面上标记校准/导星的目标星点
    void lockPositionChanged(const QPointF& lockPosPx);
    // 选星信息：用于前端显示"导的是哪一颗星点"（含 SNR/HFD）
    void lockStarSelected(double x, double y, double snr, double hfd);
    // 导星中：每帧更新"当前星点质心"位置（用于前端让框跟随星点）
    void guideStarCentroidChanged(const QPointF& centroidPx);
    // 多星导星：用于前端绘制副星点绿色圆圈（当前实现可能暂不 emit，先保证接口/编译对齐）
    void multiStarSecondaryPointsChanged(const QVector<QPointF>& ptsPx);
    // 快速方向检测状态变化：用于前端显示蓝色UI
    void directionDetectionStateChanged(bool active);
};


