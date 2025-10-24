#include "websocketthread.h"
#include <QMetaObject>

WebSocketThread::WebSocketThread(const QUrl &httpUrl, const QUrl &httpsUrl, QObject *parent) :
    QThread(parent), httpUrl(httpUrl), httpsUrl(httpsUrl), client(nullptr) {}

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

void WebSocketThread::run() {
    setPriority(QThread::HighPriority);
    client = new WebSocketClient(httpUrl, httpsUrl);
    connect(this, &WebSocketThread::sendMessageToClient, client, &WebSocketClient::messageSend);
    connect(this, &WebSocketThread::sendProcessCommandReturn, client, &WebSocketClient::sendProcessCommandReturn);
    bool ok = connect(client, &WebSocketClient::messageReceived, this, &WebSocketThread::receivedMessage);
    if (!ok) {
        qDebug() << "Failed to connect messageReceived signal";
    }
    exec();  // Start the event loop
}