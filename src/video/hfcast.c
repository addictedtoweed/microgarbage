/* ============================================================
 *  hfcast.c — fixed-point heightfield raycaster. See hfcast.h.
 *  Q16.16 throughout; no float, no libm. Public domain (CC0).
 * ============================================================ */

#include "video/hfcast.h"
#include "math/mat_q16.h"

/* march tuning (Q16.16), matching the float prototype */
#define HF_MAXT     q16_from_int(160)
#define HF_DTNEAR   q16_from_double(1.5)
#define HF_DTK      q16_from_double(0.045)
#define HF_MAXSTEPS 256

/* shading model constants (Q16.16) */
#define HF_FOG_MIN  q16_from_double(0.20)
#define HF_AMB      q16_from_double(0.28)
#define HF_DIFF     q16_from_double(0.72)
#define HF_LAVA_A   q16_from_double(0.80)
#define HF_LAVA_B   q16_from_double(0.20)
#define HF_CEIL_A   q16_from_double(0.16)
#define HF_CEIL_B   q16_from_double(0.34)
#define HF_NORMAL_UP 3                       /* heightfield normal y-bias */

static inline int hf_clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* bright (Q16.16, ~0..1) -> palette index within a material ramp. */
static inline uint8_t hf_shade(int base, int ramp, q16_16_t bright) {
    q16_16_t scaled = q16_mul(bright, q16_from_int(ramp - 1)) + (Q16_ONE >> 1);
    int s = q16_to_int(scaled);
    return (uint8_t)(base + hf_clampi(s, 0, ramp - 1));
}

long hfcast_render(const HfScene *s, const HfCamera *c,
                   uint8_t *fb, int fbw, int fbh) {
    const int      msz  = s->mapsz;
    const unsigned mask = (unsigned)(msz - 1);
    const q16_16_t fogr = s->fog_range;
    long steps = 0;

    /* basis whose columns are (right, up, fwd): basis * (nx,ny,1) is the
     * unnormalised ray direction for a screen point. */
    mat3_q16 basis = mat3_q16_from_basis_cols(c->right, c->up, c->fwd);

    for (int py = 0; py < fbh; py++) {
        /* ndc_y = 1 - (2*py+1)/fbh   (top=+1 .. bottom=-1) */
        q16_16_t ndc_y = Q16_ONE - q16_div(q16_from_int(2 * py + 1), q16_from_int(fbh));
        q16_16_t ny    = q16_mul(ndc_y, c->halfh);

        for (int px = 0; px < fbw; px++) {
            q16_16_t ndc_x = q16_div(q16_from_int(2 * px + 1), q16_from_int(fbw)) - Q16_ONE;
            q16_16_t nx    = q16_mul(ndc_x, c->halfw);

            vec3_q16 d = mat3_q16_mul_vec3(basis, vec3_q16_make(nx, ny, Q16_ONE));
            d = vec3_q16_normalize(d);

            uint8_t  out  = 0;                 /* sky */
            q16_16_t t    = Q16_ONE;
            q16_16_t prev = Q16_ONE;

            for (int it = 0; it < HF_MAXSTEPS; it++) {
                steps++;
                q16_16_t wx = c->pos.x + q16_mul(d.x, t);
                q16_16_t wy = c->pos.y + q16_mul(d.y, t);
                q16_16_t wz = c->pos.z + q16_mul(d.z, t);
                int xi = (int)(q16_to_int(wx)) & (int)mask;
                int zi = (int)(q16_to_int(wz)) & (int)mask;
                unsigned cell = (unsigned)zi * (unsigned)msz + (unsigned)xi;

                q16_16_t fl = q16_from_int(s->floor[cell]);
                q16_16_t cl = q16_from_int(s->ceiling[cell]);
                int      hit_mat = -1, is_floor = 0;

                if (wy <= fl) {                                  /* hit floor: refine */
                    q16_16_t lo = prev, hi = t;
                    for (int r = 0; r < 3; r++) {
                        q16_16_t mid = (lo + hi) >> 1;
                        int mxi = (int)q16_to_int(c->pos.x + q16_mul(d.x, mid)) & (int)mask;
                        int mzi = (int)q16_to_int(c->pos.z + q16_mul(d.z, mid)) & (int)mask;
                        unsigned mc = (unsigned)mzi * (unsigned)msz + (unsigned)mxi;
                        if (c->pos.y + q16_mul(d.y, mid) <= q16_from_int(s->floor[mc])) hi = mid;
                        else lo = mid;
                    }
                    t  = hi;
                    xi = (int)q16_to_int(c->pos.x + q16_mul(d.x, t)) & (int)mask;
                    zi = (int)q16_to_int(c->pos.z + q16_mul(d.z, t)) & (int)mask;
                    cell = (unsigned)zi * (unsigned)msz + (unsigned)xi;
                    hit_mat = s->material[cell];
                    is_floor = 1;
                } else if (cl < q16_from_int(254) && wy >= cl) { /* tunnel ceiling */
                    hit_mat = s->rock_mat;
                } else if (d.y >= 0 && wy > s->sky_h) {
                    break;                                       /* above all terrain -> sky */
                }

                if (hit_mat >= 0) {
                    q16_16_t fog = Q16_ONE - q16_div(t, fogr);
                    if (fog < HF_FOG_MIN) fog = HF_FOG_MIN;
                    if (fog > Q16_ONE)    fog = Q16_ONE;

                    q16_16_t bright;
                    int base;
                    if (hit_mat == s->lava_mat) {
                        bright = HF_LAVA_A + q16_mul(HF_LAVA_B, fog);   /* emissive */
                        base = s->lava_base;
                    } else if (is_floor) {
                        int hL = s->floor[(unsigned)zi * (unsigned)msz + (((unsigned)xi - 1) & mask)];
                        int hR = s->floor[(unsigned)zi * (unsigned)msz + (((unsigned)xi + 1) & mask)];
                        int hB = s->floor[(((unsigned)zi - 1) & mask) * (unsigned)msz + (unsigned)xi];
                        int hF = s->floor[(((unsigned)zi + 1) & mask) * (unsigned)msz + (unsigned)xi];
                        vec3_q16 nrm = vec3_q16_normalize(
                            vec3_q16_from_int(hL - hR, HF_NORMAL_UP, hB - hF));
                        q16_16_t lam = vec3_q16_dot(nrm, s->light);
                        if (lam < 0) lam = 0;
                        bright = q16_mul(HF_AMB + q16_mul(HF_DIFF, lam), fog);
                        base = (hit_mat == s->snow_mat) ? s->snow_base : s->rock_base;
                    } else {
                        bright = HF_CEIL_A + q16_mul(HF_CEIL_B, fog);   /* dim ceiling */
                        base = s->rock_base;
                    }
                    out = hf_shade(base, s->ramp, bright);
                    break;
                }

                prev = t;
                t += HF_DTNEAR + q16_mul(t, HF_DTK);
                if (t > HF_MAXT) break;
            }
            fb[py * fbw + px] = out;
        }
    }
    return steps;
}
