#!/usr/bin/env bash
set -euo pipefail

make
make run
make run CORES=4 MEM=16G
