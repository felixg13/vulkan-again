#!/usr/bin/env bash
# Step 0 spike: MaterialX 1.39.5 -> Slang -> SPIR-V -> reflection -> headless render.
# Throwaway. Everything is built under ./.work (delete it to start over).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
W="$HERE/.work"; mkdir -p "$W/out"
ASSETS="$HERE/../../assets"

# 1. MaterialX 1.39.5 with the Slang generator
if [ ! -d "$W/mx-install" ]; then
  git clone --depth 1 --branch v1.39.5 https://github.com/AcademySoftwareFoundation/MaterialX.git "$W/materialx"
  cmake -S "$W/materialx" -B "$W/mx-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$W/mx-install" -DMATERIALX_BUILD_TESTS=OFF -DMATERIALX_BUILD_VIEWER=OFF \
    -DMATERIALX_BUILD_GRAPH_EDITOR=OFF -DMATERIALX_BUILD_PYTHON=OFF -DMATERIALX_BUILD_RENDER=OFF \
    -DMATERIALX_BUILD_GEN_MDL=OFF -DMATERIALX_BUILD_GEN_MSL=OFF -DMATERIALX_BUILD_GEN_OSL=OFF \
    -DMATERIALX_BUILD_GEN_SLANG=ON -DMATERIALX_BUILD_DOCS=OFF
  cmake --build "$W/mx-build" -j"$(nproc)"
  cmake --install "$W/mx-build"
fi

# 2. Slang v2026.14 (the version external/CMakeLists.txt pins; the Vulkan SDK's slangc segfaults on this input)
if [ ! -x "$W/slang/bin/slangc" ]; then
  mkdir -p "$W/slang"
  curl -sL -o "$W/slang.tar.gz" https://github.com/shader-slang/slang/releases/download/v2026.14/slang-2026.14-linux-x86_64.tar.gz
  echo "9445cf0b35332bfda055c9424e307f957ff58befb04c5b45b9f9f29894a41cf8  $W/slang.tar.gz" | sha256sum -c
  tar xzf "$W/slang.tar.gz" -C "$W/slang"
fi
SLANGC="$W/slang/bin/slangc"

# 3. generator + harness
MX_PREFIX="$W/mx-install" cmake -S "$HERE" -B "$W/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$W/build"
TINYOBJ="${TINYOBJ_DIR:-$HERE/../../build/debug/_deps/tinyobjloader-src}"
g++ -std=c++20 -O1 -DPROJECT_ASSETS="\"$(realpath "$ASSETS")\"" -I"$TINYOBJ" \
  -I"${VULKAN_SDK:-/opt/vulkan-sdk/x86_64}/include" "$HERE/harness.cpp" -o "$W/harness" -lvulkan

# 4. engine vertex module
"$SLANGC" -target spirv -entry vertex_main -stage vertex -o "$W/out/engine_vertex.spv" "$HERE/engine_vertex.slang"
spirv-val --target-env vulkan1.3 "$W/out/engine_vertex.spv"

# 5. per material: generate -> compose with adapter -> compile -> validate -> reflect -> render
for m in flat textured; do
  "$W/build/gen" "$W/mx-install" "$HERE/mtlx/$m.mtlx" "$W/out/$m"
  adapter="$HERE/adapter_$m.slang"
  { echo '#define cbuffer [[vk::binding(0,1)]] cbuffer'; cat "$W/out/$m.pixel.slang" "$adapter"; } > "$W/out/$m.full.slang"
  "$SLANGC" -target spirv -entry pixel_main -stage fragment -reflection-json "$W/out/$m.json" \
    -o "$W/out/$m.frag.spv" "$W/out/$m.full.slang" 2>&1 | grep -E "^error" || true
  spirv-val --target-env vulkan1.3 "$W/out/$m.frag.spv"
  python3 -I - "$W/out/$m.json" "$W/out/$m.offsets.txt" <<'PY'
import json, sys
r = json.load(open(sys.argv[1])); out = []
et = r['parameters'][0]['type']['elementType']
def flat(prefix, fields, base):
    for f in fields:
        t = f['type']; b = f.get('binding', {})
        if b.get('kind') != 'uniform': continue
        off = base + b['offset']
        if t['kind'] == 'struct': flat(prefix + f['name'] + '.', t['fields'], off)
        elif t['kind'] == 'array':
            e = t['elementType']; st = b.get('elementStride', 0)
            for i in range(t['elementCount']):
                if e['kind'] == 'struct': flat(prefix + f'{f["name"]}[{i}].', e['fields'], off + i * st)
        else: out.append((prefix + f['name'], off, b['size']))
flat('', et['fields'], 0)
open(sys.argv[2], 'w').write('\n'.join(f'{n} {o} {s}' for n, o, s in out) + '\n')
b = r['parameters'][0]['binding']
print(f"  material block: space {b['space']}, binding {b['index']}, {b['count']} descriptor slots")
PY
  "$W/harness" "$W/out/engine_vertex.spv" "$W/out/$m.frag.spv" "$W/out/$m.offsets.txt" "$W/out/$m.values.txt" "$W/out/$m.ppm"
  python3 -I -c "from PIL import Image; Image.open('$W/out/$m.ppm').save('$W/out/$m.png')"
  echo "==> $W/out/$m.png"
done
