#!/bin/sh
# phonehome-register.sh
# Registers DCU public key with Bastion via HTTPS.
# FC-1 acts as NAT proxy; this script makes a direct HTTPS call outbound.

set -e

PHONEHOME_DIR="/etc/phonehome"
KEY_FILE="${PHONEHOME_DIR}/id_ed25519"
SENTINEL="${PHONEHOME_DIR}/.registration_complete"
SERIAL_FILE="/etc/dcu-serial"
FC1_SERIAL_FILE="/etc/fc1-serial"
BASTION_REG_URL="https://bastion.example.com/api/v1/devices/register"
PROVISIONING_TOKEN_FILE="${PHONEHOME_DIR}/provisioning_token"
CA_CERT="/etc/phonehome/bastion-ca.crt"
MAX_RETRIES=10
RETRY_INTERVAL=60
LOG_TAG="phonehome-register"

log() { logger -t "$LOG_TAG" "$1"; }

[ -f "$SENTINEL" ] && { log "Already registered. Exiting."; exit 0; }

DCU_SERIAL=$(cat "$SERIAL_FILE" | tr -d '[:space:]')
FC1_SERIAL=$(cat "$FC1_SERIAL_FILE" | tr -d '[:space:]')
PUBLIC_KEY=$(cat "${KEY_FILE}.pub")
PROV_TOKEN=$(cat "$PROVISIONING_TOKEN_FILE" | tr -d '[:space:]')

# Validate serials contain only safe characters (alphanumeric + hyphen)
case "$DCU_SERIAL" in *[!A-Za-z0-9-]*) log "ERROR: DCU serial contains invalid chars"; exit 1;; esac
case "$FC1_SERIAL" in *[!A-Za-z0-9-]*) log "ERROR: FC1 serial contains invalid chars"; exit 1;; esac

RESP_FILE=$(mktemp /tmp/phonehome_reg_XXXXXX.json)
trap 'rm -f "$RESP_FILE"' EXIT

attempt=0
while [ $attempt -lt $MAX_RETRIES ]; do
    attempt=$((attempt + 1))
    log "Registration attempt $attempt of $MAX_RETRIES"

    HTTP_STATUS=$(curl -s -o "$RESP_FILE" -w "%{http_code}" \
        --cacert "$CA_CERT" \
        --max-time 30 \
        -X POST "$BASTION_REG_URL" \
        -H "Authorization: Bearer $PROV_TOKEN" \
        -H "Content-Type: application/json" \
        -d "{
            \"serial\": \"$DCU_SERIAL\",
            \"fc1_serial\": \"$FC1_SERIAL\",
            \"device_type\": \"DCU\",
            \"public_key\": \"$PUBLIC_KEY\"
        }")

    if [ "$HTTP_STATUS" = "200" ] || [ "$HTTP_STATUS" = "201" ]; then
        log "Registration successful (HTTP $HTTP_STATUS)"
        touch "$SENTINEL"
        # Securely delete provisioning token after use
        shred -u "$PROVISIONING_TOKEN_FILE" 2>/dev/null || rm -f "$PROVISIONING_TOKEN_FILE"
        rm -f "$RESP_FILE"
        exit 0
    else
        log "Registration failed (HTTP $HTTP_STATUS). Response: $(cat "$RESP_FILE")"
        sleep $RETRY_INTERVAL
    fi
done

log "ERROR: Registration failed after $MAX_RETRIES attempts."
exit 1
