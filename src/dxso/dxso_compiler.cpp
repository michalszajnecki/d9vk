#include "dxso_compiler.h"

namespace dxvk {

  DxsoCompiler::DxsoCompiler(
    const std::string&     fileName,
    const DxsoModuleInfo&  moduleInfo,
    const DxsoProgramInfo& programInfo)
    : m_moduleInfo{ moduleInfo }
    , m_programInfo{ programInfo } {
    // Declare an entry point ID. We'll need it during the
    // initialization phase where the execution mode is set.
    m_entryPointId = m_module.allocateId();

    // Set the shader name so that we recognize it in renderdoc
    m_module.setDebugSource(
      spv::SourceLanguageUnknown, 0,
      m_module.addDebugString(fileName.c_str()),
      nullptr);

    // Set the memory model. This is the same for all shaders.
    m_module.setMemoryModel(
      spv::AddressingModelLogical,
      spv::MemoryModelGLSL450);


    // Make sure our interface registers are clear
    for (uint32_t i = 0; i < 16; i++) {
      m_vDecls.at(i) = DxsoDeclaration{ };
      m_oDecls.at(i) = DxsoDeclaration{ };
    }

    this->emitInit();
  }

  void DxsoCompiler::processInstruction(
    const DxsoInstructionContext& ctx) {
    const DxsoOpcode opcode = ctx.instruction.opcode();

    switch (opcode) {
    case DxsoOpcode::Dcl:
      return this->emitDcl(ctx);

    case DxsoOpcode::Add:
    case DxsoOpcode::Mov:
      return this->emitVectorAlu(ctx);

    default:
      Logger::warn(str::format("DxsoCompiler::processInstruction: unhandled opcode: {0}", opcode));
      break;
    }
  }

  Rc<DxvkShader> DxsoCompiler::finalize() {
    if (m_programInfo.type() == DxsoProgramType::VertexShader)
      this->emitVsFinalize();
    else
      this->emitPsFinalize();

    // Declare the entry point, we now have all the
    // information we need, including the interfaces
    m_module.addEntryPoint(m_entryPointId,
      m_programInfo.executionModel(), "main",
      m_entryPointInterfaces.size(),
      m_entryPointInterfaces.data());
    m_module.setDebugName(m_entryPointId, "main");

    DxvkShaderOptions shaderOptions = { };

    DxvkShaderConstData constData = { };

    // Create the shader module object
    return new DxvkShader(
      m_programInfo.shaderStage(),
      m_resourceSlots.size(),
      m_resourceSlots.data(),
      m_interfaceSlots,
      m_module.compile(),
      shaderOptions,
      std::move(constData));
  }

  void DxsoCompiler::emitVsFinalize() {
    this->emitMainFunctionBegin();
    m_module.opFunctionCall(
      m_module.defVoidType(),
      m_vs.functionId, 0, nullptr);
    this->emitFunctionEnd();
  }

  void DxsoCompiler::emitPsFinalize() {
    this->emitMainFunctionBegin();

    m_module.opFunctionCall(
      m_module.defVoidType(),
      m_ps.functionId, 0, nullptr);

    /*if (m_ps.killState != 0) {
      DxbcConditional cond;
      cond.labelIf = m_module.allocateId();
      cond.labelEnd = m_module.allocateId();

      uint32_t killTest = m_module.opLoad(m_module.defBoolType(), m_ps.killState);

      m_module.opSelectionMerge(cond.labelEnd, spv::SelectionControlMaskNone);
      m_module.opBranchConditional(killTest, cond.labelIf, cond.labelEnd);

      m_module.opLabel(cond.labelIf);
      m_module.opKill();

      m_module.opLabel(cond.labelEnd);
    }*/

    //this->emitOutputMapping();
    this->emitOutputDepthClamp();
    this->emitFunctionEnd();
  }

  void DxsoCompiler::emitOutputDepthClamp() {
    // HACK: Some drivers do not clamp FragDepth to [minDepth..maxDepth]
    // before writing to the depth attachment, but we do not have acccess
    // to those. Clamp to [0..1] instead.
    /*if (m_ps.builtinDepth) {
      DxbcRegisterPointer ptr;
      ptr.type = { DxbcScalarType::Float32, 1 };
      ptr.id = m_ps.builtinDepth;

      DxbcRegisterValue value = emitValueLoad(ptr);

      value.id = m_module.opFClamp(
        getVectorTypeId(ptr.type),
        value.id,
        m_module.constf32(0.0f),
        m_module.constf32(1.0f));

      emitValueStore(ptr, value,
        DxbcRegMask::firstN(1));
    }*/
  }

  void DxsoCompiler::emitInit() {
    // Set up common capabilities for all shaders
    m_module.enableCapability(spv::CapabilityShader);
    m_module.enableCapability(spv::CapabilityImageQuery);

    if (m_programInfo.type() == DxsoProgramType::VertexShader)
      this->emitVsInit();
    else
      this->emitPsInit();
  }

  void DxsoCompiler::emitVsInit() {
    m_module.enableCapability(spv::CapabilityDrawParameters);

    m_module.enableExtension("SPV_KHR_shader_draw_parameters");

    // Declare the per-vertex output block. This is where
    // the vertex shader will write the vertex position.
    const uint32_t perVertexStruct = this->getPerVertexBlockId();
    const uint32_t perVertexPointer = m_module.defPointerType(
      perVertexStruct, spv::StorageClassOutput);

    m_perVertexOut = m_module.newVar(
      perVertexPointer, spv::StorageClassOutput);
    m_entryPointInterfaces.push_back(m_perVertexOut);
    m_module.setDebugName(m_perVertexOut, "vs_vertex_out");

    // Main function of the vertex shader
    m_vs.functionId = m_module.allocateId();
    m_module.setDebugName(m_vs.functionId, "vs_main");

    this->emitFunctionBegin(
      m_vs.functionId,
      m_module.defVoidType(),
      m_module.defFunctionType(
        m_module.defVoidType(), 0, nullptr));
    this->emitFunctionLabel();
  }

  void DxsoCompiler::emitPsInit() {
    m_module.enableCapability(spv::CapabilityDerivativeControl);

    m_module.setExecutionMode(m_entryPointId,
      spv::ExecutionModeOriginUpperLeft);

    // Main function of the pixel shader
    m_ps.functionId = m_module.allocateId();
    m_module.setDebugName(m_ps.functionId, "ps_main");

    this->emitFunctionBegin(
      m_ps.functionId,
      m_module.defVoidType(),
      m_module.defFunctionType(
        m_module.defVoidType(), 0, nullptr));
    this->emitFunctionLabel();

    // We may have to defer kill operations to the end of
    // the shader in order to keep derivatives correct.
    /*if (m_analysis->usesKill && m_analysis->usesDerivatives) {
      m_ps.killState = m_module.newVarInit(
        m_module.defPointerType(m_module.defBoolType(), spv::StorageClassPrivate),
        spv::StorageClassPrivate, m_module.constBool(false));

      m_module.setDebugName(m_ps.killState, "ps_kill");

      if (m_moduleInfo.options.useSubgroupOpsForEarlyDiscard) {
        m_module.enableCapability(spv::CapabilityGroupNonUniform);
        m_module.enableCapability(spv::CapabilityGroupNonUniformBallot);

        DxbcRegisterInfo invocationMask;
        invocationMask.type = { DxbcScalarType::Uint32, 4, 0 };
        invocationMask.sclass = spv::StorageClassFunction;

        m_ps.invocationMask = emitNewVariable(invocationMask);
        m_module.setDebugName(m_ps.invocationMask, "fInvocationMask");

        m_module.opStore(m_ps.invocationMask,
          m_module.opGroupNonUniformBallot(
            getVectorTypeId({ DxbcScalarType::Uint32, 4 }),
            m_module.constu32(spv::ScopeSubgroup),
            m_module.constBool(true)));
      }
    }*/
  }

  void DxsoCompiler::emitFunctionBegin(
    uint32_t                entryPoint,
    uint32_t                returnType,
    uint32_t                funcType) {
    this->emitFunctionEnd();

    m_module.functionBegin(
      returnType, entryPoint, funcType,
      spv::FunctionControlMaskNone);

    m_insideFunction = true;
  }

  void DxsoCompiler::emitFunctionEnd() {
    if (m_insideFunction) {
      m_module.opReturn();
      m_module.functionEnd();
    }
    
    m_insideFunction = false;
  }

  void DxsoCompiler::emitFunctionLabel() {
    m_module.opLabel(m_module.allocateId());
  }

  void DxsoCompiler::emitMainFunctionBegin() {
    this->emitFunctionBegin(
      m_entryPointId,
      m_module.defVoidType(),
      m_module.defFunctionType(
        m_module.defVoidType(), 0, nullptr));
    this->emitFunctionLabel();
  }

  uint32_t DxsoCompiler::getPerVertexBlockId() {
    uint32_t floatVectorTypeId = m_module.defVectorType(
      m_module.defFloatType(32),  
      4);

    std::array<uint32_t, 1> members;
    members[0] = floatVectorTypeId;

    uint32_t typeId = m_module.defStructTypeUnique(
      members.size(), members.data());

    m_module.memberDecorateBuiltIn(typeId, 0, spv::BuiltInPosition);
    m_module.decorateBlock(typeId);

    m_module.setDebugName(typeId, "s_per_vertex");
    m_module.setDebugMemberName(typeId, 0, "position");
    return typeId;
  }

  uint32_t DxsoCompiler::emitNewVariable(DxsoRegisterType regType, spv::StorageClass storageClass) {
    return m_module.newVar(
      getPointerTypeId(regType, storageClass),
      storageClass);
  }

  void DxsoCompiler::emitVectorAlu(const DxsoInstructionContext& ctx) {
    auto& dst = getSpirvRegister(ctx.dst);
    uint32_t dstVarId = dst.varId;

    auto dstRegId = ctx.dst.registerId();
    auto& src = ctx.src;

    const uint32_t typeId = getTypeId(dstRegId.type());

    const auto opcode = ctx.instruction.opcode();
    switch (opcode) {
      case DxsoOpcode::Mov:
        dst.varId = spvId(src[0]);
        break;
      case DxsoOpcode::Add:
        dst.varId = m_module.opFAdd(typeId, spvId(src[0]), spvId(src[1]));
        break;
      default:
        Logger::warn(str::format("DxsoCompiler::emitVectorAlu: unimplemented op {0}", opcode));
        break;
    }
  }
  void DxsoCompiler::emitDcl(const DxsoInstructionContext& ctx) {
    const auto type    = ctx.dst.registerId().type();

    const bool input   = type == DxsoRegisterType::Input
                      || type == DxsoRegisterType::Texture;

    const bool sampler = type == DxsoRegisterType::Sampler;

    if (sampler) {
      // Do something else.
      Logger::warn("DxsoCompiler::emitDcl: samplers not implemented.");
      return;
    }

    mapSpirvRegister(ctx.dst, &ctx.dcl);
  }

  DxsoSpirvRegister& DxsoCompiler::getSpirvRegister(const DxsoRegister& reg){
    auto lookupId = reg.registerId();

    for (auto& regMapping : m_regs) {
      if (regMapping.regId == lookupId)
        return regMapping;
    }

    return this->mapSpirvRegister(reg, nullptr);
  }

  DxsoSpirvRegister& DxsoCompiler::mapSpirvRegister(const DxsoRegister& reg, const DxsoDeclaration* optionalPremadeDecl) {
    const auto regId = reg.registerId();
    const uint32_t regNum = regId.num();

    DxsoSpirvRegister spirvRegister;
    spirvRegister.regId = reg.registerId();

    uint32_t inputSlot = InvalidInputSlot;
    uint32_t outputSlot = InvalidOutputSlot;

    auto regType = regId.type();

    if (optionalPremadeDecl != nullptr) {
      const bool input = regType == DxsoRegisterType::Input
                      || regType == DxsoRegisterType::Texture;

      if (input)
        m_vDecls[inputSlot  = allocateInputSlot()]  = *optionalPremadeDecl;
      else
        m_oDecls[outputSlot = allocateOutputSlot()] = *optionalPremadeDecl;
    }
    else {
      if (regType == DxsoRegisterType::Input) {
        if (m_programInfo.majorVersion() != 3 && m_programInfo.type() == DxsoProgramType::PixelShader) {
          auto& dcl = m_vDecls[inputSlot = allocateInputSlot()];
          dcl.reg = reg;
          dcl.usage = DxsoUsage::Color;
          dcl.usageIndex = regNum;
        }
      }
      else if (regType == DxsoRegisterType::RasterizerOut) {
        auto& dcl = m_oDecls[outputSlot = allocateOutputSlot()];
        dcl.reg = reg;

        if (regNum == RasterOutPosition)
          dcl.usage = DxsoUsage::Position;
        else if (regNum == RasterOutFog)
          dcl.usage = DxsoUsage::Fog;
        else
          dcl.usage = DxsoUsage::PointSize;

        dcl.usageIndex = 0;
      }
      else if (regType == DxsoRegisterType::Output) { // TexcoordOut
        auto& dcl = m_oDecls[outputSlot = allocateOutputSlot()];
        dcl.reg = reg;
        dcl.usage = DxsoUsage::Texcoord;
        dcl.usageIndex = regNum;
      }
      else if (regType == DxsoRegisterType::AttributeOut) {
        auto& dcl = m_oDecls[outputSlot = allocateOutputSlot()];
        dcl.reg = reg;
        dcl.usage = DxsoUsage::Color;
        dcl.usageIndex = regNum;
      }
      else if (regType == DxsoRegisterType::Texture) {
        if (m_programInfo.type() == DxsoProgramType::PixelShader) {

          // SM 2+ or 1.4
          if (m_programInfo.majorVersion() >= 2
            || (m_programInfo.majorVersion() == 1
             && m_programInfo.minorVersion() == 4)) {
            auto& dcl = m_vDecls[inputSlot = allocateInputSlot()];
            dcl.reg = reg;
            dcl.usage = DxsoUsage::Texcoord;
            dcl.usageIndex = regNum;
          }
        }
      }
      else if (regType == DxsoRegisterType::ColorOut) {
        auto& dcl = m_oDecls[outputSlot = allocateOutputSlot()];
        dcl.reg = reg;
        dcl.usage = DxsoUsage::Color;
        dcl.usageIndex = regNum;
      }
    }

    auto storageClass = spv::StorageClassPrivate;

    if (inputSlot != InvalidInputSlot)
      storageClass = spv::StorageClassInput;
    else
      storageClass = spv::StorageClassOutput;

    spirvRegister.varId = this->emitNewVariable(regType, storageClass);
    const auto varId = spirvRegister.varId;

    if (inputSlot != InvalidInputSlot) {
      m_module.decorateLocation(varId, inputSlot);
      m_entryPointInterfaces.push_back(varId);

      auto& reg = m_vDecls[inputSlot].reg;
      if (reg.centroid())
        m_module.decorate(varId, spv::DecorationCentroid);
    }
    else if (outputSlot != InvalidOutputSlot) {
      m_module.decorateLocation(varId, outputSlot);
      m_entryPointInterfaces.push_back(varId);

      if (m_programInfo.type() == DxsoProgramType::PixelShader)
        m_module.decorateIndex(varId, 0);
    }

    m_regs.push_back(spirvRegister);
    return m_regs[m_regs.size() - 1];
  }

  uint32_t DxsoCompiler::getTypeId(DxsoRegisterType regType) {
    switch (regType) {
    case DxsoRegisterType::Temp:
    case DxsoRegisterType::Input:
    case DxsoRegisterType::Const:
    case DxsoRegisterType::Texture:
    //case DxsoRegisterType::Addr:
    case DxsoRegisterType::RasterizerOut:
    case DxsoRegisterType::AttributeOut:
    case DxsoRegisterType::Output:
    //case DxsoRegisterType::TexcoordOut:
    case DxsoRegisterType::ColorOut:
    case DxsoRegisterType::DepthOut:
    case DxsoRegisterType::Const2:
    case DxsoRegisterType::Const3:
    case DxsoRegisterType::Const4:
    case DxsoRegisterType::TempFloat16:
    case DxsoRegisterType::MiscType: {
      uint32_t floatType = m_module.defFloatType(32);
      return m_module.defVectorType(floatType, 4);
    }

    case DxsoRegisterType::ConstInt: {
      uint32_t intType = m_module.defIntType(32, true);
      return m_module.defVectorType(intType, 4);
    }

    case DxsoRegisterType::ConstBool:
    case DxsoRegisterType::Loop: {
      return m_module.defIntType(32, true);
    }

    case DxsoRegisterType::Predicate: {
      uint32_t boolType = m_module.defBoolType();
      return m_module.defVectorType(boolType, 4);
    }

    case DxsoRegisterType::Label:
    case DxsoRegisterType::Sampler:
      throw DxvkError("DxsoCompiler::getTypeId: Spirv type requested for Label or Sampler");

    default:
      throw DxvkError("DxsoCompiler::getTypeId: Unknown register type");
    }
  }

  uint32_t DxsoCompiler::allocateInputSlot() {
    const uint32_t slot = m_nextInputSlot;
    m_nextInputSlot++;

    m_interfaceSlots.inputSlots |= slot;
    return slot;
  }

  uint32_t DxsoCompiler::allocateOutputSlot() {
    const uint32_t slot = m_nextOutputSlot;
    m_nextOutputSlot++;

    m_interfaceSlots.outputSlots |= slot;
    return slot;
  }

}