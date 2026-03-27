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
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>

#include <functional>

#include <fitsio.h>

#include <sys/statvfs.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <climits>
#include <mutex>
#include <atomic>
#include <memory>
#include <unordered_set>

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <memory>
#include <set>
#include <unordered_set>
#include <algorithm>
#include <regex>
#include <array>
#include <thread>
#include <chrono>
#include <cmath>
#include <math.h>
#include <QElapsedTimer>
#include <functional>
#include <atomic>

#include <filesystem>
#include <filesystem>  //（按原文件保留重复包含）
namespace fs = std::filesystem;

/**********************  工程内头文件（保留原顺序注释）  **********************/
// #include "websocketclient.h"
#include "myclient.h"
#include "tools.h"
#include "websocketthread.h"
#include <QHash>
#include <QQueue>
#include "Logger.h"
#include "autopolaralignment.h"
#include "autofocus.h"
#include "SerialDeviceDetector.h"
#include "guiding/GuiderCore.h"
#include "guiding/SimGuiderFrameSource.h"
#include <stellarsolver.h>

#include "sdks/SdkSerialExecutor.h"
#include "sdks/SdkCommon.h"  // SDK 通用类型（SdkFrameData, SdkChipInfo, SdkAreaInfo 等）

/**********************  宏与常量定义  **********************/
// #define QT_Client_Version getBuildDate()
#define QT_Client_Version "20260323-4"  // 手动指定版本号

#define GPIO_PATH "/sys/class/gpio"
#define GPIO_EXPORT "/sys/class/gpio/export"
#define GPIO_UNEXPORT "/sys/class/gpio/unexport"
#define GPIO_PIN_1 "516"
#define GPIO_PIN_2 "527"
#define BIN_SIZE 20 // 图像固定 bin 尺寸

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
    void clearBoxCache(bool clearCache = true,
                       bool clearUpdatePack = false,
                       bool clearBackup = false);

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
     * @brief 🆕 获取指定设备类型当前使用的 SDK 驱动名（完全自动化）
     * @param deviceType 设备类型（"MainCamera", "GuideCamera", "Focuser"等）
     * @return SDK 驱动名（如 "QHYCCD", "ASI"），如果未设置或不支持则返回空字符串
     * 
     * 功能：自动从 SdkDriverRegistry 查询 DriverIndiName 对应的 SDK 驱动名
     * 
     * 使用示例：
     * @code
     * QString sdkDriver = getSDKDriverName("MainCamera");
     * if (!sdkDriver.isEmpty()) {
     *     // 使用 sdkDriver.toStdString() 调用 SdkManager
     * }
     * @endcode
     */
    QString getSDKDriverName(const QString& deviceType);

    /**
     * @brief 判断主相机是否为 SDK 模式
     * @return SDK 模式返回 true，INDI 模式返回 false
     */
    bool isMainCameraSDK();

    /**
     * @brief 判断主相机是否已连接（支持 SDK 和 INDI 两种模式）
     * @return 已连接返回 true，否则返回 false
     */
    bool isMainCameraConnected();

    /**
     * @brief 判断电调是否为 SDK 模式
     * @return SDK 模式返回 true，INDI 模式返回 false
     */
    bool isFocuserSDK();

    /**
     * @brief 判断赤道仪是否为 SDK 模式
     * @return SDK 模式返回 true，INDI 模式返回 false
     * @note TODO: 需要实现赤道仪SDK模式的支持，目前仅预留接口
     */
    bool isMountSDK();

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
     * @brief 判断设备类型是否支持 SDK 连接模式
     * @param description 设备描述（如 "MainCamera"）
     * @param driverName 驱动名称
     * @return 支持返回 true
     */
    bool isDeviceTypeSupportSDK(const QString &description, const QString &driverName);

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
     * @brief SDK Burst 拍摄（仅 QHYCCD SDK 支持）
     * @param Exp_ms 每帧曝光时间（毫秒）
     * @param frames 采集帧数（建议 1~1024）
     *
     * Burst 模式基于 QHYCCD Live 模式（SetStreamMode=1 + BeginLive/GetLiveFrame/StopLive），
     * 内部会对多帧做均值叠加输出为 1 张图（仍走现有 FITS/PNG/JPG 链路）。
     */
    void SDK_BurstCapture(int Exp_ms, int frames);

    // 主相机采集模式：
    // - Single：单帧（StreamMode=0，走 ExpQHYCCDSingleFrame/GetSingleFrame）
    // - Live：连续取帧（StreamMode=1 + BeginLive + 循环 GetLiveFrame）
    // - Burst：Burst 子模式（EnableBurstMode(true) + Live + SetBurstIDLE/ReleaseBurstIDLE）
    enum class MainCameraCaptureMode { Single, Live, Burst };

    // 当前主相机采集模式（默认 Single）
    MainCameraCaptureMode mainCameraCaptureMode = MainCameraCaptureMode::Single;

    // SDK 主相机“已应用”的采集模式（用于判断是否发生了模式切换）
    // 说明：QHYCCD SDK 的 StreamMode/ReadMode 往往需要在 InitQHYCCD 之前设置；
    // 因此在模式切换时，我们会 Close->Open->按 Demo 顺序重新 Init。
    bool sdkMainAppliedModeValid = false;
    MainCameraCaptureMode sdkMainAppliedMode = MainCameraCaptureMode::Single;

    // SDK Burst（连接模式子模式）状态：表示“已在连接阶段完成 EnableBurstMode+Live+IDLE 初始化”
    std::atomic_bool sdkMainBurstModeReady{false};
    std::atomic_bool sdkMainLiveReady{false};

    // SDK Live（主相机）循环取帧：切到 Live 模式时启用，切出时停止
    QTimer *sdkMainLiveTimer = nullptr;
    std::atomic_bool sdkMainLiveLoopOn{false};
    std::atomic_bool sdkMainLiveFrameInFlight{false};
    // SDK Live（主相机）后处理定时器（主线程）：从“最新帧邮箱”取最新帧，按限帧刷新前端（FITS->PNG/瓦片）
    QTimer *sdkMainLiveProcessTimer = nullptr;
    // Live 拉帧退避（ms since epoch）：用于在 BeginLive 初期/失败时降低 GetLiveFrame 频率，避免刷爆驱动
    std::atomic<long long> sdkMainLiveNextPollMs{0};
    // Live 图像处理（FITS->PNG/瓦片）是否正在进行：用于在处理过慢时丢弃中间帧，避免卡顿/积压
    std::atomic_bool sdkMainLiveProcessingBusy{false};
    // Live“处理链路”限帧：高帧率下只处理部分帧（其余帧仅取到即丢弃），避免主线程被瓦片/IO压死、WS不稳定
    // - 仅影响后处理链路（SaveQhyFrameDataToFits/saveFitsAsPNG/TileGPM），不影响 SDK 拉帧与 FPS 统计
    std::atomic<long long> sdkMainLiveLastProcessMs{0};
    int sdkMainLiveMaxProcessFps = 5; // 建议 3~5；过高会导致前端请求风暴与跳帧

    // Live 最新帧序号（用于“是否有新帧”判断）
    std::atomic_uint64_t sdkMainLiveLatestSeq{0};
    std::atomic_uint64_t sdkMainLiveProcessedSeq{0};

    // Live 最新帧邮箱（进程内）：避免每帧 memcpy 到 /dev/shm 再读回，降低 CPU/内存带宽占用。
    // - 取帧线程（sdkCamExec）写入 latestFrame，并递增 sdkMainLiveLatestSeq
    // - 主线程（sdkMainLiveProcessTimer）读取并处理；处理慢会自然丢弃中间帧（只取最新）
    mutable std::mutex sdkMainLiveLatestFrameMutex;
    std::shared_ptr<SdkFrameData> sdkMainLiveLatestFrame;

    // Live 共享内存（/dev/shm 文件 mmap）：对外提供“最新一帧”原始像素（可能被覆盖）
    int    sdkMainLiveShmFd{-1};
    void*  sdkMainLiveShmPtr{nullptr};
    size_t sdkMainLiveShmSize{0};
    size_t sdkMainLiveShmFrameBytes{0};

    /**
     * @brief 终止 INDI 拍摄
     */
    void INDI_AbortCapture();

    /**
     * @brief 聚焦回路（循环）
     */
    void FocusingLooping();
    
    /**
     * @brief 保存 SDK 模式下获取的 SdkFrameData 为 FITS 文件
     * @param frame SDK 相机帧数据
     * @param filepath FITS 文件保存路径
     */
    void SaveQhyFrameDataToFits(const SdkFrameData& frame, const std::string& filepath);
    
    // SDK 曝光定时器相关
    QTimer *sdkExposureTimer = nullptr;           // SDK 曝光图像获取定时器
    qint64 sdkExposureStartTime = 0;              // SDK 曝光开始时间戳（毫秒）
    int sdkExposureExpectedDuration = 0;          // SDK 预期曝光时长（毫秒）
    bool sdkExposureIsROI = false;                // SDK 当前曝光是否为 ROI 模式

    // SDK Burst（QHY Live）相关：不走 sdkExposureTimer 轮询，而是在 sdkCamExec 中串行抓帧
    std::atomic_bool sdkBurstActive{false};               // Burst 是否在执行
    std::atomic_bool sdkBurstCancelRequested{false};      // Burst 是否请求取消（abortExposure）

    // 根据主相机采集模式初始化/释放（连接后或模式切换时调用）
    void applySdkMainCameraCaptureMode();

    // SDK 主相机 Live 循环取帧回调
    void onSdkMainLiveTimerTimeout();
    // SDK 主相机 Live 后处理回调（主线程）
    void onSdkMainLiveProcessTimerTimeout();

    // Live “最新帧共享内存”维护
    void cleanupSdkMainLiveShm();
    bool ensureSdkMainLiveShm(size_t frameBytes);
    void writeSdkMainLiveShm(const SdkFrameData& frame, uint64_t seq);

    // SDK 导星相机（Guider）循环曝光相关：独立于主相机 SDK 曝光，避免互相抢占定时器/状态
    QTimer *sdkGuiderExposureTimer = nullptr;     // SDK 导星曝光图像获取定时器
    qint64 sdkGuiderExposureStartTime = 0;        // SDK 导星曝光开始时间戳（毫秒）
    int sdkGuiderExposureExpectedDuration = 0;    // SDK 导星预期曝光时长（毫秒）

    // SDK 串行执行线程：避免在主线程执行阻塞式 SDK 调用
    // 注意：相机与电调分开，避免相机读帧阻塞导致电调命令排队延迟
    std::unique_ptr<SdkSerialExecutor> sdkCamExec;
    std::unique_ptr<SdkSerialExecutor> sdkFocuserExec;
    std::atomic_bool sdkFrameTaskInFlight{false};           // 相机读帧任务是否在执行
    std::atomic_bool sdkGuiderFrameTaskInFlight{false};     // 导星相机读帧任务是否在执行（SDK）
    std::atomic_bool sdkFocuserPeriodicTaskInFlight{false}; // 电调周期任务是否在执行
    std::atomic_bool sdkFocuserPosTaskInFlight{false};      // 单次位置刷新任务是否在执行（用于合并请求）
    std::atomic_int  sdkFocuserPosCache{0};                 // SDK 电调位置缓存（避免主线程阻塞串口）
    std::atomic_bool sdkFocuserPosValid{false};             // 位置缓存是否有效
    // 电调操作代际号：用于让“旧任务”（尤其是位置读取/旧移动）在 SDK 线程中快速失效，
    // 避免 stop 末尾的定时器位置读取队列占满串口，导致后续 move/abort 被长期排队。
    std::atomic_uint64_t sdkFocuserOpEpoch{0};

    // 请求一次“电调位置刷新”（异步）：在 SDK 线程读取真实位置，回主线程更新缓存/可选推送 WS
    void requestSdkFocuserPositionUpdate(bool emitWs = false);
    // 请求一次“电调版本刷新”（异步）：避免在主线程触碰 QSerialPort（否则会触发 QSocketNotifier 跨线程警告）
    void requestSdkFocuserVersionUpdate(bool emitWs = true);
    
    /**
     * @brief SDK 曝光定时器回调：轮询获取图像
     */
    void onSdkExposureTimerTimeout();

    /**
     * @brief SDK 导星曝光定时器回调：轮询获取导星图像（用于 GuiderLoopExpSwitch）
     */
    void onSdkGuiderExposureTimerTimeout();

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
    // 实际执行（可能耗时）的实现：由 saveFitsAsPNG() 异步调度调用
    int saveFitsAsPNG_Worker(QString fitsFileName, bool ProcessBin);

    // 视口驱动的瓦片生成（按当前 zoom/位置优先生成视口内 z/x/y）
    void scheduleViewportTileGeneration();
    void generateViewportTiles_Once(quint64 epoch, quint64 requestSeq, int budgetMs);
    /** 同步生成当前视口要显示的瓦片，确保发送 GPM 前前端请求的瓦片已落盘，避免 404；无视口时退化为 z=0 全层 */
    void generateVisibleTilesSync(quint64 epoch);
    static int calculateTileLevelFromScale(double scale, int maxZoomLevel);
    static QString buildTileSessionId(quint64 frameId);
    int currentTilePreviewBinning() const;

    /**
     * @brief 瓦片金字塔全局处理元数据 (GPM - Global Processing Metadata)
     * 包含整幅图像的统计信息，用于前端统一处理参数
     */
    struct TileGPM {
        int imageWidth;           // 原图宽度
        int imageHeight;          // 原图高度
        // 兼容：前端仍需要一套“用于解析/预览保存”的尺寸信息（可能做过软件 bin），用于对照调试
        // - previewWidth/previewHeight: 预览/解析保存用图像（image16）的尺寸
        // - previewBinningFactor: 生成 image16 时使用的软件 bin 因子（1/2/4/8/16）
        // 说明：imageWidth/imageHeight 始终表示瓦片金字塔基准层（z=maxZoomLevel）的尺寸（当前版本由 originalImage16 构建）
        int previewWidth = 0;
        int previewHeight = 0;
        int previewBinningFactor = 1;
        int tileSize;             // 瓦片尺寸 (默认512)
        int maxZoomLevel;         // 最大缩放层级 (z=0为最高分辨率)
        double globalMin;         // 全局最小值
        double globalMax;         // 全局最大值
        double globalMean;        // 全局均值
        double globalStdDev;      // 全局标准差
        uint16_t blackLevel;      // 自动拉伸黑点
        uint16_t whiteLevel;      // 自动拉伸白点
        QString cfa;              // CFA模式 (RGGB, BGGR, GRBG, GBRG, 空=Mono)
        double gainR;             // R通道增益
        double gainB;             // B通道增益
        QString sessionId;        // 会话ID (用于瓦片缓存)
        quint64 frameId = 0;      // 帧ID（与 tilePyramidEpoch/epoch 对齐，用于前后端丢弃旧帧/防错帧）
        QString buildMode = "pyramid"; // 瓦片构建模式：pyramid / merged_single_level

        // 直方图（用于前端拉伸/显示）
        int histogramBins = 0;                 // bin 数（建议 256）
        uint64_t histogramTotal = 0;           // 总像素数
        std::vector<uint32_t> histogram;       // bin 计数
    };


    /**
     * @brief 生成瓦片金字塔
     * @param image16 16位原始图像
     * @param sessionId 会话ID
     * @param cfa CFA模式
     * @param maxMergeFactor 最低精度层的合并倍数（2^N，范围建议[1,16]）。例如 16 表示生成 16x16->...->1x1 共 5 层。
     * @param enableHistogram 是否统计直方图（用于前端拉伸/显示）；默认 false 以减少大图耗时
     * @return 生成的GPM元数据
     */
    TileGPM generateTilePyramid(const cv::Mat& image16, const QString& sessionId, const QString& cfa, int maxMergeFactor = 16, bool enableHistogram = false);

    /**
     * @brief 计算图像全局处理元数据
     * @param image16 16位原始图像
     * @param cfa CFA模式
     * @param maxMergeFactor 最低精度层的合并倍数（2^N，范围建议[1,16]）
     * @param enableHistogram 是否统计直方图（为 false 时仅计算 min/max/mean/stdDev/blackLevel/whiteLevel）；默认 false
     * @return GPM元数据
     */
    TileGPM calculateGPM(const cv::Mat& image16, const QString& cfa, int maxMergeFactor = 16, bool enableHistogram = false);

    /**
     * @brief 计算白平衡增益（基于灰度世界算法）
     * @param image16 16位原始图像
     * @param cfa CFA模式
     * @return QPair<gainR, gainB> R和B通道的增益值
     */
    QPair<double, double> calculateWhiteBalanceGains(const cv::Mat& image16, const QString& cfa);

    /**
     * @brief 保存单个瓦片
     * @param tile 瓦片数据
     * @param z 缩放层级
     * @param x 列索引
     * @param y 行索引
     * @param sessionId 会话ID
     * @param border 瓦片边界扩展像素数（用于前端局部 Bayer 转换避免接缝；前端渲染时会裁剪掉该边界）
     */
    void saveTile(const cv::Mat& tile, int z, int x, int y, const QString& sessionId, int border = 0);

    /** 内部使用：写入瓦片文件，假定目录已存在（避免每个瓦片都 mkpath，配合目录预创建使用） */
    void saveTileFast_NoMkdir(const cv::Mat& tile, const QString& tileFilePath, int border);

    /**
     * @brief 发送GPM元数据到前端
     * @param gpm GPM元数据
     */
    void sendGPMToClient(const TileGPM& gpm);

    /**
     * @brief 发送直方图数据到前端（与 TileGPM 分开，避免破坏既有解析）
     * @param gpm GPM（包含 sessionId 与 histogram 字段）
     */
    void sendHistogramToClient(const TileGPM& gpm);

    /**
     * @brief 清理旧的直方图文件
     * @param keepCount 保留最近的文件数量（默认5个）
     */
    void cleanupOldHistogramFiles(int keepCount = 5);
    void cleanupOldTileSessions(int keepCount = 5);

    /**
     * @brief 清理旧的瓦片会话目录（保留当前会话目录）
     * @param keepSessionId 当前会话 ID，该目录不删除
     */
    void cleanupOldTileSessionDirs(const QString& keepSessionId);

    // 瓦片相关配置
    int tilePyramidTileSize = 512;                    // 瓦片尺寸
    // 瓦片与直方图放在 tmpfs，避免 SD 卡频繁写/删；nginx 需 alias /img/capture-tiles/ -> 本目录
    std::string tilePyramidPath = "/dev/shm/capture-tiles/";

    // 瓦片生成性能/节流（目标：前端首次可见内容在 <100ms 内到达；完整金字塔后台补齐）
    std::atomic_uint64_t tilePyramidEpoch{0};         // 每次新帧生成 ++，用于取消旧任务
    int tilePyramidFastBudgetMs = 100;                // 同步阶段预算（毫秒）
    int tilePyramidFastSyncMaxZ = 1;                  // 同步生成的最大层级（z=0 为最低精度）；其余后台生成
    bool tilePyramidFastEnableMedianBlur = false;     // 同步阶段是否做 medianBlur（大图可能超时）
    QString tileBuildMode = QStringLiteral("pyramid"); // 瓦片构建模式：金字塔 / 合并图+单层细化

    // 前端视口参数（来自 Vue_Command: sendVisibleArea:x:y:scale）
    std::atomic<double> tileViewportX{0.0};           // 视口中心 X（原图像素）
    std::atomic<double> tileViewportY{0.0};           // 视口中心 Y（原图像素）
    std::atomic<double> tileViewportScale{1.0};       // 缩放比例（0.1~1.0；越小越放大）
    std::atomic_uint64_t tileViewportRequestSeq{0};   // 每次 sendVisibleArea ++，用于打断旧视口瓦片生成
    double tileViewportAspect = 16.0 / 9.0;           // 视口宽高比（与前端 CanvasWidth/CanvasHeight 一致；默认 16:9）

    // 最新一帧的瓦片源图与元数据（用于“拖动/缩放时按需补瓦片”，避免反复 readFits）
    struct TileFrameState {
        quint64 epoch = 0;
        QString sessionId;
        int imageWidth = 0;
        int imageHeight = 0;
        int previewBinningFactor = 1;
        int tileSize = 512;
        int maxZoomLevel = 0;
        QString cfa;
    };
    mutable std::mutex tileFrameMutex;
    TileFrameState tileFrame;
    std::shared_ptr<cv::Mat> tileFrameImage16;        // CV_16UC1 原图（maxZoomLevel层）

    std::atomic_bool tileViewportGenInFlight{false};
    std::atomic_bool tileViewportGenPending{false};

    // “已生成瓦片”去重（同一 epoch 内避免重复写同一 z/x/y）
    mutable std::mutex tileGenDoneMutex;
    quint64 tileGenDoneEpoch = 0;
    std::unordered_set<uint64_t> tileGenDoneKeys;


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

    /**********************  导星相机（INDI 直出图，替代 PHD2）  **********************/
public:
    // 导星循环曝光开关（用于导星相机预览/取图，不涉及 PHD2）
    bool isGuiderLoopExp = false;
    // 导星相机曝光时间（ms）
    int guiderExpMs = 1000;

private:
    // 内置导星核心；未初始化时保持为空，相关逻辑自动降级为仅取图/显示。
    GuiderCore *guiderCore = nullptr;
    std::unique_ptr<guiding::SimGuiderFrameSource> simGuiderFrameSource;
    // 导星循环曝光定时器（singleShot：收到一帧后再触发下一帧，避免重入）
    QTimer *guiderLoopTimer = nullptr;
    bool guiderExposureInFlight = false;
    // 用于像素尺度换算（arcsec/px）的导星相机/镜筒参数缓存。
    double guiderPixelSizeUm = 0.0;
    double guiderFocalLengthMm = 0.0;
    bool guiderScaleHintSent = false;
    // 导星 UI 叠加层缓存（供前端复用既有 PHD2Box/Cross/MultiStar 协议）。
    int glPHD_CurrentImageSizeX = 0;
    int glPHD_CurrentImageSizeY = 0;
    bool guiderMultiStarSecondaryPtsPending = false;
    bool guiderPhaseGuiding = false;
    bool guiderDirectionDetectActive = false;
    bool guiderForceRecalibrateOnNextStart = false;
    int guiderChartSampleIndex = 0;
    QVector<QPointF> guiderMultiStarSecondaryPtsPx;

private Q_SLOTS:
    void onGuiderLoopTimeout();
    void PersistGuidingFits(const QString& sourceFitsPath);

public:
    void ControlGuide(int Direction, int Duration);
    void ControlGuideEx(int Direction, int Duration, const QString& source);

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
    int autoFocusExposureTime = 1000;      // 自动对焦曝光时间(ms)，仅作用于自动对焦流程
    int autoFocusCoarseDivisions = 10;     // 粗调分段数（总行程 / 此值），默认 10

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

    /** FocusingLooping 在真正下发曝光前记录：与当前帧 FITS/.bin 像素对应的传感器 ROI 原点（scaled 空间）及坐标系倍率 */
    bool lastFocusExposureSnapshotValid = false;
    int lastFocusExposureScaledX = 0;
    int lastFocusExposureScaledY = 0;
    int lastFocusExposureRoiCoordScale = 1;
    /** 本次曝光请求的 ROI 宽高（传感器像素，与 SetResolution/setCCDFrameInfo 一致）；用于全幅 FITS 下裁剪 .bin */
    int lastFocusExposureRoiW = 0;
    int lastFocusExposureRoiH = 0;

    /**
     * @brief 启动自动对焦流程
     */
    void startAutoFocus();
    // 新：仅从当前位置执行 HFR 精调（固定步长 100、采样 11 点）
    void startAutoFocusFineHFROnly();
    void startAutoFocusSuperFineOnly();
    
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
    double guiderCameraOffset = 0.0;  // 导星相机偏置
    double guiderCameraGain = 0.0;    // 导星相机增益
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
    int glExpTime = 1000;            // 主相机曝光时间（ms）
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
    // 计划任务表：避免 Refocus=ON 时在 startSetCFW <-> AutoFocus 回调之间无限重入
    // 语义：记录“已经为哪个 schedule_currentNum 行触发过一次 Refocus（自动对焦）”
    int schedule_refocusTriggeredIndex = -1;
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
     * @brief 赤道仪移动前，如当前在导星则暂时停止导星（记录状态，供结束后恢复）
     */
    void pauseGuidingBeforeMountMove();

    /**
     * @brief 赤道仪移动完成后，如移动前在导星则自动恢复导星
     */
    void resumeGuidingAfterMountMove();

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

    // -------- 赤道仪移动与导星协调 --------
    // 记录“赤道仪开始移动前是否处于导星状态”，用于移动完成后自动恢复导星
    bool wasGuidingBeforeMountMove = false;

    /**********************  文件/目录与 USB 相关  **********************/
public:
    // 图像保存根目录（运行时在构造函数中初始化）
    // 默认：系统家目录下的 ~/images
    // 可通过环境变量覆盖：QUARCS_IMAGE_SAVE_ROOT="/path/to/images"
    std::string ImageSaveBasePath = "";        // e.g. /home/user/images
    QString     ImageSaveBaseDirectory = "";   // 与 ImageSaveBasePath 一致（QString 版本）
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
    QString sdkMainCameraId;       // SDK 主相机 cameraId（用于 CFWList 等稳定 key；避免依赖 INDI 的 FILTER_NAME）

    /**
     * @brief 清理 QHYCCD SDK 句柄池并释放 SDK 全局资源（适用于"断开全部/断开指定驱动"等场景）
     * @param reason 清理原因（用于日志记录）
     * @param deviceType 要清理的设备类型："All"（清理所有）、"MainCamera"（仅主相机）、"Focuser"（仅电调）、"CameraPool"（仅相机池，不清理主相机绑定）
     * @note 不会修改 systemdevicelist 里的 isSDKConnect（保留用户模式选择），只会清理 isConnect/isBind/DeviceIndiName
     */
    void cleanupQhySdkPoolAndResource(const QString& reason, const QString& deviceType = "All");

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
    void CopyImagesToUsb(QStringList RemoveImgPath, QString usbName = "");

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
    QString localAppVersion = ""; // 本地应用版本

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

    /**
     * @brief 关闭当前热点指定秒数后再重新启动
     * @param delaySeconds 关闭时长（秒），默认 10 秒
     */
    void restartHotspotWithDelay(int delaySeconds = 10);

    /**********************  网络模式（AP/WAN）  **********************/
public:
    /**
     * @brief 查询网络模式状态（由 net-mode.sh 输出 JSON）
     */
    void requestNetStatus();

    /**
     * @brief 切换网络模式（ap/wan）
     * @param mode "ap" or "wan"
     */
    void switchNetMode(const QString &mode);

    /**********************  Wi‑Fi 配置（扫描/保存）  **********************/
public:
    /**
     * @brief 扫描附近 Wi‑Fi 热点
     * @note 通过 nmcli，结果回传：WiFiScan|[{"ssid","signal","security"},...]
     */
    void wifiScan();

    /**
     * @brief 保存/更新上级 Wi‑Fi profile（默认建议用固定 con-name：wan-uplink）
     * @note 前端发送：wifiSaveB64|<base64(JSON)>
     *       JSON 示例：{"name":"wan-uplink","ssid":"xxx","psk":"yyy"}
     */
    void wifiSaveFromB64Payload(const QString &b64Payload);

private:
    /**
     * @brief 以异步方式执行 sudo 命令，避免阻塞主线程
     */
    void runSudoAsync(const QString &program, const QStringList &args,
                      const std::function<void(int, const QString &, const QString &)> &onDone);

/**********************  串口/设备路径  **********************/
public:
    SerialDeviceDetector detector; // 串口设备检测器

    // 前端手动选择的串口（仅保存在内存中，不写入配置）
    QString mountSerialPortOverride;
    QString focuserSerialPortOverride;

    /**
     * @brief 获取当前连接的串口列表
     * @return 串口设备路径列表（如 /dev/ttyUSB0）
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

    /**
     * @brief 向前端发送指定驱动类型的串口候选列表及当前已保存的串口
     * @param driverType 驱动类型（"Mount" / "Focuser"）
     *
     * 消息格式：
     *   SerialPortOptions:<driverType>:<savedPort>:<port1>:<port2>:...
     */
    void sendSerialPortOptions(const QString &driverType);

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
    static const int COMMAND_DEBOUNCE_MS = 100; // 防抖时间窗口（500毫秒），短时间内重复的完整消息（命令和参数都相同）只执行一次

/**********************  线程/定时器（通用）  **********************/
public:
    bool one_touch_connect = true;        // 一键连接开关
    bool one_touch_connect_first = true;  // 一键连接首次标志
    int glMainCCDSizeX = 0;               // 主相机宽
    int glMainCCDSizeY = 0;               // 主相机高
    double CameraTemperature = 16;         // 当前相机温度
    int glOffsetValue = 0, glOffsetMin = 0, glOffsetMax = 0; // 偏置范围
    int glGainValue = 0, glGainMin = 0, glGainMax = 0;       // 增益范围
    int glGuiderOffsetValue = 0, glGuiderOffsetMin = 0, glGuiderOffsetMax = 0; // 导星偏置范围
    int glGuiderGainValue = 0, glGuiderGainMin = 0, glGuiderGainMax = 0;       // 导星增益范围
    int glUsbTrafficValue = 0, glUsbTrafficMin = 0, glUsbTrafficMax = 0, glUsbTrafficStep = 1; // USB_TRAFFIC 范围（可选）

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
 
    // Web 前端静态资源目录（URL /img 的真实根目录）
    // - 部署环境：通常为 /var/www/html/img/
    // - 开发环境：apps/web-frontend 的 server.py 从 dist/ 提供静态文件，此时 /img 映射到 dist/img
    //   推荐通过环境变量 QUARCS_WEB_IMG_ROOT 覆盖（见 mainwindow.cpp），而不是改这里。
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
