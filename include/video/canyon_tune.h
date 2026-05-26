/* ============================================================
 *  canyon_tune.h — one place for every tunable in the lava-canyon
 *  flythrough (demo_fly3d) and the streamed procedural canyon
 *  (course_eval_long / the proc baker in course.c).
 *
 *  These are PLAIN numbers (no q16/float helpers) so both the portable
 *  course module and the demo can include this without pulling in any
 *  dependency. The demo wraps the distance values in q16_from_int().
 *
 *  Edit here to change the look/feel; nothing else needs touching.
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef VIDEO_CANYON_TUNE_H
#define VIDEO_CANYON_TUNE_H

/* ---- render / framebuffer -------------------------------------------
 * Raycast at this low res, then Mode-7 stretch to 256x224. Fewer pixels
 * = cheaper march (the perf budget). FB dims must be multiples of 8. */
#define CANYON_FB_W            64
#define CANYON_FB_H            56
#define CANYON_MAPSZ           512    /* toroidal terrain ring (power of 2); bounds max draw distance */

/* ---- draw distance --------------------------------------------------
 * The map ring (CANYON_MAPSZ) is the hard ceiling: you can't see farther
 * than it without far terrain wrapping back on itself. Keep
 * MAX_T < STREAM_LEAD < MAPSZ. Units are world cells. */
#define CANYON_FOG_RANGE       260    /* depth at which fog reaches its floor (visible distance) */
#define CANYON_MAX_T           320    /* hard ray-march distance cap (should be >= FOG_RANGE)     */
#define CANYON_SKY_H           170    /* a ray climbing past this is sky; must exceed tallest wall */
#define CANYON_STREAM_LEAD     400.0f /* bake this far ahead of the camera (must be < MAPSZ)       */

/* ---- forward motion / speed ----------------------------------------- */
#define CANYON_STREAM_SPEED    380.0f /* units/sec for the endless run            */
#define CANYON_GEN_VEL          34.0f /* cruise velocity for 'gen' courses        */
#define CANYON_SONG_SEC          2.5f /* one lap = this many seconds (non-stream)  */
#define CANYON_OFF_AMP           0.0f /* simulated lead/lag amplitude (0 = drone)  */
#define CANYON_OFF_W             0.5f /* lead/lag rate (rad/s)                     */

/* ---- camera: altitude (fixed-height flyover) ------------------------ */
#define CANYON_RIDE_H           24.0f /* camera clearance above the channel floor  */
#define CANYON_ALT_LAG          0.12f /* altitude follow rate (higher = snappier)  */
#define CANYON_ALT_MIN_CLR       4.0f /* clamp: stay at least this far off the lava */
#define CANYON_ALT_MAX_CLR      30.0f /* clamp: stay this far below the rim          */

/* ---- camera: lateral rail + aim ------------------------------------- */
#define CANYON_POS_K            0.04f /* lateral smoothing (low = near-straight rail) */
#define CANYON_AIM_K            0.06f /* look-direction gimbal smoothing              */
#define CANYON_LAT_LEASH        24.0f /* max lateral drift from the channel centre    */
#define CANYON_BACK_BASE         3.0f /* trailing-centre distance, base               */
#define CANYON_BACK_K          0.045f /* trailing-centre distance per unit speed      */
#define CANYON_LOOK_AHEAD       34.0f /* aim this far down the path                   */
#define CANYON_LOOK_UP           2.0f /* raise the aim point (look level, not down)   */

/* ---- camera: banking + FOV ------------------------------------------ */
#define CANYON_BANK_K           0.03f /* roll per unit path curvature   */
#define CANYON_BANK_MAX         0.45f /* roll clamp (radians)           */
#define CANYON_ROLL_SMOOTH      0.08f /* roll easing rate               */
#define CANYON_FOV_BASE         1.12f /* base vertical FOV (radians)    */
#define CANYON_FOV_K          0.0010f /* FOV widening per unit speed    */
#define CANYON_FOV_MAX          1.40f /* FOV clamp                      */

/* ---- non-stream (loaded/generated) collision clearances ------------- */
#define CANYON_FLOOR_CLR        14.0f /* keep the camera this far above the floor map */
#define CANYON_CEIL_CLR          8.0f /* keep the camera this far below a tunnel roof */

/* ---- lighting (unit-ish direction; renderer normalizes) ------------- */
#define CANYON_LIGHT_X          0.53f
#define CANYON_LIGHT_Y          0.74f
#define CANYON_LIGHT_Z          0.42f

/* ---- streamed canyon shape (course_eval_long) -----------------------
 * The canyon is a straight channel along +x at z = CANYON_Z_CENTER.
 * Each value is base + amplitude*sin(freq*x [+ seed phase]). */
#define CANYON_Z_CENTER         128.0f

#define CANYON_FLOOR_Y           72.0f  /* channel floor height (centre)        */
#define CANYON_ROLL_AMP           6.0f  /* gentle floor roll amplitude          */
#define CANYON_ROLL_FREQ       0.0011f  /* floor roll frequency (slow)          */

#define CANYON_WIDTH             60.0f  /* channel half-width                   */
#define CANYON_WIDTH_AMP          4.0f
#define CANYON_WIDTH_FREQ       0.008f

#define CANYON_WALL              60.0f  /* canyon wall height (= plateau above floor) */
#define CANYON_WALL_AMP1          8.0f  /* faster variety octave                */
#define CANYON_WALL_FREQ1      0.0035f
#define CANYON_WALL_AMP2          6.0f  /* slow drift octave (no rhythmic heave) */
#define CANYON_WALL_FREQ2      0.0011f

#define CANYON_FLAT2             0.35f  /* inner (d/width)^2 that stays FLAT floor;
                                         * bigger = wider flat floor, steeper walls */

#define CANYON_GATE_FREQ        0.006f  /* lava on/off alternation frequency    */
#define CANYON_LAVA_GATE         0.25f  /* gate threshold (>this => lava present) */
#define CANYON_LAVA              10.0f  /* lava river half-width                 */
#define CANYON_LAVA_AMP           3.0f
#define CANYON_LAVA_FREQ         0.02f

#endif /* VIDEO_CANYON_TUNE_H */
