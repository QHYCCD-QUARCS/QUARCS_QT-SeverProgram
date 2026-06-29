#include "mainwindow.h"
#include "quarcs_build_version.h"

#include <functional>

// SDK 框架相关头文件
#include "sdks/SdkManager.h"
#include "sdks/SdkCommon.h"  // 包含统一的 SDK 类型定义（SdkControlParamInfo, SdkFocuserOpenParam 等）
#include "sdks/SdkDriverRegistry.h"  // 🆕 驱动注册表
#include "sdks/SdkSerialExecutor.h"

#include <cstdint>
#include <cstddef>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <future>
#include <memory>
#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <unordered_set>
#include <chrono>
#include <utility>

// Qt 相关头文件（用于文件/目录操作）
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QDataStream>
#include <QPointer>
#include <QSet>
#include <QVector>
#include <QTemporaryFile>
#include <QCoreApplication>
#include <QThread>
#include <QProcess>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrentRun>

// 系统调用相关头文件（用于 FIFO 操作）
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>

// ============================================================================
// INDI 设备指针（全局变量）
// ============================================================================

INDI::BaseDevice *dpMount      = nullptr;  // 赤道仪设备指针
INDI::BaseDevice *dpGuider     = nullptr;  // 导星相机设备指针
INDI::BaseDevice *dpPoleScope  = nullptr;  // 极轴镜设备指针
INDI::BaseDevice *dpMainCamera = nullptr;  // 主相机设备指针
INDI::BaseDevice *dpFocuser    = nullptr;  // 电调设备指针
INDI::BaseDevice *dpCFW        = nullptr;  // 滤镜轮设备指针

namespace {

template <typename Func>
void postGuiderCore(GuiderCore *core, Func &&func)
{
    if (!core)
        return;
    QMetaObject::invokeMethod(core,
                              [core, fn = std::forward<Func>(func)]() mutable {
                                  fn(core);
                              },
                              Qt::QueuedConnection);
}

} // namespace

// ============================================================================
// SDK 设备句柄（当设备使用 SDK 模式时使用对应句柄）
// ============================================================================

SdkDeviceHandle sdkMountHandle      = nullptr;  // SDK 赤道仪句柄
SdkDeviceHandle sdkGuiderHandle     = nullptr;  // SDK 导星相机句柄
SdkDeviceHandle sdkPoleScopeHandle = nullptr;  // SDK 极轴镜句柄
SdkDeviceHandle sdkMainCameraHandle = nullptr; // SDK 主相机句柄
SdkDeviceHandle sdkFocuserHandle   = nullptr;  // SDK 电调句柄
SdkDeviceHandle sdkCFWHandle        = nullptr;  // SDK 滤镜轮句柄

QString sdkFocuserPort;  // SDK 电调串口路径

namespace {
bool isValidObservatoryLocation(double latitude, double longitude)
{
    return std::isfinite(latitude) &&
           std::isfinite(longitude) &&
           std::abs(latitude) <= 90.0 &&
           std::abs(longitude) <= 180.0 &&
           !(latitude == 0.0 && longitude == 0.0) &&
           latitude != -1.0 &&
           longitude != -1.0;
}

const char *polarRoleName(MainWindow::PolarAlignmentCameraRole role)
{
    switch (role)
    {
    case MainWindow::PolarAlignmentCameraRole::Guider:
        return "Guider";
    case MainWindow::PolarAlignmentCameraRole::PoleCamera:
        return "PoleCamera";
    case MainWindow::PolarAlignmentCameraRole::MainCamera:
    default:
        return "MainCamera";
    }
}

bool isPoleMasterName(const QString &name)
{
    return name.contains("POLEMASTER", Qt::CaseInsensitive);
}
}

// ============================================================================
// QHYCCD SDK 多相机句柄池（用于"设备分配"逻辑）
// ============================================================================
// 设计说明：
// - 启动/连接时扫描到多少台相机就全部打开，放入池中
// - 通过前端 BindingDevice 的 DeviceIndex 选择具体哪一台分配给 MainCamera 等角色
// - 为了复用现有前端协议（DeviceIndex 是 int），SDK 相机使用负数 index 编码：
//     uiIndex = -(poolIndex + 1)  =>  poolIndex = -uiIndex - 1

QVector<SdkDeviceHandle> g_sdkQhyCamHandles;      // SDK 相机句柄池
QVector<QString>         g_sdkQhyCamIds;          // SDK 相机 ID 池（与句柄一一对应）
int                      g_sdkMainCameraPoolIndex = -1;  // 当前分配给 MainCamera 的池索引（-1 表示未分配）
int                      g_sdkGuiderPoolIndex = -1;      // 当前分配给 Guider 的池索引（-1 表示未分配）
int                      g_sdkPoleCameraPoolIndex = -1;  // 当前分配给 PoleCamera 的池索引（-1 表示未分配）

namespace {
struct SyncCommandResult {
    int exitCode = -1;
    QString out;
    QString err;
    bool finished = false;
};

SyncCommandResult runCommandSync(const QString &program, const QStringList &args, int timeoutMs = 3000)
{
    QProcess process;
    process.start(program, args);
    if (!process.waitForStarted(timeoutMs)) {
        return {-1, QString(), process.errorString(), false};
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        return {124,
                QString::fromUtf8(process.readAllStandardOutput()).trimmed(),
                QString::fromUtf8(process.readAllStandardError()).trimmed() + "\n(timeout)",
                false};
    }
    return {process.exitCode(),
            QString::fromUtf8(process.readAllStandardOutput()).trimmed(),
            QString::fromUtf8(process.readAllStandardError()).trimmed(),
            true};
}

SyncCommandResult runSudoSync(const QString &program, const QStringList &args, int timeoutMs = 3000)
{
    QStringList sudoArgs;
    sudoArgs << "-n" << program;
    sudoArgs << args;
    return runCommandSync("sudo", sudoArgs, timeoutMs);
}

bool isValidSystemDeviceIndex(const SystemDeviceList &deviceList, int index)
{
    return index >= 0 && index < deviceList.system_devices.size();
}

void drawFocusDebugCrosshair(cv::Mat &image16, double x, double y)
{
    if (image16.empty() || image16.type() != CV_16UC1) {
        return;
    }

    const int cx = static_cast<int>(std::lround(x));
    const int cy = static_cast<int>(std::lround(y));
    if (cx < 0 || cy < 0 || cx >= image16.cols || cy >= image16.rows) {
        return;
    }

    const int armBase = std::max(6, std::min(image16.cols, image16.rows) / 20);
    const int arm = armBase * 10;
    const int x0 = std::max(0, cx - arm);
    const int x1 = std::min(image16.cols - 1, cx + arm);
    const int y0 = std::max(0, cy - arm);
    const int y1 = std::min(image16.rows - 1, cy + arm);
    const cv::Scalar white(std::numeric_limits<unsigned short>::max());
    const cv::Scalar black(0);

    cv::line(image16, cv::Point(x0, cy), cv::Point(x1, cy), black, 3, cv::LINE_8);
    cv::line(image16, cv::Point(cx, y0), cv::Point(cx, y1), black, 3, cv::LINE_8);
    cv::line(image16, cv::Point(x0, cy), cv::Point(x1, cy), white, 1, cv::LINE_8);
    cv::line(image16, cv::Point(cx, y0), cv::Point(cx, y1), white, 1, cv::LINE_8);
}

constexpr const char *kPreferredHostapdConf = "/etc/hostapd/uap0.conf";
constexpr const char *kPreferredWpaSupplicantConf = "/etc/wpa_supplicant/wpa_supplicant-wlan0.conf";
constexpr const char *kPreferredWlanDhcpService = "wlan0-dhcp.service";
constexpr const char *kPreferredHostapdService = "hostapd@uap0";
constexpr const char *kPreferredWpaService = "wpa_supplicant@wlan0";
constexpr const char *kSavedStaProfilesConfigKey = "SavedStaProfilesJson";
constexpr const char *kLegacyHotspotConnectionName = "RaspBerryPi-WiFi";
constexpr const char *kDefaultHotspotName = "LQ";
constexpr const char *kDefaultCountryCode = "CN";

bool preferApStaStack()
{
    const bool hostapdConf = QFile::exists(QString::fromUtf8(kPreferredHostapdConf));
    const bool wpaConf = QFile::exists(QString::fromUtf8(kPreferredWpaSupplicantConf));
    return hostapdConf || wpaConf;
}

QString readTextFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const SyncCommandResult result = runSudoSync("/bin/cat", {path}, 2000);
        if (result.exitCode == 0) {
            return result.out;
        }
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

QString parseKeyValueLine(const QString &content, const QString &key)
{
    const QStringList lines = content.split('\n');
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.startsWith('#') || !line.startsWith(key + '=')) {
            continue;
        }
        return line.mid(key.length() + 1).trimmed();
    }
    return QString();
}

QString normalizeWpaQuotedValue(QString value)
{
    value = value.trimmed();
    if (value.size() >= 2 && value.startsWith('"') && value.endsWith('"')) {
        value = value.mid(1, value.size() - 2);
    }
    value.replace("\\\"", "\"");
    value.replace("\\\\", "\\");
    return value;
}

QString normalizeWifiSsidForMatch(QString value)
{
    value = value.trimmed();
    static const QRegularExpression trailingSignalRe(QStringLiteral(R"(\s+\d{1,3}%$)"));
    value.remove(trailingSignalRe);
    return value.trimmed();
}

int hexDigitValue(QChar ch)
{
    const ushort c = ch.unicode();
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

QString decodeIwEscapedSsid(const QString &value)
{
    QByteArray bytes;
    for (int i = 0; i < value.size(); ++i) {
        if (value.at(i) == QLatin1Char('\\') && i + 3 < value.size() && value.at(i + 1) == QLatin1Char('x')) {
            const int hi = hexDigitValue(value.at(i + 2));
            const int lo = hexDigitValue(value.at(i + 3));
            if (hi >= 0 && lo >= 0) {
                bytes.append(static_cast<char>((hi << 4) | lo));
                i += 3;
                continue;
            }
        }
        bytes.append(QString(value.at(i)).toUtf8());
    }
    return QString::fromUtf8(bytes).trimmed();
}

QString escapeWpaQuotedValue(QString value)
{
    value.replace("\\", "\\\\");
    value.replace("\"", "\\\"");
    return value;
}

QString readPreferredHotspotName()
{
    const QString content = readTextFile(QString::fromUtf8(kPreferredHostapdConf));
    const QString ssid = parseKeyValueLine(content, "ssid");
    if (!ssid.isEmpty()) {
        return ssid;
    }
    return QString::fromUtf8(kDefaultHotspotName);
}

QString buildPreferredWpaSupplicantConf(const QString &ssid, const QString &psk);

bool writePreferredHotspotName(const QString &newName, QString *errorOut = nullptr)
{
    const QString path = QString::fromUtf8(kPreferredHostapdConf);
    QString content = readTextFile(path);
    if (content.isEmpty() && !QFile::exists(path)) {
        if (errorOut) *errorOut = QStringLiteral("hostapd_conf_missing");
        return false;
    }

    QString updated = content;
    QRegularExpression re(R"((^|\n)\s*ssid=.*(?=\n|$))");
    QRegularExpressionMatch match = re.match(updated);
    if (match.hasMatch()) {
        const int start = match.capturedStart();
        const int length = match.capturedLength();
        const QString prefix = (updated.mid(start, 1) == "\n") ? "\n" : "";
        updated.replace(start, length, prefix + "ssid=" + newName);
    } else {
        if (!updated.isEmpty() && !updated.endsWith('\n')) {
            updated.append('\n');
        }
        updated += "ssid=" + newName + "\n";
    }

    QTemporaryFile file(QDir::tempPath() + "/quarcs-hostapd-XXXXXX.conf");
    if (!file.open()) {
        if (errorOut) *errorOut = file.errorString();
        return false;
    }
    file.write(updated.toUtf8());
    file.flush();

    const SyncCommandResult installRes =
        runSudoSync("/usr/bin/install", {"-m", "644", file.fileName(), path}, 3000);
    if (installRes.exitCode != 0) {
        if (errorOut) *errorOut = (installRes.err.isEmpty() ? installRes.out : installRes.err).trimmed();
        return false;
    }
    return true;
}

bool writePreferredWpaSupplicantConf(const QString &ssid, const QString &psk, QString *errorOut = nullptr)
{
    QTemporaryFile file(QDir::tempPath() + "/quarcs-wpa-XXXXXX.conf");
    if (!file.open()) {
        if (errorOut) *errorOut = file.errorString();
        return false;
    }
    file.write(buildPreferredWpaSupplicantConf(ssid, psk).toUtf8());
    file.flush();

    const SyncCommandResult installRes =
        runSudoSync("/usr/bin/install", {"-m", "600", file.fileName(), QString::fromUtf8(kPreferredWpaSupplicantConf)}, 3000);
    if (installRes.exitCode != 0) {
        if (errorOut) *errorOut = (installRes.err.isEmpty() ? installRes.out : installRes.err).trimmed();
        return false;
    }
    return true;
}

QJsonArray sanitizeSavedStaProfiles(const QJsonArray &profiles)
{
    QJsonArray cleaned;
    QSet<QString> seenExactSsids;
    for (const QJsonValue &value : profiles) {
        if (!value.isObject()) continue;
        const QJsonObject obj = value.toObject();
        const QString ssid = obj.value("ssid").toString().trimmed();
        const QString psk = obj.value("psk").toString();
        const QString name = obj.value("name").toString("wan-uplink").trimmed();
        if (ssid.isEmpty() || psk.isEmpty()) continue;
        if (seenExactSsids.contains(ssid)) continue;
        seenExactSsids.insert(ssid);
        QJsonObject saved;
        saved["ssid"] = ssid;
        saved["psk"] = psk;
        saved["name"] = name.isEmpty() ? QStringLiteral("wan-uplink") : name;
        cleaned.append(saved);
    }
    return cleaned;
}

QJsonArray readSavedStaProfilesFromConfig()
{
    Tools::makeConfigFile();
    std::unordered_map<std::string, std::string> config;
    Tools::readClientSettings("config/config.ini", config);

    const auto it = config.find(kSavedStaProfilesConfigKey);
    if (it == config.end() || it->second.empty()) {
        return QJsonArray();
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(it->second), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        Logger::Log(("readSavedStaProfilesFromConfig | bad_json: " + err.errorString()).toStdString(),
                    LogLevel::WARNING, DeviceType::MAIN);
        return QJsonArray();
    }

    return sanitizeSavedStaProfiles(doc.array());
}

void writeSavedStaProfilesToConfig(const QJsonArray &profiles)
{
    Tools::makeConfigFile();
    std::unordered_map<std::string, std::string> config;
    const QJsonDocument doc(sanitizeSavedStaProfiles(profiles));
    config[kSavedStaProfilesConfigKey] = doc.toJson(QJsonDocument::Compact).toStdString();
    Tools::saveClientSettings("config/config.ini", config);
}

QJsonObject findSavedStaProfileBySsid(const QJsonArray &profiles, const QString &ssid)
{
    const QString target = ssid.trimmed();
    const QString normalizedTarget = normalizeWifiSsidForMatch(ssid);

    for (const QJsonValue &value : profiles) {
        if (!value.isObject()) continue;
        const QJsonObject obj = value.toObject();
        if (obj.value("ssid").toString().trimmed() == target) {
            return obj;
        }
    }

    for (const QJsonValue &value : profiles) {
        if (!value.isObject()) continue;
        const QJsonObject obj = value.toObject();
        if (normalizeWifiSsidForMatch(obj.value("ssid").toString()) == normalizedTarget) {
            return obj;
        }
    }

    return QJsonObject();
}

QJsonArray upsertSavedStaProfile(const QJsonArray &profiles, const QString &ssid, const QString &psk, const QString &name)
{
    const QString normalizedSsid = normalizeWifiSsidForMatch(ssid);
    QJsonArray updated;
    bool replaced = false;

    for (const QJsonValue &value : profiles) {
        if (!value.isObject()) continue;
        QJsonObject obj = value.toObject();
        const QString existingSsid = obj.value("ssid").toString().trimmed();
        const bool sameProfile =
            existingSsid == ssid.trimmed() ||
            normalizeWifiSsidForMatch(existingSsid) == normalizedSsid;

        if (sameProfile) {
            obj["ssid"] = ssid.trimmed();
            obj["psk"] = psk;
            obj["name"] = name;
            replaced = true;
        }
        updated.append(obj);
    }

    if (!replaced) {
        QJsonObject obj;
        obj["ssid"] = ssid.trimmed();
        obj["psk"] = psk;
        obj["name"] = name;
        updated.append(obj);
    }

    return sanitizeSavedStaProfiles(updated);
}

QString readPreferredCountryCode()
{
    const QString content = readTextFile(QString::fromUtf8(kPreferredWpaSupplicantConf));
    const QString country = parseKeyValueLine(content, "country");
    if (!country.isEmpty()) {
        return country;
    }
    return QString::fromUtf8(kDefaultCountryCode);
}

QJsonObject readPreferredSavedStaConfig()
{
    const QString content = readTextFile(QString::fromUtf8(kPreferredWpaSupplicantConf));
    QJsonObject obj;
    obj["ssid"] = QString();
    obj["psk"] = QString();

    if (content.isEmpty()) {
        return obj;
    }

    QRegularExpression ssidRe(R"(ssid\s*=\s*("(?:\\.|[^"])*"|[^\n]+))");
    QRegularExpression pskRe(R"(psk\s*=\s*("(?:\\.|[^"])*"|[^\n]+))");

    const QRegularExpressionMatch ssidMatch = ssidRe.match(content);
    if (ssidMatch.hasMatch()) {
        obj["ssid"] = normalizeWpaQuotedValue(ssidMatch.captured(1));
    }

    const QRegularExpressionMatch pskMatch = pskRe.match(content);
    if (pskMatch.hasMatch()) {
        obj["psk"] = normalizeWpaQuotedValue(pskMatch.captured(1));
    }

    return obj;
}

QString buildPreferredWpaSupplicantConf(const QString &ssid, const QString &psk)
{
    const QString country = readPreferredCountryCode();
    return QStringLiteral(
               "ctrl_interface=/var/run/wpa_supplicant\n"
               "update_config=1\n"
               "country=%1\n"
               "\n"
               "network={\n"
               "    ssid=\"%2\"\n"
               "    psk=\"%3\"\n"
               "}\n")
        .arg(country,
             escapeWpaQuotedValue(ssid),
             escapeWpaQuotedValue(psk));
}

QString parseIpAddrOutput(const QString &out)
{
    QRegularExpression re(R"(\binet\s+([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+))");
    const QRegularExpressionMatch match = re.match(out);
    if (match.hasMatch()) {
        return match.captured(1);
    }
    return QString();
}

int signalDbmToPercent(double dbm)
{
    if (dbm <= -100.0) return 0;
    if (dbm >= -50.0) return 100;
    return static_cast<int>(std::lround((dbm + 100.0) * 2.0));
}

QJsonArray parseIwScanOutput(const QString &out)
{
    struct Entry {
        QString ssid;
        int signal = 0;
        QString security;
    };

    QVector<Entry> entries;
    Entry current;
    bool inBlock = false;
    bool blockHasPrivacy = false;
    bool blockHasRsn = false;
    bool blockHasWpa = false;

    auto flush = [&]() {
        if (!inBlock || current.ssid.trimmed().isEmpty()) {
            current = Entry{};
            inBlock = false;
            blockHasPrivacy = false;
            blockHasRsn = false;
            blockHasWpa = false;
            return;
        }
        if (blockHasRsn) current.security = "WPA2";
        else if (blockHasWpa) current.security = "WPA";
        else if (blockHasPrivacy) current.security = "WEP";
        else current.security = "OPEN";
        entries.push_back(current);
        current = Entry{};
        inBlock = false;
        blockHasPrivacy = false;
        blockHasRsn = false;
        blockHasWpa = false;
    };

    const QStringList lines = out.split('\n');
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.startsWith("BSS ")) {
            flush();
            inBlock = true;
            continue;
        }
        if (!inBlock) continue;

        if (line.startsWith("SSID:")) {
            current.ssid = decodeIwEscapedSsid(line.mid(QStringLiteral("SSID:").length()));
        } else if (line.startsWith("signal:")) {
            bool ok = false;
            const double dbm = line.mid(QStringLiteral("signal:").length()).trimmed().split(' ').value(0).toDouble(&ok);
            if (ok) current.signal = signalDbmToPercent(dbm);
        } else if (line.startsWith("RSN:")) {
            blockHasRsn = true;
        } else if (line.startsWith("WPA:")) {
            blockHasWpa = true;
        } else if (line.contains("capability:") && line.contains("Privacy")) {
            blockHasPrivacy = true;
        }
    }
    flush();

    QJsonArray arr;
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        return a.signal > b.signal;
    });

    QSet<QString> seenSsids;
    for (const Entry &entry : entries) {
        const QString ssid = entry.ssid.trimmed();
        if (ssid.isEmpty() || seenSsids.contains(ssid)) continue;
        seenSsids.insert(ssid);
        QJsonObject obj;
        obj["ssid"] = ssid;
        obj["signal"] = entry.signal;
        obj["security"] = entry.security;
        arr.append(obj);
    }
    return arr;
}

QString readWpaCliField(const QString &field)
{
    const QStringList wpaCliPrograms = {
        QStringLiteral("/usr/sbin/wpa_cli"),
        QStringLiteral("/sbin/wpa_cli"),
        QStringLiteral("/usr/bin/wpa_cli"),
        QStringLiteral("wpa_cli")
    };

    for (const QString &program : wpaCliPrograms) {
        const SyncCommandResult result = runSudoSync(program, {"-i", "wlan0", "status"}, 2500);
        if (result.exitCode != 0 || result.out.isEmpty()) {
            continue;
        }
        const QStringList lines = result.out.split('\n');
        for (const QString &line : lines) {
            if (line.startsWith(field + '=')) {
                return normalizeWpaQuotedValue(line.mid(field.length() + 1));
            }
        }
    }
    return QString();
}

QString readIwLinkedSsid(const QString &ifname)
{
    const QStringList iwPrograms = {
        QStringLiteral("/sbin/iw"),
        QStringLiteral("/usr/sbin/iw"),
        QStringLiteral("/usr/bin/iw"),
        QStringLiteral("iw")
    };

    for (const QString &program : iwPrograms) {
        const SyncCommandResult result = runCommandSync(program, {"dev", ifname, "link"}, 2500);
        if (result.exitCode != 0 || result.out.isEmpty()) {
            continue;
        }

        const QStringList lines = result.out.split('\n');
        for (const QString &rawLine : lines) {
            const QString line = rawLine.trimmed();
            if (line.startsWith(QStringLiteral("Not connected"), Qt::CaseInsensitive)) {
                return QString();
            }
            if (line.startsWith(QStringLiteral("SSID:"))) {
                const QString ssid = line.mid(QStringLiteral("SSID:").length()).trimmed();
                if (!ssid.isEmpty()) {
                    return ssid;
                }
            }
        }
    }

    return QString();
}

QString readCurrentStaSsid(const QString &ifname)
{
    const QString wpaSsid = readWpaCliField("ssid");
    if (!wpaSsid.isEmpty()) {
        return wpaSsid;
    }

    return readIwLinkedSsid(ifname);
}

QString readDefaultGateway()
{
    const SyncCommandResult result = runCommandSync("/sbin/ip", {"route", "show", "default"}, 2000);
    if (result.exitCode != 0) {
        return QString();
    }
    QRegularExpression re(R"(\bvia\s+([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+))");
    const QRegularExpressionMatch match = re.match(result.out);
    if (match.hasMatch()) {
        return match.captured(1);
    }
    return QString();
}

QString readInterfaceIpv4(const QString &ifname)
{
    const SyncCommandResult result = runCommandSync("/sbin/ip", {"-4", "-o", "addr", "show", "dev", ifname}, 2000);
    if (result.exitCode != 0) {
        return QString();
    }
    return parseIpAddrOutput(result.out);
}

QString readSystemdIsActive(const QString &service)
{
    const SyncCommandResult result = runSudoSync("/usr/bin/systemctl", {"is-active", service}, 2500);
    if (result.exitCode == 0) {
        return result.out.trimmed();
    }
    return QStringLiteral("inactive");
}

QJsonObject buildPreferredNetStatusJson()
{
    const QString hotspotName = readPreferredHotspotName();
    const QJsonObject currentWpaProfile = readPreferredSavedStaConfig();
    const QJsonArray savedStaProfiles = readSavedStaProfilesFromConfig();
    const QString wlanIp = readInterfaceIpv4("wlan0");
    const QString ethIp = readInterfaceIpv4("eth0");
    const QString uapIp = readInterfaceIpv4("uap0");
    const QString gateway = readDefaultGateway();
    const QString hostapdState = readSystemdIsActive(QString::fromUtf8(kPreferredHostapdService));
    const QString wpaState = readWpaCliField("wpa_state");
    QString staSsid = readCurrentStaSsid("wlan0");
    if (staSsid.isEmpty() && !wlanIp.isEmpty()) {
        const QString configuredSsid = currentWpaProfile.value("ssid").toString().trimmed();
        const bool explicitlyDisconnected =
            wpaState.compare(QStringLiteral("DISCONNECTED"), Qt::CaseInsensitive) == 0 ||
            wpaState.compare(QStringLiteral("INACTIVE"), Qt::CaseInsensitive) == 0;
        if (!configuredSsid.isEmpty() && !explicitlyDisconnected) {
            staSsid = configuredSsid;
        }
    }

    QString mode = "ap";
    if (hostapdState == "active" && !staSsid.isEmpty() && !wlanIp.isEmpty()) {
        mode = "ap+sta";
    } else if (hostapdState == "active") {
        mode = "ap";
    } else if (!wlanIp.isEmpty() || !ethIp.isEmpty()) {
        mode = "wan";
    }

    QJsonObject obj;
    obj["mode"] = mode;
    obj["stack"] = "ap_sta_systemd";
    obj["hotspot_ssid"] = hotspotName;
    obj["sta_ssid"] = staSsid;
    obj["saved_sta_profiles"] = savedStaProfiles;
    QJsonObject savedSta = findSavedStaProfileBySsid(savedStaProfiles, staSsid);
    if (savedSta.isEmpty()) {
        savedSta = findSavedStaProfileBySsid(savedStaProfiles, currentWpaProfile.value("ssid").toString());
    }
    if (savedSta.isEmpty()) {
        savedSta = currentWpaProfile;
    }
    obj["saved_sta_ssid"] = savedSta.value("ssid").toString();
    obj["saved_sta_psk"] = savedSta.value("psk").toString();
    obj["wlan_ip"] = wlanIp;
    obj["eth_ip"] = ethIp;
    obj["uap_ip"] = uapIp;
    obj["gateway"] = gateway;
    obj["zerotier"] = readSystemdIsActive("zerotier-one");
    obj["hostapd"] = hostapdState;
    obj["wpa_state"] = wpaState;
    return obj;
}
}

QString MainWindow::latestMainCaptureFitsPath() const
{
    if (!lastMainCaptureFitsPath.isEmpty() && QFile::exists(lastMainCaptureFitsPath))
        return lastMainCaptureFitsPath;

    const QString fallback = QStringLiteral("/dev/shm/ccd_simulator.fits");
    if (QFile::exists(fallback))
        return fallback;

    return QString();
}

// 索引转换辅助函数
static inline int sdkUiIndexFromPoolIndex(int poolIndex)
{
    return -(poolIndex + 1);
}

// SDK 非“池化设备”（例如电调）使用固定负数 index，避免与 INDI 的正 index 冲突，也避免与相机池的 -(n+1) 冲突。
// 约定：相机池使用 [-1, -2, ...]；固定设备从 -10000 往下分配。
static constexpr int SDK_FOCUSER_UI_INDEX = -10001;

static constexpr int kIndiFocuserRelMoveChunkMax = 10000;

static inline int sdkPoolIndexFromUiIndex(int uiIndex)
{
    return -uiIndex - 1;
}

static inline bool sdkPoolIndexValid(int poolIndex)
{
    return poolIndex >= 0 &&
           poolIndex < g_sdkQhyCamHandles.size() &&
           poolIndex < g_sdkQhyCamIds.size() &&
           !g_sdkQhyCamIds[poolIndex].isEmpty();
}

// ============================================================================
// CFW (滤镜轮) SDK/INDI 兼容辅助函数
// ============================================================================
// 说明：前端协议使用 1-based 位置（CFW[1..N]），QHY SDK 内部使用 0-based（0..N-1）

/**
 * @brief 将前端 1-based 位置转换为 SDK 0-based 位置
 * @param pos1Based 前端位置（1-based）
 * @return SDK 位置（0-based）
 */
static inline int toSdkCfwPos0(int pos1Based)
{
    return std::max(0, pos1Based - 1);
}

/**
 * @brief 将 SDK 0-based 位置转换为前端 1-based 位置
 * @param pos0Based SDK 位置（0-based）
 * @return 前端位置（1-based）
 */
static inline int toUiCfwPos1(int pos0Based)
{
    return pos0Based + 1;
}

/**
 * @brief 生成 CFW 存储键名（用于 Tools::saveCFWList/readCFWList）
 * @param cameraId 相机 ID
 * @return 存储键名
 * @note 确保：
 *       - 不依赖 INDI 的 FILTER_NAME
 *       - 不与外置 CFW（dpCFW 的 deviceName）冲突
 *       - 多相机时也能区分
 */
static inline QString sdkCfwStorageKey(const QString& cameraId)
{
    if (!cameraId.isEmpty())
        return "SDK_CFW_" + cameraId;
    return "SDK_CFW_MainCamera";
}

/**
 * @brief 获取滤镜轮槽位数量
 * @param handle SDK 设备句柄
 * @param slotsNum 输出参数：槽位数量
 * @param errMsg 输出参数：错误信息（可选）
 * @return 成功返回 true，失败返回 false
 */
static bool sdkGetCfwSlotsNum(SdkDeviceHandle handle, int& slotsNum, std::string* errMsg = nullptr)
{
    SdkCommand cmd;
    cmd.type = SdkCommandType::Custom;
    cmd.name = "GetCFWSlotsNum";
    cmd.payload = std::any();
    
    // 直接通过设备句柄调用，无需指定驱动名称（类似INDI的调用方式）
    SdkResult res = SdkManager::instance().callByHandle(handle, cmd);
    if (!res.success || !res.payload.has_value()) {
        if (errMsg) *errMsg = res.message;
        return false;
    }
    
    try {
        slotsNum = std::any_cast<int>(res.payload);
        return true;
    } catch (const std::bad_any_cast&) {
        if (errMsg) *errMsg = "GetCFWSlotsNum bad_any_cast";
        return false;
    }
}

/**
 * @brief 获取滤镜轮当前位置（0-based）
 * @param handle SDK 设备句柄
 * @param pos0 输出参数：当前位置（0-based）
 * @param errMsg 输出参数：错误信息（可选）
 * @return 成功返回 true，失败返回 false
 */
static bool sdkGetCfwPosition0(SdkDeviceHandle handle, int& pos0, std::string* errMsg = nullptr)
{
    SdkCommand cmd;
    cmd.type = SdkCommandType::Custom;
    cmd.name = "GetCFWPosition";
    cmd.payload = std::any();
    
    // 直接通过设备句柄调用，无需指定驱动名称
    SdkResult res = SdkManager::instance().callByHandle(handle, cmd);
    if (!res.success || !res.payload.has_value()) {
        if (errMsg) *errMsg = res.message;
        return false;
    }
    
    try {
        pos0 = std::any_cast<int>(res.payload);
        return true;
    } catch (const std::bad_any_cast&) {
        if (errMsg) *errMsg = "GetCFWPosition bad_any_cast";
        return false;
    }
}

/**
 * @brief 设置滤镜轮位置并等待到位（0-based）
 * @param handle SDK 设备句柄
 * @param targetPos0 目标位置（0-based）
 * @param timeoutMs 超时时间（毫秒）
 * @param errMsg 输出参数：错误信息（可选）
 * @return 成功返回 true，失败返回 false
 * @note 先下发设置命令，再轮询确认位置，避免滤镜未到位就开拍
 */
static bool sdkSetCfwPosition0AndWait(SdkDeviceHandle handle, int targetPos0, int timeoutMs, std::string* errMsg = nullptr)
{
    // 1) 下发设置命令
    {
        SdkCommand cmd;
        cmd.type = SdkCommandType::Custom;
        cmd.name = "SetCFWPosition";
        cmd.payload = targetPos0;
        
        // 直接通过设备句柄调用，无需指定驱动名称
        SdkResult res = SdkManager::instance().callByHandle(handle, cmd);
        if (!res.success) {
            if (errMsg) *errMsg = res.message;
            return false;
        }
    }

    // 2) 轮询确认位置（每 200ms 检查一次）
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        int cur0 = -1;
        if (sdkGetCfwPosition0(handle, cur0, errMsg) && cur0 == targetPos0)
            return true;
        QThread::msleep(200);
    }
    
    if (errMsg) *errMsg = "SetCFWPosition timeout";
    return false;
}

/**
 * @brief INDI：设置滤镜轮位置并等待到位（1-based）
 * @param client INDI client
 * @param dp 设备指针（相机或独立滤镜轮）
 * @param targetPos1 目标位置（1-based）
 * @param timeoutMs 超时时间（毫秒）
 * @param errMsg 输出参数：错误信息（可选）
 * @return 成功返回 true，失败返回 false
 *
 * @note 仅当确认位置已到位才返回成功，避免“命令已下发但未到位”导致前端位置提前变化。
 */
static bool indiSetCfwPosition1AndWait(MyClient* client,
                                      INDI::BaseDevice* dp,
                                      int targetPos1,
                                      int timeoutMs,
                                      std::string* errMsg = nullptr)
{
    if (!client || !dp) {
        if (errMsg) *errMsg = "device_null";
        return false;
    }
    if (!dp->isConnected()) {
        if (errMsg) *errMsg = "device_disconnected";
        return false;
    }

    // 1) 下发设置命令
    const uint32_t ret = client->setCFWPosition(dp, targetPos1);
    if (ret != QHYCCD_SUCCESS) {
        if (errMsg) *errMsg = "indi_error";
        return false;
    }

    // 2) 轮询确认位置（每 200ms 检查一次）
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < timeoutMs) {
        if (!dp->isConnected()) {
            if (errMsg) *errMsg = "device_disconnected";
            return false;
        }
        int pos = -1, min = 0, max = 0;
        const uint32_t gr = client->getCFWPosition(dp, pos, min, max);
        if (gr == QHYCCD_SUCCESS && pos == targetPos1)
            return true;
        QThread::msleep(200);
    }

    if (errMsg) *errMsg = "timeout";
    return false;
}

// ============================================================================
// 全局变量定义
// ============================================================================

DriversList drivers_list;              // 驱动列表
std::vector<DevGroup> dev_groups;      // 设备分组列表
std::vector<Device> devices;           // 设备列表
DriversListNew drivers_list_new;       // 新版驱动列表
SystemDevice systemdevice;             // 系统设备
SystemDeviceList systemdevicelist;     // 系统设备列表
QUrl websocketUrl;                     // WebSocket 连接 URL

int LoopCaptureNum = 0;
// Loop capture 在 Burst 模式下需要知道每次 Burst 的帧数（由 takeExposureBurst 更新）
int LoopCaptureBurstFrames = 1;

// ============================================================================
// MainWindow 类静态成员变量定义
// ============================================================================

MainWindow *MainWindow::instance = nullptr;

/**
 * @brief 获取构建日期字符串（从编译时宏 __DATE__ / __TIME__ 解析）
 * @return 构建日期字符串（格式：YYYYMMDDHHMM）
 */
std::string MainWindow::getBuildDate()
{
    static const std::map<std::string, std::string> monthMap = {
        {"Jan", "01"}, {"Feb", "02"}, {"Mar", "03"}, {"Apr", "04"},
        {"May", "05"}, {"Jun", "06"}, {"Jul", "07"}, {"Aug", "08"},
        {"Sep", "09"}, {"Oct", "10"}, {"Nov", "11"}, {"Dec", "12"}
    };

    std::string date = __DATE__;
    std::string time = __TIME__;
    std::stringstream dateStream(date);
    std::stringstream timeStream(time);
    std::string month, day, year;
    std::string hour, minute;
    dateStream >> month >> day >> year;
    std::getline(timeStream, hour, ':');
    std::getline(timeStream, minute, ':');

    return year + monthMap.at(month) + (day.size() == 1 ? "0" + day : day) + hour + minute;
}

MainWindow::MainWindow(QObject *parent) : QObject(parent)
{
    // 初始化极轴校准对象为nullptr
    polarAlignment = nullptr;

    // 初始化相机参数
    glFocalLength = 0;
    glCameraSize_width = 0.0;
    glCameraSize_height = 0.0;

    system_timer = new QTimer(this); // 用于对系统的监测
    connect(system_timer, &QTimer::timeout, this, &MainWindow::updateCPUInfo);
    system_timer->start(3000);

    Logger::Initialize();
    getHostAddress();

    // ========================= 图像保存根目录（本地） =========================
    // 默认：系统家目录下 ~/images
    // 可通过环境变量 QUARCS_IMAGE_SAVE_ROOT 覆盖
    // 注意：这里必须在 Logger::Initialize() 之后再记录日志，避免未初始化导致崩溃
    {
        QString base = QDir::cleanPath(QDir::homePath() + "/images");
        if (const char *env = std::getenv("QUARCS_IMAGE_SAVE_ROOT"))
        {
            const std::string v(env);
            if (!v.empty())
            {
                base = QDir::cleanPath(QString::fromStdString(v));
                Logger::Log("MainWindow | QUARCS_IMAGE_SAVE_ROOT=" + v, LogLevel::INFO, DeviceType::MAIN);
            }
        }
        ImageSaveBasePath = base.toStdString();
        ImageSaveBaseDirectory = base;
        Logger::Log("MainWindow | ImageSaveBasePath=" + ImageSaveBasePath, LogLevel::INFO, DeviceType::MAIN);
    }

    // 允许通过环境变量覆盖 /img 的真实根目录（Web 静态 img 目录）
    // 例如：
    //   export QUARCS_WEB_IMG_ROOT="/home/quarcs/.../apps/web-frontend/dist/img/"
    //   export QUARCS_WEB_IMG_ROOT="/var/www/html/img/"
    if (const char *env = std::getenv("QUARCS_WEB_IMG_ROOT"))
    {
        const std::string v(env);
        if (!v.empty())
        {
            vueImagePath = v;
            Logger::Log("MainWindow | QUARCS_WEB_IMG_ROOT=" + vueImagePath, LogLevel::INFO, DeviceType::MAIN);
        }
    }
    // 规范化末尾斜杠
    if (!vueImagePath.empty() && vueImagePath.back() != '/')
        vueImagePath.push_back('/');

    // ========================= /img/capture-tiles 映射自检（树莓派部署） =========================
    // 前端会通过 HTTP 请求 /img/capture-tiles/<session>/<z>/<x>/<y>.bin
    // 而后端瓦片默认写入 tmpfs：/dev/shm/capture-tiles/...
    // 若 Web 静态根目录为 /var/www/html/img/ 且没有 nginx alias，则需要建立：
    //   /var/www/html/img/capture-tiles  ->  /dev/shm/capture-tiles
    // 这样无需额外改 nginx，就能直接访问到 tmpfs 瓦片文件。
    {
        const QString imgRoot = QString::fromStdString(vueImagePath);              // e.g. /var/www/html/img/
        QString target = QString::fromStdString(tilePyramidPath);                 // e.g. /dev/shm/capture-tiles/
        if (!target.endsWith('/')) target += '/';
        const QString linkPath = imgRoot + QStringLiteral("capture-tiles");       // e.g. /var/www/html/img/capture-tiles

        // 1) 确保目标目录存在（tmpfs）
        if (!target.isEmpty()) {
            QDir tdir(target);
            if (!tdir.exists()) {
                if (QDir().mkpath(target)) {
                    Logger::Log("Tile tiles root created: " + target.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                } else {
                    Logger::Log("Failed to create tile tiles root: " + target.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                }
            }
        }

        // 2) 建立/校验符号链接
        if (!imgRoot.isEmpty() && !target.isEmpty()) {
            QFileInfo fi(linkPath);
            if (fi.exists() || fi.isSymLink()) {
                if (fi.isSymLink()) {
                    const QString cur = fi.symLinkTarget();
                    // 不强制完全相等（可能存在无尾斜杠），做一次归一化比较
                    QString curNorm = cur;
                    if (!curNorm.endsWith('/')) curNorm += '/';
                    if (curNorm == target) {
                        Logger::Log("Tile mapping OK: " + linkPath.toStdString() + " -> " + target.toStdString(),
                                    LogLevel::INFO, DeviceType::MAIN);
                    } else {
                        // 尝试修复为正确目标（-sfn：覆盖旧链接）
                        const int rc = QProcess::execute(QStringLiteral("ln"),
                                                        QStringList() << QStringLiteral("-sfn") << target << linkPath);
                        if (rc == 0) {
                            Logger::Log("Tile mapping fixed: " + linkPath.toStdString() + " -> " + target.toStdString(),
                                        LogLevel::INFO, DeviceType::MAIN);
                        } else {
                            Logger::Log("Tile mapping exists but points elsewhere: " + linkPath.toStdString() +
                                            " -> " + cur.toStdString() + " (expected " + target.toStdString() + "), rc=" + std::to_string(rc),
                                        LogLevel::WARNING, DeviceType::MAIN);
                        }
                    }
                } else {
                    // 已存在非 symlink 的目录/文件：不做破坏性操作，只提示
                    Logger::Log("Tile mapping not created because path already exists (not symlink): " + linkPath.toStdString() +
                                    " . Consider removing it or configure nginx alias: /img/capture-tiles/ -> " + target.toStdString(),
                                LogLevel::WARNING, DeviceType::MAIN);
                }
            } else {
                // linkPath 不存在：创建符号链接
                QDir().mkpath(QFileInfo(linkPath).absolutePath());
                const int rc = QProcess::execute(QStringLiteral("ln"),
                                                QStringList() << QStringLiteral("-sfn") << target << linkPath);
                if (rc == 0) {
                    Logger::Log("Tile mapping created: " + linkPath.toStdString() + " -> " + target.toStdString(),
                                LogLevel::INFO, DeviceType::MAIN);
                } else {
                    Logger::Log("Failed to create tile mapping: " + linkPath.toStdString() + " -> " + target.toStdString() +
                                    ", rc=" + std::to_string(rc) + ". Permission issue? (need write access to " + imgRoot.toStdString() + ")",
                                LogLevel::WARNING, DeviceType::MAIN);
                }
            }
        }
    }

    // ========================= /img/downloads 映射自检（下载软链目录） =========================
    // 前端会通过 HTTP 请求 /img/downloads/<token>/<type>/<relPath>
    // 后端会在 vueImagePath/downloads/<token>/... 下生成一批软链接（或兜底复制）。
    // 这里将 <imgRoot>/downloads 映射到 QT 的“图像保存根目录”下的固定路径，确保一致性：
    //   <imgRoot>/downloads  ->  <ImageSaveBasePath>/downloads/
    // 注意：ImageSaveBasePath 可能是相对路径，因此这里会转换为当前工作目录下的绝对路径后再建链接。
    // 如需指定其它固定目录，可通过环境变量 QUARCS_DOWNLOADS_ROOT 覆盖：
    //   export QUARCS_DOWNLOADS_ROOT="/path/to/downloads/"
    {
        const QString imgRoot = QString::fromStdString(vueImagePath);              // e.g. /var/www/html/img/
        // 默认：跟随 QT 图像保存根目录（固定落盘位置）
        QString base = QString::fromStdString(ImageSaveBasePath);
        if (!QDir::isAbsolutePath(base))
            base = QDir::cleanPath(QDir::current().absoluteFilePath(base));
        QString target = QDir::cleanPath(base + "/downloads/");
        if (const char *env = std::getenv("QUARCS_DOWNLOADS_ROOT"))
        {
            const std::string v(env);
            if (!v.empty())
            {
                target = QString::fromStdString(v);
                Logger::Log("MainWindow | QUARCS_DOWNLOADS_ROOT=" + v, LogLevel::INFO, DeviceType::MAIN);
            }
        }
        if (!target.endsWith('/')) target += '/';
        const QString linkPath = imgRoot + QStringLiteral("downloads");            // e.g. /var/www/html/img/downloads

        // 1) 确保目标目录存在
        if (!target.isEmpty())
        {
            QDir tdir(target);
            if (!tdir.exists())
            {
                if (QDir().mkpath(target))
                {
                    Logger::Log("Downloads root created: " + target.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                }
                else
                {
                    Logger::Log("Failed to create downloads root: " + target.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                }
            }
        }

        // 2) 建立/校验符号链接
        if (!imgRoot.isEmpty() && !target.isEmpty())
        {
            QFileInfo fi(linkPath);
            if (fi.exists() || fi.isSymLink())
            {
                if (fi.isSymLink())
                {
                    const QString cur = fi.symLinkTarget();
                    QString curNorm = cur;
                    if (!curNorm.endsWith('/')) curNorm += '/';
                    if (curNorm == target)
                    {
                        Logger::Log("Downloads mapping OK: " + linkPath.toStdString() + " -> " + target.toStdString(),
                                    LogLevel::INFO, DeviceType::MAIN);
                    }
                    else
                    {
                        const int rc = QProcess::execute(QStringLiteral("ln"),
                                                        QStringList() << QStringLiteral("-sfn") << target << linkPath);
                        if (rc == 0)
                        {
                            Logger::Log("Downloads mapping fixed: " + linkPath.toStdString() + " -> " + target.toStdString(),
                                        LogLevel::INFO, DeviceType::MAIN);
                        }
                        else
                        {
                            Logger::Log("Downloads mapping exists but points elsewhere: " + linkPath.toStdString() +
                                            " -> " + cur.toStdString() + " (expected " + target.toStdString() + "), rc=" + std::to_string(rc),
                                        LogLevel::WARNING, DeviceType::MAIN);
                        }
                    }
                }
                else
                {
                    // 已存在非 symlink 的目录/文件：不做破坏性操作，只提示
                    Logger::Log("Downloads mapping not created because path already exists (not symlink): " + linkPath.toStdString() +
                                    " . Consider removing it or configure web server to serve it.",
                                LogLevel::WARNING, DeviceType::MAIN);
                }
            }
            else
            {
                QDir().mkpath(QFileInfo(linkPath).absolutePath());
                const int rc = QProcess::execute(QStringLiteral("ln"),
                                                QStringList() << QStringLiteral("-sfn") << target << linkPath);
                if (rc == 0)
                {
                    Logger::Log("Downloads mapping created: " + linkPath.toStdString() + " -> " + target.toStdString(),
                                LogLevel::INFO, DeviceType::MAIN);
                }
                else
                {
                    Logger::Log("Failed to create downloads mapping: " + linkPath.toStdString() + " -> " + target.toStdString() +
                                    ", rc=" + std::to_string(rc) + ". Permission issue? (need write access to " + imgRoot.toStdString() + ")",
                                LogLevel::WARNING, DeviceType::MAIN);
                }
            }
        }
    }

    wsThread = new WebSocketThread(websockethttpUrl, websockethttpsUrl);
    connect(wsThread, &WebSocketThread::receivedMessage, this, &MainWindow::onMessageReceived);
    wsThread->start();
    Logger::wsThread = wsThread;

    // 记住当前实例
    instance = this;

    // 安装自定义的消息处理器
    // qInstallMessageHandler(customMessageHandler);

    initINDIServer();
    initINDIClient();

    initGPIO();

    readDriversListFromFiles("/usr/share/indi/drivers.xml", drivers_list, dev_groups, devices);

    Tools::InitSystemDeviceList();
    Tools::initSystemDeviceList(systemdevicelist);
    Tools::makeConfigFile();
    // 取消：不再读取/保存中天翻转持久化状态（统一使用几何法 needsFlip）
    Tools::makeImageFolder();
    connect(Tools::getInstance(), &Tools::parseInfoEmitted, this, &MainWindow::onParseInfoEmitted);

    // 🆕 初始化 SDK 驱动注册表（在所有驱动自动注册后）
    // 必须在静态初始化完成后调用，用于构建 INDI 驱动名到 SDK 驱动名的映射
    SdkDriverRegistry::instance().initialize();
    Logger::Log("MainWindow | SDK driver registry initialized", LogLevel::INFO, DeviceType::MAIN);

    m_thread = new QThread;
    m_threadTimer = new QTimer;
    m_threadTimer->setInterval(200);
    m_threadTimer->moveToThread(m_thread);
    connect(m_threadTimer, &QTimer::timeout, this, &MainWindow::onTimeout);
    connect(m_thread, &QThread::finished, m_threadTimer, &QTimer::stop);
    connect(m_thread, &QThread::destroyed, m_threadTimer, &QTimer::deleteLater);
    connect(m_thread, &QThread::started, m_threadTimer, QOverload<>::of(&QTimer::start));
    m_thread->start();

    // 导星相机循环曝光（INDI 直出图）：使用 singleShot，收到一帧后再调度下一帧，避免重入
    guiderLoopTimer = new QTimer(this);
    guiderLoopTimer->setSingleShot(true);
    connect(guiderLoopTimer, &QTimer::timeout, this, &MainWindow::onGuiderLoopTimeout);
    guiderCoreThread = new QThread(this);
    guiderCoreThread->setObjectName(QStringLiteral("GuiderCoreThread"));
    qRegisterMetaType<cv::Mat>("cv::Mat");
    qRegisterMetaType<QVector<QPointF>>("QVector<QPointF>");
    qRegisterMetaType<QVector<QString>>("QVector<QString>");
    qRegisterMetaType<QPointF>("QPointF");
    guiderCore = new GuiderCore();
    guiderParamsCache = guiderCore->params();
    {
        std::unordered_map<std::string, std::string> config;
        Tools::readClientSettings("config/config.ini", config);
        const auto it = config.find("GuiderExposureMs");
        if (it != config.end())
        {
            bool ok = false;
            const int savedGuiderExpMs = QString::fromStdString(it->second).trimmed().toInt(&ok);
            if (ok && savedGuiderExpMs > 0)
            {
                guiderExpMs = savedGuiderExpMs;
                guiderParamsCache.exposureMs = savedGuiderExpMs;
                guiderCore->setParams(guiderParamsCache);
                Logger::Log("BuiltInGuider | restored GuiderExposureMs=" +
                                std::to_string(savedGuiderExpMs),
                            LogLevel::INFO, DeviceType::GUIDER);
            }
        }
    }
    guiderCoreStateCache = guiderCore->state();
    guiderCore->moveToThread(guiderCoreThread);
    connect(guiderCoreThread, &QThread::finished, guiderCore, &QObject::deleteLater);
    guiderCoreThread->start();
    Logger::Log("BuiltInGuider | GuiderCore moved to GuiderCoreThread",
                LogLevel::INFO, DeviceType::GUIDER);
    syncGuiderScaleParams(true, false);
    publishGuiderSearchBoxMode(false);
#if QUARCS_SIM_GUIDER
    simGuiderFrameSource = std::make_unique<guiding::SimGuiderFrameSource>();
    Logger::Log("BuiltInGuider | simulated frame source enabled", LogLevel::INFO, DeviceType::GUIDER);
#endif
    connect(guiderCore, &GuiderCore::requestExposure, this, [this](int exposureMs) {
        guiderExpMs = std::max(1, exposureMs);
        isGuiderLoopExp = true;
        guiderExposureInFlight = false;
        emit wsThread->sendMessageToClient("GuiderLoopExpStatus:true");
        emit wsThread->sendMessageToClient("GuiderUpdateStatus:1");
        if (guiderLoopTimer)
            guiderLoopTimer->start(0);
    });
    connect(guiderCore, &GuiderCore::requestPersistGuidingFits, this, &MainWindow::PersistGuidingFits);
    connect(guiderCore, &GuiderCore::requestPersistGuidingFitsAnnotated, this,
            [this](const QString& sourceFitsPath, const cv::Mat& image16, int imageW, int imageH,
                   const QVector<QPointF>& dedupCandidates,
                   const QVector<QPointF>& snrCandidates,
                   const QVector<QPointF>& candidates,
                   const QVector<QString>& candidateLabels,
                   const QPointF& selected) {
        m_debugStarDedupCandidates = dedupCandidates;
        m_debugStarSnrCandidates = snrCandidates;
        m_debugStarCandidates = candidates;
        m_debugStarCandidateLabels = candidateLabels;
        m_debugStarSelected = selected;
        Logger::Log("BuiltInGuider | requestPersistGuidingFitsAnnotated received in MainWindow: image=" +
                        std::to_string(imageW) + "x" + std::to_string(imageH) +
                        " dedupCandidates=" + std::to_string(dedupCandidates.size()) +
                        " snrCandidates=" + std::to_string(snrCandidates.size()) +
                        " candidates=" + std::to_string(candidates.size()) +
                        " selected=" + std::to_string((selected.x() != 0.0 || selected.y() != 0.0) ? 1 : 0),
                    LogLevel::INFO, DeviceType::GUIDER);
        PersistGuidingPreviewFromFrame(sourceFitsPath, image16);
    }, Qt::BlockingQueuedConnection);
    connect(guiderCore, &GuiderCore::requestPulse, this, [this](const guiding::PulseCommand& cmd) {
        ControlGuideEx(static_cast<int>(cmd.dir), cmd.durationMs, QStringLiteral("BuiltInGuider"));
    });
    connect(guiderCore, &GuiderCore::lockPositionChanged, this, [this](const QPointF& lockPosPx) {
        guiderLockPosPx = lockPosPx;
        guiderLockPosValid = true;
        glPHD_CurrentImageSizeX = std::max(1, glPHD_CurrentImageSizeX);
        glPHD_CurrentImageSizeY = std::max(1, glPHD_CurrentImageSizeY);
        emit wsThread->sendMessageToClient("PHD2StarCrossView:true");
        emit wsThread->sendMessageToClient(
            "PHD2StarCrossPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
            QString::number(glPHD_CurrentImageSizeY) + ":" +
            QString::number(static_cast<int>(std::lround(lockPosPx.x()))) + ":" +
            QString::number(static_cast<int>(std::lround(lockPosPx.y()))));
    });
    connect(guiderCore, &GuiderCore::lockStarSelected, this, [this](double x, double y, double snr, double hfd) {
        const int searchHalfSizePx = std::max(4, guiderParamsCache.guideSearchHalfSizePx);
        glPHD_CurrentImageSizeX = std::max(1, glPHD_CurrentImageSizeX);
        glPHD_CurrentImageSizeY = std::max(1, glPHD_CurrentImageSizeY);
        emit wsThread->sendMessageToClient("PHD2StarBoxView:true");
        emit wsThread->sendMessageToClient(
            "PHD2StarBoxPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
            QString::number(glPHD_CurrentImageSizeY) + ":" +
            QString::number(static_cast<int>(std::lround(x))) + ":" +
            QString::number(static_cast<int>(std::lround(y))) + ":" +
            QString::number(searchHalfSizePx));
        emit wsThread->sendMessageToClient(
            "GuiderSelectedStar:" +
            QString::number(x, 'f', 2) + ":" +
            QString::number(y, 'f', 2) + ":" +
            QString::number(snr, 'f', 2) + ":" +
            QString::number(hfd, 'f', 2));
        emit wsThread->sendMessageToClient(
            "GuiderStarSelected:x=" +
            QString::number(x, 'f', 2) + ":y=" +
            QString::number(y, 'f', 2) + ":snr=" +
            QString::number(snr, 'f', 2) + ":hfd=" +
            QString::number(hfd, 'f', 2));
        emit wsThread->sendMessageToClient(
            QStringLiteral("SendDebugMessage|Info|导星选星成功：X=%1 Y=%2 SNR=%3 HFD=%4")
                .arg(x, 0, 'f', 2)
                .arg(y, 0, 'f', 2)
                .arg(snr, 0, 'f', 2)
                .arg(hfd, 0, 'f', 2));
    });
    // DEBUG: publish current frame's star candidates so the frontend can draw
    // color overlays on top of the guider preview.
    connect(guiderCore, &GuiderCore::debugStarCandidatesChanged, this,
            [this](int imageW, int imageH, const QVector<QPointF>& dedupCandidates,
                   const QVector<QPointF>& snrCandidates,
                   const QVector<QPointF>& candidates,
                   const QVector<QString>& candidateLabels,
                   const QPointF& selected) {
        m_debugStarDedupCandidates = dedupCandidates;
        m_debugStarSnrCandidates = snrCandidates;
        m_debugStarCandidates = candidates;
        m_debugStarCandidateLabels = candidateLabels;
        m_debugStarSelected = selected;
        Logger::Log("BuiltInGuider | debugStarCandidatesChanged received in MainWindow: image=" +
                        std::to_string(imageW) + "x" + std::to_string(imageH) +
                        " dedupCandidates=" + std::to_string(dedupCandidates.size()) +
                        " snrCandidates=" + std::to_string(snrCandidates.size()) +
                        " candidates=" + std::to_string(candidates.size()) +
                        " selected=" + std::to_string((selected.x() != 0.0 || selected.y() != 0.0) ? 1 : 0),
                    LogLevel::INFO, DeviceType::GUIDER);

        emit wsThread->sendMessageToClient("ClearGuiderDebugCandidates");

        const int safeImageW = std::max(1, imageW);
        const int safeImageH = std::max(1, imageH);
        const int maxCandidatesToSend = 48;
        for (int i = 0; i < dedupCandidates.size() && i < maxCandidatesToSend; ++i)
        {
            const auto& pt = dedupCandidates[i];
            emit wsThread->sendMessageToClient(
                "GuiderDebugDedupCandidatePosition:" + QString::number(safeImageW) + ":" +
                QString::number(safeImageH) + ":" +
                QString::number(static_cast<int>(std::lround(pt.x()))) + ":" +
                QString::number(static_cast<int>(std::lround(pt.y()))));
        }

        for (int i = 0; i < snrCandidates.size() && i < maxCandidatesToSend; ++i)
        {
            const auto& pt = snrCandidates[i];
            emit wsThread->sendMessageToClient(
                "GuiderDebugSnrCandidatePosition:" + QString::number(safeImageW) + ":" +
                QString::number(safeImageH) + ":" +
                QString::number(static_cast<int>(std::lround(pt.x()))) + ":" +
                QString::number(static_cast<int>(std::lround(pt.y()))));
        }

        for (int i = 0; i < candidates.size() && i < maxCandidatesToSend; ++i)
        {
            const auto& pt = candidates[i];
            const QString label = (i < candidateLabels.size()) ? candidateLabels[i] : QString();
            emit wsThread->sendMessageToClient(
                "GuiderDebugCandidatePosition:" + QString::number(safeImageW) + ":" +
                QString::number(safeImageH) + ":" +
                QString::number(static_cast<int>(std::lround(pt.x()))) + ":" +
                QString::number(static_cast<int>(std::lround(pt.y()))));
            emit wsThread->sendMessageToClient(
                "GuiderDebugFinalCandidatePosition:" + QString::number(safeImageW) + ":" +
                QString::number(safeImageH) + ":" +
                QString::number(static_cast<int>(std::lround(pt.x()))) + ":" +
                QString::number(static_cast<int>(std::lround(pt.y()))) + ":" +
                label);
        }

        if (selected.x() != 0.0 || selected.y() != 0.0)
        {
            emit wsThread->sendMessageToClient(
                "GuiderDebugSelectedCandidatePosition:" + QString::number(safeImageW) + ":" +
                QString::number(safeImageH) + ":" +
                QString::number(static_cast<int>(std::lround(selected.x()))) + ":" +
                QString::number(static_cast<int>(std::lround(selected.y()))));
        }
    });
    connect(guiderCore, &GuiderCore::guideStarCentroidChanged, this, [this](const QPointF& centroidPx) {
        const int searchHalfSizePx = std::max(4, guiderParamsCache.guideSearchHalfSizePx);
        guiderGuideStarCentroidPx = centroidPx;
        guiderGuideStarCentroidValid = true;
        glPHD_CurrentImageSizeX = std::max(1, glPHD_CurrentImageSizeX);
        glPHD_CurrentImageSizeY = std::max(1, glPHD_CurrentImageSizeY);
        emit wsThread->sendMessageToClient("PHD2StarBoxView:true");
        emit wsThread->sendMessageToClient(
            "PHD2StarBoxPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
            QString::number(glPHD_CurrentImageSizeY) + ":" +
            QString::number(static_cast<int>(std::lround(centroidPx.x()))) + ":" +
            QString::number(static_cast<int>(std::lround(centroidPx.y()))) + ":" +
            QString::number(searchHalfSizePx));
    });
    connect(guiderCore, &GuiderCore::multiStarSecondaryPointsChanged, this, [this](const QVector<QPointF>& ptsPx) {
        guiderMultiStarSecondaryPtsPx = ptsPx;
        guiderMultiStarSecondaryPtsPending = !ptsPx.isEmpty();

        emit wsThread->sendMessageToClient("ClearPHD2MultiStars");
        if (ptsPx.isEmpty())
            return;

        if (glPHD_CurrentImageSizeX <= 0 || glPHD_CurrentImageSizeY <= 0 || !guiderPhaseGuiding || guiderDirectionDetectActive)
            return;

        for (int i = 0; i < ptsPx.size(); ++i)
        {
            if (i >= 8) break;
            const auto& p = ptsPx[i];
            emit wsThread->sendMessageToClient(
                "PHD2MultiStarsPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
                QString::number(glPHD_CurrentImageSizeY) + ":" +
                QString::number(static_cast<int>(std::lround(p.x()))) + ":" +
                QString::number(static_cast<int>(std::lround(p.y()))));
        }
        guiderMultiStarSecondaryPtsPending = false;
    });
    connect(guiderCore, &GuiderCore::directionDetectionStateChanged, this, [this](bool active) {
        guiderDirectionDetectActive = active;
        emit wsThread->sendMessageToClient(QString("GuiderStatus:%1")
                                               .arg(active ? "InDirectionDetection"
                                                           : (guiderPhaseGuiding ? "InGuiding" : "InCalibration")));
    });
    connect(guiderCore, &GuiderCore::guideErrorUpdated, this, [this](double raErrPx, double decErrPx) {
        const double scale = currentGuiderArcsecPerPixel();
        const double raErr = (scale > 0.0) ? (raErrPx * scale) : raErrPx;
        const double decErr = (scale > 0.0) ? (decErrPx * scale) : decErrPx;
        emit wsThread->sendMessageToClient(
            "AddLineChartData:" + QString::number(guiderChartSampleIndex++) + ":" +
            QString::number(raErr, 'f', 6) + ":" +
            QString::number(decErr, 'f', 6));
        // Keep the scatter plot fed from the same real-time guider error stream.
        // Match the legacy scatter convention by flipping Y when drawing dX/dY points.
        emit wsThread->sendMessageToClient(
            "AddScatterChartData:" + QString::number(raErr, 'f', 6) + ":" +
            QString::number(-decErr, 'f', 6));
    });
    connect(guiderCore, &GuiderCore::guidePulseIssued, this,
            [this](const guiding::PulseCommand& cmd, double raErrPx, double decErrPx) {
        const QString dir =
            (cmd.dir == guiding::GuideDir::North) ? QStringLiteral("NORTH") :
            (cmd.dir == guiding::GuideDir::South) ? QStringLiteral("SOUTH") :
            (cmd.dir == guiding::GuideDir::East)  ? QStringLiteral("EAST")  :
                                                    QStringLiteral("WEST");
        emit wsThread->sendMessageToClient(
            "GuiderPulse:" + dir + ":" + QString::number(cmd.durationMs) +
            ":raErrPx=" + QString::number(raErrPx, 'f', 6) +
            ":decErrPx=" + QString::number(decErrPx, 'f', 6));
    });
    connect(guiderCore, &GuiderCore::calibrationResultChanged, this, [this](const guiding::CalibrationResult& r) {
        emit wsThread->sendMessageToClient(
            QStringLiteral("GuiderCalibration:cameraAngleDeg=") + QString::number(r.cameraAngleDeg, 'f', 2) +
            ":orthoErrDeg=" + QString::number(r.orthoErrDeg, 'f', 2) +
            ":raMsPerPixel=" + QString::number(r.raMsPerPixel, 'f', 2) +
            ":decMsPerPixel=" + QString::number(r.decMsPerPixel, 'f', 2) +
            ":raSteps=" + QString::number(r.raStepCount) +
            ":decSteps=" + QString::number(r.decStepCount) +
            ":raTravelPx=" + QString::number(r.raTravelPx, 'f', 2) +
            ":decTravelPx=" + QString::number(r.decTravelPx, 'f', 2));
    });
    connect(guiderCore, &GuiderCore::stateChanged, this, [this](guiding::State state) {
        guiderCoreStateCache = state;
        guiderPhaseGuiding = (state == guiding::State::Guiding);
        if (state == guiding::State::Idle || state == guiding::State::Looping ||
            state == guiding::State::Stopped || state == guiding::State::Error)
        {
            guiderDirectionDetectActive = false;
            guiderChartSampleIndex = 0;
            guiderLockPosValid = false;
            guiderGuideStarCentroidValid = false;
            isGuiderLoopExp = false;
            guiderExposureInFlight = false;
            if (guiderLoopTimer)
                guiderLoopTimer->stop();
            if (sdkGuiderExposureTimer)
                sdkGuiderExposureTimer->stop();
            emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
            emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
            emit wsThread->sendMessageToClient("PHD2StarBoxView:false");
            emit wsThread->sendMessageToClient("PHD2StarCrossView:false");
            emit wsThread->sendMessageToClient("ClearPHD2MultiStars");
            stopGuiderAutoBatchCapture();
            clearGuiderDebugAnnotations(state == guiding::State::Stopped);
        }
        emit wsThread->sendMessageToClient(QString("GuiderCoreState:%1").arg(static_cast<int>(state)));
        switch (state)
        {
        case guiding::State::Selecting:
            emit wsThread->sendMessageToClient("GuiderSwitchStatus:true");
            emit wsThread->sendMessageToClient("GuiderStatus:InSelecting");
            emit wsThread->sendMessageToClient("SendDebugMessage|Info|导星正在自动选星，请稍候");
            break;
        case guiding::State::Calibrating:
            emit wsThread->sendMessageToClient("GuiderSwitchStatus:true");
            emit wsThread->sendMessageToClient("GuiderStatus:InCalibration");
            emit wsThread->sendMessageToClient("SendDebugMessage|Info|导星已锁定星点，正在校准");
            break;
        case guiding::State::Guiding:
            emit wsThread->sendMessageToClient("GuiderSwitchStatus:true");
            emit wsThread->sendMessageToClient("GuiderStatus:InGuiding");
            emit wsThread->sendMessageToClient("SendDebugMessage|Info|导星已进入闭环导星");
            break;
        case guiding::State::Idle:
        case guiding::State::Looping:
        case guiding::State::Stopped:
        case guiding::State::Error:
        default:
            emit wsThread->sendMessageToClient("GuiderSwitchStatus:false");
            break;
        }
    });
    connect(guiderCore, &GuiderCore::infoMessage, this, [this](const QString& msg) {
        Logger::Log("BuiltInGuider | " + msg.toStdString(), LogLevel::INFO, DeviceType::GUIDER);
        emit wsThread->sendMessageToClient("GuiderCoreInfo:" + msg);
        if (msg.contains(QStringLiteral("选星成功")))
        {
            emit wsThread->sendMessageToClient("GuiderStatus:StarSelected");
            emit wsThread->sendMessageToClient("SendDebugMessage|Info|" + msg);
        }
        else if (msg.contains(QStringLiteral("自动选星进行中")) ||
                 msg.contains(QStringLiteral("等待下一帧")) ||
                 msg.contains(QStringLiteral("候选星点")))
        {
            emit wsThread->sendMessageToClient("GuiderStatus:SelectingProgress");
            emit wsThread->sendMessageToClient("SendDebugMessage|Info|" + msg);
        }
        else if (msg.contains(QStringLiteral("进入校准阶段")) ||
                 msg.contains(QStringLiteral("校准完成")))
        {
            emit wsThread->sendMessageToClient("SendDebugMessage|Info|" + msg);
        }
    });
    connect(guiderCore, &GuiderCore::errorOccurred, this, [this](const QString& msg) {
        Logger::Log("BuiltInGuider | " + msg.toStdString(), LogLevel::WARNING, DeviceType::GUIDER);
        emit wsThread->sendMessageToClient("GuiderCoreError:" + msg);
        emit wsThread->sendMessageToClient("ErrorMessage:导星错误 - " + msg);
        emit wsThread->sendMessageToClient("SendDebugMessage|Warning|导星错误：" + msg);
        if (msg.contains(QStringLiteral("LostStar"), Qt::CaseInsensitive) ||
            msg.contains(QStringLiteral("丢星"), Qt::CaseInsensitive))
        {
            emit wsThread->sendMessageToClient("GuiderStatus:StarLostAlert");
        }
    });
    // getConnectedSerialPorts();

    // 电调控制初始化
    focusMoveTimer = new QTimer(this);
    connect(focusMoveTimer, &QTimer::timeout, this, &MainWindow::HandleFocuserMovementDataPeriodically);

    // SDK 曝光定时器初始化
    sdkExposureTimer = new QTimer(this);
    sdkExposureTimer->setSingleShot(false); // 允许多次触发
    connect(sdkExposureTimer, &QTimer::timeout, this, &MainWindow::onSdkExposureTimerTimeout);

    // SDK 主相机 Live 循环取帧定时器初始化（仅 QHYCCD SDK 使用）
    sdkMainLiveTimer = new QTimer(this);
    sdkMainLiveTimer->setSingleShot(false);
    // 默认 33ms (~30fps)。实际出帧速度由相机曝光/传输决定。
    sdkMainLiveTimer->setInterval(33);
    connect(sdkMainLiveTimer, &QTimer::timeout, this, &MainWindow::onSdkMainLiveTimerTimeout);

    // SDK 主相机 Live 后处理定时器（主线程）：从“最新帧邮箱”取最新帧做 FITS/PNG/瓦片
    // 说明：取帧与处理彻底解耦，处理慢只会丢中间帧，不影响 SDK 拉帧与 FPS 统计
    sdkMainLiveProcessTimer = new QTimer(this);
    sdkMainLiveProcessTimer->setSingleShot(false);
    // 频率不用很高；真正的限帧由 sdkMainLiveMaxProcessFps 控制
    sdkMainLiveProcessTimer->setInterval(50);
    connect(sdkMainLiveProcessTimer, &QTimer::timeout, this, &MainWindow::onSdkMainLiveProcessTimerTimeout);

    // SDK 导星曝光定时器初始化（独立于主相机 SDK 曝光）
    sdkGuiderExposureTimer = new QTimer(this);
    sdkGuiderExposureTimer->setSingleShot(false);
    connect(sdkGuiderExposureTimer, &QTimer::timeout, this, &MainWindow::onSdkGuiderExposureTimerTimeout);

    // SDK 串行执行线程（所有阻塞式 SDK 调用都应投递到对应线程）
    // 每个相机独立通道，避免某个设备的阻塞读帧拖住其它相机。
    sdkCamExec = std::make_unique<SdkSerialExecutor>(QStringLiteral("SdkCamWorker"));
    sdkMainCamExec = std::make_unique<SdkSerialExecutor>(QStringLiteral("SdkMainCameraWorker"));
    sdkGuiderCamExec = std::make_unique<SdkSerialExecutor>(QStringLiteral("SdkGuiderCameraWorker"));
    sdkPoleCamExec = std::make_unique<SdkSerialExecutor>(QStringLiteral("SdkPoleCameraWorker"));
    sdkFocuserExec = std::make_unique<SdkSerialExecutor>(QStringLiteral("SdkFocuserWorker"));

    emit wsThread->sendMessageToClient("ServerInitSuccess");
    Logger::Log("ServerInitSuccess", LogLevel::INFO, DeviceType::MAIN);

}

MainWindow::~MainWindow()
{
    // 停止可能触发 SDK 调用的定时器，避免析构期间还有回调进来
    if (sdkExposureTimer)
        sdkExposureTimer->stop();
    if (sdkMainLiveTimer)
        sdkMainLiveTimer->stop();
    if (sdkMainLiveProcessTimer)
        sdkMainLiveProcessTimer->stop();
    if (sdkGuiderExposureTimer)
        sdkGuiderExposureTimer->stop();
    if (focusMoveTimer)
        focusMoveTimer->stop();

    if (guiderCoreThread && guiderCoreThread->isRunning() && guiderCore)
    {
        QMetaObject::invokeMethod(guiderCore, [core = guiderCore]() {
            core->stopGuiding();
            core->stopLoop();
        }, Qt::BlockingQueuedConnection);
        guiderCoreThread->quit();
        guiderCoreThread->wait(3000);
        guiderCore = nullptr;
    }

    // 析构兜底：若仍有 SDK 设备/句柄存在，统一关闭句柄并释放 SDK 全局资源
    //（例如用户直接关闭程序而未点“断开所有设备”）
    cleanupQhySdkPoolAndResource("MainWindow::~MainWindow", "All");

    // 停止 SDK 线程（会等待当前任务结束），避免后台任务回调访问已析构对象
    sdkPoleCamExec.reset();
    sdkGuiderCamExec.reset();
    sdkMainCamExec.reset();
    sdkCamExec.reset();
    sdkFocuserExec.reset();

    // 释放 Live 共享内存映射（确保 SDK 线程已停，避免并发写导致崩溃）
    cleanupSdkMainLiveShm();

    // 清理极轴校准对象
    if (polarAlignment != nullptr)
    {
        polarAlignment->stopPolarAlignment();
        delete polarAlignment;
        polarAlignment = nullptr;
    }

    system("pkill indiserver");
    system("rm -f /tmp/myFIFO");


    // 先断开 Logger -> WebSocket 的弱引用，避免析构过程中/析构后仍有后台线程写日志时触发野指针
    Logger::wsThread = nullptr;

    wsThread->quit();
    wsThread->wait();
    delete wsThread;
    wsThread = nullptr;

    // 清理静态实例
    instance = nullptr;
}

/**
 * @brief 判断主相机是否为 SDK 模式
 * @return SDK 模式返回 true，INDI 模式返回 false
 */
bool MainWindow::isMainCameraSDK()
{
    return (systemdevicelist.system_devices.size() > 20 &&
            systemdevicelist.system_devices[20].isSDKConnect &&
            sdkMainCameraHandle != nullptr);
}

/**
 * @brief 判断主相机是否已连接（支持 SDK 和 INDI 两种模式）
 * @return 已连接返回 true，否则返回 false
 */
bool MainWindow::isMainCameraConnected()
{
    // SDK 模式：检查 SDK 句柄是否有效
    if (systemdevicelist.system_devices.size() > 20 &&
        systemdevicelist.system_devices[20].isSDKConnect)
    {
        return sdkMainCameraHandle != nullptr;
    }
    
    // INDI 模式：检查 INDI 设备指针是否有效
    return dpMainCamera != nullptr;
}

bool MainWindow::isGuiderCameraSDK() const
{
    return (systemdevicelist.system_devices.size() > 1 &&
            systemdevicelist.system_devices[1].isSDKConnect &&
            sdkGuiderHandle != nullptr);
}

bool MainWindow::isGuiderCameraConnected() const
{
    if (systemdevicelist.system_devices.size() > 1 &&
        systemdevicelist.system_devices[1].isSDKConnect)
    {
        return sdkGuiderHandle != nullptr;
    }
    return dpGuider != nullptr;
}

bool MainWindow::isPoleCameraSDK() const
{
    return (systemdevicelist.system_devices.size() > 2 &&
            systemdevicelist.system_devices[2].isSDKConnect &&
            sdkPoleScopeHandle != nullptr);
}

bool MainWindow::isPoleCameraConnected() const
{
    if (systemdevicelist.system_devices.size() > 2 &&
        systemdevicelist.system_devices[2].isSDKConnect)
    {
        return sdkPoleScopeHandle != nullptr;
    }
    return dpPoleScope != nullptr;
}

SdkSerialExecutor* MainWindow::sdkMainCameraExecutor() const
{
    return sdkMainCamExec.get();
}

SdkSerialExecutor* MainWindow::sdkGuiderCameraExecutor() const
{
    return sdkGuiderCamExec.get();
}

SdkSerialExecutor* MainWindow::sdkPoleCameraExecutor() const
{
    return sdkPoleCamExec.get();
}

SdkSerialExecutor* MainWindow::sdkExecutorForPolarRole(PolarAlignmentCameraRole role) const
{
    switch (role)
    {
    case PolarAlignmentCameraRole::MainCamera:
        return sdkMainCameraExecutor();
    case PolarAlignmentCameraRole::Guider:
        return sdkGuiderCameraExecutor();
    case PolarAlignmentCameraRole::PoleCamera:
        return sdkPoleCameraExecutor();
    }
    return nullptr;
}

MainWindow::PolarAlignmentCameraRole MainWindow::parsePolarAlignmentCameraRole(const QString &roleText)
{
    const QString normalized = roleText.trimmed().toLower();
    if (normalized == "guider")
        return PolarAlignmentCameraRole::Guider;
    if (normalized == "polecamera" || normalized == "pole" || normalized == "polescope")
        return PolarAlignmentCameraRole::PoleCamera;
    return PolarAlignmentCameraRole::MainCamera;
}

int MainWindow::getMainCameraFocalLengthFromConfigAndMigrateIfNeeded()
{
    if (glFocalLength > 0)
        return glFocalLength;

    std::unordered_map<std::string, std::string> config;
    Tools::readClientSettings("config/config.ini", config);

    QString mainFocal;
    auto itMain = config.find("MainCameraFocalLength");
    if (itMain != config.end())
    {
        mainFocal = QString::fromStdString(itMain->second).trimmed();
    }
    else
    {
        auto itLegacy = config.find("FocalLength");
        if (itLegacy != config.end())
        {
            mainFocal = QString::fromStdString(itLegacy->second).trimmed();
            if (!mainFocal.isEmpty())
            {
                setClientSettings("MainCameraFocalLength", mainFocal);
                Logger::Log("Migrate focal length config: FocalLength -> MainCameraFocalLength = " +
                                mainFocal.toStdString(),
                            LogLevel::INFO, DeviceType::MAIN);
            }
        }
    }

    bool ok = false;
    const int focal = mainFocal.toInt(&ok);
    if (ok && focal > 0)
    {
        glFocalLength = focal;
        return focal;
    }
    return 0;
}

void MainWindow::notifyPolarAlignmentCaptureReady(PolarAlignmentCameraRole role, const QString &fitsPath)
{
    if (role != currentPolarAlignmentCameraRole)
        return;

    const QString normalizedPath = fitsPath.trimmed();
    if (normalizedPath.isEmpty())
        return;

    if (role == PolarAlignmentCameraRole::PoleCamera && wsThread != nullptr)
    {
        savePoleMasterPreviewAsJPG(normalizedPath);
    }

    if (polarAlignment != nullptr && polarAlignment->isRunning())
    {
        polarAlignment->setCapturedImagePath(normalizedPath);
        polarAlignment->setCaptureEnd(true);
    }
    else if (poleMasterPolarAlignment != nullptr && poleMasterPolarAlignment->isRunning())
    {
        poleMasterPolarAlignment->setCapturedImagePath(normalizedPath);
        poleMasterPolarAlignment->setCaptureEnd(true);
    }
    else
    {
        return;
    }

    Logger::Log("PolarAlignment capture ready | role=" +
                    std::string(polarRoleName(role)) +
                    " | fits=" + normalizedPath.toStdString(),
                LogLevel::INFO, DeviceType::MAIN);
}

/**
 * @brief 判断电调是否为 SDK 模式
 * @return SDK 模式返回 true，INDI 模式返回 false
 */
bool MainWindow::isFocuserSDK()
{
    return (systemdevicelist.system_devices.size() > 22 &&
            systemdevicelist.system_devices[22].isSDKConnect &&
            systemdevicelist.system_devices[22].isBind &&
            sdkFocuserHandle != nullptr);
}

/**
 * @brief 判断赤道仪是否为 SDK 模式
 * @return SDK 模式返回 true，INDI 模式返回 false
 * @note TODO: 需要实现赤道仪SDK模式的支持，目前仅预留接口
 *       需要确认赤道仪在 systemdevicelist.system_devices 中的索引位置
 */
bool MainWindow::isMountSDK()
{
    // TODO: 实现赤道仪SDK模式的检查
    // 需要确认赤道仪在 systemdevicelist.system_devices 中的索引（可能是19）
    // 目前假设索引为19，需要根据实际情况调整
    // return (systemdevicelist.system_devices.size() > 19 &&
    //         systemdevicelist.system_devices[19].isSDKConnect &&
    //         sdkMountHandle != nullptr);
    
    // 暂时返回false，等待实现
    return false;
}

void MainWindow::getHostAddress()
{
    // 默认使用本机回环地址连接本机的 WebSocket Hub（NodeJs-Transponder）。
    // 这样即使局域网 IP 变化，也不会影响 Qt <-> Hub 的连接（避免“换网段就断连”）。
    //
    // 如需指定 Hub 所在主机，可设置环境变量：
    // - QUARCS_WS_HOST=<ip/hostname>  例如：192.168.200.129
    // - QUARCS_WS_HOST=auto           继续使用旧的“自动探测网卡IP”逻辑
    const QByteArray envHost = qgetenv("QUARCS_WS_HOST");
    if (envHost.isEmpty()) {
        const QString host = QStringLiteral("127.0.0.1");
        websockethttpUrl  = QUrl(QStringLiteral("ws://%1:8600").arg(host));
        websockethttpsUrl = QUrl(QStringLiteral("wss://%1:8601").arg(host));
        Logger::Log("WebSocket target (default loopback): " + websockethttpUrl.toString().toStdString(),
                    LogLevel::INFO, DeviceType::MAIN);
        return;
    }

    if (envHost != "auto") {
        const QString host = QString::fromUtf8(envHost);
        websockethttpUrl  = QUrl(QStringLiteral("ws://%1:8600").arg(host));
        websockethttpsUrl = QUrl(QStringLiteral("wss://%1:8601").arg(host));
        Logger::Log("WebSocket target (from QUARCS_WS_HOST): " + websockethttpUrl.toString().toStdString(),
                    LogLevel::INFO, DeviceType::MAIN);
        return;
    }

    int retryCount = 0;
    const int maxRetries = 20;
    const int waitTime = 5000; // 5秒

    while (retryCount < maxRetries)
    {
        QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
        bool found = false;

        foreach (const QNetworkInterface &interface, interfaces)
        {
            // 排除回环接口和非活动接口
            if (interface.flags() & QNetworkInterface::IsLoopBack || !(interface.flags() & QNetworkInterface::IsUp))
                continue;

            QList<QNetworkAddressEntry> addresses = interface.addressEntries();
            foreach (const QNetworkAddressEntry &address, addresses)
            {
                if (address.ip().protocol() == QAbstractSocket::IPv4Protocol)
                {
                    QString localIpAddress = address.ip().toString();
                    Logger::Log("Local IP Address:" + localIpAddress.toStdString(), LogLevel::INFO, DeviceType::MAIN);

                    if (!localIpAddress.isEmpty())
                    {
                        QUrl getUrl(QStringLiteral("ws://%1:8600").arg(localIpAddress));
                        QUrl getUrlHttps(QStringLiteral("wss://%1:8601").arg(localIpAddress));
                        // Logger::Log("WebSocket URL:" + getUrl.toString().toStdString(), LogLevel::INFO, DeviceType::MAIN);
                        websockethttpUrl = getUrl;
                        websockethttpsUrl = getUrlHttps;
                        found = true;
                        break;
                    }
                    else
                    {
                        Logger::Log("Failed to get local IP address.", LogLevel::WARNING, DeviceType::MAIN);
                    }
                }
            }
            if (found)
                break;
        }

        if (found)
            break;

        retryCount++;
        QThread::sleep(waitTime / 1000); // 等待5秒
    }

    if (retryCount == maxRetries)
    {
        qCritical() << "Failed to detect any network interfaces after" << maxRetries << "attempts.";
    }
}

void MainWindow::onMessageReceived(const QString &message)
{
    Logger::Log("Received message in MainWindow:" + message.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);

    QStringList parts = message.split(':');
    QString trimmedMessage = message.trimmed();
    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();

    if (!lastCommandMessage.isEmpty() && lastCommandMessage == trimmedMessage && lastCommandTime > 0)
    {
        qint64 timeDiff = currentTime - lastCommandTime;

        if (timeDiff < COMMAND_DEBOUNCE_MS)
        {
            Logger::Log("Command debounce: Skipping duplicate message '" + trimmedMessage.toStdString() +
                           "' received within " + std::to_string(timeDiff) + "ms (threshold: " +
                           std::to_string(COMMAND_DEBOUNCE_MS) + "ms)",
                       LogLevel::DEBUG, DeviceType::MAIN);
            return;
        }
    }

    lastCommandMessage = trimmedMessage;
    lastCommandTime = currentTime;

    if (handleDriverSelectionCommand(message, parts) ||
        handleBindingCommand(message, parts) ||
        handleCaptureCommand(message, parts) ||
        handleFocuserCommand(message, parts) ||
        handleGuiderCommand(message, parts) ||
        handleMountCommand(message, parts) ||
        handleScheduleCommand(message, parts) ||
        handleFileAndStorageCommand(message, parts) ||
        handleSystemCommand(message, parts))
    {
        return;
    }

    Logger::Log("Unknown message: " + message.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
}

void MainWindow::runSudoAsync(const QString &program, const QStringList &args,
                              const std::function<void(int, const QString &, const QString &)> &onDone)
{
    QProcess *p = new QProcess(this);
    p->setProgram("sudo");
    QStringList a;
    // -n: non-interactive. If sudoers is not configured correctly, fail fast instead of hanging.
    a << "-n";
    a << program;
    a << args;
    p->setArguments(a);

    // Safety timeout: avoid hanging forever on slow nmcli/blocked sudo.
    QTimer *killer = new QTimer(this);
    killer->setSingleShot(true);
    killer->setInterval(15000);
    connect(killer, &QTimer::timeout, this, [p, onDone]() {
        if (p->state() != QProcess::NotRunning) {
            p->kill();
            const QString out = QString::fromUtf8(p->readAllStandardOutput());
            const QString err = QString::fromUtf8(p->readAllStandardError()) + "\n(timeout)";
            if (onDone) onDone(124, out, err);
        }
    });

    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [p, onDone, killer](int exitCode, QProcess::ExitStatus /*status*/) {
                if (killer->isActive()) killer->stop();
                killer->deleteLater();
                const QString out = QString::fromUtf8(p->readAllStandardOutput());
                const QString err = QString::fromUtf8(p->readAllStandardError());
                if (onDone) onDone(exitCode, out, err);
                p->deleteLater();
            });
    p->start();
    killer->start();
}

void MainWindow::requestNetStatus()
{
    if (preferApStaStack()) {
        const QJsonDocument doc(buildPreferredNetStatusJson());
        emit wsThread->sendMessageToClient("NetStatus|" + QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
        return;
    }

    runSudoAsync("/usr/local/sbin/net-mode.sh", {"status"},
                 [this](int exitCode, const QString &out, const QString &err) {
                     if (exitCode != 0) {
                         Logger::Log(("netStatus failed: " + err).toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                         emit wsThread->sendMessageToClient("SendDebugMessage|Warning|netStatus failed");
                         emit wsThread->sendMessageToClient("NetModeResult|status|fail|" + err.left(200));
                         return;
                     }
                     const QString json = out.trimmed();
                     emit wsThread->sendMessageToClient("NetStatus|" + json);
                 });
}

void MainWindow::switchNetMode(const QString &mode)
{
    const QString m = mode.trimmed().toLower();
    if (m != "ap" && m != "wan") {
        emit wsThread->sendMessageToClient("NetModeResult|" + m + "|fail|invalid_mode");
        return;
    }

    if (networkConfigChanging) {
        Logger::Log("switchNetMode ignored because network config is already changing",
                    LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("NetModeResult|" + m + "|fail|busy");
        return;
    }
    networkConfigChanging = true;

    if (preferApStaStack()) {
        auto finish = [this, m](int exitCode, const QString &detail) {
            networkConfigChanging = false;
            if (exitCode == 0) emit wsThread->sendMessageToClient("NetModeResult|" + m + "|ok");
            else emit wsThread->sendMessageToClient("NetModeResult|" + m + "|fail|" + detail.left(200));
            QTimer::singleShot(500, this, [this]() { requestNetStatus(); });
        };

        if (m == "ap") {
            runSudoAsync("/usr/bin/systemctl", {"restart", QString::fromUtf8(kPreferredHostapdService)},
                         [this, finish](int codeHostapd, const QString &outHostapd, const QString &errHostapd) {
                             finish(codeHostapd, (errHostapd.isEmpty() ? outHostapd : errHostapd).trimmed());
                         });
            return;
        }

        runSudoAsync("/usr/bin/systemctl", {"restart", QString::fromUtf8(kPreferredWpaService)},
                     [this, finish](int codeWpa, const QString &outWpa, const QString &errWpa) {
                         if (codeWpa != 0) {
                             finish(codeWpa, (errWpa.isEmpty() ? outWpa : errWpa).trimmed());
                             return;
                         }
                         runSudoAsync("/usr/bin/systemctl", {"restart", QString::fromUtf8(kPreferredWlanDhcpService)},
                                      [finish](int codeDhcp, const QString &outDhcp, const QString &errDhcp) {
                                          finish(codeDhcp, (errDhcp.isEmpty() ? outDhcp : errDhcp).trimmed());
                                      });
                     });
        return;
    }

    runSudoAsync("/usr/local/sbin/net-mode.sh", {m},
                 [this, m](int exitCode, const QString &out, const QString &err) {
                     networkConfigChanging = false;
                     if (exitCode == 0) {
                         emit wsThread->sendMessageToClient("NetModeResult|" + m + "|ok");
                     } else {
                         const QString detail = (err.isEmpty() ? out : err).trimmed();
                         emit wsThread->sendMessageToClient("NetModeResult|" + m + "|fail|" + detail.left(200));
                     }
                     // Always refresh status after mode switch attempt
                     QTimer::singleShot(500, this, [this]() { requestNetStatus(); });
                 });
}

void MainWindow::wifiScan()
{
    if (preferApStaStack()) {
        runSudoAsync("/sbin/ip", {"link", "set", "wlan0", "up"},
                     [this](int codeUp, const QString &outUp, const QString &errUp) {
                         if (codeUp != 0) {
                             const QString detail = (errUp.isEmpty() ? outUp : errUp).trimmed();
                             emit wsThread->sendMessageToClient("WiFiScan|[]");
                             emit wsThread->sendMessageToClient("WiFiSaveResult|scan|fail|" + detail.left(200));
                             return;
                         }
                         runSudoAsync("/sbin/iw", {"dev", "wlan0", "scan"},
                                      [this](int exitCode, const QString &out, const QString &err) {
                                          if (exitCode != 0) {
                                              Logger::Log(("wifiScan(iw) failed: " + err).toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                                              emit wsThread->sendMessageToClient("WiFiScan|[]");
                                              emit wsThread->sendMessageToClient("WiFiSaveResult|scan|fail|" + err.left(200));
                                              return;
                                          }
                                          const QJsonArray arr = parseIwScanOutput(out);
                                          const QJsonDocument doc(arr);
                                          const QString json = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
                                          emit wsThread->sendMessageToClient("WiFiScan|" + json);
                                          emit wsThread->sendMessageToClient("WiFiSaveResult|scan|ok");
                                      });
                     });
        return;
    }

    // nmcli will output lines like: SSID:SIGNAL:SECURITY
    // (If SSID contains ':', parsing may be imperfect; typical consumer SSID does not.)
    runSudoAsync("/usr/bin/nmcli",
                 {"-t", "-f", "SSID,SIGNAL,SECURITY", "dev", "wifi", "list", "ifname", "wlan0", "--rescan", "yes"},
                 [this](int exitCode, const QString &out, const QString &err) {
                     if (exitCode != 0) {
                         Logger::Log(("wifiScan failed: " + err).toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                         emit wsThread->sendMessageToClient("WiFiScan|[]");
                         emit wsThread->sendMessageToClient("WiFiSaveResult|scan|fail|" + err.left(200));
                         return;
                     }
                     QJsonArray arr;
                     const QStringList lines = out.split('\n', Qt::SkipEmptyParts);
                     for (const QString &line : lines) {
                         // SSID:SIGNAL:SECURITY
                         const QStringList p = line.split(':');
                         if (p.isEmpty()) continue;
                         const QString ssid = p.value(0).trimmed();
                         if (ssid.isEmpty()) continue;
                         const int signal = p.value(1).trimmed().toInt();
                         const QString sec = p.value(2).trimmed();
                         QJsonObject o;
                         o["ssid"] = ssid;
                         o["signal"] = signal;
                         o["security"] = sec;
                         arr.append(o);
                     }
                     QJsonDocument doc(arr);
                     const QString json = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
                     emit wsThread->sendMessageToClient("WiFiScan|" + json);
                 });
}

void MainWindow::wifiSaveFromB64Payload(const QString &b64Payload)
{
    const QByteArray decoded = QByteArray::fromBase64(b64Payload.toUtf8());
    if (decoded.isEmpty()) {
        emit wsThread->sendMessageToClient("WiFiSaveResult|save|fail|bad_base64");
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(decoded);
    if (!doc.isObject()) {
        emit wsThread->sendMessageToClient("WiFiSaveResult|save|fail|bad_json");
        return;
    }
    const QJsonObject obj = doc.object();
    const QString name = obj.value("name").toString("wan-uplink").trimmed();
    const QString ssid = obj.value("ssid").toString().trimmed();
    const QString psk  = obj.value("psk").toString();
    if (name.isEmpty() || ssid.isEmpty() || psk.isEmpty()) {
        emit wsThread->sendMessageToClient("WiFiSaveResult|save|fail|need_name_ssid_psk");
        return;
    }

    if (networkConfigChanging) {
        Logger::Log("wifiSave ignored because network config is already changing",
                    LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("WiFiSaveResult|save|fail|busy");
        return;
    }
    networkConfigChanging = true;

    if (preferApStaStack()) {
        QString writeErr;
        if (!writePreferredWpaSupplicantConf(ssid, psk, &writeErr)) {
            networkConfigChanging = false;
            emit wsThread->sendMessageToClient("WiFiSaveResult|save|fail|" + writeErr.left(200));
            return;
        }
        writeSavedStaProfilesToConfig(upsertSavedStaProfile(readSavedStaProfilesFromConfig(), ssid, psk, name));

        const SyncCommandResult chmodRes =
            runSudoSync("/bin/chmod", {"600", QString::fromUtf8(kPreferredWpaSupplicantConf)}, 2000);
        if (chmodRes.exitCode != 0) {
            const QString detail = (chmodRes.err.isEmpty() ? chmodRes.out : chmodRes.err).trimmed();
            networkConfigChanging = false;
            emit wsThread->sendMessageToClient("WiFiSaveResult|save|fail|" + detail.left(200));
            return;
        }

        runSudoAsync("/usr/bin/systemctl", {"restart", QString::fromUtf8(kPreferredWpaService)},
                     [this](int codeWpa, const QString &outWpa, const QString &errWpa) {
                         if (codeWpa != 0) {
                             networkConfigChanging = false;
                             emit wsThread->sendMessageToClient("WiFiSaveResult|save|fail|" +
                                                                (errWpa.isEmpty() ? outWpa : errWpa).trimmed().left(200));
                             return;
                         }
                         runSudoAsync("/usr/bin/systemctl", {"restart", QString::fromUtf8(kPreferredWlanDhcpService)},
                                      [this](int codeDhcp, const QString &outDhcp, const QString &errDhcp) {
                                          networkConfigChanging = false;
                                          if (codeDhcp == 0) {
                                              emit wsThread->sendMessageToClient("WiFiSaveResult|save|ok");
                                          } else {
                                              emit wsThread->sendMessageToClient("WiFiSaveResult|save|fail|" +
                                                                                 (errDhcp.isEmpty() ? outDhcp : errDhcp).trimmed().left(200));
                                          }
                                      });
                     });
        return;
    }

    // Check if connection exists
    runSudoAsync("/usr/bin/nmcli", {"-t", "-f", "NAME", "con", "show"},
                 [this, name, ssid, psk](int exitCode, const QString &out, const QString &err) {
                     if (exitCode != 0) {
                         networkConfigChanging = false;
                         emit wsThread->sendMessageToClient("WiFiSaveResult|save|fail|" + err.left(200));
                         return;
                     }
                     const QStringList names = out.split('\n', Qt::SkipEmptyParts);
                     const bool exists = names.contains(name);

                     auto finishOk = [this, name, ssid, psk]() {
                         networkConfigChanging = false;
                         writeSavedStaProfilesToConfig(upsertSavedStaProfile(readSavedStaProfilesFromConfig(), ssid, psk, name));
                         emit wsThread->sendMessageToClient("WiFiSaveResult|save|ok");
                     };
                     auto finishFail = [this](const QString &e) {
                         networkConfigChanging = false;
                         emit wsThread->sendMessageToClient("WiFiSaveResult|save|fail|" + e.left(200));
                     };

                     if (!exists) {
                         // add connection
                         runSudoAsync("/usr/bin/nmcli",
                                      {"con", "add", "type", "wifi", "ifname", "wlan0", "con-name", name, "ssid", ssid},
                                      [this, name, ssid, psk, finishOk, finishFail](int codeAdd, const QString &outAdd, const QString &errAdd) {
                                          if (codeAdd != 0) {
                                              finishFail(errAdd.isEmpty() ? outAdd : errAdd);
                                              return;
                                          }
                                          // set security + disable autoconnect
                                          runSudoAsync("/usr/bin/nmcli",
                                                       {"con", "modify", name,
                                                        "wifi-sec.key-mgmt", "wpa-psk",
                                                        "wifi-sec.psk", psk,
                                                        "autoconnect", "no",
                                                        "ipv6.method", "ignore"},
                                                       [finishOk, finishFail](int codeMod, const QString &outMod, const QString &errMod) {
                                                           if (codeMod == 0) finishOk();
                                                           else finishFail(errMod.isEmpty() ? outMod : errMod);
                                                       });
                                      });
                     } else {
                         // modify existing connection
                         runSudoAsync("/usr/bin/nmcli",
                                      {"con", "modify", name,
                                       "802-11-wireless.ssid", ssid,
                                       "wifi-sec.key-mgmt", "wpa-psk",
                                       "wifi-sec.psk", psk,
                                       "autoconnect", "no",
                                       "ipv6.method", "ignore"},
                                      [finishOk, finishFail](int codeMod, const QString &outMod, const QString &errMod) {
                                          if (codeMod == 0) finishOk();
                                          else finishFail(errMod.isEmpty() ? outMod : errMod);
                                      });
                     }
                 });
}

void MainWindow::initINDIServer()
{
    Logger::Log("initINDIServer ...", LogLevel::INFO, DeviceType::MAIN);
    system("pkill indiserver");
    system("rm -f /tmp/myFIFO");
    system("mkfifo /tmp/myFIFO");
    // FIFO 已重建：恢复 Tools 的 FIFO 熔断状态，允许后续 start/stop 写入
    Tools::resetIndiFifoState();
    glIndiServer = new QProcess();
    // glIndiServer->setReadChannel(QProcess::StandardOutput);

    // // 连接信号到槽函数
    // connect(glIndiServer, &QProcess::readyReadStandardOutput, this, &MainWindow::handleIndiServerOutput);
    // connect(glIndiServer, &QProcess::readyReadStandardError, this, &MainWindow::handleIndiServerError);

    // 已知部分驱动会在 verbose 模式下刷屏输出（例如 CCD_TEMPERATURE/CCD_COOLER_POWER 不存在）
    // 这里关闭 -v，并把输出重定向到文件，避免终端持续打印。
    glIndiServer->setProcessChannelMode(QProcess::MergedChannels);
    glIndiServer->setStandardOutputFile("/tmp/indiserver.log", QIODevice::Append);
    glIndiServer->setStandardErrorFile("/tmp/indiserver.log", QIODevice::Append);

    glIndiServer->setProgram("indiserver");
    glIndiServer->setArguments(QStringList() << "-f" << "/tmp/myFIFO" << "-p" << "7624");
    glIndiServer->start();
    Logger::Log("initINDIServer finish!", LogLevel::INFO, DeviceType::MAIN);
}

// void MainWindow::initINDIServer()
// {
//     system("pkill indiserver");
//     system("rm -f /tmp/myFIFO");
//     system("mkfifo /tmp/myFIFO");
//     glIndiServer = new QProcess();
//     glIndiServer->setReadChannel(QProcess::StandardOutput);
//     glIndiServer->start("indiserver -f /tmp/myFIFO -v -p 7624");
// }

// 槽函数：处理标准输出
void MainWindow::handleIndiServerOutput()
{
    QByteArray output = glIndiServer->readAllStandardOutput();
    Logger::Log("INDI Server Output: " + output.toStdString(), LogLevel::INFO, DeviceType::MAIN);
}

// 槽函数：处理标准错误
void MainWindow::handleIndiServerError()
{
    QByteArray error = glIndiServer->readAllStandardError();
    Logger::Log("INDI Server Error: " + error.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
}
int i = 1;
void MainWindow::initINDIClient()
{
    Logger::Log("initINDIClient ...", LogLevel::INFO, DeviceType::MAIN);
    indi_Client = new MyClient();
    indi_Client->setServer("localhost", 7624);
    indi_Client->setConnectionTimeout(3, 0);
    Logger::Log("setConnectionTimeout is 3 seconds!", LogLevel::INFO, DeviceType::MAIN);
    indi_Client->setImageReceivedCallback(
        [this](const std::string &filename, const std::string &devname)
        {
            if (dpGuider != NULL && dpGuider->getDeviceName() == devname)
            {
                guiderExposureInFlight = false;
                const QString fitsPath = QString::fromStdString(filename);

                if (guiderCore)
                {
                    QMetaObject::invokeMethod(guiderCore, "onNewFrame", Qt::QueuedConnection,
                                              Q_ARG(QString, fitsPath));
                }
                else
                {
                    PersistGuidingFits(fitsPath);
                    if (isGuiderLoopExp && guiderLoopTimer)
                    {
                        // INDI 图像回调可能运行在非 GUI 线程，必须回到 MainWindow 线程再启动 QTimer。
                        QMetaObject::invokeMethod(this, [this]() {
                            if (isGuiderLoopExp && guiderLoopTimer)
                                guiderLoopTimer->start(1);
                        }, Qt::QueuedConnection);
                    }
                }
                return;
            }

            // 曝光完成
            if (dpMainCamera != NULL)
            {
                if (dpMainCamera->getDeviceName() == devname)
                {
                    lastMainCaptureFitsPath = QString::fromStdString(filename);
                    glMainCameraStatu = "Displaying";
                    ShootStatus = "Completed";
                    if (autoFocuserIsROI && isAutoFocus)
                    {
                        saveFitsAsJPG(QString::fromStdString(filename), true);
                        Logger::Log("saveFitsAsJPG", LogLevel::DEBUG, DeviceType::MAIN);
                        autoFocus->setCaptureComplete(QString::fromStdString(filename));
                        return;
                    }
                    else if (!autoFocuserIsROI && isAutoFocus)
                    {

                        saveFitsAsPNG(QString::fromStdString(filename), true);

                        Logger::Log("saveFitsAsPNG", LogLevel::DEBUG, DeviceType::MAIN);
                        autoFocus->setCaptureComplete(QString::fromStdString(filename));
                        return;
                    }

                    // 检查：如果 isFocusLoopShooting 为 false 但 glIsFocusingLooping 刚被重置，
                    // 说明这可能是 ROI 停止时的残留帧，应该丢弃或按 ROI 处理
                    if (glIsFocusingLooping == false && !isFocusLoopShooting)
                    {
                        // 读取 FITS 获取图像尺寸，判断是否为 ROI 残留帧
                        fitsfile *fptr = nullptr;
                        int status = 0;
                        long naxes[2] = {0, 0};
                        int naxis = 0;
                        
                        if (fits_open_file(&fptr, filename.c_str(), READONLY, &status) == 0)
                        {
                            fits_get_img_dim(fptr, &naxis, &status);
                            if (naxis == 2)
                            {
                                fits_get_img_size(fptr, 2, naxes, &status);
                            }
                            fits_close_file(fptr, &status);
                        }
                        
                        // 如果图像尺寸远小于全分辨率（例如 < 80%），判定为 ROI 残留帧
                        bool isRoiFrame = false;
                        if (naxes[0] > 0 && naxes[1] > 0 && glMainCCDSizeX > 0 && glMainCCDSizeY > 0)
                        {
                            double widthRatio = (double)naxes[0] / glMainCCDSizeX;
                            double heightRatio = (double)naxes[1] / glMainCCDSizeY;
                            if (widthRatio < 0.8 || heightRatio < 0.8)
                            {
                                isRoiFrame = true;
                                Logger::Log("Image received after ROI stop, detected as ROI frame (" +
                                           std::to_string(naxes[0]) + "x" + std::to_string(naxes[1]) +
                                           " vs " + std::to_string(glMainCCDSizeX) + "x" + std::to_string(glMainCCDSizeY) +
                                           "), discarding...", LogLevel::WARNING, DeviceType::CAMERA);
                            }
                        }
                        
                        // 如果是 ROI 残留帧，直接丢弃
                        if (isRoiFrame)
                        {
                            glMainCameraStatu = "IDLE";
                            Logger::Log("ROI residual frame discarded", LogLevel::INFO, DeviceType::CAMERA);
                            return;
                        }
                        
                        // 否则按正常拍摄处理
                        emit wsThread->sendMessageToClient("ExposureCompleted");
                        emitCaptureTrace(QStringLiteral("backend_exposure_completed"), currentCaptureTraceStartedAtMs,
                                         QStringLiteral("source=fits_callback"));
                        Logger::Log("ExposureCompleted", LogLevel::INFO, DeviceType::CAMERA);
                        if (polarAlignment != nullptr)
                        {
                            if (polarAlignment->isRunning())
                            {
                                notifyPolarAlignmentCaptureReady(PolarAlignmentCameraRole::MainCamera,
                                                                 QString::fromStdString(filename));
                                return;
                            }
                        }
                        saveFitsAsPNG(QString::fromStdString(filename), true); // "/dev/shm/ccd_simulator.fits"
                        // saveFitsAsPNG("/home/quarcs/2025_06_26T08_24_13_544.fits", true);
                        // saveFitsAsPNG("/dev/shm/SOLVETEST.fits", true);
                        
                        // 如果自动保存开启，自动保存图像
                        if (mainCameraAutoSave && isScheduleRunning == false)
                        {
                            Logger::Log("Auto Save enabled, saving captured image...", LogLevel::INFO, DeviceType::MAIN);
                            CaptureImageSave();
                        }
                    }
                    else
                    {

                        saveFitsAsJPG(QString::fromStdString(filename), true);

                        Logger::Log("saveFitsAsJPG", LogLevel::DEBUG, DeviceType::MAIN);
                    }
                    // Logger::Log("拍摄完成，图像保存完成 finish!", LogLevel::INFO, DeviceType::MAIN);
                }
            }

            // 导星相机：INDI 直出图（替代 PHD2）。收到一帧后：
            // 1) 生成导星预览 JPG（前端 Guide 画面）
            // 2) 把 FITS 保存到与主相机相同的保存目录下，命名为 guider.fits（覆盖写）
            // 3) 若开启循环曝光，则调度下一帧
            if (dpGuider != NULL)
            {
                if (dpGuider->getDeviceName() == devname)
                {
                    const QString fitsPath = QString::fromStdString(filename);
                    if (polarGuiderSingleCapturePending)
                    {
                        notifyPolarAlignmentCaptureReady(PolarAlignmentCameraRole::Guider, fitsPath);
                        polarGuiderSingleCapturePending = false;
                    }

                    // 1) 预览：从 FITS 读取单通道并拉伸为 8-bit，复用原有 saveGuiderImageAsJPG 的前端协议
                    {
                        fitsfile *fptr = nullptr;
                        int status = 0;
                        int bitpix = 0;
                        int naxis = 0;
                        long naxes[2] = {0, 0};

                        if (fits_open_file(&fptr, fitsPath.toUtf8().constData(), READONLY, &status) == 0)
                        {
                            fits_get_img_param(fptr, 2, &bitpix, &naxis, naxes, &status);
                            if (status == 0 && naxis >= 2 && naxes[0] > 0 && naxes[1] > 0)
                            {
                                const long w = naxes[0];
                                const long h = naxes[1];
                                const long npix = w * h;
                                long fpixel[2] = {1, 1};

                                cv::Mat img16;
                                img16.create((int)h, (int)w, CV_16UC1);

                                if (bitpix == 8)
                                {
                                    std::vector<unsigned char> tmp((size_t)npix);
                                    fits_read_pix(fptr, TBYTE, fpixel, npix, NULL, tmp.data(), NULL, &status);
                                    if (status == 0)
                                    {
                                        uint16_t *dst = reinterpret_cast<uint16_t *>(img16.data);
                                        for (long i = 0; i < npix; ++i)
                                            dst[i] = static_cast<uint16_t>(tmp[(size_t)i]) * 257; // 8->16 展开
                                    }
                                }
                                else
                                {
                                    // 默认按 16-bit 读取（兼容大多数导星相机输出）
                                    fits_read_pix(fptr, TUSHORT, fpixel, npix, NULL, img16.data, NULL, &status);
                                }

                                if (status == 0)
                                {
                                    // 计算导星帧动态范围，避免“自动拉伸但仍全黑”的情况
                                    double minVal = 0.0, maxVal = 0.0;
                                    cv::minMaxLoc(img16, &minVal, &maxVal);

                                    uint16_t B = 0, W = 65535;
                                    // 导星预览默认启用自动拉伸：前端 Guide 画面更直观
                                    Tools::GetAutoStretch(img16, 0, B, W);

                                    // 兜底：若自动拉伸的白点远大于实际最大值，会导致整体偏暗（甚至全黑）
                                    // 这里用实际 maxVal 作为上限，确保至少能看见星点/背景变化
                                    if (maxVal > 0.0)
                                    {
                                        const uint16_t maxU16 = (uint16_t)std::min(65535.0, std::max(0.0, maxVal));
                                        if (W > (uint16_t)std::min<uint32_t>(65535u, (uint32_t)maxU16 + 1024u))
                                        {
                                            B = 0;
                                            W = std::max<uint16_t>(1, maxU16);
                                        }
                                        if (W <= B) W = (uint16_t)std::min<uint32_t>(65535u, (uint32_t)B + 10u);
                                    }

                                    Logger::Log("GuiderPreviewStretch | bitpix=" + std::to_string(bitpix) +
                                                    " min=" + std::to_string(minVal) +
                                                    " max=" + std::to_string(maxVal) +
                                                    " B=" + std::to_string(B) +
                                                    " W=" + std::to_string(W),
                                                LogLevel::DEBUG, DeviceType::GUIDER);

                                    cv::Mat img8;
                                    img8.create(img16.rows, img16.cols, CV_8UC1);
                                    Tools::Bit16To8_Stretch(img16, img8, B, W);
                                    // 兜底：如果拉伸结果仍然全黑（通常是 B/W 异常或位深/动态范围不匹配）
                                    // 强制用真实最大值做一次映射，确保“过曝”不会变成黑屏
                                    double min8 = 0.0, max8 = 0.0;
                                    cv::minMaxLoc(img8, &min8, &max8);
                                    if (max8 <= 0.0 && maxVal > 0.0) {
                                        const uint16_t maxU16 = (uint16_t)std::min(65535.0, std::max(1.0, maxVal));
                                        B = 0;
                                        W = maxU16;
                                        Tools::Bit16To8_Stretch(img16, img8, B, W);
                                        Logger::Log("GuiderPreviewStretch | fallback restretch applied (img8 max==0). B=" +
                                                        std::to_string(B) + " W=" + std::to_string(W),
                                                    LogLevel::INFO, DeviceType::GUIDER);
                                    }
                                    saveGuiderImageAsJPG(img8);
                                }
                            }
                            fits_close_file(fptr, &status);
                        }
                    }

                    // 2) 保存：与主相机 CaptureImageSave 的目录结构保持一致（按日期）
                    {
                        std::time_t currentTime = std::time(nullptr);
                        std::tm *timeInfo = std::localtime(&currentTime);
                        char buffer[80];
                        std::strftime(buffer, 80, "%Y-%m-%d", timeInfo); // YYYY-MM-DD

                        const QString destinationDirectory = ImageSaveBaseDirectory + "/CaptureImage/" + QString(buffer);
                        const QString destinationPath = destinationDirectory + "/guider.fits";
                        const bool isUSBSave = (saveMode != "local");

                        // U 盘模式需要提前创建目录（sudo），本地模式 saveImageFile 会自行创建
                        const QString dirPathToCreate = isUSBSave ? destinationDirectory : QString();
                        int checkResult = checkStorageSpaceAndCreateDirectory(
                            fitsPath,
                            ImageSaveBaseDirectory + "/CaptureImage",
                            dirPathToCreate,
                            "GuiderFitsSave",
                            isUSBSave,
                            nullptr);
                        if (checkResult == 0)
                        {
                            saveImageFile(fitsPath, destinationPath, "GuiderFitsSave", isUSBSave);
                        }
                    }

                    // 3) 循环曝光：放行下一帧
                    guiderExposureInFlight = false;
                    if (isGuiderLoopExp && guiderLoopTimer)
                    {
                        // 注意：INDI 图像回调可能在非 GUI 线程触发，直接 start(QTimer) 会报：
                        // "QObject::startTimer: Timers cannot be started from another thread"
                        // 因此把启动下一帧投递回 MainWindow 线程执行。
                        QMetaObject::invokeMethod(this, [this]() {
                            if (isGuiderLoopExp && guiderLoopTimer)
                                guiderLoopTimer->start(1);
                        }, Qt::QueuedConnection);
                    }
                }
            }
            if (dpPoleScope != NULL)
            {
                if (dpPoleScope->getDeviceName() == devname)
                {
                    const QString fitsPath = QString::fromStdString(filename);
                    if (polarGuiderSingleCapturePending)
                    {
                        notifyPolarAlignmentCaptureReady(PolarAlignmentCameraRole::PoleCamera, fitsPath);
                        polarGuiderSingleCapturePending = false;
                    }
                    guiderExposureInFlight = false;
                }
            }
        });
    Logger::Log("indi_Client->setImageReceivedCallback finish!", LogLevel::INFO, DeviceType::MAIN);

    indi_Client->setMessageReceivedCallback(
        [this](const std::string &message)
        {
            // qDebug("indi初始信息 %s", message.c_str());
            QString messageStr = QString::fromStdString(message.c_str());

            // 使用正则表达式移除时间戳
            std::regex timestampRegex(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}: )");
            messageStr = QString::fromStdString(std::regex_replace(messageStr.toStdString(), timestampRegex, ""));

            // qDebug("indi提取后信息 %s", messageStr.toStdString().c_str());
            // 使用正则表达式提取并移除日志类型
            std::regex typeRegex(R"(\[(INFO|WARNING|ERROR)\])");
            std::smatch typeMatch;
            QString logType;
            if (std::regex_search(message, typeMatch, typeRegex) && typeMatch.size() > 1)
            {
                logType = QString::fromStdString(typeMatch[1].str());
                // 移除日志类型
                messageStr = QString::fromStdString(std::regex_replace(messageStr.toStdString(), typeRegex, ""));
            }

            if (messageStr.contains("Telescope focal length is missing.") ||
                messageStr.contains("Telescope aperture is missing."))
            {
                // 跳过打印
                return;
            }
            if (logType == "WARNING")
            {
                DeviceType deviceType = getDeviceTypeFromPartialString(messageStr.toStdString());
                Logger::Log("[INDI SERVER] " + messageStr.toStdString(), LogLevel::WARNING, deviceType);
            }
            else if (logType == "ERROR")
            {
                DeviceType deviceType = getDeviceTypeFromPartialString(messageStr.toStdString());
                Logger::Log("[INDI SERVER] " + messageStr.toStdString(), LogLevel::ERROR, deviceType);
            }
            else
            {
                DeviceType deviceType = getDeviceTypeFromPartialString(messageStr.toStdString());
                // 导星循环曝光会高频刷 “Image saved to /dev/shm/guiding.fits” ：降为 DEBUG（默认关闭 DEBUG）避免刷屏
                if (messageStr.contains("Image saved to /dev/shm/guiding.fits"))
                    Logger::Log("[INDI SERVER] " + messageStr.toStdString(), LogLevel::DEBUG, deviceType);
                else
                    Logger::Log("[INDI SERVER] " + messageStr.toStdString(), LogLevel::INFO, deviceType);
            }

            std::regex regexPattern(R"(OnStep slew/syncError:\s*(.*))");
            std::smatch matchResult;

            if (std::regex_search(message, matchResult, regexPattern))
            {
                if (matchResult.size() > 1)
                {
                    QString errorContent = QString::fromStdString(matchResult[1].str());
                    Logger::Log("OnStep Error: " + errorContent.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                    emit wsThread->sendMessageToClient("OnStep Error:" + errorContent);
                    MountGotoError = true;
                }
            }
        });
    Logger::Log("indi_Client->setMessageReceivedCallback finish!", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::startPoleCameraSingleCapture(int exposureMs)
{
    const int expMs = std::max(1, exposureMs);
    sdkGuiderExposureRole = "PoleCamera";

    if (isGuiderLoopExp)
    {
        Logger::Log("startPoleCameraSingleCapture | stopping guider loop before polar capture",
                    LogLevel::INFO, DeviceType::GUIDER);
        if (guiderCore)
        {
            postGuiderCore(guiderCore, [](GuiderCore *core) {
                core->stopGuiding();
                core->stopLoop();
            });
        }
        isGuiderLoopExp = false;
        if (guiderLoopTimer)
            guiderLoopTimer->stop();
        if (sdkGuiderExposureTimer)
            sdkGuiderExposureTimer->stop();
        emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
    }

    if (isPoleCameraSDK())
    {
        SdkSerialExecutor *poleExec = sdkPoleCameraExecutor();
        if (!poleExec || !poleExec->isRunning())
        {
            Logger::Log("startPoleCameraSingleCapture | sdkPoleCamExec is not running", LogLevel::ERROR, DeviceType::MAIN);
            polarGuiderSingleCapturePending = false;
            guiderExposureInFlight = false;
            return;
        }
        if (guiderExposureInFlight || sdkGuiderFrameTaskInFlight.load())
        {
            Logger::Log("startPoleCameraSingleCapture | exposure/readout is already in flight, retrying shortly",
                        LogLevel::WARNING, DeviceType::MAIN);
            QTimer::singleShot(250, this, [this, expMs]() {
                const bool oldPolarRunning = polarAlignment != nullptr && polarAlignment->isRunning();
                const bool poleMasterRunning = poleMasterPolarAlignment != nullptr && poleMasterPolarAlignment->isRunning();
                if ((oldPolarRunning || poleMasterRunning) &&
                    currentPolarAlignmentCameraRole == PolarAlignmentCameraRole::PoleCamera)
                {
                    startPoleCameraSingleCapture(expMs);
                }
            });
            return;
        }

        polarGuiderSingleCapturePending = true;
        guiderExposureInFlight = true;
        const double expSec = expMs / 1000.0;
        const SdkDeviceHandle handleSnap = sdkPoleScopeHandle;

        poleExec->post([this, handleSnap, expMs, expSec]() {
            auto failOnMain = [this](const std::string &message) {
                QMetaObject::invokeMethod(this, [this, message]() {
                    Logger::Log(message, LogLevel::ERROR, DeviceType::MAIN);
                    polarGuiderSingleCapturePending = false;
                    guiderExposureInFlight = false;
                }, Qt::QueuedConnection);
            };

            SdkCommand cancelCmd;
            cancelCmd.type = SdkCommandType::Custom;
            cancelCmd.name = "CancelExposure";
            cancelCmd.payload = std::any();
            SdkManager::instance().callByHandle(handleSnap, cancelCmd);

            SdkCommand setExpCmd;
            setExpCmd.type = SdkCommandType::Custom;
            setExpCmd.name = "SetExposure";
            setExpCmd.payload = expSec * 1000000.0;
            SdkResult setRes = SdkManager::instance().callByHandle(handleSnap, setExpCmd);
            if (!setRes.success)
            {
                failOnMain("startPoleCameraSingleCapture | SDK SetExposure failed: " + setRes.message);
                return;
            }

            SdkCommand startExpCmd;
            startExpCmd.type = SdkCommandType::Custom;
            startExpCmd.name = "StartSingleExposure";
            startExpCmd.payload = std::any();
            SdkResult startRes = SdkManager::instance().callByHandle(handleSnap, startExpCmd);
            if (!startRes.success)
            {
                failOnMain("startPoleCameraSingleCapture | SDK StartSingleExposure failed: " + startRes.message);
                return;
            }

            QMetaObject::invokeMethod(this, [this, expMs]() {
                sdkGuiderExposureStartTime = QDateTime::currentMSecsSinceEpoch();
                sdkGuiderExposureExpectedDuration = expMs;
                sdkGuiderExposureRole = "PoleCamera";
                if (sdkGuiderExposureTimer)
                    sdkGuiderExposureTimer->start(expMs);
            }, Qt::QueuedConnection);
        });
        return;
    }

    if (!indi_Client || dpPoleScope == nullptr || !dpPoleScope->isConnected())
    {
        Logger::Log("startPoleCameraSingleCapture | pole camera is not connected", LogLevel::ERROR, DeviceType::MAIN);
        polarGuiderSingleCapturePending = false;
        guiderExposureInFlight = false;
        return;
    }

    polarGuiderSingleCapturePending = true;
    guiderExposureInFlight = true;
    const double expSec = expMs / 1000.0;
    indi_Client->takeExposure(dpPoleScope, expSec);
}

DeviceType MainWindow::getDeviceTypeFromPartialString(const std::string &typeStr)
{
    // 使用find()方法检查字符串中是否包含特定的子字符串
    if (typeStr.find("Exposure done, downloading image") != std::string::npos || typeStr.find("Download complete") != std::string::npos || typeStr.find("Uploading file.") != std::string::npos || typeStr.find("Image saved to") != std::string::npos)
    {
        // Logger::Log("获取的信息类型是相机", LogLevel::INFO, DeviceType::CAMERA);
        return DeviceType::CAMERA;
    }
    else if (0)
    {
        // Logger::Log("DeviceType::MOUNT", LogLevel::INFO, DeviceType::MOUNT);
        return DeviceType::MOUNT;
    }
    else
    {
        // Logger::Log("DeviceType::MAIN", LogLevel::INFO, DeviceType::MAIN);
        return DeviceType::MAIN;
    }
}

namespace
{
/** sysfs GPIO 节点在 export 后可能延迟出现，对 ENOENT/EINTR 做短重试。 */
int openSysfsGpioRetry(const char *path, int flags)
{
    int fd = -1;
    for (int i = 0; i < 80; ++i)
    {
        fd = open(path, flags);
        if (fd >= 0)
            return fd;
        if (errno != ENOENT && errno != EINTR)
            break;
        usleep(2500);
    }
    return -1;
}

bool readSysfsGpioText(const char *path, char *buffer, size_t bufferSize)
{
    if (!buffer || bufferSize == 0)
        return false;

    const int fd = openSysfsGpioRetry(path, O_RDONLY);
    if (fd < 0)
        return false;

    errno = 0;
    const ssize_t bytesRead = read(fd, buffer, bufferSize - 1);
    close(fd);
    if (bytesRead <= 0)
        return false;

    buffer[bytesRead] = '\0';
    return true;
}

bool gpioDirectionIsOut(const char *pin)
{
    char path[128];
    char direction[16];

    snprintf(path, sizeof(path), GPIO_PATH "/gpio%s/direction", pin);
    if (!readSysfsGpioText(path, direction, sizeof(direction)))
        return false;

    return strncmp(direction, "out", 3) == 0;
}

std::string formatBayerPhaseDebug(const QString& baseCfa, int cfaOffsetX, int cfaOffsetY,
                                  int frameStartX, int frameStartY, const QString& resolvedCfa)
{
    const int totalShiftX = cfaOffsetX + frameStartX;
    const int totalShiftY = cfaOffsetY + frameStartY;
    return "baseCFA=" + baseCfa.toStdString() +
           ", cfaOffset=(" + std::to_string(cfaOffsetX) + "," + std::to_string(cfaOffsetY) + ")" +
           ", frameStart=(" + std::to_string(frameStartX) + "," + std::to_string(frameStartY) + ")" +
           ", totalShift=(" + std::to_string(totalShiftX) + "," + std::to_string(totalShiftY) + ")" +
           ", parity=(" + std::to_string(totalShiftX & 1) + "," + std::to_string(totalShiftY & 1) + ")" +
           ", resolvedCFA=" + resolvedCfa.toStdString();
}

std::string sampleBayer2x2Debug(const cv::Mat& image16)
{
    if (image16.empty() || image16.type() != CV_16UC1) {
        return "2x2=unavailable";
    }

    const int maxY = std::min(2, image16.rows);
    const int maxX = std::min(2, image16.cols);
    std::ostringstream oss;
    oss << "2x2=[";
    for (int y = 0; y < maxY; ++y) {
        if (y > 0) oss << ";";
        for (int x = 0; x < maxX; ++x) {
            if (x > 0) oss << ",";
            oss << image16.at<uint16_t>(y, x);
        }
    }
    oss << "]";
    return oss.str();
}
} // namespace

void MainWindow::initGPIO()
{
    Logger::Log("Initializing GPIO...", LogLevel::INFO, DeviceType::MAIN);
    // Initialize GPIO_PIN_1
    exportGPIO(GPIO_PIN_1);
    setGPIODirection(GPIO_PIN_1, "out");
    Logger::Log(gpioDirectionIsOut(GPIO_PIN_1)
                    ? "Set direction of GPIO_PIN_1 to output completed!"
                    : "GPIO_PIN_1 direction is not out after initialization",
                gpioDirectionIsOut(GPIO_PIN_1) ? LogLevel::INFO : LogLevel::WARNING,
                DeviceType::MAIN);

    // Initialize GPIO_PIN_2
    exportGPIO(GPIO_PIN_2);
    setGPIODirection(GPIO_PIN_2, "out");
    Logger::Log(gpioDirectionIsOut(GPIO_PIN_2)
                    ? "Set direction of GPIO_PIN_2 to output completed!"
                    : "GPIO_PIN_2 direction is not out after initialization",
                gpioDirectionIsOut(GPIO_PIN_2) ? LogLevel::INFO : LogLevel::WARNING,
                DeviceType::MAIN);

    // Set GPIO_PIN_1 to high level
    setGPIOValue(GPIO_PIN_1, "1");
    Logger::Log("Set GPIO_PIN_1 level to high completed!", LogLevel::INFO, DeviceType::MAIN);
    // Set GPIO_PIN_2 to high level
    setGPIOValue(GPIO_PIN_2, "1");
    Logger::Log("Set GPIO_PIN_2 level to high completed!", LogLevel::INFO, DeviceType::MAIN);

    Logger::Log("GPIO initialization completed!", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::exportGPIO(const char *pin)
{
    int fd;
    char buf[64];

    // Export GPIO pin
    fd = open(GPIO_EXPORT, O_WRONLY);
    if (fd < 0)
    {
        Logger::Log("Failed to open export file for writing", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    snprintf(buf, sizeof(buf), "%s", pin);
    errno = 0;
    if (write(fd, buf, strlen(buf)) != (ssize_t) strlen(buf))
    {
        // EBUSY usually means the GPIO is already exported (common on restart) — not a fatal error.
        if (errno == EBUSY)
        {
            Logger::Log(std::string("GPIO already exported: pin=") + pin, LogLevel::DEBUG, DeviceType::MAIN);
        }
        else
        {
            Logger::Log(std::string("Failed to write to export file: ") + strerror(errno), LogLevel::WARNING, DeviceType::MAIN);
        }
        close(fd);
        return;
    }
    close(fd);
    Logger::Log("GPIO pin export successful", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::setGPIODirection(const char *pin, const char *direction)
{
    int fd;
    char path[128];

    // Set GPIO direction
    snprintf(path, sizeof(path), GPIO_PATH "/gpio%s/direction", pin);
    fd = openSysfsGpioRetry(path, O_WRONLY);
    if (fd < 0)
    {
        Logger::Log(std::string("Failed to open GPIO direction file for writing: ") + path + " " + strerror(errno),
                    LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    errno = 0;
    if (write(fd, direction, strlen(direction)) != (ssize_t) strlen(direction))
    {
        Logger::Log(std::string("Failed to set GPIO direction: ") + strerror(errno), LogLevel::WARNING, DeviceType::MAIN);
        close(fd);
        return;
    }
    close(fd);
    Logger::Log("GPIO direction set successfully", LogLevel::INFO, DeviceType::MAIN);
}
void MainWindow::setGPIOValue(const char *pin, const char *value)
{
    int fd;
    char path[128];

    if (!gpioDirectionIsOut(pin))
    {
        Logger::Log(std::string("GPIO direction is not out before writing value, trying to repair: pin=") + pin,
                    LogLevel::WARNING, DeviceType::MAIN);
        setGPIODirection(pin, "out");
        if (!gpioDirectionIsOut(pin))
        {
            Logger::Log(std::string("GPIO direction is still not out, abort writing value: pin=") + pin,
                        LogLevel::WARNING, DeviceType::MAIN);
            return;
        }
    }

    // Set GPIO value
    snprintf(path, sizeof(path), GPIO_PATH "/gpio%s/value", pin);
    fd = openSysfsGpioRetry(path, O_WRONLY);
    if (fd < 0)
    {
        Logger::Log(std::string("Failed to open GPIO value file for writing: ") + path + " " + strerror(errno),
                    LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    errno = 0;
    if (write(fd, value, strlen(value)) != (ssize_t) strlen(value))
    {
        Logger::Log(std::string("Failed to write to GPIO value: ") + strerror(errno), LogLevel::WARNING, DeviceType::MAIN);
        close(fd);
        return;
    }
    close(fd);
    Logger::Log("GPIO value set successfully", LogLevel::INFO, DeviceType::MAIN);
}
int MainWindow::readGPIOValue(const char *pin)
{
    char path[128];
    char value[8]; // Store the read value

    // Construct the path to the GPIO value file
    snprintf(path, sizeof(path), GPIO_PATH "/gpio%s/value", pin);

    if (!readSysfsGpioText(path, value, sizeof(value)))
    {
        Logger::Log(std::string("Failed to read GPIO value file: ") + path + " " + strerror(errno),
                    LogLevel::WARNING, DeviceType::MAIN);
        return -1; // Return -1 to indicate read failure
    }

    // Determine if the read value is '1' or '0'
    if (value[0] == '1')
    {
        return 1; // Return 1 to indicate high level
    }
    else if (value[0] == '0')
    {
        return 0; // Return 0 to indicate low level
    }
    else
    {
        return -1; // If the value is not '0' or '1', return -1
    }
}
void MainWindow::getGPIOsStatus()
{
    int value1 = readGPIOValue(GPIO_PIN_1);
    emit wsThread->sendMessageToClient("OutputPowerStatus:1:" + QString::number(value1));
    int value2 = readGPIOValue(GPIO_PIN_2);
    emit wsThread->sendMessageToClient("OutputPowerStatus:2:" + QString::number(value2));
}

void MainWindow::onTimeout()
{
    // TODO(PHD2): PHD2 数据轮询已停用（导星改为 INDI 直出图），如需恢复再启用 ShowPHDdata()
    // ShowPHDdata();

    // 显示赤道仪指向
    mountDisplayCounter++;
    if (dpMount != NULL)
    {
        if (dpMount->isConnected())
        {
            if (mountDisplayCounter >= 5)
            {
                
                double RA_HOURS, DEC_DEGREE;
                indi_Client->getTelescopeRADECJNOW(dpMount, RA_HOURS, DEC_DEGREE);
                double CurrentRA_Degree = Tools::HourToDegree(RA_HOURS);
                double CurrentDEC_Degree = DEC_DEGREE;

                emit wsThread->sendMessageToClient("TelescopeRADEC:" 
                    + QString::number(CurrentRA_Degree) 
                    + ":" + QString::number(CurrentDEC_Degree));

                // Logger::Log("当前指向:RA:" + std::to_string(RA_HOURS) + " 小时,DEC:" + std::to_string(CurrentDEC_Degree) + " 度", LogLevel::INFO, DeviceType::MAIN);

                // 直接每次执行原"慢速"查询内容
                bool isParked = false;
                indi_Client->getTelescopePark(dpMount, isParked);
                emit wsThread->sendMessageToClient(
                    isParked ? "TelescopePark:ON" : "TelescopePark:OFF");
                
                QString NewTelescopePierSide;
                indi_Client->getTelescopePierSide(dpMount, NewTelescopePierSide);
                if (NewTelescopePierSide != TelescopePierSide)
                {
                    // 出现方向侧变化,此时意味着进行了中天翻转,判断是否完成翻转
                    if (indi_Client->mountState.isMovingNow() == false) {
                        emit wsThread->sendMessageToClient("FlipStatus:success");
                        TelescopePierSide = NewTelescopePierSide;

                        // ===== 内置导星：翻转后强制重新校准（PHD2 常规做法）=====
                        // 翻转会改变力学状态，且可能导致相机/视场变化；继续使用旧校准容易越导越偏。
                        if (guiderCore)
                        {
                            const guiding::State gs = guiderCoreStateCache;
                            if (gs == guiding::State::Guiding || gs == guiding::State::Calibrating)
                            {
                                Logger::Log("BuiltInGuider | pier side changed, force recalibrate after meridian flip.",
                                            LogLevel::INFO, DeviceType::GUIDER);
                                emit wsThread->sendMessageToClient("GuiderCoreInfo:MeridianFlipDetected:Recalibrating");
                                postGuiderCore(guiderCore, [](GuiderCore *core) {
                                    core->clearCachedCalibration();
                                    core->stopGuiding();
                                    core->startGuidingForceCalibrate();
                                });
                            }
                        }
                    }
                }
                emit wsThread->sendMessageToClient("TelescopePierSide:" + NewTelescopePierSide);
                // Logger::Log("TelescopePierSide:" + NewTelescopePierSide.toStdString(), LogLevel::INFO, DeviceType::MAIN);

                indi_Client->getTelescopeMoving(dpMount);

                bool isTrack = false;
                indi_Client->getTelescopeTrackEnable(dpMount, isTrack);

                if (polarAlignment != nullptr && polarAlignment->isRunning() && isTrack) {
                    indi_Client->setTelescopeTrackEnable(dpMount, false);
                    sleep(1);
                    indi_Client->getTelescopeTrackEnable(dpMount, isTrack);
                }               

                emit wsThread->sendMessageToClient(isTrack ? "TelescopeTrack:ON" 
                                                           : "TelescopeTrack:OFF");

                if (!FirstRecordTelescopePierSide)
                {
                    if (FirstTelescopePierSide != TelescopePierSide)
                        isMeridianFlipped = true;
                    else
                        isMeridianFlipped = false;
                }

                emit wsThread->sendMessageToClient("TelescopeStatus:" + TelescopeControl_Status());

                mountDisplayCounter = 0;

                // const MeridianStatus ms = checkMeridianStatus(); // 这个计算当前距离中天的时间
                // switch (ms.event) {
                //   case FlipEvent::Started: emit wsThread->sendMessageToClient("MeridianFlip:STARTED"); break;
                //   case FlipEvent::Done:    emit wsThread->sendMessageToClient("MeridianFlip:DONE");    break;
                //   case FlipEvent::Failed:  emit wsThread->sendMessageToClient("MeridianFlip:FAILED");  break;
                //   default: break;
                // }

                // if (!std::isnan(ms.etaMinutes)) {
                //     // 显示规则：与翻转需求绑定 —— 需要翻转显示负号，不需要显示正号
                //     const bool showNeg = ms.needsFlip;
                //     const double absMinutes = std::fabs(ms.etaMinutes);
                //     const int totalSeconds = static_cast<int>(std::llround(absMinutes * 60.0));
                //     const int hours = totalSeconds / 3600;
                //     const int mins  = (totalSeconds % 3600) / 60;
                //     const int secs  = totalSeconds % 60;

                //     const QString hms = QString("%1%2:%3:%4")
                //                             .arg(showNeg ? "-" : "")
                //                             .arg(hours, 2, 10, QLatin1Char('0'))
                //                             .arg(mins,  2, 10, QLatin1Char('0'))
                //                             .arg(secs,  2, 10, QLatin1Char('0'));
                //     emit wsThread->sendMessageToClient("MeridianETA_hms:" + hms);
                //     Logger::Log("MeridianETA_hms:" + hms.toStdString() + " side:" + TelescopePierSide.toStdString() + " needflip:" + (ms.needsFlip ? "true" : "false"), LogLevel::INFO, DeviceType::MAIN);
                // }

                //TODO:当前判断方式存在问题,需要重新修改判断
                // 加入判断,当此时需要执行自动中天翻转,且设备设置为自动中天翻转,则执行自动中天翻转
                // if (ms.needsFlip && isAutoFlip && indi_Client->mountState.isFlipping == false && indi_Client->mountState.isFlipBacking == false) {
                //     // 预备翻转
                //     if (flipPrepareTime >= 0) {
                //         flipPrepareTime-=2;
                //         emit wsThread->sendMessageToClient("FlipStatus:FlipPrepareTime," + QString::number(flipPrepareTime));
                //     }
                //     else {
                //         emit wsThread->sendMessageToClient("FlipStatus:start");
                //         indi_Client->startFlip(dpMount);
                //     }
                // }else{
                //     flipPrepareTime = flipPrepareTimeDefault;
                // }
                // if (indi_Client->mountState.isFlipping == true || indi_Client->mountState.isFlipBacking == true) {
                //     emit wsThread->sendMessageToClient("FlipStatus:start");
                // }

            }
            // Logger::Log("11111", LogLevel::INFO, DeviceType::MAIN);
        }
    }
    

    MainCameraStatusCounter++;
    
    // 判断是 SDK 模式还是 INDI 模式
    bool isMainCameraSDK = (systemdevicelist.system_devices.size() > 20 &&
                            systemdevicelist.system_devices[20].isSDKConnect &&
                            sdkMainCameraHandle != nullptr);
    
    if (isMainCameraSDK || dpMainCamera != NULL)
    {
        if (MainCameraStatusCounter >= 5)
        {
            emit wsThread->sendMessageToClient("MainCameraStatus:" + glMainCameraStatu);
            MainCameraStatusCounter = 0;
            double CameraTemp = 0.0;
            uint32_t ret = QHYCCD_ERROR;
            
            if (isMainCameraSDK)
            {
                // SDK 模式：在 SDK 线程异步获取温度，避免阻塞主线程（与 GetSingleFrame 可能竞争设备锁）
                SdkSerialExecutor *mainExec = sdkMainCameraExecutor();
                if (mainExec && mainExec->isRunning() && sdkMainCameraHandle != nullptr)
                {
                    const SdkDeviceHandle handleSnap = sdkMainCameraHandle;
                    mainExec->post([this, handleSnap]() {
                        SdkCommand getTempCmd;
                        getTempCmd.type = SdkCommandType::Custom;
                        getTempCmd.name = "GetCurrentTemperature";
                        getTempCmd.payload = std::any();
                        // 直接通过设备句柄调用，无需指定驱动名称
                        SdkResult tempRes = SdkManager::instance().callByHandle(handleSnap, getTempCmd);
                        
                        // 回到主线程更新UI
                        if (tempRes.success && tempRes.payload.has_value()) {
                            double temp = std::any_cast<double>(tempRes.payload);
                            QMetaObject::invokeMethod(
                                this,
                                [this, temp]() {
                                    if (wsThread) {
                                        emit wsThread->sendMessageToClient("MainCameraTemperature:" + QString::number(temp));
                                    }
                                },
                                Qt::QueuedConnection);
                        }
                    });
                    // 由于是异步调用，跳过本次同步温度发送（下次异步调用会更新）
                    return;
                }
            }
            else if (dpMainCamera != NULL)
            {
                // INDI 模式：使用 indi_Client 获取温度
                ret = indi_Client->getTemperature(dpMainCamera, CameraTemp);
            }
            
            if (ret == QHYCCD_SUCCESS)
            {
                emit wsThread->sendMessageToClient("MainCameraTemperature:" + QString::number(CameraTemp));
            }
            

        }
    }
    
}

MeridianStatus MainWindow::checkMeridianStatus()
{
    MeridianStatus out;

    if (!dpMount || !dpMount->isConnected())
        return out;

    // 读 PierSide（设备上报的方向侧）
    QString pier = "UNKNOWN";
    indi_Client->getTelescopePierSide(dpMount, pier); // "EAST"/"WEST"/"UNKNOWN"

    // -------- 过中天 ETA（分钟）--------
    // 1) 当前赤经（小时）
    double raH = 0.0, decDeg = 0.0;
    indi_Client->getTelescopeRADECJNOW(dpMount, raH, decDeg);

    // 2) LST 小时（优先 TIME_LST；否则 UTC+经度估算）
    auto norm24 = [](double h){ h=fmod(h,24.0); if(h<0) h+=24.0; return h; };
    // 将可能的度/秒等单位推断并统一为小时，再规范到 [0,24)
    auto toHours = [&](double v)->double {
        if (std::isnan(v)) return v;
        double x = v;
        // 若为秒（0..86400），转换为小时
        if (std::fabs(x) > 24.0 && std::fabs(x) <= 86400.0) x /= 3600.0;
        // 若为度（0..360），转换为小时
        if (std::fabs(x) > 24.0 && std::fabs(x) <= 360.0)  x /= 15.0;
        // 若超过一圈（>360 度等），先按度归一后转小时
        if (std::fabs(x) > 360.0) x = fmod(x, 360.0) / 15.0;
        return norm24(x);
    };
    double lstH = std::numeric_limits<double>::quiet_NaN();

    // 2.1 用 INDI::PropertyNumber 读取 TIME_LST（避免 p->np 报错）
    if (true) {
        INDI::PropertyNumber lst = dpMount->getNumber("TIME_LST");
        if (lst.isValid() && lst.size() > 0) {
            lstH = toHours(lst[0].getValue());   // 统一为小时
        }
    }

    // 2.2 若没有 TIME_LST，则从 GEOGRAPHIC_COORD 取经度，用 UTC 算 LST
    if (std::isnan(lstH)) {
        double lonDeg = std::numeric_limits<double>::quiet_NaN();
        INDI::PropertyNumber geo = dpMount->getNumber("GEOGRAPHIC_COORD");
        if (geo.isValid()) {
            // 通常顺序 LAT(0), LONG(1), ELEV(2)；若你的驱动是命名项，也可用 geo["LONG"].getValue()
            if (geo.size() >= 2)
                lonDeg = geo[1].getValue();
        }
        if (!std::isnan(lonDeg)) {
            const QDateTime utc = QDateTime::currentDateTimeUtc();
            const int Y=utc.date().year(), M=utc.date().month(), D=utc.date().day();
            const int H=utc.time().hour(), Min=utc.time().minute(), S=utc.time().second(), ms=utc.time().msec();

            auto jdUTC = [&](int Y,int M,int D,int H,int Min,int S,int ms)->double{
                int a=(14-M)/12, y=Y+4800-a, m=M+12*a-3;
                long JDN=D+(153*m+2)/5+365*y+y/4-y/100+y/400-32045;
                double dayfrac=(H-12)/24.0 + Min/1440.0 + (S + ms/1000.0)/86400.0;
                return JDN + dayfrac;
            };
            const double JD = jdUTC(Y,M,D,H,Min,S,ms);
            const double Dd = JD - 2451545.0;
            double GMST = 18.697374558 + 24.06570982441908 * Dd; // 小时
            lstH = norm24(GMST + lonDeg/15.0);
        }
    }

    // 清洗 RA 单位并规范到小时
    raH = toHours(raH);

    if (!std::isnan(lstH)) {
        // 采用半开区间 [-12, 12) 规范时角，避免边界抖动
        auto wrap12 = [](double h){ while (h < -12.0) h += 24.0; while (h >= 12.0) h -= 24.0; return h; };
        const double haPrincipal = wrap12(lstH - raH); // 小时；<0 东侧，>0 西侧
        // 连续时角（避免在下中天处从 -12h 跳到 +12h 导致符号翻转）
        static bool hasContHA = false;
        static double contHA = 0.0;
        static double lastHAPrincipal = 0.0;
        if (!hasContHA) {
            contHA = haPrincipal;
            lastHAPrincipal = haPrincipal;
            hasContHA = true;
        } else {
            double delta = haPrincipal - lastHAPrincipal;
            if (delta > 12.0) delta -= 24.0;
            else if (delta < -12.0) delta += 24.0;
            contHA += delta;
            lastHAPrincipal = haPrincipal;
        }
        const bool isPastUpper = (haPrincipal > 0.0);

        // HOME 位也参与翻转判断（不特殊抑制）

        // 基于 |HA|=6h 分割（注意：这里的 6h 是时角 HA，不是 RA）：
        // - 上中天半周区间 |HA| < 6h：  HA ≥ 0 → EAST，HA < 0 → WEST
        // - 下中天半周区间 |HA| ≥ 6h：  映射取反（对称关系）
        constexpr double kHalfCycleHAHours = 6.0;
        constexpr double kBoundaryTolH = 0.02; // ≈1.2 分钟容差
        const bool isLowerRegion = (std::fabs(haPrincipal) >= (kHalfCycleHAHours - kBoundaryTolH));
        bool eastMapping = (haPrincipal >= 0.0);
        if (isLowerRegion) eastMapping = !eastMapping;
        QString theoreticalPier = eastMapping ? "EAST" : "WEST";
        if (pier == "UNKNOWN") {
            out.needsFlip = false; // 无法判断或靠近极区：不触发翻转
        } else {
            out.needsFlip = (pier != theoreticalPier);
        }

        // ETA：严格按连续时角符号（未过为正，已过为负），避免下中天跳变
        out.etaMinutes = (-contHA) * 60.0;

        // 注意：needsFlip 已在上面根据 nearPole 与理论 Pier 计算完毕
    }

    return out;
}


// void MainWindow::saveFitsAsJPG(QString filename)
// {
//     Logger::Log("Starting to save FITS as JPG...", LogLevel::INFO, DeviceType::GUIDER);
//     cv::Mat image;
//     cv::Mat image16;
//     cv::Mat SendImage;
//     Tools::readFits(filename.toLocal8Bit().constData(), image);
//     Logger::Log("FITS file read successfully.", LogLevel::INFO, DeviceType::GUIDER);

//     QList<FITSImage::Star> stars = Tools::FindStarsByStellarSolver(true, true);
//     Logger::Log("Star detection completed.", LogLevel::INFO, DeviceType::GUIDER);

//     if(stars.size() != 0){
//         FWHM = stars[0].HFR;
//         Logger::Log("FWHM calculated from detected stars.", LogLevel::INFO, DeviceType::GUIDER);
//     }
//     else {
//         FWHM = -1;
//         Logger::Log("No stars detected, FWHM set to -1.", LogLevel::WARNING, DeviceType::GUIDER);
//     }

//     if(image16.depth()==8) {
//         image.convertTo(image16, CV_16UC1, 256, 0); //x256  MSB alignment
//         Logger::Log("Image converted to 16-bit format with MSB alignment.", LogLevel::INFO, DeviceType::GUIDER);
//     }
//     else {
//         image.convertTo(image16, CV_16UC1, 1, 0);
//         Logger::Log("Image converted to 16-bit format.", LogLevel::INFO, DeviceType::GUIDER);
//     }

//     if(FWHM != -1){
//         // 在原图上绘制检测结果
//         cv::Point center(stars[0].x, stars[0].y);
//         cv::circle(image16, center, static_cast<int>(FWHM), cv::Scalar(0, 0, 255), 1); // Draw HFR circle
//         cv::circle(image16, center, 1, cv::Scalar(0, 255, 0), -1);                     // Draw center point
//         std::string hfrText = cv::format("%.2f", stars[0].HFR);
//         cv::putText(image16, hfrText, cv::Point(stars[0].x - FWHM, stars[0].y - FWHM - 5), cv::FONT_HERSHEY_SIMPLEX, 0.4, cv::Scalar(0, 0, 255), 1);
//         Logger::Log("Annotations for stars added to image.", LogLevel::INFO, DeviceType::GUIDER);
//     }

//     cv::Mat NewImage = image16;
//     FWHMCalOver = true;

//     cv::normalize(NewImage, SendImage, 0, 255, cv::NORM_MINMAX, CV_8U);    // Normalize new image
//     Logger::Log("Image normalized to 8-bit format.", LogLevel::INFO, DeviceType::GUIDER);

//     QString uniqueId = QUuid::createUuid().toString();
//     Logger::Log("Unique ID generated for new image.", LogLevel::INFO, DeviceType::GUIDER);

//     // 列出所有以"CaptureImage"为前缀的文件
//     QDir directory(QString::fromStdString(vueDirectoryPath));
//     QStringList filters;
//     filters << "CaptureImage*.jpg"; // 使用通配符来筛选以"CaptureImage"为前缀的jpg文件
//     QStringList fileList = directory.entryList(filters, QDir::Files);
//     Logger::Log("Existing image files listed for deletion.", LogLevel::INFO, DeviceType::GUIDER);

//     // 删除所有匹配的文件
//     for (const auto &file : fileList)
//     {
//         QString filePath = QString::fromStdString(vueDirectoryPath) + file;
//         QFile::remove(filePath);
//     }
//     Logger::Log("Old image files deleted.", LogLevel::INFO, DeviceType::GUIDER);

//     // 删除前一张图像文件
//     if (PriorROIImage != "NULL") {
//         QFile::remove(QString::fromStdString(PriorROIImage));
//         Logger::Log("Previous ROI image deleted.", LogLevel::INFO, DeviceType::GUIDER);
//     }

//     // 保存新的图像带有唯一ID的文件名
//     std::string fileName = "CaptureImage_" + uniqueId.toStdString() + ".jpg";
//     std::string filePath = vueDirectoryPath + fileName;
//     bool saved = cv::imwrite(filePath, SendImage);
//     Logger::Log("Attempt to save new image file.", LogLevel::INFO, DeviceType::GUIDER);

//     std::string Command = "sudo ln -sf " + filePath + " " + vueImagePath + fileName;
//     system(Command.c_str());
//     Logger::Log("Symbolic link created for new image file.", LogLevel::INFO, DeviceType::GUIDER);

//     PriorROIImage = vueImagePath + fileName;

//     if (saved)
//     {
//         emit wsThread->sendMessageToClient("SaveJpgSuccess:" + QString::fromStdString(fileName));

//         if(FWHM != -1){
//             dataPoints.append(QPointF(CurrentPosition, FWHM));

//             Logger::Log("dataPoints:" + std::to_string(CurrentPosition) + "," + std::to_string(FWHM), LogLevel::INFO, DeviceType::GUIDER);

//             float a, b, c;
//             Tools::fitQuadraticCurve(dataPoints, a, b, c);

//             if (dataPoints.size() >= 5) {
//                 QVector<QPointF> LineData;

//                 for (float x = CurrentPosition - 3000; x <= CurrentPosition + 3000; x += 10)
//                 {
//                     float y = a * x * x + b * x + c;
//                     LineData.append(QPointF(x, y));
//                 }

//                 // 计算导数为零的 x 坐标
//                 float x_min = -b / (2 * a);
//                 minPoint_X = x_min;
//                 // 计算最小值点的 y 坐标
//                 float y_min = a * x_min * x_min + b * x_min + c;

//                 QString dataString;
//                 for (const auto &point : LineData)
//                 {
//                     dataString += QString::number(point.x()) + "|" + QString::number(point.y()) + ":";
//                 }

//                 R2 = Tools::calculateRSquared(dataPoints, a, b, c);
//                 // qInfo() << "RSquared: " << R2;

//                 emit wsThread->sendMessageToClient("fitQuadraticCurve:" + dataString);
//                 emit wsThread->sendMessageToClient("fitQuadraticCurve_minPoint:" + QString::number(x_min) + ":" + QString::number(y_min));
//             }
//         }
//     }
//     else
//     {
//         Logger::Log("Failed to save image.", LogLevel::ERROR, DeviceType::GUIDER);
//     }
// }

void MainWindow::savePoleMasterPreviewAsJPG(const QString &fitsPath)
{
    Logger::Log("savePoleMasterPreviewAsJPG | start fits=" + fitsPath.toStdString(),
                LogLevel::INFO,
                DeviceType::MAIN);

    cv::Mat image;
    const int status = Tools::readFits(fitsPath.toLocal8Bit().constData(), image);
    if (status != 0 || image.empty())
    {
        Logger::Log("savePoleMasterPreviewAsJPG | failed to read FITS: " + fitsPath.toStdString(),
                    LogLevel::ERROR,
                    DeviceType::MAIN);
        return;
    }

    cv::Mat preview8;
    if (image.depth() == CV_8U)
    {
        preview8 = image;
        Logger::Log("savePoleMasterPreviewAsJPG | 8-bit source, save without stretch size=" +
                        std::to_string(preview8.cols) + "x" + std::to_string(preview8.rows),
                    LogLevel::INFO,
                    DeviceType::MAIN);
    }
    else
    {
        cv::Mat image16;
        if (image.type() == CV_16UC1)
            image16 = image;
        else
        {
            cv::Mat gray;
            if (image.channels() == 3)
                cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
            else if (image.channels() == 4)
                cv::cvtColor(image, gray, cv::COLOR_BGRA2GRAY);
            else
                gray = image;
            if (gray.type() == CV_16UC1)
                image16 = gray;
            else
                gray.convertTo(image16, CV_16UC1);
        }

        uint16_t black = 0;
        uint16_t white = 65535;
        Tools::GetAutoStretch(image16, 0, black, white);

        double minVal = 0.0;
        double maxVal = 0.0;
        cv::minMaxLoc(image16, &minVal, &maxVal);
        if (white <= black || (maxVal > 0.0 && white > std::min(65535.0, maxVal + 4096.0)))
        {
            black = static_cast<uint16_t>(std::max(0.0, std::min(65535.0, minVal)));
            white = static_cast<uint16_t>(std::max<double>(black + 1, std::min(65535.0, maxVal)));
        }

        preview8 = cv::Mat(image16.rows, image16.cols, CV_8UC(image16.channels()));
        Tools::Bit16To8_Stretch(image16, preview8, black, white);
        Logger::Log("savePoleMasterPreviewAsJPG | 16-bit stretch done size=" +
                        std::to_string(preview8.cols) + "x" + std::to_string(preview8.rows) +
                        " black=" + std::to_string(black) +
                        " white=" + std::to_string(white),
                    LogLevel::INFO,
                    DeviceType::MAIN);
    }
    if (preview8.empty())
    {
        Logger::Log("savePoleMasterPreviewAsJPG | stretched preview is empty",
                    LogLevel::ERROR,
                    DeviceType::MAIN);
        return;
    }

    const QString uniqueId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const std::string fileName = "PoleMasterImage_" + uniqueId.toStdString() + ".jpg";
    const std::string filePath = vueDirectoryPath + fileName;
    if (!cv::imwrite(filePath, preview8))
    {
        Logger::Log("savePoleMasterPreviewAsJPG | failed to write JPG: " + filePath,
                    LogLevel::ERROR,
                    DeviceType::MAIN);
        return;
    }

    const std::string command = "sudo ln -sf " + filePath + " " + vueImagePath + fileName;
    system(command.c_str());

    Logger::Log("savePoleMasterPreviewAsJPG | preview ready file=" + fileName +
                    " path=" + filePath +
                    " link=" + vueImagePath + fileName,
                LogLevel::INFO,
                DeviceType::MAIN);

    const QString frameId = QFileInfo(fitsPath).completeBaseName();
    emit wsThread->sendMessageToClient(QString("PoleMasterAlignmentFrameData:%1:%2:%3:%4")
                                           .arg(QString::fromStdString(fileName))
                                           .arg(preview8.cols)
                                           .arg(preview8.rows)
                                           .arg(frameId));

    auto cleanupOldPoleMasterImages = [&](const QString &dirPath, bool includeSymlinks, const QString &protectedFileName) {
        try
        {
            const fs::path dirFsPath = dirPath.toStdString();
            if (!fs::exists(dirFsPath)) return;
            std::vector<fs::path> items;
            const std::string protectedName = protectedFileName.toStdString();
            for (const auto &entry : fs::directory_iterator(dirFsPath))
            {
                const std::string name = entry.path().filename().string();
                if (name.rfind("PoleMasterImage_", 0) != 0 ||
                    name.size() < 4 ||
                    name.compare(name.size() - 4, 4, ".jpg") != 0 ||
                    name == protectedName)
                {
                    continue;
                }
                const bool isLink = fs::is_symlink(entry.symlink_status());
                const bool isFile = fs::is_regular_file(entry.status());
                if ((includeSymlinks && isLink) || (!includeSymlinks && isFile))
                    items.push_back(entry.path());
            }
            std::sort(items.begin(), items.end(), [](const fs::path &a, const fs::path &b) {
                std::error_code eca;
                std::error_code ecb;
                return fs::last_write_time(a, eca) > fs::last_write_time(b, ecb);
            });
            constexpr size_t kKeepRecentPoleMasterImages = 8;
            for (size_t i = kKeepRecentPoleMasterImages; i < items.size(); ++i)
            {
                std::error_code ec;
                fs::remove(items[i], ec);
            }
        }
        catch (...) {}
    };

    const QString protectedName = QString::fromStdString(fileName);
    cleanupOldPoleMasterImages(QString::fromStdString(vueDirectoryPath), false, protectedName);
    cleanupOldPoleMasterImages(QString::fromStdString(vueImagePath), true, protectedName);
}

void MainWindow::readDriversListFromFiles(const std::string &filename, DriversList &drivers_list_from,
                                          std::vector<DevGroup> &dev_groups_from, std::vector<Device> &devices_from)
{
    Logger::Log("Opening XML file: " + filename, LogLevel::INFO, DeviceType::GUIDER);
    QFile file(QString::fromStdString(filename));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        Logger::Log("Failed to open file: " + filename, LogLevel::ERROR, DeviceType::GUIDER);
        return;
    }
    QXmlStreamReader xml(&file);
    while (!xml.atEnd() && !xml.hasError())
    {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == "devGroup")
        {
            DevGroup dev_group;
            dev_group.group = xml.attributes().value("group").toString().toUtf8().constData();
            drivers_list_from.dev_groups.push_back(dev_group);
            Logger::Log("Added device group: " + dev_group.group.toStdString(), LogLevel::INFO, DeviceType::GUIDER);
        }
    }
    DIR *dir = opendir("/usr/share/indi");
    std::string DirPath = "/usr/share/indi/";
    std::string xmlpath;

    int index;

    DriversList drivers_list_get;
    std::vector<DevGroup> dev_groups_get;
    std::vector<Device> devices_get;

    DriversList drivers_list_xmls;
    DriversList drivers_list_xmls_null;
    std::vector<DevGroup> dev_groups_xmls;
    std::vector<Device> devices_xmls;

    std::vector<DevGroup> dev_groups;
    std::vector<Device> devices;

    if (dir == nullptr)
    {
        Logger::Log("Unable to find INDI drivers directory at /usr/share/indi", LogLevel::ERROR, DeviceType::GUIDER);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (strcmp(entry->d_name + strlen(entry->d_name) - 4, ".xml") == 0)
        {
            if (strcmp(entry->d_name + strlen(entry->d_name) - 6, "sk.xml") == 0)
            {
                continue; // Skip sky charts
            }
            else
            {
                xmlpath = DirPath + entry->d_name;
                QFile file(QString::fromStdString(xmlpath));
                if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                {
                    Logger::Log("Failed to open file: " + xmlpath, LogLevel::ERROR, DeviceType::GUIDER);
                }

                QXmlStreamReader xml(&file);

                while (!xml.atEnd() && !xml.hasError())
                {
                    xml.readNext();
                    if (xml.isStartElement() && xml.name() == "devGroup")
                    {
                        DevGroup dev_group;
                        dev_group.group = xml.attributes().value("group").toString().toUtf8().constData();
                        dev_groups.push_back(dev_group);
                        while (!(xml.isEndElement() && xml.name() == "devGroup"))
                        {
                            xml.readNext();
                            if (xml.isStartElement() && xml.name() == "device")
                            {
                                Device device;
                                device.label = xml.attributes().value("label").toString().toStdString();

                                device.manufacturer = xml.attributes().value("manufacturer").toString().toStdString();
                                devices.push_back(device);
                                while (!(xml.isEndElement() && xml.name() == "device"))
                                {
                                    xml.readNext();
                                    if (xml.isStartElement() && xml.name() == "driver")
                                    {
                                        device.driver_name = xml.readElementText().toStdString();
                                    }
                                    else if (xml.isStartElement() && xml.name() == "version")
                                    {
                                        device.version = xml.readElementText().toStdString();
                                    }
                                }
                                dev_group.devices.push_back(device);
                            }
                        }
                        drivers_list_xmls.dev_groups.push_back(dev_group);
                    }
                }
            }
        }
        for (int i = 0; i < drivers_list_xmls.dev_groups.size(); i++)
        {
            for (int j = 0; j < drivers_list_from.dev_groups.size(); j++)
            {
                if (drivers_list_xmls.dev_groups[i].group == drivers_list_from.dev_groups[j].group)
                {
                    for (int k = 0; k < drivers_list_xmls.dev_groups[i].devices.size(); k++)
                    {
                        Device dev;
                        dev.driver_name = drivers_list_xmls.dev_groups[i].devices[k].driver_name;
                        dev.label = drivers_list_xmls.dev_groups[i].devices[k].label;
                        dev.version = drivers_list_xmls.dev_groups[i].devices[k].version;
                        drivers_list_from.dev_groups[j].devices.push_back(dev);
                    }
                }
            }
        }
        drivers_list_xmls = drivers_list_xmls_null;
    }
    closedir(dir);
    Logger::Log("Completed reading and processing INDI driver files.", LogLevel::INFO, DeviceType::GUIDER);
}

//"Telescopes"|"Focusers"|"CCDs"|"Spectrographs"|"Filter Wheels"|"Auxiliary"|"Domes"|"Weather"|"Agent"
void MainWindow::printDevGroups2(const DriversList drivers_list, int ListNum, QString group)
{
    Logger::Log("Printing device groups for group: " + group.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("=============================== Print DevGroups ===============================", LogLevel::INFO, DeviceType::MAIN);
    bool foundGroup = false;
    for (int i = 0; i < drivers_list.dev_groups.size(); i++)
    {
        if (drivers_list.dev_groups[i].group == group)
        {
            foundGroup = true;
            Logger::Log("Processing device group: " + drivers_list.dev_groups[i].group.toStdString(), LogLevel::INFO, DeviceType::MAIN);
            // Uncomment and modify the following lines if you want to log device details and send messages
            // for (int j = 0; j < drivers_list.dev_groups[i].devices.size(); j++)
            // {
            //     qDebug() << QString::fromStdString(drivers_list.dev_groups[i].devices[j].driver_name) << QString::fromStdString(drivers_list.dev_groups[i].devices[j].version) << QString::fromStdString(drivers_list.dev_groups[i].devices[j].label);
            //     Logger::Log("Device details: " + drivers_list.dev_groups[i].devices[j].label + ", " + drivers_list.dev_groups[i].devices[j].driver_name + ", " + drivers_list.dev_groups[i].devices[j].version, LogLevel::INFO, DeviceType::MAIN);
            //     websocket->messageSend("AddDriver:"+QString::fromStdString(drivers_list.dev_groups[i].devices[j].label)+":"+QString::fromStdString(drivers_list.dev_groups[i].devices[j].driver_name));
            // }
            DeviceSelect(ListNum, i);
        }
    }
    if (!foundGroup)
    {
        systemdevicelist.currentDeviceCode = -1;
        ::drivers_list.selectedGrounp = -1;
        Logger::Log("printDevGroups2 | Device group not found: " + group.toStdString() +
                        ", currentDeviceCode reset to -1",
                    LogLevel::ERROR, DeviceType::MAIN);
    }
    Logger::Log("Completed printing device groups.", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::DeviceSelect(int systemNumber, int grounpNumber)
{
    // Tools::clearSystemDeviceListItem(systemdevicelist, systemNumber);
    SelectIndiDevice(systemNumber, grounpNumber);
}

void MainWindow::SelectIndiDevice(int systemNumber, int grounpNumber)
{
    if (!isValidSystemDeviceIndex(systemdevicelist, systemNumber))
    {
        systemdevicelist.currentDeviceCode = -1;
        drivers_list.selectedGrounp = -1;
        Logger::Log("SelectIndiDevice | Invalid systemNumber: " + std::to_string(systemNumber),
                    LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    if (grounpNumber < 0 || grounpNumber >= drivers_list.dev_groups.size())
    {
        systemdevicelist.currentDeviceCode = -1;
        drivers_list.selectedGrounp = -1;
        Logger::Log("SelectIndiDevice | Invalid grounpNumber: " + std::to_string(grounpNumber),
                    LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    systemdevicelist.currentDeviceCode = systemNumber;
    drivers_list.selectedGrounp = grounpNumber;

    // switch (systemNumber)
    // {
    // case 0:
    //     systemdevicelist.system_devices[systemNumber].Description = "Mount";
    //     break;
    // case 1:
    //     systemdevicelist.system_devices[systemNumber].Description = "Guider";
    //     break;
    // case 2:
    //     systemdevicelist.system_devices[systemNumber].Description = "PoleCamera";
    //     break;
    // case 20:
    //     systemdevicelist.system_devices[systemNumber].Description = "Main Camera #1";
    //     break;
    // case 21:
    //     systemdevicelist.system_devices[systemNumber].Description = "CFW #1";
    //     break;
    // case 22:
    //     systemdevicelist.system_devices[systemNumber].Description = "Focuser #1";
    //     break;

    // default:
    //     break;
    // }

    // qDebug() << "SelectIndiDevice:" << systemdevicelist.currentDeviceCode << "," << drivers_list.selectedGrounp;

    for (int i = 0; i < drivers_list.dev_groups[grounpNumber].devices.size(); i++)
    {
        if (grounpNumber == 1 && (QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].label) == "QHY CCD" || QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].driver_name) == "indi_qhy_ccd"))
        {
            continue;
        }
        if (grounpNumber == 20 && (QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].label) == "QHY CCD2" || QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].driver_name) == "indi_qhy_ccd2"))
        {
            continue;
        }
        // qDebug() << QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].driver_name) << QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].version) << QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].label) << QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].manufacturer);
        emit wsThread->sendMessageToClient("AddDriver:" + QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].label) + ":" + QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].driver_name));
        // qDebug() << "AddDriver:" + QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].label) + ":" + QString::fromStdString(drivers_list.dev_groups[grounpNumber].devices[i].driver_name);
    }
}

bool MainWindow::indi_Driver_Confirm(QString DriverName, QString BaudRate)
{
    if (!isValidSystemDeviceIndex(systemdevicelist, systemdevicelist.currentDeviceCode))
    {
        Logger::Log("indi_Driver_Confirm | currentDeviceCode out of bounds: " + std::to_string(systemdevicelist.currentDeviceCode),
                    LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    switch (systemdevicelist.currentDeviceCode)
    {
    case 0:
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "Mount";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].BaudRate = BaudRate.toInt();
        Logger::Log("indi_Driver_Confirm | Mount | DriverName: " + DriverName.toStdString() + " BaudRate: " + std::to_string(BaudRate.toInt()), LogLevel::INFO, DeviceType::MAIN);
        break;
    case 1:
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "Guider";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].BaudRate = BaudRate.toInt();
        Logger::Log("indi_Driver_Confirm | Guider | DriverName: " + DriverName.toStdString() + " BaudRate: " + std::to_string(BaudRate.toInt()), LogLevel::INFO, DeviceType::MAIN);
        break;
    case 2:
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "PoleCamera";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].BaudRate = BaudRate.toInt();
        Logger::Log("indi_Driver_Confirm | PoleCamera | DriverName: " + DriverName.toStdString() + " BaudRate: " + std::to_string(BaudRate.toInt()), LogLevel::INFO, DeviceType::MAIN);
        break;
    case 20:
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "MainCamera";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].BaudRate = BaudRate.toInt();
        Logger::Log("indi_Driver_Confirm | MainCamera | DriverName: " + DriverName.toStdString() + " BaudRate: " + std::to_string(BaudRate.toInt()), LogLevel::INFO, DeviceType::MAIN);
        break;
    case 21:
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "CFW";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].BaudRate = BaudRate.toInt();
        Logger::Log("indi_Driver_Confirm | CFW | DriverName: " + DriverName.toStdString() + " BaudRate: " + std::to_string(BaudRate.toInt()), LogLevel::INFO, DeviceType::MAIN);
        break;
    case 22:
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].Description = "Focuser";
        systemdevicelist.system_devices[systemdevicelist.currentDeviceCode].BaudRate = BaudRate.toInt();
        Logger::Log("indi_Driver_Confirm | Focuser | DriverName: " + DriverName.toStdString() + " BaudRate: " + std::to_string(BaudRate.toInt()), LogLevel::INFO, DeviceType::MAIN);
        break;

    default:
        Logger::Log("indi_Driver_Confirm | Invalid currentDeviceCode: " + std::to_string(systemdevicelist.currentDeviceCode), LogLevel::ERROR, DeviceType::MAIN);
        break;
    }

    auto &slot = systemdevicelist.system_devices[systemdevicelist.currentDeviceCode];
    slot.DriverIndiName = DriverName;
    slot.isConnect = false;
    slot.isBind = false;

    // 🔥 自动从 SdkDriverRegistry 查询是否支持 SDK 模式
    bool supportsSDK = SdkDriverRegistry::instance().supportsSDK(DriverName.toStdString());
    
    if (supportsSDK)
    {
        // 获取 SDK 首选名称
        std::string sdkDriverName = SdkDriverRegistry::instance().getSDKDriverName(
            DriverName.toStdString()
        );
        
        // 标记支持 SDK（用于前端显示"连接模式"切换选项）
        if (!slot.DriverFrom.contains("SDK", Qt::CaseInsensitive))
        {
            slot.DriverFrom = DriverName + "SDK";  // 例如 "indi_qhy_ccdSDK"
        }

        // 🆕 保存 SDK 驱动名（用于后续切换到 SDK 模式时自动选择正确的驱动）
        slot.SDKDriverName = QString::fromStdString(sdkDriverName);
        
        Logger::Log("indi_Driver_Confirm | Driver supports SDK: " +
                   DriverName.toStdString() + " -> " + sdkDriverName,
                   LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        // 纯 INDI 驱动，不支持 SDK
        slot.DriverFrom = "INDI";
        slot.SDKDriverName = "";
        // 若驱动不支持 SDK，则强制切回 INDI 模式，避免沿用上次的 isSDKConnect=true 导致后续 ConnectDriver 误走 SDK 流程
        slot.isSDKConnect = false;
        
        Logger::Log("indi_Driver_Confirm | Driver is INDI-only (no SDK support): " +
                   DriverName.toStdString(),
                   LogLevel::INFO, DeviceType::MAIN);
    }

    // 持久化：否则重启/重连后 DriverFrom 会丢失，前端收到 SelectedDriverList(...:false:...)
    Tools::saveSystemDeviceList(systemdevicelist);

    // 立即刷新前端缓存：让 supportSDK/connectionMode 立刻生效，UI 及时显示"连接模式"下拉框
    loadSelectedDriverList();

    return true;
}

QString MainWindow::getSDKDriverName(const QString& deviceType)
{
    // 根据设备类型找到对应的槽位索引
    int index = -1;
    if (deviceType == "MainCamera") index = 20;
    // Guider（导星相机）在 system_devices[1]
    else if (deviceType == "Guider" || deviceType == "GuideCamera") index = 1;
    // PoleCamera（电子极轴镜）在 system_devices[2]
    else if (deviceType == "PoleCamera") index = 2;
    // CFW（外置滤镜轮）在 system_devices[21]
    else if (deviceType == "CFW") index = 21;
    else if (deviceType == "Focuser") index = 22;
    // ... 可以继续添加其他设备类型的映射
    
    if (index < 0 || index >= systemdevicelist.system_devices.size())
        return "";
    
    const auto& device = systemdevicelist.system_devices[index];
    
    // 🔥 关键：直接从 SdkDriverRegistry 查询
    if (!device.DriverIndiName.isEmpty())
    {
        std::string sdkDriver = SdkDriverRegistry::instance().getSDKDriverName(
            device.DriverIndiName.toStdString()
        );
        
        if (!sdkDriver.empty())
            return QString::fromStdString(sdkDriver);
    }
    

    
    return "";
}

bool MainWindow::indi_Driver_Clear(int deviceCode)
{
    if (!isValidSystemDeviceIndex(systemdevicelist, deviceCode)) {
        Logger::Log("indi_Driver_Clear | deviceCode out of bounds: " + std::to_string(deviceCode), LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    systemdevicelist.system_devices[deviceCode].Description = "";
    systemdevicelist.system_devices[deviceCode].DriverIndiName = "";
    systemdevicelist.system_devices[deviceCode].SDKDriverName = "";
    systemdevicelist.system_devices[deviceCode].BaudRate = 9600;
    systemdevicelist.system_devices[deviceCode].DeviceIndiName = "";
    systemdevicelist.system_devices[deviceCode].DeviceIndiGroup = -1;
    systemdevicelist.system_devices[deviceCode].isConnect = false;
    systemdevicelist.system_devices[deviceCode].isBind = false;
    systemdevicelist.system_devices[deviceCode].isSDKConnect = false;
    systemdevicelist.system_devices[deviceCode].dp = nullptr;

    if (systemdevicelist.currentDeviceCode == deviceCode)
        systemdevicelist.currentDeviceCode = -1;
    if (drivers_list.selectedGrounp >= 0)
        drivers_list.selectedGrounp = -1;

    // 保存配置到文件，确保清除操作持久化
    Tools::saveSystemDeviceList(systemdevicelist);

    // 发送更新后的驱动列表给前端，确保前端UI同步更新
    loadSelectedDriverList();

    Logger::Log("indi_Driver_Clear | Driver cleared for deviceCode=" + std::to_string(deviceCode) +
                    " and configuration saved",
                LogLevel::INFO, DeviceType::MAIN);
    return true;
}

void MainWindow::indi_Device_Confirm(QString DeviceName, QString DriverName)
{
    //   qApp->processEvents();

    int deviceCode;
    deviceCode = systemdevicelist.currentDeviceCode;

    if (!isValidSystemDeviceIndex(systemdevicelist, deviceCode))
    {
        Logger::Log("indi_Device_Confirm | currentDeviceCode out of bounds: " + std::to_string(deviceCode),
                    LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    systemdevicelist.system_devices[deviceCode].DriverIndiName = DriverName;
    systemdevicelist.system_devices[deviceCode].DeviceIndiGroup = drivers_list.selectedGrounp;
    systemdevicelist.system_devices[deviceCode].DeviceIndiName = DeviceName;

    Logger::Log("system device(" + DeviceName.toStdString() + ") successfully selected", LogLevel::INFO, DeviceType::MAIN);

    Tools::printSystemDeviceList(systemdevicelist);

    Tools::saveSystemDeviceList(systemdevicelist);
}

uint32_t MainWindow::clearCheckDeviceExist(QString drivername, bool &isExist)
{
    Logger::Log("Stopping all INDI drivers.", LogLevel::INFO, DeviceType::MAIN);
    Tools::stopIndiDriverAll(drivers_list);
    Logger::Log("Starting INDI driver: " + drivername.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    Tools::startIndiDriver(drivername);

    sleep(1); // must wait some time here

    MyClient *searchClient;
    searchClient = new MyClient();
    Logger::Log("Initialized new MyClient for device search.", LogLevel::INFO, DeviceType::MAIN);
    searchClient->PrintDevices();

    searchClient->setServer("localhost", 7624);
    searchClient->setConnectionTimeout(3, 0);
    searchClient->ClearDevices(); // clear device list

    Logger::Log("Attempting to connect to INDI server at localhost:7624", LogLevel::INFO, DeviceType::MAIN);
    bool connected = searchClient->connectServer();

    if (connected == false)
    {
        Logger::Log("Failed to connect to INDI server, can not find server", LogLevel::ERROR, DeviceType::MAIN);
        return QHYCCD_ERROR;
    }

    sleep(1); // connect server will generate the callback of newDevice and then put the device into list. this need take some time and it is non-block
    searchClient->PrintDevices();

    if (searchClient->GetDeviceCount() == 0)
    {
        Logger::Log("No devices found on INDI server.", LogLevel::INFO, DeviceType::MAIN);
        searchClient->disconnectServer();
        isExist = false;
        emit wsThread->sendMessageToClient("ScanFailed:No device found.");
        return QHYCCD_SUCCESS;
    }

    Logger::Log("Devices found: " + std::to_string(searchClient->GetDeviceCount()), LogLevel::INFO, DeviceType::MAIN);
    for (int i = 0; i < searchClient->GetDeviceCount(); i++)
    {
        emit wsThread->sendMessageToClient("AddDevice:" + QString::fromStdString(searchClient->GetDeviceNameFromList(i)));
    }

    searchClient->disconnectServer();
    searchClient->ClearDevices();

    Tools::stopIndiDriver(drivername);
    Logger::Log("INDI driver stopped: " + drivername.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    return QHYCCD_SUCCESS;
}

void MainWindow::disconnectIndiServer(MyClient *client)
{
    Logger::Log("disconnectIndiServer start ...", LogLevel::INFO, DeviceType::MAIN);
    // 防御性检查：客户端指针为空则直接返回，避免段错误
    if (client == nullptr)
    {
        Logger::Log("disconnectIndiServer | client is nullptr", LogLevel::ERROR, DeviceType::MAIN);
        Tools::stopIndiDriverAll(drivers_list);
        ConnectDriverList.clear();
        return;
    }

    int deviceCount = client->GetDeviceCount();
    if (deviceCount > 0)
    {
        for (int i = 0; i < deviceCount; i++)
        {
            INDI::BaseDevice *device = client->GetDeviceFromList(i);
            if (device == nullptr)
            {
                Logger::Log("disconnectAllDevice | Device at index " + std::to_string(i) + " is nullptr", LogLevel::WARNING, DeviceType::MAIN);
                continue;
            }

            if (device->isConnected())
            {
                const char *devName = device->getDeviceName();
                QString qName = devName ? QString::fromUtf8(devName) : QString("UnknownDevice");

                client->disconnectDevice(devName ? devName : "");
                int num = 0;
                while (device->isConnected())
                {
                    Logger::Log("disconnectAllDevice | Waiting for disconnect device (" + qName.toStdString() + ") finish...", LogLevel::INFO, DeviceType::MAIN);
                    sleep(1);
                    num++;

                    if (num > 10)
                    {
                        Logger::Log("disconnectAllDevice | device (" + qName.toStdString() + ") disconnect failed.", LogLevel::WARNING, DeviceType::MAIN);
                        break;
                    }
                }
                Logger::Log("disconnectAllDevice | device (" + qName.toStdString() + ") disconnected successfully.", LogLevel::INFO, DeviceType::MAIN);
            }
        }
    }
    else
    {
        Logger::Log("disconnectIndiServer | no devices to disconnect (device count = 0)", LogLevel::INFO, DeviceType::MAIN);
    }

    Tools::stopIndiDriverAll(drivers_list);
    ConnectDriverList.clear();

    client->ClearDevices();
    client->disconnectServer();
    int k = 10;
    while (k--)
    {
        if (!client->isServerConnected())
        {
            Logger::Log("Server disconnected successfully.", LogLevel::INFO, DeviceType::MAIN);
            break;
        }
        sleep(1);
        // qApp->processEvents();
        Logger::Log("Waiting for server to disconnect...", LogLevel::INFO, DeviceType::MAIN);
    }
    Logger::Log("disconnectServer finished.", LogLevel::INFO, DeviceType::MAIN);
    if (indi_Client != nullptr)
    {
        indi_Client->PrintDevices();
    }
}

void MainWindow::cleanupQhySdkPoolAndResource(const QString& reason, const QString& deviceType)
{
    // -----------------------------
    // 约定：system_devices 中的槽位索引（避免魔法数字）
    // -----------------------------
    constexpr int kIdxMainCamera = 20;
    constexpr int kIdxFocuser    = 22;

    // -----------------------------
    // 1) 解析清理范围（Plan）
    // -----------------------------
    const bool cleanupAll        = (deviceType == "All");
    const bool cleanupMainCamera = cleanupAll || (deviceType == "MainCamera");
    const bool cleanupPoleCamera = cleanupAll || (deviceType == "PoleCamera");
    const bool cleanupFocuser    = cleanupAll || (deviceType == "Focuser");
    const bool cleanupCameraPool = cleanupAll || (deviceType == "CameraPool");

    // 注释（与实际行为一致）：
    // - MainCamera：仅清理主相机（句柄+绑定/运行态），不触碰其他相机句柄
    // - PoleCamera：SDK 极轴镜与主相机/导星相机共享同一相机池，因此按 CameraPool 清理
    // - CameraPool：清理整个相机池（关闭所有句柄 + ReleaseSdkResource），池清空后主相机绑定必然无效，因此也会复位主相机绑定/运行态

    const bool hasAnyCameraHandle =
        (sdkMainCameraHandle != nullptr) || (sdkGuiderHandle != nullptr) ||
        (sdkPoleScopeHandle != nullptr) || (!g_sdkQhyCamHandles.isEmpty());
    const bool hasFocuserHandle   = (sdkFocuserHandle != nullptr);

    const bool shouldCleanupCamera =
        (cleanupMainCamera || cleanupPoleCamera || cleanupCameraPool) && hasAnyCameraHandle;
    const bool shouldCleanupFocuser =
        cleanupFocuser && hasFocuserHandle;

    // 如果既没有相机也没有电调需要清理，直接返回
    if (!shouldCleanupCamera && !shouldCleanupFocuser)
        return;

    Logger::Log("cleanupQhySdkPoolAndResource | reason=" + reason.toStdString() +
                ", deviceType=" + deviceType.toStdString(),
                LogLevel::INFO, DeviceType::MAIN);

    // -----------------------------
    // 2) 小工具：统一线程投递（可读性 + 去重）
    // -----------------------------
    auto runOnCamThreadSync = [&](std::function<void()> fn) {
        if (sdkCamExec && sdkCamExec->isRunning())
            sdkCamExec->postAndWait(std::move(fn));
        else
            fn();
    };

    auto runOnFocuserThreadSync = [&](std::function<void()> fn) {
        if (sdkFocuserExec && sdkFocuserExec->isRunning())
            sdkFocuserExec->postAndWait(std::move(fn));
        else
            fn();
    };

    auto resetDeviceEntry = [&](int index) {
        if (index < 0 || index >= systemdevicelist.system_devices.size())
            return;
        auto &d = systemdevicelist.system_devices[index];
        d.isConnect = false;
        d.isBind = false;
        d.DeviceIndiName.clear();
        d.dp = NULL;
    };

    auto resetMainCameraRuntimeState = [&]() {
        // 运行态/前后端状态统一回到空闲，避免残留“曝光中”
        glMainCameraStatu = "IDLE";
        ShootStatus = "IDLE";
        glIsFocusingLooping = false;
        isFocusLoopShooting = false;
    };

    auto makeCancelExposureCmd = [&]() {
        SdkCommand cmd;
        cmd.type = SdkCommandType::Custom;
        cmd.name = "CancelExposure";
        cmd.payload = std::any();
        return cmd;
    };

    auto cancelAndCloseCamera = [&](SdkDeviceHandle h) {
        if (h == nullptr) return;
        // 直接通过设备句柄调用，无需指定驱动名称
        SdkManager::instance().callByHandle(h, makeCancelExposureCmd());
        SdkManager::instance().closeByHandle(h);
    };

    // 释放 SDK 全局资源需要“驱动名”；优先 MainCamera，其次 Guider，最后兜底 QHYCCD（若已注册）
    const std::string releaseDriverNameStd = [&]() -> std::string {
        QString dn = getSDKDriverName("MainCamera");
        if (dn.isEmpty())
            dn = getSDKDriverName("Guider");
        if (dn.isEmpty())
            dn = getSDKDriverName("PoleCamera");
        if (!dn.isEmpty())
            return dn.toStdString();

        // 兜底：若驱动映射缺失（比如只配置了导星/未配置主相机），仍尽量释放
        auto regs = SdkManager::instance().listRegisteredDrivers();
        for (const auto &n : regs)
        {
            if (n == "QHYCCD")
                return n;
        }
        if (!regs.empty())
            return regs.front();
        return {};
    }();

    // -----------------------------
    // 3) 先清理电调（与相机资源独立）
    // -----------------------------
    if (shouldCleanupFocuser)
    {
        const SdkDeviceHandle h = sdkFocuserHandle;

        runOnFocuserThreadSync([h]() {
            // 直接通过设备句柄关闭，无需指定驱动名称
            SdkManager::instance().closeByHandle(h);
        });

        // 本地状态复位
        sdkFocuserHandle = nullptr;
        sdkFocuserPort.clear();

        // 设备表复位（电调）
        resetDeviceEntry(kIdxFocuser);
    }

    // -----------------------------
    // 4) 清理相机（仅主相机 / 整池）
    // -----------------------------
    if (shouldCleanupCamera)
    {
        const bool cleanupFullPool = cleanupCameraPool || cleanupPoleCamera; // CameraPool / PoleCamera / All

        if (!cleanupFullPool && deviceType == "MainCamera")
        {
            // 4A) 仅清理主相机：cancel + close 主句柄，并从池中摘除（不触碰其他相机）
            if (sdkMainCameraHandle != nullptr)
            {
                const SdkDeviceHandle mainHandle = sdkMainCameraHandle;
                const int poolIndex = g_sdkMainCameraPoolIndex;

                runOnCamThreadSync([=]() {
                    // 直接通过设备句柄调用，无需指定驱动名称
                    SdkManager::instance().callByHandle(mainHandle, makeCancelExposureCmd());
                    SdkManager::instance().closeByHandle(mainHandle);
                });

                // 从池中摘除主相机（如果它在池中）
                if (poolIndex >= 0 && poolIndex < g_sdkQhyCamHandles.size())
                {
                    g_sdkQhyCamHandles[poolIndex] = nullptr;
                    if (poolIndex < g_sdkQhyCamIds.size())
                        g_sdkQhyCamIds[poolIndex].clear();
                }
            }

            // 主相机绑定/运行态复位
            sdkMainCameraHandle = nullptr;
            sdkGuiderHandle = nullptr;
            sdkPoleScopeHandle = nullptr;
            g_sdkMainCameraPoolIndex = -1;
            g_sdkGuiderPoolIndex = -1;
            g_sdkPoleCameraPoolIndex = -1;
            sdkMainCameraId.clear();
            resetMainCameraRuntimeState();

            // 设备表复位（主相机）
            resetDeviceEntry(kIdxMainCamera);
        }
        else
        {
            // 4B) 清理整个相机池：停止轮询 -> 关闭所有句柄 -> ReleaseSdkResource -> 清空池 -> 复位状态
            //
            // 注意：池被清理后主相机绑定也必然失效，因此会一并复位主相机绑定/运行态。
            if (sdkExposureTimer)
            {
                // 若 sdkExposureTimer 的线程归属不明确，建议用 invokeMethod 投递到其线程
                sdkExposureTimer->stop();
            }
            sdkExposureIsROI = false;

            std::vector<SdkDeviceHandle> handles;
            handles.reserve(static_cast<size_t>(g_sdkQhyCamHandles.size()));

            for (int i = 0; i < g_sdkQhyCamHandles.size(); ++i)
            {
                if (g_sdkQhyCamHandles[i] != nullptr)
                    handles.push_back(g_sdkQhyCamHandles[i]);
                g_sdkQhyCamHandles[i] = nullptr;
            }

            runOnCamThreadSync([=]() mutable {
                // 关闭所有句柄（尽量先取消曝光）
                for (auto h : handles)
                {
                    if (h == nullptr) continue;
                    // 直接通过设备句柄调用，无需指定驱动名称
                    SdkManager::instance().callByHandle(h, makeCancelExposureCmd());
                    SdkManager::instance().closeByHandle(h);
                }
                // 释放 SDK 全局资源（必须在全部 close 之后）
                if (!releaseDriverNameStd.empty())
                {
                    SdkCommand relCmd;
                    relCmd.type = SdkCommandType::Custom;
                    relCmd.name = "ReleaseSdkResource";
                    relCmd.payload = std::any();

                    SdkResult relRes = SdkManager::instance().call(releaseDriverNameStd, nullptr, relCmd);
                    if (!relRes.success)
                    {
                        Logger::Log("cleanupQhySdkPoolAndResource | ReleaseSdkResource failed: " + relRes.message,
                                    LogLevel::WARNING, DeviceType::MAIN);
                    }
                    else
                    {
                        Logger::Log("cleanupQhySdkPoolAndResource | ReleaseSdkResource success",
                                    LogLevel::INFO, DeviceType::MAIN);
                    }
                }
                else
                {
                    Logger::Log("cleanupQhySdkPoolAndResource | ReleaseSdkResource skipped: no valid SDK driver name",
                                LogLevel::WARNING, DeviceType::MAIN);
                }
            });

            // 清空池与 ID 列表
            g_sdkQhyCamHandles.clear();
            g_sdkQhyCamIds.clear();

            // 主相机绑定/运行态复位
            sdkMainCameraHandle = nullptr;
            sdkGuiderHandle = nullptr;
            sdkPoleScopeHandle = nullptr;
            g_sdkMainCameraPoolIndex = -1;
            g_sdkGuiderPoolIndex = -1;
            g_sdkPoleCameraPoolIndex = -1;
            sdkMainCameraId.clear();
            resetMainCameraRuntimeState();

            // 设备表复位：
            // CameraPool/All 都应清理“相机角色”绑定状态，避免仅清主相机导致 Guider/PoleCamera 残留“已连接”。
            for (int i = 0; i < systemdevicelist.system_devices.size(); ++i)
            {
                if (!systemdevicelist.system_devices[i].isSDKConnect)
                    continue;
                if (i == kIdxFocuser)
                    continue;
                const QString desc = systemdevicelist.system_devices[i].Description;
                if (desc == "MainCamera" || desc == "Guider" || desc == "PoleCamera")
                    resetDeviceEntry(i);
            }
        }
    }
    else if (cleanupMainCamera)
    {
        // 4C) 没有可关闭的相机句柄，但仍要求“解绑主相机”（用于异常状态/句柄已丢失）
        sdkMainCameraHandle = nullptr;
        sdkGuiderHandle = nullptr;
        sdkPoleScopeHandle = nullptr;
        g_sdkMainCameraPoolIndex = -1;
        g_sdkGuiderPoolIndex = -1;
        g_sdkPoleCameraPoolIndex = -1;
        sdkMainCameraId.clear();
        resetMainCameraRuntimeState();
        resetDeviceEntry(kIdxMainCamera);
    }

    // ReleaseSdkResource 已在“整池清理”路径中由相机线程任务负责执行
}


bool MainWindow::connectIndiServer(MyClient *client)
{
    Logger::Log("connectIndiServer start ...", LogLevel::INFO, DeviceType::MAIN);
    client->setConnectionTimeout(3, 0);
    Logger::Log("connectIndiServer | clear device list ...", LogLevel::INFO, DeviceType::MAIN);
    client->ClearDevices(); // clear device list
    Logger::Log("connectIndiServer | connect server ...", LogLevel::INFO, DeviceType::MAIN);
    client->connectServer();
    int k = 10;
    while (k--)
    {
        if (client->isServerConnected() == true)
        {
            break;
        }
        sleep(1);
        // qApp->processEvents();
        Logger::Log("connectIndiServer | waiting for client connected ...", LogLevel::INFO, DeviceType::MAIN);
    }
    if (client->isServerConnected() == false)
    {
        Logger::Log("connectIndiServer | failed: client is not connected after timeout.", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    sleep(1);
    client->PrintDevices();
    Logger::Log("connectIndiServer finished.", LogLevel::INFO, DeviceType::MAIN);
    return true;
}

void MainWindow::ClearSystemDeviceList()
{
    Logger::Log("ClearSystemDeviceList start ...", LogLevel::INFO, DeviceType::MAIN);
    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        systemdevicelist.system_devices[i].DeviceIndiGroup = -1;
        systemdevicelist.system_devices[i].DeviceIndiName = "";
        systemdevicelist.system_devices[i].DriverFrom = "";
        // systemdevicelist.system_devices[i].DriverIndiName = "";
        systemdevicelist.system_devices[i].isConnect = false;
        systemdevicelist.system_devices[i].dp = NULL;
        systemdevicelist.system_devices[i].isBind = false;
        // systemdevicelist.system_devices[i].Description = "";
    }
    Logger::Log("ClearSystemDeviceList finished.", LogLevel::INFO, DeviceType::MAIN);
    Tools::printSystemDeviceList(systemdevicelist);
}

void MainWindow::SDK_BurstCapture(int Exp_ms, int frames)
{
    Logger::Log("SDK_BurstCapture start ...", LogLevel::INFO, DeviceType::CAMERA);

    // 仅用于主相机 SDK，且仅 QHYCCD 支持 Burst（Live）模式
    const bool isMainCameraSDK =
        (systemdevicelist.system_devices.size() > 20 &&
         systemdevicelist.system_devices[20].isSDKConnect &&
         sdkMainCameraHandle != nullptr);

    if (!isMainCameraSDK) {
        Logger::Log("SDK_BurstCapture | Main camera is not in SDK mode, fallback to startMainCameraCapture",
                    LogLevel::WARNING, DeviceType::CAMERA);
        startMainCameraCapture(Exp_ms);
        return;
    }

    const QString sdkDriverNameRaw =
        (systemdevicelist.system_devices.size() > 20) ? systemdevicelist.system_devices[20].SDKDriverName : "";
    QString sdkDriverName = sdkDriverNameRaw.trimmed();
    if (sdkDriverName.isEmpty())
        sdkDriverName = getSDKDriverName("MainCamera").trimmed();
    if (sdkDriverName.isEmpty() && systemdevicelist.system_devices.size() > 20)
        sdkDriverName = systemdevicelist.system_devices[20].DriverIndiName.trimmed();

    auto isQhySdkDriverName = [](const QString& n) -> bool {
        const QString s = n.trimmed().toLower();
        return (s == "qhyccd" || s == "indi_qhy_ccd");
    };
    if (!isQhySdkDriverName(sdkDriverName)) {
        Logger::Log("SDK_BurstCapture | SDK driver is not QHYCCD/indi_qhy_ccd (" + sdkDriverName.toStdString() +
                        "), fallback to startMainCameraCapture",
                    LogLevel::WARNING, DeviceType::CAMERA);
        startMainCameraCapture(Exp_ms);
        return;
    }

    // 参数整理
    if (Exp_ms <= 0) Exp_ms = 1;
    if (frames <= 0) frames = 1;
    if (frames > 1024) frames = 1024; // 防御：避免极端值导致内存/耗时不可控

    // Burst 作为“连接模式子模式”：确保已在连接阶段进入 Burst/Live/IDLE
    if (mainCameraCaptureMode != MainCameraCaptureMode::Burst) {
        mainCameraCaptureMode = MainCameraCaptureMode::Burst;
    }
    applySdkMainCameraCaptureMode();

    // 若 SDK 执行线程不可用，直接失败（避免在主线程做阻塞式 SDK 调用）
    SdkSerialExecutor *mainExec = sdkMainCameraExecutor();
    if (!mainExec || !mainExec->isRunning()) {
        Logger::Log("SDK_BurstCapture | sdkMainCamExec not running", LogLevel::ERROR, DeviceType::CAMERA);
        emit wsThread->sendMessageToClient("ExposureFailed:SDK worker not running");
        emit wsThread->sendMessageToClient("CameraInExposuring:False");
        glMainCameraStatu = "IDLE";
        ShootStatus = "IDLE";
        return;
    }

    // 若正在进行普通 SDK 曝光轮询，先停止，避免竞争同一相机
    if (sdkExposureTimer && sdkExposureTimer->isActive()) {
        sdkExposureTimer->stop();
    }
    sdkFrameTaskInFlight = false;
    sdkExposureIsROI = false;

    glIsFocusingLooping = false;
    isSavePngSuccess = false;
    glMainCameraStatu = "Exposuring";
    ShootStatus = "Exposuring";

    sdkBurstActive = true;
    sdkBurstCancelRequested = false;

    // 将 SDK 错误信息转换为“可读原因”
    auto makeUserFriendlySdkReason = [](const QString& step, const QString& sdkMsg) -> QString {
        const QString raw = sdkMsg.trimmed();
        QString msg = raw;
        const int ecIdx = msg.lastIndexOf("error code", -1, Qt::CaseInsensitive);
        if (ecIdx >= 0)
        {
            msg = msg.left(ecIdx).trimmed();
            while (msg.endsWith(',')) msg.chop(1);
            msg = msg.trimmed();
        }
        if (msg.isEmpty()) msg = raw;
        return step + "：" + msg;
    };

    const int expMsSnap = Exp_ms;
    const int framesSnap = frames;

    mainExec->post([this, expMsSnap, framesSnap, makeUserFriendlySdkReason]() mutable {
        // 线程内执行：Burst 模式下“只触发 + 抓帧”，不重复 Begin/Stop Live
        QString failReason;
        bool cancelled = false;
        std::shared_ptr<SdkFrameData> outFrame = std::make_shared<SdkFrameData>();

        // 主相机在 applySdkMainCameraCaptureMode() 中可能触发 “closeById->open->register”，
        // 因此这里不要使用固定的 handleSnap；每次操作前从注册表读取当前有效句柄。
        auto waitMainCameraReady = [this](int timeoutMs, SdkDeviceInfo& outDev) -> bool {
            const auto t0 = std::chrono::steady_clock::now();
            while (true) {
                if (sdkBurstCancelRequested.load())
                    return false;
                outDev = SdkManager::instance().getDevice("MainCamera");
                if (outDev.handle != nullptr && outDev.state == SdkDeviceState::Open)
                    return true;
                const auto elapsedMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                if (elapsedMs > timeoutMs)
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        };

        auto callMain = [&](const char* name, const std::any& payload) -> SdkResult {
            SdkDeviceInfo dev;
            // Demo 重开相机可能需要几秒，这里给足等待窗口，避免对旧 handle 连续调用导致刷屏告警
            if (!waitMainCameraReady(8000, dev)) {
                SdkResult r;
                r.success = false;
                r.errorCode = SdkErrorCode::DeviceNotFound;
                r.message = "MainCamera not ready (reopening)";
                return r;
            }
            SdkCommand c;
            c.type = SdkCommandType::Custom;
            c.name = name;
            c.payload = payload;
            return SdkManager::instance().call(dev.driverName, dev.handle, c);
        };

        // 0) SetExposure(us)：以本次请求参数为准（避免 Burst/Live 已开启但曝光时间未同步）
        {
            const double expUs = static_cast<double>(std::max(1, expMsSnap)) * 1000.0;
            SdkResult setExpRes = callMain("SetExposure", expUs);
            if (!setExpRes.success) {
                // 不直接失败：后续可能仍能出帧（某些平台 SetExposure 偶发失败但实际生效），这里降级为告警
                Logger::Log("SDK_BurstCapture | SetExposure warning: " + setExpRes.message,
                            LogLevel::WARNING, DeviceType::CAMERA);
            }
        }

        // 1) 触发策略：
        // - 优先走 QHY Burst 子模式（ResetFrameCounter + SetBurstIDLE + SetBurstStartEnd + ReleaseBurstIDLE）；
        // - 若触发失败（或机型/驱动不稳定），则降级为纯 Live：BeginLive 后直接循环 GetLiveFrame（参考官方“连续拍摄”示例）。
        bool usePureLive = false;

        // 1.1) Burst 子模式：按官方 sample 的顺序触发
        // 相机将输出中间帧。为获取 N 帧，按 [start+1..end-1] 规划
        // 例如 N=4 -> start=1,end=6 -> 输出 2,3,4,5（4帧）
        const int start = 1;
        const int end = framesSnap + 2;
        auto burstTriggerT = std::chrono::steady_clock::time_point{};
        if (!usePureLive) {
            // best-effort：复位帧计数器，便于调试 + 避免部分机型累积导致错乱
            (void)callMain("ResetFrameCounter", std::any());

            // 关键：确保相机处于 IDLE 再设置 start/end（官方 sample 建议）
            SdkResult idleRes = callMain("SetBurstIDLE", std::any());
            if (!idleRes.success) {
                usePureLive = true;
                Logger::Log("SDK_BurstCapture | SetBurstIDLE failed, fallback to pure Live. msg=" + idleRes.message,
                            LogLevel::WARNING, DeviceType::CAMERA);
            }
        }
        if (!usePureLive) {
            SdkResult seRes = callMain("SetBurstStartEnd", std::make_pair(start, end));
            if (!seRes.success) {
                usePureLive = true;
                Logger::Log("SDK_BurstCapture | SetBurstStartEnd failed, fallback to pure Live. msg=" + seRes.message,
                            LogLevel::WARNING, DeviceType::CAMERA);
            }
        }
        if (!usePureLive) {
            SdkResult relRes = callMain("ReleaseBurstIDLE", std::any());
            if (!relRes.success) {
                usePureLive = true;
                Logger::Log("SDK_BurstCapture | ReleaseBurstIDLE failed, fallback to pure Live. msg=" + relRes.message,
                            LogLevel::WARNING, DeviceType::CAMERA);
            } else {
                burstTriggerT = std::chrono::steady_clock::now();
            }
        }

        if (usePureLive) {
            // 降级：尽量确保不会卡在 Burst 的 IDLE 状态，然后走连续拉帧
            (void)callMain("ReleaseBurstIDLE", std::any());
            (void)callMain("EnableBurstMode", false);
            // 参考官方连续模式：BeginLive + 循环 GetLiveFrame（StopLive 由模式切换时统一处理）
            (void)callMain("BeginLive", std::any());
        }

        // 3) 连续抓帧并做均值叠加（输出 1 张图）
        std::vector<uint32_t> accum;
        int okFrames = 0;
        int width = 0;
        int height = 0;

        // SDK 拉帧耗时（Burst）：本组输出只写一行，因此记录“参与叠加的有效帧”的平均拉帧耗时（微秒）
        long long usedAcquireSumUs = 0;
        int usedAcquireCount = 0;

        const auto t0 = std::chrono::steady_clock::now();
        auto lastFrameT = t0;
        // 参考官方 sample：部分机型第一轮/某些链路下可能较久才出第一帧，给足下限避免误判失败
        // timeout ≈ 15s + exposure * frames
        const auto maxWait = std::chrono::milliseconds(
            std::max(15000, expMsSnap * framesSnap + 15000));
        bool firstFrameLogged = false;

        while (okFrames < framesSnap)
        {
            if (sdkBurstCancelRequested.load()) {
                cancelled = true;
                break;
            }
            if (std::chrono::steady_clock::now() - t0 > maxWait) {
                break;
            }

            SdkCommand getCmd;
            getCmd.type = SdkCommandType::Custom;
            getCmd.name = "GetLiveFrame";
            getCmd.payload = std::any();

            const long long acquireStartNs =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            // GetLiveFrame 频率很高：若主相机正在重开，不要对旧 handle 忙等调用；等待注册表就绪后再取帧
            SdkDeviceInfo dev;
            if (!waitMainCameraReady(8000, dev)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            SdkResult frameRes = SdkManager::instance().call(dev.driverName, dev.handle, getCmd);
            const long long acquireEndNs =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            const long long acquireUs =
                (acquireEndNs >= acquireStartNs) ? ((acquireEndNs - acquireStartNs) / 1000LL) : -1;

            if (!frameRes.success) {
                // Burst/Live 下可能会偶发拿不到帧：短暂等待后继续
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            SdkFrameData frame;
            try {
                frame = std::any_cast<SdkFrameData>(frameRes.payload);
            } catch (const std::bad_any_cast&) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            const bool hasFrameData =
                (!frame.pixels.empty()) || (frame.rawBuffer != nullptr && frame.rawBytes > 0);
            if (frame.width <= 0 || frame.height <= 0 || !hasFrameData) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            if (okFrames == 0) {
                if (!firstFrameLogged && burstTriggerT != std::chrono::steady_clock::time_point{}) {
                    const auto dtFirstMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - burstTriggerT).count();
                    Logger::Log("SDK_BurstCapture | first frame after ReleaseBurstIDLE: " + std::to_string(dtFirstMs) + " ms",
                                LogLevel::INFO, DeviceType::CAMERA);
                    firstFrameLogged = true;
                }
                width = frame.width;
                height = frame.height;
                accum.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0u);
            }

            // 若分辨率在 Live 中变化，直接跳过（防止崩溃/错位）
            if (frame.width != width || frame.height != height || accum.empty() ||
                (static_cast<size_t>(width) * static_cast<size_t>(height)) != accum.size()) {
                continue;
            }

            // 累加像素：优先零拷贝 rawBuffer，其次使用 pixels（回退拷贝路径）
            if (!frame.pixels.empty()) {
                if (frame.pixels.size() != accum.size()) {
                    continue;
                }
                for (size_t p = 0; p < accum.size(); ++p) {
                    accum[p] += frame.pixels[p];
                }
            } else if (frame.rawBuffer != nullptr && frame.rawBytes > 0) {
                const size_t pixelCount = accum.size();
                if (frame.channels != 1 || (frame.bpp != 16 && frame.bpp != 8)) {
                    continue;
                }
                const size_t needBytes = pixelCount * (frame.bpp == 16 ? sizeof(uint16_t) : sizeof(uint8_t));
                if (frame.rawBytes < needBytes || frame.rawBuffer->size() < needBytes) {
                    continue;
                }
                if (frame.bpp == 16) {
                    const uint16_t* src = reinterpret_cast<const uint16_t*>(frame.rawBuffer->data());
                    for (size_t p = 0; p < pixelCount; ++p) {
                        accum[p] += src[p];
                    }
                } else { // 8-bit
                    const uint8_t* src = reinterpret_cast<const uint8_t*>(frame.rawBuffer->data());
                    for (size_t p = 0; p < pixelCount; ++p) {
                        accum[p] += static_cast<uint32_t>(src[p]) * 257u;
                    }
                }
            } else {
                continue;
            }
            okFrames++;
            if (acquireUs >= 0) {
                usedAcquireSumUs += acquireUs;
                usedAcquireCount++;
            }

            const auto nowT = std::chrono::steady_clock::now();
            const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(nowT - lastFrameT).count();
            const auto tMs = std::chrono::duration_cast<std::chrono::milliseconds>(nowT - t0).count();
            lastFrameT = nowT;
        }

        // 4) 回到 IDLE，等待下一次触发（不结束 Live/Burst）
        {
            (void)callMain("SetBurstIDLE", std::any());
        }

        const long long avgAcquireUs =
            (usedAcquireCount > 0) ? (usedAcquireSumUs / static_cast<long long>(usedAcquireCount)) : -1;

        if (cancelled) {
            QMetaObject::invokeMethod(this, [this]() {
                sdkBurstActive = false;
                sdkBurstCancelRequested = false;
                glMainCameraStatu = "IDLE";
                ShootStatus = "IDLE";
                emit wsThread->sendMessageToClient("CameraInExposuring:False");
            }, Qt::QueuedConnection);
            return;
        }

        if (okFrames < framesSnap || accum.empty() || width <= 0 || height <= 0) {
            failReason = QStringLiteral("Burst 获取图像失败（未获得足够有效帧）");
            QMetaObject::invokeMethod(this, [this, failReason]() {
                sdkBurstActive = false;
                sdkBurstCancelRequested = false;
                emit wsThread->sendMessageToClient("ExposureFailed:" + failReason);
                emit wsThread->sendMessageToClient("CameraInExposuring:False");
                glMainCameraStatu = "IDLE";
                ShootStatus = "IDLE";
            }, Qt::QueuedConnection);
            return;
        }

        outFrame->width = width;
        outFrame->height = height;
        outFrame->bpp = 16;
        outFrame->channels = 1;
        outFrame->pixels.resize(accum.size());
        for (size_t p = 0; p < accum.size(); ++p) {
            outFrame->pixels[p] = static_cast<uint16_t>(accum[p] / static_cast<uint32_t>(okFrames));
        }

        // 6) 回主线程：走现有 FITS/PNG/JPG 链路
        QMetaObject::invokeMethod(this, [this, outFrame, avgAcquireUs]() {
            sdkBurstActive = false;
            sdkBurstCancelRequested = false;

            // 若句柄已被清理，直接停止（避免后续访问空指针）
            if (sdkMainCameraHandle == nullptr) {
                glMainCameraStatu = "IDLE";
                ShootStatus = "IDLE";
                emit wsThread->sendMessageToClient("CameraInExposuring:False");
                return;
            }

            const std::string fitsPath = "/dev/shm/ccd_simulator.fits";
            SaveQhyFrameDataToFits(*outFrame, fitsPath);

            glMainCameraStatu = "Displaying";
            ShootStatus = "Completed";
            emit wsThread->sendMessageToClient("ExposureCompleted");
            emitCaptureTrace(QStringLiteral("backend_exposure_completed"), currentCaptureTraceStartedAtMs,
                             QStringLiteral("source=sdk_burst"));

            if (polarAlignment != nullptr && polarAlignment->isRunning())
            {
                notifyPolarAlignmentCaptureReady(PolarAlignmentCameraRole::MainCamera,
                                                 QString::fromStdString(fitsPath));
                return;
            }

            // AutoFocus：与单帧流程保持一致
            if (isAutoFocus && autoFocus != nullptr && autoFocus->isRunning())
            {
                saveFitsAsPNG(QString::fromStdString(fitsPath), true);
                autoFocus->setCaptureComplete(QString::fromStdString(fitsPath));
                Logger::Log("SDK_BurstCapture | ExposureCompleted -> autoFocus capture complete: " + fitsPath,
                            LogLevel::INFO, DeviceType::FOCUSER);
                return;
            }

            saveFitsAsPNG(QString::fromStdString(fitsPath), true);

            if (mainCameraAutoSave && isScheduleRunning == false) {
                Logger::Log("SDK_BurstCapture | Auto Save enabled, saving captured image...",
                            LogLevel::INFO, DeviceType::CAMERA);
                CaptureImageSave();
            }
        }, Qt::QueuedConnection);
    });
}

bool MainWindow::ensureSdkMainCameraSingleModeReady(QString *errorReason)
{
    const bool isMainCameraSDK =
        (systemdevicelist.system_devices.size() > 20 &&
         systemdevicelist.system_devices[20].isSDKConnect &&
         sdkMainCameraHandle != nullptr);
    if (!isMainCameraSDK)
        return true;

    auto isQhySdkDriverName = [](const QString& n) -> bool {
        const QString s = n.trimmed().toLower();
        return (s == "qhyccd" || s == "indi_qhy_ccd");
    };

    QString sdkDriverName =
        (systemdevicelist.system_devices.size() > 20) ? systemdevicelist.system_devices[20].SDKDriverName : "";
    QString effectiveSdkDriverName = sdkDriverName.trimmed();
    if (effectiveSdkDriverName.isEmpty())
        effectiveSdkDriverName = getSDKDriverName("MainCamera").trimmed();
    if (effectiveSdkDriverName.isEmpty() && systemdevicelist.system_devices.size() > 20)
        effectiveSdkDriverName = systemdevicelist.system_devices[20].DriverIndiName.trimmed();

    if (!isQhySdkDriverName(effectiveSdkDriverName))
        return true;

    const bool alreadySingleReady =
        (mainCameraCaptureMode == MainCameraCaptureMode::Single) &&
        !sdkMainLiveReady.load() &&
        !sdkMainBurstModeReady.load();
    if (alreadySingleReady)
        return true;

    SdkSerialExecutor *mainExec = sdkMainCameraExecutor();
    if (!mainExec || !mainExec->isRunning()) {
        if (errorReason)
            *errorReason = QStringLiteral("SDK worker not running");
        Logger::Log("ensureSdkMainCameraSingleModeReady | sdkMainCamExec not running",
                    LogLevel::ERROR, DeviceType::CAMERA);
        return false;
    }

    if (sdkBurstActive.load() || glMainCameraStatu == "Exposuring") {
        if (errorReason)
            *errorReason = QStringLiteral("camera busy while switching to single mode");
        Logger::Log("ensureSdkMainCameraSingleModeReady | camera busy, reject switch to single mode",
                    LogLevel::WARNING, DeviceType::CAMERA);
        return false;
    }

    Logger::Log("ensureSdkMainCameraSingleModeReady | switching SDK main camera to Single mode before exposure",
                LogLevel::INFO, DeviceType::CAMERA);
    const qint64 switchStartMs = QDateTime::currentMSecsSinceEpoch();
    const SdkDeviceHandle handleSnap = sdkMainCameraHandle;

    const bool ok = mainExec->postAndWait<bool>([handleSnap]() -> bool {
        if (handleSnap == nullptr)
            return false;

        auto callByHandle = [handleSnap](const char *name, const std::any &payload) -> SdkResult {
            SdkCommand cmd;
            cmd.type = SdkCommandType::Custom;
            cmd.name = name;
            cmd.payload = payload;
            return SdkManager::instance().callByHandle(handleSnap, cmd);
        };

        (void)callByHandle("ReleaseBurstIDLE", std::any());
        (void)callByHandle("StopLive", std::any());
        (void)callByHandle("EnableBurstMode", false);
        SdkResult streamRes = callByHandle("SetStreamMode", 0);
        if (!streamRes.success) {
            Logger::Log("ensureSdkMainCameraSingleModeReady | SetStreamMode(0) failed: " + streamRes.message,
                        LogLevel::ERROR, DeviceType::CAMERA);
            return false;
        }
        return true;
    });

    if (!ok) {
        if (errorReason)
            *errorReason = QStringLiteral("failed to switch SDK camera to single mode");
        Logger::Log("ensureSdkMainCameraSingleModeReady | failed to switch SDK main camera to Single mode",
                    LogLevel::ERROR, DeviceType::CAMERA);
        return false;
    }

    mainCameraCaptureMode = MainCameraCaptureMode::Single;
    sdkMainLiveReady = false;
    sdkMainBurstModeReady = false;
    sdkMainAppliedMode = MainCameraCaptureMode::Single;
    sdkMainAppliedModeValid = true;
    emitCaptureTrace(QStringLiteral("backend_single_mode_ready"), switchStartMs,
                     QStringLiteral("success=true"));
    Logger::Log("ensureSdkMainCameraSingleModeReady | SDK main camera switched to Single mode",
                LogLevel::INFO, DeviceType::CAMERA);
    return true;
}

void MainWindow::startMainCameraCapture(int exposureMs)
{
    Logger::Log("startMainCameraCapture start ...", LogLevel::INFO, DeviceType::CAMERA);
    emitCaptureTrace(QStringLiteral("backend_main_camera_capture_enter"), currentCaptureTraceStartedAtMs,
                     QString("exposureMs=%1").arg(exposureMs));

    glIsFocusingLooping = false;
    isSavePngSuccess = false;
    double expTime_sec = (double)exposureMs / 1000;
    Logger::Log("startMainCameraCapture | convert exposureMs to seconds:" + std::to_string(expTime_sec), LogLevel::INFO, DeviceType::CAMERA);

    // 判断主相机是 SDK 模式还是 INDI 模式
    bool isMainCameraSDK = (systemdevicelist.system_devices.size() > 20 &&
                            systemdevicelist.system_devices[20].isSDKConnect &&
                            sdkMainCameraHandle != nullptr);

    if (isMainCameraSDK)
    {
        // === SDK 模式：完整的全分辨率曝光流程 ===
        // 将 SDK 错误信息转换为“可读原因”（避免直接把 0xFFFFFFFF/4294967295 暴露给前端）
        auto makeUserFriendlySdkReason = [](const QString& step, const QString& sdkMsg) -> QString {
            const QString raw = sdkMsg.trimmed();
            QString msg = raw;

            // 去掉尾部常见的“error code: xxx/0xXXXX”
            const int ecIdx = msg.lastIndexOf("error code", -1, Qt::CaseInsensitive);
            if (ecIdx >= 0)
            {
                msg = msg.left(ecIdx).trimmed();
                while (msg.endsWith(',')) msg.chop(1);
                msg = msg.trimmed();
            }

            const bool isGenericFail =
                raw.contains("4294967295") || raw.contains("0xFFFFFFFF", Qt::CaseInsensitive);

            QString reason = QStringLiteral("曝光失败：") + step;
            if (!msg.isEmpty())
                reason += QStringLiteral("（") + msg + QStringLiteral("）");
            if (isGenericFail)
                reason += QStringLiteral("。可能原因：相机未连接/驱动未初始化/USB通信异常。请尝试重新连接相机或重启驱动。");
            return reason;
        };

        QString singleModeReason;
        if (!ensureSdkMainCameraSingleModeReady(&singleModeReason)) {
            emit wsThread->sendMessageToClient("ExposureFailed:SDK单帧模式未就绪（" + singleModeReason + "）");
            emit wsThread->sendMessageToClient("CameraInExposuring:False");
            ShootStatus = "IDLE";
            glMainCameraStatu = "IDLE";
            return;
        }

        glMainCameraStatu = "Exposuring";
        Logger::Log("startMainCameraCapture | SDK Mode | Main Camera Status: " + glMainCameraStatu.toStdString(),
                   LogLevel::INFO, DeviceType::CAMERA);
        const qint64 sdkCaptureStageStartMs = QDateTime::currentMSecsSinceEpoch();

        const bool polarAlignmentCapture =
            (polarAlignment != nullptr && polarAlignment->isRunning());
        int requestedSdkBin = 1;
        if (polarAlignmentCapture) {
            requestedSdkBin = 2;
            if (const char* envBin = std::getenv("QUARCS_POLAR_SDK_BIN")) {
                bool ok = false;
                const int parsed = QString::fromLocal8Bit(envBin).trimmed().toInt(&ok);
                if (ok) requestedSdkBin = std::clamp(parsed, 1, 4);
            }
        }

        {
            const qint64 setBinStartMs = QDateTime::currentMSecsSinceEpoch();
            SdkCommand binCmd;
            binCmd.type = SdkCommandType::Custom;
            binCmd.name = "SetBinMode";
            binCmd.payload = std::make_pair(requestedSdkBin, requestedSdkBin);
            SdkResult binRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, binCmd);
            emitCaptureTrace(QStringLiteral("backend_set_bin_done"), setBinStartMs,
                             QString("success=%1,bin=%2,polar=%3")
                                 .arg(binRes.success ? QStringLiteral("true") : QStringLiteral("false"))
                                 .arg(requestedSdkBin)
                                 .arg(polarAlignmentCapture ? QStringLiteral("true") : QStringLiteral("false")));
            if (!binRes.success) {
                Logger::Log("startMainCameraCapture | SDK SetBinMode(" +
                                std::to_string(requestedSdkBin) + "," +
                                std::to_string(requestedSdkBin) + ") failed: " + binRes.message,
                            LogLevel::WARNING, DeviceType::CAMERA);
            } else {
                Logger::Log("startMainCameraCapture | SDK SetBinMode(" +
                                std::to_string(requestedSdkBin) + "," +
                                std::to_string(requestedSdkBin) + ") success" +
                                (polarAlignmentCapture ? " for polar alignment" : " for normal capture"),
                            LogLevel::INFO, DeviceType::CAMERA);
            }
        }

        // 0. 确保全分辨率 ROI/分辨率已设置（否则部分机型会出现 ret=0 但 roi=0x0，导致上层一直轮询）
        // 优先从 SDK 获取有效区域，避免依赖 UI 侧 glMainCCDSizeX/Y 的时序。
        SdkAreaInfo fullRoi;
        {
            const qint64 effectiveAreaStartMs = QDateTime::currentMSecsSinceEpoch();
            SdkCommand effCmd;
            effCmd.type = SdkCommandType::Custom;
            effCmd.name = "GetEffectiveArea";
            effCmd.payload = std::any();
            // 直接通过设备句柄调用，无需指定驱动名称
            SdkResult effRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, effCmd);
            if (effRes.success) {
                fullRoi = std::any_cast<SdkAreaInfo>(effRes.payload);
            } else {
                // 回退：使用已知的主相机尺寸
                fullRoi.startX = 0;
                fullRoi.startY = 0;
                fullRoi.sizeX  = (glMainCCDSizeX > 0) ? static_cast<unsigned int>(glMainCCDSizeX) : 0;
                fullRoi.sizeY  = (glMainCCDSizeY > 0) ? static_cast<unsigned int>(glMainCCDSizeY) : 0;
            }
            emitCaptureTrace(QStringLiteral("backend_get_effective_area_done"), effectiveAreaStartMs,
                             QString("success=%1,startX=%2,startY=%3,sizeX=%4,sizeY=%5")
                                 .arg(effRes.success ? QStringLiteral("true") : QStringLiteral("false"))
                                 .arg(fullRoi.startX)
                                 .arg(fullRoi.startY)
                                 .arg(fullRoi.sizeX)
                                 .arg(fullRoi.sizeY));
        }
        if (fullRoi.sizeX > 0 && fullRoi.sizeY > 0) {
            const qint64 setResolutionStartMs = QDateTime::currentMSecsSinceEpoch();
            SdkCommand setResCmd;
            setResCmd.type = SdkCommandType::Custom;
            setResCmd.name = "SetResolution";
            setResCmd.payload = fullRoi;
            // 直接通过设备句柄调用，无需指定驱动名称
            SdkResult setResRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, setResCmd);
            if (!setResRes.success) {
                Logger::Log("startMainCameraCapture | SDK SetResolution(full) failed: " + setResRes.message,
                            LogLevel::ERROR, DeviceType::CAMERA);
                const QString reason =
                    makeUserFriendlySdkReason(QStringLiteral("设置分辨率失败"), QString::fromStdString(setResRes.message));
                emit wsThread->sendMessageToClient("ExposureFailed:" + reason);
                emit wsThread->sendMessageToClient("CameraInExposuring:False");
                ShootStatus = "IDLE";
                glMainCameraStatu = "IDLE";
                return;
            } else {
                emitCaptureTrace(QStringLiteral("backend_set_resolution_done"), setResolutionStartMs,
                                 QString("success=true,startX=%1,startY=%2,sizeX=%3,sizeY=%4")
                                     .arg(fullRoi.startX)
                                     .arg(fullRoi.startY)
                                     .arg(fullRoi.sizeX)
                                     .arg(fullRoi.sizeY));
                Logger::Log("startMainCameraCapture | SDK SetResolution(full) success: " +
                            std::to_string(fullRoi.startX) + "," + std::to_string(fullRoi.startY) + " " +
                            std::to_string(fullRoi.sizeX) + "x" + std::to_string(fullRoi.sizeY),
                            LogLevel::DEBUG, DeviceType::CAMERA);
            }
        } else {
            emitCaptureTrace(QStringLiteral("backend_set_resolution_skipped"), sdkCaptureStageStartMs,
                             QString("reason=invalid_full_roi,sizeX=%1,sizeY=%2")
                                 .arg(fullRoi.sizeX)
                                 .arg(fullRoi.sizeY));
            Logger::Log("startMainCameraCapture | SDK SetResolution(full) skipped: invalid fullRoi size (" +
                        std::to_string(fullRoi.sizeX) + "x" + std::to_string(fullRoi.sizeY) + ")",
                        LogLevel::WARNING, DeviceType::CAMERA);
        }

        // 1. 设置曝光时间（微秒）
        const qint64 setExposureStartMs = QDateTime::currentMSecsSinceEpoch();
        SdkCommand setExpCmd;
        setExpCmd.type = SdkCommandType::Custom;
        setExpCmd.name = "SetExposure";
        setExpCmd.payload = expTime_sec * 1000000.0; // 转换为微秒
        // 直接通过设备句柄调用，无需指定驱动名称
        SdkResult setExpRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, setExpCmd);
        if (!setExpRes.success) {
            Logger::Log("startMainCameraCapture | SDK SetExposure failed: " + setExpRes.message,
                       LogLevel::ERROR, DeviceType::CAMERA);
            const QString reason =
                makeUserFriendlySdkReason(QStringLiteral("设置曝光时间失败"), QString::fromStdString(setExpRes.message));
            emit wsThread->sendMessageToClient("ExposureFailed:" + reason);
            emit wsThread->sendMessageToClient("CameraInExposuring:False");
            ShootStatus = "IDLE";
            glMainCameraStatu = "IDLE";
            return;
        }
        emitCaptureTrace(QStringLiteral("backend_set_exposure_done"), setExposureStartMs,
                         QString("success=true,exposureUs=%1")
                             .arg(QString::number(expTime_sec * 1000000.0, 'f', 0)));
        
        // 2. 启动单帧曝光
        const qint64 startExposureCmdStartMs = QDateTime::currentMSecsSinceEpoch();
        SdkCommand startExpCmd;
        startExpCmd.type = SdkCommandType::Custom;
        startExpCmd.name = "StartSingleExposure";
        startExpCmd.payload = std::any();
        Logger::Log("CaptureTrace | stage=backend_start_single_exposure_callbyhandle_enter"
                        " | handle=" + std::to_string(reinterpret_cast<uintptr_t>(sdkMainCameraHandle)) +
                        " | thread=" + std::to_string(
                            static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()))),
                    LogLevel::INFO, DeviceType::CAMERA);
        // 直接通过设备句柄调用，无需指定驱动名称
        SdkResult startExpRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, startExpCmd);
        Logger::Log("CaptureTrace | stage=backend_start_single_exposure_callbyhandle_return"
                        " | handle=" + std::to_string(reinterpret_cast<uintptr_t>(sdkMainCameraHandle)) +
                        " | ok=" + std::string(startExpRes.success ? "true" : "false") +
                        " | msg=" + startExpRes.message,
                    startExpRes.success ? LogLevel::INFO : LogLevel::ERROR,
                    DeviceType::CAMERA);
        if (!startExpRes.success) {
            Logger::Log("startMainCameraCapture | SDK StartSingleExposure failed: " + startExpRes.message,
                       LogLevel::ERROR, DeviceType::CAMERA);
            const QString reason =
                makeUserFriendlySdkReason(QStringLiteral("启动曝光失败"), QString::fromStdString(startExpRes.message));
            emit wsThread->sendMessageToClient("ExposureFailed:" + reason);
            emit wsThread->sendMessageToClient("CameraInExposuring:False");
            ShootStatus = "IDLE";
            glMainCameraStatu = "IDLE";
            return;
        }
        emitCaptureTrace(QStringLiteral("backend_start_single_exposure_done"), startExposureCmdStartMs,
                         QString("success=true,transport=sdk"));
        emitCaptureTrace(QStringLiteral("backend_exposure_start"), currentCaptureTraceStartedAtMs,
                         QString("transport=sdk,exposureMs=%1").arg(static_cast<int>(expTime_sec * 1000)));
        Logger::Log("startMainCameraCapture | SDK StartSingleExposure success, expTime_sec:" + std::to_string(expTime_sec),
                   LogLevel::INFO, DeviceType::CAMERA);
        
        // 3. 使用定时器轮询获取图像（避免阻塞）
        int expTime_ms = static_cast<int>(expTime_sec * 1000);
        sdkExposureStartTime = QDateTime::currentMSecsSinceEpoch();
        sdkExposureExpectedDuration = expTime_ms;
        sdkExposureIsROI = false; // 全分辨率模式
        
        // 第一次等待时间 = 曝光时间（对于短曝光如 1ms，等待 1ms；长曝光如 1s，等待 1s）
        sdkExposureTimer->start(expTime_ms);
        Logger::Log("startMainCameraCapture | SDK exposure timer started, will check after " + std::to_string(expTime_ms) + "ms",
                   LogLevel::INFO, DeviceType::CAMERA);
    }
    else if (dpMainCamera)
    {
        // === INDI 模式 ===
        glMainCameraStatu = "Exposuring";
        Logger::Log("startMainCameraCapture | INDI Mode | check Main Camera Status(glMainCameraStatu):" + glMainCameraStatu.toStdString(), LogLevel::INFO, DeviceType::CAMERA);

        int value, min, max;
        uint32_t ret = indi_Client->getCCDGain(dpMainCamera, value, min, max);
        if (ret != QHYCCD_SUCCESS)
        {
            Logger::Log("startMainCameraCapture | indi getCCDGain | failed", LogLevel::WARNING, DeviceType::CAMERA);
        }
        Logger::Log("startMainCameraCapture | indi getCCDGain | value:" + std::to_string(value) + ", min:" + std::to_string(min) + ", max:" + std::to_string(max), LogLevel::INFO, DeviceType::CAMERA);
        int BINX, BINY, BINXMAX, BINYMAX;
        ret = indi_Client->getCCDBinning(dpMainCamera, BINX, BINY, BINXMAX, BINYMAX);
        if (ret != QHYCCD_SUCCESS)
        {
            Logger::Log("startMainCameraCapture | indi getCCDBinning | failed", LogLevel::WARNING, DeviceType::CAMERA);
        }
        Logger::Log("startMainCameraCapture | indi getCCDBinning | BINX:" + std::to_string(BINX) + ", BINY:" + std::to_string(BINY) + ", BINXMAX:" + std::to_string(BINXMAX) + ", BINYMAX:" + std::to_string(BINYMAX), LogLevel::INFO, DeviceType::CAMERA);
        ret = indi_Client->getCCDOffset(dpMainCamera, value, min, max);
        if (ret != QHYCCD_SUCCESS)
        {
            Logger::Log("startMainCameraCapture | indi getCCDOffset | failed", LogLevel::WARNING, DeviceType::CAMERA);
        }
        Logger::Log("startMainCameraCapture | indi getCCDOffset | value:" + std::to_string(value) + ", min:" + std::to_string(min) + ", max:" + std::to_string(max), LogLevel::INFO, DeviceType::CAMERA);
        ret = indi_Client->resetCCDFrameInfo(dpMainCamera);
        if (ret != QHYCCD_SUCCESS)
        {
            Logger::Log("startMainCameraCapture | indi resetCCDFrameInfo | failed", LogLevel::WARNING, DeviceType::CAMERA);
        }
        Logger::Log("startMainCameraCapture | indi resetCCDFrameInfo", LogLevel::INFO, DeviceType::CAMERA);
        emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));
        Logger::Log("startMainCameraCapture | sendMessageToClient | MainCameraSize:" + QString::number(glMainCCDSizeX).toStdString() + ":" + QString::number(glMainCCDSizeY).toStdString(), LogLevel::INFO, DeviceType::CAMERA);
        ret = indi_Client->takeExposure(dpMainCamera, expTime_sec);
        if (ret != QHYCCD_SUCCESS)
        {
            Logger::Log("startMainCameraCapture | indi takeExposure | failed", LogLevel::WARNING, DeviceType::CAMERA);
        }
        emitCaptureTrace(QStringLiteral("backend_exposure_start"), currentCaptureTraceStartedAtMs,
                         QString("transport=indi,exposureMs=%1").arg(static_cast<int>(expTime_sec * 1000)));
        Logger::Log("startMainCameraCapture | indi start takeExposure, expTime_sec:" + std::to_string(expTime_sec), LogLevel::INFO, DeviceType::CAMERA);
    }
    else
    {
        Logger::Log("startMainCameraCapture | Main Camera not available (both SDK and INDI are NULL)", LogLevel::WARNING, DeviceType::CAMERA);
        ShootStatus = "IDLE";
    }
    Logger::Log("startMainCameraCapture finished.", LogLevel::INFO, DeviceType::CAMERA);
}

void MainWindow::abortMainCameraCapture()
{
    Logger::Log("abortMainCameraCapture start ...", LogLevel::INFO, DeviceType::CAMERA);
    glMainCameraStatu = "IDLE";
    Logger::Log("abortMainCameraCapture | glMainCameraStatu:" + glMainCameraStatu.toStdString(), LogLevel::INFO, DeviceType::CAMERA);

    // 判断主相机是 SDK 模式还是 INDI 模式
    bool isMainCameraSDK = (systemdevicelist.system_devices.size() > 20 &&
                            systemdevicelist.system_devices[20].isSDKConnect &&
                            sdkMainCameraHandle != nullptr);

    if (isMainCameraSDK)
    {
        // === SDK 模式 ===
        
        // 🔧 修复：立即停止曝光轮询定时器，防止重复调用GetSingleFrame
        if (sdkExposureTimer && sdkExposureTimer->isActive()) {
            sdkExposureTimer->stop();
            Logger::Log("abortMainCameraCapture | Stopped sdkExposureTimer to prevent redundant GetSingleFrame calls",
                       LogLevel::DEBUG, DeviceType::CAMERA);
        }
        
        // 重置防重入标志，允许新的曝光操作
        sdkFrameTaskInFlight = false;
        
        // 重置曝光状态标志
        sdkExposureIsROI = false;

        // Burst（Live）取消：设置取消标志，并尽力停止 Live（不阻塞主线程）
        if (sdkBurstActive.load()) {
            sdkBurstCancelRequested = true;
            Logger::Log("abortMainCameraCapture | Burst active, request cancel",
                        LogLevel::INFO, DeviceType::CAMERA);

            // Burst 作为连接模式子模式：Abort 不应退出 Burst/Live，只需回到 IDLE 等待下一次触发。
            SdkSerialExecutor *mainExec = sdkMainCameraExecutor();
            if (mainExec && mainExec->isRunning() && sdkMainCameraHandle != nullptr) {
                const SdkDeviceHandle handleSnap = sdkMainCameraHandle;
                mainExec->post([handleSnap]() {
                    // 回到 IDLE（best-effort）
                    SdkCommand idle;
                    idle.type = SdkCommandType::Custom;
                    idle.name = "SetBurstIDLE";
                    idle.payload = std::any();
                    (void)SdkManager::instance().callByHandle(handleSnap, idle);
                });
            }
        }
        
        SdkCommand abortCmd;
        abortCmd.type = SdkCommandType::Custom;
        // SDK 驱动中实现的取消命令为 CancelExposure（CancelQHYCCDExposingAndReadout）
        abortCmd.name = "CancelExposure";
        abortCmd.payload = std::any();

        // 直接通过设备句柄调用，无需指定驱动名称
        SdkResult abortRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, abortCmd);
        if (!abortRes.success) {
            Logger::Log("abortMainCameraCapture | SDK CancelExposure failed: " + abortRes.message,
                       LogLevel::ERROR, DeviceType::CAMERA);
        } else {
            Logger::Log("abortMainCameraCapture | SDK CancelExposure success", LogLevel::INFO, DeviceType::CAMERA);
        }
        ShootStatus = "IDLE";
    }
    else if (dpMainCamera)
    {
        // === INDI 模式 ===
        indi_Client->setCCDAbortExposure(dpMainCamera);
        ShootStatus = "IDLE";
        Logger::Log("abortMainCameraCapture | INDI ShootStatus:" + ShootStatus.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    }
    Logger::Log("abortMainCameraCapture finished.", LogLevel::INFO, DeviceType::CAMERA);
}

// SDK 模式下保存 SdkFrameData 为 FITS 文件的辅助函数
void MainWindow::SaveQhyFrameDataToFits(const SdkFrameData& frame, const std::string& filepath)
{
    // 防御：避免写出 0x0 或空数据导致后续读取失败
    const bool hasVecPixels = !frame.pixels.empty();
    const bool hasRawPixels = (frame.rawBuffer != nullptr && frame.rawBytes > 0);
    if (frame.width <= 0 || frame.height <= 0 || (!hasVecPixels && !hasRawPixels))
    {
        Logger::Log("SaveQhyFrameDataToFits | invalid frame, skip write. size=" +
                        std::to_string(frame.width) + "x" + std::to_string(frame.height) +
                        " pixels=" + std::to_string(frame.pixels.size()) +
                        " raw=" + std::string(hasRawPixels ? "true" : "false") +
                        " rawBytes=" + std::to_string(frame.rawBytes) +
                        " bpp=" + std::to_string(frame.bpp) +
                        " ch=" + std::to_string(frame.channels),
                    LogLevel::ERROR, DeviceType::CAMERA);
        return;
    }

    fitsfile *fptr;
    int status = 0;
    long naxes[2] = {static_cast<long>(frame.width), static_cast<long>(frame.height)};
    const long fpixel[2] = {1, 1};
    
    // 删除已存在的文件
    remove(filepath.c_str());
    
    // 创建 FITS 文件
    fits_create_file(&fptr, filepath.c_str(), &status);
    if (status) {
        Logger::Log("SaveQhyFrameDataToFits | fits_create_file failed, status=" + std::to_string(status), 
                   LogLevel::ERROR, DeviceType::CAMERA);
        return;
    }
    
    // 选择写入源与像素类型（支持 8/16 位单通道）
    int bitpix = USHORT_IMG;
    int datatype = TUSHORT;
    long nelements = static_cast<long>(static_cast<long long>(frame.width) * static_cast<long long>(frame.height));
    const void* srcPtr = nullptr;

    if (hasVecPixels) {
        // 旧路径：pixels（16-bit）
        bitpix = USHORT_IMG;
        datatype = TUSHORT;
        nelements = static_cast<long>(frame.pixels.size());
        srcPtr = frame.pixels.data();
    } else {
        // 零拷贝路径：rawBuffer
        if (frame.channels != 1 || (frame.bpp != 16 && frame.bpp != 8)) {
            Logger::Log("SaveQhyFrameDataToFits | unsupported rawBuffer format: bpp=" +
                            std::to_string(frame.bpp) + " channels=" + std::to_string(frame.channels),
                        LogLevel::ERROR, DeviceType::CAMERA);
            fits_close_file(fptr, &status);
            return;
        }
        const size_t pixelCount = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
        const size_t needBytes = pixelCount * (frame.bpp == 16 ? sizeof(uint16_t) : sizeof(uint8_t));
        if (frame.rawBuffer->size() < needBytes || frame.rawBytes < needBytes) {
            Logger::Log("SaveQhyFrameDataToFits | rawBuffer too small: needBytes=" +
                            std::to_string(needBytes) + " rawBytes=" + std::to_string(frame.rawBytes) +
                            " bufSize=" + std::to_string(frame.rawBuffer->size()),
                        LogLevel::ERROR, DeviceType::CAMERA);
            fits_close_file(fptr, &status);
            return;
        }
        nelements = static_cast<long>(pixelCount);
        if (frame.bpp == 8) {
            bitpix = BYTE_IMG;
            datatype = TBYTE;
        } else {
            bitpix = USHORT_IMG;
            datatype = TUSHORT;
        }
        srcPtr = frame.rawBuffer->data();
    }

    // 创建图像
    fits_create_img(fptr, bitpix, 2, naxes, &status);
    if (status) {
        Logger::Log("SaveQhyFrameDataToFits | fits_create_img failed, status=" + std::to_string(status), 
                   LogLevel::ERROR, DeviceType::CAMERA);
        fits_close_file(fptr, &status);
        return;
    }
    
    // 写入图像数据
    fits_write_pix(fptr, datatype, const_cast<long*>(fpixel), nelements,
                   const_cast<void*>(srcPtr), &status);
    if (status) {
        Logger::Log("SaveQhyFrameDataToFits | fits_write_pix failed, status=" + std::to_string(status), 
                   LogLevel::ERROR, DeviceType::CAMERA);
    }
    
    // 关闭文件
    fits_close_file(fptr, &status);
    if (status) {
        Logger::Log("SaveQhyFrameDataToFits | fits_close_file failed, status=" + std::to_string(status), 
                   LogLevel::ERROR, DeviceType::CAMERA);
    } else {
        Logger::Log("SaveQhyFrameDataToFits | FITS saved successfully: " + filepath, 
                   LogLevel::INFO, DeviceType::CAMERA);
    }
}

// SDK 曝光定时器回调：轮询获取图像
void MainWindow::onSdkExposureTimerTimeout()
{
    // 停止定时器（获取成功或失败后会重新启动或停止）
    sdkExposureTimer->stop();

    // 若 SDK 执行线程不可用，直接失败（避免在主线程做阻塞式 SDK 调用）
    SdkSerialExecutor *mainExec = sdkMainCameraExecutor();
    if (!mainExec || !mainExec->isRunning())
    {
        Logger::Log("onSdkExposureTimerTimeout | sdkMainCamExec not running, stop polling",
                    LogLevel::ERROR, DeviceType::CAMERA);
        glMainCameraStatu = "IDLE";
        ShootStatus = "IDLE";
        return;
    }

    // 防御：在“断开驱动/断开全部”过程中句柄可能已被关闭并置空
    if (sdkMainCameraHandle == nullptr)
    {
        Logger::Log("onSdkExposureTimerTimeout | sdkMainCameraHandle is nullptr (maybe disconnected). Stop polling.",
                    LogLevel::WARNING, DeviceType::CAMERA);
        glMainCameraStatu = "IDLE";
        ShootStatus = "IDLE";
        if (sdkExposureIsROI)
        {
            glIsFocusingLooping = false;
            isFocusLoopShooting = false;
        }
        sdkExposureIsROI = false;
        return;
    }

    // 防重入：如果上一轮 GetSingleFrame 还在 SDK 线程执行，就不要在主线程重复触发
    if (sdkFrameTaskInFlight.exchange(true))
    {
        sdkExposureTimer->start(10);
        return;
    }

    // 快照：避免并发修改导致读到不一致状态
    const SdkDeviceHandle handleSnap = sdkMainCameraHandle;
    const qint64 startSnap = sdkExposureStartTime;
    const int expectedSnap = sdkExposureExpectedDuration;
    const bool isRoiSnap = sdkExposureIsROI;

    mainExec->post([this, handleSnap, startSnap, expectedSnap, isRoiSnap]() {
        const qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
        const qint64 elapsed = currentTime - startSnap;

        // 防御：避免由于 SDK 异常/参数非法导致无限轮询
        const qint64 expected = std::max<qint64>(1, expectedSnap);
        const qint64 maxWaitMs = std::max<qint64>(expected + 5000, expected * 3); // 短曝光最多等 5s，长曝光最多等 3x

        if (elapsed > maxWaitMs)
        {
            // 尝试取消 SDK 曝光/读出（在 SDK 线程执行）
            SdkCommand cancelCmd;
            cancelCmd.type = SdkCommandType::Custom;
            cancelCmd.name = "CancelExposure";
            cancelCmd.payload = std::any();
                // 直接通过设备句柄调用，无需指定驱动名称
                SdkResult cancelRes = SdkManager::instance().callByHandle(handleSnap, cancelCmd);

            QMetaObject::invokeMethod(
                this,
                [this, cancelRes, isRoiSnap, elapsed, expected, maxWaitMs]() {
                    sdkFrameTaskInFlight = false;

                    Logger::Log("onSdkExposureTimerTimeout | TIMEOUT waiting frame (elapsed=" +
                                    std::to_string(elapsed) + "ms, expected=" + std::to_string(expected) +
                                    "ms, maxWait=" + std::to_string(maxWaitMs) + "ms). Cancelling exposure.",
                                LogLevel::ERROR, DeviceType::CAMERA);

                    if (!cancelRes.success) {
                        Logger::Log("onSdkExposureTimerTimeout | CancelExposure failed: " + cancelRes.message,
                                    LogLevel::WARNING, DeviceType::CAMERA);
                    }

                    glMainCameraStatu = "IDLE";
                    ShootStatus = "IDLE";

                    if (isRoiSnap) {
                        glIsFocusingLooping = false;
                        isFocusLoopShooting = false;
                        emit wsThread->sendMessageToClient("startFocusLoopFailed:SDK GetSingleFrame timeout");
                    } else {
                        emit wsThread->sendMessageToClient("ExposureFailed:SDK GetSingleFrame timeout");
                    }
                },
                Qt::QueuedConnection);
            return;
        }

        // GetSingleFrame（在 SDK 线程执行）
        SdkCommand getFrameCmd;
        getFrameCmd.type = SdkCommandType::Custom;
        getFrameCmd.name = "GetSingleFrame";
        getFrameCmd.payload = std::any();
        Logger::Log("onSdkExposureTimerTimeout | dispatch GetSingleFrame to SDK thread: handle=" +
                        std::to_string(reinterpret_cast<uintptr_t>(handleSnap)) +
                        " elapsed=" + std::to_string(elapsed) +
                        "ms expected=" + std::to_string(expected) +
                        "ms isROI=" + std::string(isRoiSnap ? "true" : "false"),
                    LogLevel::INFO, DeviceType::CAMERA);
        // 直接通过设备句柄调用，无需指定驱动名称
        const long long acquireStartNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        SdkResult frameRes = SdkManager::instance().callByHandle(handleSnap, getFrameCmd);
        const long long acquireEndNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        const long long acquireUs =
            (acquireEndNs >= acquireStartNs) ? ((acquireEndNs - acquireStartNs) / 1000LL) : -1;

        QMetaObject::invokeMethod(
            this,
            [this, frameRes, isRoiSnap, expected, acquireUs]() mutable {
                sdkFrameTaskInFlight = false;

                // 若句柄已被清理，直接停止（避免继续轮询）
                if (sdkMainCameraHandle == nullptr)
                    return;

                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                const qint64 elapsed2 = now - sdkExposureStartTime;

                Logger::Log("onSdkExposureTimerTimeout | Elapsed: " + std::to_string(elapsed2) + "ms, Expected: " +
                                std::to_string(sdkExposureExpectedDuration) + "ms",
                            LogLevel::DEBUG, DeviceType::CAMERA);

                if (frameRes.success)
                {
                    Logger::Log("onSdkExposureTimerTimeout | GetSingleFrame success", LogLevel::INFO, DeviceType::CAMERA);

                    SdkFrameData frame;
                    try {
                        frame = std::any_cast<SdkFrameData>(frameRes.payload);
                    } catch (const std::bad_any_cast&) {
                        Logger::Log("onSdkExposureTimerTimeout | payload any_cast failed",
                                    LogLevel::WARNING, DeviceType::CAMERA);
                        glMainCameraStatu = "IDLE";
                        return;
                    }
                    Logger::Log("onSdkExposureTimerTimeout | Frame size: " +
                                    std::to_string(frame.width) + "x" + std::to_string(frame.height),
                                LogLevel::INFO, DeviceType::CAMERA);

                    // 检查：如果是 ROI 模式但 ROI 循环已停止，丢弃此帧
                    if (isRoiSnap && !isFocusLoopShooting) {
                        Logger::Log("onSdkExposureTimerTimeout | ROI loop stopped, discard frame",
                                    LogLevel::WARNING, DeviceType::CAMERA);
                        glMainCameraStatu = "IDLE";
                        return;
                    }

                    // 检查：如果当前 ROI 标志与快照不一致，说明模式已切换，丢弃此帧避免错误处理
                    if (isRoiSnap != sdkExposureIsROI) {
                        Logger::Log("onSdkExposureTimerTimeout | ROI mode changed during capture (snapshot=" +
                                    std::string(isRoiSnap ? "true" : "false") + ", current=" +
                                    std::string(sdkExposureIsROI ? "true" : "false") + "), discard frame",
                                    LogLevel::WARNING, DeviceType::CAMERA);
                        glMainCameraStatu = "IDLE";
                        return;
                    }

                    auto framePtr = std::make_shared<SdkFrameData>(std::move(frame));
                    const std::string fitsPath = "/dev/shm/ccd_simulator.fits";

                    glMainCameraStatu = "Displaying";

                    if (isRoiSnap)
                    {
                        SaveQhyFrameDataToFits(*framePtr, fitsPath);
                        saveFitsAsJPG(QString::fromStdString(fitsPath), true);
                        Logger::Log("onSdkExposureTimerTimeout | ROI mode, saveFitsAsJPG complete",
                                    LogLevel::DEBUG, DeviceType::CAMERA);
                    }
                    else
                    {
                        SaveQhyFrameDataToFits(*framePtr, fitsPath);
                        lastMainCaptureFitsPath = QString::fromStdString(fitsPath);

                        ShootStatus = "Completed";
                        emit wsThread->sendMessageToClient("ExposureCompleted");
                        emitCaptureTrace(QStringLiteral("backend_exposure_completed"), currentCaptureTraceStartedAtMs,
                                         QStringLiteral("source=sdk_timer"));
                        Logger::Log("onSdkExposureTimerTimeout | Full resolution mode, ExposureCompleted",
                                    LogLevel::INFO, DeviceType::CAMERA);

                        if (polarAlignment != nullptr && polarAlignment->isRunning())
                        {
                            notifyPolarAlignmentCaptureReady(PolarAlignmentCameraRole::MainCamera,
                                                             QString::fromStdString(fitsPath));
                            return;
                        }

                        // AutoFocus：SDK 主相机模式下，曝光完成需要通知 AutoFocus（否则会一直等待拍摄结束）
                        if (isAutoFocus && autoFocus != nullptr && autoFocus->isRunning())
                        {
                            // 与 INDI 回调一致：处理完成并落盘后，再把本次 FITS 路径交给 AutoFocus
                            saveFitsAsPNG_FromSdkFrame(framePtr, true, [this, fitsPath](bool ok) {
                                if (!ok) return;
                                if (autoFocus != nullptr && autoFocus->isRunning()) {
                                    autoFocus->setCaptureComplete(QString::fromStdString(fitsPath));
                                    Logger::Log("onSdkExposureTimerTimeout | ExposureCompleted -> autoFocus capture complete: " + fitsPath,
                                                LogLevel::INFO, DeviceType::FOCUSER);
                                }
                            });
                            return;
                        }

                        if (mainCameraAutoSave && isScheduleRunning == false) {
                            saveFitsAsPNG_FromSdkFrame(framePtr, true, [this](bool ok) {
                                if (!ok) return;
                                Logger::Log("onSdkExposureTimerTimeout | Auto Save enabled, saving captured image...",
                                            LogLevel::INFO, DeviceType::CAMERA);
                                CaptureImageSaveAsync();
                            });
                        } else {
                            saveFitsAsPNG_FromSdkFrame(framePtr, true);
                        }
                    }
                    return;
                }

                // 获取失败，继续等待（主线程只做定时器调度，不做阻塞 SDK 调用）
                Logger::Log("onSdkExposureTimerTimeout | GetSingleFrame failed: " + frameRes.message,
                            LogLevel::DEBUG, DeviceType::CAMERA);

                const bool unsupportedFormat =
                    (frameRes.message.find("unsupported format") != std::string::npos);
                if (unsupportedFormat) {
                    Logger::Log("onSdkExposureTimerTimeout | unsupported frame format is not retryable, stop exposure polling",
                                LogLevel::ERROR, DeviceType::CAMERA);
                    glMainCameraStatu = "IDLE";
                    ShootStatus = "IDLE";
                    sdkExposureIsROI = false;
                    if (isRoiSnap) {
                        glIsFocusingLooping = false;
                        isFocusLoopShooting = false;
                        emit wsThread->sendMessageToClient("startFocusLoopFailed:SDK unsupported frame format");
                    } else {
                        emit wsThread->sendMessageToClient("ExposureFailed:SDK unsupported frame format");
                    }
                    return;
                }

                // 检查：如果是 ROI 模式但 ROI 循环已停止，不要重启定时器
                if (isRoiSnap && !isFocusLoopShooting) {
                    Logger::Log("onSdkExposureTimerTimeout | ROI loop stopped, abort timer restart",
                                LogLevel::INFO, DeviceType::CAMERA);
                    glMainCameraStatu = "IDLE";
                    return;
                }

                // 检查：如果当前 ROI 标志与快照不一致，说明模式已切换，不要重启定时器
                if (isRoiSnap != sdkExposureIsROI) {
                    Logger::Log("onSdkExposureTimerTimeout | ROI mode changed (snapshot=" +
                                std::string(isRoiSnap ? "true" : "false") + ", current=" +
                                std::string(sdkExposureIsROI ? "true" : "false") + "), abort timer restart",
                                LogLevel::INFO, DeviceType::CAMERA);
                    glMainCameraStatu = "IDLE";
                    return;
                }

                int retryMs = 10;
                if (elapsed2 < expected) {
                    retryMs = static_cast<int>(std::max<qint64>(1, expected - elapsed2));
                } else if (elapsed2 > expected + 10000) {
                    retryMs = 200;
                } else if (elapsed2 > expected + 2000) {
                    retryMs = 50;
                }
                sdkExposureTimer->start(retryMs);
            },
            Qt::QueuedConnection);
    });
}

// SDK 导星曝光定时器回调：轮询获取导星图像（用于 GuiderLoopExpSwitch）
void MainWindow::onSdkGuiderExposureTimerTimeout()
{
    if (!sdkGuiderExposureTimer)
        return;

    // 单次轮询：拿到结果后由本函数决定是否继续 start()
    sdkGuiderExposureTimer->stop();

    const bool poleCapture = (sdkGuiderExposureRole == "PoleCamera");
    const bool guiderSdk =
        (!poleCapture &&
         systemdevicelist.system_devices.size() > 1 &&
         systemdevicelist.system_devices[1].isSDKConnect &&
         sdkGuiderHandle != nullptr);
    const bool poleSdk =
        (poleCapture &&
         systemdevicelist.system_devices.size() > 2 &&
         systemdevicelist.system_devices[2].isSDKConnect &&
         sdkPoleScopeHandle != nullptr);

    if ((!guiderSdk && !poleSdk) || (!isGuiderLoopExp && !polarGuiderSingleCapturePending))
    {
        guiderExposureInFlight = false;
        return;
    }

    SdkSerialExecutor *captureExec = sdkExecutorForPolarRole(poleCapture
                                                                 ? PolarAlignmentCameraRole::PoleCamera
                                                                 : PolarAlignmentCameraRole::Guider);
    if (!captureExec || !captureExec->isRunning())
    {
        Logger::Log("onSdkGuiderExposureTimerTimeout | SDK capture executor not running, stop guider polling",
                    LogLevel::ERROR, DeviceType::GUIDER);
        guiderExposureInFlight = false;
        isGuiderLoopExp = false;
        if (guiderCore)
        {
            postGuiderCore(guiderCore, [](GuiderCore *core) {
                core->stopGuiding();
                core->stopLoop();
            });
        }
        emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
        return;
    }

    // 防重入：如果上一轮 GetSingleFrame 还在 SDK 线程执行，就稍后再试
    if (sdkGuiderFrameTaskInFlight.exchange(true))
    {
        Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | frame task already in flight, retry timer in 10ms",
                    LogLevel::INFO, DeviceType::GUIDER);
        sdkGuiderExposureTimer->start(10);
        return;
    }

    const SdkDeviceHandle handleSnap = poleCapture ? sdkPoleScopeHandle : sdkGuiderHandle;
    const PolarAlignmentCameraRole captureRole =
        poleCapture ? PolarAlignmentCameraRole::PoleCamera : PolarAlignmentCameraRole::Guider;
    const qint64 startSnap = sdkGuiderExposureStartTime;
    const int expectedSnap = sdkGuiderExposureExpectedDuration;
    const qint64 timerFiredAtMs = QDateTime::currentMSecsSinceEpoch();

    Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | fired elapsedSinceExposureStartMs=" +
                    std::to_string(timerFiredAtMs - startSnap) +
                    " expectedMs=" + std::to_string(expectedSnap),
                LogLevel::INFO, DeviceType::GUIDER);

    captureExec->post([this, handleSnap, startSnap, expectedSnap, timerFiredAtMs, captureRole]() {
        QElapsedTimer workerPerf;
        workerPerf.start();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 elapsed = now - startSnap;
        const qint64 expected = std::max<qint64>(1, expectedSnap);
        const qint64 maxWaitMs = std::max<qint64>(expected + 5000, expected * 3);

        Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | sdkWorkerEnter queueDelayMs=" +
                        std::to_string(now - timerFiredAtMs) +
                        " elapsedSinceExposureStartMs=" + std::to_string(elapsed) +
                        " expectedMs=" + std::to_string(expected) +
                        " maxWaitMs=" + std::to_string(maxWaitMs),
                    LogLevel::INFO, DeviceType::GUIDER);

        if (elapsed > maxWaitMs)
        {
            QElapsedTimer cancelPerf;
            cancelPerf.start();
            SdkCommand cancelCmd;
            cancelCmd.type = SdkCommandType::Custom;
            cancelCmd.name = "CancelExposure";
            cancelCmd.payload = std::any();
            SdkResult cancelRes = SdkManager::instance().callByHandle(handleSnap, cancelCmd);
            const qint64 cancelCostMs = cancelPerf.elapsed();
            const qint64 workerTotalMs = workerPerf.elapsed();

            Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | timeout CancelExposure returned success=" +
                            std::to_string(cancelRes.success ? 1 : 0) +
                            " costMs=" + std::to_string(cancelCostMs) +
                            " workerTotalMs=" + std::to_string(workerTotalMs) +
                            " msg=" + cancelRes.message,
                        LogLevel::INFO, DeviceType::GUIDER);

            QMetaObject::invokeMethod(this, [this, cancelRes, elapsed, expected, maxWaitMs, cancelCostMs, workerTotalMs]() {
                sdkGuiderFrameTaskInFlight = false;
                Logger::Log("onSdkGuiderExposureTimerTimeout | TIMEOUT waiting guider frame (elapsed=" +
                                std::to_string(elapsed) + "ms, expected=" + std::to_string(expected) +
                                "ms, maxWait=" + std::to_string(maxWaitMs) +
                                "ms, cancelCost=" + std::to_string(cancelCostMs) +
                                "ms, workerTotal=" + std::to_string(workerTotalMs) + "ms)",
                            LogLevel::ERROR, DeviceType::GUIDER);
                if (!cancelRes.success)
                {
                    Logger::Log("onSdkGuiderExposureTimerTimeout | CancelExposure failed: " + cancelRes.message,
                                LogLevel::WARNING, DeviceType::GUIDER);
                }
                guiderExposureInFlight = false;
                polarGuiderSingleCapturePending = false;
                isGuiderLoopExp = false;
                if (guiderCore)
                {
                    postGuiderCore(guiderCore, [](GuiderCore *core) {
                        core->stopGuiding();
                        core->stopLoop();
                    });
                }
                emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
                emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
            }, Qt::QueuedConnection);
            return;
        }

        // GetSingleFrame（SDK 线程）
        SdkCommand getFrameCmd;
        getFrameCmd.type = SdkCommandType::Custom;
        getFrameCmd.name = "GetSingleFrame";
        getFrameCmd.payload = std::any();
        QElapsedTimer getFramePerf;
        getFramePerf.start();
        SdkResult frameRes = SdkManager::instance().callByHandle(handleSnap, getFrameCmd);
        const qint64 getFrameCostMs = getFramePerf.elapsed();
        const qint64 workerTotalMs = workerPerf.elapsed();

        Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | GetSingleFrame returned success=" +
                        std::to_string(frameRes.success ? 1 : 0) +
                        " costMs=" + std::to_string(getFrameCostMs) +
                        " workerTotalMs=" + std::to_string(workerTotalMs) +
                        " msg=" + frameRes.message,
                    LogLevel::INFO, DeviceType::GUIDER);

        QMetaObject::invokeMethod(this, [this, frameRes, expected, getFrameCostMs, workerTotalMs, timerFiredAtMs, captureRole]() mutable {
            QElapsedTimer mainPerf;
            mainPerf.start();
            sdkGuiderFrameTaskInFlight = false;

            Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | mainThreadResultEnter totalSinceTimerFiredMs=" +
                            std::to_string(QDateTime::currentMSecsSinceEpoch() - timerFiredAtMs) +
                            " getFrameCostMs=" + std::to_string(getFrameCostMs) +
                            " workerTotalMs=" + std::to_string(workerTotalMs),
                        LogLevel::INFO, DeviceType::GUIDER);

            const bool poleCapture = (captureRole == PolarAlignmentCameraRole::PoleCamera);
            const bool captureSdk =
                poleCapture
                    ? (systemdevicelist.system_devices.size() > 2 &&
                       systemdevicelist.system_devices[2].isSDKConnect &&
                       sdkPoleScopeHandle != nullptr)
                    : (systemdevicelist.system_devices.size() > 1 &&
                       systemdevicelist.system_devices[1].isSDKConnect &&
                       sdkGuiderHandle != nullptr);
            if (!captureSdk)
            {
                guiderExposureInFlight = false;
                return;
            }

            if (frameRes.success)
            {
                SdkFrameData frame = std::any_cast<SdkFrameData>(frameRes.payload);
                const bool hasFrameData =
                    (!frame.pixels.empty()) || (frame.rawBuffer != nullptr && frame.rawBytes > 0);
                if (frame.width <= 0 || frame.height <= 0 || !hasFrameData)
                {
                    Logger::Log("onSdkGuiderExposureTimerTimeout | invalid frame, retry",
                                LogLevel::WARNING, DeviceType::GUIDER);
                }
                else
                {
                    // SDK 导星帧统一走内置导星链路：
                    // 1) 先写固定 FITS 路径
                    // 2) 交给 GuiderCore::onNewFrame（内部会触发 PersistGuidingFits + 识别）
                    const QString sdkGuiderFitsPath = poleCapture
                        ? QStringLiteral("/dev/shm/polecamera.fits")
                        : QStringLiteral("/dev/shm/guiding.fits");
                    QElapsedTimer saveFitsPerf;
                    saveFitsPerf.start();
                    SaveQhyFrameDataToFits(frame, sdkGuiderFitsPath.toStdString());
                    Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | SaveQhyFrameDataToFits costMs=" +
                                    std::to_string(saveFitsPerf.elapsed()) +
                                    " mainTotalMs=" + std::to_string(mainPerf.elapsed()),
                                LogLevel::INFO, DeviceType::GUIDER);

                    if (polarGuiderSingleCapturePending)
                    {
                        notifyPolarAlignmentCaptureReady(captureRole, sdkGuiderFitsPath);
                        polarGuiderSingleCapturePending = false;
                    }

                    if (!poleCapture && guiderCore)
                    {
                        QElapsedTimer invokePerf;
                        invokePerf.start();
                        QMetaObject::invokeMethod(guiderCore, "onNewFrame", Qt::QueuedConnection,
                                                  Q_ARG(QString, sdkGuiderFitsPath));
                        Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | invoke guiderCore onNewFrame costMs=" +
                                        std::to_string(invokePerf.elapsed()) +
                                        " mainTotalMs=" + std::to_string(mainPerf.elapsed()),
                                    LogLevel::INFO, DeviceType::GUIDER);
                    }
                    else
                    {
                        QElapsedTimer persistPerf;
                        persistPerf.start();
                        PersistGuidingFits(sdkGuiderFitsPath);
                        Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | PersistGuidingFits costMs=" +
                                        std::to_string(persistPerf.elapsed()) +
                                        " mainTotalMs=" + std::to_string(mainPerf.elapsed()),
                                    LogLevel::INFO, DeviceType::GUIDER);
                    }
                }

                // 一帧处理完成，放行下一帧
                guiderExposureInFlight = false;
                if (isGuiderLoopExp && guiderLoopTimer)
                    guiderLoopTimer->start(1);
                Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | success path done mainTotalMs=" +
                                std::to_string(mainPerf.elapsed()),
                            LogLevel::INFO, DeviceType::GUIDER);
                return;
            }

            // 未就绪：继续轮询
            const qint64 elapsed2 = QDateTime::currentMSecsSinceEpoch() - sdkGuiderExposureStartTime;
            Logger::Log("onSdkGuiderExposureTimerTimeout | GetSingleFrame not ready: " + frameRes.message,
                        LogLevel::DEBUG, DeviceType::GUIDER);
            int retryMs = 10;
            if (elapsed2 < expected) {
                retryMs = static_cast<int>(std::max<qint64>(1, expected - elapsed2));
            } else if (elapsed2 > expected + 2000) {
                retryMs = 50;
            }
            Logger::Log("GuiderPerf | onSdkGuiderExposureTimerTimeout | not ready retryMs=" +
                            std::to_string(retryMs) +
                            " elapsedSinceExposureStartMs=" + std::to_string(elapsed2) +
                            " mainTotalMs=" + std::to_string(mainPerf.elapsed()),
                        LogLevel::INFO, DeviceType::GUIDER);
            sdkGuiderExposureTimer->start(retryMs);
        }, Qt::QueuedConnection);
    });
}




#if 0
// ============================================================================
// PHD2 已移除（2026-01）：原 PHD2 进程/共享内存/导星控制逻辑全部下线。
// 保留代码仅作历史参考，不参与编译与运行。
// ============================================================================

void MainWindow::InitPHD2()
{
    Logger::Log("InitPHD2 start ...", LogLevel::INFO, DeviceType::MAIN);
    isGuideCapture = true;

    if (!cmdPHD2) cmdPHD2 = new QProcess();
    static bool phdSignalsConnected = false;
    if (!phdSignalsConnected)
    {
        connect(cmdPHD2, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &MainWindow::onPhd2Exited);
        connect(cmdPHD2, &QProcess::errorOccurred,
                this, &MainWindow::onPhd2Error);
        phdSignalsConnected = true;
    }

    bool connected = false;
    int retryCount = 1; // 设定重试次数
    while (retryCount > 0 && !connected)
    {
        // 启动前强制结束残留进程并清理共享内存
        // 以避免“初次启动时无法关闭导致启动失败”的问题
        // 1) 若之前有 QProcess 实例，先尝试优雅结束并回收，避免僵尸进程
        if (cmdPHD2->state() != QProcess::NotRunning) {
            cmdPHD2->terminate();
            if (!cmdPHD2->waitForFinished(1500)) {
                cmdPHD2->kill();
                cmdPHD2->waitForFinished(1000);
            }
        }
        // 2) 系统级强杀（多种匹配方式）
        // 注意：有的系统进程名是 phd2.bin，这里同时匹配 phd2 和 phd2.bin
        QProcess::execute("pkill", QStringList() << "-TERM" << "-x" << "phd2");
        QProcess::execute("pkill", QStringList() << "-TERM" << "-x" << "phd2.bin");
        QThread::msleep(200);
        QProcess::execute("pkill", QStringList() << "-KILL" << "-x" << "phd2");
        QProcess::execute("pkill", QStringList() << "-KILL" << "-x" << "phd2.bin");
        QThread::msleep(100);
        // 3) 宽匹配（包含路径/命令行）
        QProcess::execute("pkill", QStringList() << "-TERM" << "-f" << "phd2");
        QThread::msleep(150);
        QProcess::execute("pkill", QStringList() << "-KILL" << "-f" << "phd2");
        QThread::msleep(150);
        // 4) 轮询确认已无残留进程（最多 1s）
        {
            QElapsedTimer waitKill;
            waitKill.start();
            while (waitKill.elapsed() < 1000) {
                int rc = QProcess::execute("pgrep", QStringList() << "-f" << "phd2");
                if (rc != 0) break; // 无匹配
                QThread::msleep(100);
            }
        }

        // 5) 启动前清空 PHD2 日志目录，以规避“损坏/异常 GuidingLog 导致启动卡死”的问题
        //    目标目录：/home/quarcs/Documents/PHD2
        {
            const QString phd2LogDirPath = QStringLiteral("/home/quarcs/Documents/PHD2");
            QDir phd2LogDir(phd2LogDirPath);
            if (phd2LogDir.exists())
            {
                // 递归删除整个日志目录及其内容，然后重新创建一个空目录
                if (!phd2LogDir.removeRecursively())
                {
                    Logger::Log("InitPHD2 | failed to clear PHD2 log dir: " + phd2LogDirPath.toStdString(),
                                LogLevel::WARNING, DeviceType::MAIN);
                }
            }
            // 确保目录最终存在（即使之前不存在或被删除）
            if (!phd2LogDir.mkpath("."))
            {
                Logger::Log("InitPHD2 | failed to recreate PHD2 log dir: " + phd2LogDirPath.toStdString(),
                            LogLevel::WARNING, DeviceType::MAIN);
            }
            else
            {
                Logger::Log("InitPHD2 | PHD2 log dir cleared: " + phd2LogDirPath.toStdString(),
                            LogLevel::INFO, DeviceType::MAIN);
            }
        }
        // 清理共享内存段（key=0x90）
        key_t cleanup_key = 0x90;
        int cleanup_id = shmget(cleanup_key, BUFSZ_PHD, 0666);
        if (cleanup_id != -1) shmctl(cleanup_id, IPC_RMID, NULL);

        // 重新生成共享内存，避免之前的进程遗留问题
        key_phd = 0x90;                                           // 重新设置共享内存的键值
        shmid_phd = shmget(key_phd, BUFSZ_PHD, IPC_CREAT | 0666); // 获取共享内存
        if (shmid_phd < 0)
        {
            Logger::Log("InitPHD2 | shared memory phd shmget ERROR", LogLevel::ERROR, DeviceType::MAIN);
            continue;
        }

        // 映射共享内存
        sharedmemory_phd = (char *)shmat(shmid_phd, NULL, 0);
        if (sharedmemory_phd == NULL)
        {
            Logger::Log("InitPHD2 | shared memory phd map ERROR", LogLevel::ERROR, DeviceType::MAIN);
            continue;
        }

        // 读取共享内存数据（避免将共享内存当作以\\0 结尾字符串打印，可能越界）
        Logger::Log("InitPHD2 | shared memory mapped", LogLevel::INFO, DeviceType::MAIN);

        // 启动 phd2 进程（显式指定实例号 1）
        cmdPHD2->start("phd2", QStringList() << "-i" << "1");
        phd2ExpectedRunning = true;

        // 等待最多 10 秒尝试连接
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 2000)
        {
            usleep(1000);
            qApp->processEvents();
            if (connectPHD() == true)
            {
                connected = true;
                break;
            }
        }

        if (!connected)
        {
            Logger::Log("InitPHD2 | Failed to connect to phd2. Retrying...", LogLevel::WARNING, DeviceType::MAIN);
            retryCount--; // 如果连接失败，重试次数减 1
        }
    }

    if (!connected)
    {
        Logger::Log("InitPHD2 | Failed to connect to phd2 after retries.", LogLevel::ERROR, DeviceType::MAIN);
    }

    Logger::Log("InitPHD2 finished.", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::onPhd2Exited(int exitCode, QProcess::ExitStatus exitStatus)
{
    Logger::Log("PHD2 exited. code=" + std::to_string(exitCode) +
                " status=" + std::to_string((int)exitStatus), LogLevel::WARNING, DeviceType::GUIDER);
    if (phd2ExpectedRunning)
    {
        // 进程异常结束时，尝试发送一次“停止循环拍摄”命令以收敛前端状态
        call_phd_StopLooping();
        
        // TODO(PHD2): 相关前端信号发送已暂停（切换到 INDI 导星直出图逻辑后不再维护 PHD2 UI）
        // emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        phd2ExpectedRunning = false;
        // 清理共享内存段，避免前端继续读到旧数据
        key_t key = 0x90;
        int id = shmget(key, BUFSZ_PHD, 0666);
        if (id != -1) shmctl(id, IPC_RMID, NULL);
        // TODO(PHD2): 提示前端是否重启（暂不发送）
        // emit wsThread->sendMessageToClient("PHD2ClosedUnexpectedly:是否重新启动PHD2?");
    }
}

void MainWindow::onPhd2Error(QProcess::ProcessError error)
{
    Logger::Log("PHD2 process error: " + std::to_string((int)error), LogLevel::ERROR, DeviceType::GUIDER);
    if (phd2ExpectedRunning)
    {
        // 进程错误时，尝试发送“停止循环拍摄”命令以确保状态一致
        call_phd_StopLooping();
        // TODO(PHD2): 相关前端信号发送已暂停（切换到 INDI 导星直出图逻辑后不再维护 PHD2 UI）
        // emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        phd2ExpectedRunning = false;
        key_t key = 0x90;
        int id = shmget(key, BUFSZ_PHD, 0666);
        if (id != -1) shmctl(id, IPC_RMID, NULL);
        // TODO(PHD2): 提示前端是否重启（暂不发送）
        // emit wsThread->sendMessageToClient("PHD2ClosedUnexpectedly:是否重新启动PHD2?");
    }
}
bool MainWindow::connectPHD(void)
{
    Logger::Log("connectPHD start ...", LogLevel::INFO, DeviceType::MAIN);
    QString versionName = "";
    call_phd_GetVersion(versionName);

    Logger::Log("connectPHD | version:" + versionName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    if (versionName != "")
    {
        // init stellarium operation
        Logger::Log("connectPHD Success!", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
    else
    {
        Logger::Log("connectPHD | there is no openPHD2 running", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("connectPHD failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
}

void MainWindow::disconnectFocuserIfConnected()
{
    if (dpFocuser && dpFocuser->isConnected())
    {
        DisconnectDevice(indi_Client, dpFocuser->getDeviceName(), "Focuser");
    }
    else if (systemdevicelist.system_devices.size() > 22 &&
             systemdevicelist.system_devices[22].isSDKConnect &&
             sdkFocuserHandle != nullptr)
    {
        SdkManager::instance().closeByHandle(sdkFocuserHandle);
        sdkFocuserHandle = nullptr;
        sdkFocuserPort.clear();
        systemdevicelist.system_devices[22].isConnect = false;
        systemdevicelist.system_devices[22].isBind = false;
    }
}

bool MainWindow::call_phd_GetVersion(QString &versionName)
{
    Logger::Log("call_phd_GetVersion start ...", LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_GetVersion | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        versionName = "";
        return false;
    }
    
    unsigned int baseAddress;
    unsigned int vendcommand;
    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x01;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    // 放宽首次连接等待时长，避免 PHD2 在树莓派等设备上启动/初始化较慢导致的超时
    // 最长等待 10 秒，让 PHD2 有充分时间写回版本信息
    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 2000)
    {
        QThread::msleep(2);
    }

    // 如果超过 10 秒仍未收到响应，则认为超时
    if (t.elapsed() >= 2000)
    {
        versionName = "";
        Logger::Log("call_phd_GetVersion | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_GetVersion failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }
    else
    {
        unsigned char addr = 0;
        uint16_t length;
        memcpy(&length, sharedmemory_phd + baseAddress + addr, sizeof(uint16_t));
        addr = addr + sizeof(uint16_t);
        // qDebug()<<length;

        if (length > 0 && length < 1024)
        {
            for (int i = 0; i < length; i++)
            {
                versionName.append(sharedmemory_phd[baseAddress + addr + i]);
            }
            Logger::Log("call_phd_GetVersion | version:" + versionName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
            Logger::Log("call_phd_GetVersion success.", LogLevel::INFO, DeviceType::MAIN);
            return true;
            // qDebug()<<versionName;
        }
        else
        {
            versionName = "";
            Logger::Log("call_phd_GetVersion | version is empty", LogLevel::ERROR, DeviceType::MAIN);
            Logger::Log("call_phd_GetVersion failed.", LogLevel::ERROR, DeviceType::MAIN);
            return false;
        }
    }
}

uint32_t MainWindow::call_phd_StartLooping(void)
{
    Logger::Log("call_phd_StartLooping start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_StartLooping | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }
    
    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x03;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // 避免忙等占满 CPU，增加适度休眠
        QThread::msleep(100);
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_StartLooping | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_StartLooping failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_StartLooping success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_StopLooping(void)
{
    Logger::Log("call_phd_StopLooping start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_StopLooping | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }
    
    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x04;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // 避免忙等占满 CPU，增加适度休眠
        QThread::msleep(100);
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_StopLooping | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_StopLooping failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_StopLooping success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_AutoFindStar(void)
{
    Logger::Log("call_phd_AutoFindStar start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_AutoFindStar | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }
    
    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x05;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_AutoFindStar | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_AutoFindStar failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_AutoFindStar success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_StartGuiding(void)
{
    Logger::Log("call_phd_StartGuiding start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_StartGuiding | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }
    
    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x06;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_StartGuiding | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_StartGuiding failed.", LogLevel::ERROR, DeviceType::GUIDER);
        // 在启动导星失败时，发送关闭循环拍摄的命令
        // call_phd_StopLooping();
        // 通知前端状态：循环曝光已关闭
        // emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        isGuiding = false;
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_StartGuiding success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_StopGuiding(void)
{
    Logger::Log("call_phd_StopGuiding start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_StopGuiding | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x17; // 与 PHD2 端 myframe.cpp 中定义的“Stop Guiding Only”命令码保持一致

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_StopGuiding | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_StopGuiding failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_StopGuiding success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}

void MainWindow::pauseGuidingBeforeMountMove()
{
    // 仅在当前逻辑导星开关为 ON 时，才在移动前主动停止导星，并记录状态以便之后恢复
    wasGuidingBeforeMountMove = false;

    if (isGuiding)
    {
        Logger::Log("pauseGuidingBeforeMountMove | guiding is ON, stop guiding before mount move.",
                    LogLevel::INFO, DeviceType::GUIDER);
        wasGuidingBeforeMountMove = true;
        // 仅停止导星，不关闭循环曝光，避免影响 PHD2 的 Loop 按钮语义
        call_phd_StopGuiding();
        // 这里不去强制修改 isGuiding / 循环开关，由 PHD2 端自身状态机与前端 UI 控制
    }
}

void MainWindow::resumeGuidingAfterMountMove()
{
    if (!wasGuidingBeforeMountMove)
    {
        // 移动前没有在导星，无需恢复
        return;
    }

    Logger::Log("resumeGuidingAfterMountMove | mount move finished, resume guiding.",
                LogLevel::INFO, DeviceType::GUIDER);

    // 参考 GuiderSwitch=true 的逻辑：如需清校准则先清，再根据是否已选星决定是否自动寻星，最后启动导星
    if (ClearCalibrationData)
    {
        ClearCalibrationData = false;
        call_phd_ClearCalibration();
        Logger::Log("resumeGuidingAfterMountMove | clear calibration data before restart guiding.",
                    LogLevel::INFO, DeviceType::GUIDER);
    }

    if (!glPHD_isSelected)
    {
        Logger::Log("resumeGuidingAfterMountMove | no selected star, call AutoFindStar before guiding.",
                    LogLevel::INFO, DeviceType::GUIDER);
        call_phd_AutoFindStar();
    }

    call_phd_StartGuiding();
    // TODO(PHD2): 相关前端信号发送已暂停（切换到 INDI 导星直出图逻辑后不再维护 PHD2 UI）
    // emit wsThread->sendMessageToClient("GuiderUpdateStatus:1");
}

uint32_t MainWindow::call_phd_checkStatus(unsigned char &status)
{
    Logger::Log("call_phd_checkStatus start ...", LogLevel::DEBUG, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_checkStatus | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        status = 0;
        return false;
    }
    
    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x07;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    // wait stellarium finished this task
    QElapsedTimer t;
    t.start();
    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    } // wait stellarium run end

    if (t.elapsed() >= 500)
    {
        // timeout
        status = 0;
        Logger::Log("call_phd_checkStatus | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_checkStatus failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }

    else
    {
        status = sharedmemory_phd[3];
        Logger::Log("call_phd_checkStatus | status:" + std::to_string(status), LogLevel::DEBUG, DeviceType::GUIDER);
        Logger::Log("call_phd_checkStatus success.", LogLevel::DEBUG, DeviceType::GUIDER);
        return true;
    }
}

uint32_t MainWindow::call_phd_setExposureTime(unsigned int expTime)
{
    Logger::Log("call_phd_setExposureTime start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_setExposureTime | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }
    
    unsigned int vendcommand;
    unsigned int baseAddress;
    Logger::Log("call_phd_setExposureTime | expTime:" + std::to_string(expTime), LogLevel::INFO, DeviceType::GUIDER);

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x0b;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &expTime, sizeof(unsigned int));
    addr = addr + sizeof(unsigned int);

    sharedmemory_phd[0] = 0x01; // enable command

    // wait stellarium finished this task
    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    } // wait stellarium run end

    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_setExposureTime | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_setExposureTime failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return QHYCCD_ERROR; // timeout
    }
    else
    {
        Logger::Log("call_phd_setExposureTime success.", LogLevel::INFO, DeviceType::GUIDER);
        return QHYCCD_SUCCESS;
    }
}

uint32_t MainWindow::call_phd_whichCamera(std::string Camera)
{
    Logger::Log("call_phd_whichCamera start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_whichCamera | Camera:" + Camera, LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_whichCamera | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return QHYCCD_ERROR;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x0d;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    int length = Camera.length() + 1;

    unsigned char addr = 0;
    // memcpy(sharedmemory_phd + baseAddress + addr, &index, sizeof(int));
    // addr = addr + sizeof(int);
    memcpy(sharedmemory_phd + baseAddress + addr, &length, sizeof(int));
    addr = addr + sizeof(int);
    memcpy(sharedmemory_phd + baseAddress + addr, Camera.c_str(), length);
    addr = addr + length;

    // wait stellarium finished this task
    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    } // wait stellarium run end

    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_whichCamera | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_whichCamera failed.", LogLevel::ERROR, DeviceType::MAIN);
        return QHYCCD_ERROR; // timeout
    }
    else
    {
        Logger::Log("call_phd_whichCamera success.", LogLevel::INFO, DeviceType::MAIN);
        return QHYCCD_SUCCESS;
    }
}

uint32_t MainWindow::call_phd_ChackControlStatus(int sdk_num)
{
    Logger::Log("call_phd_ChackControlStatus start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_ChackControlStatus | sdk_num:" + std::to_string(sdk_num), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_ChackControlStatus | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x0e;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &sdk_num, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_ChackControlStatus | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_ChackControlStatus failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_ChackControlStatus success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_ClearCalibration(void)
{
    Logger::Log("call_phd_ClearCalibration start ...", LogLevel::INFO, DeviceType::GUIDER);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_ClearCalibration | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return false;
    }
    
    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x02;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_ClearCalibration | timeout", LogLevel::ERROR, DeviceType::GUIDER);
        Logger::Log("call_phd_ClearCalibration failed.", LogLevel::ERROR, DeviceType::GUIDER);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_ClearCalibration success.", LogLevel::INFO, DeviceType::GUIDER);
        return true;
    }
}
uint32_t MainWindow::call_phd_StarClick(int x, int y)
{
    Logger::Log("call_phd_StarClick start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_StarClick | x:" + std::to_string(x) + ", y:" + std::to_string(y), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        // 说明：当前工程已默认使用内置导星（GuiderCore），模拟导星/不启动PHD2时这里会被前端误触发。
        // 为避免干扰导星日志，这里降级为 WARNING。
        Logger::Log("call_phd_StarClick | shared memory not ready (PHD2 not running?)", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x0f;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &x, sizeof(int));
    addr = addr + sizeof(int);
    memcpy(sharedmemory_phd + baseAddress + addr, &y, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_StarClick | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_StarClick failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_StarClick success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_FocalLength(int FocalLength)
{
    Logger::Log("call_phd_FocalLength start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_FocalLength | FocalLength:" + std::to_string(FocalLength), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_FocalLength | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x10;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &FocalLength, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_FocalLength | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_FocalLength failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_FocalLength success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_MultiStarGuider(bool isMultiStar)
{
    Logger::Log("call_phd_MultiStarGuider start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_MultiStarGuider | isMultiStar:" + std::to_string(isMultiStar), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_MultiStarGuider | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x11;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &isMultiStar, sizeof(bool));
    addr = addr + sizeof(bool);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_MultiStarGuider | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_MultiStarGuider failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_MultiStarGuider success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_CameraPixelSize(double PixelSize)
{
    Logger::Log("call_phd_CameraPixelSize start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_CameraPixelSize | PixelSize:" + std::to_string(PixelSize), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_CameraPixelSize | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x12;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &PixelSize, sizeof(double));
    addr = addr + sizeof(double);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_CameraPixelSize | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_CameraPixelSize failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_CameraPixelSize success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_CameraGain(int Gain)
{
    Logger::Log("call_phd_CameraGain start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_CameraGain | Gain:" + std::to_string(Gain), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_CameraGain | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x13;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &Gain, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_CameraGain | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_CameraGain failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_CameraGain success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_CalibrationDuration(int StepSize)
{
    Logger::Log("call_phd_CalibrationDuration start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_CalibrationDuration | StepSize:" + std::to_string(StepSize), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_CalibrationDuration | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x14;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &StepSize, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_CalibrationDuration | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_CalibrationDuration failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_CalibrationDuration success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_RaAggression(int Aggression)
{
    Logger::Log("call_phd_RaAggression start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_RaAggression | Aggression:" + std::to_string(Aggression), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_RaAggression | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x15;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &Aggression, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_RaAggression | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_RaAggression failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_RaAggression success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

uint32_t MainWindow::call_phd_DecAggression(int Aggression)
{
    Logger::Log("call_phd_DecAggression start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("call_phd_DecAggression | Aggression:" + std::to_string(Aggression), LogLevel::INFO, DeviceType::MAIN);
    // 修复：检查共享内存指针有效性
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("call_phd_DecAggression | shared memory not ready", LogLevel::ERROR, DeviceType::MAIN);
        return false;
    }

    unsigned int vendcommand;
    unsigned int baseAddress;

    bzero(sharedmemory_phd, 1024); // 共享内存清空

    baseAddress = 0x03;
    vendcommand = 0x16;

    sharedmemory_phd[1] = Tools::MSB(vendcommand);
    sharedmemory_phd[2] = Tools::LSB(vendcommand);

    sharedmemory_phd[0] = 0x01; // enable command

    unsigned char addr = 0;
    memcpy(sharedmemory_phd + baseAddress + addr, &Aggression, sizeof(int));
    addr = addr + sizeof(int);

    QElapsedTimer t;
    t.start();

    while (sharedmemory_phd[0] == 0x01 && t.elapsed() < 500)
    {
        // QCoreApplication::processEvents();
    }
    if (t.elapsed() >= 500)
    {
        Logger::Log("call_phd_DecAggression | timeout", LogLevel::ERROR, DeviceType::MAIN);
        Logger::Log("call_phd_DecAggression failed.", LogLevel::ERROR, DeviceType::MAIN);
        return false; // timeout
    }
    else
    {
        Logger::Log("call_phd_DecAggression success.", LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
}

// ===================== 读取端：共享内存像素解码（完整实现） =====================
// 说明：
// 1) 保持你原有的共享内存布局不变：
//    - [0 .. 1023]  : 预留区（新增 V2 头 ShmHdrV2 放在这里，兼容旧读端）
//    - [1024 ..]    : 你原有的头/状态/导星数据（currentPHDSizeX/Y、bitDepth、各项 guide 数据等）
//    - [2047]       : 帧完成标志位（0x01=写入中，0x02=完成，0x00=已读）
//    - [2048 .. end]: 像素数据（RAW / RLE 压缩 / NEAREST 缩放）
// 2) 本实现自动识别新头（若存在），按 coding 解码；若无新头，则回退旧逻辑。
// 3) 不改变你已有的“读取的内容”（导星/状态字段），仅在像素拷贝前做安全边界检查与解码。


// ===== RLE 解压（速度优先的简单实现）=====
static bool rle_decompress_8(const uint8_t* src, size_t n, uint8_t* dst, size_t outPixels) {
    size_t si=0, di=0;
    while (si+1 <= n && di < outPixels) {
        if (si + 1 > n) return false;
        uint8_t v = src[si++];
        if (si >= n) return false;
        uint8_t run = src[si++];
        if ((size_t)di + run > outPixels) return false;
        std::memset(dst + di, v, run);
        di += run;
    }
    return di == outPixels;
}

static bool rle_decompress_16(const uint8_t* src, size_t n, uint16_t* dst, size_t outPixels) {
    size_t si=0, di=0;
    while (si+4 <= n && di < outPixels) {
        uint16_t v, run;
        std::memcpy(&v,   src+si, 2); si += 2;
        std::memcpy(&run, src+si, 2); si += 2;
        if ((size_t)di + run > outPixels) return false;
        for (uint16_t k=0;k<run;++k) dst[di++] = v;
    }
    return di == outPixels;
}

// ====== 完整的读取函数（在你的 MainWindow 类内）=====
void MainWindow::ShowPHDdata()
{
    // 已弃用 PHD2：保留函数签名以减少改动，但不再读取共享内存，避免刷屏日志。
    return;

    // 修复：增强共享内存指针安全检查
    // 早退：共享内存可用且帧完成
    if (!sharedmemory_phd || sharedmemory_phd == (char*)-1) {
        Logger::Log("ShowPHDdata | shared memory not ready", LogLevel::ERROR, DeviceType::GUIDER);
        return;
    }
    
    // 修复：验证共享内存大小，确保kFlagOff在有效范围内
    const size_t total_size = (size_t)BUFSZ;
    if (kFlagOff >= total_size) {
        Logger::Log("ShowPHDdata | kFlagOff out of bounds", LogLevel::ERROR, DeviceType::GUIDER);
        return;
    }
    
    if (sharedmemory_phd[kFlagOff] != 0x02) {
        // 没有新帧
        return;
    }

    // ---------- 读图像原始头（与旧协议完全一致） ----------
    unsigned int currentPHDSizeX = 1;
    unsigned int currentPHDSizeY = 1;
    unsigned int bitDepth        = 8;

    unsigned int mem_offset = kHeaderOff;

    auto ensure = [&](size_t need) -> bool {
        // 头部区域必须在 payload 前结束
        return (mem_offset + need <= kPayloadOff && mem_offset + need <= total_size);
    };

    if (!ensure(sizeof(unsigned int))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&currentPHDSizeX, sharedmemory_phd + mem_offset, sizeof(unsigned int));
    mem_offset += sizeof(unsigned int);

    if (!ensure(sizeof(unsigned int))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&currentPHDSizeY, sharedmemory_phd + mem_offset, sizeof(unsigned int));
    mem_offset += sizeof(unsigned int);

    if (!ensure(sizeof(unsigned char))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&bitDepth, sharedmemory_phd + mem_offset, sizeof(unsigned char));
    mem_offset += sizeof(unsigned char);

    if (!(bitDepth == 8 || bitDepth == 16)) {
        Logger::Log("ShowPHDdata | invalid bitDepth: " + std::to_string(bitDepth), LogLevel::WARNING, DeviceType::GUIDER);
        sharedmemory_phd[kFlagOff] = 0x00;
        return;
    }

    /* ------------------------------  新增：先读 V2 头，决定本帧尺寸  ------------------------------ */
    ShmHdrV2 v2{}; 
    bool hasV2 = false;
    if (total_size >= sizeof(ShmHdrV2)) {
        std::memcpy(&v2, sharedmemory_phd, sizeof(ShmHdrV2));
        hasV2 = (v2.magic == SHM_MAGIC && v2.version == SHM_VER);
    }

    // 本帧用于 UI/WS 的“实际尺寸/位深”
    uint32_t dispW = hasV2 ? v2.outW : currentPHDSizeX;
    uint32_t dispH = hasV2 ? v2.outH : currentPHDSizeY;
    uint16_t useDepth = hasV2 ? v2.bitDepth : (uint16_t)bitDepth;

    // 合法性兜底
    if (dispW == 0 || dispH == 0 || !(useDepth==8 || useDepth==16)) {
        // 回退到旧头
        hasV2 = false;
        dispW = currentPHDSizeX;
        dispH = currentPHDSizeY;
        useDepth = (uint16_t)bitDepth;
    }

    // 记录原始/输出尺寸与缩放倍数，供坐标换算使用
    glPHD_OrigImageSizeX = hasV2 ? (int)v2.origW : (int)currentPHDSizeX;
    glPHD_OrigImageSizeY = hasV2 ? (int)v2.origH : (int)currentPHDSizeY;
    glPHD_OutImageSizeX  = (int)dispW;
    glPHD_OutImageSizeY  = (int)dispH;
    {
        double sx = (glPHD_OutImageSizeX  > 0) ? (double)glPHD_OrigImageSizeX / (double)glPHD_OutImageSizeX  : 1.0;
        double sy = (glPHD_OutImageSizeY  > 0) ? (double)glPHD_OrigImageSizeY / (double)glPHD_OutImageSizeY  : 1.0;
        int s = (int)std::lround((sx + sy) * 0.5);
        if (s < 1) s = 1;
        glPHD_ImageScale = s;
    }

    // ---------- 跳过你原有的 3 个 int 字段（sdk_*） ----------
    if (!ensure(sizeof(int))) { sharedmemory_phd[kFlagOff]=0x00; return; }  mem_offset += sizeof(int);
    if (!ensure(sizeof(int))) { sharedmemory_phd[kFlagOff]=0x00; return; }  mem_offset += sizeof(int);
    if (!ensure(sizeof(int))) { sharedmemory_phd[kFlagOff]=0x00; return; }  mem_offset += sizeof(int);

    // ---------- 读取导星/状态数据（保持不变，不缺少） ----------
    unsigned int guideDataIndicatorAddress = (unsigned int)mem_offset;
    if (!ensure(sizeof(unsigned char))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    unsigned char guideDataIndicator = *(unsigned char*)(sharedmemory_phd + mem_offset);
    mem_offset += sizeof(unsigned char);

    double dRa=0, dDec=0, SNR=0, MASS=0, RMSErrorX=0, RMSErrorY=0, RMSErrorTotal=0, PixelRatio=1;
    int RADUR=0, DECDUR=0; char RADIR=0, DECDIR=0; bool StarLostAlert=false, InGuiding=false;

    auto safe_copy = [&](void* dst, size_t n) -> bool {
        if (!ensure(n)) { sharedmemory_phd[kFlagOff]=0x00; return false; }
        std::memcpy(dst, sharedmemory_phd + mem_offset, n);
        mem_offset += n;
        return true;
    };

    if (!safe_copy(&dRa, sizeof(double))) return;
    if (!safe_copy(&dDec, sizeof(double))) return;
    if (!safe_copy(&SNR, sizeof(double))) return;
    if (!safe_copy(&MASS, sizeof(double))) return;
    if (!safe_copy(&RADUR, sizeof(int))) return;
    if (!safe_copy(&DECDUR, sizeof(int))) return;
    if (!safe_copy(&RADIR, sizeof(char))) return;
    if (!safe_copy(&DECDIR, sizeof(char))) return;
    if (!safe_copy(&RMSErrorX, sizeof(double))) return;
    if (!safe_copy(&RMSErrorY, sizeof(double))) return;
    if (!safe_copy(&RMSErrorTotal, sizeof(double))) return;
    if (!safe_copy(&PixelRatio, sizeof(double))) return;
    if (!safe_copy(&StarLostAlert, sizeof(bool))) return;
    if (!safe_copy(&InGuiding, sizeof(bool))) return;

    // 你的 1024+200 区域：锁星、十字、MultiStar（保持不变）
    mem_offset = kHeaderOff + 200;
    auto ensure_at = [&](size_t off, size_t n)->bool {
        return (off + n <= kPayloadOff && off + n <= total_size);
    };

    bool isSelected=false, showLockedCross=false;
    double StarX=0, StarY=0, LockedPositionX=0, LockedPositionY=0;
    unsigned char MultiStarNumber=0;
    unsigned short MultiStarX[32]={0}, MultiStarY[32]={0};

    if (!ensure_at(mem_offset, sizeof(bool))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&isSelected, sharedmemory_phd + mem_offset, sizeof(bool)); mem_offset += sizeof(bool);

    if (!ensure_at(mem_offset, sizeof(double))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&StarX, sharedmemory_phd + mem_offset, sizeof(double)); mem_offset += sizeof(double);

    if (!ensure_at(mem_offset, sizeof(double))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&StarY, sharedmemory_phd + mem_offset, sizeof(double)); mem_offset += sizeof(double);

    if (!ensure_at(mem_offset, sizeof(bool))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&showLockedCross, sharedmemory_phd + mem_offset, sizeof(bool)); mem_offset += sizeof(bool);

    if (!ensure_at(mem_offset, sizeof(double))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&LockedPositionX, sharedmemory_phd + mem_offset, sizeof(double)); mem_offset += sizeof(double);

    if (!ensure_at(mem_offset, sizeof(double))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&LockedPositionY, sharedmemory_phd + mem_offset, sizeof(double)); mem_offset += sizeof(double);

    if (!ensure_at(mem_offset, sizeof(unsigned char))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(&MultiStarNumber, sharedmemory_phd + mem_offset, sizeof(unsigned char)); mem_offset += sizeof(unsigned char);
    MultiStarNumber = std::min<unsigned char>(MultiStarNumber, 32);

    if (!ensure_at(mem_offset, sizeof(MultiStarX))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(MultiStarX, sharedmemory_phd + mem_offset, sizeof(MultiStarX)); mem_offset += sizeof(MultiStarX);

    if (!ensure_at(mem_offset, sizeof(MultiStarY))) { sharedmemory_phd[kFlagOff]=0x00; return; }
    std::memcpy(MultiStarY, sharedmemory_phd + mem_offset, sizeof(MultiStarY)); mem_offset += sizeof(MultiStarY);

    // 清除导星数据指示位（保持你的行为）
    sharedmemory_phd[guideDataIndicatorAddress] = 0x00;

    // ---------- 将导星/锁星信息分发到 UI/WS（保持你的逻辑） ----------
    glPHD_isSelected         = isSelected;
    glPHD_StarX              = StarX;
    glPHD_StarY              = StarY;
    glPHD_CurrentImageSizeX  = dispW;   // UI 显示尺寸（合并/缩放后）
    glPHD_CurrentImageSizeY  = dispH;   // UI 显示尺寸（合并/缩放后）
    glPHD_LockPositionX      = LockedPositionX;
    glPHD_LockPositionY      = LockedPositionY;
    glPHD_ShowLockCross      = showLockedCross;

    glPHD_Stars.clear();
    // TODO(PHD2): 相关前端信号发送已暂停（切换到 INDI 导星直出图逻辑后不再维护 PHD2 UI）
    // emit wsThread->sendMessageToClient("ClearPHD2MultiStars");
    const double mapRatioX = (glPHD_OrigImageSizeX > 0) ? (double)glPHD_OutImageSizeX / (double)glPHD_OrigImageSizeX : 1.0;
    const double mapRatioY = (glPHD_OrigImageSizeY > 0) ? (double)glPHD_OutImageSizeY / (double)glPHD_OrigImageSizeY : 1.0;
    for (int i = 1; i < MultiStarNumber; i++) {
        if (i > 12) break;
        int outX = (int)std::lround(MultiStarX[i] * mapRatioX);
        int outY = (int)std::lround(MultiStarY[i] * mapRatioY);
        if (outX < 0) outX = 0;
        if (outY < 0) outY = 0;
        if (outX >= glPHD_OutImageSizeX) outX = glPHD_OutImageSizeX - 1;
        if (outY >= glPHD_OutImageSizeY) outY = glPHD_OutImageSizeY - 1;
        QPoint p; p.setX(outX); p.setY(outY);
        glPHD_Stars.push_back(p);
        // TODO(PHD2): 前端多星位置同步（暂不发送）
        // emit wsThread->sendMessageToClient(
        //     "PHD2MultiStarsPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
        //     QString::number(glPHD_CurrentImageSizeY) + ":" +
        //     QString::number(outX) + ":" + QString::number(outY));
    }

    if (glPHD_isSelected) {
        // TODO(PHD2): 前端锁星框显示（暂不发送）
        // emit wsThread->sendMessageToClient("PHD2StarBoxView:true");
        int outStarX = (int)std::lround(glPHD_StarX * mapRatioX);
        int outStarY = (int)std::lround(glPHD_StarY * mapRatioY);
        if (outStarX < 0) outStarX = 0;
        if (outStarY < 0) outStarY = 0;
        if (outStarX >= glPHD_OutImageSizeX) outStarX = glPHD_OutImageSizeX - 1;
        if (outStarY >= glPHD_OutImageSizeY) outStarY = glPHD_OutImageSizeY - 1;
        // TODO(PHD2): 前端锁星框位置（暂不发送）
        // emit wsThread->sendMessageToClient(
        //     "PHD2StarBoxPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
        //     QString::number(glPHD_CurrentImageSizeY) + ":" +
        //     QString::number(outStarX) + ":" + QString::number(outStarY));
    } else {
        // TODO(PHD2): 前端锁星框隐藏（暂不发送）
        // emit wsThread->sendMessageToClient("PHD2StarBoxView:false");
    }

    if (glPHD_ShowLockCross) {
        // TODO(PHD2): 前端锁星十字显示（暂不发送）
        // emit wsThread->sendMessageToClient("PHD2StarCrossView:true");
        int outLockX = (int)std::lround(glPHD_LockPositionX * mapRatioX);
        int outLockY = (int)std::lround(glPHD_LockPositionY * mapRatioY);
        if (outLockX < 0) outLockX = 0;
        if (outLockY < 0) outLockY = 0;
        if (outLockX >= glPHD_OutImageSizeX) outLockX = glPHD_OutImageSizeX - 1;
        if (outLockY >= glPHD_OutImageSizeY) outLockY = glPHD_OutImageSizeY - 1;
        // TODO(PHD2): 前端锁星十字位置（暂不发送）
        // emit wsThread->sendMessageToClient(
        //     "PHD2StarCrossPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
        //     QString::number(glPHD_CurrentImageSizeY) + ":" +
        //     QString::number(outLockX) + ":" + QString::number(outLockY));
    } else {
        // TODO(PHD2): 前端锁星十字隐藏（暂不发送）
        // emit wsThread->sendMessageToClient("PHD2StarCrossView:false");
    }

    // ---------- 导星状态/曲线 ----------
    // 不要用 (dRa/dDec != 0) 判断“是否有新导星数据”：
    // - 误差为 0 是完全合法的（导星很稳时经常出现），此时 RMS/曲线也应持续更新。
    // - shared memory 中 guideDataIndicator 用来标记“本帧有新导星数据”，读取后会被清零。
    if (sharedmemory_phd[kFlagOff] == 0x02 && bitDepth > 0 && currentPHDSizeX > 0 && currentPHDSizeY > 0) {
        unsigned char phdstatu;
        call_phd_checkStatus(phdstatu);

        Logger::Log("ShowPHDdata | dRa:" + std::to_string(dRa) + ", dDec:" + std::to_string(dDec) +
                    ", guideDataIndicator:" + std::to_string((int)guideDataIndicator),
                    LogLevel::DEBUG, DeviceType::GUIDER);

        if (dRa != 0 || dDec != 0) {
            QPointF tmp; tmp.setX(-dRa * PixelRatio); tmp.setY(dDec * PixelRatio);
            glPHD_rmsdate.append(tmp);
            // TODO(PHD2): 前端散点图数据（暂不发送）
            // emit wsThread->sendMessageToClient("AddScatterChartData:" +
            //     QString::number(-dRa * PixelRatio) + ":" + QString::number(-dDec * PixelRatio));

            if (InGuiding) {
                // TODO(PHD2): 前端导星状态（暂不发送）
                // emit wsThread->sendMessageToClient("GuiderStatus:InGuiding");
                // emit wsThread->sendMessageToClient("GuiderUpdateStatus:1");
            } else {
                // TODO(PHD2): 前端导星状态（暂不发送）
                // emit wsThread->sendMessageToClient("GuiderStatus:InCalibration");
                // emit wsThread->sendMessageToClient("GuiderUpdateStatus:1");
            }

            if (StarLostAlert) {
                Logger::Log("ShowPHDdata | send GuiderStatus:StarLostAlert",
                            LogLevel::DEBUG, DeviceType::GUIDER);
                // TODO(PHD2): 前端丢星告警（暂不发送）
                // emit wsThread->sendMessageToClient("GuiderStatus:StarLostAlert");
                // emit wsThread->sendMessageToClient("GuiderUpdateStatus:2");
            }

            // TODO(PHD2): 前端 RMS 曲线（暂不发送）
            // emit wsThread->sendMessageToClient("AddRMSErrorData:" +
            //     QString::number(RMSErrorX, 'f', 3) + ":" + QString::number(RMSErrorX, 'f', 3));
        }

        for (int i = 0; i < glPHD_rmsdate.size(); i++) {
            if (i == glPHD_rmsdate.size() - 1) {
                // TODO(PHD2): 前端折线图数据（暂不发送）
                // emit wsThread->sendMessageToClient("AddLineChartData:" + QString::number(i) + ":" +
                //     QString::number(glPHD_rmsdate[i].x()) + ":" + QString::number(glPHD_rmsdate[i].y()));
                if (i > 50)
                    ; // TODO(PHD2): 前端折线图范围（暂不发送）
                else
                    ; // TODO(PHD2): 前端折线图范围（暂不发送）
            }
        }
    }

    // ===================== 像素数据读取 / 解码 =====================
    const size_t payload_cap_bytes = (total_size > kPayloadOff) ? (total_size - kPayloadOff) : 0;
    if (payload_cap_bytes == 0) { sharedmemory_phd[kFlagOff] = 0x00; return; }

    cv::Mat PHDImg;
    std::unique_ptr<uint8_t[]> buf;

    if (hasV2) {
        Logger::Log("V2 hdr: coding=" + std::to_string(v2.coding) +
                    " out=" + std::to_string(v2.outW) + "x" + std::to_string(v2.outH) +
                    " depth=" + std::to_string(v2.bitDepth) +
                    " payload=" + std::to_string(v2.payloadSize),
                    LogLevel::DEBUG, DeviceType::GUIDER);

        const uint16_t coding   = v2.coding;
        const uint16_t v2Depth  = v2.bitDepth;
        const size_t   bpp      = (v2Depth == 16) ? 2 : 1;
        const uint32_t outW     = v2.outW;
        const uint32_t outH     = v2.outH;
        const size_t   outPix   = (size_t)outW * (size_t)outH;
        const size_t   need     = outPix * bpp;
        const size_t   payLen   = (size_t)v2.payloadSize;

        if (!(v2Depth==8 || v2Depth==16) || outW==0 || outH==0 || payLen > payload_cap_bytes) {
            // 头异常，退回旧逻辑
            hasV2 = false;
        } else {
            const uint8_t* payload = (const uint8_t*)(sharedmemory_phd + kPayloadOff);

            if (coding == CODING_RAW || coding == CODING_NEAREST) {
                if (need > payload_cap_bytes) { sharedmemory_phd[kFlagOff] = 0x00; return; }
                buf.reset(new uint8_t[need]);
                std::memcpy(buf.get(), payload, need);
                if (v2Depth == 16) PHDImg.create(outH, outW, CV_16UC1);
                else               PHDImg.create(outH, outW, CV_8UC1);
                PHDImg.data = buf.get();
            } else if (coding == CODING_RLE) {
                buf.reset(new uint8_t[need]);
                bool ok = (v2Depth==8)
                    ? rle_decompress_8(payload, payLen, buf.get(), outPix)
                    : rle_decompress_16(payload, payLen, (uint16_t*)buf.get(), outPix);
                if (!ok) { sharedmemory_phd[kFlagOff] = 0x00; return; }
                if (v2Depth == 16) PHDImg.create(outH, outW, CV_16UC1);
                else               PHDImg.create(outH, outW, CV_8UC1);
                PHDImg.data = buf.get();
            } else {
                hasV2 = false;
            }
        }
    }

    if (!hasV2) {
        Logger::Log("Legacy path (no V2 header)", LogLevel::DEBUG, DeviceType::GUIDER);
        const size_t need = (size_t)currentPHDSizeX * (size_t)currentPHDSizeY * (bitDepth / 8);
        if (need == 0 || need > payload_cap_bytes) {
            Logger::Log("ShowPHDdata | legacy frame too large or zero", LogLevel::WARNING, DeviceType::GUIDER);
            sharedmemory_phd[kFlagOff] = 0x00;
            return;
        }
        buf.reset(new uint8_t[need]);
        std::memcpy(buf.get(), sharedmemory_phd + kPayloadOff, need);
        if (bitDepth == 16) PHDImg.create(currentPHDSizeY, currentPHDSizeX, CV_16UC1);
        else                PHDImg.create(currentPHDSizeY, currentPHDSizeX, CV_8UC1);
        PHDImg.data = buf.get();
    }

    // 像素完整拷贝/解码完成后再清 2047 标志，避免半帧被清
    sharedmemory_phd[kFlagOff] = 0x00;

    // ===== 你的后处理：拉伸/保存/显示（保持不变）=====
    uint16_t B = 0;
    uint16_t W = (PHDImg.depth() == CV_8U) ? 255 : 65535;  // 根据图像位深度设置默认最大值

    cv::Mat image_raw8;
    image_raw8.create(PHDImg.rows, PHDImg.cols, CV_8UC1);

    if (AutoStretch == true) {
        Tools::GetAutoStretch(PHDImg, 0, B, W);
    } else {
        B = 0;
        W = (PHDImg.depth() == CV_8U) ? 255 : 65535;  // 根据图像位深度设置默认最大值
    }
    Tools::Bit16To8_Stretch(PHDImg, image_raw8, B, W);
    saveGuiderImageAsJPG(image_raw8);

}



#endif // QUARCS_ENABLE_EXTERNAL_PHD2

#if QUARCS_ENABLE_EXTERNAL_PHD2
void MainWindow::getTimeNow(int index)
{
    // 获取当前时间点
    auto now = std::chrono::system_clock::now();

    // 将当前时间点转换为毫秒
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    // 将毫秒时间戳转换为时间类型（std::time_t）
    std::time_t time_now = ms / 1000; // 将毫秒转换为秒

    // 使用 std::strftime 函数将时间格式化为字符串
    char buffer[80];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S",
                  std::localtime(&time_now));

    // 添加毫秒部分
    std::string formatted_time = buffer + std::to_string(ms % 1000);

    // 打印带有当前时间的输出
    // std::cout << "TIME(ms): " << formatted_time << "," << index << std::endl;
}

void MainWindow::onPHDControlGuideTimeout()
{
    // Logger::Log("PHD2 Control Guide is Timeout !", LogLevel::DEBUG, DeviceType::MAIN);
    GetPHD2ControlInstruct();
}

void MainWindow::GetPHD2ControlInstruct()
{
    // Logger::Log("GetPHD2ControlInstruct start ...", LogLevel::DEBUG, DeviceType::MAIN);
    std::lock_guard<std::mutex> lock(receiveMutex);

    unsigned int mem_offset;

    int sdk_direction = 0;
    int sdk_duration = 0;
    int sdk_num = 0;
    int zero = 0;
    mem_offset = 1024;

    mem_offset = mem_offset + sizeof(unsigned int);
    mem_offset = mem_offset + sizeof(unsigned int);
    mem_offset = mem_offset + sizeof(unsigned char);

    int ControlInstruct = 0;

    memcpy(&ControlInstruct, sharedmemory_phd + mem_offset, sizeof(int));
    // Logger::Log("GetPHD2ControlInstruct | get ControlInstruct:" + std::to_string(ControlInstruct), LogLevel::DEBUG, DeviceType::MAIN);
    int mem_offset_sdk_num = mem_offset;
    mem_offset = mem_offset + sizeof(int);

    sdk_num = (ControlInstruct >> 24) & 0xFFF;       // 取前12位
    sdk_direction = (ControlInstruct >> 12) & 0xFFF; // 取中间12位
    sdk_duration = ControlInstruct & 0xFFF;          // 取后12位

    if (sdk_num != 0)
    {
        getTimeNow(sdk_num);
        // Logger::Log("GetPHD2ControlInstruct | PHD2ControlTelescope:" + std::to_string(sdk_num) + "," + std::to_string(sdk_direction) + "," + std::to_string(sdk_duration), LogLevel::DEBUG, DeviceType::MAIN);
    }
    if (sdk_duration != 0)
    {
        MainWindow::ControlGuideEx(sdk_direction, sdk_duration, QStringLiteral("PHD2"));

        memcpy(sharedmemory_phd + mem_offset_sdk_num, &zero, sizeof(int));
        // Logger::Log("GetPHD2ControlInstruct | set ControlInstruct to 0", LogLevel::DEBUG, DeviceType::MAIN);
        call_phd_ChackControlStatus(sdk_num); // set pFrame->ControlStatus = 0;
    }
    // Logger::Log("GetPHD2ControlInstruct finish!", LogLevel::DEBUG, DeviceType::MAIN);
}
#endif

void MainWindow::TelescopeControl_Goto(double Ra, double Dec)
{
    if (dpMount != NULL)
    {
        if (indi_Client->mountState.isTracking)
        {
            indi_Client->slewTelescopeJNowNonBlock(dpMount, Ra, Dec, true);
        }
        else
        {
            indi_Client->slewTelescopeJNowNonBlock(dpMount, Ra, Dec, false);
        }
    }
}

QString MainWindow::TelescopeControl_Status()
{
    if (dpMount != NULL)
    {
        QString Stat;
        indi_Client->getTelescopeStatus(dpMount, Stat);
        return Stat;
    }
}

bool MainWindow::TelescopeControl_Park()
{
    bool isPark = false;
    if (dpMount != NULL)
    {
        indi_Client->getTelescopePark(dpMount, isPark);
        if (isPark == false)
        {
            indi_Client->setTelescopePark(dpMount, true);
        }
        else
        {
            indi_Client->setTelescopePark(dpMount, false);
        }
        indi_Client->getTelescopePark(dpMount, isPark);
        // Logger::Log("TelescopeControl_Park | Telescope is Park ???:" + std::to_string(isPark), LogLevel::INFO, DeviceType::MAIN);
    }

    return isPark;
}

bool MainWindow::TelescopeControl_Track()
{
    bool isTrack = true;
    if (dpMount != NULL)
    {
        indi_Client->getTelescopeTrackEnable(dpMount, isTrack);
        if (isTrack == false)
        {
            indi_Client->setTelescopeTrackEnable(dpMount, true);
        }
        else
        {
            indi_Client->setTelescopeTrackEnable(dpMount, false);
        }
        indi_Client->getTelescopeTrackEnable(dpMount, isTrack);
        Logger::Log("TelescopeControl_Track | Telescope is Track ???:" + std::to_string(isTrack), LogLevel::INFO, DeviceType::MAIN);
    }
    return isTrack;
}
int MainWindow::CaptureImageSave()
{
    Logger::Log("CaptureImageSave...", LogLevel::INFO, DeviceType::MAIN);
    const QString sourcePath = latestMainCaptureFitsPath();

    if (sourcePath.isEmpty())
    {
        emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Null");
        return 1;
    }

    QString CaptureTime = Tools::getFitsCaptureTime(sourcePath.toUtf8().constData());
    Logger::Log("CaptureImageSave | getFitsCaptureTime returned: " + CaptureTime.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    // 如果无法从 FITS 文件获取时间，优先使用文件的修改时间
    if (CaptureTime.isEmpty())
    {
        QFileInfo fileInfo(sourcePath);
        if (fileInfo.exists())
        {
            // 使用文件的最后修改时间
            QDateTime fileTime = fileInfo.lastModified();
            CaptureTime = fileTime.toString("yyyy_MM_dd_HH_mm_ss");
            Logger::Log("CaptureImageSave | Using file modification time as filename: " + CaptureTime.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            // 如果文件不存在（理论上不应该发生），使用当前时间作为最后的fallback
            std::time_t currentTime = std::time(nullptr);
            std::tm *timeInfo = std::localtime(&currentTime);
            char buffer[80];
            std::strftime(buffer, 80, "%Y_%m_%dT%H_%M_%S", timeInfo);
            CaptureTime = QString::fromStdString(buffer);
            Logger::Log("CaptureImageSave | Using current timestamp as filename: " + CaptureTime.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }
    }
    
    CaptureTime.replace(QRegExp("[^a-zA-Z0-9]"), "_");
    QString resultFileName = CaptureTime + ".fits";
    Logger::Log("CaptureImageSave | Generated filename: " + resultFileName.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    std::time_t currentTime = std::time(nullptr);
    std::tm *timeInfo = std::localtime(&currentTime);
    char buffer[80];
    std::strftime(buffer, 80, "%Y-%m-%d", timeInfo); // Format: YYYY-MM-DD

    // 直接使用 ImageSaveBaseDirectory（无论是默认路径还是U盘路径）
    QString destinationDirectory = ImageSaveBaseDirectory + "/CaptureImage";
    QString destinationPath = destinationDirectory + "/" + QString(buffer) + "/" + resultFileName;
    
    // 判断是否为U盘路径（使用saveMode参数）
    bool isUSBSave = (saveMode != "local");
    
    // 使用通用函数检查存储空间并创建目录
    QString dirPathToCreate = isUSBSave ? (destinationDirectory + "/" + QString(buffer)) : QString();
    int checkResult = checkStorageSpaceAndCreateDirectory(
        sourcePath,
        destinationDirectory,
        dirPathToCreate,
        "CaptureImageSave",
        isUSBSave,
        [this]() { createCaptureDirectory(); }
    );
    if (checkResult != 0)
    {
        return checkResult;
    }

    // 检查文件是否已存在
    if (QFile::exists(destinationPath))
    {
        Logger::Log("The file already exists, there is no need to save it again:" + destinationPath.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Repeat");
        return 0;
    }

    // 使用通用函数保存文件
    int saveResult = saveImageFile(sourcePath, destinationPath, "CaptureImageSave", isUSBSave);
    if (saveResult != 0)
    {
        return saveResult;
    }

    emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Success");
    Logger::Log("CaptureImageSave | File saved successfully: " + destinationPath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    return 0;
}
int MainWindow::solveFailedImageSave(const QString& imagePath)
{
    // Logger::Log("solveFailedImageSave...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("solveFailedImageSave | Starting save process, imagePath: " + imagePath.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    // 如果未提供路径，使用默认路径
    QString sourcePathStr = imagePath.isEmpty() ? "/dev/shm/ccd_simulator.fits" : imagePath;
    const char *sourcePath = sourcePathStr.toLocal8Bit().constData();

    Logger::Log("solveFailedImageSave | Using source path: " + sourcePathStr.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    
    if (!QFile::exists(sourcePathStr))
    {
        Logger::Log("solveFailedImageSave | 文件不存在: " + sourcePathStr.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Null");
        return 1;
    }
    
    Logger::Log("solveFailedImageSave | Source file exists, file size: " + std::to_string(QFileInfo(sourcePathStr).size()) + " bytes", LogLevel::INFO, DeviceType::MAIN);

    QString CaptureTime = Tools::getFitsCaptureTime(sourcePath);
    Logger::Log("solveFailedImageSave | getFitsCaptureTime returned: " + CaptureTime.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    
    // 如果无法从 FITS 文件获取时间，优先使用文件的修改时间
    if (CaptureTime.isEmpty())
    {
        QFileInfo fileInfo(sourcePathStr);
        if (fileInfo.exists())
        {
            // 使用文件的最后修改时间
            QDateTime fileTime = fileInfo.lastModified();
            CaptureTime = fileTime.toString("yyyy_MM_dd_HH_mm_ss");
            Logger::Log("solveFailedImageSave | Using file modification time as filename: " + CaptureTime.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            // 如果文件不存在（理论上不应该发生），使用当前时间作为最后的fallback
            std::time_t currentTime = std::time(nullptr);
            std::tm *timeInfo = std::localtime(&currentTime);
            char buffer[80];
            std::strftime(buffer, 80, "%Y_%m_%dT%H_%M_%S", timeInfo);
            CaptureTime = QString::fromStdString(buffer);
            Logger::Log("solveFailedImageSave | Using current timestamp as filename: " + CaptureTime.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }
    }
    
    CaptureTime.replace(QRegExp("[^a-zA-Z0-9]"), "_");
    QString resultFileName = CaptureTime + ".fits";
    Logger::Log("solveFailedImageSave | Generated filename: " + resultFileName.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    std::time_t currentTime = std::time(nullptr);
    std::tm *timeInfo = std::localtime(&currentTime);
    char buffer[80];
    std::strftime(buffer, 80, "%Y-%m-%d", timeInfo); // Format: YYYY-MM-DD

    // 指定目标目录
    QString destinationDirectory = ImageSaveBaseDirectory + "/solveFailedImage";

    QString destinationPath = destinationDirectory + "/" + buffer + "/" + resultFileName;
    
    // 判断是否为U盘路径（使用saveMode参数）
    bool isUSBSave = (saveMode != "local");
    
    // 使用通用函数检查存储空间并创建目录
    // 注意：传入 QString 而不是 const char*，确保路径正确传递
    QString dirPathToCreate = isUSBSave ? (destinationDirectory + "/" + QString(buffer)) : QString();
    
    // 在调用前再次确认文件存在（因为文件可能在检查后被删除）
    if (!QFile::exists(sourcePathStr))
    {
        Logger::Log("solveFailedImageSave | Source file no longer exists before checkStorageSpaceAndCreateDirectory: " + sourcePathStr.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Null");
        return 1;
    }
    
    int checkResult = checkStorageSpaceAndCreateDirectory(
        sourcePathStr,  // 使用 QString 而不是 const char*
        destinationDirectory,
        dirPathToCreate,
        "solveFailedImageSave",
        isUSBSave,
        [this]() { createsolveFailedImageDirectory(); }
    );
    if (checkResult != 0)
    {
        Logger::Log("solveFailedImageSave | checkStorageSpaceAndCreateDirectory failed with code: " + std::to_string(checkResult), LogLevel::ERROR, DeviceType::MAIN);
        return checkResult;
    }

    // 检查文件是否已存在
    // if (QFile::exists(destinationPath))
    // {
    //     qWarning() << "The file already exists, there is no need to save it again:" << destinationPath;
    //     emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Repeat");
    //     return 0;
    // }

    // 使用通用函数保存文件
    // 在保存前再次确认源文件存在
    if (!QFile::exists(sourcePathStr))
    {
        Logger::Log("solveFailedImageSave | Source file no longer exists before saveImageFile: " + sourcePathStr.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Null");
        return 1;
    }
    
    Logger::Log("solveFailedImageSave | Attempting to save to: " + destinationPath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    int saveResult = saveImageFile(sourcePathStr, destinationPath, "solveFailedImageSave", isUSBSave);  // 使用 QString 而不是 const char*
    if (saveResult != 0)
    {
        Logger::Log("solveFailedImageSave | saveImageFile failed with error code: " + std::to_string(saveResult), LogLevel::ERROR, DeviceType::MAIN);
        return saveResult;
    }

    // 验证文件是否真的被保存了
    if (QFile::exists(destinationPath))
    {
        Logger::Log("solveFailedImageSave | File saved successfully to: " + destinationPath.toStdString() + ", size: " + std::to_string(QFileInfo(destinationPath).size()) + " bytes", LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        Logger::Log("solveFailedImageSave | WARNING: saveImageFile returned success but destination file does not exist: " + destinationPath.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
    }

    emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Success");
    qDebug() << "CaptureImageSaveStatus Goto Complete...";
    return 0;
}

bool MainWindow::directoryExists(const std::string &path)
{
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool MainWindow::createCaptureDirectory()
{
    Logger::Log("createCaptureDirectory start ...", LogLevel::INFO, DeviceType::MAIN);
    std::string basePath = ImageSaveBasePath + "/CaptureImage/";

    std::time_t currentTime = std::time(nullptr);
    std::tm *timeInfo = std::localtime(&currentTime);
    char buffer[80];
    std::strftime(buffer, 80, "%Y-%m-%d", timeInfo); // Format: YYYY-MM-DD
    std::string folderName = basePath + buffer;

    // 如果目录不存在，则创建
    if (!std::filesystem::exists(folderName))
    {
        if (std::filesystem::create_directory(folderName))
        {
            Logger::Log("createCaptureDirectory | Folder created successfully: " + std::string(folderName), LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            Logger::Log("createCaptureDirectory | An error occurred while creating the folder.", LogLevel::INFO, DeviceType::MAIN);
        }
    }
    else
    {
        Logger::Log("createCaptureDirectory | The folder already exists: " + std::string(folderName), LogLevel::INFO, DeviceType::MAIN);
    }
    return true;
}
bool MainWindow::createsolveFailedImageDirectory()
{
    Logger::Log("createCaptureDirectory start ...", LogLevel::INFO, DeviceType::MAIN);
    std::string basePath = ImageSaveBasePath + "/solveFailedImage/";

    std::time_t currentTime = std::time(nullptr);
    std::tm *timeInfo = std::localtime(&currentTime);
    char buffer[80];
    std::strftime(buffer, 80, "%Y-%m-%d", timeInfo); // Format: YYYY-MM-DD
    std::string folderName = basePath + buffer;

    // 如果目录不存在，则创建
    if (!std::filesystem::exists(folderName))
    {
        if (std::filesystem::create_directory(folderName))
        {
            Logger::Log("createCaptureDirectory | Folder created successfully: " + std::string(folderName), LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            Logger::Log("createCaptureDirectory | An error occurred while creating the folder.", LogLevel::INFO, DeviceType::MAIN);
        }
    }
    else
    {
        Logger::Log("createCaptureDirectory | The folder already exists: " + std::string(folderName), LogLevel::INFO, DeviceType::MAIN);
    }
    return true;
}

int MainWindow::checkStorageSpaceAndCreateDirectory(const QString &sourcePath, 
                                                     const QString &destinationDirectory,
                                                     const QString &dirPathToCreate,
                                                     const QString &functionName,
                                                     bool isUSBSave,
                                                     std::function<void()> createLocalDirectoryFunc)
{
    Logger::Log(functionName.toStdString() + " | checkStorageSpaceAndCreateDirectory | saveMode: " + saveMode.toStdString() + 
               ", isUSBSave: " + std::string(isUSBSave ? "true" : "false") + 
               ", ImageSaveBaseDirectory: " + ImageSaveBaseDirectory.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    
    // 先获取源文件大小（在空间检查之前）
    QFileInfo sourceFileInfo(sourcePath);
    if (!sourceFileInfo.exists())
    {
        Logger::Log(functionName.toStdString() + " | Source file does not exist.", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
        return 1;
    }
    long long fileSize = sourceFileInfo.size();
    
    if (isUSBSave)
    {
        // 从ImageSaveBaseDirectory提取U盘挂载点（去掉/QUARCS_ImageSave）
        QString usb_mount_point = ImageSaveBaseDirectory;
        usb_mount_point.replace("/QUARCS_ImageSave", "");
        
        Logger::Log(functionName.toStdString() + " | USB save mode | ImageSaveBaseDirectory: " + ImageSaveBaseDirectory.toStdString() + 
                   ", extracted USB mount point: " + usb_mount_point.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        
        // 检查U盘空间和可写性
        QStorageInfo storageInfo(usb_mount_point);
        if (!storageInfo.isValid() || !storageInfo.isReady())
        {
            Logger::Log(functionName.toStdString() + " | USB drive is not valid or not ready: " + usb_mount_point.toStdString() + 
                       " (isValid: " + std::string(storageInfo.isValid() ? "true" : "false") + 
                       ", isReady: " + std::string(storageInfo.isReady() ? "true" : "false") + ")", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:USB-NotAvailable");
            return 1;
        }
        
        if (storageInfo.isReadOnly())
        {
            const QString password = "quarcs";
            if (!remountReadWrite(usb_mount_point, password))
            {
                Logger::Log(functionName.toStdString() + " | Failed to remount USB as read-write.", LogLevel::WARNING, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("CaptureImageSaveStatus:USB-ReadOnly");
                return 1;
            }
        }
        
        // 检查U盘剩余空间（在创建目录之前）
        long long remaining_space = getUSBSpace(usb_mount_point);
        if (remaining_space == -1 || remaining_space <= 0)
        {
            Logger::Log(functionName.toStdString() + " | USB drive has no available space.", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:USB-NoSpace");
            return 1;
        }
        
        // 预留至少100MB的缓冲空间，避免写入时空间不足
        const long long RESERVE_SPACE = 100 * 1024 * 1024; // 100MB
        long long available_space = remaining_space - RESERVE_SPACE;
        if (available_space < 0)
        {
            available_space = 0;
        }
        
        // 检查空间是否足够（文件大小必须小于可用空间，已预留缓冲）
        if (fileSize > available_space)
        {
            Logger::Log(functionName.toStdString() + " | Insufficient USB space. Required: " + QString::number(fileSize).toStdString() + 
                       " bytes, Available: " + QString::number(remaining_space).toStdString() + 
                       " bytes (reserved: " + QString::number(RESERVE_SPACE).toStdString() + " bytes)", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:USB-NoSpace");
            return 1;
        }
        
        // 创建目录（使用sudo）- 在空间检查通过后
        // 安全检查：避免在 /media/quarcs 路径下创建任何文件夹，避免被错误识别为U盘
        QString normalizedPath = QDir(dirPathToCreate).absolutePath();
        
        // 检查路径是否在 /media/quarcs 下
        if (normalizedPath.startsWith("/media/quarcs/"))
        {
            // 提取 /media/quarcs/ 之后的部分
            QString pathAfterMedia = normalizedPath.mid(14); // 去掉 "/media/quarcs/"
            
            // 检查路径格式：应该是 /media/quarcs/某个U盘名/...
            int firstSlash = pathAfterMedia.indexOf('/');
            if (firstSlash > 0)
            {
                QString usbName = pathAfterMedia.left(firstSlash);
                // 检查这个U盘名是否在映射表中（有效的U盘挂载点）
                if (!usbMountPointsMap.contains(usbName))
                {
                    Logger::Log(functionName.toStdString() + " | Security check failed: Attempting to create directory in /media/quarcs/ but USB name '" + usbName.toStdString() + "' not found in mount points map. Path: " + dirPathToCreate.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
                    emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
                    return 1;
                }
                // 验证路径确实在U盘挂载点下
                QString expectedMountPoint = "/media/quarcs/" + usbName;
                if (!normalizedPath.startsWith(expectedMountPoint))
                {
                    Logger::Log(functionName.toStdString() + " | Security check failed: Path does not match expected mount point. Path: " + dirPathToCreate.toStdString() + ", Expected mount point: " + expectedMountPoint.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
                    emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
                    return 1;
                }
            }
            else
            {
                // 路径格式不正确，可能是直接在 /media/quarcs/ 下创建文件夹
                Logger::Log(functionName.toStdString() + " | Security check failed: Invalid path format in /media/quarcs/. Path: " + dirPathToCreate.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
                return 1;
            }
        }
        // 额外检查：确保路径不是直接在 /media/quarcs 下（没有子目录）
        else if (normalizedPath == "/media/quarcs")
        {
            Logger::Log(functionName.toStdString() + " | Security check failed: Attempting to create directory directly at /media/quarcs. Path: " + dirPathToCreate.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
            return 1;
        }
        
        const QString password = "quarcs";
        QProcess mkdirProcess;
        mkdirProcess.start("sudo", {"-S", "mkdir", "-p", dirPathToCreate});
        if (!mkdirProcess.waitForStarted() || !mkdirProcess.write((password + "\n").toUtf8()))
        {
            Logger::Log(functionName.toStdString() + " | Failed to create directory: " + dirPathToCreate.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
            return 1;
        }
        mkdirProcess.closeWriteChannel();
        mkdirProcess.waitForFinished(-1);
    }
    else
    {
        // 默认位置：先检查空间（在创建目录之前）
        QString localPath = QString::fromStdString(ImageSaveBasePath);
        Logger::Log(functionName.toStdString() + " | Local save mode | checking local path: " + localPath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        long long remaining_space = getUSBSpace(localPath);
        if (remaining_space == -1 || remaining_space <= 0)
        {
            Logger::Log(functionName.toStdString() + " | Local storage has no available space.", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:NoSpace");
            return 1;
        }
        
        // 预留至少100MB的缓冲空间，避免写入时空间不足
        const long long RESERVE_SPACE = 100 * 1024 * 1024; // 100MB
        long long available_space = remaining_space - RESERVE_SPACE;
        if (available_space < 0)
        {
            available_space = 0;
        }
        
        // 检查空间是否足够（文件大小必须小于可用空间，已预留缓冲）
        if (fileSize > available_space)
        {
            Logger::Log(functionName.toStdString() + " | Insufficient local storage space. Required: " + QString::number(fileSize).toStdString() + 
                       " bytes, Available: " + QString::number(remaining_space).toStdString() + 
                       " bytes (reserved: " + QString::number(RESERVE_SPACE).toStdString() + " bytes)", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:NoSpace");
            return 1;
        }
        
        // 创建目录 - 在空间检查通过后
        if (createLocalDirectoryFunc)
        {
            createLocalDirectoryFunc();
        }
    }
    
    return 0;
}

int MainWindow::saveImageFile(const QString &sourcePath, 
                              const QString &destinationPath,
                              const QString &functionName,
                              bool isUSBSave)
{
    if (isUSBSave)
    {
        // U盘保存使用sudo cp命令
        const QString password = "quarcs";
        QProcess cpProcess;
        cpProcess.start("sudo", {"-S", "cp", sourcePath, destinationPath});
        if (!cpProcess.waitForStarted() || !cpProcess.write((password + "\n").toUtf8()))
        {
            Logger::Log(functionName.toStdString() + " | Failed to execute sudo cp command.", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
            return 1;
        }
        cpProcess.closeWriteChannel();
        cpProcess.waitForFinished(-1);
        
        if (cpProcess.exitCode() != 0)
        {
            QByteArray stderrOutput = cpProcess.readAllStandardError();
            Logger::Log(functionName.toStdString() + " | Failed to copy file to USB: " + QString::fromUtf8(stderrOutput).toStdString(), LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
            return 1;
        }
        
        Logger::Log(functionName.toStdString() + " | File saved to USB: " + destinationPath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        // 默认位置保存使用普通文件操作
        // 将相对路径转换为绝对路径
        QString absoluteDestinationPath = destinationPath;
        if (!QDir::isAbsolutePath(destinationPath))
        {
            absoluteDestinationPath = QDir::currentPath() + "/" + destinationPath;
            Logger::Log(functionName.toStdString() + " | Converted relative path to absolute: " + destinationPath.toStdString() + " -> " + absoluteDestinationPath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }
        const QByteArray destinationPathBytes = absoluteDestinationPath.toUtf8();
        const char *destinationPathChar = destinationPathBytes.constData();
        const QByteArray sourcePathBytes = sourcePath.toUtf8();
        const char *sourcePathChar = sourcePathBytes.constData();

        // 确保目标目录存在
        std::filesystem::path destPath(destinationPathChar);
        std::filesystem::path destDir = destPath.parent_path();
        if (!destDir.empty())
        {
            if (!std::filesystem::exists(destDir))
            {
                try {
                    if (!std::filesystem::create_directories(destDir))
                    {
                        Logger::Log(functionName.toStdString() + " | Failed to create destination directory: " + destDir.string(), LogLevel::ERROR, DeviceType::MAIN);
                        emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
                        return 1;
                    }
                    Logger::Log(functionName.toStdString() + " | Created destination directory: " + destDir.string(), LogLevel::INFO, DeviceType::MAIN);
                } catch (const std::filesystem::filesystem_error& e) {
                    Logger::Log(functionName.toStdString() + " | Exception creating directory: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
                    emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
                    return 1;
                }
            }
            else
            {
                // 检查目录是否可写
                try {
                    std::filesystem::perms dirPerms = std::filesystem::status(destDir).permissions();
                    if ((dirPerms & std::filesystem::perms::owner_write) == std::filesystem::perms::none)
                    {
                        Logger::Log(functionName.toStdString() + " | Destination directory is not writable: " + destDir.string(), LogLevel::ERROR, DeviceType::MAIN);
                        emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
                        return 1;
                    }
                } catch (const std::filesystem::filesystem_error& e) {
                    Logger::Log(functionName.toStdString() + " | Exception checking directory permissions: " + std::string(e.what()), LogLevel::WARNING, DeviceType::MAIN);
                }
            }
        }

        // 检查目标文件是否已存在（处理竞态条件）
        if (std::filesystem::exists(destPath))
        {
            Logger::Log(functionName.toStdString() + " | Target file already exists, attempting to remove: " + std::string(destinationPathChar), LogLevel::WARNING, DeviceType::MAIN);
            try {
                if (!std::filesystem::remove(destPath))
                {
                    Logger::Log(functionName.toStdString() + " | Failed to remove existing file: " + std::string(destinationPathChar), LogLevel::ERROR, DeviceType::MAIN);
                    emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
                    return 1;
                }
                Logger::Log(functionName.toStdString() + " | Removed existing file: " + std::string(destinationPathChar), LogLevel::INFO, DeviceType::MAIN);
            } catch (const std::filesystem::filesystem_error& e) {
                Logger::Log(functionName.toStdString() + " | Exception removing existing file: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
                return 1;
            }
        }

        std::ifstream sourceFile(sourcePathChar, std::ios::binary);
        if (!sourceFile.is_open())
        {
            Logger::Log(functionName.toStdString() + " | Unable to open source file: " + sourcePath.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
            return 1;
        }

        std::ofstream destinationFile(destinationPathChar, std::ios::binary | std::ios::trunc);
        if (!destinationFile.is_open())
        {
            std::string dirInfo = "unknown";
            try {
                if (std::filesystem::exists(destDir))
                {
                    bool writable = (std::filesystem::status(destDir).permissions() & std::filesystem::perms::owner_write) != std::filesystem::perms::none;
                    dirInfo = "exists: yes, writable: " + std::string(writable ? "yes" : "no");
                }
                else
                {
                    dirInfo = "exists: no";
                }
            } catch (...) {
                dirInfo = "exists: unknown (exception)";
            }
            Logger::Log(functionName.toStdString() + " | Unable to create or open target file: " + std::string(destinationPathChar) + 
                       " | " + dirInfo, 
                       LogLevel::ERROR, DeviceType::MAIN);
            sourceFile.close();
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
            return 1;
        }

        destinationFile << sourceFile.rdbuf();

        sourceFile.close();
        destinationFile.close();
        
        // 验证文件是否成功写入
        if (!std::filesystem::exists(destPath))
        {
            Logger::Log(functionName.toStdString() + " | File write completed but file does not exist: " + std::string(destinationPathChar), LogLevel::ERROR, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Failed");
            return 1;
        }
        
        Logger::Log(functionName.toStdString() + " | File saved successfully to: " + std::string(destinationPathChar) + 
                   " | File size: " + std::to_string(std::filesystem::file_size(destPath)) + " bytes", 
                   LogLevel::INFO, DeviceType::MAIN);
    }
    
    return 0;
}
void MainWindow::getClientSettings()
{

    std::string fileName = "config/config.ini";

    std::unordered_map<std::string, std::string> config;

    Tools::readClientSettings(fileName, config);

    const auto itMain = config.find("MainCameraFocalLength");
    const auto itLegacy = config.find("FocalLength");
    if (itMain == config.end() && itLegacy != config.end())
    {
        const QString legacyValue = QString::fromStdString(itLegacy->second).trimmed();
        if (!legacyValue.isEmpty())
        {
            setClientSettings("MainCameraFocalLength", legacyValue);
            config["MainCameraFocalLength"] = legacyValue.toStdString();
            Logger::Log("getClientSettings | migrated FocalLength -> MainCameraFocalLength = " +
                            legacyValue.toStdString(),
                        LogLevel::INFO, DeviceType::MAIN);
        }
    }

    Logger::Log("getClientSettings | Current Config:", LogLevel::INFO, DeviceType::MAIN);
    for (const auto &pair : config)
    {
        Logger::Log("getClientSettings | " + pair.first + " = " + pair.second, LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConfigureRecovery:" + QString::fromStdString(pair.first) + ":" + QString::fromStdString(pair.second));

        if (pair.first == "Coordinates")
        {
            QStringList coordinates = QString::fromStdString(pair.second).split(",");
            if (coordinates.size() >= 2)
            {
                observatorylongitude = coordinates[1].toDouble();
                observatorylatitude = coordinates[0].toDouble();
            }
        }

    }
    Logger::Log("getClientSettings finish!", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::setClientSettings(QString ConfigName, QString ConfigValue)
{

    Logger::Log("setClientSettings start ...", LogLevel::INFO, DeviceType::MAIN);
    std::string fileName = "config/config.ini";

    std::unordered_map<std::string, std::string> config;

    config[ConfigName.toStdString()] = ConfigValue.toStdString();

    Logger::Log("setClientSettings | Save Client Setting:" + ConfigName.toStdString() + " = " + ConfigValue.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    Tools::saveClientSettings(fileName, config);
    if (ConfigName == "FocalLength" || ConfigName == "MainCameraFocalLength")
    {
        glFocalLength = ConfigValue.toDouble();
        if (ConfigName == "FocalLength")
        {
            std::unordered_map<std::string, std::string> migrateConfig;
            migrateConfig["MainCameraFocalLength"] = ConfigValue.toStdString();
            Tools::saveClientSettings(fileName, migrateConfig);
        }
    }
    if (ConfigName == "GuiderFocalLength")
    {
        guiderFocalLengthMm = ConfigValue.toDouble();
    }
    if (ConfigName == "GuiderExposureMs")
    {
        bool ok = false;
        const int exposureMs = ConfigValue.trimmed().toInt(&ok);
        if (ok && exposureMs > 0)
        {
            guiderExpMs = exposureMs;
            auto p = guiderParamsCache;
            p.exposureMs = exposureMs;
            guiderParamsCache = p;
            postGuiderCore(guiderCore, [p](GuiderCore *core) { core->setParams(p); });
        }
    }
    Logger::Log("setClientSettings finish!", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::getConnectedDevices()
{
    Logger::Log("getConnectedDevices start ...", LogLevel::INFO, DeviceType::MAIN);
    QString deviceType;
    bool isConnect;
    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        deviceType = systemdevicelist.system_devices[i].Description;
        isConnect = systemdevicelist.system_devices[i].isConnect;
        if (deviceType != "" && isConnect)
        {
            emit wsThread->sendMessageToClient("AddDeviceType:" + deviceType);
        }
    }

    for (int i = 0; i < indi_Client->GetDeviceCount(); i++)
    {
        if (indi_Client->GetDeviceFromList(i)->isConnected())
        {
            emit wsThread->sendMessageToClient("DeviceToBeAllocated:Device:" + QString::number(i) + ":" + QString::fromUtf8(indi_Client->GetDeviceFromList(i)->getDeviceName()));
        }
    }

    for (int i = 0; i < ConnectedDevices.size(); i++)
    {
        Logger::Log("getConnectedDevices | Device[" + std::to_string(i) + "]: " + ConnectedDevices[i].DeviceName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ConnectSuccess:" + ConnectedDevices[i].DeviceType + ":" + ConnectedDevices[i].DeviceName);

        if (ConnectedDevices[i].DeviceType == "MainCamera" && isMainCameraConnected())
        {
            emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));
            emit wsThread->sendMessageToClient("MainCameraOffsetRange:" + QString::number(glOffsetMin) + ":" + QString::number(glOffsetMax) + ":" + QString::number(glOffsetValue));
            emit wsThread->sendMessageToClient("MainCameraGainRange:" + QString::number(glGainMin) + ":" + QString::number(glGainMax) + ":" + QString::number(glGainValue));
            if (glUsbTrafficMax > glUsbTrafficMin)
            {
                emit wsThread->sendMessageToClient("MainCameraUsbTrafficRange:" + QString::number(glUsbTrafficMin) + ":" + QString::number(glUsbTrafficMax) + ":" + QString::number(glUsbTrafficValue) + ":" + QString::number(glUsbTrafficStep));
            }

            QString CFWname;
            indi_Client->getCFWSlotName(dpMainCamera, CFWname);
            if (CFWname != "")
            {
                Logger::Log("getConnectedDevices | get CFW Slot Name: " + CFWname.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("ConnectSuccess:CFW:" + CFWname + " (on camera)");
                isFilterOnCamera = true;

                int min, max, pos;
                indi_Client->getCFWPosition(dpMainCamera, pos, min, max);
                Logger::Log("getConnectedDevices | getCFWPosition: " + std::to_string(min) + ", " + std::to_string(max) + ", " + std::to_string(pos), LogLevel::INFO, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("CFWPositionMax:" + QString::number(max));
            }
        }
        else if (ConnectedDevices[i].DeviceType == "Guider" && (dpGuider != nullptr || sdkGuiderHandle != nullptr))
        {
            emit wsThread->sendMessageToClient("GuiderOffsetRange:" + QString::number(glGuiderOffsetMin) + ":" + QString::number(glGuiderOffsetMax) + ":" + QString::number(glGuiderOffsetValue));
            emit wsThread->sendMessageToClient("GuiderGainRange:" + QString::number(glGuiderGainMin) + ":" + QString::number(glGuiderGainMax) + ":" + QString::number(glGuiderGainValue));
        }
    }
    Logger::Log("getConnectedDevices finish!", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::clearConnectedDevices()
{
    ConnectedDevices.clear();
}



bool MainWindow::isFileExists(const QString &filePath)
{
    QFile file(filePath);
    if (file.exists())
    {
        Logger::Log("isFileExists | file exists: " + filePath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        Logger::Log("isFileExists | file does not exist: " + filePath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    }
    return file.exists();
}

void MainWindow::getStagingScheduleData()
{
    if (isStagingScheduleData)
    {
        emit wsThread->sendMessageToClient(StagingScheduleData);
    }

    // 将当前调度列表中的进度同步给前端，便于页面刷新后恢复每一行的执行进度
    for (int i = 0; i < m_scheduList.size(); ++i)
    {
        int progress = m_scheduList[i].progress;
        if (progress < 0)
        {
            progress = 0;
        }
        else if (progress > 100)
        {
            progress = 100;
        }

        // 仅对已有进度的行进行同步，避免干扰尚未使用的默认行
        if (progress > 0)
        {
            emit wsThread->sendMessageToClient(
                "UpdateScheduleProcess:" +
                QString::number(i) + ":" +
                QString::number(progress));
        }
    }

    // 无论是否有暂存数据，都向前端同步当前计划运行状态
    emit wsThread->sendMessageToClient(
        QString("ScheduleRunning:%1").arg(isScheduleRunning ? "true" : "false"));
}

void MainWindow::getStagingGuiderData()
{
    // TODO(PHD2): 前端导星曲线/散点数据同步已暂停；若恢复前端曲线显示，再按协议重发 glPHD_rmsdate
#if 0
    int dataSize = glPHD_rmsdate.size();
    int startIdx = dataSize > 50 ? dataSize - 50 : 0;

    for (int i = startIdx; i < dataSize; i++)
    {
        emit wsThread->sendMessageToClient("AddLineChartData:" + QString::number(i) + ":" + QString::number(glPHD_rmsdate[i].x()) + ":" + QString::number(glPHD_rmsdate[i].y()));
        emit wsThread->sendMessageToClient("AddScatterChartData:" + QString::number(glPHD_rmsdate[i].x()) + ":" + QString::number(-glPHD_rmsdate[i].y()));
        if (i > 50)
        {
            emit wsThread->sendMessageToClient("SetLineChartRange:" + QString::number(i - 50) + ":" + QString::number(i));
        }
    }
#endif
}

int MainWindow::MoveFileToUSB()
{
    qDebug("MoveFileToUSB");
}

void MainWindow::solveCurrentPosition()
{
    if (solveCurrentPositionTimer.isActive())
    {
        Logger::Log("solveCurrentPosition | SolveCurrentPosition is already running...", LogLevel::INFO, DeviceType::MAIN);
        return;
    }
    // 停止之前的定时器
    solveCurrentPositionTimer.stop();
    disconnect(&solveCurrentPositionTimer, &QTimer::timeout, nullptr, nullptr);
    // 判断解析图像路径下是否有图片
    if (isFileExists(QString::fromStdString(SolveImageFileName.toStdString())))
    {
        // 设置定时器为单次触发
        solveCurrentPositionTimer.setSingleShot(true);
        // 开始解析图像
        Tools::PlateSolve(SolveImageFileName, glFocalLength, glCameraSize_width, glCameraSize_height, false);
        // 连接解析完成信号到处理函数，处理解析完成后的逻辑
        connect(&solveCurrentPositionTimer, &QTimer::timeout, [this]()
        {
            if (Tools::isSolveImageFinish())  // 检查图像解析是否完成
            {
                SloveResults result = Tools::ReadSolveResult(SolveImageFileName, glMainCCDSizeX, glMainCCDSizeY);  // 读取解析结果
                if (result.RA_Degree == -1 && result.DEC_Degree == -1)
                {
                    Logger::Log("solveCurrentPosition | Solve image failed...", LogLevel::INFO, DeviceType::MAIN);
                    emit wsThread->sendMessageToClient("SolveCurrentPosition:failed");
                    solveCurrentPositionTimer.stop();
                    disconnect(&solveCurrentPositionTimer, &QTimer::timeout, nullptr, nullptr);
                    return;
                }
                else
                {
                    emit wsThread->sendMessageToClient("SolveCurrentPosition:succeeded:" + QString::number(result.RA_Degree) + ":" + QString::number(result.DEC_Degree)+":"+QString::number(result.RA_0)+":"+QString::number(result.DEC_0)+":"+QString::number(result.RA_1)+":"+QString::number(result.DEC_1)+":"+QString::number(result.RA_2)+":"+QString::number(result.DEC_2)+":"+QString::number(result.RA_3)+":"+QString::number(result.DEC_3));
                    solveCurrentPositionTimer.stop();
                    disconnect(&solveCurrentPositionTimer, &QTimer::timeout, nullptr, nullptr);
                    return;
                }
            }
            else
            {
                solveCurrentPositionTimer.start(1000);
            }
        });
        solveCurrentPositionTimer.start(1000);
    }
    else
    {
        Logger::Log("solveCurrentPosition | SolveImageFileName: " + SolveImageFileName.toStdString() + " does not exist.", LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("SolveCurrentPosition:failed");
        solveCurrentPositionTimer.stop();
        return;
    }
}

void MainWindow::TelescopeControl_SolveSYNC()
{
    // 在函数开始时断开之前的连接
    disconnect(&captureTimer, &QTimer::timeout, nullptr, nullptr);
    disconnect(&solveTimer, &QTimer::timeout, nullptr, nullptr);

    // 停止之前的定时器
    captureTimer.stop();
    solveTimer.stop();

    if (!isMainCameraConnected())
    {
        emit wsThread->sendMessageToClient("MainCameraNotConnect");
        return;
    }
    Logger::Log("TelescopeControl_SolveSYNC start ...", LogLevel::INFO, DeviceType::MAIN);
    if (glMainCameraStatu == "Exposuring" || isFocusLoopShooting == true)
    {
        Logger::Log("TelescopeControl_SolveSYNC | Camera is not idle.glMainCameraStatu:" + glMainCameraStatu.toStdString() + ", isFocusLoopShooting:" + std::to_string(isFocusLoopShooting), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("CameraNotIdle");
        return;
    }

    double Ra_Hour;
    double Dec_Degree;

    if (dpMount != NULL)
    {
        indi_Client->getTelescopeRADECJNOW(dpMount, Ra_Hour, Dec_Degree); // 获取当前望远镜的赤经和赤纬
    }
    else
    {
        Logger::Log("TelescopeControl_SolveSYNC | No Mount Connect.", LogLevel::INFO, DeviceType::MAIN);
        return; // 如果望远镜未连接，记录日志并退出
    }
    isSolveSYNC = true;
    double Ra_Degree = Tools::HourToDegree(Ra_Hour); // 将赤经从小时转换为度

    Logger::Log("TelescopeControl_SolveSYNC | CurrentRa(Degree):" + std::to_string(Ra_Degree) + "," + "CurrentDec(Degree):" + std::to_string(Dec_Degree), LogLevel::INFO, DeviceType::MAIN);
    isSavePngSuccess = false;
    startMainCameraCapture(1000); // 拍摄1秒曝光进行解析同步

    captureTimer.setSingleShot(true);

    // 连接拍摄定时器的超时信号到处理函数，处理拍摄完成后的逻辑
    connect(&captureTimer, &QTimer::timeout, [this](){
        // 如果需要中止拍摄和解算，则执行中止操作并返回
        if (EndCaptureAndSolve)
        {
            EndCaptureAndSolve = false;
            abortMainCameraCapture();
            Logger::Log("TelescopeControl_SolveSYNC | End Capture And Solve!!!", LogLevel::INFO, DeviceType::MAIN);
            isSolveSYNC = false;
            emit wsThread->sendMessageToClient("SolveImagefailed"); 
            return;
        }
        Logger::Log("TelescopeControl_SolveSYNC | WaitForShootToComplete ..." , LogLevel::INFO, DeviceType::MAIN);
  
        // 检查拍摄是否完成
        if (isSavePngSuccess) 
        {
            // 停止拍摄定时器，表示拍摄任务完成
            captureTimer.stop();

            // 开始进行图像解析
            Tools::PlateSolve(SolveImageFileName, glFocalLength, glCameraSize_width, glCameraSize_height, false);

            solveTimer.setSingleShot(true);  // 设置解析定时器为单次触发

            connect(&solveTimer, &QTimer::timeout, [this]()
            {
                // 检查解析进程是否已结束
                bool solveProcessFinished = !Tools::isPlateSolveInProgress();
                
                if (Tools::isSolveImageFinish())  // 检查图像解析是否成功完成
                {
                    SloveResults result = Tools::ReadSolveResult(SolveImageFileName, glMainCCDSizeX, glMainCCDSizeY);  // 读取解析结果
                    Logger::Log("TelescopeControl_SolveSYNC | Plate Solve Result(RA_Degree, DEC_Degree):" + std::to_string(result.RA_Degree) + ", " + std::to_string(result.DEC_Degree), LogLevel::INFO, DeviceType::MAIN);

                    if (result.RA_Degree == -1 && result.DEC_Degree == -1)
                    {
                        Logger::Log("TelescopeControl_SolveSYNC | Solve image failed...", LogLevel::INFO, DeviceType::MAIN);
                        Logger::Log("TelescopeControl_SolveSYNC | SolveImageFileName: " + SolveImageFileName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                        Logger::Log("TelescopeControl_SolveSYNC | mainCameraSaveFailedParse: " + std::string(mainCameraSaveFailedParse ? "true" : "false"), LogLevel::INFO, DeviceType::MAIN);
                        if (mainCameraSaveFailedParse)
                        {
                            int saveResult = solveFailedImageSave(SolveImageFileName);
                            if (saveResult == 0)
                            {
                                Logger::Log("TelescopeControl_SolveSYNC | Failed solve image saved successfully", LogLevel::INFO, DeviceType::MAIN);
                            }
                            else
                            {
                                Logger::Log("TelescopeControl_SolveSYNC | Failed to save failed solve image, error code: " + std::to_string(saveResult), LogLevel::WARNING, DeviceType::MAIN);
                            }
                        }
                        else
                        {
                            Logger::Log("TelescopeControl_SolveSYNC | mainCameraSaveFailedParse is disabled, skipping save", LogLevel::INFO, DeviceType::MAIN);
                        }
                        emit wsThread->sendMessageToClient("SolveImagefailed");  // 发送解析失败的消息
                        isSolveSYNC = false;
                        solveTimer.stop();  // 停止定时器
                    }
                    else
                    {
                        if (dpMount != NULL)
                        {
                            INDI::PropertyNumber property = NULL;
                            Logger::Log("TelescopeControl_SolveSYNC | syncTelescopeJNow | start", LogLevel::INFO, DeviceType::MAIN);
                            QString action = "SYNC";
                            bool isTrack = false;
                            indi_Client->getTelescopeTrackEnable(dpMount, isTrack);
                            if (!isTrack)
                            {
                                indi_Client->setTelescopeTrackEnable(dpMount, true);
                            }
                            emit wsThread->sendMessageToClient("TelescopeTrack:ON");

                            // 解析结果 RA/DEC 为“度”，下发到 INDI 前需要转换为 RA 小时制
                            double solvedRaHour = Tools::DegreeToHour(result.RA_Degree);
                            double solvedDecDeg = result.DEC_Degree;

                            // 同步望远镜的当前位置到目标位置（JNOW, RA:hour / DEC:deg）
                            uint32_t syncResult = indi_Client->syncTelescopeJNow(dpMount, solvedRaHour, solvedDecDeg);
                            if (syncResult != QHYCCD_SUCCESS)
                            {
                                Logger::Log("TelescopeControl_SolveSYNC | syncTelescopeJNow failed",
                                            LogLevel::ERROR, DeviceType::MAIN);
                                emit wsThread->sendMessageToClient("SolveImagefailed");
                            }
                            else
                            {
                                Logger::Log("TelescopeControl_SolveSYNC | syncTelescopeJNow | end", LogLevel::INFO, DeviceType::MAIN);
                                // Logger::Log("TelescopeControl_SolveSYNC | DegreeToHour:" + std::to_string(Tools::DegreeToHour(result.RA_Degree)) + "DEC_Degree:" + std::to_string(result.DEC_Degree), LogLevel::INFO, DeviceType::MAIN);

                                // indi_Client->setTelescopeRADECJNOW(dpMount, Tools::DegreeToHour(result.RA_Degree), result.DEC_Degree);  // 设置望远镜的目标位置
                                // Logger::Log("TelescopeControl_SolveSYNC | syncTelescopeJNow | end", LogLevel::INFO, DeviceType::MAIN);
                                // double a, b;
                                // indi_Client->getTelescopeRADECJNOW(dpMount, a, b);  // 获取望远镜的当前位置
                                // Logger::Log("TelescopeControl_SolveSYNC | Get_RA_Hour:" + std::to_string(a) + "Get_DEC_Degree:" + std::to_string(b), LogLevel::INFO, DeviceType::MAIN);
                                emit wsThread->sendMessageToClient("SolveImageSucceeded");
                            }

                            isSolveSYNC = false;
                            solveTimer.stop();  // 停止定时器
                        }
                        else
                        {
                            Logger::Log("TelescopeControl_SolveSYNC | No Mount Connect.", LogLevel::INFO, DeviceType::MAIN);
                            emit wsThread->sendMessageToClient("SolveImagefailed");  // 发送解析失败的消息
                            isSolveSYNC = false;
                            solveTimer.stop();  // 停止定时器
                            return;  // 如果望远镜未连接，记录日志并退出
                        }
                    }
                }
                else if (solveProcessFinished)
                {
                    // 解析进程已结束但未成功完成（退出码非0的情况）
                    Logger::Log("TelescopeControl_SolveSYNC | Solve process finished but failed (exit code != 0)", LogLevel::ERROR, DeviceType::MAIN);
                    Logger::Log("TelescopeControl_SolveSYNC | SolveImageFileName: " + SolveImageFileName.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                    Logger::Log("TelescopeControl_SolveSYNC | mainCameraSaveFailedParse: " + std::string(mainCameraSaveFailedParse ? "true" : "false"), LogLevel::INFO, DeviceType::MAIN);
                    if (mainCameraSaveFailedParse)
                    {
                        int saveResult = solveFailedImageSave(SolveImageFileName);
                        if (saveResult == 0)
                        {
                            Logger::Log("TelescopeControl_SolveSYNC | Failed solve image saved successfully", LogLevel::INFO, DeviceType::MAIN);
                        }
                        else
                        {
                            Logger::Log("TelescopeControl_SolveSYNC | Failed to save failed solve image, error code: " + std::to_string(saveResult), LogLevel::WARNING, DeviceType::MAIN);
                        }
                    }
                    else
                    {
                        Logger::Log("TelescopeControl_SolveSYNC | mainCameraSaveFailedParse is disabled, skipping save", LogLevel::INFO, DeviceType::MAIN);
                    }
                    emit wsThread->sendMessageToClient("SolveImagefailed");  // 发送解析失败的消息
                    isSolveSYNC = false;
                    solveTimer.stop();  // 停止定时器
                }
                else 
                {
                    solveTimer.start(1000);  // 如果解析未完成，重新启动定时器继续等待
                } 
            });

            solveTimer.start(1000);  // 启动解析定时器

        } 
        else 
        {
            // 如果拍摄未完成，重新启动拍摄定时器，继续等待
            captureTimer.start(1000);
        } });
    captureTimer.start(1000);
}

LocationResult MainWindow::TelescopeControl_GetLocation()
{
    LocationResult result;

    if (dpMount != NULL && indi_Client != nullptr)
    {
        const uint32_t getLocationResult =
            indi_Client->getLocation(dpMount, result.latitude_degree, result.longitude_degree, result.elevation);
        if (getLocationResult == QHYCCD_SUCCESS &&
            isValidObservatoryLocation(result.latitude_degree, result.longitude_degree))
        {
            observatorylatitude = result.latitude_degree;
            observatorylongitude = result.longitude_degree;
            if (indi_Client != nullptr)
                indi_Client->mountState.updateHomeRAHours(observatorylatitude, observatorylongitude);
            return result;
        }

        Logger::Log("TelescopeControl_GetLocation | INDI location unavailable or invalid, fallback to MainWindow cached location",
                    LogLevel::WARNING, DeviceType::MAIN);
    }

    if (!isValidObservatoryLocation(observatorylatitude, observatorylongitude))
    {
        bool latOk = false;
        bool lonOk = false;
        const double cachedLat = localLat.trimmed().toDouble(&latOk);
        const double cachedLon = localLon.trimmed().toDouble(&lonOk);
        if (latOk && lonOk && isValidObservatoryLocation(cachedLat, cachedLon))
        {
            observatorylatitude = cachedLat;
            observatorylongitude = cachedLon;
            if (indi_Client != nullptr)
                indi_Client->mountState.updateHomeRAHours(observatorylatitude, observatorylongitude);
            Logger::Log("TelescopeControl_GetLocation | restored MainWindow cached location from localLat/localLon: Latitude: " +
                            QString::number(observatorylatitude).toStdString() +
                            ", Longitude: " + QString::number(observatorylongitude).toStdString(),
                        LogLevel::INFO, DeviceType::MAIN);
        }
    }

    if (isValidObservatoryLocation(observatorylatitude, observatorylongitude))
    {
        result.latitude_degree = observatorylatitude;
        result.longitude_degree = observatorylongitude;
        result.elevation = 50.0;
        Logger::Log("TelescopeControl_GetLocation | using MainWindow cached location: Latitude: " +
                        QString::number(result.latitude_degree).toStdString() +
                        ", Longitude: " + QString::number(result.longitude_degree).toStdString(),
                    LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        Logger::Log("TelescopeControl_GetLocation | no valid INDI or MainWindow cached location available",
                    LogLevel::WARNING, DeviceType::MAIN);
    }

    return result;
}

QDateTime MainWindow::TelescopeControl_GetTimeUTC()
{
    if (dpMount != NULL)
    {
        QDateTime result;

        indi_Client->getTimeUTC(dpMount, result);

        return result;
    }
}

SphericalCoordinates MainWindow::TelescopeControl_GetRaDec()
{
    if (dpMount != NULL)
    {
        SphericalCoordinates result;
        double RA_HOURS, DEC_DEGREE;
        indi_Client->getTelescopeRADECJNOW(dpMount, RA_HOURS, DEC_DEGREE);
        result.ra = Tools::HourToDegree(RA_HOURS);
        result.dec = DEC_DEGREE;

        return result;
    }
}

void MainWindow::MountGoto(double Ra_Hour, double Dec_Degree)
{
    Logger::Log("MountGoto start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("MountGoto | RaDec(Hour):" + std::to_string(Ra_Hour) + "," + std::to_string(Dec_Degree), LogLevel::INFO, DeviceType::MAIN);

    // 在执行 GOTO 之前，如当前处于导星状态，则暂时停止导星，待转动完成后再恢复
    pauseGuidingBeforeMountMove();

    // 停止和清理先前的计时器
    telescopeTimer.stop();
    telescopeTimer.disconnect();

    TelescopeControl_Goto(Ra_Hour, Dec_Degree);

    sleep(2); // 赤道仪的状态更新有一定延迟

    // 启动等待赤道仪转动的定时器
    telescopeTimer.setSingleShot(true);

    connect(&telescopeTimer, &QTimer::timeout, [this, Ra_Hour, Dec_Degree]()
            {
        if (WaitForTelescopeToComplete()) 
        {
            telescopeTimer.stop();  // 转动完成时停止定时器
            Logger::Log("MountGoto | Mount Goto Complete!", LogLevel::INFO, DeviceType::MAIN);

            // 如果本次 GOTO 之前处于导星状态，则在赤道仪转动完成后恢复导星
            resumeGuidingAfterMountMove();
            if (GotoThenSolve) // 判断是否进行解算
            {
                Logger::Log("MountGoto | Goto Then Solve!", LogLevel::INFO, DeviceType::MAIN);
                // 启动一次解算同步流程
                isSolveSYNC = true;
                TelescopeControl_SolveSYNC(); // 开始拍摄解析
                
                if (GotoOlveTimer != nullptr)
                {
                    delete GotoOlveTimer;
                    GotoOlveTimer = nullptr;
                }
                GotoOlveTimer = new QTimer();
                GotoOlveTimer->setSingleShot(true);
                connect(GotoOlveTimer, &QTimer::timeout, [this, Ra_Hour, Dec_Degree]()
                {
                    if (!isSolveSYNC)
                    {
                        GotoOlveTimer->stop();
                        Logger::Log("MountGoto | Goto Then Solve Complete!", LogLevel::INFO, DeviceType::MAIN);
                        // 解算同步完成后，仅再执行一次 Goto 回到目标坐标
                        TelescopeControl_Goto(Ra_Hour, Dec_Degree);
                    }else{
                        GotoOlveTimer->start(1000);
                    }
                });
                GotoOlveTimer->start(1000);
            }
        } 
        else 
        {
            telescopeTimer.start(1000);  // 继续等待
        } });

    telescopeTimer.start(1000);

    Logger::Log("MountGoto finish!", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::MountOnlyGoto(double Ra_Hour, double Dec_Degree)
{
    if (dpMount == NULL)
    {
        Logger::Log("MountOnlyGoto | No Mount Connect.", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("MountOnlyGotoFailed:No Mount Connect");  // 发送转到失败的消息
        return;
    }
    if (Ra_Hour < 0 || Ra_Hour > 24 || Dec_Degree < -90 || Dec_Degree > 90)
    {
        Logger::Log("MountOnlyGoto | Invalid RaDec(Hour):" + std::to_string(Ra_Hour) + "," + std::to_string(Dec_Degree), LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("MountOnlyGotoFailed:Invalid RaDec(Hour)");  // 发送转到失败的消息
        return;
    }
    if (indi_Client->mountState.isMovingNow())
    {
        Logger::Log("MountOnlyGoto | Mount is Moving.", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("MountOnlyGotoFailed:Mount is Moving");  // 发送转到失败的消息
        return;
    }
    Logger::Log("MountOnlyGoto start ...", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("MountOnlyGoto | RaDec(Hour):" + std::to_string(Ra_Hour) + "," + std::to_string(Dec_Degree), LogLevel::INFO, DeviceType::MAIN);

    // 在执行 Goto 之前，如当前处于导星状态，则暂时停止导星，待转动完成后再恢复
    pauseGuidingBeforeMountMove();

    // 停止和清理先前的计时器
    telescopeTimer.stop();
    telescopeTimer.disconnect();

    TelescopeControl_Goto(Ra_Hour, Dec_Degree); // 转到目标位置

    sleep(2); // 赤道仪的状态更新有一定延迟

    // 启动等待赤道仪转动的定时器
    telescopeTimer.setSingleShot(true);

    connect(&telescopeTimer, &QTimer::timeout, [this, Ra_Hour, Dec_Degree]()
    {
        if (WaitForTelescopeToComplete())
        {
            telescopeTimer.stop();  // 转动完成时停止定时器
            Logger::Log("MountOnlyGoto | Mount Only Goto Complete!", LogLevel::INFO, DeviceType::MAIN);
            // 如果本次 Goto 之前处于导星状态，则在赤道仪转动完成后恢复导星
            resumeGuidingAfterMountMove();
            emit wsThread->sendMessageToClient("MountOnlyGotoSuccess");  // 发送转到成功消息
        }
        else
        {
            telescopeTimer.start(1000);  // 继续等待赤道仪转动
        }
    });
    telescopeTimer.start(1000);

    Logger::Log("MountOnlyGoto finish!", LogLevel::INFO, DeviceType::MAIN);

}
void MainWindow::DeleteImage(QStringList DelImgPath)
{
    std::string password = "quarcs"; // sudo 密码
    for (int i = 0; i < DelImgPath.size(); i++)
    {
        if (i < DelImgPath.size())
        {
            QString path = DelImgPath[i].trimmed();
            std::string pathForRm = path.toStdString();
            if (!path.isEmpty() && !QDir::isAbsolutePath(path))
                pathForRm = "./" + pathForRm;
            std::ostringstream commandStream;
            commandStream << "echo '" << password << "' | sudo -S rm -rf \"" << pathForRm << "\"";
            std::string command = commandStream.str();

            Logger::Log("DeleteImage | Deleted command:" + QString::fromStdString(command).toStdString(), LogLevel::INFO, DeviceType::MAIN);

            // 执行系统命令删除文件
            int result = system(command.c_str());

            if (result == 0)
            {
                Logger::Log("DeleteImage | Deleted file:" + DelImgPath[i].toStdString(), LogLevel::INFO, DeviceType::MAIN);
            }
            else
            {
                Logger::Log("DeleteImage | Failed to delete file:" + DelImgPath[i].toStdString(), LogLevel::INFO, DeviceType::MAIN);
            }
        }
        else
        {
            Logger::Log("DeleteImage | Index out of range: " + std::to_string(i), LogLevel::WARNING, DeviceType::MAIN);
        }
    }
}

std::string MainWindow::GetAllFile()
{
    Logger::Log("GetAllFile start ...", LogLevel::INFO, DeviceType::MAIN);
    std::string capturePath = ImageSaveBasePath + "/CaptureImage/";
    std::string planPath = ImageSaveBasePath + "/ScheduleImage/";
    std::string solveFailedImagePath = ImageSaveBasePath + "/solveFailedImage/";
    std::string resultString;
    std::string captureString = "CaptureImage{";
    std::string planString = "ScheduleImage{";
    std::string solveFailedImageString = "SolveFailedImage{";

    try
    {
        // 检查并处理 CaptureImage 目录
        if (std::filesystem::exists(capturePath) && std::filesystem::is_directory(capturePath))
        {
            for (const auto &entry : std::filesystem::directory_iterator(capturePath))
            {
                std::string fileName = entry.path().filename().string(); // 获取文件名（包含扩展名）
                captureString += fileName + ";";                         // 拼接为字符串
            }
        }
        else
        {
            Logger::Log("GetAllFile | CaptureImage directory does not exist or is not a directory: " + capturePath, LogLevel::WARNING, DeviceType::MAIN);
        }

        // 检查并处理 ScheduleImage 目录
        if (std::filesystem::exists(planPath) && std::filesystem::is_directory(planPath))
        {
            for (const auto &entry : std::filesystem::directory_iterator(planPath))
            {
                std::string folderName = entry.path().filename().string(); // 获取文件夹名
                planString += folderName + ";";
            }
        }
        else
        {
            Logger::Log("GetAllFile | ScheduleImage directory does not exist or is not a directory: " + planPath, LogLevel::WARNING, DeviceType::MAIN);
        }
        // 检查并处理 solveFailedImage 目录
        if (std::filesystem::exists(solveFailedImagePath) && std::filesystem::is_directory(solveFailedImagePath))
        {
            for (const auto &entry : std::filesystem ::directory_iterator(solveFailedImagePath))
            {
                std::string fileName = entry.path().filename().string(); // 获取文件名（包含扩展名）
                solveFailedImageString += fileName + ";";                // 拼接为字符串
            }
        }
        else
        {
            Logger::Log("GetAllFile | SolveFailedImage directory does not exist or is not a directory: " + solveFailedImagePath, LogLevel::WARNING, DeviceType::MAIN);
            // solveFailedImageString = "SolveFailedImage{}"; // 如果目录不存在，返回空字符串
        }
    }
    catch (const std::filesystem::filesystem_error &e)
    {
        Logger::Log("GetAllFile | Filesystem error: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
    }
    catch (const std::exception &e)
    {
        Logger::Log("GetAllFile | General error: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
    }

    resultString = captureString + "}:" + planString + "}:" + solveFailedImageString + '}';
    Logger::Log("GetAllFile finish!", LogLevel::INFO, DeviceType::MAIN);
    return resultString;
}
void MainWindow::GetImageFiles(std::string ImageFolder)
{
    Logger::Log("GetImageFiles start ...", LogLevel::INFO, DeviceType::MAIN);
    std::string basePath = ImageSaveBasePath + "/" + ImageFolder + "/";
    std::string ImageFilesNameString = "";

    try
    {
        // 检查目录是否存在
        if (!std::filesystem::exists(basePath))
        {
            Logger::Log("GetImageFiles | Directory does not exist: " + basePath, LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("ImageFilesName:");
            Logger::Log("GetImageFiles finish! (Directory not found)", LogLevel::INFO, DeviceType::MAIN);
            return;
        }

        if (!std::filesystem::is_directory(basePath))
        {
            Logger::Log("GetImageFiles | Path is not a directory: " + basePath, LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("ImageFilesName:");
            Logger::Log("GetImageFiles finish! (Not a directory)", LogLevel::INFO, DeviceType::MAIN);
            return;
        }

        for (const auto &entry : std::filesystem::directory_iterator(basePath))
        {
            std::string fileName = entry.path().filename().string(); // 获取文件名（包含扩展名）
            ImageFilesNameString += fileName + ";";                  // 拼接为字符串
        }
    }
    catch (const std::filesystem::filesystem_error &e)
    {
        Logger::Log("GetImageFiles | Filesystem error: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ImageFilesName:");
        Logger::Log("GetImageFiles finish! (Filesystem error)", LogLevel::INFO, DeviceType::MAIN);
        return;
    }
    catch (const std::exception &e)
    {
        Logger::Log("GetImageFiles | General error: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("ImageFilesName:");
        Logger::Log("GetImageFiles finish! (General error)", LogLevel::INFO, DeviceType::MAIN);
        return;
    }

    Logger::Log("GetImageFiles | Image Files:" + QString::fromStdString(ImageFilesNameString).toStdString(), LogLevel::INFO, DeviceType::MAIN);
    emit wsThread->sendMessageToClient("ImageFilesName:" + QString::fromStdString(ImageFilesNameString));
    Logger::Log("GetImageFiles finish!", LogLevel::INFO, DeviceType::MAIN);
}



// 解析字符串
QStringList MainWindow::parseString(const std::string &input, const std::string &imgFilePath)
{
    QStringList paths;
    QString baseString;
    size_t pos = input.find('{');
    if (pos != std::string::npos)
    {
        baseString = QString::fromStdString(input.substr(0, pos));
        std::string content = input.substr(pos + 1);
        size_t endPos = content.find('}');
        if (endPos != std::string::npos)
        {
            content = content.substr(0, endPos);

            // 去掉末尾的分号（如果有的话）
            if (!content.empty() && content.back() == ';')
            {
                content.pop_back();
            }

            QStringList parts = QString::fromStdString(content).split(';', Qt::SkipEmptyParts);
            for (const QString &part : parts)
            {
                QString path = QDir::toNativeSeparators(QString::fromStdString(imgFilePath) + "/" + baseString + "/" + part);
                paths.append(path);
            }
        }
    }
    return paths;
}

// 返回 U 盘剩余内存
long long MainWindow::getUSBSpace(const QString &usb_mount_point)
{
    Logger::Log("getUSBSpace start ...", LogLevel::INFO, DeviceType::MAIN);
    struct statvfs stat;
    if (statvfs(usb_mount_point.toUtf8().constData(), &stat) == 0)
    {
        // 使用 f_bavail 而不是 f_bfree，因为 f_bavail 是普通用户实际可用的空间
        // f_bfree 可能包含系统保留的空间，实际用户可能无法使用
        long long free_space = static_cast<long long>(stat.f_bavail) * stat.f_frsize;
        Logger::Log("getUSBSpace | USB Space (available): " + std::to_string(free_space) + " bytes", LogLevel::INFO, DeviceType::MAIN);
        return free_space;
    }
    else
    {
        Logger::Log("getUSBSpace | Failed to obtain the space information of the USB flash drive.", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("getUSBFail:Failed to obtain the space information of the USB flash drive.");
        return -1;
    }
}
long long MainWindow::getTotalSize(const QStringList &filePaths)
{
    long long totalSize = 0;
    foreach (QString filePath, filePaths)
    {
        QFileInfo fileInfo(filePath);
        if (fileInfo.exists())
        {
            totalSize += fileInfo.size();
        }
    }
    return totalSize;
}

// 获取文件系统挂载模式
bool MainWindow::isMountReadOnly(const QString &mountPoint)
{
    struct statvfs fsinfo;
    auto mountPointStr = mountPoint.toUtf8().constData();
    Logger::Log("isMountReadOnly | Checking filesystem information for mount point:" + QString::fromUtf8(mountPointStr).toStdString(), LogLevel::INFO, DeviceType::MAIN);

    if (statvfs(mountPointStr, &fsinfo) != 0)
    {
        Logger::Log("isMountReadOnly | Failed to get filesystem information for" + mountPoint.toStdString() + ":" + strerror(errno), LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(QString("getUSBFail:Failed to get filesystem information for %1, error: %2").arg(mountPoint).arg(strerror(errno)));
        return false;
    }

    Logger::Log("isMountReadOnly | Filesystem flags for" + mountPoint.toStdString() + ":" + std::to_string(fsinfo.f_flag), LogLevel::INFO, DeviceType::MAIN);
    return (fsinfo.f_flag & ST_RDONLY) != 0;
}

// 将文件系统挂载模式更改为读写模式
bool MainWindow::remountReadWrite(const QString &mountPoint, const QString &password)
{
    QProcess process;
    process.start("sudo", {"-S", "mount", "-o", "remount,rw", mountPoint});
    if (!process.waitForStarted() || !process.write((password + "\n").toUtf8()))
    {
        Logger::Log("remountReadWrite | Failed to execute command: sudo mount", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("getUSBFail:Failed to execute command: sudo mount -o remount,rw usb.");
        return false;
    }
    process.closeWriteChannel();
    process.waitForFinished(-1);
    return process.exitCode() == 0;
}

void MainWindow::CopyImagesToUsb(QStringList CopyImgPath, QString usbName)
{
    QString usb_mount_point = "";
    
    // 如果提供了U盘名，优先使用它从映射表中查找
    if (!usbName.isEmpty() && usbMountPointsMap.contains(usbName))
    {
        usb_mount_point = usbMountPointsMap[usbName];
        Logger::Log("CopyImagesToUsb | Using specified USB from map: " + usbName.toStdString() + " -> " + usb_mount_point.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    }
    
    // 如果上面没有获取到，优先使用 ImageSaveBaseDirectory 指定的U盘路径
    if (usb_mount_point.isEmpty())
    {
        // 使用saveMode判断是否为U盘保存
        bool isUSBSave = (saveMode != "local");
        
        if (isUSBSave && ImageSaveBaseDirectory.contains("/QUARCS_ImageSave"))
        {
            // 从 ImageSaveBaseDirectory 提取U盘挂载点
            usb_mount_point = ImageSaveBaseDirectory;
            usb_mount_point.replace("/QUARCS_ImageSave", "");
            
            // 验证该U盘是否仍然存在且有效
            QStorageInfo storageInfo(usb_mount_point);
            if (!storageInfo.isValid() || !storageInfo.isReady())
            {
                Logger::Log("CopyImagesToUsb | Specified USB path is no longer valid: " + usb_mount_point.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
                usb_mount_point = ""; // 重置，使用下面的逻辑重新获取
            }
            else
            {
                Logger::Log("CopyImagesToUsb | Using USB from ImageSaveBaseDirectory: " + usb_mount_point.toStdString(), LogLevel::INFO, DeviceType::MAIN);
            }
        }
    }
    
    // 如果上面没有获取到，尝试从映射表获取
    if (usb_mount_point.isEmpty())
    {
        if (usbMountPointsMap.size() == 1)
        {
            // 单个U盘，直接使用
            usb_mount_point = usbMountPointsMap.first();
            Logger::Log("CopyImagesToUsb | Using single USB from map: " + usb_mount_point.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }
        else if (usbMountPointsMap.size() > 1)
        {
            // 多个U盘，如果 ImageSaveBaseDirectory 是U盘路径但提取失败，或者没有指定，需要用户选择
            emit wsThread->sendMessageToClient("ImageSaveErroe:USB-Multiple");
            Logger::Log("CopyImagesToUsb | Multiple USB drives detected, please specify which one to use.", LogLevel::WARNING, DeviceType::MAIN);
            return;
        }
        else
        {
            // 映射表为空，尝试使用统一的U盘挂载点获取函数（作为后备）
            if (!getUSBMountPoint(usb_mount_point))
            {
                // 获取U盘名称用于错误消息
                QString base = "/media/";
                QString username = QDir::home().dirName();
                QString basePath = base + username;
                QDir baseDir(basePath);
                
                if (!baseDir.exists())
                {
                    emit wsThread->sendMessageToClient("ImageSaveErroe:USB-Null");
                    Logger::Log("CopyImagesToUsb | Base directory does not exist.", LogLevel::WARNING, DeviceType::MAIN);
                }
                else
                {
                    QStringList filters;
                    filters << "*";
                    QStringList folderList = baseDir.entryList(filters, QDir::Dirs | QDir::NoDotAndDotDot);
                    folderList.removeAll("CDROM");
                    
                    if (folderList.size() == 0)
                    {
                        emit wsThread->sendMessageToClient("ImageSaveErroe:USB-Null");
                        Logger::Log("CopyImagesToUsb | No USB drive found.", LogLevel::WARNING, DeviceType::MAIN);
                    }
                    else
                    {
                        emit wsThread->sendMessageToClient("ImageSaveErroe:USB-Multiple");
                        Logger::Log("CopyImagesToUsb | Multiple USB drives detected.", LogLevel::WARNING, DeviceType::MAIN);
                    }
                }
                return;
            }
        }
    }

    const QString password = "quarcs"; // sudo 密码

    QStorageInfo storageInfo(usb_mount_point);
    if (storageInfo.isValid() && storageInfo.isReady())
    {
        if (storageInfo.isReadOnly())
        {
            // 处理1: 该路径为只读设备
            if (!remountReadWrite(usb_mount_point, password))
            {
                Logger::Log("CopyImagesToUsb | Failed to remount filesystem as read-write.", LogLevel::WARNING, DeviceType::MAIN);
                return;
            }
            Logger::Log("CopyImagesToUsb | Filesystem remounted as read-write successfully.", LogLevel::INFO, DeviceType::MAIN);
        }
        Logger::Log("CopyImagesToUsb | This path is for writable devices.", LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        Logger::Log("CopyImagesToUsb | The specified path is not a valid file system or is not ready.", LogLevel::WARNING, DeviceType::MAIN);
    }
    // 先统计需要移动的所有文件的总大小
    long long totalSize = getTotalSize(CopyImgPath);
    if (totalSize <= 0)
    {
        Logger::Log("CopyImagesToUsb | No valid files to move or total size is 0.", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("getUSBFail:No valid files to move!");
        return;
    }
    
    // 检查U盘剩余空间
    long long remaining_space = getUSBSpace(usb_mount_point);
    if (remaining_space == -1 || remaining_space <= 0)
    {
        Logger::Log("CopyImagesToUsb | USB drive has no available space or is not accessible.", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("getUSBFail:USB drive has no available space!");
        return;
    }
    
    // 检查空间是否足够（总文件大小必须小于剩余空间）
    if (totalSize > remaining_space)
    {
        Logger::Log("CopyImagesToUsb | Insufficient storage space. Required: " + QString::number(totalSize).toStdString() + 
                   " bytes, Available: " + QString::number(remaining_space).toStdString() + " bytes", LogLevel::WARNING, DeviceType::MAIN);
        QString errorMsg = QString("Not enough storage space! Required: %1 MB, Available: %2 MB")
                          .arg(QString::number(totalSize / (1024.0 * 1024.0), 'f', 2))
                          .arg(QString::number(remaining_space / (1024.0 * 1024.0), 'f', 2));
        emit wsThread->sendMessageToClient("getUSBFail:" + errorMsg);
        return;
    }
    QString folderName = "QUARCS_ImageSave";
    QString folderPath = usb_mount_point + "/" + folderName;
    QString basePath = QString::fromStdString(ImageSaveBasePath).trimmed();
    if (basePath.endsWith('/'))
        basePath.chop(1);

    int sumMoveImage = 0;
    for (const auto &imgPath : CopyImgPath)
    {
        if (!imgPath.startsWith(basePath))
        {
            Logger::Log("CopyImagesToUsb | path is error! (not under ImageSaveBasePath): " + imgPath.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
            continue;
        }
        QString relativePath = imgPath.mid(basePath.length()).trimmed();
        if (relativePath.startsWith('/'))
            relativePath = relativePath.mid(1);
        int lastSlash = relativePath.lastIndexOf('/');
        if (lastSlash == -1)
        {
            Logger::Log("CopyImagesToUsb | path is error! (no directory part): " + imgPath.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
            continue;
        }
        QString relativeDir = relativePath.left(lastSlash + 1);
        QString destinationPath = folderPath + "/" + relativeDir;
        
        // 安全检查：避免在 /media/quarcs 路径下创建任何文件夹，避免被错误识别为U盘
        QString normalizedDestPath = QDir(destinationPath).absolutePath();
        if (normalizedDestPath.startsWith("/media/quarcs/"))
        {
            // 提取 /media/quarcs/ 之后的部分
            QString pathAfterMedia = normalizedDestPath.mid(14); // 去掉 "/media/quarcs/"
            
            // 检查路径格式：应该是 /media/quarcs/某个U盘名/...
            int firstSlash = pathAfterMedia.indexOf('/');
            if (firstSlash > 0)
            {
                QString usbName = pathAfterMedia.left(firstSlash);
                // 检查这个U盘名是否在映射表中（有效的U盘挂载点）
                if (!usbMountPointsMap.contains(usbName))
                {
                    Logger::Log("CopyImagesToUsb | Security check failed: Attempting to create directory in /media/quarcs/ but USB name '" + usbName.toStdString() + "' not found in mount points map. Path: " + destinationPath.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
                    emit wsThread->sendMessageToClient("HasMoveImgnNUmber:fail:" + QString::number(sumMoveImage));
                    continue;
                }
                // 验证路径确实在U盘挂载点下
                QString expectedMountPoint = "/media/quarcs/" + usbName;
                if (!normalizedDestPath.startsWith(expectedMountPoint))
                {
                    Logger::Log("CopyImagesToUsb | Security check failed: Path does not match expected mount point. Path: " + destinationPath.toStdString() + ", Expected mount point: " + expectedMountPoint.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
                    emit wsThread->sendMessageToClient("HasMoveImgnNUmber:fail:" + QString::number(sumMoveImage));
                    continue;
                }
            }
            else
            {
                // 路径格式不正确，可能是直接在 /media/quarcs/ 下创建文件夹
                Logger::Log("CopyImagesToUsb | Security check failed: Invalid path format in /media/quarcs/. Path: " + destinationPath.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("HasMoveImgnNUmber:fail:" + QString::number(sumMoveImage));
                continue;
            }
        }
        // 额外检查：确保路径不是直接在 /media/quarcs 下（没有子目录）
        else if (normalizedDestPath == "/media/quarcs")
        {
            Logger::Log("CopyImagesToUsb | Security check failed: Attempting to create directory directly at /media/quarcs. Path: " + destinationPath.toStdString(), LogLevel::ERROR, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("HasMoveImgnNUmber:fail:" + QString::number(sumMoveImage));
            continue;
        }
        
        QProcess process;
        process.start("sudo", {"-S", "mkdir", "-p", destinationPath});
        if (!process.waitForStarted() || !process.write((password + "\n").toUtf8()))
        {
            Logger::Log("CopyImagesToUsb | Failed to execute command: sudo mkdir.", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("HasMoveImgnNUmber:fail:" + QString::number(sumMoveImage));
            continue;
        }
        process.closeWriteChannel();
        process.waitForFinished(-1);

        process.start("sudo", {"-S", "cp", "-r", imgPath, destinationPath});
        if (!process.waitForStarted() || !process.write((password + "\n").toUtf8()))
        {
            Logger::Log("CopyImagesToUsb | Failed to execute command: sudo cp.", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("HasMoveImgnNUmber:fail:" + QString::number(sumMoveImage));
            continue;
        }
        process.closeWriteChannel();
        process.waitForFinished(-1);

        // Read the standard error output
        QByteArray stderrOutput = process.readAllStandardError();

        if (process.exitCode() == 0)
        {
            Logger::Log("CopyImagesToUsb | Copied file: " + imgPath.toStdString() + " to " + destinationPath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            Logger::Log("CopyImagesToUsb | Failed to copy file: " + imgPath.toStdString() + " to " + destinationPath.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
            // Print the error reason
            Logger::Log("CopyImagesToUsb | Error: " + QString::fromUtf8(stderrOutput).toStdString(), LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("HasMoveImgnNUmber:fail:" + QString::number(sumMoveImage));
            continue;
        }
        sumMoveImage++;
        emit wsThread->sendMessageToClient("HasMoveImgnNUmber:succeed:" + QString::number(sumMoveImage));
    }
}

void MainWindow::USBCheck()
{
    // 清空之前的U盘映射表
    usbMountPointsMap.clear();
    
    QString base = "/media/";
    QString username = QDir::home().dirName();
    QString basePath = base + username;
    QDir baseDir(basePath);
    
    if (!baseDir.exists())
    {
        Logger::Log("USBCheck | Base directory does not exist.", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("USBCheck:Null, Null");
        return;
    }

    // 获取所有文件夹，排除"."和".."，并且排除"CDROM"
    QStringList filters;
    filters << "*";
    QStringList folderList = baseDir.entryList(filters, QDir::Dirs | QDir::NoDotAndDotDot);
    folderList.removeAll("CDROM");

    if (folderList.size() == 0)
    {
        emit wsThread->sendMessageToClient("USBCheck:Null, Null");
        Logger::Log("USBCheck | No USB drive found.", LogLevel::INFO, DeviceType::MAIN);
        return;
    }
    
    // 遍历所有U盘，验证并存储到映射表
    QStringList validUsbList;
    for (const QString &folderName : folderList)
    {
        QString usb_mount_point = basePath + "/" + folderName;
        QStorageInfo storageInfo(usb_mount_point);
        
        // 验证这是否是一个真正挂载的存储设备
        if (storageInfo.isValid() && storageInfo.isReady())
        {
            // 存储U盘信息：U盘名 -> U盘路径
            usbMountPointsMap[folderName] = usb_mount_point;
            validUsbList.append(folderName);
            
            Logger::Log("USBCheck | Found USB: " + folderName.toStdString() + " -> " + usb_mount_point.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }
    }
    
    if (validUsbList.size() == 0)
    {
        emit wsThread->sendMessageToClient("USBCheck:Null, Null");
        Logger::Log("USBCheck | No valid USB drive found.", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    else if (validUsbList.size() == 1)
    {
        // 单个U盘：发送U盘名和剩余空间
        QString usbName = validUsbList.at(0);
        QString usb_mount_point = usbMountPointsMap[usbName];
        long long remaining_space = getUSBSpace(usb_mount_point);
        if (remaining_space == -1)
        {
            Logger::Log("USBCheck | Check whether a USB flash drive or portable hard drive is inserted!", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("USBCheck:Null, Null");
            return;
        }
        QString message = "USBCheck:" + usbName + "," + QString::number(remaining_space);
        Logger::Log("USBCheck | " + message.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(message);
    }
    else
    {
        // 多个U盘：发送所有U盘信息
        QString message = "USBCheck:Multiple";
        QStringList usbInfoList;
        for (const QString &usbName : validUsbList)
        {
            QString usb_mount_point = usbMountPointsMap[usbName];
            long long remaining_space = getUSBSpace(usb_mount_point);
            if (remaining_space != -1)
            {
                usbInfoList.append(usbName + "," + QString::number(remaining_space));
            }
        }
        if (usbInfoList.size() > 0)
        {
            message = message + ":" + usbInfoList.join(":");
        }
        Logger::Log("USBCheck | Multiple USB drives: " + message.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(message);
    }
}

// 获取U盘挂载点（统一函数，供其他函数复用）
bool MainWindow::getUSBMountPoint(QString &usb_mount_point)
{
    QString base = "/media/";
    QString username = QDir::home().dirName();
    QString basePath = base + username;
    QDir baseDir(basePath);
    
    if (!baseDir.exists())
    {
        Logger::Log("getUSBMountPoint | Base directory does not exist.", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }

    // 获取所有文件夹，排除"."和".."，并且排除"CDROM"
    QStringList filters;
    filters << "*";
    QStringList folderList = baseDir.entryList(filters, QDir::Dirs | QDir::NoDotAndDotDot);
    folderList.removeAll("CDROM");

    // 检查剩余文件夹数量是否为1
    if (folderList.size() == 1)
    {
        usb_mount_point = basePath + "/" + folderList.at(0);
        
        // 验证这是否是一个真正挂载的存储设备
        QStorageInfo storageInfo(usb_mount_point);
        if (!storageInfo.isValid() || !storageInfo.isReady())
        {
            Logger::Log("getUSBMountPoint | The directory exists but is not a valid mounted storage device.", LogLevel::WARNING, DeviceType::MAIN);
            return false;
        }
        
        Logger::Log("getUSBMountPoint | USB mount point:" + usb_mount_point.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        return true;
    }
    else if (folderList.size() == 0)
    {
        Logger::Log("getUSBMountPoint | No USB drive found.", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }
    else
    {
        Logger::Log("getUSBMountPoint | Multiple USB drives detected.", LogLevel::WARNING, DeviceType::MAIN);
        return false;
    }
}

void MainWindow::GetUSBFiles(const QString &usbName, const QString &relativePath)
{
    Logger::Log("GetUSBFiles start ...", LogLevel::INFO, DeviceType::MAIN);
    
    // 必须传入U盘名
    if (usbName.isEmpty())
    {
        Logger::Log("GetUSBFiles | USB name is required.", LogLevel::WARNING, DeviceType::MAIN);
        QJsonObject errorObj;
        errorObj["error"] = "USB name is required";
        errorObj["path"] = "";
        errorObj["files"] = QJsonArray();
        QJsonDocument errorDoc(errorObj);
        emit wsThread->sendMessageToClient("USBFilesList:" + errorDoc.toJson(QJsonDocument::Compact));
        return;
    }
    
    // 根据U盘名从映射表获取挂载点路径
    if (!usbMountPointsMap.contains(usbName))
    {
        Logger::Log("GetUSBFiles | Specified USB name not found: " + usbName.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
        QJsonObject errorObj;
        errorObj["error"] = QString("USB drive not found: %1").arg(usbName);
        errorObj["path"] = "";
        errorObj["files"] = QJsonArray();
        QJsonDocument errorDoc(errorObj);
        emit wsThread->sendMessageToClient("USBFilesList:" + errorDoc.toJson(QJsonDocument::Compact));
        return;
    }
    
    QString usb_mount_point = usbMountPointsMap[usbName];
    Logger::Log("GetUSBFiles | Using USB: " + usbName.toStdString() + " -> " + usb_mount_point.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    
    // 构建完整路径
    QString fullPath = usb_mount_point;
    
    // 清理路径，防止路径遍历攻击
    QString cleanPath = relativePath;
    cleanPath.replace("..", ""); // 移除路径遍历
    cleanPath.replace("//", "/"); // 移除双斜杠
    if (cleanPath.startsWith("/"))
    {
        cleanPath = cleanPath.mid(1); // 移除开头的斜杠
    }
    if (!cleanPath.isEmpty())
    {
        fullPath = usb_mount_point + "/" + cleanPath;
    }

    // 验证目录是否存在
    QDir targetDir(fullPath);
    if (!targetDir.exists())
    {
        Logger::Log("GetUSBFiles | Target directory does not exist: " + fullPath.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
        QJsonObject errorObj;
        errorObj["error"] = "Directory not found";
        errorObj["path"] = "/" + relativePath;
        errorObj["files"] = QJsonArray();
        QJsonDocument errorDoc(errorObj);
        emit wsThread->sendMessageToClient("USBFilesList:" + errorDoc.toJson(QJsonDocument::Compact));
        return;
    }

    // 获取文件列表
    QJsonArray filesArray;
    QFileInfoList entries = targetDir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name | QDir::DirsFirst);
    
    for (const QFileInfo &entry : entries)
    {
        QJsonObject fileObj;
        fileObj["name"] = entry.fileName();
        fileObj["isDirectory"] = entry.isDir();
        if (!entry.isDir())
        {
            fileObj["size"] = static_cast<qint64>(entry.size());
        }
        filesArray.append(fileObj);
    }

    // 构建返回结果
    QJsonObject result;
    QString displayPath = relativePath.isEmpty() ? "/" : ("/" + relativePath);
    result["path"] = displayPath;
    result["files"] = filesArray;

    QJsonDocument doc(result);
    QString jsonString = doc.toJson(QJsonDocument::Compact);
    
    Logger::Log("GetUSBFiles | Found " + QString::number(filesArray.size()).toStdString() + " items in " + fullPath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    emit wsThread->sendMessageToClient("USBFilesList:" + jsonString);
    Logger::Log("GetUSBFiles finish!", LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::LoopSolveImage(QString Filename, int FocalLength, double CameraWidth, double CameraHeight)
{
    Logger::Log("LoopSolveImage(" + Filename.toStdString() + ") start ...", LogLevel::INFO, DeviceType::MAIN);

    if (!isLoopSolveImage)
    {
        Logger::Log("LoopSolveImage | Loop Solve Image end.", LogLevel::INFO, DeviceType::MAIN);
        return;
    }

    solveTimer.stop();
    solveTimer.disconnect();

    Tools::PlateSolve(Filename, FocalLength, CameraWidth, CameraHeight, false);

    // 启动等待赤道仪转动的定时器
    solveTimer.setSingleShot(true);

    connect(&solveTimer, &QTimer::timeout, [this, Filename]()
            {
        // 检查赤道仪状态
        if (Tools::isSolveImageFinish())
        {
            SloveResults result = Tools::ReadSolveResult(Filename, glMainCCDSizeX, glMainCCDSizeY);
            Logger::Log("LoopSolveImage | Plate Solve Result(RA_Degree, DEC_Degree):" + std::to_string(result.RA_Degree) + ", " + std::to_string(result.DEC_Degree), LogLevel::INFO, DeviceType::MAIN);

            if (result.RA_Degree == -1 && result.DEC_Degree == -1)
            {
                Logger::Log("LoopSolveImage | Solve image failed...", LogLevel::WARNING, DeviceType::MAIN);
                emit wsThread->sendMessageToClient("SolveImagefailed");
                emit wsThread->sendMessageToClient("LoopSolveImageFinished");
            }
            else
            {
                emit wsThread->sendMessageToClient("RealTimeSolveImageResult:" + QString::number(result.RA_Degree) + ":" + QString::number(result.DEC_Degree) + ":" + QString::number(Tools::RadToDegree(0)) + ":" + QString::number(Tools::RadToDegree(0)));
                emit wsThread->sendMessageToClient("LoopSolveImageFinished");
            }

            // CaptureAndSolve(glExpTime, true);
        }
        else 
        {
            solveTimer.start(1000);  // 继续等待
        } });

    solveTimer.start(1000);
}

void MainWindow::ClearSloveResultList()
{
    SloveResultList.clear();
}

void MainWindow::RecoverySloveResul()
{
    for (const auto &result : SloveResultList)
    {
        Logger::Log("RecoverySloveResul | Plate Solve Result(RA_Degree, DEC_Degree):" + std::to_string(result.RA_Degree) + ", " + std::to_string(result.DEC_Degree), LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("SolveImageResult:" + QString::number(result.RA_Degree) + ":" + QString::number(result.DEC_Degree) + ":" + QString::number(Tools::RadToDegree(0)) + ":" + QString::number(Tools::RadToDegree(0)));
        emit wsThread->sendMessageToClient("SolveFovResult:" + QString::number(result.RA_0) + ":" + QString::number(result.DEC_0) + ":" + QString::number(result.RA_1) + ":" + QString::number(result.DEC_1) + ":" + QString::number(result.RA_2) + ":" + QString::number(result.DEC_2) + ":" + QString::number(result.RA_3) + ":" + QString::number(result.DEC_3));
    }
}

void MainWindow::editHotspotName(QString newName)
{
    Logger::Log("editHotspotName(" + newName.toStdString() + ") start ...", LogLevel::INFO, DeviceType::MAIN);

    if (networkConfigChanging) {
        Logger::Log("editHotspotName ignored because network config is already changing",
                    LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("EditHotspotNameFailed");
        emit wsThread->sendMessageToClient("WiFiSaveResult|save|fail|busy");
        return;
    }

    if (preferApStaStack()) {
        networkConfigChanging = true;
        QString writeErr;
        const bool writeOk = writePreferredHotspotName(newName, &writeErr);
        if (!writeOk) {
            networkConfigChanging = false;
            emit wsThread->sendMessageToClient("EditHotspotNameFailed");
            Logger::Log("editHotspotName | write hostapd conf failed: " + writeErr.toStdString(),
                        LogLevel::WARNING, DeviceType::MAIN);
            return;
        }

        const SyncCommandResult restartRes =
            runSudoSync("/usr/bin/systemctl", {"restart", QString::fromUtf8(kPreferredHostapdService)}, 5000);
        const QString HostpotName = getHotspotName();
        networkConfigChanging = false;
        if (restartRes.exitCode == 0 && HostpotName == newName) {
            emit wsThread->sendMessageToClient("EditHotspotNameSuccess");
        } else {
            emit wsThread->sendMessageToClient("EditHotspotNameFailed");
            Logger::Log("editHotspotName | restart hostapd failed: " +
                            (restartRes.err.isEmpty() ? restartRes.out : restartRes.err).toStdString(),
                        LogLevel::WARNING, DeviceType::MAIN);
        }
        return;
    }

    const QString connectionName = QString::fromUtf8(kLegacyHotspotConnectionName);
    networkConfigChanging = true;

    // Remote control is optional. When sudoers is not configured, fail fast instead of
    // blocking the Qt backend on a password prompt.
    const SyncCommandResult result = runSudoSync("/usr/bin/nmcli",
                                                 {"connection", "modify",
                                                  connectionName,
                                                  "802-11-wireless.ssid", newName});
    const int exitCode = result.exitCode;
    const QString out = result.out;
    const QString err = result.err;
    Logger::Log(("editHotspotName | nmcli modify exit=" + std::to_string(exitCode) +
                 " out=" + out.toStdString() + " err=" + err.toStdString()),
                LogLevel::INFO, DeviceType::MAIN);

    // Refresh name by nmcli query
    QString HostpotName = getHotspotName();
    Logger::Log("editHotspotName | New Hotspot Name:" + HostpotName.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    if (exitCode == 0 && HostpotName == newName)
    {
        emit wsThread->sendMessageToClient("EditHotspotNameSuccess");
        // Restart hotspot connection to apply changes without restarting NetworkManager
        restartHotspotWithDelay(10);
    }
    else
    {
        emit wsThread->sendMessageToClient("EditHotspotNameFailed");
        Logger::Log("editHotspotName | Edit Hotspot name failed.", LogLevel::WARNING, DeviceType::MAIN);
    }
    networkConfigChanging = false;
}

QString MainWindow::getHotspotName()
{
    if (preferApStaStack()) {
        const QString ssid = readPreferredHotspotName();
        Logger::Log("getHotspotName | hostapd ssid:" + ssid.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        return ssid;
    }

    const QString connectionName = QString::fromUtf8(kLegacyHotspotConnectionName);

    // Prefer nmcli query (stable) over parsing the nmconnection file.
    {
        const SyncCommandResult result = runSudoSync("/usr/bin/nmcli",
                                                     {"-g", "802-11-wireless.ssid",
                                                      "connection", "show", connectionName});
        const QString ssid = result.out;
        const QString err = result.err;
        if (!ssid.isEmpty()) {
            Logger::Log("getHotspotName | nmcli ssid:" + ssid.toStdString(), LogLevel::INFO, DeviceType::MAIN);
            return ssid;
        }
        if (!err.isEmpty()) {
            Logger::Log("getHotspotName | nmcli error:" + err.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
        }
    }

    // Fallback: parse file directly when readable. If permissions are insufficient,
    // keep returning "N/A" so AP mode / Qt startup remain unaffected.
    const QString output =
        readTextFile(QStringLiteral("/etc/NetworkManager/system-connections/") +
                     QString::fromUtf8(kLegacyHotspotConnectionName) +
                     QStringLiteral(".nmconnection"));
    Logger::Log("getHotspotName | file output:" + output.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    QString ssidPattern = "ssid=";
    int index = output.indexOf(ssidPattern);
    if (index != -1)
    {
        int start = index + ssidPattern.length();
        int end = output.indexOf("\n", start);
        if (end == -1) end = output.length();
        return output.mid(start, end - start).trimmed();
    }
    return "N/A";
}

/**
 * @brief 关闭当前热点指定秒数后再重新启动
 *        实现方式：使用 nmcli 将指定连接 down，然后通过 QTimer 在 delaySeconds 秒后 up。
 *        注意：该操作会在一段时间内中断当前 WiFi 热点和网络连接。
 */
void MainWindow::restartHotspotWithDelay(int delaySeconds)
{
    Logger::Log("restartHotspotWithDelay(" + std::to_string(delaySeconds) + ") start ...",
                LogLevel::INFO, DeviceType::MAIN);

    if (preferApStaStack()) {
        const SyncCommandResult stopRes =
            runSudoSync("/usr/bin/systemctl", {"stop", QString::fromUtf8(kPreferredHostapdService)}, 5000);
        if (!stopRes.out.isEmpty()) {
            Logger::Log("restartHotspotWithDelay | hostapd stop output:" + stopRes.out.toStdString(),
                        LogLevel::INFO, DeviceType::MAIN);
        }
        if (!stopRes.err.isEmpty()) {
            Logger::Log("restartHotspotWithDelay | hostapd stop error:" + stopRes.err.toStdString(),
                        LogLevel::WARNING, DeviceType::MAIN);
        }

        const int delayMs = std::max(0, delaySeconds) * 1000;
        QTimer::singleShot(delayMs, this, []() {
            const SyncCommandResult startRes =
                runSudoSync("/usr/bin/systemctl", {"start", QString::fromUtf8(kPreferredHostapdService)}, 5000);
            if (!startRes.out.isEmpty()) {
                Logger::Log("restartHotspotWithDelay | hostapd start output:" + startRes.out.toStdString(),
                            LogLevel::INFO, DeviceType::MAIN);
            }
            if (!startRes.err.isEmpty()) {
                Logger::Log("restartHotspotWithDelay | hostapd start error:" + startRes.err.toStdString(),
                            LogLevel::WARNING, DeviceType::MAIN);
            }
            Logger::Log("restartHotspotWithDelay | hostapd restart sequence finished",
                        LogLevel::INFO, DeviceType::MAIN);
        });
        return;
    }

    // 当前热点连接名称（与 getHotspotName 读取的配置文件一致）
    const QString connectionName = QString::fromUtf8(kLegacyHotspotConnectionName);

    // 先关闭当前热点
    {
        const SyncCommandResult result = runSudoSync("/usr/bin/nmcli",
                                                     {"connection", "down", connectionName});
        Logger::Log("restartHotspotWithDelay | down output:" + result.out.toStdString(),
                    LogLevel::INFO, DeviceType::MAIN);
        if (!result.err.isEmpty())
        {
            Logger::Log("restartHotspotWithDelay | down error:" + result.err.toStdString(),
                        LogLevel::WARNING, DeviceType::MAIN);
        }
    }

    // 使用 QTimer 在 delaySeconds 秒后重新启动热点，避免阻塞主线程
    int delayMs = std::max(0, delaySeconds) * 1000;

    QTimer::singleShot(delayMs, this, [this, connectionName]() {
        Logger::Log("restartHotspotWithDelay | starting hotspot again ...",
                    LogLevel::INFO, DeviceType::MAIN);

        const SyncCommandResult result = runSudoSync("/usr/bin/nmcli",
                                                     {"connection", "up", connectionName});
        Logger::Log("restartHotspotWithDelay | up output:" + result.out.toStdString(),
                    LogLevel::INFO, DeviceType::MAIN);
        if (!result.err.isEmpty())
        {
            Logger::Log("restartHotspotWithDelay | up error:" + result.err.toStdString(),
                        LogLevel::WARNING, DeviceType::MAIN);
        }

        Logger::Log("restartHotspotWithDelay | hotspot restart sequence finished",
                    LogLevel::INFO, DeviceType::MAIN);
    });
}

void MainWindow::SendDebugToVueClient(const QString &msg)
{
    emit wsThread->sendMessageToClient("SendDebugMessage|" + msg);
}

void MainWindow::customMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context);

    if (!instance)
    {
        return;
    }

    QString currentTime = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    QString typeStr;
    switch (type)
    {
    case QtDebugMsg:
        typeStr = "Debug";
        break;
    case QtInfoMsg:
        typeStr = "Info";
        break;
    case QtWarningMsg:
        typeStr = "Warning";
        break;
    case QtCriticalMsg:
        typeStr = "Critical";
        break;
    default:
        typeStr = "Unknown";
    }

    // 统一格式化日志消息
    QString logMessage = QString("[Server] %1 | %2 | %3").arg(currentTime).arg(typeStr).arg(msg);

    // // 将日志消息添加到缓存
    // instance->logCache.push_back(logMessage);

    // 统一输出到标准错误，无论日志类型
    fprintf(stderr, "%s\n", logMessage.toLocal8Bit().constData());

    // 发送到客户端的Vue应用，包括日志类型和消息
    instance->SendDebugToVueClient(typeStr + "|" + msg);
}
void MainWindow::loadSelectedDriverList()
{
    // 🔥 打印所有已注册的 SDK 驱动（包括主名称和别名）
    std::vector<std::string> registeredDrivers = SdkManager::instance().listRegisteredDrivers();
    Logger::Log("loadSelectedDriverList | ========== 已注册的 SDK 驱动列表 ==========", LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("loadSelectedDriverList | 已注册的驱动名称总数（包括别名）: " + std::to_string(registeredDrivers.size()), LogLevel::INFO, DeviceType::MAIN);
    
    // 使用集合去重，按驱动实例分组显示
    // 使用排序后的别名列表作为唯一标识，避免因顺序不同导致重复
    std::unordered_set<std::string> processedDriverKeys;
    int uniqueDriverCount = 0;
    
    for (size_t i = 0; i < registeredDrivers.size(); i++)
    {
        std::string driverName = registeredDrivers[i];
        
        // 获取该驱动的所有别名（包括主名称）
        std::vector<std::string> allNames = SdkManager::instance().getDriverAliases(driverName);
        
        if (allNames.empty())
        {
            continue;
        }
        
        // 对别名列表进行排序，生成唯一标识键
        std::vector<std::string> sortedNames = allNames;
        std::sort(sortedNames.begin(), sortedNames.end());
        std::string driverKey = "";
        for (size_t j = 0; j < sortedNames.size(); j++)
        {
            if (j > 0) driverKey += "|";
            driverKey += sortedNames[j];
        }
        
        // 检查是否已经处理过这个驱动实例
        if (processedDriverKeys.find(driverKey) == processedDriverKeys.end())
        {
            uniqueDriverCount++;
            processedDriverKeys.insert(driverKey);
            
            // 构建显示字符串（使用原始顺序，不排序）
            std::string allNamesStr = "";
            for (size_t j = 0; j < allNames.size(); j++)
            {
                if (j > 0) allNamesStr += ", ";
                allNamesStr += allNames[j];
            }
            
            // 🔥 获取驱动支持的设备类型
            std::string deviceTypesStr = "";
            try {
                // 通过驱动名称获取驱动指针并尝试获取设备类型
                // 由于 ISdkDriver 接口中没有 supportedDeviceTypes() 方法，
                // 我们需要通过类型转换来调用具体驱动的方法
                std::string firstDriverName = allNames[0];
                
                // 尝试通过 SdkManager 获取驱动指针（需要访问内部，这里使用已知的驱动名称判断）
                // 检查是否为 QHYCCD 相机驱动
                bool isQhyCamera = false;
                bool isQhyFocuser = false;
                for (const auto& name : allNames)
                {
                    if (name == "indi_qhy_ccd" || name == "indi_qhy_ccd")
                    {
                        isQhyCamera = true;
                        break;
                    }
                    if (name == "indi_qhy_focuser" || name == "indi_qhy_focuser")
                    {
                        isQhyFocuser = true;
                        break;
                    }
                }
                
                if (isQhyCamera)
                {
                    deviceTypesStr = "设备类型: MainCamera, GuideCamera";
                }
                else if (isQhyFocuser)
                {
                    deviceTypesStr = "设备类型: Focuser";
                }
                else
                {
                    deviceTypesStr = "设备类型: 未知";
                }
            } catch (...) {
                deviceTypesStr = "设备类型: 获取失败";
            }
            
            Logger::Log("loadSelectedDriverList | 驱动 #" + std::to_string(uniqueDriverCount) + ": " + allNamesStr + " | " + deviceTypesStr, LogLevel::INFO, DeviceType::MAIN);
        }
    }
    
    Logger::Log("loadSelectedDriverList | 唯一驱动实例数: " + std::to_string(uniqueDriverCount), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("loadSelectedDriverList | ==========================================", LogLevel::INFO, DeviceType::MAIN);

    // 1. 检查 systemdevicelist.system_devices 是否为空
    if (systemdevicelist.system_devices.empty())
    {
        Logger::Log("loadSelectedDriverList | system_devices is empty", LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    // 2. 检查 wsThread 是否为空
    if (wsThread == nullptr)
    {
        Logger::Log("loadSelectedDriverList | wsThread is null", LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    QString order = "SelectedDriverList";

    // 3. 使用安全的范围检查
    try
    {
        for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
        {
            // 4. 检查索引是否有效
            if (i >= systemdevicelist.system_devices.size())
            {
                Logger::Log("loadSelectedDriverList | Index out of bounds: " + std::to_string(i), LogLevel::ERROR, DeviceType::MAIN);
                break;
            }

            // 5. 检查 Description 是否有效
            if (!systemdevicelist.system_devices[i].Description.isEmpty())
            {
                QString description = systemdevicelist.system_devices[i].Description;
                QString driverName = systemdevicelist.system_devices[i].DriverIndiName;

                // 6. 即使 driverName 为空也发送，以便前端可以清除驱动显示
                if (!description.isEmpty())
                {
                    // 判断该设备是否支持 SDK（如果 driverName 为空，则不支持）
                    bool supportSDK = !driverName.isEmpty() && isDeviceTypeSupportSDK(description, driverName);
                    
                    // 获取当前连接模式
                    QString connectionMode = systemdevicelist.system_devices[i].isSDKConnect ? "SDK" : "INDI";
                    
                    // 消息格式：SelectedDriverList:Description:DriverName:SDKSupport:ConnectionMode:...
                    // SDKSupport: "true" 表示支持 SDK，"false" 表示不支持
                    // ConnectionMode: "SDK" 或 "INDI"
                    // 注意：即使 driverName 为空，也发送该条目，以便前端清除驱动显示
                    order += ":" + description + ":" + driverName + ":" + 
                             (supportSDK ? "true" : "false") + ":" + connectionMode;
                    
                    Logger::Log("loadSelectedDriverList | Added device: " + description.toStdString() + 
                               " - " + (driverName.isEmpty() ? "(empty)" : driverName.toStdString()) + 
                               " (SDK支持: " + (supportSDK ? "是" : "否") + 
                               ", 连接模式: " + connectionMode.toStdString() + ")", 
                               LogLevel::DEBUG, DeviceType::MAIN);
                }
            }
        }

        // 7. 确保 wsThread 和 sendMessageToClient 方法存在
        if (wsThread != nullptr)
        {
            Logger::Log("loadSelectedDriverList | Sending message: " + order.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
            emit wsThread->sendMessageToClient(order);
        }
    }
    catch (const std::exception &e)
    {
        Logger::Log("loadSelectedDriverList | Exception caught: " + std::string(e.what()), LogLevel::ERROR, DeviceType::MAIN);
    }
    catch (...)
    {
        Logger::Log("loadSelectedDriverList | Unknown exception caught", LogLevel::ERROR, DeviceType::MAIN);
    }
}

void MainWindow::loadBindDeviceTypeList()
{
    QString order = "BindDeviceTypeList";
    if (wsThread == nullptr)
    {
        Logger::Log("LoadBindDeviceTypeList | wsThread is nullptr, skip", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }

    for (int i = 0; i < systemdevicelist.system_devices.size(); i++)
    {
        if (systemdevicelist.system_devices[i].Description != "" && systemdevicelist.system_devices[i].isConnect == true)
        {
            order += ":" + systemdevicelist.system_devices[i].Description + ":" +
                     systemdevicelist.system_devices[i].DeviceIndiName + ":" +
                     systemdevicelist.system_devices[i].DriverIndiName + ":" + (systemdevicelist.system_devices[i].isBind ? "true" : "false");
            if (systemdevicelist.system_devices[i].Description == "MainCamera" && systemdevicelist.system_devices[i].isBind)
            {
                emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));
                emit wsThread->sendMessageToClient("MainCameraOffsetRange:" + QString::number(glOffsetMin) + ":" + QString::number(glOffsetMax) + ":" + QString::number(glOffsetValue));
                emit wsThread->sendMessageToClient("MainCameraGainRange:" + QString::number(glGainMin) + ":" + QString::number(glGainMax) + ":" + QString::number(glGainValue));

                // CFW 检测：INDI 模式下通过 dpMainCamera 查询；SDK 模式下 dpMainCamera 可能为空，必须跳过避免段错误
                if (!isMainCameraSDK())
                {
                    if (indi_Client != nullptr && dpMainCamera != nullptr)
                    {
                        QString CFWname;
                        indi_Client->getCFWSlotName(dpMainCamera, CFWname);
                        if (CFWname != "")
                        {
                            Logger::Log("LoadBindDeviceTypeList | get CFW Slot Name: " + CFWname.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                            isFilterOnCamera = true;
                            order += ":CFW:" + CFWname + " (on camera)" + ":" + systemdevicelist.system_devices[i].DriverIndiName + ":" + (systemdevicelist.system_devices[i].isBind ? "true" : "false");
                            int min, max, pos;
                            indi_Client->getCFWPosition(dpMainCamera, pos, min, max);
                            Logger::Log("LoadBindDeviceTypeList | getCFWPosition: " + std::to_string(min) + ", " + std::to_string(max) + ", " + std::to_string(pos), LogLevel::INFO, DeviceType::MAIN);
                            emit wsThread->sendMessageToClient("CFWPositionMax:" + QString::number(max));
                        }
                    }
                    else
                    {
                        Logger::Log("LoadBindDeviceTypeList | INDI main camera not ready (indi_Client or dpMainCamera is nullptr), skip CFW query", LogLevel::WARNING, DeviceType::MAIN);
                    }
                }
                else
                {
                    // SDK 模式下不在此处重复调用 IsCFWPlugged（QHY SDK 该调用可能阻塞 10~30s）。
                    // 这里改用连接阶段（AfterDeviceConnect）缓存结果，避免“加载绑定列表”链路长时间卡顿。
                    if (sdkMainCameraHandle != nullptr)
                    {
                        if (isFilterOnCamera)
                        {
                            const QString cfwDisplayName = sdkMainCameraId.isEmpty()
                                ? "CFW (on camera)"
                                : ("CFW (on camera) - " + sdkMainCameraId);

                            order += ":CFW:" + cfwDisplayName + ":" +
                                     QString::fromLatin1("indi_qhy_ccd") + ":" +
                                     (systemdevicelist.system_devices[i].isBind ? "true" : "false");

                            if (sdkMainCfwSlotsCached > 0)
                            {
                                emit wsThread->sendMessageToClient("CFWPositionMax:" + QString::number(sdkMainCfwSlotsCached));
                            }

                            // 若已有缓存名称列表，则直接推送一次，避免刷新后列表为空
                            const QString key = sdkCfwStorageKey(sdkMainCameraId);
                            const QString list = Tools::readCFWList(key);
                            if (!list.isEmpty())
                                emit wsThread->sendMessageToClient("getCFWList:" + list);
                        }
                        else
                        {
                            Logger::Log("LoadBindDeviceTypeList | MainCamera is in SDK mode, no cached CFW state",
                                        LogLevel::DEBUG, DeviceType::MAIN);
                        }
                    }
                    else
                    {
                        Logger::Log("LoadBindDeviceTypeList | MainCamera is in SDK mode but sdkMainCameraHandle is nullptr", LogLevel::WARNING, DeviceType::MAIN);
                    }
                }
            }
            else if (systemdevicelist.system_devices[i].Description == "Guider" && systemdevicelist.system_devices[i].isBind)
            {
                emit wsThread->sendMessageToClient("GuiderOffsetRange:" + QString::number(glGuiderOffsetMin) + ":" + QString::number(glGuiderOffsetMax) + ":" + QString::number(glGuiderOffsetValue));
                emit wsThread->sendMessageToClient("GuiderGainRange:" + QString::number(glGuiderGainMin) + ":" + QString::number(glGuiderGainMax) + ":" + QString::number(glGuiderGainValue));
            }
        }
    }
    Logger::Log("LoadBindDeviceTypeList | Bind Device Type List:" + order.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    emit wsThread->sendMessageToClient(order);
}

void MainWindow::loadBindDeviceList(MyClient *client)
{
    QString order = "BindDeviceList";
    QSet<QString> emittedKeys;
    auto appendDeviceToOrder = [&](const QString &type, const QString &name, int index) {
        const QString trimmedType = type.trimmed();
        const QString trimmedName = name.trimmed();
        if (trimmedType.isEmpty() || trimmedName.isEmpty())
            return;
        // 去重键必须包含 index，避免“同型号同名但不同设备”被误合并，
        // 导致设备绑定界面看不到可交换设备。
        const QString dedupKey = trimmedType + ":" + QString::number(index);
        if (emittedKeys.contains(dedupKey))
            return;
        emittedKeys.insert(dedupKey);
        order += ":" + trimmedType + ":" + trimmedName + ":" + QString::number(index);
    };

    // 先把“SDK 已打开设备”也同步给前端：
    // - SDK 模式下，这些设备不在 INDI 设备列表里，旧逻辑会导致前端刷新后“待分配设备列表”为空。
    // - 使用负 index（sdkUiIndexFromPoolIndex 返回 -(poolIndex+1)），避免与 INDI 的正 index 冲突。
    if (wsThread == nullptr)
    {
        Logger::Log("LoadBindDeviceList | wsThread is nullptr, skip", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    if (!g_sdkQhyCamIds.isEmpty())
    {
        for (int i = 0; i < g_sdkQhyCamIds.size(); ++i)
        {
            if (g_sdkQhyCamHandles.size() <= i) break;
            if (g_sdkQhyCamHandles[i] == nullptr) continue;
            if (g_sdkQhyCamIds[i].isEmpty()) continue;
            const int uiIdx = sdkUiIndexFromPoolIndex(i);
            // 直接在 BindDeviceList 中带上 Type，格式升级为三元组：Type:Name:Index
            appendDeviceToOrder("CCD", g_sdkQhyCamIds[i], uiIdx);
        }
    }

    // SDK 电调（Focuser）也需要在刷新时出现在“待分配列表”：
    // - SDK 电调不在 INDI 设备列表里，若不在此处同步，前端刷新后会看不到它
    // - 使用固定负 index，避免与 INDI 的 index 以及相机池 index 冲突
    if (systemdevicelist.system_devices.size() > 22 &&
        systemdevicelist.system_devices[22].isSDKConnect)
    {
        // 名称必须与 BindDeviceTypeList/ConnectSuccess 中的 DeviceName 保持一致，
        // 否则前端无法把该设备从“未分配列表”标记为已绑定，造成“已绑定但仍出现在未分配列表/命名变化”。
        QString name;
        const QString saved = systemdevicelist.system_devices[22].DeviceIndiName;
        if (!saved.isEmpty())
            name = saved;
        else if (!sdkFocuserPort.isEmpty())
            name = sdkFocuserPort;
        // 只使用真实串口名，避免占位符导致前端出现“SDK_Focuser”
        if (!name.isEmpty())
        {
            appendDeviceToOrder("Focuser", name, SDK_FOCUSER_UI_INDEX);
        }
        else
        {
            Logger::Log("LoadBindDeviceList | skip SDK Focuser: no valid port/name", LogLevel::WARNING, DeviceType::FOCUSER);
        }
    }

    // 再追加 INDI 已连接设备（保持原有协议）
    if (client == nullptr)
    {
        Logger::Log("LoadBindDeviceList | client is nullptr", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(order);
        return;
    }

    int deviceCount = client->GetDeviceCount();
    if (deviceCount <= 0)
    {
        Logger::Log("LoadBindDeviceList | no devices in client list", LogLevel::INFO, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(order);
        return;
    }

    for (int i = 0; i < deviceCount; i++)
    {
        INDI::BaseDevice *device = client->GetDeviceFromList(i);
        if (device == nullptr)
        {
            Logger::Log("LoadBindDeviceList | Device at index " + std::to_string(i) + " is nullptr", LogLevel::WARNING, DeviceType::MAIN);
            continue;
        }

        const char *name = device->getDeviceName();
        if (!name)
        {
            Logger::Log("LoadBindDeviceList | Device at index " + std::to_string(i) + " has null name pointer", LogLevel::WARNING, DeviceType::MAIN);
            continue;
        }

        QString qName = QString::fromUtf8(name);
        if (device->isConnected() && !qName.isEmpty())
        {
            // 类型推断：优先使用 INDI interface 位
            QString type = "Device";
            const uint32_t iface = device->getDriverInterface();
            if (iface & INDI::BaseDevice::CCD_INTERFACE) type = "CCD";
            else if (iface & INDI::BaseDevice::FILTER_INTERFACE) type = "CFW";
            else if (iface & INDI::BaseDevice::TELESCOPE_INTERFACE) type = "Mount";
            else if (iface & INDI::BaseDevice::FOCUSER_INTERFACE) type = "Focuser";
            // 待分配设备列表中暂时不展示 Mount（望远镜）项
            if (type == "Mount")
                continue;
            // BindDeviceList 升级为：Type:Name:Index
            appendDeviceToOrder(type, qName, i);
        }
    }

    Logger::Log("LoadBindDeviceList | Bind Device List:" + order.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    emit wsThread->sendMessageToClient(order);
}

void MainWindow::loadSDKVersionAndUSBSerialPath()
{
    QString order = "SDKVersionAndUSBSerialPath";
    // 某些连接/重连时序下，wsThread/indi_Client 可能暂未就绪，避免空指针段错误
    if (!wsThread)
    {
        Logger::Log("LoadSDKVersionAndUSBSerialPath | wsThread is nullptr, skip", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }

    // 主相机：支持 SDK / INDI 两种模式
    if (isMainCameraConnected())
    {
        QString sdkVersion = "null";

        // SDK 模式：不要走 INDI 的 dpMainCamera（很可能为空），直接通过 SDK Driver 获取
        if (isMainCameraSDK() && sdkMainCameraHandle != nullptr)
        {
            SdkCommand verCmd;
            verCmd.type = SdkCommandType::Custom;
            verCmd.name = "GetSdkVersion";
            verCmd.payload = std::any();
            // 直接通过设备句柄调用，无需指定驱动名称
            SdkResult verRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, verCmd);
            if (verRes.success)
            {
                try
                {
                    std::string version = std::any_cast<std::string>(verRes.payload);
                    sdkVersion = QString::fromStdString(version);
                }
                catch (const std::bad_any_cast &)
                {
                    Logger::Log("LoadSDKVersionAndUSBSerialPath | bad_any_cast for SDK version payload", LogLevel::WARNING, DeviceType::MAIN);
                }
            }
        }
        else
        {
            // INDI 模式
            if (indi_Client != nullptr && dpMainCamera != nullptr)
            {
                indi_Client->getCCDSDKVersion(dpMainCamera, sdkVersion);
            }
        }

        order += ":MainCamera:" + sdkVersion + ":null";
    }

    // 其余设备目前仅走 INDI：增加空指针保护
    if (indi_Client != nullptr && dpGuider != NULL)
    {
        QString sdkVersion = "null";
        indi_Client->getCCDSDKVersion(dpGuider, sdkVersion);
        order += ":Guider:" + sdkVersion + ":null";
    }
    if (indi_Client != nullptr && dpFocuser != NULL)
    {
        QString sdkVersion = "null";
        indi_Client->getFocuserSDKVersion(dpFocuser, sdkVersion);
        QString DevicePort = "null";
        indi_Client->getDevicePort(dpFocuser, DevicePort);
        order += ":Focuser:" + sdkVersion + ":" + DevicePort;
    }
    // if (dpCFW != NULL)
    // {
    //     QString sdkVersion;
    //     indi_Client->getSDKVersion(dpCFW, sdkVersion);
    //     QString usbSerialPath;
    //     indi_Client->getUSBSerialPath(dpCFW, usbSerialPath);
    //     order += ":CFW:" + sdkVersion + ":" + usbSerialPath;
    // }
    if (indi_Client != nullptr && dpMount != NULL)
    {
        QString sdkVersion;
        indi_Client->getMountInfo(dpMount, sdkVersion);
        QString usbSerialPath;
        indi_Client->getDevicePort(dpMount, usbSerialPath);
        order += ":Mount:" + sdkVersion + ":" + usbSerialPath;
    }
    emit wsThread->sendMessageToClient(order);
    Logger::Log("LoadSDKVersionAndUSBSerialPath | SDKVersionAndUSBSerialPath:" + order.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
}

// 串口通信列表
QStringList MainWindow::getConnectedSerialPorts()
{
    QStringList activeSerialPortNames;
    const auto infos = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : infos)
    {
         QString portPath = info.systemLocation();
        QFileInfo fi(portPath);
        QString ttyName = fi.fileName(); // 例如 ttyUSB0, ttyACM0, ttyS0
        
        // 过滤掉传统的 /dev/ttyS* 串口（这些通常是虚拟的或未使用的串口）
        // 只保留 USB 串口（ttyUSB*, ttyACM*）和真实连接的串口
        if (ttyName.startsWith("ttyS"))
        {
            // 对于 ttyS* 串口，只有当它有 /dev/serial/by-id 链接时才保留
            // 因为只有真实连接的 USB 串口设备才会有这个链接
            QStringList byIdLinks = getByIdLinksForTty(ttyName);
            if (byIdLinks.isEmpty())
            {
                // 没有 by-id 链接的 ttyS* 串口，很可能是虚拟的，跳过
                continue;
            }
        }
        
        // 使用系统路径，便于直接设置到 INDI 设备端口，例如 /dev/ttyUSB0
        // 不再强制尝试打开端口，以免过滤掉权限/占用导致暂时无法打开但仍可被用户选择的端口
        activeSerialPortNames.append(portPath);
    }
    return activeSerialPortNames;
}

QString MainWindow::resolveSerialPort(const QString &symbolicLink)
{
    QFileInfo fileInfo(symbolicLink);
    if (fileInfo.isSymLink())
    {
        QString target = fileInfo.symLinkTarget();
        Logger::Log("ResolveSerialPort | real port path:" + target.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        return target;
    }
    else
    {
        Logger::Log("ResolveSerialPort | provided path is not a symbolic link", LogLevel::WARNING, DeviceType::MAIN);
        return QString();
    }
}

void MainWindow::sendSerialPortOptions(const QString &driverType)
{
    if (!wsThread)
        return;

    // 仅支持 Mount / Focuser 两类串口设备
    if (driverType != "Mount" && driverType != "Focuser")
        return;

    // 当前可用串口路径列表（全部是真实存在的串口节点）
    QStringList ports = getConnectedSerialPorts();

    // 当前设备正在使用的串口（若已连接），或前端最近一次选择的覆盖串口
    QString currentPort;
    if (driverType == "Mount")
    {
        if (dpMount != nullptr)
        {
            indi_Client->getDevicePort(dpMount, currentPort);
        }
        if (currentPort.isEmpty())
        {
            currentPort = mountSerialPortOverride;
        }
    }
    else if (driverType == "Focuser")
    {
        if (dpFocuser != nullptr)
        {
            indi_Client->getDevicePort(dpFocuser, currentPort);
        }
        if (currentPort.isEmpty())
        {
            currentPort = focuserSerialPortOverride;
        }
    }

    // 组装带“真实路径 -> 友好名称(by-id)”的项：
    // 每一项格式为：<portPath>-><displayName>，前端再解析
    QString payload = "SerialPortOptions:" + driverType + ":" + currentPort;
    for (const QString &p : ports)
    {
        QString displayName = p;
        QFileInfo fi(p);
        QString ttyName = fi.fileName(); // 例如 ttyUSB0

        // 若能找到 /dev/serial/by-id 的符号链接，则使用其文件名作为显示名
        QStringList byIdLinks = getByIdLinksForTty(ttyName);
        if (!byIdLinks.isEmpty())
        {
            QFileInfo linkInfo(byIdLinks.first());
            displayName = linkInfo.fileName(); // 只显示 by-id 的名字，更易识别设备
        }

        payload += ":" + p + "->" + displayName;
    }

    Logger::Log("sendSerialPortOptions | " + payload.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
    emit wsThread->sendMessageToClient(payload);
}

QStringList MainWindow::findLinkToTtyDevice(const QString &directoryPath, const QString &ttyDevice)
{
    QString targetDevice = "/dev/" + ttyDevice; // 构建完整的设备路径
    QStringList foundLinks;

    // 使用 QDirIterator 递归遍历目录和子目录
    QDirIterator it(directoryPath, QDir::Files | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        it.next();
        QFileInfo fileInfo = it.fileInfo();
        if (fileInfo.isSymLink())
        {
            QString target = fileInfo.symLinkTarget();
            // 如果符号链接是相对路径，需要将其转换为绝对路径
            if (QDir::isRelativePath(target))
            {
                target = fileInfo.absoluteDir().absoluteFilePath(target);
            }
            // 检查符号链接的目标是否是指定的 tty 设备
            if (target == targetDevice)
            {
                foundLinks.append(fileInfo.absoluteFilePath());
                Logger::Log("FindLinkToTtyDevice | found link:" + fileInfo.absoluteFilePath().toStdString() + " -> " + target.toStdString(), LogLevel::INFO, DeviceType::MAIN);
            }
        }
    }
    // 返回找到的所有符号链接
    return foundLinks;
}

bool MainWindow::areFilesInSameDirectory(const QString &path1, const QString &path2)
{
    QFileInfo fileInfo1(path1);
    QFileInfo fileInfo2(path2);

    // 获取两个文件的目录路径
    QString dir1 = fileInfo1.absolutePath();
    QString dir2 = fileInfo2.absolutePath();

    // 比较目录路径是否相同
    return (dir1 == dir2);
}

QStringList MainWindow::getByIdLinksForTty(const QString &ttyDevice)
{
    QStringList results;
    QString baseDir = "/dev/serial/by-id";
    QDir dir(baseDir);
    if (!dir.exists())
    {
        return results;
    }

    QFileInfoList entryList = dir.entryInfoList(QDir::Files | QDir::System | QDir::NoDotAndDotDot);
    QString targetDevice = "/dev/" + ttyDevice;
    for (const QFileInfo &entry : entryList)
    {
        if (!entry.isSymLink())
        {
            continue;
        }
        QString target = entry.symLinkTarget();
        if (QDir::isRelativePath(target))
        {
            // 归一化相对路径为绝对路径
            target = entry.absoluteDir().absoluteFilePath(target);
        }
        QString normalizedTarget = QDir::cleanPath(target);
        if (normalizedTarget == targetDevice)
        {
            results.append(entry.absoluteFilePath());
            Logger::Log("getByIdLinksForTty | found by-id link:" + entry.absoluteFilePath().toStdString() + " -> " + normalizedTarget.toStdString(), LogLevel::INFO, DeviceType::MAIN);
        }
    }
    return results;
}

static int scoreByIdLinkForType(const QString &fileNameLower, const QString &driverType)
{
    // 简单关键字打分，匹配越多分越高
    int score = 0;
    if (driverType == "Focuser")
    {
        if (fileNameLower.contains("gigadevice")) score += 2;
        if (fileNameLower.contains("gd32")) score += 2;
        if (fileNameLower.contains("cdc_acm")) score += 1;
        if (fileNameLower.contains("acm")) score += 1;
    }
    else if (driverType == "Mount")
    {
        if (fileNameLower.contains("1a86")) score += 2;          // WCH/CH34x VID
        if (fileNameLower.contains("usb_serial")) score += 2;    // CH34x 常见 by-id
        if (fileNameLower.contains("ch34")) score += 2;
        if (fileNameLower.contains("wch")) score += 1;
        if (fileNameLower.contains("ttyusb")) score += 1;        // 兜底弱信号
    }
    return score;
}

bool MainWindow::isByIdLinkForDriverType(const QString &symlinkPath, const QString &driverType)
{
    QFileInfo fi(symlinkPath);
    QString nameLower = fi.fileName().toLower();
    return scoreByIdLinkForType(nameLower, driverType) > 0;
}

QString MainWindow::selectBestByIdLink(const QStringList &links, const QString &driverType)
{
    int bestScore = -1;
    QString best;
    for (const QString &link : links)
    {
        QFileInfo fi(link);
        QString nameLower = fi.fileName().toLower();
        int s = scoreByIdLinkForType(nameLower, driverType);
        if (s > bestScore)
        {
            bestScore = s;
            best = link;
        }
    }
    return best;
}

void MainWindow::onParseInfoEmitted(const QString &message)
{
    emit wsThread->sendMessageToClient("ParseInfoEmitted:" + message);
}

void MainWindow::disconnectDevice(const QString &deviceName, const QString &description)
{
    for (int i = 0; i < indi_Client->GetDeviceCount(); i++)
    {
        if (indi_Client->GetDeviceFromList(i)->getDeviceName() == deviceName)
        {
            indi_Client->disconnectDevice(deviceName.toStdString().c_str());
            int num = 0;
            bool disconnectSuccess = true;
  
            Logger::Log(deviceName.toStdString() + " disconnected successfully.", LogLevel::INFO, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("DisconnectDriverSuccess:" + description);
            emit wsThread->sendMessageToClient("deleteDeviceAllocationList:" + deviceName);

            // 若为赤道仪或电调，在断开后提示前端弹出串口选择 UI，方便下次连接前重新匹配
            if (description == "Mount" || description == "Focuser")
            {
                sendSerialPortOptions(description);
                emit wsThread->sendMessageToClient("RequestSerialPortSelection:" + description);
            }
            break;
        }
    }
}

void MainWindow::disconnectDriver(QString Driver)
{
    Logger::Log("Starting to disconnect driver: " + Driver.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    // 先收集要断开的设备（避免 DisconnectDevice 修改 systemdevicelist 导致迭代器/引用风险）
    QVector<QPair<QString, QString>> toDisconnect;
    for (const auto &dev : systemdevicelist.system_devices)
    {
        if (!dev.Description.isEmpty() && dev.DriverIndiName == Driver && dev.isConnect)
        {
            toDisconnect.push_back(qMakePair(dev.DeviceIndiName, dev.Description));
        }
    }

    for (const auto &item : toDisconnect)
    {
        const QString &devName = item.first;
        const QString &devType = item.second;

        // 断开前中止主相机曝光，避免断开过程中卡住
        if (devType == "MainCamera" && glMainCameraStatu == "Exposuring")
        {
            abortMainCameraCapture();
        }

        // 统一走 DisconnectDevice：它同时覆盖 INDI 与 SDK 模式清理
        DisconnectDevice(indi_Client, devName, devType);

        // 清理 ConnectedDevices 中对应项
        for (auto it = ConnectedDevices.begin(); it != ConnectedDevices.end();)
        {
            if (it->DeviceType == devType)
            {
                it = ConnectedDevices.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // 若这是一个 SDK 相关驱动（systemdevicelist 中该驱动对应的槽位被标记为 isSDKConnect），
    // 则需要同步清理 SDK 资源，避免“断开驱动后 SDK 仍占用/残留线程访问”。
    //
    // 注意：cleanupQhySdkPoolAndResource 的 deviceType 仅支持：
    // - "CameraPool"（关闭所有相机句柄 + ReleaseSdkResource）
    // - "MainCamera"（仅关闭主相机句柄，不释放全局资源）
    // - "Focuser"（关闭电调句柄）
    // - "All"
    //
    // 断开驱动场景应优先清理 CameraPool（因为同一驱动可能同时被 MainCamera/Guider 共用）。
    bool needsCameraPool = false;
    bool needsFocuser = false;
    bool sdkRelated = false;
    for (const auto &dev : systemdevicelist.system_devices)
    {
        if (!dev.isSDKConnect)
            continue;
        if (dev.DriverIndiName.isEmpty() || dev.DriverIndiName != Driver)
            continue;

        sdkRelated = true;
        if (dev.Description == "Focuser")
            needsFocuser = true;
        else if (dev.Description == "MainCamera" || dev.Description == "Guider" || dev.Description == "PoleCamera")
            needsCameraPool = true;
    }

    if (sdkRelated || Driver.contains("SDK", Qt::CaseInsensitive))
    {
        if (needsFocuser)
            cleanupQhySdkPoolAndResource("disconnectDriver:" + Driver, "Focuser");
        if (needsCameraPool)
            cleanupQhySdkPoolAndResource("disconnectDriver:" + Driver, "CameraPool");
    }

    Tools::stopIndiDriver(Driver);
    int index = ConnectDriverList.indexOf(Driver);
    if (index != -1)
    {                                      // 如果找到了
        ConnectDriverList.removeAt(index); // 从列表中删除
        Logger::Log("Driver removed successfully: " + Driver.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        Logger::Log("Driver not found in list: " + Driver.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
    }

    Logger::Log("Driver disconnected: " + Driver.toStdString(), LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::bin_image(double *input, long width, long height, double *output, long *out_w, long *out_h)
{
    *out_w = width / BIN_SIZE;
    *out_h = height / BIN_SIZE;

    for (long y = 0; y < *out_h; y++)
    {
        for (long x = 0; x < *out_w; x++)
        {
            double sum = 0.0;
            for (int dy = 0; dy < BIN_SIZE; dy++)
            {
                for (int dx = 0; dx < BIN_SIZE; dx++)
                {
                    long ix = x * BIN_SIZE + dx;
                    long iy = y * BIN_SIZE + dy;
                    sum += input[iy * width + ix];
                }
            }
            output[y * (*out_w) + x] = sum / (BIN_SIZE * BIN_SIZE);
        }
    }
}

void MainWindow::process_hdu(fitsfile *infptr, fitsfile *outfptr, int hdunum, int *status)
{
    fits_movabs_hdu(infptr, hdunum, NULL, status);

    int bitpix, naxis;
    long naxes[2] = {1, 1};
    fits_get_img_param(infptr, 2, &bitpix, &naxis, naxes, status);

    if (naxis != 2)
    {
        printf("HDU %d skipped (not 2D image).\n", hdunum);
        return;
    }

    long width = naxes[0], height = naxes[1];
    long npixels = width * height;
    double *img = (double *)malloc(npixels * sizeof(double));
    if (!img)
    {
        printf("Memory error.\n");
        exit(1);
    }

    long fpixel[2] = {1, 1};
    fits_read_pix(infptr, TDOUBLE, fpixel, npixels, NULL, img, NULL, status);

    long out_w, out_h;
    long dims[2] = {out_w, out_h};
    long out_pixels = (width / BIN_SIZE) * (height / BIN_SIZE);
    double *binned = (double *)malloc(out_pixels * sizeof(double));
    bin_image(img, width, height, binned, &out_w, &out_h);

    // 创建输出图像
    fits_create_img(outfptr, DOUBLE_IMG, 2, dims, status);
    fits_write_img(outfptr, TDOUBLE, 1, out_w * out_h, binned, status);

    free(img);
    free(binned);
}
int MainWindow::process_fixed()
{
    const char *infile = "/dev/shm/ccd_simulator_original.fits"; // 输入文件路径
    const char *outfile = "!/dev/shm/ccd_simulator_binned.fits"; // 输出文件路径
    // const char *outfile = "!merged_output.fits";  // 带 '!' 前缀，自动覆盖

    fitsfile *infptr = NULL, *outfptr = NULL;
    int status = 0, hdunum = 0, hdutype = 0;

    fits_open_file(&infptr, infile, READONLY, &status);
    if (status)
    {
        fits_report_error(stderr, status);
        return status;
    }

    fits_create_file(&outfptr, outfile, &status);
    if (status)
    {
        fits_report_error(stderr, status);
        fits_close_file(infptr, &status);
        return status;
    }

    fits_get_num_hdus(infptr, &hdunum, &status);
    for (int i = 1; i <= hdunum && status == 0; i++)
    {
        fits_movabs_hdu(infptr, i, &hdutype, &status);
        if (hdutype == IMAGE_HDU)
        {
            process_hdu(infptr, outfptr, i, &status);
        }
        else
        {
            fits_copy_hdu(infptr, outfptr, 0, &status);
        }
    }

    fits_close_file(infptr, &status);
    fits_close_file(outfptr, &status);

    if (status)
    {
        fits_report_error(stderr, status);
        return status;
    }

    printf("合并覆盖完成：ccd_simulator_binned.fits\n");
    return 0;
}


void MainWindow::sendRoiInfo()
{
    Logger::Log("==========================================", LogLevel::INFO, DeviceType::FOCUSER);
    for (auto it = roiAndFocuserInfo.begin(); it != roiAndFocuserInfo.end(); ++it)
    {
        Logger::Log("roiAndFocuserInfo | Key:" + it->first + " Value:" + std::to_string(it->second), LogLevel::INFO, DeviceType::FOCUSER);
    }
    Logger::Log("==========================================", LogLevel::INFO, DeviceType::FOCUSER);
    // 检查并获取参数，如果不存在则使用默认值
    double boxSideLength = roiAndFocuserInfo.count("BoxSideLength") ? roiAndFocuserInfo["BoxSideLength"] : 300;
    double roi_x = roiAndFocuserInfo.count("ROI_x") ? roiAndFocuserInfo["ROI_x"] : 1;
    double roi_y = roiAndFocuserInfo.count("ROI_y") ? roiAndFocuserInfo["ROI_y"] : 1;
    double visibleX = roiAndFocuserInfo.count("VisibleX") ? roiAndFocuserInfo["VisibleX"] : 0;
    double visibleY = roiAndFocuserInfo.count("VisibleY") ? roiAndFocuserInfo["VisibleY"] : 0;
    double scale = roiAndFocuserInfo.count("scale") ? roiAndFocuserInfo["scale"] : 1;
    double selectStarX = roiAndFocuserInfo.count("SelectStarX") ? roiAndFocuserInfo["SelectStarX"] : -1;
    double selectStarY = roiAndFocuserInfo.count("SelectStarY") ? roiAndFocuserInfo["SelectStarY"] : -1;
    const QPointF snapped = snapRoiOriginToBayerSafePhase(roi_x, roi_y,
                                                          static_cast<int>(std::lround(boxSideLength)),
                                                          static_cast<int>(std::lround(boxSideLength)));
    roi_x = snapped.x();
    roi_y = snapped.y();
    roiAndFocuserInfo["ROI_x"] = roi_x;
    roiAndFocuserInfo["ROI_y"] = roi_y;

    Logger::Log("sendRoiInfo | 发送参数 roi_x:" + std::to_string(roi_x) + " roi_y:" + std::to_string(roi_y) + " boxSideLength:" + std::to_string(boxSideLength) + " visibleX:" + std::to_string(visibleX) + " visibleY:" + std::to_string(visibleY) + " scale:" + std::to_string(scale) + " selectStarX:" + std::to_string(selectStarX) + " selectStarY:" + std::to_string(selectStarY), LogLevel::INFO, DeviceType::FOCUSER);

    // 发送参数
    emit wsThread->sendMessageToClient("SetRedBoxState:" + QString::number(boxSideLength) + ":" + QString::number(roi_x) + ":" + QString::number(roi_y));
    emit wsThread->sendMessageToClient("SetVisibleArea:" + QString::number(visibleX) + ":" + QString::number(visibleY) + ":" + QString::number(scale));
    emit wsThread->sendMessageToClient("SetSelectStars:" + QString::number(selectStarX) + ":" + QString::number(selectStarY));
}
void MainWindow::updateCPUInfo()
{
    // 获取CPU温度和使用率
    QProcess process;
    // 在树莓派上，可以通过读取 /sys/class/thermal/thermal_zone0/temp 文件来获取 CPU 温度
    process.start("cat", QStringList() << "/sys/class/thermal/thermal_zone0/temp");
    process.waitForFinished();
    QString output = process.readAllStandardOutput();
    float cpuTemp = output.toFloat() / 1000; // 转换为摄氏度
    if (process.error() != QProcess::UnknownError)
    {
        cpuTemp = std::numeric_limits<float>::quiet_NaN(); // 如果获取失败，设置为 NaN
    }

    // 在树莓派上，可以通过运行 'top' 命令并解析输出来获取 CPU 使用率
    process.start("sh", QStringList() << "-c" << "top -b -n1 | grep 'Cpu(s)' | awk '{print $2}' | cut -c 1-4");
    process.waitForFinished();
    output = process.readAllStandardOutput();
    QStringList cpuUsages = output.split("\n");
    float cpuUsage = 0;
    if (cpuUsages.size() > 0)
    {
        cpuUsage = cpuUsages[0].toDouble();
    }
    if (process.error() != QProcess::UnknownError)
    {
        cpuUsage = std::numeric_limits<float>::quiet_NaN(); // 如果获取失败，设置为 NaN
    }

    // Logger::Log("updateCPUInfo | CPU Temp: " + std::to_string(cpuTemp) + ", CPU Usage: " + std::to_string(cpuUsage), LogLevel::DEBUG, DeviceType::MAIN);
    emit wsThread->sendMessageToClient("updateCPUInfo:" + QString::number(cpuTemp) + ":" + QString::number(cpuUsage));
}

void MainWindow::getMainCameraParameters()
{
    Logger::Log("getMainCameraParameters start ...", LogLevel::DEBUG, DeviceType::MAIN);
    QMap<QString, QString> parameters = Tools::readParameters("MainCamera");
    QString order = "setMainCameraParameters";
    bool hasTileBuildMode = false;
    bool hasTileLevelMode = false;
    bool hasImageCfa = false;
    bool hasRoiCalcMode = false;
    for (auto it = parameters.begin(); it != parameters.end(); ++it)
    {
        Logger::Log("getMainCameraParameters | " + it.key().toStdString() + ":" + it.value().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        if (it.key() == "ImageCFA") {
            hasImageCfa = true;
            const QString normalizedCfa = normalizeCfaPattern(it.value());
            MainCameraCFA = (normalizedCfa == "NULL" || normalizedCfa == "MONO") ? QString() : normalizedCfa;
            it.value() = MainCameraCFA.isEmpty() ? QStringLiteral("null") : MainCameraCFA;
        }
        if (it.key() == "ROICalcMode") {
            hasRoiCalcMode = true;
            const QString mode = it.value().trimmed().toLower();
            roiUseSelfCalcParams = (mode == "roi" || mode == "self");
            it.value() = roiUseSelfCalcParams ? QStringLiteral("roi") : QStringLiteral("full");
        }
        if (it.key() == "Save Folder" ) {
            QString oldSaveFolder = it.value();
            // 兼容旧的"default"，转换为"local"
            if (oldSaveFolder == "default") {
                oldSaveFolder = "local";
                it.value() = "local";
            }
            
            if (oldSaveFolder == "local") {
                ImageSaveBaseDirectory = QString::fromStdString(ImageSaveBasePath);
                saveMode = "local";
            } else if (usbMountPointsMap.contains(oldSaveFolder)) {
                ImageSaveBaseDirectory = usbMountPointsMap[oldSaveFolder] + "/QUARCS_ImageSave";
                saveMode = oldSaveFolder;
            } else {
                // U盘不存在，回退到本地
                it.value() = "local";
                ImageSaveBaseDirectory = QString::fromStdString(ImageSaveBasePath);
                saveMode = "local";
                Logger::Log("LoadParameter | USB '" + oldSaveFolder.toStdString() + "' not found, using local", LogLevel::WARNING, DeviceType::MAIN);
            }
        }
        if (it.key() == "Tile Build Mode") {
            if (it.value() == "merged_single_level") {
                Logger::Log("LoadParameter | Tile Build Mode 'merged_single_level' is deprecated, forcing pyramid",
                            LogLevel::WARNING, DeviceType::MAIN);
            }
            tileBuildMode = QStringLiteral("pyramid");
            it.value() = tileBuildMode;
            hasTileBuildMode = true;
        }
        if (it.key() == "Tile Level Mode") {
            const QString requested = it.value().trimmed().toLower();
            tileLevelMode = (requested == QStringLiteral("minmax")) ? QStringLiteral("minmax") : QStringLiteral("full");
            it.value() = tileLevelMode;
            hasTileLevelMode = true;
        }
        order += ":" + it.key() + ":" + it.value();
        if (it.key() == "RedBoxSize") {
            BoxSideLength = it.value().toInt();
            roiAndFocuserInfo["BoxSideLength"] = BoxSideLength;
        }
        if (it.key() == "ROI_x") roiAndFocuserInfo["ROI_x"] = it.value().toDouble();
        if (it.key() == "ROI_y") roiAndFocuserInfo["ROI_y"] = it.value().toDouble();
        if (it.key() == "AutoSave") {
            mainCameraAutoSave = (it.value() == "true");
            // Logger::Log("/*/*/*/*/*/*getMainCameraParameters | AutoSave: " + std::to_string(mainCameraAutoSave), LogLevel::DEBUG, DeviceType::MAIN);
        }
        if (it.key() == "SaveFailedParse") {
            mainCameraSaveFailedParse = (it.value() == "true");
        }
        if (it.key() == "Temperature") {
            CameraTemperature = it.value().toDouble();
        }
        if (it.key() == "Gain") {
            CameraGain = it.value().toInt();
        }
        if (it.key() == "Offset") {
            ImageOffset = it.value().toDouble();
        }
        if (it.key() == "USB Traffic") {
            glUsbTrafficValue = it.value().toInt();
        }
    }
    if (!hasTileBuildMode) {
        tileBuildMode = QStringLiteral("pyramid");
        order += ":Tile Build Mode:" + tileBuildMode;
    }
    if (!hasTileLevelMode) {
        tileLevelMode = QStringLiteral("full");
        order += ":Tile Level Mode:" + tileLevelMode;
    }
    if (!hasRoiCalcMode) {
        roiUseSelfCalcParams = false;
        order += ":ROICalcMode:full";
    }
    if (!hasImageCfa) {
        MainCameraCFA = normalizeCfaPattern(MainCameraCFA);
        if (MainCameraCFA == "NULL" || MainCameraCFA == "MONO") {
            MainCameraCFA.clear();
        }
    }
    Logger::Log("getMainCameraParameters finish!", LogLevel::DEBUG, DeviceType::MAIN);
    emit wsThread->sendMessageToClient(order);

    emit wsThread->sendMessageToClient("MainCameraCFA:" + (MainCameraCFA.isEmpty() ? QStringLiteral("null") : MainCameraCFA));
    emit wsThread->sendMessageToClient("MainCameraCFASource:SAVED");
}

void MainWindow::getMountParameters()
{
    Logger::Log("getMountUiInfo ...", LogLevel::DEBUG, DeviceType::MAIN);
    QMap<QString, QString> parameters = Tools::readParameters("Mount");
    for (auto it = parameters.begin(); it != parameters.end(); ++it)
    {
        Logger::Log("getMountParameters | " + it.key().toStdString() + ":" + it.value().toStdString(), LogLevel::DEBUG, DeviceType::MAIN);
        if (it.key() == "AutoFlip"){
            emit wsThread->sendMessageToClient("AutoFlip:" + it.value());
            isAutoFlip = it.value() == "true";
            continue;
        }
        if (it.key() == "GotoThenSolve"){
            emit wsThread->sendMessageToClient("GotoThenSolve:" + it.value());
            GotoThenSolve = it.value() == "true";
            continue;
        }
        emit wsThread->sendMessageToClient(it.key() + ":" + it.value());
    }

    // 将当前 Mount 串口列表与已保存串口下发给前端（若无保存则 savedPort 为空）
    sendSerialPortOptions("Mount");

    Logger::Log("getMountParameters finish!", LogLevel::DEBUG, DeviceType::MAIN);
}

void MainWindow::synchronizeTime(QString time, QString date)
{
    Logger::Log("synchronizeTime start ...", LogLevel::DEBUG, DeviceType::MAIN);
    Logger::Log("synchronizeTime time: " + time.toStdString() + ", date: " + date.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);

    // 仅首次对时禁用自动时间同步，刷新/重复对时不再执行以缩短耗时（约 2s）
    static bool automaticTimeSyncDisabled = false;
    if (!automaticTimeSyncDisabled)
    {
        Logger::Log("Disabling automatic time synchronization...", LogLevel::DEBUG, DeviceType::MAIN);
        int disableResult1 = system("sudo systemctl stop systemd-timesyncd");
        int disableResult2 = system("sudo systemctl disable systemd-timesyncd");
        int disableResult3 = system("sudo timedatectl set-ntp false");
        if (disableResult1 != 0 || disableResult2 != 0 || disableResult3 != 0)
            Logger::Log("Warning: Failed to disable some automatic time sync services", LogLevel::WARNING, DeviceType::MAIN);
        else
            Logger::Log("Automatic time synchronization disabled successfully", LogLevel::DEBUG, DeviceType::MAIN);
        automaticTimeSyncDisabled = true;
        QThread::msleep(300);   // 缩短等待，原 1000ms 易在刷新时拖慢
    }

    // Create the command string
    QString command = "sudo date -s \"" + date + " " + time + "\"";

    Logger::Log("synchronizeTime command: " + command.toStdString(), LogLevel::DEBUG, DeviceType::MAIN);

    // Execute the command
    int result = system(command.toStdString().c_str());

    if (result == 0)
    {
        Logger::Log("synchronizeTime finish!", LogLevel::DEBUG, DeviceType::MAIN);
    }
    else
    {
        Logger::Log("synchronizeTime failed!", LogLevel::ERROR, DeviceType::MAIN);
    }
}

void MainWindow::setMountLocation(QString lat, QString lon)
{
    Logger::Log("setMountLocation start ...", LogLevel::DEBUG, DeviceType::MAIN);
    observatorylatitude = lat.toDouble();
    observatorylongitude = lon.toDouble();
    if (indi_Client != nullptr)
        indi_Client->mountState.updateHomeRAHours(observatorylatitude, observatorylongitude);
    // 仅当 Mount 已连接时才下发 INDI setLocation，避免未连接时 3s 超时拖慢刷新
    if (dpMount == nullptr || !dpMount->isConnected())
    {
        Logger::Log("setMountLocation | Mount not connected, skip INDI setLocation", LogLevel::DEBUG, DeviceType::MAIN);
        return;
    }
    indi_Client->setLocation(dpMount, observatorylatitude, observatorylongitude, 50);
}

void MainWindow::setMountUTC(QString time, QString date)
{
    Logger::Log("setMountUTC start ...", LogLevel::DEBUG, DeviceType::MAIN);
    if (dpMount == nullptr || !dpMount->isConnected())
    {
        Logger::Log("setMountUTC | Mount not connected, skip INDI time sync (avoid 3s timeout)", LogLevel::WARNING, DeviceType::MAIN);
        return;
    }
    QDateTime datetime = QDateTime::fromString(date + "T" + time, Qt::ISODate);
    indi_Client->setTimeUTC(dpMount, datetime);
    Logger::Log("UTC Time set for Mount: " + datetime.toString(Qt::ISODate).toStdString(), LogLevel::INFO, DeviceType::MAIN);
    indi_Client->getTimeUTC(dpMount, datetime);
    Logger::Log("UTC Time: " + datetime.currentDateTimeUtc().toString(Qt::ISODate).toStdString(), LogLevel::INFO, DeviceType::MAIN);
}

void MainWindow::getLastSelectDevice()
{
    SystemDeviceList newSystemdevicelist = Tools::readSystemDeviceList();

    // 检查是否有历史设备配置记录
    bool hasHistoryConfig = false;
    for (int i = 0; i < newSystemdevicelist.system_devices.size(); i++)
    {
        if (!newSystemdevicelist.system_devices[i].DriverIndiName.isEmpty() &&
            !newSystemdevicelist.system_devices[i].Description.isEmpty())
        {
            hasHistoryConfig = true;
            break;
        }
    }

    if (!hasHistoryConfig)
    {
        Logger::Log("No historical connection records found", LogLevel::ERROR, DeviceType::MAIN);
        return;
    }

    // 只有当未连接设备时才触发设备选择
    if (ConnectedDevices.size() == 0)
    {
        systemdevicelist = newSystemdevicelist;
        loadSelectedDriverList();
        Logger::Log("Last Connected Device Has Send", LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        Logger::Log("Devices already connected, skip loading last selection", LogLevel::INFO, DeviceType::MAIN);
    }
}

/** 自动极轴校准 */
bool MainWindow::initPolarAlignment(PolarAlignmentCameraRole role)
{
    const bool mountConnected = (dpMount != nullptr) || isMountSDK();
    if (!mountConnected)
    {
        Logger::Log("initPolarAlignment | mount is not connected", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(
            "StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,Mount must be connected");
        return false;
    }

    if (role == PolarAlignmentCameraRole::MainCamera && !isMainCameraConnected())
    {
        Logger::Log("initPolarAlignment | main camera is selected but not connected", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(
            "StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,MainCamera is not connected");
        return false;
    }

    if (role == PolarAlignmentCameraRole::Guider && !isGuiderCameraConnected())
    {
        Logger::Log("initPolarAlignment | guider is selected but not connected", LogLevel::ERROR, DeviceType::GUIDER);
        emit wsThread->sendMessageToClient(
            "StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,Guider is not connected");
        return false;
    }

    if (role == PolarAlignmentCameraRole::PoleCamera && !isPoleCameraConnected())
    {
        Logger::Log("initPolarAlignment | pole camera is selected but not connected", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(
            "StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,PoleCamera is not connected");
        return false;
    }

    if (isMountSDK() && dpMount == nullptr)
    {
        Logger::Log("initPolarAlignment | SDK模式赤道仪暂不支持，请使用INDI模式", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,SDK模式赤道仪暂不支持");
        return false;
    }
    if (indi_Client == nullptr)
    {
        Logger::Log("initPolarAlignment | indi_Client is nullptr", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,indi_Client is nullptr");
        return false;
    }

    int selectedFocalLength = 0;
    if (role == PolarAlignmentCameraRole::MainCamera)
    {
        selectedFocalLength = getMainCameraFocalLengthFromConfigAndMigrateIfNeeded();
        if (selectedFocalLength <= 0)
        {
            Logger::Log("initPolarAlignment | MainCameraFocalLength is not set", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,focal length is not set");
            return false;
        }
    }
    else if (role == PolarAlignmentCameraRole::Guider)
    {
        double guiderFocal = guiderFocalLengthMm;
        if (guiderFocal <= 0.0)
        {
            std::unordered_map<std::string, std::string> config;
            Tools::readClientSettings("config/config.ini", config);
            auto it = config.find("GuiderFocalLength");
            if (it != config.end())
            {
                bool ok = false;
                const double v = QString::fromStdString(it->second).trimmed().toDouble(&ok);
                if (ok && v > 0.0)
                    guiderFocal = v;
            }
        }
        if (guiderFocal <= 0.0)
        {
            Logger::Log("initPolarAlignment | GuiderFocalLength is not set", LogLevel::WARNING, DeviceType::GUIDER);
            emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,guider focal length is not set");
            return false;
        }
        guiderFocalLengthMm = guiderFocal;
        selectedFocalLength = static_cast<int>(std::lround(guiderFocal));
    }
    else
    {
        double poleFocal = 0.0;
        std::unordered_map<std::string, std::string> config;
        Tools::readClientSettings("config/config.ini", config);
        auto it = config.find("PoleCameraFocalLength");
        if (it != config.end())
        {
            bool ok = false;
            const double v = QString::fromStdString(it->second).trimmed().toDouble(&ok);
            if (ok && v > 0.0)
                poleFocal = v;
        }
        if (poleFocal <= 0.0)
        {
            Logger::Log("initPolarAlignment | PoleCameraFocalLength is not set", LogLevel::WARNING, DeviceType::MAIN);
            emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,pole camera focal length is not set");
            return false;
        }
        selectedFocalLength = static_cast<int>(std::lround(poleFocal));
    }

    if (!isValidObservatoryLocation(observatorylatitude, observatorylongitude))
    {
        bool latOk = false;
        bool lonOk = false;
        const double cachedLat = localLat.trimmed().toDouble(&latOk);
        const double cachedLon = localLon.trimmed().toDouble(&lonOk);
        if (latOk && lonOk && isValidObservatoryLocation(cachedLat, cachedLon))
        {
            observatorylatitude = cachedLat;
            observatorylongitude = cachedLon;
            if (indi_Client != nullptr)
                indi_Client->mountState.updateHomeRAHours(observatorylatitude, observatorylongitude);
            Logger::Log("initPolarAlignment | using MainWindow localLat/localLon fallback - Latitude: " +
                            QString::number(observatorylatitude).toStdString() +
                            ", Longitude: " + QString::number(observatorylongitude).toStdString(),
                        LogLevel::INFO, DeviceType::MAIN);
        }
    }

    if (!isValidObservatoryLocation(observatorylatitude, observatorylongitude))
    {
        Logger::Log("initPolarAlignment | observatorylatitude or observatorylongitude is invalid", LogLevel::WARNING, DeviceType::MAIN);
    }

    double cameraWidthMm = 0.0;
    double cameraHeightMm = 0.0;
    INDI::BaseDevice *captureDevice = nullptr;
    bool useSdkCaptureSource = false;
    QString captureRoleName = "MainCamera";

    if (role == PolarAlignmentCameraRole::MainCamera)
    {
        captureDevice = dpMainCamera;
        useSdkCaptureSource = isMainCameraSDK();
        captureRoleName = "MainCamera";
        cameraWidthMm = glCameraSize_width;
        cameraHeightMm = glCameraSize_height;

        if (cameraWidthMm <= 0 || cameraHeightMm <= 0)
        {
            if (!isMainCameraSDK() && dpMainCamera != nullptr)
            {
                double pixelsize, pixelsizX, pixelsizY;
                int maxX, maxY, bitDepth;
                indi_Client->getCCDBasicInfo(dpMainCamera, maxX, maxY, pixelsize, pixelsizX, pixelsizY, bitDepth);
                cameraWidthMm = maxX * pixelsize / 1000.0;
                cameraHeightMm = maxY * pixelsize / 1000.0;
                cameraWidthMm = std::round(cameraWidthMm * 10.0) / 10.0;
                cameraHeightMm = std::round(cameraHeightMm * 10.0) / 10.0;
                glCameraSize_width = cameraWidthMm;
                glCameraSize_height = cameraHeightMm;
            }
            else if (isMainCameraSDK() && sdkMainCameraHandle != nullptr)
            {
                SdkCommand chipInfoCmd;
                chipInfoCmd.type = SdkCommandType::Custom;
                chipInfoCmd.name = "GetChipInfo";
                chipInfoCmd.payload = std::any();
                SdkResult chipInfoRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, chipInfoCmd);

                if (chipInfoRes.success)
                {
                    try
                    {
                        SdkChipInfo chipInfo = std::any_cast<SdkChipInfo>(chipInfoRes.payload);
                        glMainCCDSizeX = chipInfo.maxImageSizeX;
                        glMainCCDSizeY = chipInfo.maxImageSizeY;
                        cameraWidthMm = chipInfo.chipWidthMM;
                        cameraHeightMm = chipInfo.chipHeightMM;
                        glCameraSize_width = cameraWidthMm;
                        glCameraSize_height = cameraHeightMm;
                    }
                    catch (const std::bad_any_cast &e)
                    {
                        Logger::Log("initPolarAlignment | SDK GetChipInfo bad_any_cast: " + std::string(e.what()),
                                    LogLevel::ERROR, DeviceType::MAIN);
                    }
                }
                else
                {
                    Logger::Log("initPolarAlignment | SDK GetChipInfo failed: " + chipInfoRes.message,
                                LogLevel::WARNING, DeviceType::MAIN);
                }
            }
        }
    }
    else if (role == PolarAlignmentCameraRole::Guider)
    {
        captureDevice = dpGuider;
        useSdkCaptureSource = isGuiderCameraSDK();
        captureRoleName = "Guider";

        if (!isGuiderCameraSDK() && dpGuider != nullptr)
        {
            double pixelsize, pixelsizX, pixelsizY;
            int maxX, maxY, bitDepth;
            indi_Client->getCCDBasicInfo(dpGuider, maxX, maxY, pixelsize, pixelsizX, pixelsizY, bitDepth);
            cameraWidthMm = maxX * pixelsize / 1000.0;
            cameraHeightMm = maxY * pixelsize / 1000.0;
            cameraWidthMm = std::round(cameraWidthMm * 10.0) / 10.0;
            cameraHeightMm = std::round(cameraHeightMm * 10.0) / 10.0;
        }
        else if (isGuiderCameraSDK() && sdkGuiderHandle != nullptr)
        {
            SdkCommand chipInfoCmd;
            chipInfoCmd.type = SdkCommandType::Custom;
            chipInfoCmd.name = "GetChipInfo";
            chipInfoCmd.payload = std::any();
            SdkResult chipInfoRes = SdkManager::instance().callByHandle(sdkGuiderHandle, chipInfoCmd);

            if (chipInfoRes.success)
            {
                try
                {
                    SdkChipInfo chipInfo = std::any_cast<SdkChipInfo>(chipInfoRes.payload);
                    cameraWidthMm = chipInfo.chipWidthMM;
                    cameraHeightMm = chipInfo.chipHeightMM;
                }
                catch (const std::bad_any_cast &e)
                {
                    Logger::Log("initPolarAlignment | SDK GetChipInfo bad_any_cast: " + std::string(e.what()),
                                LogLevel::ERROR, DeviceType::MAIN);
                }
            }
            else
            {
                Logger::Log("initPolarAlignment | SDK GetChipInfo failed: " + chipInfoRes.message,
                            LogLevel::WARNING, DeviceType::GUIDER);
            }
        }
    }
    else
    {
        captureDevice = dpPoleScope;
        useSdkCaptureSource = isPoleCameraSDK();
        captureRoleName = "PoleCamera";

        if (!isPoleCameraSDK() && dpPoleScope != nullptr)
        {
            double pixelsize, pixelsizX, pixelsizY;
            int maxX, maxY, bitDepth;
            indi_Client->getCCDBasicInfo(dpPoleScope, maxX, maxY, pixelsize, pixelsizX, pixelsizY, bitDepth);
            cameraWidthMm = maxX * pixelsize / 1000.0;
            cameraHeightMm = maxY * pixelsize / 1000.0;
            cameraWidthMm = std::round(cameraWidthMm * 10.0) / 10.0;
            cameraHeightMm = std::round(cameraHeightMm * 10.0) / 10.0;
        }
        else if (isPoleCameraSDK() && sdkPoleScopeHandle != nullptr)
        {
            SdkCommand chipInfoCmd;
            chipInfoCmd.type = SdkCommandType::Custom;
            chipInfoCmd.name = "GetChipInfo";
            chipInfoCmd.payload = std::any();
            SdkResult chipInfoRes = SdkManager::instance().callByHandle(sdkPoleScopeHandle, chipInfoCmd);

            if (chipInfoRes.success)
            {
                try
                {
                    SdkChipInfo chipInfo = std::any_cast<SdkChipInfo>(chipInfoRes.payload);
                    cameraWidthMm = chipInfo.chipWidthMM;
                    cameraHeightMm = chipInfo.chipHeightMM;
                }
                catch (const std::bad_any_cast &e)
                {
                    Logger::Log("initPolarAlignment | SDK GetChipInfo bad_any_cast: " + std::string(e.what()),
                                LogLevel::ERROR, DeviceType::MAIN);
                }
            }
            else
            {
                Logger::Log("initPolarAlignment | SDK GetChipInfo failed: " + chipInfoRes.message,
                            LogLevel::WARNING, DeviceType::MAIN);
            }
        }
    }

    if (cameraWidthMm <= 0 || cameraHeightMm <= 0)
    {
        Logger::Log("initPolarAlignment | Camera size parameters are invalid", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,Camera size parameters are invalid");
        return false;
    }

    currentPolarAlignmentCameraRole = role;
    polarGuiderSingleCapturePending = false;

    polarAlignment = new PolarAlignment(indi_Client, dpMount, captureDevice, useSdkCaptureSource, captureRoleName);
    if (polarAlignment == nullptr)
    {
        Logger::Log("initPolarAlignment | Failed to create PolarAlignment object", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start polar alignment,Failed to create PolarAlignment object");
        return false;
    }

    QObject::connect(polarAlignment, &PolarAlignment::requestCaptureForRole,
                     this, [this](const QString &cameraRole, int exposureMs)
                     {
                         const PolarAlignmentCameraRole requestedRole = parsePolarAlignmentCameraRole(cameraRole);
                         if (requestedRole == PolarAlignmentCameraRole::Guider)
                         {
                             this->startGuiderSingleCapture(exposureMs);
                         }
                         else if (requestedRole == PolarAlignmentCameraRole::PoleCamera)
                         {
                             this->startPoleCameraSingleCapture(exposureMs);
                         }
                         else
                         {
                             this->startMainCameraCapture(exposureMs);
                         }
                     },
                     Qt::QueuedConnection);

    // 设置配置参数
    PolarAlignmentConfig config;
    config.defaultExposureTime = 1000;         // 默认曝光时间1秒
    config.recoveryExposureTime = 5000;        // 恢复曝光时间5秒
    config.raRotationAngle = 15.0;              // RA轴每次移动1度
    config.decRotationAngle = 15.0;             // DEC轴移动1度
    config.maxRetryAttempts = 3;               // 最大重试3次
    config.captureAndAnalysisTimeout = 30000;  // 拍摄和分析超时30秒
    config.movementTimeout = 15000;            // 移动超时15秒
    config.maxAdjustmentAttempts = 3;          // 最大调整尝试次数3次
    config.adjustmentAngleReduction = 0.5;     // 调整角度减半
    config.cameraWidth = cameraWidthMm;   // 相机传感器宽度（毫米）
    config.cameraHeight = cameraHeightMm; // 相机传感器高度（毫米）
    config.focalLength = selectedFocalLength; // 焦距（毫米）
    config.latitude = observatorylatitude;     // 观测地点纬度（度）
    config.longitude = observatorylongitude;   // 观测地点经度（度）
    config.finalVerificationThreshold = 0.5;   // 最终验证精度阈值（度）

    // 设置配置
    polarAlignment->setConfig(config);

    // 连接信号
    connect(polarAlignment, &PolarAlignment::stateChanged,
            [this](PolarAlignmentState state, QString message, int percentage)
            {
                qDebug() << "状态改变:" << static_cast<int>(state) << " 消息:" << message << " 进度:" << percentage;
                emit this->wsThread->sendMessageToClient(QString("PolarAlignmentState:") +
                                                        (polarAlignment->isRunning() ? "true" : "false") + ":" +
                                                        QString::number(static_cast<int>(state)) + ":" +
                                                        message + ":" +
                                                        QString::number(percentage));

                // 极轴校准结束/停止后，自动恢复赤道仪跟踪
                if (state == PolarAlignmentState::IDLE ||
                    state == PolarAlignmentState::COMPLETED ||
                    state == PolarAlignmentState::FAILED ||
                    state == PolarAlignmentState::USER_INTERVENTION)
                {
                    qunsetenv("QUARCS_POLAR_SOLVE_FIXED");
                    qunsetenv("QUARCS_POLAR_SOLVE_RA_DEG");
                    qunsetenv("QUARCS_POLAR_SOLVE_DEC_DEG");
                    qunsetenv("QUARCS_POLAR_SOLVE_RADIUS_DEG");
                    qunsetenv("QUARCS_POLAR_SOLVE_SCALE_LOW_DEG");
                    qunsetenv("QUARCS_POLAR_SOLVE_SCALE_HIGH_DEG");
                    qunsetenv("QUARCS_POLAR_SOLVE_DEPTH");
                    if (indi_Client != nullptr && dpMount != nullptr)
                    {
                        indi_Client->setTelescopeTrackEnable(dpMount, true);


                        bool isTrack = false;
                        indi_Client->getTelescopeTrackEnable(dpMount, isTrack);
                        emit this->wsThread->sendMessageToClient(isTrack ? "TelescopeTrack:ON"
                                                                         : "TelescopeTrack:OFF");

                        Logger::Log("PolarAlignment: Telescope tracking restored after polar alignment",
                                    LogLevel::INFO, DeviceType::MAIN);
                    }
                    emit this->wsThread->sendMessageToClient("PolarAlignmentState:false:0:极轴校准已停止:0");
                }
            });

            connect(polarAlignment, &PolarAlignment::adjustmentGuideData,
                [this](double ra, double dec,
                       double ra0, double dec0, double ra1, double dec1,
                       double ra2, double dec2, double ra3, double dec3,
                       double targetRa, double targetDec,
                       double offsetRa, double offsetDec,
                       const QString &adjustmentRa, const QString &adjustmentDec,
                       double fakePolarRA, double fakePolarDEC,
                       double realPolarRA, double realPolarDEC)
                {
                    QString logMsg = QString("PolarAlignmentAdjustmentGuideData:%1:%2:%3:%4:%5:%6:%7:%8:%9:%10:%11:%12:%13:%14:%15:%16:%17:%18:%19:%20")
                                         .arg(ra)
                                         .arg(dec)
                                         .arg(ra0)
                                         .arg(dec0)
                                         .arg(ra1)
                                         .arg(dec1)
                                         .arg(ra2)
                                         .arg(dec2)
                                         .arg(ra3)
                                         .arg(dec3)
                                         .arg(targetRa)
                                         .arg(targetDec)
                                         .arg(offsetRa)
                                         .arg(offsetDec)
                                         .arg(adjustmentRa)
                                         .arg(adjustmentDec)
                                         .arg(fakePolarRA)
                                         .arg(fakePolarDEC)
                                         .arg(realPolarRA)
                                         .arg(realPolarDEC);
                    Logger::Log("目标点: " + std::to_string(targetRa) + ", " + std::to_string(targetDec), LogLevel::INFO, DeviceType::MAIN);
                    Logger::Log("假极轴: " + std::to_string(fakePolarRA) + ", " + std::to_string(fakePolarDEC), LogLevel::INFO, DeviceType::MAIN);
                    Logger::Log("真极轴: " + std::to_string(realPolarRA) + ", " + std::to_string(realPolarDEC), LogLevel::INFO, DeviceType::MAIN);
                    Logger::Log(logMsg.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                    emit this->wsThread->sendMessageToClient(logMsg);
                });

            // 连接调整阶段进度信号
            connect(polarAlignment, &PolarAlignment::guidanceAdjustmentStepProgress,
                [this](GuidanceAdjustmentStep step, QString message, int starCount)
                {
                    QString logMsg = QString("PolarAlignmentGuidanceStepProgress:%1:%2:%3")
                                         .arg(static_cast<int>(step))
                                         .arg(message)
                                         .arg(starCount);
                    Logger::Log(logMsg.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                    emit this->wsThread->sendMessageToClient(logMsg);
                });

    Logger::Log("initPolarAlignment | PolarAlignment initialized successfully, role=" +
                    std::string(polarRoleName(role)) +
                    ", focal=" + std::to_string(selectedFocalLength) +
                    ", size(mm)=" + std::to_string(cameraWidthMm) + "x" + std::to_string(cameraHeightMm),
                LogLevel::INFO, DeviceType::MAIN);
    return true;
}

bool MainWindow::initPoleMasterPolarAlignment()
{
    const bool mountConnected = (dpMount != nullptr) || isMountSDK();
    if (!mountConnected)
    {
        Logger::Log("initPoleMasterPolarAlignment | mount is not connected", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(
            "StartAutoPolarAlignmentStatus:false:Failed to start pole camera alignment,Mount must be connected");
        return false;
    }
    if (!isPoleCameraConnected())
    {
        Logger::Log("initPoleMasterPolarAlignment | pole camera is not connected", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient(
            "StartAutoPolarAlignmentStatus:false:Failed to start pole camera alignment,PoleCamera is not connected");
        return false;
    }
    if (isMountSDK() && dpMount == nullptr)
    {
        Logger::Log("initPoleMasterPolarAlignment | SDK模式赤道仪暂不支持，请使用INDI模式", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start pole camera alignment,SDK模式赤道仪暂不支持");
        return false;
    }
    if (indi_Client == nullptr)
    {
        Logger::Log("initPoleMasterPolarAlignment | indi_Client is nullptr", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start pole camera alignment,indi_Client is nullptr");
        return false;
    }

    double poleFocal = 0.0;
    std::unordered_map<std::string, std::string> clientConfig;
    Tools::readClientSettings("config/config.ini", clientConfig);
    auto it = clientConfig.find("PoleCameraFocalLength");
    if (it != clientConfig.end())
    {
        bool ok = false;
        const double v = QString::fromStdString(it->second).trimmed().toDouble(&ok);
        if (ok && v > 0.0) poleFocal = v;
    }
    if (poleFocal <= 0.0)
    {
        Logger::Log("initPoleMasterPolarAlignment | PoleCameraFocalLength is not set", LogLevel::WARNING, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start pole camera alignment,pole camera focal length is not set");
        return false;
    }

    double cameraWidthMm = 0.0;
    double cameraHeightMm = 0.0;
    if (!isPoleCameraSDK() && dpPoleScope != nullptr)
    {
        double pixelsize, pixelsizX, pixelsizY;
        int maxX, maxY, bitDepth;
        indi_Client->getCCDBasicInfo(dpPoleScope, maxX, maxY, pixelsize, pixelsizX, pixelsizY, bitDepth);
        cameraWidthMm = maxX * pixelsize / 1000.0;
        cameraHeightMm = maxY * pixelsize / 1000.0;
        cameraWidthMm = std::round(cameraWidthMm * 10.0) / 10.0;
        cameraHeightMm = std::round(cameraHeightMm * 10.0) / 10.0;
    }
    else if (isPoleCameraSDK() && sdkPoleScopeHandle != nullptr)
    {
        SdkCommand chipInfoCmd;
        chipInfoCmd.type = SdkCommandType::Custom;
        chipInfoCmd.name = "GetChipInfo";
        chipInfoCmd.payload = std::any();
        SdkResult chipInfoRes = SdkManager::instance().callByHandle(sdkPoleScopeHandle, chipInfoCmd);

        if (chipInfoRes.success)
        {
            try
            {
                SdkChipInfo chipInfo = std::any_cast<SdkChipInfo>(chipInfoRes.payload);
                cameraWidthMm = chipInfo.chipWidthMM;
                cameraHeightMm = chipInfo.chipHeightMM;
            }
            catch (const std::bad_any_cast &e)
            {
                Logger::Log("initPoleMasterPolarAlignment | SDK GetChipInfo bad_any_cast: " + std::string(e.what()),
                            LogLevel::ERROR, DeviceType::MAIN);
            }
        }
        else
        {
            Logger::Log("initPoleMasterPolarAlignment | SDK GetChipInfo failed: " + chipInfoRes.message,
                        LogLevel::WARNING, DeviceType::MAIN);
        }
    }

    if (cameraWidthMm <= 0.0 || cameraHeightMm <= 0.0)
    {
        Logger::Log("initPoleMasterPolarAlignment | Camera size parameters are invalid", LogLevel::ERROR, DeviceType::MAIN);
        emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start pole camera alignment,Camera size parameters are invalid");
        return false;
    }

    currentPolarAlignmentCameraRole = PolarAlignmentCameraRole::PoleCamera;
    polarGuiderSingleCapturePending = false;
    poleMasterPolarAlignment = new PoleMasterPolarAlignment(indi_Client,
                                                            dpMount,
                                                            dpPoleScope,
                                                            isPoleCameraSDK(),
                                                            false,
                                                            QString(),
                                                            this);
    if (poleMasterPolarAlignment == nullptr)
    {
        emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start pole camera alignment,Failed to create PoleMaster object");
        return false;
    }

    connect(poleMasterPolarAlignment, &PoleMasterPolarAlignment::requestCaptureForRole,
            this, [this](const QString &, int exposureMs)
            {
                this->startPoleCameraSingleCapture(exposureMs);
            },
            Qt::QueuedConnection);

    PoleMasterAlignmentConfig cfg;
    cfg.defaultExposureTime = 1000;
    cfg.guidanceExposureTime = 1000;
    cfg.captureTimeoutMs = 10000;
    cfg.movementTimeoutMs = 60000;
    cfg.focalLength = static_cast<int>(std::lround(poleFocal));
    cfg.cameraWidth = cameraWidthMm;
    cfg.cameraHeight = cameraHeightMm;
    cfg.raRotationAngle = 35.0;
    cfg.doneThresholdArcsec = 30.0;
    cfg.stableFrameRequirement = 3;
    cfg.latitude = observatorylatitude;
    cfg.solveSearchRadiusDeg = 5.0;
    cfg.solveIndexFilePath = "index-tycho2-09.littleendian.fits";
    poleMasterPolarAlignment->setConfig(cfg);

    connect(poleMasterPolarAlignment, &PoleMasterPolarAlignment::stateChanged,
            [this](PoleMasterAlignmentState state, QString message, int progress, bool running)
            {
                const QString stateMsg = QString("PoleMasterAlignmentState:%1:%2:%3:%4")
                                             .arg(running ? "true" : "false")
                                             .arg(static_cast<int>(state))
                                             .arg(message)
                                             .arg(progress);
                emit this->wsThread->sendMessageToClient(stateMsg);

                if (state == PoleMasterAlignmentState::IDLE ||
                    state == PoleMasterAlignmentState::COMPLETED ||
                    state == PoleMasterAlignmentState::FAILED)
                {
                    qunsetenv("QUARCS_POLAR_SOLVE_FIXED");
                    qunsetenv("QUARCS_POLAR_SOLVE_RA_DEG");
                    qunsetenv("QUARCS_POLAR_SOLVE_DEC_DEG");
                    qunsetenv("QUARCS_POLAR_SOLVE_RADIUS_DEG");
                    qunsetenv("QUARCS_POLAR_SOLVE_SCALE_LOW_DEG");
                    qunsetenv("QUARCS_POLAR_SOLVE_SCALE_HIGH_DEG");
                    qunsetenv("QUARCS_POLAR_SOLVE_DEPTH");
                    if (indi_Client != nullptr && dpMount != nullptr)
                    {
                        indi_Client->setTelescopeTrackEnable(dpMount, true);
                        bool isTrack = false;
                        indi_Client->getTelescopeTrackEnable(dpMount, isTrack);
                        emit this->wsThread->sendMessageToClient(isTrack ? "TelescopeTrack:ON" : "TelescopeTrack:OFF");
                    }
                    emit this->wsThread->sendMessageToClient("PolarAlignmentState:false:0:电子极轴镜校准已停止:0");
                }
            });

    connect(poleMasterPolarAlignment, &PoleMasterPolarAlignment::guideData,
            [this](int imageW, int imageH,
                   double axisX, double axisY,
                   double poleX, double poleY,
                   double errorPx, double errorArcsec,
                   const QString &frameId,
                   const QString &hint)
            {
                const QString msg = QString("PoleMasterAlignmentGuideData:%1:%2:%3:%4:%5:%6:%7:%8:%9:%10")
                                        .arg(imageW)
                                        .arg(imageH)
                                        .arg(axisX, 0, 'f', 3)
                                        .arg(axisY, 0, 'f', 3)
                                        .arg(poleX, 0, 'f', 3)
                                        .arg(poleY, 0, 'f', 3)
                                        .arg(errorPx, 0, 'f', 3)
                                        .arg(errorArcsec, 0, 'f', 2)
                                        .arg(frameId)
                                        .arg(hint);
                Logger::Log(msg.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                emit this->wsThread->sendMessageToClient(msg);
            });

    connect(poleMasterPolarAlignment, &PoleMasterPolarAlignment::frameData,
            [this](const QString &fileName, int imageW, int imageH, const QString &frameId)
            {
                const QString sourcePath = QDir(QString::fromStdString(vueDirectoryPath)).filePath(fileName);
                const QString targetPath = QDir(QString::fromStdString(vueImagePath)).filePath(fileName);
                if (QFileInfo::exists(sourcePath) && sourcePath != targetPath && !QFileInfo::exists(targetPath))
                {
                    QFile::link(sourcePath, targetPath);
                    if (!QFileInfo::exists(targetPath))
                        QFile::copy(sourcePath, targetPath);
                }
                emit this->wsThread->sendMessageToClient(QString("PoleMasterAlignmentFrameData:%1:%2:%3:%4")
                                                             .arg(fileName)
                                                             .arg(imageW)
                                                             .arg(imageH)
                                                             .arg(frameId));
            });

    connect(poleMasterPolarAlignment, &PoleMasterPolarAlignment::overlayData,
            [this](const QString &jsonPayload)
            {
                emit this->wsThread->sendMessageToClient("PoleMasterAlignmentOverlayData:" + jsonPayload);
            });

    Logger::Log("initPoleMasterPolarAlignment | initialized successfully, focal=" +
                    std::to_string(static_cast<int>(std::lround(poleFocal))) +
                    ", size(mm)=" + std::to_string(cameraWidthMm) + "x" + std::to_string(cameraHeightMm),
                LogLevel::INFO, DeviceType::MAIN);
    return true;
}

bool MainWindow::initPoleMasterAlignmentSimulation()
{
    currentPolarAlignmentCameraRole = PolarAlignmentCameraRole::PoleCamera;

    QString imageRoot = QString::fromStdString(vueImagePath);
    if (imageRoot.trimmed().isEmpty())
        imageRoot = QString::fromStdString(vueDirectoryPath);
    imageRoot = QDir::cleanPath(imageRoot);
    QDir dir(imageRoot);
    if (!dir.exists() && !dir.mkpath("."))
    {
        Logger::Log("initPoleMasterAlignmentSimulation | image root unavailable, stop simulation. root=" +
                        imageRoot.toStdString(),
                    LogLevel::ERROR,
                    DeviceType::MAIN);
        return false;
    }

    poleMasterPolarAlignment = new PoleMasterPolarAlignment(indi_Client,
                                                            dpMount,
                                                            dpPoleScope,
                                                            isPoleCameraSDK(),
                                                            true,
                                                            imageRoot,
                                                            this);
    if (poleMasterPolarAlignment == nullptr)
    {
        emit wsThread->sendMessageToClient("StartAutoPolarAlignmentStatus:false:Failed to start pole camera simulation,Failed to create simulation object");
        return false;
    }

    PoleMasterAlignmentConfig cfg;
    cfg.defaultExposureTime = 1000;
    cfg.guidanceExposureTime = 1000;
    cfg.captureTimeoutMs = 15000;
    cfg.movementTimeoutMs = 2000;
    cfg.focalLength = 100;
    cfg.cameraWidth = 21.0;
    cfg.cameraHeight = 15.75;
    cfg.raRotationAngle = 35.0;
    cfg.doneThresholdArcsec = 30.0;
    cfg.stableFrameRequirement = 3;
    cfg.latitude = observatorylatitude;
    cfg.solveSearchRadiusDeg = 5.0;
    cfg.solveIndexFilePath = "index-tycho2-12.littleendian.fits,index-tycho2-13.littleendian.fits,index-tycho2-14.littleendian.fits,index-tycho2-15.littleendian.fits";
    poleMasterPolarAlignment->setConfig(cfg);

    connect(poleMasterPolarAlignment, &PoleMasterPolarAlignment::stateChanged,
            [this](PoleMasterAlignmentState state, QString message, int progress, bool running)
            {
                const QString stateMsg = QString("PoleMasterAlignmentState:%1:%2:%3:%4")
                                             .arg(running ? "true" : "false")
                                             .arg(static_cast<int>(state))
                                             .arg(message)
                                             .arg(progress);
                emit this->wsThread->sendMessageToClient(stateMsg);

                if (state == PoleMasterAlignmentState::IDLE ||
                    state == PoleMasterAlignmentState::COMPLETED ||
                    state == PoleMasterAlignmentState::FAILED)
                {
                    emit this->wsThread->sendMessageToClient("PolarAlignmentState:false:0:电子极轴镜模拟校准已停止:0");
                }
            });

    connect(poleMasterPolarAlignment, &PoleMasterPolarAlignment::guideData,
            [this](int imageW, int imageH,
                   double axisX, double axisY,
                   double poleX, double poleY,
                   double errorPx, double errorArcsec,
                   const QString &frameId,
                   const QString &hint)
            {
                const QString msg = QString("PoleMasterAlignmentGuideData:%1:%2:%3:%4:%5:%6:%7:%8:%9:%10")
                                        .arg(imageW)
                                        .arg(imageH)
                                        .arg(axisX, 0, 'f', 3)
                                        .arg(axisY, 0, 'f', 3)
                                        .arg(poleX, 0, 'f', 3)
                                        .arg(poleY, 0, 'f', 3)
                                        .arg(errorPx, 0, 'f', 3)
                                        .arg(errorArcsec, 0, 'f', 2)
                                        .arg(frameId)
                                        .arg(hint);
                Logger::Log(msg.toStdString(), LogLevel::INFO, DeviceType::MAIN);
                emit this->wsThread->sendMessageToClient(msg);
            });

    connect(poleMasterPolarAlignment, &PoleMasterPolarAlignment::frameData,
            [this](const QString &fileName, int imageW, int imageH, const QString &frameId)
            {
                const QString sourcePath = QDir(QString::fromStdString(vueDirectoryPath)).filePath(fileName);
                const QString targetPath = QDir(QString::fromStdString(vueImagePath)).filePath(fileName);
                if (QFileInfo::exists(sourcePath) && sourcePath != targetPath && !QFileInfo::exists(targetPath))
                {
                    QFile::link(sourcePath, targetPath);
                    if (!QFileInfo::exists(targetPath))
                        QFile::copy(sourcePath, targetPath);
                }
                emit this->wsThread->sendMessageToClient(QString("PoleMasterAlignmentFrameData:%1:%2:%3:%4")
                                                             .arg(fileName)
                                                             .arg(imageW)
                                                             .arg(imageH)
                                                             .arg(frameId));
            });

    connect(poleMasterPolarAlignment, &PoleMasterPolarAlignment::overlayData,
            [this](const QString &jsonPayload)
            {
                emit this->wsThread->sendMessageToClient("PoleMasterAlignmentOverlayData:" + jsonPayload);
            });

    Logger::Log("initPoleMasterAlignmentSimulation | initialized successfully, imageRoot=" +
                    imageRoot.toStdString(),
                LogLevel::INFO,
                DeviceType::MAIN);
    return true;
}

// [停用 2026-04-14] 旧自动电调校准实现：保留函数体用于历史回溯，当前不再通过命令入口触发。
void MainWindow::getCheckBoxSpace()
{
    // 计算应用所在分区的可用空间，避免与实际存储分区不一致
    QString path = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
    path = QDir::rootPath();
#endif
    QFileInfo fi(path);
    quint64 freeBytes = 0;
    if (fi.exists())
    {
        QStorageInfo storage(path);
        freeBytes = static_cast<quint64>(storage.bytesAvailable());
    }
    // 发送到前端：Box_Space:<bytes>
    if (wsThread)
    {
        emit wsThread->sendMessageToClient("Box_Space:" + QString::number(freeBytes));
    }
}

void MainWindow::clearLogs()
{
    // 按 Logger::Initialize 约定：运行目录下 logs/MAIN.log 等
    const QString logDirPath = QDir::currentPath() + "/logs";
    QDir logDir(logDirPath);
    if (logDir.exists())
    {
        QFileInfoList entries = logDir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
        for (const QFileInfo &fi : entries)
        {
            // 清空内容而不删除文件
            QFile f(fi.absoluteFilePath());
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            {
                f.close();
            }
        }
    }
    if (wsThread) emit wsThread->sendMessageToClient("ClearLogs:Success");
}

void MainWindow::clearBoxCache(bool clearCache, bool clearUpdatePack, bool clearBackup)
{
    auto clearDirContents = [](const QString &dirPath)
    {
        if (dirPath.isEmpty()) return;
        QDir dir(dirPath);
        if (!dir.exists()) return;
        // 删除目录下的所有条目（文件/链接/目录），保留顶层目录本身
        QFileInfoList entries = dir.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden);
        for (const QFileInfo &fi : entries)
        {
            if (fi.isSymLink() || fi.isFile())
            {
                QFile::remove(fi.absoluteFilePath());
            }
            else if (fi.isDir())
            {
                QDir sub(fi.absoluteFilePath());
                sub.removeRecursively();
            }
        }
    };
    auto clearFileIfExists = [](const QString &filePath)
    {
        if (filePath.isEmpty()) return;
        QFileInfo fi(filePath);
        if (!fi.exists() || !fi.isFile()) return;
        QFile::remove(filePath);
    };

    // 1. 清理系统/应用缓存与回收站
    if (clearCache)
    {
        QStringList caches;
        // 用户主目录缓存根（比单纯的 CacheLocation 更全面）
        caches << (QDir::homePath() + "/.cache");
        caches << QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
               << QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
               << QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        // 额外补充持久临时目录与常见开发工具缓存
        caches << "/var/tmp";
        caches << (QDir::homePath() + "/.cursor-server/data/CachedExtensionVSIXs");
        // XDG 垃圾箱（当前用户）常规路径与 XDG_DATA_HOME 路径
        const QString trashBase = QDir::homePath() + "/.local/share/Trash";
        caches << (trashBase + "/files") << (trashBase + "/info") << (trashBase + "/expunged");
        const QString xdgDataHome = qEnvironmentVariableIsSet("XDG_DATA_HOME")
                                    ? QString::fromLocal8Bit(qgetenv("XDG_DATA_HOME"))
                                    : (QDir::homePath() + "/.local/share");
        caches << (xdgDataHome + "/Trash/files")
               << (xdgDataHome + "/Trash/info")
               << (xdgDataHome + "/Trash/expunged");
        // 常见桌面环境的垃圾箱路径（可能存在）
        caches << (QDir::homePath() + "/.Trash")
               << (QDir::homePath() + "/.Trash-1000/files")
               << (QDir::homePath() + "/.Trash-1000/info");

        for (const QString &p : caches) clearDirContents(p);

        // 尝试调用 gio 清空垃圾箱（若环境支持）
        QProcess::execute("gio", QStringList() << "trash" << "--empty");

        // 清空可移动介质等可能挂载点的垃圾箱（.Trash-UID 或 .Trash）
        QString uidStr = QString::number(getuid());
        QStringList mountRoots;
        mountRoots << "/mnt" << "/media" << "/run/media";
        for (const QString &root : mountRoots)
        {
            QDir rootDir(root);
            if (!rootDir.exists()) continue;
            QFileInfoList subs = rootDir.entryInfoList(QDir::NoDotAndDotDot | QDir::Dirs);
            for (const QFileInfo &fi : subs)
            {
                const QString base = fi.absoluteFilePath();
                clearDirContents(base + "/.Trash-" + uidStr + "/files");
                clearDirContents(base + "/.Trash-" + uidStr + "/info");
                clearDirContents(base + "/.Trash-" + uidStr + "/expunged");
                clearDirContents(base + "/.Trash/files");
                clearDirContents(base + "/.Trash/info");
                clearDirContents(base + "/.Trash/expunged");
            }
        }

        // 清理 apt 缓存与包索引，避免更新缓存长期堆积。
        clearDirContents("/var/cache/apt");
        clearDirContents("/var/lib/apt/lists");

        // 清掉常见锁文件，避免目录已清空但索引残留。
        clearFileIfExists("/var/lib/apt/lists/lock");
        clearFileIfExists("/var/cache/apt/archives/lock");

        // 限制 systemd journal 体积；失败时静默跳过，避免无 sudo 能力时中断清理。
        runSudoSync("/usr/bin/journalctl", {"--vacuum-size=100M"}, 15000);
    }

    // 2. 可选：清理更新包目录
    if (clearUpdatePack)
    {
        clearDirContents("/var/www/update_pack");
    }

    // 3. 可选：清理备份目录
    if (clearBackup)
    {
        clearDirContents("/home/quarcs/workspace/QUARCS/backup");
    }

    if (wsThread) emit wsThread->sendMessageToClient("ClearBoxCache:Success");
}
