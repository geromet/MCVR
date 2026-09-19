#include "harness.hpp"

// Pins the CPU-side layout of structs in common/shared.hpp, which is included by GLSL too and
// uploaded verbatim to GPU buffers. An accidental field add/reorder/type change breaks the ABI with
// the shaders silently; these tests make it loud. If you change a struct on purpose, update the
// goldens here AND the shader-side consumers.
#include "common/shared.hpp"

#include <cstddef>
#include <type_traits>

using namespace vk;

#define LAYOUT(T, size)                                                                                                \
    CHECK_EQ(sizeof(T), (size_t)(size));                                                                               \
    CHECK(std::is_standard_layout_v<T>);                                                                               \
    CHECK(std::is_trivially_copyable_v<T>)

TEST(layout_vertex_formats_are_tightly_packed_words) {
    // Every vertex format is a sequence of 4-byte words with no implicit padding.
    CHECK_EQ(sizeof(VertexFormat::Triangle) % 4, (size_t)0);
    LAYOUT(VertexFormat::Triangle, 24);
    LAYOUT(VertexFormat::TexturedTriangle, 20);
    LAYOUT(VertexFormat::PositionOnly, 12);
    LAYOUT(VertexFormat::PositionTexColor, 24);
    LAYOUT(VertexFormat::PositionColor, 16);
    LAYOUT(VertexFormat::PositionColorNormal, 20);
    LAYOUT(VertexFormat::PositionTex, 20);
    LAYOUT(VertexFormat::PositionColorTexLight, 28);
    LAYOUT(VertexFormat::PositionColorLight, 20);
    LAYOUT(VertexFormat::PositionTexColorLight, 28);
    LAYOUT(VertexFormat::PositionTexColorNormal, 28);
    LAYOUT(VertexFormat::PositionTexLightColor, 28);
    LAYOUT(VertexFormat::PositionColorTexLightNormal, 32);
    LAYOUT(VertexFormat::PositionColorTexOverlayLightNormal, 36);
    LAYOUT(VertexFormat::ArrayTexturedTriangle, 64);
}

TEST(layout_vertex_offsets) {
    using PV = VertexFormat::PositionColorTexLightNormal;
    CHECK_EQ(offsetof(PV, color), (size_t)12);
    CHECK_EQ(offsetof(PV, uv0), (size_t)16);
    CHECK_EQ(offsetof(PV, uv2), (size_t)24);
    CHECK_EQ(offsetof(PV, normal), (size_t)28);
}

TEST(layout_gpu_vertex_buffers_are_16_byte_multiples) {
    LAYOUT(VertexFormat::PBRVertex, 128);
    LAYOUT(VertexFormat::PositionVertex, 16);
    LAYOUT(VertexFormat::MaterialVertex, 80);
    CHECK_EQ(sizeof(VertexFormat::PBRVertex) % 16, (size_t)0);
    CHECK_EQ(sizeof(VertexFormat::PositionVertex) % 16, (size_t)0);
    CHECK_EQ(sizeof(VertexFormat::MaterialVertex) % 16, (size_t)0);
}

TEST(layout_ubo_structs_are_16_byte_multiples) {
    LAYOUT(Data::Camera, 272);
    LAYOUT(Data::DirectionalLight, 32);
    LAYOUT(Data::World, 40);
    LAYOUT(Data::OverlayPostUBO, 96);
    LAYOUT(Data::WorldUBO, 592);
    LAYOUT(Data::SkyUBO, 80);
    LAYOUT(Data::LightMapUBO, 48);
    CHECK_EQ(sizeof(Data::Camera) % 16, (size_t)0);
    CHECK_EQ(sizeof(Data::DirectionalLight) % 16, (size_t)0);
    CHECK_EQ(sizeof(Data::OverlayPostUBO) % 16, (size_t)0);
    CHECK_EQ(sizeof(Data::WorldUBO) % 16, (size_t)0);
    CHECK_EQ(sizeof(Data::SkyUBO) % 16, (size_t)0);
    CHECK_EQ(sizeof(Data::LightMapUBO) % 16, (size_t)0);
}

TEST(layout_storage_buffer_structs) {
    LAYOUT(Data::TextureMapEntry, 12);
    LAYOUT(Data::TextureMapping, 12 * 4096);
    LAYOUT(Data::ExposureData, 32 + 256 * 4);
    CHECK_EQ(offsetof(Data::ExposureData, bins), (size_t)32);
}

TEST(layout_worldubo_dvec4_fields_are_8_byte_aligned) {
    CHECK_EQ(offsetof(Data::WorldUBO, cameraPos) % 8, (size_t)0);
    CHECK_EQ(offsetof(Data::WorldUBO, chunkGridInfo) % 16, (size_t)0);
    CHECK_EQ(offsetof(Data::WorldUBO, chunkStorageSectionPos) % 16, (size_t)0);
}

TEST(shared_constants) {
    CHECK(PI > 3.14159 && PI < 3.14160);
    CHECK(INV_PI * PI > 0.9999999 && INV_PI * PI < 1.0000001);
    CHECK(TWO_PI == 2.0 * PI);
    CHECK(INV_TWO_PI * TWO_PI > 0.9999999 && INV_TWO_PI * TWO_PI < 1.0000001);
    CHECK(INV_4_PI * 4.0 * PI > 0.9999999 && INV_4_PI * 4.0 * PI < 1.0000001);
}
