# Smart Cradle - Tech Stack

## Firmware (Portenta H7)

| Layer | Technology | Purpose |
|-------|-----------|---------|
| **Platform** | Arduino Mbed OS Portenta Boards v4.0+ | Core runtime |
| **Language** | C++17 | Firmware code |
| **Connectivity** | WiFi (Portenta H7 built-in) | Network communication |
| **Messaging** | MQTT (ArduinoMqttClient library) | Telemetry + image publishing |
| **Thermal Sensor** | MLX90640 (SparkFun library) | Temperature + respiratory rate |
| **Gas Sensor** | MQ-137 (analog ADC) | Ammonia / diaper soiling |
| **Weight Sensor** | HX711 + 5kg load cell | Presence + weight |
| **Audio** | PDM digital microphone | Crying detection |
| **Camera** | OV7675 (Arduino_OV767X) | QQVGA grayscale capture |
| **Time** | NTP (UDP client) | ISO8601 timestamps |
| **Storage** | None (MQTT streaming) | Images sent to backend |

---

## Backend (Node.js)

| Layer | Technology | Purpose |
|-------|-----------|---------|
| **Runtime** | Node.js 18+ | Server runtime |
| **Framework** | Express 4.18 | REST API + static hosting |
| **MQTT Client** | mqtt 5.3.0 | Subscribe to device topics |
| **Database** | SQLite (better-sqlite3) | Persistent storage |
| **Real-time** | WebSocket (ws 8.16) | Push updates to dashboard |
| **Process Manager** | PM2 | Auto-restart + monitoring |
| **Language** | JavaScript | Backend code |

---

## Dashboard (Browser)

| Layer | Technology | Purpose |
|-------|-----------|---------|
| **UI** | Vanilla HTML/CSS/JS | No framework dependency |
| **Charts** | Chart.js 4.4.1 | Temperature + RR history |
| **Real-time** | WebSocket client | Live metric updates |
| **Camera Feed** | Base64 image rendering | View captured frames |
| **Responsive** | CSS Grid + Flexbox | Mobile-friendly layout |

---

## Infrastructure (AWS)

| Service | Purpose | Free Tier |
|---------|---------|-----------|
| **EC2 t3.micro** | Host Mosquitto + Node.js backend | 750 hrs/month |
| **Mosquitto** | MQTT broker | Open source |
| **Security Groups** | Port 22, 1883, 3001 | Free |
| **Optional: Route 53** | Custom domain | 12 months free |
| **Optional: ACM** | SSL certificates | Free |

---

## Data Flow

```
┌─────────────────┐     MQTT      ┌──────────────────┐     HTTP/WS     ┌──────────────────┐
│  Portenta H7    │──────────────▶│  Mosquitto       │───────────────▶│  Node.js Backend │
│  (C++ firmware) │               │  (EC2:1883)      │               │  (Express+SQLite)│
│                 │               │                  │               │                  │
│  - MLX90640     │               │                  │               │  - REST API      │
│  - MQ-137       │               │                  │               │  - WebSocket     │
│  - HX711        │               │                  │               │  - Admin API     │
│  - PDM Audio    │               │                  │               │  - Monitoring    │
│  - OV7675       │               │                  │               │                  │
└─────────────────┘               └──────────────────┘               └────────┬─────────┘
                                                                          │
                                                                          │ Browser
                                                                          ▼
                                                              ┌──────────────────┐
                                                              │  Dashboard UI     │
                                                              │  (Chart.js)       │
                                                              └──────────────────┘
```

---

## Message Types

| Topic | Format | Description |
|-------|--------|-------------|
| `cradle/{uuid}/telemetry` | JSON | Sensor readings every 5s |
| `cradle/{uuid}/alerts` | JSON | Sensor failures + presence changes |
| `cradle/{uuid}/images` | JSON + base64 | Camera frames every 60s |
| `cradle/{uuid}/status` | JSON | Heartbeat when cradle empty |

---

## Database Schema

| Table | Records |
|-------|---------|
| `telemetry` | Temperature, RR, weight, crying, ammonia, quality |
| `images` | Base64 frames + metadata |
| `alerts` | Sensor failures and recoveries |
| `presence_changes` | Baby placed/removed events |
| `device_status` | Heartbeat records |

---

## Key Design Decisions

1. **No SD card** — Images stream via MQTT to cloud
2. **Fixed char buffers** — Avoid heap fragmentation on embedded device
3. **Non-blocking sensors** — No `delay()` in hot path except MQ-137 sampling
4. **Per-device topics** — `cradle/{device_uuid}/#` enables multi-cradle scaling
5. **SQLite** — Zero-config DB, sufficient for pilot (up to ~50 cradles)
6. **PM2** — Keeps backend alive across reboots
7. **WebSocket** — Real-time dashboard without polling

## Data Intelligence / Admin Layer (v3 architecture)

| Capability | Implementation |
|---|---|
| Raw sensor stream retention | SQLite `sensor_samples` time-series table |
| Processed telemetry | SQLite `telemetry` table with validation/model metadata |
| Cry audio candidates | MQTT `cradle/{uuid}/audio` → `audio_events` |
| Cry ML adapter | `backend/cry-model.js`; optional `CRY_MODEL_URL` model service |
| Cry inference history | `cry_inferences` table |
| Diaper fusion | Ammonia signal + sustained weight-change signal + baseline heuristic |
| Mobile QR sharing | Expiring hashed token + `/mobile.html` secure share view |
| Admin observatory | Raw telemetry, sensor samples, health, pipeline, events, anomalies, audio events |
| Height/length | Dashboard/data-model field `baby_height_cm`; populated when a validated vision/ML estimator is integrated |

### Data architecture

```
RAW SENSOR DATA
     ↓
SIGNAL PROCESSING / BASELINE
     ↓
VALIDATION + ARTIFACT HANDLING
     ↓
AI / ML + SENSOR FUSION
     ↓
BABY MONITORING DATA MODEL
     ├── Caregiver Dashboard
     ├── Historical Trends
     ├── QR Mobile View
     └── Admin / Investor Data Observatory
```

The caregiver interface intentionally does not expose internal quality/confidence scores. Engineering diagnostics, probabilities, model versions and raw streams remain in the Admin Observatory.
