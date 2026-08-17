#!/usr/bin/env bash

set -Eeuo pipefail
umask 077

skip() {
    printf 'component.mqtt_publisher.mosquitto: SKIP: %s\n' "$1"
    exit 77
}

fail() {
    printf 'component.mqtt_publisher.mosquitto: FAIL: %s\n' "$1" >&2
    exit 1
}

for command in mosquitto mosquitto_passwd python3; do
    command -v "$command" >/dev/null 2>&1 || skip "missing $command"
done

[[ -n ${OCRSERVICE_MQTT_TEST_BINARY:-} && -x ${OCRSERVICE_MQTT_TEST_BINARY} ]] ||
    fail 'test binary is missing'

test_root=$(mktemp -d /tmp/ocrservice-task012.XXXXXX)
[[ $test_root == /tmp/ocrservice-task012.* ]] || fail 'unsafe temporary directory'
cleanup() {
    local status=$?
    if [[ $test_root == /tmp/ocrservice-task012.* && -d $test_root ]]; then
        rm -rf -- "$test_root"
    fi
    exit "$status"
}
trap cleanup EXIT

port=$(python3 -c \
    'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')
[[ $port =~ ^[0-9]+$ ]] || fail 'could not allocate test port'

export OCRSERVICE_MQTT_SERVER_PASSWORD='task012-server-password'
export OCRSERVICE_MQTT_MANAGEMENT_PASSWORD='task012-management-password'
export OCRSERVICE_MQTT_DEVICE_PASSWORD='task012-device-password'

password_file="$test_root/password_file"
printf '%s:%s\n%s:%s\n%s:%s\n' \
    plate-server "$OCRSERVICE_MQTT_SERVER_PASSWORD" \
    management-client "$OCRSERVICE_MQTT_MANAGEMENT_PASSWORD" \
    device-real-001 "$OCRSERVICE_MQTT_DEVICE_PASSWORD" >"$password_file"
mosquitto_passwd -U "$password_file"
chmod 0600 "$password_file"

acl_file="$test_root/acl_file"
printf '%s\n' \
    'user plate-server' \
    'topic write plate/management/recognition-events' \
    'topic write plate/devices/+/recognition-results' \
    '' \
    'user management-client' \
    'topic read plate/management/recognition-events' \
    '' \
    'user device-real-001' \
    'topic read plate/devices/device-real-001/recognition-results' >"$acl_file"
chmod 0600 "$acl_file"

broker_config="$test_root/mosquitto.conf"
printf '%s\n' \
    "listener $port 127.0.0.1" \
    'protocol mqtt' \
    'allow_anonymous false' \
    "password_file $password_file" \
    "acl_file $acl_file" \
    'persistence false' \
    "pid_file $test_root/mosquitto.pid" \
    "log_dest file $test_root/mosquitto.log" \
    'log_type all' >"$broker_config"

mkdir -p "$test_root/logs/wrong" "$test_root/logs/publisher"
export OCRSERVICE_MQTT_REAL_TEST=1
export OCRSERVICE_MOSQUITTO_BIN
OCRSERVICE_MOSQUITTO_BIN=$(command -v mosquitto)
export OCRSERVICE_MQTT_BROKER_CONFIG=$broker_config
export OCRSERVICE_MQTT_TEST_PORT=$port
export OCRSERVICE_MQTT_LOG_ROOT="$test_root/logs"

"$OCRSERVICE_MQTT_TEST_BINARY" \
    --gtest_filter='MqttPublisherRealTest.*' \
    --gtest_color=no

for secret in \
    "$OCRSERVICE_MQTT_SERVER_PASSWORD" \
    "$OCRSERVICE_MQTT_MANAGEMENT_PASSWORD" \
    "$OCRSERVICE_MQTT_DEVICE_PASSWORD"; do
    if grep -R -F --exclude=password_file -- "$secret" "$test_root" >/dev/null 2>&1; then
        fail 'secret appeared in operational test output'
    fi
done

printf 'component.mqtt_publisher.mosquitto: PASS\n'
