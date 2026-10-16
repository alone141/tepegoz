# Vendored dependencies

Built in-tree, so an airgapped machine needs nothing installed for them.

| Package | Version | License | Files kept | Source |
|---|---|---|---|---|
| fkYAML | 0.4.4 | MIT (`fkYAML/LICENSE.txt`) | `CMakeLists.txt`, `cmake/*.in`, `single_include/fkYAML/node.hpp`, `LICENSE.txt` | https://github.com/fktn-k/fkYAML, tag `v0.4.4` |

The files are byte-for-byte those of the release archive
`https://github.com/fktn-k/fkYAML/archive/v0.4.4.tar.gz`, whose SHA-512 is
`d1b2be36432df4cd8351f973263b62c33f221f2d7873250821d02f2d1a52cbc4340904c925aa058d2d1afe7122b85b2531581eb1b9366d88ef646e081e961446`
(the same hash vcpkg's `fkyaml` port pins). The root `CMakeLists.txt` sets `FK_YAML_USE_SINGLE_HEADER`
and adds the directory as `SYSTEM`. Only `ConfigSection/src/ConfigSection.cpp` includes the header.

Known limitation of 0.4.4: a value without quotes cannot hold two spaces in a row (`a  b`); quote it.
ConfigSection's error message names the line and this fix.
