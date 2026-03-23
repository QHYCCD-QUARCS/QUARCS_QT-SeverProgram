#ifndef SDK_DRIVER_REGISTRY_H
#define SDK_DRIVER_REGISTRY_H

#include <string>
#include <unordered_map>
#include <vector>

/**
 * @class SdkDriverRegistry
 * @brief SDK 驱动注册表：管理 INDI 驱动名到 SDK 驱动名的映射
 * 
 * 功能：
 * 1. 自动构建 INDI 驱动名 -> SDK 首选名称的映射关系
 * 2. 查询驱动是否支持 SDK 模式
 * 3. 获取驱动的推荐 SDK 实现
 * 
 * 使用场景：
 * - 用户选择 INDI 驱动时，自动判断是否支持 SDK 模式
 * - SDK 模式连接时，自动选择正确的 SDK 驱动实现
 * 
 * 使用示例：
 * @code
 * // 初始化（在 main() 或 MainWindow 构造函数中调用一次）
 * SdkDriverRegistry::instance().initialize();
 * 
 * // 查询 INDI 驱动是否支持 SDK
 * bool supports = SdkDriverRegistry::instance().supportsSDK("indi_qhy_ccd");
 * 
 * // 获取对应的 SDK 驱动名
 * std::string sdkName = SdkDriverRegistry::instance().getSDKDriverName("indi_qhy_ccd");
 * // sdkName = "QHYCCD"
 * @endcode
 */
class SdkDriverRegistry {
public:
    /**
     * @brief 获取单例实例
     */
    static SdkDriverRegistry& instance();

    /**
     * @brief 初始化驱动映射表（从 SdkManager 查询已注册的驱动）
     * 
     * 应在 SdkManager 所有驱动注册完成后调用（通常在 main() 或 MainWindow 构造函数中）
     * 
     * 工作流程：
     * 1. 遍历预定义的 INDI 驱动名列表
     * 2. 通过 SdkManager::capabilities() 检查驱动是否已注册
     * 3. 建立 INDI 驱动名 -> SDK 首选名称的映射
     */
    void initialize();

    /**
     * @brief 根据 INDI 驱动名获取对应的 SDK 首选名称
     * @param indiDriverName INDI 驱动名（如 "indi_qhy_ccd"）
     * @return SDK 首选名称（如 "QHYCCD"），如果不支持 SDK 则返回空字符串
     * 
     * 示例：
     * @code
     * std::string sdkName = SdkDriverRegistry::instance().getSDKDriverName("indi_qhy_ccd");
     * // sdkName = "QHYCCD"
     * 
     * sdkName = SdkDriverRegistry::instance().getSDKDriverName("indi_asi_ccd");
     * // sdkName = "ASI"
     * @endcode
     */
    std::string getSDKDriverName(const std::string& indiDriverName) const;

    /**
     * @brief 判断 INDI 驱动是否支持 SDK 模式
     * @param indiDriverName INDI 驱动名
     * @return 是否支持 SDK 模式
     * 
     * 示例：
     * @code
     * if (SdkDriverRegistry::instance().supportsSDK("indi_qhy_ccd")) {
     *     // 该驱动支持 SDK 模式，可以显示"连接模式"切换选项
     * }
     * @endcode
     */
    bool supportsSDK(const std::string& indiDriverName) const;

    /**
     * @brief 获取所有支持 SDK 的 INDI 驱动名列表
     * @return INDI 驱动名列表
     */
    std::vector<std::string> getAllSDKSupportedINDIDrivers() const;

    /**
     * @brief 获取指定 SDK 驱动支持的所有别名
     * @param sdkDriverName SDK 驱动名（如 "QHYCCD"）
     * @return 别名列表（包括 INDI 驱动名）
     * 
     * 示例：
     * @code
     * auto aliases = SdkDriverRegistry::instance().getDriverAliases("QHYCCD");
     * // aliases = {"indi_qhy_ccd", "indi_qhy_ccd2", "libqhyccd"}
     * @endcode
     */
    std::vector<std::string> getDriverAliases(const std::string& sdkDriverName) const;

    /**
     * @brief 手动注册 INDI 驱动名到 SDK 驱动名的映射
     * @param indiDriverName INDI 驱动名
     * @param sdkDriverName SDK 首选名称
     * 
     * 用于自定义映射规则（通常不需要手动调用，initialize() 会自动处理）
     */
    void registerMapping(const std::string& indiDriverName, const std::string& sdkDriverName);

private:
    SdkDriverRegistry() = default;
    SdkDriverRegistry(const SdkDriverRegistry&) = delete;
    SdkDriverRegistry& operator=(const SdkDriverRegistry&) = delete;

    /**
     * @brief 从驱动名获取首选名称
     * 
     * 直接使用原始名称作为首选名称（不做推断）
     * 例如："indi_qhy_ccd" -> "indi_qhy_ccd"
     */
    std::string inferPreferredName(const std::string& indiName) const;

    /**
     * @brief INDI 驱动名 -> SDK 首选名称
     */
    std::unordered_map<std::string, std::string> m_indiToSdkMap;

    /**
     * @brief SDK 首选名称 -> 所有别名列表
     */
    std::unordered_map<std::string, std::vector<std::string>> m_sdkAliases;
};

#endif // SDK_DRIVER_REGISTRY_H

