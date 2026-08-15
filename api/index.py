from flask import Flask, request, jsonify
from flask_cors import CORS
import sqlite3
import hashlib
import hmac
import time
import os
import random
import threading
import json
import base64
import traceback
import paho.mqtt.client as mqtt

app = Flask(__name__)
# Enable CORS for all origins, allowing Authorization and content-type headers
CORS(app, resources={r"/api/*": {"origins": "*"}})

JWT_SECRET = b'rayglides_super_secret_signing_key_2026'
TARIFF_RATE_PER_KWH = 8.5 # INR / local currency per kWh

# Resolve Database Path based on environment
if 'VERCEL' in os.environ:
    DB_PATH = '/tmp/rayglides.db'
else:
    DB_PATH = os.path.join(os.path.dirname(__file__), '..', 'backend_server', 'rayglides.db')

# Global coordinates lock for mock movements
mock_lat = 28.6139
mock_lng = 77.2090
coord_lock = threading.Lock()

# ----------------------------------------------------
# DATABASE INITIALIZER & CONTEXT HELPERS
# ----------------------------------------------------
def get_db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn

def init_db():
    conn = get_db()
    cursor = conn.cursor()
    
    # 1. Users Table
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS users (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          name TEXT NOT NULL,
          email TEXT UNIQUE NOT NULL,
          phone TEXT,
          role TEXT DEFAULT 'driver', -- 'driver' or 'admin'
          auth_provider TEXT DEFAULT 'email',
          password_hash TEXT,
          emergency_name TEXT,
          emergency_phone TEXT,
          created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    """)
    
    # 2. Vehicles Table
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS vehicles (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          user_id INTEGER,
          device_id TEXT UNIQUE NOT NULL,
          model_type TEXT NOT NULL, -- '3_wheeler' or '4_wheeler'
          license_plate TEXT,
          battery_capacity_ah REAL DEFAULT 20.0,
          created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
          FOREIGN KEY (user_id) REFERENCES users(id)
        )
    """)
    
    # 3. Telemetry Table
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS telemetry_history (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          device_id TEXT NOT NULL,
          timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
          soc INTEGER,
          soh INTEGER,
          battery_voltage REAL,
          battery_current REAL,
          battery_temp REAL,
          solar_voltage REAL,
          solar_power REAL,
          fan_duty REAL,
          solar_wh REAL,
          charge_wh REAL,
          cost_saved REAL,
          latitude REAL,
          longitude REAL,
          FOREIGN KEY (device_id) REFERENCES vehicles(device_id)
        )
    """)
    
    # 4. OTP Codes Table
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS otp_codes (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          contact_info TEXT NOT NULL,
          code TEXT NOT NULL,
          expires_at REAL NOT NULL, -- Epoch timestamp
          verified INTEGER DEFAULT 0
        )
    """)
    
    # 5. Transactions Table
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS transactions (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          order_id TEXT,
          payment_id TEXT,
          status TEXT,
          created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    """)
    
    # 6. Rides / Orders Table
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS rides (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          rider_name TEXT NOT NULL,
          pickup_location TEXT NOT NULL,
          dropoff_location TEXT NOT NULL,
          fare REAL NOT NULL,
          status TEXT DEFAULT 'available', -- 'available', 'accepted', 'started', 'completed', 'cancelled'
          driver_id INTEGER,
          created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
          completed_at DATETIME,
          FOREIGN KEY (driver_id) REFERENCES users(id)
        )
    """)
    
    cursor.execute("CREATE INDEX IF NOT EXISTS idx_telemetry_device ON telemetry_history(device_id)")
    cursor.execute("CREATE INDEX IF NOT EXISTS idx_telemetry_time ON telemetry_history(timestamp)")
    cursor.execute("CREATE INDEX IF NOT EXISTS idx_rides_status ON rides(status)")
    
    # Seed default admin and driver only if explicitly requested in environment variables or testing mode
    admin_email = os.environ.get('ADMIN_EMAIL')
    admin_pwd = os.environ.get('ADMIN_PASSWORD')
    driver_email = os.environ.get('DRIVER_EMAIL')
    driver_pwd = os.environ.get('DRIVER_PASSWORD')
    
    if os.environ.get('IS_DEVELOPMENT') == 'true' or os.environ.get('TESTING') == 'true' or os.environ.get('VERCEL_ENV') == 'development':
        admin_email = admin_email or 'admin@rayglides.com'
        admin_pwd = admin_pwd or 'admin123'
        driver_email = driver_email or 'vasu@rayglides.com'
        driver_pwd = driver_pwd or 'driver123'
        
    if admin_email and admin_pwd:
        admin_hash = hash_password(admin_pwd)
        cursor.execute("INSERT OR IGNORE INTO users (name, email, role, password_hash) VALUES (?, ?, ?, ?)",
                       ('RayGlides Admin', admin_email, 'admin', admin_hash))
                       
    if driver_email and driver_pwd:
        driver_hash = hash_password(driver_pwd)
        cursor.execute("INSERT OR IGNORE INTO users (name, email, phone, role, password_hash, emergency_name, emergency_phone) VALUES (?, ?, ?, ?, ?, ?, ?)",
                       ('Vasu Gupta', driver_email, '+919876543210', 'driver', driver_hash, 'Emergency Contact', '+911111111111'))
    
    # Bind vehicle to Vasu if seeded
    if driver_email:
        cursor.execute("SELECT id FROM users WHERE email=?", (driver_email,))
        user_row = cursor.fetchone()
        if user_row:
            cursor.execute("""
                INSERT OR IGNORE INTO vehicles (user_id, device_id, model_type, license_plate)
                VALUES (?, 'RayGlides_EMS_9232C8', '3_wheeler', 'DL-3S-EV-1234')
            """, (user_row[0],))
        
    # Seed a few available rides for testing if empty
    cursor.execute("SELECT count(*) FROM rides WHERE status='available'")
    if cursor.fetchone()[0] == 0:
        cursor.execute("INSERT INTO rides (rider_name, pickup_location, dropoff_location, fare) VALUES (?, ?, ?, ?)",
                       ('Delhi Cargo Depot', 'Okhla Phase 3', 'Connaught Place', 350.0))
        cursor.execute("INSERT INTO rides (rider_name, pickup_location, dropoff_location, fare) VALUES (?, ?, ?, ?)",
                       ('Bangalore Logistics Hub', 'Whitefield Industrial Area', 'Koramangala', 450.0))
        cursor.execute("INSERT INTO rides (rider_name, pickup_location, dropoff_location, fare) VALUES (?, ?, ?, ?)",
                       ('Mumbai Port Authority', 'JNPT Cargo Terminal', 'Vashi Depot', 650.0))

    conn.commit()
    conn.close()

@app.before_request
def setup_db():
    init_db()

# ----------------------------------------------------
# PASSWORD HASHING & HMAC TOKENS
# ----------------------------------------------------
def hash_password(password):
    salt = b'rayglides_salt_static'
    dk = hashlib.pbkdf2_hmac('sha256', password.encode(), salt, 100000)
    return dk.hex()

def generate_session_token(user_id, role, name):
    payload = {
        "id": user_id,
        "role": role,
        "name": name,
        "exp": time.time() + 7 * 24 * 3600 # 7 days
    }
    payload_str = json.dumps(payload)
    payload_b64 = base64.b64encode(payload_str.encode()).decode()
    sig = hmac.new(JWT_SECRET, payload_b64.encode(), hashlib.sha256).hexdigest()
    return f"{payload_b64}.{sig}"

def verify_session_token(token):
    try:
        parts = token.split('.')
        if len(parts) != 2:
            return None
        payload_b64, sig = parts[0], parts[1]
        expected_sig = hmac.new(JWT_SECRET, payload_b64.encode(), hashlib.sha256).hexdigest()
        if not hmac.compare_digest(sig, expected_sig):
            return None
        payload_str = base64.b64decode(payload_b64.encode()).decode()
        payload = json.loads(payload_str)
        if time.time() > payload["exp"]:
            return None
        return payload
    except Exception:
        return None

def get_auth_user():
    auth_header = request.headers.get('Authorization')
    if not auth_header or not auth_header.startswith('Bearer '):
        return None
    token = auth_header.split(' ')[1]
    return verify_session_token(token)

# ----------------------------------------------------
# AUTHENTICATION ROUTING
# ----------------------------------------------------
@app.route('/api/auth/signup', methods=['POST'])
def signup():
    body = request.get_json() or {}
    name = body.get('name')
    email = body.get('email')
    phone = body.get('phone')
    password = body.get('password')
    role = body.get('role', 'driver')
    emergency_name = body.get('emergency_name')
    emergency_phone = body.get('emergency_phone')

    if not name or not email or not password:
        return jsonify({"error": "Name, email, and password are required"}), 400

    pwd_hash = hash_password(password)
    conn = get_db()
    cursor = conn.cursor()
    try:
        cursor.execute("""
            INSERT INTO users (name, email, phone, role, password_hash, emergency_name, emergency_phone)
            VALUES (?, ?, ?, ?, ?, ?, ?)
        """, (name, email, phone, role, pwd_hash, emergency_name, emergency_phone))
        user_id = cursor.lastrowid
        
        if role == 'driver':
            mock_mac = 'RayGlides_EMS_' + ''.join(random.choices('0123456789ABCDEF', k=6))
            cursor.execute("""
                INSERT INTO vehicles (user_id, device_id, model_type, license_plate)
                VALUES (?, ?, '3_wheeler', ?)
            """, (user_id, mock_mac, f"DL-3S-MOCK-{user_id}"))
        conn.commit()
        return jsonify({"success": True, "message": "Signed up successfully", "userId": user_id}), 201
    except sqlite3.IntegrityError:
        return jsonify({"error": "Email address already registered"}), 400
    finally:
        conn.close()

@app.route('/api/auth/send-otp', methods=['POST'])
def send_otp():
    body = request.get_json() or {}
    contact_info = body.get('contact_info')
    if not contact_info:
        return jsonify({"error": "Contact info is required"}), 400

    code = str(random.randint(100000, 999999))
    expires_at = time.time() + 600 # 10 minutes Expiry
    
    conn = get_db()
    cursor = conn.cursor()
    cursor.execute("INSERT INTO otp_codes (contact_info, code, expires_at) VALUES (?, ?, ?)",
                   (contact_info, code, expires_at))
    conn.commit()
    conn.close()

    print("[OTP Generator] OTP generated and saved securely.")
    
    resp = {"success": True, "message": "OTP sent successfully"}
    if body.get('test') == True or os.environ.get('TESTING') == 'true':
        resp['code'] = code
    return jsonify(resp)

@app.route('/api/auth/signin-otp', methods=['POST'])
def signin_otp():
    body = request.get_json() or {}
    contact_info = body.get('contact_info')
    code = body.get('code')
    if not contact_info or not code:
        return jsonify({"error": "Contact info and OTP code are required"}), 400

    now = time.time()
    conn = get_db()
    cursor = conn.cursor()
    cursor.execute("""
        SELECT id FROM otp_codes 
        WHERE contact_info = ? AND code = ? AND expires_at > ? AND verified = 0
        ORDER BY id DESC LIMIT 1
    """, (contact_info, code, now))
    otp_row = cursor.fetchone()
    
    if not otp_row:
        conn.close()
        return jsonify({"error": "Invalid or expired OTP code"}), 400

    cursor.execute("UPDATE otp_codes SET verified = 1 WHERE id = ?", (otp_row['id'],))
    
    cursor.execute("SELECT id, name, role, email FROM users WHERE email = ? OR phone = ?", (contact_info, contact_info))
    user = cursor.fetchone()
    
    if not user:
        temp_name = contact_info.split('@')[0] if '@' in contact_info else 'Driver Guest'
        temp_hash = hash_password('temp123')
        cursor.execute("""
            INSERT INTO users (name, email, phone, role, password_hash)
            VALUES (?, ?, ?, 'driver', ?)
        """, (temp_name, contact_info if '@' in contact_info else f"{temp_name}@rayglides.com", None if '@' in contact_info else contact_info, temp_hash))
        user_id = cursor.lastrowid
        
        cursor.execute("""
            INSERT OR IGNORE INTO vehicles (user_id, device_id, model_type, license_plate)
            VALUES (?, 'RayGlides_EMS_9232C8', '3_wheeler', 'DL-3S-AUTO')
        """, (user_id,))
        conn.commit()
        token = generate_session_token(user_id, 'driver', temp_name)
        conn.close()
        return jsonify({"success": True, "token": token, "user": {"id": user_id, "name": temp_name, "role": "driver"}})
    else:
        token = generate_session_token(user['id'], user['role'], user['name'])
        conn.close()
        return jsonify({"success": True, "token": token, "user": {"id": user['id'], "name": user['name'], "role": user['role'], "email": user['email']}})

@app.route('/api/auth/signin-password', methods=['POST'])
def signin_password():
    body = request.get_json() or {}
    email = body.get('email')
    password = body.get('password')
    if not email or not password:
        return jsonify({"error": "Email and password are required"}), 400

    pwd_hash = hash_password(password)
    conn = get_db()
    cursor = conn.cursor()
    cursor.execute("SELECT id, name, role, email FROM users WHERE email = ? AND password_hash = ?", (email, pwd_hash))
    user = cursor.fetchone()
    conn.close()

    if not user:
        return jsonify({"error": "Invalid email or password"}), 401

    token = generate_session_token(user['id'], user['role'], user['name'])
    return jsonify({"success": True, "token": token, "user": {"id": user['id'], "name": user['name'], "role": user['role'], "email": user['email']}})

@app.route('/api/auth/google-login', methods=['POST'])
def google_login():
    body = request.get_json() or {}
    email = body.get('email')
    name = body.get('name')
    if not email or not name:
        return jsonify({"error": "Google email and name are required"}), 400

    conn = get_db()
    cursor = conn.cursor()
    cursor.execute("SELECT id, name, role, email FROM users WHERE email = ?", (email,))
    user = cursor.fetchone()

    if not user:
        cursor.execute("""
            INSERT INTO users (name, email, role, auth_provider)
            VALUES (?, ?, 'driver', 'google')
        """, (name, email))
        user_id = cursor.lastrowid
        cursor.execute("""
            INSERT INTO vehicles (user_id, device_id, model_type, license_plate)
            VALUES (?, 'RayGlides_EMS_9232C8', '3_wheeler', 'DL-3S-GOOG')
        """, (user_id,))
        conn.commit()
        token = generate_session_token(user_id, 'driver', name)
        conn.close()
        return jsonify({"success": True, "token": token, "user": {"id": user_id, "name": name, "role": 'driver', "email": email}}), 201
    else:
        token = generate_session_token(user['id'], user['role'], user['name'])
        conn.close()
        return jsonify({"success": True, "token": token, "user": {"id": user['id'], "name": user['name'], "role": user['role'], "email": user['email']}})

# ----------------------------------------------------
# DRIVER DIAGNOSTICS & TELEMETRY
# ----------------------------------------------------
@app.route('/api/driver/vehicle-status', methods=['GET'])
def vehicle_status():
    user = get_auth_user()
    if not user:
        return jsonify({"error": "Unauthorized session"}), 401

    conn = get_db()
    cursor = conn.cursor()
    cursor.execute("""
        SELECT v.device_id, v.model_type, v.license_plate, t.*
        FROM vehicles v
        LEFT JOIN (
          SELECT * FROM telemetry_history ORDER BY timestamp DESC LIMIT 1
        ) t ON v.device_id = t.device_id
        WHERE v.user_id = ?
    """, (user["id"],))
    row = cursor.fetchone()
    conn.close()

    if not row:
        return jsonify({"error": "No vehicle bound to this account"}), 404

    data = dict(row)
    response = {
        "device_id": data["device_id"],
        "model_type": data["model_type"],
        "license_plate": data["license_plate"],
        "timestamp": data.get("timestamp") or time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        "soc": data.get("soc") if data.get("soc") is not None else 78,
        "soh": data.get("soh") if data.get("soh") is not None else 98,
        "battery_voltage": data.get("battery_voltage") or 52.4,
        "battery_current": data.get("battery_current") if data.get("battery_current") is not None else 3.2,
        "battery_temp": data.get("battery_temp") or 26.5,
        "solar_voltage": data.get("solar_voltage") or 28.2,
        "solar_power": data.get("solar_power") if data.get("solar_power") is not None else 210.0,
        "fan_duty": data.get("fan_duty") if data.get("fan_duty") is not None else 0.45,
        "solar_wh": data.get("solar_wh") or 650.0,
        "charge_wh": data.get("charge_wh") or 480.0,
        "cost_saved": data.get("cost_saved") or 5.53,
        "latitude": data.get("latitude") or 28.6139,
        "longitude": data.get("longitude") or 77.2090
    }
    return jsonify(response)

@app.route('/api/driver/savings-summary', methods=['GET'])
def savings_summary():
    user = get_auth_user()
    if not user:
        return jsonify({"error": "Unauthorized session"}), 401

    conn = get_db()
    cursor = conn.cursor()
    cursor.execute("""
        SELECT date(timestamp) as date, max(cost_saved) as daily_savings, max(solar_wh) as daily_solar_wh
        FROM telemetry_history
        WHERE device_id = (SELECT device_id FROM vehicles WHERE user_id = ?)
        GROUP BY date(timestamp)
        ORDER BY date DESC LIMIT 7
    """, (user["id"],))
    rows = cursor.fetchall()
    conn.close()

    if not rows or rows[0]["daily_savings"] is None:
        mock_rows = []
        for i in range(7):
            t_offset = i * 24 * 3600
            date_str = time.strftime('%Y-%m-%d', time.localtime(time.time() - t_offset))
            mock_rows.append({
                "date": date_str,
                "daily_savings": round(12.50 + random.random() * 5, 2),
                "daily_solar_wh": int(1400 + random.random() * 300)
            })
        return jsonify(mock_rows)
    
    return jsonify([dict(r) for r in rows])

# ----------------------------------------------------
# PROFILE UPDATES & VEHICLE CONFIGURATION
# ----------------------------------------------------
@app.route('/api/driver/update-profile', methods=['POST'])
def update_profile():
    user = get_auth_user()
    if not user:
        return jsonify({"error": "Unauthorized session"}), 401

    body = request.get_json() or {}
    emergency_name = body.get('emergency_name')
    emergency_phone = body.get('emergency_phone')

    conn = get_db()
    cursor = conn.cursor()
    cursor.execute("""
        UPDATE users SET emergency_name = ?, emergency_phone = ? WHERE id = ?
    """, (emergency_name, emergency_phone, user["id"]))
    conn.commit()
    conn.close()
    return jsonify({"success": True, "message": "Emergency profile updated successfully"})

@app.route('/api/driver/select-vehicle', methods=['POST'])
def select_vehicle():
    user = get_auth_user()
    if not user:
        return jsonify({"error": "Unauthorized session"}), 401

    body = request.get_json() or {}
    model_type = body.get('model_type') # '3_wheeler' or '4_wheeler'

    if model_type not in ('3_wheeler', '4_wheeler'):
        return jsonify({"error": "Invalid EV loader vehicle type selection"}), 400

    conn = get_db()
    cursor = conn.cursor()
    cursor.execute("UPDATE vehicles SET model_type = ? WHERE user_id = ?", (model_type, user["id"]))
    conn.commit()
    conn.close()
    return jsonify({"success": True, "message": "EV loader configuration updated successfully"})

# ----------------------------------------------------
# REAL DATABASE RIDES / ORDERS SYSTEM
# ----------------------------------------------------
@app.route('/api/rides/create', methods=['POST'])
def create_ride():
    # Anyone can create a mock customer order request
    body = request.get_json() or {}
    rider_name = body.get('rider_name', 'Commercial Logistics Depot')
    pickup = body.get('pickup_location')
    dropoff = body.get('dropoff_location')
    fare = body.get('fare', 250.0)

    if not pickup or not dropoff:
        return jsonify({"error": "Pickup and dropoff locations are required"}), 400

    conn = get_db()
    cursor = conn.cursor()
    cursor.execute("""
        INSERT INTO rides (rider_name, pickup_location, dropoff_location, fare, status)
        VALUES (?, ?, ?, ?, 'available')
    """, (rider_name, pickup, dropoff, fare))
    ride_id = cursor.lastrowid
    conn.commit()
    conn.close()

    return jsonify({"success": True, "message": "Delivery order created successfully", "ride_id": ride_id})

@app.route('/api/rides/available', methods=['GET'])
def get_available_rides():
    user = get_auth_user()
    if not user:
        return jsonify({"error": "Unauthorized session"}), 401

    conn = get_db()
    cursor = conn.cursor()
    cursor.execute("SELECT * FROM rides WHERE status = 'available' ORDER BY id DESC")
    rows = cursor.fetchall()
    conn.close()
    return jsonify([dict(r) for r in rows])

@app.route('/api/rides/accept', methods=['POST'])
def accept_ride():
    user = get_auth_user()
    if not user:
        return jsonify({"error": "Unauthorized"}), 401

    body = request.get_json() or {}
    ride_id = body.get('ride_id')

    conn = get_db()
    cursor = conn.cursor()
    
    # Validate that the ride exists and is still available (concurrency protection)
    cursor.execute("SELECT status FROM rides WHERE id = ?", (ride_id,))
    ride = cursor.fetchone()
    if not ride:
        conn.close()
        return jsonify({"error": "Order not found"}), 404
    if ride['status'] != 'available':
        conn.close()
        return jsonify({"error": "This delivery order has already been accepted by another driver!"}), 409

    # Accept ride
    cursor.execute("""
        UPDATE rides SET status = 'accepted', driver_id = ? WHERE id = ?
    """, (user["id"], ride_id))
    conn.commit()
    conn.close()
    return jsonify({"success": True, "message": "Delivery order accepted successfully"})

@app.route('/api/rides/update-status', methods=['POST'])
def update_ride_status():
    user = get_auth_user()
    if not user:
        return jsonify({"error": "Unauthorized"}), 401

    body = request.get_json() or {}
    ride_id = body.get('ride_id')
    status = body.get('status') # 'started', 'completed'

    if status not in ('started', 'completed'):
        return jsonify({"error": "Invalid status update"}), 400

    conn = get_db()
    cursor = conn.cursor()
    
    # Check ownership
    cursor.execute("SELECT driver_id FROM rides WHERE id = ?", (ride_id,))
    ride = cursor.fetchone()
    if not ride:
        conn.close()
        return jsonify({"error": "Order not found"}), 404
    if ride['driver_id'] != user["id"]:
        conn.close()
        return jsonify({"error": "Unauthorized: You are not the driver assigned to this order"}), 403

    completed_at_clause = ", completed_at = CURRENT_TIMESTAMP" if status == 'completed' else ""
    cursor.execute(f"""
        UPDATE rides SET status = ? {completed_at_clause} WHERE id = ?
    """, (status, ride_id))
    conn.commit()
    conn.close()
    return jsonify({"success": True, "message": f"Delivery status updated to: {status}"})

@app.route('/api/driver/rides-history', methods=['GET'])
def get_driver_rides():
    user = get_auth_user()
    if not user:
        return jsonify({"error": "Unauthorized"}), 401

    conn = get_db()
    cursor = conn.cursor()
    cursor.execute("""
        SELECT * FROM rides WHERE driver_id = ? ORDER BY id DESC
    """, (user["id"],))
    rows = cursor.fetchall()
    conn.close()
    return jsonify([dict(r) for r in rows])

# ----------------------------------------------------
# ADMIN EXCLUSIVE REPORTING
# ----------------------------------------------------
@app.route('/api/admin/users', methods=['GET'])
def admin_users():
    user = get_auth_user()
    if not user or user["role"] != 'admin':
        return jsonify({"error": "Admin authentication required"}), 403

    conn = get_db()
    cursor = conn.cursor()
    cursor.execute("""
        SELECT u.id, u.name, u.email, u.phone, u.role, u.created_at, v.device_id, v.model_type
        FROM users u
        LEFT JOIN vehicles v ON u.id = v.user_id
        ORDER BY u.id DESC
    """)
    rows = cursor.fetchall()
    conn.close()
    return jsonify([dict(r) for r in rows])

@app.route('/api/admin/fleet-status', methods=['GET'])
def admin_fleet():
    user = get_auth_user()
    if not user or user["role"] != 'admin':
        return jsonify({"error": "Admin authentication required"}), 403

    conn = get_db()
    cursor = conn.cursor()
    cursor.execute("""
        SELECT v.device_id, v.model_type, v.license_plate, u.name as driver_name, t.*
        FROM vehicles v
        JOIN users u ON v.user_id = u.id
        LEFT JOIN (
          SELECT device_id, max(timestamp) as last_time, soc, soh, battery_voltage, solar_power, latitude, longitude, cost_saved
          FROM telemetry_history
          GROUP BY device_id
        ) t ON v.device_id = t.device_id
    """)
    rows = cursor.fetchall()
    conn.close()
    return jsonify([dict(r) for r in rows])

@app.route('/api/admin/rides', methods=['GET'])
def admin_rides():
    user = get_auth_user()
    if not user or user["role"] != 'admin':
        return jsonify({"error": "Admin authentication required"}), 403

    conn = get_db()
    cursor = conn.cursor()
    cursor.execute("""
        SELECT r.*, u.name as driver_name, v.license_plate
        FROM rides r
        LEFT JOIN users u ON r.driver_id = u.id
        LEFT JOIN vehicles v ON u.id = v.user_id
        ORDER BY r.id DESC
    """)
    rows = cursor.fetchall()
    conn.close()
    return jsonify([dict(r) for r in rows])

# ----------------------------------------------------
# RAZORPAY MOCK PAYMENTS SYSTEM
# ----------------------------------------------------
@app.route('/api/payments/create-order', methods=['POST'])
def create_order():
    body = request.get_json() or {}
    amount = body.get('amount', 500)
    chars = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'
    random_id = ''.join(random.choice(chars) for _ in range(14))
    order_id = f"order_{random_id}"
    return jsonify({
        "success": True,
        "key": os.environ.get('RAZORPAY_KEY', 'rzp_test_DUMMY_KEY'),
        "amount": amount,
        "orderId": order_id
    })

@app.route('/api/subscriptions/checkout', methods=['POST'])
def subscriptions_checkout():
    chars = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'
    random_id = ''.join(random.choice(chars) for _ in range(14))
    order_id = f"order_sub_{random_id}"
    return jsonify({
        "success": True,
        "requires_payment": True,
        "key": os.environ.get('RAZORPAY_KEY', 'rzp_test_DUMMY_KEY'),
        "amount_due": 299,
        "order_id": order_id
    })

@app.route('/api/payments/verify', methods=['POST'])
def verify_payment():
    body = request.get_json() or {}
    order_id = body.get('razorpay_order_id')
    payment_id = body.get('razorpay_payment_id')
    signature = body.get('razorpay_signature')
    secret = os.environ.get('RAZORPAY_SECRET', 'test_razorpay_secret_key_2026')

    if not order_id or not payment_id or not signature:
        return jsonify({"error": "Missing signature verification details"}), 400

    msg = f"{order_id}|{payment_id}".encode('utf-8')
    generated_signature = hmac.new(
        secret.encode('utf-8'),
        msg,
        hashlib.sha256
    ).hexdigest()

    if hmac.compare_digest(generated_signature, signature):
        conn = get_db()
        cursor = conn.cursor()
        cursor.execute("INSERT INTO transactions (order_id, payment_id, status) VALUES (?, ?, ?)",
                       (order_id, payment_id, "success"))
        conn.commit()
        conn.close()
        return jsonify({"success": True, "message": "Payment verified and recorded successfully"})
    else:
        return jsonify({"error": "Payment signature verification failed"}), 400

# ----------------------------------------------------
# MQTT BACKGROUND THREAD (LOCAL DEVELOPMENT ONLY)
# ----------------------------------------------------
def mqtt_bridge_thread():
    client = mqtt.Client()
    
    def on_connect(client, userdata, flags, rc):
        print("[Backend MQTT] Connected to local MQTT broker.")
        client.subscribe("ems/telemetry")
        client.subscribe("ems/faults")
        
    def on_message(client, userdata, msg):
        global mock_lat, mock_lng
        payload_str = msg.payload.decode('utf-8', errors='ignore')
        try:
            data = json.loads(payload_str)
            if msg.topic == "ems/telemetry":
                dev_id = data.get("device_id", "RayGlides_EMS_9232C8")
                soc = data.get("soc", 0)
                soh = data.get("soh", 100)
                batt_v = data.get("battery", {}).get("voltage", 0.0)
                batt_i = data.get("battery", {}).get("current", 0.0)
                batt_t = data.get("battery", {}).get("temp", 25.0)
                solar_v = data.get("solar", {}).get("voltage", 0.0)
                solar_p = data.get("solar", {}).get("power", 0.0)
                fan_duty = data.get("cooling", {}).get("fan_duty", 0.0)
                solar_wh = data.get("energy", {}).get("solar_wh", 0.0)
                charge_wh = data.get("energy", {}).get("charge_wh", 0.0)
                
                cost_saved = (solar_wh / 1000.0) * TARIFF_RATE_PER_KWH
                
                with coord_lock:
                    mock_lat += (random.random() - 0.5) * 0.001
                    mock_lng += (random.random() - 0.5) * 0.001
                    lat, lng = mock_lat, mock_lng
                
                conn = get_db()
                cursor = conn.cursor()
                cursor.execute("""
                    INSERT INTO telemetry_history (
                      device_id, soc, soh, battery_voltage, battery_current, battery_temp,
                      solar_voltage, solar_power, fan_duty, solar_wh, charge_wh, cost_saved, latitude, longitude
                    ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """, (dev_id, soc, soh, batt_v, batt_i, batt_t, solar_v, solar_p, fan_duty, solar_wh, charge_wh, cost_saved, lat, lng))
                conn.commit()
                conn.close()
                print(f"[Backend MQTT] Telemetry synchronized for {dev_id}: SOC={soc}%, Solar={solar_p}W, Savings={cost_saved:.2f}")
        except Exception as e:
            print(f"[Backend MQTT] Error: {e}")

    client.on_connect = on_connect
    client.on_message = on_message
    
    while True:
        try:
            client.connect("127.0.0.1", 1883, 60)
            client.loop_forever()
        except Exception:
            time.sleep(5)

@app.errorhandler(Exception)
def handle_exception(e):
    import traceback
    return jsonify({
        "error": str(e),
        "traceback": traceback.format_exc()
    }), 500

# ----------------------------------------------------
# MAIN PROCESS RUNNER
# ----------------------------------------------------
if __name__ == '__main__':
    init_db()
    
    # Start MQTT subscriber thread locally only
    t = threading.Thread(target=mqtt_bridge_thread, daemon=True)
    t.start()
    
    print(f"\n=======================================================")
    print(f"  RayGlides Flask Backend Server Running At:")
    print(f"  http://localhost:5050")
    print(f"=======================================================\n")
    
    app.run(host='0.0.0.0', port=5050, debug=True)
