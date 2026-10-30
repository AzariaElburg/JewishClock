"""Restarts the clock with a reset pulse on its USB serial port (RTS -> EN),
like the RESET button. Port: first argument, default COM5.

    C:/pio3/jc_clean/venv/Scripts/python utils/usb_reset.py [COM5]
"""
import sys
import time

import serial

s = serial.Serial()
s.port, s.baudrate = (sys.argv[1] if len(sys.argv) > 1 else "COM5"), 115200
s.dtr = False
s.rts = False
s.open()
s.rts = True
time.sleep(0.2)
s.rts = False
s.close()
print("reset pulse sent")
