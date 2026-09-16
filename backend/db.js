const Database = require('better-sqlite3');
const path = require('path');

const db = new Database(path.join(__dirname, 'cradle.db'));
db.pragma('journal_mode = WAL');

db.exec(`
  CREATE TABLE IF NOT EXISTS telemetry (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_id TEXT UNIQUE,
    device_id TEXT,
    event_type TEXT,
    occurred_at TEXT,
    sequence_no INTEGER,
    firmware_version TEXT,
    temperature REAL,
    respiratory_rate REAL,
    respiratory_confidence REAL,
    crying INTEGER,
    cry_detected INTEGER,
    cry_probability REAL,
    probable_pattern TEXT,
    cry_model_version TEXT,
    audio_event_id TEXT,
    ammonia_ppm REAL,
    diaper_soiled INTEGER,
    diaper_status TEXT,
    diaper_weight_delta_g REAL,
    diaper_fusion_reason TEXT,
    baby_weight REAL,
    baby_height_cm REAL,
    quality REAL,
    validation_status TEXT,
    received_at DATETIME DEFAULT CURRENT_TIMESTAMP
  );

  CREATE TABLE IF NOT EXISTS alerts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_id TEXT UNIQUE,
    device_id TEXT,
    event_type TEXT,
    occurred_at TEXT,
    sequence_no INTEGER,
    firmware_version TEXT,
    sensor TEXT,
    alert_type TEXT,
    consecutive_failures INTEGER,
    quality REAL,
    received_at DATETIME DEFAULT CURRENT_TIMESTAMP
  );

  CREATE TABLE IF NOT EXISTS presence_changes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_id TEXT UNIQUE,
    device_id TEXT,
    occurred_at TEXT,
    baby_present INTEGER,
    previous_state TEXT,
    current_state TEXT,
    received_at DATETIME DEFAULT CURRENT_TIMESTAMP
  );

  CREATE TABLE IF NOT EXISTS images (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_id TEXT UNIQUE,
    device_id TEXT,
    occurred_at TEXT,
    sequence_no INTEGER,
    width INTEGER,
    height INTEGER,
    format TEXT,
    data_base64 TEXT,
    received_at DATETIME DEFAULT CURRENT_TIMESTAMP
  );

  CREATE TABLE IF NOT EXISTS device_status (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_id TEXT UNIQUE,
    device_id TEXT,
    occurred_at TEXT,
    baby_present INTEGER,
    ambient_temperature REAL,
    quality REAL,
    received_at DATETIME DEFAULT CURRENT_TIMESTAMP
  );

  CREATE TABLE IF NOT EXISTS sensor_samples (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT,
    event_id TEXT,
    occurred_at TEXT,
    sensor TEXT,
    raw_value REAL,
    processed_value REAL,
    unit TEXT,
    status TEXT,
    metadata_json TEXT,
    received_at DATETIME DEFAULT CURRENT_TIMESTAMP
  );

  CREATE TABLE IF NOT EXISTS audio_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_id TEXT UNIQUE,
    device_id TEXT,
    occurred_at TEXT,
    sample_rate INTEGER,
    channels INTEGER,
    sample_count INTEGER,
    format TEXT,
    model_version TEXT,
    training_eligible INTEGER DEFAULT 0,
    data_base64 TEXT,
    received_at DATETIME DEFAULT CURRENT_TIMESTAMP
  );

  CREATE TABLE IF NOT EXISTS cry_inferences (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    audio_event_id TEXT,
    device_id TEXT,
    occurred_at TEXT,
    model_version TEXT,
    status TEXT,
    probable_pattern TEXT,
    probability REAL,
    inference_ms INTEGER,
    features_json TEXT,
    error TEXT,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
  );

  CREATE TABLE IF NOT EXISTS cry_reviews (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    audio_event_id TEXT NOT NULL,
    device_id TEXT,
    review_status TEXT NOT NULL DEFAULT 'NOT_REVIEWED',
    human_decision TEXT,
    human_pattern TEXT,
    evidence_note TEXT,
    training_status TEXT NOT NULL DEFAULT 'CANDIDATE',
    reviewer TEXT,
    reviewed_at DATETIME,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
  );

  CREATE INDEX IF NOT EXISTS idx_cry_reviews_audio_event
    ON cry_reviews(audio_event_id);

  CREATE INDEX IF NOT EXISTS idx_cry_reviews_device_status
    ON cry_reviews(device_id, review_status);

  CREATE INDEX IF NOT EXISTS idx_cry_reviews_training_status
    ON cry_reviews(training_status);

  CREATE TABLE IF NOT EXISTS system_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    event_id TEXT,
    device_id TEXT,
    occurred_at TEXT,
    event_type TEXT,
    severity TEXT,
    component TEXT,
    message TEXT,
    metadata_json TEXT,
    received_at DATETIME DEFAULT CURRENT_TIMESTAMP
  );

  CREATE TABLE IF NOT EXISTS share_tokens (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    token_hash TEXT UNIQUE,
    device_id TEXT,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    expires_at DATETIME,
    revoked INTEGER DEFAULT 0
  );

  CREATE INDEX IF NOT EXISTS idx_telemetry_device_time ON telemetry(device_id, received_at);
  CREATE INDEX IF NOT EXISTS idx_telemetry_occurred ON telemetry(device_id, occurred_at);
  CREATE INDEX IF NOT EXISTS idx_images_device_time ON images(device_id, received_at);
  CREATE INDEX IF NOT EXISTS idx_samples_device_time ON sensor_samples(device_id, occurred_at);
  CREATE INDEX IF NOT EXISTS idx_system_events_time ON system_events(received_at);
  CREATE INDEX IF NOT EXISTS idx_audio_events_device_time ON audio_events(device_id, occurred_at);
  CREATE INDEX IF NOT EXISTS idx_cry_inferences_device_time ON cry_inferences(device_id, occurred_at);
  CREATE INDEX IF NOT EXISTS idx_share_token_hash ON share_tokens(token_hash);
`);

function ensureColumn(table, column, definition) {
  const columns = db.prepare(`PRAGMA table_info(${table})`).all();
  if (!columns.some(c => c.name === column)) {
    db.exec(`ALTER TABLE ${table} ADD COLUMN ${column} ${definition}`);
  }
}

// Safe migrations for databases created by v1/v2.
[
  ['telemetry', 'cry_detected', 'INTEGER'],
  ['telemetry', 'cry_probability', 'REAL'],
  ['telemetry', 'probable_pattern', 'TEXT'],
  ['telemetry', 'cry_model_version', 'TEXT'],
  ['telemetry', 'audio_event_id', 'TEXT'],
  ['telemetry', 'diaper_status', 'TEXT'],
  ['telemetry', 'diaper_weight_delta_g', 'REAL'],
  ['telemetry', 'diaper_fusion_reason', 'TEXT'],
  ['telemetry', 'baby_height_cm', 'REAL'],
  ['telemetry', 'validation_status', 'TEXT'],
].forEach(([t, c, d]) => ensureColumn(t, c, d));

function insertTelemetry(data) {
  const stmt = db.prepare(`
    INSERT OR IGNORE INTO telemetry
    (event_id, device_id, event_type, occurred_at, sequence_no, firmware_version,
     temperature, respiratory_rate, respiratory_confidence, crying,
     cry_detected, cry_probability, probable_pattern, cry_model_version, audio_event_id,
     ammonia_ppm, diaper_soiled, diaper_status, diaper_weight_delta_g, diaper_fusion_reason,
     baby_weight, baby_height_cm, quality, validation_status)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
  `);
  const p = data.payload || {};
  return stmt.run(
    data.event_id, data.device_id, data.event_type, data.occurred_at,
    data.sequence_no, data.firmware_version,
    p.temperature ?? null, p.respiratory_rate ?? null,
    p.respiratory_confidence ?? null, p.crying ? 1 : 0,
    p.cry_detected != null ? (p.cry_detected ? 1 : 0) : (p.crying ? 1 : 0),
    p.cry_probability ?? null,
    p.probable_pattern ?? null,
    p.cry_model_version ?? null,
    p.audio_event_id ?? null,
    p.ammonia_ppm ?? null,
    p.diaper_soiled != null ? (p.diaper_soiled ? 1 : 0) : null,
    p.diaper_status ?? null,
    p.diaper_weight_delta_g ?? null,
    p.diaper_fusion_reason ?? null,
    p.baby_weight ?? null,
    p.baby_height_cm ?? p.baby_height ?? p.height_cm ?? null,
    data.quality ?? null,
    p.validation_status ?? (data.quality != null && data.quality >= 0.5 ? 'validated' : 'unverified')
  );
}

function insertAlert(data) {
  const stmt = db.prepare(`
    INSERT OR IGNORE INTO alerts
    (event_id, device_id, event_type, occurred_at, sequence_no, firmware_version,
     sensor, alert_type, consecutive_failures, quality)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
  `);
  const p = data.payload || {};
  return stmt.run(
    data.event_id, data.device_id, data.event_type, data.occurred_at,
    data.sequence_no, data.firmware_version,
    p.sensor ?? null, p.alert_type ?? null,
    p.consecutive_failures ?? null, data.quality ?? null
  );
}

function insertPresenceChange(data) {
  const stmt = db.prepare(`
    INSERT OR IGNORE INTO presence_changes
    (event_id, device_id, occurred_at, baby_present, previous_state, current_state)
    VALUES (?, ?, ?, ?, ?, ?)
  `);
  const p = data.payload || {};
  return stmt.run(
    data.event_id, data.device_id, data.occurred_at,
    p.baby_present ? 1 : 0, p.previous_state ?? null, p.current_state ?? null
  );
}

function insertImage(data) {
  const stmt = db.prepare(`
    INSERT OR IGNORE INTO images
    (event_id, device_id, occurred_at, sequence_no, width, height, format, data_base64)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?)
  `);
  const p = data.payload || {};
  return stmt.run(
    data.event_id, data.device_id, data.occurred_at,
    data.sequence_no, p.width ?? null, p.height ?? null,
    p.format ?? null, p.data ?? null
  );
}

function insertStatus(data) {
  const stmt = db.prepare(`
    INSERT OR IGNORE INTO device_status
    (event_id, device_id, occurred_at, baby_present, ambient_temperature, quality)
    VALUES (?, ?, ?, ?, ?, ?)
  `);
  const p = data.payload || {};
  return stmt.run(
    data.event_id, data.device_id, data.occurred_at,
    p.baby_present ? 1 : 0, p.ambient_temperature ?? null, data.quality ?? null
  );
}

function insertSensorSamples(deviceId, data) {
  const p = data.payload || {};
  const mappings = [
    ['weight', p.baby_weight, null, 'kg'],
    ['height', p.baby_height_cm ?? p.baby_height ?? p.height_cm, null, 'cm'],
    ['temperature', p.temperature, null, 'C'],
    ['respiratory_rate', p.respiratory_rate, null, 'bpm'],
    ['respiratory_confidence', p.respiratory_confidence, null, 'score'],
    ['ammonia', p.ammonia_ppm, null, 'ppm'],
    ['cry_probability', p.cry_probability, null, 'score'],
    ['diaper_weight_delta', p.diaper_weight_delta_g, null, 'g']
  ];
  const stmt = db.prepare(`
    INSERT INTO sensor_samples
    (device_id, event_id, occurred_at, sensor, raw_value, processed_value, unit, status, metadata_json)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
  `);
  const tx = db.transaction(() => {
    for (const [sensor, value, processed, unit] of mappings) {
      if (value == null) continue;
      stmt.run(
        deviceId, data.event_id, data.occurred_at, sensor, value, processed,
        unit, p.validation_status || 'streamed',
        JSON.stringify({ firmware_version: data.firmware_version, sequence_no: data.sequence_no })
      );
    }
  });
  tx();
}

function insertAudioEvent(data) {
  const p = data.payload || {};
  return db.prepare(`
    INSERT OR IGNORE INTO audio_events
    (event_id, device_id, occurred_at, sample_rate, channels, sample_count, format,
     model_version, training_eligible, data_base64)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
  `).run(
    data.event_id, data.device_id, data.occurred_at,
    p.sample_rate ?? 16000, p.channels ?? 1, p.sample_count ?? null,
    p.format ?? 'pcm_s16le_base64',
    p.model_version ?? 'cry-detector-placeholder',
    p.training_eligible ? 1 : 0,
    p.data ?? null
  );
}

function getAudioEvents(deviceId, limit = 100) {
  return db.prepare(`
    SELECT id,event_id,device_id,occurred_at,sample_rate,channels,sample_count,
           format,model_version,training_eligible,received_at
    FROM audio_events WHERE device_id = ?
    ORDER BY occurred_at DESC LIMIT ?
  `).all(deviceId, limit);
}

function getAudioEventById(id) {
  return db.prepare(`SELECT * FROM audio_events WHERE id = ? OR event_id = ?`).get(id, id);
}

function insertCryInference(result) {
  return db.prepare(`
    INSERT INTO cry_inferences
    (audio_event_id, device_id, occurred_at, model_version, status,
     probable_pattern, probability, inference_ms, features_json, error)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
  `).run(
    result.audio_event_id, result.device_id, result.occurred_at,
    result.model_version ?? 'unknown', result.status ?? 'unknown',
    result.probable_pattern ?? null, result.probability ?? null,
    result.inference_ms ?? null, JSON.stringify(result.features ?? {}),
    result.error ?? null
  );
}

function getCryInferences(deviceId, limit = 100) {
  return db.prepare(`
    SELECT * FROM cry_inferences WHERE device_id = ?
    ORDER BY occurred_at DESC LIMIT ?
  `).all(deviceId, limit);
}

function insertCryReview(review) {
  return db.prepare(`
    INSERT INTO cry_reviews
    (audio_event_id, device_id, review_status, human_decision,
     human_pattern, evidence_note, training_status, reviewer, reviewed_at)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
  `).run(
    review.audio_event_id,
    review.device_id ?? null,
    review.review_status ?? 'NOT_REVIEWED',
    review.human_decision ?? null,
    review.human_pattern ?? null,
    review.evidence_note ?? null,
    review.training_status ?? 'CANDIDATE',
    review.reviewer ?? null,
    review.reviewed_at ?? null
  );
}

function getCryReviews(deviceId, limit = 100) {
  return db.prepare(`
    SELECT
      r.*,
      a.occurred_at AS audio_occurred_at,
      a.sample_rate,
      a.channels,
      a.sample_count,
      a.format,
      (
        SELECT ci.model_version
        FROM cry_inferences ci
        WHERE ci.audio_event_id = r.audio_event_id
        ORDER BY ci.occurred_at DESC, ci.id DESC
        LIMIT 1
      ) AS ai_model_version,
      (
        SELECT ci.probable_pattern
        FROM cry_inferences ci
        WHERE ci.audio_event_id = r.audio_event_id
        ORDER BY ci.occurred_at DESC, ci.id DESC
        LIMIT 1
      ) AS ai_probable_pattern,
      (
        SELECT ci.probability
        FROM cry_inferences ci
        WHERE ci.audio_event_id = r.audio_event_id
        ORDER BY ci.occurred_at DESC, ci.id DESC
        LIMIT 1
      ) AS ai_probability,
      (
        SELECT ci.status
        FROM cry_inferences ci
        WHERE ci.audio_event_id = r.audio_event_id
        ORDER BY ci.occurred_at DESC, ci.id DESC
        LIMIT 1
      ) AS ai_status
    FROM cry_reviews r
    LEFT JOIN audio_events a
      ON a.event_id = r.audio_event_id
    WHERE r.device_id = ?
    ORDER BY r.created_at DESC
    LIMIT ?
  `).all(deviceId, limit);
}

function updateLatestCryPattern(deviceId, probablePattern, modelVersion, probability) {
  return db.prepare(`
    UPDATE telemetry SET probable_pattern = ?, cry_model_version = ?, cry_probability = ?
    WHERE id = (
      SELECT id FROM telemetry WHERE device_id = ? AND cry_detected = 1
      ORDER BY occurred_at DESC LIMIT 1
    )
  `).run(probablePattern, modelVersion, probability ?? null, deviceId);
}

function insertSystemEvent(event) {
  const stmt = db.prepare(`
    INSERT INTO system_events
    (event_id, device_id, occurred_at, event_type, severity, component, message, metadata_json)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?)
  `);
  return stmt.run(
    event.event_id ?? null, event.device_id ?? null, event.occurred_at ?? new Date().toISOString(),
    event.event_type ?? 'SYSTEM_EVENT', event.severity ?? 'info',
    event.component ?? 'backend', event.message ?? '',
    JSON.stringify(event.metadata ?? {})
  );
}

function getLatestTelemetry(limit = 50, deviceId = null) {
  if (deviceId) {
    return db.prepare(`SELECT * FROM telemetry WHERE device_id = ? ORDER BY received_at DESC LIMIT ?`).all(deviceId, limit);
  }
  return db.prepare(`SELECT * FROM telemetry ORDER BY received_at DESC LIMIT ?`).all(limit);
}

function getTelemetryHistory(deviceId, limit = 500) {
  return db.prepare(`
    SELECT * FROM telemetry WHERE device_id = ?
    ORDER BY occurred_at DESC LIMIT ?
  `).all(deviceId, limit);
}

function getLatestImages(limit = 20) {
  return db.prepare(`
    SELECT id, event_id, device_id, occurred_at, received_at FROM images
    ORDER BY received_at DESC LIMIT ?
  `).all(limit);
}

function getImageById(id) {
  return db.prepare(`SELECT * FROM images WHERE id = ? OR event_id = ?`).get(id, id);
}

function getLatestAlerts(limit = 30) {
  return db.prepare(`SELECT * FROM alerts ORDER BY received_at DESC LIMIT ?`).all(limit);
}

function getPresenceHistory(limit = 20) {
  return db.prepare(`SELECT * FROM presence_changes ORDER BY received_at DESC LIMIT ?`).all(limit);
}

function getSensorSamples(deviceId, limit = 500, sensor = null) {
  if (sensor) {
    return db.prepare(`
      SELECT * FROM sensor_samples WHERE device_id = ? AND sensor = ?
      ORDER BY occurred_at DESC LIMIT ?
    `).all(deviceId, sensor, limit);
  }
  return db.prepare(`
    SELECT * FROM sensor_samples WHERE device_id = ?
    ORDER BY occurred_at DESC LIMIT ?
  `).all(deviceId, limit);
}

function getSystemEvents(limit = 200, deviceId = null) {
  if (deviceId) {
    return db.prepare(`
      SELECT * FROM system_events WHERE device_id = ?
      ORDER BY received_at DESC LIMIT ?
    `).all(deviceId, limit);
  }
  return db.prepare(`SELECT * FROM system_events ORDER BY received_at DESC LIMIT ?`).all(limit);
}

function createShareToken(tokenHash, deviceId, expiresAt) {
  const stmt = db.prepare(`
    INSERT INTO share_tokens (token_hash, device_id, expires_at)
    VALUES (?, ?, ?)
  `);
  return stmt.run(tokenHash, deviceId, expiresAt);
}

function getShareToken(tokenHash) {
  return db.prepare(`
    SELECT * FROM share_tokens
    WHERE token_hash = ? AND revoked = 0 AND expires_at > CURRENT_TIMESTAMP
  `).get(tokenHash);
}

function revokeShareToken(tokenHash) {
  return db.prepare(`UPDATE share_tokens SET revoked = 1 WHERE token_hash = ?`).run(tokenHash);
}

function getAdminCounts() {
  return {
    totalAudioEvents: db.prepare('SELECT COUNT(*) AS c FROM audio_events').get().c,
    totalCryInferences: db.prepare('SELECT COUNT(*) AS c FROM cry_inferences').get().c,
    totalSensorSamples: db.prepare('SELECT COUNT(*) AS c FROM sensor_samples').get().c,
    totalSystemEvents: db.prepare('SELECT COUNT(*) AS c FROM system_events').get().c,
    totalAlerts: db.prepare('SELECT COUNT(*) AS c FROM alerts').get().c
  };
}

function getPublicSnapshot(deviceId) {
  return db.prepare(`
    SELECT device_id, occurred_at, baby_weight, baby_height_cm, temperature,
           respiratory_rate, crying, cry_detected, probable_pattern,
           diaper_status, diaper_soiled
    FROM telemetry
    WHERE device_id = ?
    ORDER BY occurred_at DESC LIMIT 1
  `).get(deviceId) || null;
}

module.exports = {
  insertTelemetry,
  insertAlert,
  insertPresenceChange,
  insertImage,
  insertStatus,
  insertSensorSamples,
  insertAudioEvent,
  insertCryInference,
  insertCryReview,
  updateLatestCryPattern,
  insertSystemEvent,
  getLatestTelemetry,
  getTelemetryHistory,
  getLatestImages,
  getImageById,
  getLatestAlerts,
  getPresenceHistory,
  getSensorSamples,
  getSystemEvents,
  getAudioEvents,
  getAudioEventById,
  getCryInferences,
  getCryReviews,
  createShareToken,
  getShareToken,
  revokeShareToken,
  getAdminCounts,
  getPublicSnapshot
};
