#!/usr/bin/env python3
import sys
import os
import json
import time
import threading
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

# Import local protocol library
from rayglides_protocol import (
    RayGlidesLink, NackReceived, AckTimeoutError,
    MSG_STATUS_UPDATE, MSG_FAULT_REPORT, MSG_ACK, MSG_NACK,
    decode_frame, MSG_NAMES, NACK_REASON_NAMES
)

# Global variables for state sharing
latest_state = {
    "soc": 0,
    "state": "UNKNOWN",
    "battery_voltage": 0.0,
    "battery_current": 0.0,
    "battery_temp": 0.0,
    "solar_voltage": 0.0,
    "solar_power": 0.0,
    "battery_soh": 0,
    "duty_cycle": 0,
    "active_fault": "None",
    "power_mode": "UNKNOWN",
    "cpu_freq": 160,
    "wifi_status": "DISCONNECTED",
    "last_update": 0.0
}

state_lock = threading.Lock()
clients = []
clients_lock = threading.Lock()
link = None

pending_commands = {}
pending_commands_lock = threading.Lock()
command_seq_counter = 0
seq_lock = threading.Lock()

# Fallback serial port
DEFAULT_PORT = "/dev/cu.usbmodem5C831293851"

def serial_polling_thread(port):
    global link
    while True:
        print(f"[Serial] Connecting to ESP32-S3 on {port}...")
        try:
            link = RayGlidesLink(port=port, baudrate=115200)
            print("[Serial] Thread started successfully. Polling incoming frames...")
            while True:
                frames = link.poll()
                if frames:
                    for frame in frames:
                        decoded = decode_frame(frame)
                        update_global_state(decoded)
                        broadcast_event(decoded)
                        
                        # Bridge serial fault report to MQTT broker
                        if decoded.get("type") == "fault_report":
                            try:
                                import paho.mqtt.publish as publish
                                sev_val = decoded.get("severity")
                                if sev_val == 255 or sev_val == -1:
                                    sev_str = "CLEARED"
                                elif sev_val == 1:
                                    sev_str = "CRITICAL"
                                else:
                                    sev_str = "WARNING"
                                
                                fault_code = decoded.get("fault_code", 0)
                                fault_names = {
                                    1: "Battery Not Detected",
                                    2: "Battery Over-Voltage",
                                    3: "Battery Under-Voltage",
                                    4: "Battery Over-Temperature",
                                    5: "Solar Over-Voltage",
                                    6: "Solar Reverse Polarity",
                                    7: "MPPT Over-Temperature",
                                    8: "Comm Timeout",
                                    9: "Checksum Error",
                                    10: "Sensor Failure",
                                    11: "Battery Over-Current",
                                    12: "Thermal Shutdown",
                                    13: "Watchdog Lockout",
                                    14: "Watchdog Reset Recovered"
                                }
                                fault_name = fault_names.get(fault_code, "Unknown Fault")
                                
                                mqtt_payload = {
                                    "fault_code": fault_code,
                                    "fault_name": f"F0{fault_code:02d} {fault_name}",
                                    "severity": sev_str
                                }
                                publish.single("ems/faults", json.dumps(mqtt_payload), hostname="127.0.0.1", port=1883)
                                print(f"[Dashboard] Bridged serial fault F0{fault_code:02d} ({sev_str}) to MQTT.")
                            except Exception as pe:
                                print(f"[Dashboard] Failed to publish serial fault to MQTT: {pe}")
                time.sleep(0.05)
        except Exception as e:
            print(f"[Serial] Error or disconnect: {e}")
            try:
                if 'link' in globals() and link is not None:
                    link.close()
            except Exception:
                pass
            link = None
            time.sleep(2)

def update_global_state(decoded):
    global latest_state
    with state_lock:
        latest_state["last_update"] = time.time()
        dtype = decoded.get("type")
        if dtype == "status_update":
            latest_state["soc"] = decoded.get("soc", 0)
            latest_state["battery_voltage"] = decoded.get("battery_voltage", 0)
            latest_state["battery_current"] = decoded.get("battery_current", 0)
            latest_state["battery_temp"] = decoded.get("battery_temp", 0)
            latest_state["solar_voltage"] = decoded.get("solar_voltage", 0)
            latest_state["solar_power"] = decoded.get("solar_power", 0)
            latest_state["battery_soh"] = decoded.get("battery_soh", 0)
            
            # Map state enum to name
            state_enum = decoded.get("state", 0)
            if isinstance(state_enum, int):
                state_names = ["BOOT", "SELF_TEST", "IDLE", "SOLAR_AVAILABLE", "CHARGING", "FULLY_CHARGED", "FAULT", "NO_CHARGE"]
                if state_enum < len(state_names):
                    latest_state["state"] = state_names[state_enum]
                else:
                    latest_state["state"] = f"STATE_{state_enum}"
            else:
                latest_state["state"] = str(state_enum)
                
            # Map active fault code if present
            if "active_fault_code" in decoded:
                code = decoded["active_fault_code"]
                if code == 0:
                    latest_state["active_fault"] = "None"
                else:
                    sev_str = "WARNING" if code in (5, 6) else "CRITICAL"
                    latest_state["active_fault"] = f"F0{code:02d} (Severity: {sev_str})"

        elif dtype == "fault_report":
            code = decoded.get("fault_code", 0)
            sev = decoded.get("severity", 0)
            if code == 0 or sev == 255 or sev == -1:
                latest_state["active_fault"] = "None"
            else:
                latest_state["active_fault"] = f"F0{code:02d} (Severity: {'CRITICAL' if sev==1 else 'WARNING'})"

def broadcast_event(data):
    with clients_lock:
        active_clients = []
        for q in clients:
            try:
                q.put(data)
                active_clients.append(q)
            except Exception:
                pass
        clients[:] = active_clients

# Beautiful responsive glassmorphic UI code
HTML_CONTENT = """<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>RayGlides EMS Real-Time Dashboard</title>
    <link href="https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;600;700&family=JetBrains+Mono&display=swap" rel="stylesheet">
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        :root {
            --bg-color: #0b0f19;
            --card-bg: rgba(17, 24, 39, 0.7);
            --border-color: rgba(255, 255, 255, 0.08);
            --text-primary: #f8fafc;
            --text-secondary: #94a3b8;
            --accent-green: #10b981;
            --accent-blue: #3b82f6;
            --accent-red: #ef4444;
            --accent-yellow: #f59e0b;
        }

        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }

        body {
            font-family: 'Outfit', sans-serif;
            background-color: var(--bg-color);
            background-image: radial-gradient(circle at 10% 20%, rgba(59, 130, 246, 0.08) 0%, transparent 40%),
                              radial-gradient(circle at 90% 80%, rgba(16, 185, 129, 0.08) 0%, transparent 40%);
            color: var(--text-primary);
            min-height: 100vh;
            display: flex;
            flex-direction: column;
            overflow-x: hidden;
        }

        /* Top Header */
        header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding: 1.5rem 2rem;
            background: rgba(15, 23, 42, 0.4);
            backdrop-filter: blur(12px);
            border-bottom: 1px solid var(--border-color);
            position: sticky;
            top: 0;
            z-index: 10;
        }

        .logo-section h1 {
            font-size: 1.5rem;
            font-weight: 700;
            background: linear-gradient(135deg, #60a5fa 0%, #34d399 100%);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            letter-spacing: -0.5px;
        }

        .logo-section p {
            font-size: 0.8rem;
            color: var(--text-secondary);
        }

        .status-badge {
            display: flex;
            align-items: center;
            gap: 0.5rem;
            padding: 0.5rem 1rem;
            background: rgba(255, 255, 255, 0.03);
            border: 1px solid var(--border-color);
            border-radius: 50px;
            font-size: 0.85rem;
            font-weight: 600;
        }

        .pulse-dot {
            width: 8px;
            height: 8px;
            background-color: var(--accent-green);
            border-radius: 50%;
            box-shadow: 0 0 10px var(--accent-green);
            animation: pulse 1.5s infinite;
        }

        @keyframes pulse {
            0% { transform: scale(0.9); opacity: 0.6; }
            50% { transform: scale(1.1); opacity: 1; }
            100% { transform: scale(0.9); opacity: 0.6; }
        }

        /* Container Layout */
        .container {
            max-width: 1400px;
            width: 100%;
            margin: 2rem auto;
            padding: 0 1.5rem;
            display: grid;
            grid-template-columns: 1fr 350px;
            gap: 1.5rem;
            flex-grow: 1;
        }

        /* Cards Grid */
        .dashboard-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
            gap: 1.25rem;
            align-content: start;
        }

        .card {
            background: var(--card-bg);
            border: 1px solid var(--border-color);
            border-radius: 16px;
            padding: 1.5rem;
            backdrop-filter: blur(16px);
            transition: all 0.3s cubic-bezier(0.4, 0, 0.2, 1);
            display: flex;
            flex-direction: column;
            justify-content: space-between;
            min-height: 160px;
        }

        .card:hover {
            transform: translateY(-4px);
            border-color: rgba(255, 255, 255, 0.15);
            box-shadow: 0 10px 20px rgba(0, 0, 0, 0.2);
        }

        .card-header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            color: var(--text-secondary);
            font-size: 0.9rem;
            font-weight: 600;
            margin-bottom: 1rem;
        }

        .card-value {
            font-size: 2.25rem;
            font-weight: 700;
            margin-bottom: 0.5rem;
            letter-spacing: -1px;
        }

        .card-footer {
            font-size: 0.8rem;
            color: var(--text-secondary);
            display: flex;
            align-items: center;
            gap: 0.4rem;
        }

        /* Special Large Card for SOC */
        .card-large {
            grid-column: span 2;
            display: flex;
            flex-direction: row;
            align-items: center;
            justify-content: space-between;
            padding: 2rem;
            min-height: 200px;
        }

        .soc-radial {
            position: relative;
            width: 120px;
            height: 120px;
            display: flex;
            align-items: center;
            justify-content: center;
        }

        .soc-radial svg {
            width: 100%;
            height: 100%;
            transform: rotate(-90deg);
        }

        .soc-radial circle {
            fill: none;
            stroke-width: 10;
        }

        .soc-radial .bg {
            stroke: rgba(255, 255, 255, 0.05);
        }

        .soc-radial .progress {
            stroke: url(#socGrad);
            stroke-dasharray: 314;
            stroke-dashoffset: 314;
            stroke-linecap: round;
            transition: stroke-dashoffset 0.8s ease;
        }

        .soc-text {
            position: absolute;
            font-size: 1.75rem;
            font-weight: 700;
            display: flex;
            flex-direction: column;
            align-items: center;
        }

        .soc-text span {
            font-size: 0.75rem;
            color: var(--text-secondary);
            font-weight: 400;
        }

        /* Side Panels */
        aside {
            display: flex;
            flex-direction: column;
            gap: 1.5rem;
        }

        .panel {
            background: var(--card-bg);
            border: 1px solid var(--border-color);
            border-radius: 16px;
            padding: 1.5rem;
            backdrop-filter: blur(16px);
        }

        .panel h2 {
            font-size: 1.1rem;
            font-weight: 700;
            margin-bottom: 1rem;
            border-bottom: 1px solid var(--border-color);
            padding-bottom: 0.5rem;
        }

        /* Commands / Interactive */
        .cmd-group {
            display: flex;
            flex-direction: column;
            gap: 0.75rem;
        }

        .btn {
            background: linear-gradient(135deg, rgba(59, 130, 246, 0.2) 0%, rgba(37, 99, 235, 0.3) 100%);
            border: 1px solid rgba(59, 130, 246, 0.4);
            color: var(--text-primary);
            border-radius: 8px;
            padding: 0.75rem 1rem;
            cursor: pointer;
            font-family: inherit;
            font-size: 0.9rem;
            font-weight: 600;
            transition: all 0.2s ease;
            text-align: center;
        }

        .btn:hover {
            background: linear-gradient(135deg, rgba(59, 130, 246, 0.3) 0%, rgba(37, 99, 235, 0.5) 100%);
            transform: translateY(-2px);
            box-shadow: 0 4px 12px rgba(59, 130, 246, 0.3);
        }

        .btn-red {
            background: linear-gradient(135deg, rgba(239, 68, 68, 0.2) 0%, rgba(220, 38, 38, 0.3) 100%);
            border: 1px solid rgba(239, 68, 68, 0.4);
        }

        .btn-red:hover {
            background: linear-gradient(135deg, rgba(239, 68, 68, 0.3) 0%, rgba(220, 38, 38, 0.5) 100%);
            box-shadow: 0 4px 12px rgba(239, 68, 68, 0.3);
        }

        /* Logs Console */
        .console {
            background: rgba(10, 15, 26, 0.85);
            border: 1px solid var(--border-color);
            border-radius: 8px;
            padding: 0.75rem;
            height: 250px;
            overflow-y: auto;
            font-family: 'JetBrains Mono', monospace;
            font-size: 0.75rem;
            line-height: 1.4;
            display: flex;
            flex-direction: column-reverse;
            gap: 0.4rem;
        }

        .log-entry {
            border-left: 2px solid var(--accent-blue);
            padding-left: 0.5rem;
        }

        .log-entry.status_update { border-color: var(--accent-green); }
        .log-entry.fault_report { border-color: var(--accent-red); }
        .log-entry.cmd_sent { border-color: var(--accent-yellow); }
        .log-entry.error { border-color: var(--accent-red); color: var(--accent-red); }

        .log-time {
            color: var(--text-secondary);
            margin-right: 0.4rem;
        }

        .stat-grid {
            display: grid;
            grid-template-columns: repeat(2, 1fr);
            gap: 1rem;
            margin-bottom: 1.5rem;
        }

        .stat-item {
            background: rgba(255, 255, 255, 0.02);
            border: 1px solid var(--border-color);
            border-radius: 8px;
            padding: 0.75rem;
        }

        .stat-label {
            font-size: 0.75rem;
            color: var(--text-secondary);
            margin-bottom: 0.25rem;
        }

        .stat-val {
            font-size: 1.1rem;
            font-weight: 700;
        }
    </style>
</head>
<body>

    <header>
        <div class="logo-section">
            <h1>RayGlides EMS</h1>
            <p>Advanced Energy Management System Gateway</p>
        </div>
        <div class="status-badge">
            <div class="pulse-dot"></div>
            <span>LIVE GATEWAY LINK</span>
        </div>
    </header>

    <div class="container">
        <!-- Left Side Dashboard -->
        <main class="dashboard-grid">
            
            <!-- Battery Summary Card (Large) -->
            <div class="card card-large">
                <div style="display: flex; flex-direction: column; justify-content: space-between; height: 100%;">
                    <div>
                        <h3 style="font-size: 1.25rem; font-weight: 700; margin-bottom: 0.5rem;">Battery Pack Status</h3>
                        <p style="color: var(--text-secondary); font-size: 0.9rem;">State of Health: <span id="soh-val">100</span>%</p>
                    </div>
                    <div>
                        <span id="state-badge" style="background: rgba(16,185,129,0.15); border: 1px solid var(--accent-green); color: var(--accent-green); padding: 0.35rem 0.75rem; border-radius: 50px; font-weight: 600; font-size: 0.85rem;">CHARGING</span>
                    </div>
                </div>
                <div class="soc-radial">
                    <svg viewBox="0 0 120 120">
                        <defs>
                            <linearGradient id="socGrad" x1="0%" y1="0%" x2="100%" y2="100%">
                                <stop offset="0%" stop-color="#3b82f6" />
                                <stop offset="100%" stop-color="#10b981" />
                            </linearGradient>
                        </defs>
                        <circle class="bg" cx="60" cy="60" r="50" />
                        <circle class="progress" id="soc-progress-circle" cx="60" cy="60" r="50" />
                    </svg>
                    <div class="soc-text">
                        <span id="soc-val">0</span>%
                        <span>SOC</span>
                    </div>
                </div>
            </div>

            <!-- Voltage Card -->
            <div class="card">
                <div class="card-header">
                    <span>BATTERY VOLTAGE</span>
                    <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M13 2L3 14h9l-1 8 10-12h-9l1-8z"/></svg>
                </div>
                <div>
                    <div class="card-value"><span id="v-val">0.0</span>V</div>
                </div>
                <div class="card-footer">
                    <span>Pack Total Potential</span>
                </div>
            </div>

            <!-- Current Card -->
            <div class="card">
                <div class="card-header">
                    <span>CHARGE CURRENT</span>
                    <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 2v20M17 5H9.5a3.5 3.5 0 0 0 0 7h5a3.5 3.5 0 0 1 0 7H6"/></svg>
                </div>
                <div>
                    <div class="card-value"><span id="i-val">0.0</span>A</div>
                </div>
                <div class="card-footer">
                    <span id="current-direction">Charging Pack</span>
                </div>
            </div>

            <!-- Solar Power Card -->
            <div class="card">
                <div class="card-header">
                    <span>SOLAR GENERATION</span>
                    <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="5"/><path d="M12 1v2M12 21v2M4.22 4.22l1.42 1.42M18.36 18.36l1.42 1.42M1 12h2M21 12h2M4.22 19.78l1.42-1.42M18.36 5.64l1.42-1.42"/></svg>
                </div>
                <div>
                    <div class="card-value"><span id="solar-w-val">0.0</span>W</div>
                </div>
                <div class="card-footer">
                    <span>Solar Input: <span id="solar-v-val">0.0</span>V</span>
                </div>
            </div>

            <!-- Temperature Card -->
            <div class="card">
                <div class="card-header">
                    <span>PACK TEMPERATURE</span>
                    <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 14.76V3.5a2.5 2.5 0 0 0-5 0v11.26a4.5 4.5 0 1 0 5 0z"/></svg>
                </div>
                <div>
                    <div class="card-value"><span id="temp-val">0.0</span>°C</div>
                </div>
                <div class="card-footer">
                    <span id="temp-status">Normal range</span>
                </div>
            </div>

            <!-- Historical Charts Panel (Day 58) -->
            <div class="card" style="grid-column: 1 / -1; min-height: 380px;">
                <div class="card-header" style="border-bottom: 1px solid var(--border-color); padding-bottom: 0.5rem; margin-bottom: 1rem;">
                    <span style="font-weight: 600; font-size: 1rem; color: var(--text-primary);">Historical System Analytics</span>
                    <div style="display: flex; gap: 0.5rem; align-items: center;">
                        <span style="font-size: 0.75rem; color: var(--text-secondary);">Period:</span>
                        <select id="chart-period" onchange="loadHistoricalData()" style="background: rgba(0,0,0,0.5); border: 1px solid var(--border-color); color: white; border-radius: 4px; padding: 0.2rem; font-size: 0.75rem;">
                            <option value="3600">Last 1 Hour</option>
                            <option value="10800">Last 3 Hours</option>
                            <option value="86400">Last 24 Hours</option>
                        </select>
                    </div>
                </div>
                <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 1rem; width: 100%; height: 280px;">
                    <div style="position: relative; height: 100%;">
                        <canvas id="socChart"></canvas>
                    </div>
                    <div style="position: relative; height: 100%;">
                        <canvas id="volCurChart"></canvas>
                    </div>
                    <div style="position: relative; height: 100%;">
                        <canvas id="solarChart"></canvas>
                    </div>
                </div>
            </div>

        </main>

        <!-- Right Side Panel -->
        <aside>
            <!-- System Stats Panel -->
            <div class="panel">
                <h2>System Parameters</h2>
                <div class="stat-grid">
                    <div class="stat-item">
                        <div class="stat-label">Power Profile</div>
                        <div class="stat-val" id="power-mode">NORMAL</div>
                    </div>
                    <div class="stat-item">
                        <div class="stat-label">Active Fault</div>
                        <div class="stat-val" id="active-fault" style="color: var(--accent-green); font-size: 0.95rem;">None</div>
                    </div>
                </div>
            </div>

            <!-- Interactive Command Controls -->
            <div class="panel">
                <h2>EMS Live Command Verification</h2>
                <p style="color: var(--text-secondary); font-size: 0.8rem; margin-bottom: 1rem;">
                    Sends binary commands directly to the ESP32-S3 over the serial link and awaits a matching ACK or NACK back.
                </p>
                <div class="cmd-group">
                    <button class="btn" onclick="sendCommand(2, [0, 0])">Send Clear Faults (0x02)</button>
                    <button class="btn btn-red" onclick="sendCommand(153, [1, 2])">Send Invalid Msg (0x99)</button>
                    <button class="btn btn-red" onclick="sendCorruptedChecksum()">Send Corrupted Checksum</button>
                </div>
            </div>

            <!-- Sensor Simulator Scenario Control (Day 44) -->
            <div class="panel">
                <h2>Sensor Simulation Control (Day 44)</h2>
                <p style="color: var(--text-secondary); font-size: 0.8rem; margin-bottom: 1rem;">
                    Trigger preset test scenarios to verify protections and transitions.
                </p>
                <div class="cmd-group" style="display: grid; grid-template-columns: repeat(2, 1fr); gap: 0.5rem;">
                    <button class="btn" style="font-size: 0.8rem;" onclick="sendCommand(5, [0])">Normal Ops</button>
                    <button class="btn" style="font-size: 0.8rem;" onclick="sendCommand(5, [1])">Chg Ramp</button>
                    <button class="btn btn-red" style="font-size: 0.8rem;" onclick="sendCommand(5, [2])">Low Batt</button>
                    <button class="btn" style="font-size: 0.8rem;" onclick="sendCommand(5, [3])">Full Batt</button>
                    <button class="btn btn-red" style="font-size: 0.8rem;" onclick="sendCommand(5, [4])">Overheat</button>
                    <button class="btn btn-red" style="font-size: 0.8rem;" onclick="sendCommand(5, [5])">Overcurrent</button>
                    <button class="btn" style="font-size: 0.8rem;" onclick="sendCommand(5, [6])">No Solar</button>
                    <button class="btn btn-red" style="font-size: 0.8rem;" onclick="sendCommand(5, [7])">Sensor Fail</button>
                </div>
            </div>

            <!-- EMS Config Manager (Day 42) -->
            <div class="panel">
                <h2>EMS Config Parameters (Day 42)</h2>
                <p style="color: var(--text-secondary); font-size: 0.8rem; margin-bottom: 1rem;">
                    Adjust charging and safety thresholds dynamically inside NVS memory.
                </p>
                <div style="display: flex; flex-direction: column; gap: 0.6rem; font-size: 0.85rem;">
                    <div style="display: flex; justify-content: space-between; align-items: center;">
                        <span>Batt OverV (V)</span>
                        <div style="display: flex; gap: 0.25rem; align-items: center;">
                            <input id="cfg-ov" type="number" step="0.1" value="68.0" style="width: 55px; background: rgba(0,0,0,0.3); border: 1px solid var(--border-color); color: white; padding: 0.2rem; border-radius: 4px; font-size: 0.8rem; text-align: center;">
                            <button class="btn" style="padding: 0.2rem 0.5rem; font-size: 0.75rem;" onclick="sendConfig(1, 'cfg-ov')">Set</button>
                        </div>
                    </div>
                    <div style="display: flex; justify-content: space-between; align-items: center;">
                        <span>Batt UnderV (V)</span>
                        <div style="display: flex; gap: 0.25rem; align-items: center;">
                            <input id="cfg-uv" type="number" step="0.1" value="42.0" style="width: 55px; background: rgba(0,0,0,0.3); border: 1px solid var(--border-color); color: white; padding: 0.2rem; border-radius: 4px; font-size: 0.8rem; text-align: center;">
                            <button class="btn" style="padding: 0.2rem 0.5rem; font-size: 0.75rem;" onclick="sendConfig(2, 'cfg-uv')">Set</button>
                        </div>
                    </div>
                    <div style="display: flex; justify-content: space-between; align-items: center;">
                        <span>Batt OverI (A)</span>
                        <div style="display: flex; gap: 0.25rem; align-items: center;">
                            <input id="cfg-oc" type="number" step="0.1" value="25.0" style="width: 55px; background: rgba(0,0,0,0.3); border: 1px solid var(--border-color); color: white; padding: 0.2rem; border-radius: 4px; font-size: 0.8rem; text-align: center;">
                            <button class="btn" style="padding: 0.2rem 0.5rem; font-size: 0.75rem;" onclick="sendConfig(3, 'cfg-oc')">Set</button>
                        </div>
                    </div>
                    <div style="display: flex; justify-content: space-between; align-items: center;">
                        <span>Batt OverT (°C)</span>
                        <div style="display: flex; gap: 0.25rem; align-items: center;">
                            <input id="cfg-ot" type="number" step="0.1" value="60.0" style="width: 55px; background: rgba(0,0,0,0.3); border: 1px solid var(--border-color); color: white; padding: 0.2rem; border-radius: 4px; font-size: 0.8rem; text-align: center;">
                            <button class="btn" style="padding: 0.2rem 0.5rem; font-size: 0.75rem;" onclick="sendConfig(4, 'cfg-ot')">Set</button>
                        </div>
                    </div>
                    <div style="display: flex; justify-content: space-between; align-items: center;">
                        <span>Solar Limit (V)</span>
                        <div style="display: flex; gap: 0.25rem; align-items: center;">
                            <input id="cfg-sol" type="number" step="0.1" value="26.0" style="width: 55px; background: rgba(0,0,0,0.3); border: 1px solid var(--border-color); color: white; padding: 0.2rem; border-radius: 4px; font-size: 0.8rem; text-align: center;">
                            <button class="btn" style="padding: 0.2rem 0.5rem; font-size: 0.75rem;" onclick="sendConfig(5, 'cfg-sol')">Set</button>
                        </div>
                    </div>
                </div>
            </div>

            <!-- Log Console -->
            <div class="panel" style="flex-grow: 1; display: flex; flex-direction: column;">
                <h2>Serial Event Monitor</h2>
                <div class="console" id="console">
                    <!-- Dynamic Log Events -->
                </div>
            </div>
        </aside>
    </div>

    <script>
        // Circular progress circle setting
        function setSocProgress(soc) {
            const circle = document.getElementById('soc-progress-circle');
            const radius = circle.r.baseVal.value;
            const circumference = 2 * Math.PI * radius;
            const offset = circumference - (soc / 100) * circumference;
            circle.style.strokeDashoffset = offset;
        }

        // Live Event Listening (SSE)
        const eventSource = new EventSource('/events');
        const consoleEl = document.getElementById('console');

        function addLog(type, text) {
            const entry = document.createElement('div');
            entry.className = `log-entry ${type}`;
            const timeStr = new Date().toLocaleTimeString();
            entry.innerHTML = `<span class="log-time">[${timeStr}]</span>${text}`;
            consoleEl.prepend(entry);
        }

        eventSource.onmessage = function(event) {
            const data = JSON.parse(event.data);
            
            // Dispatch updates
            if (data.type === 'status_update') {
                document.querySelectorAll('#soc-val').forEach(el => el.innerText = data.soc);
                setSocProgress(data.soc);
                document.getElementById('soh-val').innerText = data.battery_soh;
                document.getElementById('v-val').innerText = data.battery_voltage.toFixed(1);
                document.getElementById('i-val').innerText = data.battery_current.toFixed(1);
                document.getElementById('temp-val').innerText = data.battery_temp.toFixed(1);
                document.getElementById('solar-w-val').innerText = data.solar_power;
                document.getElementById('solar-v-val').innerText = data.solar_voltage.toFixed(1);
                
                // Direction indicators
                const dirEl = document.getElementById('current-direction');
                if (data.battery_current < 0) {
                    dirEl.innerText = "Discharging Pack";
                    dirEl.style.color = "var(--accent-blue)";
                } else if (data.battery_current > 0) {
                    dirEl.innerText = "Charging Pack";
                    dirEl.style.color = "var(--accent-green)";
                } else {
                    dirEl.innerText = "Pack Idle";
                    dirEl.style.color = "var(--text-secondary)";
                }

                // Temp status
                const tempEl = document.getElementById('temp-status');
                if (data.battery_temp > 45) {
                    tempEl.innerText = "Over-temperature warning!";
                    tempEl.style.color = "var(--accent-red)";
                } else {
                    tempEl.innerText = "Normal range";
                    tempEl.style.color = "var(--text-secondary)";
                }

                // State colors
                const badge = document.getElementById('state-badge');
                badge.innerText = data.state;
                if (data.state === 'CHARGING') {
                    badge.style.color = "var(--accent-green)";
                    badge.style.borderColor = "var(--accent-green)";
                    badge.style.backgroundColor = "rgba(16,185,129,0.1)";
                } else if (data.state === 'FAULT') {
                    badge.style.color = "var(--accent-red)";
                    badge.style.borderColor = "var(--accent-red)";
                    badge.style.backgroundColor = "rgba(239,68,68,0.1)";
                } else {
                    badge.style.color = "var(--accent-blue)";
                    badge.style.borderColor = "var(--accent-blue)";
                    badge.style.backgroundColor = "rgba(59,130,246,0.1)";
                }

                // Dynamic Clock Mode simulation based on power state
                const modeEl = document.getElementById('power-mode');
                if (data.state === 'CHARGING') {
                    modeEl.innerText = "PERFORMANCE (240MHz)";
                    modeEl.style.color = "var(--accent-yellow)";
                } else {
                    modeEl.innerText = "BALANCED (160MHz)";
                    modeEl.style.color = "var(--accent-blue)";
                }

                addLog('status_update', `RX Telemetry: SOC=${data.soc}% V=${data.battery_voltage}V I=${data.battery_current}A SolarP=${data.solar_power}W State=${data.state}`);
                if (window.pushLiveChartData) {
                    pushLiveChartData(data);
                }
            }
            
            else if (data.type === 'fault_report') {
                const faultEl = document.getElementById('active-fault');
                if (data.fault_code === 0) {
                    faultEl.innerText = "None";
                    faultEl.style.color = "var(--accent-green)";
                } else {
                    faultEl.innerText = `F0${data.fault_code} (Severity: ${data.severity})`;
                    faultEl.style.color = "var(--accent-red)";
                }
                addLog('fault_report', `⚠️ RX FAULT REPORT: Code=${data.fault_code} Severity=${data.severity}`);
            }
        };

        eventSource.onerror = function() {
            addLog('error', 'Gateway lost local event connection.');
        };

        // Interactive Send Command API
        function sendCommand(msgId, payload) {
            const start = performance.now();
            addLog('cmd_sent', `TX command msg_id=0x${msgId.toString(16).toUpperCase().padStart(2, '0')}...`);
            
            fetch('/api/command', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ msg_id: msgId, payload: payload })
            })
            .then(res => res.json())
            .then(data => {
                const latency = (performance.now() - start).toFixed(0);
                if (data.status === 'success') {
                    addLog('status_update', `✔ SUCCESS: Command acknowledged by ESP32 (latency: ${latency}ms)`);
                } else if (data.status === 'error') {
                    if (data.error_type === 'nack') {
                        addLog('error', `❌ REJECTED (NACK): Reason=${data.reason_name} (code: ${data.reason}) (latency: ${latency}ms)`);
                    } else if (data.error_type === 'timeout') {
                        addLog('error', `❌ TIMEOUT: No response from ESP32 (latency: ${latency}ms)`);
                    } else {
                        addLog('error', `❌ ERROR: ${data.message}`);
                    }
                }
            })
            .catch(err => addLog('error', `Failed to contact gateway server: ${err}`));
        }

        function floatToBytes(f) {
            const buf = new ArrayBuffer(4);
            const view = new DataView(buf);
            view.setFloat32(0, f, true);
            return Array.from(new Uint8Array(buf));
        }

        function sendConfig(paramId, elementId) {
            const val = parseFloat(document.getElementById(elementId).value);
            if (isNaN(val)) return;
            const bytes = floatToBytes(val);
            sendCommand(6, [paramId, ...bytes]);
        }

        function sendCorruptedChecksum() {
            const start = performance.now();
            addLog('cmd_sent', `TX checksum-corrupted command...`);
            
            fetch('/api/command_corrupted', {
                method: 'POST'
            })
            .then(res => res.json())
            .then(data => {
                const latency = (performance.now() - start).toFixed(0);
                if (data.status === 'success') {
                    addLog('status_update', `✔ ACK received (Unexpected for corrupted checksum!)`);
                } else if (data.status === 'error') {
                    if (data.error_type === 'nack') {
                        addLog('error', `❌ REJECTED (NACK): Reason=${data.reason_name} (code: ${data.reason}) (latency: ${latency}ms)`);
                    } else {
                        addLog('error', `❌ ERROR: ${data.message}`);
                    }
                }
            })
            .catch(err => addLog('error', `Failed: ${err}`));
        }

        // --- Chart.js Instances (Day 58) ---
        let socChartInstance = null;
        let volCurChartInstance = null;
        let solarChartInstance = null;

        function initCharts() {
            const ctxSoc = document.getElementById('socChart').getContext('2d');
            socChartInstance = new Chart(ctxSoc, {
                type: 'line',
                data: {
                    labels: [],
                    datasets: [{
                        label: 'SOC (%)',
                        data: [],
                        borderColor: '#10b981',
                        backgroundColor: 'rgba(16, 185, 129, 0.1)',
                        fill: true,
                        tension: 0.3
                    }]
                },
                options: {
                    responsive: true,
                    maintainAspectRatio: false,
                    scales: {
                        x: { display: false },
                        y: { min: 0, max: 100, ticks: { color: '#94a3b8' } }
                    },
                    plugins: { legend: { labels: { color: '#f8fafc' } } }
                }
            });

            const ctxVolCur = document.getElementById('volCurChart').getContext('2d');
            volCurChartInstance = new Chart(ctxVolCur, {
                type: 'line',
                data: {
                    labels: [],
                    datasets: [
                        {
                            label: 'Voltage (V)',
                            data: [],
                            borderColor: '#3b82f6',
                            yAxisID: 'yV',
                            tension: 0.3
                        },
                        {
                            label: 'Current (A)',
                            data: [],
                            borderColor: '#ef4444',
                            yAxisID: 'yI',
                            tension: 0.3
                        }
                    ]
                },
                options: {
                    responsive: true,
                    maintainAspectRatio: false,
                    scales: {
                        x: { display: false },
                        yV: { type: 'linear', position: 'left', min: 30, max: 80, ticks: { color: '#3b82f6' } },
                        yI: { type: 'linear', position: 'right', min: -30, max: 30, ticks: { color: '#ef4444' } }
                    },
                    plugins: { legend: { labels: { color: '#f8fafc' } } }
                }
            });

            const ctxSolar = document.getElementById('solarChart').getContext('2d');
            solarChartInstance = new Chart(ctxSolar, {
                type: 'line',
                data: {
                    labels: [],
                    datasets: [{
                        label: 'Solar Power (W)',
                        data: [],
                        borderColor: '#f59e0b',
                        backgroundColor: 'rgba(245, 158, 11, 0.1)',
                        fill: true,
                        tension: 0.3
                    }]
                },
                options: {
                    responsive: true,
                    maintainAspectRatio: false,
                    scales: {
                        x: { display: false },
                        y: { min: 0, max: 500, ticks: { color: '#f59e0b' } }
                    },
                    plugins: { legend: { labels: { color: '#f8fafc' } } }
                }
            });
        }

        function loadHistoricalData() {
            const period = document.getElementById('chart-period').value;
            const end = Date.now() / 1000;
            const start = end - parseInt(period);
            
            fetch(`/api/history?start=${start}&end=${end}`)
            .then(res => res.json())
            .then(data => {
                const labels = data.map(d => new Date(d.timestamp * 1000).toLocaleTimeString());
                
                // Update SOC Chart
                socChartInstance.data.labels = labels;
                socChartInstance.data.datasets[0].data = data.map(d => d.soc);
                socChartInstance.update();

                // Update Vol/Cur Chart
                volCurChartInstance.data.labels = labels;
                volCurChartInstance.data.datasets[0].data = data.map(d => d.batt_voltage);
                volCurChartInstance.data.datasets[1].data = data.map(d => d.batt_current);
                volCurChartInstance.update();

                // Update Solar Chart
                solarChartInstance.data.labels = labels;
                solarChartInstance.data.datasets[0].data = data.map(d => d.solar_power);
                solarChartInstance.update();
            });
        }

        function pushLiveChartData(data) {
            const timeStr = new Date().toLocaleTimeString();
            
            [socChartInstance, volCurChartInstance, solarChartInstance].forEach(chart => {
                if (!chart) return;
                if (chart.data.labels.length > 50) {
                    chart.data.labels.shift();
                }
                chart.data.labels.push(timeStr);
            });

            if (socChartInstance) {
                if (socChartInstance.data.datasets[0].data.length > 50) socChartInstance.data.datasets[0].data.shift();
                socChartInstance.data.datasets[0].data.push(data.soc);
                socChartInstance.update();
            }

            if (volCurChartInstance) {
                if (volCurChartInstance.data.datasets[0].data.length > 50) {
                    volCurChartInstance.data.datasets[0].data.shift();
                    volCurChartInstance.data.datasets[1].data.shift();
                }
                volCurChartInstance.data.datasets[0].data.push(data.battery_voltage);
                volCurChartInstance.data.datasets[1].data.push(data.battery_current);
                volCurChartInstance.update();
            }

            if (solarChartInstance) {
                if (solarChartInstance.data.datasets[0].data.length > 50) solarChartInstance.data.datasets[0].data.shift();
                solarChartInstance.data.datasets[0].data.push(data.solar_power);
                solarChartInstance.update();
            }
        }

        window.addEventListener('load', () => {
            initCharts();
            loadHistoricalData();
        });
    </script>
</body>
</html>
"""

def run_serial_ota(image_bytes):
    import zlib
    import struct
    import queue
    import time
    
    ota_ack_queue = queue.Queue()
    with clients_lock:
        clients.append(ota_ack_queue)
        
    try:
        actual_crc32 = zlib.crc32(image_bytes) & 0xFFFFFFFF
        total_len = len(image_bytes)
        print(f"[OTA] Starting Serial OTA. Size: {total_len} bytes, CRC32: 0x{actual_crc32:08X}")
        
        # Send MSG_OTA_BEGIN (0x10)
        # Payload format: size (4 bytes LE) + crc (4 bytes LE)
        begin_payload = struct.pack("<II", total_len, actual_crc32)
        if link is None:
            return False, "Serial link not connected"
            
        link.send(0x10, begin_payload)
        
        # Wait for OTA_ACK
        try:
            ack = ota_ack_queue.get(timeout=5.0)
            if ack.get("type") != "ota_ack" or ack.get("status") != 0:
                return False, f"OTA_BEGIN NACKed: {ack}"
        except queue.Empty:
            return False, "OTA_BEGIN timeout (no ACK)"
            
        # Stream Data Chunks
        OTA_CHUNK_MAX_DATA = 30
        chunk_total = (total_len + OTA_CHUNK_MAX_DATA - 1) // OTA_CHUNK_MAX_DATA
        
        for seq in range(chunk_total):
            start = seq * OTA_CHUNK_MAX_DATA
            chunk = image_bytes[start:start + OTA_CHUNK_MAX_DATA]
            # Payload format: seq (2 bytes LE) + chunk
            data_payload = struct.pack("<H", seq) + chunk
            
            link.send(0x11, data_payload)
            
            try:
                ack = ota_ack_queue.get(timeout=3.0)
                if ack.get("type") != "ota_ack" or ack.get("status") != 0 or ack.get("seq") != seq:
                    link.send(0x13) # MSG_OTA_ABORT
                    return False, f"OTA_DATA seq {seq} failed: {ack}"
            except queue.Empty:
                link.send(0x13) # MSG_OTA_ABORT
                return False, f"OTA_DATA seq {seq} timeout"
                
            if seq % 20 == 0 or seq == chunk_total - 1:
                print(f"[OTA] Progress: {seq + 1}/{chunk_total} chunks sent.")
                
        # Finalize
        link.send(0x12) # MSG_OTA_END
        
        try:
            ack = ota_ack_queue.get(timeout=5.0)
            if ack.get("type") != "ota_ack" or ack.get("status") != 8: # 8 = SUCCESS_REBOOTING
                return False, f"OTA_END failed: {ack}"
        except queue.Empty:
            return False, "OTA_END reboot timeout"
            
        print("[OTA] Firmware successfully updated! ESP32 rebooting.")
        return True, "Rebooting into new image"
        
    finally:
        with clients_lock:
            if ota_ack_queue in clients:
                clients.remove(ota_ack_queue)

class DashboardHTTPHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/':
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.end_headers()
            self.wfile.write(HTML_CONTENT.encode('utf-8'))
        elif self.path.startswith('/api/status'):
            with state_lock:
                self.send_json_response(latest_state)
        elif self.path.startswith('/api/faults'):
            import database
            active = database.get_active_faults()
            history = database.get_fault_history()
            self.send_json_response({"active": active, "history": history})
        elif self.path.startswith('/api/history'):
            import urllib.parse
            import database
            parsed = urllib.parse.urlparse(self.path)
            params = urllib.parse.parse_qs(parsed.query)
            # Default to last 1 hour
            end = float(params.get('end', [time.time()])[0])
            start = float(params.get('start', [end - 3600])[0])
            history = database.get_historical_telemetry(start, end)
            self.send_json_response(history)
        elif self.path == '/events':
            # Server Sent Events (SSE) stream
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Cache-Control', 'no-cache')
            self.send_header('Connection', 'keep-alive')
            self.end_headers()

            import queue
            q = queue.Queue()
            with clients_lock:
                clients.append(q)

            # Send initial state immediately
            with state_lock:
                initial_data = dict(latest_state)
            initial_data["type"] = "status_update"
            self.send_sse_event(initial_data)

            try:
                while True:
                    try:
                        # Wait for a new decoded frame to broadcast
                        data = q.get(timeout=1.0)
                        self.send_sse_event(data)
                    except queue.Empty:
                        # Keep-alive ping
                        self.wfile.write(b": ping\n\n")
                        self.wfile.flush()
            except (ConnectionResetError, BrokenPipeError):
                pass
            finally:
                with clients_lock:
                    if q in clients:
                        clients.remove(q)
        else:
            self.send_error(404, "Not Found")

    def send_sse_event(self, data):
        json_str = json.dumps(data)
        event_str = f"data: {json_str}\n\n"
        self.wfile.write(event_str.encode('utf-8'))
        self.wfile.flush()

    def do_POST(self):
        global link
        if self.path == '/api/command':
            content_length = int(self.headers['Content-Length'])
            post_data = self.rfile.read(content_length)
            try:
                cmd = json.loads(post_data.decode('utf-8'))
                msg_id = int(cmd.get("msg_id"))
                payload = bytes(cmd.get("payload", []))
                
                # Send over Serial if connected
                if link is not None:
                    try:
                        link.send_and_await(msg_id, payload, timeout=1.5)
                        # Publish to MQTT as well (allows Wi-Fi operation)
                        try:
                            import paho.mqtt.publish as publish
                            mqtt_payload = {"msg_id": msg_id, "payload": list(payload), "source": "gateway", "seq_num": 0}
                            publish.single("ems/commands", json.dumps(mqtt_payload), hostname="127.0.0.1", port=1883)
                        except Exception:
                            pass
                        self.send_json_response({
                            "status": "success",
                            "acked_msg_id": msg_id
                        })
                    except NackReceived as ne:
                        self.send_json_response({
                            "status": "nack",
                            "acked_msg_id": msg_id,
                            "reason": ne.reason,
                            "reason_name": ne.reason_name
                        })
                        return
                    except Exception as se:
                        print(f"[Dashboard] Serial write exception: {se}")
                        try:
                            link.close()
                        except Exception:
                            pass
                        link = None
                        self.send_json_response({
                            "status": "error",
                            "message": f"Serial write exception: {se}"
                        })
                        return
                else:
                    # WIRELESS MODE: Command over MQTT
                    global command_seq_counter
                    with seq_lock:
                        command_seq_counter += 1
                        seq_num = command_seq_counter
                    
                    event = threading.Event()
                    with pending_commands_lock:
                        pending_commands[seq_num] = {"event": event, "response": None}
                    
                    try:
                        import paho.mqtt.publish as publish
                        mqtt_payload = {
                            "msg_id": msg_id,
                            "payload": list(payload),
                            "source": "gateway",
                            "seq_num": seq_num
                        }
                        if msg_id == 6 and len(payload) >= 5:
                            import struct
                            param_id = payload[0]
                            float_val = struct.unpack('<f', payload[1:5])[0]
                            mqtt_payload = {
                                "param_id": param_id,
                                "value": float_val,
                                "source": "gateway",
                                "seq_num": seq_num
                            }
                            publish.single("ems/config", json.dumps(mqtt_payload), hostname="127.0.0.1", port=1883)
                        else:
                            publish.single("ems/commands", json.dumps(mqtt_payload), hostname="127.0.0.1", port=1883)
                    except Exception as pe:
                        with pending_commands_lock:
                            pending_commands.pop(seq_num, None)
                        self.send_json_response({
                            "status": "error",
                            "message": f"MQTT publish failed: {pe}"
                        })
                        return
                    
                    # Wait for response on ems/responses topic
                    success = event.wait(timeout=2.0)
                    with pending_commands_lock:
                        resp_data = pending_commands.pop(seq_num, None)
                    
                    if success and resp_data and resp_data["response"]:
                        resp = resp_data["response"]
                        status_val = resp.get("status", "error")
                        res_json = {
                            "status": status_val,
                            "acked_msg_id": msg_id
                        }
                        if status_val in ("nack", "error"):
                            res_json["reason"] = resp.get("reason", 4)
                            res_json["reason_name"] = resp.get("reason_name", "UNKNOWN_ERROR")
                        self.send_json_response(res_json)
                    else:
                        self.send_json_response({
                            "status": "error",
                            "message": "Command timeout (no response from wireless EMS)"
                        })

            except Exception as e:
                self.send_json_response({
                    "status": "error",
                    "error_type": "internal",
                    "message": str(e)
                })

        elif self.path == '/api/command_corrupted':
            # Specifically craft a frame with a corrupted checksum to test NACK_CHECKSUM_ERROR
            try:
                if link is None:
                    raise RuntimeError("Serial link not connected")
                
                # Import locally from protocol to build packet
                from rayglides_protocol import encode_frame, MSG_STATUS_UPDATE, MSG_NACK
                raw_frame = bytearray(encode_frame(MSG_STATUS_UPDATE, bytes([80, 1])))
                raw_frame[-2] ^= 0xFF  # Corrupt the checksum byte
                
                event = threading.Event()
                wait_entry = {"event": event, "response": None}
                
                with link._pending_lock:
                    link._pending_commands[MSG_STATUS_UPDATE] = wait_entry
                
                try:
                    # Inject corrupted frame directly
                    link._serial.write(bytes(raw_frame))
                    
                    # Wait for background thread to poll and receive the NACK
                    if event.wait(timeout=1.5):
                        nack_frame = wait_entry["response"]
                    else:
                        nack_frame = None
                finally:
                    with link._pending_lock:
                        link._pending_commands.pop(MSG_STATUS_UPDATE, None)

                if nack_frame:
                    from rayglides_protocol import decode_nack
                    decoded = decode_nack(nack_frame.payload)
                    self.send_json_response({
                        "status": "error",
                        "error_type": "nack",
                        "reason": decoded["reason"],
                        "reason_name": decoded["reason_name"],
                        "acked_msg_id": decoded["acked_msg_id"]
                    })
                else:
                    self.send_json_response({
                        "status": "error",
                        "error_type": "timeout",
                        "message": "No NACK received for corrupted checksum"
                    })
            except Exception as e:
                self.send_json_response({
                    "status": "error",
                    "error_type": "internal",
                    "message": str(e)
                })
        elif self.path == '/api/ota':
            try:
                content_length = int(self.headers['Content-Length'])
                image_bytes = self.rfile.read(content_length)
                
                success, msg = run_serial_ota(image_bytes)
                if success:
                    self.send_json_response({"status": "success", "message": msg})
                else:
                    self.send_json_response({"status": "error", "message": msg})
            except Exception as e:
                self.send_json_response({"status": "error", "message": str(e)})
        else:
            self.send_error(404, "Not Found")

    def send_json_response(self, data):
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps(data).encode('utf-8'))

class DashboardServer(ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        exc_type, exc_value, _ = sys.exc_info()
        if exc_type in (ConnectionResetError, BrokenPipeError) or (exc_value and "Broken pipe" in str(exc_value)):
            pass
        else:
            super().handle_error(request, client_address)

def mqtt_client_thread():
    import paho.mqtt.client as mqtt
    
    def on_connect(client, userdata, flags, rc, properties=None):
        if rc == 0:
            print("[Dashboard MQTT] Connected to broker")
            client.subscribe("ems/telemetry")
            client.subscribe("ems/faults")
            client.subscribe("ems/responses")
            
    def on_message(client, userdata, msg):
        topic = msg.topic
        payload_str = msg.payload.decode('utf-8', errors='ignore')
        try:
            data = json.loads(payload_str)
            if topic == "ems/telemetry":
                decoded = {
                    "type": "status_update",
                    "soc": data.get("soc", 0),
                    "battery_soh": data.get("soh", 0),
                    "state": data.get("state", "UNKNOWN"),
                    "battery_voltage": data.get("battery", {}).get("voltage", 0.0),
                    "battery_current": data.get("battery", {}).get("current", 0.0),
                    "battery_temp": data.get("battery", {}).get("temp", 0.0),
                    "solar_power": data.get("solar", {}).get("power", 0.0),
                    "solar_voltage": data.get("solar", {}).get("voltage", 0.0),
                    "power_mode": data.get("mode", "NORMAL"),
                }
                update_global_state(decoded)
                broadcast_event(decoded)
            elif topic == "ems/faults":
                sev = data.get("severity", "WARNING")
                decoded = {
                    "type": "fault_report",
                    "fault_code": data.get("fault_code", 0),
                    "severity": 1 if sev == "CRITICAL" else 0
                }
                update_global_state(decoded)
                broadcast_event(decoded)
            elif topic == "ems/responses":
                seq_num = data.get("seq_num")
                if seq_num is not None:
                    with pending_commands_lock:
                        if seq_num in pending_commands:
                            pending_commands[seq_num]["response"] = data
                            pending_commands[seq_num]["event"].set()
        except Exception as e:
            print(f"[Dashboard MQTT] Error processing: {e}")

    client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message
    
    while True:
        try:
            client.connect("127.0.0.1", 1883, keepalive=60)
            client.loop_forever()
        except Exception as e:
            time.sleep(2)

def main():
    port = DEFAULT_PORT
    if len(sys.argv) > 1:
        port = sys.argv[1]

    # Start serial monitoring in a separate thread
    t = threading.Thread(target=serial_polling_thread, args=(port,), daemon=True)
    t.start()

    # Start MQTT subscriber thread
    t_mqtt = threading.Thread(target=mqtt_client_thread, daemon=True)
    t_mqtt.start()

    # Start the HTTP server on port 8080
    server_address = ('', 8080)
    httpd = DashboardServer(server_address, DashboardHTTPHandler)
    print(f"\n=======================================================")
    print(f"  RayGlides EMS Premium Dashboard Running At:")
    print(f"  http://localhost:8080")
    print(f"=======================================================\n")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down server.")
        httpd.server_close()

if __name__ == '__main__':
    main()
