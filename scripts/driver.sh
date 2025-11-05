#!/usr/bin/env sh

set -euxo pipefail

DRIVER_MODE=""
INPUT_FILE=""

if [ $# == 1 ] ; then
	INPUT_FILE="$1"
elif [ $# == 2 ] ; then
	DRIVER_MODE="$1"
	INPUT_FILE="$2"
fi

OUTPUT_FILE="${INPUT_FILE%%.*}"
PREPROCESSED_FILE="$OUTPUT_FILE.i"
ASSEMBLY_FILE="$OUTPUT_FILE.s"

CC=$(which gcc)
NQCC=$(realpath "$0/../../build/enqcc")

$CC -E -P "$INPUT_FILE" -o "$PREPROCESSED_FILE"
$NQCC "$DRIVER_MODE" "$PREPROCESSED_FILE" "$ASSEMBLY_FILE"
if [ -z "$DRIVER_MODE" ] ; then
	$CC "$ASSEMBLY_FILE" -o "$OUTPUT_FILE"
fi

rm -f "$PREPROCESSED_FILE" "$ASSEMBLY_FILE"
