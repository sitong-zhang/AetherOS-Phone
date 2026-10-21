/*
 * aether-hello.c — minimal installed-application probe for the .aepkg
 * runtime path (P2-11).
 *
 * This is the app that gets packaged as hello.aepkg and installed at
 * runtime by CI. It has no dependencies beyond libc: it writes a line to
 * stdout every second so the test can prove (a) the installed binary
 * actually executes and (b) it does so under the sandbox (uid 1000,
 * CapEff=0) applied by aether-sandbox-run before exec.
 *
 * It exits after AETHER_HELLO_TICKS seconds so a forgotten background
 * instance cannot linger forever.
 */
#include <stdio.h>
#include <unistd.h>

#define AETHER_HELLO_TICKS 600

int main(void)
{
    for (int i = 1; i <= AETHER_HELLO_TICKS; i++) {
        printf("aether-hello: tick %d/%d\n", i, AETHER_HELLO_TICKS);
        fflush(stdout);
        sleep(1);
    }
    return 0;
}
