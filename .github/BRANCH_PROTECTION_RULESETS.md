# Branch protection rulesets

This repository follows the WildFoundry contract used by Dataplicity OS and
Prelude repositories: pull requests are required, required CI must pass, and
force-pushes and branch deletion are blocked.

## Protected branches

| Ruleset | Target | Required checks |
| --- | --- | --- |
| Protect main | `main` | `Format`, `Build and test (gcc)`, `Build and test (clang)`, `Minimal build`, plus the check emitted by GitHub CodeQL default setup |
| Protect release branches | `release/*` | same as `main` |

Rulesets should require a pull request with zero mandatory approvals, dismiss
stale approvals when new commits are pushed, require branches to be up to date,
and block force-push and deletion. Repository administrators retain bypass for
recovery only.

Enable private vulnerability reporting and Dependabot security updates. Keep
GitHub's CodeQL default setup enabled and require the check it emits. Do not add
an advanced CodeQL workflow unless default setup is disabled first; GitHub does
not accept both configurations at the same time.
