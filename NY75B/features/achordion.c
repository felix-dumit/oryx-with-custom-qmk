// Copyright 2022-2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
/**
 * @file achordion.c
 * @brief Achordion implementation
 *
 * For full documentation, see
 * <https://getreuer.info/posts/keyboards/achordion>
 */
#include "achordion.h"
#if !defined(IS_QK_MOD_TAP)
// Attempt to detect out-of-date QMK installation, which would fail with
// implicit-function-declaration errors in the code below.
#error "achordion: QMK version is too old to build. Please update QMK."
#else
// Copy of the `record` and `keycode` args for the current active tap-hold key.
static keyrecord_t tap_hold_record;
static uint16_t tap_hold_keycode = KC_NO;
// Timeout timer. When it expires, the key is considered held.
static uint16_t hold_timer = 0;
// Eagerly applied mods, if any.
static uint8_t eager_mods = 0;
// Flag to determine whether another key is pressed within the timeout.
static bool pressed_another_key_before_release = false;
#ifdef ACHORDION_STREAK
// Timer for typing streak
static uint16_t streak_timer = 0;
#else
// When disabled, is_streak is never true
#define is_streak false
#endif
// Achordion's current state.
enum {
  // A tap-hold key is pressed, but hasn't yet been settled as tapped or held.
  STATE_UNSETTLED,
  // Achordion is inactive.
  STATE_RELEASED,
  // Active tap-hold key has been settled as tapped.
  STATE_TAPPING,
  // Active tap-hold key has been settled as held.
  STATE_HOLDING,
  // This state is set while calling `process_record()`, which will recursively
  // call `process_achordion()`. This state is checked so that we don't process
  // events generated by Achordion and potentially create an infinite loop.
  STATE_RECURSING,
};
static uint8_t achordion_state = STATE_RELEASED;
#ifdef ACHORDION_STREAK
static void update_streak_timer(uint16_t keycode, keyrecord_t* record) {
  if (achordion_streak_continue(keycode)) {
    // We use 0 to represent an unset timer, so `| 1` to force a nonzero value.
    streak_timer = record->event.time | 1;
  } else {
    streak_timer = 0;
  }
}
#endif
// Presses or releases eager_mods through process_action(), which skips the
// usual event handling pipeline. The action is considered as a mod-tap hold or
// release, with Retro Tapping if enabled.
static void process_eager_mods_action(void) {
  action_t action;
  action.code = ACTION_MODS_TAP_KEY(
      eager_mods, QK_MOD_TAP_GET_TAP_KEYCODE(tap_hold_keycode));
  process_action(&tap_hold_record, action);
}
// Calls `process_record()` with state set to RECURSING.
static void recursively_process_record(keyrecord_t* record, uint8_t state) {
  achordion_state = STATE_RECURSING;
#if defined(POINTING_DEVICE_ENABLE) && defined(POINTING_DEVICE_AUTO_MOUSE_ENABLE)
  int8_t mouse_key_tracker = get_auto_mouse_key_tracker();
#endif
  process_record(record);
#if defined(POINTING_DEVICE_ENABLE) && defined(POINTING_DEVICE_AUTO_MOUSE_ENABLE)
  set_auto_mouse_key_tracker(mouse_key_tracker);
#endif
  achordion_state = state;
}
// Sends hold press event and settles the active tap-hold key as held.
static void settle_as_hold(void) {
  if (eager_mods) {
    // If eager mods are being applied, nothing needs to be done besides
    // updating the state.
    dprintln("Achordion: Settled eager mod as hold.");
    achordion_state = STATE_HOLDING;
  } else {
    // Create hold press event.
    dprintln("Achordion: Plumbing hold press.");
    recursively_process_record(&tap_hold_record, STATE_HOLDING);
  }
}
// Sends tap press and release and settles the active tap-hold key as tapped.
static void settle_as_tap(void) {
  if (eager_mods) {  // Clear eager mods if set.
#if defined(RETRO_TAPPING) || defined(RETRO_TAPPING_PER_KEY)
#ifdef DUMMY_MOD_NEUTRALIZER_KEYCODE
    neutralize_flashing_modifiers(get_mods());
#endif  // DUMMY_MOD_NEUTRALIZER_KEYCODE
#endif  // defined(RETRO_TAPPING) || defined(RETRO_TAPPING_PER_KEY)
    tap_hold_record.event.pressed = false;
    // To avoid falsely triggering Retro Tapping, process eager mods release as
    // a regular mods release rather than a mod-tap release.
    action_t action;
    action.code = ACTION_MODS(eager_mods);
    process_action(&tap_hold_record, action);
    eager_mods = 0;
  }
  dprintln("Achordion: Plumbing tap press.");
  tap_hold_record.event.pressed = true;
  tap_hold_record.tap.count = 1;  // Revise event as a tap.
  tap_hold_record.tap.interrupted = true;
  // Plumb tap press event.
  recursively_process_record(&tap_hold_record, STATE_TAPPING);
  send_keyboard_report();
#if TAP_CODE_DELAY > 0
  wait_ms(TAP_CODE_DELAY);
#endif  // TAP_CODE_DELAY > 0
  dprintln("Achordion: Plumbing tap release.");
  tap_hold_record.event.pressed = false;
  // Plumb tap release ev