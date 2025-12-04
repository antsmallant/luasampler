#!/bin/bash

curdir=$(pwd)
echo $curdir

# 1) 安装依赖（Ubuntu）
sudo apt-get update
sudo apt-get install -y graphviz

# 2) 安装 pprof 到项目 tools 目录
GOBIN=$curdir go install github.com/google/pprof@latest

# 3) 验证
./pprof -version