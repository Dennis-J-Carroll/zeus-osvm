#define _GNU_SOURCE
#include "jail.h"

#include <errno.h>
#include <grp.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <netinet/in.h>
#endif

#define JAIL_NOBODY 65534u

static void jail_usage(void) {
    fprintf(stderr,
        "Usage:\n"
        "  zeus jail [--no-netns] [--no-seccomp] -- <cmd> [args...]\n"
        "  zeus jail --check        Probe namespace/seccomp support\n"
        "\n"
        "The jail drops host uid/gid when run as root, enters a fresh network\n"
        "namespace, and installs a seccomp filter that refuses raw sockets.\n"
        "It fails closed: if a layer cannot be applied, nothing is executed.\n");
}

static int write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int rc = (fputs(text, f) < 0) ? -1 : 0;
    if (fclose(f) != 0) rc = -1;
    return rc;
}

#ifdef __linux__

static int append_deny(struct sock_filter *f, int n, int max, int nr) {
    if (n + 2 > max) return -1;
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                          (uint32_t)nr, 0, 1);
    f[n++] = (struct sock_filter)BPF_STMT(
        BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
    return n;
}

/* Deny the host-reaching syscalls a sandboxed bytecode machine has no business
 * making, plus raw socket creation. Argument-filtered: socket(AF_PACKET, ...)
 * and socket(..., SOCK_RAW, ...) return EPERM; ordinary TCP/UDP sockets are
 * still allowed (the network namespace is what limits their reach). */
static int build_filter(struct sock_filter *f, int max) {
    int n = 0;
    if (max < 24) return -1;

    f[n++] = (struct sock_filter)BPF_STMT(
        BPF_LD | BPF_W | BPF_ABS,
        (uint32_t)offsetof(struct seccomp_data, nr));

#ifdef SYS_ptrace
    n = append_deny(f, n, max, SYS_ptrace);
    if (n < 0) return -1;
#endif
#ifdef SYS_mount
    n = append_deny(f, n, max, SYS_mount);
    if (n < 0) return -1;
#endif
#ifdef SYS_umount2
    n = append_deny(f, n, max, SYS_umount2);
    if (n < 0) return -1;
#endif
#ifdef SYS_pivot_root
    n = append_deny(f, n, max, SYS_pivot_root);
    if (n < 0) return -1;
#endif
#ifdef SYS_chroot
    n = append_deny(f, n, max, SYS_chroot);
    if (n < 0) return -1;
#endif
#ifdef SYS_setns
    n = append_deny(f, n, max, SYS_setns);
    if (n < 0) return -1;
#endif
#ifdef SYS_unshare
    n = append_deny(f, n, max, SYS_unshare);
    if (n < 0) return -1;
#endif
#ifdef SYS_bpf
    n = append_deny(f, n, max, SYS_bpf);
    if (n < 0) return -1;
#endif
#ifdef SYS_perf_event_open
    n = append_deny(f, n, max, SYS_perf_event_open);
    if (n < 0) return -1;
#endif
#ifdef SYS_keyctl
    n = append_deny(f, n, max, SYS_keyctl);
    if (n < 0) return -1;
#endif
#ifdef SYS_add_key
    n = append_deny(f, n, max, SYS_add_key);
    if (n < 0) return -1;
#endif
#ifdef SYS_request_key
    n = append_deny(f, n, max, SYS_request_key);
    if (n < 0) return -1;
#endif
#ifdef SYS_kexec_load
    n = append_deny(f, n, max, SYS_kexec_load);
    if (n < 0) return -1;
#endif
#ifdef SYS_init_module
    n = append_deny(f, n, max, SYS_init_module);
    if (n < 0) return -1;
#endif
#ifdef SYS_finit_module
    n = append_deny(f, n, max, SYS_finit_module);
    if (n < 0) return -1;
#endif
#ifdef SYS_delete_module
    n = append_deny(f, n, max, SYS_delete_module);
    if (n < 0) return -1;
#endif
#ifdef SYS_reboot
    n = append_deny(f, n, max, SYS_reboot);
    if (n < 0) return -1;
#endif
#ifdef SYS_swapon
    n = append_deny(f, n, max, SYS_swapon);
    if (n < 0) return -1;
#endif
#ifdef SYS_swapoff
    n = append_deny(f, n, max, SYS_swapoff);
    if (n < 0) return -1;
#endif
#ifdef SYS_syslog
    n = append_deny(f, n, max, SYS_syslog);
    if (n < 0) return -1;
#endif
#ifdef SYS_open_by_handle_at
    n = append_deny(f, n, max, SYS_open_by_handle_at);
    if (n < 0) return -1;
#endif
#ifdef SYS_name_to_handle_at
    n = append_deny(f, n, max, SYS_name_to_handle_at);
    if (n < 0) return -1;
#endif
#ifdef SYS_process_vm_readv
    n = append_deny(f, n, max, SYS_process_vm_readv);
    if (n < 0) return -1;
#endif
#ifdef SYS_process_vm_writev
    n = append_deny(f, n, max, SYS_process_vm_writev);
    if (n < 0) return -1;
#endif
#ifdef SYS_userfaultfd
    n = append_deny(f, n, max, SYS_userfaultfd);
    if (n < 0) return -1;
#endif

#ifdef SYS_socket
    /* if (nr == socket) { check domain/type } else skip the block */
    if (n + 8 > max) return -1;
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                          (uint32_t)SYS_socket, 0, 7);
    f[n++] = (struct sock_filter)BPF_STMT(
        BPF_LD | BPF_W | BPF_ABS,
        (uint32_t)offsetof(struct seccomp_data, args[0]));
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                                          (uint32_t)AF_PACKET, 0, 1);
    f[n++] = (struct sock_filter)BPF_STMT(
        BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
    f[n++] = (struct sock_filter)BPF_STMT(
        BPF_LD | BPF_W | BPF_ABS,
        (uint32_t)offsetof(struct seccomp_data, args[1]));
    f[n++] = (struct sock_filter)BPF_STMT(BPF_ALU | BPF_AND | BPF_K, 0x0Fu);
    f[n++] = (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 3u, 0, 1);
    f[n++] = (struct sock_filter)BPF_STMT(
        BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
#endif

    if (n + 1 > max) return -1;
    f[n++] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    return n;
}

static int apply_seccomp(void) {
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        fprintf(stderr, "jail: PR_SET_NO_NEW_PRIVS failed: %s\n", strerror(errno));
        return -1;
    }

    struct sock_filter filter[256];
    int n = build_filter(filter, (int)(sizeof(filter) / sizeof(filter[0])));
    if (n < 0) {
        fprintf(stderr, "jail: seccomp filter does not fit\n");
        return -1;
    }

    struct sock_fprog prog;
    prog.len = (unsigned short)n;
    prog.filter = filter;

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) {
        fprintf(stderr, "jail: seccomp filter rejected: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int enter_namespaces(bool netns, bool *mapped_root) {
    *mapped_root = false;
    if (!netns) return 0;

    bool host_root = (geteuid() == 0);
    int flags = CLONE_NEWNET | (host_root ? 0 : CLONE_NEWUSER);

    if (unshare(flags) != 0) {
        fprintf(stderr, "jail: unshare(%s) failed: %s\n",
                host_root ? "CLONE_NEWNET" : "CLONE_NEWUSER|CLONE_NEWNET",
                strerror(errno));
        return -1;
    }

    if (!host_root) {
        char map[64];

        if (write_text("/proc/self/setgroups", "deny") != 0) {
            fprintf(stderr, "jail: cannot write /proc/self/setgroups: %s\n",
                    strerror(errno));
            return -1;
        }
        snprintf(map, sizeof(map), "0 %u 1\n", (unsigned)getuid());
        if (write_text("/proc/self/uid_map", map) != 0) {
            fprintf(stderr, "jail: cannot write uid_map: %s\n", strerror(errno));
            return -1;
        }
        snprintf(map, sizeof(map), "0 %u 1\n", (unsigned)getgid());
        if (write_text("/proc/self/gid_map", map) != 0) {
            fprintf(stderr, "jail: cannot write gid_map: %s\n", strerror(errno));
            return -1;
        }
        *mapped_root = true;
    }
    return 0;
}

static int drop_privileges(void) {
    if (geteuid() != 0) return 0;


    if (setgroups(0, NULL) != 0) {
        fprintf(stderr, "jail: setgroups failed: %s\n", strerror(errno));
        return -1;
    }
    if (setgid((gid_t)JAIL_NOBODY) != 0) {
        fprintf(stderr, "jail: setgid failed: %s\n", strerror(errno));
        return -1;
    }
    if (setuid((uid_t)JAIL_NOBODY) != 0) {
        fprintf(stderr, "jail: setuid failed: %s\n", strerror(errno));
        return -1;
    }
    if (geteuid() == 0) {
        fprintf(stderr, "jail: privilege drop did not take effect\n");
        return -1;
    }
    return 0;
}

/* Probe one layer in a throwaway child. Returns 0 on success, and writes the
 * observed failure to `why`. Keeping the layers independent means a host that
 * forbids user namespaces still reports whether the seccomp filter works. */
static int probe_child(int which, char *why, size_t why_len) {
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(why, why_len, "fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        if (which == 0) {
            bool mapped = false;
            if (enter_namespaces(true, &mapped) != 0) _exit(10);
        }
        if (apply_seccomp() != 0) _exit(11);

        int tcp = socket(AF_INET, SOCK_STREAM, 0);
        int raw = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
        int raw_errno = errno;

        if (tcp < 0) _exit(12);
        close(tcp);
        if (raw >= 0) _exit(13);          /* raw must be refused */
        if (raw_errno != EPERM) _exit(14);
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        snprintf(why, why_len, "waitpid failed: %s", strerror(errno));
        return -1;
    }

    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    switch (code) {
        case 0:  return 0;
        case 10: snprintf(why, why_len, "unshare(CLONE_NEWUSER|CLONE_NEWNET) refused"); break;
        case 11: snprintf(why, why_len, "seccomp filter refused"); break;
        case 12: snprintf(why, why_len, "ordinary TCP socket failed"); break;
        case 13: snprintf(why, why_len, "raw socket was NOT denied"); break;
        case 14: snprintf(why, why_len, "raw socket failed with the wrong errno"); break;
        default: snprintf(why, why_len, "probe exit %d", code); break;
    }
    return -1;
}

static int jail_probe(void) {
    printf("zeus jail probe\n");

    char ns_why[128] = {0};
    char sc_why[128] = {0};
    int ns = probe_child(0, ns_why, sizeof(ns_why));
    int sc = probe_child(1, sc_why, sizeof(sc_why));

    printf("  namespaces: %s\n", ns == 0 ? "ok" : ns_why);
    printf("  seccomp:    %s\n", sc == 0 ? "ok (raw sockets denied with EPERM)" : sc_why);

    if (ns == 0 && sc == 0) {
        printf("  verdict:    jail available\n");
        return 0;
    }
    printf("  verdict:    jail NOT available (a layer failed; it fails closed)\n");
    return 1;
}

#else /* !__linux__ */

static int enter_namespaces(bool netns, bool *mapped_root) {
    (void)netns;
    *mapped_root = false;
    fprintf(stderr, "jail: not supported on this platform\n");
    return -1;
}

static int drop_privileges(void) { return 0; }

static int jail_probe(void) {
    printf("zeus jail probe\n  verdict: jail NOT available (non-Linux host)\n");
    return 1;
}

#endif /* __linux__ */

int zeus_cmd_jail(int argc, char **argv) {
    bool netns = true;
    bool seccomp = true;
    bool check = false;
    int i = 0;

    for (; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else if (strcmp(argv[i], "--no-netns") == 0) {
            netns = false;
        } else if (strcmp(argv[i], "--no-seccomp") == 0) {
            seccomp = false;
        } else if (strcmp(argv[i], "--check") == 0) {
            check = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            jail_usage();
            return 0;
        } else {
            break;   /* first non-flag is the command */
        }
    }

    if (check) return jail_probe();

    if (i >= argc) {
        jail_usage();
        return 1;
    }

    bool mapped_root = false;
    if (enter_namespaces(netns, &mapped_root) != 0) return 1;

    /* Only a host-root process can (and must) drop to nobody; inside an
     * unprivileged user namespace we are already unprivileged on the host. */
    if (!mapped_root && drop_privileges() != 0) return 1;

#ifdef __linux__
    if (seccomp && apply_seccomp() != 0) return 1;
#else
    (void)seccomp;
#endif

    execvp(argv[i], &argv[i]);
    fprintf(stderr, "jail: exec '%s' failed: %s\n", argv[i], strerror(errno));
    return 1;
}
