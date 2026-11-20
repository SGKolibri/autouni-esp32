// Copie este arquivo para `config.h` (na mesma pasta) e preencha.
// `config.h` está no .gitignore: credenciais nunca entram no repositório.
#pragma once

// ---------- Wi-Fi ----------
#define WIFI_SSID     "minha-rede"
#define WIFI_PASSWORD "senha-da-rede"

// ---------- Broker MQTT ----------
// Copie da resposta de `POST /nodes` (campo `credentials`). A senha só aparece
// ali; se perder, gere outra com `POST /nodes/:id/rotate-credentials`.
#define MQTT_HOST         "192.168.0.10"   // IP do broker na LAN, nunca "localhost"
#define MQTT_PORT         1883
#define MQTT_USERNAME     "node-xxxxxxxx"   // credentials.username
#define MQTT_PASSWORD     "..."             // credentials.password
#define MQTT_TOPIC_PREFIX "org/00000000-0000-0000-0000-000000000001"  // credentials.topicPrefix

// ---------- Blocos habilitados ----------
// Monte e valide um bloco por vez (ver README): habilite só o que está ligado.
// Cada bloco publica no slug do device cadastrado no backend com o canal certo
// (`mqttTopic` = "devices/<slug>", `nodeId` = este nó, `channel` indicado).
#define ENABLE_PZEM    1   // channel ENERGY_METER
#define ENABLE_MQ2     0   // channel GAS_MQ2
#define ENABLE_MQ135   0   // channel GAS_MQ135
#define ENABLE_PIR     0   // channel PIR
#define RELAY_COUNT    0   // 0 a 4, channel RELAY
#define ENABLE_DIMMER  0   // channel DIMMER
#define ENABLE_IR      0   // channel IR

// Slugs: a parte depois de "devices/" no mqttTopic de cada device.
#define SLUG_PZEM   "pzem-a206"
#define SLUG_MQ2    "mq2-a206"
#define SLUG_MQ135  "mq135-a206"
#define SLUG_PIR    "pir-a206"
#define SLUG_RELAY1 "relay1-a206"
#define SLUG_RELAY2 "relay2-a206"
#define SLUG_RELAY3 "relay3-a206"
#define SLUG_RELAY4 "relay4-a206"
#define SLUG_DIMMER "dimmer-a206"
#define SLUG_IR     "ac-a206"

// ---------- Pinos ----------
#define PIN_PZEM_RX   16   // ESP32 RX2 <- TX do PZEM
#define PIN_PZEM_TX   17   // ESP32 TX2 -> RX do PZEM
#define PIN_MQ2       34   // ADC1, só entrada. Saída AO do módulo via divisor 5V -> 3,3V
#define PIN_MQ135     35   // ADC1, só entrada. Idem
#define PIN_PIR       27   // saída digital do HC-SR501 (3,3V)
#define PIN_RELAY1    25
#define PIN_RELAY2    26
#define PIN_RELAY3    32
#define PIN_RELAY4    33
#define PIN_DIMMER_GATE 13 // PWM/gate do módulo TRIAC
#define PIN_DIMMER_ZC   14 // zero-cross do módulo TRIAC
#define PIN_IR_LED      4  // LED IR (via transistor)

// A maioria dos módulos de 4 relés aciona com nível BAIXO.
#define RELAY_ACTIVE_LOW 1

// ---------- Dimmer ----------
#define MAINS_HZ 60        // Brasil: 60 Hz

// ---------- Ar-condicionado (IR) ----------
// Protocolo do aparelho, como em IRremoteESP8266 (decode_type_t): GREE,
// MIDEA, SAMSUNG_AC, LG, DAIKIN, COOLIX, ELECTRA_AC, ...
#define IR_AC_PROTOCOL GREE
#define IR_AC_DEFAULT_TEMP 24

// ---------- Intervalos ----------
#define ENERGY_INTERVAL_MS 10000
#define GAS_INTERVAL_MS     5000
// MQ-2/MQ-135 precisam aquecer; leituras antes disso não são publicadas.
#define GAS_WARMUP_MS      60000
// HC-SR501 leva ~1 min para estabilizar depois de ligado.
#define PIR_WARMUP_MS      60000
// Enquanto houver movimento, reenvia para manter a sala ocupada.
#define PIR_REPEAT_MS      30000
