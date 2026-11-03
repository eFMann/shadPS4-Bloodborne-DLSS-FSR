// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/static_vector.hpp>

#include <boost/container/small_vector.hpp>
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include <cstring>
#include <vector>

#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;

namespace {
/// A copy of descriptor writes and the infos they point to, for deferred recording.
struct PushedWrites {
    boost::container::small_vector<vk::WriteDescriptorSet, 24> writes;
    boost::container::small_vector<vk::DescriptorImageInfo, 24> images;
    boost::container::small_vector<vk::DescriptorBufferInfo, 16> buffers;
    boost::container::small_vector<vk::BufferView, 4> texel_views;

    explicit PushedWrites(const Pipeline::DescriptorWrites& source) {
        writes.assign(source.begin(), source.end());
        for (const auto& write : source) {
            if (write.pImageInfo) {
                images.insert(images.end(), write.pImageInfo,
                              write.pImageInfo + write.descriptorCount);
            }
            if (write.pBufferInfo) {
                buffers.insert(buffers.end(), write.pBufferInfo,
                               write.pBufferInfo + write.descriptorCount);
            }
            if (write.pTexelBufferView) {
                texel_views.insert(texel_views.end(), write.pTexelBufferView,
                                   write.pTexelBufferView + write.descriptorCount);
            }
        }
    }

    /// Points the writes at this copy's infos (after it reached its final address).
    void Fixup() {
        u32 image = 0, buffer = 0, texel = 0;
        for (auto& write : writes) {
            if (write.pImageInfo) {
                write.pImageInfo = images.data() + image;
                image += write.descriptorCount;
            }
            if (write.pBufferInfo) {
                write.pBufferInfo = buffers.data() + buffer;
                buffer += write.descriptorCount;
            }
            if (write.pTexelBufferView) {
                write.pTexelBufferView = texel_views.data() + texel;
                texel += write.descriptorCount;
            }
        }
    }
};
/// Push descriptor writes as one recorded command's data: each write's binding, type and count,
/// then the infos (the WriteDescriptorSets are rebuilt when recorded). A few hundred bytes a draw
/// instead of a fixed-capacity copy of the writes (~2.6 KB).
struct CompactWrite {
    u32 binding;
    u32 array_element;
    u32 count;
    vk::DescriptorType type;
    u32 kind; ///< 0 image infos, 1 buffer infos, 2 texel buffer views
    u32 pad;
};
struct CompactWritesHeader {
    u32 num_writes;
    u32 num_images;
    u32 num_buffers;
    u32 num_texels;
};

/// Encodes `source` (false: a write this format cannot hold).
bool EncodeWrites(const Pipeline::DescriptorWrites& source, std::vector<u8>& out) {
    CompactWritesHeader header{u32(source.size()), 0, 0, 0};
    for (const auto& write : source) {
        const int kinds = (write.pImageInfo ? 1 : 0) + (write.pBufferInfo ? 1 : 0) +
                          (write.pTexelBufferView ? 1 : 0);
        if (write.pNext || kinds != 1) {
            return false;
        }
        (write.pImageInfo ? header.num_images
                          : write.pBufferInfo ? header.num_buffers : header.num_texels) +=
            write.descriptorCount;
    }
    out.resize(sizeof(header) + header.num_writes * sizeof(CompactWrite) +
               header.num_images * sizeof(vk::DescriptorImageInfo) +
               header.num_buffers * sizeof(vk::DescriptorBufferInfo) +
               header.num_texels * sizeof(vk::BufferView));
    u8* at = out.data();
    std::memcpy(at, &header, sizeof(header));
    at += sizeof(header);
    u8* images = at + header.num_writes * sizeof(CompactWrite);
    u8* buffers = images + header.num_images * sizeof(vk::DescriptorImageInfo);
    u8* texels = buffers + header.num_buffers * sizeof(vk::DescriptorBufferInfo);
    for (const auto& write : source) {
        const CompactWrite compact{write.dstBinding, write.dstArrayElement, write.descriptorCount,
                                   write.descriptorType,
                                   write.pImageInfo ? 0u : write.pBufferInfo ? 1u : 2u, 0};
        std::memcpy(at, &compact, sizeof(compact));
        at += sizeof(compact);
        if (write.pImageInfo) {
            const size_t bytes = write.descriptorCount * sizeof(vk::DescriptorImageInfo);
            std::memcpy(images, write.pImageInfo, bytes);
            images += bytes;
        } else if (write.pBufferInfo) {
            const size_t bytes = write.descriptorCount * sizeof(vk::DescriptorBufferInfo);
            std::memcpy(buffers, write.pBufferInfo, bytes);
            buffers += bytes;
        } else {
            const size_t bytes = write.descriptorCount * sizeof(vk::BufferView);
            std::memcpy(texels, write.pTexelBufferView, bytes);
            texels += bytes;
        }
    }
    return true;
}

/// Records the writes EncodeWrites stored.
void PushCompactWrites(vk::CommandBuffer cmdbuf, vk::PipelineBindPoint bind_point,
                       vk::PipelineLayout layout, const u8* at) {
    CompactWritesHeader header;
    std::memcpy(&header, at, sizeof(header));
    at += sizeof(header);
    const auto* compact = reinterpret_cast<const CompactWrite*>(at);
    const auto* images = reinterpret_cast<const vk::DescriptorImageInfo*>(
        at + header.num_writes * sizeof(CompactWrite));
    const auto* buffers = reinterpret_cast<const vk::DescriptorBufferInfo*>(
        reinterpret_cast<const u8*>(images) + header.num_images * sizeof(vk::DescriptorImageInfo));
    const auto* texels = reinterpret_cast<const vk::BufferView*>(
        reinterpret_cast<const u8*>(buffers) +
        header.num_buffers * sizeof(vk::DescriptorBufferInfo));
    boost::container::small_vector<vk::WriteDescriptorSet, 32> writes(header.num_writes);
    for (u32 i = 0; i < header.num_writes; ++i) {
        const auto& c = compact[i];
        auto& write = writes[i];
        write.dstBinding = c.binding;
        write.dstArrayElement = c.array_element;
        write.descriptorCount = c.count;
        write.descriptorType = c.type;
        if (c.kind == 0) {
            write.pImageInfo = images;
            images += c.count;
        } else if (c.kind == 1) {
            write.pBufferInfo = buffers;
            buffers += c.count;
        } else {
            write.pTexelBufferView = texels;
            texels += c.count;
        }
    }
    cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, writes);
}
} // Anonymous namespace

void Pipeline::BindResources(DescriptorWrites& set_writes,
                             const Shader::PushData& push_data) const {
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    const auto layout = *pipeline_layout;

    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    scheduler.Record([layout, stage_flags, push_data](vk::CommandBuffer cmdbuf) {
        cmdbuf.pushConstants(layout, stage_flags, 0u, sizeof(push_data), &push_data);
    });

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    if (uses_push_descriptors) {
        thread_local std::vector<u8> encoded;
        if (EncodeWrites(set_writes, encoded)) {
            scheduler.RecordData(
                [bind_point, layout](vk::CommandBuffer cmdbuf, const u8* data) {
                    PushCompactWrites(cmdbuf, bind_point, layout, data);
                },
                encoded.data(), u32(encoded.size()));
            return;
        }
        scheduler.Record([bind_point, layout,
                          pushed = PushedWrites{set_writes}](vk::CommandBuffer cmdbuf) mutable {
            pushed.Fixup();
            cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, pushed.writes);
        });
        return;
    }

    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    scheduler.Record([bind_point, layout, desc_set](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindDescriptorSets(bind_point, layout, 0, desc_set, {});
    });
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
