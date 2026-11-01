// Host-side tests for the envelope follower segment. Drives the real
// ChainState and SegmentGenerator code with simulated buttons and audio.
// Run with: sh stages/test/links/run.sh

#include <cmath>
#include <cstdio>
#include <cstring>
#include "stages/chain_state.h"
#include "stages/settings.h"
#include "stages/drivers/serial_link.h"
using namespace stages;
using namespace stmlib;

ChainState cs; Settings st; SegmentGenerator sg[kNumChannels];
SerialLink l, r; IOBuffer::Block block; SegmentGenerator::Output out[kBlockSize];
uint8_t pressed = 0; int fails = 0;
const float kBlockRate = kSampleRate / kBlockSize;  // 3906.25 Hz

// Audio into segment `ch`: sine of `amp` (CV units, 1.0 = 8 V) at `freq`,
// plus a DC offset.
int ch = 1; float amp = 0.0f, freq = 1000.0f, dc = 0.0f; bool gate_high = false;
long n = 0;

// One block: chain state, then segment `ch`'s generator, like stages.cc.
void run_block() {
  static GateFlags no_gate[kBlockSize] = {0}, high[kBlockSize];
  for (size_t j = 0; j < kBlockSize; ++j) high[j] = GATE_FLAG_HIGH;
  block.cv_raw[ch] = dc + amp * sinf(2.0f * 3.14159265f * freq * n / kBlockRate);
  block.input_patched[ch] = gate_high;
  ++n;
  cs.set_local_switch_pressed(pressed);
  cs.Update(block, &st, sg, out);
  if (cs.follower(ch)) sg[ch].set_follow_input(block.cv_raw[ch]);
  sg[ch].Process(gate_high ? high : no_gate, out, kBlockSize);
}
void run_ms(float ms) { for (int k = 0; k < ms * kBlockRate / 1000.0f; ++k) run_block(); }
float value() { return out[kBlockSize - 1].value; }
// Peak and minimum output over `ms`.
float peak(float ms, float* lo = NULL) {
  float hi = -10, low = 10;
  for (int k = 0; k < ms * kBlockRate / 1000.0f; ++k) {
    run_block();
    for (size_t j = 0; j < kBlockSize; ++j) { if (out[j].value > hi) hi = out[j].value; if (out[j].value < low) low = out[j].value; }
  }
  if (lo) *lo = low;
  return hi;
}
// Buttons are polled once every 4 blocks (about 1 ms).
void tick_ms(int ms) { for (int k = 0; k < ms * 4; ++k) run_block(); }
void check(bool c, const char* what) { printf("%s %s\n", c ? "PASS" : "FAIL", what); if (!c) ++fails; }
uint16_t& cfg(int i) { return st.mutable_state()->segment_configuration[i]; }
void press(int b) { pressed |= 1 << b; } void release(int b) { pressed &= ~(1 << b); }
void tap(int b) { press(b); tick_ms(100); release(b); tick_ms(20); }
uint8_t links() { uint8_t m = 0; for (size_t i = 0; i < 6; ++i) if (cfg(i) & kLinkBit) m |= 1 << i; return m; }

int main() {
  memset(&block, 0, sizeof(block));
  for (size_t i = 0; i < kNumChannels; ++i) {
    sg[i].Init();
    block.slider[i] = block.cv_slider[i] = 0.4f; block.pot[i] = 0.0f;
  }
  st.mutable_state()->multimode = MULTI_MODE_STAGES;
  cs.Init(&l, &r, st);
  tick_ms(2100);

  // 1. Stages mode tap cycle: ramp, step, hold, follow, ramp.
  tap(1); tap(1); tap(1);
  check((cfg(1) & kFollowBit) && (cfg(1) & 3) == 0, "third tap after hold makes segment 2 a follower");
  check(cs.follower(1) && !cs.follower(0), "follower() reports segment 2 only");
  tap(1);
  check(!(cfg(1) & kFollowBit) && (cfg(1) & 3) == 0 && !cs.follower(1), "next tap goes back to ramp");

  // 2. Links next to a follower are ignored but kept.
  press(0); tick_ms(200); press(3); tick_ms(80); release(3); tick_ms(20); release(0); tick_ms(50);
  check(links() == 0x07 && cs.linked(0) && cs.linked(3), "1-4 linked");
  tap(1); tap(1); tap(1);
  check(cs.follower(1), "segment 2 back to follower");
  check(links() == 0x07, "stored links kept");
  check(!cs.linked(0) && !cs.linked(1) && cs.linked(2) && cs.linked(3), "group splits around the follower: 1 alone, 3-4 linked");
  check(sg[0].num_segments() == 1 && sg[2].num_segments() == 2, "generators follow the split");

  // 3. Hold + tap across a follower does nothing.
  uint8_t flash = cs.link_flash_id();
  press(0); tick_ms(200); press(2); tick_ms(80); release(2); tick_ms(20); release(0); tick_ms(50);
  check(cs.link_flash_id() == flash && links() == 0x07, "hold 1 + tap 3 over a follower: no link, no flash");

  // 4. Long press on a follower does not set a loop.
  press(1); tick_ms(800); release(1); tick_ms(20);
  check(!(cfg(1) & 4) && cs.follower(1), "0.8 s press on a follower: no loop");

  // 5. Level: 1.6 V peak sine (0.2) reads 0.32 at x1 gain, flat across
  //    the audio range. Attack instant, release mid.
  block.pot[1] = 0.0f; block.slider[1] = 0.5f;
  const float freqs[] = { 50, 200, 1000, 3000, 5000 };
  bool flat = true;
  for (int k = 0; k < 5; ++k) {
    amp = 0.2f; freq = freqs[k]; run_ms(300);
    float lo, hi = peak(200, &lo);
    printf("     %5.0f Hz: out %.3f..%.3f (expect about 0.32)\n", freq, lo, hi);
    flat = flat && hi > 0.27f && hi < 0.35f;
  }
  check(flat, "level tracks 50 Hz to 5 kHz within 15%");

  // 6. Gain range bits: x4 (range 2) clips at 1.0, x2 doubles.
  freq = 1000; cfg(1) |= 0x0100; run_ms(300);
  float x2 = peak(100);
  cfg(1) = (cfg(1) & ~0x0300) | 0x0200; run_ms(300);
  float x4 = peak(100);
  printf("     x2 %.3f, x4 %.3f\n", x2, x4);
  check(x2 > 0.55f && x2 < 0.68f && x4 > 0.99f && x4 <= 1.0f, "gain x2 doubles, x4 clips at full scale");
  cfg(1) &= ~0x0300; run_ms(300);

  // 7. Release: slider down = fast, up = slow.
  block.slider[1] = 0.0f; run_ms(200); amp = 0; run_ms(30);
  float fast = value();
  amp = 0.2f; block.slider[1] = 1.0f; run_ms(500); amp = 0; run_ms(30);
  float slow = value();
  printf("     30 ms after the audio stops: release min %.3f, max %.3f\n", fast, slow);
  check(fast < 0.01f && slow > 0.28f, "slider sets release time");

  // 8. Attack: pot up = slow rise, pot down = instant.
  block.slider[1] = 0.0f; run_ms(200);
  block.pot[1] = 1.0f; amp = 0.2f; run_ms(20);
  float slow_rise = value();
  amp = 0; run_ms(200); block.pot[1] = 0.0f; amp = 0.2f; run_ms(5);
  float fast_rise = peak(2);
  printf("     rise: pot max %.3f after 20 ms, pot min %.3f after 7 ms\n", slow_rise, fast_rise);
  check(slow_rise < 0.1f && fast_rise > 0.27f, "pot sets attack time");

  // 9. Gate freezes the envelope; the audio stopping doesn't move it.
  block.slider[1] = 0.3f; run_ms(200);
  float before = value();
  gate_high = true; run_ms(10); amp = 0; run_ms(300);
  float frozen = value();
  gate_high = false; run_ms(300);
  printf("     freeze: %.3f before, %.3f after 300 ms silent with gate high, %.3f released\n", before, frozen, value());
  check(cs.follower(1) && fabsf(frozen - before) < 0.05f && value() < 0.02f, "gate high freezes, gate low lets go");

  // 10. Invert (bipolar bit): negative envelope, quiet = 0, loud = -0.32.
  cfg(1) |= 0x08; run_ms(300);
  float quiet = value();
  amp = 0.2f; run_ms(300);
  float lo; peak(100, &lo);
  printf("     invert: quiet %.3f, loud %.3f\n", quiet, lo);
  check(fabsf(quiet) < 0.02f && lo < -0.27f && lo > -0.35f, "bipolar bit gives a negative envelope");
  cfg(1) |= 0x0300; run_ms(300); peak(100, &lo);
  printf("     invert at x8: %.3f\n", lo);
  check(lo < -0.99f && lo >= -1.0f, "inverted output reaches -8 V and clips there");
  cfg(1) &= ~0x0300; amp = 0;
  cfg(1) &= ~0x08; run_ms(300);

  // 11. DC offset on the input is blocked.
  amp = 0; dc = 0.5f; run_ms(1000);
  float dc_out = value();
  printf("     4 V DC after 1 s: %.3f\n", dc_out);
  check(dc_out < 0.02f, "DC input settles to zero");
  dc = 0;

  // 12. Advanced mode: ramp, step, hold, random, follow.
  st.mutable_state()->multimode = MULTI_MODE_STAGES_ADVANCED;
  cs.start_reinit(); tick_ms(2700);
  check(cs.follower(1), "follower survives a mode change");
  tap(1); tap(1); tap(1); tap(1);
  check((cfg(1) & 3) == 3 && !(cfg(1) & kFollowBit), "advanced mode: fourth tap from follow is random");
  tap(1);
  check(cs.follower(1), "advanced mode: random -> follow");

  // 13. Outside link mode (Ouroboros here) nothing is a follower.
  st.mutable_state()->multimode = MULTI_MODE_OUROBOROS;
  cs.start_reinit(); tick_ms(600);
  check(!cs.follower(1) && (cfg(1) & kFollowBit), "ouroboros: follower off, bit kept");

  printf("\n%d failure(s)\n", fails);
  return fails != 0;
}
