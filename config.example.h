#ifndef CONFIG_H
#define CONFIG_H

// ==========================================
//              WiFi Settings
// ==========================================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ==========================================
//            Hardware Settings
// ==========================================
// NeoPixel LED Strip
#define LED_PIN           16
#define NUM_LEDS          8

// Servo Motor
#define SERVO_PIN         13

// IR Transmitter & Receiver
#define IR_SEND_PIN       15  // GP15
#define IR_RECV_PIN       3   // GP3 for IR Receiver

// Fujitsu ID (Custom Code): 0=A, 1=B, 2=C, 3=D
// If AC doesn't respond, change this value (0-3) and re-flash.
#define FUJITSU_CUSTOM_CODE 0

// ==========================================
//              Servo Calibration
// ==========================================
#define SERVO_CENTER_DUTY 7.5
#define SERVO_ON_DUTY     10.8  // Position when AC is ON
#define SERVO_OFF_DUTY    4.2   // Position when AC is OFF

// ==========================================
//             Web Client Settings
// ==========================================
#define WEB_PORT          80

#endif
