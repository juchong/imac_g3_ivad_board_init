/*
   iMac G3 IVAD Board Init — Arduino Uno/Nano firmware
   Original code by Rocky Hill (qbancoffee)

   Controls the iMac G3 slot-loading CRT via its IVAD board over I2C,
   allowing the CRT to be used as a standalone VGA monitor.

   Features:
   - Initializes the IVAD board via SoftwareWire (I2C master on pins 4/5)
   - Responds to EDID requests as an I2C slave (Wire library on A4/A5,
     address 0x50) so the video source can identify the display
   - Persists display geometry/color settings in EEPROM with wear leveling
   - Serial console (115200 baud) for adjusting all display settings,
     power control, and diagnostics (send '?' for command list)
   - Auto power-on when VSYNC is detected (optional, saved to EEPROM)
   - Auto power-off after 180 seconds of VSYNC signal loss
   - Physical power button on pin 3

   Supported display modes (from EDID):
     1024x768 @ 75 Hz
     800x600 @ 95 Hz
     640x480 @ 117 Hz

   IMPORTANT: When using an HDMI-to-VGA adapter, the adapter typically
   has its own EDID and does not pass through the Arduino's EDID. You
   must manually force a compatible resolution (1024x768 @ 75 Hz) on
   the host. Native VGA sources read the Arduino's EDID directly.

   Wire library modification required:
     Change BUFFER_LENGTH to 128 in:
       arduino_install_folder/hardware/arduino/avr/libraries/Wire/src/Wire.h
     Change TWI_BUFFER_LENGTH to 128 in:
       arduino_install_folder/hardware/arduino/avr/libraries/Wire/src/utility/twi.h

   Pin assignments:
     D3       - Power button (active LOW)
     D4       - IVAD SDA (SoftwareWire)
     D5       - IVAD SCL (SoftwareWire)
     D7       - Solid-state relay control
     D10      - VSYNC input from VGA
     A4 (SDA) - VGA DDC data  (pin 12 on VGA connector)
     A5 (SCL) - VGA DDC clock (pin 15 on VGA connector)
     PD0/PD1  - Hardware serial (TX/RX) at 115200 baud

   Libraries:
     SoftwareWire - https://github.com/Testato/SoftwareWire
     EEPROMWearLevel - https://github.com/PRosenb/EEPROMWearLevel
*/

#include "ivad.h"
#include "imacG3IvadInit.h"
#include <EEPROMWearLevel.h>
#include <SoftwareWire.h>
#include <Wire.h>


byte SERIAL_BUFFER[SERIAL_BUFFER_MAX_SIZE];
byte SERIAL_BUFFER_DATA_SIZE;
byte CURRENT_CONFIG[CONFIG_EEPROM_SLOTS];
byte FIRST_RUN = 0x79;

byte data = -1;

//define solid state relay and power button pins
byte solid_state_relay_Pin = 7;

byte powerButtonPin = 3;
//int powerButtonPin = 13;

//define state variables
byte externalCircuitState = LOW;
byte buttonState = LOW;
byte prevVsyncActive = 0;
byte vsyncWasDecrementing = 0;
byte autoVsyncPowerOn = 0;
byte userRequestedOff = 0;

//vsync pin
byte vsyncPin = 10;


//vsync power off countdown in seconds
byte vsync_off_time = 180;

//counters
byte buttonPressedTime = 0;
byte vsyncDetect = 0;
unsigned long currentTime = 0;
unsigned long startTime = 0;
unsigned long elapsedTime = 0;

//The init sequence is sent on a software i2c bus.
// sda is on 4 and scl is on 5
SoftwareWire softWire( 4, 5);




void setup() {

  //define pin direction
  pinMode(solid_state_relay_Pin, OUTPUT);
  pinMode(powerButtonPin, INPUT);
  pinMode(vsyncPin, INPUT);//this pin is used to monitor VSYNC.
  pinMode(8, INPUT);//this pin is on the J5 connector for general use PB0.
  pinMode(9, INPUT);//this pin is on the J5 connector for general use PB1.

  EEPROMwl.begin(CONFIG_EEPROM_VERSION, CONFIG_EEPROM_SLOTS + 2);
  Wire.begin(0x50); //join as slave and wait for EDID requests
  softWire.begin();// join as master and send init sequence
  Serial.begin(115200);//use built in serial
  Serial.setTimeout(1000);

  Wire.onRequest(requestEvent); //event handler for requests from master
  Wire.onReceive(receiveData); // event handler when receiving from  master
  // turn it all off
  externalCircuitOff();

  Serial.println(F("iMac G3 IVAD Init ready."));
  Serial.println(F("Type '?' for help."));

  // Sentinel is version-dependent: changing CONFIG_EEPROM_VERSION always triggers first run.
  const byte FIRST_RUN_SENTINEL = 0x79 ^ CONFIG_EEPROM_VERSION;
  FIRST_RUN = EEPROMwl.read(CONFIG_EEPROM_SLOTS);
  if (FIRST_RUN != FIRST_RUN_SENTINEL ) {
    Serial.print(F("[EEPROM] First run detected (sentinel="));
    Serial.print(FIRST_RUN);
    Serial.print(F(", expected="));
    Serial.print(FIRST_RUN_SENTINEL);
    Serial.println(F("). Writing defaults."));
    EEPROMwl.update(CONFIG_EEPROM_SLOTS, FIRST_RUN_SENTINEL);
    EEPROMwl.update(AUTO_POWER_ON_EEPROM_SLOT, 0x00);
    Serial.println(F("[EEPROM] Wrote first-run sentinel and auto power-on default."));
    settings_reset_default();
    settings_store();
    settings_load();
    ivad_write_settings();
  }//end if

  // load auto vsync power-on setting; treat anything other than 0x01 as disabled
  autoVsyncPowerOn = (EEPROMwl.read(AUTO_POWER_ON_EEPROM_SLOT) == 0x01) ? 1 : 0;
  Serial.print(F("[AUTO] Auto power on: "));
  Serial.println(autoVsyncPowerOn ? F("ENABLED") : F("DISABLED"));

}//end setup

void loop() {

  buttonState = digitalRead(powerButtonPin);

  serial_processing();

  // do stuff only when the CRT is on
  if ( externalCircuitState == HIGH ) {

    currentTime = millis();
    elapsedTime = currentTime - startTime;


    //increment vsyncDetect everytime vsync is detected
    if (pulseIn(vsyncPin, HIGH, 30000) > 0) {

      if (vsyncDetect < vsync_off_time) {
        vsyncDetect++;
      }//end if
      startTime = currentTime = millis();

      if (!prevVsyncActive) {
        prevVsyncActive = 1;
        Serial.println(F("[VSYNC] signal detected."));
      } else if (vsyncWasDecrementing) {
        vsyncWasDecrementing = 0;
        Serial.print(F("[VSYNC] signal restored, countdown: "));
        Serial.println(vsyncDetect);
      }
    }//end if


    //decrement vsyncDetect whenever one second elapses
    if (elapsedTime >= 1000 && vsyncDetect > 0) {
      vsyncDetect--;
      vsyncWasDecrementing = 1;
      Serial.print(F("[VSYNC] countdown: "));
      Serial.println(vsyncDetect);
      startTime = currentTime = millis();
    }

    //do stuff whn vsyncDetect is 0
    if (vsyncDetect <= 0) {
      startTime = 0;
      currentTime = 0;
      prevVsyncActive = 0;
      Serial.println(F("[VSYNC] signal lost. Powering off."));
      externalCircuitOff();
    }

  }//end if

  // auto power on: monitor vsync even when display is off
  if (externalCircuitState == LOW && (autoVsyncPowerOn || userRequestedOff)) {
    if (pulseIn(vsyncPin, HIGH, 30000) > 0) {
      if (autoVsyncPowerOn && !userRequestedOff) {
        Serial.println(F("[AUTO] Vsync detected. Powering on."));
        externalCircuitOn();
        startTime = millis();
        currentTime = millis();
        vsyncDetect = vsync_off_time;
      }
    } else {
      // vsync is absent: clear the manual-off lock so auto power-on can fire next time
      if (userRequestedOff) {
        userRequestedOff = 0;
        Serial.println(F("[AUTO] Vsync absent. Manual off lock cleared."));
      }
    }
  }

  if (buttonState == LOW )
  {
    if (buttonPressedTime <= 10) {
      buttonPressedTime++;
    }//end if



  }
  else
  {
    //buttonPressedTime = 0;

  }

  //turn everything off if button is pressed for 10 ms
  if (buttonPressedTime > 0 && externalCircuitState == HIGH && buttonState == HIGH) {
    userRequestedOff = 1;
    externalCircuitOff();
    buttonPressedTime = 0;

  }

  //turn everything on if button is pressed for 10 ms
  if (buttonPressedTime > 0 && externalCircuitState == LOW  && buttonState == HIGH) {
    externalCircuitOn();
    buttonPressedTime = 0;
    startTime = millis();
    currentTime = millis();
    vsyncDetect = vsync_off_time;

  }




}//end loop



void handleSerial(char incoming) {
  /*
     a = move left
     s = move right
     w = move up
     z = move down

     d = skinnier
     f = fatter
     r = taller
     c = shorter

     g = contrast down
     h = contrast up

     j = brightness down
     k = brightness up


  */


  //if (Serial.available() > 0) {
  // char incoming = Serial.read();

  int index = -1;
  bool increment = true;
  const __FlashStringHelper* desc = NULL;
  switch (incoming) {
    case 'a':
      index = IVAD_SETTING_HORIZONTAL_POS;
      desc = F("move left");
      break;
    case 's':
      index = IVAD_SETTING_HORIZONTAL_POS;
      increment = false;
      desc = F("move right");
      break;
    case 'w':
      index = IVAD_SETTING_VERTICAL_POS;
      increment = false;
      desc = F("move up");
      break;
    case 'z':
      index = IVAD_SETTING_VERTICAL_POS;
      desc = F("move down");
      break;
    case 'd':
      index = IVAD_SETTING_WIDTH;
      desc = F("narrower");
      break;
    case 'f':
      index = IVAD_SETTING_WIDTH;
      increment = false;
      desc = F("wider");
      break;
    case 'r':
      index = IVAD_SETTING_HEIGHT;
      desc = F("taller");
      break;
    case 'c':
      index = IVAD_SETTING_HEIGHT;
      increment = false;
      desc = F("shorter");
      break;
    case 'g':
      index = IVAD_SETTING_CONTRAST;
      increment = false;
      desc = F("contrast -");
      break;
    case 'h':
      index = IVAD_SETTING_CONTRAST;
      desc = F("contrast +");
      break;
    case 'j':
      index = IVAD_SETTING_BRIGHTNESS;
      increment = false;
      desc = F("brightness -");
      break;
    case 'k':
      index = IVAD_SETTING_BRIGHTNESS;
      desc = F("brightness +");
      break;
    case 'x':
      index = IVAD_SETTING_PARALLELOGRAM;
      desc = F("parallelogram +");
      break;
    case 'v':
      index = IVAD_SETTING_PARALLELOGRAM;
      increment = false;
      desc = F("parallelogram -");
      break;
    case 'b':
      index = IVAD_SETTING_KEYSTONE;
      increment = false;
      desc = F("keystone pinch top");
      break;
    case 'n':
      index = IVAD_SETTING_KEYSTONE;
      desc = F("keystone pinch bot");
      break;
    case 't':
      index = IVAD_SETTING_ROTATION;
      desc = F("rotate CW");
      break;
    case 'y':
      index = IVAD_SETTING_ROTATION;
      increment = false;
      desc = F("rotate CCW");
      break;
    case 'u':
      index = IVAD_SETTING_PINCUSHION;
      increment = false;
      desc = F("pincushion out");
      break;
    case 'i':
      index = IVAD_SETTING_PINCUSHION;
      desc = F("pincushion in");
      break;
    case 'p':
      printCurrentSettings();
      break;
    case 'e'://power on
      if ( externalCircuitState == LOW ) {
        externalCircuitOn();
        startTime = millis();
        currentTime = millis();
        vsyncDetect = vsync_off_time;
        Serial.println(F("[POWER] Display turned on."));
      } else {
        Serial.println(F("[POWER] Display already on."));
      }
      break;
    case 'o'://power off
      if ( externalCircuitState == HIGH ) {
        userRequestedOff = 1;
        externalCircuitOff();
        Serial.println(F("[POWER] Display turned off."));
      } else {
        Serial.println(F("[POWER] Display already off."));
      }
      break;
    case 'q':
      autoVsyncPowerOn = autoVsyncPowerOn ? 0 : 1;
      EEPROMwl.update(AUTO_POWER_ON_EEPROM_SLOT, autoVsyncPowerOn);
      Serial.print(F("[AUTO] Auto power on: "));
      Serial.println(autoVsyncPowerOn ? F("ENABLED") : F("DISABLED"));
      Serial.print(F("[EEPROM] Saved auto power-on = "));
      Serial.println(autoVsyncPowerOn);
      break;
    case 'l'://re-run init sequence + apply settings
      if ( externalCircuitState == HIGH ) {
        Serial.println(F("[IVAD] Re-running init sequence..."));
        initIvadBoard();
        settings_load();
        ivad_write_settings();
        Serial.println(F("[IVAD] Done. Check for image."));
      } else {
        Serial.println(F("[IVAD] Display is off. Power on first with 'e'."));
      }
      break;
    case 'm'://save current settings to EEPROM
      settings_store();
      break;
    case '?':
      printHelp();
      break;
  }
  //}

  if (index > -1) {
    int val = CURRENT_CONFIG[index];
    if (increment) {
      val++;
    }
    else
    {
      val--;
    }

    ivad_change_setting(index, val);
    byte actual = CURRENT_CONFIG[index];
    Serial.print(F("[SET] "));
    Serial.print(desc);
    Serial.print(F(" = "));
    Serial.print(actual);
    if (actual == VIDEO_CONFIG_MAX[index]) Serial.print(F(" (MAX)"));
    else if (actual == VIDEO_CONFIG_MIN[index]) Serial.print(F(" (MIN)"));
    Serial.println();
  }//end if


}//end handleSerial

void printSettingRow(const __FlashStringHelper* label, byte ivad_setting) {
  byte val  = CURRENT_CONFIG[ivad_setting];
  byte mn   = VIDEO_CONFIG_MIN[ivad_setting];
  byte mx   = VIDEO_CONFIG_MAX[ivad_setting];
  byte span = mx - mn;
  byte pct  = (span > 0) ? map(val, mn, mx, 0, 100) : 0;

  Serial.print(F("  "));
  Serial.print(label);
  Serial.print(F(": "));
  if (val < 100) Serial.print(' ');
  if (val < 10)  Serial.print(' ');
  Serial.print(val);
  Serial.print(F("  ("));
  if (pct < 100) Serial.print(' ');
  if (pct < 10)  Serial.print(' ');
  Serial.print(pct);
  Serial.print(F("%  min="));
  if (mn < 100) Serial.print(' ');
  if (mn < 10)  Serial.print(' ');
  Serial.print(mn);
  Serial.print(F(" max="));
  if (mx < 100) Serial.print(' ');
  if (mx < 10)  Serial.print(' ');
  Serial.print(mx);
  Serial.println(')');
}

void printCurrentSettings() {
  Serial.println(F("======= CURRENT SETTINGS ======="));

  Serial.println(F("  -- Display Status --"));
  Serial.print(F("  power       : "));
  Serial.println(externalCircuitState == HIGH ? F("ON") : F("OFF"));
  Serial.print(F("  vsync       : "));
  Serial.println(prevVsyncActive ? F("detected") : F("not detected"));
  Serial.print(F("  vsync timer : "));
  Serial.print(vsyncDetect);
  Serial.print('/');
  Serial.println(vsync_off_time);
  Serial.print(F("  auto pwr on : "));
  Serial.println(autoVsyncPowerOn ? F("ENABLED") : F("DISABLED"));
  Serial.print(F("  manual off  : "));
  Serial.println(userRequestedOff ? F("locked (waiting for vsync loss)") : F("no lock"));


  Serial.println(F("  -- Size --"));
  printSettingRow(F("height      "), IVAD_SETTING_HEIGHT);
  printSettingRow(F("width       "), IVAD_SETTING_WIDTH);

  Serial.println(F("  -- Position --"));
  printSettingRow(F("horiz pos   "), IVAD_SETTING_HORIZONTAL_POS);
  printSettingRow(F("vert pos    "), IVAD_SETTING_VERTICAL_POS);

  Serial.println(F("  -- Geometry --"));
  printSettingRow(F("rotation    "), IVAD_SETTING_ROTATION);
  printSettingRow(F("parallelgrm "), IVAD_SETTING_PARALLELOGRAM);
  printSettingRow(F("keystone    "), IVAD_SETTING_KEYSTONE);
  printSettingRow(F("pincushion  "), IVAD_SETTING_PINCUSHION);
  printSettingRow(F("s-correct   "), IVAD_SETTING_S_CORRECTION);
  printSettingRow(F("pinc balance"), IVAD_SETTING_PINCUSHION_BALANCE);

  Serial.println(F("  -- Color/Luma --"));
  printSettingRow(F("contrast    "), IVAD_SETTING_CONTRAST);
  printSettingRow(F("brightness  "), IVAD_SETTING_BRIGHTNESS);
  printSettingRow(F("red drive   "), IVAD_SETTING_RED_DRIVE);
  printSettingRow(F("green drive "), IVAD_SETTING_GREEN_DRIVE);
  printSettingRow(F("blue drive  "), IVAD_SETTING_BLUE_DRIVE);
  printSettingRow(F("red cutoff  "), IVAD_SETTING_RED_CUTOFF);
  printSettingRow(F("green cutoff"), IVAD_SETTING_GREEN_CUTOFF);
  printSettingRow(F("blue cutoff "), IVAD_SETTING_BLUE_CUTOFF);

  Serial.println(F("================================"));
}

void printHelp() {
  Serial.println(F("========== HELP =========="));
  Serial.println(F("--- Power ---"));
  Serial.println(F("  e  : power on"));
  Serial.println(F("  o  : power off"));
  Serial.println(F("  l  : re-run IVAD init sequence (display must be on)"));
  Serial.println(F("  q  : toggle auto power on when vsync detected (saved to EEPROM)"));
  Serial.println(F("--- Position ---"));
  Serial.println(F("  a  : move left      s  : move right"));
  Serial.println(F("  w  : move up        z  : move down"));
  Serial.println(F("--- Size ---"));
  Serial.println(F("  d  : narrower       f  : wider"));
  Serial.println(F("  r  : taller         c  : shorter"));
  Serial.println(F("--- Geometry ---"));
  Serial.println(F("  x  : parallelogram +   v  : parallelogram -"));
  Serial.println(F("  b  : keystone pinch top   n  : keystone pinch bot"));
  Serial.println(F("  t  : rotate CW      y  : rotate CCW"));
  Serial.println(F("  u  : pincushion out  i  : pincushion in"));
  Serial.println(F("--- Color/Luma ---"));
  Serial.println(F("  g  : contrast -     h  : contrast +"));
  Serial.println(F("  j  : brightness -   k  : brightness +"));
  Serial.println(F("--- Save ---"));
  Serial.println(F("  m  : save current settings to EEPROM"));
  Serial.println(F("--- Info ---"));
  Serial.println(F("  p  : print current settings"));
  Serial.println(F("  ?  : print this help"));
  Serial.println(F("=========================="));
}

byte i2cErrors = 0;

void writeToIvad(byte address, byte message) {
  softWire.beginTransmission(address);
  softWire.write(message);
  byte err = softWire.endTransmission();
  if (err) i2cErrors++;

}//end method

void writeToIvad(byte address, byte message1, byte message2) {
  softWire.beginTransmission(address);
  softWire.write(message1);
  softWire.write(message2);
  byte err = softWire.endTransmission();
  if (err) i2cErrors++;

}//end method

void  readFromIvad(byte address, byte bytes) {
  softWire.requestFrom(address, bytes);
  while (softWire.available())
  {
    softWire.read();
  }

}//end method


void initIvadBoard() {

  Serial.println(F("[IVAD] Init sequence starting..."));
  i2cErrors = 0;

  writeToIvad( 0x46,0x13,0x00);
  writeToIvad(0x46,0x13,0x00);
  readFromIvad(0x46,1);
  writeToIvad(0x46,0x09,0x00);
  writeToIvad(0x53,0x33);
  readFromIvad(0x53,1);
  writeToIvad(0x46,0x13,0x0b);
  writeToIvad(0x46,0x00,0x00);
  writeToIvad(0x46,0x08,0xe4);
  writeToIvad(0x46,0x12,0xc9);
  writeToIvad(0x53,0x00);
  readFromIvad(0x53,10);
  writeToIvad(0x53,0x0a);
  readFromIvad(0x53,10);
  writeToIvad(0x53,0x14);
  readFromIvad(0x53,10);
  writeToIvad(0x53,0x1e);
  readFromIvad(0x53,10);
  writeToIvad(0x53,0x28);
  readFromIvad(0x53,10);
  writeToIvad(0x53,0x32);
  readFromIvad(0x53,10);
  writeToIvad(0x53,0x3c);
  readFromIvad(0x53,10);
  writeToIvad(0x53,0x46);
  readFromIvad(0x53,10);
  writeToIvad(0x53,0x50);
  readFromIvad(0x53,10);
  writeToIvad(0x53,0x5a);
  readFromIvad(0x53,10);
  writeToIvad(0x46,0x01,0x82);
  writeToIvad(0x46,0x02,0x82);
  writeToIvad(0x46,0x03,0x82);
  writeToIvad(0x46,0x04,0xa0);
  writeToIvad(0x46,0x05,0xa0);
  writeToIvad(0x46,0x06,0xa0);
  writeToIvad(0x46,0x07,0xad);
  writeToIvad(0x46,0x08,0xe4);
  writeToIvad(0x46,0x09,0x3d);
  writeToIvad(0x46,0x0a,0x9e);
  writeToIvad(0x46,0x0b,0xb4);
  writeToIvad(0x46,0x0c,0xc4);
  writeToIvad(0x46,0x0d,0x27);
  writeToIvad(0x46,0x0e,0xbf);
  writeToIvad(0x46,0x0f,0xc0);
  writeToIvad(0x46,0x10,0x40);
  writeToIvad(0x46,0x11,0x0a);
  writeToIvad(0x46,0x12,0x5b);
  writeToIvad(0x46,0x00,0xff);
  writeToIvad(0x53,0x00);
  readFromIvad(0x53,10);
  writeToIvad(0x53,0x10);
  readFromIvad(0x53,10);
  writeToIvad(0x53,0x20);
  readFromIvad(0x53,10);
  writeToIvad(0x53,0x30);
  readFromIvad(0x53,10);
  writeToIvad(0x46,0x11,0x05);
  writeToIvad(0x46,0x00,0xff);
  writeToIvad(0x46,0x00,0x00);
  writeToIvad(0x46,0x07,0xb1);
  writeToIvad(0x46,0x0d,0x10);
  writeToIvad(0x46,0x0c,0xc7);
  writeToIvad(0x46,0x09,0x4a);
  writeToIvad(0x46,0x08,0xea);
  writeToIvad(0x46,0x0f,0xc0);
  writeToIvad(0x46,0x0b,0xae);
  writeToIvad(0x46,0x12,0x5b);
  writeToIvad(0x46,0x00,0xff);
  writeToIvad(0x46,0x11,0x05);
  writeToIvad(0x46,0x00,0xff);
  writeToIvad(0x46,0x10,0x40);
  writeToIvad(0x46,0x06,0xa0);
  writeToIvad(0x46,0x05,0xa0);
  writeToIvad(0x46,0x04,0xa0);
  writeToIvad(0x46,0x03,0x82);
  writeToIvad(0x46,0x02,0x82);
  writeToIvad(0x46,0x01,0x82);
  writeToIvad(0x46,0x11,0x05);
  writeToIvad(0x46,0x00,0xff);
  writeToIvad(0x46,0x11,0x05);
  writeToIvad(0x46,0x00,0xff);
  writeToIvad(0x46,0x10,0x40);
  writeToIvad(0x46,0x06,0xa0);
  writeToIvad(0x46,0x05,0xa0);
  writeToIvad(0x46,0x04,0xa0);
  writeToIvad(0x46,0x03,0x82);
  writeToIvad(0x46,0x02,0x82);
  writeToIvad(0x46,0x01,0x82);
  writeToIvad(0x46,0x11,0x05);
  writeToIvad(0x46,0x00,0xff);

  Serial.print(F("[IVAD] Init sequence complete. I2C errors: "));
  Serial.println(i2cErrors);
}


void solid_state_relayOn() {
  digitalWrite(solid_state_relay_Pin, HIGH);

}

void solid_state_relayOff() {
  digitalWrite(solid_state_relay_Pin, LOW);

}




void externalCircuitOn() {
  solid_state_relayOn();
  delay(500);
  initIvadBoard();
  settings_load();
  ivad_write_settings();
  externalCircuitState = HIGH;


}

void externalCircuitOff() {
  solid_state_relayOff();
  externalCircuitState = LOW;

}



// function that executes whenever data is requested by master
// this function is registered as an event.
void requestEvent() {
  //delay(500);
  Wire.write(edid, 128);



}//end method
// function that executes whenever data is received by the slave


void receiveData(byte byteCount) {


  while (Wire.available()) {
    data = Wire.read();
  }
}


/*
  Serial input handler. Supports two protocols:
  - Single-character commands (a-z, ?, etc.) for interactive use
  - 9-byte binary packets (oshimai's protocol) for the CRT control panel app
*/
void serial_processing()
{

  byte b ;

  if (Serial.available()) {

    do
    {
      b = Serial.read();
      SERIAL_BUFFER[SERIAL_BUFFER_DATA_SIZE++] = b;
    }
    while (Serial.available() &&  b != SERIAL_EOL_MARKER);


    //call other serial handler
    if (SERIAL_BUFFER_DATA_SIZE != 9)
    {
      if (SERIAL_BUFFER[0] != 0x07) {
        SERIAL_BUFFER_DATA_SIZE = 0;
        handleSerial((char)SERIAL_BUFFER[0]);
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
          byte ret[8] { 0x06, id, 0x01, CONFIG_EEPROM_VERSION, 0x03, 0xFF, 0x04, SERIAL_EOL_MARKER };
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
          byte ret[7] { 0x06, id, 0x00, 0x03, 0xFF, 0x04, SERIAL_EOL_MARKER };
          ret[4] = checksum(ret, 4);
          Serial.write(ret, 7);
        }
        break;

      case 0x04: // IVAD Reset from EEPROM
        {
          settings_load();
          byte ret[7] { 0x06, id, 0x00, 0x03, 0xFF, 0x04, SERIAL_EOL_MARKER };
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
          byte ret[7] { 0x06, id, 0x00, 0x03, 0xFF, 0x04, SERIAL_EOL_MARKER };
          ret[4] = checksum(ret, 4);
          Serial.write(ret, 7);
        }
        break;

      case 0x06: // Write SRAM to EEPROM
        {
          settings_store();
          byte ret[7] { 0x06, id, 0x00, 0x03, 0xFF, 0x04, SERIAL_EOL_MARKER };
          ret[4] = checksum(ret, 4);
          Serial.write(ret, 7);
        }
    }//end switch

    SERIAL_BUFFER[1] = 0xFF;

  }

}//end if









//===================================



byte checksum(const byte arr[], const int len)
{
  int sum = 1; // Checksum may never be 0.

  for (int i = 0; i < len; i++)
    sum += arr[i];

  byte ret = 256 - (sum % 256);

  return ret;
}




int ivad_change_setting(const int ivad_setting, int value)
{

  if (value < (int)VIDEO_CONFIG_MIN[ivad_setting]) value = VIDEO_CONFIG_MIN[ivad_setting];
  if (value > (int)VIDEO_CONFIG_MAX[ivad_setting]) value = VIDEO_CONFIG_MAX[ivad_setting];
  CURRENT_CONFIG[ivad_setting] = (byte)value;

  writeToIvad(IVAD_REGISTER_PROPERTY, ivad_setting, CURRENT_CONFIG[ivad_setting]);
  CURRENT_CONFIG[CONFIG_OFFSET_CHECKSUM] = checksum(CURRENT_CONFIG, CONFIG_EEPROM_SLOTS - 1);

  return 0;
}



/*
  This function loads the monitor property values from the EEPROM
  into variables.
*/
void settings_load()
{
  // Set something so a checksum mismatch can trigger if there's nothing in the EEPROM.

  for (byte eeprom_memory_offset = 0 ; eeprom_memory_offset < CONFIG_EEPROM_SLOTS ; eeprom_memory_offset++) {
    CURRENT_CONFIG[eeprom_memory_offset] = EEPROMwl.read(eeprom_memory_offset);
  }//end for

  byte loaded_checksum = CURRENT_CONFIG[CONFIG_OFFSET_CHECKSUM];
  byte expected_checksum = checksum(CURRENT_CONFIG, CONFIG_EEPROM_SLOTS - 1);

  if (loaded_checksum != expected_checksum)
  {
    Serial.print(F("[EEPROM] Checksum mismatch (loaded="));
    Serial.print(loaded_checksum);
    Serial.print(F(", expected="));
    Serial.print(expected_checksum);
    Serial.println(F("). Resetting to defaults."));
    settings_reset_default();
    settings_store();
  }


}

void ivad_write_settings()
{
  i2cErrors = 0;
  for (int IVAD_SETTING = 0 ; IVAD_SETTING < IVAD_SETTING_END ;  IVAD_SETTING ++ )
  {
    writeToIvad(IVAD_REGISTER_PROPERTY, IVAD_SETTING, CURRENT_CONFIG[IVAD_SETTING]);
  }
  Serial.print(F("[IVAD] Settings written. I2C errors: "));
  Serial.println(i2cErrors);

}



void printSettingName(byte slot) {
  switch (slot) {
    case CONFIG_OFFSET_CONTRAST:          Serial.print(F("contrast        ")); break;
    case CONFIG_OFFSET_RED_DRIVE:         Serial.print(F("red drive       ")); break;
    case CONFIG_OFFSET_GREEN_DRIVE:       Serial.print(F("green drive     ")); break;
    case CONFIG_OFFSET_BLUE_DRIVE:        Serial.print(F("blue drive      ")); break;
    case CONFIG_OFFSET_RED_CUTOFF:        Serial.print(F("red cutoff      ")); break;
    case CONFIG_OFFSET_GREEN_CUTOFF:      Serial.print(F("green cutoff    ")); break;
    case CONFIG_OFFSET_BLUE_CUTOFF:       Serial.print(F("blue cutoff     ")); break;
    case CONFIG_OFFSET_HORIZONTAL_POS:    Serial.print(F("horiz pos       ")); break;
    case CONFIG_OFFSET_HEIGHT:            Serial.print(F("height          ")); break;
    case CONFIG_OFFSET_VERTICAL_POS:      Serial.print(F("vert pos        ")); break;
    case CONFIG_OFFSET_S_CORRECTION:      Serial.print(F("s-correction    ")); break;
    case CONFIG_OFFSET_KEYSTONE:          Serial.print(F("keystone        ")); break;
    case CONFIG_OFFSET_PINCUSHION:        Serial.print(F("pincushion      ")); break;
    case CONFIG_OFFSET_WIDTH:             Serial.print(F("width           ")); break;
    case CONFIG_OFFSET_PINCUSHION_BALANCE:Serial.print(F("pinc balance    ")); break;
    case CONFIG_OFFSET_PARALLELOGRAM:     Serial.print(F("parallelogram   ")); break;
    case CONFIG_OFFSET_RESERVED6:         Serial.print(F("reserved6       ")); break;
    case CONFIG_OFFSET_BRIGHTNESS:        Serial.print(F("brightness      ")); break;
    case CONFIG_OFFSET_ROTATION:          Serial.print(F("rotation        ")); break;
    case CONFIG_OFFSET_CHECKSUM:          Serial.print(F("checksum        ")); break;
    default:                              Serial.print(F("unknown         ")); break;
  }
}

void settings_store()
{

  byte current_config_checksum = checksum(CURRENT_CONFIG, CONFIG_EEPROM_SLOTS - 1);
  CURRENT_CONFIG[CONFIG_OFFSET_CHECKSUM] = current_config_checksum;

  Serial.println(F("[EEPROM] Saving config..."));
  for (byte slot = 0 ; slot < CONFIG_EEPROM_SLOTS ; slot++) {
    byte old_val = EEPROMwl.read(slot);
    EEPROMwl.update(slot, CURRENT_CONFIG[slot]);
    Serial.print(F("  ["));
    if (slot < 10) Serial.print(' ');
    Serial.print(slot);
    Serial.print(F("] "));
    printSettingName(slot);
    Serial.print(F(": "));
    Serial.print(CURRENT_CONFIG[slot]);
    if (old_val != CURRENT_CONFIG[slot]) {
      Serial.print(F("  (was "));
      Serial.print(old_val);
      Serial.print(')');
    }
    Serial.println();
  }
  Serial.print(F("[EEPROM] Save complete. Checksum: "));
  Serial.println(current_config_checksum);

}


void settings_reset_default()
{

  Serial.println(F("[EEPROM] Resetting CURRENT_CONFIG to defaults."));
  for (byte eeprom_memory_offset = 0 ; eeprom_memory_offset < CONFIG_EEPROM_SLOTS ; eeprom_memory_offset++) {
    CURRENT_CONFIG[eeprom_memory_offset] = VIDEO_CONFIG_DEFAULT[eeprom_memory_offset];
  }//end for

  byte current_config_checksum = checksum(CURRENT_CONFIG, CONFIG_EEPROM_SLOTS - 1);
  CURRENT_CONFIG[CONFIG_OFFSET_CHECKSUM] = current_config_checksum;



}
