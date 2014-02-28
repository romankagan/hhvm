/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2014 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/jit/simplifier.h"

#include <sstream>
#include <type_traits>
#include <limits>

#include "hphp/runtime/base/smart-containers.h"
#include "hphp/runtime/base/type-conversions.h"
#include "hphp/runtime/vm/jit/guard-relaxation.h"
#include "hphp/runtime/vm/jit/ir-builder.h"
#include "hphp/runtime/vm/hhbc.h"
#include "hphp/runtime/vm/runtime.h"

namespace HPHP {
namespace JIT {

TRACE_SET_MOD(hhir);

//////////////////////////////////////////////////////////////////////

StackValueInfo getStackValue(SSATmp* sp, uint32_t index) {
  FTRACE(5, "getStackValue: idx = {}, {}\n", index, sp->inst()->toString());
  assert(sp->isA(Type::StkPtr));
  IRInstruction* inst = sp->inst();

  switch (inst->op()) {
  case DefInlineSP:
  case DefSP:
    return StackValueInfo { inst, Type::StackElem };

  case ReDefGeneratorSP: {
    auto const extra = inst->extra<ReDefGeneratorSP>();
    auto info = getStackValue(inst->src(0), index);
    if (extra->spansCall) info.spansCall = true;
    return info;
  }

  case StashGeneratorSP:
    return getStackValue(inst->src(1), index);

  case ReDefSP: {
    auto const extra = inst->extra<ReDefSP>();
    auto info = getStackValue(inst->src(0), index);
    if (extra->spansCall) info.spansCall = true;
    return info;
  }

  case PassSP:
  case ExceptionBarrier:
    return getStackValue(inst->src(0), index);

  case SideExitGuardStk:
    if (inst->extra<SideExitGuardData>()->checkedSlot == index) {
      return StackValueInfo { inst, inst->typeParam() };
    }
    return getStackValue(inst->src(0), index);

  case AssertStk:
    // fallthrough
  case CastStk:
    // fallthrough
  case CoerceStk:
    // fallthrough
  case GuardStk:
    // We don't have a value, but we may know the type due to guarding
    // on it.
    if (inst->extra<StackOffset>()->offset == index) {
      return StackValueInfo { inst, inst->typeParam() };
    }
    return getStackValue(inst->src(0), index);

  case CheckStk:
    // CheckStk's resulting type is the intersection of its typeParam
    // with whatever type preceded it.
    if (inst->extra<StackOffset>()->offset == index) {
      Type prevType = getStackValue(inst->src(0), index).knownType;
      return StackValueInfo { inst, inst->typeParam() & prevType};
    }
    return getStackValue(inst->src(0), index);

  case CallArray: {
    if (index == 0) {
      // return value from call
      return StackValueInfo { inst, Type::Gen };
    }
    auto info =
      getStackValue(inst->src(0),
                    // Pushes a return value, pops an ActRec and args Array
                    index -
                      (1 /* pushed */ - (kNumActRecCells + 1) /* popped */));
    info.spansCall = true;
    return info;
  }

  case Call: {
    if (index == 0) {
      // return value from call
      return StackValueInfo { inst, Type::Gen };
    }
    auto info =
      getStackValue(inst->src(0),
                    index -
                    (1 /* pushed */ - kNumActRecCells /* popped */));
    info.spansCall = true;
    return info;
  }

  case SpillStack: {
    int64_t numPushed    = 0;
    int32_t numSpillSrcs = inst->numSrcs() - 2;

    for (int i = 0; i < numSpillSrcs; ++i) {
      SSATmp* tmp = inst->src(i + 2);
      if (index == numPushed) {
        return StackValueInfo { tmp };
      }
      ++numPushed;
    }

    // This is not one of the values pushed onto the stack by this
    // spillstack instruction, so continue searching.
    SSATmp* prevSp = inst->src(0);
    int64_t numPopped = inst->src(1)->getValInt();
    return getStackValue(prevSp,
                         // pop values pushed by spillstack
                         index - (numPushed - numPopped));
  }

  case InterpOne:
  case InterpOneCF: {
    SSATmp* prevSp = inst->src(0);
    auto const& extra = *inst->extra<InterpOneData>();
    int64_t spAdjustment = extra.cellsPopped - extra.cellsPushed;
    switch (extra.opcode) {
    // some instructions are kinda funny and mess with the stack
    // in places other than the top
    case Op::CGetL2:
      if (index == 1) return StackValueInfo { inst, inst->typeParam() };
      if (index == 0) return getStackValue(prevSp, index);
      break;
    case Op::CGetL3:
      if (index == 2) return StackValueInfo { inst, inst->typeParam() };
      if (index < 2)  return getStackValue(prevSp, index);
      break;
    case Op::UnpackCont:
      if (index == 0) return StackValueInfo { inst, Type::Int };
      if (index == 1) return StackValueInfo { inst, Type::Cell };
      break;
    case Op::FPushCufSafe:
      if (index == kNumActRecCells) return StackValueInfo { inst, Type::Bool };
      if (index == kNumActRecCells + 1) return getStackValue(prevSp, 0);
      break;
    case Op::FPushCtor:
    case Op::FPushCtorD:
      if (index == kNumActRecCells) return StackValueInfo { inst, Type::Obj };
      if (index == kNumActRecCells + 1) return getStackValue(prevSp, 0);
    case Op::AsyncAwait:
      if (index == 0) return StackValueInfo { inst, Type::Bool };
      if (index == 1) return StackValueInfo { inst, Type::Cell };
      break;

    default:
      if (index == 0 && inst->hasTypeParam()) {
        return StackValueInfo { inst, inst->typeParam() };
      }
      break;
    }

    // If the index we're looking for is a cell pushed by the InterpOne (other
    // than top of stack), we know nothing about its type.
    if (index < extra.cellsPushed) {
      return StackValueInfo{ inst, Type::StackElem };
    }
    return getStackValue(prevSp, index + spAdjustment);
  }

  case SpillFrame:
  case CufIterSpillFrame:
    // pushes an ActRec
    if (index < kNumActRecCells) {
      return StackValueInfo { inst, Type::StackElem };
    }
    return getStackValue(inst->src(0), index - kNumActRecCells);

  default:
    {
      // Assume it's a vector instruction.  This will assert in
      // minstrBaseIdx if not.
      auto const base = inst->src(minstrBaseIdx(inst));
      assert(base->inst()->op() == LdStackAddr);
      if (base->inst()->extra<LdStackAddr>()->offset == index) {
        MInstrEffects effects(inst);
        assert(effects.baseTypeChanged || effects.baseValChanged);
        return StackValueInfo { inst, effects.baseType.derefIfPtr() };
      }
      return getStackValue(base->inst()->src(0), index);
    }
  }

  not_reached();
}

smart::vector<SSATmp*> collectStackValues(SSATmp* sp, uint32_t stackDepth) {
  smart::vector<SSATmp*> ret;
  ret.reserve(stackDepth);
  for (uint32_t i = 0; i < stackDepth; ++i) {
    auto const value = getStackValue(sp, i).value;
    if (value) {
      ret.push_back(value);
    }
  }
  return ret;
}

//////////////////////////////////////////////////////////////////////

void copyProp(IRInstruction* inst) {
  for (uint32_t i = 0; i < inst->numSrcs(); i++) {
    auto tmp     = inst->src(i);
    auto srcInst = tmp->inst();

    if (srcInst->is(Mov, PassSP, PassFP)) {
      inst->setSrc(i, srcInst->src(0));
    }

    // We're assuming that all of our src instructions have already been
    // copyPropped.
    assert(!inst->src(i)->inst()->is(Mov, PassSP, PassFP));
  }
}

const SSATmp* canonical(const SSATmp* val) {
  return canonical(const_cast<SSATmp*>(val));
}

SSATmp* canonical(SSATmp* value) {
  auto inst = value->inst();

  while (inst->isPassthrough()) {
    value = inst->getPassthroughValue();
    inst = value->inst();
  }
  return value;
}

IRInstruction* findSpillFrame(SSATmp* sp) {
  auto inst = sp->inst();
  while (!inst->is(SpillFrame)) {
    assert(inst->dst()->isA(Type::StkPtr));
    assert(!inst->is(RetAdjustStack, GenericRetDecRefs));
    if (inst->is(DefSP)) return nullptr;
    if (inst->is(InterpOne) && isFPush(inst->extra<InterpOne>()->opcode)) {
      // A non-punted translation of this bytecode would contain a SpillFrame.
      return nullptr;
    }

    // M-instr support opcodes have the previous sp in varying sources.
    if (inst->modifiesStack()) inst = inst->previousStkPtr()->inst();
    else                       inst = inst->src(0)->inst();
  }

  return inst;
}

IRInstruction* findPassFP(IRInstruction* fpInst) {
  while (!fpInst->is(DefFP, DefInlineFP, PassFP)) {
    assert(fpInst->dst()->isA(Type::FramePtr));
    fpInst = fpInst->src(0)->inst();
  }
  return fpInst->is(PassFP) ? fpInst : nullptr;
}

const IRInstruction* frameRoot(const IRInstruction* fpInst) {
  return frameRoot(const_cast<IRInstruction*>(fpInst));
}

IRInstruction* frameRoot(IRInstruction* fpInst) {
  while (!fpInst->is(DefFP, DefInlineFP)) {
    assert(fpInst->dst()->isA(Type::FramePtr));
    fpInst = fpInst->src(0)->inst();
  }
  return fpInst;
}

//////////////////////////////////////////////////////////////////////

template<class... Args> SSATmp* Simplifier::cns(Args&&... cns) {
  return m_irb.cns(std::forward<Args>(cns)...);
}

template<class... Args> SSATmp* Simplifier::gen(Opcode op, Args&&... args) {
  assert(!m_insts.empty());
  return m_irb.gen(op, m_insts.top()->marker(), std::forward<Args>(args)...);
}

template<class... Args> SSATmp* Simplifier::gen(Opcode op, BCMarker marker,
                                                Args&&... args) {
  return m_irb.gen(op, marker, std::forward<Args>(args)...);
}

//////////////////////////////////////////////////////////////////////

SSATmp* Simplifier::simplify(IRInstruction* inst) {
  m_insts.push(inst);
  SCOPE_EXIT {
    assert(m_insts.top() == inst);
    m_insts.pop();
  };

  SSATmp* src1 = inst->numSrcs() < 1 ? nullptr : inst->src(0);
  SSATmp* src2 = inst->numSrcs() < 2 ? nullptr : inst->src(1);

  Opcode opc = inst->op();
  switch (opc) {
  case AbsDbl:    return simplifyAbsDbl(inst);

  case AddInt:    return simplifyAddInt(src1, src2);
  case SubInt:    return simplifySubInt(src1, src2);
  case MulInt:    return simplifyMulInt(src1, src2);
  case AddDbl:    return simplifyAddDbl(src1, src2);
  case SubDbl:    return simplifySubDbl(src1, src2);
  case MulDbl:    return simplifyMulDbl(src1, src2);

  case DivDbl:    return simplifyDivDbl(inst);
  case Mod:       return simplifyMod(src1, src2);
  case BitAnd:    return simplifyBitAnd(src1, src2);
  case BitOr:     return simplifyBitOr(src1, src2);
  case BitXor:    return simplifyBitXor(src1, src2);
  case LogicXor:  return simplifyLogicXor(src1, src2);
  case Shl:       return simplifyShl(inst);
  case Shr:       return simplifyShr(inst);

  case Gt:
  case Gte:
  case Lt:
  case Lte:
  case Eq:
  case Neq:
  case GtInt:
  case GteInt:
  case LtInt:
  case LteInt:
  case EqInt:
  case NeqInt:
  case Same:
  case NSame:
    return simplifyCmp(opc, inst, src1, src2);

  case ConcatCellCell: return simplifyConcatCellCell(inst);
  case ConcatStrStr:  return simplifyConcatStrStr(src1, src2);
  case Mov:           return simplifyMov(src1);
  case Not:           return simplifyNot(src1);
  case ConvBoolToArr: return simplifyConvToArr(inst);
  case ConvDblToArr:  return simplifyConvToArr(inst);
  case ConvIntToArr:  return simplifyConvToArr(inst);
  case ConvStrToArr:  return simplifyConvToArr(inst);
  case ConvArrToBool: return simplifyConvArrToBool(inst);
  case ConvDblToBool: return simplifyConvDblToBool(inst);
  case ConvIntToBool: return simplifyConvIntToBool(inst);
  case ConvStrToBool: return simplifyConvStrToBool(inst);
  case ConvArrToDbl:  return simplifyConvArrToDbl(inst);
  case ConvBoolToDbl: return simplifyConvBoolToDbl(inst);
  case ConvIntToDbl:  return simplifyConvIntToDbl(inst);
  case ConvStrToDbl:  return simplifyConvStrToDbl(inst);
  case ConvArrToInt:  return simplifyConvArrToInt(inst);
  case ConvBoolToInt: return simplifyConvBoolToInt(inst);
  case ConvDblToInt:  return simplifyConvDblToInt(inst);
  case ConvStrToInt:  return simplifyConvStrToInt(inst);
  case ConvBoolToStr: return simplifyConvBoolToStr(inst);
  case ConvDblToStr:  return simplifyConvDblToStr(inst);
  case ConvIntToStr:  return simplifyConvIntToStr(inst);
  case ConvCellToBool:return simplifyConvCellToBool(inst);
  case ConvCellToStr: return simplifyConvCellToStr(inst);
  case ConvCellToInt: return simplifyConvCellToInt(inst);
  case ConvCellToDbl: return simplifyConvCellToDbl(inst);
  case Floor:         return simplifyFloor(inst);
  case Ceil:          return simplifyCeil(inst);
  case Unbox:         return simplifyUnbox(inst);
  case UnboxPtr:      return simplifyUnboxPtr(inst);
  case BoxPtr:        return simplifyBoxPtr(inst);
  case IsType:
  case IsNType:       return simplifyIsType(inst);
  case IsScalarType:  return simplifyIsScalarType(inst);
  case CheckInit:     return simplifyCheckInit(inst);

  case JmpZero:
  case JmpNZero:
    return simplifyCondJmp(inst);

  case JmpGt:
  case JmpGte:
  case JmpLt:
  case JmpLte:
  case JmpEq:
  case JmpNeq:
  case JmpGtInt:
  case JmpGteInt:
  case JmpLtInt:
  case JmpLteInt:
  case JmpEqInt:
  case JmpNeqInt:
  case JmpSame:
  case JmpNSame:
    return simplifyQueryJmp(inst);

  case PrintStr:
  case PrintInt:
  case PrintBool:    return simplifyPrint(inst);
  case DecRef:
  case DecRefNZ:     return simplifyDecRef(inst);
  case IncRef:       return simplifyIncRef(inst);
  case IncRefCtx:    return simplifyIncRefCtx(inst);
  case CheckType:    return simplifyCheckType(inst);
  case AssertType:   return simplifyAssertType(inst);
  case CheckStk:     return simplifyCheckStk(inst);
  case AssertNonNull:return simplifyAssertNonNull(inst);

  case LdCls:        return simplifyLdCls(inst);
  case LdCtx:        return simplifyLdCtx(inst);
  case LdClsCtx:     return simplifyLdClsCtx(inst);
  case GetCtxFwdCall:return simplifyGetCtxFwdCall(inst);
  case ConvClsToCctx: return simplifyConvClsToCctx(inst);

  case SpillStack:   return simplifySpillStack(inst);
  case CastStk:      return simplifyCastStk(inst);
  case CoerceStk:    return simplifyCoerceStk(inst);
  case AssertStk:    return simplifyAssertStk(inst);
  case LdStack:      return simplifyLdStack(inst);
  case TakeStack:    return simplifyTakeStack(inst);
  case LdStackAddr:  return simplifyLdStackAddr(inst);
  case DecRefStack:  return simplifyDecRefStack(inst);
  case DecRefLoc:    return simplifyDecRefLoc(inst);
  case LdLoc:        return simplifyLdLoc(inst);

  case ExitOnVarEnv: return simplifyExitOnVarEnv(inst);

  case CheckPackedArrayBounds: return simplifyCheckPackedArrayBounds(inst);
  case LdPackedArrayElem:      return simplifyLdPackedArrayElem(inst);

  default:
    return nullptr;
  }
}

SSATmp* Simplifier::simplifySpillStack(IRInstruction* inst) {
  auto const sp           = inst->src(0);
  auto const spDeficit    = inst->src(1)->getValInt();
  auto const numSpillSrcs = inst->srcs().subpiece(2).size();

  // If there's nothing to spill, and no stack adjustment, we don't
  // need the instruction; the old stack is still accurate.
  if (!numSpillSrcs && spDeficit == 0) return sp;

  return nullptr;
}

// We never inline functions that could have a VarEnv, so an
// ExitOnVarEnv that has a frame based on DefInlineFP can be removed.
SSATmp* Simplifier::simplifyExitOnVarEnv(IRInstruction* inst) {
  auto const frameInst = inst->src(0)->inst();
  if (frameInst->op() == DefInlineFP) {
    inst->convertToNop();
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyLdCtx(IRInstruction* inst) {
  auto const func = inst->extra<LdCtx>()->func;
  if (func->isStatic()) {
    // ActRec->m_cls of a static function is always a valid class pointer with
    // the bottom bit set
    return gen(LdCctx, inst->src(0));
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyLdClsCtx(IRInstruction* inst) {
  SSATmp*  ctx = inst->src(0);
  Type ctxType = ctx->type();
  if (ctxType.equals(Type::Obj)) {
    // this pointer... load its class ptr
    return gen(LdObjClass, ctx);
  }
  if (ctxType.equals(Type::Cctx)) {
    return gen(LdClsCctx, ctx);
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyGetCtxFwdCall(IRInstruction* inst) {
  SSATmp*  srcCtx = inst->src(0);
  if (srcCtx->isA(Type::Cctx)) {
    return srcCtx;
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyConvClsToCctx(IRInstruction* inst) {
  auto* srcInst = inst->src(0)->inst();
  if (srcInst->is(LdClsCctx)) return srcInst->src(0);

  return nullptr;
}

SSATmp* Simplifier::simplifyLdCls(IRInstruction* inst) {
  SSATmp* clsName = inst->src(0);
  if (clsName->isConst()) {
    const Class* cls = Unit::lookupClass(clsName->getValStr());
    if (cls) {
      if (RDS::isPersistentHandle(cls->classHandle())) {
        // the class is always defined
        return cns(cls);
      }
      const Class* ctx = inst->src(1)->getValClass();
      if (ctx && ctx->classof(cls)) {
        // the class of the current function being compiled is the
        // same as or derived from cls, so cls must be defined and
        // cannot change the next time we execute this same code
        return cns(cls);
      }
    }
    return gen(LdClsCached, inst->taken(), clsName);
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyCheckType(IRInstruction* inst) {
  SSATmp* src  = inst->src(0);
  auto const oldType = src->type();
  auto const newType = inst->typeParam();

  if (oldType.not(newType)) {
    if (oldType.isBoxed() && newType.isBoxed()) {
      /* This CheckType serves to update the inner type hint for a boxed
       * value, which requires no runtime work.  This depends on the type being
       * boxed, and constraining it with DataTypeCountness will do it.  */
      m_irb.constrainValue(src, DataTypeCountness);
      return gen(AssertType, newType, src);
    }
    /* This check will always fail. It's probably due to an incorrect
     * prediction. Generate a Jmp, and return src because
     * following instructions may depend on the output of CheckType
     * (they'll be DCEd later). Note that we can't use convertToJmp
     * because the return value isn't nullptr, so the original
     * instruction won't be inserted into the stream. */
    gen(Jmp, inst->taken());
    return src;
  }

  if (newType >= oldType) {
    /*
     * The type of the src is the same or more refined than type, so the guard
     * is unnecessary.
     */
    return src;
  }
  if (newType < oldType) {
    assert(!src->isConst());
    return nullptr;
  }

  return nullptr;
}

SSATmp* Simplifier::simplifyAssertType(IRInstruction* inst) {
  auto const src = inst->src(0);

  return simplifyAssertTypeOp(inst, src->type(), [&](TypeConstraint tc) {
    m_irb.constrainValue(src, tc);
  });
}

SSATmp* Simplifier::simplifyCheckStk(IRInstruction* inst) {
  auto const newType = inst->typeParam();
  auto sp = inst->src(0);
  auto offset = inst->extra<CheckStk>()->offset;

  auto stkVal = getStackValue(sp, offset);
  auto const oldType = stkVal.knownType;

  if (newType < oldType) {
    // The new type is strictly better than the old type.
    return nullptr;
  }

  if (newType >= oldType) {
    // The new type isn't better than the old type.
    return sp;
  }

  if (newType.not(oldType)) {
    if (oldType.isBoxed() && newType.isBoxed()) {
      /* This CheckStk serves to update the inner type hint for a boxed
       * value, which requires no runtime work. This depends on the type being
       * boxed, and constraining it with DataTypeCountness will do it.  */
      m_irb.constrainStack(sp, offset, DataTypeCountness);
      return gen(AssertStk, newType, sp);
    }
    /* This check will always fail. It's probably due to an incorrect
     * prediction. Generate a Jmp, and return the source because
     * following instructions may depend on the output of CheckStk
     * (they'll be DCEd later).  Note that we can't use convertToJmp
     * because the return value isn't nullptr, so the original
     * instruction won't be inserted into the stream. */
    gen(Jmp, inst->taken());
    return sp;
  }

  return nullptr;
}

SSATmp* Simplifier::simplifyQueryJmp(IRInstruction* inst) {
  SSATmp* src1 = inst->src(0);
  SSATmp* src2 = inst->src(1);
  Opcode opc = inst->op();
  // reuse the logic in simplifyCmp.
  SSATmp* newCmp = simplifyCmp(queryJmpToQueryOp(opc), nullptr, src1, src2);
  if (!newCmp) return nullptr;

  // Become an equivalent conditional jump and reuse that logic.
  m_irb.unit().replace(
    inst,
    JmpNZero,
    inst->taken(),
    newCmp
  );

  return simplifyCondJmp(inst);
}

SSATmp* Simplifier::simplifyMov(SSATmp* src) {
  return src;
}

SSATmp* Simplifier::simplifyNot(SSATmp* src) {
  if (src->isConst()) {
    return cns(!src->getValBool());
  }

  IRInstruction* inst = src->inst();
  Opcode op = inst->op();

  switch (op) {
  // !!X --> X
  case Not:
    return inst->src(0);

  // !(X cmp Y) --> X opposite_cmp Y
  case Lt:
  case Lte:
  case Gt:
  case Gte:
  case Eq:
  case Neq:
  case Same:
  case NSame:
    // Not for Dbl:  (x < NaN) != !(x >= NaN)
    if (!inst->src(0)->isA(Type::Dbl) &&
        !inst->src(1)->isA(Type::Dbl)) {
      return gen(negateQueryOp(op), inst->src(0), inst->src(1));
    }
    break;

  case InstanceOfBitmask:
  case NInstanceOfBitmask:
    // TODO: combine this with the above check and use isQueryOp or
    // add an isNegatable.
    return gen(
      negateQueryOp(op),
      std::make_pair(inst->numSrcs(), inst->srcs().begin())
    );
    return nullptr;
  // TODO !(X | non_zero) --> 0
  default: (void)op;
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyAbsDbl(IRInstruction* inst) {
  auto src = inst->src(0);

  if (src->isConst()) {
    double val = src->getValDbl();
    return cns(fabs(val));
  }

  return nullptr;
}

template <class Oper>
SSATmp* Simplifier::simplifyConst(SSATmp* src1, SSATmp* src2, Oper op) {
  // don't canonicalize to the right, OP might not be commutative
  if (!src1->isConst() || !src2->isConst()) {
    return nullptr;
  }
  if (src1->type().isNull()) {
    /* Null op Null */
    if (src2->type().isNull()) {
      return cns(op(0,0));
    }
    /* Null op ConstInt */
    if (src2->isA(Type::Int)) {
      return cns(op(0, src2->getValInt()));
    }
    /* Null op ConstBool */
    if (src2->isA(Type::Bool)) {
      return cns(op(0, src2->getValRawInt()));
    }
    /* Null op StaticStr */
    if (src2->isA(Type::StaticStr)) {
      const StringData* str = src2->getValStr();
      if (str->isInteger()) {
        return cns(op(0, str->toInt64()));
      }
      return cns(op(0,0));
    }
  }
  if (src1->isA(Type::Int)) {
    /* ConstInt op Null */
    if (src2->type().isNull()) {
      return cns(op(src1->getValInt(), 0));
    }
    /* ConstInt op ConstInt */
    if (src2->isA(Type::Int)) {
      return cns(op(src1->getValInt(), src2->getValInt()));
    }
    /* ConstInt op ConstBool */
    if (src2->isA(Type::Bool)) {
      return cns(op(src1->getValInt(), src2->getValRawInt()));
    }
    /* ConstInt op StaticStr */
    if (src2->isA(Type::StaticStr)) {
      const StringData* str = src2->getValStr();
      if (str->isInteger()) {
        return cns(op(src1->getValInt(), str->toInt64()));
      }
      return cns(op(src1->getValInt(), 0));
    }
  }
  if (src1->isA(Type::Bool)) {
    /* ConstBool op Null */
    if (src2->type().isNull()) {
      return cns(op(src1->getValRawInt(), 0));
    }
    /* ConstBool op ConstInt */
    if (src2->isA(Type::Int)) {
      return cns(op(src1->getValRawInt(), src2->getValInt()));
    }
    /* ConstBool op ConstBool */
    if (src2->isA(Type::Bool)) {
      return cns(op(src1->getValRawInt(), src2->getValRawInt()));
    }
    /* ConstBool op StaticStr */
    if (src2->isA(Type::StaticStr)) {
      const StringData* str = src2->getValStr();
      if (str->isInteger()) {
        return cns(op(src1->getValBool(), str->toInt64()));
      }
      return cns(op(src1->getValRawInt(), 0));
    }
  }
  if (src1->isA(Type::StaticStr)) {
    const StringData* str = src1->getValStr();
    int64_t strInt = 0;
    if (str->isInteger()) {
      strInt = str->toInt64();
    }
    /* StaticStr op Null */
    if (src2->type().isNull()) {
      return cns(op(strInt, 0));
    }
    /* StaticStr op ConstInt */
    if (src2->isA(Type::Int)) {
      return cns(op(strInt, src2->getValInt()));
    }
    /* StaticStr op ConstBool */
    if (src2->isA(Type::Bool)) {
      return cns(op(strInt, src2->getValBool()));
    }
    /* StaticStr op StaticStr */
    if (src2->isA(Type::StaticStr)) {
      const StringData* str2 = src2->getValStr();
      if (str2->isInteger()) {
        return cns(op(strInt, str2->toInt64()));
      }
      return cns(op(strInt, 0));
    }
  }
  return nullptr;
}

template <class Oper>
SSATmp* Simplifier::simplifyCommutative(SSATmp* src1,
                                        SSATmp* src2,
                                        Opcode opcode,
                                        Oper op) {
  if (auto simp = simplifyConst(src1, src2, op)) {
    return simp;
  }
  // Canonicalize constants to the right.
  if (src1->isConst() && !src2->isConst()) {
    return gen(opcode, src2, src1);
  }
  if (!src1->isA(Type::Int) || !src2->isA(Type::Int)) {
    return nullptr;
  }

  auto inst1 = src1->inst();
  auto inst2 = src2->inst();
  if (inst1->op() == opcode && inst1->src(1)->isConst()) {
    // (X + C1) + C2 --> X + C3
    if (src2->isConst()) {
      int64_t right = inst1->src(1)->getValInt();
      right = op(right, src2->getValInt());
      return gen(opcode, inst1->src(0), cns(right));
    }
    // (X + C1) + (Y + C2) --> X + Y + C3
    if (inst2->op() == opcode && inst2->src(1)->isConst()) {
      int64_t right = inst1->src(1)->getValInt();
      right = op(right, inst2->src(1)->getValInt());
      SSATmp* left = gen(opcode, inst1->src(0), inst2->src(0));
      return gen(opcode, left, cns(right));
    }
  }
  return nullptr;
}

template <class OutOper, class InOper>
SSATmp* Simplifier::simplifyDistributive(SSATmp* src1,
                                         SSATmp* src2,
                                         Opcode outcode,
                                         Opcode incode,
                                         OutOper outop,
                                         InOper inop) {
  // assumes that outop is commutative, don't use with subtract!
  if (auto simp = simplifyCommutative(src1, src2, outcode, outop)) {
    return simp;
  }
  auto inst1 = src1->inst();
  auto inst2 = src2->inst();
  Opcode op1 = inst1->op();
  Opcode op2 = inst2->op();
  // all combinations of X * Y + X * Z --> X * (Y + Z)
  if (op1 == incode && op2 == incode) {
    if (inst1->src(0) == inst2->src(0)) {
      SSATmp* fold = gen(outcode, inst1->src(1), inst2->src(1));
      return gen(incode, inst1->src(0), fold);
    }
    if (inst1->src(0) == inst2->src(1)) {
      SSATmp* fold = gen(outcode, inst1->src(1), inst2->src(0));
      return gen(incode, inst1->src(0), fold);
    }
    if (inst1->src(1) == inst2->src(0)) {
      SSATmp* fold = gen(outcode, inst1->src(0), inst2->src(1));
      return gen(incode, inst1->src(1), fold);
    }
    if (inst1->src(1) == inst2->src(1)) {
      SSATmp* fold = gen(outcode, inst1->src(0), inst2->src(0));
      return gen(incode, inst1->src(1), fold);
    }
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyAddInt(SSATmp* src1, SSATmp* src2) {
  auto add = std::plus<int64_t>();
  auto mul = std::multiplies<int64_t>();
  if (auto simp = simplifyDistributive(src1, src2, AddInt, MulInt, add, mul)) {
    return simp;
  }
  if (src2->isConst()) {
    int64_t src2Val = src2->getValInt();
    // X + 0 --> X
    if (src2Val == 0) {
      return src1;
    }
    // X + -C --> X - C
    if (src2Val < 0) {
      return gen(SubInt, src1, cns(-src2Val));
    }
  }
  // X + (0 - Y) --> X - Y
  auto inst2 = src2->inst();
  if (inst2->op() == SubInt) {
    SSATmp* src = inst2->src(0);
    if (src->isConst() && src->getValInt() == 0) {
      return gen(SubInt, src1, inst2->src(1));
    }
  }
  auto inst1 = src1->inst();

  // (X - C1) + ...
  if (inst1->op() == SubInt && inst1->src(1)->isConst()) {
    auto x = inst1->src(0);
    auto c1 = inst1->src(1);

    // (X - C1) + C2 --> X + (C2 - C1)
    if (src2->isConst()) {
      auto rhs = gen(SubInt, cns(src2->getValInt()), c1);
      return gen(AddInt, x, rhs);
    }

    // (X - C1) + (Y +/- C2)
    if ((inst2->op() == AddInt || inst2->op() == SubInt) &&
        inst2->src(1)->isConst()) {
      auto y = inst2->src(0);
      auto c2 = inst2->src(1);
      SSATmp* rhs = nullptr;
      if (inst2->op() == SubInt) {
        // (X - C1) + (Y - C2) --> X + Y + (-C1 - C2)
        rhs = gen(SubInt, gen(SubInt, cns(0), c1), c2);
      } else {
        // (X - C1) + (Y + C2) --> X + Y + (C2 - C1)
        rhs = gen(SubInt, c2, c1);
      }
      auto lhs = gen(AddInt, x, y);
      return gen(AddInt, lhs, rhs);
    }
    // (X - C1) + (Y + C2) --> X + Y + (C2 - C1)
    if (inst2->op() == AddInt && inst2->src(1)->isConst()) {
      auto y = inst2->src(0);
      auto c2 = inst2->src(1);

      auto lhs = gen(AddInt, x, y);
      auto rhs = gen(SubInt, c2, c1);
      return gen(AddInt, lhs, rhs);
    }
  }

  return nullptr;
}

SSATmp* Simplifier::simplifySubInt(SSATmp* src1, SSATmp* src2) {
  auto sub = std::minus<int64_t>();
  auto c = simplifyConst(src1, src2, sub);
  if (c != nullptr) {
    return c;
  }
  // X - X --> 0
  if (src1 == src2) {
    return cns(0);
  }
  if (src2->isConst()) {
    int64_t src2Val = src2->getValInt();
    // X - 0 --> X
    if (src2Val == 0) {
      return src1;
    }
    // X - -C --> X + C
    if (src2Val < 0 && src2Val > std::numeric_limits<int64_t>::min()) {
      return gen(AddInt, src1, cns(-src2Val));
    }
  }
  // X - (0 - Y) --> X + Y
  auto inst2 = src2->inst();
  if (inst2->op() == SubInt) {
    SSATmp* src = inst2->src(0);
    if (src->isConst() && src->getValInt() == 0) {
      return gen(AddInt, src1, inst2->src(1));
    }
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyMulInt(SSATmp* src1, SSATmp* src2) {
  auto mul = std::multiplies<int64_t>();
  if (auto simp = simplifyCommutative(src1, src2, MulInt, mul)) {
    return simp;
  }

  if (!src2->isConst()) {
    return nullptr;
  }

  int64_t rhs = src2->getValInt();

  // X * (-1) --> -X
  if (rhs == -1) return gen(SubInt, cns(0), src1);
  // X * 0 --> 0
  if (rhs == 0) return cns(0);
  // X * 1 --> X
  if (rhs == 1) return src1;
  // X * 2 --> X + X
  if (rhs == 2) return gen(AddInt, src1, src1);

  auto isPowTwo = [](int64_t a) {
    return a > 0 && folly::isPowTwo<uint64_t>(a);
  };
  auto log2 = [](int64_t a) {
    assert(a > 0);
    return folly::findLastSet<uint64_t>(a) - 1;
  };

  // X * 2^C --> X << C
  if (isPowTwo(rhs)) return gen(Shl, src1, cns(log2(rhs)));

  // X * (2^C + 1) --> ((X << C) + X)
  if (isPowTwo(rhs - 1)) {
    auto lhs = gen(Shl, src1, cns(log2(rhs - 1)));
    return gen(AddInt, lhs, src1);
  }
  // X * (2^C - 1) --> ((X << C) - X)
  if (isPowTwo(rhs + 1)) {
    auto lhs = gen(Shl, src1, cns(log2(rhs + 1)));
    return gen(SubInt, lhs, src1);
  }

  return nullptr;
}

SSATmp* Simplifier::simplifyAddDbl(SSATmp* src1, SSATmp* src2) {
  if (auto simp = simplifyConst(src1, src2, std::plus<double>())) {
    return simp;
  }
  return nullptr;
}

SSATmp* Simplifier::simplifySubDbl(SSATmp* src1, SSATmp* src2) {
  if (auto c = simplifyConst(src1, src2, std::minus<double>())) {
    return c;
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyMulDbl(SSATmp* src1, SSATmp* src2) {
  if (auto simp = simplifyConst(src1, src2, std::multiplies<double>())) {
    return simp;
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyMod(SSATmp* src1, SSATmp* src2) {
  if (!src2->isConst()) {
    return nullptr;
  }
  int64_t src2Val = src2->getValInt();
  // refrain from generating undefined IR
  assert(src2Val != 0);
  // simplify const
  if (src1->isConst()) {
    // still don't want undefined IR
    assert(src1->getValInt() != std::numeric_limits<int64_t>::min() ||
           src2Val != -1);
    return cns(src1->getValInt() % src2Val);
  }
  // X % 1, X % -1 --> 0
  if (src2Val == 1 || src2Val == -1LL) {
    return cns(0);
  }
  // X % LONG_MIN = X (largest magnitude possible as rhs)
  if (src2Val == std::numeric_limits<int64_t>::min()) {
    return src1;
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyDivDbl(IRInstruction* inst) {
  auto src1 = inst->src(0);
  auto src2 = inst->src(1);

  if (!src2->isConst()) return nullptr;

  // not supporting integers (#2570625)
  double src2Val = src2->getValDbl();

  // X / 0 -> bool(false)
  if (src2Val == 0.0) {
    // Ideally we'd generate a RaiseWarning and return false here, but we need
    // a catch trace for that and we can't make a catch trace without
    // HhbcTranslator.
    return nullptr;
  }

  // statically compute X / Y
  if (src1->isConst()) {
    return cns(src1->getValDbl() / src2Val);
  }

  return nullptr;
}

SSATmp* Simplifier::simplifyBitAnd(SSATmp* src1, SSATmp* src2) {
  auto bit_and = [](int64_t a, int64_t b) { return a & b; };
  auto bit_or = [](int64_t a, int64_t b) { return a | b; };
  auto simp = simplifyDistributive(src1, src2, BitAnd, BitOr, bit_and, bit_or);
  if (simp != nullptr) {
    return simp;
  }
  // X & X --> X
  if (src1 == src2) {
    return src1;
  }
  if (src2->isConst()) {
    // X & 0 --> 0
    if (src2->getValInt() == 0) {
      return cns(0);
    }
    // X & (~0) --> X
    if (src2->getValInt() == ~0L) {
      return src1;
    }
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyBitOr(SSATmp* src1, SSATmp* src2) {
  auto bit_and = [](int64_t a, int64_t b) { return a & b; };
  auto bit_or = [](int64_t a, int64_t b) { return a | b; };
  auto simp = simplifyDistributive(src1, src2, BitOr, BitAnd, bit_or, bit_and);
  if (simp != nullptr) {
    return simp;
  }
  // X | X --> X
  if (src1 == src2) {
    return src1;
  }
  if (src2->isConst()) {
    // X | 0 --> X
    if (src2->getValInt() == 0) {
      return src1;
    }
    // X | (~0) --> ~0
    if (src2->getValInt() == ~uint64_t(0)) {
      return cns(~uint64_t(0));
    }
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyBitXor(SSATmp* src1, SSATmp* src2) {
  auto bitxor = [](int64_t a, int64_t b) { return a ^ b; };
  if (auto simp = simplifyCommutative(src1, src2, BitXor, bitxor)) {
    return simp;
  }
  // X ^ X --> 0
  if (src1 == src2)
    return cns(0);
  // X ^ 0 --> X; X ^ -1 --> ~X
  if (src2->isConst()) {
    if (src2->getValInt() == 0) {
      return src1;
    }
    if (src2->getValInt() == -1) {
      return gen(BitNot, src1);
    }
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyLogicXor(SSATmp* src1, SSATmp* src2) {
  // Canonicalize constants to the right.
  if (src1->isConst() && !src2->isConst()) {
    return gen(LogicXor, src2, src1);
  }

  // Both constants.
  if (src1->isConst() && src2->isConst()) {
    return cns(bool(src1->getValBool() ^ src2->getValBool()));
  }

  // One constant: either a Not or constant result.
  if (src2->isConst()) {
    return src2->getValBool() ? gen(Not, src1) : src1;
  }
  return nullptr;
}

template<class Oper>
SSATmp* Simplifier::simplifyShift(SSATmp* src1, SSATmp* src2, Oper op) {
  if (src1->isConst()) {
    if (src1->getValInt() == 0) {
      return cns(0);
    }

    if (src2->isConst()) {
      return cns(op(src1->getValInt(), src2->getValInt()));
    }
  }

  if (src2->isConst() && src2->getValInt() == 0) {
    return src1;
  }

  return nullptr;
}

SSATmp* Simplifier::simplifyShl(IRInstruction* inst) {
  auto src1 = inst->src(0);
  auto src2 = inst->src(1);

  return simplifyShift(src1, src2, [] (int64_t a, int64_t b) {
                       return a << b; });
}

SSATmp* Simplifier::simplifyShr(IRInstruction* inst) {
  auto src1 = inst->src(0);
  auto src2 = inst->src(1);

  return simplifyShift(src1, src2, [] (int64_t a, int64_t b) {
                       return a >> b; });
}

template<class T, class U>
static typename std::common_type<T,U>::type cmpOp(Opcode opName, T a, U b) {
  switch (opName) {
  case GtInt:
  case Gt:   return a > b;
  case GteInt:
  case Gte:  return a >= b;
  case LtInt:
  case Lt:   return a < b;
  case LteInt:
  case Lte:  return a <= b;
  case Same:
  case EqInt:
  case Eq:   return a == b;
  case NSame:
  case NeqInt:
  case Neq:  return a != b;
  default:
    not_reached();
  }
}

SSATmp* Simplifier::simplifyCmp(Opcode opName, IRInstruction* inst,
                                SSATmp* src1, SSATmp* src2) {
  auto newInst = [inst, this](Opcode op, SSATmp* src1, SSATmp* src2) {
    return gen(op, inst ? inst->taken() : (Block*)nullptr, src1, src2);
  };
  // ---------------------------------------------------------------------
  // Perform some execution optimizations immediately
  // ---------------------------------------------------------------------

  auto const type1 = src1->type();
  auto const type2 = src2->type();

  // Identity optimization
  if (src1 == src2 && type1.not(Type::Dbl)) {
    // (val1 == val1) does not simplify to true when val1 is a NaN
    return cns(bool(cmpOp(opName, 0, 0)));
  }

  // need both types to be unboxed to simplify, and the code below assumes the
  // types are known DataTypes.
  if (!type1.isKnownUnboxedDataType() || !type2.isKnownUnboxedDataType()) {
    return nullptr;
  }

  // ---------------------------------------------------------------------
  // OpSame and OpNSame have some special rules
  // ---------------------------------------------------------------------

  if (opName == Same || opName == NSame) {
    // OpSame and OpNSame do not perform type juggling
    if (type1.toDataType() != type2.toDataType() &&
        !(type1.isString() && type2.isString())) {
      return cns(opName == NSame);
    }

    // src1 and src2 are same type, treating Str and StaticStr as the same

    // OpSame and OpNSame have special rules for string, array, object, and
    // resource.  Other types may simplify to OpEq and OpNeq, respectively
    if (type1.isString() && type2.isString()) {
      if (src1->isConst() && src2->isConst()) {
        auto str1 = src1->getValStr();
        auto str2 = src2->getValStr();
        bool same = str1->same(str2);
        return cns(bool(cmpOp(opName, same, 1)));
      } else {
        return nullptr;
      }
    }

    auto const badTypes = Type::Obj | Type::Res | Type::Arr;
    if (type1.maybe(badTypes) || type2.maybe(badTypes)) {
      return nullptr;
    }

    // Type is a primitive type - simplify to Eq/Neq
    return newInst(opName == Same ? Eq : Neq, src1, src2);
  }

  // ---------------------------------------------------------------------
  // We may now perform constant-constant optimizations
  // ---------------------------------------------------------------------

  // Null cmp Null
  if (type1.isNull() && type2.isNull()) {
    return cns(bool(cmpOp(opName, 0, 0)));
  }
  // const cmp const
  // TODO this list is incomplete - feel free to add more
  // TODO: can simplify const arrays when sizes are different or both 0
  if (src1->isConst() && src2->isConst()) {
    // StaticStr cmp StaticStr
    if (src1->isA(Type::StaticStr) &&
        src2->isA(Type::StaticStr)) {
      int cmp = src1->getValStr()->compare(src2->getValStr());
      return cns(bool(cmpOp(opName, cmp, 0)));
    }
    // ConstInt cmp ConstInt
    if (src1->isA(Type::Int) && src2->isA(Type::Int)) {
      return cns(bool(
        cmpOp(opName, src1->getValInt(), src2->getValInt())));
    }
    // ConstBool cmp ConstBool
    if (src1->isA(Type::Bool) && src2->isA(Type::Bool)) {
      return cns(bool(
        cmpOp(opName, src1->getValBool(), src2->getValBool())));
    }
  }

  // ---------------------------------------------------------------------
  // Constant bool comparisons can be strength-reduced
  // NOTE: Comparisons with bools get juggled to bool.
  // ---------------------------------------------------------------------

  // Perform constant-bool optimizations
  if (src2->isA(Type::Bool) && src2->isConst()) {
    bool b = src2->getValBool();

    // The result of the comparison might be independent of the truth
    // value of the LHS. If so, then simplify.
    // E.g. `some-int > true`. some-int may juggle to false or true
    //  (0 or 1), but `0 > true` and `1 > true` are both false, so we can
    //  simplify to false immediately.
    if (cmpOp(opName, false, b) == cmpOp(opName, true, b)) {
      return cns(bool(cmpOp(opName, false, b)));
    }

    // There are only two distinct booleans - false and true (0 and 1).
    // From above, we know that (0 OP b) != (1 OP b).
    // Hence exactly one of (0 OP b) and (1 OP b) is true.
    // Hence there is exactly one boolean value of src1 that results in the
    // overall expression being true (after type-juggling).
    // Hence we may check for equality with that boolean.
    // E.g. `some-int > false` is equivalent to `some-int == true`
    if (opName != Eq) {
      if (cmpOp(opName, false, b)) {
        return newInst(Eq, src1, cns(false));
      } else {
        return newInst(Eq, src1, cns(true));
      }
    }
  }

  // Lower to int-comparison if possible.
  if (!isIntQueryOp(opName) && type1 <= Type::Int && type2 <= Type::Int) {
    return newInst(queryToIntQueryOp(opName), src1, src2);
  }

  // ---------------------------------------------------------------------
  // For same-type cmps, canonicalize any constants to the right
  // Then stop - there are no more simplifications left
  // ---------------------------------------------------------------------

  if (type1.toDataType() == type2.toDataType() ||
      (type1.isString() && type2.isString())) {
    if (src1->isConst() && !src2->isConst()) {
      return newInst(commuteQueryOp(opName), src2, src1);
    }
    return nullptr;
  }

  // ---------------------------------------------------------------------
  // Perform type juggling and type canonicalization for different types
  // see http://www.php.net/manual/en/language.operators.comparison.php
  // ---------------------------------------------------------------------

  // nulls get canonicalized to the right
  if (type1.isNull()) {
    return newInst(commuteQueryOp(opName), src2, src1);
  }

  // case 1a: null cmp string. Convert null to ""
  if (type1.isString() && type2.isNull()) {
    return newInst(opName, src1, cns(makeStaticString("")));
  }

  // case 1b: null cmp object. Convert null to false and the object to true
  if (type1.isObj() && type2.isNull()) {
    return newInst(opName, cns(true), cns(false));
  }

  // case 2a: null cmp anything. Convert null to false
  if (type2.isNull()) {
    return newInst(opName, src1, cns(false));
  }

  // bools get canonicalized to the right
  if (src1->isA(Type::Bool)) {
    return newInst(commuteQueryOp(opName), src2, src1);
  }

  // case 2b: bool cmp anything. Convert anything to bool
  if (src2->isA(Type::Bool)) {
    if (src1->isConst()) {
      if (src1->isA(Type::Int)) {
        return newInst(opName, cns(bool(src1->getValInt())), src2);
      } else if (type1.isString()) {
        auto str = src1->getValStr();
        return newInst(opName, cns(str->toBoolean()), src2);
      }
    }

    // Optimize comparison between int and const bool
    if (src1->isA(Type::Int) && src2->isConst()) {
      // Based on the const bool optimization (above) opName should be OpEq
      always_assert(opName == Eq);

      if (src2->getValBool()) {
        return newInst(Neq, src1, cns(0));
      } else {
        return newInst(Eq, src1, cns(0));
      }
    }

    // Nothing fancy to do - perform juggling as normal.
    return newInst(opName, gen(ConvCellToBool, src1), src2);
  }

  // From here on, we must be careful of how Type::Obj gets dealt with,
  // since Type::Obj can refer to an object or to a resource.

  // case 3: object cmp object. No juggling to do
  // same-type simplification is performed above

  // strings get canonicalized to the left
  if (type2.isString()) {
    return newInst(commuteQueryOp(opName), src2, src1);
  }

  // ints get canonicalized to the right
  if (src1->isA(Type::Int)) {
    return newInst(commuteQueryOp(opName), src2, src1);
  }

  // case 4: number/string/resource cmp. Convert to number (int OR double)
  // NOTE: The following if-test only checks for some of the SRON-SRON
  //  cases (specifically, string-int). Other cases (like string-string)
  //  are dealt with earlier, while other cases (like number-resource)
  //  are not caught at all (and end up exiting this macro at the bottom).
  if (type1.isString() && src1->isConst() && src2->isA(Type::Int)) {
    auto str = src1->getValStr();
    int64_t si; double sd;
    auto st = str->isNumericWithVal(si, sd, true /* allow errors */);
    if (st == KindOfDouble) {
      return newInst(opName, cns(sd), src2);
    }
    if (st == KindOfNull) {
      si = 0;
    }
    return newInst(opName, cns(si), src2);
  }

  // case 5: array cmp array. No juggling to do
  // same-type simplification is performed above

  // case 6: array cmp anything. Array is greater
  if (src1->isArray()) {
    return cns(bool(cmpOp(opName, 1, 0)));
  }
  if (src2->isArray()) {
    return cns(bool(cmpOp(opName, 0, 1)));
  }

  // case 7: object cmp anything. Object is greater
  // ---------------------------------------------------------------------
  // Unfortunately, we are unsure of whether Type::Obj is an object or a
  // resource, so this code cannot be applied.
  // ---------------------------------------------------------------------
  return nullptr;
}

SSATmp* Simplifier::simplifyIsType(IRInstruction* inst) {
  bool trueSense = inst->op() == IsType;
  auto type      = inst->typeParam();
  auto src       = inst->src(0);
  auto srcType   = src->type();

  if (m_irb.typeMightRelax(src)) return nullptr;

  // The comparisons below won't work for these cases covered by this
  // assert, and we currently don't generate these types.
  assert(type.isKnownUnboxedDataType());

  // Testing for StaticStr will make you miss out on CountedStr, and vice versa,
  // and similarly for arrays. PHP treats both types of string the same, so if
  // the distinction matters to you here, be careful.
  assert(IMPLIES(type.isString(), type.equals(Type::Str)));
  assert(IMPLIES(type.isArray(), type.equals(Type::Arr)));

  // The types are disjoint; the result must be false.
  if (srcType.not(type)) {
    return cns(!trueSense);
  }

  // The src type is a subtype of the tested type; the result must be true.
  if (srcType <= type) {
    return cns(trueSense);
  }

  // At this point, either the tested type is a subtype of the src type, or they
  // are non-disjoint but neither is a subtype of the other. We can't simplify
  // this away.
  return nullptr;
}

SSATmp* Simplifier::simplifyIsScalarType(IRInstruction* inst) {
  SSATmp* src = inst->src(0);
  if (src->type().isKnownDataType()) {
    if (src->isA(Type::Int | Type::Dbl | Type::Str | Type::Bool)) {
      return cns(true);
    } else {
      return cns(false);
    }
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyConcatCellCell(IRInstruction* inst) {
  SSATmp* src1 = inst->src(0);
  SSATmp* src2 = inst->src(1);
  auto catchBlock = inst->taken();

  if (src1->isA(Type::Str) && src2->isA(Type::Str)) { // StrStr
    return gen(ConcatStrStr, catchBlock, src1, src2);
  }
  if (src1->isA(Type::Int) && src2->isA(Type::Str)) { // IntStr
    return gen(ConcatIntStr, catchBlock, src1, src2);
  }
  if (src1->isA(Type::Str) && src2->isA(Type::Int)) { // StrInt
    return gen(ConcatStrInt, catchBlock, src1, src2);
  }

  // XXX: t3770157. All the cases below need two different catch blocks but we
  // only have access to one here.
  return nullptr;

  if (src1->isA(Type::Int)) { // IntCell
    auto* asStr = gen(ConvCellToStr, catchBlock, src2);
    auto* result = gen(ConcatIntStr, src1, asStr);
    // ConcatIntStr doesn't consume its second input so we have to decref it
    // here.
    gen(DecRef, asStr);
    return result;
  }
  if (src2->isA(Type::Int)) { // CellInt
    auto const asStr = gen(ConvCellToStr, catchBlock, src1);
    // concat promises to decref its first argument. we need to do it here
    gen(DecRef, src1);
    return gen(ConcatStrInt, asStr, src2);
  }
  if (src1->isA(Type::Str)) { // StrCell
    auto* asStr = gen(ConvCellToStr, catchBlock, src2);
    auto* result = gen(ConcatStrStr, src1, asStr);
    // ConcatStrStr doesn't consume its second input so we have to decref it
    // here.
    gen(DecRef, asStr);
    return result;
  }
  if (src2->isA(Type::Str)) { // CellStr
    auto const asStr = gen(ConvCellToStr, catchBlock, src1);
    // concat promises to decref its first argument. we need to do it here
    gen(DecRef, src1);
    return gen(ConcatStrStr, asStr, src2);
  }

  return nullptr;
}

SSATmp* Simplifier::simplifyConcatStrStr(SSATmp* src1, SSATmp* src2) {
  if (src1->isConst() && src1->isA(Type::StaticStr) &&
      src2->isConst() && src2->isA(Type::StaticStr)) {
    StringData* str1 = const_cast<StringData *>(src1->getValStr());
    StringData* str2 = const_cast<StringData *>(src2->getValStr());
    StringData* merge = makeStaticString(concat_ss(str1, str2));
    return cns(merge);
  }

  return nullptr;
}

SSATmp* Simplifier::simplifyConvToArr(IRInstruction* inst) {
  SSATmp* src  = inst->src(0);
  if (src->isConst()) {
    Array arr = Array::Create(src->getValVariant());
    return cns(ArrayData::GetScalarArray(arr.get()));
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyConvArrToBool(IRInstruction* inst) {
  SSATmp* src  = inst->src(0);
  if (src->isConst()) {
    if (src->getValArr()->empty()) {
      return cns(false);
    }
    return cns(true);
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyConvDblToBool(IRInstruction* inst) {
  SSATmp* src  = inst->src(0);
  if (src->isConst()) {
    return cns(bool(src->getValDbl()));
  }

  return nullptr;
}

SSATmp* Simplifier::simplifyConvIntToBool(IRInstruction* inst) {
  SSATmp* src  = inst->src(0);
  if (src->isConst()) {
    return cns(bool(src->getValInt()));
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyConvStrToBool(IRInstruction* inst) {
  SSATmp* src  = inst->src(0);
  if (src->isConst()) {
    // only the strings "", and "0" convert to false, all other strings
    // are converted to true
    const StringData* str = src->getValStr();
    return cns(!str->empty() && !str->isZero());
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyConvArrToDbl(IRInstruction* inst) {
  SSATmp* src = inst->src(0);
  if (src->isConst()) {
    if (src->getValArr()->empty()) {
      return cns(0.0);
    }
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyConvBoolToDbl(IRInstruction* inst) {
  SSATmp* src = inst->src(0);
  if (src->isConst()) {
    return cns(double(src->getValBool()));
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyConvIntToDbl(IRInstruction* inst) {
  SSATmp* src = inst->src(0);
  if (src->isConst()) {
    return cns(double(src->getValInt()));
  }
  if (src->inst()->is(ConvBoolToInt)) {
    return gen(ConvBoolToDbl, src->inst()->src(0));
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyConvStrToDbl(IRInstruction* inst) {
  SSATmp* src = inst->src(0);
  if (src->isConst()) {
    const StringData *str = src->getValStr();
    int64_t lval;
    double dval;
    DataType ret = str->isNumericWithVal(lval, dval, 1);
    if (ret == KindOfInt64) {
      dval = (double)lval;
    } else if (ret != KindOfDouble) {
      dval = 0.0;
    }
    return cns(dval);
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyConvArrToInt(IRInstruction* inst) {
  SSATmp* src  = inst->src(0);
  if (src->isConst()) {
    if (src->getValArr()->empty()) {
      return cns(0);
    }
    return cns(1);
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyConvBoolToInt(IRInstruction* inst) {
  SSATmp* src  = inst->src(0);
  if (src->isConst()) {
    return cns(int(src->getValBool()));
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyConvDblToInt(IRInstruction* inst) {
  SSATmp* src  = inst->src(0);
  if (src->isConst()) {
    return cns(toInt64(src->getValDbl()));
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyConvStrToInt(IRInstruction* inst) {
  SSATmp* src  = inst->src(0);
  if (src->isConst()) {
    const StringData *str = src->getValStr();
    int64_t lval;
    double dval;
    DataType ret = str->isNumericWithVal(lval, dval, 1);
    if (ret == KindOfDouble) {
      lval = (int64_t)dval;
    } else if (ret != KindOfInt64) {
      lval = 0;
    }
    return cns(lval);
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyConvBoolToStr(IRInstruction* inst) {
  SSATmp* src  = inst->src(0);
  if (src->isConst()) {
    if (src->getValBool()) {
      return cns(makeStaticString("1"));
    }
    return cns(makeStaticString(""));
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyConvDblToStr(IRInstruction* inst) {
  SSATmp* src  = inst->src(0);
  if (src->isConst()) {
    String dblStr(buildStringData(src->getValDbl()));
    return cns(makeStaticString(dblStr));
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyConvIntToStr(IRInstruction* inst) {
  SSATmp* src  = inst->src(0);
  if (src->isConst()) {
    return cns(
      makeStaticString(folly::to<std::string>(src->getValInt()))
    );
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyConvCellToBool(IRInstruction* inst) {
  auto const src     = inst->src(0);
  auto const srcType = src->type();

  if (srcType.isBool())   return src;
  if (srcType.isNull())   return cns(false);
  if (srcType.isArray())  return gen(ConvArrToBool, src);
  if (srcType.isDbl())    return gen(ConvDblToBool, src);
  if (srcType.isInt())    return gen(ConvIntToBool, src);
  if (srcType.isString()) return gen(ConvStrToBool, src);
  if (srcType.isObj()) {
    if (auto cls = srcType.getClass()) {
      // t3429711 we should test cls->m_ODAttr
      // here, but currently it doesnt have all
      // the flags set.
      if (!cls->instanceCtor()) {
        return cns(true);
      }
    }
    return gen(ConvObjToBool, src);
  }
  if (srcType.isRes())    return nullptr; // No specialization yet

  return nullptr;
}

SSATmp* Simplifier::simplifyConvCellToStr(IRInstruction* inst) {
  auto const src        = inst->src(0);
  auto const srcType    = src->type();
  auto const catchTrace = inst->taken();

  if (srcType.isBool())   return gen(ConvBoolToStr, src);
  if (srcType.isNull())   return cns(makeStaticString(""));
  if (srcType.isArray())  {
    gen(RaiseNotice, catchTrace,
        cns(makeStaticString("Array to string conversion")));
    return cns(makeStaticString("Array"));
  }
  if (srcType.isDbl())    return gen(ConvDblToStr, src);
  if (srcType.isInt())    return gen(ConvIntToStr, src);
  if (srcType.isString()) {
    gen(IncRef, src);
    return src;
  }
  if (srcType.isObj())    return gen(ConvObjToStr, catchTrace, src);
  if (srcType.isRes())    return gen(ConvResToStr, catchTrace, src);

  return nullptr;
}

SSATmp* Simplifier::simplifyConvCellToInt(IRInstruction* inst) {
  auto const src      = inst->src(0);
  auto const srcType  = src->type();

  if (srcType.isInt())    return src;
  if (srcType.isNull())   return cns(0);
  if (srcType.isArray())  return gen(ConvArrToInt, src);
  if (srcType.isBool())   return gen(ConvBoolToInt, src);
  if (srcType.isDbl())    return gen(ConvDblToInt, src);
  if (srcType.isString()) return gen(ConvStrToInt, src);
  if (srcType.isObj())    return gen(ConvObjToInt, inst->taken(), src);
  if (srcType.isRes())    return nullptr; // No specialization yet

  return nullptr;
}

SSATmp* Simplifier::simplifyConvCellToDbl(IRInstruction* inst) {
  auto const src      = inst->src(0);
  auto const srcType  = src->type();

  if (srcType.isDbl())    return src;
  if (srcType.isNull())   return cns(0.0);
  if (srcType.isArray())  return gen(ConvArrToDbl, src);
  if (srcType.isBool())   return gen(ConvBoolToDbl, src);
  if (srcType.isInt())    return gen(ConvIntToDbl, src);
  if (srcType.isString()) return gen(ConvStrToDbl, src);
  if (srcType.isObj())    return gen(ConvObjToDbl, inst->taken(), src);
  if (srcType.isRes())    return nullptr; // No specialization yet

  return nullptr;
}

template<class Oper>
SSATmp* Simplifier::simplifyRoundCommon(IRInstruction* inst, Oper op) {
  auto const src  = inst->src(0);

  if (src->isConst()) {
    return cns(op(src->getValDbl()));
  }

  auto srcInst = src->inst();
  if (srcInst->op() == ConvIntToDbl || srcInst->op() == ConvBoolToDbl) {
    return src;
  }

  return nullptr;
}

SSATmp* Simplifier::simplifyFloor(IRInstruction* inst) {
  return simplifyRoundCommon(inst, floor);
}

SSATmp* Simplifier::simplifyCeil(IRInstruction* inst) {
  return simplifyRoundCommon(inst, ceil);
}

SSATmp* Simplifier::simplifyUnbox(IRInstruction* inst) {
  auto* src = inst->src(0);
  auto type = outputType(inst);

  Type srcType = src->type();
  if (srcType.notBoxed()) {
    assert(type.equals(srcType));
    return src;
  }
  if (srcType.isBoxed()) {
    srcType = srcType.innerType();
    assert(type.equals(srcType));
    return gen(LdRef, type, inst->taken(), src);
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyUnboxPtr(IRInstruction* inst) {
  if (inst->src(0)->isA(Type::PtrToCell)) {
    return inst->src(0);
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyBoxPtr(IRInstruction* inst) {
  if (inst->src(0)->isA(Type::PtrToBoxedCell)) {
    return inst->src(0);
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyCheckInit(IRInstruction* inst) {
  auto const srcType = inst->src(0)->type();
  assert(srcType.notPtr());
  assert(inst->taken());
  if (srcType.isInit()) inst->convertToNop();
  return nullptr;
}

SSATmp* Simplifier::simplifyPrint(IRInstruction* inst) {
  if (inst->src(0)->type().isNull()) {
    inst->convertToNop();
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyDecRef(IRInstruction* inst) {
  auto src = inst->src(0);
  if (!m_irb.typeMightRelax(src) && !isRefCounted(src)) {
    inst->convertToNop();
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyIncRef(IRInstruction* inst) {
  SSATmp* src = inst->src(0);
  if (!m_irb.typeMightRelax(src) && !isRefCounted(src)) {
    inst->convertToNop();
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyIncRefCtx(IRInstruction* inst) {
  auto* ctx = inst->src(0);
  if (ctx->isA(Type::Obj)) {
    inst->setOpcode(IncRef);
  } else if (!m_irb.typeMightRelax(ctx) && ctx->type().notCounted()) {
    inst->convertToNop();
  }

  return nullptr;
}

SSATmp* Simplifier::simplifyCondJmp(IRInstruction* inst) {
  SSATmp* const src            = inst->src(0);
  IRInstruction* const srcInst = src->inst();
  const Opcode srcOpcode       = srcInst->op();

  // After other simplifications below (isConvIntOrPtrToBool), we can
  // end up with a non-Bool input.  Nothing more to do in this case.
  if (src->type() != Type::Bool) {
    return nullptr;
  }

  // Constant propagate.
  if (src->isConst()) {
    bool val = src->getValBool();
    if (inst->op() == JmpZero) {
      val = !val;
    }
    if (val) {
      inst->convertToJmp();
    } else {
      inst->convertToNop();
    }
    return nullptr;
  }

  // Pull negations into the jump.
  if (src->inst()->op() == Not) {
    inst->setOpcode(inst->op() == JmpZero ? JmpNZero : JmpZero);
    inst->setSrc(0, srcInst->src(0));
    return nullptr;
  }

  /*
   * Try to combine the src inst with the Jmp.  We can't do any
   * combinations of the src instruction with the jump if the src's
   * are refcounted, since we may have dec refs between the src
   * instruction and the jump.
   */
  for (auto& src : srcInst->srcs()) {
    if (isRefCounted(src)) return nullptr;
  }

  // If the source is conversion of an int or pointer to boolean, we
  // can test the int/ptr value directly.
  auto isConvIntOrPtrToBool = [&](IRInstruction* instr) {
    switch (instr->op()) {
    case ConvIntToBool:
      return true;
    case ConvCellToBool:
      return instr->src(0)->type().subtypeOfAny(
        Type::Func, Type::Cls, Type::VarEnv, Type::TCA);
    default:
      return false;
    }
  };
  if (isConvIntOrPtrToBool(srcInst)) {
    // We can just check the int or ptr directly. Borrow the Conv's src.
    inst->setSrc(0, srcInst->src(0));
    return nullptr;
  }

  auto canCompareFused = [&]() {
    auto src1Type = srcInst->src(0)->type();
    auto src2Type = srcInst->src(1)->type();
    return ((src1Type == Type::Int && src2Type == Type::Int) ||
            ((src1Type == Type::Int || src1Type == Type::Dbl) &&
             (src2Type == Type::Int || src2Type == Type::Dbl)) ||
            (src1Type == Type::Bool && src2Type == Type::Bool) ||
            (src1Type == Type::Cls && src2Type == Type::Cls));
  };

  // Fuse jumps with query operators.
  if (isFusableQueryOp(srcOpcode) && canCompareFused()) {
    auto opc = queryToJmpOp(inst->op() == JmpZero
                            ? negateQueryOp(srcOpcode) : srcOpcode);
    SrcRange ssas = srcInst->srcs();

    m_irb.unit().replace(
      inst,
      opc,
      inst->maybeTypeParam(),
      std::make_pair(ssas.size(), ssas.begin())
    );
  }

  return nullptr;
}

SSATmp* Simplifier::simplifyCastStk(IRInstruction* inst) {
  auto const info = getStackValue(inst->src(0),
                                  inst->extra<CastStk>()->offset);
  if (info.knownType <= inst->typeParam()) {
    // No need to cast---the type was as good or better.
    inst->convertToNop();
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyCoerceStk(IRInstruction* inst) {
  auto const info = getStackValue(inst->src(0),
                                  inst->extra<CoerceStk>()->offset);
  if (info.knownType <= inst->typeParam()) {
    // No need to cast---the type was as good or better.
    inst->convertToNop();
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyAssertStk(IRInstruction* inst) {
  auto const idx = inst->extra<AssertStk>()->offset;
  auto const info = getStackValue(inst->src(0), idx);

  return simplifyAssertTypeOp(inst, info.knownType,
    [&](TypeConstraint tc) {
      m_irb.constrainStack(inst->src(0), idx, tc);
    }
  );
}

SSATmp* Simplifier::simplifyLdStack(IRInstruction* inst) {
  auto const info = getStackValue(inst->src(0),
                                  inst->extra<LdStack>()->offset);

  // We don't want to extend live ranges of tmps across calls, so we
  // don't get the value if spansCall is true; however, we can use
  // any type information known.
  auto* value = info.value;
  if (value && (!info.spansCall || info.value->inst()->is(DefConst))) {
    // The refcount optimizations depend on reliably tracking refcount
    // producers and consumers. LdStack and other raw loads are special cased
    // during the analysis, so if we're going to replace this LdStack with a
    // value that isn't from another raw load, we need to leave something in
    // its place to preserve that information.
    if (!value->inst()->isRawLoad() &&
        (value->type().maybeCounted() || m_irb.typeMightRelax(info.value))) {
      gen(TakeStack, info.value);
    }
    return info.value;
  }
  inst->setTypeParam(
    Type::mostRefined(inst->typeParam(), info.knownType)
  );
  return nullptr;
}

SSATmp* Simplifier::simplifyTakeStack(IRInstruction* inst) {
  if (inst->src(0)->type().notCounted() &&
      !m_irb.typeMightRelax(inst->src(0))) {
    inst->convertToNop();
  }

  return nullptr;
}

SSATmp* Simplifier::simplifyDecRefLoc(IRInstruction* inst) {
  auto const localValue = m_irb.localValue(inst->extra<DecRefLoc>()->locId,
                                          DataTypeGeneric);
  if (!m_irb.typeMightRelax(localValue) && inst->typeParam().notCounted()) {
    inst->convertToNop();
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyLdLoc(IRInstruction* inst) {
  // Ideally we'd replace LdLoc<Null,...> with a constant value of that type,
  // but that prevents the guard relaxation code from tracing the source of
  // values.
  return nullptr;
}

SSATmp* Simplifier::simplifyLdStackAddr(IRInstruction* inst) {
  auto const info = getStackValue(inst->src(0),
                                  inst->extra<StackOffset>()->offset);
  inst->setTypeParam(
    Type::mostRefined(inst->typeParam(), info.knownType.ptr())
  );
  return nullptr;
}

SSATmp* Simplifier::simplifyDecRefStack(IRInstruction* inst) {
  auto const info = getStackValue(inst->src(0),
                                  inst->extra<StackOffset>()->offset);
  if (info.value && !info.spansCall) {
    if (info.value->type().maybeCounted() || m_irb.typeMightRelax(info.value)) {
      gen(TakeStack, info.value);
    }
    inst->convertToNop();
    return gen(DecRef, info.value);
  }
  if (m_irb.typeMightRelax(info.value)) {
    return nullptr;
  }

  inst->setTypeParam(
    Type::mostRefined(inst->typeParam(), info.knownType)
  );
  if (inst->typeParam().notCounted()) {
    inst->convertToNop();
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyAssertNonNull(IRInstruction* inst) {
  if (inst->src(0)->type().not(Type::Nullptr)) {
    return inst->src(0);
  }
  return nullptr;
}

SSATmp* Simplifier::simplifyCheckPackedArrayBounds(IRInstruction* inst) {
  auto* array = inst->src(0);
  auto* idx = inst->src(1);

  if (idx->isConst()) {
    auto const idxVal = (uint64_t)idx->getValInt();
    if (idxVal >= 0xffffffffull) {
      // ArrayData can't hold more than 2^32 - 1 elements, so this is always
      // going to fail.
      inst->convertToJmp();
    } else if (array->isConst()) {
      if (idxVal >= array->getValArr()->size()) {
        inst->convertToJmp();
      } else {
        // We should convert inst to a nop here but that exposes t3626113
      }
    }
  }

  return nullptr;
}

SSATmp* Simplifier::simplifyLdPackedArrayElem(IRInstruction* inst) {
  auto* arrayTmp = inst->src(0);
  auto* idxTmp   = inst->src(1);
  if (arrayTmp->isConst() && idxTmp->isConst()) {
    auto* value = arrayTmp->getValArr()->nvGet(idxTmp->getValInt());
    if (!value) {
      // The index doesn't exist. This code should be unreachable at runtime.
      return nullptr;
    }

    if (value->m_type == KindOfRef) value = value->m_data.pref->tv();
    return cns(*value);
  }

  return nullptr;
}

SSATmp* Simplifier::simplifyAssertTypeOp(IRInstruction* inst, Type oldType,
                                         ConstraintFunc constrain) const {
  auto const newType = inst->typeParam();

  if (oldType.not(newType)) {
    // If both types are boxed this is ok and even expected as a means to
    // update the hint for the inner type.
    if (oldType.isBoxed() && newType.isBoxed()) return nullptr;

    // We got external information (probably from static analysis) that
    // conflicts with what we've built up so far. There's no reasonable way to
    // continue here: we can't properly fatal the request because we can't make
    // a catch trace or SpillStack without HhbcTranslator, we can't punt on
    // just this instruction because we might not be in the initial translation
    // phase, and we can't just plow on forward since we'll probably generate
    // malformed IR. Since this case is very rare, just punt on the whole trace
    // so it gets interpreted.
    TRACE_PUNT("Invalid AssertTypeOp");
  }

  // Asserting in these situations doesn't add any information.
  if (oldType == Type::Cls || newType == Type::Gen) return inst->src(0);

  // We're asserting a strict subtype of the old type, so keep the assert
  // around.
  if (newType < oldType) return nullptr;

  // oldType is at least as good as the new type. Kill this assert op but
  // preserve the type we were asserting in case the source type gets relaxed
  // past it.
  if (newType >= oldType) {
    constrain({DataTypeGeneric, newType});
    return inst->src(0);
  }

  // Now we're left with cases where neither type is a subtype of the other but
  // they have some nonzero intersection. We want to end up asserting the
  // intersection, but we have to constrain the input to avoid reintroducing
  // types that were removed from the original typeParam.
  auto const intersect = newType & oldType;
  inst->setTypeParam(intersect);

  TypeConstraint tc;
  if (intersect != newType) {
    auto increment = [](DataTypeCategory& cat) {
      always_assert(cat != DataTypeSpecialized);
      cat = static_cast<DataTypeCategory>(static_cast<uint8_t>(cat) + 1);
    };

    Type relaxed;
    // Find the most general constraint that doesn't modify the type being
    // asserted.
    while ((relaxed = newType & relaxType(oldType, tc)) != intersect) {
      if (tc.category > DataTypeGeneric &&
          relaxed.maybeBoxed() && intersect.maybeBoxed() &&
          (relaxed & Type::Cell) == (intersect & Type::Cell)) {
        // If the inner type is why we failed, constrain that a level.
        increment(tc.innerCat);
      } else {
        increment(tc.category);
      }
    }
  }
  constrain(tc);

  return nullptr;
}

//////////////////////////////////////////////////////////////////////

}}
