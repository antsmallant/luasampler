#!/bin/bash

curdir=$(pwd)

git submodule update --init

cd 3rd/lua-5.4.8
make linux
make local

cd $curdir
make clean
make linux





