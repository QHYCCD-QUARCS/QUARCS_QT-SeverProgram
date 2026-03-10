#include <QCoreApplication>
#include <QDebug>
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

    MainWindow mainWindow;
    return a.exec();
}

