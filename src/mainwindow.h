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

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include <stellarsolver.h>

#include "platesolveworker.h"

#include <regex>

#include <thread> // 确保包含此头文件
#include <chrono> // 包含用于时间的头文件

#define QT_Client_Version "20250115"

#define GPIO_PATH "/sys/class/gpio"
#define GPIO_EXPORT "/sys/class/gpio/export"
#define GPIO_UNEXPORT "/sys/class/gpio/unexport"
#define GPIO_PIN_1 "516"
#define GPIO_PIN_2 "527"

class MainWindow : public QObject
{
    Q_OBJECT

public:
    explicit MainWindow(QObject *parent = nullptr);
    ~MainWindow();

    //获取IP地址
    void getHostAddress();

    //初始化INDI客户端
    void initINDIClient();
    //初始化IND服务端
    void initINDIServer();

    //初始化树莓派IO口
    void initGPIO();

    // 导出 GPIO 引脚
    void exportGPIO(const char* pin);

    // 设置 GPIO 方向
    void setGPIODirection(const char* pin, const char* direction);

    // 设置 GPIO 电平
    void setGPIOValue(const char* pin, const char* value);

    // 读取 GPIO 电平
    int readGPIOValue(const char* pin);

    // 获取 GPIO 状态
    void getGPIOsStatus();

    // QString connectIndiServer();
    // void disconnectIndiServer();
    // void connectDevice(int x);

    // 从文件中读取驱动列表
    void readDriversListFromFiles(const std::string &filename, DriversList &drivers_list_from,
                              std::vector<DevGroup> &dev_groups_from, std::vector<Device> &devices_from);
    // 打印驱动列表
    void printDevGroups2(const DriversList drivers_list, int ListNum, QString group);

    // 选取驱动
    void DeviceSelect(int systemNumber,int grounpNumber);
    // 选取INDI 设备
    void SelectIndiDevice(int systemNumber,int grounpNumber);

    // 确认INDI驱动（将驱动名填入systemdevicelist列表中）
    bool indi_Driver_Confirm(QString DriverName);
    // 清空systemdevicelist列表中的驱动
    bool indi_Driver_Clear();
    // 确认INDI设备（将设备名填入systemdevicelist列表中）
    void indi_Device_Confirm(QString DeviceName, QString DriverName);

    // 验证设备是否存在
    uint32_t clearCheckDeviceExist(QString drivername,bool &isExist);

    // 一次性连接所有所选设备
    void ConnectAllDeviceOnce();
    // 自动连接上一次所连接的设备
    void AutoConnectAllDevice();

    // 将设备绑定到dp指针
    void BindingDevice(QString DeviceType, int DeviceIndex);
    // 解除设备与dp指针的绑定
    void UnBindingDevice(QString DeviceType);
    // 设备连接完后，需要对设备进行一些参数设置
    void AfterDeviceConnect();
    // 设备连接完后，需要对设备进行一些参数设置
    void AfterDeviceConnect(INDI::BaseDevice *dp);
    // 断开INDI服务端的连接
    void disconnectIndiServer(MyClient *client);
    // 连接INDI服务端
    void connectIndiServer(MyClient *client);

    // 清空systemdevicelist列表
    void ClearSystemDeviceList();

    QVector<int> ConnectedCCDList;  // 已连接的相机设备列表
    QVector<int> ConnectedTELESCOPEList;// 已连接的赤道仪设备列表
    QVector<int> ConnectedFOCUSERList;// 已连接的电调设备列表
    QVector<int> ConnectedFILTERList;// 已连接的滤镜轮设备列表

    // 主相机拍摄（单位ms）
    void INDI_Capture(int Exp_times);

    // 终止主相机拍摄
    void INDI_AbortCapture();

    // 主相机循环拍摄（用于调焦）
    void FocusingLooping();

    // 将Fits图保存为JPG格式
    void saveFitsAsJPG(QString filename);

    // 将Fits图保存为PNG格式
    int saveFitsAsPNG(QString fitsFileName, bool ProcessBin);

    // 将导星相机（PHD2）拍摄的图像保存为JPG格式
    void saveGuiderImageAsJPG(cv::Mat Image);

    // 初始化PHD2
    void InitPHD2();

    // 通过共享内存连接PHD2
    bool connectPHD(void);

    // 通过共享内存获取PHD2版本信息
    bool call_phd_GetVersion(QString &versionName);

    // 开始PHD2循环拍摄
    uint32_t call_phd_StartLooping(void);

    // 停止PHD2循环拍摄
    uint32_t call_phd_StopLooping(void);

    // 自动选取导星目标
    uint32_t call_phd_AutoFindStar(void);

    // 开始导星
    uint32_t call_phd_StartGuiding(void);

    // 获取PHD2导星的状态
    uint32_t call_phd_checkStatus(unsigned char &status);

    // 设置PHD2中导星镜的曝光时间
    uint32_t call_phd_setExposureTime(unsigned int expTime);

    // PHD2连接导星相机时通过相机名字选取相机
    uint32_t call_phd_whichCamera(std::string Camera);

    // 确定导星赤道仪控制状态
    uint32_t call_phd_ChackControlStatus(int sdk_num);

    // 清除导星校准数据
    uint32_t call_phd_ClearCalibration(void);

    // 手动选取导星目标（发送目标在星图中的坐标）
    uint32_t call_phd_StarClick(int x, int y);

    // 设置导星镜的焦距
    uint32_t call_phd_FocalLength(int FocalLength);

    // 多星导星开关
    uint32_t call_phd_MultiStarGuider(bool isMultiStar);

    // 设置导星镜像素尺寸大小
    uint32_t call_phd_CameraPixelSize(double PixelSize);

    // 设置导星镜的增益
    uint32_t call_phd_CameraGain(int Gain);

    // 设置校准时的持续时长
    uint32_t call_phd_CalibrationDuration(int StepSize);

    // 设导星算法中的赤经修正强度
    uint32_t call_phd_RaAggression(int Aggression);

    // 设导星算法中的赤纬修正强度
    uint32_t call_phd_DecAggression(int Aggression);

    // 获取导星过程中产生的数据
    void ShowPHDdata();

    // 通过从PHD2获取到的数据控制赤道仪进行导星
    void ControlGuide(int Direction, int Duration);

    // 获取导星的控制指令
    void GetPHD2ControlInstruct();

    // 获取当前时间
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
    // 主相机拍摄得到的图像存储路径
    std::string vueImagePath = "/home/quarcs/workspace/QUARCS/QUARCS_stellarium-web-engine/apps/web-frontend/dist/img/";  // /var/www/html/img/

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

    int glROI_x;
    int glROI_y;
    int CaptureViewWidth;
    int CaptureViewHeight;
    int BoxSideLength = 500;
    
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

    // 主循环信号
    Q_SLOT void onTimeout();

    int currentSpeed = 3;
    int currentSteps = 5000;
    int CurrentPosition = 0;
    int TargetPosition = 0;
    bool MoveInward = true;
    int AutoMovePosition;

    bool FWHMCalOver = false;

    float minPoint_X;

    // 电调移动并计算拍摄的ROI图像中星点大小
    double FocusMoveAndCalHFR(bool isInward, int steps);
    // 电调Goto并计算拍摄的ROI图像中星点大小
    double FocusGotoAndCalFWHM(int steps);

    QTimer FWHMTimer; 

    QString MainCameraCFA;

    double ImageGainR = 1.0;
    double ImageGainB = 1.0;

    QVector<QPointF> dataPoints;    // FWHM Data

    double R2;

    // 自动调焦
    void AutoFocus();

    bool StopAutoFocus = false;

    // 控制电调Goto
    void FocuserControl_Goto(int position);

    // 控制电调移动
    void FocuserControl_Move(bool isInward, int steps);

    // 设置电调转动速度
    int  FocuserControl_setSpeed(int speed);
    // 获取电调转动速度
    int  FocuserControl_getSpeed();

    // 获取电调当前位置
    int FocuserControl_getPosition();

    // 控制赤道仪Goto到目标位置
    void TelescopeControl_Goto(double Ra,double Dec);

    // 获取赤道仪当前状态
    MountStatus TelescopeControl_Status();

    // 赤道仪Park
    bool TelescopeControl_Park();

    // 赤道仪跟踪开关
    bool TelescopeControl_Track();

    // 解析图像并将解析结果同步给赤道仪
    void TelescopeControl_SolveSYNC();

    // 获取赤道仪内部的定位
    LocationResult TelescopeControl_GetLocation();

    // 获取赤道仪内部的时间
    QDateTime TelescopeControl_GetTimeUTC();

    // 获取赤道仪当前的RaDec值
    SphericalCoordinates TelescopeControl_GetRaDec();

    // 图像解析
    void SolveImage(QString Filename, int FocalLength, double CameraWidth, double CameraHeight);

    // 循环拍摄时的图像解析
    void LoopSolveImage(QString Filename, int FocalLength, double CameraWidth, double CameraHeight);

    // 主相机拍摄并解析
    void CaptureAndSolve(int ExpTime, bool isLoop);

    bool EndCaptureAndSolve = false;

    bool TakeNewCapture = true;

    // 计划任务表数据
    void ScheduleTabelData(QString message);

    bool isLoopSolveImage = false;

    bool isSingleSolveImage = false;

    int SolveImageHeight = 0;
    int SolveImageWidth = 0;

    QString SolveImageFileName = "/dev/shm/SolveImage.tiff";

    std::vector<SloveResults> SloveResultList;

    // 清理解析结果
    void ClearSloveResultList();

    // 恢复解析结果
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

    // 计划任务表开始执行
    void startSchedule();

    // 开始执行下一个计划任务
    void nextSchedule();

    // 开始执行赤道仪Goto
    void startMountGoto(double ra, double dec);       // Ra:Hour, Dec:Degree

    // 开始执行导星
    void startGuiding();

    // 如果设定了具体执行时间，则将等待至具体设置时间再继续执行
    void startTimeWaiting();

    // 开始拍摄
    void startCapture(int ExpTime);

    // 设置滤镜轮
    void startSetCFW(int pos);

    // 判断赤道仪Goto是否完毕
    bool WaitForTelescopeToComplete();

    // 判断相机拍摄是否完毕
    bool WaitForShootToComplete();

    // 判断导星是否完毕
    bool WaitForGuidingToComplete();

    // 判断是否到了具体执行时间
    bool WaitForTimeToComplete();

    // 判断调焦是否完毕
    bool WaitForFocuserToComplete();

    // 保存计划任务表中拍摄的图像
    int ScheduleImageSave(QString name, int num);

    // 拍摄图像保存
    int CaptureImageSave();

    QString ScheduleTargetNames;

    // std::string ImageSaveBasePath = "/home/quarcs/QUARCS_SaveImage/";
    // QString ImageSaveBaseDirectory = "/home/quarcs/QUARCS_SaveImage/";
    std::string ImageSaveBasePath = "image";
    QString ImageSaveBaseDirectory = "image";

    // 判断文件是否存在
    bool directoryExists(const std::string& path);

    // 创建计划任务表存储图像的文件夹
    bool createScheduleDirectory();

    // 创建手动拍摄时存储图像的文件夹
    bool createCaptureDirectory();

     

    QVector<ConnectedDevice> ConnectedDevices;

    // int NumberOfTimesConnectDevice = 0;

    // 获取已连接的设备
    void getConnectedDevices();

    // 获取客户端配置信息
    void getClientSettings();

    // 设置客户端配置
    void setClientSettings(QString ConfigName, QString ConfigValue);

    // 清除存储已连接的设备的设备列表
    void clearConnectedDevices();

    bool isStagingImage = false;
    QString SavedImage;

    // 获取前一次拍摄的图像
    void getStagingImage();

    bool isStagingScheduleData = false;
    QString StagingScheduleData;

    // 获取之前配置的计划任务表信息
    void getStagingScheduleData();

    // 获取之前的导星信息
    void getStagingGuiderData();

    QElapsedTimer ImageSolveTimer;
    qint64 ImageSolveTime;


    // 将文件移动至USB设备
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

    // 赤道仪Goto
    void MountGoto(double Ra_Hour, double Dec_Degree);


    // void CaptureImageSave();
    void DeleteImage(QStringList DelImgPath);
    std::string GetAllFile();
    QStringList parseString(const std::string &input, const std::string &imgFilePath);
    // 获取USB设备剩余空间
    long long getUSBSpace(const QString &usb_mount_point);
    // 获取USB设备总的空间大小
    long long getTotalSize(const QStringList &filePaths);
    // 移动图像文件到USB设备
    void RemoveImageToUsb(QStringList RemoveImgPath);
    // 获取文件系统挂载模式
    bool isMountReadOnly(const QString& mountPoint);
    // 将文件系统挂载模式更改为读写模式
    bool remountReadWrite(const QString& mountPoint, const QString& password);
    // 获取图像文件
    void GetImageFiles(std::string ImageFolder);
    // 检查USB设备
    void USBCheck();

    PlateSolveWorker *platesolveworker = new PlateSolveWorker;

    
    QVector<QString> INDI_Driver_List;

    // 编辑树莓派热点名称
    void editHotspotName(QString newName);

    // 获取树莓派热点名称
    QString getHotspotName();

    // 非静态函数，用于处理 qDebug 信息
    void SendDebugToVueClient(const QString &msg);

    // 静态函数，用于安装自定义的消息处理器
    static void customMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg);

private slots:
    void onMessageReceived(const QString &message);
    // void sendMessage(QString message);

private:
    // WebSocketClient *websocket;
    WebSocketThread *wsThread;

    MyClient *indi_Client;
    QProcess *glIndiServer;

    static MainWindow *instance;
};

#endif // MAINWINDOW_H
