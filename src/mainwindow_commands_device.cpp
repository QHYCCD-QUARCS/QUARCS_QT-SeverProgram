#include "mainwindow_command_support.h"

bool MainWindow::handleDriverSelectionCommand(const QString &message, const QStringList &parts)
{
    const QString command = parts.isEmpty() ? message.trimmed() : parts[0].trimmed();
    if (!(
            command == QLatin1String("ConfirmIndiDriver") ||
            command == QLatin1String("ClearIndiDriver") ||
            command == QLatin1String("ConfirmIndiDevice") ||
            command == QLatin1String("SelectIndiDriver")))
    {
        return false;
    }

    auto run = [this, &message, &parts]() {
    if (parts.size() >= 2 && parts[0].trimmed() == "ConfirmIndiDriver")
    {
        if (parts.size() >= 4)
        {
            bool slotOk = false;
            const int targetSlot = parts[3].trimmed().toInt(&slotOk);
            if (!slotOk || targetSlot < 0 || targetSlot >= systemdevicelist.system_devices.size())
            {
                Logger::Log("ConfirmIndiDriver | Invalid target slot: " + parts[3].trimmed().toStdString(),
                            LogLevel::ERROR, DeviceType::MAIN);
                return;
            }
            systemdevicelist.currentDeviceCode = targetSlot;
            const QString driverName = parts[1].trimmed();
            const QString baudRate = parts[2].trimmed();
            Logger::Log("ConfirmIndiDriver:" + driverName.toStdString() + ":" +
                            baudRate.toStdString() + ":slot=" + std::to_string(targetSlot),
                        LogLevel::DEBUG, DeviceType::MAIN);
            indi_Driver_Confirm(driverName, baudRate);
        }
        else if (parts.size() == 2)
        {
            Logger::Log("ConfirmIndiDriver:" + parts[1].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
            QString driverName = parts[1].trimmed();
            indi_Driver_Confirm(driverName, "9600");
        }
        else if (parts.size() == 3)
        {
            Logger::Log("ConfirmIndiDriver:" + parts[1].trimmed().toStdString() + ":" + parts[2].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
            QString driverName = parts[1].trimmed();
            QString baudRate = parts[2].trimmed();
            indi_Driver_Confirm(driverName, baudRate);
        }
    }
    else if (message == "ClearIndiDriver")
    {
        Logger::Log("ClearIndiDriver", LogLevel::DEBUG, DeviceType::MAIN);
        if (!indi_Driver_Clear(systemdevicelist.currentDeviceCode))
        {
            Logger::Log("ClearIndiDriver | Legacy command failed because currentDeviceCode is invalid",
                        LogLevel::ERROR, DeviceType::MAIN);
        }
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "ClearIndiDriver")
    {
        const QString deviceCodeText = parts[1].trimmed();
        bool ok = false;
        const int deviceCode = deviceCodeText.toInt(&ok);
        Logger::Log("ClearIndiDriver:" + deviceCodeText.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        if (!ok)
        {
            Logger::Log("ClearIndiDriver | Invalid deviceCode: " + deviceCodeText.toStdString(),
                        LogLevel::ERROR, DeviceType::MAIN);
            return;
        }
        indi_Driver_Clear(deviceCode);
    }
    else if (parts.size() == 3 && parts[0].trimmed() == "ConfirmIndiDevice")
    {
        Logger::Log("ConfirmIndiDevice:" + parts[1].trimmed().toStdString() + ":" + parts[2].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        QString deviceName = parts[1].trimmed();
        QString driverName = parts[2].trimmed();
        // connectDevice(x);
        indi_Device_Confirm(deviceName, driverName);
    }
    else if (parts.size() == 3 && parts[0].trimmed() == "SelectIndiDriver")
    {
        Logger::Log("SelectIndiDriver:" + parts[1].trimmed().toStdString() + ":" + parts[2].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        QString Group = parts[1].trimmed();
        int ListNum = parts[2].trimmed().toInt();
        printDevGroups2(drivers_list, ListNum, Group);
    }
    };

    run();
    return true;
}

bool MainWindow::handleBindingCommand(const QString &message, const QStringList &parts)
{
    const QString command = parts.isEmpty() ? message.trimmed() : parts[0].trimmed();
    if (!(
            command == QLatin1String("BindingDevice") ||
            command == QLatin1String("UnBindingDevice")))
    {
        return false;
    }

    auto run = [this, &message, &parts]() {
    if (parts.size() == 3 && parts[0].trimmed() == "BindingDevice")
    {
        Logger::Log("BindingDevice:" + parts[1].trimmed().toStdString() + ":" + parts[2].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        QString devicetype = parts[1].trimmed();
        int deviceindex = parts[2].trimmed().toInt();
        // connectDevice(x);
        BindingDevice(devicetype, deviceindex);
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "UnBindingDevice")
    {
        Logger::Log("UnBindingDevice:" + parts[1].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        QString devicetype = parts[1].trimmed();
        // connectDevice(x);
        UnBindingDevice(devicetype);
    }
    };

    run();
    return true;
}

bool MainWindow::handleSystemCommand(const QString &message, const QStringList &parts)
{
    const QString command = parts.isEmpty() ? message.trimmed() : parts[0].trimmed();
    if (!(
            command == QLatin1String("connectAllDevice") ||
            command == QLatin1String("disconnectAllDevice") ||
            command == QLatin1String("SetSerialPort") ||
            command == QLatin1String("getClientSettings") ||
            command == QLatin1String("getConnectedDevices") ||
            command == QLatin1String("getGPIOsStatus") ||
            command == QLatin1String("SwitchOutPutPower") ||
            command == QLatin1String("getQTClientVersion") ||
            command == QLatin1String("getTotalVersion") ||
            command == QLatin1String("getHotspotName") ||
            command == QLatin1String("editHotspotName") ||
            command == QLatin1String("restartHotspot10s") ||
            command == QLatin1String("netStatus") ||
            command == QLatin1String("netMode") ||
            command == QLatin1String("wifiScan") ||
            command == QLatin1String("DSLRCameraInfo") ||
            command == QLatin1String("saveToConfigFile") ||
            command == QLatin1String("RestartRaspberryPi") ||
            command == QLatin1String("ShutdownRaspberryPi") ||
            command == QLatin1String("ConnectDriver") ||
            command == QLatin1String("DisconnectDevice") ||
            command == QLatin1String("loadSelectedDriverList") ||
            command == QLatin1String("loadBindDeviceList") ||
            command == QLatin1String("loadBindDeviceTypeList") ||
            command == QLatin1String("SetConnectionMode") ||
            command == QLatin1String("disconnectSelectDriver") ||
            command == QLatin1String("testQtServerProcess") ||
            command == QLatin1String("localMessage") ||
            command == QLatin1String("showRoiImageSuccess") ||
            command == QLatin1String("getLastSelectDevice") ||
            command == QLatin1String("CheckBoxSpace") ||
            command == QLatin1String("ClearLogs") ||
            command == QLatin1String("ClearBoxCache") ||
            command == QLatin1String("loadSDKVersionAndUSBSerialPath") ||
            message.startsWith(QLatin1String("wifiSaveB64|"))))
    {
        return false;
    }

    auto run = [this, &message, &parts]() {
    if (message == "connectAllDevice")
    {
        Logger::Log("connectAllDevice", LogLevel::DEBUG, DeviceType::MAIN);
        // DeviceConnect();
        ConnectAllDeviceOnce();
    }
    else if (message == "disconnectAllDevice")
    {
        Logger::Log("disconnectAllDevice ...", LogLevel::DEBUG, DeviceType::MAIN);

        // 先显式停止导星循环与当前曝光，避免残留 isGuiderLoopExp 状态影响后续 reconnect all 自动起循环。
        stopGuiderLoopAndExposure(QStringLiteral("disconnectAllDevice"));

        // 先清理 SDK：SDK 设备不止主相机，多相机场景会在池中打开多个句柄
        // 统一关闭所有 SDK 句柄并释放资源，避免"断开后 SDK 仍占用/仍在调用"
        cleanupQhySdkPoolAndResource("disconnectAllDevice", "All");

        disconnectIndiServer(indi_Client);
        Logger::Log("disconnectIndiServer ...", LogLevel::DEBUG, DeviceType::MAIN);
        // ClearSystemDeviceList();
        // Logger::Log("ClearSystemDeviceList ...", LogLevel::DEBUG, DeviceType::MAIN);
        clearConnectedDevices();
        Logger::Log("clearConnectedDevices ...", LogLevel::DEBUG, DeviceType::MAIN);
        // 重启indi服务器
        initINDIServer();
        Logger::Log("initINDIServer ...", LogLevel::DEBUG, DeviceType::MAIN);
        initINDIClient();

        Logger::Log("initINDIClient ...", LogLevel::DEBUG, DeviceType::MAIN);
        // Tools::InitSystemDeviceList();
        // Logger::Log("Tools::InitSystemDeviceList ...", LogLevel::DEBUG, DeviceType::MAIN);
        // Tools::initSystemDeviceList(systemdevicelist);
        getLastSelectDevice();
        Logger::Log("disconnectAllDevice end!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (parts.size() == 3 && parts[0].trimmed() == "SetSerialPort")
    {
        // 手动设置串口路径，仅针对 Mount / Focuser
        QString devType = parts[1].trimmed();
        QString portPath = parts[2].trimmed();

        // 特殊值 "default" 或空字符串：表示回到自动匹配模式，不强制指定串口
        bool isDefault =
            portPath.trimmed().isEmpty() ||
            portPath.trimmed().compare("default", Qt::CaseInsensitive) == 0;

        if (devType == "Mount")
        {
            mountSerialPortOverride = isDefault ? QString() : portPath;
        }
        else if (devType == "Focuser")
        {
            focuserSerialPortOverride = isDefault ? QString() : portPath;
        }

        if (isDefault)
        {
            Logger::Log("SetSerialPort | " + devType.toStdString() + " -> <default(auto-detect)>",
                        LogLevel::INFO, DeviceType::MAIN);
            // 不立即修改设备端口，后续连接时走自动识别/重新匹配逻辑
            return;
        }

        Logger::Log("SetSerialPort | " + devType.toStdString() + " -> " + portPath.toStdString(),
                    LogLevel::INFO, DeviceType::MAIN);

        // 若设备已存在，则立即更新其串口端口（仅修改内存状态，不做持久化）
        if (devType == "Mount" && dpMount != nullptr)
        {
            indi_Client->setDevicePort(dpMount, portPath);
        }
        else if (devType == "Focuser" && dpFocuser != nullptr)
        {
            indi_Client->setDevicePort(dpFocuser, portPath);
        }
    }
    else if (message == "getClientSettings")
    {
        Logger::Log("getClientSettings ...", LogLevel::DEBUG, DeviceType::MAIN);
        getClientSettings();
        Logger::Log("getClientSettings finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (message == "getConnectedDevices")
    {
        // getConnectedDevices();
    }



    else if (message == "getGPIOsStatus")
    {
        Logger::Log("getGPIOsStatus ...", LogLevel::DEBUG, DeviceType::MAIN);
        getGPIOsStatus();
        Logger::Log("getGPIOsStatus finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "SwitchOutPutPower")
    {
        Logger::Log("SwitchOutPutPower ...", LogLevel::DEBUG, DeviceType::MAIN);
        int index = parts[1].trimmed().toInt();
        int value;

        if (index == 1)
        {
            value = readGPIOValue(GPIO_PIN_1);
            if (value == 1)
            {
                setGPIOValue(GPIO_PIN_1, "0");
                value = readGPIOValue(GPIO_PIN_1);
            }
            else
            {
                setGPIOValue(GPIO_PIN_1, "1");
                value = readGPIOValue(GPIO_PIN_1);
            }
        }
        else if (index == 2)
        {
            value = readGPIOValue(GPIO_PIN_2);
            if (value == 1)
            {
                setGPIOValue(GPIO_PIN_2, "0");
                value = readGPIOValue(GPIO_PIN_2);
            }
            else
            {
                setGPIOValue(GPIO_PIN_2, "1");
                value = readGPIOValue(GPIO_PIN_2);
            }
        }

        emit wsThread->sendMessageToClient("OutputPowerStatus:" + QString::number(index) + ":" + QString::number(value));
        Logger::Log("SwitchOutPutPower finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (message == "getQTClientVersion")
    {
        Logger::Log("getQTClientVersion ...", LogLevel::DEBUG, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("QTClientVersion:" + QString::fromLatin1(quarcsQtClientVersion()));
    }

    // 获取总版本号（来自环境变量 QUARCS_TOTAL_VERSION，格式 x.x.x）
    else if (message == "getTotalVersion")
    {
        Logger::Log("getTotalVersion ...", LogLevel::DEBUG, DeviceType::MAIN);

        // 从环境变量中读取总版本号，未设置时回退到 0.0.0
        QByteArray envVersion = qgetenv("QUARCS_TOTAL_VERSION");
        QString totalVersion = envVersion.isEmpty() ? "0.0.0" : QString::fromUtf8(envVersion);

        emit wsThread->sendMessageToClient("TotalVersion:" + totalVersion);
    }

    else if (message == "getHotspotName")
    {
        QString HostpotName = getHotspotName();
        Logger::Log("HotspotName:" + HostpotName.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("HotspotName:" + HostpotName);
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "editHotspotName")
    {
        Logger::Log("editHotspotName ...", LogLevel::DEBUG, DeviceType::MAIN);
        QString HostpotName = parts[1].trimmed();
        editHotspotName(HostpotName);
        Logger::Log("editHotspotName finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (message == "restartHotspot10s")
    {
        Logger::Log("restartHotspot10s ...", LogLevel::DEBUG, DeviceType::MAIN);
        restartHotspotWithDelay(10);
        Logger::Log("restartHotspot10s finish (command issued)", LogLevel::DEBUG, DeviceType::MAIN);
    }

    // ===== Network mode (AP/WAN) + ZeroTier =====
    else if (message == "netStatus")
    {
        Logger::Log("netStatus ...", LogLevel::DEBUG, DeviceType::MAIN);
        requestNetStatus();
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "netMode")
    {
        const QString mode = parts[1].trimmed(); // ap / wan
        Logger::Log("netMode:" + mode.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        switchNetMode(mode);
    }

    // ===== Wi‑Fi scan / save (uplink profiles) =====
    else if (message == "wifiScan")
    {
        Logger::Log("wifiScan ...", LogLevel::DEBUG, DeviceType::MAIN);
        wifiScan();
    }
    else if (message.startsWith("wifiSaveB64|"))
    {
        const QString payload = message.mid(QString("wifiSaveB64|").length());
        Logger::Log("wifiSaveB64 ...", LogLevel::DEBUG, DeviceType::MAIN);
        wifiSaveFromB64Payload(payload);
    }

    else if (parts.size() == 4 && parts[0].trimmed() == "DSLRCameraInfo")
    {
        Logger::Log("DSLRCameraInfo ...", LogLevel::DEBUG, DeviceType::MAIN);
        int Width = parts[1].trimmed().toInt();
        int Height = parts[2].trimmed().toInt();
        double PixelSize = parts[3].trimmed().toDouble();

        if (dpMainCamera != NULL)
        {
            DSLRsInfo DSLRsInfo;
            DSLRsInfo.Name = dpMainCamera->getDeviceName();
            DSLRsInfo.SizeX = Width;
            DSLRsInfo.SizeY = Height;
            DSLRsInfo.PixelSize = PixelSize;
            Tools::saveDSLRsInfo(DSLRsInfo);
            NotSetDSLRsInfo = false;
            // indi_Client->setCCDBasicInfo(dpMainCamera, Width, Height, PixelSize, PixelSize, PixelSize, 8);
            AfterDeviceConnect(dpMainCamera);

            
            Logger::Log("DSLRCameraInfo finish!", LogLevel::DEBUG, DeviceType::MAIN);
        }
        else
        {
            Logger::Log("DSLRCameraInfo failed! Main Camera is NULL", LogLevel::DEBUG, DeviceType::MAIN);
        }
    }
    else if (parts.size() == 3 && parts[0].trimmed() == "saveToConfigFile")
    {
        Logger::Log("saveToConfigFile ...", LogLevel::DEBUG, DeviceType::MAIN);
        QString ConfigName = parts[1].trimmed();
        QString ConfigValue = parts[2].trimmed();

        setClientSettings(ConfigName, ConfigValue);
        if (ConfigName == "Coordinates")
        {
            QStringList parts = ConfigValue.split(",");
            if (parts.size() >= 2)
            {
                setMountLocation(parts[0], parts[1]);
            }
        }
        if (ConfigName == "FocalLength")
        {
            glFocalLength = ConfigValue.toInt();
        }
        else if (ConfigName == "MainCameraFocalLength")
        {
            glFocalLength = ConfigValue.toInt();
        }
        else if (ConfigName == "GuiderFocalLength")
        {
            guiderFocalLengthMm = ConfigValue.toDouble();
        }
        if (wsThread != nullptr)
        {
            if (ConfigName == "FocalLength" || ConfigName == "MainCameraFocalLength")
            {
                emit wsThread->sendMessageToClient("ConfigureRecovery:MainCameraFocalLength:" + QString::number(glFocalLength));
            }
            else if (ConfigName == "GuiderFocalLength")
            {
                emit wsThread->sendMessageToClient("ConfigureRecovery:GuiderFocalLength:" + QString::number(guiderFocalLengthMm, 'g', 12));
            }
        }

        Logger::Log("saveToConfigFile finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    // PHD2 已移除：不再支持 CalibrationDuration / RaAggression / DecAggression

    else if (message == "RestartRaspberryPi")
    {
        Logger::Log("RestartRaspberryPi ...", LogLevel::DEBUG, DeviceType::MAIN);
        system("reboot");
        Logger::Log("RestartRaspberryPi finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (message == "ShutdownRaspberryPi")
    {
        Logger::Log("ShutdownRaspberryPi ...", LogLevel::DEBUG, DeviceType::MAIN);
        system("shutdown -h now");
        Logger::Log("ShutdownRaspberryPi finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (parts.size() >= 3 && parts[0].trimmed() == "ConnectDriver")
    {
        QString DriverName = parts[1].trimmed();
        // 支持格式: ConnectDriver:DriverName:DriverType 或 ConnectDriver:DriverName:DriverType:ConnectionMode
        // 将第3部分及之后的所有部分用冒号连接作为 DriverType，以支持 "MainCamera:SDK" 这样的格式
        QStringList typeAndMode;
        for (int i = 2; i < parts.size(); ++i) {
            typeAndMode.append(parts[i].trimmed());
        }
        QString DriverType = typeAndMode.join(":");
        Logger::Log("Connect Driver to " + DriverName.toStdString() + " with type " + DriverType.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        ConnectDriver(DriverName, DriverType);
    }
    else if (parts.size() == 3 && parts[0].trimmed() == "DisconnectDevice")
    {
        QString DeviceName = parts[1].trimmed();
        QString DeviceType = parts[2].trimmed();
        Logger::Log("Disconnect " + DeviceType.toStdString() + " Device(" + DeviceName.toStdString() + ")", LogLevel::DEBUG, DeviceType::MAIN);
        DisconnectDevice(indi_Client, DeviceName, DeviceType);
    }
    else if (message == "loadSelectedDriverList")
    {
        Logger::Log("loadSelectedDriverList ...", LogLevel::DEBUG, DeviceType::MAIN);
        loadSelectedDriverList();
        Logger::Log("loadSelectedDriverList finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (message == "loadBindDeviceList")
    {
        Logger::Log("loadBindDeviceList ...", LogLevel::DEBUG, DeviceType::MAIN);
        loadBindDeviceList(indi_Client);
        Logger::Log("loadBindDeviceList finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (message == "loadBindDeviceTypeList")
    {
        Logger::Log("loadBindDeviceTypeList ...", LogLevel::DEBUG, DeviceType::MAIN);
        loadBindDeviceTypeList();
        Logger::Log("loadBindDeviceTypeList finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (parts.size() == 3 && parts[0].trimmed() == "SetConnectionMode")
    {
        // 消息格式：SetConnectionMode:DeviceDescription:Mode
        // 例如：SetConnectionMode:MainCamera:SDK 或 SetConnectionMode:MainCamera:INDI
        QString deviceDescription = parts[1].trimmed();
        QString mode = parts[2].trimmed();
        
        Logger::Log("SetConnectionMode | Device: " + deviceDescription.toStdString() + 
                   ", Mode: " + mode.toStdString(), LogLevel::INFO, DeviceType::MAIN);

        const bool newIsSdk = (mode.toUpper() == "SDK");

        // 工具：按 Description 查找索引
        auto findDeviceIndexByDesc = [&](const QString& desc) -> int {
            for (int i = 0; i < systemdevicelist.system_devices.size(); ++i) {
                if (systemdevicelist.system_devices[i].Description == desc) return i;
            }
            return -1;
        };

        const int idx = findDeviceIndexByDesc(deviceDescription);
        if (idx < 0) {
            Logger::Log("SetConnectionMode | Device " + deviceDescription.toStdString() +
                        " not found in system device list", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("SetConnectionModeFailed:" + deviceDescription +
                                               ":Device not found");
            return;
        }

        // ===== 连接模式锁定：已连接时不允许切换 =====
        const bool oldIsSdk = systemdevicelist.system_devices[idx].isSDKConnect;
        const bool selfConnected = systemdevicelist.system_devices[idx].isConnect;
        if (selfConnected && oldIsSdk != newIsSdk)
        {
            Logger::Log("SetConnectionMode | Device " + deviceDescription.toStdString() +
                            " is already connected. Changing connection mode is forbidden. Please disconnect first.",
                        LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("SetConnectionModeFailed:" + deviceDescription +
                                               ":DeviceConnectedLockModeChangeForbidden");
            return;
        }

        // QHYCCD 相机三角色共享同一 SDK/INDI 驱动资源，同驱动时必须同模式。
        const QStringList cameraMutexRoles = {"MainCamera", "Guider", "PoleCamera"};
        const bool isCameraMutexRole = cameraMutexRoles.contains(deviceDescription);
        const int idxMain = isCameraMutexRole ? findDeviceIndexByDesc("MainCamera") : -1;
        const int idxGuider = isCameraMutexRole ? findDeviceIndexByDesc("Guider") : -1;
        const int idxPole = isCameraMutexRole ? findDeviceIndexByDesc("PoleCamera") : -1;

        QVector<int> linkedCameraIndexes;
        QStringList linkedCameraDescriptions;
        if (isCameraMutexRole)
        {
            const QString targetDriver = systemdevicelist.system_devices[idx].DriverIndiName.trimmed();
            if (!targetDriver.isEmpty())
            {
                for (const auto &role : cameraMutexRoles)
                {
                    const int roleIdx = findDeviceIndexByDesc(role);
                    if (roleIdx < 0 || roleIdx >= systemdevicelist.system_devices.size())
                        continue;
                    const QString roleDriver = systemdevicelist.system_devices[roleIdx].DriverIndiName.trimmed();
                    if (!roleDriver.isEmpty() && roleDriver.compare(targetDriver, Qt::CaseInsensitive) == 0)
                    {
                        linkedCameraIndexes.push_back(roleIdx);
                        linkedCameraDescriptions.push_back(role);
                    }
                }
            }
        }
        const bool sameDriverCameraGroup = isCameraMutexRole && linkedCameraIndexes.size() > 1;

        if (sameDriverCameraGroup)
        {
            bool anySdkConnected = false;
            bool anyIndiConnected = false;
            for (int roleIdx : linkedCameraIndexes)
            {
                const bool connected = systemdevicelist.system_devices[roleIdx].isConnect;
                anySdkConnected = anySdkConnected || (connected && systemdevicelist.system_devices[roleIdx].isSDKConnect);
                anyIndiConnected = anyIndiConnected || (connected && !systemdevicelist.system_devices[roleIdx].isSDKConnect);
            }

            // 反向锁定：若任一已通过 INDI 连接，则禁止切到 SDK（避免同驱动混用两种连接方式）
            if (newIsSdk && anyIndiConnected)
            {
                Logger::Log("SetConnectionMode | INDI connected in QHY camera group. Switching to SDK is forbidden.",
                            LogLevel::WARNING, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("SetConnectionModeFailed:" + deviceDescription +
                                                   ":INDIConnectedLockSdkForbidden");
                return;
            }

            // 原有锁定：若任一已通过 SDK 连接，则禁止切到 INDI
            if (!newIsSdk && anySdkConnected)
            {
                Logger::Log("SetConnectionMode | SDK connected in QHY camera group. Switching to INDI is forbidden.",
                            LogLevel::WARNING, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("SetConnectionModeFailed:" + deviceDescription +
                                                   ":SDKConnectedLockIndiForbidden");
                return;
            }

            // 未连接的第二个角色允许切换到已连接相机的相同模式。
            // 上面的 anyIndiConnected/anySdkConnected 检查已经阻止了真正的模式冲突；
            // 若在这里仅因组内存在连接就拒绝，会导致 Guider=SDK 后 MainCamera
            // 无法从默认 INDI 对齐到 SDK。
        }

        // 支持性校验：若要切到 SDK，需要该设备支持；同驱动联动时也要求另一台支持
        {
            const QString driverIndiName = systemdevicelist.system_devices[idx].DriverIndiName;
            const bool supportSDK = isDeviceTypeSupportSDK(deviceDescription, driverIndiName);
            if (!supportSDK && newIsSdk) {
                Logger::Log("SetConnectionMode | Device " + deviceDescription.toStdString() +
                            " does not support SDK mode", LogLevel::WARNING, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("SetConnectionModeFailed:" + deviceDescription +
                                                   ":Device does not support SDK mode");
                return;
            }

            if (sameDriverCameraGroup && newIsSdk) {
                // 对同驱动相机组也做一次支持性校验（防止配置异常）
                for (int i = 0; i < linkedCameraIndexes.size(); ++i)
                {
                    const int peerIdx = linkedCameraIndexes[i];
                    const QString peerDesc = linkedCameraDescriptions.value(i);
                    const QString peerDriverIndiName = systemdevicelist.system_devices[peerIdx].DriverIndiName;
                    const bool peerSupportSDK = isDeviceTypeSupportSDK(peerDesc, peerDriverIndiName);
                    if (!peerSupportSDK) {
                        Logger::Log("SetConnectionMode | Linked camera device " + peerDesc.toStdString() +
                                    " does not support SDK mode", LogLevel::WARNING, DeviceType::MAIN);
                        emit wsThread->sendMessageToClient("SetConnectionModeFailed:" + deviceDescription +
                                                           ":Linked camera device does not support SDK mode");
                        return;
                    }
                }
            }
        }

        // 应用模式（必要时同步另一台）
        systemdevicelist.system_devices[idx].isSDKConnect = newIsSdk;

        // 同驱动联动：同步相机三角色的 isSDKConnect，防止 Main/Guider/PoleCamera 混用 SDK/INDI
        if (sameDriverCameraGroup) {
            for (int roleIdx : linkedCameraIndexes)
                systemdevicelist.system_devices[roleIdx].isSDKConnect = newIsSdk;
        }

        // ===== 关键修复：从 SDK 切到 INDI 时，释放 SDK 句柄以避免串口占用（仅 Focuser 需要） =====
        if (oldIsSdk && !newIsSdk)
        {
            if (deviceDescription == "Focuser")
            {
                // 切换连接模式本质上意味着“当前连接作废”，避免前端/后续流程误判为仍连接/仍绑定
                systemdevicelist.system_devices[idx].isConnect = false;
                systemdevicelist.system_devices[idx].isBind = false;

                if (sdkFocuserHandle != nullptr)
                {
                    Logger::Log("SetConnectionMode | Switching Focuser SDK->INDI, closing SDK focuser handle to release serial port.",
                                LogLevel::INFO, DeviceType::FOCUSER);
                    SdkManager::instance().closeByHandle(sdkFocuserHandle);
                    sdkFocuserHandle = nullptr;
                    sdkFocuserPort.clear();
                }
            }
            else if (cameraMutexRoles.contains(deviceDescription))
            {
                // 相机从 SDK 切到 INDI 时，必须清空 SDK 相机池与前端待分配列表残留。
                // 否则后续 loadBindDeviceList 会把“旧 SDK 设备 + 新 INDI 设备”同时下发，造成重复显示。
                QStringList staleSdkCameraNames;
                for (const auto &id : g_sdkQhyCamIds)
                {
                    const QString name = id.trimmed();
                    if (!name.isEmpty() && !staleSdkCameraNames.contains(name))
                        staleSdkCameraNames.push_back(name);
                }
                if (idxMain >= 0 && idxMain < systemdevicelist.system_devices.size())
                {
                    const QString name = systemdevicelist.system_devices[idxMain].DeviceIndiName.trimmed();
                    if (!name.isEmpty() && !staleSdkCameraNames.contains(name))
                        staleSdkCameraNames.push_back(name);
                    systemdevicelist.system_devices[idxMain].isConnect = false;
                    systemdevicelist.system_devices[idxMain].isBind = false;
                }
                if (idxGuider >= 0 && idxGuider < systemdevicelist.system_devices.size())
                {
                    const QString name = systemdevicelist.system_devices[idxGuider].DeviceIndiName.trimmed();
                    if (!name.isEmpty() && !staleSdkCameraNames.contains(name))
                        staleSdkCameraNames.push_back(name);
                    systemdevicelist.system_devices[idxGuider].isConnect = false;
                    systemdevicelist.system_devices[idxGuider].isBind = false;
                }
                if (idxPole >= 0 && idxPole < systemdevicelist.system_devices.size())
                {
                    const QString name = systemdevicelist.system_devices[idxPole].DeviceIndiName.trimmed();
                    if (!name.isEmpty() && !staleSdkCameraNames.contains(name))
                        staleSdkCameraNames.push_back(name);
                    systemdevicelist.system_devices[idxPole].isConnect = false;
                    systemdevicelist.system_devices[idxPole].isBind = false;
                }

                cleanupQhySdkPoolAndResource("SetConnectionMode: Camera SDK->INDI", "CameraPool");

                if (wsThread != nullptr)
                {
                    for (const auto &name : staleSdkCameraNames)
                    {
                        emit wsThread->sendMessageToClient("deleteDeviceAllocationList:" + name);
                    }
                }
            }
        }

        Logger::Log("SetConnectionMode | Device " + deviceDescription.toStdString() +
                    " connection mode set to " + mode.toStdString(),
                    LogLevel::INFO, DeviceType::MAIN);

        // 保存配置到文件
        Tools::saveSystemDeviceList(systemdevicelist);

        // 回包：当前设备 success；若联动了同驱动相机组也回包，方便前端 UI 同步
        emit wsThread->sendMessageToClient("SetConnectionModeSuccess:" + deviceDescription + ":" + mode);
        if (sameDriverCameraGroup) {
            for (const auto &peerDesc : linkedCameraDescriptions)
            {
                if (peerDesc != deviceDescription)
                    emit wsThread->sendMessageToClient("SetConnectionModeSuccess:" + peerDesc + ":" + mode);
            }
        }
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "disconnectSelectDriver")
    {
        Logger::Log("disconnect Driver " + parts[1].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        disconnectDriver(parts[1]);
        Logger::Log("disconnect Driver " + parts[1].trimmed().toStdString() + " success!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (parts[0].trimmed() == "testQtServerProcess")
    {
        // Logger::Log("testQtServerProcess ... .....................", LogLevel::DEBUG, DeviceType::MAIN);
        emit wsThread->sendProcessCommandReturn("ServerInitSuccess");
    }
    else if (parts[0].trimmed() == "localMessage")
    {
        if (parts.size() >= 4)
        {
            Logger::Log("localMessage ...", LogLevel::DEBUG, DeviceType::MAIN);
            localLat = parts[1].trimmed();
            localLon = parts[2].trimmed();
            if (parts.size() >= 5)
            {
                localAppVersion = parts[4].trimmed();
            }
            // Logger::Log("1-----------初始参数设置: " + localLat.toStdString() + "," + localLon.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
            if (parts[3].trimmed() == "zh")
            {
                setClientSettings("ClientLanguage", "cn");
                localLanguage = "cn";
            }
            else
            {
                setClientSettings("ClientLanguage", parts[3].trimmed());
                localLanguage = parts[3].trimmed();
            }
            emit wsThread->sendMessageToClient(message);
            Logger::Log("localMessage finish!", LogLevel::DEBUG, DeviceType::MAIN);
        }
        else
        {
            // Logger::Log("1------------恢复浏览器状态 ...", LogLevel::DEBUG, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("localMessage:" + localLat + ":" + localLon + ":" + localLanguage);
        }
    }
    else if (parts[0].trimmed() == "showRoiImageSuccess" && parts.size() == 2)
    {
        Logger::Log("showRoiImageSuccess ...", LogLevel::DEBUG, DeviceType::MAIN);
        // if (parts[1].trimmed() == "true") focusLoopShooting(true);
        // else focusLoopShooting(false);
        Logger::Log("showRoiImageSuccess finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (parts[0].trimmed() == "getLastSelectDevice")
    {
        Logger::Log("getLastSelectDevice ...", LogLevel::DEBUG, DeviceType::MAIN);
        getLastSelectDevice();
    }
    else if(parts[0].trimmed() == "CheckBoxSpace")
    {
        Logger::Log("CheckBoxSpace ...", LogLevel::DEBUG, DeviceType::MAIN);
        getCheckBoxSpace();
        Logger::Log("CheckBoxSpace finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if(parts[0].trimmed() == "ClearLogs")
    {
        Logger::Log("ClearLogs ...", LogLevel::DEBUG, DeviceType::MAIN);
        clearLogs();
        Logger::Log("ClearLogs finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if(parts[0].trimmed() == "ClearBoxCache")
    {
        Logger::Log("ClearBoxCache ...", LogLevel::DEBUG, DeviceType::MAIN);
        bool clearCache = true;
        bool clearUpdatePack = false;
        bool clearBackup = false;
        // 支持形如 ClearBoxCache:1:0:1 的扩展协议
        if (parts.size() >= 4)
        {
            clearCache      = (parts[1].trimmed() != "0");
            clearUpdatePack = (parts[2].trimmed() != "0");
            clearBackup     = (parts[3].trimmed() != "0");
        }
        clearBoxCache(clearCache, clearUpdatePack, clearBackup);
        Logger::Log("ClearBoxCache finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if(parts[0].trimmed() == "loadSDKVersionAndUSBSerialPath")
    {
        Logger::Log("loadSDKVersionAndUSBSerialPath ...", LogLevel::DEBUG, DeviceType::MAIN);
        loadSDKVersionAndUSBSerialPath();
        Logger::Log("loadSDKVersionAndUSBSerialPath finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    };

    run();
    return true;
}
