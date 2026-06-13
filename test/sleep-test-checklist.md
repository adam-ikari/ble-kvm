# Low-Power Sleep Test Checklist

## Hardware: M5StickS3 (battery)

### Screen Off Test
1. Power on StickS3 with battery
2. Wait for screen_off_timeout_sec (default 30s) with no interaction
3. [ ] Screen backlight turns off after timeout
4. [ ] Press button — screen lights up immediately
5. [ ] Screen-off timer resets on button press

### KVM Sleep Test
1. Disconnect all BLE and USB connections
2. Wait for sleep_timeout_sec (default 60s)
3. [ ] Device enters sleep (LED breathing)
4. [ ] Connect BLE from PC — device wakes up
5. [ ] LED returns to solid, screen lights up

### PPT Sleep Test
1. Switch to PPT mode (side button)
2. Leave device still (no IMU motion)
3. Wait for sleep_timeout_sec
4. [ ] Device enters sleep
5. [ ] Move device — IMU motion detected, sleep timer resets

### Wake Test
1. In sleep state
2. [ ] BLE connection from PC wakes device
3. [ ] Physical button press wakes device
4. [ ] USB VBUS (plug into PC) wakes device

### Web API Test
1. [ ] GET /api/status returns screen_off_timeout_sec, sleep_timeout_sec, sleep_state
2. [ ] GET /api/settings returns screen_off_timeout_sec, sleep_timeout_sec
3. [ ] PATCH /api/settings with {"screen_off_timeout_sec": 15} updates config
4. [ ] PATCH /api/settings with {"sleep_timeout_sec": 30} updates config

## Hardware: M5StampS3 (single-button, no battery)

### Button Test
1. [ ] Short press cycles owner (1 → 2 → 3 → 1)
2. [ ] Long press (≥ 5s) triggers factory reset
3. [ ] Double-click (< 500ms between clicks) grants web auth

### No Sleep
1. [ ] GET /api/status does NOT include sleep_state field
2. [ ] No sleep behavior, no crashes
3. [ ] Normal operation unaffected

## Hardware: Generic ESP32-S3 (no battery)

### No Sleep
1. [ ] No sleep behavior, no crashes
2. [ ] Normal operation unaffected
