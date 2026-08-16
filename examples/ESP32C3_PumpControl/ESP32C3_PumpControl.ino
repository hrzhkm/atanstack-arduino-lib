// Use case:
// ESP32-C3 Super Mini pump control over MQTT.
// Register a single active-low relay (pump) as a remote switch capability
// and accept on/off commands from the AtanStack control topic.
// Daily automation rules are synced to the device (retained MQTT config) and
// executed locally on the device clock, so scheduled pump runs keep firing
// even when the AtanStack broker is unreachable.
// The built-in LED mirrors the pump state as a local status indicator.

#include <WiFi.h>
#include <Atanstack.h>

const char* WIFI_SSID = "UBA_2.4G";
const char* WIFI_PASSWORD = "izhanhebat123";
const char* DEVICE_PID = "ATN-XXXX-XXXX-XXXX";
const char* DEVICE_SECRET = "REPLACE_WITH_DEVICE_SECRET";

AtanstackClient atanstack;

unsigned long lastHeartbeatMs = 0;

const int PUMP_PIN = 6;
const int RELAY_ON = LOW;   // Active-low relay
const int RELAY_OFF = HIGH;
const int LED_PIN = 8;      // Built-in LED on ESP32-C3 Super Mini
const int LED_ON = LOW;     // Inverted logic on this board
const int LED_OFF = HIGH;

void connectWifi() {
  Serial.print("wifi: connecting to ");
  Serial.println(WIFI_SSID);

  // ESP32-C3 Super Mini specific: its PCB antenna/RF design fails to
  // associate at the default WiFi TX power. Forcing STA mode and lowering
  // TX power to 8.5 dBm is the known-good workaround for these boards.
  // (Classic boards like the DevKit V1 do not need this.)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  // Bounded wait (~15s). Blink the built-in LED while connecting so the
  // board's status is visible even without a serial monitor (the C3 Super
  // Mini's native USB-Serial/JTAG can be hard to read headlessly).
  const unsigned long deadline = millis() + 15000;
  bool ledState = false;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    Serial.print(".");
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? LED_ON : LED_OFF);
    delay(250);
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    // Don't hang here. Fall through to loop(), which reports reconnect
    // state and heartbeats; the library also retries the broker there.
    digitalWrite(LED_PIN, LED_OFF);
    Serial.println("wifi: not connected within timeout, continuing to loop");
    return;
  }

  Serial.print("wifi: connected, ip=");
  Serial.println(WiFi.localIP());
}

void setup() {
  // Preload the inactive level before enabling output to avoid an active-low
  // glitch while Wi-Fi, time, and MQTT initialize.
  digitalWrite(PUMP_PIN, RELAY_OFF);
  pinMode(PUMP_PIN, OUTPUT);

  Serial.begin(115200);
  delay(500);
  Serial.println("atanstack: boot pump control");

  // Built-in LED starts off (mirrors pump idle state).
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF);

  // begin() loads any persisted automation rules from flash immediately, so
  // schedules are armed even before Wi-Fi/clock/broker are available. Connect
  // and clock sync happen later in loop() via the library.
  if (!atanstack.begin(DEVICE_PID, DEVICE_SECRET)) {
    Serial.print("atanstack: begin failed: ");
    Serial.println(atanstack.lastError());
    return;
  }

  // Register the pump as an active-low relay switch capability. Rules target
  // this GPIO; it must be registered for local execution to drive the pin.
  if (!atanstack.switchPin(PUMP_PIN, RELAY_ON)) {
    Serial.print("atanstack: switchPin pump gpio 6 failed: ");
    Serial.println(atanstack.lastError());
    return;
  }

  Serial.println("atanstack: armed for local schedules, connecting");
}

void loop() {
  // Drives MQTT reconnects, NTP clock sync, and local automation schedule
  // execution. Schedules fire even while the broker is unreachable.
  atanstack.loop();

  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
    delay(1000);
    return;
  }

  if (!atanstack.connected()) {
    Serial.print("atanstack: waiting reconnect, lastError=");
    Serial.println(atanstack.lastError());
    Serial.print("atanstack: mqtt state=");
    Serial.println(atanstack.mqttState());
    delay(1000);
    return;
  }

  // Mirror the pump output level onto the built-in LED so the board shows
  // pump state locally. The library drives PUMP_PIN in response to remote
  // control commands; we just read it back here.
  const bool pumpOn = (digitalRead(PUMP_PIN) == RELAY_ON);
  digitalWrite(LED_PIN, pumpOn ? LED_ON : LED_OFF);

  // Periodic heartbeat so connection state is observable on the serial
  // monitor even when attached after boot (native USB CDC drops the
  // setup() logs if the host opens the port late).
  const unsigned long now = millis();
  if ((now - lastHeartbeatMs) >= 5000) {
    lastHeartbeatMs = now;
    Serial.print("atanstack: heartbeat connected=1 pump=");
    Serial.println(pumpOn ? "on" : "off");
  }

  delay(10);
}
