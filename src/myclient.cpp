#include "myclient.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <QDebug>
#include <QObject>
// #include <qhyccd.h>

#include <fitsio.h>
#include "tools.h"

// #include "/usr/include/libindi/indiccd.h"
#include "QThread"
#include "QElapsedTimer"
#include "QTimeZone"

// #define SUDO

/**************************************************************************************
**
***************************************************************************************/
// MyClient::MyClient(HomePageWidget *gui)
MyClient::MyClient()
{

    //   pMain = gui;
    Logger::Log("indi_client | MyClient::MyClent", LogLevel::INFO, DeviceType::MAIN);
    //   qInfo()<<"MyClient::MyClent"<<gethostid()<<getPort()<<getegid()<<geteuid();
}

//************************************************************************
//*********************** event ******************************************
//************************************************************************

void MyClient::newMessage(INDI::BaseDevice baseDevice, int messageID)
{
    // qDebug("[INDI SERVER] %s", baseDevice.messageQueue(messageID).c_str());

    std::string message = baseDevice->messageQueue(messageID);
    receiveMessage(message);
}

void MyClient::newProperty(INDI::Property property)
{
    // if (!baseDevice.isDeviceNameMatch("Simple CCD"))
    //    return;

    // qDebug() << "newProperty: " << property->getName();
    // qDebug("Recveing message from Server %s", baseDevice.messageQueue(messageID).c_str());
}

void MyClient::newDevice(INDI::BaseDevice baseDevice)
{
    // the new Device is a callback function , the connect server will trig it. it can be override here

    // Logger::Log("indi_client | newDevice", LogLevel::INFO, DeviceType::MAIN);

    const char *DeviceName = baseDevice.getDeviceName();

    AddDevice(baseDevice, baseDevice.getDeviceName());

    // Logger::Log("indi_client | newDevice | +++++++++++++++++++++++++++++++++", LogLevel::INFO, DeviceType::MAIN);
}

void MyClient::updateProperty(INDI::Property property)
{
    if (property.getType() == INDI_BLOB)
    {
        // CaptureTestTime = CaptureTestTimer.elapsed();
        // qDebug() << "\033[32m" << "Exposure completed:" << CaptureTestTime << "milliseconds" << "\033[0m";
        // qInfo() << "Exposure completed:" << CaptureTestTime << "milliseconds";
        // CaptureTestTimer.invalidate();

        // qInfo("Recveing image from Server size len name label format %d %d %s %s %s", property.getBLOB()->bp->size,property.getBLOB()->bp->bloblen,property.getBLOB()->bp->name,property.getBLOB()->bp->label,property.getBLOB()->bp->format);

        // std::ofstream myfile;
        // std::string filename="/dev/shm/ccd_simulator.fits";
        // myfile.open(filename, std::ios::out | std::ios::binary);
        // myfile.write(static_cast<char *>(property.getBLOB()->bp->blob), property.getBLOB()->bp->bloblen);
        // myfile.close();

        // QString devname_;
        // Tools::readFitsHeadForDevName(filename,devname_);
        // std::string devname = devname_.toStdString();

        // receiveImage(filename, devname);
    }
    else if (property.getType() == INDI_TEXT)
    {
        auto tvp = property.getText();
        if (tvp->isNameMatch("CCD_FILE_PATH"))
        {
            auto filepath = tvp->findWidgetByName("FILE_PATH");
            if (filepath)
            {
                Logger::Log("indi_client | updateProperty | New Capture Image Save To" + QString(filepath->getText()).toStdString(), LogLevel::INFO, DeviceType::CAMERA);
                // qDebug() << "\033[32m" << "New Capture Image Save To" << QString(filepath->getText()) << "\033[0m";
                // qInfo() << "New Capture Image Save To" << QString(filepath->getText());

                CaptureTestTime = CaptureTestTimer.elapsed();
                Logger::Log("indi_client | updateProperty | Exposure completed:" + std::to_string(CaptureTestTime) + "ms", LogLevel::INFO, DeviceType::CAMERA);
                CaptureTestTimer.invalidate();

                QString devname_;
                Tools::readFitsHeadForDevName(QString(filepath->getText()).toStdString(), devname_);
                std::string devname = devname_.toStdString();

                receiveImage(QString(filepath->getText()).toStdString(), devname);
                Logger::Log("indi_client | updateProperty | receiveImage | " + QString(filepath->getText()).toStdString() + ", " + devname, LogLevel::INFO, DeviceType::CAMERA);
            }
        }
    }
    else if (property.getType() == INDI_NUMBER)
    {
    }
}

//************************ device list management***********************************

void MyClient::AddDevice(INDI::BaseDevice *device, const std::string &name)
{
    // 修复：确保deviceList和deviceNames保持同步
    // 检查两个列表的大小是否一致
    if (deviceList.size() != deviceNames.size()) {
        Logger::Log("indi_client | AddDevice | Warning: deviceList and deviceNames size mismatch, fixing...", LogLevel::WARNING, DeviceType::MAIN);
        // 同步两个列表的大小，取较小的值
        size_t minSize = std::min(deviceList.size(), deviceNames.size());
        deviceList.resize(minSize);
        deviceNames.resize(minSize);
    }
    
    for (int i = 0; i < deviceNames.size() && i < deviceList.size(); i++)
    {
        if (deviceNames[i] == name)
        {
            // 修复：检查指针有效性
            if (deviceList[i] != nullptr && deviceList[i]->isConnected())
            {
                // 如果设备已经连接，跳过这个设备
                return;
            }
            else
            {
                // 如果设备没有连接，替换这个设备
                deviceList[i] = device;
                return;
            }
        }
    }
    // 如果没有找到同名的设备，添加新设备
    // 确保同时添加到两个列表，保持同步
    deviceList.push_back(device);
    deviceNames.push_back(name);
    Logger::Log("indi_client | newDevice | New DeviceName:" + std::string(name) + ", GetDeviceCount:" + std::to_string(GetDeviceCount()), LogLevel::INFO, DeviceType::MAIN);
}

void MyClient::RemoveDevice(const std::string &name)
{
    int index = -1;
    for (int i = 0; i < deviceNames.size(); i++)
    {
        if (deviceNames[i] == name)
        {
            index = i;
            break;
        }
    }

    if (index >= 0)
    {
        deviceList.erase(deviceList.begin() + index);
        deviceNames.erase(deviceNames.begin() + index);
    }
}

int MyClient::GetDeviceCount() const
{
    return deviceList.size();
}

void MyClient::ClearDevices()
{
    deviceList.clear();
    deviceNames.clear();
}

QString MyClient::PrintDevices()
{
    // qDebug() << "\033[1;36m--------- INDI Device List ---------\033[0m";
    // qInfo() << "--------- INDI Device List ---------";
    Logger::Log(" --------- INDI Device List ---------", LogLevel::INFO, DeviceType::MAIN);
    QString dev;
    if (deviceNames.size() == 0)
    {
        Logger::Log("indi_client | PrintDevices | no device exist", LogLevel::INFO, DeviceType::MAIN);
    }

    else
    {
        for (int i = 0; i < deviceNames.size(); i++)
        {
            // 修复：检查deviceList和deviceNames是否同步，以及指针是否有效
            if (i >= deviceList.size()) {
                Logger::Log("indi_client | PrintDevices | Warning: deviceList size mismatch at index " + std::to_string(i), LogLevel::WARNING, DeviceType::MAIN);
                break;
            }
            
            if (deviceList[i] == nullptr) {
                Logger::Log("indi_client | PrintDevices | Warning: deviceList[" + std::to_string(i) + "] is nullptr", LogLevel::WARNING, DeviceType::MAIN);
                // 继续处理其他设备，但不访问空指针
                if (i > 0) {
                    dev.append("|");
                }
                dev.append(deviceNames[i].c_str());
                dev.append(":");
                dev.append(QString::number(i));
                continue;
            }
            
            std::string logMessage = "indi_client | PrintDevices | Device " + std::to_string(i) + ": " + deviceNames[i] + " (Driver: " + deviceList[i]->getDriverExec() + ")";
            Logger::Log(logMessage, LogLevel::INFO, DeviceType::MAIN);
            if (i > 0)
            {
                dev.append("|"); // 添加分隔符
            }
            dev.append(deviceNames[i].c_str()); // 添加序号
            dev.append(":");
            dev.append(QString::number(i)); // 添加deviceNames元素
        }
    }
    // Logger::Log("indi_client | PrintDevices | ------------------------------------", LogLevel::INFO, DeviceType::MAIN);
    return dev;
}

INDI::BaseDevice *MyClient::GetDeviceFromList(int index)
{
    // 这个函数接受一个整型参数 index，表示要返回的设备在列表中的位置。如果 index 超出了列表的范围，函数返回 nullptr。否则，函数返回 deviceList 数组中对应位置的设备指针。
    if (index < 0 || index >= deviceList.size())
    {
        return nullptr;
    }
    return deviceList[index];
}

INDI::BaseDevice *MyClient::GetDeviceFromListWithName(std::string devName)
{

    for (int i = 0; i < deviceList.size(); i++)
    {
        if (deviceNames[i] == devName)
            return deviceList[i];
    }

    // if not found return null
    return nullptr;
}

std::string MyClient::GetDeviceNameFromList(int index)
{
    if (index < 0 || index >= deviceNames.size())
    {
        return "";
    }
    return deviceNames[index];
}

// void MyClient::disconnectAllDevice(void){
//     //disconnect all device in the device list
//     INDI::BaseDevice *dp;
//     qDebug()<<"disconnectAllDevice"<<deviceList.size();
//     PrintDevices();
//     for(int i=0;i<GetDeviceCount();i++){
//           dp=GetDeviceFromList(i);
//           if(dp->isConnected()) {
//               disconnectDevice(dp->getDeviceName());
//               qDebug()<<"disconnectAllDevice |"<<dp->getDeviceName()<<dp->isConnected();
//           }
//     }
// }

void MyClient::disconnectAllDevice(void)
{
    // disconnect all device in the device list
    //  INDI::BaseDevice *dp;
    QVector<INDI::BaseDevice *> dp;
    Logger::Log("indi_client | disconnectAllDevice", LogLevel::INFO, DeviceType::MAIN);
    PrintDevices();
    for (int i = 0; i < GetDeviceCount(); i++)
    {
        dp.append(GetDeviceFromList(i));
        if (dp[i]->isConnected())
        {
            disconnectDevice(dp[i]->getDeviceName());
            while (dp[i]->isConnected())
            {
                Logger::Log("indi_client | disconnectAllDevice | Waiting for disconnect finish...", LogLevel::INFO, DeviceType::MAIN);
                sleep(1);
            }
            Logger::Log("indi_client | disconnectAllDevice | " + std::string(dp[i]->getDeviceName()) + " " + std::to_string(dp[i]->isConnected()), LogLevel::INFO, DeviceType::MAIN);
        }
    }
}

// need to wait the connection completely finished then call this , otherwise it may not output all
void MyClient::listAllProperties(INDI::BaseDevice *dp)
{
    std::vector<INDI::Property> properties(dp->getProperties().begin(), dp->getProperties().end());

    // Iterate over the list of properties and print the names of the PropertyNumber properties
    for (INDI::Property *property : properties)
    {
        // INDI::PropertyNumber *numberProperty = static_cast<INDI::PropertyNumber *>(property);
        if (property != nullptr)
        {
            std::cout << property->getName() << std::endl;
        }
    }
}

void MyClient::GetAllPropertyName(INDI::BaseDevice *dp)
{
    // 直接使用范围for循环遍历属性
    for (const auto &property : dp->getProperties())
    {
        const char *propertyName = property->getName();
        const char *propertyType = PropertyTypeToString(property->getType());
        // 在此处处理属性
        // propertyName 包含了属性的名称
        Logger::Log("indi_client | GetAllPropertyName | " + std::string(propertyName) + ", " + std::string(propertyType), LogLevel::INFO, DeviceType::MAIN);
    }
}

const char *MyClient::PropertyTypeToString(INDI_PROPERTY_TYPE type)
{
    // 使用一个自定义函数将属性类型枚举值转换为对应的字符串
    switch (type)
    {
    case INDI_NUMBER: /*!< INumberVectorProperty. */
        return "Number";
    case INDI_SWITCH: /*!< ISwitchVectorProperty. */
        return "Switch";
    case INDI_TEXT: /*!< ITextVectorProperty. */
        return "Text";
    case INDI_LIGHT: /*!< ILightVectorProperty. */
        return "Light";
    case INDI_BLOB: /*!< IBLOBVectorProperty. */
        return "Blob";
    case INDI_UNKNOWN:
        return "Unknown";
    default:
        return "Unknown";
    }
}

uint32_t MyClient::setBaudRate(INDI::BaseDevice *dp, int baudRate)
{
    INDI::PropertySwitch baudRateProperty = dp->getProperty("DEVICE_BAUD_RATE");
    if (!baudRateProperty.isValid())
    {
        Logger::Log("indi_client | setBaudRate | Error: unable to find DEVICE_BAUD_RATE property...", LogLevel::WARNING, DeviceType::MAIN);
        return QHYCCD_ERROR;
    }
    if (baudRate == 9600)
    {
        baudRateProperty[0].setState(ISS_ON);
        baudRateProperty[1].setState(ISS_OFF);
        baudRateProperty[2].setState(ISS_OFF);
        baudRateProperty[3].setState(ISS_OFF);
        baudRateProperty[4].setState(ISS_OFF);
        baudRateProperty[5].setState(ISS_OFF);
    }
    else if (baudRate == 19200)
    {
        baudRateProperty[0].setState(ISS_OFF);
        baudRateProperty[1].setState(ISS_ON);
        baudRateProperty[2].setState(ISS_OFF);
        baudRateProperty[3].setState(ISS_OFF);
        baudRateProperty[4].setState(ISS_OFF);
        baudRateProperty[5].setState(ISS_OFF);
    }
    else if (baudRate == 38400)
    {
        baudRateProperty[0].setState(ISS_OFF);
        baudRateProperty[1].setState(ISS_OFF);
        baudRateProperty[2].setState(ISS_ON);
        baudRateProperty[3].setState(ISS_OFF);
        baudRateProperty[4].setState(ISS_OFF);
        baudRateProperty[5].setState(ISS_OFF);
    }
    else if (baudRate == 57600)
    {
        baudRateProperty[0].setState(ISS_OFF);
        baudRateProperty[1].setState(ISS_OFF);
        baudRateProperty[2].setState(ISS_OFF);
        baudRateProperty[3].setState(ISS_ON);
        baudRateProperty[4].setState(ISS_OFF);
        baudRateProperty[5].setState(ISS_OFF);
    }
    else if (baudRate == 115200)
    {
        baudRateProperty[0].setState(ISS_OFF);
        baudRateProperty[1].setState(ISS_OFF);
        baudRateProperty[2].setState(ISS_OFF);
        baudRateProperty[3].setState(ISS_OFF);
        baudRateProperty[4].setState(ISS_ON);
    }
    else if (baudRate == 230400)
    {
        baudRateProperty[0].setState(ISS_OFF);
        baudRateProperty[1].setState(ISS_OFF);
        baudRateProperty[2].setState(ISS_OFF);
        baudRateProperty[3].setState(ISS_OFF);
        baudRateProperty[4].setState(ISS_OFF);
        baudRateProperty[5].setState(ISS_ON);
    }else{
        Logger::Log("indi_client | setBaudRate | Invalid baud rate " + std::to_string(baudRate), LogLevel::ERROR, DeviceType::MAIN);
        return QHYCCD_ERROR;
    }
    sendNewProperty(baudRateProperty);
    Logger::Log("indi_client | setBaudRate | " + std::to_string(baudRate), LogLevel::INFO, DeviceType::MAIN);
    return QHYCCD_SUCCESS;
}
/**************************************************************************************
**                                  CCD API
***************************************************************************************/

uint32_t MyClient::setTemperature(INDI::BaseDevice *dp, double value)
{
    char *propertyName = "CCD_TEMPERATURE";
    INDI::PropertyNumber ccdTemperature = dp->getProperty(propertyName);

    if (!ccdTemperature.isValid())
    {
        Logger::Log("indi_client | setTemperature | Error: unable to find CCD_TEMPERATURE property...", LogLevel::WARNING, DeviceType::MAIN);
        return QHYCCD_ERROR;
    }

    Logger::Log("indi_client | setTemperature | Setting temperature to " + std::to_string(value) + " C.", LogLevel::INFO, DeviceType::MAIN);
    ccdTemperature[0].setValue(value);
    sendNewProperty(ccdTemperature);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getTemperature(INDI::BaseDevice *dp, double &value)
{

    char *propertyName = "CCD_TEMPERATURE";
    INDI::PropertyNumber ccdTemperature = dp->getProperty(propertyName);

    if (!ccdTemperature.isValid())
    {
        // qDebug("Error: unable to find CCD_TEMPERATURE property...");
        return QHYCCD_ERROR;
    }

    // qDebug("getting temperature  %g C.", value);

    value = ccdTemperature->np[0].value;

    return QHYCCD_SUCCESS;
}

// 辅助：将一个 Switch 向量中的指定条目置 ON（其余 OFF），并发送
static bool setSwitchOneOf(INDI::BaseDevice *dp,
                           const char *propName,
                           std::initializer_list<const char *> wantedEntries,
                           std::function<void(const INDI::PropertySwitch &)> sender,
                           int wait_ms = 800, int retries = 5)
{
    INDI::PropertySwitch sw = dp->getProperty(propName);
    if (!sw.isValid())
        return false;

    // 先全部 OFF
    for (size_t i = 0; i < sw.size(); ++i)
        sw[i].setState(ISS_OFF);

    // 匹配目标项
    bool found = false;
    for (const char *want : wantedEntries)
    {
        for (size_t i = 0; i < sw.size(); ++i)
        {
            std::string name = sw[i].getName();
            std::string low = name;
            std::transform(low.begin(), low.end(), low.begin(), ::tolower);

            std::string w = want;
            std::transform(w.begin(), w.end(), w.begin(), ::tolower);

            if (low == w || low.find(w) != std::string::npos)
            {
                sw[i].setState(ISS_ON);
                found = true;
                break;
            }
        }
        if (found)
            break;
    }

    if (!found)
        return false;

    sender(sw); // e.g. sendNewProperty(sw) or client->sendNewSwitch(sw)

    // 简单回执等待（轮询是否已生效）
    for (int k = 0; k < retries; ++k)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        INDI::PropertySwitch cur = dp->getProperty(propName);
        if (!cur.isValid())
            break;

        // 若我们置 ON 的那一项仍然是 ON，则认为成功
        for (const char *want : wantedEntries)
        {
            for (size_t i = 0; i < cur.size(); ++i)
            {
                std::string low = cur[i].getName();
                std::transform(low.begin(), low.end(), low.begin(), ::tolower);

                std::string w = want;
                std::transform(w.begin(), w.end(), w.begin(), ::tolower);

                if ((low == w || low.find(w) != std::string::npos) &&
                    cur[i].getState() == ISS_ON)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

uint32_t MyClient::disableDSLRLiveView(INDI::BaseDevice *dp)
{
    if (!dp)
        return QHYCCD_ERROR;

    // --- 更可靠的 DSLR 判定：看关键属性是否存在 ---
    const bool maybeCanon =
        dp->getProperty("viewfinder").isValid() ||
        dp->getProperty("eosmoviemode").isValid() ||
        dp->getProperty("CCD_VIDEO_STREAM").isValid() ||
        dp->getProperty("AUX_VIDEO_STREAM").isValid();

    if (!maybeCanon)
        return QHYCCD_SUCCESS;

    Logger::Log("indi_client | disableDSLRLiveView | Canon-like device detected, stopping streams & LV", LogLevel::INFO, DeviceType::CAMERA);

    auto sendSwitch = [this](const INDI::PropertySwitch &p)
    { this->sendNewProperty(p); };

    // 1) 先停所有视频流（避免忙碌）
    setSwitchOneOf(dp, "CCD_VIDEO_STREAM", {"off", "stop"}, sendSwitch);
    setSwitchOneOf(dp, "AUX_VIDEO_STREAM", {"off", "stop"}, sendSwitch);
    setSwitchOneOf(dp, "RECORD_STREAM", {"off", "stop"}, sendSwitch);

    // 2) 关闭 LiveView（indi_canon_ccd 真正的属性名是小写 viewfinder）
    setSwitchOneOf(dp, "viewfinder", {"off"}, sendSwitch);

    // 3) 关闭电影模式（否则快门会被固件占用）
    setSwitchOneOf(dp, "eosmoviemode", {"off"}, sendSwitch);

    // 4) 确保是拍照输出而非视频：格式、目标与上传模式
    //    注意：属性真实名有 "CCD_" 前缀
    setSwitchOneOf(dp, "CCD_CAPTURE_FORMAT", {"raw", "jpeg", "image", "native"}, sendSwitch);
    setSwitchOneOf(dp, "CCD_CAPTURE_TARGET", {"pc", "client"}, sendSwitch);
    setSwitchOneOf(dp, "UPLOAD_MODE", {"client", "both"}, sendSwitch);

    // 5) 回传 BLOB（否则你会感觉“没拍照”）
    setSwitchOneOf(dp, "CCD_FORCE_BLOB", {"on"}, sendSwitch);

    // 6) 可选的安全项
    setSwitchOneOf(dp, "uilock", {"off"}, sendSwitch);
    setSwitchOneOf(dp, "popupflash", {"off"}, sendSwitch);

    Logger::Log("indi_client | disableDSLRLiveView | LiveView/Movie disabled, capture set to PC/Client", LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::takeExposure(INDI::BaseDevice *dp, double seconds)
{


    INDI::PropertyNumber ccdExposure = dp->getProperty("CCD_EXPOSURE");

    if (!ccdExposure.isValid())
    {
        Logger::Log("indi_client | takeExposure | Error: unable to find CCD Simulator CCD_EXPOSURE property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    CaptureTestTimer.start();
    Logger::Log("indi_client | takeExposure | Exposure start.", LogLevel::DEBUG, DeviceType::CAMERA);

    // Take a 1 second exposure
    Logger::Log("indi_client | takeExposure | Taking a " + std::to_string(seconds) + " second exposure.", LogLevel::DEBUG, DeviceType::CAMERA);
    ccdExposure[0].setValue(seconds);
    sendNewProperty(ccdExposure);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCCDAbortExposure(INDI::BaseDevice *dp)
{
    INDI::PropertySwitch ccdabort = dp->getProperty("CCD_ABORT_EXPOSURE");

    if (!ccdabort.isValid())
    {
        Logger::Log("indi_client | setCCDAbortExposure | Error: unable to find  CCD_ABORT_EXPOSURE property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    //  ccdabort[0].setValue(1); //?? need to be confirmed with Jasem
    ccdabort[0].setState(ISS_ON);
    sendNewProperty(ccdabort);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getCCDFrameInfo(INDI::BaseDevice *dp, int &X, int &Y, int &WIDTH, int &HEIGHT)
{
    INDI::PropertyNumber ccdFrameInfo = dp->getProperty("CCD_FRAME");

    if (!ccdFrameInfo.isValid())
    {
        Logger::Log("indi_client | getCCDFrameInfo | Error: unable to find CCD Simulator ccdFrameInfo property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    X = ccdFrameInfo->np[0].value;
    Y = ccdFrameInfo->np[1].value;
    WIDTH = ccdFrameInfo->np[2].value;
    HEIGHT = ccdFrameInfo->np[3].value;

    Logger::Log("indi_client | getCCDFrameInfo | " + std::to_string(X) + ", " + std::to_string(Y) + ", " + std::to_string(WIDTH) + ", " + std::to_string(HEIGHT), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCCDFrameInfo(INDI::BaseDevice *dp, int X, int Y, int WIDTH, int HEIGHT)
{
    INDI::PropertyNumber ccdFrameInfo = dp->getProperty("CCD_FRAME");

    if (!ccdFrameInfo.isValid())
    {
        Logger::Log("indi_client | setCCDFrameInfo | Error: unable to find CCD Simulator ccdFrameInfo property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    Logger::Log("indi_client | setCCDFrameInfo | " + std::to_string(X) + ", " + std::to_string(Y) + ", " + std::to_string(WIDTH) + ", " + std::to_string(HEIGHT), LogLevel::DEBUG, DeviceType::CAMERA);

    ccdFrameInfo[0].setValue(X);
    ccdFrameInfo[1].setValue(Y);
    ccdFrameInfo[2].setValue(WIDTH);
    ccdFrameInfo[3].setValue(HEIGHT);
    sendNewProperty(ccdFrameInfo);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::resetCCDFrameInfo(INDI::BaseDevice *dp)
{
    INDI::PropertySwitch resetFrameInfo = dp->getProperty("CCD_FRAME_RESET");

    if (!resetFrameInfo.isValid())
    {
        Logger::Log("indi_client | resetCCDFrameInfo | Error: unable to find resetCCDFrameInfo property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    resetFrameInfo[0].setState(ISS_ON);
    // resetFrameInfo[0].setState(ISS_OFF);  //?? if need to set back?
    sendNewProperty(resetFrameInfo);
    resetFrameInfo[0].setState(ISS_OFF);
    sendNewProperty(resetFrameInfo);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCCDCooler(INDI::BaseDevice *dp, bool enable)
{
    INDI::PropertySwitch ccdCooler = dp->getProperty("CCD_COOLER");

    if (!ccdCooler.isValid())
    {
        Logger::Log("indi_client | setCCDCooler | Error: unable to find CCD_COOLER property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    Logger::Log("indi_client | setCCDCooler | " + std::to_string(enable), LogLevel::INFO, DeviceType::CAMERA);

    if (enable == false)
        ccdCooler[0].setState(ISS_OFF);
    else
        ccdCooler[0].setState(ISS_ON);

    sendNewProperty(ccdCooler);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getCCDCooler(INDI::BaseDevice *dp, bool &enable)
{
    INDI::PropertySwitch ccdCooler = dp->getProperty("CCD_COOLER");

    if (!ccdCooler.isValid())
    {
        Logger::Log("indi_client | getCCDCooler | Error: unable to find CCD_COOLER property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    Logger::Log("indi_client | getCCDCooler | " + std::to_string(ccdCooler[0].getState()), LogLevel::INFO, DeviceType::CAMERA);

    if (ccdCooler[0].getState() == ISS_OFF)
        enable = false;
    else
        enable = true;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getCCDBasicInfo(INDI::BaseDevice *dp, int &maxX, int &maxY, double &pixelsize, double &pixelsizX, double &pixelsizY, int &bitDepth)
{
    INDI::PropertyNumber ccdInfo = dp->getProperty("CCD_INFO");

    if (!ccdInfo.isValid())
    {
        Logger::Log("indi_client | getCCDBasicInfo | Error: unable to find  CCD_INFO property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    maxX = ccdInfo->np[0].value;
    maxY = ccdInfo->np[1].value;
    pixelsize = ccdInfo->np[2].value;
    pixelsizX = ccdInfo->np[3].value;
    pixelsizY = ccdInfo->np[4].value;
    bitDepth = ccdInfo->np[5].value;
    Logger::Log("indi_client | getCCDBasicInfo | " + std::to_string(maxX) + ", " + std::to_string(maxY) + ", " + std::to_string(pixelsize) + ", " + std::to_string(pixelsizX) + ", " + std::to_string(pixelsizY) + ", " + std::to_string(bitDepth), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCCDBasicInfo(INDI::BaseDevice *dp, int maxX, int maxY, double pixelsize, double pixelsizX, double pixelsizY, int bitDepth)
{
    INDI::PropertyNumber ccdInfo = dp->getProperty("CCD_INFO");

    if (!ccdInfo.isValid())
    {
        Logger::Log("indi_client | setCCDBasicInfo | Error: unable to find  CCD_INFO property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    ccdInfo->np[0].value = maxX;
    ccdInfo->np[1].value = maxY;
    ccdInfo->np[2].value = pixelsize;
    ccdInfo->np[3].value = pixelsizX;
    ccdInfo->np[4].value = pixelsizY;
    ccdInfo->np[5].value = bitDepth;
    Logger::Log("indi_client | setCCDBasicInfo | " + std::to_string(maxX) + ", " + std::to_string(maxY) + ", " + std::to_string(pixelsize) + ", " + std::to_string(pixelsizX) + ", " + std::to_string(pixelsizY) + ", " + std::to_string(bitDepth), LogLevel::INFO, DeviceType::CAMERA);

    sendNewProperty(ccdInfo);

    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getCCDBinning(INDI::BaseDevice *dp, int &BINX, int &BINY, int &BINXMAX, int &BINYMAX)
{
    INDI::PropertyNumber ccdbinning = dp->getProperty("CCD_BINNING");

    if (!ccdbinning.isValid())
    {
        Logger::Log("indi_client | getCCDBinning | Error: unable to find  CCD_BINNING property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    BINX = ccdbinning->np[0].value;
    BINY = ccdbinning->np[1].value;
    BINXMAX = ccdbinning->np[0].max;
    BINYMAX = ccdbinning->np[1].max;

    Logger::Log("indi_client | getCCDBinning | " + std::to_string(BINX) + ", " + std::to_string(BINY) + ", " + std::to_string(BINXMAX) + ", " + std::to_string(BINYMAX), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCCDBinnign(INDI::BaseDevice *dp, int BINX, int BINY)
{
    INDI::PropertyNumber ccdbinning = dp->getProperty("CCD_BINNING");

    if (!ccdbinning.isValid())
    {
        Logger::Log("indi_client | setCCDBinnign | Error: unable to find  CCD_BINNING property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    ccdbinning[0].setValue(BINX);
    ccdbinning[1].setValue(BINY);
    sendNewProperty(ccdbinning);
    Logger::Log("indi_client | setCCDBinnign | " + std::to_string(BINX) + ", " + std::to_string(BINY), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getCCDCFA(INDI::BaseDevice *dp, int &offsetX, int &offsetY, QString &CFATYPE)
{
    INDI::PropertyText ccdCFA = dp->getProperty("CCD_CFA");

    if (!ccdCFA.isValid())
    {
        Logger::Log("indi_client | getCCDCFA | Error: unable to find  CCD_CFA property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    std::string a, b, c;

    a = ccdCFA[0].getText();
    b = ccdCFA[1].getText();
    c = ccdCFA[2].getText();

    offsetX = std::stoi(a);
    offsetY = std::stoi(b);
    CFATYPE = QString::fromStdString(c);
    Logger::Log("indi_client | getCCDCFA | " + std::to_string(offsetX) + ", " + std::to_string(offsetY) + ", " + CFATYPE.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getCCDSDKVersion(INDI::BaseDevice *dp, QString &version)
{
    INDI::PropertyText ccdCFA = dp->getProperty("SDK_VERSION");

    if (!ccdCFA.isValid())
    {
        Logger::Log("indi_client | getCCDSDKVersion | Error: unable to find  SDK_VERSION property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    std::string a;

    a = ccdCFA[0].getText();

    version = QString::fromStdString(a);
    // qDebug()<<version;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getCCDGain(INDI::BaseDevice *dp, int &value, int &min, int &max)
{
    INDI::PropertyNumber ccdgain = dp->getProperty("CCD_GAIN");

    if (!ccdgain.isValid())
    {
        Logger::Log("indi_client | getCCDGain | Error: unable to find  CCD_GAIN property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    value = ccdgain->np[0].value;
    min = ccdgain->np[0].min;
    max = ccdgain->np[0].max;

    Logger::Log("indi_client | getCCDGain | " + std::to_string(value) + ", " + std::to_string(min) + ", " + std::to_string(max), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCCDGain(INDI::BaseDevice *dp, int value)
{
    INDI::PropertyNumber ccdgain = dp->getProperty("CCD_GAIN");

    if (!ccdgain.isValid())
    {
        Logger::Log("indi_client | setCCDGain | Error: unable to find  CCD_BINNING property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    ccdgain[0].setValue(value);
    sendNewProperty(ccdgain);

    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getCCDOffset(INDI::BaseDevice *dp, int &value, int &min, int &max)
{
    INDI::PropertyNumber ccdoffset = dp->getProperty("CCD_OFFSET");

    if (!ccdoffset.isValid())
    {
        Logger::Log("indi_client | getCCDOffset | Error: unable to find  CCD_OFFSET property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    value = ccdoffset->np[0].value;
    min = ccdoffset->np[0].min;
    max = ccdoffset->np[0].max;

    Logger::Log("indi_client | getCCDOffset | " + std::to_string(value) + ", " + std::to_string(min) + ", " + std::to_string(max), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCCDOffset(INDI::BaseDevice *dp, int value)
{
    INDI::PropertyNumber ccdoffset = dp->getProperty("CCD_OFFSET");

    if (!ccdoffset.isValid())
    {
        Logger::Log("indi_client | setCCDOffset | Error: unable to find  CCD_OFFSET property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    ccdoffset[0].setValue(value);
    sendNewProperty(ccdoffset);

    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getCCDReadMode(INDI::BaseDevice *dp, int &value, int &min, int &max)
{
    INDI::PropertyNumber ccdreadmode = dp->getProperty("READ_MODE");

    if (!ccdreadmode.isValid())
    {
        Logger::Log("indi_client | getCCDReadMode | Error: unable to find  READ_MODE property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    value = ccdreadmode->np[0].value;
    min = ccdreadmode->np[0].min;
    max = ccdreadmode->np[0].max;

    Logger::Log("indi_client | getCCDReadMode | " + std::to_string(value) + ", " + std::to_string(min) + ", " + std::to_string(max), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCCDReadMode(INDI::BaseDevice *dp, int value)
{
    INDI::PropertyNumber ccdreadmode = dp->getProperty("READ_MODE");

    if (!ccdreadmode.isValid())
    {
        Logger::Log("indi_client | setCCDReadMode | Error: unable to find  READ_MODE property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    ccdreadmode[0].setValue(value);
    sendNewProperty(ccdreadmode);

    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCCDUploadModeToLacal(INDI::BaseDevice *dp)
{
    INDI::PropertySwitch uploadmode = dp->getProperty("UPLOAD_MODE");

    if (!uploadmode.isValid())
    {
        Logger::Log("indi_client | setCCDUploadModeToLacal | Error: unable to find UPLOAD_MODE property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    uploadmode[0].setState(ISS_OFF);
    uploadmode[1].setState(ISS_ON);
    uploadmode[2].setState(ISS_OFF);

    sendNewProperty(uploadmode);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCCDUpload(INDI::BaseDevice *dp, QString Dir, QString Prefix)
{
    INDI::PropertyText upload = dp->getProperty("UPLOAD_SETTINGS");

    if (!upload.isValid())
    {
        Logger::Log("indi_client | setCCDUpload | Error: unable to find UPLOAD_SETTINGS property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    upload[0].setText(Dir.toLatin1().data());
    upload[1].setText(Prefix.toLatin1().data());

    sendNewProperty(upload);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::StartWatch(INDI::BaseDevice *dp)
{

    Logger::Log("indi_client | StartWatch | start", LogLevel::INFO, DeviceType::CAMERA);
    // wait for the availability of the device
    watchDevice(dp->getDeviceName(), [this](INDI::BaseDevice device)
                {

        // wait for the availability of the "CONNECTION" property
        device.watchProperty("CONNECTION", [this](INDI::Property)
        {
            Logger::Log("indi_client | StartWatch | Connect to INDI Driver...", LogLevel::INFO, DeviceType::CAMERA);

            //connectDevice("Simple CCD");
        });

        // wait for the availability of the "CCD_TEMPERATURE" property
        device.watchProperty("CCD_TEMPERATURE", [this](INDI::PropertyNumber property)
        {

           // if (dp->isConnected())
           // {
                Logger::Log("indi_client | StartWatch | CCD_TEMPERATURE event ", LogLevel::INFO, DeviceType::CAMERA);

                //setTemperature(-20);
           // }

            // call lambda function if property changed
            property.onUpdate([property, this]()
            {
                Logger::Log("indi_client | StartWatch | Receving new CCD Temperature: " + std::to_string(property[0].getValue()) + " C", LogLevel::INFO, DeviceType::CAMERA);
                if (property[0].getValue() == -20)
                {
                    Logger::Log("indi_client | StartWatch | CCD temperature reached desired value!", LogLevel::INFO, DeviceType::CAMERA);

                }
            });
        });

        // wait for the availability of the "CCD1" property
        device.watchProperty("CCD1", [](INDI::PropertyBlob property)
        {
            // call lambda function if property changed
            property.onUpdate([property]()
            {
                // Save FITS file to disk
                std::ofstream myfile;
#ifdef SUDO
                system("sudo rm ccd_simulator.fits");
#else
                system("rm ccd_simulator.fits");
#endif

                myfile.open("ccd_simulator.fits", std::ios::out | std::ios::binary);
                myfile.write(static_cast<char *>(property[0].getBlob()), property[0].getBlobLen());
                myfile.close();

                Logger::Log("indi_client | StartWatch | Received image, saved as ccd_simulator.fits", LogLevel::INFO, DeviceType::CAMERA);
            });
        }); });

    Logger::Log("indi_client | StartWatch | finish", LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

/**************************************************************************************
**                                  Mount API
***************************************************************************************/

uint32_t MyClient::getMountInfo(INDI::BaseDevice *dp, QString &version)
{
    INDI::PropertyText mountInfo = dp->getProperty("DRIVER_INFO");
    if (!mountInfo.isValid() || mountInfo->ntp <= 0)
    {
        Logger::Log("indi_client | getMountInfo | Error: unable to find DRIVER_INFO or empty", LogLevel::WARNING, DeviceType::MOUNT);
        return QHYCCD_ERROR;
    }

    int chosenIdx = -1; // 用于提取版本项
    // 附加字段：便于注释式打印
    std::string drvName, drvExec, drvVersion, drvInterface;
    for (int i = 0; i < mountInfo->ntp; i++)
    {
        const char *name = mountInfo->tp[i].name;
        const char *text = mountInfo->tp[i].text;

        // std::ostringstream oss;
        // oss << "indi_client | getMountInfo | DRIVER_INFO"
        //     << " | tp[" << i << "]"
        //     << " name:" << (name ? name : "")
        //     << " text:" << (text ? text : "")
        //     << " label:" << (mountInfo->label ? mountInfo->label : "")
        //     << " group:" << (mountInfo->group ? mountInfo->group : "")
        //     << " perm:" << static_cast<int>(mountInfo->p)
        //     << " state:" << static_cast<int>(mountInfo->s)
        //     << " ntp:" << mountInfo->ntp;
        // Logger::Log(oss.str(), LogLevel::INFO, DeviceType::MOUNT);

        if (chosenIdx == -1 && name)
        {
            std::string nm(name);
            if (nm.find("VERSION") != std::string::npos)
                chosenIdx = i;
        }

        // 抽取常见字段
        if (name)
        {
            std::string nm(name);
            if (nm == "DRIVER_NAME")
                drvName = text ? text : "";
            else if (nm == "DRIVER_EXEC")
                drvExec = text ? text : "";
            else if (nm == "DRIVER_VERSION")
                drvVersion = text ? text : "";
            else if (nm == "DRIVER_INTERFACE")
                drvInterface = text ? text : "";
        }
    }

    if (chosenIdx >= 0)
        version = QString::fromUtf8(mountInfo->tp[chosenIdx].text ? mountInfo->tp[chosenIdx].text : "");
    else
        version.clear();

    // 注释式汇总打印，清晰展示关键值
    // {
    //     std::ostringstream sum;
    //     sum << "indi_client | getMountInfo | Summary"
    //         << " | DriverName:" << drvName
    //         << " | Exec:" << drvExec
    //         << " | Version:" << (!drvVersion.empty() ? drvVersion : version.toStdString())
    //         << " | Interface:" << drvInterface;
    //     Logger::Log(sum.str(), LogLevel::INFO, DeviceType::MOUNT);
    // }

    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setAutoFlip(INDI::BaseDevice *dp, bool ON)
{
    INDI::PropertySwitch flip = dp->getProperty("AutoFlip");

    if (!flip.isValid())
    {
        Logger::Log("indi_client | setCCDAbortExposure | Error: unable to find  CCD_ABORT_EXPOSURE property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    //  ccdabort[0].setValue(1); //?? need to be confirmed with Jasem
    if (ON)
    {
        flip[0].setState(ISS_OFF);
        flip[1].setState(ISS_ON);
        //   Logger::Log("flip[0].name =" + std::to_string(flip[0].getName().c_str()),LogLevel::WARNING, DeviceType::CAMERA);
        //   Logger::Log("flip[0].name =" + std::to_string(flip[0].getName().c_str()),LogLevel::WARNING, DeviceType::CAMERA);
        qDebug() << "flip[0].name =" << flip[0].getName();
        qDebug() << "flip[1].name =" << flip[1].getName();
        // Logger::Log("indi_client | setCCDAbortExposure | Error: unable to find  CCD_ABORT_EXPOSURE property...", LogLevel::WARNING, DeviceType::CAMERA);
    }
    else
    {
        flip[0].setState(ISS_ON);
        flip[1].setState(ISS_OFF);
        // Logger::Log("indi_client | takeExposure | Taking a " + std::to_string(seconds) + " second exposure.", LogLevel::INFO, DeviceType::CAMERA);
    }

    sendNewProperty(flip);

    if (auto mpm = dp->getNumber("Minutes Past Meridian"); mpm.isValid())
    {
        Logger::Log("indi_client | Minutes Past Meridian 属性内容：", LogLevel::INFO, DeviceType::CAMERA);

        for (int i = 0; i < mpm.size(); i++)
        {
            auto &n = mpm[i];

            Logger::Log("  名称: " + std::string(n.getName()),
                        LogLevel::INFO, DeviceType::CAMERA);
            Logger::Log("  标签: " + std::string(n.getLabel()),
                        LogLevel::INFO, DeviceType::CAMERA);
            Logger::Log("  当前值: " + QString::number(n.getValue(), 'f', 2).toStdString(),
                        LogLevel::INFO, DeviceType::CAMERA);
            Logger::Log("  最小值: " + QString::number(n.getMin(), 'f', 2).toStdString(),
                        LogLevel::INFO, DeviceType::CAMERA);
            Logger::Log("  最大值: " + QString::number(n.getMax(), 'f', 2).toStdString(),
                        LogLevel::INFO, DeviceType::CAMERA);
            Logger::Log("  步长: " + QString::number(n.getStep(), 'f', 2).toStdString(),
                        LogLevel::INFO, DeviceType::CAMERA);
        }
    }
    // setMinutesPastMeridian(dp,10.0,10.0);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::startFlip(INDI::BaseDevice *dp)
{
    if (mountState.isMovingNow())
    {
        setTelescopeAbortMotion(dp);
        sleep(2);
    };
    // 目标：若接近/触发物理边界，先在当前 PierSide 方向“回退”一点，再执行 goto 触发自动路径规划的翻转
    double raHours = 0.0, decDeg = 0.0;
    if (getTelescopeRADECJNOW(dp, raHours, decDeg) != QHYCCD_SUCCESS)
    {
        Logger::Log("indi_client | startFlip | Error: cannot read RA/DEC", LogLevel::WARNING, DeviceType::MOUNT);
        return QHYCCD_ERROR;
    }

    mountState.Flip_RA_Hours = raHours;
    mountState.Flip_DEC_Degree = decDeg;

    // 先执行回退，回退到目标RA/DEC的1度范围内
    if (flipBack(dp, raHours, decDeg) != QHYCCD_SUCCESS)
    {
        Logger::Log("indi_client | startFlip | Error: flip back failed", LogLevel::WARNING, DeviceType::MOUNT);
        return QHYCCD_ERROR;
    }

    return QHYCCD_SUCCESS;
}

// 中天翻转回退
uint32_t MyClient::flipBack(INDI::BaseDevice *dp, double raHours, double decDeg)
{
    mountState.isFlipBacking = true;
    QString pierSide = "UNKNOWN";
    getTelescopePierSide(dp, pierSide); // "EAST" / "WEST" / "UNKNOWN"

    // 回退角度（RA）：2°（≈0.1333h），足以脱离 ±1 分钟的子午线限制
    constexpr double kBackoffDeg = 2.0;
    constexpr double kBackoffH = kBackoffDeg / 15.0; // 小时

    auto norm24 = [](double h)
    { h = fmod(h, 24.0); if (h < 0) h += 24.0; return h; };

    // 默认回退策略：
    // - WEST 侧：RA += backoff（HA 通常为负，增加 RA 令 HA 更负，远离中天）
    // - EAST  侧：RA -= backoff（HA 通常为正，减少 RA 令 HA 更正，远离中天）
    // - UNKNOWN：若可读 TIME_LST，则按 HA 符号回退；否则保守采用 EAST 策略
    double raBackoff = raHours;
    if (pierSide == "WEST")
    {
        raBackoff = norm24(raHours + kBackoffH);
    }
    else if (pierSide == "EAST")
    {
        raBackoff = norm24(raHours - kBackoffH);
    }
    else
    {
        // UNKNOWN：尝试读取 TIME_LST 来判定 HA 符号
        auto toHours = [&](double v) -> double
        {
            if (std::isnan(v))
                return v;
            double x = v;
            if (std::fabs(x) > 24.0 && std::fabs(x) <= 86400.0)
                x /= 3600.0; // 秒→小时
            if (std::fabs(x) > 24.0 && std::fabs(x) <= 360.0)
                x /= 15.0; // 度→小时
            if (std::fabs(x) > 360.0)
                x = fmod(x, 360.0) / 15.0;
            return norm24(x);
        };
        INDI::PropertyNumber lst = dp->getProperty("TIME_LST");
        double lstH = std::numeric_limits<double>::quiet_NaN();
        if (lst.isValid() && lst.size() > 0)
            lstH = toHours(lst[0].getValue());
        if (!std::isnan(lstH))
        {
            auto wrap12 = [](double h)
            { while (h < -12.0) h += 24.0; while (h >= 12.0) h -= 24.0; return h; };
            const double ha = wrap12(lstH - raHours);
            const double sign = (ha >= 0.0) ? +1.0 : -1.0; // 令 |HA| 变大
            raBackoff = norm24(raHours - sign * kBackoffH);
        }
        else
        {
            // 兜底按 EAST 策略
            raBackoff = norm24(raHours - kBackoffH);
        }
    }

    // 可选：提升到最高速以尽快回退（忽略失败）
    int totalRates = 0;
    if (getTelescopeTotalSlewRate(dp, totalRates) == QHYCCD_SUCCESS && totalRates > 0)
    {
        setTelescopeSlewRate(dp, totalRates);
    }

    // 1) 回退到 raBackoff/decDeg（不强制追踪）
    if (slewTelescopeJNowNonBlock(dp, raBackoff, decDeg, false) != QHYCCD_SUCCESS)
    {
        Logger::Log("indi_client | startFlip | Error: backoff slew failed", LogLevel::WARNING, DeviceType::MOUNT);
        return QHYCCD_ERROR;
    }

    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setMinutesPastMeridian(INDI::BaseDevice *dp, double Eastvalue, double Westvalue)
{
    INDI::PropertyNumber mpm = dp->getProperty("Minutes Past Meridian");
    if (!mpm.isValid())
    {
        Logger::Log("indi_client | setMinutesPastMeridian | Error: unable to find  Minutes Past Meridian property...", LogLevel::WARNING, DeviceType::MOUNT);
        return QHYCCD_ERROR;
    }
    mpm[0].setValue(Eastvalue);
    mpm[1].setValue(Westvalue);
    sendNewProperty(mpm);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getMinutesPastMeridian(INDI::BaseDevice *dp, double &Eastvalue, double &Westvalue)
{
    INDI::PropertyNumber mpm = dp->getProperty("Minutes Past Meridian");
    if (!mpm.isValid())
    {
        Logger::Log("indi_client | getMinutesPastMeridian | Error: unable to find  Minutes Past Meridian property...", LogLevel::WARNING, DeviceType::MOUNT);
        return QHYCCD_ERROR;
    }
    Eastvalue = mpm[0].getValue();
    Westvalue = mpm[1].getValue();
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setAUXENCODERS(INDI::BaseDevice *dp)
{
    INDI::PropertySwitch encoders = dp->getProperty("AUXENCODER");

    if (!encoders.isValid())
    {
        Logger::Log("indi_client | setAUXENCODERS | Error: unable to find  AUXENCODER property...", LogLevel::WARNING, DeviceType::MOUNT);
        return QHYCCD_ERROR;
    }

    encoders[0].setState(ISS_OFF);
    encoders[1].setState(ISS_ON);
    sendNewProperty(encoders);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getTelescopeInfo(INDI::BaseDevice *dp, double &telescope_aperture, double &telescope_focal, double &guider_aperature, double &guider_focal)
{
    INDI::PropertyNumber property = dp->getProperty("TELESCOPE_INFO");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getTelescopeInfo | Error: unable to find  TELESCOPE_INFO property...", LogLevel::WARNING, DeviceType::MOUNT);
        return QHYCCD_ERROR;
    }

    telescope_aperture = property->np[0].value;
    telescope_focal = property->np[1].value;
    guider_aperature = property->np[2].value;
    guider_focal = property->np[3].value;
    Logger::Log("indi_client | getTelescopeInfo | " + std::to_string(telescope_aperture) + ", " + std::to_string(telescope_focal) + ", " + std::to_string(guider_aperature) + ", " + std::to_string(guider_focal), LogLevel::INFO, DeviceType::MOUNT);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeInfo(INDI::BaseDevice *dp, double telescope_aperture, double telescope_focal, double guider_aperature, double guider_focal)
{
    INDI::PropertyNumber property = dp->getProperty("TELESCOPE_INFO");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setTelescopeInfo | Error: unable to find  TELESCOPE_INFO property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    property[0].setValue(telescope_aperture);
    property[1].setValue(telescope_focal);
    property[2].setValue(guider_aperature);
    property[3].setValue(guider_focal);
    sendNewProperty(property);

    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getTelescopePierSide(INDI::BaseDevice *dp, QString &side)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_PIER_SIDE");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getTelescopePierSide | Error: unable to find TELESCOPE_PIER_SIDE property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    if (property[0].getState() == ISS_ON)
        side = "WEST";
    else if (property[1].getState() == ISS_ON)
        side = "EAST";

    // qDebug() << "getTelescopePierSide" << side ;
    return QHYCCD_SUCCESS;
}

/*
// 未在mainwindow.cpp中使用的函数 - 已注释
uint32_t MyClient::getTelescopeTrackRate(INDI::BaseDevice *dp, QString &rate)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_TRACK_RATE");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getTelescopeTrackRate | Error: unable to find  TELESCOPE_TRACK_RATE property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    if (property[0].getState() == ISS_ON)
        rate = "SIDEREAL";
    else if (property[1].getState() == ISS_ON)
        rate = "SOLAR";
    else if (property[2].getState() == ISS_ON)
        rate = "LUNAR"; //??
    else if (property[3].getState() == ISS_ON)
        rate = "CUSTOM";
    Logger::Log("indi_client | getTelescopeTrackRate | " + rate.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}
*/

uint32_t MyClient::setTelescopeTrackRate(INDI::BaseDevice *dp, QString rate)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_TRACK_RATE");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setTelescopeTrackRate | Error: unable to find  TELESCOPE_TRACK_RATE property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    if (rate == "SIDEREAL")
    {
        property[0].setState(ISS_ON);
        property[1].setState(ISS_OFF);
        property[2].setState(ISS_OFF);
        property[3].setState(ISS_OFF);
    }
    else if (rate == "SOLAR")
    {
        property[0].setState(ISS_OFF);
        property[1].setState(ISS_ON);
        property[2].setState(ISS_OFF);
        property[3].setState(ISS_OFF);
    }
    else if (rate == "LUNAR")
    {
        property[0].setState(ISS_OFF);
        property[1].setState(ISS_OFF);
        property[2].setState(ISS_ON);
        property[3].setState(ISS_OFF);
    }
    else if (rate == "CUSTOM")
    {
        property[0].setState(ISS_OFF);
        property[1].setState(ISS_OFF);
        property[2].setState(ISS_OFF);
        property[3].setState(ISS_ON);
    }

    sendNewProperty(property);
    // qDebug()<<"setTelescopeTrackRate"<< value;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getTelescopeTrackEnable(INDI::BaseDevice *dp, bool &enable)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_TRACK_STATE");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getTelescopeTrackEnable | Error: unable to find  TELESCOPE_TRACK_STATE property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    if (property[0].getState() == ISS_ON)
    {
        enable = true;
        mountState.isTracking = true;
    }
    else if (property[1].getState() == ISS_ON)
    {
        enable = false;
        mountState.isTracking = false;
    }

    // Logger::Log("indi_client | getTelescopeTrackEnable | " + std::to_string(enable), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeTrackEnable(INDI::BaseDevice *dp, bool enable)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_TRACK_STATE");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setTelescopeTrackEnable | Error: unable to find  TELESCOPE_TRACK_STATE property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    if (enable == true)
    {
        property[0].setState(ISS_ON);
        property[1].setState(ISS_OFF);
        mountState.isTracking = true;
    }
    else if (enable == false)
    {
        property[0].setState(ISS_OFF);
        property[1].setState(ISS_ON);
        mountState.isTracking = false;
    }

    sendNewProperty(property);

    Logger::Log("indi_client | setTelescopeTrackEnable | " + std::to_string(enable), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeParkOption(INDI::BaseDevice *dp, QString option)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_PARK_OPTION");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setTelescopeParkOption | Error: unable to find  TELESCOPE_PARK_OPTION property...", LogLevel::WARNING, DeviceType::MOUNT);
        return QHYCCD_ERROR;
    }

    if (option == "CURRENT")
    {
        property[0].setState(ISS_ON);
    }
    else if (option == "DEFAULT")
    {
        property[1].setState(ISS_ON);
    }
    else if (option == "WRITE")
    {
        property[2].setState(ISS_ON);
    }

    sendNewProperty(property);
    Logger::Log("indi_client | setTelescopeParkOption | " + option.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

/*
// 未在mainwindow.cpp中使用的函数 - 已注释
uint32_t MyClient::getTelescopeParkPosition(INDI::BaseDevice *dp, double &RA_DEGREE, double &DEC_DEGREE)
{
    INDI::PropertyNumber property = dp->getProperty("TELESCOPE_PARK_POSITION");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getTelescopeParkPosition | Error: unable to find  TELESCOPE_PARK_POSITION property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    RA_DEGREE = property->np[0].value;
    DEC_DEGREE = property->np[1].value;
    Logger::Log("indi_client | getTelescopeParkPosition | " + std::to_string(RA_DEGREE) + ", " + std::to_string(DEC_DEGREE), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}
*/
/*
// 未在mainwindow.cpp中使用的函数 - 已注释
uint32_t MyClient::setTelescopeParkPosition(INDI::BaseDevice *dp, double RA_DEGREE, double DEC_DEGREE)
{
    INDI::PropertyNumber property = dp->getProperty("TELESCOPE_PARK_POSITION");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setTelescopeParkPosition | Error: unable to find  TELESCOPE_PARK_POSITION property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    property->np[0].value = RA_DEGREE;
    property->np[1].value = DEC_DEGREE;

    sendNewProperty(property);
    Logger::Log("indi_client | setTelescopeParkPosition | " + std::to_string(RA_DEGREE) + ", " + std::to_string(DEC_DEGREE), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}
*/
uint32_t MyClient::getTelescopePark(INDI::BaseDevice *dp, bool &isParked)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_PARK");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getTelescopePark | Error: unable to find TELESCOPE_PARK property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }
    if (property[0].getState() == ISS_ON)
    {
        isParked = true;
        mountState.isParked = true;
    }
    else if (property[1].getState() == ISS_ON)
    {
        isParked = false;
        mountState.isParked = false;
    }

    // Logger::Log("indi_client | getTelescopePark | " + std::to_string(isParked), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopePark(INDI::BaseDevice *dp, bool isParked)
{
    if (mountState.isMovingNow())
    {
        Logger::Log("indi_client | setTelescopePark | Telescope is moving, abort motion...", LogLevel::INFO, DeviceType::CAMERA);
        setTelescopeAbortMotion(dp);
    }
    // 固定驻车状态为固定到当前位置
    setTelescopeParkOption(dp, "CURRENT");

    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_PARK");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setTelescopePark | Error: unable to find TELESCOPE_PARK property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    if (isParked == false)
    {
        // 取消驻车
        property[1].setState(ISS_ON);
        property[0].setState(ISS_OFF);
        // 启用跟踪
        setTelescopeTrackEnable(dp, true);
        mountState.isParked = false;
    }
    else
    {
        // 驻车
        property[0].setState(ISS_ON);
        property[1].setState(ISS_OFF);
        // 停止跟踪
        setTelescopeTrackEnable(dp, false);
        mountState.isParked = true;
    }
    sendNewProperty(property);

    Logger::Log("indi_client | setTelescopePark | " + std::to_string(isParked), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeHomeInit(INDI::BaseDevice *dp, QString command)
{
    // ---------- 优先尝试 HOME_INIT（Switch） ----------
    INDI::PropertySwitch prop = dp->getProperty("HOME_INIT");
    if (prop.isValid())
    {
        Logger::Log("indi_client | setTelescopeHomeInit | HOME_INIT found",
                    LogLevel::INFO, DeviceType::CAMERA);

        // SLEW 到 Home 时的前置限制
        if ((command == "RETURN_HOME" || command == "AT_HOME") && (mountState.isMovingNow() || mountState.isParked))
        {
            Logger::Log("indi_client | setTelescopeHomeInit | Telescope is moving or parked, return...",
                        LogLevel::INFO, DeviceType::CAMERA);
            return QHYCCD_ERROR;
        }

        // 先全部关
        for (int i = 0; i < prop->nsp; ++i)
            prop[i].setState(ISS_OFF);

        if (command == "SLEWHOME" || command == "RETURN_HOME")
        {
            prop[0].setState(ISS_ON);
            sendNewProperty(prop);

            mountState.isHoming = true; // 标记正在回零

            Logger::Log(QString("indi_client | setTelescopeHomeInit | %1 via HOME_INIT")
                            .arg(command)
                            .toUtf8()
                            .constData(),
                        LogLevel::INFO, DeviceType::CAMERA);

            // === 每秒打印一次 HOME_INIT 状态 ===
            QTimer *timer = new QTimer();
            QObject::connect(timer, &QTimer::timeout, [dp, timer, this]()
                             {
                // mountState.printCurrentState();
                INDI::PropertyNumber eq = dp->getProperty("EQUATORIAL_EOD_COORD");
                if (eq.isValid()) {
                    sleep(1);
                    if (eq.getState() == IPS_OK || eq.getState() == IPS_IDLE){
                        timer->stop();
                        QObject::disconnect(timer, &QTimer::timeout, nullptr, nullptr);
                        mountState.isHoming = false;
                        Logger::Log("indi_client | setTelescopeHomeInit | HOME_INIT completed",
                                    LogLevel::INFO, DeviceType::CAMERA);
                        // 释放 timer 内存，避免内存泄漏
                        timer->deleteLater();
                        return;
                    }
                }else{
                    Logger::Log("indi_client | setTelescopeHomeInit | EQUATORIAL_EOD_COORD not found",
                                LogLevel::WARNING, DeviceType::CAMERA);
                    QObject::disconnect(timer, &QTimer::timeout, nullptr, nullptr);
                    mountState.isHoming = false;
                    // 释放 timer 内存，避免内存泄漏
                    timer->deleteLater();
                    return ;
                } });
            timer->start(1000); // 1秒一次

            return QHYCCD_SUCCESS;
        }
        else if (command == "SYNCHOME" || command == "AT_HOME")
        {
            prop[1].setState(ISS_ON);
            sendNewProperty(prop);
            // mountState.isHoming = true; // 标记正在回零
            Logger::Log(QString("indi_client | setTelescopeHomeInit | %1 via HOME_INIT")
                            .arg(command)
                            .toUtf8()
                            .constData(),
                        LogLevel::INFO, DeviceType::CAMERA);
            return QHYCCD_SUCCESS;
        }

        // HOME_INIT 存在但缺少目标项 → 回退
        Logger::Log(QString("indi_client | setTelescopeHomeInit | HOME_INIT missing target item: %1, fallback...")
                        .arg(command)
                        .toUtf8()
                        .constData(),
                    LogLevel::WARNING, DeviceType::CAMERA);
    }
    else
    {
        Logger::Log("indi_client | setTelescopeHomeInit | HOME_INIT not found, fallback...",
                    LogLevel::DEBUG, DeviceType::CAMERA);
    }

    // ---------- 回退：用 GOTO 实现 ----------
    if (command == "SLEWHOME" || command == "RETURN_HOME")
    {
        if (mountState.isMovingNow() || mountState.isParked)
        {
            Logger::Log("indi_client | setTelescopeHomeInit | Telescope is moving or parked, return...",
                        LogLevel::INFO, DeviceType::MOUNT);
            return QHYCCD_ERROR;
        }

        mountState.updateHomeRAHours(mountState.Latitude_Degree, mountState.Longitude_Degree);
        mountState.isHoming = true;
        if (mountState.isTracking)
        {
            uint32_t result = slewTelescopeJNowNonBlock(dp, mountState.Home_RA_Hours, mountState.Home_DEC_Degree, true);
            if (result != QHYCCD_SUCCESS)
            {
                Logger::Log("indi_client | setTelescopeHomeInit | Fallback: RETURN_HOME by RA/DEC goto failed",
                            LogLevel::ERROR, DeviceType::MOUNT);
                mountState.isHoming = false;
                return QHYCCD_ERROR;
            }
        }
        else
        {
            uint32_t result = slewTelescopeJNowNonBlock(dp, mountState.Home_RA_Hours, mountState.Home_DEC_Degree, false);
            if (result != QHYCCD_SUCCESS)
            {
                Logger::Log("indi_client | setTelescopeHomeInit | Fallback: RETURN_HOME by RA/DEC goto failed",
                            LogLevel::ERROR, DeviceType::MOUNT);
                mountState.isHoming = false;
                return QHYCCD_ERROR;
            }
        }

        Logger::Log("indi_client | setTelescopeHomeInit | Fallback: RETURN_HOME by RA/DEC goto",
                    LogLevel::INFO, DeviceType::MOUNT);
        return QHYCCD_SUCCESS;
    }
    else if (command == "SYNCHOME" || command == "AT_HOME")
    {
        mountState.updateHomeRAHours(mountState.Latitude_Degree, mountState.Longitude_Degree);
        syncTelescopeJNow(dp, mountState.Home_RA_Hours, mountState.Home_DEC_Degree);
        Logger::Log("indi_client | setTelescopeHomeInit | Fallback: AT_HOME by saving current RA/DEC",
                    LogLevel::INFO, DeviceType::MOUNT);
        return QHYCCD_SUCCESS;
    }

    Logger::Log("indi_client | setTelescopeHomeInit | Error: invalid command (post-fallback)",
                LogLevel::ERROR, DeviceType::MOUNT);
    return QHYCCD_ERROR;
}

uint32_t MyClient::getTelescopeMoving(INDI::BaseDevice *dp)
{
    INDI::PropertyNumber eq = dp->getProperty("EQUATORIAL_EOD_COORD");
    if (eq.isValid())
    {
        if (eq.getState() == IPS_OK || eq.getState() == IPS_IDLE)
        {
            mountState.isMoving = false;
        }
        else
        {
            mountState.isMoving = true;
        }
    }
    else
    {
        Logger::Log("indi_client | getTelescopeMoving | Error: unable to find EQUATORIAL_EOD_COORD property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }
    return QHYCCD_SUCCESS;
}

// uint32_t MyClient::setTelescopeHomeInit(INDI::BaseDevice *dp, QString command)
// {
//     // 判断command是否为SLEWHOME或SYNCHOME
//     if (command == "SLEWHOME")
//     {

//         if (mountState.isMovingNow() || mountState.isParked){
//             Logger::Log("indi_client | setTelescopeHomeInit | Telescope is moving or parked, return...", LogLevel::INFO, DeviceType::CAMERA);
//             return QHYCCD_ERROR;
//         }
//         mountState.isHoming = true;
//         setTelescopeRADECJNOW(dp, mountState.Home_RA_Hours, mountState.Home_DEC_Degree);
//         Logger::Log("indi_client | setTelescopeHomeInit | SLEWHOME command sent", LogLevel::INFO, DeviceType::CAMERA);
//     }
//     else if (command == "SYNCHOME")
//     {
//         double currentRA;
//         double currentDEC;
//         getTelescopeRADECJNOW(dp, currentRA, currentDEC);
//         Logger::Log("indi_client | setTelescopeHomeInit | SYNCHOME command sent", LogLevel::INFO, DeviceType::CAMERA);
//         mountState.Home_RA_Hours = currentRA;
//         mountState.Home_DEC_Degree = currentDEC;
//     }
//     else
//     {
//         Logger::Log("indi_client | setTelescopeHomeInit | Error: invalid command", LogLevel::ERROR, DeviceType::CAMERA);
//         return QHYCCD_ERROR;
//     }
//     return QHYCCD_SUCCESS;
// }

uint32_t MyClient::getTelescopeSlewRate(INDI::BaseDevice *dp, int &speed)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_SLEW_RATE");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getTelescopeSlewRate | Error: unable to find TELESCOPE_SLEW_RATE property...", LogLevel::WARNING, DeviceType::MOUNT);
        return QHYCCD_ERROR;
    }

    for (int i = 0; i < property.count(); i++)
    {
        if (property[i].getState() == ISS_ON)
        {
            speed = i + 1;
            Logger::Log("indi_client | getTelescopeSlewRate | " + std::to_string(speed), LogLevel::INFO, DeviceType::MOUNT);
            if (speed >= 1 && speed <= property.count())
                Logger::Log("indi_client | getTelescopeSlewRate | " + std::to_string(speed) + " " + property[speed - 1].getLabel(), LogLevel::INFO, DeviceType::MOUNT);
            return QHYCCD_SUCCESS;
        }
    }

    Logger::Log("indi_client | getTelescopeSlewRate | Error: not found slew rate", LogLevel::ERROR, DeviceType::MOUNT);
    speed = -1;
    return QHYCCD_ERROR;
}

// uint32_t MyClient::getTelescopeSlewRate(INDI::BaseDevice *dp,int &speed)
// {
//     INDI::PropertySwitch property = dp->getProperty("TELESCOPE_SLEW_RATE");

//     if (!property.isValid())
//     {
//         Logger::Log("indi_client | getTelescopeSlewRate | Error: unable to find TELESCOPE_SLEW_RATE property...", LogLevel::WARNING, DeviceType::MOUNT);
//         return QHYCCD_ERROR;
//     }

//     if(property[0].getState()==ISS_ON)        speed=1;
//     else if(property[1].getState()==ISS_ON)   speed=2;
//     else if(property[2].getState()==ISS_ON)   speed=3;
//     else if(property[3].getState()==ISS_ON)   speed=4;
//     else if(property[4].getState()==ISS_ON)   speed=5;
//     else if(property[5].getState()==ISS_ON)   speed=6;
//     else if(property[6].getState()==ISS_ON)   speed=7;
//     else if(property[7].getState()==ISS_ON)   speed=8;
//     else if(property[8].getState()==ISS_ON)   speed=9;
//     else if(property[9].getState()==ISS_ON)   speed=10;
//     else
//     {
//         Logger::Log("indi_client | getTelescopeSlewRate | Error: not found slew rate", LogLevel::ERROR, DeviceType::MOUNT);
//         speed = -1;
//         return QHYCCD_ERROR;
//     }

//     Logger::Log("indi_client | getTelescopeSlewRate | " + std::to_string(speed), LogLevel::INFO, DeviceType::MOUNT);
//     if(speed>=0 && speed<=9) Logger::Log("indi_client | getTelescopeSlewRate | " + std::to_string(speed) + " " + property[speed].getLabel(), LogLevel::INFO, DeviceType::MOUNT);

//     return QHYCCD_SUCCESS;

// }

uint32_t MyClient::setTelescopeSlewRate(INDI::BaseDevice *dp, int speed)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_SLEW_RATE");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setTelescopeSlewRate | Error: unable to find TELESCOPE_SLEW_RATE property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    Logger::Log("indi_client | setTelescopeSlewRate | " + std::to_string(property->count()), LogLevel::INFO, DeviceType::CAMERA);
    if (speed >= 0 && speed <= property->count())
    {
        property[speed - 1].setState(ISS_ON);

        for (int i = 0; i < property->count(); i++)
        {
            if (i != speed - 1)
            {
                property[i].setState(ISS_OFF);
            }
        }

        sendNewProperty(property);
    }

    Logger::Log("indi_client | setTelescopeSlewRate | " + std::to_string(speed), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getTelescopeTotalSlewRate(INDI::BaseDevice *dp, int &total)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_SLEW_RATE");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getTelescopeTotalSlewRate | Error: unable to find TELESCOPE_SLEW_RATE property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    total = property->count();

    Logger::Log("indi_client | getTelescopeTotalSlewRate:" + std::to_string(total), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getTelescopeMaxSlewRateOptions(INDI::BaseDevice *dp, int &min, int &max, int &value)
{
    //?? maybe onstep only
    INDI::PropertyNumber property = dp->getProperty("Max slew Rate");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getTelescopeMaxSlewRateOptions | Error: unable to find Max slew Rate property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    max = property->np[0].max;
    min = property->np[0].min;
    value = property->np[0].value;

    Logger::Log("indi_client | getTelescopeMaxSlewRateOptions" + std::to_string(max) + " " + std::to_string(min) + " " + std::to_string(value), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeMaxSlewRateOptions(INDI::BaseDevice *dp, int value)
{
    //?? maybe onstep only
    INDI::PropertyNumber property = dp->getProperty("Max slew Rate");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setTelescopeMaxSlewRateOptions | Error: unable to find Max slew Rate property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    property->np[0].value = value;
    sendNewProperty(property);

    QElapsedTimer t;
    t.start();

    while (property->getState() != IPS_OK && t.elapsed() < 3000)
    {
        QThread::msleep(100);
    }

    if (t.elapsed() > 3000)
    {
        Logger::Log("indi_client | setTelescopeMaxSlewRateOptions | ERROR : timeout ", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    Logger::Log("indi_client | setTelescopeMaxSlewRateOptions" + std::to_string(value), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeAbortMotion(INDI::BaseDevice *dp)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_ABORT_MOTION");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setTelescopeAbortMotion | Error: unable to find  TELESCOPE_ABORT_MOTION property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    property[0].setState(ISS_ON);
    sendNewProperty(property);

    // 设置移动状态为停止
    mountState.isNS_Moving = false;
    mountState.isWE_Moving = false;
    mountState.isSlewing = false;
    mountState.isHoming = false;
    mountState.isGuiding = false;
    mountState.isMoving = false;
    mountState.isFlipping = false;
    mountState.isFlipBacking = false;
    if (MountGotoTimer.isActive())
    {
        MountGotoTimer.stop();
        QObject::disconnect(&MountGotoTimer, &QTimer::timeout, nullptr, nullptr);
    }
    // 更新追踪状态
    getTelescopeTrackEnable(dp, mountState.isTracking);

    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getTelescopeMoveWE(INDI::BaseDevice *dp, QString &statu)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_MOTION_WE");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getTelescopeMoveWE | Error: unable to find  TELESCOPE_MOTION_WE property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    if (property[0].getState() == ISS_ON)
    {
        statu = "WEST";
        mountState.isWE_Moving = true;
    }
    else if (property[1].getState() == ISS_ON)
    {
        statu = "EAST";
        mountState.isWE_Moving = true;
    }
    else
    {
        statu = "STOP";
        mountState.isWE_Moving = false;
    }
    Logger::Log("indi_client | getTelescopeMoveWE" + statu.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeMoveWE(INDI::BaseDevice *dp, QString command)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_MOTION_WE");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setTelescopeMoveWE | Error: unable to find  TELESCOPE_MOTION_WE property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }
    if (mountState.isMovingNow() && command != "STOP")
    {
        Logger::Log("indi_client | setTelescopeMoveWE | Telescope is moving, return...", LogLevel::INFO, DeviceType::CAMERA);
        return QHYCCD_SUCCESS;
    }
    if (mountState.isParked)
        return QHYCCD_ERROR;

    if (command == "WEST")
    {
        property[0].setState(ISS_ON);
        property[1].setState(ISS_OFF);
        mountState.isWE_Moving = true;
    }
    else if (command == "EAST")
    {
        property[0].setState(ISS_OFF);
        property[1].setState(ISS_ON);
        mountState.isWE_Moving = true;
    }
    else if (command == "STOP")
    {
        // 如果当前轴没有在移动，STOP 指令也不能通过
        if (!mountState.isWE_Moving)
        {
            Logger::Log("indi_client | setTelescopeMoveWE | STOP command not allowed, axis not moving...", LogLevel::INFO, DeviceType::CAMERA);
            return QHYCCD_SUCCESS;
        }
        property[0].setState(ISS_OFF);
        property[1].setState(ISS_OFF);
        mountState.isWE_Moving = false;
    }

    sendNewProperty(property);
    Logger::Log("indi_client | setTelescopeMoveWE" + command.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getTelescopeMoveNS(INDI::BaseDevice *dp, QString &statu)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_MOTION_NS");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getTelescopeMoveNS | Error: unable to find  TELESCOPE_MOTION_NS property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    if (property[0].getState() == ISS_ON)
    {
        statu = "NORTH";
        mountState.isNS_Moving = true;
    }
    else if (property[1].getState() == ISS_ON)
    {
        statu = "SOUTH";
        mountState.isNS_Moving = true;
    }
    else
    {
        statu = "STOP";
        mountState.isNS_Moving = false;
    }
    Logger::Log("indi_client | getTelescopeMoveNS" + statu.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeMoveNS(INDI::BaseDevice *dp, QString command)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_MOTION_NS");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setTelescopeMoveNS | Error: unable to find  TELESCOPE_MOTION_NS property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }
    if (mountState.isMovingNow() && command != "STOP")
    {
        Logger::Log("indi_client | setTelescopeMoveNS | Telescope is moving, return...", LogLevel::INFO, DeviceType::CAMERA);
        return QHYCCD_SUCCESS;
    }
    if (mountState.isParked)
        return QHYCCD_ERROR;

    if (command == "NORTH")
    {
        property[0].setState(ISS_ON);
        property[1].setState(ISS_OFF);
        mountState.isNS_Moving = true;
    }
    else if (command == "SOUTH")
    {
        property[0].setState(ISS_OFF);
        property[1].setState(ISS_ON);
        mountState.isNS_Moving = true;
    }
    else if (command == "STOP")
    {
        // 如果当前轴没有在移动，STOP 指令也不能通过
        if (!mountState.isNS_Moving)
        {
            Logger::Log("indi_client | setTelescopeMoveNS | STOP command not allowed, axis not moving...", LogLevel::INFO, DeviceType::CAMERA);
            return QHYCCD_SUCCESS;
        }
        property[0].setState(ISS_OFF);
        property[1].setState(ISS_OFF);
        mountState.isNS_Moving = false;
    }

    sendNewProperty(property);
    Logger::Log("indi_client | setTelescopeMoveNS" + command.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeGuideNS(INDI::BaseDevice *dp, int dir, int time_guide)
{
    INDI::PropertyNumber property = dp->getProperty("TELESCOPE_TIMED_GUIDE_NS");
    if (!property.isValid())
    {
        Logger::Log("indi_client | setTelescopeGuideNS | Error: unable to find TELESCOPE_TIMED_GUIDE_NS property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }
    if (mountState.isParked)
        return QHYCCD_ERROR;
    if (dir == 1)
    {
        property->np[1].value = time_guide;
        property->np[0].value = 0;
        mountState.isGuiding = true;
    }
    else
    {
        property->np[0].value = time_guide;
        property->np[1].value = 0;
        mountState.isGuiding = false;
    }
    sendNewProperty(property);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeGuideWE(INDI::BaseDevice *dp, int dir, int time_guide)
{
    INDI::PropertyNumber property = dp->getProperty("TELESCOPE_TIMED_GUIDE_WE");
    if (!property.isValid())
    {
        Logger::Log("indi_client | setTelescopeGuideWE | Error: unable to find TELESCOPE_TIMED_GUIDE_WE property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }
    if (dir == 3)
    {
        property->np[0].value = time_guide;
        property->np[1].value = 0;
        mountState.isGuiding = true;
    }
    else
    {
        property->np[1].value = time_guide;
        property->np[0].value = 0;
        mountState.isGuiding = false;
    }
    sendNewProperty(property);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeActionAfterPositionSet(INDI::BaseDevice *dp, QString action)
{
    INDI::PropertySwitch property = dp->getProperty("ON_COORD_SET");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setTelescopeActionAfterPositionSet | Error: unable to find  ON_COORD_SET property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    // qDebug()<<"ON_COORD_SET"<< property->count();
    // for(int i=0;i<property->count();i++){
    //     qDebug()<<"ON_COORD_SET name : " <<property[i].getName();
    //     qDebug()<<"ON_COORD_SET label :" <<property[i].getLabel();
    //     qDebug()<<"ON_COORD_SET state :" <<property[i].getState();
    //     qDebug()<<"ON_COORD_SET aux :" <<property[i].getAux();
    // }

    // 按 name/label 自动识别 TRACK / SLEW / SYNC，而不是假定固定下标
    int idxTrack = -1;
    int idxSlew  = -1;
    int idxSync  = -1;

    for (int i = 0; i < property->count(); ++i)
    {
        QString name  = QString::fromStdString(property[i].getName()).toUpper();
        QString label = QString::fromStdString(property[i].getLabel()).toUpper();

        if (idxTrack == -1 && (name.contains("TRACK") || label.contains("TRACK")))
            idxTrack = i;
        else if (idxSlew == -1 && (name.contains("SLEW") || label.contains("SLEW")))
            idxSlew = i;
        else if (idxSync == -1 && (name.contains("SYNC") || label.contains("SYNC")))
            idxSync = i;
    }

    // 若完全无法识别，打印一次详细信息，方便针对不同驱动（EQMod/OnStep等）调试
    if (idxTrack == -1 && idxSlew == -1 && idxSync == -1)
    {
        Logger::Log("indi_client | setTelescopeActionAfterPositionSet | ON_COORD_SET entries not recognized, dump list:", LogLevel::WARNING, DeviceType::MOUNT);
        for (int i = 0; i < property->count(); ++i)
        {
            std::string name  = property[i].getName();
            std::string label = property[i].getLabel();
            Logger::Log("  [" + std::to_string(i) + "] name=" + name + ", label=" + label,
                        LogLevel::WARNING, DeviceType::MOUNT);
        }
        return QHYCCD_ERROR;
    }

    // 先全部置为 OFF
    for (int i = 0; i < property->count(); ++i)
        property[i].setState(ISS_OFF);

    if (action == "TRACK")
    {
        if (idxTrack == -1)
        {
            Logger::Log("indi_client | setTelescopeActionAfterPositionSet | TRACK entry not found in ON_COORD_SET",
                        LogLevel::WARNING, DeviceType::MOUNT);
            return QHYCCD_ERROR;
        }
        property[idxTrack].setState(ISS_ON);
        currentAction = "TRACK";
    }
    else if (action == "SLEW")
    {
        if (idxSlew == -1)
        {
            Logger::Log("indi_client | setTelescopeActionAfterPositionSet | SLEW entry not found in ON_COORD_SET",
                        LogLevel::WARNING, DeviceType::MOUNT);
            return QHYCCD_ERROR;
        }
        property[idxSlew].setState(ISS_ON);
        currentAction = "SLEW";
    }
    else if (action == "SYNC")
    {
        if (idxSync == -1)
        {
            Logger::Log("indi_client | setTelescopeActionAfterPositionSet | SYNC entry not found in ON_COORD_SET",
                        LogLevel::WARNING, DeviceType::MOUNT);
            return QHYCCD_ERROR;
        }
        property[idxSync].setState(ISS_ON);
        currentAction = "SYNC";
    }

    sendNewProperty(property);

    QElapsedTimer t;
    t.start();

    while (property->getState() != IPS_OK && t.elapsed() < 3000)
    {
        QThread::msleep(100);
    }

    if (t.elapsed() > 3000)
    {
        Logger::Log("indi_client | setTelescopeActionAfterPositionSet | ERROR : timeout ", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    Logger::Log("indi_client | setTelescopeActionAfterPositionSet " + action.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getTelescopeRADECJ2000(INDI::BaseDevice *dp, double &RA_Hours, double &DEC_Degree)
{
    INDI::PropertyNumber property = dp->getProperty("EQUATORIAL_COORD");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getTelescopeRADECJ2000 | Error: unable to find  EQUATORIAL_COORD property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    RA_Hours = property->np[0].value;
    DEC_Degree = property->np[1].value;
    Logger::Log("indi_client | getTelescopeRADECJ2000" + std::to_string(RA_Hours) + " " + std::to_string(DEC_Degree), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

/*
// 未在mainwindow.cpp中使用的函数 - 已注释
uint32_t MyClient::setTelescopeRADECJ2000(INDI::BaseDevice *dp, double RA_Hours, double DEC_Degree)
{
    INDI::PropertyNumber property = dp->getProperty("EQUATORIAL_COORD");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setTelescopeRADECJ2000 | Error: unable to find  EQUATORIAL_COORD property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    property->np[0].value = RA_Hours;
    property->np[1].value = DEC_Degree;

    sendNewProperty(property);
    // qDebug()<<"setTelescopeRADECJ2000"<< value;
    return QHYCCD_SUCCESS;
}
*/

uint32_t MyClient::getTelescopeRADECJNOW(INDI::BaseDevice *dp, double &RA_Hours, double &DEC_Degree)
{
    INDI::PropertyNumber property = dp->getProperty("EQUATORIAL_EOD_COORD");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getTelescopeRADECJNOW | Error: unable to find  EQUATORIAL_EOD_COORD property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }
    RA_Hours = property->np[0].value;
    DEC_Degree = property->np[1].value;
    // qDebug() << "getTelescopeRADECJNOW" << RA_Hours << DEC_Degree ;
    return QHYCCD_SUCCESS;
}

// uint32_t MyClient::setTelescopeRADECJNOW(INDI::BaseDevice *dp,double RA_Hours,double DEC_Degree)
// {
//     INDI::PropertyNumber property = dp->getProperty("EQUATORIAL_EOD_COORD");

//     if (!property.isValid())
//     {
//         Logger::Log("indi_client | setTelescopeRADECJNOW | Error: unable to find  EQUATORIAL_EOD_COORD property...", LogLevel::WARNING, DeviceType::CAMERA);
//         return QHYCCD_ERROR;
//     }

//     property->np[0].value=RA_Hours;
//     property->np[1].value=DEC_Degree;

//     sendNewProperty(property);

//     // 更新goto状态
//     if (!mountState.isHoming){
//         mountState.isSlewing = true;
//     }
//     connect(&MountGotoTimer, &QTimer::timeout, [this,dp,RA_Hours,DEC_Degree](){
//         double currentRA,currentDEC;
//         getTelescopeRADECJNOW(dp,currentRA,currentDEC);
//         if (abs(currentRA - RA_Hours) < 0.01 && abs(currentDEC - DEC_Degree) < 0.01){
//             MountGotoTimer.stop();
//             disconnect(&MountGotoTimer, &QTimer::timeout, this, nullptr);
//             if (mountState.isHoming){
//                 mountState.isHoming = false;
//             }else{
//                 mountState.isSlewing = false;
//             }
//         }
//         oldRA_Hours = currentRA;
//         oldDEC_Degree = currentDEC;
//     });
//     MountGotoTimer.start(1000);

//     return QHYCCD_SUCCESS;
// }

uint32_t MyClient::setTelescopeRADECJNOW(INDI::BaseDevice *dp, double RA_Hours, double DEC_Degree)
{
    INDI::PropertyNumber property = dp->getProperty("EQUATORIAL_EOD_COORD");
    if (!property.isValid())
    {
        Logger::Log("indi_client | setTelescopeRADECJNOW | Error: unable to find EQUATORIAL_EOD_COORD property...",
                    LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    if (mountState.isSlewing)
    {
        Logger::Log("indi_client | setTelescopeRADECJNOW | Error: telescope is slewing", LogLevel::WARNING, DeviceType::MOUNT);
        return QHYCCD_ERROR;
    }

    property->np[0].value = RA_Hours;
    property->np[1].value = DEC_Degree;
    sendNewProperty(property);

    if (currentAction == "SYNC")
    {
        Logger::Log("indi_client | setTelescopeRADECJNOW | Sync RA: " + std::to_string(RA_Hours) + " DEC: " + std::to_string(DEC_Degree) + " completed", LogLevel::INFO, DeviceType::MOUNT);
        return QHYCCD_SUCCESS;
    }
    
    // 若不是回零过程，标记为正在转动
    if (!mountState.isHoming && !mountState.isFlipping && !mountState.isFlipBacking)
        mountState.isSlewing = true;

    // ---------------- 判定参数（可按需调整） ----------------
    const double TOL_RA_HOUR = 0.01; // RA 容差（小时）≈ 36 arcmin
    const double TOL_DEC_DEG = 0.01; // DEC 容差（度）≈ 36 arcsec
    const int HIT_NEED = 3;          // 连续命中次数（抗抖动）
    const int MAX_TICKS = 600;       // 超时（1 Hz 轮询，600 秒）

    const double MIN_MOVE_RA_H = 0.0005; // 卡死判定的最小位移阈值（小时）≈ 7.5"
    const double MIN_MOVE_DEC = 0.002;   // 卡死判定的最小位移阈值（度）   ≈ 7.2"

    // RA 最短差（考虑 24h 环回）
    auto raDiffHour = [](double a, double b)
    {
        double d = fabs(a - b);
        if (d > 12.0)
            d = 24.0 - d; // 取最短差
        return d;
    };

    // 极点判定：在极点附近 DEC≈±90° 时，RA 有 12h 等价
    auto nearPole = [](double decDeg, double thrDeg = 89.0)
    {
        return std::fabs(decDeg) >= thrDeg;
    };

    // 保险：先断开已有连接，避免重复回调
    QObject::disconnect(&MountGotoTimer, &QTimer::timeout, nullptr, nullptr);

    QObject::connect(&MountGotoTimer, &QTimer::timeout, [this, dp]()
                     {
                         INDI::PropertyNumber eq = dp->getProperty("EQUATORIAL_EOD_COORD");
                         if (eq.isValid())
                         {
                             if (eq.getState() == IPS_OK || eq.getState() == IPS_IDLE)
                             {
                                 MountGotoTimer.stop();
                                 QObject::disconnect(&MountGotoTimer, &QTimer::timeout, nullptr, nullptr);
                                 if (mountState.isHoming)
                                 {
                                     mountState.isHoming = false;
                                     mountState.isSlewing = false;
                                     Logger::Log("indi_client | setTelescopeRADECJNOW | Homing Completed!", LogLevel::INFO, DeviceType::MOUNT);
                                     return;
                                 }
                                 else if (mountState.isSlewing)
                                 {
                                     mountState.isSlewing = false;
                                     Logger::Log("indi_client | setTelescopeRADECJNOW | Slew Completed!", LogLevel::INFO, DeviceType::MOUNT);
                                     return;
                                 }
                                 else if (mountState.isFlipping)
                                 {
                                     mountState.isFlipping = false;
                                     mountState.isFlipBacking = false;
                                     Logger::Log("indi_client | setTelescopeRADECJNOW | Flip Completed!", LogLevel::INFO, DeviceType::MOUNT);
                                     return;
                                 }
                                 else if (mountState.isFlipBacking)
                                 {
                                     mountState.isFlipBacking = false;
                                     mountState.isFlipping = true;
                                     if (slewTelescopeJNowNonBlock(dp, mountState.Flip_RA_Hours, mountState.Flip_DEC_Degree, true) == QHYCCD_SUCCESS)
                                     {
                                         Logger::Log("indi_client | setTelescopeRADECJNOW | Flip Back Completed,start flip!", LogLevel::INFO, DeviceType::MOUNT);
                                         return;
                                     }
                                     else
                                     {
                                         Logger::Log("indi_client | setTelescopeRADECJNOW | Flip Back Completed,but start flip failed!", LogLevel::WARNING, DeviceType::MOUNT);
                                         return;
                                     }
                                 }
                                 else
                                 {
                                     Logger::Log("indi_client | setTelescopeRADECJNOW | Unknown state completed!", LogLevel::WARNING, DeviceType::MOUNT);
                                     return;
                                 }
                             }
                         }
                         else
                         {
                             Logger::Log("indi_client | setTelescopeRADECJNOW | EQUATORIAL_EOD_COORD not found",
                                         LogLevel::WARNING, DeviceType::MOUNT);
                             QObject::disconnect(&MountGotoTimer, &QTimer::timeout, nullptr, nullptr);
                             mountState.isHoming = false;
                             return;
                         }
                     });

    // 统一用固定周期
    MountGotoTimer.start(1000);

    return QHYCCD_SUCCESS;
}

/*
// 未在mainwindow.cpp中使用的函数 - 已注释
uint32_t MyClient::getTelescopeTargetRADECJNOW(INDI::BaseDevice *dp, double &RA_Hours, double &DEC_Degree)
{
    INDI::PropertyNumber property = dp->getProperty("TARGET_EOD_COORD");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getTelescopeTargetRADECJNOW | Error: unable to find  TARGET_EOD_COORD property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    RA_Hours = property->np[0].value;
    DEC_Degree = property->np[1].value;
    Logger::Log("indi_client | getTelescopeTargetRADECJNOW" + std::to_string(RA_Hours) + " " + std::to_string(DEC_Degree), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}
*/

/*
// 未在mainwindow.cpp中使用的函数 - 已注释
uint32_t MyClient::setTelescopeTargetRADECJNOW(INDI::BaseDevice *dp, double RA_Hours, double DEC_Degree)
{
    INDI::PropertyNumber property = dp->getProperty("TARGET_EOD_COORD");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setTelescopeTargetRADECJNOW | Error: unable to find  TARGET_EOD_COORD property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    property->np[0].value = RA_Hours;
    property->np[1].value = DEC_Degree;

    sendNewProperty(property);
    // qDebug()<<"setTelescopeTargetRADECJNOW"<< value;
    return QHYCCD_SUCCESS;
}
*/

// compose slew command
uint32_t MyClient::slewTelescopeJNowNonBlock(INDI::BaseDevice *dp, double RA_Hours, double DEC_Degree, bool EnableTracking)
{
    QString action;
    if (EnableTracking == true)
        action = "TRACK";
    else
        action = "SLEW";

    if (mountState.isParked)
    {
        Logger::Log("indi_client | slewTelescopeJNowNonBlock | Error: telescope is parked", LogLevel::WARNING, DeviceType::MOUNT);
        return QHYCCD_ERROR;
    }

    uint32_t result1 = setTelescopeActionAfterPositionSet(dp, action);
    uint32_t result2 = setTelescopeRADECJNOW(dp, RA_Hours, DEC_Degree);

    if (result1 != QHYCCD_SUCCESS || result2 != QHYCCD_SUCCESS)
    {
        return QHYCCD_ERROR;
    }

    return QHYCCD_SUCCESS;
}

uint32_t MyClient::syncTelescopeJNow(INDI::BaseDevice *dp, double RA_Hours, double DEC_Degree)
{
    Logger::Log("indi_client | syncTelescopeJNow | start", LogLevel::INFO, DeviceType::CAMERA);

    // 1. 将 ON_COORD_SET 设为 SYNC
    uint32_t result = setTelescopeActionAfterPositionSet(dp, "SYNC");
    if (result != QHYCCD_SUCCESS)
    {
        Logger::Log("indi_client | syncTelescopeJNow | setTelescopeActionAfterPositionSet(SYNC) failed",
                    LogLevel::WARNING, DeviceType::MOUNT);
        return result;
    }

    // 2. 下发同步坐标（注意：RA_Hours 必须为小时制）
    result = setTelescopeRADECJNOW(dp, RA_Hours, DEC_Degree);
    if (result != QHYCCD_SUCCESS)
    {
        Logger::Log("indi_client | syncTelescopeJNow | setTelescopeRADECJNOW failed",
                    LogLevel::WARNING, DeviceType::MOUNT);
        return result;
    }

    // 3. 恢复为 SLEW，避免后续 GOTO 仍然处于 SYNC 模式
    result = setTelescopeActionAfterPositionSet(dp, "SLEW");
    if (result != QHYCCD_SUCCESS)
    {
        Logger::Log("indi_client | syncTelescopeJNow | setTelescopeActionAfterPositionSet(SLEW) failed",
                    LogLevel::WARNING, DeviceType::MOUNT);
        return result;
    }

    Logger::Log("indi_client | syncTelescopeJNow | end", LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getTelescopetAZALT(INDI::BaseDevice *dp, double &AZ_DEGREE, double &ALT_DEGREE)
{
    INDI::PropertyNumber property = dp->getProperty("HORIZONTAL_COORD");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getTelescopetAZALT | Error: unable to find  HORIZONTAL_COORD property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    ALT_DEGREE = property->np[0].value;
    AZ_DEGREE = property->np[1].value;
    Logger::Log("indi_client | getTelescopetAZALT" + std::to_string(AZ_DEGREE) + " " + std::to_string(ALT_DEGREE), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

/*
// 未在mainwindow.cpp中使用的函数 - 已注释
uint32_t MyClient::setTelescopetAZALT(INDI::BaseDevice *dp, double AZ_DEGREE, double ALT_DEGREE)
{
    INDI::PropertyNumber property = dp->getProperty("HORIZONTAL_COORD");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setTelescopetAZALT | Error: unable to find  HORIZONTAL_COORD property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }
    property->np[0].value = AZ_DEGREE;
    property->np[1].value = ALT_DEGREE;

    sendNewProperty(property);
    // qDebug()<<"setTelescopetAZALT"<< AZ_DEGREE <<ALT_DEGREE;
    return QHYCCD_SUCCESS;
}
*/

// uint32_t MyClient::getTelescopeStatus(INDI::BaseDevice *dp,QString &statu,QString &error)
// {
//     if(QString::fromStdString(dp->getDeviceName()) == "LX200 OnStep") {
//         INDI::PropertyText property = dp->getProperty("OnStep Status");

//         if (!property.isValid())
//         {
//             Logger::Log("indi_client | getTelescopeStatus | Error: unable to find  OnStep Status property...", LogLevel::WARNING, DeviceType::CAMERA);
//             return QHYCCD_ERROR;
//         }

//         // 打印property的所有内容
//         Logger::Log("=== OnStep Status Property 详细信息 ===", LogLevel::INFO, DeviceType::MOUNT);
//         Logger::Log("Property名称: " + QString::fromStdString(property.getName()).toStdString(), LogLevel::INFO, DeviceType::MOUNT);
//         Logger::Log("Property标签: " + QString::fromStdString(property.getLabel()).toStdString(), LogLevel::INFO, DeviceType::MOUNT);
//         Logger::Log("Property组名: " + QString::fromStdString(property.getGroupName()).toStdString(), LogLevel::INFO, DeviceType::MOUNT);
//         Logger::Log("Property权限: " + QString::number(property.getPermission()).toStdString(), LogLevel::INFO, DeviceType::MOUNT);
//         Logger::Log("Property状态: " + QString::fromStdString(property.getStateAsString()).toStdString(), LogLevel::INFO, DeviceType::MOUNT);
//         Logger::Log("Property数量: " + QString::number(property.count()).toStdString(), LogLevel::INFO, DeviceType::MOUNT);

//         // 打印所有文本值
//         for (int i = 0; i < property.count(); i++) {
//             QString value = QString::fromStdString(property[i].getText());
//             QString label = QString::fromStdString(property[i].getLabel());
//             Logger::Log(QString("  [%1] Label: %2, Value: %3").arg(i).arg(label).arg(value).toStdString(), LogLevel::INFO, DeviceType::MOUNT);
//         }
//         Logger::Log("=====================================", LogLevel::INFO, DeviceType::MOUNT);

//         statu = QString::fromStdString(property[1].getText());
//         Logger::Log("当前赤道仪状态:" + statu.toStdString(), LogLevel::WARNING, DeviceType::MOUNT);
//         // error = QString::fromStdString(property[7].getText());
//         // qDebug()<<"OnStep error: "<< error;
//         // if(error != "None") {
//         //     qDebug() << "\033[32m" << "OnStep error: " << error << "\033[0m";
//         // }

//         return QHYCCD_SUCCESS;
//     }
// }

uint32_t MyClient::getTelescopeStatus(INDI::BaseDevice *dp, QString &statu)
{
    bool isMoving = mountState.isMovingNow();
    if (isMoving)
    {
        statu = "Moving";
    }
    else
    {
        statu = "Idle";
    }
    return QHYCCD_SUCCESS;
}

// uint32_t MyClient::getTelescopeStatus(INDI::BaseDevice *dp, QString &statu, QString &error)
// {

//     INDI::PropertyText property = dp->getProperty("OnStep Status");
//     if (!property.isValid())
//     {
//         // Logger::Log("indi_client | getTelescopeStatus | Error: unable to find  OnStep Status property...", LogLevel::WARNING, DeviceType::CAMERA);
//     }
//     else
//     {
//         statu = property[1].getText();
//     }

//     if (statu.isEmpty())
//     {
//         // Logger::Log("indi_client | getTelescopeStatus | Error: OnStep Status is empty", LogLevel::WARNING, DeviceType::CAMERA);
//     }
//     else
//     {
//         return QHYCCD_SUCCESS;
//     }

//     INDI::PropertyLight property1 = dp->getProperty("RASTATUS");
//     INDI::PropertyLight property2 = dp->getProperty("DESTATUS");

//     if (!property1.isValid() && !property2.isValid())
//     {
//         // Logger::Log("indi_client | getTelescopeStatus | Error: unable to find RASTATUS OR DESTATUS property...", LogLevel::WARNING, DeviceType::CAMERA);
//     }
//     else
//     {
//         if (property1.count() == 5)
//         {
//             if (property1[0].getState() == 1)
//             {
//                 // RAfirstTrack = true;
//                 if (property1[1].getState() == 1)
//                 {
//                     if (property1[2].getState() == 1)
//                     {
//                         statu = "Busy";
//                     }
//                     else if (property1[2].getState() == 2)
//                     {
//                         if (ismove)
//                         {
//                             statu = "Busy";
//                             // Logger::Log("indi_client | getTelescopeStatus | RASTATUS state is Tracking", LogLevel::INFO, DeviceType::MOUNT);
//                         }
//                         else
//                         {
//                             statu = "Tracking";
//                             // Logger::Log("indi_client | getTelescopeStatus | RASTATUS state is Idle", LogLevel::INFO, DeviceType::MOUNT);
//                         }
//                     }
//                 }
//                 else if (property1[1].getState() == 2)
//                 {
//                     statu = "Idle";
//                 }
//                 else
//                 {
//                     statu = "Busy";
//                     // Logger::Log("indi_client | getTelescopeStatus | Error: RASTATUS state is not recognized", LogLevel::WARNING, DeviceType::CAMERA);
//                 }
//             }
//         }

//         if (property2.count() == 5 && statu != "Busy")
//         {
//             if (property2[0].getState() == 1)
//             {
//                 // DECfirstTrack = true;
//                 if (property2[1].getState() == 1)
//                 {
//                     statu = "Busy";
//                     // Logger::Log("indi_client | getTelescopeStatus | RASTATUS state is Busy", LogLevel::INFO, DeviceType::MOUNT);
//                 }
//                 else if (property2[1].getState() == 2)
//                 {
//                     statu = "Idle";
//                 }
//                 else
//                 {
//                     statu = "Busy";
//                     // Logger::Log("indi_client | getTelescopeStatus | Error: RASTATUS state is not recognized", LogLevel::WARNING, DeviceType::CAMERA);
//                 }
//             }
//         }
//     }
//     if (statu.isEmpty())
//     {
//         // Logger::Log("indi_client | getTelescopeStatus | Error: RASTATUS is empty", LogLevel::WARNING, DeviceType::CAMERA);
//         // Logger::Log("indi_client | getTelescopeStatus | Error: unable to find mount state...", LogLevel::ERROR, DeviceType::CAMERA);
//         return QHYCCD_ERROR;
//     }
//     else
//     {
//         return QHYCCD_SUCCESS;
//     }
// }

// uint32_t MyClient::getTelescopeStatus(INDI::BaseDevice *dp,QString &statu,QString &error)
// {
//     if(QString::fromStdString(dp->getDeviceName()) == "LX200 OnStep") {
//         INDI::PropertyText property = dp->getProperty("OnStep Status");

//         if (!property.isValid())
//         {
//             Logger::Log("indi_client | getTelescopeStatus | Error: unable to find  OnStep Status property...", LogLevel::WARNING, DeviceType::CAMERA);
//             return QHYCCD_ERROR;
//         }

//         statu = QString::fromStdString(property[1].getText());
//         Logger::Log("当前赤道仪状态:" + statu.toStdString(), LogLevel::WARNING, DeviceType::MOUNT);
//         // error = QString::fromStdString(property[7].getText());
//         // qDebug()<<"OnStep error: "<< error;
//         // if(error != "None") {
//         //     qDebug() << "\033[32m" << "OnStep error: " << error << "\033[0m";
//         // }

//         return QHYCCD_SUCCESS;
//     }
// }

/**************************************************************************************
**                                  Focus API
***************************************************************************************/

uint32_t MyClient::getFocuserSDKVersion(INDI::BaseDevice *dp, QString &version)
{
    INDI::PropertyNumber focuserSDKVersion = dp->getProperty("FOCUS_VERSION");
    if (!focuserSDKVersion.isValid() || focuserSDKVersion->nnp <= 0)
    {
        Logger::Log("indi_client | getFocuserSDKVersion | Error: unable to find  FOCUS_VERSION property...", LogLevel::WARNING, DeviceType::FOCUSER);
        return QHYCCD_ERROR;
    }

    // 选择条目：优先 name 含 "VERSION" 的项，否则使用第 0 项
    int idx = 0;
    for (int i = 0; i < focuserSDKVersion->nnp; i++)
    {
        const char *nm = focuserSDKVersion->np[i].name;
        if (nm && std::string(nm).find("VERSION") != std::string::npos)
        {
            idx = i;
            break;
        }
    }

    // 将浮点安全地转换为整数版本号（如 20231207）
    double v = focuserSDKVersion->np[idx].value;
    uint64_t ver = static_cast<uint64_t>(std::llround(v));
    version = QString::number(static_cast<qulonglong>(ver));

    // 记录一次规范化日志（禁用科学计数法）
    // std::ostringstream oss;
    // oss << std::fixed << std::setprecision(0);
    // oss << "indi_client | getFocuserSDKVersion | FOCUS_VERSION"
    //     << " | choose idx:" << idx
    //     << " name:" << (focuserSDKVersion->np[idx].name ? focuserSDKVersion->np[idx].name : "")
    //     << " value_raw:" << v
    //     << " value_norm:" << ver
    //     << " label:" << (focuserSDKVersion->label ? focuserSDKVersion->label : "")
    //     << " group:" << (focuserSDKVersion->group ? focuserSDKVersion->group : "")
    //     << " perm:" << static_cast<int>(focuserSDKVersion->p)
    //     << " state:" << static_cast<int>(focuserSDKVersion->s)
    //     << " nnp:" << focuserSDKVersion->nnp;
    // Logger::Log(oss.str(), LogLevel::INFO, DeviceType::FOCUSER);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getFocuserSpeed(INDI::BaseDevice *dp, int &value, int &min, int &max, int &step)
{
    INDI::PropertyNumber property = dp->getProperty("FOCUS_SPEED");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getFocuserSpeed | Error: unable to find  FOCUS_SPEED property...", LogLevel::WARNING, DeviceType::FOCUSER);
        return QHYCCD_ERROR;
    }

    value = property->np[0].value;
    min = property->np[0].min;
    max = property->np[0].max;
    step = property->np[0].step;
    Logger::Log("indi_client | getFocuserSpeed" + std::to_string(value) + " " + std::to_string(min) + " " + std::to_string(max), LogLevel::INFO, DeviceType::FOCUSER);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setFocuserSpeed(INDI::BaseDevice *dp, int value)
{
    INDI::PropertyNumber property = dp->getProperty("FOCUS_SPEED");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setFocuserSpeed | Error: unable to find  FOCUS_SPEED property...", LogLevel::WARNING, DeviceType::FOCUSER);
        return QHYCCD_ERROR;
    }
    property->np[0].value = value;

    sendNewProperty(property);

    Logger::Log("indi_client | setFocuserSpeed" + std::to_string(value), LogLevel::INFO, DeviceType::FOCUSER);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getFocuserMoveDiretion(INDI::BaseDevice *dp, bool &isDirectionIn)
{
    INDI::PropertySwitch property = dp->getProperty("FOCUS_MOTION");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getFocuserMoveDiretion | Error: unable to find  FOCUS_MOTION property...", LogLevel::WARNING, DeviceType::FOCUSER);
        return QHYCCD_ERROR;
    }

    if (property[0].getState() == ISS_ON)
    {
        isDirectionIn = true;
    }
    else if (property[1].getState() == ISS_ON)
    {
        isDirectionIn = false;
    }

    Logger::Log("indi_client | getFocuserMoveDiretion | IN/OUT isDirectionIn:" + std::to_string(isDirectionIn), LogLevel::INFO, DeviceType::FOCUSER);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setFocuserMoveDiretion(INDI::BaseDevice *dp, bool isDirectionIn)
{
    INDI::PropertySwitch property = dp->getProperty("FOCUS_MOTION");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setFocuserMoveDiretion | Error: unable to find  FOCUS_MOTION property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    if (isDirectionIn == true)
    {
        property[0].setState(ISS_ON);
        property[1].setState(ISS_OFF);
    }
    if (isDirectionIn == false)
    {
        property[0].setState(ISS_OFF);
        property[1].setState(ISS_ON);
    }
    sendNewProperty(property);
    Logger::Log("indi_client | setFocuserMoveDiretion | IN/OUT isDirectionIn:" + std::to_string(isDirectionIn), LogLevel::INFO, DeviceType::FOCUSER);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getFocuserMaxLimit(INDI::BaseDevice *dp, int &maxlimit)
{
    INDI::PropertyNumber property = dp->getProperty("FOCUS_MAX");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getFocuserMaxLimit | Error: unable to find  FOCUS_MAX property...", LogLevel::WARNING, DeviceType::FOCUSER);
        return QHYCCD_ERROR;
    }

    maxlimit = property->np[0].value;

    Logger::Log("indi_client | getFocuserMaxLimit" + std::to_string(maxlimit), LogLevel::INFO, DeviceType::FOCUSER);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setFocuserMaxLimit(INDI::BaseDevice *dp, int maxlimit)
{
    INDI::PropertyNumber property = dp->getProperty("FOCUS_MAX");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setFocuserMaxLimit | Error: unable to find  FOCUS_MAX property...", LogLevel::WARNING, DeviceType::FOCUSER);
        return QHYCCD_ERROR;
    }
    property->np[0].value = maxlimit;

    sendNewProperty(property);

    Logger::Log("indi_client | setFocuserMaxLimit" + std::to_string(maxlimit), LogLevel::INFO, DeviceType::FOCUSER);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getFocuserReverse(INDI::BaseDevice *dp, bool &isReversed)
{
    INDI::PropertySwitch property = dp->getProperty("FOCUS_REVERSE_MOTION");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getFocuserReverse | Error: unable to find  FOCUS_REVERSE_MOTION property...", LogLevel::WARNING, DeviceType::FOCUSER);
        return QHYCCD_ERROR;
    }

    if (property[0].getState() == ISS_ON)
    {
        isReversed = true;
    }
    else if (property[1].getState() == ISS_ON)
    {
        isReversed = false;
    }

    Logger::Log("indi_client | getFocuserReverse | IN/OUT isDirectionIn:" + std::to_string(isReversed), LogLevel::INFO, DeviceType::FOCUSER);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setFocuserReverse(INDI::BaseDevice *dp, bool isReversed)
{
    INDI::PropertySwitch property = dp->getProperty("FOCUS_REVERSE_MOTION");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setFocuserReverse | Error: unable to find  FOCUS_REVERSE_MOTION property...", LogLevel::WARNING, DeviceType::FOCUSER);
        return QHYCCD_ERROR;
    }

    if (isReversed == true)
    {
        property[0].setState(ISS_ON);
        property[1].setState(ISS_OFF);
    }
    if (isReversed == false)
    {
        property[0].setState(ISS_OFF);
        property[1].setState(ISS_ON);
    }
    sendNewProperty(property);
    Logger::Log("indi_client | setFocuserReverse | IN/OUT isDirectionIn:" + std::to_string(isReversed), LogLevel::INFO, DeviceType::FOCUSER);
    return QHYCCD_SUCCESS;
}

//---------------actions------------------

uint32_t MyClient::moveFocuserSteps(INDI::BaseDevice *dp, int steps)
{
    INDI::PropertyNumber property = dp->getProperty("REL_FOCUS_POSITION");

    if (!property.isValid())
    {
        Logger::Log("indi_client | moveFocuserSteps | Error: unable to find  REL_FOCUS_POSITION property...", LogLevel::WARNING, DeviceType::FOCUSER);
        return QHYCCD_ERROR;
    }
    if (steps > 0)
    {
        property->np[0].value = steps;
    }
    else
    {
        Logger::Log("indi_client | moveFocuserSteps | Error: steps is negative", LogLevel::WARNING, DeviceType::FOCUSER);
        return QHYCCD_ERROR;
    }

    sendNewProperty(property);

    Logger::Log("indi_client | moveFocuserSteps " + std::to_string(steps), LogLevel::INFO, DeviceType::FOCUSER);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getFocuserRange(INDI::BaseDevice *dp, int &min, int &max, int &step, int &value)
{
    INDI::PropertyNumber property = dp->getProperty("ABS_FOCUS_POSITION");
    if (!property.isValid())
    {
        Logger::Log("indi_client | getFocuserRange | Error: unable to find  ABS_FOCUS_POSITION property...", LogLevel::WARNING, DeviceType::FOCUSER);
        return QHYCCD_ERROR;
    }
    min = property->np[0].min;
    max = property->np[0].max;
    step = property->np[0].step;
    value = property->np[0].value;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::moveFocuserToAbsolutePosition(INDI::BaseDevice *dp, int position)
{
    INDI::PropertyNumber property = dp->getProperty("ABS_FOCUS_POSITION");

    if (!property.isValid())
    {
        Logger::Log("indi_client | moveFocuserToAbsolutePosition | Error: unable to find  ABS_FOCUS_POSITION property...", LogLevel::WARNING, DeviceType::FOCUSER);
        return QHYCCD_ERROR;
    }
    property->np[0].value = position;

    sendNewProperty(property);

    Logger::Log("indi_client | moveFocuserToAbsolutePosition" + std::to_string(position), LogLevel::INFO, DeviceType::FOCUSER);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getFocuserAbsolutePosition(INDI::BaseDevice *dp, int &position)
{
    INDI::PropertyNumber property = dp->getProperty("ABS_FOCUS_POSITION");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getFocuserAbsolutePosition | Error: unable to find  ABS_FOCUS_POSITION property...", LogLevel::WARNING, DeviceType::FOCUSER);
        return QHYCCD_ERROR;
    }

    position = property->np[0].value;

    // sendNewProperty(property);

    Logger::Log("indi_client | getFocuserAbsolutePosition: " + std::to_string(position), LogLevel::DEBUG, DeviceType::FOCUSER);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::moveFocuserWithTime(INDI::BaseDevice *dp, int msec)
{
    // move the focuser at defined motion direction and defined move speed with msec time
    INDI::PropertyNumber property = dp->getProperty("FOCUS_TIMER");

    if (!property.isValid())
    {
        Logger::Log("indi_client | moveFocuserWithTime | Error: unable to find  FOCUS_TIMER property...", LogLevel::WARNING, DeviceType::FOCUSER);
        return QHYCCD_ERROR;
    }
    property->np[0].value = msec;

    sendNewProperty(property);

    Logger::Log("indi_client | moveFocuserWithTime" + std::to_string(msec), LogLevel::INFO, DeviceType::FOCUSER);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::abortFocuserMove(INDI::BaseDevice *dp)
{
    INDI::PropertySwitch property = dp->getProperty("FOCUS_ABORT_MOTION");

    if (!property.isValid())
    {
        Logger::Log("indi_client | abortFocuserMove | Error: unable to find  FOCUS_ABORT_MOTION property...", LogLevel::WARNING, DeviceType::FOCUSER);
        return QHYCCD_ERROR;
    }

    property[0].setState(ISS_ON);
    sendNewProperty(property);

    Logger::Log("indi_client | abortFocuserMove", LogLevel::INFO, DeviceType::FOCUSER);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::syncFocuserPosition(INDI::BaseDevice *dp, int position)
{
    INDI::PropertyNumber property = dp->getProperty("FOCUS_SYNC");

    if (!property.isValid())
    {
        Logger::Log("indi_client | syncFocuserPosition | Error: unable to find  FOCUS_SYNC property...", LogLevel::WARNING, DeviceType::FOCUSER);
        return QHYCCD_ERROR;
    }
    property->np[0].value = position;

    sendNewProperty(property);

    Logger::Log("indi_client | syncFocuserPosition" + std::to_string(position), LogLevel::INFO, DeviceType::FOCUSER);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getFocuserOutTemperature(INDI::BaseDevice *dp, double &value)
{
    INDI::PropertyNumber property = dp->getProperty("FOCUS_TEMPERATURE");

    // property->np[0].value = 0;

    sendNewProperty(property);

    if (!property.isValid())
    {
        Logger::Log("indi_client | getFocuserOutTemperature | Error: unable to find  FOCUS_TEMPERATURE property...", LogLevel::WARNING, DeviceType::FOCUSER);
        return QHYCCD_ERROR;
    }

    value = property->np[0].value;
    Logger::Log("indi_client | getFocuserOutTemperature" + std::to_string(value), LogLevel::INFO, DeviceType::FOCUSER);

    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getFocuserChipTemperature(INDI::BaseDevice *dp, double &value)
{
    INDI::PropertyNumber property = dp->getProperty("CHIP_TEMPERATURE");

    sendNewProperty(property);

    if (!property.isValid())
    {
        Logger::Log("indi_client | getFocuserChipTemperature | Error: unable to find  FOCUS_TEMPERATURE property...", LogLevel::WARNING, DeviceType::FOCUSER);
        return QHYCCD_ERROR;
    }

    value = property->np[0].value;
    Logger::Log("indi_client | getFocuserChipTemperature" + std::to_string(value), LogLevel::INFO, DeviceType::FOCUSER);

    return QHYCCD_SUCCESS;
}

/**************************************************************************************
**                                  CFW API
***************************************************************************************/
uint32_t MyClient::getCFWPosition(INDI::BaseDevice *dp, int &position, int &min, int &max)
{
    INDI::PropertyNumber property = dp->getProperty("FILTER_SLOT");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getCFWPosition | Error: unable to find  FILTER_SLOT property...", LogLevel::WARNING, DeviceType::CAMERA);
        min = 0;
        max = 0;
        position = 0;
        return QHYCCD_ERROR;
    }

    position = property->np[0].value;
    min = property->np[0].min;
    max = property->np[0].max;

    Logger::Log("indi_client | getCFWPosition" + std::to_string(position) + " " + std::to_string(min) + " " + std::to_string(max), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCFWPosition(INDI::BaseDevice *dp, int position)
{
    INDI::PropertyNumber property = dp->getProperty("FILTER_SLOT");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setCFWPosition | Error: unable to find  FILTER_SLOT property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }
    property->np[0].value = position;

    sendNewProperty(property);

    QElapsedTimer t;
    t.start();

    int timeout = 10000;
    while (t.elapsed() < timeout)
    {
        Logger::Log("indi_client | setCFWPosition | State:" + std::string(property->getStateAsString()), LogLevel::DEBUG, DeviceType::CAMERA);
        // qDebug() << "State:" << property->getState();
        QThread::msleep(300);
        if (property->getState() == IPS_OK)
        {
            Logger::Log("indi_client | setCFWPosition | State:" + std::string(property->getStateAsString()), LogLevel::INFO, DeviceType::CAMERA);
            break; // it will not wait the motor arrived
        }
    }

    if (t.elapsed() > timeout)
    {
        Logger::Log("indi_client | setCFWPosition | ERROR : timeout ", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getCFWSlotName(INDI::BaseDevice *dp, QString &name)
{
    INDI::PropertyText property = dp->getProperty("FILTER_NAME");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getCFWSlotName | Error: unable to find  FILTER_NAME property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    name = property[0].getText();

    Logger::Log("indi_client | getCFWSlotName" + name.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCFWSlotName(INDI::BaseDevice *dp, QString name)
{
    INDI::PropertyText property = dp->getProperty("FILTER_NAME");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setCFWSlotName | Error: unable to find  FILTER_NAME property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    property[0].setText(name.toLatin1().data());

    sendNewProperty(property);
    // qDebug()<<"setCFWSlotName"<< name ;
    return QHYCCD_SUCCESS;
}

/**************************************************************************************
**                         Generic Properties
***************************************************************************************/
uint32_t MyClient::getDevicePort(INDI::BaseDevice *dp, QString &Device_Port) // add by CJQ 2023.3.3
{
    INDI::PropertyText property = dp->getProperty("DEVICE_PORT");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getDevicePort | Error: unable to find DEVICE_PORT property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    Device_Port = property[0].getText();

    Logger::Log("indi_client | getDevicePort" + Device_Port.toStdString(), LogLevel::INFO, DeviceType::CAMERA);

    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setDevicePort(INDI::BaseDevice *dp, QString Device_Port) // add by CJQ 2023.3.28
{
    INDI::PropertyText property = dp->getProperty("DEVICE_PORT");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setDevicePort | Error: unable to find DEVICE_PORT property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    property[0].setText(Device_Port.toLatin1().data());

    sendNewProperty(property);

    Logger::Log("indi_client | setDevicePort" + Device_Port.toStdString(), LogLevel::INFO, DeviceType::CAMERA);

    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTimeUTC(INDI::BaseDevice *dp, QDateTime datetime)
{
    INDI::PropertyText property = dp->getProperty("TIME_UTC");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setTimeUTC | Error: unable to find  TIME_UTC property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    QDateTime datetime_utc;
    datetime_utc = datetime.toUTC();

    QString time_utc = datetime_utc.toString(Qt::ISODate);
    QTimeZone timeZone = datetime.timeZone();
    // Print the time zone offset and abbreviation
    Logger::Log("indi_client | setTimeUTC | Time zone offset:" + std::to_string(timeZone.offsetFromUtc(datetime)), LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("indi_client | setTimeUTC | Time zone abbreviation:" + timeZone.abbreviation(datetime).toStdString(), LogLevel::INFO, DeviceType::CAMERA);

    int timezone_hours = (timeZone.offsetFromUtc(datetime_utc)) / 3600;

    QString offset = QString::number(timezone_hours);

    Logger::Log("indi_client | setTimeUTC" + time_utc.toStdString() + " " + offset.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    property[0].setText(time_utc.toLatin1().data());
    property[1].setText(offset.toLatin1().data());

    Logger::Log("indi_client | setTimeUTC | property[0].setText(time_utc.toLatin1().data());" + std::string(time_utc.toLatin1().data()), LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("indi_client | setTimeUTC | property[1].setText(offset.toLatin1().data());" + std::string(offset.toLatin1().data()), LogLevel::INFO, DeviceType::CAMERA);

    sendNewProperty(property);

    QElapsedTimer t;
    t.start();

    while (property->getState() != IPS_OK && t.elapsed() < 3000)
    {
        QThread::msleep(100);
    }

    if (t.elapsed() > 3000)
    {
        Logger::Log("indi_client | setTimeUTC | ERROR : timeout ", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    Logger::Log("indi_client | setTimeUTC" + time_utc.toStdString() + " " + offset.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getTimeUTC(INDI::BaseDevice *dp, QDateTime &datetime)
{
    INDI::PropertyText property = dp->getProperty("TIME_UTC");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getTimeUTC | Error: unable to find  TIME_UTC property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    QString time = property[0].getText(); // ISO8601 string , UTC
    QString offset = property[1].getText();

    Logger::Log("indi_client | getTimeUTC" + time.toStdString() + " " + offset.toStdString(), LogLevel::INFO, DeviceType::CAMERA);

    datetime = QDateTime::fromString(time, Qt::ISODate);
    QTimeZone timeZone(offset.toInt() * 3600);
    datetime.setTimeZone(timeZone);

    QElapsedTimer t;
    t.start();

    while (property->getState() != IPS_OK && t.elapsed() < 3000)
    {
        QThread::msleep(100);
    }

    if (t.elapsed() > 3000)
    {
        Logger::Log("indi_client | getTimeUTC | ERROR : timeout ", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setLocation(INDI::BaseDevice *dp, double latitude_degree, double longitude_degree, double elevation)
{
    INDI::PropertyNumber property = dp->getProperty("GEOGRAPHIC_COORD");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setLocation | Error: unable to find  GEOGRAPHIC_COORD property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    property->np[0].value = latitude_degree;
    property->np[1].value = longitude_degree;
    property->np[2].value = elevation;

    sendNewProperty(property);

    QElapsedTimer t;
    t.start();

    while (property->getState() != IPS_OK && t.elapsed() < 3000)
    {
        QThread::msleep(100);
    }

    if (t.elapsed() > 3000)
    {
        Logger::Log("indi_client | setLocation | ERROR : timeout ", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    Logger::Log("indi_client | setLocation" + std::to_string(latitude_degree) + " " + std::to_string(longitude_degree) + " " + std::to_string(elevation), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getLocation(INDI::BaseDevice *dp, double &latitude_degree, double &longitude_degree, double &elevation)
{
    INDI::PropertyNumber property = dp->getProperty("GEOGRAPHIC_COORD");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getLocation | Error: unable to find  GEOGRAPHIC_COORD property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    latitude_degree = property->np[0].value; // ISO8601 string
    longitude_degree = property->np[1].value;
    elevation = property->np[2].value;

    Logger::Log("indi_client | getLocation" + std::to_string(latitude_degree) + " " + std::to_string(longitude_degree) + " " + std::to_string(elevation), LogLevel::INFO, DeviceType::CAMERA);

    QElapsedTimer t;
    t.start();

    while (property->getState() != IPS_OK && t.elapsed() < 3000)
    {
        QThread::msleep(100);
    }

    if (t.elapsed() > 3000)
    {
        Logger::Log("indi_client | getLocation | ERROR : timeout ", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setAtmosphere(INDI::BaseDevice *dp, double temperature, double pressure, double humidity)
{
    INDI::PropertyNumber property = dp->getProperty("ATMOSPHERE");

    if (!property.isValid())
    {
        Logger::Log("indi_client | setAtmosphere | Error: unable to find  ATMOSPHERE property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    property->np[0].value = temperature;
    property->np[1].value = pressure;
    property->np[2].value = humidity;

    sendNewProperty(property);

    QElapsedTimer t;
    t.start();

    while (property->getState() != IPS_OK && t.elapsed() < 3000)
    {
        QThread::msleep(100);
    }

    if (t.elapsed() > 3000)
    {
        Logger::Log("indi_client | setAtmosphere | ERROR : timeout ", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    Logger::Log("indi_client | setAtmosphere" + std::to_string(temperature) + " " + std::to_string(pressure) + " " + std::to_string(humidity), LogLevel::INFO, DeviceType::CAMERA);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getAtmosphere(INDI::BaseDevice *dp, double &temperature, double &pressure, double &humidity)
{
    INDI::PropertyNumber property = dp->getProperty("ATMOSPHERE");

    if (!property.isValid())
    {
        Logger::Log("indi_client | getAtmosphere | Error: unable to find  ATMOSPHERE property...", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    temperature = property->np[0].value; // ISO8601 string
    pressure = property->np[1].value;
    humidity = property->np[2].value;

    Logger::Log("indi_client | getAtmosphere" + std::to_string(temperature) + " " + std::to_string(pressure) + " " + std::to_string(humidity), LogLevel::INFO, DeviceType::CAMERA);

    QElapsedTimer t;
    t.start();

    while (property->getState() != IPS_OK && t.elapsed() < 3000)
    {
        QThread::msleep(100);
    }

    if (t.elapsed() > 3000)
    {
        Logger::Log("indi_client | getAtmosphere | ERROR : timeout ", LogLevel::WARNING, DeviceType::CAMERA);
        return QHYCCD_ERROR;
    }

    return QHYCCD_SUCCESS;
}
