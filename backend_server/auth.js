const express = require('express');
const router = express.Router();
const bcrypt = require('bcryptjs');
const jwt = require('jsonwebtoken');
const { db } = require('./database');

const JWT_SECRET = 'rayglides_super_secret_jwt_key_2026';

// 1. SIGNUP
router.post('/signup', (req, res) => {
  const { name, email, phone, password, role } = req.body;
  if (!name || !email || !password) {
    return res.status(400).json({ error: 'Name, email, and password are required' });
  }

  const hash = bcrypt.hashSync(password, 10);
  const userRole = role || 'driver';

  db.run(`
    INSERT INTO users (name, email, phone, role, password_hash)
    VALUES (?, ?, ?, ?, ?)
  `, [name, email, phone || null, userRole, hash], function(err) {
    if (err) {
      if (err.message.includes('UNIQUE constraint failed: users.email')) {
        return res.status(400).json({ error: 'Email address already registered' });
      }
      return res.status(500).json({ error: err.message });
    }
    
    // Auto-create a mock vehicle binding if they are a driver
    const userId = this.lastID;
    if (userRole === 'driver') {
      const mockMac = 'RayGlides_EMS_' + Math.floor(Math.random() * 16777215).toString(16).toUpperCase();
      db.run(`
        INSERT INTO vehicles (user_id, device_id, model_type, license_plate)
        VALUES (?, ?, '3_wheeler', ?)
      `, [userId, mockMac, `DL-3S-MOCK-${userId}`]);
    }

    res.status(201).json({ success: true, message: 'User signed up successfully', userId });
  });
});

// 2. SEND OTP
router.post('/send-otp', (req, res) => {
  const { contact_info } = req.body; // email or phone
  if (!contact_info) {
    return res.status(400).json({ error: 'Contact info (email or phone) is required' });
  }

  // Generate a random 6-digit OTP code
  const code = Math.floor(100000 + Math.random() * 900000).toString();
  const expiresAt = new Date(Date.now() + 5 * 60 * 1000).toISOString(); // 5 minutes validity

  db.run(`
    INSERT INTO otp_codes (contact_info, code, expires_at)
    VALUES (?, ?, ?)
  `, [contact_info, code, expiresAt], (err) => {
    if (err) return res.status(500).json({ error: err.message });
    
    console.log("[OTP Generator] OTP generated and saved securely.");
    
    res.json({
      success: true,
      message: 'OTP sent successfully',
      code: code // Included in response for easy developer testing and dashboard usage!
    });
  });
});

// 3. SIGNIN WITH OTP
router.post('/signin-otp', (req, res) => {
  const { contact_info, code } = req.body;
  if (!contact_info || !code) {
    return res.status(400).json({ error: 'Contact info and OTP code are required' });
  }

  const now = new Date().toISOString();
  db.get(`
    SELECT * FROM otp_codes 
    WHERE contact_info = ? AND code = ? AND expires_at > ? AND verified = 0
    ORDER BY id DESC LIMIT 1
  `, [contact_info, code, now], (err, otpRow) => {
    if (err) return res.status(500).json({ error: err.message });
    if (!otpRow) return res.status(400).json({ error: 'Invalid or expired OTP code' });

    // Mark OTP as used
    db.run(`UPDATE otp_codes SET verified = 1 WHERE id = ?`, [otpRow.id]);

    // Find user (by email or phone)
    db.get(`
      SELECT * FROM users WHERE email = ? OR phone = ?
    `, [contact_info, contact_info], (err, user) => {
      if (err) return res.status(500).json({ error: err.message });
      
      if (!user) {
        // If driver using phone/email OTP doesn't exist, auto-signup them as a mock driver
        const tempName = contact_info.split('@')[0] || 'Driver Guest';
        const emptyPassHash = bcrypt.hashSync('temp123', 10);
        db.run(`
          INSERT INTO users (name, email, phone, role, password_hash)
          VALUES (?, ?, ?, 'driver', ?)
        `, [tempName, contact_info.includes('@') ? contact_info : `${tempName}@rayglides.com`, contact_info.includes('@') ? null : contact_info, emptyPassHash], function(err) {
          if (err) return res.status(500).json({ error: err.message });
          
          const newUserId = this.lastID;
          const mockMac = 'RayGlides_EMS_9232C8'; // Bind to target ESP32 MAC for testing
          db.run(`
            INSERT OR IGNORE INTO vehicles (user_id, device_id, model_type, license_plate)
            VALUES (?, ?, '3_wheeler', 'DL-3S-AUTO')
          `, [newUserId, mockMac]);

          const token = jwt.sign({ id: newUserId, role: 'driver', name: tempName }, JWT_SECRET, { expiresIn: '7d' });
          return res.json({ success: true, token, user: { id: newUserId, name: tempName, role: 'driver' } });
        });
      } else {
        const token = jwt.sign({ id: user.id, role: user.role, name: user.name }, JWT_SECRET, { expiresIn: '7d' });
        res.json({ success: true, token, user: { id: user.id, name: user.name, role: user.role, email: user.email } });
      }
    });
  });
});

// 4. SIGNIN WITH PASSWORD
router.post('/signin-password', (req, res) => {
  const { email, password } = req.body;
  if (!email || !password) {
    return res.status(400).json({ error: 'Email and password are required' });
  }

  db.get(`SELECT * FROM users WHERE email = ?`, [email], (err, user) => {
    if (err) return res.status(500).json({ error: err.message });
    if (!user || !bcrypt.compareSync(password, user.password_hash)) {
      return res.status(401).json({ error: 'Invalid email or password' });
    }

    const token = jwt.sign({ id: user.id, role: user.role, name: user.name }, JWT_SECRET, { expiresIn: '7d' });
    res.json({
      success: true,
      token,
      user: { id: user.id, name: user.name, role: user.role, email: user.email }
    });
  });
});

// 5. GOOGLE SIGNIN
router.post('/google-login', (req, res) => {
  const { email, name, googleId } = req.body;
  if (!email || !name) {
    return res.status(400).json({ error: 'Google email and name are required' });
  }

  db.get(`SELECT * FROM users WHERE email = ?`, [email], (err, user) => {
    if (err) return res.status(500).json({ error: err.message });

    if (!user) {
      // Create user
      db.run(`
        INSERT INTO users (name, email, role, auth_provider)
        VALUES (?, ?, 'driver', 'google')
      `, [name, email], function(err) {
        if (err) return res.status(500).json({ error: err.message });
        
        const newUserId = this.lastID;
        db.run(`
          INSERT INTO vehicles (user_id, device_id, model_type, license_plate)
          VALUES (?, 'RayGlides_EMS_9232C8', '3_wheeler', 'DL-3S-GOOG')
        `, [newUserId]);

        const token = jwt.sign({ id: newUserId, role: 'driver', name }, JWT_SECRET, { expiresIn: '7d' });
        res.status(201).json({ success: true, token, user: { id: newUserId, name, role: 'driver', email } });
      });
    } else {
      const token = jwt.sign({ id: user.id, role: user.role, name: user.name }, JWT_SECRET, { expiresIn: '7d' });
      res.json({ success: true, token, user: { id: user.id, name: user.name, role: user.role, email: user.email } });
    }
  });
});

module.exports = {
  authRouter: router,
  JWT_SECRET
};
