#ifndef ZEUS_POLICY_H
#define ZEUS_POLICY_H

#include <stdbool.h>
#include <stdint.h>
#include "vm.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Policy at the effect seam
 * -------------------------
 * The VM produces effects; it never decides whether they may touch the host.
 * A ZeusPolicy is held by the supervisor (never by the machine and never by a
 * snapshot), so denying an effect cannot be undone by restoring a file.
 *
 * Defaults are deny: a zeroed policy allows nothing. `STANDARD` is the
 * behaviour of `zeus run` — console and sockets — and deliberately excludes
 * the raw-packet family (`raw=false`). Raw sockets need `FULL`, which the CLI
 * only selects with an explicit --allow-raw.
 */

typedef enum {
    ZEUS_POLICY_ALLOW = 0,
    ZEUS_POLICY_DENY = 1
} ZeusPolicyVerdict;

typedef enum {
    ZEUS_PROFILE_DENY_ALL = 0,  /* nothing may reach the host */
    ZEUS_PROFILE_STANDARD,      /* console + sockets; raw denied */
    ZEUS_PROFILE_FULL           /* standard + raw-packet effects */
} ZeusPolicyProfile;

typedef struct {
    bool allow[ZEUS_EFFECT_KIND_COUNT];
    bool allow_raw;                     /* master switch for raw_* effects */
    bool ask[ZEUS_EFFECT_KIND_COUNT];   /* park the machine for approval */
    uint32_t max_effect_len;            /* 0 = no cap on buffer operands */
} ZeusPolicy;

/* Fill `p` with the named profile. */
void zeus_policy_init(ZeusPolicy *p, ZeusPolicyProfile profile);

void zeus_policy_allow(ZeusPolicy *p, ZeusEffectKind kind);
void zeus_policy_deny(ZeusPolicy *p, ZeusEffectKind kind);

/* Require approval for `kind`. */
void zeus_policy_ask(ZeusPolicy *p, ZeusEffectKind kind);

/* Require approval for every non-sink effect (PRINT/PRINTC are pure sinks and
 * are delivered without a human in the loop unless include_sinks is true). */
void zeus_policy_ask_all(ZeusPolicy *p, bool include_sinks);

/* The one policy decision. Checks the kind, the raw capability and the
 * buffer cap; returns ALLOW only if all three pass. */
ZeusPolicyVerdict zeus_policy_check(const ZeusPolicy *p, const ZeusEffect *eff);

bool zeus_policy_needs_approval(const ZeusPolicy *p, ZeusEffectKind kind);

/* Stable reason for a check result ("allow", "kind_not_allowed",
 * "raw_capability_disabled", "effect_too_large", "bad_effect"). */
const char *zeus_policy_reason(const ZeusPolicy *p, const ZeusEffect *eff);

bool zeus_effect_is_raw(ZeusEffectKind kind);

#ifdef __cplusplus
}
#endif

#endif /* ZEUS_POLICY_H */
