/*
 * ZippyTheBalancingRobot -- Balancing Robot Control Software
 *
 *	  Copyright (C) 2015  Larry McGovern
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License <http://www.gnu.org/licenses/> for more details.
 *
 *    Please also check out Kristian's code at https://github.com/TKJElectronics/Balanduino, which was a
 *    source of inspiration for this software
 */

#include <Wire.h>             // I2C Library
#include <PWM.h>              // PWM Frequency Library at https://code.google.com/archive/p/arduino-pwm-frequency-library/downloads
#include <EnableInterrupt.h>  // Enable Interrupt library
#include <digitalWriteFast.h> // DigitalWriteFast Library
#include <LiquidCrystal.h>    // Liquid Crystal library

// Uncomment CALCTIME to calculate processing time and display on LCD.  
// Time needs to be under 5 msec for reliable performance at 200 Hz

//#define CALCTIME  
/*
  LCD Circuit:
 * #1: LCD RS pin to digital pin 1
 * #2: LCD Enable pin to digital pin 0
 * #3: LCD D4 pin to digital pin 14
 * #4: LCD D5 pin to digital pin 15
 * #5: LCD D6 pin to digital pin 16
 * #6: LCD D7 pin to digital pin 17
 * LCD R/W pin to ground
 * LCD VSS pin to ground
 * LCD VCC pin to 5V
 * 10K resistor:
 * ends to +5V and ground
 * wiper to LCD VO pin (pin 3)
*/

// Pins

#define LCD_Pin1 1
#define LCD_Pin2 0
#define LCD_Pin3 14
#define LCD_Pin4 15
#define LCD_Pin5 16
#define LCD_Pin6 17

#define c_RightEncoderPinA 2
#define c_RightEncoderPinB 4

#define c_LeftEncoderPinA 3
#define c_LeftEncoderPinB 12

#define ThrottlePin 5
#define SteeringPin  6

#define MotorR_B 7
#define MotorR_A 8
#define MotorR_PWM 9

#define MotorL_PWM 10
#define MotorL_A 11
#define MotorL_B 13

#define buttonPin A7
#define voltSensePin A6

LiquidCrystal lcd(LCD_Pin1, LCD_Pin2, LCD_Pin3, LCD_Pin4, LCD_Pin5, LCD_Pin6);

volatile uint8_t bUpdateFlagsShared;
volatile uint16_t _ThrottleInShared;
volatile uint16_t _SteeringInShared;
uint32_t ThrottleStart;
uint32_t SteeringStart;

// Quadrature encoders
// Left encoder
#define LeftEncoderIsReversed
volatile bool _LeftEncoderBSet;
volatile long _LeftEncoderTicks = 0;

// Right encoder
volatile bool _RightEncoderBSet;
volatile long _RightEncoderTicks = 0;

#define DEG_PER_COUNT 0.75f      // 480 Counts per Revolution
#define VOLT_PER_COUNT 0.01465f  // 5 V * (30 kOhm)/(10 kOhm) / 1024

#define THROTTLE_FLAG 1
#define STEERING_FLAG 2
#define LEFTENC_FLAG 4
#define RIGHTENC_FLAG 8

#define DT 5000 // 5 msec time step
#define FHz 200 // Sample Rate

/*
 *  Control Gains
 */
#define PWMFREQ 10000
#define K_p 15.0f
#define K_i 100.0f
#define K_d 0.15f
#define wheelPosGain 0.0015f
#define wheelRateGain 0.003f
#define Kp_Rotation 0.1f
#define MaxAngleOffset 5.5f

uint32_t LastTime = 0;

float MotorScaleFactor = 0.01f;
bool RechargeBattery = 0;
 
// Gyro Address
static const uint8_t IMU = 0x68; // I2C address of the MPU-6050
uint8_t i2cBuffer[14];

float accAngle, gyroRate;
float PitchEst, BiasEst;

float gyroYzero;
float IntState = 0;  // Integral State

float voltageFilt = 0;

void setup(void)
{  
  lcd.begin(16, 2);
  lcd.print("Lay Zippy down");
  lcd.setCursor(0, 1);
  lcd.print("and press button");

  // Motor PWM Setup
  InitTimersSafe();  // Initialize timers for new PWM frequency
  SetPinFrequencySafe(MotorL_PWM, PWMFREQ);
  SetPinFrequencySafe(MotorR_PWM, PWMFREQ);

  // RC Receiver Setup
  pinMode(ThrottlePin, INPUT);
  digitalWrite(ThrottlePin, HIGH); //use the internal pullup resistor
  pinMode(SteeringPin, INPUT);
  digitalWrite(SteeringPin, HIGH); //use the internal pullup resistor
  
  enableInterrupt(ThrottlePin, calcThrottle, CHANGE);
  enableInterrupt(SteeringPin, calcSteering, CHANGE);
  
  // Quadrature encoder Setup
  // Left encoder
  pinMode(c_LeftEncoderPinA, INPUT);      // sets pin A as input
  digitalWrite(c_LeftEncoderPinA, LOW);  // turn on pulldown resistor
  pinMode(c_LeftEncoderPinB, INPUT);      // sets pin B as input
  digitalWrite(c_LeftEncoderPinB, LOW);  // pulldown resistor
  enableInterrupt(c_LeftEncoderPinA, HandleLeftMotorInterruptA, RISING);
  
  // Right encoder
  pinMode(c_RightEncoderPinA, INPUT);      // sets pin A as input
  digitalWrite(c_RightEncoderPinA, LOW);  // pulldown resistor
  pinMode(c_RightEncoderPinB, INPUT);      // sets pin B as input
  digitalWrite(c_RightEncoderPinB, LOW);  // pulldown resistor
  enableInterrupt(c_RightEncoderPinA, HandleRightMotorInterruptA, RISING);
  
  // Set up Gyro
  Wire.begin();
  Wire.setClock(400000UL); // Set I2C frequency to 400kHz

  while (i2cWrite(0x6B, 0x80, true)); // Reset device, this resets all internal registers to their default values
  do {
    while (i2cRead(0x6B, i2cBuffer, 1));
  } while (i2cBuffer[0] & 0x80); // Wait for the bit to clear
  delay(5);
  while (i2cWrite(0x6B, 0x09, true)); // PLL with X axis gyroscope reference, disable temperature sensor and disable sleep mode

  i2cBuffer[0] = 4; // Set the sample rate to 200Hz = 1kHz/(1+4)
  i2cBuffer[1] = 0x03; // Disable FSYNC and set 44 Hz Acc filtering, 42 Hz Gyro filtering, 1 KHz sampling
  i2cBuffer[2] = 0x00; // Set Gyro Full Scale Range to ±250deg/s
  i2cBuffer[3] = 0x00; // Set Accelerometer Full Scale Range to ±2g
  while (i2cWrite(0x19, i2cBuffer, 4, true)); // Write to all four registers at once
  delay(100); // Wait for the sensor to get ready

  /* Calibrate gyro zero value */
  
  while (analogRead(buttonPin) < 800);
  lcd.clear();
  lcd.print("Calibrating Gyro");
  
  while (calibrateGyro()); // Run again if the robot is moved while calibrating

  standUpRobot();  // Ask user to stand up robot and push button when ready

}

/*
 *  Main Loop
 */
void loop(void)
{
  int i;
  static unsigned long int timer, encoderTimer, voltageTimer, voltageTimerOut;
  static int throttle_glitch_persistent, steer_glitch_persistent;
  static float Error, wheelVelocity;
  static double wheelPosition, lastWheelPosition, rotationAngle;
  static unsigned long int WheelSpeedTimer;
  static double PosCmd, rotationCmd;
  static float AOCmd;
  static uint16_t ThrottleIn, SteeringIn, ThrottleInGood, SteeringInGood, bUpdateFlags;
  float ThrottleF, SteeringF;
  static long LeftEncoderTicks, RightEncoderTicks;
  float TorqueCMD;
  static float TurnTorque;
  int dutyCycle_L = 0, dutyCycle_R = 0;
  float AngleOffset;
  static unsigned long int CalcTime, MaxCalcTime, AveCalcTime, TimeCounter;
  static int NumSamples;

  // Turn off interrupts and copy over data which changes during interrupts
  if (bUpdateFlagsShared) {
    noInterrupts();
    bUpdateFlags = bUpdateFlagsShared;
    if (bUpdateFlags & THROTTLE_FLAG) {
      ThrottleIn = _ThrottleInShared;
    }
    if (bUpdateFlags & STEERING_FLAG)  {
      SteeringIn = _SteeringInShared;
    }
    if (bUpdateFlags & LEFTENC_FLAG) {
      LeftEncoderTicks = _LeftEncoderTicks;
    }
    if (bUpdateFlags & RIGHTENC_FLAG) {
      RightEncoderTicks = _RightEncoderTicks;
    }
    bUpdateFlagsShared = 0;
    interrupts(); // we have local copies of the inputs, so now we can turn interrupts back on
  }

  // Read and filter gyro data
  readIMUdata(&accAngle, &gyroRate);
  KalmanFilter(accAngle, gyroRate);

  // Calculate Wheel Position & Speed at 10 Hz
  if (millis() - WheelSpeedTimer > 100) {
    wheelPosition = 0.5f * (LeftEncoderTicks + RightEncoderTicks) * DEG_PER_COUNT;
    wheelVelocity = 10.0f * (wheelPosition - lastWheelPosition);
    rotationAngle = 0.5f * (LeftEncoderTicks - RightEncoderTicks) * DEG_PER_COUNT * 0.486f; // Rotation angle in deg.  Note: wheel diam = 104 mm
                                                                                            // and dist between wheels = 214 mm, so rotation 
                                                                                            // angle = 104 / 214 = 0.406 * wheel angle 
    lastWheelPosition = wheelPosition;
    WheelSpeedTimer = millis();
  }

  if (millis() - voltageTimer > 1000) {
    const float voltageFiltConst = 0.73; // K = exp(s*T), where s = 0.05*2*pi (1/20 Hz), and T = 1 sec
    float voltageFiltTemp = voltageFiltConst * voltageFilt + (1 - voltageFiltConst) * (float)analogRead(voltSensePin) * VOLT_PER_COUNT;
    if (!isnan(voltageFiltTemp)) {
      if (voltageFiltTemp > 9.5f && voltageFiltTemp < 13.0f) {
        voltageFilt = voltageFiltTemp;  // Update voltage only if measurement is within reasonable range
        MotorScaleFactor = 0.01f * 11.8f / voltageFilt;  // Adjust motor scale factor for ideal voltage of 11.8 V
      }
    }
    if (voltageFilt < 11.0f) {
      RechargeBattery = 1;
    }
    voltageTimer = millis();
  }
#ifndef CALCTIME
  if (millis() - voltageTimerOut > 10000) {
    lcd.clear();
    if (RechargeBattery) {
      lcd.print("Recharge Battery");
    }
    else {
      lcd.print("Go Zippy Go!!");
    }
    lcd.setCursor(0, 1);
    lcd.print("Battery: ");
    lcd.print(voltageFilt);
    lcd.print(" V");
    voltageTimerOut = millis();
  }
#endif

  if (PitchEst < -45 || PitchEst > 45) {  // Robot has fallen down

    // Disable motors
    digitalWriteFast(MotorL_A, LOW);  
    digitalWriteFast(MotorL_B, LOW);
    digitalWriteFast(MotorR_A, LOW);
    digitalWriteFast(MotorR_B, LOW);
    pwmWrite(MotorL_PWM, 0);
    pwmWrite(MotorR_PWM, 0);

    // Reset integrator and encoder position
    IntState = 0;
    _LeftEncoderTicks = 0; LeftEncoderTicks = 0;
    _RightEncoderTicks = 0; RightEncoderTicks = 0;

    wheelPosition = 0; lastWheelPosition = 0;
    wheelVelocity = 0; WheelSpeedTimer = millis();

    PosCmd = 0; rotationCmd = 0; 

    standUpRobot();  // Ask user to stand robot back up
    return;
  }
  else {
    
    // Filter ThrottleIn and convert to AOCmd
    if (ThrottleIn > 800 && ThrottleIn < 2200 && !RechargeBattery) {  // Valid range
      if ((abs(ThrottleIn - ThrottleInGood) < 200) || (throttle_glitch_persistent > 20)) { // Changes greater than 200 are assumed to be a glitch
        ThrottleInGood = ThrottleIn;
        throttle_glitch_persistent = 0;
      }
      else {
        throttle_glitch_persistent++;
      }
      ThrottleF = (float)ThrottleInGood;
      AOCmd = (ThrottleF - 1500.0f) / 75.0f; // SteeringIn ranges from 1000 to 2000.  Rescale to -6.7 to +6.7 deg
    }
    else {
      throttle_glitch_persistent++;
      if (throttle_glitch_persistent > 20) {
        throttle_glitch_persistent = 0;
      }
      AOCmd = 0;
    }
    AngleOffset = constrain(AOCmd, -MaxAngleOffset, MaxAngleOffset);  // Limits throttle input
    
    if (abs(AOCmd) > 1.0f) { // If angle offset command greater than 1 deg, then commanding forward/reverse motion
       PosCmd = wheelPosition + 1.0f * wheelVelocity;  // Set encoder position command to a location ahead of robot, 
                                                      // allowing robot to drift to a new location when stopping
       AngleOffset -=  wheelVelocity * wheelRateGain;
    }
    else { // Stop robot
      // Apply encoder feedback outer loop
      AngleOffset -= (wheelPosition - PosCmd) * wheelPosGain  + wheelVelocity * wheelRateGain;  // PD controller
    }
    AngleOffset = constrain(AngleOffset, -MaxAngleOffset, MaxAngleOffset);  // Additional limiter after outer loop
    
    Error = AngleOffset - PitchEst;
    IntState = IntState + Error / FHz;
    IntState = constrain(IntState, -5.0f, 5.0f);
    
    TorqueCMD = K_p * Error + K_i * IntState - K_d * gyroRate;  // PID Feedback Control

    // Filter SteeringIn and convert to TurnTorque
    if (SteeringIn > 800 && SteeringIn < 2200 && !RechargeBattery) {  // Valid range
      if ((abs(SteeringIn - SteeringInGood) < 200) || (steer_glitch_persistent > 20)) { // Changes greater than 200 are assumed to be a glitch
        SteeringInGood = SteeringIn;
        steer_glitch_persistent = 0;
      }
      else {
        steer_glitch_persistent++;
      }
      SteeringF = SteeringInGood;
      TurnTorque = (SteeringF - 1500.0f) / 20.0f;  // SteeringIn ranges from 1000 to 2000.  Rescale to -25 to 25.
      TurnTorque = constrain(TurnTorque, -25.0f, 25.0f);
      if (abs(TurnTorque) < 5.0f && abs(AOCmd) < 2.0f) {
        TurnTorque += Kp_Rotation * (rotationCmd - rotationAngle);  // Maintain current rotation angle
      }
      else {
        rotationCmd = rotationAngle;
      }
    }
    else {
      steer_glitch_persistent++;
      if (steer_glitch_persistent > 20) {
        steer_glitch_persistent = 0;
      }
      TurnTorque = 0;
    }

    dutyCycle_L = 255 * ((TorqueCMD - TurnTorque) * MotorScaleFactor);
    dutyCycle_R = 255 * ((TorqueCMD + TurnTorque) * MotorScaleFactor);

    if (dutyCycle_L < 0) {
      digitalWriteFast(MotorL_A, LOW);
      digitalWriteFast(MotorL_B, HIGH);
      dutyCycle_L = -dutyCycle_L;
    }
    else {
      digitalWriteFast(MotorL_A, HIGH);
      digitalWriteFast(MotorL_B, LOW);
    }

    if (dutyCycle_R < 0) {
      digitalWriteFast(MotorR_A, LOW);
      digitalWriteFast(MotorR_B, HIGH);
      dutyCycle_R = -dutyCycle_R;
    }
    else {
      digitalWriteFast(MotorR_A, HIGH);
      digitalWriteFast(MotorR_B, LOW);
    }
    if (dutyCycle_L > 255) {
      dutyCycle_L = 255;
    }
    if (dutyCycle_R > 255) {
      dutyCycle_R = 255;
    }
  }

  pwmWrite(MotorL_PWM, dutyCycle_L);
  pwmWrite(MotorR_PWM, dutyCycle_R);

#ifdef CALCTIME
  MaxCalcTime, AveCalcTime, TimeCounter, NumSamples;
  CalcTime = micros() - LastTime;
  if (MaxCalcTime < CalcTime) {
    MaxCalcTime = CalcTime;
  }
  AveCalcTime += CalcTime;
  NumSamples++;
  if ((millis() - TimeCounter) > 1000) {
    TimeCounter = millis();
    AveCalcTime /= NumSamples;
    NumSamples = 0;
    lcd.clear();
    lcd.print("Ave: ");
    lcd.print((float)AveCalcTime / 1000.0f);
    lcd.setCursor(0, 1);
    lcd.print("Max: ");
    lcd.print((float)MaxCalcTime / 1000.0f);
    MaxCalcTime = 0;
    AveCalcTime = 0;
  }
#endif

  while (micros() - LastTime < DT);
  LastTime = micros();
}

// Stand up robot and wait for button push
void standUpRobot() {
  lcd.clear();
  lcd.print("Stand & Push Btn");

  int button_pushed = 0;
  unsigned long timer;
  while (!button_pushed) {  // Calculate and display pitch until button pushed
    timer = millis();
    PitchEst = 0;
    for (int i = 0; i < 50; i++) {  // Average 50 samples
      readIMUdata(&accAngle, &gyroRate);
      PitchEst += accAngle;
      if (analogRead(buttonPin) > 800) {
        button_pushed = 1;
      }
    }
    PitchEst /= 50.0f;
    
    lcd.setCursor(0, 1);
    lcd.print("Pitch: ");
    lcd.print(PitchEst);
    while (millis() < timer + 250) {
      if (analogRead(buttonPin) > 800) {
        button_pushed = 1;
      }
    }
  }
  BiasEst = 0;
  
  for (int i = 0; i < 20; i++) {
    voltageFilt += (float)analogRead(voltSensePin) * VOLT_PER_COUNT;
  }
  voltageFilt /= 20.0f;

  lcd.clear();
  lcd.print("Go Zippy Go!!");
  lcd.setCursor(0, 1);
  lcd.print("Battery: ");
  lcd.print(voltageFilt);
  lcd.print(" V");
  delay(100);
  LastTime = micros();  
}

float KalmanFilter(float PitchMeas, float RateMeas) {
  /*
   * Steady State Kalman Filter -- Hard code gains into function
   *
   * [pitch; bias]_(k+1) = A * [pitch; bias]_(k) + B * rate_meas + G * w
   * pitch = C * [pitch; bias] + v
   *
   * A = [1 -dt; 0 1];
   * B = [dt; 0];
   * C = [1 0];
   *
   * G = [1 0; 0 1];
   * cov(w) = Q;
   * cov(v) = R;
   *
   * Kalman Filter gain solved using dlqe in Octave (GNU version of Matlab), assuming:
   * Q = diag([0.001 0.001]);
   * R = 1.5;
   * dt = 0.01;
   * then: K = dlqe(A,G,C,Q,R) = [0.033810; -0.025380];
   */
  const float dt = 0.005;
  const float K[2] = {0.016905, -0.012690}; // Gains originally calculated for 100 Hz,
  // Divide by 2 for 200 Hz gains
  float y = PitchMeas - PitchEst;
  PitchEst += dt * (RateMeas - BiasEst);
  PitchEst += K[0] * y;
  BiasEst  += K[1] * y;
}

bool calibrateGyro() {
  int16_t gyroYbuffer[50], AcX, AcZ;

  gyroYzero = 0;

  for (uint8_t i = 0; i < 50; i++) {
    readIMUdata(&accAngle, &gyroRate);
    gyroYbuffer[i] = gyroRate;
    gyroYzero += gyroYbuffer[i];
    delay(10);
  }
  if (!checkMinMax(gyroYbuffer, 50, 2000)) {  // Check that min and max differ by no more than 2000 counts (15 deg/sec)
                                              // Note: Zero-rate output spec value is +/- 20 deg/sec, so this value may
                                              // need to be increased for gyros with large drift
    return 1;
  }

  gyroYzero /= (50.0f * 131.0f);
  return 0;
}

bool checkMinMax(int16_t *array, uint8_t length, int16_t maxDifference) { // Used to check that the robot is laying still while calibrating
  int16_t min = array[0], max = array[0];
  for (uint8_t i = 1; i < length; i++) {
    if (array[i] < min)
      min = array[i];
    else if (array[i] > max)
      max = array[i];
  }
  return max - min < maxDifference;
}

/*
 * Interrupt services for RC Inputs (Throttle and Steering)
 */
void calcThrottle()
{
  // if the pin is high, its a rising edge of the signal pulse,
  // so lets record its value
  if (digitalReadFast(ThrottlePin) == HIGH)
  {
    ThrottleStart = micros();
  }
  else
  {
    // else it must be a falling edge, so lets get the time and subtract the time
    // of the rising edge this gives use the time between the rising and falling
    // edges i.e. the pulse duration.
    _ThrottleInShared = (uint16_t)(micros() - ThrottleStart);
  }
  // use set the throttle flag to indicate that a new throttle signal has been received
  bUpdateFlagsShared |= THROTTLE_FLAG;
}

void calcSteering()
{
  if (digitalReadFast(SteeringPin) == HIGH)
  {
    SteeringStart = micros();
  }
  else
  {
    _SteeringInShared = (uint16_t)(micros() - SteeringStart);
    bUpdateFlagsShared |= STEERING_FLAG;
  }
}

/*
 *  Interrupt service routines for the motor's quadrature encoder
 */
void HandleLeftMotorInterruptA()
{
  // Test transition; since the interrupt will only fire on 'rising' we don't need to read pin A
  _LeftEncoderBSet = digitalReadFast(c_LeftEncoderPinB);   // read the input pin

  // and adjust counter + if A leads B
#ifdef LeftEncoderIsReversed
  _LeftEncoderTicks -= _LeftEncoderBSet ? -1 : +1;
#else
  _LeftEncoderTicks += _LeftEncoderBSet ? -1 : +1;
#endif
  bUpdateFlagsShared |= LEFTENC_FLAG;
}

void HandleRightMotorInterruptA()
{
  // Test transition; since the interrupt will only fire on 'rising' we don't need to read pin A
  _RightEncoderBSet = digitalReadFast(c_RightEncoderPinB);   // read the input pin

  // and adjust counter + if A leads B
#ifdef RightEncoderIsReversed
  _RightEncoderTicks -= _RightEncoderBSet ? -1 : +1;
#else
  _RightEncoderTicks += _RightEncoderBSet ? -1 : +1;
#endif
  bUpdateFlagsShared |= RIGHTENC_FLAG;
}

void readIMUdata(float *accAngle, float *gyroRate) {
  while (i2cRead(0x3B, i2cBuffer, 14));
  int16_t AcX = ((i2cBuffer[0] << 8) | i2cBuffer[1]);
  int16_t AcZ = ((i2cBuffer[4] << 8) | i2cBuffer[5]);
  int16_t GyY = ((i2cBuffer[10] << 8) | i2cBuffer[11]);

  *accAngle = -atan2((float)AcX, (float)AcZ) * RAD_TO_DEG;
  *gyroRate = (float)GyY / 131.0f - gyroYzero; // Convert to deg/s
}

uint8_t i2cWrite(uint8_t registerAddress, uint8_t data, bool sendStop) {
  return i2cWrite(registerAddress, &data, 1, sendStop); // Returns 0 on success
}

uint8_t i2cWrite(uint8_t registerAddress, uint8_t *data, uint8_t length, bool sendStop) {
  Wire.beginTransmission(IMU);
  Wire.write(registerAddress);
  Wire.write(data, length);
  return(Wire.endTransmission(sendStop)); // Returns 0 on success
}

uint8_t i2cRead(uint8_t registerAddress, uint8_t *data, uint8_t nbytes) {
  uint32_t timeOutTimer;
  Wire.beginTransmission(IMU);
  Wire.write(registerAddress);
  uint8_t rcode = Wire.endTransmission(false); // Don't release the bus
  if (rcode) {
    return rcode; 
  }
  Wire.requestFrom(IMU, nbytes, (uint8_t)true); // Send a repeated start and then release the bus after reading
  for (uint8_t i = 0; i < nbytes; i++) {
    if (Wire.available())
      data[i] = Wire.read();
    else {
      timeOutTimer = micros();
      while (((micros() - timeOutTimer) < 100) && !Wire.available());
      if (Wire.available())
        data[i] = Wire.read();
      else {
        return 5; // Timeout
      }
    }
  }
  return 0; // Success
}

