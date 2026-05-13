#include "polemasterpolaralignment.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <fitsio.h>

#include "Logger.h"

namespace {
constexpr double kTruePoleRaDeg = 0.0;
constexpr double kTruePoleDecDeg = 89.9999;

bool isFinitePoint(const QPointF &p)
{
    return std::isfinite(p.x()) && std::isfinite(p.y());
}
}

PoleMasterPolarAlignment::PoleMasterPolarAlignment(MyClient *indiServer,
                                                   INDI::BaseDevice *dpMount,
                                                   INDI::BaseDevice *dpPoleCamera,
                                                   bool useSdkCaptureSource,
                                                   QObject *parent)
    : QObject(parent)
    , indiServer(indiServer)
    , dpMount(dpMount)
    , dpPoleCamera(dpPoleCamera)
    , useSdkCaptureSource(useSdkCaptureSource)
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
    if (indiServer == nullptr || dpMount == nullptr)
    {
        Logger::Log("PoleMasterPolarAlignment: mount or INDI client unavailable", LogLevel::ERROR, DeviceType::MAIN);
        setState(PoleMasterAlignmentState::FAILED, "电子极轴镜校准启动失败：赤道仪不可用", 0);
        return false;
    }
    if (!useSdkCaptureSource && dpPoleCamera == nullptr)
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
    axisCenterPx = QPointF(-1.0, -1.0);
    axisRadiusPx = 0.0;
    stableFrames = 0;
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

    switch (currentState)
    {
    case PoleMasterAlignmentState::INITIALIZING:
        setState(PoleMasterAlignmentState::FIRST_CAPTURE, "拍摄并解析第一帧星场", 10);
        break;
    case PoleMasterAlignmentState::FIRST_CAPTURE:
    {
        SolveFrame frame;
        if (!captureAndSolve(frame))
        {
            setState(PoleMasterAlignmentState::FAILED, "第一帧解析失败", 10);
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
        if (!captureAndSolve(frame))
        {
            setState(PoleMasterAlignmentState::FAILED, "第二帧解析失败", 38);
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
        if (!captureAndSolve(frame))
        {
            setState(PoleMasterAlignmentState::FAILED, "第三帧解析失败", 62);
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
            setState(PoleMasterAlignmentState::GUIDING_ADJUSTMENT, "进入极轴镜实时调整", 82);
        break;
    case PoleMasterAlignmentState::GUIDING_ADJUSTMENT:
    {
        SolveFrame frame;
        if (captureAndSolve(frame))
        {
            emitCurrentGuide(frame);
        }
        else
        {
            Logger::Log("PoleMasterPolarAlignment: guidance frame solve failed, retrying", LogLevel::WARNING, DeviceType::MAIN);
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
    if (!captureImage(currentState == PoleMasterAlignmentState::GUIDING_ADJUSTMENT
                          ? config.guidanceExposureTime
                          : config.defaultExposureTime))
    {
        return false;
    }
    if (!waitForCaptureComplete()) return false;
    if (!solveImage(lastCapturedImage)) return false;
    return readSolveFrame(lastCapturedImage, frame);
}

bool PoleMasterPolarAlignment::captureImage(int exposureMs)
{
    captureEnded = false;
    latestCapturePathFromHost.clear();
    lastCapturedImage.clear();

    Logger::Log("PoleMasterPolarAlignment: request capture exposureMs=" + std::to_string(std::max(1, exposureMs)),
                LogLevel::INFO,
                DeviceType::MAIN);

    // Route both INDI and SDK captures through MainWindow so the existing
    // PoleCamera callback/pending flag remains the single capture-complete path.
    emit requestCaptureForRole("PoleCamera", std::max(1, exposureMs));
    return true;
}

bool PoleMasterPolarAlignment::waitForCaptureComplete()
{
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
    const bool ok = Tools::PlateSolve(fitsPath,
                                      config.focalLength,
                                      config.cameraWidth,
                                      config.cameraHeight,
                                      false,
                                      1);
    if (!ok) return false;
    QCoreApplication::processEvents();
    return Tools::isSolveImageFinish();
}

bool PoleMasterPolarAlignment::readSolveFrame(const QString &fitsPath, SolveFrame &frame) const
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
    if (!skyToPixel(wcsPath, kTruePoleRaDeg, kTruePoleDecDeg, polePx)) return false;

    frame.fitsPath = fitsPath;
    frame.wcsPath = wcsPath;
    frame.imageW = imageW;
    frame.imageH = imageH;
    frame.pixelScaleArcsecPerPixel = solveResult.pixelScaleArcsecPerPixel > 0.0
                                         ? solveResult.pixelScaleArcsecPerPixel
                                         : 1.0;
    frame.truePolePx = polePx;
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

bool PoleMasterPolarAlignment::moveRaAxis(double angleDeg)
{
    if (indiServer == nullptr || dpMount == nullptr) return false;
    double raHours = 0.0;
    double decDeg = 0.0;
    indiServer->getTelescopeRADECJNOW(dpMount, raHours, decDeg);
    double raDeg = Tools::HourToDegree(raHours);
    raDeg = std::fmod(raDeg + angleDeg, 360.0);
    if (raDeg < 0.0) raDeg += 360.0;
    indiServer->slewTelescopeJNowNonBlock(dpMount, Tools::DegreeToHour(raDeg), decDeg, true);
    return true;
}

bool PoleMasterPolarAlignment::waitForMountIdle() const
{
    if (indiServer == nullptr || dpMount == nullptr) return false;
    QElapsedTimer timer;
    timer.start();
    QString status;
    while (timer.elapsed() < config.movementTimeoutMs)
    {
        indiServer->getTelescopeStatus(dpMount, status);
        if (status == "Idle") return true;
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
    if (!fitCircle3Points(calibrationFrames[0].truePolePx,
                          calibrationFrames[1].truePolePx,
                          calibrationFrames[2].truePolePx,
                          center,
                          radius))
    {
        return false;
    }
    if (!isFinitePoint(center) || !std::isfinite(radius) || radius < 5.0)
        return false;

    axisCenterPx = center;
    axisRadiusPx = radius;
    Logger::Log("PoleMasterPolarAlignment: axis center fitted x=" + std::to_string(center.x()) +
                    " y=" + std::to_string(center.y()) +
                    " radius=" + std::to_string(radius),
                LogLevel::INFO,
                DeviceType::MAIN);
    emitCurrentGuide(calibrationFrames.last());
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
