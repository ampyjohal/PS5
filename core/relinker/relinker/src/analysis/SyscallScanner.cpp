#include <relinker/analysis/SyscallScanner.hpp>
#include <codegen/IInstructionScanner.hpp>
#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>

namespace Relinker {

static constexpr std::uint8_t SYSCALL_BYTE0 = 0x0F;
static constexpr std::uint8_t SYSCALL_BYTE1 = 0x05;
static constexpr std::uint8_t INT80_BYTE0 = 0xCD;
static constexpr std::uint8_t INT80_BYTE1 = 0x80;
static constexpr std::uint8_t SYSENTER_BYTE0 = 0x0F;
static constexpr std::uint8_t SYSENTER_BYTE1 = 0x34;
static constexpr std::uint8_t SYSRET_BYTE0 = 0x0F;
static constexpr std::uint8_t SYSRET_BYTE1 = 0x07;
static constexpr std::size_t SYSCALL_NUMBER_LOOKBACK = 8;

enum class ForbiddenInstruction {
    None,
    Invocation,
    Return,
};

static ForbiddenInstruction ClassifyForbiddenInstruction(
    const std::vector<std::uint8_t>& codeSection,
    const std::size_t offset,
    const std::size_t limit) {
    if (offset + 1 >= limit)
        return ForbiddenInstruction::None;

    const std::uint8_t byte0 = codeSection[offset];
    const std::uint8_t byte1 = codeSection[offset + 1];

    const bool isSyscall = (byte0 == SYSCALL_BYTE0 && byte1 == SYSCALL_BYTE1);
    const bool isInt80 = (byte0 == INT80_BYTE0 && byte1 == INT80_BYTE1);
    const bool isSysenter = (byte0 == SYSENTER_BYTE0 && byte1 == SYSENTER_BYTE1);
    const bool isSysret = (byte0 == SYSRET_BYTE0 && byte1 == SYSRET_BYTE1);

    if (isSyscall || isInt80 || isSysenter)
        return ForbiddenInstruction::Invocation;

    if (isSysret)
        return ForbiddenInstruction::Return;

    return ForbiddenInstruction::None;
}

static std::optional<std::uint32_t> ConstantRaxLoad(const std::uint8_t* bytes, const std::size_t length) {
    if (length == 7 && bytes[0] == 0x48 && bytes[1] == 0xC7 && bytes[2] == 0xC0) {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes + 3, sizeof(value));
        return value;
    }

    if (length == 5 && bytes[0] == 0xB8) {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes + 1, sizeof(value));
        return value;
    }

    return std::nullopt;
}

static constexpr std::uint8_t MOV_R10_FROM_RCX[] = {0x49, 0x89, 0xCA};

static std::optional<std::uint32_t> ConstantSyscallNumber(
    const std::vector<std::uint8_t>& codeSection,
    const FileByteOffset codeSectionOffset,
    const std::vector<Codegen::InstructionMatch>& matches,
    const std::size_t syscallIndex) {
    for (std::size_t step = 1; step <= SYSCALL_NUMBER_LOOKBACK && step <= syscallIndex; ++step) {
        const auto& match = matches[syscallIndex - step];
        const std::size_t start = static_cast<std::size_t>(match.Offset - codeSectionOffset);

        if (start >= codeSection.size() || match.Length > codeSection.size() - start)
            return std::nullopt;

        const std::uint8_t* bytes = codeSection.data() + start;

        if (const auto value = ConstantRaxLoad(bytes, match.Length))
            return value;

        const bool movesFirstSyscallArgument = match.Length == sizeof(MOV_R10_FROM_RCX)
            && std::memcmp(bytes, MOV_R10_FROM_RCX, sizeof(MOV_R10_FROM_RCX)) == 0;

        if (!movesFirstSyscallArgument)
            return std::nullopt;
    }

    return std::nullopt;
}

class SyscallScanner : public ISyscallScanner {
public:
    void ScanCodeSectionForSyscalls(
        const std::vector<std::uint8_t>& codeSection,
        FileByteOffset codeSectionOffset,
        FileByteOffset codeSectionSize) override;
};

void SyscallScanner::ScanCodeSectionForSyscalls(
    const std::vector<std::uint8_t>& codeSection,
    const FileByteOffset codeSectionOffset,
    const FileByteOffset codeSectionSize) {
    const std::size_t limit = std::min(codeSection.size(), static_cast<std::size_t>(codeSectionSize));

    const auto scanner = Codegen::MakeInstructionScanner();
    const auto matches = scanner->ScanCodeSection(codeSection, codeSectionOffset, codeSectionSize);

    for (std::size_t index = 0; index < matches.size(); ++index) {
        const auto& match = matches[index];
        const std::size_t offset = static_cast<std::size_t>(match.Offset - codeSectionOffset);

        const auto kind = ClassifyForbiddenInstruction(codeSection, offset, limit);

        if (kind == ForbiddenInstruction::None)
            continue;

        std::ostringstream msg;
        msg << "Forbidden syscall instruction at code offset 0x" << std::hex << match.Offset;

        if (kind == ForbiddenInstruction::Invocation)
            if (const auto number = ConstantSyscallNumber(codeSection, codeSectionOffset, matches, index))
                msg << ": syscall number " << std::dec << *number;

        throw RelinkerException(msg.str(), match.Offset);
    }
}

std::unique_ptr<ISyscallScanner> MakeSyscallScanner() {
    return std::make_unique<SyscallScanner>();
}

class NullSyscallScanner : public ISyscallScanner {
public:
    void ScanCodeSectionForSyscalls(
        const std::vector<std::uint8_t>&,
        FileByteOffset,
        FileByteOffset
    ) override {}
};

std::unique_ptr<ISyscallScanner> MakeNullSyscallScanner() {
    return std::make_unique<NullSyscallScanner>();
}

}
