#include "SdkManager.h"
#include "../Logger.h"
#include <unordered_set>
#include <chrono>
#include <sstream>
#include <thread>

namespace {
bool isSdkInitKeyCommand(const std::string& name)
{
    return name == "SetReadMode" || name == "SetStreamMode" || name == "InitCamera";
}

std::string handleToString(SdkDeviceHandle handle)
{
    std::ostringstream oss;
    oss << handle;
    return oss.str();
}
}

/**
 * @brief 获取SdkManager单例实例
 * @return SdkManager的单例引用
 * 
 * 使用静态局部变量实现线程安全的单例模式，确保全局只有一个管理器实例
 */
SdkManager& SdkManager::instance()
{
    static SdkManager inst;
    return inst;
}

/**
 * @brief 注册SDK驱动
 * @param driverName 驱动的主名称（用于查找和调用）
 * @param driver 驱动对象的唯一指针（所有权转移给管理器）
 * 
 * 功能说明：
 * - 将驱动对象存储到管理器中，由管理器统一管理生命周期
 * - 支持驱动别名：驱动可以通过driverNames()返回多个名称
 * - 线程安全：使用互斥锁保护驱动注册过程
 * 
 * 使用示例：
 * @code
 * SdkManager::instance().registerDriver("QHYCCD", std::make_unique<QhyCameraDriver>());
 * @endcode
 */
void SdkManager::registerDriver(const std::string& driverName, std::unique_ptr<ISdkDriver> driver)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // 先保存驱动对象到存储容器，获取原始指针
    // 注意：驱动对象的所有权转移给m_driverStorage，确保生命周期由管理器管理
    m_driverStorage.push_back(std::move(driver));
    ISdkDriver* raw = m_driverStorage.back().get();

    // 注册主名称：使用传入的driverName作为主键
    m_drivers[driverName] = raw;

    // 注册额外别名：由驱动返回的所有名称都注册为指向同一个驱动实例
    // 这样可以通过不同的名称访问同一个驱动，提供向后兼容性
    for (const auto& name : raw->driverNames()) {
        if (!name.empty() && name != driverName) {
            m_drivers[name] = raw;
        }
    }
}

/**
 * @brief 通过驱动名和设备句柄执行命令
 * @param driverName 驱动名称（必须已注册）
 * @param device 设备句柄（由openDevice()返回）
 * @param command 要执行的命令
 * @return 执行结果，包含成功状态、错误信息和返回数据
 * 
 * 功能说明：
 * - 查找指定的驱动并执行命令
 * - 线程安全：驱动查找过程受互斥锁保护
 * - 性能优化：互斥锁仅在查找驱动时持有，不阻塞SDK调用
 * 
 * 使用示例：
 * @code
 * SdkCommand cmd{SdkCommandType::Custom, "GetTemperature", {}};
 * SdkResult result = SdkManager::instance().call("QHYCCD", handle, cmd);
 * if (result.success) {
 *     double temp = std::any_cast<double>(result.payload);
 * }
 * @endcode
 */
SdkResult SdkManager::call(const std::string& driverName,
                           SdkDeviceHandle device,
                           const SdkCommand& command)
{
    ISdkDriver* driver = nullptr;
    ISdkLogger* logger = nullptr;
    {
        // 仅保护驱动查找与logger指针读取，避免把mutex持有到阻塞式SDK调用结束
        // 这样可以提高并发性能，避免长时间持有锁
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_drivers.find(driverName);
        if (it != m_drivers.end())
            driver = it->second;
        logger = m_logger;
    }

    // 检查驱动是否存在
    if (!driver) {
        SdkResult r;
        r.success = false;
        r.errorCode = SdkErrorCode::DriverNotFound;
        r.message = "驱动未找到: " + driverName;
        if (logger) {
            logger->log(SdkLogLevel::Error, r.message);
        } else {
            Logger::Log(r.message, LogLevel::ERROR, DeviceType::MAIN);
        }
        return r;
    }

    const bool skipNoisyLog = (command.name == "GetCurrentTemperature");
    const bool keyInitCmd = isSdkInitKeyCommand(command.name);

    // 记录调试日志（跳过温度查询命令以避免频繁打印）
    if (!skipNoisyLog) {
        const std::string preLog =
            "调用驱动: " + driverName +
            ", 命令: " + command.name +
            ", handle=" + handleToString(device) +
            ", thread=" + std::to_string(
                static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
        if (logger) {
            logger->log(SdkLogLevel::Debug, preLog);
        } else {
            Logger::Log(preLog, LogLevel::DEBUG, DeviceType::MAIN);
        }
    }

    // 执行命令（此时已释放锁，不会阻塞其他操作）
    const auto t0 = std::chrono::steady_clock::now();
    SdkResult execResult;
    try {
        execResult = driver->execute(device, command);
    } catch (const std::exception& e) {
        execResult.success = false;
        execResult.errorCode = SdkErrorCode::OperationFailed;
        execResult.message = std::string("driver->execute exception: ") + e.what();
    } catch (...) {
        execResult.success = false;
        execResult.errorCode = SdkErrorCode::OperationFailed;
        execResult.message = "driver->execute unknown exception";
    }
    const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();

    // 对关键初始化命令和失败命令补充结束日志，便于定位崩溃前最后一步
    if (!skipNoisyLog && (keyInitCmd || !execResult.success)) {
        const std::string postLog =
            "调用完成: " + driverName +
            ", 命令: " + command.name +
            ", ok=" + std::string(execResult.success ? "true" : "false") +
            ", costMs=" + std::to_string(dtMs) +
            ", errorCode=" + std::to_string(static_cast<int>(execResult.errorCode)) +
            ", msg=" + execResult.message;
        if (logger) {
            logger->log(execResult.success ? SdkLogLevel::Debug : SdkLogLevel::Error, postLog);
        } else {
            Logger::Log(postLog,
                        execResult.success ? LogLevel::DEBUG : LogLevel::ERROR,
                        DeviceType::MAIN);
        }
    }
    return execResult;
}

/**
 * @brief 打开设备
 * @param driverName 驱动名称
 * @param openParam 打开参数（类型由具体驱动决定，如设备ID、串口路径等）
 * @return 执行结果，成功时payload包含设备句柄(SdkDeviceHandle)
 * 
 * 功能说明：
 * - 查找驱动并调用其openDevice方法
 * - 返回的设备句柄需要由调用者保存，用于后续操作
 * - 建议使用openAndRegister()自动注册到设备注册表
 * 
 * 使用示例：
 * @code
 * // 方式1：直接打开（需要手动管理句柄）
 * SdkResult result = SdkManager::instance().open("QHYCCD", std::string("相机ID"));
 * if (result.success) {
 *     SdkDeviceHandle handle = std::any_cast<SdkDeviceHandle>(result.payload);
 *     // 使用handle进行操作...
 * }
 * 
 * // 方式2：打开并注册（推荐，类似INDI的调用方式）
 * result = SdkManager::instance().openAndRegister("QHYCCD", std::string("相机ID"), "MainCamera");
 * @endcode
 */
SdkResult SdkManager::open(const std::string& driverName, const std::any& openParam)
{
    ISdkDriver* driver = nullptr;
    ISdkLogger* logger = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_drivers.find(driverName);
        if (it != m_drivers.end())
            driver = it->second;
        logger = m_logger;
    }

    if (!driver) {
        SdkResult r;
        r.success = false;
        r.errorCode = SdkErrorCode::DriverNotFound;
        r.message = "驱动未找到: " + driverName;
        if (logger) {
            logger->log(SdkLogLevel::Error, r.message);
        } else {
            Logger::Log(r.message, LogLevel::ERROR, DeviceType::MAIN);
        }
        return r;
    }

    return driver->openDevice(openParam);
}

/**
 * @brief 关闭设备
 * @param driverName 驱动名称
 * @param device 设备句柄（由openDevice()返回）
 * @return 执行结果
 * 
 * 功能说明：
 * - 查找驱动并调用其closeDevice方法
 * - 关闭后设备句柄失效，不应再使用
 * - 如果设备已注册到注册表，建议使用closeById()自动清理
 * 
 * 使用示例：
 * @code
 * SdkResult result = SdkManager::instance().close("QHYCCD", handle);
 * if (!result.success) {
 *     // 处理关闭失败的情况
 * }
 * @endcode
 */
SdkResult SdkManager::close(const std::string& driverName, SdkDeviceHandle device)
{
    ISdkDriver* driver = nullptr;
    ISdkLogger* logger = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_drivers.find(driverName);
        if (it != m_drivers.end())
            driver = it->second;
        logger = m_logger;
    }

    if (!driver) {
        SdkResult r;
        r.success = false;
        r.errorCode = SdkErrorCode::DriverNotFound;
        r.message = "驱动未找到: " + driverName;
        if (logger) {
            logger->log(SdkLogLevel::Error, r.message);
        } else {
            Logger::Log(r.message, LogLevel::ERROR, DeviceType::MAIN);
        }
        return r;
    }

    return driver->closeDevice(device);
}

void SdkManager::setLogger(ISdkLogger* logger)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_logger = logger;
}

ISdkLogger* SdkManager::logger() const
{
    return m_logger;
}

std::vector<SdkCommandInfo> SdkManager::listCommands(const std::string& driverName)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<SdkCommandInfo> empty;

    auto it = m_drivers.find(driverName);
    if (it == m_drivers.end()) {
        return empty;
    }

    return it->second->commandList();
}

/**
 * @brief 通过日志输出某个驱动支持的所有命令及说明
 * @param driverName 驱动名称
 * 
 * 功能说明：
 * - 获取驱动支持的所有命令并输出到日志
 * - 用于调试和了解驱动功能
 * 
 * 使用示例：
 * @code
 * SdkManager::instance().logCommands("QHYCCD");
 * @endcode
 */
void SdkManager::logCommands(const std::string& driverName)
{
    auto cmds = listCommands(driverName);
    if (cmds.empty()) {
        std::string msg = "未找到命令或驱动不存在: " + driverName;
        if (m_logger) {
            m_logger->log(SdkLogLevel::Info, msg);
        } else {
            Logger::Log(msg, LogLevel::INFO, DeviceType::MAIN);
        }
        return;
    }

    for (const auto& c : cmds) {
        std::string line = "SDK[" + driverName + "] 命令: " + c.name + " - " + c.description;
        if (m_logger) {
            m_logger->log(SdkLogLevel::Info, line);
        } else {
            Logger::Log(line, LogLevel::INFO, DeviceType::MAIN);
        }
    }
}

// ==================== 设备注册表管理方法 ====================

/**
 * @brief 手动注册设备到注册表
 * @param driverName 驱动名称
 * @param deviceId 设备唯一标识（如序列号、设备名称等）
 * @param handle 设备句柄（由openDevice()返回）
 * @param description 设备描述信息（可选）
 * @param metadata 设备元数据（可选，如能力信息、版本等）
 * @return 执行结果
 * 
 * 功能说明：
 * - 将设备信息注册到管理器，便于后续通过deviceId访问
 * - 如果deviceId已存在，将覆盖旧记录
 * - 注册后可以使用callById()和closeById()简化调用
 * 
 * 使用示例：
 * @code
 * SdkResult openResult = SdkManager::instance().open("QHYCCD", std::string("相机ID"));
 * if (openResult.success) {
 *     SdkDeviceHandle handle = std::any_cast<SdkDeviceHandle>(openResult.payload);
 *     SdkManager::instance().registerDevice("QHYCCD", "MainCamera", handle, "主相机");
 * }
 * @endcode
 */
SdkResult SdkManager::registerDevice(const std::string& driverName,
                                     const std::string& deviceId,
                                     SdkDeviceHandle handle,
                                     const std::string& description,
                                     const std::any& metadata)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // 检查驱动是否存在
    auto it = m_drivers.find(driverName);
    if (it == m_drivers.end()) {
        SdkResult r;
        r.success = false;
        r.errorCode = SdkErrorCode::DriverNotFound;
        r.message = "驱动未找到: " + driverName;
        if (m_logger) {
            m_logger->log(SdkLogLevel::Error, r.message);
        } else {
            Logger::Log(r.message, LogLevel::ERROR, DeviceType::MAIN);
        }
        return r;
    }

    // 创建或更新设备信息
    SdkDeviceInfo info;
    info.deviceId = deviceId;
    info.driverName = driverName;
    info.description = description;
    info.handle = handle;
    info.state = SdkDeviceState::Open;
    info.metadata = metadata;

    // 注册到设备表（如果已存在则覆盖）
    m_devices[deviceId] = info;

    std::string msg = "设备已注册: " + deviceId + " (驱动: " + driverName + ")";
    if (m_logger) {
        m_logger->log(SdkLogLevel::Info, msg);
    } else {
        Logger::Log(msg, LogLevel::INFO, DeviceType::MAIN);
    }

    SdkResult r;
    r.success = true;
    r.errorCode = SdkErrorCode::Success;
    r.message = msg;
    return r;
}

/**
 * @brief 从注册表取消注册设备（不关闭设备）
 * @param deviceId 设备唯一标识
 * @return 执行结果
 * 
 * 功能说明：
 * - 从注册表中移除设备信息，但不关闭设备
 * - 设备句柄仍然有效，可以继续使用
 * - 如果设备不存在，返回成功（幂等操作）
 * 
 * 使用示例：
 * @code
 * SdkManager::instance().unregisterDevice("MainCamera");
 * @endcode
 */
SdkResult SdkManager::unregisterDevice(const std::string& deviceId)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_devices.find(deviceId);
    if (it == m_devices.end()) {
        // 设备不存在，返回成功（幂等操作）
        SdkResult r;
        r.success = true;
        r.errorCode = SdkErrorCode::Success;
        r.message = "设备不存在（可能已取消注册）: " + deviceId;
        return r;
    }

    m_devices.erase(it);

    std::string msg = "设备已取消注册: " + deviceId;
    if (m_logger) {
        m_logger->log(SdkLogLevel::Info, msg);
    } else {
        Logger::Log(msg, LogLevel::INFO, DeviceType::MAIN);
    }

    SdkResult r;
    r.success = true;
    r.errorCode = SdkErrorCode::Success;
    r.message = msg;
    return r;
}

/**
 * @brief 更新设备状态
 * @param deviceId 设备唯一标识
 * @param state 新状态
 * @return 执行结果
 * 
 * 功能说明：
 * - 更新注册表中设备的状态信息
 * - 用于跟踪设备状态变化（打开/关闭/错误等）
 * 
 * 使用示例：
 * @code
 * SdkManager::instance().updateDeviceState("MainCamera", SdkDeviceState::Error);
 * @endcode
 */
SdkResult SdkManager::updateDeviceState(const std::string& deviceId, SdkDeviceState state)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_devices.find(deviceId);
    if (it == m_devices.end()) {
        SdkResult r;
        r.success = false;
        r.errorCode = SdkErrorCode::DeviceNotFound;
        r.message = "设备未找到: " + deviceId;
        if (m_logger) {
            m_logger->log(SdkLogLevel::Warning, r.message);
        } else {
            Logger::Log(r.message, LogLevel::WARNING, DeviceType::MAIN);
        }
        return r;
    }

    it->second.state = state;
    // 可以在这里添加时间戳等额外信息

    SdkResult r;
    r.success = true;
    r.errorCode = SdkErrorCode::Success;
    r.message = "设备状态已更新: " + deviceId;
    return r;
}

/**
 * @brief 根据设备ID获取设备信息
 * @param deviceId 设备唯一标识
 * @return 设备信息，如果不存在则返回state=Unknown且handle=nullptr的默认值
 * 
 * 功能说明：
 * - 从注册表中查找设备信息
 * - 线程安全：使用互斥锁保护
 * - 如果设备不存在，返回默认值（不会抛出异常）
 * 
 * 使用示例：
 * @code
 * SdkDeviceInfo info = SdkManager::instance().getDevice("MainCamera");
 * if (info.state == SdkDeviceState::Open) {
 *     // 设备已打开，可以使用
 * }
 * @endcode
 */
SdkDeviceInfo SdkManager::getDevice(const std::string& deviceId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_devices.find(deviceId);
    if (it == m_devices.end()) {
        // 返回默认值（state=Unknown, handle=nullptr）
        SdkDeviceInfo empty;
        empty.deviceId = deviceId;
        empty.state = SdkDeviceState::Unknown;
        return empty;
    }

    return it->second;
}

/**
 * @brief 列出所有已注册的设备
 * @param driverName 驱动名称过滤（可选，为空则返回所有设备）
 * @return 设备信息列表
 * 
 * 功能说明：
 * - 返回注册表中所有设备的信息
 * - 可以按驱动名称过滤
 * - 线程安全：使用互斥锁保护
 * 
 * 使用示例：
 * @code
 * // 列出所有设备
 * auto allDevices = SdkManager::instance().listDevices();
 * 
 * // 只列出QHYCCD驱动的设备
 * auto qhyDevices = SdkManager::instance().listDevices("QHYCCD");
 * @endcode
 */
std::vector<SdkDeviceInfo> SdkManager::listDevices(const std::string& driverName) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<SdkDeviceInfo> result;

    if (driverName.empty()) {
        // 返回所有设备
        result.reserve(m_devices.size());
        for (const auto& pair : m_devices) {
            result.push_back(pair.second);
        }
    } else {
        // 只返回指定驱动的设备
        for (const auto& pair : m_devices) {
            if (pair.second.driverName == driverName) {
                result.push_back(pair.second);
            }
        }
    }

    return result;
}

/**
 * @brief 通过设备ID调用命令（类似INDI的调用方式）
 * @param deviceId 设备唯一标识（必须在注册表中）
 * @param command 要执行的命令
 * @return 执行结果
 * 
 * 功能说明：
 * - 从注册表中查找设备，然后调用对应的驱动
 * - 简化调用：不需要手动传递驱动名和句柄
 * - 类似INDI的调用方式，更加方便
 * 
 * 使用示例：
 * @code
 * SdkCommand cmd{SdkCommandType::Custom, "GetTemperature", {}};
 * SdkResult result = SdkManager::instance().callById("MainCamera", cmd);
 * if (result.success) {
 *     double temp = std::any_cast<double>(result.payload);
 * }
 * @endcode
 */
SdkResult SdkManager::callById(const std::string& deviceId, const SdkCommand& command)
{
    SdkDeviceInfo device;
    ISdkLogger* logger = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_devices.find(deviceId);
        if (it == m_devices.end()) {
            SdkResult r;
            r.success = false;
            r.errorCode = SdkErrorCode::DeviceNotFound;
            r.message = "设备未找到: " + deviceId;
            if (m_logger) {
                m_logger->log(SdkLogLevel::Error, r.message);
            } else {
                Logger::Log(r.message, LogLevel::ERROR, DeviceType::MAIN);
            }
            return r;
        }
        device = it->second;
        logger = m_logger;
    }

    // 检查设备状态
    if (device.state != SdkDeviceState::Open) {
        SdkResult r;
        r.success = false;
        r.errorCode = SdkErrorCode::DeviceBusy;
        r.message = "设备状态异常，无法执行命令: " + deviceId + " (状态: " + 
                    (device.state == SdkDeviceState::Closed ? "已关闭" : 
                     device.state == SdkDeviceState::Error ? "错误" : "未知");
        if (logger) {
            logger->log(SdkLogLevel::Warning, r.message);
        } else {
            Logger::Log(r.message, LogLevel::WARNING, DeviceType::MAIN);
        }
        return r;
    }

    // 调用驱动执行命令
    return call(device.driverName, device.handle, command);
}

/**
 * @brief 通过设备ID关闭设备并从注册表移除
 * @param deviceId 设备唯一标识
 * @return 执行结果
 * 
 * 功能说明：
 * - 从注册表中查找设备
 * - 调用驱动关闭设备
 * - 从注册表中移除设备信息
 * - 一步完成关闭和清理，类似INDI的调用方式
 * 
 * 使用示例：
 * @code
 * SdkResult result = SdkManager::instance().closeById("MainCamera");
 * if (!result.success) {
 *     // 处理关闭失败
 * }
 * @endcode
 */
SdkResult SdkManager::closeById(const std::string& deviceId)
{
    SdkDeviceInfo device;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_devices.find(deviceId);
        if (it == m_devices.end()) {
            SdkResult r;
            r.success = false;
            r.errorCode = SdkErrorCode::DeviceNotFound;
            r.message = "设备未找到: " + deviceId;
            if (m_logger) {
                m_logger->log(SdkLogLevel::Warning, r.message);
            } else {
                Logger::Log(r.message, LogLevel::WARNING, DeviceType::MAIN);
            }
            return r;
        }
        device = it->second;
    }

    // 调用驱动关闭设备
    SdkResult closeResult = close(device.driverName, device.handle);

    // 无论关闭是否成功，都从注册表中移除
    unregisterDevice(deviceId);

    return closeResult;
}

/**
 * @brief 通过设备句柄调用命令
 * @param handle 设备句柄
 * @param command 要执行的命令
 * @return 执行结果
 * 
 * 自动查找设备句柄对应的驱动并执行命令，无需手动指定驱动名称。
 * 类似INDI的调用方式，直接传入设备指针即可。
 */
SdkResult SdkManager::callByHandle(SdkDeviceHandle handle, const SdkCommand& command)
{
    if (handle == nullptr) {
        SdkResult r;
        r.success = false;
        r.errorCode = SdkErrorCode::InvalidParameter;
        r.message = "设备句柄为空";
        if (m_logger) {
            m_logger->log(SdkLogLevel::Error, r.message);
        } else {
            Logger::Log(r.message, LogLevel::ERROR, DeviceType::MAIN);
        }
        return r;
    }

    // 遍历所有已注册的设备，查找匹配的句柄
    std::string driverName;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& pair : m_devices) {
            if (pair.second.handle == handle) {
                driverName = pair.second.driverName;
                break;
            }
        }
    }

    if (driverName.empty()) {
        SdkResult r;
        r.success = false;
        r.errorCode = SdkErrorCode::DeviceNotFound;
        r.message = "未找到设备句柄对应的驱动";
        if (m_logger) {
            m_logger->log(SdkLogLevel::Warning, r.message);
        } else {
            Logger::Log(r.message, LogLevel::WARNING, DeviceType::MAIN);
        }
        return r;
    }

    // 调用对应的驱动
    return call(driverName, handle, command);
}

/**
 * @brief 通过设备句柄关闭设备
 * @param handle 设备句柄
 * @return 执行结果
 * 
 * 自动查找设备句柄对应的驱动并关闭设备，无需手动指定驱动名称。
 * 注意：此方法不会从注册表移除设备，如需清理注册表请使用closeById()。
 */
SdkResult SdkManager::closeByHandle(SdkDeviceHandle handle)
{
    if (handle == nullptr) {
        SdkResult r;
        r.success = false;
        r.errorCode = SdkErrorCode::InvalidParameter;
        r.message = "设备句柄为空";
        if (m_logger) {
            m_logger->log(SdkLogLevel::Error, r.message);
        } else {
            Logger::Log(r.message, LogLevel::ERROR, DeviceType::MAIN);
        }
        return r;
    }

    // 遍历所有已注册的设备，查找匹配的句柄
    std::string driverName;
    std::string deviceId;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& pair : m_devices) {
            if (pair.second.handle == handle) {
                driverName = pair.second.driverName;
                deviceId = pair.first;
                break;
            }
        }
    }

    if (driverName.empty()) {
        SdkResult r;
        r.success = false;
        r.errorCode = SdkErrorCode::DeviceNotFound;
        r.message = "未找到设备句柄对应的驱动";
        if (m_logger) {
            m_logger->log(SdkLogLevel::Warning, r.message);
        } else {
            Logger::Log(r.message, LogLevel::WARNING, DeviceType::MAIN);
        }
        return r;
    }

    // 调用对应的驱动关闭设备
    SdkResult closeResult = close(driverName, handle);

    // 无论关闭是否成功，都从注册表中移除设备（如果找到了对应的deviceId）
    if (!deviceId.empty()) {
        unregisterDevice(deviceId);
    }

    return closeResult;
}

/**
 * @brief 扫描可用设备（如果驱动支持）
 * @param driverName 驱动名称
 * @param devices 输出参数，扫描到的设备列表
 * @return 执行结果
 * 
 * 功能说明：
 * - 调用驱动的scanDevices方法扫描可用设备
 * - 如果驱动未实现此方法，返回NotImplemented错误
 * - 扫描到的设备信息包含在devices参数中
 * 
 * 使用示例：
 * @code
 * std::vector<SdkDeviceInfo> devices;
 * SdkResult result = SdkManager::instance().scanDevices("QHYCCD", devices);
 * if (result.success) {
 *     for (const auto& dev : devices) {
 *         qDebug() << "发现设备:" << dev.deviceId.c_str();
 *     }
 * }
 * @endcode
 */
SdkResult SdkManager::scanDevices(const std::string& driverName, std::vector<SdkDeviceInfo>& devices)
{
    ISdkDriver* driver = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_drivers.find(driverName);
        if (it != m_drivers.end())
            driver = it->second;
    }

    if (!driver) {
        SdkResult r;
        r.success = false;
        r.errorCode = SdkErrorCode::DriverNotFound;
        r.message = "驱动未找到: " + driverName;
        if (m_logger) {
            m_logger->log(SdkLogLevel::Error, r.message);
        } else {
            Logger::Log(r.message, LogLevel::ERROR, DeviceType::MAIN);
        }
        return r;
    }

    // 调用驱动的扫描方法
    SdkResult result = driver->scanDevices(devices);
    
    // 为扫描到的设备设置驱动名称
    for (auto& dev : devices) {
        dev.driverName = driverName;
    }

    return result;
}

/**
 * @brief 获取驱动的能力描述
 * @param driverName 驱动名称
 * @return 驱动能力描述
 * 
 * 功能说明：
 * - 返回驱动支持的命令、参数范围等信息
 * - 用于了解驱动的功能和限制
 * 
 * 使用示例：
 * @code
 * SdkDeviceCapabilities caps = SdkManager::instance().capabilities("QHYCCD");
 * qDebug() << "支持多设备:" << caps.supportsMultiDevice;
 * @endcode
 */
SdkDeviceCapabilities SdkManager::capabilities(const std::string& driverName) const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_drivers.find(driverName);
    if (it == m_drivers.end()) {
        // 驱动不存在，返回默认能力（空）
        return {};
    }

    return it->second->capabilities();
}

/**
 * @brief 打开设备并自动注册到注册表（推荐使用，类似INDI的调用方式）
 * @param driverName 驱动名称
 * @param openParam 打开参数
 * @param deviceId 设备唯一标识（用于注册）
 * @param description 设备描述（可选）
 * @param metadata 设备元数据（可选）
 * @return 执行结果
 * 
 * 功能说明：
 * - 一步完成打开设备和注册，简化调用流程
 * - 类似INDI的调用方式：openAndRegister("QHYCCD", param, "MainCamera")
 * - 如果打开失败，不会注册到注册表
 * 
 * 使用示例：
 * @code
 * // 简单的一行调用，类似INDI
 * SdkResult result = SdkManager::instance().openAndRegister(
 *     "QHYCCD", 
 *     std::string("相机ID"), 
 *     "MainCamera",
 *     "主相机"
 * );
 * 
 * if (result.success) {
 *     // 设备已打开并注册，可以直接使用callById()调用
 *     SdkCommand cmd{SdkCommandType::Custom, "GetTemperature", {}};
 *     SdkResult tempResult = SdkManager::instance().callById("MainCamera", cmd);
 * }
 * @endcode
 */
SdkResult SdkManager::openAndRegister(const std::string& driverName,
                                      const std::any& openParam,
                                      const std::string& deviceId,
                                      const std::string& description,
                                      const std::any& metadata)
{
    // 先打开设备
    SdkResult openResult = open(driverName, openParam);
    if (!openResult.success) {
        return openResult;
    }

    // 从结果中提取设备句柄
    SdkDeviceHandle handle = nullptr;
    try {
        handle = std::any_cast<SdkDeviceHandle>(openResult.payload);
    } catch (const std::bad_any_cast&) {
        SdkResult r;
        r.success = false;
        r.errorCode = SdkErrorCode::InvalidParameter;
        r.message = "打开设备返回的句柄类型错误";
        if (m_logger) {
            m_logger->log(SdkLogLevel::Error, r.message);
        } else {
            Logger::Log(r.message, LogLevel::ERROR, DeviceType::MAIN);
        }
        return r;
    }

    // 注册到设备表
    return registerDevice(driverName, deviceId, handle, description, metadata);
}

std::vector<std::string> SdkManager::listRegisteredDrivers() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    std::vector<std::string> result;
    result.reserve(m_drivers.size());
    
    for (const auto& [name, _] : m_drivers)
    {
        result.push_back(name);
    }
    
    return result;
}

std::vector<std::string> SdkManager::getDriverAliases(const std::string& driverName) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_drivers.find(driverName);
    if (it != m_drivers.end())
    {
        return it->second->driverNames();
    }
    
    return {};
}

/**
 * @brief 根据设备类型查找驱动名称
 * @param deviceType 设备类型（相机、赤道仪、电调等）
 * @return 匹配的驱动名称列表（可能有多个驱动支持同一设备类型）
 * 
 * 功能说明：
 * - 遍历所有已注册的驱动，检查其设备类型
 * - 返回设备类型匹配的驱动名称列表
 * - 如果找不到匹配的驱动，返回空列表
 * 
 * 使用示例：
 * @code
 * // 查找所有电调驱动
 * auto focuserDrivers = SdkManager::instance().findDriversByType(SdkDeviceType::Focuser);
 * if (!focuserDrivers.empty()) {
 *     std::string driverName = focuserDrivers[0];  // 使用第一个匹配的驱动
 * }
 * @endcode
 */
std::vector<std::string> SdkManager::findDriversByType(SdkDeviceType deviceType) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    
    std::vector<std::string> result;
    
    // 遍历所有驱动，检查设备类型
    // 注意：需要去重，因为同一个驱动可能有多个别名
    std::unordered_set<std::string> seenDrivers;
    
    for (const auto& pair : m_drivers) {
        const std::string& driverName = pair.first;
        ISdkDriver* driver = pair.second;
        
        // 获取驱动的能力描述
        SdkDeviceCapabilities caps = driver->capabilities();
        
        // 检查设备类型是否匹配
        if (caps.deviceType == deviceType) {
            // 获取驱动的主名称（第一个别名）
            auto aliases = driver->driverNames();
            if (!aliases.empty()) {
                const std::string& primaryName = aliases[0];
                // 只添加主名称，避免重复
                if (seenDrivers.find(primaryName) == seenDrivers.end()) {
                    result.push_back(primaryName);
                    seenDrivers.insert(primaryName);
                }
            }
        }
    }
    
    return result;
}
