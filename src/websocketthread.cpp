#include "websocketthread.h"

WebSocketThread::WebSocketThread(const QUrl &httpUrl, const QUrl &httpsUrl, QObject *parent) :
    QThread(parent), httpUrl(httpUrl), httpsUrl(httpsUrl) {}

WebSocketThread::~WebSocketThread() {
    quit();
    wait();
    delete client;
}

void WebSocketThread::run() {
    client = new WebSocketClient(httpUrl, httpsUrl);
    connect(this, &WebSocketThread::sendMessageToClient, client, &WebSocketClient::messageSend);
    connect(this, &WebSocketThread::sendProcessCommandReturn, client, &WebSocketClient::sendProcessCommandReturn);
    bool ok = connect(client, &WebSocketClient::messageReceived, this, &WebSocketThread::receivedMessage);
    if (!ok) {
        qDebug() << "Failed to connect messageReceived signal";
    }
    exec();  // Start the event loop
}