#include "myclient.h"


#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <QDebug>
#include <QObject>
//#include <qhyccd.h>

#include <fitsio.h>
#include "tools.hpp"

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

  qDebug()<<"MyClient::MyClent"<<gethostid()<<getPort()<<getegid()<<geteuid();
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
    //qDebug("Recveing message from Server %s", baseDevice.messageQueue(messageID).c_str());
}


void MyClient::newDevice(INDI::BaseDevice baseDevice){
//the new Device is a callback function , the connect server will trig it. it can be override here

    qDebug()<<("myclient.cpp | new Device");

    const char *devname = baseDevice.getDeviceName();

    AddDevice(baseDevice,baseDevice.getDeviceName());


    qDebug() << devname;
    qDebug()<< "GetDeviceCount: " << GetDeviceCount();
}

void MyClient::updateProperty(INDI::Property property)
{
    if (property.getType() == INDI_BLOB)
    {
        CaptureTestTime = CaptureTestTimer.elapsed();
        qDebug() << "\033[32m" << "Exposure completed:" << CaptureTestTime << "milliseconds" << "\033[0m";
        CaptureTestTimer.invalidate();

        qDebug("Recveing image from Server size len name label format %d %d %s %s %s", property.getBLOB()->bp->size,property.getBLOB()->bp->bloblen,property.getBLOB()->bp->name,property.getBLOB()->bp->label,property.getBLOB()->bp->format);

        std::ofstream myfile;
        std::string filename="/dev/shm/ccd_simulator.fits";
        myfile.open(filename, std::ios::out | std::ios::binary);
        myfile.write(static_cast<char *>(property.getBLOB()->bp->blob), property.getBLOB()->bp->bloblen);
        myfile.close();

        QString devname_;
        Tools::readFitsHeadForDevName(filename,devname_);
        std::string devname = devname_.toStdString();

        receiveImage(filename, devname);
    } 
    else if (property.getType() == INDI_TEXT)
    {
        // qDebug() << "\033[32m" << "INDI new Text(label):" << property.getText()->label << "\033[0m";
        // qDebug() << "\033[32m" << "INDI new Text(name):" << property.getText()->name << "\033[0m";

        auto tvp = property.getText();
        if (tvp->isNameMatch("CCD_FILE_PATH"))
        {
            auto filepath = tvp->findWidgetByName("FILE_PATH");
            if (filepath){
                qDebug() << "\033[32m" << "New Capture Image Save To" << QString(filepath->getText()) << "\033[0m";

                CaptureTestTime = CaptureTestTimer.elapsed();
                qDebug() << "\033[32m" << "Exposure completed:" << CaptureTestTime << "milliseconds" << "\033[0m";
                CaptureTestTimer.invalidate();

                QString devname_;
                Tools::readFitsHeadForDevName(QString(filepath->getText()).toStdString(),devname_);
                std::string devname = devname_.toStdString();

                receiveImage(QString(filepath->getText()).toStdString(), devname);
            }  
        }
    } else if (property.getType() == INDI_NUMBER) {

    }
}

//************************ device list management***********************************


void MyClient::AddDevice(INDI::BaseDevice* device, const std::string& name) {
    deviceList.push_back(device);
    deviceNames.push_back(name);

}


void MyClient::RemoveDevice(const std::string& name) {
    int index = -1;
    for (int i = 0; i < deviceNames.size(); i++) {
        if (deviceNames[i] == name) {
            index = i;
            break;
        }
    }

    if (index >= 0) {
        deviceList.erase(deviceList.begin() + index);
        deviceNames.erase(deviceNames.begin() + index);
    }
}

int MyClient::GetDeviceCount() const {
    return deviceList.size();
}


void MyClient::ClearDevices() {
    deviceList.clear();
    deviceNames.clear();
}

QString MyClient::PrintDevices() {
    qDebug()<<"************ Device List ****************";
    QString dev;
    if(deviceNames.size()==0){
        qDebug()<<"myclient.cpp | PrintDevices | no device exist";
    }

    else{
        for (int i = 0; i < deviceNames.size(); i++) {
            qDebug() << i << deviceNames[i].c_str() << deviceList[i]->getDriverExec();
            if (i > 0)
            {
                dev.append("|"); // 添加分隔符
            }
            dev.append(deviceNames[i].c_str()); // 添加序号
            dev.append(":");
            dev.append(QString::number(i)); // 添加deviceNames元素
        }
    }
    qDebug()<<"*****************************************";
    return dev;
}


INDI::BaseDevice* MyClient::GetDeviceFromList(int index) {
//这个函数接受一个整型参数 index，表示要返回的设备在列表中的位置。如果 index 超出了列表的范围，函数返回 nullptr。否则，函数返回 deviceList 数组中对应位置的设备指针。
    if (index < 0 || index >= deviceList.size()) {
        return nullptr;
    }
    return deviceList[index];
}



INDI::BaseDevice* MyClient::GetDeviceFromListWithName(std::string devName) {

    for(int i=0;i<deviceList.size();i++){
        if(deviceNames[i]==devName) return deviceList[i];
    }

    //if not found return null
    return nullptr;

}


std::string MyClient::GetDeviceNameFromList(int index) {
    if (index < 0 || index >= deviceNames.size()) {
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

void MyClient::disconnectAllDevice(void){
    //disconnect all device in the device list
    // INDI::BaseDevice *dp;
    QVector<INDI::BaseDevice *> dp;
    qDebug()<<"disconnectAllDevice"<<deviceList.size();
    PrintDevices();
    for(int i=0;i<GetDeviceCount();i++)
    {
        dp.append(GetDeviceFromList(i));
        disconnectDevice(dp[i]->getDeviceName());
        while (dp[i]->isConnected())
        {
            qDebug("disconnectAllDevice | Waiting for disconnect finish...");
            sleep(1);
        }   
        qDebug()<<"disconnectAllDevice |"<<dp[i]->getDeviceName()<<dp[i]->isConnected();
    }
}




//need to wait the connection completely finished then call this , otherwise it may not output all
void MyClient::listAllProperties(INDI::BaseDevice *dp){
    std::vector<INDI::Property> properties(dp->getProperties().begin(),dp->getProperties().end());

     // Iterate over the list of properties and print the names of the PropertyNumber properties
     for (INDI::Property *property : properties)
     {
       //INDI::PropertyNumber *numberProperty = static_cast<INDI::PropertyNumber *>(property);
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
        qDebug() << "\033[31m" << propertyName << "\033[0m" << "\033[32m" << propertyType << "\033[0m";
    }
}

const char * MyClient::PropertyTypeToString(INDI_PROPERTY_TYPE type)
{
    // 使用一个自定义函数将属性类型枚举值转换为对应的字符串
    switch (type)
    {  
        case INDI_NUMBER: /*!< INumberVectorProperty. */
            return "Number";
        case INDI_SWITCH: /*!< ISwitchVectorProperty. */
            return "Switch";
        case INDI_TEXT:   /*!< ITextVectorProperty. */
            return "Text";
        case INDI_LIGHT:  /*!< ILightVectorProperty. */
            return "Light";
        case INDI_BLOB:   /*!< IBLOBVectorProperty. */
            return "Blob";
        case INDI_UNKNOWN:
            return "Unknown";
    }
}


/**************************************************************************************
**                                  CCD API
***************************************************************************************/

uint32_t MyClient::setTemperature(INDI::BaseDevice *dp,double value)
{
    char* propertyName = "CCD_TEMPERATURE";
    INDI::PropertyNumber ccdTemperature = dp->getProperty(propertyName);

    if (!ccdTemperature.isValid())
    {
        qDebug("Error: unable to find CCD_TEMPERATURE property...\n");
        return QHYCCD_ERROR;
    }

    IDLog("Setting temperature to %g C.\n", value);
    ccdTemperature[0].setValue(value);
    sendNewProperty(ccdTemperature);
    return QHYCCD_SUCCESS;
}


uint32_t MyClient::getTemperature(INDI::BaseDevice *dp,double &value)
{

    char* propertyName = "CCD_TEMPERATURE";
    INDI::PropertyNumber ccdTemperature = dp->getProperty(propertyName);




    if (!ccdTemperature.isValid())
    {
        //qDebug("Error: unable to find CCD_TEMPERATURE property...\n");
        return QHYCCD_ERROR;
    }

    //qDebug("getting temperature  %g C.\n", value);

    value = ccdTemperature->np[0].value;

    return QHYCCD_SUCCESS;
}


uint32_t MyClient::takeExposure(INDI::BaseDevice *dp,double seconds)
{
    INDI::PropertyNumber ccdExposure = dp->getProperty("CCD_EXPOSURE");

    if (!ccdExposure.isValid())
    {
        IDLog("Error: unable to find CCD Simulator CCD_EXPOSURE property...\n");
        return QHYCCD_ERROR;
    }

    CaptureTestTimer.start();
    qDebug() << "\033[32m" << "Exposure start." << "\033[0m";

    // Take a 1 second exposure
    IDLog("Taking a %g second exposure.\n", seconds);
    ccdExposure[0].setValue(seconds);
    sendNewProperty(ccdExposure);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCCDAbortExposure(INDI::BaseDevice *dp)
{
    INDI::PropertySwitch ccdabort = dp->getProperty("CCD_ABORT_EXPOSURE");

     if (!ccdabort.isValid())
     {
         IDLog("Error: unable to find  CCD_ABORT_EXPOSURE property...\n");
         return QHYCCD_ERROR;
     }

    //  ccdabort[0].setValue(1); //?? need to be confirmed with Jasem
     ccdabort[0].setState(ISS_ON);
     sendNewProperty(ccdabort);
     return QHYCCD_SUCCESS;
}

uint32_t MyClient::getCCDFrameInfo(INDI::BaseDevice *dp,int &X,int &Y,int &WIDTH,int &HEIGHT)
{
    INDI::PropertyNumber ccdFrameInfo = dp->getProperty("CCD_FRAME");



    if (!ccdFrameInfo.isValid())
    {
        IDLog("Error: unable to find CCD Simulator ccdFrameInfo property...\n");
        return QHYCCD_ERROR;
    }



    X = ccdFrameInfo->np[0].value;
    Y = ccdFrameInfo->np[1].value;
    WIDTH = ccdFrameInfo->np[2].value;
    HEIGHT = ccdFrameInfo->np[3].value;

    qDebug()<<"getCCDFrameInfo"<<X<<Y<<WIDTH<<HEIGHT;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCCDFrameInfo(INDI::BaseDevice *dp,int X,int Y,int WIDTH,int HEIGHT)
{
    INDI::PropertyNumber ccdFrameInfo = dp->getProperty("CCD_FRAME");

    if (!ccdFrameInfo.isValid())
    {
        IDLog("Error: unable to find CCD Simulator ccdFrameInfo property...\n");
        return QHYCCD_ERROR;
    }

    qDebug()<<"setCCDFrameInfo"<<X<<Y<<WIDTH<<HEIGHT;

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
        IDLog("Error: unable to find resetCCDFrameInfo property...\n");
        return QHYCCD_ERROR;
    }

    resetFrameInfo[0].setState(ISS_ON);
    //resetFrameInfo[0].setState(ISS_OFF);  //?? if need to set back?
    sendNewProperty(resetFrameInfo);
    resetFrameInfo[0].setState(ISS_OFF);
    sendNewProperty(resetFrameInfo);
    return QHYCCD_SUCCESS;
}



uint32_t MyClient::setCCDCooler(INDI::BaseDevice *dp,bool enable)
{
    INDI::PropertySwitch ccdCooler = dp->getProperty("CCD_COOLER");

    if (!ccdCooler.isValid())
    {
        IDLog("Error: unable to find CCD_COOLER property...\n");
        return QHYCCD_ERROR;
    }

    qDebug() << "setCCDCooler" << enable;

    if(enable==false)  ccdCooler[0].setState(ISS_OFF);
    else               ccdCooler[0].setState(ISS_ON);

    sendNewProperty(ccdCooler);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getCCDCooler(INDI::BaseDevice *dp,bool & enable)
{
    INDI::PropertySwitch ccdCooler = dp->getProperty("CCD_COOLER");

    if (!ccdCooler.isValid())
    {
        IDLog("Error: unable to find CCD_COOLER property...\n");
        return QHYCCD_ERROR;
    }

    qDebug() << "getCCDCooler" << ccdCooler[0].getState();

    if(ccdCooler[0].getState()==ISS_OFF) enable=false;
    else                                 enable=true;
    return QHYCCD_SUCCESS;
}


uint32_t MyClient::getCCDBasicInfo(INDI::BaseDevice *dp,int &maxX,int &maxY,double &pixelsize,double &pixelsizX,double &pixelsizY,int &bitDepth)
{
    INDI::PropertyNumber ccdInfo = dp->getProperty("CCD_INFO");

    if (!ccdInfo.isValid())
    {
        IDLog("Error: unable to find  CCD_INFO property...\n");
        return QHYCCD_ERROR;
    }

    maxX = ccdInfo->np[0].value;
    maxY = ccdInfo->np[1].value;
    pixelsize = ccdInfo->np[2].value;
    pixelsizX = ccdInfo->np[3].value;
    pixelsizY = ccdInfo->np[4].value;
    bitDepth  = ccdInfo->np[5].value;
    qDebug()<<"getCCDBasicInfo"<<maxX<<maxY<<pixelsize<<pixelsizX<<pixelsizY<<bitDepth;
    return QHYCCD_SUCCESS;
}



uint32_t MyClient::getCCDBinning(INDI::BaseDevice *dp,int &BINX,int &BINY,int &BINXMAX,int &BINYMAX){
    INDI::PropertyNumber ccdbinning = dp->getProperty("CCD_BINNING");

    if (!ccdbinning.isValid())
    {
        IDLog("Error: unable to find  CCD_BINNING property...\n");
        return QHYCCD_ERROR;
    }

    BINX = ccdbinning->np[0].value;
    BINY = ccdbinning->np[1].value;
    BINXMAX = ccdbinning->np[0].max;
    BINYMAX = ccdbinning->np[1].max;

    qDebug()<<"getCCDBinning"<<BINX<<BINY<<BINXMAX<<BINYMAX;
    return QHYCCD_SUCCESS;
}


uint32_t MyClient::setCCDBinnign(INDI::BaseDevice *dp,int BINX,int BINY){
    INDI::PropertyNumber ccdbinning = dp->getProperty("CCD_BINNING");

    if (!ccdbinning.isValid())
    {
        IDLog("Error: unable to find  CCD_BINNING property...\n");
        return QHYCCD_ERROR;
     }

    ccdbinning[0].setValue(BINX);
    ccdbinning[1].setValue(BINY);
    sendNewProperty(ccdbinning);
    qDebug()<<"setCCDBinnign"<<BINX<<BINY;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getCCDCFA(INDI::BaseDevice *dp,int &offsetX, int &offsetY, QString &CFATYPE)
{
    INDI::PropertyText ccdCFA = dp->getProperty("CCD_CFA");

    if (!ccdCFA.isValid())
    {
        IDLog("Error: unable to find  CCD_CFA property...\n");
        return QHYCCD_ERROR;
    }

    std::string a,b,c;

    a = ccdCFA[0].getText();
    b = ccdCFA[1].getText();
    c = ccdCFA[2].getText();

    offsetX= std::stoi(a);
    offsetY= std::stoi(b);
    CFATYPE= QString::fromStdString(c);
   // qDebug() << "getCCDCFA" << offsetX << offsetY << CFATYPE;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getCCDSDKVersion(INDI::BaseDevice *dp, QString &version)
{
    INDI::PropertyText ccdCFA = dp->getProperty("SDK_VERSION");

    if (!ccdCFA.isValid())
    {
        IDLog("Error: unable to find  SDK_VERSION property...\n");
        return QHYCCD_ERROR;
    }

    std::string a;

    a = ccdCFA[0].getText();

    version= QString::fromStdString(a);
    //qDebug()<<version;
    return QHYCCD_SUCCESS;
}



uint32_t MyClient::getCCDGain(INDI::BaseDevice *dp,int &value,int &min,int &max)
{
    INDI::PropertyNumber ccdgain = dp->getProperty("CCD_GAIN");

    if (!ccdgain.isValid())
    {
        IDLog("Error: unable to find  CCD_GAIN property...\n");
        return QHYCCD_ERROR;
    }

    value = ccdgain->np[0].value;
    min   = ccdgain->np[0].min;
    max   = ccdgain->np[0].max;

    qDebug() << "getCCDGain" << value << min << max;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCCDGain(INDI::BaseDevice *dp,int value){
    INDI::PropertyNumber ccdgain = dp->getProperty("CCD_GAIN");

    if (!ccdgain.isValid())
    {
        IDLog("Error: unable to find  CCD_BINNING property...\n");
        return QHYCCD_ERROR;
    }

    ccdgain[0].setValue(value);
    sendNewProperty(ccdgain);
    //qDebug()<<"setCCDBinnign"<< value;
    return QHYCCD_SUCCESS;
}




uint32_t MyClient::getCCDOffset(INDI::BaseDevice *dp,int &value,int &min,int &max)
{
    INDI::PropertyNumber ccdoffset = dp->getProperty("CCD_OFFSET");

    if (!ccdoffset.isValid())
    {
        IDLog("Error: unable to find  CCD_OFFSET property...\n");
        return QHYCCD_ERROR;
    }

    value = ccdoffset->np[0].value;
    min   = ccdoffset->np[0].min;
    max   = ccdoffset->np[0].max;

    qDebug() << "getCCDOFFSET" << value << min << max;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCCDOffset(INDI::BaseDevice *dp,int value)
{
    INDI::PropertyNumber ccdoffset = dp->getProperty("CCD_OFFSET");

    if (!ccdoffset.isValid())
    {
        IDLog("Error: unable to find  CCD_BINNING property...\n");
        return QHYCCD_ERROR;
    }

    ccdoffset[0].setValue(value);
    sendNewProperty(ccdoffset);
    //qDebug()<<"setCCDBinnign"<< value;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getCCDReadMode(INDI::BaseDevice *dp,int &value,int &min,int &max)
{
    INDI::PropertyNumber ccdreadmode = dp->getProperty("READ_MODE");

    if (!ccdreadmode.isValid())
    {
        IDLog("Error: unable to find  READ_MODE property...\n");
        return QHYCCD_ERROR;
    }

    value = ccdreadmode->np[0].value;
    min   = ccdreadmode->np[0].min;
    max   = ccdreadmode->np[0].max;

    qDebug() << "getCCDOFFSET" << value << min << max;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCCDReadMode(INDI::BaseDevice *dp,int value)
{
    INDI::PropertyNumber ccdreadmode = dp->getProperty("READ_MODE");

    if (!ccdreadmode.isValid())
    {
        IDLog("Error: unable to find  READ_MODE property...\n");
        return QHYCCD_ERROR;
    }

    ccdreadmode[0].setValue(value);
    sendNewProperty(ccdreadmode);
    //qDebug()<<"setCCDBinnign"<< value;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCCDUploadModeToLacal(INDI::BaseDevice *dp) {
    INDI::PropertySwitch uploadmode = dp->getProperty("UPLOAD_MODE");

    if (!uploadmode.isValid())
    {
        IDLog("Error: unable to find UPLOAD_MODE property...\n");
        return QHYCCD_ERROR;
    }

    uploadmode[0].setState(ISS_OFF);
    uploadmode[1].setState(ISS_ON);
    uploadmode[2].setState(ISS_OFF);

    sendNewProperty(uploadmode);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCCDUpload(INDI::BaseDevice *dp, QString Dir, QString Prefix) {
    INDI::PropertyText upload = dp->getProperty("UPLOAD_SETTINGS");

    if (!upload.isValid())
    {
        IDLog("Error: unable to find UPLOAD_SETTINGS property...\n");
        return QHYCCD_ERROR;
    }

    upload[0].setText(Dir.toLatin1().data());
    upload[1].setText(Prefix.toLatin1().data());

    sendNewProperty(upload);
    return QHYCCD_SUCCESS;
}


uint32_t MyClient::StartWatch(INDI::BaseDevice *dp)
{

    qDebug()<<"Watching | start";
    // wait for the availability of the device
    watchDevice(dp->getDeviceName(), [this](INDI::BaseDevice device)
    {

        // wait for the availability of the "CONNECTION" property
        device.watchProperty("CONNECTION", [this](INDI::Property)
        {
            qDebug("Watching | Connect to INDI Driver...\n");

            //connectDevice("Simple CCD");
        });

        // wait for the availability of the "CCD_TEMPERATURE" property
        device.watchProperty("CCD_TEMPERATURE", [this](INDI::PropertyNumber property)
        {

           // if (dp->isConnected())
           // {
                qDebug("Watching | CCD_TEMPERATURE event \n");

                //setTemperature(-20);
           // }

            // call lambda function if property changed
            property.onUpdate([property, this]()
            {
                qDebug("Watching | Receving new CCD Temperature: %g C\n", property[0].getValue());
                if (property[0].getValue() == -20)
                {
                    qDebug("Watching | CCD temperature reached desired value!\n");

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



                qDebug("Watching | Received image, saved as ccd_simulator.fits\n");
            });
        });
    });

        qDebug()<<"Watching | finish";
        return QHYCCD_SUCCESS;
}

/**************************************************************************************
**                                  Mount API
***************************************************************************************/
uint32_t MyClient::getTelescopeInfo(INDI::BaseDevice *dp,double &telescope_aperture,double & telescope_focal,double & guider_aperature, double &guider_focal)
{
    INDI::PropertyNumber property = dp->getProperty("TELESCOPE_INFO");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  TELESCOPE_INFO property...\n");
        return QHYCCD_ERROR;
    }

    telescope_aperture = property->np[0].value;
    telescope_focal    = property->np[1].value;
    guider_aperature   = property->np[2].value;
    guider_focal       = property->np[3].value;
    qDebug() << "getTelescopeInfo" << telescope_aperture << telescope_focal << guider_aperature << guider_focal;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeInfo(INDI::BaseDevice *dp,double telescope_aperture,double telescope_focal,double guider_aperature, double guider_focal)
{
    INDI::PropertyNumber property = dp->getProperty("TELESCOPE_INFO");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  TELESCOPE_INFO property...\n");
        return QHYCCD_ERROR;
    }

    property[0].setValue(telescope_aperture);
    property[1].setValue(telescope_focal);
    property[2].setValue(guider_aperature);
    property[3].setValue(guider_focal);
    sendNewProperty(property);
    //qDebug()<<"setCCDBinnign"<< value;
    return QHYCCD_SUCCESS;
}



uint32_t MyClient::getTelescopePierSide(INDI::BaseDevice *dp,QString &side)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_PIER_SIDE");

    if (!property.isValid())
    {
        IDLog("Error: unable to find TELESCOPE_PIER_SIDE property...\n");
        return QHYCCD_ERROR;
    }

    if(property[0].getState()==ISS_ON)      side="WEST";
    else if(property[1].getState()==ISS_ON) side="EAST";

    // qDebug() << "getTelescopePierSide" << side ;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getTelescopeTrackRate(INDI::BaseDevice *dp,QString &rate)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_TRACK_RATE");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  TELESCOPE_TRACK_RATE property...\n");
        return QHYCCD_ERROR;
    }

    if(property[0].getState()==ISS_ON)      rate="SIDEREAL";
    else if(property[1].getState()==ISS_ON) rate="SOLAR";
    else if(property[2].getState()==ISS_ON) rate="LUNAR"; //??
    else if(property[3].getState()==ISS_ON) rate="CUSTOM";
    qDebug() << "getTelescopeTrackRate" << rate ;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeTrackRate(INDI::BaseDevice *dp,QString rate)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_TRACK_RATE");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  TELESCOPE_TRACK_RATE property...\n");
        return QHYCCD_ERROR;
    }

    if(rate=="SIDEREAL")      {property[0].setState(ISS_ON);property[1].setState(ISS_OFF);property[2].setState(ISS_OFF);property[3].setState(ISS_OFF);}
    else if(rate=="SOLAR")    {property[0].setState(ISS_OFF);property[1].setState(ISS_ON);property[2].setState(ISS_OFF);property[3].setState(ISS_OFF);}
    else if(rate=="LUNAR")    {property[0].setState(ISS_OFF);property[1].setState(ISS_OFF);property[2].setState(ISS_ON);property[3].setState(ISS_OFF);}
    else if(rate=="CUSTOM")   {property[0].setState(ISS_OFF);property[1].setState(ISS_OFF);property[2].setState(ISS_OFF);property[3].setState(ISS_ON);}

    sendNewProperty(property);
    //qDebug()<<"setTelescopeTrackRate"<< value;
    return QHYCCD_SUCCESS;
}



uint32_t MyClient::getTelescopeTrackEnable(INDI::BaseDevice *dp,bool &enable)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_TRACK_STATE");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  TELESCOPE_TRACK_STATE property...\n");
        return QHYCCD_ERROR;
    }

    if(property[0].getState()==ISS_ON)      enable=true;
    else if(property[1].getState()==ISS_ON) enable=false;

    QElapsedTimer t;
    t.start();

    while(t.elapsed()<3000){
        QThread::msleep(100);
        if(property->getState()==IPS_OK) break;
        if(property->getState()==IPS_IDLE) break;
    }

    if(t.elapsed()>3000){
       qDebug() << "getTelescopeTrackEnable | ERROR : timeout ";
       return QHYCCD_ERROR;
    }


    qDebug() << "getTelescopeTrackEnable" << enable ;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeTrackEnable(INDI::BaseDevice *dp,bool enable)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_TRACK_STATE");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  TELESCOPE_TRACK_STATE property...\n");
        return QHYCCD_ERROR;
    }

    if(enable==true)          {property[0].setState(ISS_ON);property[1].setState(ISS_OFF);}
    else if(enable==false)    {property[0].setState(ISS_OFF);property[1].setState(ISS_ON);}

    sendNewProperty(property);

    /*
    QElapsedTimer t;
    t.start();


    if(enable==true){
     while(t.elapsed()<6000){
        qDebug()<<property->getStateAsString();
        QThread::msleep(100);
        if(property->getState()==IPS_IDLE) break;  //when enabled, it will become busy
     }
    }
    else{
        while(t.elapsed()<6000){
           qDebug()<<property->getStateAsString();
           QThread::msleep(100);
           if(property->getState()==IPS_IDLE) break;  //when disabled, it will become idle
     }
    }

    if(t.elapsed()>3000){
       qDebug() << "setTelescopeTrackEnable | ERROR : timeout ";
       return QHYCCD_ERROR;
    }

    */
    qDebug()<<"setTelescopeTrackEnable"<< enable;
    return QHYCCD_SUCCESS;
}





uint32_t MyClient::setTelescopeParkOption(INDI::BaseDevice *dp,QString option)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_PARK_OPTION");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  TELESCOPE_TRACK_RATE property...\n");
        return QHYCCD_ERROR;
    }

    if(option=="CURRENT")       {property[0].setState(ISS_ON);}
    else if(option=="DEFAULT")  {property[1].setState(ISS_ON);}
    else if(option=="WRITE")    {property[2].setState(ISS_ON);}


    sendNewProperty(property);
    qDebug()<<"setTelescopeParkOption"<< option;
    return QHYCCD_SUCCESS;
}


uint32_t MyClient::getTelescopeParkPosition(INDI::BaseDevice *dp,double &RA_DEGREE,double &DEC_DEGREE)
{
    INDI::PropertyNumber property = dp->getProperty("TELESCOPE_PARK_POSITION");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  TELESCOPE_PARK_POSITION property...\n");
        return QHYCCD_ERROR;
    }

    RA_DEGREE   = property->np[0].value;
    DEC_DEGREE = property->np[1].value;
    qDebug() << "getTelescopeParkPosition" << RA_DEGREE << DEC_DEGREE ;
    return QHYCCD_SUCCESS;

}
uint32_t MyClient::setTelescopeParkPosition(INDI::BaseDevice *dp,double RA_DEGREE,double DEC_DEGREE)
{
    INDI::PropertyNumber property = dp->getProperty("TELESCOPE_PARK_POSITION");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  TELESCOPE_PARK_POSITION property...\n");
        return QHYCCD_ERROR;
    }


    property->np[0].value= RA_DEGREE;
    property->np[1].value= DEC_DEGREE;

    sendNewProperty(property);
    qDebug()<<"setTelescopeParkPosition"<< RA_DEGREE << DEC_DEGREE;
    return QHYCCD_SUCCESS;
}
uint32_t MyClient::getTelescopePark(INDI::BaseDevice *dp,bool &isParked)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_PARK");

    if (!property.isValid())
    {
        IDLog("Error: unable to find TELESCOPE_PARK property...\n");
        return QHYCCD_ERROR;
    }
    if(property[0].getState()==ISS_ON)        isParked=true;
    else if(property[1].getState()==ISS_ON)   isParked=false;

    qDebug() << "getTelescopePark" << isParked;
    return QHYCCD_SUCCESS;
}
uint32_t MyClient::setTelescopePark(INDI::BaseDevice *dp,bool isParked)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_PARK");

    if (!property.isValid())
    {
        IDLog("Error: unable to find TELESCOPE_PARK property...\n");
        return QHYCCD_ERROR;
    }


    if(isParked==false) {
        property[1].setState(ISS_ON);
        property[0].setState(ISS_OFF);
    }
    else
    {
        property[0].setState(ISS_ON);
        property[1].setState(ISS_OFF);
    }                
    sendNewProperty(property);
     qDebug() << "setTelescopePark" << isParked;
    return QHYCCD_SUCCESS;
}



uint32_t MyClient::setTelescopeHomeInit(INDI::BaseDevice *dp,QString command)
{
    INDI::PropertySwitch property = dp->getProperty("HOME_INIT");

    if (!property.isValid())
    {
        IDLog("Error: unable to find HOME_INIT property...\n");
        return QHYCCD_ERROR;
    }

    if(command=="SLEWHOME")                   
    { 
        property[0].setState(ISS_ON); 
        property[1].setState(ISS_OFF);
    }
    else if(command=="SYNCHOME")              
    { 
        property[1].setState(ISS_OFF); 
        property[2].setState(ISS_ON);
    }

    sendNewProperty(property);

    QElapsedTimer t;
    t.start();

    while(t.elapsed()<3000){
        //qDebug()<<property->getStateAsString();
        QThread::msleep(100);
        if(property->getState()==IPS_IDLE) break;  // it will not wait the motor arrived
    }

    if(t.elapsed()>3000){
       qDebug() << "setTelescopeHomeInit | ERROR : timeout ";
       return QHYCCD_ERROR;
    }

    qDebug() << "setTelescopeHomeInit" << command;
    return QHYCCD_SUCCESS;
}





uint32_t MyClient::getTelescopeSlewRate(INDI::BaseDevice *dp,int &speed)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_SLEW_RATE");

    if (!property.isValid())
    {
        IDLog("Error: unable to find TELESCOPE_SLEW_RATE property...\n");
        return QHYCCD_ERROR;
    }

    if(property[0].getState()==ISS_ON)        speed=1;
    else if(property[1].getState()==ISS_ON)   speed=2;
    else if(property[2].getState()==ISS_ON)   speed=3;
    else if(property[3].getState()==ISS_ON)   speed=4;
    else if(property[4].getState()==ISS_ON)   speed=5;
    else if(property[5].getState()==ISS_ON)   speed=6;
    else if(property[6].getState()==ISS_ON)   speed=7;
    else if(property[7].getState()==ISS_ON)   speed=8;
    else if(property[8].getState()==ISS_ON)   speed=9;
    else if(property[9].getState()==ISS_ON)   speed=10;

    qDebug() << "getTelescopeSlewRate" << speed ;
    if(speed>=0 && speed<=9) qDebug()<<property[speed].getLabel();


    return QHYCCD_SUCCESS;

}

uint32_t MyClient::setTelescopeSlewRate(INDI::BaseDevice *dp,int speed)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_SLEW_RATE");

    if (!property.isValid())
    {
        IDLog("Error: unable to find TELESCOPE_SLEW_RATE property...\n");
        return QHYCCD_ERROR;
    }

    qDebug() << "property->count():" << property->count();
    if(speed>=0 && speed <= property->count())  
    {
        property[speed-1].setState(ISS_ON);

        for(int i = 0; i < property->count(); i++)
        {
            if(i != speed-1)
            {
                property[i].setState(ISS_OFF);
            }
        }

        sendNewProperty(property);
    }

    qDebug() << "setTelescopeSlewRate" << speed;
    return QHYCCD_SUCCESS;
}


uint32_t MyClient::getTelescopeTotalSlewRate(INDI::BaseDevice *dp,int &total)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_SLEW_RATE");

    if (!property.isValid())
    {
        IDLog("Error: unable to find TELESCOPE_SLEW_RATE property...\n");
        return QHYCCD_ERROR;
    }

    total=property->count();

    qDebug() << "getTelescopeTotalSlewRate:" << total;
    return QHYCCD_SUCCESS;
}




uint32_t MyClient::getTelescopeMaxSlewRateOptions(INDI::BaseDevice *dp,int &min, int &max,int &value)
{
    //?? maybe onstep only
    INDI::PropertyNumber property = dp->getProperty("Max slew Rate");

    if (!property.isValid())
    {
        IDLog("Error: unable to find Max slew Rate property...\n");
        return QHYCCD_ERROR;
    }

    max  = property->np[0].max;
    min  = property->np[0].min;
    value= property->np[0].value;

    qDebug() << "getTelescopeMaxSlewRateOptions" << max << min << value;
    return QHYCCD_SUCCESS;

}


uint32_t MyClient::setTelescopeMaxSlewRateOptions(INDI::BaseDevice *dp,int value)
{
    //?? maybe onstep only
    INDI::PropertyNumber property = dp->getProperty("Max slew Rate");

    if (!property.isValid())
    {
        IDLog("Error: unable to find Max slew Rate property...\n");
        return QHYCCD_ERROR;
    }

    property->np[0].value=value;
    sendNewProperty(property);

    QElapsedTimer t;
    t.start();

    while(property->getState()!=IPS_OK && t.elapsed()<3000){
        QThread::msleep(100);
    }

    if(t.elapsed()>3000){
       qDebug() << "setTelescopeMaxSlewRateOptions | ERROR : timeout ";
       return QHYCCD_ERROR;
    }

    qDebug() << "setTelescopeMaxSlewRateOptions" <<value;
    return QHYCCD_SUCCESS;
}







uint32_t MyClient::setTelescopeAbortMotion(INDI::BaseDevice *dp)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_ABORT_MOTION");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  TELESCOPE_ABORT_MOTION property...\n");
        return QHYCCD_ERROR;
    }

    property[0].setState(ISS_ON);
    sendNewProperty(property);
    //qDebug()<<"setTelescopeAbortMotion"<< value;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getTelescopeMoveWE(INDI::BaseDevice *dp,QString &statu)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_MOTION_WE");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  TELESCOPE_MOTION_WE property...\n");
        return QHYCCD_ERROR;
    }

    if(property[0].getState()==ISS_ON)      {statu="WEST";}
    else if(property[1].getState()==ISS_ON) {statu="EAST";}
    else                                    {statu="STOP";}
    qDebug() << "getTelescopeMoveWE" << statu ;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeMoveWE(INDI::BaseDevice *dp,QString command)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_MOTION_WE");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  TELESCOPE_MOTION_WE property...\n");
        return QHYCCD_ERROR;
    }

    if(command=="WEST")         {property[0].setState(ISS_ON);property[1].setState(ISS_OFF);}
    else if(command=="EAST")    {property[0].setState(ISS_OFF);property[1].setState(ISS_ON);}
    else if(command=="STOP")    {property[0].setState(ISS_OFF);property[1].setState(ISS_OFF);}

    sendNewProperty(property);
    qDebug()<<"setTelescopeMoveWE"<< command;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getTelescopeMoveNS(INDI::BaseDevice *dp,QString &statu)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_MOTION_NS");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  TELESCOPE_MOTION_NS property...\n");
        return QHYCCD_ERROR;
    }

    if(property[0].getState()==ISS_ON)      {statu="NORTH";}
    else if(property[1].getState()==ISS_ON) {statu="SOUTH";}
    else                                    {statu="STOP";}
    qDebug() << "getTelescopeMoveNS" << statu ;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeMoveNS(INDI::BaseDevice *dp,QString command)
{
    INDI::PropertySwitch property = dp->getProperty("TELESCOPE_MOTION_NS");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  TELESCOPE_MOTION_NS property...\n");
        return QHYCCD_ERROR;
    }

    if(command=="NORTH")         {property[0].setState(ISS_ON);property[1].setState(ISS_OFF);}
    else if(command=="SOUTH")    {property[0].setState(ISS_OFF);property[1].setState(ISS_ON);}
    else if(command=="STOP")     {property[0].setState(ISS_OFF);property[1].setState(ISS_OFF);}



    sendNewProperty(property);
    qDebug()<<"setTelescopeMoveNS"<< command;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeGuideNS(INDI::BaseDevice* dp, int dir, int time_guide)
{
    INDI::PropertyNumber property = dp->getProperty("TELESCOPE_TIMED_GUIDE_NS");
    if (!property.isValid()) {
        IDLog("Error: unable to find TELESCOPE_TIMED_GUIDE_NS property...\n");
        return QHYCCD_ERROR;
    }
    if (dir == 1) {
        property->np[1].value = time_guide;
        property->np[0].value = 0;
    } else
    {
        property->np[0].value = time_guide;
        property->np[1].value = 0;
    }
    sendNewProperty(property);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeGuideWE(INDI::BaseDevice* dp, int dir, int time_guide)
{
    INDI::PropertyNumber property = dp->getProperty("TELESCOPE_TIMED_GUIDE_WE");
    if (!property.isValid()) {
        IDLog("Error: unable to find TELESCOPE_TIMED_GUIDE_WE property...\n");
        return QHYCCD_ERROR;
    }
    if (dir == 3)
    {
        property->np[0].value = time_guide;
        property->np[1].value = 0;
    }
    else
    {    property->np[1].value = time_guide;
        property->np[0].value = 0;
    }
    sendNewProperty(property);
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeActionAfterPositionSet(INDI::BaseDevice *dp,QString action)
{
    INDI::PropertySwitch property = dp->getProperty("ON_COORD_SET");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  ON_COORD_SET property...\n");
        return QHYCCD_ERROR;
    }

    //qDebug()<<"ON_COORD_SET"<< property->count();
    //for(int i=0;i<property->count();i++){
    //    qDebug()<<"ON_COORD_SET" <<property[i].getName();
    //}

    if(action=="STOP")          {property[0].setState(ISS_ON);property[1].setState(ISS_OFF);property[2].setState(ISS_OFF);}
    else if(action=="TRACK")    {property[0].setState(ISS_OFF);property[1].setState(ISS_ON);property[2].setState(ISS_OFF);}
    else if(action=="SYNC")     {property[0].setState(ISS_OFF);property[1].setState(ISS_OFF);property[2].setState(ISS_ON);}


    sendNewProperty(property);


    QElapsedTimer t;
    t.start();

    while(property->getState()!=IPS_OK && t.elapsed()<3000){
        QThread::msleep(100);
    }

    if(t.elapsed()>3000){
       qDebug() << "setTelescopeActionAfterPositionSet | ERROR : timeout ";
       return QHYCCD_ERROR;
    }

    qDebug()<<"setTelescopeActionAfterPositionSet" << action;
    return QHYCCD_SUCCESS;
}



uint32_t MyClient::getTelescopeRADECJ2000(INDI::BaseDevice *dp,double & RA_Hours,double & DEC_Degree)
{
    INDI::PropertyNumber property = dp->getProperty("EQUATORIAL_COORD");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  EQUATORIAL_COORD property...\n");
        return QHYCCD_ERROR;
    }

    RA_Hours   = property->np[0].value;
    DEC_Degree = property->np[1].value;
    qDebug() << "getTelescopeRADECJ2000" << RA_Hours << DEC_Degree ;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeRADECJ2000(INDI::BaseDevice *dp,double RA_Hours,double DEC_Degree)
{
    INDI::PropertyNumber property = dp->getProperty("EQUATORIAL_COORD");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  EQUATORIAL_COORD property...\n");
        return QHYCCD_ERROR;
    }


    property->np[0].value= RA_Hours;
    property->np[1].value= DEC_Degree;

    sendNewProperty(property);
    //qDebug()<<"setTelescopeRADECJ2000"<< value;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getTelescopeRADECJNOW(INDI::BaseDevice *dp,double & RA_Hours,double & DEC_Degree)
{
    INDI::PropertyNumber property = dp->getProperty("EQUATORIAL_EOD_COORD");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  EQUATORIAL_EOD_COORD property...\n");
        return QHYCCD_ERROR;
    }

    RA_Hours   = property->np[0].value;
    DEC_Degree = property->np[1].value;
    //qDebug() << "getTelescopeRADECJNOW" << RA_Hours << DEC_Degree ;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeRADECJNOW(INDI::BaseDevice *dp,double RA_Hours,double DEC_Degree,INDI::PropertyNumber &property)
{
    property = dp->getProperty("EQUATORIAL_EOD_COORD");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  EQUATORIAL_EOD_COORD property...\n");
        return QHYCCD_ERROR;
    }


    property->np[0].value=RA_Hours;
    property->np[1].value=DEC_Degree;

    sendNewProperty(property);

    // QElapsedTimer t;
    // t.start();

    // while(t.elapsed()<3000){
    //     qDebug()<<property->getStateAsString();
    //     QThread::msleep(100);
    //     if(property->getState()==IPS_IDLE) break;  // it will not wait the motor arrived
    // }

    // if(t.elapsed()>3000){
    //    qDebug() << "setTelescopeRADECJNOW | ERROR : timeout ";
    //    return QHYCCD_ERROR;
    // }

    //qDebug()<<"setTelescopeRADECJNOW"<< value;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getTelescopeTargetRADECJNOW(INDI::BaseDevice *dp,double & RA_Hours,double & DEC_Degree)
{
    INDI::PropertyNumber property = dp->getProperty("TARGET_EOD_COORD");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  TARGET_EOD_COORD property...\n");
        return QHYCCD_ERROR;
    }

    RA_Hours   = property->np[0].value;
    DEC_Degree = property->np[1].value;
    qDebug() << "getTelescopeTargetRADECJNOW" << RA_Hours << DEC_Degree ;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopeTargetRADECJNOW(INDI::BaseDevice *dp,double RA_Hours,double DEC_Degree)
{
    INDI::PropertyNumber property = dp->getProperty("TARGET_EOD_COORD");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  TARGET_EOD_COORD property...\n");
        return QHYCCD_ERROR;
    }


    property->np[0].value=RA_Hours;
    property->np[1].value=DEC_Degree;

    sendNewProperty(property);
    //qDebug()<<"setTelescopeTargetRADECJNOW"<< value;
    return QHYCCD_SUCCESS;
}



//compose slew command
uint32_t MyClient::slewTelescopeJNowNonBlock(INDI::BaseDevice *dp,double RA_Hours,double DEC_Degree,bool EnableTracking,INDI::PropertyNumber &property)
{
    QString action;
    if(EnableTracking==true) action="TRACK";
    else                     action="STOP";

    setTelescopeActionAfterPositionSet(dp,action);
    setTelescopeRADECJNOW(dp,RA_Hours,DEC_Degree,property);
}


uint32_t MyClient::syncTelescopeJNow(INDI::BaseDevice *dp,double RA_Hours,double DEC_Degree,INDI::PropertyNumber &property)
{
    qDebug()<<"syncTelescopeJNow | start";
    QString action = "SYNC";


    setTelescopeActionAfterPositionSet(dp,action);

    setTelescopeRADECJNOW(dp,RA_Hours,DEC_Degree,property);
    qDebug()<<"syncTelescopeJNow | end";
}






uint32_t MyClient::getTelescopetAZALT(INDI::BaseDevice *dp,double & AZ_DEGREE,double & ALT_DEGREE)
{
    INDI::PropertyNumber property = dp->getProperty("HORIZONTAL_COORD");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  HORIZONTAL_COORD property...\n");
        return QHYCCD_ERROR;
    }

    ALT_DEGREE   = property->np[0].value;
    AZ_DEGREE    = property->np[1].value;
    qDebug() << "getTelescopetAZALT" << AZ_DEGREE << ALT_DEGREE ;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTelescopetAZALT(INDI::BaseDevice *dp,double AZ_DEGREE,double ALT_DEGREE)
{
    INDI::PropertyNumber property = dp->getProperty("HORIZONTAL_COORD");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  HORIZONTAL_COORD property...\n");
        return QHYCCD_ERROR;
    }
    property->np[0].value =AZ_DEGREE;
    property->np[1].value =ALT_DEGREE;

    sendNewProperty(property);
    //qDebug()<<"setTelescopetAZALT"<< AZ_DEGREE <<ALT_DEGREE;
    return QHYCCD_SUCCESS;
}



uint32_t MyClient::getTelescopeStatus(INDI::BaseDevice *dp,QString &statu,QString &error)
{
    INDI::PropertyText property = dp->getProperty("OnStep Status");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  OnStep Status property...\n");
        return QHYCCD_ERROR;
    }
    
    statu = property[1].getText();
    // error = property[7].getText();
    // qDebug()<<"OnStep error: "<< error;
    // if(error != "None") {
    //     qDebug() << "\033[32m" << "OnStep error: " << error << "\033[0m";
    // }
    
    return QHYCCD_SUCCESS;
}

/**************************************************************************************
**                                  Focus API
***************************************************************************************/
uint32_t MyClient::getFocuserSpeed(INDI::BaseDevice *dp,int &value ,int &min,int &max)
{
    INDI::PropertyNumber property = dp->getProperty("FOCUS_SPEED");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  FOCUS_SPEED property...\n");
        return QHYCCD_ERROR;
    }

    value   = property->np[0].value;
    min     = property->np[0].min;
    max     = property->np[0].max;
    qDebug() << "getFocuserSpeed" << value << min << max ;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setFocuserSpeed(INDI::BaseDevice *dp,int value)
{
    INDI::PropertyNumber property = dp->getProperty("FOCUS_SPEED");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  FOCUS_SPEED property...\n");
        return QHYCCD_ERROR;
    }
    property->np[0].value =value;

    sendNewProperty(property);

    qDebug()<<"setFocuserSpeed"<< value ;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getFocuserMoveDiretion(INDI::BaseDevice *dp,bool & isDirectionIn)
{
    INDI::PropertySwitch property = dp->getProperty("FOCUS_MOTION");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  FOCUS_MOTION property...\n");
        return QHYCCD_ERROR;
    }

    if(property[0].getState()==ISS_ON)      {isDirectionIn=true;}
    else if(property[1].getState()==ISS_ON) {isDirectionIn=false;}

    qDebug() << "getFocuserMoveDiretion | IN/OUT isDirectionIn:" << isDirectionIn ;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setFocuserMoveDiretion(INDI::BaseDevice *dp,bool isDirectionIn)
{
    INDI::PropertySwitch property = dp->getProperty("FOCUS_MOTION");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  FOCUS_MOTION property...\n");
        return QHYCCD_ERROR;
    }

    if(isDirectionIn==true)   {property[0].setState(ISS_ON);property[1].setState(ISS_OFF);}
    if(isDirectionIn==false)  {property[0].setState(ISS_OFF);property[1].setState(ISS_ON);}
    sendNewProperty(property);
    // qDebug() << "setFocuserMoveDiretion | IN/OUT isDirectionIn:" << isDirectionIn ;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getFocuserMaxLimit(INDI::BaseDevice *dp,int & maxlimit)
{
    INDI::PropertyNumber property = dp->getProperty("FOCUS_MAX");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  FOCUS_MAX property...\n");
        return QHYCCD_ERROR;
    }

    maxlimit   = property->np[0].value;

    qDebug() << "getFocuserMaxLimit" << maxlimit ;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setFocuserMaxLimit(INDI::BaseDevice *dp,int  maxlimit)
{
    INDI::PropertyNumber property = dp->getProperty("FOCUS_MAX");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  FOCUS_MAX property...\n");
        return QHYCCD_ERROR;
    }
    property->np[0].value =maxlimit;

    sendNewProperty(property);

    qDebug()<<"setFocuserMaxLimit"<< maxlimit;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getFocuserReverse(INDI::BaseDevice *dp,bool & isReversed)
{
    INDI::PropertySwitch property = dp->getProperty("FOCUS_REVERSE_MOTION");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  FOCUS_REVERSE_MOTION property...\n");
        return QHYCCD_ERROR;
    }

    if(property[0].getState()==ISS_ON)      {isReversed=true;}
    else if(property[1].getState()==ISS_ON) {isReversed=false;}

    qDebug() << "getFocuserReverse | IN/OUT isDirectionIn:" << isReversed ;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setFocuserReverse(INDI::BaseDevice *dp,bool  isReversed)
{
    INDI::PropertySwitch property = dp->getProperty("FOCUS_REVERSE_MOTION");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  FOCUS_REVERSE_MOTION property...\n");
        return QHYCCD_ERROR;
    }

    if(isReversed==true)   {property[0].setState(ISS_ON);property[1].setState(ISS_OFF);}
    if(isReversed==false)  {property[0].setState(ISS_OFF);property[1].setState(ISS_ON);}
    sendNewProperty(property);
    qDebug() << "setFocuserReverse | IN/OUT isDirectionIn:" << isReversed ;
    return QHYCCD_SUCCESS;
}

//---------------actions------------------


uint32_t MyClient::moveFocuserSteps(INDI::BaseDevice *dp,int steps)
{
    INDI::PropertyNumber property = dp->getProperty("REL_FOCUS_POSITION");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  REL_FOCUS_POSITION property...\n");
        return QHYCCD_ERROR;
    }
    property->np[0].value =steps;

    sendNewProperty(property);

    qDebug()<<"moveFocuserSteps"<< steps;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::moveFocuserToAbsolutePosition(INDI::BaseDevice *dp,int position)
{
    INDI::PropertyNumber property = dp->getProperty("ABS_FOCUS_POSITION");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  ABS_FOCUS_POSITION property...\n");
        return QHYCCD_ERROR;
    }
    property->np[0].value =position;

    sendNewProperty(property);

    qDebug()<<"moveFocuserToAbsolutePosition"<< position;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getFocuserAbsolutePosition(INDI::BaseDevice *dp,int & position)
{
    INDI::PropertyNumber property = dp->getProperty("ABS_FOCUS_POSITION");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  ABS_FOCUS_POSITION property...\n");
        return QHYCCD_ERROR;
    }
    
    position = property->np[0].value;

    // sendNewProperty(property);

    // qDebug()<<"getFocuserAbsolutePosition: "<< position;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::moveFocuserWithTime(INDI::BaseDevice *dp,int msec)
{
    //move the focuser at defined motion direction and defined move speed with msec time
    INDI::PropertyNumber property = dp->getProperty("FOCUS_TIMER");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  FOCUS_TIMER property...\n");
        return QHYCCD_ERROR;
    }
    property->np[0].value =msec;

    sendNewProperty(property);

    qDebug()<<"moveFocuserWithTime"<< msec;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::abortFocuserMove(INDI::BaseDevice *dp)
{
    INDI::PropertySwitch property = dp->getProperty("FOCUS_ABORT_MOTION");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  FOCUS_ABORT_MOTION property...\n");
        return QHYCCD_ERROR;
    }

    property[0].setState(ISS_ON);
    sendNewProperty(property);

    qDebug() << "abortFocuserMove" ;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::syncFocuserPosition(INDI::BaseDevice *dp,int position)
{
    INDI::PropertyNumber property = dp->getProperty("FOCUS_SYNC");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  FOCUS_SYNC property...\n");
        return QHYCCD_ERROR;
    }
    property->np[0].value =position;

    sendNewProperty(property);

    qDebug()<<"syncFocuserPosition"<< position;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getFocuserOutTemperature(INDI::BaseDevice *dp,double &value)
{
    INDI::PropertyNumber property = dp->getProperty("FOCUS_TEMPERATURE");

    // property->np[0].value = 0;

    sendNewProperty(property);

    if (!property.isValid())
    {
        IDLog("Error: unable to find  FOCUS_TEMPERATURE property...\n");
        return QHYCCD_ERROR;
    }

    value = property->np[0].value;
    qDebug()<<"getFocuserOutTemperature"<< value;

    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getFocuserChipTemperature(INDI::BaseDevice *dp,double &value)
{
    INDI::PropertyNumber property = dp->getProperty("CHIP_TEMPERATURE");

    sendNewProperty(property);

    if (!property.isValid())
    {
        IDLog("Error: unable to find  FOCUS_TEMPERATURE property...\n");
        return QHYCCD_ERROR;
    }

    value = property->np[0].value;
    qDebug()<<"getFocuserChipTemperature"<< value;

    return QHYCCD_SUCCESS;
}



/**************************************************************************************
**                                  CFW API
***************************************************************************************/
uint32_t MyClient::getCFWPosition(INDI::BaseDevice *dp,int & position,int &min,int &max)
{
    INDI::PropertyNumber property = dp->getProperty("FILTER_SLOT");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  FILTER_SLOT property...\n");
        min=0;
        max=0;
        position=0;
        return QHYCCD_ERROR;
    }

    position   = property->np[0].value;
    min        = property->np[0].min;
    max        = property->np[0].max;

    qDebug() << "getCFWCurrentPosition" << position  << min << max ;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCFWPosition(INDI::BaseDevice *dp,int position)
{
    INDI::PropertyNumber property = dp->getProperty("FILTER_SLOT");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  FILTER_SLOT property...\n");
        return QHYCCD_ERROR;
    }
    property->np[0].value =position;


    sendNewProperty(property);

    QElapsedTimer t;
    t.start();

    int timeout=10000;
    while(t.elapsed()<timeout){
        qDebug() <<property->getStateAsString();
        // qDebug() << "State:" << property->getState();
        QThread::msleep(300);
        if(property->getState()==IPS_OK) {
            qDebug() <<property->getStateAsString();
            break;  // it will not wait the motor arrived
        }
    }

    if(t.elapsed()>timeout){
       qDebug() << "setCFWPosition | ERROR : timeout ";
       return QHYCCD_ERROR;
    }

    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getCFWSlotName(INDI::BaseDevice *dp,QString & name)
{
    INDI::PropertyText property = dp->getProperty("FILTER_NAME");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  FILTER_NAME property...\n");
        return QHYCCD_ERROR;
    }

    name   = property[0].getText();

    qDebug() << "getCFWSlotName" << name;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setCFWSlotName(INDI::BaseDevice *dp,QString name)
{
    INDI::PropertyText property = dp->getProperty("FILTER_NAME");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  FILTER_NAME property...\n");
        return QHYCCD_ERROR;
    }

    property[0].setText(name.toLatin1().data());

    sendNewProperty(property);
    //qDebug()<<"setCFWSlotName"<< name ;
    return QHYCCD_SUCCESS;
}

/**************************************************************************************
**                         Generic Properties
***************************************************************************************/
uint32_t MyClient::getDevicePort(INDI::BaseDevice *dp,QString &Device_Port)        //add by CJQ 2023.3.3
{
    INDI::PropertyText property = dp->getProperty("DEVICE_PORT");

    if (!property.isValid())
    {
        IDLog("Error: unable to find DEVICE_PORT property...\n");
        return QHYCCD_ERROR;
    }

    Device_Port = property[0].getText(); 

    qDebug() << "getDevicePort" << Device_Port;
    
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setDevicePort(INDI::BaseDevice *dp,QString Device_Port)        //add by CJQ 2023.3.28
{
    INDI::PropertyText property = dp->getProperty("DEVICE_PORT");

    if (!property.isValid())
    {
        IDLog("Error: unable to find DEVICE_PORT property...\n");
        return QHYCCD_ERROR;
    }

    property[0].setText(Device_Port.toLatin1().data());

    sendNewProperty(property);

    qDebug() << "setDevicePort" << Device_Port;
    
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::setTimeUTC(INDI::BaseDevice *dp,QDateTime datetime)
{
    INDI::PropertyText property = dp->getProperty("TIME_UTC");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  TIME_UTC property...\n");
        return QHYCCD_ERROR;
    }

    QDateTime datetime_utc;
    datetime_utc=datetime.toUTC();

    QString time_utc = datetime_utc.toString(Qt::ISODate);
    QTimeZone timeZone = datetime.timeZone();
    // Print the time zone offset and abbreviation
    qDebug() << "setTimeUTC | Time zone offset:" << timeZone.offsetFromUtc(datetime);
    qDebug() << "setTimeUTC | Time zone abbreviation:" << timeZone.abbreviation(datetime);

    int timezone_hours = (timeZone.offsetFromUtc(datetime_utc))/3600;



    QString offset = QString::number(timezone_hours);


    qDebug()<<"setTimeUTC"<< time_utc <<offset;
    property[0].setText(time_utc.toLatin1().data());
    property[1].setText(offset.toLatin1().data());

    qDebug() << "property[0].setText(time_utc.toLatin1().data());" << time_utc.toLatin1().data();
    qDebug() << "property[1].setText(offset.toLatin1().data());" << offset.toLatin1().data();
    
    sendNewProperty(property); 

    QElapsedTimer t;
    t.start();

    while(property->getState()!=IPS_OK && t.elapsed()<3000){
        QThread::msleep(100);
    }

    if(t.elapsed()>3000){
       qDebug() << "setTimeUTC | ERROR : timeout ";
       return QHYCCD_ERROR;
    }



    qDebug()<<"setTimeUTC"<< time_utc << offset;
    return QHYCCD_SUCCESS;
}


uint32_t MyClient::getTimeUTC(INDI::BaseDevice *dp,QDateTime &datetime)
{
    INDI::PropertyText property = dp->getProperty("TIME_UTC");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  TIME_UTC property...\n");
        return QHYCCD_ERROR;
    }

    QString time   = property[0].getText();  //ISO8601 string , UTC
    QString offset = property[1].getText();

    qDebug()<<"getTimeUTC"<< time <<offset;





    datetime = QDateTime::fromString(time, Qt::ISODate);
    QTimeZone timeZone(offset.toInt()*3600);
    datetime.setTimeZone(timeZone);




    //qDebug()<<"getTimeUTC"<< time <<offset;
    //qDebug()<<"getTimeUTC"<< datetime.toLocalTime();


    QElapsedTimer t;
    t.start();

    while(property->getState()!=IPS_OK && t.elapsed()<3000){
        QThread::msleep(100);
    }

    if(t.elapsed()>3000){
       qDebug() << "getTimeUTC | ERROR : timeout ";
       return QHYCCD_ERROR;
    }


    return QHYCCD_SUCCESS;
}


uint32_t MyClient::setLocation(INDI::BaseDevice *dp,double latitude_degree, double longitude_degree, double elevation)
{
    INDI::PropertyNumber property = dp->getProperty("GEOGRAPHIC_COORD");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  GEOGRAPHIC_COORD property...\n");
        return QHYCCD_ERROR;
    }

    property->np[0].value = latitude_degree;
    property->np[1].value = longitude_degree;
    property->np[2].value = elevation;

    sendNewProperty(property);



    QElapsedTimer t;
    t.start();

    while(property->getState()!=IPS_OK && t.elapsed()<3000){
        QThread::msleep(100);
    }

    if(t.elapsed()>3000){
       qDebug() << "setLocation | ERROR : timeout ";
       return QHYCCD_ERROR;
    }

    qDebug()<<"setLocation"<< latitude_degree << longitude_degree << elevation;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getLocation(INDI::BaseDevice *dp,double &latitude_degree, double &longitude_degree, double &elevation)
{
    INDI::PropertyNumber property = dp->getProperty("GEOGRAPHIC_COORD");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  GEOGRAPHIC_COORD property...\n");
        return QHYCCD_ERROR;
    }

    latitude_degree   = property->np[0].value;  //ISO8601 string
    longitude_degree  = property->np[1].value;
    elevation         = property->np[2].value;

    qDebug()<<"getLocation"<< latitude_degree << longitude_degree << elevation;


    QElapsedTimer t;
    t.start();

    while(property->getState()!=IPS_OK && t.elapsed()<3000){
        QThread::msleep(100);
    }

    if(t.elapsed()>3000){
       qDebug() << "getLocation | ERROR : timeout ";
       return QHYCCD_ERROR;
    }

    return QHYCCD_SUCCESS;
}


uint32_t MyClient::setAtmosphere(INDI::BaseDevice *dp,double temperature, double pressure, double humidity)
{
    INDI::PropertyNumber property = dp->getProperty("ATMOSPHERE");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  ATMOSPHERE property...\n");
        return QHYCCD_ERROR;
    }

    property->np[0].value = temperature;
    property->np[1].value = pressure;
    property->np[2].value = humidity;

    sendNewProperty(property);

    QElapsedTimer t;
    t.start();

    while(property->getState()!=IPS_OK && t.elapsed()<3000){
        QThread::msleep(100);
    }

    if(t.elapsed()>3000){
       qDebug() << "setAtmosphere | ERROR : timeout ";
       return QHYCCD_ERROR;
    }

    qDebug()<<"setAtmosphere"<< temperature << pressure << humidity;
    return QHYCCD_SUCCESS;
}

uint32_t MyClient::getAtmosphere(INDI::BaseDevice *dp,double &temperature, double &pressure, double &humidity)
{
    INDI::PropertyNumber property = dp->getProperty("ATMOSPHERE");

    if (!property.isValid())
    {
        IDLog("Error: unable to find  ATMOSPHERE property...\n");
        return QHYCCD_ERROR;
    }

    temperature   = property->np[0].value;  //ISO8601 string
    pressure      = property->np[1].value;
    humidity      = property->np[2].value;

    qDebug()<<"getsetAtmosphere"<< temperature << pressure << humidity;


    QElapsedTimer t;
    t.start();

    while(property->getState()!=IPS_OK && t.elapsed()<3000){
        QThread::msleep(100);
    }

    if(t.elapsed()>3000){
       qDebug() << "getsetAtmosphere | ERROR : timeout ";
       return QHYCCD_ERROR;
    }

    return QHYCCD_SUCCESS;
}




