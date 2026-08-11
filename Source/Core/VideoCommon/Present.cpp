// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/Present.h"

#include "Common/ChunkFile.h"
#include "Common/VR/OpenXRInputState.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/CoreTiming.h"
#include "Core/HW/VideoInterface.h"
#include "Core/Host.h"
#include "Core/System.h"

#include "InputCommon/ControllerInterface/ControllerInterface.h"

#include "Present.h"
#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/FrameDumper.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/FramebufferShaderGen.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/OnScreenUI.h"
#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"
#include "VideoCommon/Widescreen.h"

#ifdef ENABLE_VR
#include <chrono>
#include <limits>
#include <vector>
#include "Common/Logging/Log.h"
#include "Common/Timer.h"
#include "VideoCommon/VR/OpenXRManager.h"
#endif

std::unique_ptr<VideoCommon::Presenter> g_presenter;

namespace VideoCommon
{
#ifdef ENABLE_VR
namespace
{
using HandVec3 = std::array<float, 3>;

struct HandRoomVertex
{
  HandVec3 position{};
  u32 color = 0xFFFFFFFF;
};

struct TabletopHandVertex
{
  float position[4]{};
  float right_position[4]{};
  u32 color = 0xFFFFFFFF;
};

std::string GenerateTabletopHandVertexShader(bool multiview)
{
  std::string code;
  if (multiview)
    code += "#extension GL_EXT_multiview : require\n";
  code += "ATTRIBUTE_LOCATION(0) in float4 rawpos;\n";
  code += "ATTRIBUTE_LOCATION(8) in float4 rawpos_right;\n";
  code += "ATTRIBUTE_LOCATION(5) in float4 rawcolor0;\n";
  if (g_backend_info.bSupportsGeometryShaders)
    code += "VARYING_LOCATION(0) out VertexData { float4 v_col0; };\n";
  else
    code += "VARYING_LOCATION(0) out float4 v_col0;\n";
  code += "#define opos gl_Position\n";
  code += "void main()\n{\n";
  code += "  v_col0 = rawcolor0;\n";
  code += multiview ? "  opos = (gl_ViewIndex == 0u) ? rawpos : rawpos_right;\n" :
                      "  opos = rawpos;\n";
  if (g_backend_info.api_type == APIType::Vulkan || g_backend_info.api_type == APIType::OpenGL)
    code += "  opos.y = -opos.y;\n";
  code += "}\n";
  return code;
}

HandVec3 HandAdd(const HandVec3& a, const HandVec3& b)
{
  return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

HandVec3 HandSub(const HandVec3& a, const HandVec3& b)
{
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

HandVec3 HandMul(const HandVec3& v, float s)
{
  return {v[0] * s, v[1] * s, v[2] * s};
}

float HandDot(const HandVec3& a, const HandVec3& b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

HandVec3 HandCross(const HandVec3& a, const HandVec3& b)
{
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
          a[0] * b[1] - a[1] * b[0]};
}

HandVec3 HandNormalize(HandVec3 v)
{
  const float len = std::sqrt(HandDot(v, v));
  if (len > 1.0e-6f)
    return HandMul(v, 1.0f / len);
  return {0.0f, 0.0f, -1.0f};
}

HandVec3 RotateHandVector(const std::array<float, 4>& q, const HandVec3& v)
{
  const HandVec3 qv{q[0], q[1], q[2]};
  const HandVec3 t = HandMul(HandCross(qv, v), 2.0f);
  return HandAdd(v, HandAdd(HandMul(t, q[3]), HandCross(qv, t)));
}

void AppendHandTriangle(std::vector<HandRoomVertex>* vertices, const HandVec3& a,
                        const HandVec3& b, const HandVec3& c, u32 color)
{
  vertices->push_back({a, color});
  vertices->push_back({b, color});
  vertices->push_back({c, color});
}

// Clip one hand triangle exactly against the physical tabletop surface. The previous approach
// discarded whole triangles when two vertices crossed the plane, which made the Meta hand mesh
// visibly pop/jag at the board edge. This half-space clip keeps the portion above the board and
// creates intersection vertices right on the surface. Outside the finite board footprint the hand
// remains fully visible, so reaching around the sides still works naturally.
void AppendTabletopClippedHandTriangle(std::vector<HandRoomVertex>* vertices,
                                       const std::array<HandRoomVertex, 3>& triangle,
                                       const VR::TabletopOcclusionPlane& plane)
{
  const HandVec3 centroid =
      HandMul(HandAdd(HandAdd(triangle[0].position, triangle[1].position), triangle[2].position),
              1.0f / 3.0f);
  const HandVec3 centroid_delta = HandSub(centroid, plane.center);
  const float local_x = HandDot(centroid_delta, plane.axis_x);
  const float local_z = HandDot(centroid_delta, plane.axis_z);
  constexpr float EDGE_MARGIN_M = 0.015f;
  if (std::abs(local_x) > plane.half_extent_x_m + EDGE_MARGIN_M ||
      std::abs(local_z) > plane.half_extent_z_m + EDGE_MARGIN_M)
  {
    AppendHandTriangle(vertices, triangle[0].position, triangle[1].position,
                       triangle[2].position, triangle[0].color);
    return;
  }

  // Keep the surface slightly below the rendered board to prevent tracking noise from making a
  // fingertip flicker when it is resting exactly on top.
  constexpr float SURFACE_EPSILON_M = 0.0015f;
  const auto distance_to_surface = [&](const HandRoomVertex& vertex) {
    return HandDot(HandSub(vertex.position, plane.center), plane.normal) + SURFACE_EPSILON_M;
  };
  const auto interpolate = [](const HandRoomVertex& a, const HandRoomVertex& b, float t) {
    return HandRoomVertex{HandAdd(a.position, HandMul(HandSub(b.position, a.position), t)),
                          a.color};
  };

  std::array<HandRoomVertex, 4> clipped{};
  size_t clipped_count = 0;
  for (size_t edge = 0; edge < 3; ++edge)
  {
    const HandRoomVertex& a = triangle[edge];
    const HandRoomVertex& b = triangle[(edge + 1) % 3];
    const float da = distance_to_surface(a);
    const float db = distance_to_surface(b);
    const bool a_visible = da >= 0.0f;
    const bool b_visible = db >= 0.0f;

    if (a_visible && clipped_count < clipped.size())
      clipped[clipped_count++] = a;

    if (a_visible != b_visible && clipped_count < clipped.size())
    {
      const float denominator = da - db;
      const float t = std::abs(denominator) > 1.0e-7f ? std::clamp(da / denominator, 0.0f, 1.0f) :
                                                        0.0f;
      clipped[clipped_count++] = interpolate(a, b, t);
    }
  }

  if (clipped_count < 3)
    return;

  // A clipped triangle is either a triangle or a quad. Triangulate the quad as a fan.
  for (size_t i = 1; i + 1 < clipped_count; ++i)
  {
    AppendHandTriangle(vertices, clipped[0].position, clipped[i].position,
                       clipped[i + 1].position, clipped[0].color);
  }
}

HandVec3 TransformHandPoint(const std::array<float, 4>& orientation,
                            const std::array<float, 3>& position, const HandVec3& point)
{
  HandVec3 result = RotateHandVector(orientation, point);
  result[0] += position[0];
  result[1] += position[1];
  result[2] += position[2];
  return result;
}

HandVec3 InverseTransformHandPoint(const XrPosef& pose, const HandVec3& point)
{
  const HandVec3 translated = {point[0] - pose.position.x, point[1] - pose.position.y,
                               point[2] - pose.position.z};
  const std::array<float, 4> inverse_q = {-pose.orientation.x, -pose.orientation.y,
                                           -pose.orientation.z, pose.orientation.w};
  return RotateHandVector(inverse_q, translated);
}

XrQuaternionf NormalizeHandQuaternion(XrQuaternionf q)
{
  const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (len <= 1.0e-6f)
    return {0.0f, 0.0f, 0.0f, 1.0f};
  const float inv = 1.0f / len;
  return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

XrQuaternionf MultiplyHandQuaternions(const XrQuaternionf& a, const XrQuaternionf& b)
{
  return NormalizeHandQuaternion(
      {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
       a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
       a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
       a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z});
}

XrQuaternionf HandAxisAngle(const HandVec3& axis, float angle)
{
  const HandVec3 unit_axis = HandNormalize(axis);
  const float half = angle * 0.5f;
  const float s = std::sin(half);
  return {unit_axis[0] * s, unit_axis[1] * s, unit_axis[2] * s, std::cos(half)};
}

XrQuaternionf InverseHandQuaternion(const XrQuaternionf& q)
{
  const XrQuaternionf normalized = NormalizeHandQuaternion(q);
  return {-normalized.x, -normalized.y, -normalized.z, normalized.w};
}

XrQuaternionf BlendHandQuaternion(const XrQuaternionf& a, XrQuaternionf b, float t)
{
  t = std::clamp(t, 0.0f, 1.0f);
  XrQuaternionf from = NormalizeHandQuaternion(a);
  b = NormalizeHandQuaternion(b);
  float dot = from.x * b.x + from.y * b.y + from.z * b.z + from.w * b.w;
  if (dot < 0.0f)
  {
    b = {-b.x, -b.y, -b.z, -b.w};
    dot = -dot;
  }

  dot = std::clamp(dot, -1.0f, 1.0f);
  if (dot > 0.9995f)
  {
    return NormalizeHandQuaternion(
        {from.x + (b.x - from.x) * t, from.y + (b.y - from.y) * t,
         from.z + (b.z - from.z) * t, from.w + (b.w - from.w) * t});
  }

  const float theta = std::acos(dot);
  const float sin_theta = std::sin(theta);
  if (std::abs(sin_theta) < 1.0e-6f)
    return from;

  const float from_weight = std::sin((1.0f - t) * theta) / sin_theta;
  const float to_weight = std::sin(t * theta) / sin_theta;
  return NormalizeHandQuaternion({from.x * from_weight + b.x * to_weight,
                                  from.y * from_weight + b.y * to_weight,
                                  from.z * from_weight + b.z * to_weight,
                                  from.w * from_weight + b.w * to_weight});
}

// Godot XR Tools default right-hand pose uses Grip 5.res as the open pose and Grip.res as the
// closed pose. These are the exact local bone rotations stored by those animations, in track order.
// The left-hand resources mirror Y/Z, so we can derive them without maintaining a second table.
constexpr std::array<XrHandJointEXT, 19> GODOT_HAND_TRACK_JOINTS = {{
    XR_HAND_JOINT_THUMB_METACARPAL_EXT,
    XR_HAND_JOINT_THUMB_PROXIMAL_EXT,
    XR_HAND_JOINT_THUMB_DISTAL_EXT,
    XR_HAND_JOINT_INDEX_METACARPAL_EXT,
    XR_HAND_JOINT_INDEX_PROXIMAL_EXT,
    XR_HAND_JOINT_INDEX_INTERMEDIATE_EXT,
    XR_HAND_JOINT_INDEX_DISTAL_EXT,
    XR_HAND_JOINT_MIDDLE_METACARPAL_EXT,
    XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT,
    XR_HAND_JOINT_MIDDLE_INTERMEDIATE_EXT,
    XR_HAND_JOINT_MIDDLE_DISTAL_EXT,
    XR_HAND_JOINT_RING_METACARPAL_EXT,
    XR_HAND_JOINT_RING_PROXIMAL_EXT,
    XR_HAND_JOINT_RING_INTERMEDIATE_EXT,
    XR_HAND_JOINT_RING_DISTAL_EXT,
    XR_HAND_JOINT_LITTLE_METACARPAL_EXT,
    XR_HAND_JOINT_LITTLE_PROXIMAL_EXT,
    XR_HAND_JOINT_LITTLE_INTERMEDIATE_EXT,
    XR_HAND_JOINT_LITTLE_DISTAL_EXT,
}};

constexpr std::array<XrQuaternionf, 19> GODOT_RIGHT_OPEN = {{
    {-0.090444073f, 0.041517481f, 0.166293472f, 0.981041610f},
    {-0.046619851f, -0.020970983f, -0.010327632f, 0.998639166f},
    {-0.001284546f, 0.011608096f, 0.016825879f, 0.999790251f},
    {0.102924921f, 0.009932086f, 0.007944196f, 0.994607806f},
    {-0.012859004f, 0.023610815f, 0.323258311f, 0.945928752f},
    {0.012057537f, 0.009291926f, 0.247472256f, 0.968775511f},
    {-0.035753887f, 0.000400033f, -0.006367633f, 0.999340355f},
    {-0.002649636f, 0.001144711f, 0.125991791f, 0.992027104f},
    {0.039422531f, -0.001933927f, 0.228074118f, 0.972843468f},
    {-0.012339469f, 0.008812944f, 0.280669093f, 0.959684849f},
    {-0.070265576f, -0.010190837f, 0.024330752f, 0.997179568f},
    {-0.032063454f, 0.002236245f, 0.068636559f, 0.997123897f},
    {0.025345206f, -0.008124619f, 0.249005452f, 0.968136311f},
    {0.002522330f, -0.007880733f, 0.243203878f, 0.969940007f},
    {-0.091736883f, -0.020302719f, 0.010182976f, 0.995524228f},
    {-0.062518172f, 0.000225722f, 0.115392826f, 0.991350532f},
    {0.058578640f, -0.021648303f, 0.269905210f, 0.960859597f},
    {0.006871763f, 0.003572745f, 0.211952537f, 0.977249324f},
    {0.323536992f, 0.000025657f, 0.027220426f, 0.945823908f},
}};

constexpr std::array<XrQuaternionf, 19> GODOT_RIGHT_CLOSED = {{
    {0.329559475f, -0.254927784f, 0.152024835f, 0.896264970f},
    {-0.303609967f, -0.062464122f, 0.228709221f, 0.922827899f},
    {-0.421074748f, 0.118121445f, 0.243319094f, 0.865759373f},
    {-0.001284546f, 0.011608096f, 0.016825879f, 0.999790251f},
    {0.024201123f, 0.046170827f, 0.651622951f, 0.756749690f},
    {-0.002395436f, 0.020050196f, 0.657487631f, 0.753194690f},
    {-0.036981970f, -0.034581255f, 0.793887198f, 0.605953455f},
    {-0.035753887f, 0.000400033f, -0.006367633f, 0.999340355f},
    {0.044671372f, 0.011272614f, 0.686928332f, 0.725263298f},
    {0.028719656f, 0.018174244f, 0.648451388f, 0.760496974f},
    {-0.013303184f, 0.031132488f, 0.815732896f, 0.577437162f},
    {-0.070265576f, -0.010190837f, 0.024330752f, 0.997179568f},
    {0.082144149f, 0.004789548f, 0.610544562f, 0.787695885f},
    {0.056462385f, 0.046940714f, 0.622986674f, 0.778778672f},
    {0.081240460f, 0.022468932f, 0.834616899f, 0.544343412f},
    {-0.091736883f, -0.020302719f, 0.010182976f, 0.995524228f},
    {0.103134386f, 0.027251659f, 0.608737350f, 0.786167622f},
    {0.098543160f, 0.051522903f, 0.656269729f, 0.746287286f},
    {0.126803875f, 0.053291064f, 0.791772723f, 0.595127583f},
}};

XrQuaternionf GetGodotHandQuaternion(const XrQuaternionf& right_hand_quaternion, int hand)
{
  if (hand == 0)
  {
    // Godot's left-hand Grip resources are exact mirrors of the right-hand animation on Y/Z.
    return {right_hand_quaternion.x, -right_hand_quaternion.y, -right_hand_quaternion.z,
            right_hand_quaternion.w};
  }
  return right_hand_quaternion;
}

XrPosef GetRuntimeHandLocalBindPose(const VR::TabletopHandMesh& mesh, size_t joint)
{
  const XrPosef& global = mesh.joint_bind_poses[joint];
  const int parent = static_cast<int>(mesh.joint_parents[joint]);
  if (parent < 0 || static_cast<size_t>(parent) >= mesh.joint_bind_poses.size() ||
      static_cast<size_t>(parent) == joint)
  {
    return global;
  }

  const XrPosef& parent_global = mesh.joint_bind_poses[static_cast<size_t>(parent)];
  const XrQuaternionf inverse_parent = InverseHandQuaternion(parent_global.orientation);
  const std::array<float, 4> inverse_parent_q{inverse_parent.x, inverse_parent.y, inverse_parent.z,
                                               inverse_parent.w};
  const HandVec3 offset = {global.position.x - parent_global.position.x,
                           global.position.y - parent_global.position.y,
                           global.position.z - parent_global.position.z};
  const HandVec3 local_position = RotateHandVector(inverse_parent_q, offset);

  XrPosef local{};
  local.position = {local_position[0], local_position[1], local_position[2]};
  local.orientation = MultiplyHandQuaternions(inverse_parent, global.orientation);
  return local;
}

XrPosef ComposeRuntimeHandPose(const XrPosef& parent, const XrPosef& local)
{
  const std::array<float, 4> parent_q{parent.orientation.x, parent.orientation.y,
                                      parent.orientation.z, parent.orientation.w};
  const HandVec3 rotated_position =
      RotateHandVector(parent_q, {local.position.x, local.position.y, local.position.z});

  XrPosef result{};
  result.position = {parent.position.x + rotated_position[0],
                     parent.position.y + rotated_position[1],
                     parent.position.z + rotated_position[2]};
  result.orientation = MultiplyHandQuaternions(parent.orientation, local.orientation);
  return result;
}

HandVec3 GetRuntimeHandBindPosition(const VR::TabletopHandMesh& mesh, XrHandJointEXT joint)
{
  const XrVector3f& position = mesh.joint_bind_poses[static_cast<size_t>(joint)].position;
  return {position.x, position.y, position.z};
}

bool BuildGodotDrivenMetaHandPose(const VR::TabletopHandMesh& mesh, int hand, float index_amount,
                                  float grip_amount, float thumb_amount,
                                  std::vector<XrPosef>* posed)
{
  const size_t joint_count = mesh.joint_bind_poses.size();
  if (joint_count < XR_HAND_JOINT_COUNT_EXT || mesh.joint_parents.size() < joint_count)
    return false;

  std::vector<XrPosef> local_poses(joint_count);
  for (size_t joint = 0; joint < joint_count; ++joint)
    local_poses[joint] = GetRuntimeHandLocalBindPose(mesh, joint);

  index_amount = std::clamp(index_amount, 0.0f, 1.0f);
  grip_amount = std::clamp(grip_amount, 0.0f, 1.0f);
  thumb_amount = std::clamp(thumb_amount, 0.0f, 1.0f);

  for (size_t track = 0; track < GODOT_HAND_TRACK_JOINTS.size(); ++track)
  {
    const size_t joint = static_cast<size_t>(GODOT_HAND_TRACK_JOINTS[track]);
    if (joint >= local_poses.size())
      return false;

    float amount = grip_amount;
    if (track <= 2)
      amount = thumb_amount;
    else if (track <= 6)
      amount = index_amount;

    const XrQuaternionf godot_open = GetGodotHandQuaternion(GODOT_RIGHT_OPEN[track], hand);
    const XrQuaternionf godot_closed = GetGodotHandQuaternion(GODOT_RIGHT_CLOSED[track], hand);
    const XrQuaternionf godot_delta =
        MultiplyHandQuaternions(InverseHandQuaternion(godot_open), godot_closed);
    const XrQuaternionf meta_closed =
        MultiplyHandQuaternions(local_poses[joint].orientation, godot_delta);
    local_poses[joint].orientation =
        BlendHandQuaternion(local_poses[joint].orientation, meta_closed, amount);
  }

  posed->assign(joint_count, XrPosef{});
  std::vector<bool> built(joint_count, false);
  size_t built_count = 0;
  for (size_t pass = 0; pass < joint_count && built_count < joint_count; ++pass)
  {
    bool made_progress = false;
    for (size_t joint = 0; joint < joint_count; ++joint)
    {
      if (built[joint])
        continue;

      const int parent = static_cast<int>(mesh.joint_parents[joint]);
      if (parent < 0 || static_cast<size_t>(parent) >= joint_count ||
          static_cast<size_t>(parent) == joint)
      {
        (*posed)[joint] = local_poses[joint];
      }
      else
      {
        const size_t parent_index = static_cast<size_t>(parent);
        if (!built[parent_index])
          continue;
        (*posed)[joint] = ComposeRuntimeHandPose((*posed)[parent_index], local_poses[joint]);
      }

      built[joint] = true;
      ++built_count;
      made_progress = true;
    }

    if (!made_progress)
      break;
  }

  return built_count == joint_count;
}

bool RebuildRuntimeHandGlobalPose(const VR::TabletopHandMesh& mesh,
                                  const std::vector<XrPosef>& local_poses,
                                  std::vector<XrPosef>* global_poses)
{
  const size_t joint_count = local_poses.size();
  if (joint_count == 0 || mesh.joint_parents.size() < joint_count)
    return false;

  global_poses->assign(joint_count, XrPosef{});
  std::vector<bool> built(joint_count, false);
  size_t built_count = 0;
  for (size_t pass = 0; pass < joint_count && built_count < joint_count; ++pass)
  {
    bool made_progress = false;
    for (size_t joint = 0; joint < joint_count; ++joint)
    {
      if (built[joint])
        continue;

      const int parent = static_cast<int>(mesh.joint_parents[joint]);
      if (parent < 0 || static_cast<size_t>(parent) >= joint_count ||
          static_cast<size_t>(parent) == joint)
      {
        (*global_poses)[joint] = local_poses[joint];
      }
      else
      {
        const size_t parent_index = static_cast<size_t>(parent);
        if (!built[parent_index])
          continue;
        (*global_poses)[joint] =
            ComposeRuntimeHandPose((*global_poses)[parent_index], local_poses[joint]);
      }

      built[joint] = true;
      ++built_count;
      made_progress = true;
    }
    if (!made_progress)
      break;
  }
  return built_count == joint_count;
}

void ApplyValveLocalJointCurl(const VR::TabletopHandMesh& mesh, std::vector<XrPosef>* local_poses,
                              XrHandJointEXT joint, XrHandJointEXT child,
                              const HandVec3& flex_direction_global, float radians)
{
  const size_t joint_index = static_cast<size_t>(joint);
  const size_t child_index = static_cast<size_t>(child);
  if (joint_index >= local_poses->size() || child_index >= mesh.joint_bind_poses.size() ||
      std::abs(radians) < 1.0e-5f)
  {
    return;
  }

  const HandVec3 joint_position = GetRuntimeHandBindPosition(mesh, joint);
  const HandVec3 child_position = GetRuntimeHandBindPosition(mesh, child);
  const HandVec3 finger_direction = HandNormalize(HandSub(child_position, joint_position));
  const HandVec3 hinge_global = HandNormalize(HandCross(finger_direction, flex_direction_global));
  if (HandDot(hinge_global, hinge_global) < 1.0e-8f)
    return;

  // Valve's sample stores each bone transform in its parent's space. Convert the anatomical hinge
  // from Meta's bind/global hand space into this joint's local space, then post-multiply the bind
  // local orientation. FK reconstruction moves every child joint along the resulting arc.
  const XrQuaternionf inverse_joint_bind =
      InverseHandQuaternion(mesh.joint_bind_poses[joint_index].orientation);
  const std::array<float, 4> inverse_joint_q{inverse_joint_bind.x, inverse_joint_bind.y,
                                             inverse_joint_bind.z, inverse_joint_bind.w};
  const HandVec3 hinge_local = RotateHandVector(inverse_joint_q, hinge_global);
  const XrQuaternionf bend = HandAxisAngle(hinge_local, radians);
  (*local_poses)[joint_index].orientation =
      MultiplyHandQuaternions((*local_poses)[joint_index].orientation, bend);
}

void ApplyValveFingerCurl(const VR::TabletopHandMesh& mesh, std::vector<XrPosef>* local_poses,
                          const std::array<XrHandJointEXT, 5>& finger,
                          const HandVec3& flex_direction, float amount)
{
  constexpr float DEG_TO_RAD = 0.01745329251994329577f;
  amount = std::clamp(amount, 0.0f, 1.0f);
  const std::array<float, 4> angles = {5.0f, 90.0f, 80.0f, 80.0f};
  for (size_t i = 0; i < angles.size(); ++i)
  {
    ApplyValveLocalJointCurl(mesh, local_poses, finger[i], finger[i + 1], flex_direction,
                             angles[i] * DEG_TO_RAD * amount);
  }
}

HandVec3 ChooseValveFingerFlexDirection(const VR::TabletopHandMesh& mesh,
                                        const std::array<XrHandJointEXT, 5>& finger,
                                        const HandVec3& palm_normal)
{
  const HandVec3 palm = GetRuntimeHandBindPosition(mesh, XR_HAND_JOINT_PALM_EXT);
  const auto score = [&](const HandVec3& direction) {
    std::vector<XrPosef> local_poses(mesh.joint_bind_poses.size());
    for (size_t joint = 0; joint < local_poses.size(); ++joint)
      local_poses[joint] = GetRuntimeHandLocalBindPose(mesh, joint);

    ApplyValveFingerCurl(mesh, &local_poses, finger, direction, 1.0f);
    std::vector<XrPosef> global_poses;
    if (!RebuildRuntimeHandGlobalPose(mesh, local_poses, &global_poses))
      return std::numeric_limits<float>::max();

    const XrVector3f& tip = global_poses[static_cast<size_t>(finger.back())].position;
    const HandVec3 delta = HandSub({tip.x, tip.y, tip.z}, palm);
    return HandDot(delta, delta);
  };

  const HandVec3 opposite = HandMul(palm_normal, -1.0f);
  return score(palm_normal) <= score(opposite) ? palm_normal : opposite;
}

bool BuildValveDrivenMetaHandPose(const VR::TabletopHandMesh& mesh, float index_amount,
                                  float grip_amount, float thumb_amount,
                                  std::vector<XrPosef>* posed)
{
  const size_t joint_count = mesh.joint_bind_poses.size();
  if (joint_count < XR_HAND_JOINT_COUNT_EXT || mesh.joint_parents.size() < joint_count)
    return false;

  std::vector<XrPosef> local_poses(joint_count);
  for (size_t joint = 0; joint < joint_count; ++joint)
    local_poses[joint] = GetRuntimeHandLocalBindPose(mesh, joint);

  const HandVec3 across_palm = HandNormalize(HandSub(
      GetRuntimeHandBindPosition(mesh, XR_HAND_JOINT_INDEX_PROXIMAL_EXT),
      GetRuntimeHandBindPosition(mesh, XR_HAND_JOINT_LITTLE_PROXIMAL_EXT)));
  const HandVec3 along_palm = HandNormalize(HandSub(
      GetRuntimeHandBindPosition(mesh, XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT),
      GetRuntimeHandBindPosition(mesh, XR_HAND_JOINT_WRIST_EXT)));
  const HandVec3 palm_normal = HandNormalize(HandCross(across_palm, along_palm));

  const std::array<XrHandJointEXT, 5> index = {
      XR_HAND_JOINT_INDEX_METACARPAL_EXT, XR_HAND_JOINT_INDEX_PROXIMAL_EXT,
      XR_HAND_JOINT_INDEX_INTERMEDIATE_EXT, XR_HAND_JOINT_INDEX_DISTAL_EXT,
      XR_HAND_JOINT_INDEX_TIP_EXT};
  const std::array<XrHandJointEXT, 5> middle = {
      XR_HAND_JOINT_MIDDLE_METACARPAL_EXT, XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT,
      XR_HAND_JOINT_MIDDLE_INTERMEDIATE_EXT, XR_HAND_JOINT_MIDDLE_DISTAL_EXT,
      XR_HAND_JOINT_MIDDLE_TIP_EXT};
  const std::array<XrHandJointEXT, 5> ring = {
      XR_HAND_JOINT_RING_METACARPAL_EXT, XR_HAND_JOINT_RING_PROXIMAL_EXT,
      XR_HAND_JOINT_RING_INTERMEDIATE_EXT, XR_HAND_JOINT_RING_DISTAL_EXT,
      XR_HAND_JOINT_RING_TIP_EXT};
  const std::array<XrHandJointEXT, 5> little = {
      XR_HAND_JOINT_LITTLE_METACARPAL_EXT, XR_HAND_JOINT_LITTLE_PROXIMAL_EXT,
      XR_HAND_JOINT_LITTLE_INTERMEDIATE_EXT, XR_HAND_JOINT_LITTLE_DISTAL_EXT,
      XR_HAND_JOINT_LITTLE_TIP_EXT};

  ApplyValveFingerCurl(mesh, &local_poses, index,
                       HandMul(ChooseValveFingerFlexDirection(mesh, index, palm_normal), -1.0f),
                       index_amount);
  ApplyValveFingerCurl(mesh, &local_poses, middle,
                       ChooseValveFingerFlexDirection(mesh, middle, palm_normal), grip_amount);
  ApplyValveFingerCurl(mesh, &local_poses, ring,
                       ChooseValveFingerFlexDirection(mesh, ring, palm_normal), grip_amount);
  ApplyValveFingerCurl(mesh, &local_poses, little,
                       HandMul(ChooseValveFingerFlexDirection(mesh, little, palm_normal), -1.0f),
                       grip_amount);

  // Keep the thumb conservative because its previous controller pose was already visually good.
  // Use the same local-parent FK mechanism, but with smaller bends than Valve's empty-hand sample.
  const HandVec3 thumb_target =
      GetRuntimeHandBindPosition(mesh, XR_HAND_JOINT_INDEX_PROXIMAL_EXT);
  const std::array<XrHandJointEXT, 4> thumb = {
      XR_HAND_JOINT_THUMB_METACARPAL_EXT, XR_HAND_JOINT_THUMB_PROXIMAL_EXT,
      XR_HAND_JOINT_THUMB_DISTAL_EXT, XR_HAND_JOINT_THUMB_TIP_EXT};
  constexpr float DEG_TO_RAD = 0.01745329251994329577f;
  const std::array<float, 3> thumb_angles = {5.0f, 40.0f, 35.0f};
  for (size_t i = 0; i < thumb_angles.size(); ++i)
  {
    const HandVec3 root = GetRuntimeHandBindPosition(mesh, thumb[i]);
    const HandVec3 flex_direction = HandNormalize(HandSub(thumb_target, root));
    ApplyValveLocalJointCurl(mesh, &local_poses, thumb[i], thumb[i + 1], flex_direction,
                             thumb_angles[i] * DEG_TO_RAD * std::clamp(thumb_amount, 0.0f, 1.0f));
  }

  return RebuildRuntimeHandGlobalPose(mesh, local_poses, posed);
}

bool BuildTouchDrivenRuntimeHandJoints(
    const VR::TabletopHandMesh& mesh, const Common::VR::OpenXRControllerState& controller, int hand,
    std::array<Common::VR::OpenXRPoseState, XR_HAND_JOINT_COUNT_EXT>* out_joints)
{
  if (mesh.joint_bind_poses.size() < XR_HAND_JOINT_COUNT_EXT ||
      mesh.joint_parents.size() < XR_HAND_JOINT_COUNT_EXT)
  {
    return false;
  }

  const Common::VR::OpenXRPoseState* controller_pose =
      controller.grip_pose.valid ? &controller.grip_pose :
      controller.aim_pose.valid  ? &controller.aim_pose : nullptr;
  if (!controller_pose)
    return false;

  // Use Valve's official hand-skeleton simulation model for controller-driven curls: each joint
  // rotates in its local/parent space, and the full Meta hierarchy is rebuilt with FK. This keeps
  // the Meta mesh/lengths while avoiding the cross-skeleton quaternion retargeting that made the
  // last Godot-based attempt rotate fingers almost in place.
  const float index_curl =
      std::clamp(std::max(controller.hand_trigger_value, controller.trigger_button ? 1.0f : 0.0f),
                 0.0f, 1.0f);
  const float grip_curl =
      std::clamp(std::max(controller.hand_squeeze_value, controller.squeeze_button ? 1.0f : 0.0f),
                 0.0f, 1.0f);
  const float thumb_curl =
      std::clamp(std::max(grip_curl * 0.16f, controller.hand_thumb_pressed ? 0.58f : 0.0f), 0.0f,
                 1.0f);

  std::vector<XrPosef> posed;
  if (!BuildValveDrivenMetaHandPose(mesh, index_curl, grip_curl, thumb_curl, &posed))
    return false;

  // If simultaneous optical tracking has ever been available, OpenXRManager gives us the measured
  // Touch-grip -> wrist transform. Otherwise use the grip pose as the PALM pose, which matches the
  // OpenXR grip-pose semantics much better than pretending the controller origin is the wrist.
  // This removes the large rotation/translation error seen with the previous hard-coded offset.
  const bool use_measured_wrist =
      controller.hand_wrist_from_grip_valid && controller.grip_pose.valid;
  const XrPosef& bind_reference =
      mesh.joint_bind_poses[use_measured_wrist ? XR_HAND_JOINT_WRIST_EXT : XR_HAND_JOINT_PALM_EXT];
  const XrQuaternionf inverse_bind_reference =
      {-bind_reference.orientation.x, -bind_reference.orientation.y, -bind_reference.orientation.z,
       bind_reference.orientation.w};

  std::array<float, 3> room_reference_position = controller_pose->position;
  std::array<float, 4> room_reference_orientation = controller_pose->orientation;
  if (!use_measured_wrist)
  {
    // Match Godot XR Tools' Meta Touch grip-to-palm orientation: Touch/Touch Plus use a -60 degree
    // X-axis grip rotation. Their palm offset does not add a separate left/right roll, so keep the
    // fallback symmetrical and let the runtime controller pose provide the hand-specific cant.
    constexpr float DEG_TO_RAD = 0.01745329251994329577f;
    const XrQuaternionf controller_orientation{room_reference_orientation[0],
                                                room_reference_orientation[1],
                                                room_reference_orientation[2],
                                                room_reference_orientation[3]};
    const XrQuaternionf wrist_tilt =
        HandAxisAngle({1.0f, 0.0f, 0.0f}, -60.0f * DEG_TO_RAD);
    const XrQuaternionf tilted_orientation =
        MultiplyHandQuaternions(controller_orientation, wrist_tilt);
    room_reference_orientation = {tilted_orientation.x, tilted_orientation.y,
                                  tilted_orientation.z, tilted_orientation.w};
  }
  else
  {
    const auto& calibration = controller.hand_wrist_from_grip;
    room_reference_position =
        TransformHandPoint(controller.grip_pose.orientation, controller.grip_pose.position,
                           calibration.position);
    const XrQuaternionf grip_orientation{
        controller.grip_pose.orientation[0], controller.grip_pose.orientation[1],
        controller.grip_pose.orientation[2], controller.grip_pose.orientation[3]};
    const XrQuaternionf calibration_orientation{
        calibration.orientation[0], calibration.orientation[1], calibration.orientation[2],
        calibration.orientation[3]};
    const XrQuaternionf wrist_orientation =
        MultiplyHandQuaternions(grip_orientation, calibration_orientation);
    room_reference_orientation = {wrist_orientation.x, wrist_orientation.y, wrist_orientation.z,
                                  wrist_orientation.w};
  }
  const XrQuaternionf room_reference_q{room_reference_orientation[0],
                                        room_reference_orientation[1],
                                        room_reference_orientation[2],
                                        room_reference_orientation[3]};

  for (size_t joint = 0; joint < out_joints->size(); ++joint)
  {
    const XrPosef& src = posed[joint];
    const HandVec3 reference_local = InverseTransformHandPoint(
        bind_reference, {src.position.x, src.position.y, src.position.z});

    auto& dst = (*out_joints)[joint];
    dst.valid = true;
    dst.position = TransformHandPoint(room_reference_orientation, room_reference_position,
                                      reference_local);
    const XrQuaternionf relative_orientation =
        MultiplyHandQuaternions(inverse_bind_reference, src.orientation);
    const XrQuaternionf room_orientation =
        MultiplyHandQuaternions(room_reference_q, relative_orientation);
    dst.orientation = {room_orientation.x, room_orientation.y, room_orientation.z,
                       room_orientation.w};
  }
  return true;
}

bool BuildRuntimeControllerHand(const VR::TabletopHandMesh& mesh,
                                const Common::VR::OpenXRControllerState& controller, int hand,
                                std::vector<HandRoomVertex>* room_vertices)
{
  if (!mesh.valid || mesh.vertex_positions.empty() || mesh.indices.size() < 3)
    return false;

  // Hybrid mode: real optical OpenXR hand tracking wins whenever the runtime reports a valid
  // skeleton. If hand tracking is inactive, fall back immediately to the controller-driven
  // Valve/FK pose used by Touch trigger/grip. This restores the older tracked-hand behavior
  // without sacrificing the current controller animation when no hand is detected.
  const std::array<Common::VR::OpenXRPoseState, XR_HAND_JOINT_COUNT_EXT>* joints =
      &controller.hand_joints;
  std::array<Common::VR::OpenXRPoseState, XR_HAND_JOINT_COUNT_EXT> touch_joints{};
  if (!controller.hand_joints_valid)
  {
    if (!BuildTouchDrivenRuntimeHandJoints(mesh, controller, hand, &touch_joints))
      return false;
    joints = &touch_joints;
  }

  constexpr u32 SKIN = 0xFFFFFFFF;
  std::vector<HandVec3> skinned(mesh.vertex_positions.size());
  for (size_t vertex_index = 0; vertex_index < mesh.vertex_positions.size(); ++vertex_index)
  {
    const XrVector3f& src = mesh.vertex_positions[vertex_index];
    const HandVec3 bind_vertex{src.x, src.y, src.z};
    const XrVector4sFB& blend_indices = mesh.vertex_blend_indices[vertex_index];
    const XrVector4f& blend_weights = mesh.vertex_blend_weights[vertex_index];
    const std::array<int16_t, 4> indices = {blend_indices.x, blend_indices.y, blend_indices.z,
                                             blend_indices.w};
    const std::array<float, 4> weights = {blend_weights.x, blend_weights.y, blend_weights.z,
                                           blend_weights.w};

    HandVec3 blended{0.0f, 0.0f, 0.0f};
    float total_weight = 0.0f;
    for (size_t influence = 0; influence < 4; ++influence)
    {
      const int joint = indices[influence];
      const float weight = weights[influence];
      if (weight <= 0.0001f || joint < 0 ||
          static_cast<size_t>(joint) >= mesh.joint_bind_poses.size() ||
          static_cast<size_t>(joint) >= joints->size() || !(*joints)[joint].valid)
      {
        continue;
      }

      const HandVec3 joint_local =
          InverseTransformHandPoint(mesh.joint_bind_poses[joint], bind_vertex);
      const auto& current_joint = (*joints)[joint];
      const HandVec3 vertex =
          TransformHandPoint(current_joint.orientation, current_joint.position, joint_local);
      blended = HandAdd(blended, HandMul(vertex, weight));
      total_weight += weight;
    }

    if (total_weight <= 0.001f)
      return false;
    skinned[vertex_index] = HandMul(blended, 1.0f / total_weight);
  }

  const size_t before = room_vertices->size();
  room_vertices->reserve(before + mesh.indices.size());
  for (size_t index = 0; index + 2 < mesh.indices.size(); index += 3)
  {
    const int i0 = mesh.indices[index + 0];
    const int i1 = mesh.indices[index + 1];
    const int i2 = mesh.indices[index + 2];
    if (i0 < 0 || i1 < 0 || i2 < 0 || static_cast<size_t>(i0) >= skinned.size() ||
        static_cast<size_t>(i1) >= skinned.size() || static_cast<size_t>(i2) >= skinned.size())
    {
      continue;
    }
    AppendHandTriangle(room_vertices, skinned[i0], skinned[i1], skinned[i2], SKIN);
  }

  return room_vertices->size() > before;
}

bool ProjectHandPointToEye(const VR::XREyeView& eye, const HandVec3& room_point,
                           HandVec3* out_ndc)
{
  const HandVec3 delta = {room_point[0] - eye.pose.position.x,
                          room_point[1] - eye.pose.position.y,
                          room_point[2] - eye.pose.position.z};
  const std::array<float, 4> inverse_eye_q = {-eye.pose.orientation.x, -eye.pose.orientation.y,
                                               -eye.pose.orientation.z,
                                               eye.pose.orientation.w};
  const HandVec3 local = RotateHandVector(inverse_eye_q, delta);
  const float depth = -local[2];
  if (depth < 0.025f)
    return false;

  const float tan_l = std::tan(eye.fov.angleLeft);
  const float tan_r = std::tan(eye.fov.angleRight);
  const float tan_u = std::tan(eye.fov.angleUp);
  const float tan_d = std::tan(eye.fov.angleDown);
  const float sx = local[0] / depth;
  const float sy = local[1] / depth;
  const float width = tan_r - tan_l;
  const float height = tan_u - tan_d;
  if (std::abs(width) < 1.0e-6f || std::abs(height) < 1.0e-6f)
    return false;

  *out_ndc = {(2.0f * sx - (tan_r + tan_l)) / width,
              (2.0f * sy - (tan_u + tan_d)) / height, 0.0f};
  return true;
}
}  // namespace
#endif

// Stretches the native/internal analog resolution aspect ratio from ~4:3 to ~16:9
static float SourceAspectRatioToWidescreen(float source_aspect)
{
  return source_aspect * ((16.0f / 9.0f) / (4.0f / 3.0f));
}

static std::tuple<int, int> FindClosestIntegerResolution(float width, float height,
                                                         float aspect_ratio)
{
  // We can't round both the x and y resolution as that might generate an aspect ratio
  // further away from the target one, we also can't either ceil or floor both sides,
  // so we find the combination or flooring and ceiling that is closest to the target ar.
  const int ceiled_width = static_cast<int>(std::ceil(width));
  const int ceiled_height = static_cast<int>(std::ceil(height));
  const int floored_width = static_cast<int>(std::floor(width));
  const int floored_height = static_cast<int>(std::floor(height));

  int int_width = floored_width;
  int int_height = floored_height;

  float min_aspect_ratio_distance = std::numeric_limits<float>::max();
  for (const int new_width : std::array<int, 2>{ceiled_width, floored_width})
  {
    for (const int new_height : std::array<int, 2>{ceiled_height, floored_height})
    {
      const float new_aspect_ratio = static_cast<float>(new_width) / new_height;
      const float aspect_ratio_distance = std::abs((new_aspect_ratio / aspect_ratio) - 1.f);
      if (aspect_ratio_distance < min_aspect_ratio_distance)
      {
        min_aspect_ratio_distance = aspect_ratio_distance;
        int_width = new_width;
        int_height = new_height;
      }
    }
  }

  return std::make_tuple(int_width, int_height);
}

static void TryToSnapToXFBSize(int& width, int& height, int xfb_width, int xfb_height)
{
  // Screen is blanking (e.g. game booting up), nothing to do here
  if (xfb_width == 0 || xfb_height == 0)
    return;

  // If there's only 1 pixel of either horizontal or vertical resolution difference,
  // make the output size match a multiple of the XFB native resolution,
  // to achieve the highest quality (least scaling).
  // The reason why the threshold is 1 pixel (per internal resolution multiplier) is because of
  // minor inaccuracies of the VI aspect ratio (and because some resolutions are rounded
  // while other are floored).
  const unsigned int efb_scale = g_framebuffer_manager->GetEFBScale();
  const unsigned int pixel_difference_width = std::abs(width - xfb_width);
  const unsigned int pixel_difference_height = std::abs(height - xfb_height);
  // We ignore this if there's an offset on both hor and ver size,
  // as then we'd be changing the aspect ratio too much and would need to
  // re-calculate a lot of stuff (like black bars).
  if ((pixel_difference_width <= efb_scale && pixel_difference_height == 0) ||
      (pixel_difference_height <= efb_scale && pixel_difference_width == 0))
  {
    width = xfb_width;
    height = xfb_height;
  }
}

Presenter::Presenter()
{
  auto& video_events = GetVideoEvents();

  m_config_changed =
      video_events.config_changed_event.Register([this](u32 bits) { ConfigChanged(bits); });

  m_end_field_hook = video_events.vi_end_field_event.Register(
      [this] { m_immediate_swap_happened_this_field.store(false, std::memory_order_relaxed); });

}

Presenter::~Presenter()
{
  // Disable ControllerInterface's aspect ratio adjustments so mapping dialog behaves normally.
  g_controller_interface.SetAspectRatioAdjustment(1);
}

bool Presenter::Initialize()
{
  UpdateDrawRectangle();

  m_immediate_swap_happened_this_field.store(false, std::memory_order_relaxed);

#ifdef ENABLE_VR
  // The Android GLES OpenXR path uses a headless (pbuffer) GL context — Meta's runtime
  // never starts sessions for GLES contexts bound to a window surface — but the presenter
  // must still fully initialize (post processor for the eye blits) and present each frame
  // (Present() drives the XR frame lifecycle).
  const bool xr_headless_present = g_ActiveConfig.VRSessionActive() &&
                                   g_backend_info.api_type == APIType::OpenGL;
#else
  constexpr bool xr_headless_present = false;
#endif
  if (!g_gfx->IsHeadless() || xr_headless_present)
  {
    SetBackbuffer(g_gfx->GetSurfaceInfo());

    m_post_processor = std::make_unique<VideoCommon::PostProcessing>();
    if (!m_post_processor->Initialize(m_backbuffer_format))
      return false;

    m_onscreen_ui = std::make_unique<OnScreenUI>();
    if (!m_onscreen_ui->Initialize(m_backbuffer_width, m_backbuffer_height, m_backbuffer_scale))
      return false;

    // Draw a blank frame (and complete OnScreenUI initialization)
    g_gfx->BindBackbuffer({{0.0f, 0.0f, 0.0f, 1.0f}});
    g_gfx->PresentBackbuffer();
  }

  return true;
}

bool Presenter::FetchXFB(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height, u64 ticks)
{
  ReleaseXFBContentLock();
  u64 old_xfb_id = m_last_xfb_id;

  if (fb_width == 0 || fb_height == 0)
  {
    // Game is blanking the screen
    m_xfb_entry.reset();
    m_xfb_rect = MathUtil::Rectangle<int>();
    m_last_xfb_id = std::numeric_limits<u64>::max();
  }
  else
  {
    m_xfb_entry =
        g_texture_cache->GetXFBTexture(xfb_addr, fb_width, fb_height, fb_stride, &m_xfb_rect);
    m_last_xfb_id = m_xfb_entry->id;

    m_xfb_entry->AcquireContentLock();

#ifdef ENABLE_VR
    // Pair the frame about to be presented with the pose it was rendered with (stamped
    // at its XFB copy). With ImmediateXFB off this present runs mid-way through the
    // NEXT frame's draw stream, where the live submit snapshot may already hold the
    // next frame's pose — submitting that misplaces the content under ATW.
    if (g_ActiveConfig.stereo_mode == StereoMode::OpenXR && VR::g_openxr)
      VR::g_openxr->SelectPresentPoseForXFB(xfb_addr);
#endif
  }
  m_last_xfb_addr = xfb_addr;
  m_last_xfb_ticks = ticks;
  m_last_xfb_width = fb_width;
  m_last_xfb_stride = fb_stride;
  m_last_xfb_height = fb_height;

  return old_xfb_id == m_last_xfb_id;
}

void Presenter::ViSwap(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height, u64 ticks,
                       TimePoint presentation_time)
{
  bool is_duplicate = FetchXFB(xfb_addr, fb_width, fb_stride, fb_height, ticks);

  PresentInfo present_info{
      .present_count = m_present_count++,
      .emulated_timestamp = ticks,
      .intended_present_time = presentation_time,
  };

  if (is_duplicate)
  {
    present_info.frame_count = m_frame_count - 1;  // Previous frame
    present_info.reason = PresentInfo::PresentReason::VideoInterfaceDuplicate;
  }
  else
  {
    present_info.frame_count = m_frame_count++;
    present_info.reason = PresentInfo::PresentReason::VideoInterface;
  }

  if (m_xfb_entry)
  {
    // With no references, this XFB copy wasn't stitched together
    // so just use its name directly
    if (m_xfb_entry->references.empty())
    {
      if (!m_xfb_entry->texture_info_name.empty())
        present_info.xfb_copy_hashes.push_back(m_xfb_entry->texture_info_name);
    }
    else
    {
      for (const auto& reference : m_xfb_entry->references)
      {
        if (!reference->texture_info_name.empty())
          present_info.xfb_copy_hashes.push_back(reference->texture_info_name);
      }
    }
  }

  auto& video_events = GetVideoEvents();

  video_events.before_present_event.Trigger(present_info);

  if (!is_duplicate || !g_ActiveConfig.bSkipPresentingDuplicateXFBs)
  {
    Present(&present_info);
    ProcessFrameDumping(ticks);

    video_events.after_present_event.Trigger(present_info);
  }
}

void Presenter::ImmediateSwap(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height)
{
  if (m_immediate_swap_happened_this_field.exchange(true, std::memory_order_relaxed) &&
      Config::Get(Config::GFX_HACK_CAP_IMMEDIATE_XFB))
  {
    return;
  }

  const u64 ticks = m_next_swap_estimated_ticks;

  FetchXFB(xfb_addr, fb_width, fb_stride, fb_height, ticks);

  // Normal path: full events and frame counting
  PresentInfo present_info{
      .frame_count = m_frame_count++,
      .present_count = m_present_count++,
      .reason = PresentInfo::PresentReason::Immediate,
      .emulated_timestamp = ticks,
      .intended_present_time = m_next_swap_estimated_time,
  };

  auto& video_events = GetVideoEvents();

  video_events.before_present_event.Trigger(present_info);

  Present(&present_info);
  ProcessFrameDumping(ticks);

  video_events.after_present_event.Trigger(present_info);
}

void Presenter::SetNextSwapEstimatedTime(u64 ticks, TimePoint host_time)
{
  m_next_swap_estimated_ticks = ticks;
  m_next_swap_estimated_time = host_time;
}

void Presenter::ProcessFrameDumping(u64 ticks) const
{
  if (g_frame_dumper->IsFrameDumping() && m_xfb_entry)
  {
    MathUtil::Rectangle<int> target_rect;
    switch (Config::Get(Config::GFX_FRAME_DUMPS_RESOLUTION_TYPE))
    {
    default:
    case FrameDumpResolutionType::WindowResolution:
    {
      if (!g_gfx->IsHeadless())
      {
        target_rect = GetTargetRectangle();
        break;
      }
      [[fallthrough]];
    }
    case FrameDumpResolutionType::XFBAspectRatioCorrectedResolution:
    {
      target_rect = m_xfb_rect;
      const bool allow_stretch = false;
      auto [float_width, float_height] =
          ScaleToDisplayAspectRatio(m_xfb_rect.GetWidth(), m_xfb_rect.GetHeight(), allow_stretch);
      const float draw_aspect_ratio = CalculateDrawAspectRatio(allow_stretch);
      auto [int_width, int_height] =
          FindClosestIntegerResolution(float_width, float_height, draw_aspect_ratio);
      target_rect = MathUtil::Rectangle<int>(0, 0, int_width, int_height);
      break;
    }
    case FrameDumpResolutionType::XFBRawResolution:
    {
      target_rect = m_xfb_rect;
      break;
    }
    }

    int width = target_rect.GetWidth();
    int height = target_rect.GetHeight();

    const int resolution_lcm = g_frame_dumper->GetRequiredResolutionLeastCommonMultiple();

    // Ensure divisibility by the dumper LCM and a min of 1 to make it compatible with all the
    // video encoders. Note that this is theoretically only necessary when recording videos and not
    // screenshots.
    // We always scale positively to make sure the least amount of information is lost.
    //
    // TODO: this should be added as black padding on the edges by the frame dumper.
    if ((width % resolution_lcm) != 0 || width == 0)
      width += resolution_lcm - (width % resolution_lcm);
    if ((height % resolution_lcm) != 0 || height == 0)
      height += resolution_lcm - (height % resolution_lcm);

    // Remove any black borders, there would be no point in including them in the recording
    target_rect.left = 0;
    target_rect.top = 0;
    target_rect.right = width;
    target_rect.bottom = height;

    // TODO: any scaling done by this won't be gamma corrected,
    // we should either apply post processing as well, or port its gamma correction code
    g_frame_dumper->DumpCurrentFrame(m_xfb_entry->texture.get(), m_xfb_rect, target_rect, ticks,
                                     m_frame_count);
  }
}

void Presenter::SetBackbuffer(int backbuffer_width, int backbuffer_height)
{
  const bool is_first = m_backbuffer_width == 0 && m_backbuffer_height == 0;
  const bool size_changed =
      (m_backbuffer_width != backbuffer_width || m_backbuffer_height != backbuffer_height);
  m_backbuffer_width = backbuffer_width;
  m_backbuffer_height = backbuffer_height;
  UpdateDrawRectangle();

  OnBackbufferSet(size_changed, is_first);
}

void Presenter::SetBackbuffer(SurfaceInfo info)
{
  const bool is_first = m_backbuffer_width == 0 && m_backbuffer_height == 0;
  const bool size_changed =
      (m_backbuffer_width != (int)info.width || m_backbuffer_height != (int)info.height);
  m_backbuffer_width = info.width;
  m_backbuffer_height = info.height;
  m_backbuffer_scale = info.scale;
  m_backbuffer_format = info.format;
  if (m_onscreen_ui)
    m_onscreen_ui->SetScale(info.scale);

  OnBackbufferSet(size_changed, is_first);
}

void Presenter::OnBackbufferSet(bool size_changed, bool is_first_set)
{
  UpdateDrawRectangle();

  // Automatically update the resolution scale if the window size changed,
  // or if the game XFB resolution changed.
  if (size_changed && !is_first_set && g_ActiveConfig.iEFBScale == EFB_SCALE_AUTO_INTEGRAL &&
      m_auto_resolution_scale != AutoIntegralScale())
  {
    g_framebuffer_manager->RecreateEFBFramebuffer(g_ActiveConfig.iEFBScale);
  }
  if (size_changed || is_first_set)
  {
    m_auto_resolution_scale = AutoIntegralScale();
  }
}

void Presenter::ConfigChanged(u32 changed_bits)
{
  // Check for post-processing shader changes. Done up here as it doesn't affect anything outside
  // the post-processor. Note that options are applied every frame, so no need to check those.
  if (changed_bits & ConfigChangeBits::CONFIG_CHANGE_BIT_POST_PROCESSING_SHADER && m_post_processor)
  {
    // The existing shader must not be in use when it's destroyed
    g_gfx->WaitForGPUIdle();

    m_post_processor->RecompileShader();
  }

  // Stereo mode change requires recompiling our post processing pipeline and imgui pipelines for
  // rendering the UI.
  if (changed_bits & ConfigChangeBits::CONFIG_CHANGE_BIT_STEREO_MODE)
  {
    if (m_onscreen_ui)
      m_onscreen_ui->RecompileImGuiPipeline();
    if (m_post_processor)
      m_post_processor->RecompilePipeline();
  }
}

std::tuple<MathUtil::Rectangle<int>, MathUtil::Rectangle<int>>
Presenter::ConvertStereoRectangle(const MathUtil::Rectangle<int>& rc) const
{
  // Resize target to half its original size
  auto draw_rc = rc;
  if (g_ActiveConfig.stereo_mode == StereoMode::TAB)
  {
    // The height may be negative due to flipped rectangles
    int height = rc.bottom - rc.top;
    draw_rc.top += height / 4;
    draw_rc.bottom -= height / 4;
  }
  else
  {
    int width = rc.right - rc.left;
    draw_rc.left += width / 4;
    draw_rc.right -= width / 4;
  }

  // Create two target rectangle offset to the sides of the backbuffer
  auto left_rc = draw_rc;
  auto right_rc = draw_rc;
  if (g_ActiveConfig.stereo_mode == StereoMode::TAB)
  {
    left_rc.top -= m_backbuffer_height / 4;
    left_rc.bottom -= m_backbuffer_height / 4;
    right_rc.top += m_backbuffer_height / 4;
    right_rc.bottom += m_backbuffer_height / 4;
  }
  else
  {
    left_rc.left -= m_backbuffer_width / 4;
    left_rc.right -= m_backbuffer_width / 4;
    right_rc.left += m_backbuffer_width / 4;
    right_rc.right += m_backbuffer_width / 4;
  }

  return std::make_tuple(left_rc, right_rc);
}

float Presenter::CalculateDrawAspectRatio(bool allow_stretch) const
{
  auto aspect_mode = g_ActiveConfig.aspect_mode;
  float resulting_aspect_ratio;

  if (!allow_stretch && aspect_mode == AspectMode::Stretch)
    aspect_mode = AspectMode::Auto;

  // If stretch is enabled, we prefer the aspect ratio of the window.
  if (aspect_mode == AspectMode::Stretch)
  {
    resulting_aspect_ratio =
        (static_cast<float>(m_backbuffer_width) / static_cast<float>(m_backbuffer_height));
  }
  else
  {
    // The actual aspect ratio of the XFB texture is irrelevant, the VI one is the one that matters
    const auto& vi = Core::System::GetInstance().GetVideoInterface();
    const float source_aspect_ratio = vi.GetAspectRatio();

    // This will scale up the source ~4:3 resolution to its equivalent ~16:9 resolution
    if (aspect_mode == AspectMode::ForceWide ||
        (aspect_mode == AspectMode::Auto && g_widescreen->IsGameWidescreen()))
    {
      resulting_aspect_ratio = SourceAspectRatioToWidescreen(source_aspect_ratio);
    }
    else if (aspect_mode == AspectMode::Custom)
    {
      resulting_aspect_ratio =
          source_aspect_ratio * (g_ActiveConfig.GetCustomAspectRatio() / (4.0f / 3.0f));
    }
    // For the "custom stretch" mode, we force the exact target aspect ratio, without
    // acknowledging the difference between the source aspect ratio and 4:3.
    else if (aspect_mode == AspectMode::CustomStretch)
    {
      resulting_aspect_ratio = g_ActiveConfig.GetCustomAspectRatio();
    }
    else if (aspect_mode == AspectMode::Raw)
    {
      resulting_aspect_ratio =
          m_xfb_entry ? (static_cast<float>(m_last_xfb_width) / m_last_xfb_height) : 1.f;
    }
    else
    {
      resulting_aspect_ratio = source_aspect_ratio;
    }
  }

  if (g_ActiveConfig.stereo_per_eye_resolution_full)
  {
    if (g_ActiveConfig.stereo_mode == StereoMode::SBS)
    {
      // Render twice as wide if using side-by-side 3D, since the 3D will halve the horizontal
      // resolution
      resulting_aspect_ratio *= 2.0;
    }
    else if (g_ActiveConfig.stereo_mode == StereoMode::TAB)
    {
      // Render twice as tall if using top-and-bottom 3D, since the 3D will halve the vertical
      // resolution
      resulting_aspect_ratio /= 2.0;
    }
  }

  return resulting_aspect_ratio;
}

void Presenter::AdjustRectanglesToFitBounds(MathUtil::Rectangle<int>* target_rect,
                                            MathUtil::Rectangle<int>* source_rect, int fb_width,
                                            int fb_height)
{
  const int orig_target_width = target_rect->GetWidth();
  const int orig_target_height = target_rect->GetHeight();
  const int orig_source_width = source_rect->GetWidth();
  const int orig_source_height = source_rect->GetHeight();
  if (target_rect->left < 0)
  {
    const int offset = -target_rect->left;
    target_rect->left = 0;
    source_rect->left += offset * orig_source_width / orig_target_width;
  }
  if (target_rect->right > fb_width)
  {
    const int offset = target_rect->right - fb_width;
    target_rect->right -= offset;
    source_rect->right -= offset * orig_source_width / orig_target_width;
  }
  if (target_rect->top < 0)
  {
    const int offset = -target_rect->top;
    target_rect->top = 0;
    source_rect->top += offset * orig_source_height / orig_target_height;
  }
  if (target_rect->bottom > fb_height)
  {
    const int offset = target_rect->bottom - fb_height;
    target_rect->bottom -= offset;
    source_rect->bottom -= offset * orig_source_height / orig_target_height;
  }
}

void Presenter::ReleaseXFBContentLock()
{
  if (m_xfb_entry)
    m_xfb_entry->ReleaseContentLock();
}

void Presenter::ChangeSurface(void* new_surface_handle)
{
  std::lock_guard<std::mutex> lock(m_swap_mutex);
  m_new_surface_handle = new_surface_handle;
  m_surface_changed.Set();
}

void Presenter::ResizeSurface()
{
  std::lock_guard<std::mutex> lock(m_swap_mutex);
  m_surface_resized.Set();
}

void* Presenter::GetNewSurfaceHandle()
{
  void* handle = m_new_surface_handle;
  m_new_surface_handle = nullptr;
  return handle;
}

u32 Presenter::AutoIntegralScale() const
{
  // Take the source/native resolution (XFB) and stretch it on the target (window) aspect ratio.
  // If the target resolution is larger (on either x or y), we scale the source
  // by a integer multiplier until it won't have to be scaled up anymore.
  // NOTE: this might conflict with "Config::MAIN_RENDER_WINDOW_AUTOSIZE",
  // as they mutually influence each other.
  u32 source_width = m_last_xfb_width;
  u32 source_height = m_last_xfb_height;
  const u32 target_width = m_target_rectangle.GetWidth();
  const u32 target_height = m_target_rectangle.GetHeight();
  const float source_aspect_ratio = (float)source_width / source_height;
  const float target_aspect_ratio = (float)target_width / target_height;
  if (source_aspect_ratio >= target_aspect_ratio)
    source_width = std::round(source_height * target_aspect_ratio);
  else
    source_height = std::round(source_width / target_aspect_ratio);
  const u32 width_scale =
      source_width > 0 ? ((target_width + (source_width - 1)) / source_width) : 1;
  const u32 height_scale =
      source_height > 0 ? ((target_height + (source_height - 1)) / source_height) : 1;
  // Limit to the max to avoid creating textures larger than their max supported resolution.
  return std::min(std::max(width_scale, height_scale),
                  static_cast<u32>(Config::Get(Config::GFX_MAX_EFB_SCALE)));
}

void Presenter::SetSuggestedWindowSize(int width, int height)
{
  // While trying to guess the best window resolution, we can't allow it to use the
  // "AspectMode::Stretch" setting because that would self influence the output result,
  // given it would be based on the previous frame resolution
  const bool allow_stretch = false;
  const auto [out_width, out_height] = CalculateOutputDimensions(width, height, allow_stretch);

  // Track the last values of width/height to avoid sending a window resize event every frame.
  if (out_width == m_last_window_request_width && out_height == m_last_window_request_height)
    return;

  m_last_window_request_width = out_width;
  m_last_window_request_height = out_height;
  // Pass in the suggested window size. This might not always be acknowledged.
  Host_RequestRenderWindowSize(out_width, out_height);
}

// Crop to exact forced aspect ratios if enabled and not AspectMode::Stretch.
std::tuple<float, float> Presenter::ApplyStandardAspectCrop(float width, float height,
                                                            bool allow_stretch) const
{
  auto aspect_mode = g_ActiveConfig.aspect_mode;

  if (!allow_stretch && aspect_mode == AspectMode::Stretch)
    aspect_mode = AspectMode::Auto;

  if (!g_ActiveConfig.bCrop || aspect_mode == AspectMode::Stretch || aspect_mode == AspectMode::Raw)
    return {width, height};

  // Force aspect ratios by cropping the image.
  const float current_aspect = width / height;
  float expected_aspect;
  switch (aspect_mode)
  {
  default:
  case AspectMode::Auto:
    expected_aspect = g_widescreen->IsGameWidescreen() ? (16.0f / 9.0f) : (4.0f / 3.0f);
    break;
  case AspectMode::ForceWide:
    expected_aspect = 16.0f / 9.0f;
    break;
  case AspectMode::ForceStandard:
    expected_aspect = 4.0f / 3.0f;
    break;
  // For the custom (relative) case, we want to crop from the native aspect ratio
  // to the specific target one, as they likely have a small difference
  case AspectMode::Custom:
  // There should be no cropping needed in the custom stretch case,
  // as output should always exactly match the target aspect ratio
  case AspectMode::CustomStretch:
    expected_aspect = g_ActiveConfig.GetCustomAspectRatio();
    break;
  }

  if (current_aspect > expected_aspect)
  {
    // keep height, crop width
    width = height * expected_aspect;
  }
  else
  {
    // keep width, crop height
    height = width / expected_aspect;
  }

  return {width, height};
}

void Presenter::UpdateDrawRectangle()
{
  const float draw_aspect_ratio = CalculateDrawAspectRatio();

  // Update aspect ratio hack values
  // Won't take effect until next frame
  // Don't know if there is a better place for this code so there isn't a 1 frame delay
  if (g_ActiveConfig.bWidescreenHack)
  {
    const auto& vi = Core::System::GetInstance().GetVideoInterface();
    float source_aspect_ratio = vi.GetAspectRatio();
    // If the game is meant to be in widescreen (or forced to),
    // scale the source aspect ratio to it.
    if (g_widescreen->IsGameWidescreen())
      source_aspect_ratio = SourceAspectRatioToWidescreen(source_aspect_ratio);

    const float adjust = source_aspect_ratio / draw_aspect_ratio;
    if (adjust > 1)
    {
      // Vert+
      g_Config.fAspectRatioHackW = 1;
      g_Config.fAspectRatioHackH = 1 / adjust;
    }
    else
    {
      // Hor+
      g_Config.fAspectRatioHackW = adjust;
      g_Config.fAspectRatioHackH = 1;
    }
  }
  else
  {
    // Hack is disabled.
    g_Config.fAspectRatioHackW = 1;
    g_Config.fAspectRatioHackH = 1;
  }

  // The rendering window size
  const float win_width = static_cast<float>(m_backbuffer_width);
  const float win_height = static_cast<float>(m_backbuffer_height);
  const float win_aspect_ratio = win_width / win_height;

  // FIXME: this breaks at very low widget sizes
  // Make ControllerInterface aware of the render window region actually being used
  // to adjust mouse cursor inputs.
  // This also fails to acknowledge "g_ActiveConfig.bCrop".
  g_controller_interface.SetAspectRatioAdjustment(draw_aspect_ratio / win_aspect_ratio);

  float draw_width = draw_aspect_ratio;
  float draw_height = 1;

  // Crop the picture to a standard aspect ratio. (if enabled)
  auto [crop_width, crop_height] = ApplyStandardAspectCrop(draw_width, draw_height);
  const float crop_aspect_ratio = crop_width / crop_height;

  // scale the picture to fit the rendering window
  if (win_aspect_ratio >= crop_aspect_ratio)
  {
    // the window is flatter than the picture
    draw_width *= win_height / crop_height;
    crop_width *= win_height / crop_height;
    draw_height *= win_height / crop_height;
    crop_height = win_height;
  }
  else
  {
    // the window is skinnier than the picture
    draw_width *= win_width / crop_width;
    draw_height *= win_width / crop_width;
    crop_height *= win_width / crop_width;
    crop_width = win_width;
  }

  int int_draw_width;
  int int_draw_height;

  if (g_ActiveConfig.aspect_mode != AspectMode::Raw || !m_xfb_entry)
  {
    // Find the best integer resolution: the closest aspect ratio with the least black bars.
    // This should have no influence if "AspectMode::Stretch" is active.
    const float updated_draw_aspect_ratio = draw_width / draw_height;
    const auto int_draw_res =
        FindClosestIntegerResolution(draw_width, draw_height, updated_draw_aspect_ratio);
    int_draw_width = std::get<0>(int_draw_res);
    int_draw_height = std::get<1>(int_draw_res);
    if (!g_ActiveConfig.bCrop)
    {
      if (g_ActiveConfig.aspect_mode != AspectMode::Stretch)
      {
        TryToSnapToXFBSize(int_draw_width, int_draw_height, m_xfb_rect.GetWidth(),
                           m_xfb_rect.GetHeight());
      }
      // We can't draw something bigger than the window, it will crop
      int_draw_width = std::min(int_draw_width, static_cast<int>(win_width));
      int_draw_height = std::min(int_draw_height, static_cast<int>(win_height));
    }
  }
  else
  {
    int_draw_width = m_xfb_rect.GetWidth();
    int_draw_height = m_xfb_rect.GetHeight();
  }

  m_target_rectangle.left = static_cast<int>(std::round(win_width / 2.0 - int_draw_width / 2.0));
  m_target_rectangle.top = static_cast<int>(std::round(win_height / 2.0 - int_draw_height / 2.0));
  m_target_rectangle.right = m_target_rectangle.left + int_draw_width;
  m_target_rectangle.bottom = m_target_rectangle.top + int_draw_height;
}

std::tuple<float, float> Presenter::ScaleToDisplayAspectRatio(const int width, const int height,
                                                              bool allow_stretch) const
{
  // Scale either the width or height depending the content aspect ratio.
  // This way we preserve as much resolution as possible when scaling.
  float scaled_width = static_cast<float>(width);
  float scaled_height = static_cast<float>(height);
  const float draw_aspect = CalculateDrawAspectRatio(allow_stretch);
  if (scaled_width / scaled_height >= draw_aspect)
    scaled_height = scaled_width / draw_aspect;
  else
    scaled_width = scaled_height * draw_aspect;
  return std::make_tuple(scaled_width, scaled_height);
}

std::tuple<int, int> Presenter::CalculateOutputDimensions(int width, int height,
                                                          bool allow_stretch) const
{
  // Protect against zero width and height, a minimum of 1 will do
  width = std::max(width, 1);
  height = std::max(height, 1);

  auto [scaled_width, scaled_height] = ScaleToDisplayAspectRatio(width, height, allow_stretch);

  // Apply crop if enabled.
  std::tie(scaled_width, scaled_height) =
      ApplyStandardAspectCrop(scaled_width, scaled_height, allow_stretch);

  auto aspect_mode = g_ActiveConfig.aspect_mode;

  if (!allow_stretch && aspect_mode == AspectMode::Stretch)
    aspect_mode = AspectMode::Auto;

  if (!g_ActiveConfig.bCrop && aspect_mode != AspectMode::Stretch)
  {
    // Find the closest integer resolution for the aspect ratio,
    // this avoids a small black line from being drawn on one of the four edges
    const float draw_aspect_ratio = CalculateDrawAspectRatio(allow_stretch);
    auto [int_width, int_height] =
        FindClosestIntegerResolution(scaled_width, scaled_height, draw_aspect_ratio);
    if (aspect_mode != AspectMode::Raw)
    {
      TryToSnapToXFBSize(int_width, int_height, m_xfb_rect.GetWidth(), m_xfb_rect.GetHeight());
    }
    width = int_width;
    height = int_height;
  }
  else
  {
    width = static_cast<int>(std::ceil(scaled_width));
    height = static_cast<int>(std::ceil(scaled_height));
  }

  return std::make_tuple(width, height);
}

#ifdef ENABLE_VR
bool Presenter::StartOpenXRFrameNow(double* wait_frame_ms, double* locate_views_ms,
                                    bool do_locate_views)
{
  if (wait_frame_ms)
    *wait_frame_ms = 0.0;
  if (locate_views_ms)
    *locate_views_ms = 0.0;

  if (!g_ActiveConfig.VRSessionActive() || !VR::g_openxr)
    return false;

  const auto wait_begin_start = std::chrono::high_resolution_clock::now();
  if (!VR::g_openxr->PollEvents() || !VR::g_openxr->IsSessionRunning())
    return false;

  if (!VR::g_openxr->WaitFrame() || !VR::g_openxr->BeginFrame())
    return false;

  const auto wait_begin_end = std::chrono::high_resolution_clock::now();
  if (wait_frame_ms)
  {
    *wait_frame_ms =
        std::chrono::duration<double, std::milli>(wait_begin_end - wait_begin_start).count();
  }

  if (do_locate_views && VR::g_openxr->ShouldRender())
  {
    const auto locate_start = std::chrono::high_resolution_clock::now();
    if (!VR::g_openxr->LocateViews())
      return false;
    const auto locate_end = std::chrono::high_resolution_clock::now();
    if (locate_views_ms)
    {
      *locate_views_ms =
          std::chrono::duration<double, std::milli>(locate_end - locate_start).count();
    }
    auto& geometry_shader_manager = Core::System::GetInstance().GetGeometryShaderManager();
    geometry_shader_manager.SetProjectionChanged();
    // Head-pose lock: LocateViews above still refreshes m_eye_views with the latest
    // predicted pose, but we intentionally do NOT invalidate the GS cache here. The
    // cache is only invalidated by BPStructs' XFB-copy boundary (guaranteed
    // FIFO-ordered between game frames), so every draw within a single game frame
    // sees one consistent pose snapshot. The lock applies when ImmediateXFB is off
    // (VRLockHeadPosePerFrame) — a per-present invalidate would land mid-frame there.
    if (!g_ActiveConfig.VRLockHeadPosePerFrame())
      geometry_shader_manager.InvalidateVRHeadPose();
  }

  return true;
}

void Presenter::PrepareNextOpenXRFrame()
{
  if (!g_ActiveConfig.VRSessionActive() || !VR::g_openxr)
  {
    m_openxr_frame_prepared = false;
    return;
  }

  if (VR::g_openxr->IsFrameThreadActive())
  {
    // The pacing thread owns xrWaitFrame/xrBeginFrame/xrEndFrame; here we only refresh
    // the eye poses the next game frame will be rendered with.
    const auto locate_start = std::chrono::high_resolution_clock::now();
    if (VR::g_openxr->IsSessionRunning() && VR::g_openxr->ShouldRender() &&
        VR::g_openxr->LocateViews())
    {
      auto& geometry_shader_manager = Core::System::GetInstance().GetGeometryShaderManager();
      geometry_shader_manager.SetProjectionChanged();
      // Head-pose lock: see StartOpenXRFrameNow — the GS pose cache is only
      // invalidated at the XFB-copy boundary while the lock is in effect.
      if (!g_ActiveConfig.VRLockHeadPosePerFrame())
        geometry_shader_manager.InvalidateVRHeadPose();
    }
    m_last_openxr_wait_frame_ms = 0.0;
    m_last_openxr_locate_views_ms =
        std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() -
                                                  locate_start)
            .count();
  }
  else
  {
    m_openxr_frame_prepared =
        StartOpenXRFrameNow(&m_last_openxr_wait_frame_ms, &m_last_openxr_locate_views_ms);
  }

  // Rolling window: how much of the presenting (video/emu) thread each game frame
  // spends inside blocking XR calls. On single-core games this time is stolen
  // straight from emulation — the number the pacing thread exists to erase.
  m_openxr_timing_frames++;
  m_openxr_timing_wait_ms += m_last_openxr_wait_frame_ms;
  m_openxr_timing_locate_ms += m_last_openxr_locate_views_ms;
  const u64 now_us = Common::Timer::NowUs();
  if (m_openxr_timing_window_start_us == 0)
    m_openxr_timing_window_start_us = now_us;
  if (now_us - m_openxr_timing_window_start_us >= 5'000'000 && m_openxr_timing_frames > 0)
  {
    INFO_LOG_FMT(VIDEO,
                 "XRTiming(present thread): {} frames | per frame: wait+begin={:.2f}ms "
                 "locate={:.2f}ms | pacing_thread={}",
                 m_openxr_timing_frames, m_openxr_timing_wait_ms / m_openxr_timing_frames,
                 m_openxr_timing_locate_ms / m_openxr_timing_frames,
                 VR::g_openxr->IsFrameThreadActive());
    m_openxr_timing_window_start_us = now_us;
    m_openxr_timing_frames = 0;
    m_openxr_timing_wait_ms = 0.0;
    m_openxr_timing_locate_ms = 0.0;
  }

  // The VR full-EFB clear no longer happens here: with ImmediateXFB off, Present()
  // runs mid-way through the next frame's draw stream, and clearing here wiped
  // half-drawn frames (flicker unless "Don't Clear Screen" was on). It is now
  // requested at the XFB-copy boundary (BPStructs) and deferred to the next frame's
  // first draw (FramebufferManager::FlushPendingVRClearEFB).
}

bool Presenter::EnsureTabletopHandPipeline(AbstractFramebuffer* framebuffer)
{
  if (!framebuffer)
    return false;

  const AbstractTextureFormat format = framebuffer->GetColorFormat();
  const bool uses_fdm = framebuffer->HasFragmentDensityMap();
  const bool multiview = framebuffer->GetLayers() > 1;
  if (m_tabletop_hand_pipeline && m_tabletop_hand_vertex_format &&
      m_tabletop_hand_pipeline_format == format && m_tabletop_hand_pipeline_fdm == uses_fdm &&
      m_tabletop_hand_pipeline_multiview == multiview)
  {
    return true;
  }

  m_tabletop_hand_pipeline.reset();
  if (!m_tabletop_hand_vertex_format)
  {
    PortableVertexDeclaration declaration{};
    declaration.position = {ComponentFormat::Float, 4, offsetof(TabletopHandVertex, position),
                            true, false};
    declaration.texcoords[0] = {ComponentFormat::Float, 4,
                                offsetof(TabletopHandVertex, right_position), true, false};
    declaration.colors[0] = {ComponentFormat::UByte, 4, offsetof(TabletopHandVertex, color), true,
                             false};
    declaration.stride = sizeof(TabletopHandVertex);
    m_tabletop_hand_vertex_format = g_gfx->CreateNativeVertexFormat(declaration);
    if (!m_tabletop_hand_vertex_format)
      return false;
  }

  auto vertex_shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Vertex, GenerateTabletopHandVertexShader(multiview), nullptr,
      multiview ? "Quest tabletop multiview hand vertex shader" :
                  "Quest tabletop hand vertex shader");
  auto pixel_shader = g_gfx->CreateShaderFromSource(
      ShaderStage::Pixel, FramebufferShaderGen::GenerateColorPixelShader(), nullptr,
      "Quest tabletop hand pixel shader");
  if (!vertex_shader || !pixel_shader)
    return false;

  AbstractPipelineConfig config{};
  config.vertex_format = m_tabletop_hand_vertex_format.get();
  config.vertex_shader = vertex_shader.get();
  config.pixel_shader = pixel_shader.get();
  config.rasterization_state =
      RenderState::GetNoCullRasterizationState(PrimitiveType::Triangles);
  config.depth_state = RenderState::GetNoDepthTestingDepthState();
  config.blending_state = RenderState::GetNoBlendingBlendState();
  config.framebuffer_state = RenderState::GetColorFramebufferState(format);
  config.framebuffer_state.multiview = multiview ? 1 : 0;
  config.framebuffer_state.fragment_density_map = uses_fdm ? 1 : 0;
  config.usage = AbstractPipelineUsage::Utility;
  m_tabletop_hand_pipeline = g_gfx->CreatePipeline(config);
  if (!m_tabletop_hand_pipeline)
    return false;

  m_tabletop_hand_pipeline_format = format;
  m_tabletop_hand_pipeline_fdm = uses_fdm;
  m_tabletop_hand_pipeline_multiview = multiview;
  return true;
}

void Presenter::DrawTabletopHands(AbstractFramebuffer* framebuffer, uint32_t eye_index)
{
  if (!framebuffer || !VR::g_openxr || !VR::g_openxr->IsTabletopModeActive() ||
      !VR::g_openxr->IsSessionRunning())
  {
    return;
  }

  const Common::VR::OpenXRInputSnapshot input = Common::VR::OpenXRInputState::GetSnapshot();
  std::vector<HandRoomVertex> room_vertices;
  room_vertices.reserve(12000);
  for (int hand = 0; hand < 2; ++hand)
  {
    const auto& controller = input.controllers[hand];
    const VR::TabletopHandMesh* runtime_mesh = VR::g_openxr->GetTabletopHandMesh(hand);
    if (runtime_mesh)
      BuildRuntimeControllerHand(*runtime_mesh, controller, hand, &room_vertices);
  }
  if (room_vertices.empty())
    return;

  VR::TabletopOcclusionPlane tabletop_plane{};
  if (VR::g_openxr->GetTabletopOcclusionPlane(&tabletop_plane))
  {
    std::vector<HandRoomVertex> clipped_vertices;
    clipped_vertices.reserve(room_vertices.size() * 4 / 3);
    for (size_t i = 0; i + 2 < room_vertices.size(); i += 3)
    {
      const std::array<HandRoomVertex, 3> triangle = {
          room_vertices[i + 0], room_vertices[i + 1], room_vertices[i + 2]};
      AppendTabletopClippedHandTriangle(&clipped_vertices, triangle, tabletop_plane);
    }
    room_vertices.swap(clipped_vertices);
    if (room_vertices.empty())
      return;
  }

  const bool multiview = framebuffer->GetLayers() > 1;
  if (!multiview && eye_index >= 2)
    return;

  const auto& eye_views = VR::g_openxr->GetPresentEyeViews();
  std::vector<TabletopHandVertex> projected;
  projected.reserve(room_vertices.size());

  // Geometry builders emit complete triangles. In layered multiview mode every vertex carries
  // both eye projections and gl_ViewIndex selects the matching one in the vertex shader. This
  // keeps Quest on its fast single-pass Vulkan path even while articulated hands are visible.
  for (size_t i = 0; i + 2 < room_vertices.size(); i += 3)
  {
    std::array<HandVec3, 3> left_ndc{};
    std::array<HandVec3, 3> right_ndc{};
    bool triangle_valid = true;
    for (size_t v = 0; v < 3; ++v)
    {
      const uint32_t primary_eye = multiview ? 0 : eye_index;
      if (!ProjectHandPointToEye(eye_views[primary_eye], room_vertices[i + v].position,
                                 &left_ndc[v]))
      {
        triangle_valid = false;
        break;
      }
      if (multiview)
      {
        if (!ProjectHandPointToEye(eye_views[1], room_vertices[i + v].position, &right_ndc[v]))
        {
          triangle_valid = false;
          break;
        }
      }
      else
      {
        right_ndc[v] = left_ndc[v];
      }
    }
    if (!triangle_valid)
      continue;

    for (size_t v = 0; v < 3; ++v)
    {
      TabletopHandVertex vertex{};
      vertex.position[0] = left_ndc[v][0];
      vertex.position[1] = left_ndc[v][1];
      vertex.position[2] = 0.0f;
      vertex.position[3] = 1.0f;
      vertex.right_position[0] = right_ndc[v][0];
      vertex.right_position[1] = right_ndc[v][1];
      vertex.right_position[2] = 0.0f;
      vertex.right_position[3] = 1.0f;
      vertex.color = room_vertices[i + v].color;
      projected.push_back(vertex);
    }
  }

  if (projected.empty() || !EnsureTabletopHandPipeline(framebuffer))
    return;

  g_gfx->BeginUtilityDrawing();
  g_gfx->SetFramebuffer(framebuffer);
  g_gfx->SetViewportAndScissor(framebuffer->GetRect());
  g_gfx->SetPipeline(m_tabletop_hand_pipeline.get());

  u32 base_vertex = 0;
  u32 base_index = 0;
  g_vertex_manager->UploadUtilityVertices(projected.data(), sizeof(TabletopHandVertex),
                                          static_cast<u32>(projected.size()), nullptr, 0,
                                          &base_vertex, &base_index);
  g_gfx->Draw(base_vertex, static_cast<u32>(projected.size()));
  g_gfx->EndUtilityDrawing();
}

void Presenter::BlitCurrentSourceToOpenXREyes(const AbstractTexture* source_texture,
                                              const MathUtil::Rectangle<int>& source_rc)
{
  if (!source_texture || !VR::g_openxr || !VR::g_openxr->GetSwapchain() ||
      !VR::g_openxr->ShouldRender())
  {
    return;
  }
  if (source_texture->GetLayers() < 2)
    return;

  VR::IOpenXRSwapchain* sc = VR::g_openxr->GetSwapchain();
  AbstractFramebuffer* saved_fb = g_gfx->GetCurrentFramebuffer();

  const float vr_gamma = g_ActiveConfig.vr_gamma;
  const bool apply_gamma = vr_gamma > 1.01f;
  bool saved_correct_gamma = false;
  bool saved_sdr_gamma_srgb = false;
  float saved_custom_gamma = 2.2f;
  if (apply_gamma)
  {
    saved_correct_gamma = g_ActiveConfig.color_correction.bCorrectGamma;
    saved_sdr_gamma_srgb = g_ActiveConfig.color_correction.bSDRDisplayGammaSRGB;
    saved_custom_gamma = g_ActiveConfig.color_correction.fSDRDisplayCustomGamma;
    g_ActiveConfig.color_correction.bCorrectGamma = true;
    g_ActiveConfig.color_correction.bSDRDisplayGammaSRGB = false;
    g_ActiveConfig.color_correction.fSDRDisplayCustomGamma = vr_gamma;
  }

  auto restore_post_process_state = [&] {
    if (apply_gamma)
    {
      g_ActiveConfig.color_correction.bCorrectGamma = saved_correct_gamma;
      g_ActiveConfig.color_correction.bSDRDisplayGammaSRGB = saved_sdr_gamma_srgb;
      g_ActiveConfig.color_correction.fSDRDisplayCustomGamma = saved_custom_gamma;
    }

    if (saved_fb)
      g_gfx->SetFramebuffer(saved_fb);
  };

  const MathUtil::Rectangle<int> eye_rect{
      0, 0, static_cast<int>(sc->GetEyeWidth()), static_cast<int>(sc->GetEyeHeight())};
  // With passthrough on, anything the game leaves transparent must stay transparent in
  // the OpenXR swapchain so the compositor can show the camera feed there.
  const bool tabletop_passthrough =
      VR::g_openxr->IsTabletopModeActive() && g_ActiveConfig.VRPassthroughEnabled();
  const float clear_alpha = tabletop_passthrough ? 0.0f : 1.0f;
  const Common::VR::OpenXRInputSnapshot hand_input = Common::VR::OpenXRInputState::GetSnapshot();
  const bool tabletop_hands_active =
      VR::g_openxr->IsTabletopModeActive() &&
      ((hand_input.controllers[0].connected && hand_input.controllers[0].grip_pose.valid) ||
       (hand_input.controllers[1].connected && hand_input.controllers[1].grip_pose.valid));

  // Keep Quest on the fast layered Vulkan multiview path even when the skinned runtime hands are
  // visible. DrawTabletopHands packs both eye projections into each vertex and gl_ViewIndex picks
  // the correct one, so hands no longer force two full per-eye blits.
  if (sc->SupportsLayeredRendering() && m_post_processor->CanBlitFromTextureLayered())
  {
    AbstractFramebuffer* layered_fb = sc->AcquireLayeredFramebuffer();
    if (layered_fb)
    {
      g_gfx->SetAndClearFramebuffer(layered_fb, {0.f, 0.f, 0.f, clear_alpha});
      if (m_post_processor->BlitFromTextureLayered(eye_rect, source_rc, source_texture))
      {
        if (tabletop_hands_active)
          DrawTabletopHands(layered_fb, 2);
        sc->ReleaseLayeredTexture();
        restore_post_process_state();
        return;
      }

      static bool s_logged_layered_blit_fallback = false;
      if (!s_logged_layered_blit_fallback)
      {
        WARN_LOG_FMT(VIDEO,
                     "OpenXR: layered post-process blit failed; falling back to "
                     "per-eye swapchains.");
        s_logged_layered_blit_fallback = true;
      }
      sc->ReleaseLayeredTexture();
    }
  }

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    AbstractFramebuffer* eye_fb = sc->AcquireEyeFramebuffer(eye);
    if (!eye_fb)
      continue;

    // Replay submits must also populate the acquired OpenXR image; binding it without a blit can
    // leak stale swapchain contents from an older frame.
    g_gfx->SetAndClearFramebuffer(eye_fb, {0.f, 0.f, 0.f, clear_alpha});
    m_post_processor->BlitFromTexture(eye_rect, source_rc, source_texture, static_cast<int>(eye));
    if (tabletop_hands_active)
      DrawTabletopHands(eye_fb, eye);
    sc->ReleaseEyeTexture(eye);
  }

  restore_post_process_state();
}

bool Presenter::IsOpenXRFlat() const
{
  return VR::g_openxr && g_ActiveConfig.vr_flat_screen &&
         g_ActiveConfig.stereo_mode != StereoMode::OpenXR;
}

void Presenter::BlitCurrentSourceToOpenXRFlat(const AbstractTexture* source_texture,
                                              const MathUtil::Rectangle<int>& source_rc)
{
  if (!source_texture || !VR::g_openxr || !VR::g_openxr->GetSwapchain() ||
      !VR::g_openxr->ShouldRender())
  {
    return;
  }

  VR::IOpenXRSwapchain* sc = VR::g_openxr->GetSwapchain();
  AbstractFramebuffer* flat_fb = sc->AcquireFlatFramebuffer();
  if (!flat_fb)
    return;

  AbstractFramebuffer* saved_fb = g_gfx->GetCurrentFramebuffer();

  // The game aspect drives the world size of the quad; the mono frame is stretched to fill the
  // (roughly square) eye-0 image, and the non-square quad undoes the stretch.
  const int src_w = source_rc.GetWidth();
  const int src_h = source_rc.GetHeight();
  if (src_w > 0 && src_h > 0)
    VR::g_openxr->SetFlatScreenAspect(static_cast<float>(src_w) / static_cast<float>(src_h));

  const MathUtil::Rectangle<int> flat_rect{
      0, 0, static_cast<int>(sc->GetEyeWidth()), static_cast<int>(sc->GetEyeHeight())};
  g_gfx->SetAndClearFramebuffer(flat_fb, {0.f, 0.f, 0.f, 1.f});
  m_post_processor->BlitFromTexture(flat_rect, source_rc, source_texture, 0);
  sc->ReleaseFlatTexture();

  if (saved_fb)
    g_gfx->SetFramebuffer(saved_fb);
}

bool Presenter::SubmitOpenXRFrameFromCurrentSource(const AbstractTexture* source_texture,
                                                   const MathUtil::Rectangle<int>& source_rc,
                                                   bool blit_source)
{
  if (!VR::g_openxr)
    return false;

  if (VR::IOpenXRSwapchain* sc = VR::g_openxr->GetSwapchain())
  {
    // Bracket the eye-image release (inside the blit) and the pose publish (inside
    // SubmitFrame) as one unit so the pacing thread's eager heartbeat can't fire between
    // them and submit a new image with a stale pose. No-op when the pacing thread is off.
    VR::OpenXRManager::ScopedVideoFrameHandoff handoff(
        VR::g_openxr->IsFrameThreadActive() ? VR::g_openxr.get() : nullptr);

    if (IsOpenXRFlat())
    {
      // Flat mode always (re)blits at submit time: on PC the earlier RenderXFBToScreen path
      // only mirrors to the desktop window, and on Quest the direct-to-HMD branch skips the
      // stereo eye blit for flat mode.
      BlitCurrentSourceToOpenXRFlat(source_texture, source_rc);
      return sc->SubmitFlatFrame();
    }

    if (blit_source)
      BlitCurrentSourceToOpenXREyes(source_texture, source_rc);
    return sc->SubmitFrame();
  }

  // No swapchain: with the pacing thread active it heartbeats empty frames itself;
  // the legacy flow must close the begun frame here.
  if (VR::g_openxr->IsFrameThreadActive())
    return true;
  return VR::g_openxr->EndFrame({});
}


#endif

void Presenter::RenderXFBToScreen(const MathUtil::Rectangle<int>& target_rc,
                                  const AbstractTexture* source_texture,
                                  const MathUtil::Rectangle<int>& source_rc)
{
  if (g_ActiveConfig.stereo_mode == StereoMode::QuadBuffer &&
      g_backend_info.bUsesExplictQuadBuffering)
  {
    // Quad-buffered stereo is annoying on GL.
    g_gfx->SelectLeftBuffer();
    m_post_processor->BlitFromTexture(target_rc, source_rc, source_texture, 0);

    g_gfx->SelectRightBuffer();
    m_post_processor->BlitFromTexture(target_rc, source_rc, source_texture, 1);

    g_gfx->SelectMainBuffer();
  }
  else if (g_ActiveConfig.stereo_mode == StereoMode::SBS ||
           g_ActiveConfig.stereo_mode == StereoMode::TAB)
  {
    const auto [left_rc, right_rc] = ConvertStereoRectangle(target_rc);

    m_post_processor->BlitFromTexture(left_rc, source_rc, source_texture, 0);
    m_post_processor->BlitFromTexture(right_rc, source_rc, source_texture, 1);
  }
#ifdef ENABLE_VR
  else if (g_ActiveConfig.stereo_mode == StereoMode::OpenXR)
  {
    // Mirror view on the desktop window.
    switch (g_ActiveConfig.vr_mirror_view)
    {
    case OpenXRMirrorView::BothEyes:
    {
      const auto [left_rc, right_rc] = ConvertStereoRectangle(target_rc);
      m_post_processor->BlitFromTexture(left_rc, source_rc, source_texture, 0);
      m_post_processor->BlitFromTexture(right_rc, source_rc, source_texture, 1);
      break;
    }
    case OpenXRMirrorView::LeftEye:
      m_post_processor->BlitFromTexture(target_rc, source_rc, source_texture, 0);
      break;
    case OpenXRMirrorView::RightEye:
      m_post_processor->BlitFromTexture(target_rc, source_rc, source_texture, 1);
      break;
    case OpenXRMirrorView::None:
      break;
    }

    // Eye blit. With the pacing thread active, defer it to submit time (below) so the
    // swapchain-image release ends up adjacent to the pose publish — otherwise the
    // release and publish straddle DrawImGui + PresentBackbuffer, and an eager heartbeat
    // firing in that gap submits the new image with the previous pose (stutter).
    if (!VR::g_openxr || !VR::g_openxr->IsFrameThreadActive())
      BlitCurrentSourceToOpenXREyes(source_texture, source_rc);
  }
#endif
  // Every other case will be treated the same (stereo or not).
  // If there's multiple source layers, they should all be copied.
  else
  {
    m_post_processor->BlitFromTexture(target_rc, source_rc, source_texture);
  }
}

void Presenter::Present(PresentInfo* present_info)
{
  m_present_count++;

#ifdef ENABLE_VR
  // See Initialize(): the Android GLES OpenXR path runs with a headless GL context, but
  // Present() must still run — it pumps the XR event loop and performs the eye blits.
  const bool xr_headless_present = g_ActiveConfig.VRSessionActive() &&
                                   g_backend_info.api_type == APIType::OpenGL;
#else
  constexpr bool xr_headless_present = false;
#endif
  if ((g_gfx->IsHeadless() && !xr_headless_present) || (!m_onscreen_ui && !m_xfb_entry))
    return;

  if (!g_gfx->SupportsUtilityDrawing())
  {
    // Video Software doesn't support drawing a UI or doing post-processing
    // So just show the XFB
    if (m_xfb_entry)
    {
      g_gfx->ShowImage(m_xfb_entry->texture.get(), m_xfb_rect);

      // Update the window size based on the frame that was just rendered.
      // Due to depending on guest state, we need to call this every frame.
      SetSuggestedWindowSize(m_xfb_rect.GetWidth(), m_xfb_rect.GetHeight());
    }
    return;
  }

  // Since we use the common pipelines here and draw vertices if a batch is currently being
  // built by the vertex loader, we end up trampling over its pointer, as we share the buffer
  // with the loader, and it has not been unmapped yet. Force a pipeline flush to avoid this.
  g_vertex_manager->Flush();

  UpdateDrawRectangle();

#ifdef ENABLE_VR
  // OpenXR frame lifecycle.
  //
  // Pacing thread active (default): the thread owns WaitFrame/BeginFrame/EndFrame and
  // heartbeats the last frame at HMD cadence; Present() only blits the eyes and
  // publishes the new layers. Nothing here blocks on the XR runtime.
  //
  // Legacy flow (UseXRPacingThread = false): frame N was pre-begun at the end of
  // Present(N-1) so BeginFrame→EndFrame brackets the full game render time; the blits
  // and EndFrame run inline here.
#if defined(ANDROID)
  const bool openxr_direct_to_hmd = g_ActiveConfig.VRSessionActive() &&
                                    VR::g_openxr && g_ActiveConfig.vr_android_direct_to_hmd;
#else
  constexpr bool openxr_direct_to_hmd = false;
#endif
  bool vr_frame_started = false;
  if (g_ActiveConfig.VRSessionActive() && VR::g_openxr)
  {
    if (VR::g_openxr->IsFrameThreadActive())
    {
      vr_frame_started = VR::g_openxr->IsSessionRunning();
    }
    else
    {
      vr_frame_started = m_openxr_frame_prepared;
      if (!vr_frame_started)
      {
        vr_frame_started = StartOpenXRFrameNow();
        m_openxr_frame_prepared = vr_frame_started;
      }
    }
  }

  // VI duplicate presents re-show an XFB the pacing thread is already heartbeating
  // with its stamped pose; re-blitting and re-publishing the identical content only
  // adds GPU work and reopens pose/content pairing races. The legacy (inline) flow
  // cannot skip — its pre-begun XR frame must still be ended below.
  const bool vr_skip_duplicate_publish =
      vr_frame_started && present_info != nullptr &&
      present_info->reason == PresentInfo::PresentReason::VideoInterfaceDuplicate &&
      VR::g_openxr && VR::g_openxr->IsFrameThreadActive();
#endif
#ifndef ENABLE_VR
  constexpr bool openxr_direct_to_hmd = false;
#endif

  g_gfx->BeginUtilityDrawing();
  const bool backbuffer_bound =
      !openxr_direct_to_hmd && g_gfx->BindBackbuffer({{0.0f, 0.0f, 0.0f, 1.0f}});

  // Render the XFB to the screen.
  if (backbuffer_bound && m_xfb_entry)
  {
    // Adjust the source rectangle instead of using an oversized viewport to render the XFB.
    auto render_target_rc = GetTargetRectangle();
    auto render_source_rc = m_xfb_rect;
    AdjustRectanglesToFitBounds(&render_target_rc, &render_source_rc, m_backbuffer_width,
                                m_backbuffer_height);
    RenderXFBToScreen(render_target_rc, m_xfb_entry->texture.get(), render_source_rc);
  }
#ifdef ENABLE_VR
  else if (openxr_direct_to_hmd && vr_frame_started && m_xfb_entry && !IsOpenXRFlat() &&
           !vr_skip_duplicate_publish)
  {
    // Stereo direct-to-HMD path. Flat mode blits at submit time
    // (SubmitOpenXRFrameFromCurrentSource) instead, so it is skipped here.
    auto replay_target_rc = GetTargetRectangle();
    auto replay_source_rc = m_xfb_rect;
    AdjustRectanglesToFitBounds(&replay_target_rc, &replay_source_rc, m_backbuffer_width,
                                m_backbuffer_height);
    BlitCurrentSourceToOpenXREyes(m_xfb_entry->texture.get(), replay_source_rc);
  }
#endif

  if (m_onscreen_ui)
  {
    m_onscreen_ui->Finalize();
    if (backbuffer_bound)
      m_onscreen_ui->DrawImGui();
  }

  // Present to the window system.
  {
    std::lock_guard<std::mutex> guard(m_swap_mutex);

    if (present_info != nullptr && !g_ActiveConfig.VRSessionActive())
    {
      const auto present_time = GetUpdatedPresentationTime(present_info->intended_present_time);

      Core::System::GetInstance().GetCoreTiming().SleepUntil(present_time);

      // Perhaps in the future a more accurate time can be acquired from the various backends.
      present_info->actual_present_time = Clock::now();
      present_info->present_time_accuracy = PresentInfo::PresentTimeAccuracy::PresentInProgress;
    }

    if (!openxr_direct_to_hmd)
      g_gfx->PresentBackbuffer();
  }

#ifdef ENABLE_VR
  if (vr_frame_started)
  {
    if (!vr_skip_duplicate_publish)
    {
      // Pacing thread active: the eye blit was deferred from RenderXFBToScreen to here so
      // its swapchain release sits right next to the pose publish (both inside the handoff
      // bracket in SubmitOpenXRFrameFromCurrentSource). Legacy flow already blit inline, so
      // it only publishes/ends here. Flat and direct-to-HMD manage their own blit.
      const bool blit_at_submit = VR::g_openxr && VR::g_openxr->IsFrameThreadActive() &&
                                  !openxr_direct_to_hmd && !IsOpenXRFlat();
      SubmitOpenXRFrameFromCurrentSource(m_xfb_entry ? m_xfb_entry->texture.get() : nullptr,
                                         m_xfb_rect, blit_at_submit);
    }
    m_openxr_frame_prepared = false;
  }
#endif

  if (m_xfb_entry)
  {
    // Update the window size based on the frame that was just rendered.
    // Due to depending on guest state, we need to call this every frame.
    SetSuggestedWindowSize(m_xfb_rect.GetWidth(), m_xfb_rect.GetHeight());
  }

  if (m_onscreen_ui)
    m_onscreen_ui->BeginImGuiFrame(m_backbuffer_width, m_backbuffer_height);

  g_gfx->EndUtilityDrawing();

#ifdef ENABLE_VR
  // Refresh eye poses for the next game frame (and pre-begin the next XR frame on the
  // legacy flow), then clear the EFB so the frame starts on a clean slate.
  PrepareNextOpenXRFrame();
#endif
}

TimePoint Presenter::GetUpdatedPresentationTime(TimePoint intended_presentation_time)
{
  const auto now = Clock::now();
  const auto arrival_offset = std::min(now - intended_presentation_time, DT{});

  if (!Config::Get(Config::MAIN_SMOOTH_EARLY_PRESENTATION))
  {
    m_presentation_time_offset = arrival_offset;

    // When SmoothEarlyPresentation is off and ImmediateXFB or RushFramePresentation are on,
    //  present as soon as possible as the goal is to achieve low input latency.
    if (g_ActiveConfig.bImmediateXFB || Config::Get(Config::MAIN_RUSH_FRAME_PRESENTATION))
      return now;

    return intended_presentation_time;
  }

  // Adjust slowly backward in time but quickly forward in time.
  // This keeps the pacing moderately smooth even if games produce regular sporadic bumps.
  // This was tuned to handle the terrible pacing in Brawl with "Immediate XFB".
  // Super Mario Galaxy 1 + 2 still perform poorly here in SingleCore mode.
  const auto adjustment_divisor = (arrival_offset < m_presentation_time_offset) ? 100 : 2;

  m_presentation_time_offset += (arrival_offset - m_presentation_time_offset) / adjustment_divisor;

  return intended_presentation_time + m_presentation_time_offset;
}

void Presenter::SetKeyMap(const DolphinKeyMap& key_map)
{
  if (m_onscreen_ui)
    m_onscreen_ui->SetKeyMap(key_map);
}

void Presenter::SetKey(u32 key, bool is_down, const char* chars)
{
  if (m_onscreen_ui)
    m_onscreen_ui->SetKey(key, is_down, chars);
}

void Presenter::SetMousePos(float x, float y)
{
  if (m_onscreen_ui)
    m_onscreen_ui->SetMousePos(x, y);
}

void Presenter::SetMousePress(u32 button_mask)
{
  if (m_onscreen_ui)
    m_onscreen_ui->SetMousePress(button_mask);
}

void Presenter::DoState(PointerWrap& p)
{
  p.Do(m_frame_count);
  p.Do(m_last_xfb_ticks);
  p.Do(m_last_xfb_addr);
  p.Do(m_last_xfb_width);
  p.Do(m_last_xfb_stride);
  p.Do(m_last_xfb_height);

  // If we're loading and there is a last XFB, re-display it.
  if (p.IsReadMode() && m_last_xfb_stride != 0)
  {
    // This technically counts as the end of the frame
    GetVideoEvents().after_frame_event.Trigger(Core::System::GetInstance());

    m_next_swap_estimated_ticks = m_last_xfb_ticks;
    m_next_swap_estimated_time = Clock::now();

    m_immediate_swap_happened_this_field.store(false, std::memory_order_relaxed);

    ImmediateSwap(m_last_xfb_addr, m_last_xfb_width, m_last_xfb_stride, m_last_xfb_height);
  }
}

}  // namespace VideoCommon
