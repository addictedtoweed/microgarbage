/* ============================================================
 *  ik.c — see include/ik/ik.h.
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include <math.h>
#include "ik/ik.h"

/* ---- private math helpers --------------------------------------------- */

static IkVec3 v3_(float x, float y, float z) { IkVec3 v = {x, y, z}; return v; }

static IkVec3 v3_sub(IkVec3 a, IkVec3 b) {
    return v3_(a.x - b.x, a.y - b.y, a.z - b.z);
}
static float  v3_dot(IkVec3 a, IkVec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static float  v3_len(IkVec3 v)           { return sqrtf(v3_dot(v, v)); }
static IkVec3 v3_norm_safe(IkVec3 v) {
    float l = v3_len(v);
    if (l < 1e-9f) return v3_(1.0f, 0.0f, 0.0f);
    return v3_(v.x / l, v.y / l, v.z / l);
}

static IkQuat q_(float w, float x, float y, float z) { IkQuat q = {w, x, y, z}; return q; }

static IkQuat q_axis_angle(IkVec3 axis, float angle) {
    float ha = angle * 0.5f;
    float s  = sinf(ha);
    return q_(cosf(ha), axis.x * s, axis.y * s, axis.z * s);
}
static IkQuat q_mul(IkQuat a, IkQuat b) {
    return q_(
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z,
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w);
}
/* unit-quaternion rotate (shortcut: v' = v + 2*qv x (qv x v + qw*v)) */
static IkVec3 q_rotate(IkQuat q, IkVec3 v) {
    IkVec3 qv  = v3_(q.x, q.y, q.z);
    IkVec3 t   = v3_(2.0f * (qv.y*v.z - qv.z*v.y),
                     2.0f * (qv.z*v.x - qv.x*v.z),
                     2.0f * (qv.x*v.y - qv.y*v.x));
    return v3_(v.x + q.w*t.x + (qv.y*t.z - qv.z*t.y),
               v.y + q.w*t.y + (qv.z*t.x - qv.x*t.z),
               v.z + q.w*t.z + (qv.x*t.y - qv.y*t.x));
}

static IkTransform t_identity(void) {
    IkTransform x = { {0,0,0}, {1,0,0,0} };
    return x;
}
/* compose two rigid transforms: parent_world * child_local -> child_world */
static IkTransform t_compose(IkTransform a, IkTransform b) {
    IkTransform r;
    r.r = q_mul(a.r, b.r);
    IkVec3 rt = q_rotate(a.r, b.t);
    r.t.x = a.t.x + rt.x;
    r.t.y = a.t.y + rt.y;
    r.t.z = a.t.z + rt.z;
    return r;
}

/* ---- chain API -------------------------------------------------------- */

void ik_chain_init(IkChain *c) {
    c->count = 0;
    c->end_effector = -1;
}

int ik_chain_add(IkChain *c, int parent, IkTransform offset, IkJoint joint) {
    if (c->count >= IK_MAX_NODES) return -1;
    int idx = c->count++;
    c->nodes[idx].parent = parent;
    c->nodes[idx].offset = offset;
    c->nodes[idx].joint  = joint;
    return idx;
}

void ik_chain_set_end(IkChain *c, int node_index) {
    c->end_effector = node_index;
}

void ik_chain_fk(const IkChain *c, IkTransform base, IkTransform *poses) {
    /* nodes must be ordered so that any parent appears before its children.
     * ik_chain_add appends sequentially, so callers that build top-down
     * automatically satisfy this. */
    for (int i = 0; i < c->count; i++) {
        IkTransform parent_pose =
            (c->nodes[i].parent < 0) ? base : poses[c->nodes[i].parent];
        /* apply the rigid offset to land at the joint's pre-motion frame */
        IkTransform link = t_compose(parent_pose, c->nodes[i].offset);
        /* then apply the joint's own motion */
        IkTransform jt = t_identity();
        const IkJoint *j = &c->nodes[i].joint;
        switch (j->type) {
            case IK_JOINT_REVOLUTE:
                jt.r = q_axis_angle(v3_norm_safe(j->axis), j->q);
                break;
            case IK_JOINT_PRISMATIC:
                jt.t.x = j->axis.x * j->q;
                jt.t.y = j->axis.y * j->q;
                jt.t.z = j->axis.z * j->q;
                break;
            case IK_JOINT_FIXED:
            default:
                break;
        }
        poses[i] = t_compose(link, jt);
    }
}

/* ---- Aim2DOF analytical solver --------------------------------------- *
 * Same closed-form as fm_aim_to_point: rotate the world aim direction into
 * the base frame, then atan2 twice for pan/tilt. Yaw cancels because pan
 * is defined relative to the base-projected-forward direction. */
void ik_solve_aim_2dof(const IkAim2DOFReq *in, IkAim2DOFRes *out) {
    IkVec3 d_w = v3_sub(in->target, in->nozzle);
    float  len = v3_len(d_w);
    if (len < 1e-6f) {
        out->pan = out->tilt = 0.0f;
        out->status = -1;
        return;
    }
    float nx_w = d_w.x / len;
    float ny_w = d_w.y / len;
    float nz_w = d_w.z / len;

    /* world -> base: R_base^T = Ry(-pitch) * Rx(-roll) */
    float cr = cosf(in->base_roll),  sr = sinf(in->base_roll);
    float cp = cosf(in->base_pitch), sp = sinf(in->base_pitch);

    /* Rx(-roll) * (nx, ny, nz) */
    float x1 = nx_w;
    float y1 =  cr * ny_w + sr * nz_w;
    float z1 = -sr * ny_w + cr * nz_w;

    /* Ry(-pitch) * (x1, y1, z1) */
    float nx_b = cp * x1 - sp * z1;
    float ny_b =      y1;
    float nz_b = sp * x1 + cp * z1;

    out->pan    = atan2f(ny_b, nx_b);
    out->tilt   = atan2f(nz_b, sqrtf(nx_b * nx_b + ny_b * ny_b));
    out->status = 0;
}
