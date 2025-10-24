#include "SerialDeviceDetector.h"
#include <QRegularExpression>
#include <QDebug>

static QString norm(QString s) {
    // 标准化比较用：全小写 + 用下划线替空格
    s = s.toLower();
    s.replace(' ', '_');
    return s;
}

SerialDeviceDetector::SerialDeviceDetector(QObject* parent)
    : QObject(parent)
{
    // 默认规则：Mount（CH340/FTDI/CP210x），Focuser（GD32/STM32 CDC）
    mountRule_.nameKeywords = QStringList() << "1a86" << "ch34" << "usb_serial" << "ftdi" << "cp210";
    mountRule_.vidpidList   = QStringList() << "1a86:7523" << "0403:6001" << "10c4:ea60";

    focuserRule_.nameKeywords = QStringList() << "gd32" << "gigadevice" << "cdc_acm" << "motor" << "focuser" << "stm32";
    focuserRule_.vidpidList   = QStringList() << "28e9:018a" << "0483:5740"; // 可按现场补充
}

void SerialDeviceDetector::setMountRule(const SerialMatchRule& rule)   { mountRule_ = rule; }
void SerialDeviceDetector::setFocuserRule(const SerialMatchRule& rule) { focuserRule_ = rule; }
void SerialDeviceDetector::setLogger(std::function<void(const QString&)> logger) { log_ = std::move(logger); }
void SerialDeviceDetector::setHandshakeValidator(std::function<bool(const QString&, const QString&)> validator) {
    handshake_ = std::move(validator);
}

QStringList SerialDeviceDetector::listByIdEntries() const {
    QDir dir("/dev/serial/by-id");
    if (!dir.exists()) return {};
    return dir.entryList(QDir::Files | QDir::System | QDir::Hidden);
}

QString SerialDeviceDetector::resolveByIdToReal(const QString& byIdName) const {
    const QString byIdPath = "/dev/serial/by-id/" + byIdName;
    QFileInfo fi(byIdPath);
    if (!fi.exists() || !fi.isSymLink()) return {};
    const QString target = QFileInfo(fi.symLinkTarget()).canonicalFilePath();
    return target;
}

bool SerialDeviceDetector::nameContainsVidPid(const QString& name, const QString& vidpid) {
    // 兼容 "1a86:7523" 与名称中出现的 "1a86_7523"/"ID_1a86_7523" 等写法
    const QString n = norm(name);
    const QString v = norm(vidpid);
    if (n.contains(v)) return true;
    QString v2 = v;
    return n.contains(v2.replace(":", "_"));
}

QString SerialDeviceDetector::pickByRule(const SerialMatchRule& rule, const QString& deviceType) const {
    const QStringList entries = listByIdEntries();
    if (entries.isEmpty()) {
        return {};
    }

    auto matchKeyword = [&](const QString& e)->bool {
        const QString le = e.toLower();
        for (const auto& k : rule.nameKeywords) {
            if (!k.isEmpty() && le.contains(k.toLower()))
                return true;
        }
        return false;
    };

    auto matchVidPid = [&](const QString& e)->bool {
        for (const auto& vp : rule.vidpidList) {
            if (nameContainsVidPid(e, vp))
                return true;
        }
        return false;
    };

    // 1) 先按关键词匹配
    for (const auto& e : entries) {
        if (matchKeyword(e)) {
            if (const auto real = resolveByIdToReal(e); !real.isEmpty()) {
                if (!handshake_ || handshake_(real, deviceType)) {
                    return real;
                }
            }
        }
    }

    // 2) 再按 VID:PID 匹配
    for (const auto& e : entries) {
        if (matchVidPid(e)) {
            if (const auto real = resolveByIdToReal(e); !real.isEmpty()) {
                if (!handshake_ || handshake_(real, deviceType)) {
                    return real;
                }
            }
        }
    }

    // **无兜底**：匹配不到或握手失败，返回空
    return {};
}

DevicePorts SerialDeviceDetector::rescan() {
    DevicePorts ret;
    ret.mountPort   = pickByRule(mountRule_,   "Mount");
    ret.focuserPort = pickByRule(focuserRule_, "Focuser");

    if (log_) {
        const auto entries = listByIdEntries();
        log_(QString("[Detector] by-id entries: %1").arg(entries.isEmpty() ? "<none>" : entries.join(", ")));
        log_(QString("[Detector] Mount   -> %1").arg(ret.mountPort.isEmpty()   ? "<none>" : ret.mountPort));
        log_(QString("[Detector] Focuser -> %1").arg(ret.focuserPort.isEmpty() ? "<none>" : ret.focuserPort));
    }

    cached_ = ret;
    return ret;
}

QString SerialDeviceDetector::getMountPort() {
    if (cached_.mountPort.isEmpty()) rescan();
    return cached_.mountPort;
}

QString SerialDeviceDetector::getFocuserPort() {
    if (cached_.focuserPort.isEmpty()) rescan();
    return cached_.focuserPort;
}

QString SerialDeviceDetector::detectDeviceTypeForPort(const QString& portPath) const {
    // 允许传入 /dev/ttyXXX 或者 /dev/serial/by-id/... 链接
    // 若是 by-id，尝试基于文件名进行匹配；若是 /dev/tty*，仅做简单名称启发判断
    if (portPath.isEmpty()) return {};

    auto matchByList = [&](const QString& name, const QStringList& keywords, const QStringList& vidpid)->bool {
        const QString lname = name.toLower();
        for (const auto& k : keywords) {
            if (!k.isEmpty() && lname.contains(k)) return true;
        }
        for (const auto& vp : vidpid) {
            if (nameContainsVidPid(lname, vp)) return true;
        }
        return false;
    };

    QFileInfo fi(portPath);
    QString baseName = fi.fileName();

    // 如果是 by-id 链接，优先用文件名匹配
    if (portPath.startsWith("/dev/serial/by-id/")) {
        if (matchByList(baseName, mountRule_.nameKeywords, mountRule_.vidpidList))
            return "Mount";
        if (matchByList(baseName, focuserRule_.nameKeywords, focuserRule_.vidpidList))
            return "Focuser";
        return {};
    }

    // 如果是 /dev/tty*，用启发式判断：ACM 偏向电调，USB 偏向赤道仪
    if (baseName.contains("ttyACM", Qt::CaseInsensitive))
        return "Focuser";
    if (baseName.contains("ttyUSB", Qt::CaseInsensitive))
        return "Mount";

    return {};
}
