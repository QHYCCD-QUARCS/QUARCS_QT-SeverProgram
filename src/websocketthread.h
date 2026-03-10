#ifndef WEBSOCKETTHREAD_H
#define WEBSOCKETTHREAD_H

#include <QThread>
#include <QMutex>
#include <QQueue>
#include <QString>
#include "websocketclient.h"

class WebSocketThread : public QThread
{
    Q_OBJECT

public:
    explicit WebSocketThread(const QUrl &httpUrl, const QUrl &httpsUrl, QObject *parent = nullptr);
    ~WebSocketThread();

signals:
    void receivedMessage(QString message);
    void sendMessageToClient(QString message);
    void sendProcessCommandReturn(QString message);

private slots:
    // 关键：缓存“线程未就绪”阶段发出的消息，等 WebSocketClient 创建完成后再转发，
    // 避免启动早期（例如 ServerInitSuccess）信号丢失。
    void handleSendMessageToClient(const QString &message);
    void handleSendProcessCommandReturn(const QString &message);
protected:
    void run() override;

private:
    QUrl httpUrl;
    QUrl httpsUrl;
    WebSocketClient *client;

    QMutex mutex;
    QQueue<QString> pendingToClient;
    QQueue<QString> pendingProcessReturn;
    bool clientReady = false;
};

#endif // WEBSOCKETTHREAD_H
