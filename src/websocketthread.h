#ifndef WEBSOCKETTHREAD_H
#define WEBSOCKETTHREAD_H

#include <QThread>
#include "websocketclient.h"

class WebSocketThread : public QThread
{
    Q_OBJECT

public:
    explicit WebSocketThread(const QUrl &url, QObject *parent = nullptr);
    ~WebSocketThread();

signals:
    void receivedMessage(QString message);
    void sendMessageToClient(QString message);
    void sendProcessCommandReturn(QString message);
protected:
    void run() override;

private:
    QUrl url;
    WebSocketClient *client;
};

#endif // WEBSOCKETTHREAD_H
