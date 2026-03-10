#ifndef SDKMANAGER_H
#define SDKMANAGER_H

#include "SdkCommon.h"
#include "SdkDriver.h"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <vector>

/**
 * @class SdkManager
 * @brief SDK管理器：统一管理所有SDK驱动和设备
 * 
 * SdkManager是SDK管理框架的核心类，采用单例模式，提供以下功能：
 * 
 * 1. **驱动管理**：
 *    - 注册和管理所有SDK驱动
 *    - 通过驱动名称查找和调用驱动
 * 
 * 2. **设备管理**：
 *    - 设备注册表：统一管理所有已打开的设备
 *    - 设备状态跟踪：跟踪设备状态变化
 *    - 设备发现：扫描可用设备
 * 
 * 3. **统一调用接口**：
 *    - 提供类似INDI的简单调用方式
 *    - 支持通过设备ID直接调用，无需手动管理驱动名和句柄
 * 
 * 设计特点：
 * - 线程安全：所有操作都受互斥锁保护
 * - 易于扩展：通过驱动注册机制轻松添加新驱动
 * - 使用简单：提供类似INDI的调用方式，一行代码完成设备操作
 * 
 * 使用示例：
 * @code
 * // 1. 打开并注册设备（推荐方式，类似INDI）
 * SdkResult result = SdkManager::instance().openAndRegister(
 *     "QHYCCD", 
 *     std::string("相机ID"), 
 *     "MainCamera"
 * );
 * 
 * // 2. 通过设备ID调用命令（简单方便）
 * SdkCommand cmd{SdkCommandType::Custom, "GetTemperature", {}};
 * SdkResult tempResult = SdkManager::instance().callById("MainCamera", cmd);
 * 
 * // 3. 关闭设备（自动清理注册表）
 * SdkManager::instance().closeById("MainCamera");
 * @endcode
 */
class SdkManager {
public:
    /**
     * @brief 获取SdkManager单例实例
     * @return SdkManager的单例引用
     * 
     * 使用静态局部变量实现线程安全的单例模式。
     */
    static SdkManager& instance();

    // ==================== 驱动管理接口 ====================

    /**
     * @brief 注册SDK驱动
     * @param driverName 驱动的主名称
     * @param driver 驱动对象的唯一指针（所有权转移给管理器）
     * 
     * 通常通过REGISTER_SDK_DRIVER宏自动调用，无需手动注册。
     */
    void registerDriver(const std::string& driverName, std::unique_ptr<ISdkDriver> driver);

    /**
     * @brief 获取驱动支持的所有命令及说明
     * @param driverName 驱动名称
     * @return 命令信息列表
     */
    std::vector<SdkCommandInfo> listCommands(const std::string& driverName);

    /**
     * @brief 通过日志输出驱动支持的所有命令及说明
     * @param driverName 驱动名称
     */
    void logCommands(const std::string& driverName);

    /**
     * @brief 获取驱动的能力描述
     * @param driverName 驱动名称
     * @return 驱动能力描述
     */
    SdkDeviceCapabilities capabilities(const std::string& driverName) const;

    // ==================== 设备操作接口（传统方式） ====================

    /**
     * @brief 通过驱动名和设备句柄执行命令
     * @param driverName 驱动名称
     * @param device 设备句柄
     * @param command 要执行的命令
     * @return 执行结果
     * 
     * 传统调用方式，需要手动管理驱动名和句柄。
     * 推荐使用callById()简化调用。
     */
    SdkResult call(const std::string& driverName,
                   SdkDeviceHandle device,
                   const SdkCommand& command);

    /**
     * @brief 通过驱动名打开设备
     * @param driverName 驱动名称
     * @param openParam 打开参数
     * @return 执行结果，成功时payload包含设备句柄
     * 
     * 传统调用方式，需要手动管理句柄。
     * 推荐使用openAndRegister()自动注册到注册表。
     */
    SdkResult open(const std::string& driverName, const std::any& openParam);

    /**
     * @brief 通过驱动名关闭设备
     * @param driverName 驱动名称
     * @param device 设备句柄
     * @return 执行结果
     * 
     * 传统调用方式，需要手动管理句柄。
     * 推荐使用closeById()自动清理注册表。
     */
    SdkResult close(const std::string& driverName, SdkDeviceHandle device);

    // ==================== 设备注册表管理接口 ====================

    /**
     * @brief 手动注册设备到注册表
     * @param driverName 驱动名称
     * @param deviceId 设备唯一标识
     * @param handle 设备句柄
     * @param description 设备描述（可选）
     * @param metadata 设备元数据（可选）
     * @return 执行结果
     * 
     * 将设备信息注册到管理器，便于后续通过deviceId访问。
     * 推荐使用openAndRegister()一步完成打开和注册。
     */
    SdkResult registerDevice(const std::string& driverName,
                             const std::string& deviceId,
                             SdkDeviceHandle handle,
                             const std::string& description = "",
                             const std::any& metadata = {});

    /**
     * @brief 从注册表取消注册设备（不关闭设备）
     * @param deviceId 设备唯一标识
     * @return 执行结果
     */
    SdkResult unregisterDevice(const std::string& deviceId);

    /**
     * @brief 更新设备状态
     * @param deviceId 设备唯一标识
     * @param state 新状态
     * @return 执行结果
     */
    SdkResult updateDeviceState(const std::string& deviceId, SdkDeviceState state);

    /**
     * @brief 根据设备ID获取设备信息
     * @param deviceId 设备唯一标识
     * @return 设备信息，如果不存在则返回默认值
     */
    SdkDeviceInfo getDevice(const std::string& deviceId) const;

    /**
     * @brief 列出所有已注册的设备
     * @param driverName 驱动名称过滤（可选，为空则返回所有设备）
     * @return 设备信息列表
     */
    std::vector<SdkDeviceInfo> listDevices(const std::string& driverName = "") const;

    /**
     * @brief 扫描可用设备（如果驱动支持）
     * @param driverName 驱动名称
     * @param devices 输出参数，扫描到的设备列表
     * @return 执行结果
     */
    SdkResult scanDevices(const std::string& driverName, std::vector<SdkDeviceInfo>& devices);

    // ==================== 便捷接口（推荐使用，类似INDI） ====================

    /**
     * @brief 通过设备ID调用命令（推荐使用，类似INDI的调用方式）
     * @param deviceId 设备唯一标识（必须在注册表中）
     * @param command 要执行的命令
     * @return 执行结果
     * 
     * 从注册表中查找设备，然后调用对应的驱动。
     * 简化调用：不需要手动传递驱动名和句柄。
     * 
     * 使用示例：
     * @code
     * SdkCommand cmd{SdkCommandType::Custom, "GetTemperature", {}};
     * SdkResult result = SdkManager::instance().callById("MainCamera", cmd);
     * @endcode
     */
    SdkResult callById(const std::string& deviceId, const SdkCommand& command);

    /**
     * @brief 通过设备ID关闭设备并从注册表移除（推荐使用）
     * @param deviceId 设备唯一标识
     * @return 执行结果
     * 
     * 一步完成关闭和清理，类似INDI的调用方式。
     */
    SdkResult closeById(const std::string& deviceId);

    /**
     * @brief 通过设备句柄调用命令（类似INDI的调用方式）
     * @param handle 设备句柄
     * @param command 要执行的命令
     * @return 执行结果
     * 
     * 自动查找设备句柄对应的驱动并执行命令，无需手动指定驱动名称。
     * 类似INDI的调用方式，直接传入设备指针即可。
     * 
     * 使用示例：
     * @code
     * SdkCommand cmd{SdkCommandType::Custom, "GetTemperature", {}};
     * SdkResult result = SdkManager::instance().callByHandle(sdkMainCameraHandle, cmd);
     * @endcode
     */
    SdkResult callByHandle(SdkDeviceHandle handle, const SdkCommand& command);

    /**
     * @brief 通过设备句柄关闭设备（类似INDI的调用方式）
     * @param handle 设备句柄
     * @return 执行结果
     * 
     * 自动查找设备句柄对应的驱动并关闭设备，无需手动指定驱动名称。
     * 注意：此方法不会从注册表移除设备，如需清理注册表请使用closeById()。
     * 
     * 使用示例：
     * @code
     * SdkResult result = SdkManager::instance().closeByHandle(sdkMainCameraHandle);
     * @endcode
     */
    SdkResult closeByHandle(SdkDeviceHandle handle);

    /**
     * @brief 打开设备并自动注册到注册表（推荐使用，类似INDI的调用方式）
     * @param driverName 驱动名称
     * @param openParam 打开参数
     * @param deviceId 设备唯一标识（用于注册）
     * @param description 设备描述（可选）
     * @param metadata 设备元数据（可选）
     * @return 执行结果
     * 
     * 一步完成打开设备和注册，简化调用流程。
     * 类似INDI的调用方式：openAndRegister("QHYCCD", param, "MainCamera")
     * 
     * 使用示例：
     * @code
     * SdkResult result = SdkManager::instance().openAndRegister(
     *     "QHYCCD", 
     *     std::string("相机ID"), 
     *     "MainCamera",
     *     "主相机"
     * );
     * @endcode
     */
    SdkResult openAndRegister(const std::string& driverName,
                              const std::any& openParam,
                              const std::string& deviceId,
                              const std::string& description = "",
                              const std::any& metadata = {});

    // ==================== 日志管理接口 ====================

    /**
     * @brief 设置自定义日志器
     * @param logger 日志器指针（可以为nullptr，使用默认日志系统）
     * 
     * 如果不设置，框架会使用默认的Logger系统。
     */
    void setLogger(ISdkLogger* logger);

    /**
     * @brief 获取当前日志器
     * @return 日志器指针（可能为nullptr）
     */
    ISdkLogger* logger() const;
    

    // ==================== 驱动查询接口（用于 SdkDriverRegistry）====================

    /**
     * @brief 列出所有已注册的驱动名称（包括别名）
     * @return 驱动名称列表
     */
    std::vector<std::string> listRegisteredDrivers() const;

    /**
     * @brief 获取驱动的所有别名
     * @param driverName 驱动名称（任意别名）
     * @return 别名列表，如果驱动不存在则返回空
     */
    std::vector<std::string> getDriverAliases(const std::string& driverName) const;

    /**
     * @brief 根据设备类型查找驱动名称
     * @param deviceType 设备类型（相机、赤道仪、电调等）
     * @return 匹配的驱动名称列表（可能有多个驱动支持同一设备类型）
     * 
     * 遍历所有已注册的驱动，返回设备类型匹配的驱动名称。
     * 如果找不到匹配的驱动，返回空列表。
     * 
     * 使用示例：
     * @code
     * // 查找所有电调驱动
     * auto focuserDrivers = SdkManager::instance().findDriversByType(SdkDeviceType::Focuser);
     * if (!focuserDrivers.empty()) {
     *     std::string driverName = focuserDrivers[0];  // 使用第一个匹配的驱动
     * }
     * 
     * // 查找所有相机驱动
     * auto cameraDrivers = SdkManager::instance().findDriversByType(SdkDeviceType::Camera);
     * @endcode
     */
    std::vector<std::string> findDriversByType(SdkDeviceType deviceType) const;

private:
    // 禁止复制和赋值（单例模式）
    SdkManager() = default;
    SdkManager(const SdkManager&) = delete;
    SdkManager& operator=(const SdkManager&) = delete;

    // ==================== 内部数据成员 ====================

    /**
     * @brief 驱动名称到驱动实例的映射表
     * 
     * 键：驱动名称（包括主名称和别名）
     * 值：驱动实例指针（实际内存由m_driverStorage持有）
     */
    std::unordered_map<std::string, ISdkDriver*> m_drivers;

    /**
     * @brief 驱动对象存储容器
     * 
     * 持有所有驱动对象的唯一所有权，确保驱动对象在管理器生命周期内有效。
     */
    std::vector<std::unique_ptr<ISdkDriver>> m_driverStorage;

    /**
     * @brief 自定义日志器指针
     * 
     * 如果为nullptr，使用默认的Logger系统。
     */
    ISdkLogger* m_logger{nullptr};

    /**
     * @brief 互斥锁，保护所有共享数据
     * 
     * 使用mutable修饰，允许在const方法中加锁（如getDevice、listDevices）。
     */
    mutable std::mutex m_mutex;

    /**
     * @brief 设备注册表
     * 
     * 键：设备唯一标识（deviceId）
     * 值：设备信息（包含驱动名、句柄、状态等）
     * 
     * 用于统一管理所有已打开的设备，支持通过deviceId快速查找和操作设备。
     */
    std::unordered_map<std::string, SdkDeviceInfo> m_devices;
};

/**
 * @def REGISTER_SDK_DRIVER
 * @brief SDK驱动自动注册宏
 * 
 * 在每个驱动实现文件的底部使用此宏，驱动会在程序启动时自动注册到SdkManager。
 * 
 * 工作原理：
 * - 创建一个静态注册器对象
 * - 在静态初始化阶段自动调用SdkManager::registerDriver()
 * - 无需手动初始化，编译时自动完成注册
 * 
 * 使用示例：
 * @code
 * // 在驱动源文件（如QhyCameraDriver.cpp）的底部
 * REGISTER_SDK_DRIVER("QHYCCD", QhyCameraDriver);
 * 
 * // 也可以注册多个别名
 * REGISTER_SDK_DRIVER("QHYCCD", QhyCameraDriver);
 * REGISTER_SDK_DRIVER("QHY", QhyCameraDriver);  // 别名
 * @endcode
 * 
 * @param DRIVER_NAME 驱动名称（字符串字面量）
 * @param DRIVER_CLASS 驱动类名（必须实现ISdkDriver接口）
 */
#define REGISTER_SDK_DRIVER(DRIVER_NAME, DRIVER_CLASS)             \
    namespace {                                                    \
    struct DRIVER_CLASS##Registrar {                               \
        DRIVER_CLASS##Registrar() {                                \
            SdkManager::instance().registerDriver(                \
                DRIVER_NAME, std::unique_ptr<ISdkDriver>(         \
                                new DRIVER_CLASS()));             \
        }                                                          \
    };                                                             \
    static DRIVER_CLASS##Registrar global_##DRIVER_CLASS##Registrar; \
    }

#endif // SDKMANAGER_H


