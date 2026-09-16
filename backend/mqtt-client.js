const mqtt = require('mqtt');
const db = require('./db');
const monitoring = require('./monitoring');
const cryModel = require('./cry-model');

const MQTT_BROKER = process.env.MQTT_BROKER || 'mqtt://broker.hivemq.com:1883';
const TOPIC_PREFIX = 'cradle/';

let broadcastFn = null;
const diaperState = new Map();

function setBroadcast(fn) { broadcastFn = fn; }
function broadcast(data) { if (broadcastFn) broadcastFn(data); }

function inferDiaperStatus(deviceId, payload) {
  const ammonia = Number.isFinite(payload.ammonia_ppm) && payload.ammonia_ppm >= 0
    ? payload.ammonia_ppm : null;
  const weightDelta = Number.isFinite(payload.diaper_weight_delta_g)
    ? payload.diaper_weight_delta_g : null;

  // The weight signal is total cradle weight, so only slow change relative to a
  // presence-stable baseline is considered. This is a fusion heuristic, not a
  // medical diagnosis. A future calibrated diaper-specific model can replace it.
  const state = diaperState.get(deviceId) || {
    baselineKg: null,
    lastWeightKg: null,
    status: 'Dry'
  };

  const weight = Number.isFinite(payload.baby_weight) ? payload.baby_weight : null;
  if (weight != null) {
    if (state.baselineKg == null) state.baselineKg = weight;
    const deltaG = Math.max(0, (weight - state.baselineKg) * 1000);
    state.lastWeightKg = weight;
    if (weightDelta == null) payload.diaper_weight_delta_g = Number(deltaG.toFixed(1));
  }

  const effectiveDelta = Number.isFinite(payload.diaper_weight_delta_g)
    ? payload.diaper_weight_delta_g : null;
  const ammoniaPositive = ammonia != null && ammonia >= 5;
  const weightPositive = effectiveDelta != null && effectiveDelta >= 60;
  const fullWeight = effectiveDelta != null && effectiveDelta >= 180;

  let status = 'Dry';
  let reason = 'No sustained urine-related gas or accumulation signal detected.';
  if (ammoniaPositive || fullWeight) {
    status = fullWeight || ammonia >= 10 ? 'Likely Full' : 'Urine Detected';
    reason = [
      ammoniaPositive ? `ammonia signal ${ammonia.toFixed(1)} ppm` : null,
      effectiveDelta != null ? `weight change ${effectiveDelta.toFixed(0)} g` : null
    ].filter(Boolean).join(' + ');
  } else if (weightPositive) {
    status = 'Urine Detected';
    reason = `sustained weight change ${effectiveDelta.toFixed(0)} g; ammonia signal not required`;
  }

  state.status = status;
  diaperState.set(deviceId, state);
  payload.diaper_status = status;
  payload.diaper_fusion_reason = reason;
  payload.diaper_soiled = status !== 'Dry';
  return payload;
}

function start() {
  console.log(`Connecting to MQTT broker: ${MQTT_BROKER}`);
  const client = mqtt.connect(MQTT_BROKER);

  client.on('connect', () => {
    console.log(`Connected to MQTT broker at ${MQTT_BROKER}`);
    client.subscribe(`${TOPIC_PREFIX}#`, (err) => {
      if (err) console.error('Subscribe error:', err);
      else console.log(`Subscribed to ${TOPIC_PREFIX}#`);
    });
  });

  client.on('message', (topic, message) => {
    try {
      const rawMessage = message.toString();
      let data;
      try {
        data = JSON.parse(rawMessage);
      } catch (parseError) {
        console.warn('[MQTT] Ignoring non-JSON message on ' + topic);
        return;
      }
      const parts = topic.split('/');
      const deviceId = parts[1];
      const channel = parts[2];
      if (!deviceId || !channel) return;

      if (channel === 'telemetry') {
        const payload = data.payload || {};
        inferDiaperStatus(deviceId, payload);

        // Firmware currently provides cry detection; the backend schema is ready
        // for a trained cry-pattern model. Do not invent a clinical diagnosis.
        if (payload.crying && !payload.cry_detected) payload.cry_detected = true;
        if (payload.cry_detected && !payload.probable_pattern) {
          payload.probable_pattern = 'Analysing…';
        }
        if (payload.cry_detected && !payload.audio_event_id) {
          payload.audio_event_id = `audio-${data.event_id}`;
        }

        db.insertTelemetry(data);
        db.insertSensorSamples(deviceId, data);
        db.insertSystemEvent({
          event_id: data.event_id,
          device_id: deviceId,
          occurred_at: data.occurred_at,
          event_type: 'TELEMETRY_INGESTED',
          component: 'data_pipeline',
          severity: 'info',
          message: 'Telemetry received and persisted.',
          metadata: {
            sequence_no: data.sequence_no,
            firmware_version: data.firmware_version,
            stages: ['raw_received', 'processed', 'validated', 'stored']
          }
        });
        monitoring.recordTelemetry(deviceId, data);
        broadcast({ type: 'telemetry', deviceId, data });
        return;
      }

      if (channel === 'alerts') {
        if (data.event_type === 'PRESENCE_CHANGE') {
          db.insertPresenceChange(data);
        } else {
          db.insertAlert(data);
          monitoring.recordAlert(deviceId, data.payload?.alert_type || 'unknown', JSON.stringify(data.payload));
        }
        db.insertSystemEvent({
          event_id: data.event_id,
          device_id: deviceId,
          occurred_at: data.occurred_at,
          event_type: data.event_type || 'ALERT',
          component: data.payload?.sensor || 'sensor',
          severity: data.payload?.alert_type?.includes('OFFLINE') ? 'critical' : 'warning',
          message: data.payload?.alert_type || data.event_type || 'Alert received',
          metadata: data.payload || {}
        });
        broadcast({ type: 'alert', deviceId, data });
        return;
      }

      if (channel === 'audio') {
        db.insertAudioEvent(data);
        db.insertSystemEvent({
          event_id: data.event_id, device_id: deviceId, occurred_at: data.occurred_at,
          event_type: 'AUDIO_EVENT_STORED', component: 'audio_pipeline', severity: 'info',
          message: 'Cry audio segment stored as a training candidate.',
          metadata: { model_version: data.payload?.model_version || 'cry-detector-placeholder',
                      training_eligible: !!data.payload?.training_eligible }
        });
        broadcast({ type: 'audio', deviceId, data });

        // Optional external/local ML service. The adapter is deliberately
        // pluggable so a Kaggle-trained model can be deployed later without
        // changing the dashboard contract.
        const audioEvent = db.getAudioEventById(data.event_id);
        if (audioEvent) {
          cryModel.analyze(audioEvent).then(result => {
            db.insertCryInference(result);
            db.updateLatestCryPattern(deviceId, result.probable_pattern, result.model_version, result.probability);
            broadcast({ type: 'cry_inference', deviceId, data: result });
          }).catch(error => console.error('Cry model error:', error.message));
        }
        return;
      }

      if (channel === 'images') {
        db.insertImage(data);
        monitoring.recordImage(deviceId);
        broadcast({ type: 'image', deviceId, data });
        return;
      }

      if (channel === 'status') {
        db.insertStatus(data);
        monitoring.recordStatus(deviceId, data);
        broadcast({ type: 'status', deviceId, data });
        return;
      }

      return;
    } catch (err) {
      console.error('Failed to process message:', err.message);
    }
  });

  client.on('error', (err) => console.error('MQTT error:', err.message));
  client.on('close', () => console.log('MQTT connection closed'));
  return client;
}

module.exports = { start, setBroadcast };
