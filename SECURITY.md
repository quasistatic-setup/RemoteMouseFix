# Security policy

## Supported versions

Only the latest release receives fixes. RemoteMouseFix is experimental and has no
long-term support branches.

| Version | Supported |
| --- | --- |
| 0.6.x | yes |
| older | no |

## Reporting a vulnerability

Please do not open a public issue for a security problem.

Use GitHub's private reporting instead: open the **Security** tab of this repository and
choose **Report a vulnerability**. That creates a private advisory visible only to the
maintainer and to you.

Useful in a report: what an attacker can do, which Windows version and configuration you
used, and a minimal way to reproduce it. If you attach a log, review it first, as described
in the README.

Expect a first reply within a week. There is no bug bounty.

## What is in scope

This is a local desktop tool with no network access and no privileged component, so the
interesting surface is small but real:

- The low-level mouse hook and the synthetic input it sends, in particular any way to make
  it withhold or generate input outside the documented conditions.
- The configuration parser, which reads a JSON file from the EXE directory or from a path
  given on the command line.
- The log writer and its rotation, in particular path handling and anything that could
  write outside the configured log directory.
- The release artefacts themselves, for example a checksum in the release notes that does
  not match the published ZIP.

## What is out of scope

- SmartScreen prompts and antivirus false positives. The release EXE is not code-signed
  and installs a mouse hook, so both are expected, see the README.
- Anything that requires an attacker to already run code on the machine as the same user.
- Behaviour of the game, of the remote-control application, or of Windows itself.
- Running the tool or the game with administrator rights, which is unsupported.
