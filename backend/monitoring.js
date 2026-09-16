const EventEmitter = require('events');

class MonitoringService extends EventEmitter {
  constructor() {
    super();
    this.deviceHealth = new Map();
    this.anomalies = [];
    this.alertRules = {
      telemetryTimeoutMs: 120000,
      tempMin: 15, tempMax: 45,
      rrMin: 0, rrMax: 120,
      ammoniaMax: 10,
      weightMin: 0, weightMax: 15,
      staleDataMs: 300000,
      consecutiveAlertThreshold: 3
    };
  }

  registerDevice(deviceId) {
    if (!this.deviceHealth.has(deviceId)) {
      this.deviceHealth.set(deviceId, {
        deviceId, firstSeen: new Date(), lastSeen: null,
        telemetryCount: 0, alertCount: 0, imageCount: 0,
        statusCount: 0, consecutiveFailures: 0, status: 'online',
        lastTelemetry: null, lastStatus: null,
        sensorHealth: {
          weight: 'unknown', temperature: 'unknown', respiratory: 'unknown',
          ammonia: 'unknown', audio: 'unknown', camera: 'unknown'
        },
        pipeline: {
          mqtt: 'unknown', ingestion: 'unknown', storage: 'unknown',
          validation: 'unknown', ai: 'ready'
        }
      });
    }
    return this.deviceHealth.get(deviceId);
  }

  recordTelemetry(deviceId, data) {
    const device = this.registerDevice(deviceId);
    device.lastSeen = new Date();
    device.lastTelemetry = data;
    device.telemetryCount++;
    device.consecutiveFailures = 0;
    device.status = 'online';

    const p = data.payload || {};
    device.sensorHealth.weight = p.baby_weight != null ? 'online' : 'no_data';
    device.sensorHealth.temperature = p.temperature != null ? 'online' : 'no_data';
    device.sensorHealth.respiratory = p.respiratory_rate != null ? 'online' : 'no_data';
    device.sensorHealth.ammonia = p.ammonia_ppm != null && p.ammonia_ppm >= 0 ? 'online' : 'warming_or_unavailable';
    device.sensorHealth.audio = p.cry_detected != null || p.crying != null ? 'online' : 'no_data';
    device.pipeline.mqtt = 'online';
    device.pipeline.ingestion = 'online';
    device.pipeline.storage = 'online';
    device.pipeline.validation = p.validation_status || 'processing';
    device.pipeline.ai = p.cry_model_version ? 'model_active' : 'ready_for_model';

    const anomalies = this.checkTelemetryAnomalies(data);
    anomalies.forEach(a => this.recordAnomaly(deviceId, a));
    this.emit('telemetry', { deviceId, data, anomalies });
    return anomalies;
  }

  recordStatus(deviceId, data) {
    const device = this.registerDevice(deviceId);
    device.lastSeen = new Date();
    device.lastStatus = data;
    device.statusCount++;
    device.status = 'online';
    device.sensorHealth.temperature = data.payload?.ambient_temperature != null ? 'online' : device.sensorHealth.temperature;
  }

  recordImage(deviceId) {
    const device = this.registerDevice(deviceId);
    device.imageCount++;
    device.lastSeen = new Date();
    device.sensorHealth.camera = 'online';
  }

  recordAlert(deviceId, alertType, details) {
    const device = this.registerDevice(deviceId);
    device.alertCount++;
    device.consecutiveFailures++;
    if (details) {
      try {
        const parsed = JSON.parse(details);
        if (parsed.sensor) {
          const key = String(parsed.sensor).toLowerCase().replace('pdm_', '').replace('mq137', 'ammonia');
          if (device.sensorHealth[key] !== undefined) device.sensorHealth[key] = 'alert';
        }
      } catch (_) {}
    }
    this.recordAnomaly(deviceId, {
      type: 'SENSOR_ALERT',
      message: `${alertType}: ${details}`,
      severity: alertType.includes('OFFLINE') ? 'critical' : 'warning'
    });
  }

  checkTelemetryAnomalies(data) {
    const anomalies = [];
    const p = data.payload || {};
    if (p.temperature != null && (p.temperature < this.alertRules.tempMin || p.temperature > this.alertRules.tempMax))
      anomalies.push({ type: 'TEMP_OUT_OF_RANGE', message: `Temperature ${p.temperature}C outside safe range`, severity: 'critical', value: p.temperature });
    if (p.respiratory_rate != null && (p.respiratory_rate < this.alertRules.rrMin || p.respiratory_rate > this.alertRules.rrMax))
      anomalies.push({ type: 'RR_OUT_OF_RANGE', message: `Respiratory rate ${p.respiratory_rate} bpm outside configured range`, severity: 'critical', value: p.respiratory_rate });
    if (p.respiratory_confidence != null && p.respiratory_confidence < 0.3 && p.respiratory_rate > 0)
      anomalies.push({ type: 'LOW_RR_CONFIDENCE', message: `Respiratory signal confidence low (${(p.respiratory_confidence * 100).toFixed(0)}%)`, severity: 'warning', value: p.respiratory_confidence });
    if (p.ammonia_ppm != null && p.ammonia_ppm > this.alertRules.ammoniaMax)
      anomalies.push({ type: 'HIGH_AMMONIA', message: `Ammonia level ${p.ammonia_ppm} ppm exceeds configured threshold`, severity: 'warning', value: p.ammonia_ppm });
    if (p.baby_weight != null && (p.baby_weight < this.alertRules.weightMin || p.baby_weight > this.alertRules.weightMax))
      anomalies.push({ type: 'WEIGHT_OUT_OF_RANGE', message: `Weight ${p.baby_weight} kg outside configured engineering range`, severity: 'warning', value: p.baby_weight });
    return anomalies;
  }

  recordAnomaly(deviceId, anomaly) {
    const record = {
      id: `anomaly-${Date.now()}-${Math.random().toString(36).slice(2, 11)}`,
      deviceId, ...anomaly, timestamp: new Date().toISOString()
    };
    this.anomalies.unshift(record);
    if (this.anomalies.length > 1000) this.anomalies.pop();
    this.emit('anomaly', record);
    console.log(`[ANOMALY] ${deviceId}: ${anomaly.message}`);
  }

  checkStaleDevices() {
    const now = Date.now();
    const staleDevices = [];
    for (const [deviceId, device] of this.deviceHealth) {
      if (device.lastSeen) {
        const elapsed = now - device.lastSeen.getTime();
        if (elapsed > this.alertRules.staleDataMs && device.status === 'online') {
          device.status = 'stale';
          this.recordAnomaly(deviceId, {
            type: 'DEVICE_STALE',
            message: `No data received for ${(elapsed / 1000 / 60).toFixed(1)} minutes`,
            severity: 'critical'
          });
          staleDevices.push(device);
        }
      }
    }
    return staleDevices;
  }

  getDeviceStatus(deviceId) {
    if (deviceId) return this.deviceHealth.get(deviceId) || null;
    return Array.from(this.deviceHealth.values());
  }

  getRecentAnomalies(limit = 50, severity = null) {
    let results = this.anomalies;
    if (severity) results = results.filter(a => a.severity === severity);
    return results.slice(0, limit);
  }

  getHealthSummary() {
    const devices = Array.from(this.deviceHealth.values());
    return {
      totalDevices: devices.length,
      online: devices.filter(d => d.status === 'online').length,
      stale: devices.filter(d => d.status === 'stale').length,
      totalTelemetry: devices.reduce((sum, d) => sum + d.telemetryCount, 0),
      totalAlerts: devices.reduce((sum, d) => sum + d.alertCount, 0),
      totalImages: devices.reduce((sum, d) => sum + d.imageCount, 0),
      totalAnomalies: this.anomalies.length,
      criticalAnomalies: this.anomalies.filter(a => a.severity === 'critical').length,
      uptime: process.uptime()
    };
  }
}

module.exports = new MonitoringService();
