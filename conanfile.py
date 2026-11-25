from conan import ConanFile


class SlideioProcessorConan(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"
    # data_only matches SLIDEIO_PROCESSOR_DATA_ONLY (build.sh --data-only): the data
    # library alone, which needs no OpenCV package (its version is the pin below).
    options = {"data_only": [True, False]}
    default_options = {"data_only": False}

    def requirements(self):
        self.requires("gtest/1.17.0")
        self.requires("rapidjson/cci.20250205")
        # The store engine. SlideIO's converter pins the same version.
        self.requires("sqlite3/3.44.2")
        # Raster TIFF I/O for the storage module (ruling S40). libtiff directly, never
        # SlideIO or OpenCV: the SlideIO DLLs embed OpenCV statically, and design 5.1
        # keeps OpenCV out of the data library. 4.7.1 is ConanCenter's 4.7 release with
        # TIFFClientOpenExt and TIFFOpenOptions (4.5+); SlideIO pins 4.6.0, but both
        # copies are static and private, so they need not match.
        self.requires("libtiff/4.7.1")
        # Storage uses zlib directly (the raster spool's compression, ruling S46), so it is
        # a direct requirement: through libtiff alone its headers would not be visible.
        # The version libtiff/4.7.1 resolves.
        self.requires("zlib/1.3.2")
        # Used for its version only: nothing links OpenCV. A data-only build skips the
        # package, but CMake reads this line for the version, so keep it textually as is.
        if not self.options.data_only:
            self.requires("opencv/4.14.0")

    def configure(self):
        # The storage module's spatial index is an R*Tree virtual table. It is
        # on by default; stated here so the requirement is visible (UR-BLD-5).
        self.options["sqlite3"].enable_rtree = True
        # Rasters are written with None, LZW or Deflate only (ruling S42), so libtiff
        # needs zlib and no other codec, and no C++ stream API. The recipe has no lerc
        # option: it always builds libtiff with lerc off.
        self.options["libtiff"].zlib = True
        self.options["libtiff"].jpeg = False
        for name in ["jbig", "lzma", "zstd", "webp", "libdeflate", "cxx"]:
            setattr(self.options["libtiff"], name, False)
        if self.options.data_only:
            return
        # The options SlideIO's profiles set, so the OpenCV binary SlideIO
        # already built in the same Conan cache is reused.
        disabled = [
            "ml", "dnn", "gapi", "flann", "photo", "video", "calib3d",
            "videoio", "imgcodecs", "objdetect", "stitching",
            "with_png", "with_tiff", "with_webp", "with_quirc", "with_ffmpeg",
            "with_openexr", "with_protobuf", "with_flatbuffers",
            "with_imgcodec_hdr", "with_imgcodec_pfm", "with_imgcodec_pxm",
            "with_imgcodec_sunraster",
        ]
        if self.settings.os == "Linux":
            disabled.append("with_wayland")
        for name in disabled:
            setattr(self.options["opencv"], name, False)
