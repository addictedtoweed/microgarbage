/* Tests for the ik library: Aim2DOF analytical solver + chain FK round-trip.
 *
 * Build (Windows mingw):
 *   gcc -Wall -Wextra -std=c11 -Iinclude -o build/test_ik.exe \
 *       src/ik/ik.c src/ik/tests/test_ik.c
 *
 * Public domain (CC0). No warranty. */
#include "test_runner.h"
#include "ik/ik.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define EPS 1e-4f
#define NEAR(a, b) (fabsf((a) - (b)) < EPS)

static IkVec3 v(float x, float y, float z) { IkVec3 r = {x, y, z}; return r; }

/* ---- Aim2DOF: level base, target along +X -> pan=0, tilt=0 ----------- */
static void test_aim_level_forward(void) {
    IkAim2DOFReq in = {
        .base_roll = 0, .base_pitch = 0,
        .nozzle = v(0, 0, 0),
        .target = v(5, 0, 0),
    };
    IkAim2DOFRes out;
    ik_solve_aim_2dof(&in, &out);
    ASSERT(out.status == 0);
    ASSERT(NEAR(out.pan,  0.0f));
    ASSERT(NEAR(out.tilt, 0.0f));
}

/* ---- level base, target along +Y -> pan=+pi/2, tilt=0 ---------------- */
static void test_aim_level_left(void) {
    IkAim2DOFReq in = {
        .base_roll = 0, .base_pitch = 0,
        .nozzle = v(0, 0, 0),
        .target = v(0, 3, 0),
    };
    IkAim2DOFRes out;
    ik_solve_aim_2dof(&in, &out);
    ASSERT(out.status == 0);
    ASSERT(NEAR(out.pan,  (float)M_PI * 0.5f));
    ASSERT(NEAR(out.tilt, 0.0f));
}

/* ---- level base, target above-and-forward -> pan=0, tilt=+45 deg ----- */
static void test_aim_level_up45(void) {
    IkAim2DOFReq in = {
        .base_roll = 0, .base_pitch = 0,
        .nozzle = v(0, 0, 0),
        .target = v(1, 0, 1),
    };
    IkAim2DOFRes out;
    ik_solve_aim_2dof(&in, &out);
    ASSERT(out.status == 0);
    ASSERT(NEAR(out.pan,  0.0f));
    ASSERT(NEAR(out.tilt, (float)M_PI * 0.25f));
}

/* ---- base pitched +10 deg (nose down), aim forward -> pan=0, tilt=+10 deg
 * (the nozzle must tilt up in the monitor frame to stay world-horizontal) -- */
static void test_aim_pitched_compensation(void) {
    const float pitch = 10.0f * (float)M_PI / 180.0f;
    IkAim2DOFReq in = {
        .base_roll = 0, .base_pitch = pitch,
        .nozzle = v(0, 0, 0),
        .target = v(5, 0, 0),
    };
    IkAim2DOFRes out;
    ik_solve_aim_2dof(&in, &out);
    ASSERT(out.status == 0);
    ASSERT(NEAR(out.pan,  0.0f));
    ASSERT(NEAR(out.tilt, pitch));
}

/* ---- degenerate: target coincident with nozzle -> status = -1 -------- */
static void test_aim_degenerate(void) {
    IkAim2DOFReq in = {
        .base_roll = 0, .base_pitch = 0,
        .nozzle = v(1, 2, 3),
        .target = v(1, 2, 3),
    };
    IkAim2DOFRes out;
    ik_solve_aim_2dof(&in, &out);
    ASSERT(out.status == -1);
}

/* ---- chain FK: 2-DOF pan-tilt round-trip -------------------------------
 * Build the standard pan-tilt chain, ask Aim2DOF for joint angles to hit a
 * known target, plug those joint angles into the chain, run FK, and verify
 * the nozzle tip points (within length) at the target direction. */
static void test_chain_fk_aim_roundtrip(void) {
    /* target at 3m forward, 2m left, 1m above the nozzle (which is at origin) */
    IkAim2DOFReq req = {
        .base_roll = 0, .base_pitch = 0,
        .nozzle = v(0, 0, 0),
        .target = v(3, 2, 1),
    };
    IkAim2DOFRes res;
    ik_solve_aim_2dof(&req, &res);
    ASSERT(res.status == 0);

    /* build chain: root -> pan (Z) -> tilt (Y) -> nozzle (X offset = 1m) */
    IkChain ch; ik_chain_init(&ch);
    IkTransform ident = { {0,0,0}, {1,0,0,0} };

    IkJoint pan_j  = { .type = IK_JOINT_REVOLUTE, .axis = {0,0,1},
                       .q_min=0, .q_max=0, .q = res.pan  };
    IkJoint tilt_j = { .type = IK_JOINT_REVOLUTE, .axis = {0,1,0},
                       .q_min=0, .q_max=0, .q = -res.tilt /* see note below */ };
    IkJoint nozzle_j = { .type = IK_JOINT_FIXED, .axis = {0,0,0},
                         .q_min=0, .q_max=0, .q = 0 };

    int pan_i  = ik_chain_add(&ch, -1, ident, pan_j);
    int tilt_i = ik_chain_add(&ch, pan_i, ident, tilt_j);
    IkTransform nozzle_offset = { {1,0,0}, {1,0,0,0} };   /* nozzle 1m along +X */
    int nozzle_i = ik_chain_add(&ch, tilt_i, nozzle_offset, nozzle_j);
    ik_chain_set_end(&ch, nozzle_i);

    /* Note on tilt sign: ik_solve_aim_2dof returns tilt>0 = nozzle up (world
     * +Z component). With a Ry-rotation joint, q>0 rotates +X toward -Z (nose
     * down). So feed -res.tilt to the FK joint so the chain matches the same
     * convention as the analytical solver. */

    IkTransform poses[IK_MAX_NODES];
    ik_chain_fk(&ch, ident, poses);

    /* normalize the aim direction the chain produced */
    IkVec3 dir = poses[nozzle_i].t;
    float len = sqrtf(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
    ASSERT(len > 0.5f);
    dir.x /= len; dir.y /= len; dir.z /= len;

    /* and the desired direction from nozzle to target */
    IkVec3 want = v(3, 2, 1);
    float wlen = sqrtf(want.x*want.x + want.y*want.y + want.z*want.z);
    want.x /= wlen; want.y /= wlen; want.z /= wlen;

    ASSERT(NEAR(dir.x, want.x));
    ASSERT(NEAR(dir.y, want.y));
    ASSERT(NEAR(dir.z, want.z));
}

int main(void) {
    TEST_SUITE("ik");
    RUN(test_aim_level_forward);
    RUN(test_aim_level_left);
    RUN(test_aim_level_up45);
    RUN(test_aim_pitched_compensation);
    RUN(test_aim_degenerate);
    RUN(test_chain_fk_aim_roundtrip);
    return TEST_SUITE_RESULT();
}
