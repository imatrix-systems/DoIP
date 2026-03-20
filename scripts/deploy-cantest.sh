#!/bin/bash
# Deploy DoIP Server to CAN-Test machine
#
# Usage: ./scripts/deploy-cantest.sh [--build] [--restart]
#   --build    Build before deploying (default: deploy only)
#   --restart  Restart the server after deploying
#
# Configuration (edit these if your setup changes):
TARGET_HOST="192.168.7.101"
TARGET_USER="greg"
TARGET_PASS="Sierra007!"
TARGET_DIR="/home/greg/DoIP"
SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=10"

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="$SCRIPT_DIR/doip-server"

DO_BUILD=false
DO_RESTART=false

for arg in "$@"; do
    case "$arg" in
        --build)   DO_BUILD=true ;;
        --restart) DO_RESTART=true ;;
        *)         echo "Unknown option: $arg"; exit 1 ;;
    esac
done

echo "=== DoIP Server Deploy to CAN-Test ==="
echo "  Target: ${TARGET_USER}@${TARGET_HOST}:${TARGET_DIR}"

# Step 1: Build if requested
if [ "$DO_BUILD" = true ]; then
    echo ""
    echo "[1/3] Building..."
    cd "$SCRIPT_DIR" || exit 1
    make clean && make
    if [ $? -ne 0 ]; then
        echo "ERROR: Build failed"
        exit 1
    fi
    echo "[OK] Build complete"
else
    echo ""
    echo "[1/3] Skipping build (use --build to rebuild)"
fi

# Verify binary exists
if [ ! -f "$BINARY" ]; then
    echo "ERROR: Binary not found: $BINARY"
    echo "       Run with --build or 'make' first"
    exit 1
fi

# Step 2: Deploy binary
echo ""
echo "[2/3] Deploying doip-server..."
sshpass -p "$TARGET_PASS" scp $SSH_OPTS "$BINARY" "${TARGET_USER}@${TARGET_HOST}:${TARGET_DIR}/doip-server.new"
if [ $? -ne 0 ]; then
    echo "ERROR: SCP failed — check credentials and network"
    exit 1
fi

# Atomic replace: move new over old
sshpass -p "$TARGET_PASS" ssh $SSH_OPTS "${TARGET_USER}@${TARGET_HOST}" \
    "chmod +x ${TARGET_DIR}/doip-server.new && mv ${TARGET_DIR}/doip-server.new ${TARGET_DIR}/doip-server"
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to replace binary on target"
    exit 1
fi
echo "[OK] Deployed to ${TARGET_DIR}/doip-server"

# Step 3: Restart if requested
if [ "$DO_RESTART" = true ]; then
    echo ""
    echo "[3/3] Restarting server..."
    sshpass -p "$TARGET_PASS" ssh $SSH_OPTS "${TARGET_USER}@${TARGET_HOST}" \
        "pkill -f 'doip-server' 2>/dev/null; sleep 1; cd ${TARGET_DIR} && nohup ./doip-server -c etc/doip-server.conf > log/server.log 2>&1 &"
    if [ $? -ne 0 ]; then
        echo "WARNING: Restart command failed — you may need to start manually"
    else
        sleep 2
        # Verify it started
        sshpass -p "$TARGET_PASS" ssh $SSH_OPTS "${TARGET_USER}@${TARGET_HOST}" \
            "pgrep -f doip-server > /dev/null && echo '[OK] Server running (PID: $(pgrep -f doip-server))' || echo 'WARNING: Server not running'"
    fi
else
    echo ""
    echo "[3/3] Skipping restart (use --restart to auto-restart)"
    echo "       To restart manually on CAN-Test:"
    echo "         pkill -f doip-server"
    echo "         cd ${TARGET_DIR} && ./doip-server -c etc/doip-server.conf"
fi

echo ""
echo "=== Deploy complete ==="
