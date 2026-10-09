#include <relinker/analysis/UnusedNidFilter/IControlFlowGraph.hpp>
#include <codegen/x86/X64InstructionDecoder.hpp>
#include <codegen/CodegenException.hpp>
#include <unordered_map>
#include <queue>
#include <cstring>

namespace Relinker::UnusedNidFilter {

class ControlFlowGraph : public IControlFlowGraph {
public:
    explicit ControlFlowGraph(std::unordered_set<VirtualAddress> reachable)
        : _reachable(std::move(reachable)) {}

    const std::unordered_set<VirtualAddress>& ReachableVaddrs() const override { return _reachable; }
    bool IsReachable(VirtualAddress vaddr) const override { return _reachable.count(vaddr) > 0; }

private:
    std::unordered_set<VirtualAddress> _reachable;
};

namespace {

std::unique_ptr<IControlFlowGraph> buildGraph(
    const std::vector<std::uint8_t>& text,
    VirtualAddress textVaddr,
    VirtualAddress entryVaddr,
    const std::vector<VirtualAddress>& extraEntries,
    const IRelativeRelocationIndex& relativeRelocations,
    const std::vector<StrictDataRegion>* data
) {
    const Codegen::X64InstructionDecoder decoder;
    std::unordered_set<VirtualAddress> reachable;
    std::unordered_set<VirtualAddress> visited;
    std::queue<VirtualAddress> worklist;
    std::vector<VirtualAddress> addressTaken;
    bool speculative = false;

    auto enqueue = [&](VirtualAddress va) {
        if (va < textVaddr || va >= textVaddr + static_cast<VirtualAddress>(text.size())) return;
        if (!visited.insert(va).second) return;
        worklist.push(va);
    };

    auto takeAddress = [&](VirtualAddress va) {
        if (data == nullptr) return;
        if (speculative)
            enqueue(va);
        else
            addressTaken.push_back(va);
    };

    enqueue(entryVaddr);
    for (VirtualAddress va : extraEntries) enqueue(va);
    for (VirtualAddress va : relativeRelocations.Targets()) takeAddress(va);

    while (!worklist.empty() || !speculative) {
        if (worklist.empty()) {
            speculative = true;
            for (VirtualAddress va : addressTaken) enqueue(va);
            continue;
        }

        VirtualAddress va = worklist.front();
        worklist.pop();

        if (va < textVaddr || va >= textVaddr + static_cast<VirtualAddress>(text.size()))
            throw RelinkerException("CFG: jump target outside text segment", va);

        std::size_t bufOff = static_cast<std::size_t>(va - textVaddr);
        std::size_t available = text.size() - bufOff;
        if (available == 0) throw RelinkerException("CFG: zero available bytes at target", va);

        Codegen::DecodedInstructionInfo info;
        if (speculative) {
            try {
                info = decoder.DecodeInstruction(text.data() + bufOff, available);
            } catch (const Codegen::CodegenException&) {
                continue;
            }
        } else {
            info = decoder.DecodeInstruction(text.data() + bufOff, available);
        }
        reachable.insert(va);

        VirtualAddress nextVaddr = va + static_cast<VirtualAddress>(info.Length);

        if (data != nullptr && info.HasRipRelativeDisp && !info.IsTwoByteOpcode && info.Opcode == 0x8D) {
            std::int32_t displacement = 0;
            std::memcpy(&displacement, text.data() + bufOff + info.RipRelativeDispOffset, 4);
            const auto base = static_cast<VirtualAddress>(static_cast<std::int64_t>(nextVaddr) + displacement);
            takeAddress(base);
            for (VirtualAddress target : ReadRelativeTableTargets(*data, base, textVaddr, text.size())) takeAddress(target);
        }

        switch (info.FlowKind) {
            using enum Codegen::ControlFlowKind;
            case Sequential:
                enqueue(nextVaddr);
                break;
            case ConditionalBranch:
                enqueue(nextVaddr);
                if (info.HasBranchTarget)
                    enqueue(static_cast<VirtualAddress>(static_cast<std::int64_t>(nextVaddr) + info.BranchDisp));
                break;
            case UnconditionalJump:
                if (info.HasRipRelativeDisp) {
                    std::int32_t disp = 0;
                    std::memcpy(&disp, text.data() + bufOff + info.RipRelativeDispOffset, 4);
                    VirtualAddress slotVaddr = static_cast<VirtualAddress>(
                        static_cast<std::int64_t>(nextVaddr) + disp
                    );
                    auto resolved = relativeRelocations.TargetOfSlot(slotVaddr);
                    if (resolved.has_value())
                        enqueue(*resolved);
                } else if (info.HasBranchTarget) {
                    enqueue(static_cast<VirtualAddress>(static_cast<std::int64_t>(nextVaddr) + info.BranchDisp));
                }
                break;
            case Call:
                if (info.HasRipRelativeDisp) {
                    std::int32_t disp = 0;
                    std::memcpy(&disp, text.data() + bufOff + info.RipRelativeDispOffset, 4);
                    VirtualAddress slotVaddr = static_cast<VirtualAddress>(
                        static_cast<std::int64_t>(nextVaddr) + disp
                    );
                    auto resolved = relativeRelocations.TargetOfSlot(slotVaddr);
                    if (resolved.has_value())
                        enqueue(*resolved);
                } else if (info.HasBranchTarget) {
                    enqueue(static_cast<VirtualAddress>(static_cast<std::int64_t>(nextVaddr) + info.BranchDisp));
                }
                enqueue(nextVaddr);
                break;
            case IndirectCall:
                enqueue(nextVaddr);
                break;
            case Return:
            case IndirectJump:
            case Trap:
                break;
        }
    }

    return std::make_unique<ControlFlowGraph>(std::move(reachable));
}

}

std::unique_ptr<IControlFlowGraph> BuildControlFlowGraph(
    const std::vector<std::uint8_t>& text,
    VirtualAddress textVaddr,
    VirtualAddress entryVaddr,
    const std::vector<VirtualAddress>& extraEntries,
    const IRelativeRelocationIndex& relativeRelocations
) {
    return buildGraph(text, textVaddr, entryVaddr, extraEntries, relativeRelocations, nullptr);
}

std::unique_ptr<IControlFlowGraph> BuildAddressTakenControlFlowGraph(
    const std::vector<std::uint8_t>& text,
    VirtualAddress textVaddr,
    VirtualAddress entryVaddr,
    const std::vector<VirtualAddress>& extraEntries,
    const IRelativeRelocationIndex& relativeRelocations,
    const std::vector<StrictDataRegion>& data
) {
    return buildGraph(text, textVaddr, entryVaddr, extraEntries, relativeRelocations, &data);
}

}
