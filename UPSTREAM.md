# Upstream relationship

Faceplate is a downstream project derived from kmscon.

- Upstream: <https://github.com/kmscon/kmscon>
- Baseline commit: `791e659f2420e00cda7c39b4eb53f3c2844b4885`
- Baseline date: 2026-08-28
- Upstream remote name: `upstream`

The complete kmscon Git history and original author metadata must be preserved.
Existing source copyright and licence headers must not be removed or replaced.
Copyright notices for new Faceplate work may be added only to new or materially
modified code.

## Maintenance policy

1. Keep presentation changes isolated from inherited display, seat, input,
   terminal, and PTY code wherever practical.
2. Import upstream changes in dedicated pull requests.
3. Do not mix an upstream merge with Faceplate feature work.
4. Record the new upstream commit in this file after every import.
5. Run the full build, terminal, renderer, and hardware test matrix before
   merging an upstream update.
6. Contribute generally useful fixes upstream when they are not specific to the
   Faceplate presentation layer.

Faceplate must not imply endorsement by the kmscon authors or maintainers.
