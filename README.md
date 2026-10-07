# NvrGpuRating

A plugin that tells Echo VR what GPU it runs on where Proton can't, so its first start picks the
graphics preset Windows would. Without it every Linux PC started on **Low**.

## Why

On its first start (no settings file yet) Echo rates the GPU and saves a preset:

| GPU | High | Medium | otherwise |
|---|---|---|---|
| NVIDIA: cores x base clock x 2 (NVAPI) | >= 8.0 | >= 4.5 | Low |
| AMD: TFLOPS (AGS) | >= 10.0 | >= 6.0 | Low |

Under Proton, dxvk-nvapi answers the base clock with "not supported", and the game's own AGS
can't reach the AMD driver, so the rating failed and Low was saved for good.

## What it does

It hooks `nvapi64!nvapi_QueryInterface` (the game asks it for `GetGpuCoreCount` and
`GetAllClockFrequencies` by id) and `amd_ags_x64!agsInit`. Where the real answer is 0 or
unsupported it fills in the GPU's numbers, measured without a list of GPUs, so newer generations
work as they come:

- **NVIDIA:** SMs from Vulkan (`VK_NV_shader_sm_builtins`) x cores per SM (from the CUDA
  generation NVML reports; 128 for every generation since Ampere); the base clock from NVML's
  maximum clock (NVML has none), scaled by 0.62.
- **AMD:** compute units from Vulkan (`VK_AMD_shader_core_properties[2]`) x 64 lanes x 2 x the top
  engine clock (the Linux host's `pp_dpm_sclk`, through Wine's `Z:` drive).

Where the real driver answers (Windows), nothing changes. It logs to
`plugin_logs/NvrGpuRating/NvrGpuRating.log` beside the game's exe.

The Echo VR launcher installs it: a required mod of the live build (nEVR loads it), and an
EchoLoader plugin of every event build. On Linux it also re-rates a saved Low preset once.

Release build: `https://release.echovr.de/launcher/mods/NvrGpuRating-1.0.0.dll`
(sha256 `78c047ea68c363550d8c2c7b802fcd4e025fc9b092e086bf37a733e3d91f1d71`).

## Building

```
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake
cmake --build build
```

It implements the nEVR plugin ABI v5 (`nevr_plugin_interface.h`) and pins MinHook 1.3.4.
