#!/usr/bin/env bash
# Register Apple Silicon Homebrew OpenJDK with macOS so /usr/bin/java and
# java_home see an arm64 VM. Requires sudo once.
#
# Intel Homebrew OpenJDK under /usr/local/Cellar cannot be uninstalled via
# `brew uninstall` when Intel brew fails on Apple Silicon; remove that tree
# manually if needed: rm -rf /usr/local/Cellar/openjdk && rm -f /usr/local/opt/openjdk
#
# Usage: ./tools/register-arm-openjdk-macos.sh
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

if ! command -v brew >/dev/null 2>&1; then
  echo "Error: brew not in PATH (use Apple Silicon Homebrew, e.g. /opt/homebrew/bin)" >&2
  exit 1
fi

JAVA_DK="$(brew --prefix openjdk)/libexec/openjdk.jdk"
if [[ ! -d "$JAVA_DK" ]]; then
  echo "Error: missing $JAVA_DK — run: brew install openjdk" >&2
  exit 1
fi

echo "Source JDK bundle: $JAVA_DK"
file "$JAVA_DK/Contents/Home/bin/java"

echo "Creating /Library/Java/JavaVirtualMachines/openjdk.jdk (sudo required)..."
sudo mkdir -p /Library/Java/JavaVirtualMachines
sudo ln -sfn "$JAVA_DK" /Library/Java/JavaVirtualMachines/openjdk.jdk

echo ""
echo "Registered Java VMs:"
/usr/libexec/java_home -V 2>&1 || true
echo ""
echo "Default (java_home): $(/usr/libexec/java_home 2>/dev/null || echo 'none')"
echo "Tip: export JAVA_HOME=\"\$(/usr/libexec/java_home -a arm64)\""
