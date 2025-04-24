#include "websocketthread.h"

WebSocketThread::WebSocketThread(const QUrl &url, QObject *parent) :
    QThread(parent), url(url) {}

WebSocketThread::~WebSocketThread() {
    quit();
    wait();
    delete client;
}

void WebSocketThread::run() {
    client = new WebSocketClient(url);
    connect(this, &WebSocketThread::sendMessageToClient, client, &WebSocketClient::messageSend);
    bool ok = connect(client, &WebSocketClient::messageReceived, this, &WebSocketThread::receivedMessage);
    if (!ok) {
        qDebug() << "Failed to connect messageReceived signal";
    }
    exec();  // Start the event loop
}