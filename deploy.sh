#!/bin/bash
#
# deploy.sh - Deploy DoIP server to a remote machine
#
# Usage: ./deploy.sh <user> <password> <ip_address> [dest_dir]
#
# Examples:
#   ./deploy.sh greg 'Sierra007!' 192.168.7.101
#   ./deploy.sh greg 'Sierra007!' 192.168.7.101 /opt/doip
#
# Deploys:
#   - doip-server binary
#   - doip-server.conf (patched for bind 0.0.0.0 and local paths)
#   - phonehome.conf (patched for local paths)
#   - phonehome scripts (connect, keygen, register)
#   - run.sh convenience launcher
#
# After deploy, starts the server automatically.

set -e

# --- Parse arguments ---
if [ $# -lt 3 ]; then
    echo "Usage: $0 <user> <password> <ip_address> [dest_dir]"
    echo ""
    echo "  user       SSH username"
    echo "  password   SSH password"
    echo "  ip_address Target machine IP"
    echo "  dest_dir   Install directory (default: /home/<user>/DoIP)"
    exit 1
fi

USER="$1"
PASS="$2"
HOST_IP="$3"
DEST="${4:-/home/${USER}/DoIP}"
HOST="${USER}@${HOST_IP}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# --- Helpers ---
do_ssh() {
    sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 "$HOST" "$@"
}

do_scp() {
    sshpass -p "$PASS" scp -o StrictHostKeyChecking=no "$@"
}

# --- Preflight checks ---
echo "=== DoIP Server Deployment ==="
echo "  Target:  ${HOST}:${DEST}"
echo "  Source:  ${SCRIPT_DIR}"
echo ""

# Check sshpass
if ! command -v sshpass &>/dev/null; then
    echo "ERROR: sshpass not found. Install with: sudo apt install sshpass"
    exit 1
fi

# Check binary exists
if [ ! -f "${SCRIPT_DIR}/doip-server" ]; then
    echo "ERROR: doip-server binary not found."
    echo "Run 'make' in ${SCRIPT_DIR} first."
    exit 1
fi

# Check connectivity
echo -n "Testing connection... "
if ! do_ssh 'echo ok' 2>/dev/null; then
    echo "FAILED"
    echo "Cannot SSH to ${HOST}. Check IP, username, and password."
    exit 1
fi
echo "OK"

# --- Stop existing server if running ---
echo -n "Stopping existing server... "
do_ssh "pkill -f '${DEST}/bin/doip-server' 2>/dev/null; sleep 1" || true
echo "done"

# --- Create directory structure ---
echo -n "Creating directories... "
do_ssh "mkdir -p ${DEST}/{bin,etc,scripts,blobs,log,run}"
echo "done"

# --- Upload files ---
echo -n "Uploading binary... "
do_scp "${SCRIPT_DIR}/doip-server" "${HOST}:${DEST}/bin/doip-server"
do_ssh "chmod +x ${DEST}/bin/doip-server"
echo "done"

echo -n "Uploading config... "
do_scp "${SCRIPT_DIR}/doip-server.conf" "${HOST}:${DEST}/etc/doip-server.conf"
echo "done"

echo -n "Uploading phonehome config... "
do_scp "${SCRIPT_DIR}/etc/phonehome/phonehome.conf" "${HOST}:${DEST}/etc/phonehome.conf"
echo "done"

echo -n "Uploading scripts... "
for s in phonehome-connect.sh phonehome-keygen.sh phonehome-register.sh; do
    [ -f "${SCRIPT_DIR}/scripts/$s" ] && do_scp "${SCRIPT_DIR}/scripts/$s" "${HOST}:${DEST}/scripts/"
done
do_ssh "chmod +x ${DEST}/scripts/*.sh 2>/dev/null" || true
echo "done"

# --- Patch configs for local paths ---
echo -n "Configuring for ${HOST_IP}... "
do_ssh "
    sed -i 's|^bind_address.*=.*|bind_address        = 0.0.0.0|' ${DEST}/etc/doip-server.conf
    sed -i 's|^blob_storage_dir.*=.*|blob_storage_dir    = ${DEST}/blobs|' ${DEST}/etc/doip-server.conf
    sed -i 's|^phonehome_config.*=.*|phonehome_config    = ${DEST}/etc/phonehome.conf|' ${DEST}/etc/doip-server.conf
    sed -i 's|^HMAC_SECRET_FILE=.*|HMAC_SECRET_FILE=${DEST}/etc/hmac_secret|' ${DEST}/etc/phonehome.conf
    sed -i 's|^CONNECT_SCRIPT=.*|CONNECT_SCRIPT=${DEST}/scripts/phonehome-connect.sh|' ${DEST}/etc/phonehome.conf
    sed -i 's|^LOCK_FILE=.*|LOCK_FILE=${DEST}/run/phonehome.lock|' ${DEST}/etc/phonehome.conf
"
echo "done"

# --- Create run script ---
echo -n "Creating run.sh... "
do_ssh "cat > ${DEST}/run.sh << 'RUNEOF'
#!/bin/bash
DOIP_DIR=\"\$(cd \"\$(dirname \"\$0\")\" && pwd)\"
exec \${DOIP_DIR}/bin/doip-server -c \${DOIP_DIR}/etc/doip-server.conf -l \${DOIP_DIR}/log/server.log \"\$@\"
RUNEOF
chmod +x ${DEST}/run.sh"
echo "done"

# --- Create stop script ---
echo -n "Creating stop.sh... "
do_ssh "cat > ${DEST}/stop.sh << 'STOPEOF'
#!/bin/bash
DOIP_DIR=\"\$(cd \"\$(dirname \"\$0\")\" && pwd)\"
PID=\$(pgrep -f \"\${DOIP_DIR}/bin/doip-server\")
if [ -n \"\$PID\" ]; then
    kill \$PID
    echo \"DoIP server stopped (PID \$PID)\"
else
    echo \"DoIP server not running\"
fi
STOPEOF
chmod +x ${DEST}/stop.sh"
echo "done"

# --- Start server ---
echo ""
echo "=== Starting server ==="
do_ssh "nohup ${DEST}/run.sh > /dev/null 2>&1 &
sleep 2
PID=\$(pgrep -f '${DEST}/bin/doip-server')
if [ -n \"\$PID\" ]; then
    echo \"Server running (PID \$PID)\"
    echo ''
    echo '--- Log ---'
    tail -10 ${DEST}/log/server.log
else
    echo 'FAILED to start'
    cat ${DEST}/log/server.log
    exit 1
fi"

echo ""
echo "══════════════════════════════════════════════════════════════════"
echo "  DEPLOYMENT COMPLETE"
echo "══════════════════════════════════════════════════════════════════"
echo ""
echo "  Server:  ${HOST_IP}:13400 (TCP+UDP)"
echo "  Install: ${DEST}/"
echo "  Log:     ${DEST}/log/server.log"
echo ""
echo "  Commands (on target):"
echo "    ${DEST}/run.sh        # Start (foreground)"
echo "    ${DEST}/stop.sh       # Stop"
echo "    tail -f ${DEST}/log/server.log  # Watch log"
echo ""
echo "══════════════════════════════════════════════════════════════════"
