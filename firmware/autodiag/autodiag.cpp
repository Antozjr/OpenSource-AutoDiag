/* ============================================================================
   OpenSource AutoDiag — Firmware genérico (Fases 1-2-3)
   ----------------------------------------------------------------------------
   Plataforma : ESP32 + módulo MCP2515 (TJA1050) por SPI (ver README.md).
   Bus        : CAN 11-bit, 500 kbps.

   Este firmware NO sabe nada de ningún coche en concreto: solo sabe hablar
   CAN/ISO-TP en crudo. Toda la inteligencia por coche (qué módulos existen,
   sus IDs, sus PIDs) vive en el garaje de la web app (web/app.html), no
   aquí — así sirve para cualquier coche sin tocar una línea de este archivo.

   Arranca en modo PASIVO (listen-only, igual que firmware/sniffer.ino: no
   puede transmitir nada). Cambiar a modo ACTIVO es un comando explícito
   (MODE:ACTIVE) — a partir de ahí, y solo entonces, este firmware transmite
   al bus lo que se le pida por serie.

   Segunda barrera de seguridad (la primera vive en la web, ver app.html):
   en modo activo, el comando UDS: solo acepta como primer byte del payload
   (el SID) uno de estos servicios — todos de lectura, salvo el de borrado
   de DTC que es el único "de escritura" y para el que existe:
     01 03 09 10 14 19 22 3E 04
   Cualquier otro SID se rechaza antes de tocar el bus. No añadas más SIDs
   a esa lista sin que el usuario lo pida explícitamente y entienda el
   cambio de riesgo.

   Comandos por serie (una línea de texto cada uno):
     RESET                          -> vacía la tabla pasiva de IDs
     STATS                          -> resumen de IDs distintos / frames
     MODE:ACTIVE / MODE:PASSIVE     -> activa o desactiva la transmisión
     MODE:LOOPBACK                  -> autoprueba sin tocar el bus físico
     UDS:<reqId>:<respId>:<bytesHex>-> una petición UDS/OBD2, en crudo
     SWEEP:<startId>:<endId>        -> barrido de IDs de diagnóstico
     SWEEPSTOP                      -> aborta un barrido en curso

   LED interno (GPIO2 por defecto, ver LED_PIN): refleja el estado sin
   necesidad de mirar la web — error de arranque (parpadeo rápido), pasivo
   sin tráfico (latido lento), pasivo con tráfico (titila por trama, como
   el LED de un router), y activo/loopback (dos flashes + pausa).
   ============================================================================ */

#include <Arduino.h>
#include <SPI.h>
#include <mcp_can.h>
#include <string.h>

#define CAN_CS_PIN   5
#define CAN_INT_PIN  4
#define MCP_CLOCK    MCP_8MHZ   // cambia a MCP_16MHZ si tu módulo es de ese cristal

// LED interno del ESP32 DevKit. GPIO2 es lo habitual en la mayoría de clones,
// pero varía — si el tuyo no tiene LED ahí, cambia este pin.
#define LED_PIN 2

MCP_CAN CAN(CAN_CS_PIN);

void handleSerialCommands();
void flushDirty();
void printHexN(uint32_t v, int digits);
bool sidAllowed(uint8_t sid);
int  udsRequest(uint32_t reqId, uint32_t respId, uint8_t *payload, int payLen, uint8_t *out, int outMax, unsigned long timeoutMs);
int  isotpReceiveGeneric(uint32_t rxId, uint32_t reqId, uint8_t *ff, uint8_t *out, int outMax);
void doSweep(uint32_t startId, uint32_t endId);
void updateLed();

// ---------------------------------------------------------------------------
// Captura pasiva (idéntica a firmware/sniffer.ino) — sigue activa en ambos
// modos, es lo que alimenta la pestaña Sniffer de la web app.
// ---------------------------------------------------------------------------
#define MAX_ID 0x800
struct Entry {
  uint8_t  data[8];
  uint8_t  len;
  uint32_t count;
  uint32_t lastSeen;
  bool     seen;
  bool     dirty;
};
Entry table[MAX_ID];
uint32_t totalFrames = 0;
unsigned long lastFlush = 0;
const unsigned long FLUSH_MS = 150;

// ---------------------------------------------------------------------------
// MODE_LOOPBACK: el MCP2515 se contesta a sí mismo a nivel de silicio, sin
// tocar el bus físico (ni CANH/CANL) para nada. Sirve para probar la lógica
// de peticiones/respuestas (incluido ISO-TP) sin coche ni bus real delante.
// No está pensado para el uso normal desde la web app, solo para pruebas.
enum Mode { MODE_PASSIVE, MODE_ACTIVE, MODE_LOOPBACK };
Mode mode = MODE_PASSIVE;
volatile bool sweepAbort = false;

// ---------------------------------------------------------------------------
// LED de estado (no bloqueante — nada de delay() aquí, rompería la captura
// pasiva y los tiempos de espera de las peticiones UDS):
//   - MCP2515 no responde tras arrancar -> parpadeo rápido continuo (100ms)
//   - Pasivo, sin tráfico reciente      -> "latido" lento (1 Hz)
//   - Pasivo, con tráfico en el bus     -> titila con cada trama recibida
//     (como el LED de actividad de un router)
//   - Activo/Loopback (transmitiendo)   -> dos parpadeos cortos + pausa
// ---------------------------------------------------------------------------
bool canOk = false;
unsigned long lastFrameLedMs = 0;
unsigned long lastLedToggleMs = 0;
bool ledLevel = false;

// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(LED_PIN, OUTPUT);
  Serial.println("{\"evt\":\"boot\",\"fw\":\"AUTODIAG\",\"msg\":\"iniciando MCP2515 en modo listen-only...\"}");

  int tries = 0;
  bool ok;
  while (!(ok = (CAN.begin(MCP_ANY, CAN_500KBPS, MCP_CLOCK) == CAN_OK))) {
    Serial.println("{\"evt\":\"error\",\"msg\":\"MCP2515 no responde. Revisa SPI/cristal/alimentacion.\"}");
    delay(1000);
    if (++tries > 5) break;
  }
  canOk = ok;

  CAN.setMode(MCP_LISTENONLY);   // arranque siempre en pasivo
  mode = MODE_PASSIVE;
  pinMode(CAN_INT_PIN, INPUT);

  memset(table, 0, sizeof(table));

  Serial.println("{\"evt\":\"ready\",\"proto\":\"CAN 500k\",\"mode\":\"passive\"}");
}

// ===========================================================================
void loop() {
  handleSerialCommands();
  updateLed();

  // Captura pasiva: siempre activa, en los dos modos.
  while (CAN.checkReceive() == CAN_MSGAVAIL) {
    unsigned long rxId; uint8_t len; uint8_t buf[8];
    CAN.readMsgBuf(&rxId, &len, buf);
    if (rxId >= MAX_ID) continue;
    lastFrameLedMs = millis();
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));   // titila con cada trama pasiva

    Entry &e = table[rxId];
    bool changed = !e.seen || e.len != len || memcmp(e.data, buf, len) != 0;
    memcpy(e.data, buf, len);
    e.len = len;
    e.count++;
    e.seen = true;
    e.lastSeen = millis();
    totalFrames++;
    if (changed) e.dirty = true;
  }

  if (millis() - lastFlush >= FLUSH_MS) {
    lastFlush = millis();
    flushDirty();
  }
}

// ---------------------------------------------------------------------------
void updateLed() {
  unsigned long now = millis();

  if (!canOk) {                              // error de arranque: parpadeo rápido
    if (now - lastLedToggleMs >= 100) {
      lastLedToggleMs = now;
      ledLevel = !ledLevel;
      digitalWrite(LED_PIN, ledLevel);
    }
    return;
  }

  if (mode == MODE_ACTIVE || mode == MODE_LOOPBACK) {  // transmitiendo: dos flashes + pausa
    unsigned long t = now % 760;
    bool on = (t < 80) || (t >= 160 && t < 240);
    digitalWrite(LED_PIN, on ? HIGH : LOW);
    return;
  }

  // Pasivo: si hay tráfico reciente, el propio bucle principal ya hace
  // titilar el LED por trama (ver loop()) — aquí solo entra el "latido"
  // lento cuando llevamos un rato sin ver nada.
  if (now - lastFrameLedMs > 300) {
    if (now - lastLedToggleMs >= 500) {
      lastLedToggleMs = now;
      ledLevel = !ledLevel;
      digitalWrite(LED_PIN, ledLevel);
    }
  }
}

// ---------------------------------------------------------------------------
void flushDirty() {
  for (int id = 0; id < MAX_ID; id++) {
    Entry &e = table[id];
    if (!e.dirty) continue;

    Serial.print("{\"evt\":\"frame\",\"id\":\"");
    printHexN(id, 3);
    Serial.print("\",\"len\":"); Serial.print(e.len);
    Serial.print(",\"d\":\"");
    for (int j = 0; j < e.len; j++) {
      if (e.data[j] < 16) Serial.print("0");
      Serial.print(e.data[j], HEX);
      if (j < e.len - 1) Serial.print(" ");
    }
    Serial.print("\",\"cnt\":"); Serial.print(e.count);
    Serial.print(",\"t\":"); Serial.print(e.lastSeen);
    Serial.println("}");

    e.dirty = false;
  }
}

void printHexN(uint32_t v, int digits) {
  char fmt[8]; snprintf(fmt, sizeof(fmt), "%%0%dX", digits);
  char buf[12]; snprintf(buf, sizeof(buf), fmt, v);
  Serial.print(buf);
}

// ---------------------------------------------------------------------------
// Allow-list de SIDs UDS/OBD2 permitidos en modo activo. Ver aviso al
// principio del archivo antes de tocar esta lista.
// ---------------------------------------------------------------------------
bool sidAllowed(uint8_t sid) {
  const uint8_t allowed[] = {0x01, 0x03, 0x09, 0x10, 0x14, 0x19, 0x22, 0x3E, 0x04};
  for (unsigned i = 0; i < sizeof(allowed); i++) if (allowed[i] == sid) return true;
  return false;
}

// ---------------------------------------------------------------------------
// Petición UDS/OBD2 genérica: manda payload (<=7 bytes) a reqId como single
// frame, espera respuesta en respId (single o multi-frame ISO-TP). Devuelve
// los bytes de respuesta TAL CUAL (incluido el SID de respuesta) — este
// firmware no decodifica nada, eso es trabajo de la web app.
// ---------------------------------------------------------------------------
int udsRequest(uint32_t reqId, uint32_t respId, uint8_t *payload, int payLen, uint8_t *out, int outMax, unsigned long timeoutMs) {
  if (payLen < 1 || payLen > 7) return -2;

  uint8_t tx[8] = {0};
  tx[0] = (uint8_t) payLen;
  for (int i = 0; i < payLen; i++) tx[1 + i] = payload[i];
  CAN.sendMsgBuf(reqId, 0, payLen + 1, tx);

  unsigned long t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (CAN.checkReceive() == CAN_MSGAVAIL) {
      unsigned long rxId; uint8_t len; uint8_t buf[8];
      CAN.readMsgBuf(&rxId, &len, buf);
      if (rxId != respId) continue;

      uint8_t pci = buf[0] & 0xF0;
      if (pci == 0x00) {                         // Single Frame
        uint8_t sf_len = buf[0] & 0x0F;
        int n = 0;
        for (int i = 1; i < 1 + sf_len && n < outMax; i++) out[n++] = buf[i];
        return n;
      } else if (pci == 0x10) {                  // First Frame (multi-frame)
        return isotpReceiveGeneric(rxId, reqId, buf, out, outMax);
      }
      // Flow Control (0x30) u otros PCI en el sentido de bajada: se ignoran.
    }
  }
  return -1;  // timeout
}

int isotpReceiveGeneric(uint32_t rxId, uint32_t reqId, uint8_t *ff, uint8_t *out, int outMax) {
  int totalLen = ((ff[0] & 0x0F) << 8) | ff[1];
  uint8_t assembled[128]; int a = 0;
  for (int i = 2; i < 8 && a < 128; i++) assembled[a++] = ff[i];

  uint8_t fc[8] = {0x30, 0x00, 0x00, 0, 0, 0, 0, 0};
  CAN.sendMsgBuf(reqId, 0, 8, fc);   // Flow Control (Clear To Send) al módulo

  unsigned long t0 = millis();
  while (a < totalLen && millis() - t0 < 200) {
    if (CAN.checkReceive() == CAN_MSGAVAIL) {
      unsigned long id; uint8_t len; uint8_t buf[8];
      CAN.readMsgBuf(&id, &len, buf);
      if (id != rxId) continue;
      if ((buf[0] & 0xF0) == 0x20) {             // Consecutive Frame
        for (int i = 1; i < 8 && a < 128; i++) assembled[a++] = buf[i];
      }
    }
  }
  int n = 0;
  for (int i = 0; i < a && n < outMax; i++) out[n++] = assembled[i];
  return n;
}

// ---------------------------------------------------------------------------
// Barrido de IDs de diagnóstico (Fase 2): DiagnosticSessionControl (10 03)
// a cada ID del rango, reporta los que responden en id+8.
// ---------------------------------------------------------------------------
void doSweep(uint32_t startId, uint32_t endId) {
  if (endId > 0x7FF) endId = 0x7FF;
  sweepAbort = false;
  Serial.println("{\"evt\":\"sweep_start\"}");

  uint8_t sid10[2] = {0x10, 0x03};
  uint32_t n = 0;
  for (uint32_t id = startId; id <= endId; id++) {
    if (sweepAbort) break;

    uint32_t respId = id + 8;
    uint8_t out[32];
    int r = udsRequest(id, respId, sid10, 2, out, 32, 30);
    if (r > 0) {
      Serial.print("{\"evt\":\"sweep_hit\",\"reqId\":\"");
      printHexN(id, 3);
      Serial.print("\",\"respId\":\"");
      printHexN(respId, 3);
      Serial.print("\",\"raw\":\"");
      for (int i = 0; i < r; i++) {
        if (out[i] < 16) Serial.print("0");
        Serial.print(out[i], HEX);
        if (i < r - 1) Serial.print(" ");
      }
      Serial.println("\"}");
    }

    if ((++n % 32) == 0) {
      Serial.print("{\"evt\":\"sweep_progress\",\"lastId\":\"");
      printHexN(id, 3);
      Serial.println("\"}");
    }

    // deja hueco para detectar SWEEPSTOP entre IDs
    if (Serial.available()) handleSerialCommands();
  }

  Serial.println(sweepAbort ? "{\"evt\":\"sweep_aborted\"}" : "{\"evt\":\"sweep_done\"}");
}

// ---------------------------------------------------------------------------
void handleSerialCommands() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd.length() == 0) return;

  if (cmd == "RESET") {
    memset(table, 0, sizeof(table));
    totalFrames = 0;
    Serial.println("{\"evt\":\"reset\"}");

  } else if (cmd == "STATS") {
    int distinct = 0;
    for (int id = 0; id < MAX_ID; id++) if (table[id].seen) distinct++;
    Serial.print("{\"evt\":\"stats\",\"distinct\":"); Serial.print(distinct);
    Serial.print(",\"total\":"); Serial.print(totalFrames);
    Serial.println("}");

  } else if (cmd == "MODE:ACTIVE") {
    CAN.setMode(MCP_NORMAL);
    mode = MODE_ACTIVE;
    Serial.println("{\"evt\":\"mode\",\"active\":true,\"value\":\"active\"}");

  } else if (cmd == "MODE:PASSIVE") {
    CAN.setMode(MCP_LISTENONLY);
    mode = MODE_PASSIVE;
    Serial.println("{\"evt\":\"mode\",\"active\":false,\"value\":\"passive\"}");

  } else if (cmd == "MODE:LOOPBACK") {
    CAN.setMode(MCP_LOOPBACK);
    mode = MODE_LOOPBACK;
    Serial.println("{\"evt\":\"mode\",\"active\":true,\"value\":\"loopback\"}");

  } else if (cmd == "SWEEPSTOP") {
    sweepAbort = true;

  } else if (cmd.startsWith("UDS:")) {
    if (mode == MODE_PASSIVE) {
      Serial.println("{\"evt\":\"error\",\"msg\":\"UDS requiere MODE:ACTIVE (o MODE:LOOPBACK para pruebas)\"}");
      return;
    }
    String rest = cmd.substring(4);
    int c1 = rest.indexOf(':');
    int c2 = (c1 < 0) ? -1 : rest.indexOf(':', c1 + 1);
    if (c1 < 0 || c2 < 0) {
      Serial.println("{\"evt\":\"error\",\"msg\":\"formato UDS invalido, usa UDS:reqId:respId:bytesHex\"}");
      return;
    }
    uint32_t reqId  = strtoul(rest.substring(0, c1).c_str(), NULL, 16);
    uint32_t respId = strtoul(rest.substring(c1 + 1, c2).c_str(), NULL, 16);
    String hexPart = rest.substring(c2 + 1);
    hexPart.replace(" ", "");

    uint8_t payload[16]; int payLen = 0;
    for (unsigned i = 0; i + 1 < hexPart.length() && payLen < 16; i += 2) {
      char b[3] = { hexPart[i], hexPart[i + 1], 0 };
      payload[payLen++] = (uint8_t) strtoul(b, NULL, 16);
    }
    if (payLen < 1 || payLen > 7) {
      Serial.println("{\"evt\":\"error\",\"msg\":\"payload UDS debe ser de 1 a 7 bytes\"}");
      return;
    }
    if (!sidAllowed(payload[0])) {
      Serial.print("{\"evt\":\"error\",\"msg\":\"SID no permitido: 0x");
      if (payload[0] < 16) Serial.print("0");
      Serial.print(payload[0], HEX);
      Serial.println("\"}");
      return;
    }

    uint8_t out[128];
    int n = udsRequest(reqId, respId, payload, payLen, out, 128, 100);
    Serial.print("{\"evt\":\"uds\",\"req\":\""); printHexN(reqId, 3);
    Serial.print("\",\"resp\":\""); printHexN(respId, 3);
    Serial.print("\",\"raw\":\"");
    if (n < 0) Serial.print("timeout");
    else {
      for (int i = 0; i < n; i++) {
        if (out[i] < 16) Serial.print("0");
        Serial.print(out[i], HEX);
        if (i < n - 1) Serial.print(" ");
      }
    }
    Serial.println("\"}");

  } else if (cmd.startsWith("SWEEP:")) {
    if (mode == MODE_PASSIVE) {
      Serial.println("{\"evt\":\"error\",\"msg\":\"SWEEP requiere MODE:ACTIVE (o MODE:LOOPBACK para pruebas)\"}");
      return;
    }
    String rest = cmd.substring(6);
    int c1 = rest.indexOf(':');
    if (c1 < 0) {
      Serial.println("{\"evt\":\"error\",\"msg\":\"formato SWEEP invalido, usa SWEEP:startId:endId\"}");
      return;
    }
    uint32_t startId = strtoul(rest.substring(0, c1).c_str(), NULL, 16);
    uint32_t endId   = strtoul(rest.substring(c1 + 1).c_str(), NULL, 16);
    doSweep(startId, endId);
  }
}
