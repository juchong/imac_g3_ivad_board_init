
/**
 * @file imacG3IvadInit.ino
 * @brief iMac G3 IVAD Board Initialization Controller
 *
 * This firmware controls the iMac G3's IVAD (Integrated Video Adapter) board,
 * enabling the CRT display to be used as a standard VGA monitor. It handles:
 * - Power sequencing via solid-state relay
 * - I2C communication with IVAD board for initialization and settings
 * - EDID data simulation for connected computers
 * - Serial protocol for computer control (oshimai's protocol)
 * - VSYNC monitoring for automatic power-off
 *
 * Required Libraries:
 * - EEPROMWearLevel: https://github.com/PRosenb/EEPROMWearLevel
 * - SoftwareWire: https://github.com/Testato/SoftwareWire
 *
 * Hardware Requirements:
 * - Arduino Nano/Uno
 * - Solid-state relay on pin 7 for CRT power control
 * - Button on pin 3 for manual power toggle
 * - VSYNC input on pin 10 for signal detection
 * - Software I2C on pins 4 (SDA) and 5 (SCL) for IVAD communication
 * - Hardware I2C slave on 0x50 for EDID responses
 *
 * Supported Resolutions (from EDID):
 * - 1024x768 @ 75 Hz
 * - 800x600 @ 95 Hz
 * - 640x480 @ 117 Hz
 *
 * Required Modifications to Arduino Wire Library:
 * - BUFFER_LENGTH: 32 -> 128 in Wire.h
 * - TWI_BUFFER_LENGTH: 32 -> 128 in twi.h
 *
 * @author Rocky Hill
 * @see https://github.com/qbancoffee/imac_g3_ivad_board_init
 */

#include "imacG3IvadInit.h"
#include "ivad.h"
#include <EEPROMWearLevel.h>
#include <SoftwareWire.h>
#include <Wire.h>

// =============================================================================
// GLOBAL VARIABLES
// =============================================================================

// Starting monitor property values (debug display only - not actively used)
// These appear to be initial/default values for reference
// Actual values are stored in CURRENT_CONFIG[]
byte verticalPositionValueIndex = 0x4d; // 77
byte contrastValueIndex = 0xfe;
byte horizontalPositionValueIndex = 0xb0; // 176
byte heightValueIndex = 0xf0;             // 240
byte widthValueIndex = 0x19;              // 25
byte brightnessValueIndex = 0x0a;
byte parallelogramValueIndex = 0xc6; // 198
byte keystoneValueIndex = 0x9b;      // 155
byte rotationValueIndex = 0x42;      // 66
byte pincushionValueIndex = 0xcb;    // 203

// Serial communication buffer for protocol handling
// SERIAL_BUFFER_MAX_SIZE = 32 bytes (0x20)
byte SERIAL_BUFFER[SERIAL_BUFFER_MAX_SIZE];
byte SERIAL_BUFFER_DATA_SIZE;  // Current fill level in buffer

// Current configuration stored in SRAM (loaded from EEPROM)
// Size: CONFIG_EEPROM_SLOTS (26 bytes: 25 settings + 1 checksum)
byte CURRENT_CONFIG[CONFIG_EEPROM_SLOTS];

// First-run flag stored in EEPROM to detect initial firmware flash
// Value 0x79 indicates firmware has been configured at least once
byte FIRST_RUN = 0x79;

// Holds last received I2C data byte (populated by receiveData callback)
// Currently unused but reserved for future bidirectional protocol implementation
byte data = -1;

// =============================================================================
// PIN CONFIGURATION
// =============================================================================

// Solid-state relay pin for CRT power control
// HIGH = relay energized, CRT powered on
// LOW = relay de-energized, CRT powered off
byte solid_state_relay_Pin = 7;

// Power button pin connected to front panel button
// Pulled HIGH internally, button connects to ground when pressed
byte powerButtonPin = 3;

// VSYNC input pin for detecting active video signal
// Connected to CRT's VSYNC output
// Used for auto-power-off after 180 seconds of no signal
byte vsyncPin = 10;

// =============================================================================
// STATE VARIABLES
// =============================================================================

// Current state of CRT power circuit
// HIGH = CRT is on, LOW = CRT is off
byte externalCircuitState = LOW;

// Current reading of power button pin
// Used for debounce and state change detection
byte buttonState = LOW;

// =============================================================================
// TIMING AND COUNTERS
// =============================================================================

// Time before auto power-off when no VSYNC detected (180 seconds)
byte vsync_off_time = 180;

// Debounce counter for power button
// Counts consecutive LOW readings (button pressed)
byte buttonPressedTime = 0;

// VSYNC pulse counter for auto-shutdown
// Increments on each VSYNC pulse, decrements every second
byte vsyncDetect = 0;

// Timing variables for elapsed time calculation
unsigned long currentTime = 0;   // Current millis() reading
unsigned long startTime = 0;      // Last VSYNC or second marker
unsigned long elapsedTime = 0;   // Time since last event

// =============================================================================
// SOFTWARE I2C INITIALIZATION
// =============================================================================

// SoftwareWire instance for IVAD communication
// Uses pins 4 (SDA) and 5 (SCL) for I2C bus to IVAD board
// Configured as master to send init sequences and settings
SoftwareWire softWire(4, 5);

/**
 * @brief Initialize all subsystems
 *
 * Performs one-time initialization of:
 * - GPIO pins (direction configuration)
 * - EEPROMWearLevel library for persistent storage
 * - I2C interfaces (hardware as slave, software as master)
 * - Serial communication
 * - I2C event handlers
 * - Default settings on first firmware flash
 *
 * @note CRT remains OFF after setup() - call externalCircuitOn() to power on
 */
void setup()
{

  // define pin direction
  pinMode(solid_state_relay_Pin, OUTPUT);
  pinMode(powerButtonPin, INPUT);
  pinMode(vsyncPin, INPUT); // this pin is used to monitor VSYNC.
  pinMode(8, INPUT);        // this pin is on the J5 connector for general use PB0.
  pinMode(9, INPUT);        // this pin is on the J5 connector for general use PB1.

  EEPROMwl.begin(CONFIG_EEPROM_VERSION, CONFIG_EEPROM_SLOTS + 1);
  Wire.begin(0x50);     // join as slave and wait for EDID requests
  softWire.begin();     // join as master and send init sequence
  Serial.begin(115200); // use built in serial
  Serial.setTimeout(1000);

  Wire.onRequest(requestEvent); // event handler for requests from master
  Wire.onReceive(receiveData);  // event handler when receiving from  master
  // turn it all off
  externalCircuitOff();

  // check to see if it's the 1st time running after burning firmware
  FIRST_RUN = EEPROMwl.read(CONFIG_EEPROM_SLOTS);
  if (FIRST_RUN != 0x79)
  {
    EEPROMwl.update(CONFIG_EEPROM_SLOTS, 0x79);
    settings_reset_default();
    settings_store();
    settings_load();
    ivad_write_settings();
  } // end if

  // externalCircuitOn();

} // end setup

/**
 * @brief Main event loop
 *
 * Continuously executes the following operations:
 * 1. Reads power button state
 * 2. When CRT is on: processes serial commands and monitors VSYNC
 * 3. Monitors VSYNC to detect active video signal
 * 4. Auto-power-off after 180 seconds without VSYNC
 * 5. Detects button presses for power toggle
 *
 * @note VSYNC monitoring prevents CRT from staying on with no signal
 * @note Button debouncing prevents accidental triggers
 */
void loop()
{

  buttonState = digitalRead(powerButtonPin);

  // do stuff only when the CRT is on
  if (externalCircuitState == HIGH)
  {

    currentTime = millis();
    elapsedTime = currentTime - startTime;

    serial_processing();

    // increment vsyncDetect everytime vsync is detected
    if (pulseIn(vsyncPin, HIGH, 10000) > 0)
    {

      if (vsyncDetect < vsync_off_time)
      {
        vsyncDetect++;
      } // end if
      startTime = currentTime = millis();
    } // end if

    // decrement vsyncDetect whenever one second elapses
    if (elapsedTime >= 1000 && vsyncDetect > 0)
    {
      vsyncDetect--;
      startTime = currentTime = millis();
    }

    // do stuff whn vsyncDetect is 0
    if (vsyncDetect <= 0)
    {
      startTime = 0;
      currentTime = 0;
      externalCircuitOff();
    }

  } // end if

  if (buttonState == LOW)
  {
    if (buttonPressedTime <= 10)
    {
      buttonPressedTime++;
    } // end if
  }
  else
  {
    buttonPressedTime = 0;
  }

  // turn everything off if button is pressed for 10 ms
  if (buttonPressedTime > 0 && externalCircuitState == HIGH && buttonState == HIGH)
  {
    externalCircuitOff();
    buttonPressedTime = 0;
  }

  // turn everything on if button is pressed for 10 ms
  if (buttonPressedTime > 0 && externalCircuitState == LOW && buttonState == HIGH)
  {
    externalCircuitOn();
    buttonPressedTime = 0;
    startTime = millis();
    currentTime = millis();
    vsyncDetect = vsync_off_time;
  }

} // end loop

void handleSerial(char incoming)
{
  /**
   * @brief Process single-character serial commands for manual adjustment
   *
   * Maps keyboard characters to display geometry adjustments.
   * Called by serial_processing() for non-protocol characters.
   *
   * Key mappings:
   * | Key | Action           | Setting Index         |
   * |-----|------------------|-----------------------|
   * | a/s | Move left/right  | HORIZONTAL_POS        |
   * | w/z | Move up/down     | VERTICAL_POS          |
   * | d/f | Narrower/wider   | WIDTH                 |
   * | r/c | Taller/shorter   | HEIGHT                |
   * | g/h | Contrast -/+     | CONTRAST              |
   * | j/k | Brightness -/+   | BRIGHTNESS            |
   * | x/v | Parallelogram    | PARALLELOGRAM         |
   * | b/n | Keystone         | KEYSTONE              |
   * | t/y | Rotate           | ROTATION              |
   * | u/i | Pincushion       | PINCUSHION            |
   * | p   | Print settings   | Debug output          |
   * | o   | Power off        | externalCircuitOff()  |
   *
   * @param incoming Character received from serial console
   * @see handleSerial() for protocol-based adjustments
   */

  // if (Serial.available() > 0) {
  //  char incoming = Serial.read();

  int index = -1;
  bool increment = true;
  switch (incoming)
  {
    case 'a': // move left
      // moveHorizontal(+1);
      index = IVAD_SETTING_HORIZONTAL_POS;
      break;
    case 's': // move right
      // moveHorizontal(-1);
      index = IVAD_SETTING_HORIZONTAL_POS;
      increment = false;
      break;
    case 'w': // move up
      // moveVertical(-1);
      index = IVAD_SETTING_VERTICAL_POS;
      increment = false;
      break;
    case 'z': // move down
      // moveVertical(+1);
      index = IVAD_SETTING_VERTICAL_POS;
      break;
    case 'd': // make skinnier
      // changeWidth(+1);
      index = IVAD_SETTING_WIDTH;
      break;
    case 'f': // make fatter
      // changeWidth(-1);
      index = IVAD_SETTING_WIDTH;
      increment = false;
      break;
    case 'r': // make taller
      // changeHeight(+1);
      index = IVAD_SETTING_HEIGHT;
      break;
    case 'c': // make shorter
      // changeHeight(-1);
      index = IVAD_SETTING_HEIGHT;
      increment = false;
      break;
    case 'g': // decrease contrast
      // changeContrast(-1);
      index = IVAD_SETTING_CONTRAST;
      increment = false;
      break;
    case 'h': // increase contrast
      // changeContrast(+1);
      index = IVAD_SETTING_CONTRAST;
      break;
    case 'j': // decrease brightness
      // changeBrightness(-1);
      index = IVAD_SETTING_BRIGHTNESS;
      increment = false;
      break;
    case 'k': // increase brightness
      // changeBrightness(+1);
      index = IVAD_SETTING_BRIGHTNESS;
      break;
    case 'x': // tilt paralellogram left
      // changeParallelogram(+1);
      index = IVAD_SETTING_PARALLELOGRAM;
      break;
    case 'v': // tilt paralellogram right
      // changeParallelogram(-1);
      index = IVAD_SETTING_PARALLELOGRAM;
      increment = false;
      break;
    case 'b': // keystone pinch top
      // changeKeystone(-1);
      index = IVAD_SETTING_KEYSTONE;
      increment = false;
      break;
    case 'n': // keystone pinch bottom
      // changeKeystone(+1);
      index = IVAD_SETTING_KEYSTONE;
      break;
    case 't': // rotate left
      // changeRotation(+1);
      index = IVAD_SETTING_ROTATION;
      break;
    case 'y': // rotate right
      // changeRotation(-1);
      index = IVAD_SETTING_ROTATION;
      increment = false;
      break;
    case 'u': // pincushion pull corners out
      // changePincushion(-1);
      index = IVAD_SETTING_PINCUSHION;
      increment = false;
      break;
    case 'i': // pincushion pull corners in
      // changePincushion(+1);
      index = IVAD_SETTING_PINCUSHION;
      break;
    case 'p':
      printCurrentSettings();
      break;
    case 'o': // power off
      if (externalCircuitState == HIGH)
      {
        externalCircuitOff();
      } // end if
      break;
  }

  if (index > -1)
  {
    int val = CURRENT_CONFIG[index];
    if (increment)
    {
      val++;
    }
    else
    {
      val--;
    }

    ivad_change_setting(index, val);
  } // end if

} // end handleSerial

/**
 * @brief Print current settings to serial console (debug)
 *
 * Displays all initial-value variables in hexadecimal format.
 * Note: These are global variables, not CURRENT_CONFIG[] values.
 * This function may print stale/outdated data.
 *
 * @warning These variables are NOT actively used elsewhere
 * @see CURRENT_CONFIG[] for actual current settings
 */
void printCurrentSettings()
{
  Serial.println(F("----------------------------"));

  Serial.print(F("heightValueIndex: "));
  Serial.println(heightValueIndex, HEX);

  Serial.print(F("widthValueIndex: "));
  Serial.println(widthValueIndex, HEX);

  Serial.println("");

  Serial.print(F("verticalPositionValueIndex: "));
  Serial.println(verticalPositionValueIndex, HEX);

  Serial.print(F("horizontalPositionValueIndex: "));
  Serial.println(horizontalPositionValueIndex, HEX);

  Serial.println(F(""));

  Serial.print(F("rotationValueIndex: "));
  Serial.println(rotationValueIndex, HEX);

  Serial.print(F("parallelogramValueIndex: "));
  Serial.println(parallelogramValueIndex, HEX);

  Serial.print(F("keystoneValueIndex: "));
  Serial.println(keystoneValueIndex, HEX);

  Serial.print(F("pincushionValueIndex: "));
  Serial.println(pincushionValueIndex, HEX);

  Serial.println("");

  Serial.print(F("contrastValueIndex: "));
  Serial.println(contrastValueIndex, HEX);

  Serial.print(F("brightnessValueIndex: "));
  Serial.println(brightnessValueIndex, HEX);

  Serial.println(F("----------------------------"));
}

/**
 * @brief Write single byte to IVAD via I2C
 * @param address I2C slave address (0x46 for properties, 0x53 for unlock)
 * @param message Single byte to transmit
 */
void writeToIvad(byte address, byte message)
{
  softWire.beginTransmission(address);
  softWire.write(message);
  softWire.endTransmission();

} // end method

/**
 * @brief Write register/value pair to IVAD via I2C
 * @param address  I2C slave address (0x46 typically)
 * @param message1 Register/setting index (0x00-0x12)
 * @param message2 Value to write to register
 */
void writeToIvad(byte address, byte message1, byte message2)
{
  softWire.beginTransmission(address);
  softWire.write(message1);
  softWire.write(message2);
  softWire.endTransmission();

} // end method

/**
 * @brief Read data from IVAD via I2C
 *
 * Reads bytes from IVAD board and stores in local buffer.
 * Currently data is not used (reserved for future protocol features).
 *
 * @param address I2C slave address to read from
 * @param bytes   Number of bytes to request
 * @note Read data discarded after retrieval
 */
void readFromIvad(byte address, byte bytes)
{
  char buf[bytes + 1];
  byte bytesRead = 0;
  softWire.requestFrom(address, bytes);
  while (softWire.available())
  {
    char c = softWire.read();
    buf[bytesRead++] = c;
  }
  buf[bytesRead] = '\0';

} // end method

/**
 * @brief Initialize IVAD board with startup sequence
 *
 * This function contains the critical I2C initialization sequence that
 * configures the IVAD board to accept video input. It must be called
 * before the CRT can display any image.
 *
 * Sequence breakdown:
 * 1. Reset registers 0x46 (property) and 0x53 (unknown/unlock)
 * 2. Read calibration data from 0x53 address space (10 reads)
 * 3. Write 18 configuration registers (0x01-0x12) with calibrated values
 * 4. Lock configuration by setting register 0x00 to 0xFF
 * 5. Re-read calibration data for verification
 * 6. Final geometry and color adjustments
 *
 * @note This sequence was captured from original iMac G3 logic board
 * @warning Must be called BEFORE ivad_write_settings()
 * @see externalCircuitOn() which calls this function
 */
void initIvadBoard()
{

  //  //init sequence 2 <---this is the one that works well with my iMac G3, Rocky Hill
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_WIDTH, 0x00);
  //  readFromIvad(IVAD_REGISTER_PROPERTY, 1);
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_VERTICAL_POS, 0x00);
  //  writeToIvad( 0x53, 0x33);
  //  readFromIvad(0x53, 1);
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_WIDTH, 0x0B);
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_CONTRAST, 0x00); //setting contrast to 0x00 seems to turn something on.
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_HEIGHT, 0xE4);
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_ROTATION, 0xC9);
  //  writeToIvad( 0x53, 0x00);
  //  readFromIvad(0x53, 10);
  //  writeToIvad( 0x53, 0x0A);
  //  readFromIvad(0x53, 10);
  //  writeToIvad( 0x53, 0x14);
  //  readFromIvad(0x53, 10);
  //  writeToIvad( 0x53, 0x1E);
  //  readFromIvad(0x53, 10);
  //  writeToIvad( 0x53, 0x28);
  //  readFromIvad(0x53, 10);
  //  writeToIvad( 0x53, 0x32);
  //  readFromIvad(0x53, 10);
  //  writeToIvad( 0x53, 0x3C);
  //  readFromIvad(0x53, 10);
  //  writeToIvad( 0x53, 0x46);
  //  readFromIvad(0x53, 10);
  //  writeToIvad( 0x53, 0x50);
  //  readFromIvad(0x53, 10);
  //  writeToIvad( 0x53, 0x5A);
  //  readFromIvad(0x53, 2);
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_RED_CUTOFF,         VIDEO_CONFIG_DEFAULT[ IVAD_SETTING_RED_CUTOFF ]        );
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_GREEN_CUTOFF,       VIDEO_CONFIG_DEFAULT[ IVAD_SETTING_GREEN_CUTOFF ]      );
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_BLUE_CUTOFF,        VIDEO_CONFIG_DEFAULT[ IVAD_SETTING_BLUE_CUTOFF ]       );
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_HORIZONTAL_POS,     VIDEO_CONFIG_DEFAULT[ IVAD_SETTING_HORIZONTAL_POS ]    );
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_HEIGHT,             VIDEO_CONFIG_DEFAULT[ IVAD_SETTING_HEIGHT ]            );
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_VERTICAL_POS,       VIDEO_CONFIG_DEFAULT[ IVAD_SETTING_VERTICAL_POS ]      );
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_S_CORRECTION,       VIDEO_CONFIG_DEFAULT[ IVAD_SETTING_S_CORRECTION ]      );
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_KEYSTONE,           VIDEO_CONFIG_DEFAULT[ IVAD_SETTING_KEYSTONE ]          );
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_PINCUSHION,         VIDEO_CONFIG_DEFAULT[ IVAD_SETTING_PINCUSHION ]        );
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_WIDTH,              VIDEO_CONFIG_DEFAULT[ IVAD_SETTING_WIDTH ]             );
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_PINCUSHION_BALANCE, VIDEO_CONFIG_DEFAULT[ IVAD_SETTING_PINCUSHION_BALANCE ]);
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_PARALLELOGRAM,      VIDEO_CONFIG_DEFAULT[ IVAD_SETTING_PARALLELOGRAM ]     );
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_RESERVED6,          VIDEO_CONFIG_DEFAULT[ IVAD_SETTING_RESERVED6 ]         ); // brightness
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_BRIGHTNESS,         VIDEO_CONFIG_DEFAULT[ IVAD_SETTING_BRIGHTNESS ]        );
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_ROTATION,           VIDEO_CONFIG_DEFAULT[ IVAD_SETTING_ROTATION ]          );
  //  writeToIvad( IVAD_REGISTER_PROPERTY, IVAD_SETTING_CONTRAST,           VIDEO_CONFIG_DEFAULT[ IVAD_SETTING_CONTRAST ]          );

  // provided by anothere
  writeToIvad(0x46, 0x13, 0x00);
  writeToIvad(0x46, 0x13, 0x00);
  readFromIvad(0x46, 1);
  writeToIvad(0x46, 0x09, 0x00);
  writeToIvad(0x53, 0x33);
  readFromIvad(0x53, 1);
  writeToIvad(0x46, 0x13, 0x0b);
  writeToIvad(0x46, 0x00, 0x00);
  writeToIvad(0x46, 0x08, 0xe4);
  writeToIvad(0x46, 0x12, 0xc9);
  writeToIvad(0x53, 0x00);
  readFromIvad(0x53, 10);
  writeToIvad(0x53, 0x0a);
  readFromIvad(0x53, 10);
  writeToIvad(0x53, 0x14);
  readFromIvad(0x53, 10);
  writeToIvad(0x53, 0x1e);
  readFromIvad(0x53, 10);
  writeToIvad(0x53, 0x28);
  readFromIvad(0x53, 10);
  writeToIvad(0x53, 0x32);
  readFromIvad(0x53, 10);
  writeToIvad(0x53, 0x3c);
  readFromIvad(0x53, 10);
  writeToIvad(0x53, 0x46);
  readFromIvad(0x53, 10);
  writeToIvad(0x53, 0x50);
  readFromIvad(0x53, 10);
  writeToIvad(0x53, 0x5a);
  readFromIvad(0x53, 10);
  writeToIvad(0x46, 0x01, 0x82);
  writeToIvad(0x46, 0x02, 0x82);
  writeToIvad(0x46, 0x03, 0x82);
  writeToIvad(0x46, 0x04, 0xa0);
  writeToIvad(0x46, 0x05, 0xa0);
  writeToIvad(0x46, 0x06, 0xa0);
  writeToIvad(0x46, 0x07, 0xad);
  writeToIvad(0x46, 0x08, 0xe4);
  writeToIvad(0x46, 0x09, 0x3d);
  writeToIvad(0x46, 0x0a, 0x9e);
  writeToIvad(0x46, 0x0b, 0xb4);
  writeToIvad(0x46, 0x0c, 0xc4);
  writeToIvad(0x46, 0x0d, 0x27);
  writeToIvad(0x46, 0x0e, 0xbf);
  writeToIvad(0x46, 0x0f, 0xc0);
  writeToIvad(0x46, 0x10, 0x40);
  writeToIvad(0x46, 0x11, 0x0a);
  writeToIvad(0x46, 0x12, 0x5b);
  writeToIvad(0x46, 0x00, 0xff);
  writeToIvad(0x53, 0x00);
  readFromIvad(0x53, 10);
  writeToIvad(0x53, 0x10);
  readFromIvad(0x53, 10);
  writeToIvad(0x53, 0x20);
  readFromIvad(0x53, 10);
  writeToIvad(0x53, 0x30);
  readFromIvad(0x53, 10);
  writeToIvad(0x46, 0x11, 0x05);
  writeToIvad(0x46, 0x00, 0xff);
  writeToIvad(0x46, 0x00, 0x00);
  writeToIvad(0x46, 0x07, 0xb1);
  writeToIvad(0x46, 0x0d, 0x10);
  writeToIvad(0x46, 0x0c, 0xc7);
  writeToIvad(0x46, 0x09, 0x4a);
  writeToIvad(0x46, 0x08, 0xea);
  writeToIvad(0x46, 0x0f, 0xc0);
  writeToIvad(0x46, 0x0b, 0xae);
  writeToIvad(0x46, 0x12, 0x5b);
  writeToIvad(0x46, 0x00, 0xff);
  writeToIvad(0x46, 0x11, 0x05);
  writeToIvad(0x46, 0x00, 0xff);
  writeToIvad(0x46, 0x10, 0x40);
  writeToIvad(0x46, 0x06, 0xa0);
  writeToIvad(0x46, 0x05, 0xa0);
  writeToIvad(0x46, 0x04, 0xa0);
  writeToIvad(0x46, 0x03, 0x82);
  writeToIvad(0x46, 0x02, 0x82);
  writeToIvad(0x46, 0x01, 0x82);
  writeToIvad(0x46, 0x11, 0x05);
  writeToIvad(0x46, 0x00, 0xff);
  writeToIvad(0x46, 0x11, 0x05);
  writeToIvad(0x46, 0x00, 0xff);
  writeToIvad(0x46, 0x10, 0x40);
  writeToIvad(0x46, 0x06, 0xa0);
  writeToIvad(0x46, 0x05, 0xa0);
  writeToIvad(0x46, 0x04, 0xa0);
  writeToIvad(0x46, 0x03, 0x82);
  writeToIvad(0x46, 0x02, 0x82);
  writeToIvad(0x46, 0x01, 0x82);
  writeToIvad(0x46, 0x11, 0x05);
  writeToIvad(0x46, 0x00, 0xff);
}

/**
 * @brief Activate solid-state relay to power on CRT
 *
 * Sets relay control pin HIGH, which energizes the relay coil
 * and connects AC power to the CRT subsystem.
 */
void solid_state_relayOn()
{
  digitalWrite(solid_state_relay_Pin, HIGH);
}

/**
 * @brief Deactivate solid-state relay to power off CRT
 *
 * Sets relay control pin LOW, which de-energizes the relay coil
 * and disconnects AC power from the CRT subsystem.
 */
void solid_state_relayOff()
{
  digitalWrite(solid_state_relay_Pin, LOW);
}

/**
 * @brief Turn CRT power on with full initialization
 *
 * Complete power-on sequence:
 * 1. Energize solid-state relay (applies power to CRT)
 * 2. Wait 500ms for power stabilization
 * 3. Initialize IVAD board with startup sequence
 * 4. Load saved settings from EEPROM
 * 5. Apply all settings to IVAD board
 *
 * @see solid_state_relayOn()
 * @see initIvadBoard()
 * @see settings_load()
 * @see ivad_write_settings()
 */
// these are probably too much but they are here in case I would lke to add more stuff to turn on and off
void externalCircuitOn()
{
  solid_state_relayOn();
  delay(500);
  initIvadBoard();
  settings_load();
  ivad_write_settings();
  externalCircuitState = HIGH;
}

/**
 * @brief Turn CRT power off
 *
 * Simple power-off sequence:
 * 1. De-energize solid-state relay (removes power from CRT)
 * 2. Update state tracking variable
 *
 * @note IVAD configuration is NOT cleared - next power-on will reuse it
 * @see solid_state_relayOff()
 */
void externalCircuitOff()
{
  solid_state_relayOff();
  externalCircuitState = LOW;
}

/**
 * @brief I2C request handler - responds with EDID data
 *
 * This callback is registered with Wire.onRequest() and executes
 * whenever the Arduino (as I2C slave at address 0x50) receives
 * a read request from the connected computer.
 *
 * EDID (Extended Display Identification Data) is a 128-byte data
 * structure that reports monitor capabilities to the connected
 * computer. This EDID is from an iMac G3 DV and includes:
 * - Manufacturer: Apple Computer, Inc.
 * - Model: iMac G3
 * - Supported resolutions: 1024x768@75Hz, 800x600@95Hz, 640x480@117Hz
 *
 * @note Requires Wire library buffer size increase (32->128 bytes)
 * @see imacG3IvadInit.h for EDID data
 */
void requestEvent()
{
  // delay(500);
  Wire.write(edid, 128);

} // end method
/**
 * @brief I2C receive handler - called when master sends data
 *
 * This callback is registered with Wire.onReceive() and executes
 * whenever the Arduino receives I2C data from the master.
 *
 * Currently implemented to discard all incoming data to prevent
 * buffer overflow. Reserved for future bidirectional protocol
 * implementation.
 *
 * @param byteCount Number of bytes received from master (ignored)
 * @see requestEvent() for outbound data handler
 */
void receiveData(int byteCount)
{

  while (Wire.available())
  {
    data = Wire.read();
  }
}

/**
 * @brief Parse and execute serial commands
 *
 * Implements oshimai's communications protocol for computer-to-Arduino control.
 * Handles binary packet format:
 * [0x06][ID][CMD][valA][valB][0x03][CHK][0x04][0x0A]
 *
 * Supported commands:
 * - 0x01: Get EEPROM version
 * - 0x02: Dump SRAM configuration (25 bytes + checksum)
 * - 0x03: Change IVAD setting (valA=setting, valB=value)
 * - 0x04: Reload settings from EEPROM
 * - 0x05: Reset all settings to factory defaults
 * - 0x06: Save current SRAM config to EEPROM
 *
 * @note Packet checksum: 256 - (sum % 256), initialized with 1 (never zero)
 * @note Unknown commands return error response (not silently ignored)
 */
void serial_processing()
{

  byte b;

  if (Serial.available())
  {

    do
    {
      b = Serial.read();
      if (SERIAL_BUFFER_DATA_SIZE < SERIAL_BUFFER_MAX_SIZE - 1)
      {
        SERIAL_BUFFER[SERIAL_BUFFER_DATA_SIZE++] = b;
      }
    } while (Serial.available() && b != SERIAL_EOL_MARKER);

    // call other serial handler
    if (SERIAL_BUFFER_DATA_SIZE != 9)
    {
      if (SERIAL_BUFFER[0] != 0x07)
      {
        SERIAL_BUFFER_DATA_SIZE = 0;
        handleSerial((char)b);
      }
      return;
    }

    SERIAL_BUFFER_DATA_SIZE = 0;

    byte id = SERIAL_BUFFER[1];
    byte cmd = SERIAL_BUFFER[2];
    byte valA = SERIAL_BUFFER[3];
    byte valB = SERIAL_BUFFER[4];

    switch (cmd)
    {

      case 0x01: // Get EEPROM Version
      {
        byte ret[8]{0x06, id, 0x01, CONFIG_EEPROM_VERSION, 0x03, 0xFF, 0x04, SERIAL_EOL_MARKER};
        ret[5] = checksum(ret, 5);
        Serial.write(ret, 8);
      }
      break;

      case 0x02: // Dump SRAM Config
      {
        byte ret[7 + CONFIG_EEPROM_SLOTS];
        ret[0] = 0x06;
        ret[1] = SERIAL_BUFFER[1];
        ret[2] = CONFIG_EEPROM_SLOTS;

        for (int i = 0; i < CONFIG_EEPROM_SLOTS; i++)
          ret[3 + i] = CURRENT_CONFIG[i];

        ret[2 + CONFIG_EEPROM_SLOTS + 1] = 0x03;
        ret[2 + CONFIG_EEPROM_SLOTS + 2] = checksum(ret, 2 + CONFIG_EEPROM_SLOTS + 1 + 1);
        ret[2 + CONFIG_EEPROM_SLOTS + 3] = 0x04;
        ret[2 + CONFIG_EEPROM_SLOTS + 4] = SERIAL_EOL_MARKER;
        Serial.write(ret, 7 + CONFIG_EEPROM_SLOTS);
      }
      break;

      case 0x03: // IVAD Change Setting
      {
        ivad_change_setting(valA, valB);
        byte ret[7]{0x06, id, 0x00, 0x03, 0xFF, 0x04, SERIAL_EOL_MARKER};
        ret[4] = checksum(ret, 4);
        Serial.write(ret, 7);
      }
      break;

      case 0x04: // IVAD Reset from EEPROM
      {
        settings_load();
        byte ret[7]{0x06, id, 0x00, 0x03, 0xFF, 0x04, SERIAL_EOL_MARKER};
        ret[4] = checksum(ret, 4);
        Serial.write(ret, 7);
      }
      break;

      case 0x05: // EEPROM Reset to Default
      {
        settings_reset_default();
        settings_store();
        settings_load();
        ivad_write_settings();
        byte ret[7]{0x06, id, 0x00, 0x03, 0xFF, 0x04, SERIAL_EOL_MARKER};
        ret[4] = checksum(ret, 4);
        Serial.write(ret, 7);
      }
      break;

      case 0x06: // Write SRAM to EEPROM
      {
        settings_store();
        byte ret[7]{0x06, id, 0x00, 0x03, 0xFF, 0x04, SERIAL_EOL_MARKER};
        ret[4] = checksum(ret, 4);
        Serial.write(ret, 7);
      }
      break;

      default: // Unknown command - no action
      {
        byte ret[7]{0x06, id, 0x00, 0x03, 0xFF, 0x04, SERIAL_EOL_MARKER};
        ret[4] = checksum(ret, 4);
        Serial.write(ret, 7);
      }
      break;
    } // end switch

    SERIAL_BUFFER[1] = 0xFF;
  }

} // end if

/**
 * @brief Calculate ones-complement checksum for protocol packets
 *
 * Computes: checksum = 256 - (sum(arr[i]) % 256)
 * where sum is initialized to 1 (checksum may never be zero).
 *
 * Used for:
 * - Validating serial protocol packets
 * - Computing configuration checksum for EEPROM storage
 *
 * @param arr   Data array to checksum
 * @param len   Number of bytes to include in checksum
 * @return      Single byte checksum (0x01-0xFF, never 0x00)
 */
byte checksum(const byte arr[], const int len)
{
  int sum = 1; // Checksum may never be 0.

  for (int i = 0; i < len; i++)
    sum += arr[i];

  byte ret = 256 - (sum % 256);

  return ret;
}

/**
 * @brief Change IVAD setting with bounds validation
 *
 * Updates a single setting in CURRENT_CONFIG[], validates against
 * MIN/MAX bounds, writes to IVAD board via I2C, and updates checksum.
 *
 * @param ivad_setting Setting index (IVAD_SETTING_XXX enum value)
 * @param value        New value to set (clamped to valid range)
 * @return 0 always (no error conditions)
 *
 * Bounds checking:
 * - Values below VIDEO_CONFIG_MIN[] are clamped to minimum
 * - Values above VIDEO_CONFIG_MAX[] are clamped to maximum
 *
 * @note Automatically updates CONFIG_OFFSET_CHECKSUM (last byte)
 * @see VIDEO_CONFIG_MIN in ivad.h for minimum valid values
 * @see VIDEO_CONFIG_MAX in ivad.h for maximum valid values
 */
int ivad_change_setting(const int ivad_setting, const byte value)
{

  CURRENT_CONFIG[ivad_setting] = value;

  if (CURRENT_CONFIG[ivad_setting] < VIDEO_CONFIG_MIN[ivad_setting])
    CURRENT_CONFIG[ivad_setting] = VIDEO_CONFIG_MIN[ivad_setting];
  if (CURRENT_CONFIG[ivad_setting] > VIDEO_CONFIG_MAX[ivad_setting])
    CURRENT_CONFIG[ivad_setting] = VIDEO_CONFIG_MAX[ivad_setting];

  writeToIvad(IVAD_REGISTER_PROPERTY, ivad_setting, CURRENT_CONFIG[ivad_setting]);
  CURRENT_CONFIG[CONFIG_OFFSET_CHECKSUM] = checksum(CURRENT_CONFIG, CONFIG_EEPROM_SLOTS - 1);

  return 0;
}

/**
 * @brief Load settings from EEPROM into SRAM
 *
 * Reads all 26 configuration bytes (25 settings + checksum) from
 * EEPROMWearLevel storage and validates checksum.
 *
 * On checksum mismatch:
 * - Resets all settings to factory defaults
 * - Stores validated defaults to EEPROM
 * - Loads them into SRAM
 * - Applies to IVAD board via ivad_write_settings()
 *
 * @note First-run detection: if EEPROM reads 0xFF (empty), initializes defaults
 * @see settings_reset_default()
 * @see settings_store()
 * @see ivad_write_settings()
 */
void settings_load()
{
  // Set something so a checksum mismatch can trigger if there's nothing in the EEPROM.

  for (byte eeprom_memory_offset = 0; eeprom_memory_offset < CONFIG_EEPROM_SLOTS; eeprom_memory_offset++)
  {
    CURRENT_CONFIG[eeprom_memory_offset] = EEPROMwl.read(eeprom_memory_offset);
  } // end for

  byte loaded_checksum = CURRENT_CONFIG[CONFIG_OFFSET_CHECKSUM];
  byte expected_checksum = checksum(CURRENT_CONFIG, CONFIG_EEPROM_SLOTS - 1);

  if (loaded_checksum != expected_checksum)
  {
    settings_reset_default();
    settings_store();
  }
}

/**
 * @brief Apply all settings to IVAD board
 *
 * Writes all 21 IVAD settings from SRAM configuration to the
 * IVAD board via I2C. This synchronizes the hardware with current
 * settings after loading from EEPROM or changing values.
 *
 * @see ivad_change_setting() for single-setting changes
 */
void ivad_write_settings()
{

  for (int IVAD_SETTING = 0; IVAD_SETTING < IVAD_SETTING_END; IVAD_SETTING++)
  {
    writeToIvad(IVAD_REGISTER_PROPERTY, IVAD_SETTING, CURRENT_CONFIG[IVAD_SETTING]);
  }
}

/**
 * @brief Store current configuration to EEPROM
 *
 * Computes checksum of current configuration (excluding checksum byte),
 * stores it, then writes all 26 bytes to EEPROMWearLevel storage.
 * This persists settings across power cycles.
 *
 * @note EEPROMWearLevel provides wear-leveling to extend flash lifetime
 * @see settings_load()
 */
void settings_store()
{

  // compute current config checksum and store it.
  byte current_config_checksum = checksum(CURRENT_CONFIG, CONFIG_EEPROM_SLOTS - 1);

  CURRENT_CONFIG[CONFIG_OFFSET_CHECKSUM] = current_config_checksum;

  for (byte eeprom_memory_offset = 0; eeprom_memory_offset < CONFIG_EEPROM_SLOTS; eeprom_memory_offset++)
  {
    EEPROMwl.update(eeprom_memory_offset, CURRENT_CONFIG[eeprom_memory_offset]);
  } // end for
}

/**
 * @brief Reset all settings to factory defaults
 *
 * Copies VIDEO_CONFIG_DEFAULT values (21 bytes) into CURRENT_CONFIG[]
 * and computes checksum for the new configuration.
 *
 * @note Does NOT automatically store to EEPROM - call settings_store() after
 * @see VIDEO_CONFIG_DEFAULT in ivad.h for default values
 * @see settings_store()
 */
void settings_reset_default()
{

  // compute current config checksum and store it.
  byte current_config_checksum = checksum(CURRENT_CONFIG, CONFIG_EEPROM_SLOTS - 1);

  CURRENT_CONFIG[CONFIG_OFFSET_CHECKSUM] = current_config_checksum;

  for (byte eeprom_memory_offset = 0; eeprom_memory_offset < sizeof(VIDEO_CONFIG_DEFAULT); eeprom_memory_offset++)
  {
    CURRENT_CONFIG[eeprom_memory_offset] = VIDEO_CONFIG_DEFAULT[eeprom_memory_offset];
  } // end for
}
