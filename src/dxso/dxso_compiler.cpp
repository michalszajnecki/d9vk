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
      m_oPtrs.at(i) = 0;
    }

    this->emitInit();
  }

  void DxsoCompiler::processInstruction(
    const DxsoInstructionContext& ctx) {
    const DxsoOpcode opcode = ctx.instruction.opcode();

    switch (opcode) {
    case DxsoOpcode::Dcl:
      return this->emitDcl(ctx);

    case DxsoOpcode::Def:
    case DxsoOpcode::DefI:
    case DxsoOpcode::DefB:
      return this->emitDef(opcode, ctx);

    case DxsoOpcode::Mov:
    case DxsoOpcode::Add:
    case DxsoOpcode::Sub:
    case DxsoOpcode::Mad:
    case DxsoOpcode::Mul:
    case DxsoOpcode::Rcp:
    case DxsoOpcode::Rsq:
    case DxsoOpcode::Dp3:
    case DxsoOpcode::Dp4:
    case DxsoOpcode::Min:
    case DxsoOpcode::Max:
    case DxsoOpcode::Abs:
    case DxsoOpcode::Nrm:
    case DxsoOpcode::LogP:
    case DxsoOpcode::Log:
    case DxsoOpcode::Frc:
    case DxsoOpcode::Dp2Add:
      return this->emitVectorAlu(ctx);

    default:
      Logger::warn(str::format("DxsoCompiler::processInstruction: unhandled opcode: {0}", opcode));
      break;
    }
  }

  Rc<DxvkShader> DxsoCompiler::finalize() {
    for (uint32_t i = 0; i < m_nextOutputSlot; i++)
      m_module.opStore(m_oPtrs[i], getSpirvRegister(m_oDecls[i].reg).varId);

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

  uint32_t DxsoCompiler::emitNewVariable(DxsoRegisterType regType, spv::StorageClass storageClass) {
    return m_module.newVar(
      getPointerTypeId(regType, storageClass),
      storageClass);
  }

  uint32_t DxsoCompiler::emitRegisterSwizzle(uint32_t typeId, uint32_t varId, DxsoRegSwizzle swizzle, uint32_t count) {
    if (swizzle == IdentitySwizzle)
      return varId;

    std::array<uint32_t, 4> indices;

    for (uint32_t i = 0; i < count; i++)
      indices[i] = swizzle[i];

    return m_module.opVectorShuffle(typeId, varId, varId, count, indices.data());
  }

  uint32_t DxsoCompiler::emitSrcOperandModifier(uint32_t typeId, uint32_t varId, DxsoRegModifier modifier, uint32_t count) {
    uint32_t result = varId;

    // 1 - r
    if (modifier == DxsoRegModifier::Comp) {
      uint32_t vec = m_module.constvec4f32(
        count >= 1 ? 1.0f : 0.0f,
        count >= 2 ? 1.0f : 0.0f,
        count >= 3 ? 1.0f : 0.0f,
        count >= 4 ? 1.0f : 0.0f);
      result = m_module.opFSub(typeId, vec, varId);
    }

    // r * 2
    if (modifier == DxsoRegModifier::X2
     || modifier == DxsoRegModifier::X2Neg) {
      uint32_t vec2 = m_module.constvec4f32(2.0f, 2.0f, 2.0f, 2.0f);
      result = m_module.opFMul(typeId, vec2, varId);
    }

    // abs( r )
    if (modifier == DxsoRegModifier::Abs
     || modifier == DxsoRegModifier::AbsNeg) {
      result = m_module.opFAbs(typeId, varId);
    }

    // !r
    if (modifier == DxsoRegModifier::Not) {
      uint32_t one = m_module.constBool(true);
      result = m_module.opBitwiseXor(typeId, varId, one);
    }

    // r / r.z
    // r / r.w
    if (modifier == DxsoRegModifier::Dz
     || modifier == DxsoRegModifier::Dw) {
      const uint32_t index = modifier == DxsoRegModifier::Dz ? 2 : 3;

      std::array<uint32_t, 4> indices = { index, index, index, index };

      uint32_t component = m_module.opVectorShuffle(typeId, result, result, 4, indices.data());
      result = m_module.opFDiv(typeId, result, component);
    }

    // -r
    // Treating as -r
    // Treating as -r
    // -r * 2
    // -abs(r)
    if (modifier == DxsoRegModifier::Neg
     || modifier == DxsoRegModifier::BiasNeg
     || modifier == DxsoRegModifier::SignNeg
     || modifier == DxsoRegModifier::X2Neg
     || modifier == DxsoRegModifier::AbsNeg) {
      result = m_module.opFNegate(typeId, result);
    }

    return result;
  }

  uint32_t DxsoCompiler::emitRegisterLoad(const DxsoRegister& reg, uint32_t count) {
    const uint32_t typeId = spvType(reg);

    uint32_t result = spvId(reg);
    result = emitRegisterSwizzle(typeId, result, reg.swizzle(), count);
    result = emitSrcOperandModifier(typeId, result, reg.modifier(), count);

    return result;
  }

  uint32_t DxsoCompiler::emitDstOperandModifier(uint32_t typeId, uint32_t varId, bool saturate, bool partialPrecision) {
    uint32_t result = varId;

    if (saturate) {
      uint32_t vec0 = m_module.constvec4f32(0.0f, 0.0f, 0.0f, 0.0f);
      uint32_t vec1 = m_module.constvec4f32(0.0f, 0.0f, 0.0f, 0.0f);

      result = m_module.opFClamp(typeId, result, vec0, vec1);
    }

    // Partial precision is currently ignored.
    // I'll wait for issues when some game needs this for some stupid assumption
    // about crap being truncated until this gets implemented.

    return result;
  }

  uint32_t DxsoCompiler::emitWriteMask(uint32_t typeId, uint32_t dst, uint32_t src, DxsoRegMask writeMask) {
    if (writeMask == IdentityWriteMask)
      return src;
    
    std::array<uint32_t, 4> components;
    uint32_t srcId = 4;
    for (uint32_t i = 0; i < 4; i++)
      components[i] = writeMask[i] ? srcId++ : i;

    return m_module.opVectorShuffle(typeId, dst, src, 4, components.data());
  }

  void DxsoCompiler::emitVectorAlu(const DxsoInstructionContext& ctx) {
    const auto& dst = ctx.dst;
    const auto& src = ctx.src;
    const uint32_t typeId = spvType(dst);

    const auto opcode = ctx.instruction.opcode();
    uint32_t result;
    switch (opcode) {
      case DxsoOpcode::Mov:
        result = emitRegisterLoad(src[0]);
        break;
      case DxsoOpcode::Add:
        result = m_module.opFAdd(typeId, emitRegisterLoad(src[0]), emitRegisterLoad(src[1]));
        break;
      case DxsoOpcode::Sub:
        result = m_module.opFSub(typeId, emitRegisterLoad(src[0]), emitRegisterLoad(src[1]));
        break;
      case DxsoOpcode::Mad:
        result = m_module.opFFma(
          typeId,
          emitRegisterLoad(src[0]),
          emitRegisterLoad(src[1]),
          emitRegisterLoad(src[2]));
        break;
      case DxsoOpcode::Mul:
        result = m_module.opFSub(typeId, emitRegisterLoad(src[0]), emitRegisterLoad(src[1]));
        break;
      case DxsoOpcode::Rcp:
        result = m_module.opFDiv(typeId,
          m_module.constvec4f32(1.0f, 1.0f, 1.0f, 1.0f),
          emitRegisterLoad(src[0]));
        break;
      case DxsoOpcode::Rsq:
        result = m_module.opInverseSqrt(typeId, emitRegisterLoad(src[0]));
        break;
      case DxsoOpcode::Dp3:
        result = m_module.opDot(typeId, emitRegisterLoad(src[0], 3), emitRegisterLoad(src[1], 3));
        break;
      case DxsoOpcode::Dp4:
        result = m_module.opDot(typeId, emitRegisterLoad(src[0]), emitRegisterLoad(src[1]));
        break;
      case DxsoOpcode::Min:
        result = m_module.opFMin(typeId, emitRegisterLoad(src[0]), emitRegisterLoad(src[1]));
        break;
      case DxsoOpcode::Max:
        result = m_module.opFMax(typeId, emitRegisterLoad(src[0]), emitRegisterLoad(src[1]));
        break;
      case DxsoOpcode::Abs:
        result = m_module.opFAbs(typeId, emitRegisterLoad(src[0]));
        break;
      case DxsoOpcode::Nrm: {
        // Nrm is 3D...

        uint32_t vec3 = emitRegisterLoad(src[0], 3);

        // r * rsq(r . r);
        result = m_module.opFMul(
          typeId,
          emitRegisterLoad(src[0]),
          m_module.opInverseSqrt(
            typeId,
            m_module.opDot(typeId, vec3, vec3)));
      } break;
      case DxsoOpcode::LogP:
      case DxsoOpcode::Log:
        result = m_module.opLog2(typeId, emitRegisterLoad(src[0]));
        break;
      case DxsoOpcode::Frc:
        result = m_module.opFract(typeId, emitRegisterLoad(src[0]));
        break;
      case DxsoOpcode::Dp2Add:
        result = m_module.opDot(typeId, emitRegisterLoad(src[0], 2), emitRegisterLoad(src[1], 2));
        result = m_module.opFAdd(typeId, result, emitRegisterLoad(src[2]));
        break;
      default:
        Logger::warn(str::format("DxsoCompiler::emitVectorAlu: unimplemented op {0}", opcode));
        return;
    }

    result = emitDstOperandModifier(typeId, result, dst.saturate(), dst.partialPrecision());

    auto& dstSpvReg = getSpirvRegister(dst);
    dstSpvReg.varId = emitWriteMask(typeId, dstSpvReg.varId, result, dst.writeMask());
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

  void DxsoCompiler::emitDef(DxsoOpcode opcode, const DxsoInstructionContext& ctx) {
    switch (opcode) {
      case DxsoOpcode::Def:  this->emitDefF(ctx); break;
      case DxsoOpcode::DefI: this->emitDefI(ctx); break;
      case DxsoOpcode::DefB: this->emitDefB(ctx); break;
      default:
        throw DxvkError("DxsoCompiler::emitDef: Invalid definition opcode");
        break;
    }
  }

  void DxsoCompiler::emitDefF(const DxsoInstructionContext& ctx) {
    DxsoSpirvRegister reg;
    reg.regId = ctx.dst.registerId();

    const float* data = reinterpret_cast<const float*>(ctx.def.data());
    reg.varId = m_module.constvec4f32(data[0], data[1], data[2], data[3]);

    m_regs.push_back(reg);
  }

  void DxsoCompiler::emitDefI(const DxsoInstructionContext& ctx) {
    DxsoSpirvRegister reg;
    reg.regId = ctx.dst.registerId();

    const auto& data = ctx.def;
    reg.varId = m_module.constvec4i32(data[0], data[1], data[2], data[3]);

    m_regs.push_back(reg);
  }

  void DxsoCompiler::emitDefB(const DxsoInstructionContext& ctx) {
    DxsoSpirvRegister reg;
    reg.regId = ctx.dst.registerId();

    bool data = ctx.def[0] != 0;
    reg.varId = m_module.constBool(data);

    m_regs.push_back(reg);
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

    spv::BuiltIn builtIn = spv::BuiltInMax;

    if (optionalPremadeDecl != nullptr) {
      const bool input = regType == DxsoRegisterType::Input
                      || regType == DxsoRegisterType::Texture;

      auto& decl = *optionalPremadeDecl;

      if (input)
        m_vDecls[inputSlot = allocateInputSlot()]   = decl;
      else {
        m_oDecls[outputSlot = allocateOutputSlot()] = decl;

        if (decl.usage == DxsoUsage::Position)
          builtIn = spv::BuiltInPosition;
        else if (decl.usage == DxsoUsage::PointSize)
          builtIn = spv::BuiltInPointSize;
      }
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

        if (regNum == RasterOutPosition) {
          dcl.usage = DxsoUsage::Position;
          builtIn = spv::BuiltInPosition;
        }
        else if (regNum == RasterOutFog)
          dcl.usage = DxsoUsage::Fog;
        else {
          dcl.usage = DxsoUsage::PointSize;
          builtIn = spv::BuiltInPointSize;
        }

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

    uint32_t ptrId = this->emitNewVariable(regType, storageClass);

    if (inputSlot != InvalidInputSlot) {
      m_module.decorateLocation(ptrId, inputSlot);
      m_entryPointInterfaces.push_back(ptrId);

      auto& reg = m_vDecls[inputSlot].reg;
      if (reg.centroid())
        m_module.decorate(ptrId, spv::DecorationCentroid);
    }
    else if (outputSlot != InvalidOutputSlot) {
      m_oPtrs[outputSlot] = ptrId;

      m_module.decorateLocation(ptrId, outputSlot);
      m_entryPointInterfaces.push_back(ptrId);

      if (m_programInfo.type() == DxsoProgramType::PixelShader)
        m_module.decorateIndex(ptrId, 0);
    }

    if (builtIn != spv::BuiltInMax)
      m_module.decorateBuiltIn(ptrId, builtIn);

    if (inputSlot != InvalidInputSlot)
      spirvRegister.varId = m_module.opLoad(spvType(reg), ptrId);
    else
      spirvRegister.varId = m_module.constvec4f32(0.0f, 0.0f, 0.0f, 0.0f);

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