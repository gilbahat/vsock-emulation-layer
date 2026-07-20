# Releasing

Releases are source-only: Homebrew builds from the tag tarball GitHub generates,
so there are no signed/notarized binaries to attach.

## Cutting a release

1. Update `CHANGELOG.md`: move items from `[Unreleased]` into a new
   `## [X.Y.Z] - YYYY-MM-DD` section and refresh the compare links at the bottom.
2. Commit, then tag and push:
   ```sh
   git tag vX.Y.Z
   git push origin main vX.Y.Z
   ```
3. The `Release` workflow (`.github/workflows/release.yml`) runs on the tag: it
   re-runs `make test`, creates the GitHub Release with generated notes, then
   bumps the Homebrew formula in `gilbahat/homebrew-taps`.

## Homebrew formula bump

The `bump-formula` job updates `Formula/vsock-emulation-layer.rb` in the tap
(new `url` + `sha256`) via a PR. It needs a repo secret:

- **`HOMEBREW_TAP_TOKEN`** — a fine-grained Personal Access Token scoped to
  `gilbahat/homebrew-taps` with **Contents: read and write** permission.

Set it once:
```sh
gh secret set HOMEBREW_TAP_TOKEN --repo gilbahat/vsock-emulation-layer
```
Without the secret the job no-ops (the first release's formula is authored by
hand). If you cut a release before configuring it, bump the formula manually:
compute `sha256` of the tag tarball and edit `url`/`sha256` in the tap.

```sh
curl -sL https://github.com/gilbahat/vsock-emulation-layer/archive/refs/tags/vX.Y.Z.tar.gz | shasum -a 256
```

## Versioning

Semantic Versioning. The `(cid,port)->path` convention is the interop contract;
any change to it is a breaking change and bumps the major version.
