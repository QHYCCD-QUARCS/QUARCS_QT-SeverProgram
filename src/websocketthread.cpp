#include "websocketthread.h"
#include <QMetaObject>
#include <QMutexLocker>

WebSocketThread::WebSocketThread(const QUrl &httpUrl, const QUrl &httpsUrl, QObject *parent) :
    QThread(parent), httpUrl(httpUrl), httpsUrl(httpsUrl), client(nullptr)
{
    // 把“发送到客户端”的信号先接到本对象的缓存/转发槽上。
    // 这样即便外部在 run() 还没创建 WebSocketClient 之前就 emit，也不会丢消息。
    connect(this, &WebSocketThread::sendMessageToClient,
            this, &WebSocketThread::handleSendMessageToClient,
            Qt::DirectConnection);
    connect(this, &WebSocketThread::sendProcessCommandReturn,
            this, &WebSocketThread::handleSendProcessCommandReturn,
            Qt::DirectConnection);
}

WebSocketThread::~WebSocketThread() {
    if (client) {
        if (isRunning()) {
            QMetaObject::invokeMethod(client, "stop", Qt::BlockingQueuedConnection);
            QMetaObject::invokeMethod(client, "deleteLater", Qt::BlockingQueuedConnection);
        } else {
            client->moveToThread(QThread::currentThread());
            client->stop();
            delete client;
        }
        client = nullptr;
    }
    if (isRunning()) {
        quit();
        wait();
    }
}

void WebSocketThread::handleSendMessageToClient(const QString &message)
{
    WebSocketClient *c = nullptr;
    {
        QMutexLocker locker(&mutex);
        if (!clientReady || client == nullptr) {
            pendingToClient.enqueue(message);
            return;
        }
        c = client;
    }
    // 跨线程投递到 WebSocketClient 所在线程执行
    QMetaObject::invokeMethod(c, "messageSend", Qt::QueuedConnection,
                              Q_ARG(QString, message));
}

void WebSocketThread::handleSendProcessCommandReturn(const QString &message)
{
    WebSocketClient *c = nullptr;
    {
        QMutexLocker locker(&mutex);
        if (!clientReady || client == nullptr) {
            pendingProcessReturn.enqueue(message);
            return;
        }
        c = client;
    }
    QMetaObject::invokeMethod(c, "sendProcessCommandReturn", Qt::QueuedConnection,
                              Q_ARG(QString, message));
}

void WebSocketThread::run() {
    setPriority(QThread::HighPriority);
    {
        QMutexLocker locker(&mutex);
        client = new WebSocketClient(httpUrl, httpsUrl);
        clientReady = true;
    }
    bool ok = connect(client, &WebSocketClient::messageReceived, this, &WebSocketThread::receivedMessage);
    if (!ok) {
        qDebug() << "Failed to connect messageReceived signal";
    }

    // 冲刷启动早期缓存的消息（在本线程直接调用即可）
    QQueue<QString> toClient;
    QQueue<QString> processRet;
    {
        QMutexLocker locker(&mutex);
        toClient = pendingToClient;
        processRet = pendingProcessReturn;
        pendingToClient.clear();
        pendingProcessReturn.clear();
    }
    while (!toClient.isEmpty()) {
        client->messageSend(toClient.dequeue());
    }
    while (!processRet.isEmpty()) {
        client->sendProcessCommandReturn(processRet.dequeue());
    }

    exec();  // Start the event loop
}