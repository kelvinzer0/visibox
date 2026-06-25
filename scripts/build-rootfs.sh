#!/usr/bin/env bash
# ╔══════════════════════════════════════════════════════════════╗
# ║  build-rootfs.sh — Create minimal Linux rootfs for worker   ║
# ║  No root/debootstrap required — copies host binaries + deps ║
# ╚══════════════════════════════════════════════════════════════╝
set -euo pipefail

ROOTFS="/home/z/my-project/rootfs"
BINS=(
  bash sh ls cp mv rm mkdir rmdir cat head tail ln chmod
  touch tr wc sort uniq cut basename dirname expr
  printf readlink realpath stat date sleep env tee yes true false test
  find xargs tar gzip gunzip bzip2 xz zip unzip
  grep sed awk diff patch
  curl wget ssh scp git
  python3 node npm npx make gcc g++ cc ld ar ranlib strip
  pkg-config cmake
  id whoami uname hostname pwd which file du df free ps kill
  timeout nohup setsid
  pip3 pip nproc jq
)

echo "═══════════════════════════════════════════════════"
echo "  Building minimal rootfs at ${ROOTFS}"
echo "═══════════════════════════════════════════════════"

# ─── 1. Create directory structure ────────────────────────────
echo "[1/5] Creating directory structure..."
DIRS=(
  bin sbin usr/bin usr/sbin usr/local/bin usr/local/sbin
  lib lib64 usr/lib usr/lib64
  etc etc/ssl etc/ssl/certs etc/ssh
  dev proc sys tmp var var/log var/tmp var/cache
  root root/.ssh root/.local
  home worker home/worker/.ssh home/worker/.local
  opt run
  usr/share/ca-certificates
)
for d in "${DIRS[@]}"; do
  mkdir -p "${ROOTFS}/${d}"
done

# ─── 2. Copy binaries and their shared libs ───────────────────
echo "[2/5] Copying binaries and resolving dependencies..."

copy_binary() {
  local src="$1"
  local dst_dir="$2"
  [[ ! -f "$src" ]] && return 0
  local name
  name=$(basename "$src")
  cp -L "$src" "${dst_dir}/${name}" 2>/dev/null && chmod +x "${dst_dir}/${name}"

  if ldd "$src" &>/dev/null; then
    ldd "$src" 2>/dev/null | grep -o '/[^ ]*' | while read -r lib; do
      [[ -f "$lib" ]] || continue
      mkdir -p "${ROOTFS}$(dirname "$lib")"
      cp -L "$lib" "${ROOTFS}${lib}" 2>/dev/null
    done
    local interp
    interp=$(readelf -l "$src" 2>/dev/null | grep 'interpreter' | grep -o '/[^]]*')
    if [[ -n "$interp" && -f "$interp" ]]; then
      mkdir -p "${ROOTFS}$(dirname "$interp")"
      cp -L "$interp" "${ROOTFS}${interp}" 2>/dev/null
      chmod +x "${ROOTFS}${interp}" 2>/dev/null
    fi
  fi
}

for bin in "${BINS[@]}"; do
  path=$(command -v "$bin" 2>/dev/null) || continue
  copy_binary "$path" "${ROOTFS}/usr/bin"
done

# ─── 3. Setup essential config files ──────────────────────────
echo "[3/5] Setting up configuration files..."

cat > "${ROOTFS}/etc/passwd" << 'EOF'
root:x:0:0:root:/root:/bin/bash
worker:x:1000:1000:Worker User:/home/worker:/bin/bash
nobody:x:65534:65534:nobody:/nonexistent:/usr/sbin/nologin
EOF

cat > "${ROOTFS}/etc/group" << 'EOF'
root:x:0:
worker:x:1000:
nogroup:x:65534:
EOF

cat > "${ROOTFS}/etc/shadow" << 'EOF'
root:*:19000:0:99999:7:::
worker:*:19000:0:99999:7:::
EOF
chmod 640 "${ROOTFS}/etc/shadow"

echo "worker" > "${ROOTFS}/etc/hostname"

cat > "${ROOTFS}/etc/hosts" << 'EOF'
127.0.0.1       localhost worker
::1             localhost ip6-localhost ip6-loopback
EOF

cp -L /etc/resolv.conf "${ROOTFS}/etc/resolv.conf" 2>/dev/null || \
  echo "nameserver 8.8.8.8" > "${ROOTFS}/etc/resolv.conf"

cat > "${ROOTFS}/etc/nsswitch.conf" << 'EOF'
passwd:     files
group:      files
shadow:     files
hosts:      files dns
networks:   files
EOF

if [[ -d /etc/ssl/certs ]]; then
  cp -rL /etc/ssl/certs/* "${ROOTFS}/etc/ssl/certs/" 2>/dev/null || true
fi
cp -L /etc/ssh/ssh_config 2>/dev/null "${ROOTFS}/etc/ssh/ssh_config" || true

cat > "${ROOTFS}/etc/os-release" << 'EOF'
NAME="Worker RootFS"
VERSION="1.0"
ID=worker
PRETTY_NAME="Worker RootFS (Debian-based)"
EOF

# ─── 4. Create device stubs ───────────────────────────────────
echo "[4/5] Creating device stubs..."
for dev in null zero random urandom stdin stdout stderr tty ptmx; do
  : > "${ROOTFS}/dev/${dev}"
done

# ─── 5. Permissions & bashrc ──────────────────────────────────
echo "[5/5] Setting permissions..."
chmod 1777 "${ROOTFS}/tmp" "${ROOTFS}/var/tmp"
chmod 700 "${ROOTFS}/root" "${ROOTFS}/root/.ssh"
chmod 755 "${ROOTFS}/home/worker"
chmod 700 "${ROOTFS}/home/worker/.ssh"

cat > "${ROOTFS}/root/.bashrc" << 'EOF'
export PATH=/usr/bin:/usr/sbin:/bin:/sbin:/usr/local/bin
export HOME=/root
export TERM=xterm-256color
PS1='\[\033[0;31m\]root@worker\[\033[0m\]:\[\033[1;34m\]\w\[\033[0m\]# '
EOF

cat > "${ROOTFS}/home/worker/.bashrc" << 'EOF'
export PATH=/usr/bin:/usr/sbin:/bin:/sbin:/usr/local/bin
export HOME=/home/worker
export TERM=xterm-256color
PS1='\[\033[0;32m\]worker@host\[\033[0m\]:\[\033[1;34m\]\w\[\033[0m\]$ '
EOF

# ─── Summary ──────────────────────────────────────────────────
TOTAL_SIZE=$(du -sh "$ROOTFS" | cut -f1)
BIN_COUNT=$(find "${ROOTFS}/usr/bin" -maxdepth 1 -type f 2>/dev/null | wc -l)
LIB_COUNT=$(find "${ROOTFS}/lib" "${ROOTFS}/lib64" "${ROOTFS}/usr/lib" -type f 2>/dev/null | wc -l)

echo ""
echo "═══════════════════════════════════════════════════"
echo "  ROOTFS READY"
echo "═══════════════════════════════════════════════════"
echo "  Path      : ${ROOTFS}"
echo "  Size      : ${TOTAL_SIZE}"
echo "  Binaries  : ${BIN_COUNT}"
echo "  Libraries : ${LIB_COUNT}"
echo ""
echo "  Usage:"
echo "    sudo chroot ${ROOTFS} /bin/bash"
echo "    sudo mount --bind /proc ${ROOTFS}/proc"
echo "    sudo mount --bind /sys  ${ROOTFS}/sys"
echo "    sudo mount --bind /dev  ${ROOTFS}/dev"
echo "═══════════════════════════════════════════════════"