# Build and deployment rules

## Workflow

- After code changes, ask whether a build is needed. Do not build silently.
- Before building or deploying, stop the daemon and any process that locks the target binary.
- After a successful build, ask whether deployment is needed and which targets to use.
- Never store passwords, tokens, or private keys in this file. Read them from environment variables or a secret store.
- Record deployments in the final response: what was stopped, what was backed up, what was copied, and the verification result.

## Build

Build three artifact types:

1. Windows
2. Linux
3. QNAP

Windows uses the CMake presets in this project:

```powershell
cmake --preset Release
cmake --build --preset Release --target transmission-daemon
```

The Windows binary is typically under `build\Release\daemon\Release\transmission-daemon.exe`.

MSVC note: `tr_error_code_t` is `int64_t` on Windows and `int` on POSIX. Do not mix
`return ENOENT;` with `return err;` in a function whose return type is `auto`; cast errno
values explicitly, e.g. `return static_cast<tr_error_code_t>(ENOENT);`.

Linux is built on the `transmission` server from `/root/Transmission`:

```bash
ssh transmission 'cd /root/Transmission && cmake --build build --target transmission-daemon -j2'
# artifact: /root/Transmission/build/daemon/transmission-daemon
```

QNAP is cross-built on the `transmission` server with the existing ARMv5 toolchain
(the QNAP reports `armv5tel`). The `/tmp/qnap-build-j1.sh` helper is not present;
build the existing `build-qnap` directory directly:

```bash
ssh transmission 'cd /root/Transmission && cmake --build build-qnap --target transmission-daemon -j1'
# artifact: /root/Transmission/build-qnap/daemon/transmission-daemon
```

The deployable artifact is always `transmission-daemon`.

After the build, ask whether to deploy and to which of the targets below.

## Deployment

### 1. transmission server

SSH alias: `transmission` (configured in the user's SSH config).

1. Stop the daemon and watchdog services.
2. Back up the current binary with a replacement timestamp, then copy the new `transmission-daemon` to `/usr/local/bin/transmission-daemon`.
3. Start the daemon and watchdog services.

Expected pattern:

```bash
ts=$(date +%Y%m%d-%H%M%S)
systemctl stop transmission-daemon watchdog
cp -p /usr/local/bin/transmission-daemon "/usr/local/bin/transmission-daemon.$ts"
# copy the new binary to /usr/local/bin/transmission-daemon
systemctl start transmission-daemon watchdog
```

### 2. QNAP server

SSH alias: `qnap` from this workstation (admin, uid 0). The QNAP reports `armv5tel`.

The `transmission` server cannot SSH to `qnap` non-interactively (publickey/password
denied), so copy the artifact through this workstation:

```powershell
scp transmission:/root/Transmission/build-qnap/daemon/transmission-daemon .
scp .\transmission-daemon qnap:/share/MD0_DATA/.qpkg/Transmission/bin/transmission-daemon.new
```

Do not use `/tmp` on the QNAP: it is only **32 MB** and will fail with
`No space left on device`. Always stage `.new` in the QPKG `bin` directory (or another
data volume).

QNAP is an old machine with **256 MB RAM** and is slow: use increased timeouts for
`ssh`/`scp` and remote commands.

Then:

```sh
/etc/init.d/transmission.sh stop
cd /share/MD0_DATA/.qpkg/Transmission/bin
ts=$(date +%Y%m%d-%H%M%S)
cp -p transmission-daemon "transmission-daemon.$ts"
mv -f transmission-daemon.new transmission-daemon
chmod 755 transmission-daemon
/etc/init.d/transmission.sh start
```

Verify with `ps | grep transmission-daemon`, `md5sum .../transmission-daemon`, and the
`/usr/bin/transmission-daemon` symlink target.

### 3. Local Windows host: sergeybo02

The agent shell runs as `NT AUTHORITY\LOCAL SERVICE` and is not elevated. Put the
deployment steps in a `.ps1` under `C:\Windows\SERVIC~1\LOCALS~1\AppData\Local\Temp\opencode\`,
then run it elevated and capture a transcript:

```powershell
Start-Process powershell.exe -Verb RunAs -Wait -ArgumentList `
  '-NoProfile','-ExecutionPolicy','Bypass','-File','<script.ps1>'
```

The script must:

1. `Start-Transcript` to a log file in the same temp directory.
2. Stop `Transmission` and wait for `Stopped`.
3. Back up `C:\Program Files\Transmission\transmission-daemon.exe` to
   `...\transmission-daemon.exe.<yyyyMMdd-HHmmss>`.
4. Copy `build\Release\daemon\Release\transmission-daemon.exe` to the installed path.
5. Start the service and wait for `Running`.

Verify `Get-Service Transmission` and compare `Get-FileHash ... -Algorithm MD5` of the
installed binary with the build artifact.

### 4. Remote Windows host: samsung-pc

- DNS: `samsung-pc` resolves to `192.168.1.184`. SMB `445` is open; SSH `22` is closed.
- PsExec: `C:\Users\SERGEYBO\Downloads\SysinternalsSuite\PsExec64.exe`.
- Credentials: read the `machine samsung-pc` entry from the user's `_netrc` secret store.
  Do not print or store the password. Launching the remote process as the user fails;
  run it as SYSTEM with `-s`.
- Copy files through the admin share `\\samsung-pc\C$`. An SMB session already exists;
  `net use` may fail with error 1219 and is not needed.

```powershell
Copy-Item .\build\Release\daemon\Release\transmission-daemon.exe `
  '\\samsung-pc\C$\Program Files\Transmission\transmission-daemon.exe.new'
Copy-Item .\deploy-samsung.ps1 '\\samsung-pc\C$\Windows\Temp\deploy-samsung.ps1'
& 'C:\Users\SERGEYBO\Downloads\SysinternalsSuite\PsExec64.exe' \\samsung-pc `
  -accepteula -nobanner -u <from _netrc> -p <from _netrc> -s `
  C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe `
  -NoProfile -ExecutionPolicy Bypass -File C:\Windows\Temp\deploy-samsung.ps1
```

The remote script stops `Transmission`, backs up to `.exe.<yyyyMMdd-HHmmss>`, moves
`.exe.new` into place, starts the service, and writes `C:\Windows\Temp\deploy-samsung.log`.
Read the log and verify the installed MD5.

## Access status

- `transmission`: SSH alias works; `transmission-daemon` and `watchdog` are active (checked 2026-10-22). Revalidate before deploying.
- QNAP: SSH alias `qnap` works from this workstation (admin, `armv5tel`, checked 2026-10-22). Do not use `/tmp` (only **32 MB**); stage artifacts in the QPKG `bin` directory or another data volume.
- `sergeybo02`: local host; service `Transmission` is running; the agent is not elevated, so deploy via `Start-Process -Verb RunAs`.
- `samsung-pc`: resolves to `192.168.1.184`; use `PsExec64.exe` with `-s` and credentials from `_netrc` machine `samsung-pc`.
