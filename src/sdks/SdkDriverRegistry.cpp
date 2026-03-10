#include "SdkDriverRegistry.h"
#include "SdkManager.h"
#include "../Logger.h"
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <sstream>

SdkDriverRegistry& SdkDriverRegistry::instance()
{
    static SdkDriverRegistry inst;
    return inst;
}

void SdkDriverRegistry::initialize()
{
    m_indiToSdkMap.clear();
    m_sdkAliases.clear();

    // 🔥 动态从 SdkManager 获取所有已注册的驱动及其别名
    // 这样无需硬编码映射表，驱动信息自动从头文件定义中获取
    
    // 获取所有已注册的驱动名称（包括别名）
    std::vector<std::string> allRegisteredNames = SdkManager::instance().listRegisteredDrivers();
    
    // 用于跟踪已处理的别名（避免重复处理同一驱动实例）
    std::unordered_set<std::string> processedAliases;
    
    // 用于按首选名称分组别名
    std::unordered_map<std::string, std::vector<std::string>> sdkDriverGroups;
    
    // 遍历所有已注册的驱动名称
    for (const std::string& driverName : allRegisteredNames)
    {
        // 检查是否已经处理过该别名
        if (processedAliases.find(driverName) != processedAliases.end())
        {
            continue; // 已经处理过，跳过
        }
        
        // 检查驱动是否存在
        auto caps = SdkManager::instance().capabilities(driverName);
        if (caps.supportedCommands.empty())
        {
            continue; // 驱动不存在或无效
        }
        
        // 获取该驱动的所有别名
        std::vector<std::string> aliases = SdkManager::instance().getDriverAliases(driverName);
        if (aliases.empty())
        {
            // 如果没有别名，使用驱动名称本身
            aliases = {driverName};
        }
        
        // 标记所有别名为已处理
        for (const std::string& alias : aliases)
        {
            processedAliases.insert(alias);
        }
        
        // 推断首选名称：优先选择 SDK 名称（不以 "indi_" 开头的名称）
        // 如果所有别名都以 "indi_" 开头，则使用第一个别名
        std::string preferredName;
        for (const std::string& alias : aliases)
        {
            if (alias.length() > 5 && alias.substr(0, 5) == "indi_")
            {
                // 跳过 INDI 名称，继续查找 SDK 名称
                continue;
            }
            // 找到第一个非 INDI 名称，作为首选名称
            preferredName = alias;
            break;
        }
        // 如果所有别名都是 INDI 名称，使用第一个别名
        if (preferredName.empty())
        {
            preferredName = aliases[0];
        }
        
        // 将当前驱动的所有别名添加到对应的组中（去重）
        auto& groupAliases = sdkDriverGroups[preferredName];
        for (const std::string& alias : aliases)
        {
            // 检查是否已存在，避免重复添加
            if (std::find(groupAliases.begin(), groupAliases.end(), alias) == groupAliases.end())
            {
                groupAliases.push_back(alias);
            }
        }
    }
    
    // 构建最终的映射表（使用小写键，实现大小写不敏感匹配）
    for (const auto& [sdkName, indiNames] : sdkDriverGroups)
    {
        // 将所有别名映射到首选名称（键转换为小写）
        for (const std::string& indiName : indiNames)
        {
            std::string key = indiName;
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            m_indiToSdkMap[key] = sdkName;
        }
        m_sdkAliases[sdkName] = indiNames;
    }
}

std::string SdkDriverRegistry::getSDKDriverName(const std::string& indiDriverName) const
{
    // 从映射表中查找（大小写不敏感）
    std::string key = indiDriverName;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    auto it = m_indiToSdkMap.find(key);
    if (it != m_indiToSdkMap.end())
    {
        return it->second;
    }
    
    // 未找到映射，尝试直接使用（可能本身就是 SDK 驱动名，大小写不敏感）
    // 获取所有已注册的驱动名称进行大小写不敏感匹配
    std::vector<std::string> registeredDrivers = SdkManager::instance().listRegisteredDrivers();
    for (const std::string& registeredName : registeredDrivers)
    {
        std::string registeredLower = registeredName;
        std::transform(registeredLower.begin(), registeredLower.end(), registeredLower.begin(), ::tolower);
        if (registeredLower == key)
        {
            auto caps = SdkManager::instance().capabilities(registeredName);
            if (!caps.supportedCommands.empty())
            {
                return registeredName;  // 返回原始大小写的名称
            }
        }
    }
    
    return "";  // 不支持 SDK
}

bool SdkDriverRegistry::supportsSDK(const std::string& indiDriverName) const
{
    return !getSDKDriverName(indiDriverName).empty();
}

std::vector<std::string> SdkDriverRegistry::getAllSDKSupportedINDIDrivers() const
{
    std::vector<std::string> result;
    result.reserve(m_indiToSdkMap.size());
    
    for (const auto& [indiName, _] : m_indiToSdkMap)
    {
        result.push_back(indiName);
    }
    
    return result;
}

std::vector<std::string> SdkDriverRegistry::getDriverAliases(const std::string& sdkDriverName) const
{
    auto it = m_sdkAliases.find(sdkDriverName);
    if (it != m_sdkAliases.end())
    {
        return it->second;
    }
    return {};
}

void SdkDriverRegistry::registerMapping(const std::string& indiDriverName, const std::string& sdkDriverName)
{
    m_indiToSdkMap[indiDriverName] = sdkDriverName;
    
    // 添加到别名列表
    auto& aliases = m_sdkAliases[sdkDriverName];
    if (std::find(aliases.begin(), aliases.end(), indiDriverName) == aliases.end())
    {
        aliases.push_back(indiDriverName);
    }
}

std::string SdkDriverRegistry::inferPreferredName(const std::string& indiName) const
{
    // 直接使用原始名称作为首选名称（不做推断）
    // 例如："indi_qhy_ccd" -> "indi_qhy_ccd"
    return indiName;
}

