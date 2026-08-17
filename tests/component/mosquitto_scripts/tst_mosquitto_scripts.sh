#!/usr/bin/env bash

set -Eeuo pipefail
umask 077
[[ ${OCRSERVICE_TASK011_TRACE:-0} == 1 ]] && set -x

skip() {
    printf 'component.mosquitto_scripts: SKIP: %s\n' "$1"
    exit 77
}

fatal() {
    printf 'component.mosquitto_scripts: FAIL: %s\n' "$1" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || skip "missing $1"
}

find_mysql_client() {
    if command -v mysql >/dev/null 2>&1; then
        command -v mysql
        return
    fi
    local candidate
    candidate=$(find "${HOME}/.cache/ocrservice-deps" -type f -path '*/usr/bin/mysql' \
        -executable -print -quit 2>/dev/null || true)
    [[ -n $candidate ]] || skip 'missing MySQL 8 client'
    printf '%s\n' "$candidate"
}

mysql_exec() {
    MYSQL_PWD="$MYSQL_ROOT_PASSWORD" "$MYSQL_CLIENT_BIN" \
        --protocol=TCP --host=127.0.0.1 --port="$MYSQL_PORT" \
        --user=root --database=ocrservice --batch --skip-column-names --raw "$@"
}

if [[ ${OCRSERVICE_TASK011_INNER:-0} != 1 ]]; then
    for command in docker unshare mosquitto mosquitto_passwd mosquitto_pub mosquitto_sub python3; do
        require_command "$command"
    done
    [[ -n ${OCRSERVICE_SOURCE_DIR:-} && -d $OCRSERVICE_SOURCE_DIR ]] ||
        fatal 'OCRSERVICE_SOURCE_DIR is missing'
    MYSQL_CLIENT_BIN=$(find_mysql_client)
    export MYSQL_CLIENT_BIN

    TEST_ROOT=$(mktemp -d /tmp/ocrservice-task011.XXXXXX)
    [[ $TEST_ROOT == /tmp/ocrservice-task011.* ]] || fatal 'unsafe temporary path'
    export TEST_ROOT
    MYSQL_CONTAINER="ocrservice-task011-mysql-${BASHPID}"
    export MYSQL_CONTAINER
    MYSQL_ROOT_PASSWORD='task011-mysql-password'
    export MYSQL_ROOT_PASSWORD
    MYSQL_IMAGE=${OCRSERVICE_MYSQL_TEST_IMAGE:-docker.m.daocloud.io/library/mysql:8.4}
    INNER_PROCESS=''

    cleanup_outer() {
        local status=$?
        if [[ -n $INNER_PROCESS ]]; then
            kill -TERM "$INNER_PROCESS" >/dev/null 2>&1 || true
            wait "$INNER_PROCESS" >/dev/null 2>&1 || true
        fi
        if [[ $MYSQL_CONTAINER == ocrservice-task011-mysql-* ]]; then
            docker rm -f "$MYSQL_CONTAINER" >/dev/null 2>&1 || true
        fi
        if [[ $status -ne 0 && ${OCRSERVICE_TASK011_KEEP_FAILED:-0} == 1 ]]; then
            printf 'component.mosquitto_scripts: preserved %s\n' "$TEST_ROOT" >&2
        elif [[ $TEST_ROOT == /tmp/ocrservice-task011.* && -d $TEST_ROOT ]]; then
            rm -rf -- "$TEST_ROOT"
        fi
        exit "$status"
    }
    trap cleanup_outer EXIT

    docker image inspect "$MYSQL_IMAGE" >/dev/null 2>&1 || skip "missing image $MYSQL_IMAGE"
    docker run -d --name "$MYSQL_CONTAINER" \
        -e MYSQL_ROOT_PASSWORD \
        -e MYSQL_DATABASE=ocrservice \
        -p 127.0.0.1::3306 \
        "$MYSQL_IMAGE" >/dev/null
    MYSQL_PORT=$(docker port "$MYSQL_CONTAINER" 3306/tcp | tail -1 | sed 's/.*://')
    [[ $MYSQL_PORT =~ ^[0-9]+$ ]] || fatal 'could not determine MySQL port'
    export MYSQL_PORT

    ready=false
    for _ in $(seq 1 90); do
        if mysql_exec --execute='SELECT 1' >/dev/null 2>&1; then
            ready=true
            break
        fi
        sleep 1
    done
    [[ $ready == true ]] || fatal 'MySQL did not become ready'
    mysql_exec <"${OCRSERVICE_SOURCE_DIR}/migrations/001_initial.sql"

    MQTT_TEST_PORT=$(python3 -c \
        'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')
    [[ $MQTT_TEST_PORT =~ ^[0-9]+$ ]] || fatal 'could not determine MQTT port'
    export MQTT_TEST_PORT

    set +e
    unshare --user --map-root-user --pid --fork --kill-child=KILL --mount-proc \
        env OCRSERVICE_TASK011_INNER=1 \
        OCRSERVICE_SOURCE_DIR="$OCRSERVICE_SOURCE_DIR" \
        TEST_ROOT="$TEST_ROOT" MYSQL_PORT="$MYSQL_PORT" \
        MYSQL_CLIENT_BIN="$MYSQL_CLIENT_BIN" \
        MQTT_TEST_PORT="$MQTT_TEST_PORT" bash "$0" &
    INNER_PROCESS=$!
    if ! python3 - "$INNER_PROCESS" <<'PY'
import os
import pathlib
import sys

cmdline = pathlib.Path(f"/proc/{sys.argv[1]}/cmdline").read_bytes()
raise SystemExit(1 if os.environ["MYSQL_ROOT_PASSWORD"].encode() in cmdline else 0)
PY
    then
        fatal 'MySQL password leaked into unshare argv'
    fi
    wait "$INNER_PROCESS"
    inner_status=$?
    INNER_PROCESS=''
    set -e
    exit "$inner_status"
fi

for command in awk cmp flock grep iconv od python3 sha256sum stat sync; do
    require_command "$command"
done

readonly INIT_SCRIPT="${OCRSERVICE_SOURCE_DIR}/scripts/init-mosquitto.sh"
readonly PROVISION_SCRIPT="${OCRSERVICE_SOURCE_DIR}/scripts/provision-device.sh"
readonly ACL_TEMPLATE="${OCRSERVICE_SOURCE_DIR}/deploy/mosquitto/acl.base.template"
readonly MOSQUITTO_ROOT="${TEST_ROOT}/mosquitto"
readonly SECURITY_ROOT="${MOSQUITTO_ROOT}/security"
readonly BROKER_DATA="${MOSQUITTO_ROOT}/data"
readonly BROKER_PID_FILE="${MOSQUITTO_ROOT}/mosquitto.pid"
readonly BROKER_LOG="${MOSQUITTO_ROOT}/broker.log"
readonly BROKER_CONFIG="${MOSQUITTO_ROOT}/mosquitto.conf"
readonly MQTT_TEST_PORT

mkdir -p "$MOSQUITTO_ROOT" "$BROKER_DATA" "${TEST_ROOT}/secrets" "${TEST_ROOT}/outputs"

export MQTT_SERVER_USERNAME='plate-server'
export MQTT_SERVER_PASSWORD='task011-server-password'
export MQTT_MANAGEMENT_USERNAME='management-client'
export MQTT_MANAGEMENT_PASSWORD='task011-management-password'
export MQTT_PUBLIC_HOST='127.0.0.1'
export MQTT_PORT=$MQTT_TEST_PORT
export MYSQL_HOST='127.0.0.1'
export MYSQL_DATABASE='ocrservice'
export MYSQL_USER='root'
export MYSQL_PASSWORD=$MYSQL_ROOT_PASSWORD
export MYSQL_BIN=$MYSQL_CLIENT_BIN
export MOSQUITTO_SECURITY_ROOT=$SECURITY_ROOT
export MOSQUITTO_ACL_BASE_TEMPLATE=$ACL_TEMPLATE
export MOSQUITTO_PID_FILE=$BROKER_PID_FILE
export MOSQUITTO_RUNTIME_USER='root'
export MOSQUITTO_RUNTIME_GROUP='root'
export MOSQUITTO_LOCK_TIMEOUT_SECONDS=10

broker_pid=''
second_broker_pid=''
cleanup_inner() {
    local status=$?
    if [[ -n $second_broker_pid ]]; then
        kill -TERM "$second_broker_pid" >/dev/null 2>&1 || true
        wait "$second_broker_pid" >/dev/null 2>&1 || true
    fi
    if [[ -n $broker_pid ]]; then
        kill -TERM "$broker_pid" >/dev/null 2>&1 || true
        wait "$broker_pid" >/dev/null 2>&1 || true
    fi
    exit "$status"
}
trap cleanup_inner EXIT

assert_file_mode() {
    [[ $(stat -c '%a' "$1") == "$2" ]] || fatal "wrong mode for $1"
}

assert_contains() {
    grep -Fq -- "$2" "$1" || fatal "$1 does not contain expected text"
}

assert_not_contains() {
    if grep -Fq -- "$2" "$1"; then
        fatal "$1 leaked forbidden text"
    fi
}

expect_failure() {
    local output=$1
    local error=$2
    shift 2
    if "$@" >"$output" 2>"$error"; then
        fatal 'command unexpectedly succeeded'
    fi
}

write_secret() {
    printf '%s\n' "$2" >"$1"
    chmod 0600 "$1"
}

run_init() {
    bash "$INIT_SCRIPT"
}

run_provision() {
    local device_id=$1
    local device_name=$2
    local mqtt_username=$3
    local token_file=$4
    local password_file=$5
    local output_file=$6
    shift 6
    bash "$PROVISION_SCRIPT" \
        --device-id "$device_id" \
        --device-name "$device_name" \
        --mqtt-username "$mqtt_username" \
        --http-token-file "$token_file" \
        --mqtt-password-file "$password_file" \
        --http-base-url "http://192.168.137.128:8080" \
        --output "$output_file" "$@"
}

start_broker() {
    cat >"$BROKER_CONFIG" <<EOF
user root
listener ${MQTT_TEST_PORT} 127.0.0.1
protocol mqtt
allow_anonymous false
password_file ${SECURITY_ROOT}/password_file
acl_file ${SECURITY_ROOT}/acl_file
persistence true
persistence_location ${BROKER_DATA}/
pid_file ${BROKER_PID_FILE}
log_dest file ${BROKER_LOG}
log_type all
EOF
    : >"$BROKER_LOG"
    mosquitto -c "$BROKER_CONFIG" >"${MOSQUITTO_ROOT}/broker-process.log" 2>&1 &
    broker_pid=$!
    local ready=false
    for _ in $(seq 1 50); do
        if [[ -s $BROKER_PID_FILE ]] && kill -0 "$broker_pid" >/dev/null 2>&1; then
            if mosquitto_pub -h 127.0.0.1 -p "$MQTT_TEST_PORT" \
                -u "$MQTT_SERVER_USERNAME" -P "$MQTT_SERVER_PASSWORD" \
                -t plate/management/recognition-events -m ready -q 1 >/dev/null 2>&1; then
                ready=true
                break
            fi
        fi
        sleep 0.1
    done
    [[ $ready == true ]] || fatal 'Mosquitto did not become ready'
}

stop_broker() {
    [[ -n $broker_pid ]] || return
    kill -TERM "$broker_pid"
    wait "$broker_pid"
    broker_pid=''
}

expect_delivery() {
    local username=$1
    local password=$2
    local topic=$3
    local payload=$4
    local received="${TEST_ROOT}/received-${BASHPID}-${RANDOM}"
    mosquitto_sub -h 127.0.0.1 -p "$MQTT_TEST_PORT" \
        -u "$username" -P "$password" -t "$topic" -q 1 -C 1 -W 5 >"$received" &
    local subscriber=$!
    sleep 0.2
    mosquitto_pub -h 127.0.0.1 -p "$MQTT_TEST_PORT" \
        -u "$MQTT_SERVER_USERNAME" -P "$MQTT_SERVER_PASSWORD" \
        -t "$topic" -m "$payload" -q 1
    wait "$subscriber" || fatal "allowed subscription failed for $username"
    [[ $(cat "$received") == "$payload" ]] || fatal 'delivered payload mismatch'
}

expect_filtered_delivery() {
    local username=$1
    local password=$2
    local filter=$3
    local expected_topic=$4
    local expected_payload=$5
    shift 5
    local output="${TEST_ROOT}/filtered-${BASHPID}-${RANDOM}"
    mosquitto_sub -v -h 127.0.0.1 -p "$MQTT_TEST_PORT" \
        -u "$username" -P "$password" -t "$filter" -q 1 -W 2 \
        >"$output" 2>"${output}.err" &
    local subscriber=$!
    sleep 0.2
    while (($# > 0)); do
        local publish_topic=$1
        local publish_payload=$2
        shift 2
        mosquitto_pub -h 127.0.0.1 -p "$MQTT_TEST_PORT" \
            -u "$MQTT_SERVER_USERNAME" -P "$MQTT_SERVER_PASSWORD" \
            -t "$publish_topic" -m "$publish_payload" -q 1
    done
    set +e
    wait "$subscriber"
    local status=$?
    set -e
    [[ $status -ne 0 ]] || fatal "wildcard subscriber received unexpected messages for $username"
    [[ $(wc -l <"$output") -eq 1 ]] ||
        fatal "wildcard subscriber received an unexpected message count for $username"
    [[ $(cat "$output") == "${expected_topic} ${expected_payload}" ]] ||
        fatal "wildcard subscriber received an unauthorized topic for $username"
}

expect_no_delivery() {
    local username=$1
    local password=$2
    local filter=$3
    shift 3
    local output="${TEST_ROOT}/no-delivery-${BASHPID}-${RANDOM}"
    mosquitto_sub -v -h 127.0.0.1 -p "$MQTT_TEST_PORT" \
        -u "$username" -P "$password" -t "$filter" -q 1 -W 1 \
        >"$output" 2>"${output}.err" &
    local subscriber=$!
    sleep 0.2
    while (($# > 0)); do
        mosquitto_pub -h 127.0.0.1 -p "$MQTT_TEST_PORT" \
            -u "$MQTT_SERVER_USERNAME" -P "$MQTT_SERVER_PASSWORD" \
            -t "$1" -m "$2" -q 1
        shift 2
    done
    set +e
    wait "$subscriber"
    local status=$?
    set -e
    [[ $status -ne 0 && ! -s $output ]] ||
        fatal "read-disabled account received a message for $username"
}

expect_publish_denied() {
    local username=$1
    local password=$2
    local topic=$3
    local output="${TEST_ROOT}/publish-denied-${BASHPID}-${RANDOM}"
    set +e
    mosquitto_pub -d -h 127.0.0.1 -p "$MQTT_TEST_PORT" \
        -u "$username" -P "$password" -t "$topic" -m forbidden -q 1 \
        >"$output" 2>&1
    local status=$?
    set -e
    sleep 0.1
    if [[ $status -eq 0 ]]; then
        grep -Fi 'Denied PUBLISH' "$BROKER_LOG" | grep -Fq "$topic" ||
            fatal "forbidden publish succeeded for $username"
    fi
}

# Static initialization validation and idempotency.
expect_failure "${TEST_ROOT}/init-positional.out" "${TEST_ROOT}/init-positional.err" \
    bash "$INIT_SCRIPT" positional
assert_contains "${TEST_ROOT}/init-positional.err" 'UNKNOWN_ARGUMENT'
expect_failure "${TEST_ROOT}/init-unknown.out" "${TEST_ROOT}/init-unknown.err" \
    bash "$INIT_SCRIPT" --unknown
assert_contains "${TEST_ROOT}/init-unknown.err" 'UNKNOWN_ARGUMENT'
expect_failure "${TEST_ROOT}/init-missing.out" "${TEST_ROOT}/init-missing.err" \
    env -u MQTT_SERVER_PASSWORD bash "$INIT_SCRIPT"
assert_contains "${TEST_ROOT}/init-missing.err" 'MISSING_ENVIRONMENT'
expect_failure "${TEST_ROOT}/init-duplicate.out" "${TEST_ROOT}/init-duplicate.err" \
    env MQTT_MANAGEMENT_USERNAME="$MQTT_SERVER_USERNAME" bash "$INIT_SCRIPT"
assert_contains "${TEST_ROOT}/init-duplicate.err" 'MQTT_USERNAME_CONFLICT'

MALICIOUS_TEMPLATE="${TEST_ROOT}/malicious-acl.template"
cp -- "$ACL_TEMPLATE" "$MALICIOUS_TEMPLATE"
printf 'topic read #\n' >>"$MALICIOUS_TEMPLATE"
expect_failure "${TEST_ROOT}/init-malicious-template.out" \
    "${TEST_ROOT}/init-malicious-template.err" \
    env MOSQUITTO_ACL_BASE_TEMPLATE="$MALICIOUS_TEMPLATE" bash "$INIT_SCRIPT"
assert_contains "${TEST_ROOT}/init-malicious-template.err" 'ACL_TEMPLATE_INVALID'

expect_failure "${TEST_ROOT}/duplicate-empty-option.out" \
    "${TEST_ROOT}/duplicate-empty-option.err" \
    bash "$PROVISION_SCRIPT" --device-id '' --device-id device-duplicate
assert_contains "${TEST_ROOT}/duplicate-empty-option.err" 'DUPLICATE_ARGUMENT'

mkdir -p "${SECURITY_ROOT}/acl_file"
expect_failure "${TEST_ROOT}/init-target-directory.out" \
    "${TEST_ROOT}/init-target-directory.err" run_init
assert_contains "${TEST_ROOT}/init-target-directory.err" 'FILE_RENAME_ERROR'
rmdir "${SECURITY_ROOT}/acl_file"
run_init >"${TEST_ROOT}/init.out" 2>"${TEST_ROOT}/init.err"
assert_file_mode "$SECURITY_ROOT" 700
assert_file_mode "${SECURITY_ROOT}/devices" 700
assert_file_mode "${SECURITY_ROOT}/password_file" 600
assert_file_mode "${SECURITY_ROOT}/acl.base" 640
assert_file_mode "${SECURITY_ROOT}/acl_file" 640
cmp -s "${SECURITY_ROOT}/acl.base" "${SECURITY_ROOT}/acl_file" ||
    fatal 'empty device ACL merge is not exact'

malformed_device='device-malformed'
malformed_fragment="${SECURITY_ROOT}/devices/$(printf '%s' "$malformed_device" | sha256sum | awk '{print $1}').acl"
printf 'user malformed-user\ntopic read invalid/topic\n' >"$malformed_fragment"
chmod 0640 "$malformed_fragment"
expect_failure "${TEST_ROOT}/malformed-fragment.out" \
    "${TEST_ROOT}/malformed-fragment.err" run_init
assert_contains "${TEST_ROOT}/malformed-fragment.err" 'ACL_FRAGMENT_INVALID'
rm -f -- "$malformed_fragment"

static_collision_device='device-static-collision'
static_collision_fragment="${SECURITY_ROOT}/devices/$(printf '%s' "$static_collision_device" | sha256sum | awk '{print $1}').acl"
printf 'user %s\ntopic read plate/devices/%s/recognition-results\n' \
    "$MQTT_SERVER_USERNAME" "$static_collision_device" >"$static_collision_fragment"
chmod 0640 "$static_collision_fragment"
expect_failure "${TEST_ROOT}/static-fragment.out" "${TEST_ROOT}/static-fragment.err" run_init
assert_contains "${TEST_ROOT}/static-fragment.err" 'MQTT_USERNAME_CONFLICT'
rm -f -- "$static_collision_fragment"
run_init >"${TEST_ROOT}/init-rerun.out" 2>"${TEST_ROOT}/init-rerun.err"

start_broker
expect_delivery "$MQTT_MANAGEMENT_USERNAME" "$MQTT_MANAGEMENT_PASSWORD" \
    plate/management/recognition-events management-event
expect_filtered_delivery "$MQTT_MANAGEMENT_USERNAME" "$MQTT_MANAGEMENT_PASSWORD" \
    'plate/#' plate/management/recognition-events management-wildcard-allowed \
    plate/devices/device-001/recognition-results management-must-not-see-device-one \
    plate/management/recognition-events management-wildcard-allowed \
    plate/devices/device-authorization-probe/recognition-results \
        management-must-not-see-device-two
expect_no_delivery "$MQTT_MANAGEMENT_USERNAME" "$MQTT_MANAGEMENT_PASSWORD" \
    'plate/devices/+/recognition-results' \
    plate/devices/device-001/recognition-results management-device-only-probe \
    plate/devices/device-authorization-probe/recognition-results \
        management-device-only-probe-two
expect_no_delivery "$MQTT_SERVER_USERNAME" "$MQTT_SERVER_PASSWORD" 'plate/#' \
    plate/management/recognition-events server-read-management-probe \
    plate/devices/device-001/recognition-results server-read-device-probe
expect_publish_denied "$MQTT_SERVER_USERNAME" "$MQTT_SERVER_PASSWORD" \
    unauthorized/server/topic
expect_publish_denied "$MQTT_MANAGEMENT_USERNAME" "$MQTT_MANAGEMENT_PASSWORD" \
    plate/management/recognition-events
if mosquitto_pub -h 127.0.0.1 -p "$MQTT_TEST_PORT" -t test -m anonymous >/dev/null 2>&1; then
    fatal 'anonymous publish succeeded'
fi

# New device, exact summary, ACL isolation, restart persistence and idempotency.
TOKEN_ONE="${TEST_ROOT}/secrets/device-one-token"
PASSWORD_ONE="${TEST_ROOT}/secrets/device-one-password"
SUMMARY_ONE="${TEST_ROOT}/outputs/device-one.json"
write_secret "$TOKEN_ONE" 'device-one-http-token'
write_secret "$PASSWORD_ONE" 'device-one-mqtt-password'

TOKEN_LINK="${TEST_ROOT}/secrets/device-one-token-link"
PASSWORD_LINK="${TEST_ROOT}/secrets/device-one-password-link"
ln -s "$TOKEN_ONE" "$TOKEN_LINK"
ln -s "$PASSWORD_ONE" "$PASSWORD_LINK"
expect_failure "${TEST_ROOT}/token-symlink.out" "${TEST_ROOT}/token-symlink.err" \
    run_provision device-path-check '路径检查' device-path-check-mqtt \
        "$TOKEN_LINK" "$PASSWORD_ONE" "${TEST_ROOT}/outputs/token-symlink.json"
assert_contains "${TEST_ROOT}/token-symlink.err" 'SECRET_FILE_INVALID'
expect_failure "${TEST_ROOT}/password-symlink.out" \
    "${TEST_ROOT}/password-symlink.err" \
    run_provision device-path-check '路径检查' device-path-check-mqtt \
        "$TOKEN_ONE" "$PASSWORD_LINK" "${TEST_ROOT}/outputs/password-symlink.json"
assert_contains "${TEST_ROOT}/password-symlink.err" 'SECRET_FILE_INVALID'

OUTPUT_SYMLINK_TARGET="${TEST_ROOT}/outputs/output-symlink-target.json"
OUTPUT_SYMLINK="${TEST_ROOT}/outputs/output-symlink.json"
printf 'unchanged\n' >"$OUTPUT_SYMLINK_TARGET"
ln -s "$OUTPUT_SYMLINK_TARGET" "$OUTPUT_SYMLINK"
expect_failure "${TEST_ROOT}/output-symlink.out" "${TEST_ROOT}/output-symlink.err" \
    run_provision device-path-check '路径检查' device-path-check-mqtt \
        "$TOKEN_ONE" "$PASSWORD_ONE" "$OUTPUT_SYMLINK"
assert_contains "${TEST_ROOT}/output-symlink.err" 'OUTPUT_PATH_INVALID'
[[ $(cat "$OUTPUT_SYMLINK_TARGET") == unchanged ]] ||
    fatal 'output symlink target was modified'

for collision in \
    "token:${TOKEN_ONE}" \
    "password:${PASSWORD_ONE}" \
    "security:${SECURITY_ROOT}/forbidden-summary.json" \
    "pid:${BROKER_PID_FILE}" \
    "template:${ACL_TEMPLATE}"; do
    collision_name=${collision%%:*}
    collision_path=${collision#*:}
    expect_failure "${TEST_ROOT}/output-${collision_name}.out" \
        "${TEST_ROOT}/output-${collision_name}.err" \
        run_provision device-path-check '路径检查' device-path-check-mqtt \
            "$TOKEN_ONE" "$PASSWORD_ONE" "$collision_path"
    assert_contains "${TEST_ROOT}/output-${collision_name}.err" 'OUTPUT_PATH_INVALID'
done

CONTROL_OUTPUT="${TEST_ROOT}/outputs/control"$'\n'"injected.json"
expect_failure "${TEST_ROOT}/output-control.out" "${TEST_ROOT}/output-control.err" \
    run_provision device-path-check '路径检查' device-path-check-mqtt \
        "$TOKEN_ONE" "$PASSWORD_ONE" "$CONTROL_OUTPUT"
assert_contains "${TEST_ROOT}/output-control.err" 'OUTPUT_PATH_INVALID'

expect_failure "${TEST_ROOT}/provision-malicious-template.out" \
    "${TEST_ROOT}/provision-malicious-template.err" \
    env MOSQUITTO_ACL_BASE_TEMPLATE="$MALICIOUS_TEMPLATE" \
        bash "$PROVISION_SCRIPT" \
        --device-id device-template-check --device-name '模板检查' \
        --mqtt-username device-template-check-mqtt \
        --http-token-file "$TOKEN_ONE" --mqtt-password-file "$PASSWORD_ONE" \
        --http-base-url http://192.168.137.128:8080 \
        --output "${TEST_ROOT}/outputs/template-check.json"
assert_contains "${TEST_ROOT}/provision-malicious-template.err" 'ACL_TEMPLATE_INVALID'

run_provision device-101 '入口设备一' device-101-mqtt \
    "$TOKEN_ONE" "$PASSWORD_ONE" "$SUMMARY_ONE" \
    >"${TEST_ROOT}/provision-one.out" 2>"${TEST_ROOT}/provision-one.err"
assert_file_mode "$SUMMARY_ONE" 600
assert_contains "${TEST_ROOT}/provision-one.out" 'stage=SUMMARY_WRITTEN'
assert_contains "${TEST_ROOT}/provision-one.out" "summary=${SUMMARY_ONE}"
assert_not_contains "${TEST_ROOT}/provision-one.out" 'device-one-http-token'
assert_not_contains "${TEST_ROOT}/provision-one.out" 'device-one-mqtt-password'
assert_not_contains "${TEST_ROOT}/provision-one.err" 'device-one-http-token'
assert_not_contains "${TEST_ROOT}/provision-one.err" 'device-one-mqtt-password'
python3 - "$SUMMARY_ONE" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    value = json.load(source)
assert set(value) == {"deviceId", "http", "mqtt"}
assert set(value["http"]) == {"baseUrl", "bearerToken", "uploadPath"}
assert set(value["mqtt"]) == {
    "host", "port", "tls", "username", "password", "clientId",
    "cleanSession", "qos", "resultTopic"
}
assert value["deviceId"] == "device-101"
assert value["http"]["baseUrl"] == "http://192.168.137.128:8080"
assert value["http"]["bearerToken"] == "device-one-http-token"
assert value["mqtt"]["clientId"] == "device-101"
assert value["mqtt"]["cleanSession"] is False
assert value["mqtt"]["qos"] == 1
PY
expected_hash=$(printf '%s' 'device-one-http-token' | sha256sum | awk '{print $1}')
row=$(mysql_exec --execute="SELECT CONCAT(device_id, ':', http_token_hash, ':', mqtt_username, ':', enabled) FROM devices WHERE device_id='device-101'")
[[ $row == "device-101:${expected_hash}:device-101-mqtt:1" ]] || fatal 'new device row mismatch'
expect_delivery device-101-mqtt device-one-mqtt-password \
    plate/devices/device-101/recognition-results device-result
expect_filtered_delivery device-101-mqtt device-one-mqtt-password \
    'plate/#' plate/devices/device-101/recognition-results device-wildcard-allowed \
    plate/management/recognition-events device-must-not-see-management \
    plate/devices/device-001/recognition-results device-must-not-see-device-one \
    plate/devices/device-101/recognition-results device-wildcard-allowed \
    plate/devices/device-authorization-probe/recognition-results \
        device-must-not-see-device-two
expect_no_delivery device-101-mqtt device-one-mqtt-password \
    'plate/devices/+/recognition-results' \
    plate/devices/device-001/recognition-results device-plus-probe-one \
    plate/devices/device-authorization-probe/recognition-results device-plus-probe-two
expect_publish_denied device-101-mqtt device-one-mqtt-password \
    plate/devices/device-101/recognition-results

device_fragment="${SECURITY_ROOT}/devices/$(printf '%s' device-101 | sha256sum | awk '{print $1}').acl"
printf 'user device-101-mqtt\ntopic read plate/devices/device-101/recognition-results\n' \
    >"${TEST_ROOT}/expected-device-fragment"
cmp -s "${TEST_ROOT}/expected-device-fragment" "$device_fragment" ||
    fatal 'device ACL fragment is not exact'

duplicate_device='device-duplicate-fragment'
duplicate_fragment="${SECURITY_ROOT}/devices/$(printf '%s' "$duplicate_device" | sha256sum | awk '{print $1}').acl"
printf 'user device-101-mqtt\ntopic read plate/devices/%s/recognition-results\n' \
    "$duplicate_device" >"$duplicate_fragment"
chmod 0640 "$duplicate_fragment"
expect_failure "${TEST_ROOT}/duplicate-fragment.out" \
    "${TEST_ROOT}/duplicate-fragment.err" run_init
assert_contains "${TEST_ROOT}/duplicate-fragment.err" 'MQTT_USERNAME_CONFLICT'
rm -f -- "$duplicate_fragment"

stop_broker
start_broker
expect_delivery device-101-mqtt device-one-mqtt-password \
    plate/devices/device-101/recognition-results after-restart
run_provision device-101 '入口设备一' device-101-mqtt \
    "$TOKEN_ONE" "$PASSWORD_ONE" "$SUMMARY_ONE" \
    >"${TEST_ROOT}/idempotent.out" 2>"${TEST_ROOT}/idempotent.err"
count=$(mysql_exec --execute="SELECT COUNT(*) FROM devices WHERE device_id='device-101'")
[[ $count == 1 ]] || fatal 'idempotent rerun duplicated device'

TOKEN_DUPLICATE_USER="${TEST_ROOT}/secrets/duplicate-user-token"
write_secret "$TOKEN_DUPLICATE_USER" 'duplicate-user-http-token'
expect_failure "${TEST_ROOT}/duplicate-user.out" "${TEST_ROOT}/duplicate-user.err" \
    run_provision device-duplicate-user '重复用户' device-101-mqtt \
        "$TOKEN_DUPLICATE_USER" "$PASSWORD_ONE" \
        "${TEST_ROOT}/outputs/duplicate-user.json"
assert_contains "${TEST_ROOT}/duplicate-user.err" 'MQTT_USERNAME_CONFLICT'
expect_failure "${TEST_ROOT}/static-user.out" "${TEST_ROOT}/static-user.err" \
    run_provision device-static-user '静态用户冲突' "$MQTT_SERVER_USERNAME" \
        "$TOKEN_DUPLICATE_USER" "$PASSWORD_ONE" \
        "${TEST_ROOT}/outputs/static-user.json"
assert_contains "${TEST_ROOT}/static-user.err" 'MQTT_USERNAME_CONFLICT'

TOKEN_OUTPUT_DIRECTORY="${TEST_ROOT}/secrets/output-directory-token"
write_secret "$TOKEN_OUTPUT_DIRECTORY" 'output-directory-http-token'
OUTPUT_DIRECTORY="${TEST_ROOT}/outputs/device-output-directory.json"
mkdir "$OUTPUT_DIRECTORY"
expect_failure "${TEST_ROOT}/output-directory.out" "${TEST_ROOT}/output-directory.err" \
    run_provision device-output-directory '输出目录' device-output-directory-mqtt \
        "$TOKEN_OUTPUT_DIRECTORY" "$PASSWORD_ONE" "$OUTPUT_DIRECTORY"
assert_contains "${TEST_ROOT}/output-directory.err" 'OUTPUT_PATH_INVALID'
rmdir "$OUTPUT_DIRECTORY"
run_provision device-output-directory '输出目录' device-output-directory-mqtt \
    "$TOKEN_OUTPUT_DIRECTORY" "$PASSWORD_ONE" "$OUTPUT_DIRECTORY" \
    >"${TEST_ROOT}/output-directory-retry.out" \
    2>"${TEST_ROOT}/output-directory-retry.err"
assert_file_mode "$OUTPUT_DIRECTORY" 600

mysql_exec --execute="UPDATE devices SET enabled=FALSE WHERE device_id='device-101'"
run_provision device-101 '入口设备一' device-101-mqtt \
    "$TOKEN_ONE" "$PASSWORD_ONE" "$SUMMARY_ONE" \
    >"${TEST_ROOT}/enable.out" 2>"${TEST_ROOT}/enable.err"
enabled=$(mysql_exec --execute="SELECT enabled FROM devices WHERE device_id='device-101'")
[[ $enabled == 1 ]] || fatal 'disabled device was not re-enabled'

# Default conflicts and explicit credential rotation.
TOKEN_ROTATED="${TEST_ROOT}/secrets/device-one-token-rotated"
PASSWORD_ROTATED="${TEST_ROOT}/secrets/device-one-password-rotated"
write_secret "$TOKEN_ROTATED" 'device-one-http-token-rotated'
write_secret "$PASSWORD_ROTATED" 'device-one-mqtt-password-rotated'
expect_failure "${TEST_ROOT}/rotate-default.out" "${TEST_ROOT}/rotate-default.err" \
    run_provision device-101 '入口设备一' device-101-mqtt \
        "$TOKEN_ROTATED" "$PASSWORD_ROTATED" "$SUMMARY_ONE"
assert_contains "${TEST_ROOT}/rotate-default.err" 'HTTP_TOKEN_CONFLICT'
run_provision device-101 '入口设备一' device-101-mqtt \
    "$TOKEN_ROTATED" "$PASSWORD_ROTATED" "$SUMMARY_ONE" --rotate \
    >"${TEST_ROOT}/rotate.out" 2>"${TEST_ROOT}/rotate.err"
expect_delivery device-101-mqtt device-one-mqtt-password-rotated \
    plate/devices/device-101/recognition-results rotated-result
if mosquitto_pub -h 127.0.0.1 -p "$MQTT_TEST_PORT" \
    -u device-101-mqtt -P device-one-mqtt-password \
    -t plate/devices/device-101/recognition-results -m forbidden >/dev/null 2>&1; then
    fatal 'old MQTT password remained valid after rotation'
fi
expect_failure "${TEST_ROOT}/rename.out" "${TEST_ROOT}/rename.err" \
    run_provision device-101 '不同名称' device-101-mqtt \
        "$TOKEN_ROTATED" "$PASSWORD_ROTATED" "$SUMMARY_ONE"
assert_contains "${TEST_ROOT}/rename.err" 'DEVICE_NAME_CONFLICT'
expect_failure "${TEST_ROOT}/username-change.out" "${TEST_ROOT}/username-change.err" \
    run_provision device-101 '入口设备一' changed-mqtt-user \
        "$TOKEN_ROTATED" "$PASSWORD_ROTATED" "$SUMMARY_ONE" --rotate
assert_contains "${TEST_ROOT}/username-change.err" 'MQTT_USERNAME_CHANGE_UNSUPPORTED'

# Unsafe MQTT host must fail before writing a summary and never leak the value.
CONTROL_SUMMARY="${TEST_ROOT}/outputs/control-host.json"
old_host=$MQTT_PUBLIC_HOST
MQTT_PUBLIC_HOST=$'bad\001host'
export MQTT_PUBLIC_HOST
expect_failure "${TEST_ROOT}/control-host.out" "${TEST_ROOT}/control-host.err" \
    run_provision device-control '控制主机' device-control-mqtt \
        "$TOKEN_ONE" "$PASSWORD_ONE" "$CONTROL_SUMMARY"
[[ ! -e $CONTROL_SUMMARY ]] || fatal 'invalid host wrote a summary'
MQTT_PUBLIC_HOST=$old_host
export MQTT_PUBLIC_HOST

# MySQL, password, ACL, reload and summary failure stages remain safely rerunnable.
TOKEN_STAGE="${TEST_ROOT}/secrets/stage-token"
PASSWORD_STAGE="${TEST_ROOT}/secrets/stage-password"
write_secret "$TOKEN_STAGE" 'stage-http-token'
write_secret "$PASSWORD_STAGE" 'stage-mqtt-password'
old_mysql_port=$MYSQL_PORT
MYSQL_PORT=1
export MYSQL_PORT
expect_failure "${TEST_ROOT}/mysql-fail.out" "${TEST_ROOT}/mysql-fail.err" \
    run_provision device-mysql-fail '数据库失败' device-mysql-fail-mqtt \
        "$TOKEN_STAGE" "$PASSWORD_STAGE" "${TEST_ROOT}/outputs/mysql-fail.json"
assert_contains "${TEST_ROOT}/mysql-fail.err" 'stage=VALIDATED code=MYSQL_ERROR'
MYSQL_PORT=$old_mysql_port
export MYSQL_PORT

expect_failure "${TEST_ROOT}/password-fail.out" "${TEST_ROOT}/password-fail.err" \
    env MOSQUITTO_PASSWD_BIN=/bin/false bash "$PROVISION_SCRIPT" \
        --device-id device-password-fail --device-name '密码失败' \
        --mqtt-username device-password-fail-mqtt \
        --http-token-file "$TOKEN_STAGE" --mqtt-password-file "$PASSWORD_STAGE" \
        --http-base-url http://192.168.137.128:8080 \
        --output "${TEST_ROOT}/outputs/password-fail.json"
assert_contains "${TEST_ROOT}/password-fail.err" 'stage=MYSQL_COMMITTED code=PASSWORD_UPDATE_ERROR'
run_provision device-password-fail '密码失败' device-password-fail-mqtt \
    "$TOKEN_STAGE" "$PASSWORD_STAGE" "${TEST_ROOT}/outputs/password-fail.json" \
    >"${TEST_ROOT}/password-retry.out" 2>"${TEST_ROOT}/password-retry.err"

acl_fail_id='device-acl-fail'
acl_fail_name="$(printf '%s' "$acl_fail_id" | sha256sum | awk '{print $1}').acl"
TOKEN_ACL_FAIL="${TEST_ROOT}/secrets/acl-fail-token"
write_secret "$TOKEN_ACL_FAIL" 'stage-http-token-acl'
mkdir "${SECURITY_ROOT}/devices/${acl_fail_name}"
expect_failure "${TEST_ROOT}/acl-fail.out" "${TEST_ROOT}/acl-fail.err" \
    run_provision "$acl_fail_id" 'ACL失败' device-acl-fail-mqtt \
        "$TOKEN_ACL_FAIL" "$PASSWORD_STAGE" "${TEST_ROOT}/outputs/acl-fail.json"
assert_contains "${TEST_ROOT}/acl-fail.err" 'stage=PASSWORD_INSTALLED code=FILE_RENAME_ERROR'
rmdir "${SECURITY_ROOT}/devices/${acl_fail_name}"
run_provision "$acl_fail_id" 'ACL失败' device-acl-fail-mqtt \
    "$TOKEN_ACL_FAIL" "$PASSWORD_STAGE" "${TEST_ROOT}/outputs/acl-fail.json" \
    >"${TEST_ROOT}/acl-retry.out" 2>"${TEST_ROOT}/acl-retry.err"

TOKEN_WRONG_PID="${TEST_ROOT}/secrets/wrong-pid-token"
write_secret "$TOKEN_WRONG_PID" 'stage-http-token-wrong-pid'
real_pid=$(cat "$BROKER_PID_FILE")
printf '%s\n' "$$" >"$BROKER_PID_FILE"
expect_failure "${TEST_ROOT}/wrong-pid.out" "${TEST_ROOT}/wrong-pid.err" \
    run_provision device-wrong-pid '错误PID' device-wrong-pid-mqtt \
        "$TOKEN_WRONG_PID" "$PASSWORD_STAGE" "${TEST_ROOT}/outputs/wrong-pid.json"
assert_contains "${TEST_ROOT}/wrong-pid.err" 'stage=ACL_INSTALLED code=BROKER_PID_INVALID'
printf '%s\n' "$real_pid" >"$BROKER_PID_FILE"
run_provision device-wrong-pid '错误PID' device-wrong-pid-mqtt \
    "$TOKEN_WRONG_PID" "$PASSWORD_STAGE" "${TEST_ROOT}/outputs/wrong-pid.json" \
    >"${TEST_ROOT}/wrong-pid-retry.out" 2>"${TEST_ROOT}/wrong-pid-retry.err"

second_port=$((MQTT_TEST_PORT + 1))
second_config="${MOSQUITTO_ROOT}/second.conf"
second_pid_file="${MOSQUITTO_ROOT}/second.pid"
second_data="${MOSQUITTO_ROOT}/second-data"
mkdir -p "$second_data"
cat >"$second_config" <<EOF
user root
listener ${second_port} 127.0.0.1
allow_anonymous false
password_file ${SECURITY_ROOT}/password_file
acl_file ${SECURITY_ROOT}/acl_file
persistence true
persistence_location ${second_data}/
pid_file ${second_pid_file}
log_dest file ${MOSQUITTO_ROOT}/second.log
EOF
mosquitto -c "$second_config" >"${MOSQUITTO_ROOT}/second-process.log" 2>&1 &
second_broker_pid=$!
for _ in $(seq 1 30); do [[ -s $second_pid_file ]] && break; sleep 0.1; done
TOKEN_MULTIPLE_PID="${TEST_ROOT}/secrets/multiple-pid-token"
write_secret "$TOKEN_MULTIPLE_PID" 'stage-http-token-multiple-pid'
expect_failure "${TEST_ROOT}/multiple-pid.out" "${TEST_ROOT}/multiple-pid.err" \
    run_provision device-multiple-pid '多个PID' device-multiple-pid-mqtt \
        "$TOKEN_MULTIPLE_PID" "$PASSWORD_STAGE" "${TEST_ROOT}/outputs/multiple-pid.json"
assert_contains "${TEST_ROOT}/multiple-pid.err" 'stage=ACL_INSTALLED code=BROKER_PID_INVALID'
kill -TERM "$second_broker_pid"
wait "$second_broker_pid"
second_broker_pid=''
run_provision device-multiple-pid '多个PID' device-multiple-pid-mqtt \
    "$TOKEN_MULTIPLE_PID" "$PASSWORD_STAGE" "${TEST_ROOT}/outputs/multiple-pid.json" \
    >"${TEST_ROOT}/multiple-pid-retry.out" 2>"${TEST_ROOT}/multiple-pid-retry.err"

stop_broker
printf '%s\n' "$$" >"$BROKER_PID_FILE"
TOKEN_ZERO_PID="${TEST_ROOT}/secrets/zero-pid-token"
write_secret "$TOKEN_ZERO_PID" 'stage-http-token-zero-pid'
expect_failure "${TEST_ROOT}/zero-pid.out" "${TEST_ROOT}/zero-pid.err" \
    run_provision device-zero-pid '零个PID' device-zero-pid-mqtt \
        "$TOKEN_ZERO_PID" "$PASSWORD_STAGE" "${TEST_ROOT}/outputs/zero-pid.json"
assert_contains "${TEST_ROOT}/zero-pid.err" 'stage=ACL_INSTALLED code=BROKER_PID_INVALID'
start_broker
run_provision device-zero-pid '零个PID' device-zero-pid-mqtt \
    "$TOKEN_ZERO_PID" "$PASSWORD_STAGE" "${TEST_ROOT}/outputs/zero-pid.json" \
    >"${TEST_ROOT}/zero-pid-retry.out" 2>"${TEST_ROOT}/zero-pid-retry.err"

TOKEN_SUMMARY_FAIL="${TEST_ROOT}/secrets/summary-fail-token"
write_secret "$TOKEN_SUMMARY_FAIL" 'stage-http-token-summary-fail'
expect_failure "${TEST_ROOT}/summary-fail.out" "${TEST_ROOT}/summary-fail.err" \
    run_provision device-summary-fail '摘要失败' device-summary-fail-mqtt \
        "$TOKEN_SUMMARY_FAIL" "$PASSWORD_STAGE" /proc/1/task011-summary.json
assert_contains "${TEST_ROOT}/summary-fail.err" 'stage=BROKER_RELOADED code=SUMMARY_WRITE_ERROR'
run_provision device-summary-fail '摘要失败' device-summary-fail-mqtt \
    "$TOKEN_SUMMARY_FAIL" "$PASSWORD_STAGE" "${TEST_ROOT}/outputs/summary-retry.json" \
    >"${TEST_ROOT}/summary-retry.out" 2>"${TEST_ROOT}/summary-retry.err"

# Concurrent provisioning shares one lock and converges both devices.
write_secret "${TEST_ROOT}/secrets/concurrent-a-token" 'concurrent-a-http'
write_secret "${TEST_ROOT}/secrets/concurrent-a-password" 'concurrent-a-mqtt'
write_secret "${TEST_ROOT}/secrets/concurrent-b-token" 'concurrent-b-http'
write_secret "${TEST_ROOT}/secrets/concurrent-b-password" 'concurrent-b-mqtt'
run_provision device-concurrent-a '并发设备A' device-concurrent-a-mqtt \
    "${TEST_ROOT}/secrets/concurrent-a-token" \
    "${TEST_ROOT}/secrets/concurrent-a-password" \
    "${TEST_ROOT}/outputs/concurrent-a.json" \
    >"${TEST_ROOT}/concurrent-a.out" 2>"${TEST_ROOT}/concurrent-a.err" &
concurrent_a=$!
run_provision device-concurrent-b '并发设备B' device-concurrent-b-mqtt \
    "${TEST_ROOT}/secrets/concurrent-b-token" \
    "${TEST_ROOT}/secrets/concurrent-b-password" \
    "${TEST_ROOT}/outputs/concurrent-b.json" \
    >"${TEST_ROOT}/concurrent-b.out" 2>"${TEST_ROOT}/concurrent-b.err" &
concurrent_b=$!
wait "$concurrent_a" || fatal 'concurrent provision A failed'
wait "$concurrent_b" || fatal 'concurrent provision B failed'
concurrent_count=$(mysql_exec --execute="SELECT COUNT(*) FROM devices WHERE device_id IN ('device-concurrent-a','device-concurrent-b')")
[[ $concurrent_count == 2 ]] || fatal 'concurrent provisioning did not create both devices'

# Secret values never appear in operational captures or Broker logs.
for capture in "${TEST_ROOT}"/*.out "${TEST_ROOT}"/*.err "$BROKER_LOG"; do
    [[ -f $capture ]] || continue
    for secret in \
        task011-server-password task011-management-password \
        device-one-http-token device-one-mqtt-password \
        device-one-http-token-rotated device-one-mqtt-password-rotated \
        duplicate-user-http-token output-directory-http-token \
        stage-http-token stage-mqtt-password stage-http-token-acl \
        stage-http-token-wrong-pid stage-http-token-multiple-pid \
        stage-http-token-zero-pid stage-http-token-summary-fail \
        concurrent-a-http concurrent-a-mqtt concurrent-b-http concurrent-b-mqtt \
        task011-mysql-password; do
        assert_not_contains "$capture" "$secret"
    done
done

printf 'component.mosquitto_scripts: PASS\n'
