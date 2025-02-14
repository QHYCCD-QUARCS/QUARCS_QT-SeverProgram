#ifndef TOOLS_HPP
#define TOOLS_HPP

#include <cstdint>
#include <cmath>
#include <QtCore/QtCore>
#include <QtGui/QtGui>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>
#include <qhyccd.h>
#include <dirent.h>
#include <cstdlib>
#include "baseclient.h"
#include "basedevice.h"
#include <QLabel>
#include "fitsio.h"
#include <QApplication>
#include <QObject>
#include <QDebug>

#include <stellarsolver.h>

#define Min2(a, b) ((a) < (b) ? (a) : (b))
#define Max2(a, b) ((a) > (b) ? (a) : (b))
#define LimitByte(v) ((uint8_t)Min2(Max2(v, 0), 0xFF))
#define LimitShort(v) ((uint16_t)Min2(Max2(v, 0), 0xFFFF))

struct FWHM_Result
{
  /* data */
  cv::Mat image;
  double FWHM;
};

struct HFR_Result {
    cv::Mat image;
    double HFR;
};


// three level structure to store the indi list xml structure
struct Device {
  std::string label{};
  std::string manufacturer{};
  std::string driver_name{};
  std::string version{};
};

struct DevGroup {
  QString group{};
  QVector<Device> devices{};
};

struct DriversList {
  QVector<DevGroup> dev_groups{};
  // this value is used in the selector code as a global value to indicate which
  // grounp is selected
  int selectedGrounp = -1;
};

//four level structure
struct DeviceNew {
  std::string label{};
  std::string driver_name{};
  std::string version{};
};

struct ManufacturerNew {
  std::string name{};
  std::vector<DeviceNew> devices{};
};

struct DevGroupNew {
  std::string group{};
  std::vector<ManufacturerNew> manufacturers{};
};

struct DriversListNew {
  std::vector<DevGroupNew> dev_groups{};
};

//used to store the device of the user selected

struct SystemDevice {
  QString Description{};  // Mount MainCamera Guider Focuser
  int DeviceIndiGroup{};  // number or text.  if number, using the INDI grounp// number standard
  QString DeviceIndiName{};  //"QHY CCD QHY268M-XXXX"
  QString DriverIndiName;    //"indi_qhy_ccd"  or "libqhyccd"
  QString DriverFrom{};      // INDI,ASCOM,NATIVE.
  INDI::BaseDevice *dp;
  bool isConnect = false;
};

struct SystemDeviceList {
  QVector<SystemDevice> system_devices{};
  int currentDeviceCode = -1;
};

struct DSLRsInfo
{
  QString Name;
  int SizeX;
  int SizeY;
  double PixelSize;
};

struct DSLRsInfoList
{
  QVector<DSLRsInfo> DSLRsInfoList;
};

struct CartesianCoordinates {
    double x;
    double y;
    double z;
};
struct SphericalCoordinates {
    double ra;
    double dec;
};

struct AltAz {
  double altitude;
  double azimuth;
};

struct WCSParams {
    double crpix0;
    double crpix1;
    double crval0;
    double crval1;
    double cd11;
    double cd12;
    double cd21;
    double cd22;
};

struct SloveResults
{
  double RA_Degree;
  double DEC_Degree;

  double RA_0;
  double DEC_0;
  double RA_1;
  double DEC_1;
  double RA_2;
  double DEC_2;
  double RA_3;
  double DEC_3;
};

struct MinMaxFOV
{
  double minFOV;
  double maxFOV;
};

struct StelObjectSelect
{
  QString name;
  double Ra_Hour;
  double Dec_Degree;
};

struct LocationResult
{
  double latitude_degree;
  double longitude_degree;
  double elevation;
};

struct ScheduleData
{
  QString shootTarget;    //拍摄目标
  double targetRa;
  double targetDec;
  QString shootTime;      //拍摄时间
  int exposureTime;       //曝光时间
  QString filterNumber;   //滤镜轮号
  int repeatNumber;       //重复张数
  QString shootType;      //拍摄类型
  bool resetFocusing;     //重新调焦
  int progress;           //进度
};

struct ConnectedDevice
{
  QString DeviceType;
  QString DeviceName;
};

struct ClientButtonStatus
{
  QString Button;
  QString Status;
};

enum class SystemNumber {
  Mount = 0,
  Guider = 1,
  PoleCamera = 2,
  MainCamera1 = 20,
  CFW1 = 21,
  Focuser1 = 22,
  LensCover1 = 23,
};

typedef struct
{
  QString key;     /** FITS Header Key */
  QVariant value;  /** FITS Header Value */
  QString comment; /** FITS Header Comment, if any */
} Record;

struct loadFitsResult
{
  bool success;
  FITSImage::Statistic imageStats;
  uint8_t *imageBuffer;
};

struct MountStatus
{
  QString status;
  QString error;
};

struct CamBin
{
  int camxbin;
  int camybin;
};

class Tools : public QObject {
  Q_OBJECT
  Q_DISABLE_COPY(Tools)
 public:
  static void Initialize();
  static void Release();

  static uint8_t MSB(uint16_t i);
  static uint8_t LSB(uint16_t i);

  static DriversList& driversList();

  // 从本地文件中读取SystemList
  static bool LoadSystemListFromXml(const QString& fileName);
  // 将SystemList列表保存到本地的xml文件里
  static void SaveSystemListToXml(const QString& fileName);
  // 初始化SystemDeviceList
  static void InitSystemDeviceList();
  // 清理SystemDeviceList中的设备连接状态
  static void CleanSystemDeviceListConnect();
  // 获取SystemDeviceList中的设备数量
  static int  GetTotalDeviceFromSystemDeviceList();
  // 通过设备名字从SystemDeviceList中获取设备的序号
  static bool getIndexFromSystemDeviceListByName(const QString& devname, int& index);
  // 通过序号清理SystemDeviceList中的设备
  static void ClearSystemDeviceListItem(int index);
  static SystemDeviceList& systemDeviceList();

  // 从本地文件中获取设备驱动列表
  static void readDriversListFromFiles(const std::string &filename, DriversList &drivers_list_from,std::vector<DevGroup> &dev_groups, std::vector<Device> &devices);
  // 打印设备列表
  static void printDevGroups2(const DriversList driver_list);
  // 启动INDI驱动
  static void startIndiDriver(QString driver_name);
  // 终止INDI驱动
  static void stopIndiDriver(QString driver_name);
  // 打印设备列表
  static void printSystemDeviceList(SystemDeviceList s);
  // 从SystemDeviceList中获取相机种类数量
  static QStringList getCameraNumFromSystemDeviceList(SystemDeviceList s);

  // 创建配置文件
  static void makeConfigFile();
  // 创建主相机图像保存文件夹
  static void makeImageFolder();
  // 将SystemDeviceList保存到配置文件中
  static void saveSystemDeviceList(SystemDeviceList deviceList);
  // 从配置文件中读取SystemDeviceList
  static SystemDeviceList readSystemDeviceList();
  // 将主相机曝光时间选项列表保存到配置文件中
  static void saveExpTimeList(QString List);
  // 从配置文件中读取主相机曝光时间选项列表
  static QString readExpTimeList();
  // 将滤镜轮选项列表保存到配置文件中
  static void saveCFWList(QString Name, QString List);
  // 从配置文件中读取滤镜轮选项列表
  static QString readCFWList(QString Name);

  // 将单反相机信息保存到配置文件中
  static void saveDSLRsInfo(DSLRsInfo DSLRsInfo);
  // 从配置文件中读取单反相机信息
  static DSLRsInfo readDSLRsInfo(QString Name);

  // 从配置文件中读取客户端设置内容
  static void readClientSettings(const std::string& fileName, std::unordered_map<std::string, std::string>& config);
  // 将客户端设置内容保存到配置文件中
  static void saveClientSettings(const std::string& fileName, const std::unordered_map<std::string, std::string>& config);

  // 终止所有的INDI驱动
  static void stopIndiDriverAll(const DriversList driver_list);

  // 从Fits图的头信息中读取设备名称
  static uint32_t readFitsHeadForDevName(std::string filename,QString &devname);

  // 通过序号清理SystemDeviceList中的设备
  static void clearSystemDeviceListItem(SystemDeviceList &s,int index);                          //
  // 初始化SystemDeviceList
  static void initSystemDeviceList(SystemDeviceList &s);        
  // 获取SystemDeviceList中的设备数量                                 //
  static int getTotalDeviceFromSystemDeviceList(SystemDeviceList s); 
  // 从SystemDeviceList中获取相机种类数量
  static int getDriverNumFromSystemDeviceList(SystemDeviceList s);                            //
  // 清理SystemDeviceList中的设备连接状态
  static void cleanSystemDeviceListConnect(SystemDeviceList &s);                                 //
  // 通过设备名字从SystemDeviceList中获取设备的序号
  static uint32_t getIndexFromSystemDeviceListByName(SystemDeviceList s,QString devname,int &index);   //

  // 读取Fits图中的图像信息
  static int readFits(const char* fileName, cv::Mat& image);

  // 从Fits图中获取图像拍摄时间
  static QString getFitsCaptureTime(const char* fileName);

  // 读取Fits图中的图像信息
  static int readFits_(const char* fileName, cv::Mat& image);

  // QHYCCD Camera(这部分代码与QHYCCD SDK相关，目前软件里没有使用QHYCCD SDK)
  static void ConnectQHYCCDSDK();
  static void ScanCamera();
  static void SelectQHYCCDSDKDevice(int systemNumber);
  static cv::Mat Capture();
  static int CFW();
  static void SetCFW(int cfw);
  static uint32_t& glMainCameraExpTime();
  static bool WriteFPGA(uint8_t hand, int command);
  static char* camid();
  static qhyccd_handle*& camhandle();
  static qhyccd_handle*& guiderhandle();
  static qhyccd_handle*& polerhandle();
  static qhyccd_handle*& fpgahandle();
  static qhyccd_handle*& maincamhandle();  // unused

  // Image process（图像处理这部分功能目前没有在QT服务端进行，图像处理是在Vue客户端里进行的）
  static void CvDebugShow(cv::Mat img);
  static QImage ShowHistogram(const cv::Mat& image,QLabel *label);
  static void PaintHistogram(cv::Mat src,QLabel *label);
  static void HistogramStretch(cv::Mat src,double Min,double Max);
  static void AutoStretch(cv::Mat src);
  static void ShowCvImageOnQLabel(cv::Mat image,QLabel *label);
  static void ShowOpenCV_QLabel_withRotate(cv::Mat img,QLabel *label,int RotateType,double cmdRotate_Degree,double AzAltToRADEC_Degree);
  static void ImageSoftAWB(cv::Mat sourceImg16, cv::Mat& targetImg16,
                           QString CFA, double gainR, double gainB,
                           uint16_t offset);
  static void GetAutoStretch(cv::Mat img_raw16, int mode, uint16_t& B,
                             uint16_t& W);
  static void Bit16To8_MakeLUT(uint16_t B, uint16_t W, uint8_t* lut);
  static void Bit16To8_Stretch(cv::Mat img16, cv::Mat img8, uint16_t B,
                               uint16_t W);
  static void CvDebugShow(cv::Mat img, const std::string& name = "test");
  static void CvDebugSave(cv::Mat img, const std::string& name = "test.png");

  static cv::Mat SubBackGround(cv::Mat image);
  // static bool DetectStar(cv::Mat image, double threshold, int minArea, cv::Rect& starRect);

  // 计算图像中星点的半高宽
  static FWHM_Result CalculateFWHM(cv::Mat image);
  // 计算图像中星点的HFR
  static HFR_Result CalculateHFR(cv::Mat image);

  // 计算一阶矩
  static cv::Mat CalMoments(cv::Mat image);

  // 通过StellarSolver库进行星点识别
  static QList<FITSImage::Star> FindStarsByStellarSolver(bool AllStars, bool runHFR);

  // 加载Fits图
  static loadFitsResult loadFits(QString fileName);

  // 将cv::Mat类型的图像数据保存为本地的JPG图
  static void SaveMatTo8BitJPG(cv::Mat image);

  // 将cv::Mat类型的图像数据保存为本地的PNG图
  static void SaveMatTo16BitPNG(cv::Mat image);

  // 将cv::Mat类型的图像数据保存为本地的FITS图
  static void SaveMatToFITS(const cv::Mat& image);

  // 根据图像大小，选择适合的 bin 值
  static CamBin mergeImageBasedOnSize(cv::Mat image);

  // 图像平均值合并
  static cv::Mat processMatWithBinAvg(cv::Mat& image, uint32_t camxbin, uint32_t camybin, bool isColor, bool isAVG);
  static uint32_t PixelsDataSoftBin_AVG(uint8_t *srcdata, uint8_t *bindata, uint32_t width, uint32_t height, uint32_t depth, uint32_t camxbin, uint32_t camybin);
  // 图像合并
  static uint32_t PixelsDataSoftBin(uint8_t* srcdata, uint8_t* bindata, uint32_t width, uint32_t height, uint32_t camchannels, uint32_t depth, uint32_t camxbin, uint32_t camybin, bool iscolor);

  static double getDecAngle(const QString& str);

  static QDateTime getSystemTimeUTC(void);

  static double rangeTo(double value, double max, double min);
  static double getLST_Degree(QDateTime datetimeUTC, double longitude_radian);
  static bool getJDFromDate(double *newjd, const int y, const int m, const int d, const int h, const int min, const float s);
  static double getHA_Degree(double RA_radian, double LST_Degree);
  static void ra_dec_to_alt_az(double ha_radian, double dec_radian,
                               double& alt_radian, double& az_radian,
                               double lat_radian);
  static void full_ra_dec_to_alt_az(QDateTime datetimeUTC, double ra_radian,
                                    double dec_radian, double latitude_radian,
                                    double longitude_radian, double& alt_radian,
                                    double& az_radian);
  static void alt_az_to_ra_dec(double alt_radian, double az_radian,
                               double& hr_radian, double& dec_radian,
                               double lat_radian);
  static void full_alt_az_to_ra_dec(QDateTime datetimeUTC, double alt_radian,
                                    double az_radian, double latitude_radian,
                                    double longitude_radian, double& ra_radian,
                                    double& dec_radian);
  static void getCurrentMeridianRADEC(double Latitude_radian,
                                      double Longitude_radian,
                                      double& RA_radian, double& DEC_radian);
  static double getCurrentMeridanRA(double Latitude_radian,
                                    double Longitude_radian);
  static bool periodBelongs(double value, double min, double max, double period,
                            bool minequ, bool maxequ);

  static double DegreeToRad(double degree);
  static double RadToDegree(double rad);
  static double HourToDegree(double hour);
  static double HourToRad(double hour);
  static double DegreeToHour(double degree);
  static double RadToHour(double rad);
  static QString getInfoTextA(QDateTime T_local, double RA_DEGREE,
                              double DEC_DEGREE, double dRA_degree,
                              double dDEC_degree, QString MOUNTSTATU,
                              QString GUIDESTATU);
  static QString getInfoTextB(QDateTime T_utc, double AZ_rad, double ALT_rad,
                              QString CAMSTATU, double CAMTemp,
                              double CAMTargetTemp, int CAMX, int CAMY,
                              int CFWPOS, QString CFWNAME, QString CFWSTATU);
  static QString getInfoTextC(int CPUTEMP, int CPULOAD, double DISKFREE,
                              double longitude_rad, double latitude_rad,
                              double Ra_J2000, double Dec_J2000, double Az,
                              double Alt, QString ObjName_);

  // static void MRGOTO_RADEC_rad(StelMovementMgr* mvmgr, double RA, double DEC);
  // static void MRGOTO_AZALT_rad(StelMovementMgr* mvmgr, double AZ, double ALT);
  static CartesianCoordinates convertEquatorialToCartesian(double ra, double dec, double radius); 
  static CartesianCoordinates calculateVector(CartesianCoordinates pointA, CartesianCoordinates pointB);
  static CartesianCoordinates calculatePointC(CartesianCoordinates pointA, CartesianCoordinates vectorV);
  static SphericalCoordinates convertToSphericalCoordinates(CartesianCoordinates cartesianPoint);

  static double calculateGST(const std::tm& date);
  static AltAz calculateAltAz(double ra, double dec, double lat, double lon, const std::tm& date);
  static void printDMS(double angle);
  static double DMSToDegree(int degrees, int minutes, double seconds);
  // 计算视场信息
  static MinMaxFOV calculateFOV(int FocalLength,double CameraSize_width,double CameraSize_height);
  // 判断图像解析是否完成
  static bool WaitForPlateSolveToComplete();
  // 判断图像解析是否完成
  static bool isSolveImageFinish();
  // 图像解析
  static SloveResults PlateSolve(QString filename, int FocalLength,double CameraSize_width,double CameraSize_height, bool USEQHYCCDSDK);
  // 读取图像解析结果
  static SloveResults ReadSolveResult(QString filename, int imageWidth, int imageHeight);
  // 分析WCS属性
  static WCSParams extractWCSParams(const QString& wcsInfo);
  static SphericalCoordinates pixelToRaDec(double x, double y, const WCSParams& wcs);
  // 获取视场的4个角坐标
  static std::vector<SphericalCoordinates> getFOVCorners(const WCSParams& wcs, int imageWidth, int imageHeight);

  // 拟合二次曲线
  static int fitQuadraticCurve(const QVector<QPointF>& data, float& a, float& b, float& c);

  // 计算二次曲线拟合的好坏的指标
  static double calculateRSquared(QVector<QPointF> data, float a, float b, float c);

public slots:
  // StellarSolver库的Debug输出
  void StellarSolverLogOutput(QString text);

  // 图像解析完成的信号
  SloveResults onSolveFinished(int exitCode);

 private:
  Tools();
  ~Tools();

  static Tools* instance_;
  // 通过StellarSolver库进行星点识别
  QList<FITSImage::Star> FindStarsByStellarSolver_(bool AllStars, const FITSImage::Statistic &imagestats, const uint8_t *imageBuffer, bool runHFR);
};

#endif  // TOOLS_HPP