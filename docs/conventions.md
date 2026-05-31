# Conventions

How code in this repo is shaped. Short and practical, matching
[CONTRIBUTING.md](../CONTRIBUTING.md).

These are extracted from files the existing codebase already does
well — `include/vm/vm_ecall.h`, `src/mgapi/cart_volume.c`,
`include/audio/audio_mixer.h`, `src/containers/dlist.c`. If you're
unsure how to lay something out, those four are the reference shape.

## Error returns

Pick the return type that *honestly describes the failure modes*.
Don't pick by default; pick by what's actually true about the
function. Four shapes are legitimate, each for a specific situation:

- **`int 0/-errno`** — when the function has **≥2 callable failure
  modes the caller wants to distinguish**. `mgapi_audio_init` can
  fail `-EINVAL` (bad arg), `-ENOMEM` (alloc), `-EALREADY` (already
  up); the caller acts differently on each. Use the negative
  Linux errno set (`-EINVAL`, `-ENOMEM`, `-EALREADY`, `-EIO`,
  `-EAGAIN`, `-EBUSY`). One comparison `(rc < 0)` detects failure
  and `rc` names the reason. This is the default for system-init
  and ecall-handler shapes.

- **`bool`** — when there is exactly one failure mode and the
  caller will never want to discriminate further. `dlist_push`
  fails only because the pool is full; `ring_buffer_pop` fails
  only because the buffer is empty. Converting these to `int`
  pretends to add information that isn't there. Containers and
  one-bit predicates are the right home for `bool`.

- **`T *` (or NULL)** — for allocator-shaped APIs that return
  a fresh handle. `audio_service_create`, `mixer_create`. The
  caller wants the handle on success, so the dual return is
  honest. Don't use this shape for "init this caller-owned struct"
  (that's `int 0/-errno`).

- **Custom result enum** — when you have **≥4 named outcomes**
  that the caller really branches on. `TrashfsResult` qualifies
  (`OK / IO / FULL / NOT_FOUND / CORRUPT`). A two- or three-state
  enum is noise wearing a hat; collapse to `bool` or `int`.

- **`void`** — only for "this physically cannot fail":
  `vm_reset`, `cart_window_post_pads`, pure arithmetic or
  memset-shaped operations. If you find yourself silently
  swallowing an error inside a `void` function, change the
  return type.

Never silently drop an error. If a sub-call returns negative,
either return it, log it, or fail loudly. The `-Werror` build
catches unused return values for functions marked `[[nodiscard]]`;
mark fallible APIs that way when you write them.

### What this means for existing code

Don't migrate `src/containers/*` to `int` — they're correctly
`bool` because there's one failure mode each. Don't migrate
`audio_service_create` to `int` — the pointer return is honest
(you want the handle). The mgapi modules already use the `int`
shape because they genuinely have multiple failure modes.

If you're adding a new function, ask which of the five shapes
above truthfully matches its failure mode, and pick that. Don't
adopt `int` because the doc said so — adopt it when the function
actually has multiple things that can go wrong.

## Naming

Functions are **`<module>_<verb>[_<noun>]`**. The module prefix is
short, lowercase, and matches the filename:

| Module | Prefix | File |
|---|---|---|
| audio mixer | `mixer_` | `src/audio/audio_mixer.c` |
| audio arbiter | `audio_arbiter_` | `src/audio/audio_arbiter.c` |
| VM ecall router | `vm_ecall_` | `src/vm/vm_ecall.c` |
| trashfs | `trashfs_` | `src/storage/trashfs.c` |
| mgapi audio subsystem | `mgapi_audio_` | `src/mgapi/audio_init.c` |

**Public symbols (anything declared in a `.h`) carry the module
prefix.** That's where the prefix earns its keep — cross-file calls
need disambiguation. Static helpers inside a `.c` file don't have
to be prefixed because the file already scopes them, but they
*should* be prefixed when the bare name is generic enough to be
ambiguous when someone greps:

```c
/* fine — name is specific to this file's domain */
static int tile_pixel(const Tile *t, int x, int y);
static const FreeNode *node_at(const DList *l, unsigned i);

/* prefix these — bare names would collide with other modules */
static void mgapi_audio_drain_ring(...);  /* not just drain_ring */
static int  vm_ecall_check_args(...);     /* not just check_args */
```

Rule of thumb: if you can grep the bare name across the project
and get one hit, leave it bare. If you'd get hits in five files
that each have their own version, prefix it.

### Pair verbs by allocation model

Pick the pair by *what cleanup actually does*. Three shapes coexist
because they describe different things honestly:

| Init shape | Cleanup pair | When |
|---|---|---|
| `T *T_create(...)` | `void T_destroy(T *)` | API allocates the `T` itself plus internals |
| `int T_init(T *, ...)` | `void T_destroy(T *)` | Caller owns the struct; init still allocates internal arrays / acquires OS handles |
| `int T_init(T *, ...)` | `void T_shutdown(T *)` | Caller owns the struct; init only writes fields. Nothing was allocated, nothing needs freeing |

`_destroy` is the right pair name **whenever init acquires anything
the cleanup must release** — heap, OS handles, refcounts. The
distinction between `create/destroy` and `init/destroy` is just
where the struct itself lives. `_shutdown` is the right pair name
when init only fills in fields of caller-owned storage; cleanup is
an idempotent state reset rather than a release.

Concrete examples in the tree:

- `audio_service_create / _destroy` — API owns the struct, init
  mallocs internals. Both ends free heap. Correct.
- `audio_pool_init(p, region, size) / _destroy(p)` — caller owns
  `AudioPool *p`, but init mallocs metadata arrays (owner, next,
  used_map). `_destroy` is honest: it frees those. Correct.
- `mgapi_audio_init / _shutdown` — caller-owned static singleton
  state; init wires pointers into existing storage; shutdown
  zeros and clears alive flag. Correct.
- `cart_window_init / _shutdown` — same pattern, all static
  storage. Correct.

The smell test: **if you can call `_shutdown` and skip it without
leaking, it's a shutdown. If skipping cleanup leaks heap or leaves
a socket open, it should be `_destroy`.**

## The subsystem-owner pattern

The four mgapi inits (`audio_init`, `cart_volume`, `l2_init`,
`vm_init`) all follow the same shape. New owner-style modules should
match it. The canonical skeleton:

```c
/* foo_init.h */
int  mgapi_foo_init(void *region, size_t region_size);
void mgapi_foo_shutdown(void);

typedef struct { uint32_t a; uint32_t b; /* ... */ } MgapiFooStats;
int  mgapi_foo_stats(MgapiFooStats *out);

/* foo_init.c */
static Foo    *g_foo;
static int     g_alive;

int mgapi_foo_init(void *region, size_t region_size) {
    if (g_alive) return -EALREADY;
    if (!region || region_size == 0) return -EINVAL;
    g_foo = foo_create(region, region_size);
    if (!g_foo) return -ENOMEM;
    g_alive = 1;
    return 0;
}

void mgapi_foo_shutdown(void) {
    if (!g_alive) return;
    foo_destroy(g_foo);
    g_foo = NULL;
    g_alive = 0;
}

int mgapi_foo_stats(MgapiFooStats *out) {
    if (!out) return -EINVAL;
    if (!g_alive) return -EAGAIN;
    /* fill out */
    return 0;
}
```

Notes on the shape:

- **`int g_alive` not a typedef'd bool.** Cheap to test, no header
  needed, matches the existing files.
- **Init takes a `(region, size)` pair**, not an opaque config
  struct, when the module is wrapping memory. The caller owns the
  region. The module never `malloc`s on the M7 — it composes onto
  what the caller hands it.
- **Shutdown is idempotent.** Calling it on a never-init module is a
  no-op, not a crash.
- **Stats use a versioned output struct.** Callers pass a pointer;
  the module fills it. Add new fields at the end and bump a version
  byte so an older caller's smaller struct still works.

The structure exists so that the M7 firmware boot sequence is
shaped like one big linear chain of `mgapi_X_init(...)` calls in
known order, and any of them can fail with a single `int` the
embedder can log.

## Headers

Public headers under `include/` follow a fixed order:

1. Top-of-file prose comment block — what this header is for, who
   uses it, what the calling contract is. See `vm_ecall.h` lines
   1-100 for the right amount of detail.
2. `#include`s of standard headers only. Pull project headers from
   the `.c` file, not the header, unless a type from another header
   appears in a function signature here.
3. `#define` constants and sentinel values.
4. `enum` declarations.
5. `typedef struct` declarations.
6. Function prototypes, **clustered by lifecycle** (`Lifecycle`,
   `Operations`, `Introspection`, `Diagnostics`), each cluster
   preceded by a divider comment if there's more than a handful of
   functions.

Function clusters in `vm_ecall.h` and `audio_mixer.h` are the
reference layout. They're scannable in 10 seconds because each
divider says what's coming.

If you find yourself adding a type just so the header doesn't pull
in a heavyweight include, **forward-declare it** and keep the
include in the `.c`:

```c
/* in foo.h */
struct AudioPool;                       /* forward-declare */
int foo_use_pool(struct AudioPool *p);

/* in foo.c */
#include "audio/audio_pool.h"           /* actual definition lives here */
```

## Comments

Three kinds of comment, each with a target density:

- **Doc comments cover every public symbol, either individually or
  as a cluster.** Two-to-five sentences. Says what the function
  does, what the inputs and return mean, and one thing about *why*
  the API is shaped that way if the shape is non-obvious.

  When several related functions share the same contract (e.g.
  "all of these silently no-op on out-of-range indices",
  "introspection — valid after mount"), document them as a cluster
  with one `===` section header rather than repeating the same
  caveat on each prototype. `include/audio/audio_mixer.h` "Channel
  control" and `include/storage/trashfs.h` "Introspection" are the
  model — both group 4-6 functions under one shared comment.

  Per-function comments are right when the functions in the cluster
  *don't* actually share the same contract — when one of them has
  a different failure mode or side effect, it gets its own block.
  Don't force-cluster functions just to save typing.
- **Inline comment when control flow is non-obvious.** Explain
  *why*, not *what*. `cart_window_read`'s side-effect cases are
  good examples — each `if` branch has one line saying "this
  address is read-as-signal, value is don't-care."
- **TODO comments say what condition makes them actionable.**
  `/* TODO: revisit once the bsnes mapper supports the audio source */`
  is better than a bare `TODO`. Don't ceremony with dates or
  stage numbers — `git blame` tells you when it was written. The
  condition is what's actually useful: it tells the next reader
  whether the TODO is still pending or whether the condition has
  already happened and the comment is stale.

What not to comment:

- Don't paraphrase the code below. `i = i + 1; /* increment i */`
  costs a line and tells no one anything.
- Don't comment what a function name already says. The doc
  comment is for *contract* and *constraints*, not name expansion.

## File-size soft caps

- **>800 lines**: someone should pause and ask if the file's doing
  too much. Often the answer is "no" — `audio_mixer.c` is 1k
  because it's one coherent thing — but the question is worth
  asking out loud.
- **>1500 lines**: it almost certainly needs decomposing.
  `examples/05_shell/host.c` (2k) is the example of a file that
  grew past the natural seams and could use a follow-up pass.

The caps aren't enforced by the build; they're a smell heuristic.
A 200-line file with three nested levels of state machine is worse
than a 1200-line file that's a linear catalogue of independent
functions.

## When in doubt

Pick the convention from the file closest to what you're writing:

- Writing a new mgapi subsystem? Match `src/mgapi/cart_volume.c`.
- Writing a new VM ecall handler? Match `src/vm/vm_ecall_handlers.c`.
- Writing a new container? Match `src/containers/dlist.c`.
- Writing a new public header? Match `include/vm/vm_ecall.h` or
  `include/audio/audio_mixer.h`.

If the existing files disagree, the more recent one (the mgapi
modules) is the canonical direction.
