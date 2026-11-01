// Copyright 2017 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Chain state.

#include "stages/chain_state.h"

#include <algorithm>

#include "stages/quantizer_scales.h"
#include "stages/drivers/serial_link.h"
#include "stages/segment_generator.h"
#include "stages/settings.h"
#include "stages/ui.h"

namespace stages {

using namespace std;

const uint32_t kSimpleLeftKey = stmlib::FourCC<'s', 'g', 's', 'l'>::value;
const uint32_t kSimpleRightKey = stmlib::FourCC<'s', 'g', 's', 'r'>::value;
const uint32_t kAdvancedLeftKey = stmlib::FourCC<'s', 'g', 'a', 'l'>::value;
const uint32_t kAdvancedRightKey = stmlib::FourCC<'s', 'g', 'a', 'r'>::value;

// How long before unpatching an input actually breaks the chain.
const uint32_t kUnpatchedInputDelay = 2000;
const int32_t kLongPressDuration = 500;
const int32_t kUnlinkPressDuration = 2000;

void ChainState::Init(SerialLink* left, SerialLink* right, const Settings& settings) {

  left_ = left;
  right_ = right;

  STATIC_ASSERT(sizeof(Packet) == kPacketSize, BAD_PACKET_SIZE);

  left_->Init(
#ifdef FLIPPED
      SERIAL_LINK_DIRECTION_RIGHT,
#else
      SERIAL_LINK_DIRECTION_LEFT,
#endif
      115200 * 8,
      left_rx_packet_[0].bytes,
      kPacketSize);
  right_->Init(
#ifdef FLIPPED
      SERIAL_LINK_DIRECTION_LEFT,
#else
      SERIAL_LINK_DIRECTION_RIGHT,
#endif
      115200 * 8,
      right_rx_packet_[0].bytes,
      kPacketSize);

  Reinit(settings);
}

void ChainState::Reinit(const Settings& settings) {
  index_ = 0;
  size_ = 1;

  ChannelState c = { .flags = 0b11100000, .pot = 128, .cv_slider = 32768 };

  fill(&channel_state_[0], &channel_state_[kMaxNumChannels], c);
  fill(&last_local_config_[0], &last_local_config_[kNumChannels], 0);
  fill(&unpatch_counter_[0], &unpatch_counter_[kNumChannels], 0);
  fill(&loop_status_[0], &loop_status_[kNumChannels], LOOP_STATUS_NONE);
  fill(&switch_pressed_[0], &switch_pressed_[kMaxChainSize], 0);
  fill(&switch_press_time_[0], &switch_press_time_[kMaxNumChannels], 0);

  request_.request = REQUEST_NONE;

  link_mode_ = false;
  links_ = 0;
  configured_links_ = 0xff;
  fill(&linked_config_[0], &linked_config_[kNumChannels], 0xffffffff);
  fill(&free_running_[0], &free_running_[kNumChannels], false);
  fill(&restart_[0], &restart_[kNumChannels], false);
  fill(&follower_[0], &follower_[kNumChannels], false);
  follows_ = 0;
  anchor_ = -1;
  fill(&combo_anchor_[0], &combo_anchor_[kNumChannels], -1);
  link_flash_id_ = 0;
  link_flash_mask_ = 0;
  link_flash_red_ = false;

  status_ = CHAIN_DISCOVERING_NEIGHBORS;
  counter_ = 0;
  num_internal_bindings_ = 0;
  num_bindings_ = 0;

  if (settings.state().multimode == MULTI_MODE_STAGES_ADVANCED) {
    leftKey = kAdvancedLeftKey;
    rightKey = kAdvancedRightKey;
  } else if (settings.in_seg_gen_mode()) {
    // Standard and slow LFO are the same
    leftKey = kSimpleLeftKey;
    rightKey = kSimpleRightKey;
  } else {
    // Other modes don't use chaining, so just skip it
    status_ = CHAIN_READY;
  }

  for (uint8_t i=0; i<kNumChannels; i++) {
    quantizers_[i].Init();
    quantizers_[i].Configure(scales[0]);
  }
}

void ChainState::DiscoverNeighbors() {
  // Between t = 500ms and t = 1500ms, ping the neighbors every 50ms
  if (counter_ >= 2000 &&
      counter_ <= 6000 &&
      (counter_ % 200) == 0) {
    left_tx_packet_.discovery.key = leftKey;
    left_tx_packet_.discovery.counter = size_;
    left_->Transmit(left_tx_packet_);

    right_tx_packet_.discovery.key = rightKey;
    right_tx_packet_.discovery.counter = index_;
    right_->Transmit(right_tx_packet_);
  }

  const DiscoveryPacket* l = left_->available_rx_buffer<DiscoveryPacket>();
  if (l && l->key == rightKey) {
    index_ = size_t(l->counter) + 1;
    size_ = std::max(size_, index_ + 1);
  }

  const DiscoveryPacket* r = right_->available_rx_buffer<DiscoveryPacket>();
  if (r && r->key == leftKey) {
    size_ = std::max(size_, size_t(r->counter));
  }

  bool ouroboros_ = index_ >= kMaxChainSize || size_ > kMaxChainSize;

  // The discovery phase lasts 2000ms.
  status_ = counter_ < 8000 && !ouroboros_ ? CHAIN_DISCOVERING_NEIGHBORS : CHAIN_READY;
  if (status_ == CHAIN_DISCOVERING_NEIGHBORS) {
    ++counter_;
  } else {
    counter_ = 0;
  }
}

void ChainState::StartReinit(const Settings& settings) {
  // counter_ may have ticked up... do a couple times just to be safe
  if ((counter_ % 200) == 0) {
    left_tx_packet_.discovery.key = kReinitKey;
    left_tx_packet_.discovery.counter = kReinitCount;
    right_tx_packet_.discovery.key = kReinitKey;
    right_tx_packet_.discovery.counter = kReinitCount;
    left_->Transmit(left_tx_packet_);
    right_->Transmit(right_tx_packet_);
  } else if (counter_ >= 2000) {
    Reinit(settings);
  }
  ++counter_;
}

void ChainState::TransmitRight() {
  if (index_ == size_ - 1) {
    return;
  }

  LeftToRightPacket* p = &right_tx_packet_.to_right;
  p->phase = tx_last_sample_.phase;
  p->segment = tx_last_sample_.segment;
  p->last_patched_channel = tx_last_patched_channel_;
  p->last_loop = tx_last_loop_;

  copy(&input_patched_[0], &input_patched_[index_ + 1], &p->input_patched[0]);
  copy(&switch_pressed_[0], &switch_pressed_[index_ + 1],
       &p->switch_pressed[0]);
  right_->Transmit(right_tx_packet_);
}

void ChainState::ReceiveRight() {
  const RightToLeftPacket* p = right_->available_rx_buffer<RightToLeftPacket>();
  if (p && check_reinit<RightToLeftPacket>(p)) {
    start_reinit();
    return;
  } else if (index_ == size_ - 1) {
    return;
  }

  if (p) {
    size_t rx_index = p->channel[0].index();
    if (rx_index > index_ && rx_index < size_) {
      // This packet contains the state of a module on the right.
      // Check if some settings have been changed on the remote modules,
      // then update our local copy of its state.
      for (size_t i = 0; i < kNumChannels; ++i) {
        dirty_[remote_channel_index(rx_index, i)] = \
            remote_channel(rx_index, i)->flags != p->channel[i].flags;
      }
      copy(
          &p->channel[0],
          &p->channel[kNumChannels],
          remote_channel(rx_index, 0));
      request_.request = REQUEST_NONE;
    // rx_index is only 3 bits now, so check if 0x7 instead of 0xf
    } else if (rx_index == 0x7) {
      // This suspiciously looks like a state change request packet!
      // We will take care of it later.
      request_ = *(const RequestPacket*)(p);
    }
  }
}

void ChainState::TransmitLeft() {
  if (index_ == 0) {
    return;
  }

  if (request_.request != REQUEST_NONE) {
    // Forward the request to the left.
    left_tx_packet_.request = request_;
  } else {
    // Determine which module contains the last segment of the chain
    // starting at this module's last segment.
    //
    // For example:
    //
    // 0----- 1----- 2----- 3----- 4-----
    //
    // ----X- ------ ---X-- ------ ---X--
    //
    // last = 2
    //        last = 2
    //               last = 4
    //                      last = 4
    //                             last = 5
    size_t last = size_ - 1;
    for (size_t i = index_; i < size_; ++i) {
      for (size_t j = 0; j < kNumChannels; ++j) {
        if (remote_channel(i, j)->input_patched()) {
          last = i;
          goto found;
        }
      }
    }
  found:
    // In the example above, module 1 will alternate between sending to module
    // 0 its own state, and the state of module 2.
    size_t tx_index = index_ + ((counter_ >> 2) % (last - index_ + 1));
    copy(
        remote_channel(tx_index, 0),
        remote_channel(tx_index, kNumChannels),
        &left_tx_packet_.to_left.channel[0]);
  }
  left_->Transmit(left_tx_packet_);
}

void ChainState::ReceiveLeft() {
  const LeftToRightPacket* p = left_->available_rx_buffer<LeftToRightPacket>();
  if (p && check_reinit(p)) {
    start_reinit();
  } else if (index_ == 0) {
    rx_last_patched_channel_ = size_ * kNumChannels;
    rx_last_loop_.start = -1;
    rx_last_loop_.end = -1;
    return;
  }

  if (p) {
    rx_last_patched_channel_ = p->last_patched_channel;
    rx_last_loop_ = p->last_loop;
    rx_last_sample_.phase = p->phase;
    rx_last_sample_.segment = p->segment;
    copy(&p->switch_pressed[0], &p->switch_pressed[index_],
         &switch_pressed_[0]);
    copy(&p->input_patched[0], &p->input_patched[index_], &input_patched_[0]);
  }
}

void ChainState::Configure(
    SegmentGenerator* segment_generator, const Settings& settings) {
  size_t last_local_channel = local_channel_index(0) + kNumChannels;
  size_t last_channel = size_ * kNumChannels;
  size_t last_patched_channel = rx_last_patched_channel_;
  Loop last_loop = rx_last_loop_;

  num_internal_bindings_ = 0;
  num_bindings_ = 0;

  if (link_mode_) {
    ConfigureLinked(segment_generator, settings);
    return;
  }
  fill(&free_running_[0], &free_running_[kNumChannels], false);
  fill(&follower_[0], &follower_[kNumChannels], false);
  configured_links_ = 0xff;

  segment::Configuration configuration[kMaxNumChannels];

  const uint16_t *local_configs = settings.state().segment_configuration;
  const MultiMode mode = static_cast<MultiMode>(settings.state().multimode);

  attenuate_ = 0;
  process_cv_ = 0;

  for (size_t i = 0; i < kNumChannels; ++i) {
    size_t channel = local_channel_index(i);
    segment_generator[i].SetMode(mode);
    if (!local_channel(i)->input_patched()) {
      if (channel > last_patched_channel) {
        // Create a slave channel - we are just extending a chain of segments.
        size_t segment = channel - last_patched_channel;
        segment_generator[i].ConfigureSlave(segment);
        set_loop_status(i, segment, last_loop);
      } else {
        // Create a free-running channel.
        segment::Configuration c = local_channel(i)->configuration(local_configs[i]);
        segment_generator[i].ConfigureSingleSegment(false, c);
        binding_[num_bindings_].generator = i;
        binding_[num_bindings_].source = i;
        binding_[num_bindings_].destination = 0;
        ++num_bindings_;
        ++num_internal_bindings_;
        loop_status_[i] = c.loop ? LOOP_STATUS_SELF : LOOP_STATUS_NONE;
      }
    } else {
      last_patched_channel = channel;

      // Create a normal channel, trying to extend it as far as possible.
      int num_segments = 0;
      bool add_more_segments = true;
      bool dirty = false;

      last_loop.start = -1;
      last_loop.end = -1;
      while (add_more_segments) {
        // Add an entry in the configuration array.
        segment::Configuration c = channel < last_local_channel
          ? channel_state_[channel].configuration(local_configs[i + num_segments])
          : channel_state_[channel].configuration();
        configuration[num_segments] = c;
        dirty |= dirty_[channel];

        if (c.loop) {
          if (last_loop.start == -1) last_loop.start = num_segments;
          last_loop.end = num_segments;
        }

        // Add a binding in the binding array.
        binding_[num_bindings_].generator = i;
        binding_[num_bindings_].destination = num_segments;
        if (channel < last_local_channel) {
          // Bind local CV/pot to this segment's parameters.
          binding_[num_bindings_].source = i + num_segments;
          ++num_internal_bindings_;
        } else {
          // Bind remote CV/pot to this segment's parameters.
          binding_[num_bindings_].source = channel;
        }
        ++num_bindings_;
        ++channel;
        ++num_segments;

        add_more_segments = channel < last_channel && \
             !channel_state_[channel].input_patched();
      }
      if (dirty || num_segments != segment_generator[i].num_segments()) {
        segment_generator[i].Configure(true, configuration, num_segments);
      }
      set_loop_status(i, 0, last_loop);
    }
    attenuate_ |= segment_generator[i].needs_attenuation() << i;
    process_cv_ |= segment_generator[i].needs_cv_preprocessing() << i;
  }
  tx_last_loop_ = last_loop;
  tx_last_patched_channel_ = last_patched_channel;
}

// Groups come from the stored links instead of from which gate inputs are
// patched. A group is fired by the gate in its leftmost segment; gates
// patched into the other segments of a group are ignored, and patching or
// unpatching them does not reconfigure the group.
void ChainState::ConfigureLinked(
    SegmentGenerator* segment_generator, const Settings& settings) {
  const uint16_t *local_configs = settings.state().segment_configuration;
  const MultiMode mode = static_cast<MultiMode>(settings.state().multimode);
  const bool links_changed = links_ != configured_links_;
  configured_links_ = links_;

  segment::Configuration configuration[kNumChannels];
  attenuate_ = 0;
  process_cv_ = 0;

  size_t i = 0;
  while (i < kNumChannels) {
    const size_t end = group_end(i);
    const int num_segments = end - i + 1;
    const bool patched = local_channel(i)->input_patched();
    bool dirty = links_changed;
    Loop loop;
    loop.start = -1;
    loop.end = -1;

    for (int k = 0; k < num_segments; ++k) {
      const size_t s = i + k;
      segment_generator[s].SetMode(mode);
      segment::Configuration c = local_channel(s)->configuration(
          local_configs[s]);
      configuration[k] = c;
      if (c.loop) {
        if (loop.start == -1) loop.start = k;
        loop.end = k;
      }
      // Compare what configuration() is built from: the transmitted flags
      // (type, loop, bipolar, patched) and the local-only bits. Only the
      // leftmost gate counts, so leave the patched state of the other
      // segments (and their alt gate bit) out of the comparison.
      const uint32_t flags = local_channel(s)->flags;
      const uint32_t local = local_configs[s] & ~kLinkBit;
      uint32_t signature = k == 0
          ? flags | (local << 8)
          : (flags & ~0x08) | ((local & ~0x0080) << 8);
      dirty |= signature != linked_config_[s];
      linked_config_[s] = signature;

      binding_[num_bindings_].generator = i;
      binding_[num_bindings_].source = s;
      binding_[num_bindings_].destination = k;
      ++num_bindings_;
      ++num_internal_bindings_;
      free_running_[s] = false;
      follower_[s] = false;
    }

    if (num_segments == 1 && (local_configs[i] & kFollowBit)) {
      // Envelope follower. Its gate input freezes the envelope.
      segment_generator[i].ConfigureFollower(configuration[0]);
      follower_[i] = true;
      loop_status_[i] = LOOP_STATUS_NONE;
    } else if (num_segments == 1 && !patched) {
      // Lone segment with nothing patched: same as a free-running channel.
      segment_generator[i].ConfigureSingleSegment(false, configuration[0]);
      loop_status_[i] = configuration[0].loop
          ? LOOP_STATUS_SELF
          : LOOP_STATUS_NONE;
    } else {
      if (dirty || num_segments != segment_generator[i].num_segments()) {
        segment_generator[i].Configure(true, configuration, num_segments);
        restart_[i] = true;
      }
      for (int k = 0; k < num_segments; ++k) {
        if (k) {
          segment_generator[i + k].ConfigureSlave(k);
        }
        set_loop_status(i + k, k, loop);
      }
      free_running_[i] = num_segments > 1 && !patched && loop.start != -1;
    }
    i = end + 1;
  }

  for (size_t i = 0; i < kNumChannels; ++i) {
    attenuate_ |= segment_generator[i].needs_attenuation() << i;
    process_cv_ |= segment_generator[i].needs_cv_preprocessing() << i;
  }
  tx_last_loop_ = rx_last_loop_;
  tx_last_patched_channel_ = rx_last_patched_channel_;
}

inline void ChainState::UpdateLocalState(
    const IOBuffer::Block& block,
    const Settings& settings,
    const SegmentGenerator::Output& last_out) {
  tx_last_sample_ = last_out;

  ChannelBitmask input_patched_bitmask = 0;
  for (size_t i = 0; i < kNumChannels; ++i) {
    if (block.input_patched[i]) {
      unpatch_counter_[i] = 0;
    } else if (unpatch_counter_[i] < kUnpatchedInputDelay) {
      ++unpatch_counter_[i];
    }

    bool input_patched = unpatch_counter_[i] < kUnpatchedInputDelay;
    uint16_t config = settings.state().segment_configuration[i];
    size_t channel = local_channel_index(i);
    dirty_[channel] = local_channel(i)->UpdateFlags(
        index_,
        config,
        input_patched)
      || (config != last_local_config_[i]); // Check props that are not transmitted
    if (dirty_[channel]
        && ((config >> 12 & 0x03) != (last_local_config_[i] >> 12 & 0x03))) {
      quantizers_[i].Configure(scales[config >> 12 & 0x03]);
    }
    last_local_config_[i] = config;
    if (input_patched) {
      input_patched_bitmask |= 1 << i;
    }
  }
  input_patched_[index_] = input_patched_bitmask;
}

inline void ChainState::UpdateLocalPotCvSlider(
    const IOBuffer::Block& block, const Settings& settings) {
  const uint16_t *configs = settings.state().segment_configuration;
  for (size_t i = 0; i < kNumChannels; ++i) {
    ChannelState* s = local_channel(i);
    float cvs = process_cv_ >> i & 1
        ? cv_slider(block, i, configs[i])
        : block.cv_slider[i];
    s->cv_slider = cvs * 16384.0f + 32768.0f;
    s->pot = block.pot[i] * 256.0f;
  }
}

inline void ChainState::BindRemoteParameters(
    SegmentGenerator* segment_generator) {
  for (size_t i = num_internal_bindings_; i < num_bindings_; ++i) {
    const ParameterBinding& m = binding_[i];
    segment_generator[m.generator].set_segment_parameters(
        m.destination,
        channel_state_[m.source].cv_slider / 16384.0f - 2.0f,
        channel_state_[m.source].pot / 256.0f);
  }
}

inline void ChainState::BindLocalParameters(
    const IOBuffer::Block& block,
    SegmentGenerator* segment_generator,
    const Settings& settings) {
  const uint16_t *configs = settings.state().segment_configuration;
  for (size_t i = 0; i < num_internal_bindings_; ++i) {
    const ParameterBinding& m = binding_[i];
    size_t s = m.source;
    segment_generator[m.generator].set_segment_parameters(
        m.destination,
        process_cv_ >> s & 1 ? cv_slider(block, s, configs[s]) : block.cv_slider[s],
        block.pot[s],
        block.cv[s],
        block.slider[s]);
  }
}

ChainState::RequestPacket ChainState::MakeLoopChangeRequest(
    size_t loop_start, size_t loop_end) {
  size_t channel_index = 0;
  size_t group_start = 0;
  size_t group_end = size_ * kNumChannels;

  bool inconsistent_loop = false;

  // Fill group_start and group_end, which contain the tightest interval
  // of patched channels enclosing the loop.
  //
  // LOOP     ----S- ------ --E--- ------
  // PATCHED  -x---- ------ ----x- ------
  //           ^  ^           ^ ^
  //           |  |           | |
  //           |  |           | group_end
  //           |  |           loop_end
  //           |  loop_start
  //           group_start
  for (size_t i = 0; i < size_; ++i) {
    ChannelBitmask input_patched = input_patched_[i];
    for (size_t j = 0; j < kNumChannels; ++j) {
      if (input_patched & 1) {
        if (channel_index <= loop_start) {
          group_start = channel_index;
        } else if (channel_index >= loop_end) {
          group_end = min(group_end, channel_index);
        }
        // There shouldn't be a patched channel between the loop start
        // and the loop end.
        if (channel_index > loop_start && channel_index < loop_end) {
          // LOOP     ----S- ------ --E--- ------
          // PATCHED  -x---- ---x-- ----x- ------
          inconsistent_loop = true;
        }
      }
      input_patched >>= 1;
      ++channel_index;
    }
  }

  // There shouldn't be a loop spanning multiple channels among the first
  // group of unpatched channels.
  if (group_start == 0 && !(input_patched_[0] & 1)) {
    if (loop_start != loop_end) {
      // LOOP     -S-E-- ------
      // PATCHED  -----x ---x--
      inconsistent_loop = true;
    } else {
      group_start = group_end = loop_start = loop_end;
    }
  }

  // The only situation where a loop can end on a patched channel is when
  // we have a single-channel group.
  if (group_end == loop_end && group_start != group_end) {
    // Correct:
    // LOOP     ---S--
    //             E
    // PATCHED  ---xx-

    // Incorrect:
    // LOOP     ---S-E
    // PATCHED  ---x-x
    inconsistent_loop = true;
  }

  RequestPacket result;
  if (inconsistent_loop) {
    result.request = REQUEST_NONE;
  } else {
    result.request = REQUEST_SET_LOOP;
    result.argument[0] = group_start;
    result.argument[1] = loop_start;
    result.argument[2] = loop_end;
    result.argument[3] = group_end;
  }
  return result;
}

void ChainState::PollSwitches() {
  // The last module in the chain polls the states of the switches for the
  // entire chain. The state of the switches has been passed from left
  // to right.
  //
  // If a switch has been pressed, a Request packet is passed from right
  // to left. Each module is responsible from parsing the Request packet
  // and adjusting its internal state to simulate local changes. During
  // the next cycle (1ms later), the internal change will be propagated
  // from module to module through the usual mechanism (ChannelState
  // transmission).
  //
  // New property changes (polarity, frequency range, etc.) are handled locally
  // (transmitting what property should be changed is hard). If switches are
  // being handled locally, a module emits 0xff for its switch pressed bitmask
  // (0xff cannot naturally occur). Then, the rightmost module knows to suspend
  // switch processing for that module, setting all counts to -1 for it (so
  // that state changes aren't triggered on switch release if the count had
  // been sufficiently high prior to property change). Properties that must be
  // known by other modules are then trasmitted through ChannelState; this
  // includes polarity/retrigger control (which use the same bit).
  if (link_mode_) {
    PollSwitchesLinked();
    return;
  }
  if (index_ == size_ - 1) {
    request_.request = REQUEST_NONE;
    size_t first_pressed = kMaxNumChannels;

    for (size_t i = 0; i < size_; ++i) {
      ChannelBitmask switch_pressed = switch_pressed_[i];
      size_t switch_index = i * kNumChannels;
      if (switch_pressed == 0xff) {
        // Switches are being locally processed; suspend
        fill(
            &switch_press_time_[switch_index],
            &switch_press_time_[switch_index + kNumChannels],
            -1);
      } else if (switch_pressed || switch_pressed != last_switch_pressed_[i]) {
        last_switch_pressed_[i] = switch_pressed;
        for (size_t j = 0; j < kNumChannels; ++j) {
          if (switch_pressed & 1) {
            if (switch_press_time_[switch_index] != -1) {
              ++switch_press_time_[switch_index];
              if (first_pressed != kMaxNumChannels) {
                // Simultaneously pressing a pair of buttons.
                request_ = MakeLoopChangeRequest(first_pressed, switch_index);
                switch_press_time_[first_pressed] = -1;
                switch_press_time_[switch_index] = -1;
              } else {
                first_pressed = switch_index;
              }
            }
          } else {
            if (switch_press_time_[switch_index] > kLongPressDuration) { // Long-press
              if (switch_press_time_[switch_index] < kLongPressDurationForMultiModeToggle) { // But not long enough for multi-mode toggle
                request_ = MakeLoopChangeRequest(switch_index, switch_index);
                switch_press_time_[switch_index] = -1;
              }
            } else if (switch_press_time_[switch_index] > 5) {
              // A button has been released after having been held for a
              // sufficiently long time (5ms), but not for long enough to be
              // detected as a long press.
              request_.request = REQUEST_SET_SEGMENT_TYPE;
              request_.argument[0] = switch_index;
            }
            switch_press_time_[switch_index] = 0;
          }
          switch_pressed >>= 1;
          ++switch_index;
        }
      }
    }
  }
}

// Button handling when segments can be linked (single module).
//
// Single button, acting on release:
//   short press: change segment type
//   held 0.5 s or more: toggle loop
//   held 5 s: multi-mode change (handled by the UI, as before)
// Hold A, then tap B:
//   A..B not linked yet: link A..B (green flash)
//   A..B already linked: set the loop from A to B
// Hold A, then hold B for 2 s: unlink A..B (red flash)
void ChainState::PollSwitchesLinked() {
  request_.request = REQUEST_NONE;
  const ChannelBitmask pressed = switch_pressed_[0];

  if (pressed == 0xff) {
    // The UI is using the buttons (property change or mode change). Ignore
    // every press that is in progress.
    fill(&switch_press_time_[0], &switch_press_time_[kNumChannels], -1);
    fill(&combo_anchor_[0], &combo_anchor_[kNumChannels], -1);
    anchor_ = -1;
    return;
  }

  for (size_t i = 0; i < kNumChannels; ++i) {
    const bool down = pressed & (1 << i);
    int16_t* t = &switch_press_time_[i];

    if (*t == -1) {
      if (!down) {
        *t = 0;
      }
      continue;
    }

    if (down) {
      if (*t == 0) {
        // New press.
        if (anchor_ == -1) {
          anchor_ = i;
          combo_anchor_[i] = -1;
        } else if (anchor_ != static_cast<int8_t>(i)) {
          combo_anchor_[i] = anchor_;
          combo_anchor_[anchor_] = anchor_;  // Anchor took part in a combo.
        }
      }
      if (*t < 32000) {
        ++*t;
      }
      const int8_t a = combo_anchor_[i];
      if (a != -1 && a != static_cast<int8_t>(i) &&
          *t == kUnlinkPressDuration && (pressed & (1 << a)) &&
          request_.request == REQUEST_NONE) {
        request_.request = REQUEST_UNLINK;
        request_.argument[0] = min(static_cast<size_t>(a), i);
        request_.argument[1] = max(static_cast<size_t>(a), i);
      }
    } else if (*t) {
      // Release.
      const int8_t a = combo_anchor_[i];
      if (static_cast<int8_t>(i) == anchor_) {
        if (a == -1 && *t > 5 && request_.request == REQUEST_NONE) {
          if (*t <= kLongPressDuration) {
            request_.request = REQUEST_SET_SEGMENT_TYPE;
            request_.argument[0] = i;
          } else if (*t < kLongPressDurationForMultiModeToggle &&
                     !((follows_ >> i) & 1)) {
            // (A follower has no loop.)
            request_.request = REQUEST_SET_LOOP;
            request_.argument[0] = group_start(i);
            request_.argument[1] = i;
            request_.argument[2] = i;
            request_.argument[3] = group_end(i) + 1;
          }
        }
        anchor_ = -1;
      } else if (a != -1 && *t > 5 && *t < kUnlinkPressDuration &&
                 request_.request == REQUEST_NONE) {
        const size_t lo = min(static_cast<size_t>(a), i);
        const size_t hi = max(static_cast<size_t>(a), i);
        bool has_follower = false;
        for (size_t k = lo; k <= hi; ++k) {
          has_follower = has_follower || ((follows_ >> k) & 1);
        }
        if (has_follower) {
          // Followers stand alone: nothing to link or loop.
        } else if (group_start(lo) == group_start(hi)) {
          request_.request = REQUEST_SET_LOOP;
          request_.argument[0] = group_start(lo);
          request_.argument[1] = lo;
          request_.argument[2] = hi;
          request_.argument[3] = group_end(lo) + 1;
        } else {
          request_.request = REQUEST_LINK;
          request_.argument[0] = lo;
          request_.argument[1] = hi;
        }
      }
      combo_anchor_[i] = -1;
      *t = 0;
    }
  }
}

void ChainState::SuspendSwitches() {
  set_local_switch_pressed(0xff);
}

void ChainState::HandleRequest(Settings* settings) {
  if (request_.request == REQUEST_NONE) {
    return;
  }

  const uint8_t num_types = settings->state().multimode == MULTI_MODE_STAGES_ADVANCED ? 4 : 3;

  State* s = settings->mutable_state();

  if (request_.request == REQUEST_LINK ||
      request_.request == REQUEST_UNLINK) {
    const size_t lo = request_.argument[0];
    const size_t hi = request_.argument[1];
    uint8_t span = 0;
    uint8_t flash = 0;
    for (size_t k = lo; k <= hi; ++k) {
      flash |= 1 << k;
      if (k < hi) span |= 1 << k;
    }
    if (request_.request == REQUEST_UNLINK) {
      // Every segment in the span ends up standing alone.
      if (lo > 0) span |= 1 << (lo - 1);
      if (hi < kNumChannels - 1) span |= 1 << hi;
    }
    for (size_t k = 0; k < kNumChannels; ++k) {
      if (k == kNumChannels - 1) {
        s->segment_configuration[k] &= ~kLinkBit;
      } else if (span & (1 << k)) {
        if (request_.request == REQUEST_LINK) {
          s->segment_configuration[k] |= kLinkBit;
        } else {
          s->segment_configuration[k] &= ~kLinkBit;
        }
      }
    }
    link_flash_mask_ = flash;
    link_flash_red_ = request_.request == REQUEST_UNLINK;
    ++link_flash_id_;
    settings->SaveState();
    request_.request = REQUEST_NONE;
    return;
  }

  bool dirty = false;
  for (size_t i = 0; i < kNumChannels; ++i) {
    size_t channel = local_channel_index(i);
    uint16_t config = s->segment_configuration[i];

    uint8_t type_bits = config & 0x3;
    uint8_t loop_bit = config & 0x4;

    if (!local_channel(i)->input_patched()) {
      config &= ~0b10000000; // Reset alt gate bit
    }

    if (request_.request == REQUEST_SET_SEGMENT_TYPE) {
      if (channel == request_.argument[0]) {
        const bool was_follower = config & kFollowBit;
        config &= ~(0xff00 & ~kLinkBit); // Reset LFO range and follow, keep the link
        config &= ~0b10001011; // Reset type, bipolar, and input patched behavior bits
        if (was_follower) {
          // Follower -> ramp. Outside link mode a follower runs as a ramp,
          // so move on to step there.
          config |= link_mode_ ? 0 : 1;
        } else if (link_mode_ && type_bits + 1 == num_types) {
          config |= kFollowBit; // Last type -> envelope follower
        } else {
          config |= ((type_bits + 1) % num_types); // Cycle through segment types
        }
        dirty |= true;
      }
    } else if (request_.request == REQUEST_SET_LOOP) {
      uint8_t new_loop_bit = loop_bit;
      if ((channel >= request_.argument[0] && channel < request_.argument[3])) {
        new_loop_bit = 0x0;
      }
      if (channel == request_.argument[1] || channel == request_.argument[2]) {
        if (request_.argument[1] == request_.argument[2]) {
          new_loop_bit = 0x4 - loop_bit;
        } else {
          new_loop_bit = 0x4;
        }
      }
      config &= ~0b00000100;  // Reset loop bit
      config |= new_loop_bit; // Set new loop bit
      if (new_loop_bit != loop_bit) {
        if (request_.argument[1] == request_.argument[2]) {
          // config &= ~0xff00; // Reset LFO range
          config &= ~0b0000001100000000; // Reset LFO range
        }
        dirty = true;
      }
    }
    if (config != s->segment_configuration[i]) {
      s->segment_configuration[i] = config;
      dirty = true;
    }
  }

  if (dirty) {
    settings->SaveState();
  }
}

void ChainState::Update(
    const IOBuffer::Block& block,
    Settings* settings,
    SegmentGenerator* segment_generator,
    SegmentGenerator::Output* out) {
  link_mode_ = status_ == CHAIN_READY && size_ == 1 &&
      settings->in_seg_gen_mode();
  links_ = 0;
  follows_ = 0;
  for (size_t k = 0; k < kNumChannels; ++k) {
    const uint16_t config = settings->state().segment_configuration[k];
    if (k < kNumChannels - 1 && (config & kLinkBit)) {
      links_ |= 1 << k;
    }
    if (link_mode_ && (config & kFollowBit)) {
      follows_ |= 1 << k;
    }
  }
  // Link bit k joins segments k and k + 1: drop it if either is a follower.
  links_ &= ~(follows_ | (follows_ >> 1));

  switch (status_) {
    case CHAIN_DISCOVERING_NEIGHBORS:
      DiscoverNeighbors();
      return;
    case CHAIN_REINITIALIZING:
      StartReinit(*settings);
      return;
    default:
      break;
  }

  switch (counter_ & 0x3) {
    case 0:
      PollSwitches();
      UpdateLocalState(block, *settings, out[kBlockSize - 1]);
      TransmitRight();
      break;
    case 1:
      ReceiveRight();
      HandleRequest(settings);
      break;
    case 2:
      UpdateLocalPotCvSlider(block, *settings);
      TransmitLeft();
      break;
    case 3:
      ReceiveLeft();
      Configure(segment_generator, *settings);
      BindRemoteParameters(segment_generator);
      break;
  }

  BindLocalParameters(block, segment_generator, *settings);
  fill(&out[0], &out[kBlockSize], rx_last_sample_);

  ++counter_;
}

} // namespace stages
