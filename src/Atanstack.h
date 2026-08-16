#ifndef ATANSTACK_MQTT_H
#define ATANSTACK_MQTT_H

#include <Arduino.h>
#include <ArduinoHttpClient.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <b64.h>
#include <new>
#include <time.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <Preferences.h>
#include <WiFiClientSecure.h>
#endif

#include "automationSchedule.h"

class AtanstackWebSocketClient : public Client {
 public:
  AtanstackWebSocketClient(Client& tlsClient, const char* host, uint16_t port,
                           const char* path)
      : _webSocket(tlsClient, host, port), _tlsClient(tlsClient), _path(path) {}

  int connect(IPAddress, uint16_t) override {
    return beginWebSocket() ? 1 : 0;
  }

  int connect(const char*, uint16_t) override {
    return beginWebSocket() ? 1 : 0;
  }

  size_t write(uint8_t value) override { return write(&value, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (!connected() || size > UINT16_MAX) {
      return 0;
    }

    uint8_t header[4] = {0x82, 0, 0, 0};
    size_t headerSize = 2;
    if (size < 126) {
      header[1] = 0x80 | (uint8_t)size;
    } else {
      header[1] = 0xfe;
      header[2] = (uint8_t)(size >> 8);
      header[3] = (uint8_t)size;
      headerSize = 4;
    }

    uint8_t maskKey[4];
    for (size_t i = 0; i < sizeof(maskKey); ++i) {
      maskKey[i] = random(0xff);
    }
    if (_tlsClient.write(header, headerSize) != headerSize ||
        _tlsClient.write(maskKey, sizeof(maskKey)) != sizeof(maskKey)) {
      return 0;
    }

    uint8_t masked[64];
    for (size_t offset = 0; offset < size; offset += sizeof(masked)) {
      const size_t chunkSize = min(sizeof(masked), size - offset);
      for (size_t i = 0; i < chunkSize; ++i) {
        masked[i] = buffer[offset + i] ^ maskKey[(offset + i) % sizeof(maskKey)];
      }
      if (_tlsClient.write(masked, chunkSize) != chunkSize) {
        return 0;
      }
    }
    return size;
  }

  int available() override {
    if (!connected()) {
      return 0;
    }
    if (_webSocket.available() == 0) {
      _webSocket.parseMessage();
    }
    return _webSocket.available();
  }

  int read() override { return available() > 0 ? _webSocket.read() : -1; }

  int read(uint8_t* buffer, size_t size) override {
    return available() > 0 ? _webSocket.read(buffer, size) : -1;
  }

  int peek() override { return available() > 0 ? _webSocket.peek() : -1; }

  void flush() override { _webSocket.flush(); }

  void stop() override { _webSocket.stop(); }

  uint8_t connected() override { return _webSocket.connected(); }

  operator bool() override { return connected(); }

 private:
  bool beginWebSocket() {
    _webSocket.beginRequest();
    _webSocket.connectionKeepAlive();
    int status = _webSocket.get(_path);

    if (status == 0) {
      uint8_t randomKey[16];
      char base64RandomKey[25] = {};
      for (size_t i = 0; i < sizeof(randomKey); ++i) {
        randomKey[i] = random(0x01, 0xff);
      }
      b64_encode(randomKey, sizeof(randomKey),
                 reinterpret_cast<unsigned char*>(base64RandomKey),
                 sizeof(base64RandomKey));

      _webSocket.sendHeader("Upgrade", "websocket");
      _webSocket.sendHeader("Connection", "Upgrade");
      _webSocket.sendHeader("Sec-WebSocket-Key", base64RandomKey);
      _webSocket.sendHeader("Sec-WebSocket-Version", "13");
      _webSocket.sendHeader("Sec-WebSocket-Protocol", "mqtt");
      _webSocket.endRequest();
      status = _webSocket.responseStatusCode();
      if (status > 0) {
        _webSocket.skipResponseHeaders();
      }
    }

    return status == 101;
  }

  WebSocketClient _webSocket;
  Client& _tlsClient;
  const char* _path;
};

struct AtanstackConfig {
  const char* brokerHost;
  uint16_t brokerPort;
  const char* devicePid;
  const char* deviceSecret;
  const char* mqttUsername;
  const char* clientId;
  const char* topicBase;
  const char* webSocketPath;
  size_t maxPayloadBytes;
  unsigned long reconnectIntervalMs;

  AtanstackConfig()
      : brokerHost("mqtt.atanstack.com"),
        brokerPort(443),
        devicePid(""),
        deviceSecret(""),
        mqttUsername(""),
        clientId(""),
        topicBase("atanstack/v1/devices"),
        webSocketPath("/mqtt"),
        maxPayloadBytes(1024),
        reconnectIntervalMs(5000) {}
};

class AtanstackClient;

class AtanstackEvent {
 public:
  template <typename T>
  AtanstackEvent& data(const char* key, const T& value);

  template <typename T>
  AtanstackEvent& meta(const char* key, const T& value);

  bool send();

 private:
  friend class AtanstackClient;

  AtanstackEvent(AtanstackClient& client, const char* eventType);

  AtanstackClient* _client;
  const char* _eventType;
  JsonObject _data;
  JsonObject _meta;
  bool _valid;
  bool _hasData;
};

class AtanstackClient {
 public:
  static const uint8_t low = LOW;
  static const uint8_t high = HIGH;

#if defined(ARDUINO_ARCH_ESP32)
  AtanstackClient();
#endif
  explicit AtanstackClient(Client& networkClient);
#if defined(ARDUINO_ARCH_ESP32)
  explicit AtanstackClient(WiFiClientSecure& networkClient);
#endif
  ~AtanstackClient();
  AtanstackClient(const AtanstackClient&) = delete;
  AtanstackClient& operator=(const AtanstackClient&) = delete;

  bool begin(const char* devicePid, const char* deviceSecret);
  bool begin(const AtanstackConfig& config);
  bool connect();
  void loop();
  bool connected();

  AtanstackEvent event(const char* eventType);

  JsonObject data(const char* key, const char* value);
  JsonObject data(const char* key, const String& value);
  JsonObject data(const char* key, int value);
  JsonObject data(const char* key, long value);
  JsonObject data(const char* key, float value);
  JsonObject data(const char* key, double value);
  JsonObject data(const char* key, bool value);

  JsonObject meta(const char* key, const char* value);
  JsonObject meta(const char* key, const String& value);
  JsonObject meta(const char* key, int value);
  JsonObject meta(const char* key, long value);
  JsonObject meta(const char* key, float value);
  JsonObject meta(const char* key, double value);
  JsonObject meta(const char* key, bool value);

  bool send(const char* eventType, JsonObject data, JsonObject meta = JsonObject());
  bool switchPin(uint8_t gpio, uint8_t activeLevel = low);
  const char* lastError() const;
  int mqttState();

 private:
  friend class AtanstackEvent;

  static const uint8_t kSlotCount = 8;
  struct SwitchSlot {
    uint8_t gpio;
    uint8_t activeLevel;
    bool used;
  };

#if defined(ARDUINO_ARCH_ESP32)
  static const uint8_t kAutomationRuleMax = 8;
  struct AutomationRule {
    char id[40];
    uint8_t gpio;
    uint8_t hour;
    uint8_t minute;
    bool on;
    uint32_t lastFired;
    uint32_t endEpoch;
    uint32_t durationSeconds;
    bool endOn;
  };
#endif

#if defined(ARDUINO_ARCH_ESP32)
  WiFiClientSecure _defaultNetworkClient;
#endif
  Client* _networkClient;
#if defined(ARDUINO_ARCH_ESP32)
  WiFiClientSecure* _secureNetworkClient;
#endif
  AtanstackWebSocketClient* _webSocketClient;
  PubSubClient _mqtt;
  static AtanstackClient* _activeInstance;
  AtanstackConfig _config;
  unsigned long _lastReconnectAttemptMs;
  bool _ntpInitAttempted;
  unsigned long _lastNtpAttemptMs;
  String _lastError;
  DynamicJsonDocument* _dataSlots[kSlotCount];
  DynamicJsonDocument* _metaSlots[kSlotCount];
  SwitchSlot _switchSlots[kSlotCount];
  uint8_t _nextDataSlot;
  uint8_t _nextMetaSlot;

#if defined(ARDUINO_ARCH_ESP32)
  AutomationRule _automationRules[kAutomationRuleMax];
  uint8_t _automationRuleCount;
  String _automationTz;
  Preferences _automationPrefs;
  bool _automationPrefsOpen;
#endif

  static void onMqttMessage(char* topic, byte* payload, unsigned int length);
  bool subscribeControlTopic();
  bool buildControlTopic(String& outTopic) const;
  bool buildPingTopic(String& outTopic) const;
  bool buildPongTopic(String& outTopic) const;
  bool publishPong(const char* requestId);
  bool publishSwitchCapability(uint8_t slotIndex, uint8_t gpio, uint8_t activeLevel);
  void buildSwitchControlId(uint8_t slotIndex, String& outControlId) const;
  bool publishSwitchState(uint8_t slotIndex, bool on);
  bool findSwitchSlotByGpio(uint8_t gpio, uint8_t& outIndex) const;
  bool isSwitchStateOn(const JsonDocument& doc, bool& outOn) const;
  bool applySwitchState(uint8_t gpio, bool on);
  void handleMqttMessage(char* topic, byte* payload, unsigned int length);
  bool validateConfig(const AtanstackConfig& config);
  bool validateSendArgs(const char* eventType, JsonObject data);
  bool validateKey(const char* key);
  JsonObject nextDataObject();
  JsonObject nextMetaObject();
  bool buildTopic(const char* eventType, String& outTopic) const;
  void buildTimestamp(String& outTimestamp);
  void ensureClockSynced();
  void setError(const char* message);

#if defined(ARDUINO_ARCH_ESP32)
  bool buildAutomationTopic(String& outTopic) const;
  void handleAutomationConfig(byte* payload, unsigned int length);
  void persistAutomationConfig(const String& raw);
  void loadAutomationConfig();
  void openAutomationPrefs(bool write);
  void applyAutomationTz(const char* tz);
  void runAutomationSchedule();
  uint32_t automationLastFired(const char* ruleId);
  void setAutomationLastFired(const char* ruleId, uint32_t minuteKey);
  uint32_t automationEndEpoch(const char* ruleId);
  void setAutomationEndEpoch(const char* ruleId, uint32_t endEpoch);
  bool publishAutomationFired(const AutomationRule& rule, bool on, const char* phase);
  bool automationClockSynced() const;
#endif
};

template <typename T>
AtanstackEvent& AtanstackEvent::data(const char* key, const T& value) {
  if (!_valid || !_client->validateKey(key)) {
    _valid = false;
    return *this;
  }
  _data[key] = value;
  _hasData = true;
  return *this;
}

template <typename T>
AtanstackEvent& AtanstackEvent::meta(const char* key, const T& value) {
  if (!_valid || !_client->validateKey(key)) {
    _valid = false;
    return *this;
  }
  _meta[key] = value;
  return *this;
}

#endif
