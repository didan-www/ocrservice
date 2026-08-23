#!/usr/bin/env bash
set -Eeuo pipefail
umask 077

readonly SECURITY_ROOT=${MOSQUITTO_SECURITY_ROOT:-/mosquitto/data/security}
[[ $# -eq 0 || "$1" == mosquitto || "$1" == /usr/sbin/mosquitto ]] || {
    printf 'mosquitto-entrypoint: unexpected command\n' >&2
    exit 1
}

mkdir -p /mosquitto/data "$SECURITY_ROOT" /mosquitto/log
chown -R mosquitto:mosquitto /mosquitto/data /mosquitto/log
chmod 0700 "$SECURITY_ROOT"
export MOSQUITTO_ACL_BASE_TEMPLATE=/mosquitto/config/acl.base.template
export MOSQUITTO_RUNTIME_USER=mosquitto
export MOSQUITTO_RUNTIME_GROUP=mosquitto
/usr/local/bin/init-mosquitto.sh

if [[ $# -eq 0 ]]; then
    set -- mosquitto -c /mosquitto/config/mosquitto.conf
elif [[ $# -eq 1 && "$1" == mosquitto ]]; then
    set -- "$@" -c /mosquitto/config/mosquitto.conf
fi
if command -v su-exec >/dev/null 2>&1; then
    exec su-exec mosquitto "$@"
fi
if command -v gosu >/dev/null 2>&1; then
    exec gosu mosquitto "$@"
fi
printf 'mosquitto-entrypoint: no privilege-drop helper\n' >&2
exit 1
