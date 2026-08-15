const express = require('express');
const router = express.Router();
const bcrypt = require('bcryptjs');
const jwt = require('jsonwebtoken');
const db = require('../services/firebase');

const JWT_SECRET = process.env.JWT_SECRET || 'rayglides_super_secret_jwt_key_2026';

// 1. SIGNUP
router.post('/signup', async (req, res) => {
  const { name, email, phone, password, role } = req.body;
  if (!name || !email || !password) {
    return res.status(400).json({ error: 'Name, email, and password are required' });
  }

  try {
    // Check if email already exists in Firestore users collection
    const querySnapshot = await db.collection('users').where('email', '==', email).get();
    if (!querySnapshot.empty) {
      return res.status(400).json({ error: 'Email address already registered' });
    }

    const hash = bcrypt.hashSync(password, 10);
    const userRole = role || 'driver';

    const newUserRef = await db.collection('users').add({
      name,
      email,
      phone: phone || null,
      role: userRole,
      passwordHash: hash,
      createdAt: new Date()
    });

    const userId = newUserRef.id;

    // Auto-create a mock vehicle binding if they are a driver
    if (userRole === 'driver') {
      const mockMac = 'RayGlides_EMS_' + Math.floor(Math.random() * 16777215).toString(16).toUpperCase();
      await db.collection('vehicles').add({
        userId,
        deviceId: mockMac,
        modelType: '3_wheeler',
        licensePlate: `DL-3S-MOCK-${userId.substring(0, 4)}`,
        createdAt: new Date()
      });
    }

    res.status(201).json({ success: true, message: 'User signed up successfully', userId });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// 2. SEND OTP
router.post('/send-otp', async (req, res) => {
  const { contact_info } = req.body;
  console.log("📩 send-otp request body:", req.body);
  if (!contact_info) {
    return res.status(400).json({ error: 'Contact info (email or phone) is required' });
  }

  // Generate a random 6-digit OTP code
  const code = Math.floor(100000 + Math.random() * 900000).toString();
  const expiresAt = Date.now() + 10 * 60 * 1000; // 10 minutes validity

  try {
    // Save securely in Firestore otp_codes collection
    await db.collection('otp_codes').add({
      contactInfo: contact_info,
      code: code,
      expiresAt: expiresAt,
      verified: false,
      createdAt: Date.now()
    });

    // Real-time SMS delivery implementation via Twilio if configured
    if (process.env.OTP_PROVIDER === 'twilio' && process.env.OTP_PROVIDER_API_KEY) {
      try {
        const parts = process.env.OTP_PROVIDER_API_KEY.split(':');
        const accountSid = parts[0];
        const authToken = parts[1];
        const fromNumber = process.env.TWILIO_FROM_NUMBER || '+1234567890';
        
        if (accountSid && authToken) {
          const authString = Buffer.from(`${accountSid}:${authToken}`).toString('base64');
          const bodyParams = new URLSearchParams({
            To: contact_info,
            From: fromNumber,
            Body: `Your RayRides verification code is: ${code}. Valid for 10 minutes.`
          });

          await fetch(`https://api.twilio.com/2010-04-01/Accounts/${accountSid}/Messages.json`, {
            method: 'POST',
            headers: {
              'Authorization': `Basic ${authString}`,
              'Content-Type': 'application/x-www-form-urlencoded'
            },
            body: bodyParams.toString()
          });
        }
      } catch (smsErr) {
        console.error("SMS delivery failed:", smsErr.message);
      }
    }

    // In compliance with strict production security guidelines, do NOT return the OTP code in the response body!
    res.json({
      success: true,
      message: 'OTP sent successfully'
    });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// 3. SIGNIN WITH OTP
router.post('/signin-otp', async (req, res) => {
  const { contact_info, code } = req.body;
  console.log("📩 signin-otp request body:", req.body);
  if (!contact_info || !code) {
    return res.status(400).json({ error: 'Contact info and OTP code are required' });
  }

  const now = Date.now();

  try {
    // Query matching unverified OTP code
    const otpQuery = await db.collection('otp_codes')
      .where('contactInfo', '==', contact_info)
      .where('code', '==', code)
      .where('verified', '==', false)
      .get();

    if (otpQuery.empty) {
      return res.status(400).json({ error: 'Invalid or expired OTP code' });
    }

    // Sort/filter by expiration date locally to check valid validity window
    let validOtpDoc = null;
    for (const doc of otpQuery.docs) {
      const data = doc.data();
      if (data.expiresAt > now) {
        validOtpDoc = doc;
        break;
      }
    }

    if (!validOtpDoc) {
      return res.status(400).json({ error: 'Invalid or expired OTP code' });
    }

    // Mark OTP as verified (single-use!)
    await db.collection('otp_codes').doc(validOtpDoc.id).update({
      verified: true,
      verifiedAt: now
    });

    // Check if user exists by email or phone
    let userQuery = await db.collection('users').where('email', '==', contact_info).get();
    if (userQuery.empty) {
      userQuery = await db.collection('users').where('phone', '==', contact_info).get();
    }

    if (userQuery.empty) {
      // Auto-signup user if they don't exist
      const tempName = contact_info.split('@')[0] || 'Guest User';
      const isEmail = contact_info.includes('@');
      
      const newUserRef = await db.collection('users').add({
        name: tempName,
        email: isEmail ? contact_info : `${tempName}@rayrides.com`,
        phone: isEmail ? null : contact_info,
        role: 'driver', // Default to driver as per original spec requirements
        createdAt: new Date()
      });

      const newUserId = newUserRef.id;
      const mockMac = 'RayGlides_EMS_9232C8';
      await db.collection('vehicles').add({
        userId: newUserId,
        deviceId: mockMac,
        modelType: '3_wheeler',
        licensePlate: 'DL-3S-AUTO',
        createdAt: new Date()
      });

      const token = jwt.sign({ id: newUserId, role: 'driver', name: tempName }, JWT_SECRET, { expiresIn: '7d' });
      res.json({ success: true, token, user: { id: newUserId, name: tempName, role: 'driver' } });
    } else {
      const userDoc = userQuery.docs[0];
      const userData = userDoc.data();
      const token = jwt.sign({ id: userDoc.id, role: userData.role, name: userData.name }, JWT_SECRET, { expiresIn: '7d' });
      res.json({ success: true, token, user: { id: userDoc.id, name: userData.name, role: userData.role, email: userData.email } });
    }
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// 4. SIGNIN WITH PASSWORD
router.post('/signin-password', async (req, res) => {
  const { email, password } = req.body;
  if (!email || !password) {
    return res.status(400).json({ error: 'Email and password are required' });
  }

  try {
    const querySnapshot = await db.collection('users').where('email', '==', email).get();
    if (querySnapshot.empty) {
      return res.status(401).json({ error: 'Invalid email or password' });
    }

    const userDoc = querySnapshot.docs[0];
    const userData = userDoc.data();

    if (!userData.passwordHash || !bcrypt.compareSync(password, userData.passwordHash)) {
      return res.status(401).json({ error: 'Invalid email or password' });
    }

    const token = jwt.sign({ id: userDoc.id, role: userData.role, name: userData.name }, JWT_SECRET, { expiresIn: '7d' });
    res.json({
      success: true,
      token,
      user: { id: userDoc.id, name: userData.name, role: userData.role, email: userData.email }
    });
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// 5. GOOGLE SIGNIN
router.post('/google-login', async (req, res) => {
  const { email, name, googleId } = req.body;
  if (!email || !name) {
    return res.status(400).json({ error: 'Google email and name are required' });
  }

  try {
    const querySnapshot = await db.collection('users').where('email', '==', email).get();
    if (querySnapshot.empty) {
      const newUserRef = await db.collection('users').add({
        name,
        email,
        role: 'driver',
        authProvider: 'google',
        createdAt: new Date()
      });

      const newUserId = newUserRef.id;
      await db.collection('vehicles').add({
        userId: newUserId,
        deviceId: 'RayGlides_EMS_9232C8',
        modelType: '3_wheeler',
        licensePlate: 'DL-3S-GOOG',
        createdAt: new Date()
      });

      const token = jwt.sign({ id: newUserId, role: 'driver', name }, JWT_SECRET, { expiresIn: '7d' });
      res.status(201).json({ success: true, token, user: { id: newUserId, name, role: 'driver', email } });
    } else {
      const userDoc = querySnapshot.docs[0];
      const userData = userDoc.data();
      const token = jwt.sign({ id: userDoc.id, role: userData.role, name: userData.name }, JWT_SECRET, { expiresIn: '7d' });
      res.json({ success: true, token, user: { id: userDoc.id, name: userData.name, role: userData.role, email: userData.email } });
    }
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// GET /api/auth/otp-status
router.get('/otp-status', (req, res) => {
  const provider = process.env.OTP_PROVIDER;
  const apiKey = process.env.OTP_PROVIDER_API_KEY;
  
  res.json({
    provider: provider || 'not_configured',
    hasApiKey: !!apiKey,
    apiKeyLength: apiKey ? apiKey.length : 0,
    apiKeyParts: apiKey ? apiKey.split(':').length : 0,
    fromNumber: process.env.TWILIO_FROM_NUMBER || 'not_configured'
  });
});

module.exports = router;
