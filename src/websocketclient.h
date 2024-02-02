#ifndef WEBSOCKETCLIENT_H
#define WEBSOCKETCLIENT_H

#include <QObject>
#include <QWebSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QNetworkConfigurationManager>

class WebSocketClient : public QObject
{
    Q_OBJECT
public:
    explicit WebSocketClient(const QUrl &url, QObject *parent = nullptr);
    void messageSend(QString message);
    void sendAcknowledgment(QString  messageObj);
    void reconnect();
    void onNetworkStateChanged(bool isOnline);

signals:
    void closed();
    void messageReceived(const QString &message);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(QString message);

private:
    QWebSocket webSocket;
    QUrl url;

    QTimer reconnectTimer; // 自动重连定时器
    QNetworkConfigurationManager networkManager; // 网络配置管理器
    bool isNetworkConnected = true; // 记录网络连接状态

    bool isReconnecting = false; // 添加一个标志来表示是否正在重连
};

#endif // WEBSOCKETCLIENT_H
