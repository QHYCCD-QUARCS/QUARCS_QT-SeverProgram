#include "websocketclient.h"

#include <QDebug>

WebSocketClient::WebSocketClient(const QUrl &url, QObject *parent)
    : QObject(parent), url(url) {
    connect(&webSocket, &QWebSocket::connected, this,
            &WebSocketClient::onConnected);
    connect(&webSocket, &QWebSocket::disconnected, this,
            &WebSocketClient::onDisconnected);
    webSocket.open(url);

    // 初始化自动重连定时器
    reconnectTimer.setInterval(1000);  // 2秒尝试重新连接
    connect(&reconnectTimer, &QTimer::timeout, this,
            &WebSocketClient::reconnect);

    // 连接网络状态变化信号槽
    connect(&networkManager, &QNetworkConfigurationManager::onlineStateChanged,
            this, &WebSocketClient::onNetworkStateChanged);
}

void WebSocketClient::onConnected() {
    qDebug() << "WebSocket connected";
    connect(&webSocket, &QWebSocket::textMessageReceived, this,
            &WebSocketClient::onTextMessageReceived);

    // 示例：发送一条消息到服务器
    // webSocket.sendTextMessage(QStringLiteral("Hello, WebSocket server!"));

    // 连接建立时停止自动重连定时器
    reconnectTimer.stop();
}

void WebSocketClient::onDisconnected() {
    qDebug() << "WebSocket disconnected";

    // 断开接收消息的信号与槽
    disconnect(&webSocket, &QWebSocket::textMessageReceived, this,
               &WebSocketClient::onTextMessageReceived);

    // 启动自动重连定时器，但仅在网络连接正常时重连
    if (isNetworkConnected) {
        qDebug() << "Starting reconnect timer...";
        reconnectTimer.start();
    } else {
        qDebug() << "Waitting for network...";
    }
}

void WebSocketClient::reconnect() {
    qDebug() << "Reconnecting to WebSocket server...";
    webSocket.close();  // 关闭当前连接
    webSocket.open(url);
}

void WebSocketClient::onNetworkStateChanged(bool isOnline) {
    if (isOnline) {
        qDebug() << "Network is online";
        isNetworkConnected = true;
        // 网络恢复时重置状态，执行自动重连
        reconnect();
    } else {
        qDebug() << "Network is offline";
        isNetworkConnected = false;
    }
}

void WebSocketClient::onTextMessageReceived(QString message) {
    // qDebug() << "Message received:" << message;
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    QJsonObject messageObj = doc.object();

    if (messageObj["type"].toString() == "Vue_Command") {
        // 处理命令
        emit messageReceived(messageObj["message"].toString());

        // 发送确认消息
        sendAcknowledgment(messageObj["msgid"].toString());
    } else if (messageObj["type"].toString() == "Server_msg") {
        emit messageReceived(messageObj["message"].toString());
    }

    emit closed();
}

void WebSocketClient::sendAcknowledgment(QString messageID) {
    QString utf8Message = messageID.toUtf8();

    QJsonObject messageObj;

    messageObj["type"] = "QT_Confirm";
    messageObj["msgid"] = utf8Message;

    // 然后使用WebSocket发送消息
    webSocket.sendTextMessage(QJsonDocument(messageObj).toJson());
}

void WebSocketClient::messageSend(QString message) {
    QString utf8Message =
        message.toUtf8();  // 将QString转换为UTF-8编码的QString

    QJsonObject messageObj;

    messageObj["message"] = utf8Message;
    messageObj["type"] = "QT_Return";

    // 然后使用WebSocket发送消息
    webSocket.sendTextMessage(QJsonDocument(messageObj).toJson());
}
