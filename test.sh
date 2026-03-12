#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
conda run -n websockets_working python -m unittest discover -s tests -v