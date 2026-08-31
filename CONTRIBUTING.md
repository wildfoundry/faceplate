# Contributing

Faceplate is a deliberately narrow downstream project derived from kmscon. It
adds one fixed appliance scene containing one real terminal and is not a
desktop, compositor, HMI framework, or window manager.

For bugs and feature requests, use GitHub Issues. Open an issue before a
substantial change so display, platform, upstream, and scope implications can be
agreed before implementation.

## Before opening a pull request

Build and test with Meson using the relevant optional backends. At minimum:

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

Also run the formatting checks represented in CI. Platform-specific display or
input changes must be tested on affected hardware rather than inferred from an
x86 build alone.

Keep each pull request focused on one problem. Include tests for changed
behaviour, update documentation for user-visible changes, and add an entry under
`Unreleased` in `CHANGELOG.md` once the Faceplate changelog is introduced.

Do not include credentials, real terminal contents, private system data, or
restricted branding in tests, fixtures, screenshots, or issue reports. The
Dataplicity logo belongs in the separately licensed theme package, never this
repository.

## Upstream and licensing

Preserve the complete kmscon history and all existing copyright and licence
notices. Do not replace an upstream notice with a Faceplate notice. Add
copyright only for new or materially modified work.

Changes that are generally useful to kmscon should be proposed upstream when
practical. Upstream imports must use dedicated pull requests and update
`UPSTREAM.md`.

By submitting a contribution, you agree that it may be distributed under the
licence applicable to the files being changed. New Faceplate code should use
the project's MIT licence unless a reviewed third-party component requires a
compatible, clearly documented exception.
