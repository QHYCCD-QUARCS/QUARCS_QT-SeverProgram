#include "websocketclient.h"
#include <QDebug>
#include <QSslError>
#include <QSslConfiguration>


WebSocketClient::WebSocketClient(const QUrl &httpUrl, const QUrl &httpsUrl, QObject *parent) :
    QObject(parent), httpUrl(httpUrl), httpsUrl(httpsUrl)
{
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
        
        httpsWebSocket.open(httpsUrl);
    }
    if (httpUrl.isValid()) {
        connect(&httpWebSocket, &QWebSocket::connected, this, &WebSocketClient::onHttpConnected);
        connect(&httpWebSocket, &QWebSocket::disconnected, this, &WebSocketClient::onHttpDisconnected);
        httpWebSocket.open(httpUrl);
    }

    // 初始化自动重连定时器
    reconnectTimer.setInterval(1000);
    connect(&reconnectTimer, &QTimer::timeout, this, &WebSocketClient::reconnect);

    // 连接网络状态变化信号槽
    connect(&networkManager, &QNetworkConfigurationManager::onlineStateChanged, this, &WebSocketClient::onNetworkStateChanged);
}

void WebSocketClient::onHttpsConnected()
{
    qInfo() << "WebSocket connected successfully";
    emit connectionStatusChanged(true, "Connected successfully");
    if (httpsWebSocket.isValid()) {
        connect(&httpsWebSocket, &QWebSocket::textMessageReceived,
                this, &WebSocketClient::onTextMessageReceived);
        isHttpsConnected = true;
    }

    if ((!httpWebSocket.isValid() && isHttpsConnected) || (!httpsWebSocket.isValid() && isHttpConnected) || (isHttpsConnected && isHttpConnected))
    {
        qInfo() << "Both websockets are connected";
        reconnectTimer.stop();
    }
    else
    {
        qWarning() << "Waiting for https network...";
    }
}

void WebSocketClient::onHttpsDisconnected()
{
    qWarning() << "WebSocket disconnected";
    emit connectionStatusChanged(false, "Disconnected");

    // 断开接收消息的信号与槽
    disconnect(&httpsWebSocket, &QWebSocket::textMessageReceived,
               this, &WebSocketClient::onTextMessageReceived);

    // 启动自动重连定时器，但仅在网络连接正常时重连
    if (isHttpsConnected)
    {
        isHttpsConnected = false;

        qInfo() << "Starting reconnect https timer...";
        reconnectTimer.start();
    }
    else
    {
        qWarning() << "Waiting for https network...";
    }
}

void WebSocketClient::onHttpConnected()
{
    qInfo() << "WebSocket connected successfully";
    emit connectionStatusChanged(true, "Connected successfully");
     if (httpWebSocket.isValid()) {
        connect(&httpWebSocket, &QWebSocket::textMessageReceived,
                this, &WebSocketClient::onTextMessageReceived);
        isHttpConnected = true;
    }
    if ((!httpWebSocket.isValid() && isHttpsConnected) || (!httpsWebSocket.isValid() && isHttpConnected) || (isHttpsConnected && isHttpConnected))
    {
        qInfo() << "Both websockets are connected";
        reconnectTimer.stop();
    }
    else
    {
        qWarning() << "Waiting for http network...";
    }
    
}

void WebSocketClient::onHttpDisconnected()
{
    qWarning() << "WebSocket disconnected";
    emit connectionStatusChanged(false, "Disconnected");

    // 断开接收消息的信号与槽
    disconnect(&httpWebSocket, &QWebSocket::textMessageReceived,
               this, &WebSocketClient::onTextMessageReceived);

    // 启动自动重连定时器，但仅在网络连接正常时重连
    if (isHttpConnected)
    {
        isHttpConnected = false;

        qInfo() << "Starting reconnect http timer...";
        reconnectTimer.start();
    }
    else
    {
        qWarning() << "Waiting for https network...";
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
    if (!isNetworkConnected) return;
    if (httpsWebSocket.isValid() && !isHttpsConnected)
    {
        qInfo() << "Reconnecting to https WebSocket server...";
        httpsWebSocket.close(); // 关闭当前连接
        httpsWebSocket.open(httpsUrl);
    }
    if (httpWebSocket.isValid() && !isHttpConnected)
    {
        qInfo() << "Reconnecting to http WebSocket server...";
        httpWebSocket.close(); // 关闭当前连接
        httpWebSocket.open(httpUrl);
    }
}

void WebSocketClient::onNetworkStateChanged(bool isOnline)
{
    if (isOnline)
    {
        qInfo() << "Network is online";
        isNetworkConnected = true;
        // 网络恢复时重置状态，执行自动重连
        reconnect();
    }
    else
    {
        qWarning() << "Network is offline";
        isNetworkConnected = false;
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
        QString info = "localMessage:" + lat + ":" + lon + ":" + language;
        emit messageReceived(info);
        // qDebug() << "发送app信息 received:" << info;
    }
    else
    {
        qDebug() << "Message received is undefined type:" << messageObj["type"].toString() << " " << messageObj["message"].toString();
    }

    emit closed();
}

void WebSocketClient::sendAcknowledgment(QString messageID)
{
    QString utf8Message = messageID.toUtf8();

    QJsonObject messageObj;

    messageObj["type"] = "QT_Confirm";
    messageObj["msgid"] = utf8Message;

    // 然后使用WebSocket发送消息
    if (isHttpsConnected)
    {
        httpsWebSocket.sendTextMessage(QJsonDocument(messageObj).toJson());
    }
    if (isHttpConnected)
    {
        httpWebSocket.sendTextMessage(QJsonDocument(messageObj).toJson());
    }
}

void WebSocketClient::sendProcessCommandReturn(QString message)
{
    QString utf8Message = message.toUtf8();

    QJsonObject messageObj;
    messageObj["type"] = "Process_Command_Return";
    messageObj["message"] = utf8Message;
    if (isHttpsConnected)
    {
        httpsWebSocket.sendTextMessage(QJsonDocument(messageObj).toJson());
    }
    if (isHttpConnected)
    {
        httpWebSocket.sendTextMessage(QJsonDocument(messageObj).toJson());
    }
}

void WebSocketClient::messageSend(QString message)
{
    QString utf8Message = message.toUtf8(); // 将QString转换为UTF-8编码的QString

    QJsonObject messageObj;

    messageObj["message"] = utf8Message;
    messageObj["type"] = "QT_Return";

    // 然后使用WebSocket发送消息
    if (isHttpsConnected)
    {
        httpsWebSocket.sendTextMessage(QJsonDocument(messageObj).toJson());
    }
    if (isHttpConnected)
    {
        httpWebSocket.sendTextMessage(QJsonDocument(messageObj).toJson());
    }
}
