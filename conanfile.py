from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class JournalSeedRecipe(ConanFile):
    name = "journalseed"
    version = "0.1.0"
    package_type = "application"
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "with_server": [True, False],
        "with_tests": [True, False],
        "with_benchmarks": [True, False],
    }
    default_options = {
        "with_server": True,
        "with_tests": True,
        "with_benchmarks": False,
        "drogon/*:with_postgres": True,
        "drogon/*:with_orm": True,
        "drogon/*:with_sqlite": False,
        "drogon/*:with_mysql": False,
    }

    def requirements(self):
        self.requires("glaze/7.8.4")
        self.requires("libsodium/1.0.22")
        # sol2/3.5.0's Conan recipe still pins 5.4.6. Lua 5.4 keeps ABI
        # compatibility across patch releases, so the application owns the
        # graph-level patch upgrade required by this project.
        self.requires("lua/5.4.8", override=True)
        self.requires("mpdecimal/4.0.0")
        self.requires("sol2/3.5.0")
        if self.options.with_server:
            self.requires("drogon/1.9.13")
        if self.options.with_tests:
            self.requires("catch2/3.8.1")
        if self.options.with_benchmarks:
            self.requires("benchmark/1.9.4")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()

        toolchain = CMakeToolchain(self)
        toolchain.variables["JOURNALSEED_BUILD_SERVER"] = bool(self.options.with_server)
        toolchain.variables["JOURNALSEED_BUILD_TESTS"] = bool(self.options.with_tests)
        toolchain.variables["JOURNALSEED_BUILD_BENCHMARKS"] = bool(self.options.with_benchmarks)
        toolchain.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
