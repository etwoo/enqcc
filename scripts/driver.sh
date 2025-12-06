#!/usr/bin/env bash

set -euo pipefail

SKIP_LINK=0
DRIVER_MODE="--all"
INPUT_FILE=""
LIBRARIES=()

for option in "$@" ; do
	case "$option" in
		-c) SKIP_LINK=1 ;;
		--all) DRIVER_MODE="--all" ;;
		--codegen) DRIVER_MODE="--codegen" ;;
		--lex) DRIVER_MODE="--lex" ;;
		--parse) DRIVER_MODE="--parse" ;;
		--tacky) DRIVER_MODE="--tacky" ;;
		--validate) DRIVER_MODE="--validate" ;;
		-l*) LIBRARIES+=("$option") ;;
		-*) echo "Unimplemented option" && exit 1 ;;
		*) INPUT_FILE="$option" ;;
	esac
done

OUTPUT_FILE="${INPUT_FILE%.*}"
PREPROCESSED_FILE="$OUTPUT_FILE.i"
ASSEMBLY_FILE="$OUTPUT_FILE.s"

CC=$(which gcc)
NQCC=$(realpath "$0/../../build/enqcc")

$CC -E -P "$INPUT_FILE" -o "$PREPROCESSED_FILE"
$NQCC "$DRIVER_MODE" "$PREPROCESSED_FILE" "$ASSEMBLY_FILE"
if [ "$DRIVER_MODE" == "--all" ] ; then
	if [ "$SKIP_LINK" -eq 1 ] ; then
		$CC -c "$ASSEMBLY_FILE" -o "$OUTPUT_FILE.o" "${LIBRARIES[@]}"
	else
		$CC "$ASSEMBLY_FILE" -o "$OUTPUT_FILE" "${LIBRARIES[@]}"
	fi
fi

rm -f "$PREPROCESSED_FILE" "$ASSEMBLY_FILE"
