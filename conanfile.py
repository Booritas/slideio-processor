from conan import ConanFile


class SlideioProcessorConan(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        self.requires("gtest/1.17.0")
        self.requires("rapidjson/cci.20250205")
        # The store engine. SlideIO's converter pins the same version.
        self.requires("sqlite3/3.44.2")
        # Used for its version only: nothing links OpenCV.
        self.requires("opencv/4.14.0")

    def configure(self):
        # The storage module's spatial index is an R*Tree virtual table. It is
        # on by default; stated here so the requirement is visible (UR-BLD-5).
        self.options["sqlite3"].enable_rtree = True
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
