# Aurora on webOS Homebrew (Homebrew Channel)

The official catalog is the **[webosbrew/apps-repo](https://github.com/webosbrew/apps-repo)** repository (site: [repo.webosbrew.org](https://repo.webosbrew.org/)). Official submission guide: [Submit Application](https://repo.webosbrew.org/submit).

## What you need

1. **A GitHub Release** with these files (the *Release* CI already generates them when `webosbrew-gen-manifest` is installed):
   - `com.aurora.gamestream_*_arm.ipk`
   - `com.aurora.gamestream.manifest.json`
   URL used by the catalog (always points at the latest release):
   `https://github.com/GuiDev1994/aurora-tv/releases/latest/download/com.aurora.gamestream.manifest.json`

2. **Icons** in `deploy/webos/` on the `main` branch of your repo (`icon.png`, `icon_large.png`, etc.), so the `iconUri` in the YAML and in `AresPackage.cmake` resolve correctly.

3. **YAML file** already prepared in this project:
   [`deploy/webosbrew/com.aurora.gamestream.yml`](../deploy/webosbrew/com.aurora.gamestream.yml)

## Steps to submit

1. **Fork** [webosbrew/apps-repo](https://github.com/webosbrew/apps-repo).
2. Copy `com.aurora.gamestream.yml` into the fork's **`packages/`** folder (keep the same filename).
3. Confirm the **manifestUrl** link opens the JSON from your latest release (browser or `curl`).
4. Open a **Pull Request** to `webosbrew/apps-repo` with a clear title, e.g.: *Add Aurora (com.aurora.gamestream)*.
5. Wait for review from the webOS Homebrew team.

## Maintenance

Every new **tag/release** you publish with `.ipk` + `.manifest.json` updates what users get via `releases/latest/download/…` — there's no need to change the YAML in apps-repo **as long as** the filename `com.aurora.gamestream.manifest.json` stays the same.

If you change the **app id** or the manifest filename, update the YAML in apps-repo in a separate PR.
