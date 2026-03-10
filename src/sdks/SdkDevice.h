#ifndef SDKDEVICE_H
#define SDKDEVICE_H

#include "SdkCommon.h"
#include "SdkManager.h"
#include <string>
#include <memory>

/**
 * @class SdkDevice
 * @brief RAII风格的设备包装类，自动管理设备生命周期
 * 
 * SdkDevice提供了类似INDI的简单调用方式，自动管理设备的打开和关闭。
 * 使用RAII（Resource Acquisition Is Initialization）模式，确保设备在使用
 * 完毕后自动关闭，避免资源泄漏。
 * 
 * 主要特性：
 * - 自动管理设备生命周期（构造时打开，析构时关闭）
 * - 支持移动语义，可以安全地转移所有权
 * - 提供简单的调用接口，无需手动管理句柄
 * - 线程安全：所有操作都通过SdkManager，受互斥锁保护
 * 
 * 使用示例：
 * @code
 * // 方式1：直接构造（自动打开并注册）
 * {
 *     SdkDevice camera("QHYCCD", std::string("相机ID"), "MainCamera");
 *     if (camera.isOpen()) {
 *         // 使用设备
 *         SdkCommand cmd{SdkCommandType::Custom, "GetTemperature", {}};
 *         SdkResult result = camera.call(cmd);
 *         if (result.success) {
 *             double temp = std::any_cast<double>(result.payload);
 *         }
 *     }
 *     // 离开作用域时自动关闭设备
 * }
 * 
 * // 方式2：使用openAndRegister的结果构造
 * SdkResult openResult = SdkManager::instance().openAndRegister(
 *     "QHYCCD", std::string("相机ID"), "MainCamera");
 * if (openResult.success) {
 *     SdkDevice camera("MainCamera");  // 从注册表获取
 *     // 使用设备...
 * }
 * 
 * // 方式3：手动管理（不推荐，但支持）
 * SdkDevice camera;
 * if (camera.open("QHYCCD", std::string("相机ID"), "MainCamera")) {
 *     // 使用设备...
 *     camera.close();  // 手动关闭（析构时也会自动关闭）
 * }
 * @endcode
 */
class SdkDevice {
public:
    /**
     * @brief 默认构造函数（创建未打开的设备）
     * 
     * 创建的设备处于未打开状态，需要调用open()方法打开。
     */
    SdkDevice();

    /**
     * @brief 从设备ID构造（设备必须已在注册表中）
     * @param deviceId 设备唯一标识
     * 
     * 从SdkManager的注册表中查找设备，如果设备不存在或未打开，isOpen()返回false。
     * 适用于设备已通过openAndRegister()打开的情况。
     */
    explicit SdkDevice(const std::string& deviceId);

    /**
     * @brief 打开并注册设备（推荐使用）
     * @param driverName 驱动名称
     * @param openParam 打开参数
     * @param deviceId 设备唯一标识
     * @param description 设备描述（可选）
     * 
     * 构造时自动打开设备并注册到SdkManager，如果打开失败，isOpen()返回false。
     */
    SdkDevice(const std::string& driverName,
              const std::any& openParam,
              const std::string& deviceId,
              const std::string& description = "");

    /**
     * @brief 析构函数：自动关闭设备
     * 
     * 如果设备已打开，自动调用close()关闭设备并从注册表移除。
     */
    ~SdkDevice();

    // 禁止复制（避免重复关闭）
    SdkDevice(const SdkDevice&) = delete;
    SdkDevice& operator=(const SdkDevice&) = delete;

    // 支持移动语义
    SdkDevice(SdkDevice&& other) noexcept;
    SdkDevice& operator=(SdkDevice&& other) noexcept;

    /**
     * @brief 打开并注册设备
     * @param driverName 驱动名称
     * @param openParam 打开参数
     * @param deviceId 设备唯一标识
     * @param description 设备描述（可选）
     * @return 是否成功打开
     * 
     * 如果设备已打开，先关闭旧设备再打开新设备。
     */
    bool open(const std::string& driverName,
              const std::any& openParam,
              const std::string& deviceId,
              const std::string& description = "");

    /**
     * @brief 从注册表打开设备
     * @param deviceId 设备唯一标识
     * @return 是否成功打开
     * 
     * 从SdkManager的注册表中查找设备，如果设备不存在或未打开，返回false。
     */
    bool open(const std::string& deviceId);

    /**
     * @brief 关闭设备
     * @return 是否成功关闭
     * 
     * 关闭设备并从注册表移除。如果设备未打开，返回true（幂等操作）。
     */
    bool close();

    /**
     * @brief 检查设备是否已打开
     * @return 设备是否已打开
     */
    bool isOpen() const;

    /**
     * @brief 获取设备ID
     * @return 设备唯一标识
     */
    std::string deviceId() const { return m_deviceId; }

    /**
     * @brief 获取设备信息
     * @return 设备信息（如果设备不存在，返回默认值）
     */
    SdkDeviceInfo getInfo() const;

    /**
     * @brief 调用设备命令（推荐使用）
     * @param command 要执行的命令
     * @return 执行结果
     * 
     * 如果设备未打开，返回错误结果。
     * 
     * 使用示例：
     * @code
     * SdkCommand cmd{SdkCommandType::Custom, "GetTemperature", {}};
     * SdkResult result = camera.call(cmd);
     * if (result.success) {
     *     double temp = std::any_cast<double>(result.payload);
     * }
     * @endcode
     */
    SdkResult call(const SdkCommand& command) const;

    /**
     * @brief 更新设备状态
     * @param state 新状态
     * @return 是否成功更新
     */
    bool updateState(SdkDeviceState state);

private:
    std::string m_deviceId;  ///< 设备唯一标识
    bool m_ownsDevice;       ///< 是否拥有设备（是否需要关闭）
};

#endif // SDKDEVICE_H

