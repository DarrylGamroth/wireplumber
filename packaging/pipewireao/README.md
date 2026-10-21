# Upstream WirePlumber with PipeWireAO

This directory is a small build recipe around upstream WirePlumber. The tracked
runtime source at the repository root matches `upstream-revision` exactly:
`bdc17eb2c419cbe41dbb248cf20cbe93370ee2f7` (WirePlumber 0.5.18 plus upstream
role-policy fixes). The repository remains an ordinary Git fork for upstream
patch development under the repository name `wireplumber` at
<https://github.com/DarrylGamroth/wireplumber>. Its upstream is
<https://gitlab.freedesktop.org/pipewire/wireplumber>. The `docs/AO_*` reviews
describe historical experiments;
they do not define the current runtime or its configuration.

The default source overlay changes only the two Meson pkg-config dependency
names, from `libspa-0.2` and `libpipewire-0.3` to their PipeWireAO equivalents.
It does not change WirePlumber policy, APIs, or test files.

## Build, test, and install

Requirements: Git with the pinned commit available locally, Python 3.9 or later,
Meson, Ninja, a C compiler, pkg-config, tar, and upstream build dependencies
(including GLib and D-Bus for the tests). Supply a PipeWireAO SDK containing its
libraries, pkg-config files, SPA plugins, modules, and stock configuration.
System pkg-config dependencies remain available; PipeWireAO must come from the
specified SDK. Use Linux with an ELF dynamic loader for this recipe.

```sh
python3 packaging/pipewireao/build.py build \
  --sdk "$HOME/.local/opt/pipewireao-sdk" \
  --prefix "$HOME/.local/opt/wireplumber-pipewireao" \
  --workdir /tmp/wireplumber-pipewireao-build --jobs 2
python3 packaging/pipewireao/build.py test \
  --workdir /tmp/wireplumber-pipewireao-build --jobs 2
python3 packaging/pipewireao/build.py install \
  --workdir /tmp/wireplumber-pipewireao-build
```

Both the build workspace and absolute install prefix must be new paths. The
recipe uses `git archive` of the pinned commit, never the current checkout's
runtime files. Build does not install; install is a separate explicit command
and refuses an existing prefix. No service is installed, enabled, or started.
Parallel work defaults to two jobs and is bounded at sixteen.

By default Meson fetches the bundled Lua source through upstream's checksummed
wrap. To avoid downloads, pass `--lua-source /path/to/lua-5.5.0`, using a prepared
source tree that includes its Meson build files. This selects
`--wrap-mode=nodownload` and records a digest of the copied Lua source tree,
including files reached through directory symlinks in the supplied tree.

The test command supplies `PIPEWIREAO_*` paths outside upstream's test files,
using SDK pkg-config metadata, with `SDK/share/pipewire-ao` as the config fallback
when `confdatadir` is absent. SDK symlinks into an artifact store are supported.
The newly built `libwireplumber` directory comes first in the loader path,
followed by the SDK. The wrapper forwards no inherited session endpoints,
tokens, or build flags. Tests use a private local socket directory; they do not
connect to an existing session, start the simulation, or use the GPU.

`WORKDIR/manifest.json` records the upstream URL and revision, archive digest,
patch identities and hashes, build options, tool versions, optional Lua digest,
SDK path, install prefix, and test names/results. It does not record inherited
environment variables or copy raw build logs. Meson keeps its ordinary local
logs in `WORKDIR/build/meson-logs`; review these locally when diagnosing failure.

## Independent optional upstream fixes

The default build applies neither fix. Each patch includes its regression test
and applies independently to the pinned upstream tree:

| Option | Patch | Behavior |
| --- | --- | --- |
| `--dynamic-pod-filter` | `dynamic-pod-filter.patch` | Grow the SPA POD filter builder beyond its initial 1024-byte buffer. |
| `--event-properties` | `event-properties.patch` | Read Lua event properties from the event table, preserving table, WpProperties, and omitted forms. |

Pass either or both options to `build` to opt in. These are generic candidate
upstream fixes, not AO policy. Keep them separate for upstream review; neither
requires the dependency-name overlay when applied to ordinary upstream builds.
The existing `fix/spa-pod-filter-capacity-20261007` branch and prior fork history
remain preserved.

## Site and device policy

Upstream stock WirePlumber scripts and configuration assets install normally.
Choose and maintain site profiles, device rules, and any AO device policy
separately, through normal WirePlumber configuration fragments and explicit
launch configuration. This recipe supplies and selects no AO device policy.
RTC integration should refer to this install's libraries and stock assets;
it must not use the recipe to inject site policy or enable services.

To update the upstream pin, first review the new upstream changes, independently
check the three patch files against that commit, then rebuild in fresh paths
and rerun the tests. Do not reuse an existing source workspace or install prefix.
