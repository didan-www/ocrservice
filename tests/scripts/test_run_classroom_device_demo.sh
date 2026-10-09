#!/usr/bin/env bash

set -Eeuo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
SOURCE_SCRIPT="${ROOT_DIR}/scripts/run-classroom-device-demo.sh"
WORK_DIR=$(mktemp -d)
trap 'rm -rf -- "$WORK_DIR"' EXIT

passed=0

fail() {
    printf 'test_run_classroom_device_demo: %s\n' "$1" >&2
    exit 1
}

write_health() {
    cat >"$1" <<'JSON'
{"success":true,"code":"OK","message":"ok","requestId":"req-test","data":{"status":"UP","model":"UP","mysql":"UP","mqtt":"UP","queueDepth":0,"queueCapacity":20}}
JSON
}

write_config() {
    local output=$1
    cat >"$output" <<'JSON'
{"deviceId":"device-classroom-01","http":{"baseUrl":"http://127.0.0.1:8080","bearerToken":"http-super-secret","uploadPath":"/api/v1/devices/device-classroom-01/recognitions"},"mqtt":{"host":"127.0.0.1","port":1883,"tls":false,"username":"mqtt-classroom-01","password":"mqtt-super-secret","clientId":"device-classroom-01","cleanSession":false,"qos":1,"resultTopic":"plate/devices/device-classroom-01/recognition-results"}}
JSON
    chmod 0600 "$output"
}

make_case() {
    local directory=$1
    mkdir -p "$directory/scripts" "$directory/demo-device" "$directory/fake-bin"
    chmod 0700 "$directory/demo-device"
    cp "$SOURCE_SCRIPT" "$directory/scripts/run-classroom-device-demo.sh"
    cp "$ROOT_DIR/scripts/provision-device.sh" "$directory/scripts/provision-device.sh"
    chmod 0755 "$directory/scripts/run-classroom-device-demo.sh"
    : >"$directory/docker-compose.yml"
    write_config "$directory/demo-device/device-classroom-01.json"
    printf '\xff\xd8\xff\xe0\x00\x10JFIF' >"$directory/scripts/vehicle.jpg"
    write_health "$directory/health.json"

    cat >"$directory/fake-bin/curl" <<'SH'
#!/usr/bin/env bash
set -Eeuo pipefail
output=''
while (($# > 0)); do
    case $1 in
        --output) output=$2; shift 2 ;;
        --write-out|--max-time) shift 2 ;;
        --silent|--show-error|--) shift ;;
        *) shift ;;
    esac
done
cp "$FAKE_HEALTH_FILE" "$output"
printf '%s' "${FAKE_HTTP_STATUS:-200}"
SH
    cat >"$directory/fake-bin/docker" <<'SH'
#!/usr/bin/env bash
set -Eeuo pipefail
printf '%s\n' "$*" >>"$FAKE_DOCKER_LOG"
if [[ $1 == cp ]]; then
    if [[ $2 == *:*.json ]]; then
        cp "$FAKE_CONFIG_TEMPLATE" "$3"
    fi
    exit 0
fi
if [[ $* == *' ps -q mqtt' ]]; then
    printf 'deadbeef\n'
elif [[ $* == *' ps -q app' ]]; then
    printf 'cafebabe\n'
elif [[ $* == *'printf %s "$MYSQL_PASSWORD"'* ]]; then
    printf 'ZGF0YWJhc2Utc2VjcmV0'
elif [[ $* == *'printf %s "$MQTT_PUBLIC_HOST"'* ]]; then
    printf 'MTI3LjAuMC4x'
elif [[ $* == *'command -v mysql'* ]]; then
    exit 1
elif [[ $* == *' apk add --no-cache '* ]]; then
    : >"$FAKE_INSTALL_MARKER"
elif [[ $* == *' bash -c '* && $* == *'--rotate'* ]]; then
    : >"$FAKE_PROVISION_MARKER"
fi
exit 0
SH
    cat >"$directory/simulator" <<'SH'
#!/usr/bin/env bash
set -Eeuo pipefail
printf '%s\n' "$@" >"$FAKE_SIMULATOR_LOG"
printf 'accepted=1 uniqueResults=1 duplicateResults=0 actionsExecuted=1\n'
SH
    chmod 0755 "$directory/fake-bin/curl" "$directory/fake-bin/docker" "$directory/simulator"
    : >"$directory/docker.log"
    : >"$directory/simulator.log"
}

run_case() {
    local directory=$1
    shift
    env PATH="$directory/fake-bin:$PATH" \
        FAKE_HEALTH_FILE="$directory/health.json" \
        FAKE_DOCKER_LOG="$directory/docker.log" \
        FAKE_CONFIG_TEMPLATE="$directory/config-template.json" \
        FAKE_PROVISION_MARKER="$directory/provisioned" \
        FAKE_INSTALL_MARKER="$directory/dependencies-installed" \
        FAKE_SIMULATOR_LOG="$directory/simulator.log" \
        EMBEDDED_SIMULATOR_BIN="$directory/simulator" \
        "$directory/scripts/run-classroom-device-demo.sh" "$@"
}

expect_failure() {
    local directory=$1
    local expected=$2
    shift 2
    if run_case "$directory" "$@" >"$directory/out" 2>"$directory/err"; then
        fail "expected failure containing: $expected"
    fi
    grep -Fq "$expected" "$directory/err" || fail "missing failure message: $expected"
    ((passed += 1))
}

case_dir="$WORK_DIR/reuse"
make_case "$case_dir"
run_case "$case_dir" >"$case_dir/out" 2>"$case_dir/err"
cat >"$case_dir/expected-args" <<EOF
--config
$case_dir/demo-device/device-classroom-01.json
--image
$case_dir/scripts/vehicle.jpg
--count
1
--interval-ms
1000
--timeout-seconds
120
EOF
diff -u "$case_dir/expected-args" "$case_dir/simulator.log"
[[ ! -s $case_dir/docker.log ]] || fail 'existing configuration unexpectedly invoked Docker'
grep -Fq 'accepted=1 uniqueResults=1 duplicateResults=0 actionsExecuted=1' "$case_dir/out"
! grep -Fq 'http-super-secret' "$case_dir/out" "$case_dir/err" "$case_dir/simulator.log"
! grep -Fq 'mqtt-super-secret' "$case_dir/out" "$case_dir/err" "$case_dir/simulator.log"
((passed += 1))

case_dir="$WORK_DIR/no-image"
make_case "$case_dir"
rm "$case_dir/scripts/vehicle.jpg"
expect_failure "$case_dir" 'place exactly one JPEG or PNG image'

case_dir="$WORK_DIR/multiple-images"
make_case "$case_dir"
printf '\x89PNG\r\n\x1a\n' >"$case_dir/scripts/second.PNG"
expect_failure "$case_dir" 'place exactly one JPEG or PNG image'

case_dir="$WORK_DIR/symlink-image"
make_case "$case_dir"
mv "$case_dir/scripts/vehicle.jpg" "$case_dir/vehicle.data"
ln -s "$case_dir/vehicle.data" "$case_dir/scripts/vehicle.jpg"
expect_failure "$case_dir" 'regular non-symlink file'

case_dir="$WORK_DIR/bad-signature"
make_case "$case_dir"
printf 'not-a-jpeg' >"$case_dir/scripts/vehicle.jpg"
expect_failure "$case_dir" 'does not match its JPEG signature'

case_dir="$WORK_DIR/config-mode"
make_case "$case_dir"
chmod 0644 "$case_dir/demo-device/device-classroom-01.json"
expect_failure "$case_dir" 'permissions must be exactly 0600'

case_dir="$WORK_DIR/degraded"
make_case "$case_dir"
sed -i 's/"mqtt":"UP"/"mqtt":"DOWN"/; s/"status":"UP"/"status":"DEGRADED"/' "$case_dir/health.json"
expect_failure "$case_dir" 'must all be UP'

case_dir="$WORK_DIR/provision"
make_case "$case_dir"
write_config "$case_dir/config-template.json"
rm -rf "$case_dir/demo-device"
printf '%s\n' 'teaching-only-test-env' >"$case_dir/.env"
chmod 0600 "$case_dir/.env"
run_case "$case_dir" >"$case_dir/out" 2>"$case_dir/err"
[[ -f $case_dir/provisioned ]] || fail 'empty state did not invoke provisioning'
[[ -f $case_dir/dependencies-installed ]] || fail 'missing dependencies were not installed'
grep -Fq 'device summary is missing; provisioning the classroom device' "$case_dir/out"
grep -Fq 'installing provisioning dependencies; this may take several minutes' "$case_dir/out"
grep -Fq 'classroom device provisioning completed' "$case_dir/out"
[[ $(stat -c '%a' "$case_dir/demo-device/device-classroom-01.json") == 600 ]] ||
    fail 'provisioned summary is not 0600'
[[ $(stat -c '%a' "$case_dir/demo-device") == 700 ]] || fail 'provisioned directory is not 0700'
grep -Fq -- '--project-directory' "$case_dir/docker.log"
grep -Fq -- "$case_dir/docker-compose.yml" "$case_dir/docker.log"
grep -Fq -- '--rotate' "$case_dir/docker.log"
grep -Fq -- 'mariadb-client mariadb-connector-c coreutils grep' "$case_dir/docker.log"
grep -Fq -- 'test -s /usr/lib/mariadb/plugin/caching_sha2_password.so' "$case_dir/docker.log"
! find "$case_dir/demo-device" -maxdepth 1 -type f -name '.*' | grep -q .
! grep -Fq 'http-super-secret' "$case_dir/out" "$case_dir/err" "$case_dir/docker.log" "$case_dir/simulator.log"
! grep -Fq 'mqtt-super-secret' "$case_dir/out" "$case_dir/err" "$case_dir/docker.log" "$case_dir/simulator.log"
((passed += 1))

git -C "$ROOT_DIR" check-ignore -q demo-device/device-classroom-01.json ||
    fail 'demo-device credentials are not ignored by Git'
git -C "$ROOT_DIR" check-ignore -q scripts/CLASSROOM.JPEG ||
    fail 'uppercase classroom JPEG is not ignored by Git'
git -C "$ROOT_DIR" check-ignore -q scripts/classroom.png ||
    fail 'classroom PNG is not ignored by Git'
((passed += 1))

printf 'classroom device demo script tests passed: %d\n' "$passed"
