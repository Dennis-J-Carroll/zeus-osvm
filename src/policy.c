#include "policy.h"
#include <string.h>

bool zeus_effect_is_raw(ZeusEffectKind kind) {
    switch (kind) {
        case ZEUS_EFFECT_RAW_ALLOC:
        case ZEUS_EFFECT_RAW_FREE:
        case ZEUS_EFFECT_RAW_SET_PROTO:
        case ZEUS_EFFECT_RAW_SET_DST:
        case ZEUS_EFFECT_RAW_SET_SRC:
        case ZEUS_EFFECT_RAW_SET_PAYLOAD:
        case ZEUS_EFFECT_RAW_SEND:
        case ZEUS_EFFECT_RAW_RECV:
            return true;
        default:
            return false;
    }
}

/* Effects whose operands name a memory range. */
static bool has_buffer_range(ZeusEffectKind kind) {
    switch (kind) {
        case ZEUS_EFFECT_SEND:
        case ZEUS_EFFECT_RECV:
        case ZEUS_EFFECT_RAW_SET_PAYLOAD:
        case ZEUS_EFFECT_RAW_RECV:
            return true;
        default:
            return false;
    }
}

void zeus_policy_allow(ZeusPolicy *p, ZeusEffectKind kind) {
    if (!p || kind <= ZEUS_EFFECT_NONE || kind >= ZEUS_EFFECT_KIND_COUNT) return;
    p->allow[kind] = true;
}

void zeus_policy_deny(ZeusPolicy *p, ZeusEffectKind kind) {
    if (!p || kind <= ZEUS_EFFECT_NONE || kind >= ZEUS_EFFECT_KIND_COUNT) return;
    p->allow[kind] = false;
}

void zeus_policy_ask(ZeusPolicy *p, ZeusEffectKind kind) {
    if (!p || kind <= ZEUS_EFFECT_NONE || kind >= ZEUS_EFFECT_KIND_COUNT) return;
    p->ask[kind] = true;
}

void zeus_policy_init(ZeusPolicy *p, ZeusPolicyProfile profile) {
    if (!p) return;
    memset(p, 0, sizeof(*p));

    if (profile == ZEUS_PROFILE_DENY_ALL) return;

    zeus_policy_allow(p, ZEUS_EFFECT_PRINT);
    zeus_policy_allow(p, ZEUS_EFFECT_PRINTC);
    zeus_policy_allow(p, ZEUS_EFFECT_READ);
    zeus_policy_allow(p, ZEUS_EFFECT_SOCK_TCP);
    zeus_policy_allow(p, ZEUS_EFFECT_SOCK_UDP);
    zeus_policy_allow(p, ZEUS_EFFECT_BIND);
    zeus_policy_allow(p, ZEUS_EFFECT_LISTEN);
    zeus_policy_allow(p, ZEUS_EFFECT_ACCEPT);
    zeus_policy_allow(p, ZEUS_EFFECT_CONNECT);
    zeus_policy_allow(p, ZEUS_EFFECT_SEND);
    zeus_policy_allow(p, ZEUS_EFFECT_RECV);
    zeus_policy_allow(p, ZEUS_EFFECT_CLOSE);

    if (profile == ZEUS_PROFILE_FULL) {
        p->allow_raw = true;
        zeus_policy_allow(p, ZEUS_EFFECT_RAW_ALLOC);
        zeus_policy_allow(p, ZEUS_EFFECT_RAW_FREE);
        zeus_policy_allow(p, ZEUS_EFFECT_RAW_SET_PROTO);
        zeus_policy_allow(p, ZEUS_EFFECT_RAW_SET_DST);
        zeus_policy_allow(p, ZEUS_EFFECT_RAW_SET_SRC);
        zeus_policy_allow(p, ZEUS_EFFECT_RAW_SET_PAYLOAD);
        zeus_policy_allow(p, ZEUS_EFFECT_RAW_SEND);
        zeus_policy_allow(p, ZEUS_EFFECT_RAW_RECV);
    }
}

void zeus_policy_ask_all(ZeusPolicy *p, bool include_sinks) {
    if (!p) return;
    for (int k = (int)ZEUS_EFFECT_PRINT; k < (int)ZEUS_EFFECT_KIND_COUNT; k++) {
        ZeusEffectKind kind = (ZeusEffectKind)k;
        if (!include_sinks &&
            (kind == ZEUS_EFFECT_PRINT || kind == ZEUS_EFFECT_PRINTC)) {
            continue;
        }
        p->ask[k] = true;
    }
}

ZeusPolicyVerdict zeus_policy_check(const ZeusPolicy *p, const ZeusEffect *eff) {
    if (!p || !eff) return ZEUS_POLICY_DENY;
    if (eff->kind <= ZEUS_EFFECT_NONE || eff->kind >= ZEUS_EFFECT_KIND_COUNT) {
        return ZEUS_POLICY_DENY;
    }
    /* Report the capability gap first: "raw is off" is the useful reason for
     * a raw effect, even when its kind was never allowed either. */
    if (zeus_effect_is_raw(eff->kind) && !p->allow_raw) return ZEUS_POLICY_DENY;
    if (!p->allow[eff->kind]) return ZEUS_POLICY_DENY;
    if (p->max_effect_len > 0 && has_buffer_range(eff->kind) &&
        eff->len > p->max_effect_len) {
        return ZEUS_POLICY_DENY;
    }
    return ZEUS_POLICY_ALLOW;
}

bool zeus_policy_needs_approval(const ZeusPolicy *p, ZeusEffectKind kind) {
    if (!p || kind <= ZEUS_EFFECT_NONE || kind >= ZEUS_EFFECT_KIND_COUNT) {
        return false;
    }
    return p->ask[kind];
}

const char *zeus_policy_reason(const ZeusPolicy *p, const ZeusEffect *eff) {
    if (!p || !eff) return "bad_effect";
    if (eff->kind <= ZEUS_EFFECT_NONE || eff->kind >= ZEUS_EFFECT_KIND_COUNT) {
        return "bad_effect";
    }
    if (zeus_effect_is_raw(eff->kind) && !p->allow_raw) {
        return "raw_capability_disabled";
    }
    if (!p->allow[eff->kind]) return "kind_not_allowed";
    if (p->max_effect_len > 0 && has_buffer_range(eff->kind) &&
        eff->len > p->max_effect_len) {
        return "effect_too_large";
    }
    return "allow";
}
