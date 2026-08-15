const admin = require('firebase-admin');

let serviceAccount;
if (process.env.FIREBASE_SERVICE_ACCOUNT_JSON) {
  try {
    serviceAccount = JSON.parse(process.env.FIREBASE_SERVICE_ACCOUNT_JSON);
  } catch (e) {
    console.error("Failed to parse FIREBASE_SERVICE_ACCOUNT_JSON:", e);
  }
}

if (!serviceAccount) {
  try {
    serviceAccount = require('../serviceAccountKey.json');
  } catch (e) {
    console.warn("serviceAccountKey.json not found, initializing in mock mode.");
  }
}

if (serviceAccount) {
  admin.initializeApp({
    credential: admin.credential.cert(serviceAccount),
    databaseURL: process.env.FIREBASE_DATABASE_URL || "https://rayrides5706.firebaseio.com"
  });
} else {
  admin.initializeApp({
    projectId: process.env.FIREBASE_PROJECT_ID || "rayrides5706"
  });
}

const db = admin.firestore();

module.exports = db;
