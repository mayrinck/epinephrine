# Building epinephrine

## Dependencies

- `meson` >= 1.0.0
- `gtk4` >= 4.10
- `libadwaita` >= 1.4
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
