from conan import ConanFile


class SlideioProcessorConan(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        self.requires("gtest/1.17.0")
        self.requires("rapidjson/cci.20250205")
        # Used for its version only: nothing links OpenCV.
        self.requires("opencv/4.14.0")

    def configure(self):
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
