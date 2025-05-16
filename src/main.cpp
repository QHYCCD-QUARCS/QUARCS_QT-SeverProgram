#include <QCoreApplication>
#include <QDebug>
#include <sys/prctl.h>
#include "mainwindow.h"
#include "tianwen.h"

void handleMessage(const QString &message) {
    qDebug() << "Message received in main:" << message;
    // 处理消息
}

int main(int argc, char *argv[])
{
    prctl(PR_SET_NAME, "QUARCS"); // 设置进程名
    QCoreApplication a(argc, argv);

    qDebug("main start...");

    MainWindow mainWindow;
    return a.exec();
}

