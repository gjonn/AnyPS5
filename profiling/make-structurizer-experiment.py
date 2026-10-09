"""Create an offline-only variant; production sources and cache version stay unchanged."""
from pathlib import Path
import sys

source = Path("core/shader/recompiler/ControlFlow/src/Structurizer.cpp").read_text()
start = source.index("void Structurizer::computePostDominators(")
end = source.index("void Structurizer::computeBackEdges(", start)
region = source[start:end]
needle = "for (auto& block : graph.blocks) {"
position = region.rindex(needle)
region = region[:position] + region[position:].replace(
    needle,
    "for (auto it = graph.blocks.rbegin(); it != graph.blocks.rend(); ++it) {\n            auto& block = *it;",
    1,
)
source = source[:start] + region + source[end:]
if "--sorted" in sys.argv[2:]:
    start = source.index("void Structurizer::computeDominatorTree(")
    end = source.index("void Structurizer::computeBackEdges(", start)
    region = source[start:end]
    old = "addUnique(next, block.id);\n                sortUnique(next);"
    assert region.count(old) == 2
    region = region.replace(old, "const auto position = std::lower_bound(next.begin(), next.end(), block.id);\n                if (position == next.end() || *position != block.id) next.insert(position, block.id);")
    source = source[:start] + region + source[end:]
Path(sys.argv[1]).write_text(source)
