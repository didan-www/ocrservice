#!/usr/bin/env bash
set -Eeuo pipefail
umask 077

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
SIMULATOR=${EMBEDDED_SIMULATOR_BIN:-$ROOT_DIR/build-debug/tests/tests/simulators_embedded/embedded_device_simulator}
PROJECT="ocrservice-embedded-${RANDOM}-${BASHPID}"
WORK_DIR=$(mktemp -d)
ENV_FILE="$WORK_DIR/compose.env"
OVERRIDE_FILE="$WORK_DIR/compose.override.yml"
allocate_ports() {
    python3 - <<'PY'
import socket
sockets = []
ports = []
for _ in range(2):
    sock = socket.socket()
    sock.bind(('127.0.0.1', 0))
    sockets.append(sock)
    ports.append(sock.getsockname()[1])
print(*ports)
PY
}
write_override() {
    cat >"$OVERRIDE_FILE" <<EOF
services:
  mysql:
    command: !override
      - --character-set-server=utf8mb4
      - --collation-server=utf8mb4_0900_as_cs
      - --default-time-zone=+00:00
      - --mysql-native-password=ON
  mqtt:
    ports: !override
      - "127.0.0.1:${ALLOCATED_MQTT_PORT}:1883"
  app:
    ports: !override
      - "127.0.0.1:${ALLOCATED_HTTP_PORT}:8080"
EOF
}
read -r ALLOCATED_HTTP_PORT ALLOCATED_MQTT_PORT < <(allocate_ports)
write_override
COMPOSE=(docker compose --project-name "$PROJECT" --env-file "$ENV_FILE" \
    -f "$ROOT_DIR/docker-compose.yml" -f "$OVERRIDE_FILE")
background_pids=()
HTTP_PORT=''
MQTT_PORT=''

cleanup() {
    local status=$?
    set +e
    for process in "${background_pids[@]:-}"; do
        kill "$process" >/dev/null 2>&1 || true
        wait "$process" >/dev/null 2>&1 || true
    done
    "${COMPOSE[@]}" down --volumes --remove-orphans >/dev/null 2>&1 || true
    rm -rf -- "$WORK_DIR"
    exit "$status"
}
trap cleanup EXIT INT TERM

fail() {
    printf 'embedded-e2e: %s\n' "$1" >&2
    exit 1
}

stage() {
    printf 'embedded-e2e: stage=%s\n' "$1"
}

command -v docker >/dev/null || fail 'docker is required'
command -v python3 >/dev/null || fail 'python3 is required'
command -v openssl >/dev/null || fail 'openssl is required'
[[ -x $SIMULATOR ]] || fail 'embedded simulator binary is not executable'
cp "$ROOT_DIR/.env.example" "$ENV_FILE"
chmod 0600 "$ENV_FILE"

wait_healthy() {
    local timeout_seconds=$1
    shift
    local deadline=$((SECONDS + timeout_seconds))
    local service health all_healthy
    while ((SECONDS < deadline)); do
        all_healthy=true
        for service in "$@"; do
            health=$("${COMPOSE[@]}" ps --format '{{.Health}}' "$service" 2>/dev/null || true)
            if [[ $health != healthy ]]; then
                all_healthy=false
                break
            fi
        done
        [[ $all_healthy == true ]] && return 0
        sleep 2
    done
    "${COMPOSE[@]}" ps >&2 || true
    fail 'Compose services did not become healthy before the deadline'
}

mysql_exec() {
    local sql=$1
    "${COMPOSE[@]}" exec -T mysql sh -c \
        'MYSQL_PWD="$MYSQL_PASSWORD" exec mysql -u ocrservice -D ocrservice --batch --skip-column-names --raw --silent' \
        <<<"$sql"
}

env_value() {
    local name=$1
    sed -n "s/^${name}=//p" "$ENV_FILE"
}

check_app_health() {
    python3 - "$HTTP_PORT" <<'PY'
import sys
import urllib.request
with urllib.request.urlopen('http://127.0.0.1:' + sys.argv[1] + '/health', timeout=5) as response:
    if response.status != 200:
        raise SystemExit('application health endpoint is not HTTP 200')
PY
}

wait_app_mqtt_down() {
    python3 - "$HTTP_PORT" <<'PY'
import json
import sys
import time
import urllib.request

url = 'http://127.0.0.1:' + sys.argv[1] + '/health'
deadline = time.monotonic() + 30
while time.monotonic() < deadline:
    try:
        with urllib.request.urlopen(url, timeout=2) as response:
            body = json.load(response)
            data = body.get('data') if isinstance(body, dict) else None
            if (response.status == 200 and isinstance(body, dict) and
                    body.get('success') is True and
                    isinstance(data, dict) and data.get('status') == 'DEGRADED' and
                    data.get('mqtt') == 'DOWN'):
                raise SystemExit(0)
    except (OSError, ValueError):
        pass
    time.sleep(0.2)
raise SystemExit('application did not report HTTP 200 DEGRADED/MQTT DOWN')
PY
}

wait_mqtt_listener() {
    python3 - "$MQTT_PORT" <<'PY'
import socket
import sys
import time
deadline = time.monotonic() + 30
while time.monotonic() < deadline:
    try:
        with socket.create_connection(('127.0.0.1', int(sys.argv[1])), timeout=1):
            raise SystemExit(0)
    except OSError:
        time.sleep(0.2)
raise SystemExit('MQTT listener did not become ready')
PY
}

write_blank_png() {
    local output=$1
    python3 - "$output" <<'PY'
import struct
import sys
import zlib

width, height = 640, 480
raw = b''.join(b'\x00' + b'\xff\xff\xff' * width for _ in range(height))

def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)

png = b'\x89PNG\r\n\x1a\n'
png += chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(raw, 9))
png += chunk(b'IEND', b'')
with open(sys.argv[1], 'wb') as stream:
    stream.write(png)
PY
}

prepare_success_image() {
    local output=$1
    if [[ -n ${EMBEDDED_E2E_SUCCESS_IMAGE:-} ]]; then
        cp -- "$EMBEDDED_E2E_SUCCESS_IMAGE" "$output"
        [[ -n ${EMBEDDED_E2E_SUCCESS_IMAGE_SHA256:-} ]] ||
            fail 'alternate success fixture requires EMBEDDED_E2E_SUCCESS_IMAGE_SHA256'
        [[ $(sha256sum "$output" | awk '{print $1}') == "$EMBEDDED_E2E_SUCCESS_IMAGE_SHA256" ]] ||
            fail 'alternate success fixture SHA-256 does not match'
    else
        python3 - "$ROOT_DIR/tests/system/embedded_e2e/fixtures/vehicle-1363-q75.jpg.b64" "$output" <<'PY'
import base64
import hashlib
import sys
expected = 'c3cefbfe4685ba8c05a61b63b163cbc832846a8c341c8232ac85f01c0f122126'
with open(sys.argv[1], 'rb') as stream:
    data = base64.b64decode(stream.read().strip(), validate=True)
if hashlib.sha256(data).hexdigest() != expected:
    raise SystemExit('decoded success fixture SHA-256 does not match')
with open(sys.argv[2], 'wb') as stream:
    stream.write(data)
PY
    fi
}

provision_device() {
    local device_id=$1
    local mqtt_username=$2
    local output=$3
    local http_token_file="$WORK_DIR/${device_id}.http"
    local mqtt_password_file="$WORK_DIR/${device_id}.mqtt"
    openssl rand -hex 24 >"$http_token_file"
    openssl rand -hex 24 >"$mqtt_password_file"
    chmod 0600 "$http_token_file" "$mqtt_password_file"

    local mqtt_container
    mqtt_container=$("${COMPOSE[@]}" ps -q mqtt)
    docker cp "$http_token_file" "$mqtt_container:/tmp/${device_id}.http" >/dev/null
    docker cp "$mqtt_password_file" "$mqtt_container:/tmp/${device_id}.mqtt" >/dev/null
    "${COMPOSE[@]}" exec -T -u root mqtt chmod 0600 "/tmp/${device_id}.http" "/tmp/${device_id}.mqtt"
    "${COMPOSE[@]}" exec -T -u root mqtt bash -c \
        'set -a; source /tmp/ocrservice-provision.env; set +a; exec /tmp/provision-device.sh "$@"' \
        provision \
        --device-id "$device_id" \
        --device-name "TASK-022 ${device_id}" \
        --mqtt-username "$mqtt_username" \
        --http-token-file "/tmp/${device_id}.http" \
        --mqtt-password-file "/tmp/${device_id}.mqtt" \
        --http-base-url "http://127.0.0.1:${HTTP_PORT}" \
        --output "/tmp/${device_id}.json" >/dev/null
    docker cp "$mqtt_container:/tmp/${device_id}.json" "$output" >/dev/null
    chmod 0600 "$output"
    "${COMPOSE[@]}" exec -T -u root mqtt rm -f \
        "/tmp/${device_id}.http" "/tmp/${device_id}.mqtt" "/tmp/${device_id}.json"
}

wait_final_row() {
    local device_id=$1
    local simulator_status=${2:-}
    local deadline=$((SECONDS + 120))
    local row=''
    while ((SECONDS < deadline)); do
        if [[ -n $simulator_status && -s $simulator_status &&
              $(<"$simulator_status") != 0 ]]; then
            fail "simulator for ${device_id} failed before a final database state"
        fi
        row=$(mysql_exec "SELECT CONCAT(recognition_id, '/', status, '/', revision) FROM recognition_logs WHERE device_id='${device_id}' ORDER BY created_at DESC, recognition_id DESC LIMIT 1;" || true)
        if [[ $row == */SUCCEEDED/* || $row == */FAILED/* ]]; then
            printf '%s' "$row"
            return 0
        fi
        sleep 1
    done
    fail "recognition for ${device_id} did not reach a final database state"
}

make_final_payload() {
    local device_id=$1
    local output=$2
    mysql_exec "SELECT JSON_OBJECT('schemaVersion', 1, 'recognitionId', recognition_id, 'revision', revision, 'deviceId', device_id, 'status', status, 'plateNumber', plate_number, 'errorCode', error_code, 'errorMessage', error_message, 'capturedAt', CONCAT(DATE_FORMAT(CONVERT_TZ(captured_at, '+00:00', '+08:00'), '%Y-%m-%dT%H:%i:%s.'), LPAD(FLOOR(MICROSECOND(captured_at) / 1000), 3, '0'), '+08:00'), 'startedAt', CONCAT(DATE_FORMAT(CONVERT_TZ(started_at, '+00:00', '+08:00'), '%Y-%m-%dT%H:%i:%s.'), LPAD(FLOOR(MICROSECOND(started_at) / 1000), 3, '0'), '+08:00'), 'completedAt', CONCAT(DATE_FORMAT(CONVERT_TZ(completed_at, '+00:00', '+08:00'), '%Y-%m-%dT%H:%i:%s.'), LPAD(FLOOR(MICROSECOND(completed_at) / 1000), 3, '0'), '+08:00'), 'durationMs', duration_ms, 'gateAction', IF(status='SUCCEEDED', 'OPEN', 'KEEP_CLOSED')) FROM recognition_logs WHERE device_id='${device_id}' ORDER BY created_at DESC, recognition_id DESC LIMIT 1;" >"$output"
    chmod 0600 "$output"
    python3 - "$output" "$device_id" <<'PY'
import json
import sys
with open(sys.argv[1], encoding='utf-8') as stream:
    value = json.load(stream)
expected = {'schemaVersion', 'recognitionId', 'revision', 'deviceId', 'status', 'plateNumber',
            'errorCode', 'errorMessage', 'capturedAt', 'startedAt', 'completedAt',
            'durationMs', 'gateAction'}
if set(value) != expected or value['deviceId'] != sys.argv[2] or value['status'] not in {'SUCCEEDED', 'FAILED'}:
    raise SystemExit('invalid replay payload')
PY
}

publish_file_twice() {
    local device_id=$1
    local payload=$2
    local mqtt_container
    mqtt_container=$("${COMPOSE[@]}" ps -q mqtt)
    docker cp "$payload" "$mqtt_container:/tmp/replay.json" >/dev/null
    "${COMPOSE[@]}" exec -T mqtt sh -c \
        'mosquitto_pub -h 127.0.0.1 -u "$MQTT_SERVER_USERNAME" -P "$MQTT_SERVER_PASSWORD" -q 1 -t "$1" -f /tmp/replay.json && mosquitto_pub -h 127.0.0.1 -u "$MQTT_SERVER_USERNAME" -P "$MQTT_SERVER_PASSWORD" -q 1 -t "$1" -f /tmp/replay.json' \
        publish "plate/devices/${device_id}/recognition-results"
    "${COMPOSE[@]}" exec -T mqtt rm -f /tmp/replay.json
}

stage compose-start
compose_started=false
for attempt in 1 2 3; do
    if "${COMPOSE[@]}" config --quiet && "${COMPOSE[@]}" up -d; then
        compose_started=true
        break
    fi
    "${COMPOSE[@]}" down --volumes --remove-orphans >/dev/null 2>&1 || true
    read -r ALLOCATED_HTTP_PORT ALLOCATED_MQTT_PORT < <(allocate_ports)
    write_override
done
[[ $compose_started == true ]] || fail 'could not reserve loopback ports after three attempts'
wait_healthy 180 mysql mqtt app
HTTP_PORT=$("${COMPOSE[@]}" port app 8080 | awk -F: 'END {print $NF}')
MQTT_PORT=$("${COMPOSE[@]}" port mqtt 1883 | awk -F: 'END {print $NF}')
[[ $HTTP_PORT =~ ^[1-9][0-9]*$ && $MQTT_PORT =~ ^[1-9][0-9]*$ ]] ||
    fail 'failed to resolve dynamic Compose ports'
[[ $HTTP_PORT == "$ALLOCATED_HTTP_PORT" && $MQTT_PORT == "$ALLOCATED_MQTT_PORT" ]] ||
    fail 'Compose did not preserve the allocated loopback ports'
app_mysql_password=$(env_value MYSQL_PASSWORD)
[[ $app_mysql_password =~ ^[A-Za-z0-9._-]+$ ]] ||
    fail 'teaching MySQL password is not safe for the isolated auth compatibility setup'
"${COMPOSE[@]}" exec -T mysql sh -c \
    'printf "ALTER USER '\''ocrservice'\''@'\''%%'\'' IDENTIFIED WITH mysql_native_password BY '\''%s'\'';\n" "$MYSQL_PASSWORD" | MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysql --user=root --batch --silent'

mqtt_container=$("${COMPOSE[@]}" ps -q mqtt)
docker cp "$ROOT_DIR/scripts/provision-device.sh" "$mqtt_container:/tmp/provision-device.sh" >/dev/null
"${COMPOSE[@]}" exec -T -u root mqtt apk add --no-cache mariadb-client coreutils grep >/dev/null
provision_env="$WORK_DIR/provision.env"
cat >"$provision_env" <<EOF
MYSQL_HOST=mysql
MYSQL_PORT=3306
MYSQL_DATABASE=ocrservice
MYSQL_USER=ocrservice
MYSQL_PASSWORD=$(env_value MYSQL_PASSWORD)
MQTT_PUBLIC_HOST=127.0.0.1
MQTT_PORT=${MQTT_PORT}
MQTT_SERVER_USERNAME=plate-server
MQTT_MANAGEMENT_USERNAME=management-client
EOF
chmod 0600 "$provision_env"
docker cp "$provision_env" "$mqtt_container:/tmp/ocrservice-provision.env" >/dev/null
"${COMPOSE[@]}" exec -T -u root mqtt chmod 0700 /tmp/provision-device.sh
"${COMPOSE[@]}" exec -T -u root mqtt chmod 0600 /tmp/ocrservice-provision.env
"${COMPOSE[@]}" exec -T -u root mqtt bash -c \
    'set -a; source /tmp/ocrservice-provision.env; set +a; MYSQL_PWD="$MYSQL_PASSWORD" mysql --protocol=TCP --host="$MYSQL_HOST" --port="$MYSQL_PORT" --user="$MYSQL_USER" --database="$MYSQL_DATABASE" --batch --skip-column-names -e "SELECT 1" >/dev/null'

blank_image="$WORK_DIR/blank.png"
success_image="$WORK_DIR/success.png"
write_blank_png "$blank_image"
prepare_success_image "$success_image"

failure_config="$WORK_DIR/device-failure.json"
other_config="$WORK_DIR/device-other.json"
success_config="$WORK_DIR/device-success.json"
persistent_config="$WORK_DIR/device-persistent.json"
loss_config="$WORK_DIR/device-loss.json"
rate_config="$WORK_DIR/device-rate.json"
provision_device device-task022-failure mqtt-task022-failure "$failure_config"
provision_device device-task022-other mqtt-task022-other "$other_config"
provision_device device-task022-success mqtt-task022-success "$success_config"
provision_device device-task022-persistent mqtt-task022-persistent "$persistent_config"
provision_device device-task022-loss mqtt-task022-loss "$loss_config"
provision_device device-task022-rate mqtt-task022-rate "$rate_config"

stage credential-rejection
wrong_config="$WORK_DIR/device-wrong-password.json"
python3 - "$failure_config" "$wrong_config" <<'PY'
import json
import os
import sys
with open(sys.argv[1], encoding='utf-8') as stream:
    value = json.load(stream)
value['mqtt']['password'] = 'definitely-wrong-task022-password'
with open(sys.argv[2], 'w', encoding='utf-8') as stream:
    json.dump(value, stream, separators=(',', ':'))
os.chmod(sys.argv[2], 0o600)
PY
"$SIMULATOR" --config "$wrong_config" --expect-connect-failure --timeout-seconds 8 >/dev/null

wrong_http_config="$WORK_DIR/device-wrong-http.json"
python3 - "$failure_config" "$wrong_http_config" <<'PY'
import json
import os
import sys
with open(sys.argv[1], encoding='utf-8') as stream:
    value = json.load(stream)
value['http']['bearerToken'] = 'definitely-wrong-task022-http-token'
with open(sys.argv[2], 'w', encoding='utf-8') as stream:
    json.dump(value, stream, separators=(',', ':'))
os.chmod(sys.argv[2], 0o600)
PY
before_wrong_http=$(mysql_exec "SELECT COUNT(*) FROM recognition_logs WHERE device_id='device-task022-failure';")
if "$SIMULATOR" --config "$wrong_http_config" --image "$blank_image" --timeout-seconds 10 \
    >"$WORK_DIR/wrong-http.out" 2>"$WORK_DIR/wrong-http.err"; then
    fail 'wrong HTTP Bearer token unexpectedly accepted an upload'
fi
after_wrong_http=$(mysql_exec "SELECT COUNT(*) FROM recognition_logs WHERE device_id='device-task022-failure';")
[[ $before_wrong_http == "$after_wrong_http" ]] || fail 'wrong HTTP token created a recognition record'

stage acl-isolation
acl_output="$WORK_DIR/acl.out"
acl_ready="$WORK_DIR/acl.ready"
"$SIMULATOR" --config "$failure_config" \
    --probe-forbidden-topic 'plate/devices/device-task022-other/recognition-results' \
    --ready-file "$acl_ready" \
    --timeout-seconds 6 >"$acl_output" &
acl_pid=$!
background_pids+=("$acl_pid")
for _ in $(seq 1 80); do
    [[ -f $acl_ready ]] && break
    sleep 0.1
done
[[ -f $acl_ready ]] || fail 'ACL other-device probe did not reach SUBACK'
"${COMPOSE[@]}" exec -T mqtt sh -c \
    'mosquitto_pub -h 127.0.0.1 -u "$MQTT_SERVER_USERNAME" -P "$MQTT_SERVER_PASSWORD" -q 1 -t plate/devices/device-task022-other/recognition-results -m acl-probe'
wait "$acl_pid"
grep -Fxq 'forbiddenDelivery=false' "$acl_output"

management_acl_output="$WORK_DIR/management-acl.out"
management_acl_ready="$WORK_DIR/management-acl.ready"
"$SIMULATOR" --config "$failure_config" \
    --probe-forbidden-topic 'plate/management/recognition-events' \
    --ready-file "$management_acl_ready" --timeout-seconds 6 >"$management_acl_output" &
management_acl_pid=$!
background_pids+=("$management_acl_pid")
for _ in $(seq 1 80); do
    [[ -f $management_acl_ready ]] && break
    sleep 0.1
done
[[ -f $management_acl_ready ]] || fail 'ACL management probe did not reach SUBACK'
"${COMPOSE[@]}" exec -T mqtt sh -c \
    'mosquitto_pub -h 127.0.0.1 -u "$MQTT_SERVER_USERNAME" -P "$MQTT_SERVER_PASSWORD" -q 1 -t plate/management/recognition-events -m acl-probe'
wait "$management_acl_pid"
grep -Fxq 'forbiddenDelivery=false' "$management_acl_output"

stage failed-processing-and-duplicate
processing_payload="$WORK_DIR/processing.json"
timeout 45 "${COMPOSE[@]}" exec -T mqtt sh -c \
    'mosquitto_sub -h 127.0.0.1 -u "$MQTT_MANAGEMENT_USERNAME" -P "$MQTT_MANAGEMENT_PASSWORD" -q 1 -t plate/management/recognition-events -C 1 -W 40' \
    >"$processing_payload" &
processing_pid=$!
background_pids+=("$processing_pid")
sleep 1
failure_output="$WORK_DIR/failure.out"
failure_status="$WORK_DIR/failure.status"
(
    set +e
    "$SIMULATOR" --config "$failure_config" --image "$blank_image" \
        --expect-action KEEP_CLOSED --settle-seconds 10 --timeout-seconds 120 \
        >"$failure_output"
    status=$?
    printf '%s\n' "$status" >"$failure_status"
    exit "$status"
) &
failure_pid=$!
background_pids+=("$failure_pid")
wait_final_row device-task022-failure "$failure_status" >/dev/null
replay_payload="$WORK_DIR/replay.json"
make_final_payload device-task022-failure "$replay_payload"
publish_file_twice device-task022-failure "$replay_payload"
wait "$failure_pid"
wait "$processing_pid"
python3 - "$processing_payload" <<'PY'
import json
import sys
with open(sys.argv[1], encoding='utf-8') as stream:
    value = json.load(stream)
if value.get('status') != 'PROCESSING' or value.get('deviceId') != 'device-task022-failure':
    raise SystemExit('missing exact PROCESSING management evidence')
PY
grep -Eq '^accepted=1 uniqueResults=1 duplicateResults=2 actionsExecuted=1$' "$failure_output"

stage succeeded-open
"$SIMULATOR" --config "$success_config" --image "$success_image" \
    --expect-action OPEN --timeout-seconds 120 >"$WORK_DIR/success.out"
grep -Fxq 'accepted=1 uniqueResults=1 duplicateResults=0 actionsExecuted=1' "$WORK_DIR/success.out"
[[ $(wait_final_row device-task022-success) == */SUCCEEDED/* ]]

stage persistent-session
"$SIMULATOR" --config "$persistent_config" --image "$blank_image" \
    --offline-during-upload --expect-action KEEP_CLOSED --timeout-seconds 120 \
    >"$WORK_DIR/persistent.out"
grep -Fxq 'accepted=1 uniqueResults=1 duplicateResults=0 actionsExecuted=1' "$WORK_DIR/persistent.out"

stage broker-publish-loss
ready_file="$WORK_DIR/loss.ready"
continue_file="$WORK_DIR/loss.continue"
loss_output="$WORK_DIR/loss.out"
"$SIMULATOR" --config "$loss_config" --image "$blank_image" --expect-no-result \
    --ready-file "$ready_file" --continue-file "$continue_file" --timeout-seconds 12 \
    >"$loss_output" &
loss_pid=$!
background_pids+=("$loss_pid")
for _ in $(seq 1 120); do
    [[ -f $ready_file ]] && break
    sleep 0.1
done
[[ -f $ready_file ]] || fail 'simulator did not confirm SUBACK before Broker-loss scenario'
"${COMPOSE[@]}" stop mqtt >/dev/null
wait_app_mqtt_down
: >"$continue_file"
wait "$loss_pid"
grep -Fxq 'accepted=1 uniqueResults=0 duplicateResults=0 actionsExecuted=0' "$loss_output"
loss_row=$(wait_final_row device-task022-loss)
[[ $loss_row == */FAILED/* ]]
app_id=$("${COMPOSE[@]}" ps -q app)
[[ $(docker inspect --format '{{.State.Running}}' "$app_id") == true ]]
check_app_health
"${COMPOSE[@]}" up -d mqtt >/dev/null
wait_healthy 60 mqtt
MQTT_PORT=$("${COMPOSE[@]}" port mqtt 1883 | awk -F: 'END {print $NF}')
[[ $MQTT_PORT == "$ALLOCATED_MQTT_PORT" ]] ||
    fail 'Broker restart changed the fixed loopback port'
if ! wait_mqtt_listener; then
    "${COMPOSE[@]}" ps >&2 || true
    "${COMPOSE[@]}" logs --tail 80 mqtt >&2 || true
    fail 'MQTT listener did not recover after Broker restart'
fi
loss_id=${loss_row%%/*}
"$SIMULATOR" --config "$loss_config" --persistent-no-result-id "$loss_id" \
    --timeout-seconds 6 >"$WORK_DIR/loss-reconnect.out"
grep -Fxq 'replayedResult=false' "$WORK_DIR/loss-reconnect.out"

stage rate-1hz
rate_start=$(date +%s)
"$SIMULATOR" --config "$rate_config" --image "$blank_image" --count 30 \
    --interval-ms 1000 --expect-action KEEP_CLOSED --timeout-seconds 180 \
    >"$WORK_DIR/rate.out"
rate_elapsed=$(( $(date +%s) - rate_start ))
((rate_elapsed >= 29)) || fail '1 Hz scenario completed before thirty scheduled upload slots'
grep -Fxq 'accepted=30 uniqueResults=30 duplicateResults=0 actionsExecuted=30' "$WORK_DIR/rate.out"
[[ $(mysql_exec "SELECT COUNT(*) FROM recognition_logs WHERE device_id='device-task022-rate';") == 30 ]]
check_app_health

printf 'embedded e2e passed: mqttAuth=1 httpAuth=1 acl=2 processing=1 failed=1 success=1 persistent=1 duplicate=1 brokerLoss=1 rate1Hz=30\n'
