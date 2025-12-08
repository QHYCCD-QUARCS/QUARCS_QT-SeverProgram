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
#include "Logger.h"

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
  int BaudRate{9600};  // 9600,19200,38400,57600,115200,230400
  INDI::BaseDevice *dp;
  bool isConnect = false;
  bool isBind = false;
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
  double crpix0, crpix1;  // 参考像素
  double crval0, crval1;  // 参考天球坐标
  double cd11, cd12, cd21, cd22;  // 变换矩阵
};

struct FieldOfView {
    double width;          // 视场宽度(度)
    double height;         // 视场高度(度)
    double calculatedWidth;  // 计算的宽度
    double calculatedHeight; // 计算的高度
    double imageWidth;       // 图像宽度
    double imageHeight;      // 图像高度
    double ra_min;         // 最小赤经
    double ra_max;         // 最大赤经
    double dec_min;        // 最小赤纬
    double dec_max;        // 最大赤纬
    double ra_center;      // 中心赤经
    double dec_center;     // 中心赤纬
    double orientation;    // 方向角
    double area;           // 视场面积
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
  int exposureDelay;      //曝光延迟（毫秒），相邻两张曝光之间额外等待的时间
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

struct CamBin
{
  int camxbin;
  int camybin;
};

// 定义 Bayer 模式的枚举类型
enum BayerPattern {
  BAYER_RGGB = 0,  // 红-绿 绿-蓝
  BAYER_BGGR = 1,  // 蓝-绿 绿-红
  BAYER_GRBG = 2,  // 绿-红 蓝-绿
  BAYER_GBRG = 3   // 绿-蓝 红-绿
};


class Tools : public QObject {
  Q_OBJECT
  Q_DISABLE_COPY(Tools)
 protected:
  static Tools* instance_;

 public:
  static Tools* getInstance() {
    return instance_;
  }



  static void Initialize();
  static void Release();

  static uint8_t MSB(uint16_t i);
  static uint8_t LSB(uint16_t i);

  static DriversList& driversList();

  static bool LoadSystemListFromXml(const QString& fileName);
  static void SaveSystemListToXml(const QString& fileName);
  static void InitSystemDeviceList();
  static void CleanSystemDeviceListConnect();
  static int  GetTotalDeviceFromSystemDeviceList();
  static bool getIndexFromSystemDeviceListByName(const QString& devname, int& index);
  static void ClearSystemDeviceListItem(int index);
  static SystemDeviceList& systemDeviceList();

  static void readDriversListFromFiles(const std::string &filename, DriversList &drivers_list_from,std::vector<DevGroup> &dev_groups, std::vector<Device> &devices);
  static void printDevGroups2(const DriversList driver_list);
  static void startIndiDriver(QString driver_name);
  static void stopIndiDriver(QString driver_name);
  static void printSystemDeviceList(const SystemDeviceList& s);
  static QStringList getCameraNumFromSystemDeviceList(const SystemDeviceList& s);

  static void makeConfigFile();
  static void makeImageFolder();
  static void saveSystemDeviceList(SystemDeviceList deviceList);
  static SystemDeviceList readSystemDeviceList();
  static void saveExpTimeList(QString List);
  static QString readExpTimeList();
  static void saveCFWList(QString Name, QString List);
  static QString readCFWList(QString Name);

  // ---------- Schedule presets (任务计划表预设) ----------
  /**
   * @brief 保存任务计划表预设到独立文件
   * @param name  预设名称（作为文件名的一部分）
   * @param data  任务计划表原始数据（不含前缀，如一串以 '[' 分隔的行）
   *
   * 路径示例：config/schedules/<name>.sched
   */
  static void saveSchedulePreset(const QString &name, const QString &data);

  /**
   * @brief 读取指定名称的任务计划表预设
   * @param name  预设名称
   * @return 成功返回任务计划表数据（与 StagingScheduleData: 后面的格式一致），失败返回空字符串
   */
  static QString readSchedulePreset(const QString &name);

  /**
   * @brief 列出所有已保存的任务计划表预设名称
   * @return 名称列表（不含扩展名）
   */
  static QStringList listSchedulePresets();

  /**
   * @brief 删除指定名称的任务计划表预设
   * @param name  预设名称
   * @return 删除成功返回 true，失败或不存在返回 false
   */
  static bool deleteSchedulePreset(const QString &name);

  static void saveDSLRsInfo(DSLRsInfo DSLRsInfo);
  static DSLRsInfo readDSLRsInfo(QString Name);

  static void readClientSettings(const std::string& fileName, std::unordered_map<std::string, std::string>& config);
  static void saveClientSettings(const std::string& fileName, const std::unordered_map<std::string, std::string>& config);

  static void stopIndiDriverAll(const DriversList driver_list);

  static uint32_t readFitsHeadForDevName(std::string filename,QString &devname);

  static void clearSystemDeviceListItem(SystemDeviceList &s,int index);                          //
  static void initSystemDeviceList(SystemDeviceList &s);                                         //
  static int getTotalDeviceFromSystemDeviceList(const SystemDeviceList& s); 
  static int getDriverNumFromSystemDeviceList(const SystemDeviceList& s);                            //
  static void cleanSystemDeviceListConnect(SystemDeviceList &s);                                 //
  static uint32_t getIndexFromSystemDeviceListByName(const SystemDeviceList& s,QString devname,int &index);   //
  static int readFits(const char* fileName, cv::Mat& image);

  static QString getFitsCaptureTime(const char* fileName);

  static int readFits_(const char* fileName, cv::Mat& image);

  // ---------------- Focused star detection (C++ 实现，来自 findstars.py 的合焦算法) ----------------
  struct FocusedStar {
    double x;       // 星心 x（全图坐标）
    double y;       // 星心 y（全图坐标）
    double flux;    // 前景总光通量
    double hfr;     // Half Flux Radius
    double radius;  // 几何半径（像素）
    int area;       // 像素个数
    double snr;     // 信噪比
    QString snrQuality; // 信噪比评价
  };

  /**
   * @brief 检测合焦小星（完整C++实现，不依赖Python）
   * @param image16  原始16位（或8/16/32F可兼容）单通道图像，cv::Mat
   * @param kSigma   阈值倍数（默认 3.5）
   * @param minArea  最小面积（默认 3）
   * @param maxArea  最大面积（默认 200，若 <=0 则表示不限制最大面积）
   * @param minSNR   最小SNR阈值（默认 3.0）
   * @param bgKsize  背景估计高斯核大小（默认 51，奇数）
   * @param smoothSigma 前景轻微平滑sigma（默认 1.0）
   * @param verbose  输出调试信息
   * @return         通过噪声过滤与去重后的合焦星点列表
   */
  static std::vector<FocusedStar> DetectFocusedStars(const cv::Mat& image16,
                                                     double kSigma = 3.5,
                                                     int minArea = 3,
                                                     int maxArea = 200,
                                                     double minSNR = 3.0,
                                                     int bgKsize = 51,
                                                     double smoothSigma = 1.0,
                                                     bool verbose = false);

  /**
   * @brief 从FITS文件直接检测合焦小星（C++实现）
   * @param fileName FITS 文件路径（UTF-8 C字符串）
   * @param outStars 输出星点列表
   * @param verbose  输出调试信息
   * @return 0 表示成功；非0为CFITSIO或读取错误码
   */
  static int DetectFocusedStarsFromFITS(const char* fileName,
                                        std::vector<FocusedStar>& outStars,
                                        bool verbose = false);

  /**
   * @brief 从星点集合计算中位数HFR（若为空返回0）
   */
  static double MedianHFR(const std::vector<FocusedStar>& stars);

  // QHYCCD Camera
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

  // Image process
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
  static cv::Mat convert8UTo16U_BayerSafe(const cv::Mat& mat8u, bool scaleRange = true);
  static void CvDebugShow(cv::Mat img, const std::string& name = "test");
  static void CvDebugSave(cv::Mat img, const std::string& name = "test.png");

  static cv::Mat SubBackGround(cv::Mat image);
  // static bool DetectStar(cv::Mat image, double threshold, int minArea, cv::Rect& starRect);
  static FWHM_Result CalculateFWHM(cv::Mat image);
  static HFR_Result CalculateHFR(cv::Mat image);

  static cv::Mat CalMoments(cv::Mat image);

  static QList<FITSImage::Star> FindStarsByStellarSolver(bool AllStars, bool runHFR);
  static QList<FITSImage::Star> FindStarsByQHYCCDSDK(bool AllStars, bool runHFR);
  // 基于C++合焦算法的星点识别（仿照上面两个接口）
  static QList<FITSImage::Star> FindStarsByFocusedCpp(bool AllStars, bool runHFR);
  
  /**
   * @brief 从文件路径读取FITS图像并识别星点数量
   * @param fileName FITS文件路径
   * @param AllStars 是否返回所有星点（true）或只返回部分星点（false）
   * @param runHFR 是否计算HFR
   * @return 识别到的星点数量，如果失败返回-1
   */
  static int FindStarsCountFromFile(QString fileName, bool AllStars = true, bool runHFR = false);

  static bool findStarsByPython_Process(QString filename);
  static double getLastHFR();

  /**
   * @brief 使用 calculatestars.py 脚本计算单张 FITS 图像的 median_HFR
   * @param filename FITS 文件路径
   * @return 是否成功启动并运行脚本（即使未解析到有效数值也返回 true，由上层根据返回的数值是否大于0决定是否参与拟合）
   */
  static bool findMedianHFRByPython_Process(QString filename);

  /**
   * @brief 获取最近一次通过 calculatestars.py 计算得到的 median_HFR
   *
   * 目前内部与 getLastHFR() 共享同一存储（g_lastHFR），只是提供一个语义更清晰的别名。
   */
  static double getLastMedianHFR();

  // 使用新的 findstars.py 脚本计算 avg_top50_snr（用于粗调/精调）
  static bool findSNRByPython_Process(QString filename);
  static double getLastSNR();

  static loadFitsResult loadFits(QString fileName);

  static void SaveMatTo8BitJPG(cv::Mat image);

  static void SaveMatTo16BitPNG(cv::Mat image);

  static void SaveMatToFITS(const cv::Mat& image);

  static CamBin mergeImageBasedOnSize(cv::Mat image);

  static cv::Mat processMatWithBinAvg(cv::Mat& image, uint32_t camxbin, uint32_t camybin, bool isColor, bool isAVG);

  static uint32_t PixelsDataSoftBin_AVG(uint8_t *srcdata, uint8_t *bindata, uint32_t width, uint32_t height, uint32_t depth, uint32_t camxbin, uint32_t camybin);

  static cv::Mat PixelsDataSoftBin_Bayer(cv::Mat srcMat, uint32_t camxbin, uint32_t camybin, BayerPattern bayerPattern);

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
  static MinMaxFOV calculateFOV(int FocalLength,double CameraSize_width,double CameraSize_height);
  static bool WaitForPlateSolveToComplete();
  static bool isSolveImageFinish();
  static bool isPlateSolveInProgress();
  static bool PlateSolve(QString filename, int FocalLength,double CameraSize_width,double CameraSize_height, bool USEQHYCCDSDK, int mode = 1, double lastRA = 0.0, double lastDEC = 0.0);
  // mode: 0=基础模式, 1=包含视场参数, 2=包含视场和位置参数
  // lastRA: 上次解析的赤经，单位为度 (0-360°)
  // lastDEC: 上次解析的赤纬，单位为度 (-90° to +90°)
  // 智能回退: 模式2→1→0, 模式1→0, 根据参数可用性自动选择最优模式
  static SloveResults ReadSolveResult(QString filename, int imageWidth, int imageHeight);
  static WCSParams extractWCSParams(const QString& wcsInfo);
  static FieldOfView extractFieldOfViewFromWcsInfo(const QString& wcsInfo);
  static SphericalCoordinates pixelToRaDec(double x, double y, const WCSParams& wcs);
  static std::vector<SphericalCoordinates> getFOVCorners(const WCSParams& wcs, int imageWidth, int imageHeight);
  static SphericalCoordinates xy2rdByExternal(const QString& wcsFile, double x, double y, bool& ok);

  static StelObjectSelect getStelObjectSelectName();

  static StelObjectSelect getTargetRaDecFromStel(std::string SearchName);

  // 通用二次曲线拟合（不做离群点剔除），供已有功能复用
  static int fitQuadraticCurve(const QVector<QPointF>& data, float& a, float& b, float& c);

  // 专门用于自动对焦阶段的二次曲线拟合：在拟合前自动剔除离群点
  static int fitQuadraticCurveForAutoFocus(const QVector<QPointF>& data, float& a, float& b, float& c);

  static double calculateRSquared(QVector<QPointF> data, float a, float b, float c);

  static void saveParameter(const QString& deviceCategory, const QString& functionCategory, const QString& parameterValue);
  static QMap<QString, QString> readParameters(const QString& deviceCategory);
  
public slots:
  void StellarSolverLogOutput(QString text);

  SloveResults onSolveFinished(int exitCode);

signals:
    void parseInfoEmitted(const QString& message);

 private:
  Tools();
  ~Tools();

  QList<FITSImage::Star> FindStarsByStellarSolver_(bool AllStars, const FITSImage::Statistic &imagestats, const uint8_t *imageBuffer, bool runHFR);
  QList<FITSImage::Star> FindStarsByQHYCCDSDK_(bool AllStars, const FITSImage::Statistic &imagestats, const uint8_t *imageBuffer, bool runHFR);


};

#endif  // TOOLS_HPP