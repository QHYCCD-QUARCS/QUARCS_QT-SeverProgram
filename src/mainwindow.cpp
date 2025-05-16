#include "mainwindow.h"

INDI::BaseDevice *dpMount = nullptr, *dpGuider = nullptr, *dpPoleScope = nullptr;
INDI::BaseDevice *dpMainCamera = nullptr, *dpFocuser = nullptr, *dpCFW = nullptr;

DriversList drivers_list;
std::vector<DevGroup> dev_groups;
std::vector<Device> devices;

DriversListNew drivers_list_new;

SystemDevice systemdevice;
SystemDeviceList systemdevicelist;

// QUrl websocketUrl(QStringLiteral("ws://192.168.2.31:8600"));
QUrl websocketUrl;

// 定义静态成员变量 instance
MainWindow *MainWindow::instance = nullptr;

std::string MainWindow::getBuildDate() { // 编译时的日期
    static const std::map<std::string, std::string> monthMap = {
        {"Jan", "01"}, {"Feb", "02"}, {"Mar", "03"}, {"Apr", "04"},
        {"May", "05"}, {"Jun", "06"}, {"Jul", "07"}, {"Aug", "08"},
        {"Sep", "09"}, {"Oct", "10"}, {"Nov", "11"}, {"Dec", "12"}
    };

    std::string date = __DATE__;
    std::stringstream dateStream(date);
    std::string month, day, year;
    dateStream >> month >> day >> year;

    return year + monthMap.at(month) + (day.size() == 1 ? "0" + day : day);
}

MainWindow::MainWindow(QObject *parent) : QObject(parent)
{
    system_timer = new QTimer(this);  // 用于对系统的监测
    connect(system_timer, &QTimer::timeout, this, &MainWindow::updateCPUInfo);
    system_timer->start(3000);

    Logger::Initialize();
    getHostAddress();

    wsThread = new WebSocketThread(websocketUrl);
    connect(wsThread, &WebSocketThread::receivedMessage, this, &MainWindow::onMessageReceived);
    wsThread->start();
    Logger::wsThread = wsThread;

    // 记住当前实例
    instance = this;

    // 安装自定义的消息处理器
    // qInstallMessageHandler(customMessageHandler);

    InitPHD2();

    initINDIServer();
    initINDIClient();

    initGPIO();

    readDriversListFromFiles("/usr/share/indi/drivers.xml", drivers_list, dev_groups, devices);

    Tools::InitSystemDeviceList();
    Tools::initSystemDeviceList(systemdevicelist);
    Tools::makeConfigFile();
    Tools::makeImageFolder();
    connect(Tools::getInstance(), &Tools::parseInfoEmitted, this, &MainWindow::onParseInfoEmitted);

    m_thread = new QThread;
    m_threadTimer = new QTimer;
    m_threadTimer->setInterval(10);
    m_threadTimer->moveToThread(m_thread);
    connect(m_thread, &QThread::started, m_threadTimer, qOverload<>(&QTimer::start));
    connect(m_threadTimer, &QTimer::timeout, this, &MainWindow::onTimeout);
    connect(m_thread, &QThread::finished, m_threadTimer, &QTimer::stop);
    connect(m_thread, &QThread::destroyed, m_threadTimer, &QTimer::deleteLater);
    m_thread->start();

    PHDControlGuide_thread = new QThread;
    PHDControlGuide_threadTimer = new QTimer;
    PHDControlGuide_threadTimer->setInterval(5);
    PHDControlGuide_threadTimer->moveToThread(PHDControlGuide_thread);
    connect(PHDControlGuide_thread, &QThread::started, PHDControlGuide_threadTimer, qOverload<>(&QTimer::start));
    connect(PHDControlGuide_threadTimer, &QTimer::timeout, this, &MainWindow::onPHDControlGuideTimeout);
    connect(PHDControlGuide_thread, &QThread::finished, PHDControlGuide_threadTimer, &QTimer::stop);
    connect(PHDControlGuide_thread, &QThread::destroyed, PHDControlGuide_threadTimer, &QTimer::deleteLater);
    PHDControlGuide_thread->start();
    // getConnectedSerialPorts();
    
    // 电调控制初始化
    focusMoveTimer = new QTimer(this);
    connect(focusMoveTimer, &QTimer::timeout, this, &MainWindow::HandleFocuserMovementDataPeriodically);

    emit wsThread->sendMessageToClient("ServerInitSuccess");

    tianwen = new TianWen();
    TianWen::wsThread = wsThread;
}

MainWindow::~MainWindow()
{
    system("pkill indiserver");
    system("rm -f /tmp/myFIFO");

    wsThread->quit();
    wsThread->wait();
    delete wsThread;

    // 清理静态实例
    instance = nullptr;
}

void MainWindow::getHostAddress()
{
    int retryCount = 0;
    const int maxRetries = 20;
    const int waitTime = 5000; // 5秒

    while (retryCount < maxRetries)
    {
        QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
        bool found = false;

        foreach (const QNetworkInterface &interface, interfaces) {
            // 排除回环接口和非活动接口
            if (interface.flags() & QNetworkInterface::IsLoopBack || !(interface.flags() & QNetworkInterface::IsUp))
                continue;

        QList<QNetworkAddressEntry> addresses = interface.addressEntries();
        foreach (const QNetworkAddressEntry &address, addresses) {
            if (address.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                QString localIpAddress = address.ip().toString();
                Logger::Log("Local IP Address:" + localIpAddress.toStdString(), LogLevel::INFO, DeviceType::MAIN);

                if (!localIpAddress.isEmpty()) {
                    QUrl getUrl(QStringLiteral("ws://%1:8600").arg(localIpAddress));
                    Logger::Log("WebSocket URL:" + getUrl.toString().toStdString(), LogLevel::INFO, DeviceType::MAIN);
                    websocketUrl = getUrl;
                    found = true;
                    break;
                } else {
                    Logger::Log("Failed to get local IP address.", LogLevel::WARNING, DeviceType::MAIN);
                }
                }
            }
            if (found) break;
        }

        if (found) break;

        retryCount++;
        QThread::sleep(waitTime / 1000); // 等待5秒
    }

    if (retryCount == maxRetries) {
        qCritical() << "Failed to detect any network interfaces after" << maxRetries << "attempts.";
    }
}

void MainWindow::onMessageReceived(const QString &message)
{
    // 处理接收到的消息
    Logger::Log("Received message in MainWindow:" + message.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
    // 分割消息
    QStringList parts = message.split(':');

    if (parts.size() >= 2 && parts[0].trimmed() == "ConfirmIndiDriver")
    {
        if (parts.size() == 2) {
            Logger::Log("ConfirmIndiDriver:" + parts[1].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
            QString driverName = parts[1].trimmed();
            indi_Driver_Confirm(driverName,"9600");
        } else if (parts.size() == 3) {
            Logger::Log("ConfirmIndiDriver:" + parts[1].trimmed().toStdString() + ":" + parts[2].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
            QString driverName = parts[1].trimmed();
            QString baudRate = parts[2].trimmed();
            indi_Driver_Confirm(driverName, baudRate);
        }
    }
    else if (message == "ClearIndiDriver")
    {
        Logger::Log("ClearIndiDriver", LogLevel::DEBUG, DeviceType::MAIN);
        indi_Driver_Clear();
    }
    else if (parts.size() == 3 && parts[0].trimmed() == "ConfirmIndiDevice")
    {
        Logger::Log("ConfirmIndiDevice:" + parts[1].trimmed().toStdString() + ":" + parts[2].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        QString deviceName = parts[1].trimmed();
        QString driverName = parts[2].trimmed();
        // connectDevice(x);
        indi_Device_Confirm(deviceName, driverName);
    }
    else if (parts.size() == 3 && parts[0].trimmed() == "BindingDevice")
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
    else if (parts.size() == 3 && parts[0].trimmed() == "SelectIndiDriver")
    {
        Logger::Log("SelectIndiDriver:" + parts[1].trimmed().toStdString() + ":" + parts[2].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        QString Group = parts[1].trimmed();
        int ListNum = parts[2].trimmed().toInt();
        printDevGroups2(drivers_list, ListNum, Group);
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "takeExposure")
    {
        Logger::Log("takeExposure:" + parts[1].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::CAMERA);
        Logger::Log("Accept takeExposure order ,set ExpTime is " + parts[1].trimmed().toStdString() + " ms", LogLevel::DEBUG, DeviceType::CAMERA);
        int ExpTime = parts[1].trimmed().toInt();
        INDI_Capture(ExpTime);
        glExpTime = ExpTime;
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "focusSpeed")
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
        QString LR = parts[1].trimmed();
        // int Steps = parts[2].trimmed().toInt();
        if(LR == "Left")
        {
            FocuserControlMove(true);
            // FocusMoveAndCalHFR(true,Steps);
        }
        else if(LR == "Right")
        {
            // FocusMoveAndCalHFR(false,Steps);
            FocuserControlMove(false);
        }
        // else if(LR == "Target")
        // {
        //     FocusGotoAndCalFWHM(Steps);
        // }
    }else if (parts.size() == 2 && parts[0].trimmed() == "getFocuserMoveState"){
        if(isFocusMoveDone && parts[1].trimmed() == "true")
        {
            focusMoveEndTime = 2;
        }
    }
    else if (parts[0].trimmed() == "focusMoveStop")
    {
        FocuserControlStop();
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "SyncFocuserStep")
    {
        Logger::Log("SyncFocuserStep:" + parts[1].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        int Steps = parts[1].trimmed().toInt();
        if(dpFocuser != NULL) {
            indi_Client->syncFocuserPosition(dpFocuser, Steps);
            sleep(1);
            CurrentPosition = FocuserControl_getPosition();
            Logger::Log("Focuser Current Position: " + std::to_string(CurrentPosition), LogLevel::DEBUG, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
        }
    }
    // else if (parts.size() == 5 && parts[0].trimmed() == "RedBox")
    // {
    //     Logger::Log("RedBox:" + parts[1].trimmed().toStdString() + ":" + parts[2].trimmed().toStdString() + ":" + parts[3].trimmed().toStdString() + ":" + parts[4].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
    //     int x = parts[1].trimmed().toInt();
    //     int y = parts[2].trimmed().toInt();
    //     int width = parts[3].trimmed().toInt();
    //     int height = parts[4].trimmed().toInt();
    //     glROI_x = x;
    //     glROI_y = y;
    //     CaptureViewWidth = width;
    //     CaptureViewHeight = height;
    //     Logger::Log("RedBox:" + std::to_string(glROI_x) + ":" + std::to_string(glROI_y) + ":" + std::to_string(CaptureViewWidth) + ":" + std::to_string(CaptureViewHeight), LogLevel::DEBUG, DeviceType::MAIN);
    // }
    else if (parts.size() == 3 && parts[0].trimmed() == "setROIPosition")
    {
        Logger::Log("setROIPosition:" + parts[1].trimmed().toStdString() + "*" + parts[2].trimmed().toStdString() , LogLevel::INFO, DeviceType::MAIN);
        glROI_x = parts[1].trimmed().toDouble();
        glROI_y = parts[2].trimmed().toDouble();
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "RedBoxSizeChange")
    {
        Logger::Log("RedBoxSizeChange:" + parts[1].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        BoxSideLength = parts[1].trimmed().toInt();
        Tools::saveParameter("MainCamera","BoxSideLength",parts[1].trimmed());
        Logger::Log("BoxSideLength:" + std::to_string(BoxSideLength), LogLevel::DEBUG, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));
    }
    else if (message == "AutoFocus")
    {
        // Logger::Log("AutoFocus", LogLevel::INFO, DeviceType::MAIN);
        // AutoFocus();
        isAutoFocus = true;
        autoFocusStep = 0;

    }
    else if (message == "StopAutoFocus")
    {
        Logger::Log("StopAutoFocus", LogLevel::DEBUG, DeviceType::MAIN);
        StopAutoFocus = true;
    }
    else if (message == "abortExposure")
    {
        Logger::Log("abortExposure", LogLevel::DEBUG, DeviceType::CAMERA);
        INDI_AbortCapture();
    }
    else if (message == "connectAllDevice")
    {
        Logger::Log("connectAllDevice", LogLevel::DEBUG, DeviceType::MAIN);
        // DeviceConnect();
        ConnectAllDeviceOnce();
    }
    else if (message == "autoConnectAllDevice")
    {
        Logger::Log("autoConnectAllDevice", LogLevel::DEBUG, DeviceType::MAIN);
        // DeviceConnect();
        AutoConnectAllDevice();
    }
    else if (message == "CS")
    {
        // QString Dev = connectIndiServer();
        // websocket->messageSend("AddDevice:"+Dev);
    }
    else if (message == "disconnectAllDevice")
    {
        Logger::Log("disconnectAllDevice ...", LogLevel::DEBUG, DeviceType::MAIN);
        disconnectIndiServer(indi_Client);
        Logger::Log("disconnectIndiServer ...", LogLevel::DEBUG, DeviceType::MAIN);
        ClearSystemDeviceList();
        Logger::Log("ClearSystemDeviceList ...", LogLevel::DEBUG, DeviceType::MAIN);
        clearConnectedDevices();
        Logger::Log("clearConnectedDevices ...", LogLevel::DEBUG, DeviceType::MAIN);
        // 重启indi服务器
        initINDIServer();
        Logger::Log("initINDIServer ...", LogLevel::DEBUG, DeviceType::MAIN);
        initINDIClient();
        
        Logger::Log("initINDIClient ...", LogLevel::DEBUG, DeviceType::MAIN);
        Tools::InitSystemDeviceList();
        Logger::Log("Tools::InitSystemDeviceList ...", LogLevel::DEBUG, DeviceType::MAIN);
        Tools::initSystemDeviceList(systemdevicelist);
        Logger::Log("disconnectAllDevice end!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (message == "MountMoveWest")
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
    else if (message == "MountPark")
    {
        Logger::Log("MountPark ...", LogLevel::DEBUG, DeviceType::MOUNT);
        if (dpMount != NULL)
        {
            bool isPark = TelescopeControl_Park();
            if(isPark)
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
            if(isTrack)
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
            indi_Client->setTelescopeHomeInit(dpMount,"SLEWHOME");
        }
    }
    else if (message == "MountSYNC")
    {
        Logger::Log("MountSYNC ...", LogLevel::DEBUG, DeviceType::MOUNT);
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeHomeInit(dpMount,"SYNCHOME");
        }
    }

    else if (message == "MountSpeedSwitch")
    {
        Logger::Log("MountSpeedSwitch ", LogLevel::DEBUG, DeviceType::MOUNT);
        if (dpMount != NULL)
        {
            int currentSpeed;
            indi_Client->getTelescopeSlewRate(dpMount,currentSpeed);
            Logger::Log("Current Speed:" + std::to_string(currentSpeed) + " Total Speed:" + std::to_string(glTelescopeTotalSlewRate), LogLevel::DEBUG, DeviceType::MOUNT);

            if(currentSpeed == glTelescopeTotalSlewRate) {
                indi_Client->setTelescopeSlewRate(dpMount,1);
            } else {
                indi_Client->setTelescopeSlewRate(dpMount,currentSpeed+1);
                Logger::Log("Set Speed to:" + std::to_string(currentSpeed), LogLevel::DEBUG, DeviceType::MOUNT);
            }

            int ChangedSpeed;
            indi_Client->getTelescopeSlewRate(dpMount,ChangedSpeed);
            Logger::Log("Changed Speed:" + std::to_string(ChangedSpeed), LogLevel::DEBUG, DeviceType::MOUNT);
            emit wsThread->sendMessageToClient("MountSetSpeedSuccess:" + QString::number(ChangedSpeed));
        }
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "ImageGainR")
    {
        ImageGainR = parts[1].trimmed().toDouble();
        Logger::Log("GainR is set to " + std::to_string(ImageGainR), LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveParameter("MainCamera","ImageGainR",parts[1].trimmed());
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "ImageGainB")
    {
        ImageGainB = parts[1].trimmed().toDouble();
        Logger::Log("GainB is set to " + std::to_string(ImageGainB), LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveParameter("MainCamera","ImageGainB",parts[1].trimmed());
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "ImageOffset")
    {
        ImageOffset = parts[1].trimmed().toDouble();
        Logger::Log("ImageOffset is set to " + std::to_string(ImageOffset), LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveParameter("MainCamera","ImageOffset",parts[1].trimmed());
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "ImageCFA")
    {
        MainCameraCFA = parts[1].trimmed();
        Logger::Log("ImageCFA is set to " + MainCameraCFA.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveParameter("MainCamera","ImageCFA",parts[1].trimmed());
    }

    else if (parts[0].trimmed() == "ScheduleTabelData")
    {
        ScheduleTabelData(message);
    }

    else if (parts.size() == 4 && parts[0].trimmed() == "MountGoto")
    {
        if (dpMount != NULL){
            Logger::Log("MountGoto ...", LogLevel::DEBUG, DeviceType::MOUNT);
            QStringList RaDecList = message.split(',');
            QStringList RaList = RaDecList[0].split(':');
            QStringList DecList = RaDecList[1].split(':');

            double Ra_Rad, Dec_Rad;
            Ra_Rad  = RaList[2].trimmed().toDouble();
            Dec_Rad = DecList[1].trimmed().toDouble();

            Logger::Log("Mount Goto RaDec(Rad):" + std::to_string(Ra_Rad) + "," + std::to_string(Dec_Rad), LogLevel::DEBUG, DeviceType::MOUNT);

            double Ra_Hour, Dec_Degree;
            Ra_Hour  = Tools::RadToHour(Ra_Rad);
            Dec_Degree = Tools::RadToDegree(Dec_Rad);

            MountGoto(Ra_Hour, Dec_Degree);
            Logger::Log("MountGoto finish!", LogLevel::DEBUG, DeviceType::MOUNT);
        }else{

        }

    }

    else if (message == "StopSchedule")
    {
        Logger::Log("StopSchedule !", LogLevel::DEBUG, DeviceType::MAIN);
        StopSchedule = true;
    }

    else if (message == "CaptureImageSave")
    {
        Logger::Log("CaptureImageSave ...", LogLevel::DEBUG, DeviceType::MAIN);
        CaptureImageSave();
        Logger::Log("CaptureImageSave finish!", LogLevel::DEBUG, DeviceType::MAIN);
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

    else if (message == "getStagingImage")
    {
        Logger::Log("Reload Main Camera Image.", LogLevel::DEBUG, DeviceType::MAIN);
        getStagingImage();
        Logger::Log("Reload Main Camera Image finish!", LogLevel::DEBUG, DeviceType::MAIN);
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

    else if (message == "getStagingGuiderData")
    {
        Logger::Log("getStagingGuiderData ...", LogLevel::DEBUG, DeviceType::MAIN);
        getStagingGuiderData();
        Logger::Log("getStagingGuiderData finish!", LogLevel::DEBUG, DeviceType::MAIN);
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
        if (Tools::readExpTimeList() != QString()){
            emit wsThread->sendMessageToClient(Tools::readExpTimeList());
        }
        Logger::Log("getExpTimeList finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (message == "getCaptureStatus")
    {
        Logger::Log("getCaptureStatus ...", LogLevel::DEBUG, DeviceType::MAIN);
        if (glMainCameraStatu == "Exposuring")
        {
            if (isFocusLoopShooting) {
                emit wsThread->sendMessageToClient("CameraInExposuring:False");
            } else {
                if (isFocusLoopShooting) {
                    emit wsThread->sendMessageToClient("CameraInExposuring:False");
                } else {
                    emit wsThread->sendMessageToClient("CameraInExposuring:True");
                }
            }
        }
        Logger::Log("getCaptureStatus finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (parts[0].trimmed() == "SetCFWPosition" && parts.size() == 2)
    {
        Logger::Log("SetCFWPosition ...", LogLevel::DEBUG, DeviceType::CFW);
        int pos = parts[1].trimmed().toInt();

        if(isFilterOnCamera) {
            if (dpMainCamera != NULL)
            {
                indi_Client->setCFWPosition(dpMainCamera, pos);
                emit wsThread->sendMessageToClient("SetCFWPositionSuccess:" + QString::number(pos));
                Logger::Log("Set CFW Position to" + std::to_string(pos) + "Success!!!", LogLevel::DEBUG, DeviceType::CFW);
            }
        } else {
            if (dpCFW != NULL)
            {
                indi_Client->setCFWPosition(dpCFW, pos);
                emit wsThread->sendMessageToClient("SetCFWPositionSuccess:" + QString::number(pos));
                Logger::Log("Set CFW Position to" + std::to_string(pos) + "Success!!!", LogLevel::DEBUG, DeviceType::CFW);
            }
        }
    }

    else if (parts[0].trimmed() == "CFWList")
    {
        Logger::Log("Save CFWList ...", LogLevel::DEBUG, DeviceType::CFW);
        if(isFilterOnCamera) {
            if (dpMainCamera != NULL)
            {
                QString CFWname;
                indi_Client->getCFWSlotName(dpMainCamera, CFWname);
                Tools::saveCFWList(CFWname, parts[1]);
            }
        } else {
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
        if(isFilterOnCamera) {
            if (dpMainCamera != NULL)
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
        } else {
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

    else if (message == "ClearCalibrationData")
    {
        ClearCalibrationData = true;
        Logger::Log("Clear polar alignment calibration data", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (parts[0].trimmed() == "GuiderSwitch" && parts.size() == 2)
    {
        Logger::Log("GuiderSwitch ...", LogLevel::INFO, DeviceType::GUIDER);
        if (isGuiding && parts[1].trimmed() == "false"){
            isGuiding = false;
            call_phd_StopLooping();
            emit wsThread->sendMessageToClient("GuiderSwitchStatus:false");
            isGuiderLoopExp = false;
            emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
            emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
            // emit wsThread->sendMessageToClient("GuiderStatus:InCalibration");
            Logger::Log("Stop GuiderSwitch finish!", LogLevel::INFO, DeviceType::GUIDER);
        } else if (!isGuiding && parts[1].trimmed() == "true"){
            isGuiding = true;
            emit wsThread->sendMessageToClient("GuiderSwitchStatus:true");
            if (ClearCalibrationData) {
                ClearCalibrationData = false;
                call_phd_ClearCalibration();
            }
            Logger::Log("clear calibration data finish!", LogLevel::INFO, DeviceType::GUIDER);
            
            // call_phd_StartLooping();
            // sleep(1);
            if (glPHD_isSelected == false) {
                Logger::Log("AutoFindStar is not selected, start AutoFindStar ...", LogLevel::INFO, DeviceType::GUIDER);
                call_phd_AutoFindStar();
            }
            call_phd_StartGuiding();
            emit wsThread->sendMessageToClient("GuiderUpdateStatus:1");
            Logger::Log("Start GuiderSwitch finish!", LogLevel::INFO, DeviceType::GUIDER);
        } else {
            if (parts[1].trimmed() == "true") {
                emit wsThread->sendMessageToClient("GuiderSwitchStatus:true");
                emit wsThread->sendMessageToClient("GuiderUpdateStatus:1");
            } else {
                emit wsThread->sendMessageToClient("GuiderSwitchStatus:false");
                emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
            }
            Logger::Log("GuiderSwitch status already set " + parts[1].trimmed().toStdString() + " not change!!!", LogLevel::WARNING, DeviceType::GUIDER);
        }
    }

    else if (parts[0].trimmed() == "GuiderLoopExpSwitch" && parts.size() == 2)
    {
        if(dpGuider != NULL) {
            if (isGuiderLoopExp && parts[1].trimmed() == "false"){
                Logger::Log("Stop GuiderLoopExp ...", LogLevel::INFO, DeviceType::GUIDER);
                isGuiderLoopExp = false;
                isGuiding = false;
                call_phd_StopLooping();
                emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
                emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
                Logger::Log("Stop GuiderLoopExp finish!", LogLevel::INFO, DeviceType::GUIDER);
            } else if (!isGuiderLoopExp && parts[1].trimmed() == "true"){
                Logger::Log("Start GuiderLoopExp ...", LogLevel::INFO, DeviceType::GUIDER);
                isGuiderLoopExp = true;
                emit wsThread->sendMessageToClient("GuiderLoopExpStatus:true");
                emit wsThread->sendMessageToClient("GuiderUpdateStatus:1");
                call_phd_StartLooping();
                Logger::Log("Start GuiderLoopExp finish!", LogLevel::INFO, DeviceType::GUIDER);
            }else{
                Logger::Log("GuiderLoopExp status already set " + parts[1].trimmed().toStdString() + " not change!!!", LogLevel::WARNING, DeviceType::GUIDER);
                if (parts[1].trimmed() == "true"){
                    emit wsThread->sendMessageToClient("GuiderLoopExpStatus:true");
                    emit wsThread->sendMessageToClient("GuiderUpdateStatus:1");
                } else {
                    emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
                    emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
                }
            }
        }else{
            Logger::Log("GuiderLoopExp is not connected", LogLevel::INFO, DeviceType::GUIDER);
        }
    }

    else if (message == "PHD2Recalibrate")
    {
        Logger::Log("PHD2Recalibrate ...", LogLevel::DEBUG, DeviceType::GUIDER);
        call_phd_ClearCalibration();
        call_phd_StartLooping();
        sleep(1);

        call_phd_AutoFindStar();
        call_phd_StartGuiding();
        Logger::Log("PHD2Recalibrate finish!", LogLevel::DEBUG, DeviceType::GUIDER);
        // emit wsThread->sendMessageToClient("GuiderSwitchStatus:true");

        // call_phd_StarClick(641,363);
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "GuiderExpTimeSwitch")
    {
        Logger::Log("GuiderExpTimeSwitch :" + std::to_string(parts[1].toInt()), LogLevel::DEBUG, DeviceType::GUIDER);
        call_phd_setExposureTime(parts[1].toInt());
        Logger::Log("GuiderExpTimeSwitch finish!", LogLevel::DEBUG, DeviceType::GUIDER);
    }

    else if (message == "clearGuiderData")
    {
        Logger::Log("clearGuiderData ...", LogLevel::DEBUG, DeviceType::GUIDER);
        glPHD_rmsdate.clear();
        Logger::Log("clearGuiderData finish!", LogLevel::DEBUG, DeviceType::GUIDER);
    }

    // else if (message == "getGuiderSwitchStatus") 
    // {
    //     if(isGuiding) {
    //         emit wsThread->sendMessageToClient("GuiderSwitchStatus:true");
    //     } else {
    //         emit wsThread->sendMessageToClient("GuiderSwitchStatus:false");
    //     }
    // }

    else if (parts.size() == 2 && parts[0].trimmed() == "SolveSYNC")
    {
        Logger::Log("SolveSYNC ...", LogLevel::DEBUG, DeviceType::MAIN);
        glFocalLength = parts[1].trimmed().toInt();
        // glCameraSize_width  = parts[2].trimmed().toDouble();
        // glCameraSize_height = parts[3].trimmed().toDouble();

        if(glFocalLength == 0) {
            emit wsThread->sendMessageToClient("FocalLengthError");
            Logger::Log("FocalLengthError", LogLevel::DEBUG, DeviceType::MAIN);
        } else {
            TelescopeControl_SolveSYNC();
            Logger::Log("SolveSYNC finish!", LogLevel::DEBUG, DeviceType::MAIN);
        }
    }

    else if (message == "ClearDataPoints")
    {
        Logger::Log("ClearDataPoints ...", LogLevel::DEBUG, DeviceType::MAIN);
        // FWHM Data
        dataPoints.clear();
        Logger::Log("ClearDataPoints finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (message == "ShowAllImageFolder")
    {
        Logger::Log("ShowAllImageFolder ...", LogLevel::DEBUG, DeviceType::MAIN);
        std::string allFile = GetAllFile();
        emit wsThread->sendMessageToClient("ShowAllImageFolder:" + QString::fromStdString(allFile));
        Logger::Log("ShowAllImageFolder finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "MoveFileToUSB")
    {
        Logger::Log("MoveFileToUSB ...", LogLevel::DEBUG, DeviceType::MAIN);
        QStringList ImagePath= parseString(parts[1].trimmed().toStdString(),ImageSaveBasePath);
        RemoveImageToUsb(ImagePath);
        Logger::Log("MoveFileToUSB finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (parts[0].trimmed() == "DeleteFile")
    {
        Logger::Log("DeleteFile ...", LogLevel::DEBUG, DeviceType::MAIN);
        QString ImagePathString = message; // 创建副本
        ImagePathString.replace("DeleteFile:", "");

        QStringList ImagePath= parseString(ImagePathString.toStdString(),ImageSaveBasePath);
        DeleteImage(ImagePath);
        Logger::Log("DeleteFile finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (message == "USBCheck"){
        Logger::Log("USBCheck ...", LogLevel::DEBUG, DeviceType::MAIN);
        USBCheck();
        Logger::Log("USBCheck finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "GetImageFiles")
    {
        Logger::Log("GetImageFiles ...", LogLevel::DEBUG, DeviceType::MAIN);
        std::string FolderPath= parts[1].trimmed().toStdString();
        GetImageFiles(FolderPath);
        Logger::Log("GetImageFiles finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (parts[0].trimmed() == "ReadImageFile") {
        Logger::Log("ReadImageFile ...", LogLevel::DEBUG, DeviceType::MAIN);
        QString ImagePath = message; // 创建副本
        ImagePath.replace("ReadImageFile:", "image/");
        // ImagePath.replace(" ", "\\ "); // 转义空格
        // ImagePath.replace("[", "\\["); // 转义左方括号
        // ImagePath.replace("]", "\\]"); // 转义右方括号
        // ImagePath.replace(",", "\\,"); // 转义逗号
        saveFitsAsPNG(ImagePath, false);
        Logger::Log("ReadImageFile finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "SolveImage")
    {
        Logger::Log("SolveImage ...", LogLevel::DEBUG, DeviceType::CAMERA);
        glFocalLength = parts[1].trimmed().toInt();

        if(glFocalLength == 0) {
            emit wsThread->sendMessageToClient("FocalLengthError");
            Logger::Log("SolveImage FocalLengthError", LogLevel::DEBUG, DeviceType::CAMERA);
        } else {
            EndCaptureAndSolve = false;
            CaptureAndSolve(glExpTime, false);
            Logger::Log("SolveImage finish!", LogLevel::DEBUG, DeviceType::CAMERA);
        }
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "startLoopSolveImage") {
        Logger::Log("startLoopSolveImage ...", LogLevel::DEBUG, DeviceType::CAMERA);
        glFocalLength = parts[1].trimmed().toInt();

        if(glFocalLength == 0) {
            emit wsThread->sendMessageToClient("FocalLengthError");
            Logger::Log("startLoopSolveImage FocalLengthError", LogLevel::DEBUG, DeviceType::CAMERA);
        } else {
            isLoopSolveImage = true;
            EndCaptureAndSolve = false;
            CaptureAndSolve(glExpTime, true);
            Logger::Log("startLoopSolveImage finish!", LogLevel::DEBUG, DeviceType::CAMERA);
        }
    }

    else if (message == "stopLoopSolveImage") {
        Logger::Log("stopLoopSolveImage ...", LogLevel::DEBUG, DeviceType::CAMERA);
        isLoopSolveImage = false;
        Logger::Log("stopLoopSolveImage finish!", LogLevel::DEBUG, DeviceType::CAMERA);
    }

    else if (message == "EndCaptureAndSolve") {
        Logger::Log("EndCaptureAndSolve ...", LogLevel::DEBUG, DeviceType::CAMERA);
        EndCaptureAndSolve = true;
        Logger::Log("EndCaptureAndSolve finish!", LogLevel::DEBUG, DeviceType::CAMERA);
    }

    else if (message == "getStagingSolveResult") {
        Logger::Log("getStagingSolveResult ...", LogLevel::DEBUG, DeviceType::CAMERA);
        RecoverySloveResul();
        Logger::Log("getStagingSolveResult finish!", LogLevel::DEBUG, DeviceType::CAMERA);
    }

    else if (message == "ClearSloveResultList") {
        Logger::Log("ClearSloveResultList ...", LogLevel::DEBUG, DeviceType::CAMERA);
        ClearSloveResultList();
        Logger::Log("ClearSloveResultList finish!", LogLevel::DEBUG, DeviceType::CAMERA);
    }

    else if (message == "getOriginalImage") {
        Logger::Log("getOriginalImage ...", LogLevel::DEBUG, DeviceType::CAMERA);
        saveFitsAsPNG(QString::fromStdString("/dev/shm/ccd_simulator_original.fits"), false);
        Logger::Log("getOriginalImage finish!", LogLevel::DEBUG, DeviceType::CAMERA);
    }

    else if (message == "getGPIOsStatus") {
        Logger::Log("getGPIOsStatus ...", LogLevel::DEBUG, DeviceType::MAIN);
        getGPIOsStatus();
        Logger::Log("getGPIOsStatus finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "SwitchOutPutPower")
    {
        Logger::Log("SwitchOutPutPower ...", LogLevel::DEBUG, DeviceType::MAIN);
        int index = parts[1].trimmed().toInt();
        int value;

        if(index == 1) {
            value = readGPIOValue(GPIO_PIN_1);
            if(value == 1) {
                setGPIOValue(GPIO_PIN_1, "0");
                value = readGPIOValue(GPIO_PIN_1);
            } else {
                setGPIOValue(GPIO_PIN_1, "1");
                value = readGPIOValue(GPIO_PIN_1);
            }
        } 
        else if(index == 2) {
            value = readGPIOValue(GPIO_PIN_2);
            if(value == 1) {
                setGPIOValue(GPIO_PIN_2, "0");
                value = readGPIOValue(GPIO_PIN_2);
            } else {
                setGPIOValue(GPIO_PIN_2, "1");
                value = readGPIOValue(GPIO_PIN_2);
            }
        }

        emit wsThread->sendMessageToClient("OutputPowerStatus:" + QString::number(index) + ":" + QString::number(value));
        Logger::Log("SwitchOutPutPower finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "SetBinning") {
        glMainCameraBinning = parts[1].trimmed().toInt();
        Logger::Log("Set Binning to " + std::to_string(glMainCameraBinning), LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveParameter("MainCamera","Binning",parts[1].trimmed());

        // if(dpMainCamera != NULL) {
        //     indi_Client->setCCDBinnign(dpMainCamera, 2, 2);
        // }
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "SetCameraTemperature") {
        double CameraTemperature = parts[1].trimmed().toDouble();
        Logger::Log("Set Camera Temperature to " + std::to_string(CameraTemperature), LogLevel::DEBUG, DeviceType::MAIN);

        if(dpMainCamera != NULL) {
            indi_Client->setTemperature(dpMainCamera, CameraTemperature);
        }
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "SetCameraGain") {
        int CameraGain = parts[1].trimmed().toInt();
        Logger::Log("Set Camera Gain to " + std::to_string(CameraGain), LogLevel::DEBUG, DeviceType::MAIN);
        Tools::saveParameter("MainCamera","Gain",parts[1].trimmed());
        if(dpMainCamera != NULL) {
            indi_Client->setCCDGain(dpMainCamera, CameraGain);
        }
    }

    else if (parts.size() == 5 && parts[0].trimmed() == "GuiderCanvasClick")
    {
        int CanvasWidth = parts[1].trimmed().toInt();
        int CanvasHeight = parts[2].trimmed().toInt();
        int Click_X = parts[3].trimmed().toInt();
        int Click_Y = parts[4].trimmed().toInt();

        Logger::Log("GuiderCanvasClick:" + std::to_string(CanvasWidth) + "," + std::to_string(CanvasHeight) + "," + std::to_string(Click_X) + "," + std::to_string(Click_Y), LogLevel::DEBUG, DeviceType::MAIN);

        if (glPHD_CurrentImageSizeX != 0 && glPHD_CurrentImageSizeY != 0)
        {
            Logger::Log("PHD2ImageSize:" + std::to_string(glPHD_CurrentImageSizeX) + "," + std::to_string(glPHD_CurrentImageSizeY), LogLevel::DEBUG, DeviceType::MAIN);
            double ratioZoomX = (double) glPHD_CurrentImageSizeX / CanvasWidth;
            double ratioZoomY = (double) glPHD_CurrentImageSizeY / CanvasHeight;
            Logger::Log("ratioZoom:" + std::to_string(ratioZoomX) + "," + std::to_string(ratioZoomY), LogLevel::DEBUG, DeviceType::MAIN);
            double PHD2Click_X = (double) Click_X * ratioZoomX;
            double PHD2Click_Y = (double) Click_Y * ratioZoomY;
            Logger::Log("PHD2Click:" + std::to_string(PHD2Click_X) + "," + std::to_string(PHD2Click_Y), LogLevel::DEBUG, DeviceType::MAIN);
            call_phd_StarClick(PHD2Click_X, PHD2Click_Y);
        }
    }

    else if (message == "getQTClientVersion") {
        Logger::Log("getQTClientVersion ...", LogLevel::DEBUG, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("QTClientVersion:" + QString::fromStdString(QT_Client_Version));
    }

    else if (message == "getHotspotName") {
        QString HostpotName = getHotspotName();
        Logger::Log("HotspotName:" + HostpotName.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("HotspotName:" + HostpotName);
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "editHotspotName") {
        Logger::Log("editHotspotName ...", LogLevel::DEBUG, DeviceType::MAIN);
        QString HostpotName = parts[1].trimmed();
        editHotspotName(HostpotName);
        Logger::Log("editHotspotName finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (parts.size() == 4 && parts[0].trimmed() == "DSLRCameraInfo") {
        Logger::Log("DSLRCameraInfo ...", LogLevel::DEBUG, DeviceType::MAIN);
        int Width = parts[1].trimmed().toInt();
        int Height = parts[2].trimmed().toInt();
        double PixelSize = parts[3].trimmed().toDouble();
        
        if(dpMainCamera != NULL) {
            indi_Client->setCCDBasicInfo(dpMainCamera, Width, Height, PixelSize, PixelSize, PixelSize, 8);
            AfterDeviceConnect(dpMainCamera);

            DSLRsInfo DSLRsInfo;
            DSLRsInfo.Name = dpMainCamera->getDeviceName();
            DSLRsInfo.SizeX = Width;
            DSLRsInfo.SizeY = Height;
            DSLRsInfo.PixelSize = PixelSize;

            Tools::saveDSLRsInfo(DSLRsInfo);
            Logger::Log("DSLRCameraInfo finish!", LogLevel::DEBUG, DeviceType::MAIN);
        } else {
            Logger::Log("DSLRCameraInfo failed! Main Camera is NULL", LogLevel::DEBUG, DeviceType::MAIN);
        }

    }

    else if (parts.size() == 3 && parts[0].trimmed() == "saveToConfigFile") {
        Logger::Log("saveToConfigFile ...", LogLevel::DEBUG, DeviceType::MAIN);
        QString ConfigName = parts[1].trimmed();
        QString ConfigValue = parts[2].trimmed();
        
        setClientSettings(ConfigName, ConfigValue);
        Logger::Log("saveToConfigFile finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "GuiderFocalLength") {
        int FocalLength = parts[1].trimmed().toInt();
        Logger::Log("Set Guider Focal Length to " + std::to_string(FocalLength), LogLevel::DEBUG, DeviceType::MAIN);

        call_phd_FocalLength(FocalLength);
    }

    else if (message == "RestartRaspberryPi") {
        Logger::Log("RestartRaspberryPi ...", LogLevel::DEBUG, DeviceType::MAIN);
        system("reboot");
        Logger::Log("RestartRaspberryPi finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (message == "ShutdownRaspberryPi") {
        Logger::Log("ShutdownRaspberryPi ...", LogLevel::DEBUG, DeviceType::MAIN);
        system("shutdown -h now");
        Logger::Log("ShutdownRaspberryPi finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "MultiStarGuider") {
        bool isMultiStar = (parts[1].trimmed() == "true");
        Logger::Log("Set Multi Star Guider to" + std::to_string(isMultiStar), LogLevel::DEBUG, DeviceType::MAIN);

        call_phd_MultiStarGuider(isMultiStar);
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "GuiderPixelSize") {
        double PixelSize = parts[1].trimmed().toDouble();
        Logger::Log("Set Guider Pixel Size to" + std::to_string(PixelSize), LogLevel::DEBUG, DeviceType::MAIN);

        call_phd_CameraPixelSize(PixelSize);
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "GuiderGain") {
        int Gain = parts[1].trimmed().toInt();
        Logger::Log("Set Guider Gain to" + std::to_string(Gain), LogLevel::DEBUG, DeviceType::MAIN);

        call_phd_CameraGain(Gain);
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "CalibrationDuration") {
        int StepSize = parts[1].trimmed().toInt();
        Logger::Log("Set Calibration Duration to" + std::to_string(StepSize), LogLevel::DEBUG, DeviceType::MAIN);

        call_phd_CalibrationDuration(StepSize);
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "RaAggression") {
        int Aggression = parts[1].trimmed().toInt();
        Logger::Log("Set Ra Aggression to" + std::to_string(Aggression), LogLevel::DEBUG, DeviceType::MAIN);

        call_phd_RaAggression(Aggression);
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "DecAggression") {
        int Aggression = parts[1].trimmed().toInt();
        Logger::Log("Set Dec Aggression to" + std::to_string(Aggression), LogLevel::DEBUG, DeviceType::MAIN);

        call_phd_DecAggression(Aggression);
    }
    else if (parts.size() == 3 && parts[0].trimmed() == "ConnectDriver")
    {
        QString DriverName = parts[1].trimmed();
        QString DriverType = parts[2].trimmed();
        Logger::Log("Connect Driver to " + DriverName.toStdString() + " with type " + DriverType.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        ConnectDriver(DriverName, DriverType);
    }
    else if (parts.size() == 3 && parts[0].trimmed() == "DisconnectDevice")
    {
        QString DeviceName = parts[1].trimmed();
        QString DeviceType = parts[2].trimmed();
        Logger::Log("Disconnect " + DeviceType.toStdString() + " Device(" + DeviceName.toStdString() + ")", LogLevel::DEBUG, DeviceType::MAIN);
        DisconnectDevice(indi_Client, DeviceName, DeviceType);
    }else if(message == "loadSelectedDriverList"){
        Logger::Log("loadSelectedDriverList ...", LogLevel::DEBUG, DeviceType::MAIN);
        loadSelectedDriverList();
        Logger::Log("loadSelectedDriverList finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }else if(message == "loadBindDeviceList"){
        Logger::Log("loadBindDeviceList ...", LogLevel::DEBUG, DeviceType::MAIN);
        loadBindDeviceList(indi_Client);
        Logger::Log("loadBindDeviceList finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }else if(message == "loadBindDeviceTypeList"){
        Logger::Log("loadBindDeviceTypeList ...", LogLevel::DEBUG, DeviceType::MAIN);
        loadBindDeviceTypeList();
        Logger::Log("loadBindDeviceTypeList finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }else if(parts.size() == 2 && parts[0].trimmed() == "disconnectSelectDriver"){
        Logger::Log("disconnect Driver " + parts[1].trimmed().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        disconnectDriver(parts[1]);
        Logger::Log("disconnect Driver " + parts[1].trimmed().toStdString() +" success!", LogLevel::DEBUG, DeviceType::MAIN);
    }else if(parts.size() == 2 && parts[0].trimmed() == "FocuserMoveState"){
        Logger::Log("FocuserMoveState ...", LogLevel::DEBUG, DeviceType::FOCUSER);
        if (parts[1].trimmed() == "true") {
            focusMoveEndTime = 2;
        }
        Logger::Log("FocuserMoveState finish!", LogLevel::DEBUG, DeviceType::FOCUSER);
    }else if(parts.size() == 2 && parts[0].trimmed() == "FocusLoopShooting"){
        Logger::Log("FocusLoopShooting ...", LogLevel::DEBUG, DeviceType::FOCUSER);
        focusLoopShooting(parts[1].trimmed() == "true");
        Logger::Log("FocusLoopShooting finish!", LogLevel::DEBUG, DeviceType::FOCUSER);
    }else if(parts[0].trimmed() == "getFocuserLoopingState"){
        Logger::Log("getFocuserLoopingState ...", LogLevel::DEBUG, DeviceType::FOCUSER);
        getFocuserLoopingState();
        Logger::Log("getFocuserLoopingState finish!", LogLevel::DEBUG, DeviceType::FOCUSER);
    }else if(parts[0].trimmed() == "getROIInfo"){
        Logger::Log("getRedBoxState ...", LogLevel::DEBUG, DeviceType::MAIN);
        sendRoiInfo();
        Logger::Log("getRedBoxState finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }else if(parts[0].trimmed() == "sendRedBoxState" && parts.size() == 4){
        Logger::Log("sendRedBoxState ...", LogLevel::DEBUG, DeviceType::MAIN);
        roiAndFocuserInfo["BoxSideLength"] = parts[1].trimmed().toDouble();
        roiAndFocuserInfo["ROI_x"] = parts[2].trimmed().toDouble();
        roiAndFocuserInfo["ROI_y"] = parts[3].trimmed().toDouble();
        Logger::Log("sendRedBoxState finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }else if(parts[0].trimmed() == "sendVisibleArea" && parts.size() == 4){
        Logger::Log("sendVisibleArea ...", LogLevel::DEBUG, DeviceType::MAIN);
        roiAndFocuserInfo["VisibleX"] = parts[1].trimmed().toDouble();
        roiAndFocuserInfo["VisibleY"] = parts[2].trimmed().toDouble();
        roiAndFocuserInfo["Scale"] = parts[3].trimmed().toDouble();
    }else if(parts[0].trimmed() == "sendSelectStars" && parts.size() == 3){
        Logger::Log("sendSelectStars ...", LogLevel::DEBUG, DeviceType::MAIN);
        roiAndFocuserInfo["SelectStarX"] = parts[1].trimmed().toDouble();
        roiAndFocuserInfo["SelectStarY"] = parts[2].trimmed().toDouble();
        Logger::Log("sendSelectStars finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }else if(parts[0].trimmed() == "testQtServerProcess"){
        Logger::Log("testQtServerProcess ... .....................", LogLevel::DEBUG, DeviceType::MAIN);
        emit wsThread->sendProcessCommandReturn("ServerInitSuccess");
    }
    else{
        Logger::Log("Unknown message: " + message.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
    }
    
}

void MainWindow::initINDIServer()
{
    Logger::Log("initINDIServer ...", LogLevel::INFO, DeviceType::MAIN);
    system("pkill indiserver");
    system("rm -f /tmp/myFIFO");
    system("mkfifo /tmp/myFIFO");
    glIndiServer = new QProcess();
    // glIndiServer->setReadChannel(QProcess::StandardOutput);

    // // 连接信号到槽函数
    // connect(glIndiServer, &QProcess::readyReadStandardOutput, this, &MainWindow::handleIndiServerOutput);
    // connect(glIndiServer, &QProcess::readyReadStandardError, this, &MainWindow::handleIndiServerError);

    glIndiServer->start("indiserver -f /tmp/myFIFO -v -p 7624");
    Logger::Log("initINDIServer finish!", LogLevel::INFO, DeviceType::MAIN);
}

// void MainWindow::initINDIServer()
// {
//     system("pkill indiserver");
//     system("rm -f /tmp/myFIFO");
//     system("mkfifo /tmp/myFIFO");
//     glIndiServer = new QProcess();
//     glIndiServer->setReadChannel(QProcess::StandardOutput);
//     glIndiServer->start("indiserver -f /tmp/myFIFO -v -p 7624");
// }


// 槽函数：处理标准输出
void MainWindow::handleIndiServerOutput()
{
    QByteArray output = glIndiServer->readAllStandardOutput();
    Logger::Log("INDI Server Output: " + output.toStdString(), LogLevel::INFO, DeviceType::MAIN);
}

// 槽函数：处理标准错误
void MainWindow::handleIndiServerError()
{
    QByteArray error = glIndiServer->readAllStandardError();
    Logger::Log("INDI Server Error: " + error.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
}

void MainWindow::initINDIClient()
{
    Logger::Log("initINDIClient ...", LogLevel::INFO, DeviceType::MAIN);
    indi_Client = new MyClient();
    indi_Client->setServer("localhost", 7624);
    indi_Client->setConnectionTimeout(3, 0);
    Logger::Log("setConnectionTimeout is 3 seconds!", LogLevel::INFO, DeviceType::MAIN);
    indi_Client->setImageReceivedCallback(
        [this](const std::string &filename, const std::string &devname)
        {
            // 曝光完成
            if(dpMainCamera!=NULL){
                if(dpMainCamera->getDeviceName()==devname)
                {
                    glMainCameraStatu = "Displaying";
                    ShootStatus = "Completed";
                    if(glIsFocusingLooping == false)
                    {
                        emit wsThread->sendMessageToClient("ExposureCompleted");
                        Logger::Log("ExposureCompleted", LogLevel::INFO, DeviceType::CAMERA);
                        saveFitsAsPNG(QString::fromStdString(filename), true);    // "/dev/shm/ccd_simulator.fits"

                        // saveFitsAsPNG("/dev/shm/SOLVETEST.fits", true);
                    }
                    else
                    {
                        if (isFocusLoopShooting ){
                            saveFitsAsJPG(QString::fromStdString(filename), true);
                            Logger::Log("saveFitsAsJPG", LogLevel::DEBUG, DeviceType::MAIN);
                        }
                    }
                    // Logger::Log("拍摄完成，图像保存完成 finish!", LogLevel::INFO, DeviceType::MAIN);

                    
                }
            }
        }
    );
    Logger::Log("indi_Client->setImageReceivedCallback finish!", LogLevel::INFO, DeviceType::MAIN);

    indi_Client->setMessageReceivedCallback(
        [this](const std::string &message) {
            // qDebug("indi初始信息 %s", message.c_str());
            QString messageStr = QString::fromStdString(message.c_str());

            // 使用正则表达式移除时间戳
            std::regex timestampRegex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}: )");
            messageStr = QString::fromStdString(std::regex_replace(messageStr.toStdString(), timestampRegex, ""));

            // qDebug("indi提取后信息 %s", messageStr.toStdString().c_str());
            // 使用正则表达式提取并移除日志类型
            std::regex typeRegex(R"(\[(INFO|WARNING|ERROR)\])");
            std::smatch typeMatch;
            QString logType;
            if (std::regex_search(message, typeMatch, typeRegex) && typeMatch.size() > 1) {
                logType = QString::fromStdString(typeMatch[1].str());
                // 移除日志类型
                messageStr = QString::fromStdString(std::regex_replace(messageStr.toStdString(), typeRegex, ""));
            }

            if (messageStr.contains("Telescope focal length is missing.") || 
                messageStr.contains("Telescope aperture is missing.")) {
                // 跳过打印
                return;
            }
            if (logType == "WARNING") {
                DeviceType deviceType = getDeviceTypeFromPartialString(messageStr.toStdString());
                Logger::Log("[INDI SERVER] " + messageStr.toStdString(), LogLevel::WARNING, deviceType);
            }else if (logType == "ERROR") {
                DeviceType deviceType = getDeviceTypeFromPartialString(messageStr.toStdString());
                Logger::Log("[INDI SERVER] " + messageStr.toStdString(), LogLevel::ERROR, deviceType);
            }else{  
                DeviceType deviceType = getDeviceTypeFromPartialString(messageStr.toStdString());
                Logger::Log("[INDI SERVER] " + messageStr.toStdString(), LogLevel::INFO, deviceType);
            }

            std::regex regexPattern(R"(OnStep slew/syncError:\s*(.*))");
            std::smatch matchResult;

            if (std::regex_search(message, matchResult, regexPattern)) {
                if (matchResult.size() > 1) {
                    QString errorContent = QString::fromStdString(matchResult[1].str());
                    Logger::Log("OnStep Error: " + errorContent.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                    emit wsThread->sendMessageToClient("OnStep Error:" + errorContent);
                    MountGotoError = true;
                }
            }
        }
    );
    Logger::Log("indi_Client->setMessageReceivedCallback finish!", LogLevel::INFO, DeviceType::MAIN);
}

DeviceType MainWindow::getDeviceTypeFromPartialString(const std::string& typeStr) {
    // 使用find()方法检查字符串中是否包含特定的子字符串
    if (typeStr.find("Exposure done, downloading image") != std::string::npos || typeStr.find("Download complete") != std::string::npos || typeStr.find("Uploading file.") != std::string::npos || typeStr.find("Image saved to") != std::string::npos) {
        // Logger::Log("获取的信息类型是相机", LogLevel::INFO, DeviceType::CAMERA);
        return DeviceType::CAMERA;
    } else if (0) {
        // Logger::Log("DeviceType::MOUNT", LogLevel::INFO, DeviceType::MOUNT);
        return DeviceType::MOUNT;
    } else {
        // Logger::Log("DeviceType::MAIN", LogLevel::INFO, DeviceType::MAIN);
        return DeviceType::MAIN;  
    }
}   

void MainWindow::initGPIO() {
    Logger::Log("Initializing GPIO...", LogLevel::INFO, DeviceType::MAIN);
    // Initialize GPIO_PIN_1
    exportGPIO(GPIO_PIN_1);
    setGPIODirection(GPIO_PIN_1, "out");
    Logger::Log("Set direction of GPIO_PIN_1 to output completed!", LogLevel::INFO, DeviceType::MAIN);

    // Initialize GPIO_PIN_2
    exportGPIO(GPIO_PIN_2);
    setGPIODirection(GPIO_PIN_2, "out");
    Logger::Log("Set direction of GPIO_PIN_2 to output completed!", LogLevel::INFO, DeviceType::MAIN);

    // Set GPIO_PIN_1 to high level
    setGPIOValue(GPIO_PIN_1, "1");
    Logger::Log("Set GPIO_PIN_1 level to high completed!", LogLevel::INFO, DeviceType::MAIN);
    // Set GPIO_PIN_2 to high level
    setGPIOValue(GPIO_PIN_2, "1");
    Logger::Log("Set GPIO_PIN_2 level to high completed!", LogLevel::INFO, DeviceType::MAIN);

    Logger::Log("GPIO initialization completed!", LogLevel::INFO, DeviceType::MAIN);
}



void MainWindow::exportGPIO(const char* pin) {
    int fd;
    char buf[64];

    // Export GPIO pin
    fd = open(GPIO_EXPORT, O_WRONLY);
    if (fd < 0) {
        Logger::Log("Failed to open export file for writing", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    snprintf(buf, sizeof(buf), "%s", pin);
    if (write(fd, buf, strlen(buf)) != strlen(buf)) {
        Logger::Log("Failed to write to export file", LogLevel::INFO, DeviceType::MAIN);
        close(fd);
        return;
    }
    close(fd);
    Logger::Log("GPIO pin export successful", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::setGPIODirection(const char* pin, const char* direction) {
    int fd;
    char path[128];

    // Set GPIO direction
    snprintf(path, sizeof(path), GPIO_PATH "/gpio%s/direction", pin);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        Logger::Log("Failed to open GPIO direction file for writing", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    if (write(fd, direction, strlen(direction) + 1) != strlen(direction) + 1) {
        Logger::Log("Failed to set GPIO direction", LogLevel::INFO, DeviceType::MAIN);
        close(fd);
        return;
    }
    close(fd);
    Logger::Log("GPIO direction set successfully", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::setGPIOValue(const char* pin, const char* value) {
    int fd;
    char path[128];

    // Set GPIO value
    snprintf(path, sizeof(path), GPIO_PATH "/gpio%s/value", pin);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        Logger::Log("Failed to open GPIO value file for writing", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    if (write(fd, value, strlen(value) + 1) != strlen(value) + 1) {
        Logger::Log("Failed to write to GPIO value", LogLevel::INFO, DeviceType::MAIN);
        close(fd);
        return;
    }
    close(fd);
    Logger::Log("GPIO value set successfully", LogLevel::INFO, DeviceType::MAIN);
}

int MainWindow::readGPIOValue(const char* pin) {
    int fd;
    char path[128];
    char value[3];  // Store the read value

    // Construct the path to the GPIO value file
    snprintf(path, sizeof(path), GPIO_PATH "/gpio%s/value", pin);

    // Open the GPIO value file for reading
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        Logger::Log("Failed to open GPIO value file for reading", LogLevel::WARNING, DeviceType::MAIN);
        return -1;  // Return -1 to indicate read failure
    }

    // Read the file content
    if (read(fd, value, sizeof(value)) < 0) {
        Logger::Log("Failed to read GPIO value", LogLevel::INFO, DeviceType::MAIN);
        close(fd);
        return -1;  // Return -1 to indicate read failure
    }

    close(fd);

    // Determine if the read value is '1' or '0'
    if (value[0] == '1') {
        return 1;  // Return 1 to indicate high level
    } else if (value[0] == '0') {
        return 0;  // Return 0 to indicate low level
    } else {
        return -1;  // If the value is not '0' or '1', return -1
    }
}

void MainWindow::getGPIOsStatus() {
    int value1 = readGPIOValue(GPIO_PIN_1);
    emit wsThread->sendMessageToClient("OutputPowerStatus:1:" + QString::number(value1));
    int value2 = readGPIOValue(GPIO_PIN_2);
    emit wsThread->sendMessageToClient("OutputPowerStatus:2:" + QString::number(value2));
}

void MainWindow::onTimeout()
{
    ShowPHDdata();

    // 显示赤道仪指向
    mountDisplayCounter++;
    if (dpMount != NULL)
    {
        if (dpMount->isConnected())
        {
            if (mountDisplayCounter >= 100)
            {
                double RA_HOURS, DEC_DEGREE;
                indi_Client->getTelescopeRADECJNOW(dpMount, RA_HOURS, DEC_DEGREE);
                double CurrentRA_Degree = Tools::HourToDegree(RA_HOURS);
                double CurrentDEC_Degree = DEC_DEGREE;

                emit wsThread->sendMessageToClient("TelescopeRADEC:" + QString::number(CurrentRA_Degree) + ":" + QString::number(CurrentDEC_Degree));
                
                // bool isSlewing = (TelescopeControl_Status() != "Slewing");

                indi_Client->getTelescopePierSide(dpMount, TelescopePierSide);
                // qDebug() << "TelescopePierSide: " << TelescopePierSide;
                emit wsThread->sendMessageToClient("TelescopePierSide:" + TelescopePierSide);

                if(!FirstRecordTelescopePierSide) {
                    if(FirstTelescopePierSide != TelescopePierSide) {
                        isMeridianFlipped = true;
                    } else {
                        isMeridianFlipped = false;
                    }
                }

                emit wsThread->sendMessageToClient("TelescopeStatus:" + TelescopeControl_Status().status);

                mountDisplayCounter = 0;
            }
        }
    }

    MainCameraStatusCounter++;
    if(dpMainCamera != NULL) {
        if(MainCameraStatusCounter >= 100) {
            emit wsThread->sendMessageToClient("MainCameraStatus:" + glMainCameraStatu);
            MainCameraStatusCounter = 0;
            double CameraTemp;
            uint32_t ret;
            ret = indi_Client->getTemperature(dpMainCamera, CameraTemp);
            if(ret == QHYCCD_SUCCESS) {
                emit wsThread->sendMessageToClient("MainCameraTemperature:" + QString::number(CameraTemp));
            }
        }
    }
}

// void MainWindow::saveFitsAsJPG(QString filename)
// {
//     Logger::Log("Starting to save FITS as JPG...", LogLevel::INFO, DeviceType::GUIDER);
//     cv::Mat image;
//     cv::Mat image16;
//     cv::Mat SendImage;
//     Tools::readFits(filename.toLocal8Bit().constData(), image);
//     Logger::Log("FITS file read successfully.", LogLevel::INFO, DeviceType::GUIDER);

//     QList<FITSImage::Star> stars = Tools::FindStarsByStellarSolver(true, true);
//     Logger::Log("Star detection completed.", LogLevel::INFO, DeviceType::GUIDER);

//     if(stars.size() != 0){
//         FWHM = stars[0].HFR;
//         Logger::Log("FWHM calculated from detected stars.", LogLevel::INFO, DeviceType::GUIDER);
//     }
//     else {
//         FWHM = -1;
//         Logger::Log("No stars detected, FWHM set to -1.", LogLevel::WARNING, DeviceType::GUIDER);
//     }   

//     if(image16.depth()==8) {
//         image.convertTo(image16, CV_16UC1, 256, 0); //x256  MSB alignment
//         Logger::Log("Image converted to 16-bit format with MSB alignment.", LogLevel::INFO, DeviceType::GUIDER);
//     }
//     else {
//         image.convertTo(image16, CV_16UC1, 1, 0);
//         Logger::Log("Image converted to 16-bit format.", LogLevel::INFO, DeviceType::GUIDER);
//     }

//     if(FWHM != -1){
//         // 在原图上绘制检测结果
//         cv::Point center(stars[0].x, stars[0].y);
//         cv::circle(image16, center, static_cast<int>(FWHM), cv::Scalar(0, 0, 255), 1); // Draw HFR circle
//         cv::circle(image16, center, 1, cv::Scalar(0, 255, 0), -1);                     // Draw center point
//         std::string hfrText = cv::format("%.2f", stars[0].HFR);
//         cv::putText(image16, hfrText, cv::Point(stars[0].x - FWHM, stars[0].y - FWHM - 5), cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(0, 0, 255), 1);
//         Logger::Log("Annotations for stars added to image.", LogLevel::INFO, DeviceType::GUIDER);
//     }

//     cv::Mat NewImage = image16;
//     FWHMCalOver = true;

//     cv::normalize(NewImage, SendImage, 0, 255, cv::NORM_MINMAX, CV_8U);    // Normalize new image
//     Logger::Log("Image normalized to 8-bit format.", LogLevel::INFO, DeviceType::GUIDER);

//     QString uniqueId = QUuid::createUuid().toString();
//     Logger::Log("Unique ID generated for new image.", LogLevel::INFO, DeviceType::GUIDER);

//     // 列出所有以"CaptureImage"为前缀的文件
//     QDir directory(QString::fromStdString(vueDirectoryPath));
//     QStringList filters;
//     filters << "CaptureImage*.jpg"; // 使用通配符来筛选以"CaptureImage"为前缀的jpg文件
//     QStringList fileList = directory.entryList(filters, QDir::Files);
//     Logger::Log("Existing image files listed for deletion.", LogLevel::INFO, DeviceType::GUIDER);

//     // 删除所有匹配的文件
//     for (const auto &file : fileList)
//     {
//         QString filePath = QString::fromStdString(vueDirectoryPath) + file;
//         QFile::remove(filePath);
//     }
//     Logger::Log("Old image files deleted.", LogLevel::INFO, DeviceType::GUIDER);

//     // 删除前一张图像文件
//     if (PriorROIImage != "NULL") {
//         QFile::remove(QString::fromStdString(PriorROIImage));
//         Logger::Log("Previous ROI image deleted.", LogLevel::INFO, DeviceType::GUIDER);
//     }

//     // 保存新的图像带有唯一ID的文件名
//     std::string fileName = "CaptureImage_" + uniqueId.toStdString() + ".jpg";
//     std::string filePath = vueDirectoryPath + fileName;
//     bool saved = cv::imwrite(filePath, SendImage);
//     Logger::Log("Attempt to save new image file.", LogLevel::INFO, DeviceType::GUIDER);

//     std::string Command = "sudo ln -sf " + filePath + " " + vueImagePath + fileName;
//     system(Command.c_str());
//     Logger::Log("Symbolic link created for new image file.", LogLevel::INFO, DeviceType::GUIDER);

//     PriorROIImage = vueImagePath + fileName;

//     if (saved)
//     {
//         emit wsThread->sendMessageToClient("SaveJpgSuccess:" + QString::fromStdString(fileName));

//         if(FWHM != -1){
//             dataPoints.append(QPointF(CurrentPosition, FWHM));

//             Logger::Log("dataPoints:" + std::to_string(CurrentPosition) + "," + std::to_string(FWHM), LogLevel::INFO, DeviceType::GUIDER);

//             float a, b, c;
//             Tools::fitQuadraticCurve(dataPoints, a, b, c);

//             if (dataPoints.size() >= 5) {
//                 QVector<QPointF> LineData;

//                 for (float x = CurrentPosition - 3000; x <= CurrentPosition + 3000; x += 10)
//                 {
//                     float y = a * x * x + b * x + c;
//                     LineData.append(QPointF(x, y));
//                 }

//                 // 计算导数为零的 x 坐标
//                 float x_min = -b / (2 * a);
//                 minPoint_X = x_min;
//                 // 计算最小值点的 y 坐标
//                 float y_min = a * x_min * x_min + b * x_min + c;

//                 QString dataString;
//                 for (const auto &point : LineData)
//                 {
//                     dataString += QString::number(point.x()) + "|" + QString::number(point.y()) + ":";
//                 }

//                 R2 = Tools::calculateRSquared(dataPoints, a, b, c);
//                 // qInfo() << "RSquared: " << R2;

//                 emit wsThread->sendMessageToClient("fitQuadraticCurve:" + dataString);
//                 emit wsThread->sendMessageToClient("fitQuadraticCurve_minPoint:" + QString::number(x_min) + ":" + QString::number(y_min));
//             }    
//         }
//     }
//     else
//     {
//         Logger::Log("Failed to save image.", LogLevel::ERROR, DeviceType::GUIDER);
//     }
// }
int a = 0;
int MainWindow::saveFitsAsPNG(QString fitsFileName, bool ProcessBin)
{
    // if (a == 0) {
    //     fitsFileName = "/home/quarcs/workspace/image/2025-03-20/2025_03_20T13_47_25_522.fits";
    //     a++;
    // }else if (a == 1) {
    //     fitsFileName = "/home/quarcs/workspace/image/2025-03-20/2025_03_20T13_43_18_926.fits";
    //     a++;
    // }else if (a == 2) {
    //     fitsFileName = "/home/quarcs/workspace/image/2025-03-20/2025_03_20T13_37_55_350.fits";
    //     a++;
    // }else if (a == 3) {
    //     fitsFileName = "/home/quarcs/workspace/image/2025-03-20/2025_03_20T13_36_54_070.fits";
    //     a=0;
    // }else{
    //     a = 0;
    //     fitsFileName = "/home/quarcs/workspace/image/2025-03-20/2025_03_20T13_47_25_522.fits";
    //     a++;
    // }
    // Add these lines to replace the original .fits file with ccd_simulator_original.fits


    Logger::Log("Starting to save FITS as PNG...", LogLevel::INFO, DeviceType::CAMERA);
    cv::Mat image;
    cv::Mat OriginImage;
    Logger::Log("FITS file path: " + fitsFileName.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    int status = Tools::readFits(fitsFileName.toLocal8Bit().constData(), image);

    if (status != 0)
    {
        Logger::Log("Failed to read FITS file: " + fitsFileName.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
        return status;
    }

    bool isColor = !(MainCameraCFA == "");
    Logger::Log("Camera color mode: " + std::string(isColor ? "Color" : "Monochrome"), LogLevel::INFO, DeviceType::CAMERA);

    if(ProcessBin && glMainCameraBinning != 1){
        // 进行 2x2 的合并
        image = Tools::processMatWithBinAvg(image, glMainCameraBinning, glMainCameraBinning, isColor, false);
        if (image.empty()) {
            Logger::Log("Failed to process image with binning.", LogLevel::ERROR, DeviceType::CAMERA);
            return -1;
        }
        Logger::Log("Image binned: new size - " + std::to_string(image.cols) + "x" + std::to_string(image.rows), LogLevel::INFO, DeviceType::CAMERA);
    }

    cv::Mat srcImage = image.clone();
    cv::Mat dstImage;
    Logger::Log("Starting median blur...", LogLevel::INFO, DeviceType::CAMERA);
    cv::medianBlur(srcImage, dstImage, 3);
    Logger::Log("Median blur applied successfully.", LogLevel::INFO, DeviceType::CAMERA);

    cv::imwrite(SolveImageFileName.toStdString(), dstImage);
    Logger::Log("Blurred image saved to " + SolveImageFileName.toStdString(), LogLevel::INFO, DeviceType::CAMERA);

    Tools::SaveMatToFITS(image);
    Logger::Log("Image saved as FITS.", LogLevel::INFO, DeviceType::CAMERA);
    
    int width = image.cols;
    int height = image.rows;
    Logger::Log("Image dimensions: " + std::to_string(width) + "x" + std::to_string(height), LogLevel::INFO, DeviceType::CAMERA);
    emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(width) + ":" + QString::number(height));
    if(ProcessBin) {
        emit wsThread->sendMessageToClient("MainCameraBinning:" + QString::number(glMainCameraBinning));
    } else {
        emit wsThread->sendMessageToClient("MainCameraBinning:" + QString::number(1));
    }
    

    std::vector<unsigned char> imageData;  //uint16_t
    imageData.assign(image.data, image.data + image.total() * image.channels() * 2);
    Logger::Log("Image data prepared for binary file.", LogLevel::INFO, DeviceType::CAMERA);
    
    QString uniqueId = QUuid::createUuid().toString();
    Logger::Log("Unique ID generated: " + uniqueId.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    
    QDir directory(QString::fromStdString(vueDirectoryPath));
    QStringList filters;
    filters << "CaptureImage*.bin";
    QStringList fileList = directory.entryList(filters, QDir::Files);
    Logger::Log("Existing binary files listed for deletion.", LogLevel::INFO, DeviceType::CAMERA);
    
    for (const auto &file : fileList)
    {
        QString filePath = QString::fromStdString(vueDirectoryPath) + file;
        QFile::remove(filePath);
    }
    Logger::Log("Old binary files deleted.", LogLevel::INFO, DeviceType::CAMERA);

    // 删除前一张图像文件
    if (PriorCaptureImage != "NULL") {
        QFile::remove(QString::fromStdString(PriorCaptureImage));
        Logger::Log("Previous capture image deleted.", LogLevel::INFO, DeviceType::CAMERA);
    }
    
    std::string fileName_ = "CaptureImage_" + uniqueId.toStdString() + ".bin";
    std::string filePath_ = vueDirectoryPath + fileName_;
    
    Logger::Log("Opening file for writing: " + filePath_, LogLevel::INFO, DeviceType::CAMERA);
    std::ofstream outFile(filePath_, std::ios::binary);
    if (!outFile) {
        Logger::Log("Failed to open file for writing.", LogLevel::ERROR, DeviceType::CAMERA);
        throw std::runtime_error("Failed to open file for writing.");
    }

    Logger::Log("Writing data to file...", LogLevel::INFO, DeviceType::CAMERA);
    outFile.write(reinterpret_cast<const char*>(imageData.data()), imageData.size());
    if (!outFile) {
        Logger::Log("Failed to write data to file.", LogLevel::ERROR, DeviceType::CAMERA);
        throw std::runtime_error("Failed to write data to file.");
    }

    outFile.close();
    if (!outFile) {
        Logger::Log("Failed to close the file properly.", LogLevel::ERROR, DeviceType::CAMERA);
        throw std::runtime_error("Failed to close the file properly.");
    }

    std::string Command = "sudo ln -sf " + filePath_ + " " + vueImagePath + fileName_;
    system(Command.c_str());
    Logger::Log("Symbolic link created for new image file.", LogLevel::INFO, DeviceType::CAMERA);

    PriorCaptureImage = vueImagePath + fileName_;
    emit wsThread->sendMessageToClient("SaveBinSuccess:" + QString::fromStdString(fileName_));
    isStagingImage = true;
    SavedImage = fileName_;
    Logger::Log("Binary image saved and client notified.", LogLevel::INFO, DeviceType::CAMERA);

    QList<FITSImage::Star> stars = Tools::FindStarsByStellarSolver(false, true);
    QString dataString;
    for (const auto &star : stars)
    {
        dataString += QString::number(star.x) + "|" + QString::number(star.y) + "|" + QString::number(star.HFR) + ":";
    }
    emit wsThread->sendMessageToClient("DetectedStars:" + dataString);
    Logger::Log("Star detection data sent to client.", LogLevel::INFO, DeviceType::CAMERA);
    if (!fitsFileName.contains("ccd_simulator_original.fits")) {
        QString destinationPath = "/dev/shm/ccd_simulator_original.fits";
        QFile destinationFile(destinationPath);
        if (destinationFile.exists()) {
            destinationFile.remove();
        }
        QFile::copy(fitsFileName, destinationPath);
    }
}

cv::Mat MainWindow::colorImage(cv::Mat img16)
{
    Logger::Log("Starting color image processing...", LogLevel::INFO, DeviceType::MAIN);
    // color camera, need to do debayer and color balance
    cv::Mat AWBImg16;
    cv::Mat AWBImg16color;
    cv::Mat AWBImg16mono;
    cv::Mat AWBImg8color;

    uint16_t B=0;
    uint16_t W=65535;

    AWBImg16.create(img16.rows, img16.cols, CV_16UC1);
    AWBImg16color.create(img16.rows, img16.cols, CV_16UC3);
    AWBImg16mono.create(img16.rows, img16.cols, CV_16UC1);
    AWBImg8color.create(img16.rows, img16.cols, CV_8UC3);

    Logger::Log("Matrices for image processing created.", LogLevel::INFO, DeviceType::MAIN);
    Tools::ImageSoftAWB(img16, AWBImg16, MainCameraCFA, ImageGainR, ImageGainB, 30); // image software Auto White Balance is done in RAW image.
    Logger::Log("Auto White Balance applied.", LogLevel::INFO, DeviceType::MAIN);
    cv::cvtColor(AWBImg16, AWBImg16color, CV_BayerRG2BGR);
    Logger::Log("Image converted from Bayer to BGR.", LogLevel::INFO, DeviceType::MAIN);

    cv::cvtColor(AWBImg16color, AWBImg16mono, cv::COLOR_BGR2GRAY);
    Logger::Log("Image converted to grayscale.", LogLevel::INFO, DeviceType::MAIN);

    if (AutoStretch == true)
    {
        Tools::GetAutoStretch(AWBImg16mono, 0, B, W);
        Logger::Log("Auto stretch applied.", LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        B = 0;
        W = 65535;
        Logger::Log("Auto stretch not applied, using default values.", LogLevel::INFO, DeviceType::MAIN);
    }
    Logger::Log("AutoStretch values: B=" + std::to_string(B) + ", W=" + std::to_string(W), LogLevel::INFO, DeviceType::MAIN);
    Tools::Bit16To8_Stretch(AWBImg16color, AWBImg8color, B, W);
    Logger::Log("Image stretched from 16-bit to 8-bit.", LogLevel::INFO, DeviceType::MAIN);

    AWBImg16.release();
    AWBImg16color.release();
    AWBImg16mono.release();
    AWBImg8color.release();
    Logger::Log("Temporary matrices released.", LogLevel::INFO, DeviceType::MAIN);

    return AWBImg16color;
}

void MainWindow::saveGuiderImageAsJPG(cv::Mat Image)
{
    Logger::Log("Starting to save guider image as JPG...", LogLevel::INFO, DeviceType::GUIDER);
    
    // 生成唯一ID
    QString uniqueId = QUuid::createUuid().toString();
    Logger::Log("Generated unique ID for new guider image: " + uniqueId.toStdString(), LogLevel::INFO, DeviceType::GUIDER);

    // 列出所有以"GuiderImage"为前缀的文件
    QDir directory(QString::fromStdString(vueDirectoryPath));
    QStringList filters;
    filters << "GuiderImage*.jpg"; // 使用通配符来筛选以"GuiderImage"为前缀的jpg文件
    QStringList fileList = directory.entryList(filters, QDir::Files);
    Logger::Log("Listed existing guider images for deletion.", LogLevel::INFO, DeviceType::GUIDER);

    // 删除所有匹配的文件
    for (const auto &file : fileList)
    {
        QString filePath = QString::fromStdString(vueDirectoryPath) + file;
        QFile::remove(filePath);
        Logger::Log("Deleted guider image file: " + filePath.toStdString(), LogLevel::INFO, DeviceType::GUIDER);
    }

    // 删除前一张图像文件
    if (PriorGuiderImage != "NULL") {
        QFile::remove(QString::fromStdString(PriorGuiderImage));
        Logger::Log("Deleted previous guider image file: " + PriorGuiderImage, LogLevel::INFO, DeviceType::GUIDER);
    }

    // 保存新的图像带有唯一ID的文件名
    std::string fileName = "GuiderImage_" + uniqueId.toStdString() + ".jpg";
    std::string filePath = vueDirectoryPath + fileName;
    bool saved = cv::imwrite(filePath, Image);
    Logger::Log("Attempted to save new guider image.", LogLevel::INFO, DeviceType::GUIDER);

    std::string Command = "sudo ln -sf " + filePath + " " + vueImagePath + fileName;
    system(Command.c_str());
    Logger::Log("Created symbolic link for new guider image.", LogLevel::INFO, DeviceType::GUIDER);

    PriorGuiderImage = vueImagePath + fileName;

    if (saved)
    {
        emit wsThread->sendMessageToClient("SaveGuiderImageSuccess:" + QString::fromStdString(fileName));
        Logger::Log("Guider image saved successfully and client notified.", LogLevel::INFO, DeviceType::GUIDER);
    }
    else
    {
        Logger::Log("Failed to save guider image.", LogLevel::ERROR, DeviceType::GUIDER);
    }
}

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

//"Telescopes"|"Focusers"|"CCDs"|"Spectrographs"|"Filter Wheels"|"Auxiliary"|"Domes"|"Weather"|"Agent"
void MainWindow::printDevGroups2(const DriversList drivers_list, int ListNum, QString group)
{
    Logger::Log("Printing device groups for group: " + group.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("=============================== Print DevGroups ===============================", LogLevel::INFO, DeviceType::MAIN);
    for (int i = 0; i < drivers_list.dev_groups.size(); i++)
    {
        if (drivers_list.dev_groups[i].group == group)
        {
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
    Logger::Log("Completed printing device groups.", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::DeviceSelect(int systemNumber, int grounpNumber)
{
    // Tools::clearSystemDeviceListItem(systemdevicelist, systemNumber);
    SelectIndiDevice(systemNumber, grounpNumber);
}

void MainWindow::SelectIndiDevice(int systemNumber, int grounpNumber)
{
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
        if (grounpNumber == 1 && (QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].label) == "QHY CCD" || QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].driver_name) == "indi_qhy_ccd")){
            continue;
        }
        if (grounpNumber == 20 && (QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].label) == "QHY CCD2" || QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].driver_name) == "indi_qhy_ccd2")){
            continue;
        }
        // qDebug() << QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].driver_name) << QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].version) << QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].label) << QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].manufacturer);
        emit wsThread->sendMessageToClient("AddDriver:" + QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].label) + ":" + QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].driver_name));
        // qDebug() << "AddDriver:" + QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].label) + ":" + QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].driver_name);
    }
}

bool MainWindow::indi_Driver_Confirm(QString DriverName, QString BaudRate)
{
    // bool isExist;
    // qDebug() << "call clearCheckDeviceExist:" << DriverName;
    // uint32_t ret = clearCheckDeviceExist(DriverName, isExist);

    // if (isExist == false)
    // {
    //     systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].DeviceIndiGroup = -1;
    //     systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].DeviceIndiName = "";
    //     systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].DriverFrom = "";
    //     systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].DriverIndiName = "";
    //     systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].isConnect = false;
    //     systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].dp = NULL;
    //     systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "";
    // }

    // qDebug() << "\033[0m\033[1;35m"
    // return isExist;
    switch (systemdevicelist.currentDeviceCode)
    {
    case 0:
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "Mount";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].BaudRate = BaudRate.toInt();
        // emit wsThread->sendMessageToClient("AddDeviceType:Mount");
        break;
    case 1:
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "Guider";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].BaudRate = BaudRate.toInt();
        // emit wsThread->sendMessageToClient("AddDeviceType:Guider");
        break;
    case 2:
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "PoleCamera";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].BaudRate = BaudRate.toInt();
        // emit wsThread->sendMessageToClient("AddDeviceType:PoleCamera");
        break;
    case 20:
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "MainCamera";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].BaudRate = BaudRate.toInt();
        // emit wsThread->sendMessageToClient("AddDeviceType:MainCamera");
        break;
    case 21:
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "CFW";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].BaudRate = BaudRate.toInt();
        // emit wsThread->sendMessageToClient("AddDeviceType:CFW");
        break;
    case 22:
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "Focuser";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].BaudRate = BaudRate.toInt();
        // emit wsThread->sendMessageToClient("AddDeviceType:Focuser");
        break;

    default:
        break;
    }

    systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].DriverIndiName = DriverName;
}

bool MainWindow::indi_Driver_Clear()
{
    systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "";
    systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].DriverIndiName = "";
    systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].BaudRate = 9600;
}

void MainWindow::indi_Device_Confirm(QString DeviceName, QString DriverName)
{
    //   qApp->processEvents();

    int deviceCode;
    deviceCode = systemdevicelist.currentDeviceCode;

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

void MainWindow::ConnectAllDeviceOnce() {
    Logger::Log("Connecting all devices once.", LogLevel::INFO, DeviceType::MAIN);
    dpMount = nullptr;
    dpGuider = nullptr;
    dpPoleScope = nullptr;
    dpMainCamera = nullptr;
    dpFocuser = nullptr;
    dpCFW = nullptr;
    
    int SelectedDriverNum = Tools::getDriverNumFromSystemDeviceList(systemdevicelist);
    if (SelectedDriverNum == 0)
    {
        Logger::Log("No driver in system device list.", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectFailed:No driver in system device list.");
        return;
    }

    // NumberOfTimesConnectDevice = 0;

    Tools::cleanSystemDeviceListConnect(systemdevicelist);
    disconnectIndiServer(indi_Client);
    Tools::stopIndiDriverAll(drivers_list);
    

    QString driverName;
    QString deviceType;
    QVector<QString> nameCheck;

    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        driverName = systemdevicelist.system_devices[i].DriverIndiName;
        deviceType = systemdevicelist.system_devices[i].Description;

        
        if (driverName != "")
        {
            bool isFound = false;
            for (auto item : nameCheck)
            {
                if ((item == driverName) || (item == "indi_qhy_ccd" && driverName == "indi_qhy_ccd2") || (item == "indi_qhy_ccd2" && driverName == "indi_qhy_ccd"))
                {
                    
                    isFound = true;
                    Logger::Log("Found one duplite driver,do not start it again: " + driverName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                    break;
                }
            }

            if (isFound == false)
            {
                Logger::Log("Start INDI Driver:" + driverName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                Tools::startIndiDriver(driverName);
                nameCheck.push_back(driverName);
                ConnectDriverList.push_back(driverName);
            }
        }
    }

    sleep(1);

    if (indi_Client->isServerConnected() == false)
    {
        Logger::Log("Can not find server.", LogLevel::ERROR, DeviceType::MAIN);
        connectIndiServer(indi_Client);
        sleep(1);
    }

    QTimer *timer = new QTimer(this);
    timer->setInterval(1000); // 设置定时器间隔为1000毫秒
    int time = 0;
    connect(timer, &QTimer::timeout, this, [this, timer, &time]() {
        if (indi_Client->GetDeviceCount() > 0 || time >= 10) {
            timer->stop();
            timer->deleteLater();
            sleep(2);
            continueConnectAllDeviceOnce(); // 继续执行设备连接的剩余部分
        } else {
            Logger::Log("Waiting for devices...", LogLevel::INFO, DeviceType::MAIN);
            time++;
        }
    });
    timer->start();
}

void MainWindow::continueConnectAllDeviceOnce() {
    if (indi_Client->GetDeviceCount() == 0) {
        Logger::Log("Driver start success but no device found", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectFailed:No device found.");
        Tools::stopIndiDriverAll(drivers_list);
        ConnectDriverList.clear();
        return;
    }

    for (int i = 0; i < indi_Client->GetDeviceCount(); i++)
    {
        Logger::Log("Start connecting devices:" + indi_Client->GetDeviceNameFromList(i), LogLevel::INFO, DeviceType::MAIN);
        indi_Client->setBaudRate(indi_Client->GetDeviceFromList(i), systemdevicelist.system_devices[i].BaudRate);
        indi_Client->connectDevice(indi_Client->GetDeviceNameFromList(i).c_str());

        int waitTime = 0;
        while (!indi_Client->GetDeviceFromList(i)->isConnected() && waitTime < 5) {
            Logger::Log("Wait for Connect" + indi_Client->GetDeviceNameFromList(i), LogLevel::INFO, DeviceType::MAIN);
            QThread::msleep(1000);  // 等待1秒
            waitTime++;
        }
        if (!indi_Client->GetDeviceFromList(i)->isConnected() && indi_Client->GetDeviceFromList(i)->getDriverInterface() & INDI::BaseDevice::FOCUSER_INTERFACE) {
            QString DevicePort;
            indi_Client->getDevicePort(indi_Client->GetDeviceFromList(i), DevicePort);
            // 检查指定的端口是否在/dev/serial/by-id目录下存在
            QFile portFile(DevicePort);
            if (!portFile.exists()) {
                Logger::Log("Port " + DevicePort.toStdString() + " does not exist.", LogLevel::ERROR, DeviceType::MAIN);
                // 获取所有连接的串口
                QStringList connectedPorts = getConnectedSerialPorts();
                for (int j = 0; j < connectedPorts.size(); j++) {

                    Logger::Log("Connected Ports:" + connectedPorts[j].toStdString(), LogLevel::INFO, DeviceType::MAIN);
                    if (connectedPorts[j].contains("ttyACM")) {
                        Logger::Log("Found ttyACM device:" + connectedPorts[j].toStdString(), LogLevel::INFO, DeviceType::MAIN);
                    }else {
                        Logger::Log("Not found ttyACM device", LogLevel::INFO, DeviceType::MAIN);
                        continue;
                    }
                    QStringList links = findLinkToTtyDevice("/dev", connectedPorts[j]);
                    QString link = "";
                    if (links.isEmpty()) {
                        Logger::Log("No link found for " + connectedPorts[j].toStdString(), LogLevel::INFO, DeviceType::MAIN);
                    } else {
                        for (int k = 0; k < links.size(); k++) {
                            if (areFilesInSameDirectory(links[k], DevicePort)) {
                                if (link == "") {
                                    link = links[k];
                                }else {
                                    Logger::Log("No Found only one link for " + connectedPorts[j].toStdString(), LogLevel::INFO, DeviceType::MAIN);
                                    link = "";
                                    break;
                                }
                            }
                        }
                        if (link == "") {
                            Logger::Log("No Found link for " + connectedPorts[j].toStdString(), LogLevel::INFO, DeviceType::MAIN);
                            continue;
                        }
                        Logger::Log("Link found for " + connectedPorts[j].toStdString() + ": " + link.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                        DevicePort = link;
                        indi_Client->setDevicePort(indi_Client->GetDeviceFromList(i), DevicePort);
                        indi_Client->setBaudRate(indi_Client->GetDeviceFromList(i), systemdevicelist.system_devices[i].BaudRate);
                        indi_Client->connectDevice(indi_Client->GetDeviceNameFromList(i).c_str());
                        waitTime = 0;
                        while (waitTime < 5)
                        {
                            Logger::Log("Wait for Connect" + indi_Client->GetDeviceNameFromList(i), LogLevel::INFO, DeviceType::MAIN);

                            QThread::msleep(1000); // 等待1秒
                            waitTime++;
                            if (indi_Client->GetDeviceFromList(i)->isConnected()) {
                                break;
                            }
                        }
                    }
        
                }
                
            }
        }
    }
    // sleep(10);

    ConnectedCCDList.clear();
    ConnectedTELESCOPEList.clear();
    ConnectedFOCUSERList.clear();
    ConnectedFILTERList.clear();
    for (int i = 0; i < indi_Client->GetDeviceCount(); i++)   //  indi_Client->GetDeviceFromList(i)
    {
        if(indi_Client->GetDeviceFromList(i)->isConnected()) {
            if(indi_Client->GetDeviceFromList(i)->getDriverInterface() & INDI::BaseDevice::CCD_INTERFACE) {
                Logger::Log("We received a CCD!", LogLevel::INFO, DeviceType::MAIN);
                ConnectedCCDList.push_back(i);
            } else if (indi_Client->GetDeviceFromList(i)->getDriverInterface() & INDI::BaseDevice::FILTER_INTERFACE) {
                Logger::Log("We received a FILTER!", LogLevel::INFO, DeviceType::MAIN);
                ConnectedFILTERList.push_back(i);
            } else if (indi_Client->GetDeviceFromList(i)->getDriverInterface() & INDI::BaseDevice::TELESCOPE_INTERFACE) {
                Logger::Log("We received a TELESCOPE!", LogLevel::INFO, DeviceType::MAIN);
                ConnectedTELESCOPEList.push_back(i);
            } else if (indi_Client->GetDeviceFromList(i)->getDriverInterface() & INDI::BaseDevice::FOCUSER_INTERFACE) {
                Logger::Log("We received a FOCUSER!", LogLevel::INFO, DeviceType::MAIN);
                ConnectedFOCUSERList.push_back(i);
            }  
            Logger::Log("Driver:" + std::string(indi_Client->GetDeviceFromList(i)->getDriverExec()) + " Device:" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()), LogLevel::INFO, DeviceType::MAIN);
        } else {
            QString DeviceName = indi_Client->GetDeviceFromList(i)->getDeviceName();
            Logger::Log("Connect failed device:" + DeviceName.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("ConnectFailed:Connect device failed:" + DeviceName);
        }
    }

    Tools::printSystemDeviceList(systemdevicelist);

    QStringList SelectedCameras = Tools::getCameraNumFromSystemDeviceList(systemdevicelist);
    Logger::Log("Number of Selected cameras:" + std::to_string(SelectedCameras.size()), LogLevel::INFO, DeviceType::MAIN);
    for(auto Camera: SelectedCameras){
        Logger::Log("Selected Cameras:" + Camera.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    }

    Logger::Log("Number of Connected CCD:" + std::to_string(ConnectedCCDList.size()), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("Number of Connected TELESCOPE:" + std::to_string(ConnectedTELESCOPEList.size()), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("Number of Connected FOCUSER:" + std::to_string(ConnectedFOCUSERList.size()), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("Number of Connected FILTER:" + std::to_string(ConnectedFILTERList.size()), LogLevel::INFO, DeviceType::MAIN);

    for (int i = 0; i < indi_Client->GetDeviceCount(); i++){
        if (indi_Client->GetDeviceFromList(i)->isConnected()) {
            for (int j = 0; j < systemdevicelist.system_devices.size(); j++) {
                if (systemdevicelist.system_devices[j].DriverIndiName == indi_Client->GetDeviceFromList(i)->getDriverExec() || (systemdevicelist.system_devices[j].DriverIndiName == "indi_qhy_ccd" && std::string(indi_Client->GetDeviceFromList(i)->getDriverExec()) == "indi_qhy_ccd2") || (systemdevicelist.system_devices[j].DriverIndiName == "indi_qhy_ccd2" && std::string(indi_Client->GetDeviceFromList(i)->getDriverExec()) == "indi_qhy_ccd")) {
                    emit wsThread->sendMessageToClient("AddDeviceType:" + systemdevicelist.system_devices[j].Description);
                }
            }
        }
    }

    if (ConnectedCCDList.size() <= 0 && ConnectedTELESCOPEList.size() <= 0 && ConnectedFOCUSERList.size() <= 0 && ConnectedFILTERList.size() <= 0) {
        Logger::Log("No Device Connected", LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectFailed:No Device Connected");
        Tools::stopIndiDriverAll(drivers_list);
        ConnectDriverList.clear();
        return;
    }

    bool EachDeviceOne = true;

    if(SelectedCameras.size() == 1 && ConnectedCCDList.size() == 1) {
        Logger::Log("The Camera Selected and Connected are Both 1", LogLevel::INFO, DeviceType::MAIN);
        if(SelectedCameras[0] == "Guider") {
            dpGuider = indi_Client->GetDeviceFromList(ConnectedCCDList[0]);
            systemdevicelist.system_devices[1].isConnect = true;
            indi_Client->disconnectDevice(indi_Client->GetDeviceFromList(ConnectedCCDList[0])->getDeviceName());
            sleep(1);
            call_phd_whichCamera(indi_Client->GetDeviceFromList(ConnectedCCDList[0])->getDeviceName());
            // PHD2 connect status
            AfterDeviceConnect(dpGuider);

        } else if(SelectedCameras[0] == "PoleCamera") {
            dpPoleScope = indi_Client->GetDeviceFromList(ConnectedCCDList[0]);
            systemdevicelist.system_devices[2].isConnect = true;
            AfterDeviceConnect(dpPoleScope);
        } else if(SelectedCameras[0] == "MainCamera") {
            dpMainCamera = indi_Client->GetDeviceFromList(ConnectedCCDList[0]);
            systemdevicelist.system_devices[20].isConnect = true;
            AfterDeviceConnect(dpMainCamera);
        } 
    } else if(SelectedCameras.size() > 1 || ConnectedCCDList.size() > 1) {
        EachDeviceOne = false;
        for(int i = 0; i < ConnectedCCDList.size(); i++) {
            emit wsThread->sendMessageToClient("DeviceToBeAllocated:CCD:" + QString::number(ConnectedCCDList[i]) + ":" + QString::fromUtf8(indi_Client->GetDeviceFromList(ConnectedCCDList[i])->getDeviceName()));       //already allocated
        }
    }

    if(ConnectedTELESCOPEList.size() == 1) {
        Logger::Log("Mount Connected Success and Mount device is only one!", LogLevel::INFO, DeviceType::MAIN);
        dpMount = indi_Client->GetDeviceFromList(ConnectedTELESCOPEList[0]);
        systemdevicelist.system_devices[0].isConnect = true;
        AfterDeviceConnect(dpMount);
    } else if(ConnectedTELESCOPEList.size() > 1) {
        EachDeviceOne = false;
        for(int i = 0; i < ConnectedTELESCOPEList.size(); i++) {
            emit wsThread->sendMessageToClient("DeviceToBeAllocated:Mount:" + QString::number(ConnectedTELESCOPEList[i]) + ":" + QString::fromUtf8(indi_Client->GetDeviceFromList(ConnectedTELESCOPEList[i])->getDeviceName()));
        }
    }

    if(ConnectedFOCUSERList.size() == 1) {
        Logger::Log("Focuser Connected Success and Focuser device is only one!", LogLevel::INFO, DeviceType::MAIN);
        dpFocuser = indi_Client->GetDeviceFromList(ConnectedFOCUSERList[0]);
        systemdevicelist.system_devices[22].isConnect = true;
        AfterDeviceConnect(dpFocuser);
    } else if(ConnectedFOCUSERList.size() > 1) {
        EachDeviceOne = false;
        for(int i = 0; i < ConnectedFOCUSERList.size(); i++) {
            emit wsThread->sendMessageToClient("DeviceToBeAllocated:Focuser:" + QString::number(ConnectedFOCUSERList[i]) + ":" + QString::fromUtf8(indi_Client->GetDeviceFromList(ConnectedFOCUSERList[i])->getDeviceName()));
        }
    }

    if(ConnectedFILTERList.size() == 1) {
        Logger::Log("Filter Connected Success and Filter device is only one!", LogLevel::INFO, DeviceType::MAIN);
        dpCFW = indi_Client->GetDeviceFromList(ConnectedFILTERList[0]);
        systemdevicelist.system_devices[21].isConnect = true;
        AfterDeviceConnect(dpCFW);
    } else if(ConnectedFILTERList.size() > 1) {
        EachDeviceOne = false;
        for(int i = 0; i < ConnectedFILTERList.size(); i++) {
            emit wsThread->sendMessageToClient("DeviceToBeAllocated:CFW:" + QString::number(ConnectedFILTERList[i]) + ":" + QString::fromUtf8(indi_Client->GetDeviceFromList(ConnectedFILTERList[i])->getDeviceName()));
        }
    }

    
    Logger::Log("Each Device Only Has One:" + std::to_string(EachDeviceOne), LogLevel::INFO, DeviceType::MAIN);
    if(EachDeviceOne) {
        // AfterDeviceConnect();
    } else {
        emit wsThread->sendMessageToClient("ShowDeviceAllocationWindow");
    }
}

void MainWindow::AutoConnectAllDevice() {
    dpMount = nullptr;
    dpGuider = nullptr;
    dpPoleScope = nullptr;
    dpMainCamera = nullptr;
    dpFocuser = nullptr;
    dpCFW = nullptr;

    SystemDeviceList newSystemdevicelist = Tools::readSystemDeviceList();
    if(newSystemdevicelist.system_devices.size() != systemdevicelist.system_devices.size()) {
        Logger::Log("No historical connection records found", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectFailed:No historical connection records found.");
        return;
    }
    systemdevicelist = newSystemdevicelist;
    
    int SelectedDriverNum = Tools::getDriverNumFromSystemDeviceList(systemdevicelist);
    if (SelectedDriverNum == 0)
    {
        Logger::Log("No driver in system device list", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectFailed:No driver in system device list.");
        return;
    }

    // NumberOfTimesConnectDevice = 0;

    Tools::cleanSystemDeviceListConnect(systemdevicelist);
    disconnectIndiServer(indi_Client);
    Tools::stopIndiDriverAll(drivers_list);

    QString driverName;
    QString deviceType;
    QVector<QString> nameCheck;

    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        driverName = systemdevicelist.system_devices[i].DriverIndiName;
        deviceType = systemdevicelist.system_devices[i].Description;
        // if (deviceType != ""){
        //     emit wsThread->sendMessageToClient("AddDeviceType:" + deviceType);
        // }
        
        if (driverName != "")
        {
            bool isFound = false;
            for (auto item : nameCheck)
            {
                if ((item == driverName) || (item == "indi_qhy_ccd" && driverName == "indi_qhy_ccd2") || (item == "indi_qhy_ccd2" && driverName == "indi_qhy_ccd"))
                {
                    isFound = true;
                    Logger::Log("found one duplite driver,do not start it again" + driverName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                    break;
                }
            }

            if (isFound == false)
            {
                Logger::Log("Start Connect INDI Driver:" + driverName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                Tools::startIndiDriver(driverName);
                nameCheck.push_back(driverName);
                sleep(1);
            }
            // Tools::startIndiDriver(driverName);
        }
    }

    sleep(1);

    if (indi_Client->isServerConnected() == false)
    {
        Logger::Log("can not find server", LogLevel::ERROR, DeviceType::MAIN);
        connectIndiServer(indi_Client);
        sleep(1);
    }

    QTimer *timer = new QTimer(this);
    timer->setInterval(1000); // 设置定时器间隔为1000毫秒
    int time = 0;
    connect(timer, &QTimer::timeout, this, [this, timer, &time]() {
        if (indi_Client->GetDeviceCount() > 0 || time >= 10) {
            timer->stop();
            timer->deleteLater();
            sleep(2);
            continueAutoConnectAllDevice(); // 继续执行设备连接的剩余部分
        } else {
            Logger::Log("Waiting for devices...", LogLevel::INFO, DeviceType::MAIN);
            time++;
        }
    });
    timer->start();
}
    
void MainWindow::continueAutoConnectAllDevice(){
    if (indi_Client->GetDeviceCount() == 0) {
        Logger::Log("Driver start success but no device found", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectFailed:No device found.");
        Tools::stopIndiDriverAll(drivers_list);
        ConnectDriverList.clear();
        return;
    }

    for (int i = 0; i < indi_Client->GetDeviceCount(); i++)
    {
        Logger::Log("Start connecting devices:" + QString::fromStdString(indi_Client->GetDeviceNameFromList(i)).toStdString(), LogLevel::INFO, DeviceType::MAIN);
        indi_Client->setBaudRate(indi_Client->GetDeviceFromList(i), systemdevicelist.system_devices[i].BaudRate);
        indi_Client->connectDevice(indi_Client->GetDeviceNameFromList(i).c_str());

        int waitTime = 0;
        while (!indi_Client->GetDeviceFromList(i)->isConnected() && waitTime < 5) {
            Logger::Log("Wait for Connect" + indi_Client->GetDeviceNameFromList(i), LogLevel::INFO, DeviceType::MAIN);
            QThread::msleep(1000);  // 等待1秒
            waitTime++;
        }
    }

    // sleep(5);
    Tools::printSystemDeviceList(systemdevicelist);

    for (int i = 0; i < indi_Client->GetDeviceCount(); i++)   //  indi_Client->GetDeviceFromList(i)
    {
        if(indi_Client->GetDeviceFromList(i)->isConnected()) {
            int index;
            uint32_t ret;
            ret = Tools::getIndexFromSystemDeviceListByName(systemdevicelist, QString::fromStdString(indi_Client->GetDeviceFromList(i)->getDeviceName()), index);
            if (ret == QHYCCD_SUCCESS)
            {
                systemdevicelist.system_devices[index].dp = indi_Client->GetDeviceFromList(i);
                systemdevicelist.system_devices[index].isConnect = true;
                
                if (index == 1)
                {
                    indi_Client->disconnectDevice(indi_Client->GetDeviceFromList(i)->getDeviceName());
                    sleep(1);
                    call_phd_whichCamera(systemdevicelist.system_devices[index].dp->getDeviceName());  // PHD2 Guider Connect
                }
            }
        } else {
            QString DeviceName = indi_Client->GetDeviceFromList(i)->getDeviceName();
            Logger::Log("Connect failed device:" + DeviceName.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("ConnectFailed:Connect device failed:" + DeviceName);
        }
    }

    for (int i = 0; i < indi_Client->GetDeviceCount(); i++){
        if (indi_Client->GetDeviceFromList(i)->isConnected()) {
            for (int j = 0; j < systemdevicelist.system_devices.size(); j++) {
                if (systemdevicelist.system_devices[j].DriverIndiName == indi_Client->GetDeviceFromList(i)->getDriverExec()) {
                    emit wsThread->sendMessageToClient("AddDeviceType:" + systemdevicelist.system_devices[j].Description);
                }
            }
        }
    }

    Tools::printSystemDeviceList(systemdevicelist);

    if (systemdevicelist.system_devices[0].dp != NULL){
        Logger::Log("Find dpMount", LogLevel::INFO, DeviceType::MAIN);
        dpMount = systemdevicelist.system_devices[0].dp;
    } 
    if (systemdevicelist.system_devices[1].dp != NULL){
        Logger::Log("Find dpGuider", LogLevel::INFO, DeviceType::MAIN);
        dpGuider = systemdevicelist.system_devices[1].dp;
    }  
    if (systemdevicelist.system_devices[2].dp != NULL){
        Logger::Log("Find dpPoleScope", LogLevel::INFO, DeviceType::MAIN);
        dpPoleScope = systemdevicelist.system_devices[2].dp;
    }   
    if (systemdevicelist.system_devices[20].dp != NULL){
        Logger::Log("Find dpMainCamera", LogLevel::INFO, DeviceType::MAIN);
        dpMainCamera = systemdevicelist.system_devices[20].dp;
    } 
    if (systemdevicelist.system_devices[21].dp != NULL){
        Logger::Log("Find dpCFW", LogLevel::INFO, DeviceType::MAIN);
        dpCFW = systemdevicelist.system_devices[21].dp;
    }
    if (systemdevicelist.system_devices[22].dp != NULL){
        Logger::Log("Find dpFocuser", LogLevel::INFO, DeviceType::MAIN);
        dpFocuser = systemdevicelist.system_devices[22].dp;
    }

    if (dpFocuser == nullptr && dpMount == nullptr && dpMainCamera == nullptr && dpGuider == nullptr && dpPoleScope == nullptr && dpCFW == nullptr) {
        Logger::Log("all device connect failed", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectFailed:all device connect failed.");
        return;
    }
    AfterDeviceConnect();

}

void MainWindow::BindingDevice(QString DeviceType, int DeviceIndex)
{
    indi_Client->PrintDevices();
    Logger::Log("BindingDevice:" + DeviceType.toStdString() + ":" + QString::number(DeviceIndex).toStdString(), LogLevel::INFO, DeviceType::MAIN);

    if (DeviceType == "Guider")
    {
        Logger::Log("Binding Guider Device start ...", LogLevel::INFO, DeviceType::MAIN);
        dpGuider = indi_Client->GetDeviceFromList(DeviceIndex);
        indi_Client->disconnectDevice(indi_Client->GetDeviceFromList(DeviceIndex)->getDeviceName());
        Logger::Log("Disconnect Guider Device", LogLevel::INFO, DeviceType::MAIN);
        sleep(1);
        call_phd_whichCamera(indi_Client->GetDeviceFromList(DeviceIndex)->getDeviceName());
        Logger::Log("Call PHD2 Guider Connect", LogLevel::INFO, DeviceType::MAIN);
        systemdevicelist.system_devices[1].isConnect = true;
        systemdevicelist.system_devices[1].isBind = true;
        AfterDeviceConnect(dpGuider);
        Logger::Log("Binding Guider Device end !", LogLevel::INFO, DeviceType::MAIN);
    }
    else if (DeviceType == "MainCamera")
    {
        Logger::Log("Binding MainCamera Device start ...", LogLevel::INFO, DeviceType::MAIN);
        dpMainCamera = indi_Client->GetDeviceFromList(DeviceIndex);
        systemdevicelist.system_devices[20].isConnect = true;
        systemdevicelist.system_devices[20].isBind = true;
        AfterDeviceConnect(dpMainCamera);
        Logger::Log("Binding MainCamera Device end !", LogLevel::INFO, DeviceType::MAIN);
    }
    else if (DeviceType == "Mount")
    {
        Logger::Log("Binding Mount Device start ...", LogLevel::INFO, DeviceType::MAIN);
        dpMount = indi_Client->GetDeviceFromList(DeviceIndex);
        systemdevicelist.system_devices[0].isConnect = true;
        systemdevicelist.system_devices[0].isBind = true;
        AfterDeviceConnect(dpMount);
        Logger::Log("Binding Mount Device end !", LogLevel::INFO, DeviceType::MAIN);
    }
    else if (DeviceType == "Focuser")
    {
        Logger::Log("Binding Focuser Device start ...", LogLevel::INFO, DeviceType::MAIN);
        dpFocuser = indi_Client->GetDeviceFromList(DeviceIndex);
        systemdevicelist.system_devices[22].isConnect = true;
        systemdevicelist.system_devices[22].isBind = true;
        AfterDeviceConnect(dpFocuser);
        Logger::Log("Binding Focuser Device end !", LogLevel::INFO, DeviceType::MAIN);
    }
    else if (DeviceType == "PoleCamera")
    {
        Logger::Log("Binding PoleCamera Device start ...", LogLevel::INFO, DeviceType::MAIN);
        dpPoleScope = indi_Client->GetDeviceFromList(DeviceIndex);
        systemdevicelist.system_devices[2].isConnect = true;
        systemdevicelist.system_devices[2].isBind = true;
        AfterDeviceConnect(dpPoleScope);
        Logger::Log("Binding PoleCamera Device end !", LogLevel::INFO, DeviceType::MAIN);
    }
    else if (DeviceType == "CFW")
    {
        Logger::Log("Binding CFW Device start ...", LogLevel::INFO, DeviceType::MAIN);
        dpCFW = indi_Client->GetDeviceFromList(DeviceIndex);
        systemdevicelist.system_devices[21].isConnect = true;
        systemdevicelist.system_devices[21].isBind = true;
        AfterDeviceConnect(dpCFW);
        Logger::Log("Binding CFW Device end !", LogLevel::INFO, DeviceType::MAIN);
    }
}

void MainWindow::UnBindingDevice(QString DeviceType)
{
    indi_Client->PrintDevices();
    Logger::Log("UnBindingDevice:" + DeviceType.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    if (DeviceType == "Guider")
    {
        Logger::Log("UnBinding Guider Device start ...", LogLevel::INFO, DeviceType::MAIN);
        indi_Client->disconnectDevice(dpGuider->getDeviceName());
        Logger::Log("Disconnect Guider Device", LogLevel::INFO, DeviceType::MAIN);
        sleep(1);
        indi_Client->setBaudRate(dpGuider, systemdevicelist.system_devices[1].BaudRate);
        indi_Client->connectDevice(dpGuider->getDeviceName());
        Logger::Log("Connect Guider Device", LogLevel::INFO, DeviceType::MAIN);
        sleep(3);
        int DeviceIndex;
        for (int i = 0; i < indi_Client->GetDeviceCount(); i++)   //  indi_Client->GetDeviceFromList(i)
        {
            if(indi_Client->GetDeviceFromList(i)->getDeviceName() == dpGuider->getDeviceName()) {
                DeviceIndex = i;
            }
        }
        systemdevicelist.system_devices[1].isBind = false;
        systemdevicelist.system_devices[1].DeviceIndiName = "";
        dpGuider = nullptr;
        Logger::Log("UnBinding Guider Device end !", LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("DeviceToBeAllocated:CCD:" + QString::number(DeviceIndex) + ":" + QString::fromUtf8(indi_Client->GetDeviceFromList(DeviceIndex)->getDeviceName()));
    }
    else if (DeviceType == "MainCamera")
    {
        Logger::Log("UnBinding MainCamera Device(" + std::string(dpMainCamera->getDeviceName()) + ") start ...", LogLevel::INFO, DeviceType::MAIN);
        
        int DeviceIndex;
        for (int i = 0; i < indi_Client->GetDeviceCount(); i++)   //  indi_Client->GetDeviceFromList(i)
        {
            if(indi_Client->GetDeviceFromList(i)->getDeviceName() == dpMainCamera->getDeviceName()) {
                DeviceIndex = i;
            }
        }

        QString CFWname;
        indi_Client->getCFWSlotName(dpMainCamera, CFWname);
        if(CFWname != "") {
            emit wsThread->sendMessageToClient("deleteDeviceTypeAllocationList:CFW");
        }
        systemdevicelist.system_devices[20].isBind = false;
        systemdevicelist.system_devices[20].DeviceIndiName = "";
        dpMainCamera = nullptr;


        emit wsThread->sendMessageToClient("DeviceToBeAllocated:CCD:" + QString::number(DeviceIndex) + ":" + QString::fromUtf8(indi_Client->GetDeviceFromList(DeviceIndex)->getDeviceName()));       //already allocated
    }
    else if (DeviceType == "Mount")
    {
        int DeviceIndex;
        for (int i = 0; i < indi_Client->GetDeviceCount(); i++)   //  indi_Client->GetDeviceFromList(i)
        {
            if(indi_Client->GetDeviceFromList(i)->getDeviceName() == dpMount->getDeviceName()) {
                DeviceIndex = i;
            }
        }
        systemdevicelist.system_devices[0].isBind = false;
        systemdevicelist.system_devices[0].DeviceIndiName = "";
        dpMount = nullptr;

        emit wsThread->sendMessageToClient("DeviceToBeAllocated:Mount:" + QString::number(DeviceIndex) + ":" + QString::fromUtf8(indi_Client->GetDeviceFromList(DeviceIndex)->getDeviceName()));
    }
    else if (DeviceType == "Focuser")
    {
        int DeviceIndex;
        for (int i = 0; i < indi_Client->GetDeviceCount(); i++)   //  indi_Client->GetDeviceFromList(i)
        {
            if(indi_Client->GetDeviceFromList(i)->getDeviceName() == dpFocuser->getDeviceName()) {
                DeviceIndex = i;
            }
        }
        systemdevicelist.system_devices[22].isBind = false;
        systemdevicelist.system_devices[22].DeviceIndiName = "";
        dpFocuser = nullptr;

        emit wsThread->sendMessageToClient("DeviceToBeAllocated:Focuser:" + QString::number(DeviceIndex) + ":" + QString::fromUtf8(indi_Client->GetDeviceFromList(DeviceIndex)->getDeviceName()));
    }
    else if (DeviceType == "PoleCamera")
    {
        int DeviceIndex;
        for (int i = 0; i < indi_Client->GetDeviceCount(); i++)   //  indi_Client->GetDeviceFromList(i)
        {
            if(indi_Client->GetDeviceFromList(i)->getDeviceName() == dpPoleScope->getDeviceName()) {
                DeviceIndex = i;
            }
        }
        systemdevicelist.system_devices[2].isBind = false;
        systemdevicelist.system_devices[2].DeviceIndiName = "";
        dpPoleScope = nullptr;

        emit wsThread->sendMessageToClient("DeviceToBeAllocated:CCD:" + QString::number(DeviceIndex) + ":" + QString::fromUtf8(indi_Client->GetDeviceFromList(DeviceIndex)->getDeviceName()));
    }
    else if (DeviceType == "CFW")
    {
        int DeviceIndex;
        for (int i = 0; i < indi_Client->GetDeviceCount(); i++)   //  indi_Client->GetDeviceFromList(i)
        {
            if(indi_Client->GetDeviceFromList(i)->getDeviceName() == dpCFW->getDeviceName()) {
                DeviceIndex = i;
            }
        }
        systemdevicelist.system_devices[21].isBind = false;
        systemdevicelist.system_devices[21].DeviceIndiName = "";
        dpCFW = nullptr;

        emit wsThread->sendMessageToClient("DeviceToBeAllocated:CFW:" + QString::number(DeviceIndex) + ":" + QString::fromUtf8(indi_Client->GetDeviceFromList(DeviceIndex)->getDeviceName()));
    }

    indi_Client->PrintDevices();
}

// void MainWindow::DeviceConnect()
// {
//     int SelectedDevicesNum = Tools::getTotalDeviceFromSystemDeviceList(systemdevicelist);
//     if (SelectedDevicesNum == 0)
//     {
//         qWarning() << "System Connect | Error: no device in system device list";
//         emit wsThread->sendMessageToClient("ConnectFailed:no device in system device list.");
//         return;
//     }

//     Tools::cleanSystemDeviceListConnect(systemdevicelist);
//     Tools::printSystemDeviceList(systemdevicelist);
//     disconnectIndiServer(indi_Client);
//     Tools::stopIndiDriverAll(drivers_list);

//     QString driverName;
//     QVector<QString> nameCheck;

//     for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
//     {
//         driverName = systemdevicelist.system_devices[i].DriverIndiName;
//         if (driverName != "")
//         {
//             bool isFound = false;
//             for (auto item : nameCheck)
//             {
//                 if (item == driverName)
//                 {
//                     isFound = true;
//                     qWarning() << "System Connect | found one duplite driver,do not start it again" << driverName;
//                     break;
//                 }
//             }

//             if (isFound == false)
//             {
//                 Tools::startIndiDriver(driverName);
//                 nameCheck.push_back(driverName);
//             }
//         }
//     }

//     sleep(1);

//     connectIndiServer(indi_Client);

//     if (indi_Client->isServerConnected() == false)
//     {
//         qDebug() << "System Connect | ERROR:can not find server";
//         return;
//     }

//     if (indi_Client->GetDeviceCount() == 0)
//     {
//         qDebug() << "System Connect | Error:No device found";
//         emit wsThread->sendMessageToClient("ConnectFailed:No device found.");
//         return;
//     }

//     uint32_t ret;
//     int index;
//     for (int i = 0; i < indi_Client->GetDeviceCount(); i++)
//     {
//         qDebug() << "Start connecting devices:" << QString::fromStdString(indi_Client->GetDeviceNameFromList(i));
//         ret = Tools::getIndexFromSystemDeviceListByName(systemdevicelist, QString::fromStdString(indi_Client->GetDeviceNameFromList(i)), index);
//         if (ret == QHYCCD_SUCCESS)
//         {
//             systemdevicelist.system_devices[index].dp = indi_Client->GetDeviceFromList(i);

//             if (index == 1)
//             {
//                 call_phd_whichCamera(systemdevicelist.system_devices[index].dp->getDeviceName());  // PHD2 Guider Connect
//             }
//             else
//             {
//                 indi_Client->connectDevice(systemdevicelist.system_devices[index].dp->getDeviceName());
//             }
//         }
//     }

//     sleep(3);

//     for (int i = 0; i < indi_Client->GetDeviceCount(); i++)
//     {
//         ret = Tools::getIndexFromSystemDeviceListByName(systemdevicelist, QString::fromStdString(indi_Client->GetDeviceNameFromList(i)), index);
//         if (ret == QHYCCD_SUCCESS)
//         {
//             if (systemdevicelist.system_devices[index].dp->isConnected() == true)
//             {
//                 systemdevicelist.system_devices[index].isConnect = true;

//                 if(systemdevicelist.system_devices[index].dp->getDriverInterface() & INDI::BaseDevice::CCD_INTERFACE) {
//                     qDebug() << "\033[1;32m We received a CCD!\033[0m" << systemdevicelist.system_devices[index].dp->getDriverExec();
//                 } else if (systemdevicelist.system_devices[index].dp->getDriverInterface() & INDI::BaseDevice::FILTER_INTERFACE) {
//                     qDebug() << "\033[1;32m We received a FILTER!\033[0m" << systemdevicelist.system_devices[index].dp->getDriverExec();
//                 } else if (systemdevicelist.system_devices[index].dp->getDriverInterface() & INDI::BaseDevice::TELESCOPE_INTERFACE) {
//                     qDebug() << "\033[1;32m We received a TELESCOPE!\033[0m" << systemdevicelist.system_devices[index].dp->getDriverExec();
//                 } else if (systemdevicelist.system_devices[index].dp->getDriverInterface() & INDI::BaseDevice::FOCUSER_INTERFACE) {
//                     qDebug() << "\033[1;32m We received a FOCUSER!\033[0m" << systemdevicelist.system_devices[index].dp->getDriverExec();
//                 }
//             }

//             if (index == 1)
//             {
//                 if (indi_Client->GetDeviceNameFromList(i).find("Simulator") != std::string::npos)
//                 {
//                     systemdevicelist.system_devices[index].isConnect = true;
//                 }
//             }
//         }
//         else
//         {
//             qDebug() << "System Connect |Warn:" << QString::fromStdString(indi_Client->GetDeviceNameFromList(i)) << "is found in the client list but not in pre-select system list" << i;
//         }
//     }

    
//     AfterDeviceConnect();
// }

void MainWindow::AfterDeviceConnect()
{
    Logger::Log("Starting AfterDeviceConnect process.", LogLevel::INFO, DeviceType::MAIN);

    if (dpMainCamera != NULL)
    {
        Logger::Log("MainCamera connected after Device("+QString::fromUtf8(dpMainCamera->getDeviceName()).toStdString()+") Connect: " + QString::fromUtf8(dpMainCamera->getDeviceName()).toStdString(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectSuccess:MainCamera:" + QString::fromUtf8(dpMainCamera->getDeviceName()) + ":" + QString::fromUtf8(dpMainCamera->getDriverExec()));
        ConnectedDevices.push_back({"MainCamera", QString::fromUtf8(dpMainCamera->getDeviceName())});

        systemdevicelist.system_devices[20].DeviceIndiName = QString::fromUtf8(dpMainCamera->getDeviceName());
        systemdevicelist.system_devices[20].isBind = true;

        indi_Client->setBLOBMode(B_ALSO, dpMainCamera->getDeviceName(), nullptr);
        indi_Client->enableDirectBlobAccess(dpMainCamera->getDeviceName(), nullptr);

        QString SDKVERSION;
        indi_Client->getCCDSDKVersion(dpMainCamera, SDKVERSION);
        Logger::Log("MainCamera SDK version: " + SDKVERSION.toStdString(), LogLevel::INFO, DeviceType::MAIN);

        indi_Client->getCCDOffset(dpMainCamera, glOffsetValue, glOffsetMin, glOffsetMax);
        Logger::Log("CCD Offset - Value: " + std::to_string(glOffsetValue) + ", Min: " + std::to_string(glOffsetMin) + ", Max: " + std::to_string(glOffsetMax), LogLevel::INFO, DeviceType::MAIN);

        indi_Client->getCCDGain(dpMainCamera, glGainValue, glGainMin, glGainMax);
        Logger::Log("CCD Gain - Value: " + std::to_string(glGainValue) + ", Min: " + std::to_string(glGainMin) + ", Max: " + std::to_string(glGainMax), LogLevel::INFO, DeviceType::MAIN);

        int maxX, maxY; 
        double pixelsize, pixelsizX, pixelsizY;
        int bitDepth;
        indi_Client->getCCDBasicInfo(dpMainCamera, maxX, maxY, pixelsize, pixelsizX, pixelsizY, bitDepth);
        Logger::Log("CCD Basic Info - MaxX: " + std::to_string(maxX) + ", MaxY: " + std::to_string(maxY) + ", PixelSize: " + std::to_string(pixelsize), LogLevel::INFO, DeviceType::MAIN);

        if(maxX == 0 && maxY == 0) {
            QString CameraName = dpMainCamera->getDeviceName();
            Logger::Log("This may be a DSLRs Camera, need to set Resolution and pixel size. Camera: " + CameraName.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
            DSLRsInfo DSLRsInfo = Tools::readDSLRsInfo(CameraName);
            if(DSLRsInfo.Name == CameraName && DSLRsInfo.SizeX != 0 && DSLRsInfo.SizeY != 0 && DSLRsInfo.PixelSize != 0) {
                indi_Client->setCCDBasicInfo(dpMainCamera, DSLRsInfo.SizeX, DSLRsInfo.SizeY, DSLRsInfo.PixelSize, DSLRsInfo.PixelSize, DSLRsInfo.PixelSize, 8);
                indi_Client->getCCDBasicInfo(dpMainCamera, maxX, maxY, pixelsize, pixelsizX, pixelsizY, bitDepth);
                Logger::Log("Updated CCD Basic Info for DSLRs Camera.", LogLevel::INFO, DeviceType::MAIN);
            } else {
                emit wsThread->sendMessageToClient("DSLRsSetup:" + QString::fromUtf8(dpMainCamera->getDeviceName()));
            }
        }

        glCameraSize_width = maxX * pixelsize / 1000;
        glCameraSize_width = std::round(glCameraSize_width * 10) / 10;
        glCameraSize_height = maxY * pixelsize / 1000;
        glCameraSize_height = std::round(glCameraSize_height * 10) / 10;
        Logger::Log("CCD Chip size - Width: " + std::to_string(glCameraSize_width) + ", Height: " + std::to_string(glCameraSize_height), LogLevel::INFO, DeviceType::MAIN);

        int X, Y;
        indi_Client->getCCDFrameInfo(dpMainCamera, X, Y, glMainCCDSizeX, glMainCCDSizeY);
        Logger::Log("CCD Frame Info - SizeX: " + std::to_string(glMainCCDSizeX) + ", SizeY: " + std::to_string(glMainCCDSizeY), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));

        int offsetX, offsetY;
        indi_Client->getCCDCFA(dpMainCamera, offsetX, offsetY, MainCameraCFA);
        Logger::Log("CCD CFA Info - OffsetX: " + std::to_string(offsetX) + ", OffsetY: " + std::to_string(offsetY) + ", CFA: " + MainCameraCFA.toStdString(), LogLevel::INFO, DeviceType::MAIN);

        indi_Client->setCCDUploadModeToLacal(dpMainCamera);
        indi_Client->setCCDUpload(dpMainCamera, "/dev/shm", "ccd_simulator");

        QString CFWname;
        indi_Client->getCFWSlotName(dpMainCamera, CFWname);
        if(CFWname != "") {
            Logger::Log("CFW Slot Name: " + CFWname.toStdString(), LogLevel::INFO, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("ConnectSuccess:CFW:" + CFWname + " (on camera):" + QString::fromUtf8(dpMainCamera->getDriverExec()));
            isFilterOnCamera = true;

            int min, max, pos;
            indi_Client->getCFWPosition(dpMainCamera, pos, min, max);
            Logger::Log("CFW Position - Min: " + std::to_string(min) + ", Max: " + std::to_string(max) + ", Current: " + std::to_string(pos), LogLevel::INFO, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CFWPositionMax:" + QString::number(max));
            Logger::Log("CFW connected successfully.", LogLevel::INFO, DeviceType::MAIN);
        }
        Logger::Log("MainCamera connected successfully.", LogLevel::INFO, DeviceType::MAIN);
    }

    if (dpMount != NULL)
    {
        Logger::Log("Mount connected after Device("+QString::fromUtf8(dpMount->getDeviceName()).toStdString()+") Connect: " + QString::fromUtf8(dpMount->getDeviceName()).toStdString(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectSuccess:Mount:" + QString::fromUtf8(dpMount->getDeviceName()) + ":" + QString::fromUtf8(dpMount->getDriverExec()));
        ConnectedDevices.push_back({"Mount", QString::fromUtf8(dpMount->getDeviceName())});
        
        systemdevicelist.system_devices[0].DeviceIndiName = QString::fromUtf8(dpMount->getDeviceName());
        systemdevicelist.system_devices[0].isBind = true;


        indi_Client->GetAllPropertyName(dpMount);
        QString DevicePort;
        indi_Client->getDevicePort(dpMount, DevicePort);
        Logger::Log("Device port for Mount: " + DevicePort.toStdString(), LogLevel::INFO, DeviceType::MAIN);

        double glLongitude_radian, glLatitude_radian;
        glLongitude_radian = Tools::getDecAngle("116° 14' 53.91");
        glLatitude_radian = Tools::getDecAngle("40° 09' 14.93");
        Logger::Log("Mount location set to Longitude: " + QString::number(Tools::RadToDegree(glLongitude_radian)).toStdString() + ", Latitude: " + QString::number(Tools::RadToDegree(glLatitude_radian)).toStdString(), LogLevel::INFO, DeviceType::MAIN);

        indi_Client->setLocation(dpMount, Tools::RadToDegree(glLatitude_radian), Tools::RadToDegree(glLongitude_radian), 10);
        QDateTime datetime = QDateTime::currentDateTime();
        indi_Client->setTimeUTC(dpMount, datetime);
        Logger::Log("UTC Time set for Mount: " + datetime.toString(Qt::ISODate).toStdString(), LogLevel::INFO, DeviceType::MAIN);
        indi_Client->getTimeUTC(dpMount, datetime);
        Logger::Log("UTC Time: " + datetime.currentDateTimeUtc().toString(Qt::ISODate).toStdString(), LogLevel::INFO, DeviceType::MAIN);

        double a, b, c, d;
        indi_Client->getTelescopeInfo(dpMount, a, b, c, d);
        Logger::Log("Telescope Info - A: " + std::to_string(a) + ", B: " + std::to_string(b) + ", C: " + std::to_string(c) + ", D: " + std::to_string(d), LogLevel::INFO, DeviceType::MAIN);

        indi_Client->getTelescopeRADECJ2000(dpMount, a, b);
        indi_Client->getTelescopeRADECJNOW(dpMount, a, b);

        bool isPark;
        indi_Client->getTelescopePark(dpMount, isPark);
        Logger::Log("Telescope Park Status: " + std::string(isPark ? "Parked" : "Not Parked"), LogLevel::INFO, DeviceType::MAIN);

        int maxspeed, minspeed, speedvalue, total;
        indi_Client->getTelescopeTotalSlewRate(dpMount, total);
        glTelescopeTotalSlewRate = total;
        Logger::Log("Telescope Total Slew Rate: " + std::to_string(total), LogLevel::INFO, DeviceType::MAIN);

        emit wsThread->sendMessageToClient("TelescopeTotalSlewRate:" + QString::number(total));
        indi_Client->getTelescopeMaxSlewRateOptions(dpMount, minspeed, maxspeed, speedvalue);
        indi_Client->setTelescopeSlewRate(dpMount, total);
        int speed;
        indi_Client->getTelescopeSlewRate(dpMount,speed);
        Logger::Log("Current Telescope Slew Rate: " + std::to_string(speed), LogLevel::INFO, DeviceType::MAIN);
        // emit wsThread->sendMessageToClient("TelescopeCurrentSlewRate:" + QString::number(speed));
        emit wsThread->sendMessageToClient("MountSetSpeedSuccess:" + QString::number(speed));
        indi_Client->setTelescopeTrackEnable(dpMount, true);

        bool isTrack = false;
        indi_Client->getTelescopeTrackEnable(dpMount, isTrack);

        if (isTrack)
        {
            emit wsThread->sendMessageToClient("TelescopeTrack:ON");
        }
        else
        {
            emit wsThread->sendMessageToClient("TelescopeTrack:OFF");
        }
        Logger::Log("Telescope Tracking Status: " + std::string(isTrack ? "Enabled" : "Disabled"), LogLevel::INFO, DeviceType::MAIN);

        indi_Client->setTelescopeTrackRate(dpMount, "SIDEREAL");
        QString side;
        indi_Client->getTelescopePierSide(dpMount, side);
        Logger::Log("Telescope Pier Side: " + side.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("TelescopePierSide:" + side);
        Logger::Log("Mount connected successfully.", LogLevel::INFO, DeviceType::MAIN);
    }

    if (dpFocuser != NULL)
    {
        Logger::Log("Focuser connected after Device(" + QString::fromUtf8(dpFocuser->getDeviceName()).toStdString() + ") Connect: " + dpFocuser->getDeviceName(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectSuccess:Focuser:" + QString::fromUtf8(dpFocuser->getDeviceName()) + ":" + QString::fromUtf8(dpFocuser->getDriverExec()));
        ConnectedDevices.push_back({"Focuser", QString::fromUtf8(dpFocuser->getDeviceName())});

        systemdevicelist.system_devices[22].DeviceIndiName = QString::fromUtf8(dpFocuser->getDeviceName());
        systemdevicelist.system_devices[22].isBind = true;
        
        indi_Client->GetAllPropertyName(dpFocuser);
        // indi_Client->syncFocuserPosition(dpFocuser, 0);
        CurrentPosition = FocuserControl_getPosition();
        Logger::Log("Focuser Current Position: " + std::to_string(CurrentPosition), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
        Logger::Log("Focuser connected successfully.", LogLevel::INFO, DeviceType::MAIN);
    }

    if (dpCFW != NULL)
    {
        Logger::Log("CFW connected after Device("+QString::fromUtf8(dpCFW->getDeviceName()).toStdString()+") Connect: " + dpCFW->getDeviceName(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectSuccess:CFW:" + QString::fromUtf8(dpCFW->getDeviceName()) + ":" + QString::fromUtf8(dpCFW->getDriverExec()));
        ConnectedDevices.push_back({"CFW", QString::fromUtf8(dpCFW->getDeviceName())});

        systemdevicelist.system_devices[21].DeviceIndiName = QString::fromUtf8(dpCFW->getDeviceName());
        systemdevicelist.system_devices[21].isBind = true;

        indi_Client->GetAllPropertyName(dpCFW);
        int min, max, pos;
        indi_Client->getCFWPosition(dpCFW, pos, min, max);
        Logger::Log("CFW Position - Min: " + std::to_string(min) + ", Max: " + std::to_string(max) + ", Current: " + std::to_string(pos), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("CFWPositionMax:" + QString::number(max));
        Logger::Log("CFW connected successfully.", LogLevel::INFO, DeviceType::MAIN);
        if(Tools::readCFWList(QString::fromUtf8(dpCFW->getDeviceName())) != QString())
        {
            emit wsThread->sendMessageToClient("getCFWList:" + Tools::readCFWList(QString::fromUtf8(dpCFW->getDeviceName())));
        }
    }

    if (dpGuider != NULL)
    {
        Logger::Log("Guider connected after Device("+QString::fromUtf8(dpGuider->getDeviceName()).toStdString()+") Connect: " + dpGuider->getDeviceName(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectSuccess:Guider:" + QString::fromUtf8(dpGuider->getDeviceName()) + ":" + QString::fromUtf8(dpGuider->getDriverExec()));
        ConnectedDevices.push_back({"Guider", QString::fromUtf8(dpGuider->getDeviceName())});
        Logger::Log("Guider connected successfully.", LogLevel::INFO, DeviceType::MAIN);

        systemdevicelist.system_devices[1].DeviceIndiName = QString::fromUtf8(dpGuider->getDeviceName());
        systemdevicelist.system_devices[1].isBind = true;
    }
    Logger::Log("All devices connected after successfully.", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::AfterDeviceConnect(INDI::BaseDevice *dp)
{
    if (dpMainCamera == dp)
    {
        Logger::Log("MainCamera connected after Device(" + QString::fromUtf8(dpMainCamera->getDeviceName()).toStdString() + ") Connect: " + QString::fromUtf8(dpMainCamera->getDeviceName()).toStdString(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectSuccess:MainCamera:" + QString::fromUtf8(dpMainCamera->getDeviceName()) + ":" + QString::fromUtf8(dpMainCamera->getDriverExec()));
        ConnectedDevices.push_back({"MainCamera", QString::fromUtf8(dpMainCamera->getDeviceName())});

        systemdevicelist.system_devices[20].DeviceIndiName = QString::fromUtf8(dpMainCamera->getDeviceName());
        systemdevicelist.system_devices[20].isBind = true;

        indi_Client->setBLOBMode(B_ALSO, dpMainCamera->getDeviceName(), nullptr);
        indi_Client->enableDirectBlobAccess(dpMainCamera->getDeviceName(), nullptr);

        QString SDKVERSION;
        indi_Client->getCCDSDKVersion(dpMainCamera, SDKVERSION);
        Logger::Log("MainCamera SDK version: " + SDKVERSION.toStdString(), LogLevel::INFO, DeviceType::MAIN);

        indi_Client->getCCDOffset(dpMainCamera, glOffsetValue, glOffsetMin, glOffsetMax);
        emit wsThread->sendMessageToClient("MainCameraOffsetRange:" + QString::number(glOffsetMin) + ":" + QString::number(glOffsetMax));
        Logger::Log("CCD Offset - Value: " + std::to_string(glOffsetValue) + ", Min: " + std::to_string(glOffsetMin) + ", Max: " + std::to_string(glOffsetMax), LogLevel::INFO, DeviceType::MAIN);

        indi_Client->getCCDGain(dpMainCamera, glGainValue, glGainMin, glGainMax);
        Logger::Log("CCD Gain - Value: " + std::to_string(glGainValue) + ", Min: " + std::to_string(glGainMin) + ", Max: " + std::to_string(glGainMax), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("MainCameraGainRange:" + QString::number(glGainMin) + ":" + QString::number(glGainMax));

        int maxX, maxY; 
        double pixelsize, pixelsizX, pixelsizY;
        int bitDepth;
        indi_Client->getCCDBasicInfo(dpMainCamera, maxX, maxY, pixelsize, pixelsizX, pixelsizY, bitDepth);
        Logger::Log("CCD Basic Info - MaxX: " + std::to_string(maxX) + ", MaxY: " + std::to_string(maxY) + ", PixelSize: " + std::to_string(pixelsize), LogLevel::INFO, DeviceType::MAIN);

        if(maxX == 0 && maxY == 0) {
            QString CameraName = dpMainCamera->getDeviceName();
            Logger::Log("This may be a DSLRs Camera, need to set Resolution and pixel size. Camera: " + CameraName.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
            DSLRsInfo DSLRsInfo = Tools::readDSLRsInfo(CameraName);
            if(DSLRsInfo.Name == CameraName && DSLRsInfo.SizeX != 0 && DSLRsInfo.SizeY != 0 && DSLRsInfo.PixelSize != 0) {
                indi_Client->setCCDBasicInfo(dpMainCamera, DSLRsInfo.SizeX, DSLRsInfo.SizeY, DSLRsInfo.PixelSize, DSLRsInfo.PixelSize, DSLRsInfo.PixelSize, 8);
                indi_Client->getCCDBasicInfo(dpMainCamera, maxX, maxY, pixelsize, pixelsizX, pixelsizY, bitDepth);
                Logger::Log("Updated CCD Basic Info for DSLRs Camera.", LogLevel::INFO, DeviceType::MAIN);
            } else {
                emit wsThread->sendMessageToClient("DSLRsSetup:" + QString::fromUtf8(dpMainCamera->getDeviceName()));
            }
        }

        glCameraSize_width = maxX * pixelsize / 1000;
        glCameraSize_width = std::round(glCameraSize_width * 10) / 10;
        glCameraSize_height = maxY * pixelsize / 1000;
        glCameraSize_height = std::round(glCameraSize_height * 10) / 10;
        Logger::Log("CCD Chip size - Width: " + std::to_string(glCameraSize_width) + ", Height: " + std::to_string(glCameraSize_height), LogLevel::INFO, DeviceType::MAIN);

        int X, Y;
        indi_Client->getCCDFrameInfo(dpMainCamera, X, Y, glMainCCDSizeX, glMainCCDSizeY);
        Logger::Log("CCD Frame Info - SizeX: " + std::to_string(glMainCCDSizeX) + ", SizeY: " + std::to_string(glMainCCDSizeY), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));

        int offsetX, offsetY;
        indi_Client->getCCDCFA(dpMainCamera, offsetX, offsetY, MainCameraCFA);
        Logger::Log("CCD CFA Info - OffsetX: " + std::to_string(offsetX) + ", OffsetY: " + std::to_string(offsetY) + ", CFA: " + MainCameraCFA.toStdString(), LogLevel::INFO, DeviceType::MAIN);

        indi_Client->setCCDUploadModeToLacal(dpMainCamera);
        indi_Client->setCCDUpload(dpMainCamera, "/dev/shm", "ccd_simulator");

        QString CFWname;
        indi_Client->getCFWSlotName(dpMainCamera, CFWname);
        if(CFWname != "") {
            Logger::Log("CFW Slot Name: " + CFWname.toStdString(), LogLevel::INFO, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("ConnectSuccess:CFW:" + CFWname + " (on camera):" + QString::fromUtf8(dpMainCamera->getDriverExec()));
            isFilterOnCamera = true;

            int min, max, pos;
            indi_Client->getCFWPosition(dpMainCamera, pos, min, max);
            Logger::Log("CFW Position - Min: " + std::to_string(min) + ", Max: " + std::to_string(max) + ", Current: " + std::to_string(pos), LogLevel::INFO, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CFWPositionMax:" + QString::number(max));
        }
        Logger::Log("MainCamera connected successfully.", LogLevel::INFO, DeviceType::MAIN);
    }

    if (dpMount == dp)
    {
        Logger::Log("Mount connected after Device(" + QString::fromUtf8(dpMount->getDeviceName()).toStdString() + ") Connect: " + QString::fromUtf8(dpMount->getDeviceName()).toStdString(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectSuccess:Mount:" + QString::fromUtf8(dpMount->getDeviceName()) + ":" + QString::fromUtf8(dpMount->getDriverExec()));
        ConnectedDevices.push_back({"Mount", QString::fromUtf8(dpMount->getDeviceName())});

        systemdevicelist.system_devices[0].DeviceIndiName = QString::fromUtf8(dpMount->getDeviceName());
        systemdevicelist.system_devices[0].isBind = true;
        QString DevicePort;

        indi_Client->GetAllPropertyName(dpMount);

        getClientSettings();
        indi_Client->setLocation(dpMount,observatorylatitude,observatorylongitude,50);
        indi_Client->setAutoFlip(dpMount,true);
        indi_Client->setAUXENCODERS(dpMount);

        indi_Client->getDevicePort(dpMount, DevicePort);
        Logger::Log("Device port for Mount: " + DevicePort.toStdString(), LogLevel::INFO, DeviceType::MAIN);

        double glLongitude_radian, glLatitude_radian;
        glLongitude_radian = Tools::getDecAngle("116° 14' 53.91");
        glLatitude_radian = Tools::getDecAngle("40° 09' 14.93");
        Logger::Log("Mount location set to Longitude: " + QString::number(Tools::RadToDegree(glLongitude_radian)).toStdString() + ", Latitude: " + QString::number(Tools::RadToDegree(glLatitude_radian)).toStdString(), LogLevel::INFO, DeviceType::MAIN);

        indi_Client->setLocation(dpMount, Tools::RadToDegree(glLatitude_radian), Tools::RadToDegree(glLongitude_radian), 10);
        QDateTime datetime = QDateTime::currentDateTime();
        indi_Client->setTimeUTC(dpMount, datetime);
        Logger::Log("UTC Time set for Mount: " + datetime.toString(Qt::ISODate).toStdString(), LogLevel::INFO, DeviceType::MAIN);   
        indi_Client->getTimeUTC(dpMount, datetime);
        Logger::Log("UTC Time: " + datetime.currentDateTimeUtc().toString(Qt::ISODate).toStdString(), LogLevel::INFO, DeviceType::MAIN);

        double a, b, c, d;
        indi_Client->getTelescopeInfo(dpMount, a, b, c, d);
        Logger::Log("Telescope Info - a: " + std::to_string(a) + ", b: " + std::to_string(b) + ", c: " + std::to_string(c) + ", d: " + std::to_string(d), LogLevel::INFO, DeviceType::MAIN);

        indi_Client->getTelescopeRADECJ2000(dpMount, a, b);
        Logger::Log("Telescope RA/DEC J2000 - RA: " + std::to_string(a) + ", DEC: " + std::to_string(b), LogLevel::INFO, DeviceType::MAIN);
        indi_Client->getTelescopeRADECJNOW(dpMount, a, b);
        Logger::Log("Telescope RA/DEC JNOW - RA: " + std::to_string(a) + ", DEC: " + std::to_string(b), LogLevel::INFO, DeviceType::MAIN);

        bool isPark;
        indi_Client->getTelescopePark(dpMount, isPark);
        Logger::Log("Telescope Park Status: " + std::string(isPark ? "Parked" : "Unparked"), LogLevel::INFO, DeviceType::MAIN);

        int maxspeed, minspeed, speedvalue, total;
        indi_Client->getTelescopeTotalSlewRate(dpMount, total);
        glTelescopeTotalSlewRate = total;
        Logger::Log("Telescope Total Slew Rate: " + std::to_string(total), LogLevel::INFO, DeviceType::MAIN);

        emit wsThread->sendMessageToClient("TelescopeTotalSlewRate:" + QString::number(total));
        indi_Client->getTelescopeMaxSlewRateOptions(dpMount, minspeed, maxspeed, speedvalue);
        indi_Client->setTelescopeSlewRate(dpMount, total);
        int speed;
        indi_Client->getTelescopeSlewRate(dpMount,speed);
        Logger::Log("Current Telescope Slew Rate: " + std::to_string(speed), LogLevel::INFO, DeviceType::MAIN);
        // emit wsThread->sendMessageToClient("TelescopeCurrentSlewRate:" + QString::number(speed));
        emit wsThread->sendMessageToClient("MountSetSpeedSuccess:" + QString::number(speed));
        indi_Client->setTelescopeTrackEnable(dpMount, true);

        bool isTrack = false;
        indi_Client->getTelescopeTrackEnable(dpMount, isTrack);

        if (isTrack)
        {
            emit wsThread->sendMessageToClient("TelescopeTrack:ON");
        }
        else
        {
            emit wsThread->sendMessageToClient("TelescopeTrack:OFF");
        }
        Logger::Log("Telescope Tracking Status: " + std::string(isTrack ? "Enabled" : "Disabled"), LogLevel::INFO, DeviceType::MAIN);
        indi_Client->setTelescopeTrackRate(dpMount, "SIDEREAL");
        QString side;
        indi_Client->getTelescopePierSide(dpMount, side);
        Logger::Log("Telescope Pier Side: " + side.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("TelescopePierSide:" + side);
        Logger::Log("Mount connected successfully.", LogLevel::INFO, DeviceType::MAIN);
    }

    if (dpFocuser == dp)
    {
        Logger::Log("Focuser connected after Device(" + QString::fromUtf8(dpFocuser->getDeviceName()).toStdString() + ") Connect: " + dpFocuser->getDeviceName(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectSuccess:Focuser:" + QString::fromUtf8(dpFocuser->getDeviceName()) + ":" + QString::fromUtf8(dpFocuser->getDriverExec()));
        ConnectedDevices.push_back({"Focuser", QString::fromUtf8(dpFocuser->getDeviceName())});

        systemdevicelist.system_devices[22].DeviceIndiName = QString::fromUtf8(dpFocuser->getDeviceName());
        systemdevicelist.system_devices[22].isBind = true;
        indi_Client->GetAllPropertyName(dpFocuser);
        // indi_Client->syncFocuserPosition(dpFocuser, 0);
        CurrentPosition = FocuserControl_getPosition();
        Logger::Log("Focuser Current Position: " + std::to_string(CurrentPosition), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
        Logger::Log("Focuser connected successfully.", LogLevel::INFO, DeviceType::MAIN);
    }

    if (dpCFW == dp)
    {   
        Logger::Log("CFW connected after Device("+QString::fromUtf8(dpCFW->getDeviceName()).toStdString()+") Connect: " + dpCFW->getDeviceName(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectSuccess:CFW:" + QString::fromUtf8(dpCFW->getDeviceName()) + ":" + QString::fromUtf8(dpCFW->getDriverExec()));
        ConnectedDevices.push_back({"CFW", QString::fromUtf8(dpCFW->getDeviceName())});

        systemdevicelist.system_devices[21].DeviceIndiName = QString::fromUtf8(dpCFW->getDeviceName());
        systemdevicelist.system_devices[21].isBind = true;
        indi_Client->GetAllPropertyName(dpCFW);
        int min, max, pos;
        indi_Client->getCFWPosition(dpCFW, pos, min, max);
        Logger::Log("CFW Position - Min: " + std::to_string(min) + ", Max: " + std::to_string(max) + ", Current: " + std::to_string(pos), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("CFWPositionMax:" + QString::number(max));
        if(Tools::readCFWList(QString::fromUtf8(dpCFW->getDeviceName())) != QString())
        {
            emit wsThread->sendMessageToClient("getCFWList:" + Tools::readCFWList(QString::fromUtf8(dpCFW->getDeviceName())));
        }
        Logger::Log("CFW connected successfully.", LogLevel::INFO, DeviceType::MAIN);
    }

    if (dpGuider == dp)
    {
        Logger::Log("Guider connected after Device("+QString::fromUtf8(dpGuider->getDeviceName()).toStdString()+") Connect: " + dpGuider->getDeviceName(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectSuccess:Guider:" + QString::fromUtf8(dpGuider->getDeviceName()) + ":" + QString::fromUtf8(dpGuider->getDriverExec()));
        ConnectedDevices.push_back({"Guider", QString::fromUtf8(dpGuider->getDeviceName())});
        Logger::Log("Guider connected successfully.", LogLevel::INFO, DeviceType::MAIN);
        systemdevicelist.system_devices[1].DeviceIndiName = QString::fromUtf8(dpGuider->getDeviceName());
        systemdevicelist.system_devices[1].isBind = true;
    }

    Tools::saveSystemDeviceList(systemdevicelist);
    qDebug() << "*** ***  当前系统列表 *** ***";
    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        if(systemdevicelist.system_devices[i].Description != ""){
            qDebug() << "设备类型："<<systemdevicelist.system_devices[i].Description;
            qDebug() << "设备名称："<<systemdevicelist.system_devices[i].DeviceIndiName ;
            qDebug() << "是否绑定："<<systemdevicelist.system_devices[i].isBind;
            qDebug() << "驱动名称："<<systemdevicelist.system_devices[i].DriverIndiName;
            qDebug() << " *** *** *** *** *** *** ";
        }
    }
    qDebug() << "*** ***  当前设备列表 *** ***";
    for (int i = 0; i < indi_Client->GetDeviceCount(); i++){
        qDebug() << "设备名称："<<QString::fromStdString(indi_Client->GetDeviceNameFromList(i));
        qDebug() << "驱动名称："<<QString::fromStdString(indi_Client->GetDeviceFromList(i)->getDriverExec());
        qDebug() << "是否连接：" << QString::fromStdString(std::to_string(indi_Client->GetDeviceFromList(i)->isConnected()));
        qDebug() << " *** *** *** *** *** *** ";
    }
}

void MainWindow::disconnectIndiServer(MyClient *client)
{
    Logger::Log("disconnectIndiServer start ...", LogLevel::INFO, DeviceType::MAIN);
    // client->disconnectAllDevice();
    QVector<INDI::BaseDevice *> dp;
    
    for(int i=0; i < client->GetDeviceCount(); i++)
    {
        dp.append(client->GetDeviceFromList(i));
        if (dp[i]->isConnected())
        {
            client->disconnectDevice(dp[i]->getDeviceName());
            int num = 0;
            while (dp[i]->isConnected())
            {
                Logger::Log("disconnectAllDevice | Waiting for disconnect device (" + QString::fromUtf8(dp[i]->getDeviceName()).toStdString() + ") finish...", LogLevel::INFO, DeviceType::MAIN);
                sleep(1);
                num++;

                if(num > 10) {
                    Logger::Log("disconnectAllDevice | device (" + QString::fromUtf8(dp[i]->getDeviceName()).toStdString() + ") disconnect failed.", LogLevel::WARNING, DeviceType::MAIN);
                    break;
                }
            }
            Logger::Log("disconnectAllDevice | device (" + QString::fromUtf8(dp[i]->getDeviceName()).toStdString() + ") disconnected successfully.", LogLevel::INFO, DeviceType::MAIN);
        }
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
    indi_Client->PrintDevices();
    Tools::printSystemDeviceList(systemdevicelist);
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
    sleep(1);
    client->PrintDevices();
    Logger::Log("connectIndiServer finished.", LogLevel::INFO, DeviceType::MAIN);
    return true;
}

void MainWindow::ClearSystemDeviceList() {
    Logger::Log("ClearSystemDeviceList start ...", LogLevel::INFO, DeviceType::MAIN);
    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        systemdevicelist.system_devices[i].DeviceIndiGroup = -1;
        systemdevicelist.system_devices[i].DeviceIndiName = "";
        systemdevicelist.system_devices[i].DriverFrom = "";
        // systemdevicelist.system_devices[i].DriverIndiName = "";
        systemdevicelist.system_devices[i].isConnect = false;
        systemdevicelist.system_devices[i].dp = NULL;
        // systemdevicelist.system_devices[i].Description = "";
    }
    Logger::Log("ClearSystemDeviceList finished.", LogLevel::INFO, DeviceType::MAIN);
    Tools::printSystemDeviceList(systemdevicelist);
    
}

void MainWindow::INDI_Capture(int Exp_times)
{
    Logger::Log("INDI_Capture start ...", LogLevel::INFO, DeviceType::CAMERA);
    glIsFocusingLooping = false;
    double expTime_sec;
    expTime_sec = (double)Exp_times / 1000;
    Logger::Log("INDI_Capture | convert Exp_times to seconds:" + std::to_string(expTime_sec), LogLevel::INFO, DeviceType::CAMERA);

    if (dpMainCamera)
    {
        glMainCameraStatu = "Exposuring";
        Logger::Log("INDI_Capture | check Main Camera Status(glMainCameraStatu):" + glMainCameraStatu.toStdString(), LogLevel::INFO, DeviceType::CAMERA);

        int value, min, max;
        uint32_t ret = indi_Client->getCCDGain(dpMainCamera, value, min, max);
        if (ret != QHYCCD_SUCCESS)
        {
            Logger::Log("INDI_Capture | indi getCCDGain | failed", LogLevel::WARNING, DeviceType::CAMERA);
        }
        Logger::Log("INDI_Capture | indi getCCDGain | value:" + std::to_string(value) + ", min:" + std::to_string(min) + ", max:" + std::to_string(max), LogLevel::INFO, DeviceType::CAMERA);
        int BINX, BINY, BINXMAX, BINYMAX;
        ret = indi_Client->getCCDBinning(dpMainCamera, BINX, BINY, BINXMAX, BINYMAX);
        if (ret != QHYCCD_SUCCESS)
        {
            Logger::Log("INDI_Capture | indi getCCDBinning | failed", LogLevel::WARNING, DeviceType::CAMERA);
        }
        Logger::Log("INDI_Capture | indi getCCDBinning | BINX:" + std::to_string(BINX) + ", BINY:" + std::to_string(BINY) + ", BINXMAX:" + std::to_string(BINXMAX) + ", BINYMAX:" + std::to_string(BINYMAX), LogLevel::INFO, DeviceType::CAMERA);
        ret = indi_Client->getCCDOffset(dpMainCamera, value, min, max);
        if (ret != QHYCCD_SUCCESS)
        {
            Logger::Log("INDI_Capture | indi getCCDOffset | failed", LogLevel::WARNING, DeviceType::CAMERA);
        }
        Logger::Log("INDI_Capture | indi getCCDOffset | value:" + std::to_string(value) + ", min:" + std::to_string(min) + ", max:" + std::to_string(max), LogLevel::INFO, DeviceType::CAMERA);
        ret = indi_Client->resetCCDFrameInfo(dpMainCamera);
        if (ret != QHYCCD_SUCCESS)
        {
            Logger::Log("INDI_Capture | indi resetCCDFrameInfo | failed", LogLevel::WARNING, DeviceType::CAMERA);
        }
        Logger::Log("INDI_Capture | indi resetCCDFrameInfo", LogLevel::INFO, DeviceType::CAMERA);
        emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));
        Logger::Log("INDI_Capture | sendMessageToClient | MainCameraSize:" + QString::number(glMainCCDSizeX).toStdString() + ":" + QString::number(glMainCCDSizeY).toStdString(), LogLevel::INFO, DeviceType::CAMERA);
        ret = indi_Client->takeExposure(dpMainCamera, expTime_sec);
        if (ret != QHYCCD_SUCCESS)
        {
            Logger::Log("INDI_Capture | indi takeExposure | failed", LogLevel::WARNING, DeviceType::CAMERA);
        }
        Logger::Log("INDI_Capture | indi start takeExposure, expTime_sec:" + std::to_string(expTime_sec), LogLevel::INFO, DeviceType::CAMERA);
    }
    else
    {
        Logger::Log("INDI_Capture | dpMainCamera is NULL", LogLevel::WARNING, DeviceType::CAMERA);
    }
    Logger::Log("INDI_Capture finished.", LogLevel::INFO, DeviceType::CAMERA);
}

void MainWindow::INDI_AbortCapture()
{
    Logger::Log("INDI_AbortCapture start ...", LogLevel::INFO, DeviceType::CAMERA);
    glMainCameraStatu="IDLE";
    Logger::Log("INDI_AbortCapture | glMainCameraStatu:" + glMainCameraStatu.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    if (dpMainCamera)
    {
        indi_Client->setCCDAbortExposure(dpMainCamera);
        ShootStatus = "IDLE";
        Logger::Log("INDI_AbortCapture | ShootStatus:" + ShootStatus.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    }
    Logger::Log("INDI_AbortCapture finished.", LogLevel::INFO, DeviceType::CAMERA);
}

void MainWindow::FocusingLooping()
{
    Logger::Log("FocusingLooping start ...", LogLevel::DEBUG, DeviceType::FOCUSER);
    // 检查相机是否连接，如果未连接则记录警告并返回
    if (dpMainCamera == NULL)
    {
        Logger::Log("FocusingLooping | dpMainCamera is NULL", LogLevel::WARNING, DeviceType::FOCUSER);
        return;
    }

    glIsFocusingLooping = true;
    Logger::Log("FocusingLooping | glIsFocusingLooping:" + std::to_string(glIsFocusingLooping), LogLevel::DEBUG, DeviceType::FOCUSER);
    // 如果相机状态为“显示中”，则开始处理曝光
    if (glMainCameraStatu != "Exposuring")
    {
        double expTime_sec;
        expTime_sec = (double)glExpTime / 1000;  // 将曝光时间从毫秒转换为秒

        glMainCameraStatu = "Exposuring";
        Logger::Log("FocusingLooping | glMainCameraStatu:" + glMainCameraStatu.toStdString(), LogLevel::DEBUG, DeviceType::FOCUSER);

        QSize cameraResolution{glMainCCDSizeX, glMainCCDSizeY};
        QSize ROI{BoxSideLength, BoxSideLength};

        // int cameraX = static_cast<int>(glROI_x * cameraResolution.width() / static_cast<double>(CaptureViewWidth));
        // int cameraY = static_cast<int>(glROI_y * cameraResolution.height() / static_cast<double>(CaptureViewHeight));

        Logger::Log("FocusingLooping |当前ROI值 glROI_x:" + std::to_string(glROI_x) + ", glROI_y:" + std::to_string(glROI_y), LogLevel::DEBUG, DeviceType::FOCUSER);
        int cameraX = static_cast<int>(glROI_x );
        int cameraY = static_cast<int>(glROI_y );


        // 确保 cameraX 和 cameraY 是偶数
        if (cameraX % 2 != 0) {
            cameraX += 1;
        }
        if (cameraY % 2 != 0) {
            cameraY += 1;
        }

        // 检查计算出的曝光区域是否在相机的有效范围内
        if (cameraX < glMainCCDSizeX - ROI.width() && cameraY < glMainCCDSizeY - ROI.height())
        {
            Logger::Log("FocusingLooping | set Camera ROI x:" + std::to_string(cameraX) + ", y:" + std::to_string(cameraY)+", width:" + std::to_string(BoxSideLength) + ", height:" + std::to_string(BoxSideLength), LogLevel::DEBUG, DeviceType::FOCUSER);
            indi_Client->setCCDFrameInfo(dpMainCamera, cameraX, cameraY, BoxSideLength, BoxSideLength); // 设置相机的曝光区域
            indi_Client->takeExposure(dpMainCamera, expTime_sec);  // 进行曝光
            Logger::Log("FocusingLooping | takeExposure, expTime_sec:" + std::to_string(expTime_sec), LogLevel::DEBUG, DeviceType::FOCUSER);
        }
        else
        {
            Logger::Log("FocusingLooping | Too close to the edge, please reselect the area.", LogLevel::WARNING, DeviceType::FOCUSER); // 如果区域太靠近边缘，记录警告并调整
            if (cameraX + ROI.width() > glMainCCDSizeX)
                cameraX = glMainCCDSizeX - ROI.width();
            if (cameraY + ROI.height() > glMainCCDSizeY)
                cameraY = glMainCCDSizeY - ROI.height();

            indi_Client->setCCDFrameInfo(dpMainCamera, cameraX, cameraY, ROI.width(), ROI.height()); // 重新设置曝光区域并进行曝光
            indi_Client->takeExposure(dpMainCamera, expTime_sec);
        }
    }else{
        emit wsThread->sendMessageToClient("startFocusLoopSjootingfile:Wait Take Picture Finish!");
    }
    Logger::Log("FocusingLooping finished.", LogLevel::DEBUG, DeviceType::FOCUSER);
}

void MainWindow::refreshGuideImage(cv::Mat img16, QString CFA)
{
    // strechShowImage(img16, CFA, true, true, 0, 0, 65535, 1.0, 1.7, 100, true);
}

void MainWindow::strechShowImage(cv::Mat img16,QString CFA,bool AutoStretch,bool AWB,int AutoStretchMode,uint16_t blacklevel,uint16_t whitelevel,double ratioRG,double ratioBG,uint16_t offset,bool updateHistogram){
    Logger::Log("strechShowImage start ...", LogLevel::INFO, DeviceType::MAIN);
   uint16_t B=0;
   uint16_t W=65535;

 if(CFA=="MONO") {
    //mono camera, do not do debayer and color balance process
    Logger::Log("strechShowImage | CFA is MONO", LogLevel::INFO, DeviceType::MAIN);
     cv::Mat image_raw8;
     image_raw8.create(img16.rows,img16.cols,CV_8UC1);

     if(AutoStretch==true){
        Tools::GetAutoStretch(img16,AutoStretchMode,B,W);
     } else {
        B=blacklevel;
        W=whitelevel;
     }

    Tools::Bit16To8_Stretch(img16,image_raw8,B,W);

    // saveGuiderImageAsJPG(image_raw8);


    image_raw8.release();
 }

 else{
      //color camera, need to do debayer and color balance
    Logger::Log("strechShowImage | CFA is COLOR", LogLevel::INFO, DeviceType::MAIN);
     cv::Mat AWBImg16;
     cv::Mat AWBImg16color;
     cv::Mat AWBImg16mono;
     cv::Mat AWBImg8color;

     AWBImg16.create(img16.rows,img16.cols,CV_16UC1);
     AWBImg16color.create(img16.rows,img16.cols,CV_16UC3);
     AWBImg16mono.create(img16.rows,img16.cols,CV_16UC1);
     AWBImg8color.create(img16.rows,img16.cols,CV_8UC3);

     Tools::ImageSoftAWB(img16,AWBImg16,CFA,ratioRG,ratioBG,offset);  //image software Auto White Balance is done in RAW image.
     cv::cvtColor(AWBImg16,AWBImg16color,CV_BayerRG2BGR);
    //  qDebug()<<"strechShowImage | 1";
     cv::cvtColor(AWBImg16color,AWBImg16mono,cv::COLOR_BGR2GRAY);
    //  qDebug()<<"strechShowImage | 2";

     if(AutoStretch==true){
        Tools::GetAutoStretch(AWBImg16mono,AutoStretchMode,B,W);
     }


     else{
         B=blacklevel;
         W=whitelevel;
     }
     Logger::Log("strechShowImage | image stretch | B:" + std::to_string(B) + ", W:" + std::to_string(W), LogLevel::INFO, DeviceType::MAIN);
     Tools::Bit16To8_Stretch(AWBImg16color,AWBImg8color,B,W);

    //  Tools::ShowCvImageOnQLabel(AWBImg8color,lable);
    // saveGuiderImageAsJPG(AWBImg8color);

    AWBImg16.release();
    AWBImg16color.release();
    AWBImg16mono.release();
    AWBImg8color.release();
 }
 glMainCameraStatu="IDLE";
 Logger::Log("strechShowImage | glMainCameraStatu:" + glMainCameraStatu.toStdString(), LogLevel::INFO, DeviceType::MAIN);
 Logger::Log("strechShowImage finished.", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::InitPHD2()
{
    Logger::Log("InitPHD2 start ...", LogLevel::INFO, DeviceType::MAIN);
    isGuideCapture = true;

    cmdPHD2 = new QProcess();

    bool connected = false;
    int retryCount = 3; // 设定重试次数
    while (retryCount > 0 && !connected)
    {
        // 杀死所有已存在的 phd2 进程
        cmdPHD2->start("pkill phd2");
        sleep(3);
        cmdPHD2->waitForStarted();
        cmdPHD2->waitForFinished();

        // 重新生成共享内存，避免之前的进程遗留问题
        key_phd = 0x90; // 重新设置共享内存的键值
        shmid_phd = shmget(key_phd, BUFSZ_PHD, IPC_CREAT | 0666); // 获取共享内存
        if (shmid_phd < 0)
        {
            Logger::Log("InitPHD2 | shared memory phd shmget ERROR", LogLevel::ERROR, DeviceType::MAIN);
            continue;
        }

        // 映射共享内存
        sharedmemory_phd = (char *)shmat(shmid_phd, NULL, 0);
        if (sharedmemory_phd == NULL)
        {
            Logger::Log("InitPHD2 | shared memory phd map ERROR", LogLevel::ERROR, DeviceType::MAIN);
            continue;
        }

        // 读取共享内存数据
        Logger::Log("InitPHD2 | data_phd = [" + std::string(sharedmemory_phd) + "]", LogLevel::INFO, DeviceType::MAIN);

        // 启动 phd2 进程
        cmdPHD2->start("phd2");

        // 等待最多 10 秒尝试连接
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 10000)
        {
            usleep(10000);
            qApp->processEvents();
            if (connectPHD() == true)
            {
                connected = true;
                break;
            }
        }

        if (!connected)
        {
            Logger::Log("InitPHD2 | Failed to connect to phd2. Retrying...", LogLevel::WARNING, DeviceType::MAIN);
            retryCount--; // 如果连接失败，重试次数减 1
        }
    }

    if (!connected)
    {
        Logger::Log("InitPHD2 | Failed to connect to phd2 after retries.", LogLevel::ERROR, DeviceType::MAIN);
    }

    Logger::Log("InitPHD2 finished.", LogLevel::INFO, DeviceType::MAIN);
}

// void MainWindow::InitPHD2()
// {
//     Logger::Log("InitPHD2 start ...", LogLevel::INFO, DeviceType::MAIN);
//     isGuideCapture = true;

//     cmdPHD2 = new QProcess();
//     cmdPHD2->start("pkill phd2");
//     cmdPHD2->waitForStarted();
//     cmdPHD2->waitForFinished();

//     key_phd = ftok("../", 2015);
//     key_phd = 0x90;

//     if (key_phd == -1)
//     {
//         Logger::Log("InitPHD2 | ftok_phd", LogLevel::WARNING, DeviceType::MAIN);
//     }

//     // build the shared memory
//     system("ipcs -m"); // 查看共享内存
//     shmid_phd = shmget(key_phd, BUFSZ_PHD, IPC_CREAT | 0666);
//     if (shmid_phd < 0)
//     {
//         Logger::Log("InitPHD2 | shared memory phd shmget ERROR", LogLevel::ERROR, DeviceType::MAIN);
//         exit(-1);
//     }

//     // 映射
//     sharedmemory_phd = (char *)shmat(shmid_phd, NULL, 0);
//     if (sharedmemory_phd == NULL)
//     {
//         Logger::Log("InitPHD2 | shared memory phd map ERROR", LogLevel::ERROR, DeviceType::MAIN);
//         exit(-1);
//     }

//     // 读共享内存区数据
//     Logger::Log("InitPHD2 | data_phd = [" + std::string(sharedmemory_phd) + "]", LogLevel::INFO, DeviceType::MAIN);

//     cmdPHD2->start("phd2");

//     QElapsedTimer t;
//     t.start();
//     while (t.elapsed() < 10000)
//     {
//         usleep(10000);
//         qApp->processEvents();
//         if (connectPHD() == true)
//             break;
//     }
//     Logger::Log("InitPHD2 finished.", LogLevel::INFO, DeviceType::MAIN);
// }

bool MainWindow::connectPHD(void)
{
    Logger::Log("connectPHD start ...", LogLevel::INFO, DeviceType::MAIN);
    QString versionName = "";
    call_phd_GetVersion(versionName);

    Logger::Log("connectPHD | version:" + versionName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    if (versionName != "")
    {
        // init stellarium operation
        Logger::Log("connectPHD Success!", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
    else
    {
        Logger::Log("connectPHD | there is no openPHD2 running", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("connectPHD failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
}

bool MainWindow::call_phd_GetVersion(QString &versionName)
{
    Logger::Log("call_phd_GetVersion start ...", LogLevel::INFO, DeviceType::MAIN);
    unsigned int baseAddress;
    unsigned int vendcommand;
    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x01;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }

    if (t.elapsed() >= 500)
    {
        versionName = "";
        Logger::Log("call_phd_GetVersion | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_GetVersion failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    else
    {
        unsigned char addr = 0;
        uint16_t length;
        memcpy(&length, sharedmemory_phd + baseAddress + addr, sizeof(uint16_t));
        addr = addr + sizeof(uint16_t);
        // qDebug()<<length;

        if (length > 0 && length < 1024)
        {
            for (int i = 0; i < length; i++)
            {
                versionName.append(sharedmemory_phd[baseAddress + addr + i]);
            }
            Logger::Log("call_phd_GetVersion | version:" + versionName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
            Logger::Log("call_phd_GetVersion success.", LogLevel::INFO, DeviceType::MAIN);
            return true;
            // qDebug()<<versionName;
        }
        else
        {
            versionName = "";
            Logger::Log("call_phd_GetVersion | version is empty", LogLevel::ERROR, DeviceType::MAIN);
            Logger::Log("call_phd_GetVersion failed.", LogLevel::ERROR, DeviceType::MAIN);
            return false;
        }
    }
}

uint32_t MainWindow::call_phd_StartLooping(void)
{
    Logger::Log("call_phd_StartLooping start ...", LogLevel::INFO, DeviceType::GUIDER);
    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x03;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_StartLooping | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_StartLooping failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_StartLooping success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_StopLooping(void)
{
    Logger::Log("call_phd_StopLooping start ...", LogLevel::INFO, DeviceType::GUIDER);
    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x04;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_StopLooping | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_StopLooping failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_StopLooping success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_AutoFindStar(void)
{
    Logger::Log("call_phd_AutoFindStar start ...", LogLevel::INFO, DeviceType::GUIDER);
    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x05;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_AutoFindStar | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_AutoFindStar failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_AutoFindStar success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_StartGuiding(void)
{
    Logger::Log("call_phd_StartGuiding start ...", LogLevel::INFO, DeviceType::GUIDER);
    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x06;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_StartGuiding | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_StartGuiding failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_StartGuiding success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_checkStatus(unsigned char &status)
{
    Logger::Log("call_phd_checkStatus start ...", LogLevel::INFO, DeviceType::GUIDER);
    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x07;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    // wait stellarium finished this task
    QElapsedTimer t;
    t.start();
    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    } // wait stellarium run end

    if (t.elapsed() >= 500)
    {
        // timeout
        status = 0;
        Logger::Log("call_phd_checkStatus | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_checkStatus failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }

    else
    {
        status = sharedmemory_phd[3];
        Logger::Log("call_phd_checkStatus | status:" + std::to_string(status), LogLevel::INFO, DeviceType::GUIDER);
        Logger::Log("call_phd_checkStatus success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_setExposureTime(unsigned int expTime)
{
    Logger::Log("call_phd_setExposureTime start ...", LogLevel::INFO, DeviceType::GUIDER);
    unsigned int vendcommand;
    unsigned int baseAddress;
    Logger::Log("call_phd_setExposureTime | expTime:" + std::to_string(expTime), LogLevel::INFO, DeviceType::GUIDER);

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x0b;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &expTime, sizeof(unsigned int));
    addr = addr + sizeof(unsigned int);

    sharedmemory_phd[0] = 0x01; // enable command

    // wait stellarium finished this task
    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    } // wait stellarium run end

    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_setExposureTime | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_setExposureTime failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return QHYCCD_ERROR; // timeout
    }
    else
    {
        Logger::Log("call_phd_setExposureTime success.", LogLevel::INFO, DeviceType::GUIDER);
        return QHYCCD_SUCCESS;
    }
}

uint32_t MainWindow::call_phd_whichCamera(std::string Camera)
{
    Logger::Log("call_phd_whichCamera start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_whichCamera | Camera:" + Camera, LogLevel::INFO, DeviceType::MAIN);

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x0d;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    int length = Camera.length() + 1;

    unsigned char addr = 0;
    // memcpy(sharedmemory_phd + baseAddress + addr, &index, sizeof(int));
    // addr = addr + sizeof(int);
    memcpy(sharedmemory_phd + baseAddress + addr, &length, sizeof(int));
    addr = addr + sizeof(int);
    memcpy(sharedmemory_phd + baseAddress + addr, Camera.c_str(), length);
    addr = addr + length;

    // wait stellarium finished this task
    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    } // wait stellarium run end

    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_whichCamera | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_whichCamera failed.", LogLevel::ERROR, DeviceType::MAIN);
        return QHYCCD_ERROR; // timeout
    }
    else
    {
        Logger::Log("call_phd_whichCamera success.", LogLevel::INFO, DeviceType::MAIN);
        return QHYCCD_SUCCESS;
    }
}

uint32_t MainWindow::call_phd_ChackControlStatus(int sdk_num)
{
    Logger::Log("call_phd_ChackControlStatus start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_ChackControlStatus | sdk_num:" + std::to_string(sdk_num), LogLevel::INFO, DeviceType::MAIN);

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x0e;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &sdk_num, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_ChackControlStatus | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_ChackControlStatus failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_ChackControlStatus success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_ClearCalibration(void)
{
    Logger::Log("call_phd_ClearCalibration start ...", LogLevel::INFO, DeviceType::GUIDER);
    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x02;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_ClearCalibration | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_ClearCalibration failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_ClearCalibration success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_StarClick(int x, int y)
{
    Logger::Log("call_phd_StarClick start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_StarClick | x:" + std::to_string(x) + ", y:" + std::to_string(y), LogLevel::INFO, DeviceType::MAIN);

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x0f;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &x, sizeof(int));
    addr = addr + sizeof(int);
    memcpy(sharedmemory_phd + baseAddress + addr, &y, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_StarClick | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_StarClick failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_StarClick success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_FocalLength(int FocalLength) {
    Logger::Log("call_phd_FocalLength start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_FocalLength | FocalLength:" + std::to_string(FocalLength), LogLevel::INFO, DeviceType::MAIN);

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x10;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &FocalLength, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_FocalLength | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_FocalLength failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_FocalLength success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_MultiStarGuider(bool isMultiStar) {
    Logger::Log("call_phd_MultiStarGuider start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_MultiStarGuider | isMultiStar:" + std::to_string(isMultiStar), LogLevel::INFO, DeviceType::MAIN);

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x11;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &isMultiStar, sizeof(bool));
    addr = addr + sizeof(bool);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_MultiStarGuider | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_MultiStarGuider failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_MultiStarGuider success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_CameraPixelSize(double PixelSize) {
    Logger::Log("call_phd_CameraPixelSize start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_CameraPixelSize | PixelSize:" + std::to_string(PixelSize), LogLevel::INFO, DeviceType::MAIN);

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x12;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &PixelSize, sizeof(double));
    addr = addr + sizeof(double);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_CameraPixelSize | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_CameraPixelSize failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_CameraPixelSize success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_CameraGain(int Gain) {
    Logger::Log("call_phd_CameraGain start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_CameraGain | Gain:" + std::to_string(Gain), LogLevel::INFO, DeviceType::MAIN);

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x13;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &Gain, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_CameraGain | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_CameraGain failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_CameraGain success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_CalibrationDuration(int StepSize) {
    Logger::Log("call_phd_CalibrationDuration start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_CalibrationDuration | StepSize:" + std::to_string(StepSize), LogLevel::INFO, DeviceType::MAIN);

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x14;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &StepSize, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_CalibrationDuration | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_CalibrationDuration failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_CalibrationDuration success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_RaAggression(int Aggression) {
    Logger::Log("call_phd_RaAggression start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_RaAggression | Aggression:" + std::to_string(Aggression), LogLevel::INFO, DeviceType::MAIN);

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x15;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &Aggression, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_RaAggression | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_RaAggression failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_RaAggression success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_DecAggression(int Aggression) {
    Logger::Log("call_phd_DecAggression start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_DecAggression | Aggression:" + std::to_string(Aggression), LogLevel::INFO, DeviceType::MAIN);

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x16;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &Aggression, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_DecAggression | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_DecAggression failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_DecAggression success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

void MainWindow::ShowPHDdata()
{
    // Logger::Log("ShowPHDdata start ...", LogLevel::DEBUG, DeviceType::MAIN);
    unsigned int currentPHDSizeX = 1;
    unsigned int currentPHDSizeY = 1;
    unsigned int bitDepth = 1;

    unsigned char guideDataIndicator;
    unsigned int guideDataIndicatorAddress;
    double dRa, dDec, SNR, MASS, RMSErrorX, RMSErrorY, RMSErrorTotal, PixelRatio;
    int RADUR, DECDUR;
    char RADIR, DECDIR;
    unsigned char LossAlert;

    double StarX;
    double StarY;
    bool isSelected;

    bool showLockedCross;
    double LockedPositionX;
    double LockedPositionY;

    unsigned char MultiStarNumber;
    unsigned short MultiStarX[32];
    unsigned short MultiStarY[32];

    unsigned int mem_offset;
    int sdk_direction = 0;
    int sdk_duration = 0;
    int sdk_num;
    int zero = 0;

    bool StarLostAlert = false;

    if (sharedmemory_phd[2047] != 0x02)
    {
        // Logger::Log("ShowPHDdata | no image comes", LogLevel::DEBUG, DeviceType::MAIN);
        // sleep(1); // 如果没有图像，等待1秒
        return; // if there is no image comes, return
    }

    mem_offset = 1024;
    // guide image dimention data
    memcpy(&currentPHDSizeX, sharedmemory_phd + mem_offset, sizeof(unsigned int));
    // Logger::Log("ShowPHDdata | get currentPHDSizeX:" + std::to_string(currentPHDSizeX), LogLevel::DEBUG, DeviceType::MAIN);
    mem_offset = mem_offset + sizeof(unsigned int);
    memcpy(&currentPHDSizeY, sharedmemory_phd + mem_offset, sizeof(unsigned int));
    // Logger::Log("ShowPHDdata | get currentPHDSizeY:" + std::to_string(currentPHDSizeY), LogLevel::DEBUG, DeviceType::MAIN);
    mem_offset = mem_offset + sizeof(unsigned int);
    memcpy(&bitDepth, sharedmemory_phd + mem_offset, sizeof(unsigned char));
    // Logger::Log("ShowPHDdata | get bitDepth:" + std::to_string(bitDepth), LogLevel::DEBUG, DeviceType::MAIN);
    mem_offset = mem_offset + sizeof(unsigned char);

    mem_offset = mem_offset + sizeof(int); // &sdk_num
    mem_offset = mem_offset + sizeof(int); // &sdk_direction
    mem_offset = mem_offset + sizeof(int); // &sdk_duration

    guideDataIndicatorAddress = mem_offset;

    // guide error data
    guideDataIndicator = sharedmemory_phd[guideDataIndicatorAddress];
    // Logger::Log("ShowPHDdata | get guideDataIndicator:" + std::to_string(guideDataIndicator), LogLevel::DEBUG, DeviceType::GUIDER);

    mem_offset = mem_offset + sizeof(unsigned char);
    memcpy(&dRa, sharedmemory_phd + mem_offset, sizeof(double));
    // Logger::Log("ShowPHDdata | get dRa:" + std::to_string(dRa), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(double);
    memcpy(&dDec, sharedmemory_phd + mem_offset, sizeof(double));
    // Logger::Log("ShowPHDdata | get dDec:" + std::to_string(dDec), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(double);
    memcpy(&SNR, sharedmemory_phd + mem_offset, sizeof(double));
    // Logger::Log("ShowPHDdata | get SNR:" + std::to_string(SNR), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(double);
    memcpy(&MASS, sharedmemory_phd + mem_offset, sizeof(double));
    // Logger::Log("ShowPHDdata | get MASS:" + std::to_string(MASS), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(double);

    memcpy(&RADUR, sharedmemory_phd + mem_offset, sizeof(int));
    // Logger::Log("ShowPHDdata | get RADUR:" + std::to_string(RADUR), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(int);
    memcpy(&DECDUR, sharedmemory_phd + mem_offset, sizeof(int));
    // Logger::Log("ShowPHDdata | get DECDUR:" + std::to_string(DECDUR), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(int);

    memcpy(&RADIR, sharedmemory_phd + mem_offset, sizeof(char));
    // Logger::Log("ShowPHDdata | get RADIR:" + std::to_string(RADIR), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(char);
    memcpy(&DECDIR, sharedmemory_phd + mem_offset, sizeof(char));
    // Logger::Log("ShowPHDdata | get DECDIR:" + std::to_string(DECDIR), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(char);

    memcpy(&RMSErrorX, sharedmemory_phd + mem_offset, sizeof(double));
    // Logger::Log("ShowPHDdata | get RMSErrorX:" + std::to_string(RMSErrorX), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(double);
    memcpy(&RMSErrorY, sharedmemory_phd + mem_offset, sizeof(double));
    // Logger::Log("ShowPHDdata | get RMSErrorY:" + std::to_string(RMSErrorY), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(double);
    memcpy(&RMSErrorTotal, sharedmemory_phd + mem_offset, sizeof(double));
    // Logger::Log("ShowPHDdata | get RMSErrorTotal:" + std::to_string(RMSErrorTotal), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(double);
    memcpy(&PixelRatio, sharedmemory_phd + mem_offset, sizeof(double));
    // Logger::Log("ShowPHDdata | get PixelRatio:" + std::to_string(PixelRatio), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(double);
    memcpy(&StarLostAlert, sharedmemory_phd + mem_offset, sizeof(bool));
    // Logger::Log("ShowPHDdata | get StarLostAlert:" + std::to_string(StarLostAlert), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(bool);
    memcpy(&InGuiding, sharedmemory_phd + mem_offset, sizeof(bool));
    // Logger::Log("ShowPHDdata | get InGuiding:" + std::to_string(InGuiding), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(bool);

    mem_offset = 1024 + 200;
    memcpy(&isSelected, sharedmemory_phd + mem_offset, sizeof(bool));
    // Logger::Log("ShowPHDdata | get isSelected:" + std::to_string(isSelected), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(bool);
    memcpy(&StarX, sharedmemory_phd + mem_offset, sizeof(double));
    // Logger::Log("ShowPHDdata | get StarX:" + std::to_string(StarX), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(double);
    memcpy(&StarY, sharedmemory_phd + mem_offset, sizeof(double));
    // Logger::Log("ShowPHDdata | get StarY:" + std::to_string(StarY), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(double);
    memcpy(&showLockedCross, sharedmemory_phd + mem_offset, sizeof(bool));
    // Logger::Log("ShowPHDdata | get showLockedCross:" + std::to_string(showLockedCross), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(bool);
    memcpy(&LockedPositionX, sharedmemory_phd + mem_offset, sizeof(double));
    // Logger::Log("ShowPHDdata | get LockedPositionX:" + std::to_string(LockedPositionX), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(double);
    memcpy(&LockedPositionY, sharedmemory_phd + mem_offset, sizeof(double));
    // Logger::Log("ShowPHDdata | get LockedPositionY:" + std::to_string(LockedPositionY), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(double);
    memcpy(&MultiStarNumber, sharedmemory_phd + mem_offset, sizeof(unsigned char));
    // Logger::Log("ShowPHDdata | get MultiStarNumber:" + std::to_string(MultiStarNumber), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(unsigned char);
    memcpy(MultiStarX, sharedmemory_phd + mem_offset, sizeof(MultiStarX));
    // Logger::Log("ShowPHDdata | get MultiStarX , MultistarX length:" + std::to_string(sizeof(MultiStarX)), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(MultiStarX);
    memcpy(MultiStarY, sharedmemory_phd + mem_offset, sizeof(MultiStarY));
    // Logger::Log("ShowPHDdata | get MultiStarY , MultistarY length:" + std::to_string(sizeof(MultiStarY)), LogLevel::DEBUG, DeviceType::GUIDER);
    mem_offset = mem_offset + sizeof(MultiStarY);

    sharedmemory_phd[guideDataIndicatorAddress] = 0x00; // have been read back

    
    glPHD_isSelected = isSelected;
    glPHD_StarX = StarX;
    glPHD_StarY = StarY;
    glPHD_CurrentImageSizeX = currentPHDSizeX;
    glPHD_CurrentImageSizeY = currentPHDSizeY;
    glPHD_LockPositionX = LockedPositionX;
    glPHD_LockPositionY = LockedPositionY;
    glPHD_ShowLockCross = showLockedCross;

    // Logger::Log("ShowPHDdata | clear glPHD_Stars", LogLevel::DEBUG, DeviceType::GUIDER);
    glPHD_Stars.clear();
    // Logger::Log("ShowPHDdata | send ClearPHD2MultiStars", LogLevel::DEBUG, DeviceType::GUIDER);
    emit wsThread->sendMessageToClient("ClearPHD2MultiStars");
    for (int i = 1; i < MultiStarNumber; i++)
    {
        if (i > 12)
            break;
        QPoint p;
        p.setX(MultiStarX[i]);
        p.setY(MultiStarY[i]);
        glPHD_Stars.push_back(p);
        // qDebug() << "MultiStar[" << i << "]:" << MultiStarX[i] << ", " << MultiStarY[i];
        emit wsThread->sendMessageToClient("PHD2MultiStarsPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" + QString::number(glPHD_CurrentImageSizeY)+ ":" + QString::number(MultiStarX[i]) + ":" + QString::number(MultiStarY[i]));
    }

    // Logger::Log("ShowPHDdata | send PHD2StarBoxView and PHD2StarBoxPosition", LogLevel::DEBUG, DeviceType::GUIDER);
    // if (glPHD_StarX != 0 && glPHD_StarY != 0)
    // {
        if (glPHD_isSelected == true)
        {
            // qDebug() << "PHD2StarBoxPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" + QString::number(glPHD_CurrentImageSizeY) + ":" + QString::number(glPHD_StarX) + ":" + QString::number(glPHD_StarY);
            emit wsThread->sendMessageToClient("PHD2StarBoxView:true");
            emit wsThread->sendMessageToClient("PHD2StarBoxPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" + QString::number(glPHD_CurrentImageSizeY)+ ":" + QString::number(glPHD_StarX) + ":" + QString::number(glPHD_StarY));
        }
        else
        {
            emit wsThread->sendMessageToClient("PHD2StarBoxView:false");
        }

    // Logger::Log("ShowPHDdata | send PHD2StarCrossView and PHD2StarCrossPosition", LogLevel::DEBUG, DeviceType::GUIDER);
        if (glPHD_ShowLockCross == true)
        {
            emit wsThread->sendMessageToClient("PHD2StarCrossView:true");
            emit wsThread->sendMessageToClient("PHD2StarCrossPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" + QString::number(glPHD_CurrentImageSizeY) + ":" + QString::number(glPHD_LockPositionX) + ":" + QString::number(glPHD_LockPositionY));
        }
        else
        {
            emit wsThread->sendMessageToClient("PHD2StarCrossView:false");
        }
    // }

    if (sharedmemory_phd[2047] == 0x02 && bitDepth > 0 && currentPHDSizeX > 0 && currentPHDSizeY > 0)
    {
        // 导星过程中的数据
        unsigned char phdstatu;
        call_phd_checkStatus(phdstatu);
        // Logger::Log("ShowPHDdata | phdstatu:" + std::to_string(phdstatu), LogLevel::DEBUG, DeviceType::GUIDER);
        // Logger::Log("ShowPHDdata | dRa:" + std::to_string(dRa) + ", dDec:" + std::to_string(dDec), LogLevel::DEBUG, DeviceType::GUIDER);
        if (dRa != 0 && dDec != 0)
        {
            // Logger::Log("ShowPHDdata | dRa:" + std::to_string(dRa) + ", dDec:" + std::to_string(dDec), LogLevel::DEBUG, DeviceType::GUIDER);
            QPointF tmp;
            tmp.setX(-dRa * PixelRatio);
            tmp.setY(dDec * PixelRatio);
            glPHD_rmsdate.append(tmp);
            // Logger::Log("ShowPHDdata | send AddScatterChartData", LogLevel::DEBUG, DeviceType::GUIDER);
            emit wsThread->sendMessageToClient("AddScatterChartData:" + QString::number(-dRa * PixelRatio) + ":" + QString::number(-dDec * PixelRatio));

            // 图像中的小绿框
            if (InGuiding == true)
            {
                // Logger::Log("ShowPHDdata | send GuiderStatus:InGuiding", LogLevel::DEBUG, DeviceType::GUIDER);
                emit wsThread->sendMessageToClient("GuiderStatus:InGuiding");
                emit wsThread->sendMessageToClient("GuiderUpdateStatus:1");
            }
            else
            {
                // Logger::Log("ShowPHDdata | send GuiderStatus:InCalibration", LogLevel::DEBUG, DeviceType::GUIDER);
                emit wsThread->sendMessageToClient("GuiderStatus:InCalibration");
                emit wsThread->sendMessageToClient("GuiderUpdateStatus:1");
            }

            if (StarLostAlert == true)
            {
                // Logger::Log("ShowPHDdata | send GuiderStatus:StarLostAlert", LogLevel::DEBUG, DeviceType::GUIDER);
                emit wsThread->sendMessageToClient("GuiderStatus:StarLostAlert");
                emit wsThread->sendMessageToClient("GuiderUpdateStatus:2");
            }

            emit wsThread->sendMessageToClient("AddRMSErrorData:" + QString::number(RMSErrorX, 'f', 3) + ":" + QString::number(RMSErrorX, 'f', 3));
        }
        // Logger::Log("ShowPHDdata | glPHD_rmsdate.size():" + std::to_string(glPHD_rmsdate.size()), LogLevel::DEBUG, DeviceType::GUIDER);
        for (int i = 0; i < glPHD_rmsdate.size(); i++)
        {
            if (i == glPHD_rmsdate.size() - 1)
            {
                // Logger::Log("ShowPHDdata | send AddLineChartData", LogLevel::DEBUG, DeviceType::GUIDER);
                emit wsThread->sendMessageToClient("AddLineChartData:" + QString::number(i) + ":" + QString::number(glPHD_rmsdate[i].x()) + ":" + QString::number(glPHD_rmsdate[i].y()));
                if (i > 50)
                {
                    // Logger::Log("ShowPHDdata | send SetLineChartRange", LogLevel::DEBUG, DeviceType::GUIDER);
                    emit wsThread->sendMessageToClient("SetLineChartRange:" + QString::number(i - 50) + ":" + QString::number(i));
                } else {
                    // Logger::Log("ShowPHDdata | send SetLineChartRange", LogLevel::DEBUG, DeviceType::GUIDER);
                    emit wsThread->sendMessageToClient("SetLineChartRange:" + QString::number(0) + ":" + QString::number(50));
                }
            }
        }

        unsigned int byteCount;
        byteCount = currentPHDSizeX * currentPHDSizeY * (bitDepth / 8);

        unsigned char *srcData = new unsigned char[byteCount];

        mem_offset = 2048;
        // Logger::Log("ShowPHDdata | mem_offset:" + std::to_string(mem_offset), LogLevel::DEBUG, DeviceType::GUIDER);
        memcpy(srcData, sharedmemory_phd + mem_offset, byteCount);
        // Logger::Log("ShowPHDdata | get srcData", LogLevel::DEBUG, DeviceType::GUIDER);
        sharedmemory_phd[2047] = 0x00; // 0x00= image has been read

        // Logger::Log("ShowPHDdata | guider image start to process ...", LogLevel::DEBUG, DeviceType::GUIDER);
        cv::Mat img8;
        cv::Mat PHDImg;

        img8.create(currentPHDSizeY, currentPHDSizeX, CV_8UC1);
        if (bitDepth == 16)
            PHDImg.create(currentPHDSizeY, currentPHDSizeX, CV_16UC1);
        else
            PHDImg.create(currentPHDSizeY, currentPHDSizeX, CV_8UC1);

        PHDImg.data = srcData;

        uint16_t B = 0;
        uint16_t W = 65535;

        cv::Mat image_raw8;
        image_raw8.create(PHDImg.rows, PHDImg.cols, CV_8UC1);

        if (AutoStretch == true)
        {
            // Logger::Log("ShowPHDdata | Tools::GetAutoStretch", LogLevel::DEBUG, DeviceType::GUIDER);
            Tools::GetAutoStretch(PHDImg, 0, B, W);
        }
        else
        {
            B = 0;
            W = 65535;
        }

        // Logger::Log("ShowPHDdata | Tools::Bit16To8_Stretch", LogLevel::DEBUG, DeviceType::GUIDER);
        Tools::Bit16To8_Stretch(PHDImg, image_raw8, B, W);

        // Logger::Log("ShowPHDdata | saveGuiderImageAsJPG", LogLevel::DEBUG, DeviceType::GUIDER);
        saveGuiderImageAsJPG(image_raw8);

        // Logger::Log("ShowPHDdata | guider image process done", LogLevel::DEBUG, DeviceType::GUIDER);
        delete[] srcData;
        img8.release();
        PHDImg.release();
        // Logger::Log("ShowPHDdata finish!", LogLevel::DEBUG, DeviceType::GUIDER);
    }
}

void MainWindow::ControlGuide(int Direction, int Duration)
{
    Logger::Log("ControlGuide start ...", LogLevel::INFO, DeviceType::GUIDER);
    switch (Direction)
    {
    case 1:
    {
        if (dpMount != NULL)
        {
            if(isMeridianFlipped) {
                Logger::Log("ControlGuide | setTelescopeGuideNS Direction:0, Duration:" + std::to_string(Duration), LogLevel::INFO, DeviceType::GUIDER);
                indi_Client->setTelescopeGuideNS(dpMount, 0, Duration);
            } else {
                Logger::Log("ControlGuide | setTelescopeGuideNS Direction:" + std::to_string(Direction) + ", Duration:" + std::to_string(Duration), LogLevel::INFO, DeviceType::GUIDER);
                indi_Client->setTelescopeGuideNS(dpMount, Direction, Duration);
            }
        }
        break;
    }
    case 0:
    {
        if (dpMount != NULL)
        {
            if(isMeridianFlipped) {
                Logger::Log("ControlGuide | setTelescopeGuideNS Direction:1, Duration:" + std::to_string(Duration), LogLevel::INFO, DeviceType::GUIDER);
                indi_Client->setTelescopeGuideNS(dpMount, 1, Duration);
            } else {
                Logger::Log("ControlGuide | setTelescopeGuideNS Direction:" + std::to_string(Direction) + ", Duration:" + std::to_string(Duration), LogLevel::INFO, DeviceType::GUIDER);
                indi_Client->setTelescopeGuideNS(dpMount, Direction, Duration);
            }
        }
        break;
    }
    case 2:
    {
        if (dpMount != NULL)
        {   
            Logger::Log("ControlGuide | setTelescopeGuideWE Direction:" + std::to_string(Direction) + ", Duration:" + std::to_string(Duration), LogLevel::INFO, DeviceType::GUIDER);
            indi_Client->setTelescopeGuideWE(dpMount, Direction, Duration);
        }
        break;
    }
    case 3:
    {
        if (dpMount != NULL)
        {
            Logger::Log("ControlGuide | setTelescopeGuideWE Direction:" + std::to_string(Direction) + ", Duration:" + std::to_string(Duration), LogLevel::INFO, DeviceType::GUIDER);
            indi_Client->setTelescopeGuideWE(dpMount, Direction, Duration);
        }
        break;
    }
    default:
        break; //
    }
    Logger::Log("ControlGuide finish!", LogLevel::INFO, DeviceType::GUIDER);
}

void MainWindow::getTimeNow(int index)
{
    // 获取当前时间点
    auto now = std::chrono::system_clock::now();

    // 将当前时间点转换为毫秒
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    // 将毫秒时间戳转换为时间类型（std::time_t）
    std::time_t time_now = ms / 1000; // 将毫秒转换为秒

    // 使用 std::strftime 函数将时间格式化为字符串
    char buffer[80];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S",
                  std::localtime(&time_now));

    // 添加毫秒部分
    std::string formatted_time = buffer + std::to_string(ms % 1000);

    // 打印带有当前时间的输出
    // std::cout << "TIME(ms): " << formatted_time << "," << index << std::endl;
}

void MainWindow::onPHDControlGuideTimeout()
{
    // Logger::Log("PHD2 Control Guide is Timeout !", LogLevel::DEBUG, DeviceType::MAIN);
    GetPHD2ControlInstruct();
}

void MainWindow::GetPHD2ControlInstruct()
{
    // Logger::Log("GetPHD2ControlInstruct start ...", LogLevel::DEBUG, DeviceType::MAIN);
    std::lock_guard<std::mutex> lock(receiveMutex);

    unsigned int mem_offset;

    int sdk_direction = 0;
    int sdk_duration = 0;
    int sdk_num = 0;
    int zero = 0;
    mem_offset = 1024;

    mem_offset = mem_offset + sizeof(unsigned int);
    mem_offset = mem_offset + sizeof(unsigned int);
    mem_offset = mem_offset + sizeof(unsigned char);

    int ControlInstruct = 0;

    memcpy(&ControlInstruct, sharedmemory_phd + mem_offset, sizeof(int));
    // Logger::Log("GetPHD2ControlInstruct | get ControlInstruct:" + std::to_string(ControlInstruct), LogLevel::DEBUG, DeviceType::MAIN);
    int mem_offset_sdk_num = mem_offset;
    mem_offset = mem_offset + sizeof(int);

    sdk_num = (ControlInstruct >> 24) & 0xFFF;       // 取前12位
    sdk_direction = (ControlInstruct >> 12) & 0xFFF; // 取中间12位
    sdk_duration = ControlInstruct & 0xFFF;          // 取后12位

    if (sdk_num != 0)
    {
        getTimeNow(sdk_num);
        // Logger::Log("GetPHD2ControlInstruct | PHD2ControlTelescope:" + std::to_string(sdk_num) + "," + std::to_string(sdk_direction) + "," + std::to_string(sdk_duration), LogLevel::DEBUG, DeviceType::MAIN);
    }
    if (sdk_duration != 0)
    {
        MainWindow::ControlGuide(sdk_direction, sdk_duration);

        memcpy(sharedmemory_phd + mem_offset_sdk_num, &zero, sizeof(int));
        // Logger::Log("GetPHD2ControlInstruct | set ControlInstruct to 0", LogLevel::DEBUG, DeviceType::MAIN);
        call_phd_ChackControlStatus(sdk_num); // set pFrame->ControlStatus = 0;
    }
    // Logger::Log("GetPHD2ControlInstruct finish!", LogLevel::DEBUG, DeviceType::MAIN);
}


// double MainWindow::FocusGotoAndCalFWHM(int steps) {
//     QEventLoop loop;
//     double FWHM = 0;

//     // 停止和清理先前的计时器
//     FWHMTimer.stop();
//     FWHMTimer.disconnect();

//     FWHMCalOver = false;
//     FocuserControl_Goto(steps);

//     FWHMTimer.setSingleShot(true);

//     connect(&FWHMTimer, &QTimer::timeout, this, [this, &loop, &FWHM]() {
//         if (FWHMCalOver) 
//         {
//             FWHM = this->FWHM;  // 假设 this->FWHM 保存了计算结果
//             FWHMTimer.stop();
//             Logger::Log("FocusGotoAndCalFWHM | FWHM Calculation Complete!", LogLevel::INFO, DeviceType::MAIN);
//             emit wsThread->sendMessageToClient("FocusMoveDone:" + QString::number(FWHM));
//             loop.quit();
//         } 
//         else 
//         {
//             FWHMTimer.start(1000);  // 继续等待
//         }
//     });

//     FWHMTimer.start(1000);
//     loop.exec();  // 等待事件循环直到调用 loop.quit()

//     return FWHM;
// }

void MainWindow::HandleFocuserMovementDataPeriodically() {
    if (!isFocusMoveDone) {
        focusMoveTimer->stop();
        return;
    }

    CurrentPosition = FocuserControl_getPosition();
    if (CurrentPosition == INT_MIN) {
        Logger::Log("HandleFocuserMovementDataPeriodically | get current position failed!", LogLevel::WARNING, DeviceType::FOCUSER);
    } else {
        // Logger::Log("HandleFocuserMovementDataPeriodically | Current Position: " + std::to_string(CurrentPosition), LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
    }

    bool isInward = this->currentDirection; // 假设您有一个成员变量来存储当前的移动方向
    if (isInward) {
        if (CurrentPosition != TargetPosition && CurrentPosition-TargetPosition <= 1000) {
        
        }else if (CurrentPosition == TargetPosition){
            int steps = 60000+CurrentPosition;
            if (steps > 60000) {
                steps = 60000;
                TargetPosition = CurrentPosition - steps;
            }else{
                TargetPosition = -60000;
            }
            indi_Client->setFocuserMoveDiretion(dpFocuser, isInward);
            indi_Client->moveFocuserSteps(dpFocuser, steps);
            
            Logger::Log("HandleFocuserMovementDataPeriodically | CurrentPosition: " + std::to_string(CurrentPosition) + " ,set move steps "+ std::to_string(steps) + " ,backward inward", LogLevel::INFO, DeviceType::FOCUSER);
        }else{
            Logger::Log("Moving inward ... , CurrentPosition: " + std::to_string(CurrentPosition) , LogLevel::INFO, DeviceType::FOCUSER);
        }
    } else if (!isInward) {
        if (TargetPosition-CurrentPosition <= 1000) {
            int steps = 60000-CurrentPosition;
            if (steps > 60000) {
                steps = 60000;
                TargetPosition = CurrentPosition + steps;
            }else{
                TargetPosition = 60000;
            }

            indi_Client->setFocuserMoveDiretion(dpFocuser, isInward);
            indi_Client->moveFocuserSteps(dpFocuser, steps);
            Logger::Log("HandleFocuserMovementDataPeriodically | CurrentPosition: " + std::to_string(CurrentPosition) + " ,set move steps "+ std::to_string(steps) + " ,backward outward", LogLevel::INFO, DeviceType::FOCUSER);
        }else{
            Logger::Log("Moving outward ... , CurrentPosition: " + std::to_string(CurrentPosition), LogLevel::INFO, DeviceType::FOCUSER);
        }
    }
    focusMoveEndTime -= 0.5;
    if (focusMoveEndTime <= 0) {
        FocuserControlStop();
    } 
}

void MainWindow::FocuserControlMove(bool isInward) {
    this->currentDirection = isInward; // 更新当前方向
    if (dpFocuser == NULL) {
        Logger::Log("FocuserControlMove | dpFocuser is NULL", LogLevel::WARNING, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("FocusMoveDone:" + 0);
        return;
    }
    focusMoveEndTime = 2;
    isFocusMoveDone = true;
    CurrentPosition = FocuserControl_getPosition();
    TargetPosition = CurrentPosition;
    if (CurrentPosition == 60000 && !isInward) {
        emit wsThread->sendMessageToClient("FocusMoveToLimit:The current position has moved to the inner limit and cannot move further. If you need to continue moving, please recalibrate the position of the servo.");
        focusMoveTimer->stop();
        return;
    }else if (CurrentPosition == -60000 && isInward) {
        emit wsThread->sendMessageToClient("FocusMoveToLimit:The current position has moved to the outer limit and cannot move further. If you need to continue moving, please recalibrate the position of the servo.");
        focusMoveTimer->stop();
        return;
    }
    focusMoveTimer->start(1000);
}

void MainWindow::FocuserControlStop() {
    Logger::Log("focusMoveStop | Stop Focuser Move", LogLevel::INFO, DeviceType::FOCUSER);
    isFocusMoveDone = false;
    if(dpFocuser != NULL) {
        indi_Client->abortFocuserMove(dpFocuser);
    }else{
        Logger::Log("focusMoveStop | dpFocuser is NULL", LogLevel::WARNING, DeviceType::FOCUSER);
    }
    if(focusMoveTimer->isActive()) {
        focusMoveTimer->stop();
    }
    CurrentPosition = FocuserControl_getPosition();
    emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
    if (updatePositionTimer != nullptr) {
        // 添加计时器以定期更新位置
        updatePositionTimer->stop();
        updatePositionTimer->deleteLater();
        updatePositionTimer = nullptr;
    } 
    updatePositionTimer = new QTimer(this);
    updatePositionTimer->setInterval(1000); // 设置计时器间隔为1000毫秒
    updateCount = 0; // 初始化计数器    

    connect(updatePositionTimer, &QTimer::timeout, [this]() {
        if (isFocusMoveDone || updateCount >= 3) {
            updatePositionTimer->stop();
            updatePositionTimer->deleteLater();
            updatePositionTimer = nullptr;
            Logger::Log("focusMoveStop | Timer manually released", LogLevel::INFO, DeviceType::FOCUSER);
            return;
        }
        CurrentPosition = FocuserControl_getPosition();
        emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
        Logger::Log("focusMoveStop | Current Focuser Position: " + std::to_string(CurrentPosition), LogLevel::INFO, DeviceType::FOCUSER);
        updateCount++;
    });

    updatePositionTimer->start(); // 启动计时器
    
    Logger::Log("focusMoveStop | Current Focuser Position: " + std::to_string(CurrentPosition), LogLevel::INFO, DeviceType::FOCUSER);
}

void MainWindow::CheckFocuserMoveOrder() {
    emit wsThread->sendMessageToClient("getFocuserMoveState");
}



void MainWindow::FocuserControl_Move(bool isInward, int steps)
{
    // 记录开始移动焦点器的日志
    Logger::Log("FocuserControl_Move start ...", LogLevel::INFO, DeviceType::FOCUSER);
    if (dpFocuser != NULL) 
    {
        // 停止并断开焦点器移动的计时器
        focusTimer.stop();
        focusTimer.disconnect();

        // 获取当前焦点器的位置
        CurrentPosition = FocuserControl_getPosition();

        // 根据移动方向计算目标位置
        if(isInward == false)
        {
            TargetPosition = CurrentPosition + steps;
        }
        else
        {
            TargetPosition = CurrentPosition - steps;
        }
        // 记录目标位置的日志
        Logger::Log("FocuserControl_Move | Target Position: " + std::to_string(TargetPosition), LogLevel::INFO, DeviceType::FOCUSER);

        // 设置焦点器的移动方向并执行移动
        indi_Client->setFocuserMoveDiretion(dpFocuser, isInward);
        indi_Client->moveFocuserSteps(dpFocuser, steps);

        // 设置计时器为单次触发
        focusTimer.setSingleShot(true);

        // 连接计时器的超时信号到处理函数，用于监控焦点器的移动状态
        connect(&focusTimer, &QTimer::timeout, [this]() {
            // 更新当前位置
            CurrentPosition = FocuserControl_getPosition();
            // 向客户端发送当前位置和目标位置
            emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(TargetPosition));
            
            // 检查焦点器是否已经到达目标位置
            if (WaitForFocuserToComplete()) 
            {
                // 停止计时器
                focusTimer.stop();
                // 记录焦点器移动完成的日志
                Logger::Log("FocuserControl_Move | Focuser Move Complete!", LogLevel::INFO, DeviceType::FOCUSER);
                // 向客户端发送焦点器移动完成的消息
                emit wsThread->sendMessageToClient("FocusMoveDone:" + QString::number(FWHM));
                // 执行焦点循环处理
                FocusingLooping();
            } 
            else 
            {
                // 如果焦点器未到达目标位置，重新启动计时器，继续等待
                // focusTimer.start(100);
                emit wsThread->sendMessageToClient("FocusMoveDone:" + QString::number(FWHM));
            } 
        });

        // 启动计时器
        focusTimer.start(100);
    }
    else 
    {
        // 如果焦点器对象不存在，记录日志并发送错误消息
        Logger::Log("FocuserControl_Move | dpFocuser is NULL", LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("FocusMoveDone:" + 0);
    }
    // 记录焦点器移动结束的日志
    Logger::Log("FocuserControl_Move finish!", LogLevel::INFO, DeviceType::FOCUSER);
}

bool MainWindow::WaitForFocuserToComplete() {
   return(CurrentPosition == TargetPosition);
}

int MainWindow::FocuserControl_setSpeed(int speed)
{
  Logger::Log("FocuserControl_setSpeed start ...", LogLevel::DEBUG, DeviceType::FOCUSER);
  if (dpFocuser != NULL) 
  {
    int value, min, max, step;
    indi_Client->setFocuserSpeed(dpFocuser, speed);
    indi_Client->getFocuserSpeed(dpFocuser, value, min, max, step);
    Logger::Log("FocuserControl_setSpeed | Focuser Speed: " + std::to_string(value) + "," + std::to_string(min) + "," + std::to_string(max) + "," + std::to_string(step), LogLevel::INFO, DeviceType::FOCUSER);
    return value;
  }
  Logger::Log("FocuserControl_setSpeed finish!", LogLevel::DEBUG, DeviceType::FOCUSER);
}

int MainWindow::FocuserControl_getSpeed()
{
  Logger::Log("FocuserControl_getSpeed start ...", LogLevel::DEBUG, DeviceType::FOCUSER);
  if (dpFocuser != NULL) 
  {
    int value, min, max, step;
    indi_Client->getFocuserSpeed(dpFocuser, value, min, max, step);
    Logger::Log("FocuserControl_getSpeed | Focuser Speed: " + std::to_string(value) + "," + std::to_string(min) + "," + std::to_string(max) + "," + std::to_string(step), LogLevel::INFO, DeviceType::FOCUSER);
    return value;
  }
  Logger::Log("FocuserControl_getSpeed finish!", LogLevel::DEBUG, DeviceType::FOCUSER);
}

int MainWindow::FocuserControl_getPosition()
{
  Logger::Log("FocuserControl_getPosition start ...", LogLevel::DEBUG, DeviceType::FOCUSER);
  if (dpFocuser != NULL) 
  {
    int value;
    indi_Client->getFocuserAbsolutePosition(dpFocuser, value);
    Logger::Log("FocuserControl_getPosition | Focuser Position: " + std::to_string(value), LogLevel::DEBUG, DeviceType::FOCUSER);
    return value;
  }
  else
  {
    Logger::Log("FocuserControl_getPosition | dpFocuser is NULL", LogLevel::WARNING, DeviceType::FOCUSER);
    return INT_MIN; // 使用 INT_MIN 作为特殊的错误值
  }
  Logger::Log("FocuserControl_getPosition finish!", LogLevel::DEBUG, DeviceType::FOCUSER);
}

void MainWindow::TelescopeControl_Goto(double Ra,double Dec)
{
  if(dpMount!=NULL)
  {
    INDI::PropertyNumber property = NULL;
    indi_Client->slewTelescopeJNowNonBlock(dpMount,Ra,Dec,true,property);
  }
}

MountStatus MainWindow::TelescopeControl_Status()
{
  if (dpMount != NULL) 
  {
    MountStatus Stat;
    
    indi_Client->getTelescopeStatus(dpMount,Stat.status,Stat.error);
    
    return Stat;
  }
}

bool MainWindow::TelescopeControl_Park()
{
  bool isPark = false;
  if(dpMount!=NULL)
  {
    indi_Client->getTelescopePark(dpMount,isPark);
    if(isPark == false)
    {
      indi_Client->setTelescopePark(dpMount,true);
    }
    else
    {
      indi_Client->setTelescopePark(dpMount,false);
    }
    indi_Client->getTelescopePark(dpMount,isPark);
    Logger::Log("TelescopeControl_Park | Telescope is Park ???:" + std::to_string(isPark), LogLevel::INFO, DeviceType::MAIN);
  }

  return isPark;
}

bool MainWindow::TelescopeControl_Track()
{
  bool isTrack = true;
  if(dpMount!=NULL)
  {
    indi_Client->getTelescopeTrackEnable(dpMount,isTrack);
    if(isTrack == false)
    {
      indi_Client->setTelescopeTrackEnable(dpMount,true);
    }
    else
    {
      indi_Client->setTelescopeTrackEnable(dpMount,false);
    }
    indi_Client->getTelescopeTrackEnable(dpMount,isTrack);
    Logger::Log("TelescopeControl_Track | Telescope is Track ???:" + std::to_string(isTrack), LogLevel::INFO, DeviceType::MAIN);
  }
  return isTrack;
}


// TODO: 任务计划表数据解析
void MainWindow::ScheduleTabelData(QString message)
{
    ScheduleTargetNames.clear();
    m_scheduList.clear();
    schedule_currentShootNum = 0;
    QStringList ColDataList = message.split('[');
    for (int i = 1; i < ColDataList.size(); ++i)
    {
        QString ColData = ColDataList[i]; // ",M 24, Ra:4.785693,Dec:-0.323759,12:00:00,1 s,Ha,,Bias,ON,],"
        ScheduleData rowData;
        qDebug() << "ColData[" << i << "]:" << ColData;

        QStringList RowDataList = ColData.split(',');
        for (int j = 1; j < 10; ++j)
        {
            if (j == 1)
            {
                rowData.shootTarget = RowDataList[j];
                rowData.shootType = RowDataList[j + 7];
                qDebug() << "Target:" << rowData.shootTarget;
                qDebug() << "Type:" << rowData.shootType;
                // 将 shootTarget 添加到 ScheduleTargetNames 中
                if (!ScheduleTargetNames.isEmpty())
                {
                    ScheduleTargetNames += ",";
                }
                ScheduleTargetNames += rowData.shootTarget + "," + rowData.shootType ;
            }
            else if (j == 2)
            {
                QStringList parts = RowDataList[j].split(':');
                rowData.targetRa = Tools::RadToHour(parts[1].toDouble());
                qDebug() << "Ra:" << rowData.targetRa;
            }
            else if (j == 3)
            {
                QStringList parts = RowDataList[j].split(':');
                rowData.targetDec = Tools::RadToDegree(parts[1].toDouble());
                qDebug() << "Dec:" << rowData.targetDec;
            }
            else if (j == 4)
            {
                rowData.shootTime = RowDataList[j];
                qDebug() << "Time:" << rowData.shootTime;
            }
            else if (j == 5)
            {
                QStringList parts = RowDataList[j].split(' ');
                QString value = parts[0];
                QString unit = parts[1];
                if (unit == "s")
                    rowData.exposureTime = value.toInt() * 1000; // Convert seconds to milliseconds
                else if (unit == "ms")
                    rowData.exposureTime = value.toInt(); // Milliseconds
                if (rowData.exposureTime == 0)
                {
                    rowData.exposureTime = 1000;
                    qDebug() << "Exptime error, Exptime = 1000 ms";
                }
                qDebug() << "Exptime:" << rowData.exposureTime;
            }
            else if (j == 6)
            {
                rowData.filterNumber = RowDataList[j];
                qDebug() << "CFW:" << rowData.filterNumber;
            }
            else if (j == 7)
            {
                if (RowDataList[j] == "")
                {
                    rowData.repeatNumber = 1;
                    qDebug() << "Repeat error, Repeat = 1";
                }
                else
                {
                    rowData.repeatNumber = RowDataList[j].toInt();
                }
                qDebug() << "Repeat:" << rowData.repeatNumber;
            }
            // else if (j == 8)
            // {
            //     rowData.shootType = RowDataList[j];
            //     qDebug() << "Type:" << rowData.shootType;
            // }
            else if (j == 9)
            {
                rowData.resetFocusing = (RowDataList[j] == "ON");
                qDebug() << "Focus:" << rowData.resetFocusing;
            }
        }
        rowData.progress = 0;
        // scheduleTable.Data.push_back(rowData);
        m_scheduList.append(rowData);
    }
    startSchedule();
}


void MainWindow::startSchedule()
{
    createScheduleDirectory();
    if (schedule_currentNum >= 0 && schedule_currentNum < m_scheduList.size())
    {
        qDebug() << "startSchedule...";
        schedule_ExpTime = m_scheduList[schedule_currentNum].exposureTime;
        schedule_RepeatNum = m_scheduList[schedule_currentNum].repeatNumber;
        schedule_CFWpos = m_scheduList[schedule_currentNum].filterNumber.toInt();
        StopSchedule = false;
        startTimeWaiting();
    }
    else
    {
        qDebug() << "Index out of range, Schedule is complete!";
        StopSchedule = true;
        schedule_currentNum = 0;
        call_phd_StopLooping();
        GuidingHasStarted = false;
        // 在实际应用中，你可能想要返回一个默认值或者处理索引超出范围的情况
        // 这里仅仅是一个简单的示例
        // return ScheduleData();
    }
}

void MainWindow::nextSchedule()
{
    // next schedule...
    schedule_currentNum++;
    qDebug() << "next schedule...";
    startSchedule();
}

void MainWindow::startTimeWaiting()
{
    qDebug() << "startTimeWaiting...";
    // 停止和清理先前的计时器
    timewaitingTimer.stop();
    timewaitingTimer.disconnect();
    // 启动等待的定时器
    timewaitingTimer.setSingleShot(true);
    connect(&timewaitingTimer, &QTimer::timeout, [this]()
            {
        if (StopSchedule)
        {
            StopSchedule = false;
            qDebug("Schedule is stop!");
            return;
        }

        if (WaitForTimeToComplete()) 
        {
            timewaitingTimer.stop();  // 完成时停止定时器
            qDebug() << "Time Waiting Complete...";

            startMountGoto(m_scheduList[schedule_currentNum].targetRa, m_scheduList[schedule_currentNum].targetDec);
        } 
        else 
        {
            timewaitingTimer.start(1000);  // 继续等待
        } });
    timewaitingTimer.start(1000);
}

void MainWindow::startMountGoto(double ra, double dec) // Ra:Hour, Dec:Degree
{
    qDebug() << "startMountGoto...";
    // 停止和清理先前的计时器
    telescopeTimer.stop();
    telescopeTimer.disconnect();

    qDebug() << "Mount Goto:" << ra << "," << dec;
    MountGotoError = false;

    auto now = std::chrono::system_clock::now();
    double observatory_lon = 116.14; // 东经116.14度
    double lst = computeLST(observatorylongitude, now);

     //TelescopeControl_Goto(ra, dec);
    double RA_HOURS, DEC_DEGREE;
    indi_Client->getTelescopeRADECJNOW(dpMount, RA_HOURS, DEC_DEGREE);
    double CurrentRA_Degree = Tools::HourToDegree(RA_HOURS);
    double CurrentDEC_Degree = DEC_DEGREE;


    // performObservation(
    //     CurrentRA_Degree, CurrentDEC_Degree,
    //     ra, dec,
    //     observatorylongitude,observatorylatitude) ;
    performObservation(
        lst,CurrentDEC_Degree,
        ra, dec,
        observatorylongitude,observatorylatitude);
            
    call_phd_StopLooping();
    GuidingHasStarted = false;

    sleep(2); // 赤道仪的状态更新有一定延迟

    // 启动等待赤道仪转动的定时器
    telescopeTimer.setSingleShot(true);

    connect(&telescopeTimer, &QTimer::timeout, [this]()
            {
        if (StopSchedule)
        {
            StopSchedule = false;
            qDebug("Schedule is stop!");

            if (dpMount != NULL)
            {
                indi_Client->setTelescopeAbortMotion(dpMount);
            }

            return;
        }
        // 检查赤道仪状态
        if (WaitForTelescopeToComplete()) 
        {
            telescopeTimer.stop();  // 转动完成时停止定时器
            qDebug() << "Mount Goto Complete!";

            if(MountGotoError) {
                MountGotoError = false;

                nextSchedule();

                return;
            }

            if(GuidingHasStarted == false)
            {
                qDebug() << "Mount Goto Complete...";
                startSetCFW(schedule_CFWpos);
            }
        } 
        else 
        {
            telescopeTimer.start(1000);  // 继续等待
        } });

    telescopeTimer.start(1000);
}



bool MainWindow::needsMeridianFlip(double lst,double targetRA)
{
    double hourAngle=lst-targetRA;
    hourAngle=fmod(hourAngle+24.0,24.0);
    if(hourAngle>12.0)hourAngle-=24.0;
        return(hourAngle>0);
}

void MainWindow::performObservation(
    double lst,double currentDec,
    double targetRA,double targetDec,
    double observatoryLongitude,double observatoryLatitude){
    if (needsMeridianFlip(lst,targetRA))
    {
        std::cout<<"Meridian flip is needed."<<std::endl;
        TelescopeControl_Goto(lst,observatoryLatitude);
        std::cout<<"Performing meridian flip..."<<std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(60));
        TelescopeControl_Goto(targetRA,targetDec);

    }
    else{
        std::cout << "No flip needed. Moving directly."<<std::endl;
        TelescopeControl_Goto(targetRA,targetDec);
    }
}


// 计算儒略日（JD）
double MainWindow::getJulianDate(const std::chrono::system_clock::time_point& utc_time) {
    auto duration = utc_time.time_since_epoch();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    return 2440587.5 + seconds / 86400.0; // Unix时间转儒略日
}

// 计算格林尼治恒星时（GMST）
double MainWindow::computeGMST(const std::chrono::system_clock::time_point& utc_time) {
    const double JD = getJulianDate(utc_time);
    const double T = (JD - 2451545.0) / 36525.0; // 以J2000为基准的世纪数

    // IAU公式（精度±0.1秒）
    double GMST = 280.46061837 + 360.98564736629 * (JD - 2451545.0)
                  + 0.000387933 * T*T - T*T*T / 38710000.0;
    
    // 规范化到0-360度范围
    GMST = fmod(GMST, 360.0);
    if (GMST < 0) GMST += 360.0;
    
    return GMST / 15.0; // 转换为小时单位
}

// 计算地方恒星时（LST）
double MainWindow::computeLST(double longitude_east, const std::chrono::system_clock::time_point& utc_time) {
    double GMST_hours = computeGMST(utc_time);
    double LST = GMST_hours + longitude_east / 15.0; // 东经为正
    return fmod(LST + 24.0, 24.0); // 确保结果在0-24之间
}

void MainWindow::startGuiding()
{
    qDebug() << "startGuiding...";
    // 停止和清理先前的计时器
    guiderTimer.stop();
    guiderTimer.disconnect();

    GuidingHasStarted = true;
    call_phd_StartLooping();
    sleep(2);
    call_phd_AutoFindStar();
    call_phd_StartGuiding();

    // 启动等待赤道仪转动的定时器
    guiderTimer.setSingleShot(true);

    connect(&guiderTimer, &QTimer::timeout, [this]()
            {
        if (StopSchedule)
        {
            StopSchedule = false;
            call_phd_StopLooping();
            qDebug("Schedule is stop!");
            return;
        }
        // 检查赤道仪状态
        if (WaitForGuidingToComplete()) 
        {
            guiderTimer.stop();  // 转动完成时停止定时器
            qDebug() << "Guiding Complete...";

            // startCapture(schedule_ExpTime);
            startSetCFW(schedule_CFWpos);
        } 
        else 
        {
            guiderTimer.start(1000);  // 继续等待
        } });

    guiderTimer.start(1000);
}

void MainWindow::startSetCFW(int pos)
{
    qDebug() << "startSetCFW...";
    if (isFilterOnCamera)
    {
        if (dpMainCamera != NULL)
        {
            qDebug() << "schedule CFW pos:" << pos;
            indi_Client->setCFWPosition(dpMainCamera, pos);
            qDebug() << "CFW Goto Complete...";
            startCapture(schedule_ExpTime);
        }
    }
    else
    {
        if (dpCFW != NULL)
        {
            qDebug() << "schedule CFW pos:" << pos;
            indi_Client->setCFWPosition(dpCFW, pos);
            qDebug() << "CFW Goto Complete...";
            startCapture(schedule_ExpTime);
        }
    }
}

void MainWindow::startCapture(int ExpTime)
{
    qDebug() << "startCapture...";
    // 停止和清理先前的计时器
    captureTimer.stop();
    captureTimer.disconnect();

    ShootStatus = "InProgress";
    qDebug() << "ShootStatus: " << ShootStatus;
    INDI_Capture(ExpTime);
    schedule_currentShootNum++;

    captureTimer.setSingleShot(true);

    connect(&captureTimer, &QTimer::timeout, [this]()
            {
        if (StopSchedule)
        {
            StopSchedule = false;
            INDI_AbortCapture();
            qDebug("Schedule is stop!");
            return;
        }
        // 检查赤道仪状态
        if (WaitForShootToComplete()) 
        {
            captureTimer.stop();  // 转动完成时停止定时器
            qDebug() << "Capture" << schedule_currentShootNum << "Complete!";
            ScheduleImageSave(m_scheduList[schedule_currentNum].shootTarget, schedule_currentShootNum);
            // Process
            m_scheduList[schedule_currentNum].progress = (static_cast<double>(schedule_currentShootNum) / m_scheduList[schedule_currentNum].repeatNumber) * 100;
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));

            if (schedule_currentShootNum < schedule_RepeatNum)
            {
                startCapture(schedule_ExpTime);
            }
            else
            {
                schedule_currentShootNum = 0;

                // // next schedule...
                // schedule_currentNum ++;
                // qDebug() << "next schedule...";
                // startSchedule();
                qDebug() << "Capture Goto Complete...";
                nextSchedule();
            }

        } 
        else 
        {
            captureTimer.start(1000);  // 继续等待
        } });

    captureTimer.start(1000);
}

bool MainWindow::WaitForTelescopeToComplete()
{
    return (TelescopeControl_Status().status != "Slewing");
}

bool MainWindow::WaitForShootToComplete()
{
    qInfo("Wait For Shoot To Complete...");
    return (ShootStatus != "InProgress");
}

bool MainWindow::WaitForGuidingToComplete()
{
    qInfo() << "Wait For Guiding To Complete..." << InGuiding;
    return InGuiding;
}

bool MainWindow::WaitForTimeToComplete()
{
    qInfo() << "Wait For Time To Complete...";
    QString TargetTime = m_scheduList[schedule_currentNum].shootTime;

    // 如果获取到的目标时间不是完整的时间格式，直接返回 true
    if (TargetTime.length() != 5 || TargetTime[2] != ':')
        return true;

    // 获取当前时间
    QTime currentTime = QTime::currentTime();
    // 解析目标时间
    QTime targetTime = QTime::fromString(TargetTime, "hh:mm");

    qInfo() << "currentTime:" << currentTime << ", targetTime:" << targetTime;

    // 如果目标时间晚于当前时间，返回 false
    if (targetTime > currentTime)
        return false;

    // 目标时间早于或等于当前时间，返回 true
    return true;
}

int MainWindow::CaptureImageSave()
{
    qDebug() << "CaptureImageSave...";
    createCaptureDirectory();
    const char *sourcePath = "/dev/shm/ccd_simulator.fits";

    if (!QFile::exists("/dev/shm/ccd_simulator.fits"))
    {
        emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Null");
    }

    QString CaptureTime = Tools::getFitsCaptureTime("/dev/shm/ccd_simulator.fits");
    CaptureTime.replace(QRegExp("[^a-zA-Z0-9]"), "_");
    QString resultFileName = CaptureTime + ".fits";

    std::time_t currentTime = std::time(nullptr);
    std::tm *timeInfo = std::localtime(&currentTime);
    char buffer[80];
    std::strftime(buffer, 80, "%Y-%m-%d", timeInfo); // Format: YYYY-MM-DD

    // 指定目标目录
    QString destinationDirectory = ImageSaveBaseDirectory + "/CaptureImage";

    QString destinationPath = destinationDirectory + "/" + buffer + "/" + resultFileName;

    // 检查文件是否已存在
    if (QFile::exists(destinationPath))
    {
        qWarning() << "The file already exists, there is no need to save it again:" << destinationPath;
        emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Repeat");
        return 0; // 或返回其他状态码
    }

    // 将QString转换为const char*
    const char *destinationPathChar = destinationPath.toUtf8().constData();

    std::ifstream sourceFile(sourcePath, std::ios::binary);
    if (!sourceFile.is_open())
    {
        qWarning() << "Unable to open source file:" << sourcePath;
        return 1;
    }

    std::ofstream destinationFile(destinationPathChar, std::ios::binary);
    if (!destinationFile.is_open())
    {
        qWarning() << "Unable to create or open target file:" << destinationPathChar;
        return 1;
    }

    destinationFile << sourceFile.rdbuf();

    sourceFile.close();
    destinationFile.close();

    emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Success");
    qDebug() << "CaptureImageSaveStatus Goto Complete...";
    return 0;
}

int MainWindow::ScheduleImageSave(QString name, int num)
{
    qDebug() << "ScheduleImageSave...";
    const char *sourcePath = "/dev/shm/ccd_simulator.fits";

    name.replace(' ', '_');
    QString resultFileName = QString("%1-%2.fits").arg(name).arg(num);

    std::time_t currentTime = std::time(nullptr);
    std::tm *timeInfo = std::localtime(&currentTime);
    char buffer[80];
    std::strftime(buffer, 80, "%Y-%m-%d", timeInfo); // Format: YYYY-MM-DD

    // 指定目标目录
    QString destinationDirectory = ImageSaveBaseDirectory + "/ScheduleImage";

    // 拼接目标文件路径
    QString destinationPath = destinationDirectory + "/" + buffer + " " + QTime::currentTime().toString("hh") + "h (" + ScheduleTargetNames  +")" + "/" + resultFileName;

    // 将QString转换为const char*
    const char *destinationPathChar = destinationPath.toUtf8().constData();

    std::ifstream sourceFile(sourcePath, std::ios::binary);
    if (!sourceFile.is_open())
    {
        qWarning() << "Unable to open source file:" << sourcePath;
        return 1;
    }

    std::ofstream destinationFile(destinationPathChar, std::ios::binary);
    if (!destinationFile.is_open())
    {
        qWarning() << "Unable to create or open target file:" << destinationPathChar;
        return 1;
    }

    destinationFile << sourceFile.rdbuf();

    sourceFile.close();
    destinationFile.close();
    qDebug() << "ScheduleImageSave Goto Complete...";
    return 0;
}

bool MainWindow::directoryExists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool MainWindow::createScheduleDirectory() {
    Logger::Log("createScheduleDirectory start ...", LogLevel::INFO, DeviceType::MAIN);
    std::string basePath = ImageSaveBasePath + "/ScheduleImage";

    std::time_t currentTime = std::time(nullptr);
    std::tm* timeInfo = std::localtime(&currentTime);
    char buffer[80];
    std::strftime(buffer, 80, "%Y-%m-%d", timeInfo); // Format: YYYY-MM-DD
    std::string folderName = basePath + "/" + buffer + " " + QTime::currentTime().toString("hh").toStdString() + "h (" + ScheduleTargetNames.toStdString() + ")";

    // // Check if directory already exists
    // if (directoryExists(folderName)) {
    //     std::cout << "Directory already exists: " << folderName << std::endl;
    //     return true;
    // }

    // // Create directory
    // int status = mkdir(folderName.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    // if (status == 0) {
    //     std::cout << "Directory created successfully: " << folderName << std::endl;
    //     return true;
    // } else {
    //     std::cerr << "Failed to create directory: " << folderName << std::endl;
    //     return false;
    // }
    
    // 如果目录不存在，则创建
    if (!std::filesystem::exists(folderName))
    {
        if (std::filesystem::create_directory(folderName))
        {
            Logger::Log("createScheduleDirectory | Folder created successfully: " + std::string(folderName), LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            Logger::Log("createScheduleDirectory | An error occurred while creating the folder.", LogLevel::INFO, DeviceType::MAIN);
        }
    }
    else
    {
        Logger::Log("createScheduleDirectory | The folder already exists: " + std::string(folderName), LogLevel::INFO, DeviceType::MAIN);
    }
}

bool MainWindow::createCaptureDirectory() {
    Logger::Log("createCaptureDirectory start ...", LogLevel::INFO, DeviceType::MAIN);
    std::string basePath = ImageSaveBasePath + "/CaptureImage/";

    std::time_t currentTime = std::time(nullptr);
    std::tm* timeInfo = std::localtime(&currentTime);
    char buffer[80];
    std::strftime(buffer, 80, "%Y-%m-%d", timeInfo); // Format: YYYY-MM-DD
    std::string folderName = basePath + buffer;

    // // Check if directory already exists
    // if (directoryExists(folderName)) {
    //     std::cout << "Directory already exists: " << folderName << std::endl;
    //     return true;
    // }

    // // Create directory
    // int status = mkdir(folderName.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    // if (status == 0) {
    //     std::cout << "Directory created successfully: " << folderName << std::endl;
    //     return true;
    // } else {
    //     std::cerr << "Failed to create directory: " << folderName << std::endl;
    //     return false;
    // }

    // 如果目录不存在，则创建
    if (!std::filesystem::exists(folderName))
    {
        if (std::filesystem::create_directory(folderName))
        {
            Logger::Log("createCaptureDirectory | Folder created successfully: " + std::string(folderName), LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            Logger::Log("createCaptureDirectory | An error occurred while creating the folder.", LogLevel::INFO, DeviceType::MAIN);
        }
    }
    else
    {
        Logger::Log("createCaptureDirectory | The folder already exists: " + std::string(folderName), LogLevel::INFO, DeviceType::MAIN);
    }
}

void MainWindow::getClientSettings()
{
   
    std::string fileName = "config/config.ini";

    std::unordered_map<std::string, std::string> config;
   
    Tools::readClientSettings(fileName, config);

    Logger::Log("getClientSettings | Current Config:", LogLevel::INFO, DeviceType::MAIN);
    for (const auto &pair : config)
    {
        Logger::Log("getClientSettings | " + pair.first + " = " + pair.second, LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConfigureRecovery:" + QString::fromStdString(pair.first) + ":" + QString::fromStdString(pair.second));
        
        if(pair.first=="Coordinates")
        {
            QStringList coordinates =QString::fromStdString(pair.second).split(",");
            if(coordinates.size()==2)
            {
                observatorylongitude=coordinates[1].toDouble();
                observatorylatitude=coordinates[0].toDouble();
             }
        }
    }
    Logger::Log("getClientSettings finish!", LogLevel::INFO, DeviceType::MAIN);
}

// void MainWindow::getClientSettings() {
//     Logger::Log("getClientSettings start ...", LogLevel::INFO, DeviceType::MAIN);
//     std::string fileName = "config/config.ini";
    
//     std::unordered_map<std::string, std::string> config;

//     Tools::readClientSettings(fileName, config);

//     Logger::Log("getClientSettings | Current Config:", LogLevel::INFO, DeviceType::MAIN);
//     for (const auto& pair : config) {
//         Logger::Log("getClientSettings | " + pair.first + " = " + pair.second, LogLevel::INFO, DeviceType::MAIN);
//         emit wsThread->sendMessageToClient("ConfigureRecovery:" + QString::fromStdString(pair.first) + ":" + QString::fromStdString(pair.second));
//         if (pair.first == "Coordinates") {
//             std::string coordinate = pair.second;
//             std::stringstream ss(coordinate);
//             std::string item;
//             std::vector<std::string> elements;
//             while (std::getline(ss, item, ',')) {
//                 elements.push_back(item);
//             }
//             if (elements.size() == 2) {
//                 DEC = std::stod(elements[0]);
//                 RA = std::stod(elements[1]);
//             }
//         }
//     }
//     Logger::Log("getClientSettings finish!", LogLevel::INFO, DeviceType::MAIN);
// }

void MainWindow::setClientSettings(QString ConfigName, QString ConfigValue) {
    Logger::Log("setClientSettings start ...", LogLevel::INFO, DeviceType::MAIN);
    std::string fileName = "config/config.ini";
    
    std::unordered_map<std::string, std::string> config;

    config[ConfigName.toStdString()] = ConfigValue.toStdString();

    Logger::Log("setClientSettings | Save Client Setting:" + ConfigName.toStdString() + " = " + ConfigValue.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    Tools::saveClientSettings(fileName, config);
    Logger::Log("setClientSettings finish!", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::getConnectedDevices(){
    Logger::Log("getConnectedDevices start ...", LogLevel::INFO, DeviceType::MAIN);
    QString deviceType;
    bool isConnect;
    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {   
        deviceType = systemdevicelist.system_devices[i].Description;
        isConnect = systemdevicelist.system_devices[i].isConnect;
        if (deviceType != "" && isConnect){
            emit wsThread->sendMessageToClient("AddDeviceType:" + deviceType);
        }
    }



    for(int i = 0; i < indi_Client->GetDeviceCount(); i++){
        if (indi_Client->GetDeviceFromList(i)->isConnected()){
            emit wsThread->sendMessageToClient("DeviceToBeAllocated:Device:" + QString::number(i) + ":" + QString::fromUtf8(indi_Client->GetDeviceFromList(i)->getDeviceName()));
        }
    }

    for(int i = 0; i < ConnectedDevices.size(); i++){
        Logger::Log("getConnectedDevices | Device[" + std::to_string(i) + "]: " + ConnectedDevices[i].DeviceName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectSuccess:" + ConnectedDevices[i].DeviceType + ":" + ConnectedDevices[i].DeviceName);

        if(ConnectedDevices[i].DeviceType == "MainCamera" && dpMainCamera != NULL) {
            emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));
            emit wsThread->sendMessageToClient("MainCameraOffsetRange:" + QString::number(glOffsetMin) + ":" + QString::number(glOffsetMax));
            emit wsThread->sendMessageToClient("MainCameraGainRange:" + QString::number(glGainMin) + ":" + QString::number(glGainMax));

            QString CFWname;
            indi_Client->getCFWSlotName(dpMainCamera, CFWname);
            if(CFWname != "") {
                Logger::Log("getConnectedDevices | get CFW Slot Name: " + CFWname.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("ConnectSuccess:CFW:" + CFWname +" (on camera)");
                isFilterOnCamera = true;

                int min, max, pos;
                indi_Client->getCFWPosition(dpMainCamera, pos, min, max);
                Logger::Log("getConnectedDevices | getCFWPosition: " + std::to_string(min) + ", " + std::to_string(max) + ", " + std::to_string(pos), LogLevel::INFO, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("CFWPositionMax:" + QString::number(max));
            }
        }
    }
    Logger::Log("getConnectedDevices finish!", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::clearConnectedDevices() {
    ConnectedDevices.clear();
}

void MainWindow::getStagingImage(){
    if(isStagingImage && SavedImage != "" && isFileExists(QString::fromStdString(vueImagePath+SavedImage))){
        Logger::Log("getStagingImage | ready to upload image: " + SavedImage, LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("SaveBinSuccess:" + QString::fromStdString(SavedImage));
    }else{
        Logger::Log("getStagingImage | no image to upload", LogLevel::INFO, DeviceType::MAIN);
    }
}

bool MainWindow::isFileExists(const QString& filePath) {
    QFile file(filePath);
    if (file.exists()) {
        Logger::Log("isFileExists | file exists: " + filePath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    } else {
        Logger::Log("isFileExists | file does not exist: " + filePath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    }
    return file.exists();
}

void MainWindow::getStagingScheduleData(){
    if(isStagingScheduleData) {
        // emit wsThread->sendMessageToClient("RecoveryScheduleData:" + StagingScheduleData);
        emit wsThread->sendMessageToClient(StagingScheduleData);
    }
}

void MainWindow::getStagingGuiderData() {
    int dataSize = glPHD_rmsdate.size();
    int startIdx = dataSize > 50 ? dataSize - 50 : 0;

    for (int i = startIdx; i < dataSize; i++)
    {
        emit wsThread->sendMessageToClient("AddLineChartData:" + QString::number(i) + ":" + QString::number(glPHD_rmsdate[i].x()) + ":" + QString::number(glPHD_rmsdate[i].y()));
        emit wsThread->sendMessageToClient("AddScatterChartData:" + QString::number(glPHD_rmsdate[i].x()) + ":" + QString::number(-glPHD_rmsdate[i].y()));
        if (i > 50) {
            emit wsThread->sendMessageToClient("SetLineChartRange:" + QString::number(i - 50) + ":" + QString::number(i));
        }
    }
}

int MainWindow::MoveFileToUSB(){
    qDebug("MoveFileToUSB");

}

void MainWindow::TelescopeControl_SolveSYNC()
{
    Logger::Log("TelescopeControl_SolveSYNC start ...", LogLevel::INFO, DeviceType::MAIN);
    int FocalLength = glFocalLength;  // 获取焦距

    double Ra_Hour;
    double Dec_Degree;

    if (dpMount != NULL)
    {
        indi_Client->getTelescopeRADECJNOW(dpMount, Ra_Hour, Dec_Degree);  // 获取当前望远镜的赤经和赤纬
    }
    else
    {
        Logger::Log("TelescopeControl_SolveSYNC | No Mount Connect.", LogLevel::INFO, DeviceType::MAIN);
        return;  // 如果望远镜未连接，记录日志并退出
    }
    double Ra_Degree = Tools::HourToDegree(Ra_Hour);  // 将赤经从小时转换为度

    Logger::Log("TelescopeControl_SolveSYNC | CurrentRa(Degree):" + std::to_string(Ra_Degree) + "," + "CurrentDec(Degree):" + std::to_string(Dec_Degree), LogLevel::INFO, DeviceType::MAIN);

    // SloveResults result = Tools::PlateSolve("/dev/shm/ccd_simulator.fits", FocalLength, glCameraSize_width, glCameraSize_height, false);
    SloveResults result;

    solveTimer.stop();  // 停止之前的定时器
    solveTimer.disconnect();  // 断开定时器的连接

    // QObject::connect(platesolveworker, &PlateSolveWorker::resultReady, [this](const SloveResults &result) {});  // 连接解析结果的信号

    // platesolveworker->setParams(SolveImageFileName, FocalLength, glCameraSize_width, glCameraSize_height, false); // 设置解析参数
    // platesolveworker->start();  // 开始解析
    Tools::PlateSolve(SolveImageFileName, FocalLength, glCameraSize_width, glCameraSize_height, false);

    solveTimer.setSingleShot(true);  // 设置定时器为单次触发

    connect(&solveTimer, &QTimer::timeout, [this]()
    {
        if (Tools::isSolveImageFinish())  // 检查图像解析是否完成
        {
            SloveResults result = Tools::ReadSolveResult(SolveImageFileName, SolveImageWidth, SolveImageHeight);  // 读取解析结果
            Logger::Log("TelescopeControl_SolveSYNC | Plate Solve Result(RA_Degree, DEC_Degree):" + std::to_string(result.RA_Degree) + ", " + std::to_string(result.DEC_Degree), LogLevel::INFO, DeviceType::MAIN);

            if (result.RA_Degree == -1 && result.DEC_Degree == -1)
            {
                Logger::Log("TelescopeControl_SolveSYNC | Solve image failed...", LogLevel::INFO, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("SolveImagefailed");  // 发送解析失败的消息
            }
            else
            {
                if (dpMount != NULL)
                {
                    INDI::PropertyNumber property = NULL;
                    Logger::Log("TelescopeControl_SolveSYNC | syncTelescopeJNow | start", LogLevel::INFO, DeviceType::MAIN);
                    QString action = "SYNC";

                    indi_Client->setTelescopeActionAfterPositionSet(dpMount, action);  // 设置望远镜的同步动作

                    Logger::Log("TelescopeControl_SolveSYNC | DegreeToHour:" + std::to_string(Tools::DegreeToHour(result.RA_Degree)) + "DEC_Degree:" + std::to_string(result.DEC_Degree), LogLevel::INFO, DeviceType::MAIN);

                    indi_Client->setTelescopeRADECJNOW(dpMount, Tools::DegreeToHour(result.RA_Degree), result.DEC_Degree, property);  // 设置望远镜的目标位置
                    Logger::Log("TelescopeControl_SolveSYNC | syncTelescopeJNow | end", LogLevel::INFO, DeviceType::MAIN);
                    double a, b;
                    indi_Client->getTelescopeRADECJNOW(dpMount, a, b);  // 获取望远镜的当前位置
                    Logger::Log("TelescopeControl_SolveSYNC | Get_RA_Hour:" + std::to_string(a) + "Get_DEC_Degree:" + std::to_string(b), LogLevel::INFO, DeviceType::MAIN);
                }
                else
                {
                    Logger::Log("TelescopeControl_SolveSYNC | No Mount Connect.", LogLevel::INFO, DeviceType::MAIN);
                    return;  // 如果望远镜未连接，记录日志并退出
                }
            }
        }
        else 
        {
            solveTimer.start(1000);  // 如果解析未完成，重新启动定时器继续等待
        } 
    });

    solveTimer.start(1000);  // 启动定时器
}

LocationResult MainWindow::TelescopeControl_GetLocation()
{
  if (dpMount != NULL) 
  {
    LocationResult result;
    
    indi_Client->getLocation(dpMount, result.latitude_degree, result.longitude_degree, result.elevation);

    return result;
  }
}

QDateTime MainWindow::TelescopeControl_GetTimeUTC()
{
  if (dpMount != NULL) 
  {
    QDateTime result;
    
    indi_Client->getTimeUTC(dpMount, result);

    return result;
  }
}

SphericalCoordinates MainWindow::TelescopeControl_GetRaDec()
{
  if (dpMount != NULL) 
  {
    SphericalCoordinates result;
    double RA_HOURS, DEC_DEGREE;
    indi_Client->getTelescopeRADECJNOW(dpMount, RA_HOURS, DEC_DEGREE);
    result.ra = Tools::HourToDegree(RA_HOURS);
    result.dec = DEC_DEGREE;

    return result;
  }
}

void MainWindow::MountGoto(double Ra_Hour, double Dec_Degree)
{
    Logger::Log("MountGoto start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("MountGoto | RaDec(Hour):" + std::to_string(Ra_Hour) + "," + std::to_string(Dec_Degree), LogLevel::INFO, DeviceType::MAIN);

    // 停止和清理先前的计时器
    telescopeTimer.stop();
    telescopeTimer.disconnect();

    TelescopeControl_Goto(Ra_Hour, Dec_Degree);

    sleep(2); // 赤道仪的状态更新有一定延迟

    // 启动等待赤道仪转动的定时器
    telescopeTimer.setSingleShot(true);

    connect(&telescopeTimer, &QTimer::timeout, [this]()
    {
        // 检查赤道仪状态
        if (WaitForTelescopeToComplete()) 
        {
            telescopeTimer.stop();  // 转动完成时停止定时器
            Logger::Log("MountGoto | Mount Goto Complete!", LogLevel::INFO, DeviceType::MAIN);
        } 
        else 
        {
            telescopeTimer.start(1000);  // 继续等待
        } });

    telescopeTimer.start(1000);

    Logger::Log("MountGoto finish!", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::DeleteImage(QStringList DelImgPath)
{
    std::string password = "quarcs"; // sudo 密码
    for (int i = 0; i < DelImgPath.size(); i++) {
        if (i < DelImgPath.size()) {
            std::ostringstream commandStream;
            commandStream << "echo '" << password << "' | sudo -S rm -rf \"./" << DelImgPath[i].toStdString() << "\"";
            std::string command = commandStream.str();

            Logger::Log("DeleteImage | Deleted command:" + QString::fromStdString(command).toStdString(), LogLevel::INFO, DeviceType::MAIN);
        
            // 执行系统命令删除文件
            int result = system(command.c_str());
        
            if (result == 0) {
                Logger::Log("DeleteImage | Deleted file:" + DelImgPath[i].toStdString(), LogLevel::INFO, DeviceType::MAIN);
            } else {
                Logger::Log("DeleteImage | Failed to delete file:" + DelImgPath[i].toStdString(), LogLevel::INFO, DeviceType::MAIN);
            }
        } else {
            Logger::Log("DeleteImage | Index out of range: " + std::to_string(i), LogLevel::WARNING, DeviceType::MAIN);
        }
    }
}

std::string MainWindow::GetAllFile()
{
    Logger::Log("GetAllFile start ...", LogLevel::INFO, DeviceType::MAIN);
    std::string capturePath = ImageSaveBasePath + "/CaptureImage/";
    std::string planPath = ImageSaveBasePath + "/ScheduleImage/";
    std::string resultString;
    std::string captureString = "CaptureImage{";
    std::string planString = "ScheduleImage{";
    for (const auto &entry : std::filesystem::directory_iterator(capturePath))
    {
        std::string fileName = entry.path().filename().string(); // 获取文件名（包含扩展名）
        captureString += fileName + ";";                // 拼接为字符串
    }
    for (const auto &entry : std::filesystem::directory_iterator(planPath))
    {
        std::string folderName = entry.path().filename().string(); // 获取文件夹名
        planString += folderName + ";";
    }
    resultString = captureString + "}:" + planString + '}';
    Logger::Log("GetAllFile finish!", LogLevel::INFO, DeviceType::MAIN);
    return resultString;
}

void MainWindow::GetImageFiles(std::string ImageFolder) {
    Logger::Log("GetImageFiles start ...", LogLevel::INFO, DeviceType::MAIN);
    std::string basePath = ImageSaveBasePath + "/" + ImageFolder + "/";
    std::string ImageFilesNameString = "";
    for (const auto &entry : std::filesystem::directory_iterator(basePath))
    {
        std::string fileName = entry.path().filename().string(); // 获取文件名（包含扩展名）
        ImageFilesNameString += fileName + ";";                // 拼接为字符串
    }
    Logger::Log("GetImageFiles | Image Files:" + QString::fromStdString(ImageFilesNameString).toStdString(), LogLevel::INFO, DeviceType::MAIN);

    emit wsThread->sendMessageToClient("ImageFilesName:" + QString::fromStdString(ImageFilesNameString));
    Logger::Log("GetImageFiles finish!", LogLevel::INFO, DeviceType::MAIN);
}


// std::string MainWindow::GetAllFile()
// {
//     std::string capturePath = ImageSaveBasePath + "/CaptureImage/";
//     std::string planPath = ImageSaveBasePath + "/ScheduleImage/";
//     std::string resultString;
//     std::string captureString = "CaptureImage{";
//     std::string planString = "ScheduleImage{";

//     // 读取CaptureImage文件夹中的子文件夹，并进一步读取图像文件
//     for (const auto &entry : std::filesystem::directory_iterator(capturePath))
//     {
//         std::string folderName = entry.path().filename().string(); // 获取文件夹名
//         if (entry.is_directory()) {
//             captureString += folderName + "{";

//             // 遍历文件夹中的所有文件（假设都是.fits文件）
//             for (const auto &imageEntry : std::filesystem::directory_iterator(entry.path()))
//             {
//                 std::string imageName = imageEntry.path().filename().string();
//                 if (imageEntry.is_regular_file()) {
//                     captureString += imageName + ";"; // 拼接图像文件名
//                 }
//             }

//             captureString += "}"; // 结束当前文件夹的图像列表
//         }
//     }

//     // 读取ScheduleImage文件夹中的文件夹名，并进一步读取图像文件
//     for (const auto &entry : std::filesystem::directory_iterator(planPath))
//     {
//         std::string folderName = entry.path().filename().string(); // 获取文件夹名
//         if (entry.is_directory()) {
//             planString += folderName + "{";

//             // 遍历文件夹中的所有文件（假设都是.fits文件）
//             for (const auto &imageEntry : std::filesystem::directory_iterator(entry.path()))
//             {
//                 std::string imageName = imageEntry.path().filename().string();
//                 if (imageEntry.is_regular_file()) {
//                     planString += imageName + ";"; // 拼接图像文件名
//                 }
//             }

//             planString += "}"; // 结束当前文件夹的图像列表
//         }
//     }

//     resultString = captureString + "}:" + planString + "}";
//     return resultString;
// }



// 解析字符串
QStringList MainWindow::parseString(const std::string &input, const std::string &imgFilePath) {
    QStringList paths;
    QString baseString;
    size_t pos = input.find('{');
    if (pos != std::string::npos) {
        baseString = QString::fromStdString(input.substr(0, pos));
        std::string content = input.substr(pos + 1);
        size_t endPos = content.find('}');
        if (endPos != std::string::npos) {
            content = content.substr(0, endPos);

            // 去掉末尾的分号（如果有的话）
            if (!content.empty() && content.back() == ';') {
                content.pop_back();
            }

            QStringList parts = QString::fromStdString(content).split(';', Qt::SkipEmptyParts);
            for (const QString &part : parts) {
                QString path = QDir::toNativeSeparators(QString::fromStdString(imgFilePath) + "/" + baseString + "/" + part);
                paths.append(path);
            }
        }
    }
    return paths;
}

// 返回 U 盘剩余内存
long long MainWindow::getUSBSpace(const QString &usb_mount_point)
{
    Logger::Log("getUSBSpace start ...", LogLevel::INFO, DeviceType::MAIN);
    struct statvfs stat;
    if (statvfs(usb_mount_point.toUtf8().constData(), &stat) == 0) {
        long long free_space = static_cast<long long>(stat.f_bfree) * stat.f_frsize;
        Logger::Log("getUSBSpace | USB Space: " + std::to_string(free_space) + " bytes", LogLevel::INFO, DeviceType::MAIN);
        return free_space;
    } else {
        Logger::Log("getUSBSpace | Failed to obtain the space information of the USB flash drive.", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("getUSBFail:Failed to obtain the space information of the USB flash drive." );
        return -1;
    }
}

long long MainWindow::getTotalSize(const QStringList &filePaths) 
{
    long long totalSize = 0;
    foreach(QString filePath, filePaths) {
        QFileInfo fileInfo(filePath);
        if(fileInfo.exists()) {
            totalSize += fileInfo.size();
        }
    }
    return totalSize;
}

// 获取文件系统挂载模式
bool MainWindow::isMountReadOnly(const QString& mountPoint) {
    struct statvfs fsinfo;
    auto mountPointStr = mountPoint.toUtf8().constData();
    Logger::Log("isMountReadOnly | Checking filesystem information for mount point:" + QString::fromUtf8(mountPointStr).toStdString(), LogLevel::INFO, DeviceType::MAIN);

    if (statvfs(mountPointStr, &fsinfo) != 0) {
        Logger::Log("isMountReadOnly | Failed to get filesystem information for" + mountPoint.toStdString() + ":" + strerror(errno), LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(QString("getUSBFail:Failed to get filesystem information for %1, error: %2").arg(mountPoint).arg(strerror(errno)));
        return false;
    }

    Logger::Log("isMountReadOnly | Filesystem flags for" + mountPoint.toStdString() + ":" + std::to_string(fsinfo.f_flag), LogLevel::INFO, DeviceType::MAIN);
    return (fsinfo.f_flag & ST_RDONLY) != 0;
}


// 将文件系统挂载模式更改为读写模式
bool MainWindow::remountReadWrite(const QString& mountPoint, const QString& password) {
    QProcess process;
    process.start("sudo", {"-S", "mount", "-o", "remount,rw", mountPoint});
    if (!process.waitForStarted() || !process.write((password + "\n").toUtf8())) {
        Logger::Log("remountReadWrite | Failed to execute command: sudo mount", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("getUSBFail:Failed to execute command: sudo mount -o remount,rw usb." );
        return false;
    }
    process.closeWriteChannel();
    process.waitForFinished(-1);
    return process.exitCode() == 0;
}

void MainWindow::RemoveImageToUsb(QStringList RemoveImgPath)
{
    QString base = "/media/";
    QString username = QDir::home().dirName();
    QString basePath = base + username;
    QDir baseDir(basePath);
    QString usb_mount_point = "";
    if (!baseDir.exists()) {
        Logger::Log("RemoveImageToUsb | Base directory does not exist.", LogLevel::WARNING, DeviceType::MAIN);
        return ;
    }

    // 获取所有文件夹，排除"."和".."，并且排除"CDROM"
    QStringList filters;
    filters << "*";
    QStringList folderList = baseDir.entryList(filters, QDir::Dirs | QDir::NoDotAndDotDot);

    // 排除包含"CDROM"的文件夹
    folderList.removeAll("CDROM");
    
    // 检查剩余文件夹数量是否为1
    if (folderList.size() == 1) {
        usb_mount_point = basePath + "/" + folderList.at(0);
        Logger::Log("RemoveImageToUsb | USB mount point:" + usb_mount_point.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    } else if (folderList.size() == 0)  {
        emit wsThread->sendMessageToClient("ImageSaveErroe:USB-Null");
        Logger::Log("RemoveImageToUsb | The directory does not contain exactly one folder excluding CDROM.", LogLevel::WARNING, DeviceType::MAIN);
        return;
    } else {
        emit wsThread->sendMessageToClient("ImageSaveErroe:USB-Multiple");
        Logger::Log("RemoveImageToUsb | The directory does not contain exactly one folder excluding CDROM.", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }

    const QString password = "quarcs"; // sudo 密码

    QStorageInfo storageInfo(usb_mount_point);
    if (storageInfo.isValid() && storageInfo.isReady()) {
        if (storageInfo.isReadOnly()) {
            // 处理1: 该路径为只读设备
           if (!remountReadWrite(usb_mount_point, password)) {
            Logger::Log("RemoveImageToUsb | Failed to remount filesystem as read-write.", LogLevel::WARNING, DeviceType::MAIN);
            return;
            }
            Logger::Log("RemoveImageToUsb | Filesystem remounted as read-write successfully.", LogLevel::INFO, DeviceType::MAIN);
        }
        Logger::Log("RemoveImageToUsb | This path is for writable devices.", LogLevel::INFO, DeviceType::MAIN);
    }else{
        Logger::Log("RemoveImageToUsb | The specified path is not a valid file system or is not ready.", LogLevel::WARNING, DeviceType::MAIN);
    }
    long long remaining_space = getUSBSpace(usb_mount_point);
    if (remaining_space == -1){
        Logger::Log("RemoveImageToUsb | Check whether a USB flash drive or portable hard drive is inserted!", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("getUSBFail:Check whether a USB flash drive or portable hard drive is inserted!" );
        return ;
    }
    long long totalSize = getTotalSize(RemoveImgPath);
    if (totalSize >= remaining_space) {
        Logger::Log("RemoveImageToUsb | Insufficient storage space, unable to copy files to USB drive!", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("getUSBFail:Not enough storage space to copy files to USB flash drive!" );
        return;
    }
    QDateTime currentDateTime = QDateTime::currentDateTime();
    QString folderName = "QUARCS_ImageSave";
    QString folderPath = usb_mount_point + "/" + folderName;

    int sumMoveImage = 0;
    for (const auto &imgPath : RemoveImgPath) {
        QString fileName = imgPath;
        int pos = fileName.lastIndexOf('/');
        int pos1 = fileName.lastIndexOf("/", pos - 1);
        if (pos ==-1 || pos1 == -1)
        {
            Logger::Log("RemoveImageToUsb | path is error!", LogLevel::WARNING, DeviceType::MAIN);
            return;
        }
        QString destinationPath = folderPath + fileName.mid(pos1, pos - pos1 + 1);
        QProcess process;
        process.start("sudo", {"-S", "mkdir", "-p", destinationPath});
        if (!process.waitForStarted() || !process.write((password + "\n").toUtf8())) {
            Logger::Log("RemoveImageToUsb | Failed to execute command: sudo mkdir.", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("HasMoveImgnNUmber:fail:" + QString::number(sumMoveImage));
            continue;
        }
        process.closeWriteChannel();
        process.waitForFinished(-1);

        process.start("sudo", {"-S", "cp", "-r", imgPath, destinationPath });
        if (!process.waitForStarted() || !process.write((password + "\n").toUtf8())) {
            Logger::Log("RemoveImageToUsb | Failed to execute command: sudo cp.", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("HasMoveImgnNUmber:fail:" + QString::number(sumMoveImage));
            continue;
        }
        process.closeWriteChannel();
        process.waitForFinished(-1);

        // Read the standard error output
        QByteArray stderrOutput = process.readAllStandardError();

        if (process.exitCode() == 0) {
            Logger::Log("RemoveImageToUsb | Copied file: "  + imgPath.toStdString() + " to " + destinationPath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        } else {
            Logger::Log("RemoveImageToUsb | Failed to copy file: " + imgPath.toStdString() + " to " + destinationPath.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
            // Print the error reason
            Logger::Log("RemoveImageToUsb | Error: " + QString::fromUtf8(stderrOutput).toStdString(), LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("HasMoveImgnNUmber:fail:" + QString::number(sumMoveImage));
            continue;
        }
        sumMoveImage ++;
        emit wsThread->sendMessageToClient("HasMoveImgnNUmber:succeed:" + QString::number(sumMoveImage));
    }
}

void MainWindow::USBCheck(){
    QString message;
    QString base = "/media/";
    QString username = QDir::home().dirName();
    QString basePath = base + username;
    QDir baseDir(basePath);
    QString usb_mount_point = "";
    if (!baseDir.exists()) {
        Logger::Log("USBCheck | Base directory does not exist.", LogLevel::WARNING, DeviceType::MAIN);
        return ;
    }

    // 获取所有文件夹，排除"."和".."，并且排除"CDROM"
    QStringList filters;
    filters << "*";
    QStringList folderList = baseDir.entryList(filters, QDir::Dirs | QDir::NoDotAndDotDot);

    // 排除包含"CDROM"的文件夹
    folderList.removeAll("CDROM");

    if (folderList.size() == 1) {
        usb_mount_point = basePath + "/" + folderList.at(0);
        Logger::Log("USBCheck | USB mount point:" + usb_mount_point.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        QString usbName = folderList.join(",");
        message = "USBCheck";
        long long remaining_space = getUSBSpace(usb_mount_point);
        if (remaining_space == -1){
            Logger::Log("USBCheck | Check whether a USB flash drive or portable hard drive is inserted!", LogLevel::WARNING, DeviceType::MAIN);
            return ;
        }
        message = message + ":" + folderList.at(0) + "," + QString::number(remaining_space);
        Logger::Log("USBCheck | " + message.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(message);
    } else if (folderList.size() == 0) {
        emit wsThread->sendMessageToClient("USBCheck:Null, Null");
        return;
    } else {
        emit wsThread->sendMessageToClient("USBCheck:Multiple, Multiple");
        return;
    }

    return ;
}

void MainWindow::SolveImage(QString Filename, int FocalLength, double CameraWidth, double CameraHeight) {
    Logger::Log("SolveImage(" + Filename.toStdString() + ") start ...", LogLevel::INFO, DeviceType::MAIN);

    // 停止和清理先前的计时器
    solveTimer.stop();
    solveTimer.disconnect();

    // Tools::PlateSolve(Filename, FocalLength, CameraWidth, CameraHeight, 4656, 3522, false);

    // QObject::connect(platesolveworker, &PlateSolveWorker::resultReady, [this](const SloveResults &result) {});

    // platesolveworker->setParams(Filename, FocalLength, CameraWidth, CameraHeight, false); // 设置参数
    // platesolveworker->start();
    Tools::PlateSolve(Filename, FocalLength, CameraWidth, CameraHeight, false);

    solveTimer.setSingleShot(true);

    connect(&solveTimer, &QTimer::timeout, [this, Filename]()
    {
        if (Tools::isSolveImageFinish())
        {
            sleep(1);
            SloveResults result = Tools::ReadSolveResult(Filename, SolveImageWidth, SolveImageHeight);
            Logger::Log("SolveImage | Plate Solve Result(RA_Degree, DEC_Degree):" + std::to_string(result.RA_Degree) + ", " + std::to_string(result.DEC_Degree), LogLevel::INFO, DeviceType::MAIN);

            if (result.RA_Degree == -1 && result.DEC_Degree == -1)
            {
                Logger::Log("SolveImage | Solve image failed...", LogLevel::WARNING, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("SolveImagefailed");
            }
            else
            {
                if (SloveResultList.size() == 3) {
                    SloveResultList.clear();
                }
                SloveResultList.push_back(result);
                emit wsThread->sendMessageToClient("SolveImageResult:" + QString::number(result.RA_Degree) + ":" + QString::number(result.DEC_Degree) + ":" + QString::number(Tools::RadToDegree(0)) + ":" + QString::number(Tools::RadToDegree(0)));
                emit wsThread->sendMessageToClient("SolveFovResult:" + QString::number(result.RA_0) + ":" + QString::number(result.DEC_0) + ":" + QString::number(result.RA_1) + ":" + QString::number(result.DEC_1) + ":" + QString::number(result.RA_2) + ":" + QString::number(result.DEC_2) + ":" + QString::number(result.RA_3) + ":" + QString::number(result.DEC_3));
            }
        }
        else 
        {
            solveTimer.start(1000);  // 继续等待
        } 
    });

    solveTimer.start(1000);
}

void MainWindow::LoopSolveImage(QString Filename, int FocalLength, double CameraWidth, double CameraHeight) {
    Logger::Log("LoopSolveImage(" + Filename.toStdString() + ") start ...", LogLevel::INFO, DeviceType::MAIN);

    if(!isLoopSolveImage) {
        Logger::Log("LoopSolveImage | Loop Solve Image end.", LogLevel::INFO, DeviceType::MAIN);
        return;
    }
 
    solveTimer.stop();
    solveTimer.disconnect();
    

    // QObject::connect(platesolveworker, &PlateSolveWorker::resultReady, [this](const SloveResults &result) {});

    // platesolveworker->setParams(Filename, FocalLength, CameraWidth, CameraHeight, false); // 设置参数
    // platesolveworker->start();
    Tools::PlateSolve(Filename, FocalLength, CameraWidth, CameraHeight, false);
    

    // 启动等待赤道仪转动的定时器
    solveTimer.setSingleShot(true);

    connect(&solveTimer, &QTimer::timeout, [this, Filename]()
    {
        // 检查赤道仪状态
        if (Tools::isSolveImageFinish())
        {
            SloveResults result = Tools::ReadSolveResult(Filename, SolveImageWidth, SolveImageHeight);
            Logger::Log("LoopSolveImage | Plate Solve Result(RA_Degree, DEC_Degree):" + std::to_string(result.RA_Degree) + ", " + std::to_string(result.DEC_Degree), LogLevel::INFO, DeviceType::MAIN);

            if (result.RA_Degree == -1 && result.DEC_Degree == -1)
            {
                Logger::Log("LoopSolveImage | Solve image failed...", LogLevel::WARNING, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("SolveImagefailed");
                emit wsThread->sendMessageToClient("LoopSolveImageFinished");
            }
            else
            {
                emit wsThread->sendMessageToClient("RealTimeSolveImageResult:" + QString::number(result.RA_Degree) + ":" + QString::number(result.DEC_Degree) + ":" + QString::number(Tools::RadToDegree(0)) + ":" + QString::number(Tools::RadToDegree(0)));
                emit wsThread->sendMessageToClient("LoopSolveImageFinished");
            }

            // CaptureAndSolve(glExpTime, true);
        }
        else 
        {
            solveTimer.start(1000);  // 继续等待
        } 
    });

    solveTimer.start(1000);
}


void MainWindow::ClearSloveResultList() {
    SloveResultList.clear();
}

void MainWindow::RecoverySloveResul()
{
    for (const auto &result : SloveResultList)
    {
        Logger::Log("RecoverySloveResul | Plate Solve Result(RA_Degree, DEC_Degree):" + std::to_string(result.RA_Degree) + ", " + std::to_string(result.DEC_Degree), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("SolveImageResult:" + QString::number(result.RA_Degree) + ":" + QString::number(result.DEC_Degree) + ":" + QString::number(Tools::RadToDegree(0)) + ":" + QString::number(Tools::RadToDegree(0)));
        emit wsThread->sendMessageToClient("SolveFovResult:" + QString::number(result.RA_0) + ":" + QString::number(result.DEC_0) + ":" + QString::number(result.RA_1) + ":" + QString::number(result.DEC_1) + ":" + QString::number(result.RA_2) + ":" + QString::number(result.DEC_2) + ":" + QString::number(result.RA_3) + ":" + QString::number(result.DEC_3));
    }
}


void MainWindow::CaptureAndSolve(int ExpTime, bool isLoop) {
    // 如果是循环解算模式但当前没有在解算图像，则记录信息并返回
    if(isLoop && !isLoopSolveImage) {
        Logger::Log("CaptureAndSolve | Loop Solve Image end.", LogLevel::INFO, DeviceType::MAIN);
        return;
    }

    // 停止并断开之前的计时器连接，准备新的捕获周期
    captureTimer.stop();
    captureTimer.disconnect();

    // 设置当前拍摄状态为进行中
    ShootStatus = "InProgress";
    Logger::Log("CaptureAndSolve | ShootStatus: " + ShootStatus.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    // 开始使用指定的曝光时间进行拍摄
    INDI_Capture(ExpTime);

    // 设置计时器为单次触发
    captureTimer.setSingleShot(true);

    // 连接计时器的超时信号到一个匿名函数，处理拍摄和解算后的逻辑
    connect(&captureTimer, &QTimer::timeout, [this, isLoop]() {
        // 如果需要结束拍摄和解算，则执行中止操作并返回
        if (EndCaptureAndSolve)
        {
            EndCaptureAndSolve = false;
            INDI_AbortCapture();
            Logger::Log("CaptureAndSolve | End Capture And Solve!!!", LogLevel::INFO, DeviceType::MAIN);
            return;
        }
        Logger::Log("CaptureAndSolve | WaitForShootToComplete ..." , LogLevel::INFO, DeviceType::MAIN);
  
        // 检查拍摄和解算是否完成
        if (WaitForShootToComplete() && Tools::WaitForPlateSolveToComplete()) 
        {
            // 停止计时器，表示当前周期任务完成
            captureTimer.stop();
            // 根据是否循环解算，执行一次解算或继续循环解算
            if(!isLoop) {
                ImageSolveTimer.start();
                Logger::Log("Solve image start.", LogLevel::INFO, DeviceType::MAIN);
                SolveImage(SolveImageFileName, glFocalLength, glCameraSize_width, glCameraSize_height);
                ImageSolveTime = ImageSolveTimer.elapsed();
                Logger::Log("Solve image completed:" + std::to_string(ImageSolveTime) + "milliseconds", LogLevel::INFO, DeviceType::MAIN);
                ImageSolveTimer.invalidate();
            } else {
                LoopSolveImage(SolveImageFileName, glFocalLength, glCameraSize_width, glCameraSize_height);
            }
        } 
        else 
        {
            // 如果拍摄或解算未完成，重新启动计时器，继续等待
            captureTimer.start(3000);
        } 
    });

    // 启动计时器，开始等待周期
    captureTimer.start(3000);
}


void MainWindow::editHotspotName(QString newName) {
    Logger::Log("editHotspotName(" + newName.toStdString() + ") start ...", LogLevel::INFO, DeviceType::MAIN);
    QString command = QString("echo 'quarcs' | sudo -S sed -i 's/^ssid=.*/ssid=%1/' /etc/NetworkManager/system-connections/RaspBerryPi-WiFi.nmconnection").arg(newName);
    
    Logger::Log("editHotspotName | command:" + command.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    
    QProcess process;
    process.start("bash", QStringList() << "-c" << command);
    process.waitForFinished();
    
    QString HostpotName = getHotspotName();
    Logger::Log("editHotspotName | New Hotspot Name:" + HostpotName.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    if(HostpotName == newName) {
        emit wsThread->sendMessageToClient("EditHotspotNameSuccess");
        // restart NetworkManager
        process.start("sudo systemctl restart NetworkManager");
        process.waitForFinished();
    } else {
        emit wsThread->sendMessageToClient("EditHotspotNameFailed");
        Logger::Log("editHotspotName | Edit Hotspot name failed.", LogLevel::WARNING, DeviceType::MAIN);
    }
}

QString MainWindow::getHotspotName() {
    QProcess process;
    process.start("sudo", QStringList() << "cat" << "/etc/NetworkManager/system-connections/RaspBerryPi-WiFi.nmconnection");
    process.waitForFinished();
    
    // Get the command output
    QString output = process.readAllStandardOutput();
    Logger::Log("getHotspotName | output:" + output.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    // Look for the SSID configuration
    QString ssidPattern = "ssid=";
    int index = output.indexOf(ssidPattern);
    if (index != -1) {
        int start = index + ssidPattern.length();
        int end = output.indexOf("\n", start);
        if (end == -1) end = output.length();
        return output.mid(start, end - start);
    }
    
    // If SSID is not found, return a default value
    return "N/A";
}

void MainWindow::SendDebugToVueClient(const QString &msg)
{
    emit wsThread->sendMessageToClient("SendDebugMessage|" + msg);
}

void MainWindow::customMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context);

    if (!instance) {
        return;
    }

    QString currentTime = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    QString typeStr;
    switch(type) {
        case QtDebugMsg:
            typeStr = "Debug";
            break;
        case QtInfoMsg:
            typeStr = "Info"; 
            break;
        case QtWarningMsg:
            typeStr = "Warning";
            break;
        case QtCriticalMsg:
            typeStr = "Critical";
            break;
        default:
            typeStr = "Unknown";
    }

    // 统一格式化日志消息
    QString logMessage = QString("[Server] %1 | %2 | %3").arg(currentTime).arg(typeStr).arg(msg);

    // // 将日志消息添加到缓存
    // instance->logCache.push_back(logMessage);

    // 统一输出到标准错误，无论日志类型
    fprintf(stderr, "%s\n", logMessage.toLocal8Bit().constData());

    // 发送到客户端的Vue应用，包括日志类型和消息
    instance->SendDebugToVueClient(typeStr + "|" + msg);
}

void MainWindow::ConnectDriver(QString DriverName, QString DriverType)
{
    Logger::Log("ConnectDriver(" + DriverName.toStdString() + ", " + DriverType.toStdString() + ") start ...", LogLevel::INFO, DeviceType::MAIN);
    if (DriverName == "" || DriverType == "")
    {
        Logger::Log("ConnectDriver | DriverName(" + DriverName.toStdString() + ") or DriverType(" + DriverType.toStdString() + ") is Null", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectFailed:DriverName or DriverType is Null.");
        emit wsThread->sendMessageToClient("ConnectDriverFailed:DriverName or DriverType is Null.");
        return;
    }
    // if ((DriverName == "indi_qhy_ccd" || DriverName == "indi_qhy_ccd2") && ConnectDriverList.size() != 0) {
    //     std::vector<std::string> binddeviceNameList;
    //     for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    //     {
    //         if ((systemdevicelist.system_devices[i].DriverIndiName == "indi_qhy_ccd" && DriverName == "indi_qhy_ccd2") || (systemdevicelist.system_devices[i].DriverIndiName == "indi_qhy_ccd2" && DriverName == "indi_qhy_ccd") && systemdevicelist.system_devices[i].isConnect && systemdevicelist.system_devices[i].isBind && systemdevicelist.system_devices[i].DeviceIndiName != "")
    //         {
    //             Logger::Log("ConnectDriver | find bind device(" + systemdevicelist.system_devices[i].DeviceIndiName.toStdString() + ") for Driver(" + DriverName.toStdString() + ")", LogLevel::INFO, DeviceType::MAIN);
    //             binddeviceNameList.push_back(systemdevicelist.system_devices[i].DeviceIndiName.toStdString());
    //         }
    //     }
    //     for (int i = 0; i < indi_Client->GetDeviceCount(); i++)
    //     {
    //         bool deviceIsBind = false;
            
    //         if ((indi_Client->GetDeviceFromList(i)->getDriverExec() == DriverName) || (indi_Client->GetDeviceFromList(i)->getDriverExec() == "indi_qhy_ccd") || (indi_Client->GetDeviceFromList(i)->getDriverExec() == "indi_qhy_ccd2"))
    //         {
    //             for (int j = 0; j < binddeviceNameList.size(); j++) {
    //                 if (std::string(indi_Client->GetDeviceNameFromList(i).c_str()) == binddeviceNameList[j])
    //                 {
    //                     Logger::Log("ConnectDriver | Driver(" + DriverName.toStdString() + ") has device(" + std::string(indi_Client->GetDeviceNameFromList(i).c_str()) + ") is bind!", LogLevel::INFO, DeviceType::MAIN);
    //                     deviceIsBind = true;
    //                     break;
    //                 }
    //                 qDebug() << "设备名" << indi_Client->GetDeviceNameFromList(i).c_str();
    //             }
    //             if (!deviceIsBind) {
    //                 Logger::Log("ConnectDriver | Driver(" + DriverName.toStdString() + ") is not bind, is qhy_ccd device ,will be deleted to reset driver", LogLevel::INFO, DeviceType::MAIN);
    //                 indi_Client->disconnectDevice(indi_Client->GetDeviceNameFromList(i).c_str());
    //                 indi_Client->RemoveDevice(indi_Client->GetDeviceNameFromList(i).c_str());
    //             }
    //         }
    //     }
    // }
    //     qDebug() << "步骤1";
    // if ((DriverName == "indi_qhy_ccd" || DriverName == "indi_qhy_ccd2") && ConnectDriverList.size() != 0) {
    //     qDebug() << "步骤2";
    //     std::vector<std::string> binddeviceNameList;
    //     for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    //     {
    //         qDebug() << "步骤3";
    //         if ((systemdevicelist.system_devices[i].DriverIndiName == "indi_qhy_ccd" && DriverName == "indi_qhy_ccd2") || (systemdevicelist.system_devices[i].DriverIndiName == "indi_qhy_ccd2" && DriverName == "indi_qhy_ccd")) {
    //             if (systemdevicelist.system_devices[i].isConnect) {
    //                 if (systemdevicelist.system_devices[i].isBind) {
    //                     if (systemdevicelist.system_devices[i].DeviceIndiName != "") {
    //                         Logger::Log("ConnectDriver | 找到绑定设备(" + systemdevicelist.system_devices[i].DeviceIndiName.toStdString() + ") 对应驱动(" + DriverName.toStdString() + ")", LogLevel::INFO, DeviceType::MAIN);
    //                         binddeviceNameList.push_back(systemdevicelist.system_devices[i].DeviceIndiName.toStdString());
    //                     } else {
    //                         qDebug() << "设备名为空";
    //                     }
    //                 } else {
    //                     qDebug() << "设备没有绑定";
    //                 }
    //             } else {
    //                 qDebug() << "设备没有连接";
    //             }
    //         } else {
    //             qDebug() << "驱动名不匹配" << systemdevicelist.system_devices[i].DriverIndiName <<" != " << DriverName;
    //         }
    //     }
    //     for (int i = 0; i < indi_Client->GetDeviceCount(); i++)
    //     {
    //         qDebug() << "步骤4";
    //         bool deviceIsBind = false;
    //         qDebug() << "驱动名" << indi_Client->GetDeviceFromList(i)->getDriverExec();
    //         if (std::string(indi_Client->GetDeviceFromList(i)->getDriverExec()) == DriverName.toStdString() || std::string(indi_Client->GetDeviceFromList(i)->getDriverExec()) == "indi_qhy_ccd" || std::string(indi_Client->GetDeviceFromList(i)->getDriverExec()) == "indi_qhy_ccd2")
    //         {
    //             qDebug() << "步骤5";
    //             for (int j = 0; j < binddeviceNameList.size(); j++) {
    //                 qDebug() << "步骤6";
    //                 if (std::string(indi_Client->GetDeviceNameFromList(i).c_str()) == binddeviceNameList[j])
    //                 {
    //                     Logger::Log("ConnectDriver | 驱动(" + DriverName.toStdString() + ") 已有设备(" + std::string(indi_Client->GetDeviceNameFromList(i).c_str()) + ") 是绑定的!", LogLevel::INFO, DeviceType::MAIN);
    //                     deviceIsBind = true;
    //                     break;
    //                 }
    //             }
    //             qDebug() << "步骤7";
    //             if (!deviceIsBind) {
    //                 qDebug() << "步骤8";
    //                 Logger::Log("ConnectDriver | 驱动(" + DriverName.toStdString() + ") 没有绑定，是qhy_ccd设备，将被删除以重置驱动", LogLevel::INFO, DeviceType::MAIN);
    //                 indi_Client->disconnectDevice(indi_Client->GetDeviceNameFromList(i).c_str());
    //                 indi_Client->RemoveDevice(indi_Client->GetDeviceNameFromList(i).c_str());
    //                 qDebug() << "当前设备列表";
    //                 for (int i = 0; i < indi_Client->GetDeviceCount(); i++) {
    //                     qDebug() << "设备名称" << indi_Client->GetDeviceNameFromList(i).c_str();
    //                     qDebug() << "驱动名称" << indi_Client->GetDeviceFromList(i)->getDriverExec();
    //                 }
    //                 sleep(1);
    //             }
    //         }
    //     }
    // }
    
    int driverCode = -1;
    bool isDriverConnected = false;
    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        // qInfo() << "systemdevicelist.system_devices["<< i << "].DriverIndiName:" << systemdevicelist.system_devices[i].DriverIndiName;
        // qInfo() << "systemdevicelist.system_devices["<< i << "].Description:" << systemdevicelist.system_devices[i].Description;

        if (systemdevicelist.system_devices[i].Description == DriverType)
        {
            if (systemdevicelist.system_devices[i].isConnect)
            {
                Logger::Log("ConnectDriver | Driver(" + DriverName.toStdString() + ") is already connected", LogLevel::INFO, DeviceType::MAIN);
            }
            else
            {
                if (ConnectDriverList.contains(DriverName))
                {
                    Logger::Log("ConnectDriver | Driver(" + DriverName.toStdString() + ") is already in ConnectDriverList", LogLevel::INFO, DeviceType::MAIN);
                }
                else
                {
                    Logger::Log("ConnectDriver | Driver(" + DriverName.toStdString() + ") is not connected, start to connect", LogLevel::INFO, DeviceType::MAIN);
                    ConnectDriverList.push_back(DriverName);
                    Tools::startIndiDriver(DriverName);
                }
            }
            driverCode = i;
            isDriverConnected = true;
            break;
        }
    }
    
    if (!isDriverConnected) {
        Logger::Log("ConnectDriver | " + DriverType.toStdString() + " Driver(" + DriverName.toStdString() + ") is not selected, start to connect", LogLevel::INFO, DeviceType::MAIN);
        if (DriverType == "Mount") {
            driverCode = 0;
            systemdevicelist.system_devices[0].Description = "Mount";
            systemdevicelist.system_devices[0].DriverIndiName = DriverName;
        } else if (DriverType == "Guider") {
            driverCode = 1;
            systemdevicelist.system_devices[1].Description = "Guider";
            systemdevicelist.system_devices[1].DriverIndiName = DriverName;
        } else if (DriverType == "PoleCamera") {
            driverCode = 2;
            systemdevicelist.system_devices[2].Description = "PoleCamera";
            systemdevicelist.system_devices[2].DriverIndiName = DriverName;
        } else if (DriverType == "MainCamera") {
            driverCode = 20;
            systemdevicelist.system_devices[20].Description = "MainCamera";
            systemdevicelist.system_devices[20].DriverIndiName = DriverName;
        } else if (DriverType == "CFW") {
            driverCode = 21;
            systemdevicelist.system_devices[21].Description = "CFW";
            systemdevicelist.system_devices[21].DriverIndiName = DriverName;
        } else if (DriverType == "Focuser") {
            driverCode = 22;
            systemdevicelist.system_devices[22].Description = "Focuser";
            systemdevicelist.system_devices[22].DriverIndiName = DriverName;
        }else {
            Logger::Log("ConnectDriver | DriverType(" + DriverType.toStdString() + ") is not supported.", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("ConnectFailed:DriverType is not supported.");
            emit wsThread->sendMessageToClient("ConnectDriverFailed:DriverType is not supported.");
            Tools::stopIndiDriver(DriverName);
            ConnectDriverList.removeAll(DriverName);
            return;
        }
        Tools::startIndiDriver(DriverName);
        ConnectDriverList.push_back(DriverName);
    }
    // connectIndiServer(indi_Client);
    sleep(1);
    if (indi_Client->isServerConnected() == false)
    {
        Logger::Log("ConnectDriver | indi Client is not connected, try to connect", LogLevel::INFO, DeviceType::MAIN);
        connectIndiServer(indi_Client);
        if (indi_Client->isServerConnected() == false)
        {
            Logger::Log("ConnectDriver | Connect indi server failed", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("ConnectFailed:Connect indi server failed.");
            emit wsThread->sendMessageToClient("ConnectDriverFailed:Connect indi server failed." );
            Tools::stopIndiDriver(DriverName);
            ConnectDriverList.removeAll(DriverName);
            return;
        }
    }
    sleep(1);
    int time = 0;
    while (time < 10) {
        if (indi_Client->GetDeviceCount() > 0) {
            Logger::Log("ConnectDriver | Find device!", LogLevel::INFO, DeviceType::MAIN);
            break;
        }
        Logger::Log("ConnectDriver | Wait find device...", LogLevel::INFO, DeviceType::MAIN);
        QThread::msleep(1000);
        time++;
    }
    sleep(1);
    if (indi_Client->GetDeviceCount() == 0)
    {
        Logger::Log("ConnectDriver | No device found", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectDriverFailed:No device found.");
        Tools::stopIndiDriver(DriverName);
        ConnectDriverList.removeAll(DriverName);
        return;
    }
    // 记录连接的设备的id列表
    std::vector<int> connectedDeviceIdList;
    for (int i = 0; i < indi_Client->GetDeviceCount(); i++)
    {
        
        if (indi_Client->GetDeviceFromList(i)->getDriverExec() == DriverName)
        {
            if (indi_Client->GetDeviceFromList(i)->isConnected())
            {
                Logger::Log("ConnectDriver | Device(" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + ") is connected", LogLevel::INFO, DeviceType::MAIN);
                bool isDeviceBind = false;
                for (int j = 0; j < systemdevicelist.system_devices.size(); j++) {
                    if (systemdevicelist.system_devices[j].DeviceIndiName == indi_Client->GetDeviceNameFromList(i).c_str()) {
                        isDeviceBind = true;
                    }
                }
                if (!isDeviceBind) {
                    Logger::Log("ConnectDriver | Device(" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + ") is not bind, start to disconnect", LogLevel::INFO, DeviceType::MAIN);
                    indi_Client->disconnectDevice(indi_Client->GetDeviceFromList(i)->getDeviceName());
                    time = 0;
                    while (indi_Client->GetDeviceFromList(i)->isConnected() && time < 30) {
                        Logger::Log("ConnectDriver | Wait for disconnect" + std::string(indi_Client->GetDeviceNameFromList(i).c_str()), LogLevel::INFO, DeviceType::MAIN);
                        QThread::msleep(1000);
                        time++;
                    }
                    if (!indi_Client->GetDeviceFromList(i)->isConnected()) {
                        Logger::Log("ConnectDriver | Device(" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + ") is disconnected, start to connect", LogLevel::INFO, DeviceType::MAIN);
                    }else {
                        Logger::Log("ConnectDriver | Device(" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + ") is not disconnected", LogLevel::WARNING, DeviceType::MAIN);
                    }

                    // Tools::startIndiDriver(DriverName);

                    // qInfo() << "Device(" << indi_Client->GetDeviceFromList(i)->getDeviceName() << ") is not bind";
                    time = 0;
                    indi_Client->setBaudRate(indi_Client->GetDeviceFromList(i), systemdevicelist.system_devices[i].BaudRate);
                    indi_Client->connectDevice(indi_Client->GetDeviceFromList(i)->getDeviceName());
                    while (!indi_Client->GetDeviceFromList(i)->isConnected() && time < 15) {
                        Logger::Log("ConnectDriver | Wait for connect" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + ",state:" + std::to_string(indi_Client->GetDeviceFromList(i)->isConnected()), LogLevel::INFO, DeviceType::MAIN);
                        QThread::msleep(1000);
                        time++;
                    }
                    if (!indi_Client->GetDeviceFromList(i)->isConnected()) {
                        Logger::Log("ConnectDriver | Device (" + std::string(indi_Client->GetDeviceNameFromList(i).c_str()) + ") is not exist", LogLevel::WARNING, DeviceType::MAIN);
                        emit wsThread->sendMessageToClient("deleteDeviceAllocationList:" + QString::fromUtf8(indi_Client->GetDeviceNameFromList(i).c_str()));
                        indi_Client->RemoveDevice(indi_Client->GetDeviceNameFromList(i).c_str());
                        for (int j = 0; j < connectedDeviceIdList.size(); j++) {
                            if (connectedDeviceIdList[j] == i) {
                                connectedDeviceIdList.erase(connectedDeviceIdList.begin() + j);
                                break;
                            }
                        }
                    }else {
                        connectedDeviceIdList.push_back(i);
                    }
                }else{
                    Logger::Log("ConnectDriver | Device(" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + ") is bind, is Used", LogLevel::INFO, DeviceType::MAIN);
                }
            }
            else
            {
                Logger::Log("ConnectDriver | Device(" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + ") is connecting...", LogLevel::INFO, DeviceType::MAIN);
                indi_Client->setBaudRate(indi_Client->GetDeviceFromList(i), systemdevicelist.system_devices[i].BaudRate);
                indi_Client->connectDevice(indi_Client->GetDeviceNameFromList(i).c_str());
                int waitTime = 0;
                bool connectState = false;
                while (waitTime < 30)
                {
                    Logger::Log("ConnectDriver | Wait for Connect " + std::string(indi_Client->GetDeviceNameFromList(i).c_str()), LogLevel::INFO, DeviceType::MAIN);
                    QThread::msleep(1000); // 等待1秒
                    waitTime++;
                    if (indi_Client->GetDeviceFromList(i)->isConnected()) {
                        connectState = true;
                        break;
                    }
                }
                if (connectState) {
                    connectedDeviceIdList.push_back(i);
                }else {
                    if (DriverType == "Focuser") {
                        QString DevicePort;
                        indi_Client->getDevicePort(indi_Client->GetDeviceFromList(i), DevicePort);
                        // 检查指定的端口是否在/dev/serial/by-id目录下存在
                        QFile portFile(DevicePort);
                        if (!portFile.exists()) {
                            Logger::Log("ConnectDriver | Port " + DevicePort.toStdString() + " does not exist.", LogLevel::WARNING, DeviceType::MAIN);
                            // 获取所有连接的串口
                            QStringList connectedPorts = getConnectedSerialPorts();
                            for (int j = 0; j < connectedPorts.size(); j++) {
                                Logger::Log("ConnectDriver | Connected Ports:" + connectedPorts[j].toStdString(), LogLevel::INFO, DeviceType::MAIN);
                                if (connectedPorts[j].contains("ttyACM")) {
                                    Logger::Log("ConnectDriver | Found ttyACM device:" + connectedPorts[j].toStdString(), LogLevel::INFO, DeviceType::MAIN);
                                }else {
                                    Logger::Log("ConnectDriver | Not found ttyACM device", LogLevel::INFO, DeviceType::MAIN);
                                    continue;
                                }
                                QStringList links = findLinkToTtyDevice("/dev", connectedPorts[j]);
                                QString link = "";
                                if (links.isEmpty()) {
                                    Logger::Log("ConnectDriver | No link found for " + connectedPorts[j].toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                                } else {
                                    for (int k = 0; k < links.size(); k++) {
                                        if (areFilesInSameDirectory(links[k], DevicePort)) {
                                            if (link == "") {
                                                link = links[k];
                                            }else {
                                                Logger::Log("ConnectDriver | No Found only one link for " + connectedPorts[j].toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                                                link = "";
                                                break;
                                            }
                                        }
                                    }
                                    if (link == "") {
                                        Logger::Log("ConnectDriver | No Found link for " + connectedPorts[j].toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                                        continue;
                                    }
                                    Logger::Log("ConnectDriver | Link found for " + connectedPorts[j].toStdString() + ": " + link.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                                    DevicePort = link;
                                    indi_Client->setDevicePort(indi_Client->GetDeviceFromList(i), DevicePort);
                                    indi_Client->setBaudRate(indi_Client->GetDeviceFromList(i), systemdevicelist.system_devices[i].BaudRate);
                                    indi_Client->connectDevice(indi_Client->GetDeviceNameFromList(i).c_str());
                                    waitTime = 0;
                                    connectState = false;
                                    while (waitTime < 30)
                                    {
                                        Logger::Log("ConnectDriver | Wait for Connect" + std::string(indi_Client->GetDeviceNameFromList(i).c_str()), LogLevel::INFO, DeviceType::MAIN);
                                        QThread::msleep(1000); // 等待1秒
                                        waitTime++;
                                        if (indi_Client->GetDeviceFromList(i)->isConnected()) {
                                            connectState = true;
                                            break;
                                        }
                                    }
                                    if (connectState) {
                                        connectedDeviceIdList.push_back(i);
                                    }
                                }
                
                            }
                            
                        }
                    }else{
                        emit wsThread->sendMessageToClient("deleteDeviceAllocationList:" + QString::fromUtf8(indi_Client->GetDeviceNameFromList(i).c_str()));
                        indi_Client->disconnectDevice(indi_Client->GetDeviceNameFromList(i).c_str());
                        Logger::Log("ConnectDriver | Device (" + std::string(indi_Client->GetDeviceNameFromList(i).c_str()) + ") is not exist", LogLevel::WARNING, DeviceType::MAIN);
                        indi_Client->RemoveDevice(indi_Client->GetDeviceNameFromList(i).c_str());
                    }
                    
                    
                    // qWarning() << "Device (" << indi_Client->GetDeviceNameFromList(i).c_str() << ") is not exist";
                    
                }
            }
            
        }
    }

    if (connectedDeviceIdList.size() == 0){
        Logger::Log("ConnectDriver | Driver(" + DriverName.toStdString() + ") No Device found.", LogLevel::WARNING, DeviceType::MAIN);
        Tools::stopIndiDriver(DriverName);
        int index = ConnectDriverList.indexOf(DriverName);
        if (index != -1) { // 如果找到了
            ConnectDriverList.removeAt(index); // 从列表中删除
            Logger::Log("Driver removed successfully: " + DriverName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        } else {
            Logger::Log("Driver not found in list: " + DriverName.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
        }
        emit wsThread->sendMessageToClient("ConnectDriverFailed:Driver(" + DriverName + ") No Device found.");
        return;
    }

    ConnectedCCDList.clear();
    ConnectedTELESCOPEList.clear();
    ConnectedFOCUSERList.clear();
    ConnectedFILTERList.clear();

    // 判断连接设备的类型
    for (int i = 0; i < connectedDeviceIdList.size(); i++)
    {
        if (indi_Client->GetDeviceFromList(connectedDeviceIdList[i])->isConnected())
        {
            if (indi_Client->GetDeviceFromList(connectedDeviceIdList[i])->getDriverInterface() & INDI::BaseDevice::CCD_INTERFACE)
            {
                Logger::Log("ConnectDriver | We received a CCD!", LogLevel::INFO, DeviceType::MAIN);
                ConnectedCCDList.push_back(connectedDeviceIdList[i]);
            }
            else if (indi_Client->GetDeviceFromList(connectedDeviceIdList[i])->getDriverInterface() & INDI::BaseDevice::FILTER_INTERFACE)
            {
                Logger::Log("ConnectDriver | We received a FILTER!", LogLevel::INFO, DeviceType::MAIN);
                ConnectedFILTERList.push_back(connectedDeviceIdList[i]);
            }
            else if (indi_Client->GetDeviceFromList(connectedDeviceIdList[i])->getDriverInterface() & INDI::BaseDevice::TELESCOPE_INTERFACE)
            {
                Logger::Log("ConnectDriver | We received a TELESCOPE!", LogLevel::INFO, DeviceType::MAIN);
                ConnectedTELESCOPEList.push_back(connectedDeviceIdList[i]);
            }
            else if (indi_Client->GetDeviceFromList(connectedDeviceIdList[i])->getDriverInterface() & INDI::BaseDevice::FOCUSER_INTERFACE)
            {
                Logger::Log("ConnectDriver | We received a FOCUSER!", LogLevel::INFO, DeviceType::MAIN);
                ConnectedFOCUSERList.push_back(connectedDeviceIdList[i]);
            }
            // qInfo() << "Driver:" << indi_Client->GetDeviceFromList(connectedDeviceIdList[i])->getDriverExec() << "Device:" << indi_Client->GetDeviceFromList(connectedDeviceIdList[i])->getDeviceName();
        }
        else
        {
            Logger::Log("ConnectDriver | Connect failed device:" + std::string(indi_Client->GetDeviceFromList(connectedDeviceIdList[i])->getDeviceName()), LogLevel::WARNING, DeviceType::MAIN);
            // emit wsThread->sendMessageToClient("ConnectDriverFailed:Connect failed device:" + indi_Client->GetDeviceFromList(connectedDeviceIdList[i])->getDeviceName());
            // emit wsThread->sendMessageToClient("ConnectDriverFailed:Connect failed device:" + indi_Client->GetDeviceFromList(connectedDeviceIdList[i])->getDeviceName());
        }
    }

    QStringList SelectedCameras;
    for (int i = 0; i < systemdevicelist.system_devices.size(); i++) {
        if (systemdevicelist.system_devices[i].DriverIndiName == DriverName) {
            SelectedCameras.push_back(systemdevicelist.system_devices[i].Description);
        }
    }
    Logger::Log("ConnectDriver | Driver(" + DriverName.toStdString() + ") supports " + std::to_string(SelectedCameras.size()) + " devices", LogLevel::INFO, DeviceType::MAIN);
    for (auto Camera : SelectedCameras)
    {
        Logger::Log("ConnectDriver | Driver(" + DriverName.toStdString() + ") supports " + Camera.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    }

    Logger::Log("ConnectDriver | Number of Connected CCD:" + std::to_string(ConnectedCCDList.size()), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("ConnectDriver | Number of Connected TELESCOPE:" + std::to_string(ConnectedTELESCOPEList.size()), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("ConnectDriver | Number of Connected FOCUSER:" + std::to_string(ConnectedFOCUSERList.size()), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("ConnectDriver | Number of Connected FILTER:" + std::to_string(ConnectedFILTERList.size()), LogLevel::INFO, DeviceType::MAIN);

    // 判断连接设备的数量,
    bool EachDeviceOne = true;

    if (SelectedCameras.size() == 1 && ConnectedCCDList.size() == 1)
    {
        Logger::Log("ConnectDriver | The Camera Selected and Connected are Both 1", LogLevel::INFO, DeviceType::MAIN);
        if (SelectedCameras[0] == "Guider")
        {
            dpGuider = indi_Client->GetDeviceFromList(ConnectedCCDList[0]);
            systemdevicelist.system_devices[1].isConnect = true;
            indi_Client->disconnectDevice(indi_Client->GetDeviceFromList(ConnectedCCDList[0])->getDeviceName());
            sleep(1);
            call_phd_whichCamera(indi_Client->GetDeviceFromList(ConnectedCCDList[0])->getDeviceName());
            // PHD2 connect status
            AfterDeviceConnect(dpGuider);
        }
        else if (SelectedCameras[0] == "PoleCamera")
        {
            dpPoleScope = indi_Client->GetDeviceFromList(ConnectedCCDList[0]);
            systemdevicelist.system_devices[2].isConnect = true;
            AfterDeviceConnect(dpPoleScope);
        }
        else if (SelectedCameras[0] == "MainCamera")
        {
            Logger::Log("ConnectDriver | MainCamera Connected Success!", LogLevel::INFO, DeviceType::MAIN);
            dpMainCamera = indi_Client->GetDeviceFromList(ConnectedCCDList[0]);
            systemdevicelist.system_devices[20].isConnect = true;
            AfterDeviceConnect(dpMainCamera);
        }
    }
    else if (SelectedCameras.size() > 1 || ConnectedCCDList.size() > 1)
    {
        EachDeviceOne = false;
        for (int i = 0; i < ConnectedCCDList.size(); i++)
        {
            emit wsThread->sendMessageToClient("DeviceToBeAllocated:CCD:" + QString::number(ConnectedCCDList[i]) + ":" + QString::fromUtf8(indi_Client->GetDeviceFromList(ConnectedCCDList[i])->getDeviceName())); // already allocated
        }
    }

    if (ConnectedTELESCOPEList.size() == 1)
    {
        Logger::Log("ConnectDriver | Mount Connected Success!", LogLevel::INFO, DeviceType::MAIN);
        dpMount = indi_Client->GetDeviceFromList(ConnectedTELESCOPEList[0]);
        systemdevicelist.system_devices[0].isConnect = true;
        AfterDeviceConnect(dpMount);
    }
    else if (ConnectedTELESCOPEList.size() > 1)
    {
        EachDeviceOne = false;
        for (int i = 0; i < ConnectedTELESCOPEList.size(); i++)
        {
            emit wsThread->sendMessageToClient("DeviceToBeAllocated:Mount:" + QString::number(ConnectedTELESCOPEList[i]) + ":" + QString::fromUtf8(indi_Client->GetDeviceFromList(ConnectedTELESCOPEList[i])->getDeviceName()));
        }
    }

    if (ConnectedFOCUSERList.size() == 1)
    {
        Logger::Log("ConnectDriver | Focuser Connected Success!", LogLevel::INFO, DeviceType::MAIN);
        dpFocuser = indi_Client->GetDeviceFromList(ConnectedFOCUSERList[0]);
        systemdevicelist.system_devices[22].isConnect = true;
        AfterDeviceConnect(dpFocuser);
    }
    else if (ConnectedFOCUSERList.size() > 1)
    {
        EachDeviceOne = false;
        for (int i = 0; i < ConnectedFOCUSERList.size(); i++)
        {
            emit wsThread->sendMessageToClient("DeviceToBeAllocated:Focuser:" + QString::number(ConnectedFOCUSERList[i]) + ":" + QString::fromUtf8(indi_Client->GetDeviceFromList(ConnectedFOCUSERList[i])->getDeviceName()));
        }
    }

    if (ConnectedFILTERList.size() == 1)
    {
        Logger::Log("ConnectDriver | Filter Connected Success!", LogLevel::INFO, DeviceType::MAIN);
        dpCFW = indi_Client->GetDeviceFromList(ConnectedFILTERList[0]);
        systemdevicelist.system_devices[21].isConnect = true;
        AfterDeviceConnect(dpCFW);
    }
    else if (ConnectedFILTERList.size() > 1)
    {
        EachDeviceOne = false;
        for (int i = 0; i < ConnectedFILTERList.size(); i++)
        {
            emit wsThread->sendMessageToClient("DeviceToBeAllocated:CFW:" + QString::number(ConnectedFILTERList[i]) + ":" + QString::fromUtf8(indi_Client->GetDeviceFromList(ConnectedFILTERList[i])->getDeviceName()));
        }
    }

    Logger::Log("Each Device Only Has One:" + std::to_string(EachDeviceOne), LogLevel::INFO, DeviceType::MAIN);
    if (EachDeviceOne)
    {
        // AfterDeviceConnect();
    }
    else
    {
        emit wsThread->sendMessageToClient("ShowDeviceAllocationWindow");
    }
    emit wsThread->sendMessageToClient("AddDeviceType:" + systemdevicelist.system_devices[driverCode].Description);
    emit wsThread->sendMessageToClient("ConnectDriverSuccess:" + DriverName);
}

void MainWindow::DisconnectDevice(MyClient *client, QString DeviceName, QString DeviceType)
{
    if (DeviceName == "" || DeviceType ==""){
        Logger::Log("DisconnectDevice | DeviceName(" + DeviceName.toStdString() + ") or DeviceType(" + DeviceType.toStdString() + ") is Null", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    if (DeviceType == "Not Bind Device"){
        emit wsThread->sendMessageToClient("DisconnectDriverSuccess:" + DeviceType);
        return;
    }


    Logger::Log("DisconnectDevice | Disconnect " + DeviceType.toStdString() + " Device(" + DeviceName.toStdString() + ") start...", LogLevel::INFO, DeviceType::MAIN);
    int num = 0;
    int thisDriverhasDevice = 0;
    bool driverIsUsed = false;
    bool disconnectsuccess = true;
    QString disconnectdriverName;
    QVector<QString> NeedDisconnectDeviceNameList;

    for (int i = 0; i < client->GetDeviceCount(); i++)
    {
        if (client->GetDeviceFromList(i)->getDeviceName() == DeviceName )
        {
            client->disconnectDevice(client->GetDeviceFromList(i)->getDeviceName());
            while (client->GetDeviceFromList(i)->isConnected())
            {
                Logger::Log("DisconnectDevice | Waiting for disconnect finish...", LogLevel::INFO, DeviceType::MAIN);
                sleep(1);
                num++;
                if (num > 5)
                {
                    Logger::Log("DisconnectDevice | Disconnect " + DeviceType.toStdString() + " Device(" + DeviceName.toStdString() + ") failed.", LogLevel::WARNING, DeviceType::MAIN);
                    disconnectsuccess = false;
                    break;
                }
            }
            if (!disconnectsuccess){
                break;
            }
            Logger::Log("DisconnectDevice | Disconnect " + DeviceType.toStdString() + " Device(" + DeviceName.toStdString() + ") success.", LogLevel::INFO, DeviceType::MAIN);
        
            emit wsThread->sendMessageToClient("DisconnectDriverSuccess:" + DeviceType);
            break;
        }
    }
    if (!disconnectsuccess){
        Logger::Log("DisconnectDevice | Disconnect " + DeviceType.toStdString() + " Device(" + DeviceName.toStdString() + ") failed.", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("DisconnectDriverFail:" + DeviceType);
    }
            
    if (DeviceType == "MainCamera"){
        dpMainCamera = NULL;
        disconnectdriverName = systemdevicelist.system_devices[20].DriverIndiName;
        systemdevicelist.system_devices[20].isConnect = false;
        systemdevicelist.system_devices[20].DeviceIndiName = "";
        systemdevicelist.system_devices[20].DeviceIndiGroup = -1;
        systemdevicelist.system_devices[20].DriverFrom = "";
        systemdevicelist.system_devices[20].dp = NULL;
    }else if (DeviceType == "Guider"){
        dpGuider = NULL;
        disconnectdriverName = systemdevicelist.system_devices[1].DriverIndiName;
        systemdevicelist.system_devices[1].isConnect = false;
        systemdevicelist.system_devices[1].DeviceIndiName = "";
        systemdevicelist.system_devices[1].DeviceIndiGroup = -1;
        systemdevicelist.system_devices[1].DriverFrom = "";
        systemdevicelist.system_devices[1].dp = NULL;
    }else if (DeviceType == "PoleCamera"){
        dpPoleScope = NULL;
        disconnectdriverName = systemdevicelist.system_devices[2].DriverIndiName;
        systemdevicelist.system_devices[2].isConnect = false;
        systemdevicelist.system_devices[2].DeviceIndiName = "";
        systemdevicelist.system_devices[2].DeviceIndiGroup = -1;
        systemdevicelist.system_devices[2].DriverFrom = "";
        systemdevicelist.system_devices[2].dp = NULL;
    }else if (DeviceType == "Mount"){
        dpMount = NULL;
        disconnectdriverName = systemdevicelist.system_devices[0].DriverIndiName;
        systemdevicelist.system_devices[0].isConnect = false;
        systemdevicelist.system_devices[0].DeviceIndiName = "";
        systemdevicelist.system_devices[0].DeviceIndiGroup = -1;
        systemdevicelist.system_devices[0].DriverFrom = "";
        systemdevicelist.system_devices[0].dp = NULL;
    }else if (DeviceType == "Focuser"){
        dpFocuser = NULL;
        disconnectdriverName = systemdevicelist.system_devices[22].DriverIndiName;
        systemdevicelist.system_devices[22].isConnect = false;
        systemdevicelist.system_devices[22].DeviceIndiName = "";
        systemdevicelist.system_devices[22].DeviceIndiGroup = -1;
        systemdevicelist.system_devices[22].DriverFrom = "";
        systemdevicelist.system_devices[22].dp = NULL;
    }else if (DeviceType == "CFW"){
        dpCFW = NULL;
        disconnectdriverName = systemdevicelist.system_devices[21].DriverIndiName;
        systemdevicelist.system_devices[21].isConnect = false;
        systemdevicelist.system_devices[21].DeviceIndiName = "";
        systemdevicelist.system_devices[21].DeviceIndiGroup = -1;
        systemdevicelist.system_devices[21].DriverFrom = "";
        systemdevicelist.system_devices[21].dp = NULL;
    }

    QStringList SelectedCameras;
    // QStringList SelectedCameras = Tools::getCameraNumFromSystemDeviceList(systemdevicelist);
    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        // qInfo() << "systemdevicelist.system_devices["<< i <<"].DriverIndiName:" << systemdevicelist.system_devices[i].DriverIndiName;
        // qInfo() << "systemdevicelist.system_devices["<< i <<"].Description:" << systemdevicelist.system_devices[i].Description;
        if (systemdevicelist.system_devices[i].Description !="" && systemdevicelist.system_devices[i].isConnect == true){
            SelectedCameras.push_back(systemdevicelist.system_devices[i].Description);
        }
    }

    for (auto it = ConnectedDevices.begin(); it != ConnectedDevices.end(); ) {
        if (it->DeviceType == DeviceType) {
            it = ConnectedDevices.erase(it);  // 删除元素并更新迭代器
        } else {
            ++it;  // 仅在不删除时递增迭代器
        }
    }

    if (SelectedCameras.size() == 0)
    {
        Logger::Log("DisconnectDevice | No Device Connected, need to reconnect all devices", LogLevel::INFO, DeviceType::MAIN);
        disconnectIndiServer(indi_Client);
        ClearSystemDeviceList();
        clearConnectedDevices();

        initINDIServer();
        initINDIClient();
        Tools::InitSystemDeviceList();
        Tools::initSystemDeviceList(systemdevicelist);
        emit wsThread->sendMessageToClient("DisconnectDriverSuccess:all");
        return;
    }

    // Tools::startIndiDriver(disconnectdriverName);
    sleep(1);
    
    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        if (systemdevicelist.system_devices[i].DriverIndiName == disconnectdriverName)
        {
            if (systemdevicelist.system_devices[i].isConnect == true)
            {
                driverIsUsed = true;
                thisDriverhasDevice++;
            }
        }
        // if ( systemdevicelist.system_devices[i].isConnect == false && systemdevicelist.system_devices[i].DeviceIndiName != DeviceName && systemdevicelist.system_devices[i].DeviceIndiName != "")
        // {
        //     Logger::Log("DisconnectDevice | Get new device(" + systemdevicelist.system_devices[i].DeviceIndiName.toStdString() + "),try to connect it", LogLevel::INFO, DeviceType::MAIN);
        //     client->connectDevice(client->GetDeviceFromList(i)->getDeviceName());
        //     while (!client->GetDeviceFromList(i)->isConnected())
        //     {
        //         Logger::Log("DisconnectDevice | Waiting for connect finish...", LogLevel::INFO, DeviceType::MAIN);
        //         sleep(1);
        //     }
        //     if (client->GetDeviceFromList(i)->isConnected())
        //     {
        //         Logger::Log("DisconnectDevice | Connect Device(" + systemdevicelist.system_devices[i].DeviceIndiName.toStdString() + ") success.", LogLevel::INFO, DeviceType::MAIN);
        //     }
        //     else
        //     {
        //         Logger::Log("DisconnectDevice | Connect Device(" + systemdevicelist.system_devices[i].DeviceIndiName.toStdString() + ") failed.", LogLevel::WARNING, DeviceType::MAIN);
        //     }
        // }
    }
    num = 0;
    if (thisDriverhasDevice >= 1 && client->GetDeviceFromListWithName(DeviceName.toStdString())->isConnected() == false)
    {
        client->connectDevice(DeviceName.toStdString().c_str());
        Logger::Log("DisconnectDevice | This Driver has more than one device is using, need to reconnect device(" + DeviceName.toStdString() + ")", LogLevel::INFO, DeviceType::MAIN);
        while (!client->GetDeviceFromListWithName(DeviceName.toStdString())->isConnected())
        {
            Logger::Log("DisconnectDevice | Waiting for connect finish...", LogLevel::INFO, DeviceType::MAIN);
            sleep(1);
            num++;
            if (num > 10)
            {
                Logger::Log("DisconnectDevice | Reconnect Device(" + DeviceName.toStdString() + ") failed.", LogLevel::WARNING, DeviceType::MAIN);
                break;
            }
        }
        if (client->GetDeviceFromListWithName(DeviceName.toStdString())->isConnected())
        {
            Logger::Log("DisconnectDevice | Reconnect Device(" + DeviceName.toStdString() + ") success.", LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            Logger::Log("DisconnectDevice | Reconnect Device(" + DeviceName.toStdString() + ") failed.", LogLevel::WARNING, DeviceType::MAIN);
        }
        emit wsThread->sendMessageToClient("disconnectDevicehasortherdevice:"+disconnectdriverName);
    }else{
        Tools::stopIndiDriver(disconnectdriverName);
        int index = ConnectDriverList.indexOf(disconnectdriverName);
        if (index != -1) { // 如果找到了
            ConnectDriverList.removeAt(index); // 从列表中删除
            Logger::Log("Driver removed successfully: " + disconnectdriverName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        } else {
            Logger::Log("Driver not found in list: " + disconnectdriverName.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
        }
    }
    emit wsThread->sendMessageToClient("deleteDeviceAllocationList:"+DeviceName);

    // for (int i = 0; i < indi_Client->GetDeviceCount(); i++)
    // {
    //     Logger::Log("DisconnectDevice | 当前已连接 Device(" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + "),所属驱动:" + std::string(indi_Client->GetDeviceFromList(i)->getDriverExec()) + ",状态:" + std::to_string(indi_Client->GetDeviceFromList(i)->isConnected()), LogLevel::INFO, DeviceType::MAIN);
    // }

}

void MainWindow::initDeviceList()
{
  
}

void MainWindow::loadSelectedDriverList(){
    QString order = "SelectedDriverList";
    for(int i = 0; i < systemdevicelist.system_devices.size(); i++){
        if(systemdevicelist.system_devices[i].Description != ""){
           order += ":" + systemdevicelist.system_devices[i].Description + ":" + systemdevicelist.system_devices[i].DriverIndiName ;
        }
    }
    // Tools::printSystemDeviceList(systemdevicelist);
    // Logger::Log("SelectedDriverList | " + order.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    emit wsThread->sendMessageToClient(order);
}

void MainWindow::loadBindDeviceTypeList(){
    QString order = "BindDeviceTypeList";
    for(int i = 0; i < systemdevicelist.system_devices.size(); i++){
        if(systemdevicelist.system_devices[i].Description != "" && systemdevicelist.system_devices[i].isConnect == true){
            order += ":" + systemdevicelist.system_devices[i].Description 
                  + ":" + systemdevicelist.system_devices[i].DeviceIndiName 
                  + ":" + systemdevicelist.system_devices[i].DriverIndiName
                  + ":" + (systemdevicelist.system_devices[i].isBind ? "true" : "false");
            if(systemdevicelist.system_devices[i].Description == "MainCamera" && systemdevicelist.system_devices[i].isBind){
                emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));
                emit wsThread->sendMessageToClient("MainCameraOffsetRange:" + QString::number(glOffsetMin) + ":" + QString::number(glOffsetMax));
                emit wsThread->sendMessageToClient("MainCameraGainRange:" + QString::number(glGainMin) + ":" + QString::number(glGainMax));

                QString CFWname;
                indi_Client->getCFWSlotName(dpMainCamera, CFWname);
                if(CFWname != "") {
                    Logger::Log("LoadBindDeviceTypeList | get CFW Slot Name: " + CFWname.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                    // emit wsThread->sendMessageToClient("ConnectSuccess:CFW:" + CFWname +" (on camera)");
                    isFilterOnCamera = true;
                    order += ":CFW:" + CFWname +" (on camera)" + ":"+systemdevicelist.system_devices[i].DriverIndiName+ ":" + (systemdevicelist.system_devices[i].isBind ? "true" : "false");
                    int min, max, pos;
                    indi_Client->getCFWPosition(dpMainCamera, pos, min, max);
                    Logger::Log("LoadBindDeviceTypeList | getCFWPosition: " + std::to_string(min) + ", " + std::to_string(max) + ", " + std::to_string(pos), LogLevel::INFO, DeviceType::MAIN);
                    emit wsThread->sendMessageToClient("CFWPositionMax:" + QString::number(max));
                }
            }
        }
    }
    Logger::Log("LoadBindDeviceTypeList | Bind Device Type List:" + order.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    emit wsThread->sendMessageToClient(order);
}

void MainWindow::loadBindDeviceList(MyClient *client){
    QString order = "BindDeviceList";
    for(int i = 0; i < client->GetDeviceCount(); i++){
        if (client->GetDeviceFromList(i)->isConnected() && client->GetDeviceFromList(i)->getDeviceName()!=""){
            order += ":" + QString::fromUtf8(client->GetDeviceFromList(i)->getDeviceName()) 
                     + ":" + QString::number(i);
        }
    }



    Logger::Log("LoadBindDeviceList | Bind Device List:" + order.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    emit wsThread->sendMessageToClient(order);
}
// 串口通信列表
QStringList MainWindow::getConnectedSerialPorts() {
    QStringList activeSerialPortNames;
    const auto infos = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : infos) {
        QSerialPort serial;
        serial.setPort(info);
        if (serial.open(QIODevice::ReadWrite)) {  // 尝试以读写方式打开串口
            activeSerialPortNames.append(info.portName());
            serial.close();  // 关闭串口
        }
    }
    return activeSerialPortNames;
}

QString MainWindow::resolveSerialPort(const QString& symbolicLink) {
    QFileInfo fileInfo(symbolicLink);
    if (fileInfo.isSymLink()) {
        QString target = fileInfo.symLinkTarget();
        Logger::Log("ResolveSerialPort | real port path:" + target.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        return target;
    } else {
        Logger::Log("ResolveSerialPort | provided path is not a symbolic link", LogLevel::WARNING, DeviceType::MAIN);
        return QString();
    }
}

QStringList MainWindow::findLinkToTtyDevice(const QString& directoryPath, const QString& ttyDevice) {
    QString targetDevice = "/dev/" + ttyDevice;  // 构建完整的设备路径
    QStringList foundLinks;

    // 使用 QDirIterator 递归遍历目录和子目录
    QDirIterator it(directoryPath, QDir::Files | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        QFileInfo fileInfo = it.fileInfo();
        if (fileInfo.isSymLink()) {
            QString target = fileInfo.symLinkTarget();
            // 如果符号链接是相对路径，需要将其转换为绝对路径
            if (QDir::isRelativePath(target)) {
                target = fileInfo.absoluteDir().absoluteFilePath(target);
            }
            // 检查符号链接的目标是否是指定的 tty 设备
            if (target == targetDevice) {
                foundLinks.append(fileInfo.absoluteFilePath());
                Logger::Log("FindLinkToTtyDevice | found link:" + fileInfo.absoluteFilePath().toStdString() + " -> " + target.toStdString(), LogLevel::INFO, DeviceType::MAIN);
            }
        }
    }
    // 返回找到的所有符号链接
    return foundLinks;
}

bool MainWindow::areFilesInSameDirectory(const QString& path1, const QString& path2) {
    QFileInfo fileInfo1(path1);
    QFileInfo fileInfo2(path2);

    // 获取两个文件的目录路径
    QString dir1 = fileInfo1.absolutePath();
    QString dir2 = fileInfo2.absolutePath();

    // 比较目录路径是否相同
    return (dir1 == dir2);
}

void MainWindow::onParseInfoEmitted(const QString& message){
    emit wsThread->sendMessageToClient("ParseInfoEmitted:" + message);
}

void MainWindow::loadParameters(){

}

void MainWindow::disconnectDevice(const QString& deviceName, const QString& description) {
    for (int i = 0; i < indi_Client->GetDeviceCount(); i++) {
        if (indi_Client->GetDeviceFromList(i)->getDeviceName() == deviceName) {
            indi_Client->disconnectDevice(deviceName.toStdString().c_str());
            int num = 0;
            bool disconnectSuccess = true;
            // while (indi_Client->GetDeviceFromList(i)->isConnected()) {
            //     Logger::Log("Waiting for " + deviceName.toStdString() + " to disconnect...", LogLevel::INFO, DeviceType::MAIN);
            //     sleep(1);
            //     if (++num > 5) {
            //         Logger::Log("Failed to disconnect " + deviceName.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
            //         disconnectSuccess = false;
            //         break;
            //     }
            // }
            // if (disconnectSuccess) {
            //     Logger::Log(deviceName.toStdString() + " disconnected successfully.", LogLevel::INFO, DeviceType::MAIN);
            //     emit wsThread->sendMessageToClient("DisconnectDriverSuccess:" + description);
            //     emit wsThread->sendMessageToClient("deleteDeviceAllocationList:" + deviceName);
            // }
            Logger::Log(deviceName.toStdString() + " disconnected successfully.", LogLevel::INFO, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("DisconnectDriverSuccess:" + description);
            emit wsThread->sendMessageToClient("deleteDeviceAllocationList:" + deviceName);
            break;
        }
    }
}

void MainWindow::disconnectDriver(QString Driver) {
    Logger::Log("Starting to disconnect driver: " + Driver.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    for (const auto& device : systemdevicelist.system_devices) {
        if (device.Description != "" && device.DriverIndiName == Driver && device.isConnect) {
            disconnectDevice(device.DeviceIndiName, device.Description);
            if (device.Description == "MainCamera" ) {
                if (glMainCameraStatu == "Exposuring"){
                    INDI_AbortCapture();
                }
                systemdevicelist.system_devices[20].isConnect = false;
                systemdevicelist.system_devices[20].DeviceIndiName = "";
                systemdevicelist.system_devices[20].DeviceIndiGroup = -1;
                systemdevicelist.system_devices[20].DriverFrom = "";
                systemdevicelist.system_devices[20].isBind = false;
                systemdevicelist.system_devices[20].dp = NULL;
            }
            if (device.Description == "Guider") {
                if (isGuiderLoopExp){
                    call_phd_StopLooping();
                    InitPHD2();
                }
                systemdevicelist.system_devices[1].isConnect = false;
                systemdevicelist.system_devices[1].DeviceIndiName = "";
                systemdevicelist.system_devices[1].DeviceIndiGroup = -1;
                systemdevicelist.system_devices[1].DriverFrom = "";
                systemdevicelist.system_devices[1].isBind = false;
                systemdevicelist.system_devices[1].dp = NULL;
            }
            for (auto it = ConnectedDevices.begin(); it != ConnectedDevices.end(); ) {
                if (it->DeviceType == device.Description) {
                    it = ConnectedDevices.erase(it);  // 删除元素并更新迭代器
                } else {
                    ++it;  // 仅在不删除时递增迭代器
                }
            }
        }
    }
    Tools::stopIndiDriver(Driver);
    int index = ConnectDriverList.indexOf(Driver);
    if (index != -1) { // 如果找到了
        ConnectDriverList.removeAt(index); // 从列表中删除
        Logger::Log("Driver removed successfully: " + Driver.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    } else {
        Logger::Log("Driver not found in list: " + Driver.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
    }
    
    Logger::Log("Driver disconnected: " + Driver.toStdString(), LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::focusLoopShooting(bool isLoop){
    if (isLoop){
        isFocusLoopShooting = true;
        if (glMainCameraStatu == "Exposuring"){
            emit wsThread->sendMessageToClient("startFocusLoopSjootingfile:Wait Take Picture Finish!");
            isFocusLoopShooting = false;
            return;
        }
        if (dpMainCamera == NULL){
            emit wsThread->sendMessageToClient("startFocusLoopSjootingfile:MainCamera is not connected!");
            isFocusLoopShooting = false;
            return;
        }
        FocusingLooping();
        
    }else{
        isFocusLoopShooting = false;
        if (glMainCameraStatu == "Exposuring" && dpMainCamera != NULL){
            INDI_AbortCapture();
        }
        
    }
}

void MainWindow::saveFitsAsJPG(QString filename, bool ProcessBin)
{
    cv::Mat image;
    // 读取FITS文件
    Tools::readFits(filename.toLocal8Bit().constData(), image);

    QList<FITSImage::Star> stars = Tools::FindStarsByStellarSolver(true, true);
    currentSelectStarPosition = selectStar(stars);
    emit wsThread->sendMessageToClient("addData_Point:" + QString::number(currentSelectStarPosition.x()) + ":" + QString::number(currentSelectStarPosition.y()));

    // FWHMCalOver = true;

    // 判断当前相机是否为彩色相机
    bool isColor = !(MainCameraCFA == "");

    if(ProcessBin && glMainCameraBinning != 1){
        // 进行 2x2 的合并
        image = Tools::processMatWithBinAvg(image, glMainCameraBinning, glMainCameraBinning, isColor, false);
        if (image.empty()) {
            if (isFocusLoopShooting){
                FocusingLooping();
            }
            return;
        }
    }

    if (image.depth() == CV_8U) {
        image.convertTo(image, CV_16UC1, 256, 0);
    }

    // 将图像转换为16位格式
    // image.convertTo(image, (image.depth() == 8) ? CV_16UC1 : CV_16UC1, 256, 0);

    // 归一化图像
    // cv::normalize(image, image, 0, 255, cv::NORM_MINMAX, CV_8U);

    // 删除所有以 "focuserPicture_" 开头的文件
    for (const auto & entry : std::filesystem::directory_iterator(vueDirectoryPath)) {
        if (entry.path().filename().string().rfind("focuserPicture_", 0) == 0) {
            std::filesystem::remove(entry.path());
        }
    }

    // 保存新的图像带有唯一ID的文件名
    time_t now = time(0);
    tm *ltm = localtime(&now);
    std::string fileName = "focuserPicture_" + std::to_string(ltm->tm_hour) + std::to_string(ltm->tm_min) + std::to_string(ltm->tm_sec) + ".bin";
    std::string filePath = vueDirectoryPath + fileName;
    std::ofstream outFile(filePath, std::ios::binary);

    // 检查文件是否成功打开
    if (!outFile) {
        Logger::Log("Failed to open file: " + filePath, LogLevel::WARNING, DeviceType::FOCUSER);
        if (isFocusLoopShooting){
            FocusingLooping();
        }
        return;
    }

    // 将图像数据写入到文件中
    outFile.write(reinterpret_cast<const char*>(image.data), image.total() * image.elemSize());

    // 检查是否成功写入
    if (!outFile) {
        Logger::Log("Failed to write to file: " + filePath, LogLevel::ERROR, DeviceType::FOCUSER);
        if (isFocusLoopShooting){
            FocusingLooping();
        }
        return;
    }

    outFile.close();

    bool saved = true;

    std::string Command = "ln -sf " + filePath + " " + vueImagePath + fileName;
    system(Command.c_str());
    Logger::Log("Symbolic link created for new image file.", LogLevel::DEBUG, DeviceType::FOCUSER);


    if (saved)
    {
        Logger::Log("SaveJpgSuccess:" + fileName + " to " + filePath +",image size:" + std::to_string(image.cols) + "x" + std::to_string(image.rows), LogLevel::DEBUG, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("SaveJpgSuccess:" + QString::fromStdString(fileName) + ":" + QString::number(glROI_x) + ":" + QString::number(glROI_y));
        if (isAutoFocus){
            AutoFocus(currentSelectStarPosition);
        }
    
    }else{
        Logger::Log("Failed to save image.", LogLevel::ERROR, DeviceType::GUIDER);
    }
    focusLoopShooting(isFocusLoopShooting);
}

void MainWindow::AutoFocus(QPointF selectStarPosition){
    if (dpFocuser == NULL || dpMainCamera == NULL){
        Logger::Log("AutoFocus | dpFocuser or dpMainCamera is NULL", LogLevel::WARNING, DeviceType::FOCUSER);
        isAutoFocus = false;
        emit wsThread->sendMessageToClient("AutoFocusOver:true");
        return;
    }
    if (selectStarPosition.y() == 0 || selectStarPosition.isNull()){
        Logger::Log("AutoFocus | selectStarPosition is NULL", LogLevel::WARNING, DeviceType::FOCUSER);
        overSelectStarAutoFocusStep += 1;
        return;
    }

    int currentPosition = FocuserControl_getPosition();

    int steps = 100;
    // 初始化清空保存列表
    if (autoFocusStep == 0){
        currentAutoFocusStarPositionList.clear();
        allAutoFocusStarPositionList.clear();
        autoFocusStep = 1;

        // 定义开始方向
        int value, min, max, step;
        indi_Client->getFocuserSpeed(dpFocuser, value, min, max, step);
                
        if (currentPosition > (max-min)/2){
            currentDirection = false;
        }else{
            currentDirection = true;
        }
    }

    // 进行5次拟合，确定五个点
    if (autoFocusStep >= 1 && autoFocusStep <= 5){
        currentAutoFocusStarPositionList.append(selectStarPosition);
        if (currentAutoFocusStarPositionList.size() == 3){
            if (currentAutoFocusStarPositionList[0].x() == currentAutoFocusStarPositionList[1].x() && currentAutoFocusStarPositionList[0].x() == currentAutoFocusStarPositionList[2].x()){
                allAutoFocusStarPositionList.append(currentAutoFocusStarPositionList[0]);
                allAutoFocusStarPositionList.append(currentAutoFocusStarPositionList[1]);
                allAutoFocusStarPositionList.append(currentAutoFocusStarPositionList[2]);
                currentAutoFocusStarPositionList.clear();
                autoFocusStep++;
                indi_Client->setFocuserMoveDiretion(dpFocuser, currentDirection);
                indi_Client->moveFocuserSteps(dpFocuser, steps);
                int timeOut = 0;
                while (FocuserControl_getPosition() == currentPosition + steps || FocuserControl_getPosition() == currentPosition - steps){
                    QThread::msleep(100);
                    timeOut++;
                    if (timeOut > 30){
                        Logger::Log("AutoFocus | 获取到第" + std::to_string(autoFocusStep) + "步的3个点,但电调位置移动失败！", LogLevel::WARNING, DeviceType::FOCUSER);
                        break;
                    }
                }
                Logger::Log("AutoFocus | 获取到第" + std::to_string(autoFocusStep) + "步的3个点!", LogLevel::INFO, DeviceType::FOCUSER);
            }else{
                currentAutoFocusStarPositionList.clear();
                Logger::Log("AutoFocus | 获取到第" + std::to_string(autoFocusStep) + "步的3个点,但电调位置移动,重新开始！", LogLevel::WARNING, DeviceType::FOCUSER);
            }
        }else{
            Logger::Log("AutoFocus | 获取到第" + std::to_string(autoFocusStep) + "步的第" + std::to_string(currentAutoFocusStarPositionList.size()) + "个点", LogLevel::INFO, DeviceType::FOCUSER);
        }
        
    }
    

    if (autoFocusStep == 6){
        // 计算拟合曲线
        float a, b, c;
        bool isSuccess = fitQuadraticCurve(allAutoFocusStarPositionList, a, b, c);
        if (isSuccess){
            Logger::Log("AutoFocus | 拟合曲线成功！ y = " + std::to_string(a) + "x^2 + " + std::to_string(b) + "x + " + std::to_string(c), LogLevel::INFO, DeviceType::FOCUSER);
        }else{
            Logger::Log("AutoFocus | 拟合曲线失败！", LogLevel::WARNING, DeviceType::FOCUSER);
        }
    }

}

int MainWindow::fitQuadraticCurve(const QVector<QPointF>& data, float& a, float& b, float& c) {
    int n = data.size();
    if (n < 3) {
        qDebug() << "Not enough data points for fitting.";
        return -1; // 数据点数量不足
    }

    cv::Mat A(n, 3, CV_32F);
    cv::Mat B(n, 1, CV_32F);
    cv::Mat W = cv::Mat::eye(n, n, CV_32F); // 权重矩阵

    // 初始化矩阵 A 和 B
    for (int i = 0; i < n; ++i) {
        float x = data[i].x();
        float y = data[i].y();
        A.at<float>(i, 0) = x * x;
        A.at<float>(i, 1) = x;
        A.at<float>(i, 2) = 1;
        B.at<float>(i, 0) = y;
    }

    cv::Mat X;
    const float delta = 1.0; // Huber 损失的阈值
    const int maxIterations = 10;

    for (int iter = 0; iter < maxIterations; ++iter) {
        cv::solve(W * A, W * B, X, cv::DECOMP_QR);

        // 更新权重
        for (int i = 0; i < n; ++i) {
            float x = data[i].x();
            float y = data[i].y();
            float yFit = X.at<float>(0, 0) * x * x + X.at<float>(1, 0) * x + X.at<float>(2, 0);
            float error = std::abs(y - yFit);

            // 使用 Huber 损失函数更新权重
            if (error <= delta) {
                W.at<float>(i, i) = 1.0;
            } else {
                W.at<float>(i, i) = delta / error;
            }
        }
    }

    a = X.at<float>(0, 0);
    b = X.at<float>(1, 0);
    c = X.at<float>(2, 0);

    Logger::Log("AutoFocus | 拟合曲线成功！ y = " + std::to_string(a) + "x^2 + " + std::to_string(b) + "x + " + std::to_string(c), LogLevel::INFO, DeviceType::FOCUSER);

    // 识别并去掉异常点
    QVector<QPointF> filteredData;
    for (int i = 0; i < n; ++i) {
        if (W.at<float>(i, i) >= 0.5) { // 仅保留高权重点
            filteredData.append(data[i]);
        }
    }

    // 如果去掉异常点后数据点不足，返回失败
    if (filteredData.size() < 3) {
        Logger::Log("AutoFocus | 拟合曲线过滤后数据点不足！", LogLevel::WARNING, DeviceType::FOCUSER);
        return -1;
    }

    // 打印过滤后的数据点
    // qDebug() << "Filtered data points:";
    for (const QPointF &p : filteredData) {
        // Logger::Log("AutoFocus | 拟合曲线过滤后的数据点: " + std::to_string(p.x()) + "," + std::to_string(p.y()), LogLevel::INFO, DeviceType::FOCUSER);
    }

    // 重新拟合去掉异常点后的数据
    // 避免递归调用
    if (filteredData.size() != data.size()) {
        return fitQuadraticCurve(filteredData, a, b, c);
    }

    return 0;
}

void MainWindow::getFocuserLoopingState(){
    if (isFocusLoopShooting){
        emit wsThread->sendMessageToClient("setFocuserLoopingState:true");
    }
    else{
        emit wsThread->sendMessageToClient("setFocuserLoopingState:false");
    }
}

void MainWindow::sendRoiInfo(){
    // 检查并获取参数，如果不存在则使用默认值
    double boxSideLength = roiAndFocuserInfo.count("BoxSideLength") ? roiAndFocuserInfo["BoxSideLength"] : this->BoxSideLength;
    double roi_x = roiAndFocuserInfo.count("ROI_x") ? roiAndFocuserInfo["ROI_x"] : this->glROI_x;
    double roi_y = roiAndFocuserInfo.count("ROI_y") ? roiAndFocuserInfo["ROI_y"] : this->glROI_y;
    double visibleX = roiAndFocuserInfo.count("VisibleX") ? roiAndFocuserInfo["VisibleX"] : 0;
    double visibleY = roiAndFocuserInfo.count("VisibleY") ? roiAndFocuserInfo["VisibleY"] : 0;
    double scale = roiAndFocuserInfo.count("scale") ? roiAndFocuserInfo["scale"] : 1;
    double selectStarX = roiAndFocuserInfo.count("SelectStarX") ? roiAndFocuserInfo["SelectStarX"] : -1;
    double selectStarY = roiAndFocuserInfo.count("SelectStarY") ? roiAndFocuserInfo["SelectStarY"] : -1;

    // 发送参数
    emit wsThread->sendMessageToClient("SetRedBoxState:" + QString::number(boxSideLength) + ":" + QString::number(roi_x) + ":" + QString::number(roi_y));
    emit wsThread->sendMessageToClient("SetVisibleArea:" + QString::number(visibleX) + ":" + QString::number(visibleY) + ":" + QString::number(scale));
    emit wsThread->sendMessageToClient("SetSelectStars:" + QString::number(selectStarX) + ":" + QString::number(selectStarY));
}

QPointF MainWindow::selectStar(QList<FITSImage::Star> stars){
    // 拍摄完成后，找到目标星点
    if (stars.size() <= 0){
        Logger::Log("selectStar | No star found in stars", LogLevel::INFO, DeviceType::FOCUSER);
        return QPointF(FocuserControl_getPosition(), 0);
    }
    // 选择星点
    double selectStarX = roiAndFocuserInfo["SelectStarX"]; 
    double selectStarY = roiAndFocuserInfo["SelectStarY"];
    double selectStarHFR = 0;
    if (selectStarX == -1 || selectStarY == -1){
        // 未手动选择，则选择最亮的那颗且居中的那颗
        double minScore = std::numeric_limits<double>::max();
        double centerX = BoxSideLength / 2.0;  // 假设 'image' 是捕获的图像
        double centerY = BoxSideLength / 2.0;

        // 计算中位数 HFR
        std::vector<double> HFRs;
        for (const auto &star : stars) {
            HFRs.push_back(star.HFR);
        }
        std::sort(HFRs.begin(), HFRs.end());
        double medianHFR = HFRs[HFRs.size() / 2];

        for (const auto &star : stars)
        {
            double distanceToCenter = std::sqrt(std::pow(star.x - centerX, 2) + std::pow(star.y - centerY, 2));
            double alpha = 0.1;
            // 'alpha' 应根据您的需求进行定义
            // 计算得分，得分是星星的 HFR 值与中位数的差的绝对值和星星到图像中心距离的加权和
            double score = std::abs(star.HFR - medianHFR) + alpha * distanceToCenter;  
            if (score < minScore) {
                minScore = score;
                selectStarX = star.x;
                selectStarY = star.y;
                selectStarHFR = star.HFR;
            }
        }
    }
    else{
        selectStarX = roiAndFocuserInfo["SelectStarX"]-roiAndFocuserInfo["ROI_x"]; 
        selectStarY = roiAndFocuserInfo["SelectStarY"]-roiAndFocuserInfo["ROI_y"]; 
        // 手动选择，则选择手动选择的那颗
        double minDistance = std::numeric_limits<double>::max();
        for (const auto &star : stars)
        {
            double distance = std::sqrt(std::pow(star.x - selectStarX, 2) + std::pow(star.y - selectStarY, 2));
            if (distance < minDistance) {
                minDistance = distance;
                selectStarX = star.x;
                selectStarY = star.y;
                selectStarHFR = star.HFR;
            }
        }
    }

    // 将星点信息和电调位置存储到列表中

    // starWithFocuserPosition.focuserPosition = FocuserControl_getPosition();
    // focusMeasures.append(QPointF(starWithFocuserPosition.focuserPosition, starWithFocuserPosition.HFR));

    return QPointF(FocuserControl_getPosition(),selectStarHFR);
}

// void MainWindow::FocuserControl_Goto(int Position)
// {
//     Logger::Log("FocuserControl_Goto start ...", LogLevel::INFO, DeviceType::MAIN);
//     if (dpFocuser != NULL) 
//     {
//         focusTimer.stop();
//         focusTimer.disconnect();

//         CurrentPosition = FocuserControl_getPosition();
//         Logger::Log("FocuserControl_Goto | CurrentPosition:" + std::to_string(CurrentPosition), LogLevel::INFO, DeviceType::MAIN);
//         TargetPosition = Position;

//         Logger::Log("FocuserControl_Goto | TargetPosition:" + std::to_string(TargetPosition), LogLevel::INFO, DeviceType::MAIN);

//         indi_Client->moveFocuserToAbsolutePosition(dpFocuser, Position);
//         Logger::Log("FocuserControl_Goto | start Goto focuser ...", LogLevel::INFO, DeviceType::MAIN);

//         focusTimer.setSingleShot(true);

//         connect(&focusTimer, &QTimer::timeout, [this]() {
//             CurrentPosition = FocuserControl_getPosition();
//             emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(TargetPosition));
            
//             if (WaitForFocuserToComplete()) 
//             {
//                 focusTimer.stop();  // 转动完成时停止定时器
//                 Logger::Log("FocuserControl_Goto | Focuser Goto Complete!", LogLevel::INFO, DeviceType::MAIN);
//                 emit wsThread->sendMessageToClient("FocusMoveDone:" + QString::number(FWHM));
//                 FocusingLooping();
//             } 
//             else 
//             {
//                 Logger::Log("FocuserControl_Goto | Focuser Goto Complete!", LogLevel::INFO, DeviceType::MAIN);
//                 emit wsThread->sendMessageToClient("FocusMoveDone:" + QString::number(FWHM));
//                 Logger::Log("FocuserControl_Goto | continue wait for focuser to complete ...", LogLevel::INFO, DeviceType::MAIN);
//                 // focusTimer.start(100);  // 继续等待
//             } 
//         });

//         focusTimer.start(100);
//     }
//     else 
//     {
//         Logger::Log("FocuserControl_Goto | dpFocuser is NULL", LogLevel::INFO, DeviceType::MAIN);
//         emit wsThread->sendMessageToClient("FocusMoveDone:" + 0);
//     }
//     Logger::Log("FocuserControl_Goto finish!", LogLevel::INFO, DeviceType::MAIN);
// }

// void MainWindow::AutoFocus() 
// {
//     Logger::Log("AutoFocus start ...", LogLevel::INFO, DeviceType::MAIN);
//     StopAutoFocus = false; // 初始化停止自动对焦标志为false
//     int stepIncrement = 100; // 每次移动的步长

//     bool isInward = true; // 初始移动方向为向内

//     Logger::Log("AutoFocus | to move focuser 5 steps outward ...", LogLevel::INFO, DeviceType::MAIN);
//     FocusMoveAndCalHFR(!isInward, stepIncrement * 5); // 向外移动5个步长并计算HFR

//     int initialPosition = FocuserControl_getPosition(); // 获取初始位置
//     int currentPosition = initialPosition; // 当前对焦器位置

//     int Pass3Steps; // 第三阶段的步数

//     QVector<QPointF> focusMeasures; // 存储对焦测量值

//     int OnePassSteps = 8; // 每个阶段的步数
//     int LostStarNum = 0; // 丢失星点的计数

//     Logger::Log("AutoFocus | to move focuser 5 steps inward ...", LogLevel::INFO, DeviceType::MAIN);
//     // 第一阶段：向内移动并计算HFR
//     for(int i = 1; i < OnePassSteps; i++) {
//         if (StopAutoFocus) { // 检查是否停止自动对焦
//             Logger::Log("AutoFocus | Stop Auto Focus...", LogLevel::INFO, DeviceType::MAIN);
//             emit wsThread->sendMessageToClient("AutoFocusOver:true");
//             return;
//         }
//         double HFR = FocusMoveAndCalHFR(isInward, stepIncrement); // 计算HFR(半光通量半径)
//         Logger::Log("AutoFocus | Pass1: HFR-" + std::to_string(i) + "(" + std::to_string(HFR) + ") Calculation Complete!", LogLevel::INFO, DeviceType::MAIN);
//         if (HFR == -1) { // 检查HFR是否有效
//             LostStarNum++;
//             if (LostStarNum >= 3) { // 如果丢失星点超过3次，返回初始位置
//                 Logger::Log("AutoFocus | Too many number of lost star points.", LogLevel::INFO, DeviceType::MAIN);
//                 FocusGotoAndCalFWHM(initialPosition-stepIncrement * 5);
//                 Logger::Log("AutoFocus | Returned to the starting point.", LogLevel::INFO, DeviceType::MAIN);
//                 emit wsThread->sendMessageToClient("AutoFocusOver:true");
//                 return;
//             }
//         }
//         currentPosition = FocuserControl_getPosition(); // 更新当前对焦器位置
//         focusMeasures.append(QPointF(currentPosition, HFR)); // 记录测量值
//     }

//     // 拟合抛物线并检查结果
//     float a, b, c;
//     int result = Tools::fitQuadraticCurve(focusMeasures, a, b, c);
//     if (result == 0)
//     {
//         if(R2 < 0.8) { // 检查拟合的R²值
//             Logger::Log("AutoFocus | not good fit, R² < 0.8", LogLevel::INFO, DeviceType::MAIN);
//             emit wsThread->sendMessageToClient("AutoFocusOver:true");
//             return;
//         }

//         if(a < 0) { // 检查抛物线的开口方向
//             Logger::Log("AutoFocus | parabola is downward", LogLevel::INFO, DeviceType::MAIN);
//         }

//         int countLessThan = 0;
//         int countGreaterThan = 0;

//         // 统计测量点相对于最小点的分布
//         for (const QPointF &point : focusMeasures)
//         {
//             if (point.x() < minPoint_X)
//             {
//                 countLessThan++;
//             }
//             else if (point.x() > minPoint_X)
//             {
//                 countGreaterThan++;
//             }
//         }

//         // 根据分布调整对焦器
//         if (countLessThan > countGreaterThan) {
//             Logger::Log("AutoFocus | More points are less than minPoint_X.", LogLevel::INFO, DeviceType::MAIN);
//             if(a > 0) {
//                 FocusMoveAndCalHFR(!isInward, stepIncrement * (OnePassSteps-1) * 2);
//             } 
//         }
//         else if (countGreaterThan > countLessThan) {
//             Logger::Log("AutoFocus | More points are greater than minPoint_X.", LogLevel::INFO, DeviceType::MAIN);
//             if(a < 0) {
//                 FocusMoveAndCalHFR(!isInward, stepIncrement * (OnePassSteps-1) * 2);
//             }
//         }
//     }

//     // 第二阶段：继续向内移动并计算HFR
//     for(int i = 1; i < OnePassSteps; i++) {
//         if (StopAutoFocus) {
//             Logger::Log("AutoFocus | Stop Auto Focus...", LogLevel::INFO, DeviceType::MAIN);
//             emit wsThread->sendMessageToClient("AutoFocusOver:true");
//             return;
//         }
//         double HFR = FocusMoveAndCalHFR(isInward, stepIncrement);
//         Logger::Log("AutoFocus | Pass2: HFR-" + std::to_string(i) + "(" + std::to_string(HFR) + ") Calculation Complete!", LogLevel::INFO, DeviceType::MAIN);
//         currentPosition = FocuserControl_getPosition();
//         focusMeasures.append(QPointF(currentPosition, HFR));
//     }

//     // 再次拟合抛物线并检查结果
//     Logger::Log("AutoFocus | to fit quadratic curve again ...", LogLevel::INFO, DeviceType::MAIN);
//     float a_, b_, c_;
//     int result_ = Tools::fitQuadraticCurve(focusMeasures, a_, b_, c_);
//     if (result_ == 0)
//     {
//         if(R2 < 0.8) {
//             Logger::Log("AutoFocus | not good fit, R² < 0.8", LogLevel::INFO, DeviceType::MAIN);
//             emit wsThread->sendMessageToClient("AutoFocusOver:true");
//             return;
//         }

//         if(a_ < 0) {
//             Logger::Log("AutoFocus | parabola is downward", LogLevel::INFO, DeviceType::MAIN);
//         }

//         int countLessThan = 0;
//         int countGreaterThan = 0;

//         for (const QPointF &point : focusMeasures)
//         {
//             if (point.x() < minPoint_X)
//             {
//                 countLessThan++;
//             }
//             else if (point.x() > minPoint_X)
//             {
//                 countGreaterThan++;
//             }
//         }

//         // 根据分布调整对焦器
//         if (countLessThan > countGreaterThan) {
//             Logger::Log("AutoFocus | More points are less than minPoint_X.", LogLevel::INFO, DeviceType::MAIN);
//             Pass3Steps = countLessThan-countGreaterThan;
//             Logger::Log("AutoFocus | Pass3Steps: " + std::to_string(Pass3Steps), LogLevel::INFO, DeviceType::MAIN);

//             FocusGotoAndCalFWHM(minPoint_X);

//             FocusMoveAndCalHFR(!isInward, stepIncrement * countLessThan);
//         }
//         else if (countGreaterThan > countLessThan) {
//             Logger::Log("AutoFocus | More points are greater than minPoint_X.", LogLevel::INFO, DeviceType::MAIN);
//             Pass3Steps = countGreaterThan - countLessThan;
//             Logger::Log("AutoFocus | Pass3Steps: " + std::to_string(Pass3Steps), LogLevel::INFO, DeviceType::MAIN);

//             FocusGotoAndCalFWHM(minPoint_X);
//             if(countLessThan > 0){
//                 FocusMoveAndCalHFR(isInward, stepIncrement * countLessThan);
//             }
//         }
//         else {
//             Logger::Log("AutoFocus | The number of points less than and greater than minPoint_X is equal.", LogLevel::INFO, DeviceType::MAIN);
//             FocusGotoAndCalFWHM(minPoint_X);
//             Logger::Log("AutoFocus | Auto focus complete. Best step: " + std::to_string(minPoint_X), LogLevel::INFO, DeviceType::MAIN);
//             emit wsThread->sendMessageToClient("AutoFocusOver:true");
//             return;
//         }
//     }

//     // 第三阶段：根据Pass3Steps调整对焦器
//     for(int i = 1; i < Pass3Steps + 1; i++) {
//         if (StopAutoFocus) {
//             Logger::Log("AutoFocus | Stop Auto Focus...", LogLevel::INFO, DeviceType::MAIN);
//             emit wsThread->sendMessageToClient("AutoFocusOver:true");
//             return;
//         }
//         double HFR = FocusMoveAndCalHFR(isInward, stepIncrement);
//         Logger::Log("AutoFocus | Pass3: HFR-" + std::to_string(i) + "(" + std::to_string(HFR) + ") Calculation Complete!", LogLevel::INFO, DeviceType::MAIN);
//         currentPosition = FocuserControl_getPosition();
//         focusMeasures.append(QPointF(currentPosition, HFR));
//     }

//     FocusGotoAndCalFWHM(minPoint_X); // 移动到最佳位置
//     Logger::Log("AutoFocus | Auto focus complete. Best step: " + std::to_string(minPoint_X), LogLevel::INFO, DeviceType::MAIN);
//     currentPosition = FocuserControl_getPosition();
//     Logger::Log("AutoFocus | Current Position : " + std::to_string(currentPosition), LogLevel::INFO, DeviceType::MAIN);
//     emit wsThread->sendMessageToClient("AutoFocusOver:true"); // 通知自动对焦完成
// }


// double MainWindow::FocusMoveAndCalHFR(bool isInward, int steps) {
//     // 记录开始移动和计算FWHM的日志
//     Logger::Log("FocusMoveAndCalHFR start ...", LogLevel::INFO, DeviceType::FOCUSER);
//     QEventLoop loop;
//     double FWHM = 0;

//     // 停止并断开之前的计时器连接，准备新的操作
//     FWHMTimer.stop();
//     FWHMTimer.disconnect();

//     // 设置FWHM计算未完成的标志
//     FWHMCalOver = false;
//     // 调用移动焦点器的函数，传入移动方向和步数
//     FocuserControl_Move(isInward, steps);

//     // 设置计时器为单次触发
//     FWHMTimer.setSingleShot(true);

//     // 连接计时器的超时信号到处理函数，用于处理FWHM计算
//     connect(&FWHMTimer, &QTimer::timeout, this, [this, &loop, &FWHM]() {
//         if (FWHMCalOver) // 如果FWHM计算完成
//         {
//             FWHM = this->FWHM;  // 获取计算结果
//             FWHMTimer.stop();  // 停止计时器
//             // 记录FWHM计算完成的日志
//             Logger::Log("FocusMoveAndCalHFR | FWHM Calculation Complete!", LogLevel::INFO, DeviceType::FOCUSER);
//             // 发送计算完成的消息给客户端
//             emit wsThread->sendMessageToClient("FocusMoveDone:" + QString::number(FWHM));
//             loop.quit();  // 退出事件循环
//         } 
//         else 
//         {
//             FWHMTimer.start(1000);  // 如果FWHM未计算完成，重新启动计时器，继续等待
//         }
//     });

//     FWHMTimer.start(1000);  // 启动计时器，等待FWHM计算
//     loop.exec();  // 进入事件循环，等待计时器信号或计算完成

//     return FWHM;  // 返回计算得到的FWHM值
// }

void MainWindow::updateCPUInfo()
{
    // 获取CPU温度和使用率
    QProcess process;
    // 在树莓派上，可以通过读取 /sys/class/thermal/thermal_zone0/temp 文件来获取 CPU 温度
    process.start("cat", QStringList() << "/sys/class/thermal/thermal_zone0/temp");
    process.waitForFinished();
    QString output = process.readAllStandardOutput();
    float cpuTemp = output.toFloat() / 1000;  // 转换为摄氏度
    if (process.error() != QProcess::UnknownError) {
        cpuTemp = std::numeric_limits<float>::quiet_NaN();  // 如果获取失败，设置为 NaN
    }

    // 在树莓派上，可以通过运行 'top' 命令并解析输出来获取 CPU 使用率
    process.start("sh", QStringList() << "-c" << "top -b -n1 | grep 'Cpu(s)' | awk '{print $2}' | cut -c 1-4");
    process.waitForFinished();
    output = process.readAllStandardOutput();
    QStringList cpuUsages = output.split("\n");
    float cpuUsage = 0;
    if (cpuUsages.size() > 0) {
        cpuUsage = cpuUsages[0].toDouble();
    }
    if (process.error() != QProcess::UnknownError) {
        cpuUsage = std::numeric_limits<float>::quiet_NaN();  // 如果获取失败，设置为 NaN
    }

    Logger::Log("updateCPUInfo | CPU Temp: " + std::to_string(cpuTemp) + ", CPU Usage: " + std::to_string(cpuUsage), LogLevel::DEBUG, DeviceType::MAIN);
    emit wsThread->sendMessageToClient("updateCPUInfo:" + QString::number(cpuTemp) + ":" + QString::number(cpuUsage));
}

