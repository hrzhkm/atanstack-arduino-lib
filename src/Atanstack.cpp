#include "Atanstack.h"

AtanstackClient* AtanstackClient::_activeInstance = nullptr;

namespace {
#if defined(ARDUINO_ARCH_ESP32)
const char kAtanstackRootCa[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
)EOF";
#endif

uint32_t fnv1aHash(const String& value) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < value.length(); ++i) {
    hash ^= (uint8_t)value[i];
    hash *= 16777619u;
  }
  return hash;
}

void appendBase36Chunk(String& out, uint32_t value) {
  static const char* kAlphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  for (int i = 0; i < 4; ++i) {
    out += kAlphabet[value % 36u];
    value /= 36u;
  }
}

void appendPaddedBase36Chunk(String& out, uint32_t value) {
  static const char* kAlphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  char chunk[4];
  for (int i = 3; i >= 0; --i) {
    chunk[i] = kAlphabet[value % 36u];
    value /= 36u;
  }
  out += chunk[0];
  out += chunk[1];
  out += chunk[2];
  out += chunk[3];
}
}  // namespace

#if defined(ARDUINO_ARCH_ESP32)
AtanstackClient::AtanstackClient()
    : _networkClient(&_defaultNetworkClient),
      _secureNetworkClient(&_defaultNetworkClient),
      _webSocketClient(nullptr),
      _mqtt(_defaultNetworkClient),
      _lastReconnectAttemptMs(0),
      _ntpInitAttempted(false),
      _lastNtpAttemptMs(0),
      _lastError(""),
      _nextDataSlot(0),
      _nextMetaSlot(0),
      _automationRuleCount(0),
      _automationPrefsOpen(false) {
  for (uint8_t i = 0; i < kSlotCount; ++i) {
    _dataSlots[i] = nullptr;
    _metaSlots[i] = nullptr;
    _switchSlots[i].gpio = 0;
    _switchSlots[i].activeLevel = LOW;
    _switchSlots[i].used = false;
  }
  for (uint8_t i = 0; i < kAutomationRuleMax; ++i) {
    _automationRules[i].id[0] = '\0';
    _automationRules[i].gpio = 0;
    _automationRules[i].hour = 0;
    _automationRules[i].minute = 0;
    _automationRules[i].on = false;
    _automationRules[i].lastFired = 0;
    _automationRules[i].endEpoch = 0;
    _automationRules[i].durationSeconds = 0;
    _automationRules[i].endOn = false;
  }
}

AtanstackClient::AtanstackClient(Client& networkClient) : AtanstackClient() {
  _networkClient = &networkClient;
  _secureNetworkClient = nullptr;
  _mqtt.setClient(networkClient);
}

AtanstackClient::AtanstackClient(WiFiClientSecure& networkClient)
    : AtanstackClient() {
  _networkClient = &networkClient;
  _secureNetworkClient = &networkClient;
  _mqtt.setClient(networkClient);
}
#else
AtanstackClient::AtanstackClient(Client& networkClient)
    : _networkClient(&networkClient),
      _webSocketClient(nullptr),
      _mqtt(networkClient),
      _lastReconnectAttemptMs(0),
      _ntpInitAttempted(false),
      _lastNtpAttemptMs(0),
      _lastError(""),
      _nextDataSlot(0),
      _nextMetaSlot(0) {
  for (uint8_t i = 0; i < kSlotCount; ++i) {
    _dataSlots[i] = nullptr;
    _metaSlots[i] = nullptr;
    _switchSlots[i].gpio = 0;
    _switchSlots[i].activeLevel = LOW;
    _switchSlots[i].used = false;
  }
}
#endif

AtanstackClient::~AtanstackClient() {
  delete _webSocketClient;
  for (uint8_t i = 0; i < kSlotCount; ++i) {
    delete _dataSlots[i];
    delete _metaSlots[i];
  }
  if (_activeInstance == this) {
    _activeInstance = nullptr;
  }
}

bool AtanstackClient::begin(const char* devicePid, const char* deviceSecret) {
  AtanstackConfig config;
  config.devicePid = devicePid;
  config.deviceSecret = deviceSecret;
  return begin(config);
}

bool AtanstackClient::begin(const AtanstackConfig& config) {
  if (!validateConfig(config)) {
    return false;
  }

  _config = config;
#if defined(ARDUINO_ARCH_ESP32)
  if (_secureNetworkClient != nullptr &&
      strcmp(_config.brokerHost, "mqtt.atanstack.com") == 0 &&
      _config.webSocketPath != nullptr) {
    _secureNetworkClient->setCACert(kAtanstackRootCa);
  }
#endif
  delete _webSocketClient;
  _webSocketClient = nullptr;
  if (_config.webSocketPath != nullptr &&
      strlen(_config.webSocketPath) > 0) {
    _webSocketClient = new (std::nothrow) AtanstackWebSocketClient(
        *_networkClient, _config.brokerHost, _config.brokerPort,
        _config.webSocketPath);
    if (_webSocketClient == nullptr) {
      setError("websocket_allocation_failed");
      return false;
    }
    _mqtt.setClient(*_webSocketClient);
  } else {
    _mqtt.setClient(*_networkClient);
  }
  _mqtt.setServer(_config.brokerHost, _config.brokerPort);
  _activeInstance = this;
  _mqtt.setCallback(AtanstackClient::onMqttMessage);
  _mqtt.setBufferSize((uint16_t)_config.maxPayloadBytes);
  for (uint8_t i = 0; i < kSlotCount; ++i) {
    if (_dataSlots[i] != nullptr) {
      delete _dataSlots[i];
    }
    if (_metaSlots[i] != nullptr) {
      delete _metaSlots[i];
    }
    _dataSlots[i] = new DynamicJsonDocument(_config.maxPayloadBytes);
    _metaSlots[i] = new DynamicJsonDocument(_config.maxPayloadBytes / 2);
    _dataSlots[i]->to<JsonObject>();
    _metaSlots[i]->to<JsonObject>();
  }
  _nextDataSlot = 0;
  _nextMetaSlot = 0;
  _ntpInitAttempted = false;
  _lastNtpAttemptMs = 0;
  _lastError = "";
#if defined(ARDUINO_ARCH_ESP32)
  loadAutomationConfig();
#endif
  return true;
}

bool AtanstackClient::connect() {
  if (_config.devicePid == nullptr || strlen(_config.devicePid) == 0) {
    setError("missing_device_pid");
    return false;
  }
  if (_config.deviceSecret == nullptr || strlen(_config.deviceSecret) == 0) {
    setError("missing_device_secret");
    return false;
  }

  if (_mqtt.connected()) {
    return true;
  }

  const char* clientId = (_config.clientId != nullptr && strlen(_config.clientId) > 0)
                             ? _config.clientId
                             : _config.devicePid;
  const char* mqttUsername =
      (_config.mqttUsername != nullptr && strlen(_config.mqttUsername) > 0)
          ? _config.mqttUsername
          : _config.devicePid;

  if (!_mqtt.connect(clientId, mqttUsername, _config.deviceSecret)) {
    setError("mqtt_connect_failed");
    return false;
  }

  if (!subscribeControlTopic()) {
    return false;
  }
  for (uint8_t i = 0; i < kSlotCount; ++i) {
    if (_switchSlots[i].used) {
      publishSwitchCapability(i, _switchSlots[i].gpio, _switchSlots[i].activeLevel);
    }
  }
  _lastError = "";
  return true;
}

void AtanstackClient::loop() {
  _mqtt.loop();
  ensureClockSynced();
#if defined(ARDUINO_ARCH_ESP32)
  runAutomationSchedule();
#endif
  unsigned long now = millis();

  if (_mqtt.connected()) {
    return;
  }

  if ((now - _lastReconnectAttemptMs) < _config.reconnectIntervalMs) {
    return;
  }

  _lastReconnectAttemptMs = now;
  connect();
}

bool AtanstackClient::connected() { return _mqtt.connected(); }

AtanstackEvent::AtanstackEvent(AtanstackClient& client, const char* eventType)
    : _client(&client),
      _eventType(eventType),
      _data(client.nextDataObject()),
      _meta(client.nextMetaObject()),
      _valid(!_data.isNull() && !_meta.isNull()),
      _hasData(false) {}

bool AtanstackEvent::send() {
  if (!_valid) {
    return false;
  }
  if (!_hasData) {
    _client->setError("missing_data");
    return false;
  }
  return _client->send(_eventType, _data, _meta);
}

AtanstackEvent AtanstackClient::event(const char* eventType) {
  return AtanstackEvent(*this, eventType);
}

JsonObject AtanstackClient::data(const char* key, const char* value) {
  if (!validateKey(key)) {
    return JsonObject();
  }
  JsonObject obj = nextDataObject();
  obj[key] = value;
  return obj;
}

JsonObject AtanstackClient::data(const char* key, const String& value) {
  if (!validateKey(key)) {
    return JsonObject();
  }
  JsonObject obj = nextDataObject();
  obj[key] = value;
  return obj;
}

JsonObject AtanstackClient::data(const char* key, int value) {
  if (!validateKey(key)) {
    return JsonObject();
  }
  JsonObject obj = nextDataObject();
  obj[key] = value;
  return obj;
}

JsonObject AtanstackClient::data(const char* key, long value) {
  if (!validateKey(key)) {
    return JsonObject();
  }
  JsonObject obj = nextDataObject();
  obj[key] = value;
  return obj;
}

JsonObject AtanstackClient::data(const char* key, float value) {
  if (!validateKey(key)) {
    return JsonObject();
  }
  JsonObject obj = nextDataObject();
  obj[key] = value;
  return obj;
}

JsonObject AtanstackClient::data(const char* key, double value) {
  if (!validateKey(key)) {
    return JsonObject();
  }
  JsonObject obj = nextDataObject();
  obj[key] = value;
  return obj;
}

JsonObject AtanstackClient::data(const char* key, bool value) {
  if (!validateKey(key)) {
    return JsonObject();
  }
  JsonObject obj = nextDataObject();
  obj[key] = value;
  return obj;
}

JsonObject AtanstackClient::meta(const char* key, const char* value) {
  if (!validateKey(key)) {
    return JsonObject();
  }
  JsonObject obj = nextMetaObject();
  obj[key] = value;
  return obj;
}

JsonObject AtanstackClient::meta(const char* key, const String& value) {
  if (!validateKey(key)) {
    return JsonObject();
  }
  JsonObject obj = nextMetaObject();
  obj[key] = value;
  return obj;
}

JsonObject AtanstackClient::meta(const char* key, int value) {
  if (!validateKey(key)) {
    return JsonObject();
  }
  JsonObject obj = nextMetaObject();
  obj[key] = value;
  return obj;
}

JsonObject AtanstackClient::meta(const char* key, long value) {
  if (!validateKey(key)) {
    return JsonObject();
  }
  JsonObject obj = nextMetaObject();
  obj[key] = value;
  return obj;
}

JsonObject AtanstackClient::meta(const char* key, float value) {
  if (!validateKey(key)) {
    return JsonObject();
  }
  JsonObject obj = nextMetaObject();
  obj[key] = value;
  return obj;
}

JsonObject AtanstackClient::meta(const char* key, double value) {
  if (!validateKey(key)) {
    return JsonObject();
  }
  JsonObject obj = nextMetaObject();
  obj[key] = value;
  return obj;
}

JsonObject AtanstackClient::meta(const char* key, bool value) {
  if (!validateKey(key)) {
    return JsonObject();
  }
  JsonObject obj = nextMetaObject();
  obj[key] = value;
  return obj;
}

bool AtanstackClient::send(const char* eventType, JsonObject data, JsonObject meta) {
  if (!validateSendArgs(eventType, data)) {
    return false;
  }

  if (!_mqtt.connected()) {
    setError("mqtt_not_connected");
    return false;
  }

  String topic;
  if (!buildTopic(eventType, topic)) {
    return false;
  }

  DynamicJsonDocument doc(_config.maxPayloadBytes);
  doc["device_id"] = _config.devicePid;
  doc["event_type"] = eventType;
  String timestamp;
  buildTimestamp(timestamp);
  doc["timestamp"] = timestamp;
  doc["schema_version"] = 1;

  JsonObject payloadData = doc.createNestedObject("data");
  for (JsonPair kv : data) {
    payloadData[kv.key()] = kv.value();
  }

  JsonObject payloadMeta = doc.createNestedObject("meta");
  if (!meta.isNull()) {
    for (JsonPair kv : meta) {
      payloadMeta[kv.key()] = kv.value();
    }
  }

  String payload;
  size_t written = serializeJson(doc, payload);
  if (written == 0) {
    setError("serialize_failed");
    return false;
  }

  if (payload.length() > _config.maxPayloadBytes) {
    setError("payload_too_large");
    return false;
  }

  if (!_mqtt.publish(topic.c_str(), payload.c_str())) {
    setError("publish_failed");
    return false;
  }

  _lastError = "";
  return true;
}

bool AtanstackClient::switchPin(uint8_t gpio, uint8_t activeLevel) {
  if (activeLevel != LOW && activeLevel != HIGH) {
    setError("invalid_active_level");
    return false;
  }

  uint8_t slotIndex = 0;
  if (!findSwitchSlotByGpio(gpio, slotIndex)) {
    for (uint8_t i = 0; i < kSlotCount; ++i) {
      if (!_switchSlots[i].used) {
        slotIndex = i;
        _switchSlots[i].used = true;
        _switchSlots[i].gpio = gpio;
        break;
      }
    }
    if (!_switchSlots[slotIndex].used || _switchSlots[slotIndex].gpio != gpio) {
      setError("switch_slot_full");
      return false;
    }
  }

  _switchSlots[slotIndex].activeLevel = activeLevel;
  pinMode(gpio, OUTPUT);
  digitalWrite(gpio, activeLevel == LOW ? HIGH : LOW);

  if (_mqtt.connected()) {
    if (!publishSwitchCapability(slotIndex, gpio, activeLevel)) {
      return false;
    }
  }

  _lastError = "";
  return true;
}

const char* AtanstackClient::lastError() const {
  return _lastError.length() == 0 ? "" : _lastError.c_str();
}

int AtanstackClient::mqttState() { return _mqtt.state(); }

void AtanstackClient::onMqttMessage(char* topic, byte* payload, unsigned int length) {
  if (_activeInstance == nullptr) {
    return;
  }
  _activeInstance->handleMqttMessage(topic, payload, length);
}

bool AtanstackClient::subscribeControlTopic() {
  String topic;
  if (!buildControlTopic(topic)) {
    setError("build_control_topic_failed");
    return false;
  }
  if (!_mqtt.subscribe(topic.c_str())) {
    setError("subscribe_control_failed");
    return false;
  }

  String pingTopic;
  if (!buildPingTopic(pingTopic)) {
    setError("build_ping_topic_failed");
    return false;
  }
  if (!_mqtt.subscribe(pingTopic.c_str())) {
    setError("subscribe_ping_failed");
    return false;
  }

#if defined(ARDUINO_ARCH_ESP32)
  String automationTopic;
  if (!buildAutomationTopic(automationTopic)) {
    setError("build_automation_topic_failed");
    return false;
  }
  if (!_mqtt.subscribe(automationTopic.c_str())) {
    setError("subscribe_automation_failed");
    return false;
  }
#endif

  return true;
}

bool AtanstackClient::buildControlTopic(String& outTopic) const {
  if (_config.topicBase == nullptr || strlen(_config.topicBase) == 0) {
    return false;
  }
  outTopic.reserve(strlen(_config.topicBase) + strlen(_config.devicePid) + 20);
  outTopic = _config.topicBase;
  outTopic += "/";
  outTopic += _config.devicePid;
  outTopic += "/control/switch";
  return true;
}

bool AtanstackClient::buildPingTopic(String& outTopic) const {
  if (_config.topicBase == nullptr || strlen(_config.topicBase) == 0) {
    return false;
  }
  outTopic.reserve(strlen(_config.topicBase) + strlen(_config.devicePid) + 20);
  outTopic = _config.topicBase;
  outTopic += "/";
  outTopic += _config.devicePid;
  outTopic += "/commands/ping";
  return true;
}

bool AtanstackClient::buildPongTopic(String& outTopic) const {
  if (_config.topicBase == nullptr || strlen(_config.topicBase) == 0) {
    return false;
  }
  outTopic.reserve(strlen(_config.topicBase) + strlen(_config.devicePid) + 18);
  outTopic = _config.topicBase;
  outTopic += "/";
  outTopic += _config.devicePid;
  outTopic += "/status/pong";
  return true;
}

#if defined(ARDUINO_ARCH_ESP32)
bool AtanstackClient::buildAutomationTopic(String& outTopic) const {
  if (_config.topicBase == nullptr || strlen(_config.topicBase) == 0) {
    return false;
  }
  outTopic.reserve(strlen(_config.topicBase) + strlen(_config.devicePid) + 24);
  outTopic = _config.topicBase;
  outTopic += "/";
  outTopic += _config.devicePid;
  outTopic += "/control/automation";
  return true;
}
#endif

bool AtanstackClient::publishPong(const char* requestId) {
  if (requestId == nullptr || strlen(requestId) == 0) {
    setError("pong_missing_request_id");
    return false;
  }

  String topic;
  if (!buildPongTopic(topic)) {
    setError("pong_topic_failed");
    return false;
  }

  DynamicJsonDocument doc(_config.maxPayloadBytes / 2);
  doc["request_id"] = requestId;
  String sentAt;
  buildTimestamp(sentAt);
  doc["sent_at"] = sentAt;

  String payload;
  if (serializeJson(doc, payload) == 0) {
    setError("pong_serialize_failed");
    return false;
  }

  if (!_mqtt.publish(topic.c_str(), payload.c_str())) {
    setError("pong_publish_failed");
    return false;
  }

  _lastError = "";
  return true;
}

bool AtanstackClient::publishSwitchCapability(uint8_t slotIndex,
                                              uint8_t gpio,
                                              uint8_t activeLevel) {
  JsonObject capability = data("gpio", (int)gpio);
  if (capability.isNull()) {
    return false;
  }
  String controlId;
  buildSwitchControlId(slotIndex, controlId);
  capability["control_id"] = controlId;
  capability["active_low"] = activeLevel == LOW;
  capability["control"] = "switch";
  return send("capability-switch", capability);
}

void AtanstackClient::buildSwitchControlId(uint8_t slotIndex, String& outControlId) const {
  const String deviceId = String(_config.devicePid != nullptr ? _config.devicePid : "");
  const String slotTag = String((unsigned int)slotIndex + 1);
  const uint32_t hashA = fnv1aHash(deviceId);
  const uint32_t hashB = fnv1aHash(deviceId + "-" + slotTag);
  const uint32_t slotCode = (uint32_t)slotIndex + 1u;

  outControlId = "switch-";
  appendBase36Chunk(outControlId, hashA);
  outControlId += "-";
  appendBase36Chunk(outControlId, hashB);
  outControlId += "-";
  appendPaddedBase36Chunk(outControlId, slotCode);
}

bool AtanstackClient::publishSwitchState(uint8_t slotIndex, bool on) {
  JsonObject state = data("gpio", (int)_switchSlots[slotIndex].gpio);
  if (state.isNull()) {
    return false;
  }
  String controlId;
  buildSwitchControlId(slotIndex, controlId);
  state["control_id"] = controlId;
  state["state"] = on ? "on" : "off";
  return send("switch-state", state);
}

bool AtanstackClient::findSwitchSlotByGpio(uint8_t gpio, uint8_t& outIndex) const {
  for (uint8_t i = 0; i < kSlotCount; ++i) {
    if (_switchSlots[i].used && _switchSlots[i].gpio == gpio) {
      outIndex = i;
      return true;
    }
  }
  return false;
}

bool AtanstackClient::isSwitchStateOn(const JsonDocument& doc, bool& outOn) const {
  if (doc["on"].is<bool>()) {
    outOn = doc["on"].as<bool>();
    return true;
  }
  if (doc["state"].is<const char*>()) {
    const char* state = doc["state"];
    if (state != nullptr && strcmp(state, "on") == 0) {
      outOn = true;
      return true;
    }
    if (state != nullptr && strcmp(state, "off") == 0) {
      outOn = false;
      return true;
    }
  }
  return false;
}

bool AtanstackClient::applySwitchState(uint8_t gpio, bool on) {
  uint8_t slotIndex = 0;
  if (!findSwitchSlotByGpio(gpio, slotIndex)) {
    setError("switch_not_registered");
    return false;
  }
  const uint8_t onLevel = _switchSlots[slotIndex].activeLevel;
  const uint8_t offLevel = onLevel == LOW ? HIGH : LOW;
  digitalWrite(gpio, on ? onLevel : offLevel);
  return publishSwitchState(slotIndex, on);
}

void AtanstackClient::handleMqttMessage(char* topic, byte* payload, unsigned int length) {
  if (topic == nullptr || payload == nullptr) {
    return;
  }

#if defined(ARDUINO_ARCH_ESP32)
  String automationTopic;
  if (buildAutomationTopic(automationTopic)) {
    if (strcmp(topic, automationTopic.c_str()) == 0) {
      handleAutomationConfig(payload, length);
      return;
    }
  }
#endif

  if (length == 0) {
    return;
  }

  String controlTopic;
  if (!buildControlTopic(controlTopic)) {
    return;
  }
  String pingTopic;
  if (!buildPingTopic(pingTopic)) {
    return;
  }

  DynamicJsonDocument doc(_config.maxPayloadBytes);
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    setError("command_payload_invalid");
    return;
  }

  if (strcmp(topic, controlTopic.c_str()) == 0) {
    if (!doc["gpio"].is<int>()) {
      setError("control_missing_gpio");
      return;
    }

    bool on = false;
    if (!isSwitchStateOn(doc, on)) {
      setError("control_missing_state");
      return;
    }

    applySwitchState((uint8_t)doc["gpio"].as<int>(), on);
    return;
  }

  if (strcmp(topic, pingTopic.c_str()) == 0) {
    const char* requestId =
        doc["request_id"].is<const char*>() ? doc["request_id"].as<const char*>()
                                             : nullptr;
    publishPong(requestId);
    return;
  }
}

bool AtanstackClient::validateConfig(const AtanstackConfig& config) {
  if (config.brokerHost == nullptr || strlen(config.brokerHost) == 0) {
    setError("missing_broker_host");
    return false;
  }
  if (config.devicePid == nullptr || strlen(config.devicePid) == 0) {
    setError("missing_device_pid");
    return false;
  }
  if (config.deviceSecret == nullptr || strlen(config.deviceSecret) == 0) {
    setError("missing_device_secret");
    return false;
  }
  if (config.topicBase == nullptr || strlen(config.topicBase) == 0) {
    setError("missing_topic_base");
    return false;
  }
  if (config.maxPayloadBytes < 256) {
    setError("payload_limit_too_small");
    return false;
  }
  return true;
}

bool AtanstackClient::validateSendArgs(const char* eventType, JsonObject data) {
  if (eventType == nullptr || strlen(eventType) == 0) {
    setError("missing_event_type");
    return false;
  }
  if (data.isNull()) {
    setError("missing_data");
    return false;
  }
  return true;
}

bool AtanstackClient::validateKey(const char* key) {
  if (key == nullptr || strlen(key) == 0) {
    setError("missing_key");
    return false;
  }
  return true;
}

JsonObject AtanstackClient::nextDataObject() {
  DynamicJsonDocument* slot = _dataSlots[_nextDataSlot];
  if (slot == nullptr || slot->capacity() == 0) {
    setError("client_not_initialized");
    return JsonObject();
  }
  slot->clear();
  JsonObject obj = slot->to<JsonObject>();
  _nextDataSlot = (_nextDataSlot + 1) % kSlotCount;
  return obj;
}

JsonObject AtanstackClient::nextMetaObject() {
  DynamicJsonDocument* slot = _metaSlots[_nextMetaSlot];
  if (slot == nullptr || slot->capacity() == 0) {
    setError("client_not_initialized");
    return JsonObject();
  }
  slot->clear();
  JsonObject obj = slot->to<JsonObject>();
  _nextMetaSlot = (_nextMetaSlot + 1) % kSlotCount;
  return obj;
}

bool AtanstackClient::buildTopic(const char* eventType, String& outTopic) const {
  if (_config.topicBase == nullptr || strlen(_config.topicBase) == 0) {
    return false;
  }
  outTopic.reserve(strlen(_config.topicBase) + strlen(_config.devicePid) + strlen(eventType) + 16);
  outTopic = _config.topicBase;
  outTopic += "/";
  outTopic += _config.devicePid;
  outTopic += "/events/";
  outTopic += eventType;
  return true;
}

void AtanstackClient::buildTimestamp(String& outTimestamp) {
  ensureClockSynced();
  const time_t now = time(nullptr);
  if (now < 946684800) {
    outTimestamp = "1970-01-01T00:00:00Z";
    return;
  }

  struct tm timeInfo;
  gmtime_r(&now, &timeInfo);
  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeInfo);
  outTimestamp = buffer;
}

void AtanstackClient::ensureClockSynced() {
#if defined(ESP32) || defined(ESP8266)
  const unsigned long nowMs = millis();
  if (_ntpInitAttempted && (nowMs - _lastNtpAttemptMs) < 10000) {
    return;
  }

  if (time(nullptr) >= 946684800) {
    return;
  }

#if defined(ARDUINO_ARCH_ESP32)
  if (_automationTz.length() > 0) {
    configTzTime(_automationTz.c_str(), "pool.ntp.org", "time.nist.gov");
  } else {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  }
#else
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
#endif
  _ntpInitAttempted = true;
  _lastNtpAttemptMs = nowMs;
#endif
}

#if defined(ARDUINO_ARCH_ESP32)
bool AtanstackClient::automationClockSynced() const {
  return time(nullptr) >= 946684800;
}

void AtanstackClient::openAutomationPrefs(bool write) {
  if (_automationPrefsOpen) {
    _automationPrefs.end();
    _automationPrefsOpen = false;
  }
  _automationPrefsOpen = _automationPrefs.begin("atan-auto", !write);
}

void AtanstackClient::applyAutomationTz(const char* tz) {
  if (tz == nullptr || strlen(tz) == 0) {
    return;
  }
  _automationTz = tz;
  configTzTime(_automationTz.c_str(), "pool.ntp.org", "time.nist.gov");
}

void AtanstackClient::persistAutomationConfig(const String& raw) {
  openAutomationPrefs(true);
  if (_automationPrefsOpen) {
    _automationPrefs.putString("cfg", raw);
  }
  openAutomationPrefs(false);
}

void AtanstackClient::loadAutomationConfig() {
  openAutomationPrefs(false);
  if (!_automationPrefsOpen) {
    return;
  }
  String raw = _automationPrefs.getString("cfg", "");
  openAutomationPrefs(false);
  if (raw.length() == 0) {
    return;
  }
  handleAutomationConfig(reinterpret_cast<byte*>(&raw[0]), raw.length());
}

void AtanstackClient::handleAutomationConfig(byte* payload, unsigned int length) {
  _automationRuleCount = 0;
  if (payload == nullptr || length == 0) {
    persistAutomationConfig(String());
    return;
  }

  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    setError("automation_config_invalid");
    return;
  }

  const char* tz = doc["tz"] | "";
  JsonArray rules = doc["rules"].as<JsonArray>();
  if (!rules.isNull()) {
    for (JsonObject rule : rules) {
      if (_automationRuleCount >= kAutomationRuleMax) {
        break;
      }
      const char* id = rule["id"] | "";
      const char* timeValue = rule["time"] | "";
      const char* stateValue = rule["state"] | "";
      const char* endValue = rule["end"] | "";
      int gpio = rule["gpio"] | -1;
      int dur = rule["dur"] | 0;
      int hour = -1;
      int minute = -1;
      if (strlen(id) == 0 || strlen(timeValue) == 0 || gpio < 0 ||
          sscanf(timeValue, "%d:%d", &hour, &minute) != 2 || hour < 0 ||
          hour > 23 || minute < 0 || minute > 59 ||
          (strcmp(stateValue, "on") != 0 && strcmp(stateValue, "off") != 0) ||
          dur < 0 || (dur > 0 && strcmp(endValue, "on") != 0 &&
                      strcmp(endValue, "off") != 0)) {
        continue;
      }
      AutomationRule& slot = _automationRules[_automationRuleCount];
      strncpy(slot.id, id, sizeof(slot.id) - 1);
      slot.id[sizeof(slot.id) - 1] = '\0';
      slot.gpio = (uint8_t)gpio;
      slot.hour = (uint8_t)hour;
      slot.minute = (uint8_t)minute;
      slot.on = strcmp(stateValue, "on") == 0;
      slot.lastFired = automationLastFired(slot.id);
      slot.durationSeconds = (uint32_t)dur;
      slot.endOn = strcmp(endValue, "on") == 0;
      slot.endEpoch = automationEndEpoch(slot.id);
      ++_automationRuleCount;
    }
  }

  applyAutomationTz(tz);
  persistAutomationConfig(String(reinterpret_cast<char*>(payload), length));
}

uint32_t AtanstackClient::automationLastFired(const char* ruleId) {
  openAutomationPrefs(false);
  if (!_automationPrefsOpen) {
    return 0;
  }
  const uint32_t hash = fnv1aHash(String(ruleId));
  char key[12];
  snprintf(key, sizeof(key), "f%08lx", (unsigned long)hash);
  const uint32_t value = _automationPrefs.getULong(key, 0);
  openAutomationPrefs(false);
  return value;
}

void AtanstackClient::setAutomationLastFired(const char* ruleId,
                                             uint32_t minuteKey) {
  openAutomationPrefs(true);
  if (!_automationPrefsOpen) {
    return;
  }
  const uint32_t hash = fnv1aHash(String(ruleId));
  char key[12];
  snprintf(key, sizeof(key), "f%08lx", (unsigned long)hash);
  _automationPrefs.putULong(key, minuteKey);
  openAutomationPrefs(false);
}

uint32_t AtanstackClient::automationEndEpoch(const char* ruleId) {
  openAutomationPrefs(false);
  if (!_automationPrefsOpen) {
    return 0;
  }
  const uint32_t hash = fnv1aHash(String(ruleId));
  char key[12];
  snprintf(key, sizeof(key), "e%08lx", (unsigned long)hash);
  const uint32_t value = _automationPrefs.getULong(key, 0);
  openAutomationPrefs(false);
  return value;
}

void AtanstackClient::setAutomationEndEpoch(const char* ruleId,
                                            uint32_t endEpoch) {
  openAutomationPrefs(true);
  if (!_automationPrefsOpen) {
    return;
  }
  const uint32_t hash = fnv1aHash(String(ruleId));
  char key[12];
  snprintf(key, sizeof(key), "e%08lx", (unsigned long)hash);
  _automationPrefs.putULong(key, endEpoch);
  openAutomationPrefs(false);
}

bool AtanstackClient::publishAutomationFired(const AutomationRule& rule,
                                             bool on,
                                             const char* phase) {
  JsonObject fired = data("rule_id", String(rule.id));
  if (fired.isNull()) {
    return false;
  }
  fired["gpio"] = (int)rule.gpio;
  fired["state"] = on ? "on" : "off";
  fired["phase"] = phase;
  return send("automation-fired", fired);
}

void AtanstackClient::runAutomationSchedule() {
  if (_automationRuleCount == 0 || !automationClockSynced()) {
    return;
  }
  // ponytail: requires a synced clock this boot; an ESP32 that boots fully
  // offline (no NTP) keeps persisted rules but cannot fire until it sees a
  // network once. Add an RTC/NVS epoch backup if boot-while-offline matters.

  const time_t nowEpoch = time(nullptr);
  struct tm local;
  localtime_r(&nowEpoch, &local);
  const uint32_t minuteKey =
      automationMinuteKey(local.tm_year + 1900, local.tm_yday, local.tm_hour,
                          local.tm_min);

  for (uint8_t i = 0; i < _automationRuleCount; ++i) {
    AutomationRule& rule = _automationRules[i];

    if (rule.endEpoch != 0 && automationEndDue((uint32_t)nowEpoch, rule.endEpoch)) {
      uint8_t slotIndex = 0;
      if (findSwitchSlotByGpio(rule.gpio, slotIndex)) {
        applySwitchState(rule.gpio, rule.endOn);
        publishAutomationFired(rule, rule.endOn, "end");
      }
      rule.endEpoch = 0;
      setAutomationEndEpoch(rule.id, 0);
    }

    if (!automationMinuteDue(minuteKey, rule.hour, rule.minute,
                             rule.lastFired)) {
      continue;
    }
    uint8_t slotIndex = 0;
    if (!findSwitchSlotByGpio(rule.gpio, slotIndex)) {
      continue;
    }
    applySwitchState(rule.gpio, rule.on);
    rule.lastFired = minuteKey;
    setAutomationLastFired(rule.id, minuteKey);
    if (rule.durationSeconds > 0) {
      rule.endEpoch = (uint32_t)nowEpoch + rule.durationSeconds;
      setAutomationEndEpoch(rule.id, rule.endEpoch);
    }
    publishAutomationFired(rule, rule.on, "start");
  }
}
#endif

void AtanstackClient::setError(const char* message) {
  _lastError = message == nullptr ? "unknown" : message;
}
