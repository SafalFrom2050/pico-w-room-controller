#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <SimpleMDNS.h>
#define RAW_BUFFER_LENGTH 750 // For air condition remotes (144+ bits)
#include <IRremote.hpp> // Use IRremote for hardware pulsing
#include "config.h"

// Hardware Objects
WebServer server(WEB_PORT);

// State Variables
bool acPower = false;
int acTargetTemp = 24;
String acMode = "cool"; // cool, heat, off
bool acPowerful = false;

// Light State Variables
bool normalLightState = false;
bool nightLightState = false;

// IR Receiver and TV State Variables
String lastRxProtocol = "UNKNOWN";
uint8_t lastRxProtocolId = 0;
uint16_t lastRxAddress = 0;
uint32_t lastRxCommand = 0;
uint64_t lastRxRawData = 0;
uint16_t lastRxBits = 0;
uint32_t lastRxTimestamp = 0;
bool lastRxValid = false;

struct TVButton {
    uint8_t protocol;  // 0 = unconfigured
    uint16_t address;
    uint32_t command;
    uint64_t rawData;
};

TVButton tvPower = {0, 0, 0, 0};
TVButton tvNetflix = {0, 0, 0, 0};


// --- Custom Fujitsu Protocol Sender (Manual Mark/Space) ---
// Timings from IRremoteESP8266's ir_Fujitsu.cpp (verified/stable)
#define FUJI_HDR_MARK   3324
#define FUJI_HDR_SPACE  1574
#define FUJI_BIT_MARK   448
#define FUJI_ONE_SPACE  1182
#define FUJI_ZERO_SPACE 390
#define FUJI_MIN_GAP    8100

void sendIRProtocol(uint8_t protocolId, uint16_t address, uint32_t command) {
    if (protocolId == 0) {
        Serial.println("IR Send: Protocol ID 0 (unconfigured)");
        return;
    }
    
    // Stop receiver to prevent jitter
    IrReceiver.stop();
    delay(5);
    
    decode_type_t protocol = (decode_type_t)protocolId;
    Serial.printf("IR Send: Protocol=%s(%d), Address=0x%04X, Command=0x%08X\n",
                  getProtocolString(protocol), protocolId, address, command);
                  
    switch (protocol) {
        case NEC:
            IrSender.sendNEC(address, command, 0);
            break;
        case SONY:
            IrSender.sendSony(address, command, 0);
            break;
        case SAMSUNG:
            IrSender.sendSamsung(address, command, 0);
            break;
        case PANASONIC:
            IrSender.sendPanasonic(address, command, 0);
            break;
        case RC5:
            IrSender.sendRC5(address, command, 0);
            break;
        case RC6:
            IrSender.sendRC6(address, command, 0);
            break;
        default:
            Serial.printf("Unsupported protocol for send: %d\n", protocolId);
            break;
    }
    
    delay(5);
    // Restart receiver
    IrReceiver.start();
}
void sendTVButton(const TVButton& btn) {
    if (btn.protocol == 0) {
        Serial.println("IR Send: Protocol ID 0 (unconfigured)");
        return;
    }
    
    decode_type_t protocol = (decode_type_t)btn.protocol;
    
    if (protocol == PULSE_DISTANCE) {
        // Stop receiver to prevent jitter
        IrReceiver.stop();
        delay(5);
        
        Serial.print("IR Send (PulseDistance): RawData=0x");
        if (btn.rawData > 0xFFFFFFFFULL) {
            Serial.print((uint32_t)(btn.rawData >> 32), HEX);
            Serial.printf("%08X\n", (uint32_t)(btn.rawData & 0xFFFFFFFF));
        } else {
            Serial.println((uint32_t)btn.rawData, HEX);
        }
        
        IrSender.sendPulseDistanceWidth(
            38,                      // frequency
            3500,                    // header mark
            1650,                    // header space
            400,                     // one mark
            1300,                    // one space
            400,                     // zero mark
            400,                     // zero space
            btn.rawData,             // data (uint64_t)
            40,                      // number of bits
            0,                       // flags (LSB first)
            110,                     // repeat period in ms
            0,                       // repeats
            nullptr                  // special repeat callback
        );
        
        delay(5);
        // Restart receiver
        IrReceiver.start();
    } else {
        sendIRProtocol(btn.protocol, btn.address, btn.command);
    }
}


// Low-level sender: builds raw timing array and uses IrSender.sendRaw().
// This is the most proven path in the IRremote library.
void sendFujitsuRaw(const uint8_t* data, uint16_t nbytes) {
    // Stop receiver to prevent jitter
    IrReceiver.stop();
    delay(5);

    // Build raw timing array: header(2) + bits(nbytes*16) + stop(1)
    uint16_t rawLen = 2 + (nbytes * 16) + 1;
    uint16_t rawBuf[rawLen];
    uint16_t idx = 0;

    // Header
    rawBuf[idx++] = FUJI_HDR_MARK;
    rawBuf[idx++] = FUJI_HDR_SPACE;

    // Data bits (LSB first, byte by byte)
    for (uint16_t i = 0; i < nbytes; i++) {
        for (uint8_t bit = 0; bit < 8; bit++) {
            rawBuf[idx++] = FUJI_BIT_MARK;
            if (data[i] & (1 << bit)) {
                rawBuf[idx++] = FUJI_ONE_SPACE;   // 1
            } else {
                rawBuf[idx++] = FUJI_ZERO_SPACE;   // 0
            }
        }
    }

    // Stop bit (trailing mark)
    rawBuf[idx++] = FUJI_BIT_MARK;

    // Send using IRremote's built-in sendRaw
    IrSender.sendRaw(rawBuf, idx, 38);

    delay(5);
    // Restart receiver
    IrReceiver.start();

    // Print hex dump of what was sent
    Serial.print("IR TX ["); Serial.print(nbytes); Serial.print("B]: ");
    for (uint16_t i = 0; i < nbytes; i++) {
        Serial.printf("%02X ", data[i]);
    }
    Serial.println();
}

void sendFujitsuShort(uint8_t cmd) {
    // 7-byte short code: Header(5) + Cmd + ~Cmd
    uint8_t data[7] = {0x14, 0x63, 0x00, 0x10, 0x10, cmd, (uint8_t)(~cmd)};
    sendFujitsuRaw(data, 7);
    Serial.println("IR: Sent Fujitsu Short Code");
}

void sendFujitsuLong(bool isStart, uint8_t temp, String mode) {
    // AR-RFL5J 18-byte protocol (decoded from raw captures 2026-02-19)
    // Bytes 0-6: Fixed header + RestLength
    uint8_t data[18] = {0x14, 0x63, 0x00, 0x10, 0x10, 0xFE, 0x0B,
                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    // Byte 7: Power ON constant (0x41 for all ON long codes)
    data[7] = 0x41;

    // Byte 8: Temperature + Power flag
    //   bit 0: Power (1=ON)
    //   bit 1: Fahrenheit (0=Celsius)
    //   bits 4-7: (temp - 8) / 2
    uint8_t tempVal = (temp - 8) / 2;
    data[8] = (tempVal << 4) | 0x01;  // Power ON, Celsius

    // Byte 9: Mode
    //   0x00 = Auto, 0x01 = Cool, 0x02 = Dry, 0x04 = Heat
    if (mode == "cool") {
        data[9] = 0x01;
    } else if (mode == "heat") {
        data[9] = 0x04;
    } else if (mode == "dry") {
        data[9] = 0x02;
    } else {
        data[9] = 0x00;  // Auto
    }

    // Bytes 10-14: zeros (fan/swing/timer defaults)

    // Bytes 15-16: Fan/swing settings (default values from captures)
    data[15] = 0x12;
    data[16] = 0x04;

    // Byte 17: Checksum = 0x100 - sum(bytes 7..16)
    uint8_t sum = 0;
    for (int i = 7; i < 17; i++) {
        sum += data[i];
    }
    data[17] = (uint8_t)(0x100 - sum);

    sendFujitsuRaw(data, 18);
    Serial.printf("IR: Sent Fujitsu Long | Temp:%d | Mode:%s | Byte8:0x%02X | Chk:0x%02X\n",
                  temp, mode.c_str(), data[8], data[17]);
}

void sendIRCommand() {
  Serial.print("Sending IR Command: ");
  Serial.print(acPower ? "ON" : "OFF");
  Serial.print(" | Temp: "); Serial.print(acTargetTemp);
  Serial.print(" | Mode: "); Serial.println(acMode);

  if (acPower) {
      // Use the custom Fujitsu long-frame sender for ON and temperature updates
      // Determine if we are just starting or updating settings
      static bool lastAcPower = false;
      bool isStart = (acPower && !lastAcPower);
      
      sendFujitsuLong(isStart, (uint8_t)acTargetTemp, acMode);
      lastAcPower = acPower;
  } else {
      // Use the verified 56-bit Short Code for OFF
      sendFujitsuShort(0x02);
  }
}

// --- Web Handlers ---
void handleRoot() {
  Serial.print("--- Request received: ");
  Serial.println(server.uri());
  
  String json = "{";
  json += "\"targetHeatingCoolingState\":" + String(acPower ? (acMode == "heat" ? 1 : 2) : 0) + ",";
  json += "\"targetTemperature\":" + String((float)acTargetTemp, 1) + ",";
  json += "\"currentHeatingCoolingState\":" + String(acPower ? (acMode == "heat" ? 1 : 2) : 0) + ",";
  json += "\"currentTemperature\":" + String((float)acTargetTemp, 1);
  json += "}";
  
  Serial.println("Response: " + json);
  server.send(200, "application/json", json);
}

void handleTargetTemp() {
  String val = server.arg("value");
  Serial.printf("SET /targetTemperature value=%s\n", val.c_str());
  if (val.length() > 0) {
    acTargetTemp = val.toInt();
    acPowerful = false; // Changing temp cancels powerful mode
    sendIRCommand();
  }
  server.send(200, "text/plain", "OK");
}

void handleTargetState() {
  String val = server.arg("value");
  Serial.printf("SET /targetHeatingCoolingState value=%s\n", val.c_str());
  if (val.length() > 0) {
    int state = val.toInt();
    if (state == 0) {
      acPower = false;
      acMode = "off";
    } else if (state == 1) {
      acPower = true;
      acMode = "heat";
    } else if (state == 2) {
      acPower = true;
      acMode = "cool";
    }
    acPowerful = false; // Changing mode cancels powerful mode
    sendIRCommand();
  }
  server.send(200, "text/plain", "OK");
}

void handlePowerful() {
  String val = server.arg("value");
  Serial.printf("SET /powerful value=%s\n", val.c_str());
  if (val.length() > 0) {
    int state = val.toInt();
    if (state == 1) {
      // Send 56-bit short code 0x39 to trigger Powerful Mode
      sendFujitsuShort(0x39);
      acPowerful = true;
    } else {
      // Resending the regular state cancels Powerful Mode on Fujitsu ACs
      sendIRCommand();
      acPowerful = false;
    }
  }
  server.send(200, "text/plain", "OK");
}

void handlePowerfulStatus() {
  server.send(200, "text/plain", acPowerful ? "1" : "0");
}
// --- EEPROM Settings & Storage ---
#include <EEPROM.h>
#define EEPROM_MAGIC_ADDR           0
#define EEPROM_THRESHOLD_ADDR       1
#define EEPROM_NIGHT_THRESHOLD_ADDR 3
#define EEPROM_MAGIC_VAL            0xAA

int lightOnThreshold = LIGHT_ON_THRESHOLD;       // Mutable normal mode threshold
int lightNightThreshold = LIGHT_NIGHT_THRESHOLD; // Mutable night mode threshold

void saveSettings() {
  EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VAL);
  EEPROM.write(EEPROM_THRESHOLD_ADDR, (lightOnThreshold >> 8) & 0xFF);
  EEPROM.write(EEPROM_THRESHOLD_ADDR + 1, lightOnThreshold & 0xFF);
  EEPROM.write(EEPROM_NIGHT_THRESHOLD_ADDR, (lightNightThreshold >> 8) & 0xFF);
  EEPROM.write(EEPROM_NIGHT_THRESHOLD_ADDR + 1, lightNightThreshold & 0xFF);
  EEPROM.commit();
  Serial.printf("EEPROM: Saved lightOnThreshold = %d, lightNightThreshold = %d\n", lightOnThreshold, lightNightThreshold);
}

void loadSettings() {
  EEPROM.begin(256);
  uint8_t magic = EEPROM.read(EEPROM_MAGIC_ADDR);
  if (magic == EEPROM_MAGIC_VAL) {
    uint16_t msb = EEPROM.read(EEPROM_THRESHOLD_ADDR);
    uint16_t lsb = EEPROM.read(EEPROM_THRESHOLD_ADDR + 1);
    lightOnThreshold = (msb << 8) | lsb;
    
    uint16_t n_msb = EEPROM.read(EEPROM_NIGHT_THRESHOLD_ADDR);
    uint16_t n_lsb = EEPROM.read(EEPROM_NIGHT_THRESHOLD_ADDR + 1);
    lightNightThreshold = (n_msb << 8) | n_lsb;
    
    // Bounds check to handle migration from single-threshold layouts
    if (lightOnThreshold < 0 || lightOnThreshold > 1023) {
      lightOnThreshold = LIGHT_ON_THRESHOLD;
    }
    if (lightNightThreshold < 0 || lightNightThreshold > 1023) {
      lightNightThreshold = LIGHT_NIGHT_THRESHOLD;
    }
    
    Serial.printf("EEPROM: Loaded lightOnThreshold = %d, lightNightThreshold = %d\n", lightOnThreshold, lightNightThreshold);
  } else {
    Serial.println("EEPROM: Magic value missing. Initializing with defaults...");
    lightOnThreshold = LIGHT_ON_THRESHOLD;
    lightNightThreshold = LIGHT_NIGHT_THRESHOLD;
    saveSettings();
  }
}

#define EEPROM_TV_POWER_PROTOCOL_ADDR 5
#define EEPROM_TV_POWER_ADDRESS_ADDR  6
#define EEPROM_TV_POWER_COMMAND_ADDR  8
#define EEPROM_TV_POWER_RAWDATA_ADDR  12

#define EEPROM_TV_NETFLIX_PROTOCOL_ADDR 20
#define EEPROM_TV_NETFLIX_ADDRESS_ADDR  21
#define EEPROM_TV_NETFLIX_COMMAND_ADDR  23
#define EEPROM_TV_NETFLIX_RAWDATA_ADDR  27

void saveTVButton(bool isNetflix) {
  int startAddr = isNetflix ? EEPROM_TV_NETFLIX_PROTOCOL_ADDR : EEPROM_TV_POWER_PROTOCOL_ADDR;
  TVButton btn = isNetflix ? tvNetflix : tvPower;
  
  EEPROM.write(startAddr, btn.protocol);
  EEPROM.write(startAddr + 1, (btn.address >> 8) & 0xFF);
  EEPROM.write(startAddr + 2, btn.address & 0xFF);
  EEPROM.write(startAddr + 3, (btn.command >> 24) & 0xFF);
  EEPROM.write(startAddr + 4, (btn.command >> 16) & 0xFF);
  EEPROM.write(startAddr + 5, (btn.command >> 8) & 0xFF);
  EEPROM.write(startAddr + 6, btn.command & 0xFF);
  
  // Save rawData (8 bytes)
  int rawAddr = isNetflix ? EEPROM_TV_NETFLIX_RAWDATA_ADDR : EEPROM_TV_POWER_RAWDATA_ADDR;
  for (int i = 0; i < 8; i++) {
    EEPROM.write(rawAddr + i, (btn.rawData >> ((7 - i) * 8)) & 0xFF);
  }
  
  EEPROM.commit();
  
  Serial.print("EEPROM: Saved TV ");
  Serial.print(isNetflix ? "Netflix" : "Power");
  Serial.printf(": Protocol=%d, Address=0x%04X, Command=0x%08X, RawData=0x", btn.protocol, btn.address, btn.command);
  if (btn.rawData > 0xFFFFFFFFULL) {
    Serial.print((uint32_t)(btn.rawData >> 32), HEX);
    Serial.printf("%08X\n", (uint32_t)(btn.rawData & 0xFFFFFFFF));
  } else {
    Serial.println((uint32_t)btn.rawData, HEX);
  }
}

void loadTVButtons() {
  uint8_t magic = EEPROM.read(EEPROM_MAGIC_ADDR);
  if (magic == EEPROM_MAGIC_VAL) {
    // Load TV Power
    tvPower.protocol = EEPROM.read(EEPROM_TV_POWER_PROTOCOL_ADDR);
    if (tvPower.protocol == 255) tvPower.protocol = 0;
    tvPower.address = (EEPROM.read(EEPROM_TV_POWER_ADDRESS_ADDR) << 8) | EEPROM.read(EEPROM_TV_POWER_ADDRESS_ADDR + 1);
    tvPower.command = ((uint32_t)EEPROM.read(EEPROM_TV_POWER_COMMAND_ADDR) << 24) |
                      ((uint32_t)EEPROM.read(EEPROM_TV_POWER_COMMAND_ADDR + 1) << 16) |
                      ((uint32_t)EEPROM.read(EEPROM_TV_POWER_COMMAND_ADDR + 2) << 8) |
                      EEPROM.read(EEPROM_TV_POWER_COMMAND_ADDR + 3);
    tvPower.rawData = 0;
    for (int i = 0; i < 8; i++) {
      tvPower.rawData = (tvPower.rawData << 8) | EEPROM.read(EEPROM_TV_POWER_RAWDATA_ADDR + i);
    }
    
    // Load TV Netflix
    tvNetflix.protocol = EEPROM.read(EEPROM_TV_NETFLIX_PROTOCOL_ADDR);
    if (tvNetflix.protocol == 255) tvNetflix.protocol = 0;
    tvNetflix.address = (EEPROM.read(EEPROM_TV_NETFLIX_ADDRESS_ADDR) << 8) | EEPROM.read(EEPROM_TV_NETFLIX_ADDRESS_ADDR + 1);
    tvNetflix.command = ((uint32_t)EEPROM.read(EEPROM_TV_NETFLIX_COMMAND_ADDR) << 24) |
                        ((uint32_t)EEPROM.read(EEPROM_TV_NETFLIX_COMMAND_ADDR + 1) << 16) |
                        ((uint32_t)EEPROM.read(EEPROM_TV_NETFLIX_COMMAND_ADDR + 2) << 8) |
                        EEPROM.read(EEPROM_TV_NETFLIX_COMMAND_ADDR + 3);
    tvNetflix.rawData = 0;
    for (int i = 0; i < 8; i++) {
      tvNetflix.rawData = (tvNetflix.rawData << 8) | EEPROM.read(EEPROM_TV_NETFLIX_RAWDATA_ADDR + i);
    }

    // Apply fallbacks for PulseDistance (protocolId = 2) if rawData is 0 or unprogrammed (0xFFFFFFFF...)
    if (tvPower.protocol == 2 && (tvPower.rawData == 0 || tvPower.rawData == 0xFFFFFFFFFFFFFFFFULL)) {
      tvPower.rawData = 0x2AD5B9162CULL;
      Serial.println("EEPROM: TV Power rawData fallback applied (0x2AD5B9162C)");
    }
    if (tvNetflix.protocol == 2 && (tvNetflix.rawData == 0 || tvNetflix.rawData == 0xFFFFFFFFFFFFFFFFULL)) {
      tvNetflix.rawData = 0xEF10B9162CULL;
      Serial.println("EEPROM: TV Netflix rawData fallback applied (0xEF10B9162C)");
    }
                        
    Serial.print("EEPROM: Loaded TV Power: Protocol=");
    Serial.print(tvPower.protocol);
    Serial.printf(", Address=0x%04X, Command=0x%08X, RawData=0x", tvPower.address, tvPower.command);
    if (tvPower.rawData > 0xFFFFFFFFULL) {
      Serial.print((uint32_t)(tvPower.rawData >> 32), HEX);
      Serial.printf("%08X\n", (uint32_t)(tvPower.rawData & 0xFFFFFFFF));
    } else {
      Serial.println((uint32_t)tvPower.rawData, HEX);
    }

    Serial.print("EEPROM: Loaded TV Netflix: Protocol=");
    Serial.print(tvNetflix.protocol);
    Serial.printf(", Address=0x%04X, Command=0x%08X, RawData=0x", tvNetflix.address, tvNetflix.command);
    if (tvNetflix.rawData > 0xFFFFFFFFULL) {
      Serial.print((uint32_t)(tvNetflix.rawData >> 32), HEX);
      Serial.printf("%08X\n", (uint32_t)(tvNetflix.rawData & 0xFFFFFFFF));
    } else {
      Serial.println((uint32_t)tvNetflix.rawData, HEX);
    }
  } else {
    tvPower = {0, 0, 0, 0};
    tvNetflix = {0, 0, 0, 0};
  }
}


// --- Light Sensor Helpers ---
int getLightSensorValue() {
  return (analogRead(LIGHT_SENSOR_PIN_L) + analogRead(LIGHT_SENSOR_PIN_R)) / 2;
}

bool isLightPhysicallyOn() {
  int val = getLightSensorValue();
  Serial.printf("Light Sensor: %d (Normal Thresh: %d, Night Thresh: %d)\n", val, lightOnThreshold, lightNightThreshold);
  return val >= lightNightThreshold;
}

void handleLightNormal() {
  String val = server.arg("value");
  Serial.printf("SET /light/normal value=%s\n", val.c_str());
  if (val.length() > 0) {
    int state = val.toInt();
    if (state == 1) {
      if (!isLightPhysicallyOn()) {
        Serial.println("IR Normal: Light is OFF. Sending Power Toggle...");
        sendIRProtocol(NEC, 0x0A01, 0x04);
        delay(500);
      } else {
        Serial.println("IR Normal: Light is already ON. Skipping Power Toggle.");
      }
      Serial.println("IR Normal: Sending Normal Mode x2...");
      sendIRProtocol(NEC, 0x0A01, 0x01);
      delay(150);
      sendIRProtocol(NEC, 0x0A01, 0x01);
      
      normalLightState = true;
      nightLightState = false;
    } else {
      if (isLightPhysicallyOn()) {
        Serial.println("IR Normal: Light is ON. Sending Power Toggle (OFF)...");
        sendIRProtocol(NEC, 0x0A01, 0x04);
      } else {
        Serial.println("IR Normal: Light is already OFF. Skipping Power Toggle.");
      }
      normalLightState = false;
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleLightNormalStatus() {
  server.send(200, "text/plain", normalLightState ? "1" : "0");
}

void handleLightNight() {
  String val = server.arg("value");
  Serial.printf("SET /light/night value=%s\n", val.c_str());
  if (val.length() > 0) {
    int state = val.toInt();
    if (state == 1) {
      if (!isLightPhysicallyOn()) {
        Serial.println("IR Night: Light is OFF. Sending Power Toggle...");
        sendIRProtocol(NEC, 0x0A01, 0x04);
        delay(500);
      } else {
        Serial.println("IR Night: Light is already ON. Skipping Power Toggle.");
      }
      Serial.println("IR Night: Sending Night Mode x2...");
      sendIRProtocol(NEC, 0x0A01, 0x12);
      delay(150);
      sendIRProtocol(NEC, 0x0A01, 0x12);
      
      nightLightState = true;
      normalLightState = false;
    } else {
      if (isLightPhysicallyOn()) {
        Serial.println("IR Night: Light is ON. Sending Power Toggle (OFF)...");
        sendIRProtocol(NEC, 0x0A01, 0x04);
      } else {
        Serial.println("IR Night: Light is already OFF. Skipping Power Toggle.");
      }
      nightLightState = false;
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleLightNightStatus() {
  server.send(200, "text/plain", nightLightState ? "1" : "0");
}

void handleLightStatus() {
  String json = "{";
  json += "\"normal\":" + String(normalLightState ? 1 : 0) + ",";
  json += "\"night\":" + String(nightLightState ? 1 : 0);
  json += "}";
  server.send(200, "application/json", json);
}

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Smart Home Dashboard</title>
    <style>
        :root {
            --bg-color: #0c0f1d;
            --card-bg: rgba(255, 255, 255, 0.05);
            --card-border: rgba(255, 255, 255, 0.1);
            --text-color: #f1f3f9;
            --accent-blue: #3b82f6;
            --accent-orange: #f97316;
            --accent-green: #10b981;
            --text-muted: #94a3b8;
        }
        * {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
        }
        body {
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
            background-color: var(--bg-color);
            color: var(--text-color);
            min-height: 100vh;
            display: flex;
            flex-direction: column;
            align-items: center;
            padding: 20px;
        }
        header {
            margin-bottom: 30px;
            text-align: center;
        }
        header h1 {
            font-size: 2rem;
            font-weight: 700;
            background: linear-gradient(135deg, #60a5fa, #c084fc);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            margin-bottom: 5px;
        }
        header p {
            color: var(--text-muted);
            font-size: 0.9rem;
        }
        .container {
            width: 100%;
            max-width: 900px;
            display: grid;
            grid-template-columns: 1fr;
            gap: 20px;
        }
        @media(min-width: 768px) {
            .container {
                grid-template-columns: 1fr 1fr;
            }
        }
        .card {
            background: var(--card-bg);
            border: 1px solid var(--card-border);
            border-radius: 20px;
            padding: 24px;
            backdrop-filter: blur(12px);
            box-shadow: 0 10px 30px rgba(0, 0, 0, 0.5);
            display: flex;
            flex-direction: column;
        }
        .card-header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 20px;
            border-bottom: 1px solid rgba(255, 255, 255, 0.05);
            padding-bottom: 15px;
        }
        .card-title {
            font-size: 1.25rem;
            font-weight: 600;
        }
        .status-badge {
            font-size: 0.8rem;
            padding: 4px 10px;
            border-radius: 20px;
            font-weight: 500;
        }
        .status-off { background: rgba(239, 68, 68, 0.15); color: #ef4444; }
        .status-cool { background: rgba(59, 130, 246, 0.15); color: #3b82f6; }
        .status-heat { background: rgba(249, 115, 22, 0.15); color: #f97316; }
        .status-on { background: rgba(16, 185, 129, 0.15); color: #10b981; }

        .control-group {
            margin-bottom: 20px;
        }
        .control-label {
            font-size: 0.85rem;
            color: var(--text-muted);
            margin-bottom: 8px;
            text-transform: uppercase;
            letter-spacing: 0.05em;
        }
        .btn-group {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(60px, 1fr));
            gap: 10px;
        }
        .btn {
            background: rgba(255, 255, 255, 0.05);
            border: 1px solid var(--card-border);
            color: var(--text-color);
            padding: 12px;
            border-radius: 12px;
            font-size: 0.95rem;
            font-weight: 500;
            cursor: pointer;
            transition: all 0.2s ease;
            display: flex;
            align-items: center;
            justify-content: center;
        }
        .btn:hover {
            background: rgba(255, 255, 255, 0.1);
            transform: translateY(-2px);
        }
        .btn-active-cool {
            background: var(--accent-blue) !important;
            border-color: var(--accent-blue);
            box-shadow: 0 0 15px rgba(59, 130, 246, 0.4);
        }
        .btn-active-heat {
            background: var(--accent-orange) !important;
            border-color: var(--accent-orange);
            box-shadow: 0 0 15px rgba(249, 115, 22, 0.4);
        }
        .btn-active-on {
            background: var(--accent-green) !important;
            border-color: var(--accent-green);
            box-shadow: 0 0 15px rgba(16, 185, 129, 0.4);
        }
        
        .temp-display-container {
            display: flex;
            align-items: center;
            justify-content: space-between;
            background: rgba(0, 0, 0, 0.2);
            padding: 15px 20px;
            border-radius: 15px;
            margin-bottom: 20px;
        }
        .temp-val {
            font-size: 2.25rem;
            font-weight: 700;
        }
        .temp-btn {
            width: 44px;
            height: 44px;
            border-radius: 50%;
            background: rgba(255, 255, 255, 0.08);
            border: 1px solid var(--card-border);
            color: var(--text-color);
            font-size: 1.5rem;
            font-weight: 600;
            cursor: pointer;
            display: flex;
            align-items: center;
            justify-content: center;
            transition: all 0.2s ease;
        }
        .temp-btn:hover {
            background: rgba(255, 255, 255, 0.15);
            transform: scale(1.1);
        }

        .switch-container {
            display: flex;
            justify-content: space-between;
            align-items: center;
            background: rgba(255, 255, 255, 0.02);
            padding: 15px;
            border-radius: 12px;
            border: 1px solid rgba(255, 255, 255, 0.03);
        }
        .switch-title {
            font-size: 0.95rem;
            font-weight: 500;
        }
        .switch-subtitle {
            font-size: 0.8rem;
            color: var(--text-muted);
        }
        .toggle-switch {
            position: relative;
            display: inline-block;
            width: 50px;
            height: 28px;
        }
        .toggle-switch input {
            opacity: 0;
            width: 0;
            height: 0;
        }
        .slider {
            position: absolute;
            cursor: pointer;
            top: 0; left: 0; right: 0; bottom: 0;
            background-color: rgba(255, 255, 255, 0.1);
            transition: .3s;
            border-radius: 34px;
            border: 1px solid var(--card-border);
        }
        .slider:before {
            position: absolute;
            content: "";
            height: 20px; width: 20px;
            left: 3px; bottom: 3px;
            background-color: var(--text-color);
            transition: .3s;
            border-radius: 50%;
        }
        input:checked + .slider {
            background-color: var(--accent-blue);
        }
        input:checked + .slider:before {
            transform: translateX(22px);
        }
        
        .slider-control {
            width: 100%;
            -webkit-appearance: none;
            background: rgba(255, 255, 255, 0.1);
            height: 8px;
            border-radius: 5px;
            outline: none;
            margin: 15px 0;
        }
        .slider-control::-webkit-slider-thumb {
            -webkit-appearance: none;
            appearance: none;
            width: 20px; height: 20px;
            border-radius: 50%;
            background: var(--accent-blue);
            cursor: pointer;
            box-shadow: 0 0 10px rgba(59, 130, 246, 0.5);
            transition: transform 0.1s;
        }
        .slider-control::-webkit-slider-thumb:hover {
            transform: scale(1.2);
        }
    </style>
</head>
<body>
    <header>
        <h1>Smart Dashboard</h1>
        <p>fujitsu-ac.local • Room Controls</p>
    </header>
    
    <div class="container">
        <!-- AC CARD -->
        <div class="card">
            <div class="card-header">
                <div class="card-title">Fujitsu A/C</div>
                <div id="ac-badge" class="status-badge status-off">OFF</div>
            </div>
            
            <div class="control-group">
                <div class="control-label">Target Temperature</div>
                <div class="temp-display-container">
                    <button class="temp-btn" onclick="adjustTemp(-1)">-</button>
                    <div class="temp-val"><span id="ac-temp">24</span>°C</div>
                    <button class="temp-btn" onclick="adjustTemp(1)">+</button>
                </div>
            </div>
            
            <div class="control-group">
                <div class="control-label">A/C Mode</div>
                <div class="btn-group">
                    <button id="btn-mode-off" class="btn" onclick="setAcMode(0)">Off</button>
                    <button id="btn-mode-cool" class="btn" onclick="setAcMode(2)">Cool</button>
                    <button id="btn-mode-heat" class="btn" onclick="setAcMode(1)">Heat</button>
                </div>
            </div>
            
            <div class="switch-container">
                <div>
                    <div class="switch-title">Powerful Mode</div>
                    <div class="switch-subtitle">Runs max power for 20 mins</div>
                </div>
                <label class="toggle-switch">
                    <input type="checkbox" id="ac-powerful" onchange="togglePowerful()">
                    <span class="slider"></span>
                </label>
            </div>
        </div>
        
        <!-- LIGHTS CARD -->
        <div class="card">
            <div class="card-header">
                <div class="card-title">Bedroom Lights</div>
                <div id="light-badge" class="status-badge status-off">OFF</div>
            </div>
            
            <div class="control-group" style="display: flex; flex-direction: column; gap: 15px;">
                <div class="switch-container">
                    <div>
                        <div class="switch-title">Normal Light</div>
                        <div class="switch-subtitle">Full brightness natural light</div>
                    </div>
                    <label class="toggle-switch">
                        <input type="checkbox" id="light-normal-switch" onchange="toggleNormalLight()">
                        <span class="slider"></span>
                    </label>
                </div>
                
                <div class="switch-container">
                    <div>
                        <div class="switch-title">Night Light</div>
                        <div class="switch-subtitle">Dim warm sleep light</div>
                    </div>
                    <label class="toggle-switch">
                        <input type="checkbox" id="light-night-switch" onchange="toggleNightLight()">
                        <span class="slider"></span>
                    </label>
                </div>
            </div>
            
            <!-- Sensor Calibration Section -->
            <div class="control-group" style="margin-top: 20px; border-top: 1px solid rgba(255, 255, 255, 0.05); padding-top: 15px;">
                <div class="control-label">Sensor Calibration</div>
                <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-bottom: 10px;">
                    <div style="background: rgba(0,0,0,0.2); padding: 8px 12px; border-radius: 8px; text-align: center;">
                        <div style="font-size: 0.75rem; color: var(--text-muted);">Left ADC</div>
                        <div id="sensor-left" style="font-size: 1.1rem; font-weight: 600;">-</div>
                    </div>
                    <div style="background: rgba(0,0,0,0.2); padding: 8px 12px; border-radius: 8px; text-align: center;">
                        <div style="font-size: 0.75rem; color: var(--text-muted);">Right ADC</div>
                        <div id="sensor-right" style="font-size: 1.1rem; font-weight: 600;">-</div>
                    </div>
                </div>
                <div style="background: rgba(255, 255, 255, 0.02); border: 1px solid rgba(255, 255, 255, 0.05); padding: 12px; border-radius: 12px; font-size: 0.85rem; display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px;">
                    <div>
                        <div>Average: <strong id="sensor-avg">-</strong></div>
                        <div style="font-size: 0.75rem; color: var(--text-muted); line-height: 1.4; margin-top: 4px;">
                            Normal Thresh: <span id="sensor-threshold">-</span><br>
                            Night Thresh: <span id="sensor-threshold-night">-</span>
                        </div>
                    </div>
                    <div id="sensor-physical-state" class="status-badge status-off">DETECTION: -</div>
                </div>
                
                <div style="display: flex; flex-direction: column; gap: 10px; background: rgba(0,0,0,0.1); padding: 12px; border-radius: 10px; margin-bottom: 10px;">
                    <div>
                        <div style="font-size: 0.75rem; color: var(--text-muted); margin-bottom: 4px;">Normal Light Threshold</div>
                        <div style="display: flex; gap: 10px; align-items: center;">
                            <input type="range" id="threshold-slider" min="0" max="1023" step="5" style="flex-grow: 1; accent-color: var(--accent-blue);" oninput="document.getElementById('slider-val').innerText = this.value">
                            <div style="font-size: 0.85rem; min-width: 35px; text-align: right; font-weight: 600;"><span id="slider-val">350</span></div>
                        </div>
                    </div>
                    <div>
                        <div style="font-size: 0.75rem; color: var(--text-muted); margin-bottom: 4px;">Night Light Threshold</div>
                        <div style="display: flex; gap: 10px; align-items: center;">
                            <input type="range" id="threshold-night-slider" min="0" max="1023" step="5" style="flex-grow: 1; accent-color: var(--accent-orange);" oninput="document.getElementById('slider-night-val').innerText = this.value">
                            <div style="font-size: 0.85rem; min-width: 35px; text-align: right; font-weight: 600;"><span id="slider-night-val">100</span></div>
                        </div>
                    </div>
                </div>
                <button class="btn" style="width: 100%; padding: 8px; font-size: 0.85rem; border-radius: 8px; background: var(--accent-blue);" onclick="saveThresholds()">Save Calibration</button>
            </div>
        </div>
        
        <!-- TV CONTROL CARD -->
        <div class="card">
            <div class="card-header">
                <div class="card-title">TV Control</div>
                <div id="tv-badge" class="status-badge status-off">UNCONFIGURED</div>
            </div>
            
            <div class="control-group" style="display: flex; flex-direction: column; gap: 15px; flex-grow: 1; justify-content: center;">
                <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 15px;">
                    <button id="btn-tv-power" class="btn" style="background: rgba(239, 68, 68, 0.15); border-color: rgba(239, 68, 68, 0.3); color: #ef4444;" onclick="sendTvCommand('power')">
                        <span style="font-size: 1.2rem; margin-right: 8px;">⏻</span> Power
                    </button>
                    <button id="btn-tv-netflix" class="btn" style="background: rgba(229, 9, 20, 0.15); border-color: rgba(229, 9, 20, 0.3); color: #e50914;" onclick="sendTvCommand('netflix')">
                        Netflix
                    </button>
                </div>
                <div id="tv-status-summary" style="font-size: 0.80rem; color: var(--text-muted); background: rgba(0,0,0,0.2); padding: 12px; border-radius: 12px; border: 1px solid var(--card-border); margin-top: 10px; line-height: 1.6;">
                    <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 6px;">
                        <span>Power IR:</span>
                        <span id="tv-power-info" class="status-badge status-off" style="font-size: 0.75rem;">Unconfigured</span>
                    </div>
                    <div style="display: flex; justify-content: space-between; align-items: center;">
                        <span>Netflix IR:</span>
                        <span id="tv-netflix-info" class="status-badge status-off" style="font-size: 0.75rem;">Unconfigured</span>
                    </div>
                </div>
            </div>
        </div>

        <!-- IR LEARNING CARD -->
        <div class="card">
            <div class="card-header">
                <div class="card-title">IR Receiver (GP3)</div>
                <div id="ir-rx-badge" class="status-badge status-cool">READY</div>
            </div>
            
            <div class="control-group" style="display: flex; flex-direction: column; gap: 15px; flex-grow: 1;">
                <div style="background: rgba(0, 0, 0, 0.25); padding: 15px; border-radius: 15px; border: 1px solid var(--card-border); font-family: monospace; font-size: 0.85rem; line-height: 1.6; min-height: 110px; display: flex; flex-direction: column; justify-content: center;">
                    <div id="ir-no-code" style="text-align: center; color: var(--text-muted); font-style: italic;">
                        Point TV remote at GP3 receiver and press a button...
                    </div>
                    <div id="ir-code-details" style="display: none;">
                        <div style="margin-bottom: 4px;">Protocol: <strong id="ir-val-protocol" style="color: var(--accent-orange);">-</strong></div>
                        <div style="margin-bottom: 4px;">Address: <strong id="ir-val-address" style="color: var(--accent-blue);">-</strong></div>
                        <div style="margin-bottom: 4px;">Command: <strong id="ir-val-command" style="color: var(--accent-green);">-</strong></div>
                        <div style="margin-bottom: 4px;">Raw Data: <strong id="ir-val-raw" style="color: #a855f7;">-</strong></div>
                        <div style="margin-bottom: 4px;">Bits: <strong id="ir-val-bits" style="color: #ec4899;">-</strong></div>
                        <div style="font-size: 0.75rem; color: var(--text-muted); margin-top: 8px; border-top: 1px solid rgba(255,255,255,0.05); padding-top: 4px;">Captured: <span id="ir-val-time">-</span></div>
                    </div>
                </div>
                
                <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 10px;">
                    <button id="btn-assign-power" class="btn" style="font-size: 0.85rem; padding: 10px;" onclick="assignIrCode('power')" disabled>Assign to Power</button>
                    <button id="btn-assign-netflix" class="btn" style="font-size: 0.85rem; padding: 10px;" onclick="assignIrCode('netflix')" disabled>Assign to Netflix</button>
                </div>
            </div>
        </div>
    </div>

    <script>
        let acTargetTemp = 24;
        let acState = 0;
        let acPowerful = false;
        let normalLightState = false;
        let nightLightState = false;
        let hasSetSlider = false;
        let lastIrTimestamp = 0;

        async function init() {
            await syncAcState();
            await syncLightState();
            await syncTvStatus();
            await syncSensorData();
            
            setInterval(syncSensorData, 2000);
            setInterval(pollIrReceiver, 1000);
        }

        async function syncSensorData() {
            try {
                let res = await fetch('/light/sensor');
                let data = await res.json();
                
                document.getElementById('sensor-left').innerText = data.left;
                document.getElementById('sensor-right').innerText = data.right;
                document.getElementById('sensor-avg').innerText = data.average;
                document.getElementById('sensor-threshold').innerText = data.threshold;
                document.getElementById('sensor-threshold-night').innerText = data.nightThreshold;
                
                if (!hasSetSlider) {
                    document.getElementById('threshold-slider').value = data.threshold;
                    document.getElementById('slider-val').innerText = data.threshold;
                    document.getElementById('threshold-night-slider').value = data.nightThreshold;
                    document.getElementById('slider-night-val').innerText = data.nightThreshold;
                    hasSetSlider = true;
                }
                
                const physBadge = document.getElementById('sensor-physical-state');
                if (data.physicallyNormal) {
                    physBadge.innerText = 'LIT (NORMAL)';
                    physBadge.className = 'status-badge status-on';
                } else if (data.physicallyNight) {
                    physBadge.innerText = 'LIT (NIGHT)';
                    physBadge.className = 'status-badge status-heat';
                } else {
                    physBadge.innerText = 'DARK';
                    physBadge.className = 'status-badge status-off';
                }
            } catch(e) { console.error("Error syncing sensor data", e); }
        }

        async function saveThresholds() {
            const normalVal = document.getElementById('threshold-slider').value;
            const nightVal = document.getElementById('threshold-night-slider').value;
            try {
                let res = await fetch('/light/threshold?normal=' + normalVal + '&night=' + nightVal);
                let txt = await res.text();
                console.log("Thresholds save:", txt);
                hasSetSlider = false; // Force slider reset on next sync
                await syncSensorData();
                alert("Calibration thresholds saved successfully to EEPROM!");
            } catch(e) {
                console.error("Error saving thresholds", e);
                alert("Failed to save calibration.");
            }
        }

        async function syncAcState() {
            try {
                let res = await fetch('/status');
                let data = await res.json();
                
                acTargetTemp = Math.round(data.targetTemperature);
                acState = data.targetHeatingCoolingState;
                
                document.getElementById('ac-temp').innerText = acTargetTemp;
                
                document.getElementById('btn-mode-off').classList.toggle('btn-active-on', acState === 0);
                document.getElementById('btn-mode-cool').classList.toggle('btn-active-cool', acState === 2);
                document.getElementById('btn-mode-heat').classList.toggle('btn-active-heat', acState === 1);
                
                const badge = document.getElementById('ac-badge');
                if (acState === 0) {
                    badge.innerText = 'OFF';
                    badge.className = 'status-badge status-off';
                } else if (acState === 2) {
                    badge.innerText = 'COOL';
                    badge.className = 'status-badge status-cool';
                } else if (acState === 1) {
                    badge.innerText = 'HEAT';
                    badge.className = 'status-badge status-heat';
                }

                let powRes = await fetch('/powerfulStatus');
                let powVal = await powRes.text();
                acPowerful = (powVal.trim() === "1");
                document.getElementById('ac-powerful').checked = acPowerful;
            } catch(e) { console.error("Error syncing A/C state", e); }
        }

        async function syncLightState() {
            try {
                let res = await fetch('/light/status');
                let data = await res.json();
                
                normalLightState = (data.normal === 1);
                nightLightState = (data.night === 1);
                
                document.getElementById('light-normal-switch').checked = normalLightState;
                document.getElementById('light-night-switch').checked = nightLightState;
                
                const badge = document.getElementById('light-badge');
                if (normalLightState) {
                    badge.innerText = 'NORMAL';
                    badge.className = 'status-badge status-on';
                } else if (nightLightState) {
                    badge.innerText = 'NIGHT';
                    badge.className = 'status-badge status-heat';
                } else {
                    badge.innerText = 'OFF';
                    badge.className = 'status-badge status-off';
                }
            } catch(e) { console.error("Error syncing Lights state", e); }
        }

        async function syncTvStatus() {
            try {
                let res = await fetch('/tv/status');
                let data = await res.json();
                
                let powerConfigured = data.power.configured;
                let netflixConfigured = data.netflix.configured;
                
                const powerBadge = document.getElementById('tv-power-info');
                if (powerConfigured) {
                    let displayInfo = data.power.protocol;
                    if (data.power.protocol === "PulseDistance") {
                        displayInfo += " (Raw: " + data.power.raw + ")";
                    } else {
                        displayInfo += " (Addr: " + data.power.address + ", Cmd: " + data.power.command + ")";
                    }
                    powerBadge.innerText = displayInfo;
                    powerBadge.className = "status-badge status-on";
                    document.getElementById('btn-tv-power').disabled = false;
                    document.getElementById('btn-tv-power').style.opacity = 1;
                } else {
                    powerBadge.innerText = "Unconfigured";
                    powerBadge.className = "status-badge status-off";
                    document.getElementById('btn-tv-power').disabled = true;
                    document.getElementById('btn-tv-power').style.opacity = 0.5;
                }
                
                const netflixBadge = document.getElementById('tv-netflix-info');
                if (netflixConfigured) {
                    let displayInfo = data.netflix.protocol;
                    if (data.netflix.protocol === "PulseDistance") {
                        displayInfo += " (Raw: " + data.netflix.raw + ")";
                    } else {
                        displayInfo += " (Addr: " + data.netflix.address + ", Cmd: " + data.netflix.command + ")";
                    }
                    netflixBadge.innerText = displayInfo;
                    netflixBadge.className = "status-badge status-on";
                    document.getElementById('btn-tv-netflix').disabled = false;
                    document.getElementById('btn-tv-netflix').style.opacity = 1;
                } else {
                    netflixBadge.innerText = "Unconfigured";
                    netflixBadge.className = "status-badge status-off";
                    document.getElementById('btn-tv-netflix').disabled = true;
                    document.getElementById('btn-tv-netflix').style.opacity = 0.5;
                }

                const tvBadge = document.getElementById('tv-badge');
                if (powerConfigured && netflixConfigured) {
                    tvBadge.innerText = "CONFIGURED";
                    tvBadge.className = "status-badge status-on";
                } else if (powerConfigured || netflixConfigured) {
                    tvBadge.innerText = "PARTIAL";
                    tvBadge.className = "status-badge status-heat";
                } else {
                    tvBadge.innerText = "UNCONFIGURED";
                    tvBadge.className = "status-badge status-off";
                }
            } catch(e) { console.error("Error syncing TV status", e); }
        }

        async function pollIrReceiver() {
            try {
                let res = await fetch('/ir/last');
                let data = await res.json();
                
                if (data.received) {
                    if (data.timestamp !== lastIrTimestamp) {
                        lastIrTimestamp = data.timestamp;
                        
                        document.getElementById('ir-no-code').style.display = 'none';
                        document.getElementById('ir-code-details').style.display = 'block';
                        
                        document.getElementById('ir-val-protocol').innerText = data.protocol;
                        document.getElementById('ir-val-address').innerText = data.address;
                        document.getElementById('ir-val-command').innerText = data.command;
                        document.getElementById('ir-val-raw').innerText = data.raw;
                        document.getElementById('ir-val-bits').innerText = data.bits;
                        
                        let timeStr = new Date().toLocaleTimeString();
                        document.getElementById('ir-val-time').innerText = timeStr;
                        
                        document.getElementById('btn-assign-power').disabled = false;
                        document.getElementById('btn-assign-netflix').disabled = false;
                        
                        const rxBadge = document.getElementById('ir-rx-badge');
                        rxBadge.innerText = 'NEW CODE!';
                        rxBadge.className = 'status-badge status-on';
                        setTimeout(() => {
                            rxBadge.innerText = 'READY';
                            rxBadge.className = 'status-badge status-cool';
                        }, 2000);
                    }
                }
            } catch(e) { console.error("Error polling IR receiver", e); }
        }

        async function sendTvCommand(btn) {
            try {
                let res = await fetch('/tv/' + btn);
                if (!res.ok) {
                    let txt = await res.text();
                    alert(txt);
                }
            } catch(e) { console.error("Error sending TV command", e); }
        }

        async function assignIrCode(btn) {
            try {
                let res = await fetch('/tv/assign?button=' + btn);
                let txt = await res.text();
                console.log("Assign IR:", txt);
                await syncTvStatus();
                alert("Assigned IR code to TV " + btn + "!");
            } catch(e) {
                console.error("Error assigning IR code", e);
                alert("Failed to assign IR code.");
            }
        }

        async function setAcMode(mode) {
            await fetch('/targetHeatingCoolingState?value=' + mode);
            await syncAcState();
        }

        async function adjustTemp(dir) {
            let nextTemp = acTargetTemp + dir;
            if (nextTemp < 18) nextTemp = 18;
            if (nextTemp > 30) nextTemp = 30;
            await fetch('/targetTemperature?value=' + nextTemp);
            await syncAcState();
        }

        async function togglePowerful() {
            const chk = document.getElementById('ac-powerful');
            let val = chk.checked ? 1 : 0;
            await fetch('/powerful?value=' + val);
            await syncAcState();
        }

        async function toggleNormalLight() {
            const chk = document.getElementById('light-normal-switch');
            let val = chk.checked ? 1 : 0;
            await fetch('/light/normal?value=' + val);
            await syncLightState();
        }

        async function toggleNightLight() {
            const chk = document.getElementById('light-night-switch');
            let val = chk.checked ? 1 : 0;
            await fetch('/light/night?value=' + val);
            await syncLightState();
        }

        init();
    </script>
</body>
</html>
)rawliteral";

void handleDashboard() {
  server.send_P(200, "text/html", DASHBOARD_HTML);
}



void setup() {
  Serial.begin(115200);
  
  // Load EEPROM settings
  loadSettings();
  loadTVButtons();
  
  // WiFi Connection
  pinMode(LED_BUILTIN, OUTPUT);
  
  // Light Sensors
  pinMode(LIGHT_SENSOR_PIN_L, INPUT);
  pinMode(LIGHT_SENSOR_PIN_R, INPUT);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED_BUILTIN, HIGH);
    delay(250);
    digitalWrite(LED_BUILTIN, LOW);
    delay(250);
    Serial.print(".");
  }
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.print("\nConnected! IP: ");
  Serial.println(WiFi.localIP());

  // Set up OTA with custom hostname
  ArduinoOTA.setHostname("fujitsu-ac");
  ArduinoOTA.onStart([]() {
    Serial.println("OTA Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA End");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  Serial.println("OTA Service started");
  Serial.println("mDNS responder started: http://fujitsu-ac.local");

  server.on("/", handleDashboard);
  server.on("/status", handleRoot);
  server.on("/targetTemperature", handleTargetTemp);
  server.on("/targetHeatingCoolingState", handleTargetState);
  server.on("/powerful", handlePowerful);
  server.on("/powerfulStatus", handlePowerfulStatus);
  server.on("/light/status", handleLightStatus);
  server.on("/light/normal", handleLightNormal);
  server.on("/light/normalStatus", handleLightNormalStatus);
  server.on("/light/night", handleLightNight);
  server.on("/light/nightStatus", handleLightNightStatus);
  
  server.on("/light/sensor", HTTP_GET, []() {
    int left = analogRead(LIGHT_SENSOR_PIN_L);
    int right = analogRead(LIGHT_SENSOR_PIN_R);
    int avg = (left + right) / 2;
    String json = "{";
    json += "\"left\":" + String(left) + ",";
    json += "\"right\":" + String(right) + ",";
    json += "\"average\":" + String(avg) + ",";
    json += "\"threshold\":" + String(lightOnThreshold) + ",";
    json += "\"nightThreshold\":" + String(lightNightThreshold) + ",";
    json += "\"physicallyOn\":" + (avg >= lightNightThreshold ? String("true") : String("false")) + ",";
    json += "\"physicallyNormal\":" + (avg >= lightOnThreshold ? String("true") : String("false")) + ",";
    json += "\"physicallyNight\":" + ((avg >= lightNightThreshold && avg < lightOnThreshold) ? String("true") : String("false"));
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/light/threshold", HTTP_GET, []() {
    bool updated = false;
    if (server.hasArg("normal")) {
      int val = server.arg("normal").toInt();
      if (val >= 0 && val <= 1023) {
        lightOnThreshold = val;
        updated = true;
      }
    }
    if (server.hasArg("night")) {
      int val = server.arg("night").toInt();
      if (val >= 0 && val <= 1023) {
        lightNightThreshold = val;
        updated = true;
      }
    }
    if (server.hasArg("value")) {
      int val = server.arg("value").toInt();
      if (val >= 0 && val <= 1023) {
        lightOnThreshold = val;
        updated = true;
      }
    }
    
    if (updated) {
      saveSettings();
      server.send(200, "text/plain", "Thresholds updated");
    } else {
      server.send(400, "text/plain", "Invalid or missing parameters");
    }
  });
  
  server.onNotFound([]() {
    Serial.printf("404: %s\n", server.uri().c_str());
    server.send(404, "text/plain", "Not Found");
  });

  server.on("/raw", HTTP_GET, []() {
    if (!server.hasArg("hex")) {
      server.send(400, "text/plain", "Missing 'hex' argument");
      return;
    }
    String hex = server.arg("hex");
    hex.replace(" ", ""); // Remove spaces
    int len = hex.length() / 2;
    if (len > 32) len = 32; // Safety cap
    
    uint8_t data[32];
    for (int i = 0; i < len; i++) {
      String byteStr = hex.substring(i * 2, i * 2 + 2);
      data[i] = (uint8_t)strtol(byteStr.c_str(), NULL, 16);
    }

    sendFujitsuRaw(data, len);
    server.send(200, "text/plain", "Sent " + String(len) + "-byte Raw IR (manual mark/space)");
    Serial.println("IR: Sent Raw HEX Replay: " + hex);
  });

  // Self-test: send the EXACT bytes captured from the user's AR-RFL5J remote
  server.on("/selftest", HTTP_GET, []() {
    String test = server.hasArg("cmd") ? server.arg("cmd") : "cool_on";
    
    if (test == "off") {
      // Power OFF (7 bytes) - CONFIRMED WORKING
      uint8_t off[] = {0x14, 0x63, 0x00, 0x10, 0x10, 0x02, 0xFD};
      sendFujitsuRaw(off, 7);
      server.send(200, "text/plain", "Sent: Power OFF (7B)");
    } else {
      // Cool ON at 24C (18 bytes) - EXACT CAPTURE from AR-RFL5J remote
      uint8_t cool_on[] = {0x14, 0x63, 0x00, 0x10, 0x10, 0xFE, 0x0B, 0x01, 0xE2, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x08, 0x6E};
      sendFujitsuRaw(cool_on, 18);
      server.send(200, "text/plain", "Sent: Cool ON 24C (18B) - exact remote capture");
    }
  });

  server.on("/ir/last", HTTP_GET, []() {
    String json = "{";
    json += "\"received\":" + String(lastRxValid ? "true" : "false") + ",";
    json += "\"protocol\":\"" + lastRxProtocol + "\",";
    json += "\"protocolId\":" + String(lastRxProtocolId) + ",";
    json += "\"address\":\"0x" + String(lastRxAddress, HEX) + "\",";
    json += "\"command\":\"0x" + String(lastRxCommand, HEX) + "\",";
    char rawHex[32];
    if (lastRxRawData > 0xFFFFFFFFULL) {
        sprintf(rawHex, "0x%X%08X", (uint32_t)(lastRxRawData >> 32), (uint32_t)(lastRxRawData & 0xFFFFFFFF));
    } else {
        sprintf(rawHex, "0x%X", (uint32_t)lastRxRawData);
    }
    json += "\"raw\":\"" + String(rawHex) + "\",";
    json += "\"bits\":" + String(lastRxBits) + ",";
    json += "\"timestamp\":" + String(lastRxTimestamp);
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/ir/dump", HTTP_GET, []() {
    if (!lastRxValid) {
      server.send(200, "text/plain", "No IR code received yet.");
      return;
    }
    
    String output = "IR Protocol: " + lastRxProtocol + "\n";
    output += "Bits: " + String(lastRxBits) + "\n";
    char rawHex[32];
    if (lastRxRawData > 0xFFFFFFFFULL) {
        sprintf(rawHex, "0x%X%08X", (uint32_t)(lastRxRawData >> 32), (uint32_t)(lastRxRawData & 0xFFFFFFFF));
    } else {
        sprintf(rawHex, "0x%X", (uint32_t)lastRxRawData);
    }
    output += "Raw Data: " + String(rawHex) + "\n\n";
    
    output += "Raw Timing Buffer (length " + String(IrReceiver.irparams.rawlen) + "):\n";
    for (size_t i = 1; i < IrReceiver.irparams.rawlen; i++) {
      uint32_t duration = IrReceiver.irparams.rawbuf[i] * 50; // Convert ticks to microseconds
      if (i % 2 == 1) {
        output += "Mark: " + String(duration) + " us\n";
      } else {
        output += "Space: " + String(duration) + " us\n";
      }
    }
    
    server.send(200, "text/plain", output);
  });

  server.on("/tv/assign", HTTP_GET, []() {
    if (!server.hasArg("button")) {
      server.send(400, "text/plain", "Missing 'button' parameter");
      return;
    }
    if (!lastRxValid || lastRxProtocolId == 0) {
      server.send(400, "text/plain", "No valid IR code received yet to assign");
      return;
    }
    String btn = server.arg("button");
    if (btn == "power") {
      tvPower.protocol = lastRxProtocolId;
      tvPower.address = lastRxAddress;
      tvPower.command = lastRxCommand;
      tvPower.rawData = lastRxRawData;
      saveTVButton(false);
    } else if (btn == "netflix") {
      tvNetflix.protocol = lastRxProtocolId;
      tvNetflix.address = lastRxAddress;
      tvNetflix.command = lastRxCommand;
      tvNetflix.rawData = lastRxRawData;
      saveTVButton(true);
    } else {
      server.send(400, "text/plain", "Invalid button (must be power or netflix)");
      return;
    }
    server.send(200, "text/plain", "Successfully assigned last received IR code to TV " + btn);
  });

  server.on("/tv/status", HTTP_GET, []() {
    String json = "{";
    json += "\"power\":{";
    json += "\"configured\":" + String(tvPower.protocol != 0 ? "true" : "false") + ",";
    json += "\"protocol\":\"" + String(getProtocolString((decode_type_t)tvPower.protocol)) + "\",";
    json += "\"address\":\"0x" + String(tvPower.address, HEX) + "\",";
    json += "\"command\":\"0x" + String(tvPower.command, HEX) + "\",";
    char rawHexPower[32];
    if (tvPower.rawData > 0xFFFFFFFFULL) {
        sprintf(rawHexPower, "0x%X%08X", (uint32_t)(tvPower.rawData >> 32), (uint32_t)(tvPower.rawData & 0xFFFFFFFF));
    } else {
        sprintf(rawHexPower, "0x%X", (uint32_t)tvPower.rawData);
    }
    json += "\"raw\":\"" + String(rawHexPower) + "\"";
    json += "},";
    json += "\"netflix\":{";
    json += "\"configured\":" + String(tvNetflix.protocol != 0 ? "true" : "false") + ",";
    json += "\"protocol\":\"" + String(getProtocolString((decode_type_t)tvNetflix.protocol)) + "\",";
    json += "\"address\":\"0x" + String(tvNetflix.address, HEX) + "\",";
    json += "\"command\":\"0x" + String(tvNetflix.command, HEX) + "\",";
    char rawHexNetflix[32];
    if (tvNetflix.rawData > 0xFFFFFFFFULL) {
        sprintf(rawHexNetflix, "0x%X%08X", (uint32_t)(tvNetflix.rawData >> 32), (uint32_t)(tvNetflix.rawData & 0xFFFFFFFF));
    } else {
        sprintf(rawHexNetflix, "0x%X", (uint32_t)tvNetflix.rawData);
    }
    json += "\"raw\":\"" + String(rawHexNetflix) + "\"";
    json += "}";
    json += "}";
    server.send(200, "application/json", json);
  });

  server.on("/tv/power", HTTP_GET, []() {
    if (tvPower.protocol == 0) {
      server.send(400, "text/plain", "TV Power button is not configured");
      return;
    }
    sendTVButton(tvPower);
    server.send(200, "text/plain", "OK");
  });

  server.on("/tv/netflix", HTTP_GET, []() {
    if (tvNetflix.protocol == 0) {
      server.send(400, "text/plain", "TV Netflix button is not configured");
      return;
    }
    sendTVButton(tvNetflix);
    server.send(200, "text/plain", "OK");
  });

  server.begin();

  // IR Sender initialization
  IrSender.begin(IR_SEND_PIN, DISABLE_LED_FEEDBACK);
  pinMode(IR_SEND_PIN, OUTPUT_12MA); // Boost drive strength from default 4mA to 12mA
  Serial.println("IR: IRremote Sender on Pin " + String(IR_SEND_PIN) + " boosted to 12mA");
  
  Serial.println("Web Server started");
}

void loop() {
  server.handleClient();
  MDNS.update();
  ArduinoOTA.handle();
}

void setup1() {
  // IR Receiver initialization on Core 1
  IrReceiver.begin(IR_RECEIVE_PIN, DISABLE_LED_FEEDBACK);
  pinMode(IR_RECEIVE_PIN, INPUT_PULLUP); // Enable internal pull-up to prevent noise
  Serial.println("IR: IRremote Receiver on Pin " + String(IR_RECEIVE_PIN) + " initialized on Core 1");
}

void loop1() {
  // Handle IR receiver decoding on Core 1
  if (IrReceiver.decode()) {
    // Only capture standard protocol signals that aren't repeats, or generic PulseDistance
    if (IrReceiver.decodedIRData.protocol != UNKNOWN && !(IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT)) {
      lastRxProtocol = getProtocolString(IrReceiver.decodedIRData.protocol);
      lastRxProtocolId = (uint8_t)IrReceiver.decodedIRData.protocol;
      lastRxAddress = IrReceiver.decodedIRData.address;
      lastRxCommand = IrReceiver.decodedIRData.command;
      lastRxRawData = IrReceiver.decodedIRData.decodedRawData;
      lastRxBits = IrReceiver.decodedIRData.numberOfBits;
      lastRxTimestamp = millis();
      lastRxValid = true;
      
      Serial.print("IR RX (Core 1): Protocol=");
      Serial.print(lastRxProtocol);
      Serial.printf("(%d), Address=0x%04X, Command=0x%08X, Raw=0x", lastRxProtocolId, lastRxAddress, lastRxCommand);
      if (lastRxRawData > 0xFFFFFFFFULL) {
        Serial.print((uint32_t)(lastRxRawData >> 32), HEX);
        Serial.printf("%08X", (uint32_t)(lastRxRawData & 0xFFFFFFFF));
      } else {
        Serial.print((uint32_t)lastRxRawData, HEX);
      }
      Serial.printf(", Bits=%d\n", lastRxBits);
    }
    IrReceiver.resume();
  }
  delay(1); // Yield to prevent Core 1 from hogging the CPU bus
}
