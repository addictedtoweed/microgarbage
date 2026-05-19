/* ============================================================
 *  audio.h — aggregator for audio modules
 *
 *  Include this to pull in audio_mixer and music_player. Each
 *  module is independently usable; this header just saves you the
 *  #includes.
 *
 *  audio_mixer depends on containers/ring_buffer and math/fixed_point.
 *  music_player depends on audio_mixer.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef GARBAGE_AUDIO_H
#define GARBAGE_AUDIO_H

#include "audio/audio_mixer.h"
#include "audio/music_player.h"

#endif /* GARBAGE_AUDIO_H */
