#!/bin/bash
# Rebuild the CoreELEC kernel the way projects/Amlogic-ce/packages/linux/package.mk does, to get
# Module.symvers + a tree to build media_modules against (called by build.sh). ~10 min.
# Recipe checked against CoreELEC 22.0-Piers_rc1; build.sh verifies the result against the
# box's own modules, so a recipe drift in a later release shows up there, not on the box.
set -euo pipefail
. "$ENV"
L=$B/linux
if [ ! -f "$L/.cube-prepared" ]; then
    rm -rf "$L" "$B/.unpack" && mkdir -p "$B/.unpack"
    tar -xzf "$WORK/dl/linux-$LINUX_COMMIT.tar.gz" -C "$B/.unpack" && mv "$B/.unpack"/linux-amlogic-* "$L"
    tar -xzf "$WORK/dl/common_drivers-$COMMON_DRIVERS_COMMIT.tar.gz" -C "$B/.unpack"
    mkdir -p "$L/common_drivers" && cp -r "$B/.unpack"/common_drivers-*/* "$L/common_drivers/"
    rm -rf "$B/.unpack"
    touch "$L/.scmversion"; echo "$COMMON_DRIVERS_COMMIT" > "$L/common_drivers/.scmversion"
    cd "$L"
    cp "$WORK/dl/linux.aarch64-$CE_COMMIT.conf" .config
    sed -i -e "s|@DISTRONAME@|CoreELEC|g" .config
    sed -e "s|^KBUILD_CFLAGS += \$(call cc-option,-Wimplicit-fallthrough,).*||" \
        -e "s|^KBUILD_CFLAGS   := \(.*\)|KBUILD_CFLAGS   := -Wno-format -Wno-unused-function -Wno-misleading-indentation \1|" \
        -e "s|^KBUILD_LDFLAGS :=|KBUILD_LDFLAGS := \$(call ld-option,--no-warn-rwx-segments)|" -i Makefile
    sed -i "s|-z norelro||" arch/arm64/Makefile
    # CoreELEC generates this from its bl30 package; it only feeds the bootloader GPIO driver
    printf "typedef struct bl30_gpio { char *name; uint32_t number; } bl30_gpio_t;\ntypedef struct bl30_gpios_soc { enum meson_cpuid_type_e cpuid; bl30_gpio_t gpio[256]; } bl30_gpios_soc_t;\nbl30_gpios_soc_t bl30_gpios[] = {\n};\n" > common_drivers/drivers/bootloader/gpio_data.h
    touch .cube-prepared
fi
cd "$L"
export PATH=$SRC/clang-$CLANG_VERSION/bin:$PATH
K="make ARCH=arm64 LLVM=1 CC=clang CROSS_COMPILE=aarch64-linux-gnu- -j$JOBS"
$K olddefconfig
$K Image modules
ls -la Module.symvers
echo KERNEL_BUILD_DONE
