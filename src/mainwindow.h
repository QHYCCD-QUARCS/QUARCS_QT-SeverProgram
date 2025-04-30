#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QObject>
#include <QThread>
#include <QProcess>
#include <fitsio.h>
// #include "websocketclient.h"
#include "myclient.h"
#include <QFile>
#include "tools.hpp"
#include <QXmlStreamReader>
#include "websocketthread.h"

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <iostream>
#include <fstream>
#include <vector>
#include <QNetworkInterface>
#include <filesystem>

#include <string>
#include <algorithm>
#include <sys/statvfs.h>
#include <QStorageInfo>
#include <QtSerialPort/QSerialPortInfo>
#include <QSerialPort>

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <climits> 

#include <stellarsolver.h>

#include "platesolveworker.h"

#include <regex>

#include <thread> // 确保包含此头文件
#include <chrono> // 包含用于时间的头文件
#include <cmath>  // 包含数学函数

#define QT_Client_Version "20250408"

#define GPIO_PATH "/sys/class/gpio"
#define GPIO_EXPORT "/sys/class/gpio/export"
#define GPIO_UNEXPORT "/sys/class/gpio/unexport"
#define GPIO_PIN_1 "516"
#define GPIO_PIN_2 "527"

#include "Logger.h"


// 定义一个新的结构体来存储星点的信息和电调位置
struct StarWithFocuserPosition {
    double x;  // 星点的 x 坐标
    double y;  // 星点的 y 坐标
    double HFR;  // 星点的 HFR 值
    int focuserPosition;  // 当前电调位置
};

class MainWindow : public QObject
{
    Q_OBJECT

public:
    explicit MainWindow(QObject *parent = nullptr);
    ~MainWindow();

    void getHostAddress();

    void initINDIClient();
    void initINDIServer();

    void initGPIO();

    void exportGPIO(const char* pin);

    void setGPIODirection(const char* pin, const char* direction);

    void setGPIOValue(const char* pin, const char* value);

    int readGPIOValue(const char* pin);

    void getGPIOsStatus();

    // QString connectIndiServer();
    // void disconnectIndiServer();
    // void connectDevice(int x);

    void readDriversListFromFiles(const std::string &filename, DriversList &drivers_list_from,
                              std::vector<DevGroup> &dev_groups_from, std::vector<Device> &devices_from);
    void printDevGroups2(const DriversList drivers_list, int ListNum, QString group);

    void DeviceSelect(int systemNumber,int grounpNumber);
    void SelectIndiDevice(int systemNumber,int grounpNumber);

    bool indi_Driver_Confirm(QString DriverName);
    bool indi_Driver_Clear();
    void indi_Device_Confirm(QString DeviceName, QString DriverName);

    uint32_t clearCheckDeviceExist(QString drivername,bool &isExist);

    void ConnectAllDeviceOnce();
    void continueConnectAllDeviceOnce();
    void AutoConnectAllDevice();
    void continueAutoConnectAllDevice();
    void DeviceConnect();
    void disconnectDriver(QString Driver);
    void disconnectDevice(const QString& deviceName, const QString& description);
    void BindingDevice(QString DeviceType, int DeviceIndex);
    void UnBindingDevice(QString DeviceType);
    void AfterDeviceConnect();
    void AfterDeviceConnect(INDI::BaseDevice *dp);
    void disconnectIndiServer(MyClient *client);
    bool connectIndiServer(MyClient *client);

    void ClearSystemDeviceList();
    void ConnectDriver(QString DriverName,QString DriverType);
    void DisconnectDevice(MyClient *client,QString DeviceName,QString DeviceType);
    void initDeviceList();
    void loadSelectedDriverList();
    void loadBindDeviceList(MyClient *client);
    void loadBindDeviceTypeList();

    QVector<int> ConnectedCCDList;
    QVector<int> ConnectedTELESCOPEList;
    QVector<int> ConnectedFOCUSERList;
    QVector<int> ConnectedFILTERList;
    QVector<QString> ConnectDriverList;

    //ms
    void INDI_Capture(int Exp_times);

    void INDI_AbortCapture();

    void FocusingLooping();

    void saveFitsAsJPG(QString filename, bool ProcessBin);

    int saveFitsAsPNG(QString fitsFileName, bool ProcessBin);

    void saveGuiderImageAsJPG(cv::Mat Image);

    cv::Mat colorImage(cv::Mat img16);

    void refreshGuideImage(cv::Mat image16,QString CFA);

    void strechShowImage(cv::Mat img16,QString CFA,bool AutoStretch,bool AWB,int AutoStretchMode,uint16_t blacklevel,uint16_t whitelevel,double ratioRG,double ratioBG,uint16_t offset,bool updateHistogram);

    void InitPHD2();

    bool connectPHD(void);

    bool call_phd_GetVersion(QString &versionName);

    uint32_t call_phd_StartLooping(void);

    uint32_t call_phd_StopLooping(void);

    uint32_t call_phd_AutoFindStar(void);

    uint32_t call_phd_StartGuiding(void);

    uint32_t call_phd_checkStatus(unsigned char &status);

    uint32_t call_phd_setExposureTime(unsigned int expTime);

    uint32_t call_phd_whichCamera(std::string Camera);

    uint32_t call_phd_ChackControlStatus(int sdk_num);

    uint32_t call_phd_ClearCalibration(void);

    uint32_t call_phd_StarClick(int x, int y);

    uint32_t call_phd_FocalLength(int FocalLength);

    uint32_t call_phd_MultiStarGuider(bool isMultiStar);

    uint32_t call_phd_CameraPixelSize(double PixelSize);

    uint32_t call_phd_CameraGain(int Gain);

    uint32_t call_phd_CalibrationDuration(int StepSize);

    uint32_t call_phd_RaAggression(int Aggression);

    uint32_t call_phd_DecAggression(int Aggression);

    void ShowPHDdata();

    void ControlGuide(int Direction, int Duration);

    void GetPHD2ControlInstruct();

    void getTimeNow(int index);

    int glExpTime = 1000;

    bool one_touch_connect = true;
    bool one_touch_connect_first = true;
    int glMainCCDSizeX = 0;
    int glMainCCDSizeY = 0;

    int glOffsetValue = 0, glOffsetMin = 0, glOffsetMax = 0;
    int glGainValue = 0, glGainMin = 0, glGainMax = 0;

    bool glIsFocusingLooping;
    QString glMainCameraStatu;
    QElapsedTimer glMainCameraCaptureTimer;

    // std::string vueDirectoryPath = "/home/quarcs/workspace/QUARCS/QUARCS_stellarium-web-engine/apps/web-frontend/dist/img/";
    std::string vueDirectoryPath = "/dev/shm/";
    // std::string vueImagePath = "/home/quarcs/workspace/QUARCS/QUARCS_stellarium-web-engine/apps/web-frontend/dist/img/";  // /var/www/html/img/
    std::string vueImagePath = "/var/www/html/img/";
    std::string PriorGuiderImage = "NULL";
    std::string PriorROIImage = "NULL";
    std::string PriorCaptureImage = "NULL";

    bool AutoStretch = true;

    QProcess* cmdPHD2;
    int key_phd;
    int shmid_phd;
    bool isGuideCapture = true;
    // #define BUFSZ_PHD 16590848 33554432
    #define BUFSZ_PHD 16590848
    char *sharedmemory_phd;

    char phd_direction;
    int phd_step;
    double phd_dist;

    QVector<QPoint> glPHD_Stars;
    QVector<QPointF> glPHD_rmsdate;

    bool glPHD_isSelected;
    double glPHD_StarX = 0;
    double glPHD_StarY = 0;
    int glPHD_CurrentImageSizeX = 0;
    int glPHD_CurrentImageSizeY = 0;
    double glPHD_LockPositionX;
    double glPHD_LockPositionY;
    bool glPHD_ShowLockCross;
    bool glPHD_StartGuide = false;

    bool ClearCalibrationData = true;
    bool isGuiding = false;
    bool isGuiderLoopExp = false;

    double glROI_x = 0;        // ROI的起始x坐标
    double glROI_y = 0;        // ROI的起始y坐标
    // int CaptureViewWidth = 0;
    // int CaptureViewHeight = 0;
    int BoxSideLength = 300;
    
    double FWHM = 0;

    bool InGuiding = false;

    QString TelescopePierSide;

    bool FirstRecordTelescopePierSide = true;
    QString FirstTelescopePierSide;

    bool isMeridianFlipped = false;

    QThread *PHDControlGuide_thread = nullptr;
    QTimer *PHDControlGuide_threadTimer = nullptr;

    std::mutex receiveMutex;

    Q_SLOT void onPHDControlGuideTimeout();

    QThread* m_thread = nullptr;
    QTimer* m_threadTimer = nullptr;

    Q_SLOT void onTimeout();

    int currentSpeed = 3;
    int currentSteps = 5000;
    int CurrentPosition = 0;
    int TargetPosition = 0;
    bool MoveInward = true;
    int AutoMovePosition;

    bool FWHMCalOver = false;

    float minPoint_X;

    double FocusMoveAndCalHFR(bool isInward, int steps);
    double FocusGotoAndCalFWHM(int steps);

    bool currentDirection = true;   // 标志电调旋转方向 true: inward, false: outward
    double focusMoveEndTime = 0;     // 用来控制电调移动时，因为浏览器关闭或刷新或网络卡死导致的结束命令丢失超时
    QTimer* focusMoveTimer = nullptr;   // 用于控制焦距移动的定时器，每500ms执行一次步数的设置
    bool isFocusMoveDone = false;   // 用于标记电调是否在移动，全局标志电调是否在移动
    void FocuserControlMove(bool isInward); //控制电调移动
    void HandleFocuserMovementDataPeriodically(); //重置定时器，并更新电调移动参数，
    void FocuserControlStop(); //停止电调移动
    QTimer* updatePositionTimer = nullptr; //用于更新电调位置的定时器，仅在电调移动停止时使用，当电调移动时停止
    int updateCount = 0; //用于更新电调位置的计数器，仅在电调移动停止时使用，当电调移动时停止
    void CheckFocuserMoveOrder(); //检查电调移动命令是否正常
    bool isFocusLoopShooting = false; //控制ROi循环拍摄
    void focusLoopShooting(bool isLoop); //控制ROi循环拍摄
    void getFocuserLoopingState(); //获取ROi循环拍摄状态
    std::pair<int,double> selectStar(QList<FITSImage::Star> stars); //用于选择用于计算自动对焦的星点
    // 用于同步ROI的信息
    std::map<std::string, double> roiAndFocuserInfo; // 用于存储ROI信息
    // roiAndFocuserInfo["ROI_x"] = 0;// ROI的x坐标,是左上角坐标,参考系是原大小的图像
    // roiAndFocuserInfo["ROI_y"] = 0; // ROI的y坐标,是左上角坐标,参考系是原大小的图像
    // roiAndFocuserInfo["BoxSideLength"] = 300; // ROI的边长,单位是像素,参考系是原大小的图像
    // roiAndFocuserInfo["VisibleX"] = 0;      // 可见区域的x坐标,是中心点坐标,参考系是原大小的图像
    // roiAndFocuserInfo["VisibleY"] = 0;      // 可见区域的y坐标,是中心点坐标,参考系是原大小的图像
    // roiAndFocuserInfo["Scale"] = 1;        // 缩放比例,1为全图以宽为基准的全部显示,0.1是全图以宽为基准的10%显示
    // roiAndFocuserInfo["SelectStarX"] = -1; // 选择的星点的x坐标,是中心点坐标,参考系是全图的图像
    // roiAndFocuserInfo["SelectStarY"] = -1; // 选择的星点的y坐标,是中心点坐标,参考系是全图的图像
    std::pair<int,double> currentSelectStarPosition;    // 用于存储当前选择的星点位置
    std::vector<std::pair<int,double>> currentAutoFocusStarPositionList; // 用于存储当前自动对焦的星点位置
    std::vector<std::pair<int,double>> allAutoFocusStarPositionList;   // 用于存储所有拟合曲线的的星点位置
    int overSelectStarAutoFocusStep = 0;  // 用于结束使用选择的星点进行自动对焦
    void AutoFocus(std::pair<int,double> selectStarPosition); // 自动对焦处理逻辑
    int autoFocusStep = 0;
    void sendRoiInfo();   // 用于发送ROI信息
    

    QTimer FWHMTimer; 

    QString MainCameraCFA;

    double ImageGainR = 1.0;
    double ImageGainB = 1.0;
    double ImageOffset = 0.0;
    QVector<QPointF> dataPoints;    // FWHM Data

    double R2;

    // void AutoFocus();
    bool isAutoFocus = false;
    bool StopAutoFocus = false;

    void FocuserControl_Goto(int position);

    void FocuserControl_Move(bool isInward, int steps);

    int  FocuserControl_setSpeed(int speed);
    int  FocuserControl_getSpeed();

    int FocuserControl_getPosition();


    double observatorylongitude=-1;  // 观测站经度,自动翻转使用
    double observatorylatitude =-1; // 观测站纬度,自动翻转使用
    bool needsMeridianFlip(double lst,double targetRA);
    // 执行观测流程
    // void performObservation(
    //     double currentRA, double currentDec,
    //     double targetRA, double targetDec,
    //     double observatoryLongitude,double observatoryLatitude);

    void performObservation(double lst,double currentDec,double targetRA,double targetDec,double observatoryLongitude,double observatoryLatitude);
    double getJulianDate(const std::chrono::system_clock::time_point& utc_time);
    double computeGMST(const std::chrono::system_clock::time_point& utc_time);
    double computeLST(double longitude_east, const std::chrono::system_clock::time_point& utc_time);

    void TelescopeControl_Goto(double Ra,double Dec);

    MountStatus TelescopeControl_Status();

    bool TelescopeControl_Park();

    bool TelescopeControl_Track();

    void TelescopeControl_Home();

    void TelescopeControl_SYNCHome();

    void TelescopeControl_SolveSYNC();

    LocationResult TelescopeControl_GetLocation();

    QDateTime TelescopeControl_GetTimeUTC();

    SphericalCoordinates TelescopeControl_GetRaDec();

    void SolveImage(QString Filename, int FocalLength, double CameraWidth, double CameraHeight);

    void LoopSolveImage(QString Filename, int FocalLength, double CameraWidth, double CameraHeight);

    void LoopCapture(int ExpTime);

    void CaptureAndSolve(int ExpTime, bool isLoop);

    bool EndCaptureAndSolve = false;

    bool TakeNewCapture = true;

    void ScheduleTabelData(QString message);

    bool isLoopSolveImage = false;

    bool isSingleSolveImage = false;

    int SolveImageHeight = 0;
    int SolveImageWidth = 0;

    QString SolveImageFileName = "/dev/shm/SolveImage.tiff";

    std::vector<SloveResults> SloveResultList;

    void ClearSloveResultList();

    void RecoverySloveResul();

    int glFocalLength = 0;
    double glCameraSize_width;
    double glCameraSize_height;

    int glTelescopeTotalSlewRate;

    QList<ScheduleData> m_scheduList;

    QTimer telescopeTimer;
    QTimer guiderTimer;
    QTimer captureTimer;
    QTimer timewaitingTimer;
    QTimer filterTimer;
    QTimer focusTimer;
    QTimer solveTimer;

    int schedule_currentNum = 0;
    int schedule_ExpTime;
    int schedule_CFWpos;
    int schedule_RepeatNum;
    int schedule_currentShootNum = 0;

    bool InSlewing;
    bool GuidingHasStarted = false;
    QString ShootStatus;

    bool StopSchedule = false;
    bool StopPlateSolve = false;

    bool MountGotoError = false;

    void startSchedule();

    void nextSchedule();

    void startMountGoto(double ra, double dec);       // Ra:Hour, Dec:Degree

    void startGuiding();

    void startTimeWaiting();

    void startCapture(int ExpTime);

    void startSetCFW(int pos);

    bool WaitForTelescopeToComplete();

    bool WaitForShootToComplete();

    bool WaitForGuidingToComplete();

    bool WaitForTimeToComplete();

    bool WaitForFocuserToComplete();

    int ScheduleImageSave(QString name, int num);

    int CaptureImageSave();

    QString ScheduleTargetNames;

    // std::string ImageSaveBasePath = "/home/quarcs/QUARCS_SaveImage/";
    // QString ImageSaveBaseDirectory = "/home/quarcs/QUARCS_SaveImage/";
    std::string ImageSaveBasePath = "image";
    QString ImageSaveBaseDirectory = "image";

    bool directoryExists(const std::string& path);

    bool createScheduleDirectory();

    bool createCaptureDirectory();

     

    QVector<ConnectedDevice> ConnectedDevices;

    // int NumberOfTimesConnectDevice = 0;

    void getConnectedDevices();

    void getClientSettings();

    void setClientSettings(QString ConfigName, QString ConfigValue);

    void clearConnectedDevices();

    bool isStagingImage = false;
    std::string SavedImage;

    bool isFileExists(const QString& filePath);

    void getStagingImage();

    bool isStagingScheduleData = false;
    QString StagingScheduleData;

    void getStagingScheduleData();

    void getStagingGuiderData();

    QElapsedTimer ImageSolveTimer;
    qint64 ImageSolveTime;


    int MoveFileToUSB();

    int mountDisplayCounter = 0;

    int MainCameraStatusCounter = 0;

    int glMainCameraBinning = 1;

    bool isFilterOnCamera = false;

    bool isFirstCapture = true;

    double glCurrentLocationLat = 0;
    double glCurrentLocationLng = 0; 

    double LastRA_Degree = 0;
    double LastDEC_Degree = 0;

    void MountGoto(double Ra_Hour, double Dec_Degree);


    // void CaptureImageSave();
    void DeleteImage(QStringList DelImgPath);
    std::string GetAllFile();
    QStringList parseString(const std::string &input, const std::string &imgFilePath);
    long long getUSBSpace(const QString &usb_mount_point);
    long long getTotalSize(const QStringList &filePaths);
    void RemoveImageToUsb(QStringList RemoveImgPath);
    bool isMountReadOnly(const QString& mountPoint);
    bool remountReadWrite(const QString& mountPoint, const QString& password);

    void GetImageFiles(std::string ImageFolder);

    void USBCheck();

    PlateSolveWorker *platesolveworker = new PlateSolveWorker;

    
    QVector<QString> INDI_Driver_List;

    void editHotspotName(QString newName);

    QString getHotspotName();

    // 非静态函数，用于处理 qDebug 信息
    void SendDebugToVueClient(const QString &msg);

    DeviceType getDeviceTypeFromPartialString(const std::string& typeStr);

    // 静态函数，用于安装自定义的消息处理器
    static void customMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg);

    // 添加日志缓存区
    std::vector<std::string> logCache;  // 用于存储日志信息的缓存区

    QStringList getConnectedSerialPorts();  // 获取串口通信列表
    QString resolveSerialPort(const QString& symbolicLink);  // 解析串口通信路径
    QStringList findLinkToTtyDevice(const QString& directoryPath, const QString& ttyDevice);  // 查找指向tty设备的符号链接列表
    bool areFilesInSameDirectory(const QString& path1, const QString& path2);  // 检查两个文件是否在同一目录下

    void loadParameters();

    void handleIndiServerOutput();
    void handleIndiServerError();


    double DEC = -1;
    double RA = -1;
private slots:
    void onMessageReceived(const QString &message);
    // void sendMessage(QString message);
    void onParseInfoEmitted(const QString& message);
private:
    // WebSocketClient *websocket;
    WebSocketThread *wsThread;

    MyClient *indi_Client;
    QProcess *glIndiServer;

    static MainWindow *instance;
};

#endif // MAINWINDOW_H
