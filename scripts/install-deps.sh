#!/usr/bin/env bash
set -euo pipefail

echo "Installing system dependencies..."
if [[ "$(uname -s)" == "Darwin" ]]; then
  # macOS
  if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew missing. Please install Homebrew first: https://brew.sh/"
    exit 1
  fi
  brew update
  brew install boost cmake quickfix
elif [[ -f "/etc/debian_version" ]]; then
  # Debian / Ubuntu
  sudo apt-get update
  sudo apt-get install -y build-essential cmake libboost-all-dev libquickfix-dev
else
  echo "Unsupported OS. Please install dependencies manually (Boost, CMake, QuickFIX)."
fi

echo "Dependencies installed."
