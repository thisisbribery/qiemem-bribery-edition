// Host-test stand-in for stages/settings.h: same State layout and helpers,
// no flash storage.
#ifndef STAGES_SETTINGS_H_
#define STAGES_SETTINGS_H_

#include <cstring>
#include "stmlib/stmlib.h"
#include "stages/io_buffer.h"
#include "stages/modes.h"

namespace stages {

#define is_bipolar(seg_config) seg_config & 0x08

const uint16_t kFollowBit = 0x4000;
const uint16_t kLinkBit = 0x8000;

struct State {
  uint16_t segment_configuration[kNumChannels];
  uint8_t color_blind;
  uint8_t multimode;
};

class Settings {
 public:
  Settings() : saves(0) { memset(&state_, 0, sizeof(state_)); }
  State* mutable_state() { return &state_; }
  const State& state() const { return state_; }
  void SaveState() { ++saves; }
  inline bool in_seg_gen_mode() const {
    return state_.multimode == MULTI_MODE_STAGES
      || state_.multimode == MULTI_MODE_STAGES_ADVANCED
      || state_.multimode == MULTI_MODE_STAGES_SLOW_LFO;
  }
  inline bool in_ouroboros_mode() const {
    return state_.multimode == MULTI_MODE_OUROBOROS
      || state_.multimode == MULTI_MODE_OUROBOROS_ALTERNATE;
  }
  int saves;
 private:
  State state_;
};

}  // namespace stages

#endif
