#!/bin/sh
# Decode MPCSH 1.7 / MPCECC 1.7 sources from gzip+base64 parts
set -e
decode_one() {
  name="$1"
  shift
  cat "$@" | base64 -d | gzip -dc > "$name"
  echo "OK $name ($(wc -c < "$name") bytes)"
}
decode_one MPCSH1.7.c MPCSH1.7.c.b64.part01 MPCSH1.7.c.b64.part02
decode_one MPCECC1.7.c MPCECC1.7.c.b64.part01 MPCECC1.7.c.b64.part02
