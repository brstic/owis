#!/bin/sh
# OWIS - build vehicle data from your own licensed diagnostic installation (never shipped by OWIS).
cd "$(dirname "$0")"
PY=$(command -v python3 || command -v python)
if [ -z "$PY" ]; then echo "Python 3 is required (python.org). Install it, then run this again."; read x; exit 1; fi
echo "Drag your installation's Projects-V folder here and press Enter"
echo "  (e.g. .../PIDT/Projects-V ; older cars also .../PIDT_MIG/Projects-V):"
printf "  Projects-V folder: "; read SRC; SRC=$(echo "$SRC" | sed "s/^ *//; s/ *$//; s/^'//; s/'$//")
MIG=""; [ -d "$SRC/../PIDT_MIG/Projects-V" ] && MIG="$SRC/../PIDT_MIG/Projects-V"
"$PY" "$(dirname "$0")/builder/build_data.py" "$SRC" $MIG --out "$(dirname "$0")/data"
echo "Done. Run ./owis-cli scan"; read x
