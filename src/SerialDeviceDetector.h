#pragma once
#include <QObject>
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <functional>

struct SerialMatchRule {
    // /dev/serial/by-id/ 文件名的关键词（不区分大小写）
    QStringList nameKeywords;
    // 设备的 VID:PID（小写16进制，冒号分隔），如 {"1a86:7523","0403:6001"}
    QStringList vidpidList;
};

struct DevicePorts {
    QString mountPort;   // 解析后的真实串口路径（如 /dev/ttyUSB0）
    QString focuserPort; // 解析后的真实串口路径（如 /dev/ttyACM1）
};

class SerialDeviceDetector : public QObject {
    Q_OBJECT
public:
    explicit SerialDeviceDetector(QObject* parent=nullptr);

    // 配置匹配规则（可覆盖默认规则）
    void setMountRule(const SerialMatchRule& rule);
    void setFocuserRule(const SerialMatchRule& rule);

    // 可选：设置握手校验器（返回 true 表示该端口确认为对应类型）
    // 传入参数：realPort=/dev/ttyXXX, deviceType="Mount" 或 "Focuser"
    void setHandshakeValidator(std::function<bool(const QString& realPort, const QString& deviceType)> validator);

    // 重新扫描，返回结果（无匹配则返回空字符串，不瞎返回）
    DevicePorts rescan();

    // 读取（内部有缓存；若为空将触发一次扫描）
    QString getMountPort();
    QString getFocuserPort();

    // 判断传入端口类型：返回 "Mount" / "Focuser" / 空字符串（未知）
    QString detectDeviceTypeForPort(const QString& portPath) const;

    // 可选：外部注入日志
    void setLogger(std::function<void(const QString&)> logger);

private:
    // 工具函数
    QStringList listByIdEntries() const;
    QString resolveByIdToReal(const QString& byIdName) const;
    static bool nameContainsVidPid(const QString& name, const QString& vidpid);

    // 核心选择逻辑（不含任何兜底）
    QString pickByRule(const SerialMatchRule& rule, const QString& deviceType) const;

    // 成员
    DevicePorts cached_;
    SerialMatchRule mountRule_;
    SerialMatchRule focuserRule_;
    std::function<bool(const QString&, const QString&)> handshake_;
    std::function<void(const QString&)> log_;
};
