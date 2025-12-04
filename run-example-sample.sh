#!/bin/bash

# If invoked by /bin/sh, re-exec with bash to support 'pipefail'
if [ -z "${BASH_VERSION:-}" ]; then exec /bin/bash "$0" "$@"; fi

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
LUA_BIN="$ROOT/3rd/lua-5.4.8/install/bin/lua"
FLAME="$HOME/software/FlameGraph/flamegraph.pl"

rm -f cpu-c-samples.txt cpu-c-samples.raw cpu-c-samples.offline.txt cpu-c-samples-c.svg

"$LUA_BIN" example_sample.lua

# Lua 栈火焰图
echo "$FLAME cpu-samples.txt > cpu-samples.svg"
"$FLAME" cpu-samples.txt > cpu-samples.svg

# C 栈离线符号化（addr2line），生成 cpu-c-samples.offline.txt（folded）与 svg
if [[ -f cpu-c-samples.raw ]]; then
	declare -A MODMAP

	# 建立模块名到绝对路径映射
	MODMAP["lua"]="$LUA_BIN"
	if [[ -f "$ROOT/luaprofilec.so" ]]; then
		MODMAP["luaprofilec.so"]="$ROOT/luaprofilec.so"
	fi
	# 从 ldd 解析共享库路径
	while read -r line; do
		# 形如:  libc.so.6 => /usr/lib/x86_64-linux-gnu/libc.so.6 (0x...)
		if [[ "$line" == *"=>"* ]]; then
			name="$(echo "$line" | awk '{print $1}')"
			path="$(echo "$line" | awk '{print $3}')"
			base="$(basename "$path" 2>/dev/null || true)"
			if [[ -n "${base:-}" && -n "${path:-}" && -r "$path" ]]; then
				MODMAP["$base"]="$path"
			fi
		fi
	done < <(ldd "$LUA_BIN")

	# 符号化一条样本（root->leaf 的 module!0xoff;...）为函数名折叠行
	symbolize_line() {
		local line="$1"
		local IFS=';'
		read -r -a frames <<< "$line"
		local out=()
		for tok in "${frames[@]}"; do
			local mod="${tok%%!*}"
			local off="${tok##*!}"
			local bin="${MODMAP[$mod]:-}"
			local fn=""
			if [[ -n "$bin" && -r "$bin" ]]; then
				# 取函数名（第一行），失败则回退到 module+0xoff
				fn="$(addr2line -Cf -e "$bin" "$off" 2>/dev/null | head -n1 || true)"
				if [[ -z "$fn" || "$fn" == "??" ]]; then
					fn="${mod}+${off}"
				fi
			else
				fn="${mod}+${off}"
			fi
			out+=("$fn")
		done
		(IFS=';'; echo "${out[*]}")
	}

	# 逐行符号化并聚合计数
	tmp="$(mktemp)"
	while IFS= read -r l; do
		[[ -z "$l" ]] && continue
		symbolize_line "$l"
	done < cpu-c-samples.raw | sort | uniq -c | awk '{c=$1; $1=""; sub(/^ /,""); print $0" "c}' > "$tmp"
	mv "$tmp" cpu-c-samples.offline.txt

	# 生成 C 栈火焰图（离线符号化结果）
	"$FLAME" cpu-c-samples.offline.txt > cpu-c-samples-c.svg
fi

# pprof（legacy）生成 C 栈火焰图（若可用）
if [[ -f cpu-c-profile.pprof ]]; then
    echo "pprof -svg $LUA_BIN cpu-c-profile.pprof > cpu-c-samples-pprof.svg"
    ./tools/pprof -svg "$LUA_BIN" cpu-c-profile.pprof > cpu-c-samples-pprof.svg
fi