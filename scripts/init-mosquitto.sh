#!/usr/bin/env bash

set -Eeuo pipefail
umask 077

readonly USERNAME_PATTERN='[A-Za-z0-9][A-Za-z0-9._-]{0,63}'

fail() {
    printf 'init-mosquitto: code=%s\n' "$1" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || fail "MISSING_DEPENDENCY"
}

require_environment() {
    local name=$1
    [[ -n ${!name:-} ]] || fail "MISSING_ENVIRONMENT"
}

valid_username() {
    [[ $1 =~ ^${USERNAME_PATTERN}$ ]]
}

write_expected_template() {
    local output=$1
    printf '%s\n' \
        'user {{MQTT_SERVER_USERNAME}}' \
        'topic write plate/management/recognition-events' \
        'topic write plate/devices/+/recognition-results' \
        '' \
        'user {{MQTT_MANAGEMENT_USERNAME}}' \
        'topic read plate/management/recognition-events' >"$output" ||
        fail "ACL_TEMPLATE_ERROR"
}

write_expected_base_acl() {
    local output=$1
    printf '%s\n' \
        "user ${MQTT_SERVER_USERNAME}" \
        'topic write plate/management/recognition-events' \
        'topic write plate/devices/+/recognition-results' \
        '' \
        "user ${MQTT_MANAGEMENT_USERNAME}" \
        'topic read plate/management/recognition-events' >"$output" ||
        fail "ACL_TEMPLATE_ERROR"
}

set_owner() {
    local path=$1
    if [[ $(id -u) -eq 0 ]]; then
        chown "${MOSQUITTO_RUNTIME_USER}:${MOSQUITTO_RUNTIME_GROUP}" "$path" ||
            fail "FILE_OWNERSHIP_ERROR"
    else
        [[ $(stat -c '%u:%g' "$path") == "$(id -u):$(id -g)" ]] ||
            fail "FILE_OWNERSHIP_ERROR"
    fi
}

secure_directory() {
    local path=$1
    mkdir -p "$path" || fail "DIRECTORY_CREATE_ERROR"
    chmod 0700 "$path" || fail "FILE_PERMISSION_ERROR"
    set_owner "$path"
}

sync_path() {
    sync -f "$1" >/dev/null 2>&1 || fail "FILE_SYNC_ERROR"
}

atomic_replace() {
    local temporary=$1
    local target=$2
    local mode=$3
    chmod "$mode" "$temporary" || fail "FILE_PERMISSION_ERROR"
    set_owner "$temporary"
    sync_path "$temporary"
    mv -fT -- "$temporary" "$target" || fail "FILE_RENAME_ERROR"
    sync_path "$(dirname -- "$target")"
}

update_password() {
    local file=$1
    local username=$2
    local password=$3
    local create=$4
    local error_file=$5
    local -a arguments=()
    [[ $create == true ]] && arguments+=(-c)
    if ! printf '%s\n%s\n' "$password" "$password" |
        "$MOSQUITTO_PASSWD_BIN" "${arguments[@]}" "$file" "$username" \
            >/dev/null 2>"$error_file"; then
        fail "PASSWORD_UPDATE_ERROR"
    fi
}

validate_and_merge_fragments() {
    local base_file=$1
    local output_file=$2
    local password_file=$3
    local -A usernames=()
    usernames["$MQTT_SERVER_USERNAME"]='static-server'
    usernames["$MQTT_MANAGEMENT_USERNAME"]='static-management'

    cp -- "$base_file" "$output_file" || fail "ACL_WRITE_ERROR"
    while IFS= read -r -d '' fragment; do
        local -a lines=()
        mapfile -t lines <"$fragment"
        [[ ${#lines[@]} -eq 2 ]] || fail "ACL_FRAGMENT_INVALID"
        [[ ${lines[0]} =~ ^user\ (${USERNAME_PATTERN})$ ]] ||
            fail "ACL_FRAGMENT_INVALID"
        local username=${BASH_REMATCH[1]}
        [[ ${lines[1]} =~ ^topic\ read\ plate/devices/(${USERNAME_PATTERN})/recognition-results$ ]] ||
            fail "ACL_FRAGMENT_INVALID"
        local device_id=${BASH_REMATCH[1]}
        local expected_name
        expected_name="$(printf '%s' "$device_id" | sha256sum | awk '{print $1}').acl"
        [[ $(basename -- "$fragment") == "$expected_name" ]] || fail "ACL_FRAGMENT_INVALID"
        [[ -z ${usernames[$username]+present} ]] || fail "MQTT_USERNAME_CONFLICT"
        usernames["$username"]=$device_id
        awk -F: -v expected="$username" \
            '$1 == expected { found = 1 } END { exit !found }' "$password_file" ||
            fail "PASSWORD_ACL_MISMATCH"
        chmod 0640 "$fragment" || fail "FILE_PERMISSION_ERROR"
        set_owner "$fragment"
        printf '\n' >>"$output_file"
        cat -- "$fragment" >>"$output_file" || fail "ACL_WRITE_ERROR"
    done < <(find "$DEVICE_ACL_DIRECTORY" -maxdepth 1 -type f -name '*.acl' -print0 | sort -z)
}

[[ $# -eq 0 ]] || fail "UNKNOWN_ARGUMENT"

for name in \
    MQTT_SERVER_USERNAME MQTT_SERVER_PASSWORD \
    MQTT_MANAGEMENT_USERNAME MQTT_MANAGEMENT_PASSWORD; do
    require_environment "$name"
done

valid_username "$MQTT_SERVER_USERNAME" || fail "MQTT_USERNAME_INVALID"
valid_username "$MQTT_MANAGEMENT_USERNAME" || fail "MQTT_USERNAME_INVALID"
[[ $MQTT_SERVER_USERNAME != "$MQTT_MANAGEMENT_USERNAME" ]] ||
    fail "MQTT_USERNAME_CONFLICT"
[[ $MQTT_SERVER_PASSWORD != *$'\n'* && $MQTT_SERVER_PASSWORD != *$'\r'* ]] ||
    fail "MQTT_PASSWORD_INVALID"
[[ $MQTT_MANAGEMENT_PASSWORD != *$'\n'* && $MQTT_MANAGEMENT_PASSWORD != *$'\r'* ]] ||
    fail "MQTT_PASSWORD_INVALID"

readonly SCRIPT_DIRECTORY="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
default_template="${SCRIPT_DIRECTORY}/../deploy/mosquitto/acl.base.template"
[[ -f $default_template ]] || default_template='/mosquitto/config/acl.base.template'

readonly SECURITY_ROOT="${MOSQUITTO_SECURITY_ROOT:-/mosquitto/data/security}"
readonly DEVICE_ACL_DIRECTORY="${SECURITY_ROOT}/devices"
readonly PASSWORD_FILE="${SECURITY_ROOT}/password_file"
readonly BASE_ACL_FILE="${SECURITY_ROOT}/acl.base"
readonly COMBINED_ACL_FILE="${SECURITY_ROOT}/acl_file"
readonly LOCK_FILE="${SECURITY_ROOT}/.security.lock"
readonly ACL_TEMPLATE="${MOSQUITTO_ACL_BASE_TEMPLATE:-$default_template}"
readonly MOSQUITTO_PASSWD_BIN="${MOSQUITTO_PASSWD_BIN:-mosquitto_passwd}"
readonly MOSQUITTO_RUNTIME_USER="${MOSQUITTO_RUNTIME_USER:-mosquitto}"
readonly MOSQUITTO_RUNTIME_GROUP="${MOSQUITTO_RUNTIME_GROUP:-mosquitto}"
readonly LOCK_TIMEOUT_SECONDS="${MOSQUITTO_LOCK_TIMEOUT_SECONDS:-60}"

[[ $LOCK_TIMEOUT_SECONDS =~ ^[1-9][0-9]*$ ]] || fail "LOCK_TIMEOUT_INVALID"
[[ -f $ACL_TEMPLATE ]] || fail "ACL_TEMPLATE_MISSING"
require_command flock
require_command cmp
require_command sha256sum
require_command sync
require_command "$MOSQUITTO_PASSWD_BIN"

secure_directory "$SECURITY_ROOT"
secure_directory "$DEVICE_ACL_DIRECTORY"
touch "$LOCK_FILE" || fail "LOCK_CREATE_ERROR"
chmod 0600 "$LOCK_FILE" || fail "FILE_PERMISSION_ERROR"
set_owner "$LOCK_FILE"
exec 9>>"$LOCK_FILE"
flock -w "$LOCK_TIMEOUT_SECONDS" 9 || fail "LOCK_TIMEOUT"

temporary_files=()
cleanup() {
    if ((${#temporary_files[@]} > 0)); then
        rm -f -- "${temporary_files[@]}" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

password_temporary="$(mktemp "${SECURITY_ROOT}/.password.XXXXXX")" || fail "TEMP_FILE_ERROR"
temporary_files+=("$password_temporary")
password_error="$(mktemp "${SECURITY_ROOT}/.password-error.XXXXXX")" || fail "TEMP_FILE_ERROR"
temporary_files+=("$password_error")
create_password=true
if [[ -s $PASSWORD_FILE ]]; then
    cp -- "$PASSWORD_FILE" "$password_temporary" || fail "PASSWORD_COPY_ERROR"
    create_password=false
fi
update_password "$password_temporary" "$MQTT_SERVER_USERNAME" "$MQTT_SERVER_PASSWORD" \
    "$create_password" "$password_error"
update_password "$password_temporary" "$MQTT_MANAGEMENT_USERNAME" \
    "$MQTT_MANAGEMENT_PASSWORD" false "$password_error"

base_temporary="$(mktemp "${SECURITY_ROOT}/.acl-base.XXXXXX")" || fail "TEMP_FILE_ERROR"
temporary_files+=("$base_temporary")
expected_template="$(mktemp "${SECURITY_ROOT}/.acl-template-expected.XXXXXX")" ||
    fail "TEMP_FILE_ERROR"
temporary_files+=("$expected_template")
expected_base="$(mktemp "${SECURITY_ROOT}/.acl-base-expected.XXXXXX")" ||
    fail "TEMP_FILE_ERROR"
temporary_files+=("$expected_base")
write_expected_template "$expected_template"
cmp -s "$ACL_TEMPLATE" "$expected_template" || fail "ACL_TEMPLATE_INVALID"
sed \
    -e "s/{{MQTT_SERVER_USERNAME}}/${MQTT_SERVER_USERNAME}/g" \
    -e "s/{{MQTT_MANAGEMENT_USERNAME}}/${MQTT_MANAGEMENT_USERNAME}/g" \
    "$ACL_TEMPLATE" >"$base_temporary" || fail "ACL_TEMPLATE_ERROR"
write_expected_base_acl "$expected_base"
cmp -s "$base_temporary" "$expected_base" || fail "ACL_TEMPLATE_INVALID"

combined_temporary="$(mktemp "${SECURITY_ROOT}/.acl-combined.XXXXXX")" ||
    fail "TEMP_FILE_ERROR"
temporary_files+=("$combined_temporary")
validate_and_merge_fragments "$base_temporary" "$combined_temporary" "$password_temporary"

atomic_replace "$password_temporary" "$PASSWORD_FILE" 0600
atomic_replace "$base_temporary" "$BASE_ACL_FILE" 0640
atomic_replace "$combined_temporary" "$COMBINED_ACL_FILE" 0640

printf 'init-mosquitto: initialized\n' >&2
