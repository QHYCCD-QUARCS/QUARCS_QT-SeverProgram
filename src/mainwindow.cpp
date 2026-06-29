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

    // ========================= 图像保存根目录（本地） =========================
    // 默认：系统家目录下 ~/images
    // 可通过环境变量 QUARCS_IMAGE_SAVE_ROOT 覆盖
    // 注意：这里必须在 Logger::Initialize() 之后再记录日志，避免未初始化导致崩溃
    {
        QString base = QDir::cleanPath(QDir::homePath() + "/images");
        if (const char *env = std::getenv("QUARCS_IMAGE_SAVE_ROOT"))
        {
            const std::string v(env);
            if (!v.empty())
            {
                base = QDir::cleanPath(QString::fromStdString(v));
                Logger::Log("MainWindow | QUARCS_IMAGE_SAVE_ROOT=" + v, LogLevel::INFO, DeviceType::MAIN);
            }
        }
        ImageSaveBasePath = base.toStdString();
        ImageSaveBaseDirectory = base;
        Logger::Log("MainWindow | ImageSaveBasePath=" + ImageSaveBasePath, LogLevel::INFO, DeviceType::MAIN);
    }

    // 允许通过环境变量覆盖 /img 的真实根目录（Web 静态 img 目录）
    // 例如：
    //   export QUARCS_WEB_IMG_ROOT="/home/quarcs/.../apps/web-frontend/dist/img/"
    //   export QUARCS_WEB_IMG_ROOT="/var/www/html/img/"
    if (const char *env = std::getenv("QUARCS_WEB_IMG_ROOT"))
    {
        const std::string v(env);
        if (!v.empty())
        {
            vueImagePath = v;
            Logger::Log("MainWindow | QUARCS_WEB_IMG_ROOT=" + vueImagePath, LogLevel::INFO, DeviceType::MAIN);
        }
    }
    // 规范化末尾斜杠
    if (!vueImagePath.empty() && vueImagePath.back() != '/')
        vueImagePath.push_back('/');

    // ========================= /img/capture-tiles 映射自检（树莓派部署） =========================
    // 前端会通过 HTTP 请求 /img/capture-tiles/<session>/<z>/<x>/<y>.bin
    // 而后端瓦片默认写入 tmpfs：/dev/shm/capture-tiles/...
    // 若 Web 静态根目录为 /var/www/html/img/ 且没有 nginx alias，则需要建立：
    //   /var/www/html/img/capture-tiles  ->  /dev/shm/capture-tiles
    // 这样无需额外改 nginx，就能直接访问到 tmpfs 瓦片文件。
    {
        const QString imgRoot = QString::fromStdString(vueImagePath);              // e.g. /var/www/html/img/
        QString target = QString::fromStdString(tilePyramidPath);                 // e.g. /dev/shm/capture-tiles/
        if (!target.endsWith('/')) target += '/';
        const QString linkPath = imgRoot + QStringLiteral("capture-tiles");       // e.g. /var/www/html/img/capture-tiles

        // 1) 确保目标目录存在（tmpfs）
        if (!target.isEmpty()) {
            QDir tdir(target);
            if (!tdir.exists()) {
                if (QDir().mkpath(target)) {
                    Logger::Log("Tile tiles root created: " + target.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                } else {
                    Logger::Log("Failed to create tile tiles root: " + target.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                }
            }
        }

        // 2) 建立/校验符号链接
        if (!imgRoot.isEmpty() && !target.isEmpty()) {
            QFileInfo fi(linkPath);
            if (fi.exists() || fi.isSymLink()) {
                if (fi.isSymLink()) {
                    const QString cur = fi.symLinkTarget();
                    // 不强制完全相等（可能存在无尾斜杠），做一次归一化比较
                    QString curNorm = cur;
                    if (!curNorm.endsWith('/')) curNorm += '/';
                    if (curNorm == target) {
                        Logger::Log("Tile mapping OK: " + linkPath.toStdString() + " -> " + target.toStdString(),
                                    LogLevel::INFO, DeviceType::MAIN);
                    } else {
                        // 尝试修复为正确目标（-sfn：覆盖旧链接）
                        const int rc = QProcess::execute(QStringLiteral("ln"),
                                                        QStringList() << QStringLiteral("-sfn") << target << linkPath);
                        if (rc == 0) {
                            Logger::Log("Tile mapping fixed: " + linkPath.toStdString() + " -> " + target.toStdString(),
                                        LogLevel::INFO, DeviceType::MAIN);
                        } else {
                            Logger::Log("Tile mapping exists but points elsewhere: " + linkPath.toStdString() +
                                            " -> " + cur.toStdString() + " (expected " + target.toStdString() + "), rc=" + std::to_string(rc),
                                        LogLevel::WARNING, DeviceType::MAIN);
                        }
                    }
                } else {
                    // 已存在非 symlink 的目录/文件：不做破坏性操作，只提示
                    Logger::Log("Tile mapping not created because path already exists (not symlink): " + linkPath.toStdString() +
                                    " . Consider removing it or configure nginx alias: /img/capture-tiles/ -> " + target.toStdString(),
                                LogLevel::WARNING, DeviceType::MAIN);
                }
            } else {
                // linkPath 不存在：创建符号链接
                QDir().mkpath(QFileInfo(linkPath).absolutePath());
                const int rc = QProcess::execute(QStringLiteral("ln"),
                                                QStringList() << QStringLiteral("-sfn") << target << linkPath);
                if (rc == 0) {
                    Logger::Log("Tile mapping created: " + linkPath.toStdString() + " -> " + target.toStdString(),
                                LogLevel::INFO, DeviceType::MAIN);
                } else {
                    Logger::Log("Failed to create tile mapping: " + linkPath.toStdString() + " -> " + target.toStdString() +
                                    ", rc=" + std::to_string(rc) + ". Permission issue? (need write access to " + imgRoot.toStdString() + ")",
                                LogLevel::WARNING, DeviceType::MAIN);
                }
            }
        }
    }

    // ========================= /img/downloads 映射自检（下载软链目录） =========================
    // 前端会通过 HTTP 请求 /img/downloads/<token>/<type>/<relPath>
    // 后端会在 vueImagePath/downloads/<token>/... 下生成一批软链接（或兜底复制）。
    // 这里将 <imgRoot>/downloads 映射到 QT 的“图像保存根目录”下的固定路径，确保一致性：
    //   <imgRoot>/downloads  ->  <ImageSaveBasePath>/downloads/
    // 注意：ImageSaveBasePath 可能是相对路径，因此这里会转换为当前工作目录下的绝对路径后再建链接。
    // 如需指定其它固定目录，可通过环境变量 QUARCS_DOWNLOADS_ROOT 覆盖：
    //   export QUARCS_DOWNLOADS_ROOT="/path/to/downloads/"
    {
        const QString imgRoot = QString::fromStdString(vueImagePath);              // e.g. /var/www/html/img/
        // 默认：跟随 QT 图像保存根目录（固定落盘位置）
        QString base = QString::fromStdString(ImageSaveBasePath);
        if (!QDir::isAbsolutePath(base))
            base = QDir::cleanPath(QDir::current().absoluteFilePath(base));
        QString target = QDir::cleanPath(base + "/downloads/");
        if (const char *env = std::getenv("QUARCS_DOWNLOADS_ROOT"))
        {
            const std::string v(env);
            if (!v.empty())
            {
                target = QString::fromStdString(v);
                Logger::Log("MainWindow | QUARCS_DOWNLOADS_ROOT=" + v, LogLevel::INFO, DeviceType::MAIN);
            }
        }
        if (!target.endsWith('/')) target += '/';
        const QString linkPath = imgRoot + QStringLiteral("downloads");            // e.g. /var/www/html/img/downloads

        // 1) 确保目标目录存在
        if (!target.isEmpty())
        {
            QDir tdir(target);
            if (!tdir.exists())
            {
                if (QDir().mkpath(target))
                {
                    Logger::Log("Downloads root created: " + target.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                }
                else
                {
                    Logger::Log("Failed to create downloads root: " + target.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                }
            }
        }

        // 2) 建立/校验符号链接
        if (!imgRoot.isEmpty() && !target.isEmpty())
        {
            QFileInfo fi(linkPath);
            if (fi.exists() || fi.isSymLink())
            {
                if (fi.isSymLink())
                {
                    const QString cur = fi.symLinkTarget();
                    QString curNorm = cur;
                    if (!curNorm.endsWith('/')) curNorm += '/';
                    if (curNorm == target)
                    {
                        Logger::Log("Downloads mapping OK: " + linkPath.toStdString() + " -> " + target.toStdString(),
                                    LogLevel::INFO, DeviceType::MAIN);
                    }
                    else
                    {
                        const int rc = QProcess::execute(QStringLiteral("ln"),
                                                        QStringList() << QStringLiteral("-sfn") << target << linkPath);
                        if (rc == 0)
                        {
                            Logger::Log("Downloads mapping fixed: " + linkPath.toStdString() + " -> " + target.toStdString(),
                                        LogLevel::INFO, DeviceType::MAIN);
                        }
                        else
                        {
                            Logger::Log("Downloads mapping exists but points elsewhere: " + linkPath.toStdString() +
                                            " -> " + cur.toStdString() + " (expected " + target.toStdString() + "), rc=" + std::to_string(rc),
                                        LogLevel::WARNING, DeviceType::MAIN);
                        }
                    }
                }
                else
                {
                    // 已存在非 symlink 的目录/文件：不做破坏性操作，只提示
                    Logger::Log("Downloads mapping not created because path already exists (not symlink): " + linkPath.toStdString() +
                                    " . Consider removing it or configure web server to serve it.",
                                LogLevel::WARNING, DeviceType::MAIN);
                }
            }
            else
            {
                QDir().mkpath(QFileInfo(linkPath).absolutePath());
                const int rc = QProcess::execute(QStringLiteral("ln"),
                                                QStringList() << QStringLiteral("-sfn") << target << linkPath);
                if (rc == 0)
                {
                    Logger::Log("Downloads mapping created: " + linkPath.toStdString() + " -> " + target.toStdString(),
                                LogLevel::INFO, DeviceType::MAIN);
                }
                else
                {
                    Logger::Log("Failed to create downloads mapping: " + linkPath.toStdString() + " -> " + target.toStdString() +
                                    ", rc=" + std::to_string(rc) + ". Permission issue? (need write access to " + imgRoot.toStdString() + ")",
                                LogLevel::WARNING, DeviceType::MAIN);
                }
            }
        }
    }

    wsThread = new WebSocketThread(websockethttpUrl, websockethttpsUrl);
    connect(wsThread, &WebSocketThread::receivedMessage, this, &MainWindow::onMessageReceived);
    wsThread->start();
    Logger::wsThread = wsThread;

    // 记住当前实例
    instance = this;

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

    // 电调控制初始化
    focusMoveTimer = new QTimer(this);
    connect(focusMoveTimer, &QTimer::timeout, this, &MainWindow::HandleFocuserMovementDataPeriodically);

    // SDK 曝光定时器初始化
    sdkExposureTimer = new QTimer(this);
    sdkExposureTimer->setSingleShot(false); // 允许多次触发
    connect(sdkExposureTimer, &QTimer::timeout, this, &MainWindow::onSdkExposureTimerTimeout);

    // SDK 主相机 Live 循环取帧定时器初始化（仅 QHYCCD SDK 使用）
    sdkMainLiveTimer = new QTimer(this);
    sdkMainLiveTimer->setSingleShot(false);
    // 默认 33ms (~30fps)。实际出帧速度由相机曝光/传输决定。
    sdkMainLiveTimer->setInterval(33);
    connect(sdkMainLiveTimer, &QTimer::timeout, this, &MainWindow::onSdkMainLiveTimerTimeout);

    // SDK 主相机 Live 后处理定时器（主线程）：从“最新帧邮箱”取最新帧做 FITS/PNG/瓦片
    // 说明：取帧与处理彻底解耦，处理慢只会丢中间帧，不影响 SDK 拉帧与 FPS 统计
    sdkMainLiveProcessTimer = new QTimer(this);
    sdkMainLiveProcessTimer->setSingleShot(false);
    // 频率不用很高；真正的限帧由 sdkMainLiveMaxProcessFps 控制
    sdkMainLiveProcessTimer->setInterval(50);
    connect(sdkMainLiveProcessTimer, &QTimer::timeout, this, &MainWindow::onSdkMainLiveProcessTimerTimeout);

    // SDK 导星曝光定时器初始化（独立于主相机 SDK 曝光）
    sdkGuiderExposureTimer = new QTimer(this);
    sdkGuiderExposureTimer->setSingleShot(false);
    connect(sdkGuiderExposureTimer, &QTimer::timeout, this, &MainWindow::onSdkGuiderExposureTimerTimeout);

    // SDK 串行执行线程（所有阻塞式 SDK 调用都应投递到对应线程）
    // 每个相机独立通道，避免某个设备的阻塞读帧拖住其它相机。
    sdkCamExec = std::make_unique<SdkSerialExecutor>(QStringLiteral("SdkCamWorker"));
    sdkMainCamExec = std::make_unique<SdkSerialExecutor>(QStringLiteral("SdkMainCameraWorker"));
    sdkGuiderCamExec = std::make_unique<SdkSerialExecutor>(QStringLiteral("SdkGuiderCameraWorker"));
    sdkPoleCamExec = std::make_unique<SdkSerialExecutor>(QStringLiteral("SdkPoleCameraWorker"));
    sdkFocuserExec = std::make_unique<SdkSerialExecutor>(QStringLiteral("SdkFocuserWorker"));

    emit wsThread->sendMessageToClient("ServerInitSuccess");
    Logger::Log("ServerInitSuccess", LogLevel::INFO, DeviceType::MAIN);

}

MainWindow::~MainWindow()
{
    // 停止可能触发 SDK 调用的定时器，避免析构期间还有回调进来
    if (sdkExposureTimer)
        sdkExposureTimer->stop();
    if (sdkMainLiveTimer)
        sdkMainLiveTimer->stop();
    if (sdkMainLiveProcessTimer)
        sdkMainLiveProcessTimer->stop();
    if (sdkGuiderExposureTimer)
        sdkGuiderExposureTimer->stop();
    if (focusMoveTimer)
        focusMoveTimer->stop();

    if (guiderCoreThread && guiderCoreThread->isRunning() && guiderCore)
    {
        QMetaObject::invokeMethod(guiderCore, [core = guiderCore]() {
            core->stopGuiding();
            core->stopLoop();
        }, Qt::BlockingQueuedConnection);
        guiderCoreThread->quit();
        guiderCoreThread->wait(3000);
        guiderCore = nullptr;
    }

    // 析构兜底：若仍有 SDK 设备/句柄存在，统一关闭句柄并释放 SDK 全局资源
    //（例如用户直接关闭程序而未点“断开所有设备”）
    cleanupQhySdkPoolAndResource("MainWindow::~MainWindow", "All");

    // 停止 SDK 线程（会等待当前任务结束），避免后台任务回调访问已析构对象
    sdkPoleCamExec.reset();
    sdkGuiderCamExec.reset();
    sdkMainCamExec.reset();
    sdkCamExec.reset();
    sdkFocuserExec.reset();

    // 释放 Live 共享内存映射（确保 SDK 线程已停，避免并发写导致崩溃）
    cleanupSdkMainLiveShm();

    // 清理极轴校准对象
    if (polarAlignment != nullptr)
    {
        polarAlignment->stopPolarAlignment();
        delete polarAlignment;
        polarAlignment = nullptr;
    }

    system("pkill indiserver");
    system("rm -f /tmp/myFIFO");


    // 先断开 Logger -> WebSocket 的弱引用，避免析构过程中/析构后仍有后台线程写日志时触发野指针
    Logger::wsThread = nullptr;

    wsThread->quit();
    wsThread->wait();
    delete wsThread;
    wsThread = nullptr;

    // 清理静态实例
    instance = nullptr;
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

void MainWindow::initINDIServer()
{
    Logger::Log("initINDIServer ...", LogLevel::INFO, DeviceType::MAIN);
    system("pkill indiserver");
    system("rm -f /tmp/myFIFO");
    system("mkfifo /tmp/myFIFO");
    // FIFO 已重建：恢复 Tools 的 FIFO 熔断状态，允许后续 start/stop 写入
    Tools::resetIndiFifoState();
    glIndiServer = new QProcess();
    // glIndiServer->setReadChannel(QProcess::StandardOutput);

    // // 连接信号到槽函数
    // connect(glIndiServer, &QProcess::readyReadStandardOutput, this, &MainWindow::handleIndiServerOutput);
    // connect(glIndiServer, &QProcess::readyReadStandardError, this, &MainWindow::handleIndiServerError);

    // 已知部分驱动会在 verbose 模式下刷屏输出（例如 CCD_TEMPERATURE/CCD_COOLER_POWER 不存在）
    // 这里关闭 -v，并把输出重定向到文件，避免终端持续打印。
    glIndiServer->setProcessChannelMode(QProcess::MergedChannels);
    glIndiServer->setStandardOutputFile("/tmp/indiserver.log", QIODevice::Append);
    glIndiServer->setStandardErrorFile("/tmp/indiserver.log", QIODevice::Append);

    glIndiServer->setProgram("indiserver");
    glIndiServer->setArguments(QStringList() << "-f" << "/tmp/myFIFO" << "-p" << "7624");
    glIndiServer->start();
    Logger::Log("initINDIServer finish!", LogLevel::INFO, DeviceType::MAIN);
}

// void MainWindow::initINDIServer()
// {
//     system("pkill indiserver");
//     system("rm -f /tmp/myFIFO");
//     system("mkfifo /tmp/myFIFO");
//     glIndiServer = new QProcess();
//     glIndiServer->setReadChannel(QProcess::StandardOutput);
//     glIndiServer->start("indiserver -f /tmp/myFIFO -v -p 7624");
// }

// 槽函数：处理标准输出
void MainWindow::handleIndiServerOutput()
{
    QByteArray output = glIndiServer->readAllStandardOutput();
    Logger::Log("INDI Server Output: " + output.toStdString(), LogLevel::INFO, DeviceType::MAIN);
}

// 槽函数：处理标准错误
void MainWindow::handleIndiServerError()
{
    QByteArray error = glIndiServer->readAllStandardError();
    Logger::Log("INDI Server Error: " + error.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
}
int i = 1;
void MainWindow::initINDIClient()
{
    Logger::Log("initINDIClient ...", LogLevel::INFO, DeviceType::MAIN);
    indi_Client = new MyClient();
    indi_Client->setServer("localhost", 7624);
    indi_Client->setConnectionTimeout(3, 0);
    Logger::Log("setConnectionTimeout is 3 seconds!", LogLevel::INFO, DeviceType::MAIN);
    indi_Client->setImageReceivedCallback(
        [this](const std::string &filename, const std::string &devname)
        {
            if (dpGuider != NULL && dpGuider->getDeviceName() == devname)
            {
                guiderExposureInFlight = false;
                const QString fitsPath = QString::fromStdString(filename);

                if (guiderCore)
                {
                    QMetaObject::invokeMethod(guiderCore, "onNewFrame", Qt::QueuedConnection,
                                              Q_ARG(QString, fitsPath));
                }
                else
                {
                    PersistGuidingFits(fitsPath);
                    if (isGuiderLoopExp && guiderLoopTimer)
                    {
                        // INDI 图像回调可能运行在非 GUI 线程，必须回到 MainWindow 线程再启动 QTimer。
                        QMetaObject::invokeMethod(this, [this]() {
                            if (isGuiderLoopExp && guiderLoopTimer)
                                guiderLoopTimer->start(1);
                        }, Qt::QueuedConnection);
                    }
                }
                return;
            }

            // 曝光完成
            if (dpMainCamera != NULL)
            {
                if (dpMainCamera->getDeviceName() == devname)
                {
                    lastMainCaptureFitsPath = QString::fromStdString(filename);
                    glMainCameraStatu = "Displaying";
                    ShootStatus = "Completed";
                    if (autoFocuserIsROI && isAutoFocus)
                    {
                        saveFitsAsJPG(QString::fromStdString(filename), true);
                        Logger::Log("saveFitsAsJPG", LogLevel::DEBUG, DeviceType::MAIN);
                        autoFocus->setCaptureComplete(QString::fromStdString(filename));
                        return;
                    }
                    else if (!autoFocuserIsROI && isAutoFocus)
                    {

                        saveFitsAsPNG(QString::fromStdString(filename), true);

                        Logger::Log("saveFitsAsPNG", LogLevel::DEBUG, DeviceType::MAIN);
                        autoFocus->setCaptureComplete(QString::fromStdString(filename));
                        return;
                    }

                    // 检查：如果 isFocusLoopShooting 为 false 但 glIsFocusingLooping 刚被重置，
                    // 说明这可能是 ROI 停止时的残留帧，应该丢弃或按 ROI 处理
                    if (glIsFocusingLooping == false && !isFocusLoopShooting)
                    {
                        // 读取 FITS 获取图像尺寸，判断是否为 ROI 残留帧
                        fitsfile *fptr = nullptr;
                        int status = 0;
                        long naxes[2] = {0, 0};
                        int naxis = 0;

                        if (fits_open_file(&fptr, filename.c_str(), READONLY, &status) == 0)
                        {
                            fits_get_img_dim(fptr, &naxis, &status);
                            if (naxis == 2)
                            {
                                fits_get_img_size(fptr, 2, naxes, &status);
                            }
                            fits_close_file(fptr, &status);
                        }

                        // 如果图像尺寸远小于全分辨率（例如 < 80%），判定为 ROI 残留帧
                        bool isRoiFrame = false;
                        if (naxes[0] > 0 && naxes[1] > 0 && glMainCCDSizeX > 0 && glMainCCDSizeY > 0)
                        {
                            double widthRatio = (double)naxes[0] / glMainCCDSizeX;
                            double heightRatio = (double)naxes[1] / glMainCCDSizeY;
                            if (widthRatio < 0.8 || heightRatio < 0.8)
                            {
                                isRoiFrame = true;
                                Logger::Log("Image received after ROI stop, detected as ROI frame (" +
                                           std::to_string(naxes[0]) + "x" + std::to_string(naxes[1]) +
                                           " vs " + std::to_string(glMainCCDSizeX) + "x" + std::to_string(glMainCCDSizeY) +
                                           "), discarding...", LogLevel::WARNING, DeviceType::CAMERA);
                            }
                        }

                        // 如果是 ROI 残留帧，直接丢弃
                        if (isRoiFrame)
                        {
                            glMainCameraStatu = "IDLE";
                            Logger::Log("ROI residual frame discarded", LogLevel::INFO, DeviceType::CAMERA);
                            return;
                        }

                        // 否则按正常拍摄处理
                        emit wsThread->sendMessageToClient("ExposureCompleted");
                        emitCaptureTrace(QStringLiteral("backend_exposure_completed"), currentCaptureTraceStartedAtMs,
                                         QStringLiteral("source=fits_callback"));
                        Logger::Log("ExposureCompleted", LogLevel::INFO, DeviceType::CAMERA);
                        if (polarAlignment != nullptr)
                        {
                            if (polarAlignment->isRunning())
                            {
                                notifyPolarAlignmentCaptureReady(PolarAlignmentCameraRole::MainCamera,
                                                                 QString::fromStdString(filename));
                                return;
                            }
                        }
                        saveFitsAsPNG(QString::fromStdString(filename), true); // "/dev/shm/ccd_simulator.fits"
                        // saveFitsAsPNG("/home/quarcs/2025_06_26T08_24_13_544.fits", true);
                        // saveFitsAsPNG("/dev/shm/SOLVETEST.fits", true);

                        // 如果自动保存开启，自动保存图像
                        if (mainCameraAutoSave && isScheduleRunning == false)
                        {
                            Logger::Log("Auto Save enabled, saving captured image...", LogLevel::INFO, DeviceType::MAIN);
                            CaptureImageSave();
                        }
                    }
                    else
                    {

                        saveFitsAsJPG(QString::fromStdString(filename), true);

                        Logger::Log("saveFitsAsJPG", LogLevel::DEBUG, DeviceType::MAIN);
                    }
                    // Logger::Log("拍摄完成，图像保存完成 finish!", LogLevel::INFO, DeviceType::MAIN);
                }
            }

            // 导星相机：INDI 直出图（替代 PHD2）。收到一帧后：
            // 1) 生成导星预览 JPG（前端 Guide 画面）
            // 2) 把 FITS 保存到与主相机相同的保存目录下，命名为 guider.fits（覆盖写）
            // 3) 若开启循环曝光，则调度下一帧
            if (dpGuider != NULL)
            {
                if (dpGuider->getDeviceName() == devname)
                {
                    const QString fitsPath = QString::fromStdString(filename);
                    if (polarGuiderSingleCapturePending)
                    {
                        notifyPolarAlignmentCaptureReady(PolarAlignmentCameraRole::Guider, fitsPath);
                        polarGuiderSingleCapturePending = false;
                    }

                    // 1) 预览：从 FITS 读取单通道并拉伸为 8-bit，复用原有 saveGuiderImageAsJPG 的前端协议
                    {
                        fitsfile *fptr = nullptr;
                        int status = 0;
                        int bitpix = 0;
                        int naxis = 0;
                        long naxes[2] = {0, 0};

                        if (fits_open_file(&fptr, fitsPath.toUtf8().constData(), READONLY, &status) == 0)
                        {
                            fits_get_img_param(fptr, 2, &bitpix, &naxis, naxes, &status);
                            if (status == 0 && naxis >= 2 && naxes[0] > 0 && naxes[1] > 0)
                            {
                                const long w = naxes[0];
                                const long h = naxes[1];
                                const long npix = w * h;
                                long fpixel[2] = {1, 1};

                                cv::Mat img16;
                                img16.create((int)h, (int)w, CV_16UC1);

                                if (bitpix == 8)
                                {
                                    std::vector<unsigned char> tmp((size_t)npix);
                                    fits_read_pix(fptr, TBYTE, fpixel, npix, NULL, tmp.data(), NULL, &status);
                                    if (status == 0)
                                    {
                                        uint16_t *dst = reinterpret_cast<uint16_t *>(img16.data);
                                        for (long i = 0; i < npix; ++i)
                                            dst[i] = static_cast<uint16_t>(tmp[(size_t)i]) * 257; // 8->16 展开
                                    }
                                }
                                else
                                {
                                    // 默认按 16-bit 读取（兼容大多数导星相机输出）
                                    fits_read_pix(fptr, TUSHORT, fpixel, npix, NULL, img16.data, NULL, &status);
                                }

                                if (status == 0)
                                {
                                    // 计算导星帧动态范围，避免“自动拉伸但仍全黑”的情况
                                    double minVal = 0.0, maxVal = 0.0;
                                    cv::minMaxLoc(img16, &minVal, &maxVal);

                                    uint16_t B = 0, W = 65535;
                                    // 导星预览默认启用自动拉伸：前端 Guide 画面更直观
                                    Tools::GetAutoStretch(img16, 0, B, W);

                                    // 兜底：若自动拉伸的白点远大于实际最大值，会导致整体偏暗（甚至全黑）
                                    // 这里用实际 maxVal 作为上限，确保至少能看见星点/背景变化
                                    if (maxVal > 0.0)
                                    {
                                        const uint16_t maxU16 = (uint16_t)std::min(65535.0, std::max(0.0, maxVal));
                                        if (W > (uint16_t)std::min<uint32_t>(65535u, (uint32_t)maxU16 + 1024u))
                                        {
                                            B = 0;
                                            W = std::max<uint16_t>(1, maxU16);
                                        }
                                        if (W <= B) W = (uint16_t)std::min<uint32_t>(65535u, (uint32_t)B + 10u);
                                    }

                                    Logger::Log("GuiderPreviewStretch | bitpix=" + std::to_string(bitpix) +
                                                    " min=" + std::to_string(minVal) +
                                                    " max=" + std::to_string(maxVal) +
                                                    " B=" + std::to_string(B) +
                                                    " W=" + std::to_string(W),
                                                LogLevel::DEBUG, DeviceType::GUIDER);

                                    cv::Mat img8;
                                    img8.create(img16.rows, img16.cols, CV_8UC1);
                                    Tools::Bit16To8_Stretch(img16, img8, B, W);
                                    // 兜底：如果拉伸结果仍然全黑（通常是 B/W 异常或位深/动态范围不匹配）
                                    // 强制用真实最大值做一次映射，确保“过曝”不会变成黑屏
                                    double min8 = 0.0, max8 = 0.0;
                                    cv::minMaxLoc(img8, &min8, &max8);
                                    if (max8 <= 0.0 && maxVal > 0.0) {
                                        const uint16_t maxU16 = (uint16_t)std::min(65535.0, std::max(1.0, maxVal));
                                        B = 0;
                                        W = maxU16;
                                        Tools::Bit16To8_Stretch(img16, img8, B, W);
                                        Logger::Log("GuiderPreviewStretch | fallback restretch applied (img8 max==0). B=" +
                                                        std::to_string(B) + " W=" + std::to_string(W),
                                                    LogLevel::INFO, DeviceType::GUIDER);
                                    }
                                    saveGuiderImageAsJPG(img8);
                                }
                            }
                            fits_close_file(fptr, &status);
                        }
                    }

                    // 2) 保存：与主相机 CaptureImageSave 的目录结构保持一致（按日期）
                    {
                        std::time_t currentTime = std::time(nullptr);
                        std::tm *timeInfo = std::localtime(&currentTime);
                        char buffer[80];
                        std::strftime(buffer, 80, "%Y-%m-%d", timeInfo); // YYYY-MM-DD

                        const QString destinationDirectory = ImageSaveBaseDirectory + "/CaptureImage/" + QString(buffer);
                        const QString destinationPath = destinationDirectory + "/guider.fits";
                        const bool isUSBSave = (saveMode != "local");

                        // U 盘模式需要提前创建目录（sudo），本地模式 saveImageFile 会自行创建
                        const QString dirPathToCreate = isUSBSave ? destinationDirectory : QString();
                        int checkResult = checkStorageSpaceAndCreateDirectory(
                            fitsPath,
                            ImageSaveBaseDirectory + "/CaptureImage",
                            dirPathToCreate,
                            "GuiderFitsSave",
                            isUSBSave,
                            nullptr);
                        if (checkResult == 0)
                        {
                            saveImageFile(fitsPath, destinationPath, "GuiderFitsSave", isUSBSave);
                        }
                    }

                    // 3) 循环曝光：放行下一帧
                    guiderExposureInFlight = false;
                    if (isGuiderLoopExp && guiderLoopTimer)
                    {
                        // 注意：INDI 图像回调可能在非 GUI 线程触发，直接 start(QTimer) 会报：
                        // "QObject::startTimer: Timers cannot be started from another thread"
                        // 因此把启动下一帧投递回 MainWindow 线程执行。
                        QMetaObject::invokeMethod(this, [this]() {
                            if (isGuiderLoopExp && guiderLoopTimer)
                                guiderLoopTimer->start(1);
                        }, Qt::QueuedConnection);
                    }
                }
            }
            if (dpPoleScope != NULL)
            {
                if (dpPoleScope->getDeviceName() == devname)
                {
                    const QString fitsPath = QString::fromStdString(filename);
                    if (polarGuiderSingleCapturePending)
                    {
                        notifyPolarAlignmentCaptureReady(PolarAlignmentCameraRole::PoleCamera, fitsPath);
                        polarGuiderSingleCapturePending = false;
                    }
                    guiderExposureInFlight = false;
                }
            }
        });
    Logger::Log("indi_Client->setImageReceivedCallback finish!", LogLevel::INFO, DeviceType::MAIN);

    indi_Client->setMessageReceivedCallback(
        [this](const std::string &message)
        {
            // qDebug("indi初始信息 %s", message.c_str());
            QString messageStr = QString::fromStdString(message.c_str());

            // 使用正则表达式移除时间戳
            std::regex timestampRegex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}: )");
            messageStr = QString::fromStdString(std::regex_replace(messageStr.toStdString(), timestampRegex, ""));

            // qDebug("indi提取后信息 %s", messageStr.toStdString().c_str());
            // 使用正则表达式提取并移除日志类型
            std::regex typeRegex(R"(\[(INFO|WARNING|ERROR)\])");
            std::smatch typeMatch;
            QString logType;
            if (std::regex_search(message, typeMatch, typeRegex) && typeMatch.size() > 1)
            {
                logType = QString::fromStdString(typeMatch[1].str());
                // 移除日志类型
                messageStr = QString::fromStdString(std::regex_replace(messageStr.toStdString(), typeRegex, ""));
            }

            if (messageStr.contains("Telescope focal length is missing.") ||
                messageStr.contains("Telescope aperture is missing."))
            {
                // 跳过打印
                return;
            }
            if (logType == "WARNING")
            {
                DeviceType deviceType = getDeviceTypeFromPartialString(messageStr.toStdString());
                Logger::Log("[INDI SERVER] " + messageStr.toStdString(), LogLevel::WARNING, deviceType);
            }
            else if (logType == "ERROR")
            {
                DeviceType deviceType = getDeviceTypeFromPartialString(messageStr.toStdString());
                Logger::Log("[INDI SERVER] " + messageStr.toStdString(), LogLevel::ERROR, deviceType);
            }
            else
            {
                DeviceType deviceType = getDeviceTypeFromPartialString(messageStr.toStdString());
                // 导星循环曝光会高频刷 “Image saved to /dev/shm/guiding.fits” ：降为 DEBUG（默认关闭 DEBUG）避免刷屏
                if (messageStr.contains("Image saved to /dev/shm/guiding.fits"))
                    Logger::Log("[INDI SERVER] " + messageStr.toStdString(), LogLevel::DEBUG, deviceType);
                else
                    Logger::Log("[INDI SERVER] " + messageStr.toStdString(), LogLevel::INFO, deviceType);
            }

            std::regex regexPattern(R"(OnStep slew/syncError:\s*(.*))");
            std::smatch matchResult;

            if (std::regex_search(message, matchResult, regexPattern))
            {
                if (matchResult.size() > 1)
                {
                    QString errorContent = QString::fromStdString(matchResult[1].str());
                    Logger::Log("OnStep Error: " + errorContent.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                    emit wsThread->sendMessageToClient("OnStep Error:" + errorContent);
                    MountGotoError = true;
                }
            }
        });
    Logger::Log("indi_Client->setMessageReceivedCallback finish!", LogLevel::INFO, DeviceType::MAIN);
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




#if 0
// ============================================================================
// PHD2 已移除（2026-01）：原 PHD2 进程/共享内存/导星控制逻辑全部下线。
// 保留代码仅作历史参考，不参与编译与运行。
// ============================================================================

void MainWindow::InitPHD2()
{
    Logger::Log("InitPHD2 start ...", LogLevel::INFO, DeviceType::MAIN);
    isGuideCapture = true;

    if (!cmdPHD2) cmdPHD2 = new QProcess();
    static bool phdSignalsConnected = false;
    if (!phdSignalsConnected)
    {
        connect(cmdPHD2, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &MainWindow::onPhd2Exited);
        connect(cmdPHD2, &QProcess::errorOccurred,
                this, &MainWindow::onPhd2Error);
        phdSignalsConnected = true;
    }

    bool connected = false;
    int retryCount = 1; // 设定重试次数
    while (retryCount > 0 && !connected)
    {
        // 启动前强制结束残留进程并清理共享内存
        // 以避免“初次启动时无法关闭导致启动失败”的问题
        // 1) 若之前有 QProcess 实例，先尝试优雅结束并回收，避免僵尸进程
        if (cmdPHD2->state() != QProcess::NotRunning) {
            cmdPHD2->terminate();
            if (!cmdPHD2->waitForFinished(1500)) {
                cmdPHD2->kill();
                cmdPHD2->waitForFinished(1000);
            }
        }
        // 2) 系统级强杀（多种匹配方式）
        // 注意：有的系统进程名是 phd2.bin，这里同时匹配 phd2 和 phd2.bin
        QProcess::execute("pkill", QStringList() << "-TERM" << "-x" << "phd2");
        QProcess::execute("pkill", QStringList() << "-TERM" << "-x" << "phd2.bin");
        QThread::msleep(200);
        QProcess::execute("pkill", QStringList() << "-KILL" << "-x" << "phd2");
        QProcess::execute("pkill", QStringList() << "-KILL" << "-x" << "phd2.bin");
        QThread::msleep(100);
        // 3) 宽匹配（包含路径/命令行）
        QProcess::execute("pkill", QStringList() << "-TERM" << "-f" << "phd2");
        QThread::msleep(150);
        QProcess::execute("pkill", QStringList() << "-KILL" << "-f" << "phd2");
        QThread::msleep(150);
        // 4) 轮询确认已无残留进程（最多 1s）
        {
            QElapsedTimer waitKill;
            waitKill.start();
            while (waitKill.elapsed() < 1000) {
                int rc = QProcess::execute("pgrep", QStringList() << "-f" << "phd2");
                if (rc != 0) break; // 无匹配
                QThread::msleep(100);
            }
        }

        // 5) 启动前清空 PHD2 日志目录，以规避“损坏/异常 GuidingLog 导致启动卡死”的问题
        //    目标目录：/home/quarcs/Documents/PHD2
        {
            const QString phd2LogDirPath = QStringLiteral("/home/quarcs/Documents/PHD2");
            QDir phd2LogDir(phd2LogDirPath);
            if (phd2LogDir.exists())
            {
                // 递归删除整个日志目录及其内容，然后重新创建一个空目录
                if (!phd2LogDir.removeRecursively())
                {
                    Logger::Log("InitPHD2 | failed to clear PHD2 log dir: " + phd2LogDirPath.toStdString(),
                                LogLevel::WARNING, DeviceType::MAIN);
                }
            }
            // 确保目录最终存在（即使之前不存在或被删除）
            if (!phd2LogDir.mkpath("."))
            {
                Logger::Log("InitPHD2 | failed to recreate PHD2 log dir: " + phd2LogDirPath.toStdString(),
                            LogLevel::WARNING, DeviceType::MAIN);
            }
            else
            {
                Logger::Log("InitPHD2 | PHD2 log dir cleared: " + phd2LogDirPath.toStdString(),
                            LogLevel::INFO, DeviceType::MAIN);
            }
        }
        // 清理共享内存段（key=0x90）
        key_t cleanup_key = 0x90;
        int cleanup_id = shmget(cleanup_key, BUFSZ_PHD, 0666);
        if (cleanup_id != -1) shmctl(cleanup_id, IPC_RMID, NULL);

        // 重新生成共享内存，避免之前的进程遗留问题
        key_phd = 0x90;                                           // 重新设置共享内存的键值
        shmid_phd = shmget(key_phd, BUFSZ_PHD, IPC_CREAT | 0666); // 获取共享内存
        if (shmid_phd < 0)
        {
            Logger::Log("InitPHD2 | shared memory phd shmget ERROR", LogLevel::ERROR, DeviceType::MAIN);
            continue;
        }

        // 映射共享内存
        sharedmemory_phd = (char *)shmat(shmid_phd, NULL, 0);
        if (sharedmemory_phd == NULL)
        {
            Logger::Log("InitPHD2 | shared memory phd map ERROR", LogLevel::ERROR, DeviceType::MAIN);
            continue;
        }

        // 读取共享内存数据（避免将共享内存当作以\\0 结尾字符串打印，可能越界）
        Logger::Log("InitPHD2 | shared memory mapped", LogLevel::INFO, DeviceType::MAIN);

        // 启动 phd2 进程（显式指定实例号 1）
        cmdPHD2->start("phd2", QStringList() << "-i" << "1");
        phd2ExpectedRunning = true;

        // 等待最多 10 秒尝试连接
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 2000)
        {
            usleep(1000);
            qApp->processEvents();
            if (connectPHD() == true)
            {
                connected = true;
                break;
            }
        }

        if (!connected)
        {
            Logger::Log("InitPHD2 | Failed to connect to phd2. Retrying...", LogLevel::WARNING, DeviceType::MAIN);
            retryCount--; // 如果连接失败，重试次数减 1
        }
    }

    if (!connected)
    {
        Logger::Log("InitPHD2 | Failed to connect to phd2 after retries.", LogLevel::ERROR, DeviceType::MAIN);
    }

    Logger::Log("InitPHD2 finished.", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::onPhd2Exited(int exitCode, QProcess::ExitStatus exitStatus)
{
    Logger::Log("PHD2 exited. code=" + std::to_string(exitCode) +
                " status=" + std::to_string((int)exitStatus), LogLevel::WARNING, DeviceType::GUIDER);
    if (phd2ExpectedRunning)
    {
        // 进程异常结束时，尝试发送一次“停止循环拍摄”命令以收敛前端状态
        call_phd_StopLooping();

        // TODO(PHD2): 相关前端信号发送已暂停（切换到 INDI 导星直出图逻辑后不再维护 PHD2 UI）
        // emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        phd2ExpectedRunning = false;
        // 清理共享内存段，避免前端继续读到旧数据
        key_t key = 0x90;
        int id = shmget(key, BUFSZ_PHD, 0666);
        if (id != -1) shmctl(id, IPC_RMID, NULL);
        // TODO(PHD2): 提示前端是否重启（暂不发送）
        // emit wsThread->sendMessageToClient("PHD2ClosedUnexpectedly:是否重新启动PHD2?");
    }
}

void MainWindow::onPhd2Error(QProcess::ProcessError error)
{
    Logger::Log("PHD2 process error: " + std::to_string((int)error), LogLevel::ERROR, DeviceType::GUIDER);
    if (phd2ExpectedRunning)
    {
        // 进程错误时，尝试发送“停止循环拍摄”命令以确保状态一致
        call_phd_StopLooping();
        // TODO(PHD2): 相关前端信号发送已暂停（切换到 INDI 导星直出图逻辑后不再维护 PHD2 UI）
        // emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        phd2ExpectedRunning = false;
        key_t key = 0x90;
        int id = shmget(key, BUFSZ_PHD, 0666);
        if (id != -1) shmctl(id, IPC_RMID, NULL);
        // TODO(PHD2): 提示前端是否重启（暂不发送）
        // emit wsThread->sendMessageToClient("PHD2ClosedUnexpectedly:是否重新启动PHD2?");
    }
}
bool MainWindow::connectPHD(void)
{
    Logger::Log("connectPHD start ...", LogLevel::INFO, DeviceType::MAIN);
    QString versionName = "";
    call_phd_GetVersion(versionName);

    Logger::Log("connectPHD | version:" + versionName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    if (versionName != "")
    {
        // init stellarium operation
        Logger::Log("connectPHD Success!", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
    else
    {
        Logger::Log("connectPHD | there is no openPHD2 running", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("connectPHD failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
}

void MainWindow::disconnectFocuserIfConnected()
{
    if (dpFocuser && dpFocuser->isConnected())
    {
        DisconnectDevice(indi_Client, dpFocuser->getDeviceName(), "Focuser");
    }
    else if (systemdevicelist.system_devices.size() > 22 &&
             systemdevicelist.system_devices[22].isSDKConnect &&
             sdkFocuserHandle != nullptr)
    {
        SdkManager::instance().closeByHandle(sdkFocuserHandle);
        sdkFocuserHandle = nullptr;
        sdkFocuserPort.clear();
        systemdevicelist.system_devices[22].isConnect = false;
        systemdevicelist.system_devices[22].isBind = false;
    }
}

bool MainWindow::call_phd_GetVersion(QString &versionName)
{
    Logger::Log("call_phd_GetVersion start ...", LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_GetVersion | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        versionName = "";
        return false;
    }

    unsigned int baseAddress;
    unsigned int vendcommand;
    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x01;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    // 放宽首次连接等待时长，避免 PHD2 在树莓派等设备上启动/初始化较慢导致的超时
    // 最长等待 10 秒，让 PHD2 有充分时间写回版本信息
    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 2000)
    {
        QThread::msleep(2);
    }

    // 如果超过 10 秒仍未收到响应，则认为超时
    if (t.elapsed() >= 2000)
    {
        versionName = "";
        Logger::Log("call_phd_GetVersion | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_GetVersion failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    else
    {
        unsigned char addr = 0;
        uint16_t length;
        memcpy(&length, sharedmemory_phd + baseAddress + addr, sizeof(uint16_t));
        addr = addr + sizeof(uint16_t);
        // qDebug()<<length;

        if (length > 0 && length < 1024)
        {
            for (int i = 0; i < length; i++)
            {
                versionName.append(sharedmemory_phd[baseAddress + addr + i]);
            }
            Logger::Log("call_phd_GetVersion | version:" + versionName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
            Logger::Log("call_phd_GetVersion success.", LogLevel::INFO, DeviceType::MAIN);
            return true;
            // qDebug()<<versionName;
        }
        else
        {
            versionName = "";
            Logger::Log("call_phd_GetVersion | version is empty", LogLevel::ERROR, DeviceType::MAIN);
            Logger::Log("call_phd_GetVersion failed.", LogLevel::ERROR, DeviceType::MAIN);
            return false;
        }
    }
}

uint32_t MainWindow::call_phd_StartLooping(void)
{
    Logger::Log("call_phd_StartLooping start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_StartLooping | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x03;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // 避免忙等占满 CPU，增加适度休眠
        QThread::msleep(100);
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_StartLooping | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_StartLooping failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_StartLooping success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_StopLooping(void)
{
    Logger::Log("call_phd_StopLooping start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_StopLooping | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x04;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // 避免忙等占满 CPU，增加适度休眠
        QThread::msleep(100);
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_StopLooping | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_StopLooping failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_StopLooping success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_AutoFindStar(void)
{
    Logger::Log("call_phd_AutoFindStar start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_AutoFindStar | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x05;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_AutoFindStar | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_AutoFindStar failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_AutoFindStar success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_StartGuiding(void)
{
    Logger::Log("call_phd_StartGuiding start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_StartGuiding | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x06;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_StartGuiding | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_StartGuiding failed.", LogLevel::ERROR, DeviceType::GUIDER);
        // 在启动导星失败时，发送关闭循环拍摄的命令
        // call_phd_StopLooping();
        // 通知前端状态：循环曝光已关闭
        // emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        isGuiding = false;
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_StartGuiding success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_StopGuiding(void)
{
    Logger::Log("call_phd_StopGuiding start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_StopGuiding | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x17; // 与 PHD2 端 myframe.cpp 中定义的“Stop Guiding Only”命令码保持一致

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_StopGuiding | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_StopGuiding failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_StopGuiding success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

void MainWindow::pauseGuidingBeforeMountMove()
{
    // 仅在当前逻辑导星开关为 ON 时，才在移动前主动停止导星，并记录状态以便之后恢复
    wasGuidingBeforeMountMove = false;

    if (isGuiding)
    {
        Logger::Log("pauseGuidingBeforeMountMove | guiding is ON, stop guiding before mount move.",
                    LogLevel::INFO, DeviceType::GUIDER);
        wasGuidingBeforeMountMove = true;
        // 仅停止导星，不关闭循环曝光，避免影响 PHD2 的 Loop 按钮语义
        call_phd_StopGuiding();
        // 这里不去强制修改 isGuiding / 循环开关，由 PHD2 端自身状态机与前端 UI 控制
    }
}

void MainWindow::resumeGuidingAfterMountMove()
{
    if (!wasGuidingBeforeMountMove)
    {
        // 移动前没有在导星，无需恢复
        return;
    }

    Logger::Log("resumeGuidingAfterMountMove | mount move finished, resume guiding.",
                LogLevel::INFO, DeviceType::GUIDER);

    // 参考 GuiderSwitch=true 的逻辑：如需清校准则先清，再根据是否已选星决定是否自动寻星，最后启动导星
    if (ClearCalibrationData)
    {
        ClearCalibrationData = false;
        call_phd_ClearCalibration();
        Logger::Log("resumeGuidingAfterMountMove | clear calibration data before restart guiding.",
                    LogLevel::INFO, DeviceType::GUIDER);
    }

    if (!glPHD_isSelected)
    {
        Logger::Log("resumeGuidingAfterMountMove | no selected star, call AutoFindStar before guiding.",
                    LogLevel::INFO, DeviceType::GUIDER);
        call_phd_AutoFindStar();
    }

    call_phd_StartGuiding();
    // TODO(PHD2): 相关前端信号发送已暂停（切换到 INDI 导星直出图逻辑后不再维护 PHD2 UI）
    // emit wsThread->sendMessageToClient("GuiderUpdateStatus:1");
}

uint32_t MainWindow::call_phd_checkStatus(unsigned char &status)
{
    Logger::Log("call_phd_checkStatus start ...", LogLevel::DEBUG, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_checkStatus | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        status = 0;
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x07;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    // wait stellarium finished this task
    QElapsedTimer t;
    t.start();
    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    } // wait stellarium run end

    if (t.elapsed() >= 500)
    {
        // timeout
        status = 0;
        Logger::Log("call_phd_checkStatus | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_checkStatus failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }

    else
    {
        status = sharedmemory_phd[3];
        Logger::Log("call_phd_checkStatus | status:" + std::to_string(status), LogLevel::DEBUG, DeviceType::GUIDER);
        Logger::Log("call_phd_checkStatus success.", LogLevel::DEBUG, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_setExposureTime(unsigned int expTime)
{
    Logger::Log("call_phd_setExposureTime start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_setExposureTime | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;
    Logger::Log("call_phd_setExposureTime | expTime:" + std::to_string(expTime), LogLevel::INFO, DeviceType::GUIDER);

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x0b;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &expTime, sizeof(unsigned int));
    addr = addr + sizeof(unsigned int);

    sharedmemory_phd[0] = 0x01; // enable command

    // wait stellarium finished this task
    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    } // wait stellarium run end

    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_setExposureTime | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_setExposureTime failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return QHYCCD_ERROR; // timeout
    }
    else
    {
        Logger::Log("call_phd_setExposureTime success.", LogLevel::INFO, DeviceType::GUIDER);
        return QHYCCD_SUCCESS;
    }
}

uint32_t MainWindow::call_phd_whichCamera(std::string Camera)
{
    Logger::Log("call_phd_whichCamera start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_whichCamera | Camera:" + Camera, LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_whichCamera | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return QHYCCD_ERROR;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x0d;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    int length = Camera.length() + 1;

    unsigned char addr = 0;
    // memcpy(sharedmemory_phd + baseAddress + addr, &index, sizeof(int));
    // addr = addr + sizeof(int);
    memcpy(sharedmemory_phd + baseAddress + addr, &length, sizeof(int));
    addr = addr + sizeof(int);
    memcpy(sharedmemory_phd + baseAddress + addr, Camera.c_str(), length);
    addr = addr + length;

    // wait stellarium finished this task
    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    } // wait stellarium run end

    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_whichCamera | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_whichCamera failed.", LogLevel::ERROR, DeviceType::MAIN);
        return QHYCCD_ERROR; // timeout
    }
    else
    {
        Logger::Log("call_phd_whichCamera success.", LogLevel::INFO, DeviceType::MAIN);
        return QHYCCD_SUCCESS;
    }
}

uint32_t MainWindow::call_phd_ChackControlStatus(int sdk_num)
{
    Logger::Log("call_phd_ChackControlStatus start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_ChackControlStatus | sdk_num:" + std::to_string(sdk_num), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_ChackControlStatus | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x0e;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &sdk_num, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_ChackControlStatus | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_ChackControlStatus failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_ChackControlStatus success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_ClearCalibration(void)
{
    Logger::Log("call_phd_ClearCalibration start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_ClearCalibration | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x02;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_ClearCalibration | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_ClearCalibration failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_ClearCalibration success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}
uint32_t MainWindow::call_phd_StarClick(int x, int y)
{
    Logger::Log("call_phd_StarClick start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_StarClick | x:" + std::to_string(x) + ", y:" + std::to_string(y), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        // 说明：当前工程已默认使用内置导星（GuiderCore），模拟导星/不启动PHD2时这里会被前端误触发。
        // 为避免干扰导星日志，这里降级为 WARNING。
        Logger::Log("call_phd_StarClick | shared memory not ready (PHD2 not running?)", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x0f;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &x, sizeof(int));
    addr = addr + sizeof(int);
    memcpy(sharedmemory_phd + baseAddress + addr, &y, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_StarClick | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_StarClick failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_StarClick success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_FocalLength(int FocalLength)
{
    Logger::Log("call_phd_FocalLength start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_FocalLength | FocalLength:" + std::to_string(FocalLength), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_FocalLength | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x10;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &FocalLength, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_FocalLength | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_FocalLength failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_FocalLength success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_MultiStarGuider(bool isMultiStar)
{
    Logger::Log("call_phd_MultiStarGuider start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_MultiStarGuider | isMultiStar:" + std::to_string(isMultiStar), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_MultiStarGuider | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x11;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &isMultiStar, sizeof(bool));
    addr = addr + sizeof(bool);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_MultiStarGuider | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_MultiStarGuider failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_MultiStarGuider success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_CameraPixelSize(double PixelSize)
{
    Logger::Log("call_phd_CameraPixelSize start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_CameraPixelSize | PixelSize:" + std::to_string(PixelSize), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_CameraPixelSize | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x12;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &PixelSize, sizeof(double));
    addr = addr + sizeof(double);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_CameraPixelSize | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_CameraPixelSize failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_CameraPixelSize success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_CameraGain(int Gain)
{
    Logger::Log("call_phd_CameraGain start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_CameraGain | Gain:" + std::to_string(Gain), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_CameraGain | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x13;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &Gain, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_CameraGain | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_CameraGain failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_CameraGain success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_CalibrationDuration(int StepSize)
{
    Logger::Log("call_phd_CalibrationDuration start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_CalibrationDuration | StepSize:" + std::to_string(StepSize), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_CalibrationDuration | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x14;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &StepSize, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_CalibrationDuration | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_CalibrationDuration failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_CalibrationDuration success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_RaAggression(int Aggression)
{
    Logger::Log("call_phd_RaAggression start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_RaAggression | Aggression:" + std::to_string(Aggression), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_RaAggression | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x15;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &Aggression, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_RaAggression | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_RaAggression failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_RaAggression success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_DecAggression(int Aggression)
{
    Logger::Log("call_phd_DecAggression start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_DecAggression | Aggression:" + std::to_string(Aggression), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_DecAggression | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x16;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &Aggression, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_DecAggression | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_DecAggression failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_DecAggression success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

// ===================== 读取端：共享内存像素解码（完整实现） =====================
// 说明：
// 1) 保持你原有的共享内存布局不变：
//    - [0 .. 1023]  : 预留区（新增 V2 头 ShmHdrV2 放在这里，兼容旧读端）
//    - [1024 ..]    : 你原有的头/状态/导星数据（currentPHDSizeX/Y、bitDepth、各项 guide 数据等）
//    - [2047]       : 帧完成标志位（0x01=写入中，0x02=完成，0x00=已读）
//    - [2048 .. end]: 像素数据（RAW / RLE 压缩 / NEAREST 缩放）
// 2) 本实现自动识别新头（若存在），按 coding 解码；若无新头，则回退旧逻辑。
// 3) 不改变你已有的“读取的内容”（导星/状态字段），仅在像素拷贝前做安全边界检查与解码。


// ===== RLE 解压（速度优先的简单实现）=====
static bool rle_decompress_8(const uint8_t* src, size_t n, uint8_t* dst, size_t outPixels) {
    size_t si=0, di=0;
    while (si+1 <= n && di < outPixels) {
        if (si + 1 > n) return false;
        uint8_t v = src[si++];
        if (si >= n) return false;
        uint8_t run = src[si++];
        if ((size_t)di + run > outPixels) return false;
        std::memset(dst + di, v, run);
        di += run;
    }
    return di == outPixels;
}

static bool rle_decompress_16(const uint8_t* src, size_t n, uint16_t* dst, size_t outPixels) {
    size_t si=0, di=0;
    while (si+4 <= n && di < outPixels) {
        uint16_t v, run;
        std::memcpy(&v,   src+si, 2); si += 2;
        std::memcpy(&run, src+si, 2); si += 2;
        if ((size_t)di + run > outPixels) return false;
        for (uint16_t k=0;k<run;++k) dst[di++] = v;
    }
    return di == outPixels;
}

// ====== 完整的读取函数（在你的 MainWindow 类内）=====
void MainWindow::ShowPHDdata()
{
    // 已弃用 PHD2：保留函数签名以减少改动，但不再读取共享内存，避免刷屏日志。
    return;

    // 修复：增强共享内存指针安全检查
    // 早退：共享内存可用且帧完成
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("ShowPHDdata | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return;
    }

    // 修复：验证共享内存大小，确保kFlagOff在有效范围内
    const size_t total_size = (size_t)BUFSZ;
    if (kFlagOff >= total_size) {
        Logger::Log("ShowPHDdata | kFlagOff out of bounds", LogLevel::ERROR, DeviceType::GUIDER);
        return;
    }

    if (sharedmemory_phd[kFlagOff] != 0x02) {
        // 没有新帧
        return;
    }

    // ---------- 读图像原始头（与旧协议完全一致） ----------
    unsigned int currentPHDSizeX = 1;
    unsigned int currentPHDSizeY = 1;
    unsigned int bitDepth        = 8;

    unsigned int mem_offset = kHeaderOff;

    auto ensure = [&](size_t need) -> bool {
        // 头部区域必须在 payload 前结束
        return (mem_offset + need <= kPayloadOff && mem_offset + need <= total_size);
    };

    if (!ensure(sizeof(unsigned int))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&currentPHDSizeX, sharedmemory_phd + mem_offset, sizeof(unsigned int));
    mem_offset += sizeof(unsigned int);

    if (!ensure(sizeof(unsigned int))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&currentPHDSizeY, sharedmemory_phd + mem_offset, sizeof(unsigned int));
    mem_offset += sizeof(unsigned int);

    if (!ensure(sizeof(unsigned char))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&bitDepth, sharedmemory_phd + mem_offset, sizeof(unsigned char));
    mem_offset += sizeof(unsigned char);

    if (!(bitDepth == 8 || bitDepth == 16)) {
        Logger::Log("ShowPHDdata | invalid bitDepth: " + std::to_string(bitDepth), LogLevel::WARNING, DeviceType::GUIDER);
        sharedmemory_phd[kFlagOff] = 0x00;
        return;
    }

    /* ------------------------------  新增：先读 V2 头，决定本帧尺寸  ------------------------------ */
    ShmHdrV2 v2{};
    bool hasV2 = false;
    if (total_size >= sizeof(ShmHdrV2)) {
        std::memcpy(&v2, sharedmemory_phd, sizeof(ShmHdrV2));
        hasV2 = (v2.magic == SHM_MAGIC && v2.version == SHM_VER);
    }

    // 本帧用于 UI/WS 的“实际尺寸/位深”
    uint32_t dispW = hasV2 ? v2.outW : currentPHDSizeX;
    uint32_t dispH = hasV2 ? v2.outH : currentPHDSizeY;
    uint16_t useDepth = hasV2 ? v2.bitDepth : (uint16_t)bitDepth;

    // 合法性兜底
    if (dispW == 0 || dispH == 0 || !(useDepth==8 || useDepth==16)) {
        // 回退到旧头
        hasV2 = false;
        dispW = currentPHDSizeX;
        dispH = currentPHDSizeY;
        useDepth = (uint16_t)bitDepth;
    }

    // 记录原始/输出尺寸与缩放倍数，供坐标换算使用
    glPHD_OrigImageSizeX = hasV2 ? (int)v2.origW : (int)currentPHDSizeX;
    glPHD_OrigImageSizeY = hasV2 ? (int)v2.origH : (int)currentPHDSizeY;
    glPHD_OutImageSizeX  = (int)dispW;
    glPHD_OutImageSizeY  = (int)dispH;
    {
        double sx = (glPHD_OutImageSizeX  > 0) ? (double)glPHD_OrigImageSizeX / (double)glPHD_OutImageSizeX  : 1.0;
        double sy = (glPHD_OutImageSizeY  > 0) ? (double)glPHD_OrigImageSizeY / (double)glPHD_OutImageSizeY  : 1.0;
        int s = (int)std::lround((sx + sy) * 0.5);
        if (s < 1) s = 1;
        glPHD_ImageScale = s;
    }

    // ---------- 跳过你原有的 3 个 int 字段（sdk_*） ----------
    if (!ensure(sizeof(int))) { sharedmemory_phd[kFlagOff]=0x00; return; }  mem_offset += sizeof(int);
    if (!ensure(sizeof(int))) { sharedmemory_phd[kFlagOff]=0x00; return; }  mem_offset += sizeof(int);
    if (!ensure(sizeof(int))) { sharedmemory_phd[kFlagOff]=0x00; return; }  mem_offset += sizeof(int);

    // ---------- 读取导星/状态数据（保持不变，不缺少） ----------
    unsigned int guideDataIndicatorAddress = (unsigned int)mem_offset;
    if (!ensure(sizeof(unsigned char))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    unsigned char guideDataIndicator = *(unsigned char*)(sharedmemory_phd + mem_offset);
    mem_offset += sizeof(unsigned char);

    double dRa=0, dDec=0, SNR=0, MASS=0, RMSErrorX=0, RMSErrorY=0, RMSErrorTotal=0, PixelRatio=1;
    int RADUR=0, DECDUR=0; char RADIR=0, DECDIR=0; bool StarLostAlert=false, InGuiding=false;

    auto safe_copy = [&](void* dst, size_t n) -> bool {
        if (!ensure(n)) { sharedmemory_phd[kFlagOff]=0x00; return false; }
        std::memcpy(dst, sharedmemory_phd + mem_offset, n);
        mem_offset += n;
        return true;
    };

    if (!safe_copy(&dRa, sizeof(double))) return;
    if (!safe_copy(&dDec, sizeof(double))) return;
    if (!safe_copy(&SNR, sizeof(double))) return;
    if (!safe_copy(&MASS, sizeof(double))) return;
    if (!safe_copy(&RADUR, sizeof(int))) return;
    if (!safe_copy(&DECDUR, sizeof(int))) return;
    if (!safe_copy(&RADIR, sizeof(char))) return;
    if (!safe_copy(&DECDIR, sizeof(char))) return;
    if (!safe_copy(&RMSErrorX, sizeof(double))) return;
    if (!safe_copy(&RMSErrorY, sizeof(double))) return;
    if (!safe_copy(&RMSErrorTotal, sizeof(double))) return;
    if (!safe_copy(&PixelRatio, sizeof(double))) return;
    if (!safe_copy(&StarLostAlert, sizeof(bool))) return;
    if (!safe_copy(&InGuiding, sizeof(bool))) return;

    // 你的 1024+200 区域：锁星、十字、MultiStar（保持不变）
    mem_offset = kHeaderOff + 200;
    auto ensure_at = [&](size_t off, size_t n)->bool {
        return (off + n <= kPayloadOff && off + n <= total_size);
    };

    bool isSelected=false, showLockedCross=false;
    double StarX=0, StarY=0, LockedPositionX=0, LockedPositionY=0;
    unsigned char MultiStarNumber=0;
    unsigned short MultiStarX[32]={0}, MultiStarY[32]={0};

    if (!ensure_at(mem_offset, sizeof(bool))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&isSelected, sharedmemory_phd + mem_offset, sizeof(bool)); mem_offset += sizeof(bool);

    if (!ensure_at(mem_offset, sizeof(double))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&StarX, sharedmemory_phd + mem_offset, sizeof(double)); mem_offset += sizeof(double);

    if (!ensure_at(mem_offset, sizeof(double))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&StarY, sharedmemory_phd + mem_offset, sizeof(double)); mem_offset += sizeof(double);

    if (!ensure_at(mem_offset, sizeof(bool))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&showLockedCross, sharedmemory_phd + mem_offset, sizeof(bool)); mem_offset += sizeof(bool);

    if (!ensure_at(mem_offset, sizeof(double))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&LockedPositionX, sharedmemory_phd + mem_offset, sizeof(double)); mem_offset += sizeof(double);

    if (!ensure_at(mem_offset, sizeof(double))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&LockedPositionY, sharedmemory_phd + mem_offset, sizeof(double)); mem_offset += sizeof(double);

    if (!ensure_at(mem_offset, sizeof(unsigned char))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&MultiStarNumber, sharedmemory_phd + mem_offset, sizeof(unsigned char)); mem_offset += sizeof(unsigned char);
    MultiStarNumber = std::min<unsigned char>(MultiStarNumber, 32);

    if (!ensure_at(mem_offset, sizeof(MultiStarX))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(MultiStarX, sharedmemory_phd + mem_offset, sizeof(MultiStarX)); mem_offset += sizeof(MultiStarX);

    if (!ensure_at(mem_offset, sizeof(MultiStarY))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(MultiStarY, sharedmemory_phd + mem_offset, sizeof(MultiStarY)); mem_offset += sizeof(MultiStarY);

    // 清除导星数据指示位（保持你的行为）
    sharedmemory_phd[guideDataIndicatorAddress] = 0x00;

    // ---------- 将导星/锁星信息分发到 UI/WS（保持你的逻辑） ----------
    glPHD_isSelected         = isSelected;
    glPHD_StarX              = StarX;
    glPHD_StarY              = StarY;
    glPHD_CurrentImageSizeX  = dispW;   // UI 显示尺寸（合并/缩放后）
    glPHD_CurrentImageSizeY  = dispH;   // UI 显示尺寸（合并/缩放后）
    glPHD_LockPositionX      = LockedPositionX;
    glPHD_LockPositionY      = LockedPositionY;
    glPHD_ShowLockCross      = showLockedCross;

    glPHD_Stars.clear();
    // TODO(PHD2): 相关前端信号发送已暂停（切换到 INDI 导星直出图逻辑后不再维护 PHD2 UI）
    // emit wsThread->sendMessageToClient("ClearPHD2MultiStars");
    const double mapRatioX = (glPHD_OrigImageSizeX > 0) ? (double)glPHD_OutImageSizeX / (double)glPHD_OrigImageSizeX : 1.0;
    const double mapRatioY = (glPHD_OrigImageSizeY > 0) ? (double)glPHD_OutImageSizeY / (double)glPHD_OrigImageSizeY : 1.0;
    for (int i = 1; i < MultiStarNumber; i++) {
        if (i > 12) break;
        int outX = (int)std::lround(MultiStarX[i] * mapRatioX);
        int outY = (int)std::lround(MultiStarY[i] * mapRatioY);
        if (outX < 0) outX = 0;
        if (outY < 0) outY = 0;
        if (outX >= glPHD_OutImageSizeX) outX = glPHD_OutImageSizeX - 1;
        if (outY >= glPHD_OutImageSizeY) outY = glPHD_OutImageSizeY - 1;
        QPoint p; p.setX(outX); p.setY(outY);
        glPHD_Stars.push_back(p);
        // TODO(PHD2): 前端多星位置同步（暂不发送）
        // emit wsThread->sendMessageToClient(
        //     "PHD2MultiStarsPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
        //     QString::number(glPHD_CurrentImageSizeY) + ":" +
        //     QString::number(outX) + ":" + QString::number(outY));
    }

    if (glPHD_isSelected) {
        // TODO(PHD2): 前端锁星框显示（暂不发送）
        // emit wsThread->sendMessageToClient("PHD2StarBoxView:true");
        int outStarX = (int)std::lround(glPHD_StarX * mapRatioX);
        int outStarY = (int)std::lround(glPHD_StarY * mapRatioY);
        if (outStarX < 0) outStarX = 0;
        if (outStarY < 0) outStarY = 0;
        if (outStarX >= glPHD_OutImageSizeX) outStarX = glPHD_OutImageSizeX - 1;
        if (outStarY >= glPHD_OutImageSizeY) outStarY = glPHD_OutImageSizeY - 1;
        // TODO(PHD2): 前端锁星框位置（暂不发送）
        // emit wsThread->sendMessageToClient(
        //     "PHD2StarBoxPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
        //     QString::number(glPHD_CurrentImageSizeY) + ":" +
        //     QString::number(outStarX) + ":" + QString::number(outStarY));
    } else {
        // TODO(PHD2): 前端锁星框隐藏（暂不发送）
        // emit wsThread->sendMessageToClient("PHD2StarBoxView:false");
    }

    if (glPHD_ShowLockCross) {
        // TODO(PHD2): 前端锁星十字显示（暂不发送）
        // emit wsThread->sendMessageToClient("PHD2StarCrossView:true");
        int outLockX = (int)std::lround(glPHD_LockPositionX * mapRatioX);
        int outLockY = (int)std::lround(glPHD_LockPositionY * mapRatioY);
        if (outLockX < 0) outLockX = 0;
        if (outLockY < 0) outLockY = 0;
        if (outLockX >= glPHD_OutImageSizeX) outLockX = glPHD_OutImageSizeX - 1;
        if (outLockY >= glPHD_OutImageSizeY) outLockY = glPHD_OutImageSizeY - 1;
        // TODO(PHD2): 前端锁星十字位置（暂不发送）
        // emit wsThread->sendMessageToClient(
        //     "PHD2StarCrossPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
        //     QString::number(glPHD_CurrentImageSizeY) + ":" +
        //     QString::number(outLockX) + ":" + QString::number(outLockY));
    } else {
        // TODO(PHD2): 前端锁星十字隐藏（暂不发送）
        // emit wsThread->sendMessageToClient("PHD2StarCrossView:false");
    }

    // ---------- 导星状态/曲线 ----------
    // 不要用 (dRa/dDec != 0) 判断“是否有新导星数据”：
    // - 误差为 0 是完全合法的（导星很稳时经常出现），此时 RMS/曲线也应持续更新。
    // - shared memory 中 guideDataIndicator 用来标记“本帧有新导星数据”，读取后会被清零。
    if (sharedmemory_phd[kFlagOff] == 0x02 && bitDepth > 0 && currentPHDSizeX > 0 && currentPHDSizeY > 0) {
        unsigned char phdstatu;
        call_phd_checkStatus(phdstatu);

        Logger::Log("ShowPHDdata | dRa:" + std::to_string(dRa) + ", dDec:" + std::to_string(dDec) +
                    ", guideDataIndicator:" + std::to_string((int)guideDataIndicator),
                    LogLevel::DEBUG, DeviceType::GUIDER);

        if (dRa != 0 || dDec != 0) {
            QPointF tmp; tmp.setX(-dRa * PixelRatio); tmp.setY(dDec * PixelRatio);
            glPHD_rmsdate.append(tmp);
            // TODO(PHD2): 前端散点图数据（暂不发送）
            // emit wsThread->sendMessageToClient("AddScatterChartData:" +
            //     QString::number(-dRa * PixelRatio) + ":" + QString::number(-dDec * PixelRatio));

            if (InGuiding) {
                // TODO(PHD2): 前端导星状态（暂不发送）
                // emit wsThread->sendMessageToClient("GuiderStatus:InGuiding");
                // emit wsThread->sendMessageToClient("GuiderUpdateStatus:1");
            } else {
                // TODO(PHD2): 前端导星状态（暂不发送）
                // emit wsThread->sendMessageToClient("GuiderStatus:InCalibration");
                // emit wsThread->sendMessageToClient("GuiderUpdateStatus:1");
            }

            if (StarLostAlert) {
                Logger::Log("ShowPHDdata | send GuiderStatus:StarLostAlert",
                            LogLevel::DEBUG, DeviceType::GUIDER);
                // TODO(PHD2): 前端丢星告警（暂不发送）
                // emit wsThread->sendMessageToClient("GuiderStatus:StarLostAlert");
                // emit wsThread->sendMessageToClient("GuiderUpdateStatus:2");
            }

            // TODO(PHD2): 前端 RMS 曲线（暂不发送）
            // emit wsThread->sendMessageToClient("AddRMSErrorData:" +
            //     QString::number(RMSErrorX, 'f', 3) + ":" + QString::number(RMSErrorX, 'f', 3));
        }

        for (int i = 0; i < glPHD_rmsdate.size(); i++) {
            if (i == glPHD_rmsdate.size() - 1) {
                // TODO(PHD2): 前端折线图数据（暂不发送）
                // emit wsThread->sendMessageToClient("AddLineChartData:" + QString::number(i) + ":" +
                //     QString::number(glPHD_rmsdate[i].x()) + ":" + QString::number(glPHD_rmsdate[i].y()));
                if (i > 50)
                    ; // TODO(PHD2): 前端折线图范围（暂不发送）
                else
                    ; // TODO(PHD2): 前端折线图范围（暂不发送）
            }
        }
    }

    // ===================== 像素数据读取 / 解码 =====================
    const size_t payload_cap_bytes = (total_size > kPayloadOff) ? (total_size - kPayloadOff) : 0;
    if (payload_cap_bytes == 0) { sharedmemory_phd[kFlagOff] = 0x00; return; }

    cv::Mat PHDImg;
    std::unique_ptr<uint8_t[]> buf;

    if (hasV2) {
        Logger::Log("V2 hdr: coding=" + std::to_string(v2.coding) +
                    " out=" + std::to_string(v2.outW) + "x" + std::to_string(v2.outH) +
                    " depth=" + std::to_string(v2.bitDepth) +
                    " payload=" + std::to_string(v2.payloadSize),
                    LogLevel::DEBUG, DeviceType::GUIDER);

        const uint16_t coding   = v2.coding;
        const uint16_t v2Depth  = v2.bitDepth;
        const size_t   bpp      = (v2Depth == 16) ? 2 : 1;
        const uint32_t outW     = v2.outW;
        const uint32_t outH     = v2.outH;
        const size_t   outPix   = (size_t)outW * (size_t)outH;
        const size_t   need     = outPix * bpp;
        const size_t   payLen   = (size_t)v2.payloadSize;

        if (!(v2Depth==8 || v2Depth==16) || outW==0 || outH==0 || payLen > payload_cap_bytes) {
            // 头异常，退回旧逻辑
            hasV2 = false;
        } else {
            const uint8_t* payload = (const uint8_t*)(sharedmemory_phd + kPayloadOff);

            if (coding == CODING_RAW || coding == CODING_NEAREST) {
                if (need > payload_cap_bytes) { sharedmemory_phd[kFlagOff] = 0x00; return; }
                buf.reset(new uint8_t[need]);
                std::memcpy(buf.get(), payload, need);
                if (v2Depth == 16) PHDImg.create(outH, outW, CV_16UC1);
                else               PHDImg.create(outH, outW, CV_8UC1);
                PHDImg.data = buf.get();
            } else if (coding == CODING_RLE) {
                buf.reset(new uint8_t[need]);
                bool ok = (v2Depth==8)
                    ? rle_decompress_8(payload, payLen, buf.get(), outPix)
                    : rle_decompress_16(payload, payLen, (uint16_t*)buf.get(), outPix);
                if (!ok) { sharedmemory_phd[kFlagOff] = 0x00; return; }
                if (v2Depth == 16) PHDImg.create(outH, outW, CV_16UC1);
                else               PHDImg.create(outH, outW, CV_8UC1);
                PHDImg.data = buf.get();
            } else {
                hasV2 = false;
            }
        }
    }

    if (!hasV2) {
        Logger::Log("Legacy path (no V2 header)", LogLevel::DEBUG, DeviceType::GUIDER);
        const size_t need = (size_t)currentPHDSizeX * (size_t)currentPHDSizeY * (bitDepth / 8);
        if (need == 0 || need > payload_cap_bytes) {
            Logger::Log("ShowPHDdata | legacy frame too large or zero", LogLevel::WARNING, DeviceType::GUIDER);
            sharedmemory_phd[kFlagOff] = 0x00;
            return;
        }
        buf.reset(new uint8_t[need]);
        std::memcpy(buf.get(), sharedmemory_phd + kPayloadOff, need);
        if (bitDepth == 16) PHDImg.create(currentPHDSizeY, currentPHDSizeX, CV_16UC1);
        else                PHDImg.create(currentPHDSizeY, currentPHDSizeX, CV_8UC1);
        PHDImg.data = buf.get();
    }

    // 像素完整拷贝/解码完成后再清 2047 标志，避免半帧被清
    sharedmemory_phd[kFlagOff] = 0x00;

    // ===== 你的后处理：拉伸/保存/显示（保持不变）=====
    uint16_t B = 0;
    uint16_t W = (PHDImg.depth() == CV_8U) ? 255 : 65535;  // 根据图像位深度设置默认最大值

    cv::Mat image_raw8;
    image_raw8.create(PHDImg.rows, PHDImg.cols, CV_8UC1);

    if (AutoStretch == true) {
        Tools::GetAutoStretch(PHDImg, 0, B, W);
    } else {
        B = 0;
        W = (PHDImg.depth() == CV_8U) ? 255 : 65535;  // 根据图像位深度设置默认最大值
    }
    Tools::Bit16To8_Stretch(PHDImg, image_raw8, B, W);
    saveGuiderImageAsJPG(image_raw8);

}



#endif // QUARCS_ENABLE_EXTERNAL_PHD2

#if QUARCS_ENABLE_EXTERNAL_PHD2
void MainWindow::getTimeNow(int index)
{
    // 获取当前时间点
    auto now = std::chrono::system_clock::now();

    // 将当前时间点转换为毫秒
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    // 将毫秒时间戳转换为时间类型（std::time_t）
    std::time_t time_now = ms / 1000; // 将毫秒转换为秒

    // 使用 std::strftime 函数将时间格式化为字符串
    char buffer[80];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S",
                  std::localtime(&time_now));

    // 添加毫秒部分
    std::string formatted_time = buffer + std::to_string(ms % 1000);

    // 打印带有当前时间的输出
    // std::cout << "TIME(ms): " << formatted_time << "," << index << std::endl;
}

void MainWindow::onPHDControlGuideTimeout()
{
    // Logger::Log("PHD2 Control Guide is Timeout !", LogLevel::DEBUG, DeviceType::MAIN);
    GetPHD2ControlInstruct();
}

void MainWindow::GetPHD2ControlInstruct()
{
    // Logger::Log("GetPHD2ControlInstruct start ...", LogLevel::DEBUG, DeviceType::MAIN);
    std::lock_guard<std::mutex> lock(receiveMutex);

    unsigned int mem_offset;

    int sdk_direction = 0;
    int sdk_duration = 0;
    int sdk_num = 0;
    int zero = 0;
    mem_offset = 1024;

    mem_offset = mem_offset + sizeof(unsigned int);
    mem_offset = mem_offset + sizeof(unsigned int);
    mem_offset = mem_offset + sizeof(unsigned char);

    int ControlInstruct = 0;

    memcpy(&ControlInstruct, sharedmemory_phd + mem_offset, sizeof(int));
    // Logger::Log("GetPHD2ControlInstruct | get ControlInstruct:" + std::to_string(ControlInstruct), LogLevel::DEBUG, DeviceType::MAIN);
    int mem_offset_sdk_num = mem_offset;
    mem_offset = mem_offset + sizeof(int);

    sdk_num = (ControlInstruct >> 24) & 0xFFF;       // 取前12位
    sdk_direction = (ControlInstruct >> 12) & 0xFFF; // 取中间12位
    sdk_duration = ControlInstruct & 0xFFF;          // 取后12位

    if (sdk_num != 0)
    {
        getTimeNow(sdk_num);
        // Logger::Log("GetPHD2ControlInstruct | PHD2ControlTelescope:" + std::to_string(sdk_num) + "," + std::to_string(sdk_direction) + "," + std::to_string(sdk_duration), LogLevel::DEBUG, DeviceType::MAIN);
    }
    if (sdk_duration != 0)
    {
        MainWindow::ControlGuideEx(sdk_direction, sdk_duration, QStringLiteral("PHD2"));

        memcpy(sharedmemory_phd + mem_offset_sdk_num, &zero, sizeof(int));
        // Logger::Log("GetPHD2ControlInstruct | set ControlInstruct to 0", LogLevel::DEBUG, DeviceType::MAIN);
        call_phd_ChackControlStatus(sdk_num); // set pFrame->ControlStatus = 0;
    }
    // Logger::Log("GetPHD2ControlInstruct finish!", LogLevel::DEBUG, DeviceType::MAIN);
}
#endif








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
