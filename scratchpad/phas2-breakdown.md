# Phase 2 Breakdown

this is based on on the  /home/felix/git/vulkan-again/.claude/plans/phase-2-plan-resilient-toast.md architecture proposition, but i will implement some adjustement that i think will improve the architecture and propose implementation steps.

Your task will be to describe the new architeture and implementaiton steps in a markdown documents in scratchpad/ 

# Step 1 - Scene
This first step doesn't touch materialX. The goal is to prepare the scene loading and render loop to abandon the single texture buffer and instancing in flavor of a declared scene composed of DrawablePrims.

A side job will be to remove the selected attribute from the push constants because we are getting ready to loose the fine grain controle for fragment shader once.

For now the DrawablePrim will use a ktx texture handle instead of material handle, (code that will be reaplced in future step)

down the line the render loop will look something like:

for each DrawablePrim:
    auto& mat  = materials[prim.materialId];
    auto& mesh = meshes[prim.meshId];
    VkPipeline pipeline = pipelineCache.getOrCreate(mat.shader, mesh.vertexLayout, pass.format);
    bind pipeline
    bind set 1  (mat.descriptorSet)
    push BDA    (per-draw transform)
    vkCmdDrawIndexed(...)

But for now we will not create a pipeline chache, an we will run on the assumption that the same pipeline is usable for all objects of the scene (the textures we are using are known in advance). That means that we are keeping the single pipeline layout creation before the render loop and in the render loop we are binding the descriptor set for each drawable prim 


## step 2 : Data driven pipeline creation and cache
After step one we are now reusing the same pipeline 3 times by binding different descripor set. There for extrcting the pipeline creation inside the getorcreate from the pipeline cache will be pretty trival.
After we do scene loading for mesh and textures we can now getorcreate the pipeline for each drawableprim, if we successfully handled the cahcing we should have only one pipeline in the map.

The pipeline cache is going to follow the pattern of functions inside of a namespace
namespace PipelineCache {
      static std::unordered_map<Key, VkPipeline>       pipelines_;
      static std::unordered_map<...>, VkPipelineLayout> layouts_;

  VkPipeline getOrCreate(VkDevice device, VkPipelineCache vkCache,
                         VkDescriptorSetLayout set0, const Shader&,
                         VkDescriptorSetLayout set1, const VertexLayout&,
                         VkFormat color, VkFormat depth);
  }

I am wondering if the getorcreate interface is the best one forergonomics because that then involves calling getorcreate again in the renderloop to retreive and bind the pipeline, alternativly we could call get or create in the render loop which create the pipeline at the moment it is needed. the third option is having a seperate create and get, but then we still need to have a create path everytime we call get to make sure we handle the non existing entry gracefully on the get

## step 3 - material X
Note: this step will test the quality of our abstraction at this point and we should insert more steps before it if you beleive more refactoring would be needed to make the material X integration clean

This step has the reponsiability of replacing the ktx texture with material X and therefore defining the interface responisble for consuming the materialX document

Again we will use the namespace architecure that will expose compile-> materialHandle and compileFromFile -> material Handle
 the namespace will be called Material and will have it's own .h and .cpp file

  ├── compile(MaterialX::DocumentPtr) → MaterialHandle
  ├── compileFromFile(path)           → MaterialHandle  (convenience)
  └── CompiledMaterial
        ├── Shader                  (SPIR-V blob, via existing shader.h)
        ├── VkDescriptorSetLayout   (set 1, built from Slang reflection)
        ├── VkDescriptorSet         (set 1, textures + scalars written once at compile time)
        └── VkPipeline              (from PipelineCache — one per unique SPIR-V)

The compiled material will be stored in the DrawblePrim
