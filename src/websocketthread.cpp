#include "websocketthread.h"

WebSocketThread::WebSocketThread(const QUrl &url, QObject *parent)
    : QThread(parent), url(url) {}

WebSocketThread::~WebSocketThread() {
    quit();
    wait();
    delete client;
}

void WebSocketThread::run() {
    client = new WebSocketClient(url);
    connect(this, &WebSocketThread::sendMessageToClient, client,
            &WebSocketClient::messageSend);
    connect(client, &WebSocketClient::messageReceived, this,
            &WebSocketThread::receivedMessage);
    exec();  // Start the event loop
}