# slideio-processor

A C++ library of image-processing algorithms for images larger than memory,
built on [SlideIO](https://github.com/Booritas/slideio). See
[`docs/project.md`](docs/project.md) for the goal, key decisions and the
document index.

**Research use only. Not for diagnostic use.**

## SlideIO dependency

The library needs SlideIO's scene-source seam (`ISceneSource`,
`SceneMetadata`, `makeScene`), merged in SlideIO PR #86 and not yet in a
SlideIO release. Until it is, SlideIO is the git submodule `extern/slideio`,
pinned to a commit.

The build never compiles the submodule itself. SlideIO is built and installed
to a local prefix with its own `install.py`, and this project finds it with
`find_package(slideio)`, as it would a released package. That keeps the
project on SlideIO's installed headers only, and makes moving to a released
package a change of where the package is found (`requirements.md` UR-BLD-7).

### Getting the source

```sh
git clone --recurse-submodules <this repository>
# or, in an existing clone:
git submodule update --init --recursive
```

### Installing SlideIO from the submodule

SlideIO's own prerequisites apply: Python 3, CMake, and Conan 2 (see
`extern/slideio/README.md`). Pass an absolute prefix; `install.py` resolves a
relative one against the SlideIO directory.

```sh
cd extern/slideio
python3 install.py -a install -c release --prefix <absolute path>/build/slideio-install
```

On Windows without `python3` on `PATH`, use `py -3`.

The project's CMake configuration then finds it with
`-DCMAKE_PREFIX_PATH=<absolute path>/build/slideio-install/release`.

### Moving the pin

```sh
git -C extern/slideio fetch
git -C extern/slideio checkout <commit>
git add extern/slideio
```

Reinstall SlideIO after moving the pin.

## Building

Conan 2 is required. On this Windows setup the executable is not on `PATH`;
call it by its full path. SlideIO must already be installed to the local prefix
(see above).

```sh
conan install . -of build/conan -s build_type=Release -s compiler.cppstd=17 --build=missing
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=build/conan/conan_toolchain.cmake \
      -DCMAKE_PREFIX_PATH=<absolute path>/build/slideio-install/release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Run a single test with `-R <regex>`.

OpenCV comes in through Conan now, but no library links it yet. The first
`./build.sh -c debug` may build OpenCV Debug from source when the Conan cache lacks it.

## Documentation

`include/slideio/processor/core/core.hpp` includes the whole `core` module, and
`include/slideio/processor/config/config.hpp` the whole `config` module. API
documentation is generated from the headers with Doxygen; the `docs` target
exists only when Doxygen is installed:

```sh
cmake --build build --target docs
```

Requirement-to-test coverage is in `docs/traceability.md`.
