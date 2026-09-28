//===----------------------------------------------------------------------===//
// SpirvReflectTest.cpp
// Hermetic tests for the SPIR-V reflection parser.
//
// The parser walks a raw binary word stream, so the two things worth testing are
// (a) that well-formed modules reflect to exactly the right descriptors, and
// (b) that malformed input is rejected or ignored without reading out of bounds.
// The modules below are assembled word-by-word so both are checked precisely, with
// no dependency on glslang being installed.
//===----------------------------------------------------------------------===//

#include "csrun/compiler/SpirvReflect.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace {

using csrun::SpirvDescriptorKind;
using csrun::SpirvReflectStage;

// Opcodes / decorations mirrored from the parser's subset.
constexpr std::uint16_t OpTypeInt = 21;
constexpr std::uint16_t OpTypeFloat = 22;
constexpr std::uint16_t OpTypeVector = 23;
constexpr std::uint16_t OpTypeMatrix = 24;
constexpr std::uint16_t OpTypeArray = 28;
constexpr std::uint16_t OpTypeImage = 25;
constexpr std::uint16_t OpTypeSampledImage = 27;
constexpr std::uint16_t OpTypeRuntimeArray = 29;
constexpr std::uint16_t OpTypeStruct = 30;
constexpr std::uint16_t OpTypePointer = 32;
constexpr std::uint16_t OpConstant = 43;
constexpr std::uint16_t OpVariable = 59;
constexpr std::uint16_t OpDecorate = 71;
constexpr std::uint16_t OpMemberDecorate = 72;
constexpr std::uint16_t OpCapability = 17;

constexpr std::uint32_t DecBlock = 2;
constexpr std::uint32_t DecOffset = 35;
constexpr std::uint32_t DecLocation = 30;
constexpr std::uint32_t DecBinding = 33;
constexpr std::uint32_t DecDescriptorSet = 34;
constexpr std::uint32_t DecInputAttachmentIndex = 43;

constexpr std::uint32_t SCUniform = 2;
constexpr std::uint32_t SCInput = 1;
constexpr std::uint32_t SCPushConstant = 9;
constexpr std::uint32_t SCStorageBuffer = 12;
constexpr std::uint32_t SCUniformConstant = 0;

/// Assembles a SPIR-V module word stream with a valid 5-word header.
///
/// Decorations are queued separately from types/variables because the SPIR-V logical
/// layout places the annotation section *before* types and global variables. The parser
/// is a single pass, so a fixture that emitted `OpDecorate` after `OpVariable` would
/// model a module that no real compiler produces.
class SpvBuilder {
public:
  SpvBuilder() : header_{0x07230203u, 0x00010000u, 0u, 128u, 0u} {}

  /// A types/constants/global-variables instruction.
  void op(std::uint16_t opcode, std::initializer_list<std::uint32_t> operands) {
    body_.push_back((static_cast<std::uint32_t>(1 + operands.size()) << 16) | opcode);
    for (const std::uint32_t v : operands)
      body_.push_back(v);
  }

  /// An annotation instruction (OpDecorate / OpMemberDecorate) -- emitted before `op`.
  void annot(std::uint16_t opcode, std::initializer_list<std::uint32_t> operands) {
    annots_.push_back((static_cast<std::uint32_t>(1 + operands.size()) << 16) | opcode);
    for (const std::uint32_t v : operands)
      annots_.push_back(v);
  }

  /// Appends a raw word verbatim, into whichever section it lands in.
  void raw(std::uint32_t w) { body_.push_back(w); }

  std::vector<std::uint32_t> words() const {
    std::vector<std::uint32_t> out = header_;
    out.insert(out.end(), annots_.begin(), annots_.end());
    out.insert(out.end(), body_.begin(), body_.end());
    return out;
  }

  void setHeaderWord(std::size_t i, std::uint32_t v) { header_[i] = v; }

private:
  std::vector<std::uint32_t> header_;
  std::vector<std::uint32_t> annots_;
  std::vector<std::uint32_t> body_;
};

/// A compute/fragment module with three storage buffers at set 0, bindings 0..2.
SpvBuilder threeStorageBuffers() {
  SpvBuilder b;
  for (std::uint32_t i = 0; i < 3; ++i) {
    b.annot(OpDecorate, {10 + i, DecBinding, i});
    b.annot(OpDecorate, {10 + i, DecDescriptorSet, 0});
  }
  b.annot(OpMemberDecorate, {2, 0, DecOffset, 0});
  b.op(OpTypeFloat, {1, 32});                    // %1 = float
  b.op(OpTypeStruct, {2, 1});                   // %2 = struct { float }
  b.op(OpTypePointer, {3, SCStorageBuffer, 2}); // %3 = ptr StorageBuffer %2
  for (std::uint32_t i = 0; i < 3; ++i)
    b.op(OpVariable, {3, 10 + i, SCStorageBuffer});
  return b;
}

} // namespace

//===----------------------------------------------------------------------===//
// Malformed / untrusted input
//===----------------------------------------------------------------------===//

TEST(SpirvReflectTest, RejectsEmptyInput) {
  const std::vector<std::uint32_t> words;
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  EXPECT_TRUE(r.hasError());
  EXPECT_EQ(r.error().code, csrun::ErrorCode::CompilationFailed);
}

TEST(SpirvReflectTest, RejectsTruncatedHeader) {
  // Valid magic but fewer than the 5 header words.
  const std::vector<std::uint32_t> words{0x07230203u, 0x00010000u, 0u};
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  EXPECT_TRUE(r.hasError());
}

TEST(SpirvReflectTest, RejectsBadMagic) {
  SpvBuilder b = threeStorageBuffers();
  b.setHeaderWord(0, 0xdeadbeefu);
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  EXPECT_TRUE(r.hasError());
  EXPECT_EQ(r.error().code, csrun::ErrorCode::CompilationFailed);
}

TEST(SpirvReflectTest, StopsAtZeroWordCountInsteadOfLooping) {
  // WordCount == 0 would otherwise advance the cursor by zero and spin forever.
  SpvBuilder b = threeStorageBuffers();
  b.raw((0u << 16) | OpDecorate);
  b.raw(1);
  b.raw(DecLocation);
  b.raw(0);
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  ASSERT_TRUE(r.hasValue());
  // Parsing stopped at the zero-count instruction, so nothing after it was reached.
  EXPECT_EQ(r.value().descriptors.size(), 3u);
}

TEST(SpirvReflectTest, StopsAtTruncatedTrailingInstruction) {
  SpvBuilder b = threeStorageBuffers();
  // Claim 64 words but supply only 2 -- must not read past the end.
  b.raw((64u << 16) | OpDecorate);
  b.raw(1);
  b.raw(DecBinding);
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  ASSERT_TRUE(r.hasValue());
  EXPECT_EQ(r.value().descriptors.size(), 3u);
}

TEST(SpirvReflectTest, IgnoresInstructionsShorterThanTheirOpcodeNeeds) {
  // OpTypeFloat needs 3 words; hand it 1 and it must be skipped, not read OOB.
  SpvBuilder b;
  b.raw((1u << 16) | OpTypeFloat);
  b.raw(1);
  b.raw((1u << 16) | OpTypeStruct);
  b.raw(2);
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  ASSERT_TRUE(r.hasValue());
  EXPECT_TRUE(r.value().descriptors.empty());
}

TEST(SpirvReflectTest, SurvivesUnknownOpcodes) {
  SpvBuilder b = threeStorageBuffers();
  b.raw((1u << 16) | 4242u); // not in the parser's subset
  b.raw(0xffffffu);
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  ASSERT_TRUE(r.hasValue());
  EXPECT_EQ(r.value().descriptors.size(), 3u);
}

TEST(SpirvReflectTest, DanglingTypeReferencesDoNotCrash) {
  // OpVariable and OpTypeStruct reference ids that are never defined.
  SpvBuilder b;
  b.op(OpTypePointer, {3, SCStorageBuffer, 77});
  b.op(OpVariable, {3, 10, SCStorageBuffer});
  b.annot(OpDecorate, {10, DecBinding, 0});
  b.op(OpTypeStruct, {2, 99, 98, 97});
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  ASSERT_TRUE(r.hasValue());
  // The binding is reflected, but its size cannot be computed from missing types.
  ASSERT_EQ(r.value().descriptors.size(), 1u);
  EXPECT_EQ(r.value().descriptors[0].binding, 0u);
  EXPECT_GT(r.value().descriptors[0].sizeBytes, 0u) << "falls back to the default size";
}

//===----------------------------------------------------------------------===//
// Descriptor reflection
//===----------------------------------------------------------------------===//

TEST(SpirvReflectTest, ReflectsStorageBufferDescriptors) {
  const SpvBuilder b = threeStorageBuffers();
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  ASSERT_TRUE(r.hasValue());
  ASSERT_EQ(r.value().descriptors.size(), 3u);
  for (std::uint32_t i = 0; i < 3; ++i) {
    EXPECT_EQ(r.value().descriptors[i].binding, i);
    EXPECT_EQ(r.value().descriptors[i].set, 0u);
    EXPECT_EQ(r.value().descriptors[i].kind, SpirvDescriptorKind::StorageBuffer);
    EXPECT_EQ(r.value().descriptors[i].count, 1u);
    EXPECT_EQ(r.value().descriptors[i].sizeBytes, 4u) << "one float member";
  }
}

TEST(SpirvReflectTest, ReflectsDescriptorSetAndBinding) {
  SpvBuilder b;
  b.op(OpTypeFloat, {1, 32});
  b.op(OpTypeStruct, {2, 1});
  b.annot(OpDecorate, {2, DecBlock});
  b.op(OpTypePointer, {3, SCUniform, 2});
  b.op(OpVariable, {3, 10, SCUniform});
  b.annot(OpDecorate, {10, DecBinding, 7});
  b.annot(OpDecorate, {10, DecDescriptorSet, 3});
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  ASSERT_TRUE(r.hasValue());
  ASSERT_EQ(r.value().descriptors.size(), 1u);
  EXPECT_EQ(r.value().descriptors[0].set, 3u);
  EXPECT_EQ(r.value().descriptors[0].binding, 7u);
  EXPECT_EQ(r.value().descriptors[0].kind, SpirvDescriptorKind::UniformBuffer);
}

TEST(SpirvReflectTest, BlockDecorationYieldsUniformBuffer) {
  SpvBuilder b;
  b.op(OpTypeFloat, {1, 32});
  b.op(OpTypeStruct, {2, 1});
  b.annot(OpDecorate, {2, DecBlock});
  b.op(OpTypePointer, {3, SCUniform, 2});
  b.op(OpVariable, {3, 10, SCUniform});
  b.annot(OpDecorate, {10, DecBinding, 0});
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  ASSERT_TRUE(r.hasValue());
  ASSERT_EQ(r.value().descriptors.size(), 1u);
  EXPECT_EQ(r.value().descriptors[0].kind, SpirvDescriptorKind::UniformBuffer);
}

TEST(SpirvReflectTest, ReflectsBlockMemberLayoutIncludingNonMat4AlignedMatrices) {
  // Reproduces the `gears` block: mat4 projection, mat4 view, vec4 lightpos, then
  // mat4 model[3]. `model[0]` starts at byte 144, which is not a 64-byte boundary, so
  // smoke defaults that assume "identity at every 64-byte boundary" silently leave it
  // zero. Reflection has to report the real offsets.
  SpvBuilder b;
  b.op(OpTypeFloat, {1, 32});                      // %1 = float
  b.op(OpTypeVector, {2, 1, 4});                  // %2 = vec4
  b.op(OpTypeMatrix, {3, 2, 4});                  // %3 = mat4
  b.op(OpConstant, {1, 20, 3});                    // %20 = 3 (array length)
  b.raw((4u << 16) | OpTypeArray);                // OpTypeArray %5 = mat4(%3) [%20]
  b.raw(5);
  b.raw(3);
  b.raw(20);
  b.op(OpTypeStruct, {6, 3, 3, 2, 5});            // projection, view, lightpos, model[3]
  b.annot(OpMemberDecorate, {6, 0, DecOffset, 0});
  b.annot(OpMemberDecorate, {6, 1, DecOffset, 64});
  b.annot(OpMemberDecorate, {6, 2, DecOffset, 128});
  b.annot(OpMemberDecorate, {6, 3, DecOffset, 144});
  b.annot(OpDecorate, {6, DecBlock});
  b.op(OpTypePointer, {7, SCUniform, 6});
  b.op(OpVariable, {7, 10, SCUniform});
  b.annot(OpDecorate, {10, DecBinding, 0});
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Vertex);
  ASSERT_TRUE(r.hasValue());
  ASSERT_EQ(r.value().descriptors.size(), 1u);
  const auto &d = r.value().descriptors[0];
  ASSERT_EQ(d.kind, SpirvDescriptorKind::UniformBuffer);
  // The vec4 is not a matrix, so it is not reported. Size still reflects the whole block.
  EXPECT_EQ(d.sizeBytes, 336u);
  ASSERT_EQ(d.members.size(), 3u);

  EXPECT_EQ(d.members[0].offset, 0u);
  EXPECT_EQ(d.members[0].columns, 4u);
  EXPECT_EQ(d.members[0].rows, 4u);
  EXPECT_EQ(d.members[0].sizeBytes, 64u);

  EXPECT_EQ(d.members[1].offset, 64u);
  EXPECT_EQ(d.members[1].sizeBytes, 64u);

  // The array of mat4 starts mid-alignment; this is the offset the old heuristic missed.
  EXPECT_EQ(d.members[2].offset, 144u);
  EXPECT_EQ(d.members[2].sizeBytes, 192u);
  EXPECT_EQ(d.members[2].columns, 4u);
}

TEST(SpirvReflectTest, FlattensMatricesNestedInArraysOfStructs) {
  // `texturearray` declares `Instance{ mat4 model; float4 arrayIndex; } instance[8]`
  // after two mat4s. Reaching the per-instance models is what keeps the instanced
  // geometry on screen; a top-level-only view leaves them zero and the draw is clipped.
  SpvBuilder b;
  b.op(OpTypeFloat, {1, 32});                      // %1 = float
  b.op(OpTypeVector, {2, 1, 4});                  // %2 = vec4
  b.op(OpTypeMatrix, {3, 2, 4});                  // %3 = mat4
  b.op(OpTypeStruct, {6, 3, 2});                  // %6 = Instance { model, arrayIndex }
  b.annot(OpMemberDecorate, {6, 0, DecOffset, 0});
  b.annot(OpMemberDecorate, {6, 1, DecOffset, 64});
  b.op(OpConstant, {1, 20, 8});                    // %20 = 8
  b.raw((4u << 16) | OpTypeArray);                // %5 = Instance[8]
  b.raw(5);
  b.raw(6);
  b.raw(20);
  b.op(OpTypeStruct, {7, 3, 3, 5});               // projection, view, instance[8]
  b.annot(OpMemberDecorate, {7, 0, DecOffset, 0});
  b.annot(OpMemberDecorate, {7, 1, DecOffset, 64});
  b.annot(OpMemberDecorate, {7, 2, DecOffset, 128});
  b.annot(OpDecorate, {7, DecBlock});
  b.op(OpTypePointer, {8, SCUniform, 7});
  b.op(OpVariable, {8, 10, SCUniform});
  b.annot(OpDecorate, {10, DecBinding, 0});
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Vertex);
  ASSERT_TRUE(r.hasValue());
  ASSERT_EQ(r.value().descriptors.size(), 1u);
  const auto &d = r.value().descriptors[0];
  // projection, view, then all eight instance models. Instance is 80 bytes
  // (mat4 + vec4), so the models land at 128, 208, 288, ...
  ASSERT_EQ(d.members.size(), 10u);
  EXPECT_EQ(d.members[0].offset, 0u);
  EXPECT_EQ(d.members[1].offset, 64u);
  for (std::size_t i = 2; i < d.members.size(); ++i) {
    EXPECT_EQ(d.members[i].offset, 128u + static_cast<std::uint32_t>(i - 2) * 80u)
        << "instance model " << (i - 2);
    EXPECT_EQ(d.members[i].sizeBytes, 64u);
  }
}

TEST(SpirvReflectTest, BlockWithoutMemberOffsetsReportsNoMatrices) {
  // No OpMemberDecorate → no layout to honour, so callers must keep their fallback
  // rather than inventing offsets.
  SpvBuilder b;
  b.op(OpTypeFloat, {1, 32});
  b.op(OpTypeStruct, {2, 1});
  b.annot(OpDecorate, {2, DecBlock});
  b.op(OpTypePointer, {3, SCUniform, 2});
  b.op(OpVariable, {3, 10, SCUniform});
  b.annot(OpDecorate, {10, DecBinding, 0});
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  ASSERT_TRUE(r.hasValue());
  ASSERT_EQ(r.value().descriptors.size(), 1u);
  EXPECT_TRUE(r.value().descriptors[0].members.empty());
}

TEST(SpirvReflectTest, ReflectsCombinedImageSampler) {
  SpvBuilder b;
  b.op(OpTypeSampledImage, {1, 2});
  b.op(OpTypePointer, {3, SCUniformConstant, 1});
  b.op(OpVariable, {3, 10, SCUniformConstant});
  b.annot(OpDecorate, {10, DecBinding, 2});
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  ASSERT_TRUE(r.hasValue());
  ASSERT_EQ(r.value().descriptors.size(), 1u);
  EXPECT_EQ(r.value().descriptors[0].kind, SpirvDescriptorKind::CombinedImageSampler);
}

TEST(SpirvReflectTest, ReflectsStorageImageAndMarksInputAttachment) {
  SpvBuilder b;
  // OpTypeImage: id, sampled type, Dim, depth, arrayed, ms, sampled, image format
  b.op(OpTypeImage, {1, 1, 2 /*Dim2D*/, 1, 0, 0, 2 /*storage*/, 0});
  b.op(OpTypePointer, {2, SCUniformConstant, 1});
  b.op(OpVariable, {2, 10, SCUniformConstant});
  b.annot(OpDecorate, {10, DecBinding, 1});
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  ASSERT_TRUE(r.hasValue());
  ASSERT_EQ(r.value().descriptors.size(), 1u);
  EXPECT_EQ(r.value().descriptors[0].kind, SpirvDescriptorKind::StorageImage);
  EXPECT_FALSE(r.value().usesInputAttachment);
}

TEST(SpirvReflectTest, SubpassDataImageMarksInputAttachmentAndCarriesIndex) {
  SpvBuilder b;
  b.op(OpTypeImage, {1, 1, 6 /*DimSubpassData*/, 1, 0, 0, 1, 0});
  b.op(OpTypePointer, {2, SCUniformConstant, 1});
  b.op(OpVariable, {2, 10, SCUniformConstant});
  b.annot(OpDecorate, {10, DecBinding, 0});
  b.annot(OpDecorate, {10, DecInputAttachmentIndex, 2});
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  ASSERT_TRUE(r.hasValue());
  EXPECT_TRUE(r.value().usesInputAttachment);
  ASSERT_EQ(r.value().descriptors.size(), 1u);
  EXPECT_EQ(r.value().descriptors[0].kind, SpirvDescriptorKind::InputAttachment);
  EXPECT_EQ(r.value().descriptors[0].inputAttachmentIndex, 2u);
}

TEST(SpirvReflectTest, UndecoratedSubpassDataDefaultsToAttachmentZero) {
  SpvBuilder b;
  b.op(OpTypeImage, {1, 1, 6 /*DimSubpassData*/, 1, 0, 0, 1, 0});
  b.op(OpTypePointer, {2, SCUniformConstant, 1});
  b.op(OpVariable, {2, 10, SCUniformConstant});
  b.annot(OpDecorate, {10, DecBinding, 0});
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  ASSERT_TRUE(r.hasValue());
  ASSERT_EQ(r.value().descriptors.size(), 1u);
  EXPECT_EQ(r.value().descriptors[0].kind, SpirvDescriptorKind::InputAttachment);
  EXPECT_EQ(r.value().descriptors[0].inputAttachmentIndex, 0u);
}

TEST(SpirvReflectTest, SampledImageWithInputAttachmentDecorationIsAnInputAttachment) {
  // glslang is free to emit a non-subpassData dimension plus the decoration; the
  // decoration is the authoritative signal, so it must win over the sampled-image path.
  SpvBuilder b;
  b.op(OpTypeImage, {1, 1, 2 /*Dim2D*/, 1, 0, 0, 1 /*sampled*/, 0});
  b.op(OpTypePointer, {2, SCUniformConstant, 1});
  b.op(OpVariable, {2, 10, SCUniformConstant});
  b.annot(OpDecorate, {10, DecBinding, 0});
  b.annot(OpDecorate, {10, DecInputAttachmentIndex, 1});
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  ASSERT_TRUE(r.hasValue());
  ASSERT_EQ(r.value().descriptors.size(), 1u);
  EXPECT_EQ(r.value().descriptors[0].kind, SpirvDescriptorKind::InputAttachment);
  EXPECT_EQ(r.value().descriptors[0].inputAttachmentIndex, 1u);
}

TEST(SpirvReflectTest, RuntimeArrayMarksDescriptorIndexing) {
  SpvBuilder b;
  b.op(OpTypeFloat, {1, 32});
  b.op(OpTypeRuntimeArray, {2, 1});
  b.op(OpTypePointer, {3, SCStorageBuffer, 2});
  b.op(OpVariable, {3, 10, SCStorageBuffer});
  b.annot(OpDecorate, {10, DecBinding, 0});
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  ASSERT_TRUE(r.hasValue());
  EXPECT_TRUE(r.value().usesRuntimeDescriptorArray);
  ASSERT_EQ(r.value().descriptors.size(), 1u);
  EXPECT_EQ(r.value().descriptors[0].count, 8u) << "runtime arrays bind a fixed smoke count";
}

TEST(SpirvReflectTest, FixedArrayUsesConstantLength) {
  SpvBuilder b;
  b.op(OpTypeFloat, {1, 32});
  b.op(OpConstant, {2, 20, 6}); // %20 = 6, referenced as the array length
  b.op(OpTypeRuntimeArray, {3, 1});
  // OpTypeArray: result, element type, length id
  b.raw((4u << 16) | 28u);
  b.raw(4);
  b.raw(1);
  b.raw(20);
  b.op(OpTypePointer, {5, SCStorageBuffer, 4});
  b.op(OpVariable, {5, 10, SCStorageBuffer});
  b.annot(OpDecorate, {10, DecBinding, 0});
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  ASSERT_TRUE(r.hasValue());
  ASSERT_EQ(r.value().descriptors.size(), 1u);
  EXPECT_EQ(r.value().descriptors[0].count, 6u);
}

TEST(SpirvReflectTest, PushConstantSizeComesFromStructLayout) {
  SpvBuilder b;
  b.op(OpTypeFloat, {1, 32});
  b.op(OpTypeVector, {2, 1, 4}); // vec4 = 16 bytes
  b.op(OpTypeStruct, {3, 2});    // one vec4 member
  b.op(OpTypePointer, {4, SCPushConstant, 3});
  b.op(OpVariable, {4, 10, SCPushConstant});
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  ASSERT_TRUE(r.hasValue());
  ASSERT_EQ(r.value().pushConstants.size(), 1u);
  EXPECT_EQ(r.value().pushConstants[0].size, 16u);
  EXPECT_TRUE(r.value().pushConstants[0].fromFragment);
  EXPECT_FALSE(r.value().pushConstants[0].fromVertex);
}

TEST(SpirvReflectTest, PushConstantWithUnknownSizeFallsBackToDefault) {
  SpvBuilder b;
  b.op(OpTypeStruct, {3}); // no members -> size computes to 0
  b.op(OpTypePointer, {4, SCPushConstant, 3});
  b.op(OpVariable, {4, 10, SCPushConstant});
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  ASSERT_TRUE(r.hasValue());
  ASSERT_EQ(r.value().pushConstants.size(), 1u);
  EXPECT_EQ(r.value().pushConstants[0].size, 128u);
}

TEST(SpirvReflectTest, StructSizeUsesMemberOffsets) {
  SpvBuilder b;
  b.op(OpTypeFloat, {1, 32});
  b.op(OpTypeStruct, {2, 1, 1}); // two floats
  b.annot(OpMemberDecorate, {2, 0, DecOffset, 0});
  b.annot(OpMemberDecorate, {2, 1, DecOffset, 4});
  b.annot(OpDecorate, {2, DecBlock});
  b.op(OpTypePointer, {3, SCUniform, 2});
  b.op(OpVariable, {3, 10, SCUniform});
  b.annot(OpDecorate, {10, DecBinding, 0});
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  ASSERT_TRUE(r.hasValue());
  ASSERT_EQ(r.value().descriptors.size(), 1u);
  EXPECT_EQ(r.value().descriptors[0].sizeBytes, 8u);
}

//===----------------------------------------------------------------------===//
// Vertex inputs and stage flags
//===----------------------------------------------------------------------===//

TEST(SpirvReflectTest, ReflectsVertexInputsSortedByLocation) {
  SpvBuilder b;
  b.op(OpTypeFloat, {1, 32});
  b.op(OpTypeVector, {2, 1, 3}); // vec3
  b.op(OpTypePointer, {3, SCInput, 2});
  b.op(OpVariable, {3, 10, SCInput});
  b.annot(OpDecorate, {10, DecLocation, 2});
  b.op(OpVariable, {3, 11, SCInput});
  b.annot(OpDecorate, {11, DecLocation, 0});
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Vertex);
  ASSERT_TRUE(r.hasValue());
  ASSERT_EQ(r.value().vertexInputs.size(), 2u);
  EXPECT_EQ(r.value().vertexInputs[0].location, 0u) << "must be sorted";
  EXPECT_EQ(r.value().vertexInputs[1].location, 2u);
  EXPECT_EQ(r.value().vertexInputs[0].components, 3u);
  EXPECT_EQ(r.value().vertexInputs[0].type, csrun::SpirvVertexCompType::Float);
}

TEST(SpirvReflectTest, DistinguishesSignedAndUnsignedIntAttributes) {
  SpvBuilder b;
  b.op(OpTypeInt, {1, 32, 1});  // signed
  b.op(OpTypeInt, {2, 32, 0});  // unsigned
  b.op(OpTypePointer, {3, SCInput, 1});
  b.op(OpTypePointer, {4, SCInput, 2});
  b.op(OpVariable, {3, 10, SCInput});
  b.annot(OpDecorate, {10, DecLocation, 0});
  b.op(OpVariable, {4, 11, SCInput});
  b.annot(OpDecorate, {11, DecLocation, 1});
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Vertex);
  ASSERT_TRUE(r.hasValue());
  ASSERT_EQ(r.value().vertexInputs.size(), 2u);
  EXPECT_EQ(r.value().vertexInputs[0].type, csrun::SpirvVertexCompType::Int);
  EXPECT_EQ(r.value().vertexInputs[1].type, csrun::SpirvVertexCompType::Uint);
  EXPECT_EQ(r.value().vertexInputs[0].components, 1u);
}

TEST(SpirvReflectTest, NonVertexStagesProduceNoVertexInputs) {
  SpvBuilder b;
  b.op(OpTypeFloat, {1, 32});
  b.op(OpTypePointer, {3, SCInput, 1});
  b.op(OpVariable, {3, 10, SCInput});
  b.annot(OpDecorate, {10, DecLocation, 0});
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Fragment);
  ASSERT_TRUE(r.hasValue());
  EXPECT_TRUE(r.value().vertexInputs.empty());
}

TEST(SpirvReflectTest, StageFlagIsSetForEveryStage) {
  const SpvBuilder b = threeStorageBuffers();
  const std::pair<SpirvReflectStage, bool csrun::SpirvDescriptorBinding::*> kCases[] = {
      {SpirvReflectStage::Vertex, &csrun::SpirvDescriptorBinding::fromVertex},
      {SpirvReflectStage::Fragment, &csrun::SpirvDescriptorBinding::fromFragment},
      {SpirvReflectStage::Geometry, &csrun::SpirvDescriptorBinding::fromGeometry},
      {SpirvReflectStage::TessControl, &csrun::SpirvDescriptorBinding::fromTessControl},
      {SpirvReflectStage::TessEvaluation, &csrun::SpirvDescriptorBinding::fromTessEvaluation},
      {SpirvReflectStage::Mesh, &csrun::SpirvDescriptorBinding::fromMesh},
      {SpirvReflectStage::Task, &csrun::SpirvDescriptorBinding::fromTask},
  };
  for (const auto &entry : kCases) {
    const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, entry.first);
    ASSERT_TRUE(r.hasValue());
    ASSERT_EQ(r.value().descriptors.size(), 3u);
    for (const auto &d : r.value().descriptors) {
      const bool flag = d.*(entry.second);
      EXPECT_TRUE(flag);
      // Exactly one stage flag may be set at a time.
      int set = 0;
      set += d.fromVertex ? 1 : 0;
      set += d.fromFragment ? 1 : 0;
      set += d.fromGeometry ? 1 : 0;
      set += d.fromTessControl ? 1 : 0;
      set += d.fromTessEvaluation ? 1 : 0;
      set += d.fromMesh ? 1 : 0;
      set += d.fromTask ? 1 : 0;
      EXPECT_EQ(set, 1);
    }
  }
}

TEST(SpirvReflectTest, DetectsClipDistanceCapability) {
  SpvBuilder b;
  b.op(OpCapability, {32});
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Vertex);
  ASSERT_TRUE(r.hasValue());
  EXPECT_TRUE(r.value().usesClipDistance);
}

TEST(SpirvReflectTest, IgnoresUnrelatedCapabilities) {
  SpvBuilder b;
  b.op(OpCapability, {1}); // Matrix
  const auto words = b.words();
  auto r = csrun::reflectSpirvModule(words, SpirvReflectStage::Vertex);
  ASSERT_TRUE(r.hasValue());
  EXPECT_FALSE(r.value().usesClipDistance);
}

//===----------------------------------------------------------------------===//
// Merging
//===----------------------------------------------------------------------===//

TEST(SpirvReflectTest, MergeUnionsDescriptorsFromBothStages) {
  const SpvBuilder vb = threeStorageBuffers();

  SpvBuilder fb;
  fb.op(OpTypeFloat, {1, 32});
  fb.op(OpTypeStruct, {2, 1});
  fb.annot(OpDecorate, {2, DecBlock});
  fb.op(OpTypePointer, {3, SCUniform, 2});
  fb.op(OpVariable, {3, 20, SCUniform});
  fb.annot(OpDecorate, {20, DecBinding, 0});
  fb.annot(OpDecorate, {20, DecDescriptorSet, 1});

  const auto vw = vb.words();
  auto v = csrun::reflectSpirvModule(vw, SpirvReflectStage::Vertex);
  const auto fw = fb.words();
  auto f = csrun::reflectSpirvModule(fw, SpirvReflectStage::Fragment);
  ASSERT_TRUE(v.hasValue());
  ASSERT_TRUE(f.hasValue());

  const auto merged = csrun::mergeSpirvReflections(v.value(), f.value());
  EXPECT_EQ(merged.descriptors.size(), 4u) << "3 from vertex + 1 from fragment";
  EXPECT_TRUE(merged.vertexInputs.empty());
}

TEST(SpirvReflectTest, MergePreservesCapabilityFlags) {
  SpvBuilder a;
  a.op(OpCapability, {32});
  SpvBuilder b;
  b.op(OpTypeImage, {1, 1, 6, 1, 0, 0, 1, 0});

  const auto aw = a.words();
  auto va = csrun::reflectSpirvModule(aw, SpirvReflectStage::Vertex);
  auto fb = csrun::reflectSpirvModule(b.words(), SpirvReflectStage::Fragment);
  ASSERT_TRUE(va.hasValue());
  ASSERT_TRUE(fb.hasValue());

  const auto merged = csrun::mergeSpirvReflections(va.value(), fb.value());
  EXPECT_TRUE(merged.usesClipDistance) << "flags from either stage must survive the merge";
  EXPECT_TRUE(merged.usesInputAttachment);
}

TEST(SpirvReflectTest, MergeWithEmptyStagesIsSafe) {
  const csrun::SpirvModuleReflection empty;
  const SpvBuilder b = threeStorageBuffers();
  auto v = csrun::reflectSpirvModule(b.words(), SpirvReflectStage::Vertex);
  ASSERT_TRUE(v.hasValue());

  const auto merged = csrun::mergeSpirvReflections(v.value(), empty);
  EXPECT_EQ(merged.descriptors.size(), 3u);
}

//===----------------------------------------------------------------------===//
// toString
//===----------------------------------------------------------------------===//

TEST(SpirvReflectTest, ToStringCoversEveryKind) {
  EXPECT_EQ(csrun::toString(SpirvDescriptorKind::UniformBuffer), "uniform_buffer");
  EXPECT_EQ(csrun::toString(SpirvDescriptorKind::StorageBuffer), "storage_buffer");
  EXPECT_EQ(csrun::toString(SpirvDescriptorKind::CombinedImageSampler),
            "combined_image_sampler");
  EXPECT_EQ(csrun::toString(SpirvDescriptorKind::SampledImage), "sampled_image");
  EXPECT_EQ(csrun::toString(SpirvDescriptorKind::Sampler), "sampler");
  EXPECT_EQ(csrun::toString(SpirvDescriptorKind::StorageImage), "storage_image");
  EXPECT_EQ(csrun::toString(SpirvDescriptorKind::InputAttachment), "input_attachment");
  EXPECT_EQ(csrun::toString(SpirvDescriptorKind::Unknown), "unknown");
}
