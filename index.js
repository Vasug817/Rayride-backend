// index.js

const express = require("express");
const cors = require("cors");
require("dotenv").config();

const app = express();
const port = process.env.PORT || 3000;

// Middleware
app.use(cors());
app.use(express.json());

// Routes
const authRoute = require('./routes/auth');
app.use('/api/auth', authRoute);

const usersRoute = require('./routes/users');
app.use('/api/users', usersRoute);

const ridesRoute = require('./routes/rides');
app.use('/api/rides', ridesRoute);

const batteryRoute = require('./routes/battery');
app.use('/api/battery', batteryRoute);

const mapsRoute = require('./routes/maps');
app.use('/api/maps', mapsRoute);

const walletRoutes = require('./routes/wallet');
app.use('/api/wallet', walletRoutes);

const notificationsRoute = require('./routes/notifications');
app.use('/api/notifications', notificationsRoute);

const matchRoute = require('./routes/match');
app.use('/api/match', matchRoute);

const negotiationRoutes = require('./routes/negotiation');
app.use('/api/negotiation', negotiationRoutes);

const queueRoutes = require('./routes/queue');
app.use('/api/queue', queueRoutes);

const syncRoutes = require('./routes/sync');
app.use('/api/sync', syncRoutes);

const paymentsRoute = require('./routes/payments');
app.use('/api/payments', paymentsRoute);


// Start Server
app.listen(port, () => {
  console.log(`🚀 Server running at http://localhost:${port}`);
});
