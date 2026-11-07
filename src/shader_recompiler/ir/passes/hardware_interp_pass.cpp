// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Gives pixel shader inputs back to the GPU's own interpolation where that can't change the
// result.
//
// GCN pixel shaders interpolate their inputs themselves. v_interp_p1 and v_interp_p2 compute
// P0 + I * (P1 - P0) + J * (P2 - P0) from the values at the triangle's three vertices and the
// barycentrics I and J. Where the driver's interpolation is not bit-exact
// (Profile::needs_manual_interpolation, NVIDIA), the recompiler keeps that arithmetic, so an
// integer carried in a float input (Bloodborne's instance index) truncates exactly as on the
// console. Every input pays for it: three per-vertex reads and the arithmetic, instead of one
// interpolated read.
//
// Most inputs only end up in texture coordinates, colors and similar arithmetic, where a last-bit
// difference is invisible. For each input, this pass checks that every read of it is that formula
// on the hardware's barycentrics, then walks forward from the interpolated values through the
// whole shader, tracking whether each value is still a float or has become a float's bits in an
// integer register. Reaching anything that depends on the exact bits keeps the formula for that
// input; otherwise its reads become plain interpolated ones.

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <optional>
#include <unordered_set>
#include <vector>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {
namespace {

using Interpolation = Info::Interpolation;
using IR::Opcode;

IR::Inst* Producer(const IR::Value& value, Opcode op) {
    if (value.IsImmediate()) {
        return nullptr;
    }
    IR::Inst* const inst = value.TryInst();
    return inst && inst->GetOpcode() == op ? inst : nullptr;
}

IR::Inst* AttributeLoad(const IR::Value& value) {
    return Producer(value, Opcode::GetAttribute);
}

bool LoadsVertex(const IR::Inst* load, IR::Attribute attr, u32 comp, u32 vertex) {
    return load->Arg(0).Attribute() == attr && load->Arg(1).U32() == comp &&
           load->Arg(2).U32() == vertex;
}

/// The interpolation the hardware applies for these barycentrics; none for anything else (the
/// pull model, or a value that isn't a barycentric).
std::optional<Interpolation> HardwareKind(IR::Attribute bary) {
    switch (bary) {
    case IR::Attribute::BaryCoordSmooth:
        return Interpolation{Qualifier::Smooth, Qualifier::None};
    case IR::Attribute::BaryCoordSmoothCentroid:
        return Interpolation{Qualifier::Smooth, Qualifier::Centroid};
    case IR::Attribute::BaryCoordSmoothSample:
        return Interpolation{Qualifier::Smooth, Qualifier::Sample};
    case IR::Attribute::BaryCoordNoPersp:
        return Interpolation{Qualifier::NoPerspective, Qualifier::None};
    case IR::Attribute::BaryCoordNoPerspCentroid:
        return Interpolation{Qualifier::NoPerspective, Qualifier::Centroid};
    case IR::Attribute::BaryCoordNoPerspSample:
        return Interpolation{Qualifier::NoPerspective, Qualifier::Sample};
    default:
        return std::nullopt;
    }
}

/// Writes of a GCN register that SSA has already resolved: nothing reads them anymore. A bit
/// cast that feeds only such writes is just as dead.
bool IsLeftoverWrite(const IR::Inst* inst) {
    const Opcode op = inst->GetOpcode();
    if (op == Opcode::SetVectorRegister || op == Opcode::SetScalarRegister) {
        return true;
    }
    if (op != Opcode::BitCastU32F32 && op != Opcode::BitCastF32U32) {
        return false;
    }
    for (const IR::Use& use : inst->Uses()) {
        if (!IsLeftoverWrite(use.user)) {
            return false;
        }
    }
    return true;
}

/// Whether `inst` has no users besides `expected` and leftover register writes.
bool PrivateTo(const IR::Inst* inst, std::initializer_list<const IR::Inst*> expected) {
    for (const IR::Use& use : inst->Uses()) {
        bool listed = false;
        for (const IR::Inst* user : expected) {
            listed = listed || user == use.user;
        }
        if (!listed && !IsLeftoverWrite(use.user)) {
            return false;
        }
    }
    return true;
}

/// Removes `inst` and its leftover register writes.
void Erase(IR::Inst* inst) {
    const std::vector<IR::Use> uses(inst->Uses().begin(), inst->Uses().end());
    for (const IR::Use& use : uses) {
        Erase(use.user);
    }
    inst->Invalidate();
}

/// One component of one input interpolated in two steps:
///   step1 = fma(P1 - P0, I, P0)     (v_interp_p1)
///   value = fma(P2 - P0, J, step1)  (v_interp_p2, with its own read of P0)
struct Chain {
    IR::Inst* value;
    IR::Inst* step1;
    IR::Inst* edge1;                ///< P1 - P0
    IR::Inst* edge2;                ///< P2 - P0
    std::array<IR::Inst*, 4> loads; ///< P0 (step 1), P1, P0 (step 2), P2
    IR::Attribute input;
    u32 comp;
    Interpolation kind;
};

std::optional<Chain> FindChain(IR::Inst& value) {
    if (value.GetOpcode() != Opcode::FPFma32) {
        return std::nullopt;
    }
    Chain chain{};
    chain.value = &value;
    chain.edge2 = Producer(value.Arg(0), Opcode::FPSub32);
    IR::Inst* const j = AttributeLoad(value.Arg(1));
    chain.step1 = Producer(value.Arg(2), Opcode::FPFma32);
    if (!chain.edge2 || !j || !chain.step1) {
        return std::nullopt;
    }
    chain.edge1 = Producer(chain.step1->Arg(0), Opcode::FPSub32);
    IR::Inst* const i = AttributeLoad(chain.step1->Arg(1));
    IR::Inst* const p0 = AttributeLoad(chain.step1->Arg(2));
    if (!chain.edge1 || !i || !p0 || AttributeLoad(chain.edge1->Arg(1)) != p0) {
        return std::nullopt;
    }
    IR::Inst* const p1 = AttributeLoad(chain.edge1->Arg(0));
    IR::Inst* const p0_again = AttributeLoad(chain.edge2->Arg(1));
    IR::Inst* const p2 = AttributeLoad(chain.edge2->Arg(0));
    if (!p1 || !p0_again || !p2) {
        return std::nullopt;
    }
    chain.input = p0->Arg(0).Attribute();
    chain.comp = p0->Arg(1).U32();
    if (!IR::IsParam(chain.input)) {
        return std::nullopt;
    }
    const IR::Attribute bary = i->Arg(0).Attribute();
    const auto kind = HardwareKind(bary);
    if (!kind || !LoadsVertex(i, bary, 0, 0) || !LoadsVertex(j, bary, 1, 0) ||
        !LoadsVertex(p0, chain.input, chain.comp, 0) ||
        !LoadsVertex(p1, chain.input, chain.comp, 1) ||
        !LoadsVertex(p0_again, chain.input, chain.comp, 0) ||
        !LoadsVertex(p2, chain.input, chain.comp, 2)) {
        return std::nullopt;
    }
    // Nothing else may see the intermediate steps or the per-vertex values: they disappear.
    if (!PrivateTo(chain.step1, {chain.value}) || !PrivateTo(chain.edge1, {chain.step1}) ||
        !PrivateTo(chain.edge2, {chain.value}) || !PrivateTo(p0, {chain.edge1, chain.step1}) ||
        !PrivateTo(p1, {chain.edge1}) || !PrivateTo(p0_again, {chain.edge2}) ||
        !PrivateTo(p2, {chain.edge2})) {
        return std::nullopt;
    }
    chain.kind = *kind;
    chain.loads = {p0, p1, p0_again, p2};
    return chain;
}

/// How an interpolated value is held: as a float, or as a float's bits in an integer register
/// (GCN registers hold bits; SSA keeps the bit casts until they fold).
enum class Form : u8 { Float, Bits };

/// What a use does with a value that may be off by the last bit.
enum class Effect : u8 {
    Sensitive, ///< the result can change: keep the exact formula
    Carries,   ///< the result carries the value on: keep following
    Harmless,  ///< the value ends here without changing anything visible
};

bool IsFloatComparison(Opcode op) {
    switch (op) {
    case Opcode::FPOrdEqual32:
    case Opcode::FPUnordEqual32:
    case Opcode::FPOrdNotEqual32:
    case Opcode::FPUnordNotEqual32:
    case Opcode::FPOrdLessThan32:
    case Opcode::FPUnordLessThan32:
    case Opcode::FPOrdGreaterThan32:
    case Opcode::FPUnordGreaterThan32:
    case Opcode::FPOrdLessThanEqual32:
    case Opcode::FPUnordLessThanEqual32:
    case Opcode::FPOrdGreaterThanEqual32:
    case Opcode::FPUnordGreaterThanEqual32:
        return true;
    default:
        return false;
    }
}

bool IsTextureLookup(Opcode op) {
    switch (op) {
    case Opcode::ImageSampleRaw:
    case Opcode::ImageSampleImplicitLod:
    case Opcode::ImageSampleExplicitLod:
    case Opcode::ImageSampleDrefImplicitLod:
    case Opcode::ImageSampleDrefExplicitLod:
    case Opcode::ImageGather:
    case Opcode::ImageGatherDref:
    case Opcode::ImageGradient:
    case Opcode::ImageQueryLod:
        return true;
    default:
        return false;
    }
}

bool IsRounding(Opcode op) {
    return op == Opcode::FPFloor32 || op == Opcode::FPCeil32 || op == Opcode::FPTrunc32 ||
           op == Opcode::FPFract32 || op == Opcode::FPRoundEven32;
}

bool IsFloatType(IR::Type type) {
    constexpr IR::Type any_float = IR::Type::F16 | IR::Type::F32 | IR::Type::F64 | IR::Type::F16x2 |
                                   IR::Type::F16x3 | IR::Type::F16x4 | IR::Type::F32x2 |
                                   IR::Type::F32x3 | IR::Type::F32x4 | IR::Type::F64x2 |
                                   IR::Type::F64x3 | IR::Type::F64x4;
    return (type & any_float) != IR::Type::Void;
}

/// A float comparison whose answer can't change when its operand moves by the last bit.
bool ComparisonIsHarmless(const IR::Inst* cmp) {
    for (size_t arg = 0; arg < 2; ++arg) {
        const IR::Value operand = cmp->Arg(arg);
        if (!operand.IsImmediate() || operand.Type() != IR::Type::F32) {
            continue;
        }
        // Zero: equal values at the three vertices give exactly zero either way. A constant
        // strictly between two integers: an integer-valued input lands on the same side.
        const f32 constant = operand.F32();
        if (constant == 0.0f || constant != std::trunc(constant)) {
            return true;
        }
    }
    // |a| against |b| picks a cube map face; on an edge, both faces hold the same texels.
    return Producer(cmp->Arg(0), Opcode::FPAbs32) && Producer(cmp->Arg(1), Opcode::FPAbs32);
}

Effect EffectOf(const IR::Inst* user, Form form) {
    const Opcode op = user->GetOpcode();
    if (op == Opcode::Phi || op == Opcode::BitCastU32F32 || op == Opcode::BitCastF32U32) {
        return Effect::Carries;
    }
    if (op == Opcode::SetVectorRegister || op == Opcode::SetScalarRegister) {
        return Effect::Harmless; // already read through by SSA
    }
    const bool returns_nothing = user->Type() == IR::Type::Void;
    if (form == Form::Bits) {
        // Moving the bits to another lane keeps them as they are; any other integer use of a
        // float's bits sees the last bit.
        if (op == Opcode::QuadBroadcast || op == Opcode::Shuffle || op == Opcode::ShuffleXor) {
            return Effect::Carries;
        }
        return returns_nothing ? Effect::Harmless : Effect::Sensitive;
    }
    if (IsFloatComparison(op)) {
        return ComparisonIsHarmless(user) ? Effect::Harmless : Effect::Sensitive;
    }
    if (IsRounding(op)) {
        return Effect::Sensitive;
    }
    // Color exports as 16-bit floats (v_cvt_pkrtz_f16_f32) round to 11 bits, and texture
    // coordinates are filtered: a last-bit difference doesn't survive either.
    if (op == Opcode::PackHalf2x16 || IsTextureLookup(op) || returns_nothing) {
        return Effect::Harmless;
    }
    return IsFloatType(user->Type()) ? Effect::Carries : Effect::Sensitive;
}

/// Follows the interpolated values of one input through the shader; true when one reaches a
/// sensitive use. Gives up (true) past a fixed number of steps.
bool ReachesSensitiveUse(const std::vector<IR::Inst*>& values) {
    constexpr size_t MaxSteps = 4096;
    struct Step {
        const IR::Inst* inst;
        Form form;
    };
    const auto key = [](const IR::Inst* inst, Form form) {
        return reinterpret_cast<uintptr_t>(inst) | static_cast<uintptr_t>(form);
    };
    static_assert(alignof(IR::Inst) >= 2);
    std::unordered_set<uintptr_t> visited;
    std::deque<Step> queue;
    for (const IR::Inst* value : values) {
        if (visited.insert(key(value, Form::Float)).second) {
            queue.push_back({value, Form::Float});
        }
    }
    while (!queue.empty()) {
        const Step step = queue.front();
        queue.pop_front();
        for (const IR::Use& use : step.inst->Uses()) {
            const IR::Inst* user = use.user;
            switch (EffectOf(user, step.form)) {
            case Effect::Sensitive:
                return true;
            case Effect::Harmless:
                break;
            case Effect::Carries: {
                const Opcode op = user->GetOpcode();
                const Form next = op == Opcode::BitCastU32F32 ? Form::Bits
                                  : op == Opcode::Phi         ? step.form
                                                              : Form::Float;
                if (visited.insert(key(user, next)).second) {
                    if (visited.size() > MaxSteps) {
                        return true;
                    }
                    queue.push_back({user, next});
                }
                break;
            }
            }
        }
    }
    return false;
}

struct InputState {
    std::vector<Chain> chains;
    std::optional<Interpolation> kind;
    bool mixed_kinds = false;
};

} // namespace

u32 HardwareInterpolationInputs(IR::Program& program, const RuntimeInfo& runtime_info, bool apply) {
    Info& info = program.info;
    std::array<InputState, IR::NumParams> inputs{};
    std::unordered_set<const IR::Inst*> chain_loads;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (auto chain = FindChain(inst)) {
                InputState& state = inputs[u32(chain->input) - u32(IR::Attribute::Param0)];
                if (state.kind && (state.kind->primary != chain->kind.primary ||
                                   state.kind->auxiliary != chain->kind.auxiliary)) {
                    state.mixed_kinds = true;
                }
                state.kind = chain->kind;
                chain_loads.insert(chain->loads.begin(), chain->loads.end());
                state.chains.push_back(*chain);
            }
        }
    }
    // Inputs read in any other way, even once, keep the formula everywhere.
    std::array<bool, IR::NumParams> other_reads{};
    for (IR::Block* const block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == Opcode::GetAttribute && inst.HasUses() &&
                IR::IsParam(inst.Arg(0).Attribute()) && !chain_loads.contains(&inst)) {
                other_reads[u32(inst.Arg(0).Attribute()) - u32(IR::Attribute::Param0)] = true;
            }
        }
    }
    // Two inputs fed by one vertex output would need two interpolations at one location
    // (invalid SPIR-V that some captures hold anyway): leave those as they are.
    const auto& ps_inputs = runtime_info.hw.fs.inputs;
    const u32 num_ps_inputs = std::min<u32>(runtime_info.hw.fs.num_inputs, IR::NumParams);
    std::array<bool, IR::NumParams> shared_location{};
    for (u32 a = 0; a < num_ps_inputs; ++a) {
        for (u32 b = 0; b < num_ps_inputs; ++b) {
            if (a != b && !ps_inputs[b].IsDefault() &&
                ps_inputs[b].param_index == ps_inputs[a].param_index) {
                shared_location[a] = true;
            }
        }
    }

    u32 given_back = 0;
    for (u32 n = 0; n < IR::NumParams; ++n) {
        const InputState& state = inputs[n];
        if (state.chains.empty() || state.mixed_kinds || other_reads[n] || shared_location[n] ||
            info.fs_interpolation[n].primary != Qualifier::PerVertex) {
            continue;
        }
        std::vector<IR::Inst*> values;
        values.reserve(state.chains.size());
        for (const Chain& chain : state.chains) {
            values.push_back(chain.value);
        }
        if (!ReachesSensitiveUse(values)) {
            given_back |= 1u << n;
        }
    }
    if (!apply) {
        return given_back;
    }
    for (u32 n = 0; n < IR::NumParams; ++n) {
        if (!(given_back & (1u << n))) {
            continue;
        }
        for (const Chain& chain : inputs[n].chains) {
            IR::IREmitter ir{*chain.value->GetParent(),
                             IR::Block::InstructionList::s_iterator_to(*chain.value)};
            chain.value->ReplaceUsesWithAndRemove(ir.GetAttribute(chain.input, chain.comp));
            // The per-vertex reads must go too: none may reach the SPIR-V of an interpolated
            // input.
            Erase(chain.step1);
            Erase(chain.edge1);
            Erase(chain.edge2);
            for (IR::Inst* const load : chain.loads) {
                Erase(load);
            }
        }
        info.fs_interpolation[n] = *inputs[n].kind;
    }
    return given_back;
}

} // namespace Shader::Optimization
