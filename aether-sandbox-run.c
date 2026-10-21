/*
 * aether-sandbox-run.c — standalone sandbox launcher (Application Runtime
 * entry point for .aepkg-installed apps).
 *
 * aether-init applies aether_apply_sandbox() to services marked
 * `sandbox yes` in its manifest. Installed applications need the same
 * treatment but are spawned from the shell / app runtime, not from the
 * service manifest — this binary is that path: it applies the identical
 * sandbox (uid/gid 1000, capability drop, seccomp allow-list,
 * NoNewPrivs, private mount ns) and then execs the target.
 *
 * Unlike aether-init's best-effort policy (a service whose sandbox fails
 * still execs, with the failure logged), this launcher treats sandbox
 * failure as fatal: its entire purpose is to run things confined, so
 * running unconfined would be a silent lie.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>

#include "aether-sandbox.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: aether-sandbox-run <cmd> [args...]\n"
                        "  applies the AetherOS service sandbox, then execs <cmd>\n");
        return 2;
    }
    if (aether_apply_sandbox(argv[1]) != 0) {
        fprintf(stderr, "aether-sandbox-run: sandbox application failed — "
                        "refusing to exec %s unconfined\n", argv[1]);
        return 1;
    }
    execvp(argv[1], &argv[1]);
    perror("aether-sandbox-run: execvp");
    return 1;
}
