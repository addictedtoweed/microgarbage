/* ============================================================
 *  mg_game.h — umbrella header for the cart-side game API.
 *
 *  Include this single header; it drags in the per-concern
 *  subheaders for frame pacing, input, sprites, actors, BG layers,
 *  Mode 7, HDMA, graphics, audio, and the panic path.
 *
 *  Full spec: docs/game-api.md.
 *
 *  Game programmers ONLY include this header; the SYS_MG_* numbers
 *  in vm_runtime.h are implementation detail of the mg_* library.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MG_GAME_H
#define MG_GAME_H

#include "mg_panic.h"
#include "mg_frame.h"
#include "mg_input.h"
#include "mg_sprite.h"
#include "mg_actor.h"
#include "mg_gfx.h"
#include "mg_bg.h"
#include "mg_mode7.h"
#include "mg_hdma.h"

/* Still pending: mg_audio.h - SFX + streaming wrappers. */

#endif /* MG_GAME_H */
