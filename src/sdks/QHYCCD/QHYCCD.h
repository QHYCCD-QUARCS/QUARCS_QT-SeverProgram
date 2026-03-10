#ifndef QHY_CAMERA_DRIVER_H
#define QHY_CAMERA_DRIVER_H

#include "../SdkDriver.h"
#include "../SdkCommon.h"  // 使用统一的 SDK 类型定义
#include <qhyccd.h> // QHYCCD SDK 头文件
#include <vector>
#include <string>
#include <cstdint>

// 对外可用的驱动名（两种别名）
#define SDK_DRIVER_NAME_INDI_QHY_CCD "indi_qhy_ccd"
#define SDK_DRIVER_NAME_QHYCCD       "QHYCCD"

// 注意：所有类型定义（SdkFrameData, SdkAreaInfo, SdkChipInfo 等）已在 SdkCommon.h 中统一定义

// QHY 相机 SDK 驱动
class QhyCameraDriver : public ISdkDriver {
public:
    QhyCameraDriver();
    ~QhyCameraDriver() override;

    std::vector<std::string> driverNames() const override;
    std::vector<SdkCommandInfo> commandList() const override;

    // openDevice：openParam 期望为 std::string（cameraId）
    SdkResult openDevice(const std::any& openParam) override;
    // closeDevice：handle 为 SdkDeviceHandle（实际为 qhyccd_handle*）
    SdkResult closeDevice(SdkDeviceHandle handle) override;
    // execute：所有自定义命令的统一入口
    SdkResult execute(SdkDeviceHandle handle, const SdkCommand& cmd) override;

    // 扫描可用设备（实现ISdkDriver的可选接口）
    SdkResult scanDevices(std::vector<SdkDeviceInfo>& outDevices) override;
    // 获取驱动能力描述
    SdkDeviceCapabilities capabilities() const override;

private:
    // SDK 全局资源是否已初始化
    bool m_resourceInited{false};

    // 工具函数：从 std::any 中安全取出参数
    template<typename T>
    bool extractParam(const std::any& anyValue, T& out, SdkResult& r, const char* name) const
    {
        try {
            out = std::any_cast<T>(anyValue);
            return true;
        } catch (const std::bad_any_cast&) {
            r.success = false;
            r.message = std::string("Invalid parameter type for ") + name;
            return false;
        }
    }

    // 内部辅助：从 QHYCCD 错误码构造结果
    SdkResult makeResultFromRet(const std::string& action, unsigned int ret) const;
};

#endif // QHY_CAMERA_DRIVER_H



