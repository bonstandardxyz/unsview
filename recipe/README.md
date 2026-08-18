# Local conda build for unsview

This recipe builds unsview from the working tree (`source: path: ..`),
so you can iterate without tagging a GitHub release.

## Prerequisites

```sh
conda install -n base conda-build
```

## Build (native — Mac or Ubuntu)

From the project root:

```sh
conda build recipe
```

The artifact lands in `$CONDA_PREFIX/conda-bld/<platform>/unsview-0.1.0-*.conda`.

## Install + smoke test in a fresh env

```sh
conda create -n unsview-test -c local unsview
conda activate unsview-test

unsview -h
unsview samples/x1.10242.static.nc -v ter -t 0 -o /tmp/ter.png
```

The GUI is included on Linux and macOS alike; try:

```sh
unsview samples/x1.10242.static.nc -v ter
```

On macOS the conda X11 packages supply client libraries but no X server, so
this also needs XQuartz (`brew install --cask xquartz`, then log out and back
in). Without it you get `Can't open display`, which is a *runtime* gap — the
GUI is compiled in either way.

## Test inside the conda-forge Linux build container

This is the same image conda-forge CI uses. Lets you catch glibc /
compiler-pin issues before publishing:

```sh
docker run --rm -v "$PWD":/work -w /work \
    condaforge/linux-anvil-x86_64:alma9 \
    bash -c "conda build recipe -c conda-forge"
```

A native macOS conda build now exercises the X11 stack too (conda-forge ships
osx-64/osx-arm64 `xorg-libxaw`), so it does catch most recipe mistakes. Use the
container when you need the Linux package's own pins and glibc validated. Check
either build log for `X11 GUI: enabled`.

## When you're ready to publish

1. Tag `v0.1.0` on GitHub and let GitHub make the source tarball.
2. In `meta.yaml`, swap the `source: path: ..` block for:
   ```yaml
   source:
     url: https://github.com/bonstandardxyz/unsview/archive/v{{ version }}.tar.gz
     sha256: <output of `shasum -a 256 v0.1.0.tar.gz`>
   ```
3. Open a PR against `conda-forge/staged-recipes` with this `recipe/`
   directory copied in as `recipes/unsview/`.
