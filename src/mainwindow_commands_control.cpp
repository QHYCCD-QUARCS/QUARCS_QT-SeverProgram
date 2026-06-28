#include "mainwindow_command_support.h"

bool MainWindow::handleGuiderCommand(const QString &message, const QStringList &parts)
{
    const QString command = parts.isEmpty() ? message.trimmed() : parts[0].trimmed();
    if (!(
            command == QLatin1String("SetGuiderOffset") ||
            command == QLatin1String("getStagingGuiderData") ||
            command == QLatin1String("ClearCalibrationData") ||
            command == QLatin1String("getGuiderStatus") ||
            command == QLatin1String("GuiderSwitch") ||
            command == QLatin1String("GuiderLoopExpSwitch") ||
            command == QLatin1String("GuiderExpTimeSwitch") ||
            command == QLatin1String("SetGuiderGain") ||
            command == QLatin1String("ClearDataPoints") ||
            command == QLatin1String("GuiderCanvasClick") ||
            command == QLatin1String("GuiderFocalLength") ||
            command == QLatin1String("GuiderPixelSize") ||
            command == QLatin1String("MultiStarGuider") ||
            command == QLatin1String("GuiderSearchBoxMode") ||
            command == QLatin1String("GuiderDecGuideDir") ||
            command == QLatin1String("CalibrationDuration") ||
            command == QLatin1String("RaAggression") ||
            command == QLatin1String("DecAggression")))
    {
        return false;
    }

    auto run = [this, &message, &parts]() {
    if (parts.size() == 2 && parts[0].trimmed() == "SetGuiderOffset")
    {
        guiderCameraOffset = parts[1].trimmed().toDouble();
        Logger::Log("SetGuiderOffset is set to " + std::to_string(guiderCameraOffset), LogLevel::DEBUG, DeviceType::GUIDER);
        Tools::saveParameter("Guider", "Offset", parts[1].trimmed());

        bool isGuiderSDK = (systemdevicelist.system_devices.size() > 1 &&
                            systemdevicelist.system_devices[1].isSDKConnect &&
                            sdkGuiderHandle != nullptr);

        if (isGuiderSDK)
        {
            SdkCommand setOffsetCmd;
            setOffsetCmd.type = SdkCommandType::Custom;
            setOffsetCmd.name = "SetOffset";
            setOffsetCmd.payload = guiderCameraOffset;
            SdkResult res = SdkManager::instance().callByHandle(sdkGuiderHandle, setOffsetCmd);
            if (!res.success) {
                Logger::Log("SetGuiderOffset | SDK SetOffset failed: " + res.message, LogLevel::ERROR, DeviceType::GUIDER);
            } else {
                glGuiderOffsetValue = static_cast<int>(guiderCameraOffset);
                Logger::Log("SetGuiderOffset | SDK SetOffset success", LogLevel::INFO, DeviceType::GUIDER);
            }
        }
        else if (dpGuider != NULL)
        {
            if (indi_Client->setCCDOffset(dpGuider, static_cast<int>(guiderCameraOffset)) == QHYCCD_SUCCESS)
            {
                glGuiderOffsetValue = static_cast<int>(guiderCameraOffset);
            }
        }

        emit wsThread->sendMessageToClient("GuiderOffsetRange:" + QString::number(glGuiderOffsetMin) + ":" +
                                           QString::number(glGuiderOffsetMax) + ":" +
                                           QString::number(glGuiderOffsetValue));
    }

    else if (message == "getStagingGuiderData")
    {
        Logger::Log("getStagingGuiderData ...", LogLevel::DEBUG, DeviceType::MAIN);
        getStagingGuiderData();
        Logger::Log("getStagingGuiderData finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (message == "ClearCalibrationData"
             || message == "clearGuiderData"
             || message == "PHD2Recalibrate")
    {
        Logger::Log(("BuiltInGuider | recalibration request received: " + message).toStdString(),
                    LogLevel::INFO, DeviceType::GUIDER);
        Logger::Log("BuiltInGuider | clear cached calibration/backlash and force recalibrate on next start",
                    LogLevel::INFO, DeviceType::GUIDER);
        guiderForceRecalibrateOnNextStart = true;
        if (guiderCore)
        {
            postGuiderCore(guiderCore, [](GuiderCore *core) { core->clearCachedCalibration(); });
            emit wsThread->sendMessageToClient("GuiderCoreInfo:CachedCalibrationAndBacklashCleared:WillRecalibrateOnNextStart");

            const bool guiderSdk =
                (systemdevicelist.system_devices.size() > 1 &&
                 systemdevicelist.system_devices[1].isSDKConnect &&
                 sdkGuiderHandle != nullptr);
            const bool guiderConnected = ((dpGuider != NULL && dpGuider->isConnected()) || guiderSdk);
            const guiding::State gs = guiderCoreStateCache;

            // OpenPHD2-style UX: if the user confirms recalibration while Loop is already active,
            // start the fresh calibration flow immediately instead of waiting for another click.
            if (guiderConnected && (gs == guiding::State::Looping || gs == guiding::State::Stopped))
            {
                Logger::Log("BuiltInGuider | loop is active, start forced recalibration immediately.",
                            LogLevel::INFO, DeviceType::GUIDER);
                postGuiderCore(guiderCore, [](GuiderCore *core) { core->startGuidingForceCalibrate(); });
                guiderForceRecalibrateOnNextStart = false;
            }
        }
        else
        {
            emit wsThread->sendMessageToClient("GuiderCoreInfo:GuiderCoreNotReady:PendingRecalibrateOnNextStart");
        }
    }
    else if(message == "getGuiderStatus")
    {
        Logger::Log("getGuiderStatus ...", LogLevel::DEBUG, DeviceType::GUIDER);
        const bool guiderSdk =
            (systemdevicelist.system_devices.size() > 1 &&
             systemdevicelist.system_devices[1].isSDKConnect &&
             sdkGuiderHandle != nullptr);
        const bool loopOn = (isGuiderLoopExp && ((dpGuider != NULL && dpGuider->isConnected()) || guiderSdk));
        bool guidingOn = false;
        QString guiderStatus;
        if (guiderCore)
        {
            switch (guiderCoreStateCache)
            {
            case guiding::State::Selecting:
                guidingOn = true;
                guiderStatus = QStringLiteral("InSelecting");
                break;
            case guiding::State::Calibrating:
                guidingOn = true;
                guiderStatus = QStringLiteral("InCalibration");
                break;
            case guiding::State::Guiding:
                guidingOn = true;
                guiderStatus = QStringLiteral("InGuiding");
                break;
            default:
                break;
            }
        }
        emit wsThread->sendMessageToClient(QString("GuiderSwitchStatus:%1").arg(guidingOn ? "true" : "false"));
        emit wsThread->sendMessageToClient(QString("GuiderLoopExpStatus:%1").arg(loopOn ? "true" : "false"));
        if (!guiderStatus.isEmpty())
            emit wsThread->sendMessageToClient("GuiderStatus:" + guiderStatus);
        publishGuiderErrorUnit(true, false);
        publishGuiderSearchBoxMode(false);
        
        Logger::Log("getGuiderStatus finish!", LogLevel::DEBUG, DeviceType::GUIDER);
    }

    else if (parts[0].trimmed() == "GuiderSwitch" && parts.size() == 2)
    {
        const bool guiderSdk =
            (systemdevicelist.system_devices.size() > 1 &&
             systemdevicelist.system_devices[1].isSDKConnect &&
             sdkGuiderHandle != nullptr);
        const bool guiderConnected = ((dpGuider != NULL && dpGuider->isConnected()) || guiderSdk);
        const bool wantOn = (parts[1].trimmed() == "true");

        if (!guiderCore)
        {
            Logger::Log("GuiderSwitch failed: guiderCore not initialized", LogLevel::ERROR, DeviceType::GUIDER);
            emit wsThread->sendMessageToClient("GuiderSwitchStatus:false");
            return;
        }

        if (wantOn)
        {
            if (!guiderConnected)
            {
                Logger::Log("GuiderSwitch failed: guider is not connected", LogLevel::WARNING, DeviceType::GUIDER);
                emit wsThread->sendMessageToClient("GuiderSwitchStatus:false");
                emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
                return;
            }

            Logger::Log("GuiderSwitch -> start built-in guider", LogLevel::INFO, DeviceType::GUIDER);
            if (guiderForceRecalibrateOnNextStart)
            {
                Logger::Log("GuiderSwitch -> start built-in guider with forced recalibration",
                            LogLevel::INFO, DeviceType::GUIDER);
                postGuiderCore(guiderCore, [](GuiderCore *core) { core->startGuidingForceCalibrate(); });
                guiderForceRecalibrateOnNextStart = false;
            }
            else
            {
                postGuiderCore(guiderCore, [](GuiderCore *core) { core->startGuiding(); });
            }
        }
        else
        {
            Logger::Log("GuiderSwitch -> stop built-in guider", LogLevel::INFO, DeviceType::GUIDER);
            postGuiderCore(guiderCore, [](GuiderCore *core) { core->stopGuiding(); });
        }
    }

    else if (parts[0].trimmed() == "GuiderLoopExpSwitch" && parts.size() == 2)
    {
        const bool guiderSdk =
            (systemdevicelist.system_devices.size() > 1 &&
             systemdevicelist.system_devices[1].isSDKConnect &&
             sdkGuiderHandle != nullptr);

        if ((dpGuider == NULL || !dpGuider->isConnected()) && !guiderSdk)
        {
            Logger::Log("GuiderLoopExpSwitch failed: guider is not connected", LogLevel::WARNING, DeviceType::GUIDER);
            emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
            emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
            return;
        }

        const bool wantOn = (parts[1].trimmed() == "true");
        if (wantOn && !isGuiderLoopExp)
        {
            Logger::Log(std::string("Start GuiderLoopExp (") + (guiderSdk ? "SDK" : "INDI") + ") ...",
                        LogLevel::INFO, DeviceType::GUIDER);
            if (guiderCore)
            {
                postGuiderCore(guiderCore, [](GuiderCore *core) { core->startLoop(); });
            }
            else
            {
                isGuiderLoopExp = true;
                guiderExposureInFlight = false;
                emit wsThread->sendMessageToClient("GuiderLoopExpStatus:true");
                emit wsThread->sendMessageToClient("GuiderUpdateStatus:1");

                if (guiderLoopTimer)
                    guiderLoopTimer->start(0);
            }
        }
        else if (!wantOn && isGuiderLoopExp)
        {
            Logger::Log(std::string("Stop GuiderLoopExp (") + (guiderSdk ? "SDK" : "INDI") + ") ...",
                        LogLevel::INFO, DeviceType::GUIDER);
            if (guiderCore)
            {
                postGuiderCore(guiderCore, [](GuiderCore *core) {
                    core->stopGuiding();
                    core->stopLoop();
                });
            }
            isGuiderLoopExp = false;
            guiderExposureInFlight = false;
            if (guiderLoopTimer)
                guiderLoopTimer->stop();

            // 尝试中止当前曝光（SDK/INDI）
            if (systemdevicelist.system_devices.size() > 1 &&
                systemdevicelist.system_devices[1].isSDKConnect &&
                sdkGuiderHandle != nullptr)
            {
                if (sdkGuiderExposureTimer)
                    sdkGuiderExposureTimer->stop();
                // Do not clear sdkGuiderFrameTaskInFlight here: a GetSingleFrame task may
                // still be inside the vendor SDK. Forcing the flag open allows another
                // QHY call on the same handle and can crash libqhyccd.
                const SdkDeviceHandle handleSnap = sdkGuiderHandle;
                SdkSerialExecutor *guiderExec = sdkGuiderCameraExecutor();
                if (guiderExec && guiderExec->isRunning())
                {
                    guiderExec->post([handleSnap]() {
                        SdkCommand abortCmd;
                        abortCmd.type = SdkCommandType::Custom;
                        abortCmd.name = "CancelExposure";
                        abortCmd.payload = std::any();
                        SdkManager::instance().callByHandle(handleSnap, abortCmd);
                    });
                }
            }
            else if (indi_Client && dpGuider)
            {
                indi_Client->setCCDAbortExposure(dpGuider);
            }

            emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
            emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
        }
        else
        {
            // 状态未变化
            emit wsThread->sendMessageToClient(QString("GuiderLoopExpStatus:%1").arg(isGuiderLoopExp ? "true" : "false"));
        }
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "GuiderExpTimeSwitch")
    {
        guiderExpMs = std::max(1, parts[1].toInt());
        auto p = guiderParamsCache;
        p.exposureMs = guiderExpMs;
        guiderParamsCache = p;
        postGuiderCore(guiderCore, [p](GuiderCore *core) { core->setParams(p); });
        setClientSettings("GuiderExposureMs", QString::number(guiderExpMs));
        Logger::Log("GuiderExpTimeSwitch (INDI) ms=" + std::to_string(guiderExpMs), LogLevel::INFO, DeviceType::GUIDER);
        // 同步到 GuiderCore 后，正在循环曝光时下一帧会自动使用新的曝光时间。
    }
    else if (message == "ClearDataPoints")
    {
        Logger::Log("ClearDataPoints ...", LogLevel::DEBUG, DeviceType::MAIN);
        // FWHM Data
        dataPoints.clear();
        Logger::Log("ClearDataPoints finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "SetGuiderGain")
    {
        guiderCameraGain = parts[1].trimmed().toDouble();
        Logger::Log("Set Guider Gain to " + std::to_string(guiderCameraGain), LogLevel::DEBUG, DeviceType::GUIDER);
        Tools::saveParameter("Guider", "Gain", parts[1].trimmed());

        bool isGuiderSDK = (systemdevicelist.system_devices.size() > 1 &&
                            systemdevicelist.system_devices[1].isSDKConnect &&
                            sdkGuiderHandle != nullptr);

        if (isGuiderSDK)
        {
            SdkCommand setGainCmd;
            setGainCmd.type = SdkCommandType::Custom;
            setGainCmd.name = "SetGain";
            setGainCmd.payload = guiderCameraGain;
            SdkResult res = SdkManager::instance().callByHandle(sdkGuiderHandle, setGainCmd);
            if (!res.success) {
                Logger::Log("SetGuiderGain | SDK SetGain failed: " + res.message, LogLevel::ERROR, DeviceType::GUIDER);
            } else {
                glGuiderGainValue = static_cast<int>(guiderCameraGain);
                Logger::Log("SetGuiderGain | SDK SetGain success", LogLevel::INFO, DeviceType::GUIDER);
            }
        }
        else if (dpGuider != NULL)
        {
            if (indi_Client->setCCDGain(dpGuider, static_cast<int>(guiderCameraGain)) == QHYCCD_SUCCESS)
            {
                glGuiderGainValue = static_cast<int>(guiderCameraGain);
            }
        }

        emit wsThread->sendMessageToClient("GuiderGainRange:" + QString::number(glGuiderGainMin) + ":" +
                                           QString::number(glGuiderGainMax) + ":" +
                                           QString::number(glGuiderGainValue));
    }

    else if (parts.size() == 5 && parts[0].trimmed() == "GuiderCanvasClick")
    {
        if (!guiderCore)
        {
            Logger::Log("GuiderCanvasClick ignored: guiderCore not initialized.", LogLevel::WARNING, DeviceType::GUIDER);
            return;
        }

        const int canvasW = std::max(1, parts[1].trimmed().toInt());
        const int canvasH = std::max(1, parts[2].trimmed().toInt());
        const double clickX = parts[3].trimmed().toDouble();
        const double clickY = parts[4].trimmed().toDouble();

        const int imageW = std::max(1, glPHD_CurrentImageSizeX);
        const int imageH = std::max(1, glPHD_CurrentImageSizeY);

        const double mappedX = std::clamp(clickX * static_cast<double>(imageW) / static_cast<double>(canvasW),
                                          0.0, static_cast<double>(imageW - 1));
        const double mappedY = std::clamp(clickY * static_cast<double>(imageH) / static_cast<double>(canvasH),
                                          0.0, static_cast<double>(imageH - 1));

        postGuiderCore(guiderCore, [mappedX, mappedY](GuiderCore *core) {
            core->setManualLock(mappedX, mappedY);
        });
        Logger::Log("GuiderCanvasClick -> built-in manual lock: canvas=" +
                        std::to_string(canvasW) + "x" + std::to_string(canvasH) +
                        " click=(" + std::to_string(clickX) + "," + std::to_string(clickY) + ")" +
                        " mapped=(" + std::to_string(mappedX) + "," + std::to_string(mappedY) + ")",
                    LogLevel::INFO, DeviceType::GUIDER);
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "GuiderFocalLength")
    {
        const QString v = parts[1].trimmed();
        bool ok = false;
        const double focal = v.toDouble(&ok);
        if (ok && focal > 0.0)
        {
            setClientSettings("GuiderFocalLength", QString::number(focal, 'g', 12));
            Logger::Log("GuiderFocalLength updated: " + std::to_string(guiderFocalLengthMm),
                        LogLevel::INFO, DeviceType::GUIDER);
            syncGuiderScaleParams(true, true);
        }
        else
        {
            Logger::Log("GuiderFocalLength ignored because value is invalid: " + v.toStdString(),
                        LogLevel::WARNING, DeviceType::GUIDER);
        }
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "GuiderPixelSize")
    {
        guiderPixelSizeUm = std::max(0.0, parts[1].trimmed().toDouble());
        Logger::Log("BuiltInGuider | GuiderPixelSize set to " + std::to_string(guiderPixelSizeUm) + " um",
                    LogLevel::INFO, DeviceType::GUIDER);
        syncGuiderScaleParams(true, true);
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "MultiStarGuider")
    {
        if (!guiderCore)
        {
            Logger::Log("MultiStarGuider ignored: guiderCore not initialized",
                        LogLevel::WARNING, DeviceType::GUIDER);
        }
        else
        {
            const QString rawValue = parts[1].trimmed().toLower();
            const bool enabled = (rawValue == "true" || rawValue == "1" || rawValue == "yes" || rawValue == "on");
            auto p = guiderParamsCache;
            p.enableMultiStar = enabled;
            guiderParamsCache = p;
            postGuiderCore(guiderCore, [p](GuiderCore *core) { core->setParams(p); });
            Logger::Log(std::string("BuiltInGuider | MultiStarGuider set to ") + (enabled ? "true" : "false"),
                        LogLevel::INFO, DeviceType::GUIDER);
            emit wsThread->sendMessageToClient(QStringLiteral("GuiderCoreInfo:%1")
                                                   .arg(enabled
                                                            ? QStringLiteral("多星导星已开启：自动选星时将参考副星修正偏移")
                                                            : QStringLiteral("多星导星已关闭：当前使用单星导星")));
        }
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "GuiderSearchBoxMode")
    {
        if (!guiderCore)
        {
            Logger::Log("GuiderSearchBoxMode ignored: guiderCore not initialized",
                        LogLevel::WARNING, DeviceType::GUIDER);
        }
        else
        {
            const QString rawMode = parts[1].trimmed().toUpper();
            QString appliedMode = QStringLiteral("AUTO");
            int searchHalfSizePx = 24;
            if (rawMode == "S")
            {
                appliedMode = QStringLiteral("S");
                searchHalfSizePx = 16;
            }
            else if (rawMode == "M")
            {
                appliedMode = QStringLiteral("M");
                searchHalfSizePx = 24;
            }
            else if (rawMode == "L")
            {
                appliedMode = QStringLiteral("L");
                searchHalfSizePx = 36;
            }

            auto p = guiderParamsCache;
            p.guideSearchHalfSizePx = searchHalfSizePx;
            guiderParamsCache = p;
            guiderSearchBoxMode = appliedMode;
            postGuiderCore(guiderCore, [p](GuiderCore *core) { core->setParams(p); });
            Logger::Log(("BuiltInGuider | GuiderSearchBoxMode set to " + appliedMode +
                         " (halfSizePx=" + QString::number(searchHalfSizePx) + ")").toStdString(),
                        LogLevel::INFO, DeviceType::GUIDER);
            publishGuiderSearchBoxMode(true);
        }
    }
    else if (parts.size() == 2 &&
             parts[0].trimmed() == "GuiderDecGuideDir")
    {
        if (!guiderCore)
        {
            Logger::Log("GuiderDecGuideDir ignored: guiderCore not initialized",
                        LogLevel::WARNING, DeviceType::GUIDER);
        }
        else
        {
            const QString rawMode = parts[1].trimmed().toUpper();
            auto p = guiderParamsCache;
            p.autoDecGuideDir = false;
            p.allowedDecDirs.clear();

            QString appliedMode;
            if (rawMode == "DEC+" || rawMode == "NORTH")
            {
                p.allowedDecDirs.insert(guiding::GuideDir::North);
                appliedMode = QStringLiteral("DEC+");
            }
            else if (rawMode == "DEC-" || rawMode == "SOUTH")
            {
                p.allowedDecDirs.insert(guiding::GuideDir::South);
                appliedMode = QStringLiteral("DEC-");
            }
            else
            {
                p.allowedDecDirs.insert(guiding::GuideDir::North);
                p.allowedDecDirs.insert(guiding::GuideDir::South);
                appliedMode = QStringLiteral("BOTH");
            }

            guiderParamsCache = p;
            postGuiderCore(guiderCore, [p](GuiderCore *core) { core->setParams(p); });
            Logger::Log(("BuiltInGuider | GuiderDecGuideDir set to " + appliedMode).toStdString(),
                        LogLevel::INFO, DeviceType::GUIDER);
            emit wsThread->sendMessageToClient("GuiderCoreInfo:DEC单向导星模式已设置为:" + appliedMode);
        }
    }
    else if (parts.size() == 2 &&
             (parts[0].trimmed() == "CalibrationDuration" ||
              parts[0].trimmed() == "RaAggression" ||
              parts[0].trimmed() == "DecAggression"))
    {
        Logger::Log("Guider guiding setting ignored (PHD2 removed): " + parts[0].trimmed().toStdString(),
                    LogLevel::WARNING, DeviceType::GUIDER);
    }
    };

    run();
    return true;
}

bool MainWindow::handleMountCommand(const QString &message, const QStringList &parts)
{
    const QString command = parts.isEmpty() ? message.trimmed() : parts[0].trimmed();
    if (!(
            command == QLatin1String("MountMoveWest") ||
            command == QLatin1String("MountMoveEast") ||
            command == QLatin1String("MountMoveNorth") ||
            command == QLatin1String("MountMoveSouth") ||
            command == QLatin1String("MountMoveAbort") ||
            command == QLatin1String("MountMoveRAStop") ||
            command == QLatin1String("MountMoveDECStop") ||
            command == QLatin1String("MountPark") ||
            command == QLatin1String("MountTrack") ||
            command == QLatin1String("MountHome") ||
            command == QLatin1String("MountSYNC") ||
            command == QLatin1String("MountSpeedSwitch") ||
            command == QLatin1String("GotoThenSolve") ||
            command == QLatin1String("Goto") ||
            command == QLatin1String("AutoFlip") ||
            command == QLatin1String("EastMinutesPastMeridian") ||
            command == QLatin1String("WestMinutesPastMeridian") ||
            command == QLatin1String("MountGoto") ||
            command == QLatin1String("SolveSYNC") ||
            command == QLatin1String("getMountParameters") ||
            command == QLatin1String("SynchronizeTime") ||
            command == QLatin1String("currectLocation") ||
            command == QLatin1String("reGetLocation") ||
            command == QLatin1String("StartAutoPolarAlignment") ||
            command == QLatin1String("StartPoleMasterAlignmentSimulation") ||
            command == QLatin1String("StopAutoPolarAlignment") ||
            command == QLatin1String("getPolarAlignmentState")))
    {
        return false;
    }

    auto run = [this, &message, &parts]() {
    if (message == "MountMoveWest")
    {
        Logger::Log("MountMoveWest ...", LogLevel::DEBUG, DeviceType::MOUNT);
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeMoveWE(dpMount, "WEST");
        }
    }
    else if (message == "MountMoveEast")
    {
        Logger::Log("MountMoveEast ...", LogLevel::DEBUG, DeviceType::MOUNT);
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeMoveWE(dpMount, "EAST");
        }
    }
    else if (message == "MountMoveNorth")
    {
        Logger::Log("MountMoveNorth ...", LogLevel::DEBUG, DeviceType::MOUNT);
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeMoveNS(dpMount, "NORTH");
        }
    }
    else if (message == "MountMoveSouth")
    {
        Logger::Log("MountMoveSouth ...", LogLevel::DEBUG, DeviceType::MOUNT);
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeMoveNS(dpMount, "SOUTH");
        }
    }
    else if (message == "MountMoveAbort")
    {
        Logger::Log("MountMoveAbort ...", LogLevel::DEBUG, DeviceType::MOUNT);
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeAbortMotion(dpMount);
        }
    }
    else if(parts[0].trimmed() == "MountMoveRAStop")
    {
        Logger::Log("MountMoveRAStop ...", LogLevel::DEBUG, DeviceType::MOUNT);
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeMoveWE(dpMount, "STOP");
        }
    }
    else if(parts[0].trimmed() == "MountMoveDECStop")
    {
        Logger::Log("MountMoveDECStop ...", LogLevel::DEBUG, DeviceType::MOUNT);
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeMoveNS(dpMount, "STOP");
        }
    }
    else if (message == "MountPark")
    {
        Logger::Log("MountPark ...", LogLevel::DEBUG, DeviceType::MOUNT);
        if (dpMount != NULL)
        {
            bool isPark = TelescopeControl_Park();
            if (isPark)
            {
                emit wsThread->sendMessageToClient("TelescopePark:ON");
            }
            else
            {
                emit wsThread->sendMessageToClient("TelescopePark:OFF");
            }
        }
    }
    else if (message == "MountTrack")
    {
        Logger::Log("MountTrack ...", LogLevel::DEBUG, DeviceType::MOUNT);
        if (dpMount != NULL)
        {
            bool isTrack = TelescopeControl_Track();
            if (isTrack)
            {
                emit wsThread->sendMessageToClient("TelescopeTrack:ON");
                Logger::Log("TelescopeTrack:ON", LogLevel::DEBUG, DeviceType::MOUNT);
            }
            else
            {
                emit wsThread->sendMessageToClient("TelescopeTrack:OFF");
                Logger::Log("TelescopeTrack:OFF", LogLevel::DEBUG, DeviceType::MOUNT);
            }
        }
    }
    else if (message == "MountHome")
    {
        Logger::Log("MountHome ...", LogLevel::DEBUG, DeviceType::MOUNT);
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeHomeInit(dpMount, "SLEWHOME");
        }
    }
    else if (message == "MountSYNC")
    {
        Logger::Log("MountSYNC ...", LogLevel::DEBUG, DeviceType::MOUNT);
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeHomeInit(dpMount, "SYNCHOME");
        }
    }

    else if (message == "MountSpeedSwitch")
    {
        Logger::Log("MountSpeedSwitch ", LogLevel::DEBUG, DeviceType::MOUNT);
        if (dpMount != NULL)
        {
            int currentSpeed;
            indi_Client->getTelescopeSlewRate(dpMount, currentSpeed);
            Logger::Log("Current Speed:" + std::to_string(currentSpeed) + " Total Speed:" + std::to_string(glTelescopeTotalSlewRate), LogLevel::DEBUG, DeviceType::MOUNT);

            if (currentSpeed == glTelescopeTotalSlewRate)
            {
                indi_Client->setTelescopeSlewRate(dpMount, 1);
            }
            else
            {
                indi_Client->setTelescopeSlewRate(dpMount, currentSpeed + 1);
                Logger::Log("Set Speed to:" + std::to_string(currentSpeed), LogLevel::DEBUG, DeviceType::MOUNT);
            }

            int ChangedSpeed;
            indi_Client->getTelescopeSlewRate(dpMount, ChangedSpeed);
            Logger::Log("Changed Speed:" + std::to_string(ChangedSpeed), LogLevel::DEBUG, DeviceType::MOUNT);
            emit wsThread->sendMessageToClient("MountSetSpeedSuccess:" + QString::number(ChangedSpeed));
        }
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "GotoThenSolve")
    {
        Logger::Log("GotoThenSolve ...", LogLevel::DEBUG, DeviceType::MOUNT);
        if (parts[1].trimmed() == "true")
        {
            GotoThenSolve = true;
        }
        else
        {
            GotoThenSolve = false;
        }
        Tools::saveParameter("Mount", "GotoThenSolve", parts[1].trimmed());
    }
    else if(parts.size() == 3 && parts[0].trimmed() == "Goto")
    {
        Logger::Log("Goto ...", LogLevel::DEBUG, DeviceType::MOUNT);
        double Ra = parts[1].trimmed().toDouble();
        double Dec = parts[2].trimmed().toDouble();
        MountOnlyGoto(Ra, Dec);
        Logger::Log("Goto finish!", LogLevel::DEBUG, DeviceType::MOUNT);
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "AutoFlip")
    {
        isAutoFlip = parts[1].trimmed() == "true" ? true : false;
        Tools::saveParameter("Mount", "AutoFlip", parts[1].trimmed());
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "EastMinutesPastMeridian")
    {
        EastMinutesPastMeridian = parts[1].trimmed().toDouble();
        Tools::saveParameter("Mount", "EastMinutesPastMeridian", parts[1].trimmed());
        if (dpMount != NULL)
        {
            indi_Client->setMinutesPastMeridian(dpMount, EastMinutesPastMeridian, WestMinutesPastMeridian);
            // emit wsThread->sendMessageToClient("MinutesPastMeridian:" + QString::number(EastMinutesPastMeridian) + ":" + QString::number(WestMinutesPastMeridian));
        }
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "WestMinutesPastMeridian")
    {
        WestMinutesPastMeridian = parts[1].trimmed().toDouble();
        Tools::saveParameter("Mount", "WestMinutesPastMeridian", parts[1].trimmed());
        if (dpMount != NULL)
        {
            indi_Client->setMinutesPastMeridian(dpMount, EastMinutesPastMeridian, WestMinutesPastMeridian);
            // emit wsThread->sendMessageToClient("MinutesPastMeridian:" + QString::number(EastMinutesPastMeridian) + ":" + QString::number(WestMinutesPastMeridian));
        }
    }
    else if (parts.size() == 4 && parts[0].trimmed() == "MountGoto")
    {
        if (dpMount != NULL)
        {
            Logger::Log("MountGoto ...", LogLevel::DEBUG, DeviceType::MOUNT);
            QStringList RaDecList = message.split(',');
            QStringList RaList = RaDecList[0].split(':');
            QStringList DecList = RaDecList[1].split(':');

            double Ra_Rad, Dec_Rad;
            Ra_Rad = RaList[2].trimmed().toDouble();
            Dec_Rad = DecList[1].trimmed().toDouble();

            Logger::Log("Mount Goto RaDec(Rad):" + std::to_string(Ra_Rad) + "," + std::to_string(Dec_Rad), LogLevel::DEBUG, DeviceType::MOUNT);

            double Ra_Hour, Dec_Degree;
            Ra_Hour = Tools::RadToHour(Ra_Rad);
            Dec_Degree = Tools::RadToDegree(Dec_Rad);

            MountGoto(Ra_Hour, Dec_Degree);
            Logger::Log("MountGoto finish!", LogLevel::DEBUG, DeviceType::MOUNT);
        }
        else
        {
        }
    }
    else if (parts[0].trimmed() == "SolveSYNC")
    {
        Logger::Log("SolveSYNC ...", LogLevel::DEBUG, DeviceType::MAIN);
        if (dpMount == NULL ){
            Logger::Log("Mount not connect", LogLevel::DEBUG, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("MountNotConnect");
            return;
        }
        if (!isMainCameraConnected()){
            Logger::Log("MainCamera not connect", LogLevel::DEBUG, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("MainCameraNotConnect");
            return;
        }
        if (isSolveSYNC)
        {
            Logger::Log("SolveSYNC is already running", LogLevel::DEBUG, DeviceType::MAIN);
            return;
        }
        if (glFocalLength == 0)
        {
            emit wsThread->sendMessageToClient("FocalLengthError");
            Logger::Log("FocalLengthError", LogLevel::DEBUG, DeviceType::MAIN);
        }
        else
        {
            TelescopeControl_SolveSYNC();
            Logger::Log("SolveSYNC finish!", LogLevel::DEBUG, DeviceType::MAIN);
        }
    }

    else if (parts[0].trimmed() == "getMountParameters")
    {
        Logger::Log("getMountParameters ...", LogLevel::DEBUG, DeviceType::MAIN);
        getMountParameters();
        Logger::Log("getMountParameters finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (parts[0].trimmed() == "SynchronizeTime")
    {
        QRegExp rx("SynchronizeTime:(\\d{2}:\\d{2}:\\d{2}):(\\d{4}-\\d{2}-\\d{2})");
        int pos = rx.indexIn(message);
        if (pos > -1)
        {
            QString time = rx.cap(1);
            QString date = rx.cap(2);
            Logger::Log("SynchronizeTime ...", LogLevel::DEBUG, DeviceType::MAIN);
            static QDateTime s_lastMountUTCSync;
            bool doMountUTC = true;
            if (s_lastMountUTCSync.isValid() && s_lastMountUTCSync.msecsTo(QDateTime::currentDateTime()) < 10000)
                doMountUTC = false;   // 10s 内不重复下发 Mount 时间，避免 INDI 双超时拖慢刷新
            synchronizeTime(time, date);
            if (doMountUTC)
                setMountUTC(time, date);
            else
                Logger::Log("SynchronizeTime | skip setMountUTC (debounce 10s)", LogLevel::DEBUG, DeviceType::MAIN);
            s_lastMountUTCSync = QDateTime::currentDateTime();
            Logger::Log("SynchronizeTime finish!", LogLevel::DEBUG, DeviceType::MAIN);
        }
    }
    else if (parts[0].trimmed() == "currectLocation" && parts.size() == 3)
    {
        Logger::Log("currectLocation ...", LogLevel::DEBUG, DeviceType::MAIN);
        QString CurrentLocationLat = parts[1].trimmed();
        QString CurrentLocationLon = parts[2].trimmed();
        setMountLocation(CurrentLocationLat, CurrentLocationLon);
        Logger::Log("currectLocation finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (parts[0].trimmed() == "reGetLocation")
    {
        Logger::Log("reGetLocation ...", LogLevel::DEBUG, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("sendGetLocation:" + localLat + ":" + localLon);
        Logger::Log("reGetLocation finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (parts[0].trimmed() == "StartAutoPolarAlignment")
    {
        const QString roleText = (parts.size() >= 2) ? parts[1].trimmed() : QStringLiteral("MainCamera");
        currentPolarAlignmentCameraRole = parsePolarAlignmentCameraRole(roleText);
        bool polarSolveNorthOk = false;
        const double polarSolveLat = localLat.trimmed().toDouble(&polarSolveNorthOk);
        const bool polarSolveNorth = !polarSolveNorthOk || polarSolveLat >= 0.0;
        Logger::Log("StartAutoPolarAlignment ... role=" +
                        std::string(polarRoleName(currentPolarAlignmentCameraRole)),
                    LogLevel::DEBUG, DeviceType::MAIN);
        if (polarAlignment != nullptr)
        {
            polarAlignment->stopPolarAlignment();
            delete polarAlignment;
            polarAlignment = nullptr;
            Logger::Log("ResetAutoPolarAlignment: Reset successfully", LogLevel::INFO, DeviceType::MAIN);
        }
        if (poleMasterPolarAlignment != nullptr)
        {
            poleMasterPolarAlignment->stop();
            delete poleMasterPolarAlignment;
            poleMasterPolarAlignment = nullptr;
            Logger::Log("ResetAutoPolarAlignment: PoleMaster reset successfully", LogLevel::INFO, DeviceType::MAIN);
        }
        if (poleMasterAlignmentSimulation != nullptr)
        {
            poleMasterAlignmentSimulation->stop();
            delete poleMasterAlignmentSimulation;
            poleMasterAlignmentSimulation = nullptr;
            Logger::Log("ResetAutoPolarAlignment: PoleMaster simulation reset successfully", LogLevel::INFO, DeviceType::MAIN);
        }
        if (polarAlignment == nullptr && poleMasterPolarAlignment == nullptr && poleMasterAlignmentSimulation == nullptr)
        {
            Logger::Log("ResetAutoPolarAlignment: alignment objects are nullptr", LogLevel::WARNING, DeviceType::MAIN);
        }

        // 从客户端配置同步自动极轴解析模式到模块环境变量。
        {
            std::unordered_map<std::string, std::string> config;
            Tools::readClientSettings("config/config.ini", config);

            auto normalizeGuidanceMode = [](const QString& raw) -> QString {
                const QString v = raw.trimmed().toLower();
                if (v == "trajectory_full" || v == "trajectory" || v == "full" ||
                    v == "full_solve" || v == "per_frame" || v == "1") {
                    return "trajectory_full";
                }
                return "guide_fast";
            };

            QString guideMode = "guide_fast";
            auto itMode = config.find("PolarAlignmentGuidanceSolveMode");
            if (itMode != config.end())
            {
                guideMode = normalizeGuidanceMode(QString::fromStdString(itMode->second));
            }
            qputenv("QUARCS_POLAR_GUIDANCE_SOLVE_MODE", guideMode.toUtf8());
            Logger::Log("StartAutoPolarAlignment: PolarAlignmentGuidanceSolveMode=" + guideMode.toStdString(),
                        LogLevel::INFO, DeviceType::MAIN);
        }

        const bool usePoleMaster = currentPolarAlignmentCameraRole == PolarAlignmentCameraRole::PoleCamera;
        if (usePoleMaster)
        {
            qputenv("QUARCS_POLAR_SOLVE_FIXED", "1");
            qputenv("QUARCS_POLAR_SOLVE_RA_DEG", "0.000000");
            qputenv("QUARCS_POLAR_SOLVE_DEC_DEG", polarSolveNorth ? "89.500000" : "-89.500000");
            qputenv("QUARCS_POLAR_SOLVE_RADIUS_DEG", "8.000000");
            qputenv("QUARCS_POLAR_SOLVE_SCALE_LOW_DEG", "10.000000");
            qputenv("QUARCS_POLAR_SOLVE_SCALE_HIGH_DEG", "12.000000");
            qputenv("QUARCS_POLAR_SOLVE_DEPTH", "1-80");
        }
        else
        {
            qunsetenv("QUARCS_POLAR_SOLVE_FIXED");
            qunsetenv("QUARCS_POLAR_SOLVE_RA_DEG");
            qunsetenv("QUARCS_POLAR_SOLVE_DEC_DEG");
            qunsetenv("QUARCS_POLAR_SOLVE_RADIUS_DEG");
            qunsetenv("QUARCS_POLAR_SOLVE_SCALE_LOW_DEG");
            qunsetenv("QUARCS_POLAR_SOLVE_SCALE_HIGH_DEG");
            qunsetenv("QUARCS_POLAR_SOLVE_DEPTH");
        }

        bool isSuccess = usePoleMaster ? initPoleMasterPolarAlignment()
                                       : initPolarAlignment(currentPolarAlignmentCameraRole);
        if (isSuccess)
        {
            // 启动自动极轴校准前，先关闭赤道仪跟踪
            bool trackingDisabled = false;
            if (indi_Client != nullptr && dpMount != nullptr)
            {
                indi_Client->setTelescopeTrackEnable(dpMount, false);
                emit wsThread->sendMessageToClient("TelescopeTrack:OFF");
                Logger::Log("StartAutoPolarAlignment: Telescope tracking disabled for polar alignment",
                            LogLevel::INFO, DeviceType::MAIN);
                trackingDisabled = true;
            }

            const bool started = usePoleMaster
                                     ? (poleMasterPolarAlignment != nullptr && poleMasterPolarAlignment->start())
                                     : (polarAlignment != nullptr && polarAlignment->startPolarAlignment());
            if (started)
            {
                Logger::Log("StartAutoPolarAlignment: Started successfully", LogLevel::INFO, DeviceType::MAIN);
            }
            else
            {
                Logger::Log("StartAutoPolarAlignment: Failed to start polar alignment", LogLevel::ERROR, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start polar alignment");
                qunsetenv("QUARCS_POLAR_SOLVE_FIXED");
                qunsetenv("QUARCS_POLAR_SOLVE_RA_DEG");
                qunsetenv("QUARCS_POLAR_SOLVE_DEC_DEG");
                qunsetenv("QUARCS_POLAR_SOLVE_RADIUS_DEG");
                qunsetenv("QUARCS_POLAR_SOLVE_SCALE_LOW_DEG");
                qunsetenv("QUARCS_POLAR_SOLVE_SCALE_HIGH_DEG");
                qunsetenv("QUARCS_POLAR_SOLVE_DEPTH");

                // 启动失败时恢复之前的跟踪状态
                if (trackingDisabled && indi_Client != nullptr && dpMount != nullptr)
                {
                    indi_Client->setTelescopeTrackEnable(dpMount, true);

                    bool isTrack = false;
                    indi_Client->getTelescopeTrackEnable(dpMount, isTrack);
                    emit wsThread->sendMessageToClient(isTrack ? "TelescopeTrack:ON"
                                                               : "TelescopeTrack:OFF");
                    Logger::Log("StartAutoPolarAlignment: Restore telescope tracking because start failed",
                                LogLevel::INFO, DeviceType::MAIN);
                }
            }
        }
        else
        {
            Logger::Log("StartAutoPolarAlignment: Failed to initialize polar alignment", LogLevel::ERROR, DeviceType::MAIN);
            qunsetenv("QUARCS_POLAR_SOLVE_FIXED");
            qunsetenv("QUARCS_POLAR_SOLVE_RA_DEG");
            qunsetenv("QUARCS_POLAR_SOLVE_DEC_DEG");
            qunsetenv("QUARCS_POLAR_SOLVE_RADIUS_DEG");
            qunsetenv("QUARCS_POLAR_SOLVE_SCALE_LOW_DEG");
            qunsetenv("QUARCS_POLAR_SOLVE_SCALE_HIGH_DEG");
            qunsetenv("QUARCS_POLAR_SOLVE_DEPTH");
        }
    }
    else if (parts[0].trimmed() == "StartPoleMasterAlignmentSimulation")
    {
        Logger::Log("StartPoleMasterAlignmentSimulation ...", LogLevel::DEBUG, DeviceType::MAIN);
        if (polarAlignment != nullptr)
        {
            polarAlignment->stopPolarAlignment();
            delete polarAlignment;
            polarAlignment = nullptr;
        }
        if (poleMasterPolarAlignment != nullptr)
        {
            poleMasterPolarAlignment->stop();
            delete poleMasterPolarAlignment;
            poleMasterPolarAlignment = nullptr;
        }
        if (poleMasterAlignmentSimulation != nullptr)
        {
            poleMasterAlignmentSimulation->stop();
            delete poleMasterAlignmentSimulation;
            poleMasterAlignmentSimulation = nullptr;
        }
        qunsetenv("QUARCS_POLAR_SOLVE_FIXED");
        qunsetenv("QUARCS_POLAR_SOLVE_RA_DEG");
        qunsetenv("QUARCS_POLAR_SOLVE_DEC_DEG");
        qunsetenv("QUARCS_POLAR_SOLVE_RADIUS_DEG");
        qunsetenv("QUARCS_POLAR_SOLVE_SCALE_LOW_DEG");
        qunsetenv("QUARCS_POLAR_SOLVE_SCALE_HIGH_DEG");
        qunsetenv("QUARCS_POLAR_SOLVE_DEPTH");

        if (initPoleMasterAlignmentSimulation() &&
            poleMasterPolarAlignment != nullptr &&
            poleMasterPolarAlignment->start())
        {
            Logger::Log("StartPoleMasterAlignmentSimulation: Started successfully", LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            Logger::Log("StartPoleMasterAlignmentSimulation: Failed to start", LogLevel::ERROR, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start pole camera simulation");
        }
    }
    else if (parts[0].trimmed() == "StopAutoPolarAlignment")
    {
        Logger::Log("StopAutoPolarAlignment ...", LogLevel::DEBUG, DeviceType::MAIN);
        qunsetenv("QUARCS_POLAR_SOLVE_FIXED");
        qunsetenv("QUARCS_POLAR_SOLVE_RA_DEG");
        qunsetenv("QUARCS_POLAR_SOLVE_DEC_DEG");
        qunsetenv("QUARCS_POLAR_SOLVE_RADIUS_DEG");
        qunsetenv("QUARCS_POLAR_SOLVE_SCALE_LOW_DEG");
        qunsetenv("QUARCS_POLAR_SOLVE_SCALE_HIGH_DEG");
        qunsetenv("QUARCS_POLAR_SOLVE_DEPTH");
        if (polarAlignment != nullptr)
        {
            polarAlignment->stopPolarAlignment();
            Logger::Log("StopAutoPolarAlignment: Stopped successfully", LogLevel::INFO, DeviceType::MAIN);
        }
        if (poleMasterPolarAlignment != nullptr)
        {
            poleMasterPolarAlignment->stop();
            Logger::Log("StopAutoPolarAlignment: PoleMaster stopped successfully", LogLevel::INFO, DeviceType::MAIN);
        }
        if (poleMasterAlignmentSimulation != nullptr)
        {
            poleMasterAlignmentSimulation->stop();
            Logger::Log("StopAutoPolarAlignment: PoleMaster simulation stopped successfully", LogLevel::INFO, DeviceType::MAIN);
        }
        if (polarAlignment == nullptr && poleMasterPolarAlignment == nullptr && poleMasterAlignmentSimulation == nullptr)
        {
            Logger::Log("StopAutoPolarAlignment: alignment objects are nullptr", LogLevel::WARNING, DeviceType::MAIN);
        }
        Logger::Log("StopAutoPolarAlignment finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    // [停用 2026-04-14] 旧自动电调校准命令入口已停用，改为前端手动移动+手动设置 MinLimit/MaxLimit。
    else if (parts[0].trimmed() == "getPolarAlignmentState")
    {
        Logger::Log("getPolarAlignmentState ...", LogLevel::DEBUG, DeviceType::MAIN);
        if (polarAlignment != nullptr)
        {
            // 无论是否正在运行，都返回一次当前状态，避免“结束后轮询无响应”造成前端状态卡住。
            // 1.获取当前状态
            PolarAlignmentState currentState = polarAlignment->getCurrentState();
            // 2.获取当前信息
            QString currentStatusMessage = polarAlignment->getCurrentStatusMessage();
            // 3.获取当前进度
            int progressPercentage = polarAlignment->getProgressPercentage();
            emit wsThread->sendMessageToClient(QString("PolarAlignmentState:") +
                                               (polarAlignment->isRunning() ? "true" : "false") + ":" +
                                               QString::number(static_cast<int>(currentState)) + ":" +
                                               currentStatusMessage + ":" +
                                               QString::number(progressPercentage));

            // 4.运行中再发送可控数据（避免结束后反复刷历史点位）
            if (polarAlignment->isRunning())
            {
                polarAlignment->sendValidAdjustmentGuideData();
            }
        }else{
            emit wsThread->sendMessageToClient(QString("PolarAlignmentState:false:未启动:未启动:0"));
        }
    }
    };

    run();
    return true;
}

bool MainWindow::handleFileAndStorageCommand(const QString &message, const QStringList &parts)
{
    const QString command = parts.isEmpty() ? message.trimmed() : parts[0].trimmed();
    if (!(
            command == QLatin1String("ShowAllImageFolder") ||
            command == QLatin1String("MoveFileToUSB") ||
            command == QLatin1String("DeleteFile") ||
            command == QLatin1String("USBCheck") ||
            command == QLatin1String("GetImageFiles") ||
            command == QLatin1String("GetDownloadManifest") ||
            command == QLatin1String("ClearDownloadLinks") ||
            command == QLatin1String("GetUSBFiles") ||
            command == QLatin1String("ReadImageFile") ||
            command == QLatin1String("stopLoopSolveImage") ||
            command == QLatin1String("EndCaptureAndSolve") ||
            command == QLatin1String("getStagingSolveResult") ||
            command == QLatin1String("ClearSloveResultList") ||
            command == QLatin1String("getOriginalImage") ||
            command == QLatin1String("sendVisibleArea") ||
            command == QLatin1String("queryTileBatchReady") ||
            command == QLatin1String("sendSelectStars")))
    {
        return false;
    }

    auto run = [this, &message, &parts]() {
    if (message == "ShowAllImageFolder")
    {
        Logger::Log("ShowAllImageFolder ...", LogLevel::DEBUG, DeviceType::MAIN);
        std::string allFile = GetAllFile();
        emit wsThread->sendMessageToClient("ShowAllImageFolder:" + QString::fromStdString(allFile));
        Logger::Log("ShowAllImageFolder finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (parts.size() >= 2 && parts[0].trimmed() == "MoveFileToUSB")
    {
        Logger::Log("MoveFileToUSB ...", LogLevel::DEBUG, DeviceType::MAIN);
        QStringList ImagePath = parseString(parts[1].trimmed().toStdString(), ImageSaveBasePath);
        QString usbName = "";
        // 如果提供了U盘名（parts.size() >= 3），则使用它
        if (parts.size() >= 3)
        {
            usbName = parts[2].trimmed();
        }
        Logger::Log("MoveFileToUSB | ImagePath: " + ImagePath.join(", ").toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        CopyImagesToUsb(ImagePath, usbName);
        Logger::Log("MoveFileToUSB finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (parts[0].trimmed() == "DeleteFile")
    {
        Logger::Log("DeleteFile ...", LogLevel::DEBUG, DeviceType::MAIN);
        QString ImagePathString = message; // 创建副本
        ImagePathString.replace("DeleteFile:", "");

        QStringList ImagePath = parseString(ImagePathString.toStdString(), ImageSaveBasePath);
        DeleteImage(ImagePath);
        Logger::Log("DeleteFile finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (message == "USBCheck")
    {
        Logger::Log("USBCheck ...", LogLevel::DEBUG, DeviceType::MAIN);
        USBCheck();
        Logger::Log("USBCheck finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "GetImageFiles")
    {
        Logger::Log("GetImageFiles ...", LogLevel::DEBUG, DeviceType::MAIN);
        std::string FolderPath = parts[1].trimmed().toStdString();
        GetImageFiles(FolderPath);
        Logger::Log("GetImageFiles finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (parts[0].trimmed() == "GetDownloadManifest")
    {
        // 格式：
        // - GetDownloadManifest:<FolderType>{<folderName>;...}  （按文件夹下载：后端展开该文件夹内所有文件）
        // - GetDownloadManifest:<FolderType>{<folderName>/<fileName>;...} （按具体文件下载）
        //
        // 返回：
        // - DownloadManifest:<json>
        Logger::Log("GetDownloadManifest ...", LogLevel::DEBUG, DeviceType::MAIN);

        QString req = message; // 创建副本
        req.replace("GetDownloadManifest:", "");
        const int bracePos = req.indexOf('{');
        const int endBracePos = req.lastIndexOf('}');
        if (bracePos <= 0 || endBracePos <= bracePos)
        {
            Logger::Log("GetDownloadManifest | invalid request format: " + req.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
            QJsonObject errorObj;
            errorObj["error"] = "Invalid request format";
            errorObj["totalBytes"] = 0;
            errorObj["files"] = QJsonArray();
            QJsonDocument errorDoc(errorObj);
            emit wsThread->sendMessageToClient("DownloadManifest:" + errorDoc.toJson(QJsonDocument::Compact));
            return;
        }

        const QString folderType = req.left(bracePos).trimmed();
        QString content = req.mid(bracePos + 1, endBracePos - bracePos - 1);
        // 去掉末尾分号（若有）
        if (content.endsWith(';')) content.chop(1);
        const QStringList rawParts = content.split(';', Qt::SkipEmptyParts);

        // 生成下载会话 token（仅用于前端与 ClearDownloadLinks 兼容，不再创建临时目录）
        const QString token = QDateTime::currentDateTimeUtc().toString("yyyyMMddHHmmsszzz");

        // 直接下载原文件：不复制、不建软链接，manifest 中 url 指向 /img/direct/<folderType>/<relPath>
        // 由 Web 服务（server.py）根据环境变量 IMAGE_SAVE_BASE_PATH 从原路径流式提供文件
        const QString srcRoot = QDir::cleanPath(QString::fromStdString(ImageSaveBasePath) + "/" + folderType) + "/";
        Logger::Log("GetDownloadManifest | direct download, srcRoot=" + srcRoot.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);

        auto isUnsafeRelPath = [](const QString &p) -> bool {
            const QString s = p.trimmed();
            if (s.isEmpty()) return true;
            if (s.startsWith('/') || s.startsWith('\\')) return true;
            if (s.contains("..")) return true;
            if (s.contains(QChar(u'\0'))) return true;
            return false;
        };

        QJsonArray filesArray;
        long long totalBytes = 0;

        auto addOneFile = [&](const QString &relFolder, const QString &fileName) {
            const QString relPath = relFolder.isEmpty() ? fileName : (relFolder + "/" + fileName);
            if (isUnsafeRelPath(relPath)) return;

            const QString srcPath = QDir::cleanPath(srcRoot + relPath);
            QFileInfo fi(srcPath);
            if (!fi.exists() || !fi.isFile())
                return;

            const qint64 size = fi.size();
            totalBytes += size;

            QJsonObject fileObj;
            fileObj["name"] = fileName;
            fileObj["relPath"] = relPath;
            fileObj["size"] = static_cast<qint64>(size);
            // 直接使用原路径的 URL，由 server.py 根据 IMAGE_SAVE_BASE_PATH 从原路径提供
            fileObj["url"] = QString("/img/direct/%1/%2").arg(folderType, relPath);
            filesArray.append(fileObj);
        };

        for (const QString &p : rawParts)
        {
            const QString part = p.trimmed();
            if (isUnsafeRelPath(part)) continue;

            // part 可能是 folderName 或 folderName/fileName
            const QString srcPath = QDir::cleanPath(srcRoot + part);
            QFileInfo fi(srcPath);
            if (fi.exists() && fi.isDir())
            {
                QDir dir(srcPath);
                const QFileInfoList list = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
                for (const QFileInfo &f : list)
                {
                    addOneFile(part, f.fileName());
                }
            }
            else if (fi.exists() && fi.isFile())
            {
                const int slash = part.lastIndexOf('/');
                if (slash >= 0)
                {
                    addOneFile(part.left(slash), part.mid(slash + 1));
                }
                else
                {
                    addOneFile("", part);
                }
            }
            else
            {
                // 不存在则跳过
                continue;
            }
        }

        QJsonObject result;
        result["token"] = token;
        result["type"] = folderType;
        result["totalBytes"] = static_cast<qint64>(totalBytes);
        result["files"] = filesArray;
        QJsonDocument doc(result);
        emit wsThread->sendMessageToClient("DownloadManifest:" + doc.toJson(QJsonDocument::Compact));

        Logger::Log("GetDownloadManifest finish! files=" + std::to_string(filesArray.size()) + ", bytes=" + std::to_string(totalBytes),
                    LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (parts[0].trimmed() == "ClearDownloadLinks")
    {
        // 格式：ClearDownloadLinks:<token>
        // 删除 /var/www/html/img/downloads/<token>/ 下所有内容（下载完成/取消后清理）
        Logger::Log("ClearDownloadLinks ...", LogLevel::DEBUG, DeviceType::MAIN);
        if (parts.size() < 2)
        {
            Logger::Log("ClearDownloadLinks | missing token", LogLevel::WARNING, DeviceType::MAIN);
            return;
        }
        const QString token = parts[1].trimmed();
        // 安全：token 只允许数字，避免路径穿越
        for (const QChar &ch : token)
        {
            if (!ch.isDigit())
            {
                Logger::Log("ClearDownloadLinks | invalid token: " + token.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                return;
            }
        }

        // 使用 vueImagePath 对应的静态 img 根目录
        QString imgRoot = QString::fromStdString(vueImagePath);
        if (imgRoot.endsWith('/')) imgRoot.chop(1);
        imgRoot = QDir::cleanPath(imgRoot);
        const QString root = QDir::cleanPath(QString("%1/downloads/%2").arg(imgRoot, token));
        QDir dir(root);
        if (!dir.exists())
        {
            Logger::Log("ClearDownloadLinks | dir not exist: " + root.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
            return;
        }
        const bool ok = dir.removeRecursively();
        Logger::Log(std::string("ClearDownloadLinks finish! ok=") + (ok ? "true" : "false") + ", path=" + root.toStdString(),
                    ok ? LogLevel::DEBUG : LogLevel::WARNING, DeviceType::MAIN);
    }
    else if (parts[0].trimmed() == "GetUSBFiles")
    {
        Logger::Log("GetUSBFiles ...", LogLevel::DEBUG, DeviceType::MAIN);
        // 格式：GetUSBFiles:usb_name:relativePath
        // 两个参数都是必需的：U盘名和相对路径
        QString usbName = QString();
        QString relativePath = QString();
        
        if (parts.size() >= 3)
        {
            usbName = parts[1].trimmed();
            relativePath = parts[2].trimmed();
        }
        else if (parts.size() >= 2)
        {
            // 如果只有两个部分，可能是旧格式兼容，但需要两个参数
            usbName = parts[1].trimmed();
            relativePath = ""; // 空字符串，但函数内部会检查
        }
        
        // 直接调用，GetUSBFiles函数内部会验证参数
        GetUSBFiles(usbName, relativePath);
        Logger::Log("GetUSBFiles finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (parts[0].trimmed() == "ReadImageFile")
    {
        Logger::Log("ReadImageFile ...", LogLevel::DEBUG, DeviceType::MAIN);
        // message: ReadImageFile:<FolderType>/<folderName>/<fileName>
        // 真实路径：<ImageSaveBasePath>/<FolderType>/<folderName>/<fileName>
        QString ImagePath = message; // 创建副本
        ImagePath.replace("ReadImageFile:", "");
        const QString root = QString::fromStdString(ImageSaveBasePath);
        ImagePath = QDir::cleanPath(root + "/" + ImagePath);
        // ImagePath.replace(" ", "\\ "); // 转义空格
        // ImagePath.replace("[", "\\["); // 转义左方括号
        // ImagePath.replace("]", "\\]"); // 转义右方括号
        // ImagePath.replace(",", "\\,"); // 转义逗号
        saveFitsAsPNG(ImagePath, false);
        Logger::Log("ReadImageFile finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }



    else if (message == "stopLoopSolveImage")
    {
        Logger::Log("stopLoopSolveImage ...", LogLevel::DEBUG, DeviceType::CAMERA);
        isLoopSolveImage = false;
        Logger::Log("stopLoopSolveImage finish!", LogLevel::DEBUG, DeviceType::CAMERA);
    }

    else if (message == "EndCaptureAndSolve")
    {
        Logger::Log("EndCaptureAndSolve ...", LogLevel::DEBUG, DeviceType::CAMERA);
        EndCaptureAndSolve = true;
        Logger::Log("EndCaptureAndSolve finish!", LogLevel::DEBUG, DeviceType::CAMERA);
    }

    else if (message == "getStagingSolveResult")
    {
        Logger::Log("getStagingSolveResult ...", LogLevel::DEBUG, DeviceType::CAMERA);
        RecoverySloveResul();
        Logger::Log("getStagingSolveResult finish!", LogLevel::DEBUG, DeviceType::CAMERA);
    }

    else if (message == "ClearSloveResultList")
    {
        Logger::Log("ClearSloveResultList ...", LogLevel::DEBUG, DeviceType::CAMERA);
        ClearSloveResultList();
        Logger::Log("ClearSloveResultList finish!", LogLevel::DEBUG, DeviceType::CAMERA);
    }

    else if (message == "getOriginalImage")
    {
        Logger::Log("getOriginalImage ...", LogLevel::DEBUG, DeviceType::CAMERA);
        saveFitsAsPNG(QString::fromStdString("/dev/shm/ccd_simulator_original.fits"), false);
        Logger::Log("getOriginalImage finish!", LogLevel::DEBUG, DeviceType::CAMERA);
    }

    else if (parts[0].trimmed() == "sendVisibleArea" &&
             (parts.size() == 4 || parts.size() == 5 || parts.size() == 6 || parts.size() == 7))
    {
        Logger::Log("sendVisibleArea ...", LogLevel::DEBUG, DeviceType::MAIN);
        const double vx = parts[1].trimmed().toDouble();
        const double vy = parts[2].trimmed().toDouble();
        const double sc = parts[3].trimmed().toDouble();
        roiAndFocuserInfo["VisibleX"] = vx;
        roiAndFocuserInfo["VisibleY"] = vy;
        roiAndFocuserInfo["scale"] = sc;
        // 可选 frameId（旧协议 v2）：用于跨帧不去抖（前端把 frameId 追加到命令末尾）
        // 这里当前仅用于兼容解析；瓦片生成始终以最新 tileFrame.epoch 为准。
        if (parts.size() >= 5) {
            (void)parts[4].trimmed().toULongLong();
        }
        int targetZ = -1;
        int maxZCap = -1;
        if (parts.size() == 6) {
            targetZ = parts[5].trimmed().toInt();
        } else if (parts.size() == 7) {
            targetZ = parts[5].trimmed().toInt();
            maxZCap = parts[6].trimmed().toInt();
        }

        // 同步到瓦片视口参数（供后端按需生成视口瓦片）
        tileViewportX = vx;
        tileViewportY = vy;
        tileViewportScale = sc;
        tileViewportTargetZ = targetZ;
        tileViewportMaxZCap = maxZCap;
        ++tileViewportRequestSeq;

        Logger::Log("sendVisibleArea parsed: frameId=" +
                        std::to_string(parts.size() >= 5
                                           ? static_cast<unsigned long long>(parts[4].trimmed().toULongLong())
                                           : 0ULL) +
                        ", targetZ=" + std::to_string(targetZ) +
                        ", maxZCap=" + std::to_string(maxZCap),
                    LogLevel::DEBUG, DeviceType::MAIN);

        // 视口变化：调度“按需补瓦片”（不会阻塞主线程；会做合并/节流）
        scheduleViewportTileGeneration();
    }
    else if (parts[0].trimmed() == "queryTileBatchReady" && (parts.size() == 3 || parts.size() == 4))
    {
        const QString sessionId = parts[1].trimmed();
        const quint64 frameId = parts[2].trimmed().toULongLong();
        QStringList requestedTileKeys;
        if (parts.size() == 4) {
            requestedTileKeys = parts[3].split(',', Qt::SkipEmptyParts);
            for (QString &key : requestedTileKeys) {
                key = key.trimmed();
            }
            QStringList filteredRequestedTileKeys;
            filteredRequestedTileKeys.reserve(requestedTileKeys.size());
            for (const QString& key : requestedTileKeys) {
                if (!key.isEmpty()) {
                    filteredRequestedTileKeys.push_back(key);
                }
            }
            requestedTileKeys = filteredRequestedTileKeys;
        }
        Logger::Log("queryTileBatchReady: session=" + sessionId.toStdString() +
                        ", frameId=" + std::to_string(static_cast<unsigned long long>(frameId)) +
                        ", requestedCount=" + std::to_string(requestedTileKeys.size()),
                    LogLevel::DEBUG, DeviceType::MAIN);
        sendCurrentTileBatchReadySnapshotToClient(sessionId, frameId, requestedTileKeys);
        sendCurrentTileGenerationCompleteSnapshotToClient(sessionId, frameId);
    }
    else if (parts[0].trimmed() == "sendSelectStars" && parts.size() == 3)
    {
        Logger::Log("sendSelectStars ...", LogLevel::DEBUG, DeviceType::MAIN);
        roiAndFocuserInfo["SelectStarX"] = parts[1].trimmed().toDouble();
        roiAndFocuserInfo["SelectStarY"] = parts[2].trimmed().toDouble();
        // NewSelectStar = true;
        Logger::Log("sendSelectStars finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    };

    run();
    return true;
}
