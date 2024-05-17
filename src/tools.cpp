#include "tools.hpp"
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

Tools* Tools::instance_ = nullptr;

int glChannelCount;

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
      qDebug() << "loadSystemListFromXml | xmlRead has ERROR";
      return false;
    }

  } else {
    qDebug() << "loadSystemListFromXml | ERROR: Can not open file";
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

bool Tools::GetIndexFromSystemDeviceList(const QString& devname, int& index) {
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
    qDebug() << "getIndexFromSystemDeviceList | found device in system list. "
                "device name"
             << devname << "index" << index;
    return true;
  } else {
    index = 0;
    qDebug() << "getIndexFromSystemDeviceList | not found device in system "
                "list, devname"
             << devname;
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
  // qDebug()<<"clearSystemDeviceListItem";
}

void Tools::readDriversListFromFiles(const std::string &filename, DriversList &drivers_list_from,
                              std::vector<DevGroup> &dev_groups_from, std::vector<Device> &devices_from)
{
    QFile file(QString::fromStdString(filename));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        // 处理打开文件失败的情况
        qDebug() << "打开文件失败";
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
                    // 处理打开文件失败的情况
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
    qDebug("printDevGroups2===============================");
    // qDebug() << "Number of groups:" << driver_list.dev_groups.size();
    for (int i = 0; i < driver_list.dev_groups.size(); i++)
    {
        // qDebug() << QString::fromStdString(driver_list.dev_groups[i].group);
        qDebug() << driver_list.dev_groups[i].group;
        // qDebug() << "Number of devices:" << driver_list.dev_groups[i].devices.size();
        for (int j = 0; j < driver_list.dev_groups[i].devices.size(); j++)
        {
            qDebug() << QString::fromStdString(driver_list.dev_groups[i].devices[j].driver_name) << QString::fromStdString(driver_list.dev_groups[i].devices[j].version) << QString::fromStdString(driver_list.dev_groups[i].devices[j].label);
        }
    }
}

void Tools::printSystemDeviceList(SystemDeviceList s){
    //starndard sequence
    // s[0]: Mount
    // s[1]: Guider
    // s[2]: Pole Camera (polemaster)
    // s[3]:
    // s[4]:
    // s[5]:
    // s[6]:
    // s[7]:
    // s[8]:
    // s[9]:
    // s[10]:
    // s[11]:
    // s[12]:
    // s[13]:
    // s[14]:
    // s[15]:
    // s[16]:
    // s[17]:
    // s[18]:
    // s[19]:
    // s[20]: Camera #1
    // s[21]: CFW #1
    // s[22]: Focuser #1
    // s[23]: LensCover #1




    qDebug()<<"*****************System Device Selected***************"<<s.system_devices.size();
    QString dpName;
    for (int i=0;i<s.system_devices.size();i++){
          if(s.system_devices[i].dp==NULL) dpName="NULL";
          else                             dpName=s.system_devices[i].dp->getDeviceName();

          qDebug()<< i << s.system_devices[i].DeviceIndiGroup << s.system_devices[i].DriverFrom << s.system_devices[i].DriverIndiName << s.system_devices[i].DeviceIndiName << s.system_devices[i].Description <<s.system_devices[i].isConnect <<dpName;
    }
    qDebug() << "******************************************************";
}

void Tools::makeConfigFolder() {
  std::string directory = "config"; // 要创建的文件夹名

    // 如果目录不存在，则创建
    if (!std::filesystem::exists(directory))
    {
        if (std::filesystem::create_directory(directory))
        {
            std::cout << "文件夹创建成功: " << directory << std::endl;
        }
        else
        {
            std::cerr << "创建文件夹时发生错误" << std::endl;
        }
    }
    else
    {
        std::cout << "文件夹已存在: " << directory << std::endl;
    }
}

void Tools::makeImageFolder() {
    std::string directory = "image"; // 要创建的文件夹名

    // 如果目录不存在，则创建
    if (!std::filesystem::exists(directory))
    {
        if (std::filesystem::create_directory(directory))
        {
            std::cout << "文件夹创建成功: " << directory << std::endl;

            // 创建子文件夹 CaptureImage
            std::string captureDirectory = directory + "/CaptureImage";
            if (std::filesystem::create_directory(captureDirectory))
            {
                std::cout << "子文件夹创建成功: " << captureDirectory << std::endl;
            }
            else
            {
                std::cerr << "创建子文件夹时发生错误" << std::endl;
            }

            // 创建子文件夹 ScheduleImage
            std::string scheduleDirectory = directory + "/ScheduleImage";
            if (std::filesystem::create_directory(scheduleDirectory))
            {
                std::cout << "子文件夹创建成功: " << scheduleDirectory << std::endl;
            }
            else
            {
                std::cerr << "创建子文件夹时发生错误" << std::endl;
            }
        }
        else
        {
            std::cerr << "创建文件夹时发生错误" << std::endl;
        }
    }
    else
    {
        std::cout << "文件夹已存在: " << directory << std::endl;
    }
}

void Tools::saveSystemDeviceList(SystemDeviceList deviceList)
{
  std::string directory = "config"; // 配置文件夹名
  std::string filename = directory + "/device_connect.dat"; // 在配置文件夹中创建文件

  std::ofstream outfile(filename, std::ios::binary);

  if (!outfile.is_open()) {
        std::cerr << "打开文件写入时发生错误: " << filename << std::endl;
        return;
    }

    for (const auto& device : deviceList.system_devices) {
        // 转换 QString 成员为 UTF-8 字符串
        QByteArray descriptionUtf8 = device.Description.toUtf8();
        QByteArray deviceIndiNameUtf8 = device.DeviceIndiName.toUtf8();
        QByteArray driverIndiNameUtf8 = device.DriverIndiName.toUtf8();
        QByteArray driverFromUtf8 = device.DriverFrom.toUtf8();

        // 写入 QString 大小信息和数据
        size_t descriptionSize = static_cast<size_t>(descriptionUtf8.size());
        outfile.write(reinterpret_cast<const char*>(&descriptionSize), sizeof(size_t));
        outfile.write(descriptionUtf8.constData(), descriptionSize);

        outfile.write(reinterpret_cast<const char*>(&device.DeviceIndiGroup), sizeof(int));

        size_t deviceIndiNameSize = static_cast<size_t>(deviceIndiNameUtf8.size());
        outfile.write(reinterpret_cast<const char*>(&deviceIndiNameSize), sizeof(size_t));
        outfile.write(deviceIndiNameUtf8.constData(), deviceIndiNameSize);

        size_t driverIndiNameSize = static_cast<size_t>(driverIndiNameUtf8.size());
        outfile.write(reinterpret_cast<const char*>(&driverIndiNameSize), sizeof(size_t));
        outfile.write(driverIndiNameUtf8.constData(), driverIndiNameSize);

        size_t driverFromSize = static_cast<size_t>(driverFromUtf8.size());
        outfile.write(reinterpret_cast<const char*>(&driverFromSize), sizeof(size_t));
        outfile.write(driverFromUtf8.constData(), driverFromSize);

        outfile.write(reinterpret_cast<const char*>(&device.isConnect), sizeof(bool));
    }

    outfile.close();
}

SystemDeviceList Tools::readSystemDeviceList()
{
  SystemDeviceList deviceList;
  std::string directory = "config"; // 配置文件夹名
  std::string filename = directory + "/device_connect.dat"; // 在配置文件夹中创建文件
  std::ifstream infile(filename, std::ios::binary);

  if (!infile.is_open()) {
        std::cerr << "打开文件读取时发生错误: " << filename << std::endl;
        return deviceList;
    }

    while (true) {
        SystemDevice device;

        // 读取 QString 成员
        size_t descriptionSize;
        infile.read(reinterpret_cast<char*>(&descriptionSize), sizeof(size_t));
        if (infile.eof()) {
            break;
        }
        std::vector<char> descriptionBuffer(descriptionSize);
        infile.read(descriptionBuffer.data(), descriptionSize);
        device.Description = QString::fromUtf8(descriptionBuffer.data(), static_cast<int>(descriptionSize));

        infile.read(reinterpret_cast<char*>(&device.DeviceIndiGroup), sizeof(int));

        size_t deviceIndiNameSize;
        infile.read(reinterpret_cast<char*>(&deviceIndiNameSize), sizeof(size_t));
        std::vector<char> deviceIndiNameBuffer(deviceIndiNameSize);
        infile.read(deviceIndiNameBuffer.data(), deviceIndiNameSize);
        device.DeviceIndiName = QString::fromUtf8(deviceIndiNameBuffer.data(), static_cast<int>(deviceIndiNameSize));

        size_t driverIndiNameSize;
        infile.read(reinterpret_cast<char*>(&driverIndiNameSize), sizeof(size_t));
        std::vector<char> driverIndiNameBuffer(driverIndiNameSize);
        infile.read(driverIndiNameBuffer.data(), driverIndiNameSize);
        device.DriverIndiName = QString::fromUtf8(driverIndiNameBuffer.data(), static_cast<int>(driverIndiNameSize));

        size_t driverFromSize;
        infile.read(reinterpret_cast<char*>(&driverFromSize), sizeof(size_t));
        std::vector<char> driverFromBuffer(driverFromSize);
        infile.read(driverFromBuffer.data(), driverFromSize);
        device.DriverFrom = QString::fromUtf8(driverFromBuffer.data(), static_cast<int>(driverFromSize));

        infile.read(reinterpret_cast<char*>(&device.isConnect), sizeof(bool));

        deviceList.system_devices.push_back(device);
    }

    infile.close();

    return deviceList;
}

void Tools::saveExpTimeList(QString List)
{
  std::string directory = "config";                      // 配置文件夹名
  std::string filename = directory + "/ExpTimeList.dat"; // 在配置文件夹中创建文件

  std::ofstream outfile(filename, std::ios::binary);

  if (!outfile.is_open())
  {
    std::cerr << "打开文件写入时发生错误: " << filename << std::endl;
    return;
  }

  QByteArray ExpTimeListUtf8 = List.toUtf8();

  // 写入 QString 大小信息和数据
  size_t ExpTimeListSize = static_cast<size_t>(ExpTimeListUtf8.size());
  outfile.write(reinterpret_cast<const char *>(&ExpTimeListSize), sizeof(size_t));
  outfile.write(ExpTimeListUtf8.constData(), ExpTimeListSize);

  outfile.close();
}

QString Tools::readExpTimeList()
{
  std::string directory = "config";                      // 配置文件夹名
  std::string filename = directory + "/ExpTimeList.dat"; // 在配置文件夹中创建文件
  std::ifstream infile(filename, std::ios::binary);

  if (!infile.is_open())
  {
    std::cerr << "打开文件读取时发生错误: " << filename << std::endl;
    return QString();
  }

  // 读取经验时间列表的大小
  size_t ExpTimeListSize;
  infile.read(reinterpret_cast<char *>(&ExpTimeListSize), sizeof(size_t));

  // 分配内存空间用于存储经验时间列表的数据
  char *buffer = new char[ExpTimeListSize];

  // 读取经验时间列表的数据
  infile.read(buffer, ExpTimeListSize);

  // 将读取的数据转换为 QString
  QString ExpTimeList = QString::fromUtf8(buffer, ExpTimeListSize);

  // 释放内存空间
  delete[] buffer;

  // 关闭文件
  infile.close();

  return ExpTimeList;
}

void Tools::saveCFWList(QString Name, QString List)
{
  std::string directory = "config";                      // 配置文件夹名
  std::string filename = directory + "/CFWList(" + Name.toStdString() +").dat"; // 在配置文件夹中创建文件

  std::ofstream outfile(filename, std::ios::binary);

  if (!outfile.is_open())
  {
    std::cerr << "打开文件写入时发生错误: " << filename << std::endl;
    return;
  }

  QByteArray CFWListUtf8 = List.toUtf8();

  // 写入 QString 大小信息和数据
  size_t CFWListSize = static_cast<size_t>(CFWListUtf8.size());
  outfile.write(reinterpret_cast<const char *>(&CFWListSize), sizeof(size_t));
  outfile.write(CFWListUtf8.constData(), CFWListSize);

  outfile.close();
}

QString Tools::readCFWList(QString Name)
{
  std::string directory = "config";                      // 配置文件夹名
  std::string filename = directory + "/CFWList(" + Name.toStdString() +").dat";  // 在配置文件夹中创建文件
  std::ifstream infile(filename, std::ios::binary);

  if (!infile.is_open())
  {
    std::cerr << "打开文件读取时发生错误: " << filename << std::endl;
    return QString();
  }

  // 读取经验时间列表的大小
  size_t CFWListSize;
  infile.read(reinterpret_cast<char *>(&CFWListSize), sizeof(size_t));

  // 分配内存空间用于存储经验时间列表的数据
  char *buffer = new char[CFWListSize];

  // 读取经验时间列表的数据
  infile.read(buffer, CFWListSize);

  // 将读取的数据转换为 QString
  QString CFWList = QString::fromUtf8(buffer, CFWListSize);

  // 释放内存空间
  delete[] buffer;

  // 关闭文件
  infile.close();

  return CFWList;
}

void Tools::clearSystemDeviceListItem(SystemDeviceList &s,int index){
    //clear one device
    qDebug()<<"index:"<<index;
    if (s.system_devices.empty()) {
        qDebug()<<"s.system_devices is nullptr";
    }
    else {
        s.system_devices[index].Description="";
        s.system_devices[index].DeviceIndiGroup=-1;
        s.system_devices[index].DeviceIndiName="";
        s.system_devices[index].dp=NULL;
        s.system_devices[index].DriverFrom="";
        s.system_devices[index].DriverIndiName="";
        s.system_devices[index].isConnect=false;
        qDebug()<<"clearSystemDeviceListItem";
    }
}

void Tools::initSystemDeviceList(SystemDeviceList &s){
    s.system_devices.reserve(32); //pre-define 32 devices
    SystemDevice dev;
    dev.DeviceIndiName="";
    dev.DeviceIndiGroup=-1;
    dev.DeviceIndiName="";
    dev.DriverFrom="";      //DriverFrom 用于存储驱动类型。如果来自于INDI，则是“INDI"  如果来自于QHYCCD SDK  则是”QHYCCDSDK"
    dev.isConnect=false;
    dev.dp=NULL;

    for(int i=0;i<32;i++){
        s.system_devices.push_back(dev);
    }
}

int Tools::getTotalDeviceFromSystemDeviceList(SystemDeviceList s){
    //according the deviceIndiName to get how many devices in systemDeviceList
    //This
    int i=0;
    for(auto dev:s.system_devices){
        if(dev.DeviceIndiName !="") i++;
    }
    return i;
}

void Tools::cleanSystemDeviceListConnect(SystemDeviceList &s){
    for (int i=0;i<s.system_devices.size();i++){
        s.system_devices[i].isConnect=false;
        s.system_devices[i].dp=NULL;
    }
}

uint32_t Tools::getIndexFromSystemDeviceList(SystemDeviceList s,QString devname,int &index){
    int i=0;
    for(auto dev:s.system_devices){
        if (dev.DeviceIndiName == devname ){
            index = i;
            break;
        }
        i++;
    }
    if(i<32) {
        index=i;
        qDebug()<<"getIndexFromSystemDeviceList | found device in system list. device name" << devname<< "index" <<index;
        return QHYCCD_SUCCESS;
    }
    else{
        index=0;
        qDebug()<<"getIndexFromSystemDeviceList | not found device in system list, devname"<<devname;
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
        qDebug("stopIndiDriverAll | ERROR | INDI DRIVER NOT running");
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
//currently it can only handle the 8bit and 16bit RAW image
//does not support 32bit RAW image, 8bit and 16bit RGB image
  fitsfile* fptr;
  int status = 0;
  int bitpix, naxis;
  long naxes[2];
  long nelements;
  unsigned short* array;

  // 打开 FITS 文件
  if (fits_open_file(&fptr, fileName, READONLY, &status)) {
    return status;
  }

  // 读取图像信息
  if (fits_get_img_param(fptr, 2, &bitpix, &naxis, naxes, &status)) {
    return status;
  }

  // 确保图像是二维的
  if (naxis != 2) {
    return -1;
  }

  // 读取图像数据
  nelements = naxes[0] * naxes[1];
  array = new unsigned short[nelements];
  if (fits_read_img(fptr, TUSHORT, 1, nelements, NULL, array, NULL, &status)) {
    delete[] array;
    return status;
  }

  // 将数据转换为 cv::Mat
  if(bitpix==16)      image = cv::Mat(naxes[1], naxes[0], CV_16U, array).clone();
  else if(bitpix==8)  image = cv::Mat(naxes[1], naxes[0], CV_8U, array).clone();

  // 释放内存并关闭文件
  delete[] array;
  fits_close_file(fptr, &status);

  return status;
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
  qDebug("initqhyccdresosurce %d", ret);
  uint32_t devices = 0;

  devices = ScanQHYCCD();
  qDebug("found qhyccd device %d", devices);

  if (devices < 1) {
    qDebug() << "SelectQHYCCDSDKDevice | No QHYCCD SDK Device Found";
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
      qDebug("Found FPGA device:%d", fpgahandle_);
    } else if (strcmp(cameraName, "QHY5III178") == 0) {
      guiderhandle_ = OpenQHYCCD(camid_);
      qDebug("Found guider device:%d", guiderhandle_);
    } else if (strcmp(cameraName, "POLEMASTER") == 0) {
      // polerhandle_ = OpenQHYCCD(camid_);
      guiderhandle_ = OpenQHYCCD(camid_);
      qDebug("Found poler device:%d", polerhandle_);
    }
  }
}

void Tools::ScanCamera() {
  if (Tools::systemDeviceList().currentDeviceCode != 1) {
    int ret;
    camhandle_ = OpenQHYCCD(camid_);
    if (camhandle_ != NULL) {
      qDebug("Open QHYCCD success.\n");
    } else {
      qDebug("Open QHYCCD failure.\n");
    }

    ret = IsQHYCCDControlAvailable(camhandle_, CAM_SINGLEFRAMEMODE);
    if (QHYCCD_ERROR == ret) {
      qDebug("The detected camera is not support single frame.");
      // release sdk resources
      ret = ReleaseQHYCCDResource();
      if (QHYCCD_SUCCESS == ret) {
        qDebug("SDK resources released.");
      } else {
        qDebug() << "Cannot release SDK resources, error:" << ret;
      }
    }

    int mode = 0;
    ret = SetQHYCCDStreamMode(camhandle_, mode);
    if (QHYCCD_SUCCESS == ret) {
      qDebug() << "SetQHYCCDStreamMode set to:" << mode << "success.";
    } else {
      qDebug() << "SetQHYCCDStreamMode:" << mode << "failure, error:" << ret;
    }
    qDebug() << "\033[0m\033[1;35m"
             << "initialize camera"
             << "\033[0m";
    // initialize camera
    ret = InitQHYCCD(camhandle_);
    if (QHYCCD_SUCCESS == ret) {
      qDebug("InitQHYCCD success.");
    } else {
      qDebug() << "InitQHYCCD faililure, error:" << ret;
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
    qDebug() << "GetQHYCCDOverScanArea success";
  } else {
    qDebug() << "GetQHYCCDOverScanArea error";
    return {};
  }
  ret = GetQHYCCDOverScanArea(camhandle_, &effectiveStartX, &effectiveStartY,
                              &effectiveSizeX, &effectiveSizeY);
  if (QHYCCD_SUCCESS == ret) {
    qDebug() << "GetQHYCCDEffectiveArea success";
  } else {
    qDebug() << "GetQHYCCDEffectiveArea error";
    return {};
  }
  ret =
      GetQHYCCDChipInfo(camhandle_, &chipWidthMM, &chipHeightMM, &maxImageSizeX,
                        &maxImageSizeY, &pixelWidthUM, &pixelHeightUM, &bpp);
  if (QHYCCD_SUCCESS == ret) {
    qDebug() << "GetQHYCCDChipInfo success";
  } else {
    qDebug() << "GetQHYCCDChipInfo error";
    return {};
  }

  roiStartX = 0;
  roiStartY = 0;
  roiSizeX = maxImageSizeX;
  roiSizeY = maxImageSizeY;

  ret = IsQHYCCDControlAvailable(camhandle_, CAM_COLOR);
  if (ret == BAYER_GB || ret == BAYER_GR || ret == BAYER_BG ||
      ret == BAYER_RG) {
    qDebug() << "This is a color camera.";
    qDebug() << "even this is a color camera, in Single Frame mode THE SDK "
                "ONLY SUPPORT RAW OUTPUT.So please do not set "
                "SetQHYCCDDebayerOnOff() to true;";
  } else {
    qDebug() << "This is a mono camera.";
  }

  ret = IsQHYCCDControlAvailable(camhandle_, CONTROL_USBTRAFFIC);
  if (QHYCCD_SUCCESS == ret) {
    ret = SetQHYCCDParam(camhandle_, CONTROL_USBTRAFFIC, USB_TRAFFIC);
    if (QHYCCD_SUCCESS == ret) {
      qDebug() << "SetQHYCCDParam CONTROL_USBTRAFFIC set to:" << USB_TRAFFIC
               << "success.";
    } else {
      qDebug() << "SetQHYCCDParam CONTROL_USBTRAFFIC error";
      getchar();
      return {};
    }
  }

  ret = IsQHYCCDControlAvailable(camhandle_, CONTROL_GAIN);
  if (QHYCCD_SUCCESS == ret) {
    ret = SetQHYCCDParam(camhandle_, CONTROL_GAIN, CHIP_GAIN);
    if (QHYCCD_SUCCESS == ret) {
      qDebug() << "SetQHYCCDParam CONTROL_GAIN set to:" << CHIP_GAIN
               << "success.";
    } else {
      qDebug() << "SetQHYCCDParam CONTROL_GAIN error";
      getchar();
      return {};
    }
  }

  ret = IsQHYCCDControlAvailable(camhandle_, CONTROL_OFFSET);
  if (QHYCCD_SUCCESS == ret) {
    ret = SetQHYCCDParam(camhandle_, CONTROL_OFFSET, CHIP_OFFSET);
    if (QHYCCD_SUCCESS == ret) {
      qDebug() << "SetQHYCCDParam CONTROL_OFFSET set to:" << CHIP_OFFSET
               << "success.";
    } else {
      qDebug() << "SetQHYCCDParam CONTROL_OFFSET failed.";
      getchar();
      return {};
    }
  }

  ret = SetQHYCCDParam(camhandle_, CONTROL_EXPOSURE, EXPOSURE_TIME);
  if (QHYCCD_SUCCESS == ret) {
    qDebug() << "SetQHYCCDParam CONTROL_EXPOSURE set to:" << EXPOSURE_TIME
             << "success.";
  } else {
    qDebug() << "SetQHYCCDParam CONTROL_EXPOSURE failure";
    getchar();
    return {};
  }

  ret =
      SetQHYCCDResolution(camhandle_, roiStartX, roiStartY, roiSizeX, roiSizeY);
  if (QHYCCD_SUCCESS == ret) {
    qDebug() << "SetQHYCCDResolution success.";
  } else {
    qDebug() << "SetQHYCCDResolution error.";
    return {};
  }

  ret = SetQHYCCDBinMode(camhandle_, camBinX, camBinY);
  if (QHYCCD_SUCCESS == ret) {
    qDebug() << "SetQHYCCDBinMode success.";
  } else {
    qDebug() << "SetQHYCCDBinMode error.";
    return {};
  }

  ret = IsQHYCCDControlAvailable(camhandle_, CONTROL_TRANSFERBIT);
  if (QHYCCD_SUCCESS == ret) {
    ret = SetQHYCCDBitsMode(camhandle_, 16);
    if (QHYCCD_SUCCESS == ret) {
      qDebug() << "SetQHYCCDBitsMode success.";
    } else {
      qDebug() << "SetQHYCCDBitsMode error.";
      getchar();
      return {};
    }
  }

  qDebug() << "ExpQHYCCDSingleFrame(camhandle) - start...";
  ret = ExpQHYCCDSingleFrame(camhandle_);
  qDebug() << "ExpQHYCCDSingleFrame(camhandle) - end...";
  if (QHYCCD_ERROR != ret) {
    qDebug() << "ExpQHYCCDSingleFrame success.";
    if (QHYCCD_READ_DIRECTLY != ret) {
      QElapsedTimer t;
      t.start();

      QThread::usleep(glMainCameraExpTime_);

      qDebug() << t.elapsed();
    }
  } else {
    qDebug() << "ExpQHYCCDSingleFrame failure, error";
  }

  uint32_t length = GetQHYCCDMemLength(camhandle_);

  if (length > 0) {
    pImgData = new unsigned char[length];
    memset(pImgData, 0, length);
    qDebug() << "Allocated memory for frame:" << length;
  } else {
    qDebug() << "Cannot allocate memory for frame.";
    return {};
  }

  QElapsedTimer t;
  t.start();
  cv::Mat mmat;

  ret = GetQHYCCDSingleFrame(camhandle_, &roiSizeX, &roiSizeY, &bpp, &channels,
                             pImgData);
  if (QHYCCD_SUCCESS == ret) {
    qDebug() << "GetQHYCCDSingleFrame success.";
    // process image here

    // emit signalRefreshMainPageMainCameraImage(pImgData,"MONO");

    mmat = cv::Mat(maxImageSizeY, maxImageSizeX, CV_16UC1, pImgData, 0);

    std::vector<int> creat_quality;
    creat_quality.push_back(cv::IMWRITE_PNG_COMPRESSION);
    creat_quality.push_back(0);
    cv::imwrite("/dev/shm/SDK_Capture.png", mmat, creat_quality);
    mmat = mmat.clone();
  } else {
    qDebug() << "GetQHYCCDSingleFrame error";
    return {};
  }

  delete[] pImgData;

  qDebug() << t.elapsed();

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
  qDebug() << "showHistogram |" << image.channels();
  #endif
  QElapsedTimer t;
  t.start();

  QImage ret;

  if (image.channels() == 3) {
    #ifdef ImageDebug
    qDebug() << "showHistogram | Draw Histograme color";
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
    qDebug() << "showHistogram | Draw Histograme color | min of max value in 3 "
                "channels"
             << min_maxval;
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
    qDebug() << "showHistogram | Draw Histograme mono";
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
  qDebug() << "showHistogram | used time(ms) " << t.elapsed();
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
        qDebug()<<"showHistogram | Draw Histograme color | min of max value in 3 channels"<<min_maxval;
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
  qDebug()<<"showOpenCV_QLabel_withRotate | used time(ms) "<<t.elapsed();
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
    qDebug()<<"showOpenCV_QLabel_withRotate | "<<w<<h<<img.channels()<<img.type();
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
       qDebug(" showOpenCV_QLabel_withRotate | ERROR : unsupport image type") ;
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
    qDebug()<<"showOpenCV_QLabel_withRotate | used time(ms) "<<t.elapsed();
  #endif
}

void Tools::ImageSoftAWB(cv::Mat sourceImg16, cv::Mat& targetImg16, QString CFA,
                         double gainR, double gainB, uint16_t offset) {
  QElapsedTimer t;
  t.start();

  int height = sourceImg16.rows;
  int width = sourceImg16.cols;

  double gain1, gain2, gain3, gain4;

  qDebug() << "CFA:" << CFA;
  qDebug() << "gainR:" << gainR << "," << "gainB:" << gainB;

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

  qDebug() << "ImageSoftAWB | used time (ms) " << t.elapsed();
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
  qDebug() << "getAutoStretch |mean std B W" << mean.val[0] << std.val[0] << B << W;
  qDebug() << "getAutoStretch | used time(ms) " << t.elapsed();
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
  qDebug() << "Bit16To8_MakeLUT |" << B << W;
  #endif
}

void Tools::Bit16To8_Stretch(cv::Mat img16, cv::Mat img8, uint16_t B,
                             uint16_t W) {
  // this API support 16bit image input, 3 channel and 1 channel
  #ifdef ImageDebug
  qDebug() << "Bit16To8_Stretch | start" << B << W;
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
  qDebug() << "Bit16To8_Stretch | used time(ms) " << t.elapsed();
  #endif
  // cvDebugShow(img16);
  // cvDebugShow(img8);
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
                qDebug() << "internal error, hd = " << hd;
            }
        }
        else
        {
            qDebug("getDecAngle failed to parse angle string: "); // << str;
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
                qDebug() << "getDecAngle does not accept ...H...N/S: " << str;
                return -0.0;
            }
            h2d = 15;
        }
        double deg = (d + (m / 60) + (s / 3600)) * h2d * sgn;
        return deg * 2 * M_PI / 360.;
    }

    qDebug() << "getDecAngle failed to parse angle string: " << str;
    return -0.0;
}

cv::Mat Tools::CalMoments(cv::Mat image)
{
  qDebug("CalMoments:1");
  cv::Mat grayImage;
  if(image.channels() == 1)
  {
    grayImage = image.clone();
  }
  else
  {
    cvtColor(image, grayImage, CV_RGB2GRAY);
  }
  qDebug("CalMoments:2");
 

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

    qDebug("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
    qDebug() << "Backgroud brightness:" << Background;

    cv::Mat m = cv::Mat(gray.size(), gray.type(), cv::Scalar(Background));
    cv::Mat dst = cv::Mat::zeros(gray.size(), gray.type());

    subtract(gray, m, dst);

    return dst;
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

    qDebug() << "Barycentric Coordinate:" << FirstMoment_x << "," << FirstMoment_y;


    int height = subimage.rows;
    int width = subimage.cols;

    ushort Bri1 = subimage.at<ushort>(FirstMoment_y, FirstMoment_x);
    qDebug() << "Max:" << Bri1;

    int x_s, x_b;
    for (int i = FirstMoment_x; i > 0; i--)
    {
        ushort Bri_Left = subimage.at<ushort>(FirstMoment_y, i);
        if (Bri_Left <= Bri1 / 2)
        {
            x_s = i;
            break;
        }
    }

    for (int j = FirstMoment_x; j < width; j++)
    {
        ushort Bri_Right = subimage.at<ushort>(FirstMoment_y, j);
        if (Bri_Right <= Bri1 / 2)
        {
            x_b = j;
            break;
        }
    }

    // double FWHM = (x_b - x_s)/10.0;
    double FWHM = static_cast<double>(x_b-x_s)/10.0;
    qDebug() << x_b << x_s;

    qDebug() << "[[[[[FWHM:" << FWHM << "]]]]]";
    qDebug("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");

    cv::Mat imagePoint = image.clone();

    cv::Point PointA(FirstMoment_x/10, FirstMoment_y/10);
    cv::Point PointB(FirstMoment_x/10, FirstMoment_y/10);
    cv::line(imagePoint, PointA, PointB, cv::Scalar(0, 0, 0), 3, 8, 0);
    // imshow("Point",imagePoint);

    // return imagePoint;

    result.image = imagePoint;
    result.FWHM = FWHM;

    return result;

    // cv::waitKey();
}

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
// double Tools::getLST_Degree(QDateTime datetimeUTC, double longitude_radian) {
//   int year = datetimeUTC.date().year();
//   int month = datetimeUTC.date().month();
//   int day = datetimeUTC.date().day();
//   int hour = datetimeUTC.time().hour();
//   int minute = datetimeUTC.time().minute();
//   int second = datetimeUTC.time().second();
//   int msec = datetimeUTC.time().msec();

// #ifdef debug
//   qDebug() << "tools.cpp|getLST_Degree|datetimeUTC:" << datetimeUTC;
//   qDebug() << "tools.cpp|getLST_Degree|datetimeUTC:" << year << month << day
//            << hour << minute << second << msec;
// #endif

//   double jd;
//   StelUtils::getJDFromDate(&jd, year, month, day, hour, minute, second);

//   double d;
//   d = jd - 2451545.0;

// #ifdef debug
//   qDebug("tools.cpp|getLST_Degree|d = %f", d);
// #endif

//   double UT;

//   UT = hour + (minute * 60 + second + (double)msec / 1000) / 3600.0;

// #ifdef debug
//   qDebug("tools.cpp|getLST_Degree|UT = %f", UT);
// #endif

//   double longitude_Degree = RadToDegree(longitude_radian);

// #ifdef debug
//   qDebug("tools.cpp|getLST_Degree|longitude (degree) = %f", longitude_Degree);
// #endif

//   double LST;

//   LST = 100.46 + 0.985647 * d + longitude_Degree + 15 * UT;

// #ifdef debug
//   qDebug("tools.cpp|getLST_Degree|LST before range = %f", LST);
// #endif

//   LST = rangeTo(LST, 360.0, 0.0);

// #ifdef debug
//   qDebug("tools.cpp|getLST_Degree|LST after  range (degree) %f (hms) %s", LST,
//          qPrintable(radToHmsStr(DegreeToRad(LST), true)));
// #endif

//   return LST;
// }

double Tools::getHA_Degree(double RA_radian, double LST_Degree) {
  double HA;
  HA = LST_Degree - RadToDegree(RA_radian);

  HA = rangeTo(HA, 360.0, 0.0);

#ifdef debug
  qDebug("tools.cpp|getHA|HA (degree) %f (hms) %s", HA,
         qPrintable(radToHmsStr(DegreeToRad(HA), true)));
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
  qDebug("tools.cpp|ra_dec_to_alt_az|az alt (radian):%f %f (Degree) %f %f",
         az_radian, alt_radian, RadToDegree(az_radian),
         RadToDegree(alt_radian));
  qDebug("tools.cpp|ra_dec_to_alt_az|az alt (dms):%s %s",
         qPrintable(radToDmsStr(az_radian)),
         qPrintable(radToDmsStr(alt_radian)));
#endif
}


// 2023.12.21 CJQ
// void Tools::full_ra_dec_to_alt_az(QDateTime datetimeUTC, double ra_radian,
//                                   double dec_radian, double latitude_radian,
//                                   double longitude_radian, double& alt_radian,
//                                   double& az_radian) {
//   double LST_Degree = getLST_Degree(datetimeUTC, longitude_radian);
//   double HA_Degree = getHA_Degree(ra_radian, LST_Degree);
//   ra_dec_to_alt_az(DegreeToRad(HA_Degree), dec_radian, alt_radian, az_radian,
//                    latitude_radian);
// }

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
// void Tools::full_alt_az_to_ra_dec(QDateTime datetimeUTC, double alt_radian,
//                                   double az_radian, double latitude_radian,
//                                   double longitude_radian, double& ra_radian,
//                                   double& dec_radian) {
//   double ha_radian = 0;

//   alt_az_to_ra_dec(alt_radian, az_radian, ha_radian, dec_radian,
//                    latitude_radian);

//   double LST_Degree = getLST_Degree(datetimeUTC, longitude_radian);

//   double RA_Degree;
//   RA_Degree = LST_Degree - RadToDegree(ha_radian);

//   RA_Degree = rangeTo(RA_Degree, 360.0, 0.0);

//   ra_radian = DegreeToRad(RA_Degree);
// }

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

MinMaxFOV Tools::calculateFOV(int FocalLength,double CameraSize_width,double CameraSize_height)
{
  MinMaxFOV result;
  qDebug() << "FocalLength: " << FocalLength << ", " << "CameraSize: " << CameraSize_width << ", " << CameraSize_height;

  double CameraSize_diagonal = sqrt(pow(CameraSize_width, 2) + pow(CameraSize_height, 2));
  qDebug() << CameraSize_diagonal;

  double minFOV,maxFOV;

  minFOV = 2 * atan(CameraSize_height / (2 * FocalLength)) * 180 / M_PI;
  maxFOV = 2 * atan(CameraSize_diagonal / (2 * FocalLength)) * 180 / M_PI;

  result.minFOV = minFOV;
  result.maxFOV = maxFOV;
  
  qDebug() << "minFov: " << result.minFOV << ", " << "maxFov: " << result.maxFOV;

  return result;
}

SloveResults Tools::PlateSlove(int FocalLength,double CameraSize_width,double CameraSize_height, double Ra_Degree, double Dec_Degree, bool USEQHYCCDSDK)
{
  SloveResults result;
  MinMaxFOV FOV;

  qDebug() << "Ra_Degree:" << Ra_Degree << "," << "Dec_Degree:" << Dec_Degree;

  QString RA = QString::number(Ra_Degree);
  QString DEC = QString::number(Dec_Degree);

  FOV = calculateFOV(FocalLength, CameraSize_width, CameraSize_height);

  QString MinFOV = QString::number(FOV.minFOV);
  QString MaxFOV = QString::number(FOV.maxFOV);

  // solve image
  QProcess* cmd_test = new QProcess();

  QString command_qstr;
  QString filename;
  if(USEQHYCCDSDK == false)
  {
    filename = "/dev/shm/ccd_simulator_";
    command_qstr="solve-field " + filename + ".fits" + " --overwrite --scale-units degwidth --scale-low " + MinFOV + " --scale-high " + MaxFOV + " --ra " + RA + " --dec " + DEC + " --radius 10 --nsigma 12  --no-plots  --no-remove-lines --uniformize 0 --timestamp";
  }
  else
  {
    filename = "/dev/shm/SDK_Capture";
    command_qstr="solve-field " + filename + ".png"  + " --overwrite --scale-units degwidth --scale-low " + MinFOV + " --scale-high " + MaxFOV + " --ra " + RA + " --dec " + DEC + " --radius 10 --nsigma 12  --no-plots  --no-remove-lines --uniformize 0 --timestamp";
  }

  const char* command;
  command = command_qstr.toLocal8Bit();
  qDebug() << command;  // TODO:注释掉

  cmd_test->start(command);
  cmd_test->waitForStarted();
  cmd_test->waitForFinished();

  QApplication::processEvents();

  command_qstr = "wcsinfo " + filename + ".wcs";
  command = command_qstr.toLocal8Bit();

  cmd_test->start(command);
  cmd_test->waitForStarted();
  cmd_test->waitForFinished();

  QString str;
  str = cmd_test->readAllStandardOutput().data();

  qDebug("%s", qPrintable(str));

  int pos1 = str.indexOf("ra_center");
  int pos2 = str.indexOf("dec_center");
  int pos3 = str.indexOf("orientation_center");
  int pos4 = str.indexOf("ra_center_h");
  qDebug("pos 1 2 3 4: %d %d %d %d", pos1, pos2, pos3, pos4);

  QString str_RA_Degree, str_DEC_Degree, str_Rotation;

  str_RA_Degree = str.mid(pos1 + 10, pos2 - pos1 - 10 - 1);
  str_DEC_Degree = str.mid(pos2 + 11, pos3 - pos2 - 11 - 1);
  str_Rotation = str.mid(pos3 + 19, pos4 - pos3 - 19 - 1);

  double RA_Degree, DEC_Degree, Rotation_Degree;
  RA_Degree = str_RA_Degree.toDouble();
  DEC_Degree = str_DEC_Degree.toDouble();
  Rotation_Degree = str_Rotation.toDouble();

  qDebug("RA DEC Rotation(degree) %f %f %f", RA_Degree, DEC_Degree, Rotation_Degree);
  if (str == "") {
    qDebug("Tools:Plate Solve Failur");
    result.RA_Degree = -1;
    result.DEC_Degree = -1;
    return result;
  } else {
    qDebug() << "RA DEC " << QString::number(RA_Degree, 'g', 9) << " " << QString::number(DEC_Degree, 'g', 9);
    result.RA_Degree = RA_Degree;
    result.DEC_Degree = DEC_Degree;
    return result;
  }
}

int Tools::fitQuadraticCurve(const QVector<QPointF>& data, float& a, float& b, float& c) {
    int n = data.size();
    if (n < 4) {
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

// 2023.12.21 CJQ
// StelObjectSelect Tools::getStelObjectSelectName() 
// {
//   StelObjectSelect Object;
//   StelObjectMgr* Watch_objectMgr = GETSTELMODULE(StelObjectMgr);
//   QList<StelObjectP> Watch_Selected = Watch_objectMgr->getSelectedObject();
//   if (!Watch_Selected.empty()) {
//     Object.name = Watch_Selected[0]->getEnglishName();

//     double dec_j2000 = 0;
// 		double ra_j2000 = 0;
// 		StelUtils::rectToSphe(&ra_j2000,&dec_j2000,Watch_Selected[0]->getJ2000EquatorialPos(StelApp::getInstance().getCore())); 

//     double GOTO_RA = Tools::RadToHour(ra_j2000);
//     double GOTO_DEC = Tools::RadToDegree(dec_j2000);

//     Object.Ra_Hour = GOTO_RA;
//     Object.Dec_Degree = GOTO_DEC;

//     return Object;
//   }
//   else
//   {
//     Object.name = "No Select";
//     Object.Ra_Hour = 0;
//     Object.Dec_Degree = 0;

//     return Object;
//   }
// }

// 2023.12.21 CJQ
// StelObjectSelect Tools::getTargetRaDecFromStel(std::string SearchName)
// {
//   StelObjectSelect result;
//   result.name = SearchName.c_str();
//   StelObjectMgr* Search_objectMgr = GETSTELMODULE(StelObjectMgr);
// 	StelMovementMgr* Search_mvmgr = GETSTELMODULE(StelMovementMgr);

// 	Search_objectMgr->findAndSelect(SearchName.c_str());

// 	QList<StelObjectP> newSelected = Search_objectMgr->getSelectedObject();
// 	if (!newSelected.empty())
// 	{
// 		// Can't point to home planet
// 		if (newSelected[0]->getEnglishName()!= StelApp::getInstance().getCore()->getCurrentLocation().planetName)
// 		{
//       double dec_j2000 = 0;
// 		  double ra_j2000 = 0;
// 		  StelUtils::rectToSphe(&ra_j2000,&dec_j2000,newSelected[0]->getJ2000EquatorialPos(StelApp::getInstance().getCore()));
// 			Search_mvmgr->moveToObject(newSelected[0], Search_mvmgr->getAutoMoveDuration());
// 			Search_mvmgr->setFlagTracking(true);
//       result.Ra_Hour = Tools::RadToHour(ra_j2000);
//       result.Dec_Degree = Tools::RadToDegree(dec_j2000);
//       return result;
// 		}
// 		else
//     {
// 			Search_objectMgr->unSelect();
//       result.name = "No Target";
//       result.Ra_Hour = 0;
//       result.Dec_Degree = 0;

//       return result;
// 		}	
// 	}
// }