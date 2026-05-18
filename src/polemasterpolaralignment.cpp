#include "polemasterpolaralignment.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QPainter>
#include <QProcess>
#include <QRadialGradient>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <fitsio.h>
#include <opencv2/video/tracking.hpp>
#include <opencv2/calib3d.hpp>

#include "Logger.h"

namespace {
constexpr double kTruePoleRaDeg = 0.0;
constexpr double kNorthPoleDecDeg = 89.9999;
constexpr double kSouthPoleDecDeg = -89.9999;
constexpr double kPolarisRaDeg = 37.95456067;
constexpr double kPolarisDecDeg = 89.26410897;
constexpr double kSimulationArcsecPerPixel = 4.0;
constexpr int kMinPoleMasterStars = 12;
constexpr int kMaxExposureRetries = 3;
constexpr int kSimGuidanceExposureMinMs = 400;
constexpr int kSimGuidanceExposureMaxMs = 5000;
constexpr int kSimGuidanceExposureUpStepMs = 250;
constexpr int kSimGuidanceExposureDownStepMs = 150;
constexpr int kSimGuidanceStarCountLow = 12;
constexpr int kSimGuidanceStarCountHigh = 42;
const QVector<int> kPoleCaptureExposurePlanMs = {500, 1000, 2000, 3000, 5000};
constexpr int kGuidanceBootstrapExposureMs = 100;
constexpr int kGuidanceBootstrapStarCount = 5;
constexpr int kSolveExposureMinMs = 500;
constexpr int kSolveExposureMaxMs = 5000;
constexpr int kSolveExposureStepUpMs = 500;
constexpr int kSolveExposureStepDownMs = 250;

struct FixedPoleStar {
    const char *id = "";
    const char *name = "";
    const char *templateRole = "";
    double raDeg = 0.0;
    double decDeg = 0.0;
    double mag = 99.0;
};

struct FixedStarCandidate {
    int detectedIndex = -1;
    double distancePx = 0.0;
    double distanceArcsec = std::numeric_limits<double>::infinity();
};

struct FixedStarMatchResult {
    QVector<int> matchedDetectedIndex;
    QVector<double> matchedDistancePx;
    QVector<double> matchedDistanceArcsec;
    QVector<bool> matched;
};

bool isFinitePoint(const QPointF &p)
{
    return std::isfinite(p.x()) && std::isfinite(p.y());
}

double normalizeSignedAngleDeg(double deg)
{
    while (deg <= -180.0) deg += 360.0;
    while (deg > 180.0) deg -= 360.0;
    return deg;
}

double angularDistanceArcsec(double ra1Deg, double dec1Deg, double ra2Deg, double dec2Deg)
{
    constexpr double kRadToArcsec = 206264.80624709636;
    const double ra1 = ra1Deg * M_PI / 180.0;
    const double dec1 = dec1Deg * M_PI / 180.0;
    const double ra2 = ra2Deg * M_PI / 180.0;
    const double dec2 = dec2Deg * M_PI / 180.0;
    const double cosD = std::sin(dec1) * std::sin(dec2) +
                        std::cos(dec1) * std::cos(dec2) * std::cos(ra1 - ra2);
    return std::acos(std::clamp(cosD, -1.0, 1.0)) * kRadToArcsec;
}

QJsonObject pointJson(const QPointF &p)
{
    QJsonObject obj;
    obj["x"] = p.x();
    obj["y"] = p.y();
    obj["valid"] = isFinitePoint(p);
    return obj;
}

QVector<FixedPoleStar> fixedPoleStars(bool southernHemisphere)
{
    if (southernHemisphere)
    {
        // 南半球模板星同样按亮星优先，排除仅长曝光下才稳定可见的暗星。
        return {
            {"S1", "Sigma Octantis", "B", 317.191708, -88.956500, 5.45},
            {"S2", "Chi Octantis", "A", 283.698542, -87.605528, 5.29},
            {"S3", "Tau Octantis", "D", 352.014875, -87.482250, 5.50},
            {"S4", "Delta Octantis", "S4", 216.732583, -83.227917, 4.31},
            {"S5", "Nu Octantis", "S5", 323.148875, -77.389500, 3.73}
        };
    }

    // 北半球模板星固定为 5 颗亮星，用于 0.1s 曝光可见性约束。
    // 避免引入仅长曝光可见的暗星，导致模板匹配不稳定。
    return {
        {"N1", "Polaris", "B", kPolarisRaDeg, kPolarisDecDeg, 1.98},
        {"N2", "Kochab", "A", 222.676458, 74.155500, 2.08},
        {"N3", "Pherkad", "D", 230.182125, 71.833972, 3.00},
        {"N4", "Yildun", "N4", 263.053833, 86.586389, 4.35},
        {"N5", "Epsilon UMi", "N5", 251.492083, 82.037250, 4.22}
    };
}

FixedStarMatchResult solveGlobalFixedStarMatch(const QVector<QVector<FixedStarCandidate>> &candidatesByStar,
                                               int detectedStarCount,
                                               int starCount,
                                               double unmatchedPenaltyCost)
{
    FixedStarMatchResult result;
    result.matchedDetectedIndex = QVector<int>(starCount, -1);
    result.matchedDistancePx = QVector<double>(starCount, -1.0);
    result.matchedDistanceArcsec = QVector<double>(starCount, -1.0);
    result.matched = QVector<bool>(starCount, false);
    if (starCount <= 0 || detectedStarCount <= 0)
        return result;

    QVector<int> order(starCount);
    for (int i = 0; i < starCount; ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return candidatesByStar[a].size() < candidatesByStar[b].size();
    });

    QVector<int> currentDetected(starCount, -1);
    QVector<double> currentDistancePx(starCount, -1.0);
    QVector<double> currentDistanceArcsec(starCount, -1.0);
    QVector<bool> usedDetected(detectedStarCount, false);
    double bestCost = std::numeric_limits<double>::infinity();
    double currentCost = 0.0;

    std::function<void(int)> dfs = [&](int depth) {
        if (currentCost >= bestCost)
            return;
        if (depth >= order.size())
        {
            bestCost = currentCost;
            result.matchedDetectedIndex = currentDetected;
            result.matchedDistancePx = currentDistancePx;
            result.matchedDistanceArcsec = currentDistanceArcsec;
            for (int i = 0; i < starCount; ++i)
                result.matched[i] = currentDetected[i] >= 0;
            return;
        }

        const int starIdx = order[depth];
        const QVector<FixedStarCandidate> &candidates = candidatesByStar[starIdx];

        currentCost += unmatchedPenaltyCost;
        dfs(depth + 1);
        currentCost -= unmatchedPenaltyCost;

        for (const FixedStarCandidate &candidate : candidates)
        {
            if (candidate.detectedIndex < 0 || candidate.detectedIndex >= detectedStarCount)
                continue;
            if (usedDetected[candidate.detectedIndex])
                continue;

            usedDetected[candidate.detectedIndex] = true;
            currentDetected[starIdx] = candidate.detectedIndex;
            currentDistancePx[starIdx] = candidate.distancePx;
            currentDistanceArcsec[starIdx] = candidate.distanceArcsec;
            currentCost += candidate.distanceArcsec;

            dfs(depth + 1);

            currentCost -= candidate.distanceArcsec;
            currentDetected[starIdx] = -1;
            currentDistancePx[starIdx] = -1.0;
            currentDistanceArcsec[starIdx] = -1.0;
            usedDetected[candidate.detectedIndex] = false;
        }
    };

    dfs(0);
    return result;
}

bool buildGray8FromFits(const QString &fitsPath, cv::Mat &gray8, int &imageW, int &imageH)
{
    imageW = 0;
    imageH = 0;
    gray8.release();

    cv::Mat image;
    const int status = Tools::readFits(fitsPath.toLocal8Bit().constData(), image);
    if (status != 0 || image.empty())
        return false;

    cv::Mat gray;
    if (image.channels() == 3)
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    else if (image.channels() == 4)
        cv::cvtColor(image, gray, cv::COLOR_BGRA2GRAY);
    else
        gray = image;

    if (gray.depth() == CV_8U)
    {
        gray8 = gray.clone();
    }
    else
    {
        cv::Mat image16;
        if (gray.type() == CV_16UC1)
            image16 = gray;
        else
            gray.convertTo(image16, CV_16UC1);

        uint16_t black = 0;
        uint16_t white = 65535;
        Tools::GetAutoStretch(image16, 0, black, white);

        double minVal = 0.0;
        double maxVal = 0.0;
        cv::minMaxLoc(image16, &minVal, &maxVal);
        if (white <= black || (maxVal > 0.0 && white > std::min(65535.0, maxVal + 4096.0)))
        {
            black = static_cast<uint16_t>(std::max(0.0, std::min(65535.0, minVal)));
            white = static_cast<uint16_t>(std::max<double>(black + 1, std::min(65535.0, maxVal)));
        }

        gray8 = cv::Mat(image16.rows, image16.cols, CV_8UC1);
        Tools::Bit16To8_Stretch(image16, gray8, black, white);
    }

    imageW = gray8.cols;
    imageH = gray8.rows;
    return !gray8.empty();
}

bool buildPreviewJpgFromFits(const QString &fitsPath, const QString &jpgPath, int &imageW, int &imageH)
{
    imageW = 0;
    imageH = 0;

    cv::Mat image;
    const int status = Tools::readFits(fitsPath.toLocal8Bit().constData(), image);
    if (status != 0 || image.empty())
        return false;

    cv::Mat preview8;
    if (image.depth() == CV_8U)
    {
        preview8 = image;
    }
    else
    {
        cv::Mat image16;
        if (image.type() == CV_16UC1)
            image16 = image;
        else
        {
            cv::Mat gray;
            if (image.channels() == 3)
                cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
            else if (image.channels() == 4)
                cv::cvtColor(image, gray, cv::COLOR_BGRA2GRAY);
            else
                gray = image;

            if (gray.type() == CV_16UC1)
                image16 = gray;
            else
                gray.convertTo(image16, CV_16UC1);
        }

        uint16_t black = 0;
        uint16_t white = 65535;
        Tools::GetAutoStretch(image16, 0, black, white);

        double minVal = 0.0;
        double maxVal = 0.0;
        cv::minMaxLoc(image16, &minVal, &maxVal);
        if (white <= black || (maxVal > 0.0 && white > std::min(65535.0, maxVal + 4096.0)))
        {
            black = static_cast<uint16_t>(std::max(0.0, std::min(65535.0, minVal)));
            white = static_cast<uint16_t>(std::max<double>(black + 1, std::min(65535.0, maxVal)));
        }

        preview8 = cv::Mat(image16.rows, image16.cols, CV_8UC(image16.channels()));
        Tools::Bit16To8_Stretch(image16, preview8, black, white);
    }

    if (preview8.empty())
        return false;

    const QFileInfo jpgInfo(jpgPath);
    QDir outDir = jpgInfo.dir();
    if (!outDir.exists() && !outDir.mkpath("."))
        return false;

    if (!cv::imwrite(jpgPath.toStdString(), preview8))
        return false;

    imageW = preview8.cols;
    imageH = preview8.rows;
    return true;
}

const QVector<QPointF> &simulationPolePath()
{
    static const QVector<QPointF> path = {
        {668.0, 505.0},
        {648.0, 493.0},
        {626.0, 501.0},
        {606.0, 474.0},
        {587.0, 482.0},
        {574.0, 456.0},
        {552.0, 463.0},
        {545.0, 440.0},
        {532.0, 449.0},
        {526.0, 427.0},
        {516.0, 433.0},
        {522.0, 414.0},
        {511.0, 420.0},
        {516.0, 404.0},
        {507.0, 410.0},
        {514.0, 397.0},
        {506.0, 401.0},
        {512.0, 392.0},
        {507.0, 395.0},
        {512.8, 389.8},
        {508.2, 392.0},
        {512.4, 388.2},
        {509.2, 390.0},
        {512.2, 386.9},
        {510.0, 388.2},
        {512.0, 386.0},
        {510.8, 386.8},
        {512.0, 385.3},
        {511.2, 385.7},
        {512.5, 384.9},
        {511.8, 384.6},
        {512.3, 384.4},
        {512.1, 384.2},
        {512.0, 384.1}
    };
    return path;
}

bool isLikelyOverexposedFits(const QString &fitsPath)
{
    cv::Mat image;
    const int status = Tools::readFits(fitsPath.toLocal8Bit().constData(), image);
    if (status != 0 || image.empty())
        return false;

    cv::Mat gray;
    if (image.channels() == 3)
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    else if (image.channels() == 4)
        cv::cvtColor(image, gray, cv::COLOR_BGRA2GRAY);
    else
        gray = image;

    cv::Mat gray64;
    gray.convertTo(gray64, CV_64F);
    double minVal = 0.0;
    double maxVal = 0.0;
    cv::minMaxLoc(gray64, &minVal, &maxVal);

    // FITS 16bit/float saturation heuristic: max close to full scale with notable saturated area.
    const double satThreshold = maxVal > 4096.0 ? maxVal * 0.985 : 252.0;
    cv::Mat mask = (gray64 >= satThreshold);
    const int satPixels = cv::countNonZero(mask);
    const int totalPixels = std::max(1, gray64.rows * gray64.cols);
    const double satRatio = static_cast<double>(satPixels) / static_cast<double>(totalPixels);
    return satRatio > 0.0015;
}
}

PoleMasterPolarAlignment::PoleMasterPolarAlignment(MyClient *indiServer,
                                                   INDI::BaseDevice *dpMount,
                                                   INDI::BaseDevice *dpPoleCamera,
                                                   bool useSdkCaptureSource,
                                                   bool simulationMode,
                                                   const QString &simulationImageRootPath,
                                                   QObject *parent)
    : QObject(parent)
    , indiServer(indiServer)
    , dpMount(dpMount)
    , dpPoleCamera(dpPoleCamera)
    , useSdkCaptureSource(useSdkCaptureSource)
    , simulationMode(simulationMode)
    , simulationImageRootPath(simulationImageRootPath)
{
    stateTimer.setSingleShot(true);
    connect(&stateTimer, &QTimer::timeout, this, &PoleMasterPolarAlignment::onStateTimerTimeout);
}

PoleMasterPolarAlignment::~PoleMasterPolarAlignment()
{
    stop();
}

void PoleMasterPolarAlignment::setConfig(const PoleMasterAlignmentConfig &newConfig)
{
    config = newConfig;
}

bool PoleMasterPolarAlignment::start()
{
    if (running) return false;
    if (!simulationMode && (indiServer == nullptr || dpMount == nullptr))
    {
        Logger::Log("PoleMasterPolarAlignment: mount or INDI client unavailable", LogLevel::ERROR, DeviceType::MAIN);
        setState(PoleMasterAlignmentState::FAILED, "电子极轴镜校准启动失败：赤道仪不可用", 0);
        return false;
    }
    if (!simulationMode && !useSdkCaptureSource && dpPoleCamera == nullptr)
    {
        Logger::Log("PoleMasterPolarAlignment: pole camera unavailable", LogLevel::ERROR, DeviceType::MAIN);
        setState(PoleMasterAlignmentState::FAILED, "电子极轴镜校准启动失败：极轴镜不可用", 0);
        return false;
    }
    if (config.focalLength <= 0 || config.cameraWidth <= 0.0 || config.cameraHeight <= 0.0)
    {
        Logger::Log("PoleMasterPolarAlignment: invalid camera configuration", LogLevel::ERROR, DeviceType::MAIN);
        setState(PoleMasterAlignmentState::FAILED, "电子极轴镜参数无效", 0);
        return false;
    }

    running = true;
    captureEnded = false;
    latestCapturePathFromHost.clear();
    lastCapturedImage.clear();
    calibrationFrames.clear();
    hasGuidingAnchorFrame = false;
    guidingAnchorFrame = SolveFrame();
    axisCenterPx = QPointF(-1.0, -1.0);
    axisRadiusPx = 0.0;
    axisResidualPx = 0.0;
    stableFrames = 0;
    actualRaRotations.clear();
    hasStartMountPosition = captureMountPosition(startRaDeg, startDecDeg);
    lastRaDeg = startRaDeg;
    lastDecDeg = startDecDeg;
    guidingTrackingInitialized = false;
    guidingLockStarPx = QPointF(-1.0, -1.0);
    guidingPoleOffsetPx = QPointF(0.0, 0.0);
    guidingLockConfidence = 0.0;
    guidingLostFrames = 0;
    adaptiveGuidanceExposureMs = kGuidanceBootstrapExposureMs;
    adaptiveSolveExposureMs = 0;
    guidanceExposureBootstrapped = false;
    guidanceExposureSeededFromSolve = false;
    previousGuideGray8.release();
    previousSelectedTrackStars.clear();
    guideGrayReady = false;
    guidanceImageW = 0;
    guidanceImageH = 0;
    guidancePixelScaleArcsecPerPixel = 1.0;
    simulationFrameIndex = 0;
    simulationVirtualRaDeg = 0.0;
    simulationVirtualDecDeg = truePoleDecDeg();
    simulationGuidingOffsetPx = simulationPolePath().first() - currentSimulationAxisPx();
    simulationGuidingFrameCount = 0;
    setState(PoleMasterAlignmentState::INITIALIZING, "电子极轴镜校准初始化", 5);
    return true;
}

void PoleMasterPolarAlignment::stop()
{
    if (!running && currentState == PoleMasterAlignmentState::IDLE) return;
    running = false;
    stateTimer.stop();
    setState(PoleMasterAlignmentState::IDLE, "电子极轴镜校准已停止", 0);
}

bool PoleMasterPolarAlignment::isRunning() const
{
    return running;
}

void PoleMasterPolarAlignment::setCaptureEnd(bool isEnd)
{
    captureEnded = isEnd;
}

void PoleMasterPolarAlignment::setCapturedImagePath(const QString &fitsPath)
{
    latestCapturePathFromHost = fitsPath.trimmed();
}

void PoleMasterPolarAlignment::onStateTimerTimeout()
{
    processCurrentState();
}

void PoleMasterPolarAlignment::setState(PoleMasterAlignmentState newState, const QString &message, int progress)
{
    currentState = newState;
    Logger::Log("PoleMasterPolarAlignment: state=" + std::to_string(static_cast<int>(newState)) +
                    " message=" + message.toStdString() +
                    " progress=" + std::to_string(progress),
                LogLevel::INFO,
                DeviceType::MAIN);

    const bool stillRunning = running &&
        newState != PoleMasterAlignmentState::IDLE &&
        newState != PoleMasterAlignmentState::COMPLETED &&
        newState != PoleMasterAlignmentState::FAILED;
    emit stateChanged(newState, message, progress, stillRunning);

    if (newState == PoleMasterAlignmentState::COMPLETED || newState == PoleMasterAlignmentState::FAILED)
    {
        running = false;
        emit stateChanged(newState, message, progress, false);
        return;
    }

    if (running) stateTimer.start(50);
}

void PoleMasterPolarAlignment::processCurrentState()
{
    if (!running) return;

    auto captureFailureMessage = [this](const QString &fallback) -> QString
    {
        if (lastCaptureQualityPoor)
        {
            if (!lastCaptureFailureDetail.isEmpty())
                return "环境质量较差，极轴镜星场解析失败：" + lastCaptureFailureDetail;
            return "环境质量较差，极轴镜星场解析失败，请检查云层/雾霾或提升曝光后重试";
        }
        return fallback;
    };

    switch (currentState)
    {
    case PoleMasterAlignmentState::INITIALIZING:
        setState(PoleMasterAlignmentState::FIRST_CAPTURE, "拍摄并解析第一帧星场", 10);
        break;
    case PoleMasterAlignmentState::FIRST_CAPTURE:
    {
        SolveFrame frame;
        if (!captureAndSolveWithQuality(frame, "field-confirmation"))
        {
            setState(PoleMasterAlignmentState::FAILED, captureFailureMessage("第一帧解析失败"), 10);
            break;
        }
        calibrationFrames.append(frame);
        setState(PoleMasterAlignmentState::MOVING_RA_FIRST, "第一次旋转赤经轴", 25);
        break;
    }
    case PoleMasterAlignmentState::MOVING_RA_FIRST:
        if (!moveRaAxis(config.raRotationAngle) || !waitForMountIdle())
            setState(PoleMasterAlignmentState::FAILED, "第一次赤经轴旋转失败", 25);
        else
            setState(PoleMasterAlignmentState::SECOND_CAPTURE, "拍摄并解析第二帧星场", 38);
        break;
    case PoleMasterAlignmentState::SECOND_CAPTURE:
    {
        SolveFrame frame;
        if (!captureAndSolveWithQuality(frame, "ra-rotation-1"))
        {
            setState(PoleMasterAlignmentState::FAILED, captureFailureMessage("第二帧解析失败"), 38);
            break;
        }
        calibrationFrames.append(frame);
        setState(PoleMasterAlignmentState::MOVING_RA_SECOND, "第二次旋转赤经轴", 50);
        break;
    }
    case PoleMasterAlignmentState::MOVING_RA_SECOND:
        if (!moveRaAxis(config.raRotationAngle) || !waitForMountIdle())
            setState(PoleMasterAlignmentState::FAILED, "第二次赤经轴旋转失败", 50);
        else
            setState(PoleMasterAlignmentState::THIRD_CAPTURE, "拍摄并解析第三帧星场", 62);
        break;
    case PoleMasterAlignmentState::THIRD_CAPTURE:
    {
        SolveFrame frame;
        if (!captureAndSolveWithQuality(frame, "ra-rotation-2"))
        {
            setState(PoleMasterAlignmentState::FAILED, captureFailureMessage("第三帧解析失败"), 62);
            break;
        }
        calibrationFrames.append(frame);
        setState(PoleMasterAlignmentState::CALCULATING_AXIS, "标定赤经轴旋转中心", 74);
        break;
    }
    case PoleMasterAlignmentState::CALCULATING_AXIS:
        if (!fitAxisCenter())
            setState(PoleMasterAlignmentState::FAILED, "赤经轴圆心拟合失败", 74);
        else
        {
            returnToStartPosition();
            SolveFrame homeAnchor;
            if (!captureAndSolveWithQuality(homeAnchor, "guiding-anchor"))
            {
                setState(PoleMasterAlignmentState::FAILED, captureFailureMessage("返回 Home 后基准帧解析失败"), 82);
                break;
            }
            hasGuidingAnchorFrame = true;
            guidingAnchorFrame = homeAnchor;
            if (simulationMode && isFinitePoint(axisCenterPx) && isFinitePoint(homeAnchor.truePolePx))
            {
                // Keep simulation visually continuous across phase switch:
                // guiding starts from the solved home-anchor pole offset
                // instead of jumping back to the initial scripted offset.
                simulationGuidingOffsetPx = homeAnchor.truePolePx - axisCenterPx;
                simulationGuidingFrameCount = 0;
            }
            setState(PoleMasterAlignmentState::GUIDING_ADJUSTMENT, "已返回 Home，进入极轴镜实时调整", 82);
        }
        break;
    case PoleMasterAlignmentState::GUIDING_ADJUSTMENT:
    {
        if (!guidingTrackingInitialized)
        {
            if (!initGuidingTrackingFromLastSolve())
            {
                setState(PoleMasterAlignmentState::FAILED, "实时引导初始化失败：无法建立跟踪锚点", 82);
                break;
            }
        }
        SolveFrame frame;
        if (captureAndTrackGuideFrame(frame))
        {
            emitCurrentGuide(frame);
        }
        else
        {
            Logger::Log("PoleMasterPolarAlignment: guidance tracking failed, retrying", LogLevel::WARNING, DeviceType::MAIN);
        }
        if (running) stateTimer.start(100);
        break;
    }
    default:
        break;
    }
}

bool PoleMasterPolarAlignment::captureAndSolve(SolveFrame &frame)
{
    const int exposureMs = currentState == PoleMasterAlignmentState::GUIDING_ADJUSTMENT
                               ? config.guidanceExposureTime
                               : config.defaultExposureTime;
    if (!captureImage(exposureMs))
    {
        return false;
    }
    if (!waitForCaptureComplete()) return false;
    if (!solveImage(lastCapturedImage)) return false;
    if (!readSolveFrame(lastCapturedImage, exposureMs, frame)) return false;
    enrichFrameDiagnostics(frame);
    emitOverlay(currentState == PoleMasterAlignmentState::GUIDING_ADJUSTMENT ? "guiding" : "capture", &frame);
    return true;
}

bool PoleMasterPolarAlignment::captureAndSolveWithQuality(SolveFrame &frame, const QString &phase)
{
    lastCaptureQualityPoor = false;
    lastCaptureFailureDetail.clear();
    QStringList warnings;
    const QString expectedFitsPath = QStringLiteral("/dev/shm/polecamera.fits");
    QStringList failureReasons;
    if (adaptiveSolveExposureMs <= 0)
    {
        const int seedExposure = config.defaultExposureTime > 0 ? config.defaultExposureTime : kPoleCaptureExposurePlanMs.first();
        adaptiveSolveExposureMs = std::clamp(seedExposure, kSolveExposureMinMs, kSolveExposureMaxMs);
    }

    const bool parseOnlyPhase = (phase == QStringLiteral("field-confirmation"));

    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const int exposureMs = std::clamp(adaptiveSolveExposureMs, kSolveExposureMinMs, kSolveExposureMaxMs);
        const QDateTime beforeCaptureMtime = QFileInfo(expectedFitsPath).lastModified();
        if (!captureImage(exposureMs))
        {
            lastCaptureFailureDetail = "触发拍摄失败";
            return false;
        }
        if (!waitForCaptureComplete())
        {
            lastCaptureFailureDetail = "拍摄超时或未获得有效FITS";
            return false;
        }
        const QFileInfo capturedInfo(lastCapturedImage);
        if (capturedInfo.exists() &&
            capturedInfo.absoluteFilePath() == QFileInfo(expectedFitsPath).absoluteFilePath() &&
            beforeCaptureMtime.isValid() &&
            capturedInfo.lastModified().isValid() &&
            capturedInfo.lastModified() <= beforeCaptureMtime)
        {
            warnings << QString("拍摄帧时间戳未更新，判定为旧帧，自动重拍");
            failureReasons << QString("旧帧（%1ms）").arg(exposureMs);
            Logger::Log("PoleMasterPolarAlignment: captured FITS timestamp unchanged, force recapture. path=" +
                            lastCapturedImage.toStdString() +
                            " attempt=" + std::to_string(attempt + 1) +
                            "/8",
                        LogLevel::WARNING,
                        DeviceType::MAIN);
            emitOverlay(phase, nullptr, warnings);
            continue;
        }
        const bool likelyOverexposed = isLikelyOverexposedFits(lastCapturedImage);
        if (!solveImage(lastCapturedImage))
        {
            warnings << QString("曝光%1ms解析失败，继续重试").arg(exposureMs);
            failureReasons << QString("板解失败（%1ms）").arg(exposureMs);
            adaptiveSolveExposureMs = likelyOverexposed
                                          ? std::max(kSolveExposureMinMs, exposureMs - kSolveExposureStepDownMs)
                                          : std::min(kSolveExposureMaxMs, exposureMs + kSolveExposureStepUpMs);
            emitOverlay(phase, nullptr, warnings);
            continue;
        }
        if (!readSolveFrame(lastCapturedImage, exposureMs, frame))
        {
            warnings << QString("曝光%1ms读取解算结果失败，继续重试").arg(exposureMs);
            failureReasons << QString("读取解算结果失败（%1ms）").arg(exposureMs);
            adaptiveSolveExposureMs = likelyOverexposed
                                          ? std::max(kSolveExposureMinMs, exposureMs - kSolveExposureStepDownMs)
                                          : std::min(kSolveExposureMaxMs, exposureMs + kSolveExposureStepUpMs);
            emitOverlay(phase, nullptr, warnings);
            continue;
        }
        enrichFrameDiagnostics(frame);

        // 星场确认阶段以板解成功为准：成功后立即锁定当前曝光，
        // 不再附加星点数量门槛，避免“能解但因星点门槛被拒绝”的误判。
        if (parseOnlyPhase)
        {
            adaptiveSolveExposureMs = exposureMs;
            emitOverlay(phase, &frame, warnings);
            return true;
        }

        if (frame.detectedStars.size() >= kMinPoleMasterStars)
        {
            adaptiveSolveExposureMs = exposureMs;
            emitOverlay(phase, &frame, warnings);
            return true;
        }

        warnings << QString("星点数量不足：%1/%2，自动增加曝光重拍").arg(frame.detectedStars.size()).arg(kMinPoleMasterStars);
        failureReasons << QString("星点不足%1/%2（%3ms）")
                              .arg(frame.detectedStars.size())
                              .arg(kMinPoleMasterStars)
                              .arg(exposureMs);
        adaptiveSolveExposureMs = likelyOverexposed
                                      ? std::max(kSolveExposureMinMs, exposureMs - kSolveExposureStepDownMs)
                                      : std::min(kSolveExposureMaxMs, exposureMs + kSolveExposureStepUpMs);
        emitOverlay(phase, &frame, warnings);
    }
    lastCaptureQualityPoor = true;
    lastCaptureFailureDetail = failureReasons.isEmpty()
                                   ? QString("当前曝光自适应重试后仍未得到可用解析结果")
                                   : QString("当前曝光自适应重试失败（%1）").arg(failureReasons.join("，"));
    warnings << lastCaptureFailureDetail;
    emitOverlay(phase, nullptr, warnings);
    return false;
}

bool PoleMasterPolarAlignment::captureImage(int exposureMs)
{
    QElapsedTimer stageTimer;
    stageTimer.start();
    if (simulationMode)
        return captureImageSimulated(exposureMs);

    captureEnded = false;
    latestCapturePathFromHost.clear();
    lastCapturedImage.clear();

    Logger::Log("PoleMasterPolarAlignment: request capture exposureMs=" + std::to_string(std::max(1, exposureMs)),
                LogLevel::INFO,
                DeviceType::MAIN);

    // Route both INDI and SDK captures through MainWindow so the existing
    // PoleCamera callback/pending flag remains the single capture-complete path.
    emit requestCaptureForRole("PoleCamera", std::max(1, exposureMs));
    Logger::Log("PoleMasterPolarAlignment: capture dispatch done mode=real exposureMs=" +
                    std::to_string(std::max(1, exposureMs)) +
                    " dispatchMs=" + std::to_string(stageTimer.elapsed()),
                LogLevel::INFO,
                DeviceType::MAIN);
    return true;
}

bool PoleMasterPolarAlignment::captureImageSimulated(int exposureMs)
{
    QElapsedTimer totalTimer;
    totalTimer.start();
    captureEnded = false;
    latestCapturePathFromHost.clear();
    lastCapturedImage.clear();

    const QString scriptPath = simulationScriptPath();
    if (scriptPath.isEmpty())
    {
        Logger::Log("PoleMasterPolarAlignment(sim): simulation script not found", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    QString imageRoot = simulationImageRootPath.trimmed();
    if (imageRoot.isEmpty())
        imageRoot = "/tmp";
    QDir rootDir(imageRoot);
    if (!rootDir.exists() && !rootDir.mkpath("."))
        return false;

    const QString fileName = QString("PoleMasterSim_%1.jpg").arg(simulationFrameIndex, 4, 10, QLatin1Char('0'));
    const QPointF axis = currentSimulationAxisPx();
    const QPointF pole = simulationPoleForCurrentState();
    const double rollDeg = std::fmod(std::fabs(simulationVirtualRaDeg), 360.0);
    const int scriptIndex = simulationScriptIndexForState();
    const int clippedExposure = std::max(10, exposureMs);
    QStringList args = {
        scriptPath,
        "--out-dir", imageRoot,
        "--fits-dir", "/dev/shm",
        "--fits-name", "polecamera.fits",
        "--frame-index", QString::number(simulationFrameIndex),
        "--script-index", QString::number(scriptIndex),
        "--state-number", QString::number(static_cast<int>(currentState)),
        "--exposure-ms", QString::number(clippedExposure),
        "--hemisphere", config.latitude < 0.0 ? "south" : "north",
        "--width", "1024",
        "--height", "768",
        "--axis-x", QString::number(axis.x(), 'f', 4),
        "--axis-y", QString::number(axis.y(), 'f', 4),
        "--fov", "11.0",
        "--fov-y", "8.0",
        "--max-mag", "15.0",
        "--pole-x", QString::number(pole.x(), 'f', 4),
        "--pole-y", QString::number(pole.y(), 'f', 4),
        "--roll-deg", QString::number(rollDeg, 'f', 4)
    };

    QElapsedTimer simGenTimer;
    simGenTimer.start();
    QProcess proc;
    proc.start("python3", args);
    if (!proc.waitForStarted(5000))
        return false;
    if (!proc.waitForFinished(25000))
    {
        proc.kill();
        return false;
    }
    const QByteArray out = proc.readAllStandardOutput().trimmed();
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0 || out.isEmpty())
        return false;

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(out, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return false;

    const QJsonObject root = doc.object();
    const QString fitsPath = root.value("fitsPath").toString().trimmed();
    if (fitsPath.isEmpty() || !QFileInfo::exists(fitsPath))
        return false;
    const qint64 simGenerateMs = simGenTimer.elapsed();
    latestCapturePathFromHost = fitsPath;
    captureEnded = true;
    const QString emittedFileName = root.value("fileName").toString().trimmed().isEmpty() ? fileName : root.value("fileName").toString().trimmed();
    const QString emittedJpgPath = QDir(imageRoot).filePath(emittedFileName);
    int previewW = 0;
    int previewH = 0;
    QElapsedTimer previewTimer;
    previewTimer.start();
    if (!buildPreviewJpgFromFits(fitsPath, emittedJpgPath, previewW, previewH))
    {
        Logger::Log("PoleMasterPolarAlignment(sim): failed to build preview jpg from fits, fits=" +
                        fitsPath.toStdString() +
                        " jpg=" + emittedJpgPath.toStdString(),
                    LogLevel::WARNING,
                    DeviceType::MAIN);
    }
    const qint64 previewBuildMs = previewTimer.elapsed();
    Logger::Log("PoleMasterPolarAlignment(sim): capture pipeline timing mode=sim exposureMs=" +
                    std::to_string(clippedExposure) +
                    " simGenerateMs=" + std::to_string(simGenerateMs) +
                    " previewBuildMs=" + std::to_string(previewBuildMs) +
                    " totalMs=" + std::to_string(totalTimer.elapsed()) +
                    " fitsPath=" + fitsPath.toStdString(),
                LogLevel::INFO,
                DeviceType::MAIN);
    pendingFrameFileName = emittedFileName;
    pendingFrameImageW = (previewW > 0 ? previewW : 1024);
    pendingFrameImageH = (previewH > 0 ? previewH : 768);
    pendingFrameId = QFileInfo(fitsPath).completeBaseName();
    ++simulationFrameIndex;
    return true;
}

bool PoleMasterPolarAlignment::waitForCaptureComplete()
{
    QElapsedTimer waitTimer;
    waitTimer.start();
    QEventLoop loop;
    QTimer timeoutTimer;
    QTimer checkTimer;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.start(config.captureTimeoutMs);
    checkTimer.setInterval(100);

    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(&checkTimer, &QTimer::timeout, [&]() {
        if (!running)
        {
            loop.quit();
            return;
        }
        if (captureEnded)
        {
            lastCapturedImage = latestCapturePathFromHost.trimmed();
            loop.quit();
        }
    });
    checkTimer.start();
    loop.exec();
    checkTimer.stop();
    timeoutTimer.stop();

    const bool ok = captureEnded && !lastCapturedImage.isEmpty() && QFileInfo::exists(lastCapturedImage);
    Logger::Log("PoleMasterPolarAlignment: capture wait result mode=" +
                    std::string(simulationMode ? "sim" : "real") +
                    " ok=" + std::to_string(ok ? 1 : 0) +
                    " waitMs=" + std::to_string(waitTimer.elapsed()) +
                    " captureEnded=" + std::to_string(captureEnded ? 1 : 0) +
                    " path=" + lastCapturedImage.toStdString(),
                ok ? LogLevel::INFO : LogLevel::WARNING,
                DeviceType::MAIN);
    if (!ok)
    {
        Logger::Log("PoleMasterPolarAlignment: capture wait failed timeoutMs=" +
                        std::to_string(config.captureTimeoutMs) +
                        " captureEnded=" + std::to_string(captureEnded ? 1 : 0) +
                        " path=" + lastCapturedImage.toStdString(),
                    LogLevel::WARNING,
                    DeviceType::MAIN);
    }
    return ok;
}

bool PoleMasterPolarAlignment::solveImage(const QString &fitsPath)
{
    const double priorRaDeg = kTruePoleRaDeg;
    const double priorDecDeg = truePoleDecDeg();
    const double solveSearchRadiusDeg = config.solveSearchRadiusDeg > 0.0 ? config.solveSearchRadiusDeg : 5.0;
    QString backendConfigPath;
    const QString indexFilePath = config.solveIndexFilePath.trimmed();
    if (!indexFilePath.isEmpty())
    {
        const QString backendCfg = "/dev/shm/quarcs-pole-solve-backend.cfg";
        QFile cfgFile(backendCfg);
        if (cfgFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        {
            QString cfgContent =
                "add_path /usr/local/astrometry/data\n"
                "inparallel\n"
                "cpulimit 20\n";
            const QStringList indexFiles = indexFilePath.split(',', Qt::SkipEmptyParts);
            for (const QString &indexNameRaw : indexFiles)
            {
                const QString indexName = indexNameRaw.trimmed();
                if (!indexName.isEmpty())
                    cfgContent += "index " + indexName + "\n";
            }
            if (indexFiles.isEmpty())
            {
                cfgContent += "index " + indexFilePath + "\n";
            }
            cfgFile.write(cfgContent.toUtf8());
            cfgFile.close();
            backendConfigPath = backendCfg;
        }
        else
        {
            Logger::Log("PoleMasterPolarAlignment: failed to write backend config, fallback to default indexes",
                        LogLevel::WARNING,
                        DeviceType::MAIN);
        }
    }
    const bool ok = Tools::PlateSolve(fitsPath,
                                      config.focalLength,
                                      config.cameraWidth,
                                      config.cameraHeight,
                                      false,
                                      2,
                                      priorRaDeg,
                                      priorDecDeg,
                                      solveSearchRadiusDeg,
                                      backendConfigPath);
    if (!ok) return false;
    QCoreApplication::processEvents();
    return Tools::isSolveImageFinish();
}

bool PoleMasterPolarAlignment::readSolveFrame(const QString &fitsPath, int exposureMs, SolveFrame &frame) const
{
    int imageW = 0;
    int imageH = 0;
    if (!readFitsSize(fitsPath, imageW, imageH)) return false;

    SloveResults solveResult = Tools::ReadSolveResult(fitsPath, imageW, imageH);
    if (solveResult.RA_Degree < -0.5 || solveResult.DEC_Degree < -90.5)
        return false;

    const QFileInfo fitsInfo(fitsPath);
    const QString wcsPath = fitsInfo.dir().filePath(fitsInfo.completeBaseName() + ".wcs");
    if (!QFileInfo::exists(wcsPath)) return false;

    QPointF polePx;
    if (!skyToPixel(wcsPath, kTruePoleRaDeg, truePoleDecDeg(), polePx)) return false;

    frame.fitsPath = fitsPath;
    frame.wcsPath = wcsPath;
    frame.imageW = imageW;
    frame.imageH = imageH;
    frame.pixelScaleArcsecPerPixel = solveResult.pixelScaleArcsecPerPixel > 0.0
                                         ? solveResult.pixelScaleArcsecPerPixel
                                         : 1.0;
    frame.truePolePx = polePx;
    frame.raDeg = solveResult.RA_Degree;
    frame.decDeg = solveResult.DEC_Degree;
    frame.exposureMs = exposureMs;
    frame.frameId = fitsInfo.completeBaseName();
    frame.northAngleValid = estimateNorthAngleDeg(frame, frame.northAngleDeg);
    return true;
}

bool PoleMasterPolarAlignment::readFitsSize(const QString &fitsPath, int &imageW, int &imageH) const
{
    imageW = 0;
    imageH = 0;
    fitsfile *fptr = nullptr;
    int status = 0;
    int naxis = 0;
    long naxes[2] = {0, 0};
    if (fits_open_file(&fptr, fitsPath.toUtf8().constData(), READONLY, &status)) return false;
    fits_get_img_dim(fptr, &naxis, &status);
    if (status == 0 && naxis >= 2)
        fits_get_img_size(fptr, 2, naxes, &status);
    fits_close_file(fptr, &status);
    if (status != 0 || naxes[0] <= 0 || naxes[1] <= 0) return false;
    imageW = static_cast<int>(naxes[0]);
    imageH = static_cast<int>(naxes[1]);
    return true;
}

bool PoleMasterPolarAlignment::skyToPixel(const QString &wcsPath, double raDeg, double decDeg, QPointF &point) const
{
    point = QPointF(-1.0, -1.0);
    QProcess proc;
    const QStringList args = {
        "-w", wcsPath,
        "-r", QString::number(raDeg, 'f', 8),
        "-d", QString::number(decDeg, 'f', 8)
    };
    proc.start("wcs-rd2xy", args);
    if (!proc.waitForFinished(5000))
    {
        proc.kill();
        Logger::Log("PoleMasterPolarAlignment: wcs-rd2xy timeout", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }

    const QString out = proc.readAllStandardOutput() + "\n" + proc.readAllStandardError();
    QRegularExpression reFloat("[-+]?\\d+(?:\\.\\d+)?(?:[eE][-+]?\\d+)?");
    QRegularExpressionMatchIterator it = reFloat.globalMatch(out);
    QVector<double> values;
    while (it.hasNext())
    {
        bool ok = false;
        const double v = it.next().captured(0).toDouble(&ok);
        if (ok) values.push_back(v);
    }
    if (values.size() < 2)
    {
        Logger::Log("PoleMasterPolarAlignment: wcs-rd2xy output parse failed: " + out.toStdString(),
                    LogLevel::WARNING,
                    DeviceType::MAIN);
        return false;
    }
    point = QPointF(values[values.size() - 2], values[values.size() - 1]);
    return isFinitePoint(point);
}

bool PoleMasterPolarAlignment::estimateNorthAngleDeg(const SolveFrame &frame, double &northAngleDeg) const
{
    northAngleDeg = 0.0;
    if (frame.wcsPath.isEmpty() || frame.imageW <= 0 || frame.imageH <= 0)
        return false;

    const double cx = frame.imageW * 0.5;
    const double cy = frame.imageH * 0.5;
    bool okCenter = false;
    const SphericalCoordinates centerSky = Tools::xy2rdByExternal(frame.wcsPath, cx, cy, okCenter);
    if (!okCenter || !std::isfinite(centerSky.ra) || !std::isfinite(centerSky.dec))
        return false;

    QPointF pCenter;
    if (!skyToPixel(frame.wcsPath, centerSky.ra, centerSky.dec, pCenter))
        return false;

    const double targetDec = std::clamp(centerSky.dec + 0.15, -89.8, 89.8);
    QPointF pNorth;
    if (!skyToPixel(frame.wcsPath, centerSky.ra, targetDec, pNorth))
        return false;

    const double dx = pNorth.x() - pCenter.x();
    const double dy = pNorth.y() - pCenter.y();
    if (!std::isfinite(dx) || !std::isfinite(dy) || std::hypot(dx, dy) < 1e-6)
        return false;

    northAngleDeg = std::atan2(dx, -dy) * 180.0 / M_PI;
    northAngleDeg = normalizeSignedAngleDeg(northAngleDeg);
    return true;
}

void PoleMasterPolarAlignment::enrichFrameDiagnostics(SolveFrame &frame) const
{
    frame.detectedStars.clear();
    frame.detectedStarScores.clear();
    const QList<FITSImage::Star> stars = Tools::FindStarsByFocusedCppFromFile(frame.fitsPath, true, true);
    QVector<FITSImage::Star> sortedStars = stars.toVector();
    std::sort(sortedStars.begin(), sortedStars.end(), [](const FITSImage::Star &a, const FITSImage::Star &b) {
        return a.theta > b.theta;
    });
    for (const FITSImage::Star &star : sortedStars)
    {
        if (!std::isfinite(star.x) || !std::isfinite(star.y)) continue;
        if (star.x < 0.0 || star.y < 0.0 || star.x > frame.imageW || star.y > frame.imageH) continue;
        frame.detectedStars.append(QPointF(star.x, star.y));
        frame.detectedStarScores.append(star.theta);
        if (frame.detectedStars.size() >= 120) break;
    }
}

double PoleMasterPolarAlignment::truePoleDecDeg() const
{
    return config.latitude < 0.0 ? kSouthPoleDecDeg : kNorthPoleDecDeg;
}

QJsonArray PoleMasterPolarAlignment::pointsToJson(const QVector<QPointF> &points, int limit) const
{
    QJsonArray arr;
    for (int i = 0; i < points.size() && i < limit; ++i)
        arr.append(pointJson(points[i]));
    return arr;
}

QJsonArray PoleMasterPolarAlignment::fixedStarsToJson(const SolveFrame &frame) const
{
    QJsonArray arr;
    if (frame.wcsPath.isEmpty() || !QFileInfo::exists(frame.wcsPath))
        return arr;
    constexpr double kFixedStarMatchRadiusPx = 45.0;
    const double matchRadiusArcsec = kFixedStarMatchRadiusPx * std::max(0.001, frame.pixelScaleArcsecPerPixel);
    const QVector<FixedPoleStar> stars = fixedPoleStars(config.latitude < 0.0);
    QVector<QPointF> expectedList;
    QVector<bool> inFrameList;
    QVector<QVector<FixedStarCandidate>> candidatesByStar;
    expectedList.reserve(stars.size());
    inFrameList.reserve(stars.size());
    candidatesByStar.resize(stars.size());

    for (int i = 0; i < stars.size(); ++i)
    {
        QPointF expected;
        const bool projected = skyToPixel(frame.wcsPath, stars[i].raDeg, stars[i].decDeg, expected);
        const bool inFrame = projected &&
            expected.x() >= 0.0 && expected.y() >= 0.0 &&
            expected.x() <= frame.imageW && expected.y() <= frame.imageH;
        expectedList.push_back(expected);
        inFrameList.push_back(inFrame);
        if (!inFrame)
            continue;

        QVector<FixedStarCandidate> candidates;
        candidates.reserve(frame.detectedStars.size());
        for (int detectedIndex = 0; detectedIndex < frame.detectedStars.size(); ++detectedIndex)
        {
            const QPointF &candidate = frame.detectedStars[detectedIndex];
            const double distancePx = std::hypot(candidate.x() - expected.x(),
                                                 candidate.y() - expected.y());
            if (distancePx > kFixedStarMatchRadiusPx * 1.8)
                continue;

            bool ok = false;
            const SphericalCoordinates sky = Tools::xy2rdByExternal(frame.wcsPath,
                                                                    candidate.x(),
                                                                    candidate.y(),
                                                                    ok);
            if (!ok || !std::isfinite(sky.ra) || !std::isfinite(sky.dec))
                continue;
            const double distanceArcsec = angularDistanceArcsec(stars[i].raDeg, stars[i].decDeg,
                                                                sky.ra, sky.dec);
            if (distanceArcsec >= matchRadiusArcsec)
                continue;
            candidates.push_back({detectedIndex, distancePx, distanceArcsec});
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const FixedStarCandidate &a, const FixedStarCandidate &b) {
                      return a.distanceArcsec < b.distanceArcsec;
                  });
        if (candidates.size() > 12)
            candidates.resize(12);
        candidatesByStar[i] = std::move(candidates);
    }

    QVector<QPointF> detectedList;
    QVector<double> bestDistancePxList(stars.size(), -1.0);
    QVector<double> bestDistanceArcsecList(stars.size(), -1.0);
    QVector<bool> matchedList(stars.size(), false);
    detectedList.reserve(stars.size());
    for (int i = 0; i < stars.size(); ++i)
        detectedList.push_back(expectedList[i]);

    const double unmatchedPenaltyCost = matchRadiusArcsec * 1.25;
    const FixedStarMatchResult matchResult = solveGlobalFixedStarMatch(candidatesByStar,
                                                                        frame.detectedStars.size(),
                                                                        stars.size(),
                                                                        unmatchedPenaltyCost);
    for (int i = 0; i < stars.size(); ++i)
    {
        if (!inFrameList[i] || !matchResult.matched[i])
            continue;
        const int detectedIndex = matchResult.matchedDetectedIndex[i];
        if (detectedIndex < 0 || detectedIndex >= frame.detectedStars.size())
            continue;
        detectedList[i] = frame.detectedStars[detectedIndex];
        bestDistancePxList[i] = matchResult.matchedDistancePx[i];
        bestDistanceArcsecList[i] = matchResult.matchedDistanceArcsec[i];
        matchedList[i] = true;
    }

    for (int i = 0; i < stars.size(); ++i)
    {
        const FixedPoleStar &star = stars[i];
        QJsonObject obj;
        obj["id"] = star.id;
        obj["name"] = star.name;
        obj["raDeg"] = star.raDeg;
        obj["decDeg"] = star.decDeg;
        obj["mag"] = star.mag;
        obj["templateRole"] = star.templateRole;
        obj["expected"] = pointJson(expectedList[i]);
        obj["detected"] = pointJson(detectedList[i]);
        obj["matched"] = matchedList[i];
        obj["visible"] = inFrameList[i];
        obj["distancePx"] = matchedList[i] ? bestDistancePxList[i] : -1.0;
        obj["distanceArcsec"] = matchedList[i] ? bestDistanceArcsecList[i] : -1.0;
        obj["matchRadiusPx"] = kFixedStarMatchRadiusPx;
        obj["matchRadiusArcsec"] = matchRadiusArcsec;
        arr.append(obj);
    }

    return arr;
}

QJsonArray PoleMasterPolarAlignment::rotationSamplesToJson() const
{
    QJsonArray arr;
    for (int i = 0; i < calibrationFrames.size(); ++i)
    {
        QJsonObject obj;
        obj["index"] = i + 1;
        obj["pole"] = pointJson(calibrationFrames[i].truePolePx);
        obj["stars"] = pointsToJson(calibrationFrames[i].detectedStars, 24);
        arr.append(obj);
    }
    return arr;
}

QJsonObject PoleMasterPolarAlignment::qualityJson(const SolveFrame *frame, const QStringList &warnings, const QJsonObject &extra) const
{
    QJsonObject quality = extra;
    if (frame != nullptr)
    {
        quality["starCount"] = frame->detectedStars.size();
        quality["detectedStarCount"] = frame->detectedStars.size();
        quality["selectedTrackStarCount"] = frame->selectedTrackStars.size();
        quality["selectionReason"] = "top-k-bright-stars-with-min-separation";
        quality["exposureMs"] = frame->exposureMs;
        quality["pixelScaleArcsecPerPixel"] = frame->pixelScaleArcsecPerPixel;
        quality["lockConfidence"] = frame->lockConfidence;
        quality["trackingMode"] = frame->trackingMode;
        if (frame->northAngleValid)
        {
            double relativeDeg = frame->northAngleDeg;
            if (!calibrationFrames.isEmpty() && calibrationFrames.first().northAngleValid)
                relativeDeg = normalizeSignedAngleDeg(frame->northAngleDeg - calibrationFrames.first().northAngleDeg);
            quality["rotationFromFirstDeg"] = relativeDeg;
            quality["rotationDirection"] = relativeDeg >= 0.0 ? "cw" : "ccw";
        }
    }
    quality["axisRadiusPx"] = axisRadiusPx;
    quality["axisResidualPx"] = axisResidualPx;
    if (!actualRaRotations.isEmpty())
        quality["lastRaRotationDeg"] = actualRaRotations.last();
    QJsonArray warningArr;
    for (const QString &warning : warnings)
        warningArr.append(warning);
    quality["warnings"] = warningArr;
    return quality;
}

void PoleMasterPolarAlignment::emitOverlay(const QString &phase, const SolveFrame *frame, const QStringList &warnings, const QJsonObject &extra)
{
    QJsonObject root;
    root["phase"] = phase;
    root["hemisphere"] = config.latitude < 0.0 ? "south" : "north";
    QJsonArray templateRoles;
    const QVector<FixedPoleStar> templateStars = fixedPoleStars(config.latitude < 0.0);
    for (const FixedPoleStar &s : templateStars)
    {
        QJsonObject item;
        item["id"] = s.id;
        item["name"] = s.name;
        item["templateRole"] = s.templateRole;
        templateRoles.append(item);
    }
    root["templateRoles"] = templateRoles;
    QJsonObject qualityExtra = extra;
    if (frame != nullptr)
    {
        const QJsonArray fixedStars = fixedStarsToJson(*frame);
        int fixedMatchedCount = 0;
        for (const QJsonValue &value : fixedStars)
        {
            if (value.toObject().value("matched").toBool(false))
                fixedMatchedCount++;
        }
        qualityExtra["fixedStarCount"] = fixedStars.size();
        qualityExtra["fixedStarMatchedCount"] = fixedMatchedCount;
        root["imageW"] = frame->imageW;
        root["imageH"] = frame->imageH;
        root["frameId"] = frame->frameId;
        root["fitsPath"] = QFileInfo(frame->fitsPath).fileName();
        root["fixedStars"] = fixedStars;
        root["detectedStars"] = pointsToJson(frame->detectedStars, 120);
        root["selectedTrackStars"] = pointsToJson(frame->selectedTrackStars, 60);
    }
    else if (!calibrationFrames.isEmpty())
    {
        root["imageW"] = calibrationFrames.last().imageW;
        root["imageH"] = calibrationFrames.last().imageH;
        root["frameId"] = calibrationFrames.last().frameId;
    }
    else if (!pendingFrameId.isEmpty())
    {
        root["frameId"] = pendingFrameId;
    }
    root["rotationSamples"] = rotationSamplesToJson();
    QJsonObject axis = pointJson(axisCenterPx);
    axis["radiusPx"] = axisRadiusPx;
    axis["residualPx"] = axisResidualPx;
    root["axisCandidate"] = axis;
    QJsonObject tracking;
    tracking["mode"] = frame ? frame->trackingMode : QString("idle");
    tracking["lockState"] = guidingTrackingInitialized && isFinitePoint(guidingLockStarPx) ? QString("locked") : QString("unlocked");
    tracking["lockConfidence"] = frame ? frame->lockConfidence : guidingLockConfidence;
    tracking["selectedTrackStarCount"] = frame ? frame->selectedTrackStars.size() : 0;
    tracking["lostFrames"] = guidingLostFrames;
    root["tracking"] = tracking;
    root["quality"] = qualityJson(frame, warnings, qualityExtra);
    QJsonArray warningArr;
    for (const QString &warning : warnings)
        warningArr.append(warning);
    root["warnings"] = warningArr;
    emit overlayData(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    if (!pendingFrameFileName.isEmpty())
    {
        emit frameData(pendingFrameFileName, pendingFrameImageW, pendingFrameImageH, pendingFrameId);
        pendingFrameFileName.clear();
        pendingFrameImageW = 0;
        pendingFrameImageH = 0;
        pendingFrameId.clear();
    }
}

bool PoleMasterPolarAlignment::moveRaAxis(double angleDeg)
{
    if (simulationMode)
    {
        lastRaDeg = simulationVirtualRaDeg;
        simulationVirtualRaDeg = std::fmod(simulationVirtualRaDeg + angleDeg + 360.0, 360.0);
        return true;
    }
    if (indiServer == nullptr || dpMount == nullptr) return false;
    double raHours = 0.0;
    double decDeg = 0.0;
    indiServer->getTelescopeRADECJNOW(dpMount, raHours, decDeg);
    double raDeg = Tools::HourToDegree(raHours);
    lastRaDeg = raDeg;
    lastDecDeg = decDeg;
    raDeg = std::fmod(raDeg + angleDeg, 360.0);
    if (raDeg < 0.0) raDeg += 360.0;
    indiServer->slewTelescopeJNowNonBlock(dpMount, Tools::DegreeToHour(raDeg), decDeg, true);
    return true;
}

bool PoleMasterPolarAlignment::captureMountPosition(double &raDeg, double &decDeg) const
{
    if (simulationMode)
    {
        raDeg = simulationVirtualRaDeg;
        decDeg = simulationVirtualDecDeg;
        return true;
    }
    if (indiServer == nullptr || dpMount == nullptr) return false;
    double raHours = 0.0;
    double dec = 0.0;
    indiServer->getTelescopeRADECJNOW(dpMount, raHours, dec);
    raDeg = Tools::HourToDegree(raHours);
    decDeg = dec;
    return std::isfinite(raDeg) && std::isfinite(decDeg);
}

double PoleMasterPolarAlignment::wrappedAngleDelta(double fromDeg, double toDeg) const
{
    double delta = std::fmod(toDeg - fromDeg + 540.0, 360.0) - 180.0;
    if (delta < -180.0) delta += 360.0;
    return delta;
}

bool PoleMasterPolarAlignment::returnToStartPosition()
{
    if (!hasStartMountPosition || indiServer == nullptr || dpMount == nullptr) return false;
    emitOverlay("return-home", calibrationFrames.isEmpty() ? nullptr : &calibrationFrames.last());
    indiServer->slewTelescopeJNowNonBlock(dpMount, Tools::DegreeToHour(startRaDeg), startDecDeg, true);
    return waitForMountIdle();
}

bool PoleMasterPolarAlignment::waitForMountIdle()
{
    if (simulationMode)
    {
        const double delta = std::fabs(wrappedAngleDelta(lastRaDeg, simulationVirtualRaDeg));
        if (delta > 0.01)
            actualRaRotations.append(delta);
        lastRaDeg = simulationVirtualRaDeg;
        lastDecDeg = simulationVirtualDecDeg;
        QThread::msleep(120);
        return true;
    }
    if (indiServer == nullptr || dpMount == nullptr) return false;
    QElapsedTimer timer;
    timer.start();
    QString status;
    while (timer.elapsed() < config.movementTimeoutMs)
    {
        indiServer->getTelescopeStatus(dpMount, status);
        if (status == "Idle")
        {
            double nowRa = 0.0;
            double nowDec = 0.0;
            if (captureMountPosition(nowRa, nowDec))
            {
                const double delta = std::fabs(wrappedAngleDelta(lastRaDeg, nowRa));
                if (delta > 1.0)
                    actualRaRotations.append(delta);
                lastRaDeg = nowRa;
                lastDecDeg = nowDec;
            }
            return true;
        }
        QThread::msleep(500);
        QCoreApplication::processEvents();
    }
    return false;
}

bool PoleMasterPolarAlignment::fitAxisCenter()
{
    if (calibrationFrames.size() < 3) return false;
    QPointF center;
    double radius = 0.0;
    double residual = 0.0;
    QStringList warnings;
    bool usedMultiStar = fitAxisCenterFromDetectedStars(center, radius, residual);
    if (!usedMultiStar)
    {
        warnings << "多星旋转中心匹配不足，已降级为天极三点圆拟合";
        if (!fitCircle3Points(calibrationFrames[0].truePolePx,
                              calibrationFrames[1].truePolePx,
                              calibrationFrames[2].truePolePx,
                              center,
                              radius))
        {
            return false;
        }
        residual = 0.0;
    }
    if (!isFinitePoint(center) || !std::isfinite(radius) || radius < 5.0)
        return false;

    axisCenterPx = center;
    axisRadiusPx = radius;
    axisResidualPx = residual;
    if (!actualRaRotations.isEmpty() && actualRaRotations.last() < 30.0)
        warnings << QString("最近一次 RA 实际旋转角 %1°，低于 30° 建议值").arg(actualRaRotations.last(), 0, 'f', 1);
    Logger::Log("PoleMasterPolarAlignment: axis center fitted x=" + std::to_string(center.x()) +
                    " y=" + std::to_string(center.y()) +
                    " radius=" + std::to_string(radius) +
                    " residual=" + std::to_string(residual) +
                    " method=" + std::string(usedMultiStar ? "multi-star" : "fallback-pole"),
                LogLevel::INFO,
                DeviceType::MAIN);
    QJsonObject extra;
    extra["method"] = usedMultiStar ? "multi-star" : "fallback-pole";
    emitOverlay("axis-fit", &calibrationFrames.last(), warnings, extra);
    emitCurrentGuide(calibrationFrames.last());
    return true;
}

bool PoleMasterPolarAlignment::fitAxisCenterFromDetectedStars(QPointF &center, double &radius, double &residual) const
{
    center = QPointF(-1.0, -1.0);
    radius = 0.0;
    residual = 0.0;
    if (calibrationFrames.size() < 3) return false;

    QVector<QPointF> centers;
    QVector<double> radii;
    const QVector<QPointF> &base = calibrationFrames[0].detectedStars;
    for (const QPointF &p1 : base)
    {
        QPointF p2(-1.0, -1.0);
        QPointF p3(-1.0, -1.0);
        double d2Best = 1e9;
        double d3Best = 1e9;
        for (const QPointF &candidate : calibrationFrames[1].detectedStars)
        {
            const double d = std::hypot(candidate.x() - p1.x(), candidate.y() - p1.y());
            if (d > 8.0 && d < d2Best)
            {
                d2Best = d;
                p2 = candidate;
            }
        }
        for (const QPointF &candidate : calibrationFrames[2].detectedStars)
        {
            const double d = std::hypot(candidate.x() - p1.x(), candidate.y() - p1.y());
            if (d > 16.0 && d < d3Best)
            {
                d3Best = d;
                p3 = candidate;
            }
        }
        QPointF c;
        double r = 0.0;
        if (isFinitePoint(p2) && isFinitePoint(p3) && fitCircle3Points(p1, p2, p3, c, r) && r > 20.0 && r < 3000.0)
        {
            centers.append(c);
            radii.append(r);
        }
        if (centers.size() >= 16) break;
    }
    if (centers.size() < 3) return false;

    std::sort(centers.begin(), centers.end(), [](const QPointF &a, const QPointF &b) { return a.x() < b.x(); });
    QVector<double> ys;
    ys.reserve(centers.size());
    for (const QPointF &p : centers) ys.append(p.y());
    std::sort(ys.begin(), ys.end());
    const QPointF medianCenter(centers[centers.size() / 2].x(), ys[ys.size() / 2]);
    double sum = 0.0;
    for (const QPointF &p : centers)
        sum += std::hypot(p.x() - medianCenter.x(), p.y() - medianCenter.y());
    residual = sum / centers.size();
    center = medianCenter;
    std::sort(radii.begin(), radii.end());
    radius = radii[radii.size() / 2];
    return residual < 180.0 && isFinitePoint(center);
}

QVector<int> PoleMasterPolarAlignment::selectTrackingStarIndices(const SolveFrame &frame) const
{
    QVector<int> indices;
    const int maxStars = std::max(8, config.trackMaxStars);
    const double minSep = std::max(4.0, config.trackMinSeparationPx);
    const double marginX = frame.imageW * std::clamp(config.trackEdgeMarginRatio, 0.01, 0.25);
    const double marginY = frame.imageH * std::clamp(config.trackEdgeMarginRatio, 0.01, 0.25);

    QVector<int> order(frame.detectedStars.size());
    for (int i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const double sa = (a < frame.detectedStarScores.size()) ? frame.detectedStarScores[a] : 0.0;
        const double sb = (b < frame.detectedStarScores.size()) ? frame.detectedStarScores[b] : 0.0;
        return sa > sb;
    });

    for (int idx : order)
    {
        const QPointF &p = frame.detectedStars[idx];
        if (p.x() < marginX || p.x() > frame.imageW - marginX || p.y() < marginY || p.y() > frame.imageH - marginY)
            continue;

        bool tooClose = false;
        for (int kept : indices)
        {
            const QPointF &k = frame.detectedStars[kept];
            if (std::hypot(p.x() - k.x(), p.y() - k.y()) < minSep)
            {
                tooClose = true;
                break;
            }
        }
        if (tooClose)
            continue;

        indices.append(idx);
        if (indices.size() >= maxStars)
            break;
    }

    return indices;
}

bool PoleMasterPolarAlignment::chooseInitialLockStar(const SolveFrame &frame, QPointF &selected) const
{
    if (frame.selectedTrackStars.isEmpty())
        return false;
    const double cx = frame.imageW * 0.5;
    const double cy = frame.imageH * 0.5;
    double bestScore = std::numeric_limits<double>::infinity();
    QPointF best;
    for (const QPointF &s : frame.selectedTrackStars)
    {
        const double score = std::hypot(s.x() - cx, s.y() - cy);
        if (score < bestScore)
        {
            bestScore = score;
            best = s;
        }
    }
    if (!isFinitePoint(best))
        return false;
    selected = best;
    return true;
}

bool PoleMasterPolarAlignment::updateLockStarByTracking(const cv::Mat &currentGray8,
                                                        const QVector<QPointF> &candidateStars,
                                                        QPointF &updatedLockStar,
                                                        double &confidence)
{
    updatedLockStar = guidingLockStarPx;
    confidence = guidingLockConfidence;
    if (!guideGrayReady || previousGuideGray8.empty() || currentGray8.empty() || !isFinitePoint(guidingLockStarPx))
        return false;

    std::vector<cv::Point2f> prevPts(1, cv::Point2f(static_cast<float>(guidingLockStarPx.x()),
                                                    static_cast<float>(guidingLockStarPx.y())));
    std::vector<cv::Point2f> nextPts;
    std::vector<uchar> status;
    std::vector<float> err;
    cv::calcOpticalFlowPyrLK(previousGuideGray8, currentGray8, prevPts, nextPts, status, err);
    if (status.empty() || status[0] == 0)
        return false;

    const QPointF lkPoint(nextPts[0].x, nextPts[0].y);
    if (!isFinitePoint(lkPoint))
        return false;

    const double adaptiveRadius = std::clamp(config.trackBaseRadiusPx + (1.0 - guidingLockConfidence) * 24.0,
                                             config.trackBaseRadiusPx,
                                             config.trackMaxRadiusPx);
    bool found = false;
    QPointF best = lkPoint;
    double bestDist = adaptiveRadius;
    for (const QPointF &s : candidateStars)
    {
        const double d = std::hypot(s.x() - lkPoint.x(), s.y() - lkPoint.y());
        if (d <= bestDist)
        {
            bestDist = d;
            best = s;
            found = true;
        }
    }

    if (found)
    {
        updatedLockStar = best;
        confidence = std::clamp(1.0 - bestDist / std::max(1.0, adaptiveRadius), 0.4, 1.0);
    }
    else
    {
        if (lkPoint.x() < 0.0 || lkPoint.y() < 0.0 || lkPoint.x() >= currentGray8.cols || lkPoint.y() >= currentGray8.rows)
            return false;
        updatedLockStar = lkPoint;
        confidence = std::max(0.15, guidingLockConfidence * 0.9);
    }
    return true;
}

bool PoleMasterPolarAlignment::updateLockStarByGlobalTracking(const cv::Mat &currentGray8,
                                                              const QVector<QPointF> &candidateStars,
                                                              QPointF &updatedLockStar,
                                                              double &confidence)
{
    updatedLockStar = guidingLockStarPx;
    confidence = guidingLockConfidence;
    if (!guideGrayReady || previousGuideGray8.empty() || currentGray8.empty() || !isFinitePoint(guidingLockStarPx))
        return false;
    if (previousSelectedTrackStars.size() < 4 || candidateStars.size() < 4)
        return false;

    std::vector<cv::Point2f> prevPts;
    prevPts.reserve(static_cast<size_t>(previousSelectedTrackStars.size()));
    for (const QPointF &p : previousSelectedTrackStars)
        prevPts.emplace_back(static_cast<float>(p.x()), static_cast<float>(p.y()));

    std::vector<cv::Point2f> trackedPts;
    std::vector<uchar> status;
    std::vector<float> err;
    cv::calcOpticalFlowPyrLK(previousGuideGray8, currentGray8, prevPts, trackedPts, status, err);
    if (trackedPts.size() != prevPts.size() || status.empty())
        return false;

    const double adaptiveRadius = std::clamp(config.trackBaseRadiusPx + (1.0 - guidingLockConfidence) * 24.0,
                                             config.trackBaseRadiusPx,
                                             std::max(config.trackMaxRadiusPx, config.trackBaseRadiusPx + 1.0));

    std::vector<cv::Point2f> srcPts;
    std::vector<cv::Point2f> dstPts;
    srcPts.reserve(prevPts.size());
    dstPts.reserve(prevPts.size());
    for (size_t i = 0; i < prevPts.size(); ++i)
    {
        if (status[i] == 0)
            continue;
        const QPointF lkPoint(trackedPts[i].x, trackedPts[i].y);
        if (!isFinitePoint(lkPoint))
            continue;

        bool found = false;
        QPointF best = lkPoint;
        double bestDist = adaptiveRadius;
        for (const QPointF &s : candidateStars)
        {
            const double d = std::hypot(s.x() - lkPoint.x(), s.y() - lkPoint.y());
            if (d <= bestDist)
            {
                bestDist = d;
                best = s;
                found = true;
            }
        }
        if (!found)
            continue;

        srcPts.emplace_back(prevPts[i].x, prevPts[i].y);
        dstPts.emplace_back(static_cast<float>(best.x()), static_cast<float>(best.y()));
    }

    if (srcPts.size() < 4 || dstPts.size() < 4)
        return false;

    cv::Mat inliers;
    const cv::Mat affine = cv::estimateAffinePartial2D(srcPts, dstPts, inliers, cv::RANSAC, 3.0, 2000, 0.99, 10);
    if (affine.empty() || affine.rows != 2 || affine.cols != 3)
        return false;

    cv::Matx23d A;
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c)
            A(r, c) = affine.at<double>(r, c);

    const double x = guidingLockStarPx.x();
    const double y = guidingLockStarPx.y();
    const QPointF transformed(A(0, 0) * x + A(0, 1) * y + A(0, 2),
                              A(1, 0) * x + A(1, 1) * y + A(1, 2));
    if (!isFinitePoint(transformed))
        return false;

    QPointF best = transformed;
    double bestDist = std::max(8.0, adaptiveRadius * 1.4);
    bool snapped = false;
    for (const QPointF &s : candidateStars)
    {
        const double d = std::hypot(s.x() - transformed.x(), s.y() - transformed.y());
        if (d <= bestDist)
        {
            bestDist = d;
            best = s;
            snapped = true;
        }
    }

    int inlierCount = 0;
    for (int i = 0; i < inliers.rows; ++i)
    {
        if (inliers.at<uchar>(i, 0) != 0)
            ++inlierCount;
    }
    const double inlierRatio = srcPts.empty() ? 0.0 : static_cast<double>(inlierCount) / static_cast<double>(srcPts.size());
    const double snapScore = snapped ? std::clamp(1.0 - bestDist / std::max(1.0, adaptiveRadius * 1.4), 0.2, 1.0) : 0.2;
    updatedLockStar = best;
    confidence = std::clamp(0.65 * inlierRatio + 0.35 * snapScore, 0.3, 1.0);
    return true;
}

bool PoleMasterPolarAlignment::recoverTrackingWithoutSolve(const QVector<QPointF> &candidateStars)
{
    if (candidateStars.isEmpty())
        return false;

    if (isFinitePoint(guidingLockStarPx))
    {
        const double maxRecoverRadius = std::max(config.trackMaxRadiusPx * 2.0, 40.0);
        for (const QPointF &s : candidateStars)
        {
            if (std::hypot(s.x() - guidingLockStarPx.x(), s.y() - guidingLockStarPx.y()) <= maxRecoverRadius)
            {
                guidingLockStarPx = s;
                guidingLockConfidence = 0.5;
                guidingLostFrames = 0;
                return true;
            }
        }
    }

    const double cx = guidanceImageW * 0.5;
    const double cy = guidanceImageH * 0.5;
    double bestScore = std::numeric_limits<double>::infinity();
    QPointF best(-1.0, -1.0);
    for (const QPointF &s : candidateStars)
    {
        const double score = std::hypot(s.x() - cx, s.y() - cy);
        if (score < bestScore)
        {
            bestScore = score;
            best = s;
        }
    }
    if (!isFinitePoint(best))
        return false;

    guidingLockStarPx = best;
    guidingLockConfidence = 0.35;
    guidingLostFrames = 0;
    return true;
}

bool PoleMasterPolarAlignment::initGuidingTrackingFromLastSolve()
{
    if ((calibrationFrames.isEmpty() && !hasGuidingAnchorFrame) || !isFinitePoint(axisCenterPx))
        return false;

    const SolveFrame &anchor = hasGuidingAnchorFrame ? guidingAnchorFrame : calibrationFrames.last();
    if (anchor.fitsPath.isEmpty() || !QFileInfo::exists(anchor.fitsPath))
        return false;

    SolveFrame initFrame = anchor;
    initFrame.selectedTrackStars.clear();
    initFrame.trackedLockStarPx = QPointF(-1.0, -1.0);
    initFrame.lockConfidence = 0.0;
    initFrame.trackingMode = "tracking-init";
    if (initFrame.detectedStars.isEmpty())
        enrichFrameDiagnostics(initFrame);

    const QVector<int> selectedIdx = selectTrackingStarIndices(initFrame);
    for (int idx : selectedIdx)
        initFrame.selectedTrackStars.append(initFrame.detectedStars[idx]);
    if (initFrame.selectedTrackStars.isEmpty())
        return false;

    QPointF selectedLock;
    if (!chooseInitialLockStar(initFrame, selectedLock))
        return false;

    cv::Mat gray8;
    int imageW = 0;
    int imageH = 0;
    if (!buildGray8FromFits(anchor.fitsPath, gray8, imageW, imageH))
        return false;

    guidingLockStarPx = selectedLock;
    guidingPoleOffsetPx = anchor.truePolePx - guidingLockStarPx;
    guidingLockConfidence = 1.0;
    guidingLostFrames = 0;
    previousGuideGray8 = gray8.clone();
    guideGrayReady = !previousGuideGray8.empty();
    previousSelectedTrackStars = initFrame.selectedTrackStars;
    guidanceImageW = anchor.imageW;
    guidanceImageH = anchor.imageH;
    guidancePixelScaleArcsecPerPixel = anchor.pixelScaleArcsecPerPixel > 0.0 ? anchor.pixelScaleArcsecPerPixel : 1.0;
    guidingTrackingInitialized = true;

    initFrame.trackedLockStarPx = guidingLockStarPx;
    initFrame.lockConfidence = guidingLockConfidence;
    initFrame.trackingMode = "tracking-init";
    emitOverlay("guiding-init", &initFrame);
    return true;
}

bool PoleMasterPolarAlignment::captureAndTrackGuideFrame(SolveFrame &frame)
{
    QElapsedTimer totalTimer;
    totalTimer.start();
    if (!guidingTrackingInitialized)
        return false;

    if (simulationMode)
        updateSimulationGuidingOffset();

    const int exposureMin = std::max(kGuidanceBootstrapExposureMs, config.guidanceExposureMinMs);
    const int exposureMax = std::max(exposureMin, config.guidanceExposureMaxMs);
    const int upStep = std::max(1, config.guidanceExposureUpStepMs);
    const int downStep = std::max(1, config.guidanceExposureDownStepMs);
    const int starCountLow = std::max(1, config.guidanceStarCountLow);
    const int starCountHigh = std::max(starCountLow, config.guidanceStarCountHigh);
    if (!guidanceExposureSeededFromSolve)
    {
        int seedExposureMs = adaptiveSolveExposureMs;
        if (seedExposureMs <= 0)
        {
            if (hasGuidingAnchorFrame && guidingAnchorFrame.exposureMs > 0)
                seedExposureMs = guidingAnchorFrame.exposureMs;
            else if (!calibrationFrames.isEmpty() && calibrationFrames.last().exposureMs > 0)
                seedExposureMs = calibrationFrames.last().exposureMs;
            else
                seedExposureMs = config.guidanceExposureTime;
        }
        adaptiveGuidanceExposureMs = std::clamp(seedExposureMs, exposureMin, exposureMax);
        guidanceExposureSeededFromSolve = true;
        guidanceExposureBootstrapped = true;
    }
    else
    {
        adaptiveGuidanceExposureMs = std::clamp(adaptiveGuidanceExposureMs, exposureMin, exposureMax);
    }
    const int exposureMs = adaptiveGuidanceExposureMs;
    QElapsedTimer captureDispatchTimer;
    captureDispatchTimer.start();
    if (!captureImage(exposureMs))
        return false;
    const qint64 captureDispatchMs = captureDispatchTimer.elapsed();
    QElapsedTimer captureWaitTimer;
    captureWaitTimer.start();
    if (!waitForCaptureComplete())
        return false;
    const qint64 waitCaptureMs = captureWaitTimer.elapsed();

    cv::Mat gray8;
    int imageW = 0;
    int imageH = 0;
    QElapsedTimer fitsProcessTimer;
    fitsProcessTimer.start();
    if (!buildGray8FromFits(lastCapturedImage, gray8, imageW, imageH))
        return false;
    const qint64 fitsProcessMs = fitsProcessTimer.elapsed();

    frame.fitsPath = lastCapturedImage;
    frame.imageW = imageW;
    frame.imageH = imageH;
    frame.pixelScaleArcsecPerPixel = guidancePixelScaleArcsecPerPixel;
    frame.exposureMs = exposureMs;
    frame.frameId = QFileInfo(lastCapturedImage).completeBaseName();
    frame.trackingMode = "tracking-fused";
    enrichFrameDiagnostics(frame);

    const QVector<int> selectedIdx = selectTrackingStarIndices(frame);
    for (int idx : selectedIdx)
        frame.selectedTrackStars.append(frame.detectedStars[idx]);

    const int starCount = frame.detectedStars.size();
    if (starCount < starCountLow)
        adaptiveGuidanceExposureMs = std::min(exposureMax, adaptiveGuidanceExposureMs + upStep);
    else if (starCount > starCountHigh)
        adaptiveGuidanceExposureMs = std::max(exposureMin, adaptiveGuidanceExposureMs - downStep);

    QPointF updatedLock;
    double updatedConfidence = guidingLockConfidence;
    const bool tracked = updateLockStarByGlobalTracking(gray8, frame.selectedTrackStars, updatedLock, updatedConfidence);
    frame.trackingMode = "tracking-global";
    if (tracked)
    {
        guidingLockStarPx = updatedLock;
        guidingLockConfidence = updatedConfidence;
        guidingLostFrames = 0;
    }
    else
    {
        ++guidingLostFrames;
        frame.trackingMode = "tracking-recover";
        if (!recoverTrackingWithoutSolve(frame.selectedTrackStars))
        {
            guidingLockConfidence = std::max(0.0, guidingLockConfidence - 0.2);
        }
    }

    if (guidingLostFrames > std::max(2, config.trackLostFrameLimit))
    {
        setState(PoleMasterAlignmentState::FAILED, "实时引导失败：星点持续丢失，请重新执行校准", 82);
        return false;
    }

    Logger::Log("PoleMasterPolarAlignment: guide frame timing mode=" +
                    std::string(simulationMode ? "sim" : "real") +
                    " exposureMs=" + std::to_string(exposureMs) +
                    " stars=" + std::to_string(starCount) +
                    " captureDispatchMs=" + std::to_string(captureDispatchMs) +
                    " waitCaptureMs=" + std::to_string(waitCaptureMs) +
                    " fitsProcessMs=" + std::to_string(fitsProcessMs) +
                    " totalMs=" + std::to_string(totalTimer.elapsed()) +
                    " fitsPath=" + lastCapturedImage.toStdString(),
                LogLevel::INFO,
                DeviceType::MAIN);

    if (!isFinitePoint(guidingLockStarPx))
        return false;

    frame.trackedLockStarPx = guidingLockStarPx;
    frame.lockConfidence = guidingLockConfidence;
    if (simulationMode)
    {
        // In simulation, guide error should follow the synthetic pole truth used
        // by frame generation, not the lock-star anchored offset model.
        const QPointF simulatedPole = simulationPoleForCurrentState();
        frame.truePolePx = isFinitePoint(simulatedPole) ? simulatedPole
                                                        : (guidingLockStarPx + guidingPoleOffsetPx);
    }
    else
    {
        frame.truePolePx = guidingLockStarPx + guidingPoleOffsetPx;
    }
    previousGuideGray8 = gray8.clone();
    guideGrayReady = !previousGuideGray8.empty();
    previousSelectedTrackStars = frame.selectedTrackStars;
    emitOverlay("guiding", &frame);
    return true;
}

bool PoleMasterPolarAlignment::emitCurrentGuide(const SolveFrame &frame)
{
    if (!isFinitePoint(axisCenterPx) || !isFinitePoint(frame.truePolePx)) return false;
    const double dx = axisCenterPx.x() - frame.truePolePx.x();
    const double dy = axisCenterPx.y() - frame.truePolePx.y();
    const double errorPx = std::hypot(dx, dy);
    const double errorArcsec = errorPx * std::max(0.001, frame.pixelScaleArcsecPerPixel);
    const QString hint = buildHint(dx, dy);

    emit guideData(frame.imageW,
                   frame.imageH,
                   axisCenterPx.x(),
                   axisCenterPx.y(),
                   frame.truePolePx.x(),
                   frame.truePolePx.y(),
                   errorPx,
                   errorArcsec,
                   frame.frameId,
                   hint);

    if (errorArcsec <= config.doneThresholdArcsec)
        stableFrames++;
    else
        stableFrames = 0;

    if (stableFrames >= config.stableFrameRequirement)
        setState(PoleMasterAlignmentState::COMPLETED, "电子极轴镜校准完成", 100);
    return true;
}

QString PoleMasterPolarAlignment::buildHint(double dx, double dy) const
{
    const QString horizontal = std::fabs(dx) < 2.0 ? QString() : (dx > 0.0 ? "向右" : "向左");
    const QString vertical = std::fabs(dy) < 2.0 ? QString() : (dy > 0.0 ? "向下" : "向上");
    if (horizontal.isEmpty() && vertical.isEmpty()) return "保持当前位置";
    if (horizontal.isEmpty()) return vertical;
    if (vertical.isEmpty()) return horizontal;
    return horizontal + "/" + vertical;
}

QString PoleMasterPolarAlignment::simulationScriptPath() const
{
    const QString sourceFolderScript = QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath("polemaster_simulation/polemaster_simulate_frame.py");
    if (QFileInfo::exists(sourceFolderScript))
        return QFileInfo(sourceFolderScript).absoluteFilePath();
    const QString cwdFolderScript = QDir::current().filePath("src/polemaster_simulation/polemaster_simulate_frame.py");
    if (QFileInfo::exists(cwdFolderScript))
        return QFileInfo(cwdFolderScript).absoluteFilePath();
    return QString();
}

int PoleMasterPolarAlignment::simulationScriptIndexForState() const
{
    if (currentState == PoleMasterAlignmentState::FIRST_CAPTURE)
        return 0;
    if (currentState == PoleMasterAlignmentState::SECOND_CAPTURE)
        return 8;
    if (currentState == PoleMasterAlignmentState::THIRD_CAPTURE)
        return 16;
    if (currentState == PoleMasterAlignmentState::GUIDING_ADJUSTMENT)
        return 16 + simulationGuidingFrameCount;
    return 0;
}

QPointF PoleMasterPolarAlignment::simulationPoleForCurrentState() const
{
    const QPointF axis = currentSimulationAxisPx();
    if (currentState == PoleMasterAlignmentState::GUIDING_ADJUSTMENT)
        return axis + simulationGuidingOffsetPx;
    if (currentState == PoleMasterAlignmentState::SECOND_CAPTURE)
        return simulationPolePath().value(8, simulationPolePath().first());
    if (currentState == PoleMasterAlignmentState::THIRD_CAPTURE)
        return simulationPolePath().value(16, simulationPolePath().last());
    return simulationPolePath().first();
}

QPointF PoleMasterPolarAlignment::currentSimulationAxisPx() const
{
    if (isFinitePoint(axisCenterPx))
        return axisCenterPx;
    return simulationReferenceAxisPx();
}

QPointF PoleMasterPolarAlignment::simulationReferenceAxisPx()
{
    const QVector<QPointF> &path = simulationPolePath();
    if (path.size() > 16)
    {
        QPointF center;
        double radius = 0.0;
        if (fitCircle3Points(path[0], path[8], path[16], center, radius) &&
            isFinitePoint(center) && std::isfinite(radius) && radius > 1.0)
            return center;
    }
    return QPointF(512.0, 384.0);
}

void PoleMasterPolarAlignment::updateSimulationGuidingOffset()
{
    const double decay = 0.87;
    const double noiseX = (QRandomGenerator::global()->generateDouble() - 0.5) * 3.2;
    const double noiseY = (QRandomGenerator::global()->generateDouble() - 0.5) * 3.2;
    simulationGuidingOffsetPx.setX(simulationGuidingOffsetPx.x() * decay + noiseX);
    simulationGuidingOffsetPx.setY(simulationGuidingOffsetPx.y() * decay + noiseY);
    ++simulationGuidingFrameCount;
}

bool PoleMasterPolarAlignment::fitCircle3Points(const QPointF &p1,
                                                const QPointF &p2,
                                                const QPointF &p3,
                                                QPointF &center,
                                                double &radius)
{
    center = QPointF(-1.0, -1.0);
    radius = 0.0;
    const double x1 = p1.x(), y1 = p1.y();
    const double x2 = p2.x(), y2 = p2.y();
    const double x3 = p3.x(), y3 = p3.y();
    const double d = 2.0 * (x1 * (y2 - y3) + x2 * (y3 - y1) + x3 * (y1 - y2));
    if (std::fabs(d) < 1e-6) return false;

    const double x1s = x1 * x1 + y1 * y1;
    const double x2s = x2 * x2 + y2 * y2;
    const double x3s = x3 * x3 + y3 * y3;
    const double ux = (x1s * (y2 - y3) + x2s * (y3 - y1) + x3s * (y1 - y2)) / d;
    const double uy = (x1s * (x3 - x2) + x2s * (x1 - x3) + x3s * (x2 - x1)) / d;
    center = QPointF(ux, uy);
    radius = std::hypot(ux - x1, uy - y1);
    return isFinitePoint(center) && std::isfinite(radius);
}

PoleMasterAlignmentSimulation::PoleMasterAlignmentSimulation(const QString &imageRootPath,
                                                             QObject *parent)
    : QObject(parent)
    , imageRootPath(imageRootPath)
{
    stepTimer.setSingleShot(true);
    connect(&stepTimer, &QTimer::timeout, this, &PoleMasterAlignmentSimulation::onStepTimerTimeout);
}

PoleMasterAlignmentSimulation::~PoleMasterAlignmentSimulation()
{
    stop();
}

bool PoleMasterAlignmentSimulation::start()
{
    if (running) return false;
    running = true;
    scriptIndex = 0;
    frameIndex = 0;
    stableFrames = 0;
    lastGeneratedPole = currentPolePosition();
    lastGeneratedAxis = axisCenter;
    lastGeneratedPixelScaleArcsecPerPixel = kSimulationArcsecPerPixel;
    lastGeneratedOverlayJson.clear();
    generatedSolveFrames.clear();
    generatedAxisRadiusPx = 122.0;
    generatedAxisResidualPx = 3.4;
    adaptiveGuidanceExposureMs = 1000;
    lastGeneratedExposureMs = simulationExposureMs();
    setState(PoleMasterAlignmentState::INITIALIZING, "电子极轴镜模拟校准初始化", 5);
    return true;
}

void PoleMasterAlignmentSimulation::stop()
{
    if (!running && currentState == PoleMasterAlignmentState::IDLE) return;
    running = false;
    stepTimer.stop();
    setState(PoleMasterAlignmentState::IDLE, "电子极轴镜模拟校准已停止", 0);
}

bool PoleMasterAlignmentSimulation::isRunning() const
{
    return running;
}

void PoleMasterAlignmentSimulation::setState(PoleMasterAlignmentState newState,
                                             const QString &message,
                                             int progress)
{
    currentState = newState;
    Logger::Log("PoleMasterAlignmentSimulation: state=" +
                    std::to_string(static_cast<int>(newState)) +
                    " message=" + message.toStdString() +
                    " progress=" + std::to_string(progress),
                LogLevel::INFO,
                DeviceType::MAIN);

    const bool stillRunning = running &&
        newState != PoleMasterAlignmentState::IDLE &&
        newState != PoleMasterAlignmentState::COMPLETED &&
        newState != PoleMasterAlignmentState::FAILED;
    emit stateChanged(newState, message, progress, stillRunning);

    if (newState == PoleMasterAlignmentState::COMPLETED || newState == PoleMasterAlignmentState::FAILED)
    {
        running = false;
        emit stateChanged(newState, message, progress, false);
        return;
    }

    if (running) stepTimer.start(450);
}

void PoleMasterAlignmentSimulation::onStepTimerTimeout()
{
    if (!running) return;

    auto failAndStop = [this](const QString &message)
    {
        setState(PoleMasterAlignmentState::FAILED, message, 0);
    };

    switch (currentState)
    {
    case PoleMasterAlignmentState::INITIALIZING:
        setState(PoleMasterAlignmentState::FIRST_CAPTURE, "模拟拍摄并解析第一帧星场", 10);
        break;
    case PoleMasterAlignmentState::FIRST_CAPTURE:
        if (!emitFrameAndGuide())
        {
            failAndStop("电子极轴镜模拟校准失败：首帧解析失败");
            break;
        }
        scriptIndex = 1;
        setState(PoleMasterAlignmentState::MOVING_RA_FIRST, "模拟第一次旋转赤经轴", 25);
        break;
    case PoleMasterAlignmentState::MOVING_RA_FIRST:
        if (!emitFrameAndGuide())
        {
            failAndStop("电子极轴镜模拟校准失败：首次赤经轴旋转阶段解析失败");
            break;
        }
        setState(PoleMasterAlignmentState::SECOND_CAPTURE, "模拟拍摄并解析第二帧星场", 38);
        break;
    case PoleMasterAlignmentState::SECOND_CAPTURE:
        scriptIndex = 2;
        if (!emitFrameAndGuide())
        {
            failAndStop("电子极轴镜模拟校准失败：第二帧解析失败");
            break;
        }
        setState(PoleMasterAlignmentState::MOVING_RA_SECOND, "模拟第二次旋转赤经轴", 50);
        break;
    case PoleMasterAlignmentState::MOVING_RA_SECOND:
        if (!emitFrameAndGuide())
        {
            failAndStop("电子极轴镜模拟校准失败：第二次赤经轴旋转阶段解析失败");
            break;
        }
        setState(PoleMasterAlignmentState::THIRD_CAPTURE, "模拟拍摄并解析第三帧星场", 62);
        break;
    case PoleMasterAlignmentState::THIRD_CAPTURE:
        scriptIndex = 3;
        if (!emitFrameAndGuide())
        {
            failAndStop("电子极轴镜模拟校准失败：第三帧解析失败");
            break;
        }
        setState(PoleMasterAlignmentState::CALCULATING_AXIS, "模拟标定赤经轴旋转中心", 74);
        break;
    case PoleMasterAlignmentState::CALCULATING_AXIS:
        if (!emitFrameAndGuide())
        {
            failAndStop("电子极轴镜模拟校准失败：赤经轴中心标定失败");
            break;
        }
        // Keep guiding stage continuous with the last calibration reference frame.
        scriptIndex = std::max(scriptIndex, 16);
        setState(PoleMasterAlignmentState::GUIDING_ADJUSTMENT, "进入极轴镜模拟实时调整", 82);
        break;
    case PoleMasterAlignmentState::GUIDING_ADJUSTMENT:
        if (scriptIndex < simulationPolePath().size() - 1)
            scriptIndex++;
        if (!emitFrameAndGuide())
        {
            failAndStop("电子极轴镜模拟校准失败：实时调整阶段解析失败");
            break;
        }
        if (std::hypot(lastGeneratedPole.x() - lastGeneratedAxis.x(),
                       lastGeneratedPole.y() - lastGeneratedAxis.y()) *
                std::max(0.001, lastGeneratedPixelScaleArcsecPerPixel) <= 30.0)
            stableFrames++;
        else
            stableFrames = 0;
        if (scriptIndex >= simulationPolePath().size() - 1 && stableFrames >= 3)
            setState(PoleMasterAlignmentState::COMPLETED, "电子极轴镜模拟校准完成", 100);
        else if (running)
            stepTimer.start(700);
        break;
    default:
        break;
    }
}

bool PoleMasterAlignmentSimulation::emitFrameAndGuide()
{
    const QString fileName = generateFrameImage();
    if (fileName.isEmpty())
        return false;
    const QString frameId = QFileInfo(fileName).completeBaseName();

    const QPointF pole = isFinitePoint(lastGeneratedPole) ? lastGeneratedPole : currentPolePosition();
    const QPointF axis = isFinitePoint(lastGeneratedAxis) ? lastGeneratedAxis : axisCenter;
    const double pixelScale = std::max(0.001, lastGeneratedPixelScaleArcsecPerPixel);
    const double dx = axis.x() - pole.x();
    const double dy = axis.y() - pole.y();
    const double errorPx = std::hypot(dx, dy);
    const double errorArcsec = errorPx * pixelScale;
    emit guideData(imageSize.width(),
                   imageSize.height(),
                   axis.x(),
                   axis.y(),
                   pole.x(),
                   pole.y(),
                   errorPx,
                   errorArcsec,
                   frameId,
                   buildHint(dx, dy));
    emitOverlay(currentState == PoleMasterAlignmentState::GUIDING_ADJUSTMENT ? "guiding" : "simulation", frameId);
    emit frameData(fileName, imageSize.width(), imageSize.height(), frameId);
    return true;
}

void PoleMasterAlignmentSimulation::emitOverlay(const QString &phase, const QString &frameId)
{
    if (!lastGeneratedOverlayJson.isEmpty())
    {
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(lastGeneratedOverlayJson.toUtf8(), &parseError);
        if (parseError.error == QJsonParseError::NoError && doc.isObject())
        {
            QJsonObject root = doc.object();
            root["phase"] = phase;
            root["frameId"] = frameId;
            emit overlayData(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
            return;
        }
    }

    QJsonObject root;
    root["phase"] = phase;
    root["imageW"] = imageSize.width();
    root["imageH"] = imageSize.height();
    root["frameId"] = frameId;
    root["hemisphere"] = "north";

    QJsonArray fixedArr;
    const QVector<FixedPoleStar> catalog = fixedPoleStars(false);
    const QVector<QPointF> templateStars = currentTemplateStarPositions();
    int fixedMatchedCount = 0;
    for (int i = 0; i < templateStars.size() && i < catalog.size(); ++i)
    {
        const bool matched = true;
        const QPointF expected = templateStars[i];
        const QPointF detected = matched
            ? expected
            : expected + QPointF(-54.0, -36.0);
        QJsonObject obj;
        obj["id"] = catalog[i].id;
        obj["name"] = catalog[i].name;
        obj["raDeg"] = catalog[i].raDeg;
        obj["decDeg"] = catalog[i].decDeg;
        obj["mag"] = catalog[i].mag;
        obj["expected"] = pointJson(expected);
        obj["detected"] = matched ? pointJson(detected) : pointJson(QPointF(-1.0, -1.0));
        obj["matched"] = matched;
        obj["visible"] = true;
        obj["distancePx"] = matched ? std::hypot(detected.x() - expected.x(),
                                                 detected.y() - expected.y()) : -1.0;
        obj["matchRadiusPx"] = 45.0;
        fixedArr.append(obj);
        if (matched) fixedMatchedCount++;
    }
    root["fixedStars"] = fixedArr;

    const QVector<QPointF> poles = {
        simulationPolePath().value(0),
        simulationPolePath().value(8),
        simulationPolePath().value(16)
    };
    int visibleSampleCount = 1;
    if (currentState == PoleMasterAlignmentState::SECOND_CAPTURE ||
        currentState == PoleMasterAlignmentState::MOVING_RA_SECOND)
        visibleSampleCount = 2;
    else if (currentState == PoleMasterAlignmentState::THIRD_CAPTURE ||
             currentState == PoleMasterAlignmentState::CALCULATING_AXIS ||
             currentState == PoleMasterAlignmentState::GUIDING_ADJUSTMENT ||
             currentState == PoleMasterAlignmentState::COMPLETED)
        visibleSampleCount = 3;

    QJsonArray samples;
    for (int i = 0; i < std::min(visibleSampleCount, poles.size()); ++i)
    {
        QJsonObject sample;
        sample["index"] = i + 1;
        sample["pole"] = pointJson(poles[i]);
        QJsonArray stars;
        const double angle = i * 0.18;
        const double c = std::cos(angle);
        const double s = std::sin(angle);
        for (int n = 0; n < std::min(10, fixedStars().size()); ++n)
        {
            QPointF centered = fixedStars()[n].position - axisCenter;
            QPointF rotated(centered.x() * c - centered.y() * s,
                            centered.x() * s + centered.y() * c);
            stars.append(pointJson(axisCenter + rotated));
        }
        sample["stars"] = stars;
        samples.append(sample);
    }
    root["rotationSamples"] = samples;

    QJsonObject axis = pointJson(axisCenter);
    axis["radiusPx"] = 122.0;
    axis["residualPx"] = 3.4;
    root["axisCandidate"] = axis;

    QJsonObject quality;
    quality["starCount"] = fixedStars().size();
    quality["fixedStarCount"] = fixedArr.size();
    quality["fixedStarMatchedCount"] = fixedMatchedCount;
    quality["axisResidualPx"] = 3.4;
    quality["lastRaRotationDeg"] = 35.0;
    quality["method"] = "simulation-multi-star";
    quality["exposureMs"] = lastGeneratedExposureMs;
    root["quality"] = quality;
    root["warnings"] = QJsonArray();

    emit overlayData(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

QString PoleMasterAlignmentSimulation::simulationScriptPath() const
{
    const QString envPath = QString::fromLocal8Bit(qgetenv("QUARCS_POLEMASTER_SIM_SCRIPT")).trimmed();
    if (!envPath.isEmpty() && QFileInfo::exists(envPath))
        return envPath;

    const QString sourceFolderScript = QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath("polemaster_simulation/polemaster_simulate_frame.py");
    if (QFileInfo::exists(sourceFolderScript))
        return QFileInfo(sourceFolderScript).absoluteFilePath();

    const QString sourceRelative = QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath("polemaster_simulate_frame.py");
    if (QFileInfo::exists(sourceRelative))
        return QFileInfo(sourceRelative).absoluteFilePath();

    const QString legacySourceRelative = QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath("../scripts/polemaster_simulate_frame.py");
    if (QFileInfo::exists(legacySourceRelative))
        return QFileInfo(legacySourceRelative).absoluteFilePath();

    const QString cwdRelative = QDir::current().filePath("src/polemaster_simulate_frame.py");
    if (QFileInfo::exists(cwdRelative))
        return QFileInfo(cwdRelative).absoluteFilePath();

    const QString cwdFolderScript = QDir::current().filePath("src/polemaster_simulation/polemaster_simulate_frame.py");
    if (QFileInfo::exists(cwdFolderScript))
        return QFileInfo(cwdFolderScript).absoluteFilePath();

    const QString legacyCwdRelative = QDir::current().filePath("scripts/polemaster_simulate_frame.py");
    if (QFileInfo::exists(legacyCwdRelative))
        return QFileInfo(legacyCwdRelative).absoluteFilePath();

    const QString appRelative = QDir(QCoreApplication::applicationDirPath()).filePath("polemaster_simulate_frame.py");
    if (QFileInfo::exists(appRelative))
        return QFileInfo(appRelative).absoluteFilePath();

    const QString appFolderScript = QDir(QCoreApplication::applicationDirPath()).filePath("polemaster_simulation/polemaster_simulate_frame.py");
    if (QFileInfo::exists(appFolderScript))
        return QFileInfo(appFolderScript).absoluteFilePath();

    const QString legacyAppRelative = QDir(QCoreApplication::applicationDirPath()).filePath("scripts/polemaster_simulate_frame.py");
    if (QFileInfo::exists(legacyAppRelative))
        return QFileInfo(legacyAppRelative).absoluteFilePath();

    return QString();
}

bool PoleMasterAlignmentSimulation::readGeneratedFitsSize(const QString &fitsPath, int &imageW, int &imageH) const
{
    imageW = 0;
    imageH = 0;
    fitsfile *fptr = nullptr;
    int status = 0;
    int naxis = 0;
    long naxes[2] = {0, 0};
    if (fits_open_file(&fptr, fitsPath.toUtf8().constData(), READONLY, &status)) return false;
    fits_get_img_dim(fptr, &naxis, &status);
    if (status == 0 && naxis >= 2)
        fits_get_img_size(fptr, 2, naxes, &status);
    fits_close_file(fptr, &status);
    if (status != 0 || naxes[0] <= 0 || naxes[1] <= 0) return false;
    imageW = static_cast<int>(naxes[0]);
    imageH = static_cast<int>(naxes[1]);
    return true;
}

bool PoleMasterAlignmentSimulation::skyToPixelFromWcs(const QString &wcsPath, double raDeg, double decDeg, QPointF &point) const
{
    point = QPointF(-1.0, -1.0);
    QProcess proc;
    const QStringList args = {
        "-w", wcsPath,
        "-r", QString::number(raDeg, 'f', 8),
        "-d", QString::number(decDeg, 'f', 8)
    };
    proc.start("wcs-rd2xy", args);
    if (!proc.waitForFinished(5000))
    {
        proc.kill();
        return false;
    }

    const QString out = proc.readAllStandardOutput() + "\n" + proc.readAllStandardError();
    QRegularExpression reFloat("[-+]?\\d+(?:\\.\\d+)?(?:[eE][-+]?\\d+)?");
    QRegularExpressionMatchIterator it = reFloat.globalMatch(out);
    QVector<double> values;
    while (it.hasNext())
    {
        bool ok = false;
        const double v = it.next().captured(0).toDouble(&ok);
        if (ok) values.push_back(v);
    }
    if (values.size() < 2) return false;
    point = QPointF(values[values.size() - 2], values[values.size() - 1]);
    return isFinitePoint(point);
}

void PoleMasterAlignmentSimulation::enrichGeneratedFrameDiagnostics(SimSolvedFrame &frame) const
{
    frame.detectedStars.clear();
    const QList<FITSImage::Star> stars = Tools::FindStarsByFocusedCppFromFile(frame.fitsPath, true, true);
    QVector<FITSImage::Star> sortedStars = stars.toVector();
    std::sort(sortedStars.begin(), sortedStars.end(), [](const FITSImage::Star &a, const FITSImage::Star &b) {
        return a.theta > b.theta;
    });
    for (const FITSImage::Star &star : sortedStars)
    {
        if (!std::isfinite(star.x) || !std::isfinite(star.y)) continue;
        if (star.x < 0.0 || star.y < 0.0 || star.x > frame.imageW || star.y > frame.imageH) continue;
        frame.detectedStars.append(QPointF(star.x, star.y));
        if (frame.detectedStars.size() >= 120) break;
    }
}

bool PoleMasterAlignmentSimulation::solveGeneratedFits(const QString &fitsPath, SimSolvedFrame &frame)
{
    frame = SimSolvedFrame();
    if (!QFileInfo::exists(fitsPath)) return false;

    // Simulated PoleMaster frames use a 12 deg square FoV. Keep simulation
    // solver parameters aligned with the real PoleMaster path (mode2 + prior).
    QString backendConfigPath;
    {
        const QString backendCfg = "/dev/shm/quarcs-pole-solve-backend.cfg";
        QFile cfgFile(backendCfg);
        if (cfgFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        {
            const QString cfgContent =
                "add_path /usr/local/astrometry/data\n"
                "inparallel\n"
                "cpulimit 20\n"
                "index index-tycho2-12.littleendian.fits\n"
                "index index-tycho2-13.littleendian.fits\n"
                "index index-tycho2-14.littleendian.fits\n"
                "index index-tycho2-15.littleendian.fits\n";
            cfgFile.write(cfgContent.toUtf8());
            cfgFile.close();
            backendConfigPath = backendCfg;
        }
    }
    const bool plateSolveStarted = Tools::PlateSolve(fitsPath,
                                                     100,
                                                     21.0,
                                                     15.75,
                                                     false,
                                                     2,
                                                     lastSolvedRaDeg,
                                                     lastSolvedDecDeg,
                                                     5.0,
                                                     backendConfigPath);
    bool plateSolved = false;
    if (plateSolveStarted)
    {
        QCoreApplication::processEvents();
        plateSolved = Tools::isSolveImageFinish();
    }

    const QFileInfo fitsInfo(fitsPath);
    const QString solvedWcsPath = fitsInfo.dir().filePath(fitsInfo.completeBaseName() + ".wcs");
    const bool solvedWcsExists = QFileInfo::exists(solvedWcsPath);
    if (!plateSolveStarted || !plateSolved || !solvedWcsExists)
    {
        Logger::Log("PoleMasterAlignmentSimulation: plate solve did not produce usable WCS. "
                    "started=" + std::to_string(plateSolveStarted ? 1 : 0) +
                    " finished=" + std::to_string(plateSolved ? 1 : 0) +
                    " wcsExists=" + std::to_string(solvedWcsExists ? 1 : 0) +
                    " fits=" + fitsPath.toStdString() +
                    " wcs=" + solvedWcsPath.toStdString(),
                    LogLevel::WARNING,
                    DeviceType::MAIN);
        return false;
    }

    int imageW = 0;
    int imageH = 0;
    if (!readGeneratedFitsSize(fitsPath, imageW, imageH)) return false;
    const QString wcsPath = solvedWcsPath;

    QPointF polePx;
    if (!skyToPixelFromWcs(wcsPath, kTruePoleRaDeg, kNorthPoleDecDeg, polePx))
        return false;

    SloveResults solveResult = Tools::ReadSolveResult(fitsPath, imageW, imageH);
    const double fallbackScale = 12.0 * 3600.0 / std::max(1, imageW);
    frame.fitsPath = fitsPath;
    frame.wcsPath = wcsPath;
    frame.imageW = imageW;
    frame.imageH = imageH;
    frame.pixelScaleArcsecPerPixel = solveResult.pixelScaleArcsecPerPixel > 0.0
                                         ? solveResult.pixelScaleArcsecPerPixel
                                         : fallbackScale;
    frame.truePolePx = polePx;
    frame.frameId = fitsInfo.completeBaseName();
    frame.plateSolved = plateSolved;
    lastSolvedRaDeg = solveResult.RA_Degree;
    lastSolvedDecDeg = solveResult.DEC_Degree;
    enrichGeneratedFrameDiagnostics(frame);

    Logger::Log("PoleMasterAlignmentSimulation: generated FITS plate-solved" +
                    std::string(" path=") + fitsPath.toStdString() +
                    " wcs=" + wcsPath.toStdString() +
                    " stars=" + std::to_string(frame.detectedStars.size()),
                LogLevel::INFO,
                DeviceType::MAIN);
    return true;
}

QJsonArray PoleMasterAlignmentSimulation::generatedPointsToJson(const QVector<QPointF> &points, int limit) const
{
    QJsonArray arr;
    for (int i = 0; i < points.size() && i < limit; ++i)
        arr.append(pointJson(points[i]));
    return arr;
}

QJsonArray PoleMasterAlignmentSimulation::generatedFixedStarsToJson(const SimSolvedFrame &frame) const
{
    QJsonArray arr;
    constexpr double kFixedStarMatchRadiusPx = 45.0;
    const double matchRadiusArcsec = kFixedStarMatchRadiusPx * std::max(0.001, frame.pixelScaleArcsecPerPixel);
    const QVector<FixedPoleStar> stars = fixedPoleStars(false);
    QVector<QPointF> expectedList;
    QVector<bool> inFrameList;
    QVector<QVector<FixedStarCandidate>> candidatesByStar;
    expectedList.reserve(stars.size());
    inFrameList.reserve(stars.size());
    candidatesByStar.resize(stars.size());

    for (int i = 0; i < stars.size(); ++i)
    {
        QPointF expected;
        const bool projected = skyToPixelFromWcs(frame.wcsPath, stars[i].raDeg, stars[i].decDeg, expected);
        const bool inFrame = projected &&
            expected.x() >= 0.0 && expected.y() >= 0.0 &&
            expected.x() <= frame.imageW && expected.y() <= frame.imageH;
        expectedList.push_back(expected);
        inFrameList.push_back(inFrame);
        if (!inFrame)
            continue;

        QVector<FixedStarCandidate> candidates;
        candidates.reserve(frame.detectedStars.size());
        for (int detectedIndex = 0; detectedIndex < frame.detectedStars.size(); ++detectedIndex)
        {
            const QPointF &candidate = frame.detectedStars[detectedIndex];
            const double distancePx = std::hypot(candidate.x() - expected.x(),
                                                 candidate.y() - expected.y());
            if (distancePx > kFixedStarMatchRadiusPx * 1.8)
                continue;

            bool ok = false;
            const SphericalCoordinates sky = Tools::xy2rdByExternal(frame.wcsPath,
                                                                    candidate.x(),
                                                                    candidate.y(),
                                                                    ok);
            if (!ok || !std::isfinite(sky.ra) || !std::isfinite(sky.dec))
                continue;
            const double distanceArcsec = angularDistanceArcsec(stars[i].raDeg, stars[i].decDeg,
                                                                sky.ra, sky.dec);
            if (distanceArcsec >= matchRadiusArcsec)
                continue;
            candidates.push_back({detectedIndex, distancePx, distanceArcsec});
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const FixedStarCandidate &a, const FixedStarCandidate &b) {
                      return a.distanceArcsec < b.distanceArcsec;
                  });
        if (candidates.size() > 12)
            candidates.resize(12);
        candidatesByStar[i] = std::move(candidates);
    }

    QVector<QPointF> detectedList;
    QVector<double> bestDistancePxList(stars.size(), -1.0);
    QVector<double> bestDistanceArcsecList(stars.size(), -1.0);
    QVector<bool> matchedList(stars.size(), false);
    detectedList.reserve(stars.size());
    for (int i = 0; i < stars.size(); ++i)
        detectedList.push_back(expectedList[i]);

    const double unmatchedPenaltyCost = matchRadiusArcsec * 1.25;
    const FixedStarMatchResult matchResult = solveGlobalFixedStarMatch(candidatesByStar,
                                                                        frame.detectedStars.size(),
                                                                        stars.size(),
                                                                        unmatchedPenaltyCost);
    for (int i = 0; i < stars.size(); ++i)
    {
        if (!inFrameList[i] || !matchResult.matched[i])
            continue;
        const int detectedIndex = matchResult.matchedDetectedIndex[i];
        if (detectedIndex < 0 || detectedIndex >= frame.detectedStars.size())
            continue;
        detectedList[i] = frame.detectedStars[detectedIndex];
        bestDistancePxList[i] = matchResult.matchedDistancePx[i];
        bestDistanceArcsecList[i] = matchResult.matchedDistanceArcsec[i];
        matchedList[i] = true;
    }

    for (int i = 0; i < stars.size(); ++i)
    {
        const FixedPoleStar &star = stars[i];
        QJsonObject obj;
        obj["id"] = star.id;
        obj["name"] = star.name;
        obj["raDeg"] = star.raDeg;
        obj["decDeg"] = star.decDeg;
        obj["mag"] = star.mag;
        obj["expected"] = pointJson(expectedList[i]);
        obj["detected"] = pointJson(detectedList[i]);
        obj["matched"] = matchedList[i];
        obj["visible"] = inFrameList[i];
        obj["distancePx"] = matchedList[i] ? bestDistancePxList[i] : -1.0;
        obj["distanceArcsec"] = matchedList[i] ? bestDistanceArcsecList[i] : -1.0;
        obj["matchRadiusPx"] = kFixedStarMatchRadiusPx;
        obj["matchRadiusArcsec"] = matchRadiusArcsec;
        arr.append(obj);
    }

    return arr;
}

QJsonArray PoleMasterAlignmentSimulation::generatedRotationSamplesToJson() const
{
    QJsonArray arr;
    for (int i = 0; i < generatedSolveFrames.size(); ++i)
    {
        QJsonObject obj;
        obj["index"] = i + 1;
        obj["pole"] = pointJson(generatedSolveFrames[i].truePolePx);
        obj["stars"] = generatedPointsToJson(generatedSolveFrames[i].detectedStars, 24);
        arr.append(obj);
    }
    return arr;
}

QJsonObject PoleMasterAlignmentSimulation::generatedQualityJson(const SimSolvedFrame *frame, const QJsonObject &extra) const
{
    QJsonObject quality = extra;
    if (frame != nullptr)
    {
        quality["starCount"] = frame->detectedStars.size();
        quality["pixelScaleArcsecPerPixel"] = frame->pixelScaleArcsecPerPixel;
        quality["fitsPath"] = QFileInfo(frame->fitsPath).fileName();
        quality["plateSolved"] = frame->plateSolved;
    }
    quality["axisRadiusPx"] = generatedAxisRadiusPx;
    quality["axisResidualPx"] = generatedAxisResidualPx;
    quality["lastRaRotationDeg"] = generatedSolveFrames.size() >= 2 ? 35.0 : 0.0;
    quality["method"] = frame && frame->plateSolved ? "fits-plate-solve" : "embedded-wcs-fallback";
    quality["exposureMs"] = lastGeneratedExposureMs;
    return quality;
}

QJsonObject PoleMasterAlignmentSimulation::buildGeneratedOverlay(const QString &phase, const SimSolvedFrame *frame, const QJsonObject &extra) const
{
    QJsonObject root;
    root["phase"] = phase;
    root["imageW"] = frame ? frame->imageW : imageSize.width();
    root["imageH"] = frame ? frame->imageH : imageSize.height();
    root["hemisphere"] = "north";
    if (frame != nullptr)
    {
        const QJsonArray fixedStars = generatedFixedStarsToJson(*frame);
        int fixedMatchedCount = 0;
        for (const QJsonValue &value : fixedStars)
        {
            if (value.toObject().value("matched").toBool(false))
                fixedMatchedCount++;
        }
        QJsonObject qualityExtra = extra;
        qualityExtra["fixedStarCount"] = fixedStars.size();
        qualityExtra["fixedStarMatchedCount"] = fixedMatchedCount;
        root["frameId"] = frame->frameId;
        root["fixedStars"] = fixedStars;
        root["detectedStars"] = generatedPointsToJson(frame->detectedStars, 120);
        root["quality"] = generatedQualityJson(frame, qualityExtra);
    }
    else
    {
        root["quality"] = generatedQualityJson(nullptr, extra);
    }
    root["rotationSamples"] = generatedRotationSamplesToJson();
    QJsonObject axis = pointJson(lastGeneratedAxis);
    axis["radiusPx"] = generatedAxisRadiusPx;
    axis["residualPx"] = generatedAxisResidualPx;
    root["axisCandidate"] = axis;
    root["warnings"] = QJsonArray();
    return root;
}

bool PoleMasterAlignmentSimulation::generateFrameImageFromScript(QString &fileName, QString *failureReason, int exposureOverrideMs)
{
    fileName.clear();
    auto setFailureReason = [failureReason](const QString &reason)
    {
        if (failureReason != nullptr)
            *failureReason = reason;
    };

    const QString scriptPath = simulationScriptPath();
    if (scriptPath.isEmpty())
    {
        Logger::Log("PoleMasterAlignmentSimulation: simulation script not found",
                    LogLevel::WARNING,
                    DeviceType::MAIN);
        setFailureReason("simulation script not found");
        return false;
    }

    const int exposureMs = exposureOverrideMs > 0 ? exposureOverrideMs : simulationExposureMs();
    lastGeneratedExposureMs = exposureMs;
    const QString generatedFileName = QString("PoleMasterSim_%1.jpg").arg(frameIndex, 4, 10, QLatin1Char('0'));
    const QString generatedFilePath = QDir(imageRootPath).filePath(generatedFileName);

    auto generateFitsWithExposure = [&](int exposureForAttempt, QString &outFitsPath) -> bool
    {
        QStringList args;
        args << scriptPath
             << "--out-dir" << imageRootPath
             << "--fits-dir" << "/dev/shm"
             << "--fits-name" << "polecamera.fits"
             << "--frame-index" << QString::number(frameIndex)
             << "--script-index" << QString::number(scriptIndex)
             << "--state-number" << QString::number(static_cast<int>(currentState))
             << "--exposure-ms" << QString::number(exposureForAttempt)
             << "--hemisphere" << "north"
             << "--width" << QString::number(imageSize.width())
             << "--height" << QString::number(imageSize.height())
             << "--axis-x" << QString::number(axisCenter.x(), 'f', 3)
             << "--axis-y" << QString::number(axisCenter.y(), 'f', 3);

        QProcess proc;
        proc.start("python3", args);
        if (!proc.waitForStarted(3000))
        {
            proc.start("python", args);
            if (!proc.waitForStarted(3000))
            {
                setFailureReason("failed to start python interpreter");
                return false;
            }
        }
        if (!proc.waitForFinished(20000))
        {
            proc.kill();
            Logger::Log("PoleMasterAlignmentSimulation: simulation script timeout: " + scriptPath.toStdString(),
                        LogLevel::WARNING,
                        DeviceType::MAIN);
            setFailureReason("simulation script timeout");
            return false;
        }
        const QByteArray stdoutBytes = proc.readAllStandardOutput();
        const QByteArray stderrBytes = proc.readAllStandardError();
        if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        {
            const QString err = QString::fromUtf8(stderrBytes).trimmed();
            Logger::Log("PoleMasterAlignmentSimulation: simulation script failed: " +
                            err.left(512).toStdString(),
                        LogLevel::WARNING,
                        DeviceType::MAIN);
            setFailureReason("simulation script failed: " + err);
            return false;
        }
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(stdoutBytes.trimmed(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        {
            Logger::Log("PoleMasterAlignmentSimulation: simulation script JSON parse failed: " +
                            parseError.errorString().toStdString(),
                        LogLevel::WARNING,
                        DeviceType::MAIN);
            setFailureReason("simulation script JSON parse failed: " + parseError.errorString());
            return false;
        }

        const QJsonObject root = doc.object();
        if (!stderrBytes.trimmed().isEmpty())
        {
            const QString stderrText = QString::fromUtf8(stderrBytes).trimmed();
            Logger::Log("PoleMasterAlignmentSimulation: simulation script stderr: " +
                            stderrText.left(512).toStdString(),
                        LogLevel::INFO,
                        DeviceType::MAIN);
        }
        outFitsPath = root.value("fitsPath").toString().trimmed();
        if (outFitsPath.isEmpty() || !QFileInfo::exists(outFitsPath))
        {
            const QJsonObject overlayObj = root.value("overlay").toObject();
            const QJsonArray warnings = overlayObj.value("warnings").toArray();
            QStringList warningTexts;
            warningTexts.reserve(warnings.size());
            for (const QJsonValue &warning : warnings)
            {
                const QString text = warning.toString().trimmed();
                if (!text.isEmpty())
                    warningTexts.push_back(text);
            }
            const QString warningJoined = warningTexts.isEmpty()
                                              ? QStringLiteral("<none>")
                                              : warningTexts.join(" | ");
            const QString fitsFileName = root.value("fitsFileName").toString().trimmed();
            Logger::Log("PoleMasterAlignmentSimulation: invalid fitsPath from simulation script: " +
                            outFitsPath.toStdString() +
                            " fitsFileName=" + fitsFileName.toStdString() +
                            " expectedFitsPath=/dev/shm/polecamera.fits" +
                            " warnings=" + warningJoined.toStdString() +
                            " stdout=" + QString::fromUtf8(stdoutBytes).left(512).toStdString(),
                        LogLevel::WARNING,
                        DeviceType::MAIN);
            setFailureReason("invalid fitsPath from simulation script");
            return false;
        }
        int previewW = 0;
        int previewH = 0;
        if (!buildPreviewJpgFromFits(outFitsPath, generatedFilePath, previewW, previewH))
        {
            Logger::Log("PoleMasterAlignmentSimulation: failed to build preview JPG from FITS " +
                            outFitsPath.toStdString(),
                        LogLevel::WARNING,
                        DeviceType::MAIN);
            setFailureReason("failed to build preview JPG from FITS");
            return false;
        }
        return true;
    };

    const bool guidancePhase = currentState == PoleMasterAlignmentState::GUIDING_ADJUSTMENT;
    SimSolvedFrame solvedFrame;
    bool solved = false;
    int attemptExposureMs = exposureMs;
    QString currentFitsPath;
    if (!guidancePhase)
    {
        for (int attempt = 0; attempt <= kMaxExposureRetries; ++attempt)
        {
            lastGeneratedExposureMs = attemptExposureMs;
            if (!generateFitsWithExposure(attemptExposureMs, currentFitsPath))
                return false;
            if (solveGeneratedFits(currentFitsPath, solvedFrame))
            {
                solved = true;
                break;
            }
            if (attempt >= kMaxExposureRetries)
                break;
            attemptExposureMs = std::min(8000, attemptExposureMs + 500);
            Logger::Log("PoleMasterAlignmentSimulation: solveGeneratedFits failed, regenerate FITS with higher exposure. fits=" +
                            currentFitsPath.toStdString() +
                            " retry=" + std::to_string(attempt + 1) +
                            "/" + std::to_string(kMaxExposureRetries) +
                            " exposureMs=" + std::to_string(attemptExposureMs),
                        LogLevel::WARNING,
                        DeviceType::MAIN);
        }
        if (!solved)
        {
            Logger::Log("PoleMasterAlignmentSimulation: solveGeneratedFits failed after exposure retries. fits=" +
                            currentFitsPath.toStdString() +
                            " reason=plate solve failed",
                        LogLevel::ERROR,
                        DeviceType::MAIN);
            setFailureReason("plate solve failed after exposure retries");
            return false;
        }
    }
    else
    {
        if (!generateFitsWithExposure(attemptExposureMs, currentFitsPath))
            return false;

        int imageW = 0;
        int imageH = 0;
        if (!readGeneratedFitsSize(currentFitsPath, imageW, imageH))
        {
            setFailureReason("failed to read generated FITS size");
            return false;
        }

        solvedFrame.fitsPath = currentFitsPath;
        solvedFrame.imageW = imageW;
        solvedFrame.imageH = imageH;
        solvedFrame.pixelScaleArcsecPerPixel = std::max(0.001, lastGeneratedPixelScaleArcsecPerPixel);
        solvedFrame.truePolePx = currentPolePosition();
        solvedFrame.frameId = QFileInfo(currentFitsPath).completeBaseName();
        solvedFrame.plateSolved = false;
        enrichGeneratedFrameDiagnostics(solvedFrame);

        const int starCount = solvedFrame.detectedStars.size();
        if (starCount < kSimGuidanceStarCountLow)
        {
            adaptiveGuidanceExposureMs = std::min(kSimGuidanceExposureMaxMs,
                                                  adaptiveGuidanceExposureMs + kSimGuidanceExposureUpStepMs);
        }
        else if (starCount > kSimGuidanceStarCountHigh)
        {
            adaptiveGuidanceExposureMs = std::max(kSimGuidanceExposureMinMs,
                                                  adaptiveGuidanceExposureMs - kSimGuidanceExposureDownStepMs);
        }
    }

    if (currentState == PoleMasterAlignmentState::FIRST_CAPTURE ||
        currentState == PoleMasterAlignmentState::SECOND_CAPTURE ||
        currentState == PoleMasterAlignmentState::THIRD_CAPTURE)
    {
        generatedSolveFrames.append(solvedFrame);
    }
    lastGeneratedAxis = axisCenter;
    lastGeneratedPole = solvedFrame.truePolePx;
    lastGeneratedPixelScaleArcsecPerPixel = solvedFrame.pixelScaleArcsecPerPixel;
    const QJsonObject overlay = buildGeneratedOverlay(
        currentState == PoleMasterAlignmentState::GUIDING_ADJUSTMENT ? "guiding" : "simulation",
        &solvedFrame);
    lastGeneratedOverlayJson = QString::fromUtf8(QJsonDocument(overlay).toJson(QJsonDocument::Compact));

    fileName = generatedFileName;
    frameIndex++;
    return true;
}

QString PoleMasterAlignmentSimulation::generateFrameImage()
{
    QString scriptedFileName;
    QString failureReason;
    if (generateFrameImageFromScript(scriptedFileName, &failureReason, -1))
        return scriptedFileName;
    Logger::Log("PoleMasterAlignmentSimulation: frame generation failed and fallback disabled. reason=" +
                    failureReason.left(256).toStdString(),
                LogLevel::ERROR,
                DeviceType::MAIN);
    return QString();
}

int PoleMasterAlignmentSimulation::simulationExposureMs() const
{
    // 标定阶段保持长曝光以保障模拟板解稳定；调整阶段按星点数量自适应曝光。
    if (currentState == PoleMasterAlignmentState::GUIDING_ADJUSTMENT)
        return adaptiveGuidanceExposureMs;
    return 8000;
}

QString PoleMasterAlignmentSimulation::buildHint(double dx, double dy) const
{
    const QString horizontal = std::fabs(dx) < 2.0 ? QString() : (dx > 0.0 ? "向右调方位" : "向左调方位");
    const QString vertical = std::fabs(dy) < 2.0 ? QString() : (dy > 0.0 ? "降低高度" : "升高高度");
    if (horizontal.isEmpty() && vertical.isEmpty()) return "保持当前位置";
    if (horizontal.isEmpty()) return vertical;
    if (vertical.isEmpty()) return horizontal;
    return horizontal + "/" + vertical;
}

QPointF PoleMasterAlignmentSimulation::currentPolePosition() const
{
    const QVector<QPointF> &path = simulationPolePath();
    const int i = std::clamp(scriptIndex, 0, path.size() - 1);
    return path[i];
}

QVector<QPointF> PoleMasterAlignmentSimulation::currentTemplateStarPositions() const
{
    const QPointF pole = currentPolePosition();
    return {
        pole + QPointF(-92.0, -18.0),
        pole + QPointF(28.0, -110.0),
        pole + QPointF(116.0, -24.0),
        pole + QPointF(58.0, 94.0),
        pole + QPointF(-84.0, 92.0)
    };
}

QPointF PoleMasterAlignmentSimulation::currentStarImagePosition(const SimStar &star) const
{
    const QPointF pole = currentPolePosition();
    const QPointF offset = pole - simulationPolePath().first();
    const double roll = (scriptIndex - 3) * 0.008;
    const double cosR = std::cos(roll);
    const double sinR = std::sin(roll);
    const QPointF centered = star.position - axisCenter;
    const QPointF rotated(centered.x() * cosR - centered.y() * sinR,
                          centered.x() * sinR + centered.y() * cosR);
    return axisCenter + rotated + offset * 0.42;
}

QVector<PoleMasterAlignmentSimulation::SimStar> PoleMasterAlignmentSimulation::fixedStars()
{
    return {
        {{96, 88}, 6.7}, {{168, 138}, 8.8}, {{242, 74}, 7.9}, {{341, 116}, 6.2},
        {{458, 90}, 8.4}, {{607, 112}, 7.1}, {{735, 72}, 8.1}, {{886, 132}, 6.6},
        {{104, 248}, 8.5}, {{218, 224}, 6.9}, {{319, 289}, 7.8}, {{432, 238}, 8.9},
        {{548, 277}, 6.4}, {{673, 232}, 7.5}, {{813, 292}, 8.2}, {{944, 239}, 7.0},
        {{82, 412}, 7.7}, {{197, 365}, 8.6}, {{286, 446}, 6.5}, {{392, 392}, 7.4},
        {{505, 438}, 8.7}, {{619, 373}, 6.8}, {{746, 431}, 8.0}, {{873, 384}, 7.2},
        {{973, 466}, 8.4}, {{143, 581}, 6.6}, {{251, 518}, 8.3}, {{356, 633}, 7.6},
        {{487, 556}, 8.1}, {{578, 647}, 6.3}, {{696, 534}, 7.9}, {{802, 612}, 8.8},
        {{928, 557}, 7.1}, {{73, 704}, 8.2}, {{424, 713}, 7.3}, {{744, 721}, 6.9}
    };
}
