#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_NeoPixel.h>
#include <LEAmDNS.h> // mDNS support for Pico W
#define RAW_BUFFER_LENGTH 750 // For air condition remotes (144+ bits)
#include <IRremote.hpp> // Use IRremote for hardware pulsing
#include "RP2040_PWM.h"
#include "config.h"

// Hardware Objects
Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
RP2040_PWM* servo;
WebServer server(WEB_PORT);

// State Variables
bool acPower = false;
int acTargetTemp = 24;
String acMode = "cool"; // cool, heat, off

// Physical hardware targets
float currentDuty = SERVO_OFF_DUTY;
float targetDuty = SERVO_OFF_DUTY;
unsigned long lastServoUpdate = 0;

// Raw IR capture storage (for capture→replay from real remote)
uint16_t capturedRaw[600];  // Max 600 mark/space entries
uint16_t capturedLen = 0;
bool captureArmed = false;

void setup() {
  Serial.begin(115200);
  
  // Initialize LEDs
  pixels.begin();
  pixels.show(); // Off
  
  // Initialize Servo
  servo = new RP2040_PWM(SERVO_PIN, 50.0f, 0);
  if (servo) {
    servo->setPWM(SERVO_PIN, 50.0f, currentDuty);
  }
  
  // WiFi Connection
  pinMode(LED_BUILTIN, OUTPUT);
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

  server.on("/", handleRoot);
  server.on("/status", handleRoot);
  server.on("/targetTemperature", handleTargetTemp);
  server.on("/targetHeatingCoolingState", handleTargetState);
  
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

    // Use manual mark/space sender (same as normal commands)
    pixels.setPixelColor(0, pixels.Color(255, 255, 0)); pixels.show();
    sendFujitsuRaw(data, len);
    pixels.setPixelColor(0, pixels.Color(0, 0, 0)); pixels.show();
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

  // Arm capture: next IR signal received will be stored
  server.on("/capture", HTTP_GET, []() {
    captureArmed = true;
    capturedLen = 0;
    server.send(200, "text/plain", "Capture armed. Point your remote at the receiver and press a button.");
    Serial.println("IR CAPTURE: Armed - waiting for signal...");
  });

  // Replay the last captured raw signal
  server.on("/replay", HTTP_GET, []() {
    if (capturedLen == 0) {
      server.send(400, "text/plain", "No signal captured yet. Hit /capture first, then press your remote.");
      return;
    }
    // Disable interference sources
    IrReceiver.stop();
    if (servo) servo->setPWM(SERVO_PIN, 50.0f, 0);
    
    IrSender.sendRaw(capturedRaw, capturedLen, 38);
    
    IrReceiver.start();
    if (servo) servo->setPWM(SERVO_PIN, 50.0f, currentDuty);
    
    server.send(200, "text/plain", "Replayed " + String(capturedLen) + " raw entries");
    Serial.println("IR REPLAY: Sent " + String(capturedLen) + " raw entries");
  });

  // Show the last captured raw timings + decoded hex bytes
  server.on("/captured", HTTP_GET, []() {
    if (capturedLen == 0) {
      server.send(200, "text/plain", "No signal captured yet.");
      return;
    }
    String resp = "Captured " + String(capturedLen) + " entries:\n";
    
    // Print raw timings
    for (uint16_t i = 0; i < capturedLen; i++) {
      resp += String(capturedRaw[i]);
      if (i < capturedLen - 1) resp += ",";
      if (i % 16 == 15) resp += "\n";
    }
    resp += "\n\n";
    
    // Decode to hex bytes (skip header mark[0]+space[1], then pairs of mark+space)
    resp += "Decoded bytes (LSB first): ";
    uint8_t byteVal = 0;
    int bitCount = 0;
    int byteCount = 0;
    // Data starts at index 2 (after header mark+space)
    // Each bit = mark(even) + space(odd). Space > 800us = 1, else = 0
    for (uint16_t i = 2; i < capturedLen - 1; i += 2) {
      uint16_t spaceVal = capturedRaw[i + 1];  // space is at odd index
      if (spaceVal > 800) {
        byteVal |= (1 << bitCount);
      }
      bitCount++;
      if (bitCount == 8) {
        char hex[4];
        sprintf(hex, "%02X ", byteVal);
        resp += hex;
        byteVal = 0;
        bitCount = 0;
        byteCount++;
      }
    }
    if (bitCount > 0) {
      char hex[4];
      sprintf(hex, "%02X ", byteVal);
      resp += hex;
      byteCount++;
    }
    resp += "\nTotal bytes: " + String(byteCount);
    
    server.send(200, "text/plain", resp);
  });

  server.begin();
  
  // mDNS initialization
  if (MDNS.begin("fujitsu-ac")) {
    Serial.println("mDNS responder started: http://fujitsu-ac.local");
  }

  // IR Sender initialization
  IrSender.begin(IR_SEND_PIN, DISABLE_LED_FEEDBACK);
  
  // IR Receiver initialization
  IrReceiver.begin(IR_RECV_PIN, ENABLE_LED_FEEDBACK);
  
  Serial.println("IR: IRremote Sender on Pin " + String(IR_SEND_PIN));
  Serial.println("IR: IRremote Receiver on Pin " + String(IR_RECV_PIN));

  Serial.println("Web Server started");
}

void loop() {
  server.handleClient();
  MDNS.update();
  
  // Handle IR Receive (Sniffer mode)
  if (IrReceiver.decode()) {
    // If capture is armed, store raw timings for replay
    if (captureArmed) {
      capturedLen = 0;
      // rawbuf[0] is the gap, skip it. Copy from [1] to [rawlen-1]
      uint16_t rawlen = IrReceiver.irparams.rawlen;
      if (rawlen > 601) rawlen = 601;  // cap to buffer size + 1
      for (uint16_t i = 1; i < rawlen; i++) {
        capturedRaw[capturedLen++] = IrReceiver.irparams.rawbuf[i] * MICROS_PER_TICK;
      }
      captureArmed = false;
      Serial.printf("IR CAPTURE: Stored %d raw entries (from %d rawlen)\n", capturedLen, rawlen);
      
      // Also print the timings to Serial
      Serial.print("Raw timings: ");
      for (uint16_t i = 0; i < capturedLen; i++) {
        Serial.print(capturedRaw[i]);
        if (i < capturedLen - 1) Serial.print(",");
      }
      Serial.println();
    }
    
    Serial.println("--- IR Signal Captured ---");
    Serial.print("Protocol: "); Serial.println(IrReceiver.getProtocolString());
    Serial.print("Bits: "); Serial.println(IrReceiver.decodedIRData.numberOfBits);
    
    // Hex Dump of Bytes (LSB First decoding)
    Serial.print("Data (Hex): ");
    uint8_t byteVal = 0;
    int bitCount = 0;
    
    // v4.x API: irparams.rawbuf[1]=HDR_MARK, [2]=HDR_SPACE, [3]=BIT0_MARK, [4]=BIT0_SPACE
    // We want the space of each bit to determine 0 or 1.
    for (int i = 0; i < IrReceiver.decodedIRData.numberOfBits; i++) {
        int bitSpaceIdx = (i * 2) + 4; 
        if (bitSpaceIdx < IrReceiver.irparams.rawlen) {
            uint16_t spaceLen = IrReceiver.irparams.rawbuf[bitSpaceIdx] * 50; 
            if (spaceLen > 800) { 
                byteVal |= (1 << bitCount);
            }
        }
        bitCount++;
        if (bitCount == 8) {
            if (byteVal < 0x10) Serial.print("0");
            Serial.print(byteVal, HEX);
            Serial.print(" ");
            byteVal = 0;
            bitCount = 0;
        }
    }
    if (bitCount > 0) {
        if (byteVal < 0x10) Serial.print("0");
        Serial.print(byteVal, HEX);
    }
    Serial.println();
    
    // Print summary to ensure we see the result even if timing list is long
    Serial.print("Summary: ");
    IrReceiver.printIRResultShort(&Serial);
    Serial.println();
    
    IrReceiver.printIRResultRawFormatted(&Serial, true);
    IrReceiver.resume(); 
    Serial.println("--------------------------");
  }

  // Handle NeoPixel Throttling (Don't update too fast, it kills WiFi)
  static unsigned long lastHwUpdate = 0;
  if (millis() - lastHwUpdate > 20) {
    lastHwUpdate = millis();
    updateHardware();
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
    sendIRCommand();
  }
  server.send(200, "text/plain", "OK");
}

// --- Logic Implementations ---

// --- Custom Fujitsu Protocol Sender (Manual Mark/Space) ---
// Timings from IRremoteESP8266's ir_Fujitsu.cpp (verified/stable)
#define FUJI_HDR_MARK   3324
#define FUJI_HDR_SPACE  1574
#define FUJI_BIT_MARK   448
#define FUJI_ONE_SPACE  1182
#define FUJI_ZERO_SPACE 390
#define FUJI_MIN_GAP    8100

// Low-level sender: builds raw timing array and uses IrSender.sendRaw().
// This is the most proven path in the IRremote library.
// Disables all potential interrupt sources for clean timing.
void sendFujitsuRaw(const uint8_t* data, uint16_t nbytes) {
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

    // Disable all interrupt sources for clean transmission
    IrReceiver.stop();
    if (servo) servo->setPWM(SERVO_PIN, 50.0f, 0);  // Stop servo PWM

    // Send using IRremote's built-in sendRaw
    IrSender.sendRaw(rawBuf, idx, 38);

    // Re-enable everything
    IrReceiver.start();
    if (servo) servo->setPWM(SERVO_PIN, 50.0f, currentDuty);  // Restore servo

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
      // Use the custom 144-bit sender for ON and Temperature updates
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

void updateHardware() {
  // 1. Servo Smooth Movement (follows acPower)
  targetDuty = acPower ? SERVO_ON_DUTY : SERVO_OFF_DUTY;
  
  if (abs(currentDuty - targetDuty) > 0.05) {
    if (currentDuty < targetDuty) currentDuty += 0.1;
    else currentDuty -= 0.1;
    
    if (servo) {
      servo->setPWM(SERVO_PIN, 50.0f, currentDuty);
    }
  }

  // 2. LED Color based on temperature
  static uint32_t lastColor = 1; // Start with something different than 0
  uint32_t color;
  if (!acPower) {
    color = pixels.Color(0, 0, 0); // Off
  } else if (acTargetTemp >= 26) {
    color = pixels.Color(100, 0, 0); // Red
  } else if (acTargetTemp <= 22) {
    color = pixels.Color(0, 0, 100); // Blue
  } else {
    color = pixels.Color(0, 100, 0); // Green
  }

  if (color != lastColor) {
    for(int i=0; i<NUM_LEDS; i++) {
      pixels.setPixelColor(i, color);
    }
    pixels.show();
    lastColor = color;
  }
}
