#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QObject>
#include <QThread>
#include <QProcess>
#include <fitsio.h>
// #include "websocketclient.h"
#include "myclient.h"
#include <QFile>
#include "tools.h"
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

#include <regex>

#include <thread> // 确保包含此头文件
#include <chrono> // 包含用于时间的头文件
#include <cmath>  // 包含数学函数
#include <set>
#include <unordered_set>
#include <filesystem>
#include <math.h>
namespace fs = std::filesystem;

#define QT_Client_Version getBuildDate()

#define GPIO_PATH "/sys/class/gpio"
#define GPIO_EXPORT "/sys/class/gpio/export"
#define GPIO_UNEXPORT "/sys/class/gpio/unexport"
#define GPIO_PIN_1 "516"
#define GPIO_PIN_2 "527"
#define BIN_SIZE 20 // 
#include "Logger.h"
#include "autopolaralignment.h"
#include "autofocus.h"


// 定义一个新的结构体来存储星点的信息和电调位置
struct StarList {
    int id = -1; // 星星的编号
    double x = 0, y = 0; // 星星的坐标
    double hfr = 0; // 星星的半高全宽
    int vector1_id = -1, vector2_id = -1, vector3_id = -1; // 向量1，表示方向和距离
    QPointF vector1 = QPointF(0, 0); // 向量1，表示方向和距离
    QPointF vector2 = QPointF(0, 0); // 向量2，表示方向和距离
    QPointF vector3 = QPointF(0, 0); // 向量3，表示方向和距离
    QString status = "wait"; // 星星的状态
    int focuserPosition = 0;  // 当前电调位置
};

// 定义一个用来判断星点匹配度的结构体
struct StarMatch{
    int id = -1; // 星星的编号
    int vector1_id = -1, vector2_id = -1, vector3_id = -1; // 向量1，2，3的编号
    bool isMatch1 = false, isMatch2 = false, isMatch3 = false; // 是否匹配
};

class MainWindow : public QObject
{
    Q_OBJECT

public:
    explicit MainWindow(QObject *parent = nullptr);
    ~MainWindow();
    QTimer *system_timer = nullptr;
    void bin_image(double* input, long width, long height, double* output, long* out_w, long* out_h);
    void process_hdu(fitsfile* infptr, fitsfile* outfptr, int hdunum, int* status);
    int process_fixed() ;//20*20xbinning
    
    void updateCPUInfo();

    std::string getBuildDate();

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

    bool indi_Driver_Confirm(QString DriverName, QString BaudRate);
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
    double CameraTemperature = 16;
    int glOffsetValue = 0, glOffsetMin = 0, glOffsetMax = 0;
    int glGainValue = 0, glGainMin = 0, glGainMax = 0;

    bool glIsFocusingLooping;
    QString glMainCameraStatu;
    QElapsedTimer glMainCameraCaptureTimer;

    // std::string vueDirectoryPath = "/home/quarcs/workspace/QUARCS/QUARCS_stellarium-web-engine/apps/web-frontend/dist/img/";
    std::string vueDirectoryPath = "/dev/shm/";
    std::string vueImagePath = "/home/quarcs/workspace/QUARCS/QUARCS_stellarium-web-engine/apps/web-frontend/dist/img/";  // /var/www/html/img/
    // std::string vueImagePath = "/var/www/html/img/";
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
    int startPosition = 0;  // 电调开始位置，用于计算电调移动的步数
    int CurrentPosition = 0;   // 电调当前位置
    int TargetPosition = 0;   // 电调要移动到的位置
    bool MoveInward = true;
    int AutoMovePosition;

    bool FWHMCalOver = false;

    float minPoint_X;


    bool currentDirection = true;   // 标志电调旋转方向 true: inward, false: outward
    double focusMoveEndTime = 0;     // 用来控制电调移动时，因为浏览器关闭或刷新或网络卡死导致的结束命令丢失超时
    QTimer* focusMoveTimer = nullptr;   // 用于控制焦距移动的定时器，每500ms执行一次步数的设置
    bool isFocusMoveDone = false;   // 用于标记电调是否在移动，全局标志电调是否在移动
    int focuserMaxPosition = -1;
    int focuserMinPosition = -1;
    void FocuserControlMove(bool isInward); //控制电调移动
    void HandleFocuserMovementDataPeriodically(); //重置定时器，并更新电调移动参数，
    void FocuserControlStop(bool isClickMove = false); //停止电调移动
    QTimer* updatePositionTimer = nullptr; //用于更新电调位置的定时器，仅在电调移动停止时使用，当电调移动时停止
    int updateCount = 0; //用于更新电调位置的计数器，仅在电调移动停止时使用，当电调移动时停止
    void CheckFocuserMoveOrder(); //检查电调移动命令是否正常
    bool isFocusLoopShooting = false; //控制ROi循环拍摄
    void focusLoopShooting(bool isLoop); //控制ROi循环拍摄
    void getFocuserLoopingState(); //获取ROi循环拍摄状态
    QPointF selectStar(QList<FITSImage::Star> stars); //用于选择用于计算自动对焦的星点
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
    // roiAndFocuserInfo["SelectStarHFR"] = -1; // 选择的星点的HFR值
    QPointF currentSelectStarPosition;    // 用于存储当前选择的星点位置
    QVector<QPointF> currentAutoFocusStarPositionList; // 用于存储当前自动对焦的星点位置
    QVector<QPointF> allAutoFocusStarPositionList;   // 用于存储所有拟合曲线的的星点位置
    int overSelectStarAutoFocusStep = 0;  // 用于结束使用选择的星点进行自动对焦
    QVector<bool> isAutoFocusStarPositionList; // 用于存储没有星点的方向，用于自动对焦
    // void AutoFocus(QPointF selectStarPosition); // 自动对焦处理逻辑
    void startAutoFocus();
    AutoFocus *autoFocus = nullptr;
    int autoFocusStep = 0; // 用于存储自动对焦的步数
    bool autoFocuserIsROI = false; // 用于存储自动对焦是否使用ROI
    void sendRoiInfo();   // 用于发送ROI信息  
    // int fitQuadraticCurve(const QVector<QPointF>& data, float& a, float& b, float& c);  // 拟合二次曲线
    // std::vector<StarList> starMap; // 用于存储图信息
    // int updateStarMapPosition(QList<FITSImage::Star> stars); // 计算星图相对位置，为星点编号
    // double calculateDistance(double x1, double y1, double x2, double y2); // 
    // void compareStarVector(QList<FITSImage::Star> stars); // 匹配星点和星图
    // double calculateMatchScore(const FITSImage::Star& currentStar, const StarList& referenceStar, const QList<FITSImage::Star>& allStars);
    // double findNearestStar(const QList<FITSImage::Star>& stars, const QPointF& position);
    // double getAdaptiveThreshold();
    // double getMinMatchScore();
    // int findBestStar();
    // void calculateStarVector(); // 计算星点之间的向量
    // int selectStarInStarMapId = -1; // 用于存储选择的星点在星图中的编号
    // bool NewSelectStar = true; // 用于标记是否选择新的星点
    // int starMapLossNum = 0; // 用于存储星图丢失的次数
    void focusMoveToMin(); // 移动到最小位置
    void focusMoveToMax(); // 移动到最大位置
    void focusSetTravelRange(); // 设置电调行程范围
    int lastPosition = 0; // 用于存储上一次的电调位置
    QTimer* focusMoveToMaxorMinTimer = nullptr; // 用于控制焦距移动的定时器，每500ms执行一次步数的设置

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

    bool EndCaptureAndSolve = false;

    bool TakeNewCapture = true;

    bool isSavePngSuccess = false;

    void ScheduleTabelData(QString message);

    bool isLoopSolveImage = false;

    bool isSingleSolveImage = false;

    int SolveImageHeight = 0;
    int SolveImageWidth = 0;

    QString SolveImageFileName = "/dev/shm/SolveImage.tiff";
    // QString SolveImageFileName ="/dev/shm/ccd_simulator.fits";
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
    int schedule_currentShootNum = 0;  // 用于存储当前拍摄的次数
    int expTime_ms = 0;  // 用于存储当前拍摄的时间

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


    void handleIndiServerOutput();
    void handleIndiServerError();

    void getMainCameraParameters();

    void synchronizeTime(QString time, QString date); // 同步系统时间

    double DEC = -1;
    double RA = -1;

    void setMountLocation(QString lat, QString lon);
    void setMountUTC(QString time, QString date);

    QString localLon = ""; // 本地经度
    QString localLat = "";  // 本地纬度
    QString localLanguage = ""; // 本地语言
    QString localTime = ""; // 本地时区

    void getLastSelectDevice();

    // 自动极轴校准
    bool initPolarAlignment();
    PolarAlignment *polarAlignment;
    

    
private slots:
    void onMessageReceived(const QString &message);
    // void sendMessage(QString message);
    void onParseInfoEmitted(const QString& message);
private:
    // WebSocketClient *websocket;
    WebSocketThread *wsThread;
    QUrl websockethttpUrl;
    QUrl websockethttpsUrl;

    MyClient *indi_Client;
    QProcess *glIndiServer;

    static MainWindow *instance;
};

#endif // MAINWINDOW_H
