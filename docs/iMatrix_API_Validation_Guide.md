# iMatrix API Validation Guide

Reference for verifying FC-1 gateway and CAN controller data uploads via the iMatrix cloud API.

## API Details

| Field | Value |
|-------|-------|
| Base URL | `https://api-dev.imatrixsys.com/api/v1` |
| Auth | Email/password login, returns JWT token |
| Token header | `x-auth-token` |

### Credentials (Aptera account)

| Field | Value |
|-------|-------|
| Username | `imatrix.aptera@gmail.com` |
| Password | `SierraSnow100!` |

### Device Serial Numbers

| Device | SN | Name | Product ID |
|--------|----|------|------------|
| FC-1 Gateway #1 | 799874683 | FC Aptera #1 | 374664309 |
| FC-1 Gateway #2 | 513973109 | FC Aptera #2 | 374664309 |
| FC-1 Gateway #3 | 174664659 | FC Aptera #3 | 374664309 |
| FC-1 Gateway #5 | (check API) | FC Aptera #5 | 374664309 |
| CAN Controller (via #1) | 911934657 | 358088755905422 | 2201718576 |

**Note:** The CAN controller appears in the things list with its IMEI as the name. Its `sn` field is the CAN controller serial number used in `selfreport/` URIs.

---

## Authentication

```bash
# Login and get token
TOKEN=$(curl -s -X POST \
  "https://api-dev.imatrixsys.com/api/v1/login" \
  -H "Content-Type: application/json" \
  -d '{"email":"imatrix.aptera@gmail.com","password":"SierraSnow100!"}' \
  | python3 -c "import sys,json; print(json.load(sys.stdin)['token'])")

echo "Token: ${TOKEN:0:20}..."
```

Or in Python:

```python
import requests

API = "https://api-dev.imatrixsys.com/api/v1"
r = requests.post(f"{API}/login", json={
    "email": "imatrix.aptera@gmail.com",
    "password": "SierraSnow100!"
})
token = r.json()["token"]
headers = {"x-auth-token": token}
```

---

## API Endpoints

### List All Devices

```
GET /things?limit=50
```

Returns `{"list": [...], "total": N}`. Each thing has:
- `sn` — serial number (uint32, used in all other endpoints)
- `name` — device name or IMEI
- `mac` — MAC address
- `productId` — product identifier
- `organizationId` — org that owns the device

```bash
curl -s -H "x-auth-token: $TOKEN" \
  "$API/things?limit=50" | python3 -m json.tool
```

### Device Info

```
GET /things/sn/{serial_number}
```

Returns device metadata (name, product, organization, firmware version).

### List Sensors for a Device

```
GET /things/{sn}/sensors
```

Returns all sensors configured for the device. Each sensor has an `id` (sensor ID used in history queries) and `name`.

### Sensor History (Single Sensor)

```
GET /things/{sn}/sensor/{sensor_id}/history/{from_ms}/{to_ms}?group_by_time=NONE
```

- `sn` — device serial number
- `sensor_id` — numeric sensor ID
- `from_ms` / `to_ms` — time range in milliseconds since epoch
- `group_by_time=NONE` — returns raw ungrouped data points

Response format:
```json
{
  "799874683": {
    "2": {
      "min": 38.9017,
      "max": 38.9019,
      "avg": 38.9018,
      "data": [
        {"time": 1773759157000, "value": 38.9018},
        {"time": 1773759159000, "value": 38.9018}
      ]
    }
  }
}
```

### Dashboard History (All Sensors)

```
GET /dashboard/{sn}/history/{from_ms}/{to_ms}?group_by_time=NONE
```

Returns data for ALL sensors of a device in one call. Same time parameters as single sensor endpoint.

### Fleet Vehicle Info

```
GET /fleet/vehicles
```

Returns `{"success": true, "vehicles": [...], "totalCount": N}` with VIN, make, model, year for registered vehicles.

---

## Common Validation Tasks

### Check if device is uploading (last 10 minutes)

```bash
NOW_MS=$(python3 -c "import time; print(int(time.time()*1000))")
FROM_MS=$((NOW_MS - 600000))

# Gateway
curl -s -H "x-auth-token: $TOKEN" \
  "$API/things/799874683/sensor/2/history/$FROM_MS/$NOW_MS?group_by_time=NONE" \
  | python3 -c "
import sys, json
d = json.load(sys.stdin)
pts = d.get('799874683', {}).get('2', {}).get('data', [])
if pts:
    age = ($NOW_MS - pts[-1]['time']) / 1000
    print(f'Latest: {pts[-1][\"value\"]}, {age:.0f}s ago, {len(pts)} points')
else:
    print('No data in last 10 minutes')
"
```

### Verify both gateway and CAN controller have data

```python
import requests, time

API = "https://api-dev.imatrixsys.com/api/v1"
# ... (login as above) ...

GW_SN = "799874683"
CAN_SN = "911934657"
now_ms = int(time.time() * 1000)
from_ms = now_ms - 600_000  # last 10 minutes

for sn, label in [(GW_SN, "Gateway"), (CAN_SN, "CAN Controller")]:
    r = requests.get(
        f"{API}/things/{sn}/sensor/2/history/{from_ms}/{now_ms}?group_by_time=NONE",
        headers=headers, timeout=15
    )
    data = r.json()
    pts = data.get(sn, {}).get("2", {}).get("data", [])
    if pts:
        age = (now_ms - pts[-1]["time"]) / 1000
        print(f"{label} ({sn}): {len(pts)} pts, age={age:.0f}s")
    else:
        print(f"{label} ({sn}): NO DATA")
```

### Verify no data on tsd/ path (phantom device check)

After switching from `tsd/` to `selfreport/`, verify no new data appears under unexpected serial numbers. List all things and check for any with recent data that don't match known gateway or CAN controller SNs.

### Verify selfreport URI in FC-1 log

Enable upload debug on the FC-1 and grep for URI patterns:

```bash
# Enable debug
cd ~/iMatrix/DOIP
./scripts/fc1 -d 192.168.7.1 cmd "debug DEBUGS_FOR_UPLOADS"

# Wait 5 minutes, then check
sshpass -p 'PasswordQConnect' ssh -p 22222 root@192.168.7.1 \
  "grep -i 'selfreport\|tsd/' /var/log/fc-1.log | tail -20"
```

**Expected:** All lines show `selfreport/{mfr_id}/{sn}/0`. No `tsd/{mfr_id}/1`.

---

## Well-Known Sensor IDs

| ID | Name | Unit | Source |
|----|------|------|--------|
| 2 | GPS Latitude | degrees | Gateway |
| 3 | GPS Longitude | degrees | Gateway |
| 29 | Battery Voltage | V | Gateway |
| 34 | Cellular RSSI | dBm | Gateway |
| 123 | Vehicle Speed | km/h | CAN Controller |

For a full list, use `GET /things/{sn}/sensors`.

---

## Existing Test Scripts

Located in `~/iMatrix/obd2_dev/monitoring/`:

| Script | Purpose |
|--------|---------|
| `score_test.py` | 10-criteria OBD2 cloud validation (6-hour run) |
| `validate_fleet_sensors.py` | Incremental 15-minute sensor validation |
| `generate_report.py` | Post-drive per-sensor analysis |
| `monitor.sh` | Real-time 5-minute staleness monitor |

All scripts use `IMATRIX_USER` and `IMATRIX_PASS` environment variables.

```bash
export IMATRIX_USER="imatrix.aptera@gmail.com"
export IMATRIX_PASS="SierraSnow100!"
cd ~/iMatrix/obd2_dev/monitoring
python3 score_test.py
```

---

## Troubleshooting

### HTTP 403 on data queries
- Wrong serial number. Use `GET /things?limit=50` to find correct SNs.
- The `sn` field in the things list is the serial number, not the `name` field.

### No data in time window
- Check if FC-1 has WAN connectivity: `grep 'UDP still down' /var/log/fc-1.log`
- Widen the time window (e.g., last 24 hours: `from_ms = now_ms - 86400_000`)
- Check upload status: `./scripts/fc1 -d 192.168.7.1 cmd "v"` (shows upload counters)

### Data appears under wrong SN
- Check `selfreport` URI in debug log
- Verify `get_can_serial_no()` returns expected value: `./scripts/fc1 -d 192.168.7.1 cmd "can status"`

---

*Last validated: 2026-03-17 — Gateway SN 799874683 and CAN SN 911934657 both showing fresh data via selfreport URI.*
