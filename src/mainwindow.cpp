#include "mainwindow.h"
#include "quarcs_build_version.h"
#include <functional>

// SDK 框架相关头文件
#include "sdks/SdkManager.h"
#include "sdks/SdkCommon.h"  // 包含统一的 SDK 类型定义（SdkControlParamInfo, SdkFocuserOpenParam 等）
#include "sdks/SdkDriverRegistry.h"  // 🆕 驱动注册表
#include "sdks/SdkSerialExecutor.h"

#include <cstdint>
#include <cstddef>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <future>
#include <memory>
#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <unordered_set>
#include <chrono>
#include <utility>

// Qt 相关头文件（用于文件/目录操作）
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QDataStream>
#include <QPointer>
#include <QSet>
#include <QVector>
#include <QTemporaryFile>
#include <QCoreApplication>
#include <QThread>
#include <QProcess>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrentRun>

// 系统调用相关头文件（用于 FIFO 操作）
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>

// ============================================================================
// INDI 设备指针（全局变量）
// ============================================================================

INDI::BaseDevice *dpMount      = nullptr;  // 赤道仪设备指针
INDI::BaseDevice *dpGuider     = nullptr;  // 导星相机设备指针
INDI::BaseDevice *dpPoleScope  = nullptr;  // 极轴镜设备指针
INDI::BaseDevice *dpMainCamera = nullptr;  // 主相机设备指针
INDI::BaseDevice *dpFocuser    = nullptr;  // 电调设备指针
INDI::BaseDevice *dpCFW        = nullptr;  // 滤镜轮设备指针

namespace {

inline bool isPoleMasterNameLocal(const QString &name)
{
    return name.contains("POLEMASTER", Qt::CaseInsensitive);
}

template <typename Func>
void postGuiderCore(GuiderCore *core, Func &&func)
{
    if (!core)
        return;
    QMetaObject::invokeMethod(core,
                              [core, fn = std::forward<Func>(func)]() mutable {
                                  fn(core);
                              },
                              Qt::QueuedConnection);
}

} // namespace

// ============================================================================
// SDK 设备句柄（当设备使用 SDK 模式时使用对应句柄）
// ============================================================================

SdkDeviceHandle sdkMountHandle      = nullptr;  // SDK 赤道仪句柄
SdkDeviceHandle sdkGuiderHandle     = nullptr;  // SDK 导星相机句柄
SdkDeviceHandle sdkPoleScopeHandle = nullptr;  // SDK 极轴镜句柄
SdkDeviceHandle sdkMainCameraHandle = nullptr; // SDK 主相机句柄
SdkDeviceHandle sdkFocuserHandle   = nullptr;  // SDK 电调句柄
SdkDeviceHandle sdkCFWHandle        = nullptr;  // SDK 滤镜轮句柄

QString sdkFocuserPort;  // SDK 电调串口路径

namespace {
}

// ============================================================================
// QHYCCD SDK 多相机句柄池（用于"设备分配"逻辑）
// ============================================================================
// 设计说明：
// - 启动/连接时扫描到多少台相机就全部打开，放入池中
// - 通过前端 BindingDevice 的 DeviceIndex 选择具体哪一台分配给 MainCamera 等角色
// - 为了复用现有前端协议（DeviceIndex 是 int），SDK 相机使用负数 index 编码：
//     uiIndex = -(poolIndex + 1)  =>  poolIndex = -uiIndex - 1

QVector<SdkDeviceHandle> g_sdkQhyCamHandles;      // SDK 相机句柄池
QVector<QString>         g_sdkQhyCamIds;          // SDK 相机 ID 池（与句柄一一对应）
int                      g_sdkMainCameraPoolIndex = -1;  // 当前分配给 MainCamera 的池索引（-1 表示未分配）
int                      g_sdkGuiderPoolIndex = -1;      // 当前分配给 Guider 的池索引（-1 表示未分配）
int                      g_sdkPoleCameraPoolIndex = -1;  // 当前分配给 PoleCamera 的池索引（-1 表示未分配）

namespace {
struct SyncCommandResult {
    int exitCode = -1;
    QString out;
    QString err;
    bool finished = false;
};

SyncCommandResult runCommandSync(const QString &program, const QStringList &args, int timeoutMs = 3000)
{
    QProcess process;
    process.start(program, args);
    if (!process.waitForStarted(timeoutMs)) {
        return {-1, QString(), process.errorString(), false};
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return {124,
                QString::fromUtf8(process.readAllStandardOutput()).trimmed(),
                QString::fromUtf8(process.readAllStandardError()).trimmed() + "\n(timeout)",
                false};
    }
    return {process.exitCode(),
            QString::fromUtf8(process.readAllStandardOutput()).trimmed(),
            QString::fromUtf8(process.readAllStandardError()).trimmed(),
            true};
}

SyncCommandResult runSudoSync(const QString &program, const QStringList &args, int timeoutMs = 3000)
{
    QStringList sudoArgs;
    sudoArgs << "-n" << program;
    sudoArgs << args;
    return runCommandSync("sudo", sudoArgs, timeoutMs);
}

bool isValidSystemDeviceIndex(const SystemDeviceList &deviceList, int index)
{
    return index >= 0 && index < deviceList.system_devices.size();
}

void drawFocusDebugCrosshair(cv::Mat &image16, double x, double y)
{
    if (image16.empty() || image16.type() != CV_16UC1) {
        return;
    }

    const int cx = static_cast<int>(std::lround(x));
    const int cy = static_cast<int>(std::lround(y));
    if (cx < 0 || cy < 0 || cx >= image16.cols || cy >= image16.rows) {
        return;
    }

    const int armBase = std::max(6, std::min(image16.cols, image16.rows) / 20);
    const int arm = armBase * 10;
    const int x0 = std::max(0, cx - arm);
    const int x1 = std::min(image16.cols - 1, cx + arm);
    const int y0 = std::max(0, cy - arm);
    const int y1 = std::min(image16.rows - 1, cy + arm);
    const cv::Scalar white(std::numeric_limits<unsigned short>::max());
    const cv::Scalar black(0);

    cv::line(image16, cv::Point(x0, cy), cv::Point(x1, cy), black, 3, quarcs_cv_compat::kLine8);
    cv::line(image16, cv::Point(cx, y0), cv::Point(cx, y1), black, 3, quarcs_cv_compat::kLine8);
    cv::line(image16, cv::Point(x0, cy), cv::Point(x1, cy), white, 1, quarcs_cv_compat::kLine8);
    cv::line(image16, cv::Point(cx, y0), cv::Point(cx, y1), white, 1, quarcs_cv_compat::kLine8);
}

}

// 索引转换辅助函数
static inline int sdkUiIndexFromPoolIndex(int poolIndex)
{
    return -(poolIndex + 1);
}

// SDK 非“池化设备”（例如电调）使用固定负数 index，避免与 INDI 的正 index 冲突，也避免与相机池的 -(n+1) 冲突。
// 约定：相机池使用 [-1, -2, ...]；固定设备从 -10000 往下分配。
static constexpr int SDK_FOCUSER_UI_INDEX = -10001;

static constexpr int kIndiFocuserRelMoveChunkMax = 10000;

static inline int sdkPoolIndexFromUiIndex(int uiIndex)
{
    return -uiIndex - 1;
}

static inline bool sdkPoolIndexValid(int poolIndex)
{
    return poolIndex >= 0 &&
           poolIndex < g_sdkQhyCamHandles.size() &&
           poolIndex < g_sdkQhyCamIds.size() &&
           !g_sdkQhyCamIds[poolIndex].isEmpty();
}

QhyCameraBiasType MainWindow::classifyQhyCameraBias(const QString &cameraId) const
{
    if (cameraId.isEmpty())
        return QhyCameraBiasType::Neutral;
    if (isPoleMasterNameLocal(cameraId))
        return QhyCameraBiasType::Neutral;
    if (cameraId.contains("DEMO", Qt::CaseInsensitive))
        return QhyCameraBiasType::Neutral;
    if (cameraId.contains("5III", Qt::CaseInsensitive))
        return QhyCameraBiasType::GuiderPreferred;
    return QhyCameraBiasType::MainPreferred;
}

long long MainWindow::readQhyPixelCountByHandle(SdkDeviceHandle handle) const
{
    if (handle == nullptr)
        return -1;

    SdkCommand chipInfoCmd;
    chipInfoCmd.type = SdkCommandType::Custom;
    chipInfoCmd.name = "GetChipInfo";
    chipInfoCmd.payload = std::any();

    SdkResult chipInfoRes = SdkManager::instance().callByHandle(handle, chipInfoCmd);
    if (!chipInfoRes.success || !chipInfoRes.payload.has_value())
        return -1;

    try
    {
        const SdkChipInfo chipInfo = std::any_cast<SdkChipInfo>(chipInfoRes.payload);
        if (chipInfo.maxImageSizeX <= 0 || chipInfo.maxImageSizeY <= 0)
            return -1;
        return static_cast<long long>(chipInfo.maxImageSizeX) *
               static_cast<long long>(chipInfo.maxImageSizeY);
    }
    catch (const std::bad_any_cast &)
    {
        return -1;
    }
}

QString MainWindow::qhyAssignedRoleForPoolIndex(int poolIndex) const
{
    if (poolIndex == g_sdkPoleCameraPoolIndex)
        return "PoleCamera";
    if (poolIndex == g_sdkMainCameraPoolIndex)
        return "MainCamera";
    if (poolIndex == g_sdkGuiderPoolIndex)
        return "Guider";
    return QString();
}

void MainWindow::resetQhyAllocationState()
{
    qhyCameraPoolSnapshot.clear();
    qhyAllocationDraft = QhyAllocationDraft{};
    qhyAllocationFinal = QhyAllocationFinal{};
}

void MainWindow::syncQhyCameraPoolSnapshotFromGlobals()
{
    qhyCameraPoolSnapshot.clear();

    const int poolSize = std::max(g_sdkQhyCamHandles.size(), g_sdkQhyCamIds.size());
    qhyCameraPoolSnapshot.reserve(poolSize);

    for (int i = 0; i < poolSize; ++i)
    {
        const SdkDeviceHandle handle = (i < g_sdkQhyCamHandles.size()) ? g_sdkQhyCamHandles[i] : nullptr;
        const QString cameraId = (i < g_sdkQhyCamIds.size()) ? g_sdkQhyCamIds[i].trimmed() : QString();

        if (handle == nullptr && cameraId.isEmpty())
            continue;

        QhyCameraPoolEntry entry;
        entry.poolIndex = i;
        entry.uiIndex = sdkUiIndexFromPoolIndex(i);
        entry.cameraId = cameraId;
        entry.displayName = cameraId;
        entry.isOpened = (handle != nullptr);
        entry.isDemo = cameraId.contains("DEMO", Qt::CaseInsensitive);
        entry.isPoleMaster = isPoleMasterNameLocal(cameraId);
        entry.is5III = cameraId.contains("5III", Qt::CaseInsensitive);
        entry.biasType = classifyQhyCameraBias(cameraId);
        entry.pixelCount = readQhyPixelCountByHandle(handle);
        entry.assignedRole = qhyAssignedRoleForPoolIndex(i);

        qhyCameraPoolSnapshot.push_back(entry);
    }
}

void MainWindow::syncQhyAllocationStateFromLegacyBindings()
{
    syncQhyCameraPoolSnapshotFromGlobals();

    qhyAllocationDraft = QhyAllocationDraft{};
    qhyAllocationFinal = QhyAllocationFinal{};
    const auto &deviceList = Tools::systemDeviceList();

    auto markRequested = [&](QhyRoleAllocation &role, int deviceIndex) {
        if (deviceIndex >= 0 && deviceIndex < deviceList.system_devices.size())
            role.requested = deviceList.system_devices[deviceIndex].isSDKConnect;
    };
    markRequested(qhyAllocationDraft.mainCamera, 20);
    markRequested(qhyAllocationDraft.guider, 1);
    markRequested(qhyAllocationDraft.poleCamera, 2);

    qhyAllocationDraft.scanComplete = true;
    qhyAllocationDraft.cameraCount = qhyCameraPoolSnapshot.size();

    auto assignRole = [&](QhyRoleAllocation &draftRole,
                          QhyFinalRoleBinding &finalRole,
                          int poolIndex,
                          const QString &fallbackCameraId,
                          bool manualLocked) {
        draftRole.manualLocked = manualLocked;
        draftRole.poolIndex = poolIndex;
        draftRole.assigned = (poolIndex >= 0) || !fallbackCameraId.isEmpty();
        draftRole.cameraId = fallbackCameraId;

        finalRole.poolIndex = poolIndex;
        finalRole.cameraId = fallbackCameraId;
        finalRole.lockState = manualLocked
            ? QhyAllocationLockState::ManualLocked
            : (draftRole.assigned ? QhyAllocationLockState::AutoAssigned
                                  : QhyAllocationLockState::Unassigned);

        if (poolIndex >= 0)
        {
            for (const auto &entry : qhyCameraPoolSnapshot)
            {
                if (entry.poolIndex == poolIndex)
                {
                    draftRole.cameraId = entry.cameraId;
                    finalRole.cameraId = entry.cameraId;
                    draftRole.assigned = true;
                    break;
                }
            }
        }
    };

    const bool mainManualLocked = (g_sdkMainCameraPoolIndex >= 0) &&
        (20 < deviceList.system_devices.size()) &&
        !deviceList.system_devices[20].DeviceIndiName.trimmed().isEmpty();
    const bool guiderManualLocked = (g_sdkGuiderPoolIndex >= 0) &&
        (1 < deviceList.system_devices.size()) &&
        !deviceList.system_devices[1].DeviceIndiName.trimmed().isEmpty();
    const bool poleManualLocked = (g_sdkPoleCameraPoolIndex >= 0) &&
        (2 < deviceList.system_devices.size()) &&
        !deviceList.system_devices[2].DeviceIndiName.trimmed().isEmpty();

    assignRole(qhyAllocationDraft.mainCamera,
               qhyAllocationFinal.mainCamera,
               g_sdkMainCameraPoolIndex,
               sdkMainCameraId.trimmed(),
               mainManualLocked);
    assignRole(qhyAllocationDraft.guider,
               qhyAllocationFinal.guider,
               g_sdkGuiderPoolIndex,
               (1 < deviceList.system_devices.size())
                   ? deviceList.system_devices[1].DeviceIndiName.trimmed()
                   : QString(),
               guiderManualLocked);
    assignRole(qhyAllocationDraft.poleCamera,
               qhyAllocationFinal.poleCamera,
               g_sdkPoleCameraPoolIndex,
               (2 < deviceList.system_devices.size())
                   ? deviceList.system_devices[2].DeviceIndiName.trimmed()
                   : QString(),
               poleManualLocked);

    const bool anyManualLocked =
        qhyAllocationDraft.mainCamera.manualLocked ||
        qhyAllocationDraft.guider.manualLocked ||
        qhyAllocationDraft.poleCamera.manualLocked;
    const bool anyAssigned =
        qhyAllocationDraft.mainCamera.assigned ||
        qhyAllocationDraft.guider.assigned ||
        qhyAllocationDraft.poleCamera.assigned;

    qhyAllocationDraft.source = anyManualLocked
        ? QhyAllocationSource::Manual
        : (anyAssigned ? QhyAllocationSource::Auto
                       : QhyAllocationSource::None);
    qhyAllocationFinal.scanComplete = qhyAllocationDraft.scanComplete;
}

// ============================================================================
// CFW (滤镜轮) SDK/INDI 兼容辅助函数
// ============================================================================
// 说明：前端协议使用 1-based 位置（CFW[1..N]），QHY SDK 内部使用 0-based（0..N-1）

/**
 * @brief 将前端 1-based 位置转换为 SDK 0-based 位置
 * @param pos1Based 前端位置（1-based）
 * @return SDK 位置（0-based）
 */
static inline int toSdkCfwPos0(int pos1Based)
{
    return std::max(0, pos1Based - 1);
}

/**
 * @brief 将 SDK 0-based 位置转换为前端 1-based 位置
 * @param pos0Based SDK 位置（0-based）
 * @return 前端位置（1-based）
 */
static inline int toUiCfwPos1(int pos0Based)
{
    return pos0Based + 1;
}

/**
 * @brief 生成 CFW 存储键名（用于 Tools::saveCFWList/readCFWList）
 * @param cameraId 相机 ID
 * @return 存储键名
 * @note 确保：
 *       - 不依赖 INDI 的 FILTER_NAME
 *       - 不与外置 CFW（dpCFW 的 deviceName）冲突
 *       - 多相机时也能区分
 */
static inline QString sdkCfwStorageKey(const QString& cameraId)
{
    if (!cameraId.isEmpty())
        return "SDK_CFW_" + cameraId;
    return "SDK_CFW_MainCamera";
}

/**
 * @brief 获取滤镜轮槽位数量
 * @param handle SDK 设备句柄
 * @param slotsNum 输出参数：槽位数量
 * @param errMsg 输出参数：错误信息（可选）
 * @return 成功返回 true，失败返回 false
 */
static bool sdkGetCfwSlotsNum(SdkDeviceHandle handle, int& slotsNum, std::string* errMsg = nullptr)
{
    SdkCommand cmd;
    cmd.type = SdkCommandType::Custom;
    cmd.name = "GetCFWSlotsNum";
    cmd.payload = std::any();

    // 直接通过设备句柄调用，无需指定驱动名称（类似INDI的调用方式）
    SdkResult res = SdkManager::instance().callByHandle(handle, cmd);
    if (!res.success || !res.payload.has_value()) {
        if (errMsg) *errMsg = res.message;
        return false;
    }

    try {
        slotsNum = std::any_cast<int>(res.payload);
        return true;
    } catch (const std::bad_any_cast&) {
        if (errMsg) *errMsg = "GetCFWSlotsNum bad_any_cast";
        return false;
    }
}

/**
 * @brief 获取滤镜轮当前位置（0-based）
 * @param handle SDK 设备句柄
 * @param pos0 输出参数：当前位置（0-based）
 * @param errMsg 输出参数：错误信息（可选）
 * @return 成功返回 true，失败返回 false
 */
static bool sdkGetCfwPosition0(SdkDeviceHandle handle, int& pos0, std::string* errMsg = nullptr)
{
    SdkCommand cmd;
    cmd.type = SdkCommandType::Custom;
    cmd.name = "GetCFWPosition";
    cmd.payload = std::any();

    // 直接通过设备句柄调用，无需指定驱动名称
    SdkResult res = SdkManager::instance().callByHandle(handle, cmd);
    if (!res.success || !res.payload.has_value()) {
        if (errMsg) *errMsg = res.message;
        return false;
    }

    try {
        pos0 = std::any_cast<int>(res.payload);
        return true;
    } catch (const std::bad_any_cast&) {
        if (errMsg) *errMsg = "GetCFWPosition bad_any_cast";
        return false;
    }
}

/**
 * @brief 设置滤镜轮位置并等待到位（0-based）
 * @param handle SDK 设备句柄
 * @param targetPos0 目标位置（0-based）
 * @param timeoutMs 超时时间（毫秒）
 * @param errMsg 输出参数：错误信息（可选）
 * @return 成功返回 true，失败返回 false
 * @note 先下发设置命令，再轮询确认位置，避免滤镜未到位就开拍
 */
static bool sdkSetCfwPosition0AndWait(SdkDeviceHandle handle, int targetPos0, int timeoutMs, std::string* errMsg = nullptr)
{
    // 1) 下发设置命令
    {
        SdkCommand cmd;
        cmd.type = SdkCommandType::Custom;
        cmd.name = "SetCFWPosition";
        cmd.payload = targetPos0;

        // 直接通过设备句柄调用，无需指定驱动名称
        SdkResult res = SdkManager::instance().callByHandle(handle, cmd);
        if (!res.success) {
            if (errMsg) *errMsg = res.message;
            return false;
        }
    }

    // 2) 轮询确认位置（每 200ms 检查一次）
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        int cur0 = -1;
        if (sdkGetCfwPosition0(handle, cur0, errMsg) && cur0 == targetPos0)
            return true;
        QThread::msleep(200);
    }

    if (errMsg) *errMsg = "SetCFWPosition timeout";
    return false;
}

/**
 * @brief INDI：设置滤镜轮位置并等待到位（1-based）
 * @param client INDI client
 * @param dp 设备指针（相机或独立滤镜轮）
 * @param targetPos1 目标位置（1-based）
 * @param timeoutMs 超时时间（毫秒）
 * @param errMsg 输出参数：错误信息（可选）
 * @return 成功返回 true，失败返回 false
 *
 * @note 仅当确认位置已到位才返回成功，避免“命令已下发但未到位”导致前端位置提前变化。
 */
static bool indiSetCfwPosition1AndWait(MyClient* client,
                                      INDI::BaseDevice* dp,
                                      int targetPos1,
                                      int timeoutMs,
                                      std::string* errMsg = nullptr)
{
    if (!client || !dp) {
        if (errMsg) *errMsg = "device_null";
        return false;
    }
    if (!dp->isConnected()) {
        if (errMsg) *errMsg = "device_disconnected";
        return false;
    }

    // 1) 下发设置命令
    const uint32_t ret = client->setCFWPosition(dp, targetPos1);
    if (ret != QHYCCD_SUCCESS) {
        if (errMsg) *errMsg = "indi_error";
        return false;
    }

    // 2) 轮询确认位置（每 200ms 检查一次）
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (!dp->isConnected()) {
            if (errMsg) *errMsg = "device_disconnected";
            return false;
        }
        int pos = -1, min = 0, max = 0;
        const uint32_t gr = client->getCFWPosition(dp, pos, min, max);
        if (gr == QHYCCD_SUCCESS && pos == targetPos1)
            return true;
        QThread::msleep(200);
    }

    if (errMsg) *errMsg = "timeout";
    return false;
}

// ============================================================================
// 全局变量定义
// ============================================================================

DriversList drivers_list;              // 驱动列表
std::vector<DevGroup> dev_groups;      // 设备分组列表
std::vector<Device> devices;           // 设备列表
DriversListNew drivers_list_new;       // 新版驱动列表
SystemDevice systemdevice;             // 系统设备
SystemDeviceList systemdevicelist;     // 系统设备列表
QUrl websocketUrl;                     // WebSocket 连接 URL

int LoopCaptureNum = 0;
// Loop capture 在 Burst 模式下需要知道每次 Burst 的帧数（由 takeExposureBurst 更新）
int LoopCaptureBurstFrames = 1;

// ============================================================================
// MainWindow 类静态成员变量定义
// ============================================================================

MainWindow *MainWindow::instance = nullptr;

/**
 * @brief 获取构建日期字符串（从编译时宏 __DATE__ / __TIME__ 解析）
 * @return 构建日期字符串（格式：YYYYMMDDHHMM）
 */
std::string MainWindow::getBuildDate()
{
    static const std::map<std::string, std::string> monthMap = {
        {"Jan", "01"}, {"Feb", "02"}, {"Mar", "03"}, {"Apr", "04"},
        {"May", "05"}, {"Jun", "06"}, {"Jul", "07"}, {"Aug", "08"},
        {"Sep", "09"}, {"Oct", "10"}, {"Nov", "11"}, {"Dec", "12"}
    };

    std::string date = __DATE__;
    std::string time = __TIME__;
    std::stringstream dateStream(date);
    std::stringstream timeStream(time);
    std::string month, day, year;
    std::string hour, minute;
    dateStream >> month >> day >> year;
    std::getline(timeStream, hour, ':');
    std::getline(timeStream, minute, ':');

    return year + monthMap.at(month) + (day.size() == 1 ? "0" + day : day) + hour + minute;
}

MainWindow::MainWindow(QObject *parent) : QObject(parent)
{
    // 初始化极轴校准对象为nullptr
    polarAlignment = nullptr;

    // 初始化相机参数
    glFocalLength = 0;
    glCameraSize_width = 0.0;
    glCameraSize_height = 0.0;

    system_timer = new QTimer(this); // 用于对系统的监测
    connect(system_timer, &QTimer::timeout, this, &MainWindow::updateCPUInfo);
    system_timer->start(3000);

    Logger::Initialize();
    getHostAddress();
    initializeStorageAndWebPaths();
    initializeWebSocketBridge();

    // 安装自定义的消息处理器
    // qInstallMessageHandler(customMessageHandler);

    initINDIServer();
    initINDIClient();

    initGPIO();

    readDriversListFromFiles("/usr/share/indi/drivers.xml", drivers_list, dev_groups, devices);

    Tools::InitSystemDeviceList();
    Tools::initSystemDeviceList(systemdevicelist);
    Tools::makeConfigFile();
    // 取消：不再读取/保存中天翻转持久化状态（统一使用几何法 needsFlip）
    Tools::makeImageFolder();
    connect(Tools::getInstance(), &Tools::parseInfoEmitted, this, &MainWindow::onParseInfoEmitted);

    // 🆕 初始化 SDK 驱动注册表（在所有驱动自动注册后）
    // 必须在静态初始化完成后调用，用于构建 INDI 驱动名到 SDK 驱动名的映射
    SdkDriverRegistry::instance().initialize();
    Logger::Log("MainWindow | SDK driver registry initialized", LogLevel::INFO, DeviceType::MAIN);

    m_thread = new QThread;
    m_threadTimer = new QTimer;
    m_threadTimer->setInterval(200);
    m_threadTimer->moveToThread(m_thread);
    connect(m_threadTimer, &QTimer::timeout, this, &MainWindow::onTimeout);
    connect(m_thread, &QThread::finished, m_threadTimer, &QTimer::stop);
    connect(m_thread, &QThread::destroyed, m_threadTimer, &QTimer::deleteLater);
    connect(m_thread, &QThread::started, m_threadTimer, QOverload<>::of(&QTimer::start));
    m_thread->start();

    initializeBuiltInGuiderRuntime();

    initializeRuntimeWorkersAndTimers();

}

/**
 * @brief 判断主相机是否为 SDK 模式
 * @return SDK 模式返回 true，INDI 模式返回 false
 */
bool MainWindow::isMainCameraSDK()
{
    return (systemdevicelist.system_devices.size() > 20 &&
            systemdevicelist.system_devices[20].isSDKConnect &&
            sdkMainCameraHandle != nullptr);
}

/**
 * @brief 判断主相机是否已连接（支持 SDK 和 INDI 两种模式）
 * @return 已连接返回 true，否则返回 false
 */
bool MainWindow::isMainCameraConnected()
{
    // SDK 模式：检查 SDK 句柄是否有效
    if (systemdevicelist.system_devices.size() > 20 &&
        systemdevicelist.system_devices[20].isSDKConnect)
    {
        return sdkMainCameraHandle != nullptr;
    }

    // INDI 模式：检查 INDI 设备指针是否有效
    return dpMainCamera != nullptr;
}

bool MainWindow::isGuiderCameraSDK() const
{
    return (systemdevicelist.system_devices.size() > 1 &&
            systemdevicelist.system_devices[1].isSDKConnect &&
            sdkGuiderHandle != nullptr);
}

bool MainWindow::isGuiderCameraConnected() const
{
    if (systemdevicelist.system_devices.size() > 1 &&
        systemdevicelist.system_devices[1].isSDKConnect)
    {
        return sdkGuiderHandle != nullptr;
    }
    return dpGuider != nullptr;
}

bool MainWindow::isPoleCameraSDK() const
{
    return (systemdevicelist.system_devices.size() > 2 &&
            systemdevicelist.system_devices[2].isSDKConnect &&
            sdkPoleScopeHandle != nullptr);
}

bool MainWindow::isPoleCameraConnected() const
{
    if (systemdevicelist.system_devices.size() > 2 &&
        systemdevicelist.system_devices[2].isSDKConnect)
    {
        return sdkPoleScopeHandle != nullptr;
    }
    return dpPoleScope != nullptr;
}

SdkSerialExecutor* MainWindow::sdkMainCameraExecutor() const
{
    return sdkMainCamExec.get();
}

SdkSerialExecutor* MainWindow::sdkGuiderCameraExecutor() const
{
    return sdkGuiderCamExec.get();
}

SdkSerialExecutor* MainWindow::sdkPoleCameraExecutor() const
{
    return sdkPoleCamExec.get();
}

SdkSerialExecutor* MainWindow::sdkExecutorForPolarRole(PolarAlignmentCameraRole role) const
{
    switch (role)
    {
    case PolarAlignmentCameraRole::MainCamera:
        return sdkMainCameraExecutor();
    case PolarAlignmentCameraRole::Guider:
        return sdkGuiderCameraExecutor();
    case PolarAlignmentCameraRole::PoleCamera:
        return sdkPoleCameraExecutor();
    }
    return nullptr;
}

int MainWindow::getMainCameraFocalLengthFromConfigAndMigrateIfNeeded()
{
    if (glFocalLength > 0)
        return glFocalLength;

    std::unordered_map<std::string, std::string> config;
    Tools::readClientSettings("config/config.ini", config);

    QString mainFocal;
    auto itMain = config.find("MainCameraFocalLength");
    if (itMain != config.end())
    {
        mainFocal = QString::fromStdString(itMain->second).trimmed();
    }
    else
    {
        auto itLegacy = config.find("FocalLength");
        if (itLegacy != config.end())
        {
            mainFocal = QString::fromStdString(itLegacy->second).trimmed();
            if (!mainFocal.isEmpty())
            {
                setClientSettings("MainCameraFocalLength", mainFocal);
                Logger::Log("Migrate focal length config: FocalLength -> MainCameraFocalLength = " +
                                mainFocal.toStdString(),
                            LogLevel::INFO, DeviceType::MAIN);
            }
        }
    }

    bool ok = false;
    const int focal = mainFocal.toInt(&ok);
    if (ok && focal > 0)
    {
        glFocalLength = focal;
        return focal;
    }
    return 0;
}


/**
 * @brief 判断电调是否为 SDK 模式
 * @return SDK 模式返回 true，INDI 模式返回 false
 */
bool MainWindow::isFocuserSDK()
{
    return (systemdevicelist.system_devices.size() > 22 &&
            systemdevicelist.system_devices[22].isSDKConnect &&
            systemdevicelist.system_devices[22].isBind &&
            sdkFocuserHandle != nullptr);
}

/**
 * @brief 判断赤道仪是否为 SDK 模式
 * @return SDK 模式返回 true，INDI 模式返回 false
 * @note TODO: 需要实现赤道仪SDK模式的支持，目前仅预留接口
 *       需要确认赤道仪在 systemdevicelist.system_devices 中的索引位置
 */
bool MainWindow::isMountSDK()
{
    // TODO: 实现赤道仪SDK模式的检查
    // 需要确认赤道仪在 systemdevicelist.system_devices 中的索引（可能是19）
    // 目前假设索引为19，需要根据实际情况调整
    // return (systemdevicelist.system_devices.size() > 19 &&
    //         systemdevicelist.system_devices[19].isSDKConnect &&
    //         sdkMountHandle != nullptr);

    // 暂时返回false，等待实现
    return false;
}

void MainWindow::getHostAddress()
{
    // 默认使用本机回环地址连接本机的 WebSocket Hub（NodeJs-Transponder）。
    // 这样即使局域网 IP 变化，也不会影响 Qt <-> Hub 的连接（避免“换网段就断连”）。
    //
    // 如需指定 Hub 所在主机，可设置环境变量：
    // - QUARCS_WS_HOST=<ip/hostname>  例如：192.168.200.129
    // - QUARCS_WS_HOST=auto           继续使用旧的“自动探测网卡IP”逻辑
    const QByteArray envHost = qgetenv("QUARCS_WS_HOST");
    if (envHost.isEmpty()) {
        const QString host = QStringLiteral("127.0.0.1");
        websockethttpUrl  = QUrl(QStringLiteral("ws://%1:8600").arg(host));
        websockethttpsUrl = QUrl(QStringLiteral("wss://%1:8601").arg(host));
        Logger::Log("WebSocket target (default loopback): " + websockethttpUrl.toString().toStdString(),
                    LogLevel::INFO, DeviceType::MAIN);
        return;
    }

    if (envHost != "auto") {
        const QString host = QString::fromUtf8(envHost);
        websockethttpUrl  = QUrl(QStringLiteral("ws://%1:8600").arg(host));
        websockethttpsUrl = QUrl(QStringLiteral("wss://%1:8601").arg(host));
        Logger::Log("WebSocket target (from QUARCS_WS_HOST): " + websockethttpUrl.toString().toStdString(),
                    LogLevel::INFO, DeviceType::MAIN);
        return;
    }

    int retryCount = 0;
    const int maxRetries = 20;
    const int waitTime = 5000; // 5秒

    while (retryCount < maxRetries)
    {
        QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
        bool found = false;

        foreach (const QNetworkInterface &interface, interfaces)
        {
            // 排除回环接口和非活动接口
            if (interface.flags() & QNetworkInterface::IsLoopBack || !(interface.flags() & QNetworkInterface::IsUp))
                continue;

            QList<QNetworkAddressEntry> addresses = interface.addressEntries();
            foreach (const QNetworkAddressEntry &address, addresses)
            {
                if (address.ip().protocol() == QAbstractSocket::IPv4Protocol)
                {
                    QString localIpAddress = address.ip().toString();
                    Logger::Log("Local IP Address:" + localIpAddress.toStdString(), LogLevel::INFO, DeviceType::MAIN);

                    if (!localIpAddress.isEmpty())
                    {
                        QUrl getUrl(QStringLiteral("ws://%1:8600").arg(localIpAddress));
                        QUrl getUrlHttps(QStringLiteral("wss://%1:8601").arg(localIpAddress));
                        // Logger::Log("WebSocket URL:" + getUrl.toString().toStdString(), LogLevel::INFO, DeviceType::MAIN);
                        websockethttpUrl = getUrl;
                        websockethttpsUrl = getUrlHttps;
                        found = true;
                        break;
                    }
                    else
                    {
                        Logger::Log("Failed to get local IP address.", LogLevel::WARNING, DeviceType::MAIN);
                    }
                }
            }
            if (found)
                break;
        }

        if (found)
            break;

        retryCount++;
        QThread::sleep(waitTime / 1000); // 等待5秒
    }

    if (retryCount == maxRetries)
    {
        qCritical() << "Failed to detect any network interfaces after" << maxRetries << "attempts.";
    }
}

void MainWindow::onMessageReceived(const QString &message)
{
    Logger::Log("Received message in MainWindow:" + message.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);

    QStringList parts = message.split(':');
    QString trimmedMessage = message.trimmed();
    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();

    if (!lastCommandMessage.isEmpty() && lastCommandMessage == trimmedMessage && lastCommandTime > 0)
    {
        qint64 timeDiff = currentTime - lastCommandTime;

        if (timeDiff < COMMAND_DEBOUNCE_MS)
        {
            Logger::Log("Command debounce: Skipping duplicate message '" + trimmedMessage.toStdString() +
                           "' received within " + std::to_string(timeDiff) + "ms (threshold: " +
                           std::to_string(COMMAND_DEBOUNCE_MS) + "ms)",
                       LogLevel::DEBUG, DeviceType::MAIN);
            return;
        }
    }

    lastCommandMessage = trimmedMessage;
    lastCommandTime = currentTime;

    if (handleDriverSelectionCommand(message, parts) ||
        handleBindingCommand(message, parts) ||
        handleCaptureCommand(message, parts) ||
        handleFocuserCommand(message, parts) ||
        handleGuiderCommand(message, parts) ||
        handleMountCommand(message, parts) ||
        handleScheduleCommand(message, parts) ||
        handleFileAndStorageCommand(message, parts) ||
        handleSystemCommand(message, parts))
    {
        return;
    }

    Logger::Log("Unknown message: " + message.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
}

DeviceType MainWindow::getDeviceTypeFromPartialString(const std::string &typeStr)
{
    // 使用find()方法检查字符串中是否包含特定的子字符串
    if (typeStr.find("Exposure done, downloading image") != std::string::npos || typeStr.find("Download complete") != std::string::npos || typeStr.find("Uploading file.") != std::string::npos || typeStr.find("Image saved to") != std::string::npos)
    {
        // Logger::Log("获取的信息类型是相机", LogLevel::INFO, DeviceType::CAMERA);
        return DeviceType::CAMERA;
    }
    else if (0)
    {
        // Logger::Log("DeviceType::MOUNT", LogLevel::INFO, DeviceType::MOUNT);
        return DeviceType::MOUNT;
    }
    else
    {
        // Logger::Log("DeviceType::MAIN", LogLevel::INFO, DeviceType::MAIN);
        return DeviceType::MAIN;
    }
}

namespace
{
std::string formatBayerPhaseDebug(const QString& baseCfa, int cfaOffsetX, int cfaOffsetY,
                                  int frameStartX, int frameStartY, const QString& resolvedCfa)
{
    const int totalShiftX = cfaOffsetX + frameStartX;
    const int totalShiftY = cfaOffsetY + frameStartY;
    return "baseCFA=" + baseCfa.toStdString() +
           ", cfaOffset=(" + std::to_string(cfaOffsetX) + "," + std::to_string(cfaOffsetY) + ")" +
           ", frameStart=(" + std::to_string(frameStartX) + "," + std::to_string(frameStartY) + ")" +
           ", totalShift=(" + std::to_string(totalShiftX) + "," + std::to_string(totalShiftY) + ")" +
           ", parity=(" + std::to_string(totalShiftX & 1) + "," + std::to_string(totalShiftY & 1) + ")" +
           ", resolvedCFA=" + resolvedCfa.toStdString();
}

std::string sampleBayer2x2Debug(const cv::Mat& image16)
{
    if (image16.empty() || image16.type() != CV_16UC1) {
        return "2x2=unavailable";
    }

    const int maxY = std::min(2, image16.rows);
    const int maxX = std::min(2, image16.cols);
    std::ostringstream oss;
    oss << "2x2=[";
    for (int y = 0; y < maxY; ++y) {
        if (y > 0) oss << ";";
        for (int x = 0; x < maxX; ++x) {
            if (x > 0) oss << ",";
            oss << image16.at<uint16_t>(y, x);
        }
    }
    oss << "]";
    return oss.str();
}
} // namespace

void MainWindow::onTimeout()
{
    // TODO(PHD2): PHD2 数据轮询已停用（导星改为 INDI 直出图），如需恢复再启用 ShowPHDdata()
    // ShowPHDdata();

    // 显示赤道仪指向
    mountDisplayCounter++;
    if (dpMount != NULL)
    {
        if (dpMount->isConnected())
        {
            if (mountDisplayCounter >= 5)
            {

                double RA_HOURS, DEC_DEGREE;
                indi_Client->getTelescopeRADECJNOW(dpMount, RA_HOURS, DEC_DEGREE);
                double CurrentRA_Degree = Tools::HourToDegree(RA_HOURS);
                double CurrentDEC_Degree = DEC_DEGREE;

                emit wsThread->sendMessageToClient("TelescopeRADEC:"
                    + QString::number(CurrentRA_Degree)
                    + ":" + QString::number(CurrentDEC_Degree));

                // Logger::Log("当前指向:RA:" + std::to_string(RA_HOURS) + " 小时,DEC:" + std::to_string(CurrentDEC_Degree) + " 度", LogLevel::INFO, DeviceType::MAIN);

                // 直接每次执行原"慢速"查询内容
                bool isParked = false;
                indi_Client->getTelescopePark(dpMount, isParked);
                emit wsThread->sendMessageToClient(
                    isParked ? "TelescopePark:ON" : "TelescopePark:OFF");

                QString NewTelescopePierSide;
                indi_Client->getTelescopePierSide(dpMount, NewTelescopePierSide);
                if (NewTelescopePierSide != TelescopePierSide)
                {
                    // 出现方向侧变化,此时意味着进行了中天翻转,判断是否完成翻转
                    if (indi_Client->mountState.isMovingNow() == false) {
                        emit wsThread->sendMessageToClient("FlipStatus:success");
                        TelescopePierSide = NewTelescopePierSide;

                        // ===== 内置导星：翻转后强制重新校准（PHD2 常规做法）=====
                        // 翻转会改变力学状态，且可能导致相机/视场变化；继续使用旧校准容易越导越偏。
                        if (guiderCore)
                        {
                            const guiding::State gs = guiderCoreStateCache;
                            if (gs == guiding::State::Guiding || gs == guiding::State::Calibrating)
                            {
                                Logger::Log("BuiltInGuider | pier side changed, force recalibrate after meridian flip.",
                                            LogLevel::INFO, DeviceType::GUIDER);
                                emit wsThread->sendMessageToClient("GuiderCoreInfo:MeridianFlipDetected:Recalibrating");
                                postGuiderCore(guiderCore, [](GuiderCore *core) {
                                    core->clearCachedCalibration();
                                    core->stopGuiding();
                                    core->startGuidingForceCalibrate();
                                });
                            }
                        }
                    }
                }
                emit wsThread->sendMessageToClient("TelescopePierSide:" + NewTelescopePierSide);
                // Logger::Log("TelescopePierSide:" + NewTelescopePierSide.toStdString(), LogLevel::INFO, DeviceType::MAIN);

                indi_Client->getTelescopeMoving(dpMount);

                bool isTrack = false;
                indi_Client->getTelescopeTrackEnable(dpMount, isTrack);

                if (polarAlignment != nullptr && polarAlignment->isRunning() && isTrack) {
                    indi_Client->setTelescopeTrackEnable(dpMount, false);
                    sleep(1);
                    indi_Client->getTelescopeTrackEnable(dpMount, isTrack);
                }

                emit wsThread->sendMessageToClient(isTrack ? "TelescopeTrack:ON"
                                                           : "TelescopeTrack:OFF");

                if (!FirstRecordTelescopePierSide)
                {
                    if (FirstTelescopePierSide != TelescopePierSide)
                        isMeridianFlipped = true;
                    else
                        isMeridianFlipped = false;
                }

                emit wsThread->sendMessageToClient("TelescopeStatus:" + TelescopeControl_Status());

                mountDisplayCounter = 0;

                // const MeridianStatus ms = checkMeridianStatus(); // 这个计算当前距离中天的时间
                // switch (ms.event) {
                //   case FlipEvent::Started: emit wsThread->sendMessageToClient("MeridianFlip:STARTED"); break;
                //   case FlipEvent::Done:    emit wsThread->sendMessageToClient("MeridianFlip:DONE");    break;
                //   case FlipEvent::Failed:  emit wsThread->sendMessageToClient("MeridianFlip:FAILED");  break;
                //   default: break;
                // }

                // if (!std::isnan(ms.etaMinutes)) {
                //     // 显示规则：与翻转需求绑定 —— 需要翻转显示负号，不需要显示正号
                //     const bool showNeg = ms.needsFlip;
                //     const double absMinutes = std::fabs(ms.etaMinutes);
                //     const int totalSeconds = static_cast<int>(std::llround(absMinutes * 60.0));
                //     const int hours = totalSeconds / 3600;
                //     const int mins  = (totalSeconds % 3600) / 60;
                //     const int secs  = totalSeconds % 60;

                //     const QString hms = QString("%1%2:%3:%4")
                //                             .arg(showNeg ? "-" : "")
                //                             .arg(hours, 2, 10, QLatin1Char('0'))
                //                             .arg(mins,  2, 10, QLatin1Char('0'))
                //                             .arg(secs,  2, 10, QLatin1Char('0'));
                //     emit wsThread->sendMessageToClient("MeridianETA_hms:" + hms);
                //     Logger::Log("MeridianETA_hms:" + hms.toStdString() + " side:" + TelescopePierSide.toStdString() + " needflip:" + (ms.needsFlip ? "true" : "false"), LogLevel::INFO, DeviceType::MAIN);
                // }

                //TODO:当前判断方式存在问题,需要重新修改判断
                // 加入判断,当此时需要执行自动中天翻转,且设备设置为自动中天翻转,则执行自动中天翻转
                // if (ms.needsFlip && isAutoFlip && indi_Client->mountState.isFlipping == false && indi_Client->mountState.isFlipBacking == false) {
                //     // 预备翻转
                //     if (flipPrepareTime >= 0) {
                //         flipPrepareTime-=2;
                //         emit wsThread->sendMessageToClient("FlipStatus:FlipPrepareTime," + QString::number(flipPrepareTime));
                //     }
                //     else {
                //         emit wsThread->sendMessageToClient("FlipStatus:start");
                //         indi_Client->startFlip(dpMount);
                //     }
                // }else{
                //     flipPrepareTime = flipPrepareTimeDefault;
                // }
                // if (indi_Client->mountState.isFlipping == true || indi_Client->mountState.isFlipBacking == true) {
                //     emit wsThread->sendMessageToClient("FlipStatus:start");
                // }

            }
            // Logger::Log("11111", LogLevel::INFO, DeviceType::MAIN);
        }
    }


    MainCameraStatusCounter++;

    // 判断是 SDK 模式还是 INDI 模式
    bool isMainCameraSDK = (systemdevicelist.system_devices.size() > 20 &&
                            systemdevicelist.system_devices[20].isSDKConnect &&
                            sdkMainCameraHandle != nullptr);

    if (isMainCameraSDK || dpMainCamera != NULL)
    {
        if (MainCameraStatusCounter >= 5)
        {
            emit wsThread->sendMessageToClient("MainCameraStatus:" + glMainCameraStatu);
            MainCameraStatusCounter = 0;
            double CameraTemp = 0.0;
            uint32_t ret = QHYCCD_ERROR;

            if (isMainCameraSDK)
            {
                // SDK 模式：在 SDK 线程异步获取温度，避免阻塞主线程（与 GetSingleFrame 可能竞争设备锁）
                SdkSerialExecutor *mainExec = sdkMainCameraExecutor();
                if (mainExec && mainExec->isRunning() && sdkMainCameraHandle != nullptr)
                {
                    const SdkDeviceHandle handleSnap = sdkMainCameraHandle;
                    mainExec->post([this, handleSnap]() {
                        SdkCommand getTempCmd;
                        getTempCmd.type = SdkCommandType::Custom;
                        getTempCmd.name = "GetCurrentTemperature";
                        getTempCmd.payload = std::any();
                        // 直接通过设备句柄调用，无需指定驱动名称
                        SdkResult tempRes = SdkManager::instance().callByHandle(handleSnap, getTempCmd);

                        // 回到主线程更新UI
                        if (tempRes.success && tempRes.payload.has_value()) {
                            double temp = std::any_cast<double>(tempRes.payload);
                            QMetaObject::invokeMethod(
                                this,
                                [this, temp]() {
                                    if (wsThread) {
                                        emit wsThread->sendMessageToClient("MainCameraTemperature:" + QString::number(temp));
                                    }
                                },
                                Qt::QueuedConnection);
                        }
                    });
                    // 由于是异步调用，跳过本次同步温度发送（下次异步调用会更新）
                    return;
                }
            }
            else if (dpMainCamera != NULL)
            {
                // INDI 模式：使用 indi_Client 获取温度
                ret = indi_Client->getTemperature(dpMainCamera, CameraTemp);
            }

            if (ret == QHYCCD_SUCCESS)
            {
                emit wsThread->sendMessageToClient("MainCameraTemperature:" + QString::number(CameraTemp));
            }


        }
    }

}

MeridianStatus MainWindow::checkMeridianStatus()
{
    MeridianStatus out;

    if (!dpMount || !dpMount->isConnected())
        return out;

    // 读 PierSide（设备上报的方向侧）
    QString pier = "UNKNOWN";
    indi_Client->getTelescopePierSide(dpMount, pier); // "EAST"/"WEST"/"UNKNOWN"

    // -------- 过中天 ETA（分钟）--------
    // 1) 当前赤经（小时）
    double raH = 0.0, decDeg = 0.0;
    indi_Client->getTelescopeRADECJNOW(dpMount, raH, decDeg);

    // 2) LST 小时（优先 TIME_LST；否则 UTC+经度估算）
    auto norm24 = [](double h){ h=fmod(h,24.0); if(h<0) h+=24.0; return h; };
    // 将可能的度/秒等单位推断并统一为小时，再规范到 [0,24)
    auto toHours = [&](double v)->double {
        if (std::isnan(v)) return v;
        double x = v;
        // 若为秒（0..86400），转换为小时
        if (std::fabs(x) > 24.0 && std::fabs(x) <= 86400.0) x /= 3600.0;
        // 若为度（0..360），转换为小时
        if (std::fabs(x) > 24.0 && std::fabs(x) <= 360.0)  x /= 15.0;
        // 若超过一圈（>360 度等），先按度归一后转小时
        if (std::fabs(x) > 360.0) x = fmod(x, 360.0) / 15.0;
        return norm24(x);
    };
    double lstH = std::numeric_limits<double>::quiet_NaN();

    // 2.1 用 INDI::PropertyNumber 读取 TIME_LST（避免 p->np 报错）
    if (true) {
        INDI::PropertyNumber lst = dpMount->getNumber("TIME_LST");
        if (lst.isValid() && lst.size() > 0) {
            lstH = toHours(lst[0].getValue());   // 统一为小时
        }
    }

    // 2.2 若没有 TIME_LST，则从 GEOGRAPHIC_COORD 取经度，用 UTC 算 LST
    if (std::isnan(lstH)) {
        double lonDeg = std::numeric_limits<double>::quiet_NaN();
        INDI::PropertyNumber geo = dpMount->getNumber("GEOGRAPHIC_COORD");
        if (geo.isValid()) {
            // 通常顺序 LAT(0), LONG(1), ELEV(2)；若你的驱动是命名项，也可用 geo["LONG"].getValue()
            if (geo.size() >= 2)
                lonDeg = geo[1].getValue();
        }
        if (!std::isnan(lonDeg)) {
            const QDateTime utc = QDateTime::currentDateTimeUtc();
            const int Y=utc.date().year(), M=utc.date().month(), D=utc.date().day();
            const int H=utc.time().hour(), Min=utc.time().minute(), S=utc.time().second(), ms=utc.time().msec();

            auto jdUTC = [&](int Y,int M,int D,int H,int Min,int S,int ms)->double{
                int a=(14-M)/12, y=Y+4800-a, m=M+12*a-3;
                long JDN=D+(153*m+2)/5+365*y+y/4-y/100+y/400-32045;
                double dayfrac=(H-12)/24.0 + Min/1440.0 + (S + ms/1000.0)/86400.0;
                return JDN + dayfrac;
            };
            const double JD = jdUTC(Y,M,D,H,Min,S,ms);
            const double Dd = JD - 2451545.0;
            double GMST = 18.697374558 + 24.06570982441908 * Dd; // 小时
            lstH = norm24(GMST + lonDeg/15.0);
        }
    }

    // 清洗 RA 单位并规范到小时
    raH = toHours(raH);

    if (!std::isnan(lstH)) {
        // 采用半开区间 [-12, 12) 规范时角，避免边界抖动
        auto wrap12 = [](double h){ while (h < -12.0) h += 24.0; while (h >= 12.0) h -= 24.0; return h; };
        const double haPrincipal = wrap12(lstH - raH); // 小时；<0 东侧，>0 西侧
        // 连续时角（避免在下中天处从 -12h 跳到 +12h 导致符号翻转）
        static bool hasContHA = false;
        static double contHA = 0.0;
        static double lastHAPrincipal = 0.0;
        if (!hasContHA) {
            contHA = haPrincipal;
            lastHAPrincipal = haPrincipal;
            hasContHA = true;
        } else {
            double delta = haPrincipal - lastHAPrincipal;
            if (delta > 12.0) delta -= 24.0;
            else if (delta < -12.0) delta += 24.0;
            contHA += delta;
            lastHAPrincipal = haPrincipal;
        }
        const bool isPastUpper = (haPrincipal > 0.0);

        // HOME 位也参与翻转判断（不特殊抑制）

        // 基于 |HA|=6h 分割（注意：这里的 6h 是时角 HA，不是 RA）：
        // - 上中天半周区间 |HA| < 6h：  HA ≥ 0 → EAST，HA < 0 → WEST
        // - 下中天半周区间 |HA| ≥ 6h：  映射取反（对称关系）
        constexpr double kHalfCycleHAHours = 6.0;
        constexpr double kBoundaryTolH = 0.02; // ≈1.2 分钟容差
        const bool isLowerRegion = (std::fabs(haPrincipal) >= (kHalfCycleHAHours - kBoundaryTolH));
        bool eastMapping = (haPrincipal >= 0.0);
        if (isLowerRegion) eastMapping = !eastMapping;
        QString theoreticalPier = eastMapping ? "EAST" : "WEST";
        if (pier == "UNKNOWN") {
            out.needsFlip = false; // 无法判断或靠近极区：不触发翻转
        } else {
            out.needsFlip = (pier != theoreticalPier);
        }

        // ETA：严格按连续时角符号（未过为正，已过为负），避免下中天跳变
        out.etaMinutes = (-contHA) * 60.0;

        // 注意：needsFlip 已在上面根据 nearPole 与理论 Pier 计算完毕
    }

    return out;
}


// void MainWindow::saveFitsAsJPG(QString filename)
// {
//     Logger::Log("Starting to save FITS as JPG...", LogLevel::INFO, DeviceType::GUIDER);
//     cv::Mat image;
//     cv::Mat image16;
//     cv::Mat SendImage;
//     Tools::readFits(filename.toLocal8Bit().constData(), image);
//     Logger::Log("FITS file read successfully.", LogLevel::INFO, DeviceType::GUIDER);

//     QList<FITSImage::Star> stars = Tools::FindStarsByStellarSolver(true, true);
//     Logger::Log("Star detection completed.", LogLevel::INFO, DeviceType::GUIDER);

//     if(stars.size() != 0){
//         FWHM = stars[0].HFR;
//         Logger::Log("FWHM calculated from detected stars.", LogLevel::INFO, DeviceType::GUIDER);
//     }
//     else {
//         FWHM = -1;
//         Logger::Log("No stars detected, FWHM set to -1.", LogLevel::WARNING, DeviceType::GUIDER);
//     }

//     if(image16.depth()==8) {
//         image.convertTo(image16, CV_16UC1, 256, 0); //x256  MSB alignment
//         Logger::Log("Image converted to 16-bit format with MSB alignment.", LogLevel::INFO, DeviceType::GUIDER);
//     }
//     else {
//         image.convertTo(image16, CV_16UC1, 1, 0);
//         Logger::Log("Image converted to 16-bit format.", LogLevel::INFO, DeviceType::GUIDER);
//     }

//     if(FWHM != -1){
//         // 在原图上绘制检测结果
//         cv::Point center(stars[0].x, stars[0].y);
//         cv::circle(image16, center, static_cast<int>(FWHM), cv::Scalar(0, 0, 255), 1); // Draw HFR circle
//         cv::circle(image16, center, 1, cv::Scalar(0, 255, 0), -1);                     // Draw center point
//         std::string hfrText = cv::format("%.2f", stars[0].HFR);
//         cv::putText(image16, hfrText, cv::Point(stars[0].x - FWHM, stars[0].y - FWHM - 5), cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(0, 0, 255), 1);
//         Logger::Log("Annotations for stars added to image.", LogLevel::INFO, DeviceType::GUIDER);
//     }

//     cv::Mat NewImage = image16;
//     FWHMCalOver = true;

//     cv::normalize(NewImage, SendImage, 0, 255, cv::NORM_MINMAX, CV_8U);    // Normalize new image
//     Logger::Log("Image normalized to 8-bit format.", LogLevel::INFO, DeviceType::GUIDER);

//     QString uniqueId = QUuid::createUuid().toString();
//     Logger::Log("Unique ID generated for new image.", LogLevel::INFO, DeviceType::GUIDER);

//     // 列出所有以"CaptureImage"为前缀的文件
//     QDir directory(QString::fromStdString(vueDirectoryPath));
//     QStringList filters;
//     filters << "CaptureImage*.jpg"; // 使用通配符来筛选以"CaptureImage"为前缀的jpg文件
//     QStringList fileList = directory.entryList(filters, QDir::Files);
//     Logger::Log("Existing image files listed for deletion.", LogLevel::INFO, DeviceType::GUIDER);

//     // 删除所有匹配的文件
//     for (const auto &file : fileList)
//     {
//         QString filePath = QString::fromStdString(vueDirectoryPath) + file;
//         QFile::remove(filePath);
//     }
//     Logger::Log("Old image files deleted.", LogLevel::INFO, DeviceType::GUIDER);

//     // 删除前一张图像文件
//     if (PriorROIImage != "NULL") {
//         QFile::remove(QString::fromStdString(PriorROIImage));
//         Logger::Log("Previous ROI image deleted.", LogLevel::INFO, DeviceType::GUIDER);
//     }

//     // 保存新的图像带有唯一ID的文件名
//     std::string fileName = "CaptureImage_" + uniqueId.toStdString() + ".jpg";
//     std::string filePath = vueDirectoryPath + fileName;
//     bool saved = cv::imwrite(filePath, SendImage);
//     Logger::Log("Attempt to save new image file.", LogLevel::INFO, DeviceType::GUIDER);

//     std::string Command = "sudo ln -sf " + filePath + " " + vueImagePath + fileName;
//     system(Command.c_str());
//     Logger::Log("Symbolic link created for new image file.", LogLevel::INFO, DeviceType::GUIDER);

//     PriorROIImage = vueImagePath + fileName;

//     if (saved)
//     {
//         emit wsThread->sendMessageToClient("SaveJpgSuccess:" + QString::fromStdString(fileName));

//         if(FWHM != -1){
//             dataPoints.append(QPointF(CurrentPosition, FWHM));

//             Logger::Log("dataPoints:" + std::to_string(CurrentPosition) + "," + std::to_string(FWHM), LogLevel::INFO, DeviceType::GUIDER);

//             float a, b, c;
//             Tools::fitQuadraticCurve(dataPoints, a, b, c);

//             if (dataPoints.size() >= 5) {
//                 QVector<QPointF> LineData;

//                 for (float x = CurrentPosition - 3000; x <= CurrentPosition + 3000; x += 10)
//                 {
//                     float y = a * x * x + b * x + c;
//                     LineData.append(QPointF(x, y));
//                 }

//                 // 计算导数为零的 x 坐标
//                 float x_min = -b / (2 * a);
//                 minPoint_X = x_min;
//                 // 计算最小值点的 y 坐标
//                 float y_min = a * x_min * x_min + b * x_min + c;

//                 QString dataString;
//                 for (const auto &point : LineData)
//                 {
//                     dataString += QString::number(point.x()) + "|" + QString::number(point.y()) + ":";
//                 }

//                 R2 = Tools::calculateRSquared(dataPoints, a, b, c);
//                 // qInfo() << "RSquared: " << R2;

//                 emit wsThread->sendMessageToClient("fitQuadraticCurve:" + dataString);
//                 emit wsThread->sendMessageToClient("fitQuadraticCurve_minPoint:" + QString::number(x_min) + ":" + QString::number(y_min));
//             }
//         }
//     }
//     else
//     {
//         Logger::Log("Failed to save image.", LogLevel::ERROR, DeviceType::GUIDER);
//     }
// }

//"Telescopes"|"Focusers"|"CCDs"|"Spectrographs"|"Filter Wheels"|"Auxiliary"|"Domes"|"Weather"|"Agent"
