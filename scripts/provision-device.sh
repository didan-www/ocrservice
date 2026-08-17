#!/usr/bin/env bash

set -Eeuo pipefail
umask 077

readonly USERNAME_PATTERN='[A-Za-z0-9][A-Za-z0-9._-]{0,63}'
stage='NONE'
mysql_active=false
temporary_files=()
declare -A ACL_USER_DEVICE=()
declare -A ACL_DEVICE_USER=()
declare -A SEEN_OPTIONS=()
declare -a MYSQL_RESULT_LINES=()

cleanup() {
    local status=$?
    if [[ $mysql_active == true ]]; then
        printf 'ROLLBACK;\nquit\n' 2>/dev/null >&"$MYSQL_INPUT_FD" || true
        wait "$MYSQL_CLIENT_PID_VALUE" >/dev/null 2>&1 || true
    fi
    if ((${#temporary_files[@]} > 0)); then
        rm -f -- "${temporary_files[@]}" >/dev/null 2>&1 || true
    fi
    exit "$status"
}
trap cleanup EXIT

fail() {
    printf 'provision-device: failed stage=%s code=%s\n' "$stage" "$1" >&2
    exit 1
}

complete_stage() {
    stage=$1
    printf 'provision-device: stage=%s\n' "$stage"
}

usage() {
    printf '%s\n' \
        'usage: provision-device.sh --device-id ID --device-name NAME' \
        '       --mqtt-username USER --http-token-file FILE' \
        '       --mqtt-password-file FILE --http-base-url URL' \
        '       --output FILE [--rotate]'
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || fail "MISSING_DEPENDENCY"
}

require_environment() {
    local name=$1
    [[ -n ${!name:-} ]] || fail "MISSING_ENVIRONMENT"
}

assign_once() {
    local variable=$1
    local option=$2
    local value=$3
    [[ -z ${SEEN_OPTIONS[$option]+present} ]] || fail "DUPLICATE_ARGUMENT"
    SEEN_OPTIONS["$option"]=true
    printf -v "$variable" '%s' "$value"
}

valid_identifier() {
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

read_secret_file() {
    local path=$1
    local output_variable=$2
    [[ -f $path && ! -L $path ]] || fail "SECRET_FILE_INVALID"
    local permissions
    permissions=$(stat -c '%a' "$path") || fail "SECRET_FILE_INVALID"
    local numeric_permissions=$((8#$permissions))
    (( (numeric_permissions & 077) == 0 )) || fail "SECRET_FILE_PERMISSIONS"

    local secret=''
    local extra=''
    local file_descriptor
    exec {file_descriptor}<"$path" || fail "SECRET_FILE_INVALID"
    if ! IFS= read -r secret <&"$file_descriptor"; then
        [[ -n $secret ]] || fail "SECRET_INVALID"
    fi
    if IFS= read -r extra <&"$file_descriptor" || [[ -n $extra ]]; then
        fail "SECRET_INVALID"
    fi
    exec {file_descriptor}<&-
    [[ ${#secret} -ge 1 && ${#secret} -le 256 ]] || fail "SECRET_INVALID"
    if printf '%s' "$secret" | LC_ALL=C grep -q '[^[:graph:]]'; then
        fail "SECRET_INVALID"
    fi
    printf -v "$output_variable" '%s' "$secret"
}

validate_device_name() {
    [[ -n $1 ]] || fail "DEVICE_NAME_INVALID"
    printf '%s' "$1" | iconv -f UTF-8 -t UTF-8 >/dev/null 2>&1 ||
        fail "DEVICE_NAME_INVALID"
    local count
    count=$(printf '%s' "$1" | LC_ALL=C.UTF-8 wc -m) || fail "DEVICE_NAME_INVALID"
    [[ $count =~ ^[0-9]+$ ]] || fail "DEVICE_NAME_INVALID"
    ((count >= 1 && count <= 100)) || fail "DEVICE_NAME_INVALID"
    if printf '%s' "$1" | LC_ALL=C.UTF-8 grep -q '[[:cntrl:]]'; then
        fail "DEVICE_NAME_INVALID"
    fi
}

to_hex() {
    printf '%s' "$1" | od -An -v -tx1 | tr -d ' \n'
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

sync_path() {
    sync -f "$1" >/dev/null 2>&1 || fail "FILE_SYNC_ERROR"
}

atomic_security_replace() {
    local temporary=$1
    local target=$2
    local mode=$3
    chmod "$mode" "$temporary" || fail "FILE_PERMISSION_ERROR"
    set_owner "$temporary"
    sync_path "$temporary"
    mv -fT -- "$temporary" "$target" || fail "FILE_RENAME_ERROR"
    sync_path "$(dirname -- "$target")"
}

password_has_user() {
    local username=$1
    local file=$2
    awk -F: -v expected="$username" '$1 == expected { found = 1 } END { exit !found }' "$file"
}

parse_fragment() {
    local fragment=$1
    local -a lines=()
    mapfile -t lines <"$fragment"
    [[ ${#lines[@]} -eq 2 ]] || fail "ACL_FRAGMENT_INVALID"
    [[ ${lines[0]} =~ ^user\ (${USERNAME_PATTERN})$ ]] || fail "ACL_FRAGMENT_INVALID"
    PARSED_USERNAME=${BASH_REMATCH[1]}
    [[ ${lines[1]} =~ ^topic\ read\ plate/devices/(${USERNAME_PATTERN})/recognition-results$ ]] ||
        fail "ACL_FRAGMENT_INVALID"
    PARSED_DEVICE_ID=${BASH_REMATCH[1]}
    local expected_name
    expected_name="$(printf '%s' "$PARSED_DEVICE_ID" | sha256sum | awk '{print $1}').acl"
    [[ $(basename -- "$fragment") == "$expected_name" ]] || fail "ACL_FRAGMENT_INVALID"
}

validate_existing_security_state() {
    [[ -f $PASSWORD_FILE && -f $BASE_ACL_FILE && -f $COMBINED_ACL_FILE ]] ||
        fail "MOSQUITTO_NOT_INITIALIZED"
    [[ $(stat -c '%a' "$PASSWORD_FILE") == 600 ]] || fail "FILE_PERMISSION_ERROR"
    [[ $(stat -c '%a' "$BASE_ACL_FILE") == 640 ]] || fail "FILE_PERMISSION_ERROR"
    [[ $(stat -c '%a' "$COMBINED_ACL_FILE") == 640 ]] || fail "FILE_PERMISSION_ERROR"
    password_has_user "$MQTT_SERVER_USERNAME" "$PASSWORD_FILE" ||
        fail "PASSWORD_ACL_MISMATCH"
    password_has_user "$MQTT_MANAGEMENT_USERNAME" "$PASSWORD_FILE" ||
        fail "PASSWORD_ACL_MISMATCH"

    local expected_base
    expected_base="$(mktemp "${SECURITY_ROOT}/.expected-base.XXXXXX")" || fail "TEMP_FILE_ERROR"
    temporary_files+=("$expected_base")
    local expected_template
    expected_template="$(mktemp "${SECURITY_ROOT}/.expected-template.XXXXXX")" ||
        fail "TEMP_FILE_ERROR"
    temporary_files+=("$expected_template")
    local rendered_template
    rendered_template="$(mktemp "${SECURITY_ROOT}/.rendered-template.XXXXXX")" ||
        fail "TEMP_FILE_ERROR"
    temporary_files+=("$rendered_template")
    write_expected_template "$expected_template"
    cmp -s "$ACL_TEMPLATE" "$expected_template" || fail "ACL_TEMPLATE_INVALID"
    sed \
        -e "s/{{MQTT_SERVER_USERNAME}}/${MQTT_SERVER_USERNAME}/g" \
        -e "s/{{MQTT_MANAGEMENT_USERNAME}}/${MQTT_MANAGEMENT_USERNAME}/g" \
        "$ACL_TEMPLATE" >"$rendered_template" || fail "ACL_TEMPLATE_ERROR"
    write_expected_base_acl "$expected_base"
    cmp -s "$rendered_template" "$expected_base" || fail "ACL_TEMPLATE_INVALID"
    cmp -s "$expected_base" "$BASE_ACL_FILE" || fail "STATIC_ACL_MISMATCH"

    ACL_USER_DEVICE=()
    ACL_DEVICE_USER=()
    ACL_USER_DEVICE["$MQTT_SERVER_USERNAME"]='static-server'
    ACL_USER_DEVICE["$MQTT_MANAGEMENT_USERNAME"]='static-management'
    while IFS= read -r -d '' fragment; do
        [[ $(stat -c '%a' "$fragment") == 640 ]] || fail "FILE_PERMISSION_ERROR"
        parse_fragment "$fragment"
        [[ -z ${ACL_USER_DEVICE[$PARSED_USERNAME]+present} ]] ||
            fail "MQTT_USERNAME_CONFLICT"
        [[ -z ${ACL_DEVICE_USER[$PARSED_DEVICE_ID]+present} ]] ||
            fail "ACL_FRAGMENT_INVALID"
        password_has_user "$PARSED_USERNAME" "$PASSWORD_FILE" ||
            fail "PASSWORD_ACL_MISMATCH"
        ACL_USER_DEVICE["$PARSED_USERNAME"]=$PARSED_DEVICE_ID
        ACL_DEVICE_USER["$PARSED_DEVICE_ID"]=$PARSED_USERNAME
    done < <(find "$DEVICE_ACL_DIRECTORY" -maxdepth 1 -type f -name '*.acl' -print0 | sort -z)
}

start_mysql() {
    MYSQL_ERROR_FILE="$(mktemp "${SECURITY_ROOT}/.mysql-error.XXXXXX")" ||
        fail "TEMP_FILE_ERROR"
    temporary_files+=("$MYSQL_ERROR_FILE")
    coproc MYSQL_CLIENT {
        MYSQL_PWD="$MYSQL_PASSWORD" "$MYSQL_BIN" \
            --protocol=TCP \
            --host="$MYSQL_HOST" \
            --port="$MYSQL_PORT" \
            --user="$MYSQL_USER" \
            --database="$MYSQL_DATABASE" \
            --connect-timeout=10 \
            --batch --skip-column-names --raw --silent --unbuffered \
            2>"$MYSQL_ERROR_FILE"
    }
    MYSQL_OUTPUT_FD=${MYSQL_CLIENT[0]}
    MYSQL_INPUT_FD=${MYSQL_CLIENT[1]}
    MYSQL_CLIENT_PID_VALUE=$MYSQL_CLIENT_PID
    mysql_active=true
}

mysql_send() {
    printf '%s\n' "$1" >&"$MYSQL_INPUT_FD" || fail "MYSQL_ERROR"
}

mysql_read_to_marker() {
    local marker=$1
    MYSQL_RESULT_LINES=()
    local line
    while IFS= read -r line <&"$MYSQL_OUTPUT_FD"; do
        if [[ $line == "$marker" ]]; then
            return 0
        fi
        MYSQL_RESULT_LINES+=("$line")
    done
    fail "MYSQL_ERROR"
}

finish_mysql() {
    mysql_send 'quit'
    if ! wait "$MYSQL_CLIENT_PID_VALUE"; then
        fail "MYSQL_ERROR"
    fi
    mysql_active=false
}

update_device_password() {
    local temporary=$1
    local error_file=$2
    if ! printf '%s\n%s\n' "$mqtt_password" "$mqtt_password" |
        "$MOSQUITTO_PASSWD_BIN" "$temporary" "$mqtt_username" \
            >/dev/null 2>"$error_file"; then
        fail "PASSWORD_UPDATE_ERROR"
    fi
}

build_combined_acl() {
    local new_fragment=$1
    local output=$2
    local target_name=$3
    cp -- "$BASE_ACL_FILE" "$output" || fail "ACL_WRITE_ERROR"
    local -a names=()
    while IFS= read -r -d '' fragment; do
        names+=("$(basename -- "$fragment")")
    done < <(find "$DEVICE_ACL_DIRECTORY" -maxdepth 1 -type f -name '*.acl' -print0)
    names+=("$target_name")
    mapfile -t names < <(printf '%s\n' "${names[@]}" | sort -u)
    local name
    for name in "${names[@]}"; do
        printf '\n' >>"$output"
        if [[ $name == "$target_name" ]]; then
            cat -- "$new_fragment" >>"$output" || fail "ACL_WRITE_ERROR"
        else
            cat -- "${DEVICE_ACL_DIRECTORY}/${name}" >>"$output" || fail "ACL_WRITE_ERROR"
        fi
    done
}

json_escape() {
    local value=$1
    value=${value//\\/\\\\}
    value=${value//\"/\\\"}
    printf '%s' "$value"
}

device_id=''
device_name=''
mqtt_username=''
http_token_file=''
mqtt_password_file=''
http_base_url=''
output_file=''
rotate=false

while (($# > 0)); do
    case $1 in
        --device-id|--device-name|--mqtt-username|--http-token-file|--mqtt-password-file|--http-base-url|--output)
            (($# >= 2)) || fail "MISSING_ARGUMENT_VALUE"
            case $1 in
                --device-id) assign_once device_id "$1" "$2" ;;
                --device-name) assign_once device_name "$1" "$2" ;;
                --mqtt-username) assign_once mqtt_username "$1" "$2" ;;
                --http-token-file) assign_once http_token_file "$1" "$2" ;;
                --mqtt-password-file) assign_once mqtt_password_file "$1" "$2" ;;
                --http-base-url) assign_once http_base_url "$1" "$2" ;;
                --output) assign_once output_file "$1" "$2" ;;
            esac
            shift 2
            ;;
        --rotate)
            [[ $rotate == false ]] || fail "DUPLICATE_ARGUMENT"
            rotate=true
            shift
            ;;
        --help)
            usage
            exit 0
            ;;
        *) fail "UNKNOWN_ARGUMENT" ;;
    esac
done

for value in device_id device_name mqtt_username http_token_file mqtt_password_file \
    http_base_url output_file; do
    [[ -n ${!value} ]] || fail "MISSING_ARGUMENT"
done
[[ $output_file != '-' ]] || fail "OUTPUT_PATH_INVALID"
if printf '%s' "$output_file" | LC_ALL=C grep -zq '[[:cntrl:]]'; then
    fail "OUTPUT_PATH_INVALID"
fi
[[ -d $(dirname -- "$output_file") ]] || fail "OUTPUT_PATH_INVALID"
[[ ! -d $output_file ]] || fail "OUTPUT_PATH_INVALID"
valid_identifier "$device_id" || fail "DEVICE_ID_INVALID"
valid_identifier "$mqtt_username" || fail "MQTT_USERNAME_INVALID"
validate_device_name "$device_name"
[[ $http_base_url == http://* ]] || fail "HTTP_BASE_URL_INVALID"
http_authority=${http_base_url#http://}
[[ $http_authority =~ ^[][A-Za-z0-9._:-]+$ ]] || fail "HTTP_BASE_URL_INVALID"
http_base_url=${http_base_url%/}

for name in MYSQL_HOST MYSQL_PORT MYSQL_DATABASE MYSQL_USER MYSQL_PASSWORD \
    MQTT_PUBLIC_HOST MQTT_PORT MQTT_SERVER_USERNAME MQTT_MANAGEMENT_USERNAME; do
    require_environment "$name"
done
[[ $MYSQL_PORT =~ ^[0-9]+$ ]] && ((MYSQL_PORT >= 1 && MYSQL_PORT <= 65535)) ||
    fail "MYSQL_PORT_INVALID"
[[ $MQTT_PORT =~ ^[0-9]+$ ]] && ((MQTT_PORT >= 1 && MQTT_PORT <= 65535)) ||
    fail "MQTT_PORT_INVALID"
valid_identifier "$MQTT_SERVER_USERNAME" || fail "MQTT_USERNAME_INVALID"
valid_identifier "$MQTT_MANAGEMENT_USERNAME" || fail "MQTT_USERNAME_INVALID"
[[ $MQTT_SERVER_USERNAME != "$MQTT_MANAGEMENT_USERNAME" ]] ||
    fail "MQTT_USERNAME_CONFLICT"
[[ $mqtt_username != "$MQTT_SERVER_USERNAME" &&
   $mqtt_username != "$MQTT_MANAGEMENT_USERNAME" ]] || fail "MQTT_USERNAME_CONFLICT"
[[ $MQTT_PUBLIC_HOST =~ ^[][A-Za-z0-9._:-]+$ ]] || fail "MQTT_PUBLIC_HOST_INVALID"

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
readonly PID_FILE="${MOSQUITTO_PID_FILE:-/mosquitto/data/mosquitto.pid}"
readonly MOSQUITTO_PASSWD_BIN="${MOSQUITTO_PASSWD_BIN:-mosquitto_passwd}"
readonly MYSQL_BIN="${MYSQL_BIN:-mysql}"
readonly MOSQUITTO_RUNTIME_USER="${MOSQUITTO_RUNTIME_USER:-mosquitto}"
readonly MOSQUITTO_RUNTIME_GROUP="${MOSQUITTO_RUNTIME_GROUP:-mosquitto}"
readonly LOCK_TIMEOUT_SECONDS="${MOSQUITTO_LOCK_TIMEOUT_SECONDS:-60}"

[[ $LOCK_TIMEOUT_SECONDS =~ ^[1-9][0-9]*$ ]] || fail "LOCK_TIMEOUT_INVALID"
[[ -d $SECURITY_ROOT && -d $DEVICE_ACL_DIRECTORY && -f $LOCK_FILE ]] ||
    fail "MOSQUITTO_NOT_INITIALIZED"
[[ -f $ACL_TEMPLATE ]] || fail "ACL_TEMPLATE_MISSING"
for command in awk cmp flock grep iconv od realpath sha256sum stat sync "$MYSQL_BIN" \
    "$MOSQUITTO_PASSWD_BIN"; do
    require_command "$command"
done

[[ ! -L $http_token_file && ! -L $mqtt_password_file ]] ||
    fail "SECRET_FILE_INVALID"
[[ ! -L $output_file ]] || fail "OUTPUT_PATH_INVALID"
http_token_canonical=$(realpath -e -- "$http_token_file") || fail "SECRET_FILE_INVALID"
mqtt_password_canonical=$(realpath -e -- "$mqtt_password_file") ||
    fail "SECRET_FILE_INVALID"
output_canonical=$(realpath -m -- "$output_file") || fail "OUTPUT_PATH_INVALID"
security_root_canonical=$(realpath -e -- "$SECURITY_ROOT") ||
    fail "MOSQUITTO_NOT_INITIALIZED"
pid_canonical=$(realpath -m -- "$PID_FILE") || fail "OUTPUT_PATH_INVALID"
acl_template_canonical=$(realpath -e -- "$ACL_TEMPLATE") || fail "ACL_TEMPLATE_MISSING"
if printf '%s' "$output_canonical" | LC_ALL=C grep -zq '[[:cntrl:]]'; then
    fail "OUTPUT_PATH_INVALID"
fi
case $output_canonical in
    "$security_root_canonical"|"$security_root_canonical"/*)
        fail "OUTPUT_PATH_INVALID"
        ;;
esac
for protected_path in \
    "$http_token_canonical" "$mqtt_password_canonical" \
    "$pid_canonical" "$acl_template_canonical"; do
    [[ $output_canonical != "$protected_path" ]] || fail "OUTPUT_PATH_INVALID"
    if [[ -e $output_file && -e $protected_path && $output_file -ef $protected_path ]]; then
        fail "OUTPUT_PATH_INVALID"
    fi
done
http_token_file=$http_token_canonical
mqtt_password_file=$mqtt_password_canonical
output_file=$output_canonical

http_token=''
mqtt_password=''
read_secret_file "$http_token_file" http_token
read_secret_file "$mqtt_password_file" mqtt_password

exec 9>>"$LOCK_FILE"
flock -w "$LOCK_TIMEOUT_SECONDS" 9 || fail "LOCK_TIMEOUT"
validate_existing_security_state

target_fragment_name="$(printf '%s' "$device_id" | sha256sum | awk '{print $1}').acl"
if [[ -n ${ACL_DEVICE_USER[$device_id]+present} &&
      ${ACL_DEVICE_USER[$device_id]} != "$mqtt_username" ]]; then
    fail "MQTT_USERNAME_CHANGE_UNSUPPORTED"
fi
if [[ -n ${ACL_USER_DEVICE[$mqtt_username]+present} &&
      ${ACL_USER_DEVICE[$mqtt_username]} != "$device_id" ]]; then
    fail "MQTT_USERNAME_CONFLICT"
fi

complete_stage VALIDATED

device_id_hex=$(to_hex "$device_id")
device_name_hex=$(to_hex "$device_name")
mqtt_username_hex=$(to_hex "$mqtt_username")
http_token_hash=$(printf '%s' "$http_token" | sha256sum | awk '{print $1}')
http_token_hash_hex=$(to_hex "$http_token_hash")
device_id_sql="CONVERT(0x${device_id_hex} USING utf8mb4) COLLATE utf8mb4_0900_as_cs"
device_name_sql="CONVERT(0x${device_name_hex} USING utf8mb4)"
mqtt_username_sql="CONVERT(0x${mqtt_username_hex} USING ascii) COLLATE ascii_bin"
http_token_hash_sql="CONVERT(0x${http_token_hash_hex} USING ascii) COLLATE ascii_bin"

start_mysql
mysql_send "SET NAMES utf8mb4; SET SESSION time_zone = '+00:00'; START TRANSACTION; SELECT CONCAT('__ROW__', CHAR(9), HEX(device_name), CHAR(9), http_token_hash, CHAR(9), HEX(mqtt_username), CHAR(9), enabled) FROM devices WHERE device_id = ${device_id_sql} FOR UPDATE; SELECT CONCAT('__CONFLICT__', CHAR(9), HEX(device_id)) FROM devices WHERE device_id <> ${device_id_sql} AND (http_token_hash = ${http_token_hash_sql} OR mqtt_username = ${mqtt_username_sql}) FOR UPDATE; SELECT '__STATE_DONE__';"
mysql_read_to_marker '__STATE_DONE__'

row_line=''
conflict_count=0
for line in "${MYSQL_RESULT_LINES[@]}"; do
    if [[ $line == __ROW__$'\t'* ]]; then
        [[ -z $row_line ]] || fail "MYSQL_STATE_INVALID"
        row_line=$line
    elif [[ $line == __CONFLICT__$'\t'* ]]; then
        ((conflict_count += 1))
    else
        fail "MYSQL_STATE_INVALID"
    fi
done
((conflict_count == 0)) || fail "DEVICE_CREDENTIAL_CONFLICT"

database_action='NONE'
if [[ -z $row_line ]]; then
    database_action='INSERT'
    mysql_send "INSERT INTO devices (device_id, device_name, http_token_hash, mqtt_username, enabled, created_at, updated_at) VALUES (${device_id_sql}, ${device_name_sql}, ${http_token_hash_sql}, ${mqtt_username_sql}, TRUE, UTC_TIMESTAMP(3), UTC_TIMESTAMP(3)); COMMIT; SELECT '__COMMIT_DONE__';"
else
    IFS=$'\t' read -r _ existing_name_hex existing_hash existing_mqtt_hex existing_enabled \
        <<<"$row_line"
    [[ ${existing_name_hex^^} == ${device_name_hex^^} ]] || fail "DEVICE_NAME_CONFLICT"
    [[ ${existing_mqtt_hex^^} == ${mqtt_username_hex^^} ]] || fail "MQTT_USERNAME_CHANGE_UNSUPPORTED"
    if [[ $existing_hash != "$http_token_hash" && $rotate == false ]]; then
        fail "HTTP_TOKEN_CONFLICT"
    fi
    if [[ $existing_hash != "$http_token_hash" ]]; then
        database_action='ROTATE'
        mysql_send "UPDATE devices SET http_token_hash = ${http_token_hash_sql}, enabled = TRUE, updated_at = UTC_TIMESTAMP(3) WHERE device_id = ${device_id_sql}; COMMIT; SELECT '__COMMIT_DONE__';"
    elif [[ $existing_enabled == 0 ]]; then
        database_action='ENABLE'
        mysql_send "UPDATE devices SET enabled = TRUE, updated_at = UTC_TIMESTAMP(3) WHERE device_id = ${device_id_sql}; COMMIT; SELECT '__COMMIT_DONE__';"
    else
        mysql_send "COMMIT; SELECT '__COMMIT_DONE__';"
    fi
fi
mysql_read_to_marker '__COMMIT_DONE__'
[[ ${#MYSQL_RESULT_LINES[@]} -eq 0 ]] || fail "MYSQL_STATE_INVALID"
finish_mysql
complete_stage MYSQL_COMMITTED

password_temporary="$(mktemp "${SECURITY_ROOT}/.password.XXXXXX")" || fail "TEMP_FILE_ERROR"
temporary_files+=("$password_temporary")
password_error="$(mktemp "${SECURITY_ROOT}/.password-error.XXXXXX")" || fail "TEMP_FILE_ERROR"
temporary_files+=("$password_error")
cp -- "$PASSWORD_FILE" "$password_temporary" || fail "PASSWORD_COPY_ERROR"
update_device_password "$password_temporary" "$password_error"
atomic_security_replace "$password_temporary" "$PASSWORD_FILE" 0600
complete_stage PASSWORD_INSTALLED

fragment_temporary="$(mktemp "${DEVICE_ACL_DIRECTORY}/.device-acl.XXXXXX")" ||
    fail "TEMP_FILE_ERROR"
temporary_files+=("$fragment_temporary")
printf 'user %s\ntopic read plate/devices/%s/recognition-results\n' \
    "$mqtt_username" "$device_id" >"$fragment_temporary"
combined_temporary="$(mktemp "${SECURITY_ROOT}/.acl-combined.XXXXXX")" ||
    fail "TEMP_FILE_ERROR"
temporary_files+=("$combined_temporary")
build_combined_acl "$fragment_temporary" "$combined_temporary" "$target_fragment_name"
atomic_security_replace "$fragment_temporary" \
    "${DEVICE_ACL_DIRECTORY}/${target_fragment_name}" 0640
atomic_security_replace "$combined_temporary" "$COMBINED_ACL_FILE" 0640
complete_stage ACL_INSTALLED

[[ -f $PID_FILE ]] || fail "BROKER_PID_INVALID"
broker_pid=''
if ! IFS= read -r broker_pid <"$PID_FILE"; then
    [[ -n $broker_pid ]] || fail "BROKER_PID_INVALID"
fi
[[ $broker_pid =~ ^[1-9][0-9]*$ ]] || fail "BROKER_PID_INVALID"
kill -0 "$broker_pid" >/dev/null 2>&1 || fail "BROKER_NOT_RUNNING"
mosquitto_pids=()
for process_comm in /proc/[0-9]*/comm; do
    [[ -r $process_comm ]] || continue
    IFS= read -r process_name <"$process_comm" || continue
    if [[ $process_name == mosquitto ]]; then
        process_path=${process_comm#/proc/}
        mosquitto_pids+=("${process_path%/comm}")
    fi
done
if [[ ${#mosquitto_pids[@]} -ne 1 || ${mosquitto_pids[0]:-} != "$broker_pid" ]]; then
    printf 'provision-device: brokerPidExpected=%s brokerProcessCount=%s brokerPidObserved=%s\n' \
        "$broker_pid" "${#mosquitto_pids[@]}" "${mosquitto_pids[0]:-none}" >&2
    fail "BROKER_PID_INVALID"
fi
kill -HUP "$broker_pid" >/dev/null 2>&1 || fail "BROKER_RELOAD_ERROR"
sleep 0.1
kill -0 "$broker_pid" >/dev/null 2>&1 || fail "BROKER_RELOAD_ERROR"
complete_stage BROKER_RELOADED

output_parent=$(dirname -- "$output_file")
output_temporary="$(mktemp "${output_parent}/.$(basename -- "$output_file").XXXXXX")" ||
    fail "SUMMARY_WRITE_ERROR"
temporary_files+=("$output_temporary")
escaped_device_id=$(json_escape "$device_id")
escaped_http_base_url=$(json_escape "$http_base_url")
escaped_http_token=$(json_escape "$http_token")
escaped_mqtt_host=$(json_escape "$MQTT_PUBLIC_HOST")
escaped_mqtt_username=$(json_escape "$mqtt_username")
escaped_mqtt_password=$(json_escape "$mqtt_password")
cat >"$output_temporary" <<EOF
{
  "deviceId": "${escaped_device_id}",
  "http": {
    "baseUrl": "${escaped_http_base_url}",
    "bearerToken": "${escaped_http_token}",
    "uploadPath": "/api/v1/devices/${escaped_device_id}/recognitions"
  },
  "mqtt": {
    "host": "${escaped_mqtt_host}",
    "port": ${MQTT_PORT},
    "tls": false,
    "username": "${escaped_mqtt_username}",
    "password": "${escaped_mqtt_password}",
    "clientId": "${escaped_device_id}",
    "cleanSession": false,
    "qos": 1,
    "resultTopic": "plate/devices/${escaped_device_id}/recognition-results"
  }
}
EOF
chmod 0600 "$output_temporary" || fail "SUMMARY_WRITE_ERROR"
sync_path "$output_temporary"
mv -fT -- "$output_temporary" "$output_file" || fail "SUMMARY_WRITE_ERROR"
sync_path "$output_parent"
complete_stage SUMMARY_WRITTEN
printf 'provision-device: summary=%s\n' "$output_file"
