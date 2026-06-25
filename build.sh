#!/bin/bash
# build.sh — Build script for VisiBox
# Handles the Makefile tab-fix that configure doesn't do,
# then builds the project.
set -e

cd "$(dirname "$0")"

# Regenerate configure if needed
if [ ! -f configure ] || [ configure.ac -nt configure ]; then
    echo ">>> Running autoconf..."
    autoconf
fi

# Run configure if needed
if [ ! -f Makefile ] || [ Makefile.in -nt Makefile ] || [ configure -nt Makefile ]; then
    echo ">>> Running configure..."
    ./configure --enable-minimal-config
fi

# Fix Makefile recipe indentation (configure generates spaces instead of tabs)
python3 -c "
makefile = 'Makefile'
with open(makefile, 'r') as f:
    content = f.read()
lines = content.split('\n')
result = []
fixed = 0
for line in lines:
    if line.startswith('        ') and not line.lstrip().startswith('#'):
        result.append('\t' + line.lstrip())
        fixed += 1
    else:
        result.append(line)
with open(makefile, 'w') as f:
    f.write('\n'.join(result))
print(f'  Fixed {fixed} recipe lines')
"

# Build
echo ">>> Building visibox..."
make -j"$(nproc)"

echo ">>> Build complete: $(ls -lh bash | awk '{print $5, $NF}')"