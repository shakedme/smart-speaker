# GDB Restaurant Monitor 🍔

This script monitors the GDB Burger restaurant on Wolt and alerts you when it shows the "not accepting orders" message.

## Setup

1. **Install Node.js** (if not already installed)
   - Download from https://nodejs.org/

2. **Install dependencies**
   ```bash
   npm install
   ```

## Usage

Run the monitoring script:
```bash
npm start
```

Or directly:
```bash
node monitor_gdb.js
```

## What it does

- 🔍 Checks the GDB restaurant page every 10 seconds
- 🚨 Alerts with console messages and beep sound when "not accepting orders" message appears
- 📱 Keeps the browser window open when alert is triggered
- ✅ Shows status updates in real-time

## Controls

- **Ctrl+C** to stop monitoring
- Browser window will open (set `headless: true` in the script to hide it)

## Features

- Real-time monitoring with visual feedback
- Automatic retry on errors
- Graceful shutdown handling
- Console beep alert when message found
- Timestamp logging for each check

## Customization

Edit `monitor_gdb.js` to:
- Change check interval (currently 10 seconds)
- Run in headless mode (no browser window)
- Modify alert behavior
- Add additional notification methods 