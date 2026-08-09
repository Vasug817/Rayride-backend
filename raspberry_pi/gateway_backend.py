import json
import time
import sys
import paho.mqtt.client as mqtt
import database
import sqlite3

BROKER_HOST = "127.0.0.1"
BROKER_PORT = 1883

def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        print("[Gateway] Connected to local MQTT Broker successfully.")
        # Subscribe to telemetry, faults, historical, and sync status
        client.subscribe("ems/telemetry")
        client.subscribe("ems/faults")
        client.subscribe("ems/historical")
        client.subscribe("ems/sync")
    else:
        print(f"[Gateway] Connection failed with code {rc}")

last_telemetry_insert_time = 0

def on_message(client, userdata, msg):
    global last_telemetry_insert_time
    topic = msg.topic
    payload_str = msg.payload.decode('utf-8', errors='ignore')
    
    try:
        data = json.loads(payload_str)
    except json.JSONDecodeError:
        print(f"[Gateway] [WARN] Received malformed JSON on topic {topic}: {payload_str}")
        return

    if topic == "ems/telemetry":
        # Validate critical fields
        required_fields = ["soc", "soh", "state", "battery", "solar"]
        if not all(field in data for field in required_fields):
            print(f"[Gateway] [WARN] Telemetry missing required fields: {data}")
            return
            
        print(f"[Gateway] Received telemetry sequence_num={data.get('sequence_num')} state={data.get('state')} SOC={data.get('soc')}%")
        current_time = time.time()
        # Rate limit database writes to once every 5 seconds for live telemetry
        if current_time - last_telemetry_insert_time < 5.0:
            return
        last_telemetry_insert_time = current_time

        # Add timestamp and persist to database
        data["timestamp"] = current_time
        database.insert_telemetry(data)
        
        # Log telemetry briefly
        print(f"[Gateway] Persisted Telemetry: SOC={data['soc']}% State={data['state']} Fault={data.get('fault', 'None')}")
        
    elif topic == "ems/historical":
        # Validate critical fields
        required_fields = ["timestamp_offset", "soc", "state", "battery", "solar"]
        if not all(field in data for field in required_fields):
            print(f"[Gateway] [WARN] Historical telemetry missing required fields: {data}")
            return
            
        # Calculate absolute timestamp
        offset = float(data["timestamp_offset"])
        timestamp = time.time() - offset
        
        # Check for duplicates in the DB (same timestamp +/- 1.0s and same SOC and battery voltage)
        try:
            conn = sqlite3.connect(database.DB_PATH)
            cursor = conn.cursor()
            cursor.execute(
                "SELECT 1 FROM telemetry WHERE ABS(timestamp - ?) < 1.0 AND soc = ? AND ABS(batt_voltage - ?) < 0.1",
                (timestamp, data["soc"], data["battery"].get("voltage", 0.0))
            )
            exists = cursor.fetchone()
            conn.close()
            if exists:
                print(f"[Gateway] Skipped duplicate historical record at offset={offset}s")
                return
        except Exception as db_err:
            print(f"[Gateway] Error checking duplicate: {db_err}")
            
        # Save to database
        data["timestamp"] = timestamp
        database.insert_telemetry(data)
        print(f"[Gateway] Synchronized Historical Telemetry: SOC={data['soc']}% State={data['state']} (offset={offset}s ago)")
        
    elif topic == "ems/sync":
        status = data.get("status")
        count = data.get("count", 0)
        if status == "sync_started":
            print(f"[Gateway] 🔄 Offline data synchronization STARTED. Expecting {count} records...")
        elif status == "sync_completed":
            print("[Gateway] 🔄 Offline data synchronization COMPLETED successfully.")

    elif topic == "ems/faults":
        required_fields = ["fault_code", "fault_name", "severity"]
        if not all(field in data for field in required_fields):
            print(f"[Gateway] [WARN] Fault message missing required fields: {data}")
            return
            
        fault_code = data["fault_code"]
        fault_name = data["fault_name"]
        severity = data["severity"]
        
        # Severity == -1 or "CLEARED" maps to cleared
        if severity == -1 or severity == "CLEARED":
            status = "CLEARED"
            print(f"[Gateway] 🟢 FAULT CLEARED: {fault_name} (Code: {fault_code})")
        else:
            status = "LATCHED"
            print(f"[Gateway] ⚠️ FAULT LATCHED: {fault_name} (Code: {fault_code}, Severity: {severity})")
            
        # Log to faults table
        database.insert_fault(fault_code, fault_name, severity, status)

def main():
    print("[Gateway] Starting RayGlides local EMS gateway backend...")
    
    # Initialize SQLite database
    database.init_db()
    
    client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message
    
    while True:
        try:
            client.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
            break
        except Exception as e:
            print(f"[Gateway] Waiting for broker to start... ({e})")
            time.sleep(2)
            
    client.loop_forever()

if __name__ == '__main__':
    main()
