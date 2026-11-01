// Host-side tests for segment linking on the qiemem firmware.
// Simulates button presses and gate patching against the real ChainState
// and SegmentGenerator code. Run with: sh stages/test/links/run.sh

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
uint8_t suspend = 0;  // 1 = the UI is using the buttons (prop/mode change)

void tick_ms(int ms) {  // one PollSwitches per 4 Update calls (~1 ms)
  for (int k = 0; k < ms * 4; ++k) {
    cs.set_local_switch_pressed(suspend ? 0xff : pressed);
    cs.Update(block, &st, sg, out);
  }
}
void check(bool c, const char* what) { printf("%s %s\n", c ? "PASS" : "FAIL", what); if (!c) ++fails; }
uint16_t cfg(int i) { return st.state().segment_configuration[i]; }
uint8_t loops() { uint8_t m = 0; for (size_t i = 0; i < 6; ++i) if (cfg(i) & 4) m |= 1 << i; return m; }
uint8_t links() { uint8_t m = 0; for (size_t i = 0; i < 6; ++i) if (cfg(i) & kLinkBit) m |= 1 << i; return m; }
void press(int b) { pressed |= 1 << b; } void release(int b) { pressed &= ~(1 << b); }
void reset_all() {
  for (size_t i = 0; i < 6; ++i) st.mutable_state()->segment_configuration[i] = 0;
  for (size_t i = 0; i < 6; ++i) { block.cv_slider[i] = 0.4f; block.slider[i] = 0.4f; block.cv[i] = 0.0f; block.pot[i] = 0.5f; block.input_patched[i] = false; }
  tick_ms(50);
}

// Runs ~4 s with the firmware's free-run gate logic, returns true if the
// output of segment 1's generator keeps moving through its segments.
// Mirrors the free-run gate logic in stages.cc Process().
bool was = false;
int restarts = 0;
void run_block(uint8_t buttons) {
  static GateFlags no_gate[kBlockSize] = {0}, fr[kBlockSize];
  cs.set_local_switch_pressed(buttons); cs.Update(block, &st, sg, out);
  const GateFlags* g = no_gate;
  const bool restart = cs.take_restart(0);
  if (restart) ++restarts;
  if (cs.free_running(0)) { for (size_t j = 0; j < kBlockSize; ++j) fr[j] = GATE_FLAG_HIGH; if (!was || restart) fr[0] = GateFlags(GATE_FLAG_HIGH | GATE_FLAG_RISING); was = true; g = fr; } else was = false;
  sg[0].Process(g, out, kBlockSize);
}
bool cycles(float* range_out, bool need_range = true) {
  float lo = 10, hi = -10; int seg_changes = 0, last_seg = -1;
  for (int k = 0; k < 4000 * 4; ++k) {
    run_block(0);
    if (k > 4000) { for (size_t j = 0; j < kBlockSize; ++j) { if (out[j].value < lo) lo = out[j].value; if (out[j].value > hi) hi = out[j].value; if (out[j].segment != last_seg) { ++seg_changes; last_seg = out[j].segment; } } }
  }
  *range_out = hi - lo;
  return (!need_range || hi - lo > 0.3f) && seg_changes > 4;
}

int main() {
  memset(&block, 0, sizeof(block));
  for (size_t i = 0; i < kNumChannels; ++i) sg[i].Init();
  st.mutable_state()->multimode = MULTI_MODE_STAGES;
  cs.Init(&l, &r, st);
  tick_ms(2100);
  check(cs.size() == 1 && cs.link_mode(), "single module in Stages mode -> link mode");
  reset_all();

  // 1. Hold 1 past the loop threshold, tap 4: link 1-4, no loop on 1.
  press(0); tick_ms(1500); press(3); tick_ms(100); release(3); tick_ms(50); release(0); tick_ms(50);
  check(links() == 0x07, "hold 1 + tap 4 links segments 1-4");
  check(loops() == 0, "holding the first button did not toggle a loop");
  check(cs.link_flash_mask() == 0x0f && !cs.link_flash_red(), "green flash on 1-4");
  check(cs.linked(0) && cs.linked(3) && !cs.linked(4), "linked() reports 1-4 only");
  check(sg[0].num_segments() == 4, "segment 1 runs a 4-segment group");

  // 2. Hold 1, tap 3 inside the group: sets loop 1..3.
  press(0); tick_ms(300); press(2); tick_ms(100); release(2); tick_ms(20); release(0); tick_ms(50);
  check(links() == 0x07, "tap inside a linked group keeps links");
  check(loops() == 0x05, "tap inside a linked group sets loop from 1 to 3");

  // 3. Nothing patched in segment 1 + loop: group free-runs.
  check(cs.free_running(0), "looped group with no gate free-runs");
  block.input_patched[0] = true; tick_ms(20);
  check(!cs.free_running(0), "patching a gate into segment 1 stops free-run");
  block.input_patched[0] = false; tick_ms(2100);

  // 4. Short press on segment 1 changes type but keeps the link.
  press(0); tick_ms(100); release(0); tick_ms(20);
  check((cfg(0) & 3) == 1 && links() == 0x07, "type change on a linked segment keeps the link");

  // 5. Hold 1, hold 4 for 2.5 s: unlink, red flash.
  press(0); tick_ms(200); press(3); tick_ms(2500); release(3); tick_ms(20); release(0); tick_ms(50);
  check(links() == 0, "hold 1 + hold 4 for 2 s unlinks");
  check(cs.link_flash_mask() == 0x0f && cs.link_flash_red(), "red flash on 1-4");
  check(sg[0].num_segments() == 1 && !cs.linked(0), "segment 1 back to standing alone");

  // 6. Single short press changes type (3 types in Stages mode); 0.6 s press
  //    toggles loop.
  uint8_t before = cfg(4) & 3;
  press(4); tick_ms(100); release(4); tick_ms(20);
  check((cfg(4) & 3) == (before + 1) % 3, "short press changes segment type");
  press(5); tick_ms(600); release(5); tick_ms(20);
  check(loops() & 0x20, "0.6 s press toggles loop on segment 6");
  check(!(cfg(5) & kLinkBit), "segment 6 never stores a link bit");

  // 7. Right-to-left also works, and unlinking the middle splits groups.
  press(5); tick_ms(200); press(0); tick_ms(80); release(0); tick_ms(20); release(5); tick_ms(50);
  check(links() == 0x1f, "hold 6 + tap 1 links all six");
  press(2); tick_ms(200); press(3); tick_ms(2200); release(3); release(2); tick_ms(50);
  check(links() == 0x11, "unlinking 3-4 leaves groups 1-2 and 5-6");
  check(cs.linked(0) && cs.linked(1) && !cs.linked(2) && !cs.linked(3) && cs.linked(4), "linked() matches the split");

  // 8. Gate patched into segment 2 of a linked group does not split it.
  block.input_patched[1] = true; tick_ms(20);
  check(sg[0].num_segments() == 2 && cs.linked(1), "gate inside a group is ignored, group stays whole");
  block.input_patched[1] = false; tick_ms(2100);

  // 9. While the UI owns the buttons (hold + slider/pot, mode change), a
  //    release does nothing.
  uint16_t c3 = cfg(3);
  press(3); tick_ms(100); suspend = 1; tick_ms(200); suspend = 0; release(3); tick_ms(20);
  check(cfg(3) == c3, "release after a UI property change does not change type");
  press(3); press(4); tick_ms(100); suspend = 1; tick_ms(50); suspend = 0; release(4); release(3); tick_ms(20);
  check(links() == 0x11, "combo interrupted by the UI does not link");

  // 10. Holding past 5 s with no mode change (current mode button) does
  //     nothing, same as stock qiemem.
  uint8_t l0 = loops();
  press(0); tick_ms(5200); release(0); tick_ms(20);
  check(loops() == l0, "5 s press does not toggle loop");

  // 11. Free-run actually cycles: link 1-2, loop 1..2, nothing patched.
  reset_all();
  press(0); tick_ms(200); press(1); tick_ms(80); release(1); tick_ms(20); release(0); tick_ms(50);
  press(0); tick_ms(200); press(1); tick_ms(80); release(1); tick_ms(20); release(0); tick_ms(50);
  check(links() == 0x01 && loops() == 0x03, "group 1-2 with loop 1..2");
  float range;
  bool ok = cycles(&range);
  printf("     free-run output range %.3f\n", range);
  check(ok, "looped group with no gate keeps cycling");

  // 11b. Changing a segment's type reconfigures the group; it must keep
  //      cycling (needs a fresh rising edge). Hold, not step: a step waits
  //      for the gate to fall, which never happens in free-run.
  restarts = 0;
  for (int t = 0; t < 2; ++t)
    for (int k = 0; k < 400; ++k) run_block(k < 100 ? 0x02 : 0);  // tap 2
  check((cfg(1) & 3) == 2 && restarts > 0, "segment 2 changed to hold, group restarted");
  // Ramp into a hold at the same level is flat, so only count segments.
  ok = cycles(&range, false);
  check(ok, "free-running group keeps cycling after a type change");

  // 11d. Same, with the loop ending before the last segment of the group
  //      (group 1-3, loop 1..2). Without a fresh edge this one hangs.
  reset_all();
  for (int k = 0; k < 400; ++k) run_block(k < 200 ? 0x01 : (k < 280 ? 0x05 : (k < 300 ? 0x01 : 0)));
  for (int k = 0; k < 400; ++k) run_block(k < 200 ? 0x01 : (k < 280 ? 0x03 : (k < 300 ? 0x01 : 0)));
  check(links() == 0x03 && loops() == 0x03, "group 1-3 with loop 1..2");
  check(cycles(&range), "group 1-3 free-runs");
  for (int t = 0; t < 2; ++t)
    for (int k = 0; k < 400; ++k) run_block(k < 100 ? 0x04 : 0);  // tap 3 twice: hold
  check((cfg(2) & 3) == 2, "segment 3 changed to hold");
  check(cycles(&range), "group 1-3 keeps cycling after the type change");

  // 11c. Patching a gate into segment 2 does not reconfigure the group.
  restarts = 0;
  block.input_patched[1] = true;
  for (int k = 0; k < 4 * 2100; ++k) run_block(0);
  check(restarts == 0 && cs.free_running(0), "gate into segment 2 leaves the running group alone");
  block.input_patched[1] = false;
  for (int k = 0; k < 4 * 2100; ++k) run_block(0);
  check(restarts == 0, "unpatching it leaves the group alone too");

  // 12. Unlooped linked group with no gate waits.
  reset_all();
  press(0); tick_ms(200); press(2); tick_ms(80); release(2); tick_ms(20); release(0); tick_ms(50);
  check(links() == 0x03 && loops() == 0 && !cs.free_running(0), "unlooped group with no gate does not free-run");

  // 13. Advanced mode: four types, linking still works.
  st.mutable_state()->multimode = MULTI_MODE_STAGES_ADVANCED;
  cs.start_reinit(); tick_ms(2100 + 600);
  check(cs.link_mode() && links() == 0x03, "advanced mode keeps link mode and links");
  for (int k = 0; k < 3; ++k) { press(4); tick_ms(100); release(4); tick_ms(20); }
  check((cfg(4) & 3) == 3, "advanced mode cycles to the fourth (random) type");

  // 14. Ouroboros mode: no link mode, links stay stored but unused.
  st.mutable_state()->multimode = MULTI_MODE_OUROBOROS;
  cs.start_reinit(); tick_ms(600);
  check(!cs.link_mode() && !cs.linked(0), "ouroboros mode turns link mode off");
  check(links() == 0x03, "links stay stored for when Stages mode comes back");

  printf("\n%d failure(s)\n", fails);
  return fails != 0;
}
