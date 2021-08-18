#include "KalmanMPU6050.h"
#include "logdef.h"
#include "sensor.h"
#include "quaternion.h"
#include "vector_3d.h"
#include "sensor_processing_lib.h"

#define DEBUG_INIT()
#define DEBUG_PRINT(x)
#define DEBUG_PRINTLN(x)
#define DEBUG_TS_PRINT(x)
#define DEBUG_TS_PRINTLN(x)

#define sqr(x) x *x
#define hypotenuse(x, y) sqrt(sqr(x) + sqr(y))

// MPU-6050
#define IMU_ADDR 0x68
#define IMU_ACCEL_XOUT_H 0x3B
#define IMU_REG 0x19
#define IMU_PWR_MGMT_1 0x6B

// Kalman Variables
Kalman IMU::kalmanX; // Create the Kalman instances
Kalman IMU::kalmanY;

// Private Variables
double IMU::gyroXAngle = 0;
double IMU::gyroYAngle = 0; // Angle calculate using the gyro only
double IMU::gyroZAngle = 0; // Angle calculate using the gyro only
uint32_t IMU::lastProcessed = 0;

float 	IMU::myrolly = 0;
float 	IMU::myrollz = 0;
float 	IMU::myaccroll = 0;
double  IMU::mypitch = 0;
double  IMU::filterPitch = 0;
double  IMU::filterRoll = 0;

uint64_t IMU::last_rts=0;
double IMU::accelX = 0.0;
double IMU::accelY = 0.0;
double IMU::accelZ = 0.0;
double IMU::gyroX = 0.0;
float  IMU::pitchfilter = 0.0;
float  IMU::rollfilter = 0.0;
double IMU::gyroY = 0.0;
double IMU::gyroZ = 0.0;
double IMU::kalXAngle = 0.0;
double IMU::kalYAngle = 0.0;
float IMU::filterAccRoll = 0.0;
float IMU::filterGyroRoll = 0.0;
Quaternion IMU::att_quat;
vector_ijk IMU::att_vector;
euler_angles IMU::euler;

// Kalman Function Definition

void Kalman_Init(Kalman *kalPointer, double qang, double qbias, double rmeas )
{
	/* We will set the variables like so, these can also be tuned by the user */
	kalPointer->Q_angle = 0.01;
	kalPointer->Q_bias = 0.03;
	kalPointer->R_measure = 0.1;

	kalPointer->angle = 0; // Reset the angle
	kalPointer->bias = 0;  // Reset bias

	kalPointer->P[0][0] = 0; // Since we assume that the bias is 0 and we know the starting angle (use setAngle), the error covariance matrix is set like so - see: http://en.wikipedia.org/wiki/Kalman_filter#Example_application.2C_technical
	kalPointer->P[0][1] = 0;
	kalPointer->P[1][0] = 0;
	kalPointer->P[1][1] = 0;
}

// The angle should be in degrees and the rate should be in degrees per second and the delta time in seconds
double Kalman_GetAngle(Kalman *kalPointer,
		double newAngle, double newRate, double dt)
{
	// KasBot V2  -  Kalman filter module - http://www.x-firm.com/?page_id=145
	// Modified by Kristian Lauszus
	// See my blog post for more information: http://blog.tkjelectronics.dk/2012/09/a-practical-approach-to-kalman-filter-and-how-to-implement-it

	// Discrete Kalman filter time update equations - Time Update ("Predict")
	// Update xhat - Project the state ahead
	/* Step 1 */
	kalPointer->rate = newRate - kalPointer->bias;
	kalPointer->angle += dt * kalPointer->rate;

	// Update estimation error covariance - Project the error covariance ahead
	/* Step 2 */
	kalPointer->P[0][0] += dt * (dt * kalPointer->P[1][1] - kalPointer->P[0][1] - kalPointer->P[1][0] + kalPointer->Q_angle);
	kalPointer->P[0][1] -= dt * kalPointer->P[1][1];
	kalPointer->P[1][0] -= dt * kalPointer->P[1][1];
	kalPointer->P[1][1] += kalPointer->Q_bias * dt;

	// Discrete Kalman filter measurement update equations - Measurement Update ("Correct")
	// Calculate Kalman gain - Compute the Kalman gain
	/* Step 4 */
	kalPointer->S = kalPointer->P[0][0] + kalPointer->R_measure;
	/* Step 5 */
	kalPointer->K[0] = kalPointer->P[0][0] / kalPointer->S;
	kalPointer->K[1] = kalPointer->P[1][0] / kalPointer->S;

	// Calculate angle and bias - Update estimate with measurement zk (newAngle)
	/* Step 3 */
	kalPointer->y = newAngle - kalPointer->angle;
	/* Step 6 */
	kalPointer->angle += kalPointer->K[0] * kalPointer->y;
	kalPointer->bias += kalPointer->K[1] * kalPointer->y;

	// Calculate estimation error covariance - Update the error covariance
	/* Step 7 */
	kalPointer->P[0][0] -= kalPointer->K[0] * kalPointer->P[0][0];
	kalPointer->P[0][1] -= kalPointer->K[0] * kalPointer->P[0][1];
	kalPointer->P[1][0] -= kalPointer->K[1] * kalPointer->P[0][0];
	kalPointer->P[1][1] -= kalPointer->K[1] * kalPointer->P[0][1];

	return kalPointer->angle;
};

void IMU::init()
{
	Kalman_Init(&kalmanX);
	Kalman_Init(&kalmanY);

	MPU6050Read();
	// sleep( 0.1 );
	double roll=0;
	double pitch=0;
	// IMU::RollPitchFromAccel(&roll, &pitch);

	kalmanX.angle = roll; // Set starting angle
	kalmanY.angle = pitch;
	gyroXAngle = roll;
	gyroYAngle = pitch;

	lastProcessed = micros();
	att_quat = quaternion_initialize(1.0,0.0,0.0,0.0);
	att_vector = vector_3d_initialize(0.0,0.0,-1.0);
	euler = { 0,0,0 };
	ESP_LOGD(FNAME, "Finished IMU setup  gyroYAngle:%f ", gyroYAngle);
}



void IMU::read()
{
	double dt=0;
	bool ret=false;
	MPU6050Read();
	uint64_t rts = esp_timer_get_time();
	if( last_rts == 0 )
		ret=true;
	dt = (double)(rts - last_rts)/1000000.0;
	last_rts = rts;
	if( ret )
		return;

	if( getTAS() < 10 ) {
		// On ground, the simple kalman filter worked perfectly.
		double roll, pitch;
		IMU::RollPitchFromAccel(&roll, &pitch);
		ESP_LOGD( FNAME, "RollPitchFromAccel: roll: %f pitch: %f  dt: %f", roll, pitch, dt );

		double gyroXRate, gyroYRate;
		gyroXRate = (double)gyroX; // is already in deg/s
		gyroYRate = (double)gyroY; // dito
		// This fixes the transition problem when the accelerometer angle jumps between -180 and 180 degrees
		if ((pitch < -90 && kalYAngle > 90) ||
				(pitch > 90 && kalYAngle < -90))
		{
			kalmanY.angle = pitch;
			kalYAngle = pitch;
			gyroYAngle = pitch;
		}
		else
		{
			kalYAngle = Kalman_GetAngle(&kalmanY, pitch, gyroYRate, dt); // Calculate the angle using a Kalman filter
		}

		if (abs(kalYAngle) > 90)
			gyroXRate = -gyroXRate;                                   // Invert rate, so it fits the restriced accelerometer reading
		kalXAngle = Kalman_GetAngle(&kalmanX, roll, gyroXRate, dt); // Calculate the angle using a Kalman filter
		filterRoll = kalXAngle;

		gyroXAngle += gyroXRate * dt; // Calculate gyro angle without any filter
		gyroYAngle += gyroYRate * dt;
		//gyroXAngle += kalmanX.rate * dt; // Calculate gyro angle using the unbiased rate
		//gyroYAngle += kalmanY.rate * dt;

		// Reset the gyro angle when it has drifted too much
		if (gyroXAngle < -180 || gyroXAngle > 180)
			gyroXAngle = kalXAngle;
		if (gyroYAngle < -180 || gyroYAngle > 180) {
			gyroYAngle = kalYAngle;
			ESP_LOGD( FNAME, "3: gyroXAngle Y:%f", gyroYAngle );
		}
		filterPitch = kalYAngle;
	}
	else
	{   // Simple kalman algo as above needs adjustment in aircraft with fixed wings, acceleration's differ, esp. in a curve there is no lateral acceleration
		// This part is a deterministic and noise resistant approach for centrifugal force removal
		// 1: exstimate roll angle from Z axis omega plus airspeed
		myrollz = R2D(atan(  (gyroZ *PI/180 * (getTAS()/3.6) ) / 9.81 ));
		// 2: estimate angle of bank from increased acceleration in Z axis
		float posacc=accelZ;
		// only positive G-force is to be considered, curve at negative G is not define
		if( posacc < 1 )
			posacc = 1;
		float aroll = acos( 1 / posacc )*180/PI;
		// estimate sign of acceleration angle from gyro
		float sign_accroll=aroll;
		if( myrollz < 0 )
			sign_accroll = -sign_accroll;
		float akroll = -(myrollz + sign_accroll)/2;

		// Calculate Pitch from Gyro and acceleration
		double pitch;
		PitchFromAccel(&pitch);

		// to get pitch and roll independent of circling, image sensor values into quaternion format
		if( ahrs_gyro_ena.get() ){
			uint16_t ax=(UINT16_MAX/2)*sin(D2R(pitch));
			uint16_t ay=(-(UINT16_MAX/2)*sin(D2R(akroll))) * cos( D2R(pitch) );
			uint16_t az=(int16_t)(-(UINT16_MAX/2)*cos(D2R(akroll))) * cos( D2R(pitch) );

			att_vector = update_fused_vector(att_vector,ax, ay, az,D2R(gyroX),D2R(gyroY),D2R(gyroZ),dt);
			att_quat = quaternion_from_accelerometer(att_vector.a,att_vector.b,att_vector.c);
			euler = quaternion_to_euler_angles(att_quat);
			// treat gimbal lock, limit to 80 deg
			if( euler.roll > 80.0 )
				euler.roll = 80.0;
			if( euler.pitch > 80.0 )
				euler.pitch = 80.0;
			if( euler.roll < -80.0 )
				euler.roll = -80.0;
			if( euler.pitch < -80.0 )
				euler.pitch = -80.0;
		}else{
			kalYAngle = Kalman_GetAngle(&kalmanY, pitch, 0, dt);           // Pacify the raw angle using a Kalman filter
			kalXAngle = Kalman_GetAngle(&kalmanX, akroll, 0, dt);
		}

		if( ahrs_gyro_ena.get() )
			filterRoll =  euler.roll;
		else
			filterRoll = kalXAngle;

		if( ahrs_gyro_ena.get() )
			filterPitch =  euler.pitch;
		else
			filterPitch += (kalYAngle - filterPitch) * 0.2;   // addittional low pass filter

		// ESP_LOGI( FNAME,"Pitch=%.1f Roll=%.1f kalX:%.1f Ay:%d Pitch%.1f", euler.pitch, euler.roll, kalXAngle, (int)(-32768.0*sin(D2R(kalXAngle))), pitch );
	}
}

// IMU Function Definition

void IMU::MPU6050Read()
{
	accelX = -accelG[2];
	accelY = accelG[1];
	accelZ = accelG[0];
	gyroX = -(gyroDPS.z);
	gyroY = gyroDPS.y;
	gyroZ = gyroDPS.x;
}

void IMU::PitchFromAccel(double *pitch)
{
	*pitch = atan2(-accelX, accelZ) * RAD_TO_DEG;
}

void IMU::RollPitchFromAccel(double *roll, double *pitch)
{
	// Source: http://www.freescale.com/files/sensors/doc/app_note/AN3461.pdf eq. 25 and eq. 26
	// atan2 outputs the value of -π to π (radians) - see http://en.wikipedia.org/wiki/Atan2
	// It is then converted from radians to degrees

	*roll = atan((double)accelY / hypotenuse((double)accelX, (double)accelZ)) * RAD_TO_DEG;
	*pitch = atan2((double)-accelX, (double)accelZ) * RAD_TO_DEG;

	ESP_LOGD( FNAME,"Accelerometer Roll: %f  Pitch: %f  (y:%f x:%f)", *roll, *pitch, (double)accelY, (double)accelX );

}
