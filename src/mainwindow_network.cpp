#include "mainwindow.h"

#include <QRegularExpression>
#include <QTemporaryFile>

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

} // namespace

void MainWindow::runSudoAsync(const QString &program, const QStringList &args,
                              const std::function<void(int, const QString &, const QString &)> &onDone)
{
    QProcess *p = new QProcess(this);
    p->setProgram("sudo");
    QStringList a;
    a << "-n";
    a << program;
    a << args;
    p->setArguments(a);

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
                         [finish](int codeHostapd, const QString &outHostapd, const QString &errHostapd) {
                             finish(codeHostapd, (errHostapd.isEmpty() ? outHostapd : errHostapd).trimmed());
                         });
            return;
        }

        runSudoAsync("/usr/bin/systemctl", {"restart", QString::fromUtf8(kPreferredWpaService)},
                     [finish](int codeWpa, const QString &outWpa, const QString &errWpa) {
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
                         runSudoAsync("/usr/bin/nmcli",
                                      {"con", "add", "type", "wifi", "ifname", "wlan0", "con-name", name, "ssid", ssid},
                                      [name, psk, finishOk, finishFail](int codeAdd, const QString &outAdd, const QString &errAdd) {
                                          if (codeAdd != 0) {
                                              finishFail(errAdd.isEmpty() ? outAdd : errAdd);
                                              return;
                                          }
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

    const QString HostpotName = getHotspotName();
    Logger::Log("editHotspotName | New Hotspot Name:" + HostpotName.toStdString(), LogLevel::INFO, DeviceType::MAIN);

    if (exitCode == 0 && HostpotName == newName)
    {
        emit wsThread->sendMessageToClient("EditHotspotNameSuccess");
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

    const QString connectionName = QString::fromUtf8(kLegacyHotspotConnectionName);

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

    int delayMs = std::max(0, delaySeconds) * 1000;

    QTimer::singleShot(delayMs, this, [connectionName]() {
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
