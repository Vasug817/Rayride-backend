const express = require('express');
const Razorpay = require('razorpay');
const crypto = require('crypto');
const router = express.Router();
const db = require('../services/firebase');

const razorpay = new Razorpay({
  key_id: process.env.RAZORPAY_KEY || process.env.RAZORPAY_KEY_ID,
  key_secret: process.env.RAZORPAY_SECRET || process.env.RAZORPAY_KEY_SECRET
});

// ✅ Create payment order
router.post('/create-order', async (req, res) => {
  const { amount, rideId, userId } = req.body;

  const options = {
    amount: amount * 100, // Razorpay expects amount in paise
    currency: "INR",
    receipt: `rcpt_${rideId}`
  };

  try {
    const order = await razorpay.orders.create(options);

    await db.collection("payments").add({
      userId,
      rideId,
      amount,
      orderId: order.id,
      status: "pending",
      createdAt: Date.now()
    });

    res.send({ orderId: order.id });
  } catch (err) {
    res.status(500).send({ error: err.message });
  }
});

// ✅ Verify payment
router.post('/verify-payment', async (req, res) => {
  const { razorpay_order_id, razorpay_payment_id, razorpay_signature } = req.body;

  const generated_signature = crypto
    .createHmac('sha256', process.env.RAZORPAY_SECRET || process.env.RAZORPAY_KEY_SECRET)
    .update(`${razorpay_order_id}|${razorpay_payment_id}`)
    .digest('hex');

  if (generated_signature === razorpay_signature) {
    try {
      // Update status in Firestore
      const snapshot = await db.collection("payments")
        .where("orderId", "==", razorpay_order_id).get();

      if (!snapshot.empty) {
        const paymentDoc = snapshot.docs[0];
        await db.collection("payments").doc(paymentDoc.id).update({
          status: "success",
          paymentId: razorpay_payment_id,
          verifiedAt: Date.now()
        });
      }

      res.send({ message: "Payment verified!" });
    } catch (err) {
      res.status(500).send({ error: err.message });
    }
  } else {
    res.status(400).send({ error: "Invalid signature" });
  }
});

module.exports = router;
