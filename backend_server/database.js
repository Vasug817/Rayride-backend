const sqlite3 = require('sqlite3').verbose();
const path = require('path');
const bcrypt = require('bcryptjs');

const DB_PATH = path.join(__dirname, 'rayglides.db');
const db = new sqlite3.Database(DB_PATH);

function initDb() {
  return new Promise((resolve, reject) => {
    db.serialize(() => {
      // 1. Users Table
      db.run(`
        CREATE TABLE IF NOT EXISTS users (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          name TEXT NOT NULL,
          email TEXT UNIQUE NOT NULL,
          phone TEXT,
          role TEXT DEFAULT 'driver', -- 'driver' or 'admin'
          auth_provider TEXT DEFAULT 'email', -- 'email' or 'google'
          password_hash TEXT,
          created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        )
      `);

      // 2. Vehicles Table
      db.run(`
        CREATE TABLE IF NOT EXISTS vehicles (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          user_id INTEGER,
          device_id TEXT UNIQUE NOT NULL, -- ESP32 MAC address/ID
          model_type TEXT NOT NULL, -- '3_wheeler' or '4_wheeler'
          license_plate TEXT,
          battery_capacity_ah REAL DEFAULT 20.0,
          created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
          FOREIGN KEY (user_id) REFERENCES users(id)
        )
      `);

      // 3. Telemetry History Table
      db.run(`
        CREATE TABLE IF NOT EXISTS telemetry_history (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          device_id TEXT NOT NULL,
          timestamp DATETIME NOT NULL,
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
      `);

      // 4. OTP Codes Table
      db.run(`
        CREATE TABLE IF NOT EXISTS otp_codes (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          contact_info TEXT NOT NULL,
          code TEXT NOT NULL,
          expires_at DATETIME NOT NULL,
          verified INTEGER DEFAULT 0
        )
      `);

      // Create indexes for fast querying
      db.run(`CREATE INDEX IF NOT EXISTS idx_telemetry_device ON telemetry_history(device_id)`);
      db.run(`CREATE INDEX IF NOT EXISTS idx_telemetry_time ON telemetry_history(timestamp)`);

      // Seed default admin and driver only if explicitly requested in environment variables or testing mode
      let adminEmail = process.env.ADMIN_EMAIL;
      let adminPwd = process.env.ADMIN_PASSWORD;
      let driverEmail = process.env.DRIVER_EMAIL;
      let driverPwd = process.env.DRIVER_PASSWORD;
      
      if (process.env.IS_DEVELOPMENT === 'true' || process.env.TESTING === 'true') {
        adminEmail = adminEmail || 'admin@rayglides.com';
        adminPwd = adminPwd || 'admin123';
        driverEmail = driverEmail || 'vasu@rayglides.com';
        driverPwd = driverPwd || 'driver123';
      }
 
      if (adminEmail && adminPwd) {
        const adminPassHash = bcrypt.hashSync(adminPwd, 10);
        db.run(`
          INSERT OR IGNORE INTO users (name, email, role, password_hash)
          VALUES ('RayGlides Administrator', ?, 'admin', ?)
        `, [adminEmail, adminPassHash]);
      }
 
      if (driverEmail && driverPwd) {
        const driverPassHash = bcrypt.hashSync(driverPwd, 10);
        db.run(`
          INSERT OR IGNORE INTO users (name, email, phone, role, password_hash)
          VALUES ('Vasu Gupta', ?, '+919876543210', 'driver', ?)
        `, [driverEmail, driverPassHash], function(err) {
          if (err) return reject(err);
          
          // If driver was inserted, bind a vehicle
          const userId = this.lastID || 2;
          db.run(`
            INSERT OR IGNORE INTO vehicles (user_id, device_id, model_type, license_plate)
            VALUES (?, 'RayGlides_EMS_9232C8', '3_wheeler', 'DL-3S-EV-1234')
          `, [userId], (err) => {
            if (err) return reject(err);
            resolve();
          });
        });
      } else {
        resolve();
      }
    });
  });
}

module.exports = {
  db,
  initDb
};
