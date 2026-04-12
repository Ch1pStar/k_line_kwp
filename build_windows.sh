#!/usr/bin/env bash

docker exec 85 bash -c "cd /opt/projects/side-projects/k_line_kwp/ && ./build.sh"
cp ./build_linux/k_line_kwp.uf2 /d/