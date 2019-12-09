#include <asmjit/asmjit.h>
#include <cpuinfo.h>
#include <immintrin.h>
#include <cassert>
#include <cmath>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include "./CodeCache.h"
#include "./RefImplementations.h"
#include "fbgemm/Fbgemm.h"

namespace fbgemm {

namespace {

namespace x86 = asmjit::x86;

template <typename inType = std::uint8_t, typename indxType = std::int64_t>
class ReturnFunctionSignature {
 public:
  using jit_embedding_kernel = bool (*)(
      std::int64_t output_size,
      std::int64_t index_size,
      std::int64_t data_size,
      const inType* input,
      const indxType* indices,
      const int* lengths,
      const float* weights,
      float* out);
};

// A trait class to handle ISA specifics
template <inst_set_t instSet>
struct InstSetTrait {};

template <>
struct InstSetTrait<inst_set_t::avx2> {
  typedef x86::Ymm vec_reg_t;

  constexpr static int VLEN = 8;
  constexpr static int NUM_VEC_REG = 16;
  static const vec_reg_t GetVecReg(int idx) {
    return vec_reg_t(idx);
  }
};

template <>
struct InstSetTrait<inst_set_t::avx512> {
  typedef x86::Zmm vec_reg_t;

  constexpr static int VLEN = 16;
  constexpr static int NUM_VEC_REG = 32;
  static const vec_reg_t GetVecReg(int idx) {
    return vec_reg_t(idx);
  }
};

template <typename inType = std::uint8_t, typename indxType = std::int64_t>
class GenEmbeddingSpMDMLookup {
 public:
  GenEmbeddingSpMDMLookup() {}
  template <inst_set_t instSet>
  typename ReturnFunctionSignature<inType, indxType>::jit_embedding_kernel
  getOrCreate(
      int block_size,
      bool has_weight,
      bool is_weight_positional,
      bool normalize_by_lengths,
      int prefetch);

 private:
  static asmjit::JitRuntime& runtime() {
    static asmjit::JitRuntime rt; //< JIT Runtime for asmjit,
                                  // depents on other static
                                  // variables.  Required to prevent
                                  // initialization order fiasco
    return rt;
  }

  static std::mutex rtMutex_; ///< Controll access to runtime;

  // The hash depends on embedding dimension (block size), weighted sls,
  // positional weights, normalize by lenths, and prefetch distance, is8bit
  static CodeCache<
      std::tuple<int, bool, bool, bool, int, bool>,
      typename ReturnFunctionSignature<inType, indxType>::jit_embedding_kernel>
      codeCache_; ///< JIT Code Cache for reuse.

  // These are the registers shared
  // between uint8 and fp32 implementations
  x86::Gp output_size;
  x86::Gp index_size;
  x86::Gp data_size;
  x86::Gp input;
  x86::Gp indices;
  x86::Gp lengths;
  x86::Gp weights;
  x86::Gp out;
  x86::Gp scratchReg1_;
  x86::Gp scratchReg1D_;
  x86::Gp scratchReg2_;
  x86::Gp scratchReg3_;
  x86::Gpd lengths_R_;

}; // GenEmbeddingSpmDMLookup

template <typename inType, typename indxType>
std::mutex GenEmbeddingSpMDMLookup<inType, indxType>::rtMutex_;

template <typename inType, typename indxType>
CodeCache<
    std::tuple<int, bool, bool, bool, int, bool>,
    typename ReturnFunctionSignature<inType, indxType>::jit_embedding_kernel>
    GenEmbeddingSpMDMLookup<inType, indxType>::codeCache_;

template <typename inType, typename indxType>
template <inst_set_t instSet>
typename ReturnFunctionSignature<inType, indxType>::jit_embedding_kernel
GenEmbeddingSpMDMLookup<inType, indxType>::getOrCreate(
    int block_size,
    bool has_weight,
    bool is_weight_positional,
    bool normalize_by_lengths,
    int prefetch) {
  bool is8bit = std::is_same<inType, std::uint8_t>::value;
  std::tuple<int, bool, bool, bool, int, bool> kernelSig = std::make_tuple(
      block_size,
      has_weight,
      is_weight_positional,
      normalize_by_lengths,
      prefetch,
      is8bit);

  return codeCache_.getOrCreate(
      kernelSig,
      [&]() ->
      typename ReturnFunctionSignature<inType, indxType>::jit_embedding_kernel {
        // TODO: Make this tunable
        int pref_dist = prefetch;
        bool areIndices64b = std::is_same<indxType, std::int64_t>::value;

        asmjit::CodeHolder code;
        code.init(runtime().codeInfo());
        x86::Assembler assembler(&code);
        x86::Emitter* a = assembler.as<x86::Emitter>();
#if defined(FBGEMM_LOG_CODE)
        std::string filename = "embeddinglookup_";
        filename += "_emd_dim_" + std::to_string(block_size);
        if (!areIndices64b)
          filename += "_32bit";
        if (areIndices64b)
          filename += "_64bit";
        if (instSet == inst_set_t::avx2)
          filename += "_avx2";
        if (instSet == inst_set_t::avx512)
          filename += "_avx512";
        if (prefetch)
          filename += "_prefetch";
        if (has_weight)
          filename += "_hasweight";
        if (normalize_by_lengths)
          filename += "_normalize_by_lengths";
        filename += ".txt";
        FILE* codeLogFile = fopen(filename.c_str(), "w");
        asmjit::FileLogger* codeLogger = new asmjit::FileLogger(codeLogFile);
        code.setLogger(codeLogger);
#endif
        // arguments to the function created
        output_size = a->zdi();
        // index_size will be overwritten to hold the end address of indices
        index_size = a->zsi();
        data_size = a->zdx();
        input = a->zcx();
        indices = a->gpz(8);
        lengths = a->gpz(9);
        weights = a->gpz(10);
        out = a->gpz(11);
        lengths_R_ = a->gpz(12).r32();

        scratchReg1_ = a->gpz(13);
        scratchReg1D_ = a->gpz(13).r32();
        scratchReg2_ = a->gpz(14);
        scratchReg3_ = a->gpz(15);

        asmjit::FuncDetail func;

        func.init(asmjit::FuncSignatureT<
                  bool,
                  std::int64_t, // output_size
                  std::int64_t, // index_size
                  std::int64_t, // data_size
                  const inType*, // input uint8_t or float
                  const indxType*, // indices
                  const int*, // lengths
                  const float*, // weights
                  float*>(asmjit::CallConv::kIdHost));

        asmjit::FuncFrame frame;
        frame.init(func);

        if (instSet == inst_set_t::avx2) {
          frame.setDirtyRegs(
              x86::Reg::kGroupVec,
              asmjit::Support::bitMask(0, 1, 2, 3, 4, 5, 6, 7) |
                  asmjit::Support::bitMask(8, 9, 10, 11, 12, 13, 14, 15));
        } else {
          frame.setDirtyRegs(
              x86::Reg::kGroupVec,
              asmjit::Support::bitMask(0, 1, 2, 3, 4, 5, 6, 7) |
                  asmjit::Support::bitMask(8, 9, 10, 11, 12, 13, 14, 15) |
                  asmjit::Support::bitMask(16, 17, 18, 19, 20, 21, 22, 23) |
                  asmjit::Support::bitMask(24, 25, 26, 27, 28, 29, 30, 31));
        }

        frame.setDirtyRegs(
            x86::Reg::kGroupGp,
            asmjit::Support::bitMask(8, 9, 10, 11, 12, 13, 14, 15));

        asmjit::FuncArgsAssignment args(&func);
        args.assignAll(
            output_size,
            index_size,
            data_size,
            input,
            indices,
            lengths,
            weights,
            out);

        args.updateFuncFrame(frame);
        frame.finalize();

        a->emitProlog(frame);
        a->emitArgsAssignment(frame, args);

        InstSetTrait<instSet> inst_trait;
        constexpr int vlen = inst_trait.VLEN;
        constexpr int NUM_VEC_REG = inst_trait.NUM_VEC_REG;
        int unroll_factor = NUM_VEC_REG;

        typedef typename InstSetTrait<instSet>::vec_reg_t vec_reg_t;

        int num_vec_regs_per_block = (block_size + vlen - 1) / vlen;
        int remainder = block_size % vlen;

        vec_reg_t scale_vreg; // holds scale
        vec_reg_t bias_vreg; // holds bias
        vec_reg_t w_vreg; // for weighted sls -- weights
        vec_reg_t
            vlen_inv_vreg; // used for normalize by lengths -- 1/ lengths[i]
        vec_reg_t temp_vreg_cvt_uint2flt; // for converting uint8->int32->float
        x86::Ymm mask_vreg; // mask for avx2
        x86::Xmm temp_xmm; // a shadow xmm reg of temp_vreg_cvt_uint2flt
        x86::Xmm vlen_inv_vreg_xmm; // a shadow xmm reg of vlen_inv_vreg
        vec_reg_t temp_vreg;

        if (is8bit) {
          // We need 2 vec registers for 1. scale 2. bias
          --unroll_factor;
          scale_vreg = inst_trait.GetVecReg(unroll_factor);
          --unroll_factor;
          bias_vreg = inst_trait.GetVecReg(unroll_factor);

          // We need 1 vec register to convert from uint8->int32->float
          --unroll_factor;
          temp_vreg_cvt_uint2flt = inst_trait.GetVecReg(unroll_factor);
          if (instSet == inst_set_t::avx2) {
            temp_xmm = x86::Xmm(temp_vreg_cvt_uint2flt.id());
          }
        } else {
          temp_xmm = x86::xmm0;
        }

        if (has_weight) {
          --unroll_factor;
          w_vreg = inst_trait.GetVecReg(unroll_factor);
        }

        if (remainder) {
          // AVX512 doesn't need to use vector register for masking
          unroll_factor -= (instSet == inst_set_t::avx2 ? 2 : 1);
          temp_vreg = inst_trait.GetVecReg(unroll_factor);
          if (instSet == inst_set_t::avx2) {
            mask_vreg = x86::ymm(unroll_factor + 1);
          }
        }

        if (normalize_by_lengths) {
          --unroll_factor;
          if (is8bit) {
            vlen_inv_vreg = inst_trait.GetVecReg(
                remainder ? unroll_factor + 1 : unroll_factor);
          } else {
            vlen_inv_vreg = inst_trait.GetVecReg(unroll_factor);
          }
          if (instSet == inst_set_t::avx2) {
            vlen_inv_vreg_xmm = x86::Xmm(vlen_inv_vreg.id());
          }
        }

        if (remainder) {
          if (instSet == inst_set_t::avx2) {
            a->lea(
                x86::rsp,
                x86::dword_ptr(x86::rsp, (int32_t)(-vlen * sizeof(int32_t))));
            for (int i = 0; i < remainder; i++) {
              a->mov(x86::dword_ptr(x86::rsp, i * sizeof(int32_t)), -1);
            }
            for (int i = remainder; i < vlen; i++) {
              a->mov(x86::dword_ptr(x86::rsp, i * sizeof(int32_t)), 0);
            }
            a->vmovups(mask_vreg, x86::dword_ptr(x86::rsp));
            a->lea(
                x86::rsp,
                x86::dword_ptr(x86::rsp, (int32_t)(vlen * sizeof(int32_t))));

          } else {
            a->mov(scratchReg1_, (1 << remainder) - 1);
            a->kmovw(x86::k(1), scratchReg1_);
          }
        }

        // Compute the end address of indices
        a->imul(
            scratchReg1_,
            index_size,
            static_cast<asmjit::Imm>(sizeof(indxType)));
        a->add(scratchReg1_, indices);
        a->mov(index_size, scratchReg1_);

        asmjit::Label exit = a->newLabel();
        asmjit::Label error = a->newLabel();
        asmjit::Label LoopRangeIndexBegin = a->newLabel();
        asmjit::Label LoopRangeIndexEnd = a->newLabel();

        if (has_weight && is_weight_positional) {
          a->mov(scratchReg3_, weights);
        }

        // rangeIndex loop begins (iterate output_size times)
        a->bind(LoopRangeIndexBegin);
        a->dec(output_size);
        a->jl(LoopRangeIndexEnd);

        if (normalize_by_lengths) {
          asmjit::Label IfLengthsBegin = a->newLabel();
          asmjit::Label IfLengthsEnd = a->newLabel();
          a->bind(IfLengthsBegin);
          a->cmp(x86::dword_ptr(lengths), 1);
          a->vxorps(vlen_inv_vreg, vlen_inv_vreg, vlen_inv_vreg);
          a->jl(IfLengthsEnd);

          if (instSet == inst_set_t::avx2) {
            a->mov(lengths_R_, 1);
            a->cvtsi2ss(vlen_inv_vreg_xmm, lengths_R_);
            a->cvtsi2ss(temp_xmm, x86::dword_ptr(lengths));
            a->divss(vlen_inv_vreg_xmm, temp_xmm);
            // a->cvtsi2ss(x86::xmm0, x86::dword_ptr(lengths));
            // a->divss(vlen_inv_vreg_xmm, x86::xmm0);
            a->vpbroadcastd(vlen_inv_vreg, vlen_inv_vreg_xmm);
          } else { // avx512
            vec_reg_t temp_zmm = inst_trait.GetVecReg(0);
            a->mov(lengths_R_, 1);
            a->cvtsi2ss(x86::xmm0, lengths_R_);
            a->vpbroadcastd(vlen_inv_vreg, x86::xmm0);
            if (is8bit) {
              a->vpbroadcastd(temp_vreg_cvt_uint2flt, x86::dword_ptr(lengths));
              a->vcvtdq2ps(temp_vreg_cvt_uint2flt, temp_vreg_cvt_uint2flt);
              a->vdivps(vlen_inv_vreg, vlen_inv_vreg, temp_vreg_cvt_uint2flt);
            } else {
              a->vpbroadcastd(temp_zmm, x86::dword_ptr(lengths));
              a->vcvtdq2ps(temp_zmm, temp_zmm);
              a->vdivps(vlen_inv_vreg, vlen_inv_vreg, temp_zmm);
            }
          }
          a->bind(IfLengthsEnd);
        }

        for (int vec_idx = 0; vec_idx < num_vec_regs_per_block;
             vec_idx += unroll_factor) {
          int cur_unroll_factor =
              std::min(unroll_factor, num_vec_regs_per_block - vec_idx);

          // Initialize output regs
          for (int v = 0; v < cur_unroll_factor; ++v) {
            vec_reg_t out_vreg = inst_trait.GetVecReg(v);
            a->vxorps(out_vreg, out_vreg, out_vreg);
          }

          a->mov(lengths_R_, x86::dword_ptr(lengths));

          // Array out of bound check
          a->imul(
              scratchReg1_,
              lengths_R_,
              static_cast<asmjit::Imm>(sizeof(indxType)));

          a->add(scratchReg1_, indices);
          a->cmp(scratchReg1_, index_size);
          a->jg(error);

          asmjit::Label LoopDataIndexBegin = a->newLabel();
          asmjit::Label LoopDataIndexEnd = a->newLabel();

          if (has_weight && is_weight_positional) {
            a->mov(weights, scratchReg3_);
          }
          // dataIndex loop begins (iterate lengths_R_ times)
          a->bind(LoopDataIndexBegin);
          a->dec(lengths_R_);
          a->jl(LoopDataIndexEnd);

          // Array out of bound check
          if (areIndices64b) {
            a->mov(scratchReg1_, x86::qword_ptr(indices));
          } else {
            a->mov(scratchReg1_.r32(), x86::dword_ptr(indices));
          }
          a->cmp(scratchReg1_, 0);
          a->jl(error);
          a->cmp(scratchReg1_, data_size);
          a->jge(error);

          int fused_block_size =
              block_size * sizeof(uint8_t) + 2 * sizeof(float);
          if (is8bit) {
            a->imul(scratchReg1_, static_cast<asmjit::Imm>(fused_block_size));
          } else {
            a->imul(
                scratchReg1_,
                static_cast<asmjit::Imm>(block_size * sizeof(float)));
          }

          if (pref_dist) {
            asmjit::Label pref_dist_reset_start = a->newLabel();
            asmjit::Label pref_dist_reset_end = a->newLabel();
            // out of bound handling for prefetch
            a->mov(scratchReg2_, indices);
            a->add(
                scratchReg2_,
                static_cast<asmjit::Imm>(pref_dist * sizeof(indxType)));
            a->cmp(scratchReg2_, index_size);
            a->jge(pref_dist_reset_start);

            if (areIndices64b) {
              a->mov(
                  scratchReg2_,
                  x86::qword_ptr(indices, pref_dist * sizeof(indxType)));
            } else {
              a->mov(
                  scratchReg2_.r32(),
                  x86::dword_ptr(indices, pref_dist * sizeof(indxType)));
            }

            a->cmp(scratchReg2_, 0);
            a->jl(pref_dist_reset_start);
            a->cmp(scratchReg2_, data_size);
            a->jge(pref_dist_reset_start);

            // everything is okay, prefetch a few rows ahead
            a->jmp(pref_dist_reset_end);

            a->bind(pref_dist_reset_start);
            // things are not okay just get the current row
            // this can be improved to getting the max dist row.
            if (areIndices64b) {
              a->mov(scratchReg2_, x86::qword_ptr(indices));
            } else {
              a->mov(scratchReg2_.r32(), x86::dword_ptr(indices));
            }

            a->bind(pref_dist_reset_end);
            if (is8bit) {
              // This has to be fused_block_size
              a->imul(scratchReg2_, static_cast<asmjit::Imm>(fused_block_size));
            } else {
              a->imul(
                  scratchReg2_,
                  static_cast<asmjit::Imm>(block_size * sizeof(float)));
            }
          }

          a->add(indices, static_cast<asmjit::Imm>(sizeof(indxType)));

          // broadcast the scale
          x86::Mem scale_src, bias_src;
          if (is8bit) {
            scale_src = x86::dword_ptr(
                input, scratchReg1_, 0, block_size * sizeof(uint8_t));
            bias_src = x86::dword_ptr(
                input,
                scratchReg1_,
                0,
                block_size * sizeof(uint8_t) + sizeof(float));
            a->vbroadcastss(scale_vreg, scale_src);
            a->vbroadcastss(bias_vreg, bias_src);
          }

          if (has_weight) {
            a->vbroadcastss(w_vreg, x86::dword_ptr(weights));
            if (is8bit) {
              a->vmulps(scale_vreg, scale_vreg, w_vreg);
              a->vmulps(bias_vreg, bias_vreg, w_vreg);
            }
            a->add(weights, static_cast<asmjit::Imm>(sizeof(float)));
          }

          // The main computation
          for (int v = 0; v < cur_unroll_factor; ++v) {
            auto src_addr = x86::dword_ptr(
                input,
                scratchReg1_,
                0,
                (vec_idx + v) * (vlen) * sizeof(inType));
            vec_reg_t out_vreg = inst_trait.GetVecReg(v);

            // For 8bit SLS convert usigned 8-bit to 32bit int, then to float
            // multiply with scale and then add with bias
            if (is8bit) {
              if (remainder && vec_idx + v == num_vec_regs_per_block - 1 &&
                  instSet == inst_set_t::avx512) {
                a->k(x86::k(1)).vpmovzxbd(temp_vreg_cvt_uint2flt, src_addr);
                a->k(x86::k(1)).vcvtdq2ps(
                    temp_vreg_cvt_uint2flt, temp_vreg_cvt_uint2flt);
                a->k(x86::k(1)).vaddps(out_vreg, out_vreg, bias_vreg);
                a->k(x86::k(1)).vfmadd231ps(
                    out_vreg, temp_vreg_cvt_uint2flt, scale_vreg);
              } else {
                // We don't use a mask for AVX2 since we can use the extra
                //"padding" of the 2 floats (= 8 chars) scale and bias
                // this ensures we never access out of bound data
                a->vpmovzxbd(temp_vreg_cvt_uint2flt, src_addr);
                a->vcvtdq2ps(temp_vreg_cvt_uint2flt, temp_vreg_cvt_uint2flt);
                a->vaddps(out_vreg, out_vreg, bias_vreg);
                a->vfmadd231ps(out_vreg, temp_vreg_cvt_uint2flt, scale_vreg);
              }
            } else {
              // This part for FP32 SLS
              if (remainder && vec_idx + v == num_vec_regs_per_block - 1 &&
                  instSet == inst_set_t::avx2) {
                a->vmaskmovps(
                    x86::ymm(temp_vreg.id()),
                    x86::ymm(mask_vreg.id()),
                    src_addr);
              }
              if (has_weight) {
                if (remainder && vec_idx + v == num_vec_regs_per_block - 1) {
                  if (instSet == inst_set_t::avx2) {
                    a->vfmadd231ps(out_vreg, w_vreg, temp_vreg);
                  } else {
                    a->k(x86::k(1)).vfmadd231ps(out_vreg, w_vreg, src_addr);
                  }
                } else {
                  a->vfmadd231ps(out_vreg, w_vreg, src_addr);
                }
              } else {
                if (remainder && vec_idx + v == num_vec_regs_per_block - 1) {
                  if (instSet == inst_set_t::avx2) {
                    a->vaddps(out_vreg, out_vreg, temp_vreg);
                  } else {
                    a->k(x86::k(1)).vaddps(out_vreg, out_vreg, src_addr);
                  }
                } else {
                  a->vaddps(out_vreg, out_vreg, src_addr);
                }
              }
            }

            if (pref_dist && v % (64 / (vlen * sizeof(inType))) == 0) {
              a->prefetcht0(x86::dword_ptr(
                  input,
                  scratchReg2_,
                  0,
                  (vec_idx + v) * vlen * sizeof(inType)));
            }
          }

          a->jmp(LoopDataIndexBegin);
          a->bind(LoopDataIndexEnd);

          // This loop is for writing back out_vreg (results)
          // back to memory
          for (int v = 0; v < cur_unroll_factor; ++v) {
            auto dst_addr =
                x86::dword_ptr(out, (vec_idx + v) * vlen * sizeof(float));
            vec_reg_t out_vreg = inst_trait.GetVecReg(v);

            if (normalize_by_lengths) {
              a->vmulps(out_vreg, out_vreg, vlen_inv_vreg);
            }

            if (remainder && vec_idx + v == num_vec_regs_per_block - 1) {
              if (instSet == inst_set_t::avx2) {
                a->vmaskmovps(dst_addr, mask_vreg, x86::Ymm(out_vreg.id()));
              } else {
                a->k(x86::k(1)).vmovups(dst_addr, out_vreg);
              }
            } else {
              a->vmovups(dst_addr, out_vreg);
            }
          }

          if (vec_idx + unroll_factor < num_vec_regs_per_block) {
            // Reset lengths_R_, indices, weights to run the dataIndex loop
            // again
            a->mov(lengths_R_, x86::dword_ptr(lengths));

            if (has_weight) {
              a->imul(
                  scratchReg1_,
                  lengths_R_,
                  static_cast<asmjit::Imm>(sizeof(float)));
              a->sub(weights, scratchReg1_);
              a->imul(
                  scratchReg1_,
                  static_cast<asmjit::Imm>(sizeof(indxType) / sizeof(float)));
              a->sub(indices, scratchReg1_);
            } else {
              a->imul(
                  scratchReg1_,
                  lengths_R_,
                  static_cast<asmjit::Imm>(sizeof(indxType)));
              a->sub(indices, scratchReg1_);
            }
          }
        }

        a->add(lengths, static_cast<asmjit::Imm>(sizeof(int)));
        a->add(out, static_cast<asmjit::Imm>(block_size * sizeof(float)));

        a->jmp(LoopRangeIndexBegin);
        a->bind(LoopRangeIndexEnd);

        a->cmp(indices, index_size);
        a->jne(error);
        a->mov(x86::eax, true);
        a->jmp(exit);
        a->bind(error);
        a->mov(x86::eax, false);
        a->bind(exit);

        a->emitEpilog(frame);

        // jit_fused8bitembedding_kernel fn;
        typename ReturnFunctionSignature<inType, indxType>::jit_embedding_kernel
            fn;
        asmjit::Error err;
        {
          std::unique_lock<std::mutex> lock(rtMutex_);
          err = runtime().add(&fn, &code);
        }
        if (err) {
          std::cout << "Error: in fn add" << std::endl;
          return nullptr;
        }

#if defined(FBGEMM_LOG_CODE)
        fclose(codeLogFile);
        delete codeLogger;
#endif
        return fn;
      });
}

} // namespace

template <typename inType, typename indxType>
bool EmbeddingSpMDM(
    const std::int64_t block_size,
    const std::int64_t output_size,
    const std::int64_t index_size,
    const std::int64_t data_size,
    const inType* input,
    const indxType* indices,
    const int* lengths,
    const float* weights, // optional, can be null for non-weighted sum
    bool normalize_by_lengths,
    float* out,
    int prefetch,
    bool IS_WEIGHT_POSITIONAL) {
  static GenEmbeddingSpMDMLookup<inType, indxType> kernel_generator;
  if (!cpuinfo_initialize()) {
    throw std::runtime_error("Failed to initialize cpuinfo!");
  }
  typename ReturnFunctionSignature<inType, indxType>::jit_embedding_kernel fn;
  if (fbgemmHasAvx512Support()) {
    fn = kernel_generator.template getOrCreate<inst_set_t::avx512>(
        block_size,
        weights ? true : false,
        IS_WEIGHT_POSITIONAL,
        normalize_by_lengths,
        prefetch);
  } else if (fbgemmHasAvx2Support()) {
    fn = kernel_generator.template getOrCreate<inst_set_t::avx2>(
        block_size,
        weights ? true : false,
        IS_WEIGHT_POSITIONAL,
        normalize_by_lengths,
        prefetch);
  } else {
#ifdef VLOG
    VLOG(0) << "AVX2 or AVX512 not found, taking the slow path";
#endif
    auto success = EmbeddingSpMDM_ref(
        block_size,
        output_size,
        index_size,
        data_size,
        input,
        indices,
        lengths,
        weights,
        normalize_by_lengths,
        out);
    return success;
  }

  auto success =
      fn(output_size,
         index_size,
         data_size,
         input,
         indices,
         lengths,
         weights,
         out);
  return success;
}

template bool EmbeddingSpMDM(
    const std::int64_t block_size,
    const std::int64_t output_size,
    const std::int64_t index_size,
    const std::int64_t data_size,
    const float* input,
    const std::int64_t* indices,
    const int* lengths,
    const float* weights, // optional, can be null for non-weighted sum
    bool normalize_by_lengths,
    float* out,
    int prefetch,
    bool IS_WEIGHT_POSITIONAL);

template bool EmbeddingSpMDM(
    const std::int64_t block_size,
    const std::int64_t output_size,
    const std::int64_t index_size,
    const std::int64_t data_size,
    const float* input,
    const std::int32_t* indices,
    const int* lengths,
    const float* weights, // optional, can be null for non-weighted sum
    bool normalize_by_lengths,
    float* out,
    int prefetch,
    bool IS_WEIGHT_POSITIONAL);

template bool EmbeddingSpMDM(
    const std::int64_t block_size,
    const std::int64_t output_size,
    const std::int64_t index_size,
    const std::int64_t data_size,
    const std::uint8_t* input,
    const std::int64_t* indices,
    const int* lengths,
    const float* weights, // optional, can be null for non-weighted sum
    bool normalize_by_lengths,
    float* out,
    int prefetch,
    bool IS_WEIGHT_POSITIONAL);

template bool EmbeddingSpMDM(
    const std::int64_t block_size,
    const std::int64_t output_size,
    const std::int64_t index_size,
    const std::int64_t data_size,
    const std::uint8_t* input,
    const std::int32_t* indices,
    const int* lengths,
    const float* weights, // optional, can be null for non-weighted sum
    bool normalize_by_lengths,
    float* out,
    int prefetch,
    bool IS_WEIGHT_POSITIONAL);

} // namespace fbgemm
