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
(see above). libtiff comes in through Conan and is linked privately into
`slideio-processor-data`; the data library links neither SlideIO nor OpenCV.

```sh
conan install . -of build/conan -s build_type=Release -s compiler.cppstd=17 --build=missing
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=build/conan/conan_toolchain.cmake \
      -DCMAKE_PREFIX_PATH=<absolute path>/build/slideio-install/release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Run a single test with `-R <regex>`.

OpenCV comes in through Conan and is linked privately into
`slideio-processor-engine` only. The first `./build.sh -c debug` may build
OpenCV Debug from source when the Conan cache lacks it.

SQLite comes in through Conan and is linked privately into `slideio-processor-data`.

### Warnings as errors

```sh
./build.sh --warnings-as-errors     # -Werror, or /WX on MSVC, for this project's sources
```

With plain CMake, configure with `-DSLIDEIO_PROCESSOR_WARNINGS_AS_ERRORS=ON` (default
OFF). It covers this project's targets, not SlideIO or the Conan dependencies. CI builds
every job with it, so a warning on any of the four compilers fails the build.

### Release builds without test seams

The tests need a few test-only exports in the shared libraries (the
`detail::*ForTesting` functions and `detail::trackForCloseTesting`). For a release
package, build without them:

```sh
./build.sh --no-test-seams          # writes build-noseams; tests are not built
scripts/check-no-test-seams.sh build-noseams
```

The script exits 0 and prints `no test seams exported` when neither library exports a
symbol containing `Testing`, nor any of the six test-only engine functions the tests call
(`notifyTileSkipped`, `takeAwaitedDeferred`, `completeDeferred`, `stageRecordJson`,
`stageFromRecord`, `sameDeclaration`). With plain CMake, configure with
`-DSLIDEIO_PROCESSOR_TEST_SEAMS=OFF`. The default is ON, which `./run-tests.sh` needs.
CI builds this way on Ubuntu after the tests and runs the check.

## Libraries

The project builds two shared libraries:

- **`slideio-processor-data`** — the `core`, `config` and `storage` modules. It
  links libtiff and SQLite privately and links neither SlideIO nor OpenCV; the
  configure step fails if either ever reaches its link line.
- **`slideio-processor-engine`** — the `pipeline` module (operations, the
  pipeline and its tile executor, seam merging, and a pipeline's lazy SlideIO
  scene) and the `algorithm` module (the operation catalog and its registry:
  so far the fixed, Otsu and adaptive thresholds, type conversion, colour
  conversion, stain deconvolution, the Gaussian, median, Sobel, Scharr,
  Laplacian, Canny and bilateral filters, and erode, dilate, open and close,
  registered when the library loads), with `workspace` to join it later. It
  links `slideio-processor-data` and
  `slideio::slideio` **publicly**, because its public headers name their types
  (`slideio::Scene` among them), and `opencv_core` and `opencv_imgproc`
  **privately**. No OpenCV type, header or macro appears in a public header;
  the build compiles every pipeline and algorithm header without an OpenCV
  include path to keep it so. SlideIO's libraries carry their own static
  OpenCV; the two copies never meet, because SlideIO's API passes buffers, not
  OpenCV objects. Under MSVC the engine's configuration must match SlideIO's
  (Release with Release, Debug with Debug), which the build checks.

A consumer of the pipeline links `slideio-processor-engine` and gets the data
library and SlideIO with it.

## Data library only

Setting `SLIDEIO_PROCESSOR_DATA_ONLY=ON` configures only `slideio-processor-data`
(`core`, `config`, `storage`) and `reporting`, with their tests. It needs neither
SlideIO nor OpenCV, and `./build.sh --data-only` builds it into `build-data` without
building SlideIO (test it with `./run-tests.sh --data-only`). A project that embeds
the library, such as a viewer, adds it with `add_subdirectory`:

```cmake
set(SLIDEIO_PROCESSOR_DATA_ONLY ON CACHE BOOL "" FORCE)
set(SLIDEIO_PROCESSOR_TEST_SEAMS OFF CACHE BOOL "" FORCE)
add_subdirectory(path/to/slideio-processor)
target_link_libraries(<target> PRIVATE slideio-processor-data)
```

The consuming project must make sqlite3 (with R-tree enabled), rapidjson, libtiff
and zlib available to `find_package`, together with Threads. The parent's
directory-scoped compile options and definitions, and `CMAKE_MSVC_RUNTIME_LIBRARY`,
apply to the processor's sources. The library lands in `${CMAKE_BINARY_DIR}/bin` (plus
`/<Config>` under multi-config generators) of the parent build, not next to the
parent's executable, so on Windows copy it beside the executable:

```cmake
add_custom_command(TARGET <t> POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:<t>> $<TARGET_FILE_DIR:<t>>
    COMMAND_EXPAND_LISTS)
```

`tests/data_only_consumer` is such a project;
`scripts/check-data-only-consumer.sh build-data/conan` builds and runs it, and CI does
the same.

## Documentation

`include/slideio/processor/core/core.hpp` includes the whole `core` module, and
`include/slideio/processor/config/config.hpp` the whole `config` module,
`include/slideio/processor/storage/storage.hpp` the whole `storage` module, and
`include/slideio/processor/pipeline/pipeline_all.hpp` the whole `pipeline`
module (`pipeline.hpp` is the `Pipeline` class). API
documentation is generated from the headers with Doxygen; the `docs` target
exists only when Doxygen is installed:

```sh
cmake --build build --target docs
```

Requirement-to-test coverage is in `docs/traceability.md`.
