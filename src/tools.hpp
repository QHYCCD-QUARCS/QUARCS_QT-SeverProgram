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

struct FWHM_Result
{
  /* data */
  cv::Mat image;
  double FWHM;
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
  QString Description{};
  int DeviceIndiGroup{};  // number or text.  if number, using the INDI grounp// number standard
  QString DeviceIndiName{};  //"QHY CCD QHY268M-XXXX"
  QString DriverIndiName;    //"indi_qhy_ccd"  or "libqhyccd"
  QString DriverFrom{};      // INDI,ASCOM,NATIVE.
  INDI::BaseDevice *dp;
  // void* dp = nullptr;
  bool isConnect = false;
};

struct SystemDeviceList {
  QVector<SystemDevice> system_devices{};
  int currentDeviceCode = -1;
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

struct SloveResults
{
  double RA_Degree;
  double DEC_Degree;
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

enum class SystemNumber {
  Mount = 0,
  Guider = 1,
  PoleCamera = 2,
  MainCamera1 = 20,
  CFW1 = 21,
  Focuser1 = 22,
  LensCover1 = 23,
};

class Tools {
  Q_DISABLE_COPY(Tools)
 public:
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
  static bool GetIndexFromSystemDeviceList(const QString& devname, int& index);
  static void ClearSystemDeviceListItem(int index);
  static SystemDeviceList& systemDeviceList();

  static void readDriversListFromFiles(const std::string &filename, DriversList &drivers_list_from,std::vector<DevGroup> &dev_groups, std::vector<Device> &devices);
  static void printDevGroups2(const DriversList driver_list);
  static void startIndiDriver(QString driver_name);
  static void stopIndiDriver(QString driver_name);
  static void printSystemDeviceList(SystemDeviceList s);
  static void saveSystemDeviceList(SystemDeviceList deviceList);
  static SystemDeviceList readSystemDeviceList();
  static void stopIndiDriverAll(const DriversList driver_list);

  static uint32_t readFitsHeadForDevName(std::string filename,QString &devname);

  static void clearSystemDeviceListItem(SystemDeviceList &s,int index);                          //
  static void initSystemDeviceList(SystemDeviceList &s);                                         //
  static int getTotalDeviceFromSystemDeviceList(SystemDeviceList s);                             //
  static void cleanSystemDeviceListConnect(SystemDeviceList &s);                                 //
  static uint32_t getIndexFromSystemDeviceList(SystemDeviceList s,QString devname,int &index);   //

  static int readFits(const char* fileName, cv::Mat& image);

  static int readFits_(const char* fileName, cv::Mat& image);

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
  static void CvDebugShow(cv::Mat img, const std::string& name = "test");
  static void CvDebugSave(cv::Mat img, const std::string& name = "test.png");

  static cv::Mat SubBackGround(cv::Mat image);
  static FWHM_Result CalculateFWHM(cv::Mat image);

  static cv::Mat CalMoments(cv::Mat image);

  static double getDecAngle(const QString& str);

  static QDateTime getSystemTimeUTC(void);

  static double rangeTo(double value, double max, double min);
  static double getLST_Degree(QDateTime datetimeUTC, double longitude_radian);
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

  static MinMaxFOV calculateFOV(int FocalLength,double CameraSize_width,double CameraSize_height);
  static SloveResults PlateSlove(int FocalLength,double CameraSize_width,double CameraSize_height, double Ra_Degree, double Dec_Degree, bool USEQHYCCDSDK);

  static StelObjectSelect getStelObjectSelectName();

  static StelObjectSelect getTargetRaDecFromStel(std::string SearchName);

  static int fitQuadraticCurve(const QVector<QPointF>& data, float& a, float& b, float& c);

 private:
  Tools();
  ~Tools();

  static Tools* instance_;
};

#endif  // TOOLS_HPP