#!/usr/bin/env bash
set -euo pipefail

readonly CONTAINER_NAME="journalseed-postgres"
readonly VOLUME_NAME="journalseed-postgres-data"
readonly IMAGE="docker.io/library/postgres:18.2-alpine"
readonly PORT="${JOURNALSEED_POSTGRES_PORT:-5432}"
readonly MACHINE="${JOURNALSEED_PODMAN_MACHINE:-podman-machine-default}"

podman_command() {
  if podman machine inspect "${MACHINE}" >/dev/null 2>&1; then
    local remote_command="podman"
    local argument
    for argument in "$@"; do
      printf -v remote_command '%s %q' "${remote_command}" "${argument}"
    done
    podman machine ssh "${MACHINE}" "${remote_command}"
  else
    podman "$@"
  fi
}

start_database() {
  if podman_command container exists "${CONTAINER_NAME}"; then
    podman_command start "${CONTAINER_NAME}" >/dev/null
  else
    if ! podman_command volume exists "${VOLUME_NAME}"; then
      podman_command volume create "${VOLUME_NAME}" >/dev/null
    fi
    podman_command run --detach \
      --name "${CONTAINER_NAME}" \
      --publish "127.0.0.1:${PORT}:5432" \
      --env POSTGRES_DB=journalseed \
      --env POSTGRES_USER=journalseed \
      --env POSTGRES_PASSWORD=journalseed \
      --health-cmd="pg_isready -U journalseed -d journalseed" \
      --health-interval=2s \
      --health-timeout=3s \
      --health-retries=20 \
      --volume "${VOLUME_NAME}:/var/lib/postgresql/data" \
      --volume "$(pwd)/infra/postgres/postgresql.conf:/etc/postgresql/postgresql.conf:ro" \
      "${IMAGE}" \
      -c config_file=/etc/postgresql/postgresql.conf >/dev/null
  fi

  for _ in $(seq 1 30); do
    if podman_command healthcheck run "${CONTAINER_NAME}" >/dev/null 2>&1; then
      printf 'PostgreSQL is ready at 127.0.0.1:%s\n' "${PORT}"
      return
    fi
    sleep 1
  done

  podman_command logs "${CONTAINER_NAME}"
  return 1
}

case "${1:-start}" in
  start) start_database ;;
  stop) podman_command stop "${CONTAINER_NAME}" ;;
  status) podman_command ps --filter "name=${CONTAINER_NAME}" ;;
  logs) podman_command logs --follow "${CONTAINER_NAME}" ;;
  reset)
    podman_command rm --force "${CONTAINER_NAME}" 2>/dev/null || true
    podman_command volume rm "${VOLUME_NAME}" 2>/dev/null || true
    start_database
    ;;
  *) printf 'Usage: %s {start|stop|status|logs|reset}\n' "$0" >&2; exit 2 ;;
esac
