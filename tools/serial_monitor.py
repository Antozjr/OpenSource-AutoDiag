#!/usr/bin/env python3
"""Monitor serie simple para el ESP32 (alternativa a `pio device monitor`
cuando no hay terminal interactiva disponible).

Uso:
    python3 tools/serial_monitor.py [puerto] [baudios]

Por defecto: puerto /dev/ttyUSB0, 115200 baudios.
Para salir: Ctrl+C
"""
import sys
import time

import serial

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

    ser = serial.Serial(port, baud, timeout=1)
    print(f"Escuchando {port} @ {baud} baudios (Ctrl+C para salir)")
    try:
        while True:
            line = ser.readline()
            if line:
                sys.stdout.write(line.decode(errors="replace"))
                sys.stdout.flush()
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()

if __name__ == "__main__":
    main()
