# Making a release

A release is a tag. Pushing a tag `v<version>` (for example `v0.1.0`) runs
`.github/workflows/release.yml`. It builds, from that tag:
- the Cedarview kernel package in its release flavor, which has no test-kit patches
  (`powervr-sgx-deb.yml`);
- `mesa-sgx` and `sgx545-firmware` (`mesa-debian.yml`);
- `SHA256SUMS` over all of them.

It then attaches them to a **draft pre-release**. Nothing is public until a person
publishes the draft on GitHub. Publishing runs `.github/workflows/apt-repo.yml`, which
rebuilds the signed apt repository on GitHub Pages from every published release.

The packages take their versions from the tag:

| package | version for `v0.1.0` |
|---|---|
| `linux-image-7.3.3-sgx`, `linux-headers-7.3.3-sgx` | `7.3.3-sgx0.1.0` |
| `mesa-sgx` | `<Mesa version>+sgx0.1.0` |
| `sgx545-firmware` | `0.1.0` |

A tag is `v` and a version that starts with a digit: digits, letters, dots, `+` and
`-`. A `-` becomes `~` in the package versions, so a pre-release sorts before its release
(`v0.2.0-rc1` gives `0.2.0~rc1`, which apt treats as older than `0.2.0`).

## Steps

1. On `dev`, with CI green, tag the commit and push the tag:

   ```bash
   git tag -a v0.1.0 -m "powervr_sgx 0.1.0" && git push origin v0.1.0
   ```

2. Wait for the `release` run to finish. The kernel build takes several hours.
3. On the Releases page, check the draft: its five files (the four packages and
   `SHA256SUMS`), the notes, and that `SHA256SUMS` matches. Then publish it.
4. Publishing starts `apt-repo`. When it finishes, the repository is live, and
   `sudo apt update` on a machine set up as in `docs/INSTALL.md` sees the new versions.

To throw a test tag away, delete its draft release first and then the tag:

```bash
gh release delete v0.0.0-test --yes --repo The1TrueJoe/powervr_sgx
```

```bash
git push origin :refs/tags/v0.0.0-test && git tag -d v0.0.0-test
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

The workflow publishes the public key itself, at
`https://the1truejoe.github.io/powervr_sgx/powervr-sgx.gpg` (binary, for
`/usr/share/keyrings`) and `powervr-sgx.asc` (armored). It checks the repository's
signatures against that file before it deploys.

To rotate the key, make a new one, replace the secret, and run `apt-repo` again from
the Actions tab. Users then fetch the new `powervr-sgx.gpg` once.
