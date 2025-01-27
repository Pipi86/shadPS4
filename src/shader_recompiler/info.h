// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>
#include "common/assert.h"
#include "common/types.h"
#include "shader_recompiler/ir/attribute.h"
#include "shader_recompiler/ir/reg.h"
#include "shader_recompiler/ir/type.h"
#include "shader_recompiler/params.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/resource.h"

namespace Shader {

static constexpr size_t NumUserDataRegs = 16;

enum class TextureType : u32 {
    Color1D,
    ColorArray1D,
    Color2D,
    ColorArray2D,
    Color3D,
    ColorCube,
    Buffer,
};
constexpr u32 NUM_TEXTURE_TYPES = 7;

struct Info;

struct BufferResource {
    u32 sgpr_base;
    u32 dword_offset;
    IR::Type used_types;
    AmdGpu::Buffer inline_cbuf;
    bool is_gds_buffer{};
    bool is_instance_data{};
    bool is_written{};

    bool IsStorage(AmdGpu::Buffer buffer) const noexcept {
        static constexpr size_t MaxUboSize = 65536;
        return buffer.GetSize() > MaxUboSize || is_written || is_gds_buffer;
    }

    constexpr AmdGpu::Buffer GetSharp(const Info& info) const noexcept;
};
using BufferResourceList = boost::container::small_vector<BufferResource, 16>;

struct TextureBufferResource {
    u32 sgpr_base;
    u32 dword_offset;
    AmdGpu::NumberFormat nfmt;
    bool is_written{};

    constexpr AmdGpu::Buffer GetSharp(const Info& info) const noexcept;
};
using TextureBufferResourceList = boost::container::small_vector<TextureBufferResource, 16>;

struct ImageResource {
    u32 sgpr_base;
    u32 dword_offset;
    AmdGpu::ImageType type;
    AmdGpu::NumberFormat nfmt;
    bool is_storage;
    bool is_depth;
    bool is_atomic{};

    constexpr AmdGpu::Image GetSharp(const Info& info) const noexcept;
};
using ImageResourceList = boost::container::small_vector<ImageResource, 16>;

struct SamplerResource {
    u32 sgpr_base;
    u32 dword_offset;
    AmdGpu::Sampler inline_sampler{};
    u32 associated_image : 4;
    u32 disable_aniso : 1;

    constexpr AmdGpu::Sampler GetSharp(const Info& info) const noexcept;
};
using SamplerResourceList = boost::container::small_vector<SamplerResource, 16>;

struct PushData {
    static constexpr size_t BufOffsetIndex = 2;

    u32 step0;
    u32 step1;
    std::array<u8, 32> buf_offsets;

    void AddOffset(u32 binding, u32 offset) {
        ASSERT(offset < 256 && binding < buf_offsets.size());
        buf_offsets[binding] = offset;
    }
};

/**
 * Contains general information generated by the shader recompiler for an input program.
 */
struct Info {
    struct VsInput {
        enum InstanceIdType : u8 {
            None = 0,
            OverStepRate0 = 1,
            OverStepRate1 = 2,
            Plain = 3,
        };

        AmdGpu::NumberFormat fmt;
        u16 binding;
        u16 num_components;
        u8 sgpr_base;
        u8 dword_offset;
        InstanceIdType instance_step_rate;
        s32 instance_data_buf;
    };
    boost::container::static_vector<VsInput, 32> vs_inputs{};

    struct AttributeFlags {
        bool Get(IR::Attribute attrib, u32 comp = 0) const {
            return flags[Index(attrib)] & (1 << comp);
        }

        bool GetAny(IR::Attribute attrib) const {
            return flags[Index(attrib)];
        }

        void Set(IR::Attribute attrib, u32 comp = 0) {
            flags[Index(attrib)] |= (1 << comp);
        }

        u32 NumComponents(IR::Attribute attrib) const {
            return 4;
        }

        static size_t Index(IR::Attribute attrib) {
            return static_cast<size_t>(attrib);
        }

        std::array<u8, IR::NumAttributes> flags;
    };
    AttributeFlags loads{};
    AttributeFlags stores{};

    s8 vertex_offset_sgpr = -1;
    s8 instance_offset_sgpr = -1;

    BufferResourceList buffers;
    TextureBufferResourceList texture_buffers;
    ImageResourceList images;
    SamplerResourceList samplers;

    std::span<const u32> user_data;
    Stage stage;

    u64 pgm_hash{};
    VAddr pgm_base;
    bool has_storage_images{};
    bool has_image_buffers{};
    bool has_texel_buffers{};
    bool has_discard{};
    bool has_image_gather{};
    bool has_image_query{};
    bool uses_lane_id{};
    bool uses_group_quad{};
    bool uses_shared{};
    bool uses_fp16{};
    bool uses_step_rates{};
    bool translation_failed{}; // indicates that shader has unsupported instructions

    explicit Info(Stage stage_, ShaderParams params)
        : stage{stage_}, pgm_hash{params.hash}, pgm_base{params.Base()},
          user_data{params.user_data} {}

    template <typename T>
    T ReadUd(u32 ptr_index, u32 dword_offset) const noexcept {
        T data;
        const u32* base = user_data.data();
        if (ptr_index != IR::NumScalarRegs) {
            std::memcpy(&base, &user_data[ptr_index], sizeof(base));
        }
        std::memcpy(&data, base + dword_offset, sizeof(T));
        return data;
    }

    size_t NumBindings() const noexcept {
        return buffers.size() + texture_buffers.size() + images.size() + samplers.size();
    }

    [[nodiscard]] std::pair<u32, u32> GetDrawOffsets() const noexcept {
        u32 vertex_offset = 0;
        u32 instance_offset = 0;
        if (vertex_offset_sgpr != -1) {
            vertex_offset = user_data[vertex_offset_sgpr];
        }
        if (instance_offset_sgpr != -1) {
            instance_offset = user_data[instance_offset_sgpr];
        }
        return {vertex_offset, instance_offset};
    }
};

constexpr AmdGpu::Buffer BufferResource::GetSharp(const Info& info) const noexcept {
    return inline_cbuf ? inline_cbuf : info.ReadUd<AmdGpu::Buffer>(sgpr_base, dword_offset);
}

constexpr AmdGpu::Buffer TextureBufferResource::GetSharp(const Info& info) const noexcept {
    return info.ReadUd<AmdGpu::Buffer>(sgpr_base, dword_offset);
}

constexpr AmdGpu::Image ImageResource::GetSharp(const Info& info) const noexcept {
    return info.ReadUd<AmdGpu::Image>(sgpr_base, dword_offset);
}

constexpr AmdGpu::Sampler SamplerResource::GetSharp(const Info& info) const noexcept {
    return inline_sampler ? inline_sampler : info.ReadUd<AmdGpu::Sampler>(sgpr_base, dword_offset);
}

} // namespace Shader

template <>
struct fmt::formatter<Shader::Stage> {
    constexpr auto parse(format_parse_context& ctx) {
        return ctx.begin();
    }
    auto format(const Shader::Stage stage, format_context& ctx) const {
        constexpr static std::array names = {"fs", "vs", "gs", "es", "hs", "ls", "cs"};
        return fmt::format_to(ctx.out(), "{}", names[static_cast<size_t>(stage)]);
    }
};
