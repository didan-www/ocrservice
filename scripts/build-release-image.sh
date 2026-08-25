#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
readonly ROOT_DIR
readonly RELEASE_TAG='ocrservice:req-001-v1.0.0'
readonly EXPECTED_YOLO_SHA256='bfa426b74d4b619207cca55b296ce3603fb784755a205791af0d0dab4fcf6c04'
readonly EXPECTED_LPRNET_SHA256='c78e54070d0e2b8a6f8d548feb52b75321d5f464bcaadb679ec7ee31433cffa4'
readonly ONNXRUNTIME_URL=${OCRSERVICE_RELEASE_ONNXRUNTIME_URL:-https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-linux-x64-1.20.1.tgz}

fail() {
    printf 'build-release-image: %s\n' "$1" >&2
    exit 1
}

usage() {
    printf 'usage: %s --output /absolute/path/ocrservice-req-001-v1.0.0-linux-amd64.tar\n' "$0"
}

[[ $# -eq 2 && $1 == --output ]] || {
    usage >&2
    exit 2
}

for command_name in docker git sha256sum mktemp realpath ln; do
    command -v "$command_name" >/dev/null || fail "$command_name is required"
done

readonly -a RELEASE_INPUT_PATHS=(
    Dockerfile
    CMakeLists.txt
    .dockerignore
    cmake
    config
    deploy
    generated
    migrations
    models
    modules
    src
)

git -C "$ROOT_DIR" diff --quiet HEAD -- "${RELEASE_INPUT_PATHS[@]}" ||
    fail 'release image inputs contain tracked changes not present in HEAD'
untracked_release_inputs=()
while IFS= read -r -d '' untracked_path; do
    untracked_release_inputs+=("$untracked_path")
done < <(git -C "$ROOT_DIR" ls-files --others --exclude-standard -z -- "${RELEASE_INPUT_PATHS[@]}")
(( ${#untracked_release_inputs[@]} == 0 )) ||
    fail 'release image inputs contain untracked files not present in HEAD'

readonly REQUESTED_ARCHIVE_PATH=$2
[[ $REQUESTED_ARCHIVE_PATH == /* ]] || fail 'output path must be absolute'
[[ $REQUESTED_ARCHIVE_PATH != *[$'\001'-$'\037'$'\177']* ]] ||
    fail 'output path contains a control character'

requested_parent=$(dirname -- "$REQUESTED_ARCHIVE_PATH")
archive_name=$(basename -- "$REQUESTED_ARCHIVE_PATH")
[[ $archive_name != . && $archive_name != .. && $archive_name != */* ]] ||
    fail 'output filename is invalid'
archive_parent=$(realpath -e -- "$requested_parent") || fail 'output parent does not exist'
[[ -d $archive_parent ]] || fail 'output parent is not a directory'
readonly ARCHIVE_DIR=$archive_parent
readonly ARCHIVE_PATH="${ARCHIVE_DIR}/${archive_name}"

[[ ! -L $ARCHIVE_PATH ]] || fail 'output path must not be a symbolic link'
[[ ! -e $ARCHIVE_PATH ]] || fail 'output path already exists'
case $ARCHIVE_PATH in
    "$ROOT_DIR" | "$ROOT_DIR"/*) fail 'output archive must be outside the source repository' ;;
esac
[[ -w $ARCHIVE_DIR ]] || fail 'output parent is not writable'

docker version >/dev/null

iid_file=$(mktemp "${ARCHIVE_DIR}/.ocrservice-release-iid.XXXXXX")
archive_tmp=$(mktemp "${ARCHIVE_DIR}/.ocrservice-release-archive.XXXXXX")
cleanup() {
    rm -f -- "$iid_file" "$archive_tmp"
}
trap cleanup EXIT INT TERM

revision=$(git -C "$ROOT_DIR" rev-parse HEAD)
[[ $revision =~ ^[0-9a-f]{40}$ ]] || fail 'unable to resolve the source revision'

docker build \
    --platform linux/amd64 \
    --target runtime \
    --build-arg "ONNXRUNTIME_URL=${ONNXRUNTIME_URL}" \
    --iidfile "$iid_file" \
    --label "org.opencontainers.image.revision=${revision}" \
    --label 'org.opencontainers.image.version=req-001-v1.0.0' \
    --tag "$RELEASE_TAG" \
    "$ROOT_DIR"

image_id=$(<"$iid_file")
tag_image_id=$(docker image inspect --format '{{.Id}}' "$RELEASE_TAG")
[[ $image_id == "$tag_image_id" && $image_id == sha256:* ]] || fail 'tag does not resolve to the built immutable image ID'

image_os=$(docker image inspect --format '{{.Os}}' "$RELEASE_TAG")
image_arch=$(docker image inspect --format '{{.Architecture}}' "$RELEASE_TAG")
[[ $image_os == linux && $image_arch == amd64 ]] || fail 'release image is not linux/amd64'

model_sums=$(docker run --rm --entrypoint /usr/bin/sha256sum "$RELEASE_TAG" \
    /app/models/yolov8_plate.onnx /app/models/lprnet.onnx)
grep -Fqx "${EXPECTED_YOLO_SHA256}  /app/models/yolov8_plate.onnx" <<<"$model_sums" ||
    fail 'YOLOv8 model hash in the image is incorrect'
grep -Fqx "${EXPECTED_LPRNET_SHA256}  /app/models/lprnet.onnx" <<<"$model_sums" ||
    fail 'LPRNet model hash in the image is incorrect'

ldd_output=$(docker run --rm --entrypoint /usr/bin/ldd "$RELEASE_TAG" /app/ocrservice)
if grep -Fq 'not found' <<<"$ldd_output"; then
    fail 'release executable has an unresolved shared library'
fi

docker image save --output "$archive_tmp" "$RELEASE_TAG"
[[ -s $archive_tmp ]] || fail 'docker image archive is empty'
chmod 0644 "$archive_tmp"
if ! ln -- "$archive_tmp" "$ARCHIVE_PATH"; then
    fail 'output path was created before the archive could be published'
fi
rm -f -- "$archive_tmp"
archive_sha256=$(sha256sum "$ARCHIVE_PATH" | awk '{print $1}')

repo_digests=$(docker image inspect --format '{{join .RepoDigests ","}}' "$RELEASE_TAG")
[[ -n $repo_digests ]] || repo_digests='none-local-daemon'

printf 'releaseTag=%s\n' "$RELEASE_TAG"
printf 'sourceRevision=%s\n' "$revision"
printf 'imageId=%s\n' "$image_id"
printf 'repoDigest=%s\n' "$repo_digests"
printf 'platform=%s/%s\n' "$image_os" "$image_arch"
printf 'archive=%s\n' "$ARCHIVE_PATH"
printf 'archiveSha256=%s\n' "$archive_sha256"
printf 'yolov8Sha256=%s\n' "$EXPECTED_YOLO_SHA256"
printf 'lprnetSha256=%s\n' "$EXPECTED_LPRNET_SHA256"
