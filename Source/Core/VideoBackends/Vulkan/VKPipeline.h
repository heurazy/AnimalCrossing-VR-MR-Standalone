// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <utility>

#include "VideoBackends/Vulkan/VulkanLoader.h"
#include "VideoCommon/AbstractPipeline.h"

namespace Vulkan
{
class VKPipeline final : public AbstractPipeline
{
public:
  explicit VKPipeline(const AbstractPipelineConfig& config, VkPipeline pipeline,
                      VkPipelineLayout pipeline_layout, AbstractPipelineUsage usage);
  ~VKPipeline() override;

  VkPipeline GetVkPipeline() const { return m_pipeline; }

  // Relinquishes ownership of the handle so the caller can hand it to deferred destruction;
  // the destructor then destroys VK_NULL_HANDLE, which is a no-op.
  VkPipeline Release() { return std::exchange(m_pipeline, VK_NULL_HANDLE); }
  VkPipelineLayout GetVkPipelineLayout() const { return m_pipeline_layout; }
  AbstractPipelineUsage GetUsage() const { return m_usage; }
  static std::unique_ptr<VKPipeline> Create(const AbstractPipelineConfig& config);

private:
  VkPipeline m_pipeline;
  VkPipelineLayout m_pipeline_layout;
  AbstractPipelineUsage m_usage;
};

}  // namespace Vulkan
