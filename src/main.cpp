#include <QCoreApplication>
#include <QDebug>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <sys/prctl.h>
#include "mainwindow.h"
#include "guiding/GuiderTypes.h"

void handleMessage(const QString &message) {
    qDebug() << "Message received in main:" << message;
    // 处理消息
}

int main(int argc, char *argv[])
{
    prctl(PR_SET_NAME, "QUARCS"); // 设置进程名
    QCoreApplication a(argc, argv);

    // 修复：允许 queued connection 传递 guiding::State，避免刷 Qt 警告
    qRegisterMetaType<guiding::State>("guiding::State");

    qDebug("main start...");

    // 禁用系统代理（http_proxy/https_proxy/no_proxy），避免局域网 WebSocket/HTTP
    // 连接被错误地走代理，导致“前后端断连”。
    QNetworkProxyFactory::setUseSystemConfiguration(false);
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);

    MainWindow mainWindow;
    return a.exec();
}

