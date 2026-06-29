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

void MainWindow::onGuiderLoopTimeout()
{
    if (!isGuiderLoopExp)
        return;

#if QUARCS_SIM_GUIDER
    if (simGuiderFrameSource)
    {
        if (guiderExposureInFlight)
            return;

        guiderExposureInFlight = true;
        const int expMs = std::max(1, guiderExpMs);
        const QString fitsPath = simGuiderFrameSource->generateNextFrame(expMs);
        guiderExposureInFlight = false;

        if (fitsPath.isEmpty() || !QFile::exists(fitsPath))
        {
            Logger::Log("onGuiderLoopTimeout | simulated guider frame generation failed",
                        LogLevel::ERROR, DeviceType::GUIDER);
            isGuiderLoopExp = false;
            if (guiderLoopTimer)
                guiderLoopTimer->stop();
            emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
            emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
            return;
        }

        if (guiderCore)
        {
            QMetaObject::invokeMethod(guiderCore, "onNewFrame", Qt::QueuedConnection,
                                      Q_ARG(QString, fitsPath));
        }
        else
        {
            PersistGuidingFits(fitsPath);
            if (isGuiderLoopExp && guiderLoopTimer)
                guiderLoopTimer->start(1);
        }
        return;
    }
#endif

    const bool guiderSdk =
        (systemdevicelist.system_devices.size() > 1 &&
         systemdevicelist.system_devices[1].isSDKConnect &&
         sdkGuiderHandle != nullptr);

    // SDK 模式：导星相机不依赖 INDI 连接
    if (guiderSdk)
    {
        SdkSerialExecutor *guiderExec = sdkGuiderCameraExecutor();
        if (!guiderExec || !guiderExec->isRunning())
        {
            Logger::Log("onGuiderLoopTimeout | sdkGuiderCamExec not running, stopping guider loop", LogLevel::WARNING, DeviceType::GUIDER);
            isGuiderLoopExp = false;
            guiderExposureInFlight = false;
            if (guiderCore)
            {
                postGuiderCore(guiderCore, [](GuiderCore *core) {
                    core->stopGuiding();
                    core->stopLoop();
                });
            }
            if (guiderLoopTimer)
                guiderLoopTimer->stop();
            emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
            emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
            return;
        }
        if (guiderExposureInFlight)
            return;

        guiderExposureInFlight = true;
        const double expSec = std::max(1, guiderExpMs) / 1000.0;
        const int expMs = std::max(1, guiderExpMs);

        // 0) 对齐主相机 SDK：曝光前确保分辨率/ROI 为有效全分辨率，否则某些机型会出现 GetSingleFrame 卡死/返回无效帧
        {
            QElapsedTimer loopPerf;
            loopPerf.start();
            qint64 loopLastMs = 0;
            auto logLoopStage = [&](const std::string& stage) {
                const qint64 nowMs = loopPerf.elapsed();
                Logger::Log("GuiderPerf | GuiderLoop(SDK) | stage=" + stage +
                                " deltaMs=" + std::to_string(nowMs - loopLastMs) +
                                " totalMs=" + std::to_string(nowMs),
                            LogLevel::INFO, DeviceType::GUIDER);
                loopLastMs = nowMs;
            };

            // 尝试取消上一帧可能残留的曝光/读出（避免连续触发时卡死）
            SdkCommand cancelCmd;
            cancelCmd.type = SdkCommandType::Custom;
            cancelCmd.name = "CancelExposure";
            cancelCmd.payload = std::any();
            SdkResult cancelRes = SdkManager::instance().callByHandle(sdkGuiderHandle, cancelCmd);
            Logger::Log("GuiderPerf | GuiderLoop(SDK) | CancelExposure success=" +
                            std::to_string(cancelRes.success ? 1 : 0) +
                            " msg=" + cancelRes.message,
                        LogLevel::INFO, DeviceType::GUIDER);
            logLoopStage("cancel_previous_exposure");

            SdkAreaInfo fullRoi;
            bool haveFullRoi = false;
            {
                SdkCommand effCmd;
                effCmd.type = SdkCommandType::Custom;
                effCmd.name = "GetEffectiveArea";
                effCmd.payload = std::any();
                SdkResult effRes = SdkManager::instance().callByHandle(sdkGuiderHandle, effCmd);
                Logger::Log("GuiderPerf | GuiderLoop(SDK) | GetEffectiveArea success=" +
                                std::to_string(effRes.success ? 1 : 0) +
                                " msg=" + effRes.message,
                            LogLevel::INFO, DeviceType::GUIDER);
                logLoopStage("get_effective_area");
                if (effRes.success)
                {
                    try
                    {
                        fullRoi = std::any_cast<SdkAreaInfo>(effRes.payload);
                        haveFullRoi = (fullRoi.sizeX > 0 && fullRoi.sizeY > 0);
                    }
                    catch (const std::bad_any_cast &)
                    {
                        haveFullRoi = false;
                    }
                }
            }
            if (!haveFullRoi)
            {
                // 回退：GetChipInfo（尽量拿到最大分辨率）
                SdkCommand chipCmd;
                chipCmd.type = SdkCommandType::Custom;
                chipCmd.name = "GetChipInfo";
                chipCmd.payload = std::any();
                SdkResult chipRes = SdkManager::instance().callByHandle(sdkGuiderHandle, chipCmd);
                Logger::Log("GuiderPerf | GuiderLoop(SDK) | GetChipInfo fallback success=" +
                                std::to_string(chipRes.success ? 1 : 0) +
                                " msg=" + chipRes.message,
                            LogLevel::INFO, DeviceType::GUIDER);
                logLoopStage("get_chip_info_fallback");
                if (chipRes.success)
                {
                    try
                    {
                        SdkChipInfo chip = std::any_cast<SdkChipInfo>(chipRes.payload);
                        fullRoi.startX = 0;
                        fullRoi.startY = 0;
                        fullRoi.sizeX = chip.maxImageSizeX;
                        fullRoi.sizeY = chip.maxImageSizeY;
                        haveFullRoi = (fullRoi.sizeX > 0 && fullRoi.sizeY > 0);
                    }
                    catch (const std::bad_any_cast &)
                    {
                        haveFullRoi = false;
                    }
                }
            }

            if (haveFullRoi)
            {
                SdkCommand setResCmd;
                setResCmd.type = SdkCommandType::Custom;
                setResCmd.name = "SetResolution";
                setResCmd.payload = fullRoi;
                SdkResult setResRes = SdkManager::instance().callByHandle(sdkGuiderHandle, setResCmd);
                Logger::Log("GuiderPerf | GuiderLoop(SDK) | SetResolution success=" +
                                std::to_string(setResRes.success ? 1 : 0) +
                                " roi=" + std::to_string(fullRoi.startX) + "," + std::to_string(fullRoi.startY) +
                                "," + std::to_string(fullRoi.sizeX) + "x" + std::to_string(fullRoi.sizeY) +
                                " msg=" + setResRes.message,
                            LogLevel::INFO, DeviceType::GUIDER);
                logLoopStage("set_resolution");
                if (!setResRes.success)
                {
                    Logger::Log("GuiderLoop(SDK) | SetResolution(full) failed: " + setResRes.message,
                                LogLevel::WARNING, DeviceType::GUIDER);
                }
            }
            else
            {
                Logger::Log("GuiderLoop(SDK) | SetResolution(full) skipped: cannot get valid full ROI",
                            LogLevel::WARNING, DeviceType::GUIDER);
                logLoopStage("set_resolution_skipped");
            }
        }

        // 1) SetExposure（us）
        {
            QElapsedTimer setExpPerf;
            setExpPerf.start();
            SdkCommand setExpCmd;
            setExpCmd.type = SdkCommandType::Custom;
            setExpCmd.name = "SetExposure";
            setExpCmd.payload = expSec * 1000000.0;
            SdkResult setRes = SdkManager::instance().callByHandle(sdkGuiderHandle, setExpCmd);
            Logger::Log("GuiderPerf | GuiderLoop(SDK) | SetExposure success=" +
                            std::to_string(setRes.success ? 1 : 0) +
                            " costMs=" + std::to_string(setExpPerf.elapsed()) +
                            " exposureUs=" + std::to_string(static_cast<qint64>(expSec * 1000000.0)) +
                            " msg=" + setRes.message,
                        LogLevel::INFO, DeviceType::GUIDER);
            if (!setRes.success)
            {
                Logger::Log("GuiderLoop(SDK) | SetExposure failed: " + setRes.message, LogLevel::ERROR, DeviceType::GUIDER);
            }
        }

        // 2) StartSingleExposure
        {
            QElapsedTimer startExpPerf;
            startExpPerf.start();
            SdkCommand startExpCmd;
            startExpCmd.type = SdkCommandType::Custom;
            startExpCmd.name = "StartSingleExposure";
            startExpCmd.payload = std::any();
            SdkResult startRes = SdkManager::instance().callByHandle(sdkGuiderHandle, startExpCmd);
            Logger::Log("GuiderPerf | GuiderLoop(SDK) | StartSingleExposure success=" +
                            std::to_string(startRes.success ? 1 : 0) +
                            " costMs=" + std::to_string(startExpPerf.elapsed()) +
                            " msg=" + startRes.message,
                        LogLevel::INFO, DeviceType::GUIDER);
            if (!startRes.success)
            {
                Logger::Log("GuiderLoop(SDK) | StartSingleExposure failed: " + startRes.message, LogLevel::ERROR, DeviceType::GUIDER);
                guiderExposureInFlight = false;
                if (isGuiderLoopExp && guiderLoopTimer)
                    guiderLoopTimer->start(200);
                return;
            }
        }

        // 3) Poll GetSingleFrame via timer (main thread)
        sdkGuiderExposureStartTime = QDateTime::currentMSecsSinceEpoch();
        sdkGuiderExposureExpectedDuration = expMs;
        if (sdkGuiderExposureTimer)
        {
            sdkGuiderExposureTimer->start(expMs);
            Logger::Log("GuiderPerf | GuiderLoop(SDK) | exposure timer started delayMs=" +
                            std::to_string(expMs),
                        LogLevel::INFO, DeviceType::GUIDER);
        }
        return;
    }

    if (!indi_Client || dpGuider == NULL || !dpGuider->isConnected())
    {
        Logger::Log("onGuiderLoopTimeout | guider not connected, stopping loop", LogLevel::WARNING, DeviceType::GUIDER);
        isGuiderLoopExp = false;
        guiderExposureInFlight = false;
        if (guiderCore)
        {
            postGuiderCore(guiderCore, [](GuiderCore *core) {
                core->stopGuiding();
                core->stopLoop();
            });
        }
        if (guiderLoopTimer)
            guiderLoopTimer->stop();
        emit wsThread->sendMessageToClient("GuiderLoopExpStatus:false");
        emit wsThread->sendMessageToClient("GuiderUpdateStatus:0");
        return;
    }

    if (guiderExposureInFlight)
        return;

    guiderExposureInFlight = true;
    const double expSec = std::max(1, guiderExpMs) / 1000.0;
    Logger::Log("onGuiderLoopTimeout | taking guider exposure " + std::to_string(expSec) + "s", LogLevel::DEBUG, DeviceType::GUIDER);
    indi_Client->takeExposure(dpGuider, expSec);
}

void MainWindow::startGuiderSingleCapture(int exposureMs)
{
    const int expMs = std::max(1, exposureMs);
    sdkGuiderExposureRole = "Guider";

    if (isGuiderLoopExp)
    {
        Logger::Log("startGuiderSingleCapture | stopping guider loop before polar capture",
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

    const bool guiderSdk = isGuiderCameraSDK();
    if (guiderSdk)
    {
        SdkSerialExecutor *guiderExec = sdkGuiderCameraExecutor();
        if (!guiderExec || !guiderExec->isRunning())
        {
            Logger::Log("startGuiderSingleCapture | sdkGuiderCamExec is not running", LogLevel::ERROR, DeviceType::GUIDER);
            polarGuiderSingleCapturePending = false;
            guiderExposureInFlight = false;
            return;
        }
        if (guiderExposureInFlight || sdkGuiderFrameTaskInFlight.load())
        {
            Logger::Log("startGuiderSingleCapture | guider exposure/readout is already in flight, retrying shortly",
                        LogLevel::WARNING, DeviceType::GUIDER);
            QTimer::singleShot(250, this, [this, expMs]() {
                if (polarAlignment != nullptr && polarAlignment->isRunning() &&
                    currentPolarAlignmentCameraRole == PolarAlignmentCameraRole::Guider)
                {
                    startGuiderSingleCapture(expMs);
                }
            });
            return;
        }

        polarGuiderSingleCapturePending = true;
        guiderExposureInFlight = true;
        const double expSec = expMs / 1000.0;
        const SdkDeviceHandle handleSnap = sdkGuiderHandle;

        guiderExec->post([this, handleSnap, expMs, expSec]() {
            auto failOnMain = [this](const std::string &message) {
                QMetaObject::invokeMethod(this, [this, message]() {
                    Logger::Log(message, LogLevel::ERROR, DeviceType::GUIDER);
                    polarGuiderSingleCapturePending = false;
                    guiderExposureInFlight = false;
                }, Qt::QueuedConnection);
            };

            SdkCommand cancelCmd;
            cancelCmd.type = SdkCommandType::Custom;
            cancelCmd.name = "CancelExposure";
            cancelCmd.payload = std::any();
            SdkManager::instance().callByHandle(handleSnap, cancelCmd);

            SdkAreaInfo fullRoi;
            bool haveFullRoi = false;
            SdkCommand effCmd;
            effCmd.type = SdkCommandType::Custom;
            effCmd.name = "GetEffectiveArea";
            effCmd.payload = std::any();
            SdkResult effRes = SdkManager::instance().callByHandle(handleSnap, effCmd);
            if (effRes.success)
            {
                try
                {
                    fullRoi = std::any_cast<SdkAreaInfo>(effRes.payload);
                    haveFullRoi = (fullRoi.sizeX > 0 && fullRoi.sizeY > 0);
                }
                catch (const std::bad_any_cast &)
                {
                    haveFullRoi = false;
                }
            }
            if (!haveFullRoi)
            {
                SdkCommand chipCmd;
                chipCmd.type = SdkCommandType::Custom;
                chipCmd.name = "GetChipInfo";
                chipCmd.payload = std::any();
                SdkResult chipRes = SdkManager::instance().callByHandle(handleSnap, chipCmd);
                if (chipRes.success)
                {
                    try
                    {
                        SdkChipInfo chip = std::any_cast<SdkChipInfo>(chipRes.payload);
                        fullRoi.startX = 0;
                        fullRoi.startY = 0;
                        fullRoi.sizeX = chip.maxImageSizeX;
                        fullRoi.sizeY = chip.maxImageSizeY;
                        haveFullRoi = (fullRoi.sizeX > 0 && fullRoi.sizeY > 0);
                    }
                    catch (const std::bad_any_cast &)
                    {
                        haveFullRoi = false;
                    }
                }
            }
            if (haveFullRoi)
            {
                SdkCommand setResCmd;
                setResCmd.type = SdkCommandType::Custom;
                setResCmd.name = "SetResolution";
                setResCmd.payload = fullRoi;
                SdkResult setResRes = SdkManager::instance().callByHandle(handleSnap, setResCmd);
                if (!setResRes.success)
                {
                    Logger::Log("startGuiderSingleCapture | SDK SetResolution(full) failed: " + setResRes.message,
                                LogLevel::WARNING, DeviceType::GUIDER);
                }
            }

            SdkCommand setExpCmd;
            setExpCmd.type = SdkCommandType::Custom;
            setExpCmd.name = "SetExposure";
            setExpCmd.payload = expSec * 1000000.0;
            SdkResult setRes = SdkManager::instance().callByHandle(handleSnap, setExpCmd);
            if (!setRes.success)
            {
                failOnMain("startGuiderSingleCapture | SDK SetExposure failed: " + setRes.message);
                return;
            }

            SdkCommand startExpCmd;
            startExpCmd.type = SdkCommandType::Custom;
            startExpCmd.name = "StartSingleExposure";
            startExpCmd.payload = std::any();
            SdkResult startRes = SdkManager::instance().callByHandle(handleSnap, startExpCmd);
            if (!startRes.success)
            {
                failOnMain("startGuiderSingleCapture | SDK StartSingleExposure failed: " + startRes.message);
                return;
            }

            QMetaObject::invokeMethod(this, [this, expMs]() {
                sdkGuiderExposureStartTime = QDateTime::currentMSecsSinceEpoch();
                sdkGuiderExposureExpectedDuration = expMs;
                if (sdkGuiderExposureTimer)
                    sdkGuiderExposureTimer->start(expMs);
            }, Qt::QueuedConnection);
        });
        return;
    }

    if (!indi_Client || dpGuider == nullptr || !dpGuider->isConnected())
    {
        Logger::Log("startGuiderSingleCapture | guider is not connected", LogLevel::ERROR, DeviceType::GUIDER);
        polarGuiderSingleCapturePending = false;
        guiderExposureInFlight = false;
        return;
    }

    polarGuiderSingleCapturePending = true;
    guiderExposureInFlight = true;
    const double expSec = expMs / 1000.0;
    indi_Client->takeExposure(dpGuider, expSec);
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

int MainWindow::saveFitsAsPNG(QString fitsFileName, bool ProcessBin, std::function<void(bool)> onComplete)
{
    // 目标：主线程不阻塞；重活放后台，并可用 epoch 取消旧帧任务
    const quint64 epoch = ++tilePyramidEpoch;
    QPointer<MainWindow> self(this);
    const QString fitsCopy = fitsFileName;
    const bool processBinCopy = ProcessBin;

    QtConcurrent::run([self, epoch, fitsCopy, processBinCopy, onComplete]() {
        if (!self) return;
        if (self->tilePyramidEpoch.load() != epoch) return;
        const int rc = self->saveFitsAsPNG_Worker(fitsCopy, processBinCopy);

        // Live 模式的“处理链路 busy”必须在处理结束后再释放（否则会导致每帧都排队重处理）
        QMetaObject::invokeMethod(self, [self, epoch, rc, onComplete]() {
            if (!self) return;
            if (self->tilePyramidEpoch.load() != epoch) return;
            if (onComplete) onComplete(rc == 0);
            (void)rc;
            self->sdkMainLiveProcessingBusy = false;
        }, Qt::QueuedConnection);
    });

    return 0;
}

int MainWindow::saveFitsAsPNG_FromSdkFrame(const std::shared_ptr<SdkFrameData>& frame, bool ProcessBin, std::function<void(bool)> onComplete)
{
    const quint64 epoch = ++tilePyramidEpoch;
    QPointer<MainWindow> self(this);
    const bool processBinCopy = ProcessBin;
    auto frameCopy = frame;

    QtConcurrent::run([self, epoch, processBinCopy, frameCopy, onComplete]() {
        if (!self || !frameCopy) return;
        if (self->tilePyramidEpoch.load() != epoch) return;
        const int rc = self->saveFitsAsPNG_FromSdkFrame_Worker(frameCopy, processBinCopy);

        QMetaObject::invokeMethod(self, [self, epoch, rc, onComplete]() {
            if (!self) return;
            if (self->tilePyramidEpoch.load() != epoch) return;
            if (onComplete) onComplete(rc == 0);
            (void)rc;
            self->sdkMainLiveProcessingBusy = false;
        }, Qt::QueuedConnection);
    });

    return 0;
}

int MainWindow::saveFitsAsPNG_Worker(QString fitsFileName, bool ProcessBin)
{
    // 旧实现会在 saveFitsAsPNG() 一开始就触发下一帧拍摄（并且直接调用 startMainCameraCapture），
    // 在当前 SDK/定时器/线程队列/状态机体系下会与“出图链路”竞争同一相机资源，导致不稳定。
    // 新语义：仅当本帧完整出图成功后，才异步触发下一帧（QueuedConnection），避免递归/重入。
    Logger::Log("Starting to save FITS as PNG...", LogLevel::INFO, DeviceType::CAMERA);
    const quint64 epochAtStart = tilePyramidEpoch.load();
    
    cv::Mat image;
    cv::Mat originalImage16;
    Logger::Log("FITS file path: " + fitsFileName.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    int status = Tools::readFits(fitsFileName.toLocal8Bit().constData(), image);

    if (status != 0)
    {
        Logger::Log("Failed to read FITS file: " + fitsFileName.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
        return status;
    }
    if (image.empty())
    {
        Logger::Log("saveFitsAsPNG | readFits succeeded but image is empty: " + fitsFileName.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
        return -1;
    }
    if (image.type() == CV_8UC1 || image.type() == CV_8UC3 || image.type() == CV_16UC1)
    {
        originalImage16 = Tools::convert8UTo16U_BayerSafe(image, false);
        image.release();
    }
    else
    {
        Logger::Log("The current image data type is not supported for processing.", LogLevel::WARNING, DeviceType::CAMERA);
        return -1;
    }
    if (originalImage16.empty())
    {
        Logger::Log("saveFitsAsPNG | convert8UTo16U_BayerSafe returned empty image; skip medianBlur", LogLevel::ERROR, DeviceType::CAMERA);
        return -1;
    }

    // 创建MainCameraCFA的局部副本，防止多线程竞态条件导致的值污染
    QString localCameraCFA = MainCameraCFA;

    // 验证CFA值的合法性
    QStringList validCFAValues = {"RGGB", "BGGR", "GRBG", "GBRG", "RG", "BG", "GR", "GB", "", "null"};
    if (!validCFAValues.contains(localCameraCFA))
    {
        Logger::Log("saveFitsAsPNG | Invalid MainCameraCFA value detected: '" + localCameraCFA.toStdString() +
                   "'. Using empty (Mono mode) for this operation.", LogLevel::ERROR, DeviceType::CAMERA);
        localCameraCFA = "";
    }

    const int rc = processImageForFrontend(originalImage16, localCameraCFA, ProcessBin, fitsFileName);
    if (rc != 0) {
        return rc;
    }

    if (!fitsFileName.contains("ccd_simulator_original.fits"))
    {
        const QString destinationPath = QStringLiteral("/dev/shm/ccd_simulator_original.fits");
        QFile::remove(destinationPath);
        QFile::copy(fitsFileName, destinationPath);
    }

    return 0;
}

int MainWindow::saveFitsAsPNG_FromSdkFrame_Worker(std::shared_ptr<SdkFrameData> frame, bool ProcessBin)
{
    Logger::Log("Starting to save SDK frame as PNG...", LogLevel::INFO, DeviceType::CAMERA);
    if (!frame)
    {
        Logger::Log("saveFitsAsPNG_FromSdkFrame | frame is null", LogLevel::ERROR, DeviceType::CAMERA);
        return -1;
    }

    cv::Mat sourceImage;
    cv::Mat originalImage16;

    if (!frame->pixels.empty())
    {
        const size_t needPixels = static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height);
        if (frame->pixels.size() < needPixels)
        {
            Logger::Log("saveFitsAsPNG_FromSdkFrame | pixels buffer too small", LogLevel::ERROR, DeviceType::CAMERA);
            return -1;
        }
        sourceImage = cv::Mat(frame->height, frame->width, CV_16UC1,
                              const_cast<uint16_t*>(frame->pixels.data())).clone();
    }
    else if (frame->rawBuffer != nullptr && frame->rawBytes > 0)
    {
        if (frame->channels != 1 || (frame->bpp != 16 && frame->bpp != 8))
        {
            Logger::Log("saveFitsAsPNG_FromSdkFrame | unsupported rawBuffer format: bpp=" +
                        std::to_string(frame->bpp) + " channels=" + std::to_string(frame->channels),
                        LogLevel::ERROR, DeviceType::CAMERA);
            return -1;
        }

        const size_t pixelCount = static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height);
        const size_t needBytes = pixelCount * static_cast<size_t>(frame->bpp / 8);
        if (frame->rawBuffer->size() < needBytes || frame->rawBytes < needBytes)
        {
            Logger::Log("saveFitsAsPNG_FromSdkFrame | rawBuffer too small: needBytes=" +
                        std::to_string(needBytes) + " rawBytes=" + std::to_string(frame->rawBytes) +
                        " bufSize=" + std::to_string(frame->rawBuffer->size()),
                        LogLevel::ERROR, DeviceType::CAMERA);
            return -1;
        }

        const int cvType = (frame->bpp == 16) ? CV_16UC1 : CV_8UC1;
        sourceImage = cv::Mat(frame->height, frame->width, cvType, frame->rawBuffer->data()).clone();
    }
    else
    {
        Logger::Log("saveFitsAsPNG_FromSdkFrame | frame has no image payload", LogLevel::ERROR, DeviceType::CAMERA);
        return -1;
    }

    originalImage16 = Tools::convert8UTo16U_BayerSafe(sourceImage, false);
    if (originalImage16.empty())
    {
        Logger::Log("saveFitsAsPNG_FromSdkFrame | convert8UTo16U_BayerSafe returned empty image",
                    LogLevel::ERROR, DeviceType::CAMERA);
        return -1;
    }

    QString localCameraCFA = MainCameraCFA;
    QStringList validCFAValues = {"RGGB", "BGGR", "GRBG", "GBRG", "RG", "BG", "GR", "GB", "", "null"};
    if (!validCFAValues.contains(localCameraCFA))
    {
        Logger::Log("saveFitsAsPNG_FromSdkFrame | Invalid MainCameraCFA value detected: '" +
                    localCameraCFA.toStdString() + "'. Using empty (Mono mode) for this operation.",
                    LogLevel::ERROR, DeviceType::CAMERA);
        localCameraCFA = "";
    }

    const int rc = processImageForFrontend(originalImage16, localCameraCFA, ProcessBin, QStringLiteral("sdk_frame"));
    if (rc != 0) {
        return rc;
    }

    const std::string fitsPath = "/dev/shm/ccd_simulator.fits";
    SaveQhyFrameDataToFits(*frame, fitsPath);
    const QString destinationPath = QStringLiteral("/dev/shm/ccd_simulator_original.fits");
    QFile::remove(destinationPath);
    QFile::copy(QString::fromStdString(fitsPath), destinationPath);
    return 0;
}

int MainWindow::processImageForFrontend(const cv::Mat& inputImage16, const QString& cameraCFA, bool ProcessBin, const QString& sourceTag)
{
    emitCaptureTrace(QStringLiteral("backend_process_image_start"), currentCaptureTraceStartedAtMs,
                     QString("sourceTag=%1").arg(sourceTag));
    const qint64 processStageStartMs = QDateTime::currentMSecsSinceEpoch();
    const quint64 epochAtStart = tilePyramidEpoch.load();
    cv::Mat originalImage16 = inputImage16.clone();
    cv::Mat image16;
    QString effectiveCameraCFA = cameraCFA;

    // 中值滤波（可选）：大图上会显著增加耗时；默认在 fast 模式关闭
    if (tilePyramidFastEnableMedianBlur) {
        Logger::Log("Starting median blur...", LogLevel::INFO, DeviceType::CAMERA);
        try
        {
            cv::medianBlur(originalImage16, originalImage16, 3);
        }
        catch (const cv::Exception &e)
        {
            Logger::Log(std::string("saveFitsAsPNG | medianBlur failed: ") + e.what(), LogLevel::ERROR, DeviceType::CAMERA);
            return -1;
        }
        Logger::Log("Median blur applied successfully.", LogLevel::INFO, DeviceType::CAMERA);
    } else {
        Logger::Log("Median blur skipped (fast mode).", LogLevel::DEBUG, DeviceType::CAMERA);
    }

    // 使用局部CFA副本，避免全局变量在多线程环境中被污染
    bool isColor = !(effectiveCameraCFA == "" || effectiveCameraCFA == "null");
    Logger::Log("Camera color mode: " + std::string(isColor ? "Color" : "Mono") + " CFA: " + effectiveCameraCFA.toStdString() +
                " source: " + sourceTag.toStdString(), LogLevel::INFO, DeviceType::CAMERA);

    // 记录预览/解析保存使用的软件 bin 因子（用于前端对照调试）
    int binningFactor = 1;
    if (ProcessBin) {
        binningFactor = glMainCameraBinning;
        if (binningFactor < 1) binningFactor = 1;
        if (binningFactor > 16) binningFactor = 16;
        // 防御：若不是 2^N，则向上取整到最近的 2^N（例如 3->4, 6->8），并封顶 16
        int p2 = 1;
        while (p2 < binningFactor && p2 < 16) p2 <<= 1;
        if (p2 > 16) p2 = 16;
        binningFactor = p2;
    }

    if (ProcessBin && binningFactor != 1)
    {
        // 单色大图预览优先走 OpenCV INTER_AREA，避免通用 bin 路径带来的额外逐像素开销。
        if (!isColor && originalImage16.type() == CV_16UC1)
        {
            const int newWidth = std::max(1, originalImage16.cols / binningFactor);
            const int newHeight = std::max(1, originalImage16.rows / binningFactor);
            cv::resize(originalImage16, image16, cv::Size(newWidth, newHeight), 0, 0, cv::INTER_AREA);
        }
        // 使用新的Mat版本的PixelsDataSoftBin_Bayer函数
        else if (effectiveCameraCFA == "RGGB" || effectiveCameraCFA == "RG")
        {
            image16 = Tools::PixelsDataSoftBin_Bayer(originalImage16, binningFactor, binningFactor, BAYER_RGGB);
        }
        else if (effectiveCameraCFA == "BGGR" || effectiveCameraCFA == "BG")
        {
            image16 = Tools::PixelsDataSoftBin_Bayer(originalImage16, binningFactor, binningFactor, BAYER_BGGR);
        }
        else if (effectiveCameraCFA == "GRBG" || effectiveCameraCFA == "GR")
        {
            image16 = Tools::PixelsDataSoftBin_Bayer(originalImage16, binningFactor, binningFactor, BAYER_GRBG);
        }
        else if (effectiveCameraCFA == "GBRG" || effectiveCameraCFA == "GB")
        {
            image16 = Tools::PixelsDataSoftBin_Bayer(originalImage16, binningFactor, binningFactor, BAYER_GBRG);
        }
        else
        {
            image16 = Tools::processMatWithBinAvg(originalImage16, binningFactor, binningFactor, isColor, true);
        }
    }
    else
    {
        image16 = originalImage16.clone();
    }
    emitCaptureTrace(QStringLiteral("backend_process_preview_ready"), processStageStartMs,
                     QString("processBin=%1,preview=%2x%3,tileSource=%4x%5")
                         .arg(ProcessBin ? QStringLiteral("true") : QStringLiteral("false"))
                         .arg(image16.cols)
                         .arg(image16.rows)
                         .arg(originalImage16.cols)
                         .arg(originalImage16.rows));

    // 软件 bin 后图仅用于另一路 FITS 保存；瓦片源统一使用原图，避免在瓦片构建前提前合并。
    const cv::Mat& tileSourceImage = originalImage16;

    const int width = tileSourceImage.cols;
    const int height = tileSourceImage.rows;
    Logger::Log("MainCameraSize (tile source) dimensions: " + std::to_string(width) + "x" + std::to_string(height), LogLevel::INFO, DeviceType::CAMERA);
    emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(width) + ":" + QString::number(height));
    emit wsThread->sendMessageToClient("MainCameraBinning:1");

    if (tileSourceImage.empty())
    {
        Logger::Log("saveFitsAsPNG | tileSourceImage is empty, cannot save.", LogLevel::ERROR, DeviceType::CAMERA);
        return -1;
    }
    if (tileSourceImage.type() != CV_16UC1 && tileSourceImage.type() != CV_8UC1)
    {
        Logger::Log("saveFitsAsPNG | unsupported image type: " + std::to_string(tileSourceImage.type()), LogLevel::WARNING, DeviceType::CAMERA);
        return -1;
    }

    // 自动图像优化已前端化：后端主链路不再按帧计算自动白平衡增益。
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | saveFitsAsPNG | tileSourceImageSize = " +
                    std::to_string(tileSourceImage.cols) + "x" + std::to_string(tileSourceImage.rows),
                LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | saveFitsAsPNG | tileSourceImageType = " +
                    std::to_string(tileSourceImage.type()),
                LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | saveFitsAsPNG | effectiveCameraCFA = " +
                    effectiveCameraCFA.toStdString(),
                LogLevel::INFO, DeviceType::CAMERA);

    // ========================= 瓦片（视口驱动生成） =========================
    // 每张图像使用独立会话目录：live_<epoch>，便于区分并清理旧图
    const QString sessionId = QString("live_%1").arg(epochAtStart);

    // 确保 tiles 主目录存在（tmpfs：/dev/shm/capture-tiles/）
    QDir tilesDir(QString::fromStdString(tilePyramidPath));
    if (!tilesDir.exists()) {
        if (!tilesDir.mkpath(".")) {
            Logger::Log("Failed to create tiles directory: " + tilePyramidPath, LogLevel::ERROR, DeviceType::CAMERA);
            return -1;
        }
        QFile::setPermissions(QString::fromStdString(tilePyramidPath),
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
            QFileDevice::ReadGroup | QFileDevice::ExeGroup |
            QFileDevice::ReadOther | QFileDevice::ExeOther);
        Logger::Log("Created tiles directory (tmpfs): " + tilePyramidPath, LogLevel::INFO, DeviceType::CAMERA);
    }
    // 创建当前会话目录
    if (!tilesDir.mkpath(sessionId)) {
        Logger::Log("Failed to create session tiles directory: " + sessionId.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
        return -1;
    }

    // GPM 仅保留图像与瓦片元数据；自动优化参数改由前端基于 Z=0 计算。
    int maxMergeFactor = 16;
    const qint64 gpmCalcStartMs = QDateTime::currentMSecsSinceEpoch();
    TileGPM gpm = calculateGPM(tileSourceImage, effectiveCameraCFA, maxMergeFactor, /*enableHistogram=*/false);
    emitCaptureTrace(QStringLiteral("backend_calculate_gpm_done"), gpmCalcStartMs,
                     QString("sessionCandidate=live_%1,image=%2x%3,maxZoom=%4")
                         .arg(QString::number(static_cast<qulonglong>(epochAtStart)))
                         .arg(tileSourceImage.cols)
                         .arg(tileSourceImage.rows)
                         .arg(gpm.maxZoomLevel));
    gpm.sessionId = sessionId;
    gpm.previewWidth = image16.cols;
    gpm.previewHeight = image16.rows;
    gpm.previewBinningFactor = binningFactor;
    // 帧ID：与本次 saveFitsAsPNG() 的 epoch 对齐，用于前端/瓦片请求做“错帧丢弃”
    gpm.frameId = epochAtStart;
    gpm.buildMode = QStringLiteral("pyramid");
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | saveFitsAsPNG | sessionId = " +
                    sessionId.toStdString(),
                LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | saveFitsAsPNG | frameId = " +
                    std::to_string(static_cast<unsigned long long>(gpm.frameId)),
                LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | saveFitsAsPNG | gpmBlackWhite = " +
                    std::to_string(gpm.blackLevel) + "," + std::to_string(gpm.whiteLevel),
                LogLevel::INFO, DeviceType::CAMERA);

    // 保存“最新帧”供视口拖动/缩放时按需补瓦片（避免反复 readFits）
    {
        std::lock_guard<std::mutex> lk(tileFrameMutex);
        tileFrame.epoch = epochAtStart;
        tileFrame.sessionId = sessionId;
        tileFrame.frameId = epochAtStart;
        tileFrame.imageWidth = tileSourceImage.cols;
        tileFrame.imageHeight = tileSourceImage.rows;
        tileFrame.previewBinningFactor = binningFactor;
        tileFrame.tileSize = tilePyramidTileSize;
        tileFrame.maxZoomLevel = gpm.maxZoomLevel;
        tileFrame.cfa = effectiveCameraCFA;
        tileFrame.blackLevel = gpm.blackLevel;
        tileFrame.whiteLevel = gpm.whiteLevel;
        tileFrameImage16 = std::make_shared<cv::Mat>(tileSourceImage); // 共享底层buffer（ref-count）
        tileFramePreviewImage16 = std::make_shared<cv::Mat>(image16);
    }

    // 首帧链路只同步准备 z=0 预览瓦片：
    // - 保证前端收到 GPM 后，Z0 一定可立即拉取
    // - currentZ-1/currentZ 的局部高清瓦片改走后续异步补齐，避免它们阻塞首图
    const qint64 visibleTilesStartMs = QDateTime::currentMSecsSinceEpoch();
    generateVisibleTilesSync(epochAtStart, /*includeViewportLevels=*/false);
    emitCaptureTrace(QStringLiteral("backend_visible_tiles_ready"), visibleTilesStartMs,
                     QString("sessionId=%1,frameId=%2")
                         .arg(sessionId)
                         .arg(QString::number(static_cast<qulonglong>(epochAtStart))));

    // 发送GPM到前端
    if (tilePyramidEpoch.load() != epochAtStart) {
        Logger::Log("saveFitsAsPNG | cancelled before sending GPM (newer epoch)", LogLevel::WARNING, DeviceType::CAMERA);
        return -1;
    }
    sendGPMToClient(gpm);
    emitCaptureTrace(QStringLiteral("backend_tilegpm_sent"), currentCaptureTraceStartedAtMs,
                     QString("sessionId=%1,frameId=%2,serverNowMs=%3")
                         .arg(gpm.sessionId)
                         .arg(QString::number(static_cast<qulonglong>(gpm.frameId)))
                         .arg(QString::number(QDateTime::currentMSecsSinceEpoch())));

    // Z0/GPM 已经先行放出；当前视口的其它高清瓦片再异步补齐即可。
    const qint64 viewportScheduleStartMs = QDateTime::currentMSecsSinceEpoch();
    scheduleViewportTileGeneration();
    scheduleFullResTileCompletion();
    emitCaptureTrace(QStringLiteral("backend_schedule_viewport_tiles_done"), viewportScheduleStartMs,
                     QString("sessionId=%1,frameId=%2")
                         .arg(sessionId)
                         .arg(QString::number(static_cast<qulonglong>(epochAtStart))));
    // 删除其它旧会话的瓦片目录，仅保留当前 sessionId
    cleanupOldTileSessionDirs(sessionId);

    // 预览 FITS 挪到后台保存，避免首帧显示被额外 IO/编码阻塞。
    auto previewFitsImage = std::make_shared<cv::Mat>(image16);
    QtConcurrent::run([previewFitsImage]() {
        if (!previewFitsImage || previewFitsImage->empty()) return;
        Tools::SaveMatToFITS(*previewFitsImage);
        Logger::Log("Image saved as FITS.", LogLevel::INFO, DeviceType::CAMERA);
    });

    // 更新状态（回主线程，避免数据竞争；同时在这里触发 loop capture）
    QMetaObject::invokeMethod(this, [this, sessionId, epochAtStart]() {
        if (tilePyramidEpoch.load() != epochAtStart) {
            return;
        }
        isStagingImage = true;
        SavedImage = sessionId.toStdString();
        isSavePngSuccess = true;

        // Loop capture：仅当本帧“出图链路”完成成功后才触发下一帧，避免重入。
        if (LoopCaptureNum > 0) {
            LoopCaptureNum--;
            const int nextExpMs = glExpTime;

            // 防御：若状态机判定仍忙，则直接跳过（避免连环触发造成 Exposuring 竞争）
            if (glMainCameraStatu == "Exposuring" || sdkBurstActive.load()) {
                Logger::Log("LoopCapture | camera busy, skip next trigger", LogLevel::WARNING, DeviceType::CAMERA);
                return;
            }

            // 按当前主相机采集模式分流：
            // - Single：走 startMainCameraCapture（覆盖 INDI + SDK 单帧）
            // - Burst：走 SDK_BurstCapture（输出仍走 saveFitsAsPNG 链路）
            if (mainCameraCaptureMode == MainCameraCaptureMode::Burst) {
                Logger::Log("LoopCapture | trigger next BURST, exp_ms=" + std::to_string(nextExpMs) +
                                ", frames=" + std::to_string(LoopCaptureBurstFrames) +
                                ", remaining=" + std::to_string(LoopCaptureNum),
                            LogLevel::INFO, DeviceType::CAMERA);
                SDK_BurstCapture(nextExpMs, LoopCaptureBurstFrames);
            } else {
                Logger::Log("LoopCapture | trigger next SINGLE, exp_ms=" + std::to_string(nextExpMs) +
                                ", remaining=" + std::to_string(LoopCaptureNum),
                            LogLevel::INFO, DeviceType::CAMERA);
                startMainCameraCapture(nextExpMs);
            }
        }
    }, Qt::QueuedConnection);

    Logger::Log("Tile GPM sent; viewport-driven tiles scheduled.", LogLevel::INFO, DeviceType::CAMERA);
    // ========================= 视口驱动瓦片结束 =========================

    // 移除这里的 setCaptureComplete 调用，避免与外部调用重复
    // 调用者会在需要时调用 autoFocus->setCaptureComplete()
    // if (isAutoFocus)
    // {
    //     autoFocus->setCaptureComplete(fitsFileName);
    // }

    Logger::Log("saveFitsAsPNG completed successfully.", LogLevel::DEBUG, DeviceType::CAMERA);
    return 0;  // 🔧 修复：函数必须返回值，避免未定义行为导致内存错误

}

// ========================= 瓦片金字塔生成相关函数 =========================

/**
 * @brief 计算白平衡增益（基于灰度世界算法）
 * @param image16 16位原始图像
 * @param cfa CFA模式
 * @return QPair<gainR, gainB> R和B通道的增益值
 */
QString MainWindow::normalizeCfaPattern(const QString& cfa)
{
    const QString normalized = cfa.trimmed().toUpper();
    if (normalized == "RG" || normalized == "RGGB") return QStringLiteral("RGGB");
    if (normalized == "BG" || normalized == "BGGR") return QStringLiteral("BGGR");
    if (normalized == "GR" || normalized == "GRBG") return QStringLiteral("GRBG");
    if (normalized == "GB" || normalized == "GBRG") return QStringLiteral("GBRG");
    return normalized;
}

QPointF MainWindow::snapRoiOriginToBayerSafePhase(double roiX, double roiY, int roiWidth, int roiHeight) const
{
    const bool tileModeActive = (isStagingImage && !SavedImage.empty());
    const int roiCoordScale = tileModeActive ? 1 : std::max(1, glMainCameraBinning);

    int effMinX = 0;
    int effMinY = 0;
    int effW = std::max(0, glMainCCDSizeX);
    int effH = std::max(0, glMainCCDSizeY);
    if (sdkMainEffectiveAreaCacheValid) {
        effMinX = sdkMainEffectiveAreaMinX;
        effMinY = sdkMainEffectiveAreaMinY;
        effW = sdkMainEffectiveAreaWidth;
        effH = sdkMainEffectiveAreaHeight;
    }

    int scaledX = static_cast<int>(std::lround(roiX * roiCoordScale));
    int scaledY = static_cast<int>(std::lround(roiY * roiCoordScale));
    scaledX = std::max(0, scaledX);
    scaledY = std::max(0, scaledY);

    if (roiWidth > 0 && effW > 0) scaledX = std::min(scaledX, std::max(0, effW - roiWidth));
    if (roiHeight > 0 && effH > 0) scaledY = std::min(scaledY, std::max(0, effH - roiHeight));

    if (((effMinX + scaledX) & 1) != 0) {
        if (roiWidth > 0 && effW > 0 && scaledX + 1 <= std::max(0, effW - roiWidth)) {
            scaledX += 1;
        } else {
            scaledX = std::max(0, scaledX - 1);
        }
    }
    if (((effMinY + scaledY) & 1) != 0) {
        if (roiHeight > 0 && effH > 0 && scaledY + 1 <= std::max(0, effH - roiHeight)) {
            scaledY += 1;
        } else {
            scaledY = std::max(0, scaledY - 1);
        }
    }

    if (roiWidth > 0 && effW > 0) scaledX = std::min(scaledX, std::max(0, effW - roiWidth));
    if (roiHeight > 0 && effH > 0) scaledY = std::min(scaledY, std::max(0, effH - roiHeight));

    return QPointF(static_cast<double>(scaledX) / static_cast<double>(roiCoordScale),
                   static_cast<double>(scaledY) / static_cast<double>(roiCoordScale));
}

int MainWindow::getOpenCvBayerToBgrCode(const QString& cfa)
{
    const QString normalizedCfa = normalizeCfaPattern(cfa);
    if (normalizedCfa == "RGGB") return cv::COLOR_BayerRG2BGR;
    if (normalizedCfa == "BGGR") return cv::COLOR_BayerBG2BGR;
    if (normalizedCfa == "GRBG") return cv::COLOR_BayerGR2BGR;
    if (normalizedCfa == "GBRG") return cv::COLOR_BayerGB2BGR;
    return -1;
}

bool MainWindow::tryGetBayerPattern(const QString& cfa, BayerPattern& outPattern)
{
    const QString normalizedCfa = normalizeCfaPattern(cfa);
    if (normalizedCfa == "RGGB") {
        outPattern = BAYER_RGGB;
        return true;
    }
    if (normalizedCfa == "BGGR") {
        outPattern = BAYER_BGGR;
        return true;
    }
    if (normalizedCfa == "GRBG") {
        outPattern = BAYER_GRBG;
        return true;
    }
    if (normalizedCfa == "GBRG") {
        outPattern = BAYER_GBRG;
        return true;
    }
    return false;
}

cv::Mat MainWindow::downsampleTileImageForLevel(const cv::Mat& image, const QString& cfa, int scaleFactor)
{
    if (image.empty()) return cv::Mat();
    if (scaleFactor <= 1) return image;

    // 非 2^N 或非单通道 RAW 的路径，退回普通面积缩小。
    const bool isPowerOfTwo = (scaleFactor & (scaleFactor - 1)) == 0;
    BayerPattern bayerPattern = BAYER_RGGB;
    const bool useBayerSafe =
        isPowerOfTwo &&
        (image.type() == CV_16UC1 || image.type() == CV_8UC1 || image.type() == CV_32SC1) &&
        tryGetBayerPattern(cfa, bayerPattern);

    if (!useBayerSafe) {
        cv::Mat resized;
        cv::resize(image, resized,
                   cv::Size(std::max(1, image.cols / scaleFactor), std::max(1, image.rows / scaleFactor)),
                   0, 0, cv::INTER_AREA);
        return resized;
    }

    cv::Mat current = image;
    int factor = scaleFactor;
    while (factor > 1) {
        cv::Mat next = Tools::PixelsDataSoftBin_Bayer(current, 2, 2, bayerPattern);
        if (next.empty()) {
            Logger::Log("[TileDebug] event=downsampleTileImageForLevelFallback reason=BayerSafeBinFailed cfa=" +
                            cfa.toStdString() + " scaleFactor=" + std::to_string(scaleFactor),
                        LogLevel::WARNING, DeviceType::CAMERA);
            cv::Mat fallback;
            cv::resize(image, fallback,
                       cv::Size(std::max(1, image.cols / scaleFactor), std::max(1, image.rows / scaleFactor)),
                       0, 0, cv::INTER_AREA);
            return fallback;
        }
        current = std::move(next);
        factor >>= 1;
    }
    Logger::Log("[TileDebug] event=downsampleTileImageForLevel cfa=" + cfa.toStdString() +
                    " scaleFactor=" + std::to_string(scaleFactor) +
                    " input=" + std::to_string(image.cols) + "x" + std::to_string(image.rows) +
                    " output=" + std::to_string(current.cols) + "x" + std::to_string(current.rows),
                LogLevel::DEBUG, DeviceType::CAMERA);
    return current;
}

QString MainWindow::deriveCfaPatternForOffset(const QString& baseCfa, int shiftX, int shiftY)
{
    const QString normalizedCfa = normalizeCfaPattern(baseCfa);
    if (normalizedCfa.isEmpty() || normalizedCfa == "NULL" || normalizedCfa == "MONO" || normalizedCfa == "NULL") {
        return QString();
    }

    const bool oddX = (shiftX & 1) != 0;
    const bool oddY = (shiftY & 1) != 0;

    if (normalizedCfa == "RGGB") {
        if (!oddX && !oddY) return QStringLiteral("RGGB");
        if ( oddX && !oddY) return QStringLiteral("GRBG");
        if (!oddX &&  oddY) return QStringLiteral("GBRG");
        return QStringLiteral("BGGR");
    }
    if (normalizedCfa == "GRBG") {
        if (!oddX && !oddY) return QStringLiteral("GRBG");
        if ( oddX && !oddY) return QStringLiteral("RGGB");
        if (!oddX &&  oddY) return QStringLiteral("BGGR");
        return QStringLiteral("GBRG");
    }
    if (normalizedCfa == "GBRG") {
        if (!oddX && !oddY) return QStringLiteral("GBRG");
        if ( oddX && !oddY) return QStringLiteral("BGGR");
        if (!oddX &&  oddY) return QStringLiteral("RGGB");
        return QStringLiteral("GRBG");
    }
    if (normalizedCfa == "BGGR") {
        if (!oddX && !oddY) return QStringLiteral("BGGR");
        if ( oddX && !oddY) return QStringLiteral("GBRG");
        if (!oddX &&  oddY) return QStringLiteral("GRBG");
        return QStringLiteral("RGGB");
    }

    return normalizedCfa;
}

QString MainWindow::resolveFrameCfa(int frameStartX, int frameStartY) const
{
    const QString normalizedBase = normalizeCfaPattern(MainCameraCFA);
    if (normalizedBase.isEmpty() || normalizedBase == "NULL" || normalizedBase == "MONO") {
        return QString();
    }
    const int totalShiftX = MainCameraCFAOffsetX + frameStartX;
    const int totalShiftY = MainCameraCFAOffsetY + frameStartY;
    return deriveCfaPatternForOffset(normalizedBase, totalShiftX, totalShiftY);
}

QPair<double, double> MainWindow::calculateWhiteBalanceGains(const cv::Mat& image16, const QString& cfa, uint16_t offset)
{
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateWhiteBalanceGains | imageSize = " +
                    std::to_string(image16.cols) + "x" + std::to_string(image16.rows),
                LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateWhiteBalanceGains | cfa = " +
                    cfa.toStdString(),
                LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateWhiteBalanceGains | offset = " +
                    std::to_string(offset),
                LogLevel::INFO, DeviceType::MAIN);
    if (image16.empty() || cfa == "null" || cfa.isEmpty()) {
        Logger::Log("无法计算白平衡：图像为空或非彩色图像", LogLevel::WARNING, DeviceType::MAIN);
        return QPair<double, double>(1.0, 1.0);
    }

    if (image16.type() != CV_16UC1 || image16.rows < 2 || image16.cols < 2) {
        Logger::Log("无法计算白平衡：图像类型不是 CV_16UC1 或尺寸过小", LogLevel::WARNING, DeviceType::MAIN);
        return QPair<double, double>(1.0, 1.0);
    }

    std::vector<cv::Point> rOffsets;
    std::vector<cv::Point> gOffsets;
    std::vector<cv::Point> bOffsets;
    const QString normalizedCfa = normalizeCfaPattern(cfa);

    if (normalizedCfa == "RGGB") {
        rOffsets = { cv::Point(0, 0) };
        gOffsets = { cv::Point(1, 0), cv::Point(0, 1) };
        bOffsets = { cv::Point(1, 1) };
    } else if (normalizedCfa == "GRBG") {
        gOffsets = { cv::Point(0, 0), cv::Point(1, 1) };
        rOffsets = { cv::Point(1, 0) };
        bOffsets = { cv::Point(0, 1) };
    } else if (normalizedCfa == "GBRG") {
        gOffsets = { cv::Point(0, 0), cv::Point(1, 1) };
        bOffsets = { cv::Point(1, 0) };
        rOffsets = { cv::Point(0, 1) };
    } else if (normalizedCfa == "BGGR") {
        bOffsets = { cv::Point(0, 0) };
        gOffsets = { cv::Point(1, 0), cv::Point(0, 1) };
        rOffsets = { cv::Point(1, 1) };
    } else {
        Logger::Log("未知的CFA模式: " + cfa.toStdString(), LogLevel::WARNING, DeviceType::MAIN);
        return QPair<double, double>(1.0, 1.0);
    }

    const int rows = image16.rows;
    const int cols = image16.cols;
    const int sampleStep = std::max(2, static_cast<int>(std::floor(std::min(rows, cols) / 200.0)) * 2);

    struct SampleTriplet {
        double r = 0.0;
        double g1 = 0.0;
        double g2 = 0.0;
        double g = 0.0;
        double b = 0.0;
        double luma = 0.0;
    };
    std::vector<SampleTriplet> samples;
    samples.reserve(static_cast<size_t>((rows / sampleStep + 1) * (cols / sampleStep + 1)));

    auto correctedPixelAt = [&](int y, int x) -> double {
        if (y < 0 || y >= rows || x < 0 || x >= cols) return 0.0;
        const uint16_t raw = image16.at<uint16_t>(y, x);
        return static_cast<double>(raw > offset ? (raw - offset) : 0);
    };

    for (int y = 0; y < rows; y += sampleStep) {
        for (int x = 0; x < cols; x += sampleStep) {
            if ((y % (sampleStep * 2)) != 0 || (x % (sampleStep * 2)) != 0) continue;

            double r = 0.0;
            double g1 = 0.0;
            double g2 = 0.0;
            double b = 0.0;

            for (const auto& pos : rOffsets) r += correctedPixelAt(y + pos.y, x + pos.x);
            for (const auto& pos : bOffsets) b += correctedPixelAt(y + pos.y, x + pos.x);
            if (gOffsets.size() >= 1) g1 = correctedPixelAt(y + gOffsets[0].y, x + gOffsets[0].x);
            if (gOffsets.size() >= 2) g2 = correctedPixelAt(y + gOffsets[1].y, x + gOffsets[1].x);
            const double g = (g1 + g2) * 0.5;

            // 过滤黑电平附近样本与异常色块，避免整体发灰、ROI 发绿。
            if (r <= 32.0 || g1 <= 32.0 || g2 <= 32.0 || b <= 32.0) continue;
            const double maxRgb = std::max({r, g, b});
            const double minRgb = std::min({r, g, b});
            if (minRgb <= 0.0 || (maxRgb / minRgb) > 2.5) continue;

            samples.push_back({r, g1, g2, g, b, (r + 2.0 * g + b) * 0.25});
        }
    }

    if (samples.empty()) {
        Logger::Log("白平衡采样失败：有效 Bayer block 样本不足", LogLevel::WARNING, DeviceType::MAIN);
        return QPair<double, double>(1.0, 1.0);
    }

    std::vector<double> lumas;
    lumas.reserve(samples.size());
    for (const auto& sample : samples) lumas.push_back(sample.luma);
    std::sort(lumas.begin(), lumas.end());

    const size_t lowerIndex = std::min(lumas.size() - 1, static_cast<size_t>(std::floor((lumas.size() - 1) * 0.15)));
    const size_t upperIndex = std::min(lumas.size() - 1, static_cast<size_t>(std::floor((lumas.size() - 1) * 0.85)));
    const double lumaMin = lumas[lowerIndex];
    const double lumaMax = lumas[upperIndex];

    std::vector<double> g1Values;
    std::vector<double> g2Values;
    std::vector<double> greenPlaneRatios;
    std::vector<double> ratiosR;
    std::vector<double> ratiosB;
    g1Values.reserve(samples.size());
    g2Values.reserve(samples.size());
    greenPlaneRatios.reserve(samples.size());
    ratiosR.reserve(samples.size());
    ratiosB.reserve(samples.size());
    for (const auto& sample : samples) {
        if (sample.luma < lumaMin || sample.luma > lumaMax) continue;
        g1Values.push_back(sample.g1);
        g2Values.push_back(sample.g2);
        greenPlaneRatios.push_back(std::max(sample.g1, sample.g2) / std::max(std::min(sample.g1, sample.g2), 1.0));
        ratiosR.push_back(sample.g / std::max(sample.r, 1.0));
        ratiosB.push_back(sample.g / std::max(sample.b, 1.0));
    }

    if (g1Values.empty() || g2Values.empty() || ratiosR.empty() || ratiosB.empty()) {
        Logger::Log("白平衡采样失败：中亮度样本不足", LogLevel::WARNING, DeviceType::MAIN);
        return QPair<double, double>(1.0, 1.0);
    }

    auto trimmedMean = [](std::vector<double>& values) -> double {
        std::sort(values.begin(), values.end());
        const size_t lower = static_cast<size_t>(std::floor(values.size() * 0.1));
        const size_t upper = static_cast<size_t>(std::ceil(values.size() * 0.9));
        if (upper <= lower || upper > values.size()) {
            return values[values.size() / 2];
        }

        double sum = 0.0;
        size_t count = 0;
        for (size_t i = lower; i < upper; ++i) {
            sum += values[i];
            ++count;
        }
        return count > 0 ? (sum / static_cast<double>(count))
                         : values[values.size() / 2];
    };

    const double g1Mean = trimmedMean(g1Values);
    const double g2Mean = trimmedMean(g2Values);
    const double greenPlaneMismatch = std::abs(g1Mean - g2Mean) / std::max((g1Mean + g2Mean) * 0.5, 1.0);
    const double greenPlaneRatio = trimmedMean(greenPlaneRatios);
    if (greenPlaneMismatch > 0.12 || greenPlaneRatio > 1.15) {
        Logger::Log("白平衡已跳过：G1/G2 平面失衡过大, g1Mean=" + std::to_string(g1Mean) +
                    ", g2Mean=" + std::to_string(g2Mean) +
                    ", mismatch=" + std::to_string(greenPlaneMismatch) +
                    ", ratio=" + std::to_string(greenPlaneRatio),
                    LogLevel::WARNING, DeviceType::MAIN);
        return QPair<double, double>(1.0, 1.0);
    }

    const double gainR = std::max(0.1, std::min(3.0, trimmedMean(ratiosR)));
    const double gainB = std::max(0.1, std::min(3.0, trimmedMean(ratiosB)));

    Logger::Log("白平衡增益计算完成: R=" + std::to_string(gainR) +
                ", B=" + std::to_string(gainB) +
                ", offset=" + std::to_string(offset) +
                ", samples=" + std::to_string(samples.size()) +
                ", g1Mean=" + std::to_string(g1Mean) +
                ", g2Mean=" + std::to_string(g2Mean),
                LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateWhiteBalanceGains | sampleCount = " +
                    std::to_string(samples.size()),
                LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateWhiteBalanceGains | lumaRange = " +
                    std::to_string(lumaMin) + "," + std::to_string(lumaMax),
                LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateWhiteBalanceGains | gainR = " +
                    std::to_string(gainR),
                LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateWhiteBalanceGains | gainB = " +
                    std::to_string(gainB),
                LogLevel::INFO, DeviceType::MAIN);

    return QPair<double, double>(gainR, gainB);
}

MainWindow::TileGPM MainWindow::calculateGPM(const cv::Mat& image16, const QString& cfa, int maxMergeFactor, bool enableHistogram)
{
    TileGPM gpm;
    gpm.imageWidth = image16.cols;
    gpm.imageHeight = image16.rows;
    gpm.tileSize = tilePyramidTileSize;
    gpm.cfa = cfa;
    gpm.gainR = ImageGainR;
    gpm.gainB = ImageGainB;
    gpm.buildMode = QStringLiteral("pyramid");
    gpm.levelMode = (tileLevelMode.trimmed().toLower() == QStringLiteral("minmax"))
        ? QStringLiteral("minmax")
        : QStringLiteral("full");

    // 计算最大缩放层级（基于最低精度层的合并倍数 maxMergeFactor=2^N）
    // 例：maxMergeFactor=16 => level 0=16x16 ... level 4=1x1（共5层）
    if (maxMergeFactor < 1) maxMergeFactor = 1;
    if (maxMergeFactor > 16) maxMergeFactor = 16;
    // 若不是 2^N，则向上取整到最近的 2^N（例如 3->4, 6->8），并封顶 16；
    // 以保证与 generateTilePyramid 的“每层缩小2倍”递进一致
    int p2 = 1;
    while (p2 < maxMergeFactor && p2 < 16) p2 <<= 1;
    if (p2 > 16) p2 = 16;
    maxMergeFactor = p2;
    gpm.maxZoomLevel = 0;
    int factor = maxMergeFactor;
    while (factor > 1) {
        factor /= 2;
        gpm.maxZoomLevel++;
    }

    Logger::Log("Calculating GPM for image " + std::to_string(image16.cols) + "x" + std::to_string(image16.rows) + 
                ", maxZoomLevel=" + std::to_string(gpm.maxZoomLevel) + " (" + std::to_string(maxMergeFactor) + "x" + std::to_string(maxMergeFactor) + " -> 1x1)" +
                (enableHistogram ? ", histogram=on" : ", histogram=off"), LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | cfa = " +
                    cfa.toStdString(),
                LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | enableHistogram = " +
                    std::string(enableHistogram ? "true" : "false"),
                LogLevel::INFO, DeviceType::CAMERA);
    gpm.gainR = 1.0;
    gpm.gainB = 1.0;
    gpm.globalMin = 0.0;
    gpm.globalMax = 0.0;
    gpm.globalMean = 0.0;
    gpm.globalStdDev = 0.0;
    gpm.blackLevel = 0;
    gpm.whiteLevel = 65535;
    gpm.histogramBins = 0;
    gpm.histogramTotal = 0;
    gpm.histogram.clear();
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | auto optimization metadata disabled on backend; returning neutral display params",
                LogLevel::INFO, DeviceType::CAMERA);
    return gpm;

    auto tryComputeColorLumaStats = [&](double& meanLumaOut, double& stdLumaOut, size_t& sampleCountOut) -> bool {
        if (image16.type() != CV_16UC1 || image16.rows < 2 || image16.cols < 2) {
            return false;
        }

        const QString normalizedCfa = normalizeCfaPattern(cfa);
        std::vector<cv::Point> rOffsets;
        std::vector<cv::Point> gOffsets;
        std::vector<cv::Point> bOffsets;
        if (normalizedCfa == "RGGB") {
            rOffsets = { cv::Point(0, 0) };
            gOffsets = { cv::Point(1, 0), cv::Point(0, 1) };
            bOffsets = { cv::Point(1, 1) };
        } else if (normalizedCfa == "GRBG") {
            gOffsets = { cv::Point(0, 0), cv::Point(1, 1) };
            rOffsets = { cv::Point(1, 0) };
            bOffsets = { cv::Point(0, 1) };
        } else if (normalizedCfa == "GBRG") {
            gOffsets = { cv::Point(0, 0), cv::Point(1, 1) };
            bOffsets = { cv::Point(1, 0) };
            rOffsets = { cv::Point(0, 1) };
        } else if (normalizedCfa == "BGGR") {
            bOffsets = { cv::Point(0, 0) };
            gOffsets = { cv::Point(1, 0), cv::Point(0, 1) };
            rOffsets = { cv::Point(1, 1) };
        } else {
            return false;
        }

        const int rows = image16.rows;
        const int cols = image16.cols;
        const int sampleStep = std::max(2, static_cast<int>(std::floor(std::min(rows, cols) / 200.0)) * 2);

        long double sumLuma = 0.0L;
        long double sumSqLuma = 0.0L;
        size_t sampleCount = 0;

        auto rawPixelAt = [&](int y, int x) -> double {
            if (y < 0 || y >= rows || x < 0 || x >= cols) return 0.0;
            return static_cast<double>(image16.at<uint16_t>(y, x));
        };

        for (int y = 0; y < rows; y += sampleStep) {
            for (int x = 0; x < cols; x += sampleStep) {
                if ((y % (sampleStep * 2)) != 0 || (x % (sampleStep * 2)) != 0) continue;

                double r = 0.0;
                double g1 = 0.0;
                double g2 = 0.0;
                double b = 0.0;

                for (const auto& pos : rOffsets) r += rawPixelAt(y + pos.y, x + pos.x);
                for (const auto& pos : bOffsets) b += rawPixelAt(y + pos.y, x + pos.x);
                if (gOffsets.size() >= 1) g1 = rawPixelAt(y + gOffsets[0].y, x + gOffsets[0].x);
                if (gOffsets.size() >= 2) g2 = rawPixelAt(y + gOffsets[1].y, x + gOffsets[1].x);
                const double g = (g1 + g2) * 0.5;
                const double luma = (r + 2.0 * g + b) * 0.25;

                sumLuma += static_cast<long double>(luma);
                sumSqLuma += static_cast<long double>(luma) * static_cast<long double>(luma);
                ++sampleCount;
            }
        }

        if (sampleCount == 0) {
            return false;
        }

        const long double dn = static_cast<long double>(sampleCount);
        const long double meanLuma = sumLuma / dn;
        long double varLuma = (sumSqLuma / dn) - meanLuma * meanLuma;
        if (varLuma < 0.0L) varLuma = 0.0L;

        meanLumaOut = static_cast<double>(meanLuma);
        stdLumaOut = static_cast<double>(std::sqrt(varLuma));
        sampleCountOut = sampleCount;
        return true;
    };

    // 目标：尽量把全局统计/拉伸/直方图合并到“一次全图扫描”里（对 CV_16UC1 生效）
    // 性能：若不需要直方图且图像很大，则采用“子采样统计”（把耗时压到 ~10ms 级别）
    if (image16.type() == CV_16UC1) {
        const size_t nTotal = image16.total();
        gpm.histogramTotal = enableHistogram ? static_cast<uint64_t>(nTotal) : 0;
        gpm.histogramBins = enableHistogram ? 65536 : 0;
        std::vector<uint32_t> hist;
        if (enableHistogram) {
            static constexpr int HIST_BINS = 65536;
            hist.assign(static_cast<size_t>(HIST_BINS), 0u);
        }

        uint16_t minV = std::numeric_limits<uint16_t>::max();
        uint16_t maxV = 0;
        uint64_t sum = 0;
        unsigned __int128 sumSq = 0; // 防溢出：n*65535^2 可能接近 1e17

        size_t n = nTotal;
        if (!enableHistogram && nTotal > 2'000'000) {
            // 目标采样点数（约 1e6），用于快速估计 mean/stddev/min/max
            constexpr long double TARGET = 1'000'000.0L;
            const long double ratio = static_cast<long double>(nTotal) / TARGET;
            const int step = std::max(1, static_cast<int>(std::sqrt(std::max(1.0L, ratio))));
            n = 0;
            for (int r = 0; r < image16.rows; r += step) {
                const uint16_t* row = image16.ptr<uint16_t>(r);
                for (int c = 0; c < image16.cols; c += step) {
                    const uint16_t v = row[c];
                    if (v < minV) minV = v;
                    if (v > maxV) maxV = v;
                    sum += v;
                    sumSq += static_cast<unsigned __int128>(v) * static_cast<unsigned __int128>(v);
                    ++n;
                }
            }
            Logger::Log("GPM | subsample stats: total=" + std::to_string(nTotal) +
                            ", sampled=" + std::to_string(n) + ", step=" + std::to_string(step),
                        LogLevel::DEBUG, DeviceType::CAMERA);
        } else {
            if (image16.isContinuous()) {
                const uint16_t* p = image16.ptr<uint16_t>(0);
                for (size_t i = 0; i < n; ++i) {
                    const uint16_t v = p[i];
                    if (enableHistogram) ++hist[v];
                    if (v < minV) minV = v;
                    if (v > maxV) maxV = v;
                    sum += v;
                    sumSq += static_cast<unsigned __int128>(v) * static_cast<unsigned __int128>(v);
                }
            } else {
                for (int r = 0; r < image16.rows; ++r) {
                    const uint16_t* row = image16.ptr<uint16_t>(r);
                    for (int c = 0; c < image16.cols; ++c) {
                        const uint16_t v = row[c];
                        if (enableHistogram) ++hist[v];
                        if (v < minV) minV = v;
                        if (v > maxV) maxV = v;
                        sum += v;
                        sumSq += static_cast<unsigned __int128>(v) * static_cast<unsigned __int128>(v);
                    }
                }
            }
        }

        gpm.globalMin = static_cast<double>(minV);
        gpm.globalMax = static_cast<double>(maxV);
        if (enableHistogram) gpm.histogram = std::move(hist);

        // mean/stdDev（由 sum/sumSq 推导）
        const long double dn = static_cast<long double>(std::max<size_t>(1, n));
        const long double mean = static_cast<long double>(sum) / dn;
        const long double ex2 = static_cast<long double>(sumSq) / dn;
        long double var = ex2 - mean * mean;
        if (var < 0) var = 0; // 防浮点误差导致负数
        const long double stdDev = std::sqrt(var);
        gpm.globalMean = static_cast<double>(mean);
        gpm.globalStdDev = static_cast<double>(stdDev);
        Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | rawMean = " +
                        std::to_string(gpm.globalMean),
                    LogLevel::INFO, DeviceType::CAMERA);
        Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | rawStdDev = " +
                        std::to_string(gpm.globalStdDev),
                    LogLevel::INFO, DeviceType::CAMERA);

        const bool isColorRaw =
            !normalizeCfaPattern(cfa).isEmpty() &&
            normalizeCfaPattern(cfa) != "null";
        if (isColorRaw) {
            double meanLuma = 0.0;
            double stdLuma = 0.0;
            size_t lumaSamples = 0;
            if (tryComputeColorLumaStats(meanLuma, stdLuma, lumaSamples)) {
                gpm.globalMean = meanLuma;
                gpm.globalStdDev = stdLuma;
                Logger::Log("GPM | color RAW luma stats override: meanLuma=" + std::to_string(meanLuma) +
                                ", stdLuma=" + std::to_string(stdLuma) +
                                ", sampledBlocks=" + std::to_string(lumaSamples),
                            LogLevel::INFO, DeviceType::CAMERA);
                Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | meanLuma = " +
                                std::to_string(meanLuma),
                            LogLevel::INFO, DeviceType::CAMERA);
                Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | stdLuma = " +
                                std::to_string(stdLuma),
                            LogLevel::INFO, DeviceType::CAMERA);
                Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | lumaSampleCount = " +
                                std::to_string(lumaSamples),
                            LogLevel::INFO, DeviceType::CAMERA);
            } else {
                Logger::Log("GPM | color RAW luma stats unavailable, fallback to direct RAW stats",
                            LogLevel::WARNING, DeviceType::CAMERA);
            }
        }

        // black/white（等价于 Tools::GetAutoStretch(mode=0)，但不再额外扫描图像）
        {
            constexpr int a = 3;
            constexpr int b = 5;
            const long double stretchMean = static_cast<long double>(gpm.globalMean);
            const long double stretchStdDev = static_cast<long double>(gpm.globalStdDev);
            long double bx = stretchMean - stretchStdDev * a;
            long double wx = stretchMean + stretchStdDev * b;
            if (bx == wx) wx = bx + 10;

            const uint16_t maxValue = 65535;
            if (bx < 0) bx = 0;
            if (bx > maxValue) bx = maxValue;
            if (wx < 0) wx = 0;
            if (wx > maxValue) wx = maxValue;

            // 关键修复：
            // - bx/wx 可能是小数，直接 static_cast<uint16_t> 会截断为 0，导致 B==W==0（前端回退到 0-65535）
            // - 这里改为“先在整数域四舍五入”，并保证 white > black（至少相差 1）
            long long bi = std::llround(bx);
            long long wi = std::llround(wx);

            // 若区间过窄/反转，优先回退到 min/max（对低信号帧更稳）
            if (wi <= bi) {
                if (maxV > minV) {
                    bi = static_cast<long long>(minV);
                    wi = static_cast<long long>(maxV);
                } else {
                    wi = bi + 1;
                }
            }

            // 再次钳制到 16bit 范围，并保证 wi > bi
            bi = std::max<long long>(0, std::min<long long>(maxValue, bi));
            wi = std::max<long long>(0, std::min<long long>(maxValue, wi));
            if (wi <= bi) {
                wi = std::min<long long>(maxValue, bi + 1);
                if (wi <= bi) bi = std::max<long long>(0, wi - 1);
            }

            // 过曝保护：当画面整体接近饱和时，保留一个明显更亮的窗口，
            // 避免前端把“高亮满屏”拉成近黑。
            if (stretchMean >= static_cast<long double>(maxValue) * 0.85L ||
                minV >= static_cast<uint16_t>(maxValue * 0.75)) {
                wi = maxValue;
                bi = std::min<long long>(bi, maxValue / 2);
            }

            uint16_t B = static_cast<uint16_t>(bi);
            uint16_t W = static_cast<uint16_t>(wi);
            if (B >= maxValue && W >= maxValue) {
                B = 0;
                W = maxValue;
            }
            gpm.blackLevel = B;
            gpm.whiteLevel = W;
            Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | blackLevel = " +
                            std::to_string(gpm.blackLevel),
                        LogLevel::INFO, DeviceType::CAMERA);
            Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | whiteLevel = " +
                            std::to_string(gpm.whiteLevel),
                        LogLevel::INFO, DeviceType::CAMERA);
        }
    } else {
        // 兼容路径：非 16UC1 时，保留原策略（可按需扩展 8bit/3通道）
        cv::Scalar mean, stdDev;
        cv::meanStdDev(image16, mean, stdDev);
        gpm.globalMean = mean[0];
        gpm.globalStdDev = stdDev[0];

        double minVal, maxVal;
        cv::minMaxLoc(image16, &minVal, &maxVal);
        gpm.globalMin = minVal;
        gpm.globalMax = maxVal;

        uint16_t B = 0, W = 65535;
        Tools::GetAutoStretch(image16, 0, B, W);
        gpm.blackLevel = B;
        gpm.whiteLevel = W;
    }

    Logger::Log("GPM calculated: min=" + std::to_string(gpm.globalMin) + 
                ", max=" + std::to_string(gpm.globalMax) +
                ", mean=" + std::to_string(gpm.globalMean) +
                ", stdDev=" + std::to_string(gpm.globalStdDev) +
                ", blackLevel=" + std::to_string(gpm.blackLevel) +
                ", whiteLevel=" + std::to_string(gpm.whiteLevel),
                LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | globalMinMax = " +
                    std::to_string(gpm.globalMin) + "," + std::to_string(gpm.globalMax),
                LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | calculateGPM | globalMeanStdDev = " +
                    std::to_string(gpm.globalMean) + "," + std::to_string(gpm.globalStdDev),
                LogLevel::INFO, DeviceType::CAMERA);

    return gpm;
}

QString MainWindow::buildTileSessionId(quint64 frameId)
{
    return QStringLiteral("live_%1").arg(QString::number(static_cast<qulonglong>(frameId)));
}

int MainWindow::currentTilePreviewBinning() const
{
    std::lock_guard<std::mutex> lk(tileFrameMutex);
    return std::max(1, tileFrame.previewBinningFactor);
}

int MainWindow::calculateTileLevelFromScale(double scale, int maxZoomLevel)
{
    // 与前端 App.vue 的 calculateTileLevel 保持一致（10 档离散映射，scale 越小越“放大”）
    const double MIN_SCALE = 0.1;
    const double MAX_SCALE = 1.0;
    const int LEVELS = 10; // 0.1~1.0 共 10 档

    if (maxZoomLevel <= 0) return 0;
    const double s = std::max(MIN_SCALE, std::min(MAX_SCALE, scale));
    const double denom = (MAX_SCALE - MIN_SCALE);
    const double t = (denom != 0.0) ? ((s - MIN_SCALE) / denom) : 0.0; // 0..1
    const int idx = static_cast<int>(std::lround(t * (LEVELS - 1)));    // 0..9
    const int invIdx = (LEVELS - 1) - idx;                              // 9..0
    const int z = static_cast<int>(std::lround((static_cast<double>(invIdx) / (LEVELS - 1)) * maxZoomLevel));
    return std::max(0, std::min(maxZoomLevel, z));
}

void MainWindow::scheduleViewportTileGeneration()
{
    // 合并请求：若已有任务在跑，仅标记 pending，结束后再跑一轮（使用最新视口参数）。
    // 旧任务会在生成循环中检测 tileViewportRequestSeq，并尽快自行退出。
    if (tileViewportGenInFlight.exchange(true))
    {
        tileViewportGenPending = true;
        return;
    }

    tileViewportGenPending = false;

    TileFrameState st;
    std::shared_ptr<cv::Mat> img;
    {
        std::lock_guard<std::mutex> lk(tileFrameMutex);
        st = tileFrame;
        img = tileFrameImage16;
    }
    if (!img || img->empty() || st.sessionId.isEmpty() || st.imageWidth <= 0 || st.imageHeight <= 0)
    {
        tileViewportGenInFlight = false;
        return;
    }

    const quint64 epoch = st.epoch;
    const quint64 requestSeq = tileViewportRequestSeq.load();
    const int budgetMs = std::max(1, tilePyramidFastBudgetMs);
    QPointer<MainWindow> self(this);

    QtConcurrent::run([self, epoch, requestSeq, budgetMs]() {
        if (!self) return;
        self->generateViewportTiles_Once(epoch, requestSeq, budgetMs);

        QMetaObject::invokeMethod(self, [self]() {
            if (!self) return;
            self->tileViewportGenInFlight = false;
            if (self->tileViewportGenPending.exchange(false))
            {
                self->scheduleViewportTileGeneration();
                return;
            }
            self->scheduleFullResTileCompletion();
        }, Qt::QueuedConnection);
    });
}

void MainWindow::scheduleFullResTileCompletion()
{
    if (tileBuildMode.trimmed() != QStringLiteral("pyramid")) {
        return;
    }
    if (tileViewportGenInFlight.load()) {
        tileFullResGenPending = true;
        return;
    }
    if (tileFullResGenInFlight.exchange(true))
    {
        tileFullResGenPending = true;
        return;
    }

    tileFullResGenPending = false;

    TileFrameState st;
    std::shared_ptr<cv::Mat> img;
    {
        std::lock_guard<std::mutex> lk(tileFrameMutex);
        st = tileFrame;
        img = tileFrameImage16;
    }
    if (!img || img->empty() || st.sessionId.isEmpty() || st.imageWidth <= 0 || st.imageHeight <= 0)
    {
        tileFullResGenInFlight = false;
        return;
    }

    const quint64 epoch = st.epoch;
    const quint64 requestSeq = tileViewportRequestSeq.load();
    const int budgetMs = std::max(1, tilePyramidFastBudgetMs);
    QPointer<MainWindow> self(this);

    QtConcurrent::run([self, epoch, requestSeq, budgetMs]() {
        if (!self) return;
        self->generateFullResTiles_Once(epoch, requestSeq, budgetMs);

        QMetaObject::invokeMethod(self, [self]() {
            if (!self) return;
            self->tileFullResGenInFlight = false;
            if (self->tileViewportGenInFlight.load()) {
                self->tileFullResGenPending = true;
                return;
            }
            if (self->tileFullResGenPending.exchange(false))
            {
                self->scheduleFullResTileCompletion();
            }
        }, Qt::QueuedConnection);
    });
}

void MainWindow::generateViewportTiles_Once(quint64 epoch, quint64 requestSeq, int budgetMs)
{
    TileFrameState st;
    std::shared_ptr<cv::Mat> img;
    {
        std::lock_guard<std::mutex> lk(tileFrameMutex);
        st = tileFrame;
        img = tileFrameImage16;
    }
    if (!img || img->empty()) return;
    if (st.epoch != epoch) return;
    if (tilePyramidEpoch.load() != epoch) return;
    if (tileViewportRequestSeq.load() != requestSeq) return;

    const double vx = tileViewportX.load();
    const double vy = tileViewportY.load();
    const double sc = tileViewportScale.load();

    const int W = st.imageWidth;
    const int H = st.imageHeight;
    const int T = (st.tileSize > 0) ? st.tileSize : 512;
    const int maxZ = std::max(0, st.maxZoomLevel);
    const int requestedTargetZ = tileViewportTargetZ.load();
    const int requestedMaxZCap = tileViewportMaxZCap.load();
    const int effectiveMaxZ = (requestedMaxZCap >= 0)
        ? std::max(0, std::min(maxZ, requestedMaxZCap))
        : maxZ;

    // 视口矩形（尽量与前端一致；aspect 使用默认 16:9）
    const double aspect = (tileViewportAspect > 0.1) ? tileViewportAspect : (16.0 / 9.0);
    const double visibleX = std::isfinite(vx) ? vx : (W / 2.0);
    const double visibleY = std::isfinite(vy) ? vy : (H / 2.0);
    // 前端 scale 连续缩放最小可到 0.01（但瓦片层级映射仍会把 <0.1 clamp 到 0.1）
    // 这里用于计算可见区域宽高，应允许更小的 scale；否则会出现“视口瓦片范围算错/为0”。
    const double MIN_VIEW_SCALE = 0.01;
    const double MAX_VIEW_SCALE = 1.0;
    const double scale = std::max(MIN_VIEW_SCALE, std::min(MAX_VIEW_SCALE, (std::isfinite(sc) ? sc : 1.0)));

    const double visibleWidth = W * scale;
    const double visibleHeight = (aspect != 0.0) ? (visibleWidth / aspect) : (H * scale);
    const int fallbackZ = std::min(calculateTileLevelFromScale(scale, maxZ), effectiveMaxZ);
    const int currentZ = (requestedTargetZ >= 0)
        ? std::max(0, std::min(effectiveMaxZ, requestedTargetZ))
        : fallbackZ;
    const bool forceFullImageForCappedMode = (requestedMaxZCap >= 0);
    Logger::Log("[TileDebug] event=generateViewportTiles_OnceBegin session=" + st.sessionId.toStdString() +
                    " frameId=" + std::to_string(static_cast<unsigned long long>(st.frameId)) +
                    " epoch=" + std::to_string(static_cast<unsigned long long>(epoch)) +
                    " requestSeq=" + std::to_string(static_cast<unsigned long long>(requestSeq)) +
                    " requestedTargetZ=" + std::to_string(requestedTargetZ) +
                    " requestedMaxZCap=" + std::to_string(requestedMaxZCap) +
                    " effectiveMaxZ=" + std::to_string(effectiveMaxZ) +
                    " currentZ=" + std::to_string(currentZ) +
                    " scale=" + std::to_string(scale) +
                    " visibleCenter=" + std::to_string(visibleX) + "," + std::to_string(visibleY) +
                    " forceFullImageForCappedMode=" + std::string(forceFullImageForCappedMode ? "true" : "false"),
                LogLevel::INFO, DeviceType::CAMERA);

    QElapsedTimer timer;
    timer.start();

    const QString sessionTilePath = QString::fromStdString(tilePyramidPath) + st.sessionId;
    constexpr int TILE_BORDER = 2;

    struct TileReq { int x; int y; double prio; bool prefetch; };
    bool interruptedByBudget = false;
    auto shouldStop = [&]() -> bool {
        if (tilePyramidEpoch.load() != epoch) return true;
        if (tileViewportRequestSeq.load() != requestSeq) return true;
        if (budgetMs > 0 && timer.elapsed() > budgetMs) {
            interruptedByBudget = true;
            return true;
        }
        return false;
    };

    // 普通拍摄统一只围绕前端显式 targetZ 生成当前视口需要的层级；
    // z=0 整图预览已由 generateVisibleTilesSync() 提前准备。
    const bool minMaxOnly = (tileLevelMode.trimmed().toLower() == QStringLiteral("minmax"));
    const int coarseZ = std::max(0, currentZ - 1);
    std::vector<int> levelsToGenerate = minMaxOnly
        ? std::vector<int>{currentZ}
        : std::vector<int>{coarseZ, currentZ};
    std::sort(levelsToGenerate.begin(), levelsToGenerate.end());
    levelsToGenerate.erase(std::unique(levelsToGenerate.begin(), levelsToGenerate.end()), levelsToGenerate.end());
    const bool allowIdlePrefetch = !forceFullImageForCappedMode && currentZ > 0;
    constexpr int PREFETCH_RING = 1;

    int totalWritten = 0;
    QStringList readyTileKeys;
    std::set<uint64_t> doneKeys;
    bool completedVisibleTiles = true;
    bool completedAllWork = true;
    auto makeKey = [](int tz, int tx, int ty) -> uint64_t {
        return (static_cast<uint64_t>(tz) << 40) |
               (static_cast<uint64_t>(tx) << 20) |
               static_cast<uint64_t>(ty);
    };
    for (int z : levelsToGenerate)
    {
        if (shouldStop()) {
            completedVisibleTiles = false;
            completedAllWork = false;
            break;
        }

        const int levelScaleInt = 1 << std::max(0, (maxZ - z)); // 2^(maxZ-z)
        const double levelScale = static_cast<double>(levelScaleInt);
        const bool singlePreviewTile = (z == 0);

        // 与前端请求策略保持一致：
        // - z=0：整图单瓦片预览
        // - z>0：仅当前视口范围的高分辨率瓦片
        const bool fullImageForThisZ = (z == 0) || forceFullImageForCappedMode;
        const double left = fullImageForThisZ ? 0.0 : std::max(0.0, visibleX - visibleWidth / 2.0);
        const double top = fullImageForThisZ ? 0.0 : std::max(0.0, visibleY - visibleHeight / 2.0);
        const double right = fullImageForThisZ ? static_cast<double>(W) : std::min(static_cast<double>(W), left + visibleWidth);
        const double bottom = fullImageForThisZ ? static_cast<double>(H) : std::min(static_cast<double>(H), top + visibleHeight);

        const double levelLeft = left / levelScale;
        const double levelTop = top / levelScale;
        const double levelRight = right / levelScale;
        const double levelBottom = bottom / levelScale;

        const int levelWidth = static_cast<int>(std::ceil(W / levelScale));
        const int levelHeight = static_cast<int>(std::ceil(H / levelScale));
        const int maxTilesX = singlePreviewTile ? 1 : static_cast<int>(std::ceil(static_cast<double>(levelWidth) / T));
        const int maxTilesY = singlePreviewTile ? 1 : static_cast<int>(std::ceil(static_cast<double>(levelHeight) / T));
        const int startX = singlePreviewTile ? 0 : static_cast<int>(std::floor(levelLeft / T));
        const int startY = singlePreviewTile ? 0 : static_cast<int>(std::floor(levelTop / T));
        const int endX = singlePreviewTile ? 0 : static_cast<int>(std::ceil(levelRight / T) - 1.0);
        const int endY = singlePreviewTile ? 0 : static_cast<int>(std::ceil(levelBottom / T) - 1.0);
        const bool prefetchThisLevel = allowIdlePrefetch && (z == currentZ) && !singlePreviewTile;
        Logger::Log("[TileDebug] event=generateViewportTiles_OnceLevel session=" + st.sessionId.toStdString() +
                        " frameId=" + std::to_string(static_cast<unsigned long long>(st.frameId)) +
                        " z=" + std::to_string(z) +
                        " fullImage=" + std::string(fullImageForThisZ ? "true" : "false") +
                        " tileRange=[" + std::to_string(startX) + "," + std::to_string(startY) +
                        "]-[" + std::to_string(endX) + "," + std::to_string(endY) + "]" +
                        " maxTiles=" + std::to_string(maxTilesX) + "x" + std::to_string(maxTilesY) +
                        " prefetch=" + std::string(prefetchThisLevel ? "ring1" : "none"),
                    LogLevel::DEBUG, DeviceType::CAMERA);

        std::vector<TileReq> tiles;
        const int prefetchStartX = prefetchThisLevel ? std::max(0, startX - PREFETCH_RING) : startX;
        const int prefetchStartY = prefetchThisLevel ? std::max(0, startY - PREFETCH_RING) : startY;
        const int prefetchEndX = prefetchThisLevel ? std::min(maxTilesX - 1, endX + PREFETCH_RING) : endX;
        const int prefetchEndY = prefetchThisLevel ? std::min(maxTilesY - 1, endY + PREFETCH_RING) : endY;
        tiles.reserve(static_cast<size_t>(std::max(0, (prefetchEndX - prefetchStartX + 1) * (prefetchEndY - prefetchStartY + 1))));

        const int cxTile = singlePreviewTile ? 0 : static_cast<int>(std::floor((visibleX / levelScale) / T));
        const int cyTile = singlePreviewTile ? 0 : static_cast<int>(std::floor((visibleY / levelScale) / T));

        for (int ty = prefetchStartY; ty <= prefetchEndY; ++ty)
        {
            if (ty < 0 || ty >= maxTilesY) continue;
            for (int tx = prefetchStartX; tx <= prefetchEndX; ++tx)
            {
                if (tx < 0 || tx >= maxTilesX) continue;
                const bool isPrimaryTile = (tx >= startX && tx <= endX && ty >= startY && ty <= endY);
                if (!isPrimaryTile && !prefetchThisLevel) continue;
                const double dx = static_cast<double>(tx - cxTile);
                const double dy = static_cast<double>(ty - cyTile);
                const double dist = (z == 0) ? 0.0 : std::sqrt(dx * dx + dy * dy);
                const double penalty = isPrimaryTile ? 0.0 : 10000.0;
                tiles.push_back({tx, ty, penalty + dist, !isPrimaryTile});
            }
        }

        std::sort(tiles.begin(), tiles.end(), [](const TileReq& a, const TileReq& b) {
            return a.prio < b.prio;
        });

        const QString zDirPath = sessionTilePath + "/" + QString::number(z);
        QDir().mkpath(zDirPath);

        for (const auto& t : tiles)
        {
            if (shouldStop()) {
                if (!t.prefetch) {
                    completedVisibleTiles = false;
                }
                completedAllWork = false;
                break;
            }

            // 预算续跑时从同一 requestSeq 重新进入本函数。
            // 同一 epoch 下，瓦片内容只由 frame/z/x/y 决定，与“本轮从哪个视口触发”无关；
            // 因此可以安全跳过已经生成完成的瓦片，避免每轮都从高优先级第一块重新开始，
            // 导致 capped/full-image 模式下长期只补出局部一小块。
            const uint64_t packedKey = makeKey(z, t.x, t.y);
            {
                std::lock_guard<std::mutex> lk(tileGenDoneMutex);
                if (tileGenDoneEpoch == epoch && tileGenDoneKeys.find(packedKey) != tileGenDoneKeys.end()) {
                    continue;
                }
            }

            const QString xDirPath = zDirPath + "/" + QString::number(t.x);
            QDir().mkpath(xDirPath);
            const QString tileFilePath = xDirPath + "/" + QString::number(t.y) + ".bin";

            // 每次视口生成都直接覆盖写，不保留旧的“已生成”去重记录，避免视口变化后沿用旧数据。
            const int x0 = singlePreviewTile ? 0 : (t.x * T);
            const int y0 = singlePreviewTile ? 0 : (t.y * T);
            const int coreWidth = singlePreviewTile ? levelWidth : T;
            const int coreHeight = singlePreviewTile ? levelHeight : T;
            const cv::Rect wantedLevel(x0 - TILE_BORDER, y0 - TILE_BORDER,
                                       coreWidth + 2 * TILE_BORDER, coreHeight + 2 * TILE_BORDER);
            const cv::Rect wantedOrig(wantedLevel.x * levelScaleInt,
                                      wantedLevel.y * levelScaleInt,
                                      wantedLevel.width * levelScaleInt,
                                      wantedLevel.height * levelScaleInt);
            const cv::Rect boundsOrig(0, 0, img->cols, img->rows);
            const cv::Rect srcRect = wantedOrig & boundsOrig;

            cv::Mat padded;
            if (srcRect.width <= 0 || srcRect.height <= 0)
            {
                padded = cv::Mat::zeros(wantedOrig.height, wantedOrig.width, img->type());
            }
            else
            {
                cv::Mat src = (*img)(srcRect);
                const int topPad = srcRect.y - wantedOrig.y;
                const int leftPad = srcRect.x - wantedOrig.x;
                const int bottomPad = (wantedOrig.y + wantedOrig.height) - (srcRect.y + srcRect.height);
                const int rightPad = (wantedOrig.x + wantedOrig.width) - (srcRect.x + srcRect.width);
                cv::copyMakeBorder(src, padded, topPad, bottomPad, leftPad, rightPad, cv::BORDER_REPLICATE);
            }

            cv::Mat tileLevel;
            if (levelScaleInt == 1)
            {
                tileLevel = padded;
            }
            else
            {
                tileLevel = downsampleTileImageForLevel(padded, st.cfa, levelScaleInt);
                if (tileLevel.cols != wantedLevel.width || tileLevel.rows != wantedLevel.height) {
                    Logger::Log("[TileDebug] event=tileSizeMismatchAfterDownsample session=" + st.sessionId.toStdString() +
                                    " frameId=" + std::to_string(static_cast<unsigned long long>(st.frameId)) +
                                    " z=" + std::to_string(z) +
                                    " x=" + std::to_string(t.x) +
                                    " y=" + std::to_string(t.y) +
                                    " expected=" + std::to_string(wantedLevel.width) + "x" + std::to_string(wantedLevel.height) +
                                    " actual=" + std::to_string(tileLevel.cols) + "x" + std::to_string(tileLevel.rows),
                                LogLevel::WARNING, DeviceType::CAMERA);
                }
            }

            saveTileFast_NoMkdir(tileLevel, tileFilePath, TILE_BORDER);
            readyTileKeys.push_back(QString::number(z) + "/" + QString::number(t.x) + "/" + QString::number(t.y));
            doneKeys.insert(packedKey);
            totalWritten++;
        }
    }

    std::string levelSummary;
    levelSummary.clear();
    for (size_t i = 0; i < levelsToGenerate.size(); ++i)
    {
        if (i > 0) levelSummary += ",";
        levelSummary += std::to_string(levelsToGenerate[i]);
    }
    Logger::Log("[TileDebug] event=generateViewportTiles_OnceEnd session=" + st.sessionId.toStdString() +
               " frameId=" + std::to_string(static_cast<unsigned long long>(st.frameId)) +
               " wroteTiles=" + std::to_string(totalWritten) +
               " levels=" + levelSummary,
               LogLevel::DEBUG, DeviceType::CAMERA);
    if (!doneKeys.empty()) {
        std::lock_guard<std::mutex> lk(tileGenDoneMutex);
        if (tileGenDoneEpoch == epoch) {
            for (uint64_t key : doneKeys) tileGenDoneKeys.insert(key);
        }
    }
    if (!readyTileKeys.isEmpty()) {
        sendTileBatchReadyToClient(st.sessionId, epoch, readyTileKeys);
    }
    if (completedVisibleTiles && tileBuildMode.trimmed() == QStringLiteral("merged_single_level")) {
        sendTileGenerationCompleteToClient(st.sessionId, epoch);
    }
    if (!completedAllWork &&
        interruptedByBudget &&
        tilePyramidEpoch.load() == epoch &&
        tileViewportRequestSeq.load() == requestSeq) {
        Logger::Log("[TileDebug] event=generateViewportTiles_OnceBudgetRerun session=" +
                        st.sessionId.toStdString() + " frameId=" +
                        std::to_string(static_cast<unsigned long long>(st.frameId)) + " epoch=" +
                        std::to_string(static_cast<unsigned long long>(epoch)),
                    LogLevel::INFO, DeviceType::CAMERA);
        tileViewportGenPending = true;
    }
}

void MainWindow::generateFullResTiles_Once(quint64 epoch, quint64 requestSeq, int budgetMs)
{
    TileFrameState st;
    std::shared_ptr<cv::Mat> img;
    {
        std::lock_guard<std::mutex> lk(tileFrameMutex);
        st = tileFrame;
        img = tileFrameImage16;
    }
    if (!img || img->empty()) return;
    if (st.epoch != epoch) return;
    if (tilePyramidEpoch.load() != epoch) return;

    if (tileBuildMode.trimmed() == QStringLiteral("merged_single_level")) {
        const int z = std::max(0, st.maxZoomLevel);
        const int T = (st.tileSize > 0) ? st.tileSize : 512;
        const int levelWidth = st.imageWidth;
        const int levelHeight = st.imageHeight;
        const int maxTilesX = static_cast<int>(std::ceil(static_cast<double>(levelWidth) / T));
        const int maxTilesY = static_cast<int>(std::ceil(static_cast<double>(levelHeight) / T));
        const QString sessionTilePath = QString::fromStdString(tilePyramidPath) + st.sessionId;
        const QString zDirPath = sessionTilePath + "/" + QString::number(z);
        constexpr int TILE_BORDER = 2;
        constexpr int READY_BATCH_SIZE = 32;

        auto makeKey = [](int tz, int tx, int ty) -> uint64_t {
            return (static_cast<uint64_t>(tz) << 40) |
                   (static_cast<uint64_t>(tx) << 20) |
                   static_cast<uint64_t>(ty);
        };

        if (!QDir().mkpath(zDirPath)) {
            Logger::Log("generateFullResTiles_Once: failed to mkpath " + zDirPath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
            return;
        }

        int written = 0;
        int skippedExisting = 0;
        QStringList readyTileKeys;
        readyTileKeys.reserve(READY_BATCH_SIZE);

        for (int ty = 0; ty < maxTilesY; ++ty)
        {
            if (tilePyramidEpoch.load() != epoch) return;
            for (int tx = 0; tx < maxTilesX; ++tx)
            {
                if (tilePyramidEpoch.load() != epoch) return;

                const uint64_t packedKey = makeKey(z, tx, ty);
                {
                    std::lock_guard<std::mutex> lk(tileGenDoneMutex);
                    if (tileGenDoneEpoch == epoch && tileGenDoneKeys.find(packedKey) != tileGenDoneKeys.end()) {
                        skippedExisting++;
                        continue;
                    }
                }

                const QString xDirPath = zDirPath + "/" + QString::number(tx);
                if (!QDir().mkpath(xDirPath)) {
                    continue;
                }
                const QString tileFilePath = xDirPath + "/" + QString::number(ty) + ".bin";

                const int x0 = tx * T;
                const int y0 = ty * T;
                const cv::Rect wantedLevel(x0 - TILE_BORDER, y0 - TILE_BORDER,
                                           T + 2 * TILE_BORDER, T + 2 * TILE_BORDER);
                const cv::Rect boundsOrig(0, 0, img->cols, img->rows);
                const cv::Rect srcRect = wantedLevel & boundsOrig;

                cv::Mat padded;
                if (srcRect.width <= 0 || srcRect.height <= 0)
                {
                    padded = cv::Mat::zeros(wantedLevel.height, wantedLevel.width, img->type());
                }
                else
                {
                    cv::Mat src = (*img)(srcRect);
                    const int topPad = srcRect.y - wantedLevel.y;
                    const int leftPad = srcRect.x - wantedLevel.x;
                    const int bottomPad = (wantedLevel.y + wantedLevel.height) - (srcRect.y + srcRect.height);
                    const int rightPad = (wantedLevel.x + wantedLevel.width) - (srcRect.x + srcRect.width);
                    cv::copyMakeBorder(src, padded, topPad, bottomPad, leftPad, rightPad, cv::BORDER_REPLICATE);
                }

                saveTileFast_NoMkdir(padded, tileFilePath, TILE_BORDER);

                {
                    std::lock_guard<std::mutex> lk(tileGenDoneMutex);
                    if (tileGenDoneEpoch == epoch) {
                        tileGenDoneKeys.insert(packedKey);
                    }
                }

                readyTileKeys.push_back(QString::number(z) + "/" + QString::number(tx) + "/" + QString::number(ty));
                written++;

                if (readyTileKeys.size() >= READY_BATCH_SIZE) {
                    sendTileBatchReadyToClient(st.sessionId, epoch, readyTileKeys);
                    readyTileKeys.clear();
                }
            }
        }

        if (!readyTileKeys.isEmpty()) {
            sendTileBatchReadyToClient(st.sessionId, epoch, readyTileKeys);
        }

        sendTileGenerationCompleteToClient(st.sessionId, epoch);

        Logger::Log("generateFullResTiles_Once: wrote " + std::to_string(written) +
                   " full-res tiles at z=" + std::to_string(z) +
                   " for session " + st.sessionId.toStdString() +
                   " (skippedExisting=" + std::to_string(skippedExisting) + ")",
                   LogLevel::DEBUG, DeviceType::CAMERA);
        return;
    }

    if (tileBuildMode.trimmed() != QStringLiteral("pyramid")) return;

    QElapsedTimer timer;
    timer.start();

    const int maxZ = std::max(0, st.maxZoomLevel);
    const int T = (st.tileSize > 0) ? st.tileSize : 512;
    const QString sessionTilePath = QString::fromStdString(tilePyramidPath) + st.sessionId;
    constexpr int TILE_BORDER = 2;
    constexpr int READY_BATCH_SIZE = 32;
    bool completedAllTiles = true;
    bool interruptedByViewport = false;
    bool interruptedByBudget = false;

    auto makeKey = [](int tz, int tx, int ty) -> uint64_t {
        return (static_cast<uint64_t>(tz) << 40) |
               (static_cast<uint64_t>(tx) << 20) |
               static_cast<uint64_t>(ty);
    };

    auto shouldStop = [&]() -> bool {
        if (tilePyramidEpoch.load() != epoch) return true;
        if (tileViewportRequestSeq.load() != requestSeq) {
            interruptedByViewport = true;
            return true;
        }
        if (budgetMs > 0 && timer.elapsed() > budgetMs) {
            interruptedByBudget = true;
            return true;
        }
        return false;
    };

    int written = 0;
    int skippedExisting = 0;
    QStringList readyTileKeys;
    readyTileKeys.reserve(READY_BATCH_SIZE);
    std::set<uint64_t> doneKeys;

    const bool minMaxOnly = (tileLevelMode.trimmed().toLower() == QStringLiteral("minmax"));
    std::vector<int> fullResLevels;
    if (minMaxOnly) {
        fullResLevels = {0, maxZ};
    } else {
        fullResLevels.reserve(maxZ + 1);
        for (int z = 0; z <= maxZ; ++z) fullResLevels.push_back(z);
    }
    std::sort(fullResLevels.begin(), fullResLevels.end());
    fullResLevels.erase(std::unique(fullResLevels.begin(), fullResLevels.end()), fullResLevels.end());

    for (int z : fullResLevels)
    {
        if (shouldStop()) {
            completedAllTiles = false;
            break;
        }
        const int levelScaleInt = 1 << std::max(0, (maxZ - z));
        const int levelWidth = static_cast<int>(std::ceil(static_cast<double>(st.imageWidth) / levelScaleInt));
        const int levelHeight = static_cast<int>(std::ceil(static_cast<double>(st.imageHeight) / levelScaleInt));
        const int maxTilesX = (levelWidth + T - 1) / T;
        const int maxTilesY = (levelHeight + T - 1) / T;
        const QString zDirPath = sessionTilePath + "/" + QString::number(z);
        if (!QDir().mkpath(zDirPath)) {
            Logger::Log("generateFullResTiles_Once: failed to mkpath " + zDirPath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
            completedAllTiles = false;
            break;
        }

        for (int ty = 0; ty < maxTilesY; ++ty)
        {
            if (shouldStop()) {
                completedAllTiles = false;
                break;
            }
            for (int tx = 0; tx < maxTilesX; ++tx)
            {
                if (shouldStop()) {
                    completedAllTiles = false;
                    break;
                }

                const uint64_t packedKey = makeKey(z, tx, ty);
                {
                    std::lock_guard<std::mutex> lk(tileGenDoneMutex);
                    if (tileGenDoneEpoch == epoch && tileGenDoneKeys.find(packedKey) != tileGenDoneKeys.end()) {
                        skippedExisting++;
                        continue;
                    }
                }

                const QString xDirPath = zDirPath + "/" + QString::number(tx);
                if (!QDir().mkpath(xDirPath)) {
                    completedAllTiles = false;
                    continue;
                }
                const QString tileFilePath = xDirPath + "/" + QString::number(ty) + ".bin";

                const int x0 = tx * T;
                const int y0 = ty * T;
                const cv::Rect wantedLevel(x0 - TILE_BORDER, y0 - TILE_BORDER,
                                           T + 2 * TILE_BORDER, T + 2 * TILE_BORDER);
                const cv::Rect wantedOrig(wantedLevel.x * levelScaleInt,
                                          wantedLevel.y * levelScaleInt,
                                          wantedLevel.width * levelScaleInt,
                                          wantedLevel.height * levelScaleInt);
                const cv::Rect boundsOrig(0, 0, img->cols, img->rows);
                const cv::Rect srcRect = wantedOrig & boundsOrig;

                cv::Mat padded;
                if (srcRect.width <= 0 || srcRect.height <= 0)
                {
                    padded = cv::Mat::zeros(wantedOrig.height, wantedOrig.width, img->type());
                }
                else
                {
                    cv::Mat src = (*img)(srcRect);
                    const int topPad = srcRect.y - wantedOrig.y;
                    const int leftPad = srcRect.x - wantedOrig.x;
                    const int bottomPad = (wantedOrig.y + wantedOrig.height) - (srcRect.y + srcRect.height);
                    const int rightPad = (wantedOrig.x + wantedOrig.width) - (srcRect.x + srcRect.width);
                    cv::copyMakeBorder(src, padded, topPad, bottomPad, leftPad, rightPad, cv::BORDER_REPLICATE);
                }

                cv::Mat tileLevel;
                if (levelScaleInt == 1)
                {
                    tileLevel = padded;
                }
                else
                {
                    tileLevel = downsampleTileImageForLevel(padded, st.cfa, levelScaleInt);
                }

                saveTileFast_NoMkdir(tileLevel, tileFilePath, TILE_BORDER);
                doneKeys.insert(packedKey);
                readyTileKeys.push_back(QString::number(z) + "/" + QString::number(tx) + "/" + QString::number(ty));
                written++;

                if (readyTileKeys.size() >= READY_BATCH_SIZE) {
                    {
                        std::lock_guard<std::mutex> lk(tileGenDoneMutex);
                        if (tileGenDoneEpoch == epoch) {
                            for (uint64_t key : doneKeys) tileGenDoneKeys.insert(key);
                        }
                    }
                    doneKeys.clear();
                    sendTileBatchReadyToClient(st.sessionId, epoch, readyTileKeys);
                    readyTileKeys.clear();
                }
            }
            if (!completedAllTiles) break;
        }
        if (!completedAllTiles) break;
    }

    if (!doneKeys.empty()) {
        std::lock_guard<std::mutex> lk(tileGenDoneMutex);
        if (tileGenDoneEpoch == epoch) {
            for (uint64_t key : doneKeys) tileGenDoneKeys.insert(key);
        }
    }
    if (!readyTileKeys.isEmpty()) {
        sendTileBatchReadyToClient(st.sessionId, epoch, readyTileKeys);
    }
    if (completedAllTiles) {
        sendTileGenerationCompleteToClient(st.sessionId, epoch);
    } else if (tilePyramidEpoch.load() == epoch) {
        tileFullResGenPending = true;
    }

    Logger::Log("generateFullResTiles_Once: wrote " + std::to_string(written) +
               " pyramid tiles for session " + st.sessionId.toStdString() +
               " (skippedExisting=" + std::to_string(skippedExisting) +
               ", interruptedByViewport=" + std::string(interruptedByViewport ? "true" : "false") +
               ", interruptedByBudget=" + std::string(interruptedByBudget ? "true" : "false") + ")",
               LogLevel::DEBUG, DeviceType::CAMERA);
}

void MainWindow::generateVisibleTilesSync(quint64 epoch, bool includeViewportLevels)
{
    TileFrameState st;
    std::shared_ptr<cv::Mat> img;
    std::shared_ptr<cv::Mat> previewImg;
    {
        std::lock_guard<std::mutex> lk(tileFrameMutex);
        st = tileFrame;
        img = tileFrameImage16;
        previewImg = tileFramePreviewImage16;
    }
    if (!img || img->empty() || st.sessionId.isEmpty() || st.imageWidth <= 0 || st.imageHeight <= 0) return;
    if (st.epoch != epoch || tilePyramidEpoch.load() != epoch) return;

    const int W = st.imageWidth;
    const int H = st.imageHeight;
    const int T = (st.tileSize > 0) ? st.tileSize : 512;
    const int maxZ = std::max(0, st.maxZoomLevel);

    // 视口：无有效视口时按整图（scale=1）处理，即 z=0 全层
    const double vx = tileViewportX.load();
    const double vy = tileViewportY.load();
    const double sc = tileViewportScale.load();
    const int requestedTargetZ = tileViewportTargetZ.load();
    const int requestedMaxZCap = tileViewportMaxZCap.load();
    const int effectiveMaxZ = (requestedMaxZCap >= 0)
        ? std::max(0, std::min(maxZ, requestedMaxZCap))
        : maxZ;
    const double aspect = (tileViewportAspect > 0.1) ? tileViewportAspect : (16.0 / 9.0);
    const double visibleX = std::isfinite(vx) ? vx : (W / 2.0);
    const double visibleY = std::isfinite(vy) ? vy : (H / 2.0);
    const double MIN_VIEW_SCALE = 0.01;
    const double MAX_VIEW_SCALE = 1.0;
    const double scale = std::isfinite(sc) && sc > 0
        ? std::max(MIN_VIEW_SCALE, std::min(MAX_VIEW_SCALE, sc))
        : 1.0;
    const int fallbackZ = std::min(calculateTileLevelFromScale(scale, maxZ), effectiveMaxZ);
    const int currentZ = (requestedTargetZ >= 0)
        ? std::max(0, std::min(effectiveMaxZ, requestedTargetZ))
        : fallbackZ;
    const bool forceFullImageForCappedMode = (requestedMaxZCap >= 0);
    Logger::Log("[TileDebug] event=generateVisibleTilesSyncBegin session=" + st.sessionId.toStdString() +
                    " frameId=" + std::to_string(static_cast<unsigned long long>(st.frameId)) +
                    " epoch=" + std::to_string(static_cast<unsigned long long>(epoch)) +
                    " requestedTargetZ=" + std::to_string(requestedTargetZ) +
                    " scale=" + std::to_string(scale) +
                    " visibleCenter=" + std::to_string(visibleX) + "," + std::to_string(visibleY) +
                    " currentZ=" + std::to_string(currentZ) +
                    " requestedMaxZCap=" + std::to_string(requestedMaxZCap),
                LogLevel::INFO, DeviceType::CAMERA);
    // 同步预生成阶段：
    // - 默认只准备 z=0 整图预览，优先保证 TileGPM + 首图尽快出现
    // - 若显式要求，再额外同步准备 targetZ-1 / targetZ 当前视口层
    const bool minMaxOnly = (tileLevelMode.trimmed().toLower() == QStringLiteral("minmax"));
    std::vector<int> levelsToSync = {0};
    if (includeViewportLevels) {
        if (!minMaxOnly) {
            levelsToSync.push_back(std::max(0, currentZ - 1));
        }
        levelsToSync.push_back(currentZ);
    }
    std::sort(levelsToSync.begin(), levelsToSync.end());
    levelsToSync.erase(std::unique(levelsToSync.begin(), levelsToSync.end()), levelsToSync.end());

    const QString sessionTilePath = QString::fromStdString(tilePyramidPath) + st.sessionId;
    constexpr int TILE_BORDER = 2;
    auto makeKey = [](int tz, int tx, int ty) -> uint64_t {
        return (static_cast<uint64_t>(tz) << 40) |
               (static_cast<uint64_t>(tx) << 20) |
               static_cast<uint64_t>(ty);
    };
    {
        std::lock_guard<std::mutex> lk(tileGenDoneMutex);
        tileGenDoneEpoch = epoch;
        tileGenDoneKeys.clear();
        tileGenCompleteEpoch = 0;
    }
    int totalCount = 0;
    QStringList readyTileKeys;
    const double visibleWidth = W * scale;
    const double visibleHeight = (aspect != 0.0) ? (visibleWidth / aspect) : (H * scale);

    for (int z : levelsToSync) {
        if (tilePyramidEpoch.load() != epoch) return;
        const int levelScaleInt = 1 << std::max(0, (maxZ - z));
        const double levelScale = static_cast<double>(levelScaleInt);
        const bool singlePreviewTile = (z == 0);
        const bool fullImage = singlePreviewTile || forceFullImageForCappedMode;
        const double left = fullImage ? 0.0 : std::max(0.0, visibleX - visibleWidth / 2.0);
        const double top = fullImage ? 0.0 : std::max(0.0, visibleY - visibleHeight / 2.0);
        const double right = fullImage ? static_cast<double>(W) : std::min(static_cast<double>(W), left + visibleWidth);
        const double bottom = fullImage ? static_cast<double>(H) : std::min(static_cast<double>(H), top + visibleHeight);
        const double levelLeft = left / levelScale;
        const double levelTop = top / levelScale;
        const double levelRight = right / levelScale;
        const double levelBottom = bottom / levelScale;
        const int levelWidth = static_cast<int>(std::ceil(static_cast<double>(W) / levelScale));
        const int levelHeight = static_cast<int>(std::ceil(static_cast<double>(H) / levelScale));
        const int maxTilesX = singlePreviewTile ? 1 : static_cast<int>(std::ceil(static_cast<double>(levelWidth) / T));
        const int maxTilesY = singlePreviewTile ? 1 : static_cast<int>(std::ceil(static_cast<double>(levelHeight) / T));
        const int startX = singlePreviewTile ? 0 : static_cast<int>(std::floor(levelLeft / T));
        const int startY = singlePreviewTile ? 0 : static_cast<int>(std::floor(levelTop / T));
        const int endX = singlePreviewTile ? 0 : static_cast<int>(std::ceil(levelRight / T) - 1.0);
        const int endY = singlePreviewTile ? 0 : static_cast<int>(std::ceil(levelBottom / T) - 1.0);
        Logger::Log("[TileDebug] event=generateVisibleTilesSyncLevel session=" + st.sessionId.toStdString() +
                        " frameId=" + std::to_string(static_cast<unsigned long long>(st.frameId)) +
                        " z=" + std::to_string(z) +
                        " fullImage=" + std::string(fullImage ? "true" : "false") +
                        " tileRange=[" + std::to_string(startX) + "," + std::to_string(startY) +
                        "]-[" + std::to_string(endX) + "," + std::to_string(endY) + "]" +
                        " maxTiles=" + std::to_string(maxTilesX) + "x" + std::to_string(maxTilesY),
                    LogLevel::DEBUG, DeviceType::CAMERA);

        const QString zDirPath = sessionTilePath + "/" + QString::number(z);
        if (!QDir().mkpath(zDirPath)) {
            Logger::Log("generateVisibleTilesSync: failed to mkpath " + zDirPath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
            continue;
        }
        std::set<uint64_t> doneKeys;
        for (int ty = startY; ty <= endY; ++ty) {
            if (ty < 0 || ty >= maxTilesY) continue;
            for (int tx = startX; tx <= endX; ++tx) {
                if (tx < 0 || tx >= maxTilesX) continue;
                if (tilePyramidEpoch.load() != epoch) return;
                const uint64_t key = makeKey(z, tx, ty);
                doneKeys.insert(key);
                totalCount++;
                const QString xDirPath = zDirPath + "/" + QString::number(tx);
                if (!QDir().mkpath(xDirPath)) continue;
                const QString tileFilePath = xDirPath + "/" + QString::number(ty) + ".bin";

                if (singlePreviewTile && previewImg && !previewImg->empty()) {
                    cv::Mat previewTile;
                    cv::copyMakeBorder(*previewImg, previewTile,
                                       TILE_BORDER, TILE_BORDER, TILE_BORDER, TILE_BORDER,
                                       cv::BORDER_REPLICATE);
                    saveTileFast_NoMkdir(previewTile, tileFilePath, TILE_BORDER);
                    const QString readyKey =
                        QString::number(z) + "/" + QString::number(tx) + "/" + QString::number(ty);
                    // 首屏体验优化：z=0 预览瓦片一旦原子写完，立即单独放行给前端。
                    // 局部放大的清晰细节仍由后续 currentZ-1/currentZ 瓦片继续覆盖，不受影响。
                    sendTileBatchReadyToClient(st.sessionId, epoch, QStringList{readyKey});
                    continue;
                }

                const int x0 = singlePreviewTile ? 0 : (tx * T);
                const int y0 = singlePreviewTile ? 0 : (ty * T);
                const int coreWidth = singlePreviewTile ? levelWidth : T;
                const int coreHeight = singlePreviewTile ? levelHeight : T;
                const cv::Rect wantedLevel(x0 - TILE_BORDER, y0 - TILE_BORDER,
                                           coreWidth + 2 * TILE_BORDER, coreHeight + 2 * TILE_BORDER);
                const cv::Rect wantedOrig(wantedLevel.x * levelScaleInt,
                                          wantedLevel.y * levelScaleInt,
                                          wantedLevel.width * levelScaleInt,
                                          wantedLevel.height * levelScaleInt);
                const cv::Rect boundsOrig(0, 0, img->cols, img->rows);
                const cv::Rect srcRect = wantedOrig & boundsOrig;

                cv::Mat padded;
                if (srcRect.width <= 0 || srcRect.height <= 0) {
                    padded = cv::Mat::zeros(wantedOrig.height, wantedOrig.width, img->type());
                } else {
                    cv::Mat src = (*img)(srcRect);
                    const int topPad = srcRect.y - wantedOrig.y;
                    const int leftPad = srcRect.x - wantedOrig.x;
                    const int bottomPad = (wantedOrig.y + wantedOrig.height) - (srcRect.y + srcRect.height);
                    const int rightPad = (wantedOrig.x + wantedOrig.width) - (srcRect.x + srcRect.width);
                    cv::copyMakeBorder(src, padded, topPad, bottomPad, leftPad, rightPad, cv::BORDER_REPLICATE);
                }
                cv::Mat tileLevel;
                if (levelScaleInt == 1) {
                    tileLevel = padded;
                } else {
                    tileLevel = downsampleTileImageForLevel(padded, st.cfa, levelScaleInt);
                    if (tileLevel.cols != wantedLevel.width || tileLevel.rows != wantedLevel.height) {
                        Logger::Log("[TileDebug] event=tileSizeMismatchAfterDownsample session=" + st.sessionId.toStdString() +
                                        " frameId=" + std::to_string(static_cast<unsigned long long>(st.frameId)) +
                                        " z=" + std::to_string(z) +
                                        " x=" + std::to_string(tx) +
                                        " y=" + std::to_string(ty) +
                                        " expected=" + std::to_string(wantedLevel.width) + "x" + std::to_string(wantedLevel.height) +
                                        " actual=" + std::to_string(tileLevel.cols) + "x" + std::to_string(tileLevel.rows),
                                    LogLevel::WARNING, DeviceType::CAMERA);
                    }
                }
                saveTileFast_NoMkdir(tileLevel, tileFilePath, TILE_BORDER);
                readyTileKeys.push_back(QString::number(z) + "/" + QString::number(tx) + "/" + QString::number(ty));
            }
        }
        {
            std::lock_guard<std::mutex> lk(tileGenDoneMutex);
            for (uint64_t k : doneKeys) tileGenDoneKeys.insert(k);
        }
    }
    std::string levelSummary;
    levelSummary.clear();
    for (size_t i = 0; i < levelsToSync.size(); ++i)
    {
        if (i > 0) levelSummary += ",";
        levelSummary += std::to_string(levelsToSync[i]);
    }
    Logger::Log("[TileDebug] event=generateVisibleTilesSyncEnd session=" + st.sessionId.toStdString() +
               " frameId=" + std::to_string(static_cast<unsigned long long>(st.frameId)) +
               " wroteTiles=" + std::to_string(totalCount) +
               " levels=" + levelSummary,
               LogLevel::DEBUG, DeviceType::CAMERA);
    if (!readyTileKeys.isEmpty()) {
        sendTileBatchReadyToClient(st.sessionId, epoch, readyTileKeys);
    }
}

void MainWindow::saveTile(const cv::Mat& tile, int z, int x, int y, const QString& sessionId, int border)
{
    // 构建瓦片存储路径: tiles/{sessionId}/{z}/{x}/{y}.bin
    QString tileDirPath = QString::fromStdString(tilePyramidPath) + sessionId + "/" + 
                          QString::number(z) + "/" + QString::number(x) + "/";
    QString tileFilePath = tileDirPath + QString::number(y) + ".bin";

    // 创建目录
    QDir dir;
    if (!dir.mkpath(tileDirPath)) {
        Logger::Log("Failed to create tile directory: " + tileDirPath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
        return;
    }

    // 保存瓦片为二进制文件 (原始16位数据)
    std::ofstream outFile(tileFilePath.toStdString(), std::ios::binary);
    if (!outFile) {
        Logger::Log("Failed to open tile file for writing: " + tileFilePath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
        return;
    }

    // 写入瓦片元数据头 (16字节)
    int32_t width = tile.cols;
    int32_t height = tile.rows;
    int32_t type = tile.type();  // CV_16UC1
    // reserved：用于向前端传递“额外边界像素数”，前端会在去马赛克/拉伸后裁剪掉该边界
    int32_t reserved = border;
    outFile.write(reinterpret_cast<const char*>(&width), sizeof(int32_t));
    outFile.write(reinterpret_cast<const char*>(&height), sizeof(int32_t));
    outFile.write(reinterpret_cast<const char*>(&type), sizeof(int32_t));
    outFile.write(reinterpret_cast<const char*>(&reserved), sizeof(int32_t));

    // 写入瓦片像素数据
    if (tile.isContinuous()) {
        outFile.write(reinterpret_cast<const char*>(tile.data), 
                      static_cast<std::streamsize>(tile.total() * tile.elemSize()));
    } else {
        for (int r = 0; r < tile.rows; ++r) {
            outFile.write(reinterpret_cast<const char*>(tile.ptr(r)), 
                          static_cast<std::streamsize>(tile.cols * tile.elemSize()));
        }
    }

    outFile.close();
}

void MainWindow::saveTileFast_NoMkdir(const cv::Mat& tile, const QString& tileFilePath, int border)
{
    const bool isZ0Tile =
        tileFilePath.contains(QStringLiteral("/0/0/0.bin")) ||
        tileFilePath.endsWith(QStringLiteral("\\0\\0\\0.bin"));
    const qint64 z0WriteStartMs = isZ0Tile ? QDateTime::currentMSecsSinceEpoch() : 0;
    if (isZ0Tile) {
        emitCaptureTrace(QStringLiteral("backend_z0_tile_write_start"), currentCaptureTraceStartedAtMs,
                         QString("path=%1,width=%2,height=%3")
                             .arg(tileFilePath)
                             .arg(tile.cols)
                             .arg(tile.rows));
    }

    // 原子写入：避免前端 fetch 在文件写入中途读到“半瓦片”，导致解析失败/花屏/长时间不刷新。
    // QSaveFile 会写入临时文件，commit 时原子替换目标文件（同一文件系统内）。
    QSaveFile file(tileFilePath);
    file.setDirectWriteFallback(true);
    if (!file.open(QIODevice::WriteOnly)) {
        Logger::Log("Failed to open tile file for writing: " + tileFilePath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
        return;
    }

    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);

    const int32_t width = tile.cols;
    const int32_t height = tile.rows;
    const int32_t type = tile.type();
    const int32_t reserved = border;
    out << width;
    out << height;
    out << type;
    out << reserved;

    if (tile.isContinuous()) {
        const qint64 bytes = static_cast<qint64>(tile.total() * tile.elemSize());
        if (file.write(reinterpret_cast<const char*>(tile.data), bytes) != bytes) {
            Logger::Log("Failed to write tile bytes: " + tileFilePath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
            file.cancelWriting();
            return;
        }
    } else {
        const qint64 rowBytes = static_cast<qint64>(tile.cols * tile.elemSize());
        for (int r = 0; r < tile.rows; ++r) {
            if (file.write(reinterpret_cast<const char*>(tile.ptr(r)), rowBytes) != rowBytes) {
                Logger::Log("Failed to write tile row bytes: " + tileFilePath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
                file.cancelWriting();
                return;
            }
        }
    }

    if (!file.commit()) {
        Logger::Log("Failed to commit tile file (atomic replace): " + tileFilePath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
        return;
    }

    if (isZ0Tile) {
        emitCaptureTrace(QStringLiteral("backend_z0_tile_write_done"), z0WriteStartMs,
                         QString("path=%1,width=%2,height=%3,fileBytes=%4")
                             .arg(tileFilePath)
                             .arg(tile.cols)
                             .arg(tile.rows)
                             .arg(static_cast<qint64>(QFileInfo(tileFilePath).size())));
    }
}

MainWindow::TileGPM MainWindow::generateTilePyramid(const cv::Mat& image16, const QString& sessionId, const QString& cfa, int maxMergeFactor, bool enableHistogram)
{
    Logger::Log("Starting tile pyramid generation for session: " + sessionId.toStdString(), LogLevel::INFO, DeviceType::CAMERA);

    // 取消机制：新帧到来时 tilePyramidEpoch 会递增，旧任务应尽快退出
    const quint64 epochAtStart = tilePyramidEpoch.load();
    QElapsedTimer budgetTimer;
    budgetTimer.start();
    const int budgetMs = std::max(0, tilePyramidFastBudgetMs);

    // 1. 计算GPM
    TileGPM gpm = calculateGPM(image16, cfa, maxMergeFactor, enableHistogram);
    gpm.sessionId = sessionId;
    Logger::Log("Tile pyramid | step GPM done", LogLevel::INFO, DeviceType::CAMERA);

    // 2. 确保 live 目录存在（不删除，直接覆盖写瓦片，避免 SD 卡/磁盘抖动）
    QString sessionTilePath = QString::fromStdString(tilePyramidPath) + sessionId;
    QDir().mkpath(sessionTilePath);
    Logger::Log("Tile pyramid | step session dir mkpath done (overwrite mode)", LogLevel::INFO, DeviceType::CAMERA);

    // 3. 生成各层级瓦片
    // 反向金字塔：level 0是最低精度（16x16合并），maxZoomLevel是原图
    const int T = tilePyramidTileSize;
    
    // 首先生成所有层级的图像（从最高精度到最低精度）
    std::vector<cv::Mat> pyramidLevels;
    pyramidLevels.resize(gpm.maxZoomLevel + 1);
    
    // 最高层级是原图
    pyramidLevels[gpm.maxZoomLevel] = image16.clone();
    Logger::Log("Tile pyramid | clone top level " + std::to_string(image16.cols) + "x" + std::to_string(image16.rows), LogLevel::INFO, DeviceType::CAMERA);
    
    // 从高精度向低精度生成（每次合并2x2像素为1像素）
    for (int z = gpm.maxZoomLevel - 1; z >= 0; --z) {
        const cv::Mat& higherLevel = pyramidLevels[z + 1];
        cv::Mat lowerLevel;

        // 彩色 RAW 必须保持 CFA 相位，不能直接在 Bayer 单通道上做普通面积缩小。
        lowerLevel = downsampleTileImageForLevel(higherLevel, cfa, 2);
        pyramidLevels[z] = lowerLevel;
    }
    Logger::Log("Tile pyramid | build pyramid levels (resize chain) done", LogLevel::INFO, DeviceType::CAMERA);
    
    // 现在生成每个层级的瓦片
    const int requestedSyncMaxZ = std::max(0, tilePyramidFastSyncMaxZ);
    const int syncMaxZ = std::min(requestedSyncMaxZ, gpm.maxZoomLevel);
    int writtenMaxZ = -1;

    for (int z = 0; z <= gpm.maxZoomLevel; ++z) {
        // 预算与取消：同步阶段只写到 syncMaxZ，且尽量把耗时控制在 budgetMs 内
        if (z > syncMaxZ) break;
        if (budgetMs > 0 && budgetTimer.elapsed() > budgetMs) break;
        if (tilePyramidEpoch.load() != epochAtStart) {
            Logger::Log("Tile pyramid | cancelled by newer epoch (sync phase)", LogLevel::WARNING, DeviceType::CAMERA);
            return gpm;
        }

        const cv::Mat& currentLevel = pyramidLevels[z];
        int levelWidth = currentLevel.cols;
        int levelHeight = currentLevel.rows;
        int tilesX = (levelWidth + T - 1) / T;  // 向上取整
        int tilesY = (levelHeight + T - 1) / T;
        int tileCount = tilesX * tilesY;
        
        // 计算当前层级的合并倍数
        int mergeFactor = 1 << (gpm.maxZoomLevel - z);  // 2^(maxZoomLevel - z)

        Logger::Log("Generating level " + std::to_string(z) + " (merge factor " + std::to_string(mergeFactor) + "x" + std::to_string(mergeFactor) + "): " + 
                    std::to_string(levelWidth) + "x" + std::to_string(levelHeight) + 
                    ", tiles: " + std::to_string(tilesX) + "x" + std::to_string(tilesY) + " (" + std::to_string(tileCount) + " total)",
                    LogLevel::INFO, DeviceType::CAMERA);

        // 生成当前层级的所有瓦片
        // 为了让前端“按瓦片局部去马赛克(Bayer->RGBA)”不出现接缝，
        // 这里为每个瓦片额外带一点邻域边界像素（前端渲染时会裁掉）。
        // 说明：边界像素来自相邻区域，超出图像边界时用 BORDER_REPLICATE 补齐。
        constexpr int TILE_BORDER = 2; // 像素；2 对 OpenCV Bayer bilinear 去马赛克更稳
        // 目录预创建：按层级 z 和列 tx 创建目录，避免每个瓦片都 mkpath（系统调用成本高）
        const QString zDirPath = sessionTilePath + "/" + QString::number(z);
        QDir().mkpath(zDirPath);
        for (int tx = 0; tx < tilesX; ++tx) {
            const QString xDirPath = zDirPath + "/" + QString::number(tx);
            QDir().mkpath(xDirPath);
            for (int ty = 0; ty < tilesY; ++ty) {
                QString tileFilePath = xDirPath + "/" + QString::number(ty) + ".bin";

                // 计算瓦片在当前层级的像素范围
                int x0 = tx * T;
                int y0 = ty * T;
                int x1 = std::min(x0 + T, levelWidth);
                int y1 = std::min(y0 + T, levelHeight);

                // 目标瓦片（含边界）的期望区域：以 [x0,y0] 为内核左上角，外扩 TILE_BORDER
                const cv::Rect wanted(x0 - TILE_BORDER, y0 - TILE_BORDER, T + 2 * TILE_BORDER, T + 2 * TILE_BORDER);
                const cv::Rect bounds(0, 0, levelWidth, levelHeight);
                const cv::Rect srcRect = wanted & bounds;

                if (srcRect.width <= 0 || srcRect.height <= 0) {
                    // 极端情况：理论上不会发生（除非图像尺寸为0）
                    cv::Mat emptyTile = cv::Mat::zeros(T + 2 * TILE_BORDER, T + 2 * TILE_BORDER, currentLevel.type());
                    saveTileFast_NoMkdir(emptyTile, tileFilePath, TILE_BORDER);
                    continue;
                }

                // 先取交集区域，再用 copyMakeBorder 补齐到 wanted 的大小（用边界复制，避免黑边）
                cv::Mat src = currentLevel(srcRect);
                const int top = srcRect.y - wanted.y;
                const int left = srcRect.x - wanted.x;
                const int bottom = (wanted.y + wanted.height) - (srcRect.y + srcRect.height);
                const int right = (wanted.x + wanted.width) - (srcRect.x + srcRect.width);

                cv::Mat tileWithBorder;
                cv::copyMakeBorder(src, tileWithBorder, top, bottom, left, right, cv::BORDER_REPLICATE);

                // 确保尺寸正确（防御性）
                if (tileWithBorder.cols != (T + 2 * TILE_BORDER) || tileWithBorder.rows != (T + 2 * TILE_BORDER)) {
                    cv::resize(tileWithBorder, tileWithBorder, cv::Size(T + 2 * TILE_BORDER, T + 2 * TILE_BORDER), 0, 0, cv::INTER_NEAREST);
                }

                // 保存瓦片（目录已预创建，使用 NoMkdir 写入）
                saveTileFast_NoMkdir(tileWithBorder, tileFilePath, TILE_BORDER);
            }
        }
        Logger::Log("Tile pyramid | level " + std::to_string(z) + " done, " + std::to_string(tileCount) + " tiles written", LogLevel::INFO, DeviceType::CAMERA);
        writtenMaxZ = z;
    }

    // 后台补齐剩余层级（避免同步阶段长时间占用 CPU/IO，影响 WS/状态机）
    if (writtenMaxZ < gpm.maxZoomLevel && tilePyramidEpoch.load() == epochAtStart) {
        const int bgStartZ = std::max(0, writtenMaxZ + 1);
        const int bgEndZ = gpm.maxZoomLevel;
        const int tileSizeCopy = tilePyramidTileSize;
        const QString sessionTilePathCopy = sessionTilePath;
        const QString sessionIdCopy = sessionId;
        const quint64 epochCopy = epochAtStart;
        const cv::Mat imageShare = image16; // 共享底层数据（ref-count），避免 clone 大图

        QPointer<MainWindow> self(this);
        QtConcurrent::run([self, epochCopy, imageShare, sessionTilePathCopy, sessionIdCopy, tileSizeCopy, bgStartZ, bgEndZ, gpm, cfa, maxMergeFactor]() mutable {
            if (!self) return;
            if (self->tilePyramidEpoch.load() != epochCopy) return;

            // 重新构建金字塔层（避免捕获 pyramidLevels 导致大内存常驻）
            std::vector<cv::Mat> levels;
            levels.resize(gpm.maxZoomLevel + 1);
            // 共享底层 buffer（cv::Mat 引用计数）；只读场景线程安全
            levels[gpm.maxZoomLevel] = imageShare;
            for (int z = gpm.maxZoomLevel - 1; z >= 0; --z) {
                const cv::Mat& higher = levels[z + 1];
                cv::Mat lower = downsampleTileImageForLevel(higher, cfa, 2);
                levels[z] = std::move(lower);
                if (self->tilePyramidEpoch.load() != epochCopy) return;
            }

            const int Tbg = tileSizeCopy;
            constexpr int TILE_BORDER = 2;
            for (int z = bgStartZ; z <= bgEndZ; ++z) {
                if (self->tilePyramidEpoch.load() != epochCopy) return;
                const cv::Mat& currentLevel = levels[z];
                int levelWidth = currentLevel.cols;
                int levelHeight = currentLevel.rows;
                int tilesX = (levelWidth + Tbg - 1) / Tbg;
                int tilesY = (levelHeight + Tbg - 1) / Tbg;

                const QString zDirPath = sessionTilePathCopy + "/" + QString::number(z);
                QDir().mkpath(zDirPath);
                for (int tx = 0; tx < tilesX; ++tx) {
                    if (self->tilePyramidEpoch.load() != epochCopy) return;
                    const QString xDirPath = zDirPath + "/" + QString::number(tx);
                    QDir().mkpath(xDirPath);
                    for (int ty = 0; ty < tilesY; ++ty) {
                        QString tileFilePath = xDirPath + "/" + QString::number(ty) + ".bin";
                        int x0 = tx * Tbg;
                        int y0 = ty * Tbg;
                        const cv::Rect wanted(x0 - TILE_BORDER, y0 - TILE_BORDER, Tbg + 2 * TILE_BORDER, Tbg + 2 * TILE_BORDER);
                        const cv::Rect bounds(0, 0, levelWidth, levelHeight);
                        const cv::Rect srcRect = wanted & bounds;

                        cv::Mat tileWithBorder;
                        if (srcRect.width <= 0 || srcRect.height <= 0) {
                            tileWithBorder = cv::Mat::zeros(Tbg + 2 * TILE_BORDER, Tbg + 2 * TILE_BORDER, currentLevel.type());
                        } else {
                            cv::Mat src = currentLevel(srcRect);
                            const int top = srcRect.y - wanted.y;
                            const int left = srcRect.x - wanted.x;
                            const int bottom = (wanted.y + wanted.height) - (srcRect.y + srcRect.height);
                            const int right = (wanted.x + wanted.width) - (srcRect.x + srcRect.width);
                            cv::copyMakeBorder(src, tileWithBorder, top, bottom, left, right, cv::BORDER_REPLICATE);
                            if (tileWithBorder.cols != (Tbg + 2 * TILE_BORDER) || tileWithBorder.rows != (Tbg + 2 * TILE_BORDER)) {
                                cv::resize(tileWithBorder, tileWithBorder, cv::Size(Tbg + 2 * TILE_BORDER, Tbg + 2 * TILE_BORDER), 0, 0, cv::INTER_NEAREST);
                            }
                        }
                        self->saveTileFast_NoMkdir(tileWithBorder, tileFilePath, TILE_BORDER);
                    }
                }
            }
        });

        Logger::Log("Tile pyramid | sync wrote z<= " + std::to_string(writtenMaxZ) +
                        ", background will write z=" + std::to_string(bgStartZ) + ".." + std::to_string(bgEndZ),
                    LogLevel::INFO, DeviceType::CAMERA);
    }

    // 4. 保存GPM元数据文件
    QString gpmFilePath = sessionTilePath + "/gpm.json";
    QJsonObject gpmJson;
    gpmJson["imageWidth"] = gpm.imageWidth;
    gpmJson["imageHeight"] = gpm.imageHeight;
    gpmJson["tileSize"] = gpm.tileSize;
    gpmJson["maxZoomLevel"] = gpm.maxZoomLevel;
    gpmJson["globalMin"] = gpm.globalMin;
    gpmJson["globalMax"] = gpm.globalMax;
    gpmJson["globalMean"] = gpm.globalMean;
    gpmJson["globalStdDev"] = gpm.globalStdDev;
    gpmJson["blackLevel"] = gpm.blackLevel;
    gpmJson["whiteLevel"] = gpm.whiteLevel;
    gpmJson["cfa"] = gpm.cfa;
    gpmJson["gainR"] = gpm.gainR;
    gpmJson["gainB"] = gpm.gainB;
    gpmJson["sessionId"] = gpm.sessionId;
    // frameId 可能超过 JSON number 的安全整数范围（JS 2^53），这里按字符串写入更安全
    gpmJson["frameId"] = QString::number(static_cast<qulonglong>(gpm.frameId));
    gpmJson["buildMode"] = gpm.buildMode;
    gpmJson["levelMode"] = gpm.levelMode;

        // 直方图（可选）
        // 注意：完整 65536 bins 写入 JSON 会非常大；这里只写入基础信息，详细直方图走 WebSocket B64 通道
        if (gpm.histogramBins > 0 && !gpm.histogram.empty()) {
            gpmJson["histogramBins"] = gpm.histogramBins;
            gpmJson["histogramTotal"] = QString::number(static_cast<qulonglong>(gpm.histogramTotal));
        }

    QJsonDocument gpmDoc(gpmJson);
    // 原子写入 gpm.json：避免读取到半写 JSON（高帧率覆盖写时尤其明显）
    {
        QSaveFile gpmFile(gpmFilePath);
        gpmFile.setDirectWriteFallback(true);
        if (!gpmFile.open(QIODevice::WriteOnly)) {
            Logger::Log("Failed to open gpm.json for writing: " + gpmFilePath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
        } else {
            const QByteArray json = gpmDoc.toJson();
            if (gpmFile.write(json) != json.size()) {
                Logger::Log("Failed to write gpm.json bytes: " + gpmFilePath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
                gpmFile.cancelWriting();
            } else if (!gpmFile.commit()) {
                Logger::Log("Failed to commit gpm.json (atomic replace): " + gpmFilePath.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
            } else {
                Logger::Log("GPM saved to: " + gpmFilePath.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
            }
        }
    }
    Logger::Log("Tile pyramid | step write gpm.json done", LogLevel::INFO, DeviceType::CAMERA);

    Logger::Log("Tile pyramid generation completed for session: " + sessionId.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    return gpm;
}

void MainWindow::emitCaptureTrace(const QString& stage, qint64 startedAtMs, const QString& detail)
{
    if (currentCaptureTraceId.trimmed().isEmpty() || wsThread == nullptr) {
        return;
    }
    const qint64 baseMs = (startedAtMs >= 0) ? startedAtMs : currentCaptureTraceStartedAtMs;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsedMs = (baseMs > 0) ? std::max<qint64>(0, nowMs - baseMs) : 0;
    QString safeDetail = detail;
    safeDetail.replace('\n', ' ');
    safeDetail.replace('\r', ' ');
    emit wsThread->sendMessageToClient(
        QString("CaptureTrace:%1:%2:%3:%4")
            .arg(currentCaptureTraceId)
            .arg(stage)
            .arg(elapsedMs)
            .arg(safeDetail)
    );
    Logger::Log(
        QString("CaptureTrace | traceId=%1 | stage=%2 | backendElapsedMs=%3 | detail=%4")
            .arg(currentCaptureTraceId)
            .arg(stage)
            .arg(elapsedMs)
            .arg(safeDetail)
            .toStdString(),
        LogLevel::INFO,
        DeviceType::CAMERA
    );
}

void MainWindow::sendGPMToClient(const TileGPM& gpm)
{
    // 构建GPM消息发送给前端
    // 格式(兼容扩展):
    // - v1: TileGPM:{sessionId}:{imageWidth}:{imageHeight}:{tileSize}:{maxZoomLevel}:{blackLevel}:{whiteLevel}:{cfa}:{gainR}:{gainB}
    // - v2(追加): ...:{previewWidth}:{previewHeight}:{previewBinningFactor}
    // - v3(追加): ...:{frameId}
    // - v4(追加): ...:{buildMode}
    // - v5(追加): ...:{levelMode}
    // 说明：追加字段放在末尾，旧前端按前 11 段解析不会受影响。
    QString gpmMessage = QString("TileGPM:%1:%2:%3:%4:%5:%6:%7:%8:%9:%10:%11:%12:%13:%14:%15:%16")
        .arg(gpm.sessionId)
        .arg(gpm.imageWidth)
        .arg(gpm.imageHeight)
        .arg(gpm.tileSize)
        .arg(gpm.maxZoomLevel)
        .arg(gpm.blackLevel)
        .arg(gpm.whiteLevel)
        .arg(gpm.cfa)
        .arg(gpm.gainR)
        .arg(gpm.gainB)
        .arg(gpm.previewWidth)
        .arg(gpm.previewHeight)
        .arg(gpm.previewBinningFactor)
        .arg(QString::number(static_cast<qulonglong>(gpm.frameId)))
        .arg(gpm.buildMode)
        .arg(gpm.levelMode);

    emit wsThread->sendMessageToClient(gpmMessage);
    Logger::Log("GPM sent to client: " + gpmMessage.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | sendGPMToClient | sessionId = " +
                    gpm.sessionId.toStdString(),
                LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | sendGPMToClient | frameId = " +
                    std::to_string(static_cast<unsigned long long>(gpm.frameId)),
                LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | sendGPMToClient | blackWhite = " +
                    std::to_string(gpm.blackLevel) + "," + std::to_string(gpm.whiteLevel),
                LogLevel::INFO, DeviceType::CAMERA);
    Logger::Log("MainCameraImagePipeLine | mainwindow.cpp | sendGPMToClient | gainRB = " +
                    std::to_string(gpm.gainR) + "," + std::to_string(gpm.gainB),
                LogLevel::INFO, DeviceType::CAMERA);
}

void MainWindow::sendTileBatchReadyToClient(const QString& sessionId, quint64 frameId, const QStringList& tileKeys)
{
    if (sessionId.isEmpty() || tileKeys.isEmpty()) {
        return;
    }

    if (QThread::currentThread() != thread()) {
        QPointer<MainWindow> self(this);
        QMetaObject::invokeMethod(this, [self, sessionId, frameId, tileKeys]() {
            if (!self) return;
            self->sendTileBatchReadyToClient(sessionId, frameId, tileKeys);
        }, Qt::QueuedConnection);
        return;
    }

    QJsonObject payload;
    payload["sessionId"] = sessionId;
    payload["frameId"] = QString::number(static_cast<qulonglong>(frameId));
    QJsonArray tilesJson;
    for (const QString& key : tileKeys) {
        tilesJson.append(key);
    }
    payload["tiles"] = tilesJson;

    const QString message = QStringLiteral("TileBatchReady:")
        + QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    emit wsThread->sendMessageToClient(message);
    QStringList sampleKeys;
    const int sampleCount = std::min(8, tileKeys.size());
    for (int i = 0; i < sampleCount; ++i) {
        sampleKeys.push_back(tileKeys.at(i));
    }
    Logger::Log("TileBatchReady sent to client: session=" + sessionId.toStdString() +
                    ", frameId=" + std::to_string(static_cast<unsigned long long>(frameId)) +
                    ", count=" + std::to_string(tileKeys.size()) +
                    ", sample=[" + sampleKeys.join(", ").toStdString() + "]",
                LogLevel::DEBUG, DeviceType::CAMERA);
    const bool containsZ0 = tileKeys.contains(QStringLiteral("0/0/0"));
    emitCaptureTrace(QStringLiteral("backend_tilebatchready_sent"), currentCaptureTraceStartedAtMs,
                     QString("sessionId=%1,frameId=%2,count=%3,containsZ0=%4")
                         .arg(sessionId)
                         .arg(QString::number(static_cast<qulonglong>(frameId)))
                         .arg(tileKeys.size())
                         .arg(containsZ0 ? QStringLiteral("true") : QStringLiteral("false")));
}

void MainWindow::sendCurrentTileBatchReadySnapshotToClient(const QString& sessionId, quint64 frameId, const QStringList& requestedTileKeys)
{
    if (sessionId.isEmpty() || frameId == 0) {
        return;
    }

    TileFrameState st;
    {
        std::lock_guard<std::mutex> lk(tileFrameMutex);
        st = tileFrame;
    }
    if (st.sessionId != sessionId || st.epoch != frameId) {
        Logger::Log("queryTileBatchReady ignored: current session/frame mismatch, currentSession=" +
                        st.sessionId.toStdString() + ", currentFrame=" +
                        std::to_string(static_cast<unsigned long long>(st.epoch)),
                    LogLevel::DEBUG, DeviceType::CAMERA);
        return;
    }

    std::vector<uint64_t> readyKeys;
    {
        std::lock_guard<std::mutex> lk(tileGenDoneMutex);
        if (tileGenDoneEpoch != frameId || tileGenDoneKeys.empty()) {
            return;
        }
        readyKeys.assign(tileGenDoneKeys.begin(), tileGenDoneKeys.end());
    }

    std::set<uint64_t> requestedPackedKeys;
    if (!requestedTileKeys.isEmpty()) {
        for (const QString& key : requestedTileKeys) {
            const QStringList keyParts = key.split('/');
            if (keyParts.size() != 3) continue;
            bool okZ = false;
            bool okX = false;
            bool okY = false;
            const int z = keyParts[0].toInt(&okZ);
            const int x = keyParts[1].toInt(&okX);
            const int y = keyParts[2].toInt(&okY);
            if (!okZ || !okX || !okY || z < 0 || x < 0 || y < 0) continue;
            const uint64_t packedKey =
                (static_cast<uint64_t>(z) << 40) |
                (static_cast<uint64_t>(x) << 20) |
                static_cast<uint64_t>(y);
            requestedPackedKeys.insert(packedKey);
        }
        readyKeys.erase(
            std::remove_if(readyKeys.begin(), readyKeys.end(),
                           [&requestedPackedKeys](uint64_t packedKey) {
                               return requestedPackedKeys.find(packedKey) == requestedPackedKeys.end();
                           }),
            readyKeys.end());
        if (readyKeys.empty()) {
            return;
        }
    }

    std::sort(readyKeys.begin(), readyKeys.end());
    QStringList tileKeys;
    tileKeys.reserve(static_cast<int>(readyKeys.size()));
    for (uint64_t packedKey : readyKeys) {
        const int z = static_cast<int>((packedKey >> 40) & ((1ULL << 24) - 1));
        const int x = static_cast<int>((packedKey >> 20) & ((1ULL << 20) - 1));
        const int y = static_cast<int>(packedKey & ((1ULL << 20) - 1));
        tileKeys.push_back(QString::number(z) + "/" + QString::number(x) + "/" + QString::number(y));
    }

    Logger::Log("queryTileBatchReady snapshot: session=" + sessionId.toStdString() +
                    ", frameId=" + std::to_string(static_cast<unsigned long long>(frameId)) +
                    ", count=" + std::to_string(tileKeys.size()) +
                    ", requestedCount=" + std::to_string(requestedTileKeys.size()),
                LogLevel::DEBUG, DeviceType::CAMERA);
    sendTileBatchReadyToClient(sessionId, frameId, tileKeys);
}

void MainWindow::sendTileGenerationCompleteToClient(const QString& sessionId, quint64 frameId)
{
    if (sessionId.isEmpty() || frameId == 0) {
        return;
    }

    if (QThread::currentThread() != thread()) {
        QPointer<MainWindow> self(this);
        QMetaObject::invokeMethod(this, [self, sessionId, frameId]() {
            if (!self) return;
            self->sendTileGenerationCompleteToClient(sessionId, frameId);
        }, Qt::QueuedConnection);
        return;
    }

    TileFrameState st;
    {
        std::lock_guard<std::mutex> lk(tileFrameMutex);
        st = tileFrame;
    }
    if (st.sessionId != sessionId || st.epoch != frameId || tilePyramidEpoch.load() != frameId) {
        return;
    }

    size_t readyCount = 0;
    {
        std::lock_guard<std::mutex> lk(tileGenDoneMutex);
        if (tileGenDoneEpoch != frameId) {
            return;
        }
        if (tileGenCompleteEpoch == frameId) {
            return;
        }
        readyCount = tileGenDoneKeys.size();
        tileGenCompleteEpoch = frameId;
    }

    QJsonObject payload;
    payload["sessionId"] = sessionId;
    payload["frameId"] = QString::number(static_cast<qulonglong>(frameId));
    payload["readyCount"] = static_cast<qint64>(readyCount);
    payload["complete"] = true;

    const QString message = QStringLiteral("TileGenerationComplete:")
        + QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    emit wsThread->sendMessageToClient(message);

    Logger::Log("TileGenerationComplete sent to client: session=" + sessionId.toStdString() +
                    ", frameId=" + std::to_string(static_cast<unsigned long long>(frameId)) +
                    ", readyCount=" + std::to_string(static_cast<unsigned long long>(readyCount)),
                LogLevel::DEBUG, DeviceType::CAMERA);
}

void MainWindow::sendCurrentTileGenerationCompleteSnapshotToClient(const QString& sessionId, quint64 frameId)
{
    if (sessionId.isEmpty() || frameId == 0) {
        return;
    }

    TileFrameState st;
    {
        std::lock_guard<std::mutex> lk(tileFrameMutex);
        st = tileFrame;
    }
    if (st.sessionId != sessionId || st.epoch != frameId) {
        return;
    }

    size_t readyCount = 0;
    {
        std::lock_guard<std::mutex> lk(tileGenDoneMutex);
        if (tileGenCompleteEpoch != frameId) {
            return;
        }
        readyCount = tileGenDoneKeys.size();
    }

    Logger::Log("queryTileGenerationComplete snapshot: session=" + sessionId.toStdString() +
                    ", frameId=" + std::to_string(static_cast<unsigned long long>(frameId)),
                LogLevel::DEBUG, DeviceType::CAMERA);

    QJsonObject payload;
    payload["sessionId"] = sessionId;
    payload["frameId"] = QString::number(static_cast<qulonglong>(frameId));
    payload["readyCount"] = static_cast<qint64>(readyCount);
    payload["complete"] = true;

    const QString message = QStringLiteral("TileGenerationComplete:")
        + QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    emit wsThread->sendMessageToClient(message);
}

void MainWindow::sendHistogramToClient(const TileGPM& gpm)
{
    if (gpm.sessionId.isEmpty() || gpm.histogramBins <= 0 || gpm.histogram.empty()) {
        return;
    }

    // ========================= 新方案：直方图保存到 tmpfs，供 HTTPS 下载 =========================
    // 直方图与瓦片同目录（/dev/shm/capture-tiles/），按 session 独立命名。

    QString histogramFileName = QString("histogram_%1.bin").arg(gpm.sessionId);
    QString histogramFilePath = QString::fromStdString(tilePyramidPath) + histogramFileName;
    
    // 2. 将直方图保存为二进制文件
    QFile binFile(histogramFilePath);
    if (!binFile.open(QIODevice::WriteOnly)) {
        Logger::Log("Failed to create histogram file: " + histogramFilePath.toStdString(),
                    LogLevel::ERROR, DeviceType::CAMERA);
        return;
    }
    
    // 写入文件头和数据（便于前端解析）
    // 文件格式：[bins(4字节)][total(8字节)][histogram数据(bins*4字节)]
    QDataStream out(&binFile);
    out.setByteOrder(QDataStream::LittleEndian);
    
    // 写入bins数量（4字节，uint32）
    out << static_cast<quint32>(gpm.histogramBins);
    
    // 写入总像素数（8字节，uint64）
    out << static_cast<quint64>(gpm.histogramTotal);
    
    // 写入直方图数据（bins * 4字节）
    for (uint32_t count : gpm.histogram) {
        out << static_cast<quint32>(count);
    }
    
    qint64 fileSize = binFile.size();
    binFile.close();
    
    Logger::Log("Histogram saved to file: " + histogramFilePath.toStdString() + 
                ", size: " + std::to_string(fileSize) + " bytes" +
                ", bins: " + std::to_string(gpm.histogramBins),
                LogLevel::INFO, DeviceType::CAMERA);
    
    // 3. 构建下载 URL（nginx 需 alias /img/capture-tiles/ -> /dev/shm/capture-tiles/）
    QString histogramUrl = QString("/img/capture-tiles/%1").arg(histogramFileName);
    
    // 4. 通过WebSocket发送元数据和URL（而不是完整的Base64数据）
    // 格式: TileHistogramFile:{sessionId}:{bins}:{total}:{url}
    QString msg = QString("TileHistogramFile:%1:%2:%3:%4")
        .arg(gpm.sessionId)
        .arg(gpm.histogramBins)
        .arg(QString::number(static_cast<qulonglong>(gpm.histogramTotal)))
        .arg(histogramUrl);
    
    emit wsThread->sendMessageToClient(msg);
    
    Logger::Log("Histogram URL sent to client: session=" + gpm.sessionId.toStdString() +
                ", url=" + histogramUrl.toStdString(),
                LogLevel::INFO, DeviceType::CAMERA);
    
    // 5. 每帧独立 histogram 文件；保留最近几帧，兼顾前端异步下载与 tmpfs 占用。
    cleanupOldHistogramFiles(5);
}

void MainWindow::cleanupOldHistogramFiles(int keepCount)
{
    // 直方图与瓦片同在 tmpfs（tilePyramidPath）
    QDir directory(QString::fromStdString(tilePyramidPath));
    QStringList filters;
    filters << "histogram_*.bin";
    
    // 按修改时间排序，最新的在前
    QFileInfoList fileList = directory.entryInfoList(filters, QDir::Files, QDir::Time);
    
    if (fileList.size() <= keepCount) {
        return; // 文件数量未超过保留数量，无需清理
    }
    
    // 删除超出保留数量的旧文件
    for (int i = keepCount; i < fileList.size(); ++i) {
        QString filePath = fileList[i].absoluteFilePath();
        if (QFile::remove(filePath)) {
            Logger::Log("Removed old histogram file: " + filePath.toStdString(), 
                       LogLevel::INFO, DeviceType::CAMERA);
        } else {
            Logger::Log("Failed to remove old histogram file: " + filePath.toStdString(), 
                       LogLevel::WARNING, DeviceType::CAMERA);
        }
    }
    
    int removedCount = fileList.size() - keepCount;
    if (removedCount > 0) {
        Logger::Log("Cleaned up " + std::to_string(removedCount) + " old histogram file(s), kept " + 
                    std::to_string(keepCount) + " most recent", 
                    LogLevel::INFO, DeviceType::CAMERA);
    }
}

void MainWindow::cleanupOldTileSessionDirs(const QString& keepSessionId)
{
    if (keepSessionId.isEmpty()) return;
    QDir baseDir(QString::fromStdString(tilePyramidPath));
    if (!baseDir.exists()) return;
    const QStringList entries = baseDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    int removed = 0;
    for (const QString& name : entries) {
        if (name == keepSessionId) continue;
        // 只删除会话目录：live 或 live_<数字>
        if (name != QStringLiteral("live") && !name.startsWith(QStringLiteral("live_"))) continue;
        QString absPath = baseDir.absoluteFilePath(name);
        QDir subDir(absPath);
        if (subDir.removeRecursively()) {
            removed++;
            Logger::Log("Removed old tile session dir: " + absPath.toStdString(), LogLevel::INFO, DeviceType::CAMERA);
        } else {
            Logger::Log("Failed to remove old tile session dir: " + absPath.toStdString(), LogLevel::WARNING, DeviceType::CAMERA);
        }
    }
    if (removed > 0) {
        Logger::Log("Cleaned up " + std::to_string(removed) + " old tile session dir(s), kept " + keepSessionId.toStdString(),
                    LogLevel::INFO, DeviceType::CAMERA);
    }
}

// ========================= 瓦片金字塔生成相关函数结束 =========================

cv::Mat MainWindow::colorImage(cv::Mat img16)
{
    Logger::Log("Starting color image processing...", LogLevel::INFO, DeviceType::MAIN);
    QString effectiveCameraCFA = MainCameraCFA;
    // color camera, need to do debayer and color balance
    cv::Mat AWBImg16;
    cv::Mat AWBImg16color;
    cv::Mat AWBImg16mono;
    cv::Mat AWBImg8color;

    uint16_t B = 0;
    uint16_t W = 65535;

    AWBImg16.create(img16.rows, img16.cols, CV_16UC1);
    AWBImg16color.create(img16.rows, img16.cols, CV_16UC3);
    AWBImg16mono.create(img16.rows, img16.cols, CV_16UC1);
    AWBImg8color.create(img16.rows, img16.cols, CV_8UC3);

    const uint16_t offset = static_cast<uint16_t>(std::clamp(std::lround(ImageOffset), 0l, 65535l));
    Logger::Log("Matrices for image processing created.", LogLevel::INFO, DeviceType::MAIN);
    Tools::ImageSoftAWB(img16, AWBImg16, effectiveCameraCFA, ImageGainR, ImageGainB, offset); // image software Auto White Balance is done in RAW image.
    Logger::Log("Auto White Balance applied.", LogLevel::INFO, DeviceType::MAIN);
    const int demosaicCode = getOpenCvBayerToBgrCode(effectiveCameraCFA);
    if (demosaicCode < 0) {
        Logger::Log("colorImage | invalid CFA for Bayer->BGR conversion: " + effectiveCameraCFA.toStdString(),
                    LogLevel::WARNING, DeviceType::MAIN);
        return cv::Mat();
    }
    cv::cvtColor(AWBImg16, AWBImg16color, demosaicCode);
    Logger::Log("Image converted from Bayer to BGR.", LogLevel::INFO, DeviceType::MAIN);

    cv::cvtColor(AWBImg16color, AWBImg16mono, cv::COLOR_BGR2GRAY);
    Logger::Log("Image converted to grayscale.", LogLevel::INFO, DeviceType::MAIN);

    if (AutoStretch == true)
    {
        Tools::GetAutoStretch(AWBImg16mono, 0, B, W);
        Logger::Log("Auto stretch applied.", LogLevel::INFO, DeviceType::MAIN);
    }
    else
    {
        B = 0;
        W = 65535;
        Logger::Log("Auto stretch not applied, using default values.", LogLevel::INFO, DeviceType::MAIN);
    }
    Logger::Log("AutoStretch values: B=" + std::to_string(B) + ", W=" + std::to_string(W), LogLevel::INFO, DeviceType::MAIN);
    Tools::Bit16To8_Stretch(AWBImg16color, AWBImg8color, B, W);
    Logger::Log("Image stretched from 16-bit to 8-bit.", LogLevel::INFO, DeviceType::MAIN);

    AWBImg16.release();
    AWBImg16color.release();
    AWBImg16mono.release();
    AWBImg8color.release();
    Logger::Log("Temporary matrices released.", LogLevel::INFO, DeviceType::MAIN);

    return AWBImg16color;
}
void MainWindow::saveGuiderImageAsJPG(cv::Mat Image)
{
    // 循环曝光会高频触发：降为 DEBUG（默认关闭 DEBUG）避免刷屏
    Logger::Log("Starting to save guider image as JPG...", LogLevel::DEBUG, DeviceType::GUIDER);
    QElapsedTimer perfTimer;
    perfTimer.start();
    qint64 perfLastMs = 0;
    auto logPerfStage = [&](const std::string& stage) {
        const qint64 nowMs = perfTimer.elapsed();
        Logger::Log("GuiderPerf | saveGuiderImageAsJPG | stage=" + stage +
                        " deltaMs=" + std::to_string(nowMs - perfLastMs) +
                        " totalMs=" + std::to_string(nowMs),
                    LogLevel::INFO, DeviceType::GUIDER);
        perfLastMs = nowMs;
    };

    constexpr int kKeepRecentGuiderImages = 12;
    constexpr int kGuiderPreviewMaxWidth = 1920;

    cv::Mat preview = Image;
    int downsampleLevel = 0;
    while (!preview.empty() && preview.cols > kGuiderPreviewMaxWidth)
    {
        cv::Mat downsampled;
        cv::resize(preview, downsampled, cv::Size(), 0.5, 0.5, cv::INTER_AREA);
        preview = downsampled;
        ++downsampleLevel;
    }
    logPerfStage("preview_downsample");

    if (downsampleLevel > 0)
    {
        Logger::Log("saveGuiderImageAsJPG | preview downsampled for transport: " +
                        std::to_string(Image.cols) + "x" + std::to_string(Image.rows) + " -> " +
                        std::to_string(preview.cols) + "x" + std::to_string(preview.rows) +
                        " (2x2 level=" + std::to_string(downsampleLevel) + ")",
                    LogLevel::INFO, DeviceType::GUIDER);
    }

    // 生成唯一ID
    QString uniqueId = QUuid::createUuid().toString();
    Logger::Log("Generated unique ID for new guider image: " + uniqueId.toStdString(), LogLevel::DEBUG, DeviceType::GUIDER);
    logPerfStage("generate_unique_id");

    // 保存新的图像带有唯一ID的文件名
    std::string fileName = "GuiderImage_" + uniqueId.toStdString() + ".jpg";
    std::string filePath = vueDirectoryPath + fileName;
    bool saved = cv::imwrite(filePath, preview);
    Logger::Log("Attempted to save new guider image.", LogLevel::DEBUG, DeviceType::GUIDER);
    logPerfStage("write_preview_jpg");

    std::string Command = "sudo ln -sf " + filePath + " " + vueImagePath + fileName;
    system(Command.c_str());
    Logger::Log("Created symbolic link for new guider image.", LogLevel::DEBUG, DeviceType::GUIDER);
    logPerfStage("create_preview_symlink");

    PriorGuiderImage = vueImagePath + fileName;

    auto cleanupOldGuiderImages = [&](const QString& dirPath, bool includeSymlinks, const QString& protectedFileName) {
        try {
            const fs::path dirFsPath = dirPath.toStdString();
            if (!fs::exists(dirFsPath))
                return;

            auto hasPrefix = [](const std::string& s, const std::string& p) -> bool {
                return s.rfind(p, 0) == 0;
            };
            auto hasSuffix = [](const std::string& s, const std::string& suf) -> bool {
                return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
            };

            struct EntryInfo {
                fs::path path;
                bool timeOk = false;
                fs::file_time_type t;
            };

            std::vector<EntryInfo> items;
            items.reserve(256);

            const std::string protectedName = protectedFileName.toStdString();
            bool hasProtectedEntry = false;

            for (const auto& entry : fs::directory_iterator(dirFsPath)) {
                const std::string name = entry.path().filename().string();
                if (!hasPrefix(name, "GuiderImage_") || !hasSuffix(name, ".jpg"))
                    continue;

                const bool isLink = fs::is_symlink(entry.symlink_status());
                const bool isFile = fs::is_regular_file(entry.status());
                if (includeSymlinks) {
                    if (!isLink)
                        continue;
                } else {
                    if (!isFile)
                        continue;
                }

                if (name == protectedName) {
                    hasProtectedEntry = true;
                    continue;
                }

                EntryInfo info;
                info.path = entry.path();
                try {
                    info.t = fs::last_write_time(entry.path());
                    info.timeOk = true;
                } catch (...) {
                    info.timeOk = false;
                }
                items.push_back(std::move(info));
            }

            std::sort(items.begin(), items.end(), [](const EntryInfo& a, const EntryInfo& b) {
                if (a.timeOk != b.timeOk)
                    return a.timeOk;
                if (!a.timeOk)
                    return a.path.string() < b.path.string();
                return a.t > b.t;
            });

            const int keepOthers = std::max(0, kKeepRecentGuiderImages - (hasProtectedEntry ? 1 : 0));

            Logger::Log("Listed existing guider images for cleanup in " + dirPath.toStdString() +
                            ", count=" + std::to_string(items.size() + (hasProtectedEntry ? 1 : 0)) +
                            ", protected=" + protectedName,
                        LogLevel::DEBUG, DeviceType::GUIDER);

            int kept = 0;
            for (const auto& item : items)
            {
                const bool shouldKeep = item.timeOk && kept < keepOthers;
                if (shouldKeep)
                {
                    kept++;
                    continue;
                }

                std::error_code ec;
                fs::remove(item.path, ec);
                if (!ec)
                {
                    Logger::Log("Deleted old guider image file: " + item.path.string(),
                                LogLevel::DEBUG, DeviceType::GUIDER);
                }
            }
        } catch (...) {
            Logger::Log("cleanupOldGuiderImages | cleanup failed in " + dirPath.toStdString(),
                        LogLevel::WARNING, DeviceType::GUIDER);
        }
    };

    cleanupOldGuiderImages(QString::fromStdString(vueDirectoryPath), false, QString::fromStdString(fileName));
    logPerfStage("cleanup_preview_dir");
    cleanupOldGuiderImages(QString::fromStdString(vueImagePath), true, QString::fromStdString(fileName));
    logPerfStage("cleanup_preview_symlink_dir");

    if (saved)
    {
        emit wsThread->sendMessageToClient(QString("GuideSize:%1:%2").arg(preview.cols).arg(preview.rows));
        emit wsThread->sendMessageToClient("SaveGuiderImageSuccess:" + QString::fromStdString(fileName));
        Logger::Log("Guider image saved successfully and client notified.", LogLevel::DEBUG, DeviceType::GUIDER);
        logPerfStage("notify_client");
    }
    else
    {
        Logger::Log("Failed to save guider image.", LogLevel::ERROR, DeviceType::GUIDER);
        logPerfStage("notify_client_failed");
    }
}

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

void MainWindow::FocusingLooping()
{
    Logger::Log("FocusingLooping start ...", LogLevel::DEBUG, DeviceType::FOCUSER);
    
    // 判断主相机是 SDK 模式还是 INDI 模式
    bool isMainCameraSDK = (systemdevicelist.system_devices.size() > 20 &&
                            systemdevicelist.system_devices[20].isSDKConnect &&
                            sdkMainCameraHandle != nullptr);
    
    // 检查相机是否连接，如果未连接则记录警告并返回
    if (!isMainCameraSDK && dpMainCamera == NULL)
    {
        Logger::Log("FocusingLooping | Main Camera not available (both SDK and INDI are NULL)", LogLevel::WARNING, DeviceType::FOCUSER);
        return;
    }

    isSavePngSuccess = false;

    glIsFocusingLooping = true;
    Logger::Log("FocusingLooping | glIsFocusingLooping:" + std::to_string(glIsFocusingLooping), LogLevel::DEBUG, DeviceType::FOCUSER);
    
    // 如果相机状态为"显示中"，则开始处理曝光
    if (glMainCameraStatu != "Exposuring")
    {
        double expTime_sec = (double)glExpTime / 1000; // 将曝光时间从毫秒转换为秒

        glMainCameraStatu = "Exposuring";
        Logger::Log("FocusingLooping | glMainCameraStatu:" + glMainCameraStatu.toStdString(), LogLevel::DEBUG, DeviceType::FOCUSER);

        // 防御：SDK 模式下必须先拿到主相机尺寸，否则后续会把 ROI 夹成 0x0
        if (isMainCameraSDK && (glMainCCDSizeX <= 0 || glMainCCDSizeY <= 0))
        {
            SdkCommand chipInfoCmd;
            chipInfoCmd.type = SdkCommandType::Custom;
            chipInfoCmd.name = "GetChipInfo";
            chipInfoCmd.payload = std::any();
            // 直接通过设备句柄调用，无需指定驱动名称
            SdkResult chipInfoRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, chipInfoCmd);
            if (chipInfoRes.success)
            {
                try {
                    SdkChipInfo chipInfo = std::any_cast<SdkChipInfo>(chipInfoRes.payload);
                    glMainCCDSizeX = chipInfo.maxImageSizeX;
                    glMainCCDSizeY = chipInfo.maxImageSizeY;
                    emit wsThread->sendMessageToClient("MainCameraSize:" + QString::number(glMainCCDSizeX) + ":" + QString::number(glMainCCDSizeY));
                    Logger::Log("FocusingLooping | SDK ChipInfo loaded on-demand: " +
                                std::to_string(glMainCCDSizeX) + "x" + std::to_string(glMainCCDSizeY),
                                LogLevel::DEBUG, DeviceType::FOCUSER);
                } catch (const std::bad_any_cast&) {
                    // ignore, will fail below
                }
            }
        }
        if (isMainCameraSDK && (glMainCCDSizeX <= 0 || glMainCCDSizeY <= 0))
        {
            Logger::Log("FocusingLooping | SDK main camera size not initialized (glMainCCDSizeX/Y=0), abort focusing loop.",
                        LogLevel::ERROR, DeviceType::FOCUSER);
            glMainCameraStatu = "IDLE";
            glIsFocusingLooping = false;
            isFocusLoopShooting = false;
            emit wsThread->sendMessageToClient("startFocusLoopFailed:SDK MainCamera size not initialized");
            return;
        }

        QSize cameraResolution{glMainCCDSizeX, glMainCCDSizeY};
        // SDK：GetEffectiveArea 得到有效区在芯片上的偏移与尺寸；ROI_x/y 与前端一致为「仅有效区内」相对坐标（原点在有效区左上角）
        int effMinX = 0;
        int effMinY = 0;
        int effW = glMainCCDSizeX;
        int effH = glMainCCDSizeY;
        if (isMainCameraSDK && sdkMainCameraHandle != nullptr) {
            if (sdkMainEffectiveAreaCacheHandle != sdkMainCameraHandle) {
                sdkMainEffectiveAreaCacheValid = false;
                sdkMainEffectiveAreaCacheHandle = sdkMainCameraHandle;
            }

            if (!sdkMainEffectiveAreaCacheValid) {
                SdkCommand effCmd;
                effCmd.type = SdkCommandType::Custom;
                effCmd.name = "GetEffectiveArea";
                effCmd.payload = std::any();
                SdkResult effRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, effCmd);
                if (effRes.success) {
                    try {
                        SdkAreaInfo eff = std::any_cast<SdkAreaInfo>(effRes.payload);
                        if (eff.sizeX > 0U && eff.sizeY > 0U) {
                            sdkMainEffectiveAreaMinX = static_cast<int>(eff.startX);
                            sdkMainEffectiveAreaMinY = static_cast<int>(eff.startY);
                            sdkMainEffectiveAreaWidth = static_cast<int>(eff.sizeX);
                            sdkMainEffectiveAreaHeight = static_cast<int>(eff.sizeY);
                            sdkMainEffectiveAreaCacheValid = true;
                            Logger::Log("FocusingLooping | cached GetEffectiveArea: start=(" +
                                            std::to_string(sdkMainEffectiveAreaMinX) + "," +
                                            std::to_string(sdkMainEffectiveAreaMinY) + ") size=(" +
                                            std::to_string(sdkMainEffectiveAreaWidth) + "x" +
                                            std::to_string(sdkMainEffectiveAreaHeight) + ")",
                                        LogLevel::DEBUG, DeviceType::FOCUSER);
                        }
                    } catch (const std::bad_any_cast&) {
                    }
                }
            }

            if (sdkMainEffectiveAreaCacheValid) {
                effMinX = sdkMainEffectiveAreaMinX;
                effMinY = sdkMainEffectiveAreaMinY;
                effW = sdkMainEffectiveAreaWidth;
                effH = sdkMainEffectiveAreaHeight;
                Logger::Log("FocusingLooping | effective area baseline: start=(" + std::to_string(effMinX) + "," +
                                std::to_string(effMinY) + ") size=(" + std::to_string(effW) + "x" +
                                std::to_string(effH) + ")",
                            LogLevel::DEBUG, DeviceType::FOCUSER);
            }
        }
        // 使用局部变量，避免把全局 BoxSideLength 夹成 0（会导致后续一直 0x0 ROI）
        int roiBox = BoxSideLength;
        if (roiBox <= 0) roiBox = 300;
        {
            const int capW = std::min(glMainCCDSizeX, effW);
            const int capH = std::min(glMainCCDSizeY, effH);
            if (roiBox > capW) roiBox = capW;
            if (roiBox > capH) roiBox = capH;
        }
        if (roiBox < 2) roiBox = 2; // 最小 2，便于后续偶数对齐
        QSize ROI{roiBox, roiBox};

        Logger::Log("FocusingLooping |当前ROI值 ROI_x:" + std::to_string(roiAndFocuserInfo["ROI_x"]) + ", ROI_y:" + std::to_string(roiAndFocuserInfo["ROI_y"]), LogLevel::DEBUG, DeviceType::FOCUSER);
        int cameraX = static_cast<int>(roiAndFocuserInfo["ROI_x"]);
        int cameraY = static_cast<int>(roiAndFocuserInfo["ROI_y"]);

        // 确保 cameraX 和 cameraY 是偶数
        if (cameraX % 2 != 0) cameraX += 1;
        if (cameraY % 2 != 0) cameraY += 1;

        // 坐标体系：ROI_x/y 为「有效成像区」内坐标（相对有效区原点），乘 binning 得 scaledX/Y（仍在相对空间，范围 [0, effW-w]）
        const bool tileModeActive = (isStagingImage && !SavedImage.empty());
        const int roiCoordScale = tileModeActive ? 1 : std::max(1, glMainCameraBinning);

        int scaledX = cameraX * roiCoordScale;
        int scaledY = cameraY * roiCoordScale;
        if (scaledX < 0) scaledX = 0;
        if (scaledY < 0) scaledY = 0;
        ROI = QSize(roiBox, roiBox);
        if (scaledX > effW - ROI.width()) scaledX = effW - ROI.width();
        if (scaledY > effH - ROI.height()) scaledY = effH - ROI.height();

        if (scaledX <= effW - ROI.width() && scaledY <= effH - ROI.height())
        {
            Logger::Log("FocusingLooping | set Camera ROI x:" + std::to_string(cameraX) + ", y:" + std::to_string(cameraY) + ", width:" + std::to_string(roiBox) + ", height:" + std::to_string(roiBox), LogLevel::DEBUG, DeviceType::FOCUSER);
            // 将裁剪后的坐标反馈给前端，统一回到“当前预览坐标系”。
            if (roiCoordScale > 0) {
                roiAndFocuserInfo["ROI_x"] = static_cast<double>(scaledX) / roiCoordScale;
                roiAndFocuserInfo["ROI_y"] = static_cast<double>(scaledY) / roiCoordScale;
            }
            
            if (isMainCameraSDK)
            {
                // === SDK 模式：完整的 ROI 曝光流程 ===
                // 0. 先尝试取消上一帧可能残留的曝光/读出（避免某些机型在连续 ROI 切换时卡死）
                {
                    SdkCommand cancelCmd;
                    cancelCmd.type = SdkCommandType::Custom;
                    cancelCmd.name = "CancelExposure";
                    cancelCmd.payload = std::any();
                    // 直接通过设备句柄调用，无需指定驱动名称
                    SdkManager::instance().callByHandle(sdkMainCameraHandle, cancelCmd);
                }

                // 1. 设置 ROI（部分机型要求 ROI 起点/宽高为偶数；芯片坐标 = 有效区偏移 + 相对坐标）
                int roiW = roiBox;
                int roiH = roiBox;
                if (roiW % 2 != 0) roiW += 1;
                if (roiH % 2 != 0) roiH += 1;
                if (roiW > effW) roiW = effW;
                if (roiH > effH) roiH = effH;
                if ((effMinX + scaledX) % 2 != 0) scaledX = std::max(0, scaledX - 1);
                if ((effMinY + scaledY) % 2 != 0) scaledY = std::max(0, scaledY - 1);
                if (scaledX > effW - roiW) scaledX = effW - roiW;
                if (scaledY > effH - roiH) scaledY = effH - roiH;
                if (scaledX < 0) scaledX = 0;
                if (scaledY < 0) scaledY = 0;

                const int sensorStartX = effMinX + scaledX;
                const int sensorStartY = effMinY + scaledY;

                lastFocusExposureSnapshotValid = true;
                lastFocusExposureScaledX = scaledX;
                lastFocusExposureScaledY = scaledY;
                lastFocusExposureEffMinX = effMinX;
                lastFocusExposureEffMinY = effMinY;
                lastFocusExposureRoiCoordScale = std::max(1, roiCoordScale);
                lastFocusExposureRoiW = roiW;
                lastFocusExposureRoiH = roiH;

                const QString focusResolvedCfa = resolveFrameCfa(sensorStartX, sensorStartY);
                Logger::Log("FocusingLooping | ROI Bayer debug | previewROI=(" + std::to_string(cameraX) + "," + std::to_string(cameraY) +
                                ") scaled=(" + std::to_string(scaledX) + "," + std::to_string(scaledY) + ")" +
                                ", roiCoordScale=" + std::to_string(roiCoordScale) +
                                ", effRect=(" + std::to_string(effMinX) + "," + std::to_string(effMinY) + "," +
                                std::to_string(effW) + "x" + std::to_string(effH) + ")" +
                                ", roiSize=" + std::to_string(roiW) + "x" + std::to_string(roiH) +
                                ", sensorParity=(" + std::to_string(sensorStartX & 1) + "," + std::to_string(sensorStartY & 1) + ")" +
                                ", " + formatBayerPhaseDebug(MainCameraCFA, MainCameraCFAOffsetX, MainCameraCFAOffsetY,
                                                             sensorStartX, sensorStartY, focusResolvedCfa),
                            LogLevel::INFO, DeviceType::FOCUSER);

                SdkAreaInfo roi;
                roi.startX = static_cast<unsigned int>(sensorStartX);
                roi.startY = static_cast<unsigned int>(sensorStartY);
                roi.sizeX = static_cast<unsigned int>(roiW);
                roi.sizeY = static_cast<unsigned int>(roiH);
                
                SdkCommand setRoiCmd;
                setRoiCmd.type = SdkCommandType::Custom;
                setRoiCmd.name = "SetResolution";
                setRoiCmd.payload = roi;
                Logger::Log("FocusingLooping | SDK SetResolution | start=(" + std::to_string(roi.startX) + "," + std::to_string(roi.startY) + ") size=(" + std::to_string(roi.sizeX) + "x" + std::to_string(roi.sizeY) + ")",
                            LogLevel::DEBUG, DeviceType::FOCUSER);
                // 直接通过设备句柄调用，无需指定驱动名称
                SdkResult roiRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, setRoiCmd);
                if (!roiRes.success) {
                    Logger::Log("FocusingLooping | SDK SetResolution failed: " + roiRes.message, 
                               LogLevel::ERROR, DeviceType::FOCUSER);
                    glMainCameraStatu = "IDLE";
                    glIsFocusingLooping = false;
                    isFocusLoopShooting = false;
                    emit wsThread->sendMessageToClient("startFocusLoopFailed:SDK SetResolution failed");
                    return;
                }
                
                // 2. 设置曝光时间（微秒）
                SdkCommand setExpCmd;
                setExpCmd.type = SdkCommandType::Custom;
                setExpCmd.name = "SetExposure";
                setExpCmd.payload = expTime_sec * 1000000.0; // 转换为微秒
                // 直接通过设备句柄调用，无需指定驱动名称
        SdkResult setExpRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, setExpCmd);
                if (!setExpRes.success) {
                    Logger::Log("FocusingLooping | SDK SetExposure failed: " + setExpRes.message, 
                               LogLevel::ERROR, DeviceType::FOCUSER);
                }
                
                // 3. 启动单帧曝光
                SdkCommand startExpCmd;
                startExpCmd.type = SdkCommandType::Custom;
                startExpCmd.name = "StartSingleExposure";
                startExpCmd.payload = std::any();
                // 直接通过设备句柄调用，无需指定驱动名称
        SdkResult startExpRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, startExpCmd);
                if (!startExpRes.success) {
                    Logger::Log("FocusingLooping | SDK StartSingleExposure failed: " + startExpRes.message, 
                               LogLevel::ERROR, DeviceType::FOCUSER);
                    glMainCameraStatu = "IDLE";
                    return;
                }
                Logger::Log("FocusingLooping | SDK StartSingleExposure success, will check image after exposure time", 
                           LogLevel::DEBUG, DeviceType::FOCUSER);
                
                // 4. 使用定时器轮询获取图像（避免阻塞）
                int expTime_ms = static_cast<int>(expTime_sec * 1000);
                sdkExposureStartTime = QDateTime::currentMSecsSinceEpoch();
                sdkExposureExpectedDuration = std::max(1, expTime_ms);
                sdkExposureIsROI = true; // ROI 模式
                
                // 第一次等待时间 = 曝光时间
                sdkExposureTimer->start(std::max(1, expTime_ms));
                Logger::Log("FocusingLooping | SDK exposure timer started, will check after " + std::to_string(expTime_ms) + "ms", 
                           LogLevel::DEBUG, DeviceType::FOCUSER);
            }
            else
            {
                // === INDI 模式 ===（未取 GetEffectiveArea 时 effMin=0，相对坐标即芯片坐标）
                const int indiX = effMinX + scaledX;
                const int indiY = effMinY + scaledY;
                lastFocusExposureSnapshotValid = true;
                lastFocusExposureScaledX = scaledX;
                lastFocusExposureScaledY = scaledY;
                lastFocusExposureEffMinX = effMinX;
                lastFocusExposureEffMinY = effMinY;
                lastFocusExposureRoiCoordScale = std::max(1, roiCoordScale);
                lastFocusExposureRoiW = BoxSideLength;
                lastFocusExposureRoiH = BoxSideLength;

                const QString focusResolvedCfa = resolveFrameCfa(indiX, indiY);
                Logger::Log("FocusingLooping | ROI Bayer debug | previewROI=(" + std::to_string(cameraX) + "," + std::to_string(cameraY) +
                                ") scaled=(" + std::to_string(scaledX) + "," + std::to_string(scaledY) + ")" +
                                ", roiCoordScale=" + std::to_string(roiCoordScale) +
                                ", effRect=(" + std::to_string(effMinX) + "," + std::to_string(effMinY) + "," +
                                std::to_string(effW) + "x" + std::to_string(effH) + ")" +
                                ", roiSize=" + std::to_string(BoxSideLength) + "x" + std::to_string(BoxSideLength) +
                                ", sensorParity=(" + std::to_string(indiX & 1) + "," + std::to_string(indiY & 1) + ")" +
                                ", " + formatBayerPhaseDebug(MainCameraCFA, MainCameraCFAOffsetX, MainCameraCFAOffsetY,
                                                             indiX, indiY, focusResolvedCfa),
                            LogLevel::INFO, DeviceType::FOCUSER);

                Logger::Log("FocusingLooping | INDI setCCDFrameInfo | (" + std::to_string(indiX) + "," + std::to_string(indiY) + ") " + std::to_string(BoxSideLength) + "x" + std::to_string(BoxSideLength),
                            LogLevel::DEBUG, DeviceType::FOCUSER);
                indi_Client->setCCDFrameInfo(dpMainCamera, indiX, indiY, BoxSideLength, BoxSideLength);
                indi_Client->takeExposure(dpMainCamera, expTime_sec);
                Logger::Log("FocusingLooping | INDI takeExposure, expTime_sec:" + std::to_string(expTime_sec), LogLevel::DEBUG, DeviceType::FOCUSER);
            }
        }
        else
        {
            Logger::Log("FocusingLooping | Too close to the edge, please reselect the area.", LogLevel::WARNING, DeviceType::FOCUSER);
            if (scaledX + ROI.width() > effW)
                scaledX = effW - ROI.width();
            if (scaledY + ROI.height() > effH)
                scaledY = effH - ROI.height();

            // 将修正后的坐标反馈给前端（同上：统一回到当前预览坐标系）
            if (roiCoordScale > 0) {
                roiAndFocuserInfo["ROI_x"] = static_cast<double>(scaledX) / roiCoordScale;
                roiAndFocuserInfo["ROI_y"] = static_cast<double>(scaledY) / roiCoordScale;
            }
            
            if (isMainCameraSDK)
            {
                // === SDK 模式（边缘调整后）===
                // 0. 先尝试取消上一帧可能残留的曝光/读出
                {
                    SdkCommand cancelCmd;
                    cancelCmd.type = SdkCommandType::Custom;
                    cancelCmd.name = "CancelExposure";
                    cancelCmd.payload = std::any();
                    // 直接通过设备句柄调用，无需指定驱动名称
                    SdkManager::instance().callByHandle(sdkMainCameraHandle, cancelCmd);
                }

                // ROI 对齐：偶数对齐，贴边后再次确保不越界
                int roiW = ROI.width();
                int roiH = ROI.height();
                if (roiW % 2 != 0) roiW += 1;
                if (roiH % 2 != 0) roiH += 1;
                if (roiW > effW) roiW = effW;
                if (roiH > effH) roiH = effH;
                if ((effMinX + scaledX) % 2 != 0) scaledX = std::max(0, scaledX - 1);
                if ((effMinY + scaledY) % 2 != 0) scaledY = std::max(0, scaledY - 1);
                if (scaledX > effW - roiW) scaledX = effW - roiW;
                if (scaledY > effH - roiH) scaledY = effH - roiH;
                if (scaledX < 0) scaledX = 0;
                if (scaledY < 0) scaledY = 0;

                const int sensorStartXEdge = effMinX + scaledX;
                const int sensorStartYEdge = effMinY + scaledY;

                lastFocusExposureSnapshotValid = true;
                lastFocusExposureScaledX = scaledX;
                lastFocusExposureScaledY = scaledY;
                lastFocusExposureEffMinX = effMinX;
                lastFocusExposureEffMinY = effMinY;
                lastFocusExposureRoiCoordScale = std::max(1, roiCoordScale);
                lastFocusExposureRoiW = roiW;
                lastFocusExposureRoiH = roiH;

                const QString focusResolvedCfa = resolveFrameCfa(sensorStartXEdge, sensorStartYEdge);
                Logger::Log("FocusingLooping | ROI Bayer debug(edge) | previewROI=(" + std::to_string(cameraX) + "," + std::to_string(cameraY) +
                                ") scaled=(" + std::to_string(scaledX) + "," + std::to_string(scaledY) + ")" +
                                ", roiCoordScale=" + std::to_string(roiCoordScale) +
                                ", effRect=(" + std::to_string(effMinX) + "," + std::to_string(effMinY) + "," +
                                std::to_string(effW) + "x" + std::to_string(effH) + ")" +
                                ", roiSize=" + std::to_string(roiW) + "x" + std::to_string(roiH) +
                                ", sensorParity=(" + std::to_string(sensorStartXEdge & 1) + "," + std::to_string(sensorStartYEdge & 1) + ")" +
                                ", " + formatBayerPhaseDebug(MainCameraCFA, MainCameraCFAOffsetX, MainCameraCFAOffsetY,
                                                             sensorStartXEdge, sensorStartYEdge, focusResolvedCfa),
                            LogLevel::INFO, DeviceType::FOCUSER);

                SdkAreaInfo roi;
                roi.startX = static_cast<unsigned int>(sensorStartXEdge);
                roi.startY = static_cast<unsigned int>(sensorStartYEdge);
                roi.sizeX = static_cast<unsigned int>(roiW);
                roi.sizeY = static_cast<unsigned int>(roiH);
                
                SdkCommand setRoiCmd;
                setRoiCmd.type = SdkCommandType::Custom;
                setRoiCmd.name = "SetResolution";
                setRoiCmd.payload = roi;
                Logger::Log("FocusingLooping | SDK SetResolution | start=(" + std::to_string(roi.startX) + "," + std::to_string(roi.startY) + ") size=(" + std::to_string(roi.sizeX) + "x" + std::to_string(roi.sizeY) + ")",
                            LogLevel::DEBUG, DeviceType::FOCUSER);
                // 直接通过设备句柄调用，无需指定驱动名称
                SdkResult roiRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, setRoiCmd);
                if (!roiRes.success) {
                    Logger::Log("FocusingLooping | SDK SetResolution failed: " + roiRes.message, 
                               LogLevel::ERROR, DeviceType::FOCUSER);
                    glMainCameraStatu = "IDLE";
                    glIsFocusingLooping = false;
                    isFocusLoopShooting = false;
                    emit wsThread->sendMessageToClient("startFocusLoopFailed:SDK SetResolution failed");
                    return;
                }
                
                SdkCommand setExpCmd;
                setExpCmd.type = SdkCommandType::Custom;
                setExpCmd.name = "SetExposure";
                setExpCmd.payload = expTime_sec * 1000000.0;
                // 直接通过设备句柄调用，无需指定驱动名称
                SdkManager::instance().callByHandle(sdkMainCameraHandle, setExpCmd);
                
                SdkCommand startExpCmd;
                startExpCmd.type = SdkCommandType::Custom;
                startExpCmd.name = "StartSingleExposure";
                startExpCmd.payload = std::any();
                // 直接通过设备句柄调用，无需指定驱动名称
        SdkResult startExpRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, startExpCmd);
                if (!startExpRes.success) {
                    Logger::Log("FocusingLooping | SDK StartSingleExposure failed: " + startExpRes.message, 
                               LogLevel::ERROR, DeviceType::FOCUSER);
                    glMainCameraStatu = "IDLE";
                    return;
                }
                
                // 使用定时器轮询获取图像
                int expTime_ms = static_cast<int>(expTime_sec * 1000);
                sdkExposureStartTime = QDateTime::currentMSecsSinceEpoch();
                sdkExposureExpectedDuration = std::max(1, expTime_ms);
                sdkExposureIsROI = true; // ROI 模式
                
                sdkExposureTimer->start(std::max(1, expTime_ms));
                Logger::Log("FocusingLooping | SDK exposure timer started (edge adjusted), will check after " + std::to_string(expTime_ms) + "ms", 
                           LogLevel::DEBUG, DeviceType::FOCUSER);
            }
            else
            {
                // === INDI 模式 ===
                const int indiXe = effMinX + scaledX;
                const int indiYe = effMinY + scaledY;
                lastFocusExposureSnapshotValid = true;
                lastFocusExposureScaledX = scaledX;
                lastFocusExposureScaledY = scaledY;
                lastFocusExposureEffMinX = effMinX;
                lastFocusExposureEffMinY = effMinY;
                lastFocusExposureRoiCoordScale = std::max(1, roiCoordScale);
                lastFocusExposureRoiW = ROI.width();
                lastFocusExposureRoiH = ROI.height();

                const QString focusResolvedCfa = resolveFrameCfa(indiXe, indiYe);
                Logger::Log("FocusingLooping | ROI Bayer debug(edge) | previewROI=(" + std::to_string(cameraX) + "," + std::to_string(cameraY) +
                                ") scaled=(" + std::to_string(scaledX) + "," + std::to_string(scaledY) + ")" +
                                ", roiCoordScale=" + std::to_string(roiCoordScale) +
                                ", effRect=(" + std::to_string(effMinX) + "," + std::to_string(effMinY) + "," +
                                std::to_string(effW) + "x" + std::to_string(effH) + ")" +
                                ", roiSize=" + std::to_string(ROI.width()) + "x" + std::to_string(ROI.height()) +
                                ", sensorParity=(" + std::to_string(indiXe & 1) + "," + std::to_string(indiYe & 1) + ")" +
                                ", " + formatBayerPhaseDebug(MainCameraCFA, MainCameraCFAOffsetX, MainCameraCFAOffsetY,
                                                             indiXe, indiYe, focusResolvedCfa),
                            LogLevel::INFO, DeviceType::FOCUSER);

                Logger::Log("FocusingLooping | INDI setCCDFrameInfo | (" + std::to_string(indiXe) + "," + std::to_string(indiYe) + ") " + std::to_string(ROI.width()) + "x" + std::to_string(ROI.height()),
                            LogLevel::DEBUG, DeviceType::FOCUSER);
                indi_Client->setCCDFrameInfo(dpMainCamera, indiXe, indiYe, ROI.width(), ROI.height());
                indi_Client->takeExposure(dpMainCamera, expTime_sec);
            }
        }
    }
    else
    {
        emit wsThread->sendMessageToClient("startFocusLoopFailed:Wait Take Picture Finish!");
    }
    Logger::Log("FocusingLooping finished.", LogLevel::DEBUG, DeviceType::FOCUSER);
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
        // SDK 电调：直接关闭串口句柄
        // 直接通过设备句柄关闭，无需指定驱动名称
        SdkManager::instance().closeByHandle(sdkFocuserHandle);
        sdkFocuserHandle = nullptr;
        sdkFocuserPort.clear();
        systemdevicelist.system_devices[22].isConnect = false;
        systemdevicelist.system_devices[22].isBind = false;
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

void MainWindow::ControlGuide(int Direction, int Duration)
{
    ControlGuideEx(Direction, Duration, QStringLiteral("Legacy/Unknown"));
}

static inline std::string GuideDirNameFromInt(int dir)
{
    switch (dir)
    {
    case 0: return "SOUTH";
    case 1: return "NORTH";
    case 2: return "EAST";
    case 3: return "WEST";
    default: return "UNK";
    }
}

void MainWindow::ControlGuideEx(int Direction, int Duration, const QString& source)
{
    const std::string reqDir = GuideDirNameFromInt(Direction);

#if QUARCS_SIM_GUIDER
    if (simGuiderFrameSource)
    {
        guiding::PulseCommand cmd;
        cmd.dir = static_cast<guiding::GuideDir>(Direction);
        cmd.durationMs = std::max(0, Duration);
        simGuiderFrameSource->injectPulse(cmd);

        Logger::Log("GuidePulse TX | src=" + source.toStdString() +
                        " | SIM inject dir=" + reqDir +
                        " durationMs=" + std::to_string(cmd.durationMs),
                    LogLevel::INFO, DeviceType::GUIDER);
        return;
    }
#endif

    if (!dpMount || !indi_Client)
    {
        Logger::Log("GuidePulse TX | src=" + source.toStdString() + " | ABORT (mount/client null) | dir=" + reqDir +
                        " durationMs=" + std::to_string(Duration),
                    LogLevel::WARNING, DeviceType::GUIDER);
        return;
    }

    // meridian flip: 仅对 NS 做方向映射（按原逻辑保持）
    int actualDir = Direction;
    if (isMeridianFlipped && (Direction == 0 || Direction == 1))
        actualDir = (Direction == 0) ? 1 : 0;

    const std::string actualDirStr = GuideDirNameFromInt(actualDir);
    const std::string axis = (Direction == 0 || Direction == 1) ? "NS" : (Direction == 2 || Direction == 3) ? "WE" : "UNK";

    Logger::Log("GuidePulse TX | src=" + source.toStdString() +
                    " | dir=" + reqDir +
                    " durationMs=" + std::to_string(Duration) +
                    " | axis=" + axis +
                    " meridianFlipped=" + std::string(isMeridianFlipped ? "1" : "0") +
                    " mappedDir=" + actualDirStr + "(" + std::to_string(actualDir) + ")",
                LogLevel::INFO, DeviceType::GUIDER);

    uint32_t ret = QHYCCD_ERROR;
    if (axis == "NS")
        ret = indi_Client->setTelescopeGuideNS(dpMount, actualDir, Duration);
    else if (axis == "WE")
        ret = indi_Client->setTelescopeGuideWE(dpMount, actualDir, Duration);

    if (ret != QHYCCD_SUCCESS)
    {
        Logger::Log("GuidePulse TX | src=" + source.toStdString() + " | FAILED ret=" + std::to_string(ret) +
                        " | dir=" + reqDir + " durationMs=" + std::to_string(Duration),
                    LogLevel::WARNING, DeviceType::GUIDER);
    }
}

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
#endif // PHD2 removed

// ============================================================================
// PHD2 removed: mount move guiding pause/resume stubs
// ============================================================================
// 说明：
// - 头文件 `mainwindow.h` 仍保留了 `pauseGuidingBeforeMountMove()` /
//   `resumeGuidingAfterMountMove()` 的声明，且赤道仪 Goto/Move 流程会调用它们。
// - 旧实现依赖 PHD2 导星状态机（现已通过 `#if 0` 下线），为避免链接错误与行为歧义，
//   这里提供“INDI 直出图”场景下的空实现（只记录日志，不控制导星/循环曝光）。
void MainWindow::pauseGuidingBeforeMountMove()
{
    Logger::Log("pauseGuidingBeforeMountMove | PHD2 removed, no-op (INDI guider imaging continues if enabled).",
                LogLevel::DEBUG, DeviceType::GUIDER);
}

void MainWindow::resumeGuidingAfterMountMove()
{
    Logger::Log("resumeGuidingAfterMountMove | PHD2 removed, no-op (INDI guider imaging continues if enabled).",
                LogLevel::DEBUG, DeviceType::GUIDER);
}

void MainWindow::getFocuserAbsoluteRange(int &absoluteMin, int &absoluteMax) const
{
    absoluteMin = -100000;
    absoluteMax = 100000;
    if (dpFocuser != nullptr)
    {
        int min = -1, max = -1, step = 0, value = 0;
        indi_Client->getFocuserRange(dpFocuser, min, max, step, value);
        if (min != -1 && max != -1 && min < max)
        {
            absoluteMin = min;
            absoluteMax = max;
        }
    }
}

bool MainWindow::maybeExpandFocuserLimitForCalibration(bool isInward, int currentPosition)
{
    if (!focuserManualCalibrationMode)
    {
        return false;
    }

    int absoluteMin = -100000;
    int absoluteMax = 100000;
    getFocuserAbsoluteRange(absoluteMin, absoluteMax);

    const bool reachedPhysicalLimit = isInward ? (currentPosition <= absoluteMin) : (currentPosition >= absoluteMax);
    if (!reachedPhysicalLimit)
    {
        emit wsThread->sendMessageToClient("FocusMoveToLimit:Not at focuser physical limit yet. Calibration move stopped.");
        return false;
    }

    const int desiredDir = isInward ? 1 : -1;
    if (focuserCalibrationExpandedDir != 0 && focuserCalibrationExpandedDir != desiredDir)
    {
        emit wsThread->sendMessageToClient(
            "FocusMoveToLimit:Reached physical limit again immediately after reverse direction during calibration. Movement stopped, please check mechanics.");
        return false;
    }

    focuserCalibrationExpandedDir = desiredDir;
    emit wsThread->sendMessageToClient(
        QString("FocusMoveToLimit:Reached focuser physical %1 limit (%2). Movement stopped.")
            .arg(isInward ? "inner" : "outer")
            .arg(isInward ? absoluteMin : absoluteMax));
    return false;
}

void MainWindow::HandleFocuserMovementDataPeriodically()
{
    Logger::Log("HandleFocuserMovementDataPeriodically | Entry, isFocusMoveDone: " + std::to_string(isFocusMoveDone), LogLevel::DEBUG, DeviceType::FOCUSER);
    
    if (!isFocusMoveDone)
    {
        focusMoveTimer->stop();
        return;
    }

    const bool focuserSdkReady =
        (systemdevicelist.system_devices.size() > 22 &&
         systemdevicelist.system_devices[22].isSDKConnect &&
         systemdevicelist.system_devices[22].isBind &&
         sdkFocuserHandle != nullptr);
    
    Logger::Log("HandleFocuserMovementDataPeriodically | dpFocuser: " + std::string(dpFocuser ? "Valid" : "NULL") + 
                ", focuserSdkReady: " + std::to_string(focuserSdkReady), 
                LogLevel::DEBUG, DeviceType::FOCUSER);
    
    if (dpFocuser == NULL && !focuserSdkReady)
    {
        focusMoveTimer->stop();
        return;
    }

    // SDK 模式：把串口阻塞读取/写入放到 SDK 线程执行，主线程只做状态更新
    if (dpFocuser == NULL && focuserSdkReady)
    {
        Logger::Log("HandleFocuserMovementDataPeriodically | Entering SDK Mode", LogLevel::DEBUG, DeviceType::FOCUSER);
        if (!sdkFocuserExec || !sdkFocuserExec->isRunning())
        {
            focusMoveTimer->stop();
            return;
        }

        // 防重入：上一轮周期任务还没完成时，不叠加新的阻塞调用
        if (sdkFocuserPeriodicTaskInFlight.exchange(true))
            return;

        const SdkDeviceHandle handleSnap = sdkFocuserHandle;
        const bool isInwardSnap = this->currentDirection;
        const int targetSnap = this->TargetPosition;
        int minSnap = this->focuserMinPosition;
        int maxSnap = this->focuserMaxPosition;
        if (focuserManualCalibrationMode)
        {
            getFocuserAbsoluteRange(minSnap, maxSnap);
        }
        const int startPosSnap = this->startPosition;
        const uint64_t epochSnap = sdkFocuserOpEpoch.load(std::memory_order_relaxed);

        sdkFocuserExec->post([this, handleSnap, isInwardSnap, targetSnap, minSnap, maxSnap, startPosSnap, epochSnap]() {
            bool ok = false;
            int curPos = INT_MIN;
            std::string err;

            // 若等待期间已经开始了新的电调操作，则本轮周期任务直接失效（不再占用串口）
            if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
            {
                QMetaObject::invokeMethod(
                    this,
                    [this]() {
                        sdkFocuserPeriodicTaskInFlight = false;
                    },
                    Qt::QueuedConnection);
                return;
            }

            // 1) GetPosition（阻塞串口读回包）
            SdkCommand getPosCmd;
            getPosCmd.type = SdkCommandType::Custom;
            getPosCmd.name = "GetPosition";
            getPosCmd.payload = std::any();
            // 直接通过设备句柄调用，无需指定驱动名称
            SdkResult posRes = SdkManager::instance().callByHandle(handleSnap, getPosCmd);
            if (posRes.success && posRes.payload.has_value())
            {
                try {
                    curPos = std::any_cast<int>(posRes.payload);
                    ok = true;
                } catch (const std::bad_any_cast&) {
                    err = "SDK GetPosition bad_any_cast";
                }
            }
            else
            {
                err = posRes.message;
            }

            // 2) 下发 MoveRelative
            // - 正常情况下：仅当到达上一段目标点（curPos == targetSnap）才下发下一段
            // - 但在“新开始的一次移动”时，TargetPosition/startPosition 可能来自缓存值，
            //   与实际 curPos 不一致；此时也应允许下发第一段移动，否则会出现“收到了 move 但不下发”的现象。
            bool issuedMove = false;
            int newTarget = targetSnap;
            int steps = 0;
            bool limitReached = false;
            std::string limitMsg;

            const bool isFirstSegment = (targetSnap == startPosSnap);
            if (ok && (curPos == targetSnap || isFirstSegment))
            {
                // 若此时被新的操作打断，则不再继续下发 MoveRelative
                if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
                {
                    QMetaObject::invokeMethod(
                        this,
                        [this]() {
                            sdkFocuserPeriodicTaskInFlight = false;
                        },
                        Qt::QueuedConnection);
                    return;
                }

                if (isInwardSnap)
                {
                    steps = curPos - minSnap;
                    if (steps > 60000)
                    {
                        steps = 60000;
                        newTarget = curPos - steps;
                    }
                    else
                    {
                        newTarget = minSnap;
                    }
                    if (steps <= 0)
                    {
                        maybeExpandFocuserLimitForCalibration(true, curPos);
                        limitReached = true;
                        limitMsg.clear();
                    }
                }
                else
                {
                    steps = maxSnap - curPos;
                    if (steps > 60000)
                    {
                        steps = 60000;
                        newTarget = curPos + steps;
                    }
                    else
                    {
                        newTarget = maxSnap;
                    }
                    if (steps <= 0)
                    {
                        maybeExpandFocuserLimitForCalibration(false, curPos);
                        limitReached = true;
                        limitMsg.clear();
                    }
                }

                if (!limitReached && steps > 0)
                {
                    // 再次确认：仍然属于同一代操作
                    if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
                    {
                        QMetaObject::invokeMethod(
                            this,
                            [this]() {
                                sdkFocuserPeriodicTaskInFlight = false;
                            },
                            Qt::QueuedConnection);
                        return;
                    }

                    SdkFocuserRelMoveParam p;
                    p.outward = !isInwardSnap;
                    p.steps = steps;
                    SdkCommand moveCmd{SdkCommandType::Custom, "MoveRelative", p};
                    // 直接通过设备句柄调用，无需指定驱动名称
                    SdkResult mvRes = SdkManager::instance().callByHandle(handleSnap, moveCmd);
                    if (!mvRes.success)
                    {
                        err = "SDK MoveRelative failed: " + mvRes.message;
                    }
                    else
                    {
                        issuedMove = true;
                    }
                }
            }

            QMetaObject::invokeMethod(
                this,
                [this, ok, curPos, err, issuedMove, newTarget, isInwardSnap, steps, limitReached, limitMsg]() {
                    sdkFocuserPeriodicTaskInFlight = false;

                    // 安全检查：若句柄已被清理，直接停止
                    if (sdkFocuserHandle == nullptr)
                        return;
                    
                    // 安全检查：确保 wsThread 有效
                    if (wsThread == nullptr)
                        return;

                    if (!ok)
                    {
                        CurrentPosition = INT_MIN;
                        Logger::Log("HandleFocuserMovementDataPeriodically | SDK get current position failed: " + err,
                                    LogLevel::WARNING, DeviceType::FOCUSER);
                    }
                    else
                    {
                        CurrentPosition = curPos;
                        sdkFocuserPosCache = curPos;
                        sdkFocuserPosValid = true;
                        if (wsThread != nullptr)
                        {
                            emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
                        }
                    }

                    if (limitReached)
                    {
                        if (wsThread != nullptr && !limitMsg.empty())
                        {
                            emit wsThread->sendMessageToClient(QString::fromStdString(limitMsg));
                        }
                        return;
                    }

                    if (issuedMove)
                    {
                        TargetPosition = newTarget;
                        Logger::Log("HandleFocuserMovementDataPeriodically | CurrentPosition: " + std::to_string(CurrentPosition) +
                                        " ,set move steps " + std::to_string(steps) +
                                        (isInwardSnap ? " ,backward inward" : " ,backward outward"),
                                    LogLevel::DEBUG, DeviceType::FOCUSER);
                    }

                    // 安全检查：确保 focusMoveTimer 仍然有效
                    if (focusMoveTimer == nullptr)
                        return;
                    
                    // 安全检查：如果定时器已经停止，不再继续处理
                    if (!focusMoveTimer->isActive())
                        return;

                    focusMoveEndTime -= 0.5;
                    CheckFocuserMoveOrder();
                    if (focusMoveEndTime <= 0)
                    {
                        FocuserControlStop();
                    }
                },
                Qt::QueuedConnection);
        });

        return;
    }

    // INDI 模式：保持原逻辑
    Logger::Log("HandleFocuserMovementDataPeriodically | INDI Mode", LogLevel::DEBUG, DeviceType::FOCUSER);
    CurrentPosition = FocuserControl_getPosition();
    if (focuserIndiNeedResyncTarget)
    {
        TargetPosition = CurrentPosition;
        focuserIndiNeedResyncTarget = false;
    }
    if (CurrentPosition != TargetPosition)
    {
        if (focuserIndiHaveLastPollPos && focuserIndiLastPollPos == CurrentPosition)
            focuserIndiStallCount++;
        else
            focuserIndiStallCount = 0;
        focuserIndiLastPollPos = CurrentPosition;
        focuserIndiHaveLastPollPos = true;
        if (focuserIndiStallCount >= 2)
        {
            TargetPosition = CurrentPosition;
            focuserIndiStallCount = 0;
        }
    }
    else
    {
        focuserIndiStallCount = 0;
        focuserIndiHaveLastPollPos = false;
    }
    emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));

    const bool isInward = this->currentDirection;
    Logger::Log("HandleFocuserMovementDataPeriodically | CurrentPosition: " + std::to_string(CurrentPosition) + 
                ", TargetPosition: " + std::to_string(TargetPosition) + 
                ", isInward: " + std::to_string(isInward), 
                LogLevel::DEBUG, DeviceType::FOCUSER);
    
    int moveMin = focuserMinPosition;
    int moveMax = focuserMaxPosition;
    if (focuserManualCalibrationMode)
    {
        getFocuserAbsoluteRange(moveMin, moveMax);
    }

    if (isInward)
    {
        if (CurrentPosition == TargetPosition)
        {
            int steps = CurrentPosition - moveMin;
            if (steps > kIndiFocuserRelMoveChunkMax)
            {
                steps = kIndiFocuserRelMoveChunkMax;
                TargetPosition = CurrentPosition - steps;
            }
            else
            {
                TargetPosition = moveMin;
            }
            if (steps <= 0)
            {
                maybeExpandFocuserLimitForCalibration(true, CurrentPosition);
                return;
            }
            Logger::Log("HandleFocuserMovementDataPeriodically | INDI move inward, steps: " + std::to_string(steps), LogLevel::DEBUG, DeviceType::FOCUSER);
            indi_Client->setFocuserMoveDiretion(dpFocuser, isInward);
            indi_Client->moveFocuserSteps(dpFocuser, steps);
        }
        else
        {
            Logger::Log("HandleFocuserMovementDataPeriodically | INDI skip move (inward), CurrentPosition != TargetPosition", LogLevel::DEBUG, DeviceType::FOCUSER);
        }
    }
    else
    {
        if (TargetPosition == CurrentPosition)
        {
            int steps = moveMax - CurrentPosition;
            if (steps > kIndiFocuserRelMoveChunkMax)
            {
                steps = kIndiFocuserRelMoveChunkMax;
                TargetPosition = CurrentPosition + steps;
            }
            else
            {
                TargetPosition = moveMax;
            }
            if (steps <= 0)
            {
                maybeExpandFocuserLimitForCalibration(false, CurrentPosition);
                return;
            }
            Logger::Log("HandleFocuserMovementDataPeriodically | INDI move outward, steps: " + std::to_string(steps), LogLevel::DEBUG, DeviceType::FOCUSER);
            indi_Client->setFocuserMoveDiretion(dpFocuser, isInward);
            indi_Client->moveFocuserSteps(dpFocuser, steps);
        }
        else
        {
            Logger::Log("HandleFocuserMovementDataPeriodically | INDI skip move (outward), TargetPosition != CurrentPosition", LogLevel::DEBUG, DeviceType::FOCUSER);
        }
    }

    focusMoveEndTime -= 0.5;
    CheckFocuserMoveOrder();
    if (focusMoveEndTime <= 0)
    {
        FocuserControlStop();
    }
}

void MainWindow::FocuserControlMove(bool isInward)
{
    this->currentDirection = isInward; // 更新当前方向
    // 进入一次新的电调操作：使旧的 stop-位置读取/旧移动任务在 SDK 线程中快速失效
    sdkFocuserOpEpoch.fetch_add(1, std::memory_order_relaxed);
    // 若之前有步进移动的检测定时器残留，先清理，避免状态互相阻塞
    cancelStepMoveIfAny();
    // 允许新移动立即生效：若上一轮周期任务尚未回调清理，避免被 inFlight 直接挡住（导致只 GetPosition 不 MoveRelative）
    sdkFocuserPeriodicTaskInFlight = false;
    const bool focuserSdkReady =
        (systemdevicelist.system_devices.size() > 22 &&
         systemdevicelist.system_devices[22].isSDKConnect &&
         systemdevicelist.system_devices[22].isBind &&
         sdkFocuserHandle != nullptr);
    if (dpFocuser == NULL && !focuserSdkReady)
    {
        Logger::Log("FocuserControlMove | focuser not available (both INDI and SDK are NULL)", LogLevel::WARNING, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("FocusMoveDone:0:0");
        return;
    }
    
    // 停止并清理 updatePositionTimer（如果存在），避免与移动操作冲突
    if (updatePositionTimer != nullptr)
    {
        updatePositionTimer->stop();
        updatePositionTimer->deleteLater();
        updatePositionTimer = nullptr;
    }
    
    // SDK 模式优化：如果位置读取任务正在执行，直接取消它，确保移动命令能够立即执行
    if (focuserSdkReady && sdkFocuserPosTaskInFlight.load()) {
        Logger::Log("FocuserControlMove | Cancel SDK position task to avoid blocking move command", LogLevel::DEBUG, DeviceType::FOCUSER);
        sdkFocuserPosTaskInFlight = false;
    }
    
    focusMoveEndTime = 6;
    isFocusMoveDone = true;

    // 在移动前读取位置
    if (dpFocuser != NULL)
    {
        // INDI 模式：直接读取实际位置，确保 TargetPosition 初始化正确
        CurrentPosition = FocuserControl_getPosition();
    }
    else if (focuserSdkReady)
    {
        // SDK 模式：优先使用缓存值，避免触发位置读取任务阻塞移动命令
        if (sdkFocuserPosValid.load()) {
            CurrentPosition = sdkFocuserPosCache.load();
        } else {
            // 如果缓存无效，使用 0 作为默认值，不触发位置读取任务
            // 位置会在移动过程中通过 HandleFocuserMovementDataPeriodically 更新
            CurrentPosition = 0;
        }
    }
    TargetPosition = CurrentPosition;
    startPosition = CurrentPosition;
    if (dpFocuser != nullptr)
    {
        focuserIndiNeedResyncTarget = true;
        focuserIndiStallCount = 0;
        focuserIndiHaveLastPollPos = false;
    }
    
    Logger::Log("FocuserControlMove | Init Position - CurrentPosition: " + std::to_string(CurrentPosition) + 
                ", TargetPosition: " + std::to_string(TargetPosition) + 
                ", isInward: " + std::to_string(isInward), 
                LogLevel::DEBUG, DeviceType::FOCUSER);
    if (!focuserManualCalibrationMode && CurrentPosition >= focuserMaxPosition && !isInward)
    {
        focuserIndiNeedResyncTarget = false;
        emit wsThread->sendMessageToClient("FocusMoveToLimit:The current position has moved to the outer limit and cannot move further. If you need to continue moving, please recalibrate the position of the servo.");
        focusMoveTimer->stop();
        return;
    }
    else if (!focuserManualCalibrationMode && CurrentPosition <= focuserMinPosition && isInward)
    {
        focuserIndiNeedResyncTarget = false;
        emit wsThread->sendMessageToClient("FocusMoveToLimit:The current position has moved to the inner limit and cannot move further. If you need to continue moving, please recalibrate the position of the servo.");
        focusMoveTimer->stop();
        return;
    }
    HandleFocuserMovementDataPeriodically();
    focusMoveTimer->start(1000);
}

void MainWindow::FocuserControlStop(bool isClickMove, bool silent)
{
    const bool focuserSdkReady =
        (systemdevicelist.system_devices.size() > 22 &&
         systemdevicelist.system_devices[22].isSDKConnect &&
         systemdevicelist.system_devices[22].isBind &&
         sdkFocuserHandle != nullptr);
    if (dpFocuser == NULL && !focuserSdkReady)
    {
        Logger::Log("focusMoveStop | focuser not available (both INDI and SDK are NULL)", LogLevel::WARNING, DeviceType::FOCUSER);
        return;
    }
    // stop 作为一次新的操作边界：让旧的 move/位置读取任务失效
    sdkFocuserOpEpoch.fetch_add(1, std::memory_order_relaxed);
    // 若存在步进移动相关的计时器/状态，先清理，避免 stop 后无法再次 move/step
    cancelStepMoveIfAny();
    // 避免 stop 后立即再次 move 时被 inFlight 阻塞
    sdkFocuserPeriodicTaskInFlight = false;
    Logger::Log("focusMoveStop | Stop Focuser Move", LogLevel::INFO, DeviceType::FOCUSER);
    
    // 优化：在 SDK 模式下，如果位置读取任务正在执行，直接使用缓存值，避免阻塞新的移动命令
    // 对于 INDI 模式，仍然需要调用 getPosition 来获取最新位置
    if (dpFocuser != NULL)
    {
        CurrentPosition = FocuserControl_getPosition();
    }
    else if (focuserSdkReady)
    {
        // SDK 模式：优先使用缓存值，避免触发位置读取任务阻塞新的移动命令
        if (sdkFocuserPosValid.load())
        {
            CurrentPosition = sdkFocuserPosCache.load();
        }
        else
        {
            // 如果缓存无效，且没有位置读取任务在执行，才触发位置更新
            if (!sdkFocuserPosTaskInFlight.load())
            {
                requestSdkFocuserPositionUpdate(false);
            }
            CurrentPosition = sdkFocuserPosValid.load() ? sdkFocuserPosCache.load() : 0;
        }
    }
    
    // if (isClickMove)
    // {
    //     int steps = abs(CurrentPosition - startPosition);
    //     int time = 1;
    //     while (steps < 100 && time < 10)
    //     {
    //         CurrentPosition = FocuserControl_getPosition();
    //         steps = abs(CurrentPosition - startPosition); // 删除int，避免重复声明局部变量
    //         time++;
    //         usleep(100000); // 修改为0.1秒 (100,000微秒)
    //     }
    //     Logger::Log("focusMoveStop | Click Move Steps: " + std::to_string(steps), LogLevel::INFO, DeviceType::FOCUSER);
    // }
    if (dpFocuser != NULL)
    {
        indi_Client->abortFocuserMove(dpFocuser);
    }
    else if (focuserSdkReady)
    {
        // SDK 停止：立即取消位置读取任务，确保 Abort 命令能够立即执行
        // 避免 Abort 命令被位置读取任务阻塞，导致结束命令丢失
        if (sdkFocuserPosTaskInFlight.load()) {
            Logger::Log("focusMoveStop | Cancel position task to ensure Abort command executes immediately", LogLevel::DEBUG, DeviceType::FOCUSER);
            sdkFocuserPosTaskInFlight = false;
        }
        
        // SDK 停止：不要在主线程做阻塞串口调用，投递到 SDK 线程
        if (sdkFocuserExec && sdkFocuserExec->isRunning())
        {
            const SdkDeviceHandle handleSnap = sdkFocuserHandle;
            sdkFocuserExec->post([handleSnap]() {
                SdkCommand abortCmd;
                abortCmd.type = SdkCommandType::Custom;
                abortCmd.name = "Abort";
                abortCmd.payload = std::any();
                // 直接通过设备句柄调用，无需指定驱动名称
                SdkManager::instance().callByHandle(handleSnap, abortCmd);
            });
        }
    }

    if (focusMoveTimer->isActive())
    {
        focusMoveTimer->stop();
    }
    
    // 优化：避免重复调用 getPosition，直接使用上面已经获取的 CurrentPosition
    // 如果需要更新位置，由定时器异步完成，不会阻塞新的移动命令
    isFocusMoveDone = false;
    if (!silent)
    {
        emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
        if (updatePositionTimer != nullptr)
        {
            // 添加计时器以定期更新位置
            updatePositionTimer->stop();
            updatePositionTimer->deleteLater();
            updatePositionTimer = nullptr;
        }
        updatePositionTimer = new QTimer(this);
        updatePositionTimer->setInterval(1000); // 设置计时器间隔为1000毫秒
        updateCount = 0;                        // 初始化计数器

        connect(updatePositionTimer, &QTimer::timeout, [this, sdkSkipCount = 0]() mutable
                {
        // 如果正在移动，立即停止定时器，避免位置读取阻塞移动命令
        if (isFocusMoveDone || updateCount >= 3) {
            updatePositionTimer->stop();
            updatePositionTimer->deleteLater();
            updatePositionTimer = nullptr;
            Logger::Log("focusMoveStop | Timer manually released", LogLevel::INFO, DeviceType::FOCUSER);
            return;
        }
        
        // 检查是否正在移动，如果是则跳过本次位置读取，避免阻塞移动命令
        if (focusMoveTimer && focusMoveTimer->isActive()) {
            // 正在移动，停止定时器
            updatePositionTimer->stop();
            updatePositionTimer->deleteLater();
            updatePositionTimer = nullptr;
            Logger::Log("focusMoveStop | Timer stopped due to focuser moving", LogLevel::INFO, DeviceType::FOCUSER);
            return;
        }
        
        // 检查是否有正在执行的位置读取任务，如果有则跳过本次读取，避免任务堆积
        if (sdkFocuserPosTaskInFlight.load()) {
            // SDK 模式下，位置任务正在执行时不计入“有效刷新次数”，避免 3 次机会被空耗。
            // 但为避免极端情况下定时器长期不释放，连续跳过达到上限后仍会退出。
            sdkSkipCount++;
            if (sdkSkipCount >= 10) {
                updatePositionTimer->stop();
                updatePositionTimer->deleteLater();
                updatePositionTimer = nullptr;
                Logger::Log("focusMoveStop | Timer released after too many SDK in-flight skips", LogLevel::WARNING, DeviceType::FOCUSER);
            }
            return;
        }
        
        // SDK 模式：优先使用缓存值，避免触发位置读取任务阻塞新的移动命令
        // 参考 INDI 驱动的处理方式：定时器更新位置时，如果缓存有效则直接使用
        const bool focuserSdkReady =
            (systemdevicelist.system_devices.size() > 22 &&
             systemdevicelist.system_devices[22].isSDKConnect &&
             systemdevicelist.system_devices[22].isBind &&
             sdkFocuserHandle != nullptr);
        
        if (dpFocuser == NULL && focuserSdkReady)
        {
            // SDK 模式：如果缓存有效，直接使用缓存值，不触发位置读取任务
            if (sdkFocuserPosValid.load())
            {
                CurrentPosition = sdkFocuserPosCache.load();
                emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
                Logger::Log("focusMoveStop | Current Focuser Position: " + std::to_string(CurrentPosition), LogLevel::INFO, DeviceType::FOCUSER);
                sdkSkipCount = 0;
                updateCount++;
                return;
            }
            // 缓存无效，且没有位置读取任务在执行，才触发更新请求
            if (!sdkFocuserPosTaskInFlight.load())
            {
                requestSdkFocuserPositionUpdate(false);
                // SDK 下本轮仅触发异步更新，不把“无效缓存”计作一次有效刷新
                sdkSkipCount++;
                if (sdkSkipCount >= 10) {
                    updatePositionTimer->stop();
                    updatePositionTimer->deleteLater();
                    updatePositionTimer = nullptr;
                    Logger::Log("focusMoveStop | Timer released after SDK cache remained invalid", LogLevel::WARNING, DeviceType::FOCUSER);
                }
                return;
            }
        }
        else
        {
            // INDI 模式：直接调用 getPosition
            CurrentPosition = FocuserControl_getPosition();
            sdkSkipCount = 0;
        }
        
        emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
        Logger::Log("focusMoveStop | Current Focuser Position: " + std::to_string(CurrentPosition), LogLevel::INFO, DeviceType::FOCUSER);
        updateCount++; });
        updatePositionTimer->start(); // 启动计时器
    }
    else
    {
        if (updatePositionTimer != nullptr)
        {
            updatePositionTimer->stop();
            updatePositionTimer->deleteLater();
            updatePositionTimer = nullptr;
        }
    }

    Logger::Log("focusMoveStop | Current Focuser Position: " + std::to_string(CurrentPosition), LogLevel::INFO, DeviceType::FOCUSER);
}

void MainWindow::CheckFocuserMoveOrder()
{
    emit wsThread->sendMessageToClient("getFocuserMoveState");
}

void MainWindow::FocuserControlMoveStep(bool isInward, int steps)
{
    // 记录开始移动焦点器的日志
    Logger::Log("FocuserControlMoveStep start ...", LogLevel::INFO, DeviceType::FOCUSER);
    if (isStepMoving)
    {
        Logger::Log("FocuserControlMoveStep | isStepMoving is true, return", LogLevel::INFO, DeviceType::FOCUSER);
        return;
    }

    // 进入一次新的步进移动：让旧的 stop-位置读取/旧移动任务失效
    sdkFocuserOpEpoch.fetch_add(1, std::memory_order_relaxed);
    // 避免步进移动被上一轮周期任务 inFlight 阻塞（尤其是刚 stop 之后）
    sdkFocuserPeriodicTaskInFlight = false;
    
    // 停止并清理 updatePositionTimer（如果存在），避免与移动操作冲突
    if (updatePositionTimer != nullptr)
    {
        updatePositionTimer->stop();
        updatePositionTimer->deleteLater();
        updatePositionTimer = nullptr;
    }
    
    // 优化：不等待位置读取任务完成，直接使用缓存值，避免移动命令滞后
    // 如果位置读取任务正在执行，直接取消它，确保移动命令能够立即执行
    if (sdkFocuserPosTaskInFlight.load()) {
        Logger::Log("FocuserControlMoveStep | Cancel position task to avoid blocking move command", LogLevel::DEBUG, DeviceType::FOCUSER);
        sdkFocuserPosTaskInFlight = false;
    }
    
    const bool focuserSdkReady =
        (systemdevicelist.system_devices.size() > 22 &&
         systemdevicelist.system_devices[22].isSDKConnect &&
         systemdevicelist.system_devices[22].isBind &&
         sdkFocuserHandle != nullptr);
    if (dpFocuser != NULL || focuserSdkReady)
    {
        // 防重入：已有一次步进移动在进行中时，先取消上一次，避免重复连接与计时器叠加
        cancelStepMoveIfAny();

        // 获取当前焦点器的位置，使用缓存值避免阻塞
        if (sdkFocuserPosValid.load()) {
            CurrentPosition = sdkFocuserPosCache.load();
        } else {
            // 如果缓存无效，尝试读取位置，但不等待结果
            requestSdkFocuserPositionUpdate(false);
            CurrentPosition = sdkFocuserPosValid.load() ? sdkFocuserPosCache.load() : 0;
        }

        if (dpFocuser != NULL){
            CurrentPosition = FocuserControl_getPosition();
        }

        // 根据移动方向计算目标位置
        if(isInward == false)
        {
            TargetPosition = CurrentPosition + steps;
        }
        else
        {
            TargetPosition = CurrentPosition - steps;
        }
        // 记录目标位置的日志
        Logger::Log("FocuserControlMoveStep | Target Position: " + std::to_string(TargetPosition), LogLevel::INFO, DeviceType::FOCUSER);

        // 设置焦点器的移动方向并执行移动
        if (!focuserManualCalibrationMode && TargetPosition > focuserMaxPosition)
        {
            TargetPosition = focuserMaxPosition;
        }
        else if (!focuserManualCalibrationMode && TargetPosition < focuserMinPosition)
        {
            TargetPosition = focuserMinPosition;
        }
        steps = std::abs(TargetPosition - CurrentPosition);
        if (steps <= 0 && !isInward)
        {
            maybeExpandFocuserLimitForCalibration(false, CurrentPosition);
            return;
        }
        else if (steps <= 0 && isInward)
        {
            maybeExpandFocuserLimitForCalibration(true, CurrentPosition);
            return;
        }
        if (steps <= 0)
        {
            return;
        }
        // 标记占用，防止后续点击累加
        isStepMoving = true;
        // 轮询周期参考 INDI：250ms 更平滑；超时时间按毫秒计（默认 10s）
        constexpr int kStepPollMs = 250;
        constexpr int kStepTimeoutMs = 1000;
        stepMoveOutTime = std::max(1, kStepTimeoutMs / kStepPollMs);
        if (dpFocuser != NULL)
        {
            indi_Client->setFocuserMoveDiretion(dpFocuser, isInward);
            indi_Client->moveFocuserSteps(dpFocuser, steps);
        }
        else if (focuserSdkReady)
        {
            SdkFocuserRelMoveParam p;
            p.outward = !isInward;
            p.steps = steps;
            // SDK 模式：异步下发移动，避免主线程阻塞串口
            if (sdkFocuserExec && sdkFocuserExec->isRunning())
            {
                const SdkDeviceHandle handleSnap = sdkFocuserHandle;
                const uint64_t epochSnap = sdkFocuserOpEpoch.load(std::memory_order_relaxed);
                sdkFocuserExec->post([this, handleSnap, p, epochSnap]() {
                    if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
                        return;
                    SdkCommand moveCmd{SdkCommandType::Custom, "MoveRelative", p};
                    // 直接通过设备句柄调用，无需指定驱动名称
                    SdkResult mvRes = SdkManager::instance().callByHandle(handleSnap, moveCmd);
                    if (!mvRes.success)
                    {
                        QMetaObject::invokeMethod(
                            this,
                            [this, msg = mvRes.message]() {
                                Logger::Log("FocuserControlMoveStep | SDK MoveRelative failed: " + msg,
                                            LogLevel::ERROR, DeviceType::FOCUSER);
                                isStepMoving = false;
                                emit wsThread->sendMessageToClient("FocusMoveDone:" + QString::number(CurrentPosition));
                            },
                            Qt::QueuedConnection);
                    }
                });
            }
        }

        // 设置计时器为单次触发
        focusTimer.setSingleShot(true);
        
        // 先断开旧的连接，避免重复连接导致多次回调
        disconnect(&focusTimer, &QTimer::timeout, this, nullptr);

        // 连接定时回调，检查到位与刷位置
        connect(&focusTimer, &QTimer::timeout, this, [this]() {
            stepMoveOutTime--;
            // INDI：同步读取位置
            if (dpFocuser != NULL)
            {
                CurrentPosition = FocuserControl_getPosition();
                emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
            }
            else
            {
                // SDK：不做阻塞读取。优先用缓存值（若无缓存则保持 CurrentPosition 不变），并请求异步更新。
                if (sdkFocuserPosValid.load())
                {
                    CurrentPosition = sdkFocuserPosCache.load();
                    emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
                }
                // 触发一次异步读取（内部会合并请求）；完成后会更新缓存/CurrentPosition，并在 emitWs=true 时推送最新位置
                requestSdkFocuserPositionUpdate(true);
            }
            
            const bool hitSavedLimit = (CurrentPosition <= focuserMinPosition || CurrentPosition >= focuserMaxPosition);
            if ((!focuserManualCalibrationMode && hitSavedLimit) || stepMoveOutTime <= 0 || CurrentPosition == TargetPosition) {
                focusTimer.stop();
                disconnect(&focusTimer, &QTimer::timeout, this, nullptr); // 断开连接，避免重复触发
                isStepMoving = false;
                Logger::Log("FocuserControlMoveStep | Focuser Move Complete!", LogLevel::INFO, DeviceType::FOCUSER);
                emit wsThread->sendMessageToClient("FocusMoveDone:" + QString::number(CurrentPosition));
            } else {
                focusTimer.start(250);
            }
        });
        
        // 启动定时器，开始检查移动状态
        focusTimer.start(250);

    }
    else
    {
        // 如果焦点器对象不存在，记录日志并发送错误消息
        Logger::Log("FocuserControlMoveStep | focuser not available (both INDI and SDK are NULL)", LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("FocusMoveDone:" + QString::number(0));
    }
    // 记录焦点器移动结束的日志
    Logger::Log("FocuserControlMoveStep finish!", LogLevel::INFO, DeviceType::FOCUSER);
}

void MainWindow::cancelStepMoveIfAny()
{
    // 清理可能残留的计时器与状态，避免重复连接/循环
    if (focusTimer.isActive()) focusTimer.stop();
    disconnect(&focusTimer, &QTimer::timeout, this, nullptr); // 断开所有连接
    isStepMoving = false;
}

int MainWindow::FocuserControl_setSpeed(int speed)
{
    Logger::Log("FocuserControl_setSpeed start ...", LogLevel::DEBUG, DeviceType::FOCUSER);
    if (dpFocuser != NULL)
    {
        int value, min, max, step;
        indi_Client->setFocuserSpeed(dpFocuser, speed);
        indi_Client->getFocuserSpeed(dpFocuser, value, min, max, step);
        Logger::Log("FocuserControl_setSpeed | Focuser Speed: " + std::to_string(value) + "," + std::to_string(min) + "," + std::to_string(max) + "," + std::to_string(step), LogLevel::INFO, DeviceType::FOCUSER);
        return value;
    }
    else if (systemdevicelist.system_devices.size() > 22 &&
             systemdevicelist.system_devices[22].isSDKConnect &&
             systemdevicelist.system_devices[22].isBind &&
             sdkFocuserHandle != nullptr)
    {
        // SDK 模式：异步下发，避免主线程阻塞串口
        if (sdkFocuserExec && sdkFocuserExec->isRunning())
        {
            const SdkDeviceHandle handleSnap = sdkFocuserHandle;
            const int speedSnap = speed;
            sdkFocuserExec->post([handleSnap, speedSnap]() {
                SdkCommand setCmd;
                setCmd.type = SdkCommandType::Custom;
                setCmd.name = "SetSpeed";
                setCmd.payload = speedSnap;
                // 直接通过设备句柄调用，无需指定驱动名称
                SdkManager::instance().callByHandle(handleSnap, setCmd);
            });
        }
        return speed;
    }
    Logger::Log("FocuserControl_setSpeed finish!", LogLevel::DEBUG, DeviceType::FOCUSER);
    return speed;
}

int MainWindow::FocuserControl_getSpeed()
{
    Logger::Log("FocuserControl_getSpeed start ...", LogLevel::DEBUG, DeviceType::FOCUSER);
    if (dpFocuser != NULL)
    {
        int value, min, max, step;
        indi_Client->getFocuserSpeed(dpFocuser, value, min, max, step);
        Logger::Log("FocuserControl_getSpeed | Focuser Speed: " + std::to_string(value) + "," + std::to_string(min) + "," + std::to_string(max) + "," + std::to_string(step), LogLevel::INFO, DeviceType::FOCUSER);
        return value;
    }
    else if (systemdevicelist.system_devices.size() > 22 &&
             systemdevicelist.system_devices[22].isSDKConnect &&
             systemdevicelist.system_devices[22].isBind &&
             sdkFocuserHandle != nullptr)
    {
        // SDK 驱动的 GetSpeed 本身是缓存，但仍是一次同步 call；
        // 这里直接返回 UI 侧 currentSpeed（或你已有的速度缓存），避免主线程阻塞。
        return currentSpeed;
    }
    Logger::Log("FocuserControl_getSpeed finish!", LogLevel::DEBUG, DeviceType::FOCUSER);
    return 0;
}

void MainWindow::requestSdkFocuserPositionUpdate(bool emitWs)
{
    const bool focuserSdkReady =
        (systemdevicelist.system_devices.size() > 22 &&
         systemdevicelist.system_devices[22].isSDKConnect &&
         systemdevicelist.system_devices[22].isBind &&
         sdkFocuserHandle != nullptr);

    if (!focuserSdkReady)
        return;

    if (!sdkFocuserExec || !sdkFocuserExec->isRunning())
        return;

    // 合并请求：若已有一个位置读取任务在跑，就不再重复提交
    if (sdkFocuserPosTaskInFlight.exchange(true))
        return;

    const SdkDeviceHandle handleSnap = sdkFocuserHandle;
    const uint64_t epochSnap = sdkFocuserOpEpoch.load(std::memory_order_relaxed);
    sdkFocuserExec->post([this, handleSnap, emitWs, epochSnap]() {
        // 若在队列中等待期间已经开始了新的电调操作（move/stop），则本次位置读取直接失效，
        // 以免旧的 GetPosition 占用串口阻塞后续命令。
        if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
        {
            QMetaObject::invokeMethod(
                this,
                [this]() {
                    sdkFocuserPosTaskInFlight = false;
                },
                Qt::QueuedConnection);
            return;
        }

        int pos = 0;
        std::string err;
        bool ok = false;

        SdkCommand getPosCmd;
        getPosCmd.type = SdkCommandType::Custom;
        getPosCmd.name = "GetPosition";
        getPosCmd.payload = std::any();
        // 直接通过设备句柄调用，无需指定驱动名称
        SdkResult res = SdkManager::instance().callByHandle(handleSnap, getPosCmd);
        if (res.success && res.payload.has_value())
        {
            try {
                pos = std::any_cast<int>(res.payload);
                ok = true;
            } catch (const std::bad_any_cast&) {
                err = "SDK GetPosition bad_any_cast";
            }
        }
        else
        {
            err = res.message;
        }

        // 确保无论成功或失败，都要重置标志，避免任务卡住
        QMetaObject::invokeMethod(
            this,
            [this, ok, pos, err, emitWs]() {
                // 重置标志必须在最前面，确保即使后续代码出错也能重置
                sdkFocuserPosTaskInFlight = false;

                // 安全检查：如果任务已被取消（标志被重置），直接返回
                // 注意：这里不能检查 sdkFocuserPosTaskInFlight，因为我们已经重置了它
                // 但可以检查句柄是否有效
                if (sdkFocuserHandle == nullptr)
                    return;
                
                // 安全检查：确保 wsThread 有效
                if (wsThread == nullptr)
                    return;

                if (!ok)
                {
                    Logger::Log("requestSdkFocuserPositionUpdate | SDK GetPosition failed: " + err,
                                LogLevel::DEBUG, DeviceType::FOCUSER);
                    return;
                }

                sdkFocuserPosCache = pos;
                sdkFocuserPosValid = true;
                CurrentPosition = pos;

                if (emitWs && wsThread != nullptr)
                {
                    emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(pos) + ":" + QString::number(pos));
                }
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::requestSdkFocuserVersionUpdate(bool emitWs)
{
    const bool focuserSdkReady =
        (systemdevicelist.system_devices.size() > 22 &&
         systemdevicelist.system_devices[22].isSDKConnect &&
         systemdevicelist.system_devices[22].isBind &&
         sdkFocuserHandle != nullptr);
    if (!focuserSdkReady)
        return;

    if (!sdkFocuserExec || !sdkFocuserExec->isRunning())
        return;

    const SdkDeviceHandle handleSnap = sdkFocuserHandle;
    const uint64_t epochSnap = sdkFocuserOpEpoch.load(std::memory_order_relaxed);
    sdkFocuserExec->post([this, handleSnap, emitWs, epochSnap]() {
        if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
            return;

        SdkCommand verCmd;
        verCmd.type = SdkCommandType::Custom;
        verCmd.name = "GetVersion";
        verCmd.payload = std::any();
        // 直接通过设备句柄调用，无需指定驱动名称
        SdkResult verRes = SdkManager::instance().callByHandle(handleSnap, verCmd);

        QMetaObject::invokeMethod(
            this,
            [this, verRes, emitWs]() {
                if (!sdkFocuserHandle || wsThread == nullptr)
                    return;

                if (verRes.success && verRes.payload.has_value())
                {
                    try {
                        SdkFocuserVersion ver = std::any_cast<SdkFocuserVersion>(verRes.payload);
                        if (emitWs)
                            emit wsThread->sendMessageToClient("getSDKVersion:Focuser:" + QString::number(ver.version));
                        Logger::Log("requestSdkFocuserVersionUpdate | SDK Focuser version: " + std::to_string(ver.version),
                                    LogLevel::DEBUG, DeviceType::FOCUSER);
                    } catch (const std::bad_any_cast&) {
                        Logger::Log("requestSdkFocuserVersionUpdate | bad_any_cast for SdkFocuserVersion",
                                    LogLevel::WARNING, DeviceType::FOCUSER);
                    }
                }
                else
                {
                    Logger::Log("requestSdkFocuserVersionUpdate | SDK GetVersion failed: " + verRes.message,
                                LogLevel::DEBUG, DeviceType::FOCUSER);
                }
            },
            Qt::QueuedConnection);
    });
}

int MainWindow::FocuserControl_getPosition()
{
    Logger::Log("FocuserControl_getPosition start ...", LogLevel::DEBUG, DeviceType::FOCUSER);
    if (dpFocuser != NULL)
    {
        int value;
        indi_Client->getFocuserAbsolutePosition(dpFocuser, value);
        Logger::Log("FocuserControl_getPosition | Focuser Position: " + std::to_string(value), LogLevel::DEBUG, DeviceType::FOCUSER);
        return value;
    }
    else if (systemdevicelist.system_devices.size() > 22 &&
             systemdevicelist.system_devices[22].isSDKConnect &&
             systemdevicelist.system_devices[22].isBind &&
             sdkFocuserHandle != nullptr)
    {
        // SDK 模式：优先使用缓存值，避免触发位置读取任务阻塞新的移动命令
        // 参考 INDI 驱动的处理方式：移动时不立即读取位置，通过定时器异步更新
        if (sdkFocuserPosValid.load())
        {
            // 缓存有效，直接返回缓存值，不触发更新请求
            const int pos = sdkFocuserPosCache.load();
            Logger::Log("FocuserControl_getPosition | SDK cached position: " + std::to_string(pos),
                        LogLevel::DEBUG, DeviceType::FOCUSER);
            return pos;
        }
        
        // 缓存无效，且没有位置读取任务在执行，才触发更新请求
        // 如果任务正在执行，返回 0 避免阻塞
        if (!sdkFocuserPosTaskInFlight.load())
        {
            requestSdkFocuserPositionUpdate(false);
        }
        Logger::Log("FocuserControl_getPosition | SDK cache invalid, requested async update, returning 0",
                    LogLevel::DEBUG, DeviceType::FOCUSER);
        return 0;
    }
    else
    {
        Logger::Log("FocuserControl_getPosition | dpFocuser is NULL", LogLevel::WARNING, DeviceType::FOCUSER);
        return 0; // 使用 INT_MIN 作为特殊的错误值
    }
}

bool MainWindow::tryReadStableFocuserPosition(int &stablePosition,
                                              int timeoutMs,
                                              int sampleIntervalMs,
                                              int requiredStableSamples,
                                              int toleranceSteps)
{
    const bool focuserSdkReady =
        (systemdevicelist.system_devices.size() > 22 &&
         systemdevicelist.system_devices[22].isSDKConnect &&
         systemdevicelist.system_devices[22].isBind &&
         sdkFocuserHandle != nullptr);
    const bool focuserConnected = (dpFocuser != nullptr || focuserSdkReady);
    if (!focuserConnected)
    {
        Logger::Log("tryReadStableFocuserPosition | focuser not connected", LogLevel::WARNING, DeviceType::FOCUSER);
        return false;
    }

    timeoutMs = std::max(timeoutMs, 800);
    sampleIntervalMs = std::max(sampleIntervalMs, 80);
    requiredStableSamples = std::max(requiredStableSamples, 2);
    toleranceSteps = std::max(toleranceSteps, 1);

    QElapsedTimer timer;
    timer.start();
    QVector<int> recentSamples;

    auto readPositionSample = [&]() -> bool {
        if (dpFocuser == nullptr && focuserSdkReady)
        {
            requestSdkFocuserPositionUpdate(false);
            int waited = 0;
            while (sdkFocuserPosTaskInFlight.load() && waited < 600)
            {
                QThread::msleep(20);
                QCoreApplication::processEvents();
                waited += 20;
            }

            if (!sdkFocuserPosValid.load())
            {
                return false;
            }
        }

        const int current = FocuserControl_getPosition();
        recentSamples.push_back(current);
        if (recentSamples.size() > requiredStableSamples)
        {
            recentSamples.remove(0);
        }
        return true;
    };

    while (timer.elapsed() < timeoutMs)
    {
        if (!readPositionSample())
        {
            recentSamples.clear();
            QThread::msleep(sampleIntervalMs);
            continue;
        }

        if (recentSamples.size() >= requiredStableSamples)
        {
            auto minMaxIt = std::minmax_element(recentSamples.begin(), recentSamples.end());
            auto minIt = minMaxIt.first;
            auto maxIt = minMaxIt.second;
            if ((*maxIt - *minIt) <= toleranceSteps)
            {
                QVector<int> sorted = recentSamples;
                std::sort(sorted.begin(), sorted.end());
                stablePosition = sorted[sorted.size() / 2];
                Logger::Log("tryReadStableFocuserPosition | stable position=" + std::to_string(stablePosition) +
                                ", samples=" + std::to_string(sorted[0]) + "," +
                                std::to_string(sorted[sorted.size() - 1]),
                            LogLevel::INFO, DeviceType::FOCUSER);
                return true;
            }
        }

        QThread::msleep(sampleIntervalMs);
    }

    Logger::Log("tryReadStableFocuserPosition | timeout, latest samples count=" +
                    std::to_string(recentSamples.size()),
                LogLevel::WARNING, DeviceType::FOCUSER);
    return false;
}

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
void MainWindow::ScheduleTabelData(QString message)
{
    ScheduleTargetNames.clear();
    m_scheduList.clear();
    // 每次接收到新的任务计划表时，从第一条任务开始执行
    schedule_currentNum = 0;
    schedule_currentShootNum = 0;
    // 新任务计划表开始时，重置 Refocus 触发记录，避免旧任务残留导致本次不触发
    schedule_refocusTriggeredIndex = -1;
    QStringList ColDataList = message.split('[');
    for (int i = 1; i < ColDataList.size(); ++i)
    {
        QString ColData = ColDataList[i]; // ",M 24, Ra:4.785693,Dec:-0.323759,12:00:00,1 s,Ha,,Bias,ON,],"
        ScheduleData rowData;
        rowData.exposureDelay = 0; // 初始化曝光延迟为0
        qDebug() << "ColData[" << i << "]:" << ColData;

        QStringList RowDataList = ColData.split(',');
        if (RowDataList.size() <= 10)
        {
            Logger::Log(QString("ScheduleTabelData | row %1 has insufficient columns: %2").arg(i).arg(RowDataList.size()).toStdString(),
                        LogLevel::WARNING, DeviceType::MAIN);
            continue;
        }

        for (int j = 1; j <= 10; ++j)
        {
            // 防御性检查：确保索引有效
            if (j >= RowDataList.size())
            {
                Logger::Log(QString("ScheduleTabelData | row %1 column index %2 out of range (size=%3)")
                                .arg(i).arg(j).arg(RowDataList.size()).toStdString(),
                            LogLevel::WARNING, DeviceType::MAIN);
                break;
            }

            if (j == 1)
            {
                rowData.shootTarget = RowDataList[j];
                rowData.shootType = RowDataList[j + 7];
                qDebug() << "Target:" << rowData.shootTarget;
                qDebug() << "Type:" << rowData.shootType;
                // 将 shootTarget 添加到 ScheduleTargetNames 中
                if (!ScheduleTargetNames.isEmpty())
                {
                    ScheduleTargetNames += ",";
                }
                ScheduleTargetNames += rowData.shootTarget + "," + rowData.shootType;
            }
            else if (j == 2)
            {
                QStringList parts = RowDataList[j].split(':');
                if (parts.size() >= 2)
                {
                rowData.targetRa = Tools::RadToHour(parts[1].toDouble());
                }
                else
                {
                    rowData.targetRa = 0;
                    Logger::Log(QString("ScheduleTabelData | row %1 invalid RA field: %2").arg(i).arg(RowDataList[j]).toStdString(),
                                LogLevel::WARNING, DeviceType::MAIN);
                }
                qDebug() << "Ra:" << rowData.targetRa;
            }
            else if (j == 3)
            {
                QStringList parts = RowDataList[j].split(':');
                if (parts.size() >= 2)
                {
                rowData.targetDec = Tools::RadToDegree(parts[1].toDouble());
                }
                else
                {
                    rowData.targetDec = 0;
                    Logger::Log(QString("ScheduleTabelData | row %1 invalid Dec field: %2").arg(i).arg(RowDataList[j]).toStdString(),
                                LogLevel::WARNING, DeviceType::MAIN);
                }
                qDebug() << "Dec:" << rowData.targetDec;
            }
            else if (j == 4)
            {
                rowData.shootTime = RowDataList[j];
                qDebug() << "Time:" << rowData.shootTime;
            }
            else if (j == 5)
            {
                QStringList parts = RowDataList[j].split(' ');
                if (parts.isEmpty())
                {
                    rowData.exposureTime = 1000; // 默认 1s
                    Logger::Log(QString("ScheduleTabelData | row %1 invalid exposure field: %2, fallback to 1000ms")
                                    .arg(i).arg(RowDataList[j]).toStdString(),
                                LogLevel::WARNING, DeviceType::MAIN);
                    qDebug() << "Exptime error, Exptime = 1000 ms";
                    continue;
                }

                QString value = parts[0];
                QString unit = (parts.size() > 1) ? parts[1] : "s";
                if (unit == "s")
                    rowData.exposureTime = value.toInt() * 1000; // Convert seconds to milliseconds
                else if (unit == "ms")
                    rowData.exposureTime = value.toInt(); // Milliseconds
                if (rowData.exposureTime == 0)
                {
                    rowData.exposureTime = 1000;
                    qDebug() << "Exptime error, Exptime = 1000 ms";
                }
                qDebug() << "Exptime:" << rowData.exposureTime;
            }
            else if (j == 6)
            {
                rowData.filterNumber = RowDataList[j];
                qDebug() << "CFW:" << rowData.filterNumber;
            }
            else if (j == 7)
            {
                if (RowDataList[j] == "")
                {
                    rowData.repeatNumber = 1;
                    qDebug() << "Repeat error, Repeat = 1";
                }
                else
                {
                    rowData.repeatNumber = RowDataList[j].toInt();
                }
                qDebug() << "Repeat:" << rowData.repeatNumber;
            }
            // else if (j == 8)
            // {
            //     rowData.shootType = RowDataList[j];
            //     qDebug() << "Type:" << rowData.shootType;
            // }
            else if (j == 9)
            {
                rowData.resetFocusing = (RowDataList[j] == "ON");
                qDebug() << "Focus:" << rowData.resetFocusing;
            }
            else if (j == 10)
            {
                QStringList parts = RowDataList[j].split(' ');
                if (parts.isEmpty())
                {
                    rowData.exposureDelay = 0;
                    Logger::Log(QString("ScheduleTabelData | row %1 invalid exposure delay field: %2, fallback to 0")
                                    .arg(i).arg(RowDataList[j]).toStdString(),
                                LogLevel::WARNING, DeviceType::MAIN);
                    qDebug() << "Exposure Delay error, use 0 ms";
                    continue;
                }

                QString value = parts[0];
                QString unit = parts.size() > 1 ? parts[1] : "s";
                if (unit == "s")
                    rowData.exposureDelay = value.toInt() * 1000; // Convert seconds to milliseconds
                else if (unit == "ms")
                    rowData.exposureDelay = value.toInt(); // Milliseconds
                else
                    rowData.exposureDelay = 0; // Default to 0 if unit is not recognized
                qDebug() << "Exposure Delay:" << rowData.exposureDelay << "ms";
            }
        }
        rowData.progress = 0;
        // scheduleTable.Data.push_back(rowData);
        m_scheduList.append(rowData);
    }

    // 同步更新暂存的任务计划表数据，方便前端在刷新后通过 getStagingScheduleData 恢复当前计划
    // 前端发送格式为 "ScheduleTabelData:[,Target,...[,...]]"，这里复用内容，仅替换成 StagingScheduleData 前缀
    if (message.startsWith("ScheduleTabelData:"))
    {
        QString stagingMessage = message;
        stagingMessage.replace(0,
                               QString("ScheduleTabelData:").length(),
                               "StagingScheduleData:");
        isStagingScheduleData = true;
        StagingScheduleData = stagingMessage;
    }

    startSchedule();
}
void MainWindow::startSchedule()
{
    createScheduleDirectory();
    if (schedule_currentNum >= 0 && schedule_currentNum < m_scheduList.size())
    {
        qDebug() << "startSchedule...";
        schedule_ExpTime = m_scheduList[schedule_currentNum].exposureTime;
        schedule_RepeatNum = m_scheduList[schedule_currentNum].repeatNumber;
        schedule_CFWpos = m_scheduList[schedule_currentNum].filterNumber.toInt();
        schedule_ExposureDelay = m_scheduList[schedule_currentNum].exposureDelay;
        StopSchedule = false;
        isScheduleRunning = true;
        // 通知前端：计划任务当前处于运行状态
        emit wsThread->sendMessageToClient("ScheduleRunning:true");
        startTimeWaiting();
    }
    else
    {
        qDebug() << "Index out of range, Schedule is complete!";
        StopSchedule = true;
        isScheduleRunning = false;
        schedule_currentNum = 0;
        // TODO(PHD2): 计划任务结束时停止 PHD2 循环曝光（PHD2 已移除/禁用）
        // call_phd_StopLooping();
        GuidingHasStarted = false;
        // 通知前端计划任务已完成，重置按钮状态
        emit wsThread->sendMessageToClient("ScheduleComplete");
        emit wsThread->sendMessageToClient("ScheduleRunning:false");
        // 在实际应用中，你可能想要返回一个默认值或者处理索引超出范围的情况
        // 这里仅仅是一个简单的示例
        // return ScheduleData();
    }
}

void MainWindow::nextSchedule()
{
    // next schedule...
    schedule_currentNum++;
    qDebug() << "next schedule...";
    startSchedule();
}

void MainWindow::startTimeWaiting()
{
    // m_scheduList[schedule_currentNum].progress=0;
    // emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
    qDebug() << "startTimeWaiting...";
    // 停止和清理先前的计时器
    timewaitingTimer.stop();
    timewaitingTimer.disconnect();
    // 启动等待的定时器
    timewaitingTimer.setSingleShot(true);
    connect(&timewaitingTimer, &QTimer::timeout, [this]()
            {
        if (StopSchedule)
        {
            StopSchedule = false;
            qDebug("Schedule is stop!");
            return;
        }

        if (WaitForTimeToComplete()) 
        {
            timewaitingTimer.stop();  // 完成时停止定时器
            qDebug() << "Time Waiting Complete...";
            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(1, 1.0);  // 步骤1完成：等待时间
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "wait:" +
                "0:" +
                "0:" +
                "100");

            startMountGoto(m_scheduList[schedule_currentNum].targetRa, m_scheduList[schedule_currentNum].targetDec);
        } 
        else 
        {
            timewaitingTimer.start(1000);  // 继续等待
        } });
    timewaitingTimer.start(1000);
}

void MainWindow::startMountGoto(double ra, double dec) // Ra:Hour, Dec:Degree
{
    if (dpMount == NULL)
    {
        m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(2, 1.0);  // 无赤道仪时直接跳到步骤2完成
        emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
        emit wsThread->sendMessageToClient(
            "ScheduleStepState:" +
            QString::number(schedule_currentNum) + ":" +
            "mount:" +
            "0:" +
            "0:" +
            "100");
        Logger::Log("startMountGoto | dpMount is NULL,goto failed!Skip to set CFW.", LogLevel::ERROR, DeviceType::MAIN);
        startSetCFW(schedule_CFWpos);
        return;
    }

    qDebug() << "startMountGoto...";
    // 停止和清理先前的计时器
    telescopeTimer.stop();
    telescopeTimer.disconnect();

    qDebug() << "Mount Goto:" << ra << "," << dec;
    MountGotoError = false;

    auto now = std::chrono::system_clock::now();
    double observatory_lon = 116.14; // 东经116.14度
    double lst = computeLST(observatorylongitude, now);

    // TelescopeControl_Goto(ra, dec);
    double RA_HOURS, DEC_DEGREE;
    indi_Client->getTelescopeRADECJNOW(dpMount, RA_HOURS, DEC_DEGREE);
    double CurrentRA_Degree = Tools::HourToDegree(RA_HOURS);
    double CurrentDEC_Degree = DEC_DEGREE;

    // performObservation(
    //     CurrentRA_Degree, CurrentDEC_Degree,
    //     ra, dec,
    //     observatorylongitude,observatorylatitude) ;
    // 在执行观测（GOTO）前，如当前处于导星状态，则暂时停止导星，待转动完成后再恢复
    pauseGuidingBeforeMountMove();

    performObservation(
        lst, CurrentDEC_Degree,
        ra, dec,
        observatorylongitude, observatorylatitude);

    sleep(2); // 赤道仪的状态更新有一定延迟

    // 步骤2：赤道仪转动，明确发送“开始移动”的细分信号，方便前端显示循环进度条
    // 这里将该步骤标记为进行中（本地进度 0.5），但 stepProgress 使用 0，表示刚开始。
    m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(2, 0.5);
    emit wsThread->sendMessageToClient(
        "UpdateScheduleProcess:" +
        QString::number(schedule_currentNum) + ":" +
        QString::number(m_scheduList[schedule_currentNum].progress));
    emit wsThread->sendMessageToClient(
        "ScheduleStepState:" +
        QString::number(schedule_currentNum) + ":" +
        "mount:" +
        "0:" +
        "0:" +
        "0");

    // 启动等待赤道仪转动的定时器
    telescopeTimer.setSingleShot(true);

    connect(&telescopeTimer, &QTimer::timeout, [this]()
            {
        if (StopSchedule)
        {
            StopSchedule = false;
            qDebug("Schedule is stop!");

            if (dpMount != NULL)
            {
                indi_Client->setTelescopeAbortMotion(dpMount);
            }

            return;
        }
        if (WaitForTelescopeToComplete()) 
        {
            telescopeTimer.stop();  // 转动完成时停止定时器
            qDebug() << "Mount Goto Complete!";

            if(MountGotoError) {
                MountGotoError = false;

                nextSchedule();

                return;
            }

            // 如果本次 GOTO 之前处于导星状态，则在赤道仪转动完成后恢复导星
            resumeGuidingAfterMountMove();

            qDebug() << "Mount Goto Complete...";
            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(2, 1.0);  // 步骤2完成：赤道仪转动
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "mount:" +
                "0:" +
                "0:" +
                "100");
            startSetCFW(schedule_CFWpos);
        } 
        else 
        {
            telescopeTimer.start(1000);  // 继续等待
        } });

    telescopeTimer.start(1000);
}

bool MainWindow::needsMeridianFlip(double lst, double targetRA)
{
    double hourAngle = lst - targetRA;
    hourAngle = fmod(hourAngle + 24.0, 24.0);
    if (hourAngle > 12.0)
        hourAngle -= 24.0;
    return (hourAngle > 0);
}

void MainWindow::performObservation(
    double lst, double currentDec,
    double targetRA, double targetDec,
    double observatoryLongitude, double observatoryLatitude)
{
    // if (needsMeridianFlip(lst, targetRA))
    // {
    //     std::cout << "Meridian flip is needed." << std::endl;
    //     TelescopeControl_Goto(lst, observatoryLatitude);
    //     std::cout << "Performing meridian flip..." << std::endl;
    //     std::this_thread::sleep_for(std::chrono::seconds(60));
    //     TelescopeControl_Goto(targetRA, targetDec);
    // }
    // else
    // {
        std::cout << "No flip needed. Moving directly." << std::endl;
        TelescopeControl_Goto(targetRA, targetDec);
    // }
}

// 计算儒略日（JD）
double MainWindow::getJulianDate(const std::chrono::system_clock::time_point &utc_time)
{
    auto duration = utc_time.time_since_epoch();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    return 2440587.5 + seconds / 86400.0; // Unix时间转儒略日
}

// 计算格林尼治恒星时（GMST）
double MainWindow::computeGMST(const std::chrono::system_clock::time_point &utc_time)
{
    const double JD = getJulianDate(utc_time);
    const double T = (JD - 2451545.0) / 36525.0; // 以J2000为基准的世纪数

    // IAU公式（精度±0.1秒）
    double GMST = 280.46061837 + 360.98564736629 * (JD - 2451545.0) + 0.000387933 * T * T - T * T * T / 38710000.0;

    // 规范化到0-360度范围
    GMST = fmod(GMST, 360.0);
    if (GMST < 0)
        GMST += 360.0;

    return GMST / 15.0; // 转换为小时单位
}

// 计算地方恒星时（LST）
double MainWindow::computeLST(double longitude_east, const std::chrono::system_clock::time_point &utc_time)
{
    double GMST_hours = computeGMST(utc_time);
    double LST = GMST_hours + longitude_east / 15.0; // 东经为正
    return fmod(LST + 24.0, 24.0);                   // 确保结果在0-24之间
}

void MainWindow::startGuiding()
{
    qDebug() << "startGuiding...";
    // TODO(PHD2): 导星阶段原依赖 PHD2（Looping/AutoFindStar/StartGuiding/状态轮询）。
    // 当前项目已切换到 INDI 导星相机直出图，计划任务里不再等待 PHD2 导星完成，直接进入后续流程。
    GuidingHasStarted = false;
    startSetCFW(schedule_CFWpos);
}

void MainWindow::startSetCFW(int pos)
{
    qDebug() << "startSetCFW...";
    
    // 检查是否需要自动对焦（Refocus为ON）
    if (schedule_currentNum >= 0 && schedule_currentNum < m_scheduList.size() && 
        m_scheduList[schedule_currentNum].resetFocusing &&
        schedule_refocusTriggeredIndex != schedule_currentNum)
    {
        qDebug() << "Refocus is ON, starting autofocus before setting CFW...";
        Logger::Log("计划任务表: Refocus为ON，在执行拍摄前先执行自动对焦（仅最后一步精调）", LogLevel::INFO, DeviceType::MAIN);
        
        // 先记录本行已触发过一次 Refocus，避免 AutoFocus 完成回调再次进入 startSetCFW 时无限重入
        schedule_refocusTriggeredIndex = schedule_currentNum;
        
        // 启动自动对焦（startScheduleAutoFocus会设置isScheduleTriggeredAutoFocus标志）
        startScheduleAutoFocus();
        return; // 自动对焦完成后会继续执行startSetCFW
    }
    
    // 如果不需要自动对焦，继续正常流程
    if (isFilterOnCamera)
    {
        // CFW 在相机上：INDI / SDK 双通路兼容（前端/计划任务使用 1-based 槽位编号）
        if (!isMainCameraSDK() && dpMainCamera != NULL)
        {
            qDebug() << "schedule CFW pos:" << pos;
            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(3, 0.5);  // 步骤3进行中：开始设置滤镜
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "filter:" +
                "0:" +
                "0:" +
                "50");
            indi_Client->setCFWPosition(dpMainCamera, pos);
            qDebug() << "CFW Goto Complete...";
            startCapture(schedule_ExpTime);
            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(3, 1.0);  // 步骤3完成：滤镜设置完成
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "filter:" +
                "0:" +
                "0:" +
                "100");
        }
        else if (isMainCameraSDK() && sdkMainCameraHandle != nullptr)
        {
            qDebug() << "schedule CFW pos (SDK 1-based):" << pos;
            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(3, 0.5);  // 步骤3进行中：开始设置滤镜
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "filter:" +
                "0:" +
                "0:" +
                "50");

            int target0 = toSdkCfwPos0(pos);
            std::string err;
            bool ok = sdkSetCfwPosition0AndWait(sdkMainCameraHandle, target0, 10000, &err);
            if (!ok)
            {
                Logger::Log("startSetCFW | SDK set CFW failed: " + err + " (continue capture)", LogLevel::WARNING, DeviceType::MAIN);
            }
            qDebug() << "CFW Goto Complete (SDK)...";
            startCapture(schedule_ExpTime);

            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(3, 1.0);  // 步骤3完成：滤镜设置完成
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "filter:" +
                "0:" +
                "0:" +
                "100");
        }
        else
        {
            Logger::Log("startSetCFW | dpMainCamera is NULL,set CFW failed!", LogLevel::ERROR, DeviceType::MAIN);

            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(3, 1.0);  // 无滤镜时直接跳到步骤3完成
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            startCapture(schedule_ExpTime);
        }
    }
    else
    {
        if (dpCFW != NULL)
        {
            qDebug() << "schedule CFW pos:" << pos;
            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(3, 0.5);  // 步骤3进行中：开始设置滤镜
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "filter:" +
                "0:" +
                "0:" +
                "50");
            indi_Client->setCFWPosition(dpCFW, pos);
            qDebug() << "CFW Goto Complete...";
            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(3, 1.0);  // 步骤3完成：滤镜设置完成
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "filter:" +
                "0:" +
                "0:" +
                "100");
            startCapture(schedule_ExpTime);
        }
        else
        {
            Logger::Log("startSetCFW | dpCFW is NULL,set CFW failed!", LogLevel::ERROR, DeviceType::MAIN);
            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(3, 1.0);  // 无滤镜时直接跳到步骤3完成
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            startCapture(schedule_ExpTime);
        }
    }
}

void MainWindow::startExposureDelay()
{
    qDebug() << "startExposureDelay...";
    Logger::Log(("Waiting exposure delay: " + QString::number(schedule_ExposureDelay) + " ms before next capture").toStdString(), LogLevel::INFO, DeviceType::MAIN);
    qDebug() << "Waiting exposure delay:" << schedule_ExposureDelay << "ms before next capture";
    
    // 停止和清理先前的延迟定时器
    exposureDelayTimer.stop();
    exposureDelayTimer.disconnect();
    
    // 重置已过去的时间
    exposureDelayElapsed_ms = 0;
    
    // 使用可控制的定时器，每100ms检查一次
    exposureDelayTimer.setSingleShot(false);
    // 向前端发送步骤状态：进入延时阶段
    emit wsThread->sendMessageToClient(
        "ScheduleStepState:" +
        QString::number(schedule_currentNum) + ":" +
        "delay:" +
        "0:" +
        "0:" +
        "0");
    connect(&exposureDelayTimer, &QTimer::timeout, [this]() {
        // 首先检查是否已停止，必须在最开始检查
        // 这个检查必须在任何其他操作之前，确保能够立即响应停止信号
        if (StopSchedule)
        {
            // 立即停止定时器并清理
            exposureDelayTimer.stop();
            exposureDelayTimer.disconnect();
            Logger::Log(("Exposure delay interrupted: Schedule stopped during delay wait (elapsed: " + QString::number(exposureDelayElapsed_ms) + " ms)").toStdString(), LogLevel::INFO, DeviceType::MAIN);
            qDebug() << "Exposure delay interrupted: Schedule stopped during delay wait (elapsed:" << exposureDelayElapsed_ms << "ms)";
            exposureDelayElapsed_ms = 0; // 重置已过去的时间
            return; // 立即返回，不执行任何后续操作
        }
        
        // 检查定时器是否仍然激活（防止在回调执行期间被停止）
        if (!exposureDelayTimer.isActive())
        {
            qDebug() << "Exposure delay timer is not active, returning";
            return;
        }
        
        // 增加已过去的时间
        exposureDelayElapsed_ms += 100; // 每次增加100ms
        
        // 再次检查 StopSchedule（可能在增加时间的过程中被设置）
        if (StopSchedule)
        {
            exposureDelayTimer.stop();
            exposureDelayTimer.disconnect();
            Logger::Log(("Exposure delay interrupted: Schedule stopped during delay wait (elapsed: " + QString::number(exposureDelayElapsed_ms) + " ms)").toStdString(), LogLevel::INFO, DeviceType::MAIN);
            qDebug() << "Exposure delay interrupted: Schedule stopped during delay wait (elapsed:" << exposureDelayElapsed_ms << "ms)";
            exposureDelayElapsed_ms = 0; // 重置已过去的时间
            return; // 立即返回，不执行任何后续操作
        }
        
        // 向前端报告当前延迟阶段的进度（0-100）
        if (schedule_ExposureDelay > 0)
        {
            int progress = static_cast<int>(
                qMin(100.0, exposureDelayElapsed_ms * 100.0 / static_cast<double>(schedule_ExposureDelay)));
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "delay:" +
                "0:" +
                "0:" +
                QString::number(progress));
        }
        
        // 检查是否已经过了延迟时间
        if (exposureDelayElapsed_ms >= schedule_ExposureDelay)
        {
            exposureDelayTimer.stop();
            exposureDelayTimer.disconnect();
            qDebug() << "Exposure delay complete, starting next capture";
            exposureDelayElapsed_ms = 0; // 重置已过去的时间
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "delay:" +
                "0:" +
                "0:" +
                "100");
            startCapture(schedule_ExpTime);
        }
    });
    
    exposureDelayTimer.start(100); // 每100ms检查一次
}

void MainWindow::startCapture(int ExpTime)
{
    qDebug() << "startCapture...";
    // 停止和清理先前的计时器
    captureTimer.stop();
    captureTimer.disconnect();
    // 停止曝光延迟定时器（如果正在运行）
    exposureDelayTimer.stop();
    exposureDelayTimer.disconnect();

    ShootStatus = "InProgress";
    qDebug() << "ShootStatus: " << ShootStatus;
    startMainCameraCapture(ExpTime);
    schedule_currentShootNum++;

    captureTimer.setSingleShot(true);

    expTime_ms = 0;

    connect(&captureTimer, &QTimer::timeout, [this]()
            {
        if (StopSchedule)
        {
            StopSchedule = false;
            abortMainCameraCapture();
            qDebug("Schedule is stop!");
            return;
        }
        
        // 检查赤道仪状态
        if (WaitForShootToComplete()) 
        {
            captureTimer.stop();  // 转动完成时停止定时器
            qDebug() << "Capture" << schedule_currentShootNum << "Complete!";
            ScheduleImageSave(m_scheduList[schedule_currentNum].shootTarget, schedule_currentShootNum);
            
            // 计算当前拍摄完成的进度
            // 拍摄步骤从步骤4开始，每张照片对应一个步骤
            // 步骤编号 = 3 + schedule_currentShootNum
            int currentStep = 3 + schedule_currentShootNum;
            m_scheduList[schedule_currentNum].progress = calculateScheduleProgress(currentStep, 1.0);
            emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "exposure:" +
                QString::number(schedule_currentShootNum) + ":" +
                QString::number(schedule_RepeatNum) + ":" +
                "100");

            // 同步更新循环进度（loopDone）：每完成一张就回传当前已完成张数和总张数，
            // 便于前端实时显示 “已拍/总拍” 的精细进度，而不是只在最后一次拍摄结束时一次性更新。
            if (schedule_RepeatNum > 0)
            {
                int loopProgress = static_cast<int>(
                    qMin(100.0, schedule_currentShootNum * 100.0 / static_cast<double>(schedule_RepeatNum)));
                emit wsThread->sendMessageToClient(
                    // 专用循环状态信号：ScheduleLoopState:row:loopDone:loopTotal:progress
                    "ScheduleLoopState:" +
                    QString::number(schedule_currentNum) + ":" +
                    QString::number(schedule_currentShootNum) + ":" +
                    QString::number(schedule_RepeatNum) + ":" +
                    QString::number(loopProgress));
            }

            if (schedule_currentShootNum < schedule_RepeatNum)
            {
                // 如果有曝光延迟，等待指定时间后再开始下一张
                if (schedule_ExposureDelay > 0)
                {
                    startExposureDelay();
                }
                else
                {
                    // 没有延迟，直接开始下一张
                    startCapture(schedule_ExpTime);
                }
            }
            else
            {
                schedule_currentShootNum = 0;

                m_scheduList[schedule_currentNum].progress=100;
                emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));   
                qDebug() << "Capture Goto Complete...";
                nextSchedule();
            }

        } 
        else 
        {
            expTime_ms += 1000;
            
            // 检查是否超过最大超时时间（曝光时间 + 1分钟）
            int maxTimeout = schedule_ExpTime + 60000;  // 曝光时间 + 60000毫秒（1分钟）
            if (expTime_ms > maxTimeout)
            {
                // 拍摄超时，中止拍摄并处理超时情况
                captureTimer.stop();
                abortMainCameraCapture();
                Logger::Log(QString("计划任务表拍摄超时: 当前拍摄时间 %1ms, 超过最大超时时间 %2ms (曝光时间 %3ms + 1分钟)").arg(expTime_ms).arg(maxTimeout).arg(schedule_ExpTime).toStdString(), 
                           LogLevel::WARNING, DeviceType::MAIN);
                Logger::Log("Capture timeout! expTime_ms:" + std::to_string(expTime_ms) + ", maxTimeout:" + std::to_string(maxTimeout) + ", schedule_ExpTime:" + std::to_string(schedule_ExpTime), LogLevel::WARNING, DeviceType::MAIN);
                
                // 跳过当前拍摄，继续下一个拍摄或任务
                if (schedule_currentShootNum < schedule_RepeatNum)
                {
                    // 还有后续拍摄，继续下一个拍摄
                    qDebug() << "Skip current capture, continue to next capture...";
                    // 如果有曝光延迟，等待指定时间后再开始下一张
                    if (schedule_ExposureDelay > 0)
                    {
                        startExposureDelay();
                    }
                    else
                    {
                        // 没有延迟，直接开始下一张
                        startCapture(schedule_ExpTime);
                    }
                }
                else
                {
                    // 当前任务的所有拍摄已完成或超时，进入下一个任务
                    schedule_currentShootNum = 0;
                    qDebug() << "All captures completed or timeout, move to next schedule...";
                    m_scheduList[schedule_currentNum].progress = 100;
                    emit wsThread->sendMessageToClient("UpdateScheduleProcess:" + QString::number(schedule_currentNum) + ":" + QString::number(m_scheduList[schedule_currentNum].progress));
                    emit wsThread->sendMessageToClient(
                        // 专用循环状态信号收尾：全部完成时回传 100%
                        "ScheduleLoopState:" +
                        QString::number(schedule_currentNum) + ":" +
                        QString::number(schedule_RepeatNum) + ":" +
                        QString::number(schedule_RepeatNum) + ":" +
                        "100");
                    nextSchedule();
                }
                return;
            }
            
            // 计算拍摄过程中的进度
            // 拍摄步骤从步骤4开始，每张照片对应一个步骤
            // 步骤编号 = 3 + schedule_currentShootNum
            // stepProgress = 当前曝光进度（0.0-1.0）
            int currentStep = 3 + schedule_currentShootNum;
            double shotProgress = qMin(expTime_ms / (double)schedule_ExpTime, 1.0);  // 限制在0.0-1.0之间

            // 为避免曝光时间较长时频繁刷新“当前总进度”导致前端整体进度条跳动混乱，
            // 这里不再在曝光进行过程中更新 m_scheduList[schedule_currentNum].progress，
            // 仅通过 ScheduleStepState 将当前曝光的细粒度进度（0-100%）回传给前端用于单步倒计时显示。
            Logger::Log("expTime_ms:" + std::to_string(expTime_ms) + ", schedule_ExpTime:" + std::to_string(schedule_ExpTime) + ", currentShootNum:" + std::to_string(schedule_currentShootNum) + ", RepeatNum:" + std::to_string(schedule_RepeatNum) + ", currentStep:" + std::to_string(currentStep) + ", shotProgress:" + std::to_string(shotProgress), LogLevel::INFO, DeviceType::MAIN);

            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "exposure:" +
                QString::number(schedule_currentShootNum) + ":" +
                QString::number(schedule_RepeatNum) + ":" +
                QString::number(static_cast<int>(shotProgress * 100.0)));
            captureTimer.start(1000);  // 继续等待
        } });

    captureTimer.start(1000);
}

bool MainWindow::WaitForTelescopeToComplete()
{
    return (TelescopeControl_Status() != "Moving");
}

bool MainWindow::WaitForShootToComplete()
{
    Logger::Log("Wait For Shoot To Complete...", LogLevel::INFO, DeviceType::MAIN);
    return (ShootStatus != "InProgress");
}

bool MainWindow::WaitForGuidingToComplete()
{
    Logger::Log("Wait For Guiding To Complete..." + std::to_string(InGuiding), LogLevel::INFO, DeviceType::MAIN);
    return InGuiding;
}

bool MainWindow::WaitForTimeToComplete()
{
    Logger::Log("Wait For Time To Complete...", LogLevel::INFO, DeviceType::MAIN);
    QString TargetTime = m_scheduList[schedule_currentNum].shootTime;

    // 如果获取到的目标时间不是完整的时间格式，直接返回 true
    if (TargetTime.length() != 5 || TargetTime[2] != ':')
        return true;

    // 获取当前时间
    QTime currentTime = QTime::currentTime();
    // 解析目标时间
    QTime targetTime = QTime::fromString(TargetTime, "hh:mm");

    Logger::Log("currentTime:" + currentTime.toString("hh:mm").toStdString() + ", targetTime:" + targetTime.toString("hh:mm").toStdString(), LogLevel::INFO, DeviceType::MAIN);

    // 如果目标时间晚于当前时间，返回 false
    if (targetTime > currentTime)
        return false;

    // 目标时间早于或等于当前时间，返回 true
    return true;
}

int MainWindow::calculateScheduleProgress(int stepNumber, double stepProgress)
{
    // 步骤定义：
    // 步骤1：等待时间
    // 步骤2：赤道仪转动
    // 步骤3：滤镜设置
    // 步骤4到4+RepeatNum-1：拍摄（每张照片算一个步骤）
    // 总步骤数 = 3 + schedule_RepeatNum
    
    int totalSteps = 3 + schedule_RepeatNum;
    if (totalSteps <= 0)
    {
        return 100;  // 如果总步骤数为0，直接返回100%
    }
    
    // 计算每步的进度增量
    double progressPerStep = 100.0 / totalSteps;
    
    // 计算当前步骤的进度
    // stepProgress 用于步骤内的进度（0.0-1.0），例如拍摄过程中的进度
    double currentProgress = stepNumber * progressPerStep * stepProgress;
    
    // 如果超过100%，强制转换为100%
    if (currentProgress > 100.0)
    {
        currentProgress = 100.0;
    }
    
    return static_cast<int>(currentProgress);
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

void MainWindow::CaptureImageSaveAsync()
{
    QPointer<MainWindow> self(this);
    QtConcurrent::run([self]() {
        if (!self) return;
        self->CaptureImageSave();
    });
}

void MainWindow::PersistGuidingFits(const QString& sourceFitsPath)
{
    if (sourceFitsPath.isEmpty())
        return;
    if (!QFile::exists(sourceFitsPath))
        return;

    // 按需求：导星循环曝光只需更新 /dev/shm/guiding.fits（不再额外复制到 CaptureImage/<date>/guiding.fits）
    const QString guidingShmPath = QStringLiteral("/dev/shm/guiding.fits");
    const QString effectiveFitsPath = (sourceFitsPath == guidingShmPath) ? sourceFitsPath : guidingShmPath;

    // 若 INDI 返回的路径不是 /dev/shm/guiding.fits，则覆盖同步到该固定路径
    if (sourceFitsPath != guidingShmPath)
    {
        QFile dst(guidingShmPath);
        if (dst.exists())
            dst.remove();
        if (!QFile::copy(sourceFitsPath, guidingShmPath))
        {
            Logger::Log("PersistGuidingFits | copy to /dev/shm/guiding.fits failed", LogLevel::WARNING, DeviceType::GUIDER);
            return;
        }
    }

    // 同步生成前端导星 JPG（沿用既有 SaveGuiderImageSuccess 消息协议）
    cv::Mat img;
    if (Tools::readFits(effectiveFitsPath.toUtf8().constData(), img) == 0 && !img.empty())
    {
        // 内置导星也维护“当前导星图像尺寸”，用于前端 PHD2Box/Cross 覆盖层计算
        glPHD_CurrentImageSizeX = img.cols;
        glPHD_CurrentImageSizeY = img.rows;

        // 若多星副星点等待下发（例如图像尺寸尚未拿到），在真正进入 InGuiding 时补发一次
        if (guiderMultiStarSecondaryPtsPending && wsThread
            && guiderPhaseGuiding && !guiderDirectionDetectActive)
        {
            emit wsThread->sendMessageToClient("ClearPHD2MultiStars");
            for (int i = 0; i < guiderMultiStarSecondaryPtsPx.size(); ++i)
            {
                if (i >= 8) break;
                const auto& p = guiderMultiStarSecondaryPtsPx[i];
                emit wsThread->sendMessageToClient(
                    "PHD2MultiStarsPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
                    QString::number(glPHD_CurrentImageSizeY) + ":" +
                    QString::number(static_cast<int>(std::lround(p.x()))) + ":" +
                    QString::number(static_cast<int>(std::lround(p.y()))));
            }
            guiderMultiStarSecondaryPtsPending = false;
        }

        double minV = 0.0, maxV = 0.0;
        cv::minMaxLoc(img, &minV, &maxV);

        // 循环曝光每帧都会走这里：降为 DEBUG（默认关闭 DEBUG）避免刷屏
        const uint16_t depthMax = (img.depth() == CV_8U) ? 255 : 65535;
        uint16_t B = 0;
        uint16_t W = depthMax;

        if (AutoStretch)
            Tools::GetAutoStretch(img, 0, B, W);

        if (W <= B)
            W = std::min<uint16_t>(depthMax, static_cast<uint16_t>(B + 1));

        Logger::Log("PersistGuidingFits | fits=" + effectiveFitsPath.toStdString() +
                        " depth=" + std::to_string(img.depth()) +
                        " min=" + std::to_string(minV) + " max=" + std::to_string(maxV) +
                        " B=" + std::to_string(B) + " W=" + std::to_string(W),
                    LogLevel::DEBUG, DeviceType::GUIDER);

        cv::Mat img8(img.rows, img.cols, CV_8UC1, cv::Scalar(0));

        // 近似平场/饱和帧时，传统 B/W 拉伸会把整帧映射成黑色；这里保留亮度信息，避免误判为黑屏。
        if (maxV <= minV + 1.0)
        {
            const double normalized = std::clamp(maxV / std::max<double>(1.0, depthMax), 0.0, 1.0);
            const int gray = static_cast<int>(std::lround(normalized * 255.0));
            img8.setTo(cv::Scalar(gray));
            Logger::Log("PersistGuidingFits | flat-frame fallback applied, gray=" + std::to_string(gray),
                        LogLevel::INFO, DeviceType::GUIDER);
        }
        else
        {
            Tools::Bit16To8_Stretch(img, img8, B, W);

            double min8 = 0.0, max8 = 0.0;
            cv::minMaxLoc(img8, &min8, &max8);
            if (max8 <= 0.0 && maxV > 0.0)
            {
                B = 0;
                W = static_cast<uint16_t>(std::clamp(maxV, 1.0, static_cast<double>(depthMax)));
                Tools::Bit16To8_Stretch(img, img8, B, W);
                Logger::Log("PersistGuidingFits | fallback restretch applied, B=" +
                                std::to_string(B) + " W=" + std::to_string(W),
                            LogLevel::INFO, DeviceType::GUIDER);
            }
        }

        cv::Mat guiderPreviewBgr;
        cv::cvtColor(img8, guiderPreviewBgr, cv::COLOR_GRAY2BGR);

        saveGuiderImageAsJPG(guiderPreviewBgr);
    }
}

void MainWindow::PersistGuidingPreviewFromFrame(const QString& sourceFitsPath, const cv::Mat& image16)
{
    if (sourceFitsPath.isEmpty() || image16.empty())
        return;

    const QString guidingShmPath = QStringLiteral("/dev/shm/guiding.fits");
    if (sourceFitsPath != guidingShmPath && QFile::exists(sourceFitsPath))
    {
        QFile dst(guidingShmPath);
        if (dst.exists())
            dst.remove();
        if (!QFile::copy(sourceFitsPath, guidingShmPath))
        {
            Logger::Log("PersistGuidingPreviewFromFrame | copy to /dev/shm/guiding.fits failed",
                        LogLevel::WARNING, DeviceType::GUIDER);
        }
    }

    const cv::Mat img = image16;
    glPHD_CurrentImageSizeX = img.cols;
    glPHD_CurrentImageSizeY = img.rows;

    if (guiderMultiStarSecondaryPtsPending && wsThread
        && guiderPhaseGuiding && !guiderDirectionDetectActive)
    {
        emit wsThread->sendMessageToClient("ClearPHD2MultiStars");
        for (int i = 0; i < guiderMultiStarSecondaryPtsPx.size(); ++i)
        {
            if (i >= 8) break;
            const auto& p = guiderMultiStarSecondaryPtsPx[i];
            emit wsThread->sendMessageToClient(
                "PHD2MultiStarsPosition:" + QString::number(glPHD_CurrentImageSizeX) + ":" +
                QString::number(glPHD_CurrentImageSizeY) + ":" +
                QString::number(static_cast<int>(std::lround(p.x()))) + ":" +
                QString::number(static_cast<int>(std::lround(p.y()))));
        }
        guiderMultiStarSecondaryPtsPending = false;
    }

    double minV = 0.0, maxV = 0.0;
    cv::minMaxLoc(img, &minV, &maxV);

    const uint16_t depthMax = (img.depth() == CV_8U) ? 255 : 65535;
    uint16_t B = 0;
    uint16_t W = depthMax;

    if (AutoStretch)
        Tools::GetAutoStretch(img, 0, B, W);

    if (W <= B)
        W = std::min<uint16_t>(depthMax, static_cast<uint16_t>(B + 1));

    Logger::Log("PersistGuidingPreviewFromFrame | frameDepth=" + std::to_string(img.depth()) +
                    " min=" + std::to_string(minV) + " max=" + std::to_string(maxV) +
                    " B=" + std::to_string(B) + " W=" + std::to_string(W),
                LogLevel::DEBUG, DeviceType::GUIDER);

    cv::Mat img8(img.rows, img.cols, CV_8UC1, cv::Scalar(0));
    if (maxV <= minV + 1.0)
    {
        const double normalized = std::clamp(maxV / std::max<double>(1.0, depthMax), 0.0, 1.0);
        const int gray = static_cast<int>(std::lround(normalized * 255.0));
        img8.setTo(cv::Scalar(gray));
        Logger::Log("PersistGuidingPreviewFromFrame | flat-frame fallback applied, gray=" + std::to_string(gray),
                    LogLevel::INFO, DeviceType::GUIDER);
    }
    else
    {
        Tools::Bit16To8_Stretch(img, img8, B, W);

        double min8 = 0.0, max8 = 0.0;
        cv::minMaxLoc(img8, &min8, &max8);
        if (max8 <= 0.0 && maxV > 0.0)
        {
            B = 0;
            W = static_cast<uint16_t>(std::clamp(maxV, 1.0, static_cast<double>(depthMax)));
            Tools::Bit16To8_Stretch(img, img8, B, W);
            Logger::Log("PersistGuidingPreviewFromFrame | fallback restretch applied, B=" +
                            std::to_string(B) + " W=" + std::to_string(W),
                        LogLevel::INFO, DeviceType::GUIDER);
        }
    }

    cv::Mat guiderPreviewBgr;
    cv::cvtColor(img8, guiderPreviewBgr, cv::COLOR_GRAY2BGR);

    saveGuiderImageAsJPG(guiderPreviewBgr);
}

void MainWindow::clearGuiderDebugAnnotations(bool refreshPreview)
{
    m_debugStarDedupCandidates.clear();
    m_debugStarSnrCandidates.clear();
    m_debugStarCandidates.clear();
    m_debugStarCandidateLabels.clear();
    m_debugStarSelected = QPointF(0, 0);
    emit wsThread->sendMessageToClient("ClearGuiderDebugCandidates");

    if (!refreshPreview)
        return;

    const QString guidingShmPath = QStringLiteral("/dev/shm/guiding.fits");
    if (QFile::exists(guidingShmPath))
        PersistGuidingFits(guidingShmPath);
}

void MainWindow::startGuiderAutoBatchCapture()
{
    const QString baseRoot = !ImageSaveBaseDirectory.isEmpty()
        ? ImageSaveBaseDirectory
        : QString::fromStdString(ImageSaveBasePath);
    const QString diagnosticsRoot = QDir(baseRoot).filePath(QStringLiteral("GuiderDiagnostics"));
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss_zzz"));
    const QString batchName = QStringLiteral("batch_%1").arg(stamp);
    const QString batchDir = QDir(diagnosticsRoot).filePath(batchName);

    if (!QDir().mkpath(batchDir))
    {
        Logger::Log("GuiderDiagnostics | failed to create batch dir: " + batchDir.toStdString(),
                    LogLevel::WARNING, DeviceType::GUIDER);
        m_guiderAutoBatchActive = false;
        m_guiderAutoBatchSavedFrames = 0;
        m_guiderAutoBatchDir.clear();
        return;
    }

    m_guiderAutoBatchActive = true;
    m_guiderAutoBatchSavedFrames = 0;
    m_guiderAutoBatchDir = batchDir;
    Logger::Log("GuiderDiagnostics | auto batch capture started: " + batchDir.toStdString(),
                LogLevel::INFO, DeviceType::GUIDER);
}

void MainWindow::stopGuiderAutoBatchCapture()
{
    if (!m_guiderAutoBatchActive && m_guiderAutoBatchDir.isEmpty())
        return;

    Logger::Log("GuiderDiagnostics | auto batch capture stopped: dir=" +
                    m_guiderAutoBatchDir.toStdString() +
                    " savedFrames=" + std::to_string(m_guiderAutoBatchSavedFrames),
                LogLevel::INFO, DeviceType::GUIDER);
    m_guiderAutoBatchActive = false;
    m_guiderAutoBatchSavedFrames = 0;
    m_guiderAutoBatchDir.clear();
}

void MainWindow::persistGuiderAutoBatchFrame(const QString& fitsPath)
{
    if (!m_guiderAutoBatchActive || m_guiderAutoBatchSavedFrames >= 20)
        return;
    if (fitsPath.isEmpty() || !QFile::exists(fitsPath) || m_guiderAutoBatchDir.isEmpty())
        return;

    const int nextIndex = m_guiderAutoBatchSavedFrames + 1;
    const QString fileName = QStringLiteral("frame_%1.fits").arg(nextIndex, 2, 10, QLatin1Char('0'));
    const QString destinationPath = QDir(m_guiderAutoBatchDir).filePath(fileName);

    QFile::remove(destinationPath);
    if (!QFile::copy(fitsPath, destinationPath))
    {
        Logger::Log("GuiderDiagnostics | failed to save frame to batch: " + destinationPath.toStdString(),
                    LogLevel::WARNING, DeviceType::GUIDER);
        return;
    }

    m_guiderAutoBatchSavedFrames = nextIndex;
    Logger::Log("GuiderDiagnostics | saved frame " + std::to_string(nextIndex) +
                    "/20 to " + destinationPath.toStdString(),
                LogLevel::INFO, DeviceType::GUIDER);

    if (m_guiderAutoBatchSavedFrames >= 20)
    {
        Logger::Log("GuiderDiagnostics | batch reached 20 frames, stop collecting: " +
                        m_guiderAutoBatchDir.toStdString(),
                    LogLevel::INFO, DeviceType::GUIDER);
        m_guiderAutoBatchActive = false;
    }
}

int MainWindow::ScheduleImageSave(QString name, int num)
{
    const QString sourcePath = latestMainCaptureFitsPath();

    if (sourcePath.isEmpty())
    {
        emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Null");
        return 1;
    }

    name.replace(' ', '_');
    
    std::time_t currentTime = std::time(nullptr);
    std::tm *timeInfo = std::localtime(&currentTime);
    char buffer[80];
    std::strftime(buffer, 80, "%Y-%m-%d", timeInfo); // Format: YYYY-MM-DD

    // 指定目标目录
    QString destinationDirectory = ImageSaveBaseDirectory + "/ScheduleImage";
    
    // 构建目录路径（不含文件名）
    QString dirPath = destinationDirectory + "/" + QString(buffer) + " " + QTime::currentTime().toString("hh") + "h (" + ScheduleTargetNames + ")";
    
    // 判断是否为U盘路径（使用saveMode参数）
    bool isUSBSave = (saveMode != "local");
    
    // 使用通用函数检查存储空间并创建目录
    QString dirPathToCreate = isUSBSave ? dirPath : QString();
    int checkResult = checkStorageSpaceAndCreateDirectory(
        sourcePath,
        destinationDirectory,
        dirPathToCreate,
        "ScheduleImageSave",
        isUSBSave,
        [this]() { createScheduleDirectory(); }
    );
    if (checkResult != 0)
    {
        return checkResult;
    }

    // 检查文件是否存在，如果存在则自动递增序号
    int actualNum = num;
    QString resultFileName;
    QString destinationPath;
    
    // 尝试找到不存在的文件名（最多尝试1000次，避免无限循环）
    int maxAttempts = 1000;
    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        resultFileName = QString("%1-%2.fits").arg(name).arg(actualNum);
        destinationPath = dirPath + "/" + resultFileName;
        
        // 检查文件是否存在
        if (!QFile::exists(destinationPath))
        {
            // 文件不存在，可以使用这个文件名
            break;
        }
        
        // 文件已存在，递增序号
        actualNum++;
        
        if (attempt == 0)
        {
            // 第一次发现文件存在时记录日志
            Logger::Log("ScheduleImageSave | File already exists, incrementing sequence number: " + 
                       (dirPath + "/" + QString("%1-%2.fits").arg(name).arg(num)).toStdString() + 
                       " -> trying next number", 
                       LogLevel::INFO, DeviceType::MAIN);
        }
    }
    
    if (actualNum != num)
    {
        Logger::Log("ScheduleImageSave | Using sequence number " + QString::number(actualNum).toStdString() + 
                   " instead of " + QString::number(num).toStdString() + " to avoid overwriting", 
                   LogLevel::INFO, DeviceType::MAIN);
    }

    // 使用通用函数保存文件
    int saveResult = saveImageFile(sourcePath, destinationPath, "ScheduleImageSave", isUSBSave);
    if (saveResult != 0)
    {
        return saveResult;
    }
    
    Logger::Log("ScheduleImageSave | File saved successfully: " + destinationPath.toStdString(), LogLevel::INFO, DeviceType::MAIN);
    emit wsThread->sendMessageToClient("CaptureImageSaveStatus:Success");
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

bool MainWindow::createScheduleDirectory()
{
    Logger::Log("createScheduleDirectory start ...", LogLevel::INFO, DeviceType::MAIN);
    std::string basePath = ImageSaveBasePath + "/ScheduleImage";

    std::time_t currentTime = std::time(nullptr);
    std::tm *timeInfo = std::localtime(&currentTime);
    char buffer[80];
    std::strftime(buffer, 80, "%Y-%m-%d", timeInfo); // Format: YYYY-MM-DD
    std::string folderName = basePath + "/" + buffer + " " + QTime::currentTime().toString("hh").toStdString() + "h (" + ScheduleTargetNames.toStdString() + ")";

    // 如果目录不存在，则创建（使用 create_directories 创建多层目录）
    if (!std::filesystem::exists(folderName))
    {
        if (std::filesystem::create_directories(folderName))
        {
            Logger::Log("createScheduleDirectory | Folder created successfully: " + std::string(folderName), LogLevel::INFO, DeviceType::MAIN);
        }
        else
        {
            Logger::Log("createScheduleDirectory | An error occurred while creating the folder.", LogLevel::ERROR, DeviceType::MAIN);
            return false;
        }
    }
    else
    {
        Logger::Log("createScheduleDirectory | The folder already exists: " + std::string(folderName), LogLevel::INFO, DeviceType::MAIN);
    }
    return true;
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

void MainWindow::focusLoopShooting(bool isLoop)
{
    if (isLoop)
    {
        isFocusLoopShooting = true;
        if (glMainCameraStatu == "Exposuring")
        {
            emit wsThread->sendMessageToClient("startFocusLoopFailed:Wait Take Picture Finish!");
            isFocusLoopShooting = false;
            return;
        }
        if (!isMainCameraConnected())
        {
            emit wsThread->sendMessageToClient("startFocusLoopFailed:MainCamera is not connected!");
            isFocusLoopShooting = false;
            return;
        }
        FocusingLooping();
    }
    else
    {
        isFocusLoopShooting = false;
        hasPendingRoiUpdate = false;

        // 停止聚焦循环标志（避免前端继续认为在循环）
        glIsFocusingLooping = false;

        // 判断主相机是 SDK 模式还是 INDI 模式
        bool isMainCameraSDK = (systemdevicelist.system_devices.size() > 20 &&
                                systemdevicelist.system_devices[20].isSDKConnect &&
                                sdkMainCameraHandle != nullptr);

        if (isMainCameraSDK)
        {
            // SDK 模式：先取消当前曝光，再停止定时器，最后重置标志
            // 1. 取消正在进行的曝光/读出（避免残留帧干扰）
            if (sdkMainCameraHandle != nullptr)
            {
                SdkCommand cancelCmd;
                cancelCmd.type = SdkCommandType::Custom;
                cancelCmd.name = "CancelExposure";
                cancelCmd.payload = std::any();
                // 直接通过设备句柄调用，无需指定驱动名称
                SdkResult cancelRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, cancelCmd);
                if (!cancelRes.success) {
                    Logger::Log("focusLoopShooting | CancelExposure failed: " + cancelRes.message,
                                LogLevel::WARNING, DeviceType::FOCUSER);
                }
            }

            // 2. 停止曝光轮询定时器（必须在取消曝光后，避免定时器继续触发）
            if (sdkExposureTimer) {
                sdkExposureTimer->stop();
            }

            // 3. 重置 ROI 标志（必须在停止定时器后，避免新的定时器事件捕获到 false）
            sdkExposureIsROI = false;

            // 4. 恢复全幅分辨率，避免退出 ROI 后下一次全幅拍摄仍然是 ROI 尺寸
            SdkAreaInfo full;
            full.startX = 0;
            full.startY = 0;
            full.sizeX  = glMainCCDSizeX;
            full.sizeY  = glMainCCDSizeY;
            SdkCommand setRoiCmd;
            setRoiCmd.type = SdkCommandType::Custom;
            setRoiCmd.name = "SetResolution";
            setRoiCmd.payload = full;
            // 直接通过设备句柄调用，无需指定驱动名称
            SdkResult roiRes = SdkManager::instance().callByHandle(sdkMainCameraHandle, setRoiCmd);
            if (!roiRes.success) {
                Logger::Log("focusLoopShooting | SDK restore full resolution failed: " + roiRes.message,
                            LogLevel::WARNING, DeviceType::FOCUSER);
            }

            // 5. 重置相机状态
            if (glMainCameraStatu == "Exposuring") {
                glMainCameraStatu = "IDLE";
            }
        }
        else
        {
            // INDI 模式：停止定时器和重置标志
            if (sdkExposureTimer) {
                sdkExposureTimer->stop();
            }
            sdkExposureIsROI = false;

            if (glMainCameraStatu == "Exposuring" && isMainCameraConnected())
            {
                abortMainCameraCapture();
            }
        }
    }
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


void MainWindow::saveFitsAsJPG(QString filename, bool ProcessBin)
{
    // 创建MainCameraCFA的局部副本，防止多线程竞态条件导致的值污染
    QString localCameraCFA = MainCameraCFA;
    QString roiCameraCFA = localCameraCFA;
    
    // 验证CFA值的合法性
    QStringList validCFAValues = {"RGGB", "BGGR", "GRBG", "GBRG", "RG", "BG", "GR", "GB", "", "null"};
    if (!validCFAValues.contains(localCameraCFA))
    {
        Logger::Log("saveFitsAsJPG | Invalid MainCameraCFA value detected: '" + localCameraCFA.toStdString() + 
                   "'. Using empty (Mono mode) for this operation.", LogLevel::ERROR, DeviceType::CAMERA);
        localCameraCFA = "";  // 使用单色相机模式
    }
    
    cv::Mat image;
    // 读取FITS文件
    Tools::readFits(filename.toLocal8Bit().constData(), image);
    if (image.empty())
    {
        Logger::Log("saveFitsAsJPG | readFits succeeded but image is empty: " + filename.toStdString(), LogLevel::ERROR, DeviceType::CAMERA);
        return;
    }

    Logger::Log("saveFitsAsJPG | input FITS filename=" + filename.toStdString() +
                    ", raw image=" + std::to_string(image.cols) + "x" + std::to_string(image.rows),
                LogLevel::INFO, DeviceType::FOCUSER);

    QList<FITSImage::Star> stars;
    if (roiUseSelfCalcParams)
    {
        stars = Tools::FindStarsByFocusedCppFromFile(filename, true, true);
        Logger::Log("saveFitsAsJPG | ROI star detection uses ROI self-calculated params, source=" +
                        filename.toStdString(),
                    LogLevel::INFO, DeviceType::FOCUSER);
    }
    else
    {
        stars = Tools::FindStarsByFocusedCpp(true, true);
        Logger::Log("saveFitsAsJPG | ROI star detection reuses full-frame params/source, current ROI frame=" +
                        filename.toStdString(),
                    LogLevel::INFO, DeviceType::FOCUSER);
    }
    currentSelectStarPosition = selectStar(stars);

    emit wsThread->sendMessageToClient("FocusMoveDone:" + QString::number(FocuserControl_getPosition()) + ":" + QString::number(roiAndFocuserInfo["SelectStarHFR"]));
    emit wsThread->sendMessageToClient("setSelectStarPosition:" + QString::number(roiAndFocuserInfo["SelectStarX"]) + ":" + QString::number(roiAndFocuserInfo["SelectStarY"]) + ":" + QString::number(roiAndFocuserInfo["SelectStarHFR"]) + ":" + QString::number(roiAndFocuserInfo["SelectStarSNR"]) + ":" + QString::number(roiAndFocuserInfo["SelectStarLocalMax"]) + ":" + QString::number(roiAndFocuserInfo["SelectStarBgStd"]));
    emit wsThread->sendMessageToClient("addFwhmNow:" + QString::number(roiAndFocuserInfo["SelectStarHFR"]));
    emit wsThread->sendMessageToClient("addSnrNow:" + QString::number(roiAndFocuserInfo["SelectStarSNR"]));
    Logger::Log("saveFitsAsJPG | 星点位置更新为 x:" + std::to_string(roiAndFocuserInfo["SelectStarX"]) + ",y:" + std::to_string(roiAndFocuserInfo["SelectStarY"]) + ",HFR:" + std::to_string(roiAndFocuserInfo["SelectStarHFR"]) + ",SNR:" + std::to_string(roiAndFocuserInfo["SelectStarSNR"]) + ",localMax:" + std::to_string(roiAndFocuserInfo["SelectStarLocalMax"]) + ",bgStd:" + std::to_string(roiAndFocuserInfo["SelectStarBgStd"]), LogLevel::INFO, DeviceType::FOCUSER);

    cv::Mat originalImage16;
    if (image.type() == CV_8UC1 || image.type() == CV_8UC3 || image.type() == CV_16UC1)
    {
        originalImage16 = Tools::convert8UTo16U_BayerSafe(image, false);
        Logger::Log("saveFitsAsJPG | image size:" + std::to_string(image.cols) + "x" + std::to_string(image.rows), LogLevel::INFO, DeviceType::FOCUSER);
        image.release();
    }
    else
    {
        Logger::Log("The current image data type is not supported for processing.", LogLevel::WARNING, DeviceType::CAMERA);
        image.release();
        originalImage16.release();
        return;
    }
    if (originalImage16.empty())
    {
        Logger::Log("saveFitsAsJPG | convert8UTo16U_BayerSafe returned empty image; skip medianBlur", LogLevel::ERROR, DeviceType::CAMERA);
        return;
    }
    Logger::Log("saveFitsAsJPG | image16 size:" + std::to_string(originalImage16.cols) + "x" + std::to_string(originalImage16.rows), LogLevel::INFO, DeviceType::FOCUSER);

    // 最小验证：ROI 导出链路暂时不对 Bayer RAW 做 medianBlur。
    // 若颜色恢复正常，可基本确认“RAW 上的滤波破坏 CFA 采样结构”就是偏色根因。
    Logger::Log("saveFitsAsJPG | median blur skipped for ROI Bayer validation", LogLevel::WARNING, DeviceType::CAMERA);

    // 下发给前端的 ROI .bin：不做软件合并，与相机 ROI 读出尺寸一致（与前端红框/瓦片传感器坐标对齐）。
    (void)ProcessBin;
    cv::Mat image16 = originalImage16.clone();
    // 部分驱动/SDK 在 ROI 模式下仍写出全幅 FITS；前端按 RedBoxSideLength² 解码并与 SaveJpgSuccess 的传感器原点对齐。
    // 若缓冲区大于本次曝光 ROI 尺寸，则按 FocusingLooping 记录的快照从全幅中裁出与 .bin 语义一致的子图。
    if (lastFocusExposureSnapshotValid && lastFocusExposureRoiW > 0 && lastFocusExposureRoiH > 0 && !image16.empty()
        && image16.type() == CV_16UC1)
    {
        const int cw = lastFocusExposureRoiW;
        const int ch = lastFocusExposureRoiH;
        const int sx = lastFocusExposureEffMinX + lastFocusExposureScaledX;
        const int sy = lastFocusExposureEffMinY + lastFocusExposureScaledY;
        roiCameraCFA = resolveFrameCfa(sx, sy);
        if (image16.cols == cw && image16.rows == ch)
        {
            // 已是 ROI 子帧（像素 (0,0) 即 ROI 左上角）
        }
        else if (sx >= 0 && sy >= 0 && image16.cols >= sx + cw && image16.rows >= sy + ch)
        {
            const cv::Rect patch(sx, sy, cw, ch);
            image16 = image16(patch).clone();
            Logger::Log("saveFitsAsJPG | crop full buffer to ROI " + std::to_string(cw) + "x" + std::to_string(ch)
                            + " at (" + std::to_string(sx) + "," + std::to_string(sy) + ")",
                        LogLevel::DEBUG, DeviceType::FOCUSER);
        }
        else if (image16.cols > cw || image16.rows > ch)
        {
            const cv::Rect want(sx, sy, cw, ch);
            const cv::Rect bounds(0, 0, image16.cols, image16.rows);
            const cv::Rect inter = want & bounds;
            if (inter.width > 0 && inter.height > 0)
            {
                image16 = image16(inter).clone();
                Logger::Log("saveFitsAsJPG | cropped full-frame buffer (clamped) to " + std::to_string(image16.cols) + "x" + std::to_string(image16.rows),
                            LogLevel::WARNING, DeviceType::FOCUSER);
            }
        }
    }
    roiCameraCFA = normalizeCfaPattern(roiCameraCFA);
    if (roiCameraCFA.isEmpty() && !localCameraCFA.isEmpty()) {
        roiCameraCFA = normalizeCfaPattern(localCameraCFA);
    }
    Logger::Log("saveFitsAsJPG | ROI CFA base=" + localCameraCFA.toStdString() +
                    " resolved=" + roiCameraCFA.toStdString() +
                    " cfaOffset=(" + std::to_string(MainCameraCFAOffsetX) + "," +
                    std::to_string(MainCameraCFAOffsetY) + ")",
                LogLevel::INFO, DeviceType::FOCUSER);
    if (lastFocusExposureSnapshotValid)
    {
        const int snapSensorX = lastFocusExposureEffMinX + lastFocusExposureScaledX;
        const int snapSensorY = lastFocusExposureEffMinY + lastFocusExposureScaledY;
        Logger::Log("saveFitsAsJPG | ROI Bayer debug | " +
                        formatBayerPhaseDebug(localCameraCFA, MainCameraCFAOffsetX, MainCameraCFAOffsetY,
                                             snapSensorX, snapSensorY, roiCameraCFA) +
                        ", snapshotScaled=(" + std::to_string(lastFocusExposureScaledX) + "," + std::to_string(lastFocusExposureScaledY) + ")" +
                        ", snapshotEffMin=(" + std::to_string(lastFocusExposureEffMinX) + "," + std::to_string(lastFocusExposureEffMinY) + ")" +
                        ", snapshotRoiSize=" + std::to_string(lastFocusExposureRoiW) + "x" + std::to_string(lastFocusExposureRoiH) +
                        ", outputImage=" + std::to_string(image16.cols) + "x" + std::to_string(image16.rows) +
                        ", " + sampleBayer2x2Debug(image16),
                    LogLevel::INFO, DeviceType::FOCUSER);
    }
    Logger::Log("saveFitsAsJPG | output image16 " + std::to_string(image16.cols) + "x" + std::to_string(image16.rows), LogLevel::DEBUG, DeviceType::FOCUSER);
    originalImage16.release();

    // ROI 循环频率可能高于 1Hz：若文件名只精确到秒，会在同一秒内反复覆盖同名文件，
    // 造成前端拉取到旧内容/404（尤其在前端处理变慢、跳帧时）。这里改为毫秒级并追加序号，保证全局唯一。
    static std::atomic_uint64_t roiFileSeq{0};
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const uint64_t seq = roiFileSeq.fetch_add(1, std::memory_order_relaxed);
    std::string fileName = "focuserPicture_" + std::to_string(nowMs) + "_" + std::to_string(seq) + ".bin";
    std::string filePath = vueDirectoryPath + fileName;
    std::ofstream outFile(filePath, std::ios::binary);

    // 检查文件是否成功打开
    if (!outFile)
    {
        Logger::Log("Failed to open file: " + filePath, LogLevel::WARNING, DeviceType::FOCUSER);
        if (isFocusLoopShooting)
        {
            FocusingLooping();
        }
        return;
    }

    // 将图像数据写入到文件中
    outFile.write(reinterpret_cast<const char *>(image16.data), image16.total() * image16.elemSize());

    // 检查是否成功写入
    if (!outFile)
    {
        Logger::Log("Failed to write to file: " + filePath, LogLevel::ERROR, DeviceType::FOCUSER);
        if (isFocusLoopShooting)
        {
            FocusingLooping();
        }
        return;
    }

    outFile.close();

    bool saved = true;

    // 创建/更新本次 ROI 对应的符号链接（供前端通过 /img/ 访问）
    std::string Command = "ln -sf " + filePath + " " + vueImagePath + fileName;
    system(Command.c_str());
    Logger::Log("Symbolic link created for new image file.", LogLevel::DEBUG, DeviceType::FOCUSER);

    if (saved)
    {
        // SaveJpgSuccess 中的 ROI 必须与「本帧 .bin 对应的曝光」一致：用 FocusingLooping 里在真正下发曝光前记录的快照。
        // 若先应用 hasPendingRoiUpdate / 或用户已 sendRedBoxState 再发 SaveJpgSuccess，会把下一帧坐标与当前帧像素绑在一起，造成 ROI 叠加与底图错位。
        double emitRoiX = 0.0;
        double emitRoiY = 0.0;
        if (lastFocusExposureSnapshotValid)
        {
            const int snapScale = std::max(1, lastFocusExposureRoiCoordScale);
            emitRoiX = static_cast<double>(lastFocusExposureScaledX) / static_cast<double>(snapScale);
            emitRoiY = static_cast<double>(lastFocusExposureScaledY) / static_cast<double>(snapScale);
        }
        else
        {
            emitRoiX = roiAndFocuserInfo.count("ROI_x") ? roiAndFocuserInfo["ROI_x"] : 0.0;
            emitRoiY = roiAndFocuserInfo.count("ROI_y") ? roiAndFocuserInfo["ROI_y"] : 0.0;
        }

        // 与前端 parseFloat 一致，保留小数（非瓦片 bin 缩放下 emit 可能为小数）
        Logger::Log("saveFitsAsJPG | ROI frame mapping file=" + fileName +
                        ", emitRoi=(" + std::to_string(emitRoiX) + "," + std::to_string(emitRoiY) + ")" +
                        ", image16=" + std::to_string(image16.cols) + "x" + std::to_string(image16.rows) +
                        ", roiCFA=" + roiCameraCFA.toStdString(),
                    LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("SaveJpgSuccess:" + QString::fromStdString(fileName) + ":" +
                                           QString::number(emitRoiX, 'g', 9) + ":" +
                                           QString::number(emitRoiY, 'g', 9) + ":" +
                                           (roiCameraCFA.isEmpty() ? QStringLiteral("null") : roiCameraCFA));

        // 挂起的 ROI 居中更新：在本帧图像已发出后再改 roiAndFocuserInfo，并单独通知前端（与 SaveJpgSuccess 解耦）
        if (hasPendingRoiUpdate)
        {
            hasPendingRoiUpdate = false;
            int boxSideToSend = BoxSideLength;
            if (roiAndFocuserInfo.count("BoxSideLength"))
                boxSideToSend = static_cast<int>(std::lround(roiAndFocuserInfo["BoxSideLength"]));
            const int maxX = std::max(0, glMainCCDSizeX - boxSideToSend);
            const int maxY = std::max(0, glMainCCDSizeY - boxSideToSend);
            int applyX = std::min(std::max(0, pendingRoiX), maxX);
            int applyY = std::min(std::max(0, pendingRoiY), maxY);
            if (applyX % 2 != 0) applyX += (applyX < maxX ? 1 : -1);
            if (applyY % 2 != 0) applyY += (applyY < maxY ? 1 : -1);
            applyX = std::min(std::max(0, applyX), maxX);
            applyY = std::min(std::max(0, applyY), maxY);
            // pendingRoiX/Y 已为传感器像素（见 selectStar 全图坐标）；瓦片模式下勿再除以 previewBinning。
            const bool tileModeActive = (isStagingImage && !SavedImage.empty());
            const int coordScale = tileModeActive ? 1 : std::max(1, glMainCameraBinning);
            const QPointF snapped = snapRoiOriginToBayerSafePhase(static_cast<double>(applyX) / coordScale,
                                                                  static_cast<double>(applyY) / coordScale,
                                                                  boxSideToSend, boxSideToSend);
            roiAndFocuserInfo["ROI_x"] = snapped.x();
            roiAndFocuserInfo["ROI_y"] = snapped.y();
            // 勿在此处 emit SetRedBoxState：本帧 SaveJpgSuccess 已带「当前曝光」ROI；若再发「下一帧居中」坐标，前端会在叠加层仍为当前帧像素时把红框/选星圆改到新 ROI，造成错位。下一帧 SaveJpgSuccess 会携带新快照坐标并同步 UI。sendRoiInfo() 仍会发 SetRedBoxState 供重连等场景。
        }

        Logger::Log("SaveJpgSuccess:" + fileName + " to " + filePath + ",image size:" + std::to_string(image16.cols) + "x" + std::to_string(image16.rows), LogLevel::DEBUG, DeviceType::FOCUSER);

        // 清理旧 ROI 文件/链接：保留最近 N 个，避免前端处理变慢/跳帧时出现 404 或拿不到对应帧
        constexpr size_t kKeepRecentRoiFrames = 100;
        auto cleanupRoiArtifacts = [](const std::string& dirStr, bool wantSymlink) {
            try {
                const fs::path dirPath(dirStr);
                if (!fs::exists(dirPath))
                    return;

                auto hasPrefix = [](const std::string& s, const std::string& p) -> bool {
                    return s.rfind(p, 0) == 0;
                };
                auto hasSuffix = [](const std::string& s, const std::string& suf) -> bool {
                    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
                };

                struct EntryInfo {
                    fs::path path;
                    bool timeOk = false;
                    fs::file_time_type t;
                };

                std::vector<EntryInfo> items;
                items.reserve(256);

                for (const auto& entry : fs::directory_iterator(dirPath)) {
                    const std::string name = entry.path().filename().string();
                    if (!hasPrefix(name, "focuserPicture_") || !hasSuffix(name, ".bin"))
                        continue;

                    const bool isLink = fs::is_symlink(entry.symlink_status());
                    const bool isFile = fs::is_regular_file(entry.status());
                    if (wantSymlink) {
                        if (!isLink)
                            continue;
                    } else {
                        if (!isFile)
                            continue;
                    }

                    EntryInfo info;
                    info.path = entry.path();
                    try {
                        info.t = fs::last_write_time(entry.path());
                        info.timeOk = true;
                    } catch (...) {
                        info.timeOk = false; // 可能是断链等
                    }
                    items.push_back(std::move(info));
                }

                std::sort(items.begin(), items.end(), [](const EntryInfo& a, const EntryInfo& b) {
                    if (a.timeOk != b.timeOk)
                        return a.timeOk; // timeOk=true 排在前面
                    if (!a.timeOk)
                        return a.path.string() < b.path.string();
                    return a.t > b.t; // 新的在前
                });

                // timeOk=false 的条目优先删除（通常是断链），其余超出保留数的删除
                size_t kept = 0;
                for (const auto& it : items) {
                    const bool shouldKeep = it.timeOk && kept < kKeepRecentRoiFrames;
                    if (shouldKeep) {
                        kept++;
                        continue;
                    }
                    std::error_code ec;
                    fs::remove(it.path, ec);
                }
            } catch (...) {
                // 清理失败不影响主流程
            }
        };

        cleanupRoiArtifacts(vueDirectoryPath, false);
        cleanupRoiArtifacts(vueImagePath, true);
        
    }
    else
    {
        Logger::Log("Failed to save image.", LogLevel::ERROR, DeviceType::GUIDER);
    }
    // 释放最终的图像内存
    if (isAutoFocus)
    {
        autoFocus->setCaptureComplete(filename);
    }
    image16.release();
    focusLoopShooting(isFocusLoopShooting); 

}

QPointF MainWindow::selectStar(QList<FITSImage::Star> stars){
    // 1) 边界与输入检查
    if (stars.size() <= 0) {
        Logger::Log("selectStar | no stars", LogLevel::INFO, DeviceType::FOCUSER);
        roiAndFocuserInfo["SelectStarHFR"] = 0.0;
        roiAndFocuserInfo["SelectStarSNR"] = 0.0;
        roiAndFocuserInfo["SelectStarLocalMax"] = 0.0;
        roiAndFocuserInfo["SelectStarBgStd"] = 0.0;
        return QPointF(CurrentPosition, 0);
    }

    // 2) 读取 ROI 与选择点（全图坐标）
    const double boxSide = roiAndFocuserInfo.count("BoxSideLength") ? roiAndFocuserInfo["BoxSideLength"] : BoxSideLength;
    const bool tileModeActive = (isStagingImage && !SavedImage.empty());
    const int roiCoordScale = tileModeActive ? 1 : std::max(1, glMainCameraBinning);
    const double roi_x    = roiAndFocuserInfo.count("ROI_x") ? roiAndFocuserInfo["ROI_x"] * roiCoordScale : 0;
    const double roi_y    = roiAndFocuserInfo.count("ROI_y") ? roiAndFocuserInfo["ROI_y"] * roiCoordScale : 0;
    const double selXFull = roiAndFocuserInfo.count("SelectStarX") ? roiAndFocuserInfo["SelectStarX"] : -1;
    const double selYFull = roiAndFocuserInfo.count("SelectStarY") ? roiAndFocuserInfo["SelectStarY"] : -1;
    Logger::Log("selectStar | inputs stars=" + std::to_string(stars.size()) +
                    ", boxSide=" + std::to_string(boxSide) +
                    ", roi=(" + std::to_string(roi_x) + "," + std::to_string(roi_y) + ")" +
                    ", prevSelect=(" + std::to_string(selXFull) + "," + std::to_string(selYFull) + ")" +
                    ", roiCoordScale=" + std::to_string(roiCoordScale),
                LogLevel::INFO, DeviceType::FOCUSER);

    // 3) 若已锁定目标星，则优先在本帧中追踪最近的那颗
    const int edgeMargin = 5;
    if (selectedStarLocked && lockedStarFull.x() >= 0 && lockedStarFull.y() >= 0)
    {
        // 防抖：优先在粘滞半径内寻找最近星；否则再全局最近
        int stickIdx = -1; double stickDist2 = std::numeric_limits<double>::max();
        int nearIdx  = -1; double nearDist2  = std::numeric_limits<double>::max();

        for (int i = 0; i < stars.size(); ++i) {
            const auto &s = stars[i];
            if (s.HFR <= 0) continue;

            // 排除 ROI 边缘星点（ROI 相对坐标检查）
            if (s.x <= edgeMargin || s.y <= edgeMargin || 
                s.x >= boxSide - edgeMargin || s.y >= boxSide - edgeMargin) {
                continue;
            }

            // ROI 相对坐标 -> 全图坐标
            const double sxFull = roi_x + s.x;
            const double syFull = roi_y + s.y;
            const double dx = sxFull - lockedStarFull.x();
            const double dy = syFull - lockedStarFull.y();
            const double d2 = dx*dx + dy*dy;

            if (d2 < nearDist2) { nearDist2 = d2; nearIdx = i; }
            if (d2 <= (starStickRadiusPx * starStickRadiusPx) && d2 < stickDist2) {
                stickDist2 = d2; stickIdx = i;
            }
        }

        const int bestIdx = (stickIdx != -1 ? stickIdx : nearIdx);
        if (bestIdx != -1)
        {
            const auto &best = stars[bestIdx];
            const double bestXFull = roi_x + best.x;
            const double bestYFull = roi_y + best.y;
            // 存储 ROI 相对坐标
            roiAndFocuserInfo["SelectStarX"] = best.x;
            roiAndFocuserInfo["SelectStarY"] = best.y;
            roiAndFocuserInfo["SelectStarHFR"] = best.HFR;
            roiAndFocuserInfo["SelectStarSNR"] = best.theta;
            roiAndFocuserInfo["SelectStarLocalMax"] = best.a;
            roiAndFocuserInfo["SelectStarBgStd"] = best.b;
            // 更新锁定星点的全图坐标
            lockedStarFull = QPointF(bestXFull, bestYFull);
            Logger::Log("selectStar | tracking locked star: ROI(" + std::to_string(best.x) + "," + std::to_string(best.y) + ") Full(" + std::to_string(bestXFull) + "," + std::to_string(bestYFull) + ") HFR=" + std::to_string(best.HFR) + " SNR=" + std::to_string(best.theta) + " localMax=" + std::to_string(best.a) + " bgStd=" + std::to_string(best.b), LogLevel::DEBUG, DeviceType::FOCUSER);
            // 判断是否需要居中（挂起到下一帧应用）
            const double centerX = roi_x + boxSide / 2.0;
            const double centerY = roi_y + boxSide / 2.0;
            const double halfWin = boxSide * trackWindowRatio; // 半窗
            const bool outOfWindow = std::abs(bestXFull - centerX) > halfWin || std::abs(bestYFull - centerY) > halfWin;
            if (enableAutoRoiCentering)
            {
                if (outOfWindow) {
                    outOfWindowFrames++;
                } else {
                    outOfWindowFrames = 0;
                }
                if (outOfWindowFrames >= requiredOutFramesForRecentre) {
                    double newRoiX = bestXFull - boxSide / 2.0;
                    double newRoiY = bestYFull - boxSide / 2.0;
                    const int maxX = std::max(0, glMainCCDSizeX - static_cast<int>(boxSide));
                    const int maxY = std::max(0, glMainCCDSizeY - static_cast<int>(boxSide));
                    newRoiX = std::min<double>(std::max<double>(0, newRoiX), maxX);
                    newRoiY = std::min<double>(std::max<double>(0, newRoiY), maxY);
                    int newRoiXi = static_cast<int>(std::lround(newRoiX));
                    int newRoiYi = static_cast<int>(std::lround(newRoiY));
                    if (newRoiXi % 2 != 0) newRoiXi += 1;
                    if (newRoiYi % 2 != 0) newRoiYi += 1;
                    newRoiXi = std::min(std::max(0, newRoiXi), maxX);
                    newRoiYi = std::min(std::max(0, newRoiYi), maxY);
                    // 不立即应用到本帧，挂起到下一帧再更新
                    hasPendingRoiUpdate = true;
                    pendingRoiX = newRoiXi;
                    pendingRoiY = newRoiYi;
                    outOfWindowFrames = 0;
                    Logger::Log("selectStar | tracking window exceeded for consecutive frames, pending ROI recenter", LogLevel::INFO, DeviceType::FOCUSER);
                }
            }
            return lockedStarFull;
        }
        // 若锁定丢失，则继续按下面的自动选择逻辑
        Logger::Log("selectStar | locked star lost, attempting re-selection", LogLevel::WARNING, DeviceType::FOCUSER);
        selectedStarLocked = false;
        lockedStarFull = QPointF(-1, -1);
    }

    // 3) 自动选择 ROI 内"最亮且最大的"星点
    // 简化：按亮度/面积代理：优先 HFR 大、或亮度（这里可用 HFR 或自带的 brightness 字段，如无则用 HFR）
    int bestIdx = -1; double bestScore = -1;
    for (int i = 0; i < stars.size(); ++i) {
        const auto &s = stars[i];
        if (s.HFR <= 0) continue;
        // 排除 ROI 边缘星点（使用 <= 和 >= 确保边缘像素被排除）
        if (s.x <= edgeMargin || s.y <= edgeMargin || 
            s.x >= boxSide - edgeMargin || s.y >= boxSide - edgeMargin) {
            continue;
        }
        // 评分：HFR 越大越好，可叠加亮度，如 s.peak 或 s.flux（若结构里没有则仅用 HFR）
        double score = s.HFR; // TODO: 若有亮度字段可加权
        if (score > bestScore) { bestScore = score; bestIdx = i; }
    }
    if (bestIdx == -1) {
        Logger::Log("selectStar | no valid ROI star for auto-select", LogLevel::WARNING, DeviceType::FOCUSER);
        return QPointF(CurrentPosition, 0);
    }

    const auto &autoBest = stars[bestIdx];
    const double bestXFullAuto = roi_x + autoBest.x;
    const double bestYFullAuto = roi_y + autoBest.y;
    // 存储 ROI 相对坐标
    roiAndFocuserInfo["SelectStarX"] = autoBest.x;
    roiAndFocuserInfo["SelectStarY"] = autoBest.y;
    roiAndFocuserInfo["SelectStarHFR"] = autoBest.HFR;
    roiAndFocuserInfo["SelectStarSNR"] = autoBest.theta;
    roiAndFocuserInfo["SelectStarLocalMax"] = autoBest.a;
    roiAndFocuserInfo["SelectStarBgStd"] = autoBest.b;
    // 锁定星点的全图坐标
    lockedStarFull = QPointF(bestXFullAuto, bestYFullAuto);
    selectedStarLocked = true; // 锁定
    Logger::Log("selectStar | auto-selected and locked new star ROI(x,y,HFR,SNR,localMax,bgStd)=(" + std::to_string(autoBest.x) + "," + std::to_string(autoBest.y) + "," + std::to_string(autoBest.HFR) + "," + std::to_string(autoBest.theta) + "," + std::to_string(autoBest.a) + "," + std::to_string(autoBest.b) + ") Full(" + std::to_string(bestXFullAuto) + "," + std::to_string(bestYFullAuto) + ")", LogLevel::INFO, DeviceType::FOCUSER);
    return lockedStarFull;

    // 旧分支与重复逻辑清理完毕
}

void MainWindow::startAutoFocus()
{
    // 检查电调是否连接（支持SDK和INDI模式）
    const bool useSdkMainCamera = isMainCameraSDK();   // 单设备：主相机是否走 SDK
    const bool useSdkFocuser    = isFocuserSDK();      // 单设备：电调是否走 SDK
    bool focuserConnected = (dpFocuser != nullptr) || useSdkFocuser;
    
    if (!focuserConnected || !isMainCameraConnected())
    {
        Logger::Log("AutoFocus | 调焦器或相机未连接", LogLevel::WARNING, DeviceType::FOCUSER);
        isAutoFocus = false;
        emit wsThread->sendMessageToClient("AutoFocusOver:false");
        return;
    }

    // 注意：SDK 是“单设备连接”——主相机/电调可分别走 SDK/INDI。
    // AutoFocus 内部已按 useSdkMainCamera/useSdkFocuser 分流：
    // - 主相机 SDK：通过 requestCapture/requestAbortCapture 走 MainWindow::startMainCameraCapture/abortMainCameraCapture
    // - 电调 SDK：通过 SdkManager::callByHandle( sdkFocuserHandle ) 执行移动/读位置/Abort
    // 预处理：统一清理自动对焦相关定时器与信号连接，避免残留或重复
    cleanupAutoFocusConnections();



    if (autoFocus == nullptr)
    {
        autoFocus = new AutoFocus(indi_Client, dpFocuser, dpMainCamera, wsThread,
                                  useSdkMainCamera, useSdkFocuser, sdkFocuserHandle, this);
    }
    else
    {
        // 停止旧对象并清理信号连接
        autoFocus->stopAutoFocus();
        cleanupAutoFocusConnections();
        autoFocus->deleteLater();
        autoFocus = nullptr;
        autoFocus = new AutoFocus(indi_Client, dpFocuser, dpMainCamera, wsThread,
                                  useSdkMainCamera, useSdkFocuser, sdkFocuserHandle, this);
    }

    // SDK/INDI 统一拍摄入口：由 AutoFocus 发起 requestCapture/requestAbortCapture，MainWindow 调用统一入口执行
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::requestCapture,
                                           this, [this](int exposureMs) { this->startMainCameraCapture(exposureMs); },
                                           Qt::QueuedConnection));
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::requestAbortCapture,
                                           this, [this]() { this->abortMainCameraCapture(); },
                                           Qt::QueuedConnection));
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::coarseRetryPromptRequested,
                                           this, [this](int totalDivisions, const QString &message)
                                           {
        Logger::Log(QString("请求前端弹出粗调补扫确认框：totalDivisions=%1, message=%2")
                        .arg(totalDivisions).arg(message).toStdString(),
                    LogLevel::WARNING, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(
            QString("AutoFocusCoarseRetryPrompt:%1:%2").arg(totalDivisions).arg(message));
    }));
    autoFocus->setFocuserMinPosition(focuserMinPosition);
    autoFocus->setFocuserMaxPosition(focuserMaxPosition);
    autoFocus->setCoarseDivisionCount(autoFocusCoarseDivisions);
    autoFocus->setScheduleTriggered(isScheduleTriggeredAutoFocus);
    autoFocus->setDefaultExposureTime(autoFocusExposureTime); // 自动对焦曝光时间（仅作用于自动对焦）
    autoFocus->setUseVirtualData(false);      // 使用虚拟数据
    
    // 设置空程补偿
    if (autofocusBacklashCompensation > 0) {
        autoFocus->setBacklashCompensation(autofocusBacklashCompensation, autofocusBacklashCompensation);
        autoFocus->setUseBacklashCompensation(true);
        Logger::Log(QString("设置自动对焦空程补偿: %1步").arg(autofocusBacklashCompensation).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
    } else {
        autoFocus->setUseBacklashCompensation(false);
        Logger::Log("自动对焦不使用空程补偿", LogLevel::INFO, DeviceType::FOCUSER);
    }
   for (int i = 1; i <= 11; i++) {
    std::string filename = "/home/quarcs/test_fits/coarse/" + std::to_string(i) + ".fits";
    autoFocus->setCaptureComplete(filename.c_str());
    }
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::roiInfoChanged, this, [this](const QRect &roi)
            {
        if (roi.width() == 0 && roi.height() == 0){
            roiAndFocuserInfo["ROI_x"] = 0;
            roiAndFocuserInfo["ROI_y"] = 0;
            roiAndFocuserInfo["BoxSideLength"] = 300;
            autoFocuserIsROI = false;
        }else{
            roiAndFocuserInfo["ROI_x"] = roi.x();
            roiAndFocuserInfo["ROI_y"] = roi.y();
            roiAndFocuserInfo["BoxSideLength"] = roi.width();
            autoFocuserIsROI = true;
        } }));

    // 连接二次拟合结果信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::focusFitUpdated, this, [this](double a, double b, double c, double bestPosition, double minFWHM)
            {
        Logger::Log(QString("接收到focusFitUpdated信号: a=%1, b=%2, c=%3, bestPosition=%4, minFWHM=%5")
                   .arg(a).arg(b).arg(c).arg(bestPosition).arg(minFWHM).toStdString(), 
                   LogLevel::INFO, DeviceType::FOCUSER);
        
        // 发送二次曲线数据到前端
        QString curveData = QString("fitQuadraticCurve:%1:%2:%3:%4:%5")
                           .arg(a, 0, 'g', 15)  // 使用科学计数法，保留15位有效数字，确保小系数不被截断
                           .arg(b, 0, 'g', 15)
                           .arg(c, 0, 'g', 15)
                           .arg(bestPosition, 0, 'f', 2)
                           .arg(minFWHM, 0, 'f', 3);
        
        Logger::Log(QString("发送二次曲线数据: %1").arg(curveData).toStdString(), 
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(curveData);
        
        // 发送最佳位置点数据
        QString minPointData = QString("fitQuadraticCurve_minPoint:%1:%2")
                              .arg(bestPosition, 0, 'f', 2)
                              .arg(minFWHM, 0, 'f', 3);
        
        Logger::Log(QString("发送最小点数据: %1").arg(minPointData).toStdString(), 
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(minPointData);
        
        Logger::Log(QString("二次拟合结果发送完成").toStdString(), 
                   LogLevel::INFO, DeviceType::FOCUSER);
    }));

    // 连接数据点信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::focusDataPointReady, this, [this](int position, double fwhm, const QString &stage)
            {
        Logger::Log(QString("接收到数据点: position=%1, fwhm=%2, stage=%3")
                   .arg(position).arg(fwhm).arg(stage).toStdString(), 
                   LogLevel::INFO, DeviceType::FOCUSER);
        
        // 发送数据点到前端
        QString dataPointMessage = QString("FocusMoveDone:%1:%2")
                                 .arg(position).arg(fwhm);
        
        Logger::Log(QString("发送数据点: %1").arg(dataPointMessage).toStdString(), 
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(dataPointMessage);
    }));

    // 连接启动位置更新定时器信号
    // connect(autoFocus, &AutoFocus::startPositionUpdateTimer, this, [this]()
    //         {
    //     Logger::Log("启动位置更新定时器", LogLevel::INFO, DeviceType::FOCUSER);
    //     if (focusMoveTimer) {
    //         focusMoveTimer->start(50); // 改为50毫秒间隔，与实时位置更新保持一致
    //     }
    //     // 确保实时位置更新定时器也在运行
    //     if (realtimePositionTimer) {
    //         realtimePositionTimer->start(50);
    //     }
    // });

    // 连接自动对焦失败信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autofocusFailed, this, [this]()
            {
        Logger::Log("自动对焦失败，发送提示消息到前端", LogLevel::ERROR, DeviceType::FOCUSER);
        
        // 检查是否是由计划任务表触发的自动对焦
        bool wasScheduleTriggered = isScheduleTriggeredAutoFocus;
        int savedCFWpos = schedule_CFWpos; // 保存当前任务的滤镜位置
        
        isAutoFocus = false;
        // 如果是计划触发的自动对焦，向前端报告步骤完成（失败）
        if (wasScheduleTriggered)
        {
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "autofocus:" +
                "0:" +
                "0:" +
                "100");
        }
        isScheduleTriggeredAutoFocus = false; // 重置标志
        
        emit wsThread->sendMessageToClient("FitResult:Failed:拟合结果为水平线，未找到最佳焦点");
        
        // 如果是由计划任务表触发的自动对焦失败，仍然继续执行拍摄任务
        if (wasScheduleTriggered)
        {
            Logger::Log("计划任务表触发的自动对焦失败，但仍继续执行拍摄任务", LogLevel::WARNING, DeviceType::MAIN);
            qDebug() << "Schedule-triggered autofocus failed, but continuing with CFW setup...";
            
            // 检查是否已停止计划任务表
            if (StopSchedule)
            {
                Logger::Log("计划任务表已停止，不再继续执行拍摄任务", LogLevel::INFO, DeviceType::MAIN);
                StopSchedule = false;
                return;
            }
            
            // 继续执行设置滤镜和拍摄
            startSetCFW(savedCFWpos);
        }
    }));

    // 连接星点识别结果信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::starDetectionResult, this, [this](bool detected, double fwhm)
            {
        if (detected) {
            Logger::Log(QString("识别到星点，FWHM为: %1").arg(fwhm).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient(QString("StarDetectionResult:true:%1").arg(fwhm));
        } else {
            Logger::Log("未识别到星点", LogLevel::INFO, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient("StarDetectionResult:false:0");
        }
    }));

    // 连接自动对焦模式变化信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autoFocusModeChanged, this, [this](const QString &mode, double fwhm)
            {
        Logger::Log(QString("自动对焦模式变化: %1, FWHM: %2").arg(mode).arg(fwhm).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(QString("AutoFocusModeChanged:%1:%2").arg(mode).arg(fwhm));
    }));

    // 连接自动对焦步骤变化信号 - [AUTO_FOCUS_UI_ENHANCEMENT]
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autoFocusStepChanged, this, [this](int step, const QString &description)
            {
        Logger::Log(QString("自动对焦步骤变化: 步骤%1 - %2").arg(step).arg(description).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(QString("AutoFocusStepChanged:%1:%2").arg(step).arg(description));
    }));

  // 连接自动对焦拍摄进度信号：将各阶段拍摄进度转发到前端
  autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::captureProgressChanged,
                                         this, [this](const QString &stage, int current, int total)
                                         {
    Logger::Log(QString("自动对焦拍摄进度: 阶段=%1, 当前=%2, 总数=%3")
                .arg(stage).arg(current).arg(total).toStdString(),
                LogLevel::INFO, DeviceType::FOCUSER);
    emit wsThread->sendMessageToClient(
          QString("AutoFocusCaptureProgress:%1:%2:%3").arg(stage).arg(current).arg(total));
  }));

    // 连接自动对焦完成信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autoFocusCompleted, this, [this](bool success, double bestPosition, double minHFR)
            {
        Logger::Log(QString("自动对焦完成: success=%1, bestPosition=%2, minHFR=%3")
                   .arg(success).arg(bestPosition).arg(minHFR).toStdString(), 
                   LogLevel::INFO, DeviceType::FOCUSER);
        
        // 检查是否是由计划任务表触发的自动对焦
        bool wasScheduleTriggered = isScheduleTriggeredAutoFocus;
        int savedCFWpos = schedule_CFWpos; // 保存当前任务的滤镜位置
        
        // 结束阶段：统一清理自动对焦相关定时器与信号连接
        cleanupAutoFocusConnections();
        
        // 如果是计划触发的自动对焦，向前端报告步骤完成
        if (wasScheduleTriggered)
        {
            emit wsThread->sendMessageToClient(
                "ScheduleStepState:" +
                QString::number(schedule_currentNum) + ":" +
                "autofocus:" +
                "0:" +
                "0:" +
                "100");
        }
        
        // 重置计划任务表触发标志
        isScheduleTriggeredAutoFocus = false;
        
        // 确保实时位置更新定时器继续运行
        // if (realtimePositionTimer && !realtimePositionTimer->isActive()) {
        //     realtimePositionTimer->start(50);
        //     Logger::Log("恢复实时位置更新定时器", LogLevel::INFO, DeviceType::FOCUSER);
        // }
        
        // 发送自动对焦完成消息到前端
        QString completeMessage = QString("AutoFocusOver:%1:%2:%3")
                                .arg(success ? "true" : "false")
                                .arg(bestPosition, 0, 'f', 2)
                                .arg(minHFR, 0, 'f', 3);
        
        Logger::Log(QString("发送自动对焦完成消息: %1").arg(completeMessage).toStdString(), 
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(completeMessage);
        isAutoFocus = false;
        
        // 发送自动对焦结束事件到前端 - [AUTO_FOCUS_UI_ENHANCEMENT]
        emit wsThread->sendMessageToClient("AutoFocusEnded:自动对焦已结束");
        
        // 如果是由计划任务表触发的自动对焦，完成后继续执行拍摄任务
        if (wasScheduleTriggered)
        {
            Logger::Log("计划任务表触发的自动对焦已完成，继续执行拍摄任务", LogLevel::INFO, DeviceType::MAIN);
            qDebug() << "Schedule-triggered autofocus completed, continuing with CFW setup...";
            
            // 检查是否已停止计划任务表
            if (StopSchedule)
            {
                Logger::Log("计划任务表已停止，不再继续执行拍摄任务", LogLevel::INFO, DeviceType::MAIN);
                StopSchedule = false;
                return;
            }
            
            // 继续执行设置滤镜和拍摄
            startSetCFW(savedCFWpos);
        }
    }));

    autoFocus->startAutoFocus();
    isAutoFocus = true;
    autoFocusStep = 0;
}

void MainWindow::startAutoFocusFineHFROnly()
{
    // 检查电调/相机连接（支持SDK和INDI模式）
    const bool useSdkMainCamera = isMainCameraSDK();
    const bool useSdkFocuser    = isFocuserSDK();
    bool focuserConnected = (dpFocuser != nullptr) || useSdkFocuser;
    if (!focuserConnected || !isMainCameraConnected())
    {
        Logger::Log("AutoFocus (fine-HFR only) | 调焦器或相机未连接", LogLevel::WARNING, DeviceType::FOCUSER);
        isAutoFocus = false;
        emit wsThread->sendMessageToClient("AutoFocusOver:false");
        return;
    }

    // 预处理：统一清理自动对焦相关定时器与信号连接，避免残留或重复
    cleanupAutoFocusConnections();

    if (autoFocus == nullptr)
    {
        autoFocus = new AutoFocus(indi_Client, dpFocuser, dpMainCamera, wsThread,
                                  useSdkMainCamera, useSdkFocuser, sdkFocuserHandle, this);
    }
    else
    {
        // 停止旧对象并清理信号连接
        autoFocus->stopAutoFocus();
        cleanupAutoFocusConnections();
        autoFocus->deleteLater();
        autoFocus = nullptr;
        autoFocus = new AutoFocus(indi_Client, dpFocuser, dpMainCamera, wsThread,
                                  useSdkMainCamera, useSdkFocuser, sdkFocuserHandle, this);
    }

    // SDK/INDI 统一拍摄入口（同 startAutoFocus）
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::requestCapture,
                                           this, [this](int exposureMs) { this->startMainCameraCapture(exposureMs); },
                                           Qt::QueuedConnection));
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::requestAbortCapture,
                                           this, [this]() { this->abortMainCameraCapture(); },
                                           Qt::QueuedConnection));
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::coarseRetryPromptRequested,
                                           this, [this](int totalDivisions, const QString &message)
                                           {
        Logger::Log(QString("请求前端弹出粗调补扫确认框：totalDivisions=%1, message=%2")
                        .arg(totalDivisions).arg(message).toStdString(),
                    LogLevel::WARNING, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(
            QString("AutoFocusCoarseRetryPrompt:%1:%2").arg(totalDivisions).arg(message));
    }));

    // 与常规自动对焦保持一致的参数配置
    autoFocus->setFocuserMinPosition(focuserMinPosition);
    autoFocus->setFocuserMaxPosition(focuserMaxPosition);
    autoFocus->setCoarseDivisionCount(autoFocusCoarseDivisions);
    autoFocus->setScheduleTriggered(isScheduleTriggeredAutoFocus);
    autoFocus->setDefaultExposureTime(autoFocusExposureTime); // 自动对焦曝光时间（仅作用于自动对焦）
    autoFocus->setUseVirtualData(false);      // 使用实时数据

    // 设置空程补偿
    if (autofocusBacklashCompensation > 0) {
        autoFocus->setBacklashCompensation(autofocusBacklashCompensation, autofocusBacklashCompensation);
        autoFocus->setUseBacklashCompensation(true);
        Logger::Log(QString("设置自动对焦空程补偿: %1步").arg(autofocusBacklashCompensation).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
    } else {
        autoFocus->setUseBacklashCompensation(false);
        Logger::Log("自动对焦不使用空程补偿", LogLevel::INFO, DeviceType::FOCUSER);
    }

    // 复用与 startAutoFocus / startAutoFocusSuperFineOnly 相同的信号连接，确保前端表现一致
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::roiInfoChanged, this, [this](const QRect &roi)
            {
        if (roi.width() == 0 && roi.height() == 0){
            roiAndFocuserInfo["ROI_x"] = 0;
            roiAndFocuserInfo["ROI_y"] = 0;
            roiAndFocuserInfo["BoxSideLength"] = 300;
            autoFocuserIsROI = false;
        }else{
            roiAndFocuserInfo["ROI_x"] = roi.x();
            roiAndFocuserInfo["ROI_y"] = roi.y();
            roiAndFocuserInfo["BoxSideLength"] = roi.width();
            autoFocuserIsROI = true;
        } }));

    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::focusFitUpdated, this, [this](double a, double b, double c, double bestPosition, double minFWHM)
            {
        Logger::Log(QString("接收到focusFitUpdated信号: a=%1, b=%2, c=%3, bestPosition=%4, minFWHM=%5")
                   .arg(a).arg(b).arg(c).arg(bestPosition).arg(minFWHM).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);

        QString curveData = QString("fitQuadraticCurve:%1:%2:%3:%4:%5")
                           .arg(a, 0, 'g', 15)
                           .arg(b, 0, 'g', 15)
                           .arg(c, 0, 'g', 15)
                           .arg(bestPosition, 0, 'f', 2)
                           .arg(minFWHM, 0, 'f', 3);

        Logger::Log(QString("发送二次曲线数据: %1").arg(curveData).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(curveData);

        QString minPointData = QString("fitQuadraticCurve_minPoint:%1:%2")
                              .arg(bestPosition, 0, 'f', 2)
                              .arg(minFWHM, 0, 'f', 3);

        Logger::Log(QString("发送最小点数据: %1").arg(minPointData).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(minPointData);

        Logger::Log(QString("二次拟合结果发送完成").toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);
    }));

    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::focusDataPointReady, this, [this](int position, double fwhm, const QString &stage)
            {
        Logger::Log(QString("接收到数据点: position=%1, fwhm=%2, stage=%3")
                   .arg(position).arg(fwhm).arg(stage).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);

        QString dataPointMessage = QString("FocusMoveDone:%1:%2")
                                 .arg(position).arg(fwhm);

        Logger::Log(QString("发送数据点: %1").arg(dataPointMessage).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(dataPointMessage);
    }));

    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autofocusFailed, this, [this]()
            {
        Logger::Log("自动对焦失败，发送提示消息到前端", LogLevel::ERROR, DeviceType::FOCUSER);

        bool wasScheduleTriggered = isScheduleTriggeredAutoFocus;
        int savedCFWpos = schedule_CFWpos;

        isAutoFocus = false;
        isScheduleTriggeredAutoFocus = false;

        emit wsThread->sendMessageToClient("FitResult:Failed:拟合结果为水平线，未找到最佳焦点");

        if (wasScheduleTriggered)
        {
            Logger::Log("计划任务表触发的自动对焦失败，但仍继续执行拍摄任务", LogLevel::WARNING, DeviceType::MAIN);
            qDebug() << "Schedule-triggered autofocus failed, but continuing with CFW setup...";

            if (StopSchedule)
            {
                Logger::Log("计划任务表已停止，不再继续执行拍摄任务", LogLevel::INFO, DeviceType::MAIN);
                StopSchedule = false;
                return;
            }

            startSetCFW(savedCFWpos);
        }
    }));

    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::starDetectionResult, this, [this](bool detected, double fwhm)
            {
        if (detected) {
            Logger::Log(QString("识别到星点，FWHM为: %1").arg(fwhm).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient(QString("StarDetectionResult:true:%1").arg(fwhm));
        } else {
            Logger::Log("未识别到星点", LogLevel::INFO, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient("StarDetectionResult:false:0");
        }
    }));

    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autoFocusModeChanged, this, [this](const QString &mode, double fwhm)
            {
        Logger::Log(QString("自动对焦模式变化: %1, FWHM: %2").arg(mode).arg(fwhm).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(QString("AutoFocusModeChanged:%1:%2").arg(mode).arg(fwhm));
    }));

    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autoFocusStepChanged, this, [this](int step, const QString &description)
            {
        Logger::Log(QString("自动对焦步骤变化: 步骤%1 - %2").arg(step).arg(description).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(QString("AutoFocusStepChanged:%1:%2").arg(step).arg(description));
    }));

    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autoFocusCompleted, this, [this](bool success, double bestPosition, double minHFR)
            {
        Logger::Log(QString("自动对焦完成: success=%1, bestPosition=%2, minHFR=%3")
                   .arg(success).arg(bestPosition).arg(minHFR).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);

        bool wasScheduleTriggered = isScheduleTriggeredAutoFocus;
        int savedCFWpos = schedule_CFWpos;

        cleanupAutoFocusConnections();

        isScheduleTriggeredAutoFocus = false;

        QString completeMessage = QString("AutoFocusOver:%1:%2:%3")
                                .arg(success ? "true" : "false")
                                .arg(bestPosition, 0, 'f', 2)
                                .arg(minHFR, 0, 'f', 3);

        Logger::Log(QString("发送自动对焦完成消息: %1").arg(completeMessage).toStdString(),
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(completeMessage);
        isAutoFocus = false;

        emit wsThread->sendMessageToClient("AutoFocusEnded:自动对焦已结束");

        if (wasScheduleTriggered)
        {
            Logger::Log("计划任务表触发的自动对焦已完成，继续执行拍摄任务", LogLevel::INFO, DeviceType::MAIN);
            qDebug() << "Schedule-triggered autofocus completed, continuing with CFW setup...";

            if (StopSchedule)
            {
                Logger::Log("计划任务表已停止，不再继续执行拍摄任务", LogLevel::INFO, DeviceType::MAIN);
                StopSchedule = false;
                return;
            }

            startSetCFW(savedCFWpos);
        }
    }));

    // 仅精调(Fine)：从当前位置开始，先拍当前位置，再围绕当前位置双向展开
    autoFocus->startLocalFineAdjustmentFromCurrentPosition();
    isAutoFocus = true;
    autoFocusStep = 0;
}
void MainWindow::startAutoFocusSuperFineOnly()
{
    const bool useSdkMainCamera = isMainCameraSDK();
    const bool useSdkFocuser    = isFocuserSDK();
    const bool focuserConnected = (dpFocuser != nullptr) || useSdkFocuser;
    if (!focuserConnected || !isMainCameraConnected())
    {
        Logger::Log("AutoFocus (super-fine only) | 调焦器或相机未连接", LogLevel::WARNING, DeviceType::FOCUSER);
        isAutoFocus = false;
        emit wsThread->sendMessageToClient("AutoFocusOver:false");
        return;
    }

    // 预处理：统一清理自动对焦相关定时器与信号连接，避免残留或重复
    cleanupAutoFocusConnections();

    if (autoFocus == nullptr)
    {
        autoFocus = new AutoFocus(indi_Client, dpFocuser, dpMainCamera, wsThread,
                                  useSdkMainCamera, useSdkFocuser, sdkFocuserHandle, this);
    }
    else
    {
        // 停止旧对象并清理信号连接
        autoFocus->stopAutoFocus();
        cleanupAutoFocusConnections();
        autoFocus->deleteLater();
        autoFocus = nullptr;
        autoFocus = new AutoFocus(indi_Client, dpFocuser, dpMainCamera, wsThread,
                                  useSdkMainCamera, useSdkFocuser, sdkFocuserHandle, this);
    }

    // SDK/INDI 统一拍摄入口（同 startAutoFocus）
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::requestCapture,
                                           this, [this](int exposureMs) { this->startMainCameraCapture(exposureMs); },
                                           Qt::QueuedConnection));
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::requestAbortCapture,
                                           this, [this]() { this->abortMainCameraCapture(); },
                                           Qt::QueuedConnection));
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::coarseRetryPromptRequested,
                                           this, [this](int totalDivisions, const QString &message)
                                           {
        Logger::Log(QString("请求前端弹出粗调补扫确认框：totalDivisions=%1, message=%2")
                        .arg(totalDivisions).arg(message).toStdString(),
                    LogLevel::WARNING, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(
            QString("AutoFocusCoarseRetryPrompt:%1:%2").arg(totalDivisions).arg(message));
    }));

    // 与常规自动对焦保持一致的参数配置
    autoFocus->setFocuserMinPosition(focuserMinPosition);
    autoFocus->setFocuserMaxPosition(focuserMaxPosition);
    autoFocus->setCoarseDivisionCount(autoFocusCoarseDivisions);
    autoFocus->setScheduleTriggered(isScheduleTriggeredAutoFocus);
    autoFocus->setDefaultExposureTime(autoFocusExposureTime); // 自动对焦曝光时间（仅作用于自动对焦）
    autoFocus->setUseVirtualData(false);      // 使用虚拟数据

    // 设置空程补偿
    if (autofocusBacklashCompensation > 0) {
        autoFocus->setBacklashCompensation(autofocusBacklashCompensation, autofocusBacklashCompensation);
        autoFocus->setUseBacklashCompensation(true);
        Logger::Log(QString("设置自动对焦空程补偿: %1步").arg(autofocusBacklashCompensation).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
    } else {
        autoFocus->setUseBacklashCompensation(false);
        Logger::Log("自动对焦不使用空程补偿", LogLevel::INFO, DeviceType::FOCUSER);
    }

    // 复用与 startAutoFocus 相同的信号连接
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::roiInfoChanged, this, [this](const QRect &roi)
            {
        if (roi.width() == 0 && roi.height() == 0){
            roiAndFocuserInfo["ROI_x"] = 0;
            roiAndFocuserInfo["ROI_y"] = 0;
            roiAndFocuserInfo["BoxSideLength"] = 300;
            autoFocuserIsROI = false;
        }else{
            roiAndFocuserInfo["ROI_x"] = roi.x();
            roiAndFocuserInfo["ROI_y"] = roi.y();
            roiAndFocuserInfo["BoxSideLength"] = roi.width();
            autoFocuserIsROI = true;
        } }));

    // 连接二次拟合结果信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::focusFitUpdated, this, [this](double a, double b, double c, double bestPosition, double minFWHM)
            {
        Logger::Log(QString("接收到focusFitUpdated信号: a=%1, b=%2, c=%3, bestPosition=%4, minFWHM=%5")
                   .arg(a).arg(b).arg(c).arg(bestPosition).arg(minFWHM).toStdString(), 
                   LogLevel::INFO, DeviceType::FOCUSER);
        
        // 发送二次曲线数据到前端
        QString curveData = QString("fitQuadraticCurve:%1:%2:%3:%4:%5")
                           .arg(a, 0, 'g', 15)  // 使用科学计数法，保留15位有效数字，确保小系数不被截断
                           .arg(b, 0, 'g', 15)
                           .arg(c, 0, 'g', 15)
                           .arg(bestPosition, 0, 'f', 2)
                           .arg(minFWHM, 0, 'f', 3);
        
        Logger::Log(QString("发送二次曲线数据: %1").arg(curveData).toStdString(), 
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(curveData);
        
        // 发送最佳位置点数据
        QString minPointData = QString("fitQuadraticCurve_minPoint:%1:%2")
                              .arg(bestPosition, 0, 'f', 2)
                              .arg(minFWHM, 0, 'f', 3);
        
        Logger::Log(QString("发送最小点数据: %1").arg(minPointData).toStdString(), 
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(minPointData);
        
        Logger::Log(QString("二次拟合结果发送完成").toStdString(), 
                   LogLevel::INFO, DeviceType::FOCUSER);
    }));

    // 连接数据点信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::focusDataPointReady, this, [this](int position, double fwhm, const QString &stage)
            {
        Logger::Log(QString("接收到数据点: position=%1, fwhm=%2, stage=%3")
                   .arg(position).arg(fwhm).arg(stage).toStdString(), 
                   LogLevel::INFO, DeviceType::FOCUSER);
        
        // 发送数据点到前端
        QString dataPointMessage = QString("FocusMoveDone:%1:%2")
                                 .arg(position).arg(fwhm);
        
        Logger::Log(QString("发送数据点: %1").arg(dataPointMessage).toStdString(), 
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(dataPointMessage);
    }));

    // 连接自动对焦失败信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autofocusFailed, this, [this]()
            {
        Logger::Log("自动对焦失败，发送提示消息到前端", LogLevel::ERROR, DeviceType::FOCUSER);
        
        // 检查是否是由计划任务表触发的自动对焦
        bool wasScheduleTriggered = isScheduleTriggeredAutoFocus;
        int savedCFWpos = schedule_CFWpos; // 保存当前任务的滤镜位置
        
        isAutoFocus = false;
        isScheduleTriggeredAutoFocus = false; // 重置标志
        
        emit wsThread->sendMessageToClient("FitResult:Failed:拟合结果为水平线，未找到最佳焦点");
        
        // 如果是由计划任务表触发的自动对焦失败，仍然继续执行拍摄任务
        if (wasScheduleTriggered)
        {
            Logger::Log("计划任务表触发的自动对焦失败，但仍继续执行拍摄任务", LogLevel::WARNING, DeviceType::MAIN);
            qDebug() << "Schedule-triggered autofocus failed, but continuing with CFW setup...";
            
            // 检查是否已停止计划任务表
            if (StopSchedule)
            {
                Logger::Log("计划任务表已停止，不再继续执行拍摄任务", LogLevel::INFO, DeviceType::MAIN);
                StopSchedule = false;
                return;
            }
            
            // 继续执行设置滤镜和拍摄
            startSetCFW(savedCFWpos);
        }
    }));

    // 连接星点识别结果信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::starDetectionResult, this, [this](bool detected, double fwhm)
            {
        if (detected) {
            Logger::Log(QString("识别到星点，FWHM为: %1").arg(fwhm).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient(QString("StarDetectionResult:true:%1").arg(fwhm));
        } else {
            Logger::Log("未识别到星点", LogLevel::INFO, DeviceType::FOCUSER);
            emit wsThread->sendMessageToClient("StarDetectionResult:false:0");
        }
    }));

    // 连接自动对焦模式变化信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autoFocusModeChanged, this, [this](const QString &mode, double fwhm)
            {
        Logger::Log(QString("自动对焦模式变化: %1, FWHM: %2").arg(mode).arg(fwhm).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(QString("AutoFocusModeChanged:%1:%2").arg(mode).arg(fwhm));
    }));

    // 连接自动对焦步骤变化信号 - [AUTO_FOCUS_UI_ENHANCEMENT]
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autoFocusStepChanged, this, [this](int step, const QString &description)
            {
        Logger::Log(QString("自动对焦步骤变化: 步骤%1 - %2").arg(step).arg(description).toStdString(), LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(QString("AutoFocusStepChanged:%1:%2").arg(step).arg(description));
    }));

    // 连接自动对焦完成信号
    autoFocusConnections.push_back(connect(autoFocus, &AutoFocus::autoFocusCompleted, this, [this](bool success, double bestPosition, double minHFR)
            {
        Logger::Log(QString("自动对焦完成: success=%1, bestPosition=%2, minHFR=%3")
                   .arg(success).arg(bestPosition).arg(minHFR).toStdString(), 
                   LogLevel::INFO, DeviceType::FOCUSER);
        
        // 检查是否是由计划任务表触发的自动对焦
        bool wasScheduleTriggered = isScheduleTriggeredAutoFocus;
        int savedCFWpos = schedule_CFWpos; // 保存当前任务的滤镜位置
        
        // 结束阶段：统一清理自动对焦相关定时器与信号连接
        cleanupAutoFocusConnections();
        
        // 重置计划任务表触发标志
        isScheduleTriggeredAutoFocus = false;
        
        // 发送自动对焦完成消息到前端
        QString completeMessage = QString("AutoFocusOver:%1:%2:%3")
                                .arg(success ? "true" : "false")
                                .arg(bestPosition, 0, 'f', 2)
                                .arg(minHFR, 0, 'f', 3);
        
        Logger::Log(QString("发送自动对焦完成消息: %1").arg(completeMessage).toStdString(), 
                   LogLevel::INFO, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient(completeMessage);
        isAutoFocus = false;
        
        // 发送自动对焦结束事件到前端 - [AUTO_FOCUS_UI_ENHANCEMENT]
        emit wsThread->sendMessageToClient("AutoFocusEnded:自动对焦已结束");
        
        // 如果是由计划任务表触发的自动对焦，完成后继续执行拍摄任务
        if (wasScheduleTriggered)
        {
            Logger::Log("计划任务表触发的自动对焦已完成，继续执行拍摄任务", LogLevel::INFO, DeviceType::MAIN);
            qDebug() << "Schedule-triggered autofocus completed, continuing with CFW setup...";
            
            // 检查是否已停止计划任务表
            if (StopSchedule)
            {
                Logger::Log("计划任务表已停止，不再继续执行拍摄任务", LogLevel::INFO, DeviceType::MAIN);
                StopSchedule = false;
                return;
            }
            
            // 继续执行设置滤镜和拍摄
            startSetCFW(savedCFWpos);
        }
    }));

    autoFocus->startSuperFineFromCurrentPosition();
    isAutoFocus = true;
    autoFocusStep = 0;
}

void MainWindow::startScheduleAutoFocus()
{
    // 检查设备是否连接
    if (dpFocuser == NULL || !isMainCameraConnected())
    {
        Logger::Log("计划任务表自动对焦 | 调焦器或相机未连接，跳过自动对焦", LogLevel::WARNING, DeviceType::FOCUSER);
        isScheduleTriggeredAutoFocus = false;
        // 如果自动对焦失败，直接继续执行拍摄任务
        startSetCFW(schedule_CFWpos);
        return;
    }
    
    // 如果已经有自动对焦在运行，先停止
    if (isAutoFocus && autoFocus != nullptr)
    {
        Logger::Log("计划任务表自动对焦 | 检测到已有自动对焦在运行，先停止", LogLevel::INFO, DeviceType::FOCUSER);
        autoFocus->stopAutoFocus();
        cleanupAutoFocusConnections();
        autoFocus->deleteLater();
        autoFocus = nullptr;
        isAutoFocus = false;
    }
    
    // 标记这是由计划任务表触发的自动对焦
    isScheduleTriggeredAutoFocus = true;
    // 向前端发送步骤状态：当前行进入自动对焦阶段
    emit wsThread->sendMessageToClient(
        "ScheduleStepState:" +
        QString::number(schedule_currentNum) + ":" +
        "autofocus:" +
        "0:" +
        "0:" +
        "0");
    
    // 调用通用的自动对焦启动函数
    Logger::Log("计划任务表自动对焦 | Refocus=ON：开始执行自动对焦最后一步精调(super-fine)", LogLevel::INFO, DeviceType::MAIN);
    // 仅触发自动对焦的最后一步精调：从当前位置进入 super-fine 精调（跳过粗调/精调）
    startAutoFocusSuperFineOnly();
}

void MainWindow::cleanupAutoFocusConnections()
{
    if (autoFocus) {
        // 逐个断开并清空记录的连接，覆盖所有已登记连接
        for (const QMetaObject::Connection &c : autoFocusConnections) {
            QObject::disconnect(c);
        }
        autoFocusConnections.clear();
        // 保险起见再断开双方所有连接
        disconnect(autoFocus, nullptr, this, nullptr);
        disconnect(this, nullptr, autoFocus, nullptr);
    }
}


void MainWindow::getFocuserLoopingState()
{
    if (isFocusLoopShooting)
    {
        emit wsThread->sendMessageToClient("setFocuserLoopingState:true");
    }
    else
    {
        emit wsThread->sendMessageToClient("setFocuserLoopingState:false");
    }
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
void MainWindow::focusMoveToMin()
{
    emit wsThread->sendMessageToClient("focusMoveToMinStarted");
    const bool focuserSdkReady =
        (systemdevicelist.system_devices.size() > 22 &&
         systemdevicelist.system_devices[22].isSDKConnect &&
         systemdevicelist.system_devices[22].isBind &&
         sdkFocuserHandle != nullptr);
    if (dpFocuser == nullptr && !focuserSdkReady)
    {
        Logger::Log("focusMoveToMin | focuser is not connected (both INDI and SDK are NULL)", LogLevel::ERROR, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("focusMoveFailed:focuser is not connected");
        return;
    }

    // 读取/准备行程范围（SDK 模式用已保存的 min/max；若为空则用默认）
    int min = focuserMinPosition;
    int max = focuserMaxPosition;
    int step = 0;
    int value = 0;
    if (dpFocuser != nullptr)
    {
        indi_Client->getFocuserRange(dpFocuser, min, max, step, value);
        // 同步焦点器位置到中间位置（仅 INDI 模式有该调用）
        indi_Client->syncFocuserPosition(dpFocuser, (max + min) / 2);
    }
    if (min == -1 && max == -1)
    {
        min = -64000;
        max = 64000;
    }

    CurrentPosition = FocuserControl_getPosition();
    emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));

    int steps = std::min(std::max(0, CurrentPosition - min), kIndiFocuserRelMoveChunkMax);
    Logger::Log("focusMoveToMin | Moving to minimum position: " + std::to_string(min) + " with steps: " + std::to_string(steps), LogLevel::INFO, DeviceType::FOCUSER);
    if (steps <= 0)
    {
        emit wsThread->sendMessageToClient("focusMoveFailed:already at minimum or invalid range");
        return;
    }

    if (dpFocuser != nullptr)
    {
        indi_Client->setFocuserMoveDiretion(dpFocuser, true);
        indi_Client->moveFocuserSteps(dpFocuser, steps);
    }
    else if (focuserSdkReady)
    {
        SdkFocuserRelMoveParam p;
        p.outward = false;
        p.steps = steps;
        // SDK 模式：异步下发，避免主线程阻塞串口
        if (sdkFocuserExec && sdkFocuserExec->isRunning())
        {
            const SdkDeviceHandle handleSnap = sdkFocuserHandle;
            const uint64_t epochSnap = sdkFocuserOpEpoch.load(std::memory_order_relaxed);
            sdkFocuserExec->post([handleSnap, p, this, epochSnap]() {
                if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
                    return;
                SdkCommand moveCmd{SdkCommandType::Custom, "MoveRelative", p};
                // 直接通过设备句柄调用，无需指定驱动名称
                SdkManager::instance().callByHandle(handleSnap, moveCmd);
            });
        }
    }
    TargetPosition = CurrentPosition - steps;
    focusMoveToMaxorMinTimer = new QTimer(this);
    CurrentPosition = FocuserControl_getPosition();
    lastPosition = CurrentPosition;
    std::shared_ptr<int> noChangeCount = std::make_shared<int>(0); // 连续未变化计数器（使用shared_ptr保持状态）
    connect(focusMoveToMaxorMinTimer, &QTimer::timeout, this, [this, min, focuserSdkReady, noChangeCount]()
            {
        // SDK 模式下，需要主动请求位置更新，否则缓存值不会更新
        if (dpFocuser == nullptr && focuserSdkReady)
        {
            // 请求异步更新位置缓存
            if (!sdkFocuserPosTaskInFlight.load())
            {
                requestSdkFocuserPositionUpdate(false);
            }
        }
        CurrentPosition = FocuserControl_getPosition();
        emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
        if (CurrentPosition == TargetPosition){
            int steps = std::min(std::max(0, CurrentPosition - min), kIndiFocuserRelMoveChunkMax);
            if (steps <= 0)
            {
                // Reached the minimum limit
                FocuserControlStop();
                focusMoveToMaxorMinTimer->stop();
                emit wsThread->sendMessageToClient("FocusMoveToLimit:The current position has moved to the inner limit and cannot move further. If you need to continue moving, please recalibrate the position of the servo.");
                return;
            }

            if (dpFocuser != nullptr)
            {
                indi_Client->moveFocuserSteps(dpFocuser, steps);
            }
            else if (focuserSdkReady)
            {
                SdkFocuserRelMoveParam p;
                p.outward = false;
                p.steps = steps;
                if (sdkFocuserExec && sdkFocuserExec->isRunning())
                {
                    const SdkDeviceHandle handleSnap = sdkFocuserHandle;
                    const uint64_t epochSnap = sdkFocuserOpEpoch.load(std::memory_order_relaxed);
                    sdkFocuserExec->post([handleSnap, p, this, epochSnap]() {
                        if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
                            return;
                        SdkCommand moveCmd{SdkCommandType::Custom, "MoveRelative", p};
                        // 直接通过设备句柄调用，无需指定驱动名称
                SdkManager::instance().callByHandle(handleSnap, moveCmd);
                    });
                }
            }
            TargetPosition = CurrentPosition - steps;
            emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
            *noChangeCount = 0; // 重置计数器（目标位置已更新，继续移动）
            return;
        }
        if (CurrentPosition == lastPosition){
            // Position hasn't changed - increment counter
            (*noChangeCount)++;
            
            // Check if we've reached the minimum physical limit
            if (CurrentPosition <= min)
            {
                // At physical limit - stop immediately
                FocuserControlStop();
                focusMoveToMaxorMinTimer->stop();
                emit wsThread->sendMessageToClient("FocusMoveToLimit:The current position has moved to the inner limit and cannot move further. If you need to continue moving, please recalibrate the position of the servo.");
                return;
            }
            
            // Not at limit - check if stuck (3 consecutive checks with no change)
            if (*noChangeCount >= 5)
            {
                // Focuser appears to be stuck - position hasn't changed for 3 seconds
                FocuserControlStop();
                focusMoveToMaxorMinTimer->stop();
                emit wsThread->sendMessageToClient("focusMoveFailed:focuser appears to be stuck - position not changing for 3 seconds and not at physical limit");
                return;
            }
            // Less than 3 checks - wait for more checks
            return;
        }

        // Position has changed - reset counter and update lastPosition
        *noChangeCount = 0;
        lastPosition = CurrentPosition; });
    focusMoveToMaxorMinTimer->start(1000);
    Logger::Log("focusMoveToMin | Started moving to minimum position: " + std::to_string(min), LogLevel::INFO, DeviceType::FOCUSER);
}

// [停用 2026-04-14] 旧自动电调校准实现：保留函数体用于历史回溯，当前不再通过命令入口触发。
void MainWindow::focusMoveToMax()
{
    emit wsThread->sendMessageToClient("focusMoveToMaxStarted");
    const bool focuserSdkReady =
        (systemdevicelist.system_devices.size() > 22 &&
         systemdevicelist.system_devices[22].isSDKConnect &&
         systemdevicelist.system_devices[22].isBind &&
         sdkFocuserHandle != nullptr);
    if (dpFocuser == nullptr && !focuserSdkReady)
    {
        Logger::Log("focusMoveToMax | focuser is not connected (both INDI and SDK are NULL)", LogLevel::ERROR, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("focusMoveFailed:focuser is not connected");
        return;
    }

    FocuserControlStop();
    // 等待电调完全停止（包括惯性移动）
    if (dpFocuser == nullptr && focuserSdkReady)
    {
        // SDK模式：多次读取位置，确保电调完全停止
        sleep(1);
        requestSdkFocuserPositionUpdate(true);
        sleep(1);
        requestSdkFocuserPositionUpdate(true);
        sleep(1);
        requestSdkFocuserPositionUpdate(true);
        sleep(1);
    }
    else
    {
        // INDI模式：增加等待时间，确保电调完全停止
        int timeout = 0;
        while (timeout <= 3)  // 从2改为3，增加等待时间
        {
            CurrentPosition = FocuserControl_getPosition();
            emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
            sleep(1);
            timeout++;
        }
    }
    // 最后再读取一次位置，确保获取到稳定后的最终位置
    CurrentPosition = FocuserControl_getPosition();
    emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
    sleep(1);  // 再等待1秒
    CurrentPosition = FocuserControl_getPosition();
    // 更新最小范围为当前位置
    focuserMinPosition = CurrentPosition;
    
    // 安全检查：再次读取位置，确保没有因惯性超出
    sleep(1);
    int finalPosition = FocuserControl_getPosition();
    if (finalPosition < focuserMinPosition)
    {
        Logger::Log("focusMoveToMax | Position drifted due to inertia, adjusting min limit from " + 
                   std::to_string(focuserMinPosition) + " to " + std::to_string(finalPosition), 
                   LogLevel::WARNING, DeviceType::FOCUSER);
        focuserMinPosition = finalPosition;
        CurrentPosition = finalPosition;
    }
    
    Tools::saveParameter("Focuser", "focuserMinPosition", QString::number(focuserMinPosition));
    Logger::Log("focusMoveToMax | Set min position to: " + std::to_string(focuserMinPosition), 
               LogLevel::INFO, DeviceType::FOCUSER);
    
    int min = focuserMinPosition;
    int max = focuserMaxPosition;
    int step = 0;
    int value = 0;
    if (dpFocuser != nullptr)
        indi_Client->getFocuserRange(dpFocuser, min, max, step, value); // 获取焦点器范围
    if (min == -1 && max == -1)
    {
        min = -64000;
        max = 64000;
    }

    int steps = std::min(std::max(0, max - CurrentPosition), kIndiFocuserRelMoveChunkMax);
    if (steps <= 0)
    {
        emit wsThread->sendMessageToClient("focusMoveFailed:already at maximum or invalid range");
        return;
    }

    if (dpFocuser != nullptr)
    {
        indi_Client->setFocuserMoveDiretion(dpFocuser, false);
        indi_Client->moveFocuserSteps(dpFocuser, steps);
    }
    else if (focuserSdkReady)
    {
        SdkFocuserRelMoveParam p;
        p.outward = true;
        p.steps = steps;
        if (sdkFocuserExec && sdkFocuserExec->isRunning())
        {
            const SdkDeviceHandle handleSnap = sdkFocuserHandle;
            const uint64_t epochSnap = sdkFocuserOpEpoch.load(std::memory_order_relaxed);
            sdkFocuserExec->post([handleSnap, p, this, epochSnap]() {
                if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
                    return;
                SdkCommand moveCmd{SdkCommandType::Custom, "MoveRelative", p};
                // 直接通过设备句柄调用，无需指定驱动名称
                SdkManager::instance().callByHandle(handleSnap, moveCmd);
            });
        }
    }
    TargetPosition = CurrentPosition + steps;
    if (focusMoveToMaxorMinTimer != nullptr)
    {
        focusMoveToMaxorMinTimer->stop();
        focusMoveToMaxorMinTimer->deleteLater();
        focusMoveToMaxorMinTimer = nullptr;
    }
    focusMoveToMaxorMinTimer = new QTimer(this);
    CurrentPosition = FocuserControl_getPosition();
    lastPosition = CurrentPosition;
    std::shared_ptr<int> noChangeCount = std::make_shared<int>(0); // 连续未变化计数器（使用shared_ptr保持状态）
    connect(focusMoveToMaxorMinTimer, &QTimer::timeout, this, [this, max, focuserSdkReady, noChangeCount]()
            {
        // SDK 模式下，需要主动请求位置更新，否则缓存值不会更新
        if (dpFocuser == nullptr && focuserSdkReady)
        {
            // 请求异步更新位置缓存
            if (!sdkFocuserPosTaskInFlight.load())
            {
                requestSdkFocuserPositionUpdate(false);
            }
        }
        CurrentPosition = FocuserControl_getPosition();
        emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
        if (CurrentPosition == TargetPosition){
            int steps = std::min(std::max(0, max - CurrentPosition), kIndiFocuserRelMoveChunkMax);
            if (steps <= 0)
            {
                // Reached the maximum limit
                FocuserControlStop();
                focusMoveToMaxorMinTimer->stop();
                emit wsThread->sendMessageToClient("FocusMoveToLimit:The current position has moved to the outer limit and cannot move further. If you need to continue moving, please recalibrate the position of the servo.");
                return;
            }
            if (dpFocuser != nullptr)
            {
                indi_Client->moveFocuserSteps(dpFocuser, steps);
            }
            else if (focuserSdkReady)
            {
                SdkFocuserRelMoveParam p;
                p.outward = true;
                p.steps = steps;
                if (sdkFocuserExec && sdkFocuserExec->isRunning())
                {
                    const SdkDeviceHandle handleSnap = sdkFocuserHandle;
                    const uint64_t epochSnap = sdkFocuserOpEpoch.load(std::memory_order_relaxed);
                    sdkFocuserExec->post([handleSnap, p, this, epochSnap]() {
                        if (epochSnap != sdkFocuserOpEpoch.load(std::memory_order_relaxed))
                            return;
                        SdkCommand moveCmd{SdkCommandType::Custom, "MoveRelative", p};
                        // 直接通过设备句柄调用，无需指定驱动名称
                SdkManager::instance().callByHandle(handleSnap, moveCmd);
                    });
                }
            }
            TargetPosition = CurrentPosition+steps;
            emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
            *noChangeCount = 0; // 重置计数器（目标位置已更新，继续移动）
            return;
        }
        if (CurrentPosition == lastPosition){
            // Position hasn't changed - increment counter
            (*noChangeCount)++;
            
            // Check if we've reached the maximum physical limit
            if (CurrentPosition >= max)
            {
                // At physical limit - stop immediately
                FocuserControlStop();
                focusMoveToMaxorMinTimer->stop();
                emit wsThread->sendMessageToClient("FocusMoveToLimit:The current position has moved to the outer limit and cannot move further. If you need to continue moving, please recalibrate the position of the servo.");
                return;
            }
            
            // Not at limit - check if stuck (3 consecutive checks with no change)
            if (*noChangeCount >= 5)
            {
                // Focuser appears to be stuck - position hasn't changed for 3 seconds
                FocuserControlStop();
                focusMoveToMaxorMinTimer->stop();
                emit wsThread->sendMessageToClient("focusMoveFailed:focuser appears to be stuck - position not changing for 3 seconds and not at physical limit");
                return;
            }
            // Less than 3 checks - wait for more checks
            return;
        }
        
        // Position has changed - reset counter and update lastPosition
        *noChangeCount = 0;
        lastPosition = CurrentPosition; });
    focusMoveToMaxorMinTimer->start(1000);
    Logger::Log("focusMoveToMax | Started moving to maximum position: " + std::to_string(max), LogLevel::INFO, DeviceType::FOCUSER);
}

// [停用 2026-04-14] 旧自动电调校准实现：保留函数体用于历史回溯，当前不再通过命令入口触发。
void MainWindow::focusSetTravelRange()
{
    emit wsThread->sendMessageToClient("focusSetTravelRangeStarted");
    const bool focuserSdkReady =
        (systemdevicelist.system_devices.size() > 22 &&
         systemdevicelist.system_devices[22].isSDKConnect &&
         systemdevicelist.system_devices[22].isBind &&
         sdkFocuserHandle != nullptr);

    // 不改动判断条件：INDI 与 SDK 都不可用则失败
    if (dpFocuser == nullptr && !focuserSdkReady)
    {
        Logger::Log("focusSetTravelRange | focuser is not connected (both INDI and SDK are NULL)",
                    LogLevel::ERROR, DeviceType::FOCUSER);
        emit wsThread->sendMessageToClient("focusMoveFailed:focuser is not connected");
        return;
    }

    // 清理 move-to-max/min 计时器
    if (focusMoveToMaxorMinTimer != nullptr)
    {
        focusMoveToMaxorMinTimer->stop();
        focusMoveToMaxorMinTimer->deleteLater();
        focusMoveToMaxorMinTimer = nullptr;
    }

    // 停止电调
    FocuserControlStop();

    // --- SDK 异步位置更新：收敛重复逻辑 ---
    auto waitSdkPositionTaskDone = [this]() {
        int waitCount = 0;
        while (sdkFocuserPosTaskInFlight.load() && waitCount < 100)
        {
            QThread::msleep(50);
            QCoreApplication::processEvents(); // 确保回调执行
            waitCount++;
        }
        // 给回调落地一点余量
        QThread::msleep(200);
        QCoreApplication::processEvents();
    };

    auto updateSdkPositionOnce = [&]() {
        requestSdkFocuserPositionUpdate(true);
        waitSdkPositionTaskDone();
    };

    // 读取位置一次：SDK 先触发更新；INDI 直接读
    auto readPositionOnce = [&]() -> int {
        if (dpFocuser == nullptr && focuserSdkReady)
        {
            updateSdkPositionOnce();
        }
        return FocuserControl_getPosition();
    };

    // “等待稳定”：读取三次，每次间隔 1s，第三次视为稳定值（按你的要求）
    int stablePosition = 0;
    for (int i = 0; i < 3; ++i)
    {
        // 第一次读之前也给一点时间让 Stop 指令生效/惯性衰减
        if (i == 0)
        {
            sleep(1);
        }

        stablePosition = readPositionOnce();
        CurrentPosition = stablePosition;

        // 推送实时位置给前端（保留你原有行为风格）
        emit wsThread->sendMessageToClient(
            "FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));

        // 三次之间间隔 1 秒（最后一次之后不必再等）
        if (i < 2)
        {
            sleep(1);
        }
    }

    // 将稳定位置作为最大行程端点（保持原有语义：max=当前位置）
    focuserMaxPosition = stablePosition;

    emit wsThread->sendMessageToClient("focusSetTravelRangeSuccess");
    emit wsThread->sendMessageToClient(
        "FocuserLimit:" + QString::number(focuserMinPosition) + ":" + QString::number(focuserMaxPosition));
    emit wsThread->sendMessageToClient(
        "FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));

    Tools::saveParameter("Focuser", "focuserMaxPosition", QString::number(focuserMaxPosition));
    Tools::saveParameter("Focuser", "focuserMinPosition", QString::number(focuserMinPosition));

    Logger::Log("focusSetTravelRange | Calibration complete - Min: " + std::to_string(focuserMinPosition) +
                    ", Max: " + std::to_string(focuserMaxPosition) +
                    ", Current: " + std::to_string(CurrentPosition),
                LogLevel::INFO, DeviceType::FOCUSER);
}


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

void MainWindow::getFocuserParameters()
{
    QMap<QString, QString> parameters = Tools::readParameters("Focuser");
    if (parameters.contains("focuserMaxPosition") && parameters.contains("focuserMinPosition"))
    {
        focuserMaxPosition = parameters["focuserMaxPosition"].toInt();
        focuserMinPosition = parameters["focuserMinPosition"].toInt();
    }
    else
    {
        focuserMaxPosition = -1;
        focuserMinPosition = -1;
    }
    Logger::Log("Focuser Max Position: " + std::to_string(focuserMaxPosition) + ", Min Position: " + std::to_string(focuserMinPosition), LogLevel::INFO, DeviceType::MAIN);
    Logger::Log("Focuser Current Position: " + std::to_string(CurrentPosition), LogLevel::INFO, DeviceType::MAIN);

    // 启动/刷新时先同步一次当前位置与历史上下限，避免前端误触时看不到旧值
    const bool focuserAvailable = (dpFocuser != nullptr || isFocuserSDK());
    if (focuserAvailable)
    {
        int stablePosition = 0;
        if (tryReadStableFocuserPosition(stablePosition, 1800, 160, 2, 6))
        {
            CurrentPosition = stablePosition;
        }
        else
        {
            CurrentPosition = FocuserControl_getPosition();
            Logger::Log("getFocuserParameters | stable position read timeout, fallback position=" + std::to_string(CurrentPosition),
                        LogLevel::WARNING, DeviceType::FOCUSER);
        }
    }

    emit wsThread->sendMessageToClient("FocusPosition:" + QString::number(CurrentPosition) + ":" + QString::number(CurrentPosition));
    emit wsThread->sendMessageToClient("FocuserLimit:" + QString::number(focuserMinPosition) + ":" + QString::number(focuserMaxPosition));

    // 空程
    int emptyStep = parameters.contains("Backlash") ? parameters["Backlash"].toInt() : 0;
    autofocusBacklashCompensation = emptyStep;
    
    
    // 粗调分段数（总行程 / 分段数）
    int coarseDivisions = parameters.contains("coarseStepDivisions") ? parameters["coarseStepDivisions"].toInt() : 10;
    if (coarseDivisions <= 0)
    {
        coarseDivisions = 10;
    }
    autoFocusCoarseDivisions = coarseDivisions;
    
    // Steps per Click（步进按钮每次点击移动步数）
    int stepsPerClick = parameters.contains("StepsPerClick") ? parameters["StepsPerClick"].toInt() : 50;
    if (stepsPerClick <= 0)
    {
        stepsPerClick = 50;
    }

    // 单条命令下发（前端解析：FocuserParameters:min:max:backlash:coarseDivisions:stepsPerClick）
    emit wsThread->sendMessageToClient(
        "FocuserParameters:" + QString::number(focuserMinPosition) + ":" + QString::number(focuserMaxPosition) + ":" +
        QString::number(emptyStep) + ":" + QString::number(autoFocusCoarseDivisions) + ":" + QString::number(stepsPerClick));

    

    // 将当前 Focuser 串口列表与已保存串口下发给前端（若无保存则 savedPort 为空）
    sendSerialPortOptions("Focuser");

}

void MainWindow::getFocuserState()
{

    QString state = isAutoFocus ? "true" : "false";
    emit wsThread->sendMessageToClient("updateAutoFocuserState:" + state); // 状态更新

    // 获取当前步骤
    if (isAutoFocus && autoFocus != nullptr)
    {
        emit wsThread->sendMessageToClient("AutoFocusStarted:自动对焦已开始");
        autoFocus->getAutoFocusStep();
    }

    // 获取当前点和线数据
    if (isAutoFocus && autoFocus != nullptr)
    {
        autoFocus->getAutoFocusData();
    }
}
