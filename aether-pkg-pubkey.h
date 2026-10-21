/*
 * aether-pkg-pubkey.h — AetherOS .aepkg release public key (ed25519, 32 bytes).
 *
 * GENERATED FILE — do not edit by hand. Regenerate with:
 *   aether-pkg keygen <priv.hex> <pub.hex>
 * then paste the pub.hex content below (and embed the new value here).
 *
 * Provenance: this is the PUBLIC half of the release signing keypair. The
 * private half lives host-side only ($AETHER_PKG_PRIVKEY for the prototype;
 * see README-userland.md §十六 for the key-management honesty boundary).
 * Committing the public key is safe by definition — it can only verify.
 *
 * Generated: 2026-10-18 by `aether-pkg keygen` (prototype release key).
 */
#ifndef AETHER_PKG_PUBKEY_H
#define AETHER_PKG_PUBKEY_H

static const unsigned char AETHER_PKG_PUBKEY[32] = {
    0xa0, 0x2b, 0xd5, 0x4a, 0xc2, 0xa7, 0x98, 0xa4,
    0x8d, 0x03, 0x83, 0x47, 0xe7, 0x7b, 0x42, 0x0b,
    0x56, 0x8e, 0xc9, 0x5d, 0x1d, 0x1b, 0x00, 0x26,
    0xc6, 0x45, 0xe4, 0xa9, 0xd4, 0xa6, 0x84, 0x41,
};

#endif /* AETHER_PKG_PUBKEY_H */
