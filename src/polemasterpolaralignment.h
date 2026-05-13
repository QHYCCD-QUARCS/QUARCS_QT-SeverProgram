#ifndef POLEMASTERPOLARALIGNMENT_H
#define POLEMASTERPOLARALIGNMENT_H

#include <QObject>
#include <QPointF>
#include <QTimer>
#include <QString>
#include <QVector>

#include "myclient.h"
#include "tools.h"

enum class PoleMasterAlignmentState {
    IDLE = 0,
    INITIALIZING = 1,
    FIRST_CAPTURE = 2,
    MOVING_RA_FIRST = 3,
    SECOND_CAPTURE = 4,
    MOVING_RA_SECOND = 5,
    THIRD_CAPTURE = 6,
    CALCULATING_AXIS = 7,
    GUIDING_ADJUSTMENT = 8,
    COMPLETED = 9,
    FAILED = 10
};

struct PoleMasterAlignmentConfig {
    int defaultExposureTime = 1000;
    int guidanceExposureTime = 1000;
    int captureTimeoutMs = 10000;
    int movementTimeoutMs = 60000;
    int focalLength = 0;
    double cameraWidth = 0.0;
    double cameraHeight = 0.0;
    double raRotationAngle = 35.0;
    double doneThresholdArcsec = 30.0;
    int stableFrameRequirement = 3;
};

class PoleMasterPolarAlignment : public QObject
{
    Q_OBJECT

public:
    explicit PoleMasterPolarAlignment(MyClient *indiServer,
                                      INDI::BaseDevice *dpMount,
                                      INDI::BaseDevice *dpPoleCamera,
                                      bool useSdkCaptureSource,
                                      QObject *parent = nullptr);
    ~PoleMasterPolarAlignment();

    void setConfig(const PoleMasterAlignmentConfig &config);
    bool start();
    void stop();
    bool isRunning() const;

    void setCaptureEnd(bool isEnd);
    void setCapturedImagePath(const QString &fitsPath);

signals:
    void stateChanged(PoleMasterAlignmentState state, QString message, int progress, bool running);
    void guideData(int imageW,
                   int imageH,
                   double axisX,
                   double axisY,
                   double poleX,
                   double poleY,
                   double errorPx,
                   double errorArcsec,
                   QString hint);
    void requestCaptureForRole(QString cameraRole, int exposureTimeMs);

private slots:
    void onStateTimerTimeout();

private:
    struct SolveFrame {
        QString fitsPath;
        QString wcsPath;
        int imageW = 0;
        int imageH = 0;
        double pixelScaleArcsecPerPixel = 0.0;
        QPointF truePolePx;
    };

    void setState(PoleMasterAlignmentState newState, const QString &message, int progress);
    void processCurrentState();
    bool captureAndSolve(SolveFrame &frame);
    bool captureImage(int exposureMs);
    bool waitForCaptureComplete();
    bool solveImage(const QString &fitsPath);
    bool readSolveFrame(const QString &fitsPath, SolveFrame &frame) const;
    bool readFitsSize(const QString &fitsPath, int &imageW, int &imageH) const;
    bool skyToPixel(const QString &wcsPath, double raDeg, double decDeg, QPointF &point) const;
    bool moveRaAxis(double angleDeg);
    bool waitForMountIdle() const;
    bool fitAxisCenter();
    bool emitCurrentGuide(const SolveFrame &frame);
    QString buildHint(double dx, double dy) const;
    static bool fitCircle3Points(const QPointF &p1,
                                 const QPointF &p2,
                                 const QPointF &p3,
                                 QPointF &center,
                                 double &radius);

    MyClient *indiServer = nullptr;
    INDI::BaseDevice *dpMount = nullptr;
    INDI::BaseDevice *dpPoleCamera = nullptr;
    bool useSdkCaptureSource = false;
    PoleMasterAlignmentConfig config;
    PoleMasterAlignmentState currentState = PoleMasterAlignmentState::IDLE;
    QTimer stateTimer;
    bool running = false;
    bool captureEnded = false;
    QString latestCapturePathFromHost;
    QString lastCapturedImage;
    QVector<SolveFrame> calibrationFrames;
    QPointF axisCenterPx{-1.0, -1.0};
    double axisRadiusPx = 0.0;
    int stableFrames = 0;
};

#endif // POLEMASTERPOLARALIGNMENT_H
