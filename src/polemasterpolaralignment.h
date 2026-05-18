#ifndef POLEMASTERPOLARALIGNMENT_H
#define POLEMASTERPOLARALIGNMENT_H

#include <QObject>
#include <QPointF>
#include <QTimer>
#include <QString>
#include <QVector>
#include <QSize>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

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
    int defaultExposureTime = 500;
    int guidanceExposureTime = 1000;
    int captureTimeoutMs = 10000;
    int movementTimeoutMs = 60000;
    int focalLength = 0;
    double cameraWidth = 0.0;
    double cameraHeight = 0.0;
    double raRotationAngle = 35.0;
    double doneThresholdArcsec = 30.0;
    int stableFrameRequirement = 3;
    double latitude = 0.0;
    double solveSearchRadiusDeg = 5.0;
    QString solveIndexFilePath;
    int trackMaxStars = 30;
    double trackBaseRadiusPx = 16.0;
    double trackMaxRadiusPx = 72.0;
    int trackLostFrameLimit = 12;
    double trackEdgeMarginRatio = 0.08;
    double trackMinSeparationPx = 14.0;
    int guidanceExposureMinMs = 100;
    int guidanceExposureMaxMs = 5000;
    int guidanceExposureUpStepMs = 250;
    int guidanceExposureDownStepMs = 150;
    int guidanceStarCountLow = 12;
    int guidanceStarCountHigh = 42;
};

class PoleMasterPolarAlignment : public QObject
{
    Q_OBJECT

public:
    explicit PoleMasterPolarAlignment(MyClient *indiServer,
                                      INDI::BaseDevice *dpMount,
                                      INDI::BaseDevice *dpPoleCamera,
                                      bool useSdkCaptureSource,
                                      bool simulationMode = false,
                                      const QString &simulationImageRootPath = QString(),
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
                   QString frameId,
                   QString hint);
    void frameData(QString fileName, int imageW, int imageH, QString frameId);
    void requestCaptureForRole(QString cameraRole, int exposureTimeMs);
    void overlayData(QString jsonPayload);

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
        QVector<QPointF> detectedStars;
        QVector<double> detectedStarScores;
        QVector<QPointF> selectedTrackStars;
        QPointF trackedLockStarPx{-1.0, -1.0};
        double lockConfidence = 0.0;
        QString trackingMode;
        double raDeg = 0.0;
        double decDeg = 0.0;
        int exposureMs = 0;
        QString frameId;
        double northAngleDeg = 0.0;
        bool northAngleValid = false;
    };

    void setState(PoleMasterAlignmentState newState, const QString &message, int progress);
    void processCurrentState();
    bool captureAndSolve(SolveFrame &frame);
    bool captureAndSolveWithQuality(SolveFrame &frame, const QString &phase);
    bool captureImage(int exposureMs);
    bool captureImageSimulated(int exposureMs);
    bool waitForCaptureComplete();
    bool solveImage(const QString &fitsPath);
    bool readSolveFrame(const QString &fitsPath, int exposureMs, SolveFrame &frame) const;
    bool readFitsSize(const QString &fitsPath, int &imageW, int &imageH) const;
    bool skyToPixel(const QString &wcsPath, double raDeg, double decDeg, QPointF &point) const;
    bool estimateNorthAngleDeg(const SolveFrame &frame, double &northAngleDeg) const;
    void enrichFrameDiagnostics(SolveFrame &frame) const;
    void emitOverlay(const QString &phase, const SolveFrame *frame, const QStringList &warnings = QStringList(), const QJsonObject &extra = QJsonObject());
    QJsonArray pointsToJson(const QVector<QPointF> &points, int limit = 80) const;
    QJsonArray fixedStarsToJson(const SolveFrame &frame) const;
    QJsonArray rotationSamplesToJson() const;
    QJsonObject qualityJson(const SolveFrame *frame, const QStringList &warnings, const QJsonObject &extra = QJsonObject()) const;
    double truePoleDecDeg() const;
    bool moveRaAxis(double angleDeg);
    bool captureMountPosition(double &raDeg, double &decDeg) const;
    double wrappedAngleDelta(double fromDeg, double toDeg) const;
    bool returnToStartPosition();
    bool waitForMountIdle();
    bool fitAxisCenter();
    bool fitAxisCenterFromDetectedStars(QPointF &center, double &radius, double &residual) const;
    bool emitCurrentGuide(const SolveFrame &frame);
    bool initGuidingTrackingFromLastSolve();
    bool captureAndTrackGuideFrame(SolveFrame &frame);
    bool recoverTrackingWithoutSolve(const QVector<QPointF> &candidateStars);
    QVector<int> selectTrackingStarIndices(const SolveFrame &frame) const;
    bool chooseInitialLockStar(const SolveFrame &frame, QPointF &selected) const;
    bool updateLockStarByGlobalTracking(const cv::Mat &currentGray8,
                                        const QVector<QPointF> &candidateStars,
                                        QPointF &updatedLockStar,
                                        double &confidence);
    bool updateLockStarByTracking(const cv::Mat &currentGray8, const QVector<QPointF> &candidateStars, QPointF &updatedLockStar, double &confidence);
    QString buildHint(double dx, double dy) const;
    QString simulationScriptPath() const;
    int simulationScriptIndexForState() const;
    QPointF simulationPoleForCurrentState() const;
    QPointF currentSimulationAxisPx() const;
    static QPointF simulationReferenceAxisPx();
    void updateSimulationGuidingOffset();
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
    SolveFrame guidingAnchorFrame;
    bool hasGuidingAnchorFrame = false;
    QPointF axisCenterPx{-1.0, -1.0};
    double axisRadiusPx = 0.0;
    double axisResidualPx = 0.0;
    int stableFrames = 0;
    double startRaDeg = 0.0;
    double startDecDeg = 0.0;
    double lastRaDeg = 0.0;
    double lastDecDeg = 0.0;
    bool hasStartMountPosition = false;
    QVector<double> actualRaRotations;
    bool lastCaptureQualityPoor = false;
    QString lastCaptureFailureDetail;
    bool guidingTrackingInitialized = false;
    QPointF guidingLockStarPx{-1.0, -1.0};
    QPointF guidingPoleOffsetPx{0.0, 0.0};
    double guidingLockConfidence = 0.0;
    int guidingLostFrames = 0;
    int adaptiveGuidanceExposureMs = 1000;
    int adaptiveSolveExposureMs = 0;
    bool guidanceExposureBootstrapped = false;
    bool guidanceExposureSeededFromSolve = false;
    cv::Mat previousGuideGray8;
    QVector<QPointF> previousSelectedTrackStars;
    bool guideGrayReady = false;
    int guidanceImageW = 0;
    int guidanceImageH = 0;
    double guidancePixelScaleArcsecPerPixel = 1.0;
    bool simulationMode = false;
    QString simulationImageRootPath;
    int simulationFrameIndex = 0;
    double simulationVirtualRaDeg = 0.0;
    double simulationVirtualDecDeg = 89.0;
    QPointF simulationGuidingOffsetPx{156.0, 121.0};
    int simulationGuidingFrameCount = 0;
    QString pendingFrameFileName;
    int pendingFrameImageW = 0;
    int pendingFrameImageH = 0;
    QString pendingFrameId;
};

class PoleMasterAlignmentSimulation : public QObject
{
    Q_OBJECT

public:
    explicit PoleMasterAlignmentSimulation(const QString &imageRootPath,
                                           QObject *parent = nullptr);
    ~PoleMasterAlignmentSimulation();

    bool start();
    void stop();
    bool isRunning() const;

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
                   QString frameId,
                   QString hint);
    void frameData(QString fileName, int imageW, int imageH, QString frameId);
    void overlayData(QString jsonPayload);

private slots:
    void onStepTimerTimeout();

private:
    struct SimSolvedFrame {
        QString fitsPath;
        QString wcsPath;
        int imageW = 0;
        int imageH = 0;
        double pixelScaleArcsecPerPixel = 0.0;
        QPointF truePolePx;
        QVector<QPointF> detectedStars;
        QString frameId;
        bool plateSolved = false;
    };

    struct SimStar {
        QPointF position;
        double magnitude = 5.0;
    };

    void setState(PoleMasterAlignmentState newState, const QString &message, int progress);
    bool emitFrameAndGuide();
    void emitOverlay(const QString &phase, const QString &frameId);
    QString generateFrameImage();
    bool generateFrameImageFromScript(QString &fileName, QString *failureReason = nullptr, int exposureOverrideMs = -1);
    QString simulationScriptPath() const;
    bool solveGeneratedFits(const QString &fitsPath, SimSolvedFrame &frame);
    bool readGeneratedFitsSize(const QString &fitsPath, int &imageW, int &imageH) const;
    bool skyToPixelFromWcs(const QString &wcsPath, double raDeg, double decDeg, QPointF &point) const;
    void enrichGeneratedFrameDiagnostics(SimSolvedFrame &frame) const;
    QJsonArray generatedPointsToJson(const QVector<QPointF> &points, int limit = 80) const;
    QJsonArray generatedFixedStarsToJson(const SimSolvedFrame &frame) const;
    QJsonArray generatedRotationSamplesToJson() const;
    QJsonObject generatedQualityJson(const SimSolvedFrame *frame, const QJsonObject &extra = QJsonObject()) const;
    QJsonObject buildGeneratedOverlay(const QString &phase, const SimSolvedFrame *frame, const QJsonObject &extra = QJsonObject()) const;
    int simulationExposureMs() const;
    QString buildHint(double dx, double dy) const;
    QPointF currentPolePosition() const;
    QVector<QPointF> currentTemplateStarPositions() const;
    QPointF currentStarImagePosition(const SimStar &star) const;
    static QVector<SimStar> fixedStars();

    QString imageRootPath;
    QTimer stepTimer;
    bool running = false;
    PoleMasterAlignmentState currentState = PoleMasterAlignmentState::IDLE;
    int scriptIndex = 0;
    int frameIndex = 0;
    int stableFrames = 0;
    const QSize imageSize{1024, 768};
    const QPointF axisCenter{512.0, 384.0};
    QPointF lastGeneratedPole{668.0, 505.0};
    QPointF lastGeneratedAxis{512.0, 384.0};
    double lastGeneratedPixelScaleArcsecPerPixel = 4.0;
    double lastSolvedRaDeg = 0.0;
    double lastSolvedDecDeg = 89.9999;
    QString lastGeneratedOverlayJson;
    QString lastGeneratedFrameId;
    QVector<SimSolvedFrame> generatedSolveFrames;
    double generatedAxisRadiusPx = 122.0;
    double generatedAxisResidualPx = 3.4;
    int adaptiveGuidanceExposureMs = 1000;
    int lastGeneratedExposureMs = 1000;
};

#endif // POLEMASTERPOLARALIGNMENT_H
