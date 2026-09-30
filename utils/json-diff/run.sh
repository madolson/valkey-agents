#!/bin/sh
# Differential test of JSON behaviour. See README.md.
#
#   run.sh <server-A> <module.so> <server-B> [options]   A (with module) vs B
#   run.sh --selfcheck <server> <module.so> [options]    prove the harness works
#
# Options are passed to jsondiff.py: --seed N, --count N, --with-merge,
# --b-module SO, --keep, --max-report N, --allowlist FILE, corpus files.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)

if [ "$1" = "--selfcheck" ]; then
    [ $# -ge 3 ] || { echo "usage: $0 --selfcheck <server> <module.so> [options]" >&2; exit 2; }
    server=$2 module=$3
    shift 3
    exec python3 "$DIR/jsondiff.py" selfcheck --server "$server" --module "$module" "$@"
fi

[ $# -ge 3 ] || { echo "usage: $0 <server-A> <module.so> <server-B> [options]" >&2; exit 2; }
a=$1 module=$2 b=$3
shift 3
exec python3 "$DIR/jsondiff.py" run --server-a "$a" --module "$module" --server-b "$b" "$@"
