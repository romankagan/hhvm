/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
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

#ifndef incl_HPHP_VM_FUNC_EMITTER_INL_H_
#error "func-emitter-inl.h should only be included by func-emitter.h"
#endif

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

template<class SerDe>
void EHEntEmitter::serde(SerDe& sd) {
  sd(m_type)
    (m_base)
    (m_past)
    (m_iterId)
    (m_handler)
    (m_end)
    (m_itRef)
    (m_parentIndex)
    ;
}

///////////////////////////////////////////////////////////////////////////////

inline UnitEmitter& FuncEmitter::ue() const {
  return m_ue;
}

inline PreClassEmitter* FuncEmitter::pce() const {
  return m_pce;
}

inline int FuncEmitter::sn() const {
  return m_sn;
}

inline Id FuncEmitter::id() const {
  assert(m_pce == nullptr);
  return m_id;
}

inline void FuncEmitter::setIds(int sn, Id id) {
  m_sn = sn;
  m_id = id;
}

///////////////////////////////////////////////////////////////////////////////
// Locals, iterators, and parameters.

inline Id FuncEmitter::numLocals() const {
  return m_numLocals;
}

inline Id FuncEmitter::numIterators() const {
  return m_numIterators;
}

inline Id FuncEmitter::numClsRefSlots() const {
  return m_numClsRefSlots;
}

inline Id FuncEmitter::numLiveIterators() const {
  return m_nextFreeIterator;
}

inline void FuncEmitter::setNumIterators(Id numIterators) {
  assert(m_numIterators == 0);
  m_numIterators = numIterators;
}

inline void FuncEmitter::setNumLiveIterators(Id id) {
  m_nextFreeIterator = id;
}

inline void FuncEmitter::setNumClsRefSlots(Id num) {
  assert(m_numClsRefSlots == 0);
  m_numClsRefSlots = num;
}

inline bool FuncEmitter::hasVar(const StringData* name) const {
  assert(name != nullptr);
  return m_localNames.find(name) != m_localNames.end();
}

inline Id FuncEmitter::lookupVarId(const StringData* name) const {
  assert(hasVar(name));
  return m_localNames.find(name)->second;
}

inline void FuncEmitter::freeUnnamedLocal(Id /*id*/) {
  assert(m_activeUnnamedLocals > 0);
  --m_activeUnnamedLocals;
}

inline void FuncEmitter::freeIterator(Id id) {
  --m_nextFreeIterator;
  assert(id == m_nextFreeIterator);
}

inline void FuncEmitter::appendParam(const StringData* name,
                                     const ParamInfo& info) {
  allocVarId(name);
  params.push_back(info);
}

inline const Func::NamedLocalsMap::Builder& FuncEmitter::localNameMap() const {
  return m_localNames;
}

///////////////////////////////////////////////////////////////////////////////
// Helper accessors.

inline bool FuncEmitter::isPseudoMain() const {
  return name->empty();
}

inline bool FuncEmitter::isMethod() const {
  return !isPseudoMain() && pce();
}

inline bool FuncEmitter::isVariadic() const {
  return params.size() && params[(params.size() - 1)].isVariadic();
}

inline bool FuncEmitter::isVariadicByRef() const {
  return isVariadic() && params[(params.size() -1)].byRef;
}

inline std::pair<int,int> FuncEmitter::getLocation() const {
  return std::make_pair(line1, line2);
}

///////////////////////////////////////////////////////////////////////////////
// Setters.

inline void FuncEmitter::setLocation(int l1, int l2) {
  line1 = l1;
  line2 = l2;
}

///////////////////////////////////////////////////////////////////////////////
// Serialization/Deserialization

template<class SerDe>
void FuncEmitter::serdeMetaData(SerDe& sd) {
  // NOTE: name, top, and a few other fields currently handled outside of this.
  Offset past_delta;
  if (!SerDe::deserializing) {
    past_delta = past - base;
  }

  sd(line1)
    (line2)
    (base)
    (past_delta)
    (attrs)
    (hniReturnType)
    (repoReturnType)
    (repoAwaitedReturnType)
    (docComment)
    (m_numLocals)
    (m_numIterators)
    (m_numClsRefSlots)
    (maxStackCells)
    (m_repoBoolBitset)

    (params)
    (m_localNames)
    (staticVars)
    (ehtab)
    (fpitab)
    (userAttributes)
    (retTypeConstraint)
    (retUserType)
    (originalFilename)
    ;

  if (isNative) {
    sd(dynCallWrapperId)
      ;
  }

  if (SerDe::deserializing) {
    past = base + past_delta;
  }
}

///////////////////////////////////////////////////////////////////////////////

}
