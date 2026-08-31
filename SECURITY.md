# Security policy

## Supported versions

Before 1.0, security fixes are made against the latest published Faceplate
release. Older preview releases do not receive separate security updates.

## Reporting a vulnerability

Do not open a public issue for a suspected vulnerability. Report it privately
through GitHub's security advisory flow:

<https://github.com/wildfoundry/faceplate/security/advisories/new>

Include affected versions, reproduction steps, impact, and any known
mitigation. Do not include credentials, private keys, terminal contents, or
system data that is not needed to understand the report.

## Trust model

Faceplate acquires display and input devices, controls a virtual terminal, and
launches a configured child process attached to a PTY. It does not provide
authentication, privilege elevation, remote access, telemetry, or a network
service. Authentication and command authorization belong to the launched
appliance shell or operating system.

The core package is neutral and contains no Dataplicity logo. Runtime themes
and assets are untrusted input: their paths, formats, decoded dimensions, and
memory use must be validated and bounded.

Pull-request workflows use read-only tokens. Release permissions must be
granted only to tagged or explicitly dispatched release jobs. No release
secrets may be exposed to pull-request code.
