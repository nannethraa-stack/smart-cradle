# Smart Cradle Backend

Node.js backend that receives MQTT data from the Smart Cradle firmware and displays it on a real-time dashboard.

## Setup

1. Install dependencies:
   ```
   npm install
   ```

2. Start the server:
   ```
   npm start
   ```

3. Open browser: http://localhost:3000

## MQTT Broker Setup

The backend needs an MQTT broker. Options:

### Option 1: Local Mosquitto Broker (recommended for production)
Download and install from https://mosquitto.org/download/
Then run:
```
mosquitto -v
```
Update firmware `MQTT_BROKER_IP` to your PC's IP.

### Option 2: Public Test Broker (quick testing)
Default is set to `mqtt://broker.hivemq.com:1883`.
Update firmware to publish to the same broker:
```cpp
const char MQTT_BROKER_IP[] = "broker.hivemq.com";
```

## MQTT Topics Subscribed

- `cradle/{device_uuid}/telemetry` - Sensor readings
- `cradle/{device_uuid}/alerts` - Sensor alerts and presence changes
- `cradle/{device_uuid}/images` - Base64-encoded camera frames
- `cradle/{device_uuid}/status` - Heartbeat when cradle is empty

## Dashboard Features

- Real-time temperature and respiratory rate charts
- Live camera feed with click-to-enlarge modal
- Alert/event log
- Baby presence indicator
- Data quality metric

## Database

SQLite database `cradle.db` is created automatically with tables:
- `telemetry` - All sensor readings
- `images` - Camera frames with base64 data
- `alerts` - Sensor failures/recoveries
- `presence_changes` - Baby placed/removed events
- `device_status` - Heartbeat records


## Hardware test mode / data integrity

This build contains no simulated telemetry generator, mock-data endpoint, seed dataset, or synthetic sensor stream. Executive Demo Mode is presentation-only and reads the same persisted live MQTT data as Engineering Mode. When testing with the physical firmware, data enters through the configured MQTT broker and is persisted by the normal ingestion pipeline.
