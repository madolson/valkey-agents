# AI-driven workflow

This repo is a staging copy of `valkey-io/valkey`. Claude implements changes here; nothing
reaches upstream without a human sponsoring it.

## The loop

1. A maintainer files an issue and applies `ai:implement`.
2. `.github/workflows/claude-issue.yml` runs Claude, which branches, implements, builds, tests,
   and opens a PR against `unstable` with `Fixes #N`.
3. A human reviews. Comments containing `@claude` wake
   `.github/workflows/claude-pr.yml`, which pushes follow-up commits to the same branch.
4. A human approves and merges. Claude cannot merge or push to `unstable`.
5. A maintainer promotes the change upstream (below) and labels the issue `promoted`.

## Labels

| Label | Meaning |
|---|---|
| `ai:implement` | Opt-in. Claude should pick this issue up. |
| `ai:blocked` | Claude tried and could not finish. Applied by Claude. |
| `promoted` | Change has been promoted upstream. Applied by a maintainer. |

To re-trigger a stalled issue, remove and re-apply `ai:implement`.

## Promotion to upstream

Promotion is manual and always human-sponsored. Upstream has its own review standards, and
AI-authored changes should arrive there as ordinary contributions with a named accountable
submitter.

```sh
# In a clone of valkey-io/valkey with madolson/valkey as origin
git remote add agents https://github.com/madolson/valkey-agents.git
git fetch agents unstable
git checkout -b promote/<topic> upstream/unstable
git cherry-pick -x <sha>            # commits are already DCO signed off
git push origin promote/<topic>
gh pr create --repo valkey-io/valkey --base unstable
```

Then label the originating issue in this repo `promoted`.

Check the upstream project's policy on AI-assisted contributions and disclose per that policy.

## Infrastructure

Inference runs on Amazon Bedrock via GitHub OIDC. No API key and no static AWS credential exists
in this repo.

| Secret | Purpose |
|---|---|
| `AWS_ROLE_TO_ASSUME` | ARN of the `valkey-agents-claude-runner` IAM role |
| `APP_ID` | GitHub App ID for the bot identity |
| `APP_PRIVATE_KEY` | GitHub App PEM private key |

The IAM role trusts only `repo:madolson/valkey-agents:*` and may invoke only Claude models on
Bedrock. The GitHub App is installed only on this repo, with Contents, Issues, and Pull requests
write access, and no branch-protection bypass. The App token (rather than the default
`GITHUB_TOKEN`) is used so CI triggers on Claude's pushes.

## Cost controls

| Control | Value |
|---|---|
| Max turns | 50 (issue), 30 (PR) |
| Job timeout | 45 min (issue), 30 min (PR) |
| Parallelism | 1 run per issue, 1 per PR |
| Model | `global.anthropic.claude-opus-5` |

Watch Bedrock spend in CloudWatch and Actions minutes in the repo's billing view for the first
few weeks and tune the turn limits.
