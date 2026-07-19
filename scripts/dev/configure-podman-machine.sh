#!/usr/bin/env bash
set -euo pipefail

readonly MACHINE="podman-machine-default"
readonly TARGET_DISK_GIB=100

restart_on_exit() {
  if [[ "$(podman machine inspect --format '{{.State}}' "${MACHINE}" 2>/dev/null || true)" != "running" ]]; then
    podman machine start "${MACHINE}" >/dev/null 2>&1 || true
  fi
}
trap restart_on_exit EXIT
if [[ "$(podman machine inspect --format '{{.State}}' "${MACHINE}")" == "running" ]]; then
  podman machine stop "${MACHINE}"
fi

current_disk_gib="$(podman machine inspect --format '{{.Resources.DiskSize}}' "${MACHINE}")"
set_arguments=(--cpus 6 --memory 8192)
if (( current_disk_gib < TARGET_DISK_GIB )); then
  set_arguments+=(--disk-size "${TARGET_DISK_GIB}")
fi

podman machine set "${set_arguments[@]}" "${MACHINE}"
podman machine start "${MACHINE}"
for _ in $(seq 1 30); do
  if podman info >/dev/null 2>&1; then
    break
  fi
  sleep 1
done
podman machine inspect "${MACHINE}"
