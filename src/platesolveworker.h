// PlateSolveWorker.h

#ifndef PLATESOLVEWORKER_H
#define PLATESOLVEWORKER_H

#include "tools.hpp"

#include <QThread>
// #include <QMutex>
// #include <QWaitCondition>

class PlateSolveWorker : public QThread
{
    Q_OBJECT

public:
    PlateSolveWorker(QObject *parent = nullptr);

    // void setParams(QString fn, int fl, double cw, double ch, int iw, int ih, bool useSDK);
    void setParams(QString fn, int fl, double cw, double ch, bool useSDK);
    // void stop();

    SloveResults result;

signals:
    void resultReady(const SloveResults &result);

protected:
    void run() override;

private:
    QString filename;
    int focalLength;
    double cameraWidth;
    double cameraHeight;
    // int imageWidth;
    // int imageHeight;
    bool useQHYCCDSDK;
    // bool running;
    // QMutex mutex;
};

#endif // PLATESOLVEWORKER_H
