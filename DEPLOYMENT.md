# Smart Cradle - Cloud Deployment Guide

## Overview

```
Portenta H7 (Device)
    ↓ MQTT
AWS EC2 (Mosquitto + Node.js Backend)
    ↓ HTTP/WebSocket
Hospital Browser (Dashboard URL)
```

---

## Step 1: AWS Account Setup

1. Go to https://aws.amazon.com/ and create an account
2. Complete verification (credit card required, but free tier won't charge)
3. Sign in to AWS Console

---

## Step 2: Create EC2 Instance

1. Go to **EC2 Dashboard** → **Launch Instance**
2. Configure:
   - **Name**: `smart-cradle-backend`
   - **AMI**: Ubuntu Server 22.04 LTS (free tier eligible)
   - **Instance type**: t3.micro (free tier)
   - **Key pair**: Create new → `smart-cradle-key` → Download `.pem` file
   - **Network settings**: Create security group

3. **Security Group Rules** (critical):

| Type | Protocol | Port Range | Source |
|------|----------|------------|--------|
| SSH | TCP | 22 | Your IP only |
| Custom TCP | TCP | 1883 | 0.0.0.0/0 (MQTT) |
| Custom TCP | TCP | 3001 | 0.0.0.0/0 (Dashboard) |

4. Click **Launch Instance**

5. Note the **Public IPv4 address** (e.g., `54.123.45.67`)

---

## Step 3: Connect to EC2

### Windows (PowerShell):
```powershell
# Move to your key file location
cd Downloads

# Fix permissions (PowerShell)
icacls smart-cradle-key.pem /reset
icacls smart-cradle-key.pem /grant:r "$($env:USERNAME):(R)"
icacls smart-cradle-key.pem /inheritance:r

# Connect
ssh -i "smart-cradle-key.pem" ubuntu@54.123.45.67
```

### Mac/Linux:
```bash
chmod 400 smart-cradle-key.pem
ssh -i "smart-cradle-key.pem" ubuntu@54.123.45.67
```

---

## Step 4: Install Dependencies on EC2

```bash
# Update system
sudo apt update && sudo apt upgrade -y

# Install Node.js 18.x
curl -fsSL https://deb.nodesource.com/setup_18.x | sudo -E bash -
sudo apt install -y nodejs

# Install Mosquitto MQTT Broker
sudo apt install -y mosquitto mosquitto-clients

# Install PM2 (process manager)
sudo npm install -g pm2

# Verify installations
node --version    # v18.x
npm --version     # 9.x
mosquitto -h      # version info
```

---

## Step 5: Configure Mosquitto MQTT Broker

```bash
# Create config
sudo tee /etc/mosquitto/conf.d/default.conf > /dev/null <<EOF
listener 1883 0.0.0.0
allow_anonymous true
EOF

# Start and enable on boot
sudo systemctl enable mosquitto
sudo systemctl restart mosquitto

# Test it works
mosquitto_sub -t test -v &     # subscriber in background
mosquitto_pub -t test -m hello  # send test message
kill %1                         # stop subscriber
```

---

## Step 6: Deploy Backend Application

```bash
# Create app directory
sudo mkdir -p /opt/smart-cradle
sudo chown ubuntu:ubuntu /opt/smart-cradle
cd /opt/smart-cradle

# Copy your backend files (from local machine)
# Option A: If using git
git clone https://github.com/your-repo/smart-cradle.git .

# Option B: Manually copy via SCP
# (Run this from your LOCAL machine, not EC2)
# scp -i "smart-cradle-key.pem" -r C:\Users\Yoga\Downloads\smart_cradle_backend\* ubuntu@54.123.45.67:/opt/smart-cradle/

# Install dependencies
npm install

# Create ecosystem file for PM2
cat > ecosystem.config.js <<EOF
module.exports = {
  apps: [{
    name: 'smart-cradle',
    script: 'server.js',
    env: {
      NODE_ENV: 'production',
      PORT: 3001,
      MQTT_BROKER: 'mqtt://localhost:1883'
    }
  }]
};
EOF

# Start with PM2
pm2 start ecosystem.config.js
pm2 save
pm2 startup
# Run the command PM2 outputs (usually: sudo env PATH=$PATH:/usr/bin pm2 startup ubuntu -u ubuntu --hp /home/ubuntu)

# Check status
pm2 status
pm2 logs
```

---

## Step 7: Update Device Firmware

In `smart_cradle_firmware_v2_3_1.ino`:

```cpp
const char MQTT_BROKER_IP[] = "54.123.45.67";  // Your EC2 public IP
```

Reupload to Portenta H7.

---

## Step 8: Verify Everything Works

1. **Check backend is running:**
   ```bash
   curl http://localhost:3001/api/telemetry
   ```

2. **Open browser:**
   ```
   http://54.123.45.67:3001
   ```

3. **Device should start publishing data** — dashboard updates in real-time.

---

## Step 9 (Optional): Custom Domain with HTTPS

For a professional hospital URL like `cradle.hospital.com`:

### Option A: AWS Route 53 + Application Load Balancer

1. **Route 53**: Register domain or create hosted zone
2. **Certificate Manager**: Request SSL certificate
3. **Application Load Balancer**:
   - Listen on 443 (HTTPS)
   - Target group → EC2 instance port 3001
   - Attach SSL certificate
4. **Route 53**: Create A record → ALB DNS name

### Option B: Simple Reverse Proxy with Nginx (Free)

```bash
# Install Nginx
sudo apt install -y nginx

# Install Certbot for free SSL
sudo apt install -y certbot python3-certbot-nginx

# Configure Nginx
sudo tee /etc/nginx/sites-available/smart-cradle > /dev/null <<EOF
server {
    listen 80;
    server_name cradle.hospital.com;

    location / {
        proxy_pass http://localhost:3001;
        proxy_http_version 1.1;
        proxy_set_header Upgrade \$http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host \$host;
        proxy_read_timeout 86400;
    }
}
EOF

sudo ln -s /etc/nginx/sites-available/smart-cradle /etc/nginx/sites-enabled/
sudo nginx -t
sudo systemctl restart nginx

# Get free SSL certificate
sudo certbot --nginx -d cradle.hospital.com
```

Now accessible at: `https://cradle.hospital.com`

---

## Step 10: Monitoring & Maintenance

```bash
# View logs
pm2 logs smart-cradle

# Restart after code changes
pm2 restart smart-cradle

# Monitor resources
pm2 monit

# Check MQTT connections
sudo tail -f /var/log/mosquitto/mosquitto.log
```

---

## Cost Estimate (Pilot)

| Service | Free Tier | Monthly Cost |
|---------|-----------|--------------|
| EC2 t3.micro | 750 hrs/month | $0 (first 12 months) |
| EBS Storage | 30 GB | $0 |
| Data Transfer | 1 GB/month | ~$0.09 |
| **Total** | | **~$0** |

After 12 months: ~$8-15/month for t3.micro

---

## Troubleshooting

| Issue | Solution |
|-------|----------|
| Can't connect to EC2 | Check security group allows SSH from your IP |
| MQTT timeout | Verify port 1883 is open in security group |
| Dashboard not loading | Check `pm2 status` and `pm2 logs` |
| No data appearing | Verify device firmware has correct EC2 IP |
| SSL certificate fails | Ensure port 80 is open for Certbot validation |

---

## New Data Intelligence / Cry ML Configuration

The backend now supports an optional cry-pattern model service. Set:

```bash
export CRY_MODEL_URL="https://your-model-service.example.com/infer"
```

The service receives the stored audio event and should return JSON containing `probable_pattern`, optional `probability`, `model_version`, and optional `features`.

If `CRY_MODEL_URL` is not configured, the system safely records the cry audio candidate and reports `Analysing...` rather than inventing a pattern or diagnosis.

### Production security requirements

- Run the dashboard and QR sharing over HTTPS.
- Put the MQTT broker behind authentication/TLS rather than anonymous public port 1883.
- Add authentication/authorization in front of `/admin/*` and `/api/share` before clinical or multi-site deployment.
- Treat stored cry audio as sensitive data and apply retention/access controls before using it for model training.
