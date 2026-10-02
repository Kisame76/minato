/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

// The system property area as init writes it (properties.h). Derived from bionic's
// libc/system_properties/prop_area.cpp at android-15.0.0_r1 (commit 361ba86734fb2821a6adcfdf775db8abd04e0de0):
// Area::add follows prop_area::add, find_property, find_prop_trie_node, new_prop_trie_node, new_prop_info and
// allocate_obj, and prop_info.cpp's two constructors, on the layout of prop_area.h and prop_info.h, writing into a
// buffer instead of a mapping. bionic's ContextsPreSplit reads the area when /dev/__properties__ is no folder: one
// area for every property, without property contexts or a serialized tree.
#include "properties.h"

#include <sys/sysctl.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

namespace apkrt::properties {
namespace {

constexpr std::size_t area_size = 128 * 1024;       // PA_SIZE
constexpr std::uint32_t area_magic = 0x504f5250;    // PROP_AREA_MAGIC
constexpr std::uint32_t area_version = 0xfc6ed0ab;  // PROP_AREA_VERSION
// struct prop_area: bytes_used_, serial_, magic_, version_, reserved_[28], then data_.
constexpr std::size_t header_size = 128;
constexpr std::size_t value_max = 92;  // PROP_VALUE_MAX
// struct prop_trie_node: namelen, then the offsets prop, left, right and children, then the name.
constexpr std::uint32_t node_size = 20, node_prop = 4, node_left = 8, node_right = 12, node_children = 16;
// struct prop_info: serial, the value, then the name. A long value (kLongFlag) lies on its own: the value's place
// holds a message for __system_property_get in its first 56 bytes, then the value's offset from the prop_info.
constexpr std::uint32_t info_size = 4 + value_max, info_value = 4, info_long_offset = info_value + 56;
constexpr std::uint32_t long_flag = 1u << 16;
constexpr std::string_view long_error = "Must use __system_property_read_callback() to read";

// The properties the runtime sets, as init would from the build's property files.
constexpr std::pair<std::string_view, std::string_view> properties[] = {
    // The heap of AOSP's phone-xhdpi-6144-dalvik-heap.mk, its largest phone's (ADR-0039 decision 4).
    {"dalvik.vm.heapstartsize", "16m"},
    {"dalvik.vm.heapgrowthlimit", "256m"},
    {"dalvik.vm.heapsize", "512m"},
    {"dalvik.vm.heaptargetutilization", "0.5"},
    {"dalvik.vm.heapminfree", "8m"},
    {"dalvik.vm.heapmaxfree", "32m"},
    // Android 15's version (ADR-0039 decision 9) [verified: the emulator's getprop, 2026-10-01], as a user build
    // with release keys, which is not debuggable. Build.VERSION.ACTIVE_CODENAMES reads the first of all_codenames.
    {"ro.build.version.sdk", "35"},
    {"ro.build.version.release", "15"},
    {"ro.build.version.release_or_codename", "15"},
    {"ro.build.version.release_or_preview_display", "15"},
    {"ro.build.version.codename", "REL"},
    {"ro.build.version.all_codenames", "REL"},
    {"ro.build.version.known_codenames",
     "Base,Base11,Cupcake,Donut,Eclair,Eclair01,EclairMr1,Froyo,Gingerbread,GingerbreadMr1,Honeycomb,HoneycombMr1,"
     "HoneycombMr2,IceCreamSandwich,IceCreamSandwichMr1,JellyBean,JellyBeanMr1,JellyBeanMr2,Kitkat,KitkatWatch,"
     "Lollipop,LollipopMr1,M,N,NMr1,O,OMr1,P,Q,R,S,Sv2,Tiramisu,UpsideDownCake,VanillaIceCream"},
    {"ro.build.version.min_supported_target_sdk", "28"},
    {"ro.build.version.incremental", "12368160"},
    {"ro.build.version.security_patch", "2024-09-05"},
    {"ro.build.id", "AE3A.240806.019"},
    {"ro.build.type", "user"},
    {"ro.build.tags", "release-keys"},
    {"ro.debuggable", "0"},
    // The device (decision 9): Apple's, with the model as the host names it (area()), not the emulator's; the ABI
    // arm64-v8a alone, whose 64-bit list gives Build.CPU_ABI; the density that DisplayMetrics.DENSITY_DEVICE reads;
    // and GLES 3.0. No ro.boot.qemu, and no ro.build.fingerprint, which Build then derives from these.
    {"ro.product.manufacturer", "Apple"},
    {"ro.product.brand", "Apple"},
    {"ro.hardware", "minato"},
    {"ro.product.cpu.abilist", "arm64-v8a"},
    {"ro.product.cpu.abilist32", ""},
    {"ro.product.cpu.abilist64", "arm64-v8a"},
    {"ro.sf.lcd_density", "480"},
    {"ro.opengles.version", "196608"},
};

/// The host's hw.machine, Apple's name of the device: "iPhone16,2" for the iPhone 15 Pro Max; on the Mac, its
/// processor's. No table turns it into a marketing name (decision 9).
std::string machine() {
    char name[64] = "";
    std::size_t size = sizeof name;
    if (::sysctlbyname("hw.machine", name, &size, nullptr, 0) != 0) return {};
    return name;
}

/// The area as it is written: data_'s objects at offsets from data_, which the trie's links hold.
class Area {
public:
    /// prop_area's constructor: the root node, then the dirty backup area of PROP_VALUE_MAX bytes.
    Area() : bytes_(area_size, '\0') {
        store_header(8, area_magic);
        store_header(12, area_version);
        used_ = node_size + value_max;
    }

    /// prop_area::add: the property `name` with `value`, under the nodes of its dot-separated parts; false when
    /// a part is empty, the area full, or the value too long for a property that is not read-only, as
    /// __system_property_add refuses it. A name already there keeps its value.
    bool add(std::string_view name, std::string_view value) {
        if (value.size() >= value_max && !name.starts_with("ro.")) return false;
        std::uint32_t current = 0;  // the root node
        for (std::string_view rest = name;;) {
            const auto dot = rest.find('.');
            const auto part = rest.substr(0, dot);
            if (part.empty()) return false;
            std::optional<std::uint32_t> next;
            if (load(current + node_children) == 0) {
                next = new_node(part);
                if (next) store(current + node_children, *next);
            } else {
                next = node_in(load(current + node_children), part);
            }
            if (!next) return false;
            current = *next;
            if (dot == std::string_view::npos) break;
            rest = rest.substr(dot + 1);
        }
        if (load(current + node_prop) != 0) return true;
        const auto info = new_info(name, value);
        if (!info) return false;
        store(current + node_prop, *info);
        return true;
    }

    /// The area's bytes, with bytes_used_ in its header.
    std::string bytes() && {
        store_header(0, used_);
        return std::move(bytes_);
    }

private:
    std::string bytes_;
    std::uint32_t used_ = 0;

    char *data() { return bytes_.data() + header_size; }

    std::uint32_t load(std::uint32_t offset) {
        std::uint32_t value;
        std::memcpy(&value, data() + offset, sizeof value);
        return value;
    }
    void store(std::uint32_t offset, std::uint32_t value) { std::memcpy(data() + offset, &value, sizeof value); }
    void store_header(std::size_t offset, std::uint32_t value) {
        std::memcpy(bytes_.data() + offset, &value, sizeof value);
    }

    /// allocate_obj: `size` bytes, aligned to 4, after what is used; nothing when the area is full.
    std::optional<std::uint32_t> allocate(std::size_t size) {
        const auto aligned = static_cast<std::uint32_t>((size + 3) & ~std::size_t{3});
        if (used_ + aligned > area_size - header_size) return std::nullopt;
        const auto offset = used_;
        used_ += aligned;
        return offset;
    }

    /// new_prop_trie_node: a node named `name`, its name NUL-terminated.
    std::optional<std::uint32_t> new_node(std::string_view name) {
        const auto offset = allocate(node_size + name.size() + 1);
        if (!offset) return offset;
        store(*offset, static_cast<std::uint32_t>(name.size()));
        std::memcpy(data() + *offset + node_size, name.data(), name.size());
        return offset;
    }

    /// new_prop_info: a short value in the prop_info, its length in the serial's top byte; a value of
    /// PROP_VALUE_MAX bytes or more after it, with kLongFlag and the message for get, as prop_info's long form.
    std::optional<std::uint32_t> new_info(std::string_view name, std::string_view value) {
        const auto offset = allocate(info_size + name.size() + 1);
        if (!offset) return offset;
        if (value.size() >= value_max) {
            const auto long_value = allocate(value.size() + 1);
            if (!long_value) return long_value;
            std::memcpy(data() + *long_value, value.data(), value.size());
            store(*offset, static_cast<std::uint32_t>(long_error.size()) << 24 | long_flag);
            std::memcpy(data() + *offset + info_value, long_error.data(), long_error.size());
            store(*offset + info_long_offset, *long_value - *offset);
        } else {
            store(*offset, static_cast<std::uint32_t>(value.size()) << 24);
            std::memcpy(data() + *offset + info_value, value.data(), value.size());
        }
        std::memcpy(data() + *offset + info_size, name.data(), name.size());
        return offset;
    }

    /// find_prop_trie_node with alloc_if_needed: `name`'s node in the binary tree of one level, ordered by
    /// length first (cmp_prop_name), added where the search ends.
    std::optional<std::uint32_t> node_in(std::uint32_t current, std::string_view name) {
        while (true) {
            const std::uint32_t length = load(current);
            const std::string_view node_name(data() + current + node_size, length);
            const int order = name.size() != length ? (name.size() < length ? -1 : 1) : name.compare(node_name);
            if (order == 0) return current;
            const std::uint32_t link = current + (order < 0 ? node_left : node_right);
            if (load(link) == 0) {
                const auto added = new_node(name);
                if (added) store(link, *added);
                return added;
            }
            current = load(link);
        }
    }
};

}  // namespace

std::string area() {
    Area written;
    for (const auto &[name, value] : properties) written.add(name, value);
    const std::string model = machine();
    written.add("ro.product.model", model);
    written.add("ro.product.device", model);
    return std::move(written).bytes();
}

}  // namespace apkrt::properties
