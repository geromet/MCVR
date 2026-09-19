#!/usr/bin/env bash
# Turn "### Bn. Title" sections of HUMAN_BLOCKERS.md into GitHub issues.
#
#   tools/file_blockers.sh                       # dry run: print what would be filed
#   tools/file_blockers.sh --repo OWNER/REPO     # dry run against that repo (dedupe check only)
#   tools/file_blockers.sh --repo OWNER/REPO --create
#
# Creating requires an explicit --repo (never defaults to `origin`, which may be upstream).
# Idempotent: skips a blocker whose "[blocker Bn]" title prefix already exists (open or closed).
set -euo pipefail

file="$(dirname "$0")/../HUMAN_BLOCKERS.md"
repo="" create=0
while [ $# -gt 0 ]; do
    case "$1" in
        --repo) repo="${2:?--repo needs OWNER/REPO}"; shift 2 ;;
        --create) create=1; shift ;;
        --file) file="${2:?}"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done
[ "$create" = 1 ] && [ -z "$repo" ] && { echo "--create requires --repo" >&2; exit 2; }
[ -f "$file" ] || { echo "missing $file" >&2; exit 2; }

# Emit NUL-separated records: id, title, body.
parse() {
    awk '
        function flush() { if (id != "") printf "%s\0%s\0%s\0", id, title, body }
        /^### B[0-9]+\. / {
            flush()
            id = $2; sub(/\.$/, "", id)
            title = $0; sub(/^### B[0-9]+\. /, "", title)
            body = ""; next
        }
        /^#{1,3} / { flush(); id = ""; next }
        id != "" { body = body $0 "\n" }
        END { flush() }
    ' "$file"
}

n=0
while IFS= read -r -d '' id && IFS= read -r -d '' title && IFS= read -r -d '' body; do
    n=$((n + 1))
    full="[blocker $id] $title"
    body="$(printf '%s\n\n_Filed from HUMAN_BLOCKERS.md by tools/file_blockers.sh._\n' "$body")"
    if [ -n "$repo" ] && [ -n "$(gh issue list -R "$repo" --state all --search "\"[blocker $id]\" in:title" --json number -q '.[].number')" ]; then
        echo "skip   $full (already exists)"; continue
    fi
    if [ "$create" = 1 ]; then
        echo "create $full"; gh issue create -R "$repo" --title "$full" --body "$body"
    else
        echo "would create: $full"
    fi
done < <(parse)
[ "$n" -gt 0 ] || { echo "no '### Bn.' sections found in $file" >&2; exit 1; }
