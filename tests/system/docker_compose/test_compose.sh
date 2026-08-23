#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
PROJECT="ocrservice-system-${RANDOM}-${BASHPID}"
ENV_FILE=$(mktemp)
COMPOSE=(docker compose --project-name "$PROJECT" --env-file "$ENV_FILE" -f "$ROOT_DIR/docker-compose.yml")
subscriber_pid=''

cleanup() {
    set +e
    [[ -z "$subscriber_pid" ]] || kill "$subscriber_pid" >/dev/null 2>&1
    "${COMPOSE[@]}" down --volumes --remove-orphans >/dev/null 2>&1
    rm -f -- "$ENV_FILE"
}
trap cleanup EXIT INT TERM

cp "$ROOT_DIR/.env.example" "$ENV_FILE"
command -v docker >/dev/null || { printf 'docker is required\n' >&2; exit 2; }
docker compose version >/dev/null
"${COMPOSE[@]}" config --quiet
"${COMPOSE[@]}" up --build -d

mysql_exec() {
    local sql=$1
    "${COMPOSE[@]}" exec -T mysql sh -c 'MYSQL_PWD="$MYSQL_PASSWORD" exec mysql -u ocrservice -D ocrservice --batch --skip-column-names' <<<"$sql"
}

wait_healthy() {
    local timeout_seconds=$1
    shift
    local deadline=$((SECONDS + timeout_seconds))
    local service health all_healthy container_id
    while ((SECONDS < deadline)); do
        all_healthy=true
        for service in "$@"; do
            health=$("${COMPOSE[@]}" ps --format '{{.Health}}' "$service" 2>/dev/null || true)
            if [[ "$health" != healthy ]]; then
                all_healthy=false
                break
            fi
        done
        [[ "$all_healthy" == true ]] && return 0
        sleep 2
    done

    printf 'services did not become healthy within %s seconds\n' "$timeout_seconds" >&2
    "${COMPOSE[@]}" ps >&2 || true
    for service in "$@"; do
        container_id=$("${COMPOSE[@]}" ps -q "$service" 2>/dev/null || true)
        if [[ -n "$container_id" ]]; then
            printf '%s: ' "$service" >&2
            docker inspect --format '{{if .State.Health}}{{.State.Health.Status}}{{else}}{{.State.Status}}{{end}}' "$container_id" >&2 || true
        fi
    done
    return 1
}

wait_healthy 180 mysql mqtt app
http_status=$(timeout 5 bash -c 'exec 3<>/dev/tcp/127.0.0.1/8080; printf "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n" >&3; IFS= read -r status <&3; printf "%s" "$status"')
[[ "$http_status" == HTTP/1.1\ 200* ]]
timeout 5 bash -c 'exec 3<>/dev/tcp/127.0.0.1/1883; exec 3>&-; exec 3<&-'
"${COMPOSE[@]}" exec -T app curl --fail --silent http://127.0.0.1:8080/health >/dev/null

"${COMPOSE[@]}" exec -T app sha256sum /app/models/yolov8_plate.onnx /app/models/lprnet.onnx
mysql_exec 'SELECT 1' >/dev/null
"${COMPOSE[@]}" exec -T app sh -c 'printf system-marker > /app/data/images/system-marker; printf system-log-marker > /app/data/logs/system-marker'
mysql_exec "INSERT INTO access_lists (list_type, plate_number, remark, created_by_user_id, created_by_display_name, created_at) VALUES ('WHITE', 'TEST-PERSIST', 'persisted', 1, 'admin', UTC_TIMESTAMP(3));" >/dev/null
"${COMPOSE[@]}" exec -T mqtt sh -c 'printf broker-marker > /mosquitto/data/system-marker'
"${COMPOSE[@]}" exec -T mqtt kill -USR1 1
sleep 1
"${COMPOSE[@]}" exec -T mqtt test -e /mosquitto/data/mosquitto.db

subscriber_log=$(mktemp)
timeout 8 "${COMPOSE[@]}" exec -T mqtt sh -c 'mosquitto_sub -h 127.0.0.1 -u "$MQTT_MANAGEMENT_USERNAME" -P "$MQTT_MANAGEMENT_PASSWORD" -t plate/management/recognition-events -C 1 -W 5' >"$subscriber_log" 2>&1 &
subscriber_pid=$!
sleep 1
"${COMPOSE[@]}" exec -T mqtt sh -c 'mosquitto_pub -h 127.0.0.1 -u "$MQTT_SERVER_USERNAME" -P "$MQTT_SERVER_PASSWORD" -t plate/management/recognition-events -m test-message'
wait "$subscriber_pid"
grep -Fq test-message "$subscriber_log"
rm -f "$subscriber_log"
if "${COMPOSE[@]}" exec -T mqtt sh -c 'mosquitto_sub -h 127.0.0.1 -t plate/management/recognition-events -C 1 -W 2' >/dev/null 2>&1; then
    printf 'anonymous MQTT access unexpectedly succeeded\n' >&2
    exit 1
fi

mysql_exec "INSERT INTO recognition_logs (recognition_id, device_id, capture_id, image_sha256, revision, status, image_path, image_mime, image_size_bytes, captured_at, started_at, created_at, updated_at) VALUES ('00000000-0000-4000-8000-000000000001', 'device-001', '00000000-0000-4000-8000-000000000002', REPEAT('0', 64), 1, 'PROCESSING', 'system-test.jpg', 'image/jpeg', 1, UTC_TIMESTAMP(3), UTC_TIMESTAMP(3), UTC_TIMESTAMP(3), UTC_TIMESTAMP(3));" >/dev/null

"${COMPOSE[@]}" restart app mqtt
"${COMPOSE[@]}" up -d
wait_healthy 120 mqtt app
result=''
for attempt in $(seq 1 30); do
    result=$(mysql_exec "SELECT CONCAT(status, '/', revision, '/', COALESCE(error_code, '')) FROM recognition_logs WHERE recognition_id = '00000000-0000-4000-8000-000000000001';" || true)
    [[ "$result" == FAILED/2/SERVER_RESTARTED ]] && break
    sleep 2
done
[[ "$result" == FAILED/2/SERVER_RESTARTED ]]
"${COMPOSE[@]}" exec -T app test -s /app/data/images/system-marker
"${COMPOSE[@]}" exec -T app test -s /app/data/logs/system-marker
[[ "$(mysql_exec "SELECT remark FROM access_lists WHERE plate_number = 'TEST-PERSIST';")" == persisted ]]
"${COMPOSE[@]}" exec -T mqtt test -s /mosquitto/data/security/password_file
"${COMPOSE[@]}" exec -T mqtt test -s /mosquitto/data/system-marker
printf 'docker compose system checks passed for project %s\n' "$PROJECT"
