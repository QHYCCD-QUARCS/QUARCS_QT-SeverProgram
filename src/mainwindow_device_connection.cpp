#include "mainwindow_command_support.h"

namespace
{
bool indiDriverNamesEquivalent(const QString &lhs, const QString &rhs)
{
    const QString left = lhs.trimmed();
    const QString right = rhs.trimmed();

    if (left.compare(right, Qt::CaseInsensitive) == 0)
        return true;

    const bool leftIsQhy =
        (left.compare(QStringLiteral("indi_qhy_ccd"), Qt::CaseInsensitive) == 0) ||
        (left.compare(QStringLiteral("indi_qhy_ccd2"), Qt::CaseInsensitive) == 0);
    const bool rightIsQhy =
        (right.compare(QStringLiteral("indi_qhy_ccd"), Qt::CaseInsensitive) == 0) ||
        (right.compare(QStringLiteral("indi_qhy_ccd2"), Qt::CaseInsensitive) == 0);

    return leftIsQhy && rightIsQhy;
}
}

// 把已连接的 INDI 设备绑定到角色槽位：设对应 dp 全局 + isConnect + 完成初始化。
// 收敛历史上重复 20+ 处的
//   dp = device; if (size()>N) sd[slot].isConnect = true; AfterDeviceConnect(dp)
// 样板；slot 取 DeviceSlot::*。仅覆盖 Mount/Guider/PoleCamera/MainCamera。
void MainWindow::bindDeviceToRole(int slot, INDI::BaseDevice *device)
{
    if (device == nullptr)
        return;
    switch (slot)
    {
    case DeviceSlot::Mount:      dpMount      = device; break;
    case DeviceSlot::Guider:     dpGuider     = device; break;
    case DeviceSlot::PoleCamera: dpPoleScope  = device; break;
    case DeviceSlot::MainCamera: dpMainCamera = device; break;
    default:
        Logger::Log("bindDeviceToRole | unsupported slot=" + std::to_string(slot),
                    LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    if (slot >= 0 && slot < systemdevicelist.system_devices.size())
        systemdevicelist.system_devices[slot].isConnect = true;
    AfterDeviceConnect(device);
}

// 自动决策旁路 —— 当前【封堵】：一律返回「无决策」，所有角色都走用户手动选择。
//
// 将来要恢复任何自动决策规则（"只有一台就绑给该角色"、"名字里带 POLEMASTER 的
// 就是极轴镜"、按型号推断……），只在这个函数体里写，不要写回调用点。
// 历史上这些规则散落在 continueConnectAllDeviceOnce / ConnectDriver / SDK 三条路径
// 上，同一条规则复制多份且互相不一致，是「连接行为随路径漂移」的主要来源。
//
// role       : "MainCamera" / "Guider" / "PoleCamera" / "Mount" / "Focuser" / "CFW"
// candidates : 调用方整理好的候选集
// 返回       : 选中项的 index；-1 = 无决策 -> 调用方须保持该角色未绑定并上报候选
int MainWindow::autoDecideDeviceForRole(const QString &role, const QVector<AutoDecisionCandidate> &candidates)
{
    Q_UNUSED(role);
    Q_UNUSED(candidates);
    return -1;
}

// 单点 SDK 相机扫描：构造 ScanCameras 命令并派发。收敛原先散在 4 处的重复调用。
// 后续在此加"一次/缓存/共享"（doc §7 与 memory）；当前零行为变化。
SdkResult MainWindow::sdkScanQhyCameras(const QString& driverName)
{
    SdkCommand scanCmd;
    scanCmd.type = SdkCommandType::Custom;
    scanCmd.name = "ScanCameras";
    scanCmd.payload = std::any();
    return SdkManager::instance().call(driverName.toStdString(), nullptr, scanCmd);
}

// 按需打开相机池中某槽位的相机（M2）。
// 背景：扫描（ScanQHYCCD + GetQHYCCDId）只枚举、不占用设备；真正的排他是 OpenQHYCCD。
// 旧流程"扫到几台就 open 全部"会把本该留给 INDI 的相机也占住，是混用时双开冲突的自造根源。
// 池中槽位允许 handle==nullptr（sdkPoolIndexValid 只要求 cameraId 非空），
// 直到某个角色真正被分配到它时，才在此处打开。
SdkDeviceHandle MainWindow::ensureSdkCameraOpen(int poolIndex, const QString& role)
{
    if (!sdkPoolIndexValid(poolIndex))
    {
        Logger::Log("ensureSdkCameraOpen | invalid poolIndex=" + std::to_string(poolIndex),
                    LogLevel::ERROR, DeviceType::MAIN);
        return nullptr;
    }
    if (g_sdkQhyCamHandles[poolIndex] != nullptr)
        return g_sdkQhyCamHandles[poolIndex];   // 已打开：no-op

    const QString driverName = getSDKDriverName(role);
    if (driverName.isEmpty())
    {
        Logger::Log("ensureSdkCameraOpen | cannot resolve SDK driver for role " + role.toStdString(),
                    LogLevel::ERROR, DeviceType::MAIN);
        return nullptr;
    }

    const std::string drv = driverName.toStdString();
    const std::string camId = g_sdkQhyCamIds[poolIndex].toStdString();

    SdkSerialExecutor *exec = nullptr;
    if (role == "MainCamera")      exec = sdkMainCameraExecutor();
    else if (role == "Guider")     exec = sdkGuiderCameraExecutor();
    else if (role == "PoleCamera") exec = sdkPoleCameraExecutor();

    SdkResult openRes;
    if (exec && exec->isRunning())
        openRes = exec->postAndWait<SdkResult>([drv, camId]() { return SdkManager::instance().open(drv, camId); });
    else
        openRes = SdkManager::instance().open(drv, camId);

    if (!openRes.success || !openRes.payload.has_value())
    {
        Logger::Log("ensureSdkCameraOpen | open failed for " + camId + " (role " + role.toStdString() +
                        "): " + openRes.message,
                    LogLevel::ERROR, DeviceType::MAIN);
        return nullptr;
    }

    SdkDeviceHandle handle = std::any_cast<SdkDeviceHandle>(openRes.payload);
    g_sdkQhyCamHandles[poolIndex] = handle;
    Logger::Log("ensureSdkCameraOpen | opened on demand: " + camId + " -> role " + role.toStdString() +
                    " (poolIndex=" + std::to_string(poolIndex) + ")",
                LogLevel::INFO, DeviceType::MAIN);
    return handle;
}

// 相机名 -> 前端分类标签。原先这段 if/else 在全仓复制了 ~10 份。
static QString sdkCameraCategory(const QString& camId)
{
    if (camId.contains("5III", Qt::CaseInsensitive)) return QStringLiteral("5III");
    if (camId.contains("DEMO", Qt::CaseInsensitive)) return QStringLiteral("DEMO");
    return QStringLiteral("OTHER");
}

// 扫描 SDK 相机并登记进池 —— M2 的核心：只枚举，不 open。
// ScanQHYCCD/GetQHYCCDId 只枚举、不占用设备；真正的排他是 OpenQHYCCD。旧流程"扫到几台
// 就 open 全部"会把本该留给 INDI 的相机也占住，是混用时双开冲突的自造根源。
// 这里只登记 (cameraId, handle=nullptr)；待某角色真正被分配到它时，由 ensureSdkCameraOpen 打开。
// 幂等：已登记的 cameraId 不重复加（可安全重复调用，例如池复用场景的"补齐"）。
// 返回：池中有效相机数；<0 表示扫描失败。
int MainWindow::registerSdkCameraPool(const QString& driverName)
{
    if (driverName.isEmpty())
    {
        Logger::Log("registerSdkCameraPool | empty driverName", LogLevel::ERROR, DeviceType::MAIN);
        return -1;
    }

    const SdkResult scanRes = sdkScanQhyCameras(driverName);
    if (!scanRes.success || !scanRes.payload.has_value())
    {
        Logger::Log("registerSdkCameraPool | ScanCameras failed: " + scanRes.message,
                    LogLevel::ERROR, DeviceType::MAIN);
        return -1;
    }
    const int cameraCount = std::any_cast<int>(scanRes.payload);

    for (int idx = 0; idx < cameraCount; ++idx)
    {
        SdkCommand getIdCmd;
        getIdCmd.type = SdkCommandType::Custom;
        getIdCmd.name = "GetCameraIdByIndex";
        getIdCmd.payload = idx;
        const SdkResult idRes = SdkManager::instance().call(driverName.toStdString(), nullptr, getIdCmd);
        if (!idRes.success || !idRes.payload.has_value())
        {
            Logger::Log("registerSdkCameraPool | GetCameraIdByIndex failed at idx=" + std::to_string(idx) +
                            ": " + idRes.message,
                        LogLevel::WARNING, DeviceType::MAIN);
            continue;
        }
        const QString qid = QString::fromStdString(std::any_cast<std::string>(idRes.payload));

        // 幂等：按 cameraId 判是否已登记（句柄可能为空——扫描不再 open）。
        bool already = false;
        for (int i = 0; i < g_sdkQhyCamIds.size(); ++i)
            if (g_sdkQhyCamIds[i] == qid) { already = true; break; }
        if (already)
            continue;

        // 优先复用空槽（handle==nullptr 且 id 为空），否则追加。
        int slot = -1;
        for (int i = 0; i < g_sdkQhyCamHandles.size(); ++i)
            if (g_sdkQhyCamHandles[i] == nullptr && g_sdkQhyCamIds[i].isEmpty()) { slot = i; break; }
        if (slot >= 0)
        {
            g_sdkQhyCamIds[slot] = qid;
        }
        else
        {
            g_sdkQhyCamHandles.push_back(nullptr);
            g_sdkQhyCamIds.push_back(qid);
        }
        Logger::Log("registerSdkCameraPool | registered (not opened): " + qid.toStdString(),
                    LogLevel::INFO, DeviceType::MAIN);
    }

    int valid = 0;
    for (int i = 0; i < g_sdkQhyCamIds.size(); ++i)
        if (sdkPoolIndexValid(i)) ++valid;
    Logger::Log("registerSdkCameraPool | scanned=" + std::to_string(cameraCount) +
                    " poolValid=" + std::to_string(valid),
                LogLevel::INFO, DeviceType::MAIN);
    return valid;
}

// 把池中尚未绑定给任何角色的相机上报为候选（供用户在分配面板手动指派）。
// 使用负数 UI 索引（sdkUiIndexFromPoolIndex），避免与 INDI 的正索引冲突。
void MainWindow::reportSdkCameraCandidates()
{
    if (wsThread == nullptr)
        return;
    for (int i = 0; i < g_sdkQhyCamIds.size(); ++i)
    {
        if (!sdkPoolIndexValid(i))
            continue;
        // 已被某角色占用的池槽位不作为候选
        if (i == g_sdkMainCameraPoolIndex || i == g_sdkGuiderPoolIndex || i == g_sdkPoleCameraPoolIndex)
            continue;
        const QString &camId = g_sdkQhyCamIds[i];
        emit wsThread->sendMessageToClient("DeviceToBeAllocated:CCD:" +
                                          QString::number(sdkUiIndexFromPoolIndex(i)) + ":" +
                                          camId + ":" + sdkCameraCategory(camId));
    }
}

void MainWindow::ConnectAllDeviceOnce()
{
    Logger::Log("Connecting all devices once.", LogLevel::INFO, DeviceType::MAIN);
    
    // 防御性检查：确保 indi_Client 已经初始化
    if (indi_Client == nullptr)
    {
        Logger::Log("ConnectAllDeviceOnce | indi_Client is nullptr", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectFailed:ClientNotInitialized");
        // 发送完成消息，通知前端关闭进度条
        emit wsThread->sendMessageToClient("ConnectAllDeviceComplete");
        return;
    }

    dpMount = nullptr;
    dpGuider = nullptr;
    dpPoleScope = nullptr;
    dpMainCamera = nullptr;
    dpFocuser = nullptr;
    dpCFW = nullptr;

    // 是否存在需要通过 SDK 方式连接的设备
    bool hasSDKDevice = false;
    for (const auto &dev : systemdevicelist.system_devices)
    {
        if (dev.isSDKConnect)
        {
            hasSDKDevice = true;
            break;
        }
    }

    // 仅统计需要通过 INDI 连接的驱动数量（排除 SDK 连接设备）
    int SelectedDriverNum = 0;
    for (const auto &dev : systemdevicelist.system_devices)
    {
        if (!dev.isSDKConnect && !dev.DriverIndiName.isEmpty())
        {
            SelectedDriverNum++;
        }
    }
    if (SelectedDriverNum == 0 && !hasSDKDevice)
    {
        Logger::Log("No driver in system device list.", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectFailed:No driver in system device list.");
        // 发送完成消息，通知前端关闭进度条
        emit wsThread->sendMessageToClient("ConnectAllDeviceComplete");
        return;
    }
    // NumberOfTimesConnectDevice = 0;

    Tools::cleanSystemDeviceListConnect(systemdevicelist);

    // ===================== SDK 连接阶段（放在“全部连接”的最开始）=====================
    // 目的：先连接标记为 SDK 的设备，避免 INDI 启动/等待/无设备场景影响 SDK 连接与前端体验。
    bool sdkMainConnectedNow = false;
    bool sdkGuiderConnectedNow = false;
    bool sdkPoleConnectedNow = false;
    bool sdkFocuserConnectedNow = false;
    const bool wantSdkCamera =
        hasSDKDevice &&
        ((systemdevicelist.system_devices.size() > 20 && systemdevicelist.system_devices[DeviceSlot::MainCamera].isSDKConnect) ||
         (systemdevicelist.system_devices.size() > 1  && systemdevicelist.system_devices[DeviceSlot::Guider].isSDKConnect) ||
         (systemdevicelist.system_devices.size() > 2  && systemdevicelist.system_devices[DeviceSlot::PoleCamera].isSDKConnect));

    if (wantSdkCamera)
    {
        Logger::Log("ConnectAllDeviceOnce | Start SDK connection phase (before INDI).", LogLevel::INFO, DeviceType::MAIN);

        // 初始化 QHY SDK 资源（使用新 SdkManager 框架）
        SdkCommand initCmd;
        initCmd.type = SdkCommandType::Custom;
        initCmd.name = "InitSdkResource";
        initCmd.payload = std::any();
        // 对于 nullptr 句柄，使用 getSDKDriverName 动态获取驱动名称：
        // 主相机未启用 SDK 但导星相机启用 SDK 的场景，也需要能初始化/扫描相机
        const QString sdkCameraDeviceType =
            (systemdevicelist.system_devices.size() > 20 && systemdevicelist.system_devices[DeviceSlot::MainCamera].isSDKConnect)
                ? QStringLiteral("MainCamera")
                : ((systemdevicelist.system_devices.size() > 1 && systemdevicelist.system_devices[DeviceSlot::Guider].isSDKConnect)
                       ? QStringLiteral("Guider")
                       : QStringLiteral("PoleCamera"));
        QString driverName = getSDKDriverName(sdkCameraDeviceType);
        if (driverName.isEmpty()) {
            Logger::Log("ConnectAllDeviceOnce | Cannot get SDK driver name for " + sdkCameraDeviceType.toStdString(),
                        LogLevel::ERROR, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("ConnectFailed:SDK driver name is empty");
            emit wsThread->sendMessageToClient("ConnectAllDeviceComplete");
            return;
        }
        SdkResult initRes = SdkManager::instance().call(driverName.toStdString(), nullptr, initCmd);
        if (!initRes.success) {
            Logger::Log("ConnectAllDeviceOnce | InitSdkResource failed: " + initRes.message,
                        LogLevel::ERROR, DeviceType::MAIN);
        }

        // 扫描相机设备（获取 cameraId）
        // 对于 nullptr 句柄，使用 getSDKDriverName 动态获取驱动名称
        driverName = getSDKDriverName(sdkCameraDeviceType);
        if (driverName.isEmpty()) {
            Logger::Log("ConnectAllDeviceOnce | Cannot get SDK driver name for " + sdkCameraDeviceType.toStdString(),
                        LogLevel::ERROR, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("ConnectFailed:SDK driver name is empty");
            emit wsThread->sendMessageToClient("ConnectAllDeviceComplete");
            return;
        }
        SdkResult scanRes = sdkScanQhyCameras(driverName);
        if (!scanRes.success) {
            Logger::Log("ConnectAllDeviceOnce | SDK ScanCameras failed: " + scanRes.message,
                        LogLevel::ERROR, DeviceType::CAMERA);
        } else {
            Logger::Log("ConnectAllDeviceOnce | SDK ScanCameras success: " + scanRes.message,
                        LogLevel::INFO, DeviceType::CAMERA);

            // 清理旧池
            if (!g_sdkQhyCamHandles.isEmpty())
            {
                Logger::Log("ConnectAllDeviceOnce | SDK camera pool exists, cleaning up ...", LogLevel::INFO, DeviceType::CAMERA);
                
                // 修复：只清理相机池（CameraPool），而不是所有设备（All）
                // 这样可以避免影响其他独立的SDK设备（如调焦器Focuser）
                // 调焦器使用独立的SDK驱动（"indi_qhy_focuser"），不应该被相机池的清理影响
                // 使用异步清理函数，避免主线程阻塞
                // 清理整个相机池，为重新扫描做准备
                cleanupQhySdkPoolAndResource("ConnectAllDeviceOnce: cleaning up old pool before new scan", "CameraPool");
                
                // 等待清理完成（检查所有句柄是否都为 nullptr，最多等待2秒）
                int waitCount = 0;
                const int maxWaitCount = 20;  // 20 * 100ms = 2秒
                bool allHandlesNull = false;
                
                while (waitCount < maxWaitCount)
                {
                    allHandlesNull = true;
                    for (int i = 0; i < g_sdkQhyCamHandles.size(); ++i)
                    {
                        if (g_sdkQhyCamHandles[i] != nullptr)
                        {
                            allHandlesNull = false;
                            break;
                        }
                    }
                    
                    if (allHandlesNull)
                        break;
                    
                    QThread::msleep(100);
                    waitCount++;
                }
                
                // 强制清空池引用
                g_sdkQhyCamHandles.clear();
                g_sdkQhyCamIds.clear();
                g_sdkMainCameraPoolIndex = -1;
                g_sdkGuiderPoolIndex = -1;
                g_sdkPoleCameraPoolIndex = -1;
                sdkMainCameraHandle = nullptr;
                sdkGuiderHandle = nullptr;
                sdkPoleScopeHandle = nullptr;
                sdkMainCameraId.clear();
                
                if (!allHandlesNull)
                {
                    Logger::Log("ConnectAllDeviceOnce | SDK cleanup timeout after " + 
                               std::to_string(waitCount * 100) + "ms", 
                               LogLevel::WARNING, DeviceType::CAMERA);
                }
                
                // 额外等待，确保 SDK 线程中的 close 和 ReleaseSdkResource 完成
                // 这很重要：cleanupQhySdkPoolAndResource 会立即将句柄设为 nullptr（主线程），
                // 但实际的 close 操作在 SDK 线程中异步执行，需要时间完成
                QThread::msleep(800);
            }

            // 打开全部相机，推送到待分配列表
            // M2：扫描登记（不 open）。真正的 open 推迟到 BindingDevice -> ensureSdkCameraOpen。
            const int openedCount = registerSdkCameraPool(driverName);

            if (openedCount <= 0)
            {
                Logger::Log("ConnectAllDeviceOnce | SDK scan ok but open all cameras failed.", LogLevel::ERROR, DeviceType::CAMERA);
            }
            else
            {
                const bool mainWantsSdk =
                    (systemdevicelist.system_devices.size() > 20 && systemdevicelist.system_devices[DeviceSlot::MainCamera].isSDKConnect);
                const bool guiderWantsSdk =
                    (systemdevicelist.system_devices.size() > 1 && systemdevicelist.system_devices[DeviceSlot::Guider].isSDKConnect);
                const bool poleWantsSdk =
                    (systemdevicelist.system_devices.size() > 2 && systemdevicelist.system_devices[DeviceSlot::PoleCamera].isSDKConnect);

                QVector<bool> poolAssigned(g_sdkQhyCamHandles.size(), false);
                auto findPreferredPoolIndex = [&](const QString &savedId) -> int {
                    if (savedId.isEmpty())
                        return -1;
                    for (int i = 0; i < g_sdkQhyCamIds.size(); ++i)
                    {
                        if (!poolAssigned[i] && g_sdkQhyCamIds[i] == savedId)
                            return i;
                    }
                    return -1;
                };
                auto findFirstUnassignedPoolIndex = [&]() -> int {
                    for (int i = 0; i < g_sdkQhyCamHandles.size(); ++i)
                    {
                        if (!poolAssigned[i] && sdkPoolIndexValid(i))
                            return i;
                    }
                    return -1;
                };
                auto findFirstUnassignedPoolIndexExcept = [&](int reservedIndex) -> int {
                    for (int i = 0; i < g_sdkQhyCamHandles.size(); ++i)
                    {
                        if (i == reservedIndex)
                            continue;
                        if (!poolAssigned[i] && sdkPoolIndexValid(i))
                            return i;
                    }
                    return -1;
                };
                auto bindMainCameraFromPool = [&](int poolIndex) -> bool {
                    if (!sdkPoolIndexValid(poolIndex))
                        return false;
                    // 防御：同一池索引不能重复分配给多个角色
                    if (poolAssigned[poolIndex])
                        return false;

                    poolAssigned[poolIndex] = true;
                    g_sdkMainCameraPoolIndex = poolIndex;
                    sdkMainCameraHandle = g_sdkQhyCamHandles[poolIndex];
                    sdkMainCameraId = g_sdkQhyCamIds[poolIndex];
                    systemdevicelist.system_devices[DeviceSlot::MainCamera].isConnect = true;
                    // 保留实际 cameraId，供下次自动识别同一设备
                    systemdevicelist.system_devices[DeviceSlot::MainCamera].DeviceIndiName = sdkMainCameraId;

                    QString driverName = getSDKDriverName("MainCamera");
                    if (!driverName.isEmpty() && sdkMainCameraHandle != nullptr)
                    {
                        SdkResult regRes = SdkManager::instance().registerDevice(
                            driverName.toStdString(),
                            "MainCamera",
                            sdkMainCameraHandle,
                            "主相机",
                            std::any(sdkMainCameraId.toStdString())
                        );
                        if (!regRes.success)
                        {
                            Logger::Log("ConnectAllDeviceOnce | Failed to register MainCamera to SdkManager: " + regRes.message,
                                        LogLevel::WARNING, DeviceType::CAMERA);
                        }
                    }

                    AfterDeviceConnect(nullptr);
                    emit wsThread->sendMessageToClient("AddDeviceType:MainCamera");
                    sdkMainConnectedNow = true;
                    Logger::Log("ConnectAllDeviceOnce | SDK MainCamera auto-bound: " + sdkMainCameraId.toStdString(),
                                LogLevel::INFO, DeviceType::CAMERA);
                    return true;
                };
                auto bindGuiderFromPool = [&](int poolIndex) -> bool {
                    if (!sdkPoolIndexValid(poolIndex))
                        return false;
                    // 防御：同一池索引不能重复分配给多个角色
                    if (poolAssigned[poolIndex])
                        return false;

                    poolAssigned[poolIndex] = true;
                    g_sdkGuiderPoolIndex = poolIndex;
                    sdkGuiderHandle = g_sdkQhyCamHandles[poolIndex];
                    const QString guiderId = g_sdkQhyCamIds[poolIndex];
                    systemdevicelist.system_devices[DeviceSlot::Guider].isConnect = true;
                    // 保留实际 cameraId，供下次自动识别同一设备
                    systemdevicelist.system_devices[DeviceSlot::Guider].DeviceIndiName = guiderId;

                    QString driverName = getSDKDriverName("Guider");
                    if (!driverName.isEmpty() && sdkGuiderHandle != nullptr)
                    {
                        SdkManager::instance().registerDevice(
                            driverName.toStdString(),
                            "Guider",
                            sdkGuiderHandle,
                            "导星相机",
                            std::any(guiderId.toStdString()));
                    }

                    AfterDeviceConnect(nullptr);
                    emit wsThread->sendMessageToClient("AddDeviceType:Guider");
                    sdkGuiderConnectedNow = true;
                    Logger::Log("ConnectAllDeviceOnce | SDK Guider auto-bound: " + guiderId.toStdString(),
                                LogLevel::INFO, DeviceType::GUIDER);
                    return true;
                };
                auto bindPoleCameraFromPool = [&](int poolIndex) -> bool {
                    if (!sdkPoolIndexValid(poolIndex))
                        return false;
                    if (poolAssigned[poolIndex])
                        return false;

                    poolAssigned[poolIndex] = true;
                    g_sdkPoleCameraPoolIndex = poolIndex;
                    sdkPoleScopeHandle = g_sdkQhyCamHandles[poolIndex];
                    const QString poleId = g_sdkQhyCamIds[poolIndex];
                    systemdevicelist.system_devices[DeviceSlot::PoleCamera].isConnect = true;
                    systemdevicelist.system_devices[DeviceSlot::PoleCamera].DeviceIndiName = poleId;

                    QString driverName = getSDKDriverName("PoleCamera");
                    if (!driverName.isEmpty() && sdkPoleScopeHandle != nullptr)
                    {
                        SdkResult regRes = SdkManager::instance().registerDevice(
                            driverName.toStdString(),
                            "PoleCamera",
                            sdkPoleScopeHandle,
                            "电子极轴镜",
                            std::any(poleId.toStdString()));
                        if (!regRes.success)
                        {
                            Logger::Log("ConnectAllDeviceOnce | Failed to register PoleCamera to SdkManager: " + regRes.message,
                                        LogLevel::WARNING, DeviceType::CAMERA);
                        }
                    }
                    else
                    {
                        Logger::Log("ConnectAllDeviceOnce | Cannot register PoleCamera: SDK driver name or handle is empty",
                                    LogLevel::ERROR, DeviceType::CAMERA);
                    }

                    AfterDeviceConnect(nullptr);
                    emit wsThread->sendMessageToClient("AddDeviceType:PoleCamera");
                    sdkPoleConnectedNow = true;
                    Logger::Log("ConnectAllDeviceOnce | SDK PoleCamera auto-bound: " + poleId.toStdString(),
                                LogLevel::INFO, DeviceType::CAMERA);
                    return true;
                };

                // Connect All 优先按上次保存的 cameraId 静默回绑。
                // 仅当保存的设备当前未扫描到时，才保留为待分配并要求用户选择。
                if (mainWantsSdk && !sdkMainConnectedNow)
                {
                    const QString savedMainId = systemdevicelist.system_devices[DeviceSlot::MainCamera].DeviceIndiName;
                    const int mainPickIndex = findPreferredPoolIndex(savedMainId);
                    if (mainPickIndex >= 0)
                    {
                        bindMainCameraFromPool(mainPickIndex);
                    }
                    else if (!savedMainId.isEmpty())
                    {
                        Logger::Log("ConnectAllDeviceOnce | Saved SDK MainCamera not found: " + savedMainId.toStdString(),
                                    LogLevel::WARNING, DeviceType::CAMERA);
                    }
                }

                if (guiderWantsSdk && !sdkGuiderConnectedNow)
                {
                    const QString savedGuiderId = systemdevicelist.system_devices[DeviceSlot::Guider].DeviceIndiName;
                    const int guiderPickIndex = findPreferredPoolIndex(savedGuiderId);
                    if (guiderPickIndex >= 0)
                    {
                        bindGuiderFromPool(guiderPickIndex);
                    }
                    else if (!savedGuiderId.isEmpty())
                    {
                        Logger::Log("ConnectAllDeviceOnce | Saved SDK Guider not found: " + savedGuiderId.toStdString(),
                                    LogLevel::WARNING, DeviceType::GUIDER);
                    }
                }

                // PoleMaster 仍按设备唯一性自动识别；若有已保存 ID，则优先使用保存值。
                int polePickIndex = -1;
                if (poleWantsSdk)
                {
                    const QString savedPoleId = systemdevicelist.system_devices[DeviceSlot::PoleCamera].DeviceIndiName;
                    polePickIndex = findPreferredPoolIndex(savedPoleId);
                    // 持久化回放没命中时，才轮到自动决策——走旁路（当前封堵 -> -1）。
                    // 原先此处按 isPoleMasterName() 认名字直接绑，那是自动决策散落在
                    // 调用点的典型形态，已收敛进 autoDecideDeviceForRole()。
                    if (polePickIndex < 0)
                    {
                        QVector<AutoDecisionCandidate> poleCands;
                        for (int i = 0; i < g_sdkQhyCamIds.size(); ++i)
                        {
                            if (!poolAssigned[i] && sdkPoolIndexValid(i))
                                poleCands.append({i, g_sdkQhyCamIds[i]});
                        }
                        polePickIndex = autoDecideDeviceForRole("PoleCamera", poleCands);
                    }
                    if (polePickIndex >= 0)
                        bindPoleCameraFromPool(polePickIndex);
                }

                // 关键修复：
                // 全部连接路径下，即使主相机/导星镜都已自动绑定，也要把“完整相机池”同步给前端分配列表。
                // 否则前端只收到未分配设备，导致“已绑定设备之间的交换”没有候选可选。
                if (wsThread)
                {
                    for (int i = 0; i < g_sdkQhyCamHandles.size(); ++i)
                    {
                        if (!sdkPoolIndexValid(i))
                            continue;
                        const int uiIdx = sdkUiIndexFromPoolIndex(i);
                        const QString &camId = g_sdkQhyCamIds[i];
                        // [修改] 相机分类：5III→蓝色, DEMO→橙色, 其他→绿色
                        QString category;
                        if (camId.contains("5III", Qt::CaseInsensitive))
                            category = "5III";
                        else if (camId.contains("DEMO", Qt::CaseInsensitive))
                            category = "DEMO";
                        else
                            category = "OTHER";
                        emit wsThread->sendMessageToClient("DeviceToBeAllocated:CCD:" + QString::number(uiIdx) + ":" + camId + ":" + category);
                    }
                }

                bool hasUnassignedCamera = false;
                for (int i = 0; i < g_sdkQhyCamHandles.size(); ++i)
                {
                    if (!poolAssigned[i] && sdkPoolIndexValid(i))
                    {
                        hasUnassignedCamera = true;
                        break;
                    }
                }

                const bool mainNeedsAllocation = mainWantsSdk && !sdkMainConnectedNow;
                const bool guiderNeedsAllocation = guiderWantsSdk && !sdkGuiderConnectedNow;
                const bool poleNeedsAllocation = poleWantsSdk && !sdkPoleConnectedNow;
                if (hasUnassignedCamera && (mainNeedsAllocation || guiderNeedsAllocation || poleNeedsAllocation) && g_sdkQhyCamHandles.size() > 1)
                {
                    // 相机场景改为“当前页内嵌候选条”，不再强制弹出独立分配窗口。
                    if (wsThread)
                    {
                        if (mainWantsSdk)
                        {
                            emit wsThread->sendMessageToClient("AddDeviceType:MainCamera");
                            if (mainNeedsAllocation)
                                emit wsThread->sendMessageToClient("ConnectDriverPendingAllocation:MainCamera");
                            Logger::Log("ConnectAllDeviceOnce | Sending AddDeviceType:MainCamera", LogLevel::INFO, DeviceType::CAMERA);
                        }
                        if (guiderWantsSdk)
                        {
                            emit wsThread->sendMessageToClient("AddDeviceType:Guider");
                            if (guiderNeedsAllocation)
                                emit wsThread->sendMessageToClient("ConnectDriverPendingAllocation:Guider");
                            Logger::Log("ConnectAllDeviceOnce | Sending AddDeviceType:Guider", LogLevel::INFO, DeviceType::CAMERA);
                        }
                        if (poleWantsSdk)
                        {
                            emit wsThread->sendMessageToClient("AddDeviceType:PoleCamera");
                            if (poleNeedsAllocation)
                                emit wsThread->sendMessageToClient("ConnectDriverPendingAllocation:PoleCamera");
                            Logger::Log("ConnectAllDeviceOnce | Sending AddDeviceType:PoleCamera", LogLevel::INFO, DeviceType::CAMERA);
                        }

                        for (int i = 0; i < g_sdkQhyCamHandles.size(); ++i)
                        {
                            if (poolAssigned[i] || !sdkPoolIndexValid(i))
                                continue;
                            const int uiIdx = sdkUiIndexFromPoolIndex(i);
                            const QString &camId = g_sdkQhyCamIds[i];
                            QString camId_cat;
                            if (camId.contains("5III", Qt::CaseInsensitive))
                                camId_cat = "5III";
                            else if (camId.contains("DEMO", Qt::CaseInsensitive))
                                camId_cat = "DEMO";
                            else
                                camId_cat = "OTHER";
                            emit wsThread->sendMessageToClient("DeviceToBeAllocated:CCD:" + QString::number(uiIdx) + ":" + camId + ":" + camId_cat);
                            Logger::Log("ConnectAllDeviceOnce | Sending DeviceToBeAllocated:CCD:" + QString::number(uiIdx).toStdString() + 
                                        ":" + g_sdkQhyCamIds[i].toStdString(), LogLevel::INFO, DeviceType::CAMERA);
                        }
                    }
                    Logger::Log("ConnectAllDeviceOnce | Multiple SDK cameras opened, waiting for allocation of unbound cameras.",
                                LogLevel::INFO, DeviceType::CAMERA);
                }
            }
        }
    }

    // ===================== SDK 连接：电调（Focuser）=====================
    if (hasSDKDevice &&
        systemdevicelist.system_devices.size() > 22 &&
        systemdevicelist.system_devices[DeviceSlot::Focuser].isSDKConnect)
    {
        Logger::Log("ConnectAllDeviceOnce | Start SDK focuser connection.", LogLevel::INFO, DeviceType::FOCUSER);

        // 1) 关闭旧句柄（避免重复占用串口）
        if (sdkFocuserHandle != nullptr)
        {
            SdkManager::instance().closeByHandle(sdkFocuserHandle);
            sdkFocuserHandle = nullptr;
            sdkFocuserPort.clear();
        }

        // 2) 选择串口：手动覆盖 > 上次保存 > 自动识别
        QString portToUse;
        QString portSource;
        if (!focuserSerialPortOverride.isEmpty())
        {
            portToUse = focuserSerialPortOverride;
            portSource = "override";
        }
        else if (!systemdevicelist.system_devices[DeviceSlot::Focuser].DeviceIndiName.isEmpty() &&
                 systemdevicelist.system_devices[DeviceSlot::Focuser].DeviceIndiName.startsWith("/dev/"))
        {
            portToUse = systemdevicelist.system_devices[DeviceSlot::Focuser].DeviceIndiName;
            portSource = "saved";
        }
        else
        {
            portToUse = detector.getFocuserPort();
            portSource = "auto";
        }

        if (!portToUse.isEmpty() && !detector.isPortPresent(portToUse))
        {
            Logger::Log("ConnectAllDeviceOnce | SDK focuser " + portSource.toStdString() +
                            " port is not present: " + portToUse.toStdString() +
                            ", rescan serial ports.",
                        LogLevel::WARNING, DeviceType::FOCUSER);
            const DevicePorts ports = detector.rescan();
            portToUse = ports.focuserPort;
            portSource = "auto-rescan";
            if (!portToUse.isEmpty())
                focuserSerialPortOverride = portToUse;
        }

        if (portToUse.isEmpty())
        {
            Logger::Log("ConnectAllDeviceOnce | SDK focuser port not found (override/saved/auto all empty).",
                        LogLevel::ERROR, DeviceType::FOCUSER);
        }
        else
        {
            Logger::Log("ConnectAllDeviceOnce | SDK focuser using port: " + portToUse.toStdString() +
                            ", baud: " + std::to_string(systemdevicelist.system_devices[DeviceSlot::Focuser].BaudRate),
                        LogLevel::INFO, DeviceType::FOCUSER);
            // 3) 打开串口
            SdkFocuserOpenParam p;
            p.port = portToUse.toStdString();
            p.baudRate = systemdevicelist.system_devices[DeviceSlot::Focuser].BaudRate;
            p.timeoutMs = 3000;

            // 使用getSDKDriverName动态获取驱动名称
            QString driverName = getSDKDriverName("Focuser");
            if (driverName.isEmpty()) {
                Logger::Log("ConnectAllDeviceOnce | Cannot get SDK driver name for Focuser",
                            LogLevel::ERROR, DeviceType::FOCUSER);
                emit wsThread->sendMessageToClient("ConnectFailed:SDK focuser driver name is empty");
                emit wsThread->sendMessageToClient("ConnectAllDeviceComplete");
                return;
            }
            SdkResult openRes = SdkManager::instance().open(driverName.toStdString(), p);
            if (!openRes.success || !openRes.payload.has_value())
            {
                Logger::Log("ConnectAllDeviceOnce | SDK focuser open failed: " + openRes.message,
                            LogLevel::ERROR, DeviceType::FOCUSER);
            }
            else
            {
                sdkFocuserHandle = std::any_cast<SdkDeviceHandle>(openRes.payload);
                sdkFocuserPort = portToUse;

                // 注册设备到 SdkManager，这样 callByHandle 才能找到对应的驱动
                // 注意：此时串口尚未打开（延迟打开），只是保存了参数，不会占用串口
                SdkResult regRes = SdkManager::instance().registerDevice(
                    driverName.toStdString(),
                    "Focuser",
                    sdkFocuserHandle,
                    "QHY Focuser"
                );
                if (!regRes.success)
                {
                    Logger::Log("ConnectAllDeviceOnce | SDK focuser register failed: " + regRes.message,
                                LogLevel::ERROR, DeviceType::FOCUSER);
                    SdkManager::instance().closeByHandle(sdkFocuserHandle);
                    sdkFocuserHandle = nullptr;
                    sdkFocuserPort.clear();
                }
                else
                {
                    // 4) 握手必须在 sdkFocuserExec 线程执行，确保 QSerialPort 在该线程创建/使用，
                    // 否则会触发 "QSocketNotifier ... from another thread" 并导致后续命令/读位置异常。
                    SdkResult hsRes;
                    bool hsOk = false;

                    const SdkDeviceHandle handleSnap = sdkFocuserHandle;
                    const int handshakeWaitMs = p.timeoutMs * 6 + 1000; // 2*timeoutMs*3 + 1s margin（默认约 19s）

                    auto doHandshakeOnce = [&]() -> SdkResult {
                        if (!sdkFocuserExec || !sdkFocuserExec->isRunning())
                        {
                            SdkResult r;
                            r.success = false;
                            r.message = "sdkFocuserExec not running";
                            return r;
                        }

                        auto prom = std::make_shared<std::promise<SdkResult>>();
                        auto fut = prom->get_future();

                        sdkFocuserExec->post([prom, handleSnap]() {
                            SdkCommand hs;
                            hs.type = SdkCommandType::Custom;
                            hs.name = "Handshake";
                            hs.payload = std::any();
                            // 直接通过设备句柄调用，无需指定驱动名称
                            SdkResult r = SdkManager::instance().callByHandle(handleSnap, hs);
                            prom->set_value(r);
                        });

                        const auto st = fut.wait_for(std::chrono::milliseconds(handshakeWaitMs));
                        if (st == std::future_status::ready)
                            return fut.get();

                        SdkResult r;
                        r.success = false;
                        r.message =
                            "Handshake timeout waiting for sdkFocuserExec"
                            " (worker did not finish in " + std::to_string(handshakeWaitMs) +
                            "ms; device may not respond / wrong port/baud / serial busy)";
                        return r;
                    };

                    // 经验修复：在“全部连接”里，Focuser 常紧跟相机 SDK 初始化之后立刻握手，
                    // 若 MCU/ACM 口尚在重启/枚举，就会出现 3s 读超时；稍后单独连接则正常。
                    // 这里做短暂等待 + 重试，提高鲁棒性（不会影响正常设备：成功时首轮立即返回）。
                    const int maxHandshakeAttempts = 3;
                    for (int attempt = 1; attempt <= maxHandshakeAttempts; ++attempt)
                    {
                        if (attempt > 1)
                        {
                            const int delayMs = 600 * attempt; // 1200ms, 1800ms
                            Logger::Log(std::string("ConnectAllDeviceOnce | SDK focuser handshake retry ") + std::to_string(attempt) +
                                            "/" + std::to_string(maxHandshakeAttempts) +
                                            " after " + std::to_string(delayMs) + "ms (previous error: " + hsRes.message + ")",
                                        LogLevel::WARNING, DeviceType::FOCUSER);
                            QThread::msleep(delayMs);
                        }
                        hsRes = doHandshakeOnce();
                        hsOk = hsRes.success;
                        if (hsOk)
                            break;
                    }

                    if (!hsOk)
                    {
                        Logger::Log("ConnectAllDeviceOnce | SDK focuser handshake failed: " + hsRes.message,
                                    LogLevel::ERROR, DeviceType::FOCUSER);
                        SdkManager::instance().closeByHandle(sdkFocuserHandle);
                        sdkFocuserHandle = nullptr;
                        sdkFocuserPort.clear();
                    }
                    else
                    {
                        systemdevicelist.system_devices[DeviceSlot::Focuser].isConnect = true;
                        // 注意：isBind 将由 AfterDeviceConnect 在初始化完成后设置
                        systemdevicelist.system_devices[DeviceSlot::Focuser].DeviceIndiName = portToUse;

                        // SDK-only 场景下可能没有 INDI 设备循环，补发一次 DeviceType
                        if (SelectedDriverNum == 0)
                            emit wsThread->sendMessageToClient("AddDeviceType:Focuser");

                        // 调用统一的连接后初始化流程（参数下发、版本上报、位置推送、isBind 设置等）
                        AfterDeviceConnect(nullptr);

                        sdkFocuserConnectedNow = true;
                        Logger::Log("ConnectAllDeviceOnce | SDK focuser connected: " + portToUse.toStdString(),
                                    LogLevel::INFO, DeviceType::FOCUSER);
                    }
                }
            }
        }
    }

    // 若本次只选择 SDK 设备，则到此结束（不启动/重启 INDI，避免影响 INDI 流程）
    if (SelectedDriverNum == 0 && hasSDKDevice)
    {
        if (!sdkMainConnectedNow && !sdkGuiderConnectedNow && !sdkPoleConnectedNow && !sdkFocuserConnectedNow)
        {
            emit wsThread->sendMessageToClient("ConnectFailed:SDK connection failed");
        }
        // 无论成功还是失败，都需要发送完成消息，通知前端关闭进度条
        emit wsThread->sendMessageToClient("ConnectAllDeviceComplete");
        Logger::Log("ConnectAllDeviceOnce | SDK-only connection completed (success: " + 
                    std::string(sdkMainConnectedNow || sdkGuiderConnectedNow || sdkPoleConnectedNow || sdkFocuserConnectedNow ? "true" : "false") + ")", 
                    LogLevel::INFO, DeviceType::MAIN);
        return;
    }

    // ===================== INDI 连接阶段（保持原逻辑）=====================
    
    // 在启动 INDI 驱动之前，检查是否有需要通过 INDI 连接的 MainCamera
    // 如果 MainCamera 将使用 INDI 连接，需要先释放可能被 SDK 占用的相机资源
    bool needIndiMainCamera = false;
    if (systemdevicelist.system_devices.size() > 20) {
        const auto &mainCamDev = systemdevicelist.system_devices[DeviceSlot::MainCamera];
        // 如果 MainCamera 不是 SDK 连接模式，且有 INDI 驱动名称，说明需要 INDI 连接
        if (!mainCamDev.isSDKConnect && !mainCamDev.DriverIndiName.isEmpty()) {
            needIndiMainCamera = true;
            Logger::Log("ConnectAllDeviceOnce | MainCamera will use INDI connection, checking SDK resource...", 
                        LogLevel::INFO, DeviceType::MAIN);
        }
    }
    
    // 如果需要 INDI 连接主相机，且当前有 SDK 相机资源被占用，先释放
    if (needIndiMainCamera && (sdkMainCameraHandle != nullptr || !g_sdkQhyCamHandles.isEmpty())) {
        Logger::Log("ConnectAllDeviceOnce | Releasing SDK camera resources before INDI connection...", 
                    LogLevel::INFO, DeviceType::MAIN);
        
        // 只清理相机资源，不影响 Focuser
        if (sdkExposureTimer)
            sdkExposureTimer->stop();
        sdkExposureIsROI = false;
        
        // 关闭所有 SDK 相机句柄
        std::vector<SdkDeviceHandle> handles;
        for (int i = 0; i < g_sdkQhyCamHandles.size(); ++i) {
            if (g_sdkQhyCamHandles[i] != nullptr)
                handles.push_back(g_sdkQhyCamHandles[i]);
        }
        
        for (auto h : handles) {
            if (h == nullptr) continue;
            SdkCommand cancelCmd;
            cancelCmd.type = SdkCommandType::Custom;
            cancelCmd.name = "CancelExposure";
            cancelCmd.payload = std::any();
            // 直接通过设备句柄调用，无需指定驱动名称
            SdkManager::instance().callByHandle(h, cancelCmd);
            SdkManager::instance().closeByHandle(h);
        }
        
        // 释放 SDK 全局资源（释放 libusb context）
        SdkCommand relCmd;
        relCmd.type = SdkCommandType::Custom;
        relCmd.name = "ReleaseSdkResource";
        relCmd.payload = std::any();
        // 对于nullptr句柄，使用getSDKDriverName动态获取驱动名称
        QString driverName = getSDKDriverName("MainCamera");
        if (!driverName.isEmpty()) {
            SdkResult relRes = SdkManager::instance().call(driverName.toStdString(), nullptr, relCmd);
            if (relRes.success) {
                Logger::Log("ConnectAllDeviceOnce | SDK camera resources released successfully", 
                            LogLevel::INFO, DeviceType::MAIN);
            } else {
                Logger::Log("ConnectAllDeviceOnce | Failed to release SDK camera resources: " + relRes.message, 
                            LogLevel::WARNING, DeviceType::MAIN);
            }
        }
        
        // 清理全局变量（无论 driverName 是否为空都应该执行）
        g_sdkQhyCamHandles.clear();
        g_sdkQhyCamIds.clear();
            sdkMainCameraHandle = nullptr;
            sdkGuiderHandle = nullptr;
            sdkPoleScopeHandle = nullptr;
            g_sdkMainCameraPoolIndex = -1;
            g_sdkGuiderPoolIndex = -1;
            g_sdkPoleCameraPoolIndex = -1;
            sdkMainCameraId.clear();
            glMainCameraStatu = "IDLE";
            ShootStatus = "IDLE";
        
        // 等待一下，确保 USB 设备完全释放
        QThread::msleep(500);
    }
    
    disconnectIndiServer(indi_Client);
    Tools::stopIndiDriverAll(drivers_list);

    // ===================== 检查并确保 indiserver 已就绪 =====================
    // 在启动驱动前，确保 indiserver 进程运行且 FIFO 可用
    bool indiServerReady = false;
    const QString fifoPath = "/tmp/myFIFO";
    int checkAttempts = 0;
    const int maxCheckAttempts = 10; // 最多检查10次，每次等待200ms
    
    while (!indiServerReady && checkAttempts < maxCheckAttempts)
    {
        // 1. 检查 FIFO 文件是否存在
        QFileInfo fifoInfo(fifoPath);
        if (!fifoInfo.exists())
        {
            Logger::Log("ConnectAllDeviceOnce | FIFO not found, reinitializing INDI server...", LogLevel::WARNING, DeviceType::MAIN);
            initINDIServer();
            QThread::msleep(500); // 等待 indiserver 启动
            checkAttempts++;
            continue;
        }
        
        // 2. 检查 indiserver 进程是否运行
        bool processRunning = false;
        if (glIndiServer != nullptr)
        {
            QProcess::ProcessState state = glIndiServer->state();
            processRunning = (state == QProcess::Running || state == QProcess::Starting);
        }
        
        // 如果进程未运行，重新初始化
        if (!processRunning)
        {
            Logger::Log("ConnectAllDeviceOnce | INDI server process not running, reinitializing...", LogLevel::WARNING, DeviceType::MAIN);
            initINDIServer();
            QThread::msleep(500); // 等待 indiserver 启动
            checkAttempts++;
            continue;
        }
        
        // 3. 检查 FIFO 是否可用（indiserver 已打开读端）
        // 如果 FIFO 可写，说明 indiserver 已就绪
        int fd = ::open(fifoPath.toUtf8().constData(), O_WRONLY | O_NONBLOCK);
        if (fd >= 0)
        {
            ::close(fd);
            indiServerReady = true;
            Logger::Log("ConnectAllDeviceOnce | INDI server is ready (FIFO accessible)", LogLevel::INFO, DeviceType::MAIN);
            // 重置 FIFO 状态，确保后续操作可以正常进行
            Tools::resetIndiFifoState();
            break;
        }
        else
        {
            // 保存 errno，因为后续的日志调用可能会改变它
            int savedErrno = errno;
            
            // FIFO 暂时不可用，等待 indiserver 打开读端
            if (savedErrno == ENXIO)
            {
                // 读端未打开，等待 indiserver 启动
                Logger::Log("ConnectAllDeviceOnce | Waiting for INDI server to open FIFO read end...", LogLevel::DEBUG, DeviceType::MAIN);
                QThread::msleep(200);
                checkAttempts++;
            }
            else if (savedErrno == ENOENT)
            {
                // FIFO 不存在，重新创建
                Logger::Log("ConnectAllDeviceOnce | FIFO missing, recreating...", LogLevel::WARNING, DeviceType::MAIN);
                system("rm -f /tmp/myFIFO");
                system("mkfifo /tmp/myFIFO");
                Tools::resetIndiFifoState();
                QThread::msleep(200);
                checkAttempts++;
            }
            else
            {
                // 其他错误，等待后重试
                Logger::Log("ConnectAllDeviceOnce | FIFO open failed with errno " + std::to_string(savedErrno) + 
                            " (" + std::string(std::strerror(savedErrno)) + "), retrying...", 
                            LogLevel::DEBUG, DeviceType::MAIN);
                QThread::msleep(200);
                checkAttempts++;
            }
        }
    }
    
    if (!indiServerReady)
    {
        Logger::Log("ConnectAllDeviceOnce | Failed to ensure INDI server is ready after " + 
                    std::to_string(maxCheckAttempts) + " attempts. Proceeding anyway...", 
                    LogLevel::WARNING, DeviceType::MAIN);
        // 即使未就绪也继续，让后续的错误处理来处理
    }
    // ===================== indiserver 就绪检查完成 =====================

    QString driverName;
    QString deviceType;
    QVector<QString> nameCheck;

    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        driverName = systemdevicelist.system_devices[i].DriverIndiName;
        deviceType = systemdevicelist.system_devices[i].Description;

        // 跳过标记为 SDK 连接的设备，它们在第二阶段由 SDK 方式连接
        if (systemdevicelist.system_devices[i].isSDKConnect)
        {
            continue;
        }

        if (driverName != "")
        {
            bool isFound = false;
            for (auto item : nameCheck)
            {
                if ((item == driverName) || (item == "indi_qhy_ccd" && driverName == "indi_qhy_ccd2") || (item == "indi_qhy_ccd2" && driverName == "indi_qhy_ccd"))
                {

                    isFound = true;
                    Logger::Log("Found one duplite driver,do not start it again: " + driverName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                    break;
                }
            }

            if (isFound == false)
            {
                Logger::Log("Start INDI Driver:" + driverName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                Tools::startIndiDriver(driverName);
                nameCheck.push_back(driverName);
                ConnectDriverList.push_back(driverName);
            }
        }
    }

    sleep(1);

    // 再次防御性检查，避免空指针解引用
    if (indi_Client == nullptr)
    {
        Logger::Log("ConnectAllDeviceOnce | indi_Client became nullptr before server check", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectFailed:ClientDisconnected");
        // 发送完成消息，通知前端关闭进度条
        emit wsThread->sendMessageToClient("ConnectAllDeviceComplete");
        return;
    }

    if (indi_Client->isServerConnected() == false)
    {
        Logger::Log("Can not find server.", LogLevel::ERROR, DeviceType::MAIN);
        connectIndiServer(indi_Client);
        sleep(1);
    }

    QTimer *timer = new QTimer(this);
    timer->setInterval(1000); // 设置定时器间隔为1000毫秒
    timer->setProperty("connectAllWaitCount", 0);
    connect(timer, &QTimer::timeout, this, [this, timer]()
            {
        // 防御性检查：避免 indi_Client 为空导致段错误
        if (indi_Client == nullptr) {
            Logger::Log("ConnectAllDeviceOnce | indi_Client is nullptr in timer callback", LogLevel::ERROR, DeviceType::MAIN);
            timer->stop();
            timer->deleteLater();
            emit wsThread->sendMessageToClient("ConnectFailed:ClientNotInitialized");
            // 发送完成消息，通知前端关闭进度条
            emit wsThread->sendMessageToClient("ConnectAllDeviceComplete");
            return;
        }

        const int waitCount = timer->property("connectAllWaitCount").toInt();
        if (indi_Client->GetDeviceCount() > 0 || waitCount >= 10) {
            timer->stop();
            timer->deleteLater();
            sleep(2);
            continueConnectAllDeviceOnce(); // 继续执行设备连接的剩余部分
        } else {
            Logger::Log("Waiting for devices...", LogLevel::INFO, DeviceType::MAIN);
            timer->setProperty("connectAllWaitCount", waitCount + 1);
        } });
    timer->start();
}
void MainWindow::continueConnectAllDeviceOnce()
{
    // 防御性检查：确保 indi_Client 有效
    if (indi_Client == nullptr)
    {
        Logger::Log("continueConnectAllDeviceOnce | indi_Client is nullptr", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectFailed:ClientNotInitialized");
        Tools::stopIndiDriverAll(drivers_list);
        ConnectDriverList.clear();
        // 发送完成消息，通知前端关闭进度条
        emit wsThread->sendMessageToClient("ConnectAllDeviceComplete");
        return;
    }

    // 检查是否有 SDK 设备已经连接成功
    bool hasSDKConnected = false;
    if (systemdevicelist.system_devices.size() > 20 && 
        systemdevicelist.system_devices[DeviceSlot::MainCamera].isSDKConnect && 
        systemdevicelist.system_devices[DeviceSlot::MainCamera].isConnect) {
        hasSDKConnected = true;
        Logger::Log("continueConnectAllDeviceOnce | SDK MainCamera is connected", LogLevel::INFO, DeviceType::MAIN);
    }
    if (systemdevicelist.system_devices.size() > 22 && 
        systemdevicelist.system_devices[DeviceSlot::Focuser].isSDKConnect && 
        systemdevicelist.system_devices[DeviceSlot::Focuser].isConnect) {
        hasSDKConnected = true;
        Logger::Log("continueConnectAllDeviceOnce | SDK Focuser is connected", LogLevel::INFO, DeviceType::MAIN);
    }

    if (indi_Client->GetDeviceCount() == 0)
    {
        // 如果有 SDK 设备已连接，则不报错，只记录日志并停止 INDI 驱动
        if (hasSDKConnected) {
            Logger::Log("continueConnectAllDeviceOnce | No INDI device found, but SDK devices are connected. Skipping INDI connection.", LogLevel::INFO, DeviceType::MAIN);
            Tools::stopIndiDriverAll(drivers_list);
            ConnectDriverList.clear();
            // 发送全部连接完成消息，通知前端可以关闭进度条
            emit wsThread->sendMessageToClient("ConnectAllDeviceComplete");
            Logger::Log("continueConnectAllDeviceOnce | All devices connection process completed (SDK only, no INDI)", LogLevel::INFO, DeviceType::MAIN);
            return;
        } else {
            Logger::Log("Driver start success but no device found", LogLevel::ERROR, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("ConnectFailed:No device found.");
            Tools::stopIndiDriverAll(drivers_list);
            ConnectDriverList.clear();
            // 发送完成消息，通知前端关闭进度条
            emit wsThread->sendMessageToClient("ConnectAllDeviceComplete");
            return;
        }
    }

    // 辅助函数：根据 INDI 设备找到对应的 SystemDevice 槽位，并返回应使用的波特率
    auto getBaudRateForDeviceIndex = [this](INDI::BaseDevice *device, int deviceIndex) -> int
    {
        int defaultBaud = 9600;

        // 先使用原来基于索引的逻辑（保持兼容性）
        if (deviceIndex >= 0 && deviceIndex < systemdevicelist.system_devices.size())
        {
            defaultBaud = systemdevicelist.system_devices[deviceIndex].BaudRate;
        }

        if (device == nullptr)
            return defaultBaud;

        // 再尝试根据 driver 名称在 systemdevicelist 中精确匹配
        QString driverExec = QString::fromUtf8(device->getDriverExec());
        for (int idx = 0; idx < systemdevicelist.system_devices.size(); idx++)
        {
            if (indiDriverNamesEquivalent(systemdevicelist.system_devices[idx].DriverIndiName, driverExec))
            {
                return systemdevicelist.system_devices[idx].BaudRate;
            }
        }

        return defaultBaud;
    };

    // 第一阶段：仅处理 INDI 设备（isSDKConnect == false）
    for (int i = 0; i < indi_Client->GetDeviceCount(); i++)
    {
        // 修复：检查系统设备列表索引是否有效
        if (i >= systemdevicelist.system_devices.size()) {
            Logger::Log("ConnectAllDeviceOnce | Index " + std::to_string(i) + " out of bounds for systemdevicelist (size: " + std::to_string(systemdevicelist.system_devices.size()) + ")", LogLevel::ERROR, DeviceType::MAIN);
            break; // 停止循环，避免越界访问
        }
        
        // 修复：检查设备指针是否有效
        INDI::BaseDevice *device = indi_Client->GetDeviceFromList(i);
        if (device == nullptr) {
            Logger::Log("ConnectAllDeviceOnce | Device at index " + std::to_string(i) + " is nullptr", LogLevel::WARNING, DeviceType::MAIN);
            continue; // 跳过这个设备
        }
        
        std::string deviceName = indi_Client->GetDeviceNameFromList(i);
        if (deviceName.empty()) {
            Logger::Log("ConnectAllDeviceOnce | Device name at index " + std::to_string(i) + " is empty", LogLevel::WARNING, DeviceType::MAIN);
            continue;
        }
        
        // 修复索引混用 bug：这里的 i 是 INDI 设备表下标(按驱动注册先后排序)，而
        // systemdevicelist.system_devices 是按“角色槽位”排列(Mount=0/Guider=1/…/Focuser=22)，
        // 两套下标含义不同。直接用 system_devices[i].isSDKConnect 判断会错位——例如 Guider(槽位1)
        // 为 SDK 时，恰好落在 INDI 表 i=1 的那个 INDI 设备(赤道仪或电调焦，取决于注册先后)会被
        // 误当成 SDK 而跳过，导致 connect all 时赤道仪/电调焦间歇性连不上。
        // 正确做法：按驱动名(driverExec)把该 INDI 设备匹配到对应角色槽位，再判该槽位是否 SDK。
        {
            const QString curDriverExec = QString::fromUtf8(device->getDriverExec());
            int roleSlotIdx = -1;
            for (int idx = 0; idx < systemdevicelist.system_devices.size(); idx++)
            {
                if (indiDriverNamesEquivalent(systemdevicelist.system_devices[idx].DriverIndiName, curDriverExec))
                {
                    roleSlotIdx = idx;
                    break;
                }
            }
            if (roleSlotIdx >= 0 && systemdevicelist.system_devices[roleSlotIdx].isSDKConnect)
            {
                Logger::Log("continueConnectAllDeviceOnce | Skip INDI connect for SDK role slot " + std::to_string(roleSlotIdx) +
                                " (indi device index " + std::to_string(i) + ", " + deviceName + ")",
                            LogLevel::INFO, DeviceType::MAIN);
                continue;
            }
        }

        Logger::Log("Start connecting devices(INDI):" + deviceName, LogLevel::INFO, DeviceType::MAIN);

        // 在正式连接前，仅在用户手动选择串口时应用覆盖设置；默认模式不改端口
        // 根据 driverExec 在 systemdevicelist 中查找对应的 DriverType（Mount / Focuser）
        QString driverExec = QString::fromUtf8(device->getDriverExec());
        QString driverType;
        for (int idx = 0; idx < systemdevicelist.system_devices.size(); idx++)
        {
            if (indiDriverNamesEquivalent(systemdevicelist.system_devices[idx].DriverIndiName, driverExec))
            {
                driverType = systemdevicelist.system_devices[idx].Description;
                break;
            }
        }
        // 在连接前设置端口：优先使用手动覆盖，否则自动检测
        if (driverType == "Focuser")
        {
            if (!focuserSerialPortOverride.isEmpty())
            {
                // 使用手动覆盖的端口
                indi_Client->setDevicePort(device, focuserSerialPortOverride);
                Logger::Log("ConnectAllDeviceOnce | Focuser initial Port set to: " + focuserSerialPortOverride.toStdString(),
                            LogLevel::INFO, DeviceType::MAIN);
            }
            else
            {
                // 自动检测 Focuser 端口
                QString detectedPort = detector.getFocuserPort();
                if (!detectedPort.isEmpty())
                {
                    indi_Client->setDevicePort(device, detectedPort);
                    focuserSerialPortOverride = detectedPort; // 同步覆盖值
                    Logger::Log("ConnectAllDeviceOnce | Focuser auto-detected Port set to: " + detectedPort.toStdString(),
                                LogLevel::INFO, DeviceType::MAIN);
                }
                else
                {
                    Logger::Log("ConnectAllDeviceOnce | No Focuser port detected, using default", LogLevel::WARNING, DeviceType::MAIN);
                }
            }
            // 同步当前串口与候选列表到前端
            sendSerialPortOptions(driverType);
        }
        else if (driverType == "Mount")
        {
            if (!mountSerialPortOverride.isEmpty())
            {
                // 使用手动覆盖的端口
                indi_Client->setDevicePort(device, mountSerialPortOverride);
                Logger::Log("ConnectAllDeviceOnce | Mount initial Port set to: " + mountSerialPortOverride.toStdString(),
                            LogLevel::INFO, DeviceType::MAIN);
            }
            else
            {
                // 自动检测 Mount 端口
                QString detectedPort = detector.getMountPort();
                if (!detectedPort.isEmpty())
                {
                    indi_Client->setDevicePort(device, detectedPort);
                    mountSerialPortOverride = detectedPort; // 同步覆盖值
                    Logger::Log("ConnectAllDeviceOnce | Mount auto-detected Port set to: " + detectedPort.toStdString(),
                                LogLevel::INFO, DeviceType::MAIN);
                }
                else
                {
                    Logger::Log("ConnectAllDeviceOnce | No Mount port detected, using default", LogLevel::WARNING, DeviceType::MAIN);
                }
            }
            // 同步当前串口与候选列表到前端
            sendSerialPortOptions(driverType);
        }

        int baudRateToUse = getBaudRateForDeviceIndex(device, i);
        Logger::Log("ConnectAllDeviceOnce | setBaudRate for device " + deviceName + " -> " + std::to_string(baudRateToUse),
                    LogLevel::INFO, DeviceType::MAIN);
        indi_Client->setBaudRate(device, baudRateToUse);
        indi_Client->connectDevice(deviceName.c_str());

        int waitTime = 0;
        while (device != nullptr && !device->isConnected() && waitTime < 5)
        {
            Logger::Log("Wait for Connect" + deviceName, LogLevel::INFO, DeviceType::MAIN);
            QThread::msleep(1000); // 等待1秒
            waitTime++;
        }

        if (device == nullptr || !device->isConnected())
        {
            Logger::Log("ConnectDriver | Device (" + deviceName + ") is not connected,try to update port", LogLevel::WARNING, DeviceType::MAIN);
            
            // 修复：连接失败后，先断开设备以释放可能占用的串口，避免端口被占用导致重试失败
            // 即使连接失败，INDI 驱动可能已经部分打开了串口（tty_connect），需要显式断开以确保端口完全释放
            if (device != nullptr)
            {
                indi_Client->disconnectDevice(device->getDeviceName());
                Logger::Log("ConnectAllDeviceOnce | Disconnected device to release port before retry: " + deviceName, 
                            LogLevel::INFO, DeviceType::MAIN);
                QThread::msleep(200); // 短暂等待，确保端口完全释放
            }
            
            // 特殊处理(电调和赤道仪)
            // 修复：使用已检查的device指针，避免重复调用GetDeviceFromList
            if (device != nullptr && (device->getDriverInterface() & INDI::BaseDevice::FOCUSER_INTERFACE || device->getDriverInterface() & INDI::BaseDevice::TELESCOPE_INTERFACE)){
                QString DevicePort;
                indi_Client->getDevicePort(device, DevicePort);
                QString DeviceType = detector.detectDeviceTypeForPort(DevicePort);

                // 获取设备类型
                QString DriverType = "";
                // 修复：使用已检查的device指针
                if (device != nullptr) {
                    for(int j = 0; j < systemdevicelist.system_devices.size(); j++)
                    {
                        if (indiDriverNamesEquivalent(systemdevicelist.system_devices[j].DriverIndiName,
                                                      QString::fromUtf8(device->getDriverExec())))
                        {
                            DriverType = systemdevicelist.system_devices[j].Description;
                        }
                    }
                    // 处理电调和赤道仪的连接
                    if (DeviceType != "Focuser" && DriverType == "Focuser")
                    {
                        // 识别到当前设备是电调，但是设备的串口不是电调的串口,需更新
                        // 正确的串口是detector.getFocuserPort()
                        QString realFocuserPort = detector.getFocuserPort();
                        if (!realFocuserPort.isEmpty())
                        {
                            indi_Client->setDevicePort(device, realFocuserPort);
                            // 同步更新覆盖值，保证后续连接与前端显示一致
                            focuserSerialPortOverride = realFocuserPort;
                            Logger::Log("ConnectDriver | Focuser Device (" + std::string(device->getDeviceName()) + ") Port is updated to: " + realFocuserPort.toStdString(), LogLevel::INFO, DeviceType::MAIN);

                            // 自动纠正串口后，同步当前串口与候选列表到前端
                            sendSerialPortOptions(DriverType);
                        }
                        else
                        {
                            Logger::Log("No matched Focuser port found by detector.", LogLevel::WARNING, DeviceType::MAIN);
                            continue;
                        }
                    }else if (DeviceType != "Mount" && DriverType == "Mount")
                    {
                        // 识别到当前设备是赤道仪，但是设备的串口不是赤道仪的串口,需更新
                        // 正确的串口是detector.getMountPort()
                        QString realMountPort = detector.getMountPort();
                        if (!realMountPort.isEmpty())
                        {
                            indi_Client->setDevicePort(device, realMountPort);
                            // 同步更新覆盖值，保证后续连接与前端显示一致
                            mountSerialPortOverride = realMountPort;
                            Logger::Log("ConnectDriver | Mount Device (" + std::string(device->getDeviceName()) + ") Port is updated to: " + realMountPort.toStdString(), LogLevel::INFO, DeviceType::MAIN);

                            // 自动纠正串口后，同步当前串口与候选列表到前端
                            sendSerialPortOptions(DriverType);
                        }
                        else
                        {
                            Logger::Log("No matched Mount port found by detector.", LogLevel::WARNING, DeviceType::MAIN);
                            continue;
                        }
                    }else{
                        Logger::Log("ConnectDriver | Device (" + std::string(device->getDeviceName()) + ") Port is not updated.", LogLevel::WARNING, DeviceType::MAIN);
                    }
                }
            }
            // 修复：使用已检查的device指针和deviceName
            if (device != nullptr && !deviceName.empty()) {
                int retryBaudRate = getBaudRateForDeviceIndex(device, i);
                Logger::Log("ConnectAllDeviceOnce | retry setBaudRate for device " + deviceName + " -> " + std::to_string(retryBaudRate),
                            LogLevel::INFO, DeviceType::MAIN);
                indi_Client->setBaudRate(device, retryBaudRate);
                indi_Client->connectDevice(deviceName.c_str());
        
                int waitTime = 0;
                while (device != nullptr && !device->isConnected() && waitTime < 5)
                {
                    // 修复：使用已检查的deviceName变量
                    Logger::Log("Wait for Connect" + deviceName, LogLevel::INFO, DeviceType::MAIN);
                    QThread::msleep(1000); // 等待1秒
                    waitTime++;
                }
            }
        }
    }

    // 注意：SDK 连接已在 ConnectAllDeviceOnce() 的最开始执行，这里不再重复执行，
    // 避免重复 open/close 造成句柄池混乱或影响 INDI 连接节奏。


    ConnectedCCDList.clear();
    ConnectedTELESCOPEList.clear();
    ConnectedFOCUSERList.clear();
    ConnectedFILTERList.clear();
    for (int i = 0; i < indi_Client->GetDeviceCount(); i++) //  indi_Client->GetDeviceFromList(i)
    {
        // 修复：检查设备指针是否有效
        INDI::BaseDevice *device = indi_Client->GetDeviceFromList(i);
        if (device == nullptr) {
            Logger::Log("AfterDeviceConnect | Device at index " + std::to_string(i) + " is nullptr", LogLevel::WARNING, DeviceType::MAIN);
            continue;
        }
        
        if (device->isConnected())
        {
            if (device->getDriverInterface() & INDI::BaseDevice::CCD_INTERFACE)
            {
                Logger::Log("We received a CCD!", LogLevel::INFO, DeviceType::MAIN);
                ConnectedCCDList.push_back(i);
            }
            else if (device->getDriverInterface() & INDI::BaseDevice::FILTER_INTERFACE)
            {
                Logger::Log("We received a FILTER!", LogLevel::INFO, DeviceType::MAIN);
                ConnectedFILTERList.push_back(i);
            }
            else if (device->getDriverInterface() & INDI::BaseDevice::TELESCOPE_INTERFACE)
            {
                Logger::Log("We received a TELESCOPE!", LogLevel::INFO, DeviceType::MAIN);
                ConnectedTELESCOPEList.push_back(i);
            }
            else if (device->getDriverInterface() & INDI::BaseDevice::FOCUSER_INTERFACE)
            {
                Logger::Log("We received a FOCUSER!", LogLevel::INFO, DeviceType::MAIN);
                ConnectedFOCUSERList.push_back(i);
            }
            Logger::Log("Driver:" + std::string(device->getDriverExec()) + " Device:" + std::string(device->getDeviceName()), LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            QString DeviceName = QString::fromStdString(device->getDeviceName());
            Logger::Log("Connect failed device:" + DeviceName.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("ConnectFailed:Connect device failed:" + DeviceName);
        }
    }

    Tools::printSystemDeviceList(systemdevicelist);

    QStringList SelectedCameras = Tools::getCameraNumFromSystemDeviceList(systemdevicelist);
    Logger::Log("Number of Selected cameras:" + std::to_string(SelectedCameras.size()), LogLevel::INFO, DeviceType::MAIN);
    for (auto Camera : SelectedCameras)
    {
        Logger::Log("Selected Cameras:" + Camera.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    }

    Logger::Log("Number of Connected CCD:" + std::to_string(ConnectedCCDList.size()), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("Number of Connected TELESCOPE:" + std::to_string(ConnectedTELESCOPEList.size()), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("Number of Connected FOCUSER:" + std::to_string(ConnectedFOCUSERList.size()), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("Number of Connected FILTER:" + std::to_string(ConnectedFILTERList.size()), LogLevel::INFO, DeviceType::MAIN);

    for (int i = 0; i < indi_Client->GetDeviceCount(); i++)
    {
        // 修复：检查设备指针是否有效
        INDI::BaseDevice *device = indi_Client->GetDeviceFromList(i);
        if (device == nullptr) {
            continue;
        }
        
        if (device->isConnected())
        {
            std::string driverExec = device->getDriverExec();
            QString driverExecQString = QString::fromStdString(driverExec);
            for (int j = 0; j < systemdevicelist.system_devices.size(); j++)
            {
                if (indiDriverNamesEquivalent(systemdevicelist.system_devices[j].DriverIndiName, driverExecQString))
                {
                    emit wsThread->sendMessageToClient("AddDeviceType:" + systemdevicelist.system_devices[j].Description);
                }
            }
        }
    }

    // 检查是否有任何设备连接（包括 INDI 和 SDK 设备）
    bool hasAnyDeviceConnected = (ConnectedCCDList.size() > 0 || 
                                   ConnectedTELESCOPEList.size() > 0 || 
                                   ConnectedFOCUSERList.size() > 0 || 
                                   ConnectedFILTERList.size() > 0);
    
    // 重新检查 SDK 设备连接状态（hasSDKConnected 已在函数开头声明）
    // 这里只需更新日志信息，不需要重新声明变量
    if (hasSDKConnected) {
        Logger::Log("continueConnectAllDeviceOnce | SDK devices are connected and will be counted in final check", LogLevel::INFO, DeviceType::MAIN);
    }

    if (!hasAnyDeviceConnected && !hasSDKConnected)
    {
        Logger::Log("No Device Connected (neither INDI nor SDK)", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectFailed:No Device Connected");
        Tools::stopIndiDriverAll(drivers_list);
        ConnectDriverList.clear();
        // 发送完成消息，通知前端关闭进度条
        emit wsThread->sendMessageToClient("ConnectAllDeviceComplete");
        return;
    }
    
    // 如果只有 SDK 设备连接，不处理 INDI 设备分配流程，直接返回
    if (!hasAnyDeviceConnected && hasSDKConnected)
    {
        Logger::Log("Only SDK devices connected, skipping INDI device allocation", LogLevel::INFO, DeviceType::MAIN);
        Tools::stopIndiDriverAll(drivers_list);
        ConnectDriverList.clear();
        // 发送全部连接完成消息，通知前端可以关闭进度条
        emit wsThread->sendMessageToClient("ConnectAllDeviceComplete");
        Logger::Log("continueConnectAllDeviceOnce | All devices connection process completed (SDK only)", LogLevel::INFO, DeviceType::MAIN);
        return;
    }

    auto savedDeviceNameByDescription = [&](const QString &description) -> QString
    {
        for (int idx = 0; idx < systemdevicelist.system_devices.size(); ++idx)
        {
            if (systemdevicelist.system_devices[idx].Description == description)
                return systemdevicelist.system_devices[idx].DeviceIndiName.trimmed();
        }
        return QString();
    };

    auto findConnectedIndexBySavedName = [&](const QVector<int> &connectedList, const QString &savedName,
                                             const QSet<int> &reserved = QSet<int>()) -> int
    {
        if (savedName.isEmpty())
            return -1;
        for (int idx : connectedList)
        {
            if (reserved.contains(idx))
                continue;
            if (idx < 0 || idx >= indi_Client->GetDeviceCount())
                continue;
            INDI::BaseDevice *device = indi_Client->GetDeviceFromList(idx);
            if (device == nullptr || device->getDeviceName() == nullptr)
                continue;
            if (QString::fromUtf8(device->getDeviceName()) == savedName)
                return idx;
        }
        return -1;
    };

    bool EachDeviceOne = true;
    bool hasPendingAllocation = false;

    // 「只有一台就绑给该角色」曾在此特判：它是自动决策（系统替用户挑），
    // 且该分支【不上报候选】——封堵它而不删，会让设备既绑不上也选不了。
    // 删除后 1 台与 N 台走同一条路：持久化回放命中就绑（回放用户上次的手动
    // 选择，不是决策），命中不了就作为候选上报、由用户手动指派。
    // 顺带修掉一个 bug：原特判在只有一台时【根本不查持久化】，会静默覆盖
    // 用户上次的选择（上次选 X、这次只有 Y 在场 → 直接绑 Y）。
    if (SelectedCameras.size() >= 1 || ConnectedCCDList.size() >= 1)
    {
        EachDeviceOne = false;
        QSet<int> boundCcdIndexes;

        auto autoBindCcdRole = [&](const QString &description) {
            const int idx = findConnectedIndexBySavedName(ConnectedCCDList,
                                                          savedDeviceNameByDescription(description),
                                                          boundCcdIndexes);
            if (idx < 0)
                return;

            INDI::BaseDevice *device = indi_Client->GetDeviceFromList(idx);
            if (device == nullptr)
                return;

            if (description == "Guider")
            {
                bindDeviceToRole(DeviceSlot::Guider, device);
            }
            else if (description == "PoleCamera")
            {
                bindDeviceToRole(DeviceSlot::PoleCamera, device);
            }
            else if (description == "MainCamera")
            {
                bindDeviceToRole(DeviceSlot::MainCamera, device);
            }

            boundCcdIndexes.insert(idx);
            Logger::Log("continueConnectAllDeviceOnce | INDI CCD auto-bound by saved name: " +
                            description.toStdString() + " -> " + QString::fromUtf8(device->getDeviceName()).toStdString(),
                        LogLevel::INFO, DeviceType::MAIN);
        };

        autoBindCcdRole("MainCamera");
        autoBindCcdRole("Guider");
        autoBindCcdRole("PoleCamera");
        if (dpPoleScope == nullptr)
        {
            // 原先此处按 isPoleMasterName() 认名字直接绑 PoleCamera —— 那是自动决策，
            // 已收敛进 autoDecideDeviceForRole()（当前封堵 -> -1 -> 保持未绑定）。
            // 没绑上的相机会被下面的循环作为候选上报，用户可手动指派，不会成死路。
            QVector<AutoDecisionCandidate> poleCands;
            for (int idx : ConnectedCCDList)
            {
                if (idx < 0 || idx >= indi_Client->GetDeviceCount() || boundCcdIndexes.contains(idx))
                    continue;
                INDI::BaseDevice *device = indi_Client->GetDeviceFromList(idx);
                if (device == nullptr || device->getDeviceName() == nullptr)
                    continue;
                poleCands.append({idx, QString::fromUtf8(device->getDeviceName())});
            }
            const int polePick = autoDecideDeviceForRole("PoleCamera", poleCands);
            if (polePick >= 0)
            {
                INDI::BaseDevice *device = indi_Client->GetDeviceFromList(polePick);
                if (device != nullptr)
                {
                    bindDeviceToRole(DeviceSlot::PoleCamera, device);
                    boundCcdIndexes.insert(polePick);
                }
            }
        }

        for (int i = 0; i < ConnectedCCDList.size(); i++)
        {
            if (boundCcdIndexes.contains(ConnectedCCDList[i]))
                continue;
            // 修复：检查索引是否有效
            if (ConnectedCCDList[i] >= 0 && ConnectedCCDList[i] < indi_Client->GetDeviceCount()) {
                INDI::BaseDevice *device = indi_Client->GetDeviceFromList(ConnectedCCDList[i]);
                if (device != nullptr) {
                    hasPendingAllocation = true;
                    QString devName = QString::fromUtf8(device->getDeviceName());
                    QString devName_cat;
                    if (devName.contains("5III", Qt::CaseInsensitive))
                        devName_cat = "5III";
                    else if (devName.contains("DEMO", Qt::CaseInsensitive))
                        devName_cat = "DEMO";
                    else
                        devName_cat = "OTHER";
                    emit wsThread->sendMessageToClient("DeviceToBeAllocated:CCD:" + QString::number(ConnectedCCDList[i]) + ":" + devName + ":" + devName_cat); // already allocated
                }
            }
        }
    }

    // 「只有一台就绑给该角色」曾在此特判：它是自动决策（系统替用户挑），
    // 且该分支【不上报候选】——封堵它而不删，会让设备既绑不上也选不了。
    // 删除后 1 台与 N 台走同一条路：持久化回放命中就绑（回放用户上次的手动
    // 选择，不是决策），命中不了就作为候选上报、由用户手动指派。
    // 顺带修掉一个 bug：原特判在只有一台时【根本不查持久化】，会静默覆盖
    // 用户上次的选择（上次选 X、这次只有 Y 在场 → 直接绑 Y）。
    if (ConnectedTELESCOPEList.size() >= 1)
    {
        EachDeviceOne = false;
        const int boundMountIndex = findConnectedIndexBySavedName(ConnectedTELESCOPEList, savedDeviceNameByDescription("Mount"));
        if (boundMountIndex >= 0)
        {
            INDI::BaseDevice *device = indi_Client->GetDeviceFromList(boundMountIndex);
            if (device != nullptr)
            {
                bindDeviceToRole(DeviceSlot::Mount, device);
                Logger::Log("continueConnectAllDeviceOnce | INDI Mount auto-bound by saved name: " +
                                QString::fromUtf8(device->getDeviceName()).toStdString(),
                            LogLevel::INFO, DeviceType::MAIN);
            }
        }

        for (int i = 0; i < ConnectedTELESCOPEList.size(); i++)
        {
            if (ConnectedTELESCOPEList[i] == boundMountIndex)
                continue;
            // 修复：检查索引有效性
            if (ConnectedTELESCOPEList[i] >= 0 && ConnectedTELESCOPEList[i] < indi_Client->GetDeviceCount()) {
                INDI::BaseDevice *device = indi_Client->GetDeviceFromList(ConnectedTELESCOPEList[i]);
                if (device != nullptr) {
                    hasPendingAllocation = true;
                    emit wsThread->sendMessageToClient("DeviceToBeAllocated:Mount:" + QString::number(ConnectedTELESCOPEList[i]) + ":" + QString::fromUtf8(device->getDeviceName()));
                }
            }
        }
    }

    // 「只有一台就绑给该角色」曾在此特判：它是自动决策（系统替用户挑），
    // 且该分支【不上报候选】——封堵它而不删，会让设备既绑不上也选不了。
    // 删除后 1 台与 N 台走同一条路：持久化回放命中就绑（回放用户上次的手动
    // 选择，不是决策），命中不了就作为候选上报、由用户手动指派。
    // 顺带修掉一个 bug：原特判在只有一台时【根本不查持久化】，会静默覆盖
    // 用户上次的选择（上次选 X、这次只有 Y 在场 → 直接绑 Y）。
    if (ConnectedFOCUSERList.size() >= 1)
    {
        EachDeviceOne = false;
        const int boundFocuserIndex = findConnectedIndexBySavedName(ConnectedFOCUSERList, savedDeviceNameByDescription("Focuser"));
        if (boundFocuserIndex >= 0)
        {
            INDI::BaseDevice *device = indi_Client->GetDeviceFromList(boundFocuserIndex);
            if (device != nullptr)
            {
                dpFocuser = device;
                if (systemdevicelist.system_devices.size() > 22)
                    systemdevicelist.system_devices[DeviceSlot::Focuser].isConnect = true;
                AfterDeviceConnect(dpFocuser);
                Logger::Log("continueConnectAllDeviceOnce | INDI Focuser auto-bound by saved name: " +
                                QString::fromUtf8(device->getDeviceName()).toStdString(),
                            LogLevel::INFO, DeviceType::MAIN);
            }
        }

        for (int i = 0; i < ConnectedFOCUSERList.size(); i++)
        {
            if (ConnectedFOCUSERList[i] == boundFocuserIndex)
                continue;
            // 修复：检查索引有效性
            if (ConnectedFOCUSERList[i] >= 0 && ConnectedFOCUSERList[i] < indi_Client->GetDeviceCount()) {
                INDI::BaseDevice *device = indi_Client->GetDeviceFromList(ConnectedFOCUSERList[i]);
                if (device != nullptr) {
                    hasPendingAllocation = true;
                    emit wsThread->sendMessageToClient("DeviceToBeAllocated:Focuser:" + QString::number(ConnectedFOCUSERList[i]) + ":" + QString::fromUtf8(device->getDeviceName()));
                }
            }
        }
    }

    // 「只有一台就绑给该角色」曾在此特判：它是自动决策（系统替用户挑），
    // 且该分支【不上报候选】——封堵它而不删，会让设备既绑不上也选不了。
    // 删除后 1 台与 N 台走同一条路：持久化回放命中就绑（回放用户上次的手动
    // 选择，不是决策），命中不了就作为候选上报、由用户手动指派。
    // 顺带修掉一个 bug：原特判在只有一台时【根本不查持久化】，会静默覆盖
    // 用户上次的选择（上次选 X、这次只有 Y 在场 → 直接绑 Y）。
    if (ConnectedFILTERList.size() >= 1)
    {
        EachDeviceOne = false;
        const int boundFilterIndex = findConnectedIndexBySavedName(ConnectedFILTERList, savedDeviceNameByDescription("CFW"));
        if (boundFilterIndex >= 0)
        {
            INDI::BaseDevice *device = indi_Client->GetDeviceFromList(boundFilterIndex);
            if (device != nullptr)
            {
                dpCFW = device;
                if (systemdevicelist.system_devices.size() > 21)
                    systemdevicelist.system_devices[DeviceSlot::CFW].isConnect = true;
                AfterDeviceConnect(dpCFW);
                Logger::Log("continueConnectAllDeviceOnce | INDI CFW auto-bound by saved name: " +
                                QString::fromUtf8(device->getDeviceName()).toStdString(),
                            LogLevel::INFO, DeviceType::MAIN);
            }
        }

        for (int i = 0; i < ConnectedFILTERList.size(); i++)
        {
            if (ConnectedFILTERList[i] == boundFilterIndex)
                continue;
            // 修复：检查索引有效性
            if (ConnectedFILTERList[i] >= 0 && ConnectedFILTERList[i] < indi_Client->GetDeviceCount()) {
                INDI::BaseDevice *device = indi_Client->GetDeviceFromList(ConnectedFILTERList[i]);
                if (device != nullptr) {
                    hasPendingAllocation = true;
                    emit wsThread->sendMessageToClient("DeviceToBeAllocated:CFW:" + QString::number(ConnectedFILTERList[i]) + ":" + QString::fromUtf8(device->getDeviceName()));
                }
            }
        }
    }

    Logger::Log("Each Device Only Has One:" + std::to_string(EachDeviceOne), LogLevel::INFO, DeviceType::MAIN);

    // connectAll 是「按持久化信息自动决策」的静默路径：决策已在上面的 autoBindCcdRole /
    // 各角色自动绑定里做完了。此处唯一该问的是——**有没有角色没能决策出结果**？
    //
    // 原条件是 (!EachDeviceOne && hasPendingAllocation)，问的却是「还有相机没被分配吗」。
    // 但相机有富余是完全正常的（4 台相机、2 个角色 → 剩 2 台备用），这不代表有人在等
    // 用户拿主意。于是「选好相机 → 断开全部 → connect all」这种本该静默的流程也会
    // 弹出下级抽屉（前端收到 ShowDeviceAllocationWindow 就置 drawer_2 = true）。
    //
    // 判据换成「角色」：SelectedCameras 是用户配置过的相机角色，autoBindCcdRole 找不到
    // 对应的持久化设备名时会直接 return、该角色的 dp 保持 nullptr —— 那才是自动决策没
    // 决策出来、需要回退到问用户的情形。
    //
    // 仍保留 hasPendingAllocation：没有候选可选时弹窗没有意义。两条合起来是对原条件的
    // 严格收窄——只会比原来少弹，不会在原本不弹的路径上多弹。
    bool hasUnresolvedRole = false;
    QStringList unresolvedRoles;
    for (const QString &role : SelectedCameras)
    {
        const bool unresolved = (role == "MainCamera" && dpMainCamera == nullptr) ||
                                (role == "Guider" && dpGuider == nullptr) ||
                                (role == "PoleCamera" && dpPoleScope == nullptr);
        if (unresolved)
        {
            hasUnresolvedRole = true;
            unresolvedRoles << role;
        }
    }
    if (!ConnectedFILTERList.empty() && dpCFW == nullptr)
    {
        hasUnresolvedRole = true;
        unresolvedRoles << "CFW";
    }

    if (hasUnresolvedRole && hasPendingAllocation)
    {
        Logger::Log("continueConnectAllDeviceOnce | roles unresolved by saved config: " +
                        unresolvedRoles.join(",").toStdString() + " -> ask user to allocate",
                    LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ShowDeviceAllocationWindow");
    }
    else
    {
        Logger::Log("continueConnectAllDeviceOnce | all roles resolved by saved config, silent connect (no allocation window)",
                    LogLevel::INFO, DeviceType::MAIN);
    }
    
    // 发送全部连接完成消息，通知前端可以关闭进度条
    emit wsThread->sendMessageToClient("ConnectAllDeviceComplete");
    Logger::Log("continueConnectAllDeviceOnce | All devices connection process completed", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::BindingDevice(QString DeviceType, int DeviceIndex)
{
    if (indi_Client)
        indi_Client->PrintDevices();
    Logger::Log("BindingDevice:" + DeviceType.toStdString() + ":" + QString::number(DeviceIndex).toStdString(), LogLevel::INFO, DeviceType::MAIN);

    const auto refreshBindUi = [this]() {
        loadBindDeviceTypeList();
        loadBindDeviceList(indi_Client);
    };
    const auto boundNameOf = [](INDI::BaseDevice *dev) -> QString {
        return dev ? QString::fromUtf8(dev->getDeviceName()) : QString();
    };
    const auto registerSdkRole = [this](const QString &role, SdkDeviceHandle handle, const QString &deviceName) {
        if (handle == nullptr) return;
        const QString driverName = getSDKDriverName(role);
        if (driverName.isEmpty())
        {
            Logger::Log("BindingDevice | Cannot get SDK driver name for " + role.toStdString(),
                        LogLevel::ERROR, DeviceType::MAIN);
            return;
        }
        const std::string description =
            (role == "MainCamera") ? "主相机" :
            (role == "Guider") ? "导星相机" :
            (role == "PoleCamera") ? "电子极轴镜" :
            role.toStdString();
        SdkResult regRes = SdkManager::instance().registerDevice(
            driverName.toStdString(),
            role.toStdString(),
            handle,
            description,
            std::any(deviceName.toStdString())
        );
        if (!regRes.success)
        {
            Logger::Log("BindingDevice | Failed to register SDK role " + role.toStdString() + ": " + regRes.message,
                        LogLevel::WARNING, DeviceType::MAIN);
        }
    };
    const auto clearSdkRoleBinding = [this](const QString &role) {
        int systemIndex = -1;
        if (role == "MainCamera")
        {
            g_sdkMainCameraPoolIndex = -1;
            sdkMainCameraHandle = nullptr;
            sdkMainCameraId.clear();
            systemIndex = 20;
        }
        else if (role == "Guider")
        {
            g_sdkGuiderPoolIndex = -1;
            sdkGuiderHandle = nullptr;
            systemIndex = 1;
        }
        else if (role == "PoleCamera")
        {
            g_sdkPoleCameraPoolIndex = -1;
            sdkPoleScopeHandle = nullptr;
            systemIndex = 2;
        }

        if (systemIndex >= 0 && systemdevicelist.system_devices.size() > systemIndex)
        {
            systemdevicelist.system_devices[systemIndex].isConnect = false;
            systemdevicelist.system_devices[systemIndex].isBind = false;
            systemdevicelist.system_devices[systemIndex].DeviceIndiName.clear();
        }
    };
    const auto sdkOccupantRoleOfPoolIndex = [this](int poolIndex, const QString &excludeRole = QString()) -> QString {
        if (poolIndex == g_sdkMainCameraPoolIndex && excludeRole != "MainCamera")
            return "MainCamera";
        if (poolIndex == g_sdkGuiderPoolIndex && excludeRole != "Guider")
            return "Guider";
        if (poolIndex == g_sdkPoleCameraPoolIndex && excludeRole != "PoleCamera")
            return "PoleCamera";
        return QString();
    };

    // ==================== SDK 多相机分配：复用 BindingDevice 协议 ====================
    // 仅当 MainCamera 槽位标记为 SDK 连接时启用（其它角色后续按需扩展）
    if (DeviceType == "MainCamera" &&
        systemdevicelist.system_devices.size() > 20 &&
        systemdevicelist.system_devices[DeviceSlot::MainCamera].isSDKConnect)
    {
        if (DeviceIndex >= 0)
        {
            Logger::Log("BindingDevice | MainCamera is SDK mode but got non-negative DeviceIndex (expected negative index for SDK pool): " +
                            std::to_string(DeviceIndex),
                        LogLevel::WARNING, DeviceType::MAIN);
        }
        const int poolIndex = sdkPoolIndexFromUiIndex(DeviceIndex);
        if (!sdkPoolIndexValid(poolIndex))
        {
            Logger::Log("BindingDevice | SDK pool index invalid for MainCamera. uiIndex=" + std::to_string(DeviceIndex) +
                            " poolIndex=" + std::to_string(poolIndex),
                        LogLevel::ERROR, DeviceType::MAIN);
            return;
        }

        const QString occupiedRole = sdkOccupantRoleOfPoolIndex(poolIndex, "MainCamera");
        if (!occupiedRole.isEmpty())
        {
            Logger::Log("BindingDevice | Reassign SDK camera to MainCamera and clear previous role " +
                            occupiedRole.toStdString() + ". target poolIndex=" + std::to_string(poolIndex),
                        LogLevel::INFO, DeviceType::MAIN);
            clearSdkRoleBinding(occupiedRole);
        }

        g_sdkMainCameraPoolIndex = poolIndex;
        // M2：池中该槽位可能尚未打开（扫描只枚举不 open），此刻按需打开。
        if (ensureSdkCameraOpen(poolIndex, "MainCamera") == nullptr)
        {
            Logger::Log("BindingDevice | open MainCamera failed for poolIndex=" + std::to_string(poolIndex),
                        LogLevel::ERROR, DeviceType::MAIN);
            return;
        }
        sdkMainCameraHandle = g_sdkQhyCamHandles[poolIndex];
        sdkMainCameraId = g_sdkQhyCamIds[poolIndex];

        // 只设置 isConnect = true，isBind 应该由 AfterDeviceConnect 在完成初始化后设置
        // 这样可以确保 AfterDeviceConnect 中的 SDK 初始化流程能够执行（检查条件为 !isBind），
        // 并在初始化完成后发送 ConnectSuccess 消息给前端
        systemdevicelist.system_devices[DeviceSlot::MainCamera].isConnect = true;
        systemdevicelist.system_devices[DeviceSlot::MainCamera].isBind = false;
        systemdevicelist.system_devices[DeviceSlot::MainCamera].isSDKConnect = true;
        systemdevicelist.system_devices[DeviceSlot::MainCamera].Description = "MainCamera";
        // 记录选择的相机 ID，便于下次自动重连/区分多相机
        systemdevicelist.system_devices[DeviceSlot::MainCamera].DeviceIndiName = sdkMainCameraId;

        // 将设备注册到 SdkManager 的设备注册表，以便 callByHandle 和 closeByHandle 能够找到设备
        QString driverName = getSDKDriverName("MainCamera");
        if (!driverName.isEmpty() && sdkMainCameraHandle != nullptr)
        {
            SdkResult regRes = SdkManager::instance().registerDevice(
                driverName.toStdString(),
                "MainCamera",
                sdkMainCameraHandle,
                "主相机",
                std::any(sdkMainCameraId.toStdString())
            );
            if (!regRes.success)
            {
                Logger::Log("AllocateDevice | Failed to register MainCamera to SdkManager: " + regRes.message,
                            LogLevel::WARNING, DeviceType::CAMERA);
            }
        }
        // 注意：DriverIndiName 语义为“驱动名”（例如 indi_qhy_ccd），不能被 cameraId 覆盖；
        // SDK 相机的唯一标识（cameraId）只写入 DeviceIndiName。
        if (!systemdevicelist.system_devices[DeviceSlot::MainCamera].DriverFrom.contains("SDK", Qt::CaseInsensitive)) {
            systemdevicelist.system_devices[DeviceSlot::MainCamera].DriverFrom = "SDK";
        }
        Tools::saveSystemDeviceList(systemdevicelist);

        Logger::Log("BindingDevice | Bind SDK MainCamera success: " + sdkMainCameraId.toStdString() +
                        " (poolIndex=" + std::to_string(poolIndex) + ")",
                    LogLevel::INFO, DeviceType::MAIN);

        // 复用 SDK 初始化流程，AfterDeviceConnect 会完成初始化并设置 isBind = true，同时发送 ConnectSuccess 消息给前端
        AfterDeviceConnect(nullptr);
        refreshBindUi();
        return;
    }

    // ==================== SDK 多相机分配：Guider ====================
    if (DeviceType == "Guider" &&
        systemdevicelist.system_devices.size() > 1 &&
        systemdevicelist.system_devices[DeviceSlot::Guider].isSDKConnect)
    {
        const int poolIndex = sdkPoolIndexFromUiIndex(DeviceIndex);
        if (!sdkPoolIndexValid(poolIndex))
        {
            Logger::Log("BindingDevice | SDK pool index invalid for Guider. uiIndex=" + std::to_string(DeviceIndex) +
                            " poolIndex=" + std::to_string(poolIndex),
                        LogLevel::ERROR, DeviceType::GUIDER);
            return;
        }

        const QString occupiedRole = sdkOccupantRoleOfPoolIndex(poolIndex, "Guider");
        if (!occupiedRole.isEmpty())
        {
            Logger::Log("BindingDevice | Reassign SDK camera to Guider and clear previous role " +
                            occupiedRole.toStdString() + ". target poolIndex=" + std::to_string(poolIndex),
                        LogLevel::INFO, DeviceType::GUIDER);
            clearSdkRoleBinding(occupiedRole);
        }

        g_sdkGuiderPoolIndex = poolIndex;
        // M2：池中该槽位可能尚未打开（扫描只枚举不 open），此刻按需打开。
        if (ensureSdkCameraOpen(poolIndex, "Guider") == nullptr)
        {
            Logger::Log("BindingDevice | open Guider failed for poolIndex=" + std::to_string(poolIndex),
                        LogLevel::ERROR, DeviceType::GUIDER);
            return;
        }
        sdkGuiderHandle = g_sdkQhyCamHandles[poolIndex];

        const QString guiderId = g_sdkQhyCamIds[poolIndex];

        systemdevicelist.system_devices[DeviceSlot::Guider].isConnect = true;
        systemdevicelist.system_devices[DeviceSlot::Guider].isBind = false;
        systemdevicelist.system_devices[DeviceSlot::Guider].isSDKConnect = true;
        systemdevicelist.system_devices[DeviceSlot::Guider].Description = "Guider";
        systemdevicelist.system_devices[DeviceSlot::Guider].DeviceIndiName = guiderId;
        if (!systemdevicelist.system_devices[DeviceSlot::Guider].DriverFrom.contains("SDK", Qt::CaseInsensitive))
            systemdevicelist.system_devices[DeviceSlot::Guider].DriverFrom = "SDK";
        Tools::saveSystemDeviceList(systemdevicelist);

        // 注册设备到 SdkManager
        QString driverName = getSDKDriverName("Guider");
        if (!driverName.isEmpty() && sdkGuiderHandle != nullptr)
        {
            SdkResult regRes = SdkManager::instance().registerDevice(
                driverName.toStdString(),
                "Guider",
                sdkGuiderHandle,
                "导星相机",
                std::any(guiderId.toStdString())
            );
            if (!regRes.success)
            {
                Logger::Log("BindingDevice | Failed to register Guider to SdkManager: " + regRes.message,
                            LogLevel::WARNING, DeviceType::GUIDER);
            }
        }
        Logger::Log("BindingDevice | Bind SDK Guider success: " + guiderId.toStdString() +
                        " (poolIndex=" + std::to_string(poolIndex) + ")",
                    LogLevel::INFO, DeviceType::GUIDER);

        // AfterDeviceConnect 负责完成 SDK 初始化并设置 isBind，同时发送 ConnectSuccess
        AfterDeviceConnect(nullptr);
        refreshBindUi();
        return;
    }

    // ==================== SDK 多相机分配：PoleCamera ====================
    if (DeviceType == "PoleCamera" &&
        systemdevicelist.system_devices.size() > 2 &&
        systemdevicelist.system_devices[DeviceSlot::PoleCamera].isSDKConnect)
    {
        const int poolIndex = sdkPoolIndexFromUiIndex(DeviceIndex);
        if (!sdkPoolIndexValid(poolIndex))
        {
            Logger::Log("BindingDevice | SDK pool index invalid for PoleCamera. uiIndex=" + std::to_string(DeviceIndex) +
                            " poolIndex=" + std::to_string(poolIndex),
                        LogLevel::ERROR, DeviceType::MAIN);
            return;
        }

        const QString occupiedRole = sdkOccupantRoleOfPoolIndex(poolIndex, "PoleCamera");
        if (!occupiedRole.isEmpty())
        {
            Logger::Log("BindingDevice | Reassign SDK camera to PoleCamera and clear previous role " +
                            occupiedRole.toStdString() + ". target poolIndex=" + std::to_string(poolIndex),
                        LogLevel::INFO, DeviceType::MAIN);
            clearSdkRoleBinding(occupiedRole);
        }

        g_sdkPoleCameraPoolIndex = poolIndex;
        // M2：池中该槽位可能尚未打开（扫描只枚举不 open），此刻按需打开。
        if (ensureSdkCameraOpen(poolIndex, "PoleCamera") == nullptr)
        {
            Logger::Log("BindingDevice | open PoleCamera failed for poolIndex=" + std::to_string(poolIndex),
                        LogLevel::ERROR, DeviceType::MAIN);
            return;
        }
        sdkPoleScopeHandle = g_sdkQhyCamHandles[poolIndex];

        const QString poleId = g_sdkQhyCamIds[poolIndex];
        systemdevicelist.system_devices[DeviceSlot::PoleCamera].isConnect = true;
        systemdevicelist.system_devices[DeviceSlot::PoleCamera].isBind = false;
        systemdevicelist.system_devices[DeviceSlot::PoleCamera].isSDKConnect = true;
        systemdevicelist.system_devices[DeviceSlot::PoleCamera].Description = "PoleCamera";
        systemdevicelist.system_devices[DeviceSlot::PoleCamera].DeviceIndiName = poleId;
        if (!systemdevicelist.system_devices[DeviceSlot::PoleCamera].DriverFrom.contains("SDK", Qt::CaseInsensitive))
            systemdevicelist.system_devices[DeviceSlot::PoleCamera].DriverFrom = "SDK";
        Tools::saveSystemDeviceList(systemdevicelist);

        registerSdkRole("PoleCamera", sdkPoleScopeHandle, poleId);
        Logger::Log("BindingDevice | Bind SDK PoleCamera success: " + poleId.toStdString() +
                        " (poolIndex=" + std::to_string(poolIndex) + ")",
                    LogLevel::INFO, DeviceType::MAIN);

        AfterDeviceConnect(nullptr);
        refreshBindUi();
        return;
    }

    // ==================== SDK 电调(Focuser)绑定：复用 BindingDevice 协议 ====================
    // 说明：
    // - SDK 电调不在 INDI 设备列表里，但仍需要通过设备分配面板执行 Bind/Unbind
    // - 使用固定负数 index 作为 SDK 电调的标识（见 SDK_FOCUSER_UI_INDEX）
    if (DeviceType == "Focuser" &&
        systemdevicelist.system_devices.size() > 22 &&
        systemdevicelist.system_devices[DeviceSlot::Focuser].isSDKConnect)
    {
        if (DeviceIndex != SDK_FOCUSER_UI_INDEX)
        {
            Logger::Log("BindingDevice | Focuser is SDK mode but got unexpected DeviceIndex=" + std::to_string(DeviceIndex) +
                            " (expected " + std::to_string(SDK_FOCUSER_UI_INDEX) + ")",
                        LogLevel::WARNING, DeviceType::FOCUSER);
            // 兼容旧前端/旧协议：仍允许继续（避免旧版传 0 导致无法绑定）
        }

        // 若句柄已存在，直接走初始化；否则尝试按“已选串口/已保存/自动识别”重新打开
        if (sdkFocuserHandle == nullptr)
        {
            QString portToUse;
            // 1) 优先用之前缓存的串口（例如 ConnectAllDeviceOnce 已探测到）
            if (!sdkFocuserPort.isEmpty())
            {
                portToUse = sdkFocuserPort;
            }
            // 2) 其次使用系统设备表中保存的 DeviceIndiName（若是 /dev/xxx）
            else if (!systemdevicelist.system_devices[DeviceSlot::Focuser].DeviceIndiName.isEmpty() &&
                     systemdevicelist.system_devices[DeviceSlot::Focuser].DeviceIndiName.startsWith("/dev/"))
            {
                portToUse = systemdevicelist.system_devices[DeviceSlot::Focuser].DeviceIndiName;
            }
            // 3) 最后自动识别
            else
            {
                portToUse = detector.getFocuserPort();
            }

            if (portToUse.isEmpty())
            {
                Logger::Log("BindingDevice | SDK focuser port not found, cannot bind.", LogLevel::ERROR, DeviceType::FOCUSER);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:Focuser:SDK focuser port not found");
                return;
            }

            // 打开串口
            Logger::Log("ConnectDriver | SDK focuser using port: " + portToUse.toStdString() +
                            ", baud: " + std::to_string(systemdevicelist.system_devices[DeviceSlot::Focuser].BaudRate),
                        LogLevel::INFO, DeviceType::FOCUSER);
            SdkFocuserOpenParam p;
            p.port = portToUse.toStdString();
            p.baudRate = systemdevicelist.system_devices[DeviceSlot::Focuser].BaudRate;
            p.timeoutMs = 3000;

            QString driverName = getSDKDriverName("Focuser");
            if (driverName.isEmpty())
            {
                Logger::Log("BindingDevice | Cannot get SDK driver name for Focuser", LogLevel::ERROR, DeviceType::FOCUSER);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:Focuser:Cannot get SDK driver name");
                return;
            }

            SdkResult openRes = SdkManager::instance().open(driverName.toStdString(), p);
            if (!openRes.success || !openRes.payload.has_value())
            {
                Logger::Log("BindingDevice | SDK focuser open failed: " + openRes.message, LogLevel::ERROR, DeviceType::FOCUSER);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:Focuser:SDK focuser open failed");
                return;
            }

            sdkFocuserHandle = std::any_cast<SdkDeviceHandle>(openRes.payload);
            sdkFocuserPort = portToUse;

            // 注册到 SdkManager（便于 callByHandle/closeByHandle 正常工作）
            SdkResult regRes = SdkManager::instance().registerDevice(
                driverName.toStdString(),
                "Focuser",
                sdkFocuserHandle,
                "QHY Focuser");
            if (!regRes.success)
            {
                Logger::Log("BindingDevice | SDK focuser register failed: " + regRes.message, LogLevel::ERROR, DeviceType::FOCUSER);
                SdkManager::instance().closeByHandle(sdkFocuserHandle);
                sdkFocuserHandle = nullptr;
                sdkFocuserPort.clear();
                emit wsThread->sendMessageToClient("ConnectDriverFailed:Focuser:SDK focuser register failed");
                return;
            }
        }

        // 标记已连接，触发 AfterDeviceConnect(nullptr) 走 SDK 电调初始化并最终设置 isBind
        systemdevicelist.system_devices[DeviceSlot::Focuser].isConnect = true;
        // 重要：此处不提前置 isBind，避免 AfterDeviceConnect 的 “!isBind” 初始化条件失效
        systemdevicelist.system_devices[DeviceSlot::Focuser].isBind = false;
        if (!systemdevicelist.system_devices[DeviceSlot::Focuser].DriverFrom.contains("SDK", Qt::CaseInsensitive))
            systemdevicelist.system_devices[DeviceSlot::Focuser].DriverFrom = "SDK";
        Tools::saveSystemDeviceList(systemdevicelist);

        Logger::Log("BindingDevice | Bind SDK Focuser requested. port=" + sdkFocuserPort.toStdString(),
                    LogLevel::INFO, DeviceType::FOCUSER);
        AfterDeviceConnect(nullptr);
        return;
    }

    // ==================== INDI 分配（原逻辑）====================
    if (!indi_Client)
    {
        Logger::Log("BindingDevice | indi_Client is nullptr", LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    // 修复：检查DeviceIndex是否有效
    if (DeviceIndex < 0 || DeviceIndex >= indi_Client->GetDeviceCount()) {
        Logger::Log("BindingDevice | Invalid DeviceIndex: " + std::to_string(DeviceIndex), LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    INDI::BaseDevice *device = indi_Client->GetDeviceFromList(DeviceIndex);
    if (device == nullptr) {
        Logger::Log("BindingDevice | GetDeviceFromList returned nullptr for DeviceIndex: " + std::to_string(DeviceIndex), LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    // 相机角色互斥：若用户把某台相机重新分配给另一角色，则旧角色被清空，不再做隐式 swap。
    const QStringList swappableCameraRoles = {"MainCamera", "Guider", "PoleCamera"};
    const auto roleSystemIndex = [](const QString &role) -> int {
        if (role == "MainCamera") return 20;
        if (role == "Guider") return 1;
        if (role == "PoleCamera") return 2;
        return -1;
    };
    const auto getRoleDevicePtr = [&](const QString &role) -> INDI::BaseDevice * {
        if (role == "MainCamera") return dpMainCamera;
        if (role == "Guider") return dpGuider;
        if (role == "PoleCamera") return dpPoleScope;
        return nullptr;
    };
    const auto setRoleDevicePtr = [&](const QString &role, INDI::BaseDevice *ptr) {
        if (role == "MainCamera") dpMainCamera = ptr;
        else if (role == "Guider") dpGuider = ptr;
        else if (role == "PoleCamera") dpPoleScope = ptr;
    };
    const auto syncRoleBindState = [&](const QString &role) {
        const int idx = roleSystemIndex(role);
        if (idx < 0 || systemdevicelist.system_devices.size() <= idx) return;
        INDI::BaseDevice *ptr = getRoleDevicePtr(role);
        systemdevicelist.system_devices[idx].isConnect = (ptr != nullptr);
        systemdevicelist.system_devices[idx].isBind = (ptr != nullptr);
        systemdevicelist.system_devices[idx].DeviceIndiName = boundNameOf(ptr);
    };

    if (swappableCameraRoles.contains(DeviceType))
    {
        QString occupantRole;
        for (const QString &role : swappableCameraRoles)
        {
            if (role == DeviceType) continue;
            if (getRoleDevicePtr(role) == device)
            {
                occupantRole = role;
                break;
            }
        }

        if (!occupantRole.isEmpty())
        {
            Logger::Log("BindingDevice | Reassign INDI camera to " + DeviceType.toStdString() +
                            " and clear previous role " + occupantRole.toStdString(),
                        LogLevel::INFO, DeviceType::MAIN);
            setRoleDevicePtr(occupantRole, nullptr);
            syncRoleBindState(occupantRole);
        }
    }
    
    if (DeviceType == "Guider")
    {
        Logger::Log("Binding Guider Device start ...", LogLevel::INFO, DeviceType::MAIN);
        dpGuider = device;
        if (systemdevicelist.system_devices.size() > 1) {
            systemdevicelist.system_devices[DeviceSlot::Guider].isConnect = true;
            systemdevicelist.system_devices[DeviceSlot::Guider].isBind = true;
        }
        Tools::saveSystemDeviceList(systemdevicelist);
        AfterDeviceConnect(dpGuider);
        refreshBindUi();
        Logger::Log("Binding Guider Device end !", LogLevel::INFO, DeviceType::MAIN);
    }
    else if (DeviceType == "MainCamera")
    {
        Logger::Log("Binding MainCamera Device start ...", LogLevel::INFO, DeviceType::MAIN);
        dpMainCamera = device;
        if (systemdevicelist.system_devices.size() > 20) {
            systemdevicelist.system_devices[DeviceSlot::MainCamera].isConnect = true;
            systemdevicelist.system_devices[DeviceSlot::MainCamera].isBind = true;
        }
        Tools::saveSystemDeviceList(systemdevicelist);
        AfterDeviceConnect(dpMainCamera);
        refreshBindUi();
        Logger::Log("Binding MainCamera Device end !", LogLevel::INFO, DeviceType::MAIN);
    }
    else if (DeviceType == "Mount")
    {
        Logger::Log("Binding Mount Device start ...", LogLevel::INFO, DeviceType::MAIN);
        dpMount = device;
        if (systemdevicelist.system_devices.size() > 0) {
            systemdevicelist.system_devices[DeviceSlot::Mount].isConnect = true;
            systemdevicelist.system_devices[DeviceSlot::Mount].isBind = true;
        }
        Tools::saveSystemDeviceList(systemdevicelist);
        AfterDeviceConnect(dpMount);
        Logger::Log("Binding Mount Device end !", LogLevel::INFO, DeviceType::MAIN);
    }
    else if (DeviceType == "Focuser")
    {
        Logger::Log("Binding Focuser Device start ...", LogLevel::INFO, DeviceType::MAIN);
        dpFocuser = device;
        if (systemdevicelist.system_devices.size() > 22) {
            systemdevicelist.system_devices[DeviceSlot::Focuser].isConnect = true;
            systemdevicelist.system_devices[DeviceSlot::Focuser].isBind = true;
        }
        Tools::saveSystemDeviceList(systemdevicelist);
        AfterDeviceConnect(dpFocuser);
        Logger::Log("Binding Focuser Device end !", LogLevel::INFO, DeviceType::MAIN);
    }
    else if (DeviceType == "PoleCamera")
    {
        Logger::Log("Binding PoleCamera Device start ...", LogLevel::INFO, DeviceType::MAIN);
        // 修复：使用已检查的device指针
        dpPoleScope = device;
        if (systemdevicelist.system_devices.size() > 2) {
            systemdevicelist.system_devices[DeviceSlot::PoleCamera].isConnect = true;
            systemdevicelist.system_devices[DeviceSlot::PoleCamera].isBind = true;
        }
        Tools::saveSystemDeviceList(systemdevicelist);
        AfterDeviceConnect(dpPoleScope);
        Logger::Log("Binding PoleCamera Device end !", LogLevel::INFO, DeviceType::MAIN);
    }
    else if (DeviceType == "CFW")
    {
        Logger::Log("Binding CFW Device start ...", LogLevel::INFO, DeviceType::MAIN);
        dpCFW = indi_Client->GetDeviceFromList(DeviceIndex);
        systemdevicelist.system_devices[DeviceSlot::CFW].isConnect = true;
        systemdevicelist.system_devices[DeviceSlot::CFW].isBind = true;
        Tools::saveSystemDeviceList(systemdevicelist);
        AfterDeviceConnect(dpCFW);
        Logger::Log("Binding CFW Device end !", LogLevel::INFO, DeviceType::MAIN);
    }
}
void MainWindow::UnBindingDevice(QString DeviceType)
{
    if (indi_Client)
        indi_Client->PrintDevices();
    Logger::Log("UnBindingDevice:" + DeviceType.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    if (DeviceType == "Guider")
    {
        Logger::Log("UnBinding Guider Device start ...", LogLevel::INFO, DeviceType::MAIN);

        emit wsThread->sendMessageToClient("GuiderSwitchStatus:false");
        isGuiderLoopExp = false;
        guiderExposureInFlight = false;
        if (guiderLoopTimer)
            guiderLoopTimer->stop();
        if (sdkGuiderExposureTimer)
            sdkGuiderExposureTimer->stop();

        // SDK 模式：解绑仅清理角色绑定，不关闭池句柄
        if (systemdevicelist.system_devices.size() > 1 && systemdevicelist.system_devices[DeviceSlot::Guider].isSDKConnect)
        {
            systemdevicelist.system_devices[DeviceSlot::Guider].isBind = false;
            systemdevicelist.system_devices[DeviceSlot::Guider].DeviceIndiName.clear();
            dpGuider = nullptr;

            if (g_sdkGuiderPoolIndex >= 0 && sdkPoolIndexValid(g_sdkGuiderPoolIndex))
            {
                const int uiIdx = sdkUiIndexFromPoolIndex(g_sdkGuiderPoolIndex);
                const QString &camId = g_sdkQhyCamIds[g_sdkGuiderPoolIndex];
                QString camId_cat;
                if (camId.contains("5III", Qt::CaseInsensitive))
                    camId_cat = "5III";
                else if (camId.contains("DEMO", Qt::CaseInsensitive))
                    camId_cat = "DEMO";
                else
                    camId_cat = "OTHER";
                emit wsThread->sendMessageToClient("DeviceToBeAllocated:CCD:" + QString::number(uiIdx) + ":" + camId + ":" + camId_cat);
            }

            g_sdkGuiderPoolIndex = -1;
            sdkGuiderHandle = nullptr;

            emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
            emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
            Tools::saveSystemDeviceList(systemdevicelist);
            Logger::Log("UnBinding Guider (SDK) end.", LogLevel::INFO, DeviceType::MAIN);
            return;
        }

        // INDI 模式：解绑时停止 INDI 导星循环曝光与曝光
        if (indi_Client && dpGuider)
            indi_Client->setCCDAbortExposure(dpGuider);
        emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");

        if (!dpGuider)
        {
            Logger::Log("UnBinding Guider Device skipped: dpGuider is nullptr", LogLevel::WARNING, DeviceType::MAIN);
            return;
        }

        indi_Client->disconnectDevice(dpGuider->getDeviceName());
        Logger::Log("Disconnect Guider Device", LogLevel::INFO, DeviceType::MAIN);
        sleep(1);
        indi_Client->setBaudRate(dpGuider, systemdevicelist.system_devices[DeviceSlot::Guider].BaudRate);
        indi_Client->connectDevice(dpGuider->getDeviceName());
        Logger::Log("Connect Guider Device", LogLevel::INFO, DeviceType::MAIN);
        sleep(3);
        int DeviceIndex = -1;
        for (int i = 0; i < indi_Client->GetDeviceCount(); i++)
        {
            if (indi_Client->GetDeviceFromList(i)->getDeviceName() == dpGuider->getDeviceName())
            {
                DeviceIndex = i;
            }
        }
        systemdevicelist.system_devices[DeviceSlot::Guider].isBind = false;
        systemdevicelist.system_devices[DeviceSlot::Guider].DeviceIndiName = "";
        dpGuider = nullptr;
        Tools::saveSystemDeviceList(systemdevicelist);
        Logger::Log("UnBinding Guider Device end !", LogLevel::INFO, DeviceType::MAIN);
        if (DeviceIndex >= 0 && DeviceIndex < indi_Client->GetDeviceCount())
        {
            QString devName = QString::fromUtf8(indi_Client->GetDeviceFromList(DeviceIndex)->getDeviceName());
            QString devName_cat;
            if (devName.contains("5III", Qt::CaseInsensitive))
                devName_cat = "5III";
            else if (devName.contains("DEMO", Qt::CaseInsensitive))
                devName_cat = "DEMO";
            else
                devName_cat = "OTHER";
            emit wsThread->sendMessageToClient("DeviceToBeAllocated:CCD:" + QString::number(DeviceIndex) + ":" + devName + ":" + devName_cat);
        }
    }
    else if (DeviceType == "MainCamera")
    {
        // SDK 模式：不依赖 dpMainCamera/INDI 设备列表
        if (systemdevicelist.system_devices.size() > 20 && systemdevicelist.system_devices[DeviceSlot::MainCamera].isSDKConnect)
        {
            Logger::Log("UnBinding MainCamera (SDK) start ...", LogLevel::INFO, DeviceType::MAIN);

            // 如果当前 CFW 是“相机内置”，解绑主相机时必须一并清理前端的 CFW 设备与 UI，
            // 否则切换/重新绑定其它相机后前端仍会保留滤镜控制入口，造成“残留可控”的错觉/误操作。
            if (isFilterOnCamera)
            {
                isFilterOnCamera = false;
                sdkMainCfwSlotsCached = 0;
                emit wsThread->sendMessageToClient("deleteDeviceTypeAllocationList:CFW");
            }

            // 解除绑定：仅清理角色绑定，不关闭句柄池（保持待分配列表可用）
            systemdevicelist.system_devices[DeviceSlot::MainCamera].isBind = false;
            systemdevicelist.system_devices[DeviceSlot::MainCamera].DeviceIndiName.clear();
            dpMainCamera = nullptr;

            // 将当前绑定的 SDK 相机重新放回待分配列表
            if (g_sdkMainCameraPoolIndex >= 0 && sdkPoolIndexValid(g_sdkMainCameraPoolIndex))
            {
                const int uiIdx = sdkUiIndexFromPoolIndex(g_sdkMainCameraPoolIndex);
                const QString &camId = g_sdkQhyCamIds[g_sdkMainCameraPoolIndex];
                QString camId_cat;
                if (camId.contains("5III", Qt::CaseInsensitive))
                    camId_cat = "5III";
                else if (camId.contains("DEMO", Qt::CaseInsensitive))
                    camId_cat = "DEMO";
                else
                    camId_cat = "OTHER";
                emit wsThread->sendMessageToClient("DeviceToBeAllocated:CCD:" + QString::number(uiIdx) + ":" + camId + ":" + camId_cat);
            }

            g_sdkMainCameraPoolIndex = -1;
            sdkMainCameraHandle = nullptr;
            // sdkMainCameraId 保留最后一次选择，便于自动重连/CFW key（不影响连接判断）

            Tools::saveSystemDeviceList(systemdevicelist);
            Logger::Log("UnBinding MainCamera (SDK) end.", LogLevel::INFO, DeviceType::MAIN);
            return;
        }

        Logger::Log("UnBinding MainCamera Device(" + std::string(dpMainCamera->getDeviceName()) + ") start ...", LogLevel::INFO, DeviceType::MAIN);

        int DeviceIndex;
        for (int i = 0; i < indi_Client->GetDeviceCount(); i++) //  indi_Client->GetDeviceFromList(i)
        {
            if (indi_Client->GetDeviceFromList(i)->getDeviceName() == dpMainCamera->getDeviceName())
            {
                DeviceIndex = i;
            }
        }

        QString CFWname;
        indi_Client->getCFWSlotName(dpMainCamera, CFWname);
        if (CFWname != "")
        {
            emit wsThread->sendMessageToClient("deleteDeviceTypeAllocationList:CFW");
        }
        systemdevicelist.system_devices[DeviceSlot::MainCamera].isBind = false;
        systemdevicelist.system_devices[DeviceSlot::MainCamera].DeviceIndiName = "";
        dpMainCamera = nullptr;
        Tools::saveSystemDeviceList(systemdevicelist);

        QString devName = QString::fromUtf8(indi_Client->GetDeviceFromList(DeviceIndex)->getDeviceName());
        QString devName_cat;
        if (devName.contains("5III", Qt::CaseInsensitive))
            devName_cat = "5III";
        else if (devName.contains("DEMO", Qt::CaseInsensitive))
            devName_cat = "DEMO";
        else
            devName_cat = "OTHER";
        emit wsThread->sendMessageToClient("DeviceToBeAllocated:CCD:" + QString::number(DeviceIndex) + ":" + devName + ":" + devName_cat); // already allocated
    }
    else if (DeviceType == "Mount")
    {
        int DeviceIndex;
        for (int i = 0; i < indi_Client->GetDeviceCount(); i++) //  indi_Client->GetDeviceFromList(i)
        {
            if (indi_Client->GetDeviceFromList(i)->getDeviceName() == dpMount->getDeviceName())
            {
                DeviceIndex = i;
            }
        }
        systemdevicelist.system_devices[DeviceSlot::Mount].isBind = false;
        systemdevicelist.system_devices[DeviceSlot::Mount].DeviceIndiName = "";
        dpMount = nullptr;
        Tools::saveSystemDeviceList(systemdevicelist);

        emit wsThread->sendMessageToClient("DeviceToBeAllocated:Mount:" + QString::number(DeviceIndex) + ":" + QString::fromUtf8(indi_Client->GetDeviceFromList(DeviceIndex)->getDeviceName()));
    }
    else if (DeviceType == "Focuser")
    {
        // SDK 模式：不依赖 dpFocuser/INDI 设备列表
        if (systemdevicelist.system_devices.size() > 22 && systemdevicelist.system_devices[DeviceSlot::Focuser].isSDKConnect)
        {
            Logger::Log("UnBinding Focuser (SDK) start ...", LogLevel::INFO, DeviceType::FOCUSER);

            // 解除绑定（SDK）：
            // 与主相机/导星镜一致，解绑后仍保留“已连接”状态与句柄，
            // 这样刷新后 loadBindDeviceTypeList 仍会下发 Focuser 类型，前端卡片不会消失。
            // 若确实需要“断开电调”，应走 DisconnectDriver/DisconnectAllDevice 流程，而不是 Unbind。
            systemdevicelist.system_devices[DeviceSlot::Focuser].isBind = false;
            systemdevicelist.system_devices[DeviceSlot::Focuser].isConnect = true;
            // 清空绑定名称（让左侧卡片显示为空白，避免“已解绑但仍显示旧设备名”）
            systemdevicelist.system_devices[DeviceSlot::Focuser].DeviceIndiName.clear();

            // 保留串口路径到配置中，便于下次自动重连（不清空 DeviceIndiName）
            // systemdevicelist.system_devices[DeviceSlot::Focuser].DeviceIndiName.clear();

            Logger::Log("UnBinding Focuser (SDK) end.", LogLevel::INFO, DeviceType::FOCUSER);
            // 解绑后重新进入“待分配列表”：必须使用真实串口名，避免占位符 SDK_Focuser 造成前端误判/误绑定
            // 只在可用时下发；否则跳过（用户可通过重新探测/连接流程恢复串口信息）
            QString name;
            const QString saved = systemdevicelist.system_devices[DeviceSlot::Focuser].DeviceIndiName;
            if (!saved.isEmpty())
                name = saved;
            else if (!sdkFocuserPort.isEmpty())
                name = sdkFocuserPort;
            if (!name.isEmpty())
            {
                emit wsThread->sendMessageToClient("DeviceToBeAllocated:Focuser:" + QString::number(SDK_FOCUSER_UI_INDEX) + ":" + name);
            }
            else
            {
                Logger::Log("UnBinding Focuser (SDK) | skip DeviceToBeAllocated: no valid port/name", LogLevel::WARNING, DeviceType::FOCUSER);
            }
            Tools::saveSystemDeviceList(systemdevicelist);
            return;
        }

        int DeviceIndex;
        for (int i = 0; i < indi_Client->GetDeviceCount(); i++) //  indi_Client->GetDeviceFromList(i)
        {
            if (indi_Client->GetDeviceFromList(i)->getDeviceName() == dpFocuser->getDeviceName())
            {
                DeviceIndex = i;
            }
        }
        systemdevicelist.system_devices[DeviceSlot::Focuser].isBind = false;
        systemdevicelist.system_devices[DeviceSlot::Focuser].DeviceIndiName = "";
        dpFocuser = nullptr;
        Tools::saveSystemDeviceList(systemdevicelist);

        emit wsThread->sendMessageToClient("DeviceToBeAllocated:Focuser:" + QString::number(DeviceIndex) + ":" + QString::fromUtf8(indi_Client->GetDeviceFromList(DeviceIndex)->getDeviceName()));
    }
    else if (DeviceType == "PoleCamera")
    {
        if (systemdevicelist.system_devices.size() > 2 && systemdevicelist.system_devices[DeviceSlot::PoleCamera].isSDKConnect)
        {
            Logger::Log("UnBinding PoleCamera (SDK) start ...", LogLevel::INFO, DeviceType::MAIN);
            systemdevicelist.system_devices[DeviceSlot::PoleCamera].isBind = false;
            systemdevicelist.system_devices[DeviceSlot::PoleCamera].DeviceIndiName.clear();
            dpPoleScope = nullptr;

            if (g_sdkPoleCameraPoolIndex >= 0 && sdkPoolIndexValid(g_sdkPoleCameraPoolIndex))
            {
                const int uiIdx = sdkUiIndexFromPoolIndex(g_sdkPoleCameraPoolIndex);
                const QString &camId = g_sdkQhyCamIds[g_sdkPoleCameraPoolIndex];
                QString camId_cat;
                if (camId.contains("5III", Qt::CaseInsensitive))
                    camId_cat = "5III";
                else if (camId.contains("DEMO", Qt::CaseInsensitive))
                    camId_cat = "DEMO";
                else
                    camId_cat = "OTHER";
                emit wsThread->sendMessageToClient("DeviceToBeAllocated:CCD:" + QString::number(uiIdx) + ":" + camId + ":" + camId_cat);
            }

            g_sdkPoleCameraPoolIndex = -1;
            sdkPoleScopeHandle = nullptr;
            Tools::saveSystemDeviceList(systemdevicelist);
            Logger::Log("UnBinding PoleCamera (SDK) end.", LogLevel::INFO, DeviceType::MAIN);
            return;
        }

        if (!dpPoleScope)
        {
            Logger::Log("UnBinding PoleCamera Device skipped: dpPoleScope is nullptr", LogLevel::WARNING, DeviceType::MAIN);
            return;
        }
        int DeviceIndex = -1;
        for (int i = 0; i < indi_Client->GetDeviceCount(); i++) //  indi_Client->GetDeviceFromList(i)
        {
            if (indi_Client->GetDeviceFromList(i)->getDeviceName() == dpPoleScope->getDeviceName())
            {
                DeviceIndex = i;
            }
        }
        systemdevicelist.system_devices[DeviceSlot::PoleCamera].isBind = false;
        systemdevicelist.system_devices[DeviceSlot::PoleCamera].DeviceIndiName = "";
        dpPoleScope = nullptr;
        Tools::saveSystemDeviceList(systemdevicelist);

        if (DeviceIndex >= 0 && DeviceIndex < indi_Client->GetDeviceCount())
        {
            QString devName = QString::fromUtf8(indi_Client->GetDeviceFromList(DeviceIndex)->getDeviceName());
            QString devName_cat;
            if (devName.contains("5III", Qt::CaseInsensitive))
                devName_cat = "5III";
            else if (devName.contains("DEMO", Qt::CaseInsensitive))
                devName_cat = "DEMO";
            else
                devName_cat = "OTHER";
            emit wsThread->sendMessageToClient("DeviceToBeAllocated:CCD:" + QString::number(DeviceIndex) + ":" + devName + ":" + devName_cat);
        }
    }
    else if (DeviceType == "CFW")
    {
        int DeviceIndex;
        for (int i = 0; i < indi_Client->GetDeviceCount(); i++) //  indi_Client->GetDeviceFromList(i)
        {
            if (indi_Client->GetDeviceFromList(i)->getDeviceName() == dpCFW->getDeviceName())
            {
                DeviceIndex = i;
            }
        }
        systemdevicelist.system_devices[DeviceSlot::CFW].isBind = false;
        systemdevicelist.system_devices[DeviceSlot::CFW].DeviceIndiName = "";
        dpCFW = nullptr;
        Tools::saveSystemDeviceList(systemdevicelist);

        emit wsThread->sendMessageToClient("DeviceToBeAllocated:CFW:" + QString::number(DeviceIndex) + ":" + QString::fromUtf8(indi_Client->GetDeviceFromList(DeviceIndex)->getDeviceName()));
    }

    indi_Client->PrintDevices();
}

void MainWindow::AfterDeviceConnect(INDI::BaseDevice *dp)
{
    auto ensureGuiderLoopStarted = [this](const QString& sourceTag) {
        const bool guiderSdk =
            (systemdevicelist.system_devices.size() > 1 &&
             systemdevicelist.system_devices[DeviceSlot::Guider].isSDKConnect &&
             sdkGuiderHandle != nullptr);
        const bool guiderConnected = ((dpGuider != NULL && dpGuider->isConnected()) || guiderSdk);
        if (!guiderConnected)
        {
            Logger::Log("AfterDeviceConnect | skip auto-start guider loop: guider not connected (" +
                            sourceTag.toStdString() + ")",
                        LogLevel::DEBUG, DeviceType::GUIDER);
            return;
        }
        if (isGuiderLoopExp)
        {
            Logger::Log("AfterDeviceConnect | guider loop already running, keep current state (" +
                            sourceTag.toStdString() + ")",
                        LogLevel::DEBUG, DeviceType::GUIDER);
            return;
        }

        Logger::Log("AfterDeviceConnect | auto-start guider loop after guider connected (" +
                        sourceTag.toStdString() + ")",
                    LogLevel::INFO, DeviceType::GUIDER);
        if (guiderCore)
        {
            postGuiderCore(guiderCore, [](GuiderCore *core) { core->startLoop(); });
            return;
        }

        isGuiderLoopExp = true;
        guiderExposureInFlight = false;
        if (guiderLoopTimer)
            guiderLoopTimer->start(0);
        emit wsThread->sendMessageToClient("GuiderLoopExpStatus:true");
        emit wsThread->sendMessageToClient("GuiderUpdateStatus:1");
    };

    // 处理 SDK 设备（dp 为 nullptr 的情况）
    if (dp == nullptr)
    {
        // 检查是否有 SDK 主相机连接（且未完成绑定初始化，避免重复处理）
        // 关键防护：
        // 仅当“主相机已被明确分配到 SDK 相机池中的某个句柄”时，才允许走主相机 SDK 初始化。
        // 否则在绑定 Guider / Focuser 等场景触发 AfterDeviceConnect(nullptr) 时，
        // 可能误触发主相机初始化并发送 ConnectSuccess:MainCamera，造成“主相机未绑定却被立即绑定”的现象。
        const bool mainPoolAssigned =
            (g_sdkMainCameraPoolIndex >= 0 && sdkPoolIndexValid(g_sdkMainCameraPoolIndex) && sdkMainCameraHandle != nullptr);
        if (systemdevicelist.system_devices.size() > 20 &&
            systemdevicelist.system_devices[DeviceSlot::MainCamera].isSDKConnect &&
            systemdevicelist.system_devices[DeviceSlot::MainCamera].isConnect &&
            !systemdevicelist.system_devices[DeviceSlot::MainCamera].isBind &&
            mainPoolAssigned)
        {
            Logger::Log("AfterDeviceConnect | Processing SDK MainCamera connection", 
                       LogLevel::INFO, DeviceType::CAMERA);

            const SdkDeviceHandle mainHandle = sdkMainCameraHandle;
            auto sdkCallMain = [this, mainHandle](const SdkCommand &cmd) -> SdkResult {
                if (mainHandle == nullptr)
                {
                    SdkResult r;
                    r.success = false;
                    r.errorCode = SdkErrorCode::InvalidParameter;
                    r.message = "MainCamera handle is null during SDK initialization";
                    return r;
                }

                SdkSerialExecutor *mainExec = sdkMainCameraExecutor();
                if (mainExec && mainExec->isRunning())
                {
                    return mainExec->postAndWait<SdkResult>([mainHandle, cmd]() {
                        return SdkManager::instance().callByHandle(mainHandle, cmd);
                    });
                }

                return SdkManager::instance().callByHandle(mainHandle, cmd);
            };

            bool mainSdkInitOk = true;
            QString mainSdkInitFailStep;
            QString mainSdkInitFailMsg;
            auto markMainSdkInitFailed = [&](const QString& step, const std::string& msg) {
                if (!mainSdkInitOk) {
                    return;
                }
                mainSdkInitOk = false;
                mainSdkInitFailStep = step;
                mainSdkInitFailMsg = QString::fromStdString(msg);
            };
            
            // ==================== SDK 主相机初始化流程 ====================
            // 记录“上一轮是否为相机内置 CFW”，用于在本轮未检测到 CFW 时清理前端残留状态
            const bool prevFilterOnCamera = isFilterOnCamera;
            isFilterOnCamera = false;
            sdkMainCfwSlotsCached = 0;

            // 0. 读取本地保存的主相机参数（config/config.ini）
            // 注意：INDI 分支在 dpMainCamera == dp 时会调用 getMainCameraParameters() 并设置初始参数；
            // SDK 分支此前未调用，导致 SDK 连接时未使用本地保存的 Gain/Offset/温度/USB 等参数。
            getMainCameraParameters();
            
            // 1) 关键：按 QHY SDK 手册顺序初始化（StreamMode/ReadMode 在 InitQHYCCD 之前）
            // 1.1 ReadMode（best-effort）
            {
                SdkCommand rm;
                rm.type = SdkCommandType::Custom;
                rm.name = "SetReadMode";
                rm.payload = 0;
                SdkResult rmRes = sdkCallMain(rm);
                if (!rmRes.success) {
                    Logger::Log("AfterDeviceConnect | SDK SetReadMode(0) warn: " + rmRes.message,
                                LogLevel::WARNING, DeviceType::CAMERA);
                }
            }

            // 1.2 StreamMode（默认单帧；若用户预先选择了 Live/Burst，则设为 1）
            {
                const int streamMode = (mainCameraCaptureMode == MainCameraCaptureMode::Single) ? 0 : 1;
                SdkCommand streamCmd;
                streamCmd.type = SdkCommandType::Custom;
                streamCmd.name = "SetStreamMode";
                streamCmd.payload = streamMode;
                SdkResult streamRes = sdkCallMain(streamCmd);
                if (!streamRes.success) {
                    Logger::Log("AfterDeviceConnect | SDK SetStreamMode(pre-Init) failed: " + streamRes.message,
                                LogLevel::ERROR, DeviceType::CAMERA);
                    markMainSdkInitFailed(QStringLiteral("SetStreamMode(pre-Init)"), streamRes.message);
                }
            }

            // 1.3 InitCamera
            {
                SdkCommand initCmd;
                initCmd.type = SdkCommandType::Custom;
                initCmd.name = "InitCamera";
                initCmd.payload = std::any();
                SdkResult initRes = sdkCallMain(initCmd);
                if (!initRes.success) {
                    Logger::Log("AfterDeviceConnect | SDK InitCamera failed: " + initRes.message,
                                LogLevel::ERROR, DeviceType::CAMERA);
                    markMainSdkInitFailed(QStringLiteral("InitCamera"), initRes.message);
                }
            }
            
            // 3. 设置位深为 16 位
            SdkCommand bitsCmd;
            bitsCmd.type = SdkCommandType::Custom;
            bitsCmd.name = "SetBitsMode";
            bitsCmd.payload = 16;
            // 直接通过设备句柄调用，无需指定驱动名称
            SdkResult bitsRes = sdkCallMain(bitsCmd);
            if (!bitsRes.success) {
                Logger::Log("AfterDeviceConnect | SDK SetBitsMode failed: " + bitsRes.message, 
                           LogLevel::WARNING, DeviceType::CAMERA);
            }

            // 对齐 SDK 重开流程：确保单帧曝光链路工作在 1x1 硬件 Bin。
            // 否则部分机型在 SetResolution(full) 后会出现 memLength/实际读帧尺寸不一致，进而导致 GetSingleFrame 崩溃。
            {
                SdkCommand binCmd;
                binCmd.type = SdkCommandType::Custom;
                binCmd.name = "SetBinMode";
                binCmd.payload = std::make_pair(1, 1);
                SdkResult binRes = sdkCallMain(binCmd);
                if (!binRes.success) {
                    Logger::Log("AfterDeviceConnect | SDK SetBinMode(1,1) failed: " + binRes.message,
                               LogLevel::WARNING, DeviceType::CAMERA);
                }
            }
            
            // 4. 获取芯片信息（分辨率、像素大小等）
            SdkCommand chipInfoCmd;
            chipInfoCmd.type = SdkCommandType::Custom;
            chipInfoCmd.name = "GetChipInfo";
            chipInfoCmd.payload = std::any();
            // 直接通过设备句柄调用，无需指定驱动名称
            SdkResult chipInfoRes = sdkCallMain(chipInfoCmd);
            
            if (chipInfoRes.success) {
                try {
                    SdkChipInfo chipInfo = std::any_cast<SdkChipInfo>(chipInfoRes.payload);
                    glMainCCDSizeX = chipInfo.maxImageSizeX;
                    glMainCCDSizeY = chipInfo.maxImageSizeY;
                    
                    // 计算芯片物理尺寸（mm）
                    glCameraSize_width = chipInfo.chipWidthMM;
                    glCameraSize_height = chipInfo.chipHeightMM;
                    
                    Logger::Log("AfterDeviceConnect | SDK ChipInfo - SizeX: " + std::to_string(glMainCCDSizeX) + 
                               ", SizeY: " + std::to_string(glMainCCDSizeY) + 
                               ", PixelSize: " + std::to_string(chipInfo.pixelWidthUM) + " um" +
                               ", ChipSize: " + std::to_string(glCameraSize_width) + "x" + 
                               std::to_string(glCameraSize_height) + " mm", 
                               LogLevel::INFO, DeviceType::CAMERA);
                    
                    // 发送相机尺寸给前端
                    emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + 
                                                       ":" + QString::number(glMainCCDSizeY));
                } catch (const std::bad_any_cast& e) {
                    Logger::Log("AfterDeviceConnect | Failed to cast ChipInfo: " + std::string(e.what()), 
                               LogLevel::ERROR, DeviceType::CAMERA);
                    glMainCCDSizeX = 0;
                    glMainCCDSizeY = 0;
                    markMainSdkInitFailed(QStringLiteral("GetChipInfo(any_cast)"), e.what());
                }
            } else {
                glMainCCDSizeX = 0;
                glMainCCDSizeY = 0;
                Logger::Log("AfterDeviceConnect | SDK GetChipInfo failed: " + chipInfoRes.message,
                           LogLevel::ERROR, DeviceType::CAMERA);
                markMainSdkInitFailed(QStringLiteral("GetChipInfo"), chipInfoRes.message);
            }

            if (mainSdkInitOk && (glMainCCDSizeX <= 0 || glMainCCDSizeY <= 0)) {
                Logger::Log("AfterDeviceConnect | SDK ChipInfo returned invalid image size: " +
                               std::to_string(glMainCCDSizeX) + "x" + std::to_string(glMainCCDSizeY),
                           LogLevel::ERROR, DeviceType::CAMERA);
                markMainSdkInitFailed(QStringLiteral("GetChipInfo(size)"), "invalid image size");
            }

            // 5. 应用本地保存参数到 SDK（先 set，再 get 范围/当前值，避免前端看到旧的 current）
            // 5.1 USB Traffic
            {
                SdkCommand setUsbCmd;
                setUsbCmd.type = SdkCommandType::Custom;
                setUsbCmd.name = "SetUsbTraffic";
                setUsbCmd.payload = static_cast<double>(glUsbTrafficValue);
                // 直接通过设备句柄调用，无需指定驱动名称
                SdkResult setUsbRes = sdkCallMain(setUsbCmd);
                if (!setUsbRes.success) {
                    Logger::Log("AfterDeviceConnect | SDK SetUsbTraffic failed: " + setUsbRes.message,
                               LogLevel::WARNING, DeviceType::CAMERA);
                } else {
                    Logger::Log("AfterDeviceConnect | SDK USB Traffic set to: " + std::to_string(glUsbTrafficValue),
                               LogLevel::INFO, DeviceType::CAMERA);
                }
            }

            // 标记：SDK 主相机当前已应用模式（用于后续“切换模式=Close/Open”的判定）
            sdkMainAppliedModeValid = true;
            sdkMainAppliedMode = mainCameraCaptureMode;

            // 5.2 Gain
            {
                SdkCommand setGainCmd;
                setGainCmd.type = SdkCommandType::Custom;
                setGainCmd.name = "SetGain";
                setGainCmd.payload = static_cast<double>(CameraGain);
                // 直接通过设备句柄调用，无需指定驱动名称
                SdkResult setGainRes = sdkCallMain(setGainCmd);
                if (!setGainRes.success) {
                    Logger::Log("AfterDeviceConnect | SDK SetGain failed: " + setGainRes.message,
                               LogLevel::WARNING, DeviceType::CAMERA);
                } else {
                    Logger::Log("AfterDeviceConnect | SDK Gain set to: " + std::to_string(CameraGain),
                               LogLevel::INFO, DeviceType::CAMERA);
                }
            }

            // 5.3 Offset
            {
                SdkCommand setOffsetCmd;
                setOffsetCmd.type = SdkCommandType::Custom;
                setOffsetCmd.name = "SetOffset";
                setOffsetCmd.payload = ImageOffset;
                // 直接通过设备句柄调用，无需指定驱动名称
                SdkResult setOffsetRes = sdkCallMain(setOffsetCmd);
                if (!setOffsetRes.success) {
                    Logger::Log("AfterDeviceConnect | SDK SetOffset failed: " + setOffsetRes.message,
                               LogLevel::WARNING, DeviceType::CAMERA);
                } else {
                    Logger::Log("AfterDeviceConnect | SDK Offset set to: " + std::to_string(ImageOffset),
                               LogLevel::INFO, DeviceType::CAMERA);
                }
            }

            // 5.4 Cooler target temperature
            {
                SdkCommand setTempCmd;
                setTempCmd.type = SdkCommandType::Custom;
                setTempCmd.name = "SetCoolerTargetTemperature";
                setTempCmd.payload = CameraTemperature;
                // 直接通过设备句柄调用，无需指定驱动名称
                SdkResult setTempRes = sdkCallMain(setTempCmd);
                if (!setTempRes.success) {
                    Logger::Log("AfterDeviceConnect | SDK SetCoolerTargetTemperature failed: " + setTempRes.message,
                               LogLevel::WARNING, DeviceType::CAMERA);
                } else {
                    Logger::Log("AfterDeviceConnect | SDK Temperature set to: " + std::to_string(CameraTemperature),
                               LogLevel::INFO, DeviceType::CAMERA);
                }
            }
            
            // 6. 获取 Gain 范围和当前值
            SdkCommand getGainCmd;
            getGainCmd.type = SdkCommandType::Custom;
            getGainCmd.name = "GetGain";
            getGainCmd.payload = std::any();
            // 直接通过设备句柄调用，无需指定驱动名称
            SdkResult gainRes = sdkCallMain(getGainCmd);
            
            if (gainRes.success) {
                try {
                    SdkControlParamInfo gainInfo = std::any_cast<SdkControlParamInfo>(gainRes.payload);
                    glGainMin = static_cast<int>(gainInfo.minValue);
                    glGainMax = static_cast<int>(gainInfo.maxValue);
                    glGainValue = static_cast<int>(gainInfo.current);
                    
                    Logger::Log("AfterDeviceConnect | SDK Gain - Min: " + std::to_string(glGainMin) + 
                               ", Max: " + std::to_string(glGainMax) + 
                               ", Current: " + std::to_string(glGainValue), 
                               LogLevel::INFO, DeviceType::CAMERA);
                    
                    emit wsThread->sendMessageToClient("MainCameraGainRange:" + QString::number(glGainMin) + 
                                                       ":" + QString::number(glGainMax) + 
                                                       ":" + QString::number(glGainValue));
                } catch (const std::bad_any_cast& e) {
                    Logger::Log("AfterDeviceConnect | Failed to cast Gain info: " + std::string(e.what()), 
                               LogLevel::ERROR, DeviceType::CAMERA);
                }
            }

            // 7. 获取 Offset 范围和当前值
            SdkCommand getOffsetCmd;
            getOffsetCmd.type = SdkCommandType::Custom;
            getOffsetCmd.name = "GetOffset";
            getOffsetCmd.payload = std::any();
            // 直接通过设备句柄调用，无需指定驱动名称
            SdkResult offsetRes = sdkCallMain(getOffsetCmd);
            
            if (offsetRes.success) {
                try {
                    SdkControlParamInfo offsetInfo = std::any_cast<SdkControlParamInfo>(offsetRes.payload);
                    glOffsetMin = static_cast<int>(offsetInfo.minValue);
                    glOffsetMax = static_cast<int>(offsetInfo.maxValue);
                    glOffsetValue = static_cast<int>(offsetInfo.current);
                    
                    Logger::Log("AfterDeviceConnect | SDK Offset - Min: " + std::to_string(glOffsetMin) + 
                               ", Max: " + std::to_string(glOffsetMax) + 
                               ", Current: " + std::to_string(glOffsetValue), 
                               LogLevel::INFO, DeviceType::CAMERA);
                    
                    emit wsThread->sendMessageToClient("MainCameraOffsetRange:" + QString::number(glOffsetMin) + 
                                                       ":" + QString::number(glOffsetMax) + 
                                                       ":" + QString::number(glOffsetValue));
                } catch (const std::bad_any_cast& e) {
                    Logger::Log("AfterDeviceConnect | Failed to cast Offset info: " + std::string(e.what()), 
                               LogLevel::ERROR, DeviceType::CAMERA);
                }
            }
            
            // 7-扩展. 获取 USB Traffic 范围和当前值（SDK）
            SdkCommand getUsbTrafficCmd;
            getUsbTrafficCmd.type = SdkCommandType::Custom;
            getUsbTrafficCmd.name = "GetUsbTraffic";
            getUsbTrafficCmd.payload = std::any();
            // 直接通过设备句柄调用，无需指定驱动名称
            SdkResult usbRes = sdkCallMain(getUsbTrafficCmd);

            if (usbRes.success) {
                try {
                    SdkControlParamInfo usbInfo = std::any_cast<SdkControlParamInfo>(usbRes.payload);
                    glUsbTrafficMin = static_cast<int>(usbInfo.minValue);
                    glUsbTrafficMax = static_cast<int>(usbInfo.maxValue);
                    glUsbTrafficStep = static_cast<int>(usbInfo.step);
                    glUsbTrafficValue = static_cast<int>(usbInfo.current);
                    if (glUsbTrafficStep <= 0) glUsbTrafficStep = 1;

                    Logger::Log("AfterDeviceConnect | SDK USB Traffic - Min: " + std::to_string(glUsbTrafficMin) +
                               ", Max: " + std::to_string(glUsbTrafficMax) +
                               ", Step: " + std::to_string(glUsbTrafficStep) +
                               ", Current: " + std::to_string(glUsbTrafficValue),
                               LogLevel::INFO, DeviceType::CAMERA);

                    emit wsThread->sendMessageToClient("MainCameraUsbTrafficRange:" + QString::number(glUsbTrafficMin) +
                                                       ":" + QString::number(glUsbTrafficMax) +
                                                       ":" + QString::number(glUsbTrafficValue) +
                                                       ":" + QString::number(glUsbTrafficStep));
                } catch (const std::bad_any_cast& e) {
                    Logger::Log("AfterDeviceConnect | Failed to cast USB Traffic info: " + std::string(e.what()),
                               LogLevel::ERROR, DeviceType::CAMERA);
                }
            }
            
            // 10. 计算和设置 Binning（使画面尺寸 <= 1024）
            int requiredBinning = 1;
            int currentSize = glMainCCDSizeX;
            while (currentSize > 1024 && requiredBinning <= 16) {
                requiredBinning *= 2;
                currentSize = glMainCCDSizeX / requiredBinning;
            }
            if (requiredBinning > 16) {
                requiredBinning = 16;
            }
            glMainCameraBinning = requiredBinning;
            
            Logger::Log("AfterDeviceConnect | SDK Binning selected: " + std::to_string(glMainCameraBinning) + 
                       " (Final size: " + std::to_string(glMainCCDSizeX / glMainCameraBinning) + "x" + 
                       std::to_string(glMainCCDSizeY / glMainCameraBinning) + ")", 
                       LogLevel::INFO, DeviceType::CAMERA);
            emit wsThread->sendMessageToClient("MainCameraBinning:" + QString::number(glMainCameraBinning));
            
            // 11. 获取 SDK 版本
            SdkCommand verCmd;
            verCmd.type = SdkCommandType::Custom;
            verCmd.name = "GetSdkVersion";
            verCmd.payload = std::any();
            // 直接通过设备句柄调用，无需指定驱动名称
            SdkResult verRes = sdkCallMain(verCmd);
            if (verRes.success) {
                try {
                    std::string version = std::any_cast<std::string>(verRes.payload);
                    emit wsThread->sendMessageToClient("getSDKVersion:MainCamera:" + QString::fromStdString(version));
                    Logger::Log("AfterDeviceConnect | SDK Version: " + version, LogLevel::INFO, DeviceType::CAMERA);
                } catch (const std::bad_any_cast&) {
                    Logger::Log("AfterDeviceConnect | Failed to get SDK version", LogLevel::WARNING, DeviceType::CAMERA);
                }
            }
            
            // 12. 检查是否为彩色相机并获取 CFA 信息
            SdkCommand colorCmd;
            colorCmd.type = SdkCommandType::Custom;
            colorCmd.name = "IsColorCamera";
            colorCmd.payload = std::any();
            // 直接通过设备句柄调用，无需指定驱动名称
            SdkResult colorRes = sdkCallMain(colorCmd);
            if (colorRes.success) {
                try {
                    bool isColor = std::any_cast<bool>(colorRes.payload);
                    if (isColor) {
                        MainCameraCFAOffsetX = 0;
                        MainCameraCFAOffsetY = 0;
                        const QString savedCameraCfa = normalizeCfaPattern(MainCameraCFA);
                        bool usedSavedCameraCfaFallback = false;
                        SdkCommand cfaCmd;
                        cfaCmd.type = SdkCommandType::Custom;
                        cfaCmd.name = "GetCameraCfa";
                        cfaCmd.payload = std::any();
                        SdkResult cfaRes = sdkCallMain(cfaCmd);

                        if (cfaRes.success) {
                            try {
                                MainCameraCFA = normalizeCfaPattern(QString::fromStdString(std::any_cast<std::string>(cfaRes.payload)));
                                Logger::Log("AfterDeviceConnect | SDK camera CFA detected by GetCameraCfa: " + MainCameraCFA.toStdString(),
                                            LogLevel::INFO, DeviceType::CAMERA);
                            } catch (const std::bad_any_cast&) {
                                MainCameraCFA.clear();
                                Logger::Log("AfterDeviceConnect | Failed to cast SDK camera CFA payload",
                                            LogLevel::WARNING, DeviceType::CAMERA);
                            }
                        } else {
                            MainCameraCFA.clear();
                            Logger::Log("AfterDeviceConnect | Failed to get SDK camera CFA: " + cfaRes.message,
                                        LogLevel::WARNING, DeviceType::CAMERA);
                        }

                        if (MainCameraCFA.isEmpty() &&
                            (savedCameraCfa == "RGGB" || savedCameraCfa == "BGGR" ||
                             savedCameraCfa == "GRBG" || savedCameraCfa == "GBRG")) {
                            MainCameraCFA = savedCameraCfa;
                            usedSavedCameraCfaFallback = true;
                            Logger::Log("AfterDeviceConnect | Fallback to saved SDK camera CFA: " + MainCameraCFA.toStdString(),
                                        LogLevel::WARNING, DeviceType::CAMERA);
                        }

                        if (!MainCameraCFA.isEmpty()) {
                            Tools::saveParameter("MainCamera", "ImageCFA", MainCameraCFA);
                            Logger::Log("AfterDeviceConnect | SDK Camera is color camera, CFA: " + MainCameraCFA.toStdString(),
                                       LogLevel::INFO, DeviceType::CAMERA);
                            emit wsThread->sendMessageToClient("MainCameraCFA:" + MainCameraCFA);
                            emit wsThread->sendMessageToClient("MainCameraCFASource:" +
                                                               (usedSavedCameraCfaFallback ? QStringLiteral("SDKFallback")
                                                                                           : QStringLiteral("SDK")));
                        } else {
                            Tools::saveParameter("MainCamera", "ImageCFA", QStringLiteral("null"));
                            Logger::Log("AfterDeviceConnect | SDK Camera is color camera, but CFA detection failed",
                                        LogLevel::WARNING, DeviceType::CAMERA);
                            emit wsThread->sendMessageToClient("MainCameraCFA:null");
                            emit wsThread->sendMessageToClient("MainCameraCFASource:SDKFallback");
                        }
                    } else {
                        MainCameraCFA = "";
                        Tools::saveParameter("MainCamera", "ImageCFA", QStringLiteral("null"));
                        Logger::Log("AfterDeviceConnect | SDK Camera is mono camera", LogLevel::INFO, DeviceType::CAMERA);
                        emit wsThread->sendMessageToClient("MainCameraCFA:null");
                        emit wsThread->sendMessageToClient("MainCameraCFASource:SDK");
                    }
                } catch (const std::bad_any_cast&) {
                    Logger::Log("AfterDeviceConnect | Failed to get color camera info", LogLevel::WARNING, DeviceType::CAMERA);
                }
            }
            
            // ==================== 完成初始化 ====================
            if (!mainSdkInitOk)
            {
                systemdevicelist.system_devices[DeviceSlot::MainCamera].isBind = false;
                systemdevicelist.system_devices[DeviceSlot::MainCamera].isConnect = false;
                sdkMainAppliedModeValid = false;
                sdkMainLiveReady = false;
                sdkMainBurstModeReady = false;

                const QString failDetail = mainSdkInitFailMsg.isEmpty()
                    ? mainSdkInitFailStep
                    : (mainSdkInitFailStep + ": " + mainSdkInitFailMsg);

                Logger::Log("AfterDeviceConnect | SDK MainCamera initialization aborted, treat as connect failed. step=" +
                               mainSdkInitFailStep.toStdString() + " msg=" + mainSdkInitFailMsg.toStdString(),
                           LogLevel::ERROR, DeviceType::CAMERA);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:MainCamera:SDK init failed");
                emit wsThread->sendMessageToClient("ConnectFailed:MainCamera:SDK init failed:" + failDetail);
                return;
            }
            
            // 标记设备已绑定
            systemdevicelist.system_devices[DeviceSlot::MainCamera].isBind = true;
            
            // 发送连接成功消息给前端
            // SDK 模式下，DeviceIndiName 保存 cameraId（设备名）；DriverIndiName 仍应保存“驱动名”
            QString deviceName = systemdevicelist.system_devices[DeviceSlot::MainCamera].DeviceIndiName;
            if (deviceName.isEmpty()) {
                deviceName = "SDK_MainCamera";
            }
            
            // 保存设备名称
            systemdevicelist.system_devices[DeviceSlot::MainCamera].DeviceIndiName = deviceName;
            
            // 修复：发送实际的 DriverIndiName 而不是 "SDK"，供前端用于驱动选择和连接
            // 连接模式信息通过 SelectedDriverList 消息传递，前端通过 device.connectionMode 区分
            QString driverNameForConnect = systemdevicelist.system_devices[DeviceSlot::MainCamera].DriverIndiName;
            emit wsThread->sendMessageToClient("ConnectSuccess:MainCamera:" + deviceName + ":" + driverNameForConnect);
            Logger::Log("AfterDeviceConnect | SDK MainCamera connected successfully: " + deviceName.toStdString(), 
                       LogLevel::INFO, DeviceType::CAMERA);
            
            // 添加到已连接设备列表
            ConnectedDevices.push_back({"MainCamera", deviceName});

            // ==================== SDK 模式下探测“相机内置滤镜轮(CFW)” ====================
            // 兼容前端协议：CFWPositionMax 表示槽位数量（1..N），并触发前端请求 getCFWList。
            {
                SdkCommand plugCmd;
                plugCmd.type = SdkCommandType::Custom;
                plugCmd.name = "IsCFWPlugged";
                plugCmd.payload = std::any();
                // 直接通过设备句柄调用，无需指定驱动名称
                SdkResult plugRes = sdkCallMain(plugCmd);
                if (plugRes.success && plugRes.payload.has_value())
                {
                    bool plugged = false;
                    try { plugged = std::any_cast<bool>(plugRes.payload); } catch (const std::bad_any_cast&) { plugged = false; }

                    if (plugged)
                    {
                        int slotsNum = 0;
                        std::string err;
                        if (sdkGetCfwSlotsNum(sdkMainCameraHandle, slotsNum, &err) && slotsNum > 0)
                        {
                            // 设备列表中显示的 CFW 名称：保持“on camera”语义，并用 cameraId 做区分
                            QString cfwDisplayName = sdkMainCameraId.isEmpty()
                                ? "CFW (on camera)"
                                : ("CFW (on camera) - " + sdkMainCameraId);

                            isFilterOnCamera = true;
                            sdkMainCfwSlotsCached = slotsNum;
                            Logger::Log("AfterDeviceConnect | SDK CFW detected, slotsNum=" + std::to_string(slotsNum),
                                       LogLevel::INFO, DeviceType::CFW);

                            emit wsThread->sendMessageToClient("ConnectSuccess:CFW:" + cfwDisplayName + ":" + QString::fromLatin1("indi_qhy_ccd"));
                            emit wsThread->sendMessageToClient("CFWPositionMax:" + QString::number(slotsNum));

                            int cur0 = -1;
                            if (sdkGetCfwPosition0(sdkMainCameraHandle, cur0, &err))
                            {
                                Logger::Log("AfterDeviceConnect | SDK CFW current position=" + std::to_string(toUiCfwPos1(cur0)),
                                           LogLevel::INFO, DeviceType::CFW);
                            }

                            // 若已有缓存名称列表，则直接推送一次（与外置 CFW 行为保持一致）
                            const QString key = sdkCfwStorageKey(sdkMainCameraId);
                            const QString list = Tools::readCFWList(key);
                            if (!list.isEmpty())
                                emit wsThread->sendMessageToClient("getCFWList:" + list);
                        }
                        else
                        {
                            Logger::Log("AfterDeviceConnect | SDK CFW plugged but GetCFWSlotsNum failed: " + err,
                                       LogLevel::WARNING, DeviceType::CFW);
                        }
                    }
                }
            }

            // 若本轮未检测到相机内置 CFW，但上一轮存在，则主动通知前端删除 CFW 设备，避免残留控制入口。
            if (!isFilterOnCamera && prevFilterOnCamera)
            {
                emit wsThread->sendMessageToClient("deleteDeviceTypeAllocationList:CFW");
            }
            
            Logger::Log("AfterDeviceConnect | SDK MainCamera initialization completed", 
                       LogLevel::INFO, DeviceType::CAMERA);
        }
        else if (systemdevicelist.system_devices.size() > 20 &&
                 systemdevicelist.system_devices[DeviceSlot::MainCamera].isSDKConnect &&
                 systemdevicelist.system_devices[DeviceSlot::MainCamera].isConnect &&
                 !systemdevicelist.system_devices[DeviceSlot::MainCamera].isBind &&
                 !mainPoolAssigned)
        {
            Logger::Log("AfterDeviceConnect | Skip SDK MainCamera init: main camera not assigned to SDK pool yet (likely allocating/other device init).",
                        LogLevel::DEBUG, DeviceType::CAMERA);
        }
        
        // ==================== SDK 电调(Focuser)初始化流程 ====================
        // 检查是否有 SDK 电调连接（且未完成绑定初始化，避免重复处理）
        if (systemdevicelist.system_devices.size() > 22 && 
            systemdevicelist.system_devices[DeviceSlot::Focuser].isSDKConnect && 
            systemdevicelist.system_devices[DeviceSlot::Focuser].isConnect &&
            sdkFocuserHandle != nullptr &&
            !systemdevicelist.system_devices[DeviceSlot::Focuser].isBind)
        {
            Logger::Log("AfterDeviceConnect | Processing SDK Focuser connection", 
                       LogLevel::INFO, DeviceType::FOCUSER);
            
            // 1. 下发电调参数（行程/空程等，从本地配置读取）
            getFocuserParameters();
            
            // 2. 获取并推送 SDK 版本：必须在 sdkFocuserExec 线程执行，
            // 否则会触发 "QSocketNotifier ... from another thread" 并导致后续串口读写异常。
            requestSdkFocuserVersionUpdate(true);
            
            // 3. 推送串口路径（SDK 电调的"设备端口"就是串口路径）
            if (!sdkFocuserPort.isEmpty())
            {
                emit wsThread->sendMessageToClient("getDevicePort:Focuser:" + sdkFocuserPort);
                Logger::Log("AfterDeviceConnect | SDK Focuser port: " + sdkFocuserPort.toStdString(), 
                           LogLevel::INFO, DeviceType::FOCUSER);
            }
            
            // 4. 先读取当前位置并推送（必须在范围校验前完成）
            // SDK 模式：位置读取走异步线程，避免主线程阻塞/跨线程串口
            requestSdkFocuserPositionUpdate(true);
            // 同时立即推送一次缓存值（若无缓存则为 0），避免前端空白
            // 注意：位置读取是异步的，首次连接时缓存可能尚未有效，不能据此做范围重置判定。
            const bool hasValidSdkPosition = sdkFocuserPosValid.load();
            CurrentPosition = hasValidSdkPosition ? sdkFocuserPosCache.load() : 0;
            Logger::Log("AfterDeviceConnect | SDK Focuser current position: " + std::to_string(CurrentPosition),
                        LogLevel::INFO, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) +
                                             ":" + QString::number(CurrentPosition));
            
            // 5. 读取电调范围（如果本地保存过，则使用保存值；否则使用默认值）
            // 注意：QHY 电调协议没有"读取范围"命令，范围由前端/配置管理
            if (focuserMaxPosition == -1 && focuserMinPosition == -1)
            {
                focuserMinPosition = -100000;
                focuserMaxPosition = 100000;
                Logger::Log("AfterDeviceConnect | SDK Focuser using default range: [-100000, 100000]", 
                           LogLevel::INFO, DeviceType::FOCUSER);
            }
            
            // 6. 校验当前位置是否在范围内（仅在位置缓存有效时执行）
            // SDK 首次连接时位置读取是异步的，若此时用默认 0 参与校验会导致误判并错误重置范围。
            if (!hasValidSdkPosition)
            {
                Logger::Log("AfterDeviceConnect | SDK Focuser position cache not ready yet, skip range validation this round",
                           LogLevel::INFO, DeviceType::FOCUSER);
            }
            else if (CurrentPosition < focuserMinPosition || CurrentPosition > focuserMaxPosition)
            {
                Logger::Log("AfterDeviceConnect | Warning: Current position (" + std::to_string(CurrentPosition) + 
                           ") is out of saved range [" + std::to_string(focuserMinPosition) + ", " + 
                           std::to_string(focuserMaxPosition) + "]. Local range data is invalid!", 
                           LogLevel::WARNING, DeviceType::FOCUSER);
                
                // 本地范围数据不合理，重新初始化为默认范围
                const int DEFAULT_MIN_LIMIT = -100000;
                const int DEFAULT_MAX_LIMIT = 100000;
                
                focuserMinPosition = DEFAULT_MIN_LIMIT;
                focuserMaxPosition = DEFAULT_MAX_LIMIT;
                
                // 保存新范围
                Tools::saveParameter("Focuser", "focuserMinPosition", QString::number(focuserMinPosition));
                Tools::saveParameter("Focuser", "focuserMaxPosition", QString::number(focuserMaxPosition));
                
                Logger::Log("AfterDeviceConnect | Local range data reinitialized to default: [" + 
                           std::to_string(focuserMinPosition) + ", " + std::to_string(focuserMaxPosition) + "]", 
                           LogLevel::INFO, DeviceType::FOCUSER);
                
                // 向前端发送警告消息
                emit wsThread->sendMessageToClient("FocuserRangeReset:Saved range is invalid (position out of range). " +
                                                 QString("Range has been reset to default [%1, %2]. Please recalibrate if needed.")
                                                 .arg(focuserMinPosition).arg(focuserMaxPosition));
            }
            else
            {
                Logger::Log("AfterDeviceConnect | Position validation passed. Current position (" + 
                           std::to_string(CurrentPosition) + ") is within saved range [" + 
                           std::to_string(focuserMinPosition) + ", " + std::to_string(focuserMaxPosition) + "]", 
                           LogLevel::INFO, DeviceType::FOCUSER);
            }
            
            // 7. 推送最终确认的范围
            emit wsThread->sendMessageToClient("FocuserLimit:" + QString::number(focuserMinPosition) + 
                                             ":" + QString::number(focuserMaxPosition));
            
            // 6. 标记设备已绑定
            systemdevicelist.system_devices[DeviceSlot::Focuser].isBind = true;
            
            // 7. 发送连接成功消息给前端（与 INDI 电调保持一致的格式）
            // 设备名必须为真实串口（如 /dev/ttyACM0），避免占位符导致前端错误识别
            QString deviceName = systemdevicelist.system_devices[DeviceSlot::Focuser].DeviceIndiName;
            if (deviceName.isEmpty() && !sdkFocuserPort.isEmpty())
            {
                deviceName = sdkFocuserPort;
                systemdevicelist.system_devices[DeviceSlot::Focuser].DeviceIndiName = deviceName;
            }
            if (deviceName.isEmpty())
            {
                Logger::Log("AfterDeviceConnect | SDK Focuser has empty deviceName and sdkFocuserPort, skip ConnectSuccess:Focuser",
                            LogLevel::WARNING, DeviceType::FOCUSER);
                // 仍继续其它初始化/推送（版本/位置等），但不发 ConnectSuccess，避免前端出现 SDK_Focuser
            }
            // 修复：发送实际的 DriverIndiName 而不是 "SDK"，供前端用于驱动选择和连接
            // 连接模式信息通过 SelectedDriverList 消息传递，前端通过 device.connectionMode 区分
            QString driverNameForConnect = systemdevicelist.system_devices[DeviceSlot::Focuser].DriverIndiName;
            if (!deviceName.isEmpty())
                emit wsThread->sendMessageToClient("ConnectSuccess:Focuser:" + deviceName + ":" + driverNameForConnect);
            Logger::Log("AfterDeviceConnect | SDK Focuser connected successfully: " + deviceName.toStdString(), 
                       LogLevel::INFO, DeviceType::FOCUSER);
            
            // 添加到已连接设备列表
            ConnectedDevices.push_back({"Focuser", deviceName});
            
            Logger::Log("AfterDeviceConnect | SDK Focuser initialization completed", 
                       LogLevel::INFO, DeviceType::FOCUSER);
        }
        
        // ==================== SDK 导星相机(Guider)初始化流程 ====================
        if (systemdevicelist.system_devices.size() > 1 &&
            systemdevicelist.system_devices[DeviceSlot::Guider].isSDKConnect &&
            systemdevicelist.system_devices[DeviceSlot::Guider].isConnect &&
            sdkGuiderHandle != nullptr &&
            !systemdevicelist.system_devices[DeviceSlot::Guider].isBind)
        {
            Logger::Log("AfterDeviceConnect | Processing SDK Guider connection",
                        LogLevel::INFO, DeviceType::GUIDER);

            // 1) InitCamera
            {
                SdkCommand initCmd;
                initCmd.type = SdkCommandType::Custom;
                initCmd.name = "InitCamera";
                initCmd.payload = std::any();
                SdkResult initRes = SdkManager::instance().callByHandle(sdkGuiderHandle, initCmd);
                if (!initRes.success)
                {
                    Logger::Log("AfterDeviceConnect | SDK Guider InitCamera failed: " + initRes.message,
                                LogLevel::ERROR, DeviceType::GUIDER);
                }
            }

            // 2) 单帧模式
            {
                SdkCommand streamCmd;
                streamCmd.type = SdkCommandType::Custom;
                streamCmd.name = "SetStreamMode";
                streamCmd.payload = 0;
                SdkResult streamRes = SdkManager::instance().callByHandle(sdkGuiderHandle, streamCmd);
                if (!streamRes.success)
                {
                    Logger::Log("AfterDeviceConnect | SDK Guider SetStreamMode failed: " + streamRes.message,
                                LogLevel::WARNING, DeviceType::GUIDER);
                }
            }

            // 3) 16bit
            {
                SdkCommand bitsCmd;
                bitsCmd.type = SdkCommandType::Custom;
                bitsCmd.name = "SetBitsMode";
                bitsCmd.payload = 16;
                SdkResult bitsRes = SdkManager::instance().callByHandle(sdkGuiderHandle, bitsCmd);
                if (!bitsRes.success)
                {
                    Logger::Log("AfterDeviceConnect | SDK Guider SetBitsMode failed: " + bitsRes.message,
                                LogLevel::WARNING, DeviceType::GUIDER);
                }
            }

            // 4) 版本信息
            {
                SdkCommand verCmd;
                verCmd.type = SdkCommandType::Custom;
                verCmd.name = "GetSdkVersion";
                verCmd.payload = std::any();
                SdkResult verRes = SdkManager::instance().callByHandle(sdkGuiderHandle, verCmd);
                if (verRes.success)
                {
                    try
                    {
                        std::string version = std::any_cast<std::string>(verRes.payload);
                        emit wsThread->sendMessageToClient("getSDKVersion:Guider:" + QString::fromStdString(version));
                    }
                    catch (const std::bad_any_cast &)
                    {
                        Logger::Log("AfterDeviceConnect | SDK Guider GetSdkVersion bad_any_cast",
                                    LogLevel::WARNING, DeviceType::GUIDER);
                    }
                }
            }

            QMap<QString, QString> guiderParameters = Tools::readParameters("Guider");

            {
                SdkCommand getGainCmd;
                getGainCmd.type = SdkCommandType::Custom;
                getGainCmd.name = "GetGain";
                getGainCmd.payload = std::any();
                SdkResult gainRes = SdkManager::instance().callByHandle(sdkGuiderHandle, getGainCmd);
                if (gainRes.success)
                {
                    try
                    {
                        SdkControlParamInfo gainInfo = std::any_cast<SdkControlParamInfo>(gainRes.payload);
                        glGuiderGainMin = static_cast<int>(gainInfo.minValue);
                        glGuiderGainMax = static_cast<int>(gainInfo.maxValue);
                        glGuiderGainValue = static_cast<int>(gainInfo.current);

                        if (guiderParameters.contains("Gain"))
                        {
                            guiderCameraGain = guiderParameters["Gain"].toDouble();
                            int targetGain = static_cast<int>(guiderCameraGain);
                            if (targetGain < glGuiderGainMin) targetGain = glGuiderGainMin;
                            if (targetGain > glGuiderGainMax) targetGain = glGuiderGainMax;

                            SdkCommand setGainCmd;
                            setGainCmd.type = SdkCommandType::Custom;
                            setGainCmd.name = "SetGain";
                            setGainCmd.payload = targetGain;
                            SdkResult setGainRes = SdkManager::instance().callByHandle(sdkGuiderHandle, setGainCmd);
                            if (setGainRes.success)
                            {
                                glGuiderGainValue = targetGain;
                            }
                        }

                        emit wsThread->sendMessageToClient("GuiderGainRange:" + QString::number(glGuiderGainMin) + ":" +
                                                           QString::number(glGuiderGainMax) + ":" +
                                                           QString::number(glGuiderGainValue));
                    }
                    catch (const std::bad_any_cast &e)
                    {
                        Logger::Log("AfterDeviceConnect | Failed to cast Guider Gain info: " + std::string(e.what()),
                                    LogLevel::ERROR, DeviceType::GUIDER);
                    }
                }
            }

            {
                SdkCommand getOffsetCmd;
                getOffsetCmd.type = SdkCommandType::Custom;
                getOffsetCmd.name = "GetOffset";
                getOffsetCmd.payload = std::any();
                SdkResult offsetRes = SdkManager::instance().callByHandle(sdkGuiderHandle, getOffsetCmd);
                if (offsetRes.success)
                {
                    try
                    {
                        SdkControlParamInfo offsetInfo = std::any_cast<SdkControlParamInfo>(offsetRes.payload);
                        glGuiderOffsetMin = static_cast<int>(offsetInfo.minValue);
                        glGuiderOffsetMax = static_cast<int>(offsetInfo.maxValue);
                        glGuiderOffsetValue = static_cast<int>(offsetInfo.current);

                        if (guiderParameters.contains("Offset"))
                        {
                            guiderCameraOffset = guiderParameters["Offset"].toDouble();
                            int targetOffset = static_cast<int>(guiderCameraOffset);
                            if (targetOffset < glGuiderOffsetMin) targetOffset = glGuiderOffsetMin;
                            if (targetOffset > glGuiderOffsetMax) targetOffset = glGuiderOffsetMax;

                            SdkCommand setOffsetCmd;
                            setOffsetCmd.type = SdkCommandType::Custom;
                            setOffsetCmd.name = "SetOffset";
                            setOffsetCmd.payload = targetOffset;
                            SdkResult setOffsetRes = SdkManager::instance().callByHandle(sdkGuiderHandle, setOffsetCmd);
                            if (setOffsetRes.success)
                            {
                                glGuiderOffsetValue = targetOffset;
                            }
                        }

                        emit wsThread->sendMessageToClient("GuiderOffsetRange:" + QString::number(glGuiderOffsetMin) + ":" +
                                                           QString::number(glGuiderOffsetMax) + ":" +
                                                           QString::number(glGuiderOffsetValue));
                    }
                    catch (const std::bad_any_cast &e)
                    {
                        Logger::Log("AfterDeviceConnect | Failed to cast Guider Offset info: " + std::string(e.what()),
                                    LogLevel::ERROR, DeviceType::GUIDER);
                    }
                }
            }

            {
                SdkCommand chipInfoCmd;
                chipInfoCmd.type = SdkCommandType::Custom;
                chipInfoCmd.name = "GetChipInfo";
                chipInfoCmd.payload = std::any();
                SdkResult chipInfoRes = SdkManager::instance().callByHandle(sdkGuiderHandle, chipInfoCmd);
                if (chipInfoRes.success)
                {
                    try
                    {
                        SdkChipInfo chipInfo = std::any_cast<SdkChipInfo>(chipInfoRes.payload);
                        if (chipInfo.pixelWidthUM > 0.0)
                        {
                            guiderPixelSizeUm = chipInfo.pixelWidthUM;
                            Logger::Log("AfterDeviceConnect | SDK Guider pixel size: " + std::to_string(guiderPixelSizeUm) + " um",
                                        LogLevel::INFO, DeviceType::GUIDER);
                            syncGuiderScaleParams(true, false);
                        }
                    }
                    catch (const std::bad_any_cast &e)
                    {
                        Logger::Log("AfterDeviceConnect | Failed to cast SDK Guider chip info: " + std::string(e.what()),
                                    LogLevel::WARNING, DeviceType::GUIDER);
                    }
                }
            }

            systemdevicelist.system_devices[DeviceSlot::Guider].isBind = true;
            QString deviceName = systemdevicelist.system_devices[DeviceSlot::Guider].DeviceIndiName;
            if (deviceName.isEmpty())
                deviceName = "SDK_Guider";

            QString driverNameForConnect = systemdevicelist.system_devices[DeviceSlot::Guider].DriverIndiName;
            emit wsThread->sendMessageToClient("ConnectSuccess:Guider:" + deviceName + ":" + driverNameForConnect);
            ConnectedDevices.push_back({"Guider", deviceName});
            ensureGuiderLoopStarted(QStringLiteral("SDK"));
            Logger::Log("AfterDeviceConnect | SDK Guider initialization completed",
                        LogLevel::INFO, DeviceType::GUIDER);
        }

        // ==================== SDK 电子极轴镜(PoleCamera)初始化流程 ====================
        if (systemdevicelist.system_devices.size() > 2 &&
            systemdevicelist.system_devices[DeviceSlot::PoleCamera].isSDKConnect &&
            systemdevicelist.system_devices[DeviceSlot::PoleCamera].isConnect &&
            sdkPoleScopeHandle != nullptr &&
            !systemdevicelist.system_devices[DeviceSlot::PoleCamera].isBind)
        {
            Logger::Log("AfterDeviceConnect | Processing SDK PoleCamera connection",
                        LogLevel::INFO, DeviceType::MAIN);

            SdkCommand initCmd;
            initCmd.type = SdkCommandType::Custom;
            initCmd.name = "InitCamera";
            initCmd.payload = std::any();
            SdkResult initRes = SdkManager::instance().callByHandle(sdkPoleScopeHandle, initCmd);
            if (!initRes.success)
            {
                Logger::Log("AfterDeviceConnect | SDK PoleCamera InitCamera failed: " + initRes.message,
                            LogLevel::ERROR, DeviceType::MAIN);
            }

            SdkCommand streamCmd;
            streamCmd.type = SdkCommandType::Custom;
            streamCmd.name = "SetStreamMode";
            streamCmd.payload = 0;
            SdkManager::instance().callByHandle(sdkPoleScopeHandle, streamCmd);

            SdkCommand bitsCmd;
            bitsCmd.type = SdkCommandType::Custom;
            bitsCmd.name = "SetBitsMode";
            bitsCmd.payload = 16;
            SdkManager::instance().callByHandle(sdkPoleScopeHandle, bitsCmd);

            SdkCommand verCmd;
            verCmd.type = SdkCommandType::Custom;
            verCmd.name = "GetSdkVersion";
            verCmd.payload = std::any();
            SdkResult verRes = SdkManager::instance().callByHandle(sdkPoleScopeHandle, verCmd);
            if (verRes.success)
            {
                try
                {
                    std::string version = std::any_cast<std::string>(verRes.payload);
                    emit wsThread->sendMessageToClient("getSDKVersion:PoleCamera:" + QString::fromStdString(version));
                }
                catch (const std::bad_any_cast &) {}
            }

            systemdevicelist.system_devices[DeviceSlot::PoleCamera].isBind = true;
            QString deviceName = systemdevicelist.system_devices[DeviceSlot::PoleCamera].DeviceIndiName;
            if (deviceName.isEmpty())
                deviceName = "SDK_PoleCamera";

            QString driverNameForConnect = systemdevicelist.system_devices[DeviceSlot::PoleCamera].DriverIndiName;
            emit wsThread->sendMessageToClient("ConnectSuccess:PoleCamera:" + deviceName + ":" + driverNameForConnect);
            ConnectedDevices.push_back({"PoleCamera", deviceName});
            Logger::Log("AfterDeviceConnect | SDK PoleCamera initialization completed",
                        LogLevel::INFO, DeviceType::MAIN);
        }
        
        return;
    }
    
    if (dpMainCamera == dp)
    {
        if (isDSLR(dpMainCamera) && NotSetDSLRsInfo)
        {
            QString CameraName = dpMainCamera->getDeviceName();
            Logger::Log("This may be a DSLRs Camera, need to set Resolution and pixel size. Camera: " + CameraName.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
            DSLRsInfo DSLRsInfo = Tools::readDSLRsInfo(CameraName);
            if (DSLRsInfo.Name == CameraName && DSLRsInfo.SizeX != 0 && DSLRsInfo.SizeY != 0 && DSLRsInfo.PixelSize != 0)
            {
                indi_Client->setCCDBasicInfo(dpMainCamera, DSLRsInfo.SizeX, DSLRsInfo.SizeY, DSLRsInfo.PixelSize, DSLRsInfo.PixelSize, DSLRsInfo.PixelSize, 8);
                Logger::Log("Updated CCD Basic Info for DSLRs Camera.", LogLevel::INFO, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("DSLRsSetup:" + QString::fromUtf8(dpMainCamera->getDeviceName()) + ":" + QString::number(DSLRsInfo.SizeX) + ":" + QString::number(DSLRsInfo.SizeY) + ":" + QString::number(DSLRsInfo.PixelSize));
                return;
            }
            else
            {
                emit wsThread->sendMessageToClient("DSLRsSetup:" + QString::fromUtf8(dpMainCamera->getDeviceName()));
                return;
            }
        }
        if (isDSLR(dpMainCamera) ){
            indi_Client->disableDSLRLiveView(dpMainCamera);
            Logger::Log("Disabled DSLR Live View for Camera: " + QString::fromUtf8(dpMainCamera->getDeviceName()).toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }

        // 获取主相机所有参数
        getMainCameraParameters();

        // 设置初始gain
        indi_Client->setCCDGain(dpMainCamera,CameraGain);

        // 设置初始offset
        indi_Client->setCCDOffset(dpMainCamera,ImageOffset);

        // 设置初始usb traffic
        indi_Client->setCCDUsbTraffic(dpMainCamera, glUsbTrafficValue);

        // 预先获取SDK的值为默认值
        indi_Client->getCCDOffset(dpMainCamera, glOffsetValue, glOffsetMin, glOffsetMax);
        emit wsThread->sendMessageToClient("MainCameraOffsetRange:" + QString::number(glOffsetMin) + ":" + QString::number(glOffsetMax) + ":" + QString::number(glOffsetValue));
        Logger::Log("CCD Offset - Value: " + std::to_string(glOffsetValue) + ", Min: " + std::to_string(glOffsetMin) + ", Max: " + std::to_string(glOffsetMax), LogLevel::INFO, DeviceType::MAIN);

        indi_Client->getCCDGain(dpMainCamera, glGainValue, glGainMin, glGainMax);
        Logger::Log("CCD Gain - Value: " + std::to_string(glGainValue) + ", Min: " + std::to_string(glGainMin) + ", Max: " + std::to_string(glGainMax), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("MainCameraGainRange:" + QString::number(glGainMin) + ":" + QString::number(glGainMax) + ":" + QString::number(glGainValue));

        // USB_TRAFFIC（可选）：存在则下发给前端显示（放在 Offset 下方）
        {
            int v = 0, mn = 0, mx = 0, st = 1;
            if (indi_Client->getCCDUsbTraffic(dpMainCamera, v, mn, mx, st) == QHYCCD_SUCCESS)
            {
                glUsbTrafficValue = v; glUsbTrafficMin = mn; glUsbTrafficMax = mx; glUsbTrafficStep = st;
                Logger::Log("CCD USB Traffic - Value: " + std::to_string(glUsbTrafficValue) + ", Min: " + std::to_string(glUsbTrafficMin) + ", Max: " + std::to_string(glUsbTrafficMax) + ", Step: " + std::to_string(glUsbTrafficStep), LogLevel::INFO, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("MainCameraUsbTrafficRange:" + QString::number(glUsbTrafficMin) + ":" + QString::number(glUsbTrafficMax) + ":" + QString::number(glUsbTrafficValue) + ":" + QString::number(glUsbTrafficStep));
            }
        }


        NotSetDSLRsInfo = true;
        sleep(1); // 给与初始化数据更新时间
        indi_Client->GetAllPropertyName(dpMainCamera);
        Logger::Log("MainCamera connected after Device(" + QString::fromUtf8(dpMainCamera->getDeviceName()).toStdString() + ") Connect: " + QString::fromUtf8(dpMainCamera->getDeviceName()).toStdString(), LogLevel::INFO, DeviceType::MAIN);
        
        ConnectedDevices.push_back({"MainCamera", QString::fromUtf8(dpMainCamera->getDeviceName())});

        systemdevicelist.system_devices[DeviceSlot::MainCamera].DeviceIndiName = QString::fromUtf8(dpMainCamera->getDeviceName());
        systemdevicelist.system_devices[DeviceSlot::MainCamera].isBind = true;

        indi_Client->setBLOBMode(B_ALSO, dpMainCamera->getDeviceName(), nullptr);
        indi_Client->enableDirectBlobAccess(dpMainCamera->getDeviceName(), nullptr);

        QString SDKVERSION;
        indi_Client->getCCDSDKVersion(dpMainCamera, SDKVERSION);
        emit wsThread->sendMessageToClient("getSDKVersion:MainCamera:" + SDKVERSION);
        Logger::Log("MainCamera SDK version: " + SDKVERSION.toStdString(), LogLevel::INFO, DeviceType::MAIN);


        int maxX, maxY;
        double pixelsize, pixelsizX, pixelsizY;
        int bitDepth;


        Logger::Log("CCD Basic Info - MaxX: " + std::to_string(maxX) + ", MaxY: " + std::to_string(maxY) + ", PixelSize: " + std::to_string(pixelsize), LogLevel::INFO, DeviceType::MAIN);
        if (bitDepth != 16)
        {
            Logger::Log("The current camera outputs is not 16-bit data; attempting to modify it to 16-bit.", LogLevel::INFO, DeviceType::CAMERA);
            // indi_Client->setCCDBasicInfo(dpMainCamera, maxX, maxY, pixelsize, pixelsizX, pixelsizY, 16);
        }

        // indi_Client->getCCDBasicInfo(dpMainCamera, maxX, maxY, pixelsize, pixelsizX, pixelsizY, bitDepth);
        if (bitDepth != 16)
        {
            Logger::Log("Failed to set the camera bit depth to 16-bit.", LogLevel::WARNING, DeviceType::CAMERA);
        }
        // 设置初始温度
        indi_Client->setTemperature(dpMainCamera, CameraTemperature);
        Logger::Log("CCD Temperature set to: " + std::to_string(CameraTemperature), LogLevel::INFO, DeviceType::MAIN);

        glCameraSize_width = maxX * pixelsize / 1000;
        glCameraSize_width = std::round(glCameraSize_width * 10) / 10;
        glCameraSize_height = maxY * pixelsize / 1000;
        glCameraSize_height = std::round(glCameraSize_height * 10) / 10;
        Logger::Log("CCD Chip size - Width: " + std::to_string(glCameraSize_width) + ", Height: " + std::to_string(glCameraSize_height), LogLevel::INFO, DeviceType::MAIN);

        int X, Y;
        indi_Client->getCCDFrameInfo(dpMainCamera, X, Y, glMainCCDSizeX, glMainCCDSizeY);
        Logger::Log("CCD Frame Info - SizeX: " + std::to_string(glMainCCDSizeX) + ", SizeY: " + std::to_string(glMainCCDSizeY), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));

        int offsetX, offsetY;
        indi_Client->getCCDCFA(dpMainCamera, offsetX, offsetY, MainCameraCFA);
        MainCameraCFAOffsetX = offsetX;
        MainCameraCFAOffsetY = offsetY;
        MainCameraCFA = normalizeCfaPattern(MainCameraCFA);
        if (MainCameraCFA == "NULL" || MainCameraCFA == "MONO") {
            MainCameraCFA.clear();
        }
        Tools::saveParameter("MainCamera", "ImageCFA", MainCameraCFA.isEmpty() ? QStringLiteral("null") : MainCameraCFA);
        Logger::Log("CCD CFA Info - OffsetX: " + std::to_string(offsetX) + ", OffsetY: " + std::to_string(offsetY) + ", CFA: " + MainCameraCFA.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("MainCameraCFA:" + (MainCameraCFA.isEmpty() ? QStringLiteral("null") : MainCameraCFA));
        emit wsThread->sendMessageToClient("MainCameraCFASource:INDI");
        indi_Client->setCCDUploadModeToLacal(dpMainCamera);
        indi_Client->setCCDUpload(dpMainCamera, "/dev/shm", "ccd_simulator");

        // 计算需要的binning以达到548像素以下
        int requiredBinning = 1;
        int currentSize = glMainCCDSizeX;

        // 逐步增加binning直到像素大小小于等于548
        while (currentSize > 1024 && requiredBinning <= 16)
        {
            requiredBinning *= 2;
            currentSize = glMainCCDSizeX / requiredBinning;
        }

        // 限制最大binning为16
        if (requiredBinning > 16)
        {
            requiredBinning = 16;
        }

        glMainCameraBinning = requiredBinning;

        // 记录选择的binning和最终像素大小
        int finalSize = glMainCCDSizeX / requiredBinning;
        qDebug() << "Camera binning selection: Original size =" << glMainCCDSizeX
                 << "Binning =" << requiredBinning << "Final size =" << finalSize;
        emit wsThread->sendMessageToClient("MainCameraBinning:" + QString::number(glMainCameraBinning));

        QString CFWname;
        indi_Client->getCFWSlotName(dpMainCamera, CFWname);
        if (CFWname != "")
        {
            Logger::Log("CFW Slot Name: " + CFWname.toStdString(), LogLevel::INFO, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("ConnectSuccess:CFW:" + CFWname + " (on camera):" + QString::fromUtf8(dpMainCamera->getDriverExec()));
            isFilterOnCamera = true;

            int min, max, pos;
            indi_Client->getCFWPosition(dpMainCamera, pos, min, max);
            Logger::Log("CFW Position - Min: " + std::to_string(min) + ", Max: " + std::to_string(max) + ", Current: " + std::to_string(pos), LogLevel::INFO, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CFWPositionMax:" + QString::number(max));
        }
        Logger::Log("MainCamera connected successfully.", LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectSuccess:MainCamera:" + QString::fromUtf8(dpMainCamera->getDeviceName()) + ":" + QString::fromUtf8(dpMainCamera->getDriverExec()));
    }

    if (dpMount == dp)
    {
        Logger::Log("Mount connected after Device(" + QString::fromUtf8(dpMount->getDeviceName()).toStdString() + ") Connect: " + QString::fromUtf8(dpMount->getDeviceName()).toStdString(), LogLevel::INFO, DeviceType::MAIN);
        
        ConnectedDevices.push_back({"Mount", QString::fromUtf8(dpMount->getDeviceName())});

        systemdevicelist.system_devices[DeviceSlot::Mount].DeviceIndiName = QString::fromUtf8(dpMount->getDeviceName());
        systemdevicelist.system_devices[DeviceSlot::Mount].isBind = true;
        QString DevicePort;

        indi_Client->GetAllPropertyName(dpMount);

        getClientSettings();
        getMountParameters();
        indi_Client->setLocation(dpMount, observatorylatitude, observatorylongitude, 50);
        Logger::Log("Mount location set to Latitude: " + QString::number(observatorylatitude).toStdString() + ", Longitude: " + QString::number(observatorylongitude).toStdString(), LogLevel::INFO, DeviceType::MAIN);
        indi_Client->setAutoFlip(dpMount, false);
        indi_Client->setMinutesPastMeridian(dpMount, 1, -1);


        indi_Client->setAUXENCODERS(dpMount);


        indi_Client->getDevicePort(dpMount, DevicePort);
        emit wsThread->sendMessageToClient("getDevicePort:Mount:" + DevicePort);
        Logger::Log("Device port for Mount: " + DevicePort.toStdString(), LogLevel::INFO, DeviceType::MAIN);

        // double glLongitude_radian, glLatitude_radian;
        // glLongitude_radian = Tools::getDecAngle(localLat);
        // glLatitude_radian = Tools::getDecAngle(localLon);
        // Logger::Log("Mount location set to Longitude: " + QString::number(Tools::RadToDegree(glLongitude_radian)).toStdString() + ", Latitude: " + QString::number(Tools::RadToDegree(glLatitude_radian)).toStdString(), LogLevel::INFO, DeviceType::MAIN);

        // indi_Client->setLocation(dpMount, Tools::RadToDegree(glLatitude_radian), Tools::RadToDegree(glLongitude_radian), 10);
        QDateTime datetime = QDateTime::currentDateTime();
        indi_Client->setTimeUTC(dpMount, datetime);
        Logger::Log("UTC Time set for Mount: " + datetime.toString(Qt::ISODate).toStdString(), LogLevel::INFO, DeviceType::MAIN);
        indi_Client->getTimeUTC(dpMount, datetime);
        Logger::Log("UTC Time: " + datetime.currentDateTimeUtc().toString(Qt::ISODate).toStdString(), LogLevel::INFO, DeviceType::MAIN);

        double a, b, c, d;
        indi_Client->getTelescopeInfo(dpMount, a, b, c, d);
        Logger::Log("Telescope Info - a: " + std::to_string(a) + ", b: " + std::to_string(b) + ", c: " + std::to_string(c) + ", d: " + std::to_string(d), LogLevel::INFO, DeviceType::MAIN);

        // 内置导星：优先尝试从 INDI 的 TELESCOPE_INFO 读取 guider_focal（d）
        // - 读不到不阻断流程；后续 RMS 将以 px 显示
        if (d > 0.0)
        {
            guiderFocalLengthMm = d;
            syncGuiderScaleParams(true, false);
            if (guiderUsesArcsecUnit())
            {
                Logger::Log("BuiltInGuider | pixelScaleArcsecPerPixel=" + std::to_string(currentGuiderArcsecPerPixel()),
                            LogLevel::INFO, DeviceType::GUIDER);
            }
        }
        else
        {
            if (guiderCore && !guiderScaleHintSent)
            {
                // UI 明确提示：未获取导星焦距时无法换算角秒
                emit wsThread->sendMessageToClient(QStringLiteral("GuiderCoreInfo:当前未获取导星焦距，误差以像素显示"));
                emit wsThread->sendMessageToClient(QStringLiteral("GuiderCoreInfo:若提供焦距，可显示 arcsec RMS"));
                guiderScaleHintSent = true;
            }
        }

        indi_Client->getTelescopeRADECJ2000(dpMount, a, b);
        Logger::Log("Telescope RA/DEC J2000 - RA: " + std::to_string(a) + ", DEC: " + std::to_string(b), LogLevel::INFO, DeviceType::MAIN);
        indi_Client->getTelescopeRADECJNOW(dpMount, a, b);
        Logger::Log("Telescope RA/DEC JNOW - RA: " + std::to_string(a) + ", DEC: " + std::to_string(b), LogLevel::INFO, DeviceType::MAIN);

        bool isPark;
        indi_Client->getTelescopePark(dpMount, isPark);
        Logger::Log("Telescope Park Status: " + std::string(isPark ? "Parked" : "Unparked"), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("TelescopePark:" + QString::fromStdString(isPark ? "ON" : "OFF"));

        int maxspeed, minspeed, speedvalue, total;
        indi_Client->getTelescopeTotalSlewRate(dpMount, total);
        glTelescopeTotalSlewRate = total;
        Logger::Log("Telescope Total Slew Rate: " + std::to_string(total), LogLevel::INFO, DeviceType::MAIN);

        emit wsThread->sendMessageToClient("TelescopeTotalSlewRate:" + QString::number(total));
        indi_Client->getTelescopeMaxSlewRateOptions(dpMount, minspeed, maxspeed, speedvalue);
        indi_Client->setTelescopeSlewRate(dpMount, total);
        int speed;
        indi_Client->getTelescopeSlewRate(dpMount, speed);
        Logger::Log("Current Telescope Slew Rate: " + std::to_string(speed), LogLevel::INFO, DeviceType::MAIN);
        // emit wsThread->sendMessageToClient("TelescopeCurrentSlewRate:" + QString::number(speed));
        emit wsThread->sendMessageToClient("MountSetSpeedSuccess:" + QString::number(speed));
        indi_Client->setTelescopeTrackEnable(dpMount, true);

        bool isTrack = false;
        indi_Client->getTelescopeTrackEnable(dpMount, isTrack);

        if (isTrack)
        {
            emit wsThread->sendMessageToClient("TelescopeTrack:ON");
        }
        else
        {
            emit wsThread->sendMessageToClient("TelescopeTrack:OFF");
        }
        Logger::Log("Telescope Tracking Status: " + std::string(isTrack ? "Enabled" : "Disabled"), LogLevel::INFO, DeviceType::MAIN);
        indi_Client->setTelescopeTrackRate(dpMount, "SIDEREAL");
        QString side;
        indi_Client->getTelescopePierSide(dpMount, side);
        Logger::Log("Telescope Pier Side: " + side.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("TelescopePierSide:" + side);
        Logger::Log("Mount connected successfully.", LogLevel::INFO, DeviceType::MAIN);

        // 获取驱动版本号
        QString MountSDKVersion = "null";
        indi_Client->getMountInfo(dpMount, MountSDKVersion);
        emit wsThread->sendMessageToClient("getMountInfo:" + MountSDKVersion);
        Logger::Log("Mount Info: " + MountSDKVersion.toStdString(), LogLevel::INFO, DeviceType::MAIN);

        // 设置home位置
        // indi_Client->setTelescopeHomeInit(dpMount, "SYNCHOME");
        indi_Client->mountState.updateHomeRAHours(observatorylatitude, observatorylongitude);
        indi_Client->mountState.printCurrentState();
        emit wsThread->sendMessageToClient("ConnectSuccess:Mount:" + QString::fromUtf8(dpMount->getDeviceName()) + ":" + QString::fromUtf8(dpMount->getDriverExec()));
        
    }

    if (dpFocuser == dp)
    {
        Logger::Log("Focuser connected after Device(" + QString::fromUtf8(dpFocuser->getDeviceName()).toStdString() + ") Connect: " + dpFocuser->getDeviceName(), LogLevel::INFO, DeviceType::MAIN);
        
        ConnectedDevices.push_back({"Focuser", QString::fromUtf8(dpFocuser->getDeviceName())});

        systemdevicelist.system_devices[DeviceSlot::Focuser].DeviceIndiName = QString::fromUtf8(dpFocuser->getDeviceName());
        systemdevicelist.system_devices[DeviceSlot::Focuser].isBind = true;
        indi_Client->GetAllPropertyName(dpFocuser);
        // indi_Client->syncFocuserPosition(dpFocuser, 0);
        
        // 1. 获取电调参数（行程/空程等）
        getFocuserParameters();
        
        // 2. 获取并推送版本信息
        QString SDKVERSION = "null";
        indi_Client->getFocuserSDKVersion(dpFocuser, SDKVERSION);
        emit wsThread->sendMessageToClient("getSDKVersion:Focuser:" + SDKVERSION);
        Logger::Log("Focuser SDK version: " + SDKVERSION.toStdString(), LogLevel::INFO, DeviceType::MAIN);

        // 3. 获取并推送端口信息
        QString DevicePort = "null";
        indi_Client->getDevicePort(dpFocuser, DevicePort);
        emit wsThread->sendMessageToClient("getDevicePort:Focuser:" + DevicePort);
        Logger::Log("Device port for Focuser: " + DevicePort.toStdString(), LogLevel::INFO, DeviceType::MAIN);

        // 4. 先获取当前位置（必须在范围校验前完成）
        CurrentPosition = FocuserControl_getPosition();
        Logger::Log("Focuser current position: " + std::to_string(CurrentPosition), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));

        // 5. 读取电调范围
        int min, max, step, value;
        indi_Client->getFocuserRange(dpFocuser, min, max, step, value);
        if (focuserMaxPosition == -1 && focuserMinPosition == -1)
        {
            focuserMaxPosition = max;
            focuserMinPosition = min;
            Tools::saveParameter("Focuser", "focuserMaxPosition", QString::number(focuserMaxPosition));
            Tools::saveParameter("Focuser", "focuserMinPosition", QString::number(focuserMinPosition));
        }
        
        // 6. 校验当前位置是否在范围内，如果不在则说明本地保存的范围数据不合理，需要重新初始化
        if (CurrentPosition < focuserMinPosition || CurrentPosition > focuserMaxPosition)
        {
            Logger::Log("AfterDeviceConnect | Warning: Current position (" + std::to_string(CurrentPosition) + 
                       ") is out of saved range [" + std::to_string(focuserMinPosition) + ", " + 
                       std::to_string(focuserMaxPosition) + "]. Local range data is invalid!", 
                       LogLevel::WARNING, DeviceType::FOCUSER);
            
            // 本地范围数据不合理，重新初始化为设备硬件范围（如果可用）
            if (min != -1 && max != -1)
            {
                // 使用设备的硬件范围
                focuserMinPosition = min;
                focuserMaxPosition = max;
                Logger::Log("AfterDeviceConnect | Using device hardware range for reinitialization", 
                           LogLevel::INFO, DeviceType::FOCUSER);
            }
            else
            {
                // 设备未提供范围，使用默认范围
                focuserMinPosition = 0;
                focuserMaxPosition = 100000;
                Logger::Log("AfterDeviceConnect | Device range not available, using default range", 
                           LogLevel::INFO, DeviceType::FOCUSER);
            }
            
            // 保存新范围
            Tools::saveParameter("Focuser", "focuserMinPosition", QString::number(focuserMinPosition));
            Tools::saveParameter("Focuser", "focuserMaxPosition", QString::number(focuserMaxPosition));
            
            Logger::Log("AfterDeviceConnect | Local range data reinitialized to [" + 
                       std::to_string(focuserMinPosition) + ", " + std::to_string(focuserMaxPosition) + "]", 
                       LogLevel::INFO, DeviceType::FOCUSER);
            
            // 向前端发送警告消息
            emit wsThread->sendMessageToClient("FocuserRangeReset:Saved range is invalid (position out of range). " +
                                             QString("Range has been reset to [%1, %2]. Please recalibrate if needed.")
                                             .arg(focuserMinPosition).arg(focuserMaxPosition));
        }
        else
        {
            Logger::Log("AfterDeviceConnect | Position validation passed. Current position (" + 
                       std::to_string(CurrentPosition) + ") is within saved range [" + 
                       std::to_string(focuserMinPosition) + ", " + std::to_string(focuserMaxPosition) + "]", 
                       LogLevel::INFO, DeviceType::FOCUSER);
        }
        
        // 7. 推送最终确认的范围
        emit wsThread->sendMessageToClient("FocuserLimit:" + QString::number(focuserMinPosition) + ":" + QString::number(focuserMaxPosition));
        
        Logger::Log("Focuser connected successfully.", LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectSuccess:Focuser:" + QString::fromUtf8(dpFocuser->getDeviceName()) + ":" + QString::fromUtf8(dpFocuser->getDriverExec()));
    }

    if (dpCFW == dp)
    {
        Logger::Log("CFW connected after Device(" + QString::fromUtf8(dpCFW->getDeviceName()).toStdString() + ") Connect: " + dpCFW->getDeviceName(), LogLevel::INFO, DeviceType::MAIN);
        
        ConnectedDevices.push_back({"CFW", QString::fromUtf8(dpCFW->getDeviceName())});

        systemdevicelist.system_devices[DeviceSlot::CFW].DeviceIndiName = QString::fromUtf8(dpCFW->getDeviceName());
        systemdevicelist.system_devices[DeviceSlot::CFW].isBind = true;
        indi_Client->GetAllPropertyName(dpCFW);
        int min, max, pos;
        indi_Client->getCFWPosition(dpCFW, pos, min, max);
        Logger::Log("CFW Position - Min: " + std::to_string(min) + ", Max: " + std::to_string(max) + ", Current: " + std::to_string(pos), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("CFWPositionMax:" + QString::number(max));
        if (Tools::readCFWList(QString::fromUtf8(dpCFW->getDeviceName())) != QString())
        {
            emit wsThread->sendMessageToClient("getCFWList:" + Tools::readCFWList(QString::fromUtf8(dpCFW->getDeviceName())));
        }
        Logger::Log("CFW connected successfully.", LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectSuccess:CFW:" + QString::fromUtf8(dpCFW->getDeviceName()) + ":" + QString::fromUtf8(dpCFW->getDriverExec()));
    }

    if (dpPoleScope == dp)
    {
        const QString deviceName = QString::fromUtf8(dpPoleScope->getDeviceName());
        const QString driverExec = QString::fromUtf8(dpPoleScope->getDriverExec());
        Logger::Log("PoleCamera connected after Device(" + deviceName.toStdString() + ") Connect: " + deviceName.toStdString(),
                    LogLevel::INFO, DeviceType::MAIN);

        indi_Client->GetAllPropertyName(dpPoleScope);
        ConnectedDevices.push_back({"PoleCamera", deviceName});

        if (systemdevicelist.system_devices.size() > 2)
        {
            systemdevicelist.system_devices[DeviceSlot::PoleCamera].DeviceIndiName = deviceName;
            systemdevicelist.system_devices[DeviceSlot::PoleCamera].isBind = true;
        }

        indi_Client->setBLOBMode(B_ALSO, dpPoleScope->getDeviceName(), nullptr);
        indi_Client->enableDirectBlobAccess(dpPoleScope->getDeviceName(), nullptr);
        indi_Client->setCCDUploadModeToLacal(dpPoleScope);
        indi_Client->setCCDUpload(dpPoleScope, "/dev/shm", "polecamera");

        QString SDKVERSION;
        indi_Client->getCCDSDKVersion(dpPoleScope, SDKVERSION);
        emit wsThread->sendMessageToClient("getSDKVersion:PoleCamera:" + SDKVERSION);
        Logger::Log("PoleCamera SDK version: " + SDKVERSION.toStdString(), LogLevel::INFO, DeviceType::MAIN);

        Logger::Log("PoleCamera connected successfully.", LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectSuccess:PoleCamera:" + deviceName + ":" + driverExec);
    }

    if (dpGuider == dp)
    {
        Logger::Log("Guider connected after Device(" + QString::fromUtf8(dpGuider->getDeviceName()).toStdString() + ") Connect: " + dpGuider->getDeviceName(), LogLevel::INFO, DeviceType::MAIN);
        indi_Client->GetAllPropertyName(dpGuider);
        ConnectedDevices.push_back({"Guider", QString::fromUtf8(dpGuider->getDeviceName())});
        Logger::Log("Guider connected successfully.", LogLevel::INFO, DeviceType::MAIN);

        // 内置导星：配置导星相机上传到本地固定路径 /dev/shm/guiding.fits
        indi_Client->setBLOBMode(B_ALSO, dpGuider->getDeviceName(), nullptr);
        indi_Client->enableDirectBlobAccess(dpGuider->getDeviceName(), nullptr);
        indi_Client->setCCDUploadModeToLacal(dpGuider);
        indi_Client->setCCDUpload(dpGuider, "/dev/shm", "guiding");

        systemdevicelist.system_devices[DeviceSlot::Guider].DeviceIndiName = QString::fromUtf8(dpGuider->getDeviceName());
        systemdevicelist.system_devices[DeviceSlot::Guider].isBind = true;

        // INDI 直出图：与主相机一致，启用 BLOB 并设置本地保存目录/前缀
        indi_Client->setBLOBMode(B_ALSO, dpGuider->getDeviceName(), nullptr);
        indi_Client->enableDirectBlobAccess(dpGuider->getDeviceName(), nullptr);
        indi_Client->setCCDUploadModeToLacal(dpGuider);
        indi_Client->setCCDUpload(dpGuider, "/dev/shm", "guider");

        QString SDKVERSION;
        indi_Client->getCCDSDKVersion(dpGuider, SDKVERSION);
        emit wsThread->sendMessageToClient("getSDKVersion:Guider:" + SDKVERSION);
        Logger::Log("Guider SDK version: " + SDKVERSION.toStdString(), LogLevel::INFO, DeviceType::MAIN);

        QMap<QString, QString> guiderParameters = Tools::readParameters("Guider");
        if (guiderParameters.contains("Gain"))
        {
            guiderCameraGain = guiderParameters["Gain"].toDouble();
            indi_Client->setCCDGain(dpGuider, static_cast<int>(guiderCameraGain));
        }
        if (guiderParameters.contains("Offset"))
        {
            guiderCameraOffset = guiderParameters["Offset"].toDouble();
            indi_Client->setCCDOffset(dpGuider, static_cast<int>(guiderCameraOffset));
        }

        if (indi_Client->getCCDGain(dpGuider, glGuiderGainValue, glGuiderGainMin, glGuiderGainMax) == QHYCCD_SUCCESS)
        {
            emit wsThread->sendMessageToClient("GuiderGainRange:" + QString::number(glGuiderGainMin) + ":" +
                                               QString::number(glGuiderGainMax) + ":" +
                                               QString::number(glGuiderGainValue));
        }
        if (indi_Client->getCCDOffset(dpGuider, glGuiderOffsetValue, glGuiderOffsetMin, glGuiderOffsetMax) == QHYCCD_SUCCESS)
        {
            emit wsThread->sendMessageToClient("GuiderOffsetRange:" + QString::number(glGuiderOffsetMin) + ":" +
                                               QString::number(glGuiderOffsetMax) + ":" +
                                               QString::number(glGuiderOffsetValue));
        }

        {
            int maxX = 0;
            int maxY = 0;
            int bitDepth = 0;
            double pixelSize = 0.0;
            double pixelSizeX = 0.0;
            double pixelSizeY = 0.0;
            if (indi_Client->getCCDBasicInfo(dpGuider, maxX, maxY, pixelSize, pixelSizeX, pixelSizeY, bitDepth) == QHYCCD_SUCCESS)
            {
                double candidatePixelSize = 0.0;
                if (pixelSize > 0.0)
                    candidatePixelSize = pixelSize;
                else if (pixelSizeX > 0.0 && pixelSizeY > 0.0)
                    candidatePixelSize = (pixelSizeX + pixelSizeY) * 0.5;
                else
                    candidatePixelSize = std::max(pixelSizeX, pixelSizeY);

                if (candidatePixelSize > 0.0)
                {
                    guiderPixelSizeUm = candidatePixelSize;
                    Logger::Log("AfterDeviceConnect | INDI Guider pixel size: " + std::to_string(guiderPixelSizeUm) + " um",
                                LogLevel::INFO, DeviceType::GUIDER);
                    syncGuiderScaleParams(true, false);
                }
            }
        }

        emit wsThread->sendMessageToClient("ConnectSuccess:Guider:" + QString::fromUtf8(dpGuider->getDeviceName()) + ":" + QString::fromUtf8(dpGuider->getDriverExec()));
        ensureGuiderLoopStarted(QStringLiteral("INDI"));
    }

    Tools::saveSystemDeviceList(systemdevicelist);
    // qDebug() << "*** ***  当前系统列表 *** ***";
    // for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    // {
    //     if (systemdevicelist.system_devices[i].Description != "")
    //     {
    //         qDebug() << "设备类型：" << systemdevicelist.system_devices[i].Description;
    //         qDebug() << "设备名称：" << systemdevicelist.system_devices[i].DeviceIndiName;
    //         qDebug() << "是否绑定：" << systemdevicelist.system_devices[i].isBind;
    //         qDebug() << "驱动名称：" << systemdevicelist.system_devices[i].DriverIndiName;
    //         qDebug() << " *** *** *** *** *** *** ";
    //     }
    // }
    // qDebug() << "*** ***  当前设备列表 *** ***";
    // for (int i = 0; i < indi_Client->GetDeviceCount(); i++)
    // {
    //     qDebug() << "设备名称：" << QString::fromStdString(indi_Client->GetDeviceNameFromList(i));
    //     qDebug() << "驱动名称：" << QString::fromStdString(indi_Client->GetDeviceFromList(i)->getDriverExec());
    //     qDebug() << "是否连接：" << QString::fromStdString(std::to_string(indi_Client->GetDeviceFromList(i)->isConnected()));
    //     qDebug() << " *** *** *** *** *** *** ";
    // }
}

void MainWindow::applySdkMainCameraCaptureMode()
{
    // 仅作用于“主相机 SDK + QHYCCD”
    // 说明：
    // - Live：SetStreamMode=1 + BeginLive + 循环 GetLiveFrame
    // - Burst：EnableBurstMode(true) + Live + SetBurstIDLE/ReleaseBurstIDLE（后续 burst 拍摄只触发/抓帧/回 IDLE）
    // - Single：退出 Live 并关闭 Burst，恢复传统单帧（SetStreamMode=0）
    const bool isMainCameraSDK =
        (systemdevicelist.system_devices.size() > 20 &&
         systemdevicelist.system_devices[DeviceSlot::MainCamera].isSDKConnect &&
         sdkMainCameraHandle != nullptr);

    const QString sdkDriverName =
        (systemdevicelist.system_devices.size() > 20) ? systemdevicelist.system_devices[DeviceSlot::MainCamera].SDKDriverName : "";

    // 兼容：QHY SDK 驱动名可能为 "QHYCCD" 或 "indi_qhy_ccd"（别名）。
    // 同时，部分路径下 system_devices[DeviceSlot::MainCamera].SDKDriverName 可能为空（尚未持久化/未同步），因此做一次推导兜底。
    auto isQhySdkDriverName = [](const QString& n) -> bool {
        const QString s = n.trimmed().toLower();
        return (s == "qhyccd" || s == "indi_qhy_ccd");
    };
    auto resolveMainCameraSdkDriverName = [&]() -> QString {
        QString n = sdkDriverName.trimmed();
        if (!n.isEmpty()) return n;
        n = getSDKDriverName("MainCamera").trimmed();
        if (!n.isEmpty()) return n;
        if (systemdevicelist.system_devices.size() > 20)
            n = systemdevicelist.system_devices[DeviceSlot::MainCamera].DriverIndiName.trimmed();
        return n;
    };
    const QString effectiveSdkDriverName = resolveMainCameraSdkDriverName();
    SdkSerialExecutor *mainExec = sdkMainCameraExecutor();

    if (!isMainCameraSDK || !isQhySdkDriverName(effectiveSdkDriverName) || !mainExec || !mainExec->isRunning())
    {
        // 非 QHYCCD / SDK 线程不可用时，清空“已就绪”状态，避免 UI 误判
        Logger::Log("applySdkMainCameraCaptureMode | skip (not ready) | isMainCameraSDK=" +
                        std::string(isMainCameraSDK ? "true" : "false") +
                        " sdkDriverName=" + sdkDriverName.toStdString() +
                        " effectiveSdkDriverName=" + effectiveSdkDriverName.toStdString() +
                        " sdkMainCamExecRunning=" + std::string((mainExec && mainExec->isRunning()) ? "true" : "false"),
                    LogLevel::WARNING, DeviceType::CAMERA);
        sdkMainLiveReady = false;
        sdkMainBurstModeReady = false;
        return;
    }

    auto modeName = [this]() -> std::string {
        switch (mainCameraCaptureMode) {
            case MainCameraCaptureMode::Single: return "Single";
            case MainCameraCaptureMode::Live:   return "Live";
            case MainCameraCaptureMode::Burst:  return "Burst";
        }
        return "Unknown";
    };
    Logger::Log("applySdkMainCameraCaptureMode | request mode=" + modeName() +
                    " liveReady=" + std::string(sdkMainLiveReady.load() ? "true" : "false") +
                    " burstReady=" + std::string(sdkMainBurstModeReady.load() ? "true" : "false"),
                LogLevel::INFO, DeviceType::CAMERA);

    // 防护：拍摄中不做模式切换（避免竞争）
    // - sdkBurstActive：Burst 抓帧正在进行
    // - glMainCameraStatu=="Exposuring"：单帧曝光/轮询获取正在进行
    // 切换过程中会调用 StopLive/EnableBurstMode/SetStreamMode 等，会与当前拍摄链路抢占同一相机句柄，易导致无输出/崩溃。
    if (sdkBurstActive.load() || glMainCameraStatu == "Exposuring")
    {
        Logger::Log("applySdkMainCameraCaptureMode | camera busy, skip mode apply",
                    LogLevel::WARNING, DeviceType::CAMERA);
        return;
    }

    const SdkDeviceHandle handleSnap = sdkMainCameraHandle;

    // 若发生模式切换：按 QHY SDK 标准流程重新初始化（Close -> Open -> SetReadMode/SetStreamMode -> Init -> BeginLive/IDLE）
    // 目的：保证 StreamMode/ReadMode 在 InitQHYCCD 之前生效，避免 Live 帧率异常/阻塞。
    const bool modeSwitched = (sdkMainAppliedModeValid && sdkMainAppliedMode != mainCameraCaptureMode);
    const int poolIndexSnap = g_sdkMainCameraPoolIndex;
    const QString cameraIdSnap =
        sdkPoolIndexValid(poolIndexSnap) ? g_sdkQhyCamIds[poolIndexSnap].trimmed() : sdkMainCameraId.trimmed();
    const QString driverNameSnap = effectiveSdkDriverName.trimmed();
    const int usbTrafficSnap = glUsbTrafficValue;
    const int gainSnap = CameraGain;
    const double offsetSnap = ImageOffset;
    const double tempSnap = CameraTemperature;

    auto reopenMainCameraForModeSwitch = [this,
                                       driverNameSnap,
                                       cameraIdSnap,
                                       poolIndexSnap,
                                       usbTrafficSnap,
                                       gainSnap,
                                       offsetSnap,
                                       tempSnap](MainCameraCaptureMode targetMode) {
        SdkSerialExecutor *mainExec = sdkMainCameraExecutor();
        if (!mainExec || !mainExec->isRunning()) {
            Logger::Log("reopenMainCameraForModeSwitch | sdkMainCamExec not running", LogLevel::WARNING, DeviceType::CAMERA);
            return;
        }
        if (driverNameSnap.isEmpty() || cameraIdSnap.isEmpty()) {
            Logger::Log("reopenMainCameraForModeSwitch | invalid driverName/cameraId | driver=" +
                            driverNameSnap.toStdString() + " cameraId=" + cameraIdSnap.toStdString(),
                        LogLevel::ERROR, DeviceType::CAMERA);
            return;
        }

        // 切换前先把“就绪态/占用态”清空，避免主线程定时器继续刷旧句柄
        sdkMainLiveReady = false;
        sdkMainBurstModeReady = false;
        sdkMainLiveFrameInFlight = false;
        sdkMainLiveNextPollMs = QDateTime::currentMSecsSinceEpoch() + 300;

        const std::string drv = driverNameSnap.toStdString();
        const std::string camId = cameraIdSnap.toStdString();
        const int desiredStreamMode = (targetMode == MainCameraCaptureMode::Single) ? 0 : 1;
        const bool wantBurst = (targetMode == MainCameraCaptureMode::Burst);
        const bool wantLive  = (targetMode == MainCameraCaptureMode::Live);
        const double expUs   = static_cast<double>(glExpTime > 0 ? glExpTime : 1) * 1000.0;
        const double usbTraffic = (usbTrafficSnap > 0) ? static_cast<double>(usbTrafficSnap) : 30.0;

        mainExec->post([this,
                        drv,
                        camId,
                        poolIndexSnap,
                        desiredStreamMode,
                        wantBurst,
                        wantLive,
                        expUs,
                        usbTraffic,
                        gainSnap,
                        offsetSnap,
                        tempSnap]() {
            bool ok = true;
            QString failStep;
            QString failMsg;

            auto logWarn = [](const std::string& msg) {
                Logger::Log(msg, LogLevel::WARNING, DeviceType::CAMERA);
            };

            // 1) Close（会同时从注册表移除 MainCamera）
            //    best-effort：即使失败也继续尝试 open（防止注册表异常导致卡死）
            (void)SdkManager::instance().closeById("MainCamera");

            // 2) Open（重新获得句柄）
            SdkResult openRes = SdkManager::instance().open(drv, camId);
            if (!openRes.success || !openRes.payload.has_value()) {
                ok = false;
                failStep = QStringLiteral("Open");
                failMsg = QString::fromStdString(openRes.message);
                QMetaObject::invokeMethod(this, [this, ok, poolIndexSnap, failStep, failMsg]() {
                    sdkMainCameraHandle = nullptr;
                    if (sdkPoolIndexValid(poolIndexSnap))
                        g_sdkQhyCamHandles[poolIndexSnap] = nullptr;
                    sdkMainLiveReady = false;
                    sdkMainBurstModeReady = false;
                    sdkMainAppliedModeValid = false;
                    Logger::Log("reopenMainCameraForModeSwitch | failed at " + failStep.toStdString() +
                                    " msg=" + failMsg.toStdString(),
                                LogLevel::ERROR, DeviceType::CAMERA);
                }, Qt::QueuedConnection);
                return;
            }
            SdkDeviceHandle newHandle = nullptr;
            try {
                newHandle = std::any_cast<SdkDeviceHandle>(openRes.payload);
            } catch (const std::bad_any_cast&) {
                ok = false;
                failStep = QStringLiteral("Open(any_cast)");
                failMsg = QStringLiteral("bad_any_cast");
            }
            if (!ok || newHandle == nullptr) {
                QMetaObject::invokeMethod(this, [this, poolIndexSnap, failStep, failMsg]() {
                    sdkMainCameraHandle = nullptr;
                    if (sdkPoolIndexValid(poolIndexSnap))
                        g_sdkQhyCamHandles[poolIndexSnap] = nullptr;
                    sdkMainLiveReady = false;
                    sdkMainBurstModeReady = false;
                    sdkMainAppliedModeValid = false;
                    Logger::Log("reopenMainCameraForModeSwitch | failed at " + failStep.toStdString() +
                                    " msg=" + failMsg.toStdString(),
                                LogLevel::ERROR, DeviceType::CAMERA);
                }, Qt::QueuedConnection);
                return;
            }

            // 3) Register（让 callByHandle 能找到驱动）
            (void)SdkManager::instance().registerDevice(drv, "MainCamera", newHandle, "主相机", std::any(camId));

            auto call = [&](const char* name, const std::any& payload) -> SdkResult {
                SdkCommand c;
                c.type = SdkCommandType::Custom;
                c.name = name;
                c.payload = payload;
                return SdkManager::instance().callByHandle(newHandle, c);
            };

            // 4) QHY SDK 标准初始化顺序（关键：StreamMode 在 Init 之前）
            // 4.1 ReadMode（best-effort，某些机型可能不支持/无意义）
            {
                SdkResult r = call("SetReadMode", 0);
                if (!r.success) {
                    logWarn("reopenMainCameraForModeSwitch | SetReadMode(0) warn: " + r.message);
                }
            }

            // 4.2 StreamMode(0/1) BEFORE Init
            {
                SdkResult r = call("SetStreamMode", desiredStreamMode);
                if (!r.success && ok) {
                    ok = false;
                    failStep = QStringLiteral("SetStreamMode(pre-Init)");
                    failMsg = QString::fromStdString(r.message);
                }
            }

            // 4.3 Init
            if (ok) {
                SdkResult r = call("InitCamera", std::any());
                if (!r.success && ok) {
                    ok = false;
                    failStep = QStringLiteral("InitCamera");
                    failMsg = QString::fromStdString(r.message);
                }
            }

            // 4.4 Bits/Debayer/Bin/USB/DDR（均为 best-effort）
            if (ok) {
                (void)call("SetBitsMode", 16);
                bool shouldDisableDebayer = false;
                {
                    SdkResult colorRes = call("IsColorCamera", std::any());
                    if (colorRes.success) {
                        try {
                            shouldDisableDebayer = std::any_cast<bool>(colorRes.payload);
                        } catch (const std::bad_any_cast&) {
                            shouldDisableDebayer = false;
                            logWarn("reopenMainCameraForModeSwitch | IsColorCamera payload cast failed, skip SetDebayerOnOff");
                        }
                    } else {
                        logWarn("reopenMainCameraForModeSwitch | IsColorCamera failed, skip SetDebayerOnOff: " + colorRes.message);
                    }
                }
                if (shouldDisableDebayer) {
                    (void)call("SetDebayerOnOff", false);
                } else {
                    Logger::Log("reopenMainCameraForModeSwitch | skip SetDebayerOnOff(false) for mono/unknown camera",
                                LogLevel::INFO, DeviceType::CAMERA);
                }
                (void)call("SetBinMode", std::pair<int,int>(1,1));
                (void)call("SetDDR", 1.0);
                (void)call("SetUsbTraffic", usbTraffic);
            }

            // 4.5 应用参数（Gain/Offset/温度，best-effort）
            if (ok) {
                (void)call("SetGain", static_cast<double>(gainSnap));
                (void)call("SetOffset", static_cast<double>(offsetSnap));
                (void)call("SetCoolerTargetTemperature", static_cast<double>(tempSnap));
            }

            // 4.6 分辨率：设置全幅（对部分机型可显著提升/稳定出帧）
            if (ok) {
                SdkAreaInfo fullRoi;
                bool haveFull = false;
                {
                    SdkResult chipRes = call("GetChipInfo", std::any());
                    if (chipRes.success) {
                        try {
                            SdkChipInfo chip = std::any_cast<SdkChipInfo>(chipRes.payload);
                            fullRoi.startX = 0;
                            fullRoi.startY = 0;
                            fullRoi.sizeX  = static_cast<unsigned int>(chip.maxImageSizeX);
                            fullRoi.sizeY  = static_cast<unsigned int>(chip.maxImageSizeY);
                            haveFull = (fullRoi.sizeX > 0 && fullRoi.sizeY > 0);
                        } catch (const std::bad_any_cast&) {
                            haveFull = false;
                        }
                    }
                }
                if (haveFull) {
                    (void)call("SetResolution", fullRoi);
                } else {
                    logWarn("reopenMainCameraForModeSwitch | GetChipInfo failed, skip SetResolution(full)");
                }
            }

            // 4.7 Exposure(us)
            if (ok) {
                SdkResult r = call("SetExposure", expUs);
                if (!r.success && ok) {
                    ok = false;
                    failStep = QStringLiteral("SetExposure");
                    failMsg = QString::fromStdString(r.message);
                }
            }

            // 5) Live/Burst：BeginLive +（可选 Burst 初始化）
            if (ok) {
                // 先确保 Burst 关闭，再按需开启（best-effort）
                if (!wantBurst) {
                    (void)call("EnableBurstMode", false);
                } else {
                    SdkResult r = call("EnableBurstMode", true);
                    if (!r.success && ok) {
                        ok = false;
                        failStep = QStringLiteral("EnableBurstMode(true)");
                        failMsg = QString::fromStdString(r.message);
                    }
                }
            }

            if (ok && desiredStreamMode == 1) {
                SdkResult r = call("BeginLive", std::any());
                if (!r.success && ok) {
                    ok = false;
                    failStep = QStringLiteral("BeginLive");
                    failMsg = QString::fromStdString(r.message);
                }
            }

            if (ok && wantBurst) {
                (void)call("SetBurstPatchNumber", static_cast<uint32_t>(32001));
                (void)call("ResetFrameCounter", std::any());
                SdkResult r = call("SetBurstIDLE", std::any());
                if (!r.success && ok) {
                    ok = false;
                    failStep = QStringLiteral("SetBurstIDLE");
                    failMsg = QString::fromStdString(r.message);
                }
            }

            // 6) 回主线程：更新句柄/池/状态
            QMetaObject::invokeMethod(this, [this, ok, newHandle, poolIndexSnap, desiredStreamMode, wantBurst, wantLive, failStep, failMsg]() {
                if (ok) {
                    sdkMainCameraHandle = newHandle;
                    if (sdkPoolIndexValid(poolIndexSnap))
                        g_sdkQhyCamHandles[poolIndexSnap] = newHandle;

                    sdkMainAppliedModeValid = true;
                    if (wantBurst) {
                        sdkMainAppliedMode = MainCameraCaptureMode::Burst;
                    } else if (wantLive) {
                        sdkMainAppliedMode = MainCameraCaptureMode::Live;
                    } else {
                        sdkMainAppliedMode = MainCameraCaptureMode::Single;
                    }

                    sdkMainLiveReady = (desiredStreamMode == 1);
                    sdkMainBurstModeReady = wantBurst;
                    sdkMainLiveNextPollMs = QDateTime::currentMSecsSinceEpoch() + 100;

                    Logger::Log("reopenMainCameraForModeSwitch | ok | streamMode=" + std::to_string(desiredStreamMode) +
                                    " burst=" + std::string(wantBurst ? "true" : "false"),
                                LogLevel::INFO, DeviceType::CAMERA);
                } else {
                    sdkMainCameraHandle = nullptr;
                    if (sdkPoolIndexValid(poolIndexSnap))
                        g_sdkQhyCamHandles[poolIndexSnap] = nullptr;
                    sdkMainLiveReady = false;
                    sdkMainBurstModeReady = false;
                    sdkMainAppliedModeValid = false;
                    Logger::Log("reopenMainCameraForModeSwitch | failed at " + failStep.toStdString() +
                                    " msg=" + failMsg.toStdString(),
                                LogLevel::ERROR, DeviceType::CAMERA);
                }
            }, Qt::QueuedConnection);
        });
    };

    if (mainCameraCaptureMode == MainCameraCaptureMode::Burst)
    {
        if (modeSwitched) {
            reopenMainCameraForModeSwitch(MainCameraCaptureMode::Burst);
            return;
        }

        // 已就绪则无需重复初始化
        // 就绪语义：已完成 EnableBurstMode(true) + SetStreamMode(1) + BeginLive + SetBurstIDLE
        if (sdkMainLiveReady.load() && sdkMainBurstModeReady.load())
            return;

        mainExec->post([this, handleSnap]() {
            // 连接后一次性进入 Burst：SetExposure + EnableBurst + StreamMode(1) + BeginLive + PatchNumber + ResetCounter + IDLE
            // 注意：这些调用必须串行在主相机 SDK 通道执行，避免跨线程/并发触碰 SDK 句柄导致不稳定。
            const double expUs = static_cast<double>(glExpTime > 0 ? glExpTime : 1) * 1000.0;
            bool ok = true;
            QString failStep;
            QString failMsg;

            // -1) 确保全分辨率 ROI 已设置（否则部分机型会出现 ret=0 但 roi=0x0，导致后续 Live/Burst 无输出）
            {
                SdkAreaInfo fullRoi;
                {
                    SdkCommand effCmd;
                    effCmd.type = SdkCommandType::Custom;
                    effCmd.name = "GetEffectiveArea";
                    effCmd.payload = std::any();
                    SdkResult effRes = SdkManager::instance().callByHandle(handleSnap, effCmd);
                    if (effRes.success) {
                        try {
                            fullRoi = std::any_cast<SdkAreaInfo>(effRes.payload);
                        } catch (const std::bad_any_cast&) {
                            fullRoi = SdkAreaInfo{};
                        }
                    } else {
                        // 回退：使用已知的主相机尺寸（尽量不阻断 Burst/Live）
                        fullRoi.startX = 0;
                        fullRoi.startY = 0;
                        fullRoi.sizeX  = (glMainCCDSizeX > 0) ? static_cast<unsigned int>(glMainCCDSizeX) : 0;
                        fullRoi.sizeY  = (glMainCCDSizeY > 0) ? static_cast<unsigned int>(glMainCCDSizeY) : 0;
                    }
                }

                if (fullRoi.sizeX > 0 && fullRoi.sizeY > 0) {
                    Logger::Log("applySdkMainCameraCaptureMode(Burst) | SetResolution(full) start",
                                LogLevel::INFO, DeviceType::CAMERA);
                    SdkCommand setResCmd;
                    setResCmd.type = SdkCommandType::Custom;
                    setResCmd.name = "SetResolution";
                    setResCmd.payload = fullRoi;
                    SdkResult setResRes = SdkManager::instance().callByHandle(handleSnap, setResCmd);
                    Logger::Log(std::string("applySdkMainCameraCaptureMode(Burst) | SetResolution(full) ") +
                                    (setResRes.success ? "ok" : "fail") + " msg=" + setResRes.message,
                                setResRes.success ? LogLevel::INFO : LogLevel::ERROR, DeviceType::CAMERA);
                    if (!setResRes.success && ok) {
                        ok = false;
                        failStep = QStringLiteral("SetResolution(full)");
                        failMsg = QString::fromStdString(setResRes.message);
                    }
                } else {
                    Logger::Log("applySdkMainCameraCaptureMode(Burst) | SetResolution(full) skipped: invalid fullRoi size",
                                LogLevel::WARNING, DeviceType::CAMERA);
                }
            }

            // SetExposure(us)
            {
                Logger::Log("applySdkMainCameraCaptureMode(Burst) | SetExposure start",
                            LogLevel::INFO, DeviceType::CAMERA);
                SdkCommand setExpCmd;
                setExpCmd.type = SdkCommandType::Custom;
                setExpCmd.name = "SetExposure";
                setExpCmd.payload = expUs;
                SdkResult res = SdkManager::instance().callByHandle(handleSnap, setExpCmd);
                Logger::Log(std::string("applySdkMainCameraCaptureMode(Burst) | SetExposure ") +
                                (res.success ? "ok" : "fail") + " msg=" + res.message,
                            res.success ? LogLevel::INFO : LogLevel::ERROR, DeviceType::CAMERA);
                if (!res.success && ok) {
                    ok = false;
                    failStep = QStringLiteral("SetExposure");
                    failMsg = QString::fromStdString(res.message);
                }
            }

            // EnableBurstMode(true)
            {
                Logger::Log("applySdkMainCameraCaptureMode(Burst) | EnableBurstMode(true) start",
                            LogLevel::INFO, DeviceType::CAMERA);
                SdkCommand en;
                en.type = SdkCommandType::Custom;
                en.name = "EnableBurstMode";
                en.payload = true;
                SdkResult res = SdkManager::instance().callByHandle(handleSnap, en);
                Logger::Log(std::string("applySdkMainCameraCaptureMode(Burst) | EnableBurstMode(true) ") +
                                (res.success ? "ok" : "fail") + " msg=" + res.message,
                            res.success ? LogLevel::INFO : LogLevel::ERROR, DeviceType::CAMERA);
                if (!res.success && ok) {
                    ok = false;
                    failStep = QStringLiteral("EnableBurstMode(true)");
                    failMsg = QString::fromStdString(res.message);
                }
            }

            // SetStreamMode(1)
            {
                Logger::Log("applySdkMainCameraCaptureMode(Burst) | SetStreamMode(1) start",
                            LogLevel::INFO, DeviceType::CAMERA);
                SdkCommand s;
                s.type = SdkCommandType::Custom;
                s.name = "SetStreamMode";
                s.payload = 1;
                SdkResult res = SdkManager::instance().callByHandle(handleSnap, s);
                Logger::Log(std::string("applySdkMainCameraCaptureMode(Burst) | SetStreamMode(1) ") +
                                (res.success ? "ok" : "fail") + " msg=" + res.message,
                            res.success ? LogLevel::INFO : LogLevel::ERROR, DeviceType::CAMERA);
                if (!res.success && ok) {
                    ok = false;
                    failStep = QStringLiteral("SetStreamMode(1)");
                    failMsg = QString::fromStdString(res.message);
                }
            }

            // BeginLive
            {
                Logger::Log("applySdkMainCameraCaptureMode(Burst) | BeginLive start",
                            LogLevel::INFO, DeviceType::CAMERA);
                SdkCommand b;
                b.type = SdkCommandType::Custom;
                b.name = "BeginLive";
                b.payload = std::any();
                SdkResult res = SdkManager::instance().callByHandle(handleSnap, b);
                Logger::Log(std::string("applySdkMainCameraCaptureMode(Burst) | BeginLive ") +
                                (res.success ? "ok" : "fail") + " msg=" + res.message,
                            res.success ? LogLevel::INFO : LogLevel::ERROR, DeviceType::CAMERA);
                if (!res.success && ok) {
                    ok = false;
                    failStep = QStringLiteral("BeginLive");
                    failMsg = QString::fromStdString(res.message);
                }
            }

            // PatchNumber（避免丢帧/无输出）
            // 经验值：QHY Burst/Live 在部分平台需要设置较大的 patch number 才稳定出帧。
            {
                Logger::Log("applySdkMainCameraCaptureMode(Burst) | SetBurstPatchNumber start",
                            LogLevel::INFO, DeviceType::CAMERA);
                SdkCommand p;
                p.type = SdkCommandType::Custom;
                p.name = "SetBurstPatchNumber";
                p.payload = static_cast<uint32_t>(32001);
                SdkResult res = SdkManager::instance().callByHandle(handleSnap, p);
                Logger::Log(std::string("applySdkMainCameraCaptureMode(Burst) | SetBurstPatchNumber ") +
                                (res.success ? "ok" : "fail") + " msg=" + res.message,
                            res.success ? LogLevel::INFO : LogLevel::ERROR, DeviceType::CAMERA);
                if (!res.success && ok) {
                    ok = false;
                    failStep = QStringLiteral("SetBurstPatchNumber");
                    failMsg = QString::fromStdString(res.message);
                }
            }

            // Reset frame counter（推荐）
            // 避免上一轮 Live 残留计数导致 start/end 规划异常。
            {
                Logger::Log("applySdkMainCameraCaptureMode(Burst) | ResetFrameCounter start",
                            LogLevel::INFO, DeviceType::CAMERA);
                SdkCommand r;
                r.type = SdkCommandType::Custom;
                r.name = "ResetFrameCounter";
                r.payload = std::any();
                SdkResult res = SdkManager::instance().callByHandle(handleSnap, r);
                Logger::Log(std::string("applySdkMainCameraCaptureMode(Burst) | ResetFrameCounter ") +
                                (res.success ? "ok" : "fail") + " msg=" + res.message,
                            res.success ? LogLevel::INFO : LogLevel::ERROR, DeviceType::CAMERA);
                if (!res.success && ok) {
                    ok = false;
                    failStep = QStringLiteral("ResetFrameCounter");
                    failMsg = QString::fromStdString(res.message);
                }
            }

            // SetBurstIDLE：进入等待触发状态（避免刚连接就出图）
            // 后续每次 Burst 拍摄通过 ReleaseBurstIDLE 触发输出。
            {
                Logger::Log("applySdkMainCameraCaptureMode(Burst) | SetBurstIDLE start",
                            LogLevel::INFO, DeviceType::CAMERA);
                SdkCommand idle;
                idle.type = SdkCommandType::Custom;
                idle.name = "SetBurstIDLE";
                idle.payload = std::any();
                SdkResult res = SdkManager::instance().callByHandle(handleSnap, idle);
                Logger::Log(std::string("applySdkMainCameraCaptureMode(Burst) | SetBurstIDLE ") +
                                (res.success ? "ok" : "fail") + " msg=" + res.message,
                            res.success ? LogLevel::INFO : LogLevel::ERROR, DeviceType::CAMERA);
                if (!res.success && ok) {
                    ok = false;
                    failStep = QStringLiteral("SetBurstIDLE");
                    failMsg = QString::fromStdString(res.message);
                }
            }

            QMetaObject::invokeMethod(this, [this, ok, failStep, failMsg]() {
                if (ok) {
                    sdkMainBurstModeReady = true;
                    sdkMainLiveReady = true;
                    // BeginLive 后给驱动一点 warm-up 时间，避免第一帧就疯狂 GetLiveFrame
                    sdkMainLiveNextPollMs = QDateTime::currentMSecsSinceEpoch() + 100;
                    Logger::Log("applySdkMainCameraCaptureMode | Burst mode ready (EnableBurst+Live+IDLE)",
                                LogLevel::INFO, DeviceType::CAMERA);
                } else {
                    sdkMainBurstModeReady = false;
                    sdkMainLiveReady = false;
                    Logger::Log("applySdkMainCameraCaptureMode | Burst init failed at " + failStep.toStdString() +
                                    " msg=" + failMsg.toStdString(),
                                LogLevel::ERROR, DeviceType::CAMERA);
                }
            }, Qt::QueuedConnection);
        });
        return;
    }

    if (mainCameraCaptureMode == MainCameraCaptureMode::Live)
    {
        if (modeSwitched) {
            reopenMainCameraForModeSwitch(MainCameraCaptureMode::Live);
            return;
        }

        // 就绪语义：Live 已开启，且 Burst 子模式关闭
        if (sdkMainLiveReady.load() && !sdkMainBurstModeReady.load())
            return;

        mainExec->post([this, handleSnap]() {
            bool ok = true;
            QString failStep;
            QString failMsg;

            // -1) 确保全分辨率 ROI 已设置（否则部分机型会出现 ret=0 但 roi=0x0，导致 Live 无输出）
            {
                SdkAreaInfo fullRoi;
                {
                    SdkCommand effCmd;
                    effCmd.type = SdkCommandType::Custom;
                    effCmd.name = "GetEffectiveArea";
                    effCmd.payload = std::any();
                    SdkResult effRes = SdkManager::instance().callByHandle(handleSnap, effCmd);
                    if (effRes.success) {
                        try {
                            fullRoi = std::any_cast<SdkAreaInfo>(effRes.payload);
                        } catch (const std::bad_any_cast&) {
                            fullRoi = SdkAreaInfo{};
                        }
                    } else {
                        // 回退：使用已知的主相机尺寸（尽量不阻断 Live）
                        fullRoi.startX = 0;
                        fullRoi.startY = 0;
                        fullRoi.sizeX  = (glMainCCDSizeX > 0) ? static_cast<unsigned int>(glMainCCDSizeX) : 0;
                        fullRoi.sizeY  = (glMainCCDSizeY > 0) ? static_cast<unsigned int>(glMainCCDSizeY) : 0;
                    }
                }

                if (fullRoi.sizeX > 0 && fullRoi.sizeY > 0) {
                    Logger::Log("applySdkMainCameraCaptureMode(Live) | SetResolution(full) start",
                                LogLevel::INFO, DeviceType::CAMERA);
                    SdkCommand setResCmd;
                    setResCmd.type = SdkCommandType::Custom;
                    setResCmd.name = "SetResolution";
                    setResCmd.payload = fullRoi;
                    SdkResult setResRes = SdkManager::instance().callByHandle(handleSnap, setResCmd);
                    Logger::Log(std::string("applySdkMainCameraCaptureMode(Live) | SetResolution(full) ") +
                                    (setResRes.success ? "ok" : "fail") + " msg=" + setResRes.message,
                                setResRes.success ? LogLevel::INFO : LogLevel::ERROR, DeviceType::CAMERA);
                    if (!setResRes.success && ok) {
                        ok = false;
                        failStep = QStringLiteral("SetResolution(full)");
                        failMsg = QString::fromStdString(setResRes.message);
                    }
                } else {
                    Logger::Log("applySdkMainCameraCaptureMode(Live) | SetResolution(full) skipped: invalid fullRoi size",
                                LogLevel::WARNING, DeviceType::CAMERA);
                }
            }

            // 0) best-effort：释放 IDLE（从 Burst 切到 Live 时避免仍处于等待触发态）
            {
                Logger::Log("applySdkMainCameraCaptureMode(Live) | ReleaseBurstIDLE (best-effort) start",
                            LogLevel::INFO, DeviceType::CAMERA);
                SdkCommand rel;
                rel.type = SdkCommandType::Custom;
                rel.name = "ReleaseBurstIDLE";
                rel.payload = std::any();
                SdkResult res = SdkManager::instance().callByHandle(handleSnap, rel);
                Logger::Log(std::string("applySdkMainCameraCaptureMode(Live) | ReleaseBurstIDLE ") +
                                (res.success ? "ok" : "fail") + " msg=" + res.message,
                            res.success ? LogLevel::INFO : LogLevel::WARNING, DeviceType::CAMERA);
            }

            // 1) SetExposure(us)：使用当前 glExpTime（Live 预览会频繁变更，后续由 setExposureTime 继续同步）
            {
                const double expUs = static_cast<double>(glExpTime > 0 ? glExpTime : 1) * 1000.0;
                Logger::Log("applySdkMainCameraCaptureMode(Live) | SetExposure start",
                            LogLevel::INFO, DeviceType::CAMERA);
                SdkCommand setExpCmd;
                setExpCmd.type = SdkCommandType::Custom;
                setExpCmd.name = "SetExposure";
                setExpCmd.payload = expUs;
                SdkResult res = SdkManager::instance().callByHandle(handleSnap, setExpCmd);
                Logger::Log(std::string("applySdkMainCameraCaptureMode(Live) | SetExposure ") +
                                (res.success ? "ok" : "fail") + " msg=" + res.message,
                            res.success ? LogLevel::INFO : LogLevel::ERROR, DeviceType::CAMERA);
                if (!res.success && ok) { ok = false; failStep = QStringLiteral("SetExposure"); failMsg = QString::fromStdString(res.message); }
            }

            // 2) Ensure Burst disabled
            {
                Logger::Log("applySdkMainCameraCaptureMode(Live) | EnableBurstMode(false) start",
                            LogLevel::INFO, DeviceType::CAMERA);
                SdkCommand dis;
                dis.type = SdkCommandType::Custom;
                dis.name = "EnableBurstMode";
                dis.payload = false;
                SdkResult res = SdkManager::instance().callByHandle(handleSnap, dis);
                Logger::Log(std::string("applySdkMainCameraCaptureMode(Live) | EnableBurstMode(false) ") +
                                (res.success ? "ok" : "fail") + " msg=" + res.message,
                            res.success ? LogLevel::INFO : LogLevel::ERROR, DeviceType::CAMERA);
                if (!res.success && ok) { ok = false; failStep = QStringLiteral("EnableBurstMode(false)"); failMsg = QString::fromStdString(res.message); }
            }

            // 3) SetStreamMode(1)
            {
                Logger::Log("applySdkMainCameraCaptureMode(Live) | SetStreamMode(1) start",
                            LogLevel::INFO, DeviceType::CAMERA);
                SdkCommand s;
                s.type = SdkCommandType::Custom;
                s.name = "SetStreamMode";
                s.payload = 1;
                SdkResult res = SdkManager::instance().callByHandle(handleSnap, s);
                Logger::Log(std::string("applySdkMainCameraCaptureMode(Live) | SetStreamMode(1) ") +
                                (res.success ? "ok" : "fail") + " msg=" + res.message,
                            res.success ? LogLevel::INFO : LogLevel::ERROR, DeviceType::CAMERA);
                if (!res.success && ok) { ok = false; failStep = QStringLiteral("SetStreamMode(1)"); failMsg = QString::fromStdString(res.message); }
            }

            // 4) BeginLive
            {
                Logger::Log("applySdkMainCameraCaptureMode(Live) | BeginLive start",
                            LogLevel::INFO, DeviceType::CAMERA);
                SdkCommand b;
                b.type = SdkCommandType::Custom;
                b.name = "BeginLive";
                b.payload = std::any();
                SdkResult res = SdkManager::instance().callByHandle(handleSnap, b);
                Logger::Log(std::string("applySdkMainCameraCaptureMode(Live) | BeginLive ") +
                                (res.success ? "ok" : "fail") + " msg=" + res.message,
                            res.success ? LogLevel::INFO : LogLevel::ERROR, DeviceType::CAMERA);
                if (!res.success && ok) { ok = false; failStep = QStringLiteral("BeginLive"); failMsg = QString::fromStdString(res.message); }
            }

            QMetaObject::invokeMethod(this, [this, ok, failStep, failMsg]() {
                if (ok) {
                    sdkMainLiveReady = true;
                    sdkMainBurstModeReady = false;
                    // BeginLive 后给驱动一点 warm-up 时间，避免第一帧就疯狂 GetLiveFrame
                    sdkMainLiveNextPollMs = QDateTime::currentMSecsSinceEpoch() + 100;
                    Logger::Log("applySdkMainCameraCaptureMode | Live mode ready (BeginLive)",
                                LogLevel::INFO, DeviceType::CAMERA);
                } else {
                    sdkMainLiveReady = false;
                    sdkMainBurstModeReady = false;
                    Logger::Log("applySdkMainCameraCaptureMode | Live init failed at " + failStep.toStdString() +
                                    " msg=" + failMsg.toStdString(),
                                LogLevel::ERROR, DeviceType::CAMERA);
                }
            }, Qt::QueuedConnection);
        });
        return;
    }

    // Single：退出 Live/Burst，恢复单帧模式
    if (modeSwitched) {
        reopenMainCameraForModeSwitch(MainCameraCaptureMode::Single);
        return;
    }

    if (!sdkMainLiveReady.load() && !sdkMainBurstModeReady.load())
        return;

    mainExec->post([this, handleSnap]() {
        // 释放 IDLE（best-effort）：切模式前先释放一次，避免相机仍处于等待触发状态导致 StopLive 不生效
        {
            Logger::Log("applySdkMainCameraCaptureMode(Single) | ReleaseBurstIDLE (best-effort) start",
                        LogLevel::INFO, DeviceType::CAMERA);
            SdkCommand rel;
            rel.type = SdkCommandType::Custom;
            rel.name = "ReleaseBurstIDLE";
            rel.payload = std::any();
            SdkResult res = SdkManager::instance().callByHandle(handleSnap, rel);
            Logger::Log(std::string("applySdkMainCameraCaptureMode(Single) | ReleaseBurstIDLE ") +
                            (res.success ? "ok" : "fail") + " msg=" + res.message,
                        res.success ? LogLevel::INFO : LogLevel::WARNING, DeviceType::CAMERA);
        }

        // StopLive：停止 Live 输出流
        {
            Logger::Log("applySdkMainCameraCaptureMode(Single) | StopLive start",
                        LogLevel::INFO, DeviceType::CAMERA);
            SdkCommand stop;
            stop.type = SdkCommandType::Custom;
            stop.name = "StopLive";
            stop.payload = std::any();
            SdkResult res = SdkManager::instance().callByHandle(handleSnap, stop);
            Logger::Log(std::string("applySdkMainCameraCaptureMode(Single) | StopLive ") +
                            (res.success ? "ok" : "fail") + " msg=" + res.message,
                        res.success ? LogLevel::INFO : LogLevel::WARNING, DeviceType::CAMERA);
        }

        // EnableBurstMode(false)：关闭 Burst 子模式
        {
            Logger::Log("applySdkMainCameraCaptureMode(Single) | EnableBurstMode(false) start",
                        LogLevel::INFO, DeviceType::CAMERA);
            SdkCommand dis;
            dis.type = SdkCommandType::Custom;
            dis.name = "EnableBurstMode";
            dis.payload = false;
            SdkResult res = SdkManager::instance().callByHandle(handleSnap, dis);
            Logger::Log(std::string("applySdkMainCameraCaptureMode(Single) | EnableBurstMode(false) ") +
                            (res.success ? "ok" : "fail") + " msg=" + res.message,
                        res.success ? LogLevel::INFO : LogLevel::WARNING, DeviceType::CAMERA);
        }

        // SetStreamMode(0)：恢复单帧模式（与 INDI/SDK 单帧曝光链路一致）
        {
            Logger::Log("applySdkMainCameraCaptureMode(Single) | SetStreamMode(0) start",
                        LogLevel::INFO, DeviceType::CAMERA);
            SdkCommand s;
            s.type = SdkCommandType::Custom;
            s.name = "SetStreamMode";
            s.payload = 0;
            SdkResult res = SdkManager::instance().callByHandle(handleSnap, s);
            Logger::Log(std::string("applySdkMainCameraCaptureMode(Single) | SetStreamMode(0) ") +
                            (res.success ? "ok" : "fail") + " msg=" + res.message,
                        res.success ? LogLevel::INFO : LogLevel::WARNING, DeviceType::CAMERA);
        }

        QMetaObject::invokeMethod(this, [this]() {
            sdkMainBurstModeReady = false;
            sdkMainLiveReady = false;
            Logger::Log("applySdkMainCameraCaptureMode | Switched to Single mode (StopLive+DisableBurst+StreamMode=0)",
                        LogLevel::INFO, DeviceType::CAMERA);
        }, Qt::QueuedConnection);
    });
}

double MainWindow::currentGuiderArcsecPerPixel() const
{
    double focalMm = guiderFocalLengthMm;
    double pixelUm = guiderPixelSizeUm;
    int bin = 1;

    const auto p = guiderParamsCache;
    if (p.guiderFocalLengthMm > 0.0)
        focalMm = p.guiderFocalLengthMm;
    if (p.guiderPixelSizeUm > 0.0)
        pixelUm = p.guiderPixelSizeUm;
    bin = std::max(1, p.guiderBinning);

    if (!(focalMm > 0.0 && pixelUm > 0.0))
        return 0.0;

    return 206.265 * (pixelUm * bin) / focalMm;
}

bool MainWindow::guiderUsesArcsecUnit() const
{
    return currentGuiderArcsecPerPixel() > 0.0;
}

void MainWindow::publishGuiderErrorUnit(bool force, bool emitInfo)
{
    const QString nextUnit = guiderUsesArcsecUnit() ? QStringLiteral("arcsec") : QStringLiteral("px");
    const double nextScale = currentGuiderArcsecPerPixel();
    const bool unitChanged = (guiderLastPublishedErrorUnit != nextUnit);
    const bool scaleChanged = std::abs(guiderLastPublishedArcsecPerPixel - nextScale) > 1e-9;

    if (!force && !unitChanged && !scaleChanged)
        return;

    guiderLastPublishedErrorUnit = nextUnit;
    guiderLastPublishedArcsecPerPixel = nextScale;

    if (wsThread)
    {
        emit wsThread->sendMessageToClient(
            QStringLiteral("GuiderErrorUnit:%1:%2")
                .arg(nextUnit)
                .arg(QString::number(nextScale, 'f', 6)));
    }

    if (!emitInfo || !unitChanged)
        return;

    if (wsThread)
    {
        if (nextUnit == QStringLiteral("arcsec"))
        {
            emit wsThread->sendMessageToClient(
                QStringLiteral("GuiderCoreInfo:导星误差单位已切换为角秒（arcsec）"));
        }
        else
        {
            emit wsThread->sendMessageToClient(
                QStringLiteral("GuiderCoreInfo:导星误差单位已切换为像素（px）"));
        }
    }
}

void MainWindow::syncGuiderScaleParams(bool forcePublishUnit, bool emitInfo)
{
    auto p = guiderParamsCache;
    p.guiderFocalLengthMm = std::max(0.0, guiderFocalLengthMm);
    p.guiderPixelSizeUm = std::max(0.0, guiderPixelSizeUm);
    p.pixelScaleArcsecPerPixel = 0.0;
    if (p.guiderFocalLengthMm > 0.0 && p.guiderPixelSizeUm > 0.0)
    {
        const double bin = std::max(1, p.guiderBinning);
        p.pixelScaleArcsecPerPixel = 206.265 * (p.guiderPixelSizeUm * bin) / p.guiderFocalLengthMm;
        guiderScaleHintSent = false;
    }
    guiderParamsCache = p;
    postGuiderCore(guiderCore, [p](GuiderCore *core) { core->setParams(p); });

    publishGuiderErrorUnit(forcePublishUnit, emitInfo);
}

void MainWindow::publishGuiderSearchBoxMode(bool emitInfo)
{
    const QString mode = guiderSearchBoxMode.isEmpty() ? QStringLiteral("AUTO") : guiderSearchBoxMode;
    const int halfSizePx = std::max(4, guiderParamsCache.guideSearchHalfSizePx);
    if (wsThread)
    {
        emit wsThread->sendMessageToClient(
            QStringLiteral("GuiderSearchBoxMode:%1:%2")
                .arg(mode)
                .arg(QString::number(halfSizePx)));
    }

    if (!emitInfo || !wsThread)
        return;

    emit wsThread->sendMessageToClient(
        QStringLiteral("GuiderCoreInfo:导星搜索框已设置为 %1（半径 %2 px）")
            .arg(mode)
            .arg(QString::number(halfSizePx)));
}

void MainWindow::onSdkMainLiveTimerTimeout()
{
    // 节流：避免 33ms 频率刷屏
    auto throttledSkipLog = [](const std::string& reason) {
        static qint64 lastMs = 0;
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs - lastMs < 2000)
            return;
        lastMs = nowMs;
        Logger::Log("onSdkMainLiveTimerTimeout | skip: " + reason, LogLevel::DEBUG, DeviceType::CAMERA);
    };

    if (!sdkMainLiveLoopOn.load()) {
        throttledSkipLog("sdkMainLiveLoopOn=false");
        return;
    }

    // 退避：BeginLive 初期/失败时降低 GetLiveFrame 频率，避免刷爆驱动导致 0xFFFFFFFF
    {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const long long nextMs = sdkMainLiveNextPollMs.load();
        if (nextMs > 0 && nowMs < nextMs) {
            throttledSkipLog("backoff active");
            return;
        }
    }

    // 仅在 Live 模式下工作
    if (mainCameraCaptureMode != MainCameraCaptureMode::Live) {
        throttledSkipLog("mode!=Live");
        return;
    }

    // 防护：拍摄/抓帧中不抢占
    if (sdkBurstActive.load() || glMainCameraStatu == "Exposuring") {
        throttledSkipLog(std::string("busy sdkBurstActive=") + (sdkBurstActive.load() ? "true" : "false") +
                         " glMainCameraStatu=" + glMainCameraStatu.toStdString());
        return;
    }

    // 仅用于主相机 SDK + QHYCCD
    const bool isMainCameraSDK =
        (systemdevicelist.system_devices.size() > 20 &&
         systemdevicelist.system_devices[DeviceSlot::MainCamera].isSDKConnect &&
         sdkMainCameraHandle != nullptr);
    auto isQhySdkDriverName = [](const QString& n) -> bool {
        const QString s = n.trimmed().toLower();
        return (s == "qhyccd" || s == "indi_qhy_ccd");
    };
    QString sdkDriverName =
        (systemdevicelist.system_devices.size() > 20) ? systemdevicelist.system_devices[DeviceSlot::MainCamera].SDKDriverName : "";
    QString effectiveSdkDriverName = sdkDriverName.trimmed();
    if (effectiveSdkDriverName.isEmpty())
        effectiveSdkDriverName = getSDKDriverName("MainCamera").trimmed();
    if (effectiveSdkDriverName.isEmpty() && systemdevicelist.system_devices.size() > 20)
        effectiveSdkDriverName = systemdevicelist.system_devices[DeviceSlot::MainCamera].DriverIndiName.trimmed();

    if (!isMainCameraSDK || !isQhySdkDriverName(effectiveSdkDriverName)) {
        throttledSkipLog(std::string("not SDK/QHY | isMainCameraSDK=") + (isMainCameraSDK ? "true" : "false") +
                         " sdkDriverName=" + sdkDriverName.toStdString() +
                         " effectiveSdkDriverName=" + effectiveSdkDriverName.toStdString());
        return;
    }

    if (!sdkMainLiveReady.load()) {
        throttledSkipLog("sdkMainLiveReady=false (BeginLive not ready yet)");
        return;
    }

    if (sdkMainLiveFrameInFlight.exchange(true)) {
        throttledSkipLog("sdkMainLiveFrameInFlight=true");
        return;
    }

    SdkSerialExecutor *mainExec = sdkMainCameraExecutor();
    if (!mainExec || !mainExec->isRunning())
    {
        sdkMainLiveFrameInFlight = false;
        throttledSkipLog("sdkMainCamExec not running");
        return;
    }

    mainExec->post([this]() {
        auto throttledFrameLog = [](const std::string& msg, LogLevel lvl) {
            static qint64 lastMs = 0;
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (nowMs - lastMs < 2000)
                return;
            lastMs = nowMs;
            Logger::Log(msg, lvl, DeviceType::CAMERA);
        };

        // 主相机可能正在重开（closeById->open->register）：不要捕获旧 handle。
        // 这里从注册表读取当前 MainCamera 句柄再调用，避免刷屏“未找到设备句柄对应的驱动”。
        SdkDeviceInfo dev = SdkManager::instance().getDevice("MainCamera");
        if (dev.handle == nullptr || dev.state != SdkDeviceState::Open) {
            sdkMainLiveNextPollMs = QDateTime::currentMSecsSinceEpoch() + 200;
            sdkMainLiveFrameInFlight = false;
            throttledFrameLog("LiveFrame | MainCamera not ready (reopening?)", LogLevel::WARNING);
            return;
        }

        // SDK 拉帧耗时：围绕 callByHandle 的同步调用耗时（微秒）
        const long long acquireStartNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

        // 1) 取一帧 Live
        SdkCommand getCmd;
        getCmd.type = SdkCommandType::Custom;
        // Live 取帧：始终取完整帧（用于写入“最新帧邮箱”与共享内存）
        // 注意：后处理（FITS/PNG/瓦片）由主线程的 sdkMainLiveProcessTimer 独立执行，不影响取帧速率
        getCmd.name = "GetLiveFrame";
        getCmd.payload = std::any();

        SdkResult frameRes = SdkManager::instance().call(dev.driverName, dev.handle, getCmd);
        const long long acquireEndNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        const long long acquireUs =
            (acquireEndNs >= acquireStartNs) ? ((acquireEndNs - acquireStartNs) / 1000LL) : -1;
        if (!frameRes.success)
        {
            throttledFrameLog("LiveFrame | GetLiveFrame failed: " + frameRes.message, LogLevel::WARNING);
            // 常见失败：BeginLive 后尚未出第一帧 / USB 抖动。做短退避，避免 33ms 疯狂调用把驱动打爆
            const std::string m = frameRes.message;
            long long backoffMs = 120;
            if (m.find("invalid frame meta") != std::string::npos) {
                backoffMs = 60;
            } else if (m.find("4294967295") != std::string::npos || m.find("0xFFFFFFFF") != std::string::npos) {
                backoffMs = 250;
            }
            sdkMainLiveNextPollMs = QDateTime::currentMSecsSinceEpoch() + backoffMs;
            // 失败也要立即释放 inFlight，避免主线程忙导致长时间卡住
            sdkMainLiveFrameInFlight = false;
            return;
        }

        // FPS 统计：以"成功取到一帧（SDK 返回 success）"为准，按 1s 窗口输出一次
        {
            static qint64 fpsWindowStartMs = 0;
            static int fpsCount = 0;
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (fpsWindowStartMs <= 0) {
                fpsWindowStartMs = nowMs;
                fpsCount = 0;
            }
            fpsCount++;
            const qint64 elapsedMs = nowMs - fpsWindowStartMs;
            if (elapsedMs >= 1000) {
                const double fps = (elapsedMs > 0) ? (static_cast<double>(fpsCount) * 1000.0 / static_cast<double>(elapsedMs)) : 0.0;
                Logger::Log("SDK Live FPS: " + std::to_string(fps), LogLevel::INFO, DeviceType::CAMERA);
                // 发送Live FPS到前端
                emit wsThread->sendMessageToClient("LiveFPS:" + QString::number(fps, 'f', 1));
                fpsWindowStartMs = nowMs;
                fpsCount = 0;
            }
        }

        // 成功出帧：取消退避
        sdkMainLiveNextPollMs = 0;

        SdkFrameData frame;
        try {
            frame = std::any_cast<SdkFrameData>(frameRes.payload);
        } catch (const std::bad_any_cast&) {
            throttledFrameLog("LiveFrame | payload any_cast failed", LogLevel::WARNING);
            sdkMainLiveNextPollMs = QDateTime::currentMSecsSinceEpoch() + 120;
            sdkMainLiveFrameInFlight = false;
            return;
        }
        const bool hasFrameData =
            (!frame.pixels.empty()) || (frame.rawBuffer != nullptr && frame.rawBytes > 0);
        if (frame.width <= 0 || frame.height <= 0 || !hasFrameData)
        {
            throttledFrameLog("LiveFrame | invalid frame meta (empty)", LogLevel::WARNING);
            sdkMainLiveNextPollMs = QDateTime::currentMSecsSinceEpoch() + 60;
            sdkMainLiveFrameInFlight = false;
            return;
        }

        auto outFrame = std::make_shared<SdkFrameData>(std::move(frame));
        // 性能/路径观测：每 2s 打一次取帧耗时与数据路径（rawBuffer=零拷贝，pixels=拷贝）
        {
            const bool hasRaw = (outFrame->rawBuffer != nullptr && outFrame->rawBytes > 0);
            const bool hasPix = (!outFrame->pixels.empty());
            throttledFrameLog(
                "LiveFrame | acquired ok | acquireUs=" + std::to_string(acquireUs) +
                    " size=" + std::to_string(outFrame->width) + "x" + std::to_string(outFrame->height) +
                    " bpp=" + std::to_string(outFrame->bpp) +
                    " ch=" + std::to_string(outFrame->channels) +
                    " path=" + std::string(hasRaw ? "rawBuffer" : (hasPix ? "pixels_copy" : "unknown")),
                LogLevel::INFO);
        }

        // 2) 写入“进程内最新帧邮箱”（不再每帧 memcpy 到 /dev/shm）：
        // - SDK 取帧线程仅做 GetLiveFrame + 轻量发布
        // - 主线程按限帧节奏消费 latestFrame 并做 FITS/PNG/瓦片
        const uint64_t seq = sdkMainLiveLatestSeq.fetch_add(1, std::memory_order_relaxed) + 1ULL;
        {
            std::lock_guard<std::mutex> lk(sdkMainLiveLatestFrameMutex);
            sdkMainLiveLatestFrame = outFrame;
        }

        // 这一次 SDK 拉帧已完成：立即释放 inFlight（避免因后续处理导致取帧降速）
        sdkMainLiveFrameInFlight = false;
    });
}

namespace {
struct LiveShmHeader {
    uint32_t magic{0};
    uint32_t version{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t bpp{0};
    uint32_t channels{0};
    uint64_t frameBytes{0};
    // 双缓冲：0/1
    // activeIndex：当前可读缓冲（写端写“非 active”那块，写完再发布 activeIndex）
    // bufSeq[i]：缓冲 i 的内容版本号（写端每次写完后递增并发布，用于读端校验读取期间未被覆盖）
    uint32_t activeIndex{0};
    uint32_t reserved0{0};
    uint64_t bufSeq0{0};
    uint64_t bufSeq1{0};
    uint64_t timestampNs{0};
};
static constexpr uint32_t kLiveShmMagic   = 0x51554859u; // 'QUHY'（仅作识别）
static constexpr uint32_t kLiveShmVersion = 1u;
} // namespace

void MainWindow::cleanupSdkMainLiveShm()
{
    if (sdkMainLiveShmPtr && sdkMainLiveShmSize > 0) {
        ::munmap(sdkMainLiveShmPtr, sdkMainLiveShmSize);
        sdkMainLiveShmPtr = nullptr;
        sdkMainLiveShmSize = 0;
    }
    if (sdkMainLiveShmFd >= 0) {
        ::close(sdkMainLiveShmFd);
        sdkMainLiveShmFd = -1;
    }
    sdkMainLiveShmFrameBytes = 0;
}

bool MainWindow::ensureSdkMainLiveShm(size_t frameBytes)
{
    if (frameBytes == 0)
        return false;

    // 如尺寸未变且已映射，直接复用
    if (sdkMainLiveShmPtr && sdkMainLiveShmFd >= 0 && sdkMainLiveShmFrameBytes == frameBytes)
        return true;

    // 重新映射（尺寸变化或首次）
    cleanupSdkMainLiveShm();

    const std::string path = vueDirectoryPath + "live_maincamera_latest.shm";
    // 双缓冲：header + 2 * frameBytes
    const size_t totalBytes = sizeof(LiveShmHeader) + (frameBytes * 2);

    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        Logger::Log("ensureSdkMainLiveShm | open failed: path=" + path +
                        " errno=" + std::to_string(errno) + " " + std::string(std::strerror(errno)),
                    LogLevel::WARNING, DeviceType::CAMERA);
        return false;
    }
    if (::ftruncate(fd, static_cast<off_t>(totalBytes)) != 0) {
        Logger::Log("ensureSdkMainLiveShm | ftruncate failed: bytes=" + std::to_string(totalBytes) +
                        " errno=" + std::to_string(errno) + " " + std::string(std::strerror(errno)),
                    LogLevel::WARNING, DeviceType::CAMERA);
        ::close(fd);
        return false;
    }

    void* p = ::mmap(nullptr, totalBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        Logger::Log("ensureSdkMainLiveShm | mmap failed: bytes=" + std::to_string(totalBytes) +
                        " errno=" + std::to_string(errno) + " " + std::string(std::strerror(errno)),
                    LogLevel::WARNING, DeviceType::CAMERA);
        ::close(fd);
        return false;
    }

    sdkMainLiveShmFd = fd;
    sdkMainLiveShmPtr = p;
    sdkMainLiveShmSize = totalBytes;
    sdkMainLiveShmFrameBytes = frameBytes;

    // 初始化 header（magic/version 等）
    auto* hdr = reinterpret_cast<LiveShmHeader*>(sdkMainLiveShmPtr);
    std::memset(hdr, 0, sizeof(LiveShmHeader));
    hdr->magic = kLiveShmMagic;
    hdr->version = kLiveShmVersion;
    hdr->frameBytes = static_cast<uint64_t>(frameBytes);
    hdr->activeIndex = 0;
    hdr->bufSeq0 = 0;
    hdr->bufSeq1 = 0;
    return true;
}

void MainWindow::writeSdkMainLiveShm(const SdkFrameData& frame, uint64_t seq)
{
    // 仅在 Live 循环开启时写；切模式/关闭预览时不写，避免无谓 IO
    // 注意：这里避免读取非原子 mainCameraCaptureMode（跨线程），以免触发数据竞态
    if (!sdkMainLiveLoopOn.load())
        return;

    const bool hasVecPixels = !frame.pixels.empty();
    const bool hasRawPixels = (frame.rawBuffer != nullptr && frame.rawBytes > 0);
    if (!hasVecPixels && !hasRawPixels)
        return;

    const size_t frameBytes =
        hasRawPixels ? frame.rawBytes : (frame.pixels.size() * sizeof(uint16_t));
    if (!ensureSdkMainLiveShm(frameBytes))
        return;

    auto* hdr = reinterpret_cast<LiveShmHeader*>(sdkMainLiveShmPtr);
    unsigned char* base = reinterpret_cast<unsigned char*>(sdkMainLiveShmPtr) + sizeof(LiveShmHeader);
    unsigned char* buf0 = base;
    unsigned char* buf1 = base + frameBytes;

    // 双缓冲 + 原子索引（GCC builtins，跨线程/跨进程可用）
    // - 读端只读 activeIndex 指向的缓冲
    // - 写端写非 active 的缓冲，写完发布 bufSeq + activeIndex
    const uint32_t curActive = __atomic_load_n(&hdr->activeIndex, __ATOMIC_ACQUIRE);
    const uint32_t writeIdx = (curActive == 0u) ? 1u : 0u;
    unsigned char* dst = (writeIdx == 0u) ? buf0 : buf1;
    hdr->width = static_cast<uint32_t>(std::max(0, frame.width));
    hdr->height = static_cast<uint32_t>(std::max(0, frame.height));
    hdr->bpp = frame.bpp;
    hdr->channels = frame.channels;
    hdr->frameBytes = static_cast<uint64_t>(frameBytes);

    if (hasRawPixels) {
        std::memcpy(dst, frame.rawBuffer->data(), frameBytes);
    } else {
        std::memcpy(dst, frame.pixels.data(), frameBytes);
    }

    const uint64_t tsNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    hdr->timestampNs = tsNs;

    // 发布：先发布该缓冲的 seq（release），再切换 activeIndex（release）
    if (writeIdx == 0u) {
        __atomic_store_n(&hdr->bufSeq0, seq, __ATOMIC_RELEASE);
    } else {
        __atomic_store_n(&hdr->bufSeq1, seq, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&hdr->activeIndex, writeIdx, __ATOMIC_RELEASE);
}

void MainWindow::onSdkMainLiveProcessTimerTimeout()
{
    if (!sdkMainLiveLoopOn.load())
        return;
    if (mainCameraCaptureMode != MainCameraCaptureMode::Live)
        return;
    if (!sdkMainLiveReady.load())
        return;
    if (sdkMainCameraHandle == nullptr)
        return;

    // 若上一帧仍在处理（比如 saveFitsAsPNG 很慢），本次直接跳过；取帧仍在继续覆盖邮箱
    if (sdkMainLiveProcessingBusy.load())
        return;

    const uint64_t latestSeq = sdkMainLiveLatestSeq.load(std::memory_order_relaxed);
    const uint64_t processedSeq = sdkMainLiveProcessedSeq.load(std::memory_order_relaxed);
    if (latestSeq == 0 || latestSeq == processedSeq)
        return;

    // Live 处理链路限帧：只控制“FITS/PNG/瓦片”链路，不影响 SDK 拉帧与 FPS 统计
    {
        const int maxFps = (sdkMainLiveMaxProcessFps > 0) ? sdkMainLiveMaxProcessFps : 0;
        if (maxFps > 0) {
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            const qint64 minIntervalMs = std::max<qint64>(1, 1000 / static_cast<qint64>(maxFps));
            const qint64 lastMs = static_cast<qint64>(sdkMainLiveLastProcessMs.load());
            if (lastMs > 0 && (nowMs - lastMs) < minIntervalMs) {
                return;
            }
            sdkMainLiveLastProcessMs = static_cast<long long>(nowMs);
        }
    }

    // 从“进程内最新帧邮箱”读取最新一帧（零拷贝/少拷贝）：
    // 说明：SdkFrameData 内部像素由 shared_ptr 持有（rawBuffer 或 pixels），跨线程安全共享所有权。
    std::shared_ptr<SdkFrameData> framePtr;
    {
        std::lock_guard<std::mutex> lk(sdkMainLiveLatestFrameMutex);
        framePtr = sdkMainLiveLatestFrame;
    }
    if (!framePtr)
        return;
    const SdkFrameData& frame = *framePtr;
    const bool hasVecPixels = !frame.pixels.empty();
    const bool hasRawPixels = (frame.rawBuffer != nullptr && frame.rawBytes > 0);
    if (frame.width <= 0 || frame.height <= 0 || (!hasVecPixels && !hasRawPixels))
        return;

    // 标记为“已消费到该 seq”（允许后续取到更晚的帧）
    sdkMainLiveProcessedSeq = latestSeq;

    // 处理帧率统计：以“成功拿到一帧并进入处理链路（写 FITS/PNG/瓦片）”为准
    {
        static qint64 procFpsWindowStartMs = 0;
        static int procFpsCount = 0;
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (procFpsWindowStartMs <= 0) {
            procFpsWindowStartMs = nowMs;
            procFpsCount = 0;
        }
        procFpsCount++;
        const qint64 elapsedMs = nowMs - procFpsWindowStartMs;
        if (elapsedMs >= 1000) {
            const double fps =
                (elapsedMs > 0) ? (static_cast<double>(procFpsCount) * 1000.0 / static_cast<double>(elapsedMs)) : 0.0;
            Logger::Log("SDK Live Process FPS: " + std::to_string(fps), LogLevel::INFO, DeviceType::CAMERA);
            emit wsThread->sendMessageToClient("LiveProcessFPS:" + QString::number(fps, 'f', 1));
            procFpsWindowStartMs = nowMs;
            procFpsCount = 0;
        }
    }

    const std::string fitsPath = "/dev/shm/ccd_simulator.fits";
    SaveQhyFrameDataToFits(frame, fitsPath);

    // 复用前端协议：用 ExposureCompleted 作为“刷新一帧”的信号
    emit wsThread->sendMessageToClient("ExposureCompleted");
    emitCaptureTrace(QStringLiteral("backend_exposure_completed"), currentCaptureTraceStartedAtMs,
                     QStringLiteral("source=sdk_live_process"));

    // 标记处理忙：处理完成前新帧将继续覆盖邮箱，但不会触发新的处理任务排队
    sdkMainLiveProcessingBusy = true;
    saveFitsAsPNG(QString::fromStdString(fitsPath), true);
}


bool MainWindow::hasProp(INDI::BaseDevice *dev, const char *prop)
{
    return dev && static_cast<bool>(dev->getProperty(prop));
}

// 工具函数：检查多个属性是否存在其中之一
bool MainWindow::hasAnyProp(INDI::BaseDevice *dev, std::initializer_list<const char*> props)
{
    for (auto p : props)
    {
        if (hasProp(dev, p))
            return true;
    }
    return false;
}

bool MainWindow::isDSLR(INDI::BaseDevice *device)
{
    if (!device) return false;

    // 转小写便于匹配
    auto toLower = [](QString s){ return s.toLower(); };
    QString drvExec = toLower(QString::fromUtf8(device->getDriverExec()));
    QString devName = toLower(QString::fromUtf8(device->getDeviceName()));

    auto nameHas = [&](const QString& key){
        return drvExec.contains(key) || devName.contains(key);
    };

    bool nameLooksDSLR = nameHas("gphoto") || nameHas("dslr");

    // 1) 反证：典型 SDK/制冷相机属性，若存在则优先判定为非 DSLR
    bool looksLikeSDKCam = hasAnyProp(device, {
        "CCD_COOLER", "CCD_COOLER_MODE", "CCD_COOLER_POWER", "CCD_HUMIDITY",
        "CCD_GAIN", "CCD_OFFSET", "USB_TRAFFIC", "USB_BUFFER",
        "SDK_VERSION", "READ_MODE"
    });
    if (looksLikeSDKCam)
    {
        Logger::Log("SDK/Coooler/Gain type properties found, treat as non-DSLR.",
                    LogLevel::INFO, DeviceType::MAIN);
        return false;
    }

    // 2) 正证：单反/微单常见属性
    bool dslrProps = hasAnyProp(device, {
        "ISO", "CCD_ISO", "APERTURE", "WB", "WHITE_BALANCE",
        "CAPTURE_TARGET", "IMAGE_FORMAT", "LIVEVIEW", "LIVE_VIEW", "FOCUS_MODE"
    });

    if (dslrProps)
    {
        Logger::Log("Found DSLR-specific properties, treat as DSLR.",
                    LogLevel::INFO, DeviceType::MAIN);
        return true;
    }

    // 3) 兜底：驱动名提示
    if (nameLooksDSLR)
    {
        Logger::Log("Driver name contains DSLR/GPhoto, treat as DSLR.",
                    LogLevel::INFO, DeviceType::MAIN);
        return true;
    }

    return false;
}


void MainWindow::ConnectDriver(QString DriverName, QString DriverType)
{
    Logger::Log("ConnectDriver(" + DriverName.toStdString() + ", " + DriverType.toStdString() + ") start ...", LogLevel::INFO, DeviceType::MAIN);
    if (DriverName == "" || DriverType == "")
    {
        Logger::Log("ConnectDriver | DriverName(" + DriverName.toStdString() + ") or DriverType(" + DriverType.toStdString() + ") is Null", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectFailed:DriverName or DriverType is Null.");
        emit wsThread->sendMessageToClient("ConnectDriverFailed:DriverName or DriverType is Null.");
        return;
    }

    // ===== 参数防护：检测前端误传 "SDK"/"INDI" 占位符作为 DriverName =====
    // 前端已修复（App.vue），但保留后端检测，避免旧版本前端或调试时触发错误连接。
    const QString upperName = DriverName.trimmed().toUpper();
    if (upperName == "SDK" || upperName == "INDI")
    {
        Logger::Log("ConnectDriver | Invalid DriverName placeholder: " + DriverName.toStdString() +
                        ". Frontend should send real INDI driver name (e.g. indi_qhy_focuser) instead of mode keyword.",
                    LogLevel::ERROR, DeviceType::MAIN);
        if (wsThread)
        {
            emit wsThread->sendMessageToClient("ConnectFailed:Invalid driver name placeholder (SDK/INDI). Please update frontend.");
            emit wsThread->sendMessageToClient("ConnectDriverFailed:Invalid driver name placeholder (SDK/INDI).");
        }
        return;
    }

    int driverCode = -1;
    bool isDriverConnected = false;

    // 关键字/配置判断：若对应 SystemDevice 标记为 SDK 连接，则走 SDK 流程
    auto isSDKType = [&](const QString &type) -> bool {
        // 1) 显式关键字：DriverType 中包含 "SDK"（不区分大小写）
        if (type.contains("SDK", Qt::CaseInsensitive))
            return true;

        // 2) 在系统设备列表中存在同类型且 isSDKConnect 标记为 true 的槽位
        for (const auto &dev : systemdevicelist.system_devices)
        {
            if (dev.Description == type && dev.isSDKConnect)
                return true;
        }
        return false;
    };

    bool requestedSdk = isSDKType(DriverType);

    // ===== 连接模式锁定兜底：已连接(或同驱动已连接)时拒绝用另一模式再次连接 =====
    auto findDeviceIndexByDesc = [&](const QString& desc) -> int {
        for (int i = 0; i < systemdevicelist.system_devices.size(); ++i) {
            if (systemdevicelist.system_devices[i].Description == desc) return i;
        }
        return -1;
    };
    const QString driverTypeDesc = DriverType.section(':', 0, 0).trimmed(); // e.g. "Guider:SDK" -> "Guider"

    // ===== 自动降级：驱动不支持 SDK 时，本次连接改为 INDI =====
    // 场景：
    // - 前端显式请求 "xxx:SDK"
    // - 或 systemdevicelist 里该设备槽位残留 isSDKConnect=true（例如换成 indi_simulator_ccd 后未清空）
    // 此时若 DriverName 对应的 INDI 驱动本身不支持 SDK，应自动切回 INDI，避免前端卡在“连接中/失败未回包”的状态。
    if (requestedSdk)
    {
        const std::string sdkName = SdkDriverRegistry::instance().getSDKDriverName(DriverName.toStdString());
        if (sdkName.empty())
        {
            Logger::Log("ConnectDriver | Requested SDK but driver does not support SDK: " + DriverName.toStdString() +
                            ". Fallback to INDI connection.",
                        LogLevel::WARNING, DeviceType::MAIN);

            requestedSdk = false;

            auto resolveSlotIndex = [&](const QString& desc) -> int {
                int idx = findDeviceIndexByDesc(desc);
                if (idx >= 0) return idx;
                if (desc == "MainCamera") return (systemdevicelist.system_devices.size() > 20) ? 20 : -1;
                if (desc == "Guider")    return (systemdevicelist.system_devices.size() > 1)  ? 1  : -1;
                if (desc == "CFW")       return (systemdevicelist.system_devices.size() > 21) ? 21 : -1;
                if (desc == "Focuser")   return (systemdevicelist.system_devices.size() > 22) ? 22 : -1;
                return -1;
            };

            const int slotIdx = resolveSlotIndex(driverTypeDesc);
            if (slotIdx >= 0 && slotIdx < systemdevicelist.system_devices.size())
            {
                systemdevicelist.system_devices[slotIdx].isSDKConnect = false;
                // 保持 DriverFrom/SDKDriverName 表示“是否支持 SDK”的语义，不在此处强行改写
                Tools::saveSystemDeviceList(systemdevicelist);
            }

            if (wsThread)
            {
                // 通知前端：本次自动切换到 INDI 模式（避免 UI 仍显示 SDK）
                emit wsThread->sendMessageToClient("SetConnectionModeSuccess:" + driverTypeDesc + ":INDI");
            }
        }
    }

    const bool isMainOrGuider = (driverTypeDesc == "MainCamera" || driverTypeDesc == "Guider");

    if (isMainOrGuider)
    {
        const int idxMain = findDeviceIndexByDesc("MainCamera");
        const int idxGuider = findDeviceIndexByDesc("Guider");

        const bool mainConnected = (idxMain >= 0) && systemdevicelist.system_devices[idxMain].isConnect;
        const bool guiderConnected = (idxGuider >= 0) && systemdevicelist.system_devices[idxGuider].isConnect;

        // ── M3：已拆除"同驱动任一已连接就锁定整组模式"的第三道锁 ──────────────────
        // 原先：Main 或 Guider 任一已连接时，另一个只能用【相同】模式连接，否则报
        // ConnectDriverFailed:<Role>:ConnectedInOtherMode。这挡死了"Main=SDK + Guider=INDI"。
        //
        // 该约束和 SetConnectionMode 里的两道锁同源，都是为了回避"SDK 会 open 全部相机、
        // 抢走本该留给 INDI 的那台"——而那是代码自造的冲突，已由 M2 消除
        // （registerSdkCameraPool 只登记不 open；ensureSdkCameraOpen 只在绑定时打开选中那台）。
        // 硬件层面本就允许不同物理相机分别走 SDK/INDI（见 doc §7：库全局状态每进程私有，
        // 排他只发生在 OpenQHYCCD/同一台设备上）。
        //
        // 保留的正确约束（在别处保证）：
        //   1) 同一台物理相机不得被双开 —— 一台相机只绑一个角色（cameraId 在池中唯一对应角色）；
        //   2) 已连接的设备不许改模式 —— SetConnectionMode 的 DeviceConnectedLockModeChangeForbidden；
        //   3) 同一设备不得用另一模式重复连接 —— 见下方"单设备兜底"。
    }

    // 单设备兜底：若该设备已连接，则不允许用另一模式重复连接。
    //
    // 注意 isConnect 与 isBind 是两层、不是一回事：
    //   isConnect —— 驱动/设备层：驱动在跑、设备可用；
    //   isBind    —— 角色分配层：这个角色当前绑了哪一台。
    // UnBindingDevice 只退出分配层（isBind=false），驱动仍在跑（isConnect 保持 true，
    // 前端卡片才不会消失）。所以"已连接"绝不等于"无事可做"。
    {
        const int idxDesc = findDeviceIndexByDesc(driverTypeDesc);
        if (idxDesc >= 0 && systemdevicelist.system_devices[idxDesc].isConnect)
        {
            const bool currentSdk = systemdevicelist.system_devices[idxDesc].isSDKConnect;
            if (currentSdk != requestedSdk)
            {
                Logger::Log("ConnectDriver | Device " + driverTypeDesc.toStdString() + " already connected with " +
                                std::string(currentSdk ? "SDK" : "INDI") + ". Connecting with the other mode is forbidden.",
                            LogLevel::WARNING, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:" + driverTypeDesc + ":ConnectedInOtherMode");
                return;
            }

            if (systemdevicelist.system_devices[idxDesc].isBind)
            {
                // 已连接【且仍绑着】才是真正的幂等"无事可做"。
                Logger::Log("ConnectDriver | Device " + driverTypeDesc.toStdString() + " already connected and bound (" +
                                std::string(currentSdk ? "SDK" : "INDI") + "). Skip re-connect.",
                            LogLevel::INFO, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("ConnectDriverSuccess:" + DriverName);
                return;
            }

            // 已连接但未绑定（典型：刚解绑）——用户点"连接"要的正是候选列表，
            // 这里不能 return，否则该角色再也分配不上。继续走下面的常规流程：
            // 它本身幂等（驱动已在跑会被跳过），只会重新上报未绑定的候选。
            Logger::Log("ConnectDriver | Device " + driverTypeDesc.toStdString() + " already connected but unbound (" +
                            std::string(currentSdk ? "SDK" : "INDI") + "). Re-report candidates.",
                        LogLevel::INFO, DeviceType::MAIN);
        }
    }

    if (requestedSdk)
    {
        Logger::Log("ConnectDriver | detected SDK connect type for DriverType=" + DriverType.toStdString(), LogLevel::INFO, DeviceType::MAIN);

        // 目前 SDK 连接主要面向相机，这里先处理 MainCamera 相关类型（多相机：全部打开 + 走设备分配逻辑）
        if (DriverType.contains("MainCamera", Qt::CaseInsensitive))
        {
            Logger::Log("ConnectDriver | Use SDK connection for MainCamera.", LogLevel::INFO, DeviceType::MAIN);
            bool needAllocation = false;
            bool boundByThisCall = false;

            // 标记主相机槽位为 SDK 连接，以便后续"全部连接"流程识别
            if (systemdevicelist.system_devices.size() > 20)
            {
                systemdevicelist.system_devices[DeviceSlot::MainCamera].Description = "MainCamera";
                systemdevicelist.system_devices[DeviceSlot::MainCamera].isSDKConnect = true;
                systemdevicelist.system_devices[DeviceSlot::MainCamera].DriverFrom = "SDK";
            }

            // 使用新 SdkManager 框架连接 SDK 相机
            // 1. 初始化 SDK 资源
            SdkCommand initCmd;
            initCmd.type = SdkCommandType::Custom;
            initCmd.name = "InitSdkResource";
            initCmd.payload = std::any();
            // 对于nullptr句柄，使用getSDKDriverName动态获取驱动名称
            QString driverName = getSDKDriverName("MainCamera");
            if (driverName.isEmpty()) {
                Logger::Log("ConnectDriver | Cannot get SDK driver name for MainCamera",
                            LogLevel::ERROR, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:MainCamera:Cannot get SDK driver name");
                return;
            }
            SdkResult initRes = SdkManager::instance().call(driverName.toStdString(), nullptr, initCmd);
            if (!initRes.success) {
                Logger::Log("ConnectDriver | InitSdkResource failed: " + initRes.message, 
                           LogLevel::ERROR, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:MainCamera:SDK init failed");
                return;
            }

            // 2. 扫描相机
            // 对于nullptr句柄，使用getSDKDriverName动态获取驱动名称
            driverName = getSDKDriverName("MainCamera");
            if (driverName.isEmpty()) {
                Logger::Log("ConnectDriver | Cannot get SDK driver name for MainCamera",
                            LogLevel::ERROR, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:MainCamera:Cannot get SDK driver name");
                return;
            }
            SdkResult scanRes = sdkScanQhyCameras(driverName);
            if (!scanRes.success || !scanRes.payload.has_value()) {
                Logger::Log("ConnectDriver | ScanCameras failed: " + scanRes.message, 
                           LogLevel::ERROR, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:MainCamera:SDK scan failed");
                return;
            }

            // 3. 获取相机数量并打开全部相机（多相机场景：触发设备分配逻辑）
            int cameraCount = std::any_cast<int>(scanRes.payload);
            if (cameraCount < 1) {
                Logger::Log("ConnectDriver | No QHYCCD camera found.", LogLevel::ERROR, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:MainCamera:No camera found");
                return;
            }

            // 清理旧的句柄池（若存在）
            if (!g_sdkQhyCamHandles.isEmpty())
            {
                Logger::Log("ConnectDriver | SDK camera pool already exists, cleaning up previous handles ...", LogLevel::WARNING, DeviceType::MAIN);
                
                // -----------------------------
                // 关键修复：避免“重连主相机”时清理相机池导致其它相机角色掉线
                //
                // 现象：主相机走 SDK 重连时，若相机池已存在，会调用 CameraPool 清理，
                // 同时清空 g_sdkGuiderPoolIndex/sdGuiderHandle，导致导星等其它角色被误重置。
                //
                // 策略：
                // - 若相机池正在被其它角色占用（Guider/PoleCamera 等），则禁止 CameraPool 清理；
                // - 直接复用现有池，尝试为当前角色（MainCamera）在池中选择一个“未被占用”的句柄进行绑定；
                // - 若无法自动选择，则让前端弹出分配窗口，由用户在多个相机中分配。
                // -----------------------------
                const bool poolLooksValid =
                    (g_sdkQhyCamHandles.size() == g_sdkQhyCamIds.size());
                bool poolHasCorruption = false;
                if (poolLooksValid)
                {
                    // 允许“已释放槽位”：handle==nullptr 时 id 允许为空
                    for (int i = 0; i < g_sdkQhyCamHandles.size(); ++i)
                    {
                        if (g_sdkQhyCamHandles[i] != nullptr && g_sdkQhyCamIds[i].isEmpty())
                        {
                            poolHasCorruption = true;
                            break;
                        }
                    }
                }
                const bool poolInUseByOtherRole =
                    (sdkGuiderHandle != nullptr) || (g_sdkGuiderPoolIndex >= 0) ||
                    (sdkPoleScopeHandle != nullptr);

                if (poolInUseByOtherRole)
                {
                    if (!poolLooksValid || poolHasCorruption)
                    {
                        Logger::Log("ConnectDriver | SDK camera pool exists but is inconsistent while other role is using it. "
                                    "Skip cleanup to avoid disconnecting other devices; request user allocation/restart.",
                                    LogLevel::ERROR, DeviceType::MAIN);
                        emit wsThread->sendMessageToClient("ConnectDriverFailed:MainCamera:SDK pool inconsistent while other device uses it");
                        return;
                    }

                    Logger::Log("ConnectDriver | SDK camera pool is in use by other role(s) (e.g. Guider). "
                                "Skip CameraPool cleanup and reuse existing pool for MainCamera binding.",
                                LogLevel::WARNING, DeviceType::MAIN);

                    // [方案B修复-对称] 池复用重连 MainCamera：先“补齐”池——把 scan 到但未打开的相机
                    // （尤其是本角色断开时被 close 的那台）重新 open 进池；已在池中打开的（含 Guider 占用）跳过。
                    // M2：池复用“补齐”——只登记扫描到但尚未在池中的相机（幂等、不 open）。
                    registerSdkCameraPool(driverName);

                    // [方案B修复-对称] 若 MainCamera 保留了上次绑定的 cameraId 且已在复用池中，直接自动回绑，
                    // 恢复“断开后一键重连”；找不到匹配（相机全新/有歧义）时才进入下面的“等待用户手动分配”。
                    {
                        const QString savedMainCameraId =
                            (systemdevicelist.system_devices.size() > 20)
                                ? systemdevicelist.system_devices[DeviceSlot::MainCamera].DeviceIndiName.trimmed()
                                : QString();
                        if (!savedMainCameraId.isEmpty())
                        {
                            for (int i = 0; i < g_sdkQhyCamIds.size(); ++i)
                            {
                                if (i == g_sdkGuiderPoolIndex)
                                    continue;
                                if (g_sdkQhyCamIds[i] == savedMainCameraId && sdkPoolIndexValid(i))
                                {
                                    Logger::Log("ConnectDriver | Reconnect saved SDK MainCamera (pool reuse): " +
                                                    savedMainCameraId.toStdString(),
                                                LogLevel::INFO, DeviceType::MAIN);
                                    BindingDevice("MainCamera", sdkUiIndexFromPoolIndex(i));
                                    emit wsThread->sendMessageToClient("ConnectDriverSuccess:" + DriverName);
                                    return;
                                }
                            }
                            Logger::Log("ConnectDriver | Saved SDK MainCamera not found in reused pool, waiting for selection: " +
                                            savedMainCameraId.toStdString(),
                                        LogLevel::WARNING, DeviceType::MAIN);
                        }
                    }

                    // [修改] 移除poolInUseByOtherRole场景下MainCamera自动绑定
                    // 即使复用已有池，也等待用户手动选择
                    {
                        if (systemdevicelist.system_devices.size() > 20)
                        {
                            systemdevicelist.system_devices[DeviceSlot::MainCamera].isConnect = false;
                            systemdevicelist.system_devices[DeviceSlot::MainCamera].isBind = false;
                        }
                        needAllocation = true;
                        Logger::Log("ConnectDriver | SDK MainCamera (pool reuse): waiting for user allocation.",
                                    LogLevel::INFO, DeviceType::MAIN);
                    }

                    // 为池中所有未被Guider占用的相机发送DeviceToBeAllocated
                    for (int i = 0; i < g_sdkQhyCamHandles.size(); ++i)
                    {
                        if (g_sdkQhyCamHandles[i] == nullptr)
                            continue;
                        if (i == g_sdkGuiderPoolIndex)
                            continue;
                        const int uiIdx = sdkUiIndexFromPoolIndex(i);
                        QString qid_cat;
                        if (g_sdkQhyCamIds[i].contains("5III", Qt::CaseInsensitive))
                            qid_cat = "5III";
                        else if (g_sdkQhyCamIds[i].contains("DEMO", Qt::CaseInsensitive))
                            qid_cat = "DEMO";
                        else
                            qid_cat = "OTHER";
                        emit wsThread->sendMessageToClient("DeviceToBeAllocated:CCD:" + QString::number(uiIdx) + ":" + g_sdkQhyCamIds[i] + ":" + qid_cat);
                    }

                    if (boundByThisCall)
                    {
                        emit wsThread->sendMessageToClient("AddDeviceType:MainCamera");
                        emit wsThread->sendMessageToClient("ConnectDriverSuccess:" + DriverName);
                    }
                    else if (needAllocation)
                    {
                        // 多相机未分配时不要提前宣告连接成功，避免前端立刻下发曝光命令导致空句柄访问
                        emit wsThread->sendMessageToClient("AddDeviceType:MainCamera");
                        emit wsThread->sendMessageToClient("ConnectDriverPendingAllocation:MainCamera");
                    }
                    return;
                }

                // 修复：只清理相机池（CameraPool），而不是所有设备（All）
                // 这样可以避免影响其他独立的SDK设备（如调焦器Focuser）
                // 调焦器使用独立的SDK驱动（"indi_qhy_focuser"），不应该被相机池的清理影响
                // 使用异步清理函数，避免主线程阻塞
                // 清理整个相机池，为重新连接做准备
                cleanupQhySdkPoolAndResource("Reconnect: cleaning up old pool before new scan", "CameraPool");
                
                // 等待清理完成（检查所有句柄是否都为 nullptr，最多等待2秒）
                int waitCount = 0;
                const int maxWaitCount = 20;  // 20 * 100ms = 2秒
                bool allHandlesNull = false;
                
                while (waitCount < maxWaitCount)
                {
                    allHandlesNull = true;
                    for (int i = 0; i < g_sdkQhyCamHandles.size(); ++i)
                    {
                        if (g_sdkQhyCamHandles[i] != nullptr)
                        {
                            allHandlesNull = false;
                            break;
                        }
                    }
                    
                    if (allHandlesNull)
                        break;
                    
                    QThread::msleep(100);
                    waitCount++;
                    
                    // 每500ms输出一次等待日志
                    if (waitCount % 5 == 0)
                    {
                        Logger::Log("ConnectDriver | Waiting for SDK cleanup to complete... (" + 
                                   std::to_string(waitCount * 100) + "ms elapsed)", 
                                   LogLevel::INFO, DeviceType::MAIN);
                    }
                }
                
                // 强制清空池引用
                g_sdkQhyCamHandles.clear();
                g_sdkQhyCamIds.clear();
                g_sdkMainCameraPoolIndex = -1;
                g_sdkGuiderPoolIndex = -1;
                g_sdkPoleCameraPoolIndex = -1;
                sdkMainCameraHandle = nullptr;
                sdkGuiderHandle = nullptr;
                sdkPoleScopeHandle = nullptr;
                sdkMainCameraId.clear();
                
                if (!allHandlesNull)
                {
                    Logger::Log("ConnectDriver | SDK cleanup timeout after " + 
                               std::to_string(waitCount * 100) + "ms, force cleared pool references", 
                               LogLevel::WARNING, DeviceType::MAIN);
                }
                else
                {
                    Logger::Log("ConnectDriver | SDK cleanup completed in " + 
                               std::to_string(waitCount * 100) + "ms", 
                               LogLevel::INFO, DeviceType::MAIN);
                }
                
                // 额外等待，确保 SDK 线程中的 close 和 ReleaseSdkResource 完成
                // 这很重要：cleanupQhySdkPoolAndResource 会立即将句柄设为 nullptr（主线程），
                // 但实际的 close 操作在 SDK 线程中异步执行，需要时间完成
                QThread::msleep(800);
            }

            // 4. 依次获取 cameraId 并 open，全部加入“待分配设备列表”
            // ── M2：扫描登记（不 open）+ 上报候选 ─────────────────────────────
            // ScanQHYCCD/GetQHYCCDId 只枚举、不占用设备；真正的排他是 OpenQHYCCD。
            // 旧流程在此 open 全部相机 -> 占住本该留给 INDI 的相机（混用双开冲突的自造
            // 根源），且同段逻辑全仓复制 6 份。现统一走 registerSdkCameraPool（只登记
            // cameraId、句柄留空）+ reportSdkCameraCandidates；真正的 open 推迟到
            // BindingDevice -> ensureSdkCameraOpen（只开被分配的那台）。
            const int poolValid = registerSdkCameraPool(driverName);
            if (poolValid < 1)
            {
                Logger::Log("ConnectDriver | SDK scan/register produced no camera for MainCamera.",
                            LogLevel::ERROR, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:MainCamera:No camera found");
                return;
            }
            reportSdkCameraCandidates();

            // 单设备重连：持久化选择已经确定时，扫描后按 cameraId 恢复连接。
            // 只有首次选择或原设备未扫描到时，才进入候选选择流程。
            const QString savedMainCameraId = systemdevicelist.system_devices[DeviceSlot::MainCamera].DeviceIndiName.trimmed();
            if (!savedMainCameraId.isEmpty())
            {
                for (int i = 0; i < g_sdkQhyCamIds.size(); ++i)
                {
                    if (g_sdkQhyCamIds[i] == savedMainCameraId && sdkPoolIndexValid(i))
                    {
                        Logger::Log("ConnectDriver | Reconnect saved SDK MainCamera: " +
                                        savedMainCameraId.toStdString(),
                                    LogLevel::INFO, DeviceType::CAMERA);
                        BindingDevice("MainCamera", sdkUiIndexFromPoolIndex(i));
                        emit wsThread->sendMessageToClient("ConnectDriverSuccess:" + DriverName);
                        return;
                    }
                }
                Logger::Log("ConnectDriver | Saved SDK MainCamera not found, waiting for selection: " +
                                savedMainCameraId.toStdString(),
                            LogLevel::WARNING, DeviceType::CAMERA);
            }

            // 5) [修改] 移除SDK MainCamera自动绑定，等待用户手动选择
            // 不再根据历史配置或QHY规则自动选择相机
            {
                systemdevicelist.system_devices[DeviceSlot::MainCamera].isConnect = false;
                systemdevicelist.system_devices[DeviceSlot::MainCamera].isBind = false;
                needAllocation = true;
                Logger::Log("ConnectDriver | SDK MainCamera: waiting for user allocation.", LogLevel::INFO, DeviceType::MAIN);
            }

            if (boundByThisCall)
            {
                // 对齐 INDI：通知前端驱动连接成功
                emit wsThread->sendMessageToClient("AddDeviceType:MainCamera");
                emit wsThread->sendMessageToClient("ConnectDriverSuccess:" + DriverName);
            }
            else if (needAllocation)
            {
                emit wsThread->sendMessageToClient("AddDeviceType:MainCamera");
                emit wsThread->sendMessageToClient("ConnectDriverPendingAllocation:MainCamera");
            }
            return;
        }

        // SDK 导星相机（Guider）：支持“单独连接”入口（复用 ConnectAllDeviceOnce 的 SDK 相机池逻辑）
        if (DriverType.contains("Guider", Qt::CaseInsensitive))
        {
            Logger::Log("ConnectDriver | Use SDK connection for Guider.", LogLevel::INFO, DeviceType::GUIDER);
            bool needAllocation = false;
            bool boundByThisCall = false;

            // 标记导星相机槽位为 SDK 连接，以便后续流程识别
            if (systemdevicelist.system_devices.size() > 1)
            {
                systemdevicelist.system_devices[DeviceSlot::Guider].Description = "Guider";
                systemdevicelist.system_devices[DeviceSlot::Guider].isSDKConnect = true;
                systemdevicelist.system_devices[DeviceSlot::Guider].DriverFrom = "SDK";
            }

            // 1) 初始化 SDK 资源
            SdkCommand initCmd;
            initCmd.type = SdkCommandType::Custom;
            initCmd.name = "InitSdkResource";
            initCmd.payload = std::any();

            QString driverName = getSDKDriverName("Guider");
            if (driverName.isEmpty())
            {
                Logger::Log("ConnectDriver | Cannot get SDK driver name for Guider",
                            LogLevel::ERROR, DeviceType::GUIDER);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:Guider:Cannot get SDK driver");
                return;
            }

            SdkResult initRes = SdkManager::instance().call(driverName.toStdString(), nullptr, initCmd);
            if (!initRes.success)
            {
                Logger::Log("ConnectDriver | InitSdkResource(Guider) failed: " + initRes.message,
                            LogLevel::ERROR, DeviceType::GUIDER);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:Guider:SDK init failed");
                return;
            }

            // 2) 扫描相机

            driverName = getSDKDriverName("Guider");
            if (driverName.isEmpty())
            {
                Logger::Log("ConnectDriver | Cannot get SDK driver name for Guider",
                            LogLevel::ERROR, DeviceType::GUIDER);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:Guider:Cannot get SDK driver");
                return;
            }

            SdkResult scanRes = sdkScanQhyCameras(driverName);
            if (!scanRes.success || !scanRes.payload.has_value())
            {
                Logger::Log("ConnectDriver | ScanCameras(Guider) failed: " + scanRes.message,
                            LogLevel::ERROR, DeviceType::GUIDER);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:Guider:SDK scan failed");
                return;
            }

            const int cameraCount = std::any_cast<int>(scanRes.payload);
            if (cameraCount < 1)
            {
                Logger::Log("ConnectDriver | No SDK camera found for Guider.", LogLevel::ERROR, DeviceType::GUIDER);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:Guider:No camera found");
                return;
            }

            // 3) 清理旧池（若存在），避免重复句柄/冲突
            if (!g_sdkQhyCamHandles.isEmpty())
            {
                Logger::Log("ConnectDriver | SDK camera pool already exists, cleaning up previous handles ...", LogLevel::WARNING, DeviceType::GUIDER);

                // 关键修复：避免“重连导星”时清理相机池导致主相机掉线（同一相机池共享）
                const bool poolLooksValid =
                    (g_sdkQhyCamHandles.size() == g_sdkQhyCamIds.size());
                bool poolHasCorruption = false;
                if (poolLooksValid)
                {
                    // 允许“已释放槽位”：handle==nullptr 时 id 允许为空
                    for (int i = 0; i < g_sdkQhyCamHandles.size(); ++i)
                    {
                        if (g_sdkQhyCamHandles[i] != nullptr && g_sdkQhyCamIds[i].isEmpty())
                        {
                            poolHasCorruption = true;
                            break;
                        }
                    }
                }
                const bool poolInUseByOtherRole =
                    (sdkMainCameraHandle != nullptr) || (g_sdkMainCameraPoolIndex >= 0) ||
                    (sdkPoleScopeHandle != nullptr);

                if (poolInUseByOtherRole)
                {
                    if (!poolLooksValid || poolHasCorruption)
                    {
                        Logger::Log("ConnectDriver | SDK camera pool exists but is inconsistent while other role is using it. "
                                    "Skip cleanup to avoid disconnecting other devices; request user allocation/restart.",
                                    LogLevel::ERROR, DeviceType::GUIDER);
                        emit wsThread->sendMessageToClient("ConnectDriverFailed:Guider:SDK pool inconsistent while other device uses it");
                        return;
                    }

                    Logger::Log("ConnectDriver | SDK camera pool is in use by other role(s) (e.g. MainCamera). "
                                "Skip CameraPool cleanup and reuse existing pool for Guider binding.",
                                LogLevel::WARNING, DeviceType::GUIDER);

                    // 先“补齐”池：在不清理全池的前提下，对 scan 出来的相机里未打开的那些做 open，
                    // 这样 Guider 被断开（close）后也能重联回来，而不会影响正在工作的 MainCamera。
                    int openedNew = 0;
                    // M2：池复用“补齐”——只登记扫描到但尚未在池中的相机（幂等、不 open）。
                    registerSdkCameraPool(driverName);
                    if (openedNew > 0)
                    {
                        Logger::Log("ConnectDriver | Reused pool and opened " + std::to_string(openedNew) +
                                    " additional camera(s) for Guider reconnect.",
                                    LogLevel::INFO, DeviceType::GUIDER);
                    }

                    // [方案B修复] 池复用重连：Guider 若保留了上次绑定的 cameraId 且它已在复用池中，
                    // 直接自动回绑该相机（不清池、不影响 MainCamera），恢复“断开后一键重连”。
                    // 仅当找不到匹配（相机全新/有歧义）时才进入下面的“等待用户手动分配”。
                    {
                        const QString savedGuiderId =
                            (systemdevicelist.system_devices.size() > 1)
                                ? systemdevicelist.system_devices[DeviceSlot::Guider].DeviceIndiName.trimmed()
                                : QString();
                        if (!savedGuiderId.isEmpty())
                        {
                            for (int i = 0; i < g_sdkQhyCamIds.size(); ++i)
                            {
                                if (i == g_sdkMainCameraPoolIndex)
                                    continue;
                                if (g_sdkQhyCamIds[i] == savedGuiderId && sdkPoolIndexValid(i))
                                {
                                    Logger::Log("ConnectDriver | Reconnect saved SDK Guider (pool reuse): " +
                                                    savedGuiderId.toStdString(),
                                                LogLevel::INFO, DeviceType::GUIDER);
                                    BindingDevice("Guider", sdkUiIndexFromPoolIndex(i));
                                    emit wsThread->sendMessageToClient("ConnectDriverSuccess:" + DriverName);
                                    return;
                                }
                            }
                            Logger::Log("ConnectDriver | Saved SDK Guider not found in reused pool, waiting for selection: " +
                                            savedGuiderId.toStdString(),
                                        LogLevel::WARNING, DeviceType::GUIDER);
                        }
                    }

                    // [修改] 移除poolInUseByOtherRole场景下Guider自动绑定
                    // 即使复用已有池，也等待用户手动选择
                    {
                        if (systemdevicelist.system_devices.size() > 1)
                        {
                            systemdevicelist.system_devices[DeviceSlot::Guider].isConnect = false;
                            systemdevicelist.system_devices[DeviceSlot::Guider].isBind = false;
                        }
                        needAllocation = true;
                        Logger::Log("ConnectDriver | SDK Guider (pool reuse): waiting for user allocation.",
                                    LogLevel::INFO, DeviceType::GUIDER);
                    }

                    emit wsThread->sendMessageToClient("AddDeviceType:Guider");
                    if (needAllocation)
                    {
                        emit wsThread->sendMessageToClient("ConnectDriverPendingAllocation:Guider");
                    }
                    else
                    {
                        emit wsThread->sendMessageToClient("ConnectDriverSuccess:" + DriverName);
                    }
                    return;
                }

                cleanupQhySdkPoolAndResource("Reconnect(Guider): cleaning up old pool before new scan", "CameraPool");

                // 等待清理完成（检查所有句柄是否都为 nullptr，最多等待2秒）
                int waitCount = 0;
                const int maxWaitCount = 20; // 20 * 100ms = 2秒
                bool allHandlesNull = false;

                while (waitCount < maxWaitCount)
                {
                    allHandlesNull = true;
                    for (int i = 0; i < g_sdkQhyCamHandles.size(); ++i)
                    {
                        if (g_sdkQhyCamHandles[i] != nullptr)
                        {
                            allHandlesNull = false;
                            break;
                        }
                    }

                    if (allHandlesNull)
                        break;

                    QThread::msleep(100);
                    waitCount++;
                }

                // 强制清空池引用
                g_sdkQhyCamHandles.clear();
                g_sdkQhyCamIds.clear();
                g_sdkMainCameraPoolIndex = -1;
                g_sdkGuiderPoolIndex = -1;
                g_sdkPoleCameraPoolIndex = -1;
                sdkMainCameraHandle = nullptr;
                sdkGuiderHandle = nullptr;
                sdkPoleScopeHandle = nullptr;
                sdkMainCameraId.clear();

                // 额外等待，确保 SDK 线程中的 close 和 ReleaseSdkResource 完成
                QThread::msleep(800);
            }

            // 4) 打开全部相机（多相机场景：触发设备分配逻辑；单相机：自动绑定）
            // ── M2：扫描登记（不 open）+ 上报候选 ─────────────────────────────
            // ScanQHYCCD/GetQHYCCDId 只枚举、不占用设备；真正的排他是 OpenQHYCCD。
            // 旧流程在此 open 全部相机 -> 占住本该留给 INDI 的相机（混用双开冲突的自造
            // 根源），且同段逻辑全仓复制 6 份。现统一走 registerSdkCameraPool（只登记
            // cameraId、句柄留空）+ reportSdkCameraCandidates；真正的 open 推迟到
            // BindingDevice -> ensureSdkCameraOpen（只开被分配的那台）。
            const int poolValid = registerSdkCameraPool(driverName);
            if (poolValid < 1)
            {
                Logger::Log("ConnectDriver | SDK scan/register produced no camera for Guider.",
                            LogLevel::ERROR, DeviceType::GUIDER);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:Guider:No camera found");
                return;
            }
            reportSdkCameraCandidates();

            // 单设备重连使用已提交的设备选择，不要求用户再次分配相机。
            const QString savedGuiderId = systemdevicelist.system_devices[DeviceSlot::Guider].DeviceIndiName.trimmed();
            if (!savedGuiderId.isEmpty())
            {
                for (int i = 0; i < g_sdkQhyCamIds.size(); ++i)
                {
                    if (g_sdkQhyCamIds[i] == savedGuiderId && sdkPoolIndexValid(i))
                    {
                        Logger::Log("ConnectDriver | Reconnect saved SDK Guider: " +
                                        savedGuiderId.toStdString(),
                                    LogLevel::INFO, DeviceType::GUIDER);
                        BindingDevice("Guider", sdkUiIndexFromPoolIndex(i));
                        emit wsThread->sendMessageToClient("ConnectDriverSuccess:" + DriverName);
                        return;
                    }
                }
                Logger::Log("ConnectDriver | Saved SDK Guider not found, waiting for selection: " +
                                savedGuiderId.toStdString(),
                            LogLevel::WARNING, DeviceType::GUIDER);
            }

            // 5) [修改] 移除SDK Guider自动绑定，等待用户手动选择
            // 不再根据历史配置或QHY规则自动选择相机
            {
                if (systemdevicelist.system_devices.size() > 1)
                {
                    systemdevicelist.system_devices[DeviceSlot::Guider].isConnect = false;
                    systemdevicelist.system_devices[DeviceSlot::Guider].isBind = false;
                }
                needAllocation = true;
                Logger::Log("ConnectDriver | SDK Guider: waiting for user allocation.", LogLevel::INFO, DeviceType::GUIDER);
            }

            if (boundByThisCall)
            {
                // 对齐 INDI：通知前端驱动连接成功
                emit wsThread->sendMessageToClient("AddDeviceType:Guider");
                emit wsThread->sendMessageToClient("ConnectDriverSuccess:" + DriverName);
            }
            else if (needAllocation)
            {
                emit wsThread->sendMessageToClient("AddDeviceType:Guider");
                emit wsThread->sendMessageToClient("ConnectDriverPendingAllocation:Guider");
            }
            return;
        }

        // SDK 电子极轴镜（PoleCamera）：支持“单独连接”入口
        if (DriverType.contains("PoleCamera", Qt::CaseInsensitive))
        {
            Logger::Log("ConnectDriver | Use SDK connection for PoleCamera.", LogLevel::INFO, DeviceType::MAIN);
            bool needAllocation = false;
            bool boundByThisCall = false;

            if (systemdevicelist.system_devices.size() > 2)
            {
                systemdevicelist.system_devices[DeviceSlot::PoleCamera].Description = "PoleCamera";
                systemdevicelist.system_devices[DeviceSlot::PoleCamera].DriverIndiName = DriverName;
                systemdevicelist.system_devices[DeviceSlot::PoleCamera].isSDKConnect = true;
                systemdevicelist.system_devices[DeviceSlot::PoleCamera].DriverFrom = "SDK";
            }

            QString driverName = getSDKDriverName("PoleCamera");
            if (driverName.isEmpty())
            {
                Logger::Log("ConnectDriver | Cannot get SDK driver name for PoleCamera",
                            LogLevel::ERROR, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:PoleCamera:Cannot get SDK driver");
                return;
            }

            SdkCommand initCmd;
            initCmd.type = SdkCommandType::Custom;
            initCmd.name = "InitSdkResource";
            initCmd.payload = std::any();
            SdkResult initRes = SdkManager::instance().call(driverName.toStdString(), nullptr, initCmd);
            if (!initRes.success)
            {
                Logger::Log("ConnectDriver | InitSdkResource(PoleCamera) failed: " + initRes.message,
                            LogLevel::ERROR, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:PoleCamera:SDK init failed");
                return;
            }

            SdkResult scanRes = sdkScanQhyCameras(driverName);
            if (!scanRes.success || !scanRes.payload.has_value())
            {
                Logger::Log("ConnectDriver | ScanCameras(PoleCamera) failed: " + scanRes.message,
                            LogLevel::ERROR, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:PoleCamera:SDK scan failed");
                return;
            }

            const int cameraCount = std::any_cast<int>(scanRes.payload);
            if (cameraCount < 1)
            {
                Logger::Log("ConnectDriver | No SDK camera found for PoleCamera.", LogLevel::ERROR, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:PoleCamera:No camera found");
                return;
            }

            // M2：扫描登记（不 open）+ 上报候选；真正的 open 推迟到 BindingDevice。
            registerSdkCameraPool(driverName);
            reportSdkCameraCandidates();

            // SDK PoleCamera 不做任何自动绑定，一律等用户手动选择。
            // 原先此处保留了 isPoleMasterName() 的按名识别；现已收敛进
            // autoDecideDeviceForRole()（当前封堵 -> -1 -> 保持未绑定）。
            // 候选已由上面的 reportSdkCameraCandidates() 上报，用户可手动指派。
            QVector<AutoDecisionCandidate> poleCands;
            for (int i = 0; i < g_sdkQhyCamIds.size(); ++i)
            {
                if (i == g_sdkMainCameraPoolIndex || i == g_sdkGuiderPoolIndex)
                    continue;
                if (sdkPoolIndexValid(i))
                    poleCands.append({i, g_sdkQhyCamIds[i]});
            }
            int pickIndex = autoDecideDeviceForRole("PoleCamera", poleCands);

            if (pickIndex >= 0 && sdkPoolIndexValid(pickIndex))
            {
                g_sdkPoleCameraPoolIndex = pickIndex;
                sdkPoleScopeHandle = g_sdkQhyCamHandles[pickIndex];
                const QString poleId = g_sdkQhyCamIds[pickIndex];
                boundByThisCall = true;

                if (systemdevicelist.system_devices.size() > 2)
                {
                    systemdevicelist.system_devices[DeviceSlot::PoleCamera].isConnect = true;
                    systemdevicelist.system_devices[DeviceSlot::PoleCamera].DeviceIndiName = poleId;
                }

                SdkResult regRes = SdkManager::instance().registerDevice(
                    driverName.toStdString(),
                    "PoleCamera",
                    sdkPoleScopeHandle,
                    "电子极轴镜",
                    std::any(poleId.toStdString()));
                if (!regRes.success)
                {
                    Logger::Log("ConnectDriver | Failed to register PoleCamera to SdkManager: " + regRes.message,
                                LogLevel::WARNING, DeviceType::MAIN);
                }

                AfterDeviceConnect(nullptr);
                Logger::Log("ConnectDriver | SDK PoleCamera auto-bound: " + poleId.toStdString(),
                            LogLevel::INFO, DeviceType::MAIN);
            }
            else
            {
                if (systemdevicelist.system_devices.size() > 2)
                {
                    systemdevicelist.system_devices[DeviceSlot::PoleCamera].isConnect = false;
                    systemdevicelist.system_devices[DeviceSlot::PoleCamera].isBind = false;
                }
                needAllocation = true;
            }

            if (boundByThisCall)
            {
                emit wsThread->sendMessageToClient("AddDeviceType:PoleCamera");
                emit wsThread->sendMessageToClient("ConnectDriverSuccess:" + DriverName);
            }
            else if (needAllocation)
            {
                emit wsThread->sendMessageToClient("AddDeviceType:PoleCamera");
                emit wsThread->sendMessageToClient("ConnectDriverPendingAllocation:PoleCamera");
            }
            return;
        }

        // SDK 电调（Focuser）：支持“单独连接”入口（与 ConnectAllDeviceOnce 的 SDK 电调连接行为保持一致）
        if (DriverType.contains("Focuser", Qt::CaseInsensitive))
        {
            Logger::Log("ConnectDriver | Use SDK connection for Focuser.", LogLevel::INFO, DeviceType::FOCUSER);

            // 保护性检查：systemdevicelist 是否有电调槽位（约定 index=22）
            if (systemdevicelist.system_devices.size() <= 22)
            {
                Logger::Log("ConnectDriver | SDK focuser slot(22) missing in systemdevicelist.", LogLevel::ERROR, DeviceType::FOCUSER);
                emit wsThread->sendMessageToClient("ConnectFailed:SDK focuser slot missing");
                emit wsThread->sendMessageToClient("ConnectDriverFailed:SDK focuser slot missing");
                return;
            }

            // 标记电调槽位为 SDK 连接，并记录 driverName（用于前端 SelectedDriverList / 后续重连）
            systemdevicelist.system_devices[DeviceSlot::Focuser].Description = "Focuser";
            systemdevicelist.system_devices[DeviceSlot::Focuser].DriverIndiName = DriverName;
            systemdevicelist.system_devices[DeviceSlot::Focuser].isSDKConnect = true;
            // 命名约定：DriverFrom 仅用于“是否支持 SDK”判断（contains("SDK")），这里填 SDK 即可
            if (!systemdevicelist.system_devices[DeviceSlot::Focuser].DriverFrom.contains("SDK", Qt::CaseInsensitive))
                systemdevicelist.system_devices[DeviceSlot::Focuser].DriverFrom = "QHYFOCUSERSDK";

            // 关闭旧句柄（避免重复占用串口）
            if (sdkFocuserHandle != nullptr)
            {
                SdkManager::instance().closeByHandle(sdkFocuserHandle);
                sdkFocuserHandle = nullptr;
                sdkFocuserPort.clear();
            }

            // 选择串口：手动覆盖 > 上次保存 > 自动识别
            QString portToUse;
            QString portSource;
            if (!focuserSerialPortOverride.isEmpty())
            {
                portToUse = focuserSerialPortOverride;
                portSource = "override";
            }
            else if (!systemdevicelist.system_devices[DeviceSlot::Focuser].DeviceIndiName.isEmpty() &&
                     systemdevicelist.system_devices[DeviceSlot::Focuser].DeviceIndiName.startsWith("/dev/"))
            {
                portToUse = systemdevicelist.system_devices[DeviceSlot::Focuser].DeviceIndiName;
                portSource = "saved";
            }
            else
            {
                portToUse = detector.getFocuserPort();
                portSource = "auto";
            }

            if (!portToUse.isEmpty() && !detector.isPortPresent(portToUse))
            {
                Logger::Log("ConnectDriver | SDK focuser " + portSource.toStdString() +
                                " port is not present: " + portToUse.toStdString() +
                                ", rescan serial ports.",
                            LogLevel::WARNING, DeviceType::FOCUSER);
                const DevicePorts ports = detector.rescan();
                portToUse = ports.focuserPort;
                portSource = "auto-rescan";
                if (!portToUse.isEmpty())
                    focuserSerialPortOverride = portToUse;
            }

            if (portToUse.isEmpty())
            {
                Logger::Log("ConnectDriver | SDK focuser port not found (override/saved/auto all empty).",
                            LogLevel::ERROR, DeviceType::FOCUSER);
                emit wsThread->sendMessageToClient("ConnectFailed:SDK focuser port not found");
                emit wsThread->sendMessageToClient("ConnectDriverFailed:SDK focuser port not found");
                return;
            }

            // 打开串口
            SdkFocuserOpenParam p;
            p.port = portToUse.toStdString();
            p.baudRate = systemdevicelist.system_devices[DeviceSlot::Focuser].BaudRate;
            p.timeoutMs = 3000;

            // 使用getSDKDriverName动态获取驱动名称
            QString driverName = getSDKDriverName("Focuser");
            if (driverName.isEmpty()) {
                Logger::Log("ConnectDriver | Cannot get SDK driver name for Focuser",
                            LogLevel::ERROR, DeviceType::FOCUSER);
                emit wsThread->sendMessageToClient("ConnectDriverFailed:Focuser:Cannot get SDK driver name");
                return;
            }
            SdkResult openRes = SdkManager::instance().open(driverName.toStdString(), p);
            if (!openRes.success || !openRes.payload.has_value())
            {
                Logger::Log("ConnectDriver | SDK focuser open failed: " + openRes.message,
                            LogLevel::ERROR, DeviceType::FOCUSER);
                emit wsThread->sendMessageToClient("ConnectFailed:SDK focuser open failed");
                emit wsThread->sendMessageToClient("ConnectDriverFailed:SDK focuser open failed");
                return;
            }

            sdkFocuserHandle = std::any_cast<SdkDeviceHandle>(openRes.payload);
            sdkFocuserPort = portToUse;

            // 注册设备到 SdkManager，这样 callByHandle 才能找到对应的驱动
            // 注意：此时串口尚未打开（延迟打开），只是保存了参数，不会占用串口
            SdkResult regRes = SdkManager::instance().registerDevice(
                driverName.toStdString(),
                "Focuser",
                sdkFocuserHandle,
                "QHY Focuser"
            );
            if (!regRes.success)
            {
                Logger::Log("ConnectDriver | SDK focuser register failed: " + regRes.message,
                            LogLevel::ERROR, DeviceType::FOCUSER);
                SdkManager::instance().closeByHandle(sdkFocuserHandle);
                sdkFocuserHandle = nullptr;
                sdkFocuserPort.clear();
                emit wsThread->sendMessageToClient("ConnectFailed:SDK focuser register failed");
                emit wsThread->sendMessageToClient("ConnectDriverFailed:SDK focuser register failed");
                return;
            }

            // 握手必须在 sdkFocuserExec 线程执行，确保 QSerialPort 在该线程创建/使用
            SdkResult hsRes;
            bool hsOk = false;
            const SdkDeviceHandle handleSnap = sdkFocuserHandle;
            const int handshakeWaitMs = p.timeoutMs * 6 + 1000; // 默认约 19s

            auto doHandshakeOnce = [&]() -> SdkResult {
                if (!sdkFocuserExec || !sdkFocuserExec->isRunning())
                {
                    SdkResult r;
                    r.success = false;
                    r.message = "sdkFocuserExec not running";
                    return r;
                }

                auto prom = std::make_shared<std::promise<SdkResult>>();
                auto fut = prom->get_future();
                sdkFocuserExec->post([prom, handleSnap]() {
                    SdkCommand hs;
                    hs.type = SdkCommandType::Custom;
                    hs.name = "Handshake";
                    hs.payload = std::any();
                    // 直接通过设备句柄调用，无需指定驱动名称
                    SdkResult r = SdkManager::instance().callByHandle(handleSnap, hs);
                    prom->set_value(r);
                });

                const auto st = fut.wait_for(std::chrono::milliseconds(handshakeWaitMs));
                if (st == std::future_status::ready)
                    return fut.get();

                SdkResult r;
                r.success = false;
                r.message =
                    "Handshake timeout waiting for sdkFocuserExec"
                    " (worker did not finish in " + std::to_string(handshakeWaitMs) +
                    "ms; device may not respond / wrong port/baud / serial busy)";
                return r;
            };

            const int maxHandshakeAttempts = 3;
            for (int attempt = 1; attempt <= maxHandshakeAttempts; ++attempt)
            {
                if (attempt > 1)
                {
                    const int delayMs = 600 * attempt;
                    Logger::Log(std::string("ConnectDriver | SDK focuser handshake retry ") + std::to_string(attempt) +
                                    "/" + std::to_string(maxHandshakeAttempts) +
                                    " after " + std::to_string(delayMs) + "ms (previous error: " + hsRes.message + ")",
                                LogLevel::WARNING, DeviceType::FOCUSER);
                    QThread::msleep(delayMs);
                }
                hsRes = doHandshakeOnce();
                hsOk = hsRes.success;
                if (hsOk)
                    break;
            }

            if (!hsOk)
            {
                Logger::Log("ConnectDriver | SDK focuser handshake failed: " + hsRes.message,
                            LogLevel::ERROR, DeviceType::FOCUSER);
                SdkManager::instance().closeByHandle(sdkFocuserHandle);
                sdkFocuserHandle = nullptr;
                sdkFocuserPort.clear();
                emit wsThread->sendMessageToClient("ConnectFailed:SDK focuser handshake failed");
                emit wsThread->sendMessageToClient("ConnectDriverFailed:SDK focuser handshake failed");
                return;
            }

            // 连接成功：更新 systemdevicelist（注意：isBind 将由 AfterDeviceConnect 在初始化完成后设置）
            systemdevicelist.system_devices[DeviceSlot::Focuser].isConnect = true;
            systemdevicelist.system_devices[DeviceSlot::Focuser].DeviceIndiName = portToUse;

            // 通知前端设备类型
            emit wsThread->sendMessageToClient("AddDeviceType:Focuser");

            // 调用统一的连接后初始化流程（参数下发、版本上报、位置推送、ConnectSuccess、isBind 设置等）
            AfterDeviceConnect(nullptr);
            
            // 初始化完成后持久化设备列表（此时 isBind 已被 AfterDeviceConnect 设置）
            Tools::saveSystemDeviceList(systemdevicelist);

            // 兼容"单独连接"的 loading/状态：同时发 ConnectDriverSuccess
            emit wsThread->sendMessageToClient("ConnectDriverSuccess:" + DriverName);
            Logger::Log("ConnectDriver | SDK focuser connected: " + portToUse.toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
            return;
        }

        // 其它 SDK 设备类型可在此按需扩展
        Logger::Log("ConnectDriver | SDK connection requested but unsupported DriverType=" + DriverType.toStdString(),
                    LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectFailed:SDK type not supported for " + DriverType);
        emit wsThread->sendMessageToClient("ConnectDriverFailed:SDK type not supported for " + DriverType);
        return;
    }

    // ===== 关键修复：INDI 连接前释放 SDK 电调句柄（避免串口被本进程占用）=====
    // 场景：用户从 SDK 切到 INDI，但未走“解绑”流程；或历史残留 sdkFocuserHandle 未被释放。
    // 若不释放，会导致 indi_qhy_focuser 打开 /dev/ttyACM* 时提示 "Port ... already used".
    if (DriverType == "Focuser" && sdkFocuserHandle != nullptr)
    {
        Logger::Log("ConnectDriver | INDI connect for Focuser requested, closing existing SDK focuser handle to release serial port.",
                    LogLevel::INFO, DeviceType::FOCUSER);
        // 直接通过设备句柄关闭，无需指定驱动名称
        SdkManager::instance().closeByHandle(sdkFocuserHandle);
        sdkFocuserHandle = nullptr;
        sdkFocuserPort.clear();
    }

    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        if (systemdevicelist.system_devices[i].Description == DriverType)
        {
            if (systemdevicelist.system_devices[i].isConnect)
            {
                Logger::Log("ConnectDriver | Driver(" + DriverName.toStdString() + ") is already connected", LogLevel::INFO, DeviceType::MAIN);
            }
            else
            {
                if (ConnectDriverList.contains(DriverName))
                {
                    Logger::Log("ConnectDriver | Driver(" + DriverName.toStdString() + ") is already in ConnectDriverList", LogLevel::INFO, DeviceType::MAIN);
                }
                else
                {
                    Logger::Log("ConnectDriver | Driver(" + DriverName.toStdString() + ") is not connected, start to connect", LogLevel::INFO, DeviceType::MAIN);
                    QString startError;
                    if (!Tools::startIndiDriver(DriverName, &startError))
                    {
                        Logger::Log("ConnectDriver | Failed to start INDI driver. Driver=" + DriverName.toStdString() +
                                        ", Type=" + DriverType.toStdString() + ", Reason=" + startError.toStdString(),
                                    LogLevel::ERROR, DeviceType::MAIN);
                        emit wsThread->sendMessageToClient("ConnectDriverFailed:" + DriverType + ":Start driver failed: " + startError);
                        return;
                    }
                    ConnectDriverList.push_back(DriverName);
                }
            }
            driverCode = i;
            isDriverConnected = true;
            break;
        }
    }

    if (!isDriverConnected)
    {
        Logger::Log("ConnectDriver | " + DriverType.toStdString() + " Driver(" + DriverName.toStdString() + ") is not selected, start to connect", LogLevel::INFO, DeviceType::MAIN);
        if (DriverType == "Mount")
        {
            driverCode = 0;
            systemdevicelist.system_devices[DeviceSlot::Mount].Description = "Mount";
            systemdevicelist.system_devices[DeviceSlot::Mount].DriverIndiName = DriverName;
        }
        else if (DriverType == "Guider")
        {
            driverCode = 1;
            systemdevicelist.system_devices[DeviceSlot::Guider].Description = "Guider";
            systemdevicelist.system_devices[DeviceSlot::Guider].DriverIndiName = DriverName;
        }
        else if (DriverType == "PoleCamera")
        {
            driverCode = 2;
            systemdevicelist.system_devices[DeviceSlot::PoleCamera].Description = "PoleCamera";
            systemdevicelist.system_devices[DeviceSlot::PoleCamera].DriverIndiName = DriverName;
        }
        else if (DriverType == "MainCamera")
        {
            driverCode = 20;
            systemdevicelist.system_devices[DeviceSlot::MainCamera].Description = "MainCamera";
            systemdevicelist.system_devices[DeviceSlot::MainCamera].DriverIndiName = DriverName;
        }
        else if (DriverType == "CFW")
        {
            driverCode = 21;
            systemdevicelist.system_devices[DeviceSlot::CFW].Description = "CFW";
            systemdevicelist.system_devices[DeviceSlot::CFW].DriverIndiName = DriverName;
        }
        else if (DriverType == "Focuser")
        {
            driverCode = 22;
            systemdevicelist.system_devices[DeviceSlot::Focuser].Description = "Focuser";
            systemdevicelist.system_devices[DeviceSlot::Focuser].DriverIndiName = DriverName;
        }
        else
        {
            Logger::Log("ConnectDriver | DriverType(" + DriverType.toStdString() + ") is not supported.", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("ConnectFailed:DriverType is not supported.");
            emit wsThread->sendMessageToClient("ConnectDriverFailed:DriverType is not supported.");
            Tools::stopIndiDriver(DriverName);
            ConnectDriverList.removeAll(DriverName);
            return;
        }
        QString startError;
        if (!Tools::startIndiDriver(DriverName, &startError))
        {
            Logger::Log("ConnectDriver | Failed to start INDI driver for unselected device. Driver=" + DriverName.toStdString() +
                            ", Type=" + DriverType.toStdString() + ", Reason=" + startError.toStdString(),
                        LogLevel::ERROR, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("ConnectDriverFailed:" + DriverType + ":Start driver failed: " + startError);
            return;
        }
        ConnectDriverList.push_back(DriverName);
    }
    sleep(1);
    if (indi_Client->isServerConnected() == false)
    {
        Logger::Log("ConnectDriver | indi Client is not connected, try to connect", LogLevel::INFO, DeviceType::MAIN);
        connectIndiServer(indi_Client);
        if (indi_Client->isServerConnected() == false)
        {
            Logger::Log("ConnectDriver | Connect indi server failed", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("ConnectFailed:Connect indi server failed.");
            emit wsThread->sendMessageToClient("ConnectDriverFailed:Connect indi server failed.");
            Tools::stopIndiDriver(DriverName);
            ConnectDriverList.removeAll(DriverName);
            return;
        }
    }
    sleep(1);
    auto hasRequestedDriver = [&]() -> bool {
        for (int i = 0; i < indi_Client->GetDeviceCount(); i++)
        {
            INDI::BaseDevice *device = indi_Client->GetDeviceFromList(i);
            if (device != nullptr &&
                indiDriverNamesEquivalent(QString::fromUtf8(device->getDriverExec()), DriverName))
                return true;
        }
        return false;
    };

    int time = 0;
    while (time < 10)
    {
        if (hasRequestedDriver())
        {
            Logger::Log("ConnectDriver | Requested driver published device. Driver=" + DriverName.toStdString() +
                            ", DeviceCount=" + std::to_string(indi_Client->GetDeviceCount()),
                        LogLevel::INFO, DeviceType::MAIN);
            break;
        }
        Logger::Log("ConnectDriver | Waiting for requested driver to publish device. Driver=" + DriverName.toStdString() +
                        ", Type=" + DriverType.toStdString() +
                        ", DeviceCount=" + std::to_string(indi_Client->GetDeviceCount()),
                    LogLevel::INFO, DeviceType::MAIN);
        QThread::msleep(1000);
        time++;
    }
    sleep(1);
    if (!hasRequestedDriver())
    {
        Logger::Log("ConnectDriver | Requested driver did not publish any device. Driver=" + DriverName.toStdString() +
                        ", Type=" + DriverType.toStdString() +
                        ", TotalDeviceCount=" + std::to_string(indi_Client->GetDeviceCount()),
                    LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectDriverFailed:" + DriverType + ":Driver did not publish device: " + DriverName);
        Tools::stopIndiDriver(DriverName);
        ConnectDriverList.removeAll(DriverName);
        return;
    }
    // 记录连接的设备的id列表
    std::vector<int> connectedDeviceIdList;
    for (int i = 0; i < indi_Client->GetDeviceCount(); i++)
    {

        if (indiDriverNamesEquivalent(QString::fromUtf8(indi_Client->GetDeviceFromList(i)->getDriverExec()),
                                      DriverName))
        {
            if (indi_Client->GetDeviceFromList(i)->isConnected())
            {
                Logger::Log("ConnectDriver | Device(" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + ") is connected", LogLevel::INFO, DeviceType::MAIN);
                // “这台设备是否已被某个角色占用”必须以 isBind 为准。
                // DeviceIndiName 同时承担『记住上次选的设备』的持久化职责：设备未连接、未绑定时
                // 槽位里也会留着它的名字（重启后据此显示型号）。若只比名字，就会把
                // “槽位记得它” 误判成 “它已被占用”。
                // 症状：第一台相机绑定后再连第二台 —— 第二台因 Guider 槽位persist 着它的名字
                // 而被判为 is Used -> 无可用设备 -> ConnectDriverFailed，且驱动被整个移除。
                // （以前自动历史回绑会把该槽位真绑上，掩盖了这个混淆；自动分配移除后暴露。）
                bool isDeviceBind = false;
                for (int j = 0; j < systemdevicelist.system_devices.size(); j++)
                {
                    if (systemdevicelist.system_devices[j].DeviceIndiName == indi_Client->GetDeviceNameFromList(i).c_str()
                        && systemdevicelist.system_devices[j].isBind)
                    {
                        isDeviceBind = true;
                    }
                }
                if (!isDeviceBind)
                {
                    const bool isCameraRole =
                        (DriverType == "MainCamera" || DriverType == "Guider" || DriverType == "PoleCamera");
                    const bool isQhyDriver = DriverName.contains("qhy", Qt::CaseInsensitive);
                    const bool isCcdDevice =
                        (indi_Client->GetDeviceFromList(i)->getDriverInterface() & INDI::BaseDevice::CCD_INTERFACE);

                    // 对于同一 QHY CCD 驱动下的多相机场景，已连接但尚未绑定角色的设备先保留为候选。
                    // 否则这里先断开再重连，容易让后续的 Main/Guider 自动分配丢失本来已经在线的另一台相机。
                    if (isCameraRole && isQhyDriver && isCcdDevice)
                    {
                        Logger::Log("ConnectDriver | Device(" +
                                        std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) +
                                        ") is connected but not bound, keep it as QHY CCD candidate for role allocation",
                                    LogLevel::INFO, DeviceType::MAIN);
                        connectedDeviceIdList.push_back(i);
                        continue;
                    }

                    Logger::Log("ConnectDriver | Device(" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + ") is not bind, start to disconnect", LogLevel::INFO, DeviceType::MAIN);
                    indi_Client->disconnectDevice(indi_Client->GetDeviceFromList(i)->getDeviceName());
                    time = 0;
                    while (indi_Client->GetDeviceFromList(i)->isConnected() && time < 30)
                    {
                        Logger::Log("ConnectDriver | Wait for disconnect" + std::string(indi_Client->GetDeviceNameFromList(i).c_str()), LogLevel::INFO, DeviceType::MAIN);
                        QThread::msleep(1000);
                        time++;
                    }
                    if (!indi_Client->GetDeviceFromList(i)->isConnected())
                    {
                        Logger::Log("ConnectDriver | Device(" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + ") is disconnected, start to connect", LogLevel::INFO, DeviceType::MAIN);
                    }
                    else
                    {
                        Logger::Log("ConnectDriver | Device(" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + ") is not disconnected", LogLevel::WARNING, DeviceType::MAIN);
                    }

                    time = 0;
                    // 根据 DriverName 和 DriverType 在 systemdevicelist 中找到对应的设备下标，用于获取正确的波特率
                    int sysIndex = -1;
                    for (int idx = 0; idx < systemdevicelist.system_devices.size(); idx++)
                    {
                        if (systemdevicelist.system_devices[idx].DriverIndiName == DriverName &&
                            systemdevicelist.system_devices[idx].Description == DriverType)
                        {
                            sysIndex = idx;
                            break;
                        }
                    }

                    // 在正式连接前，仅在用户手动选择串口时应用覆盖设置；默认模式不改端口
                    if (DriverType == "Focuser" && !focuserSerialPortOverride.isEmpty())
                    {
                        indi_Client->setDevicePort(indi_Client->GetDeviceFromList(i), focuserSerialPortOverride);
                        Logger::Log("ConnectDriver | Focuser initial Port set to: " + focuserSerialPortOverride.toStdString(),
                                    LogLevel::INFO, DeviceType::MAIN);
                    }
                    else if (DriverType == "Mount" && !mountSerialPortOverride.isEmpty())
                    {
                        indi_Client->setDevicePort(indi_Client->GetDeviceFromList(i), mountSerialPortOverride);
                        Logger::Log("ConnectDriver | Mount initial Port set to: " + mountSerialPortOverride.toStdString(),
                                    LogLevel::INFO, DeviceType::MAIN);
                    }

                    int baudRateToUse = 9600;
                    if (sysIndex >= 0)
                    {
                        baudRateToUse = systemdevicelist.system_devices[sysIndex].BaudRate;
                    }
                    else
                    {
                        Logger::Log("ConnectDriver | Unable to find SystemDevice for Driver(" + DriverName.toStdString() + "), use default baud 9600",
                                    LogLevel::WARNING, DeviceType::MAIN);
                    }
                    indi_Client->setBaudRate(indi_Client->GetDeviceFromList(i), baudRateToUse);
                    indi_Client->connectDevice(indi_Client->GetDeviceFromList(i)->getDeviceName());
                    while (!indi_Client->GetDeviceFromList(i)->isConnected() && time < 15)
                    {
                        Logger::Log("ConnectDriver | Wait for connect" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + ",state:" + std::to_string(indi_Client->GetDeviceFromList(i)->isConnected()), LogLevel::INFO, DeviceType::MAIN);
                        QThread::msleep(1000);
                        time++;
                    }
                    if (!indi_Client->GetDeviceFromList(i)->isConnected())
                    {
                        Logger::Log("ConnectDriver | Device (" + std::string(indi_Client->GetDeviceNameFromList(i).c_str()) + ") is not exist", LogLevel::WARNING, DeviceType::MAIN);
                        emit wsThread->sendMessageToClient("deleteDeviceAllocationList:" + QString::fromUtf8(indi_Client->GetDeviceNameFromList(i).c_str()));
                        indi_Client->RemoveDevice(indi_Client->GetDeviceNameFromList(i).c_str());
                        for (int j = 0; j < connectedDeviceIdList.size(); j++)
                        {
                            if (connectedDeviceIdList[j] == i)
                            {
                                connectedDeviceIdList.erase(connectedDeviceIdList.begin() + j);
                                break;
                            }
                        }
                    }
                    else
                    {
                        connectedDeviceIdList.push_back(i);
                    }
                }
                else
                {
                    Logger::Log("ConnectDriver | Device(" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + ") is bind, is Used", LogLevel::INFO, DeviceType::MAIN);
                }
            }
            else
            {
                Logger::Log("ConnectDriver | Device(" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + ") is connecting...", LogLevel::INFO, DeviceType::MAIN);

                // 根据 DriverName 和 DriverType 在 systemdevicelist 中找到对应的设备下标，用于获取正确的波特率
                int sysIndex = -1;
                for (int idx = 0; idx < systemdevicelist.system_devices.size(); idx++)
                {
                    if (systemdevicelist.system_devices[idx].DriverIndiName == DriverName &&
                        systemdevicelist.system_devices[idx].Description == DriverType)
                    {
                        sysIndex = idx;
                        break;
                    }
                }
                // 在正式连接前，仅在用户手动选择串口时应用覆盖设置；默认模式不改端口
                if (DriverType == "Focuser" && !focuserSerialPortOverride.isEmpty())
                {
                    indi_Client->setDevicePort(indi_Client->GetDeviceFromList(i), focuserSerialPortOverride);
                    Logger::Log("ConnectDriver | Focuser initial Port set to: " + focuserSerialPortOverride.toStdString(),
                                LogLevel::INFO, DeviceType::MAIN);
                }
                else if (DriverType == "Mount" && !mountSerialPortOverride.isEmpty())
                {
                    indi_Client->setDevicePort(indi_Client->GetDeviceFromList(i), mountSerialPortOverride);
                    Logger::Log("ConnectDriver | Mount initial Port set to: " + mountSerialPortOverride.toStdString(),
                                LogLevel::INFO, DeviceType::MAIN);
                }

                int baudRateToUse = 9600;
                if (sysIndex >= 0)
                {
                    baudRateToUse = systemdevicelist.system_devices[sysIndex].BaudRate;
                }
                else
                {
                    Logger::Log("ConnectDriver | Unable to find SystemDevice for Driver(" + DriverName.toStdString() + "), use default baud 9600",
                                LogLevel::WARNING, DeviceType::MAIN);
                }
                indi_Client->setBaudRate(indi_Client->GetDeviceFromList(i), baudRateToUse);
                indi_Client->connectDevice(indi_Client->GetDeviceNameFromList(i).c_str());
                int waitTime = 0;
                bool connectState = false;
                while (waitTime < 15)
                {
                    Logger::Log("ConnectDriver | Wait for Connect " + std::string(indi_Client->GetDeviceNameFromList(i).c_str()), LogLevel::INFO, DeviceType::MAIN);
                    QThread::msleep(1000); // 等待1秒
                    waitTime++;
                    if (indi_Client->GetDeviceFromList(i)->isConnected())
                    {
                        connectState = true;
                        break;
                    }
                }
                if (connectState)
                {
                    connectedDeviceIdList.push_back(i);
                }else{
                    Logger::Log("ConnectDriver | Device (" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + ") is not connected,try to update port", LogLevel::WARNING, DeviceType::MAIN);
                    
                    // 修复：连接失败后，先断开设备以释放可能占用的串口，避免端口被占用导致重试失败
                    // 即使连接失败，INDI 驱动可能已经部分打开了串口（tty_connect），需要显式断开以确保端口完全释放
                    indi_Client->disconnectDevice(indi_Client->GetDeviceFromList(i)->getDeviceName());
                    Logger::Log("ConnectDriver | Disconnected device to release port before retry: " + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()), 
                                LogLevel::INFO, DeviceType::MAIN);
                    QThread::msleep(200); // 短暂等待，确保端口完全释放
                    
                    // 特殊处理电调和赤道仪的连接
                    if (DriverType == "Focuser")
                    {
                        // 电调当前的串口
                        QString DevicePort;
                        indi_Client->getDevicePort(indi_Client->GetDeviceFromList(i), DevicePort);
                        if (detector.detectDeviceTypeForPort(DevicePort) != "Focuser")
                        {
                            // 识别到当前设备的串口不是电调的串口,需更新
                            // 正确的串口是detector.getFocuserPort()
                            QString realFocuserPort = detector.getFocuserPort();
                            if (!realFocuserPort.isEmpty())
                            {
                                indi_Client->setDevicePort(indi_Client->GetDeviceFromList(i), realFocuserPort);
                                // 将自动匹配到的端口同步到覆盖值，保证后续连接与前端显示一致
                                focuserSerialPortOverride = realFocuserPort;
                                Logger::Log("ConnectDriver | Focuser Device (" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + ") Port is updated to: " + realFocuserPort.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                            }
                            else
                            {
                                Logger::Log("No matched Focuser port found by detector.", LogLevel::WARNING, DeviceType::MAIN);
                                continue;
                            }
                        }else{
                            Logger::Log("ConnectDriver | Focuser Device (" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + ") Port is correct.", LogLevel::INFO, DeviceType::MAIN);
                        }
                    }
                    else if (DriverType == "Mount")
                    {
                        // 赤道仪当前的串口
                        QString DevicePort;
                        indi_Client->getDevicePort(indi_Client->GetDeviceFromList(i), DevicePort);
                        if (detector.detectDeviceTypeForPort(DevicePort) != "Mount")
                        {
                            // 识别到当前设备的串口不是赤道仪的串口,需更新
                            // 正确的串口是detector.getMountPort()
                            QString realMountPort = detector.getMountPort();
                            if (!realMountPort.isEmpty())
                            {
                                indi_Client->setDevicePort(indi_Client->GetDeviceFromList(i), realMountPort);
                                // 将自动匹配到的端口同步到覆盖值，保证后续连接与前端显示一致
                                mountSerialPortOverride = realMountPort;
                                Logger::Log("ConnectDriver | Mount Device (" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + ") Port is updated to: " + realMountPort.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                            }
                            else
                            {
                                Logger::Log("No matched Mount port found by detector.", LogLevel::WARNING, DeviceType::MAIN);
                                continue;
                            }
                        }else{
                            Logger::Log("ConnectDriver | Mount Device (" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + ") Port is correct.", LogLevel::INFO, DeviceType::MAIN);
                        }
                    }else{
                        Logger::Log("ConnectDriver | Device (" + std::string(indi_Client->GetDeviceFromList(i)->getDeviceName()) + ") Port is not updated.", LogLevel::WARNING, DeviceType::MAIN);
                        emit wsThread->sendMessageToClient("deleteDeviceAllocationList:" + QString::fromUtf8(indi_Client->GetDeviceNameFromList(i).c_str()));
                        indi_Client->disconnectDevice(indi_Client->GetDeviceNameFromList(i).c_str());
                        Logger::Log("ConnectDriver | Device (" + std::string(indi_Client->GetDeviceNameFromList(i).c_str()) + ") is not exist", LogLevel::WARNING, DeviceType::MAIN);
                        indi_Client->RemoveDevice(indi_Client->GetDeviceNameFromList(i).c_str());
                        continue;
                    }
                    // 根据 DriverName 和 DriverType 在 systemdevicelist 中找到对应的设备下标，用于获取正确的波特率
                    int sysIndex = -1;
                    for (int idx = 0; idx < systemdevicelist.system_devices.size(); idx++)
                    {
                        if (systemdevicelist.system_devices[idx].DriverIndiName == DriverName &&
                            systemdevicelist.system_devices[idx].Description == DriverType)
                        {
                            sysIndex = idx;
                            break;
                        }
                    }
                    int baudRateToUse = 9600;
                    if (sysIndex >= 0)
                    {
                        baudRateToUse = systemdevicelist.system_devices[sysIndex].BaudRate;
                    }
                    else
                    {
                        Logger::Log("ConnectDriver | Unable to find SystemDevice for Driver(" + DriverName.toStdString() + "), use default baud 9600",
                                    LogLevel::WARNING, DeviceType::MAIN);
                    }
                    indi_Client->setBaudRate(indi_Client->GetDeviceFromList(i), baudRateToUse);
                    indi_Client->connectDevice(indi_Client->GetDeviceNameFromList(i).c_str());
                    int waitTime = 0;
                    bool connectState = false;
                    while (waitTime < 15)
                    {
                        Logger::Log("ConnectDriver | Wait for Connect " + std::string(indi_Client->GetDeviceNameFromList(i).c_str()), LogLevel::INFO, DeviceType::MAIN);
                        QThread::msleep(1000); // 等待1秒
                        waitTime++;
                        if (indi_Client->GetDeviceFromList(i)->isConnected())
                        {
                            connectState = true;
                            break;
                        }
                    }
                    if (connectState)
                    {
                        connectedDeviceIdList.push_back(i);
                    }
                    else
                    {
                        Logger::Log("ConnectDriver | Device (" + std::string(indi_Client->GetDeviceNameFromList(i).c_str()) + ") is not connected,try to update port", LogLevel::WARNING, DeviceType::MAIN);

                        // 若为 Mount/Focuser 串口设备，连接失败时提示前端弹出串口选择界面
                        if (DriverType == "Mount" || DriverType == "Focuser")
                        {
                            sendSerialPortOptions(DriverType);
                            emit wsThread->sendMessageToClient("RequestSerialPortSelection:" + DriverType);
                        }

                        emit wsThread->sendMessageToClient("deleteDeviceAllocationList:" + QString::fromUtf8(indi_Client->GetDeviceNameFromList(i).c_str()));
                        indi_Client->disconnectDevice(indi_Client->GetDeviceNameFromList(i).c_str());
                        Logger::Log("ConnectDriver | Device (" + std::string(indi_Client->GetDeviceNameFromList(i).c_str()) + ") is not exist", LogLevel::WARNING, DeviceType::MAIN);
                        indi_Client->RemoveDevice(indi_Client->GetDeviceNameFromList(i).c_str());
                        continue;
                    }
                    // emit wsThread->sendMessageToClient("deleteDeviceAllocationList:" + QString::fromUtf8(indi_Client->GetDeviceNameFromList(i).c_str()));
                    // indi_Client->disconnectDevice(indi_Client->GetDeviceNameFromList(i).c_str());
                    // Logger::Log("ConnectDriver | Device (" + std::string(indi_Client->GetDeviceNameFromList(i).c_str()) + ") is not exist", LogLevel::WARNING, DeviceType::MAIN);
                    // indi_Client->RemoveDevice(indi_Client->GetDeviceNameFromList(i).c_str());
                }   
            }
        }
    }

    if (connectedDeviceIdList.size() == 0)
    {
        Logger::Log("ConnectDriver | Requested driver published device but no device connected. Driver=" + DriverName.toStdString() +
                        ", Type=" + DriverType.toStdString(),
                    LogLevel::WARNING, DeviceType::MAIN);
        Tools::stopIndiDriver(DriverName);
        int index = ConnectDriverList.indexOf(DriverName);
        if (index != -1)
        {                                      // 如果找到了
            ConnectDriverList.removeAt(index); // 从列表中删除
            Logger::Log("Driver removed successfully: " + DriverName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            Logger::Log("Driver not found in list: " + DriverName.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
        }
        emit wsThread->sendMessageToClient("ConnectDriverFailed:" + DriverType + ":Device connection failed: " + DriverName);
        return;
    }

    ConnectedCCDList.clear();
    ConnectedTELESCOPEList.clear();
    ConnectedFOCUSERList.clear();
    ConnectedFILTERList.clear();

    // 判断连接设备的类型
    for (int i = 0; i < connectedDeviceIdList.size(); i++)
    {
        if (indi_Client->GetDeviceFromList(connectedDeviceIdList[i])->isConnected())
        {
            if (indi_Client->GetDeviceFromList(connectedDeviceIdList[i])->getDriverInterface() & INDI::BaseDevice::CCD_INTERFACE)
            {
                Logger::Log("ConnectDriver | We received a CCD!", LogLevel::INFO, DeviceType::MAIN);
                ConnectedCCDList.push_back(connectedDeviceIdList[i]);
            }
            else if (indi_Client->GetDeviceFromList(connectedDeviceIdList[i])->getDriverInterface() & INDI::BaseDevice::FILTER_INTERFACE)
            {
                Logger::Log("ConnectDriver | We received a FILTER!", LogLevel::INFO, DeviceType::MAIN);
                ConnectedFILTERList.push_back(connectedDeviceIdList[i]);
            }
            else if (indi_Client->GetDeviceFromList(connectedDeviceIdList[i])->getDriverInterface() & INDI::BaseDevice::TELESCOPE_INTERFACE)
            {
                Logger::Log("ConnectDriver | We received a TELESCOPE!", LogLevel::INFO, DeviceType::MAIN);
                ConnectedTELESCOPEList.push_back(connectedDeviceIdList[i]);
            }
            else if (indi_Client->GetDeviceFromList(connectedDeviceIdList[i])->getDriverInterface() & INDI::BaseDevice::FOCUSER_INTERFACE)
            {
                Logger::Log("ConnectDriver | We received a FOCUSER!", LogLevel::INFO, DeviceType::MAIN);
                ConnectedFOCUSERList.push_back(connectedDeviceIdList[i]);
            }
        }
        else
        {
            Logger::Log("ConnectDriver | Connect failed device:" + std::string(indi_Client->GetDeviceFromList(connectedDeviceIdList[i])->getDeviceName()), LogLevel::WARNING, DeviceType::MAIN);
        }
    }

    QStringList SelectedCameras;
    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        if (systemdevicelist.system_devices[i].DriverIndiName == DriverName)
        {
            SelectedCameras.push_back(systemdevicelist.system_devices[i].Description);
        }
    }
    Logger::Log("ConnectDriver | Driver(" + DriverName.toStdString() + ") supports " + std::to_string(SelectedCameras.size()) + " devices", LogLevel::INFO, DeviceType::MAIN);
    for (auto Camera : SelectedCameras)
    {
        Logger::Log("ConnectDriver | Driver(" + DriverName.toStdString() + ") supports " + Camera.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    }

    Logger::Log("ConnectDriver | Number of Connected CCD:" + std::to_string(ConnectedCCDList.size()), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("ConnectDriver | Number of Connected TELESCOPE:" + std::to_string(ConnectedTELESCOPEList.size()), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("ConnectDriver | Number of Connected FOCUSER:" + std::to_string(ConnectedFOCUSERList.size()), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("ConnectDriver | Number of Connected FILTER:" + std::to_string(ConnectedFILTERList.size()), LogLevel::INFO, DeviceType::MAIN);

    auto savedDeviceNameByDescription = [&](const QString &description) -> QString
    {
        for (int idx = 0; idx < systemdevicelist.system_devices.size(); ++idx)
        {
            if (systemdevicelist.system_devices[idx].Description == description)
                return systemdevicelist.system_devices[idx].DeviceIndiName.trimmed();
        }
        return QString();
    };

    auto findConnectedIndexBySavedName = [&](const QVector<int> &connectedList, const QString &savedName,
                                             const QSet<int> &reserved = QSet<int>()) -> int
    {
        if (savedName.isEmpty())
            return -1;
        for (int idx : connectedList)
        {
            if (reserved.contains(idx))
                continue;
            if (idx < 0 || idx >= indi_Client->GetDeviceCount())
                continue;
            INDI::BaseDevice *device = indi_Client->GetDeviceFromList(idx);
            if (device == nullptr || device->getDeviceName() == nullptr)
                continue;
            if (QString::fromUtf8(device->getDeviceName()) == savedName)
                return idx;
        }
        return -1;
    };

    const bool requestMainCameraOnly = DriverType.contains("MainCamera", Qt::CaseInsensitive);
    const bool requestGuiderOnly = DriverType.contains("Guider", Qt::CaseInsensitive);
    const bool requestPoleOnly = DriverType.contains("PoleCamera", Qt::CaseInsensitive);
    const bool requestSingleCcdRole = requestMainCameraOnly || requestGuiderOnly || requestPoleOnly;

    // 判断连接设备的数量,
    bool EachDeviceOne = true;
    bool hasPendingAllocation = false;

    // 「只有一台就绑给该角色」曾在此特判：它是自动决策（系统替用户挑），
    // 且该分支【不上报候选】——封堵它而不删，会让设备既绑不上也选不了。
    // 删除后 1 台与 N 台走同一条路：持久化回放命中就绑（回放用户上次的手动
    // 选择，不是决策），命中不了就作为候选上报、由用户手动指派。
    // 顺带修掉一个 bug：原特判在只有一台时【根本不查持久化】，会静默覆盖
    // 用户上次的选择（上次选 X、这次只有 Y 在场 → 直接绑 Y）。
    if (SelectedCameras.size() >= 1 || ConnectedCCDList.size() >= 1)
    {
        EachDeviceOne = false;
        QSet<int> boundCcdIndexes;

        for (int i = 0; i < ConnectedCCDList.size(); i++)
        {
            if (boundCcdIndexes.contains(ConnectedCCDList[i]))
                continue;
            // 自动分配已移除：连接后一律上报候选，由用户在分配面板手动指派。
            // 修复：检查索引有效性
            if (ConnectedCCDList[i] >= 0 && ConnectedCCDList[i] < indi_Client->GetDeviceCount()) {
                INDI::BaseDevice *device = indi_Client->GetDeviceFromList(ConnectedCCDList[i]);
                if (device != nullptr) {
                    hasPendingAllocation = true;
                    QString devName = QString::fromUtf8(device->getDeviceName());
                    QString devName_cat;
                    if (devName.contains("5III", Qt::CaseInsensitive))
                        devName_cat = "5III";
                    else if (devName.contains("DEMO", Qt::CaseInsensitive))
                        devName_cat = "DEMO";
                    else
                        devName_cat = "OTHER";
                    emit wsThread->sendMessageToClient("DeviceToBeAllocated:CCD:" + QString::number(ConnectedCCDList[i]) + ":" + devName + ":" + devName_cat); // already allocated
                }
            }
        }
    }

    // 「只有一台就绑给该角色」曾在此特判：它是自动决策（系统替用户挑），
    // 且该分支【不上报候选】——封堵它而不删，会让设备既绑不上也选不了。
    // 删除后 1 台与 N 台走同一条路：持久化回放命中就绑（回放用户上次的手动
    // 选择，不是决策），命中不了就作为候选上报、由用户手动指派。
    // 顺带修掉一个 bug：原特判在只有一台时【根本不查持久化】，会静默覆盖
    // 用户上次的选择（上次选 X、这次只有 Y 在场 → 直接绑 Y）。
    if (ConnectedTELESCOPEList.size() >= 1)
    {
        EachDeviceOne = false;
        const int boundMountIndex = findConnectedIndexBySavedName(ConnectedTELESCOPEList, savedDeviceNameByDescription("Mount"));
        if (boundMountIndex >= 0)
        {
            INDI::BaseDevice *device = indi_Client->GetDeviceFromList(boundMountIndex);
            if (device != nullptr)
            {
                bindDeviceToRole(DeviceSlot::Mount, device);
                Logger::Log("ConnectDriver | INDI Mount auto-bound by saved name: " +
                                QString::fromUtf8(device->getDeviceName()).toStdString(),
                            LogLevel::INFO, DeviceType::MAIN);
            }
        }

        for (int i = 0; i < ConnectedTELESCOPEList.size(); i++)
        {
            if (ConnectedTELESCOPEList[i] == boundMountIndex)
                continue;
            // 修复：检查索引有效性
            if (ConnectedTELESCOPEList[i] >= 0 && ConnectedTELESCOPEList[i] < indi_Client->GetDeviceCount()) {
                INDI::BaseDevice *device = indi_Client->GetDeviceFromList(ConnectedTELESCOPEList[i]);
                if (device != nullptr) {
                    hasPendingAllocation = true;
                    emit wsThread->sendMessageToClient("DeviceToBeAllocated:Mount:" + QString::number(ConnectedTELESCOPEList[i]) + ":" + QString::fromUtf8(device->getDeviceName()));
                }
            }
        }
    }

    // 「只有一台就绑给该角色」曾在此特判：它是自动决策（系统替用户挑），
    // 且该分支【不上报候选】——封堵它而不删，会让设备既绑不上也选不了。
    // 删除后 1 台与 N 台走同一条路：持久化回放命中就绑（回放用户上次的手动
    // 选择，不是决策），命中不了就作为候选上报、由用户手动指派。
    // 顺带修掉一个 bug：原特判在只有一台时【根本不查持久化】，会静默覆盖
    // 用户上次的选择（上次选 X、这次只有 Y 在场 → 直接绑 Y）。
    if (ConnectedFOCUSERList.size() >= 1)
    {
        EachDeviceOne = false;
        const int boundFocuserIndex = findConnectedIndexBySavedName(ConnectedFOCUSERList, savedDeviceNameByDescription("Focuser"));
        if (boundFocuserIndex >= 0)
        {
            INDI::BaseDevice *device = indi_Client->GetDeviceFromList(boundFocuserIndex);
            if (device != nullptr)
            {
                dpFocuser = device;
                if (systemdevicelist.system_devices.size() > 22)
                    systemdevicelist.system_devices[DeviceSlot::Focuser].isConnect = true;
                AfterDeviceConnect(dpFocuser);
                Logger::Log("ConnectDriver | INDI Focuser auto-bound by saved name: " +
                                QString::fromUtf8(device->getDeviceName()).toStdString(),
                            LogLevel::INFO, DeviceType::MAIN);
            }
        }

        for (int i = 0; i < ConnectedFOCUSERList.size(); i++)
        {
            if (ConnectedFOCUSERList[i] == boundFocuserIndex)
                continue;
            // 修复：检查索引有效性
            if (ConnectedFOCUSERList[i] >= 0 && ConnectedFOCUSERList[i] < indi_Client->GetDeviceCount()) {
                INDI::BaseDevice *device = indi_Client->GetDeviceFromList(ConnectedFOCUSERList[i]);
                if (device != nullptr) {
                    hasPendingAllocation = true;
                    emit wsThread->sendMessageToClient("DeviceToBeAllocated:Focuser:" + QString::number(ConnectedFOCUSERList[i]) + ":" + QString::fromUtf8(device->getDeviceName()));
                }
            }
        }
    }

    // 「只有一台就绑给该角色」曾在此特判：它是自动决策（系统替用户挑），
    // 且该分支【不上报候选】——封堵它而不删，会让设备既绑不上也选不了。
    // 删除后 1 台与 N 台走同一条路：持久化回放命中就绑（回放用户上次的手动
    // 选择，不是决策），命中不了就作为候选上报、由用户手动指派。
    // 顺带修掉一个 bug：原特判在只有一台时【根本不查持久化】，会静默覆盖
    // 用户上次的选择（上次选 X、这次只有 Y 在场 → 直接绑 Y）。
    if (ConnectedFILTERList.size() >= 1)
    {
        EachDeviceOne = false;
        const int boundFilterIndex = findConnectedIndexBySavedName(ConnectedFILTERList, savedDeviceNameByDescription("CFW"));
        if (boundFilterIndex >= 0)
        {
            INDI::BaseDevice *device = indi_Client->GetDeviceFromList(boundFilterIndex);
            if (device != nullptr)
            {
                dpCFW = device;
                if (systemdevicelist.system_devices.size() > 21)
                    systemdevicelist.system_devices[DeviceSlot::CFW].isConnect = true;
                AfterDeviceConnect(dpCFW);
                Logger::Log("ConnectDriver | INDI CFW auto-bound by saved name: " +
                                QString::fromUtf8(device->getDeviceName()).toStdString(),
                            LogLevel::INFO, DeviceType::MAIN);
            }
        }

        for (int i = 0; i < ConnectedFILTERList.size(); i++)
        {
            if (ConnectedFILTERList[i] == boundFilterIndex)
                continue;
            // 修复：检查索引有效性
            if (ConnectedFILTERList[i] >= 0 && ConnectedFILTERList[i] < indi_Client->GetDeviceCount()) {
                INDI::BaseDevice *device = indi_Client->GetDeviceFromList(ConnectedFILTERList[i]);
                if (device != nullptr) {
                    hasPendingAllocation = true;
                    emit wsThread->sendMessageToClient("DeviceToBeAllocated:CFW:" + QString::number(ConnectedFILTERList[i]) + ":" + QString::fromUtf8(device->getDeviceName()));
                }
            }
        }
    }

    Logger::Log("Each Device Only Has One:" + std::to_string(EachDeviceOne), LogLevel::INFO, DeviceType::MAIN);
    if (!EachDeviceOne && hasPendingAllocation)
    {
        emit wsThread->sendMessageToClient("ShowDeviceAllocationWindow");
    }
    emit wsThread->sendMessageToClient("AddDeviceType:" + systemdevicelist.system_devices[driverCode].Description);

    // 自动分配移除后：被请求的相机角色不会再被自动绑定，需由用户在候选条中手动指派。
    // 此时必须发 ConnectDriverPendingAllocation 而不是 ConnectDriverSuccess——前端的
    // connectDriverSuccess() 会把二级抽屉/设备页关掉（drawer_2=false），
    // 导致刚由 ShowDeviceAllocationWindow 打开的候选条被立刻关闭、用户看不到相机列表。
    // ConnectDriverPendingAllocation 在前端会保持候选条打开，并同样结束 loading 态。
    const bool requestedCameraRoleUnbound =
        (requestMainCameraOnly && dpMainCamera == nullptr) ||
        (requestGuiderOnly && dpGuider == nullptr) ||
        (requestPoleOnly && dpPoleScope == nullptr);
    if (hasPendingAllocation && requestedCameraRoleUnbound)
    {
        const QString pendingRole = requestMainCameraOnly ? QStringLiteral("MainCamera")
                                  : (requestGuiderOnly ? QStringLiteral("Guider")
                                                       : QStringLiteral("PoleCamera"));
        Logger::Log("ConnectDriver | Requested role " + pendingRole.toStdString() +
                        " awaits manual allocation; sending ConnectDriverPendingAllocation.",
                    LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectDriverPendingAllocation:" + pendingRole);
    }
    else
    {
        emit wsThread->sendMessageToClient("ConnectDriverSuccess:" + DriverName);
    }
}
void MainWindow::DisconnectDevice(MyClient *client, QString DeviceName, QString DeviceType)
{
    // 防御：避免空指针
    if (wsThread == nullptr)
    {
        Logger::Log("DisconnectDevice | wsThread is nullptr (will still cleanup internal state)", LogLevel::WARNING, DeviceType::MAIN);
    }
    if (client == nullptr)
    {
        Logger::Log("DisconnectDevice | client is nullptr", LogLevel::ERROR, DeviceType::MAIN);
        // 继续往下走，至少能清理 systemdevicelist / SDK 句柄
    }

    if (DeviceName == "" || DeviceType == "")
    {
        Logger::Log("DisconnectDevice | DeviceName(" + DeviceName.toStdString() + ") or DeviceType(" + DeviceType.toStdString() + ") is Null", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    if (DeviceType == "Not Bind Device")
    {
        emit wsThread->sendMessageToClient("DisconnectDriverSuccess:" + DeviceType);
        return;
    }
    if (DeviceType == "Guider")

        {
        stopGuiderLoopAndExposure(QStringLiteral("DisconnectDevice:Guider"));
    }

    Logger::Log("DisconnectDevice | Disconnect " + DeviceType.toStdString() + " Device(" + DeviceName.toStdString() + ") start...", LogLevel::INFO, DeviceType::MAIN);

    auto eraseConnectedDeviceByType = [&](const QString &type) {
        for (auto it = ConnectedDevices.begin(); it != ConnectedDevices.end();)
        {
            if (it->DeviceType == type)
                it = ConnectedDevices.erase(it);
            else
                ++it;
        }
    };
    auto appendDeleteCandidate = [](QStringList &names, const QString &name) {
        const QString n = name.trimmed();
        if (n.isEmpty()) return;
        if (n.compare("Not Bind Device", Qt::CaseInsensitive) == 0) return;
        if (!names.contains(n)) names.push_back(n);
    };
    auto emitDeleteDeviceAllocationListBatch = [&](const QStringList &names) {
        if (wsThread == nullptr) return;
        for (const auto &n : names)
        {
            emit wsThread->sendMessageToClient("deleteDeviceAllocationList:" + n);
        }
    };

    // ===== Guider SDK 模式断开 =====
    if (DeviceType == "Guider")
    {
        const bool guiderMarkedSDK =
            (systemdevicelist.system_devices.size() > 1 && systemdevicelist.system_devices[DeviceSlot::Guider].isSDKConnect);

        if (guiderMarkedSDK || sdkGuiderHandle != nullptr)
        {
            Logger::Log("DisconnectDevice | Guider is in SDK mode, closing guider handle ...",
                        LogLevel::INFO, DeviceType::GUIDER);
            if (guiderLoopTimer)
                guiderLoopTimer->stop();
            if (sdkGuiderExposureTimer)
                sdkGuiderExposureTimer->stop();

            // 尝试取消曝光
            if (sdkGuiderHandle != nullptr)
            {
                SdkCommand cancelCmd;
                cancelCmd.type = SdkCommandType::Custom;
                cancelCmd.name = "CancelExposure";
                cancelCmd.payload = std::any();
                SdkManager::instance().callByHandle(sdkGuiderHandle, cancelCmd);
                SdkManager::instance().closeByHandle(sdkGuiderHandle);
            }

            // 解绑池引用（若有）
            if (g_sdkGuiderPoolIndex >= 0 && g_sdkGuiderPoolIndex < g_sdkQhyCamHandles.size())
            {
                g_sdkQhyCamHandles[g_sdkGuiderPoolIndex] = nullptr;
            }

            sdkGuiderHandle = nullptr;
            g_sdkGuiderPoolIndex = -1;
            guiderExposureInFlight = false;
            sdkGuiderFrameTaskInFlight = false;

            if (systemdevicelist.system_devices.size() > 1)
            {
                systemdevicelist.system_devices[DeviceSlot::Guider].isConnect = false;
                systemdevicelist.system_devices[DeviceSlot::Guider].isBind = false;
                // 保留 cameraId，便于下次发现同一设备时自动回绑
            }

            // 单设备 Disconnect 只释放 Guider 对应的 handle/池槽位。
            // MainCamera/PoleCamera 使用不同 handle，共享 SDK 全局资源但不共享连接状态。
            eraseConnectedDeviceByType("Guider");
            if (wsThread != nullptr)
                emit wsThread->sendMessageToClient("DisconnectDriverSuccess:Guider");
            Tools::saveSystemDeviceList(systemdevicelist);
            Logger::Log("DisconnectDevice | Guider (SDK) disconnected.", LogLevel::INFO, DeviceType::GUIDER);
            return;
        }
    }

    // ===== MainCamera SDK 模式断开（此前未实现，导致“断开后仍在调用 SDK”）=====
    if (DeviceType == "MainCamera")
    {
        const bool mainCameraMarkedSDK =
            (systemdevicelist.system_devices.size() > 20 && systemdevicelist.system_devices[DeviceSlot::MainCamera].isSDKConnect);

        if (mainCameraMarkedSDK || sdkMainCameraHandle != nullptr || !g_sdkQhyCamHandles.isEmpty())
        {
            Logger::Log("DisconnectDevice | MainCamera is in SDK mode, disconnect flow with SDK pool ...", LogLevel::INFO, DeviceType::MAIN);
            // 小工具：把主相机 SDK 调用串行投递到主相机通道，避免 UI 线程并发访问同一 handle 触发 SDK 内部崩溃
            auto postToCamThread = [&](std::function<void()> fn) {
                SdkSerialExecutor *mainExec = sdkMainCameraExecutor();
                if (mainExec && mainExec->isRunning())
                    mainExec->post(std::move(fn));
                else
                    fn();
            };

            // 1) 停止 SDK 曝光轮询，避免在句柄关闭后仍然访问
            if (sdkExposureTimer)
                sdkExposureTimer->stop();

            // 1.1) 若主相机存在“相机内置 CFW”，断开主相机时必须同步清理前端的 CFW 入口/状态，
            //      否则前端仍会保留滤镜轮控制入口，造成“CFW 未断开”的现象。
            if (isFilterOnCamera)
            {
                isFilterOnCamera = false;
                sdkMainCfwSlotsCached = 0;
                if (wsThread != nullptr)
                    emit wsThread->sendMessageToClient("deleteDeviceTypeAllocationList:CFW");
            }

            // 2) 关闭当前绑定的 MainCamera 句柄（若存在）
            QString closedCameraId;
            const int boundIdx = g_sdkMainCameraPoolIndex;
            if (sdkMainCameraHandle != nullptr && boundIdx >= 0 && sdkPoolIndexValid(boundIdx))
            {
                closedCameraId = g_sdkQhyCamIds[boundIdx];

                // 注意：GetSingleFrame 等 SDK 调用在主相机 SDK 通道中执行。
                // 如果这里在 UI 线程同步 close，同一 handle 可能被并发访问，导致 SDK 内部段错误。
                const SdkDeviceHandle handleToClose = sdkMainCameraHandle;
                postToCamThread([handleToClose]() {
                    SdkCommand cancelCmd;
                    cancelCmd.type = SdkCommandType::Custom;
                    cancelCmd.name = "CancelExposure";
                    cancelCmd.payload = std::any();
                    // 直接通过设备句柄调用，无需指定驱动名称
                    SdkResult cancelRes = SdkManager::instance().callByHandle(handleToClose, cancelCmd);
                    if (!cancelRes.success)
                    {
                        Logger::Log("DisconnectDevice | CancelExposure failed: " + cancelRes.message, LogLevel::WARNING, DeviceType::MAIN);
                    }

                    // 直接通过设备句柄关闭，无需指定驱动名称
                    SdkResult closeRes = SdkManager::instance().closeByHandle(handleToClose);
                    if (!closeRes.success)
                    {
                        Logger::Log("DisconnectDevice | SDK close failed: " + closeRes.message, LogLevel::WARNING, DeviceType::MAIN);
                    }
                    else
                    {
                        Logger::Log("DisconnectDevice | SDK close success", LogLevel::INFO, DeviceType::MAIN);
                    }
                });

                // 将该相机从“已绑定句柄”释放出来（池保留 cameraId，用于后续重新 open / 待分配）
                g_sdkQhyCamHandles[boundIdx] = nullptr;
            }

            // 4) 清理 MainCamera 绑定状态（SDK 连接模式可保留，视是否释放资源而定）
            sdkMainCameraHandle = nullptr;
            g_sdkMainCameraPoolIndex = -1;
            if (systemdevicelist.system_devices.size() > 20)
            {
                systemdevicelist.system_devices[DeviceSlot::MainCamera].isConnect = false;
                systemdevicelist.system_devices[DeviceSlot::MainCamera].isBind = false;
                // 保留 cameraId，便于下次发现同一设备时自动回绑
            }

            // 状态回到空闲
            glMainCameraStatu = "IDLE";
            ShootStatus = "IDLE";

            // 单设备 Disconnect 不清理共享池；只移除 MainCamera 的运行连接。
            eraseConnectedDeviceByType("MainCamera");
            if (wsThread != nullptr)
                emit wsThread->sendMessageToClient("DisconnectDriverSuccess:MainCamera");

            // 关键修复：
            // 走到这里说明 MainCamera 是 SDK 断开路径。此时不应继续执行下面的 INDI 断开流程，
            // 否则可能对 INDI::BaseDevice*（已被 SDK/INDI 内部异步回收或根本不存在）调用 isConnected() 触发段错误。
            Tools::saveSystemDeviceList(systemdevicelist);
            return;
        }
    }

    // ===== PoleCamera SDK 模式断开 =====
    if (DeviceType == "PoleCamera")
    {
        const bool poleCameraMarkedSDK =
            (systemdevicelist.system_devices.size() > 2 && systemdevicelist.system_devices[DeviceSlot::PoleCamera].isSDKConnect);

        if (poleCameraMarkedSDK || sdkPoleScopeHandle != nullptr)
        {
            Logger::Log("DisconnectDevice | PoleCamera is in SDK mode, closing pole camera handle ...",
                        LogLevel::INFO, DeviceType::MAIN);
            if (sdkGuiderExposureRole == "PoleCamera" && sdkGuiderExposureTimer)
                sdkGuiderExposureTimer->stop();

            if (sdkPoleScopeHandle != nullptr)
            {
                SdkCommand cancelCmd;
                cancelCmd.type = SdkCommandType::Custom;
                cancelCmd.name = "CancelExposure";
                cancelCmd.payload = std::any();
                SdkManager::instance().callByHandle(sdkPoleScopeHandle, cancelCmd);
                SdkManager::instance().closeByHandle(sdkPoleScopeHandle);
            }

            if (g_sdkPoleCameraPoolIndex >= 0 && g_sdkPoleCameraPoolIndex < g_sdkQhyCamHandles.size())
            {
                g_sdkQhyCamHandles[g_sdkPoleCameraPoolIndex] = nullptr;
            }

            sdkPoleScopeHandle = nullptr;
            g_sdkPoleCameraPoolIndex = -1;
            if (sdkGuiderExposureRole == "PoleCamera")
            {
                polarGuiderSingleCapturePending = false;
                guiderExposureInFlight = false;
                sdkGuiderFrameTaskInFlight = false;
                sdkGuiderExposureRole = "Guider";
            }

            if (systemdevicelist.system_devices.size() > 2)
            {
                systemdevicelist.system_devices[DeviceSlot::PoleCamera].isConnect = false;
                systemdevicelist.system_devices[DeviceSlot::PoleCamera].isBind = false;
            }

            eraseConnectedDeviceByType("PoleCamera");
            if (wsThread != nullptr)
                emit wsThread->sendMessageToClient("DisconnectDriverSuccess:PoleCamera");

            Tools::saveSystemDeviceList(systemdevicelist);
            Logger::Log("DisconnectDevice | PoleCamera (SDK) disconnected.", LogLevel::INFO, DeviceType::MAIN);
            return;
        }
    }

    // ===== Focuser SDK 模式断开（串口电调）=====
    if (DeviceType == "Focuser")
    {
        const bool focuserMarkedSDK =
            (systemdevicelist.system_devices.size() > 22 && systemdevicelist.system_devices[DeviceSlot::Focuser].isSDKConnect);

        if (focuserMarkedSDK || sdkFocuserHandle != nullptr)
        {
            Logger::Log("DisconnectDevice | Focuser is in SDK mode, closing serial handle ...",
                        LogLevel::INFO, DeviceType::FOCUSER);
            const QString focuserPortBeforeClear = sdkFocuserPort;

            // 先停止焦点器移动（如果有的话），避免在关闭设备时还有异步任务在执行
            if (focusMoveTimer && focusMoveTimer->isActive())
            {
                focusMoveTimer->stop();
            }
            
            // 停止位置更新定时器，避免触发新的位置读取任务
            if (updatePositionTimer != nullptr)
            {
                updatePositionTimer->stop();
                updatePositionTimer->deleteLater();
                updatePositionTimer = nullptr;
            }
            
            // 重置任务标志，避免新的任务被提交
            sdkFocuserPosTaskInFlight = false;
            sdkFocuserPeriodicTaskInFlight = false;
            
            // 注意：不调用 FocuserControlStop，因为它会触发位置读取任务
            // 直接发送停止命令到 SDK 线程（如果设备还在运行）
            if (sdkFocuserExec && sdkFocuserExec->isRunning() && sdkFocuserHandle != nullptr)
            {
                const SdkDeviceHandle handleSnap = sdkFocuserHandle;
                sdkFocuserExec->post([handleSnap]() {
                    SdkCommand abortCmd;
                    abortCmd.type = SdkCommandType::Custom;
                    abortCmd.name = "Abort";
                    abortCmd.payload = std::any();
                    // 直接通过设备句柄调用，无需指定驱动名称
                SdkManager::instance().callByHandle(handleSnap, abortCmd);
                });
            }

            // 等待 sdkFocuserExec 线程中的所有任务完成，避免在关闭设备后还有任务访问已删除的对象
            // 使用轮询方式等待，避免长时间阻塞 UI
            if (sdkFocuserExec && sdkFocuserExec->isRunning())
            {
                // 处理事件循环，等待异步任务完成
                QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                
                // 轮询等待，最多等待 4 秒（串口超时 3 秒 + 缓冲时间）
                const int maxWaitMs = 4000;
                const int pollIntervalMs = 50;  // 每次轮询间隔 50ms
                int waitedMs = 0;
                
                while (waitedMs < maxWaitMs)
                {
                    // 检查是否还有任务在执行
                    if (!sdkFocuserPosTaskInFlight.load() && !sdkFocuserPeriodicTaskInFlight.load())
                    {
                        // 所有任务完成，可以安全关闭设备
                        break;
                    }
                    
                    QThread::msleep(pollIntervalMs);
                    waitedMs += pollIntervalMs;
                    // 处理事件循环，避免 UI 完全卡死
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                }
                
                // 最后一次处理事件循环
                QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            }

            // 关闭设备句柄（在 SDK 线程中关闭，避免主线程阻塞和死锁）
            if (sdkFocuserHandle != nullptr)
            {
                const SdkDeviceHandle handleSnap = sdkFocuserHandle;
                sdkFocuserHandle = nullptr;  // 先置空，避免新任务使用已关闭的句柄
                
                if (sdkFocuserExec && sdkFocuserExec->isRunning())
                {
                    // 在 SDK 线程中关闭设备，避免主线程阻塞和死锁
                    // 这样可以确保关闭操作在同一个线程中执行，不会与正在执行的任务产生锁竞争
                    sdkFocuserExec->post([handleSnap]() {
                        // 直接通过设备句柄关闭，无需指定驱动名称
                        SdkManager::instance().closeByHandle(handleSnap);
                    });
                    
                    // 等待关闭完成（最多等待 1 秒）
                    // 由于关闭操作在 SDK 线程中执行，我们只需要等待足够的时间让操作完成
                    QThread::msleep(500);  // 等待 500ms，通常足够完成关闭操作
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                }
                else
                {
                    // 如果线程已经停止，直接关闭
                    // 直接通过设备句柄关闭，无需指定驱动名称
                    SdkManager::instance().closeByHandle(handleSnap);
                }
            }
            sdkFocuserPort.clear();

            if (systemdevicelist.system_devices.size() > 22)
            {
                systemdevicelist.system_devices[DeviceSlot::Focuser].isConnect = false;
                systemdevicelist.system_devices[DeviceSlot::Focuser].isBind = false;
                // 保留 isSDKConnect（模式选择），串口路径也保留便于下次重连
                systemdevicelist.system_devices[DeviceSlot::Focuser].dp = NULL;
            }

            eraseConnectedDeviceByType("Focuser");
            QStringList removeNames;
            appendDeleteCandidate(removeNames, DeviceName);
            appendDeleteCandidate(removeNames, focuserPortBeforeClear);
            if (systemdevicelist.system_devices.size() > 22)
                appendDeleteCandidate(removeNames, systemdevicelist.system_devices[DeviceSlot::Focuser].DeviceIndiName);
            emitDeleteDeviceAllocationListBatch(removeNames);
            if (wsThread != nullptr)
            {
                emit wsThread->sendMessageToClient("DisconnectDriverSuccess:Focuser");
            }
            Tools::saveSystemDeviceList(systemdevicelist);
            return;
        }
    }

    int num = 0;
    int thisDriverhasDevice = 0;
    bool driverIsUsed = false;
    bool disconnectsuccess = true;
    QString disconnectdriverName;
    QVector<QString> NeedDisconnectDeviceNameList;

    // INDI 模式断开（若 client 为空则跳过）
    if (client != nullptr)
    {
        for (int i = 0; i < client->GetDeviceCount(); i++)
        {
            INDI::BaseDevice *dev = client->GetDeviceFromList(i);
            if (dev == nullptr || dev->getDeviceName() == nullptr)
                continue;

            if (dev->getDeviceName() == DeviceName)
            {
                client->disconnectDevice(dev->getDeviceName());
                while (dev->isConnected())
                {
                    Logger::Log("DisconnectDevice | Waiting for disconnect finish...", LogLevel::INFO, DeviceType::MAIN);
                    sleep(1);
                    num++;
                    if (num > 5)
                    {
                        Logger::Log("DisconnectDevice | Disconnect " + DeviceType.toStdString() + " Device(" + DeviceName.toStdString() + ") failed.", LogLevel::WARNING, DeviceType::MAIN);
                        disconnectsuccess = false;
                        break;
                    }
                }
                if (!disconnectsuccess)
                {
                    break;
                }
                Logger::Log("DisconnectDevice | Disconnect " + DeviceType.toStdString() + " Device(" + DeviceName.toStdString() + ") success.", LogLevel::INFO, DeviceType::MAIN);

                // 若该主相机有“相机内置 CFW”，断开主相机时必须一并清理 CFW 的前端入口/状态
                //（无论 INDI/SDK：由 isFilterOnCamera 统一标识）
                if (DeviceType == "MainCamera" && isFilterOnCamera)
                {
                    isFilterOnCamera = false;
                    sdkMainCfwSlotsCached = 0;
                    if (wsThread != nullptr)
                        emit wsThread->sendMessageToClient("deleteDeviceTypeAllocationList:CFW");
                }

                if (wsThread != nullptr)
                    emit wsThread->sendMessageToClient("DisconnectDriverSuccess:" + DeviceType);
                break;
            }
        }
    }
    if (!disconnectsuccess)
    {
        Logger::Log("DisconnectDevice | Disconnect " + DeviceType.toStdString() + " Device(" + DeviceName.toStdString() + ") failed.", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("DisconnectDriverFail:" + DeviceType);
    }

    if (DeviceType == "MainCamera")
    {
        dpMainCamera = NULL;
        if (systemdevicelist.system_devices.size() > 20)
        {
            disconnectdriverName = systemdevicelist.system_devices[DeviceSlot::MainCamera].DriverIndiName;
            systemdevicelist.system_devices[DeviceSlot::MainCamera].isConnect = false;
            systemdevicelist.system_devices[DeviceSlot::MainCamera].isBind = false;
            // 对齐 SDK 断开语义：仅复位运行态，保留 DeviceIndiName(型号/实例名)、
            // DeviceIndiGroup、DriverIndiName、DriverFrom、isSDKConnect。型号供左侧显示与
            // 重连，下次连接由 AfterDeviceConnect 用真实 INDI 设备名刷新；清型号只在真正的
            // 删除路径(UnBindingDevice / indi_Driver_Clear)执行。
            // systemdevicelist.system_devices[DeviceSlot::MainCamera].DeviceIndiName = "";  // ❌ 不再清空（对齐 SDK）
            // systemdevicelist.system_devices[DeviceSlot::MainCamera].DriverIndiName = "";  // ❌ 不应清空（驱动名）
            // systemdevicelist.system_devices[DeviceSlot::MainCamera].DriverFrom = "";  // ❌ 不应清空（驱动能力）
            systemdevicelist.system_devices[DeviceSlot::MainCamera].dp = NULL;
        }
    }
    else if (DeviceType == "Guider")
    {
        dpGuider = NULL;
        disconnectdriverName = systemdevicelist.system_devices[DeviceSlot::Guider].DriverIndiName;
        systemdevicelist.system_devices[DeviceSlot::Guider].isConnect = false;
        systemdevicelist.system_devices[DeviceSlot::Guider].isBind = false;
        // 对齐 SDK：仅复位运行态，保留 DeviceIndiName(型号)/DeviceIndiGroup；清型号只在删除路径
        // systemdevicelist.system_devices[DeviceSlot::Guider].DeviceIndiName = "";  // ❌ 不再清空（对齐 SDK）
        // systemdevicelist.system_devices[DeviceSlot::Guider].DriverIndiName = "";  // ❌ 不应清空（驱动名）
        // systemdevicelist.system_devices[DeviceSlot::Guider].DriverFrom = "";  // ❌ 不应清空（驱动能力）
        systemdevicelist.system_devices[DeviceSlot::Guider].dp = NULL;
    }
    else if (DeviceType == "PoleCamera")
    {
        dpPoleScope = NULL;
        disconnectdriverName = systemdevicelist.system_devices[DeviceSlot::PoleCamera].DriverIndiName;
        systemdevicelist.system_devices[DeviceSlot::PoleCamera].isConnect = false;
        systemdevicelist.system_devices[DeviceSlot::PoleCamera].isBind = false;
        // 对齐 SDK：仅复位运行态，保留 DeviceIndiName(型号)/DeviceIndiGroup；清型号只在删除路径
        // systemdevicelist.system_devices[DeviceSlot::PoleCamera].DeviceIndiName = "";  // ❌ 不再清空（对齐 SDK）
        // systemdevicelist.system_devices[DeviceSlot::PoleCamera].DriverIndiName = "";  // ❌ 不应清空（驱动名）
        // systemdevicelist.system_devices[DeviceSlot::PoleCamera].DriverFrom = "";  // ❌ 不应清空（驱动能力）
        systemdevicelist.system_devices[DeviceSlot::PoleCamera].dp = NULL;
    }
    else if (DeviceType == "Mount")
    {
        dpMount = NULL;
        disconnectdriverName = systemdevicelist.system_devices[DeviceSlot::Mount].DriverIndiName;
        systemdevicelist.system_devices[DeviceSlot::Mount].isConnect = false;
        systemdevicelist.system_devices[DeviceSlot::Mount].isBind = false;
        // 对齐 SDK：仅复位运行态，保留 DeviceIndiName(型号)/DeviceIndiGroup；清型号只在删除路径
        // systemdevicelist.system_devices[DeviceSlot::Mount].DeviceIndiName = "";  // ❌ 不再清空（对齐 SDK）
        // systemdevicelist.system_devices[DeviceSlot::Mount].DriverIndiName = "";  // ❌ 不应清空（驱动名）
        // systemdevicelist.system_devices[DeviceSlot::Mount].DriverFrom = "";  // ❌ 不应清空（驱动能力）
        systemdevicelist.system_devices[DeviceSlot::Mount].dp = NULL;
    }
    else if (DeviceType == "Focuser")
    {
        dpFocuser = NULL;
        disconnectdriverName = systemdevicelist.system_devices[DeviceSlot::Focuser].DriverIndiName;
        systemdevicelist.system_devices[DeviceSlot::Focuser].isConnect = false;
        systemdevicelist.system_devices[DeviceSlot::Focuser].isBind = false;
        // 对齐 SDK：仅复位运行态，保留 DeviceIndiName(型号)/DeviceIndiGroup；清型号只在删除路径
        // systemdevicelist.system_devices[DeviceSlot::Focuser].DeviceIndiName = "";  // ❌ 不再清空（对齐 SDK）
        // systemdevicelist.system_devices[DeviceSlot::Focuser].DriverIndiName = "";  // ❌ 不应清空（驱动名）
        // systemdevicelist.system_devices[DeviceSlot::Focuser].DriverFrom = "";  // ❌ 不应清空（驱动能力）
        systemdevicelist.system_devices[DeviceSlot::Focuser].dp = NULL;
    }
    else if (DeviceType == "CFW")
    {
        dpCFW = NULL;
        disconnectdriverName = systemdevicelist.system_devices[DeviceSlot::CFW].DriverIndiName;
        systemdevicelist.system_devices[DeviceSlot::CFW].isConnect = false;
        systemdevicelist.system_devices[DeviceSlot::CFW].isBind = false;
        // 对齐 SDK：仅复位运行态，保留 DeviceIndiName(型号)/DeviceIndiGroup；清型号只在删除路径
        // systemdevicelist.system_devices[DeviceSlot::CFW].DeviceIndiName = "";  // ❌ 不再清空（对齐 SDK）
        // systemdevicelist.system_devices[DeviceSlot::CFW].DriverIndiName = "";  // ❌ 不应清空（驱动名）
        // systemdevicelist.system_devices[DeviceSlot::CFW].DriverFrom = "";  // ❌ 不应清空（驱动能力）
        systemdevicelist.system_devices[DeviceSlot::CFW].dp = NULL;
    }

    QStringList SelectedCameras;
    // QStringList SelectedCameras = Tools::getCameraNumFromSystemDeviceList(systemdevicelist);
    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        // qInfo() << "systemdevicelist.system_devices["<< i <<"].DriverIndiName:" << systemdevicelist.system_devices[i].DriverIndiName;
        // qInfo() << "systemdevicelist.system_devices["<< i <<"].Description:" << systemdevicelist.system_devices[i].Description;
        if (systemdevicelist.system_devices[i].Description != "" && systemdevicelist.system_devices[i].isConnect == true)
        {
            SelectedCameras.push_back(systemdevicelist.system_devices[i].Description);
        }
    }

    for (auto it = ConnectedDevices.begin(); it != ConnectedDevices.end();)
    {
        if (it->DeviceType == DeviceType)
        {
            it = ConnectedDevices.erase(it); // 删除元素并更新迭代器
        }
        else
        {
            ++it; // 仅在不删除时递增迭代器
        }
    }

    // 检查是否有 INDI 设备连接（非 SDK 设备）
    bool hasIndiDeviceConnected = false;
    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        if (!systemdevicelist.system_devices[i].isSDKConnect && 
            systemdevicelist.system_devices[i].isConnect == true)
        {
            hasIndiDeviceConnected = true;
            break;
        }
    }

    // 检查是否有 SDK 设备连接
    bool hasSDKDeviceConnected = false;
    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        if (systemdevicelist.system_devices[i].isSDKConnect && 
            systemdevicelist.system_devices[i].isConnect == true)
        {
            hasSDKDeviceConnected = true;
            break;
        }
    }

    // TODO::这里需要解决无INDI设备和无SDK设备的处理方式
    // 只有在 SDK 和 INDI 设备都没有连接时，才发送 DisconnectDriverSuccess:all 信号
    if (!hasIndiDeviceConnected && !hasSDKDeviceConnected)
    {
        Logger::Log("DisconnectDevice | No Device Connected (neither INDI nor SDK), need to clear all devices", LogLevel::INFO, DeviceType::MAIN);
        disconnectIndiServer(indi_Client);
        // ClearSystemDeviceList();
        clearConnectedDevices();

        initINDIServer();
        initINDIClient();
        // Tools::InitSystemDeviceList();
        // Tools::initSystemDeviceList(systemdevicelist);
        getLastSelectDevice();
        Tools::printSystemDeviceList(systemdevicelist);
        if (wsThread != nullptr) emit wsThread->sendMessageToClient("DisconnectDriverSuccess:all");
        return;
    }

    // Tools::startIndiDriver(disconnectdriverName);
    sleep(1);

    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        if (systemdevicelist.system_devices[i].DriverIndiName == disconnectdriverName)
        {
            if (systemdevicelist.system_devices[i].isConnect == true)
            {
                driverIsUsed = true;
                thisDriverhasDevice++;
            }
        }
    }
    num = 0;
    if (thisDriverhasDevice >= 1 && client->GetDeviceFromListWithName(DeviceName.toStdString())->isConnected() == false)
    {
        client->connectDevice(DeviceName.toStdString().c_str());
        Logger::Log("DisconnectDevice | This Driver has more than one device is using, need to reconnect device(" + DeviceName.toStdString() + ")", LogLevel::INFO, DeviceType::MAIN);
        while (!client->GetDeviceFromListWithName(DeviceName.toStdString())->isConnected())
        {
            Logger::Log("DisconnectDevice | Waiting for connect finish...", LogLevel::INFO, DeviceType::MAIN);
            sleep(1);
            num++;
            if (num > 10)
            {
                Logger::Log("DisconnectDevice | Reconnect Device(" + DeviceName.toStdString() + ") failed.", LogLevel::WARNING, DeviceType::MAIN);
                break;
            }
        }
        if (client->GetDeviceFromListWithName(DeviceName.toStdString())->isConnected())
        {
            Logger::Log("DisconnectDevice | Reconnect Device(" + DeviceName.toStdString() + ") success.", LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            Logger::Log("DisconnectDevice | Reconnect Device(" + DeviceName.toStdString() + ") failed.", LogLevel::WARNING, DeviceType::MAIN);
        }
        emit wsThread->sendMessageToClient("disconnectDevicehasortherdevice:" + disconnectdriverName);
    }
    else
    {
        Tools::stopIndiDriver(disconnectdriverName);
        int index = ConnectDriverList.indexOf(disconnectdriverName);
        if (index != -1)
        {                                      // 如果找到了
            ConnectDriverList.removeAt(index); // 从列表中删除
            Logger::Log("Driver removed successfully: " + disconnectdriverName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            Logger::Log("Driver not found in list: " + disconnectdriverName.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
        }
    }
    emit wsThread->sendMessageToClient("deleteDeviceAllocationList:" + DeviceName);
}

void MainWindow::stopGuiderLoopAndExposure(const QString &reason, bool emitStatus)
{
    Logger::Log("stopGuiderLoopAndExposure | reason=" + reason.toStdString(),
                LogLevel::INFO, DeviceType::GUIDER);

    if (emitStatus && wsThread != nullptr)
    {
        emit wsThread->sendMessageToClient("GuiderSwitchStatus:false");
    }

    isGuiderLoopExp = false;
    guiderExposureInFlight = false;
    sdkGuiderFrameTaskInFlight = false;

    if (guiderLoopTimer)
        guiderLoopTimer->stop();
    if (sdkGuiderExposureTimer)
        sdkGuiderExposureTimer->stop();

    if (guiderCore)
    {
        postGuiderCore(guiderCore, [](GuiderCore *core) {
            core->stopGuiding();
            core->stopLoop();
        });
    }

    if (sdkGuiderHandle != nullptr)
    {
        SdkCommand cancelCmd;
        cancelCmd.type = SdkCommandType::Custom;
        cancelCmd.name = "CancelExposure";
        cancelCmd.payload = std::any();
        SdkResult cancelRes = SdkManager::instance().callByHandle(sdkGuiderHandle, cancelCmd);
        if (!cancelRes.success)
        {
            Logger::Log("stopGuiderLoopAndExposure | SDK CancelExposure failed: " + cancelRes.message,
                        LogLevel::WARNING, DeviceType::GUIDER);
        }
    }
    else if (indi_Client != nullptr && dpGuider != NULL)
    {
        indi_Client->setCCDAbortExposure(dpGuider);
    }

    if (emitStatus && wsThread != nullptr)
    {
        emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
    }
}

bool MainWindow::isDeviceTypeSupportSDK(const QString &description, const QString &driverName)
{
    // 如果 systemdevicelist 中没有找到匹配的设备（例如用户还没确认驱动），
    // 则通过 SdkDriverRegistry 直接查询驱动是否支持 SDK
    // 这样可以确保即使配置信息不完整，也能正确判断SDK支持
    if (!driverName.isEmpty())
    {
        bool supportsSDK = SdkDriverRegistry::instance().supportsSDK(driverName.toStdString());
        if (supportsSDK)
        {
            Logger::Log("isDeviceTypeSupportSDK | Driver " + driverName.toStdString() + 
                       " supports SDK (via SdkDriverRegistry)", 
                       LogLevel::DEBUG, DeviceType::MAIN);
            return true;
        }
    }
    
    return false;
}
