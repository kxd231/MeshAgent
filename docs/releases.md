# Agent releases

Use version tags such as `1.2.0` for stable releases and `1.2.0-beta.1` for
beta builds. A `v` prefix is also accepted. These versions are independent of
MeshCentral's version and the agent's embedded build date and commit hash.

Pushing a version tag runs **Agent Release**. The workflow calls the existing
Linux, Windows, macOS and FreeBSD builds at that tag, collects the raw binaries
and creates a draft release only after every build succeeds. The expected
filenames are in `.github/release-files.json`. macOS includes a universal binary.

Manual runs must select an existing version tag. The workflow must first be
present on the default branch; see
[GitHub's manual workflow instructions](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow).

Review the files and publish the draft. Do not replace published files or move
tags. A rerun cannot overwrite an existing release. Version tags with a suffix,
such as `1.2.0-beta.1`, create prerelease drafts. Keep the prerelease flag when
publishing beta builds. PR builds remain Actions artifacts.

MeshCentral verifies default downloads against the sizes and SHA384 hashes
pinned in its own `agents/agent-defaults.json`, and imported files against the
SHA256 digest GitHub records for each asset. Releases therefore carry no
checksum file, and signing or replacing a file in a draft needs no extra step.

The workflow uses the repository's `GITHUB_TOKEN` with write access confined to
the release job. Build jobs need only read access. Public release downloads do
not require a personal access token. MeshCentral requires a token to download
Actions artifacts, including PR builds.

## Windows x64 KVM builds

The Windows x64 agent with DXGI capture and TurboJPEG (`MeshService64.exe`, for
example the `dxgi` release) is built by **KVM perf build (Windows x64)**, from a
tag that is not a version tag:

1. Push the tag, for example `git tag dxgi-2 <commit>` and `git push origin dxgi-2`.
   Tags cannot be moved or deleted afterwards, so a corrected build needs a new tag.
2. The workflow builds the tag and puts `MeshService64.exe`, `MeshService64.exe.txt`
   (size and hashes) and `kvm-dxgi-check.exe` into a draft release of that tag.
   The run summary shows the commit, size, SHA-384 and SHA-256.
3. Check the files, write the notes and publish the draft. Published releases are
   immutable; a rerun stops with an error instead of replacing their files.
4. To make MeshCentral use the new file, pin its size and SHA-384 in MeshCentral's
   `agents/agent-defaults.json`.

To retry a failed run for a tag whose release is not published yet, use
**Actions > KVM perf build (Windows x64) > Run workflow** and enter the tag. Run
without a tag, the workflow is a test build of the selected branch: the files stay
a workflow artifact, `extra_cl` may add compiler options, and no release is touched.
The build jobs have read access only; the job that creates the draft does not run
the build.

## Existing MeshCentral binaries

Run **Actions > Migrate bundled agents > Run workflow** to preserve the files
from MeshCentral 1.2.6 without rebuilding them. Select one of these profiles;
the migration workflow handles the tag, so it does not need to be pushed manually.

- `legacy` prepares a draft `legacy-1.2.6` release with the 30 native default
  files, including older platforms, universal macOS and Windows MeshCmd.
- `september` prepares a `testing-sep2026` prerelease draft with the 13 September
  testing files.

Publish the legacy draft before releasing a MeshCentral version that removes
its bundled agents. Leave migration releases excluded from GitHub's latest
release selection and keep `testing-sep2026` marked as a prerelease. That tag
preserves the existing September binaries; new beta builds use version tags
such as `1.2.0-beta.1`.

Migration tags identify the packaging commit. `.github/release-migration.json`
records the source repository, archive commit, paths, sizes and hashes of the
preserved files.

MeshCentral can pin older platforms to this migration release while selecting
new builds for other platforms. Its scheduled checks report stable releases and
exclude prereleases. Administrators can import beta releases and install them
on selected devices. Publishing a release does not change server defaults or
deploy agents.
