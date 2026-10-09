# FSR 4 Ray Regeneration (RR) Integration Guide

This guide details the architectural requirements, descriptor structures, and shader pipeline modifications needed to integrate **AMD FidelityFX Ray Regeneration (FSR 4 RR / Denoiser)** into Community Shaders.

---

## 1. Overview & Architecture

FSR Ray Regeneration is AMD's machine-learning spatio-temporal denoiser (provided via `amd_fidelityfx_denoiser_dx12.dll` and the FFX-API modular runtime).
Unlike monolithic upscalers, Ray Regeneration acts on decoupled lighting radiance signals and G-buffer surfaces:

* **Effect ID**: `FFX_API_EFFECT_ID_DENOISER = 0x00050000`
* **Create Context Desc**: `FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER = 0x00050082`
* **Primary Dispatch Desc**: `FFX_API_DISPATCH_DESC_TYPE_DENOISER = 0x00050081`
* **Signals**: Multi-signal architecture chained via `pNext`. You can enable specific signals via `signalFlags`.
* **Execution**: Dispatched via D3D12 on textures imported through the Vulkan-D3D12 interop layer (`FSRRRDenoiser` / `FSR4Backend`).

---

## 2. Context Creation & ABI

### 2.1 Descriptors

```cpp
#define FFX_API_EFFECT_ID_DENOISER 0x00050000u
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER 0x00050082u
#define FFX_API_DISPATCH_DESC_TYPE_DENOISER 0x00050081u

// Signal flag bitfield
enum FfxApiDenoiserSignalFlags : uint32_t
{
    FFX_DENOISER_SIGNAL_NONE                         = 0,
    FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE               = (1 << 0),
    FFX_DENOISER_SIGNAL_DIRECT_SPECULAR              = (1 << 1),
    FFX_DENOISER_SIGNAL_INDIRECT_DIFFUSE             = (1 << 2),
    FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR            = (1 << 3),
    FFX_DENOISER_SIGNAL_DOMINANT_LIGHT_VISIBILITY    = (1 << 4),
    FFX_DENOISER_SIGNAL_AMBIENT_OCCLUSION            = (1 << 5),
    FFX_DENOISER_SIGNAL_SPECULAR_OCCLUSION           = (1 << 6),
};

enum FfxApiCreateContextDenoiserFlags : uint32_t
{
    FFX_DENOISER_ENABLE_DEBUGGING = (1 << 0),
    FFX_DENOISER_ENABLE_VALIDATION = (1 << 1),
};

enum FfxApiDispatchDenoiserFlags : uint32_t
{
    FFX_DENOISER_DISPATCH_RESET            = (1 << 0),
    FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO = (1 << 1),
};

struct ffxCreateContextDescDenoiser
{
    ffxCreateContextDescHeader header; // type = 0x00050082
    uint32_t flags;                   // FfxApiCreateContextDenoiserFlags
    uint32_t signalFlags;             // Combination of FfxApiDenoiserSignalFlags
    uint32_t checkerboardSignalFlags;   // Subset of signalFlags (0 if full rate)
    FfxApiDimensions2D maxRenderSize;
};
```

> [!NOTE]
> Ambient Occlusion and Specular Occlusion are **optional**. If you only enable `DIRECT_DIFFUSE | DIRECT_SPECULAR | INDIRECT_DIFFUSE | INDIRECT_SPECULAR`, only those 4 descriptors are dispatched. Running *only* AO or SO without radiance is rejected by the DLL.

---

## 3. Dispatch Descriptors & G-Buffer Requirements

### 3.1 Primary G-Buffer Descriptor (`0x00050081`)

Every dispatch requires the primary G-buffer descriptor:

```cpp
struct FfxApiFloatBounds
{
    float min;
    float max;
};

struct FfxApiFloatCoords3D
{
    float x, y, z;
};

struct FfxApiMatrix4x4
{
    float rows[4][4];
};

struct ffxDispatchDescDenoiser
{
    ffxDispatchDescHeader header;             // type = 0x00050081
    ID3D12GraphicsCommandList* commandList;   // D3D12 command list
    FfxApiResource linearDepth;               // R32_FLOAT, view space Z (negative)
    FfxApiResource motionVectors;             // RGBA16_FLOAT / RG16_FLOAT
    FfxApiResource normals;                   // RGBA16_FLOAT (Octahedral XY + roughness)
    FfxApiResource diffuseAlbedo;             // RGB10A2 / RGBA16F (encoded as sqrt(albedo))
    FfxApiResource specularAlbedo;            // RGB10A2 / RGBA16F (encoded as sqrt(specular))
    FfxApiFloatCoords3D motionVectorScale;    // { scaleX, scaleY, scaleZ (+-1.0) }
    FfxApiFloatCoords2D jitterOffsets;        // Camera subpixel jitter in [-1, 1]
    FfxApiFloatCoords3D cameraPositionDelta;  // Frame-to-frame camera movement
    FfxApiMatrix4x4 view;                     // Current View matrix
    FfxApiMatrix4x4 projection;               // Current Projection matrix
    FfxApiFloatBounds linearDepthBounds;      // { minDepth, maxDepth }
    FfxApiDimensions2D renderSize;            // Current render dimensions
    uint32_t flags;                           // FfxApiDispatchDenoiserFlags
};
```

### 3.2 Signal Sub-Descriptors (Chained via `pNext`)

Each active signal has a dedicated descriptor linked to `ffxDispatchDescDenoiser.header.pNext`:

```cpp
struct FfxApiDenoiserSignal
{
    FfxApiResource input;             // Noisy radiance input (UAV or SRV)
    FfxApiResource output;            // Denoised UAV output
    uint32_t checkerboardOrigin;      // 0 if non-checkerboard
};

struct ffxDispatchDescDenoiserDirectDiffuse
{
    ffxDispatchDescHeader header;
    FfxApiDenoiserSignal signal;
};

struct ffxDispatchDescDenoiserDirectSpecular
{
    ffxDispatchDescHeader header;
    FfxApiDenoiserSignal signal;
};

struct ffxDispatchDescDenoiserIndirectDiffuse
{
    ffxDispatchDescHeader header;
    FfxApiDenoiserSignal signal;      // Alpha must contain hitDistance
};

struct ffxDispatchDescDenoiserIndirectSpecular
{
    ffxDispatchDescHeader header;
    FfxApiDenoiserSignal signal;      // Alpha must contain hitDistance
};
```

---

## 4. Input Buffer Specifications & Shader Encoding

### 4.1 Linear Depth
* **Format**: `DXGI_FORMAT_R32_FLOAT` / `VK_FORMAT_R32_SFLOAT`.
* **Semantics**: **Signed View-Space Z** (`viewSpacePos.z`), where values are negative along the camera view direction.
* In HLSL prepass:
  ```hlsl
  float depth = g_DepthTarget[pixel];
  float3 screenUVW = float3((float2(pixel) + 0.5f) / float2(renderWidth, renderHeight), depth);
  float3 viewSpacePos = ScreenSpaceToViewSpace(screenUVW, g_ClipToCamera);
  g_LinearDepth[pixel] = viewSpacePos.z;
  ```

### 4.2 Albedo Encoding ($\sqrt{\text{Albedo}}$)
* **Format**: `DXGI_FORMAT_R16G16B16A16_FLOAT` or `DXGI_FORMAT_R10G10B10A2_UNORM`.
* **Important**: Both `diffuseAlbedo` and `specularAlbedo` MUST be stored in square-root space:
  ```hlsl
  g_DiffuseAlbedoTarget[pixel]  = sqrt(diffuseAlbedo);
  g_SpecularAlbedoTarget[pixel] = sqrt(specularAlbedo);
  ```
  *(Or alternatively set `FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO` in dispatch flags if passing raw linear albedo).*

### 4.3 Normals & Roughness
* **Format**: `DXGI_FORMAT_R16G16B16A16_FLOAT`.
* **Channels**:
  * `xy`: Octahedron-encoded normal (`NormalToOctahedronUv(worldNormal)`).
  * `z`: Perceptual surface roughness ($\alpha$).
  * `w`: $0.0\text{f}$.

### 4.4 Indirect Radiance & Hit Distance
* **Format**: `DXGI_FORMAT_R16G16B16A16_FLOAT`.
* **Channels**:
  * `rgb`: Un-modulated radiance (lighting contribution divided by albedo).
  * `a`: Hit distance along the ray (`length(origin - hitPosition)`).
  * If ray misses geometry, saturate `a` to `65504.0f`.

---

## 5. Composition Phase

After the Denoiser finishes, a composition compute pass combines the denoised signals back with surface albedo:

```hlsl
float3 diffuseAlbedo  = Square(g_DiffuseAlbedo[pixel]);
float3 specularAlbedo = Square(g_SpecularAlbedo[pixel]);

float3 directDiffuse    = g_DenoisedDirectDiffuse[pixel].xyz;
float3 indirectDiffuse  = g_DenoisedIndirectDiffuse[pixel].xyz;
float3 directSpecular   = g_DenoisedDirectSpecular[pixel].xyz;
float3 indirectSpecular = g_DenoisedIndirectSpecular[pixel].xyz;
float3 emissive         = g_Emissive[pixel].xyz;

float3 composite = (directDiffuse + indirectDiffuse) * diffuseAlbedo
                 + (directSpecular + indirectSpecular) * specularAlbedo
                 + emissive;

g_Output[pixel] = float4(composite, 1.0f);
```

---

## 6. Implementation Steps in Community Shaders

### 6.1 Settings & UI (`Upscaling.cpp` / `PathTracing.cpp`)
1. In `Upscaling.h`:
   * Add denoiser enum option: `enum class DenoiserType { None = 0, DLSSD = 1, FSR_RR = 2 }`.
   * Add setting `uint32_t denoiserType = 0;`.
2. In ImGui settings menu:
   * Provide a dropdown in Ray Tracing / Path Tracing settings for Denoiser: `[ Off | DLSS-RR | FSR Ray Regeneration ]`.
   * Grey out or warn if running on an unsupported GPU/driver.

### 6.2 Prepass & G-Buffer Alignment
1. Modify `PathTracing` lighting passes to write the 4 decoupled radiance targets:
   * `DirectDiffuse` (`RGBA16F`)
   * `DirectSpecular` (`RGBA16F`)
   * `IndirectDiffuse` (`RGBA16F`, hit distance in `A`)
   * `IndirectSpecular` (`RGBA16F`, hit distance in `A`)
2. Ensure `normalRoughness` format matches `DXGI_FORMAT_R16G16B16A16_FLOAT` on both Vulkan and D3D12 interop.
3. Apply `sqrt(albedo)` on `diffuseAlbedo` and `specularAlbedo` when writing to the interop textures.

### 6.3 Interop Barriers & Timeline Synchronization
Follow the exact barrier pattern established in `FSR4Backend.cpp`:
1. **Before FFX Dispatch**:
   * Transition G-buffer textures from `D3D12_RESOURCE_STATE_COMMON` to `D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE`.
   * Transition output signal textures from `D3D12_RESOURCE_STATE_COMMON` to `D3D12_RESOURCE_STATE_UNORDERED_ACCESS`.
2. **Execute `ffxDispatch`**.
3. **After FFX Dispatch**:
   * Transition output signal textures back from `D3D12_RESOURCE_STATE_UNORDERED_ACCESS` to `D3D12_RESOURCE_STATE_COMMON`.
   * Transition input textures back to `D3D12_RESOURCE_STATE_COMMON`.
4. **Signal Timeline Fence**: Ensure GPU coherency before Vulkan composition reads the denoised textures.

### 6.4 Verification
* Check `CommunityShaders.log` for provider identification:
  ```text
  [FSR RR] Created FidelityFX Denoiser context successfully
  [FSR RR] Active provider: 'FSR Ray Regeneration - 1.2.0'
  ```
* Test composition output to verify absence of black screen, NaN blowups, or ghosting.
