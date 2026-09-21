# Hull / Domain shader support

`GraphicsPipelineDesc` enables tessellation when all of the following are set:

- `vertexShader`, `hullShader`, and `domainShader`
- `topology = PrimitiveTopology::PatchList`
- `patchControlPoints` in `1..GetLimit(Limit::TessellationPatchControlPoints)`
- a `TessellationState` matching the shader declarations

Query `Feature::Tessellation` before creating the shaders or pipeline. Resource and inline-
constant visibility can include `ShaderStageFlags::Hull` and `ShaderStageFlags::Domain`.
The shader build helper accepts the `hull` and `domain` stages. Example shader discovery also
recognizes `_hs` and `_ds` file names.

## Backend mapping

- D3D12 maps Hull/Domain to `HS`/`DS`, uses a patch PSO topology, and selects the native
  `N_CONTROL_POINT_PATCHLIST` topology from `patchControlPoints`.
- Vulkan maps them to tessellation-control/tessellation-evaluation stages and supplies
  `VkPipelineTessellationStateCreateInfo` with `patchControlPoints`.
- Metal maps Domain to a `[[patch(..., N)]]` post-tessellation vertex function. Hull is a
  compute kernel invoked once per patch and instance before the patch draw. Vertex buffers and
  Hull-visible resources keep their declared buffer/texture/sampler indices. The kernel writes
  half-precision factors to `[[buffer(30)]]`: four half values for a triangle patch (three edge,
  one inside) or six for a quad patch (four edge, two inside). Binding 30 is reserved for this
  output in the Metal Hull stage. Metal patch draws are currently non-indexed.

Metal temporarily ends and resumes the render encoder around the Hull compute pass. Attachments,
pipeline state, resources, inline constants, viewport, scissor, and stencil reference are restored
before the Domain stage runs.
