# Scene Source Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a public, OpenCV-free interface to SlideIO that lets an out-of-tree consumer present a computed raster as a genuine `slideio::Scene`.

**Architecture:** Three new installed declarations — `ISceneSource` (one pure virtual, how to read), `SceneMetadata` (an aggregate, what the scene is) and `makeScene` (the factory) — plus one internal `SourceCVScene : CVScene` that forwards reads to the source and answers metadata getters from the struct. `Scene`, `CVScene`, the drivers and the transformer are untouched. The work is purely additive.

**Tech Stack:** C++17, CMake 3.15+, Conan 2, Google Test, MSVC/GCC/Clang.

**Spec:** [`2026-10-08-scene-source-design.md`](../specs/2026-10-08-scene-source-design.md)

## Scope

**This plan covers the SlideIO repository only** — `D:\Projects\slideio\slideio`.

The spec's §7 (`Pipeline::asScene()`) is **deliberately excluded**. `slideio-processor` currently contains no code at all: no repository, no build, no `Pipeline`, `PipelineSpec`, `BlockView`, `LevelSelector` or `analysisLevel()`. Those are the whole of SDS 05, which is a far larger project than this one, and nothing in §7 can be written or tested until they exist. Planning it now would produce tasks whose every dependency is imaginary.

The SlideIO half is independently valuable and independently shippable: it closes `requirements.md` open question 3, unblocks SDS 05's raster architecture, and is useful to any SlideIO consumer wanting a computed scene. It must land and release before the `slideio-processor` side can begin, so it is the right thing to build first regardless.

**Plan location note.** This file sits beside the spec it implements. SlideIO's own `CLAUDE.md` puts internal engineering docs in `software-docs/plans/`; when work starts in that repository, copy this plan there (and the spec to `software-docs/specs/`) to follow its convention. That also answers the spec's open question §13.2.

## Global Constraints

- **C++17.** `CMAKE_CXX_STANDARD 17`, `CMAKE_CXX_STANDARD_REQUIRED ON` (root `CMakeLists.txt:83-84`).
- **No OpenCV in any installed header.** This is the entire reason the feature exists. `src/public_headers_check/` compiles each public header alone, twice, against `build/public-include` only and *without* `SLIDEIO_INTERNAL_HEADER`.
- **Installed headers must be self-contained.** Include a standard header for every `std::` name used — `<string>`, `<vector>`, `<memory>`, `<list>`, `<cstddef>` — rather than trusting a green local build. `CLAUDE.md` records four headers that were previously wrong in exactly this way.
- **`slideio::Rect`/`Size` are safe in public headers.** They forward-declare the `cv` types and include their `.inl` (which pulls OpenCV) only under `SLIDEIO_INTERNAL_HEADER`. The `.inl` files must never be installed.
- **Making a header public is one line** in `src/slideio/<module>/public-headers.cmake`. That list drives both `install(FILES)` and the compile check.
- **Branch.** `master` is the protected default branch and currently checked out. All work goes on `feature/scene-source`; never commit to `master`. Merge via pull request to `Booritas/slideio`.
- **Additive only.** No existing class changes behaviour, so no `software-docs/BREAKING_CHANGES.md` entry is required.
- **Build:** `python3 install.py -a build -c release` from the repo root.
- **Test binary:** `build/bin/Release/slideio_tests.exe` on Windows; `./build/release/bin/slideio_tests` on Linux/macOS. Filter with `--gtest_filter="SuiteName.TestName"`.
- **No test images.** Every test in this plan is synthetic, so none depends on `SLIDEIO_TEST_DATA_PATH` or the rotated image corpus.

## Review Focus

Input classes the spec implies but does not dwell on. Each has a test assigned to the task that owns the code.

1. **Degenerate geometry** — zero or negative width/height in `sceneRect`, `levelRect` or `blockSize`. This exact class of bug already bit the transformer: `transformerscene.cpp:216-226` guards a division by zero in `computeInflatedRectParams` reached through a degenerate rectangle. Expect a thrown error naming the dimensions, never a crash or a division by zero. *(Task 4, Task 5)*
2. **Rectangles outside the scene** — partly or wholly. `CVScene`'s documented contract is that the part outside is filled with the background value, not that the read is short or throws. The two level paths differ in who does it: a source that declines `readLevelBlock` is clamped and filled by `CVScene::readClampedBlock`, one that implements it is handed the rectangle untouched. *(Task 4, Task 5)*
3. **Buffer too small** — `bufferSize` smaller than the block the adapter computed. Must throw naming both sizes and never write past the end. *(Task 4)*
4. **Channel selection edge cases** — `channelIndices` empty means every channel in order; an out-of-range or duplicated index must throw rather than read out of bounds; channels of differing declared types in one block must be rejected. *(Task 4)*
5. **A source that throws** — an exception out of `readBlock` must propagate to the caller and leave the `Scene` usable for the next read, with nothing leaked and no half-written state. *(Task 4)*

---

### Task 1: `SceneMetadata` — the declaration struct

**Files:**
- Create: `src/slideio/slideio/scenemetadata.hpp`
- Modify: `src/slideio/slideio/public-headers.cmake`
- Modify: `src/slideio/slideio/CMakeLists.txt` (add to `SOURCE_FILES`)
- Create: `src/tests/main/test_scenesource.cpp`
- Modify: `src/tests/main/CMakeLists.txt` (add to `TEST_SOURCES`)

**Interfaces:**
- Consumes: nothing.
- Produces: `struct slideio::SceneMetadata` with the exact field names and defaults below. Tasks 3, 5, 6 and 7 read these fields; Task 7 validates them.

- [ ] **Step 1: Create the feature branch**

```bash
cd /d/Projects/slideio/slideio
git checkout -b feature/scene-source
```

- [ ] **Step 2: Write the failing test**

Create `src/tests/main/test_scenesource.cpp`:

```cpp
// This file is part of slideio project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://slideio.com/license.html.
#include <gtest/gtest.h>
#include "slideio/slideio/scenemetadata.hpp"

using namespace slideio;

TEST(SceneMetadata, defaultsAreTheDocumentedOnes)
{
    SceneMetadata m;
    EXPECT_TRUE(m.name.empty());
    EXPECT_TRUE(m.reportedFilePath.empty());
    EXPECT_TRUE(m.rect.empty());
    EXPECT_TRUE(m.channelDataTypes.empty());
    EXPECT_TRUE(m.channelNames.empty());
    EXPECT_TRUE(m.channelSignificantBits.empty());
    EXPECT_DOUBLE_EQ(0.0, m.resolution.x);
    EXPECT_DOUBLE_EQ(0.0, m.resolution.y);
    EXPECT_DOUBLE_EQ(0.0, m.magnification);
    EXPECT_EQ(Compression::Uncompressed, m.compression);
    EXPECT_EQ(1, m.numZSlices);
    EXPECT_EQ(1, m.numTFrames);
    EXPECT_DOUBLE_EQ(0.0, m.zSliceResolution);
    EXPECT_DOUBLE_EQ(0.0, m.tFrameResolution);
    EXPECT_TRUE(m.levels.empty());
}

TEST(SceneMetadata, isAnAggregateCallersCanFillFieldByField)
{
    SceneMetadata m;
    m.name = "computed";
    m.rect = Rect(0, 0, 400, 300);
    m.channelDataTypes = {DataType::DT_Float32, DataType::DT_Float32};
    m.levels.emplace_back(0, Size(400, 300), 1.0, 20.0, Size(256, 256));

    EXPECT_EQ("computed", m.name);
    EXPECT_EQ(400, m.rect.width);
    EXPECT_EQ(2u, m.channelDataTypes.size());
    EXPECT_EQ(1u, m.levels.size());
    EXPECT_DOUBLE_EQ(1.0, m.levels[0].getScale());
}
```

- [ ] **Step 3: Register the test file**

In `src/tests/main/CMakeLists.txt`, add to `TEST_SOURCES`:

```cmake
  test_scenesource.cpp
```

- [ ] **Step 4: Run the test to verify it fails**

```bash
python3 install.py -a build -c release
```

Expected: compile error, `Cannot open include file: 'slideio/slideio/scenemetadata.hpp'`.

- [ ] **Step 5: Write the header**

Create `src/slideio/slideio/scenemetadata.hpp`:

```cpp
// This file is part of slideio project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://slideio.com/license.html.
#pragma once
#include "slideio/slideio/slideio_def.hpp"
#include "slideio/core/slideio_enums.hpp"
#include "slideio/core/colorprofile.hpp"
#include "slideio/core/levelinfo.hpp"
#include "slideio/core/rect.hpp"
#include "slideio/core/resolution.hpp"
#include <string>
#include <vector>

namespace slideio
{
    /**@brief Everything a scene built by #makeScene reports about itself.
     *
     * A declaration, not a request. Each field is returned verbatim by the
     * corresponding Scene getter: getRect() reports #rect, it does not crop to
     * it; #channelNames is what getChannelName() hands back, not a selector.
     * The pixels come from the ISceneSource handed to makeScene alongside this.
     */
    struct SceneMetadata
    {
        /**@brief Scene::getName().*/
        std::string name;
        /**@brief Scene::getFilePath(). A computed scene has no file; leave it
         * empty, or name the slide the result was derived from.*/
        std::string reportedFilePath;
        /**@brief Scene::getRect(). Must match the size of level 0.*/
        Rect rect;
        /**@brief One entry per channel. Its size defines getNumChannels().*/
        std::vector<DataType> channelDataTypes;
        /**@brief Empty, or one name per channel.*/
        std::vector<std::string> channelNames;
        /**@brief Empty, or one count per channel. 0 means unknown.*/
        std::vector<int> channelSignificantBits;
        /**@brief Pixel size in meters.*/
        Resolution resolution{0.0, 0.0};
        /**@brief Objective magnification. 0 if not meaningful.*/
        double magnification = 0.0;
        /**@brief How the source's pixels were stored, for reporting only.*/
        Compression compression = Compression::Uncompressed;
        /**@brief 1 for a plane scene.*/
        int numZSlices = 1;
        /**@brief 1 for a scene without time frames.*/
        int numTFrames = 1;
        /**@brief Z slice thickness in meters. 0 if not meaningful.*/
        double zSliceResolution = 0.0;
        /**@brief Seconds between time frames. 0 if not meaningful.*/
        double tFrameResolution = 0.0;
        /**@brief The pyramid, finest first. At least one entry; level 0 has
         * scale 1 and the size of #rect.*/
        std::vector<LevelInfo> levels;
        /**@brief Empty means the scene carries no ICC profile.*/
        ColorProfile colorProfile;
    };
}
```

Note the deliberate absence of `SLIDEIO_EXPORTS`: this is a header-only aggregate with no out-of-line members, and exporting a class with `std::vector` members is what the `#pragma warning(disable: 4251)` in `scene.hpp` exists to silence. `makeScene` takes it by `const&`.

- [ ] **Step 6: Register the header as public and as a source**

In `src/slideio/slideio/public-headers.cmake`, add to `SLIDEIO_PUBLIC_HEADERS_slideio`:

```cmake
    scenemetadata.hpp
```

In `src/slideio/slideio/CMakeLists.txt`, add to `SOURCE_FILES`:

```cmake
   ${CMAKE_CURRENT_SOURCE_DIR}/scenemetadata.hpp
```

- [ ] **Step 7: Run the build and the test**

```bash
python3 install.py -a build -c release
./build/bin/Release/slideio_tests.exe --gtest_filter="SceneMetadata.*"
```

Expected: PASS, both tests. The build also compiles `scenemetadata.hpp` standalone twice through `public_headers_check` — if it reaches OpenCV or an internal header, the build fails there rather than in the test.

- [ ] **Step 8: Commit**

```bash
git add src/slideio/slideio/scenemetadata.hpp src/slideio/slideio/public-headers.cmake \
        src/slideio/slideio/CMakeLists.txt src/tests/main/test_scenesource.cpp \
        src/tests/main/CMakeLists.txt
git commit -m "feat: add SceneMetadata, the declaration struct for computed scenes"
```

---

### Task 2: `ISceneSource` — the read interface

**Files:**
- Create: `src/slideio/slideio/scenesource.hpp`
- Modify: `src/slideio/slideio/public-headers.cmake`
- Modify: `src/slideio/slideio/CMakeLists.txt`
- Create: `src/tests/testlib/testscenesource.hpp`
- Modify: `src/tests/main/test_scenesource.cpp`

**Interfaces:**
- Consumes: `slideio::SceneMetadata` (Task 1).
- Produces: `class slideio::ISceneSource` with pure `readBlock(...)`, and defaulted `readLevelBlock(...) -> bool`, `supportsConcurrentReads() -> bool`, `getRawMetadata() -> std::string`, `getAuxImageNames() -> std::list<std::string>`, `getAuxImage(name) -> std::shared_ptr<ISceneSource>`. Also `TestSceneSource`, used by Tasks 3–7.

- [ ] **Step 1: Write the failing test**

Append to `src/tests/main/test_scenesource.cpp`:

```cpp
#include "slideio/slideio/scenesource.hpp"
#include "tests/testlib/testscenesource.hpp"

namespace {
    // The smallest legal source: implements the one pure virtual and nothing else.
    class MinimalSource : public ISceneSource {
    public:
        void readBlock(const Rect&, const Size&, const std::vector<int>&,
                       int, int, void* buffer, size_t bufferSize) override {
            std::memset(buffer, 0, bufferSize);
        }
    };
}

TEST(ISceneSource, aMinimalSourceGetsTheDocumentedDefaults)
{
    MinimalSource s;
    EXPECT_FALSE(s.readLevelBlock(0, Rect(0,0,1,1), Size(1,1), {}, 0, 0, nullptr, 0));
    EXPECT_FALSE(s.supportsConcurrentReads());
    EXPECT_TRUE(s.getRawMetadata().empty());
    EXPECT_TRUE(s.getAuxImageNames().empty());
    EXPECT_EQ(nullptr, s.getAuxImage("anything"));
}
```

Add `#include <cstring>` to the file's includes for `std::memset`.

- [ ] **Step 2: Run the test to verify it fails**

```bash
python3 install.py -a build -c release
```

Expected: compile error, `Cannot open include file: 'slideio/slideio/scenesource.hpp'`.

- [ ] **Step 3: Write the header**

Create `src/slideio/slideio/scenesource.hpp`:

```cpp
// This file is part of slideio project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://slideio.com/license.html.
#pragma once
#include "slideio/slideio/slideio_def.hpp"
#include "slideio/core/rect.hpp"
#include "slideio/core/size.hpp"
#include <cstddef>
#include <list>
#include <memory>
#include <string>
#include <vector>

namespace slideio
{
    /**@brief Supplies the pixels of a scene that is computed rather than decoded.
     *
     * Implement this, describe the result in a SceneMetadata, and #makeScene
     * returns a genuine slideio::Scene that reads through it. The scene is a
     * real Scene -- it passes to convertScene, to a viewer and to the Python
     * bindings unchanged -- it is simply not backed by a file.
     */
    class ISceneSource
    {
    public:
        virtual ~ISceneSource() = default;

        /**@brief Reads a rectangle in SCENE coordinates, resampled to blockSize.
         *
         * The one method every source must implement.
         *
         * @param sceneRect : rectangle in the scene's own coordinate system.
         * Not clamped: SlideIO forwards it exactly as the caller gave it, so
         * the part lying outside the scene must be filled with the background
         * value, not omitted.
         * @param blockSize : size of the result. Equal to sceneRect.size()
         * means no resampling.
         * @param channelIndices : channels to return, in the given order.
         * Never empty: makeScene's adapter expands "every channel" before
         * calling, so an implementation always receives explicit indices.
         * @param zSlice : z slice index, 0 for a plane scene.
         * @param tFrame : time frame index, 0 for a scene without time frames.
         * @param buffer : destination. Row-major, channel-interleaved in the
         * order of @p channelIndices, rows contiguous with no padding, so its
         * length is blockSize.width * blockSize.height * channel count *
         * element size. Every requested channel shares one data type -- a
         * cv::Mat carries a single depth for all of its channels, so a
         * mixed-type request cannot be expressed and the adapter rejects one
         * before calling here. A scene whose channels differ in type is read
         * one channel per call.
         * @param bufferSize : its size in bytes.
         *
         * Report failure by throwing. Never by filling part of the buffer.
         */
        virtual void readBlock(const Rect& sceneRect,
                               const Size& blockSize,
                               const std::vector<int>& channelIndices,
                               int zSlice, int tFrame,
                               void* buffer, size_t bufferSize) = 0;

        /**@brief Reads from an explicitly named pyramid level, in LEVEL coordinates.
         *
         * Return false -- the default -- to let SlideIO derive the read from
         * #readBlock by scaling through LevelInfo::getScale(), which is what
         * CVScene does for a single level driver. Override and return true
         * when the source has a real pyramid and reading the named level
         * directly avoids rounding twice.
         *
         * Parameters are those of #readBlock, with levelRect in the coordinate
         * system of @p level.
         *
         * The two paths differ in who clamps. Declining hands the read to
         * CVScene::readClampedBlock, which clips levelRect to the level
         * bounds, background-fills the remainder and calls #readBlock with
         * the surviving part in scene coordinates. Overriding receives
         * levelRect unclamped and owns its own bounds handling.
         */
        virtual bool readLevelBlock(int level,
                                    const Rect& levelRect,
                                    const Size& blockSize,
                                    const std::vector<int>& channelIndices,
                                    int zSlice, int tFrame,
                                    void* buffer, size_t bufferSize) {
            return false;
        }

        /**@brief Whether reads of one source may overlap.
         *
         * False -- the default -- makes SlideIO serialise them, matching
         * CVScene. Return true only if #readBlock is safe to call from several
         * threads at once.
         */
        virtual bool supportsConcurrentReads() const { return false; }

        /**@brief Scene metadata as the source's own format. Empty by default.*/
        virtual std::string getRawMetadata() const { return std::string(); }

        /**@brief Names of auxiliary images. Empty by default.*/
        virtual std::list<std::string> getAuxImageNames() const {
            return std::list<std::string>();
        }

        /**@brief An auxiliary image by name, or nullptr. nullptr by default.*/
        virtual std::shared_ptr<ISceneSource> getAuxImage(const std::string& name) const {
            return nullptr;
        }
    };
}
```

- [ ] **Step 4: Write the shared test source**

Create `src/tests/testlib/testscenesource.hpp`. This is the synthetic source every later task reads from; its pattern is reproducible from coordinates alone, so a test can assert exact pixel values.

```cpp
// This file is part of slideio project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://slideio.com/license.html.
#pragma once
#include "slideio/slideio/scenesource.hpp"
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace slideio
{
    /**@brief A source whose pixels are a pure function of their coordinates.
     *
     * Byte valued, so a test can predict every pixel: see #expected. Records
     * the arguments of every call so a test can assert what it was asked for.
     */
    class TestSceneSource : public ISceneSource
    {
    public:
        struct Call {
            int level;          // -1 for a readBlock call
            Rect rect;
            Size blockSize;
            std::vector<int> channels;
            int zSlice;
            int tFrame;
        };

        static uint8_t expected(int x, int y, int channel) {
            return static_cast<uint8_t>((x * 7 + y * 13 + channel * 31) & 0xFF);
        }

        void setHasPyramid(bool v)            { m_hasPyramid = v; }
        void setConcurrent(bool v)            { m_concurrent = v; }
        void setThrowOnRead(bool v)           { m_throwOnRead = v; }
        const std::vector<Call>& calls() const { return m_calls; }

        void readBlock(const Rect& sceneRect, const Size& blockSize,
                       const std::vector<int>& channelIndices,
                       int zSlice, int tFrame,
                       void* buffer, size_t bufferSize) override {
            m_calls.push_back({-1, sceneRect, blockSize, channelIndices, zSlice, tFrame});
            fill(sceneRect, blockSize, channelIndices, buffer, bufferSize);
        }

        bool readLevelBlock(int level, const Rect& levelRect, const Size& blockSize,
                            const std::vector<int>& channelIndices,
                            int zSlice, int tFrame,
                            void* buffer, size_t bufferSize) override {
            if (!m_hasPyramid) {
                return false;
            }
            m_calls.push_back({level, levelRect, blockSize, channelIndices, zSlice, tFrame});
            fill(levelRect, blockSize, channelIndices, buffer, bufferSize);
            return true;
        }

        bool supportsConcurrentReads() const override { return m_concurrent; }

    private:
        void fill(const Rect& rect, const Size& blockSize,
                  const std::vector<int>& channels, void* buffer, size_t bufferSize) {
            if (m_throwOnRead) {
                throw std::runtime_error("TestSceneSource: deliberate failure");
            }
            const size_t needed = static_cast<size_t>(blockSize.width) *
                                  blockSize.height * channels.size();
            if (bufferSize < needed) {
                throw std::runtime_error("TestSceneSource: buffer too small");
            }
            uint8_t* out = static_cast<uint8_t*>(buffer);
            for (int row = 0; row < blockSize.height; ++row) {
                for (int col = 0; col < blockSize.width; ++col) {
                    for (size_t c = 0; c < channels.size(); ++c) {
                        *out++ = expected(rect.x + col, rect.y + row, channels[c]);
                    }
                }
            }
        }

        bool m_hasPyramid = false;
        bool m_concurrent = false;
        bool m_throwOnRead = false;
        std::vector<Call> m_calls;
    };
}
```

`fill` assumes `blockSize == rect.size()`, which every test in this plan uses. A resampling test would need its own source.

- [ ] **Step 5: Register the header as public and as a source**

In `src/slideio/slideio/public-headers.cmake`, add `scenesource.hpp`. In `src/slideio/slideio/CMakeLists.txt`, add `${CMAKE_CURRENT_SOURCE_DIR}/scenesource.hpp` to `SOURCE_FILES`.

- [ ] **Step 6: Run the build and the test**

```bash
python3 install.py -a build -c release
./build/bin/Release/slideio_tests.exe --gtest_filter="ISceneSource.*"
```

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add src/slideio/slideio/scenesource.hpp src/slideio/slideio/public-headers.cmake \
        src/slideio/slideio/CMakeLists.txt src/tests/testlib/testscenesource.hpp \
        src/tests/main/test_scenesource.cpp
git commit -m "feat: add ISceneSource, the read interface for computed scenes"
```

---

### Task 3: `SourceCVScene` — metadata forwarding

**Files:**
- Create: `src/slideio/slideio/sourcecvscene.hpp`, `src/slideio/slideio/sourcecvscene.cpp`
- Modify: `src/slideio/slideio/CMakeLists.txt`
- Modify: `src/tests/main/test_scenesource.cpp`

Internal — **not** added to `public-headers.cmake`.

**Interfaces:**
- Consumes: `SceneMetadata` (Task 1), `ISceneSource`, `TestSceneSource` (Task 2).
- Produces: `class slideio::SourceCVScene : public CVScene`, constructor `SourceCVScene(std::shared_ptr<ISceneSource>, SceneMetadata)`. Tasks 4–7 extend and use it.

- [ ] **Step 1: Write the failing test**

Append to `src/tests/main/test_scenesource.cpp`:

```cpp
#include "slideio/slideio/sourcecvscene.hpp"
#include "slideio/slideio/sceneinternal.hpp"
#include "slideio/slideio/scene.hpp"

namespace {
    SceneMetadata describedMetadata()
    {
        SceneMetadata m;
        m.name = "computed";
        m.reportedFilePath = "pipeline://nuclei";
        m.rect = Rect(0, 0, 400, 300);
        m.channelDataTypes = {DataType::DT_Byte, DataType::DT_Byte, DataType::DT_Byte};
        m.channelNames = {"haematoxylin", "eosin", "residual"};
        m.channelSignificantBits = {8, 8, 8};
        m.resolution = Resolution(2.5e-7, 2.5e-7);
        m.magnification = 40.0;
        m.numZSlices = 4;
        m.numTFrames = 5;
        m.zSliceResolution = 2.5e-6;
        m.tFrameResolution = 0.25;
        m.levels.emplace_back(0, Size(400, 300), 1.0,  40.0, Size(256, 256));
        m.levels.emplace_back(1, Size(200, 150), 0.5,  20.0, Size(256, 256));
        return m;
    }

    std::shared_ptr<Scene> sceneOver(const std::shared_ptr<TestSceneSource>& src,
                                     const SceneMetadata& m)
    {
        return SceneInternal::createScene(std::make_shared<SourceCVScene>(src, m));
    }
}

TEST(SourceCVScene, reportsEveryMetadataFieldVerbatim)
{
    auto src = std::make_shared<TestSceneSource>();
    auto scene = sceneOver(src, describedMetadata());

    EXPECT_EQ("computed", scene->getName());
    EXPECT_EQ("pipeline://nuclei", scene->getFilePath());
    EXPECT_EQ(std::make_tuple(0, 0, 400, 300), scene->getRect());
    EXPECT_EQ(3, scene->getNumChannels());
    EXPECT_EQ(DataType::DT_Byte, scene->getChannelDataType(1));
    EXPECT_EQ("eosin", scene->getChannelName(1));
    EXPECT_EQ(8, scene->getChannelSignificantBits(2));
    EXPECT_EQ(std::make_tuple(2.5e-7, 2.5e-7), scene->getResolution());
    EXPECT_DOUBLE_EQ(40.0, scene->getMagnification());
    EXPECT_EQ(4, scene->getNumZSlices());
    EXPECT_EQ(5, scene->getNumTFrames());
    EXPECT_DOUBLE_EQ(2.5e-6, scene->getZSliceResolution());
    EXPECT_DOUBLE_EQ(0.25, scene->getTFrameResolution());
    EXPECT_EQ(Compression::Uncompressed, scene->getCompression());
}

TEST(SourceCVScene, exposesTheDeclaredPyramid)
{
    auto src = std::make_shared<TestSceneSource>();
    auto scene = sceneOver(src, describedMetadata());

    ASSERT_EQ(2, scene->getNumZoomLevels());
    const LevelInfo* level1 = scene->getLevelInfo(1);
    ASSERT_NE(nullptr, level1);
    EXPECT_EQ(200, level1->getSize().width);
    EXPECT_DOUBLE_EQ(0.5, level1->getScale());
    EXPECT_EQ(256, level1->getTileSize().width);
}

TEST(SourceCVScene, levelInfoPointersSurviveTheCallerMetadataGoingAway)
{
    auto src = std::make_shared<TestSceneSource>();
    std::shared_ptr<Scene> scene;
    {
        SceneMetadata m = describedMetadata();
        scene = sceneOver(src, m);
    }   // m destroyed
    const LevelInfo* level0 = scene->getLevelInfo(0);
    ASSERT_NE(nullptr, level0);
    EXPECT_EQ(400, level0->getSize().width);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
python3 install.py -a build -c release
```

Expected: compile error, `Cannot open include file: 'slideio/slideio/sourcecvscene.hpp'`.

- [ ] **Step 3: Write the header**

Create `src/slideio/slideio/sourcecvscene.hpp`:

```cpp
// This file is part of slideio project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://slideio.com/license.html.
#pragma once
#include "slideio/core/cvscene.hpp"
#include "slideio/slideio/scenemetadata.hpp"
#include "slideio/slideio/scenesource.hpp"
#include <memory>
#include <string>
#include <vector>

namespace slideio
{
    /**@brief A CVScene whose pixels come from an ISceneSource.
     *
     * Internal. Consumers reach it only through makeScene(), which is what
     * keeps OpenCV out of the public surface: this header includes
     * cvscene.hpp and is never installed.
     */
    class SourceCVScene : public CVScene
    {
    public:
        SourceCVScene(std::shared_ptr<ISceneSource> source, SceneMetadata metadata);

        std::string getFilePath() const override;
        std::string getName() const override;
        int getSceneIndex() const override;
        const std::string& getDriverId() const override;
        cv::Rect getRect() const override;
        int getNumChannels() const override;
        int getNumZSlices() const override;
        int getNumTFrames() const override;
        DataType getChannelDataType(int channel) const override;
        std::string getChannelName(int channel) const override;
        Resolution getResolution() const override;
        double getZSliceResolution() const override;
        double getTFrameResolution() const override;
        int getChannelSignificantBits(int channel) const override;
        double getMagnification() const override;
        Compression getCompression() const override;
        ColorProfile getColorProfile() const override;
        int getNumZoomLevels() const override;
        const LevelInfo* getZoomLevelInfo(int level) const override;

        void readResampledBlockChannelsEx(const cv::Rect& blockRect, const cv::Size& blockSize,
            const std::vector<int>& componentIndices, int zSliceIndex, int tFrameIndex,
            cv::OutputArray output) override;

    private:
        std::shared_ptr<ISceneSource> m_source;
        SceneMetadata m_metadata;
        std::string m_driverId;
    };
}
```

- [ ] **Step 4: Write the implementation**

Create `src/slideio/slideio/sourcecvscene.cpp`:

```cpp
// This file is part of slideio project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://slideio.com/license.html.
#include "slideio/slideio/sourcecvscene.hpp"
#include "slideio/core/exceptions.hpp"
#include <utility>

using namespace slideio;

SourceCVScene::SourceCVScene(std::shared_ptr<ISceneSource> source, SceneMetadata metadata)
    : m_source(std::move(source)), m_metadata(std::move(metadata)), m_driverId("SOURCE")
{
}

std::string SourceCVScene::getFilePath() const      { return m_metadata.reportedFilePath; }
std::string SourceCVScene::getName() const          { return m_metadata.name; }
int SourceCVScene::getSceneIndex() const            { return 0; }
const std::string& SourceCVScene::getDriverId() const { return m_driverId; }
cv::Rect SourceCVScene::getRect() const             { return m_metadata.rect; }
int SourceCVScene::getNumChannels() const           { return static_cast<int>(m_metadata.channelDataTypes.size()); }
int SourceCVScene::getNumZSlices() const            { return m_metadata.numZSlices; }
int SourceCVScene::getNumTFrames() const            { return m_metadata.numTFrames; }
Resolution SourceCVScene::getResolution() const     { return m_metadata.resolution; }
double SourceCVScene::getZSliceResolution() const   { return m_metadata.zSliceResolution; }
double SourceCVScene::getTFrameResolution() const   { return m_metadata.tFrameResolution; }
double SourceCVScene::getMagnification() const      { return m_metadata.magnification; }
Compression SourceCVScene::getCompression() const   { return m_metadata.compression; }
ColorProfile SourceCVScene::getColorProfile() const { return m_metadata.colorProfile; }

DataType SourceCVScene::getChannelDataType(int channel) const
{
    if (channel < 0 || channel >= getNumChannels()) {
        RAISE_RUNTIME_ERROR << "SourceCVScene: channel " << channel
                            << " out of range [0, " << getNumChannels() << ")";
    }
    return m_metadata.channelDataTypes[channel];
}

std::string SourceCVScene::getChannelName(int channel) const
{
    if (channel < 0 || channel >= static_cast<int>(m_metadata.channelNames.size())) {
        return CVScene::getChannelName(channel);
    }
    return m_metadata.channelNames[channel];
}

int SourceCVScene::getChannelSignificantBits(int channel) const
{
    if (channel < 0 || channel >= static_cast<int>(m_metadata.channelSignificantBits.size())) {
        return 0;
    }
    return m_metadata.channelSignificantBits[channel];
}

int SourceCVScene::getNumZoomLevels() const
{
    return static_cast<int>(m_metadata.levels.size());
}

const LevelInfo* SourceCVScene::getZoomLevelInfo(int level) const
{
    if (level < 0 || level >= getNumZoomLevels()) {
        RAISE_RUNTIME_ERROR << "SourceCVScene: zoom level " << level
                            << " out of range [0, " << getNumZoomLevels() << ")";
    }
    return &m_metadata.levels[level];
}

void SourceCVScene::readResampledBlockChannelsEx(const cv::Rect&, const cv::Size&,
    const std::vector<int>&, int, int, cv::OutputArray)
{
    RAISE_RUNTIME_ERROR << "SourceCVScene: read not implemented yet";
}
```

The read stub keeps the class concrete so Task 3's metadata tests compile and pass; Task 4 replaces it.

`m_metadata` is held **by value**, which is what makes `getZoomLevelInfo` safe to return a pointer into: the scene owns its own copy of the `LevelInfo` vector, exactly as a driver-backed scene owns its levels.

- [ ] **Step 5: Register the sources**

In `src/slideio/slideio/CMakeLists.txt`, add to `SOURCE_FILES`:

```cmake
   ${CMAKE_CURRENT_SOURCE_DIR}/sourcecvscene.hpp
   ${CMAKE_CURRENT_SOURCE_DIR}/sourcecvscene.cpp
```

- [ ] **Step 6: Run the tests**

```bash
python3 install.py -a build -c release
./build/bin/Release/slideio_tests.exe --gtest_filter="SourceCVScene.*"
```

Expected: PASS, all three.

- [ ] **Step 7: Commit**

```bash
git add src/slideio/slideio/sourcecvscene.hpp src/slideio/slideio/sourcecvscene.cpp \
        src/slideio/slideio/CMakeLists.txt src/tests/main/test_scenesource.cpp
git commit -m "feat: add SourceCVScene with metadata forwarding"
```

---

### Task 4: `SourceCVScene` — the read path

Carries Review Focus items 1–5.

**Files:**
- Modify: `src/slideio/slideio/sourcecvscene.cpp:readResampledBlockChannelsEx`
- Modify: `src/tests/main/test_scenesource.cpp`

**Interfaces:**
- Consumes: everything from Task 3.
- Produces: a working `readResampledBlockChannelsEx`, on which `CVScene`'s eleven derived read overloads depend.

- [ ] **Step 1: Write the failing tests**

Append to `src/tests/main/test_scenesource.cpp`:

```cpp
namespace {
    SceneMetadata byteMetadata(int channels = 3)
    {
        SceneMetadata m;
        m.rect = Rect(0, 0, 400, 300);
        m.channelDataTypes.assign(channels, DataType::DT_Byte);
        m.levels.emplace_back(0, Size(400, 300), 1.0, 20.0, Size(256, 256));
        return m;
    }
}

TEST(SourceCVSceneRead, deliversTheSourcePixels)
{
    auto src = std::make_shared<TestSceneSource>();
    auto scene = sceneOver(src, byteMetadata());

    const std::tuple<int,int,int,int> rect(10, 20, 4, 3);
    std::vector<uint8_t> buffer(4 * 3 * 3);
    scene->readBlock(rect, buffer.data(), buffer.size());

    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            for (int c = 0; c < 3; ++c) {
                const size_t i = (static_cast<size_t>(row) * 4 + col) * 3 + c;
                EXPECT_EQ(TestSceneSource::expected(10 + col, 20 + row, c), buffer[i])
                    << "at col " << col << " row " << row << " channel " << c;
            }
        }
    }
}

TEST(SourceCVSceneRead, forwardsARectStraddlingTheEdgeUnclamped)   // Review Focus 2
{
    auto src = std::make_shared<TestSceneSource>();
    auto scene = sceneOver(src, byteMetadata());     // scene is 400x300

    // Straddles the right edge: 2 columns inside, 2 outside.
    std::vector<uint8_t> buffer(4 * 2 * 3);
    ASSERT_NO_THROW(scene->readBlock(std::make_tuple(398, 0, 4, 2),
                                     buffer.data(), buffer.size()));

    // The adapter must hand the source the whole rectangle, not a clipped one:
    // filling the outside part with background is the source's documented job,
    // and a clipped rect would silently shift the pixels the caller gets back.
    ASSERT_EQ(1u, src->calls().size());
    EXPECT_EQ(398, src->calls()[0].rect.x);
    EXPECT_EQ(4, src->calls()[0].rect.width);
}

TEST(SourceCVSceneRead, forwardsARectEntirelyOutsideTheScene)   // Review Focus 2
{
    auto src = std::make_shared<TestSceneSource>();
    auto scene = sceneOver(src, byteMetadata());

    std::vector<uint8_t> buffer(2 * 2 * 3);
    EXPECT_NO_THROW(scene->readBlock(std::make_tuple(1000, 1000, 2, 2),
                                     buffer.data(), buffer.size()));
}

TEST(SourceCVSceneRead, emptyChannelSelectionMeansEveryChannelInOrder)
{
    auto src = std::make_shared<TestSceneSource>();
    auto scene = sceneOver(src, byteMetadata());

    std::vector<uint8_t> buffer(2 * 2 * 3);
    scene->readBlockChannels(std::make_tuple(0, 0, 2, 2), {}, buffer.data(), buffer.size());

    ASSERT_EQ(1u, src->calls().size());
    EXPECT_EQ(std::vector<int>({0, 1, 2}), src->calls()[0].channels);
}

TEST(SourceCVSceneRead, selectsAndOrdersTheRequestedChannels)
{
    auto src = std::make_shared<TestSceneSource>();
    auto scene = sceneOver(src, byteMetadata());

    std::vector<uint8_t> buffer(1 * 1 * 2);
    scene->readBlockChannels(std::make_tuple(5, 6, 1, 1), {2, 0}, buffer.data(), buffer.size());

    EXPECT_EQ(TestSceneSource::expected(5, 6, 2), buffer[0]);
    EXPECT_EQ(TestSceneSource::expected(5, 6, 0), buffer[1]);
}

TEST(SourceCVSceneRead, rejectsAnOutOfRangeChannelIndex)
{
    auto src = std::make_shared<TestSceneSource>();
    auto scene = sceneOver(src, byteMetadata());
    std::vector<uint8_t> buffer(16);
    EXPECT_THROW(scene->readBlockChannels(std::make_tuple(0, 0, 2, 2), {0, 9},
                                          buffer.data(), buffer.size()),
                 slideio::RuntimeError);
}

TEST(SourceCVSceneRead, rejectsChannelsOfDifferingDataTypesInOneBlock)
{
    auto src = std::make_shared<TestSceneSource>();
    SceneMetadata m = byteMetadata(2);
    m.channelDataTypes = {DataType::DT_Byte, DataType::DT_Float32};
    auto scene = sceneOver(src, m);

    std::vector<uint8_t> buffer(64);
    EXPECT_THROW(scene->readBlockChannels(std::make_tuple(0, 0, 2, 2), {0, 1},
                                          buffer.data(), buffer.size()),
                 slideio::RuntimeError);
}

TEST(SourceCVSceneRead, readsAMixedTypeSceneOneChannelAtATime)
{
    // The rejection above costs no reach: every channel of a mixed-type scene
    // is still readable, one per call, which is what keeps UR-IMG-10 true of a
    // computed scene. Only a *simultaneous* multi-channel read is restricted,
    // and that is cv::Mat's single depth, not this adapter's choice.
    auto src = std::make_shared<TestSceneSource>();
    SceneMetadata m = byteMetadata(2);
    m.channelDataTypes = {DataType::DT_Byte, DataType::DT_Float32};
    auto scene = sceneOver(src, m);

    std::vector<uint8_t> byteBuffer(2 * 2);
    EXPECT_NO_THROW(scene->readBlockChannels(std::make_tuple(0, 0, 2, 2), {0},
                                             byteBuffer.data(), byteBuffer.size()));

    std::vector<float> floatBuffer(2 * 2);
    EXPECT_NO_THROW(scene->readBlockChannels(std::make_tuple(0, 0, 2, 2), {1},
                                             floatBuffer.data(),
                                             floatBuffer.size() * sizeof(float)));
}

TEST(SourceCVSceneRead, rejectsADegenerateBlock)   // Review Focus 1
{
    auto src = std::make_shared<TestSceneSource>();
    auto scene = sceneOver(src, byteMetadata());
    std::vector<uint8_t> buffer(16);
    EXPECT_THROW(scene->readBlock(std::make_tuple(0, 0, 0, 5), buffer.data(), buffer.size()),
                 slideio::RuntimeError);
    EXPECT_THROW(scene->readBlock(std::make_tuple(0, 0, 5, -1), buffer.data(), buffer.size()),
                 slideio::RuntimeError);
}

TEST(SourceCVSceneRead, rejectsABufferSmallerThanTheBlock)   // Review Focus 3
{
    auto src = std::make_shared<TestSceneSource>();
    auto scene = sceneOver(src, byteMetadata());
    std::vector<uint8_t> tooSmall(5);            // needs 4*3*3 = 36
    EXPECT_THROW(scene->readBlock(std::make_tuple(0, 0, 4, 3), tooSmall.data(), tooSmall.size()),
                 slideio::RuntimeError);
}

TEST(SourceCVSceneRead, aThrowingSourceLeavesTheSceneUsable)   // Review Focus 5
{
    auto src = std::make_shared<TestSceneSource>();
    auto scene = sceneOver(src, byteMetadata());
    std::vector<uint8_t> buffer(2 * 2 * 3);

    src->setThrowOnRead(true);
    EXPECT_THROW(scene->readBlock(std::make_tuple(0, 0, 2, 2), buffer.data(), buffer.size()),
                 std::exception);

    src->setThrowOnRead(false);
    EXPECT_NO_THROW(scene->readBlock(std::make_tuple(0, 0, 2, 2), buffer.data(), buffer.size()));
    EXPECT_EQ(TestSceneSource::expected(0, 0, 0), buffer[0]);
}

TEST(SourceCVSceneRead, servesA4DReadFromASourceThatImplementsOnlyReadBlock)
{
    auto src = std::make_shared<TestSceneSource>();
    SceneMetadata m = byteMetadata();
    m.numZSlices = 3;
    auto scene = sceneOver(src, m);

    std::vector<uint8_t> buffer(2 * 2 * 3 * 2);   // two z slices
    scene->read4DBlock(std::make_tuple(0, 0, 2, 2), std::make_tuple(0, 2),
                       std::make_tuple(0, 1), buffer.data(), buffer.size());

    ASSERT_EQ(2u, src->calls().size());
    EXPECT_EQ(0, src->calls()[0].zSlice);
    EXPECT_EQ(1, src->calls()[1].zSlice);
}
```

Add `#include "slideio/core/exceptions.hpp"` to the test file.

- [ ] **Step 2: Run the tests to verify they fail**

```bash
python3 install.py -a build -c release
./build/bin/Release/slideio_tests.exe --gtest_filter="SourceCVSceneRead.*"
```

Expected: every test FAILS with "SourceCVScene: read not implemented yet".

- [ ] **Step 3: Write the implementation**

Replace the stub in `src/slideio/slideio/sourcecvscene.cpp`, and add the includes:

```cpp
#include "slideio/core/tools/cvtools.hpp"
#include <numeric>
```

```cpp
void SourceCVScene::readResampledBlockChannelsEx(const cv::Rect& blockRect,
    const cv::Size& blockSize, const std::vector<int>& componentIndices,
    int zSliceIndex, int tFrameIndex, cv::OutputArray output)
{
    if (blockSize.width <= 0 || blockSize.height <= 0) {
        RAISE_RUNTIME_ERROR << "SourceCVScene: block size must be positive, got "
                            << blockSize.width << "x" << blockSize.height;
    }
    const int numChannels = getNumChannels();
    std::vector<int> channels(componentIndices);
    if (channels.empty()) {
        channels.resize(numChannels);
        std::iota(channels.begin(), channels.end(), 0);
    }
    for (int channel : channels) {
        if (channel < 0 || channel >= numChannels) {
            RAISE_RUNTIME_ERROR << "SourceCVScene: channel " << channel
                                << " out of range [0, " << numChannels << ")";
        }
    }
    // The type comes from the channels the caller asked for, deliberately not
    // from CVScene::initializeSceneBlock, which takes channel 0's type
    // whatever was requested (cvscene.cpp:330) and would size this block
    // wrongly for a mixed-type scene -- the source then writes elements of a
    // different width into it and overruns the buffer. Do not "simplify" this
    // by calling the base helper. See the design document 6.1.
    const DataType dataType = m_metadata.channelDataTypes[channels.front()];
    for (int channel : channels) {
        if (m_metadata.channelDataTypes[channel] != dataType) {
            RAISE_RUNTIME_ERROR << "SourceCVScene: all channels of one block must share a "
                                   "data type; channel " << channels.front() << " is "
                                << dataType << " and channel " << channel << " is "
                                << m_metadata.channelDataTypes[channel];
        }
    }

    const int depth = CVTools::toOpencvType(dataType);
    output.create(blockSize, CV_MAKETYPE(depth, static_cast<int>(channels.size())));
    cv::Mat mat = output.getMat();
    if (!mat.isContinuous()) {
        RAISE_RUNTIME_ERROR << "SourceCVScene: output buffer is not continuous";
    }
    m_source->readBlock(Rect(blockRect), Size(blockSize.width, blockSize.height),
                        channels, zSliceIndex, tFrameIndex,
                        mat.data, mat.total() * mat.elemSize());
}
```

Three notes. `Rect(blockRect)` uses the in-tree `cv::Rect` conversion from `rect.inl`, available because `SLIDEIO_INTERNAL_HEADER` is defined for the whole in-tree build. The buffer-too-small case of Review Focus 3 surfaces from `Scene::readBlock`'s own size check before reaching here, which is why that test asserts on `Scene` rather than on the adapter. And the background fill of Review Focus 2 is `CVScene`'s existing behaviour for rectangles outside the scene — the adapter passes the rectangle through, and the source is documented to fill the outside part.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
./build/bin/Release/slideio_tests.exe --gtest_filter="SourceCVSceneRead.*"
```

Expected: PASS, all twelve.

- [ ] **Step 5: Run the whole suite for regressions**

```bash
./build/bin/Release/slideio_tests.exe
```

Expected: no new failures against the pre-task baseline.

- [ ] **Step 6: Commit**

```bash
git add src/slideio/slideio/sourcecvscene.cpp src/tests/main/test_scenesource.cpp
git commit -m "feat: forward SourceCVScene block reads to the ISceneSource"
```

---

### Task 5: `SourceCVScene` — level reads

Carries Review Focus item 1 for level coordinates.

**Files:**
- Modify: `src/slideio/slideio/sourcecvscene.hpp`, `src/slideio/slideio/sourcecvscene.cpp`
- Modify: `src/tests/main/test_scenesource.cpp`

**Interfaces:**
- Consumes: Task 4's read path.
- Produces: `SourceCVScene::readResampledLevelBlockChannelsEx` override.

- [ ] **Step 1: Write the failing tests**

Append to `src/tests/main/test_scenesource.cpp`:

```cpp
namespace {
    SceneMetadata twoLevelMetadata()
    {
        SceneMetadata m;
        m.rect = Rect(0, 0, 400, 300);
        m.channelDataTypes.assign(3, DataType::DT_Byte);
        m.levels.emplace_back(0, Size(400, 300), 1.0, 40.0, Size(256, 256));
        m.levels.emplace_back(1, Size(200, 150), 0.5, 20.0, Size(256, 256));
        return m;
    }
}

TEST(SourceCVSceneLevels, asksASourceWithAPyramidDirectly)
{
    auto src = std::make_shared<TestSceneSource>();
    src->setHasPyramid(true);
    auto scene = sceneOver(src, twoLevelMetadata());

    std::vector<uint8_t> buffer(2 * 2 * 3);
    scene->readResampledLevelBlockChannels(1, std::make_tuple(10, 10, 2, 2),
                                           std::make_tuple(2, 2), {}, buffer.data(), buffer.size());

    ASSERT_EQ(1u, src->calls().size());
    EXPECT_EQ(1, src->calls()[0].level);                  // the named level, not -1
    EXPECT_EQ(10, src->calls()[0].rect.x);                // level coordinates, unscaled
}

TEST(SourceCVSceneLevels, derivesTheReadWhenTheSourceDeclinesTheLevel)
{
    auto src = std::make_shared<TestSceneSource>();
    src->setHasPyramid(false);                            // readLevelBlock returns false
    auto scene = sceneOver(src, twoLevelMetadata());

    std::vector<uint8_t> buffer(2 * 2 * 3);
    scene->readResampledLevelBlockChannels(1, std::make_tuple(10, 10, 2, 2),
                                           std::make_tuple(2, 2), {}, buffer.data(), buffer.size());

    ASSERT_EQ(1u, src->calls().size());
    EXPECT_EQ(-1, src->calls()[0].level);                 // came through readBlock
    EXPECT_EQ(20, src->calls()[0].rect.x);                // scaled to scene coordinates by 1/0.5
}

// The two level paths clamp differently, and an implementer who assumes
// otherwise ships wrong edge tiles. CVScene::readClampedBlock is reached only
// from the base readResampledLevelBlockChannelsEx (cvscene.cpp:233): a source
// that DECLINES the level is clamped and background-filled for it, and one
// that IMPLEMENTS the level is handed the rectangle untouched. Level 1 is
// 200x150, so (190, 140, 20, 20) hangs 10 px off both edges.

TEST(SourceCVSceneLevels, aSourceThatImplementsTheLevelGetsTheRectUnclamped)
{
    auto src = std::make_shared<TestSceneSource>();
    src->setHasPyramid(true);
    auto scene = sceneOver(src, twoLevelMetadata());

    std::vector<uint8_t> buffer(20 * 20 * 3);
    scene->readResampledLevelBlockChannels(1, std::make_tuple(190, 140, 20, 20),
                                           std::make_tuple(20, 20), {},
                                           buffer.data(), buffer.size());

    ASSERT_EQ(1u, src->calls().size());
    EXPECT_EQ(190, src->calls()[0].rect.x);               // not clipped to 190..200
    EXPECT_EQ(20,  src->calls()[0].rect.width);           // the source owns the overhang
    EXPECT_EQ(20,  src->calls()[0].blockSize.width);
}

TEST(SourceCVSceneLevels, aSourceThatDeclinesTheLevelGetsAClampedSceneRect)
{
    auto src = std::make_shared<TestSceneSource>();
    src->setHasPyramid(false);
    auto scene = sceneOver(src, twoLevelMetadata());

    std::vector<uint8_t> buffer(20 * 20 * 3);
    scene->readResampledLevelBlockChannels(1, std::make_tuple(190, 140, 20, 20),
                                           std::make_tuple(20, 20), {},
                                           buffer.data(), buffer.size());

    ASSERT_EQ(1u, src->calls().size());
    EXPECT_EQ(-1,  src->calls()[0].level);                // came through readBlock
    EXPECT_EQ(380, src->calls()[0].rect.x);               // clipped, then /0.5 to scene
    EXPECT_EQ(20,  src->calls()[0].rect.width);           // 10 level px -> 20 scene px
    EXPECT_EQ(10,  src->calls()[0].blockSize.width);      // only the surviving part

    // CVScene filled the overhang itself: 255 for an 8-bit block.
    const size_t outside = (0 * 20 + 15) * 3;             // x = 15 is past the edge
    EXPECT_EQ(255, buffer[outside]);
}

TEST(SourceCVSceneLevels, bothPathsAgreeAtLevelZero)
{
    SceneMetadata m = twoLevelMetadata();

    auto direct = std::make_shared<TestSceneSource>();
    direct->setHasPyramid(true);
    auto derived = std::make_shared<TestSceneSource>();
    derived->setHasPyramid(false);

    std::vector<uint8_t> a(4 * 4 * 3), b(4 * 4 * 3);
    sceneOver(direct, m)->readResampledLevelBlockChannels(
        0, std::make_tuple(8, 8, 4, 4), std::make_tuple(4, 4), {}, a.data(), a.size());
    sceneOver(derived, m)->readResampledLevelBlockChannels(
        0, std::make_tuple(8, 8, 4, 4), std::make_tuple(4, 4), {}, b.data(), b.size());

    EXPECT_EQ(a, b);
}

TEST(SourceCVSceneLevels, rejectsAnOutOfRangeLevel)
{
    auto src = std::make_shared<TestSceneSource>();
    src->setHasPyramid(true);
    auto scene = sceneOver(src, twoLevelMetadata());

    std::vector<uint8_t> buffer(2 * 2 * 3);
    EXPECT_THROW(scene->readResampledLevelBlockChannels(
                     5, std::make_tuple(0, 0, 2, 2), std::make_tuple(2, 2), {},
                     buffer.data(), buffer.size()),
                 slideio::RuntimeError);
}

TEST(SourceCVSceneLevels, rejectsADegenerateLevelBlock)   // Review Focus 1
{
    auto src = std::make_shared<TestSceneSource>();
    src->setHasPyramid(true);
    auto scene = sceneOver(src, twoLevelMetadata());

    std::vector<uint8_t> buffer(16);
    EXPECT_THROW(scene->readResampledLevelBlockChannels(
                     0, std::make_tuple(0, 0, 4, 4), std::make_tuple(0, 4), {},
                     buffer.data(), buffer.size()),
                 slideio::RuntimeError);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
python3 install.py -a build -c release
./build/bin/Release/slideio_tests.exe --gtest_filter="SourceCVSceneLevels.*"
```

Expected: `asksASourceWithAPyramidDirectly` FAILS — the call arrives with level `-1`, because `CVScene`'s base implementation scales down to `readResampledBlockChannelsEx` and the source is never asked. The others may pass already; they are the regression net for this change.

- [ ] **Step 3: Add the override to the header**

In `src/slideio/slideio/sourcecvscene.hpp`, below `readResampledBlockChannelsEx`:

```cpp
        void readResampledLevelBlockChannelsEx(int level, const cv::Rect& levelRect,
            const cv::Size& blockSize, const std::vector<int>& channelIndices,
            int zSliceIndex, int tFrameIndex, cv::OutputArray output) override;
```

- [ ] **Step 4: Write the implementation**

Append to `src/slideio/slideio/sourcecvscene.cpp`:

```cpp
void SourceCVScene::readResampledLevelBlockChannelsEx(int level, const cv::Rect& levelRect,
    const cv::Size& blockSize, const std::vector<int>& channelIndices,
    int zSliceIndex, int tFrameIndex, cv::OutputArray output)
{
    validateLevel(level);
    if (blockSize.width <= 0 || blockSize.height <= 0) {
        RAISE_RUNTIME_ERROR << "SourceCVScene: block size must be positive, got "
                            << blockSize.width << "x" << blockSize.height;
    }

    const int numChannels = getNumChannels();
    std::vector<int> channels(channelIndices);
    if (channels.empty()) {
        channels.resize(numChannels);
        std::iota(channels.begin(), channels.end(), 0);
    }
    for (int channel : channels) {
        if (channel < 0 || channel >= numChannels) {
            RAISE_RUNTIME_ERROR << "SourceCVScene: channel " << channel
                                << " out of range [0, " << numChannels << ")";
        }
    }

    const DataType dataType = m_metadata.channelDataTypes[channels.front()];
    const int depth = CVTools::toOpencvType(dataType);
    output.create(blockSize, CV_MAKETYPE(depth, static_cast<int>(channels.size())));
    cv::Mat mat = output.getMat();
    if (!mat.isContinuous()) {
        RAISE_RUNTIME_ERROR << "SourceCVScene: output buffer is not continuous";
    }

    const bool served = m_source->readLevelBlock(
        level, Rect(levelRect), Size(blockSize.width, blockSize.height),
        channels, zSliceIndex, tFrameIndex, mat.data, mat.total() * mat.elemSize());
    if (served) {
        return;
    }
    // The source has no pyramid of its own. Fall back to the base, which maps
    // the level rectangle to scene coordinates through LevelInfo::getScale()
    // and comes back through readResampledBlockChannelsEx.
    CVScene::readResampledLevelBlockChannelsEx(level, levelRect, blockSize, channels,
                                               zSliceIndex, tFrameIndex, output);
}
```

`validateLevel` is `CVScene`'s existing helper, declared in its **protected** section (`cvscene.hpp:332`) and so available to this derived class. It is the same one `TransformerScene::readResampledLevelBlockChannelsEx` calls.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
python3 install.py -a build -c release
./build/bin/Release/slideio_tests.exe --gtest_filter="SourceCVSceneLevels.*"
```

Expected: PASS, all seven.

- [ ] **Step 6: Commit**

```bash
git add src/slideio/slideio/sourcecvscene.hpp src/slideio/slideio/sourcecvscene.cpp \
        src/tests/main/test_scenesource.cpp
git commit -m "feat: let a source with a real pyramid serve level reads directly"
```

---

### Task 6: Concurrency and the optional extras

**Files:**
- Modify: `src/slideio/slideio/sourcecvscene.hpp`, `src/slideio/slideio/sourcecvscene.cpp`
- Modify: `src/tests/main/test_scenesource.cpp`

**Interfaces:**
- Consumes: Task 5's `SourceCVScene`.
- Produces: `supportsConcurrentReads`, `getRawMetadata`, `getAuxImageNames` overrides.

- [ ] **Step 1: Write the failing tests**

Append to `src/tests/main/test_scenesource.cpp`:

```cpp
TEST(SourceCVSceneConcurrency, defaultsToSerialisedReads)
{
    auto src = std::make_shared<TestSceneSource>();     // concurrent = false
    SourceCVScene adapter(src, byteMetadata());
    EXPECT_FALSE(adapter.supportsConcurrentReads());
}

TEST(SourceCVSceneConcurrency, reportsConcurrencyWhenTheSourceDoes)
{
    auto src = std::make_shared<TestSceneSource>();
    src->setConcurrent(true);
    SourceCVScene adapter(src, byteMetadata());
    EXPECT_TRUE(adapter.supportsConcurrentReads());
}

TEST(SourceCVSceneExtras, forwardsRawMetadataAndAuxImageNames)
{
    class DescribedSource : public TestSceneSource {
    public:
        std::string getRawMetadata() const override { return "<meta/>"; }
        std::list<std::string> getAuxImageNames() const override { return {"label", "macro"}; }
    };
    auto src = std::make_shared<DescribedSource>();
    SourceCVScene adapter(src, byteMetadata());

    EXPECT_EQ("<meta/>", adapter.getRawMetadata());
    ASSERT_EQ(2u, adapter.getAuxImageNames().size());
    EXPECT_EQ("label", adapter.getAuxImageNames().front());
    EXPECT_EQ(2, adapter.getNumAuxImages());
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
python3 install.py -a build -c release
./build/bin/Release/slideio_tests.exe --gtest_filter="SourceCVSceneConcurrency.*:SourceCVSceneExtras.*"
```

Expected: the concurrency and metadata tests FAIL — the base returns `false` and an empty string regardless of the source.

- [ ] **Step 3: Add the overrides to the header**

```cpp
        bool supportsConcurrentReads() const override;
        std::string getRawMetadata() const override;
        const std::list<std::string>& getAuxImageNames() const override;
```

Add `#include <list>` to the header.

- [ ] **Step 4: Write the implementation**

Append to `src/slideio/slideio/sourcecvscene.cpp`:

```cpp
bool SourceCVScene::supportsConcurrentReads() const
{
    return m_source->supportsConcurrentReads();
}

std::string SourceCVScene::getRawMetadata() const
{
    return m_source->getRawMetadata();
}

const std::list<std::string>& SourceCVScene::getAuxImageNames() const
{
    // Cached because the base returns a reference. Sources are expected to
    // report a fixed set, as every driver does.
    std::call_once(m_auxNamesOnce, [this]() {
        m_auxNames = m_source->getAuxImageNames();
    });
    return m_auxNames;
}
```

Add to the header's private section, and `#include <mutex>`:

```cpp
        mutable std::once_flag m_auxNamesOnce;
        mutable std::list<std::string> m_auxNames;
```

`std::call_once` rather than a plain lazy `if` because `getAuxImageNames` may be called from a read path on several threads at once — the same reasoning `LevelInfo::getTileCount`'s comment records for its own cache, and the pattern `CVScene::m_metadata` already uses.

Auxiliary *images* are deliberately not wired: `CVScene::getAuxImage` returns a `std::shared_ptr<CVScene>`, so forwarding would mean wrapping the aux `ISceneSource` in another `SourceCVScene` with metadata nobody has declared. `ISceneSource::getAuxImage` stays in the interface for a later task; until then the names are reported and the images are not retrievable. Note this in the release note.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
python3 install.py -a build -c release
./build/bin/Release/slideio_tests.exe --gtest_filter="SourceCVSceneConcurrency.*:SourceCVSceneExtras.*"
```

Expected: PASS, all three.

- [ ] **Step 6: Commit**

```bash
git add src/slideio/slideio/sourcecvscene.hpp src/slideio/slideio/sourcecvscene.cpp \
        src/tests/main/test_scenesource.cpp
git commit -m "feat: forward concurrency and raw metadata from the scene source"
```

---

### Task 7: `makeScene` and its validation

**Files:**
- Create: `src/slideio/slideio/scenesourcefactory.cpp`
- Modify: `src/slideio/slideio/scenesource.hpp` (declare `makeScene`)
- Modify: `src/slideio/slideio/CMakeLists.txt`
- Modify: `src/tests/main/test_scenesource.cpp`

**Interfaces:**
- Consumes: `SourceCVScene` (Tasks 3–6), `SceneInternal::createScene`.
- Produces: `std::shared_ptr<Scene> slideio::makeScene(std::shared_ptr<ISceneSource>, const SceneMetadata&)` — the only entry point a consumer uses.

- [ ] **Step 1: Write the failing tests**

Append to `src/tests/main/test_scenesource.cpp`:

```cpp
TEST(MakeScene, returnsAUsableSceneForValidMetadata)
{
    auto src = std::make_shared<TestSceneSource>();
    auto scene = makeScene(src, byteMetadata());

    ASSERT_NE(nullptr, scene);
    EXPECT_EQ(std::make_tuple(0, 0, 400, 300), scene->getRect());
    EXPECT_EQ(3, scene->getNumChannels());

    std::vector<uint8_t> buffer(2 * 2 * 3);
    scene->readBlock(std::make_tuple(0, 0, 2, 2), buffer.data(), buffer.size());
    EXPECT_EQ(TestSceneSource::expected(0, 0, 0), buffer[0]);
}

TEST(MakeScene, readsNoPixelsAtConstruction)
{
    auto src = std::make_shared<TestSceneSource>();
    auto scene = makeScene(src, byteMetadata());
    EXPECT_TRUE(src->calls().empty());
}

TEST(MakeScene, rejectsANullSource)
{
    EXPECT_THROW(makeScene(nullptr, byteMetadata()), slideio::RuntimeError);
}

TEST(MakeScene, rejectsEmptyOrUnknownChannelTypes)
{
    auto src = std::make_shared<TestSceneSource>();

    SceneMetadata none = byteMetadata();
    none.channelDataTypes.clear();
    EXPECT_THROW(makeScene(src, none), slideio::RuntimeError);

    SceneMetadata unknown = byteMetadata();
    unknown.channelDataTypes[1] = DataType::DT_Unknown;
    EXPECT_THROW(makeScene(src, unknown), slideio::RuntimeError);
}

TEST(MakeScene, rejectsMismatchedPerChannelVectors)
{
    auto src = std::make_shared<TestSceneSource>();

    SceneMetadata names = byteMetadata();
    names.channelNames = {"only-one"};               // 3 channels declared
    EXPECT_THROW(makeScene(src, names), slideio::RuntimeError);

    SceneMetadata bits = byteMetadata();
    bits.channelSignificantBits = {8, 8};            // 3 channels declared
    EXPECT_THROW(makeScene(src, bits), slideio::RuntimeError);
}

TEST(MakeScene, rejectsADegenerateRect)
{
    auto src = std::make_shared<TestSceneSource>();
    SceneMetadata m = byteMetadata();
    m.rect = Rect(0, 0, 0, 300);
    EXPECT_THROW(makeScene(src, m), slideio::RuntimeError);
}

TEST(MakeScene, rejectsAnEmptyPyramid)
{
    auto src = std::make_shared<TestSceneSource>();
    SceneMetadata m = byteMetadata();
    m.levels.clear();
    EXPECT_THROW(makeScene(src, m), slideio::RuntimeError);
}

TEST(MakeScene, rejectsALevelZeroThatDisagreesWithTheRect)
{
    auto src = std::make_shared<TestSceneSource>();

    SceneMetadata scale = byteMetadata();
    scale.levels[0].setScale(0.5);
    EXPECT_THROW(makeScene(src, scale), slideio::RuntimeError);

    SceneMetadata size = byteMetadata();
    size.levels[0].setSize(Size(200, 300));
    EXPECT_THROW(makeScene(src, size), slideio::RuntimeError);
}

TEST(MakeScene, rejectsNonDecreasingLevelScales)
{
    auto src = std::make_shared<TestSceneSource>();
    SceneMetadata m = twoLevelMetadata();
    m.levels[1].setScale(1.0);                       // not smaller than level 0
    EXPECT_THROW(makeScene(src, m), slideio::RuntimeError);
}

TEST(MakeScene, rejectsNonPositiveSliceAndFrameCounts)
{
    auto src = std::make_shared<TestSceneSource>();

    SceneMetadata z = byteMetadata();
    z.numZSlices = 0;
    EXPECT_THROW(makeScene(src, z), slideio::RuntimeError);

    SceneMetadata t = byteMetadata();
    t.numTFrames = -1;
    EXPECT_THROW(makeScene(src, t), slideio::RuntimeError);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
python3 install.py -a build -c release
```

Expected: compile error, `makeScene: identifier not found`.

- [ ] **Step 3: Declare `makeScene`**

At the end of `src/slideio/slideio/scenesource.hpp`, inside `namespace slideio`, after the class:

```cpp
    class Scene;

    /**@brief Wraps an ISceneSource in a genuine slideio::Scene.
     *
     * Performs no image work: no file is opened, nothing is cropped, no
     * channel is selected. Every field of @p metadata is reported verbatim by
     * the corresponding Scene getter, and pixels are fetched from @p source
     * on demand.
     *
     * @param source : supplies the pixels. Must not be null.
     * @param metadata : what the resulting scene reports about itself.
     * @return a Scene usable anywhere a file backed one is, convertScene and
     * the Python bindings included.
     *
     * Throws RuntimeError if the metadata is not internally consistent --
     * deliberately at construction, so a malformed scene fails here rather
     * than at its first tile read.
     */
    std::shared_ptr<Scene> SLIDEIO_EXPORTS makeScene(std::shared_ptr<ISceneSource> source,
                                                     const SceneMetadata& metadata);
```

Add `#include "slideio/slideio/scenemetadata.hpp"` to `scenesource.hpp`.

- [ ] **Step 4: Write the implementation**

Create `src/slideio/slideio/scenesourcefactory.cpp`:

```cpp
// This file is part of slideio project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://slideio.com/license.html.
#include "slideio/slideio/scenesource.hpp"
#include "slideio/slideio/scenemetadata.hpp"
#include "slideio/slideio/sceneinternal.hpp"
#include "slideio/slideio/sourcecvscene.hpp"
#include "slideio/core/exceptions.hpp"
#include <cmath>

using namespace slideio;

namespace
{
    void validate(const std::shared_ptr<ISceneSource>& source, const SceneMetadata& m)
    {
        if (!source) {
            RAISE_RUNTIME_ERROR << "makeScene: source must not be null";
        }
        if (m.channelDataTypes.empty()) {
            RAISE_RUNTIME_ERROR << "makeScene: channelDataTypes must name at least one channel";
        }
        const int numChannels = static_cast<int>(m.channelDataTypes.size());
        for (int channel = 0; channel < numChannels; ++channel) {
            const DataType dt = m.channelDataTypes[channel];
            if (dt == DataType::DT_Unknown || dt == DataType::DT_None) {
                RAISE_RUNTIME_ERROR << "makeScene: channelDataTypes[" << channel
                                    << "] is not a usable data type";
            }
        }
        if (!m.channelNames.empty() &&
            static_cast<int>(m.channelNames.size()) != numChannels) {
            RAISE_RUNTIME_ERROR << "makeScene: channelNames has " << m.channelNames.size()
                                << " entries for " << numChannels << " channels";
        }
        if (!m.channelSignificantBits.empty() &&
            static_cast<int>(m.channelSignificantBits.size()) != numChannels) {
            RAISE_RUNTIME_ERROR << "makeScene: channelSignificantBits has "
                                << m.channelSignificantBits.size()
                                << " entries for " << numChannels << " channels";
        }
        if (m.rect.width <= 0 || m.rect.height <= 0) {
            RAISE_RUNTIME_ERROR << "makeScene: rect must have positive size, got "
                                << m.rect.width << "x" << m.rect.height;
        }
        if (m.levels.empty()) {
            RAISE_RUNTIME_ERROR << "makeScene: levels must describe at least level 0";
        }
        if (std::fabs(m.levels[0].getScale() - 1.0) > 1.e-6) {
            RAISE_RUNTIME_ERROR << "makeScene: level 0 must have scale 1, got "
                                << m.levels[0].getScale();
        }
        if (m.levels[0].getSize().width != m.rect.width ||
            m.levels[0].getSize().height != m.rect.height) {
            RAISE_RUNTIME_ERROR << "makeScene: level 0 size "
                                << m.levels[0].getSize().width << "x"
                                << m.levels[0].getSize().height
                                << " differs from rect " << m.rect.width << "x" << m.rect.height;
        }
        for (size_t level = 0; level < m.levels.size(); ++level) {
            const LevelInfo& info = m.levels[level];
            if (info.getSize().width <= 0 || info.getSize().height <= 0) {
                RAISE_RUNTIME_ERROR << "makeScene: level " << level << " has non-positive size";
            }
            if (info.getScale() <= 0.0) {
                RAISE_RUNTIME_ERROR << "makeScene: level " << level << " has non-positive scale";
            }
            if (info.getTileSize().width < 0 || info.getTileSize().height < 0) {
                RAISE_RUNTIME_ERROR << "makeScene: level " << level << " has a negative tile size";
            }
            if (level > 0 && info.getScale() >= m.levels[level - 1].getScale()) {
                RAISE_RUNTIME_ERROR << "makeScene: level scales must decrease with index; level "
                                    << level << " has scale " << info.getScale()
                                    << " against level " << (level - 1) << "'s "
                                    << m.levels[level - 1].getScale();
            }
        }
        if (m.numZSlices < 1) {
            RAISE_RUNTIME_ERROR << "makeScene: numZSlices must be at least 1, got " << m.numZSlices;
        }
        if (m.numTFrames < 1) {
            RAISE_RUNTIME_ERROR << "makeScene: numTFrames must be at least 1, got " << m.numTFrames;
        }
    }
}

std::shared_ptr<Scene> slideio::makeScene(std::shared_ptr<ISceneSource> source,
                                          const SceneMetadata& metadata)
{
    validate(source, metadata);
    auto adapter = std::make_shared<SourceCVScene>(std::move(source), metadata);
    return SceneInternal::createScene(adapter);
}
```

- [ ] **Step 5: Register the source**

In `src/slideio/slideio/CMakeLists.txt`, add to `SOURCE_FILES`:

```cmake
   ${CMAKE_CURRENT_SOURCE_DIR}/scenesourcefactory.cpp
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
python3 install.py -a build -c release
./build/bin/Release/slideio_tests.exe --gtest_filter="MakeScene.*"
```

Expected: PASS, all ten.

- [ ] **Step 7: Commit**

```bash
git add src/slideio/slideio/scenesource.hpp src/slideio/slideio/scenesourcefactory.cpp \
        src/slideio/slideio/CMakeLists.txt src/tests/main/test_scenesource.cpp
git commit -m "feat: add makeScene with construction-time metadata validation"
```

---

### Task 8: End-to-end through the converter, and the release note

Proves the point of the whole feature: a computed scene is accepted by code that knows nothing about it.

**Files:**
- Create: `src/tests/converter/test_sourcescene_conversion.cpp`
- Modify: `src/tests/converter/CMakeLists.txt`
- Modify: `docs/_posts/` (a release note) or `software-docs/` as the maintainer prefers
- Modify: `CLAUDE.md` (public header count)

**Interfaces:**
- Consumes: `makeScene` (Task 7).
- Produces: nothing new in code.

- [ ] **Step 1: Write the failing test**

Create `src/tests/converter/test_sourcescene_conversion.cpp`:

```cpp
// This file is part of slideio project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://slideio.com/license.html.
#include <gtest/gtest.h>
#include "slideio/slideio/slideio.hpp"
#include "slideio/slideio/scenesource.hpp"
#include "slideio/slideio/scenemetadata.hpp"
#include "slideio/converter/converter.hpp"
#include "slideio/converter/converterparameters.hpp"
#include "tests/testlib/testscenesource.hpp"
#include "tests/testlib/testtools.hpp"
#include <filesystem>
#include <vector>

using namespace slideio;

TEST(SourceSceneConversion, aComputedSceneConvertsToOmeTiffAndReadsBack)
{
    auto source = std::make_shared<TestSceneSource>();

    SceneMetadata m;
    m.name = "computed";
    m.rect = Rect(0, 0, 512, 512);
    m.channelDataTypes.assign(3, DataType::DT_Byte);
    m.resolution = Resolution(2.5e-7, 2.5e-7);
    m.magnification = 20.0;
    m.levels.emplace_back(0, Size(512, 512), 1.0, 20.0, Size(256, 256));

    auto scene = makeScene(source, m);

    const std::string outPath =
        (std::filesystem::temp_directory_path() / "slideio_source_scene.ome.tiff").string();
    std::filesystem::remove(outPath);

    // Concrete parameter classes, as every test in src/tests/converter/ uses.
    // Uncompressed rather than Jpeg: the Jpeg variants require 8-bit and this
    // test is the template for float results, which cannot use them (spec 9.1).
    converter::OMETIFFConverterParameters parameters(Compression::Uncompressed);
    ASSERT_NO_THROW(converter::convertScene(scene, parameters, outPath, 4));
    ASSERT_TRUE(std::filesystem::exists(outPath));

    auto reopened = openSlide(outPath, "AUTO");
    ASSERT_NE(nullptr, reopened);
    auto readBack = reopened->getScene(0);
    ASSERT_NE(nullptr, readBack);
    EXPECT_EQ(512, std::get<2>(readBack->getRect()));
    EXPECT_EQ(512, std::get<3>(readBack->getRect()));
    EXPECT_EQ(3, readBack->getNumChannels());

    std::vector<uint8_t> buffer(4 * 4 * 3);
    readBack->readBlock(std::make_tuple(0, 0, 4, 4), buffer.data(), buffer.size());
    EXPECT_EQ(TestSceneSource::expected(0, 0, 0), buffer[0]);
    EXPECT_EQ(TestSceneSource::expected(1, 0, 0), buffer[3]);

    std::filesystem::remove(outPath);
}
```

Add `#include "slideio/core/slideio_enums.hpp"` for `Compression`.

- [ ] **Step 2: Register the test**

In `src/tests/converter/CMakeLists.txt`, add `test_sourcescene_conversion.cpp` to `TEST_SOURCES`.

- [ ] **Step 3: Run the test**

```bash
python3 install.py -a build -c release
./build/bin/Release/slideio_converter_tests.exe --gtest_filter="SourceSceneConversion.*"
```

Expected: PASS. If it fails inside the converter rather than in the setup, that is a real finding about the converter's expectations of a scene — record it before changing the test.

- [ ] **Step 4: Run every suite**

```bash
./build/bin/Release/slideio_tests.exe
./build/bin/Release/slideio_converter_tests.exe
./build/bin/Release/slideio_transformer_tests.exe
```

Expected: no new failures against the pre-branch baseline.

- [ ] **Step 5: Update the public header count**

`CLAUDE.md` says "37 headers ship in the `-dev` package." Two were added, so change it to 39. Verify against the build's own report:

```bash
python3 install.py -a configure-only -c release 2>&1 | grep "Public header check"
```

Expected: `Public header check: 39 headers`. Use whatever number it prints.

- [ ] **Step 6: Write the release note**

Add a short note recording the new public surface — `ISceneSource`, `SceneMetadata`, `makeScene` — that it is additive with no behaviour change to existing classes, and the one current limitation: auxiliary image *names* are forwarded but the images themselves are not retrievable through `Scene::getAuxImage`. Follow the existing convention in `docs/_posts/`; no `BREAKING_CHANGES.md` entry is needed.

- [ ] **Step 7: Commit and open the pull request**

```bash
git add src/tests/converter/test_sourcescene_conversion.cpp src/tests/converter/CMakeLists.txt \
        CLAUDE.md docs/_posts/
git commit -m "test: convert a computed scene end to end; document the new public surface"
git push -u origin feature/scene-source
gh pr create --repo Booritas/slideio --base master \
  --title "Add ISceneSource: present a computed raster as a slideio::Scene" \
  --body "Implements docs/superpowers/specs/2026-10-08-scene-source-design.md from slideio-processor. Additive only: three new installed declarations and one internal adapter. No existing class changes behaviour."
```

---

## Follow-on work, not in this plan

- **`Pipeline::asScene()`** (spec §7) — blocked until `slideio-processor` has a `pipeline` module. Needs its own plan once SDS 05 is implemented.
- **Auxiliary images through `ISceneSource::getAuxImage`** — the interface carries the method; the adapter does not wire it, because an aux `ISceneSource` has no declared `SceneMetadata`. Resolving it means either a metadata-per-aux-image map or an aux-specific factory.
- **Resampling sources.** `TestSceneSource` assumes `blockSize == rect.size()`. A source asked to resample is legal by the interface but untested here.
- **The buffer copy at the `cv::Mat` boundary** (spec §9.2) — measure before optimising.
- **The `convertScene` gap** (spec §9.1) — float writes uncompressed, SVS rejects mixed channel types. Worth an `Assumption 3` amendment in `slideio-processor`, not a change here.
