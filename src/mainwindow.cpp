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

    cv::line(image16, cv::Point(x0, cy), cv::Point(x1, cy), black, 3, cv::LINE_8);
    cv::line(image16, cv::Point(cx, y0), cv::Point(cx, y1), black, 3, cv::LINE_8);
    cv::line(image16, cv::Point(x0, cy), cv::Point(x1, cy), white, 1, cv::LINE_8);
    cv::line(image16, cv::Point(cx, y0), cv::Point(cx, y1), white, 1, cv::LINE_8);
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

    // 导星相机循环曝光（INDI 直出图）：使用 singleShot，收到一帧后再调度下一帧，避免重入
    guiderLoopTimer = new QTimer(this);
    guiderLoopTimer->setSingleShot(true);
    connect(guiderLoopTimer, &QTimer::timeout, this, &MainWindow::onGuiderLoopTimeout);
    guiderCoreThread = new QThread(this);
    guiderCoreThread->setObjectName(QStringLiteral("GuiderCoreThread"));
    qRegisterMetaType<cv::Mat>("cv::Mat");
    qRegisterMetaType<QVector<QPointF>>("QVector<QPointF>");
    qRegisterMetaType<QVector<QString>>("QVector<QString>");
    qRegisterMetaType<QPointF>("QPointF");
    guiderCore = new GuiderCore();
    guiderParamsCache = guiderCore->params();
    {
        std::unordered_map<std::string, std::string> config;
        Tools::readClientSettings("config/config.ini", config);
        const auto it = config.find("GuiderExposureMs");
        if (it != config.end())
        {
            bool ok = false;
            const int savedGuiderExpMs = QString::fromStdString(it->second).trimmed().toInt(&ok);
            if (ok && savedGuiderExpMs > 0)
            {
                guiderExpMs = savedGuiderExpMs;
                guiderParamsCache.exposureMs = savedGuiderExpMs;
                guiderCore->setParams(guiderParamsCache);
                Logger::Log("BuiltInGuider | restored GuiderExposureMs=" +
                                std::to_string(savedGuiderExpMs),
                            LogLevel::INFO, DeviceType::GUIDER);
            }
        }
    }
    guiderCoreStateCache = guiderCore->state();
    guiderCore->moveToThread(guiderCoreThread);
    connect(guiderCoreThread, &QThread::finished, guiderCore, &QObject::deleteLater);
    guiderCoreThread->start();
    Logger::Log("BuiltInGuider | GuiderCore moved to GuiderCoreThread",
                LogLevel::INFO, DeviceType::GUIDER);
    syncGuiderScaleParams(true, false);
    publishGuiderSearchBoxMode(false);
#if QUARCS_SIM_GUIDER
    simGuiderFrameSource = std::make_unique<guiding::SimGuiderFrameSource>();
    Logger::Log("BuiltInGuider | simulated frame source enabled", LogLevel::INFO, DeviceType::GUIDER);
#endif
    connect(guiderCore, &GuiderCore::requestExposure, this, [this](int exposureMs) {
        guiderExpMs = std::max(1, exposureMs);
        isGuiderLoopExp = true;
        guiderExposureInFlight = false;
        emit wsThread->sendMessageToClient("GuiderLoopExpStatus:true");
        emit wsThread->sendMessageToClient("GuiderUpdateStatus:1");
        if (guiderLoopTimer)
            guiderLoopTimer->start(0);
    });
    connect(guiderCore, &GuiderCore::requestPersistGuidingFits, this, &MainWindow::PersistGuidingFits);
    connect(guiderCore, &GuiderCore::requestPersistGuidingFitsAnnotated, this,
            [this](const QString& sourceFitsPath, const cv::Mat& image16, int imageW, int imageH,
                   const QVector<QPointF>& dedupCandidates,
                   const QVector<QPointF>& snrCandidates,
                   const QVector<QPointF>& candidates,
                   const QVector<QString>& candidateLabels,
                   const QPointF& selected) {
        m_debugStarDedupCandidates = dedupCandidates;
        m_debugStarSnrCandidates = snrCandidates;
        m_debugStarCandidates = candidates;
        m_debugStarCandidateLabels = candidateLabels;
        m_debugStarSelected = selected;
        Logger::Log("BuiltInGuider | requestPersistGuidingFitsAnnotated received in MainWindow: image=" +
                        std::to_string(imageW) + "x" + std::to_string(imageH) +
                        " dedupCandidates=" + std::to_string(dedupCandidates.size()) +
                        " snrCandidates=" + std::to_string(snrCandidates.size()) +
                        " candidates=" + std::to_string(candidates.size()) +
                        " selected=" + std::to_string((selected.x() != 0.0 || selected.y() != 0.0) ? 1 : 0),
                    LogLevel::INFO, DeviceType::GUIDER);
        PersistGuidingPreviewFromFrame(sourceFitsPath, image16);
    }, Qt::BlockingQueuedConnection);
    connect(guiderCore, &GuiderCore::requestPulse, this, [this](const guiding::PulseCommand& cmd) {
        ControlGuideEx(static_cast<int>(cmd.dir), cmd.durationMs, QStringLiteral("BuiltInGuider"));
    });
    connect(guiderCore, &GuiderCore::lockPositionChanged, this, [this](const QPointF& lockPosPx) {
        guiderLockPosPx = lockPosPx;
        guiderLockPosValid = true;
        glPHD_CurrentImageSizeX = std::max(1, glPHD_CurrentImageSizeX);
        glPHD_CurrentImageSizeY = std::max(1, glPHD_CurrentImageSizeY);
        emit wsThread->sendMessageToClient("PHD2StarCrossView:true");
        emit wsThread->sendMessageToClient(
            "PHD2StarCrossPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
            QString::number(glPHD_CurrentImageSizeY) + ":" +
            QString::number(static_cast<int>(std::lround(lockPosPx.x()))) + ":" +
            QString::number(static_cast<int>(std::lround(lockPosPx.y()))));
    });
    connect(guiderCore, &GuiderCore::lockStarSelected, this, [this](double x, double y, double snr, double hfd) {
        const int searchHalfSizePx = std::max(4, guiderParamsCache.guideSearchHalfSizePx);
        glPHD_CurrentImageSizeX = std::max(1, glPHD_CurrentImageSizeX);
        glPHD_CurrentImageSizeY = std::max(1, glPHD_CurrentImageSizeY);
        emit wsThread->sendMessageToClient("PHD2StarBoxView:true");
        emit wsThread->sendMessageToClient(
            "PHD2StarBoxPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
            QString::number(glPHD_CurrentImageSizeY) + ":" +
            QString::number(static_cast<int>(std::lround(x))) + ":" +
            QString::number(static_cast<int>(std::lround(y))) + ":" +
            QString::number(searchHalfSizePx));
        emit wsThread->sendMessageToClient(
            "GuiderSelectedStar:" +
            QString::number(x, 'f', 2) + ":" +
            QString::number(y, 'f', 2) + ":" +
            QString::number(snr, 'f', 2) + ":" +
            QString::number(hfd, 'f', 2));
        emit wsThread->sendMessageToClient(
            "GuiderStarSelected:x=" +
            QString::number(x, 'f', 2) + ":y=" +
            QString::number(y, 'f', 2) + ":snr=" +
            QString::number(snr, 'f', 2) + ":hfd=" +
            QString::number(hfd, 'f', 2));
        emit wsThread->sendMessageToClient(
            QStringLiteral("SendDebugMessage|Info|导星选星成功：X=%1 Y=%2 SNR=%3 HFD=%4")
                .arg(x, 0, 'f', 2)
                .arg(y, 0, 'f', 2)
                .arg(snr, 0, 'f', 2)
                .arg(hfd, 0, 'f', 2));
    });
    // DEBUG: publish current frame's star candidates so the frontend can draw
    // color overlays on top of the guider preview.
    connect(guiderCore, &GuiderCore::debugStarCandidatesChanged, this,
            [this](int imageW, int imageH, const QVector<QPointF>& dedupCandidates,
                   const QVector<QPointF>& snrCandidates,
                   const QVector<QPointF>& candidates,
                   const QVector<QString>& candidateLabels,
                   const QPointF& selected) {
        m_debugStarDedupCandidates = dedupCandidates;
        m_debugStarSnrCandidates = snrCandidates;
        m_debugStarCandidates = candidates;
        m_debugStarCandidateLabels = candidateLabels;
        m_debugStarSelected = selected;
        Logger::Log("BuiltInGuider | debugStarCandidatesChanged received in MainWindow: image=" +
                        std::to_string(imageW) + "x" + std::to_string(imageH) +
                        " dedupCandidates=" + std::to_string(dedupCandidates.size()) +
                        " snrCandidates=" + std::to_string(snrCandidates.size()) +
                        " candidates=" + std::to_string(candidates.size()) +
                        " selected=" + std::to_string((selected.x() != 0.0 || selected.y() != 0.0) ? 1 : 0),
                    LogLevel::INFO, DeviceType::GUIDER);

        emit wsThread->sendMessageToClient("ClearGuiderDebugCandidates");

        const int safeImageW = std::max(1, imageW);
        const int safeImageH = std::max(1, imageH);
        const int maxCandidatesToSend = 48;
        for (int i = 0; i < dedupCandidates.size() && i < maxCandidatesToSend; ++i)
        {
            const auto& pt = dedupCandidates[i];
            emit wsThread->sendMessageToClient(
                "GuiderDebugDedupCandidatePosition:" + QString::number(safeImageW) + ":" +
                QString::number(safeImageH) + ":" +
                QString::number(static_cast<int>(std::lround(pt.x()))) + ":" +
                QString::number(static_cast<int>(std::lround(pt.y()))));
        }

        for (int i = 0; i < snrCandidates.size() && i < maxCandidatesToSend; ++i)
        {
            const auto& pt = snrCandidates[i];
            emit wsThread->sendMessageToClient(
                "GuiderDebugSnrCandidatePosition:" + QString::number(safeImageW) + ":" +
                QString::number(safeImageH) + ":" +
                QString::number(static_cast<int>(std::lround(pt.x()))) + ":" +
                QString::number(static_cast<int>(std::lround(pt.y()))));
        }

        for (int i = 0; i < candidates.size() && i < maxCandidatesToSend; ++i)
        {
            const auto& pt = candidates[i];
            const QString label = (i < candidateLabels.size()) ? candidateLabels[i] : QString();
            emit wsThread->sendMessageToClient(
                "GuiderDebugCandidatePosition:" + QString::number(safeImageW) + ":" +
                QString::number(safeImageH) + ":" +
                QString::number(static_cast<int>(std::lround(pt.x()))) + ":" +
                QString::number(static_cast<int>(std::lround(pt.y()))));
            emit wsThread->sendMessageToClient(
                "GuiderDebugFinalCandidatePosition:" + QString::number(safeImageW) + ":" +
                QString::number(safeImageH) + ":" +
                QString::number(static_cast<int>(std::lround(pt.x()))) + ":" +
                QString::number(static_cast<int>(std::lround(pt.y()))) + ":" +
                label);
        }

        if (selected.x() != 0.0 || selected.y() != 0.0)
        {
            emit wsThread->sendMessageToClient(
                "GuiderDebugSelectedCandidatePosition:" + QString::number(safeImageW) + ":" +
                QString::number(safeImageH) + ":" +
                QString::number(static_cast<int>(std::lround(selected.x()))) + ":" +
                QString::number(static_cast<int>(std::lround(selected.y()))));
        }
    });
    connect(guiderCore, &GuiderCore::guideStarCentroidChanged, this, [this](const QPointF& centroidPx) {
        const int searchHalfSizePx = std::max(4, guiderParamsCache.guideSearchHalfSizePx);
        guiderGuideStarCentroidPx = centroidPx;
        guiderGuideStarCentroidValid = true;
        glPHD_CurrentImageSizeX = std::max(1, glPHD_CurrentImageSizeX);
        glPHD_CurrentImageSizeY = std::max(1, glPHD_CurrentImageSizeY);
        emit wsThread->sendMessageToClient("PHD2StarBoxView:true");
        emit wsThread->sendMessageToClient(
            "PHD2StarBoxPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
            QString::number(glPHD_CurrentImageSizeY) + ":" +
            QString::number(static_cast<int>(std::lround(centroidPx.x()))) + ":" +
            QString::number(static_cast<int>(std::lround(centroidPx.y()))) + ":" +
            QString::number(searchHalfSizePx));
    });
    connect(guiderCore, &GuiderCore::multiStarSecondaryPointsChanged, this, [this](const QVector<QPointF>& ptsPx) {
        guiderMultiStarSecondaryPtsPx = ptsPx;
        guiderMultiStarSecondaryPtsPending = !ptsPx.isEmpty();

        emit wsThread->sendMessageToClient("ClearPHD2MultiStars");
        if (ptsPx.isEmpty())
            return;

        if (glPHD_CurrentImageSizeX <= 0 || glPHD_CurrentImageSizeY <= 0 || !guiderPhaseGuiding || guiderDirectionDetectActive)
            return;

        for (int i = 0; i < ptsPx.size(); ++i)
        {
            if (i >= 8) break;
            const auto& p = ptsPx[i];
            emit wsThread->sendMessageToClient(
                "PHD2MultiStarsPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
                QString::number(glPHD_CurrentImageSizeY) + ":" +
                QString::number(static_cast<int>(std::lround(p.x()))) + ":" +
                QString::number(static_cast<int>(std::lround(p.y()))));
        }
        guiderMultiStarSecondaryPtsPending = false;
    });
    connect(guiderCore, &GuiderCore::directionDetectionStateChanged, this, [this](bool active) {
        guiderDirectionDetectActive = active;
        emit wsThread->sendMessageToClient(QString("GuiderStatus:%1")
                                               .arg(active ? "InDirectionDetection"
                                                           : (guiderPhaseGuiding ? "InGuiding" : "InCalibration")));
    });
    connect(guiderCore, &GuiderCore::guideErrorUpdated, this, [this](double raErrPx, double decErrPx) {
        const double scale = currentGuiderArcsecPerPixel();
        const double raErr = (scale > 0.0) ? (raErrPx * scale) : raErrPx;
        const double decErr = (scale > 0.0) ? (decErrPx * scale) : decErrPx;
        emit wsThread->sendMessageToClient(
            "AddLineChartData:" + QString::number(guiderChartSampleIndex++) + ":" +
            QString::number(raErr, 'f', 6) + ":" +
            QString::number(decErr, 'f', 6));
        // Keep the scatter plot fed from the same real-time guider error stream.
        // Match the legacy scatter convention by flipping Y when drawing dX/dY points.
        emit wsThread->sendMessageToClient(
            "AddScatterChartData:" + QString::number(raErr, 'f', 6) + ":" +
            QString::number(-decErr, 'f', 6));
    });
    connect(guiderCore, &GuiderCore::guidePulseIssued, this,
            [this](const guiding::PulseCommand& cmd, double raErrPx, double decErrPx) {
        const QString dir =
            (cmd.dir == guiding::GuideDir::North) ? QStringLiteral("NORTH") :
            (cmd.dir == guiding::GuideDir::South) ? QStringLiteral("SOUTH") :
            (cmd.dir == guiding::GuideDir::East)  ? QStringLiteral("EAST")  :
                                                    QStringLiteral("WEST");
        emit wsThread->sendMessageToClient(
            "GuiderPulse:" + dir + ":" + QString::number(cmd.durationMs) +
            ":raErrPx=" + QString::number(raErrPx, 'f', 6) +
            ":decErrPx=" + QString::number(decErrPx, 'f', 6));
    });
    connect(guiderCore, &GuiderCore::calibrationResultChanged, this, [this](const guiding::CalibrationResult& r) {
        emit wsThread->sendMessageToClient(
            QStringLiteral("GuiderCalibration:cameraAngleDeg=") + QString::number(r.cameraAngleDeg, 'f', 2) +
            ":orthoErrDeg=" + QString::number(r.orthoErrDeg, 'f', 2) +
            ":raMsPerPixel=" + QString::number(r.raMsPerPixel, 'f', 2) +
            ":decMsPerPixel=" + QString::number(r.decMsPerPixel, 'f', 2) +
            ":raSteps=" + QString::number(r.raStepCount) +
            ":decSteps=" + QString::number(r.decStepCount) +
            ":raTravelPx=" + QString::number(r.raTravelPx, 'f', 2) +
            ":decTravelPx=" + QString::number(r.decTravelPx, 'f', 2));
    });
    connect(guiderCore, &GuiderCore::stateChanged, this, [this](guiding::State state) {
        guiderCoreStateCache = state;
        guiderPhaseGuiding = (state == guiding::State::Guiding);
        if (state == guiding::State::Idle || state == guiding::State::Looping ||
            state == guiding::State::Stopped || state == guiding::State::Error)
        {
            guiderDirectionDetectActive = false;
            guiderChartSampleIndex = 0;
            guiderLockPosValid = false;
            guiderGuideStarCentroidValid = false;
            isGuiderLoopExp = false;
            guiderExposureInFlight = false;
            if (guiderLoopTimer)
                guiderLoopTimer->stop();
            if (sdkGuiderExposureTimer)
                sdkGuiderExposureTimer->stop();
            emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
            emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
            emit wsThread->sendMessageToClient("PHD2StarBoxView:false");
            emit wsThread->sendMessageToClient("PHD2StarCrossView:false");
            emit wsThread->sendMessageToClient("ClearPHD2MultiStars");
            stopGuiderAutoBatchCapture();
            clearGuiderDebugAnnotations(state == guiding::State::Stopped);
        }
        emit wsThread->sendMessageToClient(QString("GuiderCoreState:%1").arg(static_cast<int>(state)));
        switch (state)
        {
        case guiding::State::Selecting:
            emit wsThread->sendMessageToClient("GuiderSwitchStatus:true");
            emit wsThread->sendMessageToClient("GuiderStatus:InSelecting");
            emit wsThread->sendMessageToClient("SendDebugMessage|Info|导星正在自动选星，请稍候");
            break;
        case guiding::State::Calibrating:
            emit wsThread->sendMessageToClient("GuiderSwitchStatus:true");
            emit wsThread->sendMessageToClient("GuiderStatus:InCalibration");
            emit wsThread->sendMessageToClient("SendDebugMessage|Info|导星已锁定星点，正在校准");
            break;
        case guiding::State::Guiding:
            emit wsThread->sendMessageToClient("GuiderSwitchStatus:true");
            emit wsThread->sendMessageToClient("GuiderStatus:InGuiding");
            emit wsThread->sendMessageToClient("SendDebugMessage|Info|导星已进入闭环导星");
            break;
        case guiding::State::Idle:
        case guiding::State::Looping:
        case guiding::State::Stopped:
        case guiding::State::Error:
        default:
            emit wsThread->sendMessageToClient("GuiderSwitchStatus:false");
            break;
        }
    });
    connect(guiderCore, &GuiderCore::infoMessage, this, [this](const QString& msg) {
        Logger::Log("BuiltInGuider | " + msg.toStdString(), LogLevel::INFO, DeviceType::GUIDER);
        emit wsThread->sendMessageToClient("GuiderCoreInfo:" + msg);
        if (msg.contains(QStringLiteral("选星成功")))
        {
            emit wsThread->sendMessageToClient("GuiderStatus:StarSelected");
            emit wsThread->sendMessageToClient("SendDebugMessage|Info|" + msg);
        }
        else if (msg.contains(QStringLiteral("自动选星进行中")) ||
                 msg.contains(QStringLiteral("等待下一帧")) ||
                 msg.contains(QStringLiteral("候选星点")))
        {
            emit wsThread->sendMessageToClient("GuiderStatus:SelectingProgress");
            emit wsThread->sendMessageToClient("SendDebugMessage|Info|" + msg);
        }
        else if (msg.contains(QStringLiteral("进入校准阶段")) ||
                 msg.contains(QStringLiteral("校准完成")))
        {
            emit wsThread->sendMessageToClient("SendDebugMessage|Info|" + msg);
        }
    });
    connect(guiderCore, &GuiderCore::errorOccurred, this, [this](const QString& msg) {
        Logger::Log("BuiltInGuider | " + msg.toStdString(), LogLevel::WARNING, DeviceType::GUIDER);
        emit wsThread->sendMessageToClient("GuiderCoreError:" + msg);
        emit wsThread->sendMessageToClient("ErrorMessage:导星错误 - " + msg);
        emit wsThread->sendMessageToClient("SendDebugMessage|Warning|导星错误：" + msg);
        if (msg.contains(QStringLiteral("LostStar"), Qt::CaseInsensitive) ||
            msg.contains(QStringLiteral("丢星"), Qt::CaseInsensitive))
        {
            emit wsThread->sendMessageToClient("GuiderStatus:StarLostAlert");
        }
    });
    // getConnectedSerialPorts();

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

void MainWindow::startPoleCameraSingleCapture(int exposureMs)
{
    const int expMs = std::max(1, exposureMs);
    sdkGuiderExposureRole = "PoleCamera";

    if (isGuiderLoopExp)
    {
        Logger::Log("startPoleCameraSingleCapture | stopping guider loop before polar capture",
                    LogLevel::INFO, DeviceType::GUIDER);
        if (guiderCore)
        {
            postGuiderCore(guiderCore, [](GuiderCore *core) {
                core->stopGuiding();
                core->stopLoop();
            });
        }
        isGuiderLoopExp = false;
        if (guiderLoopTimer)
            guiderLoopTimer->stop();
        if (sdkGuiderExposureTimer)
            sdkGuiderExposureTimer->stop();
        emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
    }

    if (isPoleCameraSDK())
    {
        SdkSerialExecutor *poleExec = sdkPoleCameraExecutor();
        if (!poleExec || !poleExec->isRunning())
        {
            Logger::Log("startPoleCameraSingleCapture | sdkPoleCamExec is not running", LogLevel::ERROR, DeviceType::MAIN);
            polarGuiderSingleCapturePending = false;
            guiderExposureInFlight = false;
            return;
        }
        if (guiderExposureInFlight || sdkGuiderFrameTaskInFlight.load())
        {
            Logger::Log("startPoleCameraSingleCapture | exposure/readout is already in flight, retrying shortly",
                        LogLevel::WARNING, DeviceType::MAIN);
            QTimer::singleShot(250, this, [this, expMs]() {
                const bool oldPolarRunning = polarAlignment != nullptr && polarAlignment->isRunning();
                const bool poleMasterRunning = poleMasterPolarAlignment != nullptr && poleMasterPolarAlignment->isRunning();
                if ((oldPolarRunning || poleMasterRunning) &&
                    currentPolarAlignmentCameraRole == PolarAlignmentCameraRole::PoleCamera)
                {
                    startPoleCameraSingleCapture(expMs);
                }
            });
            return;
        }

        polarGuiderSingleCapturePending = true;
        guiderExposureInFlight = true;
        const double expSec = expMs / 1000.0;
        const SdkDeviceHandle handleSnap = sdkPoleScopeHandle;

        poleExec->post([this, handleSnap, expMs, expSec]() {
            auto failOnMain = [this](const std::string &message) {
                QMetaObject::invokeMethod(this, [this, message]() {
                    Logger::Log(message, LogLevel::ERROR, DeviceType::MAIN);
                    polarGuiderSingleCapturePending = false;
                    guiderExposureInFlight = false;
                }, Qt::QueuedConnection);
            };

            SdkCommand cancelCmd;
            cancelCmd.type = SdkCommandType::Custom;
            cancelCmd.name = "CancelExposure";
            cancelCmd.payload = std::any();
            SdkManager::instance().callByHandle(handleSnap, cancelCmd);

            SdkCommand setExpCmd;
            setExpCmd.type = SdkCommandType::Custom;
            setExpCmd.name = "SetExposure";
            setExpCmd.payload = expSec * 1000000.0;
            SdkResult setRes = SdkManager::instance().callByHandle(handleSnap, setExpCmd);
            if (!setRes.success)
            {
                failOnMain("startPoleCameraSingleCapture | SDK SetExposure failed: " + setRes.message);
                return;
            }

            SdkCommand startExpCmd;
            startExpCmd.type = SdkCommandType::Custom;
            startExpCmd.name = "StartSingleExposure";
            startExpCmd.payload = std::any();
            SdkResult startRes = SdkManager::instance().callByHandle(handleSnap, startExpCmd);
            if (!startRes.success)
            {
                failOnMain("startPoleCameraSingleCapture | SDK StartSingleExposure failed: " + startRes.message);
                return;
            }

            QMetaObject::invokeMethod(this, [this, expMs]() {
                sdkGuiderExposureStartTime = QDateTime::currentMSecsSinceEpoch();
                sdkGuiderExposureExpectedDuration = expMs;
                sdkGuiderExposureRole = "PoleCamera";
                if (sdkGuiderExposureTimer)
                    sdkGuiderExposureTimer->start(expMs);
            }, Qt::QueuedConnection);
        });
        return;
    }

    if (!indi_Client || dpPoleScope == nullptr || !dpPoleScope->isConnected())
    {
        Logger::Log("startPoleCameraSingleCapture | pole camera is not connected", LogLevel::ERROR, DeviceType::MAIN);
        polarGuiderSingleCapturePending = false;
        guiderExposureInFlight = false;
        return;
    }

    polarGuiderSingleCapturePending = true;
    guiderExposureInFlight = true;
    const double expSec = expMs / 1000.0;
    indi_Client->takeExposure(dpPoleScope, expSec);
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
/** sysfs GPIO 节点在 export 后可能延迟出现，对 ENOENT/EINTR 做短重试。 */
int openSysfsGpioRetry(const char *path, int flags)
{
    int fd = -1;
    for (int i = 0; i < 80; ++i)
    {
        fd = open(path, flags);
        if (fd >= 0)
            return fd;
        if (errno != ENOENT && errno != EINTR)
            break;
        usleep(2500);
    }
    return -1;
}

bool readSysfsGpioText(const char *path, char *buffer, size_t bufferSize)
{
    if (!buffer || bufferSize == 0)
        return false;

    const int fd = openSysfsGpioRetry(path, O_RDONLY);
    if (fd < 0)
        return false;

    errno = 0;
    const ssize_t bytesRead = read(fd, buffer, bufferSize - 1);
    close(fd);
    if (bytesRead <= 0)
        return false;

    buffer[bytesRead] = '\0';
    return true;
}

bool gpioDirectionIsOut(const char *pin)
{
    char path[128];
    char direction[16];

    snprintf(path, sizeof(path), GPIO_PATH "/gpio%s/direction", pin);
    if (!readSysfsGpioText(path, direction, sizeof(direction)))
        return false;

    return strncmp(direction, "out", 3) == 0;
}

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

void MainWindow::initGPIO()
{
    Logger::Log("Initializing GPIO...", LogLevel::INFO, DeviceType::MAIN);
    // Initialize GPIO_PIN_1
    exportGPIO(GPIO_PIN_1);
    setGPIODirection(GPIO_PIN_1, "out");
    Logger::Log(gpioDirectionIsOut(GPIO_PIN_1)
                    ? "Set direction of GPIO_PIN_1 to output completed!"
                    : "GPIO_PIN_1 direction is not out after initialization",
                gpioDirectionIsOut(GPIO_PIN_1) ? LogLevel::INFO : LogLevel::WARNING,
                DeviceType::MAIN);

    // Initialize GPIO_PIN_2
    exportGPIO(GPIO_PIN_2);
    setGPIODirection(GPIO_PIN_2, "out");
    Logger::Log(gpioDirectionIsOut(GPIO_PIN_2)
                    ? "Set direction of GPIO_PIN_2 to output completed!"
                    : "GPIO_PIN_2 direction is not out after initialization",
                gpioDirectionIsOut(GPIO_PIN_2) ? LogLevel::INFO : LogLevel::WARNING,
                DeviceType::MAIN);

    // Set GPIO_PIN_1 to high level
    setGPIOValue(GPIO_PIN_1, "1");
    Logger::Log("Set GPIO_PIN_1 level to high completed!", LogLevel::INFO, DeviceType::MAIN);
    // Set GPIO_PIN_2 to high level
    setGPIOValue(GPIO_PIN_2, "1");
    Logger::Log("Set GPIO_PIN_2 level to high completed!", LogLevel::INFO, DeviceType::MAIN);

    Logger::Log("GPIO initialization completed!", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::exportGPIO(const char *pin)
{
    int fd;
    char buf[64];

    // Export GPIO pin
    fd = open(GPIO_EXPORT, O_WRONLY);
    if (fd < 0)
    {
        Logger::Log("Failed to open export file for writing", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    snprintf(buf, sizeof(buf), "%s", pin);
    errno = 0;
    if (write(fd, buf, strlen(buf)) != (ssize_t) strlen(buf))
    {
        // EBUSY usually means the GPIO is already exported (common on restart) — not a fatal error.
        if (errno == EBUSY)
        {
            Logger::Log(std::string("GPIO already exported: pin=") + pin, LogLevel::DEBUG, DeviceType::MAIN);
        }
        else
        {
            Logger::Log(std::string("Failed to write to export file: ") + strerror(errno), LogLevel::WARNING, DeviceType::MAIN);
        }
        close(fd);
        return;
    }
    close(fd);
    Logger::Log("GPIO pin export successful", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::setGPIODirection(const char *pin, const char *direction)
{
    int fd;
    char path[128];

    // Set GPIO direction
    snprintf(path, sizeof(path), GPIO_PATH "/gpio%s/direction", pin);
    fd = openSysfsGpioRetry(path, O_WRONLY);
    if (fd < 0)
    {
        Logger::Log(std::string("Failed to open GPIO direction file for writing: ") + path + " " + strerror(errno),
                    LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    errno = 0;
    if (write(fd, direction, strlen(direction)) != (ssize_t) strlen(direction))
    {
        Logger::Log(std::string("Failed to set GPIO direction: ") + strerror(errno), LogLevel::WARNING, DeviceType::MAIN);
        close(fd);
        return;
    }
    close(fd);
    Logger::Log("GPIO direction set successfully", LogLevel::INFO, DeviceType::MAIN);
}
void MainWindow::setGPIOValue(const char *pin, const char *value)
{
    int fd;
    char path[128];

    if (!gpioDirectionIsOut(pin))
    {
        Logger::Log(std::string("GPIO direction is not out before writing value, trying to repair: pin=") + pin,
                    LogLevel::WARNING, DeviceType::MAIN);
        setGPIODirection(pin, "out");
        if (!gpioDirectionIsOut(pin))
        {
            Logger::Log(std::string("GPIO direction is still not out, abort writing value: pin=") + pin,
                        LogLevel::WARNING, DeviceType::MAIN);
            return;
        }
    }

    // Set GPIO value
    snprintf(path, sizeof(path), GPIO_PATH "/gpio%s/value", pin);
    fd = openSysfsGpioRetry(path, O_WRONLY);
    if (fd < 0)
    {
        Logger::Log(std::string("Failed to open GPIO value file for writing: ") + path + " " + strerror(errno),
                    LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    errno = 0;
    if (write(fd, value, strlen(value)) != (ssize_t) strlen(value))
    {
        Logger::Log(std::string("Failed to write to GPIO value: ") + strerror(errno), LogLevel::WARNING, DeviceType::MAIN);
        close(fd);
        return;
    }
    close(fd);
    Logger::Log("GPIO value set successfully", LogLevel::INFO, DeviceType::MAIN);
}
int MainWindow::readGPIOValue(const char *pin)
{
    char path[128];
    char value[8]; // Store the read value

    // Construct the path to the GPIO value file
    snprintf(path, sizeof(path), GPIO_PATH "/gpio%s/value", pin);

    if (!readSysfsGpioText(path, value, sizeof(value)))
    {
        Logger::Log(std::string("Failed to read GPIO value file: ") + path + " " + strerror(errno),
                    LogLevel::WARNING, DeviceType::MAIN);
        return -1; // Return -1 to indicate read failure
    }

    // Determine if the read value is '1' or '0'
    if (value[0] == '1')
    {
        return 1; // Return 1 to indicate high level
    }
    else if (value[0] == '0')
    {
        return 0; // Return 0 to indicate low level
    }
    else
    {
        return -1; // If the value is not '0' or '1', return -1
    }
}
void MainWindow::getGPIOsStatus()
{
    int value1 = readGPIOValue(GPIO_PIN_1);
    emit wsThread->sendMessageToClient("OutputPowerStatus:1:" + QString::number(value1));
    int value2 = readGPIOValue(GPIO_PIN_2);
    emit wsThread->sendMessageToClient("OutputPowerStatus:2:" + QString::number(value2));
}

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














// SDK 导星曝光定时器回调：轮询获取导星图像（用于 GuiderLoopExpSwitch）
void MainWindow::onSdkGuiderExposureTimerTimeout()
{
    if (!sdkGuiderExposureTimer)
        return;

    // 单次轮询：拿到结果后由本函数决定是否继续 start()
    sdkGuiderExposureTimer->stop();

    const bool poleCapture = (sdkGuiderExposureRole == "PoleCamera");
    const bool guiderSdk =
        (!poleCapture &&
         systemdevicelist.system_devices.size() > 1 &&
         systemdevicelist.system_devices[1].isSDKConnect &&
         sdkGuiderHandle != nullptr);
    const bool poleSdk =
        (poleCapture &&
         systemdevicelist.system_devices.size() > 2 &&
         systemdevicelist.system_devices[2].isSDKConnect &&
         sdkPoleScopeHandle != nullptr);

    if ((!guiderSdk && !poleSdk) || (!isGuiderLoopExp && !polarGuiderSingleCapturePending))
    {
        guiderExposureInFlight = false;
        return;
    }

    SdkSerialExecutor *captureExec = sdkExecutorForPolarRole(poleCapture
                                                                 ? PolarAlignmentCameraRole::PoleCamera
                                                                 : PolarAlignmentCameraRole::Guider);
    if (!captureExec || !captureExec->isRunning())
    {
        Logger::Log("onSdkGuiderExposureTimerTimeout | SDK capture executor not running, stop guider polling",
                    LogLevel::ERROR, DeviceType::GUIDER);
        guiderExposureInFlight = false;
        isGuiderLoopExp = false;
        if (guiderCore)
        {
            postGuiderCore(guiderCore, [](GuiderCore *core) {
                core->stopGuiding();
                core->stopLoop();
            });
        }
        emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
        return;
    }

    // 防重入：如果上一轮 GetSingleFrame 还在 SDK 线程执行，就稍后再试
    if (sdkGuiderFrameTaskInFlight.exchange(true))
    {
        Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | frame task already in flight, retry timer in 10ms",
                    LogLevel::INFO, DeviceType::GUIDER);
        sdkGuiderExposureTimer->start(10);
        return;
    }

    const SdkDeviceHandle handleSnap = poleCapture ? sdkPoleScopeHandle : sdkGuiderHandle;
    const PolarAlignmentCameraRole captureRole =
        poleCapture ? PolarAlignmentCameraRole::PoleCamera : PolarAlignmentCameraRole::Guider;
    const qint64 startSnap = sdkGuiderExposureStartTime;
    const int expectedSnap = sdkGuiderExposureExpectedDuration;
    const qint64 timerFiredAtMs = QDateTime::currentMSecsSinceEpoch();

    Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | fired elapsedSinceExposureStartMs=" +
                    std::to_string(timerFiredAtMs - startSnap) +
                    " expectedMs=" + std::to_string(expectedSnap),
                LogLevel::INFO, DeviceType::GUIDER);

    captureExec->post([this, handleSnap, startSnap, expectedSnap, timerFiredAtMs, captureRole]() {
        QElapsedTimer workerPerf;
        workerPerf.start();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 elapsed = now - startSnap;
        const qint64 expected = std::max<qint64>(1, expectedSnap);
        const qint64 maxWaitMs = std::max<qint64>(expected + 5000, expected * 3);

        Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | sdkWorkerEnter queueDelayMs=" +
                        std::to_string(now - timerFiredAtMs) +
                        " elapsedSinceExposureStartMs=" + std::to_string(elapsed) +
                        " expectedMs=" + std::to_string(expected) +
                        " maxWaitMs=" + std::to_string(maxWaitMs),
                    LogLevel::INFO, DeviceType::GUIDER);

        if (elapsed > maxWaitMs)
        {
            QElapsedTimer cancelPerf;
            cancelPerf.start();
            SdkCommand cancelCmd;
            cancelCmd.type = SdkCommandType::Custom;
            cancelCmd.name = "CancelExposure";
            cancelCmd.payload = std::any();
            SdkResult cancelRes = SdkManager::instance().callByHandle(handleSnap, cancelCmd);
            const qint64 cancelCostMs = cancelPerf.elapsed();
            const qint64 workerTotalMs = workerPerf.elapsed();

            Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | timeout CancelExposure returned success=" +
                            std::to_string(cancelRes.success ? 1 : 0) +
                            " costMs=" + std::to_string(cancelCostMs) +
                            " workerTotalMs=" + std::to_string(workerTotalMs) +
                            " msg=" + cancelRes.message,
                        LogLevel::INFO, DeviceType::GUIDER);

            QMetaObject::invokeMethod(this, [this, cancelRes, elapsed, expected, maxWaitMs, cancelCostMs, workerTotalMs]() {
                sdkGuiderFrameTaskInFlight = false;
                Logger::Log("onSdkGuiderExposureTimerTimeout | TIMEOUT waiting guider frame (elapsed=" +
                                std::to_string(elapsed) + "ms, expected=" + std::to_string(expected) +
                                "ms, maxWait=" + std::to_string(maxWaitMs) +
                                "ms, cancelCost=" + std::to_string(cancelCostMs) +
                                "ms, workerTotal=" + std::to_string(workerTotalMs) + "ms)",
                            LogLevel::ERROR, DeviceType::GUIDER);
                if (!cancelRes.success)
                {
                    Logger::Log("onSdkGuiderExposureTimerTimeout | CancelExposure failed: " + cancelRes.message,
                                LogLevel::WARNING, DeviceType::GUIDER);
                }
                guiderExposureInFlight = false;
                polarGuiderSingleCapturePending = false;
                isGuiderLoopExp = false;
                if (guiderCore)
                {
                    postGuiderCore(guiderCore, [](GuiderCore *core) {
                        core->stopGuiding();
                        core->stopLoop();
                    });
                }
                emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
                emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
            }, Qt::QueuedConnection);
            return;
        }

        // GetSingleFrame（SDK 线程）
        SdkCommand getFrameCmd;
        getFrameCmd.type = SdkCommandType::Custom;
        getFrameCmd.name = "GetSingleFrame";
        getFrameCmd.payload = std::any();
        QElapsedTimer getFramePerf;
        getFramePerf.start();
        SdkResult frameRes = SdkManager::instance().callByHandle(handleSnap, getFrameCmd);
        const qint64 getFrameCostMs = getFramePerf.elapsed();
        const qint64 workerTotalMs = workerPerf.elapsed();

        Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | GetSingleFrame returned success=" +
                        std::to_string(frameRes.success ? 1 : 0) +
                        " costMs=" + std::to_string(getFrameCostMs) +
                        " workerTotalMs=" + std::to_string(workerTotalMs) +
                        " msg=" + frameRes.message,
                    LogLevel::INFO, DeviceType::GUIDER);

        QMetaObject::invokeMethod(this, [this, frameRes, expected, getFrameCostMs, workerTotalMs, timerFiredAtMs, captureRole]() mutable {
            QElapsedTimer mainPerf;
            mainPerf.start();
            sdkGuiderFrameTaskInFlight = false;

            Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | mainThreadResultEnter totalSinceTimerFiredMs=" +
                            std::to_string(QDateTime::currentMSecsSinceEpoch() - timerFiredAtMs) +
                            " getFrameCostMs=" + std::to_string(getFrameCostMs) +
                            " workerTotalMs=" + std::to_string(workerTotalMs),
                        LogLevel::INFO, DeviceType::GUIDER);

            const bool poleCapture = (captureRole == PolarAlignmentCameraRole::PoleCamera);
            const bool captureSdk =
                poleCapture
                    ? (systemdevicelist.system_devices.size() > 2 &&
                       systemdevicelist.system_devices[2].isSDKConnect &&
                       sdkPoleScopeHandle != nullptr)
                    : (systemdevicelist.system_devices.size() > 1 &&
                       systemdevicelist.system_devices[1].isSDKConnect &&
                       sdkGuiderHandle != nullptr);
            if (!captureSdk)
            {
                guiderExposureInFlight = false;
                return;
            }

            if (frameRes.success)
            {
                SdkFrameData frame = std::any_cast<SdkFrameData>(frameRes.payload);
                const bool hasFrameData =
                    (!frame.pixels.empty()) || (frame.rawBuffer != nullptr && frame.rawBytes > 0);
                if (frame.width <= 0 || frame.height <= 0 || !hasFrameData)
                {
                    Logger::Log("onSdkGuiderExposureTimerTimeout | invalid frame, retry",
                                LogLevel::WARNING, DeviceType::GUIDER);
                }
                else
                {
                    // SDK 导星帧统一走内置导星链路：
                    // 1) 先写固定 FITS 路径
                    // 2) 交给 GuiderCore::onNewFrame（内部会触发 PersistGuidingFits + 识别）
                    const QString sdkGuiderFitsPath = poleCapture
                        ? QStringLiteral("/dev/shm/polecamera.fits")
                        : QStringLiteral("/dev/shm/guiding.fits");
                    QElapsedTimer saveFitsPerf;
                    saveFitsPerf.start();
                    SaveQhyFrameDataToFits(frame, sdkGuiderFitsPath.toStdString());
                    Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | SaveQhyFrameDataToFits costMs=" +
                                    std::to_string(saveFitsPerf.elapsed()) +
                                    " mainTotalMs=" + std::to_string(mainPerf.elapsed()),
                                LogLevel::INFO, DeviceType::GUIDER);

                    if (polarGuiderSingleCapturePending)
                    {
                        notifyPolarAlignmentCaptureReady(captureRole, sdkGuiderFitsPath);
                        polarGuiderSingleCapturePending = false;
                    }

                    if (!poleCapture && guiderCore)
                    {
                        QElapsedTimer invokePerf;
                        invokePerf.start();
                        QMetaObject::invokeMethod(guiderCore, "onNewFrame", Qt::QueuedConnection,
                                                  Q_ARG(QString, sdkGuiderFitsPath));
                        Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | invoke guiderCore onNewFrame costMs=" +
                                        std::to_string(invokePerf.elapsed()) +
                                        " mainTotalMs=" + std::to_string(mainPerf.elapsed()),
                                    LogLevel::INFO, DeviceType::GUIDER);
                    }
                    else
                    {
                        QElapsedTimer persistPerf;
                        persistPerf.start();
                        PersistGuidingFits(sdkGuiderFitsPath);
                        Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | PersistGuidingFits costMs=" +
                                        std::to_string(persistPerf.elapsed()) +
                                        " mainTotalMs=" + std::to_string(mainPerf.elapsed()),
                                    LogLevel::INFO, DeviceType::GUIDER);
                    }
                }

                // 一帧处理完成，放行下一帧
                guiderExposureInFlight = false;
                if (isGuiderLoopExp && guiderLoopTimer)
                    guiderLoopTimer->start(1);
                Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | success path done mainTotalMs=" +
                                std::to_string(mainPerf.elapsed()),
                            LogLevel::INFO, DeviceType::GUIDER);
                return;
            }

            // 未就绪：继续轮询
            const qint64 elapsed2 = QDateTime::currentMSecsSinceEpoch() - sdkGuiderExposureStartTime;
            Logger::Log("onSdkGuiderExposureTimerTimeout | GetSingleFrame not ready: " + frameRes.message,
                        LogLevel::DEBUG, DeviceType::GUIDER);
            int retryMs = 10;
            if (elapsed2 < expected) {
                retryMs = static_cast<int>(std::max<qint64>(1, expected - elapsed2));
            } else if (elapsed2 > expected + 2000) {
                retryMs = 50;
            }
            Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | not ready retryMs=" +
                            std::to_string(retryMs) +
                            " elapsedSinceExposureStartMs=" + std::to_string(elapsed2) +
                            " mainTotalMs=" + std::to_string(mainPerf.elapsed()),
                        LogLevel::INFO, DeviceType::GUIDER);
            sdkGuiderExposureTimer->start(retryMs);
        }, Qt::QueuedConnection);
    });
}












void MainWindow::getClientSettings()
{

    std::string fileName = "config/config.ini";

    std::unordered_map<std::string, std::string> config;

    Tools::readClientSettings(fileName, config);

    const auto itMain = config.find("MainCameraFocalLength");
    const auto itLegacy = config.find("FocalLength");
    if (itMain == config.end() && itLegacy != config.end())
    {
        const QString legacyValue = QString::fromStdString(itLegacy->second).trimmed();
        if (!legacyValue.isEmpty())
        {
            setClientSettings("MainCameraFocalLength", legacyValue);
            config["MainCameraFocalLength"] = legacyValue.toStdString();
            Logger::Log("getClientSettings | migrated FocalLength -> MainCameraFocalLength = " +
                            legacyValue.toStdString(),
                        LogLevel::INFO, DeviceType::MAIN);
        }
    }

    Logger::Log("getClientSettings | Current Config:", LogLevel::INFO, DeviceType::MAIN);
    for (const auto &pair : config)
    {
        Logger::Log("getClientSettings | " + pair.first + " = " + pair.second, LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConfigureRecovery:" + QString::fromStdString(pair.first) + ":" + QString::fromStdString(pair.second));

        if (pair.first == "Coordinates")
        {
            QStringList coordinates = QString::fromStdString(pair.second).split(",");
            if (coordinates.size() >= 2)
            {
                observatorylongitude = coordinates[1].toDouble();
                observatorylatitude = coordinates[0].toDouble();
            }
        }

    }
    Logger::Log("getClientSettings finish!", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::setClientSettings(QString ConfigName, QString ConfigValue)
{

    Logger::Log("setClientSettings start ...", LogLevel::INFO, DeviceType::MAIN);
    std::string fileName = "config/config.ini";

    std::unordered_map<std::string, std::string> config;

    config[ConfigName.toStdString()] = ConfigValue.toStdString();

    Logger::Log("setClientSettings | Save Client Setting:" + ConfigName.toStdString() + " = " + ConfigValue.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    Tools::saveClientSettings(fileName, config);
    if (ConfigName == "FocalLength" || ConfigName == "MainCameraFocalLength")
    {
        glFocalLength = ConfigValue.toDouble();
        if (ConfigName == "FocalLength")
        {
            std::unordered_map<std::string, std::string> migrateConfig;
            migrateConfig["MainCameraFocalLength"] = ConfigValue.toStdString();
            Tools::saveClientSettings(fileName, migrateConfig);
        }
    }
    if (ConfigName == "GuiderFocalLength")
    {
        guiderFocalLengthMm = ConfigValue.toDouble();
    }
    if (ConfigName == "GuiderExposureMs")
    {
        bool ok = false;
        const int exposureMs = ConfigValue.trimmed().toInt(&ok);
        if (ok && exposureMs > 0)
        {
            guiderExpMs = exposureMs;
            auto p = guiderParamsCache;
            p.exposureMs = exposureMs;
            guiderParamsCache = p;
            postGuiderCore(guiderCore, [p](GuiderCore *core) { core->setParams(p); });
        }
    }
    Logger::Log("setClientSettings finish!", LogLevel::INFO, DeviceType::MAIN);
}





bool MainWindow::isFileExists(const QString &filePath)
{
    QFile file(filePath);
    if (file.exists())
    {
        Logger::Log("isFileExists | file exists: " + filePath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        Logger::Log("isFileExists | file does not exist: " + filePath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    }
    return file.exists();
}



int MainWindow::MoveFileToUSB()
{
    qDebug("MoveFileToUSB");
}











// 解析字符串

// 返回 U 盘剩余内存

// 获取文件系统挂载模式

// 将文件系统挂载模式更改为读写模式



// 获取U盘挂载点（统一函数，供其他函数复用）





void MainWindow::SendDebugToVueClient(const QString &msg)
{
    emit wsThread->sendMessageToClient("SendDebugMessage|" + msg);
}

void MainWindow::customMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context);

    if (!instance)
    {
        return;
    }

    QString currentTime = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    QString typeStr;
    switch (type)
    {
    case QtDebugMsg:
        typeStr = "Debug";
        break;
    case QtInfoMsg:
        typeStr = "Info";
        break;
    case QtWarningMsg:
        typeStr = "Warning";
        break;
    case QtCriticalMsg:
        typeStr = "Critical";
        break;
    default:
        typeStr = "Unknown";
    }

    // 统一格式化日志消息
    QString logMessage = QString("[Server] %1 | %2 | %3").arg(currentTime).arg(typeStr).arg(msg);

    // // 将日志消息添加到缓存
    // instance->logCache.push_back(logMessage);

    // 统一输出到标准错误，无论日志类型
    fprintf(stderr, "%s\n", logMessage.toLocal8Bit().constData());

    // 发送到客户端的Vue应用，包括日志类型和消息
    instance->SendDebugToVueClient(typeStr + "|" + msg);
}




// 串口通信列表




bool MainWindow::areFilesInSameDirectory(const QString &path1, const QString &path2)
{
    QFileInfo fileInfo1(path1);
    QFileInfo fileInfo2(path2);

    // 获取两个文件的目录路径
    QString dir1 = fileInfo1.absolutePath();
    QString dir2 = fileInfo2.absolutePath();

    // 比较目录路径是否相同
    return (dir1 == dir2);
}


static int scoreByIdLinkForType(const QString &fileNameLower, const QString &driverType)
{
    // 简单关键字打分，匹配越多分越高
    int score = 0;
    if (driverType == "Focuser")
    {
        if (fileNameLower.contains("gigadevice")) score += 2;
        if (fileNameLower.contains("gd32")) score += 2;
        if (fileNameLower.contains("cdc_acm")) score += 1;
        if (fileNameLower.contains("acm")) score += 1;
    }
    else if (driverType == "Mount")
    {
        if (fileNameLower.contains("1a86")) score += 2;          // WCH/CH34x VID
        if (fileNameLower.contains("usb_serial")) score += 2;    // CH34x 常见 by-id
        if (fileNameLower.contains("ch34")) score += 2;
        if (fileNameLower.contains("wch")) score += 1;
        if (fileNameLower.contains("ttyusb")) score += 1;        // 兜底弱信号
    }
    return score;
}






void MainWindow::bin_image(double *input, long width, long height, double *output, long *out_w, long *out_h)
{
    *out_w = width / BIN_SIZE;
    *out_h = height / BIN_SIZE;

    for (long y = 0; y < *out_h; y++)
    {
        for (long x = 0; x < *out_w; x++)
        {
            double sum = 0.0;
            for (int dy = 0; dy < BIN_SIZE; dy++)
            {
                for (int dx = 0; dx < BIN_SIZE; dx++)
                {
                    long ix = x * BIN_SIZE + dx;
                    long iy = y * BIN_SIZE + dy;
                    sum += input[iy * width + ix];
                }
            }
            output[y * (*out_w) + x] = sum / (BIN_SIZE * BIN_SIZE);
        }
    }
}

void MainWindow::process_hdu(fitsfile *infptr, fitsfile *outfptr, int hdunum, int *status)
{
    fits_movabs_hdu(infptr, hdunum, NULL, status);

    int bitpix, naxis;
    long naxes[2] = {1, 1};
    fits_get_img_param(infptr, 2, &bitpix, &naxis, naxes, status);

    if (naxis != 2)
    {
        printf("HDU %d skipped (not 2D image).\n", hdunum);
        return;
    }

    long width = naxes[0], height = naxes[1];
    long npixels = width * height;
    double *img = (double *)malloc(npixels * sizeof(double));
    if (!img)
    {
        printf("Memory error.\n");
        exit(1);
    }

    long fpixel[2] = {1, 1};
    fits_read_pix(infptr, TDOUBLE, fpixel, npixels, NULL, img, NULL, status);

    long out_w, out_h;
    long dims[2] = {out_w, out_h};
    long out_pixels = (width / BIN_SIZE) * (height / BIN_SIZE);
    double *binned = (double *)malloc(out_pixels * sizeof(double));
    bin_image(img, width, height, binned, &out_w, &out_h);

    // 创建输出图像
    fits_create_img(outfptr, DOUBLE_IMG, 2, dims, status);
    fits_write_img(outfptr, TDOUBLE, 1, out_w * out_h, binned, status);

    free(img);
    free(binned);
}
int MainWindow::process_fixed()
{
    const char *infile = "/dev/shm/ccd_simulator_original.fits"; // 输入文件路径
    const char *outfile = "!/dev/shm/ccd_simulator_binned.fits"; // 输出文件路径
    // const char *outfile = "!merged_output.fits";  // 带 '!' 前缀，自动覆盖

    fitsfile *infptr = NULL, *outfptr = NULL;
    int status = 0, hdunum = 0, hdutype = 0;

    fits_open_file(&infptr, infile, READONLY, &status);
    if (status)
    {
        fits_report_error(stderr, status);
        return status;
    }

    fits_create_file(&outfptr, outfile, &status);
    if (status)
    {
        fits_report_error(stderr, status);
        fits_close_file(infptr, &status);
        return status;
    }

    fits_get_num_hdus(infptr, &hdunum, &status);
    for (int i = 1; i <= hdunum && status == 0; i++)
    {
        fits_movabs_hdu(infptr, i, &hdutype, &status);
        if (hdutype == IMAGE_HDU)
        {
            process_hdu(infptr, outfptr, i, &status);
        }
        else
        {
            fits_copy_hdu(infptr, outfptr, 0, &status);
        }
    }

    fits_close_file(infptr, &status);
    fits_close_file(outfptr, &status);

    if (status)
    {
        fits_report_error(stderr, status);
        return status;
    }

    printf("合并覆盖完成：ccd_simulator_binned.fits\n");
    return 0;
}


void MainWindow::sendRoiInfo()
{
    Logger::Log("==========================================", LogLevel::INFO, DeviceType::FOCUSER);
    for (auto it = roiAndFocuserInfo.begin(); it != roiAndFocuserInfo.end(); ++it)
    {
        Logger::Log("roiAndFocuserInfo | Key:" + it->first + " Value:" + std::to_string(it->second), LogLevel::INFO, DeviceType::FOCUSER);
    }
    Logger::Log("==========================================", LogLevel::INFO, DeviceType::FOCUSER);
    // 检查并获取参数，如果不存在则使用默认值
    double boxSideLength = roiAndFocuserInfo.count("BoxSideLength") ? roiAndFocuserInfo["BoxSideLength"] : 300;
    double roi_x = roiAndFocuserInfo.count("ROI_x") ? roiAndFocuserInfo["ROI_x"] : 1;
    double roi_y = roiAndFocuserInfo.count("ROI_y") ? roiAndFocuserInfo["ROI_y"] : 1;
    double visibleX = roiAndFocuserInfo.count("VisibleX") ? roiAndFocuserInfo["VisibleX"] : 0;
    double visibleY = roiAndFocuserInfo.count("VisibleY") ? roiAndFocuserInfo["VisibleY"] : 0;
    double scale = roiAndFocuserInfo.count("scale") ? roiAndFocuserInfo["scale"] : 1;
    double selectStarX = roiAndFocuserInfo.count("SelectStarX") ? roiAndFocuserInfo["SelectStarX"] : -1;
    double selectStarY = roiAndFocuserInfo.count("SelectStarY") ? roiAndFocuserInfo["SelectStarY"] : -1;
    const QPointF snapped = snapRoiOriginToBayerSafePhase(roi_x, roi_y,
                                                          static_cast<int>(std::lround(boxSideLength)),
                                                          static_cast<int>(std::lround(boxSideLength)));
    roi_x = snapped.x();
    roi_y = snapped.y();
    roiAndFocuserInfo["ROI_x"] = roi_x;
    roiAndFocuserInfo["ROI_y"] = roi_y;

    Logger::Log("sendRoiInfo | 发送参数 roi_x:" + std::to_string(roi_x) + " roi_y:" + std::to_string(roi_y) + " boxSideLength:" + std::to_string(boxSideLength) + " visibleX:" + std::to_string(visibleX) + " visibleY:" + std::to_string(visibleY) + " scale:" + std::to_string(scale) + " selectStarX:" + std::to_string(selectStarX) + " selectStarY:" + std::to_string(selectStarY), LogLevel::INFO, DeviceType::FOCUSER);

    // 发送参数
    emit wsThread->sendMessageToClient("SetRedBoxState:" + QString::number(boxSideLength) + ":" + QString::number(roi_x) + ":" + QString::number(roi_y));
    emit wsThread->sendMessageToClient("SetVisibleArea:" + QString::number(visibleX) + ":" + QString::number(visibleY) + ":" + QString::number(scale));
    emit wsThread->sendMessageToClient("SetSelectStars:" + QString::number(selectStarX) + ":" + QString::number(selectStarY));
}
void MainWindow::updateCPUInfo()
{
    // 获取CPU温度和使用率
    QProcess process;
    // 在树莓派上，可以通过读取 /sys/class/thermal/thermal_zone0/temp 文件来获取 CPU 温度
    process.start("cat", QStringList() << "/sys/class/thermal/thermal_zone0/temp");
    process.waitForFinished();
    QString output = process.readAllStandardOutput();
    float cpuTemp = output.toFloat() / 1000; // 转换为摄氏度
    if (process.error() != QProcess::UnknownError)
    {
        cpuTemp = std::numeric_limits<float>::quiet_NaN(); // 如果获取失败，设置为 NaN
    }

    // 在树莓派上，可以通过运行 'top' 命令并解析输出来获取 CPU 使用率
    process.start("sh", QStringList() << "-c" << "top -b -n1 | grep 'Cpu(s)' | awk '{print $2}' | cut -c 1-4");
    process.waitForFinished();
    output = process.readAllStandardOutput();
    QStringList cpuUsages = output.split("\n");
    float cpuUsage = 0;
    if (cpuUsages.size() > 0)
    {
        cpuUsage = cpuUsages[0].toDouble();
    }
    if (process.error() != QProcess::UnknownError)
    {
        cpuUsage = std::numeric_limits<float>::quiet_NaN(); // 如果获取失败，设置为 NaN
    }

    // Logger::Log("updateCPUInfo | CPU Temp: " + std::to_string(cpuTemp) + ", CPU Usage: " + std::to_string(cpuUsage), LogLevel::DEBUG, DeviceType::MAIN);
    emit wsThread->sendMessageToClient("updateCPUInfo:" + QString::number(cpuTemp) + ":" + QString::number(cpuUsage));
}

void MainWindow::getMainCameraParameters()
{
    Logger::Log("getMainCameraParameters start ...", LogLevel::DEBUG, DeviceType::MAIN);
    QMap<QString, QString> parameters = Tools::readParameters("MainCamera");
    QString order = "setMainCameraParameters";
    bool hasTileBuildMode = false;
    bool hasTileLevelMode = false;
    bool hasImageCfa = false;
    bool hasRoiCalcMode = false;
    for (auto it = parameters.begin(); it != parameters.end(); ++it)
    {
        Logger::Log("getMainCameraParameters | " + it.key().toStdString() + ":" + it.value().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        if (it.key() == "ImageCFA") {
            hasImageCfa = true;
            const QString normalizedCfa = normalizeCfaPattern(it.value());
            MainCameraCFA = (normalizedCfa == "NULL" || normalizedCfa == "MONO") ? QString() : normalizedCfa;
            it.value() = MainCameraCFA.isEmpty() ? QStringLiteral("null") : MainCameraCFA;
        }
        if (it.key() == "ROICalcMode") {
            hasRoiCalcMode = true;
            const QString mode = it.value().trimmed().toLower();
            roiUseSelfCalcParams = (mode == "roi" || mode == "self");
            it.value() = roiUseSelfCalcParams ? QStringLiteral("roi") : QStringLiteral("full");
        }
        if (it.key() == "Save Folder" ) {
            QString oldSaveFolder = it.value();
            // 兼容旧的"default"，转换为"local"
            if (oldSaveFolder == "default") {
                oldSaveFolder = "local";
                it.value() = "local";
            }

            if (oldSaveFolder == "local") {
                ImageSaveBaseDirectory = QString::fromStdString(ImageSaveBasePath);
                saveMode = "local";
            } else if (usbMountPointsMap.contains(oldSaveFolder)) {
                ImageSaveBaseDirectory = usbMountPointsMap[oldSaveFolder] + "/QUARCS_ImageSave";
                saveMode = oldSaveFolder;
            } else {
                // U盘不存在，回退到本地
                it.value() = "local";
                ImageSaveBaseDirectory = QString::fromStdString(ImageSaveBasePath);
                saveMode = "local";
                Logger::Log("LoadParameter | USB '" + oldSaveFolder.toStdString() + "' not found, using local", LogLevel::WARNING, DeviceType::MAIN);
            }
        }
        if (it.key() == "Tile Build Mode") {
            if (it.value() == "merged_single_level") {
                Logger::Log("LoadParameter | Tile Build Mode 'merged_single_level' is deprecated, forcing pyramid",
                            LogLevel::WARNING, DeviceType::MAIN);
            }
            tileBuildMode = QStringLiteral("pyramid");
            it.value() = tileBuildMode;
            hasTileBuildMode = true;
        }
        if (it.key() == "Tile Level Mode") {
            const QString requested = it.value().trimmed().toLower();
            tileLevelMode = (requested == QStringLiteral("minmax")) ? QStringLiteral("minmax") : QStringLiteral("full");
            it.value() = tileLevelMode;
            hasTileLevelMode = true;
        }
        order += ":" + it.key() + ":" + it.value();
        if (it.key() == "RedBoxSize") {
            BoxSideLength = it.value().toInt();
            roiAndFocuserInfo["BoxSideLength"] = BoxSideLength;
        }
        if (it.key() == "ROI_x") roiAndFocuserInfo["ROI_x"] = it.value().toDouble();
        if (it.key() == "ROI_y") roiAndFocuserInfo["ROI_y"] = it.value().toDouble();
        if (it.key() == "AutoSave") {
            mainCameraAutoSave = (it.value() == "true");
            // Logger::Log("/*/*/*/*/*/*getMainCameraParameters | AutoSave: " + std::to_string(mainCameraAutoSave), LogLevel::DEBUG, DeviceType::MAIN);
        }
        if (it.key() == "SaveFailedParse") {
            mainCameraSaveFailedParse = (it.value() == "true");
        }
        if (it.key() == "Temperature") {
            CameraTemperature = it.value().toDouble();
        }
        if (it.key() == "Gain") {
            CameraGain = it.value().toInt();
        }
        if (it.key() == "Offset") {
            ImageOffset = it.value().toDouble();
        }
        if (it.key() == "USB Traffic") {
            glUsbTrafficValue = it.value().toInt();
        }
    }
    if (!hasTileBuildMode) {
        tileBuildMode = QStringLiteral("pyramid");
        order += ":Tile Build Mode:" + tileBuildMode;
    }
    if (!hasTileLevelMode) {
        tileLevelMode = QStringLiteral("full");
        order += ":Tile Level Mode:" + tileLevelMode;
    }
    if (!hasRoiCalcMode) {
        roiUseSelfCalcParams = false;
        order += ":ROICalcMode:full";
    }
    if (!hasImageCfa) {
        MainCameraCFA = normalizeCfaPattern(MainCameraCFA);
        if (MainCameraCFA == "NULL" || MainCameraCFA == "MONO") {
            MainCameraCFA.clear();
        }
    }
    Logger::Log("getMainCameraParameters finish!", LogLevel::DEBUG, DeviceType::MAIN);
    emit wsThread->sendMessageToClient(order);

    emit wsThread->sendMessageToClient("MainCameraCFA:" + (MainCameraCFA.isEmpty() ? QStringLiteral("null") : MainCameraCFA));
    emit wsThread->sendMessageToClient("MainCameraCFASource:SAVED");
}





void MainWindow::getLastSelectDevice()
{
    SystemDeviceList newSystemdevicelist = Tools::readSystemDeviceList();

    // 检查是否有历史设备配置记录
    bool hasHistoryConfig = false;
    for (int i = 0; i < newSystemdevicelist.system_devices.size(); i++)
    {
        if (!newSystemdevicelist.system_devices[i].DriverIndiName.isEmpty() &&
            !newSystemdevicelist.system_devices[i].Description.isEmpty())
        {
            hasHistoryConfig = true;
            break;
        }
    }

    if (!hasHistoryConfig)
    {
        Logger::Log("No historical connection records found", LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    // 只有当未连接设备时才触发设备选择
    if (ConnectedDevices.size() == 0)
    {
        systemdevicelist = newSystemdevicelist;
        loadSelectedDriverList();
        Logger::Log("Last Connected Device Has Send", LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        Logger::Log("Devices already connected, skip loading last selection", LogLevel::INFO, DeviceType::MAIN);
    }
}

// [停用 2026-04-14] 旧自动电调校准实现：保留函数体用于历史回溯，当前不再通过命令入口触发。
void MainWindow::getCheckBoxSpace()
{
    // 计算应用所在分区的可用空间，避免与实际存储分区不一致
    QString path = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
    path = QDir::rootPath();
#endif
    QFileInfo fi(path);
    quint64 freeBytes = 0;
    if (fi.exists())
    {
        QStorageInfo storage(path);
        freeBytes = static_cast<quint64>(storage.bytesAvailable());
    }
    // 发送到前端：Box_Space:<bytes>
    if (wsThread)
    {
        emit wsThread->sendMessageToClient("Box_Space:" + QString::number(freeBytes));
    }
}

void MainWindow::clearLogs()
{
    // 按 Logger::Initialize 约定：运行目录下 logs/MAIN.log 等
    const QString logDirPath = QDir::currentPath() + "/logs";
    QDir logDir(logDirPath);
    if (logDir.exists())
    {
        QFileInfoList entries = logDir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
        for (const QFileInfo &fi : entries)
        {
            // 清空内容而不删除文件
            QFile f(fi.absoluteFilePath());
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            {
                f.close();
            }
        }
    }
    if (wsThread) emit wsThread->sendMessageToClient("ClearLogs:Success");
}

void MainWindow::clearBoxCache(bool clearCache, bool clearUpdatePack, bool clearBackup)
{
    auto clearDirContents = [](const QString &dirPath)
    {
        if (dirPath.isEmpty()) return;
        QDir dir(dirPath);
        if (!dir.exists()) return;
        // 删除目录下的所有条目（文件/链接/目录），保留顶层目录本身
        QFileInfoList entries = dir.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden);
        for (const QFileInfo &fi : entries)
        {
            if (fi.isSymLink() || fi.isFile())
            {
                QFile::remove(fi.absoluteFilePath());
            }
            else if (fi.isDir())
            {
                QDir sub(fi.absoluteFilePath());
                sub.removeRecursively();
            }
        }
    };
    auto clearFileIfExists = [](const QString &filePath)
    {
        if (filePath.isEmpty()) return;
        QFileInfo fi(filePath);
        if (!fi.exists() || !fi.isFile()) return;
        QFile::remove(filePath);
    };

    // 1. 清理系统/应用缓存与回收站
    if (clearCache)
    {
        QStringList caches;
        // 用户主目录缓存根（比单纯的 CacheLocation 更全面）
        caches << (QDir::homePath() + "/.cache");
        caches << QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
               << QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
               << QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        // 额外补充持久临时目录与常见开发工具缓存
        caches << "/var/tmp";
        caches << (QDir::homePath() + "/.cursor-server/data/CachedExtensionVSIXs");
        // XDG 垃圾箱（当前用户）常规路径与 XDG_DATA_HOME 路径
        const QString trashBase = QDir::homePath() + "/.local/share/Trash";
        caches << (trashBase + "/files") << (trashBase + "/info") << (trashBase + "/expunged");
        const QString xdgDataHome = qEnvironmentVariableIsSet("XDG_DATA_HOME")
                                    ? QString::fromLocal8Bit(qgetenv("XDG_DATA_HOME"))
                                    : (QDir::homePath() + "/.local/share");
        caches << (xdgDataHome + "/Trash/files")
               << (xdgDataHome + "/Trash/info")
               << (xdgDataHome + "/Trash/expunged");
        // 常见桌面环境的垃圾箱路径（可能存在）
        caches << (QDir::homePath() + "/.Trash")
               << (QDir::homePath() + "/.Trash-1000/files")
               << (QDir::homePath() + "/.Trash-1000/info");

        for (const QString &p : caches) clearDirContents(p);

        // 尝试调用 gio 清空垃圾箱（若环境支持）
        QProcess::execute("gio", QStringList() << "trash" << "--empty");

        // 清空可移动介质等可能挂载点的垃圾箱（.Trash-UID 或 .Trash）
        QString uidStr = QString::number(getuid());
        QStringList mountRoots;
        mountRoots << "/mnt" << "/media" << "/run/media";
        for (const QString &root : mountRoots)
        {
            QDir rootDir(root);
            if (!rootDir.exists()) continue;
            QFileInfoList subs = rootDir.entryInfoList(QDir::NoDotAndDotDot | QDir::Dirs);
            for (const QFileInfo &fi : subs)
            {
                const QString base = fi.absoluteFilePath();
                clearDirContents(base + "/.Trash-" + uidStr + "/files");
                clearDirContents(base + "/.Trash-" + uidStr + "/info");
                clearDirContents(base + "/.Trash-" + uidStr + "/expunged");
                clearDirContents(base + "/.Trash/files");
                clearDirContents(base + "/.Trash/info");
                clearDirContents(base + "/.Trash/expunged");
            }
        }

        // 清理 apt 缓存与包索引，避免更新缓存长期堆积。
        clearDirContents("/var/cache/apt");
        clearDirContents("/var/lib/apt/lists");

        // 清掉常见锁文件，避免目录已清空但索引残留。
        clearFileIfExists("/var/lib/apt/lists/lock");
        clearFileIfExists("/var/cache/apt/archives/lock");

        // 限制 systemd journal 体积；失败时静默跳过，避免无 sudo 能力时中断清理。
        runSudoSync("/usr/bin/journalctl", {"--vacuum-size=100M"}, 15000);
    }

    // 2. 可选：清理更新包目录
    if (clearUpdatePack)
    {
        clearDirContents("/var/www/update_pack");
    }

    // 3. 可选：清理备份目录
    if (clearBackup)
    {
        clearDirContents("/home/quarcs/workspace/QUARCS/backup");
    }

    if (wsThread) emit wsThread->sendMessageToClient("ClearBoxCache:Success");
}
