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

#include "sling/myelin/generator/expression.h"

#define __ masm->

namespace sling {
namespace myelin {

using namespace jit;

// Generate scalar float expression using SSE and XMM registers.
class ScalarFltSSEGenerator : public ExpressionGenerator {
 public:
  ScalarFltSSEGenerator() {
    model_.mov_reg_reg = true;
    model_.mov_reg_imm = true;
    model_.mov_reg_mem = true;
    model_.mov_mem_reg = true;
    model_.op_reg_reg = true;
    model_.op_reg_imm = true;
    model_.op_reg_mem = true;
    model_.func_reg_reg = true;
    model_.func_reg_imm = true;
    model_.func_reg_mem = true;
  }

  string Name() override { return "FltSSE"; }

  void Reserve() override {
    // Reserve XMM registers.
    index_->ReserveXMMRegisters(instructions_.NumRegs());

    // Allocate auxiliary registers.
    int num_mm_aux = 0;
    if (instructions_.Has(Express::BITAND) ||
        instructions_.Has(Express::BITOR) ||
        instructions_.Has(Express::AND) ||
        instructions_.Has(Express::OR) ||
        instructions_.Has(Express::XOR) ||
        instructions_.Has(Express::ANDNOT) ||
        instructions_.Has(Express::CVTFLTINT) ||
        instructions_.Has(Express::CVTINTFLT) ||
        instructions_.Has(Express::SUBINT)) {
      num_mm_aux = std::max(num_mm_aux, 1);
    }
    if (instructions_.Has(Express::NOT)) {
      num_mm_aux = std::max(num_mm_aux, 2);
      index_->ReserveAuxRegisters(1);
    }

    index_->ReserveAuxXMMRegisters(num_mm_aux);
  }

  void Generate(Express::Op *instr, MacroAssembler *masm) override {
    switch (instr->type) {
      case Express::MOV:
        if (IsLoadZero(instr) && masm->Enabled(ZEROIDIOM)) {
          // Use XOR to zero register instead of loading constant from memory.
          // This uses the floating point version of xor to avoid bypass delays
          // between integer and floating point units.
          switch (type_) {
            case DT_FLOAT:
              __ xorps(xmm(instr->dst), xmm(instr->dst));
              break;
            case DT_DOUBLE:
              __ xorpd(xmm(instr->dst), xmm(instr->dst));
              break;
            default: UNSUPPORTED;
          }
        } else {
          GenerateXMMScalarFltMove(instr, masm);
        }
        break;
      case Express::ADD:
        GenerateXMMFltOp(instr,
            &Assembler::addss, &Assembler::addsd,
            &Assembler::addss, &Assembler::addsd,
            masm);
        break;
      case Express::SUB:
        GenerateXMMFltOp(instr,
            &Assembler::subss, &Assembler::subsd,
            &Assembler::subss, &Assembler::subsd,
            masm);
        break;
      case Express::MUL:
        GenerateXMMFltOp(instr,
            &Assembler::mulss, &Assembler::mulsd,
            &Assembler::mulss, &Assembler::mulsd,
            masm);
        break;
      case Express::DIV:
        GenerateXMMFltOp(instr,
            &Assembler::divss, &Assembler::divsd,
            &Assembler::divss, &Assembler::divsd,
            masm);
        break;
      case Express::MINIMUM:
        GenerateXMMFltOp(instr,
            &Assembler::minss, &Assembler::minsd,
            &Assembler::minss, &Assembler::minsd,
            masm);
        break;
      case Express::MAXIMUM:
        GenerateXMMFltOp(instr,
            &Assembler::maxss, &Assembler::maxsd,
            &Assembler::maxss, &Assembler::maxsd,
            masm);
        break;
      case Express::SQRT:
        GenerateXMMFltOp(instr,
            &Assembler::sqrtss, &Assembler::sqrtsd,
            &Assembler::sqrtss, &Assembler::sqrtsd,
            masm, 0);
        break;
      case Express::CMPEQOQ:
        GenerateCompare(instr, masm, CMP_EQ_OQ);
        break;
      case Express::CMPNEUQ:
        GenerateCompare(instr, masm, CMP_NEQ_UQ);
        break;
      case Express::CMPLTOQ:
        GenerateCompare(instr, masm, CMP_LT_OQ);
        break;
      case Express::CMPLEOQ:
        GenerateCompare(instr, masm, CMP_LE_OQ);
        break;
      case Express::CMPGTOQ:
        GenerateCompare(instr, masm, CMP_GT_OQ);
        break;
      case Express::CMPGEOQ:
        GenerateCompare(instr, masm, CMP_GE_OQ);
        break;
      case Express::COND:
        GenerateConditional(instr, masm);
        break;
      case Express::SELECT:
        GenerateSelect(instr, masm);
        break;
      case Express::BITAND:
      case Express::BITOR:
      case Express::AND:
      case Express::OR:
      case Express::XOR:
      case Express::ANDNOT:
      case Express::NOT:
        GenerateRegisterOp(instr, masm);
        break;
      case Express::FLOOR:
        if (CPU::Enabled(SSE4_1)) {
          GenerateXMMFltOp(instr,
              &Assembler::roundss, &Assembler::roundsd,
              &Assembler::roundss, &Assembler::roundsd,
              round_down, masm);
        } else {
          UNSUPPORTED;
        }
        break;
      case Express::CVTFLTINT:
      case Express::CVTINTFLT:
        if (CPU::Enabled(SSE2)) {
          GenerateRegisterOp(instr, masm);
        } else {
          UNSUPPORTED;
        }
        break;
      case Express::CVTEXPINT:
        GenerateShift(instr, masm, false, type_ == DT_FLOAT ? 23 : 52);
        break;
      case Express::CVTINTEXP:
        GenerateShift(instr, masm, true, type_ == DT_FLOAT ? 23 : 52);
        break;
      case Express::SUBINT:
        GenerateRegisterOp(instr, masm);
        break;
      case Express::SUM:
        GenerateXMMFltAccOp(instr,
            &Assembler::addss, &Assembler::addsd,
            &Assembler::addss, &Assembler::addsd,
            masm);
        break;
      case Express::PRODUCT:
        GenerateXMMFltAccOp(instr,
            &Assembler::mulss, &Assembler::mulsd,
            &Assembler::mulss, &Assembler::mulsd,
            masm);
        break;
      case Express::MIN:
        GenerateXMMFltAccOp(instr,
            &Assembler::minss, &Assembler::minsd,
            &Assembler::minss, &Assembler::minsd,
            masm);
        break;
      case Express::MAX:
        GenerateXMMFltAccOp(instr,
            &Assembler::maxss, &Assembler::maxsd,
            &Assembler::maxss, &Assembler::maxsd,
            masm);
        break;
      default: UNSUPPORTED;
    }
  }

  // Generate left/right shift.
  void GenerateShift(Express::Op *instr, MacroAssembler *masm,
                     bool left, int bits) {
    // Move argument to destination register
    CHECK(instr->dst != -1);
    if (instr->src != -1) {
      __ movapd(xmm(instr->dst), xmm(instr->src));
    } else {
      switch (type_) {
        case DT_FLOAT:
          __ movss(xmm(instr->dst), addr(instr->args[0]));
          break;
        case DT_DOUBLE:
          __ movsd(xmm(instr->dst), addr(instr->args[0]));
          break;
        default: UNSUPPORTED;
      }
    }

    // Shift xmm register.
    switch (type_) {
      case DT_FLOAT:
        if (CPU::Enabled(SSE2)) {
          if (left) {
            __ pslld(xmm(instr->dst), bits);
          } else {
            __ psrld(xmm(instr->dst), bits);
          }
        } else {
          UNSUPPORTED;
        }
        break;
      case DT_DOUBLE:
        if (CPU::Enabled(SSE2)) {
          if (left) {
            __ psllq(xmm(instr->dst), bits);
          } else {
            __ psrlq(xmm(instr->dst), bits);
          }
        } else {
          UNSUPPORTED;
        }
        break;
      default: UNSUPPORTED;
    }
  }

  // Generate compare.
  void GenerateCompare(Express::Op *instr, MacroAssembler *masm, int8 code) {
    GenerateXMMFltOp(instr,
        &Assembler::cmpss, &Assembler::cmpsd,
        &Assembler::cmpss, &Assembler::cmpsd,
        code, masm);
  }

  // Generate scalar op that loads memory operands into a register first.
  void GenerateRegisterOp(Express::Op *instr, MacroAssembler *masm) {
    CHECK(instr->dst != -1);
    XMMRegister dst = xmm(instr->dst);
    XMMRegister src;
    if (instr->src != -1) {
      src = xmm(instr->src);
    } else {
      src = xmmaux(0);
    }

    switch (type_) {
      case DT_FLOAT:
        if (instr->src == -1) {
          __ movss(src, addr(instr->args[1]));
        }
        switch (instr->type) {
          case Express::CVTFLTINT: __ cvttps2dq(dst, src); break;
          case Express::CVTINTFLT: __ cvtdq2ps(dst, src); break;
          case Express::SUBINT: __ psubd(dst, src); break;
          case Express::BITAND:
          case Express::AND:
            __ andps(dst, src);
            break;
          case Express::BITOR:
          case Express::OR:
            __ orps(dst, src);
            break;
          case Express::XOR:
            __ xorps(dst, src);
            break;
          case Express::ANDNOT:
            __ andnps(dst, src);
            break;
          case Express::NOT:
            __ movl(aux(0), Immediate(-1));
            if (dst.code() == src.code()) {
              __ movd(xmmaux(1), aux(0));
              __ xorps(dst, xmmaux(1));
            } else {
              __ movd(dst, aux(0));
              __ xorps(dst, src);
            }
            break;
          default: UNSUPPORTED;
        }
        break;
      case DT_DOUBLE:
        if (instr->src == -1) {
          __ movsd(src, addr(instr->args[1]));
        }
        switch (instr->type) {
          case Express::CVTFLTINT: __ cvttpd2dq(dst, src); break;
          case Express::CVTINTFLT: __ cvtdq2pd(dst, src); break;
          case Express::SUBINT: __ psubq(dst, src); break;
          case Express::BITAND:
          case Express::AND:
            __ andpd(dst, src);
            break;
          case Express::BITOR:
          case Express::OR:
            __ orpd(dst, src);
            break;
          case Express::XOR:
            __ xorpd(dst, src);
            break;
          case Express::ANDNOT:
            __ andnpd(dst, src);
            break;
          case Express::NOT:
            __ movq(aux(0), Immediate(-1));
            if (dst.code() == src.code()) {
              __ movq(xmmaux(1), aux(0));
              __ xorpd(dst, xmmaux(1));
            } else {
              __ movd(dst, aux(0));
              __ xorpd(dst, src);
            }
            break;
          default: UNSUPPORTED;
        }
        break;
      default: UNSUPPORTED;
    }
  }

  // Generate conditional.
  void GenerateConditional(Express::Op *instr, MacroAssembler *masm) {
    CHECK(instr->dst != -1);
    CHECK(instr->src != -1);
    CHECK(instr->mask != -1);
    Label l1, l2;
    __ ptest(xmm(instr->mask), xmm(instr->mask));
    __ j(zero, &l1);
    __ movaps(xmm(instr->dst), xmm(instr->src));
    __ jmp(&l2);
    __ bind(&l1);
    if (instr->src != -1) {
      __ movaps(xmm(instr->dst), xmm(instr->src2));
    } else if (type_ == DT_FLOAT) {
      __ movss(xmm(instr->dst), addr(instr->args[2]));
    } else if (type_ == DT_DOUBLE) {
      __ movsd(xmm(instr->dst), addr(instr->args[2]));
    } else {
      UNSUPPORTED;
    }
    __ bind(&l2);
  }

  // Generate masked select.
  void GenerateSelect(Express::Op *instr, MacroAssembler *masm) {
    CHECK(instr->dst != -1);
    CHECK(instr->mask != -1);
    Label l1, l2;
    switch (type_) {
      case DT_FLOAT: {
        __ ptest(xmm(instr->mask), xmm(instr->mask));
        __ j(not_zero, &l1);
        __ xorps(xmm(instr->dst), xmm(instr->dst));
        if (instr->src == instr->dst) {
          __ bind(&l1);
        } else {
          __ jmp(&l2);
          __ bind(&l1);
          if (instr->src != -1) {
            __ movaps(xmm(instr->dst), xmm(instr->src));
          } else {
            __ movss(xmm(instr->dst), addr(instr->args[1]));
          }
        }
        __ bind(&l2);
        break;
      }
      case DT_DOUBLE: {
        __ ptest(xmm(instr->mask), xmm(instr->mask));
        __ j(not_zero, &l1);
        __ xorpd(xmm(instr->dst), xmm(instr->dst));
        if (instr->src == instr->dst) {
          __ bind(&l1);
        } else {
          __ jmp(&l2);
          __ bind(&l1);
          if (instr->src != -1) {
            __ movaps(xmm(instr->dst), xmm(instr->src));
          } else {
            __ movsd(xmm(instr->dst), addr(instr->args[1]));
          }
        }
        __ bind(&l2);
        break;
      }
      default: UNSUPPORTED;
    }
  }

  // Generate code for reduction operation.
  void GenerateReduce(Express::Op *instr, MacroAssembler *masm) override {
    switch (type_) {
      case DT_FLOAT:
        if (instr->dst != -1) {
          __ movss(xmm(instr->dst), xmm(instr->acc));
        } else {
          __ movss(addr(instr->result), xmm(instr->acc));
        }
        break;
      case DT_DOUBLE:
        if (instr->dst != -1) {
          __ movsd(xmm(instr->dst), xmm(instr->acc));
        } else {
          __ movsd(addr(instr->result), xmm(instr->acc));
        }
        break;
      default: UNSUPPORTED;
    }
  }
};

ExpressionGenerator *CreateScalarFltSSEGenerator() {
  return new ScalarFltSSEGenerator();
}

}  // namespace myelin
}  // namespace sling

