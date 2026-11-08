/*
 * Provenance (vendored third-party code):
 *   TweetNaCl — "A crypto library in 100 tweets"
 *   Authors: Daniel J. Bernstein, Tanja Lange, Peter Schwabe (et al.)
 *   Source:  https://tweetnacl.cr.yp.to/ (mirror used for this vendored copy:
 *            https://github.com/sbp/tweetnacl-tools, tweetnacl.c/tweetnacl.h)
 *   License: PUBLIC DOMAIN (authors explicitly released TweetNaCl into the
 *            public domain; see the TweetNaCl paper, tweetnacl-20140917.pdf)
 *   Copy is byte-identical to the upstream 20140917 release (809 lines /
 *   16637 bytes) — do not edit. If changes are ever needed, fork and
 *   document the delta here instead of silently diverging.
 *
 * AetherOS uses exactly two primitives from this library:
 *   crypto_sign_keypair()  — host-side .aepkg release-key generation
 *   crypto_sign()          — host-side .aepkg signing (aether-pkg create)
 *   crypto_sign_open()     — guest-side .aepkg signature verification
 * Everything else is compiled but unused (static build, dead-code eliminated
 * by the linker only with -ffunction-sections; we accept the small size cost
 * over patching upstream).
 *
 * NOTE: tweetnacl.c declares `extern void randombytes(u8 *, u64)` — the
 * application must provide it. aether-pkg.c provides an implementation
 * reading /dev/urandom (used only by keypair generation on the host).
 */
