#include "mainwindow_command_support.h"

void MainWindow::onGuiderLoopTimeout()
{
    if (!isGuiderLoopExp)
        return;

#if QUARCS_SIM_GUIDER
    if (simGuiderFrameSource)
    {
        if (guiderExposureInFlight)
            return;

        guiderExposureInFlight = true;
        const int expMs = std::max(1, guiderExpMs);
        const QString fitsPath = simGuiderFrameSource->generateNextFrame(expMs);
        guiderExposureInFlight = false;

        if (fitsPath.isEmpty() || !QFile::exists(fitsPath))
        {
            Logger::Log("onGuiderLoopTimeout | simulated guider frame generation failed",
                        LogLevel::ERROR, DeviceType::GUIDER);
            isGuiderLoopExp = false;
            if (guiderLoopTimer)
                guiderLoopTimer->stop();
            emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
            emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
            return;
        }

        if (guiderCore)
        {
            QMetaObject::invokeMethod(guiderCore, "onNewFrame", Qt::QueuedConnection,
                                      Q_ARG(QString, fitsPath));
        }
        else
        {
            PersistGuidingFits(fitsPath);
            if (isGuiderLoopExp && guiderLoopTimer)
                guiderLoopTimer->start(1);
        }
        return;
    }
#endif

    const bool guiderSdk =
        (systemdevicelist.system_devices.size() > 1 &&
         systemdevicelist.system_devices[DeviceSlot::Guider].isSDKConnect &&
         sdkGuiderHandle != nullptr);

    // SDK 模式：导星相机不依赖 INDI 连接
    if (guiderSdk)
    {
        SdkSerialExecutor *guiderExec = sdkGuiderCameraExecutor();
        if (!guiderExec || !guiderExec->isRunning())
        {
            Logger::Log("onGuiderLoopTimeout | sdkGuiderCamExec not running, stopping guider loop", LogLevel::WARNING, DeviceType::GUIDER);
            isGuiderLoopExp = false;
            guiderExposureInFlight = false;
            if (guiderCore)
            {
                postGuiderCore(guiderCore, [](GuiderCore *core) {
                    core->stopGuiding();
                    core->stopLoop();
                });
            }
            if (guiderLoopTimer)
                guiderLoopTimer->stop();
            emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
            emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
            return;
        }
        if (guiderExposureInFlight)
            return;

        guiderExposureInFlight = true;
        const double expSec = std::max(1, guiderExpMs) / 1000.0;
        const int expMs = std::max(1, guiderExpMs);

        // 0) 对齐主相机 SDK：曝光前确保分辨率/ROI 为有效全分辨率，否则某些机型会出现 GetSingleFrame 卡死/返回无效帧
        {
            QElapsedTimer loopPerf;
            loopPerf.start();
            qint64 loopLastMs = 0;
            auto logLoopStage = [&](const std::string& stage) {
                const qint64 nowMs = loopPerf.elapsed();
                Logger::Log("GuiderPerf | GuiderLoop(SDK) | stage=" + stage +
                                " deltaMs=" + std::to_string(nowMs - loopLastMs) +
                                " totalMs=" + std::to_string(nowMs),
                            LogLevel::INFO, DeviceType::GUIDER);
                loopLastMs = nowMs;
            };

            // 尝试取消上一帧可能残留的曝光/读出（避免连续触发时卡死）
            SdkCommand cancelCmd;
            cancelCmd.type = SdkCommandType::Custom;
            cancelCmd.name = "CancelExposure";
            cancelCmd.payload = std::any();
            SdkResult cancelRes = SdkManager::instance().callByHandle(sdkGuiderHandle, cancelCmd);
            Logger::Log("GuiderPerf | GuiderLoop(SDK) | CancelExposure success=" +
                            std::to_string(cancelRes.success ? 1 : 0) +
                            " msg=" + cancelRes.message,
                        LogLevel::INFO, DeviceType::GUIDER);
            logLoopStage("cancel_previous_exposure");

            SdkAreaInfo fullRoi;
            bool haveFullRoi = false;
            {
                SdkCommand effCmd;
                effCmd.type = SdkCommandType::Custom;
                effCmd.name = "GetEffectiveArea";
                effCmd.payload = std::any();
                SdkResult effRes = SdkManager::instance().callByHandle(sdkGuiderHandle, effCmd);
                Logger::Log("GuiderPerf | GuiderLoop(SDK) | GetEffectiveArea success=" +
                                std::to_string(effRes.success ? 1 : 0) +
                                " msg=" + effRes.message,
                            LogLevel::INFO, DeviceType::GUIDER);
                logLoopStage("get_effective_area");
                if (effRes.success)
                {
                    try
                    {
                        fullRoi = std::any_cast<SdkAreaInfo>(effRes.payload);
                        haveFullRoi = (fullRoi.sizeX > 0 && fullRoi.sizeY > 0);
                    }
                    catch (const std::bad_any_cast &)
                    {
                        haveFullRoi = false;
                    }
                }
            }
            if (!haveFullRoi)
            {
                // 回退：GetChipInfo（尽量拿到最大分辨率）
                SdkCommand chipCmd;
                chipCmd.type = SdkCommandType::Custom;
                chipCmd.name = "GetChipInfo";
                chipCmd.payload = std::any();
                SdkResult chipRes = SdkManager::instance().callByHandle(sdkGuiderHandle, chipCmd);
                Logger::Log("GuiderPerf | GuiderLoop(SDK) | GetChipInfo fallback success=" +
                                std::to_string(chipRes.success ? 1 : 0) +
                                " msg=" + chipRes.message,
                            LogLevel::INFO, DeviceType::GUIDER);
                logLoopStage("get_chip_info_fallback");
                if (chipRes.success)
                {
                    try
                    {
                        SdkChipInfo chip = std::any_cast<SdkChipInfo>(chipRes.payload);
                        fullRoi.startX = 0;
                        fullRoi.startY = 0;
                        fullRoi.sizeX = chip.maxImageSizeX;
                        fullRoi.sizeY = chip.maxImageSizeY;
                        haveFullRoi = (fullRoi.sizeX > 0 && fullRoi.sizeY > 0);
                    }
                    catch (const std::bad_any_cast &)
                    {
                        haveFullRoi = false;
                    }
                }
            }

            if (haveFullRoi)
            {
                SdkCommand setResCmd;
                setResCmd.type = SdkCommandType::Custom;
                setResCmd.name = "SetResolution";
                setResCmd.payload = fullRoi;
                SdkResult setResRes = SdkManager::instance().callByHandle(sdkGuiderHandle, setResCmd);
                Logger::Log("GuiderPerf | GuiderLoop(SDK) | SetResolution success=" +
                                std::to_string(setResRes.success ? 1 : 0) +
                                " roi=" + std::to_string(fullRoi.startX) + "," + std::to_string(fullRoi.startY) +
                                "," + std::to_string(fullRoi.sizeX) + "x" + std::to_string(fullRoi.sizeY) +
                                " msg=" + setResRes.message,
                            LogLevel::INFO, DeviceType::GUIDER);
                logLoopStage("set_resolution");
                if (!setResRes.success)
                {
                    Logger::Log("GuiderLoop(SDK) | SetResolution(full) failed: " + setResRes.message,
                                LogLevel::WARNING, DeviceType::GUIDER);
                }
            }
            else
            {
                Logger::Log("GuiderLoop(SDK) | SetResolution(full) skipped: cannot get valid full ROI",
                            LogLevel::WARNING, DeviceType::GUIDER);
                logLoopStage("set_resolution_skipped");
            }
        }

        // 1) SetExposure（us）
        {
            QElapsedTimer setExpPerf;
            setExpPerf.start();
            SdkCommand setExpCmd;
            setExpCmd.type = SdkCommandType::Custom;
            setExpCmd.name = "SetExposure";
            setExpCmd.payload = expSec * 1000000.0;
            SdkResult setRes = SdkManager::instance().callByHandle(sdkGuiderHandle, setExpCmd);
            Logger::Log("GuiderPerf | GuiderLoop(SDK) | SetExposure success=" +
                            std::to_string(setRes.success ? 1 : 0) +
                            " costMs=" + std::to_string(setExpPerf.elapsed()) +
                            " exposureUs=" + std::to_string(static_cast<qint64>(expSec * 1000000.0)) +
                            " msg=" + setRes.message,
                        LogLevel::INFO, DeviceType::GUIDER);
            if (!setRes.success)
            {
                Logger::Log("GuiderLoop(SDK) | SetExposure failed: " + setRes.message, LogLevel::ERROR, DeviceType::GUIDER);
            }
        }

        // 2) StartSingleExposure
        {
            QElapsedTimer startExpPerf;
            startExpPerf.start();
            SdkCommand startExpCmd;
            startExpCmd.type = SdkCommandType::Custom;
            startExpCmd.name = "StartSingleExposure";
            startExpCmd.payload = std::any();
            SdkResult startRes = SdkManager::instance().callByHandle(sdkGuiderHandle, startExpCmd);
            Logger::Log("GuiderPerf | GuiderLoop(SDK) | StartSingleExposure success=" +
                            std::to_string(startRes.success ? 1 : 0) +
                            " costMs=" + std::to_string(startExpPerf.elapsed()) +
                            " msg=" + startRes.message,
                        LogLevel::INFO, DeviceType::GUIDER);
            if (!startRes.success)
            {
                Logger::Log("GuiderLoop(SDK) | StartSingleExposure failed: " + startRes.message, LogLevel::ERROR, DeviceType::GUIDER);
                guiderExposureInFlight = false;
                if (isGuiderLoopExp && guiderLoopTimer)
                    guiderLoopTimer->start(200);
                return;
            }
        }

        // 3) Poll GetSingleFrame via timer (main thread)
        sdkGuiderExposureStartTime = QDateTime::currentMSecsSinceEpoch();
        sdkGuiderExposureExpectedDuration = expMs;
        if (sdkGuiderExposureTimer)
        {
            sdkGuiderExposureTimer->start(expMs);
            Logger::Log("GuiderPerf | GuiderLoop(SDK) | exposure timer started delayMs=" +
                            std::to_string(expMs),
                        LogLevel::INFO, DeviceType::GUIDER);
        }
        return;
    }

    if (!indi_Client || dpGuider == NULL || !dpGuider->isConnected())
    {
        Logger::Log("onGuiderLoopTimeout | guider not connected, stopping loop", LogLevel::WARNING, DeviceType::GUIDER);
        isGuiderLoopExp = false;
        guiderExposureInFlight = false;
        if (guiderCore)
        {
            postGuiderCore(guiderCore, [](GuiderCore *core) {
                core->stopGuiding();
                core->stopLoop();
            });
        }
        if (guiderLoopTimer)
            guiderLoopTimer->stop();
        emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
        return;
    }

    if (guiderExposureInFlight)
        return;

    guiderExposureInFlight = true;
    const double expSec = std::max(1, guiderExpMs) / 1000.0;
    Logger::Log("onGuiderLoopTimeout | taking guider exposure " + std::to_string(expSec) + "s", LogLevel::DEBUG, DeviceType::GUIDER);
    indi_Client->takeExposure(dpGuider, expSec);
}

void MainWindow::startGuiderSingleCapture(int exposureMs)
{
    const int expMs = std::max(1, exposureMs);
    sdkGuiderExposureRole = "Guider";

    if (isGuiderLoopExp)
    {
        Logger::Log("startGuiderSingleCapture | stopping guider loop before polar capture",
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

    const bool guiderSdk = isGuiderCameraSDK();
    if (guiderSdk)
    {
        SdkSerialExecutor *guiderExec = sdkGuiderCameraExecutor();
        if (!guiderExec || !guiderExec->isRunning())
        {
            Logger::Log("startGuiderSingleCapture | sdkGuiderCamExec is not running", LogLevel::ERROR, DeviceType::GUIDER);
            polarGuiderSingleCapturePending = false;
            guiderExposureInFlight = false;
            return;
        }
        if (guiderExposureInFlight || sdkGuiderFrameTaskInFlight.load())
        {
            Logger::Log("startGuiderSingleCapture | guider exposure/readout is already in flight, retrying shortly",
                        LogLevel::WARNING, DeviceType::GUIDER);
            QTimer::singleShot(250, this, [this, expMs]() {
                if (polarAlignment != nullptr && polarAlignment->isRunning() &&
                    currentPolarAlignmentCameraRole == PolarAlignmentCameraRole::Guider)
                {
                    startGuiderSingleCapture(expMs);
                }
            });
            return;
        }

        polarGuiderSingleCapturePending = true;
        guiderExposureInFlight = true;
        const double expSec = expMs / 1000.0;
        const SdkDeviceHandle handleSnap = sdkGuiderHandle;

        guiderExec->post([this, handleSnap, expMs, expSec]() {
            auto failOnMain = [this](const std::string &message) {
                QMetaObject::invokeMethod(this, [this, message]() {
                    Logger::Log(message, LogLevel::ERROR, DeviceType::GUIDER);
                    polarGuiderSingleCapturePending = false;
                    guiderExposureInFlight = false;
                }, Qt::QueuedConnection);
            };

            SdkCommand cancelCmd;
            cancelCmd.type = SdkCommandType::Custom;
            cancelCmd.name = "CancelExposure";
            cancelCmd.payload = std::any();
            SdkManager::instance().callByHandle(handleSnap, cancelCmd);

            SdkAreaInfo fullRoi;
            bool haveFullRoi = false;
            SdkCommand effCmd;
            effCmd.type = SdkCommandType::Custom;
            effCmd.name = "GetEffectiveArea";
            effCmd.payload = std::any();
            SdkResult effRes = SdkManager::instance().callByHandle(handleSnap, effCmd);
            if (effRes.success)
            {
                try
                {
                    fullRoi = std::any_cast<SdkAreaInfo>(effRes.payload);
                    haveFullRoi = (fullRoi.sizeX > 0 && fullRoi.sizeY > 0);
                }
                catch (const std::bad_any_cast &)
                {
                    haveFullRoi = false;
                }
            }
            if (!haveFullRoi)
            {
                SdkCommand chipCmd;
                chipCmd.type = SdkCommandType::Custom;
                chipCmd.name = "GetChipInfo";
                chipCmd.payload = std::any();
                SdkResult chipRes = SdkManager::instance().callByHandle(handleSnap, chipCmd);
                if (chipRes.success)
                {
                    try
                    {
                        SdkChipInfo chip = std::any_cast<SdkChipInfo>(chipRes.payload);
                        fullRoi.startX = 0;
                        fullRoi.startY = 0;
                        fullRoi.sizeX = chip.maxImageSizeX;
                        fullRoi.sizeY = chip.maxImageSizeY;
                        haveFullRoi = (fullRoi.sizeX > 0 && fullRoi.sizeY > 0);
                    }
                    catch (const std::bad_any_cast &)
                    {
                        haveFullRoi = false;
                    }
                }
            }
            if (haveFullRoi)
            {
                SdkCommand setResCmd;
                setResCmd.type = SdkCommandType::Custom;
                setResCmd.name = "SetResolution";
                setResCmd.payload = fullRoi;
                SdkResult setResRes = SdkManager::instance().callByHandle(handleSnap, setResCmd);
                if (!setResRes.success)
                {
                    Logger::Log("startGuiderSingleCapture | SDK SetResolution(full) failed: " + setResRes.message,
                                LogLevel::WARNING, DeviceType::GUIDER);
                }
            }

            SdkCommand setExpCmd;
            setExpCmd.type = SdkCommandType::Custom;
            setExpCmd.name = "SetExposure";
            setExpCmd.payload = expSec * 1000000.0;
            SdkResult setRes = SdkManager::instance().callByHandle(handleSnap, setExpCmd);
            if (!setRes.success)
            {
                failOnMain("startGuiderSingleCapture | SDK SetExposure failed: " + setRes.message);
                return;
            }

            SdkCommand startExpCmd;
            startExpCmd.type = SdkCommandType::Custom;
            startExpCmd.name = "StartSingleExposure";
            startExpCmd.payload = std::any();
            SdkResult startRes = SdkManager::instance().callByHandle(handleSnap, startExpCmd);
            if (!startRes.success)
            {
                failOnMain("startGuiderSingleCapture | SDK StartSingleExposure failed: " + startRes.message);
                return;
            }

            QMetaObject::invokeMethod(this, [this, expMs]() {
                sdkGuiderExposureStartTime = QDateTime::currentMSecsSinceEpoch();
                sdkGuiderExposureExpectedDuration = expMs;
                if (sdkGuiderExposureTimer)
                    sdkGuiderExposureTimer->start(expMs);
            }, Qt::QueuedConnection);
        });
        return;
    }

    if (!indi_Client || dpGuider == nullptr || !dpGuider->isConnected())
    {
        Logger::Log("startGuiderSingleCapture | guider is not connected", LogLevel::ERROR, DeviceType::GUIDER);
        polarGuiderSingleCapturePending = false;
        guiderExposureInFlight = false;
        return;
    }

    polarGuiderSingleCapturePending = true;
    guiderExposureInFlight = true;
    const double expSec = expMs / 1000.0;
    indi_Client->takeExposure(dpGuider, expSec);
}


void MainWindow::saveGuiderImageAsJPG(cv::Mat Image)
{
    // 循环曝光会高频触发：降为 DEBUG（默认关闭 DEBUG）避免刷屏
    Logger::Log("Starting to save guider image as JPG...", LogLevel::DEBUG, DeviceType::GUIDER);
    QElapsedTimer perfTimer;
    perfTimer.start();
    qint64 perfLastMs = 0;
    auto logPerfStage = [&](const std::string& stage) {
        const qint64 nowMs = perfTimer.elapsed();
        Logger::Log("GuiderPerf | saveGuiderImageAsJPG | stage=" + stage +
                        " deltaMs=" + std::to_string(nowMs - perfLastMs) +
                        " totalMs=" + std::to_string(nowMs),
                    LogLevel::INFO, DeviceType::GUIDER);
        perfLastMs = nowMs;
    };

    constexpr int kKeepRecentGuiderImages = 12;
    constexpr int kGuiderPreviewMaxWidth = 1920;

    cv::Mat preview = Image;
    int downsampleLevel = 0;
    while (!preview.empty() && preview.cols > kGuiderPreviewMaxWidth)
    {
        cv::Mat downsampled;
        cv::resize(preview, downsampled, cv::Size(), 0.5, 0.5, cv::INTER_AREA);
        preview = downsampled;
        ++downsampleLevel;
    }
    logPerfStage("preview_downsample");

    if (downsampleLevel > 0)
    {
        Logger::Log("saveGuiderImageAsJPG | preview downsampled for transport: " +
                        std::to_string(Image.cols) + "x" + std::to_string(Image.rows) + " -> " +
                        std::to_string(preview.cols) + "x" + std::to_string(preview.rows) +
                        " (2x2 level=" + std::to_string(downsampleLevel) + ")",
                    LogLevel::INFO, DeviceType::GUIDER);
    }

    // 生成唯一ID
    QString uniqueId = QUuid::createUuid().toString();
    Logger::Log("Generated unique ID for new guider image: " + uniqueId.toStdString(), LogLevel::DEBUG, DeviceType::GUIDER);
    logPerfStage("generate_unique_id");

    // 保存新的图像带有唯一ID的文件名
    std::string fileName = "GuiderImage_" + uniqueId.toStdString() + ".jpg";
    std::string filePath = vueDirectoryPath + fileName;
    bool saved = cv::imwrite(filePath, preview);
    Logger::Log("Attempted to save new guider image.", LogLevel::DEBUG, DeviceType::GUIDER);
    logPerfStage("write_preview_jpg");

    std::string Command = "sudo ln -sf " + filePath + " " + vueImagePath + fileName;
    system(Command.c_str());
    Logger::Log("Created symbolic link for new guider image.", LogLevel::DEBUG, DeviceType::GUIDER);
    logPerfStage("create_preview_symlink");

    PriorGuiderImage = vueImagePath + fileName;

    auto cleanupOldGuiderImages = [&](const QString& dirPath, bool includeSymlinks, const QString& protectedFileName) {
        try {
            const fs::path dirFsPath = dirPath.toStdString();
            if (!fs::exists(dirFsPath))
                return;

            auto hasPrefix = [](const std::string& s, const std::string& p) -> bool {
                return s.rfind(p, 0) == 0;
            };
            auto hasSuffix = [](const std::string& s, const std::string& suf) -> bool {
                return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
            };

            struct EntryInfo {
                fs::path path;
                bool timeOk = false;
                fs::file_time_type t;
            };

            std::vector<EntryInfo> items;
            items.reserve(256);

            const std::string protectedName = protectedFileName.toStdString();
            bool hasProtectedEntry = false;

            for (const auto& entry : fs::directory_iterator(dirFsPath)) {
                const std::string name = entry.path().filename().string();
                if (!hasPrefix(name, "GuiderImage_") || !hasSuffix(name, ".jpg"))
                    continue;

                const bool isLink = fs::is_symlink(entry.symlink_status());
                const bool isFile = fs::is_regular_file(entry.status());
                if (includeSymlinks) {
                    if (!isLink)
                        continue;
                } else {
                    if (!isFile)
                        continue;
                }

                if (name == protectedName) {
                    hasProtectedEntry = true;
                    continue;
                }

                EntryInfo info;
                info.path = entry.path();
                try {
                    info.t = fs::last_write_time(entry.path());
                    info.timeOk = true;
                } catch (...) {
                    info.timeOk = false;
                }
                items.push_back(std::move(info));
            }

            std::sort(items.begin(), items.end(), [](const EntryInfo& a, const EntryInfo& b) {
                if (a.timeOk != b.timeOk)
                    return a.timeOk;
                if (!a.timeOk)
                    return a.path.string() < b.path.string();
                return a.t > b.t;
            });

            const int keepOthers = std::max(0, kKeepRecentGuiderImages - (hasProtectedEntry ? 1 : 0));

            Logger::Log("Listed existing guider images for cleanup in " + dirPath.toStdString() +
                            ", count=" + std::to_string(items.size() + (hasProtectedEntry ? 1 : 0)) +
                            ", protected=" + protectedName,
                        LogLevel::DEBUG, DeviceType::GUIDER);

            int kept = 0;
            for (const auto& item : items)
            {
                const bool shouldKeep = item.timeOk && kept < keepOthers;
                if (shouldKeep)
                {
                    kept++;
                    continue;
                }

                std::error_code ec;
                fs::remove(item.path, ec);
                if (!ec)
                {
                    Logger::Log("Deleted old guider image file: " + item.path.string(),
                                LogLevel::DEBUG, DeviceType::GUIDER);
                }
            }
        } catch (...) {
            Logger::Log("cleanupOldGuiderImages | cleanup failed in " + dirPath.toStdString(),
                        LogLevel::WARNING, DeviceType::GUIDER);
        }
    };

    cleanupOldGuiderImages(QString::fromStdString(vueDirectoryPath), false, QString::fromStdString(fileName));
    logPerfStage("cleanup_preview_dir");
    cleanupOldGuiderImages(QString::fromStdString(vueImagePath), true, QString::fromStdString(fileName));
    logPerfStage("cleanup_preview_symlink_dir");

    if (saved)
    {
        emit wsThread->sendMessageToClient(QString("GuideSize:%1:%2").arg(preview.cols).arg(preview.rows));
        emit wsThread->sendMessageToClient("SaveGuiderImageSuccess:" + QString::fromStdString(fileName));
        Logger::Log("Guider image saved successfully and client notified.", LogLevel::DEBUG, DeviceType::GUIDER);
        logPerfStage("notify_client");
    }
    else
    {
        Logger::Log("Failed to save guider image.", LogLevel::ERROR, DeviceType::GUIDER);
        logPerfStage("notify_client_failed");
    }
}


void MainWindow::ControlGuide(int Direction, int Duration)
{
    ControlGuideEx(Direction, Duration, QStringLiteral("Legacy/Unknown"));
}

static inline std::string GuideDirNameFromInt(int dir)
{
    switch (dir)
    {
    case 0: return "SOUTH";
    case 1: return "NORTH";
    case 2: return "EAST";
    case 3: return "WEST";
    default: return "UNK";
    }
}

void MainWindow::ControlGuideEx(int Direction, int Duration, const QString& source)
{
    const std::string reqDir = GuideDirNameFromInt(Direction);

#if QUARCS_SIM_GUIDER
    if (simGuiderFrameSource)
    {
        guiding::PulseCommand cmd;
        cmd.dir = static_cast<guiding::GuideDir>(Direction);
        cmd.durationMs = std::max(0, Duration);
        simGuiderFrameSource->injectPulse(cmd);

        Logger::Log("GuidePulse TX | src=" + source.toStdString() +
                        " | SIM inject dir=" + reqDir +
                        " durationMs=" + std::to_string(cmd.durationMs),
                    LogLevel::INFO, DeviceType::GUIDER);
        return;
    }
#endif

    if (!dpMount || !indi_Client)
    {
        Logger::Log("GuidePulse TX | src=" + source.toStdString() + " | ABORT (mount/client null) | dir=" + reqDir +
                        " durationMs=" + std::to_string(Duration),
                    LogLevel::WARNING, DeviceType::GUIDER);
        return;
    }

    // meridian flip: 仅对 NS 做方向映射（按原逻辑保持）
    int actualDir = Direction;
    if (isMeridianFlipped && (Direction == 0 || Direction == 1))
        actualDir = (Direction == 0) ? 1 : 0;

    const std::string actualDirStr = GuideDirNameFromInt(actualDir);
    const std::string axis = (Direction == 0 || Direction == 1) ? "NS" : (Direction == 2 || Direction == 3) ? "WE" : "UNK";

    Logger::Log("GuidePulse TX | src=" + source.toStdString() +
                    " | dir=" + reqDir +
                    " durationMs=" + std::to_string(Duration) +
                    " | axis=" + axis +
                    " meridianFlipped=" + std::string(isMeridianFlipped ? "1" : "0") +
                    " mappedDir=" + actualDirStr + "(" + std::to_string(actualDir) + ")",
                LogLevel::INFO, DeviceType::GUIDER);

    uint32_t ret = QHYCCD_ERROR;
    if (axis == "NS")
        ret = indi_Client->setTelescopeGuideNS(dpMount, actualDir, Duration);
    else if (axis == "WE")
        ret = indi_Client->setTelescopeGuideWE(dpMount, actualDir, Duration);

    if (ret != QHYCCD_SUCCESS)
    {
        Logger::Log("GuidePulse TX | src=" + source.toStdString() + " | FAILED ret=" + std::to_string(ret) +
                        " | dir=" + reqDir + " durationMs=" + std::to_string(Duration),
                    LogLevel::WARNING, DeviceType::GUIDER);
    }
}

void MainWindow::pauseGuidingBeforeMountMove()
{
    Logger::Log("pauseGuidingBeforeMountMove | PHD2 removed, no-op (INDI guider imaging continues if enabled).",
                LogLevel::DEBUG, DeviceType::GUIDER);
}

void MainWindow::resumeGuidingAfterMountMove()
{
    Logger::Log("resumeGuidingAfterMountMove | PHD2 removed, no-op (INDI guider imaging continues if enabled).",
                LogLevel::DEBUG, DeviceType::GUIDER);
}

void MainWindow::PersistGuidingFits(const QString& sourceFitsPath)
{
    if (sourceFitsPath.isEmpty())
        return;
    if (!QFile::exists(sourceFitsPath))
        return;

    // 按需求：导星循环曝光只需更新 /dev/shm/guiding.fits（不再额外复制到 CaptureImage/<date>/guiding.fits）
    const QString guidingShmPath = QStringLiteral("/dev/shm/guiding.fits");
    const QString effectiveFitsPath = (sourceFitsPath == guidingShmPath) ? sourceFitsPath : guidingShmPath;

    // 若 INDI 返回的路径不是 /dev/shm/guiding.fits，则覆盖同步到该固定路径
    if (sourceFitsPath != guidingShmPath)
    {
        QFile dst(guidingShmPath);
        if (dst.exists())
            dst.remove();
        if (!QFile::copy(sourceFitsPath, guidingShmPath))
        {
            Logger::Log("PersistGuidingFits | copy to /dev/shm/guiding.fits failed", LogLevel::WARNING, DeviceType::GUIDER);
            return;
        }
    }

    // 同步生成前端导星 JPG（沿用既有 SaveGuiderImageSuccess 消息协议）
    cv::Mat img;
    if (Tools::readFits(effectiveFitsPath.toUtf8().constData(), img) == 0 && !img.empty())
    {
        // 内置导星也维护“当前导星图像尺寸”，用于前端 PHD2Box/Cross 覆盖层计算
        glPHD_CurrentImageSizeX = img.cols;
        glPHD_CurrentImageSizeY = img.rows;

        // 若多星副星点等待下发（例如图像尺寸尚未拿到），在真正进入 InGuiding 时补发一次
        if (guiderMultiStarSecondaryPtsPending && wsThread
            && guiderPhaseGuiding && !guiderDirectionDetectActive)
        {
            emit wsThread->sendMessageToClient("ClearPHD2MultiStars");
            for (int i = 0; i < guiderMultiStarSecondaryPtsPx.size(); ++i)
            {
                if (i >= 8) break;
                const auto& p = guiderMultiStarSecondaryPtsPx[i];
                emit wsThread->sendMessageToClient(
                    "PHD2MultiStarsPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
                    QString::number(glPHD_CurrentImageSizeY) + ":" +
                    QString::number(static_cast<int>(std::lround(p.x()))) + ":" +
                    QString::number(static_cast<int>(std::lround(p.y()))));
            }
            guiderMultiStarSecondaryPtsPending = false;
        }

        double minV = 0.0, maxV = 0.0;
        cv::minMaxLoc(img, &minV, &maxV);

        // 循环曝光每帧都会走这里：降为 DEBUG（默认关闭 DEBUG）避免刷屏
        const uint16_t depthMax = (img.depth() == CV_8U) ? 255 : 65535;
        uint16_t B = 0;
        uint16_t W = depthMax;

        if (AutoStretch)
            Tools::GetAutoStretch(img, 0, B, W);

        if (W <= B)
            W = std::min<uint16_t>(depthMax, static_cast<uint16_t>(B + 1));

        Logger::Log("PersistGuidingFits | fits=" + effectiveFitsPath.toStdString() +
                        " depth=" + std::to_string(img.depth()) +
                        " min=" + std::to_string(minV) + " max=" + std::to_string(maxV) +
                        " B=" + std::to_string(B) + " W=" + std::to_string(W),
                    LogLevel::DEBUG, DeviceType::GUIDER);

        cv::Mat img8(img.rows, img.cols, CV_8UC1, cv::Scalar(0));

        // 近似平场/饱和帧时，传统 B/W 拉伸会把整帧映射成黑色；这里保留亮度信息，避免误判为黑屏。
        if (maxV <= minV + 1.0)
        {
            const double normalized = std::clamp(maxV / std::max<double>(1.0, depthMax), 0.0, 1.0);
            const int gray = static_cast<int>(std::lround(normalized * 255.0));
            img8.setTo(cv::Scalar(gray));
            Logger::Log("PersistGuidingFits | flat-frame fallback applied, gray=" + std::to_string(gray),
                        LogLevel::INFO, DeviceType::GUIDER);
        }
        else
        {
            Tools::Bit16To8_Stretch(img, img8, B, W);

            double min8 = 0.0, max8 = 0.0;
            cv::minMaxLoc(img8, &min8, &max8);
            if (max8 <= 0.0 && maxV > 0.0)
            {
                B = 0;
                W = static_cast<uint16_t>(std::clamp(maxV, 1.0, static_cast<double>(depthMax)));
                Tools::Bit16To8_Stretch(img, img8, B, W);
                Logger::Log("PersistGuidingFits | fallback restretch applied, B=" +
                                std::to_string(B) + " W=" + std::to_string(W),
                            LogLevel::INFO, DeviceType::GUIDER);
            }
        }

        cv::Mat guiderPreviewBgr;
        cv::cvtColor(img8, guiderPreviewBgr, cv::COLOR_GRAY2BGR);

        saveGuiderImageAsJPG(guiderPreviewBgr);
    }
}

void MainWindow::PersistGuidingPreviewFromFrame(const QString& sourceFitsPath, const cv::Mat& image16)
{
    if (sourceFitsPath.isEmpty() || image16.empty())
        return;

    const QString guidingShmPath = QStringLiteral("/dev/shm/guiding.fits");
    if (sourceFitsPath != guidingShmPath && QFile::exists(sourceFitsPath))
    {
        QFile dst(guidingShmPath);
        if (dst.exists())
            dst.remove();
        if (!QFile::copy(sourceFitsPath, guidingShmPath))
        {
            Logger::Log("PersistGuidingPreviewFromFrame | copy to /dev/shm/guiding.fits failed",
                        LogLevel::WARNING, DeviceType::GUIDER);
        }
    }

    const cv::Mat img = image16;
    glPHD_CurrentImageSizeX = img.cols;
    glPHD_CurrentImageSizeY = img.rows;

    if (guiderMultiStarSecondaryPtsPending && wsThread
        && guiderPhaseGuiding && !guiderDirectionDetectActive)
    {
        emit wsThread->sendMessageToClient("ClearPHD2MultiStars");
        for (int i = 0; i < guiderMultiStarSecondaryPtsPx.size(); ++i)
        {
            if (i >= 8) break;
            const auto& p = guiderMultiStarSecondaryPtsPx[i];
            emit wsThread->sendMessageToClient(
                "PHD2MultiStarsPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
                QString::number(glPHD_CurrentImageSizeY) + ":" +
                QString::number(static_cast<int>(std::lround(p.x()))) + ":" +
                QString::number(static_cast<int>(std::lround(p.y()))));
        }
        guiderMultiStarSecondaryPtsPending = false;
    }

    double minV = 0.0, maxV = 0.0;
    cv::minMaxLoc(img, &minV, &maxV);

    const uint16_t depthMax = (img.depth() == CV_8U) ? 255 : 65535;
    uint16_t B = 0;
    uint16_t W = depthMax;

    if (AutoStretch)
        Tools::GetAutoStretch(img, 0, B, W);

    if (W <= B)
        W = std::min<uint16_t>(depthMax, static_cast<uint16_t>(B + 1));

    Logger::Log("PersistGuidingPreviewFromFrame | frameDepth=" + std::to_string(img.depth()) +
                    " min=" + std::to_string(minV) + " max=" + std::to_string(maxV) +
                    " B=" + std::to_string(B) + " W=" + std::to_string(W),
                LogLevel::DEBUG, DeviceType::GUIDER);

    cv::Mat img8(img.rows, img.cols, CV_8UC1, cv::Scalar(0));
    if (maxV <= minV + 1.0)
    {
        const double normalized = std::clamp(maxV / std::max<double>(1.0, depthMax), 0.0, 1.0);
        const int gray = static_cast<int>(std::lround(normalized * 255.0));
        img8.setTo(cv::Scalar(gray));
        Logger::Log("PersistGuidingPreviewFromFrame | flat-frame fallback applied, gray=" + std::to_string(gray),
                    LogLevel::INFO, DeviceType::GUIDER);
    }
    else
    {
        Tools::Bit16To8_Stretch(img, img8, B, W);

        double min8 = 0.0, max8 = 0.0;
        cv::minMaxLoc(img8, &min8, &max8);
        if (max8 <= 0.0 && maxV > 0.0)
        {
            B = 0;
            W = static_cast<uint16_t>(std::clamp(maxV, 1.0, static_cast<double>(depthMax)));
            Tools::Bit16To8_Stretch(img, img8, B, W);
            Logger::Log("PersistGuidingPreviewFromFrame | fallback restretch applied, B=" +
                            std::to_string(B) + " W=" + std::to_string(W),
                        LogLevel::INFO, DeviceType::GUIDER);
        }
    }

    cv::Mat guiderPreviewBgr;
    cv::cvtColor(img8, guiderPreviewBgr, cv::COLOR_GRAY2BGR);

    saveGuiderImageAsJPG(guiderPreviewBgr);
}

void MainWindow::clearGuiderDebugAnnotations(bool refreshPreview)
{
    m_debugStarDedupCandidates.clear();
    m_debugStarSnrCandidates.clear();
    m_debugStarCandidates.clear();
    m_debugStarCandidateLabels.clear();
    m_debugStarSelected = QPointF(0, 0);
    emit wsThread->sendMessageToClient("ClearGuiderDebugCandidates");

    if (!refreshPreview)
        return;

    const QString guidingShmPath = QStringLiteral("/dev/shm/guiding.fits");
    if (QFile::exists(guidingShmPath))
        PersistGuidingFits(guidingShmPath);
}

void MainWindow::startGuiderAutoBatchCapture()
{
    const QString baseRoot = !ImageSaveBaseDirectory.isEmpty()
        ? ImageSaveBaseDirectory
        : QString::fromStdString(ImageSaveBasePath);
    const QString diagnosticsRoot = QDir(baseRoot).filePath(QStringLiteral("GuiderDiagnostics"));
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss_zzz"));
    const QString batchName = QStringLiteral("batch_%1").arg(stamp);
    const QString batchDir = QDir(diagnosticsRoot).filePath(batchName);

    if (!QDir().mkpath(batchDir))
    {
        Logger::Log("GuiderDiagnostics | failed to create batch dir: " + batchDir.toStdString(),
                    LogLevel::WARNING, DeviceType::GUIDER);
        m_guiderAutoBatchActive = false;
        m_guiderAutoBatchSavedFrames = 0;
        m_guiderAutoBatchDir.clear();
        return;
    }

    m_guiderAutoBatchActive = true;
    m_guiderAutoBatchSavedFrames = 0;
    m_guiderAutoBatchDir = batchDir;
    Logger::Log("GuiderDiagnostics | auto batch capture started: " + batchDir.toStdString(),
                LogLevel::INFO, DeviceType::GUIDER);
}

void MainWindow::stopGuiderAutoBatchCapture()
{
    if (!m_guiderAutoBatchActive && m_guiderAutoBatchDir.isEmpty())
        return;

    Logger::Log("GuiderDiagnostics | auto batch capture stopped: dir=" +
                    m_guiderAutoBatchDir.toStdString() +
                    " savedFrames=" + std::to_string(m_guiderAutoBatchSavedFrames),
                LogLevel::INFO, DeviceType::GUIDER);
    m_guiderAutoBatchActive = false;
    m_guiderAutoBatchSavedFrames = 0;
    m_guiderAutoBatchDir.clear();
}

void MainWindow::persistGuiderAutoBatchFrame(const QString& fitsPath)
{
    if (!m_guiderAutoBatchActive || m_guiderAutoBatchSavedFrames >= 20)
        return;
    if (fitsPath.isEmpty() || !QFile::exists(fitsPath) || m_guiderAutoBatchDir.isEmpty())
        return;

    const int nextIndex = m_guiderAutoBatchSavedFrames + 1;
    const QString fileName = QStringLiteral("frame_%1.fits").arg(nextIndex, 2, 10, QLatin1Char('0'));
    const QString destinationPath = QDir(m_guiderAutoBatchDir).filePath(fileName);

    QFile::remove(destinationPath);
    if (!QFile::copy(fitsPath, destinationPath))
    {
        Logger::Log("GuiderDiagnostics | failed to save frame to batch: " + destinationPath.toStdString(),
                    LogLevel::WARNING, DeviceType::GUIDER);
        return;
    }

    m_guiderAutoBatchSavedFrames = nextIndex;
    Logger::Log("GuiderDiagnostics | saved frame " + std::to_string(nextIndex) +
                    "/20 to " + destinationPath.toStdString(),
                LogLevel::INFO, DeviceType::GUIDER);

    if (m_guiderAutoBatchSavedFrames >= 20)
    {
        Logger::Log("GuiderDiagnostics | batch reached 20 frames, stop collecting: " +
                        m_guiderAutoBatchDir.toStdString(),
                    LogLevel::INFO, DeviceType::GUIDER);
        m_guiderAutoBatchActive = false;
    }
}
