#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace ShaderRecompiler;

namespace {

void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

void CheckDecode() {
    const std::array<std::pair<std::uint32_t, RdnaOperandKind>, 4> sources{{
        {235u, RdnaOperandKind::SrcSharedBase},
        {236u, RdnaOperandKind::SrcSharedLimit},
        {237u, RdnaOperandKind::SrcPrivateBase},
        {238u, RdnaOperandKind::SrcPrivateLimit},
    }};
    for (const auto& [code, kind] : sources) {
        const auto operand = DecodeRdnaScalarSource(code, 0u);
        Require(operand.kind == kind, "scalar source " + std::to_string(code) + " decoded to the wrong operand kind");
    }
}

void CheckRefusesTranslation() {
    const std::array<std::uint32_t, 1> code{0xbe8022ebu};
    const auto instruction = DecodeRdnaInstruction(0u, code, 0u);
    Require(instruction.source0.kind == RdnaOperandKind::SrcSharedBase,
        "the move decodes source0 as kind " + std::to_string(static_cast<unsigned>(instruction.source0.kind)));
}

}

int main() {
    CheckDecode();
    CheckRefusesTranslation();
    std::puts("aperture sources tests passed");
    return 0;
}
