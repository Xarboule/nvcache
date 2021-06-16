# NVCache

This repository is the implementation of [this research paper](https://arxiv.org/abs/2105.10397).

NVCache is a modified libc, designed to use Persistent Main Memory (PMEM) with legacy softwares, as a bi-directional hybrid I/O cache in userland.

Our implementation is a fork of the [musl libc](https://www.musl-libc.org/).

# How to use it

## Hardware requirements :

To test this library, you need at least one PMEM module. In our case, we used Intel Optane DCPMM.
The PMEM module must be exposed as a DAX (direct access) block device in the system. For example, `/dev/dax1.0`.
Your machine must support the `clwb` instruction.

## On a regular machine :

To avoid breaking your entire system with unexpected behaviors, you must use a glibc based Linux distribution (i.e. anything but Alpine, as far as I know). This way, you can install the modified musl library alongside your system's libc with `make -j && sudo make install` in the repository folder.

In order to run software on this modified libc, you need to have its musl-backed executable. If you have it already compiled, you can rut it directly. Otherwise, you will have to install the [musl-cross-make toolchain](https://github.com/richfelker/musl-cross-make). Then, compile your software with this toolchain, and run the executable.

## In a docker container :

It is also possible to obtain some already compiled softwares in the [Alpine Linux](https://alpinelinux.org/) repositories. The easiest way to use them with NVCache, is to install them in your Dockerfile, and add a last `COPY` command in your Dockerfile to replace the container's libc with the one located in `nvcache/lib/libc.so`. You will also have to expose the PMEM device inside the container.

_Disclaimer :_ This tool was designed as a "proof of concept" that helped us to show the interesting behavior PMEM could have in I/O intensive legacy programs in our research paper. It is not supposed to become a production tool, it is not stable, and many softwares will probably crash for some obscure reason. We only tried to have the support for our experimental softwares. If you want to try another software, do it at your own risk.
