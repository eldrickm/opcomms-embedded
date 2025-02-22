//Packages by default with Arduino
#include <Wire.h>
#include <EEPROM.h>

//MUST BE DOWNLOADED
#include <Adafruit_Sensor.h>
#include <Adafruit_BNO055.h>
#include <utility/imumaths.h>
#include "nelder_mead.h"

/*****************************************************************
 *
 *  COMMAND DOCUMENTATION
 *
 *  <value> means that value is sent, with no spaces or < > characters
 *
 *  H<number> - engages hypersampling mode, which averages the specified number of samples before returning a value. "H1" returns the system to normal behavior
 *  Q - Returns Azimuth Position <space> Altitude Position <space> Voltage on Analog 0 (connected to sensor)
 *  Z - Returns Azimuth Position <space> Altitude Position <space> Voltage on Analog 0 (connected to sensor), with no error checking on position from the arm
 *  S - Returns only voltage on Analog 0 (connected to sensor)
 *  V - Toggles persistent output (continuously sends Q command, reporting position and sensor voltage)
 *  I - Toggles IMU readout
 *  M - Toggles less verbose IMU readout
 *  F - Program offsets for IMU (reset to 0 at startup)
 *  A - Auto-align, still very much a work in progress.
 *  b - Debug feature - prints the relative power in the Fourier transform at the beacon frequenccy
 *  l - Debug feature - beacons the laser at the beacon frequency for 20 minutes. Blocking, because why not.
 *
 *  Any single digit 0-9 - Sets default movement speed to that value (9 is fast, 4 is slow, 3 and below do not move)
 *  L - Azimuth motor turns left at default speed (and persists)
 *  R - Azimuth motor turns right at default speed (and persists)
 *  U - Altitude motor turns up at default speed (and persists)
 *  D - Altitude motor turns down at default speed (and persists)
 *  X - Both motors stop (creates 600ms delay to acheive full stop)
 *  G<azmPos>,<altPos> - Drives arm to specified position. HANGS PROGRAM UNTIL POSITION IS REACHED
 *  E<azmPos>,<altPos> - Drives arm to specified position using IMU. HANGS PROGRAM UNTIL POSITION IS REACHED
 *
 *  ~ - Turns on/off beam hold, keeping laser on persistently
 *    ~0 - Turns beam off
 *    ~1 - Turns beam on
 *  ! - Turns on 1 Hz laser pulse
 *  B - Blinks LED on board. HANGS PROGRAM FOR 200 ms
 *
 *  P - Allows for in-test redefinition of PPM parameters (use is discouraged)
 *  ><message> - Transmits message over PPM
 *  < - Waits to receive message over PPM. HANGS PROGRAM UNTIL MESSAGE IS RECEIVED OR 10 SECONDS ELAPSE (whichever comes first. Timeout currently untested)
 *  W - Persistently waits to receive message over PPM. HANGS PROGRAM UNITL 10 SECONDS ELAPSE WITHOUT A MESSAGE (Timeout currently untested)
 *
 *  + - Toggle analogRead precision
 *
 *  * - Wastes time, flashes pretty colors
 *
 *****************************************************************/

#define LASER 5
#define N_BITS 2
#define SENSOR_PIN A0
#define MSG_BUF_LEN 1024

#define AUTOBOOT_IMU true //Set true if calibration values should be loaded by default
//#define AUTOBOOT_IMU false

#define DEBUG false //I'm lazy; debug mode turns on a number of useful things automatically during setup
//#define DEBUG false

//These are not explicitly constant because they are nominally field configurable
int highTime = 100; //us
int lowTime = 100; //us
int sampleTime = 10; //Should be ~10us less than a factor of lowTime
int sampleTimeShiftVal = 2; //Rightshifting is much cheaper than dividing; 2^this is how many samples per interval
int sensorThreshold = 200;

int hypersample = 1; //Number of samples to be taken during each sampleSensor() call; this is explicitly intended to be changed during operation

#define WAIT_FOR_MSG_TIMEOUT 500000 //us

char msgBuf[MSG_BUF_LEN];


//Code to allow microcontroller to control Celestron arm

/*******************************************************************************/

#define RX 2 //Data coming to board from Celestron
#define TX 3 //Data coming from Celestron to board.

/******** IMPORTANT ********/
//TX and RX are electrically connected within the Celestron arm
//When one is in use, the other pin needs to be put into high-impedance (usually input mode does this)

#define EN_PIN 4 //Additional line that both Celestron and the board pull down when they are sending data over the TX/RX line
#define LED 13 //LED pin

#define LED_R 17
#define LED_G 16
#define LED_B 15

#define LED_OFF     0b000
#define LED_RED     0b001
#define LED_GREEN   0b010
#define LED_BLUE    0b100
#define LED_YELLOW  0b011
#define LED_PURPLE  0b101
#define LED_TEAL    0b110
#define LED_WHITE   0b111

#define BITTIME 50 //Time to transmit a bit, in microseconds, Teensy version
//#define BITTIME 45 //Time to transmit a bit, in microseconds, Arduino version
                   //This should really be closer to 50 uSec, but the transmission execution time on a 16MHz Arduino chews up ~5 uSec

#define POSMAX 16777216 //2^24; all angles are represented as 24 bit integers, and dividing by POSMAX converts to a fraction of a complete rotation
#define POS_TOLERANCE 5000 //Max value two position values are allowed to deviate from each other while the system believes that they followed each other
#define TEN_DEGREES_FROM_ZERO 466034 //Read the name, smartass

/******** IMPORTANT ********/
//If Serial seems to be breaking, adjust BITTIIME. The communication expects to run at 19200 baud, and depending on your microcontroller
//the time required to initiate a transition may eat into the ~50 uSec needed for each pulse

#define HIGH_PRECISION 16
#define STANDARD_PRECISION 10

/*******************************************************************************/

//Axis Command Constants
//These define several concepts used in sending commands to and from the Celestron's altitude and azimuth axes

#define POS '%' //37 - rotationally, defined as counterclockwise
#define NEG '$' //36 - rotationally, defined as clockwise
#define AZM 16
#define ALT 17
#define ROLL 18 //Mostly useless; just useful for an IMU position readout method
#define RIGHT NEG + (2*AZM) //68 - Right is a negative azimuth movement
#define LEFT POS + (2*AZM) //69 - Left is a positive azimuth movement
#define DOWN NEG + (2*ALT) //70 - Down is a negative altitude movement
#define UP POS + (2*ALT) //71 - Up is a positive altitude movement

/*******************************************************************************/

//Don't touch; these are pulled straight from Adafruit example code

#define BNO055_SAMPLERATE_DELAY_MS (100)
Adafruit_BNO055 bno = Adafruit_BNO055(55);

/*******************************************************************************/

#define BUFLEN 64
#define CHARBUFLEN 10

signed char posBuf[BUFLEN];
unsigned char charBuf[CHARBUFLEN];
bool waitMode = false;
bool beamHold = false;
bool blinkMode = false;
bool highPrecision = false;

bool saveCoefficientsEnabled = false; //Dangerous; safety must be disabled prior to attempting
bool loadCoefficientsEnabled = false; //Dangerous; safety must be disabled prior to attempting

bool bnoEnabled = true; //True by default; if fails to enable, will be set to false. Initialize to false to completely disable
#define IMU_GOTO_MAX_RECURSIONS 1
double imuAzmOffset = 0.0;
double imuAltOffset = 0.0;
char bnoVerbose = 0; //By default, do not print out position data when queried
#define VERBOSE 1
#define VERY_VERBOSE 2

int globalSpeed = 9;
bool vomitData = false;
bool blinkState = 0;

long currentAzm = -1;
long currentAlt = -1;

#include <TimerOne.h>
#include <TimerThree.h>
volatile bool transmitting = false;

void setup()
{
  //analogReference(INTERNAL);
  Serial.begin(250000);

  Timer1.initialize(100);
  Timer1.attachInterrupt(transmit_timer_tick);
  Timer1.stop();

  Timer3.initialize(25);
  Timer3.attachInterrupt(fill_analog_buffer);
  Timer3.stop();
  

  
  pinMode(EN_PIN, OUTPUT);
  pinMode(TX, OUTPUT);
  pinMode(RX, INPUT_PULLUP);
  pinMode(LED, OUTPUT);
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  pinMode(LASER, OUTPUT);

  digitalWrite(EN_PIN, HIGH);
  digitalWrite(TX, HIGH);
  ledColor(LED_OFF);

  /* Initialise the orientation sensor */
  if(!bnoEnabled || !bno.begin())
  {
    /* There was a problem detecting the BNO055 ... check your connections */
    Serial.print("Ooops, no BNO055 detected ... Check your wiring or I2C ADDR!");
    bnoEnabled = false;
  }

  if(bnoEnabled){
    //bno.setMode(bno.OPERATION_MODE_NDOF); //Initializes IMU to ignore gyro, because gyros suck //DOESN'T ACTUALLY WORK lol
    if(AUTOBOOT_IMU) loadCalibrationConstants(); //Load calibration constants from the EEPROM automatically if requested
  }

  analogReadResolution(STANDARD_PRECISION);

  if(DEBUG){
    vomitData = true;
    bnoVerbose = VERY_VERBOSE;
    beamHold = true;
  }

/*******************************************************************************/
//Initialization code above this line is required for proper startup
//Code below is optional

//Startup LED blink

  ledParty();

  long azmTarget = 1000000;
  long altTarget = 1000000;

  Timer3.start();
}

void loop() // run over and over
{
  decode_msg_buffer();
  if(Serial.available() > 0){
    byte incomingByte = Serial.read();

    if(incomingByte == '+'){
      highPrecision = !highPrecision;
      analogReadResolution(highPrecision ? HIGH_PRECISION : STANDARD_PRECISION);
    }
    if(incomingByte == 'Q') query();
    if(incomingByte == 'Z') query(true);
    if(incomingByte - '0' <= 9 && incomingByte - '0' >= 1) globalSpeed = incomingByte - '0';
    if(incomingByte == 'L') celestronDriveMotor(LEFT, globalSpeed);
    if(incomingByte == 'U') celestronDriveMotor(UP, globalSpeed);
    if(incomingByte == 'D') celestronDriveMotor(DOWN, globalSpeed);
    if(incomingByte == 'R') celestronDriveMotor(RIGHT, globalSpeed);
    if(incomingByte == 'X') celestronStopCmd(false);

    if(incomingByte == 'S') Serial.println(sampleSensor());
    if(incomingByte == 'G') celestronGoToPos(Serial.parseInt(),Serial.parseInt());
    if(incomingByte == 'E') imuGoToPos(double(Serial.parseFloat()),double(Serial.parseFloat()),0);
    if(incomingByte == 'V') vomitData = !vomitData;
    if(incomingByte == 'I') bnoVerbose = bnoVerbose != VERY_VERBOSE ? VERY_VERBOSE : 0;
    if(incomingByte == 'M') bnoVerbose = bnoVerbose != VERBOSE ? VERBOSE : 0;
    if(incomingByte == 'F'){ //Allows for programming of offsets for IMU to correct for assorted errors
      imuAzmOffset = double(Serial.parseFloat());
      imuAltOffset = double(Serial.parseFloat());
    }
    if(incomingByte == 'A'){
      alignAFS();
    }
    if(incomingByte == 'b') {
      double powah = getBeaconPower();
      Serial.println(powah);
    }
    if(incomingByte == 'l') {
      alignBeacon();
    }

    if(incomingByte == '|'){
      if(saveCoefficientsEnabled){
        saveCalibrationConstants(); //DON'T DO IT UNLESS YOU KNOW WHAT YOU'RE DOING
      }else{
        saveCoefficientsEnabled = true;
        if(bnoVerbose) Serial.println("Arming calibration constant readout; type \'|\' again to complete");
      }
    }else if(saveCoefficientsEnabled){
      saveCoefficientsEnabled = false; //If previously armed but subsequent character not '|,' disarm
    }

    if(incomingByte == '\\'){ //Actually just a '\' - it has to be "escaped"
      if(loadCoefficientsEnabled){
        loadCalibrationConstants();
      }else{
        loadCoefficientsEnabled = true;
        if(bnoVerbose) Serial.println("Arming calibration constant loading; type \'\\\' again to complete");
      }
    }else if(loadCoefficientsEnabled){
      loadCoefficientsEnabled = false; //If previously armed but subsequent character not '\,' disarm
    }

    if(incomingByte == '~'){
      if(Serial.available() && (Serial.peek() == '0' || Serial.peek() == '1')){ //Check if next char specifies a laser state; otherwise, ~ behaves normally
        beamHold = Serial.read() == '1'; //If a 0 or 1 is sent, remove it from the Serial buffer so it doesn't get processed next, and set beamHold accordingly
      }else{
        beamHold = !beamHold;
      }
      blinkMode = false;
    }
    if(incomingByte == '!'){
      beamHold = false;
      blinkMode = !blinkMode;
    }

    if(incomingByte == 'B'){
      blinkLED();
    }

    if(incomingByte == 'O'){
      defineParameters();
    }

    if(incomingByte == 'P') defineParameters();

    if(incomingByte == '>'){
      int msgLen = Serial.available(); //Counts number of bytes to be sent
      Serial.readBytes(msgBuf, msgLen);
      //blink_Packet(msgBuf, msgLen);
      Serial.println("Transmitting with Hardware Interrupts");
      transmit_msg(msgBuf, msgLen);
      clearMsgBuf();
    }
    /*
    if(incomingByte == '>'){
      int msgLen = Serial.available(); //Counts number of bytes to be sent
      Serial.readBytes(msgBuf, msgLen);
      blink_Packet(msgBuf, msgLen);
      clearMsgBuf();
    }*/
    if(incomingByte == 'W') waitMode = !waitMode;

    if(incomingByte == 'H'){
      hypersample = 1;

      if(Serial.available()){ //Don't try to parse an int if there is no more material in the Serial buffer
        int desiredSampling = Serial.parseInt();
        if(desiredSampling > 1) hypersample = desiredSampling;
      }
    }

    if(incomingByte == '<'){
      Serial.println("Waiting for msg:");
      while(Serial.available()) Serial.read(); //For no obvious reason, not clearing the Serial buffer prior to listening causes weird timing bugs

      decode_msg_buffer();
      print_message_buffer();
      print_buffer();
    }

    if(incomingByte == '*'){
      while(!Serial.available()) ledParty();
      ledColor(LED_OFF);
    }
  }

  if(!transmitting) digitalWrite(LASER,beamHold ? HIGH : LOW); //AAAAAH THIS LINE WAS HIDING AND COST ME A WHOLE DAY OF MY LIFE. WHY CRUEL WORLD? Must be disabled for hardware interrupts. 
  
  if(blinkMode){
    digitalWrite(LASER, blinkState ? HIGH : LOW);
    blinkState = !blinkState;
    delay(500);
  }

  if(waitMode){
    while(Serial.available()) Serial.read(); //For no obvious reason, not clearing the Serial buffer prior to listening causes weird timing bugs
    int charsRead = listen_for_msg();

    if(charsRead != -1){
      Serial.println(charsRead);
      Serial.print(msgBuf);
      clearMsgBuf();
    }else{
        Serial.println(0);
    }
  }


  if(vomitData){
      query();
   }
}

int sampleSensor(){
  long sumOfSamples; //hacky solution to allow many samples to be safely added together before averaging

  for(int samples = 0; samples < hypersample; samples++){
    sumOfSamples += analogRead(SENSOR_PIN);
  }

  //Don't waste the time to divide if hypersample is a 1, and catch errors if some dumbass - sorry, user - set hypersample to 0
  int averagedValue = hypersample > 1 ? sumOfSamples/(long(hypersample)) : sumOfSamples; //Use longs to try and get more precision

  if(hypersample < 1) blinkLED(500); //Indicate hypersample is out of spec and is being ignored

  return averagedValue;
}

