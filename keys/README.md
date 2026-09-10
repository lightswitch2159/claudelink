# MCUboot Signing Keys

## What is here

| File | Tracked in git? | Purpose |
|---|---|---|
| `mcuboot-xh601-priv.pem` | **No** (`.gitignore`) | MCUboot image signing key, XH601 |
| `mcuboot-xh_5102-priv.pem` | **No** (`.gitignore`) | MCUboot image signing key, XH_5102 |
| `mcuboot-xh601-pub.pem` | Yes | Public key, PEM (for `imgtool verify`) |
| `mcuboot-xh_5102-pub.pem` | Yes | Public key, PEM |
| `mcuboot-xh601-pub.c` | Yes | Public key, C array (for MCUboot `CONFIG_BOOT_SIGNATURE_KEY_FILE` builds) |
| `mcuboot-xh_5102-pub.c` | Yes | Public key, C array |

Curve: **NIST P-256 (prime256v1)**, MCUboot key type `ecdsa-p256`.

## These are NEW keys. The legacy keys were not reused.

The upstream repository committed four DFU private keys in plaintext:

```
subg_to_ble/project/boot/src/private_xh601.pem
subg_to_ble/project/boot/src/private_xh5102.pem
subg_to_ble/update/private_xh601.pem
subg_to_ble/update/private_xh_5102.pem
```

Anything ever signed with those keys must be treated as forgeable by anyone who
has cloned the repo. All four were deleted from this fork and none were reused.

Verified distinct (SHA-256 of the DER public key):

| Key | SHA-256 |
|---|---|
| legacy `private_xh601.pem` | `02350ff4725e9ca432f718449d125e8f2a5430122bac0a6188490c5be909eec2` |
| legacy `private_xh5102.pem` | `38df9ac630b3d9e102e1cf706f90fa972e92e313a6a5aee5c39e85d47d8cabf8` |
| new `mcuboot-xh601` | `20366a70a83438838e8b50f8d40ec9aa335f9d41a00104e5b1172bab6dbd1ac1` |
| new `mcuboot-xh_5102` | `864f04c79e8a9b29925c31d5536b80f25062d4986aabc24854dd3a2a11fd8712` |

Note that the legacy keys were *also* ECDSA P-256 — the curve was never the
problem. They are unusable here for two independent reasons: they are
compromised, and the nRF5 Secure DFU signature format (`nrfutil pkg generate`,
signature over a Nordic init packet) is not MCUboot's format (`imgtool sign`,
signature over an MCUboot TLV image header).

## How these were generated

```bash
imgtool keygen -k mcuboot-xh601-priv.pem -t ecdsa-p256
imgtool getpub -k mcuboot-xh601-priv.pem -l c > mcuboot-xh601-pub.c
openssl ec -in mcuboot-xh601-priv.pem -pubout -out mcuboot-xh601-pub.pem
```

Generated with `imgtool` 2.4.0 and functionally validated: a test payload was
signed with each private key and verified against the matching public key
(`imgtool verify` → "Image was correctly validated").

## Before this goes anywhere near production

These keys were generated on a development workstation for Phase 0 scaffolding.
They are **development keys**. Treat them as such:

1. **Move the private keys off the filesystem.** Production signing keys belong
   in an HSM or a managed secret store, with signing done by CI, not on a
   developer laptop.
2. **Generate separate production keys** in that secure environment. Do not
   promote these.
3. **Decide the key-per-board question.** Two keys are carried here only to
   mirror the legacy layout. One key for both boards is simpler to operate; two
   keys limit blast radius if one leaks. This is a product decision, not a
   technical one — resolve it in Phase 5.
4. **Write down the rotation and revocation plan.** MCUboot verifies against a
   public key compiled into the bootloader, so rotating a key requires a
   bootloader update. On a single-slot device that means SWD access. Plan for
   this before shipping, not after a leak.

Because this firmware sits in the communication path of an insulin pump, the
signing key is a patient-safety control, not just a supply-chain one.
