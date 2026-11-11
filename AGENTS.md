# Archcodex working instructions

## Resolve issues through a dedicated branch and pull request

## Document decisions and divergences in both the issue and PR

## Local test builds and public releases

Follow [the release process](docs/releases/process.md). `VERSION` is the single
version source. Package only clean committed source; use a new `-rc.N` candidate
number for each test delivery and retain its local `test/v...` tag and build receipt.
Keep bundles and their versioned sidecar files directly in `dist/`, without
version subdirectories.
Building or testing does not authorize pushing, creating a GitHub draft, or
publishing. Publish only on an explicit release request, using the already verified
final-version bundle and GitHub-generated notes. Never push `test/*` tags, overwrite
a distributed artifact, or rebuild a bundle as part of publication.

