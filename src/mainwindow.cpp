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

    readDriversListFromFiles("/usr/share/indi/drivers.xml", drivers_list, dev_groups, devices);

    Tools::InitSystemDeviceList();
    Tools::initSystemDeviceList(systemdevicelist);

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
    else if (parts.size() == 2 && parts[0].trimmed() == "ConfirmIndiDevice")
    {
        QString deviceName = parts[1].trimmed();
        // connectDevice(x);
        indi_Device_Confirm(deviceName);
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
            FocuserControl_Move(true,Steps);
        }
        else if(LR == "Right")
        {
            FocuserControl_Move(false,Steps);
        }
        else if(LR == "Target")
        {
            FocusMoveToPosition(Steps);
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
    else if (message == "focusCapture")
    {
        FocusingLooping();
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
    else if (message == "DS")
    {
        disconnectIndiServer();
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

    else if (parts.size() == 2 && parts[0].trimmed() == "MountSpeedSet")
    {
        int Speed = parts[1].trimmed().toInt();
        qDebug() << "MountSpeedSet:" << Speed;
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeSlewRate(dpMount,Speed-1);
            int Speed_;
            indi_Client->getTelescopeSlewRate(dpMount,Speed_);
            emit wsThread->sendMessageToClient("MountSetSpeedSuccess:" + QString::number(Speed_));
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
            // 曝光完成
            if(glIsFocusingLooping == false)
            {
                saveFitsAsPNG(QString::fromStdString(filename));
            }
            else
            {
                saveFitsAsJPG(QString::fromStdString(filename));
            }
            glMainCameraStatu = "Displaying";
        });
}

void MainWindow::onTimeout()
{
    ShowPHDdata();

    if(isMoving == true && dpFocuser != NULL)
    {
        CurrentPosition = FocuserControl_getPosition();
        emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(TargetPosition));

        if(CurrentPosition == TargetPosition)
        {
            isMoving = false;
            // emit wsThread->sendMessageToClient("FocusMoveDone:" + QString::number(FWHM));
            // emit wsThread->sendMessageToClient("FWHM_Result:" + QString::number(FWHM));
            qDebug("FocusMoveDone");
        }


    }
}

void MainWindow::saveFitsAsJPG(QString filename)
{
    cv::Mat image;
    cv::Mat image16;
    cv::Mat SendImage;
    Tools::readFits(filename.toLocal8Bit().constData(), image);

    if(image16.depth()==8) image.convertTo(image16,CV_16UC1,256,0); //x256  MSB alignment
    else                   image.convertTo(image16,CV_16UC1,1,0);

    FWHM_Result result = Tools::CalculateFWHM(image16);

    cv::Mat NewImage = result.image;
    FWHM = result.FWHM;

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

    // 保存新的图像带有唯一ID的文件名
    std::string fileName = "CaptureImage_" + uniqueId.toStdString() + ".jpg";
    std::string filePath = vueDirectoryPath + fileName;

    bool saved = cv::imwrite(filePath, SendImage);

    if (saved)
    {
        emit wsThread->sendMessageToClient("SaveJpgSuccess:" + QString::fromStdString(fileName));
        emit wsThread->sendMessageToClient("FocusMoveDone:" + QString::number(FWHM));

        dataPoints.append(QPointF(CurrentPosition, FWHM));

        qDebug() << "dataPoints:" << CurrentPosition << "," << FWHM;

        float a, b, c;
        Tools::fitQuadraticCurve(dataPoints, a, b, c);

        if (dataPoints.size() > 3) {
            qDebug() << "fitQuadraticCurve:" << a << b << c;

            QVector<QPointF> LineData;

            for (float x = CurrentPosition - 3000; x <= CurrentPosition + 3000; x += 10)
            {
                float y = a * x * x + b * x + c;
                LineData.append(QPointF(x, y));
            }

            QString dataString;
            for (const auto &point : LineData)
            {
                dataString += QString::number(point.x()) + "|" + QString::number(point.y()) + ":";
            }

            qDebug() << "LineData:" << dataString;

            emit wsThread->sendMessageToClient("fitQuadraticCurve:" + dataString);
        }    
    }
    else
    {
        qDebug() << "Save Image Failed...";
    }
}

int MainWindow::saveFitsAsPNG(QString fitsFileName)
{
    cv::Mat image_;
    int status = Tools::readFits(fitsFileName.toLocal8Bit().constData(), image_);

    if (status != 0)
    {
        qDebug() << "Failed to read FITS file: " << fitsFileName;
        return status;
    }

    cv::Mat image;
    if(MainCameraCFA == "MONO")
    {
        image = image_;
    } else {
        // image = colorImage(image_);  // Color image processing
        image = image_; // Original image
    }

    // 获取图像的宽度和高度
    int width = image.cols;
    int height = image.rows;

    qDebug() << "image size:" << width << "," << height;

    // 将图像调整为1920x1080的尺寸
    // cv::resize(image, image, cv::Size(4096, 2160));

    qDebug() << "image depth:" << image.depth();
    qDebug() << "image channels:" << image.channels();

    // 将图像数据复制到一维数组
    std::vector<unsigned char> imageData;  //uint16_t
    imageData.assign(image.data, image.data + image.total() * image.channels() * 2);
    qDebug() << "imageData Size:" << imageData.size() << "," << image.data + image.total() * image.channels();

    // 生成唯一ID
    QString uniqueId = QUuid::createUuid().toString();

    // 定义目录路径
    // std::string vueDirectoryPath = "/home/astro/workspace/GitClone/stellarium-web-engine_qscope/apps/web-frontend/dist/img/";
    // std::string vueDirectoryPath = "/dev/shm/";

    // 列出所有以"CaptureImage"为前缀的文件
    QDir directory(QString::fromStdString(vueDirectoryPath));
    QStringList filters;
    filters << "CaptureImage*.png" << "CaptureImage*.bin"; // 使用通配符来筛选以"CaptureImage"为前缀的png和bin文件
    QStringList fileList = directory.entryList(filters, QDir::Files);

    // 删除所有匹配的文件
    for (const auto &file : fileList)
    {
        QString filePath = QString::fromStdString(vueDirectoryPath) + file;
        QFile::remove(filePath);
    }

    // 保存新的图像带有唯一ID的文件名
    std::string fileName = "CaptureImage_" + uniqueId.toStdString() + ".png";
    std::string fileName_ = "CaptureImage_" + uniqueId.toStdString() + ".bin";
    std::string filePath = vueDirectoryPath + fileName;
    std::string filePath_ = vueDirectoryPath + fileName_;

    // 将图像数据保存到二进制文件
    std::ofstream outFile(filePath_, std::ios::binary);
    outFile.write(reinterpret_cast<const char*>(imageData.data()), imageData.size());
    outFile.close();

    bool saved = cv::imwrite(filePath, image);

    if (saved)
    {
        emit wsThread->sendMessageToClient("SaveBinSuccess:" + QString::fromStdString(fileName_));
        // emit wsThread->sendMessageToClient("SavePngSuccess:" + QString::fromStdString(fileName));
    }
    else
    {
        qDebug() << "Save Image Failed...";
    }
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

    // 保存新的图像带有唯一ID的文件名
    std::string fileName = "GuiderImage_" + uniqueId.toStdString() + ".jpg";
    std::string filePath = vueDirectoryPath + fileName;

    bool saved = cv::imwrite(filePath, Image);

    if (saved)
    {
        emit wsThread->sendMessageToClient("SaveGuiderImageSuccess:" + QString::fromStdString(fileName));
    }
    else
    {
        qDebug() << "Save GuiderImage Failed...";
    }
}

QString MainWindow::connectIndiServer()
{
    indi_Client->setConnectionTimeout(3, 0);
    indi_Client->ClearDevices(); // clear device list
    indi_Client->connectServer();
    qDebug("--------------------------------------------------connectServer");
    sleep(1);
    QString devname = indi_Client->PrintDevices();
    return devname;
}

void MainWindow::disconnectIndiServer()
{
    indi_Client->disconnectAllDevice();
    indi_Client->ClearDevices();
    indi_Client->disconnectServer();
    while (indi_Client->isServerConnected() == true)
    {
        qDebug("wait for client disconnected");
    }
    qDebug("--------------------------------------------------disconnectServer");
}

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
    }

    qDebug() << "\033[0m\033[1;35m"
             << "on_SystemPageComboBoxIndiDriver_currentIndexChanged Exist:" << isExist << "ret=" << ret << "\033[0m";

    return isExist;
}

void MainWindow::indi_Device_Confirm(QString DeviceName)
{
    //   qApp->processEvents();

    int deviceCode;
    deviceCode = systemdevicelist.currentDeviceCode;

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

    int timeout_ms = 6000;
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
        qDebug() << indi_Client->GetDeviceCount() << totalDevice;
    }
    if (t.elapsed() > timeout_ms)
        qDebug() << "System Connect | INDI connectServer | ERROR: timeout :device connected less than system device list";
    else
        qDebug() << "System Connect | Success, used time(ms):" << t.elapsed();
    indi_Client->PrintDevices();

    if (indi_Client->GetDeviceCount() == 0)
    {
        qDebug() << "System Connect | Error:No device found";
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
                //   call_phd_whichCamera(systemdevicelist.system_devices[index].dp->getDeviceName());  // PHD2 Guider Connect
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
    timeout_ms = 6000 * indi_Client->GetDeviceCount();
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

    if (t.elapsed() > timeout_ms)
        qDebug() << "System Connect | ERROR: Connect time exceed (ms):" << timeout_ms << t.elapsed();
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

        indi_Client->setBLOBMode(B_ALSO, dpMainCamera->getDeviceName(), nullptr);
        indi_Client->enableDirectBlobAccess(dpMainCamera->getDeviceName(), nullptr);
        QString SDKVERSION;
        indi_Client->getCCDSDKVersion(dpMainCamera, SDKVERSION);
        qDebug() << "AfterAllConnected | MainCamera SDK version" << SDKVERSION;
        int X, Y;
        // int glMainCCDSizeX,glMainCCDSizeY;
        indi_Client->getCCDFrameInfo(dpMainCamera, X, Y, glMainCCDSizeX, glMainCCDSizeY);
        qDebug() << "CCDSize:" << glMainCCDSizeX << glMainCCDSizeY;
        emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));
        // m_pToolbarWidget->CaptureView->Camera_Width = glMainCCDSizeX;
        // m_pToolbarWidget->CaptureView->Camera_Height = glMainCCDSizeY;
        int offsetX, offsetY;
        indi_Client->getCCDCFA(dpMainCamera, offsetX, offsetY, MainCameraCFA);
        qDebug() << "getCCDCFA:" << MainCameraCFA << offsetX << offsetY;
    }

    if (dpMount != NULL)
    {
        emit wsThread->sendMessageToClient("ConnectSuccess:Mount:" + QString::fromUtf8(dpMount->getDeviceName()));
        QString DevicePort; // add by CJQ 2023.3.3
        indi_Client->getDevicePort(dpMount, DevicePort);

        //????
        double glLongitude_radian, glLatitude_radian;
        glLongitude_radian = Tools::getDecAngle("116° 14' 53.91");
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
        emit wsThread->sendMessageToClient("TelescopeTotalSlewRate:" + QString::number(total));
        indi_Client->getTelescopeMaxSlewRateOptions(dpMount, minspeed, maxspeed, speedvalue);
        indi_Client->setTelescopeMaxSlewRateOptions(dpMount, total - 1);
        indi_Client->setTelescopeSlewRate(dpMount, total - 1);
        int speed;
        indi_Client->getTelescopeSlewRate(dpMount,speed);
        emit wsThread->sendMessageToClient("TelescopeCurrentSlewRate:" + QString::number(speed));
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
    }

    if (dpFocuser != NULL)
    {
        emit wsThread->sendMessageToClient("ConnectSuccess:Focuser:" + QString::fromUtf8(dpFocuser->getDeviceName()));
        indi_Client->GetAllPropertyName(dpFocuser);
        indi_Client->syncFocuserPosition(dpFocuser, 0);
    }
}

void MainWindow::disconnectIndiServer(MyClient *client)
{
    client->disconnectAllDevice();
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

void MainWindow::INDI_Capture(int Exp_times)
{
    glIsFocusingLooping = false;
    double expTime_sec;
    expTime_sec = (double)Exp_times / 1000;
    qDebug() << "expTime_sec:" << expTime_sec;

    if (dpMainCamera)
    {
        glMainCameraStatu = "Exposuring";
        qDebug() << "INDI_Capture:" << glMainCameraStatu;

        glMainCameraCaptureTimer.start();

        // indi_Client->setCCDFrameInfo(dpMainCamera,100,100,100,100);
        int value, min, max;
        indi_Client->getCCDGain(dpMainCamera, value, min, max);
        qDebug() << "CameraGain:" << value << min << max;
        indi_Client->getCCDOffset(dpMainCamera, value, min, max);
        qDebug() << "CameraOffset:" << value << min << max;

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

        glMainCameraStatu = "FocusExp";
        qDebug() << "FocusingLooping:" << glMainCameraStatu;

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
    strechShowImage(img16, CFA, true, true, 0, 0, 65535, 1.0, 1.7, 100, true);
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

    if (glPHD_StarX != 0 && glPHD_StarY != 0)
        glPHD_StartGuide = true;

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
                emit wsThread->sendMessageToClient("InGuiding");
            }
            else
            {
                // m_pToolbarWidget->LabelMainStarBox->setStyleSheet("QLabel{border:2px solid rgb(255,255,0);border-radius:3px;background-color:transparent;}");
                // m_pToolbarWidget->LabelCrossX->setStyleSheet("QLabel{border:1px solid rgb(255,255,0);border-radius:3px;background-color:transparent;}");
                // m_pToolbarWidget->LabelCrossY->setStyleSheet("QLabel{border:1px solid rgb(255,255,0);border-radius:3px;background-color:transparent;}");
                emit wsThread->sendMessageToClient("InCalibration");
            }

            if (StarLostAlert == true)
            {
                // m_pToolbarWidget->LabelMainStarBox->setStyleSheet("QLabel{border:2px solid rgb(255,0,0);border-radius:3px;background-color:transparent;}");
                // m_pToolbarWidget->LabelCrossX->setStyleSheet("QLabel{border:1px solid rgb(255,0,0);border-radius:3px;background-color:transparent;}");
                // m_pToolbarWidget->LabelCrossY->setStyleSheet("QLabel{border:1px solid rgb(255,0,0);border-radius:3px;background-color:transparent;}");
                emit wsThread->sendMessageToClient("StarLostAlert");
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
    qDebug() << "\033[32m"
             << "ControlGuide: "
             << "\033[0m" << Direction << "," << Duration;
    switch (Direction)
    {
    case 1:
    {
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeGuideNS(dpMount, Direction, Duration);
        }
        break;
    }
    case 0:
    {
        if (dpMount != NULL)
        {
            indi_Client->setTelescopeGuideNS(dpMount, Direction, Duration);
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
        std::cout << "\033[31m"
                  << "PHD2ControlTelescope: "
                  << "\033[0m" << sdk_num << "," << sdk_direction << ","
                  << sdk_duration << std::endl;
    }
    if (sdk_duration != 0)
    {
        MainWindow::ControlGuide(sdk_direction, sdk_duration);

        memcpy(sharedmemory_phd + mem_offset_sdk_num, &zero, sizeof(int));

        call_phd_ChackControlStatus(sdk_num); // set pFrame->ControlStatus = 0;
    }
}

void MainWindow::FocusMoveToPosition(int position)
{
    // ((MainWindow* )this->topLevelWidget()->children()[1]->children()[2]->children()[1])->FocuserControl_MoveToPosition(position);
    isMoving = true;
    TargetPosition = AutoMovePosition;
    qDebug() << "TargetPosition: " << TargetPosition;
}

void MainWindow::FocuserControl_Move(bool isInward, int steps)
{
  if (dpFocuser != NULL) 
  {
    isMoving = true;

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
  }
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
