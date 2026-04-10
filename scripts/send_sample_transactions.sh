#!/usr/bin/env bash

set -euo pipefail

# Returns the absolute directory that contains this script.
script_dir() {
  # Step 1: Resolve the script directory so relative paths are stable.
  cd "$(dirname "${BASH_SOURCE[0]}")" && pwd
}

# Publishes the provided transaction fixture file into the configured Kafka topic.
publish_transactions() {
  local root_dir="$1"
  local input_file="$2"
  local topic="$3"

  # Step 1: Verify the fixture file exists before invoking Docker.
  if [[ ! -f "${input_file}" ]]; then
    echo "Input file not found: ${input_file}" >&2
    return 1
  fi

  # Step 2: Pipe the fixture lines into Kafka inside the running container.
  docker compose -f "${root_dir}/docker/docker-compose.yml" exec -T kafka \
    kafka-console-producer \
      --bootstrap-server localhost:9092 \
      --topic "${topic}" < "${input_file}"
}

# Starts the script with either the provided file or the default sample file.
main() {
  # Step 1: Resolve project-relative paths.
  local root_dir
  root_dir="$(cd "$(script_dir)/.." && pwd)"

  # Step 2: Choose the input file and topic, allowing overrides from arguments.
  local input_file="${1:-${root_dir}/configs/sample_transactions.jsonl}"
  local topic="${2:-trading-transactions}"

  # Step 3: Print the operation so the caller can see what will be sent.
  echo "Publishing transactions from ${input_file} to topic ${topic}"

  # Step 4: Publish the fixture lines into Kafka.
  publish_transactions "${root_dir}" "${input_file}" "${topic}"

  # Step 5: Report success once the producer exits cleanly.
  echo "Finished publishing sample transactions"
}

# Executes the script entrypoint.
main "$@"
