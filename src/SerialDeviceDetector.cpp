#include "SerialDeviceDetector.h"
#include <QRegularExpression>
#include <QDebug>
#include <QSerialPortInfo>
#include <QDir>
#include <QFileInfo>
#include <limits>

static QString norm(QString s) {
    // 标准化比较用：全小写 + 用下划线替空格/连字符
    s = s.toLower();
    s.replace(' ', '_');
    s.replace('-', '_');
    return s;
}

SerialDeviceDetector::SerialDeviceDetector(QObject* parent)
    : QObject(parent)
{
    // =========================
    // Mount（赤道仪/手柄/控制盒）
    // - 以“常见 USB-串口桥”的 VID:PID 为主（FTDI/CP210x/Prolific/WCH/部分 STM32 VCP）
    // - 关键词包含主流品牌与系列名，供 by-id 名称辅助命中
    // =========================
    mountRule_.nameKeywords = QStringList()
            // 芯片/类属关键词
            << "1a86" << "ch34" << "usb_serial" << "ftdi" << "cp210"
            << "prolific" << "pl2303" << "wch" << "ch9102"
            // 品牌/产品关键词（按常见写法与 by-id 字符串习惯补充）
            << "sky-watcher" << "skywatcher" << "synscan"
            << "celestron" << "nexstar" << "starsense"
            << "ioptron"
            << "losmandy" << "gemini" << "gemini-2" << "gemini2"
            << "eqmod" << "onstep"
            << "explorescientific" << "pmc" << "pmc-eight" << "pmc8" << "pmc_eight"
            << "rainbow" << "rainbowastro" << "rst";

    // 说明：
    // - 0403:6001 / 0403:6015   → FTDI FT232R / FT231X（广泛用于手柄/控制盒）
    // - 067b:2303               → Prolific PL2303（Sky-Watcher/Celestron 手柄常见）
    // - 10c4:ea60               → Silicon Labs CP210x（iOptron/RainbowAstro 等常见）
    // - 1a86:7523               → WCH CH340/341（EQMOD 线/自制控制盒常见）
    // - 1a86:55d4 / 55d3 / 5523 → WCH CH9102F/CH9102X/CH341 变种
    // - 0483:5740               → STM32 VCP（现场确有赤道仪/控制盒采用）
    // - 0403:6010               → FTDI FT2232（少量控制盒为双通道—可保留以兼容）
    mountRule_.vidpidList   = QStringList()
            << "0403:6001"   /* FTDI FT232R */
            << "0403:6015"   /* FTDI FT231X */
            << "0403:6010"   /* FTDI FT2232（个别控制盒） */
            << "067b:2303"   /* Prolific PL2303（SynScan/NexStar 常见） */
            << "10c4:ea60"   /* Silicon Labs CP210x（iOptron/RainbowAstro 常见） */
            << "1a86:7523"   /* WCH CH340/341（EQMOD/自制） */
            << "1a86:55d4"   /* CH9102F */
            << "1a86:55d3"   /* CH9102X */
            << "1a86:5523"   /* CH341 变种 */
            << "0483:5740";  /* STM32 VCP：现场存在赤道仪使用 */
    // =========================
    // Focuser（调焦器）
    // - 为减少“与赤道仪共享通用桥”导致的误判，这里仅保留已知明确 VID:PID
    // - 关键词用于 by-id 名称辅助
    // =========================
    focuserRule_.nameKeywords = QStringList()
            << "gd32" << "gigadevice"
            << "focus" << "focuser" << "usb_focus"
            << "zwo" << "eaf"
            << "pegasus" << "focuscube"
            << "moonlite"
            << "optec" << "focuslynx"
            << "primaluce" << "esatto" << "sesto" << "senso" << "sesto_senso"
            << "rigel" << "nstep"
            << "lakeside";

    // 已知明确：
    // - 28e9:018a → GD32 CDC-ACM（常见于若干 DIY/品牌控制盒）
    // - 303a:9000 → Pegasus FocusCube 3（USB-CDC）
    // 注：MoonLite/Optec/PrimaLuce/Rigel/Lakeside 等很多机型基于 FTDI/CP210x，
    //     为避免与赤道仪冲突，不加入通用桥 PID，靠 by-id 名称 + handshake 兜底。
    focuserRule_.vidpidList   = QStringList()
            << "28e9:018a"   /* GD32 CDC-ACM */
            << "303a:9000";  /* Pegasus FocusCube3 */
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

int SerialDeviceDetector::scorePortForType(const QString& portPath,
                         const SerialMatchRule& rule,
                         const SerialMatchRule& otherRule,
                         const QString& deviceType) const
{
    if (portPath.isEmpty()) return 0;

    int score = 0;

    auto add = [&](int s){ score += s; };

    // 1) 直接读取 VID:PID
    QString realPort = portPath;
    if (portPath.startsWith("/dev/serial/by-id/")) {
        const QString resolved = resolveByIdToReal(QFileInfo(portPath).fileName());
        if (!resolved.isEmpty()) realPort = resolved;
    }
    for (const QSerialPortInfo &info : QSerialPortInfo::availablePorts()) {
        if (info.systemLocation() == realPort) {
            const quint16 vid = info.vendorIdentifier();
            const quint16 pid = info.productIdentifier();
            if (vid != 0 || pid != 0) {
                const QString vp = QString("%1:%2").arg(vid, 4, 16, QChar('0')).arg(pid, 4, 16, QChar('0')).toLower();
                if (rule.vidpidList.contains(vp)) add(1000);
                if (otherRule.vidpidList.contains(vp)) add(-1000);
            }
            // 厂商/产品字符串辅助（弱权重）
            const QString m = info.manufacturer().toLower();
            const QString p = info.description().toLower();
            for (const auto& k : rule.nameKeywords) {
                if (!k.isEmpty() && (m.contains(k) || p.contains(k))) { add(50); break; }
            }
            for (const auto& k : otherRule.nameKeywords) {
                if (!k.isEmpty() && (m.contains(k) || p.contains(k))) { add(-50); break; }
            }
            break;
        }
    }

    // 2) by-id 名称
    if (portPath.startsWith("/dev/serial/by-id/")) {
        const QString e = QFileInfo(portPath).fileName().toLower();
        for (const auto& vp : rule.vidpidList) if (nameContainsVidPid(e, vp)) add(300);
        for (const auto& k : rule.nameKeywords) if (!k.isEmpty() && e.contains(k)) add(100);
        for (const auto& vp : otherRule.vidpidList) if (nameContainsVidPid(e, vp)) add(-300);
        for (const auto& k : otherRule.nameKeywords) if (!k.isEmpty() && e.contains(k)) add(-100);
    } else {
        // /dev/tty* 通过 by-id 反查
        const QStringList entries = listByIdEntries();
        for (const auto& e : entries) {
            const QString real = resolveByIdToReal(e);
            if (real == portPath) {
                const QString el = e.toLower();
                for (const auto& vp : rule.vidpidList) if (nameContainsVidPid(el, vp)) add(300);
                for (const auto& k : rule.nameKeywords) if (!k.isEmpty() && el.contains(k)) add(100);
                for (const auto& vp : otherRule.vidpidList) if (nameContainsVidPid(el, vp)) add(-300);
                for (const auto& k : otherRule.nameKeywords) if (!k.isEmpty() && el.contains(k)) add(-100);
                break;
            }
        }
    }

    // 3) 握手强权重
    if (handshake_) {
        if (handshake_(realPort, deviceType)) add(5000);
        // 若另一类握手通过则强烈减分
        const QString other = (deviceType == "Mount" ? "Focuser" : "Mount");
        if (handshake_(realPort, other)) add(-5000);
    }

    return score;
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
    if (entries.isEmpty()) return {};

    // 对每个候选计算分值，选最高者
    QString bestPort;
    int bestScore = std::numeric_limits<int>::min();
    const SerialMatchRule& other = (deviceType == "Mount" ? focuserRule_ : mountRule_);

    for (const auto& e : entries) {
        const QString real = resolveByIdToReal(e);
        if (real.isEmpty()) continue;
        const int sc = scorePortForType(real, rule, other, deviceType);
        if (sc > bestScore) { bestScore = sc; bestPort = real; }
    }

    // 设阈值，分值过低视为不可靠
    if (bestScore <= 0) return {};
    return bestPort;
}

DevicePorts SerialDeviceDetector::rescan() {
    DevicePorts ret;
    ret.mountPort   = pickByRule(mountRule_,   "Mount");
    ret.focuserPort = pickByRule(focuserRule_, "Focuser");

    // 歧义消解：若某端口同时与另一类规则匹配，则优先用握手确认；无握手则置空避免误判
    const auto entries = listByIdEntries();
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
    auto findEntryByReal = [&](const QString& real)->QString {
        for (const auto& e : entries) {
            const QString r = resolveByIdToReal(e);
            if (!r.isEmpty() && r == real) return e;
        }
        return {};
    };

    // 检查 mountPort 是否也匹配 Focuser 规则
    if (!ret.mountPort.isEmpty()) {
        const QString e = findEntryByReal(ret.mountPort);
        if (!e.isEmpty() && matchByList(e, focuserRule_.nameKeywords, focuserRule_.vidpidList)) {
            bool keepMount = true;
            if (handshake_) {
                const bool okM = handshake_(ret.mountPort, "Mount");
                const bool okF = handshake_(ret.mountPort, "Focuser");
                if (!okM && okF) {
                    // 更像 Focuser：将其归入 Focuser（若空位）并清空 Mount
                    if (ret.focuserPort.isEmpty()) ret.focuserPort = ret.mountPort;
                    keepMount = false;
                } else if (!okM && !okF) {
                    keepMount = false;
                }
            } else {
                // 无握手，避免误判：置空，交由上层或下次扫描
                keepMount = false;
            }
            if (!keepMount) ret.mountPort.clear();
        }
    }

    // 检查 focuserPort 是否也匹配 Mount 规则
    if (!ret.focuserPort.isEmpty()) {
        const QString e = findEntryByReal(ret.focuserPort);
        if (!e.isEmpty() && matchByList(e, mountRule_.nameKeywords, mountRule_.vidpidList)) {
            bool keepFoc = true;
            if (handshake_) {
                const bool okF = handshake_(ret.focuserPort, "Focuser");
                const bool okM = handshake_(ret.focuserPort, "Mount");
                if (!okF && okM) {
                    // 更像 Mount：将其归入 Mount（若空位）并清空 Focuser
                    if (ret.mountPort.isEmpty()) ret.mountPort = ret.focuserPort;
                    keepFoc = false;
                } else if (!okF && !okM) {
                    keepFoc = false;
                }
            } else {
                // 无握手，避免误判：置空
                keepFoc = false;
            }
            if (!keepFoc) ret.focuserPort.clear();
        }
    }

    // 可选：避免两者命中同一物理端口（当 handshake 未设置时）
    if (!ret.mountPort.isEmpty() && ret.mountPort == ret.focuserPort) {
        // 如果发生冲突，优先保留 Mount，清空 Focuser 交由上层处理/下一轮握手判定
        ret.focuserPort.clear();
    }

    if (log_) {
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

static QString resolveByIdAbsoluteToReal(const QString& byIdPathAbs)
{
    QFileInfo fi(byIdPathAbs);
    if (!fi.exists()) return {};                    // by-id 项本身不存在
    const QString real = fi.canonicalFilePath();    // 跟随符号链接
    if (real.isEmpty()) return {};
    return real;
}

// 端口存在性与可见性校验：文件系统 + QSerialPortInfo 双重确认
static bool portNodeIsPresent(const QString& absPortPath)
{
    // 1) 文件系统节点存在（允许字符设备）
    QFileInfo fi(absPortPath);
    const bool fsOk = fi.exists() && fi.isReadable();

    // 2) 在 QSerialPortInfo 列表里能找到（系统认的串口）
    bool infoOk = false;
    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo& p : ports) {
        if (p.systemLocation() == absPortPath) {
            infoOk = true;
            break;
        }
    }

    // 两者其一为真即可认为“存在”（部分设备权限问题会导致 isReadable=false）
    return fsOk || infoOk;
}

QString SerialDeviceDetector::detectDeviceTypeForPort(const QString& portPath) const
{
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

    // —— 第一步：规范化成“绝对路径 & 真实节点” ——
    QString absInput = portPath;
    if (!absInput.startsWith('/')) {
        // 传进来如果是个 by-id 基名，补全成绝对路径
        if (QFileInfo(QStringLiteral("/dev/serial/by-id/%1").arg(portPath)).exists())
            absInput = QStringLiteral("/dev/serial/by-id/%1").arg(portPath);
        else if (QFileInfo(QStringLiteral("/dev/%1").arg(portPath)).exists())
            absInput = QStringLiteral("/dev/%1").arg(portPath);
    }

    QString realPort = absInput;
    if (absInput.startsWith("/dev/serial/by-id/")) {
        const QString real = resolveByIdAbsoluteToReal(absInput);
        if (!real.isEmpty()) realPort = real;
    }

    // —— 第二步：存在性校验（关键补充） ——
    if (!portNodeIsPresent(realPort)) {
        qWarning() << "[SerialDeviceDetector] Port not present or not visible:" << realPort
                   << "(from" << portPath << ")";
        return {}; // 不存在/不可见，直接放弃，避免误判
    }

    // —— 第三步：VID:PID 打分判定（你原有逻辑） ——
    QFileInfo fi(absInput);
    const QString baseName = fi.fileName();

    const int scMount   = scorePortForType(realPort, mountRule_,   focuserRule_, "Mount");
    const int scFocuser = scorePortForType(realPort, focuserRule_, mountRule_,   "Focuser");
    if (scMount > 0 || scFocuser > 0) {
        if (scMount > scFocuser)   return "Mount";
        if (scFocuser > scMount)   return "Focuser";
        return {}; // 分数相等保持未知，避免误判
    }

    // —— 第四步：by-id 文件名兜底匹配 ——
    if (absInput.startsWith("/dev/serial/by-id/")) {
        if (matchByList(baseName, mountRule_.nameKeywords, mountRule_.vidpidList))   return "Mount";
        if (matchByList(baseName, focuserRule_.nameKeywords, focuserRule_.vidpidList)) return "Focuser";
        return {};
    }

    // —— 第五步：从 /dev/tty* 反查 by-id ——
    if (absInput.startsWith("/dev/tty")) {
        const QStringList entries = listByIdEntries(); // 你已有
        for (const auto& e : entries) {
            const QString byIdAbs = QStringLiteral("/dev/serial/by-id/%1").arg(e);
            const QString real = resolveByIdAbsoluteToReal(byIdAbs);
            if (real == absInput) {
                if (matchByList(e, mountRule_.nameKeywords, mountRule_.vidpidList))   return "Mount";
                if (matchByList(e, focuserRule_.nameKeywords, focuserRule_.vidpidList)) return "Focuser";
            }
        }
    }

    return {};
}