/* ============================================================
 *  ik.h — portable inverse-kinematics library for the microgarbage line.
 *
 *  Two stacked layers, both small and embedded-friendly:
 *
 *    ANALYTICAL SOLVERS -- O(1), deterministic, no iteration. One typed
 *    request/result pair per topology. Hard-real-time safe.
 *      ik_solve_aim_2dof   -- pan + tilt to aim at a 3D point. The fire-
 *                             monitor gimbal. Standard pan-tilt convention:
 *                             pan about base Z, then tilt about the pan-
 *                             rotated base Y, nozzle along +X at zero.
 *      (planned)
 *        ik_solve_planar_3r       -- 3R planar arm to reach a 2D point
 *        ik_solve_spherical_wrist -- 3-DOF wrist orientation
 *      Each will land alongside Aim2DOF as products need them.
 *
 *    CHAIN ABSTRACTION  -- describe an arbitrary serial chain (linked
 *    nodes from a root) and run forward kinematics. The eventual numerical
 *    IK (damped-least-squares Jacobian) plugs in on top of this; for now
 *    FK is what callers get -- enough to verify analytical solutions and
 *    drive visualisations.
 *
 *  Single precision, no heap, deterministic compute. Quaternions internally
 *  for orientation (no gimbal lock in the math even when the mechanism has
 *  it).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MG_IK_H
#define MG_IK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- math primitives -------------------------------------------------- */

typedef struct { float x, y, z; }       IkVec3;
typedef struct { float w, x, y, z; }    IkQuat;     /* unit quaternion */
typedef struct { IkVec3 t; IkQuat r; }  IkTransform;/* rotation then translation */

/* ---- chain definition (linked nodes from a root) --------------------- */

typedef enum {
    IK_JOINT_FIXED      = 0,    /* rigid offset, no DoF                          */
    IK_JOINT_REVOLUTE   = 1,    /* rotation about `axis` (radians)               */
    IK_JOINT_PRISMATIC  = 2,    /* translation along `axis` (metres)             */
} IkJointType;

typedef struct {
    IkJointType type;
    IkVec3      axis;           /* unit axis expressed in this node's local frame */
    float       q_min, q_max;   /* joint limits (rad or m); both 0 = unlimited    */
    float       q;              /* current value (input for FK; output for IK)    */
} IkJoint;

typedef struct {
    int         parent;         /* index of parent node, or -1 for root           */
    IkTransform offset;         /* rigid offset from parent's frame to this joint */
    IkJoint     joint;
} IkNode;

#define IK_MAX_NODES 16

typedef struct {
    IkNode nodes[IK_MAX_NODES];
    int    count;
    int    end_effector;        /* index of the tool-tip node; -1 = unset */
} IkChain;

/* zero a chain (count=0, end_effector=-1). Always call this first. */
void ik_chain_init(IkChain *c);
/* append a node; returns the new node's index, or -1 if the chain is full.
 * `parent` is the index of the parent node (use -1 for root). `offset` is the
 * rigid transform from the parent's frame to this joint's frame; the joint's
 * motion is applied after the offset. */
int  ik_chain_add(IkChain *c, int parent, IkTransform offset, IkJoint joint);
/* mark which node is the tool tip (the end effector). Used by IK solvers
 * that operate on the chain abstraction. */
void ik_chain_set_end(IkChain *c, int node_index);
/* forward kinematics: writes each node's world-frame transform into poses[].
 * `poses` must have at least c->count entries. `base` is the chain root's
 * world pose (use the identity transform if the root is at the world origin). */
void ik_chain_fk(const IkChain *c, IkTransform base, IkTransform *poses);

/* ---- Aim2DOF: pan + tilt to aim at a point ---------------------------- *
 *
 *  Convention (world frame): X-forward, Y-left, Z-up, gravity along -Z.
 *  Base orientation builds as R_base = Rx(roll) * Ry(pitch). The gimbal:
 *  pan about base Z, then tilt about the pan-rotated base Y. Default aim
 *  (pan=0, tilt=0) is base +X. Yaw cancels and is not needed for the math.
 *
 *  Sign convention:
 *      base_roll  > 0  -> right side down
 *      base_pitch > 0  -> nose down
 *      pan        > 0  -> nozzle rotates from base +X toward +Y (left)
 *      tilt       > 0  -> nozzle elevation increases (up in world)
 *
 *  Status:
 *      0  -> OK
 *     -1  -> degenerate: |target - nozzle| ~= 0 (no aim direction)
 *
 *  If your hardware has flipped actuator polarity or a zero offset, apply
 *  those at the boundary (caller side) -- keep the geometric math here pure.
 */
typedef struct {
    float  base_roll;       /* radians */
    float  base_pitch;      /* radians */
    IkVec3 nozzle;          /* world position of the pan/tilt pivot */
    IkVec3 target;          /* world position of the aim point      */
} IkAim2DOFReq;

typedef struct {
    float pan;              /* azimuth (radians)   */
    float tilt;             /* elevation (radians) */
    int   status;
} IkAim2DOFRes;

void ik_solve_aim_2dof(const IkAim2DOFReq *in, IkAim2DOFRes *out);

#ifdef __cplusplus
}
#endif

#endif /* MG_IK_H */
