/*
 *  AutoUni — firmware do nó ESP32
 *  ===============================
 *  Um ESP32 é um "nó" no backend, com credencial MQTT própria. Cada sensor ou
 *  atuador ligado a ele é um device com canal (ENERGY_METER, GAS_MQ2,
 *  GAS_MQ135, PIR, RELAY, DIMMER, IR). Contrato completo no MQTT_SETUP.md do
 *  backend. Todo tópico fica sob MQTT_TOPIC_PREFIX (`org/<organizationId>`):
 *
 *    <P>/nodes/<usuário>/online      true (retido) / false (LWT, retido)
 *    <P>/devices/<pzem>/energy       {"powerW","energyTotalWh","voltage","current","frequency","powerFactor"}
 *    <P>/devices/<mq2|mq135>/sensor  {"kind":"GAS_MQ2","value":<adc>,"unit":"adc","raw":<adc>}
 *    <P>/devices/<pir>/sensor        {"motion":true|false}
 *    <P>/devices/<atuador>/command   recebido: {"state":"ON|OFF","intensity":0-100,"temperature":16-30}
 *    <P>/devices/<atuador>/status    {"status":"ON|OFF", ...} — estado REAL, depois de cada comando
 *
 *  Configuração e credenciais ficam em config.h (copie de config.example.h).
 *
 *  Bibliotecas (Library Manager): PubSubClient, ArduinoJson 7, PZEM004Tv30,
 *  IRremoteESP8266. Placa: esp32 by Espressif (core 3.x).
 */

#if __has_include("config.h")
#include "config.h"
#else
#error "Crie config.h a partir de config.example.h (credenciais ficam fora do código-fonte)"
#endif

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <PZEM004Tv30.h>
#include <IRremoteESP8266.h>
#include <IRac.h>
#include <soc/gpio_struct.h>

#define FIRMWARE_VERSION "1.0.0"

// Tipos antes de qualquer função: o Arduino gera protótipos no topo.
enum ActuatorKind : uint8_t { RELAY, DIMMER, IR_AC };

struct Actuator {
  const char* slug;
  ActuatorKind kind;
  uint8_t pin;
  bool on;
  uint8_t intensity;    // dimmer, 0-100
  uint8_t temperature;  // ar-condicionado, °C
  String commandTopic;
  String statusTopic;
};


// ===================================================================== tópicos

String topicFor(const char* slug, const char* suffix) {
  return String(MQTT_TOPIC_PREFIX) + "/devices/" + slug + "/" + suffix;
}

const String NODE_ONLINE_TOPIC =
    String(MQTT_TOPIC_PREFIX) + "/nodes/" + MQTT_USERNAME + "/online";

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

bool publishJson(const String& topic, JsonDocument& doc, bool retain = false) {
  char buf[256];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  bool ok = mqtt.connected() && mqtt.publish(topic.c_str(), (const uint8_t*)buf, n, retain);
  Serial.printf("%s %s %s\n", ok ? "->" : "!!", topic.c_str(), buf);
  return ok;
}

// ================================================================== atuadores

Actuator actuators[6];
uint8_t actuatorCount = 0;

void addActuator(const char* slug, ActuatorKind kind, uint8_t pin) {
  Actuator& a = actuators[actuatorCount++];
  a.slug = slug;
  a.kind = kind;
  a.pin = pin;
  a.on = false;
  a.intensity = 0;
  a.temperature = IR_AC_DEFAULT_TEMP;
  a.commandTopic = topicFor(slug, "command");
  a.statusTopic = topicFor(slug, "status");
}

// ---------- relé

void writeRelay(uint8_t pin, bool on) {
  digitalWrite(pin, (on ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

// ---------- dimmer TRIAC (corte de fase)
// Um timer de 100 µs conta o tempo desde a última passagem por zero; o gate é
// disparado no tick correspondente à potência. Menos potência = disparo mais
// tarde no semiciclo.

#if ENABLE_DIMMER
const uint16_t TICK_US = 100;
const uint16_t HALF_CYCLE_TICKS = 1000000UL / (2UL * MAINS_HZ) / TICK_US;  // 83 a 60 Hz
const uint16_t MIN_FIRE_TICK = 3;
const uint16_t MAX_FIRE_TICK = HALF_CYCLE_TICKS - 8;

volatile uint16_t ticksSinceZc = 0;
volatile uint16_t fireTick = 0;  // 0 = desligado
volatile bool gateHigh = false;
hw_timer_t* dimmerTimer = nullptr;

void IRAM_ATTR onZeroCross() {
  // Ruído no zero-cross: ignora pulsos no meio do semiciclo.
  if (ticksSinceZc > HALF_CYCLE_TICKS / 2) ticksSinceZc = 0;
}

void IRAM_ATTR onDimmerTick() {
  uint16_t t = ++ticksSinceZc;
  if (gateHigh) {
    GPIO.out_w1tc = (1UL << PIN_DIMMER_GATE);
    gateHigh = false;
  }
  uint16_t f = fireTick;
  if (f && t == f) {
    GPIO.out_w1ts = (1UL << PIN_DIMMER_GATE);
    gateHigh = true;
  }
}

void setupDimmer() {
  pinMode(PIN_DIMMER_GATE, OUTPUT);
  digitalWrite(PIN_DIMMER_GATE, LOW);
  pinMode(PIN_DIMMER_ZC, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_DIMMER_ZC), onZeroCross, RISING);
  dimmerTimer = timerBegin(1000000);
  timerAttachInterrupt(dimmerTimer, &onDimmerTick);
  timerAlarm(dimmerTimer, TICK_US, true, 0);
}

void writeDimmer(uint8_t intensity) {
  if (intensity == 0) {
    fireTick = 0;
    return;
  }
  uint32_t span = MAX_FIRE_TICK - MIN_FIRE_TICK;
  fireTick = MIN_FIRE_TICK + (span * (100 - intensity)) / 100;
}
#endif

// ---------- ar-condicionado por IR

#if ENABLE_IR
IRac irAc(PIN_IR_LED);

void sendAc(const Actuator& a) {
  irAc.next.protocol = decode_type_t::IR_AC_PROTOCOL;
  irAc.next.model = 1;
  irAc.next.mode = stdAc::opmode_t::kCool;
  irAc.next.celsius = true;
  irAc.next.degrees = a.temperature;
  irAc.next.fanspeed = stdAc::fanspeed_t::kAuto;
  irAc.next.swingv = stdAc::swingv_t::kOff;
  irAc.next.power = a.on;
  irAc.sendAc();
}
#endif

void applyActuator(Actuator& a) {
  switch (a.kind) {
    case RELAY:
      writeRelay(a.pin, a.on);
      break;
    case DIMMER:
#if ENABLE_DIMMER
      writeDimmer(a.on ? a.intensity : 0);
#endif
      break;
    case IR_AC:
#if ENABLE_IR
      sendAc(a);
#endif
      break;
  }
}

// Confirma o estado real. É o que o backend grava como verdade.
void publishStatus(const Actuator& a) {
  JsonDocument doc;
  doc["status"] = a.on ? "ON" : "OFF";
  if (a.kind == DIMMER) doc["intensity"] = a.intensity;
  if (a.kind == IR_AC) doc["temperature"] = a.temperature;
  publishJson(a.statusTopic, doc);
}

void handleCommand(Actuator& a, const uint8_t* payload, unsigned int len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) {
    Serial.printf("comando inválido em %s\n", a.commandTopic.c_str());
    publishStatus(a);
    return;
  }

  const char* state = doc["state"] | "";
  if (!strcmp(state, "ON")) a.on = true;
  else if (!strcmp(state, "OFF") || !strcmp(state, "STANDBY")) a.on = false;

  if (a.kind == DIMMER) {
    if (doc["intensity"].is<int>()) a.intensity = constrain(doc["intensity"].as<int>(), 0, 100);
    if (a.on && a.intensity == 0) a.intensity = 100;  // "ligar" sem intensidade
    if (a.intensity == 0) a.on = false;
  }
  if (a.kind == IR_AC && doc["temperature"].is<float>()) {
    a.temperature = constrain((int)lroundf(doc["temperature"].as<float>()), 16, 30);
  }

  applyActuator(a);
  publishStatus(a);
}

void onMqttMessage(char* topic, uint8_t* payload, unsigned int len) {
  for (uint8_t i = 0; i < actuatorCount; i++) {
    if (actuators[i].commandTopic == topic) {
      handleCommand(actuators[i], payload, len);
      return;
    }
  }
}

// ==================================================================== sensores

#if ENABLE_PZEM
HardwareSerial pzemSerial(2);
PZEM004Tv30 pzem(pzemSerial, PIN_PZEM_RX, PIN_PZEM_TX);
const String ENERGY_TOPIC = topicFor(SLUG_PZEM, "energy");

void publishEnergy() {
  float voltage = pzem.voltage();
  float current = pzem.current();
  float power = pzem.power();
  float energyKwh = pzem.energy();  // contador acumulado do PZEM, em kWh
  float frequency = pzem.frequency();
  float pf = pzem.pf();

  if (isnan(voltage) || isnan(power) || isnan(energyKwh)) {
    Serial.println("PZEM sem resposta: confira TX/RX cruzados, 5V e se há tensão na entrada AC");
    return;
  }

  JsonDocument doc;
  doc["powerW"] = power;
  // O backend calcula a energia do intervalo pela diferença do contador.
  doc["energyTotalWh"] = energyKwh * 1000.0f;
  doc["voltage"] = voltage;
  doc["current"] = current;
  if (!isnan(frequency)) doc["frequency"] = frequency;
  if (!isnan(pf)) doc["powerFactor"] = pf;
  publishJson(ENERGY_TOPIC, doc);
}
#endif

#if ENABLE_MQ2 || ENABLE_MQ135
int readAdcAverage(uint8_t pin) {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogRead(pin);
  return sum / 16;
}

void publishGas(const char* slug, const char* kind, uint8_t pin) {
  int raw = readAdcAverage(pin);
  JsonDocument doc;
  doc["kind"] = kind;
  doc["value"] = raw;
  doc["unit"] = "adc";
  doc["raw"] = raw;
  publishJson(topicFor(slug, "sensor"), doc);
}
#endif

#if ENABLE_PIR
const String PIR_TOPIC = topicFor(SLUG_PIR, "sensor");
bool pirState = false;
unsigned long pirLastSent = 0;

void publishMotion(bool motion) {
  JsonDocument doc;
  doc["motion"] = motion;
  if (publishJson(PIR_TOPIC, doc)) pirLastSent = millis();
}

void pollPir() {
  if (millis() < PIR_WARMUP_MS) return;
  bool now = digitalRead(PIN_PIR) == HIGH;
  if (now != pirState) {
    pirState = now;
    publishMotion(now);
  } else if (now && millis() - pirLastSent >= PIR_REPEAT_MS) {
    publishMotion(true);
  }
}
#endif

// ================================================================= conectividade

unsigned long wifiLostAt = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastMqttAttempt = 0;
unsigned long mqttBackoffMs = 2000;
const unsigned long WIFI_RETRY_MS = 5000;
const unsigned long MQTT_BACKOFF_MAX_MS = 30000;
// Sem Wi-Fi por tanto tempo, reinicia: recupera de travas do driver.
const unsigned long WIFI_RESTART_AFTER_MS = 5UL * 60UL * 1000UL;

void startWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttempt = millis();
  Serial.printf("Wi-Fi: conectando a %s\n", WIFI_SSID);
}

bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiLostAt) {
      Serial.printf("Wi-Fi: conectado, IP %s\n", WiFi.localIP().toString().c_str());
      wifiLostAt = 0;
    }
    return true;
  }
  unsigned long now = millis();
  if (!wifiLostAt) wifiLostAt = now ? now : 1;
  if (now - wifiLostAt > WIFI_RESTART_AFTER_MS) {
    Serial.println("Wi-Fi: fora há 5 min, reiniciando");
    ESP.restart();
  }
  if (now - lastWifiAttempt >= WIFI_RETRY_MS) {
    lastWifiAttempt = now;
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
  return false;
}

void onMqttConnected() {
  mqtt.publish(NODE_ONLINE_TOPIC.c_str(), "true", true);
  for (uint8_t i = 0; i < actuatorCount; i++) {
    mqtt.subscribe(actuators[i].commandTopic.c_str(), 1);
    // Depois de uma queda, o backend pode ter um estado otimista que não foi
    // aplicado: reafirma o estado real.
    publishStatus(actuators[i]);
  }
}

void ensureMqtt() {
  if (mqtt.connected()) return;
  unsigned long now = millis();
  if (now - lastMqttAttempt < mqttBackoffMs) return;
  lastMqttAttempt = now;

  String clientId = String(MQTT_USERNAME);
  Serial.printf("MQTT: conectando a %s:%d como %s\n", MQTT_HOST, MQTT_PORT, MQTT_USERNAME);
  // LWT: se o nó cair, o broker publica `false` (retido) por ele.
  bool ok = mqtt.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD,
                         NODE_ONLINE_TOPIC.c_str(), 1, true, "false", true);
  if (ok) {
    Serial.println("MQTT: conectado");
    mqttBackoffMs = 2000;
    onMqttConnected();
  } else {
    // rc 4/5: usuário ou senha errados (gere nova com rotate-credentials).
    Serial.printf("MQTT: falhou rc=%d, nova tentativa em %lus\n", mqtt.state(), mqttBackoffMs / 1000);
    mqttBackoffMs = min(mqttBackoffMs * 2, MQTT_BACKOFF_MAX_MS);
  }
}

// ======================================================================= setup

void setup() {
  Serial.begin(115200);
  Serial.printf("\nAutoUni nó %s, firmware %s\n", MQTT_USERNAME, FIRMWARE_VERSION);

  // Relés primeiro e já no nível "desligado", antes de qualquer espera.
#if RELAY_COUNT > 0
  const uint8_t relayPins[4] = {PIN_RELAY1, PIN_RELAY2, PIN_RELAY3, PIN_RELAY4};
  const char* relaySlugs[4] = {SLUG_RELAY1, SLUG_RELAY2, SLUG_RELAY3, SLUG_RELAY4};
  for (uint8_t i = 0; i < RELAY_COUNT && i < 4; i++) {
    writeRelay(relayPins[i], false);
    pinMode(relayPins[i], OUTPUT);
    writeRelay(relayPins[i], false);
    addActuator(relaySlugs[i], RELAY, relayPins[i]);
  }
#endif
#if ENABLE_DIMMER
  setupDimmer();
  addActuator(SLUG_DIMMER, DIMMER, PIN_DIMMER_GATE);
#endif
#if ENABLE_IR
  if (!IRac::isProtocolSupported(decode_type_t::IR_AC_PROTOCOL)) {
    Serial.println("IR: protocolo do ar-condicionado não suportado pela IRremoteESP8266");
  }
  addActuator(SLUG_IR, IR_AC, PIN_IR_LED);
#endif

#if ENABLE_PZEM
  pzemSerial.begin(9600, SERIAL_8N1, PIN_PZEM_RX, PIN_PZEM_TX);
#endif
#if ENABLE_MQ2 || ENABLE_MQ135
  analogReadResolution(12);  // 0-4095, a escala dos limites no backend
  analogSetAttenuation(ADC_11db);
#endif
#if ENABLE_PIR
  pinMode(PIN_PIR, INPUT);
#endif

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15);  // queda detectada pelo broker em ~22 s

  startWifi();
}

// ======================================================================== loop

unsigned long lastEnergy = 0;
unsigned long lastGas = 0;

void loop() {
  if (ensureWifi()) {
    ensureMqtt();
    mqtt.loop();
  }

  unsigned long now = millis();

#if ENABLE_PZEM
  if (now - lastEnergy >= ENERGY_INTERVAL_MS) {
    lastEnergy = now;
    if (mqtt.connected()) publishEnergy();
  }
#endif

#if ENABLE_MQ2 || ENABLE_MQ135
  if (now >= GAS_WARMUP_MS && now - lastGas >= GAS_INTERVAL_MS) {
    lastGas = now;
    if (mqtt.connected()) {
#if ENABLE_MQ2
      publishGas(SLUG_MQ2, "GAS_MQ2", PIN_MQ2);
#endif
#if ENABLE_MQ135
      publishGas(SLUG_MQ135, "GAS_MQ135", PIN_MQ135);
#endif
    }
  }
#endif

#if ENABLE_PIR
  if (mqtt.connected()) pollPir();
#endif

  delay(10);
}
