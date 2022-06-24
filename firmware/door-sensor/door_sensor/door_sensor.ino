#include <MLX90393_raw.h>

#include <Adafruit_MLX90393.h>
#include <Wire.h>

// const int8_t   drdy_pin = 10;
// const uint32_t baudrate = 115200;

// // I2C
// // Arduino A4 = SDA
// // Arduino A5 = SCL
// typedef MLX90393_raw<0, 0, Wire, drdy_pin> MLX;

// void belly_up()
// {
//   // The proper way to error handling is of course NOT
//   // to go belly up. Instead you might want to introduce something
//   // slightly more sophisticated.
//   Serial.println(F("Something failed\n"));
//   while (true)
//     ;
// }

void scan()
{
  byte error, address;
  int  nDevices;

  Serial.println("Scanning...");

  nDevices = 0;
  for (address = 1; address < 127; address++) {
    // The i2c_scanner uses the return value of
    // the Write.endTransmisstion to see if
    // a device did acknowledge to the address.
    Wire.beginTransmission(address);
    error = Wire.endTransmission();

    if (error == 0) {
      Serial.print("I2C device found at address 0x");
      if (address < 16)
        Serial.print("0");
      Serial.print(address, HEX);
      Serial.println("  !");

      nDevices++;
    }
    else if (error == 4) {
      Serial.print("Unknown error at address 0x");
      if (address < 16)
        Serial.print("0");
      Serial.println(address, HEX);
    }
  }
  if (nDevices == 0)
    Serial.println("No I2C devices found\n");
  else
    Serial.println("done\n");
}

// void setup()
// {
//   Serial.begin(baudrate);
//   Wire.begin();

//   scan();

//   using namespace MLX90393;

//   if (MLX::begin().error()) {
//     Serial.println(F("Failed to properly initialize MLX90393"));
//     belly_up();
//   };

//   uint16_t reg[5];

//   // uncomment the block below if you only want to modify some registers
//   // leave it untouched if you want to explicitly set all of them
//   /*
//     for (uint8_t adr=0; adr < 5; ++adr) {
//         if (MLX::readRegister(adr, reg[adr]).error()) {
//             Serial.println(F("Failed to read register: "));
//             Serial.println(adr);
//             belly_up();
//         }
//     }
//     */

//   // set whatever you need here
//   // you migth want to use the debug helper to figure out what you need / want
//   MLX::set<Z_SERIES>(reg, 0);
//   MLX::set<GAIN_SEL>(reg, 0);
//   MLX::set<HALLCONF>(reg, 0);
//   MLX::set<TRIG_INT_SEL>(reg, 0);
//   MLX::set<COMM_MODE>(reg, 0);
//   MLX::set<WOC_DIFF>(reg, 0);
//   MLX::set<EXT_TRIG>(reg, 0);
//   MLX::set<TCMP_EN>(reg, 0);
//   MLX::set<BURST_SEL>(reg, 0);
//   MLX::set<BURST_DATA_RATE>(reg, 0);
//   MLX::set<OSR2>(reg, 0);
//   MLX::set<RES_X>(reg, 0);
//   MLX::set<RES_Y>(reg, 0);
//   MLX::set<RES_Z>(reg, 0);
//   MLX::set<DIG_FLT>(reg, 0);
//   MLX::set<OSR>(reg, 0);
//   MLX::set<SENS_TC_HT>(reg, 0);
//   MLX::set<SENS_TC_LT>(reg, 0);

//   for (uint8_t adr = 0; adr < 5; ++adr) {
//     if (MLX::writeRegister(adr, reg[adr]).error()) {
//       Serial.println(F("Failed to write register: "));
//       Serial.println(adr);
//       belly_up();
//     }
//   }

//   // alternatively you might want to call
//   // template <typename PARAMETER_DESCRIPTION> static MLX90393::status_t write(uint8_t value);
//   // e.g.
//   // MLX::write(OSR, 0);
//   // to modify just some parameters.
//   // However this is less efficient if you need to set all values
//   // as it requires to read the register, set the value
//   // and write it back.
// }

// void loop()
// {
//   if (MLX::startMeasurement(MLX90393::AXIS_MASK::ALL).error()) {
//     Serial.println(F("Failed to start measurement"));
//     belly_up();
//   }
//   MLX::waitDataReady();

//   MLX90393::zyxt_t result;
//   if (MLX::readMeasurement(MLX90393::AXIS_MASK::ALL, result).error()) {
//     ;
//     Serial.println(F("Failed to read measurement"));
//     belly_up();
//   }

//   Serial.print(F("X, Y, Z, T: "));
//   Serial.print(result.x);
//   Serial.print(F(", "));
//   Serial.print(result.y);
//   Serial.print(F(", "));
//   Serial.print(result.z);
//   Serial.print(F(", "));
//   Serial.println(result.t);
// }

Adafruit_MLX90393 sensor = Adafruit_MLX90393();

void setup()
{
  Wire.begin();

  Serial.begin(115200);

  while (!Serial) {
    delay(10);
  }

  Serial.println("Connected!");

  scan();

  delay(100);

  Serial.println("Starting Adafruit MLX90393 Demo");

  for (uint8_t i = 0b0001100; i <= 0b0001111; i++) {
    if (sensor.begin_I2C(i)) {
      goto _has_sensor;
    }
    Serial.print(F("Could not find sensor on address "));
    Serial.println(i, BIN);
  }

  Serial.println("No sensor found ... check your wiring?");
  while (1) {
    delay(10);
  }

_has_sensor:
  Serial.println("Found a MLX90393 sensor");

  sensor.setGain(MLX90393_GAIN_2_5X);
  // You can check the gain too
  Serial.print("Gain set to: ");
  switch (sensor.getGain()) {
  case MLX90393_GAIN_1X: Serial.println("1 x"); break;
  case MLX90393_GAIN_1_33X: Serial.println("1.33 x"); break;
  case MLX90393_GAIN_1_67X: Serial.println("1.67 x"); break;
  case MLX90393_GAIN_2X: Serial.println("2 x"); break;
  case MLX90393_GAIN_2_5X: Serial.println("2.5 x"); break;
  case MLX90393_GAIN_3X: Serial.println("3 x"); break;
  case MLX90393_GAIN_4X: Serial.println("4 x"); break;
  case MLX90393_GAIN_5X: Serial.println("5 x"); break;
  }

  // Set resolution, per axis
  sensor.setResolution(MLX90393_X, MLX90393_RES_19);
  sensor.setResolution(MLX90393_Y, MLX90393_RES_19);
  sensor.setResolution(MLX90393_Z, MLX90393_RES_16);

  // Set oversampling
  sensor.setOversampling(MLX90393_OSR_2);

  // Set digital filtering
  sensor.setFilter(MLX90393_FILTER_6);
}

struct SensorPosition
{
  float x, y, z;
};

float distance2(struct SensorPosition const & p0, struct SensorPosition const & p1)
{
  float px = p0.x - p1.x;
  float py = p0.y - p1.y;
  float pz = p0.z - p1.z;
  return px * px + py * py * pz * pz;
}

const SensorPosition closed = {-63.08, 27.04, -81.68};
const SensorPosition locked = {-72.10, 36.05, -78.05};

const float closed_threshold = 250;
const float locked_threshold = 35;

void loop()
{
  SensorPosition current;

  // get X Y and Z data at once
  if (sensor.readData(&current.x, &current.y, &current.z)) {

    float closed_dist = distance2(current, closed);
    float locked_dist = distance2(current, locked);

    Serial.print("Is Closed: ");
    Serial.print(closed_dist < closed_threshold);
    Serial.print("\tIs Locked: ");
    Serial.print(locked_dist < locked_threshold);

    Serial.print("\tX: ");
    Serial.print(current.x, 2);
    Serial.print("\tY: ");
    Serial.print(current.y, 2);
    Serial.print("\tZ: ");
    Serial.print(current.z, 2);

    Serial.print(" uTesla.\tClosed: ");
    Serial.print(closed_dist, 2);
    Serial.print("\tLocked: ");
    Serial.print(locked_dist, 2);

    Serial.println();
  }
  else {
    Serial.println("Unable to read XYZ data from the sensor.");
  }

  // delay(500);

  // /* Or....get a new sensor event, normalized to uTesla */
  // sensors_event_t event;
  // sensor.getEvent(&event);
  // /* Display the results (magnetic field is measured in uTesla) */
  // Serial.print("X: ");
  // Serial.print(event.magnetic.x);
  // Serial.print(" \tY: ");
  // Serial.print(event.magnetic.y);
  // Serial.print(" \tZ: ");
  // Serial.print(event.magnetic.z);
  // Serial.println(" uTesla ");

  delay(500);
}
