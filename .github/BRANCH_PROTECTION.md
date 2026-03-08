# Branch Protection Setup for `main`

All changes to this repository should go through pull requests. Configure the following branch protection rules in **GitHub Settings → Branches → Add rule**.

## Recommended Settings

### Branch name pattern

```
main
```

### Protect matching branches

| Setting | Value | Why |
|---------|-------|-----|
| **Require a pull request before merging** | ✅ Enabled | All changes must go through PR review |
| Require approvals | 1 (recommended) | At least one reviewer must approve |
| Dismiss stale PR approvals when new commits are pushed | ✅ Enabled | Re-review after changes |
| **Require status checks to pass before merging** | ✅ Enabled | CI must be green |
| Status checks that are required | `Build Firmware` | The CI job name from `ci.yml` |
| Require branches to be up-to-date before merging | ✅ Enabled | Prevents merge conflicts |
| **Require conversation resolution before merging** | ✅ Enabled | All review comments must be addressed |
| **Do not allow bypassing the above settings** | ✅ Enabled | Applies rules to admins too |

### Optional (recommended for teams)

| Setting | Value |
|---------|-------|
| Require signed commits | Optional |
| Require linear history | Optional (enables squash/rebase only) |
| Restrict who can push to matching branches | Optional |

## Steps

1. Go to your repository on GitHub
2. Click **Settings** → **Branches**
3. Under "Branch protection rules", click **Add rule**
4. Enter `main` as the branch name pattern
5. Enable the settings listed above
6. Click **Create** (or **Save changes**)

> [!NOTE]
> The `Build Firmware` status check will only appear in the dropdown after the CI workflow has run at least once on a PR.
