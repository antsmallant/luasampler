# skynet snlua profile lib

[参见](https://github.com/lsg2020/skynet/commit/4ace42e80814abfff6b8e64335061a206c674f96)


# build

```
build.sh
```


# run example

```
./build/lua-5.4.8/install/bin/lua example.lua
```

# read result

## json
view json in a better way using https://jsonstudio.io/view/json-grid-viewer or https://jsongrid.com/json-grid .

## flame 

1. pprof tools

install: 
```
sudo apt update && sudo apt install -y git perl
git clone https://github.com/brendangregg/FlameGraph.git ~/FlameGraph
chmod +x ~/FlameGraph/flamegraph.pl
```

generate svg:
```
~/FlameGraph/flamegraph.pl /tmp/folded.txt > /tmp/flame.svg && xdg-open /tmp/flame.svg
```

doc:
https://github.com/google/pprof/blob/main/doc/README.md#pprof     

2. website

https://www.speedscope.app/
