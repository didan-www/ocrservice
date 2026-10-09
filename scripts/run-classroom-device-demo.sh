#!/usr/bin/env bash

set -Eeuo pipefail
umask 077

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly ROOT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
readonly CONFIG_DIR="${ROOT_DIR}/demo-device"
readonly CONFIG_FILE="${CONFIG_DIR}/device-classroom-01.json"
readonly ENV_FILE="${ROOT_DIR}/.env"
readonly MAX_IMAGE_BYTES=$((10 * 1024 * 1024))
readonly REMOTE_PREFIX="/tmp/ocrservice-classroom-device-$$"
declare -ar COMPOSE=(
    docker compose --project-directory "$ROOT_DIR"
    --env-file "$ENV_FILE" -f "${ROOT_DIR}/docker-compose.yml"
)

declare -a temporary_files=()
container_cleanup_needed=false
mysql_password_b64=''
mqtt_public_host_b64=''

fail() {
    printf 'classroom-device-demo: %s\n' "$1" >&2
    exit 1
}

cleanup() {
    local status=$?
    if [[ $container_cleanup_needed == true && -f $ENV_FILE ]]; then
        "${COMPOSE[@]}" exec -T -u root mqtt rm -f -- \
            "${REMOTE_PREFIX}.provision.sh" "${REMOTE_PREFIX}.http-token" \
            "${REMOTE_PREFIX}.mqtt-password" "${REMOTE_PREFIX}.env" \
            "${REMOTE_PREFIX}.json" >/dev/null 2>&1 || true
    fi
    if ((${#temporary_files[@]} > 0)); then
        rm -f -- "${temporary_files[@]}" >/dev/null 2>&1 || true
    fi
    mysql_password_b64=''
    mqtt_public_host_b64=''
    exit "$status"
}
trap cleanup EXIT

require_command() {
    command -v "$1" >/dev/null 2>&1 || fail "missing required command: $1"
}

validate_private_file() {
    [[ -f $1 && ! -L $1 ]] || fail "$2 must be a regular non-symlink file"
    [[ $(stat -c '%a' -- "$1") == 600 ]] || fail "$2 permissions must be exactly 0600"
}

validate_config_directory() {
    [[ -d $CONFIG_DIR && ! -L $CONFIG_DIR ]] || fail 'demo-device must be a regular directory'
    [[ $(stat -c '%a' -- "$CONFIG_DIR") == 700 ]] ||
        fail 'demo-device permissions must be exactly 0700'
    [[ $(realpath -e -- "$CONFIG_DIR") == "${ROOT_DIR}/demo-device" ]] ||
        fail 'demo-device resolves outside the repository root'
}

discover_image() {
    local candidate basename lowercase canonical size signature
    local -a matches=()
    while IFS= read -r -d '' candidate; do
        basename=${candidate##*/}
        lowercase=${basename,,}
        case $lowercase in *.jpg|*.jpeg|*.png) matches+=("$candidate") ;; esac
    done < <(find "$SCRIPT_DIR" -mindepth 1 -maxdepth 1 \
        \( -type f -o -type l \) -print0)
    ((${#matches[@]} == 1)) ||
        fail 'place exactly one JPEG or PNG image in the scripts directory'
    [[ -f ${matches[0]} && ! -L ${matches[0]} ]] ||
        fail 'the classroom image must be a regular non-symlink file'
    canonical=$(realpath -e -- "${matches[0]}") || fail 'cannot resolve the classroom image'
    [[ $(dirname -- "$canonical") == "$SCRIPT_DIR" ]] ||
        fail 'the classroom image resolves outside the scripts directory'
    size=$(stat -c '%s' -- "$canonical") || fail 'cannot read the classroom image size'
    [[ $size =~ ^[0-9]+$ ]] && ((size >= 1 && size <= MAX_IMAGE_BYTES)) ||
        fail 'the classroom image must be between 1 byte and 10 MiB'
    signature=$(od -An -N8 -tx1 -- "$canonical" | tr -d ' \n') ||
        fail 'cannot read the classroom image signature'
    lowercase=${canonical,,}
    case $lowercase in
        *.jpg|*.jpeg)
            [[ $signature == ffd8ff* ]] ||
                fail 'the classroom image extension does not match its JPEG signature'
            ;;
        *.png)
            [[ $signature == 89504e470d0a1a0a* ]] ||
                fail 'the classroom image extension does not match its PNG signature'
            ;;
    esac
    printf '%s\n' "$canonical"
}

config_health_url() {
    python3 - "$1" <<'PY'
import json
import re
import sys

def strict_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError()
        result[key] = value
    return result

try:
    with open(sys.argv[1], encoding='utf-8') as stream:
        value = json.load(stream, object_pairs_hook=strict_object)
    if set(value) != {'deviceId', 'http', 'mqtt'} or not isinstance(value['http'], dict):
        raise ValueError()
    if set(value['http']) != {'baseUrl', 'bearerToken', 'uploadPath'}:
        raise ValueError()
    base_url = value['http']['baseUrl']
    if not isinstance(base_url, str) or not re.fullmatch(r'http://[A-Za-z0-9._:\[\]-]+', base_url):
        raise ValueError()
    print(base_url + '/health')
except (OSError, UnicodeError, ValueError, TypeError, KeyError, json.JSONDecodeError):
    raise SystemExit('classroom-device-demo: device summary is invalid')
PY
}

check_health() {
    local response status size
    response=$(mktemp "${TMPDIR:-/tmp}/ocrservice-classroom-health.XXXXXX") ||
        fail 'cannot create health response file'
    temporary_files+=("$response")
    status=$(curl --silent --show-error --max-time 8 --output "$response" \
        --write-out '%{http_code}' -- "$1") || fail 'service health endpoint is unreachable'
    [[ $status == 200 ]] || fail 'service health endpoint did not return HTTP 200'
    size=$(stat -c '%s' -- "$response") || fail 'cannot inspect health response'
    ((size >= 1 && size <= 2 * 1024 * 1024)) || fail 'service health response size is invalid'
    python3 - "$response" <<'PY' || fail 'service, model, MySQL, and MQTT must all be UP'
import json
import sys

def strict_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError()
        result[key] = value
    return result

try:
    with open(sys.argv[1], encoding='utf-8') as stream:
        root = json.load(stream, object_pairs_hook=strict_object)
    data = root['data']
    if (set(root) != {'success', 'code', 'message', 'requestId', 'data'} or
            root['success'] is not True or root['code'] != 'OK' or
            not isinstance(root['message'], str) or not isinstance(root['requestId'], str) or
            not isinstance(data, dict) or
            set(data) != {'status', 'model', 'mysql', 'mqtt', 'queueDepth', 'queueCapacity'} or
            any(data[key] != 'UP' for key in ('status', 'model', 'mysql', 'mqtt')) or
            isinstance(data['queueDepth'], bool) or not isinstance(data['queueDepth'], int) or
            isinstance(data['queueCapacity'], bool) or not isinstance(data['queueCapacity'], int) or
            data['queueDepth'] < 0 or data['queueCapacity'] < 1 or
            data['queueDepth'] > data['queueCapacity']):
        raise ValueError()
except (OSError, UnicodeError, ValueError, TypeError, KeyError, json.JSONDecodeError):
    raise SystemExit(1)
PY
}

provision_device() {
    printf '%s\n' \
        'classroom-device-demo: device summary is missing; provisioning the classroom device'
    [[ -f $ENV_FILE && ! -L $ENV_FILE ]] || fail '.env must be a regular non-symlink file'
    [[ $(stat -c '%a' -- "$ENV_FILE") == 600 ]] || fail '.env permissions must be exactly 0600'
    [[ -f ${ROOT_DIR}/scripts/provision-device.sh &&
       ! -L ${ROOT_DIR}/scripts/provision-device.sh ]] ||
        fail 'scripts/provision-device.sh is unavailable'
    check_health 'http://127.0.0.1:8080/health'
    if [[ -e $CONFIG_DIR || -L $CONFIG_DIR ]]; then
        validate_config_directory
    else
        mkdir -- "$CONFIG_DIR" || fail 'cannot create demo-device'
        chmod 0700 -- "$CONFIG_DIR" || fail 'cannot secure demo-device'
    fi
    validate_config_directory

    local http_token_file mqtt_password_file provision_env mqtt_container app_container
    http_token_file=$(mktemp "${CONFIG_DIR}/.http-token.XXXXXX") || fail 'cannot create HTTP token file'
    mqtt_password_file=$(mktemp "${CONFIG_DIR}/.mqtt-password.XXXXXX") || fail 'cannot create MQTT password file'
    provision_env=$(mktemp "${CONFIG_DIR}/.provision-env.XXXXXX") || fail 'cannot create provision environment file'
    temporary_files+=("$http_token_file" "$mqtt_password_file" "$provision_env")
    openssl rand -hex 24 >"$http_token_file" || fail 'cannot generate HTTP token'
    openssl rand -hex 24 >"$mqtt_password_file" || fail 'cannot generate MQTT password'
    chmod 0600 -- "$http_token_file" "$mqtt_password_file" "$provision_env"

    mqtt_container=$("${COMPOSE[@]}" ps -q mqtt) || fail 'cannot locate the MQTT container'
    app_container=$("${COMPOSE[@]}" ps -q app) || fail 'cannot locate the application container'
    [[ $mqtt_container =~ ^[a-fA-F0-9]+$ && $app_container =~ ^[a-fA-F0-9]+$ ]] ||
        fail 'the application and MQTT Compose containers must be running'
    if ! "${COMPOSE[@]}" exec -T -u root mqtt sh -c \
        'command -v mysql >/dev/null && command -v realpath >/dev/null && command -v sha256sum >/dev/null && test -s /usr/lib/mariadb/plugin/caching_sha2_password.so'; then
        printf '%s\n' \
            'classroom-device-demo: installing provisioning dependencies; this may take several minutes'
        "${COMPOSE[@]}" exec -T -u root mqtt apk add --no-cache \
            mariadb-client mariadb-connector-c coreutils grep ||
            fail 'cannot install device provisioning dependencies'
    fi
    mysql_password_b64=$("${COMPOSE[@]}" exec -T app sh -c \
        'printf %s "$MYSQL_PASSWORD" | base64 -w0') || fail 'cannot read database configuration'
    mqtt_public_host_b64=$("${COMPOSE[@]}" exec -T app sh -c \
        'printf %s "$MQTT_PUBLIC_HOST" | base64 -w0') || fail 'cannot read MQTT host configuration'
    [[ $mysql_password_b64 =~ ^[A-Za-z0-9+/]+={0,2}$ &&
       $mqtt_public_host_b64 =~ ^[A-Za-z0-9+/]+={0,2}$ ]] ||
        fail 'the application provisioning configuration is invalid'
    printf '%s\n' 'MYSQL_HOST=mysql' 'MYSQL_PORT=3306' 'MYSQL_DATABASE=ocrservice' \
        'MYSQL_USER=ocrservice' "MYSQL_PASSWORD_B64=${mysql_password_b64}" \
        "MQTT_PUBLIC_HOST_B64=${mqtt_public_host_b64}" 'MQTT_PORT=1883' \
        'MQTT_SERVER_USERNAME=plate-server' 'MQTT_MANAGEMENT_USERNAME=management-client' \
        >"$provision_env" || fail 'cannot write provisioning environment'

    container_cleanup_needed=true
    docker cp "${ROOT_DIR}/scripts/provision-device.sh" "${mqtt_container}:${REMOTE_PREFIX}.provision.sh" >/dev/null || fail 'cannot stage provisioning script'
    docker cp "$http_token_file" "${mqtt_container}:${REMOTE_PREFIX}.http-token" >/dev/null || fail 'cannot stage HTTP token'
    docker cp "$mqtt_password_file" "${mqtt_container}:${REMOTE_PREFIX}.mqtt-password" >/dev/null || fail 'cannot stage MQTT password'
    docker cp "$provision_env" "${mqtt_container}:${REMOTE_PREFIX}.env" >/dev/null || fail 'cannot stage provisioning environment'
    "${COMPOSE[@]}" exec -T -u root mqtt chmod 0700 "${REMOTE_PREFIX}.provision.sh"
    "${COMPOSE[@]}" exec -T -u root mqtt chmod 0600 "${REMOTE_PREFIX}.http-token" \
        "${REMOTE_PREFIX}.mqtt-password" "${REMOTE_PREFIX}.env"
    "${COMPOSE[@]}" exec -T -u root mqtt bash -c '
        set -a; source "$1"; set +a
        export MYSQL_PASSWORD="$(printf %s "$MYSQL_PASSWORD_B64" | base64 -d)"
        export MQTT_PUBLIC_HOST="$(printf %s "$MQTT_PUBLIC_HOST_B64" | base64 -d)"
        unset MYSQL_PASSWORD_B64 MQTT_PUBLIC_HOST_B64
        shift
        exec "$@" --http-base-url "http://${MQTT_PUBLIC_HOST}:8080"
    ' provision "${REMOTE_PREFIX}.env" "${REMOTE_PREFIX}.provision.sh" \
        --device-id device-classroom-01 --device-name 'Classroom Gate 01' \
        --mqtt-username mqtt-classroom-01 \
        --http-token-file "${REMOTE_PREFIX}.http-token" \
        --mqtt-password-file "${REMOTE_PREFIX}.mqtt-password" \
        --output "${REMOTE_PREFIX}.json" --rotate || fail 'device provisioning failed'
    docker cp "${mqtt_container}:${REMOTE_PREFIX}.json" "$CONFIG_FILE" >/dev/null ||
        fail 'cannot retrieve the device summary'
    chmod 0600 -- "$CONFIG_FILE" || fail 'cannot secure the device summary'
    printf '%s\n' 'classroom-device-demo: classroom device provisioning completed'
}

locate_simulator() {
    local candidate
    if [[ -n ${EMBEDDED_SIMULATOR_BIN:-} ]]; then
        candidate=$EMBEDDED_SIMULATOR_BIN
        [[ $candidate == /* ]] || fail 'EMBEDDED_SIMULATOR_BIN must be an absolute path'
        [[ -f $candidate && ! -L $candidate && -x $candidate ]] ||
            fail 'EMBEDDED_SIMULATOR_BIN must be a regular executable file'
        realpath -e -- "$candidate"
        return 0
    fi
    for candidate in \
        "${ROOT_DIR}/build/teaching-release/tests/tests/simulators_embedded/embedded_device_simulator" \
        "${ROOT_DIR}/build-task024-main-release/tests/tests/simulators_embedded/embedded_device_simulator" \
        "${ROOT_DIR}/build-task024-dev-release/tests/tests/simulators_embedded/embedded_device_simulator"; do
        if [[ -e $candidate || -L $candidate ]]; then
            [[ -f $candidate && ! -L $candidate && -x $candidate ]] ||
                fail 'the embedded device simulator must be a regular executable file'
            realpath -e -- "$candidate"
            return 0
        fi
    done
    fail 'embedded_device_simulator was not found; build the teaching release first'
}

(($# == 0)) || fail 'this classroom launcher does not accept command-line arguments'
for command in curl docker find mktemp od openssl python3 realpath stat tr; do require_command "$command"; done

image=$(discover_image)
if [[ ! -e $CONFIG_FILE && ! -L $CONFIG_FILE ]]; then
    provision_device
else
    validate_config_directory
fi
validate_private_file "$CONFIG_FILE" 'device summary'
health_url=$(config_health_url "$CONFIG_FILE") || exit 1
check_health "$health_url"
simulator=$(locate_simulator)
printf 'classroom-device-demo: image validated; starting one device upload\n'
"$simulator" --config "$CONFIG_FILE" --image "$image" \
    --count 1 --interval-ms 1000 --timeout-seconds 120
printf '%s\n' \
    'classroom-device-demo: MQTT QoS 1 is not application-level guaranteed delivery;' \
    'classroom-device-demo: if the Broker misses the final publish, the gate remains closed.'
