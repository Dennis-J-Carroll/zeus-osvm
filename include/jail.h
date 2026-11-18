#ifndef ZEUS_JAIL_H
#define ZEUS_JAIL_H

#ifdef __cplusplus
extern "C" {
#endif

/* `zeus jail [--no-netns] [--no-seccomp] [--check] -- <cmd> [args...]`
 *
 * Runs a command in a network namespace, with host uid/gid dropped when
 * privileged, and under a seccomp filter that denies raw-socket creation.
 * This is what makes "raw=false" non-bypassable: the policy is cooperative,
 * the syscall filter is not. Returns only on failure (exec replaces us). */
int zeus_cmd_jail(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* ZEUS_JAIL_H */
