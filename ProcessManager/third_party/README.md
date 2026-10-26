# Vendored dependencies

Built in-tree, so an airgapped machine needs nothing installed for them. Each folder is an upstream release,
unmodified; to update one, replace its folder with another release and update its record below. The root
`.gitattributes` keeps every file here byte-for-byte. The `.gitignore` files of libzmq and cxxopts hide some of
their own release files, so adding a release takes `git add -f`.

| Package | Version | License | Files kept | Used for | Source |
|---|---|---|---|---|---|
| libzmq | 4.3.5 | MPL-2.0 (`libzmq/LICENSE`) | all | ZeroMQ, linked statically | https://github.com/zeromq/libzmq, tag `v4.3.5` |
| cppzmq | 4.10.0 | MIT (`cppzmq/LICENSE`) | all | ZeroMQ's C++ API, header-only | https://github.com/zeromq/cppzmq, tag `v4.10.0` |
| cxxopts | 3.3.1 | MIT (`cxxopts/LICENSE`) | all | the command line, header-only | https://github.com/jarro2783/cxxopts, tag `v3.3.1` |
| fkYAML | 0.4.4 | MIT (`fkYAML/LICENSE.txt`) | `CMakeLists.txt`, `cmake/*.in`, `single_include/fkYAML/node.hpp`, `LICENSE.txt` | the configuration file, header-only | https://github.com/fktn-k/fkYAML, tag `v0.4.4` |
| GoogleTest | 1.18.0 | BSD-3-Clause (`googletest/LICENSE`) | all | the tests | https://github.com/google/googletest, tag `v1.18.0` |

The root `CMakeLists.txt` adds each folder before the directories that link it. All but fkYAML are added with
`EXCLUDE_FROM_ALL`, so only the libraries the project links are built and none of their install rules run. The
include directories of libzmq, cppzmq, cxxopts and fkYAML count as system ones, set through a target property that
works with any CMake version (`add_subdirectory(... SYSTEM)` needs 3.25), so their warnings stay out of ours.

## libzmq 4.3.5
The folder is the tree of tag `v4.3.5` (commit `622fc6dde99ee172ebaa9c8628d85a7a1995a21d`, git tree
`9411fba97e708a51c94d7c32d848f09a38649d42`, file modes included). The release archive made from it,
`https://github.com/zeromq/libzmq/archive/v4.3.5.tar.gz`, has the SHA-512
`108d9c5fa761c111585c30f9c651ed92942dda0ac661155bca52cc7b6dbeb3d27b0dd994abde206eacfc3bc88d19ed24e45b291050c38469e34dca5f8c9a037d`,
the hash vcpkg's `zeromq` 4.3.5 port pins.

Only the static library, `libzmq-static`, is built. The root turns off `BUILD_SHARED`, `BUILD_TESTS`,
`WITH_PERF_TOOL`, `WITH_DOCS`, `WITH_LIBBSD`, `ENABLE_DRAFTS` and `ENABLE_CPACK`, so the library needs nothing
beyond the C and C++ runtimes and the programs need no libzmq at run time. CURVE stays off, its default.

Two of its settings reach past its folder, and the root handles both. With no `CMAKE_BUILD_TYPE`, it makes Release
the build type of the whole build; the root restores the empty one. Its `cmake_minimum_required(VERSION
2.8.12)` is refused by CMake 4, so the root raises it with `CMAKE_POLICY_VERSION_MINIMUM` while adding the folder.
CMake 3.27 to 3.31 do not read that variable and print a deprecation warning about the line instead; it is harmless.

## cppzmq 4.10.0
The folder is the tree of tag `v4.10.0` (commit `c94c20743ed7d4aa37835a5c46567ab0790d4acc`, git tree
`6e4f353c367fbc9e7b7a2ed3c04ee9d04fdf9de4`, file modes included). vcpkg's `cppzmq` port has moved on to 4.11.0, so
there is no pinned archive hash to compare with. The modules link its `cppzmq-static` target: the two headers
with `libzmq-static`. `CPPZMQ_BUILD_TESTS` is off, since its tests would download Catch2.

## cxxopts 3.3.1
The files are byte-for-byte those of the release archive `https://github.com/jarro2783/cxxopts/archive/v3.3.1.tar.gz`,
whose SHA-512 is
`a22da1436a263d51aad2f542c2099f5b4fd1b02674716ff26d2f575786dcec4e97400edebf5577de95f3ae48c7c99be7be17d7a3de3e01a9f3612667e1547908`
(the same hash vcpkg's `cxxopts` 3.3.1 port pins). As a subproject it builds no tests or examples, adds no warning
flags and installs nothing. The manager links `cxxopts::cxxopts`; only `ProcessManagerApp/source` includes the
header.

## fkYAML 0.4.4
The files are byte-for-byte those of the release archive
`https://github.com/fktn-k/fkYAML/archive/v0.4.4.tar.gz`, whose SHA-512 is
`d1b2be36432df4cd8351f973263b62c33f221f2d7873250821d02f2d1a52cbc4340904c925aa058d2d1afe7122b85b2531581eb1b9366d88ef646e081e961446`
(the same hash vcpkg's `fkyaml` port pins). The root `CMakeLists.txt` sets `FK_YAML_USE_SINGLE_HEADER` and adds
the directory. Only `ConfigSection/src/ConfigSection.cpp` includes the header.

Known limitation of 0.4.4: a value without quotes cannot hold two spaces in a row (`a  b`); quote it.
ConfigSection's error message names the line and this fix.

## GoogleTest 1.18.0
The v1.18.0 release, `https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz`, whose SHA-512 is
`ba0f5769ccf34acf1bc72d1f7e9ffb8202176d02b64f6f3d9047accfc0cf9026ff5a653d24935e2705fff8709566676452616c93ca0ca6277f1e21d79b58a10a`
(the same hash vcpkg's `gtest` 1.18.0 port pins). The files are byte-for-byte those of the archive, and the 24
Python test scripts it marks executable keep that mode in git.

It is added only with `ENABLE_TEST`. `BUILD_GMOCK` and `INSTALL_GTEST` are off, so only `gtest` and `gtest_main`
are built; the suites link `GTest::gtest_main`. It needs C++17 and CMake 3.16.
