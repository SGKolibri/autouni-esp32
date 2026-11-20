# AutoUni — firmware do nó ESP32

Um ESP32 com PZEM-004T, MQ-2, MQ-135, PIR HC-SR501, módulo de 4 relés, dimmer TRIAC e LED IR. No backend ele é um **nó** com credencial MQTT própria. Cada sensor ou atuador é um **device** com um canal. O contrato completo está em `autouni-backend/MQTT_SETUP.md`.

## O que o firmware faz

| Bloco | Canal no backend | Tópico (sob `org/<organizationId>/devices/<slug>/`) |
|---|---|---|
| PZEM-004T | `ENERGY_METER` | publica `energy`: `powerW`, `energyTotalWh` (contador do PZEM), `voltage`, `current`, `frequency`, `powerFactor` a cada 10 s |
| MQ-2 / MQ-135 | `GAS_MQ2` / `GAS_MQ135` | publica `sensor`: `{"kind","value","unit":"adc","raw"}` a cada 5 s, depois de 60 s de aquecimento |
| PIR | `PIR` | publica `sensor`: `{"motion":true}` na borda de subida, repete a cada 30 s enquanto houver movimento, e `false` na descida |
| Relés 1–4 | `RELAY` | assina `command`, aciona e confirma o estado real em `status` |
| Dimmer | `DIMMER` | assina `command` (`intensity` 0–100), corte de fase sincronizado com o zero-cross, confirma em `status` |
| IR | `IR` | assina `command` (`state`, `temperature`), envia o protocolo do ar-condicionado, confirma em `status` |

Além disso:

- **Credenciais fora do código:** Wi-Fi e MQTT ficam em `config.h`, que está no `.gitignore`.
- **LWT:** o broker publica `false` (retido) em `org/<id>/nodes/<usuário>/online` se o nó cair, e o nó publica `true` ao conectar. O backend marca o nó e todos os devices dele offline ou online.
- **Reconexão:** o Wi-Fi tenta de novo a cada 5 s e o ESP32 reinicia depois de 5 min sem rede. O MQTT reconecta com backoff de 2 s a 30 s. Ao reconectar, o nó reassina os comandos e republica o estado real de cada atuador.
- **Gás fora do `/status`:** o gás vai para o device do sensor, não mais para o `/status` do ar-condicionado. Os alarmes com histerese são do backend.

## Preparar

1. **Arduino IDE** com a placa *esp32 by Espressif* (core 3.x) e as bibliotecas PubSubClient, ArduinoJson (7.x), PZEM004Tv30 e IRremoteESP8266.
2. **No backend** (perfil ADMIN ou TECHNICIAN):
   1. `POST /nodes` com `{"name":"ESP32 Sala A206","roomId":"…"}`. Guarde o objeto `credentials` da resposta: a senha só aparece ali.
   2. Para cada bloco que vai ligar, crie ou atualize um device com `mqttTopic: "devices/<slug>"`, `nodeId` igual ao id do nó e o `channel` da tabela acima.
3. `cp config.example.h config.h` e preencha:
   - Wi-Fi.
   - `MQTT_HOST`, `MQTT_USERNAME`, `MQTT_PASSWORD` e `MQTT_TOPIC_PREFIX`, que são `credentials.username`, `.password` e `.topicPrefix`.
   - Os slugs.
   - Os `ENABLE_*` dos blocos ligados.

## Ligação

| Componente | ESP32 | Observação |
|---|---|---|
| PZEM-004T TX / RX | GPIO16 (RX2) / GPIO17 (TX2) | Cruzado: TX do PZEM no RX do ESP32. PZEM em 5 V |
| MQ-2 AO | GPIO34 | **Divisor 5 V → 3,3 V** (ex.: 10 kΩ / 20 kΩ). O ADC do ESP32 não tolera 5 V |
| MQ-135 AO | GPIO35 | Idem |
| PIR OUT | GPIO27 | O HC-SR501 já sai em 3,3 V |
| Relés IN1–IN4 | GPIO25, 26, 32, 33 | Módulo ativo em nível baixo (`RELAY_ACTIVE_LOW 1`) |
| Dimmer PWM / Z-C | GPIO13 / GPIO14 | Módulo TRIAC com zero-cross (tipo RobotDyn) |
| LED IR | GPIO4 | Através de um transistor (o GPIO sozinho dá pouco alcance) |

GPIO34 e GPIO35 são só entrada e ficam no ADC1, que funciona com o Wi-Fi ligado. Os pinos de boot (0, 2, 12 e 15) não são usados.

## Montagem em blocos

Ligue e valide **um bloco por vez**, nesta ordem: PZEM → MQ-2/MQ-135 → PIR → relé → dimmer → IR. Para cada um:

1. Habilite só ele em `config.h`, grave o firmware e abra o Serial Monitor a 115200 baud. Cada publish aparece como `-> <tópico> <payload>`, e `!!` indica que não foi enviado.
2. Acompanhe o broker com o usuário do backend:
   ```bash
   mosquitto_sub -h <broker> -u "$MQTT_USERNAME" -P "$MQTT_PASSWORD" -t 'org/#' -v
   ```
3. Confirme no backend (ou nas telas) que o dado chegou. Os critérios de cada bloco:

| Bloco | Critério de aceite |
|---|---|
| PZEM | `GET /energy/devices/<id>/readings` mostra `voltage` ~127/220 V e `powerW` coerente com a carga. Na 2ª leitura, `energyWh` ≥ 0. Os kWh do dia sobem devagar, não aos saltos |
| MQ-2 / MQ-135 | Em ar limpo, `value` estável e abaixo de `warnAt` (`GET /sensors/thresholds`). Com gás de isqueiro perto (sem acender), sobe, abre um alarme em `GET /sensors/alarms` e gera notificação. Ao dispersar, o alarme fecha abaixo de `clearAt` |
| PIR | Ao passar a mão, `Room.occupied` vira `true` (`GET /sensors/rooms/<roomId>`). Depois de `vacancyTimeoutSec` sem movimento, volta a `false` |
| Relé | `POST /devices/<id>/control {"command":"on"}`: o relé estala, o Serial mostra `status ON` e o device fica `ON` com o estado confirmado pelo nó |
| Dimmer | `{"command":"on","intensity":30}` deixa a lâmpada fraca e `intensity` 100 deixa no máximo. `off` apaga |
| IR | `{"command":"on","temperature":22}`: o ar liga a 22 °C. Se não reagir, troque `IR_AC_PROTOCOL` |

4. Desligue o ESP32 da energia. Em ~25 s o nó e os devices dele aparecem como offline.

Para calibrar o gás, deixe os sensores em burn-in por 24 a 48 h. Depois, ajuste os limites por device em `PUT /sensors/thresholds` com base no valor em ar limpo.

## Solução de problemas

| Serial | Causa |
|---|---|
| `MQTT: falhou rc=5` | Usuário ou senha errados. Gere outra senha com `POST /nodes/:id/rotate-credentials` |
| `MQTT: falhou rc=-2` | Broker inalcançável: confira `MQTT_HOST` (IP na LAN, nunca `localhost`) e a porta 1883 |
| Publica `->`, mas nada chega no backend | O device não está atribuído a este nó, o canal está errado, ou o `MQTT_TOPIC_PREFIX` é de outra organização. A ACL do broker descarta em silêncio. Rode `POST /nodes/:id/sync-acl` |
| `PZEM sem resposta` | TX/RX invertidos, PZEM sem 5 V, ou sem tensão na entrada AC (o PZEM só responde energizado) |
| Nó reconecta logo depois de alterar um device | Normal: o broker derruba a sessão quando a ACL muda, e o firmware reconecta sozinho |
