# Building epinephrine

## Dependencies

- `meson` >= 1.0.0
- `gtk4` >= 4.12
- `libadwaita` >= 1.5
- C compiler with C17 support (gcc, clang)

On Fedora:

```sh
sudo dnf install meson gtk4-devel libadwaita-devel
```

## Build

```sh
meson setup _build
meson compile -C _build
```

The binary will be at `_build/src/epinephrine`.

## Install

```sh
meson install -C _build
```

## Debug build (Address Sanitizer)

```sh
meson setup _build_asan -Db_sanitize=address -Db_lundef=false
meson compile -C _build_asan
```

Run with ASAN:

```sh
./_build_asan/src/epinephrine
```

## Rebuild after changes

```sh
meson compile -C _build
```

## Clean

```sh
rm -rf _build _build_asan
```

## Administrative helper

The UI never elevates a shell or a script from the checkout. `epi-admin` embeds
its restricted operation dispatcher and must be installed in the configured
libexec directory, owned by root with no group/other write permissions. Python 3
and Polkit (`pkexec`) are required for administrative features.

After building, install through your package or run:

```sh
sudo meson install -C _build
```

An uninstalled development build can still browse the library and read ordinary
hardware information. Administrative operations report that installation is
required. No setuid bit is used. The helper only dispatches validated operations
for package installation, supported device attributes, DMI, keyboard controls,
configuration files and the SMBus GRUB option; arbitrary shell commands and
user-provided scripts are refused.

## Tests

```sh
meson test -C _build --print-errorlogs
```

The GTK tests skip with code 77 without a display. Under a headless X server:

```sh
xvfb-run -a meson test -C _build --print-errorlogs
```

The library regression test uses 3,000 fictitious games, isolated XDG directories
and a fake metadata provider. Administrative tests validate policies and temporary
files without applying system changes. CI also builds with address/undefined
behavior sanitizers.
