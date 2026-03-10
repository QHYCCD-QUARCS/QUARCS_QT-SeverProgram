#include "SdkDevice.h"
#include "../Logger.h"

/**
 * @brief 默认构造函数：创建未打开的设备
 */
SdkDevice::SdkDevice()
    : m_deviceId()
    , m_ownsDevice(false)
{
}

/**
 * @brief 从设备ID构造：从注册表获取设备
 * @param deviceId 设备唯一标识
 */
SdkDevice::SdkDevice(const std::string& deviceId)
    : m_deviceId(deviceId)
    , m_ownsDevice(false)  // 从注册表获取，不拥有设备
{
    // 检查设备是否存在且已打开
    SdkDeviceInfo info = SdkManager::instance().getDevice(deviceId);
    if (info.state != SdkDeviceState::Open) {
        // 设备不存在或未打开，清空deviceId
        m_deviceId.clear();
    }
}

/**
 * @brief 打开并注册设备构造：自动打开设备
 * @param driverName 驱动名称
 * @param openParam 打开参数
 * @param deviceId 设备唯一标识
 * @param description 设备描述
 */
SdkDevice::SdkDevice(const std::string& driverName,
                     const std::any& openParam,
                     const std::string& deviceId,
                     const std::string& description)
    : m_deviceId(deviceId)
    , m_ownsDevice(false)  // 先设为false，open成功后再设为true
{
    open(driverName, openParam, deviceId, description);
}

/**
 * @brief 析构函数：自动关闭设备
 */
SdkDevice::~SdkDevice()
{
    close();
}

/**
 * @brief 移动构造函数
 * @param other 源对象
 */
SdkDevice::SdkDevice(SdkDevice&& other) noexcept
    : m_deviceId(std::move(other.m_deviceId))
    , m_ownsDevice(other.m_ownsDevice)
{
    // 转移所有权后，源对象不再拥有设备
    other.m_ownsDevice = false;
    other.m_deviceId.clear();
}

/**
 * @brief 移动赋值运算符
 * @param other 源对象
 * @return 自身引用
 */
SdkDevice& SdkDevice::operator=(SdkDevice&& other) noexcept
{
    if (this != &other) {
        // 先关闭当前设备
        close();

        // 转移所有权
        m_deviceId = std::move(other.m_deviceId);
        m_ownsDevice = other.m_ownsDevice;

        // 源对象不再拥有设备
        other.m_ownsDevice = false;
        other.m_deviceId.clear();
    }
    return *this;
}

/**
 * @brief 打开并注册设备
 * @param driverName 驱动名称
 * @param openParam 打开参数
 * @param deviceId 设备唯一标识
 * @param description 设备描述
 * @return 是否成功打开
 */
bool SdkDevice::open(const std::string& driverName,
                     const std::any& openParam,
                     const std::string& deviceId,
                     const std::string& description)
{
    // 如果设备已打开，先关闭
    if (m_ownsDevice && !m_deviceId.empty()) {
        close();
    }

    // 打开并注册设备
    SdkResult result = SdkManager::instance().openAndRegister(
        driverName, openParam, deviceId, description);

    if (result.success) {
        m_deviceId = deviceId;
        m_ownsDevice = true;  // 标记为拥有设备
        return true;
    } else {
        m_deviceId.clear();
        m_ownsDevice = false;
        return false;
    }
}

/**
 * @brief 从注册表打开设备
 * @param deviceId 设备唯一标识
 * @return 是否成功打开
 */
bool SdkDevice::open(const std::string& deviceId)
{
    // 如果设备已打开，先关闭
    if (m_ownsDevice && !m_deviceId.empty()) {
        close();
    }

    // 从注册表获取设备信息
    SdkDeviceInfo info = SdkManager::instance().getDevice(deviceId);
    if (info.state == SdkDeviceState::Open) {
        m_deviceId = deviceId;
        m_ownsDevice = false;  // 从注册表获取，不拥有设备
        return true;
    } else {
        m_deviceId.clear();
        m_ownsDevice = false;
        return false;
    }
}

/**
 * @brief 关闭设备
 * @return 是否成功关闭
 */
bool SdkDevice::close()
{
    if (m_deviceId.empty()) {
        return true;  // 设备未打开，返回成功（幂等操作）
    }

    // 只有拥有设备时才关闭（从注册表获取的设备不关闭）
    if (m_ownsDevice) {
        SdkResult result = SdkManager::instance().closeById(m_deviceId);
        m_ownsDevice = false;
        m_deviceId.clear();
        return result.success;
    } else {
        // 不拥有设备，只清空引用
        m_deviceId.clear();
        return true;
    }
}

/**
 * @brief 检查设备是否已打开
 * @return 设备是否已打开
 */
bool SdkDevice::isOpen() const
{
    if (m_deviceId.empty()) {
        return false;
    }

    // 检查设备在注册表中的状态
    SdkDeviceInfo info = SdkManager::instance().getDevice(m_deviceId);
    return info.state == SdkDeviceState::Open;
}

/**
 * @brief 获取设备信息
 * @return 设备信息
 */
SdkDeviceInfo SdkDevice::getInfo() const
{
    if (m_deviceId.empty()) {
        SdkDeviceInfo empty;
        return empty;
    }

    return SdkManager::instance().getDevice(m_deviceId);
}

/**
 * @brief 调用设备命令
 * @param command 要执行的命令
 * @return 执行结果
 */
SdkResult SdkDevice::call(const SdkCommand& command) const
{
    if (m_deviceId.empty()) {
        SdkResult r;
        r.success = false;
        r.errorCode = SdkErrorCode::DeviceNotFound;
        r.message = "设备未打开";
        return r;
    }

    return SdkManager::instance().callById(m_deviceId, command);
}

/**
 * @brief 更新设备状态
 * @param state 新状态
 * @return 是否成功更新
 */
bool SdkDevice::updateState(SdkDeviceState state)
{
    if (m_deviceId.empty()) {
        return false;
    }

    SdkResult result = SdkManager::instance().updateDeviceState(m_deviceId, state);
    return result.success;
}

