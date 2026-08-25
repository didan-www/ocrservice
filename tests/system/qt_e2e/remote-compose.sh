#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
readonly ROOT_DIR
readonly VM_LAN_IP=${QT_E2E_VM_LAN_IP:-192.168.137.128}

fail() {
    printf 'qt-e2e-remote: %s\n' "$1" >&2
    exit 1
}

[[ $# -eq 2 ]] || fail 'usage: remote-compose.sh <action> <run-id>'
readonly ACTION=$1
readonly RUN_ID=$2
[[ $RUN_ID =~ ^[a-z0-9]{8,32}$ ]] || fail 'run-id must contain 8-32 lowercase letters or digits'

readonly WORK_DIR="/tmp/ocrservice-qt-e2e-${RUN_ID}"
readonly ENV_FILE="${WORK_DIR}/compose.env"
readonly OVERRIDE_FILE="${WORK_DIR}/compose.override.yml"
readonly ORIGINAL_MQTT_PASSWORD_FILE="${WORK_DIR}/original-management-password"
readonly PROJECT="ocrservice-qt-e2e-${RUN_ID}"
COMPOSE=(docker compose --project-name "$PROJECT" --env-file "$ENV_FILE"
         -f "$ROOT_DIR/docker-compose.yml" -f "$OVERRIDE_FILE")

require_state() {
    [[ -f $ENV_FILE && -f $OVERRIDE_FILE ]] || fail 'run state does not exist'
}

set_env_value() {
    local name=$1
    local value=$2
    local temporary="${ENV_FILE}.tmp"
    awk -F= -v name="$name" '$1 != name { print }' "$ENV_FILE" >"$temporary"
    printf '%s=%s\n' "$name" "$value" >>"$temporary"
    chmod 0600 "$temporary"
    mv -f -- "$temporary" "$ENV_FILE"
}

wait_healthy() {
    local deadline=$((SECONDS + 180))
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

publish_management() {
    local payload=$1
    printf '%s' "$payload" | "${COMPOSE[@]}" exec -T mqtt sh -c \
        'exec mosquitto_pub -h 127.0.0.1 -u "$MQTT_SERVER_USERNAME" -P "$MQTT_SERVER_PASSWORD" -q 1 -t plate/management/recognition-events -s'
}

snapshot_json() {
    local status=$1
    local revision=$2
    local captured_at completed_at duration plate error_code error_message
    captured_at=$(mysql_exec "SELECT CONCAT(DATE_FORMAT(CONVERT_TZ(captured_at, '+00:00', '+08:00'), '%Y-%m-%dT%H:%i:%s.'), LPAD(FLOOR(MICROSECOND(captured_at)/1000), 3, '0'), '+08:00') FROM recognition_logs WHERE recognition_id='23000000-0000-4000-8000-000000000001';")
    if [[ $status == PROCESSING ]]; then
        completed_at=''
        duration=''
        plate=''
        error_code=''
        error_message=''
    else
        completed_at=$(mysql_exec "SELECT CONCAT(DATE_FORMAT(CONVERT_TZ(completed_at, '+00:00', '+08:00'), '%Y-%m-%dT%H:%i:%s.'), LPAD(FLOOR(MICROSECOND(completed_at)/1000), 3, '0'), '+08:00') FROM recognition_logs WHERE recognition_id='23000000-0000-4000-8000-000000000001';")
        duration=321
        plate=QT023A
        error_code=''
        error_message=''
    fi
    python3 - "$status" "$revision" "$captured_at" "$completed_at" "$duration" "$plate" "$error_code" "$error_message" <<'PY'
import json
import sys

status, revision, captured_at, completed_at, duration, plate, error_code, error_message = sys.argv[1:]
value = {
    'schemaVersion': 1,
    'recognitionId': '23000000-0000-4000-8000-000000000001',
    'revision': int(revision),
    'deviceId': 'device-task023',
    'status': status,
    'plateNumber': plate or None,
    'errorCode': error_code or None,
    'errorMessage': error_message or None,
    'capturedAt': captured_at,
    'startedAt': captured_at,
    'completedAt': completed_at or None,
    'durationMs': int(duration) if duration else None,
}
print(json.dumps(value, ensure_ascii=False, separators=(',', ':')))
PY
}

start_stack() {
    [[ ! -e $WORK_DIR ]] || fail 'run state already exists'
    mkdir -m 0700 -- "$WORK_DIR"
    cp -- "$ROOT_DIR/.env.example" "$ENV_FILE"
    chmod 0600 "$ENV_FILE"
    sed -n 's/^MQTT_MANAGEMENT_PASSWORD=//p' "$ENV_FILE" >"$ORIGINAL_MQTT_PASSWORD_FILE"
    chmod 0600 "$ORIGINAL_MQTT_PASSWORD_FILE"
    [[ -s $ORIGINAL_MQTT_PASSWORD_FILE ]] || fail 'management credential is missing'
    set_env_value TOKEN_TTL_SECONDS 3600
    cat >"$OVERRIDE_FILE" <<EOF
services:
  mqtt:
    ports: !override
      - "${VM_LAN_IP}:1883:1883"
  app:
    environment:
      TOKEN_TTL_SECONDS: \${TOKEN_TTL_SECONDS:-3600}
    ports: !override
      - "${VM_LAN_IP}:8080:8080"
EOF
    chmod 0600 "$OVERRIDE_FILE"
    "${COMPOSE[@]}" config --quiet
    "${COMPOSE[@]}" up -d --no-build
    wait_healthy mysql mqtt app
    printf 'stack=healthy http=8080 mqtt=1883\n'
}

seed_processing() {
    require_state
    base64 -d "$ROOT_DIR/tests/system/embedded_e2e/fixtures/vehicle-1363-q75.jpg.b64" | \
        "${COMPOSE[@]}" exec -T app sh -c \
        'owner=$(stat -c "%u:%g" /app/data/images) && mkdir -p /app/data/images/task023 && cat > /app/data/images/task023/evidence.jpg && chown "$owner" /app/data/images/task023/evidence.jpg && chmod 0600 /app/data/images/task023/evidence.jpg'
    local image_sha image_size
    image_sha=$("${COMPOSE[@]}" exec -T app sha256sum /app/data/images/task023/evidence.jpg | awk '{print $1}')
    image_size=$("${COMPOSE[@]}" exec -T app stat -c '%s' /app/data/images/task023/evidence.jpg)
    mysql_exec "INSERT INTO devices(device_id, device_name, http_token_hash, mqtt_username, enabled, created_at, updated_at) VALUES ('device-task023', 'TASK-023 controlled device', REPEAT('2', 64), 'device-task023', TRUE, UTC_TIMESTAMP(3), UTC_TIMESTAMP(3)); INSERT INTO recognition_logs(recognition_id, device_id, capture_id, image_sha256, revision, status, image_path, image_mime, image_size_bytes, captured_at, started_at, created_at, updated_at) VALUES ('23000000-0000-4000-8000-000000000001', 'device-task023', '23000000-0000-4000-8000-000000000002', '${image_sha}', 1, 'PROCESSING', 'task023/evidence.jpg', 'image/jpeg', ${image_size}, UTC_TIMESTAMP(3), UTC_TIMESTAMP(3), UTC_TIMESTAMP(3), UTC_TIMESTAMP(3));" >/dev/null
    publish_management "$(snapshot_json PROCESSING 1)"
    printf 'processing=published image=stored\n'
}

finish_recognition() {
    require_state
    mysql_exec "UPDATE recognition_logs SET revision=2, status='SUCCEEDED', plate_number='QT023A', completed_at=UTC_TIMESTAMP(3), duration_ms=321, updated_at=UTC_TIMESTAMP(3) WHERE recognition_id='23000000-0000-4000-8000-000000000001' AND status='PROCESSING';" >/dev/null
    publish_management "$(snapshot_json SUCCEEDED 2)"
    printf 'final=published status=SUCCEEDED\n'
}

recreate_mqtt() {
    local password=$1
    require_state
    set_env_value MQTT_MANAGEMENT_PASSWORD "$password"
    "${COMPOSE[@]}" up -d --no-build --force-recreate mqtt >/dev/null
    wait_healthy mqtt
}

evidence() {
    require_state
    local app_log mqtt_log api_post image api_get mqtt_connect mqtt_attempts
    local mqtt_auth_rejected image_status http_results access_state
    app_log=$("${COMPOSE[@]}" exec -T app sh -c 'cat /app/data/logs/ocrservice.jsonl 2>/dev/null' 2>/dev/null || \
        docker run --rm -v "${PROJECT}_app-logs:/logs:ro" ubuntu:22.04 \
        sh -c 'cat /logs/ocrservice.jsonl 2>/dev/null' || true)
    api_post=$(grep -c 'method=POST route=\[REDACTED_PATH\] status=200' <<<"$app_log" || true)
    image=$(grep -c 'route=\[REDACTED_PATH\]/{recognitionId}/image status=200' <<<"$app_log" || true)
    api_get=$(grep -c 'method=GET route=\[REDACTED_PATH\] status=200' <<<"$app_log" || true)
    image_status=$(grep 'route=\[REDACTED_PATH\]/{recognitionId}/image status=' <<<"$app_log" | \
        grep -o 'status=[0-9][0-9]*' | cut -d= -f2 | tail -1 || true)
    [[ -n $image_status ]] || image_status=none
    http_results=$(python3 - 3<<<"$app_log" <<'PY'
import json
import os
import re

results = []
with os.fdopen(3, encoding='utf-8') as stream:
    for line in stream:
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if event.get('module') != 'http' or event.get('event') != 'request_completed':
            continue
        match = re.fullmatch(
            r'method=(GET|POST|DELETE) route=\[REDACTED_PATH\] status=([0-9]{3})',
            event.get('detail', ''))
        code = event.get('code')
        if match is None or not isinstance(code, str) \
                or re.fullmatch(r'[A-Z][A-Z0-9_]*', code) is None:
            continue
        results.append('{}:{}:{}'.format(match.group(1), match.group(2), code))
print(','.join(results) if results else 'none')
PY
    )
    access_state=$(mysql_exec \
        "SELECT CONCAT(COUNT(*), ':', COALESCE(MAX(list_type), 'NONE')) FROM access_lists WHERE plate_number='QT023A';")
    [[ $access_state =~ ^[01]:(NONE|WHITE|BLACK)$ ]] || access_state=invalid
    mqtt_log=$("${COMPOSE[@]}" logs --no-color mqtt 2>/dev/null || true)
    mqtt_connect=$(grep -Ec 'New client connected.* as [0-9a-f-]{36} ' <<<"$mqtt_log" || true)
    mqtt_attempts=$(grep -Ec 'New connection from ' <<<"$mqtt_log" || true)
    mqtt_auth_rejected=$(grep -Eic \
        'not authori[sz]ed|bad user name or password|CONNACK.*(0, 5|not authori[sz]ed)' \
        <<<"$mqtt_log" || true)
    printf 'apiPost=%s httpResults=%s accessState=%s image=%s imageStatus=%s apiGet=%s mqttConnect=%s mqttAttempts=%s mqttAuthRejected=%s\n' \
        "$api_post" "$http_results" "$access_state" "$image" "$image_status" "$api_get" \
        "$mqtt_connect" "$mqtt_attempts" "$mqtt_auth_rejected"
}

probe_access_list() {
    require_state
    python3 - "http://${VM_LAN_IP}:8080" 3<&0 <<'PY'
import json
import os
import re
import sys
import urllib.error
import urllib.request

base_url = sys.argv[1]
with os.fdopen(3, encoding='utf-8') as stream:
    credentials = json.load(stream)
if set(credentials) != {'username', 'password', 'clientId'}:
    raise SystemExit('invalid probe login input')

def json_request(path, method, body=None, token=None):
    headers = {'Accept': 'application/json'}
    data = None
    if body is not None:
        data = json.dumps(body, separators=(',', ':')).encode('utf-8')
        headers['Content-Type'] = 'application/json'
    if token is not None:
        headers['Authorization'] = 'Bearer ' + token
    request = urllib.request.Request(base_url + path, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        try:
            return error.code, json.loads(error.read())
        except (ValueError, AttributeError):
            return error.code, {}

status, login = json_request('/api/v1/auth/login', 'POST', credentials)
if status != 200:
    raise SystemExit('probe login failed with status={}'.format(status))
token = login['data']['accessToken']
status, result = json_request(
    '/api/v1/access-lists',
    'POST',
    {'listType': 'WHITE', 'plateNumber': 'QT023A', 'remark': 'TASK-023 UI E2E'},
    token)
code = result.get('code', 'UNKNOWN')
if not isinstance(code, str) or re.fullmatch(r'[A-Z][A-Z0-9_]*', code) is None:
    code = 'UNKNOWN'
print('probeAccessCreate status={} code={}'.format(status, code))
if status == 200:
    record_id = result.get('data', {}).get('id')
    if not isinstance(record_id, int) or record_id <= 0:
        raise SystemExit('probe create returned an invalid id')
    delete_status, deleted = json_request(
        '/api/v1/access-lists/{}'.format(record_id), 'DELETE', token=token)
    delete_code = deleted.get('code', 'UNKNOWN')
    if not isinstance(delete_code, str) or re.fullmatch(r'[A-Z][A-Z0-9_]*', delete_code) is None:
        delete_code = 'UNKNOWN'
    print('probeAccessDelete status={} code={}'.format(delete_status, delete_code))
PY
}

probe_image() {
    require_state
    local result expected_sha
    result=$(python3 - "http://${VM_LAN_IP}:8080" 3<&0 <<'PY'
import hashlib
import json
import os
import sys
import urllib.error
import urllib.request

base_url = sys.argv[1]
with os.fdopen(3, encoding='utf-8') as stream:
    credentials = json.load(stream)
if set(credentials) != {'username', 'password', 'clientId'}:
    raise SystemExit('invalid probe login input')
request = urllib.request.Request(
    base_url + '/api/v1/auth/login',
    data=json.dumps(credentials, separators=(',', ':')).encode('utf-8'),
    headers={'Content-Type': 'application/json'},
    method='POST')
with urllib.request.urlopen(request, timeout=10) as response:
    login = json.load(response)
token = login['data']['accessToken']
image_request = urllib.request.Request(
    base_url + '/api/v1/recognitions/23000000-0000-4000-8000-000000000001/image',
    headers={'Authorization': 'Bearer ' + token})
try:
    with urllib.request.urlopen(image_request, timeout=10) as response:
        body = response.read()
        print('status={} contentType={} bytes={} sha256={}'.format(
            response.status,
            response.headers.get_content_type(),
            len(body),
            hashlib.sha256(body).hexdigest()))
except urllib.error.HTTPError as error:
    try:
        failure_code = json.loads(error.read()).get('code', 'UNKNOWN')
    except (ValueError, AttributeError):
        failure_code = 'UNKNOWN'
    if not isinstance(failure_code, str) or not failure_code.replace('_', '').isalnum():
        failure_code = 'UNKNOWN'
    print('status={} contentType={} bytes=0 sha256=none code={}'.format(
        error.code, error.headers.get_content_type(), failure_code))
PY
    )
    expected_sha=$(base64 -d "$ROOT_DIR/tests/system/embedded_e2e/fixtures/vehicle-1363-q75.jpg.b64" | sha256sum | awk '{print $1}')
    if [[ $result == "status=200 contentType=image/jpeg "*" sha256=${expected_sha}" ]]; then
        printf 'probeImage %s match=1\n' "$result"
    else
        printf 'probeImage %s match=0\n' "$result"
    fi
}

cleanup() {
    local cleanup_status=0
    if [[ -f $ENV_FILE && -f $OVERRIDE_FILE ]]; then
        if ! "${COMPOSE[@]}" down --volumes --remove-orphans >/dev/null 2>&1; then
            cleanup_status=1
        fi
    fi
    if ! rm -rf -- "$WORK_DIR"; then
        cleanup_status=1
    fi
    local containers volumes networks protected host_mqtt
    if ! containers=$(docker ps -a --format '{{.Names}}' |
            awk -v prefix="${PROJECT}-" 'index($0, prefix) == 1 { count++ } END { print count + 0 }'); then
        containers=unknown
        cleanup_status=1
    fi
    if ! volumes=$(docker volume ls --format '{{.Name}}' |
            awk -v prefix="${PROJECT}_" 'index($0, prefix) == 1 { count++ } END { print count + 0 }'); then
        volumes=unknown
        cleanup_status=1
    fi
    if ! networks=$(docker network ls --format '{{.Name}}' |
            awk -v prefix="${PROJECT}_" 'index($0, prefix) == 1 { count++ } END { print count + 0 }'); then
        networks=unknown
        cleanup_status=1
    fi
    if ! protected=$(docker inspect --format '{{.State.Running}}' ocrservice-task008-mysql 2>/dev/null); then
        protected=missing
        cleanup_status=1
    fi
    if ! host_mqtt=$(systemctl is-active mosquitto 2>/dev/null); then
        host_mqtt=${host_mqtt:-unknown}
        cleanup_status=1
    fi
    printf 'cleanup containers=%s volumes=%s networks=%s protected=%s hostMosquitto=%s\n' \
        "$containers" "$volumes" "$networks" "$protected" "$host_mqtt"
    if [[ $containers != 0 || $volumes != 0 || $networks != 0 ||
          $protected != true || $host_mqtt != active ]]; then
        cleanup_status=1
    fi
    return "$cleanup_status"
}

case $ACTION in
    start) start_stack ;;
    seed-processing) seed_processing ;;
    finish) finish_recognition ;;
    mqtt-stop)
        require_state
        "${COMPOSE[@]}" stop mqtt >/dev/null
        printf 'mqtt=stopped\n'
        ;;
    mqtt-start)
        require_state
        "${COMPOSE[@]}" up -d --no-build mqtt >/dev/null
        wait_healthy mqtt
        printf 'mqtt=healthy\n'
        ;;
    mqtt-bad)
        recreate_mqtt "$(openssl rand -hex 32)"
        printf 'mqtt=healthy credential=rotated\n'
        ;;
    mqtt-good)
        [[ -s $ORIGINAL_MQTT_PASSWORD_FILE ]] || fail 'original management credential is missing'
        recreate_mqtt "$(<"$ORIGINAL_MQTT_PASSWORD_FILE")"
        printf 'mqtt=healthy credential=restored\n'
        ;;
    short-ttl)
        require_state
        set_env_value TOKEN_TTL_SECONDS 10
        "${COMPOSE[@]}" up -d --no-build --force-recreate app >/dev/null
        wait_healthy app
        printf 'app=healthy tokenTtlSeconds=10\n'
        ;;
    supersede)
        require_state
        python3 - "http://${VM_LAN_IP}:8080" 3<&0 <<'PY'
import json
import os
import sys
import urllib.request

with os.fdopen(3, 'rb') as stream:
    payload = stream.read()
request = urllib.request.Request(
    sys.argv[1] + '/api/v1/auth/login',
    data=payload,
    headers={'Accept': 'application/json', 'Content-Type': 'application/json'},
    method='POST')
with urllib.request.urlopen(request, timeout=10) as response:
    if response.status != 200:
        raise SystemExit('supersede login did not return HTTP 200')
    envelope = json.load(response)
if set(envelope) != {'success', 'code', 'message', 'requestId', 'data'} \
        or envelope['success'] is not True or envelope['code'] != 'OK' \
        or not isinstance(envelope['data'], dict):
    raise SystemExit('supersede login did not return a successful strict envelope')
PY
        printf 'session=superseded\n'
        ;;
    probe-access-list) probe_access_list ;;
    probe-image) probe_image ;;
    evidence) evidence ;;
    evidence-stop)
        require_state
        "${COMPOSE[@]}" stop app >/dev/null
        evidence
        ;;
    cleanup) cleanup ;;
    *) fail 'unsupported action' ;;
esac
