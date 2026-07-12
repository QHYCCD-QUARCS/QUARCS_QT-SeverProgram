#include "mainwindow_command_support.h"

namespace {

bool isValidSystemDeviceIndex(const SystemDeviceList &deviceList, int index)
{
    return index >= 0 && index < deviceList.system_devices.size();
}

int scoreByIdLinkForType(const QString &fileNameLower, const QString &driverType)
{
    int score = 0;
    if (driverType == "Focuser")
    {
        if (fileNameLower.contains("gigadevice")) score += 2;
        if (fileNameLower.contains("gd32")) score += 2;
        if (fileNameLower.contains("cdc_acm")) score += 1;
        if (fileNameLower.contains("acm")) score += 1;
    }
    else if (driverType == "Mount")
    {
        if (fileNameLower.contains("1a86")) score += 2;
        if (fileNameLower.contains("usb_serial")) score += 2;
        if (fileNameLower.contains("ch34")) score += 2;
        if (fileNameLower.contains("wch")) score += 1;
        if (fileNameLower.contains("ttyusb")) score += 1;
    }
    return score;
}

} // namespace

void MainWindow::readDriversListFromFiles(const std::string &filename, DriversList &drivers_list_from,
                                          std::vector<DevGroup> &dev_groups_from, std::vector<Device> &devices_from)
{
    Logger::Log("Opening XML file: " + filename, LogLevel::INFO, DeviceType::GUIDER);
    QFile file(QString::fromStdString(filename));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        Logger::Log("Failed to open file: " + filename, LogLevel::ERROR, DeviceType::GUIDER);
        return;
    }
    QXmlStreamReader xml(&file);
    while (!xml.atEnd() && !xml.hasError())
    {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == "devGroup")
        {
            DevGroup dev_group;
            dev_group.group = xml.attributes().value("group").toString().toUtf8().constData();
            drivers_list_from.dev_groups.push_back(dev_group);
            Logger::Log("Added device group: " + dev_group.group.toStdString(), LogLevel::INFO, DeviceType::GUIDER);
        }
    }
    DIR *dir = opendir("/usr/share/indi");
    std::string DirPath = "/usr/share/indi/";
    std::string xmlpath;

    int index;

    DriversList drivers_list_get;
    std::vector<DevGroup> dev_groups_get;
    std::vector<Device> devices_get;

    DriversList drivers_list_xmls;
    DriversList drivers_list_xmls_null;
    std::vector<DevGroup> dev_groups_xmls;
    std::vector<Device> devices_xmls;

    std::vector<DevGroup> dev_groups;
    std::vector<Device> devices;

    if (dir == nullptr)
    {
        Logger::Log("Unable to find INDI drivers directory at /usr/share/indi", LogLevel::ERROR, DeviceType::GUIDER);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (strcmp(entry->d_name + strlen(entry->d_name) - 4, ".xml") == 0)
        {
            if (strcmp(entry->d_name + strlen(entry->d_name) - 6, "sk.xml") == 0)
            {
                continue; // Skip sky charts
            }
            else
            {
                xmlpath = DirPath + entry->d_name;
                QFile file(QString::fromStdString(xmlpath));
                if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                {
                    Logger::Log("Failed to open file: " + xmlpath, LogLevel::ERROR, DeviceType::GUIDER);
                }

                QXmlStreamReader xml(&file);

                while (!xml.atEnd() && !xml.hasError())
                {
                    xml.readNext();
                    if (xml.isStartElement() && xml.name() == "devGroup")
                    {
                        DevGroup dev_group;
                        dev_group.group = xml.attributes().value("group").toString().toUtf8().constData();
                        dev_groups.push_back(dev_group);
                        while (!(xml.isEndElement() && xml.name() == "devGroup"))
                        {
                            xml.readNext();
                            if (xml.isStartElement() && xml.name() == "device")
                            {
                                Device device;
                                device.label = xml.attributes().value("label").toString().toStdString();

                                device.manufacturer = xml.attributes().value("manufacturer").toString().toStdString();
                                devices.push_back(device);
                                while (!(xml.isEndElement() && xml.name() == "device"))
                                {
                                    xml.readNext();
                                    if (xml.isStartElement() && xml.name() == "driver")
                                    {
                                        device.driver_name = xml.readElementText().toStdString();
                                    }
                                    else if (xml.isStartElement() && xml.name() == "version")
                                    {
                                        device.version = xml.readElementText().toStdString();
                                    }
                                }
                                dev_group.devices.push_back(device);
                            }
                        }
                        drivers_list_xmls.dev_groups.push_back(dev_group);
                    }
                }
            }
        }
        for (int i = 0; i < drivers_list_xmls.dev_groups.size(); i++)
        {
            for (int j = 0; j < drivers_list_from.dev_groups.size(); j++)
            {
                if (drivers_list_xmls.dev_groups[i].group == drivers_list_from.dev_groups[j].group)
                {
                    for (int k = 0; k < drivers_list_xmls.dev_groups[i].devices.size(); k++)
                    {
                        Device dev;
                        dev.driver_name = drivers_list_xmls.dev_groups[i].devices[k].driver_name;
                        dev.label = drivers_list_xmls.dev_groups[i].devices[k].label;
                        dev.version = drivers_list_xmls.dev_groups[i].devices[k].version;
                        drivers_list_from.dev_groups[j].devices.push_back(dev);
                    }
                }
            }
        }
        drivers_list_xmls = drivers_list_xmls_null;
    }
    closedir(dir);
    Logger::Log("Completed reading and processing INDI driver files.", LogLevel::INFO, DeviceType::GUIDER);
}

void MainWindow::printDevGroups2(const DriversList drivers_list, int ListNum, QString group)
{
    Logger::Log("Printing device groups for group: " + group.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("=============================== Print DevGroups ===============================", LogLevel::INFO, DeviceType::MAIN);
    bool foundGroup = false;
    for (int i = 0; i < drivers_list.dev_groups.size(); i++)
    {
        if (drivers_list.dev_groups[i].group == group)
        {
            foundGroup = true;
            Logger::Log("Processing device group: " + drivers_list.dev_groups[i].group.toStdString(), LogLevel::INFO, DeviceType::MAIN);
            // Uncomment and modify the following lines if you want to log device details and send messages
            // for (int j = 0; j < drivers_list.dev_groups[i].devices.size(); j++)
            // {
            //     qDebug() << QString::fromStdString(drivers_list.dev_groups[i].devices[j].driver_name) << QString::fromStdString(drivers_list.dev_groups[i].devices[j].version) << QString::fromStdString(drivers_list.dev_groups[i].devices[j].label);
            //     Logger::Log("Device details: " + drivers_list.dev_groups[i].devices[j].label + ", " + drivers_list.dev_groups[i].devices[j].driver_name + ", " + drivers_list.dev_groups[i].devices[j].version, LogLevel::INFO, DeviceType::MAIN);
            //     websocket->messageSend("AddDriver:"+QString::fromStdString(drivers_list.dev_groups[i].devices[j].label)+":"+QString::fromStdString(drivers_list.dev_groups[i].devices[j].driver_name));
            // }
            DeviceSelect(ListNum, i);
        }
    }
    if (!foundGroup)
    {
        systemdevicelist.currentDeviceCode = -1;
        ::drivers_list.selectedGrounp = -1;
        Logger::Log("printDevGroups2 | Device group not found: " + group.toStdString() +
                        ", currentDeviceCode reset to -1",
                    LogLevel::ERROR, DeviceType::MAIN);
    }
    Logger::Log("Completed printing device groups.", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::DeviceSelect(int systemNumber, int grounpNumber)
{
    // Tools::clearSystemDeviceListItem(systemdevicelist, systemNumber);
    SelectIndiDevice(systemNumber, grounpNumber);
}

void MainWindow::SelectIndiDevice(int systemNumber, int grounpNumber)
{
    if (!isValidSystemDeviceIndex(systemdevicelist, systemNumber))
    {
        systemdevicelist.currentDeviceCode = -1;
        drivers_list.selectedGrounp = -1;
        Logger::Log("SelectIndiDevice | Invalid systemNumber: " + std::to_string(systemNumber),
                    LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    if (grounpNumber < 0 || grounpNumber >= drivers_list.dev_groups.size())
    {
        systemdevicelist.currentDeviceCode = -1;
        drivers_list.selectedGrounp = -1;
        Logger::Log("SelectIndiDevice | Invalid grounpNumber: " + std::to_string(grounpNumber),
                    LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    systemdevicelist.currentDeviceCode = systemNumber;
    drivers_list.selectedGrounp = grounpNumber;

    // switch (systemNumber)
    // {
    // case 0:
    //     systemdevicelist.system_devices[systemNumber].Description = "Mount";
    //     break;
    // case 1:
    //     systemdevicelist.system_devices[systemNumber].Description = "Guider";
    //     break;
    // case 2:
    //     systemdevicelist.system_devices[systemNumber].Description = "PoleCamera";
    //     break;
    // case 20:
    //     systemdevicelist.system_devices[systemNumber].Description = "Main Camera #1";
    //     break;
    // case 21:
    //     systemdevicelist.system_devices[systemNumber].Description = "CFW #1";
    //     break;
    // case 22:
    //     systemdevicelist.system_devices[systemNumber].Description = "Focuser #1";
    //     break;

    // default:
    //     break;
    // }

    // qDebug() << "SelectIndiDevice:" << systemdevicelist.currentDeviceCode << "," << drivers_list.selectedGrounp;

    for (int i = 0; i < drivers_list.dev_groups[grounpNumber].devices.size(); i++)
    {
        if (grounpNumber == 1 && (QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].label) == "QHY CCD" || QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].driver_name) == "indi_qhy_ccd"))
        {
            continue;
        }
        if (grounpNumber == 20 && (QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].label) == "QHY CCD2" || QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].driver_name) == "indi_qhy_ccd2"))
        {
            continue;
        }
        // qDebug() << QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].driver_name) << QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].version) << QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].label) << QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].manufacturer);
        emit wsThread->sendMessageToClient("AddDriver:" + QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].label) + ":" + QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].driver_name));
        // qDebug() << "AddDriver:" + QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].label) + ":" + QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].driver_name);
    }
}

bool MainWindow::indi_Driver_Confirm(QString DriverName, QString BaudRate)
{
    if (!isValidSystemDeviceIndex(systemdevicelist, systemdevicelist.currentDeviceCode))
    {
        Logger::Log("indi_Driver_Confirm | currentDeviceCode out of bounds: " + std::to_string(systemdevicelist.currentDeviceCode),
                    LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    switch (systemdevicelist.currentDeviceCode)
    {
    case 0:
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "Mount";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].BaudRate = BaudRate.toInt();
        Logger::Log("indi_Driver_Confirm | Mount | DriverName: " + DriverName.toStdString() + " BaudRate: " + std::to_string(BaudRate.toInt()), LogLevel::INFO, DeviceType::MAIN);
        break;
    case 1:
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "Guider";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].BaudRate = BaudRate.toInt();
        Logger::Log("indi_Driver_Confirm | Guider | DriverName: " + DriverName.toStdString() + " BaudRate: " + std::to_string(BaudRate.toInt()), LogLevel::INFO, DeviceType::MAIN);
        break;
    case 2:
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "PoleCamera";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].BaudRate = BaudRate.toInt();
        Logger::Log("indi_Driver_Confirm | PoleCamera | DriverName: " + DriverName.toStdString() + " BaudRate: " + std::to_string(BaudRate.toInt()), LogLevel::INFO, DeviceType::MAIN);
        break;
    case 20:
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "MainCamera";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].BaudRate = BaudRate.toInt();
        Logger::Log("indi_Driver_Confirm | MainCamera | DriverName: " + DriverName.toStdString() + " BaudRate: " + std::to_string(BaudRate.toInt()), LogLevel::INFO, DeviceType::MAIN);
        break;
    case 21:
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "CFW";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].BaudRate = BaudRate.toInt();
        Logger::Log("indi_Driver_Confirm | CFW | DriverName: " + DriverName.toStdString() + " BaudRate: " + std::to_string(BaudRate.toInt()), LogLevel::INFO, DeviceType::MAIN);
        break;
    case 22:
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "Focuser";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].BaudRate = BaudRate.toInt();
        Logger::Log("indi_Driver_Confirm | Focuser | DriverName: " + DriverName.toStdString() + " BaudRate: " + std::to_string(BaudRate.toInt()), LogLevel::INFO, DeviceType::MAIN);
        break;

    default:
        Logger::Log("indi_Driver_Confirm | Invalid currentDeviceCode: " + std::to_string(systemdevicelist.currentDeviceCode), LogLevel::ERROR, DeviceType::MAIN);
        break;
    }

    auto &slot = systemdevicelist.system_devices[systemdevicelist.currentDeviceCode];
    slot.DriverIndiName = DriverName;
    slot.isConnect = false;
    slot.isBind = false;

    // 🔥 自动从 SdkDriverRegistry 查询是否支持 SDK 模式
    bool supportsSDK = SdkDriverRegistry::instance().supportsSDK(DriverName.toStdString());

    if (supportsSDK)
    {
        // 获取 SDK 首选名称
        std::string sdkDriverName = SdkDriverRegistry::instance().getSDKDriverName(
            DriverName.toStdString()
        );

        // 标记支持 SDK（用于前端显示"连接模式"切换选项）
        if (!slot.DriverFrom.contains("SDK", Qt::CaseInsensitive))
        {
            slot.DriverFrom = DriverName + "SDK";  // 例如 "indi_qhy_ccdSDK"
        }

        // 🆕 保存 SDK 驱动名（用于后续切换到 SDK 模式时自动选择正确的驱动）
        slot.SDKDriverName = QString::fromStdString(sdkDriverName);

        Logger::Log("indi_Driver_Confirm | Driver supports SDK: " +
                   DriverName.toStdString() + " -> " + sdkDriverName,
                   LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        // 纯 INDI 驱动，不支持 SDK
        slot.DriverFrom = "INDI";
        slot.SDKDriverName = "";
        // 若驱动不支持 SDK，则强制切回 INDI 模式，避免沿用上次的 isSDKConnect=true 导致后续 ConnectDriver 误走 SDK 流程
        slot.isSDKConnect = false;

        Logger::Log("indi_Driver_Confirm | Driver is INDI-only (no SDK support): " +
                   DriverName.toStdString(),
                   LogLevel::INFO, DeviceType::MAIN);
    }

    // 持久化：否则重启/重连后 DriverFrom 会丢失，前端收到 SelectedDriverList(...:false:...)
    Tools::saveSystemDeviceList(systemdevicelist);

    // 立即刷新前端缓存：让 supportSDK/connectionMode 立刻生效，UI 及时显示"连接模式"下拉框
    loadSelectedDriverList();

    return true;
}

QString MainWindow::getSDKDriverName(const QString& deviceType)
{
    // 根据设备类型找到对应的槽位索引
    int index = -1;
    if (deviceType == "MainCamera") index = 20;
    // Guider（导星相机）在 system_devices[1]
    else if (deviceType == "Guider" || deviceType == "GuideCamera") index = 1;
    // PoleCamera（电子极轴镜）在 system_devices[2]
    else if (deviceType == "PoleCamera") index = 2;
    // CFW（外置滤镜轮）在 system_devices[21]
    else if (deviceType == "CFW") index = 21;
    else if (deviceType == "Focuser") index = 22;
    // ... 可以继续添加其他设备类型的映射

    if (index < 0 || index >= systemdevicelist.system_devices.size())
        return "";

    const auto& device = systemdevicelist.system_devices[index];

    // 🔥 关键：直接从 SdkDriverRegistry 查询
    if (!device.DriverIndiName.isEmpty())
    {
        std::string sdkDriver = SdkDriverRegistry::instance().getSDKDriverName(
            device.DriverIndiName.toStdString()
        );

        if (!sdkDriver.empty())
            return QString::fromStdString(sdkDriver);
    }



    return "";
}

bool MainWindow::indi_Driver_Clear(int deviceCode)
{
    if (!isValidSystemDeviceIndex(systemdevicelist, deviceCode)) {
        Logger::Log("indi_Driver_Clear | deviceCode out of bounds: " + std::to_string(deviceCode), LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    // Description 是固定槽位的逻辑角色，不属于驱动选择。Clear Driver 只能
    // 清除驱动、设备选择和运行态，不能删除 Guider/MainCamera 等角色身份。
    const auto roleForSlot = [](int index) -> QString {
        switch (index) {
        case 0: return QStringLiteral("Mount");
        case 1: return QStringLiteral("Guider");
        case 2: return QStringLiteral("PoleCamera");
        case 20: return QStringLiteral("MainCamera");
        case 21: return QStringLiteral("CFW");
        case 22: return QStringLiteral("Focuser");
        default: return QString();
        }
    };
    const QString fixedRole = roleForSlot(deviceCode);
    if (!fixedRole.isEmpty())
        systemdevicelist.system_devices[deviceCode].Description = fixedRole;
    systemdevicelist.system_devices[deviceCode].DriverIndiName = "";
    systemdevicelist.system_devices[deviceCode].SDKDriverName = "";
    systemdevicelist.system_devices[deviceCode].BaudRate = 9600;
    systemdevicelist.system_devices[deviceCode].DeviceIndiName = "";
    systemdevicelist.system_devices[deviceCode].DeviceIndiGroup = -1;
    systemdevicelist.system_devices[deviceCode].isConnect = false;
    systemdevicelist.system_devices[deviceCode].isBind = false;
    systemdevicelist.system_devices[deviceCode].isSDKConnect = false;
    systemdevicelist.system_devices[deviceCode].dp = nullptr;

    if (systemdevicelist.currentDeviceCode == deviceCode)
        systemdevicelist.currentDeviceCode = -1;
    if (drivers_list.selectedGrounp >= 0)
        drivers_list.selectedGrounp = -1;

    // 保存配置到文件，确保清除操作持久化
    Tools::saveSystemDeviceList(systemdevicelist);

    // 发送更新后的驱动列表给前端，确保前端UI同步更新
    loadSelectedDriverList();

    Logger::Log("indi_Driver_Clear | Driver cleared for deviceCode=" + std::to_string(deviceCode) +
                    " and configuration saved",
                LogLevel::INFO, DeviceType::MAIN);
    return true;
}

void MainWindow::indi_Device_Confirm(QString DeviceName, QString DriverName)
{
    //   qApp->processEvents();

    int deviceCode;
    deviceCode = systemdevicelist.currentDeviceCode;

    if (!isValidSystemDeviceIndex(systemdevicelist, deviceCode))
    {
        Logger::Log("indi_Device_Confirm | currentDeviceCode out of bounds: " + std::to_string(deviceCode),
                    LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    systemdevicelist.system_devices[deviceCode].DriverIndiName = DriverName;
    systemdevicelist.system_devices[deviceCode].DeviceIndiGroup = drivers_list.selectedGrounp;
    systemdevicelist.system_devices[deviceCode].DeviceIndiName = DeviceName;

    Logger::Log("system device(" + DeviceName.toStdString() + ") successfully selected", LogLevel::INFO, DeviceType::MAIN);

    Tools::printSystemDeviceList(systemdevicelist);

    Tools::saveSystemDeviceList(systemdevicelist);
}

uint32_t MainWindow::clearCheckDeviceExist(QString drivername, bool &isExist)
{
    Logger::Log("Stopping all INDI drivers.", LogLevel::INFO, DeviceType::MAIN);
    Tools::stopIndiDriverAll(drivers_list);
    Logger::Log("Starting INDI driver: " + drivername.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    Tools::startIndiDriver(drivername);

    sleep(1); // must wait some time here

    MyClient *searchClient;
    searchClient = new MyClient();
    Logger::Log("Initialized new MyClient for device search.", LogLevel::INFO, DeviceType::MAIN);
    searchClient->PrintDevices();

    searchClient->setServer("localhost", 7624);
    searchClient->setConnectionTimeout(3, 0);
    searchClient->ClearDevices(); // clear device list

    Logger::Log("Attempting to connect to INDI server at localhost:7624", LogLevel::INFO, DeviceType::MAIN);
    bool connected = searchClient->connectServer();

    if (connected == false)
    {
        Logger::Log("Failed to connect to INDI server, can not find server", LogLevel::ERROR, DeviceType::MAIN);
        return QHYCCD_ERROR;
    }

    sleep(1); // connect server will generate the callback of newDevice and then put the device into list. this need take some time and it is non-block
    searchClient->PrintDevices();

    if (searchClient->GetDeviceCount() == 0)
    {
        Logger::Log("No devices found on INDI server.", LogLevel::INFO, DeviceType::MAIN);
        searchClient->disconnectServer();
        isExist = false;
        emit wsThread->sendMessageToClient("ScanFailed:No device found.");
        return QHYCCD_SUCCESS;
    }

    Logger::Log("Devices found: " + std::to_string(searchClient->GetDeviceCount()), LogLevel::INFO, DeviceType::MAIN);
    for (int i = 0; i < searchClient->GetDeviceCount(); i++)
    {
        emit wsThread->sendMessageToClient("AddDevice:" + QString::fromStdString(searchClient->GetDeviceNameFromList(i)));
    }

    searchClient->disconnectServer();
    searchClient->ClearDevices();

    Tools::stopIndiDriver(drivername);
    Logger::Log("INDI driver stopped: " + drivername.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    return QHYCCD_SUCCESS;
}

void MainWindow::disconnectIndiServer(MyClient *client)
{
    Logger::Log("disconnectIndiServer start ...", LogLevel::INFO, DeviceType::MAIN);
    // 防御性检查：客户端指针为空则直接返回，避免段错误
    if (client == nullptr)
    {
        Logger::Log("disconnectIndiServer | client is nullptr", LogLevel::ERROR, DeviceType::MAIN);
        Tools::stopIndiDriverAll(drivers_list);
        ConnectDriverList.clear();
        return;
    }

    int deviceCount = client->GetDeviceCount();
    if (deviceCount > 0)
    {
        for (int i = 0; i < deviceCount; i++)
        {
            INDI::BaseDevice *device = client->GetDeviceFromList(i);
            if (device == nullptr)
            {
                Logger::Log("disconnectAllDevice | Device at index " + std::to_string(i) + " is nullptr", LogLevel::WARNING, DeviceType::MAIN);
                continue;
            }

            if (device->isConnected())
            {
                const char *devName = device->getDeviceName();
                QString qName = devName ? QString::fromUtf8(devName) : QString("UnknownDevice");

                client->disconnectDevice(devName ? devName : "");
                int num = 0;
                while (device->isConnected())
                {
                    Logger::Log("disconnectAllDevice | Waiting for disconnect device (" + qName.toStdString() + ") finish...", LogLevel::INFO, DeviceType::MAIN);
                    sleep(1);
                    num++;

                    if (num > 10)
                    {
                        Logger::Log("disconnectAllDevice | device (" + qName.toStdString() + ") disconnect failed.", LogLevel::WARNING, DeviceType::MAIN);
                        break;
                    }
                }
                Logger::Log("disconnectAllDevice | device (" + qName.toStdString() + ") disconnected successfully.", LogLevel::INFO, DeviceType::MAIN);
            }
        }
    }
    else
    {
        Logger::Log("disconnectIndiServer | no devices to disconnect (device count = 0)", LogLevel::INFO, DeviceType::MAIN);
    }

    Tools::stopIndiDriverAll(drivers_list);
    ConnectDriverList.clear();

    client->ClearDevices();
    client->disconnectServer();
    int k = 10;
    while (k--)
    {
        if (!client->isServerConnected())
        {
            Logger::Log("Server disconnected successfully.", LogLevel::INFO, DeviceType::MAIN);
            break;
        }
        sleep(1);
        // qApp->processEvents();
        Logger::Log("Waiting for server to disconnect...", LogLevel::INFO, DeviceType::MAIN);
    }
    Logger::Log("disconnectServer finished.", LogLevel::INFO, DeviceType::MAIN);
    if (indi_Client != nullptr)
    {
        indi_Client->PrintDevices();
    }
}

void MainWindow::cleanupQhySdkPoolAndResource(const QString& reason, const QString& deviceType)
{
    // -----------------------------
    // 约定：system_devices 中的槽位索引（避免魔法数字）
    // -----------------------------
    constexpr int kIdxMainCamera = 20;
    constexpr int kIdxFocuser    = 22;

    // -----------------------------
    // 1) 解析清理范围（Plan）
    // -----------------------------
    const bool cleanupAll        = (deviceType == "All");
    const bool cleanupMainCamera = cleanupAll || (deviceType == "MainCamera");
    const bool cleanupPoleCamera = cleanupAll || (deviceType == "PoleCamera");
    const bool cleanupFocuser    = cleanupAll || (deviceType == "Focuser");
    const bool cleanupCameraPool = cleanupAll || (deviceType == "CameraPool");

    // 注释（与实际行为一致）：
    // - MainCamera：仅清理主相机（句柄+绑定/运行态），不触碰其他相机句柄
    // - PoleCamera：SDK 极轴镜与主相机/导星相机共享同一相机池，因此按 CameraPool 清理
    // - CameraPool：清理整个相机池（关闭所有句柄 + ReleaseSdkResource），池清空后主相机绑定必然无效，因此也会复位主相机绑定/运行态

    const bool hasAnyCameraHandle =
        (sdkMainCameraHandle != nullptr) || (sdkGuiderHandle != nullptr) ||
        (sdkPoleScopeHandle != nullptr) || (!g_sdkQhyCamHandles.isEmpty());
    const bool hasFocuserHandle   = (sdkFocuserHandle != nullptr);

    const bool shouldCleanupCamera =
        (cleanupMainCamera || cleanupPoleCamera || cleanupCameraPool) && hasAnyCameraHandle;
    const bool shouldCleanupFocuser =
        cleanupFocuser && hasFocuserHandle;

    // 如果既没有相机也没有电调需要清理，直接返回
    if (!shouldCleanupCamera && !shouldCleanupFocuser)
        return;

    Logger::Log("cleanupQhySdkPoolAndResource | reason=" + reason.toStdString() +
                ", deviceType=" + deviceType.toStdString(),
                LogLevel::INFO, DeviceType::MAIN);

    // -----------------------------
    // 2) 小工具：统一线程投递（可读性 + 去重）
    // -----------------------------
    auto runOnCamThreadSync = [&](std::function<void()> fn) {
        if (sdkCamExec && sdkCamExec->isRunning())
            sdkCamExec->postAndWait(std::move(fn));
        else
            fn();
    };

    auto runOnFocuserThreadSync = [&](std::function<void()> fn) {
        if (sdkFocuserExec && sdkFocuserExec->isRunning())
            sdkFocuserExec->postAndWait(std::move(fn));
        else
            fn();
    };

    auto resetDeviceEntry = [&](int index) {
        if (index < 0 || index >= systemdevicelist.system_devices.size())
            return;
        auto &d = systemdevicelist.system_devices[index];
        d.isConnect = false;
        d.isBind = false;
        // 资源清理/Disconnect 只复位运行态。DeviceIndiName 是用户选定的
        // 持久化绑定，必须保留给下一次 Connect All 静默回连；只有明确的
        // UnBindingDevice/ClearDriver 流程才允许清除它。
        d.dp = NULL;
    };

    auto resetMainCameraRuntimeState = [&]() {
        // 运行态/前后端状态统一回到空闲，避免残留“曝光中”
        glMainCameraStatu = "IDLE";
        ShootStatus = "IDLE";
        glIsFocusingLooping = false;
        isFocusLoopShooting = false;
    };

    auto makeCancelExposureCmd = [&]() {
        SdkCommand cmd;
        cmd.type = SdkCommandType::Custom;
        cmd.name = "CancelExposure";
        cmd.payload = std::any();
        return cmd;
    };

    auto cancelAndCloseCamera = [&](SdkDeviceHandle h) {
        if (h == nullptr) return;
        // 直接通过设备句柄调用，无需指定驱动名称
        SdkManager::instance().callByHandle(h, makeCancelExposureCmd());
        SdkManager::instance().closeByHandle(h);
    };

    // 释放 SDK 全局资源需要“驱动名”；优先 MainCamera，其次 Guider，最后兜底 QHYCCD（若已注册）
    const std::string releaseDriverNameStd = [&]() -> std::string {
        QString dn = getSDKDriverName("MainCamera");
        if (dn.isEmpty())
            dn = getSDKDriverName("Guider");
        if (dn.isEmpty())
            dn = getSDKDriverName("PoleCamera");
        if (!dn.isEmpty())
            return dn.toStdString();

        // 兜底：若驱动映射缺失（比如只配置了导星/未配置主相机），仍尽量释放
        auto regs = SdkManager::instance().listRegisteredDrivers();
        for (const auto &n : regs)
        {
            if (n == "QHYCCD")
                return n;
        }
        if (!regs.empty())
            return regs.front();
        return {};
    }();

    // -----------------------------
    // 3) 先清理电调（与相机资源独立）
    // -----------------------------
    if (shouldCleanupFocuser)
    {
        const SdkDeviceHandle h = sdkFocuserHandle;

        runOnFocuserThreadSync([h]() {
            // 直接通过设备句柄关闭，无需指定驱动名称
            SdkManager::instance().closeByHandle(h);
        });

        // 本地状态复位
        sdkFocuserHandle = nullptr;
        sdkFocuserPort.clear();

        // 设备表复位（电调）
        resetDeviceEntry(kIdxFocuser);
    }

    // -----------------------------
    // 4) 清理相机（仅主相机 / 整池）
    // -----------------------------
    if (shouldCleanupCamera)
    {
        const bool cleanupFullPool = cleanupCameraPool || cleanupPoleCamera; // CameraPool / PoleCamera / All

        if (!cleanupFullPool && deviceType == "MainCamera")
        {
            // 4A) 仅清理主相机：cancel + close 主句柄，并从池中摘除（不触碰其他相机）
            if (sdkMainCameraHandle != nullptr)
            {
                const SdkDeviceHandle mainHandle = sdkMainCameraHandle;
                const int poolIndex = g_sdkMainCameraPoolIndex;

                runOnCamThreadSync([=]() {
                    // 直接通过设备句柄调用，无需指定驱动名称
                    SdkManager::instance().callByHandle(mainHandle, makeCancelExposureCmd());
                    SdkManager::instance().closeByHandle(mainHandle);
                });

                // 从池中摘除主相机（如果它在池中）
                if (poolIndex >= 0 && poolIndex < g_sdkQhyCamHandles.size())
                {
                    g_sdkQhyCamHandles[poolIndex] = nullptr;
                    if (poolIndex < g_sdkQhyCamIds.size())
                        g_sdkQhyCamIds[poolIndex].clear();
                }
            }

            // 主相机绑定/运行态复位
            sdkMainCameraHandle = nullptr;
            sdkGuiderHandle = nullptr;
            sdkPoleScopeHandle = nullptr;
            g_sdkMainCameraPoolIndex = -1;
            g_sdkGuiderPoolIndex = -1;
            g_sdkPoleCameraPoolIndex = -1;
            sdkMainCameraId.clear();
            resetMainCameraRuntimeState();

            // 设备表复位（主相机）
            resetDeviceEntry(kIdxMainCamera);
        }
        else
        {
            // 4B) 清理整个相机池：停止轮询 -> 关闭所有句柄 -> ReleaseSdkResource -> 清空池 -> 复位状态
            //
            // 注意：池被清理后主相机绑定也必然失效，因此会一并复位主相机绑定/运行态。
            if (sdkExposureTimer)
            {
                // 若 sdkExposureTimer 的线程归属不明确，建议用 invokeMethod 投递到其线程
                sdkExposureTimer->stop();
            }
            sdkExposureIsROI = false;

            std::vector<SdkDeviceHandle> handles;
            handles.reserve(static_cast<size_t>(g_sdkQhyCamHandles.size()));

            for (int i = 0; i < g_sdkQhyCamHandles.size(); ++i)
            {
                if (g_sdkQhyCamHandles[i] != nullptr)
                    handles.push_back(g_sdkQhyCamHandles[i]);
                g_sdkQhyCamHandles[i] = nullptr;
            }

            runOnCamThreadSync([=]() mutable {
                // 关闭所有句柄（尽量先取消曝光）
                for (auto h : handles)
                {
                    if (h == nullptr) continue;
                    // 直接通过设备句柄调用，无需指定驱动名称
                    SdkManager::instance().callByHandle(h, makeCancelExposureCmd());
                    SdkManager::instance().closeByHandle(h);
                }
                // 释放 SDK 全局资源（必须在全部 close 之后）
                if (!releaseDriverNameStd.empty())
                {
                    SdkCommand relCmd;
                    relCmd.type = SdkCommandType::Custom;
                    relCmd.name = "ReleaseSdkResource";
                    relCmd.payload = std::any();

                    SdkResult relRes = SdkManager::instance().call(releaseDriverNameStd, nullptr, relCmd);
                    if (!relRes.success)
                    {
                        Logger::Log("cleanupQhySdkPoolAndResource | ReleaseSdkResource failed: " + relRes.message,
                                    LogLevel::WARNING, DeviceType::MAIN);
                    }
                    else
                    {
                        Logger::Log("cleanupQhySdkPoolAndResource | ReleaseSdkResource success",
                                    LogLevel::INFO, DeviceType::MAIN);
                    }
                }
                else
                {
                    Logger::Log("cleanupQhySdkPoolAndResource | ReleaseSdkResource skipped: no valid SDK driver name",
                                LogLevel::WARNING, DeviceType::MAIN);
                }
            });

            // 清空池与 ID 列表
            g_sdkQhyCamHandles.clear();
            g_sdkQhyCamIds.clear();

            // 主相机绑定/运行态复位
            sdkMainCameraHandle = nullptr;
            sdkGuiderHandle = nullptr;
            sdkPoleScopeHandle = nullptr;
            g_sdkMainCameraPoolIndex = -1;
            g_sdkGuiderPoolIndex = -1;
            g_sdkPoleCameraPoolIndex = -1;
            sdkMainCameraId.clear();
            resetMainCameraRuntimeState();

            // 设备表复位：
            // CameraPool/All 都应清理“相机角色”绑定状态，避免仅清主相机导致 Guider/PoleCamera 残留“已连接”。
            for (int i = 0; i < systemdevicelist.system_devices.size(); ++i)
            {
                if (!systemdevicelist.system_devices[i].isSDKConnect)
                    continue;
                if (i == kIdxFocuser)
                    continue;
                const QString desc = systemdevicelist.system_devices[i].Description;
                if (desc == "MainCamera" || desc == "Guider" || desc == "PoleCamera")
                    resetDeviceEntry(i);
            }
        }
    }
    else if (cleanupMainCamera)
    {
        // 4C) 没有可关闭的相机句柄，但仍要求“解绑主相机”（用于异常状态/句柄已丢失）
        sdkMainCameraHandle = nullptr;
        sdkGuiderHandle = nullptr;
        sdkPoleScopeHandle = nullptr;
        g_sdkMainCameraPoolIndex = -1;
        g_sdkGuiderPoolIndex = -1;
        g_sdkPoleCameraPoolIndex = -1;
        sdkMainCameraId.clear();
        resetMainCameraRuntimeState();
        resetDeviceEntry(kIdxMainCamera);
    }

    // ReleaseSdkResource 已在“整池清理”路径中由相机线程任务负责执行
}

bool MainWindow::connectIndiServer(MyClient *client)
{
    Logger::Log("connectIndiServer start ...", LogLevel::INFO, DeviceType::MAIN);
    client->setConnectionTimeout(3, 0);
    Logger::Log("connectIndiServer | clear device list ...", LogLevel::INFO, DeviceType::MAIN);
    client->ClearDevices(); // clear device list
    Logger::Log("connectIndiServer | connect server ...", LogLevel::INFO, DeviceType::MAIN);
    client->connectServer();
    int k = 10;
    while (k--)
    {
        if (client->isServerConnected() == true)
        {
            break;
        }
        sleep(1);
        // qApp->processEvents();
        Logger::Log("connectIndiServer | waiting for client connected ...", LogLevel::INFO, DeviceType::MAIN);
    }
    if (client->isServerConnected() == false)
    {
        Logger::Log("connectIndiServer | failed: client is not connected after timeout.", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    sleep(1);
    client->PrintDevices();
    Logger::Log("connectIndiServer finished.", LogLevel::INFO, DeviceType::MAIN);
    return true;
}

void MainWindow::ClearSystemDeviceList()
{
    Logger::Log("ClearSystemDeviceList start ...", LogLevel::INFO, DeviceType::MAIN);
    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        systemdevicelist.system_devices[i].DeviceIndiGroup = -1;
        systemdevicelist.system_devices[i].DeviceIndiName = "";
        systemdevicelist.system_devices[i].DriverFrom = "";
        // systemdevicelist.system_devices[i].DriverIndiName = "";
        systemdevicelist.system_devices[i].isConnect = false;
        systemdevicelist.system_devices[i].dp = NULL;
        systemdevicelist.system_devices[i].isBind = false;
        // systemdevicelist.system_devices[i].Description = "";
    }
    Logger::Log("ClearSystemDeviceList finished.", LogLevel::INFO, DeviceType::MAIN);
    Tools::printSystemDeviceList(systemdevicelist);
}

void MainWindow::getConnectedDevices()
{
    Logger::Log("getConnectedDevices start ...", LogLevel::INFO, DeviceType::MAIN);
    QString deviceType;
    bool isConnect;
    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        deviceType = systemdevicelist.system_devices[i].Description;
        isConnect = systemdevicelist.system_devices[i].isConnect;
        if (deviceType != "" && isConnect)
        {
            emit wsThread->sendMessageToClient("AddDeviceType:" + deviceType);
        }
    }

    for (int i = 0; i < indi_Client->GetDeviceCount(); i++)
    {
        if (indi_Client->GetDeviceFromList(i)->isConnected())
        {
            emit wsThread->sendMessageToClient("DeviceToBeAllocated:Device:" + QString::number(i) + ":" + QString::fromUtf8(indi_Client->GetDeviceFromList(i)->getDeviceName()));
        }
    }

    for (int i = 0; i < ConnectedDevices.size(); i++)
    {
        Logger::Log("getConnectedDevices | Device[" + std::to_string(i) + "]: " + ConnectedDevices[i].DeviceName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectSuccess:" + ConnectedDevices[i].DeviceType + ":" + ConnectedDevices[i].DeviceName);

        if (ConnectedDevices[i].DeviceType == "MainCamera" && isMainCameraConnected())
        {
            emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));
            emit wsThread->sendMessageToClient("MainCameraOffsetRange:" + QString::number(glOffsetMin) + ":" + QString::number(glOffsetMax) + ":" + QString::number(glOffsetValue));
            emit wsThread->sendMessageToClient("MainCameraGainRange:" + QString::number(glGainMin) + ":" + QString::number(glGainMax) + ":" + QString::number(glGainValue));
            if (glUsbTrafficMax > glUsbTrafficMin)
            {
                emit wsThread->sendMessageToClient("MainCameraUsbTrafficRange:" + QString::number(glUsbTrafficMin) + ":" + QString::number(glUsbTrafficMax) + ":" + QString::number(glUsbTrafficValue) + ":" + QString::number(glUsbTrafficStep));
            }

            QString CFWname;
            indi_Client->getCFWSlotName(dpMainCamera, CFWname);
            if (CFWname != "")
            {
                Logger::Log("getConnectedDevices | get CFW Slot Name: " + CFWname.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("ConnectSuccess:CFW:" + CFWname + " (on camera)");
                isFilterOnCamera = true;

                int min, max, pos;
                indi_Client->getCFWPosition(dpMainCamera, pos, min, max);
                Logger::Log("getConnectedDevices | getCFWPosition: " + std::to_string(min) + ", " + std::to_string(max) + ", " + std::to_string(pos), LogLevel::INFO, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("CFWPositionMax:" + QString::number(max));
            }
        }
        else if (ConnectedDevices[i].DeviceType == "Guider" && (dpGuider != nullptr || sdkGuiderHandle != nullptr))
        {
            emit wsThread->sendMessageToClient("GuiderOffsetRange:" + QString::number(glGuiderOffsetMin) + ":" + QString::number(glGuiderOffsetMax) + ":" + QString::number(glGuiderOffsetValue));
            emit wsThread->sendMessageToClient("GuiderGainRange:" + QString::number(glGuiderGainMin) + ":" + QString::number(glGuiderGainMax) + ":" + QString::number(glGuiderGainValue));
        }
    }
    Logger::Log("getConnectedDevices finish!", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::clearConnectedDevices()
{
    ConnectedDevices.clear();
}

void MainWindow::getStagingScheduleData()
{
    if (isStagingScheduleData)
    {
        emit wsThread->sendMessageToClient(StagingScheduleData);
    }

    // 将当前调度列表中的进度同步给前端，便于页面刷新后恢复每一行的执行进度
    for (int i = 0; i < m_scheduList.size(); ++i)
    {
        int progress = m_scheduList[i].progress;
        if (progress < 0)
        {
            progress = 0;
        }
        else if (progress > 100)
        {
            progress = 100;
        }

        // 仅对已有进度的行进行同步，避免干扰尚未使用的默认行
        if (progress > 0)
        {
            emit wsThread->sendMessageToClient(
                "UpdateScheduleProcess:" +
                QString::number(i) + ":" +
                QString::number(progress));
        }
    }

    // 无论是否有暂存数据，都向前端同步当前计划运行状态
    emit wsThread->sendMessageToClient(
        QString("ScheduleRunning:%1").arg(isScheduleRunning ? "true" : "false"));
}

void MainWindow::getStagingGuiderData()
{
    // TODO(PHD2): 前端导星曲线/散点数据同步已暂停；若恢复前端曲线显示，再按协议重发 glPHD_rmsdate
#if 0
    int dataSize = glPHD_rmsdate.size();
    int startIdx = dataSize > 50 ? dataSize - 50 : 0;

    for (int i = startIdx; i < dataSize; i++)
    {
        emit wsThread->sendMessageToClient("AddLineChartData:" + QString::number(i) + ":" + QString::number(glPHD_rmsdate[i].x()) + ":" + QString::number(glPHD_rmsdate[i].y()));
        emit wsThread->sendMessageToClient("AddScatterChartData:" + QString::number(glPHD_rmsdate[i].x()) + ":" + QString::number(-glPHD_rmsdate[i].y()));
        if (i > 50)
        {
            emit wsThread->sendMessageToClient("SetLineChartRange:" + QString::number(i - 50) + ":" + QString::number(i));
        }
    }
#endif
}

void MainWindow::loadSelectedDriverList()
{
    // 🔥 打印所有已注册的 SDK 驱动（包括主名称和别名）
    std::vector<std::string> registeredDrivers = SdkManager::instance().listRegisteredDrivers();
    Logger::Log("loadSelectedDriverList | ========== 已注册的 SDK 驱动列表 ==========", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("loadSelectedDriverList | 已注册的驱动名称总数（包括别名）: " + std::to_string(registeredDrivers.size()), LogLevel::INFO, DeviceType::MAIN);

    // 使用集合去重，按驱动实例分组显示
    // 使用排序后的别名列表作为唯一标识，避免因顺序不同导致重复
    std::unordered_set<std::string> processedDriverKeys;
    int uniqueDriverCount = 0;

    for (size_t i = 0; i < registeredDrivers.size(); i++)
    {
        std::string driverName = registeredDrivers[i];

        // 获取该驱动的所有别名（包括主名称）
        std::vector<std::string> allNames = SdkManager::instance().getDriverAliases(driverName);

        if (allNames.empty())
        {
            continue;
        }

        // 对别名列表进行排序，生成唯一标识键
        std::vector<std::string> sortedNames = allNames;
        std::sort(sortedNames.begin(), sortedNames.end());
        std::string driverKey = "";
        for (size_t j = 0; j < sortedNames.size(); j++)
        {
            if (j > 0) driverKey += "|";
            driverKey += sortedNames[j];
        }

        // 检查是否已经处理过这个驱动实例
        if (processedDriverKeys.find(driverKey) == processedDriverKeys.end())
        {
            uniqueDriverCount++;
            processedDriverKeys.insert(driverKey);

            // 构建显示字符串（使用原始顺序，不排序）
            std::string allNamesStr = "";
            for (size_t j = 0; j < allNames.size(); j++)
            {
                if (j > 0) allNamesStr += ", ";
                allNamesStr += allNames[j];
            }

            // 🔥 获取驱动支持的设备类型
            std::string deviceTypesStr = "";
            try {
                // 通过驱动名称获取驱动指针并尝试获取设备类型
                // 由于 ISdkDriver 接口中没有 supportedDeviceTypes() 方法，
                // 我们需要通过类型转换来调用具体驱动的方法
                std::string firstDriverName = allNames[0];

                // 尝试通过 SdkManager 获取驱动指针（需要访问内部，这里使用已知的驱动名称判断）
                // 检查是否为 QHYCCD 相机驱动
                bool isQhyCamera = false;
                bool isQhyFocuser = false;
                for (const auto& name : allNames)
                {
                    if (name == "indi_qhy_ccd" || name == "indi_qhy_ccd")
                    {
                        isQhyCamera = true;
                        break;
                    }
                    if (name == "indi_qhy_focuser" || name == "indi_qhy_focuser")
                    {
                        isQhyFocuser = true;
                        break;
                    }
                }

                if (isQhyCamera)
                {
                    deviceTypesStr = "设备类型: MainCamera, GuideCamera";
                }
                else if (isQhyFocuser)
                {
                    deviceTypesStr = "设备类型: Focuser";
                }
                else
                {
                    deviceTypesStr = "设备类型: 未知";
                }
            } catch (...) {
                deviceTypesStr = "设备类型: 获取失败";
            }

            Logger::Log("loadSelectedDriverList | 驱动 #" + std::to_string(uniqueDriverCount) + ": " + allNamesStr + " | " + deviceTypesStr, LogLevel::INFO, DeviceType::MAIN);
        }
    }

    Logger::Log("loadSelectedDriverList | 唯一驱动实例数: " + std::to_string(uniqueDriverCount), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("loadSelectedDriverList | ==========================================", LogLevel::INFO, DeviceType::MAIN);

    // 1. 检查 systemdevicelist.system_devices 是否为空
    if (systemdevicelist.system_devices.empty())
    {
        Logger::Log("loadSelectedDriverList | system_devices is empty", LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    // 2. 检查 wsThread 是否为空
    if (wsThread == nullptr)
    {
        Logger::Log("loadSelectedDriverList | wsThread is null", LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    QString order = "SelectedDriverList";

    // 3. 使用安全的范围检查
    try
    {
        for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
        {
            // 4. 检查索引是否有效
            if (i >= systemdevicelist.system_devices.size())
            {
                Logger::Log("loadSelectedDriverList | Index out of bounds: " + std::to_string(i), LogLevel::ERROR, DeviceType::MAIN);
                break;
            }

            // 5. 检查 Description 是否有效
            if (!systemdevicelist.system_devices[i].Description.isEmpty())
            {
                QString description = systemdevicelist.system_devices[i].Description;
                QString driverName = systemdevicelist.system_devices[i].DriverIndiName;

                // 6. 即使 driverName 为空也发送，以便前端可以清除驱动显示
                if (!description.isEmpty())
                {
                    // 判断该设备是否支持 SDK（如果 driverName 为空，则不支持）
                    bool supportSDK = !driverName.isEmpty() && isDeviceTypeSupportSDK(description, driverName);

                    // 获取当前连接模式
                    QString connectionMode = systemdevicelist.system_devices[i].isSDKConnect ? "SDK" : "INDI";

                    // 消息格式：SelectedDriverList:Description:DriverName:SDKSupport:ConnectionMode:...
                    // SDKSupport: "true" 表示支持 SDK，"false" 表示不支持
                    // ConnectionMode: "SDK" 或 "INDI"
                    // 注意：即使 driverName 为空，也发送该条目，以便前端清除驱动显示
                    order += ":" + description + ":" + driverName + ":" +
                             (supportSDK ? "true" : "false") + ":" + connectionMode;

                    Logger::Log("loadSelectedDriverList | Added device: " + description.toStdString() +
                               " - " + (driverName.isEmpty() ? "(empty)" : driverName.toStdString()) +
                               " (SDK支持: " + (supportSDK ? "是" : "否") +
                               ", 连接模式: " + connectionMode.toStdString() + ")",
                               LogLevel::DEBUG, DeviceType::MAIN);
                }
            }
        }

        // 7. 确保 wsThread 和 sendMessageToClient 方法存在
        if (wsThread != nullptr)
        {
            Logger::Log("loadSelectedDriverList | Sending message: " + order.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
            emit wsThread->sendMessageToClient(order);
        }
    }
    catch (const std::exception &e)
    {
        Logger::Log("loadSelectedDriverList | Exception caught: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
    }
    catch (...)
    {
        Logger::Log("loadSelectedDriverList | Unknown exception caught", LogLevel::ERROR, DeviceType::MAIN);
    }
}

void MainWindow::loadBindDeviceTypeList()
{
    QString order = "BindDeviceTypeList";
    if (wsThread == nullptr)
    {
        Logger::Log("LoadBindDeviceTypeList | wsThread is nullptr, skip", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }

    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        if (systemdevicelist.system_devices[i].Description != "" && systemdevicelist.system_devices[i].isConnect == true)
        {
            order += ":" + systemdevicelist.system_devices[i].Description + ":" +
                     systemdevicelist.system_devices[i].DeviceIndiName + ":" +
                     systemdevicelist.system_devices[i].DriverIndiName + ":" + (systemdevicelist.system_devices[i].isBind ? "true" : "false");
            if (systemdevicelist.system_devices[i].Description == "MainCamera" && systemdevicelist.system_devices[i].isBind)
            {
                emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));
                emit wsThread->sendMessageToClient("MainCameraOffsetRange:" + QString::number(glOffsetMin) + ":" + QString::number(glOffsetMax) + ":" + QString::number(glOffsetValue));
                emit wsThread->sendMessageToClient("MainCameraGainRange:" + QString::number(glGainMin) + ":" + QString::number(glGainMax) + ":" + QString::number(glGainValue));

                // CFW 检测：INDI 模式下通过 dpMainCamera 查询；SDK 模式下 dpMainCamera 可能为空，必须跳过避免段错误
                if (!isMainCameraSDK())
                {
                    if (indi_Client != nullptr && dpMainCamera != nullptr)
                    {
                        QString CFWname;
                        indi_Client->getCFWSlotName(dpMainCamera, CFWname);
                        if (CFWname != "")
                        {
                            Logger::Log("LoadBindDeviceTypeList | get CFW Slot Name: " + CFWname.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                            isFilterOnCamera = true;
                            order += ":CFW:" + CFWname + " (on camera)" + ":" + systemdevicelist.system_devices[i].DriverIndiName + ":" + (systemdevicelist.system_devices[i].isBind ? "true" : "false");
                            int min, max, pos;
                            indi_Client->getCFWPosition(dpMainCamera, pos, min, max);
                            Logger::Log("LoadBindDeviceTypeList | getCFWPosition: " + std::to_string(min) + ", " + std::to_string(max) + ", " + std::to_string(pos), LogLevel::INFO, DeviceType::MAIN);
                            emit wsThread->sendMessageToClient("CFWPositionMax:" + QString::number(max));
                        }
                    }
                    else
                    {
                        Logger::Log("LoadBindDeviceTypeList | INDI main camera not ready (indi_Client or dpMainCamera is nullptr), skip CFW query", LogLevel::WARNING, DeviceType::MAIN);
                    }
                }
                else
                {
                    // SDK 模式下不在此处重复调用 IsCFWPlugged（QHY SDK 该调用可能阻塞 10~30s）。
                    // 这里改用连接阶段（AfterDeviceConnect）缓存结果，避免“加载绑定列表”链路长时间卡顿。
                    if (sdkMainCameraHandle != nullptr)
                    {
                        if (isFilterOnCamera)
                        {
                            const QString cfwDisplayName = sdkMainCameraId.isEmpty()
                                ? "CFW (on camera)"
                                : ("CFW (on camera) - " + sdkMainCameraId);

                            order += ":CFW:" + cfwDisplayName + ":" +
                                     QString::fromLatin1("indi_qhy_ccd") + ":" +
                                     (systemdevicelist.system_devices[i].isBind ? "true" : "false");

                            if (sdkMainCfwSlotsCached > 0)
                            {
                                emit wsThread->sendMessageToClient("CFWPositionMax:" + QString::number(sdkMainCfwSlotsCached));
                            }

                            // 若已有缓存名称列表，则直接推送一次，避免刷新后列表为空
                            const QString key = sdkCfwStorageKey(sdkMainCameraId);
                            const QString list = Tools::readCFWList(key);
                            if (!list.isEmpty())
                                emit wsThread->sendMessageToClient("getCFWList:" + list);
                        }
                        else
                        {
                            Logger::Log("LoadBindDeviceTypeList | MainCamera is in SDK mode, no cached CFW state",
                                        LogLevel::DEBUG, DeviceType::MAIN);
                        }
                    }
                    else
                    {
                        Logger::Log("LoadBindDeviceTypeList | MainCamera is in SDK mode but sdkMainCameraHandle is nullptr", LogLevel::WARNING, DeviceType::MAIN);
                    }
                }
            }
            else if (systemdevicelist.system_devices[i].Description == "Guider" && systemdevicelist.system_devices[i].isBind)
            {
                emit wsThread->sendMessageToClient("GuiderOffsetRange:" + QString::number(glGuiderOffsetMin) + ":" + QString::number(glGuiderOffsetMax) + ":" + QString::number(glGuiderOffsetValue));
                emit wsThread->sendMessageToClient("GuiderGainRange:" + QString::number(glGuiderGainMin) + ":" + QString::number(glGuiderGainMax) + ":" + QString::number(glGuiderGainValue));
            }
        }
    }
    Logger::Log("LoadBindDeviceTypeList | Bind Device Type List:" + order.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    emit wsThread->sendMessageToClient(order);
}

void MainWindow::loadBindDeviceList(MyClient *client)
{
    QString order = "BindDeviceList";
    QSet<QString> emittedKeys;
    auto appendDeviceToOrder = [&](const QString &type, const QString &name, int index) {
        const QString trimmedType = type.trimmed();
        const QString trimmedName = name.trimmed();
        if (trimmedType.isEmpty() || trimmedName.isEmpty())
            return;
        // 去重键必须包含 index，避免“同型号同名但不同设备”被误合并，
        // 导致设备绑定界面看不到可交换设备。
        const QString dedupKey = trimmedType + ":" + QString::number(index);
        if (emittedKeys.contains(dedupKey))
            return;
        emittedKeys.insert(dedupKey);
        order += ":" + trimmedType + ":" + trimmedName + ":" + QString::number(index);
    };

    // 先把“SDK 已打开设备”也同步给前端：
    // - SDK 模式下，这些设备不在 INDI 设备列表里，旧逻辑会导致前端刷新后“待分配设备列表”为空。
    // - 使用负 index（sdkUiIndexFromPoolIndex 返回 -(poolIndex+1)），避免与 INDI 的正 index 冲突。
    if (wsThread == nullptr)
    {
        Logger::Log("LoadBindDeviceList | wsThread is nullptr, skip", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    if (!g_sdkQhyCamIds.isEmpty())
    {
        for (int i = 0; i < g_sdkQhyCamIds.size(); ++i)
        {
            if (g_sdkQhyCamHandles.size() <= i) break;
            if (g_sdkQhyCamHandles[i] == nullptr) continue;
            if (g_sdkQhyCamIds[i].isEmpty()) continue;
            const int uiIdx = sdkUiIndexFromPoolIndex(i);
            // 直接在 BindDeviceList 中带上 Type，格式升级为三元组：Type:Name:Index
            appendDeviceToOrder("CCD", g_sdkQhyCamIds[i], uiIdx);
        }
    }

    // SDK 电调（Focuser）也需要在刷新时出现在“待分配列表”：
    // - SDK 电调不在 INDI 设备列表里，若不在此处同步，前端刷新后会看不到它
    // - 使用固定负 index，避免与 INDI 的 index 以及相机池 index 冲突
    if (systemdevicelist.system_devices.size() > 22 &&
        systemdevicelist.system_devices[22].isSDKConnect)
    {
        // 名称必须与 BindDeviceTypeList/ConnectSuccess 中的 DeviceName 保持一致，
        // 否则前端无法把该设备从“未分配列表”标记为已绑定，造成“已绑定但仍出现在未分配列表/命名变化”。
        QString name;
        const QString saved = systemdevicelist.system_devices[22].DeviceIndiName;
        if (!saved.isEmpty())
            name = saved;
        else if (!sdkFocuserPort.isEmpty())
            name = sdkFocuserPort;
        // 只使用真实串口名，避免占位符导致前端出现“SDK_Focuser”
        if (!name.isEmpty())
        {
            appendDeviceToOrder("Focuser", name, SDK_FOCUSER_UI_INDEX);
        }
        else
        {
            Logger::Log("LoadBindDeviceList | skip SDK Focuser: no valid port/name", LogLevel::WARNING, DeviceType::FOCUSER);
        }
    }

    // 再追加 INDI 已连接设备（保持原有协议）
    if (client == nullptr)
    {
        Logger::Log("LoadBindDeviceList | client is nullptr", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(order);
        return;
    }

    int deviceCount = client->GetDeviceCount();
    if (deviceCount <= 0)
    {
        Logger::Log("LoadBindDeviceList | no devices in client list", LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(order);
        return;
    }

    for (int i = 0; i < deviceCount; i++)
    {
        INDI::BaseDevice *device = client->GetDeviceFromList(i);
        if (device == nullptr)
        {
            Logger::Log("LoadBindDeviceList | Device at index " + std::to_string(i) + " is nullptr", LogLevel::WARNING, DeviceType::MAIN);
            continue;
        }

        const char *name = device->getDeviceName();
        if (!name)
        {
            Logger::Log("LoadBindDeviceList | Device at index " + std::to_string(i) + " has null name pointer", LogLevel::WARNING, DeviceType::MAIN);
            continue;
        }

        QString qName = QString::fromUtf8(name);
        if (device->isConnected() && !qName.isEmpty())
        {
            // 类型推断：优先使用 INDI interface 位
            QString type = "Device";
            const uint32_t iface = device->getDriverInterface();
            if (iface & INDI::BaseDevice::CCD_INTERFACE) type = "CCD";
            else if (iface & INDI::BaseDevice::FILTER_INTERFACE) type = "CFW";
            else if (iface & INDI::BaseDevice::TELESCOPE_INTERFACE) type = "Mount";
            else if (iface & INDI::BaseDevice::FOCUSER_INTERFACE) type = "Focuser";
            // 待分配设备列表中暂时不展示 Mount（望远镜）项
            if (type == "Mount")
                continue;
            // BindDeviceList 升级为：Type:Name:Index
            appendDeviceToOrder(type, qName, i);
        }
    }

    Logger::Log("LoadBindDeviceList | Bind Device List:" + order.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    emit wsThread->sendMessageToClient(order);
}

void MainWindow::loadSDKVersionAndUSBSerialPath()
{
    QString order = "SDKVersionAndUSBSerialPath";
    // 某些连接/重连时序下，wsThread/indi_Client 可能暂未就绪，避免空指针段错误
    if (!wsThread)
    {
        Logger::Log("LoadSDKVersionAndUSBSerialPath | wsThread is nullptr, skip", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }

    // 主相机：支持 SDK / INDI 两种模式
    if (isMainCameraConnected())
    {
        QString sdkVersion = "null";

        // SDK 模式：不要走 INDI 的 dpMainCamera（很可能为空），直接通过 SDK Driver 获取
        if (isMainCameraSDK() && sdkMainCameraHandle != nullptr)
        {
            SdkCommand verCmd;
            verCmd.type = SdkCommandType::Custom;
            verCmd.name = "GetSdkVersion";
            verCmd.payload = std::any();
            // 直接通过设备句柄调用，无需指定驱动名称
            SdkResult verRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, verCmd);
            if (verRes.success)
            {
                try
                {
                    std::string version = std::any_cast<std::string>(verRes.payload);
                    sdkVersion = QString::fromStdString(version);
                }
                catch (const std::bad_any_cast &)
                {
                    Logger::Log("LoadSDKVersionAndUSBSerialPath | bad_any_cast for SDK version payload", LogLevel::WARNING, DeviceType::MAIN);
                }
            }
        }
        else
        {
            // INDI 模式
            if (indi_Client != nullptr && dpMainCamera != nullptr)
            {
                indi_Client->getCCDSDKVersion(dpMainCamera, sdkVersion);
            }
        }

        order += ":MainCamera:" + sdkVersion + ":null";
    }

    // 其余设备目前仅走 INDI：增加空指针保护
    if (indi_Client != nullptr && dpGuider != NULL)
    {
        QString sdkVersion = "null";
        indi_Client->getCCDSDKVersion(dpGuider, sdkVersion);
        order += ":Guider:" + sdkVersion + ":null";
    }
    if (indi_Client != nullptr && dpFocuser != NULL)
    {
        QString sdkVersion = "null";
        indi_Client->getFocuserSDKVersion(dpFocuser, sdkVersion);
        QString DevicePort = "null";
        indi_Client->getDevicePort(dpFocuser, DevicePort);
        order += ":Focuser:" + sdkVersion + ":" + DevicePort;
    }
    // if (dpCFW != NULL)
    // {
    //     QString sdkVersion;
    //     indi_Client->getSDKVersion(dpCFW, sdkVersion);
    //     QString usbSerialPath;
    //     indi_Client->getUSBSerialPath(dpCFW, usbSerialPath);
    //     order += ":CFW:" + sdkVersion + ":" + usbSerialPath;
    // }
    if (indi_Client != nullptr && dpMount != NULL)
    {
        QString sdkVersion;
        indi_Client->getMountInfo(dpMount, sdkVersion);
        QString usbSerialPath;
        indi_Client->getDevicePort(dpMount, usbSerialPath);
        order += ":Mount:" + sdkVersion + ":" + usbSerialPath;
    }
    emit wsThread->sendMessageToClient(order);
    Logger::Log("LoadSDKVersionAndUSBSerialPath | SDKVersionAndUSBSerialPath:" + order.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
}

QStringList MainWindow::getConnectedSerialPorts()
{
    QStringList activeSerialPortNames;
    const auto infos = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : infos)
    {
         QString portPath = info.systemLocation();
        QFileInfo fi(portPath);
        QString ttyName = fi.fileName(); // 例如 ttyUSB0, ttyACM0, ttyS0

        // 过滤掉传统的 /dev/ttyS* 串口（这些通常是虚拟的或未使用的串口）
        // 只保留 USB 串口（ttyUSB*, ttyACM*）和真实连接的串口
        if (ttyName.startsWith("ttyS"))
        {
            // 对于 ttyS* 串口，只有当它有 /dev/serial/by-id 链接时才保留
            // 因为只有真实连接的 USB 串口设备才会有这个链接
            QStringList byIdLinks = getByIdLinksForTty(ttyName);
            if (byIdLinks.isEmpty())
            {
                // 没有 by-id 链接的 ttyS* 串口，很可能是虚拟的，跳过
                continue;
            }
        }

        // 使用系统路径，便于直接设置到 INDI 设备端口，例如 /dev/ttyUSB0
        // 不再强制尝试打开端口，以免过滤掉权限/占用导致暂时无法打开但仍可被用户选择的端口
        activeSerialPortNames.append(portPath);
    }
    return activeSerialPortNames;
}

QString MainWindow::resolveSerialPort(const QString &symbolicLink)
{
    QFileInfo fileInfo(symbolicLink);
    if (fileInfo.isSymLink())
    {
        QString target = fileInfo.symLinkTarget();
        Logger::Log("ResolveSerialPort | real port path:" + target.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        return target;
    }
    else
    {
        Logger::Log("ResolveSerialPort | provided path is not a symbolic link", LogLevel::WARNING, DeviceType::MAIN);
        return QString();
    }
}

void MainWindow::sendSerialPortOptions(const QString &driverType)
{
    if (!wsThread)
        return;

    // 仅支持 Mount / Focuser 两类串口设备
    if (driverType != "Mount" && driverType != "Focuser")
        return;

    // 当前可用串口路径列表（全部是真实存在的串口节点）
    QStringList ports = getConnectedSerialPorts();

    // 当前设备正在使用的串口（若已连接），或前端最近一次选择的覆盖串口
    QString currentPort;
    if (driverType == "Mount")
    {
        if (dpMount != nullptr)
        {
            indi_Client->getDevicePort(dpMount, currentPort);
        }
        if (currentPort.isEmpty())
        {
            currentPort = mountSerialPortOverride;
        }
    }
    else if (driverType == "Focuser")
    {
        if (dpFocuser != nullptr)
        {
            indi_Client->getDevicePort(dpFocuser, currentPort);
        }
        if (currentPort.isEmpty())
        {
            currentPort = focuserSerialPortOverride;
        }
    }

    // 组装带“真实路径 -> 友好名称(by-id)”的项：
    // 每一项格式为：<portPath>-><displayName>，前端再解析
    QString payload = "SerialPortOptions:" + driverType + ":" + currentPort;
    for (const QString &p : ports)
    {
        QString displayName = p;
        QFileInfo fi(p);
        QString ttyName = fi.fileName(); // 例如 ttyUSB0

        // 若能找到 /dev/serial/by-id 的符号链接，则使用其文件名作为显示名
        QStringList byIdLinks = getByIdLinksForTty(ttyName);
        if (!byIdLinks.isEmpty())
        {
            QFileInfo linkInfo(byIdLinks.first());
            displayName = linkInfo.fileName(); // 只显示 by-id 的名字，更易识别设备
        }

        payload += ":" + p + "->" + displayName;
    }

    Logger::Log("sendSerialPortOptions | " + payload.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
    emit wsThread->sendMessageToClient(payload);
}

QStringList MainWindow::findLinkToTtyDevice(const QString &directoryPath, const QString &ttyDevice)
{
    QString targetDevice = "/dev/" + ttyDevice; // 构建完整的设备路径
    QStringList foundLinks;

    // 使用 QDirIterator 递归遍历目录和子目录
    QDirIterator it(directoryPath, QDir::Files | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        it.next();
        QFileInfo fileInfo = it.fileInfo();
        if (fileInfo.isSymLink())
        {
            QString target = fileInfo.symLinkTarget();
            // 如果符号链接是相对路径，需要将其转换为绝对路径
            if (QDir::isRelativePath(target))
            {
                target = fileInfo.absoluteDir().absoluteFilePath(target);
            }
            // 检查符号链接的目标是否是指定的 tty 设备
            if (target == targetDevice)
            {
                foundLinks.append(fileInfo.absoluteFilePath());
                Logger::Log("FindLinkToTtyDevice | found link:" + fileInfo.absoluteFilePath().toStdString() + " -> " + target.toStdString(), LogLevel::INFO, DeviceType::MAIN);
            }
        }
    }
    // 返回找到的所有符号链接
    return foundLinks;
}

QStringList MainWindow::getByIdLinksForTty(const QString &ttyDevice)
{
    QStringList results;
    QString baseDir = "/dev/serial/by-id";
    QDir dir(baseDir);
    if (!dir.exists())
    {
        return results;
    }

    QFileInfoList entryList = dir.entryInfoList(QDir::Files | QDir::System | QDir::NoDotAndDotDot);
    QString targetDevice = "/dev/" + ttyDevice;
    for (const QFileInfo &entry : entryList)
    {
        if (!entry.isSymLink())
        {
            continue;
        }
        QString target = entry.symLinkTarget();
        if (QDir::isRelativePath(target))
        {
            // 归一化相对路径为绝对路径
            target = entry.absoluteDir().absoluteFilePath(target);
        }
        QString normalizedTarget = QDir::cleanPath(target);
        if (normalizedTarget == targetDevice)
        {
            results.append(entry.absoluteFilePath());
            Logger::Log("getByIdLinksForTty | found by-id link:" + entry.absoluteFilePath().toStdString() + " -> " + normalizedTarget.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }
    }
    return results;
}

bool MainWindow::isByIdLinkForDriverType(const QString &symlinkPath, const QString &driverType)
{
    QFileInfo fi(symlinkPath);
    QString nameLower = fi.fileName().toLower();
    return scoreByIdLinkForType(nameLower, driverType) > 0;
}

QString MainWindow::selectBestByIdLink(const QStringList &links, const QString &driverType)
{
    int bestScore = -1;
    QString best;
    for (const QString &link : links)
    {
        QFileInfo fi(link);
        QString nameLower = fi.fileName().toLower();
        int s = scoreByIdLinkForType(nameLower, driverType);
        if (s > bestScore)
        {
            bestScore = s;
            best = link;
        }
    }
    return best;
}

void MainWindow::onParseInfoEmitted(const QString &message)
{
    emit wsThread->sendMessageToClient("ParseInfoEmitted:" + message);
}

void MainWindow::disconnectDevice(const QString &deviceName, const QString &description)
{
    for (int i = 0; i < indi_Client->GetDeviceCount(); i++)
    {
        if (indi_Client->GetDeviceFromList(i)->getDeviceName() == deviceName)
        {
            indi_Client->disconnectDevice(deviceName.toStdString().c_str());
            int num = 0;
            bool disconnectSuccess = true;

            Logger::Log(deviceName.toStdString() + " disconnected successfully.", LogLevel::INFO, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("DisconnectDriverSuccess:" + description);
            emit wsThread->sendMessageToClient("deleteDeviceAllocationList:" + deviceName);

            // 若为赤道仪或电调，在断开后提示前端弹出串口选择 UI，方便下次连接前重新匹配
            if (description == "Mount" || description == "Focuser")
            {
                sendSerialPortOptions(description);
                emit wsThread->sendMessageToClient("RequestSerialPortSelection:" + description);
            }
            break;
        }
    }
}

void MainWindow::disconnectDriver(QString Driver)
{
    Logger::Log("Starting to disconnect driver: " + Driver.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    // 先收集要断开的设备（避免 DisconnectDevice 修改 systemdevicelist 导致迭代器/引用风险）
    QVector<QPair<QString, QString>> toDisconnect;
    for (const auto &dev : systemdevicelist.system_devices)
    {
        if (!dev.Description.isEmpty() && dev.DriverIndiName == Driver && dev.isConnect)
        {
            toDisconnect.push_back(qMakePair(dev.DeviceIndiName, dev.Description));
        }
    }

    for (const auto &item : toDisconnect)
    {
        const QString &devName = item.first;
        const QString &devType = item.second;

        // 断开前中止主相机曝光，避免断开过程中卡住
        if (devType == "MainCamera" && glMainCameraStatu == "Exposuring")
        {
            abortMainCameraCapture();
        }

        // 统一走 DisconnectDevice：它同时覆盖 INDI 与 SDK 模式清理
        DisconnectDevice(indi_Client, devName, devType);

        // 清理 ConnectedDevices 中对应项
        for (auto it = ConnectedDevices.begin(); it != ConnectedDevices.end();)
        {
            if (it->DeviceType == devType)
            {
                it = ConnectedDevices.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // 若这是一个 SDK 相关驱动（systemdevicelist 中该驱动对应的槽位被标记为 isSDKConnect），
    // 则需要同步清理 SDK 资源，避免“断开驱动后 SDK 仍占用/残留线程访问”。
    //
    // 注意：cleanupQhySdkPoolAndResource 的 deviceType 仅支持：
    // - "CameraPool"（关闭所有相机句柄 + ReleaseSdkResource）
    // - "MainCamera"（仅关闭主相机句柄，不释放全局资源）
    // - "Focuser"（关闭电调句柄）
    // - "All"
    //
    // 断开驱动场景应优先清理 CameraPool（因为同一驱动可能同时被 MainCamera/Guider 共用）。
    bool needsCameraPool = false;
    bool needsFocuser = false;
    bool sdkRelated = false;
    for (const auto &dev : systemdevicelist.system_devices)
    {
        if (!dev.isSDKConnect)
            continue;
        if (dev.DriverIndiName.isEmpty() || dev.DriverIndiName != Driver)
            continue;

        sdkRelated = true;
        if (dev.Description == "Focuser")
            needsFocuser = true;
        else if (dev.Description == "MainCamera" || dev.Description == "Guider" || dev.Description == "PoleCamera")
            needsCameraPool = true;
    }

    if (sdkRelated || Driver.contains("SDK", Qt::CaseInsensitive))
    {
        if (needsFocuser)
            cleanupQhySdkPoolAndResource("disconnectDriver:" + Driver, "Focuser");
        if (needsCameraPool)
            cleanupQhySdkPoolAndResource("disconnectDriver:" + Driver, "CameraPool");
    }

    Tools::stopIndiDriver(Driver);
    int index = ConnectDriverList.indexOf(Driver);
    if (index != -1)
    {                                      // 如果找到了
        ConnectDriverList.removeAt(index); // 从列表中删除
        Logger::Log("Driver removed successfully: " + Driver.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        Logger::Log("Driver not found in list: " + Driver.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
    }

    Logger::Log("Driver disconnected: " + Driver.toStdString(), LogLevel::INFO, DeviceType::MAIN);
}
