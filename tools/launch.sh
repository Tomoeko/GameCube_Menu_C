#!/bin/sh
set -eu

launcher_directory=$(dirname "$0")
launcher_directory=$(cd "$launcher_directory" >/dev/null && pwd -P)
exec "$launcher_directory/gamecube-menu.app/Contents/MacOS/gamecube-menu" "$@"
