# Installing `unsview` on an HPC node

`unsview` is a small native C tool with three runtime dependencies that
exist on essentially every Linux HPC cluster:

| Dependency  | How to find it on HPC                                     |
| ----------- | --------------------------------------------------------- |
| C99 compiler | `gcc` / `cc` is always present on a login node          |
| `libnetcdf`  | Use a `module load`, e.g. `module load netcdf`          |
| `libpng`     | Always installed in `/usr/lib*` — no module needed      |
| X11 / Xt / Xaw (interactive GUI, optional) | Always in `/usr/lib*` |

No Python, no conda, no Java, no JIT, no GPU.

## 1. Unpack

```sh
unzip unsview-conda.zip       # or: tar xzf unsview-conda.tgz
cd unsview
```

## 2. Load modules

The exact module names vary per site. The ones that matter:

```sh
module avail netcdf            # find the right name
module load gcc netcdf         # adjust to your site

# Common variations:
module load netcdf-c           # NCAR Derecho, Casper
module load cray-netcdf        # Perlmutter, Frontier
module load intel netcdf       # Stampede3, Frontera
```

After loading, one of `$NETCDF`, `$NETCDF_DIR`, or `$NETCDF_C_DIR` is
usually set. Check with `nc-config --prefix`.

## 3. Build

The fastest path is the bundled one-shot installer:

```sh
./install.sh                          # installs to $HOME/.local
./install.sh --prefix=$HOME/sw        # or pick a custom prefix
```

It auto-detects the compiler, netCDF (via `nc-config` or common
`$NETCDF*` env vars), libpng, and X11 headers, then builds and installs.

If your site splits X libraries across separate prefixes (common pattern:
each of libXaw / libXt / libX11 / libXmu / libXext lives in its own tree),
point each one explicitly:

```sh
./install.sh \
    --xaw-prefix=/sw/libXaw-1.0.16 \
    --xt-prefix=/sw/libXt-1.3.0 \
    --x11-prefix=/sw/libX11-1.8.7 \
    --xmu-prefix=/sw/libXmu-1.2.1 \
    --xext-prefix=/sw/libXext-1.3.6 \
    --xproto-prefix=/sw/xorgproto-2024.1 \
    --force-x11                          # skip auto-detection
```

`--xproto-prefix` covers the protocol header package (`X11/X.h`,
`X11/Xfuncproto.h`, `X11/Xosdefs.h`). On most distros it's bundled with
libX11-devel, but spack/easybuild typically installs it as a separate
header-only `xorgproto` (or older `xproto`) package.

`--force-x11` skips the Xaw header sanity check (useful when headers and
libs live in non-standard layouts). Drop `--no-x11` to build headless-only.

Run `./install.sh --help` for the full flag list.

### Spack-managed X libraries

Spack installs each package under a long, hashed prefix that ends with the
usual `include/` + `lib/` layout, e.g.

```
/spack/opt/spack/linux-rhel8-x86_64/gcc-12.3.0/libxaw-1.0.16-abc123/
    include/X11/Xaw/Form.h
    lib/libXaw.so.7
```

You have two equivalent options.

**Option A — let Spack set the environment, then build:**

```sh
spack load libxaw libxt libx11 libxmu libxext libsm libice
./install.sh --force-x11
```

`spack load` adds the right entries to `CPATH` and `LIBRARY_PATH`, so the
compiler/linker pick up the headers and libs automatically. `--force-x11`
just tells the install script to enable the GUI build (since the auto-
detector still looks at `/usr/include` and won't see the spack-loaded
paths).

**Option B — pass the spack prefixes explicitly:**

```sh
./install.sh \
    --xaw-prefix=$(spack location -i libxaw)  \
    --xt-prefix=$(spack location -i libxt)    \
    --x11-prefix=$(spack location -i libx11)  \
    --xmu-prefix=$(spack location -i libxmu)  \
    --xext-prefix=$(spack location -i libxext)\
    --sm-prefix=$(spack location -i libsm)    \
    --ice-prefix=$(spack location -i libice)  \
    --force-x11
```

If the spack-installed libs aren't in the runtime linker path, also pass:

```sh
    --x11-ldflags="-Wl,-rpath,$(spack location -i libxaw)/lib \
                   -Wl,-rpath,$(spack location -i libxt)/lib \
                   -Wl,-rpath,$(spack location -i libx11)/lib"
```

so the `unsview` binary finds them at runtime without `LD_LIBRARY_PATH`.

If you'd rather drive `make` yourself:

```sh
make NETCDF_PREFIX="$(nc-config --prefix)"
```

If `nc-config` is unavailable, point `NETCDF_PREFIX` at the install root
manually (the directory that contains `include/netcdf.h` and `lib/libnetcdf.so`).

If the build line ends with `X11 GUI: enabled` you have the interactive
mode; if it says `disabled`, the headless `-o out.png` mode still works.

## 4. Install to user-space

```sh
make install DEST=$HOME/.local
echo 'export PATH=$HOME/.local/bin:$PATH' >> ~/.bashrc
hash -r
which unsview
```

No `sudo` is required. The binary is ~200 KB.

## 5. Run

### Interactive (X11 forwarding)

From your laptop (XQuartz on Mac, X server on Linux/WSL):

```sh
ssh -Y user@hpc
unsview ~/run/x1.40962.diag.2024-01-15_00.00.00.nc \
          --grid ~/grids/x1.40962.grid.nc \
          --coast-data           # auto-loads bundled coast/borders/states
```

`--coast-data` resolves the three bundled overlays from `$UNSVIEW_DATA_DIR`,
then the compile-time install path (`$DEST/share/unsview`), then
`./samples/`. To pick custom files instead use the per-layer flags
(`--coast PATH`, `--borders PATH`, `--states PATH`, or
`--lines PATH:RRGGBB`).

GUI buttons (top toolbar):
- `<< t / t >>` step time index (or 1st extra dim)
- `<< lvl / lvl >>` step vertical level (or 2nd extra dim)
- `cmap` cycles 14 palettes (viridis, plasma, magma, inferno, cividis,
  turbo, jet, rdbu, brbg, seismic, hot, cool, terrain, gray)
- `reset view` global view, `fit data` regional auto-fit, `zoom +/-`
- `set range...` enter exact vmin/vmax; `auto range` reset to data extent
- `save png...` write current canvas (zoom/pan/overlays/cmap exactly as
  displayed) to a PNG
- `coast / borders / states` cycle each overlay: white → black → off

Mouse: drag = pan, scroll wheel = zoom, hover = show lon/lat/cell/value.
Status bar always shows variable, time, level, vmin/vmax, and cmap name.

### Headless (recommended for large meshes / slow links)

```sh
unsview diag.nc --grid grid.nc -v t2m -t 0 -W 1920 -H 960 -o qc.png
scp user@hpc:qc.png .          # or: ssh user@hpc 'cat qc.png' | imgcat
```

### Inside a Slurm batch script

```bash
#!/bin/bash
#SBATCH -t 00:05:00 -n 1
module load gcc netcdf
for t in 0 6 12 18; do
    unsview daily.nc -v t2m -t $t \
              --vmin 250 --vmax 320 \
              -W 1920 -H 960 -o frame_$t.png
done
```

## Troubleshooting

| Symptom | Likely cause and fix |
| ------- | -------------------- |
| `netcdf.h: No such file or directory` | `module load netcdf` not done, or `NETCDF_PREFIX` wrong |
| `cannot find -lnetcdf` at link time   | same — verify with `ls $NETCDF_PREFIX/lib*/libnetcdf.*` |
| `cannot find -lpng`                    | `dnf install libpng-devel` or load a libpng module     |
| `cannot find X11 / Xaw` headers        | only matters for GUI; build still succeeds in headless mode |
| `cannot open display` at runtime       | you didn't `ssh -Y`, or XQuartz/X server isn't running |
| GUI feels laggy over SSH               | switch to headless `-o out.png`; X11 forwarding is bandwidth-bound |

## Login-node etiquette

`unsview` is well within login-node norms:

- Memory: ~20 MB for a 10K-cell mesh, ~100 MB for a 1M-cell mesh
- CPU: idle when not panning; ~100 ms burst per redraw
- I/O: opens netCDF once, reads on demand
- No GPU, no temp files, no spawning

If you batch-render hundreds of PNGs, run that inside a Slurm/PBS job
rather than on the login node.
