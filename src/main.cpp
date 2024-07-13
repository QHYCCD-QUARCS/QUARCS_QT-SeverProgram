#include "mainwindow.h"

#include <QCoreApplication>
#include <QDebug>

void handleMessage(const QString &message) {
    qDebug() << "Message received in main:" << message;
    // 处理消息
}

int main(int argc, char *argv[]) {
    QCoreApplication a(argc, argv);

    qDebug("main start...");

    MainWindow mainWindow;

    return a.exec();
}
