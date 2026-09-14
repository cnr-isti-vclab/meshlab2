#!/bin/bash

set -euo pipefail

usage()
{
    cat <<'EOF'
Usage: tools/memory_probe_macos.sh [PID|process-name] [output-directory]

Interactively captures repeatable MeshLab memory checkpoints on macOS.
Before each checkpoint, open Help > Memory Info and click Copy JSON. Then enter
a short label such as baseline, one-mesh, two-mesh, or undo-cleared.
EOF
}

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This probe uses macOS footprint, vmmap, heap, and pbpaste." >&2
    exit 2
fi

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

target="${1:-MeshLab}"
output_dir="${2:-memory-probe-$(date -u +%Y%m%dT%H%M%SZ)}"

if [[ "$target" =~ ^[0-9]+$ ]]; then
    pid="$target"
else
    pid="$(pgrep -x "$target" | head -n 1 || true)"
fi

if [[ -z "$pid" ]] || ! kill -0 "$pid" 2>/dev/null; then
    echo "Cannot find a running process for '$target'." >&2
    exit 1
fi

mkdir -p "$output_dir"
cat >"$output_dir/session.txt" <<EOF
started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
pid=$pid
target=$target
host=$(hostname)
os=$(sw_vers -productVersion)
EOF

capture_command()
{
    local output="$1"
    shift
    if ! "$@" >"$output" 2>&1; then
        echo "Command failed: $*" >>"$output"
    fi
}

echo "Sampling PID $pid into $output_dir"
echo "For each phase: open Help > Memory Info, click Copy JSON, then enter a label."
echo "Press Return on an empty label to finish."

index=0
while true; do
    printf "Checkpoint label: "
    IFS= read -r label || break
    [[ -n "$label" ]] || break

    if ! kill -0 "$pid" 2>/dev/null; then
        echo "Process $pid is no longer running." >&2
        exit 1
    fi

    index=$((index + 1))
    safe_label="$(printf '%s' "$label" | tr -cs 'A-Za-z0-9._-' '_')"
    prefix="$(printf '%02d-%s' "$index" "$safe_label")"

    clipboard="$(pbpaste 2>/dev/null || true)"
    if printf '%s' "$clipboard" | grep -q 'org.meshlab.memory-report.v1'; then
        printf '%s\n' "$clipboard" >"$output_dir/$prefix-meshlab.json"
    else
        printf '%s\n' \
            "No MeshLab memory-report JSON was present on the clipboard." \
            >"$output_dir/$prefix-meshlab-missing.txt"
    fi

    capture_command "$output_dir/$prefix-footprint.txt" \
        footprint -p "$pid" -f bytes -w
    capture_command "$output_dir/$prefix-footprint.json.log" \
        footprint -p "$pid" -f bytes -w -j "$output_dir/$prefix-footprint.json"
    capture_command "$output_dir/$prefix-vmmap.txt" \
        vmmap -summary "$pid"
    capture_command "$output_dir/$prefix-heap.txt" \
        heap -s -H "$pid"
    capture_command "$output_dir/$prefix-ps.txt" \
        ps -o pid=,rss=,vsz=,etime=,command= -p "$pid"

    echo "Captured $prefix"
done

echo "Memory probe complete: $output_dir"
