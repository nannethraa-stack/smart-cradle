#!/bin/bash
# Smart Cradle - EC2 Deployment Script
# Run this on a fresh Ubuntu 22.04 EC2 instance

set -e

echo "=== Smart Cradle Backend Deployment ==="

# Update system
echo "[1/8] Updating system..."
sudo apt update && sudo apt upgrade -y

# Install Node.js
echo "[2/8] Installing Node.js..."
curl -fsSL https://deb.nodesource.com/setup_18.x | sudo -E bash -
sudo apt install -y nodejs

# Install Mosquitto
echo "[3/8] Installing Mosquitto MQTT Broker..."
sudo apt install -y mosquitto mosquitto-clients

# Configure Mosquitto
echo "[4/8] Configuring Mosquitto..."
sudo tee /etc/mosquitto/conf.d/default.conf > /dev/null <<EOF
listener 1883 0.0.0.0
allow_anonymous true
EOF

sudo systemctl enable mosquitto
sudo systemctl restart mosquitto

# Install PM2
echo "[5/8] Installing PM2..."
sudo npm install -g pm2

# Create app directory
echo "[6/8] Setting up application..."
sudo mkdir -p /opt/smart-cradle
sudo chown $USER:$USER /opt/smart-cradle
cd /opt/smart-cradle

# Copy application files (user must have files here already)
if [ ! -f "package.json" ]; then
    echo "ERROR: No package.json found. Copy your backend files to /opt/smart-cradle first."
    echo "From your local machine, run:"
    echo "  scp -i your-key.pem -r /path/to/smart_cradle_backend/* ubuntu@$(curl -s ifconfig.me):/opt/smart-cradle/"
    exit 1
fi

npm install

# Create PM2 ecosystem file
cat > ecosystem.config.js <<EOF
module.exports = {
  apps: [{
    name: 'smart-cradle',
    script: 'server.js',
    env: {
      NODE_ENV: 'production',
      PORT: 3001,
      MQTT_BROKER: 'mqtt://localhost:1883'
    },
    max_memory_restart: '512M',
    restart_delay: 5000
  }]
};
EOF

# Start with PM2
echo "[7/8] Starting application..."
pm2 start ecosystem.config.js
pm2 save
pm2 startup systemd -u $USER --hp /home/$USER

# Verify
echo "[8/8] Verifying..."
sleep 3
echo "--- PM2 Status ---"
pm2 status
echo ""
echo "--- Mosquitto Status ---"
sudo systemctl is-active mosquitto
echo ""
echo "--- Testing Endpoints ---"
curl -s http://localhost:3001/api/telemetry | head -c 200
echo ""

PUBLIC_IP=$(curl -s ifconfig.me)
echo ""
echo "=== Deployment Complete ==="
echo "Dashboard URL: http://$PUBLIC_IP:3001"
echo "MQTT Broker: $PUBLIC_IP:1883"
echo ""
echo "Next steps:"
echo "1. Update device firmware MQTT_BROKER_IP to: $PUBLIC_IP"
echo "2. Open browser to: http://$PUBLIC_IP:3001"
echo ""
echo "Useful commands:"
echo "  pm2 logs          # View logs"
echo "  pm2 restart all   # Restart backend"
echo "  pm2 monit         # Monitor resources"
