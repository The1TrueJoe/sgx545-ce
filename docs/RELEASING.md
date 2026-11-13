# Making a release

A release is a tag. Pushing a tag `v<version>` (for example `v0.1.0`) runs
`.github/workflows/release.yml`. It builds, from that tag:
- the Cedarview kernel package in its release flavor, which has no test-kit patches
  (`powervr-sgx-deb.yml`);
- `mesa-sgx` for amd64 and i386, and `sgx545-firmware` (`mesa-debian.yml`);
- `SHA256SUMS` over all of them.

It then attaches them to a **draft pre-release**, with `docs/releases/<tag>.md` as its notes
when that file exists (generic notes otherwise). Nothing is public until a person
publishes the draft on GitHub. Publishing runs `.github/workflows/apt-repo.yml`, which
rebuilds the signed apt repository on GitHub Pages from every published release.

The packages take their versions from the tag:

| package | version for `v0.1.0` |
|---|---|
| `linux-image-7.3.3-sgx`, `linux-headers-7.3.3-sgx` | `7.3.3-sgx0.1.0` |
| `mesa-sgx` | `<Mesa version>+sgx0.1.0` |
| `sgx545-firmware` | `0.1.0` |

A CI build that isn't a release numbers its packages `0~git<date>.<commit>` (the
kernel `7.3.3-sgx0~git…`, Mesa `<Mesa version>+sgx0~git…`), which sorts below every
release, so apt moves from a test build to the next release by itself.

The kernel and Mesa must come from the same release once the uAPI changes. A release's
`mesa-sgx` and `sgx545-firmware` carry `Breaks:` on the kernels before their uAPI
(`mesa/debian/BREAKS`), so apt upgrades the kernel with them. Raise it when the uAPI's
major version changes.

A tag is `v` and a version that starts with a digit: digits, letters, dots, `+` and
`-`. A `-` becomes `~` in the package versions, so a pre-release sorts before its release
(`v0.2.0-rc1` gives `0.2.0~rc1`, which apt treats as older than `0.2.0`).
GitHub doesn't keep a `~` in a file name, so the release files carry a `.`
there (`sgx545-firmware_0.2.0.rc1_all.deb`). `SHA256SUMS` lists those names,
and the package inside still says `0.2.0~rc1`.

## Steps

0. When the kernel isn't on `dev` yet, cut a release branch from `dev` and merge the
   kernel branch into it (`release/v0.2` is `dev` plus `m2-drm`), then run every workflow
   on it from the Actions tab (`gh workflow run <file> --ref release/vX.Y`). Rehearse the
   release on the branch: run `release` by hand with a version such as `0.2.0~rc0`. It builds
   the packages under that version, signs a throwaway apt repository and runs
   `tools/release/repo-test.sh` in Debian 13: the upgrade from the last published release,
   `mesa-sgx:i386` alone, a fresh install, and the way off CI-built packages. It also builds
   the driver with DKMS against the release kernel's headers. Nothing is tagged or
   published. `kernel_run` reuses a finished release-flavor `powervr-sgx-deb` run with the
   same version instead of building the kernel again. Put the notes in
   `docs/releases/v<version>.md`; the tag's draft takes them.

1. Merge `dev` into `main` (a merge commit) once `dev`'s CI is green, then tag `main`
   and push the tag:

   ```bash
   git checkout main && git merge --no-ff dev && git push origin main
   ```

   ```bash
   git tag -a v0.1.0 -m "powervr_sgx 0.1.0" && git push origin v0.1.0
   ```

2. Wait for the `release` run to finish. The kernel build takes several hours.
3. On the Releases page, check the draft: its six files (the kernel image and headers,
   the firmware, Mesa for amd64 and i386, and `SHA256SUMS`), and that `SHA256SUMS` checks against a plain download:

   ```bash
   gh release download v0.1.0 --repo The1TrueJoe/powervr_sgx --dir /tmp/v0.1.0 && (cd /tmp/v0.1.0 && sha256sum -c SHA256SUMS)
   ```

   The workflow writes short generic notes. Replace them with the release's own notes
   (what changed, what was tested, the known issues), then publish it.
4. Publishing starts `apt-repo`. When it finishes, the repository is live, and
   `sudo apt update` on a machine set up as in `docs/INSTALL.md` sees the new versions.
   Check the published key once:

   ```bash
   curl -fsSL https://telaak.dev/powervr_sgx/powervr-sgx.gpg | gpg --show-keys
   ```

   Its fingerprint must be the one `docs/INSTALL.md` gives,
   `5C26DC64CAEC63910424E220E0DD2B7AD794C167`.

To throw a test tag away, delete its draft release and the tag on GitHub, then the
local tag:

```bash
gh release delete v0.0.0-test --yes --cleanup-tag --repo The1TrueJoe/powervr_sgx
```

```bash
git tag -d v0.0.0-test
```

## One-time setup for the apt repository

The repository is signed with a key that exists only for it. The workflow reads the
key from the repository secret **`APT_SIGNING_KEY`**: an ASCII-armored private key,
with no passphrase. Create it once, on your own machine, with these commands. The key
never appears in the repository or in CI output.

1. Make the key. It signs only and expires in three years.

   ```bash
   gpg --batch --passphrase '' --quick-gen-key "powervr_sgx apt repository <joseph@telaak.dev>" ed25519 sign 3y
   ```

2. Find its fingerprint (the long hex line under `sec`):

   ```bash
   gpg --list-secret-keys --keyid-format long "powervr_sgx apt repository"
   ```

3. Store the private key as the secret. Replace `FPR` with the fingerprint:

   ```bash
   gpg --armor --export-secret-keys FPR | gh secret set APT_SIGNING_KEY --repo The1TrueJoe/powervr_sgx
   ```

4. Keep an offline backup of the key, and a revocation certificate, then remove the
   key from this machine's keyring if you like:

   ```bash
   gpg --armor --export-secret-keys FPR > powervr-sgx-apt-key.asc
   ```

   ```bash
   gpg --output powervr-sgx-apt-revoke.asc --gen-revoke FPR
   ```

5. Set GitHub Pages to deploy from Actions:

   ```bash
   gh api -X POST repos/The1TrueJoe/powervr_sgx/pages -f build_type=workflow
   ```

   If Pages is already on, switch its source to "GitHub Actions" under Settings → Pages.

6. Let tags deploy to Pages. A published release runs `apt-repo` on its tag, and the
   `github-pages` environment allows only `main` by default, so the deploy job would be
   refused:

   ```bash
   gh api -X POST repos/The1TrueJoe/powervr_sgx/environments/github-pages/deployment-branch-policies -f name='v*' -f type=tag
   ```

The workflow publishes the public key itself, at
`https://telaak.dev/powervr_sgx/powervr-sgx.gpg` (binary, for
`/usr/share/keyrings`) and `powervr-sgx.asc` (armored). It checks the repository's
signatures against that file before it deploys.

To rotate the key, make a new one, replace the secret, change the fingerprint in
`docs/INSTALL.md`, and run `apt-repo` again from the Actions tab. Users then fetch the
new `powervr-sgx.gpg` once.
