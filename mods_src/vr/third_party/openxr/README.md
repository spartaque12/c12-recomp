OpenXR headers from [KhronosGroup/OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK) release-1.1.65, unmodified (SPDX: Apache-2.0 OR MIT; see LICENSE-Apache-2.0.txt / LICENSE-MIT.txt).

`bin/openxr_loader.dll`: the OpenXR loader 1.1.65.0 built from the same OpenXR-SDK release (copied from the VCS-Recomp VR build, `VCS-mods/build/plugins/VR/openxr_loader.dll`). The loader sources (`src/loader/`) are `Apache-2.0 OR MIT`; it bundles jsoncpp (MIT). Shipped as `mods/openxr_loader.dll` by `mods_src/vr/build_vr.ps1`; the VR layer loads it at run time from the mod's folder.
