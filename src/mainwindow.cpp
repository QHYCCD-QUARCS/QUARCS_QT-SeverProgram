#include "mainwindow.h"

INDI::BaseDevice *dpMount, *dpGuider, *dpPoleScope;
INDI::BaseDevice *dpMainCamera, *dpFocuser, *dpCFW;

DriversList drivers_list;
std::vector<DevGroup> dev_groups;
std::vector<Device> devices;

DriversListNew drivers_list_new;

SystemDevice systemdevice;
SystemDeviceList systemdevicelist;

// QUrl websocketUrl(QStringLiteral("ws://192.168.2.31:8600"));
QUrl websocketUrl;

MainWindow::MainWindow(QObject *parent) : QObject(parent)
{
    getHostAddress();

    wsThread = new WebSocketThread(websocketUrl);
    connect(wsThread, &WebSocketThread::receivedMessage, this, &MainWindow::onMessageReceived);
    wsThread->start();

    InitPHD2();

    initINDIServer();
    initINDIClient();

    initGPIO();

    readDriversListFromFiles("/usr/share/indi/drivers.xml", drivers_list, dev_groups, devices);

    Tools::InitSystemDeviceList();
    Tools::initSystemDeviceList(systemdevicelist);
    Tools::makeConfigFolder();
    Tools::makeImageFolder();

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
}

MainWindow::~MainWindow()
{
    system("pkill indiserver");
    system("rm -f /tmp/myFIFO");

    wsThread->quit();
    wsThread->wait();
    delete wsThread;
}

void MainWindow::getHostAddress()
{
    QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    foreach (const QNetworkInterface &interface, interfaces) {
        // 排除回环接口和非活动接口
        if (interface.flags() & QNetworkInterface::IsLoopBack || !(interface.flags() & QNetworkInterface::IsUp))
            continue;

        QList<QNetworkAddressEntry> addresses = interface.addressEntries();
        foreach (const QNetworkAddressEntry &address, addresses) {
            if (address.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                QString localIpAddress = address.ip().toString();
                qDebug() << "Local IP Address:" << address.ip().toString();

                if (!localIpAddress.isEmpty()) {
                    QUrl getUrl(QStringLiteral("ws://%1:8600").arg(localIpAddress));
                    qDebug() << "WebSocket URL:" << getUrl.toString();
                    websocketUrl = getUrl;
                } else {
                    qDebug() << "Failed to get local IP address.";
                }
            }
        }
    }
}

void MainWindow::onMessageReceived(const QString &message)
{
    // 处理接收到的消息
    qDebug() << "Received message in MainWindow:" << message;
    // 分割消息
    QStringList parts = message.split(':');

    if (parts.size() == 2 && parts[0].trimmed() == "ConfirmIndiDriver")
    {
        QString driverName = parts[1].trimmed();
        indi_Driver_Confirm(driverName);
    }
    else if (parts.size() == 3 && parts[0].trimmed() == "ConfirmIndiDevice")
    {
        QString deviceName = parts[1].trimmed();
        QString driverName = parts[2].trimmed();
        // connectDevice(x);
        indi_Device_Confirm(deviceName, driverName);
    }
    else if (parts.size() == 3 && parts[0].trimmed() == "SelectIndiDriver")
    {
        QString Group = parts[1].trimmed();
        int ListNum = parts[2].trimmed().toInt();
        printDevGroups2(drivers_list, ListNum, Group);
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "takeExposure")
    {
        int ExpTime = parts[1].trimmed().toInt();
        qDebug() << ExpTime;
        INDI_Capture(ExpTime);
        glExpTime = ExpTime;
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "focusSpeed")
    {
        int Speed = parts[1].trimmed().toInt();
        qDebug() << Speed;
        int Speed_ = FocuserControl_setSpeed(Speed);
        emit wsThread->sendMessageToClient("FocusChangeSpeedSuccess:" + QString::number(Speed_));
    }
    else if (parts.size() == 3 && parts[0].trimmed() == "focusMove")
    {
        QString LR = parts[1].trimmed();
        int Steps = parts[2].trimmed().toInt();
        if(LR == "Left")
        {
            FocusMoveAndCalHFR(true,Steps);
        }
        else if(LR == "Right")
        {
            FocusMoveAndCalHFR(false,Steps);
        }
        else if(LR == "Target")
        {
            FocusGotoAndCalFWHM(Steps);
        }
    }
    else if (parts.size() == 5 && parts[0].trimmed() == "RedBox")
    {
        int x = parts[1].trimmed().toInt();
        int y = parts[2].trimmed().toInt();
        int width = parts[3].trimmed().toInt();
        int height = parts[4].trimmed().toInt();
        glROI_x = x;
        glROI_y = y;
        CaptureViewWidth = width;
        CaptureViewHeight = height;
        qDebug() << "RedBox:" << glROI_x << glROI_y << CaptureViewWidth << CaptureViewHeight;
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "RedBoxSizeChange")
    {
        BoxSideLength = parts[1].trimmed().toInt();
        qDebug() << "BoxSideLength:" << BoxSideLength;
        emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));
    }
    else if (message == "AutoFocus")
    {
        AutoFocus();
    }
    else if (message == "StopAutoFocus")
    {
        StopAutoFocus = true;
    }
    else if (message == "abortExposure")
    {
        INDI_AbortCapture();
    }
    else if (message == "connectAllDevice")
    {
        DeviceConnect();
    }
    else if (message == "CS")
    {
        // QString Dev = connectIndiServer();
        // websocket->messageSend("AddDevice:"+Dev);
    }
    else if (message == "disconnectAllDevice")
    {
        disconnectIndiServer(indi_Client);
        ClearSystemDeviceList();
        clearConnectedDevices();

        initINDIServer();
        initINDIClient();
        Tools::InitSystemDeviceList();
        Tools::initSystemDeviceList(systemdevicelist);
    }
    else if (message == "MountMoveWest")
    {
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeMoveWE(dpMount, "WEST");
        }
    }
    else if (message == "MountMoveEast")
    {
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeMoveWE(dpMount, "EAST");
        }
    }
    else if (message == "MountMoveNorth")
    {
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeMoveNS(dpMount, "NORTH");
        }
    }
    else if (message == "MountMoveSouth")
    {
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeMoveNS(dpMount, "SOUTH");
        }
    }
    else if (message == "MountMoveAbort")
    {
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeAbortMotion(dpMount);
        }
    }
    else if (message == "MountPark")
    {
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
        if (dpMount != NULL)
        {
            bool isTrack = TelescopeControl_Track();
            if(isTrack)
            {
                emit wsThread->sendMessageToClient("TelescopeTrack:ON");
            }
            else
            {
                emit wsThread->sendMessageToClient("TelescopeTrack:OFF");
            }
        }
    }
    else if (message == "MountHome")
    {
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeHomeInit(dpMount,"SLEWHOME");
        }
    }
    else if (message == "MountSYNC")
    {
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeHomeInit(dpMount,"SYNCHOME");
        }
    }

    else if (message == "MountSpeedSwitch")
    {
        if (dpMount != NULL)
        {
            int currentSpeed;
            indi_Client->getTelescopeSlewRate(dpMount,currentSpeed);
            qDebug() << "Current Speed:" << currentSpeed;

            qDebug() << "Total Speed:" << glTelescopeTotalSlewRate;

            if(currentSpeed == glTelescopeTotalSlewRate) {
                indi_Client->setTelescopeSlewRate(dpMount,1);
            } else {
                indi_Client->setTelescopeSlewRate(dpMount,currentSpeed+1);
                qDebug() << "Set Speed to:" << currentSpeed;
            }

            int ChangedSpeed;
            indi_Client->getTelescopeSlewRate(dpMount,ChangedSpeed);
            qDebug() << "Changed Speed:" << ChangedSpeed;
            emit wsThread->sendMessageToClient("MountSetSpeedSuccess:" + QString::number(ChangedSpeed));
        }
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "ImageGainR")
    {
        ImageGainR = parts[1].trimmed().toDouble();
        qDebug() << "GainR is set to " << ImageGainR;
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "ImageGainB")
    {
        ImageGainB = parts[1].trimmed().toDouble();
        qDebug() << "GainB is set to " << ImageGainB;
    }

    else if (parts[0].trimmed() == "ScheduleTabelData")
    {
        ScheduleTabelData(message);
    }

    else if (parts.size() == 4 && parts[0].trimmed() == "MountGoto")
    {
        QStringList RaDecList = message.split(',');
        QStringList RaList = RaDecList[0].split(':');
        QStringList DecList = RaDecList[1].split(':');

        double Ra_Rad, Dec_Rad;
        Ra_Rad  = RaList[2].trimmed().toDouble();
        Dec_Rad = DecList[1].trimmed().toDouble();

        qDebug() << "RaDec(Rad):" << Ra_Rad << "," << Dec_Rad;

        double Ra_Hour, Dec_Degree;
        Ra_Hour  = Tools::RadToHour(Ra_Rad);
        Dec_Degree = Tools::RadToDegree(Dec_Rad);

        MountGoto(Ra_Hour, Dec_Degree);
    }

    else if (message == "StopSchedule")
    {
        StopSchedule = true;
    }

    else if (message == "CaptureImageSave")
    {
        CaptureImageSave();
    }

    else if (message == "getConnectedDevices")
    {
        getConnectedDevices();
    }

    else if (message == "getStagingImage")
    {
        getStagingImage();
    }

    else if (parts[0].trimmed() == "StagingScheduleData")
    {
        isStagingScheduleData = true;
        StagingScheduleData = message;
    }

    else if (message == "getStagingScheduleData")
    {
        getStagingScheduleData();
    }

    else if (message == "getStagingGuiderData")
    {
        getStagingGuiderData();
    }

    else if (parts[0].trimmed() == "ExpTimeList")
    {
        Tools::saveExpTimeList(message);
    }

    else if (message == "getExpTimeList")
    {
        if (Tools::readExpTimeList() != QString()){
            emit wsThread->sendMessageToClient(Tools::readExpTimeList());
        }
    }

    else if (message == "getCaptureStatus")
    {
        qDebug() << "INDI Capture Statu:" << glMainCameraStatu;
        if (glMainCameraStatu == "Exposuring")
        {
            emit wsThread->sendMessageToClient("CameraInExposuring:True");
        }
    }

    else if (parts[0].trimmed() == "SetCFWPosition")
    {
        int pos = parts[1].trimmed().toInt();

        if(isFilterOnCamera) {
            if (dpMainCamera != NULL)
            {
                indi_Client->setCFWPosition(dpMainCamera, pos);
                emit wsThread->sendMessageToClient("SetCFWPositionSuccess:" + QString::number(pos));
                qDebug() << "Set CFW Position to" << pos << "Success!!!";
            }
        } else {
            if (dpCFW != NULL)
            {
                indi_Client->setCFWPosition(dpCFW, pos);
                emit wsThread->sendMessageToClient("SetCFWPositionSuccess:" + QString::number(pos));
                qDebug() << "Set CFW Position to" << pos << "Success!!!";
            }
        }
    }

    else if (parts[0].trimmed() == "CFWList")
    {
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
    }

    else if (message == "getCFWList")
    {
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
    }

    else if (message == "ClearCalibrationData")
    {
        ClearCalibrationData = true;
        qDebug() << "ClearCalibrationData: " << ClearCalibrationData;
    }

    else if (message == "GuiderSwitch")
    {
        if (isGuiding){
            isGuiding = false;
            call_phd_StopLooping();
            emit wsThread->sendMessageToClient("GuiderSwitchStatus:false");
            isGuiderLoopExp = false;
            emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        } else {
            isGuiding = true;
            // emit wsThread->sendMessageToClient("GuiderSwitchStatus:true");
            if (ClearCalibrationData) {
                ClearCalibrationData = false;
                call_phd_ClearCalibration();
            }

            // call_phd_StartLooping();
            // sleep(1);
            if (glPHD_isSelected == false) {
                call_phd_AutoFindStar();
            }
            call_phd_StartGuiding();
        }
    }

    else if (message == "GuiderLoopExpSwitch")
    {
        if (isGuiderLoopExp){
            isGuiderLoopExp = false;
            call_phd_StopLooping();
            emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        } else {
            isGuiderLoopExp = true;
            // emit wsThread->sendMessageToClient("GuiderLoopExpStatus:true");
            call_phd_StartLooping();
        }
    }

    else if (message == "PHD2Recalibrate")
    {
        call_phd_ClearCalibration();
        call_phd_StartLooping();
        sleep(1);

        call_phd_AutoFindStar();
        call_phd_StartGuiding();

        // emit wsThread->sendMessageToClient("GuiderSwitchStatus:true");

        // call_phd_StarClick(641,363);
    }

    else if (parts[0].trimmed() == "GuiderExpTimeSwitch")
    {
        call_phd_setExposureTime(parts[1].toInt());
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
        glFocalLength  = parts[1].trimmed().toInt();
        // glCameraSize_width  = parts[2].trimmed().toDouble();
        // glCameraSize_height = parts[3].trimmed().toDouble();

        TelescopeControl_SolveSYNC();
    }

    else if (message == "ClearDataPoints")
    {
        // FWHM Data
        dataPoints.clear();
    }

    else if (message == "ShowAllImageFolder")
    {
        std::string allFile = GetAllFile();
        std::cout << allFile << std::endl;
        emit wsThread->sendMessageToClient("ShowAllImageFolder:" + QString::fromStdString(allFile));
        
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "MoveFileToUSB")
    {
        QStringList ImagePath= parseString(parts[1].trimmed().toStdString(),ImageSaveBasePath);
        RemoveImageToUsb(ImagePath);
        
    }
    else if (parts.size() == 2 && parts[0].trimmed() == "DeleteFile")
    {
        QStringList ImagePath= parseString(parts[1].trimmed().toStdString(),ImageSaveBasePath);
        DeleteImage(ImagePath);
    }
    else if (message == "USBCheck"){
        USBCheck();
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "SolveImage")
    {
        glFocalLength = parts[1].trimmed().toInt();
        // glCameraSize_width = parts[2].trimmed().toDouble();
        // glCameraSize_height = parts[3].trimmed().toDouble();

        // if(Tools::WaitForPlateSolveToComplete) {
        //     SolveImage(glFocalLength, glCameraSize_width, glCameraSize_height);
        // }
        // else {
        //     qDebug("Solve image in progress!!!");
        // }

        isSingleSolveImage = true;
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "startLoopSolveImage") {
        glFocalLength = parts[1].trimmed().toInt();

        isLoopSolveImage = true;
    }

    else if (message == "stopLoopSolveImage") {
        qDebug("Set isLoopSolveImage false");
        isLoopSolveImage = false;
    }

    else if (message == "StartLoopCapture") {
        qDebug("start Loop Capture");
        LoopCapture(glExpTime);
    }

    else if (message == "StopLoopCapture") {
        qDebug("stop Loop Capture");
        StopLoopCapture = true;
    }

    else if (message == "getStagingSolveResult") {
        RecoverySloveResul();
    }

    else if (message == "ClearSloveResultList") {
        ClearSloveResultList();
    }

    else if (message == "getOriginalImage") {
        saveFitsAsPNG(QString::fromStdString("/dev/shm/ccd_simulator.fits"), false);
    }

    else if (parts.size() == 3 && parts[0].trimmed() == "saveCurrentLocation")
    {
        glCurrentLocationLat = parts[1].trimmed().toDouble();
        glCurrentLocationLng = parts[2].trimmed().toDouble();
    }

    else if (message == "getCurrentLocation") {
        if(glCurrentLocationLat != 0 && glCurrentLocationLng != 0) {
            qDebug() << "CurrentLocation:" << glCurrentLocationLat << "," << glCurrentLocationLng;
            emit wsThread->sendMessageToClient("SetCurrentLocation:" + QString::number(glCurrentLocationLat) + ":" + QString::number(glCurrentLocationLng));
        }
    }

    else if (message == "getGPIOsStatus") {
        getGPIOsStatus();
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "SwitchOutPutPower")
    {
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

        emit wsThread->sendMessageToClient("OutPutPowerStatus:" + QString::number(index) + ":" + QString::number(value));
    }

    else if (parts.size() == 2 && parts[0].trimmed() == "SetBinning") {
        glMainCameraBinning = parts[1].trimmed().toInt();
        qDebug() << "Set Binning to " << glMainCameraBinning;
    }

    else if (parts.size() == 5 && parts[0].trimmed() == "GuiderCanvasClick")
    {
        int CanvasWidth = parts[1].trimmed().toInt();
        int CanvasHeight = parts[2].trimmed().toInt();
        int Click_X = parts[3].trimmed().toInt();
        int Click_Y = parts[4].trimmed().toInt();

        qDebug() << "GuiderCanvasClick:" << CanvasWidth << "," << CanvasHeight << "," << Click_X << "," << Click_Y;

        if (glPHD_CurrentImageSizeX != 0 && glPHD_CurrentImageSizeY != 0)
        {
            qDebug() << "PHD2ImageSize:" << glPHD_CurrentImageSizeX << "," << glPHD_CurrentImageSizeY;
            double ratioZoomX = (double) glPHD_CurrentImageSizeX / CanvasWidth;
            double ratioZoomY = (double) glPHD_CurrentImageSizeY / CanvasHeight;
            qDebug() << "ratioZoom:" << ratioZoomX << "," << ratioZoomY;
            double PHD2Click_X = (double) Click_X * ratioZoomX;
            double PHD2Click_Y = (double) Click_Y * ratioZoomY;
            qDebug() << "PHD2Click:" << PHD2Click_X << "," << PHD2Click_Y;
            call_phd_StarClick(PHD2Click_X, PHD2Click_Y);
        }
    }

    else if (message == "getQTClientVersion") {
        emit wsThread->sendMessageToClient("QTClientVersion:" + QString::fromUtf8(QT_Client_Version));
    }
}

void MainWindow::initINDIServer()
{
    system("pkill indiserver");
    system("rm -f /tmp/myFIFO");
    system("mkfifo /tmp/myFIFO");
    glIndiServer = new QProcess();
    glIndiServer->setReadChannel(QProcess::StandardOutput);
    glIndiServer->start("indiserver -f /tmp/myFIFO -v -p 7624");
}

void MainWindow::initINDIClient()
{
    indi_Client = new MyClient();
    indi_Client->setServer("localhost", 7624);
    indi_Client->setConnectionTimeout(3, 0);

    indi_Client->setImageReceivedCallback(
        [this](const std::string &filename, const std::string &devname)
        {
            //   responseIndiBlobImage(QString::fromStdString(filename), QString::fromStdString(devname));
            // CaptureTestTime = CaptureTestTimer.elapsed();
            // qDebug() << "\033[32m" << "Exposure completed:" << CaptureTestTime << "milliseconds" << "\033[0m";
            // CaptureTestTimer.invalidate();
            // 曝光完成
            if(dpMainCamera!=NULL){
                if(dpMainCamera->getDeviceName()==devname)
                {
                    if(glIsFocusingLooping == false)
                    {
                        saveFitsAsPNG(QString::fromStdString(filename), true);    // "/dev/shm/ccd_simulator.fits"
                    }
                    else
                    {
                        saveFitsAsJPG(QString::fromStdString(filename));
                    }
                    glMainCameraStatu = "Displaying";
                    ShootStatus = "Completed";
                }
            }
        }
    );

    indi_Client->setMessageReceivedCallback(
        [this](const std::string &message) {
            qDebug("[INDI SERVER] %s", message.c_str());

            std::regex regexPattern(R"(OnStep slew/syncError:\s*(.*))");
            std::smatch matchResult;

            if (std::regex_search(message, matchResult, regexPattern))
            {
                if (matchResult.size() > 1)
                {
                    QString errorContent = QString::fromStdString(matchResult[1].str());
                    qDebug() << "\033[31m" << "OnStep Error: " << errorContent << "\033[0m";
                    emit wsThread->sendMessageToClient("OnStep Error:" + errorContent);
                    MountGotoError = true;
                }
            }
        }
    );
}

void MainWindow::initGPIO() {
    // 初始化 GPIO_PIN_1
    // exportGPIO(GPIO_PIN_1);
    setGPIODirection(GPIO_PIN_1, "out");

    // 初始化 GPIO_PIN_2
    // exportGPIO(GPIO_PIN_2);
    setGPIODirection(GPIO_PIN_2, "out");

    // 设置 GPIO_PIN_1 为高电平
    setGPIOValue(GPIO_PIN_1, "1");

    // 设置 GPIO_PIN_2 为高电平
    setGPIOValue(GPIO_PIN_2, "1");
}

void MainWindow::exportGPIO(const char* pin) {
    int fd;
    char buf[64];

    // 导出 GPIO 引脚
    fd = open(GPIO_EXPORT, O_WRONLY);
    if (fd < 0) {
        perror("Failed to open export for writing");
        return;
    }
    snprintf(buf, sizeof(buf), "%s", pin);
    if (write(fd, buf, strlen(buf)) != strlen(buf)) {
        perror("Failed to write to export");
        close(fd);
        return;
    }
    close(fd);
}

void MainWindow::setGPIODirection(const char* pin, const char* direction) {
    int fd;
    char path[128];

    // 设置 GPIO 方向
    snprintf(path, sizeof(path), GPIO_PATH "/gpio%s/direction", pin);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("Failed to open gpio direction for writing");
        return;
    }
    if (write(fd, direction, strlen(direction) + 1) != strlen(direction) + 1) {
        perror("Failed to set gpio direction");
        close(fd);
        return;
    }
    close(fd);
}

void MainWindow::setGPIOValue(const char* pin, const char* value) {
    int fd;
    char path[128];

    // 设置 GPIO 电平
    snprintf(path, sizeof(path), GPIO_PATH "/gpio%s/value", pin);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        perror("Failed to open gpio value for writing");
        return;
    }
    if (write(fd, value, strlen(value) + 1) != strlen(value) + 1) {
        perror("Failed to write to gpio value");
        close(fd);
        return;
    }
    close(fd);
}

int MainWindow::readGPIOValue(const char* pin) {
    int fd;
    char path[128];
    char value[3];  // 存储读取的值

    // 构建路径 /sys/class/gpio/gpio[PIN]/value
    snprintf(path, sizeof(path), GPIO_PATH "/gpio%s/value", pin);

    // 打开 GPIO value 文件进行读取
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open gpio value for reading");
        return -1;  // 返回 -1 表示读取失败
    }

    // 读取文件内容
    if (read(fd, value, sizeof(value)) < 0) {
        perror("Failed to read gpio value");
        close(fd);
        return -1;  // 返回 -1 表示读取失败
    }

    close(fd);

    // 判断读取到的值是否为 '1' 或 '0'
    if (value[0] == '1') {
        return 1;  // 返回 1 表示高电平
    } else if (value[0] == '0') {
        return 0;  // 返回 0 表示低电平
    } else {
        return -1;  // 如果读取的值不是 '0' 或 '1'，返回 -1
    }
}

void MainWindow::getGPIOsStatus() {
    int value1 = readGPIOValue(GPIO_PIN_1);
    emit wsThread->sendMessageToClient("OutPutPowerStatus:" + QString::number(1) + ":" + QString::number(value1));
    int value2 = readGPIOValue(GPIO_PIN_2);
    emit wsThread->sendMessageToClient("OutPutPowerStatus:" + QString::number(2) + ":" + QString::number(value2));
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
                qDebug() << "TelescopePierSide: " << TelescopePierSide;
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
        }
    }

    if (isLoopSolveImage)
    {
        if (Tools::WaitForPlateSolveToComplete() && !TakeNewCapture)
        {
            qDebug() << "isLoopSolveImage:" << isLoopSolveImage;

            TakeNewCapture = true;

            QString SolveImageFileName = "/dev/shm/SolveImage.tiff";

            LoopSolveImage(SolveImageFileName, glFocalLength, glCameraSize_width, glCameraSize_height);
        }
    }

    if (isSingleSolveImage)
    {
        if (Tools::WaitForPlateSolveToComplete())
        {
            isSingleSolveImage = false;

            QString SolveImageFileName = "/dev/shm/SolveImage.tiff";

            CaptureTestTimer.start();
            qDebug() << "\033[32m" << "Solve image start." << "\033[0m";
            SolveImage(SolveImageFileName, glFocalLength, glCameraSize_width, glCameraSize_height);
            CaptureTestTime = CaptureTestTimer.elapsed();
            qDebug() << "\033[32m" << "Solve image completed:" << CaptureTestTime << "milliseconds" << "\033[0m";
            CaptureTestTimer.invalidate();
        }
    }
}

void MainWindow::saveFitsAsJPG(QString filename)
{
    cv::Mat image;
    cv::Mat image16;
    cv::Mat SendImage;
    Tools::readFits(filename.toLocal8Bit().constData(), image);

    QList<FITSImage::Star> stars = Tools::FindStarsByStellarSolver(true, true);

    if(stars.size() != 0){
        FWHM = stars[0].HFR;
    }
    else {
        FWHM = -1;
    }   
    

    if(image16.depth()==8) image.convertTo(image16,CV_16UC1,256,0); //x256  MSB alignment
    else                   image.convertTo(image16,CV_16UC1,1,0);

    if(FWHM != -1){
        // 在原图上绘制检测结果
        cv::Point center(stars[0].x, stars[0].y);
        cv::circle(image16, center, static_cast<int>(FWHM), cv::Scalar(0, 0, 255), 1); // 绘制HFR圆
        cv::circle(image16, center, 1, cv::Scalar(0, 255, 0), -1);                     // 绘制中心点
        // 在图像上显示HFR数值
        std::string hfrText = cv::format("%.2f", stars[0].HFR);
        cv::putText(image16, hfrText, cv::Point(stars[0].x - FWHM, stars[0].y - FWHM - 5), cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(0, 0, 255), 1);
    }

    cv::Mat NewImage = image16;

    FWHMCalOver = true;

    // 将图像缩放到0-255范围内
    // cv::normalize(image, SendImage, 0, 255, cv::NORM_MINMAX, CV_8U);    // 原图
    cv::normalize(NewImage, SendImage, 0, 255, cv::NORM_MINMAX, CV_8U);    // New

    // 生成唯一ID
    QString uniqueId = QUuid::createUuid().toString();

    // 列出所有以"CaptureImage"为前缀的文件
    QDir directory(QString::fromStdString(vueDirectoryPath));
    QStringList filters;
    filters << "CaptureImage*.jpg"; // 使用通配符来筛选以"CaptureImage"为前缀的jpg文件
    QStringList fileList = directory.entryList(filters, QDir::Files);

    // 删除所有匹配的文件
    for (const auto &file : fileList)
    {
        QString filePath = QString::fromStdString(vueDirectoryPath) + file;
        QFile::remove(filePath);
    }

    // 删除前一张图像文件
    if (PriorROIImage != "NULL") {
        QFile::remove(QString::fromStdString(PriorROIImage));
    }

    // 保存新的图像带有唯一ID的文件名
    std::string fileName = "CaptureImage_" + uniqueId.toStdString() + ".jpg";
    std::string filePath = vueDirectoryPath + fileName;

    bool saved = cv::imwrite(filePath, SendImage);

    std::string Command = "ln -sf " + filePath + " " + vueImagePath + fileName;
    system(Command.c_str());

    PriorROIImage = vueImagePath + fileName;

    if (saved)
    {
        emit wsThread->sendMessageToClient("SaveJpgSuccess:" + QString::fromStdString(fileName));

        if(FWHM != -1){
            dataPoints.append(QPointF(CurrentPosition, FWHM));

            qDebug() << "dataPoints:" << CurrentPosition << "," << FWHM;

            float a, b, c;
            Tools::fitQuadraticCurve(dataPoints, a, b, c);

            if (dataPoints.size() >= 5) {
                QVector<QPointF> LineData;

                for (float x = CurrentPosition - 3000; x <= CurrentPosition + 3000; x += 10)
                {
                    float y = a * x * x + b * x + c;
                    LineData.append(QPointF(x, y));
                }

                // 计算导数为零的 x 坐标
                float x_min = -b / (2 * a);
                minPoint_X = x_min;
                // 计算最小值点的 y 坐标
                float y_min = a * x_min * x_min + b * x_min + c;

                QString dataString;
                for (const auto &point : LineData)
                {
                    dataString += QString::number(point.x()) + "|" + QString::number(point.y()) + ":";
                }

                R2 = Tools::calculateRSquared(dataPoints, a, b, c);
                qDebug() << "RSquared: " << R2;

                emit wsThread->sendMessageToClient("fitQuadraticCurve:" + dataString);
                emit wsThread->sendMessageToClient("fitQuadraticCurve_minPoint:" + QString::number(x_min) + ":" + QString::number(y_min));
            }    
        }
    }
    else
    {
        qDebug() << "Save Image Failed...";
    }
}

int MainWindow::saveFitsAsPNG(QString fitsFileName, bool ProcessBin)
{
    cv::Mat image;
    cv::Mat OriginImage;
    int status = Tools::readFits(fitsFileName.toLocal8Bit().constData(), image);

    if (status != 0)
    {
        qDebug() << "Failed to read FITS file: " << fitsFileName;
        return status;
    }

    bool isColor = !(MainCameraCFA == "");

    // if(ProcessBin && !isFirstCapture && glMainCameraBinning != 1){
    if(ProcessBin && glMainCameraBinning != 1){
        // 进行 2x2 的合并
        image = Tools::processMatWithBinAvg(image, glMainCameraBinning, glMainCameraBinning, isColor, false);
        qDebug() << "After bin image size:" << image.cols << "," << image.rows;
    }

    // isFirstCapture = false;

    cv::Mat srcImage = image.clone();
    cv::Mat dstImage;
    qDebug("start medianBlur.");
    cv::medianBlur(srcImage, dstImage, 3);
    qDebug("medianBlur success.");
    QString SolveImageFileName = "/dev/shm/SolveImage.tiff";
    cv::imwrite(SolveImageFileName.toStdString(), dstImage); // TODO:
    qDebug("image save success.");
    
    Tools::SaveMatTo8BitJPG(image);
    // Tools::SaveMatTo16BitPNG(image);
    // Tools::SaveMatToFITS(image);
    
    int width = image.cols;
    int height = image.rows;
    qDebug() << "image size:" << width << "," << height;
    emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(width) + ":" + QString::number(height));

    std::vector<unsigned char> imageData;  //uint16_t
    imageData.assign(image.data, image.data + image.total() * image.channels() * 2);
    qDebug() << "imageData Size:" << imageData.size() << "," << image.data + image.total() * image.channels();
    
    QString uniqueId = QUuid::createUuid().toString();
    
    QDir directory(QString::fromStdString(vueDirectoryPath));
    QStringList filters;
    filters << "CaptureImage*.bin";
    QStringList fileList = directory.entryList(filters, QDir::Files);
    
    for (const auto &file : fileList)
    {
        QString filePath = QString::fromStdString(vueDirectoryPath) + file;
        QFile::remove(filePath);
    }

    // 删除前一张图像文件
    if (PriorCaptureImage != "NULL") {
        QFile::remove(QString::fromStdString(PriorCaptureImage));
    }
    
    std::string fileName_ = "CaptureImage_" + uniqueId.toStdString() + ".bin";
    std::string filePath_ = vueDirectoryPath + fileName_;
    
    qDebug("Open file for writing.");
    std::ofstream outFile(filePath_, std::ios::binary);
    if (!outFile) {
        throw std::runtime_error("Failed to open file for writing.");
    }

    qDebug("Write data to file.");
    outFile.write(reinterpret_cast<const char*>(imageData.data()), imageData.size());
    if (!outFile) {
        throw std::runtime_error("Failed to write data to file.");
    }

    outFile.close();
    if (!outFile) {
        throw std::runtime_error("Failed to close the file properly.");
    }

    std::string Command = "ln -sf " + filePath_ + " " + vueImagePath + fileName_;
    system(Command.c_str());

    PriorCaptureImage = vueImagePath + fileName_;

    emit wsThread->sendMessageToClient("SaveBinSuccess:" + QString::fromStdString(fileName_));
    isStagingImage = true;
    SavedImage = QString::fromStdString(fileName_);

    QList<FITSImage::Star> stars = Tools::FindStarsByStellarSolver(false, true);

    QString dataString;
    for (const auto &star : stars)
    {
        dataString += QString::number(star.x) + "|" + QString::number(star.y) + "|" + QString::number(star.HFR) + ":";
    }
    emit wsThread->sendMessageToClient("DetectedStars:" + dataString);
}

cv::Mat MainWindow::colorImage(cv::Mat img16)
{
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

    Tools::ImageSoftAWB(img16, AWBImg16, MainCameraCFA, ImageGainR, ImageGainB, 30); // image software Auto White Balance is done in RAW image.
    cv::cvtColor(AWBImg16, AWBImg16color, CV_BayerRG2BGR);

    cv::cvtColor(AWBImg16color, AWBImg16mono, cv::COLOR_BGR2GRAY);

    // cv::cvtColor(AWBImg16, AWBImg16color, CV_BayerRG2RGB);

    // cv::cvtColor(AWBImg16color, AWBImg16mono, cv::COLOR_RGB2GRAY);

    if (AutoStretch == true)
    {
        Tools::GetAutoStretch(AWBImg16mono, 0, B, W);
    }
    else
    {
        B = 0;
        W = 65535;
    }
    qDebug() << "GetAutoStretch:" << B << "," << W;
    Tools::Bit16To8_Stretch(AWBImg16color, AWBImg8color, B, W);

    return AWBImg16color;

    AWBImg16.release();
    AWBImg16color.release();
    AWBImg16mono.release();
    AWBImg8color.release();
}

void MainWindow::saveGuiderImageAsJPG(cv::Mat Image)
{
    // 生成唯一ID
    QString uniqueId = QUuid::createUuid().toString();

    // 列出所有以"CaptureImage"为前缀的文件
    QDir directory(QString::fromStdString(vueDirectoryPath));
    QStringList filters;
    filters << "GuiderImage*.jpg"; // 使用通配符来筛选以"CaptureImage"为前缀的jpg文件
    QStringList fileList = directory.entryList(filters, QDir::Files);

    // 删除所有匹配的文件
    for (const auto &file : fileList)
    {
        QString filePath = QString::fromStdString(vueDirectoryPath) + file;
        QFile::remove(filePath);
    }

    // 删除前一张图像文件
    if (PriorGuiderImage != "NULL") {
        QFile::remove(QString::fromStdString(PriorGuiderImage));
    }

    // 保存新的图像带有唯一ID的文件名
    std::string fileName = "GuiderImage_" + uniqueId.toStdString() + ".jpg";
    std::string filePath = vueDirectoryPath + fileName;

    bool saved = cv::imwrite(filePath, Image);

    std::string Command = "ln -sf " + filePath + " " + vueImagePath + fileName;
    system(Command.c_str());

    PriorGuiderImage = vueImagePath + fileName;

    if (saved)
    {
        emit wsThread->sendMessageToClient("SaveGuiderImageSuccess:" + QString::fromStdString(fileName));
    }
    else
    {
        qDebug() << "Save GuiderImage Failed...";
    }
}

// QString MainWindow::connectIndiServer()
// {
//     indi_Client->setConnectionTimeout(3, 0);
//     indi_Client->ClearDevices(); // clear device list
//     indi_Client->connectServer();
//     qDebug("--------------------------------------------------connectServer");
//     sleep(1);
//     QString devname = indi_Client->PrintDevices();
//     return devname;
// }

// void MainWindow::disconnectIndiServer()
// {
//     indi_Client->disconnectAllDevice();
//     indi_Client->ClearDevices();
//     indi_Client->disconnectServer();
//     while (indi_Client->isServerConnected() == true)
//     {
//         qDebug("wait for client disconnected");
//     }
//     qDebug("--------------------------------------------------disconnectServer");
//     indi_Client->PrintDevices();
//     Tools::printSystemDeviceList(systemdevicelist);
// }

void MainWindow::readDriversListFromFiles(const std::string &filename, DriversList &drivers_list_from,
                                          std::vector<DevGroup> &dev_groups_from, std::vector<Device> &devices_from)
{
    QFile file(QString::fromStdString(filename));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        qDebug() << "Open File Faild";
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
        qDebug() << "Unable to find INDI drivers directory,Please make sure the path is true";
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (strcmp(entry->d_name + strlen(entry->d_name) - 4, ".xml") == 0)
        {
            if (strcmp(entry->d_name + strlen(entry->d_name) - 6, "sk.xml") == 0)
            {
                continue;
            }
            else
            {
                xmlpath = DirPath + entry->d_name;
                QFile file(QString::fromStdString(xmlpath));
                if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                {
                    qDebug() << "Open File faild!!!";
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
}

//"Telescopes"|"Focusers"|"CCDs"|"Spectrographs"|"Filter Wheels"|"Auxiliary"|"Domes"|"Weather"|"Agent"
void MainWindow::printDevGroups2(const DriversList drivers_list, int ListNum, QString group)
{
    qDebug("=============================== Print DevGroups ===============================");
    for (int i = 0; i < drivers_list.dev_groups.size(); i++)
    {
        if (drivers_list.dev_groups[i].group == group)
        {
            qDebug() << drivers_list.dev_groups[i].group;
            // for (int j = 0; j < drivers_list.dev_groups[i].devices.size(); j++)
            // {
            //     qDebug() << QString::fromStdString(drivers_list.dev_groups[i].devices[j].driver_name) << QString::fromStdString(drivers_list.dev_groups[i].devices[j].version) << QString::fromStdString(drivers_list.dev_groups[i].devices[j].label);
            //     websocket->messageSend("AddDriver:"+QString::fromStdString(drivers_list.dev_groups[i].devices[j].label)+":"+QString::fromStdString(drivers_list.dev_groups[i].devices[j].driver_name));
            // }
            DeviceSelect(ListNum, i);
        }
    }
}

void MainWindow::DeviceSelect(int systemNumber, int grounpNumber)
{
    Tools::clearSystemDeviceListItem(systemdevicelist, systemNumber);
    SelectIndiDevice(systemNumber, grounpNumber);
}

void MainWindow::SelectIndiDevice(int systemNumber, int grounpNumber)
{
    systemdevicelist.currentDeviceCode = systemNumber;
    drivers_list.selectedGrounp = grounpNumber;

    switch (systemNumber)
    {
    case 0:
        systemdevicelist.system_devices[systemNumber].Description = "Mount";
        break;
    case 1:
        systemdevicelist.system_devices[systemNumber].Description = "Guider";
        break;
    case 2:
        systemdevicelist.system_devices[systemNumber].Description = "PoleCamera";
        break;
    case 3:
        systemdevicelist.system_devices[systemNumber].Description = "";
        break;
    case 4:
        systemdevicelist.system_devices[systemNumber].Description = "";
        break;
    case 5:
        systemdevicelist.system_devices[systemNumber].Description = "";
        break;
    case 20:
        systemdevicelist.system_devices[systemNumber].Description = "Main Camera #1";
        break;
    case 21:
        systemdevicelist.system_devices[systemNumber].Description = "CFW #1";
        break;
    case 22:
        systemdevicelist.system_devices[systemNumber].Description = "Focuser #1";
        break;
    case 23:
        systemdevicelist.system_devices[systemNumber].Description = "Lens Cover #1";
        break;

    default:
        break;
    }

    qDebug() << "SelectIndiDevice:" << systemdevicelist.currentDeviceCode << "," << drivers_list.selectedGrounp;

    for (int i = 0; i < drivers_list.dev_groups[grounpNumber].devices.size(); i++)
    {
        // ComboBox->addItem(QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].label));
        qDebug() << QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].driver_name) << QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].version) << QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].label);
        // websocket->messageSend("AddDriver:"+QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].label)+":"+QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].driver_name));
        emit wsThread->sendMessageToClient("AddDriver:" + QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].label) + ":" + QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].driver_name));
    }
}

bool MainWindow::indi_Driver_Confirm(QString DriverName)
{
    bool isExist;
    qDebug() << "call clearCheckDeviceExist:" << DriverName;
    uint32_t ret = clearCheckDeviceExist(DriverName, isExist);

    if (isExist == false)
    {
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].DeviceIndiGroup = -1;
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].DeviceIndiName = "";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].DriverFrom = "";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].DriverIndiName = "";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].isConnect = false;
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].dp = NULL;
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "";
    }

    qDebug() << "\033[0m\033[1;35m"
             << "on_SystemPageComboBoxIndiDriver_currentIndexChanged Exist:" << isExist << "ret=" << ret << "\033[0m";

    return isExist;
}

void MainWindow::indi_Device_Confirm(QString DeviceName, QString DriverName)
{
    //   qApp->processEvents();

    int deviceCode;
    deviceCode = systemdevicelist.currentDeviceCode;

    systemdevicelist.system_devices[deviceCode].DriverIndiName = DriverName;
    systemdevicelist.system_devices[deviceCode].DeviceIndiGroup = drivers_list.selectedGrounp;
    systemdevicelist.system_devices[deviceCode].DeviceIndiName = DeviceName;

    qDebug() << "\033[0m\033[1;35m"
             << "system device successfully selected"
             << "\033[0m";
    Tools::printSystemDeviceList(systemdevicelist);

    Tools::saveSystemDeviceList(systemdevicelist);
}

uint32_t MainWindow::clearCheckDeviceExist(QString drivername, bool &isExist)
{
    int deviceCode;
    one_touch_connect = false;
    deviceCode = drivers_list.selectedGrounp;
    qDebug() << "clearCheckDeviceExist | device code" << deviceCode;

    Tools::stopIndiDriverAll(drivers_list);
    Tools::startIndiDriver(drivername);

    sleep(1); // must wait some time here

    // websocket->messageSend("sleep(3)");

    MyClient *searchClient;
    searchClient = new MyClient();
    searchClient->PrintDevices();

    searchClient->setServer("localhost", 7624);
    searchClient->setConnectionTimeout(3, 0);
    searchClient->ClearDevices(); // clear device list

    bool connected = searchClient->connectServer();

    if (connected == false)
    {
        qDebug() << "clearCheckDeviceExist | ERROR:can not find server";
        return QHYCCD_ERROR;
    }

    sleep(1); // connect server will generate the callback of newDevice and then put the device into list. this need take some time and it is non-block
    searchClient->PrintDevices();
    qDebug() << "clearCheckDeviceExist | total device:" << searchClient->GetDeviceCount();

    if (searchClient->GetDeviceCount() == 0)
    {
        searchClient->disconnectServer();
        isExist = false;
        emit wsThread->sendMessageToClient("ScanFailed:No device found.");
        return QHYCCD_SUCCESS;
    }

    QVector<INDI::BaseDevice *> dp;

    // INDI::BaseDevice *dp;

    for (int i = 0; i < searchClient->GetDeviceCount(); i++)
    {
        dp.append(searchClient->GetDeviceFromList(i));
        searchClient->connectDevice(dp[i]->getDeviceName());
    }

    QElapsedTimer t;
    t.start();

    int timeout_ms = 20000;
    int timeout_total = timeout_ms * searchClient->GetDeviceCount();
    int counter_connected;

    while (t.elapsed() < timeout_total)
    {
        sleep(1);
        // qApp->processEvents();

        counter_connected = 0;
        for (int i = 0; i < searchClient->GetDeviceCount(); i++)
        {
            if (dp[i]->isConnected() == true)
                counter_connected++;

            qDebug() << "?" << counter_connected;
        }

        if (counter_connected == searchClient->GetDeviceCount())
        {
            break;
        }
    }

    if (t.elapsed() > timeout_total)
    {
        qDebug() << "clearCheckDeviceExist | ERROR: Connect time exceed (ms):" << timeout_ms;
        isExist = false;
        searchClient->disconnectServer();
        Tools::stopIndiDriver(drivername);
        return QHYCCD_SUCCESS;
    }
    else
    {
        isExist = true;
    }

    if (isExist == true)
    {
        for (int i = 0; i < searchClient->GetDeviceCount(); i++)
        {
            qDebug() << "clearCheckDeviceExist | Exist |" << dp[i]->getDeviceName();

            qDebug() << "clearCheckDeviceExist | disconnect Device" << dp[i]->getDeviceName();
            searchClient->disconnectDevice(dp[i]->getDeviceName());
            // websocket->messageSend("AddDevice:"+QString::fromStdString(dp[i]->getDeviceName()));
            emit wsThread->sendMessageToClient("AddDevice:" + QString::fromStdString(dp[i]->getDeviceName()));
        }
    }

    // succssed find driver, define system main camera driver type
    systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].DriverIndiName = drivername;
    systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].DriverFrom = "INDI";
    systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].DeviceIndiGroup = drivers_list.selectedGrounp;
    qDebug() << "clearCheckDeviceExist |" << systemdevicelist.currentDeviceCode << drivername << "INDI";

    qDebug() << "clearCheckDeviceExist | disconnect Server";
    searchClient->disconnectServer();
    searchClient->ClearDevices();

    Tools::stopIndiDriver(drivername);

    return QHYCCD_SUCCESS;
}

void MainWindow::DeviceConnect()
{
    uint32_t ret;
    if (one_touch_connect == true)
    {
        if (one_touch_connect_first == true)
        {
            systemdevicelist = Tools::readSystemDeviceList();
            for (int i = 0; i < 32; i++)
            {
                if (systemdevicelist.system_devices[i].DeviceIndiName != "")
                {
                    qDebug() << i << systemdevicelist.system_devices[i].DeviceIndiName;
                    int ListNum = i;
                    emit wsThread->sendMessageToClient("updateDevices_:" + QString::number(i) + ":" + systemdevicelist.system_devices[i].DeviceIndiName);
                }
            }
            one_touch_connect_first = false;
            return;
        }
    }

    if (Tools::getTotalDeviceFromSystemDeviceList(systemdevicelist) == 0)
    {
        qDebug() << "System Connect | Error: no device in system device list";
        emit wsThread->sendMessageToClient("ConnectFailed:no device in system device list.");
        return;
    }
    // systemdevicelist
    Tools::cleanSystemDeviceListConnect(systemdevicelist);
    Tools::printSystemDeviceList(systemdevicelist);

    // qApp->processEvents();
    // connect all camera on the list
    QString driverName;

    QVector<QString> nameCheck;
    disconnectIndiServer(indi_Client);

    Tools::stopIndiDriverAll(drivers_list);
    int k = 3;
    while (k--)
    {
        qDebug("wait stopIndiDriverAll...");
        sleep(1);
        // qApp->processEvents();
    }

    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        driverName = systemdevicelist.system_devices[i].DriverIndiName;
        // qDebug() << "Before Start INDI Driver | DriverName: " << driverName;
        if (driverName != "")
        {
            bool isFound = false;
            for (auto item : nameCheck)
            {
                if (item == driverName)
                {
                    isFound = true;
                    qDebug() << "System Connect | found one duplite driver,do not start it again" << driverName;
                    break;
                }
            }

            if (isFound == false)
            {
                Tools::startIndiDriver(driverName);
                int k = 3;
                while (k--)
                {
                    qDebug("wait startIndiDriver...");
                    sleep(1);
                    // qApp->processEvents();
                }
                nameCheck.push_back(driverName);
            }
        }
    }

    connectIndiServer(indi_Client);

    if (indi_Client->isServerConnected() == false)
    {
        qDebug() << "System Connect | ERROR:can not find server";
        return;
    }

    sleep(3); // connect server will generate the callback of newDevice and then put the device into list. this need take some time and it is non-block

    // wait the client device list's device number match the system device list's device number
    int totalDevice = Tools::getTotalDeviceFromSystemDeviceList(systemdevicelist);
    QElapsedTimer t;
    int timeout_ms = 10000;
    t.start();
    while (t.elapsed() < timeout_ms)
    {
        if (indi_Client->GetDeviceCount() >= totalDevice)
            break;
        QThread::msleep(300);
        // qApp->processEvents();
        qDebug() << "indi client found device:" << indi_Client->GetDeviceCount() << ", total device:" << totalDevice;
    }
    if (t.elapsed() > timeout_ms){
        qDebug() << "System Connect | INDI connectServer | ERROR: timeout :device connected less than system device list";
        emit wsThread->sendMessageToClient("ConnectFailed:Device connected less than system device list.");
    }
    else
        qDebug() << "System Connect | Success, used time(ms):" << t.elapsed();
    indi_Client->PrintDevices();

    if (indi_Client->GetDeviceCount() == 0)
    {
        qDebug() << "System Connect | Error:No device found";
        emit wsThread->sendMessageToClient("ConnectFailed:No device found.");
        return;
    }
    qDebug() << "System Connect | 1";
    int index;
    int total_errors = 0;

    for (int i = 0; i < indi_Client->GetDeviceCount(); i++)
    {
        qDebug() << "System Connect | 2 loop 1" << i;
        qDebug() << QString::fromStdString(indi_Client->GetDeviceNameFromList(i));
        // take one device from indi_Client detected devices and get the index number in pre-selected systemdevicelist.
        ret = Tools::getIndexFromSystemDeviceList(systemdevicelist, QString::fromStdString(indi_Client->GetDeviceNameFromList(i)), index);
        if (ret == QHYCCD_SUCCESS)
        {

            qDebug() << "System Connect | 2 loop 2" << i << index << ret;
            systemdevicelist.system_devices[index].dp = indi_Client->GetDeviceFromList(i);

            qDebug() << "System Connect |" << index << QString::fromStdString(indi_Client->GetDeviceNameFromList(i))
                     << "device DriverVersion" << systemdevicelist.system_devices[index].dp->getDriverVersion()
                     << "DriverInterface" << systemdevicelist.system_devices[index].dp->getDriverInterface()
                     << "DriverName" << systemdevicelist.system_devices[index].dp->getDriverName()
                     << "DeviceName" << systemdevicelist.system_devices[index].dp->getDeviceName();

            systemdevicelist.system_devices[index].isConnect = false; // clean the status before connect
            if (index == 1)
            {
                call_phd_whichCamera(systemdevicelist.system_devices[index].dp->getDeviceName());  // PHD2 Guider Connect
            }
            else
            {
                indi_Client->connectDevice(systemdevicelist.system_devices[index].dp->getDeviceName());
            }
            // guider will be control by PHD2, so that the watch device should exclude the guider
            // indi_Client->StartWatch(systemdevicelist.system_devices[index].dp);
        }
        else
        {
            total_errors++;
        }
    }
    if (total_errors > 0)
    {
        qDebug() << "System Connect |Error: There is some detected list is not in the pre-select system list, total mismatch device:" << total_errors;
        // return;
    }

    // connecting.....
    // QElapsedTimer t;
    t.start();
    timeout_ms = 20000 * indi_Client->GetDeviceCount();
    while (t.elapsed() < timeout_ms)
    {
        QThread::msleep(300);
        int totalConnected = 0;
        for (int i = 0; i < indi_Client->GetDeviceCount(); i++)
        {
            ret = Tools::getIndexFromSystemDeviceList(systemdevicelist, QString::fromStdString(indi_Client->GetDeviceNameFromList(i)), index);
            if (ret == QHYCCD_SUCCESS)
            {
                if (systemdevicelist.system_devices[index].dp->isConnected() == true)
                {
                    systemdevicelist.system_devices[index].isConnect = true;
                    totalConnected++;
                }
            }
            else
            {
                qDebug() << "System Connect |Warn:" << QString::fromStdString(indi_Client->GetDeviceNameFromList(i)) << "is found in the client list but not in pre-select system list" << i;
            }
        }

        if (totalConnected >= indi_Client->GetDeviceCount())
            break;
        // qApp->processEvents();
    }

    if (t.elapsed() > timeout_ms){
        qDebug() << "System Connect | ERROR: Connect time exceed (ms):" << timeout_ms << t.elapsed();
        emit wsThread->sendMessageToClient("ConnectFailed:Device connected timeout.");
    }
    else
        qDebug() << "System Connect | Success, used time(ms):" << t.elapsed();

    if (systemdevicelist.system_devices[0].isConnect == true)
        dpMount = systemdevicelist.system_devices[0].dp;
    if (systemdevicelist.system_devices[1].isConnect == true)
        dpGuider = systemdevicelist.system_devices[1].dp;
    if (systemdevicelist.system_devices[2].isConnect == true)
        dpPoleScope = systemdevicelist.system_devices[2].dp;
    if (systemdevicelist.system_devices[20].isConnect == true)
        dpMainCamera = systemdevicelist.system_devices[20].dp;
    if (systemdevicelist.system_devices[21].isConnect == true)
        dpCFW = systemdevicelist.system_devices[21].dp;
    if (systemdevicelist.system_devices[22].isConnect == true)
        dpFocuser = systemdevicelist.system_devices[22].dp;

    // printSystemDeviceList(systemdevicelist);
    AfterDeviceConnect();
}

void MainWindow::AfterDeviceConnect()
{
    if (dpMainCamera != NULL)
    {
        qDebug() << "AfterAllConnected | DeviceName: " << dpMainCamera->getDeviceName();
        emit wsThread->sendMessageToClient("ConnectSuccess:MainCamera:" + QString::fromUtf8(dpMainCamera->getDeviceName()));
        ConnectedDevices.push_back({"MainCamera", QString::fromUtf8(dpMainCamera->getDeviceName())});

        indi_Client->setBLOBMode(B_ALSO, dpMainCamera->getDeviceName(), nullptr);
        indi_Client->enableDirectBlobAccess(dpMainCamera->getDeviceName(), nullptr);

        QString SDKVERSION;
        indi_Client->getCCDSDKVersion(dpMainCamera, SDKVERSION);
        qDebug() << "AfterAllConnected | MainCamera SDK version:" << SDKVERSION;

        int X, Y;
        indi_Client->getCCDFrameInfo(dpMainCamera, X, Y, glMainCCDSizeX, glMainCCDSizeY);
        qDebug() << "CCDSize:" << glMainCCDSizeX << glMainCCDSizeY;
        emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));

        indi_Client->getCCDOffset(dpMainCamera, glOffsetValue, glOffsetMin, glOffsetMax);
        qDebug() << "CCD Offset(Value,Min,Max):" << glOffsetValue << "," << glOffsetMin << "," << glOffsetMax;
        emit wsThread->sendMessageToClient("MainCameraOffsetRange:" + QString::number(glOffsetMin) + ":" + QString::number(glOffsetMax));

        indi_Client->getCCDGain(dpMainCamera, glGainValue, glGainMin, glGainMax);
        qDebug() << "CCD Gain(Value,Min,Max):" << glGainValue << "," << glGainMin << "," << glGainMax;
        emit wsThread->sendMessageToClient("MainCameraGainRange:" + QString::number(glGainMin) + ":" + QString::number(glGainMax));

        int maxX, maxY; 
        double pixelsize, pixelsizX, pixelsizY;
        int bitDepth;
        indi_Client->getCCDBasicInfo(dpMainCamera, maxX, maxY,  pixelsize, pixelsizX, pixelsizY, bitDepth);
        qDebug() << "CCD Basic Info:" << maxX << "," << maxY << "," << pixelsize << "," << pixelsizX << "," << pixelsizY << "," << bitDepth;

        glCameraSize_width = maxX * pixelsize / 1000;
        glCameraSize_width = std::round(glCameraSize_width * 10) / 10;
        glCameraSize_height = maxY * pixelsize / 1000;
        glCameraSize_height = std::round(glCameraSize_height * 10) / 10;
        qDebug() << "CCD Chip size:" << glCameraSize_width << "," << glCameraSize_height;

        int offsetX, offsetY;
        indi_Client->getCCDCFA(dpMainCamera, offsetX, offsetY, MainCameraCFA);
        qDebug() << "getCCDCFA:" << MainCameraCFA << offsetX << offsetY;
        // indi_Client->setTemperature(dpMainCamera, -10);

        indi_Client->setCCDUploadModeToLacal(dpMainCamera);
        indi_Client->setCCDUpload(dpMainCamera, "/dev/shm", "ccd_simulator");

        QString CFWname;
        indi_Client->getCFWSlotName(dpMainCamera, CFWname);
        if(CFWname != "") {
            qDebug() << "get CFW Slot Name: " << CFWname;
            emit wsThread->sendMessageToClient("ConnectSuccess:CFW:" + CFWname+" (on camera)");
            isFilterOnCamera = true;

            int min, max, pos;
            indi_Client->getCFWPosition(dpMainCamera, pos, min, max);
            qDebug() << "getCFWPosition: " << min << ", " << max << ", " << pos;
            emit wsThread->sendMessageToClient("CFWPositionMax:" + QString::number(max));
        }
    }

    if (dpMount != NULL)
    {
        emit wsThread->sendMessageToClient("ConnectSuccess:Mount:" + QString::fromUtf8(dpMount->getDeviceName()));
        ConnectedDevices.push_back({"Mount", QString::fromUtf8(dpMount->getDeviceName())});
        QString DevicePort; // add by CJQ 2023.3.3
        indi_Client->getDevicePort(dpMount, DevicePort);

        //????
        double glLongitude_radian, glLatitude_radian;
        glLongitude_radian = Tools::getDecAngle("116° 14' 53.91");      //TODO:
        glLatitude_radian = Tools::getDecAngle("40° 09' 14.93");
        //????

        indi_Client->setLocation(dpMount, Tools::RadToDegree(glLatitude_radian), Tools::RadToDegree(glLongitude_radian), 10);
        QDateTime datetime = QDateTime::currentDateTime();
        // datetime= QDateTime::fromString("2023-12-29T12:34:56+00:00",Qt::ISODate);
        // qDebug()<<datetime;
        indi_Client->setTimeUTC(dpMount, datetime);
        qDebug() << "AfterAllConnected_setTimeUTC |" << datetime;
        indi_Client->getTimeUTC(dpMount, datetime);
        qDebug() << "AfterAllConnected | TimeUTC: " << datetime.currentDateTimeUtc();

        double a, b, c, d;
        // indi_Client->setTelescopeInfo(dp2,100,500,30,130);
        indi_Client->getTelescopeInfo(dpMount, a, b, c, d);
        qDebug() << "AfterAllConnected | TelescopeInfo: " << a << b << c << d;

        indi_Client->getTelescopeRADECJ2000(dpMount, a, b);
        indi_Client->getTelescopeRADECJNOW(dpMount, a, b);

        bool isPark;
        indi_Client->getTelescopePark(dpMount, isPark);

        int maxspeed, minspeed, speedvalue, total;

        indi_Client->getTelescopeTotalSlewRate(dpMount, total);
        glTelescopeTotalSlewRate = total;
        qDebug() << "TelescopeTotalSlewRate: " << total;
        emit wsThread->sendMessageToClient("TelescopeTotalSlewRate:" + QString::number(total));
        indi_Client->getTelescopeMaxSlewRateOptions(dpMount, minspeed, maxspeed, speedvalue);
        // indi_Client->setTelescopeMaxSlewRateOptions(dpMount, total - 1);
        indi_Client->setTelescopeSlewRate(dpMount, total);
        int speed;
        indi_Client->getTelescopeSlewRate(dpMount,speed);
        qDebug() << "TelescopeCurrentSlewRate: " << speed;
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

        indi_Client->setTelescopeTrackRate(dpMount, "SIDEREAL");
        QString side;
        indi_Client->getTelescopePierSide(dpMount, side);
        qDebug() << "AfterAllConnected | TelescopePierSide: " << side;
        emit wsThread->sendMessageToClient("TelescopePierSide:" + side);
    }

    if (dpFocuser != NULL)
    {
        emit wsThread->sendMessageToClient("ConnectSuccess:Focuser:" + QString::fromUtf8(dpFocuser->getDeviceName()));
        ConnectedDevices.push_back({"Focuser", QString::fromUtf8(dpFocuser->getDeviceName())});
        indi_Client->GetAllPropertyName(dpFocuser);
        indi_Client->syncFocuserPosition(dpFocuser, 0);
    }

    if (dpCFW != NULL)
    {
        emit wsThread->sendMessageToClient("ConnectSuccess:CFW:" + QString::fromUtf8(dpCFW->getDeviceName()));
        ConnectedDevices.push_back({"CFW", QString::fromUtf8(dpCFW->getDeviceName())});
        indi_Client->GetAllPropertyName(dpCFW);
        int min, max, pos;
        indi_Client->getCFWPosition(dpCFW, pos, min, max);
        qDebug() << "getCFWPosition: " << min << ", " << max << ", " << pos;
        emit wsThread->sendMessageToClient("CFWPositionMax:" + QString::number(max));
        if(Tools::readCFWList(QString::fromUtf8(dpCFW->getDeviceName())) != QString())
        {
            emit wsThread->sendMessageToClient("getCFWList:" + Tools::readCFWList(QString::fromUtf8(dpCFW->getDeviceName())));
        }
    }

    if (dpGuider != NULL)
    {
        emit wsThread->sendMessageToClient("ConnectSuccess:Guider:" + QString::fromUtf8(dpGuider->getDeviceName()));
        ConnectedDevices.push_back({"Guider", QString::fromUtf8(dpGuider->getDeviceName())});
    }
}

void MainWindow::disconnectIndiServer(MyClient *client)
{
    // client->disconnectAllDevice();
    QVector<INDI::BaseDevice *> dp;
    
    for(int i=0;i < client->GetDeviceCount();i++)
    {
        dp.append(client->GetDeviceFromList(i));
        client->disconnectDevice(dp[i]->getDeviceName());
        while (dp[i]->isConnected())
        {
            qDebug("disconnectAllDevice | Waiting for disconnect finish...");
            sleep(1);
        }   
        qDebug()<<"disconnectAllDevice |"<<dp[i]->getDeviceName()<<dp[i]->isConnected();
    }

    client->ClearDevices();
    client->disconnectServer();
    int k = 10;
    while (k--)
    {
        if (client->isServerConnected() == false)
        {
            break;
        }
        sleep(1);
        // qApp->processEvents();
        qDebug("wait for client disconnected");
    }
    qDebug("--------------------------------------------------disconnectServer");
    indi_Client->PrintDevices();
    Tools::printSystemDeviceList(systemdevicelist);
}

void MainWindow::connectIndiServer(MyClient *client)
{
    client->setConnectionTimeout(3, 0);
    client->ClearDevices(); // clear device list
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
        qDebug("wait for client connected");
    }
    qDebug("--------------------------------------------------connectServer");
    sleep(1);
    client->PrintDevices();
}

void MainWindow::ClearSystemDeviceList() {
    qDebug() << "ClearSystemDeviceList";
    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        systemdevicelist.system_devices[i].DeviceIndiGroup = -1;
        systemdevicelist.system_devices[i].DeviceIndiName = "";
        systemdevicelist.system_devices[i].DriverFrom = "";
        systemdevicelist.system_devices[i].DriverIndiName = "";
        systemdevicelist.system_devices[i].isConnect = false;
        systemdevicelist.system_devices[i].dp = NULL;
        systemdevicelist.system_devices[i].Description = "";
    }
    Tools::printSystemDeviceList(systemdevicelist);
}

void MainWindow::INDI_Capture(int Exp_times)
{
    glIsFocusingLooping = false;
    double expTime_sec;
    expTime_sec = (double)Exp_times / 1000;
    qDebug() << "expTime_sec:" << expTime_sec;

    if (dpMainCamera)
    {
        glMainCameraStatu = "Exposuring";
        qDebug() << "INDI Capture Statu:" << glMainCameraStatu;

        int value, min, max;
        indi_Client->getCCDGain(dpMainCamera, value, min, max);

        indi_Client->getCCDOffset(dpMainCamera, value, min, max);

        indi_Client->resetCCDFrameInfo(dpMainCamera);

        emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));

        indi_Client->takeExposure(dpMainCamera, expTime_sec);
    }
    else
    {
        qDebug() << "dpMainCamera is NULL";
    }
    qDebug() << "INDI_Capture | exptime" << expTime_sec;
}

void MainWindow::INDI_AbortCapture()
{
    glMainCameraStatu="IDLE";
    qDebug() << "INDI Capture Statu:" << glMainCameraStatu;
    if (dpMainCamera)
    {
        indi_Client->setCCDAbortExposure(dpMainCamera);
    }
}

void MainWindow::FocusingLooping()
{
    // TO BE FIXED: may cause crash
    if (dpMainCamera == NULL)
        return;

    glIsFocusingLooping = true;
    if (glMainCameraStatu == "Displaying")
    {
        double expTime_sec;
        expTime_sec = (double)glExpTime / 1000;

        glMainCameraStatu = "Exposuring";
        qDebug() << "INDI Capture Statu:" << glMainCameraStatu;

        QSize cameraResolution{glMainCCDSizeX, glMainCCDSizeY};
        QSize ROI{BoxSideLength, BoxSideLength};

        int cameraX = glROI_x * cameraResolution.width() /
                      (double)CaptureViewWidth;
        int cameraY = glROI_y * cameraResolution.height() /
                      (double)CaptureViewHeight;

        if (cameraX < glMainCCDSizeX - ROI.width() && cameraY < glMainCCDSizeY - ROI.height())
        {
            indi_Client->setCCDFrameInfo(dpMainCamera, cameraX, cameraY, BoxSideLength, BoxSideLength); // add by CJQ 2023.2.15
            indi_Client->takeExposure(dpMainCamera, expTime_sec);
        }
        else
        {
            qDebug("Too close to the edge, please reselect the area."); //TODO:
            if (cameraX + ROI.width() > glMainCCDSizeX)
                cameraX = glMainCCDSizeX - ROI.width();
            if (cameraY + ROI.height() > glMainCCDSizeY)
                cameraY = glMainCCDSizeY - ROI.height();

            indi_Client->setCCDFrameInfo(dpMainCamera, cameraX, cameraY, ROI.width(), ROI.height()); // add by CJQ 2023.2.15
            indi_Client->takeExposure(dpMainCamera, expTime_sec);
        }
    }
}

void MainWindow::refreshGuideImage(cv::Mat img16, QString CFA)
{
    // strechShowImage(img16, CFA, true, true, 0, 0, 65535, 1.0, 1.7, 100, true);
}

void MainWindow::strechShowImage(cv::Mat img16,QString CFA,bool AutoStretch,bool AWB,int AutoStretchMode,uint16_t blacklevel,uint16_t whitelevel,double ratioRG,double ratioBG,uint16_t offset,bool updateHistogram){

   uint16_t B=0;
   uint16_t W=65535;

 if(CFA=="MONO") {
  //mono camera, do not do debayer and color balance process
     cv::Mat image_raw8;
     image_raw8.create(img16.rows,img16.cols,CV_8UC1);

     if(AutoStretch==true){
        Tools::GetAutoStretch(img16,AutoStretchMode,B,W);
     } else {
        B=blacklevel;
        W=whitelevel;
     }

    Tools::Bit16To8_Stretch(img16,image_raw8,B,W);

    saveGuiderImageAsJPG(image_raw8);


    image_raw8.release();
 }

 else{
      //color camera, need to do debayer and color balance
     cv::Mat AWBImg16;
     cv::Mat AWBImg16color;
     cv::Mat AWBImg16mono;
     cv::Mat AWBImg8color;
    #ifdef ImageDebug
     qDebug()<<"strechShowImage | color camera";
    #endif
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
     qDebug()<<B<<","<<W;
     Tools::Bit16To8_Stretch(AWBImg16color,AWBImg8color,B,W);

    //  Tools::ShowCvImageOnQLabel(AWBImg8color,lable);
    saveGuiderImageAsJPG(AWBImg8color);

    AWBImg16.release();
    AWBImg16color.release();
    AWBImg16mono.release();
    AWBImg8color.release();
 }
 glMainCameraStatu="IDLE";
 #ifdef ImageDebug
 qDebug() << "strechShowImage:" << glMainCameraStatu;
 #endif
}

void MainWindow::InitPHD2()
{
    isGuideCapture = true;

    cmdPHD2 = new QProcess();
    cmdPHD2->start("pkill phd2");
    cmdPHD2->waitForStarted();
    cmdPHD2->waitForFinished();

    key_phd = ftok("../", 2015);
    key_phd = 0x90;

    if (key_phd == -1)
    {
        qDebug("ftok_phd");
    }

    // build the shared memory
    system("ipcs -m"); // 查看共享内存
    shmid_phd = shmget(key_phd, BUFSZ_PHD, IPC_CREAT | 0666);
    if (shmid_phd < 0)
    {
        qDebug("main.cpp | main | shared memory phd shmget ERROR");
        exit(-1);
    }

    // 映射
    sharedmemory_phd = (char *)shmat(shmid_phd, NULL, 0);
    if (sharedmemory_phd == NULL)
    {
        qDebug("main.cpp | main | shared memor phd map ERROR");
        exit(-1);
    }

    // 读共享内存区数据
    qDebug("data_phd = [%s]\n", sharedmemory_phd);

    cmdPHD2->start("phd2");

    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 10000)
    {
        usleep(10000);
        qApp->processEvents();
        if (connectPHD() == true)
            break;
    }
}

bool MainWindow::connectPHD(void)
{
    QString versionName = "";
    call_phd_GetVersion(versionName);

    qDebug() << "QSCOPE|connectPHD|version:" << versionName;
    if (versionName != "")
    {
        // init stellarium operation
        return true;
    }
    else
    {
        qDebug() << "QSCOPE|connectPHD|error:there is no openPHD2 running";
        return false;
    }
}

bool MainWindow::call_phd_GetVersion(QString &versionName)
{
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
            return true;
            // qDebug()<<versionName;
        }
        else
        {
            versionName = "";
            return false;
        }
    }
}

uint32_t MainWindow::call_phd_StartLooping(void)
{
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
        return false; // timeout
    else
        return true;
}

uint32_t MainWindow::call_phd_StopLooping(void)
{
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
        return false; // timeout
    else
        return true;
}

uint32_t MainWindow::call_phd_AutoFindStar(void)
{
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
        return false; // timeout
    else
        return true;
}

uint32_t MainWindow::call_phd_StartGuiding(void)
{
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
        return false; // timeout
    else
        return true;
}

uint32_t MainWindow::call_phd_checkStatus(unsigned char &status)
{
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
        return false;
    }

    else
    {
        status = sharedmemory_phd[3];
        return true;
    }
}

uint32_t MainWindow::call_phd_setExposureTime(unsigned int expTime)
{
    unsigned int vendcommand;
    unsigned int baseAddress;
    qDebug() << "call_phd_setExposureTime" << expTime;
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
        return QHYCCD_ERROR; // timeout
    else
        return QHYCCD_SUCCESS;
}

uint32_t MainWindow::call_phd_whichCamera(std::string Camera)
{
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
        return QHYCCD_ERROR; // timeout
    else
        return QHYCCD_SUCCESS;
}

uint32_t MainWindow::call_phd_ChackControlStatus(int sdk_num)
{
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
        return false; // timeout
    else
        return true;
}

uint32_t MainWindow::call_phd_ClearCalibration(void)
{
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
        return false; // timeout
    else
        return true;
}

uint32_t MainWindow::call_phd_StarClick(int x, int y)
{
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
        return false; // timeout
    else
        return true;
}

void MainWindow::ShowPHDdata()
{
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
        return; // if there is no image comes, return

    mem_offset = 1024;
    // guide image dimention data
    memcpy(&currentPHDSizeX, sharedmemory_phd + mem_offset, sizeof(unsigned int));
    mem_offset = mem_offset + sizeof(unsigned int);
    memcpy(&currentPHDSizeY, sharedmemory_phd + mem_offset, sizeof(unsigned int));
    mem_offset = mem_offset + sizeof(unsigned int);
    memcpy(&bitDepth, sharedmemory_phd + mem_offset, sizeof(unsigned char));
    mem_offset = mem_offset + sizeof(unsigned char);

    mem_offset = mem_offset + sizeof(int); // &sdk_num
    mem_offset = mem_offset + sizeof(int); // &sdk_direction
    mem_offset = mem_offset + sizeof(int); // &sdk_duration

    guideDataIndicatorAddress = mem_offset;

    // guide error data
    guideDataIndicator = sharedmemory_phd[guideDataIndicatorAddress];

    mem_offset = mem_offset + sizeof(unsigned char);
    memcpy(&dRa, sharedmemory_phd + mem_offset, sizeof(double));
    mem_offset = mem_offset + sizeof(double);
    memcpy(&dDec, sharedmemory_phd + mem_offset, sizeof(double));
    mem_offset = mem_offset + sizeof(double);
    memcpy(&SNR, sharedmemory_phd + mem_offset, sizeof(double));
    mem_offset = mem_offset + sizeof(double);
    memcpy(&MASS, sharedmemory_phd + mem_offset, sizeof(double));
    mem_offset = mem_offset + sizeof(double);

    memcpy(&RADUR, sharedmemory_phd + mem_offset, sizeof(int));
    mem_offset = mem_offset + sizeof(int);
    memcpy(&DECDUR, sharedmemory_phd + mem_offset, sizeof(int));
    mem_offset = mem_offset + sizeof(int);

    memcpy(&RADIR, sharedmemory_phd + mem_offset, sizeof(char));
    mem_offset = mem_offset + sizeof(char);
    memcpy(&DECDIR, sharedmemory_phd + mem_offset, sizeof(char));
    mem_offset = mem_offset + sizeof(char);

    memcpy(&RMSErrorX, sharedmemory_phd + mem_offset, sizeof(double));
    mem_offset = mem_offset + sizeof(double);
    memcpy(&RMSErrorY, sharedmemory_phd + mem_offset, sizeof(double));
    mem_offset = mem_offset + sizeof(double);
    memcpy(&RMSErrorTotal, sharedmemory_phd + mem_offset, sizeof(double));
    mem_offset = mem_offset + sizeof(double);
    memcpy(&PixelRatio, sharedmemory_phd + mem_offset, sizeof(double));
    mem_offset = mem_offset + sizeof(double);
    memcpy(&StarLostAlert, sharedmemory_phd + mem_offset, sizeof(bool));
    mem_offset = mem_offset + sizeof(bool);
    memcpy(&InGuiding, sharedmemory_phd + mem_offset, sizeof(bool));
    mem_offset = mem_offset + sizeof(bool);

    mem_offset = 1024 + 200;
    memcpy(&isSelected, sharedmemory_phd + mem_offset, sizeof(bool));
    mem_offset = mem_offset + sizeof(bool);
    memcpy(&StarX, sharedmemory_phd + mem_offset, sizeof(double));
    mem_offset = mem_offset + sizeof(double);
    memcpy(&StarY, sharedmemory_phd + mem_offset, sizeof(double));
    mem_offset = mem_offset + sizeof(double);
    memcpy(&showLockedCross, sharedmemory_phd + mem_offset, sizeof(bool));
    mem_offset = mem_offset + sizeof(bool);
    memcpy(&LockedPositionX, sharedmemory_phd + mem_offset, sizeof(double));
    mem_offset = mem_offset + sizeof(double);
    memcpy(&LockedPositionY, sharedmemory_phd + mem_offset, sizeof(double));
    mem_offset = mem_offset + sizeof(double);
    memcpy(&MultiStarNumber, sharedmemory_phd + mem_offset, sizeof(unsigned char));
    mem_offset = mem_offset + sizeof(unsigned char);
    memcpy(MultiStarX, sharedmemory_phd + mem_offset, sizeof(MultiStarX));
    mem_offset = mem_offset + sizeof(MultiStarX);
    memcpy(MultiStarY, sharedmemory_phd + mem_offset, sizeof(MultiStarY));
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

    glPHD_Stars.clear();
    for (int i = 0; i < MultiStarNumber; i++)
    {
        if (i > 30)
            break;
        QPoint p;
        p.setX(MultiStarX[i]);
        p.setY(MultiStarY[i]);
        glPHD_Stars.push_back(p);
    }

    // if (glPHD_StarX != 0 && glPHD_StarY != 0)
    // {
        if (glPHD_isSelected == true)
        {
            qDebug() << "PHD2StarBoxPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" + QString::number(glPHD_CurrentImageSizeY) + ":" + QString::number(glPHD_StarX) + ":" + QString::number(glPHD_StarY);
            emit wsThread->sendMessageToClient("PHD2StarBoxView:true");
            emit wsThread->sendMessageToClient("PHD2StarBoxPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" + QString::number(glPHD_CurrentImageSizeY)+ ":" + QString::number(glPHD_StarX) + ":" + QString::number(glPHD_StarY));
        }
        else
        {
            emit wsThread->sendMessageToClient("PHD2StarBoxView:false");
        }

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

    unsigned int byteCount;
    byteCount = currentPHDSizeX * currentPHDSizeY * (bitDepth / 8);

    mem_offset = 2048;

    unsigned char m = sharedmemory_phd[2047];

    if (sharedmemory_phd[2047] == 0x02 && bitDepth > 0 && currentPHDSizeX > 0 && currentPHDSizeY > 0)
    {
        // 导星过程中的数据
        // qDebug() << guideDataIndicator << "dRa:" << dRa << "dDec:" << dDec
        //          << "rmsX:" << RMSErrorX << "rmsY:" << RMSErrorY
        //          << "rmsTotal:" << RMSErrorTotal << "SNR:" << SNR;
                unsigned char phdstatu;
        call_phd_checkStatus(phdstatu);

        if (dRa != 0 && dDec != 0)
        {
            // if(FirstRecordTelescopePierSide) {
            //     if(dpMount != NULL) {
            //         FirstRecordTelescopePierSide = false;
            //         indi_Client->getTelescopePierSide(dpMount, FirstTelescopePierSide);
            //     }
            // }
            emit wsThread->sendMessageToClient("GuiderSwitchStatus:true");

            QPointF tmp;
            tmp.setX(-dRa * PixelRatio);
            tmp.setY(dDec * PixelRatio);
            glPHD_rmsdate.append(tmp);
            //   m_pToolbarWidget->guiderLabel->Series_err->append(-dRa * PixelRatio, -dDec * PixelRatio);
            emit wsThread->sendMessageToClient("AddScatterChartData:" + QString::number(-dRa * PixelRatio) + ":" + QString::number(-dDec * PixelRatio));

            // 曲线的数值
            // qDebug() << "Ra|Dec: " << -dRa * PixelRatio << "," << dDec * PixelRatio;

            // 图像中的小绿框
            if (InGuiding == true)
            {
                // m_pToolbarWidget->LabelMainStarBox->setStyleSheet("QLabel{border:2px solid rgb(0,255,0);border-radius:3px;background-color:transparent;}");
                // m_pToolbarWidget->LabelCrossX->setStyleSheet("QLabel{border:1px solid rgb(0,255,0);border-radius:3px;background-color:transparent;}");
                // m_pToolbarWidget->LabelCrossY->setStyleSheet("QLabel{border:1px solid rgb(0,255,0);border-radius:3px;background-color:transparent;}");
                emit wsThread->sendMessageToClient("GuiderStatus:InGuiding");
            }
            else
            {
                // m_pToolbarWidget->LabelMainStarBox->setStyleSheet("QLabel{border:2px solid rgb(255,255,0);border-radius:3px;background-color:transparent;}");
                // m_pToolbarWidget->LabelCrossX->setStyleSheet("QLabel{border:1px solid rgb(255,255,0);border-radius:3px;background-color:transparent;}");
                // m_pToolbarWidget->LabelCrossY->setStyleSheet("QLabel{border:1px solid rgb(255,255,0);border-radius:3px;background-color:transparent;}");
                qDebug() << "GuiderStatus:InCalibration";
                emit wsThread->sendMessageToClient("GuiderStatus:InCalibration");
            }

            if (StarLostAlert == true)
            {
                // m_pToolbarWidget->LabelMainStarBox->setStyleSheet("QLabel{border:2px solid rgb(255,0,0);border-radius:3px;background-color:transparent;}");
                // m_pToolbarWidget->LabelCrossX->setStyleSheet("QLabel{border:1px solid rgb(255,0,0);border-radius:3px;background-color:transparent;}");
                // m_pToolbarWidget->LabelCrossY->setStyleSheet("QLabel{border:1px solid rgb(255,0,0);border-radius:3px;background-color:transparent;}");
                emit wsThread->sendMessageToClient("GuiderStatus:StarLostAlert");
            }

            emit wsThread->sendMessageToClient("AddRMSErrorData:" + QString::number(RMSErrorX, 'f', 3) + ":" + QString::number(RMSErrorX, 'f', 3));
        }
        // m_pToolbarWidget->guiderLabel->RMSErrorX_value->setPlainText(QString::number(RMSErrorX, 'f', 3));
        // m_pToolbarWidget->guiderLabel->RMSErrorY_value->setPlainText(QString::number(RMSErrorY, 'f', 3));

        // m_pToolbarWidget->guiderLabel->GuiderDataRA->clear();
        // m_pToolbarWidget->guiderLabel->GuiderDataDEC->clear();

        for (int i = 0; i < glPHD_rmsdate.size(); i++)
        {
            //   m_pToolbarWidget->guiderLabel->GuiderDataRA ->append(i, glPHD_rmsdate[i].x());
            //   m_pToolbarWidget->guiderLabel->GuiderDataDEC->append(i, glPHD_rmsdate[i].y());
            if (i == glPHD_rmsdate.size() - 1)
            {
                emit wsThread->sendMessageToClient("AddLineChartData:" + QString::number(i) + ":" + QString::number(glPHD_rmsdate[i].x()) + ":" + QString::number(glPHD_rmsdate[i].y()));
                if (i > 50)
                {
                    // m_pToolbarWidget->guiderLabel->AxisX_Graph->setRange(i-100,i);
                    emit wsThread->sendMessageToClient("SetLineChartRange:" + QString::number(i - 50) + ":" + QString::number(i));
                }
            }
        }

        unsigned char *srcData = new unsigned char[byteCount];
        mem_offset = 2048;

        memcpy(srcData, sharedmemory_phd + mem_offset, byteCount);
        sharedmemory_phd[2047] = 0x00; // 0x00= image has been read

        cv::Mat img8;
        cv::Mat PHDImg;

        img8.create(currentPHDSizeY, currentPHDSizeX, CV_8UC1);

        if (bitDepth == 16)
            PHDImg.create(currentPHDSizeY, currentPHDSizeX, CV_16UC1);
        else
            PHDImg.create(currentPHDSizeY, currentPHDSizeX, CV_8UC1);

        PHDImg.data = srcData;

        // if(isLoopSolveImage) {
        //     if(Tools::WaitForPlateSolveToComplete() && !TakeNewCapture) {
        //         qDebug() << "isLoopSolveImage:" << isLoopSolveImage;

        //         TakeNewCapture = true;

        //         // cv::Mat image;
        //         // Tools::readFits("/dev/shm/ccd_simulator.fits", image);   // ccd_simulator  SolveImage

        //         // // // 获取图像的原始宽高
        //         // // int originalWidth = image.cols;
        //         // // int originalHeight = image.rows;

        //         // // // 计算缩放后的高度
        //         // // int newWidth = 1024;
        //         // // int newHeight = static_cast<int>(originalHeight * (newWidth / static_cast<double>(originalWidth)));
        //         // // SolveImageScaledHeight = newHeight;

        //         // // // 调整图像大小
        //         // // cv::Mat resizedImage;
        //         // // cv::resize(image, resizedImage, cv::Size(newWidth, newHeight));

        //         // bool isColor = !(MainCameraCFA == "");
        //         // CamBin CamBin = Tools::mergeImageBasedOnSize(image);

        //         // cv::Mat resizedImage;

        //         // if(CamBin.camxbin != 1) {
        //         //     resizedImage = Tools::processMatWithBinAvg(image, CamBin.camxbin, CamBin.camybin, isColor, false);
        //         // } else {
        //         //     resizedImage = image.clone();
        //         // }

        //         // cv::Mat resultImage;
        //         // cv::medianBlur(resizedImage,resultImage,3);
        //         QString SolveImageFileName = "/dev/shm/SolveImage.tiff";
        //         // cv::imwrite(SolveImageFileName.toStdString(), resultImage);     //TODO:

        //         LoopSolveImage(SolveImageFileName, glFocalLength, glCameraSize_width, glCameraSize_height);
        //     }
        // }

        // if(isSingleSolveImage) {
        //     if(Tools::WaitForPlateSolveToComplete()) {
        //         isSingleSolveImage = false;

        //         // cv::Mat image;
        //         // Tools::readFits("/dev/shm/ccd_simulator.fits", image);     // ccd_simulator  SolveImage

        //         // // // 获取图像的原始宽高
        //         // // int originalWidth = image.cols;
        //         // // int originalHeight = image.rows;

        //         // // // 计算缩放后的高度
        //         // // int newWidth = 1024;
        //         // // int newHeight = static_cast<int>(originalHeight * (newWidth / static_cast<double>(originalWidth)));
        //         // // SolveImageScaledHeight = newHeight;

        //         // // // 调整图像大小
        //         // // cv::Mat resizedImage;
        //         // // cv::resize(image, resizedImage, cv::Size(newWidth, newHeight));

        //         // bool isColor = !(MainCameraCFA == "");
        //         // CamBin CamBin = Tools::mergeImageBasedOnSize(image);

        //         // qDebug() << "Binning:" << CamBin.camxbin << "," << CamBin.camybin;

        //         // cv::Mat resizedImage;

        //         // if(CamBin.camxbin != 1) {
        //         //     resizedImage = Tools::processMatWithBinAvg(image, 2, 2, isColor, false);
        //         // } else {
        //         //     resizedImage = image.clone();
        //         // }

        //         // cv::Mat resultImage;
        //         // cv::medianBlur(resizedImage,resultImage,3);
        //         QString SolveImageFileName = "/dev/shm/SolveImage.tiff";
        //         // cv::imwrite(SolveImageFileName.toStdString(), resultImage);     //TODO:
                
        //         CaptureTestTimer.start();
        //         qDebug() << "\033[32m" << "Solve image start." << "\033[0m";
        //         SolveImage(SolveImageFileName, glFocalLength, glCameraSize_width, glCameraSize_height);
        //         CaptureTestTime = CaptureTestTimer.elapsed();
        //         qDebug() << "\033[32m" << "Solve image completed:" << CaptureTestTime << "milliseconds" << "\033[0m";
        //         CaptureTestTimer.invalidate();
        //     }
        // }

        uint16_t B = 0;
        uint16_t W = 65535;

        cv::Mat image_raw8;
        image_raw8.create(PHDImg.rows, PHDImg.cols, CV_8UC1);

        if (AutoStretch == true)
        {
            Tools::GetAutoStretch(PHDImg, 0, B, W);
        }
        else
        {
            B = 0;
            W = 65535;
        }

        Tools::Bit16To8_Stretch(PHDImg, image_raw8, B, W);

        saveGuiderImageAsJPG(image_raw8);
        
        emit wsThread->sendMessageToClient("GuiderLoopExpStatus:true");

        // saveGuiderImageAsJPG(PHDImg);

        // refreshGuideImage(PHDImg, "MONO");

        int centerX = glPHD_StarX; // Replace with your X coordinate
        int centerY = glPHD_StarY; // Replace with your Y coordinate

        int cropSize = 20; // Size of the cropped region

        // Calculate crop region
        int startX = std::max(0, centerX - cropSize / 2);
        int startY = std::max(0, centerY - cropSize / 2);
        int endX = std::min(PHDImg.cols - 1, centerX + cropSize / 2);
        int endY = std::min(PHDImg.rows - 1, centerY + cropSize / 2);

        // Crop the image using OpenCV's ROI (Region of Interest) functionality
        cv::Rect cropRegion(startX, startY, endX - startX + 1, endY - startY + 1);
        cv::Mat croppedImage = PHDImg(cropRegion).clone();

        // strechShowImage(croppedImage, m_pToolbarWidget->guiderLabel->ImageLable,m_pToolbarWidget->histogramLabel->hisLabel,"MONO",false,false,0,0,65535,1.0,1.7,100,false);
        // m_pToolbarWidget->guiderLabel->ImageLable->setScaledContents(true);

        delete[] srcData;
        img8.release();
        PHDImg.release();
    }
}

void MainWindow::ControlGuide(int Direction, int Duration)
{
    // qDebug() << "\033[32m"
    //          << "ControlGuide: "
    //          << "\033[0m" << Direction << "," << Duration;
    switch (Direction)
    {
    case 1:
    {
        if (dpMount != NULL)
        {
            if(isMeridianFlipped) {
                indi_Client->setTelescopeGuideNS(dpMount, 0, Duration);
            } else {
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
                indi_Client->setTelescopeGuideNS(dpMount, 1, Duration);
            } else {
                indi_Client->setTelescopeGuideNS(dpMount, Direction, Duration);
            }
        }
        break;
    }
    case 2:
    {
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeGuideWE(dpMount, Direction, Duration);
        }
        break;
    }
    case 3:
    {
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeGuideWE(dpMount, Direction, Duration);
        }
        break;
    }
    default:
        break; //
    }
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
    GetPHD2ControlInstruct();
}

void MainWindow::GetPHD2ControlInstruct()
{
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
    int mem_offset_sdk_num = mem_offset;
    mem_offset = mem_offset + sizeof(int);

    sdk_num = (ControlInstruct >> 24) & 0xFFF;       // 取前12位
    sdk_direction = (ControlInstruct >> 12) & 0xFFF; // 取中间12位
    sdk_duration = ControlInstruct & 0xFFF;          // 取后12位

    if (sdk_num != 0)
    {
        getTimeNow(sdk_num);
        // std::cout << "\033[31m"
        //           << "PHD2ControlTelescope: "
        //           << "\033[0m" << sdk_num << "," << sdk_direction << ","
        //           << sdk_duration << std::endl;
    }
    if (sdk_duration != 0)
    {
        MainWindow::ControlGuide(sdk_direction, sdk_duration);

        memcpy(sharedmemory_phd + mem_offset_sdk_num, &zero, sizeof(int));

        call_phd_ChackControlStatus(sdk_num); // set pFrame->ControlStatus = 0;
    }
}

void MainWindow::FocuserControl_Goto(int Position)
{
  if (dpFocuser != NULL) 
  {
    focusTimer.stop();
    focusTimer.disconnect();

    CurrentPosition = FocuserControl_getPosition();

    TargetPosition = Position;

    qDebug() << "TargetPosition: " << TargetPosition;

    indi_Client->moveFocuserToAbsolutePosition(dpFocuser, Position);

    focusTimer.setSingleShot(true);

    connect(&focusTimer, &QTimer::timeout, [this]() {
        CurrentPosition = FocuserControl_getPosition();
        emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(TargetPosition));
        
        if (WaitForFocuserToComplete()) 
        {
            focusTimer.stop();  // 转动完成时停止定时器
            qDebug() << "Focuser Goto Complete!";
            FocusingLooping();
        } 
        else 
        {
            focusTimer.start(100);  // 继续等待
        } 
    });

    focusTimer.start(100);
  }
}

void MainWindow::AutoFocus() 
{
    StopAutoFocus = false;
    int stepIncrement = 100;

    bool isInward = true;

    FocusMoveAndCalHFR(!isInward, stepIncrement * 5);

    int initialPosition = FocuserControl_getPosition();
    int currentPosition = initialPosition;

    int Pass3Steps;

    QVector<QPointF> focusMeasures;

    int OnePassSteps = 8;
    int LostStarNum = 0;

    for(int i = 1; i < OnePassSteps; i++) {
        if (StopAutoFocus) {
            qDebug("Stop Auto Focus...");
            emit wsThread->sendMessageToClient("AutoFocusOver:true");
            return;
        }
        double HFR = FocusMoveAndCalHFR(isInward, stepIncrement);
        qDebug() << "Pass1: HFR-" << i << "(" << HFR << ") Calculation Complete!";
        if (HFR == -1) {
            LostStarNum++;
            if (LostStarNum >= 3) {
                qDebug("Too many number of lost star points.");
                FocusGotoAndCalFWHM(initialPosition-stepIncrement * 5);
                qDebug("Returned to the starting point.");
                emit wsThread->sendMessageToClient("AutoFocusOver:true");
                return;
            }
        }
        currentPosition = FocuserControl_getPosition();
        focusMeasures.append(QPointF(currentPosition, HFR));
    }

    float a, b, c;
    int result = Tools::fitQuadraticCurve(focusMeasures, a, b, c);
    if (result == 0)
    {
        if(R2 < 0.8) {
            qDebug("R² < 0.8");
            // FocusMoveAndCalHFR(!isInward, stepIncrement * 10);
            emit wsThread->sendMessageToClient("AutoFocusOver:true");
            return;
        }

        if(a < 0) {
            // 抛物线的开口向下
            qDebug("抛物线的开口向下");
            // FocusMoveAndCalHFR(!isInward, stepIncrement * 10);
        }

        int countLessThan = 0;
        int countGreaterThan = 0;

        for (const QPointF &point : focusMeasures)
        {
            if (point.x() < minPoint_X)
            {
                countLessThan++;
            }
            else if (point.x() > minPoint_X)
            {
                countGreaterThan++;
            }
        }

        if (countLessThan > countGreaterThan) {
            qDebug() << "More points are less than minPoint_X.";
            if(a > 0) {
                // 抛物线的开口向上
                FocusMoveAndCalHFR(!isInward, stepIncrement * (OnePassSteps-1) * 2);
            } 
        }
        else if (countGreaterThan > countLessThan) {
            qDebug() << "More points are greater than minPoint_X.";
            if(a < 0) {
                FocusMoveAndCalHFR(!isInward, stepIncrement * (OnePassSteps-1) * 2);
            }
        }
    }

    for(int i = 1; i < OnePassSteps; i++) {
        if (StopAutoFocus) {
            qDebug("Stop Auto Focus...");
            emit wsThread->sendMessageToClient("AutoFocusOver:true");
            return;
        }
        double HFR = FocusMoveAndCalHFR(isInward, stepIncrement);
        qDebug() << "Pass2: HFR-" << i << "(" << HFR << ") Calculation Complete!";
        currentPosition = FocuserControl_getPosition();
        focusMeasures.append(QPointF(currentPosition, HFR));
    }

    float a_, b_, c_;
    int result_ = Tools::fitQuadraticCurve(focusMeasures, a_, b_, c_);
    if (result_ == 0)
    {
        if(R2 < 0.8) {
            qDebug("R² < 0.8");
            // FocusMoveAndCalHFR(!isInward, stepIncrement * 10);
            emit wsThread->sendMessageToClient("AutoFocusOver:true");
            return;
        }

        if(a_ < 0) {
            // 抛物线的开口向下
            qDebug("抛物线的开口向下"); 
        }

        int countLessThan = 0;
        int countGreaterThan = 0;

        for (const QPointF &point : focusMeasures)
        {
            if (point.x() < minPoint_X)
            {
                countLessThan++;
            }
            else if (point.x() > minPoint_X)
            {
                countGreaterThan++;
            }
        }

        if (countLessThan > countGreaterThan) {
            qDebug() << "More points are less than minPoint_X.";
            Pass3Steps = countLessThan-countGreaterThan;
            qDebug() << "Pass3Steps: " << Pass3Steps;

            FocusGotoAndCalFWHM(minPoint_X);

            FocusMoveAndCalHFR(!isInward, stepIncrement * countLessThan);
        }
        else if (countGreaterThan > countLessThan) {
            qDebug() << "More points are greater than minPoint_X.";
            Pass3Steps = countGreaterThan - countLessThan;
            qDebug() << "Pass3Steps: " << Pass3Steps;

            FocusGotoAndCalFWHM(minPoint_X);
            if(countLessThan > 0){
                FocusMoveAndCalHFR(isInward, stepIncrement * countLessThan);
            }
        }
        else {
            qDebug() << "The number of points less than and greater than minPoint_X is equal.";
            FocusGotoAndCalFWHM(minPoint_X);
            qDebug() << "Auto focus complete. Best step: " << minPoint_X;
            emit wsThread->sendMessageToClient("AutoFocusOver:true");
            return;
        }
    }

    for(int i = 1; i < Pass3Steps + 1; i++) {
        if (StopAutoFocus) {
            qDebug("Stop Auto Focus...");
            emit wsThread->sendMessageToClient("AutoFocusOver:true");
            return;
        }
        double HFR = FocusMoveAndCalHFR(isInward, stepIncrement);
        qDebug() << "Pass3: HFR-" << i << "(" << HFR << ") Calculation Complete!";
        currentPosition = FocuserControl_getPosition();
        focusMeasures.append(QPointF(currentPosition, HFR));
    }

    FocusGotoAndCalFWHM(minPoint_X);
    qDebug() << "Auto focus complete. Best step: " << minPoint_X;
    currentPosition = FocuserControl_getPosition();
    qDebug() << "Current Position : " << currentPosition;
    emit wsThread->sendMessageToClient("AutoFocusOver:true");
}

double MainWindow::FocusGotoAndCalFWHM(int steps) {
    QEventLoop loop;
    double FWHM = 0;

    // 停止和清理先前的计时器
    FWHMTimer.stop();
    FWHMTimer.disconnect();

    FWHMCalOver = false;
    FocuserControl_Goto(steps);

    FWHMTimer.setSingleShot(true);

    connect(&FWHMTimer, &QTimer::timeout, this, [this, &loop, &FWHM]() {
        if (FWHMCalOver) 
        {
            FWHM = this->FWHM;  // 假设 this->FWHM 保存了计算结果
            FWHMTimer.stop();
            qDebug() << "FWHM Calculation Complete!";
            emit wsThread->sendMessageToClient("FocusMoveDone:" + QString::number(FWHM));
            loop.quit();
        } 
        else 
        {
            FWHMTimer.start(1000);  // 继续等待
        }
    });

    FWHMTimer.start(1000);
    loop.exec();  // 等待事件循环直到调用 loop.quit()

    return FWHM;
}

double MainWindow::FocusMoveAndCalHFR(bool isInward, int steps) {
    QEventLoop loop;
    double FWHM = 0;

    // 停止和清理先前的计时器
    FWHMTimer.stop();
    FWHMTimer.disconnect();

    FWHMCalOver = false;
    FocuserControl_Move(isInward, steps);

    FWHMTimer.setSingleShot(true);

    connect(&FWHMTimer, &QTimer::timeout, this, [this, &loop, &FWHM]() {
        if (FWHMCalOver) 
        {
            FWHM = this->FWHM;  // 假设 this->FWHM 保存了计算结果
            FWHMTimer.stop();
            qDebug() << "FWHM Calculation Complete!";
            emit wsThread->sendMessageToClient("FocusMoveDone:" + QString::number(FWHM));
            loop.quit();
        } 
        else 
        {
            FWHMTimer.start(1000);  // 继续等待
        }
    });

    FWHMTimer.start(1000);
    loop.exec();  // 等待事件循环直到调用 loop.quit()

    return FWHM;
}

void MainWindow::FocuserControl_Move(bool isInward, int steps)
{
  if (dpFocuser != NULL) 
  {
    focusTimer.stop();
    focusTimer.disconnect();

    CurrentPosition = FocuserControl_getPosition();

    if(isInward == false)
    {
        TargetPosition = CurrentPosition + steps;
    }
    else
    {
        TargetPosition = CurrentPosition - steps;
    }
    qDebug() << "TargetPosition: " << TargetPosition;

    indi_Client->setFocuserMoveDiretion(dpFocuser, isInward);
    indi_Client->moveFocuserSteps(dpFocuser, steps);

    focusTimer.setSingleShot(true);

    connect(&focusTimer, &QTimer::timeout, [this]() {
        CurrentPosition = FocuserControl_getPosition();
        emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(TargetPosition));
        
        if (WaitForFocuserToComplete()) 
        {
            focusTimer.stop();  // 转动完成时停止定时器
            qDebug() << "Focuser Move Complete!";
            FocusingLooping();
        } 
        else 
        {
            focusTimer.start(100);  // 继续等待
        } 
    });

    focusTimer.start(100);

  }
}

bool MainWindow::WaitForFocuserToComplete() {
   return(CurrentPosition == TargetPosition);
}

int MainWindow::FocuserControl_setSpeed(int speed)
{
  if (dpFocuser != NULL) 
  {
    int value, min, max;
    indi_Client->setFocuserSpeed(dpFocuser, speed);
    indi_Client->getFocuserSpeed(dpFocuser, value, min, max);
    qDebug() << "Focuser Speed: " << value << "," << min << "," << max;
    return value;
  }
}

int MainWindow::FocuserControl_getSpeed()
{
  if (dpFocuser != NULL) 
  {
    int value, min, max;
    indi_Client->getFocuserSpeed(dpFocuser, value, min, max);
    qDebug() << "Focuser Speed: " << value << "," << min << "," << max;
    return value;
  }
}

int MainWindow::FocuserControl_getPosition()
{
  if (dpFocuser != NULL) 
  {
    int value;
    indi_Client->getFocuserAbsolutePosition(dpFocuser, value);
    return value;
  }
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
    qDebug()<<"isPark???:"<<isPark;
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
    qDebug()<<"isTrack???:"<<isTrack;
  }
  return isTrack;
}

void MainWindow::ScheduleTabelData(QString message)
{
    ScheduleTargetNames.clear();
    m_scheduList.clear();
    schedule_currentShootNum = 0;
    QStringList ColDataList = message.split('[');
    for (int i = 1; i < ColDataList.size(); ++i) {
        QString ColData = ColDataList[i];   // ",M 24, Ra:4.785693,Dec:-0.323759,12:00:00,1 s,Ha,,Bias,ON,],"
        ScheduleData rowData;
        qDebug() << "ColData[" << i << "]:" << ColData;

        QStringList RowDataList = ColData.split(',');
        for (int j = 1; j < 10; ++j) {
            if(j == 1){
                rowData.shootTarget = RowDataList[j];
                qDebug() << "Target:" << rowData.shootTarget;
                // 将 shootTarget 添加到 ScheduleTargetNames 中
                if (!ScheduleTargetNames.isEmpty())
                {
                    ScheduleTargetNames += ",";
                }
                ScheduleTargetNames += rowData.shootTarget;
            } else if(j == 2){
                QStringList parts = RowDataList[j].split(':');
                rowData.targetRa = Tools::RadToHour(parts[1].toDouble());
                qDebug() << "Ra:" << rowData.targetRa;
            } else if(j == 3){
                QStringList parts = RowDataList[j].split(':');
                rowData.targetDec = Tools::RadToDegree(parts[1].toDouble());
                qDebug() << "Dec:" << rowData.targetDec;
            } else if(j == 4){
                rowData.shootTime = RowDataList[j];
                qDebug() << "Time:" << rowData.shootTime;
            } else if(j == 5){
                QStringList parts = RowDataList[j].split(' ');
                QString value = parts[0];
                QString unit = parts[1];
                if (unit == "s")
                    rowData.exposureTime = value.toInt() * 1000; // Convert seconds to milliseconds
                else if (unit == "ms")
                    rowData.exposureTime = value.toInt(); // Milliseconds
                if (rowData.exposureTime == 0) {
                    rowData.exposureTime = 1000;
                    qDebug() << "Exptime error, Exptime = 1000 ms";
                }
                qDebug() << "Exptime:" << rowData.exposureTime;
            } else if(j == 6){
                rowData.filterNumber = RowDataList[j];
                qDebug() << "CFW:" << rowData.filterNumber;
            } else if(j == 7){
                if(RowDataList[j] == "") {
                    rowData.repeatNumber = 1;
                    qDebug() << "Repeat error, Repeat = 1";
                } else {
                    rowData.repeatNumber = RowDataList[j].toInt();
                }
                qDebug() << "Repeat:" << rowData.repeatNumber;
            } else if(j == 8){
                rowData.shootType = RowDataList[j];
                qDebug() << "Type:" << rowData.shootType;
            } else if(j == 9){
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

void MainWindow::nextSchedule() {
    // next schedule...
    schedule_currentNum++;
    qDebug() << "next schedule...";
    startSchedule();
}

void MainWindow::startTimeWaiting() 
{
    // 停止和清理先前的计时器
    timewaitingTimer.stop();
    timewaitingTimer.disconnect();

    // 启动等待的定时器
   timewaitingTimer.setSingleShot(true);

    connect(&timewaitingTimer, &QTimer::timeout, [this]() {
        if (StopSchedule)
        {
            StopSchedule = false;
            qDebug("Schedule is stop!");
            return;
        }

        if (WaitForTimeToComplete()) 
        {
            timewaitingTimer.stop();  // 完成时停止定时器
            qDebug() << "Time Waiting Complete!";

            startMountGoto(m_scheduList[schedule_currentNum].targetRa, m_scheduList[schedule_currentNum].targetDec);
        } 
        else 
        {
            timewaitingTimer.start(1000);  // 继续等待
        } 
    });

    timewaitingTimer.start(1000);
}

void MainWindow::startMountGoto(double ra, double dec)  // Ra:Hour, Dec:Degree
{
    // 停止和清理先前的计时器
    telescopeTimer.stop();
    telescopeTimer.disconnect();

    qDebug() << "Mount Goto:" << ra << "," << dec;
    MountGotoError = false;

    TelescopeControl_Goto(ra, dec);
    call_phd_StopLooping();
    GuidingHasStarted = false;

    sleep(2); //赤道仪的状态更新有一定延迟

    // 启动等待赤道仪转动的定时器
    telescopeTimer.setSingleShot(true);

    connect(&telescopeTimer, &QTimer::timeout, [this]() {
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
                startGuiding();
            }
        } 
        else 
        {
            telescopeTimer.start(1000);  // 继续等待
        } 
    });

    telescopeTimer.start(1000);
}

void MainWindow::startGuiding() 
{
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

    connect(&guiderTimer, &QTimer::timeout, [this]() {
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
            qDebug() << "Guiding Complete!";

            // startCapture(schedule_ExpTime);
            startSetCFW(schedule_CFWpos);
        } 
        else 
        {
            guiderTimer.start(1000);  // 继续等待
        } 
    });

    guiderTimer.start(1000);
}

void MainWindow::startSetCFW(int pos)
{
    if (isFilterOnCamera) {
        if (dpMainCamera != NULL) {
            qDebug() << "schedule CFW pos:" << pos;
            indi_Client->setCFWPosition(dpMainCamera, pos);
            startCapture(schedule_ExpTime);
        }
    } else {
        if (dpCFW != NULL) {
            qDebug() << "schedule CFW pos:" << pos;
            indi_Client->setCFWPosition(dpCFW, pos);
            startCapture(schedule_ExpTime);
        }
    }
}

void MainWindow::startCapture(int ExpTime)
{
    // 停止和清理先前的计时器
    captureTimer.stop();
    captureTimer.disconnect();

    ShootStatus = "InProgress";
    qDebug() << "ShootStatus: " << ShootStatus;
    INDI_Capture(ExpTime);
    schedule_currentShootNum ++;

    captureTimer.setSingleShot(true);

    connect(&captureTimer, &QTimer::timeout, [this]() {
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

                nextSchedule();
            }

        } 
        else 
        {
            captureTimer.start(1000);  // 继续等待
        } 
    });

    captureTimer.start(1000);
}

bool MainWindow::WaitForTelescopeToComplete() {
  return (TelescopeControl_Status().status != "Slewing");
}

bool MainWindow::WaitForShootToComplete() {
  qDebug("Wait For Shoot To Complete...");
  return (ShootStatus != "InProgress");
}

bool MainWindow::WaitForGuidingToComplete() {
  qDebug() << "Wait For Guiding To Complete..." << InGuiding;
  return InGuiding;
}

bool MainWindow::WaitForTimeToComplete() {
    qDebug() << "Wait For Time To Complete...";
    QString TargetTime = m_scheduList[schedule_currentNum].shootTime;

    // 如果获取到的目标时间不是完整的时间格式，直接返回 true
    if (TargetTime.length() != 5 || TargetTime[2] != ':')
        return true;
    
    // 获取当前时间
    QTime currentTime = QTime::currentTime();
    // 解析目标时间
    QTime targetTime = QTime::fromString(TargetTime, "hh:mm");

    qDebug() << "currentTime:" << currentTime << ", targetTime:" << targetTime;

    // 如果目标时间晚于当前时间，返回 false
    if (targetTime > currentTime)
        return false;
    
    // 目标时间早于或等于当前时间，返回 true
    return true;
}

int MainWindow::CaptureImageSave() {
    createCaptureDirectory();
    const char* sourcePath = "/dev/shm/ccd_simulator.fits";

    // QString resultFileName = QTime::currentTime().toString() + ".fits";
    QString resultFileName = QTime::currentTime().toString("HH-mm-ss") + ".fits";
    
    std::time_t currentTime = std::time(nullptr);
    std::tm *timeInfo = std::localtime(&currentTime);
    char buffer[80];
    std::strftime(buffer, 80, "%Y-%m-%d", timeInfo); // Format: YYYY-MM-DD

    // 指定目标目录
    QString destinationDirectory = ImageSaveBaseDirectory + "/CaptureImage";

    QString destinationPath = destinationDirectory  + "/" + buffer + "/" + resultFileName;

    // 将QString转换为const char*
    const char* destinationPathChar = destinationPath.toUtf8().constData();

    std::ifstream sourceFile(sourcePath, std::ios::binary);
    if (!sourceFile.is_open()) {
        std::cerr << "无法打开源文件：" << sourcePath << std::endl;
        return 1;
    }

    std::ofstream destinationFile(destinationPathChar, std::ios::binary);
    if (!destinationFile.is_open()) {
        std::cerr << "无法创建或打开目标文件：" << destinationPathChar << std::endl;
        return 1;
    }

    destinationFile << sourceFile.rdbuf();

    sourceFile.close();
    destinationFile.close();

    return 0;
}

int MainWindow::ScheduleImageSave(QString name, int num)
{
  const char* sourcePath = "/dev/shm/ccd_simulator.fits";

  name.replace(' ', '_');
  QString resultFileName = QString("%1-%2.fits").arg(name).arg(num);

  std::time_t currentTime = std::time(nullptr);
  std::tm *timeInfo = std::localtime(&currentTime);
  char buffer[80];
  std::strftime(buffer, 80, "%Y-%m-%d", timeInfo); // Format: YYYY-MM-DD

  // 指定目标目录
  QString destinationDirectory = ImageSaveBaseDirectory + "/ScheduleImage";

  // 拼接目标文件路径
  QString destinationPath = destinationDirectory  + "/" + buffer + " " + QTime::currentTime().toString("hh") + "h [" + ScheduleTargetNames + "]" + "/" + resultFileName;

  // 将QString转换为const char*
  const char* destinationPathChar = destinationPath.toUtf8().constData();

  std::ifstream sourceFile(sourcePath, std::ios::binary);
  if (!sourceFile.is_open()) {
    std::cerr << "无法打开源文件：" << sourcePath << std::endl;
    return 1;
  }

  std::ofstream destinationFile(destinationPathChar, std::ios::binary);
  if (!destinationFile.is_open()) {
    std::cerr << "无法创建或打开目标文件：" << destinationPathChar << std::endl;
    return 1;
  }

  destinationFile << sourceFile.rdbuf();

  sourceFile.close();
  destinationFile.close();

  return 0;
}

bool MainWindow::directoryExists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool MainWindow::createScheduleDirectory() {
    std::string basePath = ImageSaveBasePath + "/ScheduleImage";

    std::time_t currentTime = std::time(nullptr);
    std::tm* timeInfo = std::localtime(&currentTime);
    char buffer[80];
    std::strftime(buffer, 80, "%Y-%m-%d", timeInfo); // Format: YYYY-MM-DD
    std::string folderName = basePath + "/" + buffer + " " + QTime::currentTime().toString("hh").toStdString() + "h [" + ScheduleTargetNames.toStdString() + "]";

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
            std::cout << "文件夹创建成功: " << folderName << std::endl;
        }
        else
        {
            std::cerr << "创建文件夹时发生错误" << std::endl;
        }
    }
    else
    {
        std::cout << "文件夹已存在: " << folderName << std::endl;
    }
}

bool MainWindow::createCaptureDirectory() {
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
            std::cout << "文件夹创建成功: " << folderName << std::endl;
        }
        else
        {
            std::cerr << "创建文件夹时发生错误" << std::endl;
        }
    }
    else
    {
        std::cout << "文件夹已存在: " << folderName << std::endl;
    }
}



void MainWindow::getConnectedDevices(){
    for(int i = 0; i < ConnectedDevices.size(); i++){
        qDebug() << "Device[" << i << "]: " << ConnectedDevices[i].DeviceName;
        emit wsThread->sendMessageToClient("ConnectSuccess:" + ConnectedDevices[i].DeviceType + ":" + ConnectedDevices[i].DeviceName);

        if(ConnectedDevices[i].DeviceType == "MainCamera") {
            emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));
            emit wsThread->sendMessageToClient("MainCameraOffsetRange:" + QString::number(glOffsetMin) + ":" + QString::number(glOffsetMax));
            emit wsThread->sendMessageToClient("MainCameraGainRange:" + QString::number(glGainMin) + ":" + QString::number(glGainMax));

            QString CFWname;
            indi_Client->getCFWSlotName(dpMainCamera, CFWname);
            if(CFWname != "") {
                qDebug() << "get CFW Slot Name: " << CFWname;
                emit wsThread->sendMessageToClient("ConnectSuccess:CFW:" + CFWname +" (on camera)");
                isFilterOnCamera = true;

                int min, max, pos;
                indi_Client->getCFWPosition(dpMainCamera, pos, min, max);
                qDebug() << "getCFWPosition: " << min << ", " << max << ", " << pos;
                emit wsThread->sendMessageToClient("CFWPositionMax:" + QString::number(max));
            }
        }
    }
    
}

void MainWindow::clearConnectedDevices() {
    ConnectedDevices.clear();
}

void MainWindow::getStagingImage(){
    if(isStagingImage){
        emit wsThread->sendMessageToClient("SaveBinSuccess:" + SavedImage);
    }
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
    // get parameters for calculating FOV
    int FocalLength = glFocalLength;
    double CameraSize_width = glCameraSize_width;
    double CameraSize_height = glCameraSize_height;

    double Ra_Hour;
    double Dec_Degree;

    if (dpMount != NULL)
    {
        indi_Client->getTelescopeRADECJNOW(dpMount, Ra_Hour, Dec_Degree);
    }
    else
    {
        qDebug("No Mount Connect.");
        return;
    }
    double Ra_Degree = Tools::HourToDegree(Ra_Hour);

    qDebug() << "CurrentRa(Degree):" << Ra_Degree << "," << "CurrentDec(Degree):" << Dec_Degree;

    SloveResults result = Tools::PlateSolve("/dev/shm/ccd_simulator.fits", FocalLength, CameraSize_width, CameraSize_height, false);

    qDebug() << "SloveResults: " << result.RA_Degree << "," << result.DEC_Degree;

    if (result.DEC_Degree == -1 && result.RA_Degree == -1)
    {
        qDebug("Plate Solve Failur");
        return;
    }
    else
    {
        if (dpMount != NULL)
        {
            INDI::PropertyNumber property = NULL;
            // indi_Client->syncTelescopeJNow(dpMount,RadToHour(RA_Degree),RadToDegree(DEC_Degree),property);
            qDebug() << "syncTelescopeJNow | start";
            QString action = "SYNC";

            indi_Client->setTelescopeActionAfterPositionSet(dpMount, action);

            qDebug() << "DegreeToHour:" << Tools::DegreeToHour(result.RA_Degree) << "DEC_Degree:" << result.DEC_Degree;

            indi_Client->setTelescopeRADECJNOW(dpMount, Tools::DegreeToHour(result.RA_Degree), result.DEC_Degree, property);
            qDebug() << "syncTelescopeJNow | end";
            double a, b;
            indi_Client->getTelescopeRADECJNOW(dpMount, a, b);
            qDebug() << "Get_RA_Hour:" << a << "Get_DEC_Degree:" << b;
        }
        else
        {
            qDebug("No Mount Connect.");
            return;
        }
    }
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
    qDebug() << "RaDec(Hour):" << Ra_Hour << "," << Dec_Degree;

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
            qDebug() << "Mount Goto Complete!";
        } 
        else 
        {
            telescopeTimer.start(1000);  // 继续等待
        } });

    telescopeTimer.start(1000);
}

void MainWindow::DeleteImage(QStringList DelImgPath)
{
    std::string password = "quarcs"; // sudo 密码
    for (int i = 0; i < DelImgPath.size(); i++) {
        if (i < DelImgPath.size()) {
            std::ostringstream commandStream;
            commandStream << "echo '" << password << "' | sudo -S rm -rf \"./" << DelImgPath[i].toStdString() << "\"";
            std::string command = commandStream.str();

            std::cout << "Deleted command: " << commandStream.str() << std::endl;
        
            // 执行系统命令删除文件
            int result = system(command.c_str());
        
            if (result == 0) {
                std::cout << "Deleted file: " << DelImgPath[i].toStdString() << std::endl;
            } else {
                std::cerr << "Failed to delete file: " << DelImgPath[i].toStdString() << std::endl;
            }
        } else {
            std::cerr << "Index out of range: " << i << std::endl;
        }
    }
}
std::string MainWindow::GetAllFile()
{
    std::string capturePath = ImageSaveBasePath + "/CaptureImage/";
    std::string planPath = ImageSaveBasePath + "/ScheduleImage/";
    std::string resultString;
    std::string captureString = "CaptureImage{";
    std::string planString = "ScheduleImage{";
    for (const auto &entry : std::filesystem::directory_iterator(capturePath))
    {
        std::string fileName = entry.path().filename(); // 获取文件名（包含扩展名）
        captureString += fileName + ";";                // 拼接为字符串
    }
    for (const auto &entry : std::filesystem::directory_iterator(planPath))
    {
        std::string folderName = entry.path().filename().string(); // 获取文件夹名
        planString += folderName + ";";
    }
    resultString = captureString + "}:" + planString + '}';
    return resultString;
}


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
    struct statvfs stat;
    if (statvfs(usb_mount_point.toUtf8().constData(), &stat) == 0) {
        long long free_space = static_cast<long long>(stat.f_bfree) * stat.f_frsize;
        // std::cout << "U 盘剩余空间大小: " << free_space << " bytes" << std::endl;
        return free_space;
    } else {
        std::cerr << "获取 U 盘空间信息失败。" << std::endl;
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
    qDebug() << "Checking filesystem information for mount point:" << mountPointStr;

    if (statvfs(mountPointStr, &fsinfo) != 0) {
        qCritical() << "Failed to get filesystem information for" << mountPoint << ":" << strerror(errno);
        emit wsThread->sendMessageToClient(QString("getUSBFail:Failed to get filesystem information for %1, error: %2").arg(mountPoint).arg(strerror(errno)));
        return false;
    }

    qDebug() << "Filesystem flags for" << mountPoint << ":" << fsinfo.f_flag;
    return (fsinfo.f_flag & ST_RDONLY) != 0;
}


// 将文件系统挂载模式更改为读写模式
bool MainWindow::remountReadWrite(const QString& mountPoint, const QString& password) {
    QProcess process;
    process.start("sudo", {"-S", "mount", "-o", "remount,rw", mountPoint});
    if (!process.waitForStarted() || !process.write((password + "\n").toUtf8())) {
        std::cerr << "Failed to execute command: sudo mount" << std::endl;
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
        qDebug() << "Base directory does not exist.";
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
        qDebug() << "USB mount point:" << usb_mount_point;
    } else if (folderList.size() == 0)  {
        emit wsThread->sendMessageToClient("ImageSaveErroe:USB-Null");
        qDebug() << "The directory does not contain exactly one folder excluding CDROM.";
        return;
    } else {
        emit wsThread->sendMessageToClient("ImageSaveErroe:USB-Multiple");
        qDebug() << "The directory does not contain exactly one folder excluding CDROM.";
        return;
    }

    const QString password = "quarcs"; // sudo 密码

    QStorageInfo storageInfo(usb_mount_point);
    if (storageInfo.isValid() && storageInfo.isReady()) {
        if (storageInfo.isReadOnly()) {
            // 处理1: 该路径为只读设备
           if (!remountReadWrite(usb_mount_point, password)) {
            std::cerr << "Failed to remount filesystem as read-write" << std::endl;
            return;
            }
            std::cout << "Filesystem remounted as read-write successfully" << std::endl;
        }
        qDebug() << "此路径为可写设备";
    }else{
        qDebug() << "指定路径不是有效的文件系统或未准备好";
    }
    long long remaining_space = getUSBSpace(usb_mount_point);
    if (remaining_space == -1){
        std::cerr << "Check whether a USB flash drive or portable hard drive is inserted!" << std::endl;
        return ;
    }
    long long totalSize = getTotalSize(RemoveImgPath);
    if (totalSize >= remaining_space) {
        std::cout << "存储空间不足，无法复制文件到 U 盘!" << std::endl;
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
            qDebug() << "path is error!";
            return;
        }
        QString destinationPath = folderPath + fileName.mid(pos1, pos - pos1 + 1);
        QProcess process;
        process.start("sudo", {"-S", "mkdir", "-p", destinationPath});
        if (!process.waitForStarted() || !process.write((password + "\n").toUtf8())) {
            std::cerr << "Failed to execute command: sudo mkdir" << std::endl;
            emit wsThread->sendMessageToClient("HasMoveImgnNUmber:fail:" + QString::number(sumMoveImage));
            continue;
        }
        process.closeWriteChannel();
        process.waitForFinished(-1);

        process.start("sudo", {"-S", "cp", "-r", imgPath, destinationPath });
        if (!process.waitForStarted() || !process.write((password + "\n").toUtf8())) {
            std::cerr << "Failed to execute command: sudo cp" << std::endl;
            emit wsThread->sendMessageToClient("HasMoveImgnNUmber:fail:" + QString::number(sumMoveImage));
            continue;
        }
        process.closeWriteChannel();
        process.waitForFinished(-1);

        // Read the standard error output
        QByteArray stderrOutput = process.readAllStandardError();

        if (process.exitCode() == 0) {
            std::cout << "Copied file: " << imgPath.toStdString() << " to " << destinationPath.toStdString() << std::endl;
        } else {
            std::cerr << "Failed to copy file: " << imgPath.toStdString() << " to " << destinationPath.toStdString() << std::endl;
            // Print the error reason
            std::cerr << "Error: " << stderrOutput.constData() << std::endl;
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
        qDebug() << "Base directory does not exist.";
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
        qDebug() << "USB mount point:" << usb_mount_point;
        QString usbName = folderList.join(",");
        message = "USBCheck";
        long long remaining_space = getUSBSpace(usb_mount_point);
        if (remaining_space == -1){
            std::cerr << "Check whether a USB flash drive or portable hard drive is inserted!" << std::endl;
            return ;
        }
        message = message + ":" + folderList.at(0) + "," + QString::number(remaining_space);
        qDebug()<<message;
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
    qDebug("Single Solve Image");

    // 停止和清理先前的计时器
    solveTimer.stop();
    solveTimer.disconnect();

    // Tools::PlateSolve(Filename, FocalLength, CameraWidth, CameraHeight, 4656, 3522, false);

    QObject::connect(platesolveworker, &PlateSolveWorker::resultReady, [this](const SloveResults &result) {});

    platesolveworker->setParams(Filename, FocalLength, CameraWidth, CameraHeight, false); // 设置参数
    platesolveworker->start();

    solveTimer.setSingleShot(true);

    connect(&solveTimer, &QTimer::timeout, [this, Filename]()
    {
        if (Tools::isSolveImageFinish())
        {
            SloveResults result = Tools::ReadSolveResult(Filename, 1024, SolveImageScaledHeight);
            qDebug() << "Plate Solve Result(RA_Degree, DEC_Degree):" << result.RA_Degree << ", " << result.DEC_Degree;

            if (result.RA_Degree == -1 && result.DEC_Degree == -1)
            {
                qDebug("Solve image faild...");
                emit wsThread->sendMessageToClient("SolveImageFaild");
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
    qDebug("Loop Solve Image");
 
    solveTimer.stop();
    solveTimer.disconnect();

    QObject::connect(platesolveworker, &PlateSolveWorker::resultReady, [this](const SloveResults &result) {});

    platesolveworker->setParams(Filename, FocalLength, CameraWidth, CameraHeight, false); // 设置参数
    platesolveworker->start();

    // 启动等待赤道仪转动的定时器
    solveTimer.setSingleShot(true);

    connect(&solveTimer, &QTimer::timeout, [this, Filename]()
    {
        // 检查赤道仪状态
        if (Tools::isSolveImageFinish())
        {
            SloveResults result = Tools::ReadSolveResult(Filename, 1024, SolveImageScaledHeight);
            qDebug() << "Plate Solve Result(RA_Degree, DEC_Degree):" << result.RA_Degree << ", " << result.DEC_Degree;

            if (result.RA_Degree == -1 && result.DEC_Degree == -1)
            {
                qDebug("Solve image faild...");
                emit wsThread->sendMessageToClient("SolveImageFaild");
            }
            else
            {
                emit wsThread->sendMessageToClient("RealTimeSolveImageResult:" + QString::number(result.RA_Degree) + ":" + QString::number(result.DEC_Degree) + ":" + QString::number(Tools::RadToDegree(0)) + ":" + QString::number(Tools::RadToDegree(0)));
            }
        }
        else 
        {
            solveTimer.start(1000);  // 继续等待
        } 
    });

    solveTimer.start(1000);
}

void MainWindow::LoopCapture(int ExpTime) {
    // 停止和清理先前的计时器
    captureTimer.stop();
    captureTimer.disconnect();

    ShootStatus = "InProgress";
    qDebug() << "ShootStatus: " << ShootStatus;
    INDI_Capture(ExpTime);

    captureTimer.setSingleShot(true);

    connect(&captureTimer, &QTimer::timeout, [this]() {
        if (StopLoopCapture)
        {
            StopLoopCapture = false;
            INDI_AbortCapture();
            return;
        }
        // 检查赤道仪状态
        if (WaitForShootToComplete() && Tools::WaitForPlateSolveToComplete()) 
        {
            captureTimer.stop();  // 转动完成时停止定时器

            TakeNewCapture = false;
            
            // CaptureImageSave();
            // Process
            LoopCapture(glExpTime);
        } 
        else 
        {
            captureTimer.start(3000);  // 继续等待
        } 
    });

    captureTimer.start(3000);
}

void MainWindow::ClearSloveResultList() {
    SloveResultList.clear();
}

void MainWindow::RecoverySloveResul()
{
    for (const auto &result : SloveResultList)
    {
        qDebug() << "Plate Solve Result(RA_Degree, DEC_Degree):" << result.RA_Degree << ", " << result.DEC_Degree;
        emit wsThread->sendMessageToClient("SolveImageResult:" + QString::number(result.RA_Degree) + ":" + QString::number(result.DEC_Degree) + ":" + QString::number(Tools::RadToDegree(0)) + ":" + QString::number(Tools::RadToDegree(0)));
        emit wsThread->sendMessageToClient("SolveFovResult:" + QString::number(result.RA_0) + ":" + QString::number(result.DEC_0) + ":" + QString::number(result.RA_1) + ":" + QString::number(result.DEC_1) + ":" + QString::number(result.RA_2) + ":" + QString::number(result.DEC_2) + ":" + QString::number(result.RA_3) + ":" + QString::number(result.DEC_3));
    }
}
