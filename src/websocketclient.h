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
    explicit WebSocketClient(const QUrl &httpUrl, const QUrl &httpsUrl, QObject *parent = nullptr);
    void messageSend(QString message);
    void sendAcknowledgment(QString  messageObj);
    void sendProcessCommandReturn(QString message);
    void reconnect();
    void onNetworkStateChanged(bool isOnline);

signals:
    void closed();
    void messageReceived(const QString &message);
    void connectionError(const QString &error);
    void connectionStatusChanged(bool isConnected, const QString &status);

private slots:
    void onHttpsConnected();
    void onHttpsDisconnected();
    void onHttpConnected();
    void onHttpDisconnected();
    void onTextMessageReceived(QString message);
    void onError(QAbstractSocket::SocketError error);
    void onSslErrors(const QList<QSslError> &errors);

private:
    QWebSocket httpWebSocket;
    QWebSocket httpsWebSocket;
    QUrl httpUrl;    // 添加HTTP URL成员变量
    QUrl httpsUrl;   // 添加HTTPS URL成员变量
    bool isHttpsConnected = false;
    bool isHttpConnected = false;

    QTimer reconnectTimer; // 自动重连定时器
    QNetworkConfigurationManager networkManager; // 网络配置管理器
    bool isNetworkConnected = true; // 记录网络连接状态

    bool isReconnecting = false; // 添加一个标志来表示是否正在重连
    
    QString getErrorString(QAbstractSocket::SocketError error);
};

#endif // WEBSOCKETCLIENT_H
