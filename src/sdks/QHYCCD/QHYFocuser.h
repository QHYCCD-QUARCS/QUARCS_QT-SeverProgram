#ifndef QHY_FOCUSER_DRIVER_H
#define QHY_FOCUSER_DRIVER_H

#include "../SdkDriver.h"
#include "../SdkCommon.h"  // 使用统一的 SDK 类型定义
#include "../SdkSerialExecutor.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <string>
#include <vector>
#include <mutex>

// 对外可用的驱动名（独立于相机，避免与 QHYCCD 宏冲突）
#define SDK_DRIVER_NAME_INDI_QHY_FOCUSER "indi_qhy_focuser"
#define SDK_DRIVER_NAME_QHY_FOCUSER      "QFocuser"

// 使用统一的 SDK 类型（向后兼容：保留旧类型名作为别名）
using QhyFocuserOpenParam = SdkFocuserOpenParam;

// 注意：SdkFocuserVersion 已在 SdkCommon.h 中统一定义

// 温度/电压（来自 cmd_id=4 回包）
struct QhyFocuserTelemetry
{
    double outTempC{0.0};   // o_t / 1000.0
    double chipTempC{0.0};  // c_t / 1000.0
    double voltageV{0.0};   // c_r / 10.0
};

// 相对移动参数（cmd_id=2）
struct QhyFocuserRelMoveParam
{
    bool outward{false}; // outward=true 表示“向外”（与 indi driver 的 dir_param=true 一致）
    int  steps{0};
};

class QhyFocuserDriver : public ISdkDriver
{
public:
    QhyFocuserDriver();
    ~QhyFocuserDriver() override;

    std::vector<std::string> driverNames() const override;
    std::vector<SdkCommandInfo> commandList() const override;
    SdkDeviceCapabilities capabilities() const override;

    // openDevice：openParam 期望为 SdkFocuserOpenParam（或 QhyFocuserOpenParam，向后兼容别名）
    SdkResult openDevice(const std::any &openParam) override;
    SdkResult closeDevice(SdkDeviceHandle handle) override;
    SdkResult execute(SdkDeviceHandle handle, const SdkCommand &cmd) override;

private:
    // 确保所有串口 I/O 都在同一线程串行执行，避免 QSerialPort 跨线程导致的崩溃
    SdkSerialExecutor m_exec{QStringLiteral("QhyFocuserSerialExecutor")};

    template <typename T>
    bool extractParam(const std::any &anyValue, T &out, SdkResult &r, const char *name) const
    {
        try
        {
            out = std::any_cast<T>(anyValue);
            return true;
        }
        catch (const std::bad_any_cast &)
        {
            r.success = false;
            r.message = std::string("Invalid parameter type for ") + name;
            return false;
        }
    }
};

#endif // QHY_FOCUSER_DRIVER_H



