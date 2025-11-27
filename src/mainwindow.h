#ifndef MAINWINDOW_H
#define MAINWINDOW_H

/**********************  标准/系统 & 第三方头文件  **********************/
#include <QObject>
#include <QThread>
#include <QProcess>
#include <QTimer>
#include <QElapsedTimer>
#include <QUrl>
#include <QVector>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QDateTime>
#include <QXmlStreamReader>
#include <QNetworkInterface>
#include <QStorageInfo>
#include <QtSerialPort/QSerialPortInfo>
#include <QSerialPort>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFileInfo>
#include <QDir>

#include <fitsio.h>

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <climits>

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <set>
#include <unordered_set>
#include <algorithm>
#include <regex>
#include <thread>
#include <chrono>
#include <cmath>
#include <math.h>
#include <QElapsedTimer>
#include <functional>

#include <filesystem>
#include <filesystem>  //（按原文件保留重复包含）
namespace fs = std::filesystem;

/**********************  工程内头文件（保留原顺序注释）  **********************/
// #include "websocketclient.h"
#include "myclient.h"
#include "tools.h"
#include "websocketthread.h"
#include "Logger.h"
#include "autopolaralignment.h"
#include "autofocus.h"
#include "SerialDeviceDetector.h"
#include <stellarsolver.h>

/**********************  宏与常量定义  **********************/
// #define QT_Client_Version getBuildDate()
#define QT_Client_Version "20251119"  // 手动指定版本号

#define GPIO_PATH "/sys/class/gpio"
#define GPIO_EXPORT "/sys/class/gpio/export"
#define GPIO_UNEXPORT "/sys/class/gpio/unexport"
#define GPIO_PIN_1 "516"
#define GPIO_PIN_2 "527"
#define BIN_SIZE 20 // 图像固定 bin 尺寸

// PHD 共享内存大小（原值保留）
#define BUFSZ_PHD 16590848

/**********************  前置类型（来自工程其它头）  **********************/
// 注：以下类型在工程其它头中定义，这里仅使用：
// - DriversList, DevGroup, Device, DeviceType
// - ConnectedDevice, MountStatus, LocationResult, SphericalCoordinates
// - ScheduleData, SloveResults
// - FITSImage::Star

/**********************  数据结构  **********************/
/**
 * @brief 星点信息与电调位置（用于自动对焦/星点选择）
 */
struct StarList
{
    int id = -1;                 // 星点编号
    double x = 0, y = 0;         // 星点坐标（像素）
    double hfr = 0;              // 半高全宽（HFR）
    int vector1_id = -1, vector2_id = -1, vector3_id = -1; // 向量关联星点编号
    QPointF vector1 = QPointF(0, 0); // 与其它星点的向量1
    QPointF vector2 = QPointF(0, 0); // 与其它星点的向量2
    QPointF vector3 = QPointF(0, 0); // 与其它星点的向量3
    QString status = "wait";     // 星点状态
    int focuserPosition = 0;     // 记录当前电调位置（用于对焦关联）
};

/**
 * @brief 星点匹配度结构（用于星图匹配/校验）
 */
struct StarMatch
{
    int id = -1;                        // 星点编号
    int vector1_id = -1, vector2_id = -1, vector3_id = -1; // 关联向量编号
    bool isMatch1 = false, isMatch2 = false, isMatch3 = false; // 各向量是否匹配
};

/**
 * @brief 翻转事件类型
 */
enum class FlipEvent { None, Started, Done, Failed };

/**
 * @brief 中天翻转状态
 */
struct MeridianStatus {
    FlipEvent event = FlipEvent::None;
    double etaMinutes = std::numeric_limits<double>::quiet_NaN(); // >0 距中天；<0 已过中天
    // 基于观测时间、地点、RA/DEC 推导的理论方向侧 与 设备上报 PierSide 的比较结果
    // true: 需要翻转；false: 无需翻转；当 PierSide 或 LST/RA 无法获得时，保持默认 false
    bool needsFlip = false;
};



/*
//*************************************************
                                    类名
//*************************************************
MainWindow
*/

class MainWindow : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief 构造函数
     * @param parent Qt 父对象
     */
    explicit MainWindow(QObject *parent = nullptr);

    /**
     * @brief 析构函数
     */
    ~MainWindow();

/**********************  版本/构建信息 & 系统信息  **********************/
public:
    QTimer *system_timer = nullptr; // 系统信息定时器

    /**
     * @brief 更新 CPU 信息到状态/日志
     */
    void updateCPUInfo();

    /**
     * @brief 获取构建日期字符串（用于 QT_Client_Version）
     * @return 构建日期（字符串）
     */
    std::string getBuildDate();

    /**
     * @brief 获取主机 IP 地址并缓存/上报
     */
    void getHostAddress();

    /**
     * @brief 获取盒子空间
     */
    void getCheckBoxSpace();

    /**
     * @brief 清除日志
     */
    void clearLogs();

    /**
     * @brief 清除盒子缓存
     */
    void clearBoxCache();

/**********************  INDI 客户端/服务端 初始化与设备管理  **********************/
public:
    /**
     * @brief 初始化 INDI 客户端
     */
    void initINDIClient();

    /**
     * @brief 初始化 INDI 服务器
     */
    void initINDIServer();

    /**
     * @brief 从驱动配置文件读取驱动与设备列表
     * @param filename 配置文件路径
     * @param drivers_list_from 驱动列表输出
     * @param dev_groups_from 设备分组输出
     * @param devices_from 设备列表输出
     */
    void readDriversListFromFiles(const std::string &filename, DriversList &drivers_list_from,
                                  std::vector<DevGroup> &dev_groups_from, std::vector<Device> &devices_from);

    /**
     * @brief 打印设备分组（调试）
     * @param drivers_list 驱动列表
     * @param ListNum 列表序号
     * @param group 分组名
     */
    void printDevGroups2(const DriversList drivers_list, int ListNum, QString group);

    /**
     * @brief 选择设备（系统/组）
     * @param systemNumber 系统编号
     * @param grounpNumber 分组编号
     */
    void DeviceSelect(int systemNumber, int grounpNumber);

    /**
     * @brief 选择 INDI 设备（系统/组）
     * @param systemNumber 系统编号
     * @param grounpNumber 分组编号
     */
    void SelectIndiDevice(int systemNumber, int grounpNumber);

    /**
     * @brief INDI 驱动确认（校验波特率等）
     * @param DriverName 驱动名
     * @param BaudRate 波特率
     * @return 成功返回 true
     */
    bool indi_Driver_Confirm(QString DriverName, QString BaudRate);

    /**
     * @brief 清空 INDI 驱动选择
     * @return 成功返回 true
     */
    bool indi_Driver_Clear();

    /**
     * @brief INDI 设备确认
     * @param DeviceName 设备名
     * @param DriverName 驱动名
     */
    void indi_Device_Confirm(QString DeviceName, QString DriverName);

    /**
     * @brief 清理设备存在性检查（重置标志）
     * @param drivername 驱动名
     * @param isExist 输出：是否存在
     * @return 状态码
     */
    uint32_t clearCheckDeviceExist(QString drivername, bool &isExist);

    /**
     * @brief 一次性连接所有设备（手动）
     */
    void ConnectAllDeviceOnce();

    /**
     * @brief 继续一次性连接所有设备（续连）
     */
    void continueConnectAllDeviceOnce();

    /**
     * @brief 自动连接所有设备
     */
    void AutoConnectAllDevice();

    /**
     * @brief 继续自动连接所有设备（续连）
     */
    void continueAutoConnectAllDevice();

    /**
     * @brief 连接当前选择的设备
     */
    void DeviceConnect();

    /**
     * @brief 按驱动断开
     * @param Driver 驱动名
     */
    void disconnectDriver(QString Driver);

    /**
     * @brief 按设备名断开
     * @param deviceName 设备名
     * @param description 设备描述
     */
    void disconnectDevice(const QString &deviceName, const QString &description);

    /**
     * @brief 设备绑定到指定类型
     * @param DeviceType 设备类型字符串
     * @param DeviceIndex 设备索引
     */
    void BindingDevice(QString DeviceType, int DeviceIndex);

    /**
     * @brief 解除设备与类型绑定
     * @param DeviceType 设备类型字符串
     */
    void UnBindingDevice(QString DeviceType);

    /**
     * @brief 设备连接后的收尾处理（刷新状态等）
     */
    void AfterDeviceConnect();

    /**
     * @brief 设备连接后的收尾处理（指定设备）
     * @param dp INDI 设备指针
     */
    void AfterDeviceConnect(INDI::BaseDevice *dp);

    /**
     * @brief 断开 INDI 服务器
     * @param client 客户端指针
     */
    void disconnectIndiServer(MyClient *client);

    /**
     * @brief 连接 INDI 服务器
     * @param client 客户端指针
     * @return 成功返回 true
     */
    bool connectIndiServer(MyClient *client);

    /**
     * @brief 清空系统设备列表缓存
     */
    void ClearSystemDeviceList();

    /**
     * @brief 启动指定驱动
     * @param DriverName 驱动名
     * @param DriverType 驱动类型
     */
    void ConnectDriver(QString DriverName, QString DriverType);

    /**
     * @brief 断开某设备
     * @param client 客户端
     * @param DeviceName 设备名
     * @param DeviceType 设备类型
     */
    void DisconnectDevice(MyClient *client, QString DeviceName, QString DeviceType);

    /**
     * @brief 读取已选择驱动列表
     */
    void loadSelectedDriverList();

    /**
     * @brief 读取绑定设备列表
     * @param client 客户端
     */
    void loadBindDeviceList(MyClient *client);


    /**
     * @brief 读取 SDK 版本和 USB 序列号路径
     */
    void loadSDKVersionAndUSBSerialPath();

    /**
     * @brief 读取绑定设备类型列表
     */
    void loadBindDeviceTypeList();

    /**
     * @brief 检查相机是否为单反相机
     * @param device 设备
     * @return 是否为单反相机
     */
    bool isDSLR(INDI::BaseDevice *device);  

    bool NotSetDSLRsInfo = true;  // 是否未设置DSLR相机信息

    /**
     * @brief 检查多个属性是否存在其中之一
     * @param dev 设备
     * @param props 属性列表
     * @return 是否存在其中之一
     */
    bool hasAnyProp(INDI::BaseDevice *dev, std::initializer_list<const char*> props);

    /**
     * @brief 检查属性是否存在
     * @param dev 设备
     * @param prop 属性
     * @return 是否存在
     */
    bool hasProp(INDI::BaseDevice *dev, const char *prop);

    QVector<int> ConnectedCCDList;        // 已连接相机索引
    QVector<int> ConnectedTELESCOPEList;  // 已连接赤道仪索引
    QVector<int> ConnectedFOCUSERList;    // 已连接电调索引
    QVector<int> ConnectedFILTERList;     // 已连接滤镜轮索引
    QVector<QString> ConnectDriverList;   // 已连接驱动名
    QVector<QString> INDI_Driver_List;    // INDI 驱动列表缓存
    QVector<ConnectedDevice> ConnectedDevices; // 已连接设备详细列表


/**********************  图像采集/处理（FITS/JPG/PNG/伪彩）  **********************/
public:
    /**
     * @brief INDI 拍摄
     * @param Exp_times 曝光时间（毫秒）
     */
    void INDI_Capture(int Exp_times);

    /**
     * @brief 终止 INDI 拍摄
     */
    void INDI_AbortCapture();

    /**
     * @brief 聚焦回路（循环）
     */
    void FocusingLooping();

    /**
     * @brief 保存 FITS 为 JPG
     * @param filename FITS 文件名
     * @param ProcessBin 是否进行 bin 处理
     */
    void saveFitsAsJPG(QString filename, bool ProcessBin);

    /**
     * @brief 保存 FITS 为 PNG
     * @param fitsFileName FITS 文件名
     * @param ProcessBin 是否进行 bin 处理
     * @return 0 成功，非 0 失败
     */
    int saveFitsAsPNG(QString fitsFileName, bool ProcessBin);

    /**
     * @brief 保存导星图像为 JPG
     * @param Image OpenCV 图像
     */
    void saveGuiderImageAsJPG(cv::Mat Image);

    /**
     * @brief 伪彩/去马赛克
     * @param img16 16 位图像
     * @return 彩色图像
     */
    cv::Mat colorImage(cv::Mat img16);

    /**
     * @brief 刷新导星图像（按 CFA）
     * @param image16 16 位原图
     * @param CFA Bayer 型号
     */
    void refreshGuideImage(cv::Mat image16, QString CFA);

    /**
     * @brief 图像拉伸与显示
     * @param img16 16 位原图
     * @param CFA Bayer 型号
     * @param AutoStretch 是否自动拉伸
     * @param AWB 是否自动白平衡
     * @param AutoStretchMode 拉伸模式
     * @param blacklevel 黑电平
     * @param whitelevel 白电平
     * @param ratioRG R/G 增益比
     * @param ratioBG B/G 增益比
     * @param offset 偏移
     * @param updateHistogram 是否刷新直方图
     */
    void strechShowImage(cv::Mat img16, QString CFA, bool AutoStretch, bool AWB, int AutoStretchMode,
                         uint16_t blacklevel, uint16_t whitelevel, double ratioRG, double ratioBG,
                         uint16_t offset, bool updateHistogram);

    /**
     * @brief 图像 bin 处理（固定 BIN_SIZE）
     * @param input 输入数据
     * @param width 宽
     * @param height 高
     * @param output 输出数据
     * @param out_w 输出宽
     * @param out_h 输出高
     */
    void bin_image(double *input, long width, long height, double *output, long *out_w, long *out_h);

    /**
     * @brief 处理 FITS 文件的某个 HDU
     * @param infptr 输入 FITS
     * @param outfptr 输出 FITS
     * @param hdunum HDU 序号
     * @param status CFITSIO 状态
     */
    void process_hdu(fitsfile *infptr, fitsfile *outfptr, int hdunum, int *status);

    /**
     * @brief 固定 20x20 bin 处理
     * @return 0 成功
     */
    int process_fixed(); // 20*20xbinning

/**********************  PHD2 导星控制/共享内存  **********************/
public:
    /**
     * @brief 初始化并启动 PHD2
     */
    void InitPHD2();

    /**
     * @brief 连接 PHD2
     * @return 成功返回 true
     */
    bool connectPHD(void);

    /**
     * @brief 获取 PHD 版本
     * @param versionName 输出版本
     * @return 状态码
     */
    bool call_phd_GetVersion(QString &versionName);

    uint32_t call_phd_StartLooping(void);      // 开始循环曝光
    uint32_t call_phd_StopLooping(void);       // 停止循环曝光
    uint32_t call_phd_AutoFindStar(void);      // 自动寻星
    uint32_t call_phd_StartGuiding(void);      // 开始导星
    uint32_t call_phd_checkStatus(unsigned char &status); // 查询状态
    uint32_t call_phd_setExposureTime(unsigned int expTime); // 设置曝光
    uint32_t call_phd_whichCamera(std::string Camera);  // 选择相机
    uint32_t call_phd_ChackControlStatus(int sdk_num);  // 检查控制权限
    uint32_t call_phd_ClearCalibration(void);           // 清除标定
    uint32_t call_phd_StarClick(int x, int y);          // 选择星点
    uint32_t call_phd_FocalLength(int FocalLength);     // 设置焦距
    uint32_t call_phd_MultiStarGuider(bool isMultiStar);// 多星导星开关
    uint32_t call_phd_CameraPixelSize(double PixelSize);// 设置像元尺寸
    uint32_t call_phd_CameraGain(int Gain);             // 设置增益
    uint32_t call_phd_CalibrationDuration(int StepSize);// 标定步长
    uint32_t call_phd_RaAggression(int Aggression);     // 赤经纠正力度
    uint32_t call_phd_DecAggression(int Aggression);    // 赤纬纠正力度

    /**
     * @brief 显示 PHD 数据（调试/可视化）
     */
    void ShowPHDdata();

    /**
     * @brief 发送导星脉冲
     * @param Direction 方向
     * @param Duration 持续时间（ms）
     */
    void ControlGuide(int Direction, int Duration);

    /**
     * @brief 解析并获取 PHD2 控制指令
     */
    void GetPHD2ControlInstruct();

    /**
     * @brief 记录当前时间戳（调试/统计）
     * @param index 索引
     */
    void getTimeNow(int index);

    int glExpTime = 1000;               // 默认曝光时间（ms）
    bool isGuideCapture = true;         // 是否导星模式采集
    char *sharedmemory_phd = nullptr;   // PHD 共享内存指针
    int key_phd = 0;                    // 共享内存 key
    int shmid_phd = -1;                 // 共享内存 id
    QProcess *cmdPHD2 = nullptr;        // PHD2 进程
    bool phd2ExpectedRunning = false;   // 期望 PHD2 处于运行状态（用于判定异常退出）

    char phd_direction = 0;             // 指令方向
    int phd_step = 0;                   // 步长
    double phd_dist = 0;                // 距离

    QVector<QPoint> glPHD_Stars;        // 星点列表
    QVector<QPointF> glPHD_rmsdate;     // RMS 数据
    bool glPHD_isSelected = false;      // 是否已选星
    double glPHD_StarX = 0;             // 星点 X
    double glPHD_StarY = 0;             // 星点 Y
    int glPHD_CurrentImageSizeX = 0;    // 当前图像宽
    int glPHD_CurrentImageSizeY = 0;    // 当前图像高
    int glPHD_OrigImageSizeX = 0;       // 原始图像宽（未合并/缩放前）
    int glPHD_OrigImageSizeY = 0;       // 原始图像高（未合并/缩放前）
    int glPHD_OutImageSizeX  = 0;       // 实际输出到UI的图像宽（合并/缩放后）
    int glPHD_OutImageSizeY  = 0;       // 实际输出到UI的图像高（合并/缩放后）
    int glPHD_ImageScale     = 1;       // 合并/缩放倍数（例如 NEAREST 下的 scale）
    double glPHD_LockPositionX = 0;     // 锁定位置 X
    double glPHD_LockPositionY = 0;     // 锁定位置 Y
    bool glPHD_ShowLockCross = false;   // 是否显示锁定十字
    bool glPHD_StartGuide = false;      // 是否开始导星

    bool ClearCalibrationData = true;   // 是否清除校准
    bool isGuiding = false;             // 导星中
    bool isGuiderLoopExp = false;       // 导星循环曝光

    QThread *PHDControlGuide_thread = nullptr;  // 导星控制线程
    QTimer  *PHDControlGuide_threadTimer = nullptr; // 导星控制定时器
    std::mutex receiveMutex;                     // 接收互斥

    /**
     * @brief 导星控制定时器回调（槽）
     */
    Q_SLOT void onPHDControlGuideTimeout();

    // PHD2 进程监控与恢复
    Q_SLOT void onPhd2Exited(int exitCode, QProcess::ExitStatus exitStatus);
    Q_SLOT void onPhd2Error(QProcess::ProcessError error);

    // 强制结束 PHD2 及清理共享内存（用于启动前和异常恢复）
    void forceKillPhd2AndCleanupShm();
    void cleanupPhd2Shm();
    void promptFrontendPhd2Restart();
    void disconnectFocuserIfConnected();

/**********************  对焦/电调控制与自动对焦  **********************/
public:
    int currentSpeed = 3;          // 电调速度
    int currentSteps = 5000;       // 单次移动步数
    int startPosition = 0;         // 电调开始位置
    int CurrentPosition = 0;       // 当前电调位置
    int TargetPosition = 0;        // 目标位置
    bool MoveInward = true;        // 方向（true 向内）
    int AutoMovePosition = 0;      // 自动移动目标
    bool FWHMCalOver = false;      // FWHM 计算结束
    float minPoint_X = 0.0f;       // 曲线最小点 X（可用于记录）
    bool currentDirection = true;  // 当前方向
    double focusMoveEndTime = 0;   // 结束命令超时控制
    QTimer *focusMoveTimer = nullptr; // 电调移动定时器
    bool isFocusMoveDone = false;  // 是否移动完成
    int focuserMaxPosition = -1;   // 行程上限
    int focuserMinPosition = -1;   // 行程下限
    int autofocusBacklashCompensation = 0; // 自动对焦空程补偿值

    // 固定步数移动状态与看门狗
    bool isStepMoving = false;         // 是否正在执行一次固定步数移动
    int stepMoveOutTime = 0;            // 本次固定步数移动超时时间


    /**
     * @brief 控制电调按当前方向移动（持续）
     * @param isInward true 向内，false 向外
     */
    void FocuserControlMove(bool isInward);

    /**
     * @brief 控制电调按当前方向移动（持续）
     * @param isInward true 向内，false 向外
     */
    void FocuserControlMoveStep(bool isInward, int steps);
    void cancelStepMoveIfAny();

    /**
     * @brief 周期处理电调移动数据（刷新参数）
     */
    void HandleFocuserMovementDataPeriodically();

    /**
     * @brief 停止电调移动
     * @param isClickMove 是否点击触发的短动
     */
    void FocuserControlStop(bool isClickMove = false);

    QTimer *updatePositionTimer = nullptr; // 停止时用于刷新位置的定时器
    int updateCount = 0;                   // 更新计数器

    /**
     * @brief 检查电调移动命令是否正常
     */
    void CheckFocuserMoveOrder();



    bool isFocusLoopShooting = false; // ROI 循环拍摄使能

    /**
     * @brief 开关 ROI 循环拍摄
     * @param isLoop 是否循环
     */
    void focusLoopShooting(bool isLoop);

    /**
     * @brief 获取 ROI 循环拍摄状态
     */
    void getFocuserLoopingState();

    /**
     * @brief 自动对焦星点选择器
     * @param stars 星点列表
     * @return 选择的星点中心
     */
    QPointF selectStar(QList<FITSImage::Star> stars);
    // 选择/追踪的星点（全图坐标）及锁定状态
    bool selectedStarLocked = false;
    QPointF lockedStarFull = QPointF(-1, -1);

    std::map<std::string, double> roiAndFocuserInfo; // ROI 与电调信息共享
    


    QPointF currentSelectStarPosition;                 // 当前选中星点
    QVector<QPointF> currentAutoFocusStarPositionList; // 当前曲线拟合星点
    QVector<QPointF> allAutoFocusStarPositionList;     // 所有拟合星点
    int overSelectStarAutoFocusStep = 0;               // 结束基于选星的对焦
    QVector<bool> isAutoFocusStarPositionList;         // 无星标志序列（左右方向）

    // 是否允许 selectStar 自动更新 ROI 位置（默认关闭用于排查抖动）
    bool enableAutoRoiCentering = true;
    // 追踪窗口比例（相对于 ROI 边长），当锁定星点超出该窗口时允许更新 ROI 使其回到中心
    double trackWindowRatio = 0.06;
    // 选星防抖动参数：粘滞半径（像素）。若上一帧锁定星在该半径内有匹配，则保持锁定，避免在近邻星之间跳动
    double starStickRadiusPx = 6.0;
    // 选星防抖动参数：搜索半径（像素）。若最近星超出该半径，则视为丢失，不切换星点（保持上一帧锁定）
    double starSeekRadiusPx = 12.0;
    // ROI 居中防抖：需要连续超阈值的帧数才触发挂起更新
    int requiredOutFramesForRecentre = 1;
    int outOfWindowFrames = 0;
    // 是否使用虚拟测试图像（用于星点追踪测试）
    bool useVirtualTestImages = false;

    // 待应用的 ROI 更新（用于在本帧显示后再更新 ROI，避免首帧未居中的视觉问题）
    bool hasPendingRoiUpdate = false;
    int pendingRoiX = 0;
    int pendingRoiY = 0;

    /**
     * @brief 启动自动对焦流程
     */
    void startAutoFocus();
    
    /**
     * @brief 启动计划任务表触发的自动对焦
     */
    void startScheduleAutoFocus();

    AutoFocus *autoFocus = nullptr; // 自动对焦对象
    int  autoFocusStep = 0;         // 自动对焦步数
    bool autoFocuserIsROI = false;  // 是否使用 ROI 对焦
    

    /**
     * @brief 获取焦距器参数
     */
    void getFocuserParameters();
    /**
     * @brief 清理与 AutoFocus 对象之间的所有信号槽连接，避免重复连接
     */
    void cleanupAutoFocusConnections();

    // 自动对焦相关信号连接集合，用于统一释放避免重复连接
    QVector<QMetaObject::Connection> autoFocusConnections;

    /**
     * @brief 获取焦距器状态
     */
    void getFocuserState();

    /**
     * @brief 发送 ROI 信息到前端
     */
    void sendRoiInfo();

    /**
     * @brief 电调移动到最小位置
     */
    void focusMoveToMin();

    /**
     * @brief 电调移动到最大位置
     */
    void focusMoveToMax();

    /**
     * @brief 设置电调行程范围（最小/最大）
     */
    void focusSetTravelRange();

    int lastPosition = 0;                        // 上一次位置
    QTimer *focusMoveToMaxorMinTimer = nullptr;  // 往返移动控制定时器

    QTimer FWHMTimer;          // FWHM 计算/采样定时器
    QString MainCameraCFA;     // 主相机 CFA
    double ImageGainR = 1.0;   // 显示增益 R
    double ImageGainB = 1.0;   // 显示增益 B
    double ImageOffset = 0.0;  // 显示偏移
    double CameraGain = 0;        // 相机增益
    QVector<QPointF> dataPoints; // FWHM 数据点
    double R2 = 0;             // 拟合优度
    bool isAutoFocus = false;  // 自动对焦开关
    bool StopAutoFocus = false;// 停止自动对焦

    /**
     * @brief 设置电调速度
     * @param speed 速度
     * @return 0 成功
     */
    int FocuserControl_setSpeed(int speed);

    /**
     * @brief 获取电调速度
     * @return 速度值
     */
    int FocuserControl_getSpeed();

    /**
     * @brief 获取电调当前位置
     * @return 位置值
     */
    int FocuserControl_getPosition();

/**********************  望远镜控制/天文时角 & 解算  **********************/
public:
    double observatorylongitude = -1; // 观测站经度（东经+）
    double observatorylatitude  = -1; // 观测站纬度（北纬+）

    /**
     * @brief 是否需要过中天翻转
     * @param lst 当地恒星时（小时角制）
     * @param targetRA 目标赤经（小时）
     * @return 需要翻转返回 true
     */
    bool needsMeridianFlip(double lst, double targetRA);

    /**
     * @brief 执行观测流程（包含 goto、导星与解算的协调）
     * @param lst 当地恒星时
     * @param currentDec 当前赤纬（度）
     * @param targetRA 目标赤经（小时）
     * @param targetDec 目标赤纬（度）
     * @param observatoryLongitude 经度（东经+）
     * @param observatoryLatitude 纬度（北纬+）
     */
    void performObservation(double lst, double currentDec, double targetRA, double targetDec,
                            double observatoryLongitude, double observatoryLatitude);

    /**
     * @brief 计算儒略日
     * @param utc_time UTC 时间点
     * @return JD
     */
    double getJulianDate(const std::chrono::system_clock::time_point &utc_time);

    /**
     * @brief 计算格林尼治平恒星时（GMST）
     * @param utc_time UTC 时间点
     * @return GMST（小时）
     */
    double computeGMST(const std::chrono::system_clock::time_point &utc_time);

    /**
     * @brief 计算地方恒星时（LST）
     * @param longitude_east 东经（度）
     * @param utc_time UTC 时间点
     * @return LST（小时）
     */
    double computeLST(double longitude_east, const std::chrono::system_clock::time_point &utc_time);

    /**
     * @brief 望远镜转到指定赤经赤纬
     * @param Ra 赤经（小时）
     * @param Dec 赤纬（度）
     */
    void TelescopeControl_Goto(double Ra, double Dec);

    /**
     * @brief 获取赤道仪状态
     * @return 状态
     */
    QString TelescopeControl_Status();

    /**
     * @brief 公园（停机位）
     * @return 成功返回 true
     */
    bool TelescopeControl_Park();

    /**
     * @brief 跟踪开关
     * @return 成功返回 true
     */
    bool TelescopeControl_Track();

    /**
     * @brief 回原位（Home）
     */
    void TelescopeControl_Home();

    /**
     * @brief 同步 Home 位（SYNCHome）
     */
    void TelescopeControl_SYNCHome();

    /**
     * @brief 解算后同步坐标（Solve & SYNC）
     */
    void TelescopeControl_SolveSYNC();

    /**
     * @brief 解算当前位置
     */
    void solveCurrentPosition();

    QTimer solveCurrentPositionTimer; // 解算当前位置定时器

    bool isSolveSYNC = false; // 是否处于解算同步流程

    /**
     * @brief 获取赤道仪位置
     * @return 经纬度与海拔
     */
    LocationResult TelescopeControl_GetLocation();

    /**
     * @brief 获取 UTC 时间（来自赤道仪）
     * @return QDateTime（UTC）
     */
    QDateTime TelescopeControl_GetTimeUTC();

    /**
     * @brief 获取当前赤经赤纬
     * @return 球面坐标
     */
    SphericalCoordinates TelescopeControl_GetRaDec();

    /**
     * @brief 轮询中天翻转事件
     * @return 翻转事件类型
     */
    MeridianStatus checkMeridianStatus();

    // 不再持久化/保存历史翻转状态（使用即时几何判断 needsFlip）

    /**
     * @brief 单次图像板解
     * @param Filename 图像文件
     * @param FocalLength 焦距（mm）
     * @param CameraWidth 相机宽（mm）
     * @param CameraHeight 相机高（mm）
     */
    void SolveImage(QString Filename, int FocalLength, double CameraWidth, double CameraHeight);

    /**
     * @brief 循环图像板解
     * @param Filename 图像文件
     * @param FocalLength 焦距（mm）
     * @param CameraWidth 相机宽（mm）
     * @param CameraHeight 相机高（mm）
     */
    void LoopSolveImage(QString Filename, int FocalLength, double CameraWidth, double CameraHeight);

    /**
     * @brief 循环采集
     * @param ExpTime 曝光时间（ms）
     */
    void LoopCapture(int ExpTime);

    bool EndCaptureAndSolve = false;  // 结束采集并解算
    bool TakeNewCapture = true;       // 是否需要新拍
    bool isSavePngSuccess = false;    // PNG 保存状态

    /**
     * @brief 调度表消息输出（状态同步）
     * @param message 消息文本
     */
    void ScheduleTabelData(QString message);

    bool isLoopSolveImage = false; // 循环解算标志
    bool isSingleSolveImage = false; // 单次解算标志

    int SolveImageHeight = 0; // 解算图像高
    int SolveImageWidth  = 0; // 解算图像宽

    QString SolveImageFileName = "/dev/shm/MatToFITS.fits"; // 解算默认文件
    std::vector<SloveResults> SloveResultList;              // 解算结果列表

    /**
     * @brief 清空解算结果列表
     */
    void ClearSloveResultList();

    /**
     * @brief 恢复/回滚解算结果（实现内定义）
     */
    void RecoverySloveResul();

    int glFocalLength = 0;           // 全局焦距
    double glCameraSize_width = 0;   // 相机感光面宽（mm）
    double glCameraSize_height = 0;  // 相机感光面高（mm）
    int glTelescopeTotalSlewRate = 0;// 赤道仪总速率

/**********************  调度/工作流（拍摄、导星、等待等）  **********************/
public:
    QList<ScheduleData> m_scheduList; // 调度队列

    QTimer telescopeTimer;
    QTimer guiderTimer;
    QTimer captureTimer;
    QTimer timewaitingTimer;
    QTimer filterTimer;
    QTimer focusTimer;
    QTimer solveTimer;
    QTimer exposureDelayTimer;  // 曝光延迟定时器

    int schedule_currentNum = 0;     // 当前任务序号
    int schedule_ExpTime = 0;        // 当前任务曝光（ms）
    int schedule_CFWpos = 0;         // 当前任务滤镜位
    int schedule_RepeatNum = 0;      // 任务重复次数
    int schedule_ExposureDelay = 0;  // 当前任务曝光延迟（ms）
    int schedule_currentShootNum = 0;// 已拍张数
    int expTime_ms = 0;              // 当前拍摄时间（ms）
    int exposureDelayElapsed_ms = 0; // 曝光延迟已过去的时间（ms）

    bool InSlewing = false;          // 正在转台
    bool GuidingHasStarted = false;  // 导星已启动
    QString ShootStatus;             // 拍摄状态文本

    bool StopSchedule = false;       // 停止调度
    bool isScheduleRunning = false;  // 当前计划任务是否在运行（用于前端同步）
    bool StopPlateSolve = false;     // 停止解算
    bool MountGotoError = false;     // GOTO 错误
    bool GotoThenSolve = false;      // GOTO 后解算
    bool isScheduleTriggeredAutoFocus = false;  // 是否由计划任务表触发的自动对焦

    /**
     * @brief 启动调度
     */
    void startSchedule();

    /**
     * @brief 执行下一条调度
     */
    void nextSchedule();

    /**
     * @brief 启动赤道仪转动
     * @param ra 赤经（小时）
     * @param dec 赤纬（度）
     */
    void startMountGoto(double ra, double dec);

    /**
     * @brief 启动导星
     */
    void startGuiding();

    /**
     * @brief 启动等待计时
     */
    void startTimeWaiting();

    /**
     * @brief 启动拍摄
     * @param ExpTime 曝光时间（ms）
     */
    void startCapture(int ExpTime);

    /**
     * @brief 启动曝光延迟等待
     */
    void startExposureDelay();

    /**
     * @brief 设置滤镜位置
     * @param pos 槽位编号
     */
    void startSetCFW(int pos);

    /**
     * @brief 等待赤道仪完成
     * @return 完成返回 true
     */
    bool WaitForTelescopeToComplete();

    /**
     * @brief 等待拍摄完成
     * @return 完成返回 true
     */
    bool WaitForShootToComplete();

    /**
     * @brief 等待导星完成/稳定
     * @return 完成返回 true
     */
    bool WaitForGuidingToComplete();

    /**
     * @brief 等待计时完成
     * @return 完成返回 true
     */
    bool WaitForTimeToComplete();

    /**
     * @brief 等待电调动作完成
     * @return 完成返回 true
     */
    bool WaitForFocuserToComplete();

    /**
     * @brief 计算计划表步骤的进度
     * @param stepNumber 步骤编号（1=等待，2=转动，3=滤镜，4-N=拍摄）
     * @param stepProgress 步骤内的进度（0.0-1.0，用于拍摄过程中的进度）
     * @return 计算后的进度值（限制在0-100之间）
     */
    int calculateScheduleProgress(int stepNumber, double stepProgress = 1.0);


    /**
     * @brief 调度图像保存（命名/序号）
     * @param name 目标名
     * @param num 序号
     * @return 0 成功
     */
    int ScheduleImageSave(QString name, int num);

    /**
     * @brief 通用图像保存入口
     * @return 状态码
     */
    int CaptureImageSave();

    /**
     * @brief 保存解算失败的图像
     * @return 状态码
     */
    int solveFailedImageSave(const QString& imagePath = "");

    QString ScheduleTargetNames; // 调度目标集合名

/**********************  文件/目录与 USB 相关  **********************/
public:
    // 保存路径（保留原默认值与注释）
    std::string ImageSaveBasePath = "image"; // 默认保存路径
    QString     ImageSaveBaseDirectory = "image"; // 当前保存路径
    QMap<QString, QString> usbMountPointsMap; // U盘映射表：U盘名 -> U盘路径
    QString saveMode = "local"; // 保存模式：local=本地，其它为U盘名代码U盘模式

    /**
     * @brief 检查目录是否存在
     * @param path 目录路径
     * @return 存在返回 true
     */
    bool directoryExists(const std::string &path);

    /**
     * @brief 创建调度保存目录
     * @return 成功返回 true
     */
    bool createScheduleDirectory();

    /**
     * @brief 创建采集保存目录
     * @return 成功返回 true
     */
    bool createCaptureDirectory();

    /**
     * @brief 创建解算失败图像目录
     * @return 成功返回 true
     */
    bool createsolveFailedImageDirectory();

    /**
     * @brief 检查存储空间并创建目录（通用辅助函数）
     * @param sourcePath 源文件路径
     * @param destinationDirectory 目标目录
     * @param dirPathToCreate 需要创建的目录路径（U盘保存时使用）
     * @param functionName 函数名（用于日志）
     * @param isUSBSave 是否为U盘保存
     * @param createLocalDirectoryFunc 本地保存时创建目录的回调函数（std::function<void()>）
     * @return 成功返回0，失败返回1
     */
    int checkStorageSpaceAndCreateDirectory(const QString &sourcePath, 
                                           const QString &destinationDirectory,
                                           const QString &dirPathToCreate,
                                           const QString &functionName,
                                           bool isUSBSave,
                                           std::function<void()> createLocalDirectoryFunc = nullptr);

    /**
     * @brief 保存图像文件（通用辅助函数）
     * @param sourcePath 源文件路径
     * @param destinationPath 目标文件路径
     * @param functionName 函数名（用于日志）
     * @param isUSBSave 是否为U盘保存
     * @return 成功返回0，失败返回1
     */
    int saveImageFile(const QString &sourcePath, 
                     const QString &destinationPath,
                     const QString &functionName,
                     bool isUSBSave);

    bool isStagingImage = false;   // 是否有缓存图像
    std::string SavedImage;        // 最近保存图像路径

    /**
     * @brief 判断文件是否存在
     * @param filePath 路径
     * @return 存在返回 true
     */
    bool isFileExists(const QString &filePath);

    /**
     * @brief 获取缓存图像（从共享/临时区）
     */
    void getStagingImage();

    bool isStagingScheduleData = false; // 是否有缓存调度数据
    QString StagingScheduleData;        // 缓存调度数据

    /**
     * @brief 获取缓存调度数据
     */
    void getStagingScheduleData();

    /**
     * @brief 获取缓存导星数据
     */
    void getStagingGuiderData();

    QElapsedTimer ImageSolveTimer; // 解算计时器
    qint64 ImageSolveTime = 0;     // 解算耗时（ms）

    /**
     * @brief 移动文件到 U 盘
     * @return 成功移动的文件数
     */
    int MoveFileToUSB();

    int mountDisplayCounter = 0;   // 挂载显示计数
    int MainCameraStatusCounter = 0;// 主相机状态计数
    int glMainCameraBinning = 1;   // 主相机 bin

    bool isFilterOnCamera = false; // 滤镜是否内置相机

    /**
     * @brief 赤道仪转到坐标（别名）
     * @param Ra_Hour 赤经（小时）
     * @param Dec_Degree 赤纬（度）
     */
    void MountGoto(double Ra_Hour, double Dec_Degree);


    /**
     * @brief 赤道仪转到坐标（别名）
     * @param Ra_Hour 赤经（小时）
     * @param Dec_Degree 赤纬（度）
     */
    void MountOnlyGoto(double Ra_Hour, double Dec_Degree);

    /**
     * @brief 删除图片文件
     * @param DelImgPath 路径列表
     */
    void DeleteImage(QStringList DelImgPath);

    /**
     * @brief 列举所有文件（用于前端）
     * @return JSON/字符串
     */
    std::string GetAllFile();

    /**
     * @brief 解析字符串（提取图像文件）
     * @param input 输入字符串
     * @param imgFilePath 图像路径
     * @return 文件名列表
     */
    QStringList parseString(const std::string &input, const std::string &imgFilePath);

    /**
     * @brief 查询 U 盘剩余空间
     * @param usb_mount_point 挂载点
     * @return 空间字节数
     */
    long long getUSBSpace(const QString &usb_mount_point);

    /**
     * @brief 计算文件总大小
     * @param filePaths 文件列表
     * @return 总字节数
     */
    long long getTotalSize(const QStringList &filePaths);

    /**
     * @brief 移动图像到 U 盘（并删除原始）
     * @param RemoveImgPath 路径列表
     */
    void RemoveImageToUsb(QStringList RemoveImgPath, QString usbName = "");

    /**
     * @brief 判断挂载点是否只读
     * @param mountPoint 挂载点
     * @return 只读返回 true
     */
    bool isMountReadOnly(const QString &mountPoint);

    /**
     * @brief 重新以可写方式挂载
     * @param mountPoint 挂载点
     * @param password 提权密码
     * @return 成功返回 true
     */
    bool remountReadWrite(const QString &mountPoint, const QString &password);

    /**
     * @brief 获取指定目录图像文件
     * @param ImageFolder 目录
     */
    void GetImageFiles(std::string ImageFolder);

    /**
     * @brief USB 状态检查（插拔/空间/挂载）
     */
    void USBCheck();

    /**
     * @brief 获取USB驱动器文件列表
     * @param usbName U盘名称（必需）
     * @param relativePath 相对于USB根目录的路径（空字符串表示根目录）
     */
    void GetUSBFiles(const QString &usbName, const QString &relativePath);

    /**
     * @brief 获取U盘挂载点（统一函数）
     * @param usb_mount_point 输出参数，U盘挂载点路径
     * @return 成功返回true，失败返回false（未找到、多个U盘等情况）
     */
    bool getUSBMountPoint(QString &usb_mount_point);

/**********************  GPIO 控制  **********************/
public:
    /**
     * @brief 初始化 GPIO（导出/方向）
     */
    void initGPIO();

    /**
     * @brief 导出 GPIO 管脚
     * @param pin 管脚号（字符串）
     */
    void exportGPIO(const char *pin);

    /**
     * @brief 设置 GPIO 方向
     * @param pin 管脚号
     * @param direction "in"/"out"
     */
    void setGPIODirection(const char *pin, const char *direction);

    /**
     * @brief 设置 GPIO 电平
     * @param pin 管脚号
     * @param value "0"/"1"
     */
    void setGPIOValue(const char *pin, const char *value);

    /**
     * @brief 读取 GPIO 电平
     * @param pin 管脚号
     * @return 0/1 或失败码
     */
    int readGPIOValue(const char *pin);

    /**
     * @brief 获取所有 GPIO 状态（调试打印）
     */
    void getGPIOsStatus();

/**********************  本地化/系统设置（时间、位置、网络/AP）  **********************/
public:
    /**
     * @brief 同步系统时间到设备
     * @param time 时间（HH:mm:ss）
     * @param date 日期（yyyy-MM-dd）
     */
    void synchronizeTime(QString time, QString date);

    double DEC = -1; // 当前赤纬（缓存）
    double RA  = -1; // 当前赤经（缓存）

    /**
     * @brief 设置赤道仪位置
     * @param lat 纬度字符串
     * @param lon 经度字符串
     */
    void setMountLocation(QString lat, QString lon);

    /**
     * @brief 设置赤道仪 UTC 时间
     * @param time 时间（HH:mm:ss）
     * @param date 日期（yyyy-MM-dd）
     */
    void setMountUTC(QString time, QString date);

    QString localLon = "";      // 本地经度字符串
    QString localLat = "";      // 本地纬度字符串
    QString localLanguage = ""; // 本地语言
    QString localTime = "";     // 本地时区

    /**
     * @brief 读取上次选择的设备
     */
    void getLastSelectDevice();

    /**
     * @brief 修改热点名（AP SSID）
     * @param newName 新名称
     */
    void editHotspotName(QString newName);

    /**
     * @brief 获取热点名（AP SSID）
     * @return 名称
     */
    QString getHotspotName();

/**********************  串口/设备路径  **********************/
public:
    SerialDeviceDetector detector; // 串口设备检测器

    /**
     * @brief 获取当前连接的串口列表
     * @return 串口名列表
     */
    QStringList getConnectedSerialPorts();

    /**
     * @brief 解析符号链接指向的真实串口
     * @param symbolicLink 符号链接路径
     * @return 真实设备路径
     */
    QString resolveSerialPort(const QString &symbolicLink);

    /**
     * @brief 查找指向指定 tty 设备的符号链接
     * @param directoryPath 搜索目录
     * @param ttyDevice 目标 tty 设备名
     * @return 符号链接列表
     */
    QStringList findLinkToTtyDevice(const QString &directoryPath, const QString &ttyDevice);

    /**
     * @brief 判断两个文件是否同一目录
     * @param path1 路径1
     * @param path2 路径2
     * @return 同目录返回 true
     */
    bool areFilesInSameDirectory(const QString &path1, const QString &path2);

    /**
     * @brief 获取指向指定 tty 设备的 /dev/serial/by-id 符号链接
     * @param ttyDevice 目标 tty 设备名（如 ttyUSB0/ttyACM0）
     * @return by-id 符号链接列表
     */
    QStringList getByIdLinksForTty(const QString &ttyDevice);

    /**
     * @brief 判断 by-id 链接是否匹配指定驱动类型（Focuser/Mount）
     * @param symlinkPath by-id 符号链接完整路径
     * @param driverType 驱动类型（"Focuser"/"Mount"）
     * @return 匹配返回 true
     */
    bool isByIdLinkForDriverType(const QString &symlinkPath, const QString &driverType);

    /**
     * @brief 在一组 by-id 链接中为指定驱动类型选择最优项
     * @param links 候选链接
     * @param driverType 驱动类型（"Focuser"/"Mount"）
     * @return 选中的链接；若无合适项则返回空字符串
     */
    QString selectBestByIdLink(const QStringList &links, const QString &driverType);

    /**
     * @brief 根据 tty 名称对设备类型打分（Focuser/Mount）
     */
    int scoreTtyNameForType(const QString &ttyDevice, const QString &driverType);

    /**
     * @brief 综合 by-id 与 tty 名称对端口进行打分
     */
    int scorePortForType(const QString &ttyDevice, const QStringList &byIdLinks, const QString &driverType);

/**********************  调试/日志 & 前端交互  **********************/
public:
    /**
     * @brief 发送调试信息到前端（Vue）
     * @param msg 文本
     */
    void SendDebugToVueClient(const QString &msg);

    /**
     * @brief 根据部分字符串识别设备类型
     * @param typeStr 类型片段
     * @return 设备类型枚举
     */
    DeviceType getDeviceTypeFromPartialString(const std::string &typeStr);

    /**
     * @brief 全局自定义日志处理器
     */
    static void customMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg);

    std::vector<std::string> logCache; // 日志缓存（用于批量发送）

    /**
     * @brief 处理 INDI Server 标准输出
     */
    void handleIndiServerOutput();

    /**
     * @brief 处理 INDI Server 错误输出
     */
    void handleIndiServerError();

    /**
     * @brief 获取主相机参数（增益、偏置、温度等）
     */
    void getMainCameraParameters();

    /**
     * @brief 获取赤道仪参数（自动翻转、东边分钟过中天、西边分钟过中天）
     */
    void getMountParameters();

/**********************  客户端设置（持久化）  **********************/
public:
    /**
     * @brief 读取客户端设置
     */
    void getClientSettings();

    /**
     * @brief 设置客户端配置项
     * @param ConfigName 键
     * @param ConfigValue 值
     */
    void setClientSettings(QString ConfigName, QString ConfigValue);

/**********************  自动极轴校准  **********************/
public:
    /**
     * @brief 初始化自动极轴校准
     * @return 成功返回 true
     */
    bool initPolarAlignment();

    PolarAlignment *polarAlignment = nullptr; // 极轴校准对象

/**********************  与前端消息交互（槽）  **********************/
private slots:
    /**
     * @brief 收到前端消息（JSON 文本）
     * @param message 消息
     */
    void onMessageReceived(const QString &message);

    /**
     * @brief 解析信息回调（中转）
     * @param message 信息
     */
    void onParseInfoEmitted(const QString &message);

private:
    // WebSocket消息防抖机制（只保留最后一条命令）
    QString lastCommandMessage; // 存储最后一条完整消息（命令+参数）
    qint64 lastCommandTime = 0; // 存储最后一条消息的执行时间（毫秒时间戳）
    static const int COMMAND_DEBOUNCE_MS = 500; // 防抖时间窗口（500毫秒），短时间内重复的完整消息（命令和参数都相同）只执行一次

/**********************  线程/定时器（通用）  **********************/
public:
    bool one_touch_connect = true;        // 一键连接开关
    bool one_touch_connect_first = true;  // 一键连接首次标志
    int glMainCCDSizeX = 0;               // 主相机宽
    int glMainCCDSizeY = 0;               // 主相机高
    double CameraTemperature = 16;         // 当前相机温度
    int glOffsetValue = 0, glOffsetMin = 0, glOffsetMax = 0; // 偏置范围
    int glGainValue = 0, glGainMin = 0, glGainMax = 0;       // 增益范围

    bool glIsFocusingLooping = false;     // 对焦循环开关
    QString glMainCameraStatu;            // 主相机状态
    QElapsedTimer glMainCameraCaptureTimer; // 拍摄计时
    bool mainCameraAutoSave = false;      // 主相机自动保存开关
    bool mainCameraSaveFailedParse = false;  // 主相机保存解析失败图片开关

    bool isAutoFlip = false;                  // 是否自动翻转
    int flipPrepareTimeDefault = 10;          // 预备翻转时间默认值
    int flipPrepareTime = flipPrepareTimeDefault;                 // 预备翻转时间（秒）,当为0时开始翻转
    double EastMinutesPastMeridian = 10;       // 东边分钟过中天
    double WestMinutesPastMeridian = 10;       // 西边分钟过中天

    std::string vueDirectoryPath = "/dev/shm/"; // 前端共享目录
 
    std::string vueImagePath = "/var/www/html/img/";

    // std::string vueImagePath = "/home/quarcs/workspace/QUARCS/QUARCS_stellarium-web-engine/apps/web-frontend/dist/img/";


    std::string PriorGuiderImage = "NULL"; // 上一帧导星图
    std::string PriorROIImage = "NULL";    // 上一帧 ROI 图
    std::string PriorCaptureImage = "NULL";// 上一帧拍摄图
    int BoxSideLength = 300;               // vue上绘制的框的大小
    bool AutoStretch = true;               // 自动拉伸

    bool InGuiding = false;                // 是否正在导星
    QString TelescopePierSide;             // 望远镜支架侧
    bool FirstRecordTelescopePierSide = true; // 首次记录支架侧
    QString FirstTelescopePierSide;        // 初始支架侧
    bool isMeridianFlipped = false;        // 是否执行过中天翻转

    QThread *m_thread = nullptr;           // 通用线程
    QTimer  *m_threadTimer = nullptr;      // 通用定时器

    QTimer *GotoOlveTimer = nullptr;         // 解算定时器

    /**
     * @brief 通用定时器槽
     */
    Q_SLOT void onTimeout();

/**********************  设备连接辅助  **********************/
public:
    /**
     * @brief 获取已连接设备列表（刷新缓存）
     */
    void getConnectedDevices();

    /**
     * @brief 清空已连接设备缓存
     */
    void clearConnectedDevices();

/**********************  望远镜/拍摄便捷接口  **********************/
public:
    /**
     * @brief 等待赤道仪完成（阻塞式）
     * @return 完成返回 true
     */
    // 已在“调度/工作流”区声明同名接口

/**********************  私有成员：通信对象/线程  **********************/
private:
    // WebSocketClient *websocket;  // 原注释保留
    WebSocketThread *wsThread = nullptr;  // WebSocket 线程
    QUrl websockethttpUrl;                // WS HTTP URL
    QUrl websockethttpsUrl;               // WS HTTPS URL

    MyClient *indi_Client = nullptr;      // INDI 客户端
    QProcess *glIndiServer = nullptr;     // INDI 服务器进程

    static MainWindow *instance;          // 单例指针
};

#endif // MAINWINDOW_H
