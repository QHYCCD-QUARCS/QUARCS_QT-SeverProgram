#include "mainwindow_command_support.h"

bool MainWindow::handleCaptureCommand(const QString &message, const QStringList &parts)
{
    const QString command = parts.isEmpty() ? message.trimmed() : parts[0].trimmed();
    if (!(
            command == QLatin1String("takeExposure") ||
            command == QLatin1String("takeExposureBurst") ||
            command == QLatin1String("setExposureTime") ||
            command == QLatin1String("abortExposure") ||
            command == QLatin1String("SetMainCameraCaptureMode") ||
            command == QLatin1String("ImageGainR") ||
            command == QLatin1String("ImageGainB") ||
            command == QLatin1String("CalcWhiteBalance") ||
            command == QLatin1String("ImageOffset") ||
            command == QLatin1String("ImageCFA") ||
            command == QLatin1String("SetCFWPosition") ||
            command == QLatin1String("CFWList") ||
            command == QLatin1String("getCFWList") ||
            command == QLatin1String("SetCAARotator") ||
            command == QLatin1String("getCAARotator") ||
            command == QLatin1String("SetBinning") ||

            command == QLatin1String("SetCameraTemperature") ||
            command == QLatin1String("SetCameraGain") ||
            command == QLatin1String("SetUsbTraffic") ||
            command == QLatin1String("SetMainCameraAutoSave") ||
            command == QLatin1String("SetMainCameraSaveFailedParse") ||
            command == QLatin1String("SetMainCameraSaveFolder") ||
            command == QLatin1String("SetMainCameraTileBuildMode") ||
            command == QLatin1String("SetMainCameraTileLevelMode") ||
            command == QLatin1String("MainCameraFocalLength") ||
            command == QLatin1String("PoleCameraFocalLength") ||
            command == QLatin1String("getMainCameraParameters")))
    {
        return false;
    }

    auto run = [this, &message, &parts]() {
    if (parts.size() >= 2 && parts[0].trimmed() == "takeExposure")
    {
        Logger::Log("takeExposure:" + parts[1].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::CAMERA);
        Logger::Log("Accept takeExposure order ,set ExpTime is " + parts[1].trimmed().toStdString() + " ms", LogLevel::DEBUG, DeviceType::CAMERA);
        int ExpTime = parts[1].trimmed().toInt();
        currentCaptureTraceId = (parts.size() >= 3) ? parts[2].trimmed() : QString();
        currentCaptureTraceStartedAtMs = QDateTime::currentMSecsSinceEpoch();
        emitCaptureTrace(QStringLiteral("backend_command_received"), currentCaptureTraceStartedAtMs,
                         QString("mode=single,exposureMs=%1").arg(ExpTime));

        startMainCameraCapture(ExpTime);
        glExpTime = ExpTime;
    }
    else if (parts.size() >= 3 && parts[0].trimmed() == "takeExposureBurst")
    {
        // Burst：仅 QHYCCD SDK 支持（前端也会做驱动限制，这里再做兜底校验）
        Logger::Log("takeExposureBurst:" + parts[1].trimmed().toStdString() + ":" + parts[2].trimmed().toStdString(),
                    LogLevel::DEBUG, DeviceType::CAMERA);
        const int ExpTime = parts[1].trimmed().toInt();   // ms
        const int frames  = parts[2].trimmed().toInt();   // frames
        currentCaptureTraceId = (parts.size() >= 4) ? parts[3].trimmed() : QString();
        currentCaptureTraceStartedAtMs = QDateTime::currentMSecsSinceEpoch();
        emitCaptureTrace(QStringLiteral("backend_command_received"), currentCaptureTraceStartedAtMs,
                         QString("mode=burst,exposureMs=%1,frames=%2").arg(ExpTime).arg(frames));
        // 记录本次 Burst 的帧数，供 LoopCapture 在 Burst 模式下复用
        LoopCaptureBurstFrames = (frames > 0) ? frames : 1;

        SDK_BurstCapture(ExpTime, frames);
        glExpTime = ExpTime;
    }
    else if(parts.size() == 2 && parts[0].trimmed() == "setExposureTime")
    {
        Logger::Log("setExposureTime:" + parts[1].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::CAMERA);
        int ExpTime = parts[1].trimmed().toInt();
        glExpTime = ExpTime;  // 设置曝光时间(ms)

        // 若主相机处于 SDK Live/Burst 模式（Live 已开启），则同步更新曝光时间（一次性模式，不必重启 Live）
        const bool isMainCameraSDK =
            (systemdevicelist.system_devices.size() > 20 &&
             systemdevicelist.system_devices[20].isSDKConnect &&
             sdkMainCameraHandle != nullptr);
        if (isMainCameraSDK &&
            (mainCameraCaptureMode == MainCameraCaptureMode::Burst || mainCameraCaptureMode == MainCameraCaptureMode::Live) &&
            sdkMainLiveReady.load()) {
            const double expUs = static_cast<double>(glExpTime) * 1000.0;
            SdkSerialExecutor *mainExec = sdkMainCameraExecutor();
            if (mainExec && mainExec->isRunning()) {
                mainExec->post([expUs]() {
                    // 主相机可能正在重开（closeById->open->register），旧 handle 会被取消注册；
                    // 这里不要捕获旧 handle，改为读取当前注册表中的 MainCamera 句柄再调用，避免刷屏告警。
                    const int kWaitMs = 3000;
                    const auto t0 = std::chrono::steady_clock::now();
                    SdkDeviceInfo dev;
                    while (true) {
                        dev = SdkManager::instance().getDevice("MainCamera");
                        if (dev.handle != nullptr && dev.state == SdkDeviceState::Open)
                            break;
                        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > kWaitMs)
                            return;
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    SdkCommand setExpCmd;
                    setExpCmd.type = SdkCommandType::Custom;
                    setExpCmd.name = "SetExposure";
                    setExpCmd.payload = expUs;
                    (void)SdkManager::instance().call(dev.driverName, dev.handle, setExpCmd);
                });
            }
        }
    }
    else if (message == "abortExposure")
    {
        Logger::Log("abortExposure", LogLevel::DEBUG, DeviceType::CAMERA);
        abortMainCameraCapture();
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "SetMainCameraCaptureMode")
    {
        const QString mode = parts[1].trimmed();
        Logger::Log("SetMainCameraCaptureMode:" + mode.toStdString(), LogLevel::INFO, DeviceType::CAMERA);

        if (mode.compare("Burst", Qt::CaseInsensitive) == 0) {
            mainCameraCaptureMode = MainCameraCaptureMode::Burst;
        } else if (mode.compare("Live", Qt::CaseInsensitive) == 0) {
            mainCameraCaptureMode = MainCameraCaptureMode::Live;
        } else {
            mainCameraCaptureMode = MainCameraCaptureMode::Single;
        }

        // 若主相机已处于 SDK 连接状态，则立即应用（进入/退出 Live/Burst）
        Logger::Log("SetMainCameraCaptureMode | applying mode=" + mode.toStdString(),
                    LogLevel::INFO, DeviceType::CAMERA);
        applySdkMainCameraCaptureMode();

        // Live 模式：开启循环取帧；非 Live：停止循环取帧
        if (sdkMainLiveTimer) {
            const bool wantOn = (mainCameraCaptureMode == MainCameraCaptureMode::Live);
            sdkMainLiveLoopOn = wantOn;
            sdkMainLiveFrameInFlight = false;
            sdkMainLiveNextPollMs = 0;
            sdkMainLiveProcessedSeq = 0;
            // 注意：latestFrame 保留“最后一帧”，切模式时不强制清空（方便快速恢复预览）
            if (wantOn) {
                if (!sdkMainLiveTimer->isActive())
                    sdkMainLiveTimer->start();
                if (sdkMainLiveProcessTimer && !sdkMainLiveProcessTimer->isActive())
                    sdkMainLiveProcessTimer->start();
                Logger::Log("SetMainCameraCaptureMode | sdkMainLiveTimer start (Live preview ON)",
                            LogLevel::INFO, DeviceType::CAMERA);
            } else {
                if (sdkMainLiveTimer->isActive())
                    sdkMainLiveTimer->stop();
                if (sdkMainLiveProcessTimer && sdkMainLiveProcessTimer->isActive())
                    sdkMainLiveProcessTimer->stop();
                Logger::Log("SetMainCameraCaptureMode | sdkMainLiveTimer stop (Live preview OFF)",
                            LogLevel::INFO, DeviceType::CAMERA);
            }
        }

        emit wsThread->sendMessageToClient("SetMainCameraCaptureModeSuccess:" + mode);
    }
    // PHD2 已移除：不再支持 RestartPHD2 / PHD2RestartConfirm
    else if (parts.size() == 2 && parts[0].trimmed() == "ImageGainR")
    {
        ImageGainR = parts[1].trimmed().toDouble();
        Logger::Log("GainR is set to " + std::to_string(ImageGainR), LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveParameter("MainCamera", "ImageGainR", parts[1].trimmed());
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "ImageGainB")
    {
        ImageGainB = parts[1].trimmed().toDouble();
        Logger::Log("GainB is set to " + std::to_string(ImageGainB), LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveParameter("MainCamera", "ImageGainB", parts[1].trimmed());
    }

    else if (parts.size() >= 1 && parts[0].trimmed() == "CalcWhiteBalance")
    {
        Logger::Log("收到 CalcWhiteBalance 请求，但后端自动白平衡已停用；主图链路改由前端基于 Z=0 计算", LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("WhiteBalanceGains:1.0:1.0");
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "ImageOffset")
    {
        ImageOffset = parts[1].trimmed().toDouble();
        Logger::Log("ImageOffset is set to " + std::to_string(ImageOffset), LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveParameter("MainCamera", "Offset", parts[1].trimmed());
        
        // 判断是 SDK 模式还是 INDI 模式
        bool isMainCameraSDK = (systemdevicelist.system_devices.size() > 20 &&
                                systemdevicelist.system_devices[20].isSDKConnect &&
                                sdkMainCameraHandle != nullptr);
        
        if (isMainCameraSDK)
        {
            // SDK 模式：使用 SdkManager 设置偏置
            SdkCommand setOffsetCmd;
            setOffsetCmd.type = SdkCommandType::Custom;
            setOffsetCmd.name = "SetOffset";
            setOffsetCmd.payload = ImageOffset;
            // 直接通过设备句柄调用，无需指定驱动名称
            SdkResult res = SdkManager::instance().callByHandle(sdkMainCameraHandle, setOffsetCmd);
            if (!res.success) {
                Logger::Log("ImageOffset | SDK SetOffset failed: " + res.message, LogLevel::ERROR, DeviceType::MAIN);
            } else {
                Logger::Log("ImageOffset | SDK SetOffset success", LogLevel::INFO, DeviceType::MAIN);
            }
        }
        else if (dpMainCamera != NULL)
        {
            // INDI 模式：使用 indi_Client 设置偏置
            indi_Client->setCCDOffset(dpMainCamera, ImageOffset);
        }
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "ImageCFA")
    {
        QString cfaValue = parts[1].trimmed();
        // 验证CFA值的合法性：只允许已知的Bayer模式或空值
        QStringList validCFAValues = {"RGGB", "BGGR", "GRBG", "GBRG", "RG", "BG", "GR", "GB", "", "null"};
        
        if (validCFAValues.contains(cfaValue))
        {
            MainCameraCFA = normalizeCfaPattern(cfaValue);
            if (MainCameraCFA == "NULL" || MainCameraCFA == "MONO") {
                MainCameraCFA.clear();
            }
            Logger::Log("ImageCFA is set to " + MainCameraCFA.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
            Tools::saveParameter("MainCamera", "ImageCFA", MainCameraCFA.isEmpty() ? QStringLiteral("null") : MainCameraCFA);
        }
        else
        {
            Logger::Log("ImageCFA | Invalid CFA value rejected: '" + cfaValue.toStdString() + 
                       "'. Valid values: RGGB, BGGR, GRBG, GBRG, RG, BG, GR, GB, empty, null",
                       LogLevel::ERROR, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("ErrorMessage:Invalid CFA value: " + cfaValue);
        }
    }
    else if (parts[0].trimmed() == "SetCFWPosition" && parts.size() == 2)
    {
        Logger::Log("SetCFWPosition ...", LogLevel::DEBUG, DeviceType::CFW);
        int pos1 = parts[1].trimmed().toInt(); // 前端协议：1-based
        if (pos1 <= 0) pos1 = 1;
        constexpr int kCfwMoveTimeoutMs = 10000;

        if (isFilterOnCamera)
        {
            // CFW 在相机上：INDI / SDK 双通路兼容
            if (!isMainCameraSDK() && dpMainCamera != NULL)
            {
                if (!dpMainCamera->isConnected())
                {
                    emit wsThread->sendMessageToClient("SetCFWPositionFailed:camera_disconnected");
                    Logger::Log("Set CFW Position failed: camera_disconnected", LogLevel::WARNING, DeviceType::CFW);
                }
                else
                {
                    std::string err;
                    const bool ok = indiSetCfwPosition1AndWait(indi_Client, dpMainCamera, pos1, kCfwMoveTimeoutMs, &err);
                    if (ok)
                    {
                        emit wsThread->sendMessageToClient("SetCFWPositionSuccess:" + QString::number(pos1));
                        Logger::Log("Set CFW Position to " + std::to_string(pos1) + " Success!!!", LogLevel::DEBUG, DeviceType::CFW);
                    }
                    else
                    {
                        const QString reason = (err == "device_disconnected")
                                                  ? "camera_disconnected"
                                                  : QString::fromStdString(err);
                        emit wsThread->sendMessageToClient("SetCFWPositionFailed:" + reason);
                        Logger::Log("Set CFW Position (INDI) failed: " + reason.toStdString() + ", pos=" + std::to_string(pos1), LogLevel::WARNING, DeviceType::CFW);
                    }
                }
            }
            else if (isMainCameraSDK() && sdkMainCameraHandle != nullptr)
            {
                int target0 = toSdkCfwPos0(pos1);
                std::string err;
                bool ok = sdkSetCfwPosition0AndWait(sdkMainCameraHandle, target0, kCfwMoveTimeoutMs, &err);
                if (ok)
                {
                    emit wsThread->sendMessageToClient("SetCFWPositionSuccess:" + QString::number(pos1));
                    Logger::Log("Set CFW Position (SDK) to " + std::to_string(pos1) + " Success!!!", LogLevel::DEBUG, DeviceType::CFW);
                }
                else
                {
                    Logger::Log("Set CFW Position (SDK) failed: " + err, LogLevel::WARNING, DeviceType::CFW);
                    emit wsThread->sendMessageToClient("SetCFWPositionFailed:" + QString::fromStdString(err));
                }
            }
            else if (isMainCameraSDK() && sdkMainCameraHandle == nullptr)
            {
                emit wsThread->sendMessageToClient("SetCFWPositionFailed:camera_disconnected");
                Logger::Log("Set CFW Position failed: camera_disconnected (SDK handle null)", LogLevel::WARNING, DeviceType::CFW);
            }
            else
            {
                emit wsThread->sendMessageToClient("SetCFWPositionFailed:camera_disconnected");
                Logger::Log("Set CFW Position failed: camera_disconnected (no available backend)", LogLevel::WARNING, DeviceType::CFW);
            }
        }
        else
        {
            if (dpCFW != NULL)
            {
                if (!dpCFW->isConnected())
                {
                    emit wsThread->sendMessageToClient("SetCFWPositionFailed:cfw_disconnected");
                    Logger::Log("Set CFW Position failed: cfw_disconnected", LogLevel::WARNING, DeviceType::CFW);
                }
                else
                {
                    std::string err;
                    const bool ok = indiSetCfwPosition1AndWait(indi_Client, dpCFW, pos1, kCfwMoveTimeoutMs, &err);
                    if (ok)
                    {
                        emit wsThread->sendMessageToClient("SetCFWPositionSuccess:" + QString::number(pos1));
                        Logger::Log("Set CFW Position to " + std::to_string(pos1) + " Success!!!", LogLevel::DEBUG, DeviceType::CFW);
                    }
                    else
                    {
                        const QString reason = (err == "device_disconnected")
                                                  ? "cfw_disconnected"
                                                  : QString::fromStdString(err);
                        emit wsThread->sendMessageToClient("SetCFWPositionFailed:" + reason);
                        Logger::Log("Set CFW Position (INDI ext) failed: " + reason.toStdString() + ", pos=" + std::to_string(pos1), LogLevel::WARNING, DeviceType::CFW);
                    }
                }
            }
            else
            {
                emit wsThread->sendMessageToClient("SetCFWPositionFailed:cfw_disconnected");
                Logger::Log("Set CFW Position failed: cfw_disconnected (dpCFW null)", LogLevel::WARNING, DeviceType::CFW);
            }
        }
    }

    else if (parts[0].trimmed() == "CFWList")
    {
        Logger::Log("Save CFWList ...", LogLevel::DEBUG, DeviceType::CFW);
        if (isFilterOnCamera)
        {
            // CFW 在相机上：INDI 用 slotName；SDK 用 cameraId 派生的稳定 key
            if (!isMainCameraSDK() && dpMainCamera != NULL)
            {
                QString CFWname;
                indi_Client->getCFWSlotName(dpMainCamera, CFWname);
                Tools::saveCFWList(CFWname, parts[1]);
            }
            else if (isMainCameraSDK())
            {
                Tools::saveCFWList(sdkCfwStorageKey(sdkMainCameraId), parts[1]);
            }
        }
        else
        {
            if (dpCFW != NULL)
            {
                Tools::saveCFWList(QString::fromUtf8(dpCFW->getDeviceName()), parts[1]);
            }
        }
        Logger::Log("Save CFWList finish!", LogLevel::DEBUG, DeviceType::CFW);
    }

    else if (message == "getCFWList")
    {
        Logger::Log("get CFWList ...", LogLevel::DEBUG, DeviceType::CFW);
        if (isFilterOnCamera)
        {
            // CFW 在相机上：INDI 用 slotName；SDK 用 cameraId 派生的稳定 key
            if (!isMainCameraSDK() && dpMainCamera != NULL)
            {
                // int min, max, pos;
                // indi_Client->getCFWPosition(dpMainCamera, pos, min, max);
                // emit wsThread->sendMessageToClient("CFWPositionMax:" + QString::number(max));
                QString CFWname;
                indi_Client->getCFWSlotName(dpMainCamera, CFWname);
                if (Tools::readCFWList(CFWname) != QString())
                {
                    emit wsThread->sendMessageToClient("getCFWList:" + Tools::readCFWList(CFWname));
                }
            }
            else if (isMainCameraSDK())
            {
                const QString list = Tools::readCFWList(sdkCfwStorageKey(sdkMainCameraId));
                if (!list.isEmpty())
                    emit wsThread->sendMessageToClient("getCFWList:" + list);
            }
        }
        else
        {
            if (dpCFW != NULL)
            {
                // int min, max, pos;
                // indi_Client->getCFWPosition(dpCFW, pos, min, max);
                // emit wsThread->sendMessageToClient("CFWPositionMax:" + QString::number(max));
                if (Tools::readCFWList(QString::fromUtf8(dpCFW->getDeviceName())) != QString())
                {
                    emit wsThread->sendMessageToClient("getCFWList:" + Tools::readCFWList(QString::fromUtf8(dpCFW->getDeviceName())));
                }
            }
        }
        Logger::Log("get CFWList finish!", LogLevel::DEBUG, DeviceType::CFW);
    }

    else if (parts[0].trimmed() == "SetCAARotator" && parts.size() == 2)
    {
        double angle = parts[1].trimmed().toDouble();
        if (angle < -360.0) angle = -360.0;
        if (angle > 360.0) angle = 360.0;

        if (!isCAAOnCamera || !isMainCameraSDK() || sdkCAAHandle == nullptr)
        {
            emit wsThread->sendMessageToClient("SetCAARotatorFailed:caa_disconnected");
            Logger::Log("SetCAARotator failed: caa_disconnected", LogLevel::WARNING, DeviceType::CAMERA);
        }
        else
        {
            std::string err;
            if (sdkSetCaaRotator(sdkCAAHandle, angle, &err))
            {
                SdkControlParamInfo info;
                if (sdkGetCaaRotator(sdkCAAHandle, info, &err))
                    sdkMainCaaInfoCached = info;
                else
                    sdkMainCaaInfoCached.current = angle;

                sdkCaaAccumulatedOffset += angle;
                emit wsThread->sendMessageToClient("SetCAARotatorSuccess:" + QString::number(angle, 'f', 2));
                emit wsThread->sendMessageToClient("CAARotatorAngle:" + QString::number(sdkMainCaaInfoCached.current, 'f', 2));
                emit wsThread->sendMessageToClient("CAARotatorAccumulatedOffset:" + QString::number(sdkCaaAccumulatedOffset, 'f', 2));
                Logger::Log("SetCAARotator success angle=" + std::to_string(angle), LogLevel::INFO, DeviceType::CAMERA);
            }
            else
            {
                emit wsThread->sendMessageToClient("SetCAARotatorFailed:" + QString::fromStdString(err));
                Logger::Log("SetCAARotator failed: " + err, LogLevel::WARNING, DeviceType::CAMERA);
            }
        }
    }

    else if (message == "getCAARotator")
    {
        if (!isCAAOnCamera || !isMainCameraSDK() || sdkCAAHandle == nullptr)
        {
            emit wsThread->sendMessageToClient("CAARotatorUnavailable");
            Logger::Log("getCAARotator | CAA unavailable", LogLevel::DEBUG, DeviceType::CAMERA);
        }
        else
        {
            SdkControlParamInfo info;
            std::string err;
            if (sdkGetCaaRotator(sdkCAAHandle, info, &err))
            {
                sdkMainCaaInfoCached = info;
                emit wsThread->sendMessageToClient(
                    "CAARotatorRange:" +
                    QString::number(info.minValue, 'f', 2) + ":" +
                    QString::number(info.maxValue, 'f', 2) + ":" +
                    QString::number(info.step, 'f', 2) + ":" +
                    QString::number(info.current, 'f', 2));
                emit wsThread->sendMessageToClient("CAARotatorAngle:" + QString::number(info.current, 'f', 2));
                emit wsThread->sendMessageToClient("CAARotatorAccumulatedOffset:" + QString::number(sdkCaaAccumulatedOffset, 'f', 2));
            }
            else
            {
                emit wsThread->sendMessageToClient("CAARotatorFailed:" + QString::fromStdString(err));
                Logger::Log("getCAARotator failed: " + err, LogLevel::WARNING, DeviceType::CAMERA);
            }
        }
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "SetBinning")
    {
        glMainCameraBinning = parts[1].trimmed().toInt();
        Logger::Log("Set Binning to " + std::to_string(glMainCameraBinning), LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveParameter("MainCamera", "Binning", parts[1].trimmed());

        // if(dpMainCamera != NULL) {
        //     indi_Client->setCCDBinnign(dpMainCamera, 2, 2);
        // }
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "SetCameraTemperature")
    {
        CameraTemperature = parts[1].trimmed().toDouble();
        Logger::Log("Set Camera Temperature to " + std::to_string(CameraTemperature), LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveParameter("MainCamera", "Temperature", parts[1].trimmed());
        
        // 判断是 SDK 模式还是 INDI 模式
        bool isMainCameraSDK = (systemdevicelist.system_devices.size() > 20 &&
                                systemdevicelist.system_devices[20].isSDKConnect &&
                                sdkMainCameraHandle != nullptr);
        
        if (isMainCameraSDK)
        {
            // SDK 模式：使用 SdkManager 设置制冷目标温度
            SdkCommand setTempCmd;
            setTempCmd.type = SdkCommandType::Custom;
            setTempCmd.name = "SetCoolerTargetTemperature";
            setTempCmd.payload = CameraTemperature;
            // 直接通过设备句柄调用，无需指定驱动名称
            SdkResult res = SdkManager::instance().callByHandle(sdkMainCameraHandle, setTempCmd);
            if (!res.success) {
                Logger::Log("SetCameraTemperature | SDK SetCoolerTargetTemperature failed: " + res.message, LogLevel::ERROR, DeviceType::MAIN);
            } else {
                Logger::Log("SetCameraTemperature | SDK SetCoolerTargetTemperature success", LogLevel::INFO, DeviceType::MAIN);
            }
        }
        else if (dpMainCamera != NULL)
        {
            // INDI 模式：使用 indi_Client 设置温度
            indi_Client->setTemperature(dpMainCamera, CameraTemperature);
        }
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "SetCameraGain")
    {
        CameraGain = parts[1].trimmed().toDouble();
        Logger::Log("Set Camera Gain to " + std::to_string(CameraGain), LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveParameter("MainCamera", "Gain", parts[1].trimmed());
        
        // 判断是 SDK 模式还是 INDI 模式
        bool isMainCameraSDK = (systemdevicelist.system_devices.size() > 20 &&
                                systemdevicelist.system_devices[20].isSDKConnect &&
                                sdkMainCameraHandle != nullptr);
        
        if (isMainCameraSDK)
        {
            // SDK 模式：使用 SdkManager 设置增益
            SdkCommand setGainCmd;
            setGainCmd.type = SdkCommandType::Custom;
            setGainCmd.name = "SetGain";
            setGainCmd.payload = CameraGain;
            // 直接通过设备句柄调用，无需指定驱动名称
            SdkResult res = SdkManager::instance().callByHandle(sdkMainCameraHandle, setGainCmd);
            if (!res.success) {
                Logger::Log("SetCameraGain | SDK SetGain failed: " + res.message, LogLevel::ERROR, DeviceType::MAIN);
            } else {
                Logger::Log("SetCameraGain | SDK SetGain success", LogLevel::INFO, DeviceType::MAIN);
            }
        }
        else if (dpMainCamera != NULL)
        {
            // INDI 模式：使用 indi_Client 设置增益
            indi_Client->setCCDGain(dpMainCamera, CameraGain);
        }
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "SetUsbTraffic")
    {
        int usbTraffic = parts[1].trimmed().toInt();
        Logger::Log("Set USB Traffic to " + std::to_string(usbTraffic), LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveParameter("MainCamera", "USB Traffic", parts[1].trimmed());

        // 判断是 SDK 模式还是 INDI 模式
        bool isMainCameraSDK = (systemdevicelist.system_devices.size() > 20 &&
                                systemdevicelist.system_devices[20].isSDKConnect &&
                                sdkMainCameraHandle != nullptr);

        if (isMainCameraSDK)
        {
            // SDK 模式：尝试调用 QHY SDK 的 USB Traffic 设置（若驱动不支持则忽略错误）
            SdkCommand setUsbCmd;
            setUsbCmd.type = SdkCommandType::Custom;
            setUsbCmd.name = "SetUsbTraffic";
            setUsbCmd.payload = static_cast<double>(usbTraffic);
            // 直接通过设备句柄调用，无需指定驱动名称
            SdkResult res = SdkManager::instance().callByHandle(sdkMainCameraHandle, setUsbCmd);
            if (!res.success) {
                Logger::Log("SetUsbTraffic | SDK SetUsbTraffic failed: " + res.message, LogLevel::WARNING, DeviceType::MAIN);
            } else {
                Logger::Log("SetUsbTraffic | SDK SetUsbTraffic success", LogLevel::INFO, DeviceType::MAIN);
            }

            // SDK 模式回读范围/当前值并下发到前端（用于刷新滑块）
            SdkCommand getUsbCmd;
            getUsbCmd.type = SdkCommandType::Custom;
            getUsbCmd.name = "GetUsbTraffic";
            getUsbCmd.payload = std::any();
            // 直接通过设备句柄调用，无需指定驱动名称
            SdkResult getRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, getUsbCmd);
            if (getRes.success) {
                try {
                    SdkControlParamInfo usbInfo = std::any_cast<SdkControlParamInfo>(getRes.payload);
                    glUsbTrafficMin = static_cast<int>(usbInfo.minValue);
                    glUsbTrafficMax = static_cast<int>(usbInfo.maxValue);
                    glUsbTrafficStep = static_cast<int>(usbInfo.step);
                    glUsbTrafficValue = static_cast<int>(usbInfo.current);
                    if (glUsbTrafficStep <= 0) glUsbTrafficStep = 1;

                    emit wsThread->sendMessageToClient("MainCameraUsbTrafficRange:" + QString::number(glUsbTrafficMin) +
                                                       ":" + QString::number(glUsbTrafficMax) +
                                                       ":" + QString::number(glUsbTrafficValue) +
                                                       ":" + QString::number(glUsbTrafficStep));
                } catch (const std::bad_any_cast& e) {
                    Logger::Log("SetUsbTraffic | Failed to cast USB Traffic info: " + std::string(e.what()),
                               LogLevel::ERROR, DeviceType::MAIN);
                }
            }
        }
        else if (dpMainCamera != NULL)
        {
            // INDI 模式：USB_TRAFFIC 可能不存在，失败则仅记录
            if (indi_Client->setCCDUsbTraffic(dpMainCamera, usbTraffic) != QHYCCD_SUCCESS)
            {
                Logger::Log("SetUsbTraffic | INDI setCCDUsbTraffic failed (USB_TRAFFIC may not exist)", LogLevel::WARNING, DeviceType::MAIN);
            }
        }

        // 尝试回读并下发范围/当前值，便于前端刷新显示
        if (dpMainCamera != NULL)
        {
            int v = 0, mn = 0, mx = 0, st = 1;
            if (indi_Client->getCCDUsbTraffic(dpMainCamera, v, mn, mx, st) == QHYCCD_SUCCESS)
            {
                glUsbTrafficValue = v; glUsbTrafficMin = mn; glUsbTrafficMax = mx; glUsbTrafficStep = st;
                emit wsThread->sendMessageToClient("MainCameraUsbTrafficRange:" + QString::number(glUsbTrafficMin) + ":" + QString::number(glUsbTrafficMax) + ":" + QString::number(glUsbTrafficValue) + ":" + QString::number(glUsbTrafficStep));
            }
        }
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "SetMainCameraAutoSave")
    {
        mainCameraAutoSave = (parts[1].trimmed() == "true" || parts[1].trimmed() == "1");
        Logger::Log("Set MainCamera Auto Save to " + std::string(mainCameraAutoSave ? "true" : "false"), LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveParameter("MainCamera", "AutoSave", parts[1].trimmed());
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "SetMainCameraSaveFailedParse")
    {
        mainCameraSaveFailedParse = (parts[1].trimmed() == "true" || parts[1].trimmed() == "1");
        Logger::Log("Set MainCamera Save Failed Parse to " + std::string(mainCameraSaveFailedParse ? "true" : "false"), LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveParameter("MainCamera", "SaveFailedParse", parts[1].trimmed());
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "SetMainCameraSaveFolder")
    {
        QString Mode = parts[1].trimmed();
        if(Mode == "local" || Mode == "default") {  // 兼容旧的"default"
            ImageSaveBaseDirectory = QString::fromStdString(ImageSaveBasePath);
            saveMode = "local";
            Logger::Log("Set MainCamera Save Folder to local: " + ImageSaveBaseDirectory.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        } else {
            // 根据U盘名从映射表获取路径
            if (usbMountPointsMap.contains(Mode)) {
                QString usb_mount_point = usbMountPointsMap[Mode];
                QString folderName = "QUARCS_ImageSave";
                ImageSaveBaseDirectory = usb_mount_point + "/" + folderName;
                saveMode = Mode;  // 保存U盘名
                Logger::Log("Set MainCamera Save Folder to USB: " + Mode.toStdString() + " -> " + ImageSaveBaseDirectory.toStdString(), LogLevel::INFO, DeviceType::MAIN);
            } else {
                // U盘不存在，回退到默认路径
                ImageSaveBaseDirectory = QString::fromStdString(ImageSaveBasePath);
                saveMode = "local";
                Logger::Log("Set MainCamera Save Folder: USB '" + Mode.toStdString() + "' not found, using local", LogLevel::WARNING, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("CaptureImageSaveStatus:USB-NotAvailable");
            }
        }
        // 保存时统一使用"local"替代"default"
        QString saveValue = (Mode == "default") ? "local" : Mode;
        Tools::saveParameter("MainCamera", "Save Folder", saveValue);
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "SetMainCameraTileBuildMode")
    {
        const QString requestedMode = parts[1].trimmed();
        if (requestedMode == QStringLiteral("merged_single_level")) {
            Logger::Log("SetMainCameraTileBuildMode: 'merged_single_level' is deprecated, forcing pyramid",
                        LogLevel::WARNING, DeviceType::MAIN);
        }
        tileBuildMode = QStringLiteral("pyramid");
        Logger::Log("Set MainCamera Tile Build Mode to " + tileBuildMode.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveParameter("MainCamera", "Tile Build Mode", tileBuildMode);
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "SetMainCameraTileLevelMode")
    {
        const QString requestedMode = parts[1].trimmed().toLower();
        tileLevelMode = (requestedMode == QStringLiteral("minmax")) ? QStringLiteral("minmax") : QStringLiteral("full");
        Logger::Log("Set MainCamera Tile Level Mode to " + tileLevelMode.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveParameter("MainCamera", "Tile Level Mode", tileLevelMode);
    }


    else if (parts.size() == 2 && parts[0].trimmed() == "MainCameraFocalLength")
    {
        const QString v = parts[1].trimmed();
        bool ok = false;
        const int focal = v.toInt(&ok);
        if (ok && focal > 0)
        {
            setClientSettings("MainCameraFocalLength", QString::number(focal));
            Logger::Log("MainCameraFocalLength updated: " + std::to_string(glFocalLength),
                        LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            Logger::Log("MainCameraFocalLength ignored because value is invalid: " + v.toStdString(),
                        LogLevel::WARNING, DeviceType::MAIN);
        }
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "PoleCameraFocalLength")
    {
        const QString v = parts[1].trimmed();
        bool ok = false;
        const double focal = v.toDouble(&ok);
        if (ok && focal > 0.0)
        {
            setClientSettings("PoleCameraFocalLength", QString::number(focal, 'g', 12));
            Logger::Log("PoleCameraFocalLength updated: " + std::to_string(focal),
                        LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            Logger::Log("PoleCameraFocalLength ignored because value is invalid: " + v.toStdString(),
                        LogLevel::WARNING, DeviceType::MAIN);
        }
    }
    else if (parts[0].trimmed() == "getMainCameraParameters")
    {
        Logger::Log("getMainCameraParameters ...", LogLevel::DEBUG, DeviceType::MAIN);
        getMainCameraParameters();
        Logger::Log("getMainCameraParameters finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    };

    run();
    return true;
}

bool MainWindow::handleFocuserCommand(const QString &message, const QStringList &parts)
{
    const QString command = parts.isEmpty() ? message.trimmed() : parts[0].trimmed();
    if (!(
            command == QLatin1String("focusSpeed") ||
            command == QLatin1String("focusMove") ||
            command == QLatin1String("focusMoveStep") ||
            command == QLatin1String("getFocuserMoveState") ||
            command == QLatin1String("focusMoveStop") ||
            command == QLatin1String("ManualFocuserCalibrationMode") ||
            command == QLatin1String("SyncFocuserStep") ||
            command == QLatin1String("StepsPerClick") ||
            command == QLatin1String("MinLimit") ||
            command == QLatin1String("MaxLimit") ||
            command == QLatin1String("Backlash") ||
            command == QLatin1String("Coarse Step Divisions") ||
            command == QLatin1String("AutoFocus Exposure Time (ms)") ||
            command == QLatin1String("setROIPosition") ||
            command == QLatin1String("RedBoxSizeChange") ||
            command == QLatin1String("ROICalcMode") ||
            command == QLatin1String("StopAutoFocus") ||
            command == QLatin1String("FocuserMoveState") ||
            command == QLatin1String("FocusLoopShooting") ||
            command == QLatin1String("getFocuserLoopingState") ||
            command == QLatin1String("getROIInfo") ||
            command == QLatin1String("sendRedBoxState") ||
            command == QLatin1String("focusMoveToMin") ||
            command == QLatin1String("focusMoveToMax") ||
            command == QLatin1String("focusSetTravelRange") ||
            command == QLatin1String("getFocuserParameters") ||
            command == QLatin1String("getFocuserState") ||
            command == QLatin1String("SolveCurrentPosition") ||
            message.startsWith(QLatin1String("AutoFocusConfirm:")) ||
            message.startsWith(QLatin1String("AutoFocusCoarseRetryDecision:"))))
    {
        return false;
    }

    auto run = [this, &message, &parts]() {
    if (parts.size() == 2 && parts[0].trimmed() == "focusSpeed")
    {
        Logger::Log("change focuser Speed to:" + parts[1].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::FOCUSER);
        int Speed = parts[1].trimmed().toInt();
        // qDebug() << Speed;
        int Speed_ = FocuserControl_setSpeed(Speed);
        emit wsThread->sendMessageToClient("FocusChangeSpeedSuccess:" + QString::number(Speed_));
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "focusMove")
    {
        Logger::Log("focuser to " + parts[1].trimmed().toStdString() + " move ", LogLevel::DEBUG, DeviceType::FOCUSER);
        const QString direction = parts[1].trimmed().toLower();
        const bool isLeftDir =
            (direction == "left" || direction == "inward" || direction == "in" || direction == "l");
        const bool isRightDir =
            (direction == "right" || direction == "outward" || direction == "out" || direction == "r");

        if (isLeftDir)
        {
            Logger::Log("focuser to Left move ", LogLevel::INFO, DeviceType::FOCUSER);
            FocuserControlMove(true);
        }
        else if (isRightDir)
        {
            Logger::Log("focuser to Right move ", LogLevel::INFO, DeviceType::FOCUSER);
            FocuserControlMove(false);
        }
        // else if(LR == "Target")
        // {
        //     FocusGotoAndCalFWHM(Steps);
        // }
    }
    else if(parts.size() == 3 && parts[0].trimmed() == "focusMoveStep")
    {
        Logger::Log("focuser to " + parts[1].trimmed().toStdString() + " move " + parts[2].trimmed().toStdString() + " steps", LogLevel::DEBUG, DeviceType::FOCUSER);
        const QString direction = parts[1].trimmed().toLower();
        const bool isLeftDir =
            (direction == "left" || direction == "inward" || direction == "in" || direction == "l");
        int Steps = parts[2].trimmed().toInt();
        // 单步执行时，如果上一次移动已完成，允许立即执行新的单步
        // 注意：防抖机制会阻止完全相同的命令，但不同步数的命令应该可以执行
        FocuserControlMoveStep(isLeftDir, Steps);
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "getFocuserMoveState")
    {
        if (isFocusMoveDone)
        {
            focusMoveEndTime = 6;
        }
    }
    else if (parts[0].trimmed() == "focusMoveStop" && parts.size() == 2)
    {
        if (parts[1].trimmed() == "false")
        {
            // 手动校准模式下使用静默停止，避免 stop 的异步位置回推干扰“设置边界”流程
            if (focuserManualCalibrationMode)
                FocuserControlStop(false, true);
            else
                FocuserControlStop(false);
        }
        else if (parts[1].trimmed() == "true")
        {
            FocuserControlStop(true);
        }
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "ManualFocuserCalibrationMode")
    {
        const QString mode = parts[1].trimmed().toLower();
        focuserManualCalibrationMode = (mode == "1" || mode == "true" || mode == "on");
        if (!focuserManualCalibrationMode)
        {
            focuserCalibrationExpandedDir = 0;
        }
        Logger::Log("ManualFocuserCalibrationMode:" + std::string(focuserManualCalibrationMode ? "true" : "false"),
                    LogLevel::INFO, DeviceType::FOCUSER);
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "SyncFocuserStep")
    {
        Logger::Log("SyncFocuserStep:" + parts[1].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        int Steps = parts[1].trimmed().toInt();
        
        // 检查是否是SDK模式
        const bool focuserSdkReady =
            (systemdevicelist.system_devices.size() > 22 &&
             systemdevicelist.system_devices[22].isSDKConnect &&
             systemdevicelist.system_devices[22].isBind &&
             sdkFocuserHandle != nullptr);
        
        if (dpFocuser != NULL || focuserSdkReady)
        {
            // 计算偏移量：新位置 - 当前位置
            int oldPosition = CurrentPosition;
            if (dpFocuser != NULL)
            {
                oldPosition = FocuserControl_getPosition();
            }
            else if (focuserSdkReady && sdkFocuserPosValid.load())
            {
                oldPosition = sdkFocuserPosCache.load();
            }
            
            int offset = Steps - oldPosition;
            
            Logger::Log("SyncFocuserStep | Old Position: " + std::to_string(oldPosition) + 
                       ", New Position: " + std::to_string(Steps) + 
                       ", Offset: " + std::to_string(offset), 
                       LogLevel::DEBUG, DeviceType::MAIN);
            
            // 备份原始的Min和Max Limit值，以便在同步失败时还原
            int oldMinPosition = focuserMinPosition;
            int oldMaxPosition = focuserMaxPosition;
            
            // 同步位置
            bool syncSuccess = false;
            if (dpFocuser != NULL)
            {
                // INDI模式
                indi_Client->syncFocuserPosition(dpFocuser, Steps);
                sleep(1);
                CurrentPosition = FocuserControl_getPosition();
                
                // 检查位置是否真的改变了（如果没变，说明同步失败）
                if (CurrentPosition == Steps)
                {
                    syncSuccess = true;
                }
                else
                {
                    Logger::Log("SyncFocuserStep | Warning: Position sync failed. Current position: " + 
                               std::to_string(CurrentPosition) + ", Expected: " + std::to_string(Steps), 
                               LogLevel::WARNING, DeviceType::FOCUSER);
                }
            }
            else if (focuserSdkReady)
            {
                // SDK模式：异步调用SyncPosition命令
                if (sdkFocuserExec && sdkFocuserExec->isRunning())
                {
                    const SdkDeviceHandle handleSnap = sdkFocuserHandle;
                    const uint64_t epochSnap = sdkFocuserOpEpoch.load(std::memory_order_relaxed);
                    
                    sdkFocuserExec->post([this, handleSnap, Steps, epochSnap]() {
                        if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
                            return;
                        
                        SdkCommand syncCmd{SdkCommandType::Custom, "SyncPosition", Steps};
                        // 直接通过设备句柄调用，无需指定驱动名称
                        SdkResult syncRes = SdkManager::instance().callByHandle(handleSnap, syncCmd);
                        
                        if (!syncRes.success)
                        {
                            QMetaObject::invokeMethod(
                                this,
                                [this, msg = syncRes.message]() {
                                    Logger::Log("SyncFocuserStep | SDK SyncPosition failed: " + msg,
                                                LogLevel::ERROR, DeviceType::FOCUSER);
                                },
                                Qt::QueuedConnection);
                        }
                        else
                        {
                            QMetaObject::invokeMethod(
                                this,
                                [this, Steps]() {
                                    Logger::Log("SyncFocuserStep | SDK SyncPosition success, position: " + std::to_string(Steps),
                                                LogLevel::DEBUG, DeviceType::FOCUSER);
                                    // 更新位置缓存
                                    sdkFocuserPosCache.store(Steps);
                                    sdkFocuserPosValid.store(true);
                                    CurrentPosition = Steps;
                                    emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
                                },
                                Qt::QueuedConnection);
                        }
                    });
                }
                
                // 更新当前位置（等待异步操作完成）
                QThread::msleep(100);
                CurrentPosition = Steps;
                syncSuccess = true; // SDK模式假设成功（异步验证）
            }
            
            // 只有在同步成功时才更新Min和Max Limit
            if (syncSuccess)
            {
                // 定义电调的绝对物理限制范围（防止超出硬件范围）
                int ABSOLUTE_MIN_LIMIT = -100000;
                int ABSOLUTE_MAX_LIMIT = 100000;
                
                // 根据模式获取不同的边界限制
                if (dpFocuser != NULL)
                {
                    // INDI模式：从设备获取真实的硬件范围
                    int deviceMin, deviceMax, deviceStep, deviceValue;
                    indi_Client->getFocuserRange(dpFocuser, deviceMin, deviceMax, deviceStep, deviceValue);
                    if (deviceMin != -1 && deviceMax != -1)
                    {
                        ABSOLUTE_MIN_LIMIT = deviceMin;
                        ABSOLUTE_MAX_LIMIT = deviceMax;
                        Logger::Log("SyncFocuserStep | Using INDI device range: [" + 
                                   std::to_string(ABSOLUTE_MIN_LIMIT) + ", " + 
                                   std::to_string(ABSOLUTE_MAX_LIMIT) + "]", 
                                   LogLevel::DEBUG, DeviceType::FOCUSER);
                    }
                }
                else if (focuserSdkReady)
                {
                    // SDK模式：使用自定义的固定范围
                    ABSOLUTE_MIN_LIMIT = -100000;
                    ABSOLUTE_MAX_LIMIT = 100000;
                    Logger::Log("SyncFocuserStep | Using SDK custom range: [" + 
                               std::to_string(ABSOLUTE_MIN_LIMIT) + ", " + 
                               std::to_string(ABSOLUTE_MAX_LIMIT) + "]", 
                               LogLevel::DEBUG, DeviceType::FOCUSER);
                }
                
                // 计算偏移后的新限制值
                // 当同步位置时，范围需要相应调整：原范围 + offset
                // 坐标变换逻辑：将位置 P_old 重新定义为 P_new，offset = P_new - P_old
                // 对于原坐标系中读数为 x_old 的位置，新坐标系中读数变为：x_new = x_old + offset
                // 例如：当前位置 -11488 同步到 0，offset = 0 - (-11488) = 11488
                //       如果原范围是 [-36629, 2811]，新范围应该是 [-36629+11488, 2811+11488] = [-25141, 14299]
                //       这样新的当前位置 0 就在新范围内
                int newMinPosition = focuserMinPosition;
                int newMaxPosition = focuserMaxPosition;
                
                if (focuserMinPosition != -1)
                {
                    newMinPosition = focuserMinPosition + offset;
                }
                
                if (focuserMaxPosition != -1)
                {
                    newMaxPosition = focuserMaxPosition + offset;
                }
                
                // 验证新的限制值是否在合理范围内
                bool limitsValid = true;
                std::string warningMsg;
                
                if (newMinPosition < ABSOLUTE_MIN_LIMIT || newMinPosition > ABSOLUTE_MAX_LIMIT)
                {
                    limitsValid = false;
                    warningMsg += "New Min Limit (" + std::to_string(newMinPosition) + 
                                 ") exceeds absolute range [" + std::to_string(ABSOLUTE_MIN_LIMIT) + 
                                 ", " + std::to_string(ABSOLUTE_MAX_LIMIT) + "]. ";
                }
                
                if (newMaxPosition < ABSOLUTE_MIN_LIMIT || newMaxPosition > ABSOLUTE_MAX_LIMIT)
                {
                    limitsValid = false;
                    warningMsg += "New Max Limit (" + std::to_string(newMaxPosition) + 
                                 ") exceeds absolute range [" + std::to_string(ABSOLUTE_MIN_LIMIT) + 
                                 ", " + std::to_string(ABSOLUTE_MAX_LIMIT) + "]. ";
                }
                
                // 验证Min < Max
                if (newMinPosition >= newMaxPosition)
                {
                    limitsValid = false;
                    warningMsg += "New Min Limit (" + std::to_string(newMinPosition) + 
                                 ") must be less than Max Limit (" + std::to_string(newMaxPosition) + "). ";
                }
                
                if (limitsValid)
                {
                    // 对Min和Max Limit进行同样幅度的偏移
                    if (focuserMinPosition != -1)
                    {
                        focuserMinPosition = newMinPosition;
                        
                        Tools::saveParameter("Focuser", "focuserMinPosition", QString::number(focuserMinPosition));
                        Logger::Log("SyncFocuserStep | Updated Min Limit: " + std::to_string(focuserMinPosition), 
                                   LogLevel::DEBUG, DeviceType::MAIN);
                    }
                    
                    if (focuserMaxPosition != -1)
                    {
                        focuserMaxPosition = newMaxPosition;
                        Tools::saveParameter("Focuser", "focuserMaxPosition", QString::number(focuserMaxPosition));
                        Logger::Log("SyncFocuserStep | Updated Max Limit: " + std::to_string(focuserMaxPosition), 
                                   LogLevel::DEBUG, DeviceType::MAIN);
                    }
                    
                    // 推送更新后的位置和限制范围
                    Logger::Log("Focuser Current Position: " + std::to_string(CurrentPosition), LogLevel::DEBUG, DeviceType::MAIN);
                    emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
                    emit wsThread->sendMessageToClient("FocuserLimit:" + QString::number(focuserMinPosition) + ":" + QString::number(focuserMaxPosition));
                }
                else
                {
                    // 限制值超出范围，保持原值并发出警告
                    Logger::Log("SyncFocuserStep | Warning: " + warningMsg + 
                               "Limits not updated. Current limits: [" + std::to_string(focuserMinPosition) + 
                               ", " + std::to_string(focuserMaxPosition) + "]", 
                               LogLevel::WARNING, DeviceType::FOCUSER);
                    
                    // 向前端发送警告消息
                    emit wsThread->sendMessageToClient("SyncFocuserStepWarning:The calculated limits would exceed safe range. " +
                                                     QString("Min: %1, Max: %2. Limits not updated.")
                                                     .arg(newMinPosition).arg(newMaxPosition));
                    
                    // 推送位置（已更新）和限制范围（未更新）
                    emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
                    emit wsThread->sendMessageToClient("FocuserLimit:" + QString::number(focuserMinPosition) + ":" + QString::number(focuserMaxPosition));
                }
            }
            else
            {
                // 同步失败，还原原始数据并发送警告
                focuserMinPosition = oldMinPosition;
                focuserMaxPosition = oldMaxPosition;
                
                Logger::Log("SyncFocuserStep | Sync failed. Position value may be out of range or invalid. " +
                           std::string("Min/Max limits restored to: [") + std::to_string(oldMinPosition) + 
                           ", " + std::to_string(oldMaxPosition) + "]", 
                           LogLevel::WARNING, DeviceType::FOCUSER);
                
                // 向前端发送警告消息
                emit wsThread->sendMessageToClient("SyncFocuserStepError:Position sync failed. The value " + 
                                                 QString::number(Steps) + 
                                                 " may be out of range. Please check the focuser limits.");
                
                // 推送当前的位置和未改变的限制范围
                emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
                emit wsThread->sendMessageToClient("FocuserLimit:" + QString::number(focuserMinPosition) + ":" + QString::number(focuserMaxPosition));
            }
        }
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "StepsPerClick"){
        Logger::Log("StepsPerClick:" + parts[1].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveParameter("Focuser", "StepsPerClick", parts[1].trimmed());
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "MinLimit")
    {
        Logger::Log("MinLimit:" + parts[1].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        const int frontReportedMinLimit = parts[1].trimmed().toInt();
        const bool focuserAvailable = (dpFocuser != NULL || isFocuserSDK());
        if (!focuserAvailable)
        {
            Logger::Log("MinLimit rejected: focuser not connected (INDI/SDK unavailable)", LogLevel::WARNING, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient("focusMoveFailed:电调未连接，无法设置左边界。");
            return;
        }

        // 清理先前 stop 流程可能残留的异步位置刷新，避免在设置边界窗口期插入二次位置变更
        if (updatePositionTimer != nullptr)
        {
            updatePositionTimer->stop();
            updatePositionTimer->deleteLater();
            updatePositionTimer = nullptr;
        }
        
        // 参考自动移动收尾逻辑：先停止移动，再延迟读取稳定位置，避免惯性/回包延迟导致边界取值漂移。
        if (isFocusMoveDone)
        {
            FocuserControlStop(false, true);
        }
        QThread::msleep(220);

        // 先读取一次当前位置（用于同步/诊断），边界值仍以后续“最新稳定位置”为准
        const int currentOnce = FocuserControl_getPosition();
        Logger::Log("MinLimit | current position before stable read: " + std::to_string(currentOnce),
                    LogLevel::INFO, DeviceType::FOCUSER);

        int stablePosition = 0;
        if (tryReadStableFocuserPosition(stablePosition, 1800, 160, 2, 6))
        {
            CurrentPosition = stablePosition;
            Logger::Log("MinLimit | use stable position as boundary: stable=" + std::to_string(stablePosition) +
                            ", frontClicked=" + std::to_string(frontReportedMinLimit),
                        LogLevel::INFO, DeviceType::FOCUSER);
        }
        else
        {
            Logger::Log("MinLimit rejected: stable position unavailable after delay, frontClicked=" + std::to_string(frontReportedMinLimit),
                        LogLevel::WARNING, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient("focusMoveFailed:当前位置未稳定，请停止移动后重试。");
            return;
        }
        const int MinLimit = stablePosition;

        int absoluteMin = -100000, absoluteMax = 100000;
        getFocuserAbsoluteRange(absoluteMin, absoluteMax);
        if (MinLimit < absoluteMin || MinLimit > absoluteMax)
        {
            Logger::Log("MinLimit rejected: out of physical range. value=" + std::to_string(MinLimit) +
                            ", range=[" + std::to_string(absoluteMin) + ", " + std::to_string(absoluteMax) + "]",
                        LogLevel::WARNING, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient("focusMoveFailed:左边界超出电调物理范围，请重新设置。");
            return;
        }

        if (focuserMaxPosition != -1 && MinLimit >= focuserMaxPosition)
        {
            Logger::Log("MinLimit rejected: new MinLimit >= current MaxLimit", LogLevel::WARNING, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient("focusMoveFailed:左边界必须小于右边界，请重新设置。");
            return;
        }
        Tools::saveParameter("Focuser", "focuserMinPosition", QString::number(MinLimit));
        focuserMinPosition = MinLimit;
        Logger::Log("MinLimit accepted: " + std::to_string(MinLimit), LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
        emit wsThread->sendMessageToClient("FocuserLimit:" + QString::number(focuserMinPosition) + ":" + QString::number(focuserMaxPosition));
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "MaxLimit")
    {
        Logger::Log("MaxLimit:" + parts[1].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        const int frontReportedMaxLimit = parts[1].trimmed().toInt();
        const bool focuserAvailable = (dpFocuser != NULL || isFocuserSDK());
        if (!focuserAvailable)
        {
            Logger::Log("MaxLimit rejected: focuser not connected (INDI/SDK unavailable)", LogLevel::WARNING, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient("focusMoveFailed:电调未连接，无法设置右边界。");
            return;
        }

        // 清理先前 stop 流程可能残留的异步位置刷新，避免在设置边界窗口期插入二次位置变更
        if (updatePositionTimer != nullptr)
        {
            updatePositionTimer->stop();
            updatePositionTimer->deleteLater();
            updatePositionTimer = nullptr;
        }
        
        // 参考自动移动收尾逻辑：先停止移动，再延迟读取稳定位置，避免惯性/回包延迟导致边界取值漂移。
        if (isFocusMoveDone)
        {
            FocuserControlStop(false, true);
        }
        QThread::msleep(220);

        // 先读取一次当前位置（用于同步/诊断），边界值仍以后续“最新稳定位置”为准
        const int currentOnce = FocuserControl_getPosition();
        Logger::Log("MaxLimit | current position before stable read: " + std::to_string(currentOnce),
                    LogLevel::INFO, DeviceType::FOCUSER);

        int stablePosition = 0;
        if (tryReadStableFocuserPosition(stablePosition, 1800, 160, 2, 6))
        {
            CurrentPosition = stablePosition;
            Logger::Log("MaxLimit | use stable position as boundary: stable=" + std::to_string(stablePosition) +
                            ", frontClicked=" + std::to_string(frontReportedMaxLimit),
                        LogLevel::INFO, DeviceType::FOCUSER);
        }
        else
        {
            Logger::Log("MaxLimit rejected: stable position unavailable after delay, frontClicked=" + std::to_string(frontReportedMaxLimit),
                        LogLevel::WARNING, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient("focusMoveFailed:当前位置未稳定，请停止移动后重试。");
            return;
        }
        const int MaxLimit = stablePosition;

        int absoluteMin = -100000, absoluteMax = 100000;
        getFocuserAbsoluteRange(absoluteMin, absoluteMax);
        if (MaxLimit < absoluteMin || MaxLimit > absoluteMax)
        {
            Logger::Log("MaxLimit rejected: out of physical range. value=" + std::to_string(MaxLimit) +
                            ", range=[" + std::to_string(absoluteMin) + ", " + std::to_string(absoluteMax) + "]",
                        LogLevel::WARNING, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient("focusMoveFailed:右边界超出电调物理范围，请重新设置。");
            return;
        }

        if (focuserMinPosition != -1 && MaxLimit <= focuserMinPosition)
        {
            Logger::Log("MaxLimit rejected: new MaxLimit <= current MinLimit", LogLevel::WARNING, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient("focusMoveFailed:右边界必须大于左边界，请重新设置。");
            return;
        }
        Tools::saveParameter("Focuser", "focuserMaxPosition", QString::number(MaxLimit));
        focuserMaxPosition = MaxLimit;
        Logger::Log("MaxLimit accepted: " + std::to_string(MaxLimit), LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
        emit wsThread->sendMessageToClient("FocuserLimit:" + QString::number(focuserMinPosition) + ":" + QString::number(focuserMaxPosition));
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "Backlash")
    {
        Logger::Log("Backlash:" + parts[1].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        int Backlash = parts[1].trimmed().toInt();
        if (dpFocuser != NULL)
        {
            Tools::saveParameter("Focuser", "Backlash", parts[1].trimmed());
            autofocusBacklashCompensation = Backlash;
        }
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "Coarse Step Divisions")
    {
        Logger::Log("Coarse Step Divisions:" + parts[1].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        int divisions = parts[1].trimmed().toInt();
        if (divisions <= 0)
        {
            divisions = 10;
        }
        autoFocusCoarseDivisions = divisions;
        Tools::saveParameter("Focuser", "coarseStepDivisions", QString::number(divisions));
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "AutoFocus Exposure Time (ms)")
    {
        QString valueStr = parts[1].trimmed();
        int exposureMs = valueStr.toInt();
        if (exposureMs <= 0)
        {
            exposureMs = 1000;
        }

        autoFocusExposureTime = exposureMs;

        Logger::Log("AutoFocus Exposure Time (ms) is set to " + std::to_string(autoFocusExposureTime),
                    LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveParameter("Focuser", "AutoFocusExposureTime(ms)", valueStr);

        if (autoFocus != nullptr)
        {
            autoFocus->setDefaultExposureTime(autoFocusExposureTime);
        }
    }
    else if (parts.size() == 3 && parts[0].trimmed() == "setROIPosition")
    {
        Logger::Log("setROIPosition:" + parts[1].trimmed().toStdString() + "*" + parts[2].trimmed().toStdString(), LogLevel::INFO, DeviceType::MAIN);
        const int roiBox = roiAndFocuserInfo.count("BoxSideLength")
            ? static_cast<int>(std::lround(roiAndFocuserInfo["BoxSideLength"]))
            : std::max(2, BoxSideLength);
        const QPointF snapped = snapRoiOriginToBayerSafePhase(parts[1].trimmed().toDouble(),
                                                              parts[2].trimmed().toDouble(),
                                                              roiBox, roiBox);
        roiAndFocuserInfo["ROI_x"] = snapped.x();
        roiAndFocuserInfo["ROI_y"] = snapped.y();
        Logger::Log("setROIPosition | snapped ROI to Bayer-safe origin (" +
                        std::to_string(snapped.x()) + "," + std::to_string(snapped.y()) + ")",
                    LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "RedBoxSizeChange")
    {
        Logger::Log("RedBoxSizeChange:" + parts[1].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        BoxSideLength = parts[1].trimmed().toInt();
        roiAndFocuserInfo["BoxSideLength"] = BoxSideLength;
        Tools::saveParameter("MainCamera", "RedBoxSize", parts[1].trimmed());
        Logger::Log("RedBoxSizeChange:" + std::to_string(BoxSideLength), LogLevel::DEBUG, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "ROICalcMode")
    {
        const QString mode = parts[1].trimmed().toLower();
        roiUseSelfCalcParams = (mode == "roi" || mode == "self");
        Tools::saveParameter("MainCamera", "ROICalcMode",
                             roiUseSelfCalcParams ? QStringLiteral("roi") : QStringLiteral("full"));
        Logger::Log("ROICalcMode is set to " +
                        std::string(roiUseSelfCalcParams ? "roi(self-calc)" : "full(reuse-full-frame)"),
                    LogLevel::INFO, DeviceType::FOCUSER);
    }

    else if (message.startsWith("AutoFocusConfirm:")) // [AUTO_FOCUS_UI_ENHANCEMENT]
    {
        const QString prefix = "AutoFocusConfirm:";
        QString mode = message.mid(prefix.length()).trimmed();

        // 发送自动对焦开始事件到前端
        if (mode.compare("No", Qt::CaseInsensitive) != 0) {
            if (isAutoFocus) {
                Logger::Log("AutoFocus already started", LogLevel::INFO, DeviceType::MAIN);
                return;
            }
        }

        if (mode.isEmpty() || mode == "Yes" || mode == "Coarse") {
            // 完整自动对焦流程：粗调 + 精调 + super-fine
            // 增加模式标记：full，便于前端区分不同自动对焦模式的 UI 行为
            emit wsThread->sendMessageToClient("AutoFocusStarted:full:自动对焦已开始");
            startAutoFocus();
        }
        else if (mode == "Fine") {
            // 仅从当前位置执行本地精调：先拍当前位置，再围绕当前位置双向展开
            // 增加模式标记：fine（仅精调模式）
            emit wsThread->sendMessageToClient("AutoFocusStarted:fine:自动对焦已开始");
            startAutoFocusFineHFROnly();
        }
        else { // No 或未知模式，视为取消
            Logger::Log("用户取消自动对焦", LogLevel::INFO, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("AutoFocusCancelled:用户已取消自动对焦");
        }
    }
    else if (message.startsWith("AutoFocusCoarseRetryDecision:"))
    {
        const QString prefix = "AutoFocusCoarseRetryDecision:";
        const QString decision = message.mid(prefix.length()).trimmed();
        const bool accepted =
            decision.compare("Yes", Qt::CaseInsensitive) == 0 ||
            decision.compare("True", Qt::CaseInsensitive) == 0 ||
            decision == "1";

        if (autoFocus == nullptr || !autoFocus->isRunning()) {
            Logger::Log(QString("忽略粗调补扫确认结果：自动对焦未运行，decision=%1")
                            .arg(decision).toStdString(),
                        LogLevel::WARNING, DeviceType::FOCUSER);
            return;
        }

        Logger::Log(QString("收到粗调补扫确认结果：%1")
                        .arg(accepted ? "执行补扫" : "放弃补扫").toStdString(),
                    LogLevel::INFO, DeviceType::FOCUSER);
        autoFocus->handleCoarseRetryDecision(accepted);
    }
    else if (message == "StopAutoFocus")
    {
        if (!isAutoFocus) {
            Logger::Log("AutoFocus not started, stopAutoFocus failed", LogLevel::INFO, DeviceType::MAIN);
            return;
        }
        Logger::Log("StopAutoFocus", LogLevel::DEBUG, DeviceType::MAIN);
        isAutoFocus = false;
        autoFocus->stopAutoFocus();
        autoFocus->deleteLater();
        cleanupAutoFocusConnections();
        autoFocus = nullptr;
        emit wsThread->sendMessageToClient("AutoFocusEnded:自动对焦已结束");
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "FocuserMoveState")
    {
        Logger::Log("FocuserMoveState ...", LogLevel::DEBUG, DeviceType::FOCUSER);
        if (parts[1].trimmed() == "true")
        {
            focusMoveEndTime = 2;
        }
        Logger::Log("FocuserMoveState finish!", LogLevel::DEBUG, DeviceType::FOCUSER);
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "FocusLoopShooting")
    {
        Logger::Log("FocusLoopShooting ...", LogLevel::DEBUG, DeviceType::FOCUSER);
        focusLoopShooting(parts[1].trimmed() == "true");
        Logger::Log("FocusLoopShooting finish!", LogLevel::DEBUG, DeviceType::FOCUSER);
    }
    else if (parts[0].trimmed() == "getFocuserLoopingState")
    {
        Logger::Log("getFocuserLoopingState ...", LogLevel::DEBUG, DeviceType::FOCUSER);
        getFocuserLoopingState();
        Logger::Log("getFocuserLoopingState finish!", LogLevel::DEBUG, DeviceType::FOCUSER);
    }
    else if (parts[0].trimmed() == "getROIInfo")
    {
        Logger::Log("getRedBoxState ...", LogLevel::DEBUG, DeviceType::MAIN);
        sendRoiInfo();
        Logger::Log("getRedBoxState finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (parts[0].trimmed() == "sendRedBoxState" && parts.size() == 4)
    {
        Logger::Log("sendRedBoxState ...", LogLevel::DEBUG, DeviceType::MAIN);
        if (parts[1].trimmed().toInt() == BoxSideLength && parts[2].trimmed().toDouble() == roiAndFocuserInfo["ROI_x"] && parts[3].trimmed().toDouble() == roiAndFocuserInfo["ROI_y"])
        {
            return;
        }
        BoxSideLength = parts[1].trimmed().toInt();
        roiAndFocuserInfo["BoxSideLength"] = BoxSideLength;
        const QPointF snapped = snapRoiOriginToBayerSafePhase(parts[2].trimmed().toDouble(),
                                                              parts[3].trimmed().toDouble(),
                                                              BoxSideLength, BoxSideLength);
        roiAndFocuserInfo["ROI_x"] = snapped.x();
        roiAndFocuserInfo["ROI_y"] = snapped.y();
        Tools::saveParameter("MainCamera", "ROI_x", QString::number(snapped.x(), 'g', 9));
        Tools::saveParameter("MainCamera", "ROI_y", QString::number(snapped.y(), 'g', 9));
        Logger::Log("sendRedBoxState | snapped ROI to Bayer-safe origin (" +
                        std::to_string(snapped.x()) + "," + std::to_string(snapped.y()) + ")",
                    LogLevel::DEBUG, DeviceType::MAIN);
        Logger::Log("sendRedBoxState finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (parts[0].trimmed() == "focusMoveToMin")
    {
        Logger::Log("focusMoveToMin | 已停用，拒绝执行旧自动校准命令", LogLevel::WARNING, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("focusMoveFailed:旧版自动校准命令 focusMoveToMin 已停用，请使用手动设置左右边界。");
        // [停用保留] 旧逻辑如下，保留以便回溯：
        // Logger::Log("focusMoveToMin ...", LogLevel::DEBUG, DeviceType::FOCUSER);
        // focusMoveToMin();
        // Logger::Log("focusMoveToMin finish!", LogLevel::DEBUG, DeviceType::FOCUSER);
    }
    else if (parts[0].trimmed() == "focusMoveToMax")
    {
        Logger::Log("focusMoveToMax | 已停用，拒绝执行旧自动校准命令", LogLevel::WARNING, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("focusMoveFailed:旧版自动校准命令 focusMoveToMax 已停用，请使用手动设置左右边界。");
        // [停用保留] 旧逻辑如下，保留以便回溯：
        // Logger::Log("focusMoveToMax ...", LogLevel::DEBUG, DeviceType::FOCUSER);
        // focusMoveToMax();
        // Logger::Log("focusMoveToMax finish!", LogLevel::DEBUG, DeviceType::FOCUSER);
    }
    else if (parts[0].trimmed() == "focusSetTravelRange")
    {
        Logger::Log("focusSetTravelRange | 已停用，拒绝执行旧自动校准命令", LogLevel::WARNING, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("focusMoveFailed:旧版自动校准命令 focusSetTravelRange 已停用，请使用手动设置左右边界。");
        // [停用保留] 旧逻辑如下，保留以便回溯：
        // Logger::Log("focusSetTravelRange ...", LogLevel::DEBUG, DeviceType::FOCUSER);
        // focusSetTravelRange();
        // Logger::Log("focusSetTravelRange finish!", LogLevel::DEBUG, DeviceType::FOCUSER);
    }
    else if (parts[0].trimmed() == "getFocuserParameters")
    {
        Logger::Log("getFocuserParameters ...", LogLevel::DEBUG, DeviceType::FOCUSER);
        getFocuserParameters();
        Logger::Log("getFocuserParameters finish!", LogLevel::DEBUG, DeviceType::FOCUSER);
    }
    else if(parts[0].trimmed() == "getFocuserState")
    {
        Logger::Log("getFocuserState ...", LogLevel::DEBUG, DeviceType::FOCUSER);
        getFocuserState();

        Logger::Log("getFocuserState finish!", LogLevel::DEBUG, DeviceType::FOCUSER);
    }
    else if (parts[0].trimmed() == "SolveCurrentPosition")
    {
        Logger::Log("SolveCurrentPosition ...", LogLevel::DEBUG, DeviceType::FOCUSER);
        solveCurrentPosition();
        Logger::Log("SolveCurrentPosition finish!", LogLevel::DEBUG, DeviceType::FOCUSER);
    }
    };

    run();
    return true;
}

bool MainWindow::handleScheduleCommand(const QString &message, const QStringList &parts)
{
    const QString command = parts.isEmpty() ? message.trimmed() : parts[0].trimmed();
    if (!(
            command == QLatin1String("Self Exposure Time (ms)") ||
            command == QLatin1String("ScheduleTabelData") ||
            command == QLatin1String("StopSchedule") ||
            command == QLatin1String("CaptureImageSave") ||
            command == QLatin1String("StagingScheduleData") ||
            command == QLatin1String("getStagingScheduleData") ||
            command == QLatin1String("saveSchedulePreset") ||
            command == QLatin1String("loadSchedulePreset") ||
            command == QLatin1String("deleteSchedulePreset") ||
            command == QLatin1String("listSchedulePresets") ||
            command == QLatin1String("ExpTimeList") ||
            command == QLatin1String("getExpTimeList") ||
            command == QLatin1String("getCaptureStatus") ||
            command == QLatin1String("SetMainCameraLoopCaptureNum")))
    {
        return false;
    }

    auto run = [this, &message, &parts]() {
    if (parts.size() == 2 && parts[0].trimmed() == "Self Exposure Time (ms)")
    {
        Logger::Log("Self Exposure Time (ms) is set to " + parts[1].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveParameter("MainCamera", "SelfExposureTime(ms)", parts[1].trimmed());
    }
    

    else if (parts[0].trimmed() == "ScheduleTabelData")
    {
        ScheduleTabelData(message);
    }
    else if (message == "StopSchedule")
    {
        Logger::Log("StopSchedule !", LogLevel::DEBUG, DeviceType::MAIN);
    StopSchedule = true;
    isScheduleRunning = false;
    // 立即通知前端计划任务已停止，避免仅依赖进度推断带来的延迟
    emit wsThread->sendMessageToClient("ScheduleRunning:false");
        
        // 如果自动对焦正在运行（特别是由计划任务表触发的），也要停止自动对焦
        if (isAutoFocus && autoFocus != nullptr)
        {
            Logger::Log("停止计划任务表时，检测到自动对焦正在运行，同时停止自动对焦", LogLevel::INFO, DeviceType::MAIN);
            isScheduleTriggeredAutoFocus = false; // 清除标志，避免自动对焦完成后继续执行拍摄
            autoFocus->stopAutoFocus();
            cleanupAutoFocusConnections();
            autoFocus->deleteLater();
            autoFocus = nullptr;
            isAutoFocus = false;
            emit wsThread->sendMessageToClient("AutoFocusEnded:自动对焦已停止（计划任务表已暂停）");
        }
        
        // 立即停止曝光延迟定时器
        bool wasActive = exposureDelayTimer.isActive();
        exposureDelayTimer.stop();
        exposureDelayTimer.disconnect();
        if (wasActive || exposureDelayElapsed_ms > 0)
        {
            Logger::Log(("Exposure delay timer stopped immediately (wasActive: " + QString::number(wasActive ? 1 : 0) + ", elapsed: " + QString::number(exposureDelayElapsed_ms) + " ms)").toStdString(), LogLevel::INFO, DeviceType::MAIN);
            qDebug() << "Exposure delay timer stopped immediately (wasActive:" << wasActive << ", elapsed:" << exposureDelayElapsed_ms << "ms)";
            exposureDelayElapsed_ms = 0; // 重置已过去的时间
        }
    }

    else if (message == "CaptureImageSave")
    {
        Logger::Log("CaptureImageSave ...", LogLevel::DEBUG, DeviceType::MAIN);
        CaptureImageSave();
        Logger::Log("CaptureImageSave finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (parts[0].trimmed() == "StagingScheduleData")
    {
        Logger::Log("StagingScheduleData ...", LogLevel::DEBUG, DeviceType::MAIN);
        isStagingScheduleData = true;
        StagingScheduleData = message;
        Logger::Log("StagingScheduleData finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (message == "getStagingScheduleData")
    {
        Logger::Log("getStagingScheduleData ...", LogLevel::DEBUG, DeviceType::MAIN);
        getStagingScheduleData();
        Logger::Log("getStagingScheduleData finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    // ---------- Schedule presets (任务计划表预设管理) ----------
    else if (parts[0].trimmed() == "saveSchedulePreset" && parts.size() >= 3)
    {
        // 格式：saveSchedulePreset:<name>:<rawData>
        QString presetName = parts[1].trimmed();
        // 重新拼接 data（防止 data 中本身包含 ':' 被 split 掉）
        QString rawData;
        for (int i = 2; i < parts.size(); ++i)
        {
            if (i > 2)
                rawData += ":";
            rawData += parts[i];
        }

        Logger::Log("saveSchedulePreset | name=" + presetName.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveSchedulePreset(presetName, rawData);
    }
    else if (parts[0].trimmed() == "loadSchedulePreset" && parts.size() == 2)
    {
        // 格式：loadSchedulePreset:<name>
        QString presetName = parts[1].trimmed();
        Logger::Log("loadSchedulePreset | name=" + presetName.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        QString data = Tools::readSchedulePreset(presetName);
        if (!data.isEmpty())
        {
            // 复用现有 StagingScheduleData 协议，直接推送给前端
            QString messageOut = "StagingScheduleData:" + data;
            emit wsThread->sendMessageToClient(messageOut);
        }
    }
    else if (parts[0].trimmed() == "deleteSchedulePreset" && parts.size() == 2)
    {
        // 格式：deleteSchedulePreset:<name>
        QString presetName = parts[1].trimmed();
        Logger::Log("deleteSchedulePreset | name=" + presetName.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        bool ok = Tools::deleteSchedulePreset(presetName);
        if (!ok)
        {
            Logger::Log("deleteSchedulePreset | failed to delete preset: " + presetName.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
        }
    }
    else if (message == "listSchedulePresets")
    {
        // 返回格式：SchedulePresetList:name1;name2;name3
        Logger::Log("listSchedulePresets ...", LogLevel::DEBUG, DeviceType::MAIN);
        QStringList names = Tools::listSchedulePresets();
        QString payload = "SchedulePresetList:";
        for (int i = 0; i < names.size(); ++i)
        {
            if (i > 0)
                payload += ";";
            payload += names.at(i);
        }
        emit wsThread->sendMessageToClient(payload);
        Logger::Log("listSchedulePresets finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (parts[0].trimmed() == "ExpTimeList")
    {
        Logger::Log("ExpTimeList ...", LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveExpTimeList(message);
        Logger::Log("ExpTimeList finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (message == "getExpTimeList")
    {
        Logger::Log("getExpTimeList ...", LogLevel::DEBUG, DeviceType::MAIN);
        if (Tools::readExpTimeList() != QString())
        {
            emit wsThread->sendMessageToClient(Tools::readExpTimeList());
        }
        Logger::Log("getExpTimeList finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (message == "getCaptureStatus")
    {
        Logger::Log("getCaptureStatus ...", LogLevel::DEBUG, DeviceType::MAIN);
        if (glMainCameraStatu == "Exposuring")
        {
            if (isFocusLoopShooting)
            {
                emit wsThread->sendMessageToClient("CameraInExposuring:False");
            }
            else
            {
                if (isFocusLoopShooting)
                {
                    emit wsThread->sendMessageToClient("CameraInExposuring:False");
                }
                else
                {
                    emit wsThread->sendMessageToClient("CameraInExposuring:True");
                }
            }
        }
        Logger::Log("getCaptureStatus finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if(parts[0].trimmed() == "SetMainCameraLoopCaptureNum")
    {
        Logger::Log("SetMainCameraLoopCaptureNum ...", LogLevel::DEBUG, DeviceType::MAIN);
        LoopCaptureNum = parts[1].toInt();
        Logger::Log("SetMainCameraLoopCaptureNum finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    };

    run();
    return true;
}
