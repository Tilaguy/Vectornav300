#include <iostream>
#include <cstdio>
//#include <cmath>
#include <stdlib.h>
#include <string.h>
//#define PI 3.14159265358979323846  /* pi */

// ROS Libraries
#include "ros/ros.h"
#include "sensor_msgs/Imu.h"
#include "sensor_msgs/MagneticField.h"
#include "sensor_msgs/NavSatFix.h"
#include "nav_msgs/Odometry.h"
#include "sensor_msgs/Temperature.h"
#include "sensor_msgs/FluidPressure.h"
#include "diagnostic_msgs/KeyValue.h"
#include "std_srvs/Empty.h"
#include <tf2/LinearMath/Transform.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>

ros::Publisher pubIMU, pubMag, pubGPS, pubOdom, pubTemp, pubPres, ConnStatus;
ros::ServiceServer resetOdomSrv;

// Custom user data to pass to packet callback function
struct UserData{
  int device_family;
};

// Include this header file to get access to VectorNav sensors.
#include "vn/sensors.h"
#include "vn/util.h"
#include "vn/compositedata.h"
#include "vn/matrix.h"

// We need this file for our sleep function.
#include "vn/thread.h"

using namespace std;
using namespace vn::math;
using namespace vn::sensors;
using namespace vn::protocol::uart;
using namespace vn::xplat;

//Unused covariances initilized to zero's
boost::array<double, 9ul> linear_accel_covariance = { };
boost::array<double, 9ul> angular_vel_covariance = { };
boost::array<double, 9ul> orientation_covariance = { };
vec3f Antenna_A_offset;
vec3f baseline_position;
XmlRpc::XmlRpcValue rpc_temp;

// Global Variables
bool flag = 1; // Falg to indicate the first time of execution
bool flag2 = 1; // Falg to indicate the first time of execution
bool flag1 = 0; // Falg to indicate the first time of execution
int Perc = 0;
vec3d pos_o;

diagnostic_msgs::KeyValue msgKey;


// Method declarations for future use.
void asciiOrBinaryAsyncMessageReceived(void* userData, Packet& p, size_t index);
void ConnectionState(void *userData, const char *rawData, size_t length, size_t runningIndex);
void ClearCharArray(char *Data, int length);
vec3d ECEF2ENU(vec3d posECEF, vec3d lla);

std::string frame_id;
bool tf_ned_to_enu;
bool frame_based_enu;
bool ENU_flag;


// Basic loop so we can initilize our covariance parameters above
boost::array<double, 9ul> setCov(XmlRpc::XmlRpcValue rpc){
  // Output covariance vector
  boost::array<double, 9ul> output = { 0.0 };

  // Convert the RPC message to array
  ROS_ASSERT(rpc.getType() == XmlRpc::XmlRpcValue::TypeArray);

  for (int i = 0; i < 9; i++){
    ROS_ASSERT(rpc[i].getType() == XmlRpc::XmlRpcValue::TypeDouble);
    output[i] = (double)rpc[i];
  }
  return output;
}

// Basic loop so we can initilize our baseline position configuration above
vec3f setPos(XmlRpc::XmlRpcValue rpc){
  // Output covariance vector
  vec3f output(0.0f, 0.0f, 0.0f);

  // Convert the RPC message to array
  ROS_ASSERT(rpc.getType() == XmlRpc::XmlRpcValue::TypeArray);

  for (int i = 0; i < 3; i++){
    ROS_ASSERT(rpc[i].getType() == XmlRpc::XmlRpcValue::TypeDouble);
    output[i] = (double)rpc[i];
  }
  return output;
}

int main(int argc, char *argv[]){

  // ROS node init
  ros::init(argc, argv, "vectornav");
  ros::NodeHandle n;
  ros::NodeHandle pn("~");

  pubIMU = n.advertise<sensor_msgs::Imu>("vectornav/IMU", 1000);
  pubMag = n.advertise<sensor_msgs::MagneticField>("vectornav/Mag", 1000);
  pubGPS = n.advertise<sensor_msgs::NavSatFix>("vectornav/GPS", 1000);
  pubOdom = n.advertise<nav_msgs::Odometry>("vectornav/Odom", 1000);
  pubTemp = n.advertise<sensor_msgs::Temperature>("vectornav/Temp", 1000);
  pubPres = n.advertise<sensor_msgs::FluidPressure>("vectornav/Pres", 1000);
  ConnStatus =  n.advertise<diagnostic_msgs::KeyValue>("vectornav/ConnStatus", 1000);

  // Serial Port Settings
  string SensorPort;
  int SensorBaudrate;
  int async_output_rate;

  // Sensor IMURATE (800Hz by default, used to configure device)
  int SensorImuRate;

  // Load all params
  // pn.param<type_of_data>(Param_name, Param_value, default_value)
  pn.param<std::string>("frame_id", frame_id, "vectornav");
  pn.param<bool>("tf_ned_to_enu", tf_ned_to_enu, false);
  pn.param<bool>("frame_based_enu", frame_based_enu, false);
  pn.param<bool>("ECEF2ENU", ENU_flag, false);
  pn.param<int>("async_output_rate", async_output_rate, 20);
  pn.param<std::string>("serial_port", SensorPort, "/dev/ttyUSB0");
  pn.param<int>("serial_baud", SensorBaudrate, 115200);
  pn.param<int>("fixed_imu_rate", SensorImuRate, 800);

  //Call to set covariances
  if (pn.getParam("linear_accel_covariance", rpc_temp))
    linear_accel_covariance = setCov(rpc_temp);
  if (pn.getParam("angular_vel_covariance", rpc_temp))
    angular_vel_covariance = setCov(rpc_temp);
  if (pn.getParam("orientation_covariance", rpc_temp))
    orientation_covariance = setCov(rpc_temp);

  //Call to set antenna A offset
  if (pn.getParam("Antenna_A_offset", rpc_temp))
    Antenna_A_offset = setPos(rpc_temp);
  //Call to set baseline position configuration
  if (pn.getParam("baseline_position", rpc_temp))
    baseline_position = setPos(rpc_temp);

  ROS_INFO("Connecting to: %s @ %d Baud", SensorPort.c_str(), SensorBaudrate);
  // This example walks through using the VectorNav C++ Library to connect to
  // and interact with a VectorNav sensor.

  // Now let's create a VnSensor object and use it to connect to our sensor.
  VnSensor vs;
  vs.connect(SensorPort, SensorBaudrate);

  // Now we verify connection (Should be good if we made it this far)
  if (vs.verifySensorConnectivity())
    ROS_INFO("Device connection established");
  else
    ROS_ERROR("No device communication");

  // Let's query the sensor's model number.
  string mn = vs.readModelNumber();
  ROS_INFO("Model Number: %s\n", mn.c_str());

  ROS_INFO("Restarting factory configuration .....................................");
  vs.restoreFactorySettings();
  Thread::sleepSec(3);

  // Let's do some simple reconfiguration of the sensor. As it comes from the
  // factory, the sensor outputs asynchronous data at 40 Hz.
  ROS_INFO("New resgister configuration.........................................");
  vs.writeAsyncDataOutputFrequency(async_output_rate); // see table 6.2.8
  uint32_t newHz = vs.readAsyncDataOutputFrequency();

  //added by Harold
  ImuRateConfigurationRegister IMUR = vs.readImuRateConfiguration();
  IMUR.imuRate = SensorImuRate;
  vs.writeImuRateConfiguration(IMUR);
  IMUR = vs.readImuRateConfiguration();

  /* VPE Resgister */
  VpeBasicControlRegister vpeReg = vs.readVpeBasicControl();
  // Enable *********************************************************************
  vpeReg.enable = VPEENABLE_ENABLE;
  // Heading Mode ***************************************************************
  vpeReg.headingMode = HEADINGMODE_RELATIVE;
  // vpeReg.headingMode = HEADINGMODE_ABSOLUTE;
  // vpeReg.headingMode = HEADINGMODE_INDOOR;
  // Filtering Mode *************************************************************
  //vpeReg.filteringMode = VPEMODE_OFF;
  vpeReg.filteringMode = VPEMODE_MODE1;
  // Tuning Mode ****************************************************************
  // vpeReg.tuningMode = VPEMODE_OFF;
  vpeReg.tuningMode = VPEMODE_MODE1;
  vs.writeVpeBasicControl(vpeReg);

  /* INS Resgister */
  InsBasicConfigurationRegisterVn300 InsReg = vs.readInsBasicConfigurationVn300();
  // Scenario *******************************************************************
  //InsReg.scenario = SCENARIO_AHRS;
  InsReg.scenario = SCENARIO_INSWITHPRESSURE;
  //InsReg.scenario = SCENARIO_INSWITHOUTPRESSURE;
  //InsReg.scenario = SCENARIO_GPSMOVINGBASELINEDYNAMIC;
  //InsReg.scenario = SCENARIO_GPSMOVINGBASELINESTATIC;
  // Ahrs Aiding ****************************************************************
  InsReg.ahrsAiding = 1;
  // Estimation Base line *******************************************************
  InsReg.estBaseline = 1;
  vs.writeInsBasicConfigurationVn300(InsReg); //added by Harold

  /* HIS Calibration */
  MagnetometerCalibrationControlRegister hsiReg = vs.readMagnetometerCalibrationControl();
  // HSI Mode *******************************************************************
  hsiReg.hsiMode = HSIMODE_RUN;
  //hsiReg.hsiMode = HSIMODE_OFF;
  // HSI Output *****************************************************************
  hsiReg.hsiOutput = HSIOUTPUT_USEONBOARD;
  //hsiReg.hsiOutput = HSIOUTPUT_NOONBOARD;
  vs.writeMagnetometerCalibrationControl(hsiReg);  //added by Harold

  /* BaseLine Configuration */
  /// BaseLine and Antenna A offset Configuration
  vs.writeGpsAntennaOffset(Antenna_A_offset);
  GpsCompassBaselineRegister baseli_config = vs.readGpsCompassBaseline();
  baseli_config.position = baseline_position;
  // Uncertainty calculation
  float max = 0;
  for (int i = 0; i < 3; i++){
    if (baseli_config.position[i] > max)
      max = baseli_config.position[i];
  }
  baseli_config.uncertainty = {max * 0.025, max * 0.025, max * 0.025};
  vs.writeGpsCompassBaseline(baseli_config);

  /* Save on flash memory all configurations */
  vs.writeSettings();

  /* --------------------------------------- */
  /* ------- Read all configurations ------- */
  /* --------------------------------------- */
  ROS_INFO("Async output frequency:\t%d Hz", newHz);
  ROS_INFO("IMU Frequency:\t\t%d Hz", IMUR.imuRate);
  /* VPE Resgister */
  ROS_INFO("...VPE Resgister....................................................");
  vpeReg = vs.readVpeBasicControl();
  ROS_INFO("Enable:\t\t%d", vpeReg.enable);
  ROS_INFO("Heading Mode:\t%d", vpeReg.headingMode);
  ROS_INFO("Filtering Mode:\t%d", vpeReg.filteringMode);
  ROS_INFO("Tuning Mode:\t%d", vpeReg.tuningMode);
  /* INS Resgister */
  ROS_INFO("...INS Resgister....................................................");
  InsReg = vs.readInsBasicConfigurationVn300(); //added by Harold
  ROS_INFO("Scenario:\t%d", InsReg.scenario);
  ROS_INFO("AHRS Aiding:\t%d", InsReg.ahrsAiding);
  ROS_INFO("Base Line:\t%d", InsReg.estBaseline);
  /* HIS Calibration */
  ROS_INFO("...HIS Calibration..................................................");
  hsiReg = vs.readMagnetometerCalibrationControl();//added by Harold
  ROS_INFO("Mode:\t%d", hsiReg.hsiMode);
  ROS_INFO("Output:\t%d\n", hsiReg.hsiOutput);
  /* BaseLine Configuration */
  ROS_INFO("BaseLine Configuration..............................................");
  vec3f Ant_offset = vs.readGpsAntennaOffset();
  ROS_INFO("Antena A offset:\t[%.2f, %.2f, %.2f]", Ant_offset[0], Ant_offset[1], Ant_offset[2]);
  baseli_config = vs.readGpsCompassBaseline();
  ROS_INFO("Position:\t\t[%.2f, %.2f, %.2f]", baseli_config.position[0], baseli_config.position[1], baseli_config.position[2]);
  ROS_INFO("Uncertainty:\t\t[%.4f, %.4f, %.4f]\n", baseli_config.uncertainty[0], baseli_config.uncertainty[1], baseli_config.uncertainty[2]);
  ROS_INFO("Convertion Configuration..............................................");
  if (ENU_flag) ROS_INFO("XYZ output mode: ENU");
  else          ROS_INFO("XYZ output mode: ECEF");
  if (tf_ned_to_enu) ROS_INFO("Quaternion orientation mode: ENU");
  else               ROS_INFO("Quaternion orientation mode: END");
  if (frame_based_enu && tf_ned_to_enu) ROS_INFO("Rotation mode: Mathematically\n");
  else if (!frame_based_enu && tf_ned_to_enu) ROS_INFO("Rotation mode: Manually\n");
  else                 ROS_INFO("Rotation mode: No apply\n");
  cout << endl << endl << endl << endl;

  BinaryOutputRegister bor(
    ASYNCMODE_PORT1,
    SensorImuRate / async_output_rate,  // update rate [ms]
    COMMONGROUP_QUATERNION
    | COMMONGROUP_ANGULARRATE
    | COMMONGROUP_POSITION
    | COMMONGROUP_ACCEL
    | COMMONGROUP_MAGPRES,
    TIMEGROUP_NONE,
    IMUGROUP_NONE,
    GPSGROUP_NONE,
    ATTITUDEGROUP_YPRU, //<-- returning yaw pitch roll uncertainties
    INSGROUP_INSSTATUS
    | INSGROUP_POSLLA
    | INSGROUP_POSECEF
    | INSGROUP_VELBODY
    | INSGROUP_ACCELECEF,
    GPSGROUP_NONE);

  msgKey.key = "ConnStatus[%]";
  vs.registerRawDataReceivedHandler(NULL, ConnectionState);
  ROS_INFO("Initial calibration ................................................");

  while (flag && flag2){
    vs.send("$VNRRG,98");
    vs.send("$VNRRG,86");
    ConnStatus.publish(msgKey);
  }

  vs.unregisterRawDataReceivedHandler();
  Thread::sleepSec(2);
  vs.writeBinaryOutput1(bor);

  vs.registerAsyncPacketReceivedHandler(NULL, asciiOrBinaryAsyncMessageReceived);
  ROS_INFO("bound..............................................................");
  while (!flag2){
    ConnStatus.publish(msgKey);
  }

  vs.unregisterAsyncPacketReceivedHandler();

  vs.disconnect();

  return 0;
}

void asciiOrBinaryAsyncMessageReceived(void* userData, Packet& p, size_t index){
  // By default, the orientation of IMU is NED (North East Down).
  vn::sensors::CompositeData cd = vn::sensors::CompositeData::parse(p);
  sensor_msgs::MagneticField msgMag;
  sensor_msgs::Imu msgIMU;
  sensor_msgs::NavSatFix msgGPS;
  nav_msgs::Odometry msgOdom;
  sensor_msgs::Temperature msgTemp;
  sensor_msgs::FluidPressure msgPres;
  // IMU
  msgIMU.header.stamp = ros::Time::now();
  msgIMU.header.frame_id = frame_id;
  if (cd.hasQuaternion() && cd.hasAngularRate() && cd.hasAcceleration()){
    vec4f q = cd.quaternion();
    vec3f ar = cd.angularRate();
    vec3f acel = cd.acceleration();

    if (cd.hasAttitudeUncertainty()){
      vec3f orientationStdDev = cd.attitudeUncertainty();
      msgIMU.orientation_covariance[0] = orientationStdDev[1] * orientationStdDev[1];
      msgIMU.orientation_covariance[4] = orientationStdDev[0] * orientationStdDev[0];
      msgIMU.orientation_covariance[8] = orientationStdDev[2] * orientationStdDev[2];
    }
    //Quaternion message comes in as a Yaw (z) pitch (y) Roll (x) format
    if (tf_ned_to_enu){
      // If we want the orientation to be based on the reference label on the imu
      tf2::Quaternion tf2_quat(q[0],q[1],q[2],q[3]);
      geometry_msgs::Quaternion quat_msg;

      if(frame_based_enu){
        // Create a rotation from NED -> ENU
        tf2::Quaternion q_rotate;
        q_rotate.setRPY (M_PI, 0.0, M_PI/2);
        // Apply the NED to ENU rotation such that the coordinate frame matches
        tf2_quat = q_rotate*tf2_quat;
        quat_msg = tf2::toMsg(tf2_quat);

        // Since everything is in the normal frame, no flipping required
        msgIMU.angular_velocity.x = ar[0];
        msgIMU.angular_velocity.y = ar[1];
        msgIMU.angular_velocity.z = ar[2];

        msgIMU.linear_acceleration.x = acel[0];
        msgIMU.linear_acceleration.y = acel[1];
        msgIMU.linear_acceleration.z = acel[2];
      }
      else{
        // put into ENU - swap X/Y, invert Z
        quat_msg.x = q[1];
        quat_msg.y = q[0];
        quat_msg.z = -q[2];
        quat_msg.w = q[3];

        // Flip x and y then invert z
        msgIMU.angular_velocity.x = ar[1];
        msgIMU.angular_velocity.y = ar[0];
        msgIMU.angular_velocity.z = -ar[2];
        // Flip x and y then invert z
        msgIMU.linear_acceleration.x = acel[1];
        msgIMU.linear_acceleration.y = acel[0];
        msgIMU.linear_acceleration.z = -acel[2];

        if (cd.hasAttitudeUncertainty()){
          vec3f orientationStdDev = cd.attitudeUncertainty();
          msgIMU.orientation_covariance[0] = orientationStdDev[1]*orientationStdDev[1]*M_PI/180; // Convert to radians pitch
          msgIMU.orientation_covariance[4] = orientationStdDev[0]*orientationStdDev[0]*M_PI/180; // Convert to radians Roll
          msgIMU.orientation_covariance[8] = orientationStdDev[2]*orientationStdDev[2]*M_PI/180; // Convert to radians Yaw
        }
      }
      msgIMU.orientation = quat_msg;
    }
    else
    {
        msgIMU.orientation.x = q[0];
        msgIMU.orientation.y = q[1];
        msgIMU.orientation.z = q[2];
        msgIMU.orientation.w = q[3];

        msgIMU.angular_velocity.x = ar[0];
        msgIMU.angular_velocity.y = ar[1];
        msgIMU.angular_velocity.z = ar[2];
        msgIMU.linear_acceleration.x = acel[0];
        msgIMU.linear_acceleration.y = acel[1];
        msgIMU.linear_acceleration.z = acel[2];
    }
    msgOdom.pose.pose.orientation.x = msgIMU.orientation.x;
    msgOdom.pose.pose.orientation.y = msgIMU.orientation.y;
    msgOdom.pose.pose.orientation.z = msgIMU.orientation.z;
    msgOdom.pose.pose.orientation.w = msgIMU.orientation.w;

    msgOdom.twist.twist.linear.x = msgIMU.angular_velocity.x;
    msgOdom.twist.twist.linear.y = msgIMU.angular_velocity.y;
    msgOdom.twist.twist.linear.z = msgIMU.angular_velocity.z;

    msgOdom.twist.twist.angular.x = msgIMU.linear_acceleration.x;
    msgOdom.twist.twist.angular.y = msgIMU.linear_acceleration.y;
    msgOdom.twist.twist.angular.z = msgIMU.linear_acceleration.z;
    // Covariances pulled from parameters
    msgIMU.angular_velocity_covariance = angular_vel_covariance;
    msgIMU.linear_acceleration_covariance = linear_accel_covariance;
    pubIMU.publish(msgIMU);
  }
  if (cd.hasYawPitchRoll()){
    vec3f ypr = cd.yawPitchRoll();
    //cout << "Binary Async YPR: " << ypr << endl;
  }

  // Magnetic Field
  if (cd.hasMagnetic()){
    vec3f mag = cd.magnetic();
    msgMag.header.stamp = msgIMU.header.stamp;
    msgMag.header.frame_id = msgIMU.header.frame_id;
    msgMag.magnetic_field.x = mag[0];
    msgMag.magnetic_field.y = mag[1];
    msgMag.magnetic_field.z = mag[2];
    pubMag.publish(msgMag);
    //cout << "Binary Async MagneticField: " << mag << endl;
  }

  // GPS
  msgGPS.header.stamp = msgIMU.header.stamp;
  msgGPS.header.frame_id = frame_id;
  if (cd.hasPositionEstimatedLla() && cd.hasPositionEstimatedEcef() && cd.hasInsStatus()){
    vec3d lla = cd.positionEstimatedLla();
    msgGPS.latitude = lla[0];
    msgGPS.longitude = lla[1];
    msgGPS.altitude = lla[2];
    pubGPS.publish(msgGPS);
    // cout << "Binary Async GPS_LLA: " << lla << endl;

    msgOdom.header.stamp = msgIMU.header.stamp;
    msgOdom.header.frame_id = msgIMU.header.frame_id;
    vec3d pos = cd.positionEstimatedEcef();
    if (!flag1){
      pos_o = pos;
      flag1 = 1;
    }
    //pos -= pos_o;
    if (ENU_flag){
      pos = ECEF2ENU(pos,lla);
    }
    msgOdom.pose.pose.position.x = pos[0];
    msgOdom.pose.pose.position.y = pos[1];
    msgOdom.pose.pose.position.z = pos[2];
    // cout << "Binary Async GPS_ECEF: " << pos << endl;

    pubOdom.publish(msgOdom);
  }

  // Temperature
  if (cd.hasTemperature()){
    float temp = cd.temperature();
    msgTemp.header.stamp = msgIMU.header.stamp;
    msgTemp.header.frame_id = msgIMU.header.frame_id;
    msgTemp.temperature = temp;
    pubTemp.publish(msgTemp);
    //cout << "Binary Async Temperature: " << temp << endl;
  }

  // Barometer
  if (cd.hasPressure()){
    float pres = cd.pressure();
    msgPres.header.stamp = msgIMU.header.stamp;
    msgPres.header.frame_id = msgIMU.header.frame_id;
    msgPres.fluid_pressure = pres;
    //cout << "Binary Async Pressure: " << pres << endl;
    pubPres.publish(msgPres);
  }

}

void ConnectionState(void *userData, const char *rawData, size_t length, size_t runningIndex){
  // cout << rawData << endl;
  char header1[] = "$VNRRG,98,"; // header of GNSS Compass Startup Status
  char header2[] = "$VNRRG,86,"; // header of GNSS Compass Signal Health Status
  int j = 0;
  //cout << rawData << endl;
  // Decode register 98
  for (int i = 0; i < length; i++){
    if ((rawData[i] == header1[j])){
      j++;
      if (j == 10){
        char Perc_char[3] = {rawData[i + 1], rawData[i + 2], rawData[i + 3]};
        Perc = atoi(Perc_char);
        msgKey.value = to_string(Perc);
        i = length;
        if (Perc == 100)
          flag2 = 0;
      }
    }
    else{
      j = 0;
    }
  }

  // Decode register 86
  float PVT_A = 0, RTK_A = 0, CN0_A = 0, PVT_B = 0, RTK_B = 0, CN0_2 = 0, ComPVT = 0, ComRTK = 0;
  j = 0;
  char aux[5];  // Save part of data of the message
  int aux1;     // Save the actual index
  int flag3 = 0;
  ClearCharArray(aux, 5);
  for (int i = 0; i < length; i++){
    if ((rawData[i] == header2[j])){
      j++;
      if (j == 10){
        //cout << *rawData.substr(i,i+10) << endl;
        i++;
        for (int k = 0; k < 5; k++){
          // cout << "flag3: " << flag3 << endl;
          // PVT_A
          if ((rawData[i + k] != ',') && (flag3 == 0)){
            //cout << rawData[i+k] << endl;
            aux[k] = rawData[i + k];
          }
          else if ((rawData[i + k] == ',') && (flag3 == 0)){
            PVT_A = atof(aux);
            //cout << aux << endl;
            ClearCharArray(aux, 5);
            //cout << PVT_A << endl;
            flag3++;
            i += (k + 1);
            k = 0;
          }
          // RTK_A
          if ((rawData[i + k] != ',') && (flag3 == 1)){
            //cout << rawData[i+k] << endl;
            aux[k] = rawData[i + k];
          }
          else if ((rawData[i + k] == ',') && (flag3 == 1)){
            RTK_A = atof(aux);
            //cout << aux << endl;
            ClearCharArray(aux, 5);
            //cout << RTK_A << endl;
            flag3++;
            i += (k + 1);
            k = 0;
          }
          // CN0_A
          if ((rawData[i + k] != ',') && (flag3 == 2)){
            //cout << rawData[i+k] << endl;
            aux[k] = rawData[i + k];
          }
          else if ((rawData[i + k] == ',') && (flag3 == 2)){
            CN0_A = atof(aux);
            //cout << aux << endl;
            ClearCharArray(aux, 5);
            //cout << CN0_A << endl;
            flag3++;
            i += (k + 1);
            k = 0;
          }
          // PVT_B
          if ((rawData[i + k] != ',') && (flag3 == 3)){
            //cout << rawData[i+k] << endl;
            aux[k] = rawData[i + k];
          }
          else if ((rawData[i + k] == ',') && (flag3 == 3)){
            PVT_B = atof(aux);
            //cout << aux << endl;
            ClearCharArray(aux, 5);
            //cout << PVT_B << endl;
            flag3++;
            i += (k + 1);
            k = 0;
          }
          // RTK_B
          if ((rawData[i + k] != ',') && (flag3 == 4)){
            //cout << rawData[i+k] << endl;
            aux[k] = rawData[i + k];
          }
          else if ((rawData[i + k] == ',') && (flag3 == 4)){
            RTK_B = atof(aux);
            //cout << aux << endl;
            ClearCharArray(aux, 5);
            //cout << RTK_B << endl;
            flag3++;
            i += (k + 1);
            k = 0;
          }
          // CN0_2
          if ((rawData[i + k] != ',') && (flag3 == 5)){
            //cout << rawData[i+k] << endl;
            aux[k] = rawData[i + k];
          }
          else if ((rawData[i + k] == ',') && (flag3 == 5)){
            CN0_2 = atof(aux);
            //cout << aux << endl;
            ClearCharArray(aux, 5);
            //cout << CN0_2 << endl;
            flag3++;
            i += (k + 1);
            k = 0;
          }
          // ComPVT
          if ((rawData[i + k] != ',') && (flag3 == 6)){
            //cout << rawData[i+k] << endl;
            aux[k] = rawData[i + k];
          }
          else if ((rawData[i + k] == ',') && (flag3 == 6)){
            ComPVT = atof(aux);
            //cout << aux << endl;
            ClearCharArray(aux, 5);
            //cout << ComPVT << endl;
            flag3++;
            i += (k + 1);
            k = 0;
          }
          // ComRTK
          if ((rawData[i + k] != '*') && (flag3 == 7)){
            //cout << rawData[i+k] << endl;
            aux[k] = rawData[i + k];
          }
          else if ((rawData[i + k] == '*') && (flag3 == 7)){
            ComRTK = atof(aux);
            //cout << aux << endl;
            ClearCharArray(aux, 5);
            //cout << ComRTK << endl;
            k = 1000;
          }
        }
        if(Perc != 100){
          // VT100 scape codes
          // http://www.climagic.org/mirrors/VT100_Escape_Codes.html
          printf("\033[5A\r");
          printf("\033[J\r");
        }
        printf("Startup - %3d %\n",Perc);
        cout << "PVT_A: " << PVT_A << "\tRTK_A: " << RTK_A << endl;
        cout << "PVT_B: " << PVT_B << "\tRTK_B: " << RTK_B << endl;
        cout << "ComPVT: " << ComPVT << "\tComRTK: "  << ComRTK << endl;
        cout << "CNO_A: " << CN0_A << "dBHz\tCNO_B: " << CN0_2 << "dBHz" << endl;
      }
    }
    else{
      j = 0;
    }
  }
}

void ClearCharArray(char *Data, int length){
  for (int i = 0; i < length; i++){
    Data[i] = 0;
  }
}

vec3d ECEF2ENU(vec3d posECEF, vec3d lla){
  #include <math.h>
  // Convert ECEF to NED
  // https://www.mathworks.com/help/aeroblks/directioncosinematrixeceftoned.html
  double lat_r = lla[0]*M_PI/180.0;// Lat in rad
  double lon_r = lla[1]*M_PI/180.0;// Long in rad
  cout << "lat:" << lat_r << ", lon:" << lon_r << endl;
  mat3d DCM;
  DCM.e00 = -1.0*sin(lat_r)*cos(lon_r);
  DCM.e01 = -1.0*sin(lat_r)*sin(lon_r);
  DCM.e02 = cos(lat_r);
  DCM.e10 = -1.0*sin(lon_r);
  DCM.e11 = cos(lon_r);
  DCM.e12 = 0.0;
  DCM.e20 = -1.0*cos(lat_r)*cos(lon_r);
  DCM.e21 = cos(lat_r)*sin(lon_r);
  DCM.e22 = -1.0*sin(lat_r);
  vec3d pos_ENU;
  pos_ENU.x = DCM.e00*posECEF.x + DCM.e01*posECEF.y + DCM.e02*posECEF.z;
  pos_ENU.y = DCM.e10*posECEF.x + DCM.e11*posECEF.y + DCM.e12*posECEF.z;
  pos_ENU.z = DCM.e20*posECEF.x + DCM.e21*posECEF.y + DCM.e22*posECEF.z;
  cout << DCM << endl;
  cout << pos_ENU << endl;
  //return pos_ENU;
}
