// Copyright 2017 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "sling/myelin/simd-assembler.h"

namespace sling {
namespace myelin {

using namespace jit;

void SIMDGenerator::Broadcast(int dst, const Operand &src) {
  // Broadcast is just a load for scalars.
  CHECK_EQ(VectorSize(), 1);
  Load(dst, src);
}


void SIMDGenerator::Sum(int r) {
  // Sum is a no-op for scalars.
  CHECK_EQ(VectorSize(), 1);
}

bool SIMDGenerator::SupportsMasking() {
  return false;
}

void SIMDGenerator::SetMask(int bits) {
  LOG(FATAL) << "Masking not supported";
}

void SIMDGenerator::MaskedLoad(int dst, const Operand &src) {
  LOG(FATAL) << "Masking not supported";
}

void SIMDGenerator::MaskedStore(const Operand &dst, int src) {
  LOG(FATAL) << "Masking not supported";
}

void SIMDGenerator::MaskedAdd(int dst, int src1, const jit::Operand &src2) {
  LOG(FATAL) << "Masking not supported";
}

void SIMDGenerator::MaskedMul(int dst, int src1, const jit::Operand &src2) {
  LOG(FATAL) << "Masking not supported";
}

void SIMDGenerator::MaskedMulAdd(int dst, int src1, const jit::Operand &src2) {
  LOG(FATAL) << "Masking not supported";
}

// AVX512 float SIMD generator using 512-bit ZMM registers.
class AVX512FloatGenerator : public SIMDGenerator {
 public:
  AVX512FloatGenerator(MacroAssembler *masm, bool aligned)
      : SIMDGenerator(masm, aligned) {
    mask_ = masm->kk().alloc();
  }
  ~AVX512FloatGenerator() override {
    masm_->kk().release(mask_);
  }

  // Sixteen 32-bit floats per YMM register.
  int VectorBytes() override { return 64; }
  int VectorSize() override { return 16; }
  int Alloc() override { return masm_->mm().alloc(true); }

  void Load(int dst, const Operand &src) override {
    if (aligned_) {
      masm_->vmovaps(zmm(dst), src);
    } else {
      masm_->vmovups(zmm(dst), src);
    }
  }

  void Store(const Operand &dst, int src) override {
    if (aligned_) {
      masm_->vmovaps(dst, zmm(src));
    } else {
      masm_->vmovups(dst, zmm(src));
    }
  }

  void Broadcast(int dst, const Operand &src) override {
    masm_->vbroadcastss(zmm(dst), src);
  }

  void Zero(int r) override {
    masm_->vxorps(zmm(r), zmm(r), zmm(r));
  }

  void Add(int dst, int src1, int src2) override {
    masm_->vaddps(zmm(dst), zmm(src1), zmm(src2));
  }

  void Add(int dst, int src1, const jit::Operand &src2) override {
    masm_->vaddps(zmm(dst), zmm(src1), src2);
  }

  void Mul(int dst, int src1, const jit::Operand &src2) override {
    masm_->vmulps(zmm(dst), zmm(src1), src2);
  }

  void MulAdd(int dst, int src1, const Operand &src2, bool retain) override {
    if (masm_->Enabled(FMA3)) {
      masm_->vfmadd231ps(zmm(dst), zmm(src1), src2);
    } else if (retain) {
      ZMMRegister acc = masm_->mm().allocz();
      masm_->vmulps(acc, zmm(src1), src2);
      masm_->vaddps(zmm(dst), zmm(dst), acc);
      masm_->mm().release(acc);
    } else {
      masm_->vmulps(zmm(src1), zmm(src1), src2);
      masm_->vaddps(zmm(dst), zmm(dst), zmm(src1));
    }
  }

  void Sum(int r) override {
    ZMMRegister sum = ZMMRegister::from_code(r);
    ZMMRegister acc = masm_->mm().allocz();
    masm_->vshuff32x4(acc, sum, sum, 0x0E);
    masm_->vaddps(sum, sum, acc);
    masm_->vperm2f128(acc.ymm(), sum.ymm(), sum.ymm(), 1);
    masm_->vhaddps(sum.ymm(), sum.ymm(), acc.ymm());
    masm_->vhaddps(sum.ymm(), sum.ymm(), sum.ymm());
    masm_->vhaddps(sum.ymm(), sum.ymm(), sum.ymm());
    masm_->mm().release(acc);
  }

  bool SupportsMasking() override {
    return true;
  }

  void SetMask(int bits) override {
    masm_->LoadMask(bits, mask_);
  }

  void MaskedLoad(int dst, const jit::Operand &src) override {
    if (aligned_) {
      masm_->vmovaps(zmm(dst), src, Mask(mask_, zeroing));
    } else {
      masm_->vmovups(zmm(dst), src, Mask(mask_, zeroing));
    }
  }

  void MaskedStore(const jit::Operand &dst, int src) override  {
    if (aligned_) {
      masm_->vmovaps(dst, zmm(src), Mask(mask_, merging));
    } else {
      masm_->vmovups(dst, zmm(src), Mask(mask_, merging));
    }
  }

  void MaskedAdd(int dst, int src1, const jit::Operand &src2) override {
    masm_->vaddps(zmm(dst), zmm(src1), src2, Mask(mask_, merging));
  }

  void MaskedMul(int dst, int src1, const jit::Operand &src2) override {
    masm_->vmulps(zmm(dst), zmm(src1), src2, Mask(mask_, merging));
  }

  void MaskedMulAdd(int dst, int src1, const jit::Operand &src2) override {
    masm_->vfmadd231ps(zmm(dst), zmm(src1), src2, Mask(mask_, merging));
  }

 private:
   OpmaskRegister mask_;
};

// AVX256 float SIMD generator using 256-bit YMM registers.
class AVX256FloatGenerator : public SIMDGenerator {
 public:
  AVX256FloatGenerator(MacroAssembler *masm, bool aligned)
      : SIMDGenerator(masm, aligned) {}

  // Eight 32-bit floats per YMM register.
  int VectorBytes() override { return 32; }
  int VectorSize() override { return 8; }
  int Alloc() override { return masm_->mm().alloc(false); }

  void Load(int dst, const Operand &src) override {
    if (aligned_) {
      masm_->vmovaps(ymm(dst), src);
    } else {
      masm_->vmovups(ymm(dst), src);
    }
  }

  void Store(const Operand &dst, int src) override {
    if (aligned_) {
      masm_->vmovaps(dst, ymm(src));
    } else {
      masm_->vmovups(dst, ymm(src));
    }
  }

  void Broadcast(int dst, const Operand &src) override {
    masm_->vbroadcastss(ymm(dst), src);
  }

  void Zero(int r) override {
    masm_->vxorps(ymm(r), ymm(r), ymm(r));
  }

  void Add(int dst, int src1, int src2) override {
    masm_->vaddps(ymm(dst), ymm(src1), ymm(src2));
  }

  void Add(int dst, int src1, const jit::Operand &src2) override {
    masm_->vaddps(ymm(dst), ymm(src1), src2);
  }

  void Mul(int dst, int src1, const jit::Operand &src2) override {
    masm_->vmulps(ymm(dst), ymm(src1), src2);
  }

  void MulAdd(int dst, int src1, const Operand &src2, bool retain) override {
    if (masm_->Enabled(FMA3)) {
      masm_->vfmadd231ps(ymm(dst), ymm(src1), src2);
    } else if (retain) {
      YMMRegister acc = masm_->mm().allocy();
      masm_->vmulps(acc, ymm(src1), src2);
      masm_->vaddps(ymm(dst), ymm(dst), acc);
      masm_->mm().release(acc);
    } else {
      masm_->vmulps(ymm(src1), ymm(src1), src2);
      masm_->vaddps(ymm(dst), ymm(dst), ymm(src1));
    }
  }

  void Sum(int r) override {
    YMMRegister sum = YMMRegister::from_code(r);
    YMMRegister acc = masm_->mm().allocy();
    masm_->vperm2f128(acc, sum, sum, 1);
    masm_->vhaddps(sum, sum, acc);
    masm_->vhaddps(sum, sum, sum);
    masm_->vhaddps(sum, sum, sum);
    masm_->mm().release(acc);
  }
};

// AVX128 float SIMD generator using 128-bit XMM registers.
class AVX128FloatGenerator : public SIMDGenerator {
 public:
  AVX128FloatGenerator(MacroAssembler *masm, bool aligned)
      : SIMDGenerator(masm, aligned) {}

  // Four 32-bit floats per XMM register.
  int VectorBytes() override { return 16; }
  int VectorSize() override { return 4; }
  int Alloc() override { return masm_->mm().alloc(false); }

  void Load(int dst, const Operand &src) override {
    if (aligned_) {
      masm_->vmovaps(xmm(dst), src);
    } else {
      masm_->vmovups(xmm(dst), src);
    }
  }

  void Store(const Operand &dst, int src) override {
    if (aligned_) {
      masm_->vmovaps(dst, xmm(src));
    } else {
      masm_->vmovups(dst, xmm(src));
    }
  }

  void Broadcast(int dst, const Operand &src) override {
    masm_->vbroadcastss(xmm(dst), src);
  }

  void Zero(int r) override {
    masm_->vxorps(xmm(r), xmm(r), xmm(r));
  }

  void Add(int dst, int src1, int src2) override {
    masm_->vaddps(xmm(dst), xmm(src1), xmm(src2));
  }

  void Add(int dst, int src1, const jit::Operand &src2) override {
    masm_->vaddps(xmm(dst), xmm(src1), src2);
  }

  void Mul(int dst, int src1, const jit::Operand &src2) override {
    masm_->vmulps(xmm(dst), xmm(src1), src2);
  }

  void MulAdd(int dst, int src1, const Operand &src2, bool retain) override {
    if (masm_->Enabled(FMA3)) {
      masm_->vfmadd231ps(xmm(dst), xmm(src1), src2);
    } else if (retain) {
      XMMRegister acc = masm_->mm().allocx();
      masm_->vmulps(acc, xmm(src1), src2);
      masm_->vaddps(xmm(dst), xmm(dst), acc);
      masm_->mm().release(acc);
    } else {
      masm_->vmulps(xmm(src1), xmm(src1), src2);
      masm_->vaddps(xmm(dst), xmm(dst), xmm(src1));
    }
  }

  void Sum(int r) override {
    XMMRegister sum = XMMRegister::from_code(r);
    masm_->vhaddps(sum, sum, sum);
    masm_->vhaddps(sum, sum, sum);
  }
};

// SSE128 float SIMD generator using 128-bit XMM registers.
class SSE128FloatGenerator : public SIMDGenerator {
 public:
  SSE128FloatGenerator(MacroAssembler *masm, bool aligned)
      : SIMDGenerator(masm, aligned) {}

  // Four 32-bit floats per YMM register.
  int VectorBytes() override { return 16; }
  int VectorSize() override { return 4; }
  int Alloc() override { return masm_->mm().alloc(false); }

  void Load(int dst, const Operand &src) override {
    if (aligned_) {
      masm_->movaps(xmm(dst), src);
    } else {
      masm_->movups(xmm(dst), src);
    }
  }

  void Store(const Operand &dst, int src) override {
    if (aligned_) {
      masm_->movaps(dst, xmm(src));
    } else {
      masm_->movups(dst, xmm(src));
    }
  }

  void Broadcast(int dst, const Operand &src) override {
    masm_->movss(xmm(dst), src);
    masm_->shufps(xmm(dst), xmm(dst), 0);
  }

  void Zero(int r) override {
    masm_->xorps(xmm(r), xmm(r));
  }

  void Add(int dst, int src1, int src2) override {
    if (dst != src1) masm_->movaps(xmm(dst), xmm(src1));
    masm_->addps(xmm(dst), xmm(src2));
  }

  void Add(int dst, int src1, const jit::Operand &src2) override {
    if (dst != src1) masm_->movaps(xmm(dst), xmm(src1));
    if (aligned_) {
      masm_->addps(xmm(dst), src2);
    } else {
      XMMRegister mem = masm_->mm().allocx();
      masm_->movups(mem, src2);
      masm_->addps(xmm(dst), mem);
      masm_->mm().release(mem);
    }
  }

  void Mul(int dst, int src1, const jit::Operand &src2) override {
    if (dst != src1) masm_->movaps(xmm(dst), xmm(src1));
    if (aligned_) {
      masm_->mulps(xmm(dst), src2);
    } else {
      XMMRegister mem = masm_->mm().allocx();
      masm_->movups(mem, src2);
      masm_->mulps(xmm(dst), mem);
      masm_->mm().release(mem);
    }
  }

  void MulAdd(int dst, int src1, const Operand &src2, bool retain) override {
    if (retain) {
      if (aligned_) {
        XMMRegister acc = masm_->mm().allocx();
        masm_->movaps(acc, xmm(src1));
        masm_->mulps(acc, src2);
        masm_->addps(xmm(dst), acc);
        masm_->mm().release(acc);
      } else {
        XMMRegister acc = masm_->mm().allocx();
        XMMRegister mem = masm_->mm().allocx();
        masm_->movaps(acc, xmm(src1));
        masm_->movups(mem, src2);
        masm_->mulps(acc, mem);
        masm_->addps(xmm(dst), acc);
        masm_->mm().release(acc);
        masm_->mm().release(mem);
      }
    } else {
      if (aligned_) {
        masm_->mulps(xmm(src1), src2);
        masm_->addps(xmm(dst), xmm(src1));
      } else {
        XMMRegister mem = masm_->mm().allocx();
        masm_->movups(mem, src2);
        masm_->mulps(xmm(src1), mem);
        masm_->addps(xmm(dst), xmm(src1));
        masm_->mm().release(mem);
      }
    }
  }

  void Sum(int r) override {
    XMMRegister sum = XMMRegister::from_code(r);
    masm_->haddps(sum, sum);
    masm_->haddps(sum, sum);
  }
};

// AVX512 scalar float SIMD generator.
class AVX512ScalarFloatGenerator : public SIMDGenerator {
 public:
  AVX512ScalarFloatGenerator(MacroAssembler *masm, bool aligned)
      : SIMDGenerator(masm, aligned) {}

  // Only uses the lower 32-bit float of ZMM register.
  int VectorBytes() override { return sizeof(float); }
  int VectorSize() override { return 1; }
  int Alloc() override { return masm_->mm().alloc(true); }

  void Load(int dst, const Operand &src) override {
    masm_->vmovss(zmm(dst), src);
  }

  void Store(const Operand &dst, int src) override {
    masm_->vmovss(dst, zmm(src));
  }

  void Zero(int r) override {
    masm_->vxorps(zmm(r), zmm(r), zmm(r));
  }

  void Add(int dst, int src1, int src2) override {
    masm_->vaddss(zmm(dst), zmm(src1), zmm(src2));
  }

  void Add(int dst, int src1, const jit::Operand &src2) override {
    masm_->vaddss(zmm(dst), zmm(src1), src2);
  }

  void Mul(int dst, int src1, const jit::Operand &src2) override {
    masm_->vmulss(zmm(dst), zmm(src1), src2);
  }

  void MulAdd(int dst, int src1, const Operand &src2, bool retain) override {
    if (masm_->Enabled(FMA3)) {
      masm_->vfmadd231ss(zmm(dst), zmm(src1), src2);
    } else if (retain) {
      ZMMRegister acc = masm_->mm().allocz();
      masm_->vmulss(acc, zmm(src1), src2);
      masm_->vaddss(zmm(dst), zmm(dst), acc);
      masm_->mm().release(acc);
    } else {
      masm_->vmulss(zmm(src1), zmm(src1), src2);
      masm_->vaddss(zmm(dst), zmm(dst), zmm(src1));
    }
  }
};

// AVX scalar float SIMD generator.
class AVXScalarFloatGenerator : public SIMDGenerator {
 public:
  AVXScalarFloatGenerator(MacroAssembler *masm, bool aligned)
      : SIMDGenerator(masm, aligned) {}

  // Only uses the lower 32-bit float of XMM register.
  int VectorBytes() override { return sizeof(float); }
  int VectorSize() override { return 1; }
  int Alloc() override { return masm_->mm().alloc(false); }

  void Load(int dst, const Operand &src) override {
    masm_->vmovss(XMMRegister::from_code(dst), src);
  }

  void Store(const Operand &dst, int src) override {
    masm_->vmovss(dst, XMMRegister::from_code(src));
  }

  void Zero(int r) override {
    masm_->vxorps(xmm(r), xmm(r), xmm(r));
  }

  void Add(int dst, int src1, int src2) override {
    XMMRegister d = XMMRegister::from_code(dst);
    XMMRegister s1 = XMMRegister::from_code(src1);
    XMMRegister s2 = XMMRegister::from_code(src2);
    masm_->vaddss(d, s1, s2);
  }

  void Add(int dst, int src1, const jit::Operand &src2) override {
    XMMRegister d = XMMRegister::from_code(dst);
    XMMRegister s1 = XMMRegister::from_code(src1);
    masm_->vaddss(d, s1, src2);
  }

  void Mul(int dst, int src1, const jit::Operand &src2) override {
    XMMRegister d = XMMRegister::from_code(dst);
    XMMRegister s1 = XMMRegister::from_code(src1);
    masm_->vmulss(d, s1, src2);
  }

  void MulAdd(int dst, int src1, const Operand &src2, bool retain) override {
    XMMRegister d = XMMRegister::from_code(dst);
    XMMRegister s1 = XMMRegister::from_code(src1);
    if (masm_->Enabled(FMA3)) {
      masm_->vfmadd231ss(d, s1, src2);
    } else if (retain) {
      XMMRegister acc = masm_->mm().allocx();
      masm_->vmulss(acc, s1, src2);
      masm_->vaddss(d, d, acc);
      masm_->mm().release(acc);
    } else {
      masm_->vmulss(s1, s1, src2);
      masm_->vaddss(d, d, s1);
    }
  }
};

// SSE scalar float SIMD generator.
class SSEScalarFloatGenerator : public SIMDGenerator {
 public:
  SSEScalarFloatGenerator(MacroAssembler *masm, bool aligned)
      : SIMDGenerator(masm, aligned) {}

  // Only uses the lower 32-bit float of XMM register.
  int VectorBytes() override { return sizeof(float); }
  int VectorSize() override { return 1; }
  int Alloc() override { return masm_->mm().alloc(false); }

  void Load(int dst, const Operand &src) override {
    masm_->movss(xmm(dst), src);
  }

  void Store(const Operand &dst, int src) override {
    masm_->movss(dst, xmm(src));
  }

  void Zero(int r) override {
    masm_->xorps(xmm(r), xmm(r));
  }

  void Add(int dst, int src1, int src2) override {
    if (dst != src1) masm_->movss(xmm(dst), xmm(src1));
    masm_->addss(xmm(dst), xmm(src2));
  }

  void Add(int dst, int src1, const jit::Operand &src2) override {
    if (dst != src1) masm_->movss(xmm(dst), xmm(src1));
    masm_->addss(xmm(dst), src2);
  }

  void Mul(int dst, int src1, const jit::Operand &src2) override {
    if (dst != src1) masm_->movss(xmm(dst), xmm(src1));
    masm_->mulss(xmm(dst), src2);
  }

  void MulAdd(int dst, int src1, const Operand &src2, bool retain) override {
    if (retain) {
      XMMRegister acc = masm_->mm().allocx();
      masm_->movss(acc, xmm(src1));
      masm_->mulss(acc, src2);
      masm_->addss(xmm(dst), acc);
      masm_->mm().release(acc);
    } else {
      masm_->mulss(xmm(src1), src2);
      masm_->addss(xmm(dst), xmm(src1));
    }
  }
};

bool SIMDAssembler::Supports(Type type) {
  // Only floats are currently supported.
  return type == DT_FLOAT;
}

int SIMDAssembler::VectorBytes(Type type) {
  if (CPU::Enabled(AVX512F)) return 64;
  if (CPU::Enabled(AVX)) return 32;
  if (CPU::Enabled(SSE)) return 16;
  return TypeTraits::of(type).size();
}

SIMDAssembler::SIMDAssembler(MacroAssembler *masm, Type type, bool aligned) {
  switch (type) {
    case DT_FLOAT:
      if (masm->Enabled(AVX512F)) {
        name_ = "AVX512Flt";
        add(new AVX512FloatGenerator(masm, aligned));
        add(new AVX512ScalarFloatGenerator(masm, aligned));
      } else if (masm->Enabled(AVX)) {
        name_ = "AVXFlt";
        add(new AVX256FloatGenerator(masm, aligned));
        add(new AVX128FloatGenerator(masm, aligned));
        add(new AVXScalarFloatGenerator(masm, aligned));
      } else if (masm->Enabled(SSE)) {
        name_ = "SSEFlt";
        add(new SSE128FloatGenerator(masm, aligned));
        add(new SSEScalarFloatGenerator(masm, aligned));
      }
      break;

    default:
      LOG(FATAL) << "Unsuported type";
  }
}

SIMDAssembler::~SIMDAssembler() {
  for (auto *r : cascade_) delete r;
}

void SIMDAssembler::Sum(const std::vector<int> &regs) {
  if (regs.size() == 4) {
    main()->Add(regs[0], regs[0], regs[2]);
    main()->Add(regs[1], regs[1], regs[3]);
    main()->Add(regs[0], regs[0], regs[1]);
  } else {
    for (int n = 1; n < regs.size(); ++n) {
      main()->Add(regs[0], regs[0], regs[n]);
    }
  }
}

SIMDStrategy::SIMDStrategy(SIMDAssembler *sasm, int size, int max_unrolls) {
  // Add bulk phase.
  int vecsize = sasm->main()->VectorSize();
  int main = (size / vecsize) * vecsize;
  int unrolls = std::min(main / vecsize, max_unrolls);
  int remaining = size;
  int offset = 0;
  if (unrolls > 0) {
    phases_.emplace_back(sasm->main());
    Phase &bulk = phases_.back();
    bulk.unrolls = unrolls;
    bulk.repeat = size / (vecsize * unrolls);
    remaining -= bulk.repeat * vecsize * unrolls;
    offset += bulk.repeat * vecsize * unrolls;
  }

  // Add residual phases.
  for (auto *gen : sasm->cascade()) {
    // Stop when all elements have been processed.
    if (remaining == 0) break;

    // Compute the number of elements that can be handled with this vector size.
    int vecsize = gen->VectorSize();
    int n = remaining / vecsize;
    if (n > 0) {
      // Add phase for generator.
      phases_.emplace_back(gen);
      Phase &phase = phases_.back();
      phase.unrolls = n;
      phase.offset = offset;
      offset += n * vecsize;
      remaining -= n * vecsize;
    }

    // Add masked phase for remainder if generator supports it.
    if (gen->SupportsMasking() && remaining > 0 && remaining < vecsize) {
      // Add masked phase for generator.
      phases_.emplace_back(gen);
      Phase &phase = phases_.back();
      phase.masked = remaining;
      phase.offset = offset;
      offset += remaining;
      remaining = 0;
    }
  }
}

int SIMDStrategy::MaxUnrolls() {
  int unrolls = 1;
  for (auto &p : phases_) unrolls = std::max(unrolls, p.unrolls);
  return unrolls;
}

void SIMDStrategy::PreloadMasks() {
  for (auto &p : phases_) {
    if (p.masked) p.generator->SetMask(p.masked);
  }
}

}  // namespace myelin
}  // namespace sling

