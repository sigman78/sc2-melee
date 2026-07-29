#!/usr/bin/env bash
# Formats the tree in place. Data tables fenced with `// clang-format off`
# keep their hand-built shape.
set -euo pipefail
cd "$(dirname "$0")/.."
source tools/clang-env.sh
sc2m_check_clang_version clang-format "$SC2M_CLANG_FORMAT_VERSION"
files=$(git ls-files 'src/*.cpp' 'src/*.hpp' 'tests/*.cpp' 'tests/*.hpp')
clang-format -i $files
echo "formatted $(echo "$files" | wc -l) files"
