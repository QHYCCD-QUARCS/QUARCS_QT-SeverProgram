#ifndef MYCLIENT_H
#define MYCLIENT_H

#include "baseclient.h"
#include "basedevice.h"

#include <iostream>
#include <functional>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include "Logger.h"
#include "mountstate.h"

// 回调函数类型定义
using ImageReceivedCallback = std::function<void(const std::string& filename, const std::string& devname)>;

using MessageReceivedCallback = std::function<void(const std::string& message)>;



class MyClient : public INDI::BaseClient
{
    public:
        // MyClient(HomePageWidget *gui);
        MyClient();
        ~MyClient() = default;


    public:

        QElapsedTimer CaptureTestTimer;
        qint64 CaptureTestTime;

        // uint32_t QHYCCD_SUCCESS = 1;
        // uint32_t QHYCCD_ERROR = 0;
        // 添加设备
        void AddDevice(INDI::BaseDevice* device, const std::string& name);
        // 删除设备
        void RemoveDevice(const std::string& name);
        // 获取当前设备列表的设备数
        int GetDeviceCount() const;
        //
        void ClearDevices();
        QString PrintDevices();
        INDI::BaseDevice * GetDeviceFromList(int index);
        INDI::BaseDevice * GetDeviceFromListWithName(std::string devName);
        std::string GetDeviceNameFromList(int index);
        void listAllProperties(INDI::BaseDevice *dp);
        void disconnectAllDevice(void);

        void GetAllPropertyName(INDI::BaseDevice *dp);
        const char * PropertyTypeToString(INDI_PROPERTY_TYPE type);

        uint32_t StartWatch(INDI::BaseDevice *dp);

        uint32_t setBaudRate(INDI::BaseDevice *dp, int baudRate);

        //CCD API
        uint32_t setTemperature(INDI::BaseDevice *dp,double value);
        uint32_t getTemperature(INDI::BaseDevice *dp,double &value);
        uint32_t takeExposure(INDI::BaseDevice *dp,double seconds);
        uint32_t disableDSLRLiveView(INDI::BaseDevice *dp);
        uint32_t setCCDAbortExposure(INDI::BaseDevice *dp);
        uint32_t getCCDFrameInfo(INDI::BaseDevice *dp,int &X,int &Y,int &WIDTH,int &HEIGHT);
        uint32_t setCCDFrameInfo(INDI::BaseDevice *dp,int X,int Y,int WIDTH,int HEIGHT); //something like setROI
        uint32_t resetCCDFrameInfo(INDI::BaseDevice *dp);  //will reset the ROI to default and reset the BIN to bin11
        uint32_t setCCDCooler(INDI::BaseDevice *dp,bool  enable);
        uint32_t getCCDCooler(INDI::BaseDevice *dp,bool & enable);
        uint32_t getCCDBasicInfo(INDI::BaseDevice *dp,int &maxX,int &maxY,double &pixelsize,double &pixelsizX,double &pixelsizY,int &bitDepth);
        uint32_t setCCDBasicInfo(INDI::BaseDevice *dp,int maxX,int maxY,double pixelsize,double pixelsizX,double pixelsizY,int bitDepth);
        uint32_t getCCDBinning(INDI::BaseDevice *dp,int &BINX,int &BINY,int &BINXMAX,int &BINYMAX);
        uint32_t setCCDBinnign(INDI::BaseDevice *dp,int BINX,int BINY);
        uint32_t getCCDCFA(INDI::BaseDevice *dp,int &offsetX, int &offsetY, QString &CFATYPE);
        uint32_t getCCDSDKVersion(INDI::BaseDevice *dp, QString &version);
        uint32_t getCCDGain(INDI::BaseDevice *dp,int &value,int &min,int &max);
        uint32_t setCCDGain(INDI::BaseDevice *dp,int value);
        uint32_t getCCDOffset(INDI::BaseDevice *dp,int &value,int &min,int &max);
        uint32_t setCCDOffset(INDI::BaseDevice *dp,int value);
        uint32_t getCCDReadMode(INDI::BaseDevice *dp,int &value,int &min,int &max);
        uint32_t setCCDReadMode(INDI::BaseDevice *dp,int value);

        uint32_t setCCDUploadModeToLacal(INDI::BaseDevice *dp);

        uint32_t setCCDUpload(INDI::BaseDevice *dp, QString Dir, QString Prefix);

        //Telescope API
        uint32_t getTelescopeInfo(INDI::BaseDevice *dp,double &telescope_aperture,double & telescope_focal,double & guider_aperature, double &guider_focal);
        uint32_t setTelescopeInfo(INDI::BaseDevice *dp,double telescope_aperture,double telescope_focal,double guider_aperature, double guider_focal);
        uint32_t getTelescopePierSide(INDI::BaseDevice *dp,QString &side);
        uint32_t getTelescopeTrackRate(INDI::BaseDevice *dp,QString &rate);
        uint32_t setTelescopeTrackRate(INDI::BaseDevice *dp,QString rate);
        uint32_t getTelescopeTrackEnable(INDI::BaseDevice *dp,bool &enable);
        uint32_t setTelescopeTrackEnable(INDI::BaseDevice *dp,bool enable);

        uint32_t setTelescopeAbortMotion(INDI::BaseDevice *dp);

        uint32_t setTelescopeParkOption(INDI::BaseDevice *dp,QString option);
        uint32_t getTelescopeParkPosition(INDI::BaseDevice *dp,double &RA,double &DEC);
        uint32_t setTelescopeParkPosition(INDI::BaseDevice *dp,double RA,double DEC);
        uint32_t getTelescopePark(INDI::BaseDevice *dp,bool &isParked);
        uint32_t setTelescopePark(INDI::BaseDevice *dp,bool isParked);
        uint32_t setTelescopeHomeInit(INDI::BaseDevice *dp,QString command);
        uint32_t getTelescopeMoving(INDI::BaseDevice *dp);

        uint32_t getTelescopeSlewRate(INDI::BaseDevice *dp,int &speed);
        uint32_t setTelescopeSlewRate(INDI::BaseDevice *dp,int speed);
        uint32_t getTelescopeTotalSlewRate(INDI::BaseDevice *dp,int &total);
        uint32_t getTelescopeMaxSlewRateOptions(INDI::BaseDevice *dp,int &min, int &max,int &value);
        uint32_t setTelescopeMaxSlewRateOptions(INDI::BaseDevice *dp,int value);


        uint32_t getMountInfo(INDI::BaseDevice *dp,QString &version);
        uint32_t setAutoFlip(INDI::BaseDevice *dp,bool ON);
        uint32_t startFlip(INDI::BaseDevice *dp);
        uint32_t flipBack(INDI::BaseDevice *dp, double raHours, double decDeg);
        uint32_t setMinutesPastMeridian(INDI::BaseDevice *dp,double Eastvalue , double Westvalue);
        uint32_t getMinutesPastMeridian(INDI::BaseDevice *dp,double &Eastvalue, double &Westvalue);
        uint32_t setAUXENCODERS(INDI::BaseDevice *dp);
        uint32_t getTelescopeMoveWE(INDI::BaseDevice *dp,QString &statu) ;
        uint32_t setTelescopeMoveWE(INDI::BaseDevice *dp,QString command);
        uint32_t getTelescopeMoveNS(INDI::BaseDevice *dp,QString &statu)  ;
        uint32_t setTelescopeMoveNS(INDI::BaseDevice *dp,QString command);
        uint32_t setTelescopeGuideNS(INDI::BaseDevice* dp, int dir, int time_guide);
        uint32_t setTelescopeGuideWE(INDI::BaseDevice* dp, int dir, int time_guide);
        uint32_t setTelescopeActionAfterPositionSet(INDI::BaseDevice *dp,QString action)  ;
        uint32_t getTelescopeRADECJ2000(INDI::BaseDevice *dp,double & RA_Hours,double & DEC_Degree)  ;
        uint32_t setTelescopeRADECJ2000(INDI::BaseDevice *dp,double RA_Hours,double DEC_Degree);
        uint32_t getTelescopeRADECJNOW(INDI::BaseDevice *dp,double & RA_Hours,double & DEC_Degree)  ;
        uint32_t setTelescopeRADECJNOW(INDI::BaseDevice *dp,double RA_Hours,double DEC_Degree);
        uint32_t getTelescopeTargetRADECJNOW(INDI::BaseDevice *dp,double & RA_Hours,double & DEC_Degree)  ;
        uint32_t setTelescopeTargetRADECJNOW(INDI::BaseDevice *dp,double RA_Hours,double DEC_Degree)  ;
        uint32_t getTelescopetAZALT(INDI::BaseDevice *dp,double &AZ_DEGREE,double &ALT_DEGREE);
        uint32_t setTelescopetAZALT(INDI::BaseDevice *dp,double AZ_DEGREE,double ALT_DEGREE);


        uint32_t slewTelescopeJNowNonBlock(INDI::BaseDevice *dp,double RA_Hours,double DEC_Degree,bool EnableTracking);
        uint32_t syncTelescopeJNow(INDI::BaseDevice *dp,double RA_Hours,double DEC_Degree);

        uint32_t getTelescopeStatus(INDI::BaseDevice *dp,QString &statu);
        
    


        //--------------------CFW API
        uint32_t getCFWPosition(INDI::BaseDevice *dp,int & position,int &min,int &max);
        uint32_t setCFWPosition(INDI::BaseDevice *dp,int position);
        uint32_t getCFWSlotName(INDI::BaseDevice *dp,QString & name);
        uint32_t setCFWSlotName(INDI::BaseDevice *dp,QString name);


        //Focuser API
        uint32_t getFocuserSDKVersion(INDI::BaseDevice *dp,QString &version);
        uint32_t getFocuserSpeed(INDI::BaseDevice *dp,int &value ,int &min,int &max,int &step);
        uint32_t setFocuserSpeed(INDI::BaseDevice *dp,int  value);
        uint32_t getFocuserMoveDiretion(INDI::BaseDevice *dp,bool & isDirectionIn);
        uint32_t setFocuserMoveDiretion(INDI::BaseDevice *dp,bool isDirectionIn);
        uint32_t getFocuserMaxLimit(INDI::BaseDevice *dp,int &maxlimit);
        uint32_t setFocuserMaxLimit(INDI::BaseDevice *dp,int  maxlimit);
        uint32_t getFocuserReverse(INDI::BaseDevice *dp,bool &isReversed);
        uint32_t setFocuserReverse(INDI::BaseDevice *dp,bool   isReversed);
        uint32_t moveFocuserSteps(INDI::BaseDevice *dp,int steps);
        uint32_t getFocuserRange(INDI::BaseDevice *dp,int & min, int & max, int & step, int & value);
        uint32_t moveFocuserToAbsolutePosition(INDI::BaseDevice *dp,int position);
        uint32_t getFocuserAbsolutePosition(INDI::BaseDevice *dp,int & position);
        uint32_t moveFocuserWithTime(INDI::BaseDevice *dp,int msec);
        uint32_t abortFocuserMove(INDI::BaseDevice *dp);
        uint32_t syncFocuserPosition(INDI::BaseDevice *dp,int position);
        uint32_t getFocuserOutTemperature(INDI::BaseDevice *dp,double &value);
        uint32_t getFocuserChipTemperature(INDI::BaseDevice *dp,double &value);


        //-----------GENERIC API-------------
        uint32_t getDevicePort(INDI::BaseDevice *dp,QString &Device_Port);        //add by CJQ 2023.3.3
        uint32_t setDevicePort(INDI::BaseDevice *dp,QString Device_Port);        //add by CJQ 2023.3.22
        uint32_t setTimeUTC(INDI::BaseDevice *dp,QDateTime datetime);
        uint32_t getTimeUTC(INDI::BaseDevice *dp,QDateTime &datetime);
        uint32_t setLocation(INDI::BaseDevice *dp,double latitude_degree, double longitude_degree, double elevation);
        uint32_t getLocation(INDI::BaseDevice *dp,double &latitude_degree, double &longitude_degree, double &elevation);
        uint32_t setAtmosphere(INDI::BaseDevice *dp,double temperature, double pressure, double humidity);
        uint32_t getAtmosphere(INDI::BaseDevice *dp,double &temperature, double &pressure, double &humidity);

        MountState mountState;
        QTimer MountGotoTimer;
        double oldRA_Hours = 0;
        double oldDEC_Degree = 0;
        void updateMountState(INDI::BaseDevice *dp);

        QString currentAction = "";

    //public slots:
        //void slotUpdateUI(QString filename,QString devname);



    // signals:
    //     void indiBlobImageReceived(QString filename, QString devname);  // Q_SIGNAL 
        // void updateUI(QString filename,QString devname); 

    //  设置回调函数
    void setImageReceivedCallback(ImageReceivedCallback callback) {
        imageReceivedCallback = callback;
    }

    // 收到图像并触发回调
    void receiveImage(const std::string& filename, const std::string& devname) {
        // 触发回调函数
        if (imageReceivedCallback) {
          imageReceivedCallback(filename, devname);
        }
    }

    void setMessageReceivedCallback(MessageReceivedCallback callback) {
        messageReceivedCallback = callback;
    }

    // 收到图像并触发回调
    void receiveMessage(const std::string& message) {
        // 触发回调函数
        if (imageReceivedCallback) {
          messageReceivedCallback(message);
        }
    }

    protected:
        //void newMessage(INDI::BaseDevice baseDevice, int messageID) override;
        // void newDevice(INDI::BaseDevice *dp) ;
        void newDevice(INDI::BaseDevice baseDevice) ;
        void newMessage(INDI::BaseDevice baseDevice, int messageID) override;
        // void newBLOB(IBLOB *bp) ;
        void newProperty(INDI::Property property) override;
        void updateProperty(INDI::Property property);
    private:
        INDI::BaseDevice mSimpleCCD;

        // 存储设备的列表
        std::vector<INDI::BaseDevice *> deviceList;
        // 存储设备名字的列表
        std::vector<std::string> deviceNames;


        

    ImageReceivedCallback imageReceivedCallback;

    MessageReceivedCallback messageReceivedCallback;

};


#endif // MYCLIENT_H





