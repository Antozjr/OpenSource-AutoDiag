/* ============================================================================
   OpenSource AutoDiag — Sniffer pasivo de F-CAN (solo lectura)
   ----------------------------------------------------------------------------
   Plataforma : ESP32 + módulo MCP2515 (TJA1050) por SPI (ver README.md).
   Bus        : F-CAN 11-bit, 500 kbps (el que ya llega al conector OBD2,
                pines 6/14). El B-CAN de confort no es alcanzable con este
                hardware; no lo intenta este firmware.
   Modo       : MCP2515 en LISTEN-ONLY. A nivel de silicio el controlador NO
                genera bit de ACK ni transmite tramas de ningún tipo — es la
                garantía de que este firmware no puede interferir con el bus
                del coche pase lo que pase en el código. No hay ninguna
                llamada a CAN.sendMsgBuf en todo este archivo.
   Uso        : con el contacto puesto, acciona sistemas del coche (freno,
                intermitente, ventanilla, climatizador...) y mira en la
                interfaz web qué IDs cambian justo después. Así se construye
                el mapa de IDs real de este coche — no existe uno publicado
                para el N22A/FN2.
   Salida     : una línea JSON por cada ID cuyo contenido ha cambiado desde
                la última vez, agrupadas cada ~150 ms para no saturar el
                puerto serie. Por USB a 115200 bps.
   ============================================================================ */

#include <Arduino.h>
#include <SPI.h>
#include <mcp_can.h>
#include <string.h>

#define CAN_CS_PIN   5
#define CAN_INT_PIN  4

// Mismo cristal que el firmware CAN de VGT: si ese te arrancó con MCP_8MHZ,
// deja este igual (es el mismo módulo físico).
#define MCP_CLOCK    MCP_8MHZ

MCP_CAN CAN(CAN_CS_PIN);

void handleSerialCommands();
void flushDirty();
void printHex3(int id);

// Tabla de últimas tramas vistas, indexada por ID de 11 bits (0..0x7FF).
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

// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("{\"evt\":\"boot\",\"fw\":\"SNIFFER\",\"msg\":\"iniciando MCP2515 en modo listen-only...\"}");

  int tries = 0;
  while (CAN.begin(MCP_ANY, CAN_500KBPS, MCP_CLOCK) != CAN_OK) {
    Serial.println("{\"evt\":\"error\",\"msg\":\"MCP2515 no responde. Revisa SPI/cristal/alimentacion.\"}");
    delay(1000);
    if (++tries > 5) break;
  }

  // CLAVE DE SEGURIDAD: listen-only a nivel de controlador CAN. En este modo
  // el MCP2515 es físicamente incapaz de transmitir, ni siquiera un ACK
  // involuntario. No lo cambies a MCP_NORMAL en este firmware.
  CAN.setMode(MCP_LISTENONLY);
  pinMode(CAN_INT_PIN, INPUT);

  memset(table, 0, sizeof(table));

  Serial.println("{\"evt\":\"ready\",\"proto\":\"F-CAN 500k listen-only\"}");
}

// ===========================================================================
void loop() {
  handleSerialCommands();

  while (CAN.checkReceive() == CAN_MSGAVAIL) {
    unsigned long rxId; uint8_t len; uint8_t buf[8];
    CAN.readMsgBuf(&rxId, &len, buf);
    if (rxId >= MAX_ID) continue;   // trama extendida (29-bit); fuera de alcance por ahora

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
void flushDirty() {
  for (int id = 0; id < MAX_ID; id++) {
    Entry &e = table[id];
    if (!e.dirty) continue;

    Serial.print("{\"evt\":\"frame\",\"id\":\"");
    printHex3(id);
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

void printHex3(int id) {
  char buf[4];
  snprintf(buf, sizeof(buf), "%03X", id);
  Serial.print(buf);
}

// ---------------------------------------------------------------------------
// Comandos desde la interfaz web (una línea de texto por comando):
//   RESET   -> vacía la tabla y los contadores (para empezar una prueba limpia)
//   STATS   -> emite un resumen: nº de IDs distintos vistos y frames totales
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
  }
}
