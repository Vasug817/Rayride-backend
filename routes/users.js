const express = require('express');
const router = express.Router();
const db = require('../services/firebase');

// Register a user
router.post('/register', async (req, res) => {
  const { name, role, phone, batteryLevel, location } = req.body;

  if (!name || !role || !phone || batteryLevel === undefined || !location) {
    return res.status(400).send({ error: 'Missing required fields' });
  }

  try {
    const newUser = await db.collection('users').add({
      name,
      role,
      phone,
      batteryLevel,
      currentLocation: location,
      isOnline: true,
      createdAt: new Date()
    });

    res.status(200).send({ id: newUser.id, message: "User added!" });
  } catch (err) {
    res.status(500).send({ error: err.message });
  }
});

// Get all users
router.get('/', async (req, res) => {
  try {
    const snapshot = await db.collection('users').get();
    const users = snapshot.docs.map(doc => ({ id: doc.id, ...doc.data() }));
    res.send(users);
  } catch (err) {
    res.status(500).send({ error: err.message });
  }
});

// Get user by ID
router.get('/:id', async (req, res) => {
  try {
    const doc = await db.collection('users').doc(req.params.id).get();
    if (!doc.exists) {
      return res.status(404).send({ error: 'User not found' });
    }
    res.send({ id: doc.id, ...doc.data() });
  } catch (err) {
    res.status(500).send({ error: err.message });
  }
});

// Get all rides for a specific user (commuter or driver)
router.get('/:id/rides', async (req, res) => {
  const userId = req.params.id;
  const { role } = req.query; // role=commuter or driver

  try {
    let query;
    if (role === 'commuter') {
      query = db.collection('rides').where('commuterId', '==', userId);
    } else if (role === 'driver') {
      query = db.collection('rides').where('driverId', '==', userId);
    } else {
      return res.status(400).send({ error: "Query param 'role' is required as 'commuter' or 'driver'" });
    }

    const snapshot = await query.get();
    const rides = snapshot.docs.map(doc => ({ id: doc.id, ...doc.data() }));

    res.send(rides);
  } catch (err) {
    res.status(500).send({ error: err.message });
  }
});


module.exports = router;
