#include "mainwindow_command_support.h"

void MainWindow::initializeBuiltInGuiderRuntime()
{
    // 导星相机循环曝光（INDI 直出图）：使用 singleShot，收到一帧后再调度下一帧，避免重入
    guiderLoopTimer = new QTimer(this);
    guiderLoopTimer->setSingleShot(true);
    connect(guiderLoopTimer, &QTimer::timeout, this, &MainWindow::onGuiderLoopTimeout);
    guiderCoreThread = new QThread(this);
    guiderCoreThread->setObjectName(QStringLiteral("GuiderCoreThread"));
    qRegisterMetaType<cv::Mat>("cv::Mat");
    qRegisterMetaType<QVector<QPointF>>("QVector<QPointF>");
    qRegisterMetaType<QVector<QString>>("QVector<QString>");
    qRegisterMetaType<QPointF>("QPointF");
    guiderCore = new GuiderCore();
    guiderParamsCache = guiderCore->params();
    {
        std::unordered_map<std::string, std::string> config;
        Tools::readClientSettings("config/config.ini", config);
        const auto it = config.find("GuiderExposureMs");
        if (it != config.end())
        {
            bool ok = false;
            const int savedGuiderExpMs = QString::fromStdString(it->second).trimmed().toInt(&ok);
            if (ok && savedGuiderExpMs > 0)
            {
                guiderExpMs = savedGuiderExpMs;
                guiderParamsCache.exposureMs = savedGuiderExpMs;
                guiderCore->setParams(guiderParamsCache);
                Logger::Log("BuiltInGuider | restored GuiderExposureMs=" +
                                std::to_string(savedGuiderExpMs),
                            LogLevel::INFO, DeviceType::GUIDER);
            }
        }
    }
    guiderCoreStateCache = guiderCore->state();
    guiderCore->moveToThread(guiderCoreThread);
    connect(guiderCoreThread, &QThread::finished, guiderCore, &QObject::deleteLater);
    guiderCoreThread->start();
    Logger::Log("BuiltInGuider | GuiderCore moved to GuiderCoreThread",
                LogLevel::INFO, DeviceType::GUIDER);
    syncGuiderScaleParams(true, false);
    publishGuiderSearchBoxMode(false);
#if QUARCS_SIM_GUIDER
    simGuiderFrameSource = std::make_unique<guiding::SimGuiderFrameSource>();
    Logger::Log("BuiltInGuider | simulated frame source enabled", LogLevel::INFO, DeviceType::GUIDER);
#endif
    connect(guiderCore, &GuiderCore::requestExposure, this, [this](int exposureMs) {
        guiderExpMs = std::max(1, exposureMs);
        isGuiderLoopExp = true;
        guiderExposureInFlight = false;
        emit wsThread->sendMessageToClient("GuiderLoopExpStatus:true");
        emit wsThread->sendMessageToClient("GuiderUpdateStatus:1");
        if (guiderLoopTimer)
            guiderLoopTimer->start(0);
    });
    connect(guiderCore, &GuiderCore::requestPersistGuidingFits, this, &MainWindow::PersistGuidingFits);
    connect(guiderCore, &GuiderCore::requestPersistGuidingFitsAnnotated, this,
            [this](const QString& sourceFitsPath, const cv::Mat& image16, int imageW, int imageH,
                   const QVector<QPointF>& dedupCandidates,
                   const QVector<QPointF>& snrCandidates,
                   const QVector<QPointF>& candidates,
                   const QVector<QString>& candidateLabels,
                   const QPointF& selected) {
        m_debugStarDedupCandidates = dedupCandidates;
        m_debugStarSnrCandidates = snrCandidates;
        m_debugStarCandidates = candidates;
        m_debugStarCandidateLabels = candidateLabels;
        m_debugStarSelected = selected;
        Logger::Log("BuiltInGuider | requestPersistGuidingFitsAnnotated received in MainWindow: image=" +
                        std::to_string(imageW) + "x" + std::to_string(imageH) +
                        " dedupCandidates=" + std::to_string(dedupCandidates.size()) +
                        " snrCandidates=" + std::to_string(snrCandidates.size()) +
                        " candidates=" + std::to_string(candidates.size()) +
                        " selected=" + std::to_string((selected.x() != 0.0 || selected.y() != 0.0) ? 1 : 0),
                    LogLevel::INFO, DeviceType::GUIDER);
        PersistGuidingPreviewFromFrame(sourceFitsPath, image16);
    }, Qt::BlockingQueuedConnection);
    connect(guiderCore, &GuiderCore::requestPulse, this, [this](const guiding::PulseCommand& cmd) {
        ControlGuideEx(static_cast<int>(cmd.dir), cmd.durationMs, QStringLiteral("BuiltInGuider"));
    });
    connect(guiderCore, &GuiderCore::lockPositionChanged, this, [this](const QPointF& lockPosPx) {
        guiderLockPosPx = lockPosPx;
        guiderLockPosValid = true;
        glPHD_CurrentImageSizeX = std::max(1, glPHD_CurrentImageSizeX);
        glPHD_CurrentImageSizeY = std::max(1, glPHD_CurrentImageSizeY);
        emit wsThread->sendMessageToClient("PHD2StarCrossView:true");
        emit wsThread->sendMessageToClient(
            "PHD2StarCrossPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
            QString::number(glPHD_CurrentImageSizeY) + ":" +
            QString::number(static_cast<int>(std::lround(lockPosPx.x()))) + ":" +
            QString::number(static_cast<int>(std::lround(lockPosPx.y()))));
    });
    connect(guiderCore, &GuiderCore::lockStarSelected, this, [this](double x, double y, double snr, double hfd) {
        const int searchHalfSizePx = std::max(4, guiderParamsCache.guideSearchHalfSizePx);
        glPHD_CurrentImageSizeX = std::max(1, glPHD_CurrentImageSizeX);
        glPHD_CurrentImageSizeY = std::max(1, glPHD_CurrentImageSizeY);
        emit wsThread->sendMessageToClient("PHD2StarBoxView:true");
        emit wsThread->sendMessageToClient(
            "PHD2StarBoxPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
            QString::number(glPHD_CurrentImageSizeY) + ":" +
            QString::number(static_cast<int>(std::lround(x))) + ":" +
            QString::number(static_cast<int>(std::lround(y))) + ":" +
            QString::number(searchHalfSizePx));
        emit wsThread->sendMessageToClient(
            "GuiderSelectedStar:" +
            QString::number(x, 'f', 2) + ":" +
            QString::number(y, 'f', 2) + ":" +
            QString::number(snr, 'f', 2) + ":" +
            QString::number(hfd, 'f', 2));
        emit wsThread->sendMessageToClient(
            "GuiderStarSelected:x=" +
            QString::number(x, 'f', 2) + ":y=" +
            QString::number(y, 'f', 2) + ":snr=" +
            QString::number(snr, 'f', 2) + ":hfd=" +
            QString::number(hfd, 'f', 2));
        emit wsThread->sendMessageToClient(
            QStringLiteral("SendDebugMessage|Info|导星选星成功：X=%1 Y=%2 SNR=%3 HFD=%4")
                .arg(x, 0, 'f', 2)
                .arg(y, 0, 'f', 2)
                .arg(snr, 0, 'f', 2)
                .arg(hfd, 0, 'f', 2));
    });
    connect(guiderCore, &GuiderCore::debugStarCandidatesChanged, this,
            [this](int imageW, int imageH, const QVector<QPointF>& dedupCandidates,
                   const QVector<QPointF>& snrCandidates,
                   const QVector<QPointF>& candidates,
                   const QVector<QString>& candidateLabels,
                   const QPointF& selected) {
        m_debugStarDedupCandidates = dedupCandidates;
        m_debugStarSnrCandidates = snrCandidates;
        m_debugStarCandidates = candidates;
        m_debugStarCandidateLabels = candidateLabels;
        m_debugStarSelected = selected;
        Logger::Log("BuiltInGuider | debugStarCandidatesChanged received in MainWindow: image=" +
                        std::to_string(imageW) + "x" + std::to_string(imageH) +
                        " dedupCandidates=" + std::to_string(dedupCandidates.size()) +
                        " snrCandidates=" + std::to_string(snrCandidates.size()) +
                        " candidates=" + std::to_string(candidates.size()) +
                        " selected=" + std::to_string((selected.x() != 0.0 || selected.y() != 0.0) ? 1 : 0),
                    LogLevel::INFO, DeviceType::GUIDER);

        emit wsThread->sendMessageToClient("ClearGuiderDebugCandidates");

        const int safeImageW = std::max(1, imageW);
        const int safeImageH = std::max(1, imageH);
        const int maxCandidatesToSend = 48;
        for (int i = 0; i < dedupCandidates.size() && i < maxCandidatesToSend; ++i)
        {
            const auto& pt = dedupCandidates[i];
            emit wsThread->sendMessageToClient(
                "GuiderDebugDedupCandidatePosition:" + QString::number(safeImageW) + ":" +
                QString::number(safeImageH) + ":" +
                QString::number(static_cast<int>(std::lround(pt.x()))) + ":" +
                QString::number(static_cast<int>(std::lround(pt.y()))));
        }

        for (int i = 0; i < snrCandidates.size() && i < maxCandidatesToSend; ++i)
        {
            const auto& pt = snrCandidates[i];
            emit wsThread->sendMessageToClient(
                "GuiderDebugSnrCandidatePosition:" + QString::number(safeImageW) + ":" +
                QString::number(safeImageH) + ":" +
                QString::number(static_cast<int>(std::lround(pt.x()))) + ":" +
                QString::number(static_cast<int>(std::lround(pt.y()))));
        }

        for (int i = 0; i < candidates.size() && i < maxCandidatesToSend; ++i)
        {
            const auto& pt = candidates[i];
            const QString label = (i < candidateLabels.size()) ? candidateLabels[i] : QString();
            emit wsThread->sendMessageToClient(
                "GuiderDebugCandidatePosition:" + QString::number(safeImageW) + ":" +
                QString::number(safeImageH) + ":" +
                QString::number(static_cast<int>(std::lround(pt.x()))) + ":" +
                QString::number(static_cast<int>(std::lround(pt.y()))));
            emit wsThread->sendMessageToClient(
                "GuiderDebugFinalCandidatePosition:" + QString::number(safeImageW) + ":" +
                QString::number(safeImageH) + ":" +
                QString::number(static_cast<int>(std::lround(pt.x()))) + ":" +
                QString::number(static_cast<int>(std::lround(pt.y()))) + ":" +
                label);
        }

        if (selected.x() != 0.0 || selected.y() != 0.0)
        {
            emit wsThread->sendMessageToClient(
                "GuiderDebugSelectedCandidatePosition:" + QString::number(safeImageW) + ":" +
                QString::number(safeImageH) + ":" +
                QString::number(static_cast<int>(std::lround(selected.x()))) + ":" +
                QString::number(static_cast<int>(std::lround(selected.y()))));
        }
    });
    connect(guiderCore, &GuiderCore::guideStarCentroidChanged, this, [this](const QPointF& centroidPx) {
        const int searchHalfSizePx = std::max(4, guiderParamsCache.guideSearchHalfSizePx);
        guiderGuideStarCentroidPx = centroidPx;
        guiderGuideStarCentroidValid = true;
        glPHD_CurrentImageSizeX = std::max(1, glPHD_CurrentImageSizeX);
        glPHD_CurrentImageSizeY = std::max(1, glPHD_CurrentImageSizeY);
        emit wsThread->sendMessageToClient("PHD2StarBoxView:true");
        emit wsThread->sendMessageToClient(
            "PHD2StarBoxPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
            QString::number(glPHD_CurrentImageSizeY) + ":" +
            QString::number(static_cast<int>(std::lround(centroidPx.x()))) + ":" +
            QString::number(static_cast<int>(std::lround(centroidPx.y()))) + ":" +
            QString::number(searchHalfSizePx));
    });
    connect(guiderCore, &GuiderCore::multiStarSecondaryPointsChanged, this, [this](const QVector<QPointF>& ptsPx) {
        guiderMultiStarSecondaryPtsPx = ptsPx;
        guiderMultiStarSecondaryPtsPending = !ptsPx.isEmpty();

        emit wsThread->sendMessageToClient("ClearPHD2MultiStars");
        if (ptsPx.isEmpty())
            return;

        if (glPHD_CurrentImageSizeX <= 0 || glPHD_CurrentImageSizeY <= 0 || !guiderPhaseGuiding || guiderDirectionDetectActive)
            return;

        for (int i = 0; i < ptsPx.size(); ++i)
        {
            if (i >= 8) break;
            const auto& p = ptsPx[i];
            emit wsThread->sendMessageToClient(
                "PHD2MultiStarsPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
                QString::number(glPHD_CurrentImageSizeY) + ":" +
                QString::number(static_cast<int>(std::lround(p.x()))) + ":" +
                QString::number(static_cast<int>(std::lround(p.y()))));
        }
        guiderMultiStarSecondaryPtsPending = false;
    });
    connect(guiderCore, &GuiderCore::directionDetectionStateChanged, this, [this](bool active) {
        guiderDirectionDetectActive = active;
        emit wsThread->sendMessageToClient(QString("GuiderStatus:%1")
                                               .arg(active ? "InDirectionDetection"
                                                           : (guiderPhaseGuiding ? "InGuiding" : "InCalibration")));
    });
    connect(guiderCore, &GuiderCore::guideErrorUpdated, this, [this](double raErrPx, double decErrPx) {
        const double scale = currentGuiderArcsecPerPixel();
        const double raErr = (scale > 0.0) ? (raErrPx * scale) : raErrPx;
        const double decErr = (scale > 0.0) ? (decErrPx * scale) : decErrPx;
        emit wsThread->sendMessageToClient(
            "AddLineChartData:" + QString::number(guiderChartSampleIndex++) + ":" +
            QString::number(raErr, 'f', 6) + ":" +
            QString::number(decErr, 'f', 6));
        emit wsThread->sendMessageToClient(
            "AddScatterChartData:" + QString::number(raErr, 'f', 6) + ":" +
            QString::number(-decErr, 'f', 6));
    });
    connect(guiderCore, &GuiderCore::guidePulseIssued, this,
            [this](const guiding::PulseCommand& cmd, double raErrPx, double decErrPx) {
        const QString dir =
            (cmd.dir == guiding::GuideDir::North) ? QStringLiteral("NORTH") :
            (cmd.dir == guiding::GuideDir::South) ? QStringLiteral("SOUTH") :
            (cmd.dir == guiding::GuideDir::East)  ? QStringLiteral("EAST")  :
                                                    QStringLiteral("WEST");
        emit wsThread->sendMessageToClient(
            "GuiderPulse:" + dir + ":" + QString::number(cmd.durationMs) +
            ":raErrPx=" + QString::number(raErrPx, 'f', 6) +
            ":decErrPx=" + QString::number(decErrPx, 'f', 6));
    });
    connect(guiderCore, &GuiderCore::calibrationResultChanged, this, [this](const guiding::CalibrationResult& r) {
        emit wsThread->sendMessageToClient(
            QStringLiteral("GuiderCalibration:cameraAngleDeg=") + QString::number(r.cameraAngleDeg, 'f', 2) +
            ":orthoErrDeg=" + QString::number(r.orthoErrDeg, 'f', 2) +
            ":raMsPerPixel=" + QString::number(r.raMsPerPixel, 'f', 2) +
            ":decMsPerPixel=" + QString::number(r.decMsPerPixel, 'f', 2) +
            ":raSteps=" + QString::number(r.raStepCount) +
            ":decSteps=" + QString::number(r.decStepCount) +
            ":raTravelPx=" + QString::number(r.raTravelPx, 'f', 2) +
            ":decTravelPx=" + QString::number(r.decTravelPx, 'f', 2));
    });
    connect(guiderCore, &GuiderCore::stateChanged, this, [this](guiding::State state) {
        guiderCoreStateCache = state;
        guiderPhaseGuiding = (state == guiding::State::Guiding);
        if (state == guiding::State::Idle || state == guiding::State::Looping ||
            state == guiding::State::Stopped || state == guiding::State::Error)
        {
            guiderDirectionDetectActive = false;
            guiderChartSampleIndex = 0;
            guiderLockPosValid = false;
            guiderGuideStarCentroidValid = false;
            isGuiderLoopExp = false;
            guiderExposureInFlight = false;
            if (guiderLoopTimer)
                guiderLoopTimer->stop();
            if (sdkGuiderExposureTimer)
                sdkGuiderExposureTimer->stop();
            emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
            emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
            emit wsThread->sendMessageToClient("PHD2StarBoxView:false");
            emit wsThread->sendMessageToClient("PHD2StarCrossView:false");
            emit wsThread->sendMessageToClient("ClearPHD2MultiStars");
            stopGuiderAutoBatchCapture();
            clearGuiderDebugAnnotations(state == guiding::State::Stopped);
        }
        emit wsThread->sendMessageToClient(QString("GuiderCoreState:%1").arg(static_cast<int>(state)));
        switch (state)
        {
        case guiding::State::Selecting:
            emit wsThread->sendMessageToClient("GuiderSwitchStatus:true");
            emit wsThread->sendMessageToClient("GuiderStatus:InSelecting");
            emit wsThread->sendMessageToClient("SendDebugMessage|Info|导星正在自动选星，请稍候");
            break;
        case guiding::State::Calibrating:
            emit wsThread->sendMessageToClient("GuiderSwitchStatus:true");
            emit wsThread->sendMessageToClient("GuiderStatus:InCalibration");
            emit wsThread->sendMessageToClient("SendDebugMessage|Info|导星已锁定星点，正在校准");
            break;
        case guiding::State::Guiding:
            emit wsThread->sendMessageToClient("GuiderSwitchStatus:true");
            emit wsThread->sendMessageToClient("GuiderStatus:InGuiding");
            emit wsThread->sendMessageToClient("SendDebugMessage|Info|导星已进入闭环导星");
            break;
        case guiding::State::Idle:
        case guiding::State::Looping:
        case guiding::State::Stopped:
        case guiding::State::Error:
        default:
            emit wsThread->sendMessageToClient("GuiderSwitchStatus:false");
            break;
        }
    });
    connect(guiderCore, &GuiderCore::infoMessage, this, [this](const QString& msg) {
        Logger::Log("BuiltInGuider | " + msg.toStdString(), LogLevel::INFO, DeviceType::GUIDER);
        emit wsThread->sendMessageToClient("GuiderCoreInfo:" + msg);
        if (msg.contains(QStringLiteral("选星成功")))
        {
            emit wsThread->sendMessageToClient("GuiderStatus:StarSelected");
            emit wsThread->sendMessageToClient("SendDebugMessage|Info|" + msg);
        }
        else if (msg.contains(QStringLiteral("自动选星进行中")) ||
                 msg.contains(QStringLiteral("等待下一帧")) ||
                 msg.contains(QStringLiteral("候选星点")))
        {
            emit wsThread->sendMessageToClient("GuiderStatus:SelectingProgress");
            emit wsThread->sendMessageToClient("SendDebugMessage|Info|" + msg);
        }
        else if (msg.contains(QStringLiteral("进入校准阶段")) ||
                 msg.contains(QStringLiteral("校准完成")))
        {
            emit wsThread->sendMessageToClient("SendDebugMessage|Info|" + msg);
        }
    });
    connect(guiderCore, &GuiderCore::errorOccurred, this, [this](const QString& msg) {
        Logger::Log("BuiltInGuider | " + msg.toStdString(), LogLevel::WARNING, DeviceType::GUIDER);
        emit wsThread->sendMessageToClient("GuiderCoreError:" + msg);
        emit wsThread->sendMessageToClient("ErrorMessage:导星错误 - " + msg);
        emit wsThread->sendMessageToClient("SendDebugMessage|Warning|导星错误：" + msg);
        if (msg.contains(QStringLiteral("LostStar"), Qt::CaseInsensitive) ||
            msg.contains(QStringLiteral("丢星"), Qt::CaseInsensitive))
        {
            emit wsThread->sendMessageToClient("GuiderStatus:StarLostAlert");
        }
    });
}

void MainWindow::startPoleCameraSingleCapture(int exposureMs)
{
    const int expMs = std::max(1, exposureMs);
    sdkGuiderExposureRole = "PoleCamera";

    if (isGuiderLoopExp)
    {
        Logger::Log("startPoleCameraSingleCapture | stopping guider loop before polar capture",
                    LogLevel::INFO, DeviceType::GUIDER);
        if (guiderCore)
        {
            postGuiderCore(guiderCore, [](GuiderCore *core) {
                core->stopGuiding();
                core->stopLoop();
            });
        }
        isGuiderLoopExp = false;
        if (guiderLoopTimer)
            guiderLoopTimer->stop();
        if (sdkGuiderExposureTimer)
            sdkGuiderExposureTimer->stop();
        emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
    }

    if (isPoleCameraSDK())
    {
        SdkSerialExecutor *poleExec = sdkPoleCameraExecutor();
        if (!poleExec || !poleExec->isRunning())
        {
            Logger::Log("startPoleCameraSingleCapture | sdkPoleCamExec is not running", LogLevel::ERROR, DeviceType::MAIN);
            polarGuiderSingleCapturePending = false;
            guiderExposureInFlight = false;
            return;
        }
        if (guiderExposureInFlight || sdkGuiderFrameTaskInFlight.load())
        {
            Logger::Log("startPoleCameraSingleCapture | exposure/readout is already in flight, retrying shortly",
                        LogLevel::WARNING, DeviceType::MAIN);
            QTimer::singleShot(250, this, [this, expMs]() {
                const bool oldPolarRunning = polarAlignment != nullptr && polarAlignment->isRunning();
                const bool poleMasterRunning = poleMasterPolarAlignment != nullptr && poleMasterPolarAlignment->isRunning();
                if ((oldPolarRunning || poleMasterRunning) &&
                    currentPolarAlignmentCameraRole == PolarAlignmentCameraRole::PoleCamera)
                {
                    startPoleCameraSingleCapture(expMs);
                }
            });
            return;
        }

        polarGuiderSingleCapturePending = true;
        guiderExposureInFlight = true;
        const double expSec = expMs / 1000.0;
        const SdkDeviceHandle handleSnap = sdkPoleScopeHandle;

        poleExec->post([this, handleSnap, expMs, expSec]() {
            auto failOnMain = [this](const std::string &message) {
                QMetaObject::invokeMethod(this, [this, message]() {
                    Logger::Log(message, LogLevel::ERROR, DeviceType::MAIN);
                    polarGuiderSingleCapturePending = false;
                    guiderExposureInFlight = false;
                }, Qt::QueuedConnection);
            };

            SdkCommand cancelCmd;
            cancelCmd.type = SdkCommandType::Custom;
            cancelCmd.name = "CancelExposure";
            cancelCmd.payload = std::any();
            SdkManager::instance().callByHandle(handleSnap, cancelCmd);

            SdkCommand setExpCmd;
            setExpCmd.type = SdkCommandType::Custom;
            setExpCmd.name = "SetExposure";
            setExpCmd.payload = expSec * 1000000.0;
            SdkResult setRes = SdkManager::instance().callByHandle(handleSnap, setExpCmd);
            if (!setRes.success)
            {
                failOnMain("startPoleCameraSingleCapture | SDK SetExposure failed: " + setRes.message);
                return;
            }

            SdkCommand startExpCmd;
            startExpCmd.type = SdkCommandType::Custom;
            startExpCmd.name = "StartSingleExposure";
            startExpCmd.payload = std::any();
            SdkResult startRes = SdkManager::instance().callByHandle(handleSnap, startExpCmd);
            if (!startRes.success)
            {
                failOnMain("startPoleCameraSingleCapture | SDK StartSingleExposure failed: " + startRes.message);
                return;
            }

            QMetaObject::invokeMethod(this, [this, expMs]() {
                sdkGuiderExposureStartTime = QDateTime::currentMSecsSinceEpoch();
                sdkGuiderExposureExpectedDuration = expMs;
                sdkGuiderExposureRole = "PoleCamera";
                if (sdkGuiderExposureTimer)
                    sdkGuiderExposureTimer->start(expMs);
            }, Qt::QueuedConnection);
        });
        return;
    }

    if (!indi_Client || dpPoleScope == nullptr || !dpPoleScope->isConnected())
    {
        Logger::Log("startPoleCameraSingleCapture | pole camera is not connected", LogLevel::ERROR, DeviceType::MAIN);
        polarGuiderSingleCapturePending = false;
        guiderExposureInFlight = false;
        return;
    }

    polarGuiderSingleCapturePending = true;
    guiderExposureInFlight = true;
    const double expSec = expMs / 1000.0;
    indi_Client->takeExposure(dpPoleScope, expSec);
}

void MainWindow::onSdkGuiderExposureTimerTimeout()
{
    if (!sdkGuiderExposureTimer)
        return;

    sdkGuiderExposureTimer->stop();

    const bool poleCapture = (sdkGuiderExposureRole == "PoleCamera");
    const bool guiderSdk =
        (!poleCapture &&
         systemdevicelist.system_devices.size() > 1 &&
         systemdevicelist.system_devices[1].isSDKConnect &&
         sdkGuiderHandle != nullptr);
    const bool poleSdk =
        (poleCapture &&
         systemdevicelist.system_devices.size() > 2 &&
         systemdevicelist.system_devices[2].isSDKConnect &&
         sdkPoleScopeHandle != nullptr);

    if ((!guiderSdk && !poleSdk) || (!isGuiderLoopExp && !polarGuiderSingleCapturePending))
    {
        guiderExposureInFlight = false;
        return;
    }

    SdkSerialExecutor *captureExec = sdkExecutorForPolarRole(poleCapture
                                                                 ? PolarAlignmentCameraRole::PoleCamera
                                                                 : PolarAlignmentCameraRole::Guider);
    if (!captureExec || !captureExec->isRunning())
    {
        Logger::Log("onSdkGuiderExposureTimerTimeout | SDK capture executor not running, stop guider polling",
                    LogLevel::ERROR, DeviceType::GUIDER);
        guiderExposureInFlight = false;
        isGuiderLoopExp = false;
        if (guiderCore)
        {
            postGuiderCore(guiderCore, [](GuiderCore *core) {
                core->stopGuiding();
                core->stopLoop();
            });
        }
        emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
        return;
    }

    if (sdkGuiderFrameTaskInFlight.exchange(true))
    {
        Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | frame task already in flight, retry timer in 10ms",
                    LogLevel::INFO, DeviceType::GUIDER);
        sdkGuiderExposureTimer->start(10);
        return;
    }

    const SdkDeviceHandle handleSnap = poleCapture ? sdkPoleScopeHandle : sdkGuiderHandle;
    const PolarAlignmentCameraRole captureRole =
        poleCapture ? PolarAlignmentCameraRole::PoleCamera : PolarAlignmentCameraRole::Guider;
    const qint64 startSnap = sdkGuiderExposureStartTime;
    const int expectedSnap = sdkGuiderExposureExpectedDuration;
    const qint64 timerFiredAtMs = QDateTime::currentMSecsSinceEpoch();

    Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | fired elapsedSinceExposureStartMs=" +
                    std::to_string(timerFiredAtMs - startSnap) +
                    " expectedMs=" + std::to_string(expectedSnap),
                LogLevel::INFO, DeviceType::GUIDER);

    captureExec->post([this, handleSnap, startSnap, expectedSnap, timerFiredAtMs, captureRole]() {
        QElapsedTimer workerPerf;
        workerPerf.start();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 elapsed = now - startSnap;
        const qint64 expected = std::max<qint64>(1, expectedSnap);
        const qint64 maxWaitMs = std::max<qint64>(expected + 5000, expected * 3);

        Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | sdkWorkerEnter queueDelayMs=" +
                        std::to_string(now - timerFiredAtMs) +
                        " elapsedSinceExposureStartMs=" + std::to_string(elapsed) +
                        " expectedMs=" + std::to_string(expected) +
                        " maxWaitMs=" + std::to_string(maxWaitMs),
                    LogLevel::INFO, DeviceType::GUIDER);

        if (elapsed > maxWaitMs)
        {
            QElapsedTimer cancelPerf;
            cancelPerf.start();
            SdkCommand cancelCmd;
            cancelCmd.type = SdkCommandType::Custom;
            cancelCmd.name = "CancelExposure";
            cancelCmd.payload = std::any();
            SdkResult cancelRes = SdkManager::instance().callByHandle(handleSnap, cancelCmd);
            const qint64 cancelCostMs = cancelPerf.elapsed();
            const qint64 workerTotalMs = workerPerf.elapsed();

            Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | timeout CancelExposure returned success=" +
                            std::to_string(cancelRes.success ? 1 : 0) +
                            " costMs=" + std::to_string(cancelCostMs) +
                            " workerTotalMs=" + std::to_string(workerTotalMs) +
                            " msg=" + cancelRes.message,
                        LogLevel::INFO, DeviceType::GUIDER);

            QMetaObject::invokeMethod(this, [this, cancelRes, elapsed, expected, maxWaitMs, cancelCostMs, workerTotalMs]() {
                sdkGuiderFrameTaskInFlight = false;
                Logger::Log("onSdkGuiderExposureTimerTimeout | TIMEOUT waiting guider frame (elapsed=" +
                                std::to_string(elapsed) + "ms, expected=" + std::to_string(expected) +
                                "ms, maxWait=" + std::to_string(maxWaitMs) +
                                "ms, cancelCost=" + std::to_string(cancelCostMs) +
                                "ms, workerTotal=" + std::to_string(workerTotalMs) + "ms)",
                            LogLevel::ERROR, DeviceType::GUIDER);
                if (!cancelRes.success)
                {
                    Logger::Log("onSdkGuiderExposureTimerTimeout | CancelExposure failed: " + cancelRes.message,
                                LogLevel::WARNING, DeviceType::GUIDER);
                }
                guiderExposureInFlight = false;
                polarGuiderSingleCapturePending = false;
                isGuiderLoopExp = false;
                if (guiderCore)
                {
                    postGuiderCore(guiderCore, [](GuiderCore *core) {
                        core->stopGuiding();
                        core->stopLoop();
                    });
                }
                emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
                emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
            }, Qt::QueuedConnection);
            return;
        }

        SdkCommand getFrameCmd;
        getFrameCmd.type = SdkCommandType::Custom;
        getFrameCmd.name = "GetSingleFrame";
        getFrameCmd.payload = std::any();
        QElapsedTimer getFramePerf;
        getFramePerf.start();
        SdkResult frameRes = SdkManager::instance().callByHandle(handleSnap, getFrameCmd);
        const qint64 getFrameCostMs = getFramePerf.elapsed();
        const qint64 workerTotalMs = workerPerf.elapsed();

        Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | GetSingleFrame returned success=" +
                        std::to_string(frameRes.success ? 1 : 0) +
                        " costMs=" + std::to_string(getFrameCostMs) +
                        " workerTotalMs=" + std::to_string(workerTotalMs) +
                        " msg=" + frameRes.message,
                    LogLevel::INFO, DeviceType::GUIDER);

        QMetaObject::invokeMethod(this, [this, frameRes, expected, getFrameCostMs, workerTotalMs, timerFiredAtMs, captureRole]() mutable {
            QElapsedTimer mainPerf;
            mainPerf.start();
            sdkGuiderFrameTaskInFlight = false;

            Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | mainThreadResultEnter totalSinceTimerFiredMs=" +
                            std::to_string(QDateTime::currentMSecsSinceEpoch() - timerFiredAtMs) +
                            " getFrameCostMs=" + std::to_string(getFrameCostMs) +
                            " workerTotalMs=" + std::to_string(workerTotalMs),
                        LogLevel::INFO, DeviceType::GUIDER);

            const bool poleCapture = (captureRole == PolarAlignmentCameraRole::PoleCamera);
            const bool captureSdk =
                poleCapture
                    ? (systemdevicelist.system_devices.size() > 2 &&
                       systemdevicelist.system_devices[2].isSDKConnect &&
                       sdkPoleScopeHandle != nullptr)
                    : (systemdevicelist.system_devices.size() > 1 &&
                       systemdevicelist.system_devices[1].isSDKConnect &&
                       sdkGuiderHandle != nullptr);
            if (!captureSdk)
            {
                guiderExposureInFlight = false;
                return;
            }

            if (frameRes.success)
            {
                SdkFrameData frame = std::any_cast<SdkFrameData>(frameRes.payload);
                const bool hasFrameData =
                    (!frame.pixels.empty()) || (frame.rawBuffer != nullptr && frame.rawBytes > 0);
                if (frame.width <= 0 || frame.height <= 0 || !hasFrameData)
                {
                    Logger::Log("onSdkGuiderExposureTimerTimeout | invalid frame, retry",
                                LogLevel::WARNING, DeviceType::GUIDER);
                }
                else
                {
                    const QString sdkGuiderFitsPath = poleCapture
                        ? QStringLiteral("/dev/shm/polecamera.fits")
                        : QStringLiteral("/dev/shm/guiding.fits");
                    QElapsedTimer saveFitsPerf;
                    saveFitsPerf.start();
                    SaveQhyFrameDataToFits(frame, sdkGuiderFitsPath.toStdString());
                    Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | SaveQhyFrameDataToFits costMs=" +
                                    std::to_string(saveFitsPerf.elapsed()) +
                                    " mainTotalMs=" + std::to_string(mainPerf.elapsed()),
                                LogLevel::INFO, DeviceType::GUIDER);

                    if (polarGuiderSingleCapturePending)
                    {
                        notifyPolarAlignmentCaptureReady(captureRole, sdkGuiderFitsPath);
                        polarGuiderSingleCapturePending = false;
                    }

                    if (!poleCapture && guiderCore)
                    {
                        QElapsedTimer invokePerf;
                        invokePerf.start();
                        QMetaObject::invokeMethod(guiderCore, "onNewFrame", Qt::QueuedConnection,
                                                  Q_ARG(QString, sdkGuiderFitsPath));
                        Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | invoke guiderCore onNewFrame costMs=" +
                                        std::to_string(invokePerf.elapsed()) +
                                        " mainTotalMs=" + std::to_string(mainPerf.elapsed()),
                                    LogLevel::INFO, DeviceType::GUIDER);
                    }
                    else
                    {
                        QElapsedTimer persistPerf;
                        persistPerf.start();
                        PersistGuidingFits(sdkGuiderFitsPath);
                        Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | PersistGuidingFits costMs=" +
                                        std::to_string(persistPerf.elapsed()) +
                                        " mainTotalMs=" + std::to_string(mainPerf.elapsed()),
                                    LogLevel::INFO, DeviceType::GUIDER);
                    }
                }

                guiderExposureInFlight = false;
                if (isGuiderLoopExp && guiderLoopTimer)
                    guiderLoopTimer->start(1);
                Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | success path done mainTotalMs=" +
                                std::to_string(mainPerf.elapsed()),
                            LogLevel::INFO, DeviceType::GUIDER);
                return;
            }

            const qint64 elapsed2 = QDateTime::currentMSecsSinceEpoch() - sdkGuiderExposureStartTime;
            Logger::Log("onSdkGuiderExposureTimerTimeout | GetSingleFrame not ready: " + frameRes.message,
                        LogLevel::DEBUG, DeviceType::GUIDER);
            int retryMs = 10;
            if (elapsed2 < expected) {
                retryMs = static_cast<int>(std::max<qint64>(1, expected - elapsed2));
            } else if (elapsed2 > expected + 2000) {
                retryMs = 50;
            }
            Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | not ready retryMs=" +
                            std::to_string(retryMs) +
                            " elapsedSinceExposureStartMs=" + std::to_string(elapsed2) +
                            " mainTotalMs=" + std::to_string(mainPerf.elapsed()),
                        LogLevel::INFO, DeviceType::GUIDER);
            sdkGuiderExposureTimer->start(retryMs);
        }, Qt::QueuedConnection);
    });
}
