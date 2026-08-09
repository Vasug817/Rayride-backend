import json
import time
import urllib.request
import urllib.parse
import threading
import paho.mqtt.client as mqtt

SSE_URL = "http://localhost:8080/events"
MQTT_HOST = "127.0.0.1"
MQTT_PORT = 1883

def mqtt_thread_func():
    def on_connect(client, userdata, flags, rc, properties=None):
        if rc == 0:
            print("[Bridge] Connected to MQTT broker.")
            client.subscribe("ems/commands")
            client.subscribe("ems/config")
            
    def on_message(client, userdata, msg):
        topic = msg.topic
        payload_str = msg.payload.decode('utf-8', errors='ignore')
        try:
            data = json.loads(payload_str)
            if data.get("source") == "gateway":
                return
        except Exception:
            pass
        print(f"[Bridge] Received command from MQTT [{topic}]: {payload_str}")
        try:
            data = json.loads(payload_str)
            if topic == "ems/commands":
                # Convert back to serial payload structure
                req = urllib.request.Request(
                    "http://localhost:8080/api/command",
                    data=json.dumps(data).encode('utf-8'),
                    headers={'Content-Type': 'application/json', 'X-Source': 'Bridge'}
                )
                with urllib.request.urlopen(req) as resp:
                    print(f"[Bridge] Forwarded command to serial, response: {resp.read().decode()}")
            elif topic == "ems/config":
                # Config manager changes
                param_id = data.get("param_id")
                val = data.get("value")
                import struct
                # Re-pack float to 4 bytes payload
                payload = [param_id] + list(struct.pack('<f', val))
                cmd = {"msg_id": 6, "payload": payload}
                req = urllib.request.Request(
                    "http://localhost:8080/api/command",
                    data=json.dumps(cmd).encode('utf-8'),
                    headers={'Content-Type': 'application/json', 'X-Source': 'Bridge'}
                )
                with urllib.request.urlopen(req) as resp:
                    print(f"[Bridge] Forwarded config change to serial, response: {resp.read().decode()}")
        except Exception as e:
            print(f"[Bridge] Error forwarding command: {e}")

    client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message
    
    while True:
        try:
            client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
            client.loop_forever()
        except Exception as e:
            print(f"[Bridge] MQTT connection failed: {e}. Retrying...")
            time.sleep(2)

def main():
    print("[Bridge] Starting Serial-to-MQTT Bridge...")
    
    # Start MQTT loop in background
    t = threading.Thread(target=mqtt_thread_func, daemon=True)
    t.start()
    
    # Listen to SSE events and bridge to MQTT
    while True:
        try:
            print(f"[Bridge] Connecting to dashboard SSE stream at {SSE_URL}...")
            response = urllib.request.urlopen(SSE_URL, timeout=15)
            print("[Bridge] Connected successfully! Streaming serial telemetry to MQTT...")
            
            # Start publishing client
            mqtt_pub = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
            mqtt_pub.connect(MQTT_HOST, MQTT_PORT)
            
            for line in response:
                line_str = line.decode('utf-8').strip()
                if line_str.startswith("data:"):
                    event_data = json.loads(line_str[5:].strip())
                    
                    if event_data.get("type") == "status_update":
                        # Convert to structured MQTT payload
                        mqtt_payload = {
                            "soc": event_data.get("soc", 0),
                            "soh": event_data.get("battery_soh", 0),
                            "state": event_data.get("state", "UNKNOWN"),
                            "mode": event_data.get("power_mode", "NORMAL"),
                            "fault": "None",
                            "battery": {
                                "voltage": event_data.get("battery_voltage", 0.0),
                                "current": event_data.get("battery_current", 0.0),
                                "temp": event_data.get("battery_temp", 0.0)
                            },
                            "solar": {
                                "voltage": event_data.get("solar_voltage", 0.0),
                                "power": event_data.get("solar_power", 0.0)
                            },
                            "cooling": {
                                "fan_duty": 0.0 # simulated
                            },
                            "energy": {
                                "solar_wh": 0.0,
                                "charge_wh": 0.0,
                                "consumed_wh": 0.0,
                                "net_ah": 0.0
                            }
                        }
                        mqtt_pub.publish("ems/telemetry", json.dumps(mqtt_payload))

                        
        except Exception as e:
            print(f"[Bridge] Stream error: {e}. Reconnecting in 3 seconds...")
            time.sleep(3)

if __name__ == '__main__':
    main()
