# Archcodex working instructions

## Resolve issues through a dedicated branch and pull request

- Read the issue and the user's agreed scope before implementing a fix. Create
  a dedicated branch for that issue before making repository changes; use a
  descriptive name such as `fix/4-new-session-identity`. Keep unrelated work out
  of the branch. Do not commit or push issue fixes directly to the default branch.
- Commit completed authorized work, preserving unrelated user changes. Open a
  pull request against the default branch and link it to the issue with a closing
  keyword in the PR description, such as `Closes #4`.
- Close the issue through the PR's merge into the default branch. Do not manually
  close it merely because a local commit, build or release exists, unless the
  user explicitly instructs otherwise. After an authorized merge, verify both
  the merged PR state and the closed issue state on GitHub.
- Complete the authorized workflow instead of stopping at a local fix. Follow
  the current task's authorization for pushing, opening PRs, merging and
  publishing; these instructions do not independently authorize a merge or release.

## Document decisions and divergences in both the issue and PR

- Keep the issue updated with the agreed problem, scope, acceptance criteria,
  findings, decisions and changes from the original proposal. Link the PR.
- Write the PR for a reviewer who has not read the conversation: explain the
  concrete problem, resulting behavior, implementation choices, validation and
  remaining limitations. Link the issue and relevant evidence. Keep the title
  and description aligned with the final implementation.
- In **both** the issue and PR, include a clearly labeled
  **Decisions not agreed in advance** section. For every such decision or action,
  state what was chosen, why, affected files/configuration, behavioral and
  compatibility consequences, and current status. Include relevant defaults,
  dependencies, restrictions, testing/publication requirements, Git operations
  and temporary or external artifacts. Write **None** when there are none.
- Distinguish explicit user approval from assumptions, implementation choices
  and additions to scope. Document divergences from the request or earlier
  design, including what remains deferred and whether the user accepted it.
  Disclosure after implementation is not retroactive authorization.
- Accepted limitations must be recorded accurately in both places. Do not claim
  the full original scope was implemented when the user accepted a narrower
  delivery. Do not omit routine choices merely because tests passed.
- Include the same decision summary in the final user-facing handoff.

## Release notes and publication

- Use GitHub-generated release notes for new releases, with an explicit previous
  tag and the exact source commit used for the artifact. Informative merged PR
  titles and descriptions are part of producing useful release notes.
- Verify that the expected merged PRs appear in the generated notes. Investigate
  a missing PR or incorrect comparison range instead of treating an empty
  generated changelog as adequate documentation of an issue fix.
- Keep installation instructions and implementation/compatibility details in
  project documentation. Do not add release-note lists of unchanged or
  unimplemented features.
- A preference for future release notes is not authorization to rewrite an
  already published release. Change existing release notes only when explicitly
  requested, and preserve their exact prior contents for verification/recovery.
- When publishing an already-built package, use that artifact and verify its
  identity. Do not rebuild locally or on GitHub unless requested or required by
  an authorized code change. Follow `docs/releases/README.md` for publication.
