from pathlib import Path

root = Path(__file__).resolve().parent.parent
source = Path('core/shader/recompiler/SpirvBackend/src/SpirvSpecialization.cpp')
old = (root / source).read_text()
new = (Path('C:/repos/AnyPS5-speccache') / source).read_text()
out = root / 'profiling' / 'spec-ab'
out.mkdir(exist_ok=True)

cleanup = old[old.index('    void removeDeadScalarConstants() {'):old.index('    std::optional<std::uint32_t> value(')]
cleanup = cleanup.replace('removed.insert(Result(instruction));', 'at(removed, Result(instruction)) = 1u;')
new = new.replace('#include <optional>\n', '#include <optional>\n#include <set>\n')
new = new.replace('        orderPhis();\n', '        orderPhis();\n        removeDeadScalarConstants();\n')
new = new.replace('private:\n    struct Block {', 'private:\n' + cleanup + '    struct Block {')
start = new.index('        const auto linearSelection = ')
end = new.index('        for (const auto label : labels)', start)
new = new[:start] + new[end:]
start = new.index('            if (target != 0u) {')
end = new.index('            auto& successors = ', start)
new = new[:start] + '''            if (target != 0u) {
                for (auto index = range.first; index <= range.last; ++index) if (Opcode(instructions[index]) == spv::OpSelectionMerge) instructions[index].clear();
                terminal = Make(spv::OpBranch, {target});
                op = spv::OpBranch;
                changed = true;
            }
''' + new[end:]
start = new.index('    const Block& block(')
end = new.index('    bool prune()', start)
new = new[:start] + new[end:]
new = new.replace('    std::vector<std::uint32_t> visitStamps;\n', '').replace('    std::uint32_t visitGeneration = 0;\n', '')

(out / 'old_trial.cpp').write_text(old.replace('namespace ShaderRecompiler {', 'namespace OldSpec {'))
(out / 'SpirvSpecialization.cpp').write_text(new)
print('Prepared exact trial baseline and adapted vector implementation; production sources unchanged.')
