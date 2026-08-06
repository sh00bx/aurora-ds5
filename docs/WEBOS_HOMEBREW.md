# Aurora DS5 in the Homebrew Channel

There are two ways into the Homebrew Channel app, and they are independent:

1. **Our own repository** — a repo link the user adds themselves. Works today, needs nobody's approval.
2. **The official catalog** ([repo.webosbrew.org](https://repo.webosbrew.org/), built from
   [webosbrew/apps-repo](https://github.com/webosbrew/apps-repo)) — a pull request that gets reviewed.

Both end up pulling the *same* manifest out of the *same* GitHub release, so publishing a release is all
that maintenance normally takes.

## 1. Install from our repository

In the Homebrew Channel app: **Settings → Add Repository**, then

```
https://raw.githubusercontent.com/sh00bx/aurora-ds5/main/repo.json
```

*Aurora DS5* shows up in the app list, and every later release is offered as an update automatically.

### How the pieces hang together

```
repo.json  (main branch, never changes between releases)
   └─ manifestUrl → releases/latest/download/com.aurora.ds5.manifest.json
                        └─ ipkUrl: "com.aurora.ds5_<version>_arm.ipk"   ← relative!
                              resolved against the manifest URL by both the
                              Homebrew Channel client and the catalog builder,
                              i.e. releases/latest/download/<that file>
```

The relative `ipkUrl` is what `webosbrew-gen-manifest` emits, and it is the reason nothing has to be
edited per release: `repo.json` points at `latest`, and the manifest inside a release always names the
`.ipk` sitting next to it.

**Invariants — break one and installs 404:**

- The manifest asset is named exactly `com.aurora.ds5.manifest.json` in every release.
- The `.ipk` asset keeps the CPack name `com.aurora.ds5_<version>_arm.ipk` and is attached to the same
  release as its manifest.
- `repo.json`, `description.html` and `deploy/webos/icon_large.png` stay reachable on the **`main`**
  branch — the repo link is a `raw.githubusercontent.com/.../main/...` URL.

## 2. Submit to the official catalog (optional)

1. Confirm the manifest URL resolves:
   `curl -sIL https://github.com/sh00bx/aurora-ds5/releases/latest/download/com.aurora.ds5.manifest.json`
2. Fork [webosbrew/apps-repo](https://github.com/webosbrew/apps-repo).
3. Copy [`deploy/webosbrew/com.aurora.ds5.yml`](../deploy/webosbrew/com.aurora.ds5.yml) into the fork's
   `packages/` directory, keeping the filename.
4. Open a PR titled e.g. *Add Aurora DS5 (com.aurora.ds5)* and wait for review.

The catalog rules require a real open-source license and correct attribution for ports. Aurora DS5 is
GPL-3.0 like its upstreams, and the modified libraries it links are published too — see
[Sources](#sources-gpl).

## Publishing a release

`.github/workflows/release.yml` runs on **published GitHub releases** and does everything else:

1. builds the webOS package (`ares-package`),
2. generates the Homebrew manifest with
   `webosbrew-gen-manifest -i <icon> -l <repo> -r optional`,
3. validates it against
   [`HomebrewPackageManifest.schema.json`](https://github.com/webosbrew/docs/blob/main/schemas/HomebrewPackageManifest.schema.json),
4. attaches `*.ipk`, `*.manifest.json` and the debug symbols to the release.

So the whole release procedure is:

```bash
# bump MOONLIGHT_VERSION in CMakeLists.txt first, commit, then:
git tag v1.3.2 && git push origin v1.3.2
gh release create v1.3.2 --title "Aurora DS5 1.3.2" --notes "..."
```

`rootRequired` is **`optional`**, set in `cmake/AresPackage.cmake`. That is deliberate: the app runs
without root (falling back to the daemon-free hidraw controller path), and root only unlocks the DS5
raw-ACL transport. Claiming `true` would wrongly hide the app from users without a rooted TV; claiming
`false` would hide that the headline feature needs one.

## <a name="sources-gpl"></a>Sources (GPL/LGPL)

Publishing the `.ipk` means publishing the corresponding sources. All of them are public:

| Component | License | Repository |
|---|---|---|
| App | GPL-3.0 | <https://github.com/sh00bx/aurora-ds5> |
| moonlight-common-c (modified) | GPL-3.0 | <https://github.com/sh00bx/moonlight-common-c> |
| ss4s (modified) | LGPL-3.0 | <https://github.com/sh00bx/ss4s> |
| commons-c (modified) | MIT | <https://github.com/sh00bx/commons-c> |
| ds5_txd (vendored) | MIT | <https://github.com/sh00bx/webos-ds5-raw-acl> |

If any of those ever goes private again, the release workflow keeps working but the distribution stops
being license-compliant. Keep them public.
