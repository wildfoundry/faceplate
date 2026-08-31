# Branding policy

Faceplate is neutral open-source infrastructure. The core repository, binary,
source archive, packages, tests, and default assets do not contain the
Dataplicity logo.

Branding is loaded from runtime assets. It is not embedded into source code,
converted into C data, or linked into the executable.

## Package separation

- `faceplate`: the core binary and neutral default theme.
- `faceplate-theme-example`: redistributable sample assets for integrators.
- `faceplate-theme-dataplicity`: the separately licensed Dataplicity theme.

The Dataplicity theme belongs in a separate repository and should initially be
private. A Dataplicity OS or Yocto image selects that package while assembling
the image. OEM images can provide another theme package without rebuilding the
Faceplate binary.

## Dataplicity marks

The Faceplate software licence does not grant permission to use the Dataplicity
name, logo, or other marks. If Dataplicity theme assets are later published,
their repository must carry explicit asset and trademark terms separate from
the Faceplate software licence.

Do not add Dataplicity artwork to this repository.
