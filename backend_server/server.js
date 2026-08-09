const express = require('express');
const cors = require('cors');
const mqtt = require('mqtt');
const jwt = require('jsonwebtoken');
const { initDb, db } = require('./database');
const { authRouter, JWT_SECRET } = require('./auth');

const app = express();
app.use(cors());
app.use(express.json());

const PORT = process.env.PORT || 5000;
const TARIFF_RATE_PER_KWH = 8.5; // Average commercial charging rate (in INR/local currency)

// JWT Auth Middleware
function authenticateToken(req, res, next) {
  const authHeader = req.headers['authorization'];
  const token = authHeader && authHeader.split(' ')[1];
  if (!token) return res.status(401).json({ error: 'Access token missing' });

  jwt.verify(token, JWT_SECRET, (err, user) => {
    if (err) return res.status(403).json({ error: 'Token invalid or expired' });
    req.user = user;
    next();
  });
}

// ----------------------------------------------------
// DRIVER ENDPOINTS
// ----------------------------------------------------

// 1. Get Live Vehicle Status (Battery, Charging, Health)
app.get('/api/driver/vehicle-status', authenticateToken, (req, res) => {
  db.get(`
    SELECT v.device_id, v.model_type, v.license_plate, t.*
    FROM vehicles v
    LEFT JOIN (
      SELECT * FROM telemetry_history ORDER BY timestamp DESC LIMIT 1
    ) t ON v.device_id = t.device_id
    WHERE v.user_id = ?
  `, [req.user.id], (err, row) => {
    if (err) return res.status(500).json({ error: err.message });
    if (!row) return res.status(404).json({ error: 'No vehicle bound to this account' });

    // Fallback Mock values if no active telemetry is logged yet
    const response = {
      device_id: row.device_id,
      model_type: row.model_type,
      license_plate: row.license_plate,
      timestamp: row.timestamp || new Date().toISOString(),
      soc: row.soc !== null ? row.soc : 78,
      soh: row.soh !== null ? row.soh : 98,
      battery_voltage: row.battery_voltage || 52.4,
      battery_current: row.battery_current !== null ? row.battery_current : 3.2,
      battery_temp: row.battery_temp || 26.5,
      solar_voltage: row.solar_voltage || 28.2,
      solar_power: row.solar_power !== null ? row.solar_power : 210.0,
      fan_duty: row.fan_duty !== null ? row.fan_duty : 0.45,
      solar_wh: row.solar_wh || 650.0,
      charge_wh: row.charge_wh || 480.0,
      cost_saved: row.cost_saved || 5.53,
      latitude: row.latitude || 28.6139,
      longitude: row.longitude || 77.2090
    };

    res.json(response);
  });
});

// 2. Get Savings Summary
app.get('/api/driver/savings-summary', authenticateToken, (req, res) => {
  db.all(`
    SELECT date(timestamp) as date, max(cost_saved) as daily_savings, max(solar_wh) as daily_solar_wh
    FROM telemetry_history
    WHERE device_id = (SELECT device_id FROM vehicles WHERE user_id = ?)
    GROUP BY date(timestamp)
    ORDER BY date DESC LIMIT 7
  `, [req.user.id], (err, rows) => {
    if (err) return res.status(500).json({ error: err.message });
    
    // Provide a mocked weekly log if database is empty
    if (rows.length === 0 || !rows[0].daily_savings) {
      const mockRows = [];
      for (let i = 0; i < 7; i++) {
        const d = new Date();
        d.setDate(d.getDate() - i);
        mockRows.push({
          date: d.toISOString().split('T')[0],
          daily_savings: (12.50 + Math.random() * 5).toFixed(2),
          daily_solar_wh: (1400 + Math.random() * 300).toFixed(0)
        });
      }
      return res.json(mockRows);
    }
    res.json(rows);
  });
});

// ----------------------------------------------------
// ADMIN ENDPOINTS
// ----------------------------------------------------

// 1. Get Customers List
app.get('/api/admin/users', authenticateToken, (req, res) => {
  if (req.user.role !== 'admin') return res.status(403).json({ error: 'Admin access required' });
  
  db.all(`
    SELECT u.id, u.name, u.email, u.phone, u.role, u.created_at, v.device_id, v.model_type
    FROM users u
    LEFT JOIN vehicles v ON u.id = v.user_id
    ORDER BY u.id DESC
  `, (err, rows) => {
    if (err) return res.status(500).json({ error: err.message });
    res.json(rows);
  });
});

// 2. Get Fleet Status
app.get('/api/admin/fleet-status', authenticateToken, (req, res) => {
  if (req.user.role !== 'admin') return res.status(403).json({ error: 'Admin access required' });

  db.all(`
    SELECT v.device_id, v.model_type, v.license_plate, u.name as driver_name, t.*
    FROM vehicles v
    JOIN users u ON v.user_id = u.id
    LEFT JOIN (
      SELECT device_id, max(timestamp) as last_time, soc, soh, battery_voltage, solar_power, latitude, longitude, cost_saved
      FROM telemetry_history
      GROUP BY device_id
    ) t ON v.device_id = t.device_id
  `, (err, rows) => {
    if (err) return res.status(500).json({ error: err.message });
    res.json(rows);
  });
});

app.use('/api/auth', authRouter);

// ----------------------------------------------------
// MQTT BROKER CLIENT BRIDGE
// ----------------------------------------------------

const BROKER_URL = 'mqtt://127.0.0.1:1883'; // Connect locally
const mqttClient = mqtt.connect(BROKER_URL);

// Base mock coordinates around New Delhi, India (seeding location data)
let mockLat = 28.6139;
let mockLng = 77.2090;

mqttClient.on('connect', () => {
  console.log('[Backend MQTT] Connected to local MQTT broker.');
  mqttClient.subscribe('ems/telemetry');
  mqttClient.subscribe('ems/faults');
});

mqttClient.on('message', (topic, message) => {
  const payloadStr = message.toString();
  try {
    const data = JSON.parse(payloadStr);
    
    if (topic === 'ems/telemetry') {
      const devId = data.device_id || 'RayGlides_EMS_9232C8';
      const soc = data.soc || 0;
      const soh = data.soh || 100;
      const battV = data.battery ? data.battery.voltage : 0.0;
      const battI = data.battery ? data.battery.current : 0.0;
      const battT = data.battery ? data.battery.temp : 25.0;
      const solarV = data.solar ? data.solar.voltage : 0.0;
      const solarP = data.solar ? data.solar.power : 0.0;
      const fanDuty = data.cooling ? data.cooling.fan_duty : 0.0;
      
      const solarWh = data.energy ? data.energy.solar_wh : 0.0;
      const chargeWh = data.energy ? data.energy.charge_wh : 0.0;
      
      // Savings calculation: cost saved in currency unit
      const costSaved = (solarWh / 1000.0) * TARIFF_RATE_PER_KWH;

      // Simulate a small vehicle movement/path for the maps visualization
      mockLat += (Math.random() - 0.5) * 0.001;
      mockLng += (Math.random() - 0.5) * 0.001;

      db.run(`
        INSERT INTO telemetry_history (
          device_id, timestamp, soc, soh, battery_voltage, battery_current, battery_temp,
          solar_voltage, solar_power, fan_duty, solar_wh, charge_wh, cost_saved, latitude, longitude
        ) VALUES (?, CURRENT_TIMESTAMP, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
      `, [devId, soc, soh, battV, battI, battT, solarV, solarP, fanDuty, solarWh, chargeWh, costSaved, mockLat, mockLng]);
      
      console.log(`[Backend MQTT] Synced telemetry for ${devId}: SOC=${soc}%, Solar=${solarP}W, Savings=${costSaved.toFixed(2)}`);
    }
  } catch (err) {
    console.error('[Backend MQTT] Error parsing message:', err);
  }
});

// Start Server
initDb().then(() => {
  app.listen(PORT, () => {
    console.log(`\n=======================================================`);
    console.log(`  RayGlides Backend Server Running At:`);
    console.log(`  http://localhost:${PORT}`);
    console.log(`=======================================================\n`);
  });
}).catch(err => {
  console.error('Failed to initialize database:', err);
});
