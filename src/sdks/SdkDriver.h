#ifndef SDKDRIVER_H
#define SDKDRIVER_H

#include "SdkCommon.h"
#include <string>

// 单个 SDK 驱动需要实现的通用接口
class ISdkDriver {
public:
    virtual ~ISdkDriver() = default;

    // 返回该驱动支持的一组驱动名（字符串），用于匹配宏定义的驱动名
    virtual std::vector<std::string> driverNames() const = 0;

    // 返回该驱动支持的所有命令及其说明，用于通用帮助/打印
    virtual std::vector<SdkCommandInfo> commandList() const = 0;

    // 打开/初始化设备（如果需要），返回设备句柄
    virtual SdkResult openDevice(const std::any& openParam) = 0;

    // 关闭设备，句柄由调用方传入
    virtual SdkResult closeDevice(SdkDeviceHandle handle) = 0;

    // 执行具体命令：设备句柄 + 命令 + 结果
    virtual SdkResult execute(SdkDeviceHandle handle, const SdkCommand& cmd) = 0;

    // （可选）扫描设备：默认不实现，返回 NotImplemented 方便调用层判断
    virtual SdkResult scanDevices(std::vector<SdkDeviceInfo>& outDevices) {
        SdkResult r;
        r.success = false;
        r.errorCode = SdkErrorCode::NotImplemented;
        r.message = "scanDevices not implemented";
        return r;
    }

    // （可选）返回设备能力描述，默认空实现
    virtual SdkDeviceCapabilities capabilities() const {
        return {};
    }
};

#endif // SDKDRIVER_H


