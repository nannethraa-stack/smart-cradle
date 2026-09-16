const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const path = require('path');
const crypto = require('crypto');
const db = require('./db');
const mqttClient = require('./mqtt-client');
const monitoring = require('./monitoring');

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

app.use(express.json({ limit: '1mb' }));
app.use(express.static(path.join(__dirname, 'public')));

function limitOf(value, fallback, max = 5000) {
  const n = Number.parseInt(value, 10);
  return Number.isFinite(n) ? Math.min(Math.max(n, 1), max) : fallback;
}

mqttClient.setBroadcast((data) => {
  const msg = JSON.stringify(data);
  wss.clients.forEach(client => {
    if (client.readyState === WebSocket.OPEN) client.send(msg);
  });
});

app.get('/api/telemetry', (req, res) =>
  res.json(db.getLatestTelemetry(limitOf(req.query.limit, 50), req.query.deviceId || null)));

app.get('/api/telemetry/history', (req, res) => {
  if (!req.query.deviceId) return res.status(400).json({ error: 'deviceId is required' });
  res.json(db.getTelemetryHistory(req.query.deviceId, limitOf(req.query.limit, 500)));
});

app.get('/api/images', (req, res) =>
  res.json(db.getLatestImages(limitOf(req.query.limit, 20))));

app.get('/api/images/:id', (req, res) => {
  const img = db.getImageById(req.params.id);
  if (!img) return res.status(404).json({ error: 'Image not found' });
  res.json(img);
});

app.get('/api/alerts', (req, res) =>
  res.json(db.getLatestAlerts(limitOf(req.query.limit, 30))));

app.get('/api/presence', (req, res) =>
  res.json(db.getPresenceHistory(limitOf(req.query.limit, 20))));

app.get('/admin/health', (req, res) => res.json(monitoring.getHealthSummary()));
app.get('/admin/devices', (req, res) => res.json(monitoring.getDeviceStatus()));
app.get('/admin/devices/:deviceId', (req, res) => {
  const device = monitoring.getDeviceStatus(req.params.deviceId);
  if (!device) return res.status(404).json({ error: 'Device not found' });
  res.json(device);
});
app.get('/admin/anomalies', (req, res) =>
  res.json(monitoring.getRecentAnomalies(limitOf(req.query.limit, 50), req.query.severity || null)));
app.get('/admin/anomalies/critical', (req, res) =>
  res.json(monitoring.getRecentAnomalies(100, 'critical')));

app.get('/admin/telemetry', (req, res) =>
  res.json(db.getLatestTelemetry(limitOf(req.query.limit, 200), req.query.deviceId || null)));

app.get('/admin/samples', (req, res) => {
  if (!req.query.deviceId) return res.status(400).json({ error: 'deviceId is required' });
  res.json(db.getSensorSamples(req.query.deviceId, limitOf(req.query.limit, 500), req.query.sensor || null));
});

app.get('/admin/events', (req, res) =>
  res.json(db.getSystemEvents(limitOf(req.query.limit, 200), req.query.deviceId || null)));

app.get('/admin/audio', (req, res) => {
  if (!req.query.deviceId) return res.status(400).json({ error: 'deviceId is required' });
  res.json(db.getAudioEvents(req.query.deviceId, limitOf(req.query.limit, 100)));
});

app.get('/admin/audio/:id', (req, res) => {
  const item = db.getAudioEventById(req.params.id);
  if (!item) return res.status(404).json({ error: 'Audio event not found' });
  res.json(item);
});

app.get('/admin/cry-inferences', (req, res) => {
  if (!req.query.deviceId) return res.status(400).json({ error: 'deviceId is required' });
  res.json(db.getCryInferences(req.query.deviceId, limitOf(req.query.limit, 100)));
});

app.get('/admin/cry-reviews', (req, res) => {
  if (!req.query.deviceId) {
    return res.status(400).json({ error: 'deviceId is required' });
  }

  res.json(
    db.getCryReviews(
      req.query.deviceId,
      limitOf(req.query.limit, 100)
    )
  );
});

app.post('/admin/cry-reviews', express.json({ limit: '1mb' }), (req, res) => {
  const body = req.body || {};

  if (!body.audio_event_id) {
    return res.status(400).json({ error: 'audio_event_id is required' });
  }

  if (!body.device_id) {
    return res.status(400).json({ error: 'device_id is required' });
  }

  const allowedDecisions = [
    'CONFIRM',
    'CORRECT',
    'UNABLE_TO_DETERMINE'
  ];

  const allowedPatterns = [
    'HUNGER',
    'PAIN',
    'DISCOMFORT',
    'TIRED',
    'BURPING',
    'OTHER'
  ];

  const allowedTrainingStatuses = [
    'CANDIDATE',
    'APPROVED',
    'EXCLUDED'
  ];

  const allowedReviewStatuses = [
    'NOT_REVIEWED',
    'REVIEWED'
  ];

  const reviewStatus = body.review_status || 'REVIEWED';
  const trainingStatus = body.training_status || 'CANDIDATE';

  if (!allowedReviewStatuses.includes(reviewStatus)) {
    return res.status(400).json({
      error: 'Invalid review_status'
    });
  }

  if (!allowedTrainingStatuses.includes(trainingStatus)) {
    return res.status(400).json({
      error: 'Invalid training_status'
    });
  }

  if (body.human_decision &&
      !allowedDecisions.includes(body.human_decision)) {
    return res.status(400).json({
      error: 'Invalid human_decision'
    });
  }

  if (body.human_pattern &&
      !allowedPatterns.includes(body.human_pattern)) {
    return res.status(400).json({
      error: 'Invalid human_pattern'
    });
  }

  if (body.human_decision === 'CORRECT' && !body.human_pattern) {
    return res.status(400).json({
      error: 'human_pattern is required when human_decision is CORRECT'
    });
  }

  const result = db.insertCryReview({
    audio_event_id: body.audio_event_id,
    device_id: body.device_id,
    review_status: reviewStatus,
    human_decision: body.human_decision || null,
    human_pattern: body.human_pattern || null,
    evidence_note: body.evidence_note || null,
    training_status: trainingStatus,
    reviewer: body.reviewer || null,
    reviewed_at: body.reviewed_at || new Date().toISOString()
  });

  const review = db.getCryReviews(body.device_id, 1);

  res.status(201).json({
    id: result.lastInsertRowid,
    review: review[0] || null
  });
});

app.get('/admin/stats', (req, res) => {
  const summary = monitoring.getHealthSummary();
  const memUsage = process.memoryUsage();
  res.json({
    ...summary,
    ...db.getAdminCounts(),
    memory: {
      rss: `${(memUsage.rss / 1024 / 1024).toFixed(1)} MB`,
      heapUsed: `${(memUsage.heapUsed / 1024 / 1024).toFixed(1)} MB`,
      heapTotal: `${(memUsage.heapTotal / 1024 / 1024).toFixed(1)} MB`
    },
    nodeVersion: process.version,
    platform: process.platform
  });
});

// QR sharing: token is stored hashed and expires. Do not put baby data in the QR.
app.post('/api/share', (req, res) => {
  const deviceId = req.body?.deviceId;
  const hours = Math.min(Math.max(Number(req.body?.hours) || 4, 1), 72);
  if (!deviceId) return res.status(400).json({ error: 'deviceId is required' });

  const rawToken = crypto.randomBytes(32).toString('base64url');
  const tokenHash = crypto.createHash('sha256').update(rawToken).digest('hex');
  const expiresAt = new Date(Date.now() + hours * 3600000).toISOString().replace('T', ' ').replace('Z', '');
  db.createShareToken(tokenHash, deviceId, expiresAt);

  const publicUrl = `${req.protocol}://${req.get('host')}/mobile.html?token=${encodeURIComponent(rawToken)}`;
  res.json({ url: publicUrl, token: rawToken, expiresAt });
});

function getShare(req, res, next) {
  const rawToken = req.query.token;
  if (!rawToken) return res.status(401).json({ error: 'Share token required' });
  const tokenHash = crypto.createHash('sha256').update(rawToken).digest('hex');
  const share = db.getShareToken(tokenHash);
  if (!share) return res.status(401).json({ error: 'Share token invalid, expired, or revoked' });
  req.share = share;
  next();
}

app.get('/api/shared/snapshot', getShare, (req, res) =>
  res.json(db.getPublicSnapshot(req.share.device_id)));

app.post('/api/share/revoke', (req, res) => {
  if (!req.body?.token) return res.status(400).json({ error: 'token is required' });
  const hash = crypto.createHash('sha256').update(req.body.token).digest('hex');
  res.json({ revoked: db.revokeShareToken(hash).changes > 0 });
});

wss.on('connection', ws => {
  ws.send(JSON.stringify({
    type: 'info',
    data: { message: 'Connected to Smart Cradle backend', server_time: new Date().toISOString() }
  }));
});

monitoring.on('anomaly', anomaly => {
  const msg = JSON.stringify({ type: 'anomaly', data: anomaly });
  wss.clients.forEach(client => {
    if (client.readyState === WebSocket.OPEN) client.send(msg);
  });
});

setInterval(() => monitoring.checkStaleDevices(), 60000);

const PORT = process.env.PORT || 3001;
server.listen(PORT, () => {
  console.log(`Server listening on http://localhost:${PORT}`);
  console.log('Data source policy: LIVE MQTT HARDWARE DATA ONLY (no simulation endpoints)');
  console.log(`Admin dashboard: http://localhost:${PORT}/admin/index.html`);
  mqttClient.start();
});
