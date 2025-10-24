#include "websocketclient.h"
#include <QDebug>
#include <QSslError>
#include <QSslConfiguration>
#include <QMetaObject>


WebSocketClient::WebSocketClient(const QUrl &httpUrl, const QUrl &httpsUrl, QObject *parent) :
    QObject(parent), httpUrl(httpUrl), httpsUrl(httpsUrl)
{
    // 初始化状态变量
    isHttpsConnected = false;
    isHttpConnected = false;
    isNetworkConnected = networkManager.isOnline();
    
    if (httpsUrl.isValid()) {
        // 配置SSL
        QSslConfiguration sslConfig = httpsWebSocket.sslConfiguration();
        sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);  // 禁用证书验证
        httpsWebSocket.setSslConfiguration(sslConfig);
        
        connect(&httpsWebSocket, &QWebSocket::connected, this, &WebSocketClient::onHttpsConnected);
        connect(&httpsWebSocket, &QWebSocket::disconnected, this, &WebSocketClient::onHttpsDisconnected);
        connect(&httpsWebSocket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
                this, &WebSocketClient::onError);
        connect(&httpsWebSocket, &QWebSocket::sslErrors, this, &WebSocketClient::onSslErrors);
        connect(&httpsWebSocket, &QWebSocket::pong, this, &WebSocketClient::onPongReceived);
        
        httpsWebSocket.open(httpsUrl);
    }
    
    if (httpUrl.isValid()) {
        connect(&httpWebSocket, &QWebSocket::connected, this, &WebSocketClient::onHttpConnected);
        connect(&httpWebSocket, &QWebSocket::disconnected, this, &WebSocketClient::onHttpDisconnected);
        connect(&httpWebSocket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
                this, &WebSocketClient::onError);
        connect(&httpWebSocket, &QWebSocket::pong, this, &WebSocketClient::onPongReceived);
        httpWebSocket.open(httpUrl);
    }

    // 初始化自动重连定时器（指数退避）
    reconnectTimer.setInterval(currentReconnectIntervalMs);
    connect(&reconnectTimer, &QTimer::timeout, this, &WebSocketClient::reconnect);

    // 连接网络状态变化信号槽
    connect(&networkManager, &QNetworkConfigurationManager::onlineStateChanged, this, &WebSocketClient::onNetworkStateChanged);
    
    // 心跳定时器
    connect(&heartbeatTimer, &QTimer::timeout, this, &WebSocketClient::onHeartbeatTimeout);
    heartbeatTimer.start(heartbeatIntervalMs);
    lastPongTimer.start();

    qInfo() << "WebSocketClient initialized with network state:" << (isNetworkConnected ? "ONLINE" : "OFFLINE");
}

void WebSocketClient::onHttpsConnected()
{
    qInfo() << "HTTPS WebSocket connected successfully";
    emit connectionStatusChanged(true, "HTTPS Connected successfully");
    
    if (httpsWebSocket.isValid()) {
        connect(&httpsWebSocket, &QWebSocket::textMessageReceived,
                this, &WebSocketClient::onTextMessageReceived);
        isHttpsConnected = true;
    }

    // 修复：简化条件判断逻辑
    if (isHttpsConnected && (httpUrl.isEmpty() || isHttpConnected)) {
        qInfo() << "All required websockets are connected";
        reconnectTimer.stop();
        reconnectAttempts = 0;
        currentReconnectIntervalMs = 1000;
        missedPongs = 0;
        lastPongTimer.restart();
    } else {
        qWarning() << "Waiting for other connections...";
    }
}

void WebSocketClient::onHttpsDisconnected()
{
    qWarning() << "HTTPS WebSocket disconnected";
    emit connectionStatusChanged(false, "HTTPS Disconnected");

    // 断开接收消息的信号与槽
    disconnect(&httpsWebSocket, &QWebSocket::textMessageReceived,
               this, &WebSocketClient::onTextMessageReceived);

    // 修复：总是设置状态为false并启动重连
    isHttpsConnected = false;
    
    // 只要配置了HTTPS URL就尝试重连
    if (httpsUrl.isValid()) {
        qInfo() << "Starting HTTPS reconnect timer...";
        if (!reconnectTimer.isActive()) {
            reconnectTimer.start();
        }
    }
}

void WebSocketClient::onHttpConnected()
{
    qInfo() << "HTTP WebSocket connected successfully";
    emit connectionStatusChanged(true, "HTTP Connected successfully");
    
    if (httpWebSocket.isValid()) {
        connect(&httpWebSocket, &QWebSocket::textMessageReceived,
                this, &WebSocketClient::onTextMessageReceived);
        isHttpConnected = true;
    }
    
    // 修复：简化条件判断逻辑
    if (isHttpConnected && (httpsUrl.isEmpty() || isHttpsConnected)) {
        qInfo() << "All required websockets are connected";
        reconnectTimer.stop();
        reconnectAttempts = 0;
        currentReconnectIntervalMs = 1000;
        missedPongs = 0;
        lastPongTimer.restart();
    } else {
        qWarning() << "Waiting for other connections...";
    }
}

void WebSocketClient::onHttpDisconnected()
{
    qWarning() << "HTTP WebSocket disconnected";
    emit connectionStatusChanged(false, "HTTP Disconnected");

    // 断开接收消息的信号与槽
    disconnect(&httpWebSocket, &QWebSocket::textMessageReceived,
               this, &WebSocketClient::onTextMessageReceived);

    // 修复：总是设置状态为false并启动重连
    isHttpConnected = false;
    
    // 只要配置了HTTP URL就尝试重连
    if (httpUrl.isValid()) {
        qInfo() << "Starting HTTP reconnect timer...";
        if (!reconnectTimer.isActive()) {
            reconnectTimer.start();
        }
    }
}

void WebSocketClient::onError(QAbstractSocket::SocketError error)
{
    QString errorString = getErrorString(error);
    qWarning() << "WebSocket error:" << errorString;
    emit connectionError(errorString);
    emit connectionStatusChanged(false, "Error: " + errorString);
}

void WebSocketClient::onSslErrors(const QList<QSslError> &errors)
{
    QString errorString;
    for (const QSslError &error : errors) {
        switch (error.error()) {
            case QSslError::HostNameMismatch:
                errorString += "主机名与证书不匹配\n";
                break;
            case QSslError::SelfSignedCertificate:
                errorString += "证书是自签名的\n";
                break;
            case QSslError::CertificateUntrusted:
                errorString += "证书不受信任\n";
                break;
            case QSslError::CertificateNotYetValid:
                errorString += "证书尚未生效\n";
                break;
            case QSslError::CertificateExpired:
                errorString += "证书已过期\n";
                break;
            case QSslError::UnableToGetLocalIssuerCertificate:
                errorString += "无法获取本地颁发者证书\n";
                break;
            case QSslError::UnableToVerifyFirstCertificate:
                errorString += "无法验证第一个证书\n";
                break;
            case QSslError::UnableToDecryptCertificateSignature:
                errorString += "无法解密证书签名\n";
                break;
            case QSslError::UnableToDecodeIssuerPublicKey:
                errorString += "无法解码颁发者公钥\n";
                break;
            case QSslError::CertificateSignatureFailed:
                errorString += "证书签名验证失败\n";
                break;
            case QSslError::CertificateRevoked:
                errorString += "证书已被撤销\n";
                break;
            case QSslError::InvalidCaCertificate:
                errorString += "无效的CA证书\n";
                break;
            case QSslError::PathLengthExceeded:
                errorString += "证书路径长度超出限制\n";
                break;
            case QSslError::InvalidPurpose:
                errorString += "证书用途无效\n";
                break;
            case QSslError::CertificateRejected:
                errorString += "证书被拒绝\n";
                break;
            case QSslError::SubjectIssuerMismatch:
                errorString += "证书主题与颁发者不匹配\n";
                break;
            case QSslError::AuthorityIssuerSerialNumberMismatch:
                errorString += "证书颁发者序列号不匹配\n";
                break;
            case QSslError::NoPeerCertificate:
                errorString += "没有对等证书\n";
                break;
            case QSslError::NoSslSupport:
                errorString += "不支持SSL\n";
                break;
            default:
                errorString += "SSL错误: " + error.errorString() + "\n";
                break;
        }
    }
    
    qWarning() << "SSL Errors:" << errorString;
    emit connectionError("SSL错误: " + errorString);
    emit connectionStatusChanged(false, "SSL错误: " + errorString);
    
    // 由于我们设置了VerifyNone，这里可以继续连接
    httpsWebSocket.ignoreSslErrors();
}

QString WebSocketClient::getErrorString(QAbstractSocket::SocketError error)
{
    switch (error) {
        case QAbstractSocket::ConnectionRefusedError:
            return "连接被拒绝";
        case QAbstractSocket::RemoteHostClosedError:
            return "远程主机关闭";
        case QAbstractSocket::HostNotFoundError:
            return "主机未找到";
        case QAbstractSocket::SocketAccessError:
            return "套接字访问错误";
        case QAbstractSocket::SocketResourceError:
            return "套接字资源错误";
        case QAbstractSocket::SocketTimeoutError:
            return "套接字超时";
        case QAbstractSocket::DatagramTooLargeError:
            return "数据报文太大";
        case QAbstractSocket::NetworkError:
            return "网络错误";
        case QAbstractSocket::AddressInUseError:
            return "地址已被使用";
        case QAbstractSocket::SocketAddressNotAvailableError:
            return "套接字地址不可用";
        case QAbstractSocket::UnsupportedSocketOperationError:
            return "不支持的套接字操作";
        case QAbstractSocket::UnknownSocketError:
            return "未知套接字错误";
        case QAbstractSocket::TemporaryError:
            return "临时错误";
        case QAbstractSocket::SslHandshakeFailedError:
            return "SSL握手失败";
        default:
            return "未知错误 (错误码: " + QString::number(error) + ")";
    }
}

void WebSocketClient::reconnect()
{
    qInfo() << "Attempting to reconnect WebSockets...";
    
    // 检查网络状态
    if (!isNetworkConnected) {
        qWarning() << "Network is offline, skipping reconnect attempt";
        return;
    }
    
    // 重连HTTPS WebSocket
    if (httpsUrl.isValid() && !isHttpsConnected) {
        qInfo() << "Reconnecting to HTTPS WebSocket server...";
        
        // 确保先断开现有连接
        if (httpsWebSocket.state() != QAbstractSocket::UnconnectedState) {
            httpsWebSocket.close();
        }
        
        // 等待完全断开后重新连接
        QTimer::singleShot(100, [this]() {
            httpsWebSocket.open(httpsUrl);
        });
    }
    
    // 重连HTTP WebSocket
    if (httpUrl.isValid() && !isHttpConnected) {
        qInfo() << "Reconnecting to HTTP WebSocket server...";
        
        // 确保先断开现有连接
        if (httpWebSocket.state() != QAbstractSocket::UnconnectedState) {
            httpWebSocket.close();
        }
        
        // 等待完全断开后重新连接
        QTimer::singleShot(100, [this]() {
            httpWebSocket.open(httpUrl);
        });
    }
    
    // 如果两个连接都已连接，停止重连定时器
    if ((!httpsUrl.isValid() || isHttpsConnected) && 
        (!httpUrl.isValid() || isHttpConnected)) {
        qInfo() << "All connections established, stopping reconnect timer";
        reconnectTimer.stop();
        reconnectAttempts = 0;
        currentReconnectIntervalMs = 1000;
        missedPongs = 0;
        lastPongTimer.restart();
        flushPending();
        return;
    }

    // 增加退避并更新重连频率
    reconnectAttempts++;
    currentReconnectIntervalMs = qMin(currentReconnectIntervalMs * 2, maxReconnectIntervalMs);
    reconnectTimer.setInterval(currentReconnectIntervalMs);
}

void WebSocketClient::onNetworkStateChanged(bool isOnline)
{
    qInfo() << "Network state changed to:" << (isOnline ? "ONLINE" : "OFFLINE");
    isNetworkConnected = isOnline;
    
    if (isOnline) {
        // 网络恢复时检查连接状态并启动重连
        bool needReconnect = false;
        
        if (httpsUrl.isValid() && !isHttpsConnected) {
            qInfo() << "HTTPS connection lost, will attempt reconnect";
            needReconnect = true;
        }
        
        if (httpUrl.isValid() && !isHttpConnected) {
            qInfo() << "HTTP connection lost, will attempt reconnect";
            needReconnect = true;
        }
        
        if (needReconnect) {
            // 立即尝试重连一次
            QTimer::singleShot(1000, this, &WebSocketClient::reconnect);
            
            // 启动定时重连
            if (!reconnectTimer.isActive()) {
                reconnectTimer.start();
            }
        }
    } else {
        // 网络断开时停止重连尝试
        if (reconnectTimer.isActive()) {
            qInfo() << "Network offline, stopping reconnect timer";
            reconnectTimer.stop();
        }
    }
}

void WebSocketClient::onTextMessageReceived(QString message)
{
    // qDebug() << "Message received:" << message;
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    QJsonObject messageObj = doc.object();
    if (messageObj["type"].toString() == "Vue_Command" || messageObj["type"].toString() == "Process_Command" )
    {
        // 处理命令
        emit messageReceived(messageObj["message"].toString());
        
        // qDebug() << "Message received:" << messageObj["type"].toString() << " " << messageObj["message"].toString();

        // 发送确认消息
        sendAcknowledgment(messageObj["msgid"].toString());
    }
    else if (messageObj["type"].toString() == "Server_msg")
    {
        emit messageReceived(messageObj["message"].toString());
        // qDebug() << "Message received:" << messageObj["type"].toString() << " " << messageObj["message"].toString();
    }
    else if (messageObj["type"].toString() == "APP_msg")
    {
        QString time = messageObj["time"].toString();
        QString lat = QString::number(messageObj["lat"].toDouble());
        QString lon = QString::number(messageObj["lon"].toDouble());
        QString language = messageObj["language"].toString();
        QString wifiName = messageObj["wifiname"].toString();
        QString info = "localMessage:" + lat + ":" + lon + ":" + language + ":" + wifiName;
        emit messageReceived(info);
        // qDebug() << "发送app信息 received:" << info;
    }
    else
    {
        qDebug() << "Message received is undefined type:" << messageObj["type"].toString() << " " << messageObj["message"].toString();
    }
}

void WebSocketClient::sendAcknowledgment(QString messageID)
{
    QString utf8Message = messageID.toUtf8();

    QJsonObject messageObj;

    messageObj["type"] = "QT_Confirm";
    messageObj["msgid"] = utf8Message;
    QByteArray payload = QJsonDocument(messageObj).toJson();
    if (!isHttpsConnected && !isHttpConnected) {
        if (pendingMessages.size() >= maxPendingMessages) pendingMessages.dequeue();
        pendingMessages.enqueue(payload);
        return;
    }
    if (isHttpsConnected) httpsWebSocket.sendTextMessage(QString::fromUtf8(payload));
    if (isHttpConnected) httpWebSocket.sendTextMessage(QString::fromUtf8(payload));
}

void WebSocketClient::sendProcessCommandReturn(QString message)
{
    QString utf8Message = message.toUtf8();

    QJsonObject messageObj;
    messageObj["type"] = "Process_Command_Return";
    messageObj["message"] = utf8Message;
    QByteArray payload = QJsonDocument(messageObj).toJson();
    if (!isHttpsConnected && !isHttpConnected) {
        if (pendingMessages.size() >= maxPendingMessages) pendingMessages.dequeue();
        pendingMessages.enqueue(payload);
        return;
    }
    if (isHttpsConnected) httpsWebSocket.sendTextMessage(QString::fromUtf8(payload));
    if (isHttpConnected) httpWebSocket.sendTextMessage(QString::fromUtf8(payload));
}

void WebSocketClient::messageSend(QString message)
{
    QString utf8Message = message.toUtf8(); // 将QString转换为UTF-8编码的QString

    QJsonObject messageObj;

    messageObj["message"] = utf8Message;
    messageObj["type"] = "QT_Return";
    QByteArray payload = QJsonDocument(messageObj).toJson();
    if (!isHttpsConnected && !isHttpConnected) {
        if (pendingMessages.size() >= maxPendingMessages) pendingMessages.dequeue();
        pendingMessages.enqueue(payload);
        return;
    }
    if (isHttpsConnected) httpsWebSocket.sendTextMessage(QString::fromUtf8(payload));
    if (isHttpConnected) httpWebSocket.sendTextMessage(QString::fromUtf8(payload));
}

void WebSocketClient::onHeartbeatTimeout()
{
    // 仅在至少一个连接可用时发送 ping
    bool anyConnected = (isHttpsConnected && httpsWebSocket.state() == QAbstractSocket::ConnectedState) ||
                        (isHttpConnected && httpWebSocket.state() == QAbstractSocket::ConnectedState);
    if (anyConnected) {
        if (isHttpsConnected) httpsWebSocket.ping("h");
        if (isHttpConnected) httpWebSocket.ping("h");

        // 判定是否长时间未收到PONG
        if (lastPongTimer.isValid() && lastPongTimer.elapsed() > heartbeatIntervalMs * pongTimeoutMultiplier) {
            missedPongs++;
            qWarning() << "Missed pong #" << missedPongs << ", forcing reconnect if threshold exceeded";
            if (missedPongs >= 2) {
                // 强制重连
                if (isHttpsConnected) httpsWebSocket.close();
                if (isHttpConnected) httpWebSocket.close();
                if (!reconnectTimer.isActive()) reconnectTimer.start();
            }
        }
    }
}

void WebSocketClient::onPongReceived(quint64 /*elapsedTime*/, const QByteArray &/*payload*/)
{
    missedPongs = 0;
    lastPongTimer.restart();
}

void WebSocketClient::flushPending()
{
    if (pendingMessages.isEmpty()) return;
    if (!isHttpsConnected && !isHttpConnected) return;
    int count = pendingMessages.size();
    while (!pendingMessages.isEmpty()) {
        const QByteArray payload = pendingMessages.dequeue();
        if (isHttpsConnected) httpsWebSocket.sendTextMessage(QString::fromUtf8(payload));
        if (isHttpConnected) httpWebSocket.sendTextMessage(QString::fromUtf8(payload));
    }
    qInfo() << "Flushed pending websocket messages:" << count;
}

void WebSocketClient::stop()
{
    if (reconnectTimer.isActive()) reconnectTimer.stop();
    if (heartbeatTimer.isActive()) heartbeatTimer.stop();
    disconnect(&httpsWebSocket, nullptr, this, nullptr);
    disconnect(&httpWebSocket, nullptr, this, nullptr);
    if (httpsWebSocket.state() != QAbstractSocket::UnconnectedState) httpsWebSocket.close();
    if (httpWebSocket.state() != QAbstractSocket::UnconnectedState) httpWebSocket.close();
}
