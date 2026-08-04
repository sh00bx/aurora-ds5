# Aurora – Build and installation for LG webOS

This guide explains how to enable developer mode on the TV, build Aurora, and install it manually on LG webOS TVs (LG C1 and compatible).

---

## Table of contents

1. [Developer mode on the TV](#1-developer-mode-on-the-tv)
2. [Manual installation](#2-manual-installation)
3. [Build](#3-build)
4. [Troubleshooting](#4-troubleshooting)
5. [webOS Homebrew (catalog)](#5-webos-homebrew-catalog)

---

## 1. Developer mode on the TV

To install apps manually (.ipk), the TV must be in developer mode.

### Step-by-step (LG official)

1. **LG developer account**
   - Go to [developer.lge.com](https://developer.lge.com) and create an account

2. **Developer Mode app on the TV**
   - Press **Home** on the remote
   - Open **LG Content Store**
   - Search for **"Developer Mode"**
   - Install the app

3. **Enable developer mode**
   - Open the **Developer Mode** app
   - Sign in with your LG account
   - Turn on **"Dev Mode Status"** (the TV may restart)
   - Turn on **Key Server**

4. **Connection passphrase**
   - In the Developer Mode app, note the **passphrase** shown
   - You will use it to connect the TV to your PC

> Developer mode disables automatically after several reboots without a network connection. Re-enable it from the app when needed.

### Alternative: webosbrew

If you use [webosbrew](https://webosbrew.org/):

1. Install the Homebrew Channel (see webosbrew docs)
2. Install [dev-manager-desktop](https://github.com/webosbrew/dev-manager-desktop) to install .ipk via the GUI

---

## 2. Manual installation

### Prerequisites

- TV in developer mode (see section 1)
- TV and PC on the **same network**
- Built `.ipk` package (section 3) or from a release

### Method A: ares-cli (command line)

1. **Install webOS CLI**
   - Follow [webOS TV Developer – CLI](https://webostv.developer.lge.com/develop/tools/cli-dev-guide)
   - Windows: `npm install -g @webos-tools/ares-cli`
   - Linux: `sudo npm install -g @webos-tools/ares-cli`

2. **Configure the device**
   ```bash
   ares-setup-device
   ```
   - Enter name, TV IP, and when prompted, the **passphrase** from the Developer Mode app

3. **Install the .ipk**
   ```bash
   ares-install dist/com.aurora.gamestream_1.0.2_arm.ipk -d <TV_NAME>
   ```
   (Adjust the filename for your build version.)

4. **Launch Aurora**
   ```bash
   ares-launch com.aurora.gamestream -d <TV_NAME>
   ```

### Method B: dev-manager-desktop (GUI)

1. Install [webosbrew](https://webosbrew.org/) on the TV
2. Install [dev-manager-desktop](https://github.com/webosbrew/dev-manager-desktop)
3. Ensure TV and PC are on the same network
4. Open dev-manager-desktop and install the `.ipk` file

---

## 3. Build

Aurora is built via **cross-compilation** on **Linux** (or WSL2 on Windows).

### Compatibility

- **webOS 6.x** (LG C1) – NDL, SMP, H.265, HDR
- **ARM** – toolchain `arm-webos-linux-gnueabi`

### Prerequisites

**Linux (Ubuntu/Debian):**
```bash
sudo apt-get update
sudo apt-get install -y cmake gawk curl git build-essential
```

**Windows:** Use Docker or WSL2 with Ubuntu (see section 3.4).

### 3.1. Quick build (automated script)

```bash
cd moonlight-tv   # or your repo folder name
chmod +x scripts/webos/build_for_lg.sh
./scripts/webos/build_for_lg.sh
```

The `.ipk` will be created in `dist/`.

### 3.2. Manual build (step by step)

**1. webOS SDK**

```bash
cd /tmp
curl -L -O https://github.com/openlgtv/buildroot-nc4/releases/download/webos-b17b4cc/arm-webos-linux-gnueabi_sdk-buildroot.tar.gz
tar -xzf arm-webos-linux-gnueabi_sdk-buildroot.tar.gz
./arm-webos-linux-gnueabi_sdk-buildroot/relocate-sdk.sh
```

**2. Submodules**

```bash
cd moonlight-tv   # repo folder
git submodule update --init --recursive
```

**3. Configure and build**

```bash
export TOOLCHAIN_FILE=/tmp/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake
./scripts/webos/easy_build.sh -DCMAKE_BUILD_TYPE=Release
```

**4. Release build**

```bash
CMAKE_BUILD_TYPE=Release ./scripts/webos/build_for_lg.sh
```

### 3.3. Output

The resulting package will be at:
```
dist/com.aurora.gamestream_1.0.2_arm.ipk
```

### 3.4. Windows (Docker or WSL2)

**Docker:**
```powershell
.\scripts\webos\build_with_docker.ps1
```

**WSL2 (Ubuntu):**
```bash
wsl --install -d Ubuntu
# In Ubuntu:
sudo apt-get install cmake gawk curl git build-essential
./scripts/webos/build_for_lg.sh
```

---

## 4. Troubleshooting

### "TOOLCHAIN_FILE not found"

Set the SDK path:
```bash
export WEBOS_SDK_DIR=/path/to/arm-webos-linux-gnueabi_sdk-buildroot
./scripts/webos/build_for_lg.sh
```

### Custom SDK location

```bash
sudo mv /tmp/arm-webos-linux-gnueabi_sdk-buildroot /opt/
export TOOLCHAIN_FILE=/opt/arm-webos-linux-gnueabi_sdk-buildroot/share/buildroot/toolchainfile.cmake
./scripts/webos/easy_build.sh -DCMAKE_BUILD_TYPE=Release
```

### CMake dependency errors

The buildroot-nc4 SDK includes pbnjson_c, PmLogLib, webosi18n, etc. If something is missing, verify your SDK installation.

### TV does not appear in ares-setup-device

- TV and PC on the same network
- Developer mode enabled on the TV
- Firewall not blocking the ares-cli port

### HEVC artifact drift (long sessions)

If blockiness or color smearing builds up over time with **H.265** streams, try **Settings → Video → Periodic decoder refresh (HEVC)** (interval 10–30 s). This only applies when HEVC is active; it has no effect on H.264-only streams.

### Streaming over Tailscale

Aurora does not bundle a Tailscale client on the TV. To reach a PC over Tailscale:

1. Install and sign in to **Tailscale** on your PC (Sunshine/Vibepollo host).
2. Note the PC's Tailscale IP (usually `100.x.x.x`) or MagicDNS name from the Tailscale admin console.
3. In Aurora, **Add host** manually and enter that IP (port **47989** for HTTP discovery, or the port shown in your host settings).
4. Pair and stream as on the local LAN.

The TV and PC do not need to be on the same physical network, but both must reach each other through your tailnet.

---

## 5. webOS Homebrew catalog

To list **Aurora** in the [Homebrew Channel](https://webosbrew.org/) app store ([repo.webosbrew.org](https://repo.webosbrew.org/)), submit a PR to [webosbrew/apps-repo](https://github.com/webosbrew/apps-repo) using [`deploy/webosbrew/com.aurora.gamestream.yml`](../deploy/webosbrew/com.aurora.gamestream.yml) and the checklist in **[WEBOS_HOMEBREW.md](WEBOS_HOMEBREW.md)**.
