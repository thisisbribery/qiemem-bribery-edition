#!/bin/sh
# Host-side tests for segment linking and the envelope follower. Run from
# anywhere:
#   sh stages/test/links/run.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
for TEST in link_test follow_test; do
  OUT=${TMPDIR:-/tmp}/stages_$TEST
  g++ -std=gnu++11 -DTEST -O1 -Wall -Wno-unused-variable -I"$HERE/stub" -I"$REPO" -o "$OUT" \
    "$HERE/$TEST.cc" \
    "$REPO/stages/chain_state.cc" "$REPO/stages/segment_generator.cc" \
    "$REPO/stages/quantizer.cc" "$REPO/stages/braids_quantizer.cc" \
    "$REPO/tides2/ramp/ramp_extractor.cc" "$REPO/stages/resources.cc" \
    "$REPO/stmlib/dsp/units.cc" "$REPO/stmlib/utils/random.cc"
  echo "== $TEST"
  "$OUT"
done
