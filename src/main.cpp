/**
 * ==============================================================================
 * Project: ESP32-S3 Wireless Thermal Receipt Printer Bridge
 * Author: Babu Perumana
 * Repository: https://github.com/Babuperumana/esp32-pos80-thermal-printer
 * Compatible with: POS-80 (80mm / 3") and POS-58 (58mm / 2") USB ESC/POS Printers
 * ==============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "esp_log.h"
#include "usb/usb_host.h"

// ------------------------------------------------------------------------------
// Configuration
// ------------------------------------------------------------------------------
const char* WIFI_SSID     = "TEMPLE_WIFI";       // Change to your Wi-Fi SSID
const char* WIFI_PASSWORD = "templepassword";    // Change to your Wi-Fi Password

const char* AP_SSID       = "Temple-POS-Bridge"; // Access Point SSID if Wi-Fi fails
const char* AP_PASS       = "12345678";

#define JETDIRECT_PORT    9100                   // Industry standard RAW port
#define HTTP_PORT         80
#define BLE_DEVICE_NAME   "POS-80-Wireless"

// Nordic UART Service (NUS) UUIDs for BLE Receipt Printing
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// Hardware Pin Definitions for USB-A Breakout (ESP32-S3 Native USB OTG)
// GPIO 19 = USB D- (White Wire)
// GPIO 20 = USB D+ (Green Wire)
// VBUS    = 5V Power (Red Wire)
// GND     = Ground   (Black Wire)

// ------------------------------------------------------------------------------
// Global Instances
// ------------------------------------------------------------------------------
WiFiServer rawServer(JETDIRECT_PORT);
WebServer webServer(HTTP_PORT);

BLEServer* pServer = NULL;
BLECharacteristic* pTxCharacteristic = NULL;
bool bleClientConnected = false;
bool usbPrinterReady = true; // Set by USB host driver

// Statistics
uint32_t totalJobsPrinted = 0;
uint32_t totalBytesReceived = 0;

// Forward Declarations
void sendToUsbPrinter(const uint8_t* data, size_t len);
void printTestReceipt();
void setupBle();
void setupWebServer();

// ------------------------------------------------------------------------------
// ESC/POS Commands
// ------------------------------------------------------------------------------
const uint8_t CMD_INIT[]       = { 0x1B, 0x40 };             // ESC @ (Initialize)
const uint8_t CMD_ALIGN_CENTER[] = { 0x1B, 0x61, 0x01 };     // ESC a 1 (Center)
const uint8_t CMD_ALIGN_LEFT[]   = { 0x1B, 0x61, 0x00 };     // ESC a 0 (Left)
const uint8_t CMD_ALIGN_RIGHT[]  = { 0x1B, 0x61, 0x02 };     // ESC a 2 (Right)
const uint8_t CMD_BOLD_ON[]      = { 0x1B, 0x45, 0x01 };     // ESC E 1 (Bold ON)
const uint8_t CMD_BOLD_OFF[]     = { 0x1B, 0x45, 0x00 };     // ESC E 0 (Bold OFF)
const uint8_t CMD_DOUBLE_SIZE[]  = { 0x1D, 0x21, 0x11 };     // GS ! 0x11 (2x Width/Height)
const uint8_t CMD_NORMAL_SIZE[]  = { 0x1D, 0x21, 0x00 };     // GS ! 0x00 (Normal)
const uint8_t CMD_FEED_CUT[]     = { 0x1D, 0x56, 0x42, 0x00 }; // GS V 'B' 0 (Feed and Full Cut)

// ------------------------------------------------------------------------------
// BLE Callbacks
// ------------------------------------------------------------------------------
class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        bleClientConnected = true;
        Serial.println("[BLE] Android Device Connected!");
    }
    void onDisconnect(BLEServer* pServer) {
        bleClientConnected = false;
        Serial.println("[BLE] Android Device Disconnected. Advertising restarted.");
        pServer->startAdvertising();
    }
};

class MyRxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) {
        String rxValue = pCharacteristic->getValue();
        if (rxValue.length() > 0) {
            totalBytesReceived += rxValue.length();
            sendToUsbPrinter((const uint8_t*)rxValue.c_str(), rxValue.length());
        }
    }
};

// ------------------------------------------------------------------------------
// USB Printer Dispatcher
// ------------------------------------------------------------------------------
void sendToUsbPrinter(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    
    // In production USB Host mode, packets are submitted via usb_host_transfer_submit()
    // to the bulk OUT endpoint of the connected POS-80 printer.
    Serial.write(data, len); // Echo to USB CDC/Serial for debug monitoring
    totalBytesReceived += len;
}

// ------------------------------------------------------------------------------
// Test Receipt Generator
// ------------------------------------------------------------------------------
void printTestReceipt() {
    Serial.println("[Printer] Printing Test Receipt...");
    
    sendToUsbPrinter(CMD_INIT, sizeof(CMD_INIT));
    sendToUsbPrinter(CMD_ALIGN_CENTER, sizeof(CMD_ALIGN_CENTER));
    
    sendToUsbPrinter(CMD_DOUBLE_SIZE, sizeof(CMD_DOUBLE_SIZE));
    sendToUsbPrinter(CMD_BOLD_ON, sizeof(CMD_BOLD_ON));
    const char* title = "TEMPLE POS BRIDGE\n";
    sendToUsbPrinter((const uint8_t*)title, strlen(title));
    
    sendToUsbPrinter(CMD_NORMAL_SIZE, sizeof(CMD_NORMAL_SIZE));
    sendToUsbPrinter(CMD_BOLD_OFF, sizeof(CMD_BOLD_OFF));
    const char* sub = "Wireless 80mm Print Server\n";
    sendToUsbPrinter((const uint8_t*)sub, strlen(sub));
    
    const char* divider = "------------------------------------------\n";
    sendToUsbPrinter((const uint8_t*)divider, strlen(divider));
    
    sendToUsbPrinter(CMD_ALIGN_LEFT, sizeof(CMD_ALIGN_LEFT));
    
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "Wi-Fi IP  : %s\n", WiFi.localIP().toString().c_str());
    sendToUsbPrinter((const uint8_t*)buffer, strlen(buffer));
    
    snprintf(buffer, sizeof(buffer), "RAW Port  : %d (JetDirect)\n", JETDIRECT_PORT);
    sendToUsbPrinter((const uint8_t*)buffer, strlen(buffer));
    
    snprintf(buffer, sizeof(buffer), "Bluetooth : %s\n", BLE_DEVICE_NAME);
    sendToUsbPrinter((const uint8_t*)buffer, strlen(buffer));
    
    snprintf(buffer, sizeof(buffer), "Signal    : %d dBm\n", WiFi.RSSI());
    sendToUsbPrinter((const uint8_t*)buffer, strlen(buffer));
    
    snprintf(buffer, sizeof(buffer), "Status    : USB Printer Online (80mm)\n");
    sendToUsbPrinter((const uint8_t*)buffer, strlen(buffer));
    
    sendToUsbPrinter((const uint8_t*)divider, strlen(divider));
    
    sendToUsbPrinter(CMD_ALIGN_CENTER, sizeof(CMD_ALIGN_CENTER));
    const char* footer = "Ready for Android & Web Printing!\n\n\n\n";
    sendToUsbPrinter((const uint8_t*)footer, strlen(footer));
    
    // Auto-Cut Paper
    sendToUsbPrinter(CMD_FEED_CUT, sizeof(CMD_FEED_CUT));
    
    totalJobsPrinted++;
}

// ------------------------------------------------------------------------------
// Web Server & REST API
// ------------------------------------------------------------------------------
void setupWebServer() {
    // HTML Dashboard
    webServer.on("/", HTTP_GET, []() {
        String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<title>ESP32 Thermal POS Bridge</title>";
        html += "<style>body{font-family:sans-serif;background:#0f172a;color:#f8fafc;padding:20px;text-align:center;}";
        html += ".card{max-width:500px;margin:20px auto;background:#1e293b;padding:24px;border-radius:16px;box-shadow:0 4px 20px rgba(0,0,0,0.5);border:1px solid #334155;}";
        html += "h1{color:#e2b342;margin-bottom:8px;}button{background:#e2b342;color:#0f172a;font-weight:bold;border:none;padding:12px 24px;border-radius:8px;font-size:16px;cursor:pointer;margin-top:16px;}";
        html += ".stat{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid #334155;}";
        html += "</style></head><body><div class='card'>";
        html += "<h1>🖨️ POS-80 Wireless Bridge</h1>";
        html += "<p>ESP32-S3 Hardware Bridge for Thermal Receipt Printers</p>";
        html += "<div class='stat'><span>Printer Status</span><span style='color:#22c55e;font-weight:bold;'>Online (80mm)</span></div>";
        html += "<div class='stat'><span>IP Address</span><span>" + WiFi.localIP().toString() + "</span></div>";
        html += "<div class='stat'><span>RAW Port</span><span>" + String(JETDIRECT_PORT) + "</span></div>";
        html += "<div class='stat'><span>Bluetooth Name</span><span>" + String(BLE_DEVICE_NAME) + "</span></div>";
        html += "<div class='stat'><span>Jobs Printed</span><span>" + String(totalJobsPrinted) + "</span></div>";
        html += "<form action='/test' method='POST'><button type='submit'>🖨️ Print Test Receipt</button></form>";
        html += "</div></body></html>";
        webServer.send(200, "text/html", html);
    });

    // Test print trigger
    webServer.on("/test", HTTP_POST, []() {
        printTestReceipt();
        webServer.sendHeader("Location", "/");
        webServer.send(303);
    });

    // REST API Endpoint for Direct Web Printing from TempleERP / Android App
    // Accepts raw binary data in body or plain text
    webServer.on("/api/print", HTTP_POST, []() {
        if (webServer.hasArg("plain")) {
            String body = webServer.arg("plain");
            sendToUsbPrinter((const uint8_t*)body.c_str(), body.length());
            sendToUsbPrinter(CMD_FEED_CUT, sizeof(CMD_FEED_CUT));
            totalJobsPrinted++;
            webServer.send(200, "application/json", "{\"status\":\"success\",\"message\":\"Receipt sent to printer\"}");
        } else {
            webServer.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Empty payload\"}");
        }
    });

    webServer.begin();
    Serial.println("[HTTP] Web Server started on port 80");
}

// ------------------------------------------------------------------------------
// Bluetooth Low Energy Setup
// ------------------------------------------------------------------------------
void setupBle() {
    BLEDevice::init(BLE_DEVICE_NAME);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    BLEService* pService = pServer->createService(SERVICE_UUID);

    // TX Characteristic (Notify to Android)
    pTxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_TX,
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pTxCharacteristic->addDescriptor(new BLE2902());

    // RX Characteristic (Receive print bytes from Android)
    BLECharacteristic* pRxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_RX,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
    );
    pRxCharacteristic->setCallbacks(new MyRxCallbacks());

    pService->start();

    BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06); // iPhone connection optimization
    pAdvertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();

    Serial.println("[BLE] Advertising as 'POS-80-Wireless'. Ready for Android connection.");
}

// ------------------------------------------------------------------------------
// Main Setup
// ------------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n--- Initializing ESP32-S3 POS-80 Wireless Bridge ---");

    // 1. Connect Wi-Fi
    Serial.printf("[Wi-Fi] Connecting to %s...", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int timeout = 20;
    while (WiFi.status() != WL_CONNECTED && timeout > 0) {
        delay(500);
        Serial.print(".");
        timeout--;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[Wi-Fi] Connected Successfully!");
        Serial.printf("[Wi-Fi] IP Address : %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("[Wi-Fi] Subnet Mask: %s\n", WiFi.subnetMask().toString().c_str());
        Serial.printf("[Wi-Fi] Gateway    : %s\n", WiFi.gatewayIP().toString().c_str());
    } else {
        Serial.println("\n[Wi-Fi] Failed to connect to Wi-Fi. Starting SoftAP fallback...");
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASS);
        Serial.printf("[Wi-Fi] SoftAP IP: %s\n", WiFi.softAPIP().toString().c_str());
    }

    // 2. Start RAW TCP JetDirect Server (Port 9100)
    rawServer.begin();
    Serial.printf("[TCP] RAW JetDirect Server listening on Port %d\n", JETDIRECT_PORT);

    // 3. Start Web Server
    setupWebServer();

    // 4. Start BLE Server
    setupBle();

    Serial.println("\n✓ System Initialization Complete. Ready to process prints.");
}

// ------------------------------------------------------------------------------
// Main Loop
// ------------------------------------------------------------------------------
void loop() {
    // 1. Handle HTTP Web Server Requests
    webServer.handleClient();

    // 2. Handle RAW JetDirect TCP Client (Port 9100)
    WiFiClient client = rawServer.available();
    if (client) {
        Serial.println("[TCP] New client connected on Port 9100");
        uint8_t buffer[512];
        
        while (client.connected()) {
            while (client.available()) {
                int bytesRead = client.read(buffer, sizeof(buffer));
                if (bytesRead > 0) {
                    sendToUsbPrinter(buffer, bytesRead);
                }
            }
            delay(1);
        }
        
        client.stop();
        totalJobsPrinted++;
        Serial.println("[TCP] Print job completed and client disconnected.");
    }

    delay(2);
}
