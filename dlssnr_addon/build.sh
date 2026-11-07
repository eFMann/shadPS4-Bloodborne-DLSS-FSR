#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Robert Bopko
# SPDX-License-Identifier: MIT
#
# Builds dlssnr-amd/shadps4_nr.dll, the optional DLSS 5 neural rendering add-on, with mingw-w64
# from a DLSSNR-AMD tree (gitlab.com/mochizuki0323/DLSSNR-AMD, MIT) at tag v0.0.8.2. The model's
# objects are compiled exactly as that tree's windows/build/build_asi.sh compiles them (same flags,
# same network constants as the shaders its package installs); only the plugin's own units are left
# out and shadps4_nr.cpp takes their place. Bash, not sh: it sources that tree's bash scripts.
set -euo pipefail

usage() {
    echo "usage: build.sh <DLSSNR-AMD tree at v0.0.8.2> <out dir>" >&2
    echo "  NR_MINGW / NR_MINGW_CC / NR_OBJDUMP override the x86_64-w64-mingw32 posix toolchain" >&2
    exit 2
}
log() { printf '%s\n' "$*"; }
die() { printf 'build.sh: %s\n' "$*" >&2; exit 1; }
need_cmd() { command -v "$1" >/dev/null 2>&1 || die "Missing required command: $1"; }

[ "$#" -eq 2 ] || usage
case "$1" in -h|--help) usage;; esac
addon=$(cd -- "$(dirname -- "$0")" && pwd)
src=$(cd -- "$1" && pwd) || die "No such directory: $1"
mkdir -p -- "$2"
out=$(cd -- "$2" && pwd)

cxx=${NR_MINGW:-x86_64-w64-mingw32-g++-posix}
export NR_MINGW_CC=${NR_MINGW_CC:-x86_64-w64-mingw32-gcc-posix}
cc=$NR_MINGW_CC
objdump=${NR_OBJDUMP:-x86_64-w64-mingw32-objdump}
for cmd in "$cxx" "$cc" "$objdump" python3 git curl; do need_cmd "$cmd"; done

cd -- "$src"
[ -f windows/src/pe/nr_dlssnr_model.cpp ] || die "Not a DLSSNR-AMD tree: $src"
bash fetch_deps.sh >/dev/null
minhook=artifacts/ref/DLSS5-Feeder/external/minhook
imgui=toolchain/imgui-nr
glslang=toolchain/glslang/bin/glslang

stamp=$(bash windows/build/version.sh)
python3 windows/build/generate_device_chain.py toolchain/Vulkan-Headers "$out/nr_device_chain_sizes.hpp" >/dev/null
common=(-std=c++17 -O2 -DNDEBUG -DNR_BUILD_STAMP="\"$stamp\"" -DNR_INT4=1 -Itoolchain/Vulkan-Headers/include
        -Iwindows/src -Iwindows/src/core -Iwindows/src/pe -I"$out")
source windows/build/arch/rdna4.sh

log "compiling DLSSNR-AMD $stamp"
"$cxx" "${common[@]}" "${NR_PRODUCT_DEFINES[@]}" -c windows/src/core/nr_runtime.cpp -o "$out/nr_runtime.o"
for unit in core/nr_native_plan core/nr_renodx core/nr_wilsjo2 pe/nr_pe_log pe/nr_pe_crash pe/nr_pe_interop \
            pe/nr_vram_ledger pe/nr_pe_session pe/nr_pe_bridge pe/nr_pe_d3d12split pe/nr_pe_config \
            pe/nr_dlssnr_model pe/nr_pe_renodx_route pe/nr_screenshot; do
    "$cxx" "${common[@]}" -c "windows/src/$unit.cpp" -o "$out/$(basename "$unit").o"
done
for unit in nr_pe_vkdevice nr_pe_control; do
    "$cxx" "${common[@]}" -I"$minhook/include" -c "windows/src/pe/$unit.cpp" -o "$out/$unit.o"
done

# NR Control's unit is linked because the model reports to it; it is never opened here.
python3 windows/build/embed_font.py toolchain/fonts/inter-4.1/Inter-Regular.ttf "$out/nr_menu_font.hpp" kMenuFont
"$glslang" -V --target-env vulkan1.0 --vn kMenuComposeVert windows/shaders/control/menu_compose.vert \
    -o "$out/menu_compose_vert.h" >/dev/null
"$glslang" -V --target-env vulkan1.0 --vn kMenuComposeFrag windows/shaders/control/menu_compose.frag \
    -o "$out/menu_compose_frag.h" >/dev/null
"$cxx" "${common[@]}" -I"$minhook/include" -I"$imgui" -Iwindows/src/ui -c windows/src/pe/nr_pe_menu.cpp \
    -o "$out/nr_pe_menu.o"
for unit in imgui imgui_draw imgui_tables imgui_widgets imgui_impl_dx11 imgui_impl_vulkan; do
    "$cxx" -std=c++17 -O2 -DNDEBUG -Itoolchain/Vulkan-Headers/include -I"$imgui" -c "$imgui/$unit.cpp" \
        -o "$out/im_$unit.o"
done
"$cxx" -std=c++17 -O2 -DNDEBUG -I"$imgui" -include windows/src/pe/mingw_d3d12_compat.h \
    -c "$imgui/imgui_impl_dx12.cpp" -o "$out/im_imgui_impl_dx12.o"
for unit in hook buffer trampoline hde/hde64; do
    "$cc" -O2 -I"$minhook/include" -I"$minhook/src" -c "$minhook/src/$unit.c" -o "$out/mh_$(basename "$unit").o"
done

"$cxx" "${common[@]}" -DSHADPS4_NR_BUILD -I"$addon" -c "$addon/shadps4_nr.cpp" -o "$out/shadps4_nr.o"

rm -f -- "$out"/vulkan_lazy_*.o
objs=("$out"/*.o)
ldflags=(-ld3d12 -ld3d11 -ldxgi -lole32 -lversion -static -static-libgcc -static-libstdc++)
bash windows/build/vulkan_lazy.sh "$out" "$cxx" -shared "${objs[@]}" "${ldflags[@]}"
"$cxx" -shared -o "$out/shadps4_nr.dll" "${objs[@]}" "$out/vulkan_lazy_s.o" "$out/vulkan_lazy_c.o" "${ldflags[@]}"

log "built $out/shadps4_nr.dll ($(stat -c %s "$out/shadps4_nr.dll") bytes), exports:"
"$objdump" -p "$out/shadps4_nr.dll" | grep -o '\bshadps4_nr_[a-z_]*' | sort -u | sed 's/^/    /'
