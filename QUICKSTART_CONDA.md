# unsview — Conda Build Quickstart (Ubuntu)

Everything needed to build `unsview` as a conda package on a clean Ubuntu
machine. Estimated time: ~10-15 minutes including the conda install.

## 1. Get the source

```sh
git clone https://github.com/bonstandardxyz/unsview.git
cd unsview
```

## Fastest path (one script)

After installing Miniconda + conda-build (step 2 below), the whole
build → install → smoke-test cycle is automated:

```sh
./conda_test.sh
```

It builds the package, installs it into a scratch env `unsview-test`,
and asserts: `unsview -h` works, the overlay data installed, a headless
PNG renders, overlays are on by default (vs `--no-coast-data`), and the
time-varying sample renders. It prints `PASS` on success. The manual
steps below are the same thing, broken out.

## 2. Install Miniconda + conda-build (one-time)

```sh
wget https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh
bash Miniconda3-latest-Linux-x86_64.sh -b -p $HOME/miniconda3
source $HOME/miniconda3/bin/activate
conda install -n base -c conda-forge conda-build -y
```

## 3. Build the package

From the `unsview/` directory (which contains `recipe/`):

```sh
conda build recipe -c conda-forge
```

The build downloads compilers + `libnetcdf` + `libpng` + the X11 stack
into an isolated env, compiles, and emits a `.conda` artifact under
`$CONDA_PREFIX/conda-bld/linux-64/`.

Watch for:
- **OK**: `Test passed.` and `anaconda upload ...` (last lines).
- **Fail**: link errors → see "Likely fixes" below.

## 4. Install + smoke test

```sh
conda create -n unsview-test -c local -c conda-forge unsview -y
conda activate unsview-test

unsview -h
unsview samples/synthetic.nc -v wave -t 0 -o /tmp/wave.png
ls -la /tmp/wave.png       # should be ~10-100 KB
```

GUI (only meaningful if you have a local display or X-forwarding):

```sh
unsview samples/synthetic.nc -v wave
```

For a real mesh, download one from the [MPAS-Atmosphere mesh
downloads](https://mpas-dev.github.io/atmosphere/atmosphere_meshes.html) page —
see the README's "Getting sample data" section.

## 5. (Optional) Replicate conda-forge CI exactly

This is the same image their bot uses, so any failure here predicts a
staged-recipes PR failure:

```sh
docker run --rm -v "$PWD":/work -w /work \
    condaforge/linux-anvil-x86_64:alma9 \
    bash -c "conda build recipe -c conda-forge"
```

## Likely fixes if it fails

1. **`X11/Xaw/Form.h: No such file`**
   The Makefile's auto-detect missed Xaw under the conda prefix. Edit
   `recipe/build.sh`, change the `make -j...` line to:
   ```sh
   make -j"${CPU_COUNT:-2}" V=1 HAVE_X11=1
   ```

2. **`undefined reference to ...XawForm` (or similar)**
   Library order or a missing dep. Check the link line; if `-lXaw`
   appears before `-lXt`, the Makefile is fine — the issue is more
   likely a missing `xorg-libxpm` (some distros pull it transitively).
   Add `xorg-libxpm  # [linux]` under the `host:` and `run:` blocks of
   `recipe/meta.yaml` and rebuild.

3. **`netcdf.h: No such file`**
   `NETCDF_PREFIX` not propagated. Confirm `build.sh` exports it. The
   conda env keeps headers under `$PREFIX/include`, libs under
   `$PREFIX/lib`.

4. **Test step fails: `coastlines_110m.txt missing`**
   The `make install` step didn't copy overlay files. Confirm the
   Makefile's `install:` target ran. `samples/coastlines_110m.txt`,
   `borders_50m.txt`, `states_50m.txt` are committed to the repo.

## What's in the repo

```
unsview/
  src/                  C sources
  Makefile              build + install rules (supports PREFIX/DEST/DATADIR)
  conda_test.sh         one-shot build + install + smoke-test script
  install.sh            plain-make installer for HPC (no conda)
  LICENSE               MIT
  README.md             user-facing readme
  INSTALL_HPC.md        notes for HPC plain-make installs
  QUICKSTART_CONDA.md   this file
  recipe/
    meta.yaml           conda recipe (sources from path: ..)
    build.sh            invokes the Makefile with PREFIX=$PREFIX
    README.md           recipe-specific notes
  samples/
    synthetic.nc            tiny time-varying mesh (wave, 3 steps), 77 KB
    coastlines_110m.txt     bundled overlay data
    borders_50m.txt
    states_50m.txt
```

Real MPAS meshes are deliberately **not** committed — they run from 19 MB to
hundreds of MB. Download them from the [MPAS-Atmosphere mesh
downloads](https://mpas-dev.github.io/atmosphere/atmosphere_meshes.html) page.

## When the local build is green

Tag a release on GitHub and switch `recipe/meta.yaml`'s `source:`
block from `path: ..` to a `url: ... + sha256: ...` pair pointing at
the tagged tarball. That same recipe can then be submitted to
`conda-forge/staged-recipes` for public distribution.
