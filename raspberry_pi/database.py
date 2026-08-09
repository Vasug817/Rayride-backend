import sqlite3
import os
import time

DB_PATH = os.path.join(os.path.dirname(__file__), "ems_history.db")

def init_db(db_path=DB_PATH):
    conn = sqlite3.connect(db_path)
    cursor = conn.cursor()
    
    # Create telemetry table
    cursor.execute("""
    CREATE TABLE IF NOT EXISTS telemetry (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        timestamp REAL,
        soc INTEGER,
        soh INTEGER,
        state TEXT,
        mode TEXT,
        fault TEXT,
        batt_voltage REAL,
        batt_current REAL,
        batt_temp REAL,
        solar_voltage REAL,
        solar_power REAL,
        fan_duty REAL,
        solar_wh REAL,
        charge_wh REAL,
        consumed_wh REAL,
        net_ah REAL
    )
    """)
    
    # Create faults log table
    cursor.execute("""
    CREATE TABLE IF NOT EXISTS faults (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        timestamp REAL,
        fault_code INTEGER,
        fault_name TEXT,
        severity TEXT,
        status TEXT
    )
    """)
    
    conn.commit()
    conn.close()

def insert_telemetry(data, db_path=DB_PATH):
    conn = sqlite3.connect(db_path)
    cursor = conn.cursor()
    cursor.execute("""
    INSERT INTO telemetry (
        timestamp, soc, soh, state, mode, fault,
        batt_voltage, batt_current, batt_temp,
        solar_voltage, solar_power, fan_duty,
        solar_wh, charge_wh, consumed_wh, net_ah
    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        data.get("timestamp", time.time()),
        data.get("soc", 0),
        data.get("soh", 0),
        data.get("state", "UNKNOWN"),
        data.get("mode", "UNKNOWN"),
        data.get("fault", "None"),
        data.get("battery", {}).get("voltage", 0.0),
        data.get("battery", {}).get("current", 0.0),
        data.get("battery", {}).get("temp", 0.0),
        data.get("solar", {}).get("voltage", 0.0),
        data.get("solar", {}).get("power", 0.0),
        data.get("cooling", {}).get("fan_duty", 0.0),
        data.get("energy", {}).get("solar_wh", 0.0),
        data.get("energy", {}).get("charge_wh", 0.0),
        data.get("energy", {}).get("consumed_wh", 0.0),
        data.get("energy", {}).get("net_ah", 0.0)
    ))
    conn.commit()
    conn.close()

def insert_fault(fault_code, fault_name, severity, status, db_path=DB_PATH):
    conn = sqlite3.connect(db_path)
    cursor = conn.cursor()
    cursor.execute("""
    INSERT INTO faults (timestamp, fault_code, fault_name, severity, status)
    VALUES (?, ?, ?, ?, ?)
    """, (time.time(), fault_code, fault_name, severity, status))
    conn.commit()
    conn.close()

def get_latest_status(db_path=DB_PATH):
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    cursor = conn.cursor()
    cursor.execute("SELECT * FROM telemetry ORDER BY id DESC LIMIT 1")
    row = cursor.fetchone()
    conn.close()
    if row:
        return dict(row)
    return None

def get_active_faults(db_path=DB_PATH):
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    cursor = conn.cursor()
    # Find any active fault entries in history that were not followed by a CLEARED entry for the same code
    cursor.execute("""
        SELECT f1.* FROM faults f1
        WHERE f1.status = 'LATCHED'
        AND NOT EXISTS (
            SELECT 1 FROM faults f2
            WHERE f2.fault_code = f1.fault_code
            AND f2.status = 'CLEARED'
            AND f2.id > f1.id
        )
        ORDER BY f1.id DESC
    """)
    rows = cursor.fetchall()
    conn.close()
    return [dict(r) for r in rows]

def get_fault_history(db_path=DB_PATH, limit=50):
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    cursor = conn.cursor()
    cursor.execute("SELECT * FROM faults ORDER BY id DESC LIMIT ?", (limit,))
    rows = cursor.fetchall()
    conn.close()
    return [dict(r) for r in rows]

def get_historical_telemetry(start_time, end_time, db_path=DB_PATH):
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    cursor = conn.cursor()
    cursor.execute("""
        SELECT * FROM telemetry 
        WHERE timestamp >= ? AND timestamp <= ? 
        ORDER BY id ASC
    """, (start_time, end_time))
    rows = cursor.fetchall()
    conn.close()
    return [dict(r) for r in rows]

# Initialize db on import
init_db()
