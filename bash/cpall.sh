#!/usr/bin/env bash

root="${1:-.}"

output=""

while IFS= read -r -d '' file; do
    name=$(basename "$file")

    if [[ "$file" == *.cpp ]]; then
        output+="\`\`\`cpp
// $name
$(cat "$file")
\`\`\`

"
    elif [[ "$file" == *.cppm ]]; then
        output+="\`\`\`cppm
// $name
$(cat "$file")
\`\`\`

"
    fi
done < <(find "$root" \( -name "*.cpp" -o -name "*.cppm" \) -print0)

printf "%b" "$output"
