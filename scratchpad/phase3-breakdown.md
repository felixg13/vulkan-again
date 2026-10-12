# Phase 3 Breakdown
At the end of phase 2 we should have a renderer capable of rendering mtlx materials on top of obj files defined programaticlly in a simple scene object. 
The renderer is split in 2 parts the presenter and the renderer. Now in phase 3 we put a parrel path next to presenter that registers it's classes to be a hydra render delagate.
It will defer must renderering task to the renderer and handle the bridge with hydra.
at all times contracts between the renderer and the legacy presenter can not be broken, it is our standalone testing device.

## Hydra
We are targeting to become a render delegate for hydra. That only targets usdviewer to keep it simple with not too much authoring happening.

## Steps

### Steps 1 - validation
In the validation we are listing all the overrides needed to be implemented in the delegate to render a simple scene, at that point we should generate a list of 5 simple scenes that should be handled by our renderer each presenting different cases of USD.
We are validating that against currently existing API of the renderer therefore ensuring that the phase 2 prepared the renderer approprietly to become hydra delegate. If we find gaps we will go back to phase 2 and finish setting up the needed APIs 
The goal is for Phase 3 to be almost trival and basiacally boilerplate registration of fucntionnalities

### Step 2 - Top only
In this step we will not be sending anything to the renderer. we are simply creating the skeleton of every function override that needs to be implemented to make the render delegate visible by usdviewer as a selectable render delegate.

The functions body should go as far as possible in the handling of the usd data before submission to renderer with the goal of logging every consumed data in Logs, that will provide the agent with enough context to validate if we are implementing the contracts gracefully.

An other part of the validation will be using simple usdiamging test scenes before trying to plug into usdviewer that provides a faster itteration loop on validating of data is consumed gracefully.

### Step 3 - submission to render
This part will have many sub steps were we first list all of the submissions that will have to be trasnfered to the render and once again in the renderer we log them to validate that the data is transffered gracefully. 
Submissions in order. Each one is logged on the renderer side and checked before the next
begins. They map onto the incremental renderer API from `phase2-architecture.md`.

1. **Camera and frame data** — view, projection, one directional light → `PerFrameData`.
   First, because nothing else logs sensibly without a valid view.
2. **Meshes** — `HdMesh::Sync` → `add_mesh` (points, indices, normals, st; triangulated,
   defaults filled for missing primvars).
3. **Materials** — `HdMaterial::Sync` → `hdMtlx` → `MaterialX::DocumentPtr` →
   `compile_material`. Meshes come first so a material can be checked visually on real
   geometry.
4. **Prims** — `add_prim(mesh, material, transform)` binding the above together.
5. **Transforms and visibility** — `set_transform`, `remove_prim`.
6. **Light** — update the directional light from the delegate's light data.
7. **Dirty-bit and re-sync handling** — material replacement issues a new handle and
   releases the old one; prim deletion; mesh re-sync. Last, because it matters only once
   the first pass works.

Output path for this step: the delegate renders to the renderer's offscreen target and
fills the `HdRenderBuffer` from `read_back()`.

In this section usdimaging is again the source of our validation loop again.

### Step 4 - complete scene loading and presentation
At this point when we submit a stage trhough usd imaging the data reachers the renderer and is computed, saddly we probably don't have a view on it. This is what usdview provides, we areleady validate that we can select the render delegate so at that point we are opening a scene in usdview and we witness the stage to be renderer and camera move function.

### Wrapup
A sucessful execution of these steps mean that we have a functionning render delegate for a hand picked selections of scene that are all viewed trhough usdviewer. The limitations to what scenes are not compatible are noted and acknowledge as out of scope

The standalone version is still functional and will be usd in step 4 it is also an important validation of the phase 3. Do not break contracts and if standalone contracts are broken we will fix them here

