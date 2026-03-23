#include "QHYFocuser.h"
#include "../SdkManager.h"

#include "../../Logger.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QMetaObject>
#include <QtCore/QThread>

#include <QtSerialPort/QSerialPort>

#include <memory>

// 注册驱动
REGISTER_SDK_DRIVER(SDK_DRIVER_NAME_INDI_QHY_FOCUSER, QhyFocuserDriver)

namespace
{
// 设备内部句柄（SdkDeviceHandle 实际指向该结构体）
struct QhyFocuserDevice
{
    // 注意：QSerialPort 是 QObject，必须在其所在线程中创建/使用/关闭。
    // 我们把它做成指针，并在工作线程（SdkSerialExecutor 线程）里延迟 new + open，
    // 以避免 "QSocketNotifier ... from another thread" 以及潜在段错误。
    QSerialPort* port{nullptr};
    QByteArray   rxBuffer;
    int          timeoutMs{3000};
    int          lastSpeed{0}; // SDK 协议无"读取速度"命令时，用作本地缓存
    bool         rebooted{false};

    // 打开参数（用于延迟打开）
    std::string  portName;
    int          baudRate{9600};

    // 最近一次从设备回读的值（便于上层快速获取/调试）
    SdkFocuserVersion   version;
    QhyFocuserTelemetry telemetry;
    int                 position{0};

    std::mutex ioMutex;
};

static QByteArray buildCmdJson(int cmdId, bool dir, int value)
{
    // 对齐 indi-qhy `qhy_focuser.cpp` 的 create_cmd()
    QJsonObject obj;
    obj.insert("cmd_id", cmdId);

    switch (cmdId)
    {
        case 1: // Get version
            break;
        case 2: // Relative Move
            obj.insert("dir", dir ? 1 : -1);
            obj.insert("step", value);
            break;
        case 3: // Abort
            break;
        case 4: // Temperature
            break;
        case 5: // Get Position
            break;
        case 6: // Absolute Move
            obj.insert("tar", value);
            break;
        case 7: // Set Reverse
            obj.insert("rev", value);
            break;
        case 11: // Set Position
            obj.insert("init_val", value);
            break;
        case 13: // Set Speed
            obj.insert("speed", value);
            break;
        case 16: // Set Hold
            obj.insert("ihold", 0);
            obj.insert("irun", 5);
            break;
        default:
            break;
    }

    QJsonDocument doc(obj);
    return doc.toJson(QJsonDocument::Compact);
}

static SdkResult makeErr(const std::string &msg)
{
    SdkResult r;
    r.success = false;
    r.message = msg;
    return r;
}

static bool writeAllWithTimeout(QSerialPort &port, const QByteArray &data, int timeoutMs, std::string &err);

// 仅发送命令，不等待回包（用于 Abort 等“需要尽快释放串口线程”的场景）
// 注意：后续其他命令在读取回包时会自动消费掉可能滞留在串口缓冲区里的 ACK。
static SdkResult sendNoWait(QhyFocuserDevice &dev, int cmdId, bool dir, int value, int writeTimeoutMs, const char* tag)
{
    std::lock_guard<std::mutex> lock(dev.ioMutex);
    std::string err;
    const QByteArray cmd = buildCmdJson(cmdId, dir, value);
    if (!dev.port)
        return makeErr("serial port is not opened");
    if (!writeAllWithTimeout(*dev.port, cmd, writeTimeoutMs, err))
        return makeErr(err);

    SdkResult r;
    r.success = true;
    r.message = std::string(tag) + " sent (no wait)";
    return r;
}

static bool writeAllWithTimeout(QSerialPort &port, const QByteArray &data, int timeoutMs, std::string &err)
{
    port.clear(QSerialPort::AllDirections);
    const qint64 written = port.write(data);
    if (written < 0)
    {
        err = "serial write failed: " + port.errorString().toStdString();
        return false;
    }
    if (!port.waitForBytesWritten(timeoutMs))
    {
        err = "serial waitForBytesWritten timeout: " + port.errorString().toStdString();
        return false;
    }
    return true;
}

// 优化：将读取分为两个函数
// 1. tryReadOneJsonObject: 非阻塞尝试读取，不等待
// 2. readOneJsonObject: 保持原有逻辑（用于向后兼容，但减少等待时间）

static bool tryReadOneJsonObject(QhyFocuserDevice &dev, QJsonObject &out, std::string &err)
{
    // 非阻塞尝试：只检查 buffer 中是否已有完整对象，不等待新数据
    int endIdx = dev.rxBuffer.indexOf('}');
    if (endIdx >= 0)
    {
        const QByteArray one = dev.rxBuffer.left(endIdx + 1);
        dev.rxBuffer.remove(0, endIdx + 1);

        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(one, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject())
        {
            // 解析失败：返回 false，但不设置错误（可能是脏数据）
            return false;
        }
        out = doc.object();
        return true;
    }
    
    // 尝试读取串口缓冲区中已有的数据（非阻塞）
    if (dev.port && dev.port->bytesAvailable() > 0)
    {
        dev.rxBuffer.append(dev.port->readAll());
        // 再次尝试解析
        endIdx = dev.rxBuffer.indexOf('}');
        if (endIdx >= 0)
        {
            const QByteArray one = dev.rxBuffer.left(endIdx + 1);
            dev.rxBuffer.remove(0, endIdx + 1);

            QJsonParseError pe;
            const QJsonDocument doc = QJsonDocument::fromJson(one, &pe);
            if (pe.error != QJsonParseError::NoError || !doc.isObject())
            {
                return false;
            }
            out = doc.object();
            return true;
        }
    }
    
    return false;  // 没有数据可读
}

static bool readOneJsonObject(QhyFocuserDevice &dev, QJsonObject &out, std::string &err)
{
    // 设备协议：每次回包为一个 JSON object，以 '}' 结尾
    // 参考 INDI 驱动的 tty_read_section(...,'}', TIMEOUT) 逻辑
    // 注意：此函数现在主要用于向后兼容，实际在 transact 中使用 tryReadOneJsonObject
    QElapsedTimer timer;
    timer.start();
    
    // 优化：减少锁持有时间，使用更短的等待间隔
    const int waitIntervalMs = 10;  // 从50ms减少到10ms，减少锁持有时间

    while (timer.elapsed() < dev.timeoutMs)
    {
        // 先尝试非阻塞读取
        if (tryReadOneJsonObject(dev, out, err))
            return true;

        // 如果没有数据，短暂等待（参考 INDI 的 tty_read_section 逻辑）
        if (!dev.port || !dev.port->waitForReadyRead(waitIntervalMs))
            continue;

        // 有数据到达，再次尝试读取
        dev.rxBuffer.append(dev.port->readAll());
    }

    err = "serial read timeout";
    return false;
}

// 针对移动命令的快速 transact：只等待 ACK，使用更短的超时时间
// 参考 INDI 实现：移动命令发送后只等待确认响应，不等待移动完成
static SdkResult transactMove(QhyFocuserDevice &dev, int cmdId, bool dir, int value)
{
    // 移动命令使用更短的超时时间（500ms），参考 INDI 的快速响应
    const int moveTimeoutMs = 500;
    
    // 先获取锁进行写入操作
    {
        std::lock_guard<std::mutex> lock(dev.ioMutex);
        std::string err;
        const QByteArray cmd = buildCmdJson(cmdId, dir, value);
        if (!dev.port)
            return makeErr("serial port is not opened");
        if (!writeAllWithTimeout(*dev.port, cmd, moveTimeoutMs, err))
            return makeErr(err);
    }  // 写入完成后立即释放锁

    // 只等待命令确认（ACK），不等待移动完成
    // 参考 INDI 的 ReadResponse：对于移动命令（2, 6），只要 idx 匹配就返回
    QElapsedTimer timer;
    timer.start();
    const int readAttemptIntervalMs = 10;
    
    while (timer.elapsed() < moveTimeoutMs)
    {
        QJsonObject obj;
        std::string err;
        bool readSuccess = false;
        
        // 在锁内进行读取尝试
        {
            std::lock_guard<std::mutex> lock(dev.ioMutex);
            
            if (tryReadOneJsonObject(dev, obj, err))
            {
                readSuccess = true;
            }
            else
            {
                if (dev.port && dev.port->waitForReadyRead(readAttemptIntervalMs))
                {
                    dev.rxBuffer.append(dev.port->readAll());
                    if (tryReadOneJsonObject(dev, obj, err))
                    {
                        readSuccess = true;
                    }
                }
            }
        }  // 读取尝试完成后立即释放锁
        
        if (!readSuccess)
        {
            QThread::msleep(5);
            continue;
        }
        
        // 处理响应：对于移动命令，只要 idx 匹配就返回（不检查其他字段）
        if (!obj.contains("idx"))
            continue;

        const int idx = obj.value("idx").toInt();
        if (idx == -1)
        {
            // reboot 响应
            {
                std::lock_guard<std::mutex> lock(dev.ioMutex);
                dev.rebooted = true;
            }
            SdkResult r;
            r.success = true;
            r.message = "device rebooted";
            return r;
        }

        // 对于移动命令（2, 6），只要 idx 匹配就立即返回（参考 INDI 实现）
        if (idx == cmdId)
        {
            SdkResult r;
            r.success = true;
            r.message = "Move command ack, idx=" + std::to_string(idx);
            return r;  // 立即返回，不等待移动完成
        }
    }

    return makeErr("serial read timeout (move command ack)");
}

static SdkResult transact(QhyFocuserDevice &dev, int cmdId, bool dir, int value, QJsonObject *respObj = nullptr)
{
    // 优化：使用更细粒度的锁控制，减少锁持有时间
    // 先获取锁进行写入操作
    {
        std::lock_guard<std::mutex> lock(dev.ioMutex);
        std::string err;
        const QByteArray cmd = buildCmdJson(cmdId, dir, value);
        if (!dev.port)
            return makeErr("serial port is not opened");
        if (!writeAllWithTimeout(*dev.port, cmd, dev.timeoutMs, err))
            return makeErr(err);
    }  // 写入完成后立即释放锁

    // 某些命令回包会多次出现，我们按 indi 逻辑：读到"匹配 idx 的回包且字段完整"为止
    // 参考 INDI 驱动的 ReadResponse：循环读取直到找到匹配的响应
    // 优化：每次读取尝试后释放锁，避免长时间阻塞其他命令
    QElapsedTimer timer;
    timer.start();
    const int readAttemptIntervalMs = 10;  // 每次读取尝试之间的间隔（释放锁的时间，参考 INDI 的短等待）
    
    while (timer.elapsed() < dev.timeoutMs)
    {
        QJsonObject obj;
        std::string err;
        bool readSuccess = false;
        
        // 在锁内进行读取尝试（参考 INDI 的 tty_read_section 逻辑）
        {
            std::lock_guard<std::mutex> lock(dev.ioMutex);
            
            // 先尝试从 buffer 中读取（非阻塞）
            if (tryReadOneJsonObject(dev, obj, err))
            {
                readSuccess = true;
            }
            else
            {
                // 如果没有完整对象，尝试等待新数据（短时间等待，类似 INDI 的 tty_read_section）
                // 使用很短的超时时间，避免长时间持有锁
                if (dev.port && dev.port->waitForReadyRead(readAttemptIntervalMs))
                {
                    dev.rxBuffer.append(dev.port->readAll());
                    // 再次尝试读取
                    if (tryReadOneJsonObject(dev, obj, err))
                    {
                        readSuccess = true;
                    }
                }
            }
        }  // 读取尝试完成后立即释放锁（关键：避免长时间持有锁）
        
        if (!readSuccess)
        {
            // 读取失败，在锁外等待一小段时间后继续尝试（不阻塞其他命令）
            // 这样可以给其他命令执行的机会
            QThread::msleep(5);  // 很短的等待，避免 CPU 占用过高
            continue;
        }
        
        // 成功读取到数据，处理响应（不需要锁）
        if (!obj.contains("idx"))
            continue;

        const int idx = obj.value("idx").toInt();
        if (idx == -1)
        {
            // 需要更新 dev.rebooted，需要锁保护
            {
                std::lock_guard<std::mutex> lock(dev.ioMutex);
                dev.rebooted = true;
            }
            if (respObj) *respObj = obj;
            // reboot 回包也视为一次有效响应
            SdkResult r;
            r.success = true;
            r.message = "device rebooted";
            return r;
        }

        if (idx != cmdId)
            continue;

        // 根据不同命令判断字段完整性
        if (idx == 1)
        {
            if (obj.contains("version") && obj.contains("bv"))
            {
                SdkFocuserVersion version;
                version.version = obj.value("version").toInt();
                version.boardVersion = obj.value("bv").toInt();
                if (obj.contains("id"))
                    version.id = obj.value("id").toString().toStdString();
                
                // 更新设备状态需要锁保护
                {
                    std::lock_guard<std::mutex> lock(dev.ioMutex);
                    dev.version = version;
                }
                
                if (respObj) *respObj = obj;
                SdkResult r;
                r.success = true;
                r.message = "GetVersion success";
                r.payload = version;
                return r;
            }
            continue;
        }
        else if (idx == 4)
        {
            if (obj.contains("o_t") && obj.contains("c_t") && obj.contains("c_r"))
            {
                const double o_t = obj.value("o_t").toDouble();
                const double c_t = obj.value("c_t").toDouble();
                const double c_r = obj.value("c_r").toDouble();
                
                QhyFocuserTelemetry telemetry;
                telemetry.outTempC = o_t / 1000.0;
                telemetry.chipTempC = c_t / 1000.0;
                telemetry.voltageV = c_r / 10.0;
                
                // 更新设备状态需要锁保护
                {
                    std::lock_guard<std::mutex> lock(dev.ioMutex);
                    dev.telemetry = telemetry;
                }
                
                if (respObj) *respObj = obj;
                SdkResult r;
                r.success = true;
                r.message = "GetTemperature success";
                r.payload = telemetry;
                return r;
            }
            continue;
        }
        else if (idx == 5)
        {
            if (obj.contains("pos"))
            {
                int position = obj.value("pos").toInt();
                
                // 更新设备状态需要锁保护
                {
                    std::lock_guard<std::mutex> lock(dev.ioMutex);
                    dev.position = position;
                }
                
                if (respObj) *respObj = obj;
                SdkResult r;
                r.success = true;
                r.message = "GetPosition success";
                r.payload = position;
                return r;
            }
            continue;
        }
        else
        {
            // 2/3/6/7/11/13/16：indi 侧只要求 idx 匹配即可
            if (respObj) *respObj = obj;
            SdkResult r;
            r.success = true;
            r.message = "Command ack, idx=" + std::to_string(idx);
            return r;
        }
    }

    return makeErr("serial read timeout (no valid response)");
}
} // namespace



QhyFocuserDriver::QhyFocuserDriver() = default;
QhyFocuserDriver::~QhyFocuserDriver() = default;

std::vector<std::string> QhyFocuserDriver::driverNames() const
{
    return {SDK_DRIVER_NAME_INDI_QHY_FOCUSER , SDK_DRIVER_NAME_QHY_FOCUSER};
}

std::vector<SdkCommandInfo> QhyFocuserDriver::commandList() const
{
    return {
        {"Handshake",      "握手：读取版本 + 读取温度/电压 + 必要时下发 Hold 参数"},
        {"GetVersion",     "读取电调版本信息（cmd_id=1）"},
        {"GetTemperature", "读取温度/电压（cmd_id=4）"},
        {"GetPosition",    "读取当前位置（cmd_id=5）"},
        {"MoveRelative",   "相对移动（cmd_id=2），payload=SdkFocuserRelMoveParam"},
        {"MoveAbsolute",   "绝对移动（cmd_id=6），payload=int target"},
        {"Abort",          "停止移动（cmd_id=3）"},
        {"SetReverse",     "反向（cmd_id=7），payload=bool 或 int(0/1)"},
        {"SyncPosition",   "同步当前位置（cmd_id=11），payload=int position"},
        {"SetSpeed",       "设置速度（cmd_id=13），payload=int speed(0..8，0最快)"},
        {"GetSpeed",       "获取速度（本地缓存），返回 int"},
    };
}

SdkDeviceCapabilities QhyFocuserDriver::capabilities() const
{
    SdkDeviceCapabilities caps;
    
    // 设置设备类型为电调
    caps.deviceType = SdkDeviceType::Focuser;
    
    // 将支持的命令名填入能力列表，供注册表判定"驱动有效"
    auto cmds = commandList();
    caps.supportedCommands.reserve(cmds.size());
    for (const auto &cmd : cmds)
    {
        caps.supportedCommands.push_back(cmd.name);
    }
    // 默认单设备，不设置 multi-device / threading 特性
    return caps;
}

// 确保串口在工作线程中打开（QSerialPort 必须在创建它的线程中使用）
// 返回错误信息（如果失败）
static std::string ensurePortOpened(QhyFocuserDevice &dev)
{
    std::lock_guard<std::mutex> lock(dev.ioMutex);

    // 已存在且已打开
    if (dev.port && dev.port->isOpen())
        return "";

    // 若存在但没打开，先关掉并清理（防止状态脏）
    if (dev.port && dev.port->isOpen())
        dev.port->close();

    // 在当前线程（即 SdkSerialExecutor 工作线程）创建 QSerialPort
    if (!dev.port)
        dev.port = new QSerialPort();

    // 配置并打开串口
    dev.port->setPortName(QString::fromStdString(dev.portName));
    dev.port->setBaudRate(dev.baudRate);
    dev.port->setDataBits(QSerialPort::Data8);
    dev.port->setParity(QSerialPort::NoParity);
    dev.port->setStopBits(QSerialPort::OneStop);
    dev.port->setFlowControl(QSerialPort::NoFlowControl);

    if (!dev.port->open(QIODevice::ReadWrite))
    {
        const std::string e = "Open serial failed: " + dev.port->errorString().toStdString();
        // 打开失败：释放对象，避免后续误用
        dev.port->deleteLater();
        dev.port = nullptr;
        return e;
    }

    return "";  // 成功，无错误
}

SdkResult QhyFocuserDriver::openDevice(const std::any &openParam)
{
    // 统一在串口执行线程中创建/管理设备结构体（与后续 execute/close 保持同线程）
    return m_exec.postAndWait<SdkResult>([this, openParam]() -> SdkResult {
        SdkResult r;
        QhyFocuserOpenParam p;
        if (!extractParam<QhyFocuserOpenParam>(openParam, p, r, "QhyFocuserOpenParam"))
            return r;

        auto dev = std::make_unique<QhyFocuserDevice>();
        dev->timeoutMs = (p.timeoutMs > 0) ? p.timeoutMs : 3000;

        // 保存打开参数，延迟到工作线程中打开串口
        dev->portName = p.port;
        dev->baudRate = p.baudRate;

        r.success = true;
        r.message = "Open serial deferred (worker thread): " + p.port;
        r.payload = static_cast<SdkDeviceHandle>(dev.release());
        return r;
    });
}

SdkResult QhyFocuserDriver::closeDevice(SdkDeviceHandle handle)
{
    // 强制在串口执行线程中关闭/销毁，避免 QSerialPort 跨线程
    return m_exec.postAndWait<SdkResult>([handle]() -> SdkResult {
        if (!handle)
            return makeErr("closeDevice: handle is null");

        auto *dev = static_cast<QhyFocuserDevice *>(handle);

        // 在同线程安全关闭与销毁
        if (dev->port)
        {
            if (dev->port->isOpen())
                dev->port->close();
            delete dev->port;
            dev->port = nullptr;
        }
        dev->rxBuffer.clear();

        delete dev;

        SdkResult r;
        r.success = true;
        r.message = "Close focuser serial success";
        return r;
    });
}

SdkResult QhyFocuserDriver::execute(SdkDeviceHandle handle, const SdkCommand &cmd)
{
    // 所有串口收发必须在同一线程里串行执行，避免 QSerialPort/QSocketNotifier 跨线程崩溃
    return m_exec.postAndWait<SdkResult>([this, handle, cmd]() -> SdkResult {
        if (!handle)
            return makeErr("execute: handle is null");

        auto *dev = static_cast<QhyFocuserDevice *>(handle);

        // 确保串口在工作线程中打开（QSerialPort 必须在创建它的线程中使用）
        std::string openErr = ensurePortOpened(*dev);
        if (!openErr.empty())
        {
            return makeErr(openErr);
        }

        if (cmd.type != SdkCommandType::Custom)
            return makeErr("Unsupported command type for QHY focuser");

        const std::string &name = cmd.name;

        if (name == "Handshake")
        {
            auto v = transact(*dev, 1, true, 0);
            if (!v.success)
                return v;

            auto t = transact(*dev, 4, true, 0);
            if (!t.success)
                return t;

            // 对齐 indi 驱动：若电压为 0，则下发 Hold 设置（cmd_id=16）
            if (dev->telemetry.voltageV <= 0.0)
            {
                auto h = transact(*dev, 16, true, 0);
                if (!h.success)
                    return h;
            }

            SdkResult r;
            r.success = true;
            r.message = "Handshake success";
            r.payload = dev->version;
            return r;
        }

        if (name == "GetVersion")
            return transact(*dev, 1, true, 0);

        if (name == "GetTemperature")
            return transact(*dev, 4, true, 0);

        if (name == "GetPosition")
            return transact(*dev, 5, true, 0);

        if (name == "MoveRelative")
        {
            SdkFocuserRelMoveParam p;
            SdkResult r;
            if (!extractParam<SdkFocuserRelMoveParam>(cmd.payload, p, r, "SdkFocuserRelMoveParam"))
                return r;

            if (p.steps < 0) p.steps = -p.steps;
            // 使用快速 transact：只等待 ACK，不等待移动完成（参考 INDI 实现）
            return transactMove(*dev, 2, p.outward, p.steps);
        }

        if (name == "MoveAbsolute")
        {
            int target = 0;
            SdkResult r;
            if (!extractParam<int>(cmd.payload, target, r, "target"))
                return r;
            // 使用快速 transact：只等待 ACK，不等待移动完成（参考 INDI 实现）
            return transactMove(*dev, 6, true, target);
        }

        if (name == "Abort")
        {
            // 参考 INDI：Abort 只需尽快下发并释放线程，不应阻塞后续 Move。
            // 写入成功即视为成功；ACK 会在后续读操作中被消费。
            return sendNoWait(*dev, 3, false, 0, 500, "Abort");
        }

        if (name == "SetReverse")
        {
            int v = 0;
            if (cmd.payload.type() == typeid(bool))
                v = std::any_cast<bool>(cmd.payload) ? 1 : 0;
            else if (cmd.payload.type() == typeid(int))
                v = std::any_cast<int>(cmd.payload);
            else
                return makeErr("SetReverse payload must be bool or int");

            return transact(*dev, 7, true, v);
        }

        if (name == "SyncPosition")
        {
            int pos = 0;
            SdkResult r;
            if (!extractParam<int>(cmd.payload, pos, r, "position"))
                return r;
            return transact(*dev, 11, true, pos);
        }

        if (name == "SetSpeed")
        {
            int speed = 0;
            SdkResult r;
            if (!extractParam<int>(cmd.payload, speed, r, "speed"))
                return r;

            // 对齐 indi：UpdateSetSpeed 使用 create_cmd(13, ..., 4 - speed)
            const int deviceSpeed = 4 - speed;
            auto res = transact(*dev, 13, true, deviceSpeed);
            if (res.success)
                dev->lastSpeed = speed;
            return res;
        }

        if (name == "GetSpeed")
        {
            SdkResult r;
            r.success = true;
            r.message = "GetSpeed (cached)";
            r.payload = dev->lastSpeed;
            return r;
        }

        return makeErr("Unsupported QHY focuser command: " + cmd.name);
    });
}



