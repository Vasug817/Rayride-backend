import serial
import time

def main():
    print("[Check Boot] Opening /dev/cu.usbmodem5C831293851...")
    try:
        s = serial.Serial('/dev/cu.usbmodem5C831293851', 115200, timeout=1)
        
        # Reset ESP32-S3 to normal boot (GPIO0 high, toggle EN low then high)
        print("[Check Boot] Performing normal boot reset...")
        s.dtr = False
        s.rts = True
        time.sleep(0.1)
        s.rts = False
        time.sleep(0.1)
        s.dtr = False
        time.sleep(0.1)
        
        print("[Check Boot] Listening to boot log for 12 seconds...")
        start = time.time()
        buffer = ""
        while time.time() - start < 12:
            n = s.in_waiting
            if n > 0:
                data = s.read(n)
                try:
                    text = data.decode('utf-8', errors='ignore')
                    buffer += text
                    # Print lines as they arrive
                    while "\n" in buffer:
                        line, buffer = buffer.split("\n", 1)
                        # Filter out binary framing noise if any
                        if not any(ord(c) < 32 and c not in "\r\n\t" for c in line):
                            print(f"[ESP32] {line.strip()}")
                except Exception as e:
                    pass
            time.sleep(0.05)
        s.close()
    except Exception as e:
        print(f"[Check Boot] Error: {e}")

if __name__ == '__main__':
    main()
