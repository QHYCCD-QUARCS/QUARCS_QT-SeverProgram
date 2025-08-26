#include "tools.h"
#include <vector>
#include <QFile>
#include <QString>
#include <qdebug.h>
#include <iostream>
#include <string>
#include <vector>
#include <random>
#include <fstream>
#include <qxmlstream.h>
#include "fitsio.h"
#include <filesystem>
#include <QObject>
#include <QDebug>

// #define ImageDebug

namespace {
DriversList driversList_{};

SystemDeviceList systemDeviceList_{};

uint32_t glMainCameraExpTime_ = 10 * 1000;  // unit us
char camid_[64] = {};
QByteArray x_SDK{};
qhyccd_handle* camhandle_;
qhyccd_handle* guiderhandle_;
qhyccd_handle* polerhandle_;
qhyccd_handle* fpgahandle_;
qhyccd_handle* maincamhandle_;
}  // namespace

Tools* Tools::instance_ = new Tools();

int glChannelCount;

bool PlateSolveInProgress = false;

bool isSolveImageFinished = false;

Tools::Tools() {}

Tools::~Tools() {
// 2023.12.21 CJQ
//   if (guiderhandle_ != NULL) CloseQHYCCD(guiderhandle_);
//   if (fpgahandle_ != NULL) CloseQHYCCD(fpgahandle_);
//   if (polerhandle_ != NULL) CloseQHYCCD(polerhandle_);
//   ReleaseQHYCCDResource();
}

void Tools::Initialize() { instance_ = new Tools; }

void Tools::Release() { delete instance_; }

//----------------------------------------PHD2--Tools
uint8_t Tools::MSB(uint16_t i)
{
    uint8_t j;
    j = (i & ~0x00ff) >> 8;
    return j;
}

uint8_t Tools::LSB(uint16_t i)
{
    uint8_t j;
    j = i & ~0xff00;
    return j;
}
//----------------------------------------PHD2--Tools

DriversList& Tools::driversList() { return driversList_; }

SystemDeviceList& Tools::systemDeviceList() { return systemDeviceList_; }

bool Tools::LoadSystemListFromXml(const QString& fileName) {
  // clean list ,otherwise it will be append on the end of previous
  systemDeviceList_.system_devices.clear();

  QFile file(fileName);
  if (file.open(QIODevice::ReadOnly)) {
    QXmlStreamReader xmlReader(&file);
    while (!xmlReader.atEnd()) {
      xmlReader.readNext();
      if (xmlReader.isStartElement()) {
        if (xmlReader.name() == "SystemDeviceList") {
          systemDeviceList_.currentDeviceCode =
              xmlReader.attributes().value("currentDeviceCode").toInt();
        } else if (xmlReader.name() == "SystemDevice") {
          SystemDevice systemDevice;
          xmlReader.readNext();
          while (!(xmlReader.isEndElement() &&
                   xmlReader.name() == "SystemDevice")) {
            if (xmlReader.isStartElement()) {
              if (xmlReader.name() == "Description") {
                systemDevice.Description = xmlReader.readElementText();
              } else if (xmlReader.name() == "DeviceIndiGroup") {
                systemDevice.DeviceIndiGroup =
                    xmlReader.readElementText().toInt();
              } else if (xmlReader.name() == "DeviceIndiName") {
                systemDevice.DeviceIndiName = xmlReader.readElementText();
              } else if (xmlReader.name() == "DriverIndiName") {
                systemDevice.DriverIndiName = xmlReader.readElementText();
              } else if (xmlReader.name() == "DriverFrom") {
                systemDevice.DriverFrom = xmlReader.readElementText();
              }
            }
            xmlReader.readNext();
          }
          systemDeviceList_.system_devices.push_back(systemDevice);
        }
      }
    }
    file.close();

    if (xmlReader.hasError()) {
      Logger::Log("loadSystemListFromXml | xmlRead has ERROR", LogLevel::ERROR, DeviceType::MAIN);
      return false;
    }

  } else {
    Logger::Log("loadSystemListFromXml | ERROR: Can not open file", LogLevel::ERROR, DeviceType::MAIN);
    return false;
  }

  // Come from SelectQHYCCDSDKDevice
  systemDeviceList_.system_devices[0].Description = "Mount";
  systemDeviceList_.system_devices[1].Description = "Guider";
  systemDeviceList_.system_devices[2].Description = "PoleCamera";
  systemDeviceList_.system_devices[3].Description = "";
  systemDeviceList_.system_devices[4].Description = "";
  systemDeviceList_.system_devices[5].Description = "";
  systemDeviceList_.system_devices[20].Description = "Main Camera #1";
  systemDeviceList_.system_devices[21].Description = "CFW #1";
  systemDeviceList_.system_devices[22].Description = "Focuser #1";
  systemDeviceList_.system_devices[23].Description = "Lens Cover #1";

  return true;
}

void Tools::SaveSystemListToXml(const QString& fileName) {
  QFile file(fileName);
  if (file.open(QIODevice::WriteOnly)) {
    QXmlStreamWriter xmlWriter(&file);
    xmlWriter.setAutoFormatting(true);
    xmlWriter.writeStartDocument();
    xmlWriter.writeStartElement("SystemDeviceList");
    xmlWriter.writeAttribute(
        "currentDeviceCode",
        QString::number(systemDeviceList_.currentDeviceCode));
    for (const auto& systemDevice : systemDeviceList_.system_devices) {
      xmlWriter.writeStartElement("SystemDevice");
      xmlWriter.writeTextElement("Description", systemDevice.Description);
      xmlWriter.writeTextElement("DeviceIndiGroup",
                                 QString::number(systemDevice.DeviceIndiGroup));
      xmlWriter.writeTextElement("DeviceIndiName", systemDevice.DeviceIndiName);
      xmlWriter.writeTextElement("DriverIndiName", systemDevice.DriverIndiName);
      xmlWriter.writeTextElement("DriverFrom", systemDevice.DriverFrom);
      xmlWriter.writeEndElement();
    }
    xmlWriter.writeEndElement();
    xmlWriter.writeEndDocument();
    file.close();
  }
}

void Tools::InitSystemDeviceList() {
  // pre-define 32 devices
  systemDeviceList_.system_devices.clear();
  systemDeviceList_.system_devices.reserve(32);
  SystemDevice dev;
  dev.DeviceIndiName = "";
  dev.DeviceIndiGroup = -1;
  dev.DeviceIndiName = "";
  dev.DriverFrom = "";
  dev.isConnect = false;
  dev.dp = NULL;

  for (int i = 0; i < 32; i++) {
    systemDeviceList_.system_devices.push_back(dev);
  }
}

void Tools::CleanSystemDeviceListConnect() {
  for (int i = 0; i < systemDeviceList_.system_devices.size(); i++) {
    systemDeviceList_.system_devices[i].isConnect = false;
    systemDeviceList_.system_devices[i].dp = NULL;
  }
}

int Tools::GetTotalDeviceFromSystemDeviceList() {
  // according the deviceIndiName to get how many devices in systemDeviceList
  // This
  int i = 0;
  for (auto dev : systemDeviceList_.system_devices) {
    if (dev.DeviceIndiName != "") {
      i++;
    }
  }
  return i;
}

bool Tools::getIndexFromSystemDeviceListByName(const QString& devname, int& index) {
  int i = 0;
  for (auto dev : systemDeviceList_.system_devices) {
    if (dev.DeviceIndiName == devname) {
      index = i;
      break;
    }
    i++;
  }
  if (i < 32) {
    index = i;
    Logger::Log("getIndexFromSystemDeviceListByName | found device in system list. " + devname.toStdString() + "index" + std::to_string(index), LogLevel::INFO, DeviceType::MAIN);
    return true;
  } else {
    index = 0;
    Logger::Log("getIndexFromSystemDeviceListByName | not found device in system list, devname" + devname.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    return false;
  }
}

void Tools::ClearSystemDeviceListItem(int index) {
  // clear one device
  systemDeviceList_.system_devices[index].Description = "";
  systemDeviceList_.system_devices[index].DeviceIndiGroup = -1;
  systemDeviceList_.system_devices[index].DeviceIndiName = "";
  systemDeviceList_.system_devices[index].dp = NULL;
  systemDeviceList_.system_devices[index].DriverFrom = "";
  systemDeviceList_.system_devices[index].DriverIndiName = "";
  systemDeviceList_.system_devices[index].isConnect = false;
}

void Tools::readDriversListFromFiles(const std::string &filename, DriversList &drivers_list_from,
                              std::vector<DevGroup> &dev_groups_from, std::vector<Device> &devices_from)
{
    QFile file(QString::fromStdString(filename));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        Logger::Log("readDriversListFromFiles | Fail to open Drivers List file.", LogLevel::ERROR, DeviceType::MAIN);
        return;
    }
    QXmlStreamReader xml(&file);
    while (!xml.atEnd() && !xml.hasError())
    {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == "devGroup")
        {
            DevGroup dev_group;
            // dev_group.group = xml.attributes().value("group").toString().toStdString();
            dev_group.group = xml.attributes().value("group").toString().toUtf8().constData();
            drivers_list_from.dev_groups.push_back(dev_group);
        }
    }
    // qDebug() << "Read_XML_1";
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
        Logger::Log("readDriversListFromFiles | Unable to find INDI drivers directory, Please make sure the path is true.", LogLevel::ERROR, DeviceType::MAIN);
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
            // else if (strcmp(entry->d_name, "drivers.xml") == 0)
            // {
            //     continue;
            // }
            else
            {
                xmlpath = DirPath + entry->d_name;
                QFile file(QString::fromStdString(xmlpath));
                if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                {
                    Logger::Log("readDriversListFromFiles | Open File failed!!!", LogLevel::ERROR, DeviceType::MAIN);
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
        // qDebug() << "Read_XML_3";
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
        // qDebug() << "Read_XML_4";
        // memset(&drivers_list_xmls, 0, sizeof(DriversList));
        drivers_list_xmls = drivers_list_xmls_null;
    }
    // qDebug() << "Read_XML_5";
    printDevGroups2(drivers_list_from);
    closedir(dir);
}

void Tools::printDevGroups2(const DriversList driver_list)
{
    Logger::Log("===============Group List================", LogLevel::INFO, DeviceType::MAIN);
    // qDebug() << "Number of groups:" << driver_list.dev_groups.size();
    for (int i = 0; i < driver_list.dev_groups.size(); i++)
    {
        // qDebug() << QString::fromStdString(driver_list.dev_groups[i].group);
        Logger::Log("printDevGroups2 | " + driver_list.dev_groups[i].group.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        // qDebug() << "Number of devices:" << driver_list.dev_groups[i].devices.size();
        for (int j = 0; j < driver_list.dev_groups[i].devices.size(); j++)
        {
            Logger::Log("printDevGroups2 | " + driver_list.dev_groups[i].devices[j].driver_name + " " + driver_list.dev_groups[i].devices[j].version + " " + driver_list.dev_groups[i].devices[j].label, LogLevel::INFO, DeviceType::MAIN);
        }
    }
}

void Tools::printSystemDeviceList(const SystemDeviceList& s){
    try {
        // 在最开始就打印基本信息
        Logger::Log("===============System Device Selected================", LogLevel::INFO, DeviceType::MAIN);
        // Logger::Log("printSystemDeviceList | Function started", LogLevel::INFO, DeviceType::MAIN);
        
        // 检查引用本身是否有效（虽然引用理论上不能为空，但让我们检查一下）
        try {
            // Logger::Log("printSystemDeviceList | Checking parameter validity...", LogLevel::INFO, DeviceType::MAIN);
            
            // 尝试访问 currentDeviceCode 来测试对象是否有效
            int deviceCode = s.currentDeviceCode;
            // Logger::Log("printSystemDeviceList | currentDeviceCode: " + std::to_string(deviceCode), LogLevel::INFO, DeviceType::MAIN);
            
            // 检查 system_devices 容器本身
            // Logger::Log("printSystemDeviceList | Checking system_devices container...", LogLevel::INFO, DeviceType::MAIN);
            
            // 先检查容器是否可以安全访问
            const QVector<SystemDevice>& devices = s.system_devices;
            // Logger::Log("printSystemDeviceList | Got reference to system_devices", LogLevel::INFO, DeviceType::MAIN);
            
            // 检查容器大小
            int deviceCount = devices.size();
            // Logger::Log("printSystemDeviceList | system_devices.size(): " + std::to_string(deviceCount), LogLevel::INFO, DeviceType::MAIN);
            
            // 检查容器是否为空
            bool isEmpty = devices.isEmpty();
            // Logger::Log("printSystemDeviceList | system_devices.isEmpty(): " + std::string(isEmpty ? "true" : "false"), LogLevel::INFO, DeviceType::MAIN);
            
            // 检查容器容量
            int capacity = devices.capacity();
            // Logger::Log("printSystemDeviceList | system_devices.capacity(): " + std::to_string(capacity), LogLevel::INFO, DeviceType::MAIN);
            
            if (isEmpty) {
                // Logger::Log("printSystemDeviceList | system_devices is empty", LogLevel::WARNING, DeviceType::MAIN);
                // Logger::Log("******************************************************", LogLevel::INFO, DeviceType::MAIN);
                return;
            }
            
            if (deviceCount <= 0) {
                // Logger::Log("printSystemDeviceList | Invalid device count: " + std::to_string(deviceCount), LogLevel::ERROR, DeviceType::MAIN);
                // Logger::Log("******************************************************", LogLevel::INFO, DeviceType::MAIN);
                return;
            }
            
            if (deviceCount > 100) {
                // Logger::Log("printSystemDeviceList | Suspiciously large device count: " + std::to_string(deviceCount), LogLevel::WARNING, DeviceType::MAIN);
                // Logger::Log("******************************************************", LogLevel::INFO, DeviceType::MAIN);
                return;
            }
            
            // Logger::Log("printSystemDeviceList | Starting to iterate through devices...", LogLevel::INFO, DeviceType::MAIN);
            
            // 尝试访问第一个设备来测试
            if (deviceCount > 0) {
                try {
                    // Logger::Log("printSystemDeviceList | Checking first device...", LogLevel::INFO, DeviceType::MAIN);
                    const SystemDevice& firstDevice = devices[0];
                    // Logger::Log("printSystemDeviceList | First device accessed successfully", LogLevel::INFO, DeviceType::MAIN);
                    
                    // 检查第一个设备的基本信息
                    QString desc = firstDevice.Description;
                    // Logger::Log("printSystemDeviceList | First device Description: " + (desc.isEmpty() ? "EMPTY" : desc.toStdString()), LogLevel::INFO, DeviceType::MAIN);
                    
                    int group = firstDevice.DeviceIndiGroup;
                    // Logger::Log("printSystemDeviceList | First device DeviceIndiGroup: " + std::to_string(group), LogLevel::INFO, DeviceType::MAIN);
                    
                    bool isConnect = firstDevice.isConnect;
                    // Logger::Log("printSystemDeviceList | First device isConnect: " + std::string(isConnect ? "true" : "false"), LogLevel::INFO, DeviceType::MAIN);
                    
                    // 检查设备指针
                    INDI::BaseDevice* dp = firstDevice.dp;
                    // Logger::Log("printSystemDeviceList | First device dp: " + std::string(dp ? "NOT_NULL" : "NULL"), LogLevel::INFO, DeviceType::MAIN);
                    
                } catch (const std::exception& e) {
                    // Logger::Log("printSystemDeviceList | Error accessing first device: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
                    // Logger::Log("******************************************************", LogLevel::INFO, DeviceType::MAIN);
                    return;
                } catch (...) {
                    // Logger::Log("printSystemDeviceList | Unknown error accessing first device", LogLevel::ERROR, DeviceType::MAIN);
                    // Logger::Log("******************************************************", LogLevel::INFO, DeviceType::MAIN);
                    return;
                }
            }
            
            // 现在开始正常的循环
            // Logger::Log("printSystemDeviceList | Starting main loop...", LogLevel::INFO, DeviceType::MAIN);
            
            for (int i = 0; i < deviceCount; i++) {
                try {
                    // Logger::Log("printSystemDeviceList | Processing device " + std::to_string(i) + "...", LogLevel::INFO, DeviceType::MAIN);
                    
                    // 检查索引是否有效
                    if (i >= devices.size()) {
                        // Logger::Log("printSystemDeviceList | Index out of bounds: " + std::to_string(i), LogLevel::ERROR, DeviceType::MAIN);
                        break;
                    }
                    
                    const SystemDevice& device = devices[i];
                    // Logger::Log("printSystemDeviceList | Got device reference for index " + std::to_string(i), LogLevel::INFO, DeviceType::MAIN);
                    
                    // 获取设备名称
                    QString dpName = "NULL";
                    if (device.dp != nullptr && device.dp != NULL) {
                        // Logger::Log("printSystemDeviceList | 获取到当前设备存在 ",LogLevel::INFO,DeviceType::MAIN);
                        try {
                            const char* deviceName = device.dp->getDeviceName();
                            // Logger::Log("printSystemDeviceList | 获取到当前设备存在 1",LogLevel::INFO,DeviceType::MAIN);
                            if (deviceName != nullptr) {
                                dpName = QString::fromUtf8(deviceName);
                                if (dpName.isEmpty()) {
                                    dpName = "EMPTY_NAME";
                                }
                            } else {
                                dpName = "NULL_NAME";
                            }
                        } catch (const std::exception& e) {
                            dpName = "ERROR_GETTING_NAME";
                            // Logger::Log("printSystemDeviceList | Error getting device name for device " + std::to_string(i) + ": " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
                        } catch (...) {
                            dpName = "UNKNOWN_ERROR";
                            // Logger::Log("printSystemDeviceList | Unknown error getting device name for device " + std::to_string(i), LogLevel::ERROR, DeviceType::MAIN);
                        }
                    }
                    
                    // 检查 DriverIndiName 是否为空
                    if (!device.DriverIndiName.isEmpty()) {
                        try {
                            // 使用更安全的字符串操作
                            QString logMessage = QString("printSystemDeviceList | %1 %2 %3 %4 %5 %6 %7 %8")
                                .arg(i)
                                .arg(device.DeviceIndiGroup)
                                .arg(device.DriverFrom.isEmpty() ? "NULL" : device.DriverFrom)
                                .arg(device.DriverIndiName.isEmpty() ? "NULL" : device.DriverIndiName)
                                .arg(device.DeviceIndiName.isEmpty() ? "NULL" : device.DeviceIndiName)
                                .arg(device.Description.isEmpty() ? "NULL" : device.Description)
                                .arg(device.isConnect ? "true" : "false")
                                .arg(dpName.isEmpty() ? "NULL" : dpName);
                            
                            Logger::Log(logMessage.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                        } catch (const std::exception& e) {
                            // Logger::Log("printSystemDeviceList | Error converting strings for device " + std::to_string(i) + ": " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
                        } catch (...) {
                            // Logger::Log("printSystemDeviceList | Unknown error converting strings for device " + std::to_string(i), LogLevel::ERROR, DeviceType::MAIN);
                        }
                    }
                    
                    // Logger::Log("printSystemDeviceList | Completed processing device " + std::to_string(i), LogLevel::INFO, DeviceType::MAIN);
                    
                } catch (const std::exception& e) {
                    // Logger::Log("printSystemDeviceList | Error processing device " + std::to_string(i) + ": " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
                } catch (...) {
                    // Logger::Log("printSystemDeviceList | Unknown error processing device " + std::to_string(i), LogLevel::ERROR, DeviceType::MAIN);
                }
            }
            
            // Logger::Log("printSystemDeviceList | Completed main loop", LogLevel::INFO, DeviceType::MAIN);
            
        } catch (const std::exception& e) {
            // Logger::Log("printSystemDeviceList | Error in parameter validation: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
        } catch (...) {
            // Logger::Log("printSystemDeviceList | Unknown error in parameter validation", LogLevel::ERROR, DeviceType::MAIN);
        }
        
    } catch (const std::exception& e) {
        // Logger::Log("printSystemDeviceList | Critical error: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
    } catch (...) {
        // Logger::Log("printSystemDeviceList | Unknown critical error", LogLevel::ERROR, DeviceType::MAIN);
    }
    
    Logger::Log("******************************************************", LogLevel::INFO, DeviceType::MAIN);
}

QStringList Tools::getCameraNumFromSystemDeviceList(const SystemDeviceList& s) {
  QStringList cameras;

  if (s.system_devices[1].Description != "") {
    cameras << "Guider";
  }

  if (s.system_devices[2].Description != "") {
    cameras << "PoleCamera";
  }

  if (s.system_devices[20].Description != "") {
    cameras << "MainCamera";
  }

  return cameras;
}

void Tools::makeConfigFile() {
    std::string directory = "config";  // 要创建的文件夹名
    std::string filename = "config/config.ini";  // 配置文件路径

    // 如果目录不存在，则创建
    if (!std::filesystem::exists(directory)) {
        if (std::filesystem::create_directory(directory)) {
            Logger::Log("makeConfigFile | Configuration folder created successfully: " + directory, LogLevel::INFO, DeviceType::MAIN);
        } else {
            Logger::Log("makeConfigFile | An error occurred while creating the configuration folder.", LogLevel::ERROR, DeviceType::MAIN);
            return;  // 如果文件夹创建失败，退出
        }
    } else {
        Logger::Log("makeConfigFile | The configuration folder already exists: " + directory, LogLevel::INFO, DeviceType::MAIN);
    }

    // 如果配置文件不存在，则创建空配置文件
    if (!std::filesystem::exists(filename)) {
        std::ofstream configFile(filename);  // 在 config 文件夹中创建空文件
        if (configFile) {
            Logger::Log("makeConfigFile | Configuration file created successfully: " + filename, LogLevel::INFO, DeviceType::MAIN);
        } else {
            Logger::Log("makeConfigFile | An error occurred while creating the configuration file.", LogLevel::ERROR, DeviceType::MAIN);
        }
    } else {
        Logger::Log("makeConfigFile | The configuration file already exists: " + filename, LogLevel::INFO, DeviceType::MAIN);
    }
}

void Tools::makeImageFolder() {
    std::string directory = "image"; // 要创建的文件夹名

    // 如果目录不存在，则创建
    if (!std::filesystem::exists(directory))
    {
        if (std::filesystem::create_directory(directory))
        {
            Logger::Log("makeImageFolder | Image folder created successfully: " + directory, LogLevel::INFO, DeviceType::MAIN);

            // 创建子文件夹 CaptureImage
            std::string captureDirectory = directory + "/CaptureImage";
            if (std::filesystem::create_directory(captureDirectory))
            {
                Logger::Log("makeImageFolder | Subfolder created successfully: " + captureDirectory, LogLevel::INFO, DeviceType::MAIN);
            }
            else
            {
                Logger::Log("makeImageFolder | Error occurred while creating CaptureImage subfolders.", LogLevel::ERROR, DeviceType::MAIN);
            }

            // 创建子文件夹 ScheduleImage
            std::string scheduleDirectory = directory + "/ScheduleImage";
            if (std::filesystem::create_directory(scheduleDirectory))
            {
                Logger::Log("makeImageFolder | Subfolder created successfully: " + scheduleDirectory, LogLevel::INFO, DeviceType::MAIN);
            }
            else
            {
                Logger::Log("makeImageFolder | Error occurred while creating ScheduleImage subfolders.", LogLevel::ERROR, DeviceType::MAIN);
            }
             // 创建子文件夹 solveFailedImage
            std::string solveFailedDirectory = directory + "/solveFailedImage";
            if (std::filesystem::create_directory(solveFailedDirectory))
            {
                Logger::Log("makeImageFolder | Subfolder created successfully: " + solveFailedDirectory, LogLevel::INFO, DeviceType::MAIN);
            }
            else
            {
                Logger::Log("makeImageFolder | Error occurred while creating solveFailedImage subfolders.", LogLevel::ERROR, DeviceType::MAIN);
            }
        }
        else
        {
            Logger::Log("makeImageFolder | An error occurred while creating the image folder.", LogLevel::ERROR, DeviceType::MAIN);
        }
    }
    else
    {
        Logger::Log("makeImageFolder | The image folder already exists: " + directory, LogLevel::INFO, DeviceType::MAIN);
    }
}

void Tools::saveSystemDeviceList(SystemDeviceList deviceList) {
    std::string directory = "config";  // 配置文件夹名
    std::string filename = directory + "/config.ini";  // 配置文件路径

    // 检查文件夹是否存在，如果不存在就创建
    if (!std::filesystem::exists(directory)) {
        if (!std::filesystem::create_directory(directory)) {
            Logger::Log("saveSystemDeviceList | An error occurred while creating the configuration folder: " + directory, LogLevel::ERROR, DeviceType::MAIN);
            return;
        }
    }

    // 读取现有配置文件的内容
    std::ifstream infile(filename);
    std::stringstream fileContent;
    std::string line;
    bool isInLastConnectedDeviceSection = false;

    // 读取现有文件内容
    while (std::getline(infile, line)) {
        fileContent << line << "\n";
        // 如果遇到 [LastConnectedDevice] 部分，标记为开始位置
        if (line == "[LastConnectedDevice]") {
            isInLastConnectedDeviceSection = true;
        }
        // 如果已经在 [LastConnectedDevice] 部分，直到遇到空行或另一个部分则跳出
        if (isInLastConnectedDeviceSection && line.empty()) {
            isInLastConnectedDeviceSection = false;
        }
    }
    infile.close();

    // 如果文件已经包含了 [LastConnectedDevice] 部分，则移除旧的内容
    std::string content = fileContent.str();
    size_t pos = content.find("[LastConnectedDevice]");
    if (pos != std::string::npos) {
        size_t endPos = content.find("(End of device list)\n\n", pos) + std::string("(End of device list)\n\n").length();  // 找到 [LastConnectedDevice] 部分结束的位置
        if (endPos != std::string::npos) {
            content.erase(pos, endPos - pos);  // 删除旧的设备列表部分
        }
    }

    // 打开文件进行写入（覆盖模式）
    std::ofstream outfile(filename, std::ios::out | std::ios::trunc);  // 打开文件以覆盖内容
    if (!outfile.is_open()) {
        Logger::Log("saveSystemDeviceList | An error occurred while opening the configuration file for writing: " + filename, LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    // 先写入文件的原始内容（去掉 [LastConnectedDevice] 部分）
    outfile << content;

    // 写入新的 [LastConnectedDevice] 部分
    outfile << "[LastConnectedDevice]\n";

    // 遍历设备列表并写入到配置文件
    for (const auto& device : deviceList.system_devices) {
        QByteArray descriptionUtf8 = device.Description.toUtf8();
        QByteArray deviceIndiNameUtf8 = device.DeviceIndiName.toUtf8();
        QByteArray driverIndiNameUtf8 = device.DriverIndiName.toUtf8();
        QByteArray driverFromUtf8 = device.DriverFrom.toUtf8();

        // 写入设备信息到配置文件
        outfile << "Description=" << descriptionUtf8.constData() << "\n";
        outfile << "DeviceIndiGroup=" << device.DeviceIndiGroup << "\n";
        outfile << "DeviceIndiName=" << deviceIndiNameUtf8.constData() << "\n";
        outfile << "DriverIndiName=" << driverIndiNameUtf8.constData() << "\n";
        outfile << "DriverFrom=" << driverFromUtf8.constData() << "\n";
        outfile << "isConnect=" << (device.isConnect ? "true" : "false") << "\n";
        outfile << "\n";  // 每个设备之间空一行，便于阅读
    }
    outfile << "(End of device list)\n\n";

    // 关闭文件
    outfile.close();
    Logger::Log("saveSystemDeviceList | The device list has been saved to the configuration file: " + filename, LogLevel::INFO, DeviceType::MAIN);
}

SystemDeviceList Tools::readSystemDeviceList() {
    SystemDeviceList deviceList;
    std::string filename = "config/config.ini"; // INI 配置文件路径
    std::ifstream infile(filename);

    if (!infile.is_open()) {
        Logger::Log("readSystemDeviceList | Error occurred while opening file for reading: " + filename, LogLevel::ERROR, DeviceType::MAIN);
        return deviceList;
    }

    std::string line;
    bool inLastConnectedDeviceSection = false;
    SystemDevice currentDevice;
    std::map<std::string, std::string> sectionData;

    // 逐行读取 INI 文件
    while (std::getline(infile, line)) {
        // 检查是否进入了 [LastConnectedDevice] 部分
        if (line.find("[LastConnectedDevice]") != std::string::npos) {
            inLastConnectedDeviceSection = true;
            continue; // 跳过这一行，开始读取数据
        }

        if (inLastConnectedDeviceSection) {
            // 遇到下一个部分或空行，停止读取当前部分
            if (line.empty() || line[0] == '[') {
                // 这里保存当前设备并重置
                if (!sectionData.empty()) {
                    // 用 sectionData 填充当前设备
                    currentDevice.Description = QString::fromStdString(sectionData["Description"]);
                    currentDevice.DeviceIndiGroup = -1;
                    currentDevice.DeviceIndiName = "";
                    currentDevice.DriverIndiName = QString::fromStdString(sectionData["DriverIndiName"]);
                    currentDevice.DriverFrom = QString::fromStdString(sectionData["DriverFrom"]);
                    currentDevice.dp = NULL;
                    currentDevice.isConnect = false;
                    currentDevice.isBind = false;

                    // 将当前设备添加到设备列表
                    deviceList.system_devices.push_back(currentDevice);
                }
                // 重置
                sectionData.clear();

                if (!line.empty() && line[0] == '[') {
                    // 如果遇到新的节，跳出循环
                    break;
                }
            } else {
                // 解析键值对
                std::string key, value;
                std::istringstream lineStream(line);
                if (std::getline(std::getline(lineStream, key, '='), value)) {
                    sectionData[key] = value;
                }
            }
        }
    }

    infile.close();
    return deviceList;
}

void Tools::saveExpTimeList(QString List)
{
    std::string directory = "config";  // 配置文件夹名
    std::string filename = directory + "/config.ini";  // 配置文件路径

    // 检查文件夹是否存在，如果不存在就创建
    if (!std::filesystem::exists(directory)) {
        if (!std::filesystem::create_directory(directory)) {
            Logger::Log("saveExpTimeList | An error occurred while creating the configuration folder: " + directory, LogLevel::ERROR, DeviceType::MAIN);
            return;
        }
    }

    // 读取现有配置文件的内容
    std::ifstream infile(filename);
    std::stringstream fileContent;
    std::string line;
    bool isInExpTimeListSection = false;

    // 读取现有文件内容
    while (std::getline(infile, line)) {
        fileContent << line << "\n";
        // 如果遇到 [ExpTimeList] 部分，标记为开始位置
        if (line == "[ExpTimeList]") {
            isInExpTimeListSection = true;
        }
        // 如果已经在 [LastConnectedDevice] 部分，直到遇到空行或另一个部分则跳出
        if (isInExpTimeListSection && line.empty()) {
            isInExpTimeListSection = false;
        }
    }
    infile.close();

    // 如果文件已经包含了 [LastConnectedDevice] 部分，则移除旧的内容
    std::string content = fileContent.str();
    size_t pos = content.find("[ExpTimeList]");
    if (pos != std::string::npos) {
        size_t endPos = content.find("(End of ExpTime list)\n\n", pos) + std::string("(End of ExpTime list)\n\n").length();  // 找到 [ExpTimeList] 部分结束的位置
        if (endPos != std::string::npos) {
            content.erase(pos, endPos - pos);  // 删除旧的设备列表部分
        }
    }

    // 打开文件进行写入（覆盖模式）
    std::ofstream outfile(filename, std::ios::out | std::ios::trunc);  // 打开文件以覆盖内容
    if (!outfile.is_open()) {
        Logger::Log("saveExpTimeList | An error occurred while opening the configuration file for writing: " + filename, LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    // 先写入文件的原始内容（去掉 [LastConnectedDevice] 部分）
    outfile << content;

    // 写入新的 [LastConnectedDevice] 部分
    outfile << "[ExpTimeList]\n";

    QByteArray ExpTimeListUtf8 = List.toUtf8();

    // 将 QString 转换为 UTF-8 格式的字符串，并保存
    outfile << "ExpTimeList=" << ExpTimeListUtf8.constData() << "\n";

    outfile << "(End of ExpTime list)\n\n";

    outfile.close();
}

void Tools::saveParameter(const QString& deviceCategory, const QString& functionCategory, const QString& parameterValue) {
    std::string directory = "config";  // 配置文件夹名
    std::string filename = directory + "/config.ini";  // 配置文件路径

    // 检查文件夹是否存在，如果不存在就创建
    if (!std::filesystem::exists(directory)) {
        if (!std::filesystem::create_directory(directory)) {
            Logger::Log("saveParameter | An error occurred while creating the configuration folder: " + directory, LogLevel::ERROR, DeviceType::MAIN);
            return;
        }
    }

    // 读取现有配置文件的内容
    std::ifstream infile(filename);
    std::stringstream fileContent;
    std::string line;
    bool isInSection = false;
    std::string sectionHeader = "[" + deviceCategory.toStdString() + "]";
    std::string endMarker = "(End of " + deviceCategory.toStdString() + " Parameter)";

    // 读取现有文件内容
    while (std::getline(infile, line)) {
        fileContent << line << "\n";
    }
    infile.close();

    // 查找并更新指定部分
    std::string content = fileContent.str();
    size_t pos = content.find(sectionHeader);
    if (pos != std::string::npos) {
        size_t endPos = content.find(endMarker, pos);
        if (endPos != std::string::npos) {
            // 查找并更新现有参数
            size_t paramPos = content.find(functionCategory.toStdString() + "=", pos);
            if (paramPos != std::string::npos && paramPos < endPos) {
                size_t lineEnd = content.find("\n", paramPos);
                content.replace(paramPos, lineEnd - paramPos, functionCategory.toStdString() + "=" + parameterValue.toStdString());
            } else {
                // 在结束标记之前插入新的参数
                content.insert(endPos, functionCategory.toStdString() + "=" + parameterValue.toStdString() + "\n");
            }
        } else {
            // 如果没有找到结束标记，添加新的参数和结束标记
            content.insert(pos + sectionHeader.length(), "\n" + functionCategory.toStdString() + "=" + parameterValue.toStdString() + "\n" + endMarker + "\n");
        }
    } else {
        // 如果没有找到该部分，添加新的部分
        content += sectionHeader + "\n" + functionCategory.toStdString() + "=" + parameterValue.toStdString() + "\n" + endMarker + "\n\n";
    }

    // 打开文件进行写入（覆盖模式）
    std::ofstream outfile(filename, std::ios::out | std::ios::trunc);  // 打开文件以覆盖内容
    if (!outfile.is_open()) {
        Logger::Log("saveParameter | An error occurred while opening the configuration file for writing: " + filename, LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    // 写入更新后的内容
    outfile << content;
    outfile.close();
}

QMap<QString, QString> Tools::readParameters(const QString& deviceCategory) {
    std::string filename = "config/config.ini";  // 配置文件路径
    std::ifstream infile(filename);

    QMap<QString, QString> parameters;

    if (!infile.is_open()) {
        Logger::Log("readParameters | Error occurred while opening file for reading: " + filename, LogLevel::ERROR, DeviceType::MAIN);
        return parameters;
    }

    std::string line;
    bool inSection = false;
    std::string sectionHeader = "[" + deviceCategory.toStdString() + "]";
    std::string endMarker = "(End of " + deviceCategory.toStdString() + " Parameter)";

    // 逐行读取 INI 文件
    while (std::getline(infile, line)) {
        // 检查是否进入了指定的部分
        if (line == sectionHeader) {
            inSection = true;
            continue; // 跳过这一行，开始读取数据
        }

        if (inSection) {
            // 遇到结束标记或下一个部分，停止读取当前部分
            if (line.empty() || line == endMarker || line[0] == '[') {
                break;
            }

            // 解析键值对
            std::string key, value;
            std::istringstream lineStream(line);
            if (std::getline(std::getline(lineStream, key, '='), value)) {
                parameters.insert(QString::fromStdString(key), QString::fromStdString(value));
            }
        }
    }

    infile.close();
    return parameters;
}

QString Tools::readExpTimeList()
{
    QString ExpTimeList;
    std::string filename = "config/config.ini"; // INI 配置文件路径
    std::ifstream infile(filename);

    if (!infile.is_open()) {
        Logger::Log("readExpTimeList | Error occurred while opening file for reading: " + filename, LogLevel::ERROR, DeviceType::MAIN);
        return QString();
    }

    std::string line;
    bool inExpTimeListSection = false;
    SystemDevice currentDevice;
    std::map<std::string, std::string> sectionData;

    // 逐行读取 INI 文件
    while (std::getline(infile, line)) {
        // 检查是否进入了 [LastConnectedDevice] 部分
        if (line.find("[ExpTimeList]") != std::string::npos) {
            inExpTimeListSection = true;
            continue; // 跳过这一行，开始读取数据
        }

        if (inExpTimeListSection) {
            // 遇到下一个部分或空行，停止读取当前部分
            if (line.empty() || line[0] == '[') {
                // 这里保存当前设备并重置
                if (!sectionData.empty()) {
                    // 用 sectionData 填充当前设备
                    ExpTimeList = QString::fromStdString(sectionData["ExpTimeList"]);
                }
                // 重置
                sectionData.clear();

                if (!line.empty() && line[0] == '[') {
                    // 如果遇到新的节，跳出循环
                    break;
                }
            } else {
                // 解析键值对
                std::string key, value;
                std::istringstream lineStream(line);
                if (std::getline(std::getline(lineStream, key, '='), value)) {
                    sectionData[key] = value;
                }
            }
        }
    }

    infile.close();
    return ExpTimeList;
}

void Tools::saveCFWList(QString Name, QString List)
{
    std::string directory = "config";  // 配置文件夹名
    std::string filename = directory + "/config.ini";  // 配置文件路径

    // 检查文件夹是否存在，如果不存在就创建
    if (!std::filesystem::exists(directory)) {
        if (!std::filesystem::create_directory(directory)) {
            Logger::Log("saveCFWList | An error occurred while creating the configuration folder: " + directory, LogLevel::ERROR, DeviceType::MAIN);
            return;
        }
    }

    // 读取现有配置文件的内容
    std::ifstream infile(filename);
    std::stringstream fileContent;
    std::string line;
    bool isInCFWListSection = false;

    // 读取现有文件内容
    while (std::getline(infile, line)) {
        fileContent << line << "\n";
        // 如果遇到 [CFWList] 部分，标记为开始位置
        if (line == "[CFWList(" + Name.toStdString() +")]") {
            isInCFWListSection = true;
        }
        // 如果已经在 [LastConnectedDevice] 部分，直到遇到空行或另一个部分则跳出
        if (isInCFWListSection && line.empty()) {
            isInCFWListSection = false;
        }
    }
    infile.close();

    // 如果文件已经包含了 [LastConnectedDevice] 部分，则移除旧的内容
    std::string content = fileContent.str();
    size_t pos = content.find("[CFWList(" + Name.toStdString() +")]");
    if (pos != std::string::npos) {
        size_t endPos = content.find("(End of CFW list)\n\n", pos) + std::string("(End of CFW list)\n\n").length();  
        if (endPos != std::string::npos) {
            content.erase(pos, endPos - pos);  // 删除旧的设备列表部分
        }
    }

    // 打开文件进行写入（覆盖模式）
    std::ofstream outfile(filename, std::ios::out | std::ios::trunc);  // 打开文件以覆盖内容
    if (!outfile.is_open()) {
        Logger::Log("saveCFWList | An error occurred while opening the configuration file for writing: " + filename, LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    // 先写入文件的原始内容（去掉 [LastConnectedDevice] 部分）
    outfile << content;

    // 写入新的 [LastConnectedDevice] 部分
    outfile << "[CFWList(" + Name.toStdString() +")]\n";

    QByteArray CFWListUtf8 = List.toUtf8();

    // 将 QString 转换为 UTF-8 格式的字符串，并保存
    outfile << "CFWList=" << CFWListUtf8.constData() << "\n";

    outfile << "(End of CFW list)\n\n";

    outfile.close();
}

QString Tools::readCFWList(QString Name)
{
    QString CFWList;
    std::string filename = "config/config.ini"; // INI 配置文件路径
    std::ifstream infile(filename);

    if (!infile.is_open()) {
        Logger::Log("readCFWList | Error occurred while opening file for reading: " + filename, LogLevel::ERROR, DeviceType::MAIN);
        return QString();
    }

    std::string line;
    bool inCFWListSection = false;
    SystemDevice currentDevice;
    std::map<std::string, std::string> sectionData;

    // 逐行读取 INI 文件
    while (std::getline(infile, line)) {
        // 检查是否进入了 [LastConnectedDevice] 部分
        if (line.find("[CFWList(" + Name.toStdString() +")]") != std::string::npos) {
            inCFWListSection = true;
            continue; // 跳过这一行，开始读取数据
        }

        if (inCFWListSection) {
            // 遇到下一个部分或空行，停止读取当前部分
            if (line.empty() || line[0] == '[') {
                // 这里保存当前设备并重置
                if (!sectionData.empty()) {
                    // 用 sectionData 填充当前设备
                    CFWList = QString::fromStdString(sectionData["CFWList"]);
                }
                // 重置
                sectionData.clear();

                if (!line.empty() && line[0] == '[') {
                    // 如果遇到新的节，跳出循环
                    break;
                }
            } else {
                // 解析键值对
                std::string key, value;
                std::istringstream lineStream(line);
                if (std::getline(std::getline(lineStream, key, '='), value)) {
                    sectionData[key] = value;
                }
            }
        }
    }

    infile.close();
    return CFWList;
}

void Tools::saveDSLRsInfo(DSLRsInfo DSLRsInfo)
{
  std::string directory = "config";                 // 配置文件夹名
  std::string filename = directory + "/config.ini"; // 配置文件路径

  // 检查文件夹是否存在，如果不存在就创建
  if (!std::filesystem::exists(directory))
  {
    if (!std::filesystem::create_directory(directory))
    {
      Logger::Log("saveDSLRsInfo | An error occurred while creating the configuration folder: " + directory, LogLevel::ERROR, DeviceType::MAIN);
      return;
    }
  }

  // 读取现有配置文件的内容
  std::ifstream infile(filename);
  std::stringstream fileContent;
  std::string line;
  bool isInDSLRsInfoSection = false;

  // 读取现有文件内容
  while (std::getline(infile, line))
  {
    fileContent << line << "\n";
    // 如果遇到 [DSLRsInfo] 部分，标记为开始位置
    if (line == "[DSLRsInfo(" + DSLRsInfo.Name.toStdString() + ")]")
    {
      isInDSLRsInfoSection = true;
    }
    // 如果已经在 [DSLRsInfo] 部分，直到遇到空行或另一个部分则跳出
    if (isInDSLRsInfoSection && line.empty())
    {
      isInDSLRsInfoSection = false;
    }
  }
  infile.close();

  // 如果文件已经包含了 [DSLRsInfo] 部分，则移除旧的内容
  std::string content = fileContent.str();
  size_t pos = content.find("[DSLRsInfo(" + DSLRsInfo.Name.toStdString() + ")]");
  if (pos != std::string::npos)
  {
    size_t endPos = content.find("(End of DSLR Info)\n\n", pos) + std::string("(End of DSLR Info)\n\n").length();
    if (endPos != std::string::npos)
    {
      content.erase(pos, endPos - pos); // 删除旧的设备列表部分
    }
  }

  // 打开文件进行写入（覆盖模式）
  std::ofstream outfile(filename, std::ios::out | std::ios::trunc); // 打开文件以覆盖内容
  if (!outfile.is_open())
  {
    Logger::Log("saveDSLRsInfo | An error occurred while opening the configuration file for writing: " + filename, LogLevel::ERROR, DeviceType::MAIN);
    return;
  }

  // 先写入文件的原始内容（去掉 [DSLRsInfo] 部分）
  outfile << content;

  // 写入新的 [DSLRsInfo] 部分
  outfile << "[DSLRsInfo(" + DSLRsInfo.Name.toStdString() + ")]\n";

  // 将 QString 转换为 UTF-8 格式的字符串，并保存
  outfile << "DSLRsSizeX=" << DSLRsInfo.SizeX << "\n";
  outfile << "DSLRsSizeY=" << DSLRsInfo.SizeY << "\n";
  outfile << "DSLRsPixelSize=" << DSLRsInfo.PixelSize << "\n";

  outfile << "(End of DSLR Info)\n\n";

  outfile.close();
}

DSLRsInfo Tools::readDSLRsInfo(QString Name)
{
  DSLRsInfo DSLRsInfo;
  DSLRsInfo.Name = "";
  std::string filename = "config/config.ini"; // INI 配置文件路径
  std::ifstream infile(filename);

  if (!infile.is_open())
  {
    Logger::Log("readDSLRsInfo | Error occurred while opening file for reading: " + filename, LogLevel::ERROR, DeviceType::MAIN);
    return DSLRsInfo;
  }

  std::string line;
  bool inDSLRsInfoSection = false;
  std::map<std::string, std::string> sectionData;

  // 逐行读取 INI 文件
  while (std::getline(infile, line))
  {
    // 检查是否进入了 [DSLRsInfo] 部分
    if (line.find("[DSLRsInfo(" + Name.toStdString() + ")]") != std::string::npos)
    {
      inDSLRsInfoSection = true;
      DSLRsInfo.Name = Name;
      continue; // 跳过这一行，开始读取数据
    }

    if (inDSLRsInfoSection)
    {
      // 遇到下一个部分或空行，停止读取当前部分
      if (line.empty() || line[0] == '[')
      {
        // 这里保存当前设备并重置
        if (!sectionData.empty())
        {
          // 用 sectionData 填充当前设备
          DSLRsInfo.SizeX = sectionData.count("DSLRsSizeX") ? QString::fromStdString(sectionData["DSLRsSizeX"]).toInt() : 0;
          DSLRsInfo.SizeY = sectionData.count("DSLRsSizeY") ? QString::fromStdString(sectionData["DSLRsSizeY"]).toInt() : 0;
          DSLRsInfo.PixelSize = sectionData.count("DSLRsPixelSize") ? QString::fromStdString(sectionData["DSLRsPixelSize"]).toDouble() : 0.0;
        }
        // 重置
        sectionData.clear();

        if (!line.empty() && line[0] == '[')
        {
          // 如果遇到新的节，跳出循环
          break;
        }
      }
      else
      {
        // 解析键值对
        std::string key, value;
        std::istringstream lineStream(line);
        if (std::getline(std::getline(lineStream, key, '='), value))
        {
          sectionData[key] = value;
        }
      }
    }
  }

  infile.close();
  return DSLRsInfo;
}

void Tools::readClientSettings(const std::string& fileName, std::unordered_map<std::string, std::string>& config) {
    std::ifstream file(fileName);
    if (!file.is_open()) {
        Logger::Log("readClientSettings | Error occurred while opening file for reading: " + fileName, LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    std::string line;
    std::string currentSection;

    while (std::getline(file, line)) {
        // 去除空白字符
        line.erase(0, line.find_first_not_of(" \t\n\r"));
        line.erase(line.find_last_not_of(" \t\n\r") + 1);

        if (line.empty()) continue;  // 跳过空行

        // 处理区块头，找到[ClientSettings]等部分
        if (line[0] == '[' && line[line.length() - 1] == ']') {
            currentSection = line.substr(1, line.length() - 2);
            continue;
        }

        // 处理[ClientSettings]中的内容
        if (currentSection == "ClientSettings") {
            size_t pos = line.find("=");
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
                // 去除多余的空格
                key.erase(0, key.find_first_not_of(" \t"));
                key.erase(key.find_last_not_of(" \t") + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of(" \t") + 1);
                config[key] = value;
            }
        }
    }

    file.close();
}

void Tools::saveClientSettings(const std::string& fileName, const std::unordered_map<std::string, std::string>& config) {
  // 读取文件内容
  std::ifstream inputFile(fileName);
  std::stringstream fileContents;
  fileContents << inputFile.rdbuf();
  inputFile.close();

  std::string content = fileContents.str();
  std::string section = "[ClientSettings]\n";
  size_t sectionPos = content.find(section);

  if (sectionPos == std::string::npos)
  {
    // 如果没有找到 [ClientSettings] 部分，则需要添加该部分
    content += "\n" + section + "\n";
    sectionPos = content.size() - 1; // 设定新的位置
  }
  else
  {
    // 找到 [ClientSettings] 部分后，定位到此部分结束的位置
    size_t sectionEnd = content.find("\n[", sectionPos + section.size());
    if (sectionEnd == std::string::npos)
    {
      sectionEnd = content.size();
    }

    // 进入 [ClientSettings] 部分并将配置项读取到内存中
    std::string clientSettingsSection = content.substr(sectionPos, sectionEnd - sectionPos);

    // 更新配置项
    for (const auto &pair : config)
    {
      // 查找是否已有该配置项
      size_t pos = clientSettingsSection.find(pair.first + " = ");
      if (pos != std::string::npos)
      {
        // 找到了该项，更新其值
        size_t endPos = clientSettingsSection.find("\n", pos);
        clientSettingsSection.replace(pos + pair.first.size() + 3, endPos - pos - pair.first.size() - 3, pair.second);
        Logger::Log("updateClientSettings | " + pair.first + " = " + pair.second, LogLevel::INFO, DeviceType::MAIN);
      }
      else
      {
        // 没有找到该项，添加新的配置项
        clientSettingsSection += pair.first + " = " + pair.second + "\n";
        Logger::Log("addClientSettings | " + pair.first + " = " + pair.second, LogLevel::INFO, DeviceType::MAIN);
      }
    }

    // 替换原来的部分
    content.replace(sectionPos, sectionEnd - sectionPos, clientSettingsSection);
  }

  // 将更新后的内容写回到文件
  std::ofstream outputFile(fileName);
  outputFile << content;
  outputFile.close();
}

void Tools::clearSystemDeviceListItem(SystemDeviceList &s,int index){
    //clear one device
    Logger::Log("clearSystemDeviceListItem | index:" + std::to_string(index), LogLevel::INFO, DeviceType::MAIN);
    if (s.system_devices.empty()) {
        Logger::Log("clearSystemDeviceListItem | s.system_devices is nullptr", LogLevel::INFO, DeviceType::MAIN);
    }
    else {
        s.system_devices[index].Description="";
        s.system_devices[index].DeviceIndiGroup=-1;
        s.system_devices[index].DeviceIndiName="";
        s.system_devices[index].dp=NULL;
        s.system_devices[index].DriverFrom="";
        s.system_devices[index].DriverIndiName="";
        s.system_devices[index].isConnect=false;
        Logger::Log("clearSystemDeviceListItem | SystemDeviceListItem already cleared.", LogLevel::INFO, DeviceType::MAIN);
    }
}

void Tools::initSystemDeviceList(SystemDeviceList &s){
    s.system_devices.clear();
    s.system_devices.reserve(32); //pre-define 32 devices
    SystemDevice dev;
    dev.DeviceIndiName="";
    dev.DeviceIndiGroup=-1;
    dev.DeviceIndiName="";
    dev.DriverFrom="";      //DriverFrom 用于存储驱动类型。如果来自于INDI，则是"INDI"  如果来自于QHYCCD SDK  则是"QHYCCDSDK"
    dev.isConnect=false;
    dev.dp=NULL;

    for(int i=0;i<32;i++){
        s.system_devices.push_back(dev);
    }
}

int Tools::getTotalDeviceFromSystemDeviceList(const SystemDeviceList& s){
    //according the deviceIndiName to get how many devices in systemDeviceList
    //This
    int i=0;
    for(const auto& dev : s.system_devices){
        if(dev.DeviceIndiName !="") i++;
    }
    return i;
}

int Tools::getDriverNumFromSystemDeviceList(const SystemDeviceList& s){
    int i=0;
    for(const auto& dev : s.system_devices){
        if(dev.DriverIndiName !="") i++;
    }
    return i;
}

void Tools::cleanSystemDeviceListConnect(SystemDeviceList &s){
    for (int i=0;i<s.system_devices.size();i++){
        s.system_devices[i].isConnect=false;
        s.system_devices[i].dp=NULL;
    }
}

uint32_t Tools::getIndexFromSystemDeviceListByName(const SystemDeviceList& s,QString devname,int &index){
    int i=0;
    for(const auto& dev : s.system_devices){
        if (dev.DeviceIndiName == devname ){
            index = i;
            break;
        }
        i++;
    }
    if(i<32) {
        index=i;
        Logger::Log("getIndexFromSystemDeviceListByName | found device in system list. device name" + devname.toStdString() + "index" + std::to_string(index), LogLevel::INFO, DeviceType::MAIN);
        return QHYCCD_SUCCESS;
    }
    else{
        index=0;
        Logger::Log("getIndexFromSystemDeviceListByName | not found device in system list, devname" + devname.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        return QHYCCD_ERROR;
    }
}

void Tools::startIndiDriver(QString driver_name)
{
    QString s;
    s = "echo ";
    s.append("\"start ");
    s.append(driver_name);
    s.append("\"");
    s.append("> /tmp/myFIFO");
    system(s.toUtf8().constData());
    // qDebug() << "startIndiDriver" << driver_name;
    Logger::Log("startIndiDriver | Start Connecting INDI Driver : " + driver_name.toStdString(), LogLevel::INFO, DeviceType::MAIN);
}

void Tools::stopIndiDriver(QString driver_name)
{
    QString s;
    s = "echo ";
    s.append("\"stop ");
    s.append(driver_name);
    s.append("\"");
    s.append("> /tmp/myFIFO");
    system(s.toUtf8().constData());
    // qDebug() << "stopIndiDriver" << driver_name;
}

void Tools::stopIndiDriverAll(const DriversList driver_list)
{
    // before each connection. need to stop all of the indi driver
    // need to make sure disconnect all the driver for first. If the driver is under operaiton, stop it may cause crash
    bool indiserver = false;
    // TO BE FIXED  need to check if server is running
    indiserver = true;
    if (!indiserver)
    {
        Logger::Log("stopIndiDriverAll | ERROR | INDI DRIVER NOT running", LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    for (int i = 0; i < driver_list.dev_groups.size(); i++)
    {
        //qDebug() << "------" << QString::fromStdString(driver_list.dev_groups[i].group) << "------";
        for (int j = 0; j < driver_list.dev_groups[i].devices.size(); j++)
        {
            stopIndiDriver(QString::fromStdString(driver_list.dev_groups[i].devices[j].driver_name));
        }
    }
}

uint32_t Tools::readFitsHeadForDevName(std::string filename, QString &devname)
{
    // this api is to readout the devname in the FITs . Can used to identity the image for the newBLOB

    fitsfile *fptr;       // FITS file pointer
    int status = 0;       // CFITSIO status value MUST be initialized to zero!
    char card[FLEN_CARD]; // Standard string lengths defined in fitsio.h

    // Open the FITS file
    // std::string filename = "filename.fits";
    fits_open_file(&fptr, filename.c_str(), READONLY, &status);
    if (status)
    {
        fits_report_error(stderr, status); // Print error message
        return QHYCCD_ERROR;
    }

    // Read and print header information
    int nkeys;                                      // Number of header keywords
    fits_get_hdrspace(fptr, &nkeys, NULL, &status); // Get number of keywords
    for (int i = 1; i <= nkeys; i++)
    { // Read and print each keyword
        fits_read_record(fptr, i, card, &status);
        // qDebug()<<card;

        QString s = card;

        if (s.contains("INSTRUME") == true)
        {
            // qDebug()<<s;
            int a = s.indexOf("'");
            int b = s.lastIndexOf("'");
            // qDebug()<<a<<b;
            devname = s.mid(a + 1, b - a - 1);
            // qDebug()<<devname;
        }
    }

    // Close the FITS file
    fits_close_file(fptr, &status);
    if (status)
    {
        fits_report_error(stderr, status); // Print error message
        return QHYCCD_ERROR;
    }
}

int Tools::readFits(const char* fileName, cv::Mat& image) {
    fitsfile* fptr;
    int status = 0;
    int bitpix, naxis;
    long naxes[2];
    long nelements;
    void* array = nullptr;

    // 打开 FITS 文件
    if (fits_open_file(&fptr, fileName, READONLY, &status)) {
        return status;
    }

    // 读取图像信息
    if (fits_get_img_param(fptr, 2, &bitpix, &naxis, naxes, &status)) {
        fits_close_file(fptr, &status);
        return status;
    }

    // 确保图像是二维的且尺寸有效
    if (naxis != 2 || naxes[0] <= 0 || naxes[1] <= 0) {
        fits_close_file(fptr, &status);
        return -1; // 无效图像维度或尺寸
    }

    // 动态分配内存并读取数据
    nelements = naxes[0] * naxes[1];
    if (bitpix == 8) {
        array = new uint8_t[nelements];
        if (fits_read_img(fptr, TBYTE, 1, nelements, NULL, array, NULL, &status)) {
            delete[] static_cast<uint8_t*>(array);
            fits_close_file(fptr, &status);
            return status;
        }
        image = cv::Mat(naxes[1], naxes[0], CV_8U, array).clone(); // 深拷贝
        delete[] static_cast<uint8_t*>(array); // 释放原数组
    } else if (bitpix == 16) {
        array = new uint16_t[nelements];
        if (fits_read_img(fptr, TUSHORT, 1, nelements, NULL, array, NULL, &status)) {
            delete[] static_cast<uint16_t*>(array);
            fits_close_file(fptr, &status);
            return status;
        }
        image = cv::Mat(naxes[1], naxes[0], CV_16U, array).clone(); // 深拷贝
        delete[] static_cast<uint16_t*>(array); // 释放原数组
    } else {
        fits_close_file(fptr, &status);
        return -2; // 不支持的位深度
    }

    // 关闭文件
    fits_close_file(fptr, &status);
    return status;
}

QString Tools::getFitsCaptureTime(const char* fileName) {
    fitsfile* fptr;
    int status = 0;
    char dateObs[30]; // 用于存储拍摄时间字符串

    // 打开 FITS 文件
    if (fits_open_file(&fptr, fileName, READONLY, &status)) {
        return QString(); // 返回空 QString 以表示错误
    }

    // 获取拍摄时间
    if (fits_read_key(fptr, TSTRING, "DATE-OBS", dateObs, NULL, &status)) {
        fits_close_file(fptr, &status);
        return QString(); // 返回空 QString 以表示错误
    }

    // 关闭文件
    fits_close_file(fptr, &status);
    
    // 返回 QString 类型的拍摄时间
    return QString::fromStdString(dateObs);
}

int Tools::readFits_(const char* fileName, cv::Mat& image) {
    fitsfile *fptr;
    int status = 0;

    fits_open_file(&fptr, fileName, READONLY, &status);
    if (status != 0) {
        fits_report_error(stderr, status);
        return false;
    }

    int bitpix, naxis;
    long naxes[2] = {1, 1};
    fits_get_img_param(fptr, 2, &bitpix, &naxis, naxes, &status);
    if (status != 0) {
        fits_report_error(stderr, status);
        return false;
    }

    long fpixel[2] = {1, 1};
    image.create(naxes[1], naxes[0], CV_32F);
    fits_read_pix(fptr, TFLOAT, fpixel, naxes[0]*naxes[1], nullptr, image.data, nullptr, &status);
    if (status != 0) {
        fits_report_error(stderr, status);
        return false;
    }

    fits_close_file(fptr, &status);
    if (status != 0) {
        fits_report_error(stderr, status);
        return false;
    }

    return true;
}

void Tools::ConnectQHYCCDSDK() {
  // Connnect the FPGA board, QHY5III290 Guide/Solve camera and PoleMaster

  uint32_t ret;
  uint16_t index, value;
  ret = InitQHYCCDResource();
  // EnableQHYCCDMessage(true);
  Logger::Log("initqhyccdresosurce " + std::to_string(ret), LogLevel::INFO, DeviceType::MAIN);
  uint32_t devices = 0;

  devices = ScanQHYCCD();
  Logger::Log("found qhyccd device " + std::to_string(devices), LogLevel::INFO, DeviceType::MAIN);

  if (devices < 1) {
    Logger::Log("SelectQHYCCDSDKDevice | No QHYCCD SDK Device Found", LogLevel::INFO, DeviceType::MAIN);
    return;
  }

  char cameraName[11];
  cameraName[10] = '\0';  // on debian linux it need this . on ubutun linux it
                          // can work with or without this.
  for (int i = 0; i < devices; i++) {
    ret = GetQHYCCDId(i, camid_);
    // qDebug("device name: %s",camid_);

    memcpy(cameraName, camid_, 10);
    // qDebug("cameraName: %s",cameraName);

    QDataStream stream(&x_SDK, QIODevice::WriteOnly);
    stream << cameraName;

    if (strcmp(cameraName, "QHY5III485") == 0) {
      fpgahandle_ = OpenQHYCCD(camid_);
      Logger::Log("Found FPGA device:" + std::to_string(reinterpret_cast<uintptr_t>(fpgahandle_)), LogLevel::INFO, DeviceType::MAIN);
    } else if (strcmp(cameraName, "QHY5III178") == 0) {
      guiderhandle_ = OpenQHYCCD(camid_);
      Logger::Log("Found guider device:" + std::to_string(reinterpret_cast<uintptr_t>(guiderhandle_)), LogLevel::INFO, DeviceType::MAIN);
    } else if (strcmp(cameraName, "POLEMASTER") == 0) {
      // polerhandle_ = OpenQHYCCD(camid_);
      guiderhandle_ = OpenQHYCCD(camid_);
      Logger::Log("Found poler device:" + std::to_string(reinterpret_cast<uintptr_t>(guiderhandle_)), LogLevel::INFO, DeviceType::MAIN);
    }
  }
}

void Tools::ScanCamera() {
  if (Tools::systemDeviceList().currentDeviceCode != 1) {
    int ret;
    camhandle_ = OpenQHYCCD(camid_);
    if (camhandle_ != NULL) {
      Logger::Log("Open QHYCCD success.\n", LogLevel::INFO, DeviceType::MAIN);
    } else {
      Logger::Log("Open QHYCCD failure.\n", LogLevel::INFO, DeviceType::MAIN);
    }

    ret = IsQHYCCDControlAvailable(camhandle_, CAM_SINGLEFRAMEMODE);
    if (QHYCCD_ERROR == ret) {
      Logger::Log("The detected camera is not support single frame.", LogLevel::INFO, DeviceType::MAIN);
      // release sdk resources
      ret = ReleaseQHYCCDResource();
      if (QHYCCD_SUCCESS == ret) {
        Logger::Log("SDK resources released.", LogLevel::INFO, DeviceType::MAIN);
      } else {
        Logger::Log("Cannot release SDK resources, error:" + std::to_string(ret), LogLevel::INFO, DeviceType::MAIN);
      }
    }

    int mode = 0;
    ret = SetQHYCCDStreamMode(camhandle_, mode);
    if (QHYCCD_SUCCESS == ret) {
      Logger::Log("SetQHYCCDStreamMode set to:" + std::to_string(mode) + "success.", LogLevel::INFO, DeviceType::MAIN);
    } else {
      Logger::Log("SetQHYCCDStreamMode:" + std::to_string(mode) + "failure, error:" + std::to_string(ret), LogLevel::INFO, DeviceType::MAIN);
    }
    Logger::Log("\033[0m\033[1;35minitialize camera\033[0m", LogLevel::INFO, DeviceType::MAIN);
    // initialize camera
    ret = InitQHYCCD(camhandle_);
    if (QHYCCD_SUCCESS == ret) {
      Logger::Log("InitQHYCCD success.", LogLevel::INFO, DeviceType::MAIN);
    } else {
      Logger::Log("InitQHYCCD faililure, error:" + std::to_string(ret), LogLevel::INFO, DeviceType::MAIN);
    }
  }
  if ((Tools::systemDeviceList().currentDeviceCode >= 0) &&
      (Tools::systemDeviceList().currentDeviceCode <
       Tools::systemDeviceList().system_devices.size())) {
    Tools::systemDeviceList()
        .system_devices[Tools::systemDeviceList().currentDeviceCode]
        .isConnect = true;
  }
}

void Tools::SelectQHYCCDSDKDevice(int systemNumber) {
  Tools::systemDeviceList().currentDeviceCode = systemNumber;
  // QHYCCDSDK has no Groupd define.
  Tools::driversList().selectedGrounp = -1;
}

cv::Mat Tools::Capture() {
  double expTime_sec;
  expTime_sec = (double)glMainCameraExpTime_ / 1000 / 1000;

  int USB_TRAFFIC = 10;
  int CHIP_GAIN = 10;
  int CHIP_OFFSET = 140;
  int EXPOSURE_TIME = glMainCameraExpTime_;
  int camBinX = 1;
  int camBinY = 1;

  double chipWidthMM;
  double chipHeightMM;
  double pixelWidthUM;
  double pixelHeightUM;

  unsigned int roiStartX;
  unsigned int roiStartY;
  unsigned int roiSizeX;
  unsigned int roiSizeY;

  unsigned int overscanStartX;
  unsigned int overscanStartY;
  unsigned int overscanSizeX;
  unsigned int overscanSizeY;

  unsigned int effectiveStartX;
  unsigned int effectiveStartY;
  unsigned int effectiveSizeX;
  unsigned int effectiveSizeY;

  unsigned int maxImageSizeX;
  unsigned int maxImageSizeY;
  unsigned int bpp;
  unsigned int channels;

  unsigned char* pImgData = 0;
  int ret;

  ret = GetQHYCCDOverScanArea(camhandle_, &overscanStartX, &overscanStartY,
                              &overscanSizeX, &overscanSizeY);
  if (QHYCCD_SUCCESS == ret) {
    Logger::Log("GetQHYCCDOverScanArea success", LogLevel::INFO, DeviceType::MAIN);
  } else {
    Logger::Log("GetQHYCCDOverScanArea error", LogLevel::INFO, DeviceType::MAIN);
    return {};
  }
  ret = GetQHYCCDOverScanArea(camhandle_, &effectiveStartX, &effectiveStartY,
                              &effectiveSizeX, &effectiveSizeY);
  if (QHYCCD_SUCCESS == ret) {
    Logger::Log("GetQHYCCDEffectiveArea success", LogLevel::INFO, DeviceType::MAIN);
  } else {
    Logger::Log("GetQHYCCDEffectiveArea error", LogLevel::INFO, DeviceType::MAIN);
    return {};
  }
  ret =
      GetQHYCCDChipInfo(camhandle_, &chipWidthMM, &chipHeightMM, &maxImageSizeX,
                        &maxImageSizeY, &pixelWidthUM, &pixelHeightUM, &bpp);
  if (QHYCCD_SUCCESS == ret) {
    Logger::Log("GetQHYCCDChipInfo success", LogLevel::INFO, DeviceType::MAIN);
  } else {
    Logger::Log("GetQHYCCDChipInfo error", LogLevel::INFO, DeviceType::MAIN);
    return {};
  }

  roiStartX = 0;
  roiStartY = 0;
  roiSizeX = maxImageSizeX;
  roiSizeY = maxImageSizeY;

  ret = IsQHYCCDControlAvailable(camhandle_, CAM_COLOR);
  if (ret == BAYER_GB || ret == BAYER_GR || ret == BAYER_BG ||
      ret == BAYER_RG) {
    Logger::Log("This is a color camera.", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("even this is a color camera, in Single Frame mode THE SDK "
                "ONLY SUPPORT RAW OUTPUT.So please do not set "
                "SetQHYCCDDebayerOnOff() to true;", LogLevel::INFO, DeviceType::MAIN);
  } else {
    Logger::Log("This is a mono camera.", LogLevel::INFO, DeviceType::MAIN);
  }

  ret = IsQHYCCDControlAvailable(camhandle_, CONTROL_USBTRAFFIC);
  if (QHYCCD_SUCCESS == ret) {
    ret = SetQHYCCDParam(camhandle_, CONTROL_USBTRAFFIC, USB_TRAFFIC);
    if (QHYCCD_SUCCESS == ret) {
      Logger::Log("SetQHYCCDParam CONTROL_USBTRAFFIC set to:" + std::to_string(USB_TRAFFIC)
               + "success.", LogLevel::INFO, DeviceType::MAIN);
    } else {
      Logger::Log("SetQHYCCDParam CONTROL_USBTRAFFIC error", LogLevel::INFO, DeviceType::MAIN);
      getchar();
      return {};
    }
  }

  ret = IsQHYCCDControlAvailable(camhandle_, CONTROL_GAIN);
  if (QHYCCD_SUCCESS == ret) {
    ret = SetQHYCCDParam(camhandle_, CONTROL_GAIN, CHIP_GAIN);
    if (QHYCCD_SUCCESS == ret) {
      Logger::Log("SetQHYCCDParam CONTROL_GAIN set to:" + std::to_string(CHIP_GAIN)
               + "success.", LogLevel::INFO, DeviceType::MAIN);
    } else {
      Logger::Log("SetQHYCCDParam CONTROL_GAIN error", LogLevel::INFO, DeviceType::MAIN);
      getchar();
      return {};
    }
  }

  ret = IsQHYCCDControlAvailable(camhandle_, CONTROL_OFFSET);
  if (QHYCCD_SUCCESS == ret) {
    ret = SetQHYCCDParam(camhandle_, CONTROL_OFFSET, CHIP_OFFSET);
    if (QHYCCD_SUCCESS == ret) {
      Logger::Log("SetQHYCCDParam CONTROL_OFFSET set to:" + std::to_string(CHIP_OFFSET)
               + "success.", LogLevel::INFO, DeviceType::MAIN);
    } else {
      Logger::Log("SetQHYCCDParam CONTROL_OFFSET failed.", LogLevel::INFO, DeviceType::MAIN);
      getchar();
      return {};
    }
  }

  ret = SetQHYCCDParam(camhandle_, CONTROL_EXPOSURE, EXPOSURE_TIME);
  if (QHYCCD_SUCCESS == ret) {
    Logger::Log("SetQHYCCDParam CONTROL_EXPOSURE set to:" + std::to_string(EXPOSURE_TIME)
             + "success.", LogLevel::INFO, DeviceType::MAIN);
  } else {
    Logger::Log("SetQHYCCDParam CONTROL_EXPOSURE failure", LogLevel::INFO, DeviceType::MAIN);
    getchar();
    return {};
  }

  ret =
      SetQHYCCDResolution(camhandle_, roiStartX, roiStartY, roiSizeX, roiSizeY);
  if (QHYCCD_SUCCESS == ret) {
    Logger::Log("SetQHYCCDResolution success.", LogLevel::INFO, DeviceType::MAIN);
  } else {
    Logger::Log("SetQHYCCDResolution error.", LogLevel::INFO, DeviceType::MAIN);
    return {};
  }

  ret = SetQHYCCDBinMode(camhandle_, camBinX, camBinY);
  if (QHYCCD_SUCCESS == ret) {
    Logger::Log("SetQHYCCDBinMode success.", LogLevel::INFO, DeviceType::MAIN);
  } else {
    Logger::Log("SetQHYCCDBinMode error.", LogLevel::INFO, DeviceType::MAIN);
    return {};
  }

  ret = IsQHYCCDControlAvailable(camhandle_, CONTROL_TRANSFERBIT);
  if (QHYCCD_SUCCESS == ret) {
    ret = SetQHYCCDBitsMode(camhandle_, 16);
    if (QHYCCD_SUCCESS == ret) {
      Logger::Log("SetQHYCCDBitsMode success.", LogLevel::INFO, DeviceType::MAIN);
    } else {
      Logger::Log("SetQHYCCDBitsMode error", LogLevel::INFO, DeviceType::MAIN);
      getchar();
      return {};
    }
  }

  Logger::Log("ExpQHYCCDSingleFrame(camhandle) - start...", LogLevel::INFO, DeviceType::MAIN);
  ret = ExpQHYCCDSingleFrame(camhandle_);
  Logger::Log("ExpQHYCCDSingleFrame(camhandle) - end...", LogLevel::INFO, DeviceType::MAIN);
  if (QHYCCD_ERROR != ret) {
    Logger::Log("ExpQHYCCDSingleFrame success.", LogLevel::INFO, DeviceType::MAIN);
    if (QHYCCD_READ_DIRECTLY != ret) {
      QElapsedTimer t;
      t.start();

      QThread::usleep(glMainCameraExpTime_);

      qDebug() << t.elapsed();
    }
  } else {
    Logger::Log("ExpQHYCCDSingleFrame failure, error", LogLevel::INFO, DeviceType::MAIN);
  }

  uint32_t length = GetQHYCCDMemLength(camhandle_);

  if (length > 0) {
    pImgData = new unsigned char[length];
    memset(pImgData, 0, length);
    Logger::Log("Allocated memory for frame:" + std::to_string(length), LogLevel::INFO, DeviceType::MAIN);
  } else {
    Logger::Log("Cannot allocate memory for frame.", LogLevel::INFO, DeviceType::MAIN);
    return {};
  }

  QElapsedTimer t;
  t.start();
  cv::Mat mmat;

  ret = GetQHYCCDSingleFrame(camhandle_, &roiSizeX, &roiSizeY, &bpp, &channels,
                             pImgData);
  if (QHYCCD_SUCCESS == ret) {
    Logger::Log("GetQHYCCDSingleFrame success.", LogLevel::INFO, DeviceType::MAIN);
    // process image here

    // emit signalRefreshMainPageMainCameraImage(pImgData,"MONO");

    mmat = cv::Mat(maxImageSizeY, maxImageSizeX, CV_16UC1, pImgData, 0);

    std::vector<int> creat_quality;
    creat_quality.push_back(cv::IMWRITE_PNG_COMPRESSION);
    creat_quality.push_back(0);
    cv::imwrite("/dev/shm/SDK_Capture.png", mmat, creat_quality);
    mmat = mmat.clone();
  } else {
    Logger::Log("GetQHYCCDSingleFrame error", LogLevel::INFO, DeviceType::MAIN);
    return {};
  }

  delete[] pImgData;

  Logger::Log("t.elapsed():" + std::to_string(t.elapsed()), LogLevel::INFO, DeviceType::MAIN);

  /*
  ret = CancelQHYCCDExposingAndReadout(camhandle_);
  if (QHYCCD_SUCCESS == ret) {
    qDebug() << "CancelQHYCCDExposingAndReadout success.";
  } else {
    qDebug() << "CancelQHYCCDExposingAndReadout error";
    return {};
  }
  */

  // cv::Mat img;
  // img=imread("/home/q/Pictures/1.jpg",0);

  // Mat img;
  // img.create(5000,6000,CV_8UC3);

  // showCvImageOnQLabelA(img,MainPageMainCameraImage);
  return mmat;
}

int Tools::CFW() {
  // step:  (1) display the CFW selector QLabel
  //        (2) read the min, max, pos of the by getCFWPosition
  //        (3) generate the button dynamicly and add the button to the QLabel
  //        (4)
  //

  uint32_t ret;
  int pos, min = 1, max;

  ret = IsQHYCCDCFWPlugged(camhandle_);  // 检查滤镜轮连接状态
  if (ret == QHYCCD_SUCCESS) {
    qDebug("CFW is plugged.");
    max = GetQHYCCDParam(camhandle_,
                         CONTROL_CFWSLOTSNUM);  // 获取滤镜轮孔数
    return max;
  } else {
    qDebug("CFW is NULL.");
    return 0;
  }
}

void Tools::SetCFW(int cfw) {
  uint32_t ret;
  ret = SetQHYCCDParam(camhandle_, CONTROL_CFWPORT,
                       47.0 + cfw);  // 设置目标孔位
  if (ret == QHYCCD_SUCCESS) {
    double status;
    while (status != 47.0 + cfw)  // 循环获取位置，判断是否转到目标位置
    {
      status = GetQHYCCDParam(camhandle_,
                              CONTROL_CFWPORT);  // 获取当前位置
      // sleep(500);//延时 500ms
      QThread::msleep(500);
      qDebug() << "current location:" << status;
    }
  }
}

uint32_t& Tools::glMainCameraExpTime() { return glMainCameraExpTime_; }

bool Tools::WriteFPGA(uint8_t hand, int command) {
  if (hand == 0xa0) {
    int dir = command;
    switch (dir) {
      case 1: {
        return SetQHYCCDWriteFPGA(camhandle_, 0, 0xa0, 0x01) == QHYCCD_SUCCESS;
      }
      case 2: {
        return SetQHYCCDWriteFPGA(camhandle_, 0, 0xa0, 0x02) == QHYCCD_SUCCESS;
      }
      case 3: {
        return SetQHYCCDWriteFPGA(camhandle_, 0, 0xa0, 0x03) == QHYCCD_SUCCESS;
      }
      case 4: {
        return SetQHYCCDWriteFPGA(camhandle_, 0, 0xa0, 0x04) == QHYCCD_SUCCESS;
      }
      default:
        break;
    }
  }
  if (hand == 0xa1) {
    int dir = command;
    switch (dir) {
      case 1: {
        return SetQHYCCDWriteFPGA(camhandle_, 0, 0xa1, 0x01) == QHYCCD_SUCCESS;
      }
      case 0: {
        return SetQHYCCDWriteFPGA(camhandle_, 0, 0xa1, 0x00) == QHYCCD_SUCCESS;
      }
      default:
        break;
    }
  }
  if (hand == 0xa2) {
    int qq = command;

    uint8_t m_com;
    bool ret = true;

    m_com = qq / (256 * 256 * 256 * 256 * 256 * 256 * 256);
    ret = ret &&
          (SetQHYCCDWriteFPGA(camhandle_, 0, 0xa2, m_com) == QHYCCD_SUCCESS);

    m_com = qq % (256 * 256 * 256 * 256 * 256 * 256 * 256) /
            (256 * 256 * 256 * 256 * 256 * 256);
    ret = ret &&
          (SetQHYCCDWriteFPGA(camhandle_, 0, 0xa3, m_com) == QHYCCD_SUCCESS);

    m_com = qq % (256 * 256 * 256 * 256 * 256 * 256 * 256) %
            (256 * 256 * 256 * 256 * 256 * 256) / (256 * 256 * 256 * 256 * 256);
    ret = ret &&
          (SetQHYCCDWriteFPGA(camhandle_, 0, 0xa4, m_com) == QHYCCD_SUCCESS);

    m_com = qq % (256 * 256 * 256 * 256 * 256 * 256 * 256) %
            (256 * 256 * 256 * 256 * 256 * 256) %
            (256 * 256 * 256 * 256 * 256) / (256 * 256 * 256 * 256);
    ret = ret &&
          (SetQHYCCDWriteFPGA(camhandle_, 0, 0xa5, m_com) == QHYCCD_SUCCESS);

    m_com = qq % (256 * 256 * 256 * 256 * 256 * 256 * 256) %
            (256 * 256 * 256 * 256 * 256 * 256) %
            (256 * 256 * 256 * 256 * 256) % (256 * 256 * 256 * 256) /
            (256 * 256 * 256);
    ret = ret &&
          (SetQHYCCDWriteFPGA(camhandle_, 0, 0xa6, m_com) == QHYCCD_SUCCESS);

    m_com = qq % (256 * 256 * 256 * 256 * 256 * 256 * 256) %
            (256 * 256 * 256 * 256 * 256 * 256) %
            (256 * 256 * 256 * 256 * 256) % (256 * 256 * 256 * 256) %
            (256 * 256 * 256) / (256 * 256);
    ret = ret &&
          (SetQHYCCDWriteFPGA(camhandle_, 0, 0xa7, m_com) == QHYCCD_SUCCESS);

    m_com = qq % (256 * 256 * 256 * 256 * 256 * 256 * 256) %
            (256 * 256 * 256 * 256 * 256 * 256) %
            (256 * 256 * 256 * 256 * 256) % (256 * 256 * 256 * 256) %
            (256 * 256 * 256) % (256 * 256) / 256;
    ret = ret &&
          (SetQHYCCDWriteFPGA(camhandle_, 0, 0xa8, m_com) == QHYCCD_SUCCESS);

    m_com = qq % (56 * 256 * 256 * 256 * 256 * 256 * 256) %
            (256 * 256 * 256 * 256 * 256 * 256) %
            (256 * 256 * 256 * 256 * 256) % (256 * 256 * 256 * 256) %
            (256 * 256 * 256) % (256 * 256) % 256;
    ret = ret &&
          (SetQHYCCDWriteFPGA(camhandle_, 0, 0xa9, m_com) == QHYCCD_SUCCESS);

    return ret;
  }
  //               if(m_hand=="0xa3"){
  //                 int qq=QString(command).toInt();
  //                 int m_com;
  //                 m_com=qq/(256*256*256);
  //                 SetQHYCCDWriteFPGA(camhandle_,0,0xaa,m_com);
  //                 m_com=qq%(256*256*256)/(256*256);
  //                 SetQHYCCDWriteFPGA(camhandle_,0,0xab,m_com);
  //                 m_com=qq%(256*256*256)%(256*256)/256;
  //                 SetQHYCCDWriteFPGA(camhandle_,0,0xac,m_com);
  //                 m_com=qq%(256*256*256)%(256*256)%256;
  //                 SetQHYCCDWriteFPGA(camhandle_,0,0xad,m_com);
  //                    wss_sendText(glClientIP,glClientPort,x.command_UID);
  //               }

  if (hand == 0xa4) {
    int dir = command;
    switch (dir) {
      case 1: {
        return SetQHYCCDWriteFPGA(camhandle_, 0, 0x9f, 0x01) == QHYCCD_SUCCESS;
      }
      case 0: {
        return SetQHYCCDWriteFPGA(camhandle_, 0, 0x9f, 0x00) == QHYCCD_SUCCESS;
      }
      default:
        break;
    }
  }

  return false;
}

char* Tools::camid() { return camid_; }

qhyccd_handle*& Tools::camhandle() { return camhandle_; }

qhyccd_handle*& Tools::guiderhandle() { return guiderhandle_; }

qhyccd_handle*& Tools::polerhandle() { return polerhandle_; }

qhyccd_handle*& Tools::fpgahandle() { return fpgahandle_; }

qhyccd_handle*& Tools::maincamhandle() { return maincamhandle_; }

void Tools::CvDebugShow(cv::Mat img) {
  int randomInt = rand();
  std::string name = std::to_string(randomInt);
  name = "test";
  cv::namedWindow(name, 0);
  cv::resizeWindow(name, 640, 480);
  // cv::imshow(name, img);
}

QImage Tools::ShowHistogram(const cv::Mat& image,QLabel *label) {
  // 将输入图像分成三个通道
  #ifdef ImageDebug
  Logger::Log("showHistogram |" + std::to_string(image.channels()), LogLevel::INFO, DeviceType::MAIN);
  #endif
  QElapsedTimer t;
  t.start();

  QImage ret;

  if (image.channels() == 3) {
    #ifdef ImageDebug
    Logger::Log("showHistogram | Draw Histograme color", LogLevel::INFO, DeviceType::MAIN);
    #endif
    std::vector<cv::Mat> channels;
    cv::split(image, channels);

    // 计算每个通道的直方图
    int bins = 1024;
    float range[] = {0, 65536};
    const float* histRange = {range};
    cv::Mat b_hist, g_hist, r_hist;

    cv::calcHist(&channels[0], 1, 0, cv::Mat(), b_hist, 1, &bins, &histRange);
    cv::calcHist(&channels[1], 1, 0, cv::Mat(), g_hist, 1, &bins, &histRange);
    cv::calcHist(&channels[2], 1, 0, cv::Mat(), r_hist, 1, &bins, &histRange);

    double min_b, min_g, min_r;
    double max_b, max_g, max_r;
    cv::Point min_loc_b, min_loc_g, min_loc_r;
    cv::Point max_loc_b, max_loc_g, max_loc_r;

    cv::minMaxLoc(b_hist, &min_b, &max_b, &min_loc_b, &max_loc_b);
    cv::minMaxLoc(g_hist, &min_g, &max_g, &min_loc_g, &max_loc_g);
    cv::minMaxLoc(r_hist, &min_r, &max_r, &min_loc_r, &max_loc_r);

    // for color image. need to use the global min_value as the same threshold
    // for three channel.
    int min_maxval = std::min({max_b, max_g, max_r});
    #ifdef ImageDebug
    Logger::Log("showHistogram | Draw Histograme color | min of max value in 3 "
                "channels" + std::to_string(min_maxval), LogLevel::INFO, DeviceType::MAIN);
    #endif
    // qDebug()<<min_maxval;

    cv::threshold(b_hist, b_hist, min_maxval, min_maxval, cv::THRESH_TRUNC);
    cv::threshold(g_hist, g_hist, min_maxval, min_maxval, cv::THRESH_TRUNC);
    cv::threshold(r_hist, r_hist, min_maxval, min_maxval, cv::THRESH_TRUNC);

    // 归一化直方图
    cv::normalize(b_hist, b_hist, 0, 200, cv::NORM_MINMAX, -1, cv::Mat());
    cv::normalize(g_hist, g_hist, 0, 200, cv::NORM_MINMAX, -1, cv::Mat());
    cv::normalize(r_hist, r_hist, 0, 200, cv::NORM_MINMAX, -1, cv::Mat());

    // 绘制直方图
    int width = bins;
    int height = 200;
    int bin_w = cvRound((double)width / bins);
    cv::Mat histImage(height, width, CV_8UC3, cv::Scalar(0, 0, 0));
    for (int i = 1; i < bins; i++) {
      cv::line(
          histImage,
          cv::Point(bin_w * (i - 1), height - cvRound(b_hist.at<float>(i - 1))),
          cv::Point(bin_w * (i), height - cvRound(b_hist.at<float>(i))),
          cv::Scalar(255, 0, 0));
      cv::line(
          histImage,
          cv::Point(bin_w * (i - 1), height - cvRound(g_hist.at<float>(i - 1))),
          cv::Point(bin_w * (i), height - cvRound(g_hist.at<float>(i))),
          cv::Scalar(0, 255, 0));
      cv::line(
          histImage,
          cv::Point(bin_w * (i - 1), height - cvRound(r_hist.at<float>(i - 1))),
          cv::Point(bin_w * (i), height - cvRound(r_hist.at<float>(i))),
          cv::Scalar(0, 0, 255));
    }

    // 将直方图转换为 QPixmap
    // cv::Mat rgb;

    cv::Mat histImageSmall;
    cv::resize(histImage, histImageSmall, cv::Size(800, 200));

    ret = QImage((unsigned char*)histImage.data, histImage.cols, histImage.rows,
                 QImage::Format_RGB888);

    
    // namedWindow("test",WINDOW_NORMAL );
    // cv::imshow("test",histImage);

    // 在 QLabel 上显示 QPixmap
    // label->setScaledContents(true);
    // label->setPixmap(pixmap);
    QImage qimage(histImage.data, histImage.cols, histImage.rows, QImage::Format_RGB888);
    QPixmap pixmap = QPixmap::fromImage(qimage);
    label->setPixmap(pixmap);
    label->setScaledContents(true);

  } else {
    #ifdef ImageDebug
    Logger::Log("showHistogram | Draw Histograme mono", LogLevel::INFO, DeviceType::MAIN);
    #endif
    // 绘制直方图
    cv::Mat hist;
    int bins = 1024;
    float range[] = {0, 65536};
    const float* histRange = {range};
    cv::calcHist(&image, 1, 0, cv::Mat(), hist, 1, &bins, &histRange);
    int width = bins;
    int height = 200;
    int bin_w = cvRound((double)width / bins);
    cv::Mat histImage(height, width, CV_8UC3, cv::Scalar(0, 0, 0));

    // cv::log(hist,hist);
    cv::normalize(hist, hist, 0, height, cv::NORM_MINMAX, -1, cv::Mat());
    for (int i = 1; i < bins; i++) {
      cv::line(
          histImage,
          cv::Point(bin_w * (i - 1), height - cvRound(hist.at<float>(i - 1))),
          cv::Point(bin_w * (i), height - cvRound(hist.at<float>(i))),
          cv::Scalar(255, 255, 255));
    }

    cv::Mat histImageSmall;
    cv::resize(histImage, histImageSmall, cv::Size(800, 200));

    // 将直方图转换为 QPixmap
    // cv::Mat rgb;
    // cv::cvtColor(histImageSmall, rgb, cv::COLOR_BGR2RGB);
    ret = QImage((unsigned char*)histImageSmall.data, histImageSmall.cols,
                 histImageSmall.rows, QImage::Format_RGB888);

    // 在 QLabel 上显示 QPixmap
    // label->setScaledContents(true);
    // label->setPixmap(pixmap);
    QImage qimage(histImage.data, histImage.cols, histImage.rows, QImage::Format_RGB888);
    QPixmap pixmap = QPixmap::fromImage(qimage);
    label->setPixmap(pixmap);
    label->setScaledContents(true);
  }
  #ifdef ImageDebug
  Logger::Log("showHistogram | used time(ms) " + std::to_string(t.elapsed()), LogLevel::INFO, DeviceType::MAIN);
  #endif
  return ret;
}

void Tools::PaintHistogram(cv::Mat src,QLabel *label)
{
    int channelCount = src.channels();
    std::cout << "通道数: " << channelCount << std::endl;
    glChannelCount = channelCount;

    cv::Mat gray;
    cv::cvtColor(src, gray, CV_BGR2GRAY);//转换为灰度图像

    const int channels[] = { 0 };
    cv::Mat hist;//定义输出Mat类型
    int dims = 1;//设置直方图维度
    const int histSize_gray[] = { 256 }; //直方图每一个维度划分的柱条的数目
    //每一个维度取值范围
    float pranges[] = { 0, 255 };//取值区间
    const float* ranges[] = { pranges };

    cv::calcHist(&gray, 1, channels, cv::Mat(), hist, dims, histSize_gray, ranges, true, false);//计算直方图


    std::vector<cv::Mat>bgr_planes;
    cv::split(src, bgr_planes);

    int histSize = 256;
    float range[] = { 0,256 };
    const float*Ranges = { range };
    cv::Mat b_hist, g_hist, r_hist;

    if(channelCount == 3)
    {
        cv::calcHist(&bgr_planes[0], 1, 0, cv::Mat(), b_hist, 1, &histSize, &Ranges, true, false);
        cv::calcHist(&bgr_planes[1], 1, 0, cv::Mat(), g_hist, 1, &histSize, &Ranges, true, false);
        cv::calcHist(&bgr_planes[2], 1, 0, cv::Mat(), r_hist, 1, &histSize, &Ranges, true, false);
    
        double min_b, min_g, min_r;
        double max_b, max_g, max_r;
        cv::Point min_loc_b, min_loc_g, min_loc_r;
        cv::Point max_loc_b, max_loc_g, max_loc_r;

        cv::minMaxLoc(b_hist, &min_b, &max_b, &min_loc_b, &max_loc_b);
        cv::minMaxLoc(g_hist, &min_g, &max_g, &min_loc_g, &max_loc_g);
        cv::minMaxLoc(r_hist, &min_r, &max_r, &min_loc_r, &max_loc_r);


        //for color image. need to use the global min_value as the same threshold for three channel.
        int min_maxval = std::min({max_b, max_g, max_r});
        #ifdef ImageDebug
        Logger::Log("showHistogram | Draw Histograme color | min of max value in 3 channels" + std::to_string(min_maxval), LogLevel::INFO, DeviceType::MAIN);
        #endif
        //qDebug()<<min_maxval;

        cv::threshold(b_hist, b_hist, min_maxval, min_maxval, cv::THRESH_TRUNC);
        cv::threshold(g_hist, g_hist, min_maxval, min_maxval, cv::THRESH_TRUNC);
        cv::threshold(r_hist, r_hist, min_maxval, min_maxval, cv::THRESH_TRUNC);
    }
    //归一化
    int hist_w = 500;//直方图的图像的宽
    int hist_h = 300; //直方图的图像的高
    int nHistSize = 256;
    int bin_w = cvRound((double)hist_w / nHistSize);	//区间
    cv::Mat histImage(hist_h, hist_w, CV_8UC3, cv::Scalar(0, 0, 0));//绘制直方图显示的图像

    if(channelCount == 3)
    {
        cv::normalize(b_hist, b_hist, 0, histImage.rows, cv::NORM_MINMAX, -1, cv::Mat());//归一化
        cv::normalize(g_hist, g_hist, 0, histImage.rows, cv::NORM_MINMAX, -1, cv::Mat());
        cv::normalize(r_hist, r_hist, 0, histImage.rows, cv::NORM_MINMAX, -1, cv::Mat());
    }
    cv::normalize(hist, hist, 0, histImage.rows, cv::NORM_MINMAX, -1, cv::Mat());//将直方图归一化到[0,histImage.rows]

    for (int i = 1; i < nHistSize; i++)
    {
        if(channelCount == 3)
        {
            //绘制蓝色分量直方图
            cv::line(histImage, cv::Point((i - 1)*bin_w, hist_h - cvRound(b_hist.at<float>(i - 1))),
                 cv::Point((i)*bin_w, hist_h - cvRound(b_hist.at<float>(i))), cv::Scalar(255, 0, 0));
            //绘制绿色分量直方图
            cv::line(histImage, cv::Point((i - 1)*bin_w, hist_h - cvRound(g_hist.at<float>(i - 1))),
                 cv::Point((i)*bin_w, hist_h - cvRound(g_hist.at<float>(i))), cv::Scalar(0, 255, 0));
            //绘制红色分量直方图
            cv::line(histImage, cv::Point((i - 1)*bin_w, hist_h - cvRound(r_hist.at<float>(i - 1))),
                 cv::Point((i)*bin_w, hist_h - cvRound(r_hist.at<float>(i))), cv::Scalar(0, 0, 255));
        }
        //绘制总直方图
        cv::line(histImage, cv::Point(bin_w*(i - 1), hist_h - cvRound(hist.at<float>(i - 1))),
             cv::Point(bin_w*(i), hist_h - cvRound(hist.at<float>(i))), cv::Scalar(255, 255, 255));
    }

    cv::imshow("直方图1",histImage);

    QImage qimage(histImage.data, histImage.cols, histImage.rows, QImage::Format_BGR888);
    QPixmap pixmap = QPixmap::fromImage(qimage);
    label->setPixmap(pixmap);
    label->setScaledContents(true);
}

void Tools::HistogramStretch(cv::Mat src,double Min,double Max)
{
    cv::Mat grayImage;
    cv::cvtColor(src, grayImage, cv::COLOR_BGR2GRAY);

    double alpha = 255.0/(Max-Min);
    double beta = -Min * alpha;

    if(alpha < 0)
    {
        alpha = 0;
        beta = 0;
    }else if(alpha > 255)
    {
        alpha = 255;
        beta = 0;
    }

    cv::Mat StretchImage;
    if(glChannelCount == 3)
    {
        std::vector<cv::Mat> channels;
        cv::split(src,channels);

        for (int i = 0; i < 3; i++) {
            cv::Mat channel = channels[i];
            cv::Mat stretchedChannel;
            cv::convertScaleAbs(channel, stretchedChannel, alpha, beta);
            channels[i] = stretchedChannel;
        }

        // 合并通道
        cv::merge(channels, StretchImage);
    }
    else
    {
        grayImage.convertTo(StretchImage,-1,alpha,beta);
    }

    cv::imshow("Stretch",StretchImage);
}

void Tools::AutoStretch(cv::Mat src)
{
    // cv::Mat gray;
    // cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
    
    // cv::Mat equ;
    // cv::equalizeHist(gray,equ);

    // cv::Mat res;
    // cv::hconcat(gray,equ,res);

    // cv::imshow("拼接",res);

    int channelCount = src.channels();

    if(channelCount == 3)
    {
      std::vector<cv::Mat> channels;
      cv::split(src, channels);

      for (int i = 0; i < channels.size(); ++i) {
          double minVal, maxVal;
          cv::minMaxLoc(channels[i], &minVal, &maxVal);
          channels[i].convertTo(channels[i], CV_8U, 255.0 / (maxVal - minVal), -minVal * 255.0 / (maxVal - minVal));
      }

      cv::Mat stretchedImage;
      cv::merge(channels, stretchedImage);
    }else
    {
      double minVal, maxVal;
      cv::minMaxLoc(src, &minVal, &maxVal);

      cv::Mat stretchedImage;
      src.convertTo(stretchedImage, CV_8U, 255.0 / (maxVal - minVal), -minVal * 255.0 / (maxVal - minVal));
    }
    
}

void Tools::ShowCvImageOnQLabel(cv::Mat image,QLabel *label) {
  // Convert the OpenCV image to a QImage
  QElapsedTimer t;
  t.start();

  QImage qtImage = QImage((const unsigned char*)(image.data),image.cols, image.rows, QImage::Format_RGB888);
    // Set the QLabel's image to the QImage
  label->setPixmap(QPixmap::fromImage(qtImage));
  #ifdef ImageDebug
  Logger::Log("showOpenCV_QLabel_withRotate | used time(ms) " + std::to_string(t.elapsed()), LogLevel::INFO, DeviceType::MAIN);
  #endif
}

void Tools::ShowOpenCV_QLabel_withRotate(cv::Mat img,QLabel *label,int RotateType,double cmdRotate_Degree,double AzAltToRADEC_Degree) {
  //RotateType: 0 = no rotate   1 = rotate to RADEC    2 = rotate to AZALT
    QElapsedTimer t;
    t.start();

    unsigned int w,h,bpp,channels;

    w=img.cols;
    h=img.rows;
    #ifdef ImageDebug
    Logger::Log("showOpenCV_QLabel_withRotate | " + std::to_string(w) + " " + std::to_string(h) + " " + std::to_string(img.channels()) + " " + std::to_string(img.type()), LogLevel::INFO, DeviceType::MAIN);
    #endif
    cv::Mat imgRGB888;
    imgRGB888.create(h,w,CV_8UC3);

    //convert all kinds of image type to RGB888
    //input image is 3channel 8bit image
    if(img.type()==CV_8UC3){
       cvtColor(img,imgRGB888,CV_BGR2RGB);
       //img.copyTo(imgRGB888);
    }
    else if(img.type()==CV_8UC1){
       cvtColor(img,imgRGB888,CV_GRAY2BGR);
    }
    else if(img.type()==CV_16UC1){

        long s=0;
        long k=1;

        for (int j=0;j<h;j++){
            for(int i=0;i<w;i++){
                imgRGB888.data[s]   =img.data[k];
                imgRGB888.data[s+1] =img.data[k];
                imgRGB888.data[s+2] =img.data[k];

                s=s+3;
                k=k+2;
            }
        }
    }
    else {
      #ifdef ImageDebug
       Logger::Log(" showOpenCV_QLabel_withRotate | ERROR : unsupport image type", LogLevel::INFO, DeviceType::MAIN);
       #endif
    }

    // circle(imgRGB888,cv::Point(w/2,w/2),10,CV_RGB(0, 0, 255),1,8,0);  //make a mark in center point of the image

    unsigned int byteCount;
    byteCount=w*h*3;  //3 channel 8bit image
    QByteArray p_w_picpathByteArray = QByteArray( (const char*)imgRGB888.data,  byteCount );
    uchar *transData = (unsigned char*)p_w_picpathByteArray.data();
    QImage *simg = new QImage(transData,w,h,QImage::Format_RGB888);

    QPixmap pix;
    pix=QPixmap::fromImage(*simg);

    int center_x=w/2;
    int center_y=h/2;

    //rotate:  180-cmdRoate_Degree will rotate to the RADEC system.  - RadToDegree(glRotateDataAzAltToRADEC_radian) will rotate from RADEC to AZALT system
    int nDegree;

    if (RotateType==0)      nDegree = 0;
    else if(RotateType==1)  nDegree = 180-cmdRotate_Degree;
    else if(RotateType==2)  nDegree = 180-cmdRotate_Degree -AzAltToRADEC_Degree;// cmdRotate_Degree-180;

    QMatrix mat;

    mat.translate(center_x,center_y);
    mat.rotate((nDegree));
    mat.translate(-center_x,-center_y);


    QPixmap new_pix;
    new_pix=pix.transformed(mat,Qt::SmoothTransformation);
    QPainter paint(&new_pix);

    unsigned int aleft= new_pix.width()/2-w/2;
    unsigned int atop = new_pix.height()/2-h/2;


    pix=new_pix.copy(aleft,atop,w,h);

    label->setPixmap(pix);
    //delete simg;
    imgRGB888.release();
  #ifdef ImageDebug
    Logger::Log("showOpenCV_QLabel_withRotate | used time(ms) " + std::to_string(t.elapsed()), LogLevel::INFO, DeviceType::MAIN);
  #endif
}

void Tools::ImageSoftAWB(cv::Mat sourceImg16, cv::Mat& targetImg16, QString CFA,
                         double gainR, double gainB, uint16_t offset) {
  QElapsedTimer t;
  t.start();

  int height = sourceImg16.rows;
  int width = sourceImg16.cols;

  double gain1, gain2, gain3, gain4;

  Logger::Log("CFA:" + std::string(CFA.toStdString()), LogLevel::INFO, DeviceType::MAIN);
  Logger::Log("gainR:" + std::to_string(gainR) + "," + "gainB:" + std::to_string(gainB), LogLevel::INFO, DeviceType::MAIN);

  if (CFA == "RGGB") {
    gain1 = 1.0 * gainR;
    gain2 = 1.0;
    gain3 = 1.0;
    gain4 = 1.0 * gainB;
  } else if (CFA == "GR") {
    gain1 = 1.0;
    gain2 = 1.0 * gainR;
    gain3 = 1.0 * gainB;
    gain4 = 1.0;
  } else if (CFA == "GB") {
    gain1 = 1.0;
    gain2 = 1.0 * gainB;
    gain3 = 1.0 * gainR;
    gain4 = 1.0;
  } else if (CFA == "BG") {
    gain1 = 1.0 * gainB;
    gain2 = 1.0;
    gain3 = 1.0;
    gain4 = 1.0 * gainR;
  }

  // 遍历每个像素
  for (int row = 0; row < height; row++) {
    for (int col = 0; col < width; col++) {
      // 先减去 offset，如果小于 0 则设为 0
      int pixel = sourceImg16.at<ushort>(row, col) - offset;
      if (pixel < 0) pixel = 0;

      if (row % 2 == 0 && col % 2 == 0)
        targetImg16.at<ushort>(row, col) = pixel * gain1;
      else if (row % 2 == 0 && col % 2 == 1)
        targetImg16.at<ushort>(row, col) = pixel * gain2;
      else if (row % 2 == 1 && col % 2 == 0)
        targetImg16.at<ushort>(row, col) = pixel * gain3;
      else if (row % 2 == 1 && col % 2 == 1)
        targetImg16.at<ushort>(row, col) = pixel * gain4;
    }
  }

  Logger::Log("ImageSoftAWB | used time (ms) " + std::to_string(t.elapsed()), LogLevel::INFO, DeviceType::MAIN);
}

void Tools::GetAutoStretch(cv::Mat img_raw16, int mode, uint16_t& B,
                           uint16_t& W) {
  QElapsedTimer t;
  t.start();
  cv::Scalar mean;
  cv::Scalar std;

  cv::meanStdDev(img_raw16, mean, std);

  int a, b;

  if (mode == 0) {
    a = 3;
    b = 5;
  } else if (mode == 1) {
    a = 2;
    b = 5;
  } else if (mode == 2) {
    a = 3;
    b = 8;
  } else {
    a = 2;
    b = 8;
  }

  // simple auto_stretch for astronomy

  double bx, wx;
  bx = mean.val[0] - std.val[0] * a;
  wx = mean.val[0] + std.val[0] * b;

  if (bx == wx) {
    wx = bx + 10;
  }  // avoid bx == wx

  if (bx < 0) bx = 0;
  if (wx > 65535) wx = 65535;

  B = (uint16_t)bx;
  W = (uint16_t)wx;

  // process some sepcial condtion
  // full saturated
  if (B == 65535 && W == 65535) {
    B = 0;
    W = 65535;
  }
  #ifdef ImageDebug
  Logger::Log("getAutoStretch |mean std B W" + std::to_string(mean.val[0]) + " " + std::to_string(std.val[0]) + " " + std::to_string(B) + " " + std::to_string(W), LogLevel::INFO, DeviceType::MAIN);
  Logger::Log("getAutoStretch | used time(ms) " + std::to_string(t.elapsed()), LogLevel::INFO, DeviceType::MAIN);
  #endif
}

void Tools::Bit16To8_MakeLUT(uint16_t B, uint16_t W, uint8_t* lut) {
  double ratio;
  uint32_t pixel;

  ratio = double((W - B)) / 256;

  if (ratio == 0) ratio = 1;  // avoid /zero

  for (int i = 0; i < 65536; i++) {
    pixel = i;
    if (pixel > B) {
      pixel = (uint32_t)((pixel - B) / ratio);
      if (pixel > 255) pixel = 255;
    } else
      pixel = 0;
    lut[i] = pixel;
  }
  #ifdef ImageDebug
  Logger::Log("Bit16To8_MakeLUT |" + std::to_string(B) + " " + std::to_string(W), LogLevel::INFO, DeviceType::MAIN);
  #endif
}

void Tools::Bit16To8_Stretch(cv::Mat img16, cv::Mat img8, uint16_t B,
                             uint16_t W) {
  // this API support 16bit image input, 3 channel and 1 channel
  #ifdef ImageDebug
  Logger::Log("Bit16To8_Stretch | start" + std::to_string(B) + " " + std::to_string(W), LogLevel::INFO, DeviceType::MAIN);
  #endif
  QElapsedTimer t;
  t.start();

  uint32_t pixel;
  uint32_t i, j;
  uint32_t s;
  uint32_t imageX, imageY;

  imageX = img16.cols;
  imageY = img16.rows;

  if (img16.channels() == 1) {
    uint8_t LUT16TO8[65536];
    Bit16To8_MakeLUT(B, W, LUT16TO8);

    uint16_t* data16 = (uint16_t*)img16.data;

    s = 0;

    for (i = 0; i < imageY; i++) {
      for (j = 0; j < imageX; j++) {
        pixel = data16[s];  // img16.data[k] + img16.data[k + 1] * 256;
        img8.data[s] = LUT16TO8[pixel];
        s = s + 1;
        // k = k + 1;
      }
    }

    /*

        double ratio;
        ratio = double((W - B)) / 256;
        if (ratio == 0)
        {
            ratio = 1;
        }

        for (i = 0; i < imageY; i++)
        {
            for (j = 0; j < imageX; j++)
            {
                pixel = img16.data[k] + img16.data[k + 1] * 256;
                if (pixel > B)
                {
                    pixel = (uint32_t)((pixel - B) / ratio);
                    if (pixel > 255)
                        pixel = 255;
                }
                else
                    pixel = 0;

                if (pixel > 255)
                    pixel = 255;
                img8.data[s] = pixel;
                s = s + 1;
                k = k + 2;
            }
        }
    */

  }

  else {
    uint8_t LUT16TO8R[65536];
    uint8_t LUT16TO8G[65536];
    uint8_t LUT16TO8B[65536];
    // here we use the same LUT for RGB channel
    Bit16To8_MakeLUT(B, W, LUT16TO8R);
    memcpy(LUT16TO8G, LUT16TO8R, 65536);
    memcpy(LUT16TO8B, LUT16TO8R, 65536);

    // this code will take only 50ms for 6000*4000 color image (under release).
    uint16_t* data16;

    data16 = (uint16_t*)img16.data;

    s = 0;
    for (i = 0; i < imageY; i++) {
      for (j = 0; j < imageX; j++) {
        pixel = data16[s];
        img8.data[s] = LUT16TO8R[pixel];
        s++;
        pixel = data16[s];
        img8.data[s] = LUT16TO8G[pixel];
        s++;
        pixel = data16[s];
        img8.data[s] = LUT16TO8B[pixel];
        s++;
      }
    }

    // The following code will take about 400ms for 6000*4000 color image (under
    // release)
    /*

    std::vector<cv::Mat> channels16;
    cv::split(img16, channels16);

    std::vector<cv::Mat> channels8;
    cv::split(img8, channels8);


    uint16_t* data16;

    data16= (uint16_t*) channels16[0].data;

    s = 0;
    for (i = 0; i < imageY; i++)
    {
        for (j = 0; j < imageX; j++)
        {
            pixel = data16[s];
            channels8[0].data[s] = LUT16TO8R[pixel];
            s = s + 1;
        }
    }

    data16= (uint16_t*) channels16[1].data;

    s = 0;
    for (i = 0; i < imageY; i++)
    {
        for (j = 0; j < imageX; j++)
        {
            pixel = data16[s];
            channels8[1].data[s] = LUT16TO8G[pixel];
            s = s + 1;
        }
    }

    data16= (uint16_t*) channels16[2].data;

    s = 0;
    for (i = 0; i < imageY; i++)
    {
        for (j = 0; j < imageX; j++)
        {
            pixel = data16[s];
            channels8[2].data[s]= LUT16TO8B[pixel];
            s = s + 1;
        }
    }

    cv::merge(channels8, img8);

    */
  }
  #ifdef ImageDebug
  Logger::Log("Bit16To8_Stretch | used time(ms) " + std::to_string(t.elapsed()), LogLevel::INFO, DeviceType::MAIN);
  #endif
  // cvDebugShow(img16);
  // cvDebugShow(img8);
}

cv::Mat Tools::convert8UTo16U_BayerSafe(const cv::Mat& mat8u, bool scaleRange) {
    if (mat8u.empty()) {
        Logger::Log("convert8UTo16U_BayerSafe | input image is empty", LogLevel::ERROR, DeviceType::MAIN);
        return cv::Mat();
    }

    if (mat8u.type() == CV_16UC1) {
        Logger::Log("convert8UTo16U_BayerSafe | input image is already 16U", LogLevel::INFO, DeviceType::MAIN);
        return mat8u.clone();
    }
    // 1. 输入验证 + 深拷贝（避免外部修改）
    cv::Mat input = mat8u.clone(); // 深拷贝输入



    // 3. 确保矩阵连续
    if (!input.isContinuous()) {
        input = input.clone();
    }

    // 4. 创建并初始化输出矩阵
    cv::Mat mat16u(input.rows, input.cols, CV_16UC1, cv::Scalar(0));

    // 5. 处理数据
    if (scaleRange) {
        input.convertTo(mat16u, CV_16UC1, 256.0, 0);
    } else {
        // 手动补位（优化版）
        for (int y = 0; y < input.rows; y++) {
            const uint8_t* src = input.ptr<uint8_t>(y);
            uint16_t* dst = mat16u.ptr<uint16_t>(y);
            for (int x = 0; x < input.cols; x++) {
                dst[x] = static_cast<uint16_t>(src[x]);
            }
        }
    }

    return mat16u;
}


void Tools::CvDebugShow(cv::Mat img, const std::string& name) {
  cv::namedWindow(name, 0);
  cv::resizeWindow(name, 640, 480);
  // cv::imshow(name, img);
}

void Tools::CvDebugSave(cv::Mat img, const std::string& name) {
  QFileInfo fileInfo(QString::fromStdString(name));
  if (fileInfo.isRelative()) {
    std::string path = QDir(qApp->applicationDirPath())
                           .absoluteFilePath(QString::fromStdString(name))
                           .toUtf8()
                           .toStdString();
    cv::imwrite(path + ".tiff", img);
  } else {
    cv::imwrite(name + ".tiff", img);
  }
}

double Tools::getDecAngle(const QString &str)
{
    QRegExp rex("([-+]?)\\s*"                   // [sign] (1)
                "(?:"                           // either
                "(\\d+(?:\\.\\d+)?)\\s*"        // fract (2)
                "([dhms°º]?)"                   // [dhms] (3) \u00B0\u00BA
                "|"                             // or
                "(?:(\\d+)\\s*([hHdD°º])\\s*)?" // [int degs] (4) (5)
                "(?:"                           // either
                "(?:(\\d+)\\s*['mM]\\s*)?"      //  [int mins]  (6)
                "(\\d+(?:\\.\\d+)?)\\s*[\"sS]"  //  fract secs  (7)
                "|"                             // or
                "(\\d+(?:\\.\\d+)?)\\s*['mM]"   //  fract mins (8)
                ")"                             // end
                ")"                             // end
                "\\s*([NSEW]?)",                // [point] (9)
                Qt::CaseInsensitive);
    if (rex.exactMatch(str))
    {
        QStringList caps = rex.capturedTexts();
#if 0
        std::cout << "reg exp: ";
        for( int i = 1; i <= rex.captureCount() ; ++i ){
            std::cout << i << "=\"" << caps.at(i).toStdString() << "\" ";
        }
        std::cout << std::endl;
#endif
        double d = 0;
        double m = 0;
        double s = 0;
        ushort hd = caps.at(5).isEmpty() ? 'd' : caps.at(5).toLower().at(0).unicode();
        QString pointStr = caps.at(9).toUpper() + " ";
        if (caps.at(7) != "")
        {
            // [dh, degs], [m] and s entries at 4, 5, 6, 7
            d = caps.at(4).toDouble();
            m = caps.at(6).toDouble();
            s = caps.at(7).toDouble();
        }
        else if (caps.at(8) != "")
        {
            // [dh, degs] and m entries at 4, 5, 8
            d = caps.at(4).toDouble();
            m = caps.at(8).toDouble();
        }
        else if (caps.at(2) != "")
        {
            // some value at 2, dh|m|s at 3
            double x = caps.at(2).toDouble();
            QString sS = caps.at(3) + caps.at(9);
            switch (sS.length())
            {
            case 0:
                // degrees, no point
                hd = 'd';
                break;
            case 1:
                // NnSEeWw is point for degrees, "dhms°..." distinguish dhms
                if (QString("NnSEeWw").contains(sS.at(0)))
                {
                    pointStr = sS.toUpper();
                    hd = 'd';
                }
                else
                    hd = sS.toLower().at(0).unicode();
                break;
            case 2:
                // hdms selected by 1st char, NSEW by 2nd
                hd = sS.at(0).toLower().unicode();
                pointStr = sS.right(1).toUpper();
                break;
            }
            switch (hd)
            {
            case 'h':
            case 'd':
            case 0x00B0:
            case 0x00BA:
                d = x;
                break;
            case 'm':
            case '\'':
                m = x;
                break;
            case 's':
            case '"':
                s = x;
                break;
            default:
                Logger::Log("internal error, hd = " + std::to_string(hd), LogLevel::INFO, DeviceType::MAIN);
            }
        }
        else
        {
            Logger::Log("getDecAngle failed to parse angle string: " + str.toStdString(), LogLevel::INFO, DeviceType::MAIN);
            return -0.0;
        }

        // General sign handling: group 1 or overruled by point
        int sgn = caps.at(1) == "-" ? -1 : 1;
        bool isNS = false;
        switch (pointStr.at(0).unicode())
        {
        case 'N':
            sgn = 1;
            isNS = 1;
            break;
        case 'S':
            sgn = -1;
            isNS = 1;
            break;
        case 'E':
            sgn = 1;
            break;
        case 'W':
            sgn = -1;
            break;
        default: // OK, there is no NSEW.
            break;
        }

        int h2d = 1;
        if (hd == 'h')
        {
            // Sanity check - h and N/S not accepted together
            if (isNS)
            {
                Logger::Log("getDecAngle does not accept ...H...N/S: " + str.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                return -0.0;
            }
            h2d = 15;
        }
        double deg = (d + (m / 60) + (s / 3600)) * h2d * sgn;
        return deg * 2 * M_PI / 360.;
    }

    Logger::Log("getDecAngle failed to parse angle string: " + str.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    return -0.0;
}

cv::Mat Tools::CalMoments(cv::Mat image)
{
  Logger::Log("CalMoments:1", LogLevel::INFO, DeviceType::MAIN);
  cv::Mat grayImage;
  if(image.channels() == 1)
  {
    grayImage = image.clone();
  }
  else
  {
    cvtColor(image, grayImage, CV_RGB2GRAY);
  }
  Logger::Log("CalMoments:2", LogLevel::INFO, DeviceType::MAIN);
 

  return image;
}

cv::Mat Tools::SubBackGround(cv::Mat image)
{
    cv::Mat gray;
    if(image.channels() == 1)
    {
      gray = image.clone();
    }
    else
    {
      cvtColor(image, gray, CV_RGB2GRAY);
    }
    cv::Scalar scalar = mean(gray);
    double Background = scalar.val[0];

    Logger::Log("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("Backgroud brightness:" + std::to_string(Background), LogLevel::INFO, DeviceType::MAIN);

    cv::Mat m = cv::Mat(gray.size(), gray.type(), cv::Scalar(Background));
    cv::Mat dst = cv::Mat::zeros(gray.size(), gray.type());

    subtract(gray, m, dst);

    return dst;
}

// bool Tools::DetectStar(cv::Mat image, double threshold, int minArea, cv::Rect& starRect)
// {
//     cv::Mat subimage = Tools::SubBackGround(image).clone();
//     cv::Mat binaryImage;

//     // 二值化处理，阈值可以根据需要调整
//     cv::threshold(subimage, binaryImage, threshold, 255, cv::THRESH_BINARY);

//     // 确保二值图像是CV_8UC1格式
//     if (binaryImage.type() != CV_8UC1)
//     {
//         binaryImage.convertTo(binaryImage, CV_8UC1);
//     }

//     // 连通区域检测
//     std::vector<std::vector<cv::Point>> contours;
//     cv::findContours(binaryImage, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

//     // 找到面积最大的连通区域，认为是星点
//     double maxArea = 0;
//     for (const auto& contour : contours)
//     {
//         double area = cv::contourArea(contour);
//         if (area >= minArea && area > maxArea)
//         {
//             maxArea = area;
//             starRect = cv::boundingRect(contour);
//         }
//     }

//     if (maxArea > 0) {
//         // 检查检测到的星点亮度分布是否符合要求
//         cv::Mat starRegion = subimage(starRect);
//         double meanBrightness = cv::mean(starRegion)[0];
//         if (meanBrightness > threshold) {
//             return true;
//         }
//     }

//     return false;
// }

QList<FITSImage::Star> Tools::FindStarsByStellarSolver(bool AllStars, bool runHFR)
{
  Tools tempTool;

  loadFitsResult result;

  QList<FITSImage::Star> stars;

  result = loadFits("/dev/shm/ccd_simulator.fits");

  if (!result.success)
  {
    Logger::Log("Error in loading FITS file", LogLevel::INFO, DeviceType::MAIN);
    return stars;
  }

  FITSImage::Statistic imageStats = result.imageStats;
  uint8_t *imageBuffer = result.imageBuffer;
  stars = tempTool.FindStarsByStellarSolver_(AllStars, imageStats, imageBuffer, runHFR);
  return stars;
}

QList<FITSImage::Star> Tools::FindStarsByStellarSolver_(bool AllStars, const FITSImage::Statistic &imagestats, const uint8_t *imageBuffer, bool runHFR)
{
  StellarSolver solver(imagestats, imageBuffer);
  // 配置solver参数
  SSolver::Parameters parameters;

  // 设置参数
  // parameters.apertureShape = SSolver::SHAPE_CIRCLE; // 使用圆形的星点检测形状
  // parameters.kron_fact = 2.5;                       // 设置Kron因子
  // parameters.subpix = 5;                            // 子像素设置
  // parameters.r_min = 5;                             // 最小星点半径
  // parameters.magzero = 20;                          // 零点星等
  // parameters.minarea = 20;                          // 最小星点面积
  // parameters.deblend_thresh = 32;                   // 去混叠阈值
  // parameters.deblend_contrast = 0.005;              // 去混叠对比度
  // parameters.clean = 1;                             // 清理图像
  // parameters.fwhm = 1;                              // 全宽半高
  // parameters.maxSize = 0;                           // 最大星点大小
  // parameters.minSize = 0;                           // 最小星点大小
  // parameters.maxEllipse = 1.5;                      // 最大椭圆比
  // parameters.initialKeep = 250;                     // 初始保留星点数量
  // parameters.keepNum = 100;                         // 保留星点数量
  // parameters.removeBrightest = 10;                  // 移除最亮星点比例
  // parameters.removeDimmest = 20;                    // 移除最暗星点比例
  // parameters.saturationLimit = 90;                  // 饱和度限制
  
  parameters.apertureShape = SSolver::SHAPE_CIRCLE;        // 孔径形状：圆形
  parameters.autoDownsample = true;                        // 自动降采样：启用
  parameters.clean = 1;                                    // 清理参数：启用图像清理
  parameters.clean_param = 1;                              // 清理参数值：1
  parameters.convFilterType = SSolver::CONV_GAUSSIAN;     // 卷积滤波器类型：高斯滤波
  parameters.deblend_contrast = 0.004999999888241291;     // 去混叠对比度阈值
  parameters.deblend_thresh = 32;                          // 去混叠阈值
  parameters.description = "Default focus star-extraction."; // 参数描述：默认焦点星点提取
  parameters.downsample = 1;                               // 降采样因子：1（不降采样）
  parameters.fwhm = 1;                                     // 全宽半高：1像素
  parameters.inParallel = true;                            // 并行处理：启用
  parameters.initialKeep = 250;                            // 初始保留星点数量：250个
  parameters.keepNum = 100;                                // 最终保留星点数量：100个
  parameters.kron_fact = 2.5;                              // Kron因子：2.5
  parameters.listName = "1-Focus-Default";                 // 列表名称：1-焦点-默认
  parameters.logratio_tokeep = 20.72326583694641;         // 保留星点的对数比率阈值
  parameters.logratio_tosolve = 20.72326583694641;        // 求解星点的对数比率阈值
  parameters.logratio_totune = 13.815510557964274;        // 调优星点的对数比率阈值
  parameters.magzero = 20;                                 // 零点星等：20
  parameters.maxEllipse = 1.5;                             // 最大椭圆比：1.5
  parameters.maxSize = 10;                                 // 最大星点尺寸：10像素
  parameters.maxwidth = 180;                               // 最大宽度：180像素
  parameters.minSize = 0;                                  // 最小星点尺寸：0像素
  parameters.minarea = 20;                                 // 最小星点面积：20像素
  parameters.minwidth = 0.1;                               // 最小宽度：0.1像素
  parameters.multiAlgorithm = SSolver::MULTI_AUTO;        // 多算法模式：自动选择
  parameters.partition = true;                             // 分区处理：启用
  parameters.r_min = 5;                                    // 最小星点半径：5像素
  parameters.removeBrightest = 10;                         // 移除最亮星点比例：10%
  parameters.removeDimmest = 20;                           // 移除最暗星点比例：20%
  parameters.resort = true;                                // 重新排序：启用
  parameters.saturationLimit = 90;                         // 饱和度限制：90%
  parameters.search_parity = 15;                           // 搜索奇偶性：15
  parameters.solverTimeLimit = 600;                        // 求解器时间限制：600秒
  parameters.subpix = 5;                                   // 子像素精度：5


  solver.setLogLevel(SSolver::LOG_ALL);
  solver.setSSLogLevel(SSolver::LOG_NORMAL);

  solver.setProperty("ExtractorType", SSolver::EXTRACTOR_INTERNAL);
  solver.setProperty("ProcessType", SSolver::EXTRACT);
  solver.setParameterProfile(SSolver::Parameters::DEFAULT);
  // solver.setParameterProfile(SSolver::Parameters::ALL_STARS);

  // 设置参数
  solver.setParameters(parameters);

  if(AllStars) {
    solver.setParameterProfile(SSolver::Parameters::ALL_STARS);
  }

  connect(&solver, &StellarSolver::logOutput, this, &Tools::StellarSolverLogOutput);

  // 进行星点检测
  bool success = solver.extract(runHFR);
  if (!success)
  {
    Logger::Log("Star extraction failed.", LogLevel::INFO, DeviceType::MAIN);
  }
  Logger::Log("success extract: " + std::to_string(success), LogLevel::INFO, DeviceType::MAIN);

  QList<FITSImage::Star> stars;

  try {
    stars = solver.getStarList();
    Logger::Log("Successfully got star list with " + std::to_string(stars.size()) + " stars.", LogLevel::INFO, DeviceType::MAIN);
  } catch (const std::exception &e) {
    Logger::Log("Exception getting star list: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
    return QList<FITSImage::Star>();
  } catch (...) {
    Logger::Log("Unknown exception getting star list", LogLevel::ERROR, DeviceType::MAIN);
    return QList<FITSImage::Star>();
  }

  // 输出检测到的星点信息，但避免访问可能损坏的内存
  Logger::Log("Detected " + std::to_string(stars.size()) + " stars.", LogLevel::INFO, DeviceType::MAIN);
  
  // 安全地遍历前几个星点进行调试
  int debugCount = 0;
  if (stars.size() > 0) {
    for (const auto &star : stars)
    {
      if (debugCount >= 3) break; // 只调试前3个星点
      
      try {
        Logger::Log("Star " + std::to_string(debugCount) + " at (" + 
                    std::to_string(star.x) + ", " + std::to_string(star.y) + 
                    ") with HFR: " + std::to_string(star.HFR), LogLevel::INFO, DeviceType::MAIN);
      } catch (const std::exception &e) {
        Logger::Log("Exception accessing star " + std::to_string(debugCount) + ": " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
        break;
      } catch (...) {
        Logger::Log("Unknown exception accessing star " + std::to_string(debugCount), LogLevel::ERROR, DeviceType::MAIN);
        break;
      }
      debugCount++;
    }
  } else {
    Logger::Log("No stars detected, skipping star info output", LogLevel::INFO, DeviceType::MAIN);
  }

  return stars;
}

void Tools::StellarSolverLogOutput(QString text){
  // Logger::Log("StellarSolver LogOutput: " + text.toStdString(), LogLevel::INFO, DeviceType::MAIN);
}

loadFitsResult Tools::loadFits(QString fileName)
{
  loadFitsResult result;
  QString file = fileName;
  fitsfile *fptr{nullptr};
  FITSImage::Statistic stats;
  /// Generic data image buffer
  uint8_t *m_ImageBuffer{nullptr};
  /// Above buffer size in bytes
  uint32_t m_ImageBufferSize{0};
  int status = 0, anynullptr = 0;
  long naxes[3];

  // Use open diskfile as it does not use extended file names which has problems opening
  // files with [ ] or ( ) in their names.
  if (fits_open_diskfile(&fptr, file.toLocal8Bit(), READONLY, &status))
  {
    // logIssue(QString("Error opening fits file %1").arg(file));
    Logger::Log("Error opening fits file " + file.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
    result.success = false;
    return result;
  }
  else
    stats.size = QFile(file).size();

  if (fits_movabs_hdu(fptr, 1, IMAGE_HDU, &status))
  {
    // logIssue(QString("Could not locate image HDU."));
    Logger::Log("Could not locate image HDU.", LogLevel::INFO, DeviceType::MAIN);
    fits_close_file(fptr, &status);
    result.success = false;
    return result;
  }

  int fitsBitPix = 0;
  if (fits_get_img_param(fptr, 3, &fitsBitPix, &(stats.ndim), naxes, &status))
  {
    // logIssue(QString("FITS file open error (fits_get_img_param)."));
    Logger::Log("FITS file open error (fits_get_img_param).", LogLevel::WARNING, DeviceType::MAIN);
    fits_close_file(fptr, &status);
    result.success = false;
    return result;
  }

  if (stats.ndim < 2)
  {
    // logIssue("1D FITS images are not supported.");
    Logger::Log("1D FITS images are not supported.", LogLevel::WARNING, DeviceType::MAIN);
    fits_close_file(fptr, &status);
    result.success = false;
    return result;
  }

  switch (fitsBitPix)
  {
  case BYTE_IMG:
    // stats.dataType      = SEP_TBYTE;
    stats.dataType = 11;
    stats.bytesPerPixel = sizeof(uint8_t);
    break;
  case SHORT_IMG:
    // Read SHORT image as USHORT
    stats.dataType = TUSHORT;
    stats.bytesPerPixel = sizeof(int16_t);
    break;
  case USHORT_IMG:
    stats.dataType = TUSHORT;
    stats.bytesPerPixel = sizeof(uint16_t);
    break;
  case LONG_IMG:
    // Read LONG image as ULONG
    stats.dataType = TULONG;
    stats.bytesPerPixel = sizeof(int32_t);
    break;
  case ULONG_IMG:
    stats.dataType = TULONG;
    stats.bytesPerPixel = sizeof(uint32_t);
    break;
  case FLOAT_IMG:
    stats.dataType = TFLOAT;
    stats.bytesPerPixel = sizeof(float);
    break;
  case LONGLONG_IMG:
    stats.dataType = TLONGLONG;
    stats.bytesPerPixel = sizeof(int64_t);
    break;
  case DOUBLE_IMG:
    stats.dataType = TDOUBLE;
    stats.bytesPerPixel = sizeof(double);
    break;
  default:
    // logIssue(QString("Bit depth %1 is not supported.").arg(fitsBitPix));
    Logger::Log("Bit depth %1 is not supported." + std::to_string(fitsBitPix), LogLevel::WARNING, DeviceType::MAIN);

    fits_close_file(fptr, &status);
    result.success = false;
    return result;
  }

  if (stats.ndim < 3)
    naxes[2] = 1;

  if (naxes[0] == 0 || naxes[1] == 0)
  {
    // logIssue(QString("Image has invalid dimensions %1x%2").arg(naxes[0]).arg(naxes[1]));
    Logger::Log("Image has invalid dimensions." + std::to_string(naxes[0]) + " " + std::to_string(naxes[1]), LogLevel::WARNING, DeviceType::MAIN);
  }

  stats.width = static_cast<uint16_t>(naxes[0]);
  stats.height = static_cast<uint16_t>(naxes[1]);
  stats.channels = static_cast<uint8_t>(naxes[2]);
  stats.samples_per_channel = stats.width * stats.height;

  m_ImageBufferSize = stats.samples_per_channel * stats.channels * static_cast<uint16_t>(stats.bytesPerPixel);
  // deleteImageBuffer();
  if (m_ImageBuffer)
  {
    delete[] m_ImageBuffer;
    m_ImageBuffer = nullptr;
  }

  m_ImageBuffer = new uint8_t[m_ImageBufferSize];
  if (m_ImageBuffer == nullptr)
  {
    // logIssue(QString("FITSData: Not enough memory for image_buffer channel. Requested: %1 bytes ").arg(m_ImageBufferSize));
    Logger::Log("FITSData: Not enough memory for image_buffer channel. Requested:" + std::to_string(m_ImageBufferSize) + "bytes ", LogLevel::WARNING, DeviceType::MAIN);
    fits_close_file(fptr, &status);
    result.success = false;
    return result;
  }

  long nelements = stats.samples_per_channel * stats.channels;

  if (fits_read_img(fptr, static_cast<uint16_t>(stats.dataType), 1, nelements, nullptr, m_ImageBuffer, &anynullptr, &status))
  {
    // logIssue("Error reading image.");
    Logger::Log("Error reading image.", LogLevel::WARNING, DeviceType::MAIN);
    fits_close_file(fptr, &status);
    result.success = false;
    return result;
  }

  fits_close_file(fptr, &status);

  result.success = true;
  result.imageStats = stats;
  result.imageBuffer = m_ImageBuffer;

  return result;
}

FWHM_Result Tools::CalculateFWHM(cv::Mat image)
{
  FWHM_Result result;
    cv::Mat subimage;
    subimage = Tools::SubBackGround(image).clone();
    int FirstMoment_x, FirstMoment_y;

    double scale_up = 10.0;
    resize(subimage,subimage,cv::Size(),scale_up,scale_up,cv::INTER_LINEAR);

//---------------------------寻找重心点---------------------------------
    cv::Mat imageFloat;
    subimage.convertTo(imageFloat, CV_64F);

    // Calculate the sum of pixel values
    double sum = cv::sum(imageFloat)[0];

    // Calculate the coordinates of the first-order moment (mean)
    double xCoordinate = 0.0;
    double yCoordinate = 0.0;

    for (int y = 0; y < subimage.rows; ++y) {
        for (int x = 0; x < subimage.cols; ++x) {
            double pixelValue = imageFloat.at<double>(y, x);
            xCoordinate += x * pixelValue;
            yCoordinate += y * pixelValue;
        }
    }

    xCoordinate /= sum;
    yCoordinate /= sum;

    FirstMoment_x = xCoordinate;
    FirstMoment_y = yCoordinate;
//------------------------------------------------------------

    Logger::Log("Barycentric Coordinate:" + std::to_string(FirstMoment_x) + "," + std::to_string(FirstMoment_y), LogLevel::INFO, DeviceType::MAIN);


    int height = subimage.rows;
    int width = subimage.cols;

    ushort Bri1 = subimage.at<ushort>(FirstMoment_y, FirstMoment_x);
    Logger::Log("Max:" + std::to_string(Bri1), LogLevel::INFO, DeviceType::MAIN);

    int x_s, x_b;
    // for (int i = FirstMoment_x; i > 0; i--)
    for (int i = 0; i < FirstMoment_x; i++)
    {
        ushort Bri_Left = subimage.at<ushort>(FirstMoment_y, i);
        if (Bri_Left >= Bri1 / 2)
        {
            x_s = i;
            break;
        }
    }

    // for (int j = FirstMoment_x; j < width; j++)
    for (int j = width; j > FirstMoment_x; j--)
    {
        ushort Bri_Right = subimage.at<ushort>(FirstMoment_y, j);
        if (Bri_Right >= Bri1 / 2)
        {
            x_b = j;
            break;
        }
    }

    // double FWHM = (x_b - x_s)/10.0;
    double FWHM = static_cast<double>(x_b-x_s)/10.0;
    Logger::Log("x_b:" + std::to_string(x_b) + " x_s:" + std::to_string(x_s), LogLevel::INFO, DeviceType::MAIN);

    Logger::Log("[[[[FWHM:" + std::to_string(FWHM) + "]]]]]", LogLevel::INFO, DeviceType::MAIN);
    // qDebug("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");

    cv::Mat imagePoint = image.clone();

    cv::Point PointA(FirstMoment_x/10, FirstMoment_y/10);
    cv::Point PointB(FirstMoment_x/10, FirstMoment_y/10);
    cv::line(imagePoint, PointA, PointB, cv::Scalar(0, 0, 0), 3, 8, 0);
    // imshow("Point",imagePoint);

    // return imagePoint;

    result.image = imagePoint;
    result.FWHM = FWHM;

    return result;
}

HFR_Result Tools::CalculateHFR(cv::Mat image)
{
    HFR_Result result;
    cv::Rect starRect;
    // if (!Tools::DetectStar(image, 50.0, 5, starRect)) {
    //     qDebug() << "No star detected in the image.";
    //     result.image = image;
    //     result.HFR = -1.0;
    //     return result;
    // }

    // cv::Mat starRegion = image(starRect);
    cv::Mat subimage = Tools::SubBackGround(image).clone();
    int FirstMoment_x, FirstMoment_y;

    double scale_up = 10.0;
    cv::resize(subimage, subimage, cv::Size(), scale_up, scale_up, cv::INTER_LINEAR);

    cv::Mat imageFloat;
    subimage.convertTo(imageFloat, CV_64F);

    double sum = cv::sum(imageFloat)[0];
    double xCoordinate = 0.0;
    double yCoordinate = 0.0;

    for (int y = 0; y < subimage.rows; ++y) {
        for (int x = 0; x < subimage.cols; ++x) {
            double pixelValue = imageFloat.at<double>(y, x);
            xCoordinate += x * pixelValue;
            yCoordinate += y * pixelValue;
        }
    }

    xCoordinate /= sum;
    yCoordinate /= sum;

    FirstMoment_x = static_cast<int>(xCoordinate);
    FirstMoment_y = static_cast<int>(yCoordinate);

    double maxBrightness = subimage.at<ushort>(FirstMoment_y, FirstMoment_x);
    double halfMaxBrightness = maxBrightness / 2.0;

    std::vector<double> distances;

    for (int y = 0; y < subimage.rows; ++y) {
        for (int x = 0; x < subimage.cols; ++x) {
            double pixelValue = subimage.at<ushort>(y, x);
            if (pixelValue >= halfMaxBrightness) {
                double distance = std::sqrt(std::pow(x - FirstMoment_x, 2) + std::pow(y - FirstMoment_y, 2));
                distances.push_back(distance);
            }
        }
    }

    double sumDistances = std::accumulate(distances.begin(), distances.end(), 0.0);
    double HFR = sumDistances / distances.size() / 10.0;

    // 在原图上绘制检测结果
    cv::Mat imagePoint = image.clone();
    cv::Point center(starRect.x + FirstMoment_x / 10, starRect.y + FirstMoment_y / 10);
    cv::rectangle(imagePoint, starRect, cv::Scalar(0, 255, 0), 1); // 绘制外接矩形
    cv::circle(imagePoint, center, static_cast<int>(HFR), cv::Scalar(0, 0, 255), 1); // 绘制HFR圆
    cv::circle(imagePoint, center, 1, cv::Scalar(0, 255, 0), -1); // 绘制中心点

    // 在图像上显示HFR数值
    std::string hfrText = cv::format("%.2f", HFR);
    cv::putText(imagePoint, hfrText, cv::Point(starRect.x, starRect.y - 5), cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(0, 0, 255), 1);

    result.image = imagePoint;
    result.HFR = HFR;

    return result;
}

CamBin Tools::mergeImageBasedOnSize(cv::Mat image) {
    // 获取图像尺寸
    int width = image.cols;
    int height = image.rows;
    
    // 计算合适的 bin 值，使合并后的图像不超过 2000
    CamBin CamBin;

    // 根据宽度和高度，选择适合的 bin 值
    if (width > 2000 || height > 2000) {
        if (width / 2 <= 2000 && height / 2 <= 2000) {
            CamBin.camxbin = 2;
            CamBin.camybin = 2;
        } else if (width / 3 <= 2000 && height / 3 <= 2000) {
            CamBin.camxbin = 3;
            CamBin.camybin = 3;
        } else if (width / 4 <= 2000 && height / 4 <= 2000) {
            CamBin.camxbin = 4;
            CamBin.camybin = 4;
        }
    }
    
    return CamBin;
}

// cv::Mat Tools::processMatWithBinAvg(cv::Mat &image, uint32_t camxbin, uint32_t camybin, bool isColor, bool isAVG)
// {
//     uint32_t width = image.cols;
//     uint32_t height = image.rows;
//     uint32_t depth = image.elemSize() * 8;
//     uint32_t camchannels = image.channels();

//     uint8_t *srcdata = image.data;
//     image.release();

//     uint32_t outputSize;
//     if (depth == 8) {
//         outputSize = (width / camxbin) * (height / camybin);
//     }
//     else if (depth == 16) {
//         outputSize = 2 * (width / camxbin) * (height / camybin);
//     }
//     else if (depth == 32) {
//         outputSize = 4 * (width / camxbin) * (height / camybin);
//     }
//     else {
//         Logger::Log("Unsupported depth!", LogLevel::ERROR, DeviceType::MAIN);
//         return cv::Mat(); // 返回空Mat
//     }

//     // 分配输出数据的内存
//     std::vector<uint8_t> bindata(outputSize, 0);

//     uint32_t result;
//     if(isAVG) {
//         result = PixelsDataSoftBin_AVG(srcdata, bindata.data(), width, height, depth, camxbin, camybin);
//     } else {
//         result = PixelsDataSoftBin(srcdata, bindata.data(), width, height, camchannels, depth, camxbin, camybin, isColor);
//     }

//     if (result == QHYCCD_SUCCESS) {
//         int newWidth = width / camxbin;
//         int newHeight = height / camybin;
        
//         // 创建新的Mat并复制数据
//         cv::Mat outputImage;
//         if (depth == 8) {
//             outputImage = cv::Mat(newHeight, newWidth, CV_8U);
//             memcpy(outputImage.data, bindata.data(), outputSize);
//         }
//         else if (depth == 16) {
//             outputImage = cv::Mat(newHeight, newWidth, CV_16U);
//             memcpy(outputImage.data, bindata.data(), outputSize);
//         }
//         else if (depth == 32) {
//             outputImage = cv::Mat(newHeight, newWidth, CV_32S);
//             memcpy(outputImage.data, bindata.data(), outputSize);
//         }
        
//         // 再次检查处理后的图像尺寸，确保为偶数
//         int finalWidth = outputImage.cols;
//         int finalHeight = outputImage.rows;
        
//         // 如果图像宽高不是偶数，裁剪一行或一列
//         if (finalWidth % 2 != 0 || finalHeight % 2 != 0) {
//             int cropWidth = finalWidth - (finalWidth % 2);
//             int cropHeight = finalHeight - (finalHeight % 2);
            
//             if (cropWidth > 0 && cropHeight > 0) {
//                 // 使用ROI裁剪图像
//                 cv::Rect roi(0, 0, cropWidth, cropHeight);
//                 cv::Mat croppedImage = outputImage(roi).clone();
//                 outputImage.release(); // 释放原始的输出图像内存
                
//                 return croppedImage;
//             }
//         }
        
//         return outputImage;
//     }
    
//     return cv::Mat(); // 错误时返回空Mat
// }

cv::Mat Tools::processMatWithBinAvg(cv::Mat &image, uint32_t camxbin, uint32_t camybin, bool isColor, bool isAVG)
{
    // 输入参数验证
    if (image.empty()) {
        Logger::Log("输入图像为空", LogLevel::ERROR, DeviceType::MAIN);
        return cv::Mat();
    }
    
    if (camxbin == 0 || camybin == 0) {
        Logger::Log("binning参数无效: x=" + std::to_string(camxbin) + " y=" + std::to_string(camybin), 
                   LogLevel::ERROR, DeviceType::MAIN);
        return cv::Mat();
    }
    
    uint32_t width = image.cols;
    uint32_t height = image.rows;
    
    // 检查图像尺寸是否足够binning
    if (width < camxbin || height < camybin) {
        Logger::Log("图像尺寸小于binning大小", LogLevel::ERROR, DeviceType::MAIN);
        return cv::Mat();
    }
    
    // 计算新尺寸
    int newWidth = width / camxbin;
    int newHeight = height / camybin;
    
    // 确保有效尺寸
    if (newWidth <= 0 || newHeight <= 0) {
        Logger::Log("binning后图像尺寸无效", LogLevel::ERROR, DeviceType::MAIN);
        return cv::Mat();
    }
    
    cv::Mat outputImage;
    
    try {
        if (isAVG) {
            // 使用OpenCV resize实现平均值binning
            cv::resize(image, outputImage, cv::Size(newWidth, newHeight), 0, 0, cv::INTER_AREA);
        } else {
            // 根据图像类型选择处理方法
            int depth = image.depth();
            int type = image.type();
            
            if (isColor && image.channels() == 3) {
                // 彩色图像处理
                cv::resize(image, outputImage, cv::Size(newWidth, newHeight), 0, 0, cv::INTER_AREA);
            } else {
                // 单色图像处理
                // 创建目标图像
                outputImage = cv::Mat(newHeight, newWidth, type);
                outputImage.setTo(0); // 初始化为0
                
                // 选择适当的处理方法
                if (depth == CV_8U) {
                    // 8位图像binning
                    for (int y = 0; y < newHeight; y++) {
                        for (int x = 0; x < newWidth; x++) {
                            int sum = 0;
                            for (int by = 0; by < camybin; by++) {
                                for (int bx = 0; bx < camxbin; bx++) {
                                    int srcY = y * camybin + by;
                                    int srcX = x * camxbin + bx;
                                    if (srcY < height && srcX < width) {
                                        sum += image.at<uint8_t>(srcY, srcX);
                                    }
                                }
                            }
                            outputImage.at<uint8_t>(y, x) = (uint8_t)std::min(255, sum);
                        }
                    }
                } else if (depth == CV_16U) {
                    // 16位图像binning
                    for (int y = 0; y < newHeight; y++) {
                        for (int x = 0; x < newWidth; x++) {
                            int sum = 0;
                            for (int by = 0; by < camybin; by++) {
                                for (int bx = 0; bx < camxbin; bx++) {
                                    int srcY = y * camybin + by;
                                    int srcX = x * camxbin + bx;
                                    if (srcY < height && srcX < width) {
                                        sum += image.at<uint16_t>(srcY, srcX);
                                    }
                                }
                            }
                            outputImage.at<uint16_t>(y, x) = (uint16_t)std::min(65535, sum);
                        }
                    }
                } else if (depth == CV_32S) {
                    // 32位图像binning
                    for (int y = 0; y < newHeight; y++) {
                        for (int x = 0; x < newWidth; x++) {
                            int64_t sum = 0;
                            for (int by = 0; by < camybin; by++) {
                                for (int bx = 0; bx < camxbin; bx++) {
                                    int srcY = y * camybin + by;
                                    int srcX = x * camxbin + bx;
                                    if (srcY < height && srcX < width) {
                                        sum += image.at<int32_t>(srcY, srcX);
                                    }
                                }
                            }
                            outputImage.at<int32_t>(y, x) = (int32_t)sum;
                        }
                    }
                }
            }
        }
    } catch (const cv::Exception& e) {
        Logger::Log("OpenCV错误: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
        return cv::Mat();
    } catch (const std::exception& e) {
        Logger::Log("处理错误: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
        return cv::Mat();
    }
    
    // 确保宽高都是偶数
    int finalWidth = outputImage.cols;
    int finalHeight = outputImage.rows;
    
    if (finalWidth % 2 != 0 || finalHeight % 2 != 0) {
        int cropWidth = finalWidth - (finalWidth % 2);
        int cropHeight = finalHeight - (finalHeight % 2);
        
        if (cropWidth > 0 && cropHeight > 0) {
            cv::Rect roi(0, 0, cropWidth, cropHeight);
            return outputImage(roi).clone();
        }
    }
    
    return outputImage;
}

// 修改后的 Bayer 阵列处理函数，支持不同的 Bayer 模式
cv::Mat Tools::PixelsDataSoftBin_Bayer(cv::Mat srcMat, uint32_t camxbin, uint32_t camybin, BayerPattern bayerPattern)
{
  // 打印输入参数
  Logger::Log("PixelsDataSoftBin_Bayer | 输入图像: " + std::to_string(srcMat.cols) + "x" + std::to_string(srcMat.rows) + 
              " 类型:" + std::to_string(srcMat.type()) + " bin:" + std::to_string(camxbin) + "x" + std::to_string(camybin),
              LogLevel::INFO, DeviceType::MAIN);

  if (srcMat.empty()) {
    Logger::Log("输入图像为空", LogLevel::ERROR, DeviceType::MAIN);
    return cv::Mat();
  }
  
  uint32_t width = srcMat.cols;
  uint32_t height = srcMat.rows;
  uint32_t depth = 0;
  
  // 根据Mat的类型确定位深度
  if (srcMat.type() == CV_8U) {
    depth = 8;
    Logger::Log("PixelsDataSoftBin_Bayer | 处理8位图像", LogLevel::INFO, DeviceType::MAIN);
  } else if (srcMat.type() == CV_16U) {
    depth = 16;
    Logger::Log("PixelsDataSoftBin_Bayer | 处理16位图像", LogLevel::INFO, DeviceType::MAIN);
  } else if (srcMat.type() == CV_32S) {
    depth = 32;
    Logger::Log("PixelsDataSoftBin_Bayer | 处理32位图像", LogLevel::INFO, DeviceType::MAIN);
  } else {
    Logger::Log("不支持的图像类型: " + std::to_string(srcMat.type()), LogLevel::ERROR, DeviceType::MAIN);
    return cv::Mat();
  }
  
  uint32_t newWidth = width / camxbin;
  uint32_t newHeight = height / camybin;
  
  // 检查新图像尺寸是否为0
  if (newWidth == 0 || newHeight == 0) {
    Logger::Log("PixelsDataSoftBin_Bayer | 错误: 缩放后尺寸为0! " + std::to_string(newWidth) + "x" + std::to_string(newHeight), 
                LogLevel::ERROR, DeviceType::MAIN);
    return cv::Mat();
  }
  
  Logger::Log("PixelsDataSoftBin_Bayer | 新图像尺寸: " + std::to_string(newWidth) + "x" + std::to_string(newHeight), 
              LogLevel::INFO, DeviceType::MAIN);
  
  // 创建输出图像
  cv::Mat binMat;
  try {
    if (depth == 8) {
      binMat = cv::Mat::zeros(newHeight, newWidth, CV_8U);
    } else if (depth == 16) {
      binMat = cv::Mat::zeros(newHeight, newWidth, CV_16U);
    } else if (depth == 32) {
      binMat = cv::Mat::zeros(newHeight, newWidth, CV_32S);
    }
    Logger::Log("PixelsDataSoftBin_Bayer | 输出图像创建成功", LogLevel::INFO, DeviceType::MAIN);
  } catch (cv::Exception &e) {
    Logger::Log("PixelsDataSoftBin_Bayer | 创建输出图像异常: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
    return cv::Mat();
  }
  
  // 根据 Bayer 模式确定各通道在 2x2 块中的位置
  int rOffsetY, rOffsetX, g1OffsetY, g1OffsetX, g2OffsetY, g2OffsetX, bOffsetY, bOffsetX;
  
  switch (bayerPattern) {
    case BAYER_RGGB:  // RGGB: R(0,0), G1(0,1), G2(1,0), B(1,1)
      rOffsetY = 0; rOffsetX = 0;
      g1OffsetY = 0; g1OffsetX = 1;
      g2OffsetY = 1; g2OffsetX = 0;
      bOffsetY = 1; bOffsetX = 1;
      Logger::Log("PixelsDataSoftBin_Bayer | 使用RGGB模式", LogLevel::INFO, DeviceType::MAIN);
      break;
    case BAYER_BGGR:  // BGGR: B(0,0), G1(0,1), G2(1,0), R(1,1)
      bOffsetY = 0; bOffsetX = 0;
      g1OffsetY = 0; g1OffsetX = 1;
      g2OffsetY = 1; g2OffsetX = 0;
      rOffsetY = 1; rOffsetX = 1;
      Logger::Log("PixelsDataSoftBin_Bayer | 使用BGGR模式", LogLevel::INFO, DeviceType::MAIN);
      break;
    case BAYER_GRBG:  // GRBG: G1(0,0), R(0,1), B(1,0), G2(1,1)
      g1OffsetY = 0; g1OffsetX = 0;
      rOffsetY = 0; rOffsetX = 1;
      bOffsetY = 1; bOffsetX = 0;
      g2OffsetY = 1; g2OffsetX = 1;
      Logger::Log("PixelsDataSoftBin_Bayer | 使用GRBG模式", LogLevel::INFO, DeviceType::MAIN);
      break;
    case BAYER_GBRG:  // GBRG: G1(0,0), B(0,1), R(1,0), G2(1,1)
      g1OffsetY = 0; g1OffsetX = 0;
      bOffsetY = 0; bOffsetX = 1;
      rOffsetY = 1; rOffsetX = 0;
      g2OffsetY = 1; g2OffsetX = 1;
      Logger::Log("PixelsDataSoftBin_Bayer | 使用GBRG模式", LogLevel::INFO, DeviceType::MAIN);
      break;
    default:
      Logger::Log("不支持的 Bayer 模式", LogLevel::ERROR, DeviceType::MAIN);
      binMat.release();  // 释放已分配的内存
      return cv::Mat();
  }
  try {
    // 根据位深度选择不同的处理逻辑
    if (depth == 8)
    {
      Logger::Log("PixelsDataSoftBin_Bayer | 处理8位图像数据", LogLevel::INFO, DeviceType::MAIN);
      // 处理 8 位 Bayer 阵列
      for (uint32_t by = 0; by < height; by += camybin * 2)
      {
        for (uint32_t bx = 0; bx < width; bx += camxbin * 2)
        {
          // 分别处理四个通道
          uint32_t sumR = 0, sumG1 = 0, sumG2 = 0, sumB = 0;
          uint32_t countR = 0, countG1 = 0, countG2 = 0, countB = 0;
          
          // 累加每个 Bayer 块中的像素值
          for (uint32_t y = 0; y < camybin * 2 && by + y < height; y += 2)
          {
            for (uint32_t x = 0; x < camxbin * 2 && bx + x < width; x += 2)
            {
              // R 位置
              if (by + y + rOffsetY < height && bx + x + rOffsetX < width) {
                sumR += srcMat.at<uint8_t>(by + y + rOffsetY, bx + x + rOffsetX);
                countR++;
              }
              
              // G1 位置
              if (by + y + g1OffsetY < height && bx + x + g1OffsetX < width) {
                sumG1 += srcMat.at<uint8_t>(by + y + g1OffsetY, bx + x + g1OffsetX);
                countG1++;
              }
              
              // G2 位置
              if (by + y + g2OffsetY < height && bx + x + g2OffsetX < width) {
                sumG2 += srcMat.at<uint8_t>(by + y + g2OffsetY, bx + x + g2OffsetX);
                countG2++;
              }
              
              // B 位置
              if (by + y + bOffsetY < height && bx + x + bOffsetX < width) {
                sumB += srcMat.at<uint8_t>(by + y + bOffsetY, bx + x + bOffsetX);
                countB++;
              }
            }
          }
          
          // 计算平均值并写入输出缓冲区，保持相同的 Bayer 模式
          uint32_t newY = by / camybin / 2;
          uint32_t newX = bx / camxbin / 2;
          
          // 添加边界检查，确保不会越界访问
          if (newY * 2 + rOffsetY < newHeight && newX * 2 + rOffsetX < newWidth && countR > 0)
            binMat.at<uint8_t>(newY * 2 + rOffsetY, newX * 2 + rOffsetX) = sumR / countR;
          
          if (newY * 2 + g1OffsetY < newHeight && newX * 2 + g1OffsetX < newWidth && countG1 > 0)
            binMat.at<uint8_t>(newY * 2 + g1OffsetY, newX * 2 + g1OffsetX) = sumG1 / countG1;
          
          if (newY * 2 + g2OffsetY < newHeight && newX * 2 + g2OffsetX < newWidth && countG2 > 0)
            binMat.at<uint8_t>(newY * 2 + g2OffsetY, newX * 2 + g2OffsetX) = sumG2 / countG2;
          
          if (newY * 2 + bOffsetY < newHeight && newX * 2 + bOffsetX < newWidth && countB > 0)
            binMat.at<uint8_t>(newY * 2 + bOffsetY, newX * 2 + bOffsetX) = sumB / countB;
        }
      }
    }
    else if (depth == 16)
    {
      Logger::Log("PixelsDataSoftBin_Bayer | 处理16位图像数据", LogLevel::INFO, DeviceType::MAIN);
      // 处理 16 位 Bayer 阵列
      for (uint32_t by = 0; by < height; by += camybin * 2)
      {
        for (uint32_t bx = 0; bx < width; bx += camxbin * 2)
        {
          uint32_t sumR = 0, sumG1 = 0, sumG2 = 0, sumB = 0;
          uint32_t countR = 0, countG1 = 0, countG2 = 0, countB = 0;
          
          for (uint32_t y = 0; y < camybin * 2 && by + y < height; y += 2)
          {
            for (uint32_t x = 0; x < camxbin * 2 && bx + x < width; x += 2)
            {
              if (by + y + rOffsetY < height && bx + x + rOffsetX < width) {
                sumR += srcMat.at<uint16_t>(by + y + rOffsetY, bx + x + rOffsetX);
                countR++;
              }
              
              if (by + y + g1OffsetY < height && bx + x + g1OffsetX < width) {
                sumG1 += srcMat.at<uint16_t>(by + y + g1OffsetY, bx + x + g1OffsetX);
                countG1++;
              }
              
              if (by + y + g2OffsetY < height && bx + x + g2OffsetX < width) {
                sumG2 += srcMat.at<uint16_t>(by + y + g2OffsetY, bx + x + g2OffsetX);
                countG2++;
              }
              
              if (by + y + bOffsetY < height && bx + x + bOffsetX < width) {
                sumB += srcMat.at<uint16_t>(by + y + bOffsetY, bx + x + bOffsetX);
                countB++;
              }
            }
          }
          
          uint32_t newY = by / camybin / 2;
          uint32_t newX = bx / camxbin / 2;
          
          // 添加边界检查，确保不会越界访问
          if (newY * 2 + rOffsetY < newHeight && newX * 2 + rOffsetX < newWidth && countR > 0)
            binMat.at<uint16_t>(newY * 2 + rOffsetY, newX * 2 + rOffsetX) = sumR / countR;
          
          if (newY * 2 + g1OffsetY < newHeight && newX * 2 + g1OffsetX < newWidth && countG1 > 0)
            binMat.at<uint16_t>(newY * 2 + g1OffsetY, newX * 2 + g1OffsetX) = sumG1 / countG1;
          
          if (newY * 2 + g2OffsetY < newHeight && newX * 2 + g2OffsetX < newWidth && countG2 > 0)
            binMat.at<uint16_t>(newY * 2 + g2OffsetY, newX * 2 + g2OffsetX) = sumG2 / countG2;
          
          if (newY * 2 + bOffsetY < newHeight && newX * 2 + bOffsetX < newWidth && countB > 0)
            binMat.at<uint16_t>(newY * 2 + bOffsetY, newX * 2 + bOffsetX) = sumB / countB;
        }
      }
    }
    else if (depth == 32)
    {
      Logger::Log("PixelsDataSoftBin_Bayer | 处理32位图像数据", LogLevel::INFO, DeviceType::MAIN);
      // 处理 32 位 Bayer 阵列
      for (uint32_t by = 0; by < height; by += camybin * 2)
      {
        for (uint32_t bx = 0; bx < width; bx += camxbin * 2)
        {
          uint64_t sumR = 0, sumG1 = 0, sumG2 = 0, sumB = 0;
          uint32_t countR = 0, countG1 = 0, countG2 = 0, countB = 0;
          
          for (uint32_t y = 0; y < camybin * 2 && by + y < height; y += 2)
          {
            for (uint32_t x = 0; x < camxbin * 2 && bx + x < width; x += 2)
            {
              if (by + y + rOffsetY < height && bx + x + rOffsetX < width) {
                sumR += srcMat.at<int32_t>(by + y + rOffsetY, bx + x + rOffsetX);
                countR++;
              }
              
              if (by + y + g1OffsetY < height && bx + x + g1OffsetX < width) {
                sumG1 += srcMat.at<int32_t>(by + y + g1OffsetY, bx + x + g1OffsetX);
                countG1++;
              }
              
              if (by + y + g2OffsetY < height && bx + x + g2OffsetX < width) {
                sumG2 += srcMat.at<int32_t>(by + y + g2OffsetY, bx + x + g2OffsetX);
                countG2++;
              }
              
              if (by + y + bOffsetY < height && bx + x + bOffsetX < width) {
                sumB += srcMat.at<int32_t>(by + y + bOffsetY, bx + x + bOffsetX);
                countB++;
              }
            }
          }
          
          uint32_t newY = by / camybin / 2;
          uint32_t newX = bx / camxbin / 2;
          
          // 添加边界检查，确保不会越界访问
          if (newY * 2 + rOffsetY < newHeight && newX * 2 + rOffsetX < newWidth && countR > 0)
            binMat.at<int32_t>(newY * 2 + rOffsetY, newX * 2 + rOffsetX) = sumR / countR;
          
          if (newY * 2 + g1OffsetY < newHeight && newX * 2 + g1OffsetX < newWidth && countG1 > 0)
            binMat.at<int32_t>(newY * 2 + g1OffsetY, newX * 2 + g1OffsetX) = sumG1 / countG1;
          
          if (newY * 2 + g2OffsetY < newHeight && newX * 2 + g2OffsetX < newWidth && countG2 > 0)
            binMat.at<int32_t>(newY * 2 + g2OffsetY, newX * 2 + g2OffsetX) = sumG2 / countG2;
          
          if (newY * 2 + bOffsetY < newHeight && newX * 2 + bOffsetX < newWidth && countB > 0)
            binMat.at<int32_t>(newY * 2 + bOffsetY, newX * 2 + bOffsetX) = sumB / countB;
        }
      }
    }
  } catch (cv::Exception &e) {
    Logger::Log("PixelsDataSoftBin_Bayer | 处理图像异常: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
    binMat.release();  // 释放已分配的内存
    return cv::Mat();
  } catch (std::exception &e) {
    Logger::Log("PixelsDataSoftBin_Bayer | 处理图像标准异常: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
    binMat.release();  // 释放已分配的内存
    return cv::Mat();
  } catch (...) {
    Logger::Log("PixelsDataSoftBin_Bayer | 处理图像未知异常", LogLevel::ERROR, DeviceType::MAIN);
    binMat.release();  // 释放已分配的内存
    return cv::Mat();
  }
  newWidth = binMat.cols;
  newHeight = binMat.rows;
  if (binMat.cols % 2 != 0) {
    newWidth = binMat.cols - 1;
  }
  if (binMat.rows % 2 != 0) {
    newHeight = binMat.rows - 1;
  }
  // 检查新图像尺寸是否为0
  if (newWidth == 0 || newHeight == 0) {
    Logger::Log("PixelsDataSoftBin_Bayer | 错误: 缩放后尺寸为0! " + std::to_string(newWidth) + "x" + std::to_string(newHeight), 
                LogLevel::ERROR, DeviceType::MAIN);
    binMat.release();  // 释放已分配的内存
    return cv::Mat();
  }
  
  // 创建输出图像
  cv::Mat newbinMat;
  try {
    if (binMat.type() == CV_8U) {
      newbinMat = cv::Mat::zeros(newHeight, newWidth, CV_8U);
    } else if (binMat.type() == CV_16U) {
      newbinMat = cv::Mat::zeros(newHeight, newWidth, CV_16U);
    } else if (binMat.type() == CV_32S) {
      newbinMat = cv::Mat::zeros(newHeight, newWidth, CV_32S);
    }
    Logger::Log("PixelsDataSoftBin_Bayer | 输出图像创建成功", LogLevel::INFO, DeviceType::MAIN);

    // 复制数据
    if (binMat.cols != newbinMat.cols || binMat.rows != newbinMat.rows) {
      // 需要调整大小，复制有效区域
      try {
        cv::Rect roi(0, 0, newWidth, newHeight);
        binMat(roi).copyTo(newbinMat);
        Logger::Log("PixelsDataSoftBin_Bayer | 图像数据裁剪并复制成功", LogLevel::INFO, DeviceType::MAIN);
      } catch (cv::Exception &e) {
        Logger::Log("PixelsDataSoftBin_Bayer | 复制图像数据异常: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
        binMat.release();  // 释放中间图像
        newbinMat.release();  // 释放输出图像
        return cv::Mat();
      }
    } else {
      // 尺寸相同，直接返回binMat
      newbinMat = binMat.clone();
      Logger::Log("PixelsDataSoftBin_Bayer | 图像数据直接复制成功", LogLevel::INFO, DeviceType::MAIN);
    }
    // 释放中间结果图像内存
    binMat.release();
  } catch (cv::Exception &e) {
    Logger::Log("PixelsDataSoftBin_Bayer | 创建输出图像异常: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
    binMat.release();  // 释放中间图像
    return cv::Mat();
  }
  Logger::Log("PixelsDataSoftBin_Bayer | 完成处理，返回结果图像 " + std::to_string(newbinMat.cols) + "x" + std::to_string(newbinMat.rows), 
              LogLevel::DEBUG, DeviceType::MAIN);
  
  // 因为返回值是一个拷贝，所以不需要担心返回后的内存泄漏
  return newbinMat;
}


uint32_t Tools::PixelsDataSoftBin_AVG(uint8_t *srcdata, uint8_t *bindata, uint32_t width, uint32_t height, uint32_t depth, uint32_t camxbin, uint32_t camybin)
{
  uint32_t stride = width;
  uint32_t newStride = width / camxbin;
  if (depth == 8)
  {
    uint8_t *temp;
    temp = (unsigned char *)malloc(2 * newStride * (height / camybin));
    memset(temp, 0, 2 * newStride * (height / camybin));
    for (uint32_t i = 0; i < height / camybin; i++)
    {
      for (uint32_t v = 0; v < camybin; v++)
      {
        uint16_t *pd = (uint16_t *)temp + newStride * i;
        uint8_t *ps = (uint8_t *)srcdata + stride * (i * camybin + v);
        for (uint32_t j = 0; j < width / camxbin; j++)
        {
          for (uint32_t h = 0; h < camxbin; h++)
          {
            uint32_t y = (uint16_t)(*pd + *ps);
            *pd = y;
            ps++;
          }
          pd++;
        }
      }
    }
    uint32_t i = 0;
    memset(bindata, 0, newStride * (height / camybin));
    uint16_t *tempbin = (uint16_t *)temp + i;
    uint8_t *bin = bindata + i;
    for (i = 0; i < newStride * (height / camybin); i++)
    {
      uint32_t avg = (uint8_t)(*tempbin / (camxbin * camybin));
      *bin = avg;
      bin++;
      tempbin++;
    }
    free(temp);
    // OutputDebugPrintf(QHYCCD_MSGL_INFO,"QHYCCD|QHYBASE.CPP|PixelsDataSoftBin_AVG 8bit end");
    return QHYCCD_SUCCESS;
  }
  else if (depth == 16)
  { // OutputDebugPrintf(QHYCCD_MSGL_INFO,"QHYCCD|QHYBASE.CPP|PixelsDataSoftBin_AVG 16bit");
    uint8_t *temp;
    temp = (unsigned char *)malloc(4 * newStride * (height / camybin));
    memset(temp, 0, 4 * newStride * (height / camybin));
    for (uint32_t i = 0; i < height / camybin; i++)
    {
      for (uint32_t v = 0; v < camybin; v++)
      {
        uint32_t *pd = (uint32_t *)temp + newStride * i;
        uint16_t *ps = (uint16_t *)srcdata + stride * (i * camybin + v);
        for (uint32_t j = 0; j < width / camxbin; j++)
        { // OutputDebugPrintf(QHYCCD_MSGL_INFO,"QHYCCD|QHYBASE.CPP|PixelsDataSoftBin_AVG i=%d  v=%d  j=%d",i,v,j);
          for (uint32_t h = 0; h < camxbin; h++)
          {
            uint32_t y = (uint32_t)(*pd + *ps);
            *pd = y;
            ps++;
          }
          pd++;
        }
      }
    }
    // OutputDebugPrintf(QHYCCD_MSGL_INFO,"QHYCCD|QHYBASE.CPP|PixelsDataSoftBin_AVG 16bit sum");
    uint32_t i = 0;
    memset(bindata, 0, 2 * newStride * (height / camybin));
    uint32_t *tempbin = (uint32_t *)temp + i;
    uint16_t *bin = (uint16_t *)bindata + i;
    // OutputDebugPrintf(QHYCCD_MSGL_INFO,"QHYCCD|QHYBASE.CPP|PixelsDataSoftBin_AVG 16bit for start");
    for (i = 0; i < newStride * (height / camybin); i++) // 2*
    {
      uint32_t avg = (uint16_t)(*tempbin / (camxbin * camybin));
      *bin = avg;
      bin++;
      tempbin++;
    }
    // OutputDebugPrintf(QHYCCD_MSGL_INFO,"QHYCCD|QHYBASE.CPP|PixelsDataSoftBin_AVG 16bit for end");
    free(temp);
    // OutputDebugPrintf(QHYCCD_MSGL_INFO,"QHYCCD|QHYBASE.CPP|PixelsDataSoftBin_AVG 16bit end");
    return QHYCCD_SUCCESS;
  }
  else if (depth == 32)
  {

    uint32_t *pd;
    uint32_t *ps;
    memset(bindata, 0, 4 * newStride * (height / camybin));

    for (uint32_t i = 0; i < height / camybin; i++)
    {
      for (uint32_t v = 0; v < camybin; v++)
      {
        pd = (uint32_t *)bindata + newStride * i;
        ps = (uint32_t *)srcdata + stride * (i * camybin + v);
        for (uint32_t j = 0; j < width / camxbin; j++)
        {
          for (uint32_t h = 0; h < camxbin; h++)
          {
            uint32_t y = *pd + *ps;
            *pd = y;
            ps++;
          }
          pd++;
        }
      }
    }

    pd = (uint32_t *)(bindata);

    for (uint32_t i = 0; i < height / camybin; i++)
    {
      for (uint32_t j = 0; j < width / camxbin; j++)
      {
        *pd = *pd / (camxbin * camybin);
        pd++;
      }
    }

    return QHYCCD_SUCCESS;
  }
  return QHYCCD_ERROR;
}

// uint32_t Tools::PixelsDataSoftBin(uint8_t *srcdata, uint8_t *bindata, uint32_t width, uint32_t height, uint32_t depth, uint32_t camxbin, uint32_t camybin, bool iscolor)
// {
//   if (iscolor)
//   {
//     unsigned char *data = NULL;
//     if (srcdata == bindata)
//     {
//       data = new unsigned char[(width * depth / 8 + 3) / 4 * 4 * height];
//       memcpy(data, srcdata, (width * depth / 8 + 3) / 4 * 4 * height);
//       srcdata = data;
//     }
//     if (depth == 8)
//     {
//       memset(bindata, 0, (width / camxbin) * (height / camybin));
//       for (uint32_t i = 0; i < height / camybin / 2; i++)
//       {
//         uint8_t *pd = bindata + width / camxbin * i * 2;
//         uint8_t *ps = srcdata + width * camxbin * i * 2;
//         uint8_t *psEnd = ps + width / camxbin * camxbin - 1;
//         for (; ps < psEnd - 1; ps += camxbin * 2, pd += 2)
//         {
//           for (int yi = 1; yi <= camybin; yi++)
//           {
//             for (int xi = 1; xi <= camxbin; xi++)
//             {
//               uint32_t y00 = LimitByte(pd[0] + ps[2 * (yi - 1) * width + 2 * (xi - 1)]);
//               uint32_t y01 = LimitByte(pd[1] + ps[2 * (yi - 1) * width + 2 * (xi - 1) + 1]);
//               uint32_t y10 = LimitByte(pd[width / camxbin + 0] + ps[width * (2 * yi - 1) + 2 * (xi - 1)]);
//               uint32_t y11 = LimitByte(pd[width / camxbin + 1] + ps[width * (2 * yi - 1) + 2 * xi - 1]);
//               pd[0] = y00;
//               pd[1] = y01;
//               pd[width / camxbin + 0] = y10;
//               pd[width / camxbin + 1] = y11;
//             }
//           }
//         }
//       }
//       return QHYCCD_SUCCESS;
//     }
//     else if (depth == 16)
//     {
//       memset(bindata, 0, 2 * (width / camxbin) * (height / camybin));
//       for (uint32_t i = 0; i < height / camybin / 2; i++)
//       {
//         uint16_t *pd = (uint16_t *)bindata + width / camxbin * i * 2;
//         uint16_t *ps = (uint16_t *)srcdata + width * camxbin * i * 2;
//         uint16_t *psEnd = ps + width / camxbin * camxbin - 1;
//         for (; ps < psEnd - 1; ps += camxbin * 2, pd += 2)
//         {
//           for (int yi = 1; yi <= camybin; yi++)
//           {
//             for (int xi = 1; xi <= camxbin; xi++)
//             {
//               uint32_t y00 = LimitShort(pd[0] + ps[2 * (yi - 1) * width + 2 * (xi - 1)]);
//               uint32_t y01 = LimitShort(pd[1] + ps[2 * (yi - 1) * width + 2 * (xi - 1) + 1]);
//               uint32_t y10 = LimitShort(pd[width / camxbin + 0] + ps[width * (2 * yi - 1) + 2 * (xi - 1)]);
//               uint32_t y11 = LimitShort(pd[width / camxbin + 1] + ps[width * (2 * yi - 1) + 2 * xi - 1]);
//               pd[0] = y00;
//               pd[1] = y01;
//               pd[width / camxbin + 0] = y10;
//               pd[width / camxbin + 1] = y11;
//             }
//           }
//         }
//       }
//       return QHYCCD_SUCCESS;
//     }
//     else if (depth == 32)
//     {
//       memset(bindata, 0, 4 * (width / camxbin) * (height / camybin));
//       for (uint32_t i = 0; i < height / camybin / 2; i++)
//       {
//         uint32_t *pd = (uint32_t *)bindata + width / camxbin * i * 2;
//         uint32_t *ps = (uint32_t *)srcdata + width * camxbin * i * 2;
//         uint32_t *psEnd = ps + width / camxbin * camxbin - 1;
//         for (; ps < psEnd - 1; ps += camxbin * 2, pd += 2)
//         {
//           for (int yi = 1; yi <= camybin; yi++)
//           {
//             for (int xi = 1; xi <= camxbin; xi++)
//             {
//               pd[0] += ps[2 * (yi - 1) * width + 2 * (xi - 1)];
//               pd[1] += ps[2 * (yi - 1) * width + 2 * (xi - 1) + 1];
//               pd[width / camxbin + 0] += ps[width * (2 * yi - 1) + 2 * (xi - 1)];
//               pd[width / camxbin + 1] += ps[width * (2 * yi - 1) + 2 * xi - 1];
//             }
//           }
//         }
//       }
//       return QHYCCD_SUCCESS;
//     }
//     if (data != NULL)
//     {
//       delete[] data;
//     }
//   }
//   else
//   {
//     uint32_t stride = width;
//     uint32_t newStride = width / camxbin;

//     if (depth == 8)
//     {
//       memset(bindata, 0, newStride * (height / camybin));
//       for (uint32_t i = 0; i < height / camybin; i++)
//       {
//         for (uint32_t v = 0; v < camybin; v++)
//         {
//           uint8_t *pd = bindata + newStride * i;
//           uint8_t *ps = srcdata + stride * (i * camybin + v);
//           for (uint32_t j = 0; j < width / camxbin; j++)
//           {
//             for (uint32_t h = 0; h < camxbin; h++)
//             {
//               uint32_t y = LimitByte(*pd + *ps);
//               *pd = y;
//               ps++;
//             }
//             pd++;
//           }
//         }
//       }
//       return QHYCCD_SUCCESS;
//     }
//     else if (depth == 16)
//     {
//       memset(bindata, 0, 2 * newStride * (height / camybin));
//       for (uint32_t i = 0; i < height / camybin; i++)
//       {
//         for (uint32_t v = 0; v < camybin; v++)
//         {
//           uint16_t *pd = (uint16_t *)bindata + newStride * i;
//           uint16_t *ps = (uint16_t *)srcdata + stride * (i * camybin + v);
//           for (uint32_t j = 0; j < width / camxbin; j++)
//           {
//             for (uint32_t h = 0; h < camxbin; h++)
//             {
//               uint32_t y = LimitShort(*pd + *ps);
//               *pd = y;
//               ps++;
//             }
//             pd++;
//           }
//         }
//       }
//       return QHYCCD_SUCCESS;
//     }
//     else if (depth == 32)
//     {
//       memset(bindata, 0, 4 * newStride * (height / camybin));

//       for (uint32_t i = 0; i < height / camybin; i++)
//       {
//         for (uint32_t v = 0; v < camybin; v++)
//         {
//           uint32_t *pd = (uint32_t *)bindata + newStride * i;
//           uint32_t *ps = (uint32_t *)srcdata + stride * (i * camybin + v);
//           for (uint32_t j = 0; j < width / camxbin; j++)
//           {
//             for (uint32_t h = 0; h < camxbin; h++)
//             {
//               uint32_t y = *pd + *ps;
//               *pd = y;
//               ps++;
//             }
//             pd++;
//           }
//         }
//       }
//       return QHYCCD_SUCCESS;
//     }
//   }
//   return QHYCCD_ERROR;
// }

uint32_t Tools::PixelsDataSoftBin(uint8_t *srcdata, uint8_t *bindata, uint32_t width, uint32_t height, uint32_t camchannels, uint32_t depth, uint32_t camxbin, uint32_t camybin, bool iscolor)
{
  Logger::Log("QHYCCD | QHYBASE.CPP | PixelsDataSoftBin | width = " + std::to_string(width) + " height = " + std::to_string(height) + " camchannels = " + std::to_string(camchannels) + " depth = " + std::to_string(depth) + " camxbin = " + std::to_string(camxbin) + " camybin = " + std::to_string(camybin) + " iscolor = " + std::to_string(iscolor), LogLevel::INFO, DeviceType::MAIN);
  if (iscolor)
  {
    if (depth == 8 && camchannels == 3)
    {
      Logger::Log("depth = 8, camchannels = 3", LogLevel::INFO, DeviceType::MAIN);
      unsigned char *data = NULL;
      if (srcdata == bindata)
      {
        data = new unsigned char[width * height * camchannels];
        memcpy(data, srcdata, width * height * camchannels);
        srcdata = data;
      }
      cv::Mat srcMat(cv::Size(width, height), CV_8UC(camchannels));
      cv::Mat dstMat(cv::Size(width / camxbin, height / camybin), CV_8UC(camchannels));
      memcpy(srcMat.data, srcdata, srcMat.cols * srcMat.rows * srcMat.channels());
      cv::resize(srcMat, dstMat, cv::Size(dstMat.cols, dstMat.rows));
      memcpy(bindata, dstMat.data, dstMat.cols * dstMat.rows * dstMat.channels());
      srcMat.release();
      dstMat.release();
      if (data != NULL)
      {
        delete[] data;
      }
      return QHYCCD_SUCCESS;
    }
    else if (depth == 16 && camchannels == 3)
    {
      Logger::Log("depth = 16, camchannels = 3", LogLevel::INFO, DeviceType::MAIN);
      unsigned char *data = NULL;
      if (srcdata == bindata)
      {
        data = new unsigned char[2 * width * height * camchannels];
        memcpy(data, srcdata, 2 * width * height * camchannels);
        srcdata = data;
      }
      Logger::Log("memcpy 1", LogLevel::INFO, DeviceType::MAIN);
      cv::Mat srcMat(cv::Size(2 * width, height), CV_16UC(camchannels));
      cv::Mat dstMat(cv::Size(2 * width / camxbin, height / camybin), CV_16UC(camchannels));
      memcpy(srcMat.data, srcdata, srcMat.cols * srcMat.rows * srcMat.channels());
      Logger::Log("memcpy 2", LogLevel::INFO, DeviceType::MAIN);
      cv::resize(srcMat, dstMat, cv::Size(dstMat.cols, dstMat.rows));
      memcpy(bindata, dstMat.data, dstMat.cols * dstMat.rows * dstMat.channels());
      Logger::Log("memcpy 3", LogLevel::INFO, DeviceType::MAIN);
      srcMat.release();
      dstMat.release();
      if (data != NULL)
      {
        delete[] data;
      }
      return QHYCCD_SUCCESS;
    }
    else
    {
      unsigned char *data = NULL;
      if (srcdata == bindata)
      {
        data = new unsigned char[(width * depth / 8 + 3) / 4 * 4 * height];
        memcpy(data, srcdata, (width * depth / 8 + 3) / 4 * 4 * height);
        srcdata = data;
      }
      if (depth == 8) // camchannels = 1
      {
        Logger::Log("depth = 8, camchannels = 1", LogLevel::INFO, DeviceType::MAIN);
        memset(bindata, 0, (width / camxbin) * (height / camybin));
        for (uint32_t i = 0; i < height / camybin / 2; i++)
        {
          uint8_t *pd = bindata + width / camxbin * i * 2;
          uint8_t *ps = srcdata + width * camxbin * i * 2;
          uint8_t *psEnd = ps + width / camxbin * camxbin;
          for (; ps < psEnd - camxbin * 2+1; ps += camxbin * 2, pd += 2)
          {
            for (int yi = 1; yi <= camybin; yi++)
            {
              for (int xi = 1; xi <= camxbin; xi++)
              {
                uint32_t y00 = LimitByte(pd[0] + ps[2 * (yi - 1) * width + 2 * (xi - 1)]);
                uint32_t y01 = LimitByte(pd[1] + ps[2 * (yi - 1) * width + 2 * (xi - 1) + 1]);
                uint32_t y10 = LimitByte(pd[width / camxbin + 0] + ps[width * (2 * yi - 1) + 2 * (xi - 1)]);
                uint32_t y11 = LimitByte(pd[width / camxbin + 1] + ps[width * (2 * yi - 1) + 2 * xi - 1]);
                pd[0] = y00;
                pd[1] = y01;
                pd[width / camxbin + 0] = y10;
                pd[width / camxbin + 1] = y11;
              }
            }
          }
        }
        return QHYCCD_SUCCESS;
      }
      else if (depth == 16) // camchannels = 1
      {
        Logger::Log("depth = 16, camchannels = 1", LogLevel::INFO, DeviceType::MAIN);
        memset(bindata, 0, 2 * (width / camxbin) * (height / camybin));
        Logger::Log("memcpy 1", LogLevel::INFO, DeviceType::MAIN);
        for (uint32_t i = 0; i < height / camybin / 2; i++)
        {
          uint16_t *pd = (uint16_t *)bindata + width / camxbin * i * 2;
          uint16_t *ps = (uint16_t *)srcdata + width * camxbin * i * 2;
          uint16_t *psEnd = ps + width / camxbin * camxbin;
          for (; ps < psEnd - camxbin * 2+1; ps += camxbin * 2, pd += 2)
          {
            for (int yi = 1; yi <= camybin; yi++)
            {
              for (int xi = 1; xi <= camxbin; xi++)
              {
                uint32_t y00 = LimitShort(pd[0] + ps[2 * (yi - 1) * width + 2 * (xi - 1)]);
                uint32_t y01 = LimitShort(pd[1] + ps[2 * (yi - 1) * width + 2 * (xi - 1) + 1]);
                uint32_t y10 = LimitShort(pd[width / camxbin + 0] + ps[width * (2 * yi - 1) + 2 * (xi - 1)]);
                uint32_t y11 = LimitShort(pd[width / camxbin + 1] + ps[width * (2 * yi - 1) + 2 * xi - 1]);
                pd[0] = y00;
                pd[1] = y01;
                pd[width / camxbin + 0] = y10;
                pd[width / camxbin + 1] = y11;
              }
            }
          }
        }
        return QHYCCD_SUCCESS;
      }
      else if (depth == 32) // camchannels = 1
      {
        Logger::Log("depth = 32, camchannels = 1", LogLevel::INFO, DeviceType::MAIN);
        memset(bindata, 0, 4 * (width / camxbin) * (height / camybin));
        for (uint32_t i = 0; i < height / camybin / 2; i++)
        {
          uint32_t *pd = (uint32_t *)bindata + width / camxbin * i * 2;
          uint32_t *ps = (uint32_t *)srcdata + width * camxbin * i * 2;
          uint32_t *psEnd = ps + width / camxbin * camxbin - 1;
          for (; ps < psEnd - 1; ps += camxbin * 2, pd += 2)
          {
            for (int yi = 1; yi <= camybin; yi++)
            {
              for (int xi = 1; xi <= camxbin; xi++)
              {
                pd[0] += ps[2 * (yi - 1) * width + 2 * (xi - 1)];
                pd[1] += ps[2 * (yi - 1) * width + 2 * (xi - 1) + 1];
                pd[width / camxbin + 0] += ps[width * (2 * yi - 1) + 2 * (xi - 1)];
                pd[width / camxbin + 1] += ps[width * (2 * yi - 1) + 2 * xi - 1];
              }
            }
          }
        }
        return QHYCCD_SUCCESS;
      }
      if (data != NULL)
      {
        delete[] data;
      }
    }
  }
  else  // MONO Image
  {
    uint32_t stride = width;
    uint32_t newStride = width / camxbin;

    if (depth == 8)
    {
      memset(bindata, 0, newStride * (height / camybin));
      for (uint32_t i = 0; i < height / camybin; i++)
      {
        for (uint32_t v = 0; v < camybin; v++)
        {
          uint8_t *pd = bindata + newStride * i;
          uint8_t *ps = srcdata + stride * (i * camybin + v);
          for (uint32_t j = 0; j < width / camxbin; j++)
          {
            for (uint32_t h = 0; h < camxbin; h++)
            {
              uint32_t y = LimitByte(*pd + *ps);
              *pd = y;
              ps++;
            }
            pd++;
          }
        }
      }
      return QHYCCD_SUCCESS;
    }
    else if (depth == 16)
    {
      memset(bindata, 0, 2 * newStride * (height / camybin));
      for (uint32_t i = 0; i < height / camybin; i++)
      {
        for (uint32_t v = 0; v < camybin; v++)
        {
          uint16_t *pd = (uint16_t *)bindata + newStride * i;
          uint16_t *ps = (uint16_t *)srcdata + stride * (i * camybin + v);
          for (uint32_t j = 0; j < width / camxbin; j++)
          {
            for (uint32_t h = 0; h < camxbin; h++)
            {
              uint32_t y = LimitShort(*pd + *ps);
              *pd = y;
              ps++;
            }
            pd++;
          }
        }
      }
      return QHYCCD_SUCCESS;
    }
    else if (depth == 32)
    {
      memset(bindata, 0, 4 * newStride * (height / camybin));

      for (uint32_t i = 0; i < height / camybin; i++)
      {
        for (uint32_t v = 0; v < camybin; v++)
        {
          uint32_t *pd = (uint32_t *)bindata + newStride * i;
          uint32_t *ps = (uint32_t *)srcdata + stride * (i * camybin + v);
          for (uint32_t j = 0; j < width / camxbin; j++)
          {
            for (uint32_t h = 0; h < camxbin; h++)
            {
              uint32_t y = *pd + *ps;
              *pd = y;
              ps++;
            }
            pd++;
          }
        }
      }
      return QHYCCD_SUCCESS;
    }
  }
  return QHYCCD_ERROR;
}

void Tools::SaveMatTo8BitJPG(cv::Mat image)
{
  if (image.empty())
  {
    Logger::Log("输入图像为空，无法保存！", LogLevel::ERROR, DeviceType::MAIN);
    return;
  }

  // 打印输入图像的信息
  Logger::Log("Input image type: " + std::to_string(image.type()) + ", size: " + std::to_string(image.size().width) + "x" + std::to_string(image.size().height), LogLevel::INFO, DeviceType::MAIN);

  cv::Mat image16;
  cv::Mat SendImage;

  // 确保输入图像是8位深度
  if (image.depth() == CV_8U)
  {
    Logger::Log("256, 0", LogLevel::INFO, DeviceType::MAIN);
    image.convertTo(image16, CV_16UC1, 256, 0); // x256  MSB alignment
  }
  else if (image.depth() == CV_16U)
  {
    Logger::Log("1, 0", LogLevel::INFO, DeviceType::MAIN);
    image.convertTo(image16, CV_16UC1, 1, 0);
  }
  else
  {
    Logger::Log("Unsupported image depth: " + std::to_string(image.depth()), LogLevel::ERROR, DeviceType::MAIN);
    return;
  }

  // 打印转换后图像的信息
  Logger::Log("Converted image type: " + std::to_string(image16.type()) + ", size: " + std::to_string(image16.size().width) + "x" + std::to_string(image16.size().height), LogLevel::INFO, DeviceType::MAIN);

  cv::Mat NewImage = image16;

  // 打印新图像的信息
  Logger::Log("New image type: " + std::to_string(NewImage.type()) + ", size: " + std::to_string(NewImage.size().width) + "x" + std::to_string(NewImage.size().height), LogLevel::INFO, DeviceType::MAIN);

  // 将图像缩放到0-255范围内
  cv::normalize(NewImage, SendImage, 0, 255, cv::NORM_MINMAX, CV_8U);
  // cv::convertScaleAbs(NewImage, SendImage, 1 / 256.0);

  // 打印最终图像的信息
  Logger::Log("SendImage type: " + std::to_string(SendImage.type()) + ", size: " + std::to_string(SendImage.size().width) + "x" + std::to_string(SendImage.size().height), LogLevel::INFO, DeviceType::MAIN);

  std::string outputFilename = "/dev/shm/MatTo8BitJPG.jpg";
  bool saved = cv::imwrite(outputFilename, SendImage);

  if (!saved)
  {
    Logger::Log("图像保存失败！", LogLevel::ERROR, DeviceType::MAIN);
  }
  else
  {
    Logger::Log("图像已成功保存到: " + outputFilename, LogLevel::INFO, DeviceType::MAIN);
  }
}

void Tools::SaveMatTo16BitPNG(cv::Mat image)
{
    if (image.empty())
    {
        Logger::Log("输入图像为空，无法保存！", LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    // 打印输入图像的信息
    Logger::Log("Input image type: " + std::to_string(image.type()) + ", size: " + std::to_string(image.size().width) + "x" + std::to_string(image.size().height), LogLevel::INFO, DeviceType::MAIN);

    cv::Mat image16;

    // 如果输入图像是 8 位深度，将其转换为 16 位深度
    if (image.depth() == CV_8U)
    {
        Logger::Log("将 8 位图像转换为 16 位...", LogLevel::INFO, DeviceType::MAIN);
        image.convertTo(image16, CV_16U, 256.0); // 将8位深度转换为16位深度，x256以扩展范围
    }
    else if (image.depth() == CV_16U)
    {
        // 图像已经是 16 位深度
        image16 = image;
    }
    else
    {
        Logger::Log("Unsupported image depth: " + std::to_string(image.depth()), LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    // 打印转换后图像的信息
    Logger::Log("Converted image type: " + std::to_string(image16.type()) + ", size: " + std::to_string(image16.size().width) + "x" + std::to_string(image16.size().height), LogLevel::INFO, DeviceType::MAIN);

    std::string outputFilename = "/dev/shm/MatTo16BitPNG.png";
    bool saved = cv::imwrite(outputFilename, image16); // 使用 PNG 格式保存

    if (!saved)
    {
        Logger::Log("图像保存失败！", LogLevel::ERROR, DeviceType::MAIN);
    }
    else
    {
        Logger::Log("图像已成功保存到: " + outputFilename, LogLevel::INFO, DeviceType::MAIN);
    }
}

void Tools::SaveMatToFITS(const cv::Mat& image) {
    if (image.empty()) {
        Logger::Log("输入图像为空！", LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    // 确保图像是灰度图
    cv::Mat gray;
    if (image.channels() == 3) {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = image;
    }

    // 根据图像类型动态设置 bitpix 和数据类型
    int bitpix;
    int datatype;
    if (gray.depth() == CV_8U) {
        bitpix = BYTE_IMG;   // 8 位无符号整型
        datatype = TBYTE;
    } else if (gray.depth() == CV_16U) {
        bitpix = SHORT_IMG;  // 16 位无符号整型
        datatype = TSHORT;
    } else {
        Logger::Log("不支持的图像位深度！", LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    // FITS文件要求图像数据是二维的
    long naxes[2] = {gray.cols, gray.rows};

    // 创建FITS文件，使用 '!' 来覆盖已存在的文件
    fitsfile* fptr;
    int status = 0;  // 状态变量必须初始化为0
    std::string filename = "!/dev/shm/MatToFITS.fits";  // 加 ! 来覆盖文件
    fits_create_file(&fptr, filename.c_str(), &status);
    fits_create_img(fptr, bitpix, 2, naxes, &status);

    // 将图像数据写入FITS文件
    if (gray.depth() == CV_8U) {
        fits_write_img(fptr, datatype, 1, gray.total(), gray.ptr<uchar>(), &status);
    } else if (gray.depth() == CV_16U) {
        fits_write_img(fptr, datatype, 1, gray.total(), gray.ptr<ushort>(), &status);
    }

    // 关闭FITS文件
    fits_close_file(fptr, &status);

    if (status) {
        fits_report_error(stderr, status);  // 输出错误信息
    } else {
        Logger::Log("成功保存图像到 " + filename, LogLevel::INFO, DeviceType::MAIN);
    }
}

// void Tools::SaveMatToFITS(const cv::Mat& image) {
//     if (image.empty()) {
//         Logger::Log("输入图像为空！", LogLevel::ERROR, DeviceType::MAIN);
//         return;
//     }

//     // 确保图像是灰度图
//     cv::Mat gray;
//     if (image.channels() == 3) {
//         cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
//     } else {
//         gray = image;
//     }

//     // FITS文件要求图像数据是二维的
//     long naxes[2] = {gray.cols, gray.rows};
//     int bitpix = SHORT_IMG;  // 使用16位整型保存

//     // 创建FITS文件，使用 '!' 来覆盖已存在的文件
//     fitsfile* fptr;
//     int status = 0;  // 状态变量必须初始化为0
//     std::string filename = "!/dev/shm/MatToFITS.fits";  // 加 ! 来覆盖文件
//     fits_create_file(&fptr, filename.c_str(), &status);
//     fits_create_img(fptr, bitpix, 2, naxes, &status);

//     // 将图像数据写入FITS文件
//     fits_write_img(fptr, TSHORT, 1, gray.total(), gray.ptr<short>(), &status);

//     // 关闭FITS文件
//     fits_close_file(fptr, &status);

//     if (status) {
//         fits_report_error(stderr, status);  // 输出错误信息
//     } else {
//         Logger::Log("成功保存图像到 " + filename, LogLevel::INFO, DeviceType::MAIN);
//     }
// }

/*************************************************************************
********************************Coordinate Convert*************************
*************************************************************************/

QDateTime Tools::getSystemTimeUTC(void) {
  QDateTime datetime = QDateTime::currentDateTime();
  datetime.setTimeSpec(Qt::LocalTime);

  return datetime.toUTC();
}

/*************************************************
 *Convert between Degree and Radian
 * ************************************************/
double Tools::rangeTo(double value, double max, double min) {
  double output_value;
  double period = max - min;
  if (value < min) {
    while (value < min) {
      value = value + period;
    }
  }

  else if (value > max) {
    while (value > max) {
      value = value - period;
    }
  }

  return value;
}

// 2023.12.21 CJQ
double Tools::getLST_Degree(QDateTime datetimeUTC, double longitude_radian) {
  int year = datetimeUTC.date().year();
  int month = datetimeUTC.date().month();
  int day = datetimeUTC.date().day();
  int hour = datetimeUTC.time().hour();
  int minute = datetimeUTC.time().minute();
  int second = datetimeUTC.time().second();
  int msec = datetimeUTC.time().msec();

#ifdef debug
  Logger::Log("tools.cpp|getLST_Degree|datetimeUTC:" + datetimeUTC.toString("yyyy-MM-dd HH:mm:ss.zzz"), LogLevel::INFO, DeviceType::MAIN);
  Logger::Log("tools.cpp|getLST_Degree|datetimeUTC:" + std::to_string(year) + "-" + std::to_string(month) + "-" + std::to_string(day) + " " + std::to_string(hour) + ":" + std::to_string(minute) + ":" + std::to_string(second) + "." + std::to_string(msec), LogLevel::INFO, DeviceType::MAIN);
#endif

  double jd;
  getJDFromDate(&jd, year, month, day, hour, minute, second);

  double d;
  d = jd - 2451545.0;

#ifdef debug
  Logger::Log("tools.cpp|getLST_Degree|d = " + std::to_string(d), LogLevel::INFO, DeviceType::MAIN);
#endif

  double UT;

  UT = hour + (minute * 60 + second + (double)msec / 1000) / 3600.0;

#ifdef debug
  Logger::Log("tools.cpp|getLST_Degree|UT = " + std::to_string(UT), LogLevel::INFO, DeviceType::MAIN);
#endif

  double longitude_Degree = RadToDegree(longitude_radian);

#ifdef debug
  Logger::Log("tools.cpp|getLST_Degree|longitude (degree) = " + std::to_string(longitude_Degree), LogLevel::INFO, DeviceType::MAIN);
#endif

  double LST;

  LST = 100.46 + 0.985647 * d + longitude_Degree + 15 * UT;

#ifdef debug
  Logger::Log("tools.cpp|getLST_Degree|LST before range = " + std::to_string(LST), LogLevel::INFO, DeviceType::MAIN);
#endif

  LST = rangeTo(LST, 360.0, 0.0);

#ifdef debug
  Logger::Log("tools.cpp|getLST_Degree|LST after  range (degree) " + std::to_string(LST) + " (hms) " + radToHmsStr(DegreeToRad(LST), true), LogLevel::INFO, DeviceType::MAIN);
#endif

  return LST;
}

bool Tools::getJDFromDate(double *newjd, const int y, const int m, const int d, const int h, const int min, const float s)
{
    static const long IGREG2 = 15 + 31L * (10 + 12L * 1582);
    double deltaTime = (h / 24.0) + (min / (24.0 * 60.0)) + (static_cast<double>(s) / (24.0 * 60.0 * 60.0)) - 0.5;
    QDate test((y <= 0 ? y - 1 : y), m, d);
    // if QDate will oblige, do so.
    // added hook for Julian calendar, because it has been removed from Qt5 --AW
    if (test.isValid() && y > 1582)
    {
        double qdjd = static_cast<double>(test.toJulianDay());
        qdjd += deltaTime;
        *newjd = qdjd;
        return true;
    }
    else
    {
        /*
         * Algorithm taken from "Numerical Recipes in C, 2nd Ed." (1992), pp. 11-12
         */
        long ljul;
        long jy, jm;
        long laa, lbb, lcc, lee;

        jy = y;
        if (m > 2)
        {
            jm = m + 1;
        }
        else
        {
            --jy;
            jm = m + 13;
        }

        laa = 1461 * jy / 4;
        if (jy < 0 && jy % 4)
        {
            --laa;
        }
        lbb = 306001 * jm / 10000;
        ljul = laa + lbb + d + 1720995L;

        if (d + 31L * (m + 12L * y) >= IGREG2)
        {
            lcc = jy / 100;
            if (jy < 0 && jy % 100)
            {
                --lcc;
            }
            lee = lcc / 4;
            if (lcc < 0 && lcc % 4)
            {
                --lee;
            }
            ljul += 2 - lcc + lee;
        }
        double jd = static_cast<double>(ljul);
        jd += deltaTime;
        *newjd = jd;
        return true;
    }
}

double Tools::getHA_Degree(double RA_radian, double LST_Degree) {
  double HA;
  HA = LST_Degree - RadToDegree(RA_radian);

  HA = rangeTo(HA, 360.0, 0.0);

#ifdef debug
  Logger::Log("tools.cpp|getHA|HA (degree) " + std::to_string(HA) + " (hms) " + radToHmsStr(DegreeToRad(HA), true), LogLevel::INFO, DeviceType::MAIN);
#endif

  return HA;
}

void Tools::ra_dec_to_alt_az(double ha_radian, double dec_radian,
                             double& alt_radian, double& az_radian,
                             double lat_radian) {
  // the sin , cos , asin , acos  must use radian.
  double temp;
  double cos_lat = cos(lat_radian);

  alt_radian = asin(sin(lat_radian) * sin(dec_radian) +
                    cos_lat * cos(dec_radian) * cos(ha_radian));
  if (cos_lat < .00001)
    az_radian = ha_radian; /* polar case */
  else {
    temp = acos((sin(dec_radian) - sin(alt_radian) * sin(lat_radian)) /
                (cos(alt_radian) * cos_lat));

    if (sin(ha_radian) > 0)
      az_radian = 2 * M_PI - temp;
    else
      az_radian = temp;
  }
#ifdef debug
  Logger::Log("tools.cpp|ra_dec_to_alt_az|az alt (radian):" + std::to_string(az_radian) + " " + std::to_string(alt_radian) + " (Degree) " + std::to_string(RadToDegree(az_radian)) + " " + std::to_string(RadToDegree(alt_radian)), LogLevel::INFO, DeviceType::MAIN);
  Logger::Log("tools.cpp|ra_dec_to_alt_az|az alt (dms):" + radToDmsStr(az_radian) + " " + radToDmsStr(alt_radian), LogLevel::INFO, DeviceType::MAIN);
#endif
}

// 2023.12.21 CJQ
void Tools::full_ra_dec_to_alt_az(QDateTime datetimeUTC, double ra_radian,
                                  double dec_radian, double latitude_radian,
                                  double longitude_radian, double& alt_radian,
                                  double& az_radian) {
  double LST_Degree = getLST_Degree(datetimeUTC, longitude_radian);
  double HA_Degree = getHA_Degree(ra_radian, LST_Degree);
  ra_dec_to_alt_az(DegreeToRad(HA_Degree), dec_radian, alt_radian, az_radian,
                   latitude_radian);
}

void Tools::alt_az_to_ra_dec(double alt_radian, double az_radian,
                             double& hr_radian, double& dec_radian,
                             double lat_radian) {
  double temp;
  double sin_dec;
  double cos_lat = cos(lat_radian);

  if (alt_radian > M_PI / 2.) {
    alt_radian = M_PI - alt_radian;
    az_radian += M_PI;
  }
  if (alt_radian < -M_PI / 2.) {
    alt_radian = -M_PI - alt_radian;
    az_radian -= M_PI;
  }
  sin_dec = sin(lat_radian) * sin(alt_radian) +
            cos_lat * cos(alt_radian) * cos(az_radian);
  // qDebug()<<"sin_dec"<<sin_dec;
  dec_radian = asin(sin_dec);
  if (cos_lat < .00001) /* polar case */
    hr_radian = az_radian + M_PI;
  else {
    temp = cos_lat * cos(dec_radian);
    // qDebug()<<"temp"<<temp;
    temp = (sin(alt_radian) - sin(lat_radian) * sin_dec) / temp;
    // qDebug()<<"temp"<<temp;

    temp = -temp;  // to resolve the return value = nan when alt=90degree or
                   // -90degree. , before it is temp= acos(-temp);
    temp = acos(
        fminl(fmaxl(temp, -1.0),
              1.0));  // to resolve the return value = nan when alt=90degree or
                      // -90degree. , before it is temp= acos(-temp);

    // qDebug()<<"acos(-1) temp"<<temp;
    if (sin(az_radian) > 0.)
      hr_radian = M_PI + temp;  // change - to + from original code
    else
      hr_radian = M_PI - temp;  // change + to - from original code
  }
}

// 2023.12.21 CJQ
void Tools::full_alt_az_to_ra_dec(QDateTime datetimeUTC, double alt_radian,
                                  double az_radian, double latitude_radian,
                                  double longitude_radian, double& ra_radian,
                                  double& dec_radian) {
  double ha_radian = 0;

  alt_az_to_ra_dec(alt_radian, az_radian, ha_radian, dec_radian,
                   latitude_radian);

  double LST_Degree = getLST_Degree(datetimeUTC, longitude_radian);

  double RA_Degree;
  RA_Degree = LST_Degree - RadToDegree(ha_radian);

  RA_Degree = rangeTo(RA_Degree, 360.0, 0.0);

  ra_radian = DegreeToRad(RA_Degree);
}

// 2023.12.21 CJQ
// void Tools::getCurrentMeridianRADEC(double Latitude_radian,
//                                     double Longitude_radian, double& RA_radian,
//                                     double& DEC_radian) {
//   QDateTime datetimeUTC;
//   datetimeUTC = getSystemTimeUTC();
//   full_alt_az_to_ra_dec(datetimeUTC, DegreeToRad(90.0), 0, Latitude_radian,
//                         Longitude_radian, RA_radian, DEC_radian);
// }

// 2023.12.21 CJQ
// double Tools::getCurrentMeridanRA(double Latitude_radian,
//                                   double Longitude_radian) {
//   double RA_radian = 0;
//   double DEC_radian = 0;

//   QDateTime datetimeUTC;
//   datetimeUTC = getSystemTimeUTC();
//   full_alt_az_to_ra_dec(datetimeUTC, DegreeToRad(90.0), 0, Latitude_radian,
//                         Longitude_radian, RA_radian, DEC_radian);

//   return RA_radian;
// }

bool Tools::periodBelongs(double value, double min, double max, double period,
                          bool minequ, bool maxequ) {
  // this API is to calculate if a value belong to a perioc range.  eg:  min <
  // value < max.  maxequ and minque is if there is = . eg. maxequ = true.  it
  // is   min < value <= max

  int n;
  n = (int)((value - max) / period);
  // qDebug()<<"periodBelongs|value="<<value;
  // qDebug()<<"periodBelongs|n=    "<<n;

  double max1, min1;
  double max2, min2;
  double max3, min3;

  max1 = max + (n - 1) * period;
  min1 = min + (n - 1) * period;

  max2 = max + n * period;
  min2 = min + n * period;

  max3 = max + (n + 1) * period;
  min3 = min + (n + 1) * period;

  // qDebug()<<"periodBelongs|range n-1"<< min1 << max1;
  // qDebug()<<"periodBelongs|range n  "<< min2 << max2;
  // qDebug()<<"periodBelongs|range n+1"<< min3 << max3;

  if ((maxequ == true) && (minequ == true)) {
    if (((value <= max1) && (value >= min1)) ||
        ((value <= max2) && (value >= min2)) ||
        ((value <= max3) && (value >= min3)))
      return true;
  } else if ((maxequ == true && minequ == false)) {
    if (((value <= max1) && (value > min1)) ||
        ((value <= max2) && (value > min2)) ||
        ((value <= max3) && (value > min3)))
      return true;
  } else if ((maxequ == false && minequ == false)) {
    if (((value < max1) && (value >= min1)) ||
        ((value < max2) && (value >= min2)) ||
        ((value < max3) && (value >= min3)))
      return true;
  } else {
    if (((value < max1) && (value > min1)) ||
        ((value < max2) && (value > min2)) ||
        ((value < max3) && (value > min3)))
      return true;
  }

  return false;
}

double Tools::DegreeToRad(double degree) { return M_PI * (degree / 180.0); }

double Tools::RadToDegree(double rad) { return rad * 180.0 / M_PI; }

double Tools::HourToDegree(double hour) {
  double degree;
  degree = hour * 15.0;
  return rangeTo(degree, 360.0, 0.0);
}

double Tools::HourToRad(double hour) {
  double degree;
  degree = hour * 15;
  degree = rangeTo(degree, 360.0, 0.0);
  return DegreeToRad(degree);
}

double Tools::DegreeToHour(double degree) {
  double hour;
  hour = degree / 15;
  hour = rangeTo(hour, 24.0, 0.0);
  return hour;
}

double Tools::RadToHour(double rad) {
  double degree;
  degree = RadToDegree(rad);
  degree = rangeTo(degree, 360.0, 0.0);
  return DegreeToHour(degree);
}


QString Tools::getInfoTextA(QDateTime T_local, double RA_DEGREE,
                            double DEC_DEGREE, double dRA_degree,
                            double dDEC_degree, QString MOUNTSTATU,
                            QString GUIDESTATU) {
  QList<int> start = {0,  16, 23, 50,
                      65, 75, 90, 103};  // start position of each data

  QVector<QString> strs = {"Time",    "RA/DEC", "   ", "Tracking",
                           "Guiding", "RMS",    "0.3", "0.2"};

  strs[0] = T_local.toString("HH:mm:ss") + "(Local)";
  strs[1] = "RA/DEC";

  QString temp;
// 2023.12.21 CJQ
//   temp = StelUtils::radToHmsStr(DegreeToRad(RA_DEGREE), true) + " " +
//          StelUtils::radToDmsStr(DegreeToRad(DEC_DEGREE), true, true);
  temp = temp.mid(3, temp.length() - 3);

  strs[2] = temp;
  strs[3] = MOUNTSTATU;
  strs[4] = GUIDESTATU;
  strs[5] =
      "RMS " + QString::number(dRA_degree) + "/" + QString::number(dDEC_degree);

  QString str_total;

  for (int i = 0; i < start.size(); ++i) {
    str_total.insert(start[i], strs[i]);
  }
  return str_total;
}

QString Tools::getInfoTextB(QDateTime T_utc, double AZ_rad, double ALT_rad,
                            QString CAMSTATU, double CAMTemp,
                            double CAMTargetTemp, int CAMX, int CAMY,
                            int CFWPOS, QString CFWNAME, QString CFWSTATU) {
  QList<int> start = {0,  16, 24, 50,
                      65, 75, 90, 103};  // start position of each data

  QVector<QString> strs = {"Time",          "AZ/ALT",     "   ",
                           "3000*2000",     "Exposuring", "-20.1/-20",
                           "Filter #3(Ha)", "Moving"};

  strs[0] = T_utc.toString("HH:mm:ss") + "(UTC)";
  strs[1] = "AZ/ALT";
  // 2023.12.21 CJQ
//   strs[2] = StelUtils::radToDmsStr(AZ_rad) + " " + StelUtils::radToDmsStr(ALT_rad);
  strs[3] = CAMSTATU;
  strs[4] = QString::number(CAMTemp) + "/" + QString::number(CAMTargetTemp);
  strs[5] = QString::number(CAMX) + "*" + QString::number(CAMY);
  strs[6] = "CFW " + CFWSTATU;
  strs[7] = "#" + QString::number(CFWPOS) + " " + CFWNAME;

  QString str_total;

  for (int i = 0; i < start.size(); ++i) {
    str_total.insert(start[i], strs[i]);
  }
  return str_total;
}

QString Tools::getInfoTextC(int CPUTEMP, int CPULOAD, double DISKFREE,
                            double longitude_rad, double latitude_rad,
                            double Ra_J2000, double Dec_J2000, double Az,
                            double Alt, QString ObjName_) {
  QList<int> start = {0,  16,  23,  50,
                      65, 120, 121, 122};  // start position of each data

  QVector<QString> strs = {"CPU 73C",  "Load 78%", "DiskFree 8000MB",
                           "Site Lon", "Long",     "a",
                           "b",        "c"};

  strs[0] = "CPU " + QString::number(CPUTEMP) + "C" + " " +
            QString::number(CPULOAD) + "%";
  strs[1] = "Site";
  // 2023.12.21 CJQ
//   strs[2] = StelUtils::radToDmsStr(longitude_rad) + " " +
//             StelUtils::radToDmsStr(latitude_rad);
  strs[3] = "Free " + QString::number(DISKFREE) + "G";
  // strs[4]="Info: DSN2 is coming";
  // 2023.12.21 CJQ
//   strs[4] = "Info: " + ObjName_ + StelUtils::radToHmsStr(Ra_J2000) + " " +
//             StelUtils::radToDmsStr(Dec_J2000) + " " +
//             StelUtils::radToDmsStr(3.1415926 - Az) + " " +
//             StelUtils::radToDmsStr(Alt);

  QString str_total;

  for (int i = 0; i < start.size(); ++i) {
    str_total.insert(start[i], strs[i]);
  }
  return str_total;
}

//-----------------------Stellarium Control add-on
// APIs----------------------------

// 2023.12.21 CJQ
// void Tools::MRGOTO_RADEC_rad(StelMovementMgr* mvmgr, double RA, double DEC) {
//   Vec3d pos;
//   Vec3d aimUp;

//   StelUtils::spheToRect(RA, DEC, pos);
//   // if ( (mountMode==StelMovementMgr::MountEquinoxEquatorial) &&
//   // (fabs(spinLat)> (0.9*M_PI_2)) )
//   //{
//   //  make up vector more stable.
//   //  Strictly mount should be in a new J2000 mode, but this here also
//   //  stabilizes searching J2000 coordinates.
//   // setViewDirectionJ2000(Vec3d(-cos(RA), -sin(RA), 0.) * (DEC>0. ? 1. : -1.
//   // )); aimUp=mvmgr->getViewUpVectorJ2000();
//   //}
//   // break;

//   StelMovementMgr::MountMode mountMode = mvmgr->getMountMode();
//   if ((mountMode == StelMovementMgr::MountEquinoxEquatorial) &&
//       (fabs(DEC) > (0.9 * M_PI / 2.0)))
//     aimUp = Vec3d(-cos(RA), -sin(RA), 0.) * (DEC > 0. ? 1. : -1.);
//   else
//     aimUp = Vec3d(0., 0., 1.);

//   mvmgr->setFlagTracking(false);
//   mvmgr->setViewDirectionJ2000(pos);

//   // moveToJ2000(pos, aimUp, getAutoMoveDuration());
//   // setFlagLockEquPos(true);
// }

// 2023.12.21 CJQ
// void Tools::MRGOTO_AZALT_rad(StelMovementMgr* mvmgr, double AZ, double ALT) {
//   Vec3d tmp;
//   AZ = -AZ + M_PI;  // correct it.  0=North.  180=south
//   StelUtils::spheToRect(AZ, ALT, tmp);

//   // Call inside mvmgr's thread in case of data race
//   QMetaObject::invokeMethod(mvmgr, [mvmgr, tmp]() {
//     mvmgr->setViewDirectionJ2000(mvmgr->mountFrameToJ2000(tmp));
//   });
// }


CartesianCoordinates Tools::convertEquatorialToCartesian(double ra, double dec, double radius) {
    // 将赤经和赤纬转换为弧度
    double raRad = (ra / 180.0) * M_PI;
    double decRad = (dec / 180.0) * M_PI;

    // 计算笛卡尔坐标系中的三维坐标
    double x = radius * cos(decRad) * cos(raRad);
    double y = radius * cos(decRad) * sin(raRad);
    double z = radius * sin(decRad);

    return {x, y, z};
}

CartesianCoordinates Tools::calculateVector(CartesianCoordinates pointA, CartesianCoordinates pointB) {
    CartesianCoordinates vector;
    vector.x = pointB.x - pointA.x;
    vector.y = pointB.y - pointA.y;
    vector.z = pointB.z - pointA.z;
    return vector;
}

CartesianCoordinates Tools::calculatePointC(CartesianCoordinates pointA, CartesianCoordinates vectorV) {
    CartesianCoordinates pointC;
    pointC.x = pointA.x + vectorV.x;
    pointC.y = pointA.y + vectorV.y;
    pointC.z = pointA.z + vectorV.z;
    return pointC;
}

SphericalCoordinates Tools::convertToSphericalCoordinates(CartesianCoordinates cartesianPoint) {
    double x = cartesianPoint.x;
    double y = cartesianPoint.y;
    double z = cartesianPoint.z;

    double radius = sqrt(x * x + y * y + z * z);
    double declination = asin(z / radius) * (180.0 / M_PI);
    double rightAscension = atan2(y, x) * (180.0 / M_PI);

    // 确保 rightAscension 处于 [0, 360] 范围内
    if (rightAscension < 0) {
        rightAscension += 360;
    }

    return {rightAscension, declination};
}

// MinMaxFOV Tools::calculateFOV(int FocalLength,double CameraSize_width,double CameraSize_height)
// {
//   MinMaxFOV result;
//   Logger::Log("FocalLength: " + std::to_string(FocalLength) + ", " + "CameraSize: " + std::to_string(CameraSize_width) + ", " + std::to_string(CameraSize_height), LogLevel::INFO, DeviceType::MAIN);

//   double CameraSize_diagonal = sqrt(pow(CameraSize_width, 2) + pow(CameraSize_height, 2));
//   // qDebug() << CameraSize_diagonal;

//   double minFOV,maxFOV;

//   minFOV = 2 * atan(CameraSize_height / (2 * FocalLength)) * 180 / M_PI;
//   maxFOV = 2 * atan(CameraSize_diagonal / (2 * FocalLength)) * 180 / M_PI;

//   result.minFOV = minFOV;
//   result.maxFOV = maxFOV;
  
//   Logger::Log("minFov: " + std::to_string(result.minFOV) + ", " + "maxFov: " + std::to_string(result.maxFOV), LogLevel::INFO, DeviceType::MAIN);

//   return result;
// }

MinMaxFOV Tools::calculateFOV(int FocalLength, double CameraSize_width, double CameraSize_height)
{
    MinMaxFOV result;
    Logger::Log("FocalLength: " + std::to_string(FocalLength) +
                ", CameraSize: " + std::to_string(CameraSize_width) +
                " x " + std::to_string(CameraSize_height),
                LogLevel::INFO, DeviceType::MAIN);

    // 对角线
    double CameraSize_diagonal = std::hypot(CameraSize_width, CameraSize_height);

    // 取宽高里的较小者
    double min_side = std::min(CameraSize_width, CameraSize_height);

    double minFOV = 2.0 * atan(min_side / (2.0 * FocalLength)) * 180.0 / M_PI;
    double maxFOV = 2.0 * atan(CameraSize_diagonal / (2.0 * FocalLength)) * 180.0 / M_PI;

    result.minFOV = minFOV;   // 最小视场（短边方向）
    result.maxFOV = maxFOV;   // 最大视场（对角线方向）

    Logger::Log("minFOV: " + std::to_string(result.minFOV) +
                ", maxFOV: " + std::to_string(result.maxFOV),
                LogLevel::INFO, DeviceType::MAIN);

    return result;
}


// 计算格林尼治恒星时 (GST)
double Tools::calculateGST(const std::tm& date) {
    std::tm epoch = {0, 0, 12, 1, 0, 100}; // Jan 1, 2000 12:00:00 UTC
    std::time_t epoch_time = std::mktime(&epoch);
    std::time_t now_time = std::mktime(const_cast<std::tm*>(&date));
    double JD = 2451545.0 + (now_time - epoch_time) / 86400.0;
    double T = (JD - 2451545.0) / 36525.0;
    double GST = 280.46061837 + 360.98564736629 * (JD - 2451545.0) + 0.000387933 * T * T - (T * T * T / 38710000.0);
    return fmod(GST, 360.0);
}

AltAz Tools::calculateAltAz(double ra, double dec, double lat, double lon, const std::tm& date) {
    // 将输入值转换为弧度
    double ra_rad = Tools::DegreeToRad(ra * 15.0); // 赤经转换为弧度并乘以15
    double dec_rad = Tools::DegreeToRad(dec);
    double lat_rad = Tools::DegreeToRad(lat);
    double lon_rad = Tools::DegreeToRad(lon);

    // 计算GST和LST
    double GST = Tools::calculateGST(date);
    double LST = fmod(GST + lon, 360.0); // LST在0-360度范围内
    double HA = Tools::DegreeToRad(LST) - ra_rad; // 时角

    // 计算高度角
    double alt_rad = asin(sin(dec_rad) * sin(lat_rad) + cos(dec_rad) * cos(lat_rad) * cos(HA));
    double alt_deg = Tools::RadToDegree(alt_rad);

    // 计算方位角
    double cosAz = (sin(dec_rad) - sin(alt_rad) * sin(lat_rad)) / (cos(alt_rad) * cos(lat_rad));
    double az_rad = acos(cosAz);
    double az_deg = Tools::RadToDegree(az_rad);

    // 调整方位角
    if (sin(HA) > 0) {
        az_deg = 360.0 - az_deg;
    }

    return { alt_deg, az_deg };
}

void Tools::printDMS(double angle) {
    int degrees = static_cast<int>(angle);
    double fractional = angle - degrees;
    int minutes = static_cast<int>(fractional * 60);
    double seconds = (fractional * 60 - minutes) * 60;

    std::cout << degrees << "° " << minutes << "' " << std::fixed << std::setprecision(2) << seconds << "\"";
}

double Tools::DMSToDegree(int degrees, int minutes, double seconds) {
    // 确定符号
    double sign = degrees < 0 ? -1.0 : 1.0;
    // 计算绝对值
    double absDegrees = std::abs(degrees) + minutes / 60.0 + seconds / 3600.0;
    return sign * absDegrees;
}

bool Tools::WaitForPlateSolveToComplete() {
  // qDebug() << "Wait For Plate Solve To Complete.";
  Logger::Log("Wait For Plate Solve(" + std::to_string(!PlateSolveInProgress) + ") To Complete...", LogLevel::INFO, DeviceType::MAIN);
  return !PlateSolveInProgress;
}

bool Tools::isSolveImageFinish() {
  return isSolveImageFinished;
}

bool Tools::PlateSolve(QString filename, int FocalLength, double CameraSize_width, double CameraSize_height, bool USEQHYCCDSDK)
{
    filename = "/home/quarcs/workspace/testimage/0.fits";
    PlateSolveInProgress = true;
    isSolveImageFinished = false;

    MinMaxFOV FOV = calculateFOV(FocalLength, CameraSize_width, CameraSize_height);

    QString MinFOV = QString::number(FOV.minFOV);
    QString MaxFOV = QString::number(FOV.maxFOV);

    QProcess* cmd_test = new QProcess();
    QObject::connect(cmd_test, SIGNAL(finished(int)), instance_, SLOT(onSolveFinished(int)));

    // 连接输出和错误信号以实时处理输出
    // 移除static关键字，每次调用都使用新的变量
    QString lastOutput; // 上一次的输出内容
    int repeatCount = 0; // 重复次数

    QObject::connect(cmd_test, &QProcess::readyReadStandardOutput, [cmd_test, &lastOutput, &repeatCount]() {
        QString buffer;  // 移除static，每次都是新的缓冲区
        buffer += cmd_test->readAllStandardOutput();  // 累加新的输出到缓冲区

        int newlineIndex;
        while ((newlineIndex = buffer.indexOf('\n')) != -1) {  // 检查是否有新行
            QString output = buffer.left(newlineIndex).trimmed();  // 获取一行输出
            buffer.remove(0, newlineIndex + 1);  // 从缓冲区移除这一行

            // 下面是原有的处理逻辑
            if (output == lastOutput) {
                repeatCount++;
                if (repeatCount >= 10) {
                    Logger::Log("当前解析进度:" + output.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                    emit instance_->parseInfoEmitted(output);
                    repeatCount = 0;
                }
            } else {
                lastOutput = output;
                repeatCount = 1;
                Logger::Log("当前解析进度:" + output.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                emit instance_->parseInfoEmitted(output);
            }
        }
    });

    QString command_qstr;
    if (!USEQHYCCDSDK)
    {
      // command_qstr="solve-field " + filename + " --overwrite --cpulimit 5 --scale-units degwidth --scale-low " + MinFOV + " --scale-high " + MaxFOV + " --nsigma 8  --no-plots  --no-remove-lines --uniformize 0 --timestamp";
      // command_qstr = "solve-field " + filename + " --overwrite --cpulimit 20 --scale-units degwidth --nsigma 10  --no-plots  --no-remove-lines --uniformize 0 --timestamp";
      command_qstr = "solve-field " + filename + " --overwrite  --no-plots --uniformize 0 --timestamp  --objs 10 --pixel-error 1.5 --cpulimit 20 --scale-low " + MinFOV + " --scale-high " + MaxFOV;
    }
    else
    {
        filename = "/dev/shm/SDK_Capture";
        // Adjust command if needed
    }
    
    Logger::Log("当前解析命令:" + command_qstr.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    cmd_test->start(command_qstr);
    
    if (!cmd_test->waitForStarted()) {
        Logger::Log("解析命令启动失败", LogLevel::ERROR, DeviceType::MAIN);
        PlateSolveInProgress = false;
        isSolveImageFinished = false;
        return false;
    }
    
    // 设置更长的超时时间，适应树莓派的性能
    if (!cmd_test->waitForFinished(300000)) { // 5分钟超时
        Logger::Log("解析命令执行超时，强制终止", LogLevel::WARNING, DeviceType::MAIN);
        cmd_test->kill(); // 强制终止进程
        PlateSolveInProgress = false;
        isSolveImageFinished = false;
        return false;
    }
    
    // 检查进程退出状态
    int exitCode = cmd_test->exitCode();
    if (exitCode != 0) {
        Logger::Log("解析命令执行失败，退出码: " + std::to_string(exitCode), LogLevel::ERROR, DeviceType::MAIN);
        PlateSolveInProgress = false;
        isSolveImageFinished = false;
        return false;
    }

    QApplication::processEvents();

    return true;
}

SloveResults Tools::ReadSolveResult(QString filename, int imageWidth, int imageHeight) {
  isSolveImageFinished = false;
  sleep(1);

  SloveResults result;
  filename = filename.chopped(5);  // 移除文件名的最后五个字符

  QProcess* cmd_test = new QProcess();
  // cmd_test->start("wcsinfo /dev/shm/Capture_00003_bin.wcs");  // 启动外部程序来读取WCS信息
  cmd_test->start("wcsinfo " + filename + ".wcs");
  cmd_test->waitForFinished();  // 等待外部程序执行完成

  QString str = cmd_test->readAllStandardOutput();  // 读取程序输出的结果

  if (str.isEmpty()) {
    Logger::Log("wcsinfo 输出为空，解析失败", LogLevel::ERROR, DeviceType::MAIN);
    result.RA_Degree = -1;
    result.DEC_Degree = -1;
    PlateSolveInProgress = false;
    return result;
  }

  Logger::Log("wcsinfo: " + str.toStdString(), LogLevel::INFO, DeviceType::MAIN);

  // 查找关键信息的位置
  int pos1 = str.indexOf("ra_center");
  int pos2 = str.indexOf("dec_center");
  int pos3 = str.indexOf("orientation_center");
  int pos4 = str.indexOf("ra_center_h");
  if (pos1 == -1 || pos2 == -1 || pos3 == -1 || pos4 == -1) {
    Logger::Log("无法在wcsinfo输出中找到必要的关键字", LogLevel::ERROR, DeviceType::MAIN);
    result.RA_Degree = -1;
    result.DEC_Degree = -1;
    PlateSolveInProgress = false;
    return result;
  }
  Logger::Log("pos 1 2 3 4: " + std::to_string(pos1) + " " + std::to_string(pos2) + " " + std::to_string(pos3) + " " + std::to_string(pos4), LogLevel::INFO, DeviceType::MAIN);

  // 提取并转换坐标信息
  QString str_RA_Degree = str.mid(pos1 + 10, pos2 - pos1 - 10 - 1);
  QString str_DEC_Degree = str.mid(pos2 + 11, pos3 - pos2 - 11 - 1);
  QString str_Rotation = str.mid(pos3 + 19, pos4 - pos3 - 19 - 1);

  double RA_Degree = str_RA_Degree.toDouble();
  double DEC_Degree = str_DEC_Degree.toDouble();
  double Rotation_Degree = str_Rotation.toDouble();

  // 提取WCS参数并计算视场角的四个角的坐标
  // WCSParams wcs = extractWCSParams(str);
  // std::vector<SphericalCoordinates> corners = getFOVCorners(wcs, imageWidth, imageHeight);
  // Logger::Log("FOV Corners (Ra, Dec):", LogLevel::INFO, DeviceType::MAIN);
  // for (const auto &corner : corners) {
  //   Logger::Log("Ra: " + std::to_string(corner.ra) + ", Dec: " + std::to_string(corner.dec), LogLevel::INFO, DeviceType::MAIN);
  // }
  FieldOfView fov = extractFieldOfViewFromWcsInfo(str);

  // 存储结果
  result.RA_0 = fov.ra_min;
  result.DEC_0 = fov.dec_min;
  result.RA_1 = fov.ra_max;
  result.DEC_1 = fov.dec_min;
  result.RA_2 = fov.ra_max;
  result.DEC_2 = fov.dec_max;
  result.RA_3 = fov.ra_min;
  result.DEC_3 = fov.dec_max;

  Logger::Log("RA DEC Rotation(degree) " + std::to_string(RA_Degree) + " " + std::to_string(DEC_Degree) + " " + std::to_string(Rotation_Degree), LogLevel::INFO, DeviceType::MAIN);
  Logger::Log("RA DEC " + QString::number(RA_Degree, 'g', 9).toStdString() + " " + QString::number(DEC_Degree, 'g', 9).toStdString(), LogLevel::INFO, DeviceType::MAIN);
  result.RA_Degree = RA_Degree;
  result.DEC_Degree = DEC_Degree;
  PlateSolveInProgress = false;
  return result;
}
SloveResults Tools::onSolveFinished(int exitCode) {
  Logger::Log("Solve Finished!!! 退出码: " + std::to_string(exitCode), LogLevel::INFO, DeviceType::MAIN);
  
  if (exitCode == 0) {
    isSolveImageFinished = true;
    PlateSolveInProgress = false;
    Logger::Log("解析成功完成", LogLevel::INFO, DeviceType::MAIN);
  } else {
    Logger::Log("解析失败，退出码: " + std::to_string(exitCode), LogLevel::ERROR, DeviceType::MAIN);
    isSolveImageFinished = false;
    PlateSolveInProgress = false;
  }
}

WCSParams Tools::extractWCSParams(const QString& wcsInfo) {
  WCSParams wcs;
    
    int pos1 = wcsInfo.indexOf("crpix0");
    int pos2 = wcsInfo.indexOf("crpix1");
    int pos3 = wcsInfo.indexOf("crval0");
    int pos4 = wcsInfo.indexOf("crval1");
    int pos5 = wcsInfo.indexOf("cd11");
    int pos6 = wcsInfo.indexOf("cd12");
    int pos7 = wcsInfo.indexOf("cd21");
    int pos8 = wcsInfo.indexOf("cd22");

    wcs.crpix0 = wcsInfo.mid(pos1 + 7, wcsInfo.indexOf("\n", pos1) - pos1 - 7).toDouble();
    wcs.crpix1 = wcsInfo.mid(pos2 + 7, wcsInfo.indexOf("\n", pos2) - pos2 - 7).toDouble();
    wcs.crval0 = wcsInfo.mid(pos3 + 7, wcsInfo.indexOf("\n", pos3) - pos3 - 7).toDouble();
    wcs.crval1 = wcsInfo.mid(pos4 + 7, wcsInfo.indexOf("\n", pos4) - pos4 - 7).toDouble();
    wcs.cd11 = wcsInfo.mid(pos5 + 5, wcsInfo.indexOf("\n", pos5) - pos5 - 5).toDouble();
    wcs.cd12 = wcsInfo.mid(pos6 + 5, wcsInfo.indexOf("\n", pos6) - pos6 - 5).toDouble();
    wcs.cd21 = wcsInfo.mid(pos7 + 5, wcsInfo.indexOf("\n", pos7) - pos7 - 5).toDouble();
    wcs.cd22 = wcsInfo.mid(pos8 + 5, wcsInfo.indexOf("\n", pos8) - pos8 - 5).toDouble();

    Logger::Log("crpix0: " + QString::number(wcs.crpix0, 'g', 9).toStdString(), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("crpix1: " + QString::number(wcs.crpix1, 'g', 9).toStdString(), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("crval0: " + QString::number(wcs.crval0, 'g', 9).toStdString(), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("crval1: " + QString::number(wcs.crval1, 'g', 9).toStdString(), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("cd11: " + QString::number(wcs.cd11, 'g', 9).toStdString(), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("cd12: " + QString::number(wcs.cd12, 'g', 9).toStdString(), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("cd21: " + QString::number(wcs.cd21, 'g', 9).toStdString(), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("cd22: " + QString::number(wcs.cd22, 'g', 9).toStdString(), LogLevel::INFO, DeviceType::MAIN);
    
    return wcs;
}

// 从wcsinfo输出中提取视场信息到FieldOfView结构体
FieldOfView Tools::extractFieldOfViewFromWcsInfo(const QString& wcsInfo) {
    FieldOfView fov;
    
    // 辅助函数：提取一个参数的值
    auto extractValue = [&wcsInfo](const QString& key) -> double {
        int pos = wcsInfo.indexOf(key);
        if (pos != -1) {
            int endOfLine = wcsInfo.indexOf("\n", pos);
            if (endOfLine == -1) endOfLine = wcsInfo.length();
            
            // 提取从key后到行尾的内容
            QString valueStr = wcsInfo.mid(pos + key.length(), endOfLine - pos - key.length()).trimmed();
            // 取第一个非空白部分作为数值
            QStringList parts = valueStr.split(" ", Qt::SkipEmptyParts);
            if (!parts.isEmpty()) {
                return parts[0].toDouble();
            }
        }
        return 0.0;
    };
    
    // 提取直接给出的视场参数
    fov.width = extractValue("fieldw");
    fov.height = extractValue("fieldh");
    fov.ra_min = extractValue("ramin");
    fov.ra_max = extractValue("ramax");
    fov.dec_min = extractValue("decmin");
    fov.dec_max = extractValue("decmax");
    fov.area = extractValue("fieldarea");
    fov.ra_center = extractValue("ra_center");
    fov.dec_center = extractValue("dec_center");
    fov.orientation = extractValue("orientation_center");
    
    // 计算视场大小（用于验证）
    double pixelScale = extractValue("pixscale"); // 角秒/像素
    int imageWidth = extractValue("imagew");
    int imageHeight = extractValue("imageh");
    
    fov.calculatedWidth = (imageWidth * pixelScale) / 3600.0; // 转换为度
    fov.calculatedHeight = (imageHeight * pixelScale) / 3600.0;
    
    // 记录提取的视场信息
    Logger::Log("提取的视场信息:", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("中心位置: RA=" + QString::number(fov.ra_center, 'g', 9).toStdString() + 
              ", Dec=" + QString::number(fov.dec_center, 'g', 9).toStdString(), 
              LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("视场大小: " + QString::number(fov.width, 'g', 6).toStdString() + 
              "° × " + QString::number(fov.height, 'g', 6).toStdString() + "°", 
              LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("计算大小: " + QString::number(fov.calculatedWidth, 'g', 6).toStdString() + 
              "° × " + QString::number(fov.calculatedHeight, 'g', 6).toStdString() + "°", 
              LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("视场范围: RA " + QString::number(fov.ra_min, 'g', 6).toStdString() + 
              "° 到 " + QString::number(fov.ra_max, 'g', 6).toStdString() + 
              "°, Dec " + QString::number(fov.dec_min, 'g', 6).toStdString() + 
              "° 到 " + QString::number(fov.dec_max, 'g', 6).toStdString() + "°", 
              LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("视场面积: " + QString::number(fov.area, 'g', 6).toStdString() + 
              " 平方度", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("方向角: " + QString::number(fov.orientation, 'g', 6).toStdString() + 
              "°", LogLevel::INFO, DeviceType::MAIN);
    
    return fov;
}

// 函数：从像素坐标转换为RaDec
SphericalCoordinates Tools::pixelToRaDec(double x, double y, const WCSParams& wcs) {
    double dx = x - wcs.crpix0;
    double dy = y - wcs.crpix1;

    double ra = wcs.crval0 + wcs.cd11 * dx + wcs.cd12 * dy;
    double dec = wcs.crval1 + wcs.cd21 * dx + wcs.cd22 * dy;

    return {ra, dec};
}



// 函数：从WCS参数和图像尺寸计算四个角的RaDec值
std::vector<SphericalCoordinates> Tools::getFOVCorners(const WCSParams& wcs, int imageWidth, int imageHeight) {
    std::vector<SphericalCoordinates> corners(4);
    
    // 增加调试输出
    Logger::Log("WCS参数: crpix0=" + std::to_string(wcs.crpix0) + 
                " crpix1=" + std::to_string(wcs.crpix1) +
                " 变换矩阵: " + std::to_string(wcs.cd11) + "," + 
                std::to_string(wcs.cd12) + "," + std::to_string(wcs.cd21) + 
                "," + std::to_string(wcs.cd22), LogLevel::INFO, DeviceType::MAIN);
    
    corners[0] = pixelToRaDec(0, 0, wcs);                  // Bottom-left
    corners[1] = pixelToRaDec(imageWidth, 0, wcs);         // Bottom-right
    corners[2] = pixelToRaDec(imageWidth, imageHeight, wcs); // Top-right
    corners[3] = pixelToRaDec(0, imageHeight, wcs);        // Top-left
    
    // 检查计算出的坐标是否相似
    bool tooSimilar = true;
    for (int i = 1; i < 4; i++) {
        if (std::abs(corners[i].ra - corners[0].ra) > 0.0001 ||
            std::abs(corners[i].dec - corners[0].dec) > 0.0001) {
            tooSimilar = false;
            break;
        }
    }
    
    if (tooSimilar) {
        Logger::Log("警告：计算的四个角点坐标几乎相同，WCS参数可能有问题", LogLevel::WARNING, DeviceType::MAIN);
    }
    
    return corners;
}

int Tools::fitQuadraticCurve(const QVector<QPointF>& data, float& a, float& b, float& c) {
    int n = data.size();
    if (n < 5) {
        return -1; // 数据点数量不足
    }
    cv::Mat A(n, 3, CV_32F);
    cv::Mat B(n, 1, CV_32F);

    for (int i = 0; i < n; i++) {
        float x = data[i].x();
        float y = data[i].y();
        A.at<float>(i, 0) = x * x;
        A.at<float>(i, 1) = x;
        A.at<float>(i, 2) = 1;
        B.at<float>(i, 0) = y;
    }

    cv::Mat X;
    cv::solve(A, B, X, cv::DECOMP_QR);

    a = X.at<float>(0, 0);
    b = X.at<float>(1, 0);
    c = X.at<float>(2, 0);

    return 0; // 拟合成功
}

double Tools::calculateRSquared(QVector<QPointF> data, float a, float b, float c) {
    double ssTotal = 0.0;
    double ssResidual = 0.0;
    double meanY = 0.0;

    // 计算 y 的平均值
    for (const QPointF &point : data) {
        meanY += point.y();
    }
    meanY /= data.size();

    for (const QPointF &point : data) {
        float x = point.x();
        float y = point.y();
        float yFit = a * x * x + b * x + c;
        ssTotal += (y - meanY) * (y - meanY);
        ssResidual += (y - yFit) * (y - yFit);
    }
    double rSquared = 1 - (ssResidual / ssTotal);

    // rSquaredLabel->setText(QString("R²: %1").arg(rSquared));
    return rSquared;
}


        