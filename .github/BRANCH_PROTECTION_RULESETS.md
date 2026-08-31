# Branch protection rulesets

This repository follows the WildFoundry contract used by Dataplicity OS and
Prelude repositories: pull requests are required, required CI must pass, and
force-pushes and branch deletion are blocked.

## Protected branches

| Ruleset | Target | Required checks |
| --- | --- | --- |
| Protect main | `main` | `Format`, `Build and test (gcc)`, `Build and test (clang)`, `Minimal build`, `Analyze (c-cpp)` |
| Protect release branches | `release/*` | same as `main` |

Rulesets should require a pull request with zero mandatory approvals, dismiss
stale approvals when new commits are pushed, require branches to be up to date,
and block force-push and deletion. Repository administrators retain bypass for
recovery only.

Enable private vulnerability reporting and Dependabot security updates. Use
the explicit CodeQL workflow in this repository rather than GitHub's default
CodeQL setup, so required-check names stay stable.
