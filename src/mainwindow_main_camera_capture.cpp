#include "mainwindow.h"

QString MainWindow::latestMainCaptureFitsPath() const
{
    if (!lastMainCaptureFitsPath.isEmpty() && QFile::exists(lastMainCaptureFitsPath))
        return lastMainCaptureFitsPath;

    const QString fallback = QStringLiteral("/dev/shm/ccd_simulator.fits");
    if (QFile::exists(fallback))
        return fallback;

    return QString();
}

void MainWindow::SDK_BurstCapture(int Exp_ms, int frames)
{
    Logger::Log("SDK_BurstCapture start ...", LogLevel::INFO, DeviceType::CAMERA);

    const bool isMainCameraSDK =
        (systemdevicelist.system_devices.size() > 20 &&
         systemdevicelist.system_devices[20].isSDKConnect &&
         sdkMainCameraHandle != nullptr);

    if (!isMainCameraSDK) {
        Logger::Log("SDK_BurstCapture | Main camera is not in SDK mode, fallback to startMainCameraCapture",
                    LogLevel::WARNING, DeviceType::CAMERA);
        startMainCameraCapture(Exp_ms);
        return;
    }

    const QString sdkDriverNameRaw =
        (systemdevicelist.system_devices.size() > 20) ? systemdevicelist.system_devices[20].SDKDriverName : "";
    QString sdkDriverName = sdkDriverNameRaw.trimmed();
    if (sdkDriverName.isEmpty())
        sdkDriverName = getSDKDriverName("MainCamera").trimmed();
    if (sdkDriverName.isEmpty() && systemdevicelist.system_devices.size() > 20)
        sdkDriverName = systemdevicelist.system_devices[20].DriverIndiName.trimmed();

    auto isQhySdkDriverName = [](const QString& n) -> bool {
        const QString s = n.trimmed().toLower();
        return (s == "qhyccd" || s == "indi_qhy_ccd");
    };
    if (!isQhySdkDriverName(sdkDriverName)) {
        Logger::Log("SDK_BurstCapture | SDK driver is not QHYCCD/indi_qhy_ccd (" + sdkDriverName.toStdString() +
                        "), fallback to startMainCameraCapture",
                    LogLevel::WARNING, DeviceType::CAMERA);
        startMainCameraCapture(Exp_ms);
        return;
    }

    if (Exp_ms <= 0) Exp_ms = 1;
    if (frames <= 0) frames = 1;
    if (frames > 1024) frames = 1024;

    if (mainCameraCaptureMode != MainCameraCaptureMode::Burst) {
        mainCameraCaptureMode = MainCameraCaptureMode::Burst;
    }
    applySdkMainCameraCaptureMode();

    SdkSerialExecutor *mainExec = sdkMainCameraExecutor();
    if (!mainExec || !mainExec->isRunning()) {
        Logger::Log("SDK_BurstCapture | sdkMainCamExec not running", LogLevel::ERROR, DeviceType::CAMERA);
        emit wsThread->sendMessageToClient("ExposureFailed:SDK worker not running");
        emit wsThread->sendMessageToClient("CameraInExposuring:False");
        glMainCameraStatu = "IDLE";
        ShootStatus = "IDLE";
        return;
    }

    if (sdkExposureTimer && sdkExposureTimer->isActive()) {
        sdkExposureTimer->stop();
    }
    sdkFrameTaskInFlight = false;
    sdkExposureIsROI = false;

    glIsFocusingLooping = false;
    isSavePngSuccess = false;
    glMainCameraStatu = "Exposuring";
    ShootStatus = "Exposuring";

    sdkBurstActive = true;
    sdkBurstCancelRequested = false;

    const int expMsSnap = Exp_ms;
    const int framesSnap = frames;

    mainExec->post([this, expMsSnap, framesSnap]() mutable {
        QString failReason;
        bool cancelled = false;
        std::shared_ptr<SdkFrameData> outFrame = std::make_shared<SdkFrameData>();

        auto waitMainCameraReady = [this](int timeoutMs, SdkDeviceInfo& outDev) -> bool {
            const auto t0 = std::chrono::steady_clock::now();
            while (true) {
                if (sdkBurstCancelRequested.load())
                    return false;
                outDev = SdkManager::instance().getDevice("MainCamera");
                if (outDev.handle != nullptr && outDev.state == SdkDeviceState::Open)
                    return true;
                const auto elapsedMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                if (elapsedMs > timeoutMs)
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        };

        auto callMain = [&](const char* name, const std::any& payload) -> SdkResult {
            SdkDeviceInfo dev;
            if (!waitMainCameraReady(8000, dev)) {
                SdkResult r;
                r.success = false;
                r.errorCode = SdkErrorCode::DeviceNotFound;
                r.message = "MainCamera not ready (reopening)";
                return r;
            }
            SdkCommand c;
            c.type = SdkCommandType::Custom;
            c.name = name;
            c.payload = payload;
            return SdkManager::instance().call(dev.driverName, dev.handle, c);
        };

        {
            const double expUs = static_cast<double>(std::max(1, expMsSnap)) * 1000.0;
            SdkResult setExpRes = callMain("SetExposure", expUs);
            if (!setExpRes.success) {
                Logger::Log("SDK_BurstCapture | SetExposure warning: " + setExpRes.message,
                            LogLevel::WARNING, DeviceType::CAMERA);
            }
        }

        bool usePureLive = false;
        const int start = 1;
        const int end = framesSnap + 2;
        auto burstTriggerT = std::chrono::steady_clock::time_point{};
        if (!usePureLive) {
            (void)callMain("ResetFrameCounter", std::any());

            SdkResult idleRes = callMain("SetBurstIDLE", std::any());
            if (!idleRes.success) {
                usePureLive = true;
                Logger::Log("SDK_BurstCapture | SetBurstIDLE failed, fallback to pure Live. msg=" + idleRes.message,
                            LogLevel::WARNING, DeviceType::CAMERA);
            }
        }
        if (!usePureLive) {
            SdkResult seRes = callMain("SetBurstStartEnd", std::make_pair(start, end));
            if (!seRes.success) {
                usePureLive = true;
                Logger::Log("SDK_BurstCapture | SetBurstStartEnd failed, fallback to pure Live. msg=" + seRes.message,
                            LogLevel::WARNING, DeviceType::CAMERA);
            }
        }
        if (!usePureLive) {
            SdkResult relRes = callMain("ReleaseBurstIDLE", std::any());
            if (!relRes.success) {
                usePureLive = true;
                Logger::Log("SDK_BurstCapture | ReleaseBurstIDLE failed, fallback to pure Live. msg=" + relRes.message,
                            LogLevel::WARNING, DeviceType::CAMERA);
            } else {
                burstTriggerT = std::chrono::steady_clock::now();
            }
        }

        if (usePureLive) {
            (void)callMain("ReleaseBurstIDLE", std::any());
            (void)callMain("EnableBurstMode", false);
            (void)callMain("BeginLive", std::any());
        }

        std::vector<uint32_t> accum;
        int okFrames = 0;
        int width = 0;
        int height = 0;

        const auto t0 = std::chrono::steady_clock::now();
        const auto maxWait = std::chrono::milliseconds(
            std::max(15000, expMsSnap * framesSnap + 15000));
        bool firstFrameLogged = false;

        while (okFrames < framesSnap)
        {
            if (sdkBurstCancelRequested.load()) {
                cancelled = true;
                break;
            }
            if (std::chrono::steady_clock::now() - t0 > maxWait) {
                break;
            }

            SdkCommand getCmd;
            getCmd.type = SdkCommandType::Custom;
            getCmd.name = "GetLiveFrame";
            getCmd.payload = std::any();

            SdkDeviceInfo dev;
            if (!waitMainCameraReady(8000, dev)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            SdkResult frameRes = SdkManager::instance().call(dev.driverName, dev.handle, getCmd);
            if (!frameRes.success) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            SdkFrameData frame;
            try {
                frame = std::any_cast<SdkFrameData>(frameRes.payload);
            } catch (const std::bad_any_cast&) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            const bool hasFrameData =
                (!frame.pixels.empty()) || (frame.rawBuffer != nullptr && frame.rawBytes > 0);
            if (frame.width <= 0 || frame.height <= 0 || !hasFrameData) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            if (okFrames == 0) {
                if (!firstFrameLogged && burstTriggerT != std::chrono::steady_clock::time_point{}) {
                    const auto dtFirstMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - burstTriggerT).count();
                    Logger::Log("SDK_BurstCapture | first frame after ReleaseBurstIDLE: " + std::to_string(dtFirstMs) + " ms",
                                LogLevel::INFO, DeviceType::CAMERA);
                    firstFrameLogged = true;
                }
                width = frame.width;
                height = frame.height;
                accum.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0u);
            }

            if (frame.width != width || frame.height != height || accum.empty() ||
                (static_cast<size_t>(width) * static_cast<size_t>(height)) != accum.size()) {
                continue;
            }

            if (!frame.pixels.empty()) {
                if (frame.pixels.size() != accum.size()) {
                    continue;
                }
                for (size_t p = 0; p < accum.size(); ++p) {
                    accum[p] += frame.pixels[p];
                }
            } else if (frame.rawBuffer != nullptr && frame.rawBytes > 0) {
                const size_t pixelCount = accum.size();
                if (frame.channels != 1 || (frame.bpp != 16 && frame.bpp != 8)) {
                    continue;
                }
                const size_t needBytes = pixelCount * (frame.bpp == 16 ? sizeof(uint16_t) : sizeof(uint8_t));
                if (frame.rawBytes < needBytes || frame.rawBuffer->size() < needBytes) {
                    continue;
                }
                if (frame.bpp == 16) {
                    const uint16_t* src = reinterpret_cast<const uint16_t*>(frame.rawBuffer->data());
                    for (size_t p = 0; p < pixelCount; ++p) {
                        accum[p] += src[p];
                    }
                } else {
                    const uint8_t* src = reinterpret_cast<const uint8_t*>(frame.rawBuffer->data());
                    for (size_t p = 0; p < pixelCount; ++p) {
                        accum[p] += static_cast<uint32_t>(src[p]) * 257u;
                    }
                }
            } else {
                continue;
            }
            okFrames++;
        }

        (void)callMain("SetBurstIDLE", std::any());

        if (cancelled) {
            QMetaObject::invokeMethod(this, [this]() {
                sdkBurstActive = false;
                sdkBurstCancelRequested = false;
                glMainCameraStatu = "IDLE";
                ShootStatus = "IDLE";
                emit wsThread->sendMessageToClient("CameraInExposuring:False");
            }, Qt::QueuedConnection);
            return;
        }

        if (okFrames < framesSnap || accum.empty() || width <= 0 || height <= 0) {
            failReason = QStringLiteral("Burst 获取图像失败（未获得足够有效帧）");
            QMetaObject::invokeMethod(this, [this, failReason]() {
                sdkBurstActive = false;
                sdkBurstCancelRequested = false;
                emit wsThread->sendMessageToClient("ExposureFailed:" + failReason);
                emit wsThread->sendMessageToClient("CameraInExposuring:False");
                glMainCameraStatu = "IDLE";
                ShootStatus = "IDLE";
            }, Qt::QueuedConnection);
            return;
        }

        outFrame->width = width;
        outFrame->height = height;
        outFrame->bpp = 16;
        outFrame->channels = 1;
        outFrame->pixels.resize(accum.size());
        for (size_t p = 0; p < accum.size(); ++p) {
            outFrame->pixels[p] = static_cast<uint16_t>(accum[p] / static_cast<uint32_t>(okFrames));
        }

        QMetaObject::invokeMethod(this, [this, outFrame]() {
            sdkBurstActive = false;
            sdkBurstCancelRequested = false;

            if (sdkMainCameraHandle == nullptr) {
                glMainCameraStatu = "IDLE";
                ShootStatus = "IDLE";
                emit wsThread->sendMessageToClient("CameraInExposuring:False");
                return;
            }

            const std::string fitsPath = "/dev/shm/ccd_simulator.fits";
            SaveQhyFrameDataToFits(*outFrame, fitsPath);

            glMainCameraStatu = "Displaying";
            ShootStatus = "Completed";
            emit wsThread->sendMessageToClient("ExposureCompleted");
            emitCaptureTrace(QStringLiteral("backend_exposure_completed"), currentCaptureTraceStartedAtMs,
                             QStringLiteral("source=sdk_burst"));

            if (polarAlignment != nullptr && polarAlignment->isRunning())
            {
                notifyPolarAlignmentCaptureReady(PolarAlignmentCameraRole::MainCamera,
                                                 QString::fromStdString(fitsPath));
                return;
            }

            if (isAutoFocus && autoFocus != nullptr && autoFocus->isRunning())
            {
                saveFitsAsPNG(QString::fromStdString(fitsPath), true);
                autoFocus->setCaptureComplete(QString::fromStdString(fitsPath));
                Logger::Log("SDK_BurstCapture | ExposureCompleted -> autoFocus capture complete: " + fitsPath,
                            LogLevel::INFO, DeviceType::FOCUSER);
                return;
            }

            saveFitsAsPNG(QString::fromStdString(fitsPath), true);

            if (mainCameraAutoSave && isScheduleRunning == false) {
                Logger::Log("SDK_BurstCapture | Auto Save enabled, saving captured image...",
                            LogLevel::INFO, DeviceType::CAMERA);
                CaptureImageSave();
            }
        }, Qt::QueuedConnection);
    });
}

bool MainWindow::ensureSdkMainCameraSingleModeReady(QString *errorReason)
{
    const bool isMainCameraSDK =
        (systemdevicelist.system_devices.size() > 20 &&
         systemdevicelist.system_devices[20].isSDKConnect &&
         sdkMainCameraHandle != nullptr);
    if (!isMainCameraSDK)
        return true;

    auto isQhySdkDriverName = [](const QString& n) -> bool {
        const QString s = n.trimmed().toLower();
        return (s == "qhyccd" || s == "indi_qhy_ccd");
    };

    QString sdkDriverName =
        (systemdevicelist.system_devices.size() > 20) ? systemdevicelist.system_devices[20].SDKDriverName : "";
    QString effectiveSdkDriverName = sdkDriverName.trimmed();
    if (effectiveSdkDriverName.isEmpty())
        effectiveSdkDriverName = getSDKDriverName("MainCamera").trimmed();
    if (effectiveSdkDriverName.isEmpty() && systemdevicelist.system_devices.size() > 20)
        effectiveSdkDriverName = systemdevicelist.system_devices[20].DriverIndiName.trimmed();

    if (!isQhySdkDriverName(effectiveSdkDriverName))
        return true;

    const bool alreadySingleReady =
        (mainCameraCaptureMode == MainCameraCaptureMode::Single) &&
        !sdkMainLiveReady.load() &&
        !sdkMainBurstModeReady.load();
    if (alreadySingleReady)
        return true;

    SdkSerialExecutor *mainExec = sdkMainCameraExecutor();
    if (!mainExec || !mainExec->isRunning()) {
        if (errorReason)
            *errorReason = QStringLiteral("SDK worker not running");
        Logger::Log("ensureSdkMainCameraSingleModeReady | sdkMainCamExec not running",
                    LogLevel::ERROR, DeviceType::CAMERA);
        return false;
    }

    if (sdkBurstActive.load() || glMainCameraStatu == "Exposuring") {
        if (errorReason)
            *errorReason = QStringLiteral("camera busy while switching to single mode");
        Logger::Log("ensureSdkMainCameraSingleModeReady | camera busy, reject switch to single mode",
                    LogLevel::WARNING, DeviceType::CAMERA);
        return false;
    }

    Logger::Log("ensureSdkMainCameraSingleModeReady | switching SDK main camera to Single mode before exposure",
                LogLevel::INFO, DeviceType::CAMERA);
    const qint64 switchStartMs = QDateTime::currentMSecsSinceEpoch();
    const SdkDeviceHandle handleSnap = sdkMainCameraHandle;

    const bool ok = mainExec->postAndWait<bool>([handleSnap]() -> bool {
        if (handleSnap == nullptr)
            return false;

        auto callByHandle = [handleSnap](const char *name, const std::any &payload) -> SdkResult {
            SdkCommand cmd;
            cmd.type = SdkCommandType::Custom;
            cmd.name = name;
            cmd.payload = payload;
            return SdkManager::instance().callByHandle(handleSnap, cmd);
        };

        (void)callByHandle("ReleaseBurstIDLE", std::any());
        (void)callByHandle("StopLive", std::any());
        (void)callByHandle("EnableBurstMode", false);
        SdkResult streamRes = callByHandle("SetStreamMode", 0);
        if (!streamRes.success) {
            Logger::Log("ensureSdkMainCameraSingleModeReady | SetStreamMode(0) failed: " + streamRes.message,
                        LogLevel::ERROR, DeviceType::CAMERA);
            return false;
        }
        return true;
    });

    if (!ok) {
        if (errorReason)
            *errorReason = QStringLiteral("failed to switch SDK camera to single mode");
        Logger::Log("ensureSdkMainCameraSingleModeReady | failed to switch SDK main camera to Single mode",
                    LogLevel::ERROR, DeviceType::CAMERA);
        return false;
    }

    mainCameraCaptureMode = MainCameraCaptureMode::Single;
    sdkMainLiveReady = false;
    sdkMainBurstModeReady = false;
    sdkMainAppliedMode = MainCameraCaptureMode::Single;
    sdkMainAppliedModeValid = true;
    emitCaptureTrace(QStringLiteral("backend_single_mode_ready"), switchStartMs,
                     QStringLiteral("success=true"));
    Logger::Log("ensureSdkMainCameraSingleModeReady | SDK main camera switched to Single mode",
                LogLevel::INFO, DeviceType::CAMERA);
    return true;
}

void MainWindow::startMainCameraCapture(int exposureMs)
{
    Logger::Log("startMainCameraCapture start ...", LogLevel::INFO, DeviceType::CAMERA);
    emitCaptureTrace(QStringLiteral("backend_main_camera_capture_enter"), currentCaptureTraceStartedAtMs,
                     QString("exposureMs=%1").arg(exposureMs));

    glIsFocusingLooping = false;
    isSavePngSuccess = false;
    double expTime_sec = (double)exposureMs / 1000;
    Logger::Log("startMainCameraCapture | convert exposureMs to seconds:" + std::to_string(expTime_sec), LogLevel::INFO, DeviceType::CAMERA);

    bool isMainCameraSDK = (systemdevicelist.system_devices.size() > 20 &&
                            systemdevicelist.system_devices[20].isSDKConnect &&
                            sdkMainCameraHandle != nullptr);

    if (isMainCameraSDK)
    {
        auto makeUserFriendlySdkReason = [](const QString& step, const QString& sdkMsg) -> QString {
            const QString raw = sdkMsg.trimmed();
            QString msg = raw;

            const int ecIdx = msg.lastIndexOf("error code", -1, Qt::CaseInsensitive);
            if (ecIdx >= 0)
            {
                msg = msg.left(ecIdx).trimmed();
                while (msg.endsWith(',')) msg.chop(1);
                msg = msg.trimmed();
            }

            const bool isGenericFail =
                raw.contains("4294967295") || raw.contains("0xFFFFFFFF", Qt::CaseInsensitive);

            QString reason = QStringLiteral("曝光失败：") + step;
            if (!msg.isEmpty())
                reason += QStringLiteral("（") + msg + QStringLiteral("）");
            if (isGenericFail)
                reason += QStringLiteral("。可能原因：相机未连接/驱动未初始化/USB通信异常。请尝试重新连接相机或重启驱动。");
            return reason;
        };

        QString singleModeReason;
        if (!ensureSdkMainCameraSingleModeReady(&singleModeReason)) {
            emit wsThread->sendMessageToClient("ExposureFailed:SDK单帧模式未就绪（" + singleModeReason + "）");
            emit wsThread->sendMessageToClient("CameraInExposuring:False");
            ShootStatus = "IDLE";
            glMainCameraStatu = "IDLE";
            return;
        }

        glMainCameraStatu = "Exposuring";
        Logger::Log("startMainCameraCapture | SDK Mode | Main Camera Status: " + glMainCameraStatu.toStdString(),
                   LogLevel::INFO, DeviceType::CAMERA);
        const qint64 sdkCaptureStageStartMs = QDateTime::currentMSecsSinceEpoch();

        const bool polarAlignmentCapture =
            (polarAlignment != nullptr && polarAlignment->isRunning());
        int requestedSdkBin = 1;
        if (polarAlignmentCapture) {
            requestedSdkBin = 2;
            if (const char* envBin = std::getenv("QUARCS_POLAR_SDK_BIN")) {
                bool ok = false;
                const int parsed = QString::fromLocal8Bit(envBin).trimmed().toInt(&ok);
                if (ok) requestedSdkBin = std::clamp(parsed, 1, 4);
            }
        }

        {
            const qint64 setBinStartMs = QDateTime::currentMSecsSinceEpoch();
            SdkCommand binCmd;
            binCmd.type = SdkCommandType::Custom;
            binCmd.name = "SetBinMode";
            binCmd.payload = std::make_pair(requestedSdkBin, requestedSdkBin);
            SdkResult binRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, binCmd);
            emitCaptureTrace(QStringLiteral("backend_set_bin_done"), setBinStartMs,
                             QString("success=%1,bin=%2,polar=%3")
                                 .arg(binRes.success ? QStringLiteral("true") : QStringLiteral("false"))
                                 .arg(requestedSdkBin)
                                 .arg(polarAlignmentCapture ? QStringLiteral("true") : QStringLiteral("false")));
            if (!binRes.success) {
                Logger::Log("startMainCameraCapture | SDK SetBinMode(" +
                                std::to_string(requestedSdkBin) + "," +
                                std::to_string(requestedSdkBin) + ") failed: " + binRes.message,
                            LogLevel::WARNING, DeviceType::CAMERA);
            } else {
                Logger::Log("startMainCameraCapture | SDK SetBinMode(" +
                                std::to_string(requestedSdkBin) + "," +
                                std::to_string(requestedSdkBin) + ") success" +
                                (polarAlignmentCapture ? " for polar alignment" : " for normal capture"),
                            LogLevel::INFO, DeviceType::CAMERA);
            }
        }

        SdkAreaInfo fullRoi;
        {
            const qint64 effectiveAreaStartMs = QDateTime::currentMSecsSinceEpoch();
            SdkCommand effCmd;
            effCmd.type = SdkCommandType::Custom;
            effCmd.name = "GetEffectiveArea";
            effCmd.payload = std::any();
            SdkResult effRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, effCmd);
            if (effRes.success) {
                fullRoi = std::any_cast<SdkAreaInfo>(effRes.payload);
            } else {
                fullRoi.startX = 0;
                fullRoi.startY = 0;
                fullRoi.sizeX  = (glMainCCDSizeX > 0) ? static_cast<unsigned int>(glMainCCDSizeX) : 0;
                fullRoi.sizeY  = (glMainCCDSizeY > 0) ? static_cast<unsigned int>(glMainCCDSizeY) : 0;
            }
            emitCaptureTrace(QStringLiteral("backend_get_effective_area_done"), effectiveAreaStartMs,
                             QString("success=%1,startX=%2,startY=%3,sizeX=%4,sizeY=%5")
                                 .arg(effRes.success ? QStringLiteral("true") : QStringLiteral("false"))
                                 .arg(fullRoi.startX)
                                 .arg(fullRoi.startY)
                                 .arg(fullRoi.sizeX)
                                 .arg(fullRoi.sizeY));
        }
        if (fullRoi.sizeX > 0 && fullRoi.sizeY > 0) {
            const qint64 setResolutionStartMs = QDateTime::currentMSecsSinceEpoch();
            SdkCommand setResCmd;
            setResCmd.type = SdkCommandType::Custom;
            setResCmd.name = "SetResolution";
            setResCmd.payload = fullRoi;
            SdkResult setResRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, setResCmd);
            if (!setResRes.success) {
                Logger::Log("startMainCameraCapture | SDK SetResolution(full) failed: " + setResRes.message,
                            LogLevel::ERROR, DeviceType::CAMERA);
                const QString reason =
                    makeUserFriendlySdkReason(QStringLiteral("设置分辨率失败"), QString::fromStdString(setResRes.message));
                emit wsThread->sendMessageToClient("ExposureFailed:" + reason);
                emit wsThread->sendMessageToClient("CameraInExposuring:False");
                ShootStatus = "IDLE";
                glMainCameraStatu = "IDLE";
                return;
            } else {
                emitCaptureTrace(QStringLiteral("backend_set_resolution_done"), setResolutionStartMs,
                                 QString("success=true,startX=%1,startY=%2,sizeX=%3,sizeY=%4")
                                     .arg(fullRoi.startX)
                                     .arg(fullRoi.startY)
                                     .arg(fullRoi.sizeX)
                                     .arg(fullRoi.sizeY));
                Logger::Log("startMainCameraCapture | SDK SetResolution(full) success: " +
                            std::to_string(fullRoi.startX) + "," + std::to_string(fullRoi.startY) + " " +
                            std::to_string(fullRoi.sizeX) + "x" + std::to_string(fullRoi.sizeY),
                            LogLevel::DEBUG, DeviceType::CAMERA);
            }
        } else {
            emitCaptureTrace(QStringLiteral("backend_set_resolution_skipped"), sdkCaptureStageStartMs,
                             QString("reason=invalid_full_roi,sizeX=%1,sizeY=%2")
                                 .arg(fullRoi.sizeX)
                                 .arg(fullRoi.sizeY));
            Logger::Log("startMainCameraCapture | SDK SetResolution(full) skipped: invalid fullRoi size (" +
                        std::to_string(fullRoi.sizeX) + "x" + std::to_string(fullRoi.sizeY) + ")",
                        LogLevel::WARNING, DeviceType::CAMERA);
        }

        const qint64 setExposureStartMs = QDateTime::currentMSecsSinceEpoch();
        SdkCommand setExpCmd;
        setExpCmd.type = SdkCommandType::Custom;
        setExpCmd.name = "SetExposure";
        setExpCmd.payload = expTime_sec * 1000000.0;
        SdkResult setExpRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, setExpCmd);
        if (!setExpRes.success) {
            Logger::Log("startMainCameraCapture | SDK SetExposure failed: " + setExpRes.message,
                       LogLevel::ERROR, DeviceType::CAMERA);
            const QString reason =
                makeUserFriendlySdkReason(QStringLiteral("设置曝光时间失败"), QString::fromStdString(setExpRes.message));
            emit wsThread->sendMessageToClient("ExposureFailed:" + reason);
            emit wsThread->sendMessageToClient("CameraInExposuring:False");
            ShootStatus = "IDLE";
            glMainCameraStatu = "IDLE";
            return;
        }
        emitCaptureTrace(QStringLiteral("backend_set_exposure_done"), setExposureStartMs,
                         QString("success=true,exposureUs=%1")
                             .arg(QString::number(expTime_sec * 1000000.0, 'f', 0)));

        const qint64 startExposureCmdStartMs = QDateTime::currentMSecsSinceEpoch();
        SdkCommand startExpCmd;
        startExpCmd.type = SdkCommandType::Custom;
        startExpCmd.name = "StartSingleExposure";
        startExpCmd.payload = std::any();
        Logger::Log("CaptureTrace | stage=backend_start_single_exposure_callbyhandle_enter"
                        " | handle=" + std::to_string(reinterpret_cast<uintptr_t>(sdkMainCameraHandle)) +
                        " | thread=" + std::to_string(
                            static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()))),
                    LogLevel::INFO, DeviceType::CAMERA);
        SdkResult startExpRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, startExpCmd);
        Logger::Log("CaptureTrace | stage=backend_start_single_exposure_callbyhandle_return"
                        " | handle=" + std::to_string(reinterpret_cast<uintptr_t>(sdkMainCameraHandle)) +
                        " | ok=" + std::string(startExpRes.success ? "true" : "false") +
                        " | msg=" + startExpRes.message,
                    startExpRes.success ? LogLevel::INFO : LogLevel::ERROR,
                    DeviceType::CAMERA);
        if (!startExpRes.success) {
            Logger::Log("startMainCameraCapture | SDK StartSingleExposure failed: " + startExpRes.message,
                       LogLevel::ERROR, DeviceType::CAMERA);
            const QString reason =
                makeUserFriendlySdkReason(QStringLiteral("启动曝光失败"), QString::fromStdString(startExpRes.message));
            emit wsThread->sendMessageToClient("ExposureFailed:" + reason);
            emit wsThread->sendMessageToClient("CameraInExposuring:False");
            ShootStatus = "IDLE";
            glMainCameraStatu = "IDLE";
            return;
        }
        emitCaptureTrace(QStringLiteral("backend_start_single_exposure_done"), startExposureCmdStartMs,
                         QString("success=true,transport=sdk"));
        emitCaptureTrace(QStringLiteral("backend_exposure_start"), currentCaptureTraceStartedAtMs,
                         QString("transport=sdk,exposureMs=%1").arg(static_cast<int>(expTime_sec * 1000)));
        Logger::Log("startMainCameraCapture | SDK StartSingleExposure success, expTime_sec:" + std::to_string(expTime_sec),
                   LogLevel::INFO, DeviceType::CAMERA);

        int expTime_ms = static_cast<int>(expTime_sec * 1000);
        sdkExposureStartTime = QDateTime::currentMSecsSinceEpoch();
        sdkExposureExpectedDuration = expTime_ms;
        sdkExposureIsROI = false;

        sdkExposureTimer->start(expTime_ms);
        Logger::Log("startMainCameraCapture | SDK exposure timer started, will check after " + std::to_string(expTime_ms) + "ms",
                   LogLevel::INFO, DeviceType::CAMERA);
    }
    else if (dpMainCamera)
    {
        glMainCameraStatu = "Exposuring";
        Logger::Log("startMainCameraCapture | INDI Mode | check Main Camera Status(glMainCameraStatu):" + glMainCameraStatu.toStdString(), LogLevel::INFO, DeviceType::CAMERA);

        int value, min, max;
        uint32_t ret = indi_Client->getCCDGain(dpMainCamera, value, min, max);
        if (ret != QHYCCD_SUCCESS)
        {
            Logger::Log("startMainCameraCapture | indi getCCDGain | failed", LogLevel::WARNING, DeviceType::CAMERA);
        }
        Logger::Log("startMainCameraCapture | indi getCCDGain | value:" + std::to_string(value) + ", min:" + std::to_string(min) + ", max:" + std::to_string(max), LogLevel::INFO, DeviceType::CAMERA);
        int BINX, BINY, BINXMAX, BINYMAX;
        ret = indi_Client->getCCDBinning(dpMainCamera, BINX, BINY, BINXMAX, BINYMAX);
        if (ret != QHYCCD_SUCCESS)
        {
            Logger::Log("startMainCameraCapture | indi getCCDBinning | failed", LogLevel::WARNING, DeviceType::CAMERA);
        }
        Logger::Log("startMainCameraCapture | indi getCCDBinning | BINX:" + std::to_string(BINX) + ", BINY:" + std::to_string(BINY) + ", BINXMAX:" + std::to_string(BINXMAX) + ", BINYMAX:" + std::to_string(BINYMAX), LogLevel::INFO, DeviceType::CAMERA);
        ret = indi_Client->getCCDOffset(dpMainCamera, value, min, max);
        if (ret != QHYCCD_SUCCESS)
        {
            Logger::Log("startMainCameraCapture | indi getCCDOffset | failed", LogLevel::WARNING, DeviceType::CAMERA);
        }
        Logger::Log("startMainCameraCapture | indi getCCDOffset | value:" + std::to_string(value) + ", min:" + std::to_string(min) + ", max:" + std::to_string(max), LogLevel::INFO, DeviceType::CAMERA);
        ret = indi_Client->resetCCDFrameInfo(dpMainCamera);
        if (ret != QHYCCD_SUCCESS)
        {
            Logger::Log("startMainCameraCapture | indi resetCCDFrameInfo | failed", LogLevel::WARNING, DeviceType::CAMERA);
        }
        Logger::Log("startMainCameraCapture | indi resetCCDFrameInfo", LogLevel::INFO, DeviceType::CAMERA);
        emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));
        Logger::Log("startMainCameraCapture | sendMessageToClient | MainCameraSize:" + QString::number(glMainCCDSizeX).toStdString() + ":" + QString::number(glMainCCDSizeY).toStdString(), LogLevel::INFO, DeviceType::CAMERA);
        ret = indi_Client->takeExposure(dpMainCamera, expTime_sec);
        if (ret != QHYCCD_SUCCESS)
        {
            Logger::Log("startMainCameraCapture | indi takeExposure | failed", LogLevel::WARNING, DeviceType::CAMERA);
        }
        emitCaptureTrace(QStringLiteral("backend_exposure_start"), currentCaptureTraceStartedAtMs,
                         QString("transport=indi,exposureMs=%1").arg(static_cast<int>(expTime_sec * 1000)));
        Logger::Log("startMainCameraCapture | indi start takeExposure, expTime_sec:" + std::to_string(expTime_sec), LogLevel::INFO, DeviceType::CAMERA);
    }
    else
    {
        Logger::Log("startMainCameraCapture | Main Camera not available (both SDK and INDI are NULL)", LogLevel::WARNING, DeviceType::CAMERA);
        ShootStatus = "IDLE";
    }
    Logger::Log("startMainCameraCapture finished.", LogLevel::INFO, DeviceType::CAMERA);
}

void MainWindow::abortMainCameraCapture()
{
    Logger::Log("abortMainCameraCapture start ...", LogLevel::INFO, DeviceType::CAMERA);
    glMainCameraStatu = "IDLE";
    Logger::Log("abortMainCameraCapture | glMainCameraStatu:" + glMainCameraStatu.toStdString(), LogLevel::INFO, DeviceType::CAMERA);

    bool isMainCameraSDK = (systemdevicelist.system_devices.size() > 20 &&
                            systemdevicelist.system_devices[20].isSDKConnect &&
                            sdkMainCameraHandle != nullptr);

    if (isMainCameraSDK)
    {
        if (sdkExposureTimer && sdkExposureTimer->isActive()) {
            sdkExposureTimer->stop();
            Logger::Log("abortMainCameraCapture | Stopped sdkExposureTimer to prevent redundant GetSingleFrame calls",
                       LogLevel::DEBUG, DeviceType::CAMERA);
        }

        sdkFrameTaskInFlight = false;
        sdkExposureIsROI = false;

        if (sdkBurstActive.load()) {
            sdkBurstCancelRequested = true;
            Logger::Log("abortMainCameraCapture | Burst active, request cancel",
                        LogLevel::INFO, DeviceType::CAMERA);

            SdkSerialExecutor *mainExec = sdkMainCameraExecutor();
            if (mainExec && mainExec->isRunning() && sdkMainCameraHandle != nullptr) {
                const SdkDeviceHandle handleSnap = sdkMainCameraHandle;
                mainExec->post([handleSnap]() {
                    SdkCommand idle;
                    idle.type = SdkCommandType::Custom;
                    idle.name = "SetBurstIDLE";
                    idle.payload = std::any();
                    (void)SdkManager::instance().callByHandle(handleSnap, idle);
                });
            }
        }

        SdkCommand abortCmd;
        abortCmd.type = SdkCommandType::Custom;
        abortCmd.name = "CancelExposure";
        abortCmd.payload = std::any();

        SdkResult abortRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, abortCmd);
        if (!abortRes.success) {
            Logger::Log("abortMainCameraCapture | SDK CancelExposure failed: " + abortRes.message,
                       LogLevel::ERROR, DeviceType::CAMERA);
        } else {
            Logger::Log("abortMainCameraCapture | SDK CancelExposure success", LogLevel::INFO, DeviceType::CAMERA);
        }
        ShootStatus = "IDLE";
    }
    else if (dpMainCamera)
    {
        indi_Client->setCCDAbortExposure(dpMainCamera);
        ShootStatus = "IDLE";
        Logger::Log("abortMainCameraCapture | INDI ShootStatus:" + ShootStatus.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    }
    Logger::Log("abortMainCameraCapture finished.", LogLevel::INFO, DeviceType::CAMERA);
}

void MainWindow::SaveQhyFrameDataToFits(const SdkFrameData& frame, const std::string& filepath)
{
    const bool hasVecPixels = !frame.pixels.empty();
    const bool hasRawPixels = (frame.rawBuffer != nullptr && frame.rawBytes > 0);
    if (frame.width <= 0 || frame.height <= 0 || (!hasVecPixels && !hasRawPixels))
    {
        Logger::Log("SaveQhyFrameDataToFits | invalid frame, skip write. size=" +
                        std::to_string(frame.width) + "x" + std::to_string(frame.height) +
                        " pixels=" + std::to_string(frame.pixels.size()) +
                        " raw=" + std::string(hasRawPixels ? "true" : "false") +
                        " rawBytes=" + std::to_string(frame.rawBytes) +
                        " bpp=" + std::to_string(frame.bpp) +
                        " ch=" + std::to_string(frame.channels),
                    LogLevel::ERROR, DeviceType::CAMERA);
        return;
    }

    fitsfile *fptr;
    int status = 0;
    long naxes[2] = {static_cast<long>(frame.width), static_cast<long>(frame.height)};
    const long fpixel[2] = {1, 1};

    remove(filepath.c_str());

    fits_create_file(&fptr, filepath.c_str(), &status);
    if (status) {
        Logger::Log("SaveQhyFrameDataToFits | fits_create_file failed, status=" + std::to_string(status),
                   LogLevel::ERROR, DeviceType::CAMERA);
        return;
    }

    int bitpix = USHORT_IMG;
    int datatype = TUSHORT;
    long nelements = static_cast<long>(static_cast<long long>(frame.width) * static_cast<long long>(frame.height));
    const void* srcPtr = nullptr;

    if (hasVecPixels) {
        bitpix = USHORT_IMG;
        datatype = TUSHORT;
        nelements = static_cast<long>(frame.pixels.size());
        srcPtr = frame.pixels.data();
    } else {
        if (frame.channels != 1 || (frame.bpp != 16 && frame.bpp != 8)) {
            Logger::Log("SaveQhyFrameDataToFits | unsupported rawBuffer format: bpp=" +
                            std::to_string(frame.bpp) + " channels=" + std::to_string(frame.channels),
                        LogLevel::ERROR, DeviceType::CAMERA);
            fits_close_file(fptr, &status);
            return;
        }
        const size_t pixelCount = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
        const size_t needBytes = pixelCount * (frame.bpp == 16 ? sizeof(uint16_t) : sizeof(uint8_t));
        if (frame.rawBuffer->size() < needBytes || frame.rawBytes < needBytes) {
            Logger::Log("SaveQhyFrameDataToFits | rawBuffer too small: needBytes=" +
                            std::to_string(needBytes) + " rawBytes=" + std::to_string(frame.rawBytes) +
                            " bufSize=" + std::to_string(frame.rawBuffer->size()),
                        LogLevel::ERROR, DeviceType::CAMERA);
            fits_close_file(fptr, &status);
            return;
        }
        nelements = static_cast<long>(pixelCount);
        if (frame.bpp == 8) {
            bitpix = BYTE_IMG;
            datatype = TBYTE;
        } else {
            bitpix = USHORT_IMG;
            datatype = TUSHORT;
        }
        srcPtr = frame.rawBuffer->data();
    }

    fits_create_img(fptr, bitpix, 2, naxes, &status);
    if (status) {
        Logger::Log("SaveQhyFrameDataToFits | fits_create_img failed, status=" + std::to_string(status),
                   LogLevel::ERROR, DeviceType::CAMERA);
        fits_close_file(fptr, &status);
        return;
    }

    fits_write_pix(fptr, datatype, const_cast<long*>(fpixel), nelements,
                   const_cast<void*>(srcPtr), &status);
    if (status) {
        Logger::Log("SaveQhyFrameDataToFits | fits_write_pix failed, status=" + std::to_string(status),
                   LogLevel::ERROR, DeviceType::CAMERA);
    }

    fits_close_file(fptr, &status);
    if (status) {
        Logger::Log("SaveQhyFrameDataToFits | fits_close_file failed, status=" + std::to_string(status),
                   LogLevel::ERROR, DeviceType::CAMERA);
    } else {
        Logger::Log("SaveQhyFrameDataToFits | FITS saved successfully: " + filepath,
                   LogLevel::INFO, DeviceType::CAMERA);
    }
}

void MainWindow::onSdkExposureTimerTimeout()
{
    sdkExposureTimer->stop();

    SdkSerialExecutor *mainExec = sdkMainCameraExecutor();
    if (!mainExec || !mainExec->isRunning())
    {
        Logger::Log("onSdkExposureTimerTimeout | sdkMainCamExec not running, stop polling",
                    LogLevel::ERROR, DeviceType::CAMERA);
        glMainCameraStatu = "IDLE";
        ShootStatus = "IDLE";
        return;
    }

    if (sdkMainCameraHandle == nullptr)
    {
        Logger::Log("onSdkExposureTimerTimeout | sdkMainCameraHandle is nullptr (maybe disconnected). Stop polling.",
                    LogLevel::WARNING, DeviceType::CAMERA);
        glMainCameraStatu = "IDLE";
        ShootStatus = "IDLE";
        if (sdkExposureIsROI)
        {
            glIsFocusingLooping = false;
            isFocusLoopShooting = false;
        }
        sdkExposureIsROI = false;
        return;
    }

    if (sdkFrameTaskInFlight.exchange(true))
    {
        sdkExposureTimer->start(10);
        return;
    }

    const SdkDeviceHandle handleSnap = sdkMainCameraHandle;
    const qint64 startSnap = sdkExposureStartTime;
    const int expectedSnap = sdkExposureExpectedDuration;
    const bool isRoiSnap = sdkExposureIsROI;

    mainExec->post([this, handleSnap, startSnap, expectedSnap, isRoiSnap]() {
        const qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
        const qint64 elapsed = currentTime - startSnap;

        const qint64 expected = std::max<qint64>(1, expectedSnap);
        const qint64 maxWaitMs = std::max<qint64>(expected + 5000, expected * 3);

        if (elapsed > maxWaitMs)
        {
            SdkCommand cancelCmd;
            cancelCmd.type = SdkCommandType::Custom;
            cancelCmd.name = "CancelExposure";
            cancelCmd.payload = std::any();
            SdkResult cancelRes = SdkManager::instance().callByHandle(handleSnap, cancelCmd);

            QMetaObject::invokeMethod(
                this,
                [this, cancelRes, isRoiSnap, elapsed, expected, maxWaitMs]() {
                    sdkFrameTaskInFlight = false;

                    Logger::Log("onSdkExposureTimerTimeout | TIMEOUT waiting frame (elapsed=" +
                                    std::to_string(elapsed) + "ms, expected=" + std::to_string(expected) +
                                    "ms, maxWait=" + std::to_string(maxWaitMs) + "ms). Cancelling exposure.",
                                LogLevel::ERROR, DeviceType::CAMERA);

                    if (!cancelRes.success) {
                        Logger::Log("onSdkExposureTimerTimeout | CancelExposure failed: " + cancelRes.message,
                                    LogLevel::WARNING, DeviceType::CAMERA);
                    }

                    glMainCameraStatu = "IDLE";
                    ShootStatus = "IDLE";

                    if (isRoiSnap) {
                        glIsFocusingLooping = false;
                        isFocusLoopShooting = false;
                        emit wsThread->sendMessageToClient("startFocusLoopFailed:SDK GetSingleFrame timeout");
                    } else {
                        emit wsThread->sendMessageToClient("ExposureFailed:SDK GetSingleFrame timeout");
                    }
                },
                Qt::QueuedConnection);
            return;
        }

        SdkCommand getFrameCmd;
        getFrameCmd.type = SdkCommandType::Custom;
        getFrameCmd.name = "GetSingleFrame";
        getFrameCmd.payload = std::any();
        Logger::Log("onSdkExposureTimerTimeout | dispatch GetSingleFrame to SDK thread: handle=" +
                        std::to_string(reinterpret_cast<uintptr_t>(handleSnap)) +
                        " elapsed=" + std::to_string(elapsed) +
                        "ms expected=" + std::to_string(expected) +
                        "ms isROI=" + std::string(isRoiSnap ? "true" : "false"),
                    LogLevel::INFO, DeviceType::CAMERA);
        SdkResult frameRes = SdkManager::instance().callByHandle(handleSnap, getFrameCmd);

        QMetaObject::invokeMethod(
            this,
            [this, frameRes, isRoiSnap, expected]() mutable {
                sdkFrameTaskInFlight = false;

                if (sdkMainCameraHandle == nullptr)
                    return;

                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                const qint64 elapsed2 = now - sdkExposureStartTime;

                Logger::Log("onSdkExposureTimerTimeout | Elapsed: " + std::to_string(elapsed2) + "ms, Expected: " +
                                std::to_string(sdkExposureExpectedDuration) + "ms",
                            LogLevel::DEBUG, DeviceType::CAMERA);

                if (frameRes.success)
                {
                    Logger::Log("onSdkExposureTimerTimeout | GetSingleFrame success", LogLevel::INFO, DeviceType::CAMERA);

                    SdkFrameData frame;
                    try {
                        frame = std::any_cast<SdkFrameData>(frameRes.payload);
                    } catch (const std::bad_any_cast&) {
                        Logger::Log("onSdkExposureTimerTimeout | payload any_cast failed",
                                    LogLevel::WARNING, DeviceType::CAMERA);
                        glMainCameraStatu = "IDLE";
                        return;
                    }
                    Logger::Log("onSdkExposureTimerTimeout | Frame size: " +
                                    std::to_string(frame.width) + "x" + std::to_string(frame.height),
                                LogLevel::INFO, DeviceType::CAMERA);

                    if (isRoiSnap && !isFocusLoopShooting) {
                        Logger::Log("onSdkExposureTimerTimeout | ROI loop stopped, discard frame",
                                    LogLevel::WARNING, DeviceType::CAMERA);
                        glMainCameraStatu = "IDLE";
                        return;
                    }

                    if (isRoiSnap != sdkExposureIsROI) {
                        Logger::Log("onSdkExposureTimerTimeout | ROI mode changed during capture (snapshot=" +
                                    std::string(isRoiSnap ? "true" : "false") + ", current=" +
                                    std::string(sdkExposureIsROI ? "true" : "false") + "), discard frame",
                                    LogLevel::WARNING, DeviceType::CAMERA);
                        glMainCameraStatu = "IDLE";
                        return;
                    }

                    auto framePtr = std::make_shared<SdkFrameData>(std::move(frame));
                    const std::string fitsPath = "/dev/shm/ccd_simulator.fits";

                    glMainCameraStatu = "Displaying";

                    if (isRoiSnap)
                    {
                        SaveQhyFrameDataToFits(*framePtr, fitsPath);
                        saveFitsAsJPG(QString::fromStdString(fitsPath), true);
                        Logger::Log("onSdkExposureTimerTimeout | ROI mode, saveFitsAsJPG complete",
                                    LogLevel::DEBUG, DeviceType::CAMERA);
                    }
                    else
                    {
                        SaveQhyFrameDataToFits(*framePtr, fitsPath);
                        lastMainCaptureFitsPath = QString::fromStdString(fitsPath);

                        ShootStatus = "Completed";
                        emit wsThread->sendMessageToClient("ExposureCompleted");
                        emitCaptureTrace(QStringLiteral("backend_exposure_completed"), currentCaptureTraceStartedAtMs,
                                         QStringLiteral("source=sdk_timer"));
                        Logger::Log("onSdkExposureTimerTimeout | Full resolution mode, ExposureCompleted",
                                    LogLevel::INFO, DeviceType::CAMERA);

                        if (polarAlignment != nullptr && polarAlignment->isRunning())
                        {
                            notifyPolarAlignmentCaptureReady(PolarAlignmentCameraRole::MainCamera,
                                                             QString::fromStdString(fitsPath));
                            return;
                        }

                        if (isAutoFocus && autoFocus != nullptr && autoFocus->isRunning())
                        {
                            saveFitsAsPNG_FromSdkFrame(framePtr, true, [this, fitsPath](bool ok) {
                                if (!ok) return;
                                if (autoFocus != nullptr && autoFocus->isRunning()) {
                                    autoFocus->setCaptureComplete(QString::fromStdString(fitsPath));
                                    Logger::Log("onSdkExposureTimerTimeout | ExposureCompleted -> autoFocus capture complete: " + fitsPath,
                                                LogLevel::INFO, DeviceType::FOCUSER);
                                }
                            });
                            return;
                        }

                        if (mainCameraAutoSave && isScheduleRunning == false) {
                            saveFitsAsPNG_FromSdkFrame(framePtr, true, [this](bool ok) {
                                if (!ok) return;
                                Logger::Log("onSdkExposureTimerTimeout | Auto Save enabled, saving captured image...",
                                            LogLevel::INFO, DeviceType::CAMERA);
                                CaptureImageSaveAsync();
                            });
                        } else {
                            saveFitsAsPNG_FromSdkFrame(framePtr, true);
                        }
                    }
                    return;
                }

                Logger::Log("onSdkExposureTimerTimeout | GetSingleFrame failed: " + frameRes.message,
                            LogLevel::DEBUG, DeviceType::CAMERA);

                const bool unsupportedFormat =
                    (frameRes.message.find("unsupported format") != std::string::npos);
                if (unsupportedFormat) {
                    Logger::Log("onSdkExposureTimerTimeout | unsupported frame format is not retryable, stop exposure polling",
                                LogLevel::ERROR, DeviceType::CAMERA);
                    glMainCameraStatu = "IDLE";
                    ShootStatus = "IDLE";
                    sdkExposureIsROI = false;
                    if (isRoiSnap) {
                        glIsFocusingLooping = false;
                        isFocusLoopShooting = false;
                        emit wsThread->sendMessageToClient("startFocusLoopFailed:SDK unsupported frame format");
                    } else {
                        emit wsThread->sendMessageToClient("ExposureFailed:SDK unsupported frame format");
                    }
                    return;
                }

                if (isRoiSnap && !isFocusLoopShooting) {
                    Logger::Log("onSdkExposureTimerTimeout | ROI loop stopped, abort timer restart",
                                LogLevel::INFO, DeviceType::CAMERA);
                    glMainCameraStatu = "IDLE";
                    return;
                }

                if (isRoiSnap != sdkExposureIsROI) {
                    Logger::Log("onSdkExposureTimerTimeout | ROI mode changed (snapshot=" +
                                std::string(isRoiSnap ? "true" : "false") + ", current=" +
                                std::string(sdkExposureIsROI ? "true" : "false") + "), abort timer restart",
                                LogLevel::INFO, DeviceType::CAMERA);
                    glMainCameraStatu = "IDLE";
                    return;
                }

                int retryMs = 10;
                if (elapsed2 < expected) {
                    retryMs = static_cast<int>(std::max<qint64>(1, expected - elapsed2));
                } else if (elapsed2 > expected + 10000) {
                    retryMs = 200;
                } else if (elapsed2 > expected + 2000) {
                    retryMs = 50;
                }
                sdkExposureTimer->start(retryMs);
            },
            Qt::QueuedConnection);
    });
}
