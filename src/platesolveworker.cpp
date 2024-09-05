// PlateSolveWorker.cpp

#include "platesolveworker.h"
#include <QThread>

PlateSolveWorker::PlateSolveWorker(QObject *parent)
    // : QThread(parent), focalLength(0), cameraWidth(0), cameraHeight(0), imageWidth(0), imageHeight(0), useQHYCCDSDK(false), running(true) {}
    // : QThread(parent), filename(""), focalLength(0), cameraWidth(0), cameraHeight(0), imageWidth(0), imageHeight(0), useQHYCCDSDK(false) {}
    : QThread(parent), filename(""), focalLength(0), cameraWidth(0), cameraHeight(0), useQHYCCDSDK(false) {}

void PlateSolveWorker::setParams(QString fn, int fl, double cw, double ch, bool useSDK)
{
    // QMutexLocker locker(&mutex);
    filename = fn;
    focalLength = fl;
    cameraWidth = cw;
    cameraHeight = ch;
    // imageWidth = iw;
    // imageHeight = ih;
    useQHYCCDSDK = useSDK;

    // running = true;
}

// void PlateSolveWorker::stop()
// {
//     QMutexLocker locker(&mutex);
//     running = false;
// }

void PlateSolveWorker::run()
{
    // while (true) {
    //     {
    //         QMutexLocker locker(&mutex);
    //         if (!running) {
    //             break;
    //         }
    //     }

        result = Tools::PlateSolve(filename, focalLength, cameraWidth, cameraHeight, useQHYCCDSDK);
        emit resultReady(result);
    //     QThread::sleep(1);  // 等待1秒，避免过于频繁地调用
    // }
}