#!/usr/bin/env bash
# Formats the tree in place. Data tables fenced with `// clang-format off`
# keep their hand-built shape.
set -euo pipefail
cd "$(dirname "$0")/.."
files=$(git ls-files 'src/*.cpp' 'src/*.hpp' 'tests/*.cpp' 'tests/*.hpp')
clang-format -i $files
echo "formatted $(echo "$files" | wc -l) files"
