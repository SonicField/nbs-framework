# Feature Request: Human Message Authentication

**Date:** 2026-02-27
**Status:** Design phase — needs more work before implementation
**Priority:** High — impersonation via hallucination is a live problem

## Problem

Any agent can post to chat as any handle, including `alex`. There is no mechanism to distinguish a genuine human message from an agent hallucinating or confusing itself into posting as the human. The anti-hallucination rule ("quote it with timestamp") is a social convention that doesn't prevent impersonation — it only makes it detectable after the fact, and only if someone checks.

Observed incidents:
- Agents reading chat backlog and reproducing coordinator directives in different languages, attributed to handles they don't own
- Sidecar standup messages posted under agent handles (fixed in `838b199`)
- No verified case of deliberate impersonation, but the mechanism is trivially available

## Threat Model

**In scope:** A hallucinating agent accidentally posts as the human handle. This should fail visibly — the message appears unsigned and every participant can see it's unverified.

**Out of scope:** A coordinated attack where an agent deliberately writes exploit code to extract the signing key from process memory, forge a valid signature, and impersonate the human. We accept this is possible for a same-UID adversary and don't try to prevent it.

**The line:** A simple `nbs-chat send .nbs/chat/live.chat alex "do X"` must produce a visibly unsigned message. Extracting the key from `/proc/<pid>/mem` is accepted as beyond the hallucination threat model.

## Proposed Mechanism: Passphrase-Derived Message Signing

### Startup

1. `nbs-chat-terminal` prompts for a short passphrase/PIN on startup
2. Derives an HMAC-SHA256 key from the passphrase (e.g. via PBKDF2 or HKDF)
3. Key exists only in process memory — never written to disk, never in environment variables, never in a file

### Signing

4. Each message sent from the terminal is signed: `HMAC-SHA256(key, timestamp || handle || message)`
5. The signature is embedded in the chat wire format (base64 line) — needs wire format extension
6. The terminal displays a verified marker on messages with valid signatures when reading

### Verification

7. `nbs-chat read` and `nbs-chat-terminal` display e.g. `[✓]` next to messages with valid HMACs
8. Messages without signatures or with invalid signatures display normally but without the marker
9. Agents see the marker in chat output and learn that unsigned `alex` messages are suspect

### Key Properties

- The passphrase is in the human's head — no agent can access it
- The derived key is in terminal process memory only — requires `/proc/<pid>/mem` reading to extract
- Different passphrase each session if desired (though same passphrase works too)
- No infrastructure dependencies (no Kerberos, no PKI, no hardware tokens)

## Open Design Questions

1. **Wire format extension.** How to embed the signature in the base64 chat line format without breaking backward compatibility. Options: (a) append signature as a separate field after the message, (b) add a signature line after each signed message line, (c) use a different encoding prefix for signed messages.

2. **Key distribution for verification.** The terminal holds the signing key. How do `nbs-chat read` (CLI) and other terminals verify signatures? Options: (a) the terminal writes a verification-only key (HMAC is symmetric, so this is the same key — problem), (b) switch to asymmetric signatures (Ed25519 — private key in memory, public key on disk), (c) only the terminal that sent the message can verify it (others just see "signed by someone who knew the passphrase").

3. **Asymmetric vs symmetric.** HMAC-SHA256 means the verification key IS the signing key. If we write it anywhere for other processes to verify, agents can read it and forge. Ed25519 solves this: private key in memory, public key on disk. Agents can verify but not forge. This is probably the right answer but adds complexity (need libsodium or similar).

4. **Multiple humans.** If two humans use the system, they need different key pairs. The public key file becomes a registry of trusted signers.

5. **Session continuity.** If the terminal crashes and restarts, the human re-enters the passphrase. Old messages are still verifiable if the same passphrase is used. Different passphrase = old messages become unverifiable (or we need to support multiple keys).

6. **Passphrase UX.** How often to prompt? Once at startup? Periodically? On each message? Once at startup is the minimum friction. Periodic re-entry adds security but annoyance.

7. **What about the terminal's own read display?** The terminal that sent the message can verify its own messages (it has the key). But when reading messages from a *previous* terminal session (different passphrase), it can't verify them. Is this acceptable?

8. **Agents impersonating other agents.** This proposal only covers human→agent authentication. Agent-to-agent impersonation (e.g. generalist posting as supervisor) is a separate problem. Do we need to solve that too?

## Recommendation

Use **Ed25519 asymmetric signatures**:
- Private key: derived from passphrase via PBKDF2, kept in terminal process memory
- Public key: written to `.nbs/chat/trusted-keys/<handle>.pub` on first use
- Signature: appended to wire format (needs design)
- Verification: any process can verify using the public key file
- Forgery: requires the passphrase (in the human's head) or reading the private key from `/proc/<pid>/mem`

This requires a crypto library (libsodium, OpenSSL, or hand-rolled Ed25519). Libsodium is the simplest and most audited option.

## Rejected Alternatives

| Alternative | Why rejected |
|-------------|-------------|
| Secret key in a file | pty-session gives agents full filesystem access |
| Kerberos tickets | Same-UID agents can reuse cached tickets |
| Kernel keyring | Agents can call `keyctl` via pty-session |
| Environment variable | Trivially readable |
| TPM/YubiKey | Hardware dependency, not available on all devservers |
| Rate limiting by TTY type | Bypassable by faking TTY |

## Implementation Estimate

Core changes:
- `terminal.c`: passphrase prompt, key derivation, signing on send
- `chat_file.c` or new `verify.c`: signature verification on read
- Wire format: extend base64 line format to include signature field
- Build: link libsodium (or vendored tweetnacl for zero dependencies)
- Tests: verify unsigned messages lack marker, signed messages show marker, forged signatures rejected

Dependencies: libsodium or tweetnacl (Ed25519 + PBKDF2)
