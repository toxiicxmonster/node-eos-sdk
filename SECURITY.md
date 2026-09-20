# Security Policy

## Supported versions

This project is at `0.x`. Only the latest release receives fixes.

## Reporting a vulnerability

Please report security issues privately through GitHub's
[private vulnerability reporting](https://github.com/toxiicxmonster/node-eos-sdk/security/advisories/new)
rather than opening a public issue.

Expect an acknowledgement within a week. If a fix is warranted, it will ship in
the next release with credit, unless you would rather not be named.

## Scope

This package is a thin binding over Epic's C SDK. Roughly:

**In scope** — memory safety in the binding itself (use-after-free, buffer
handling in the packet drain, callback lifetime), anything that lets untrusted
peer input reach the SDK in a shape it does not expect, and credential handling
in the example and test code.

**Out of scope** — vulnerabilities in the Epic Online Services SDK itself.
Report those to Epic Games directly, through
<https://www.epicgames.com/site/en-US/security>.

## Handling credentials

Two notes that are worth stating plainly, because both are easy to get wrong:

- **The client secret is a credential.** It belongs in the environment or a
  gitignored `.env`, not in source control, and not anywhere a renderer process
  or a shipped web bundle can read it.
- **P2P payloads come from other players.** This binding hands you the bytes a
  peer sent, unvalidated, because it cannot know what they should look like.
  Validate before you parse, and do not feed them to anything that evaluates.
