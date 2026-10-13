#include <MaterialXCore/Document.h>
#include <MaterialXFormat/XmlIo.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/Util.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenSlang/SlangShaderGenerator.h>
#include <MaterialXGenHw/HwShaderGenerator.h>
#include <fstream>
#include <iostream>
namespace mx = MaterialX;
int main(int argc, char** argv) {
  mx::FilePath libRoot = argv[1];
  mx::FilePath file = argv[2];
  std::string outBase = argv[3];
  mx::FileSearchPath search(libRoot);
  mx::DocumentPtr stdlib = mx::createDocument();
  mx::loadLibraries({"libraries"}, mx::FileSearchPath(libRoot), stdlib);
  mx::DocumentPtr doc = mx::createDocument();
  mx::readFromXmlFile(doc, file);
  doc->setDataLibrary(stdlib);
  std::string msg;
  if (!doc->validate(&msg)) std::cerr << "validate: " << msg << "\n";
  auto elems = mx::findRenderableElements(doc);
  std::cerr << "renderable: " << elems.size() << "\n";
  mx::ShaderGeneratorPtr gen = mx::SlangShaderGenerator::create();
  mx::GenContext ctx(gen);
  ctx.registerSourceCodeSearchPath(mx::FileSearchPath(libRoot));
  ctx.getOptions().targetColorSpaceOverride = "lin_rec709";
  ctx.getOptions().hwMaxActiveLightSources = 1;
  ctx.getOptions().hwDirectionalAlbedoMethod = mx::DIRECTIONAL_ALBEDO_ANALYTIC;
  mx::NodeDefPtr lightDef = stdlib->getNodeDef("ND_directional_light");
  if (!lightDef) { std::cerr << "no ND_directional_light\n"; return 1; }
  mx::HwShaderGenerator::bindLightShader(*lightDef, 1, ctx);
  mx::ShaderPtr sh = gen->generate("mat", elems[0], ctx);
  std::ofstream(outBase + ".pixel.slang") << sh->getSourceCode(mx::Stage::PIXEL);
  std::ofstream(outBase + ".vertex.slang") << sh->getSourceCode(mx::Stage::VERTEX);
  {
    std::ofstream vals(outBase + ".values.txt");
    const mx::ShaderStage& ps = sh->getStage(mx::Stage::PIXEL);
    for (auto& [bname, block] : ps.getUniformBlocks()) {
      for (size_t i = 0; i < block->size(); ++i) {
        const mx::ShaderPort* port = (*block)[i];
        vals << bname << " " << port->getVariable() << " " << port->getType().getName() << " "
             << (port->getValue() ? port->getValue()->getValueString() : std::string("-")) << "\n";
      }
    }
  }
  std::cerr << "ok\n";
}
