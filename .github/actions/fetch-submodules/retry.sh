# Runs a command up to four times, waiting 30, 60 and 120 s between the attempts, for the
# git hosts that fail now and then. Sourced by action.yml.
retry() {
  local wait=30 attempt
  for attempt in 1 2 3 4; do
    "$@" && return 0
    [ "$attempt" -lt 4 ] || break
    echo "::warning::'$*' failed (attempt $attempt of 4); retrying in $wait s"
    sleep "$wait"
    wait=$((wait * 2))
  done
  return 1
}
