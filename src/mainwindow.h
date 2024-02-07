#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QObject>
#include <QThread>
#include <QProcess>
#include <fitsio.h>
// #include "websocketclient.h"
#include "myclient.h"
#include <QFile>
#include "tools.hpp"
#include <QXmlStreamReader>
#include "websocketthread.h"

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <iostream>
#include <fstream>
#include <vector>

class MainWindow : public QObject
{
    Q_OBJECT

public:
    explicit MainWindow(QObject *parent = nullptr);
    ~MainWindow();

    void initINDIClient();
    void initINDIServer();

    QString connectIndiServer();
    void disconnectIndiServer();
    void connectDevice(int x);

    void readDriversListFromFiles(const std::string &filename, DriversList &drivers_list_from,
                              std::vector<DevGroup> &dev_groups_from, std::vector<Device> &devices_from);
    void printDevGroups2(const DriversList drivers_list, int ListNum, QString group);

    void DeviceSelect(int systemNumber,int grounpNumber);
    void SelectIndiDevice(int systemNumber,int grounpNumber);

    bool indi_Driver_Confirm(QString DriverName);
    void indi_Device_Confirm(QString DeviceName);

    uint32_t clearCheckDeviceExist(QString drivername,bool &isExist);

    void DeviceConnect();
    void AfterDeviceConnect();
    void disconnectIndiServer(MyClient *client);
    void connectIndiServer(MyClient *client);

    //ms
    void INDI_Capture(int Exp_times);

    void INDI_AbortCapture();

    void FocusingLooping();

    void saveFitsAsJPG(QString filename);

    int saveFitsAsPNG(QString fitsFileName);

    void saveGuiderImageAsJPG(cv::Mat Image);

    void refreshGuideImage(cv::Mat image16,QString CFA);

    void strechShowImage(cv::Mat img16,QString CFA,bool AutoStretch,bool AWB,int AutoStretchMode,uint16_t blacklevel,uint16_t whitelevel,double ratioRG,double ratioBG,uint16_t offset,bool updateHistogram);

    void InitPHD2();

    bool connectPHD(void);

    bool call_phd_GetVersion(QString &versionName);

    uint32_t call_phd_StartLooping(void);

    uint32_t call_phd_StopLooping(void);

    uint32_t call_phd_AutoFindStar(void);

    uint32_t call_phd_StartGuiding(void);

    uint32_t call_phd_checkStatus(unsigned char &status);

    uint32_t call_phd_setExposureTime(unsigned int expTime);

    uint32_t call_phd_whichCamera(std::string Camera);

    uint32_t call_phd_ChackControlStatus(int sdk_num);

    uint32_t call_phd_ClearCalibration(void);

    void ShowPHDdata();

    void ControlGuide(int Direction, int Duration);

    void GetPHD2ControlInstruct();

    void getTimeNow(int index);

    int glExpTime;

    bool one_touch_connect = true;
    bool one_touch_connect_first = true;
    int glMainCCDSizeX,glMainCCDSizeY;

    bool glIsFocusingLooping;
    QString glMainCameraStatu;
    QElapsedTimer glMainCameraCaptureTimer;

    std::string vueDirectoryPath = "/home/astro/workspace/GitClone/stellarium-web-engine_qscope/apps/web-frontend/dist/img/";

    bool AutoStretch = true;

    QProcess* cmdPHD2;
    int key_phd;
    int shmid_phd;
    bool isGuideCapture = true;
    #define BUFSZ_PHD 16590848
    char *sharedmemory_phd;

    char phd_direction;
    int phd_step;
    double phd_dist;

    QVector<QPoint> glPHD_Stars;
    QVector<QPointF> glPHD_rmsdate;

    bool glPHD_isSelected;
    double glPHD_StarX = 0;
    double glPHD_StarY = 0;
    int glPHD_CurrentImageSizeX;
    int glPHD_CurrentImageSizeY;
    double glPHD_LockPositionX;
    double glPHD_LockPositionY;
    bool glPHD_ShowLockCross;
    bool glPHD_StartGuide = false;

    int glROI_x;
    int glROI_y;
    int CaptureViewWidth;
    int CaptureViewHeight;
    int BoxSideLength = 500;
    
    double FWHM;

    bool InGuiding = false;

    QThread *PHDControlGuide_thread = nullptr;
    QTimer *PHDControlGuide_threadTimer = nullptr;

    std::mutex receiveMutex;

    Q_SLOT void onPHDControlGuideTimeout();

    QThread* m_thread = nullptr;
    QTimer* m_threadTimer = nullptr;

    Q_SLOT void onTimeout();

    int currentSpeed = 3;
    int currentSteps = 5000;
    int CurrentPosition = 0;
    int TargetPosition = 0;
    bool isMoving = false;
    bool MoveInward = true;
    int AutoMovePosition;

    QVector<QPointF> dataPoints;

    void FocusMove(bool isInward, int steps);

    void FocusMoveToPosition(int position);

    void FocuserControl_Move(bool isInward, int steps);

    int  FocuserControl_setSpeed(int speed);
    int  FocuserControl_getSpeed();

    int FocuserControl_getPosition();

    bool TelescopeControl_Park();

    bool TelescopeControl_Track();

    void TelescopeControl_Home();

    void TelescopeControl_SYNCHome();

private slots:
    void onMessageReceived(const QString &message);
    // void sendMessage(QString message);

private:
    // WebSocketClient *websocket;
    WebSocketThread *wsThread;

    MyClient *indi_Client;
    QProcess *glIndiServer;
};

#endif // MAINWINDOW_H
