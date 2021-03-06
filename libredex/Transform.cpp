/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Transform.h"

#include <algorithm>
#include <memory>
#include <unordered_set>
#include <list>

#include "ControlFlow.h"
#include "Debug.h"
#include "DexClass.h"
#include "DexDebugInstruction.h"
#include "IRInstruction.h"
#include "DexUtil.h"
#include "Util.h"

std::vector<Block*> postorder_sort(const std::vector<Block*>& cfg) {
  std::vector<Block*> postorder;
  std::vector<Block*> stack;
  std::unordered_set<Block*> visited;
  for (size_t i = 1; i < cfg.size(); i++) {
    if (cfg[i]->preds().size() == 0) {
      stack.push_back(cfg[i]);
    }
  }
  stack.push_back(cfg[0]);
  while (!stack.empty()) {
    auto const& curr = stack.back();
    visited.insert(curr);
    bool all_succs_visited = [&] {
      for (auto const& s : curr->succs()) {
        if (!visited.count(s)) {
          stack.push_back(s);
          return false;
        }
      }
      return true;
    }();
    if (all_succs_visited) {
      assert(curr == stack.back());
      postorder.push_back(curr);
      stack.pop_back();
    }
  }
  return postorder;
}

////////////////////////////////////////////////////////////////////////////////

MethodItemEntry::MethodItemEntry(const MethodItemEntry& that)
    : type(that.type), addr(that.addr) {
  switch (type) {
  case MFLOW_TRY:
    tentry = that.tentry;
    break;
  case MFLOW_CATCH:
    centry = that.centry;
    break;
  case MFLOW_OPCODE:
    insn = that.insn;
    break;
  case MFLOW_TARGET:
    target = that.target;
    break;
  case MFLOW_DEBUG:
    new (&dbgop) std::unique_ptr<DexDebugInstruction>(that.dbgop->clone());
    break;
  case MFLOW_POSITION:
    new (&pos) std::unique_ptr<DexPosition>(new DexPosition(*that.pos));
    break;
  case MFLOW_FALLTHROUGH:
    break;
  default:
    not_reached();
  }
}

MethodItemEntry::~MethodItemEntry() {
  switch (type) {
    case MFLOW_TRY:
      delete tentry;
      break;
    case MFLOW_CATCH:
      delete centry;
      break;
    case MFLOW_TARGET:
      delete target;
      break;
    case MFLOW_DEBUG:
      dbgop.~unique_ptr<DexDebugInstruction>();
      break;
    case MFLOW_POSITION:
      pos.~unique_ptr<DexPosition>();
      break;
    default:
      /* nothing to delete */
      break;
  }
}

void MethodItemEntry::gather_strings(std::vector<DexString*>& lstring) const {
  switch (type) {
  case MFLOW_TRY:
    break;
  case MFLOW_CATCH:
    break;
  case MFLOW_OPCODE:
    insn->gather_strings(lstring);
    break;
  case MFLOW_TARGET:
    break;
  case MFLOW_DEBUG:
    dbgop->gather_strings(lstring);
    break;
  case MFLOW_POSITION:
    // although DexPosition contains strings, these strings don't find their
    // way into the APK
    break;
  case MFLOW_FALLTHROUGH:
    break;
  default:
    not_reached();
  }
}

void MethodItemEntry::gather_methods(std::vector<DexMethod*>& lmethod) const {
  switch (type) {
  case MFLOW_TRY:
    break;
  case MFLOW_CATCH:
    break;
  case MFLOW_OPCODE:
    insn->gather_methods(lmethod);
    break;
  case MFLOW_TARGET:
    break;
  case MFLOW_DEBUG:
    dbgop->gather_methods(lmethod);
    break;
  case MFLOW_POSITION:
    break;
  case MFLOW_FALLTHROUGH:
    break;
  default:
    not_reached();
  }
}

void MethodItemEntry::gather_fields(std::vector<DexField*>& lfield) const {
  switch (type) {
  case MFLOW_TRY:
    break;
  case MFLOW_CATCH:
    break;
  case MFLOW_OPCODE:
    insn->gather_fields(lfield);
    break;
  case MFLOW_TARGET:
    break;
  case MFLOW_DEBUG:
    dbgop->gather_fields(lfield);
    break;
  case MFLOW_POSITION:
    break;
  case MFLOW_FALLTHROUGH:
    break;
  default:
    not_reached();
  }
}

void MethodItemEntry::gather_types(std::vector<DexType*>& ltype) const {
  switch (type) {
  case MFLOW_TRY:
    break;
  case MFLOW_CATCH:
    if (centry->catch_type != nullptr) {
      ltype.push_back(centry->catch_type);
    }
    break;
  case MFLOW_OPCODE:
    insn->gather_types(ltype);
    break;
  case MFLOW_TARGET:
    break;
  case MFLOW_DEBUG:
    dbgop->gather_types(ltype);
    break;
  case MFLOW_POSITION:
    break;
  case MFLOW_FALLTHROUGH:
    break;
  default:
    not_reached();
  }
}

////////////////////////////////////////////////////////////////////////////////

InlineContext::InlineContext(DexMethod* caller, bool use_liveness)
    : original_regs(caller->get_code()->get_registers_size()),
      caller_code(&*caller->get_code()) {
  auto mtcaller = caller_code;
  estimated_insn_size = mtcaller->sum_opcode_sizes();
  if (use_liveness) {
    mtcaller->build_cfg(false);
    m_liveness = Liveness::analyze(mtcaller->cfg(), original_regs);
  }
}

Liveness InlineContext::live_out(IRInstruction* insn) {
  if (m_liveness) {
    return m_liveness->at(insn);
  } else {
    // w/o liveness analysis we just assume that all caller regs are live
    auto rs = RegSet(original_regs);
    rs.flip();
    return Liveness(std::move(rs));
  }
}

IRCode::IRCode()
    : m_fmethod(new FatMethod()) {}

IRCode::IRCode(DexMethod* method) {
  m_fmethod = balloon(const_cast<DexMethod*>(method));
  auto dc = &*method->get_dex_code();
  m_registers_size = dc->get_registers_size();
  m_ins_size = dc->get_ins_size();
  m_outs_size = dc->get_outs_size();
  m_dbg = dc->release_debug_item();
}

IRCode::~IRCode() {
  m_fmethod->clear_and_dispose(FatMethodDisposer());
  delete m_fmethod;
}

void IRCode::gather_catch_types(std::vector<DexType*>& ltype) const {
  for (auto& mie : *m_fmethod) {
    if (mie.type != MFLOW_CATCH) {
      continue;
    }
    if (mie.centry->catch_type != nullptr) {
      ltype.push_back(mie.centry->catch_type);
    }
  }
}

void IRCode::gather_types(std::vector<DexType*>& ltype) const {
  for (auto& mie : *m_fmethod) {
    mie.gather_types(ltype);
  }
  if (m_dbg) m_dbg->gather_types(ltype);
}

void IRCode::gather_strings(std::vector<DexString*>& lstring) const {
  for (auto& mie : *m_fmethod) {
    mie.gather_strings(lstring);
  }
  if (m_dbg) m_dbg->gather_strings(lstring);
}

void IRCode::gather_fields(std::vector<DexField*>& lfield) const {
  for (auto& mie : *m_fmethod) {
    mie.gather_fields(lfield);
  }
}

void IRCode::gather_methods(std::vector<DexMethod*>& lmethod) const {
  for (auto& mie : *m_fmethod) {
    mie.gather_methods(lmethod);
  }
}

namespace {

int bytecount(int32_t v) {
  int bytecount = 4;
  if ((int32_t)((int8_t)(v & 0xff)) == v) {
    bytecount = 1;
  } else if ((int32_t)((int16_t)(v & 0xffff)) == v) {
    bytecount = 2;
  }
  return bytecount;
}

DexOpcode goto_for_offset(int32_t offset) {
  if (offset == 0) {
    return OPCODE_GOTO_32;
  }
  switch (bytecount(offset)) {
  case 1:
    return OPCODE_GOTO;
  case 2:
    return OPCODE_GOTO_16;
  case 4:
    return OPCODE_GOTO_32;
  default:
    always_assert_log(false, "Invalid bytecount %d", offset);
  }
}

DexOpcode invert_conditional_branch(DexOpcode op) {
  switch (op) {
  case OPCODE_IF_EQ:
    return OPCODE_IF_NE;
  case OPCODE_IF_NE:
    return OPCODE_IF_EQ;
  case OPCODE_IF_LT:
    return OPCODE_IF_GE;
  case OPCODE_IF_GE:
    return OPCODE_IF_LT;
  case OPCODE_IF_GT:
    return OPCODE_IF_LE;
  case OPCODE_IF_LE:
    return OPCODE_IF_GT;
  case OPCODE_IF_EQZ:
    return OPCODE_IF_NEZ;
  case OPCODE_IF_NEZ:
    return OPCODE_IF_EQZ;
  case OPCODE_IF_LTZ:
    return OPCODE_IF_GEZ;
  case OPCODE_IF_GEZ:
    return OPCODE_IF_LTZ;
  case OPCODE_IF_GTZ:
    return OPCODE_IF_LEZ;
  case OPCODE_IF_LEZ:
    return OPCODE_IF_GTZ;
  default:
    always_assert_log(false, "Invalid conditional opcode %s", SHOW(op));
  }
}

template <typename K, typename V>
static V get_target(MethodItemEntry* mei,
                    const std::unordered_map<K, V>& addr_map) {
  uint32_t base = mei->addr;
  int offset = mei->insn->offset();
  uint32_t target = base + offset;
  always_assert_log(
      addr_map.count(target) != 0,
      "Invalid opcode target %08x[%p](%08x) %08x in get_target %s\n",
      base,
      mei,
      offset,
      target,
      SHOW(mei->insn));
  return addr_map.at(target);
}

static void insert_mentry_before(FatMethod* fm,
                                 MethodItemEntry* mentry,
                                 MethodItemEntry* dest) {
  if (dest == nullptr) {
    fm->push_back(*mentry);
  } else {
    fm->insert(fm->iterator_to(*dest), *mentry);
  }
}

static void insert_branch_target(FatMethod* fm,
                                 MethodItemEntry* target,
                                 MethodItemEntry* src) {
  BranchTarget* bt = new BranchTarget();
  bt->type = BRANCH_SIMPLE;
  bt->src = src;

  MethodItemEntry* mentry = new MethodItemEntry(bt);
  insert_mentry_before(fm, mentry, target);
}

// Returns true if the offset could be encoded without modifying fm.
bool encode_offset(FatMethod* fm,
                   MethodItemEntry* branch_op_mie,
                   int32_t offset) {
  DexOpcode bop = branch_op_mie->insn->opcode();
  if (is_goto(bop)) {
    DexOpcode goto_op = goto_for_offset(offset);
    if (goto_op != bop) {
      auto insn = branch_op_mie->insn;
      branch_op_mie->insn = new IRInstruction(goto_op);
      delete insn;
      return false;
    }
  } else if (is_conditional_branch(bop)) {
    // if-* opcodes can only encode up to 16-bit offsets. To handle larger ones
    // we use a goto/32 and have the inverted if-* opcode skip over it. E.g.
    //
    //   if-gt <large offset>
    //   nop
    //
    // becomes
    //
    //   if-le <label>
    //   goto/32 <large offset>
    //   label:
    //   nop
    if (bytecount(offset) > 2) {
      auto insn = branch_op_mie->insn;
      branch_op_mie->insn = new IRInstruction(OPCODE_GOTO_32);

      DexOpcode inverted = invert_conditional_branch(bop);
      MethodItemEntry* mei = new MethodItemEntry(new IRInstruction(inverted));
      mei->insn->set_src(0, insn->src(0));
      insert_mentry_before(fm, mei, branch_op_mie);

      // this iterator should always be valid -- an if-* instruction cannot
      // be the last opcode in a well-formed method
      auto next_insn_it = std::next(fm->iterator_to(*branch_op_mie));
      insert_branch_target(fm, &*next_insn_it, mei);

      delete insn;
      return false;
    }
  } else {
    always_assert_log(false, "Unexpected opcode %s", SHOW(*branch_op_mie));
  }
  branch_op_mie->insn->set_offset(offset);
  return true;
}


static bool multi_target_compare_index(const BranchTarget* a,
                                       const BranchTarget* b) {
  return (a->index < b->index);
}

static bool multi_contains_gaps(const std::vector<BranchTarget*>& targets) {
  int32_t key = targets.front()->index;
  for (auto target : targets) {
    if (target->index != key) return true;
    key++;
  }
  return false;
}

static void insert_multi_branch_target(FatMethod* fm,
                                       int32_t index,
                                       MethodItemEntry* target,
                                       MethodItemEntry* src) {
  BranchTarget* bt = new BranchTarget();
  bt->type = BRANCH_MULTI;
  bt->src = src;
  bt->index = index;

  MethodItemEntry* mentry = new MethodItemEntry(bt);
  insert_mentry_before(fm, mentry, target);
}

static int32_t read_int32(const uint16_t*& data) {
  int32_t result;
  memcpy(&result, data, sizeof(int32_t));
  data += 2;
  return result;
}

static void shard_multi_target(FatMethod* fm,
                               DexOpcodeData* fopcode,
                               MethodItemEntry* src,
                               addr_mei_t& addr_to_mei) {
  const uint16_t* data = fopcode->data();
  uint16_t entries = *data++;
  auto ftype = fopcode->opcode();
  uint32_t base = src->addr;
  if (ftype == FOPCODE_PACKED_SWITCH) {
    int32_t index = read_int32(data);
    for (int i = 0; i < entries; i++) {
      uint32_t targetaddr = base + read_int32(data);
      auto target = addr_to_mei[targetaddr];
      insert_multi_branch_target(fm, index, target, src);
      index++;
    }
  } else if (ftype == FOPCODE_SPARSE_SWITCH) {
    const uint16_t* tdata = data + 2 * entries;  // entries are 32b
    for (int i = 0; i < entries; i++) {
      int32_t index = read_int32(data);
      uint32_t targetaddr = base + read_int32(tdata);
      auto target = addr_to_mei[targetaddr];
      insert_multi_branch_target(fm, index, target, src);
    }
  } else {
    always_assert_log(false, "Bad fopcode 0x%04x in shard_multi_target", ftype);
  }
}

static void generate_branch_targets(
    FatMethod* fm,
    addr_mei_t& addr_to_mei,
    std::unordered_map<uint32_t, DexOpcodeData*>& addr_to_data) {
  for (auto miter = fm->begin(); miter != fm->end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    if (mentry->type == MFLOW_OPCODE) {
      auto insn = mentry->insn;
      if (is_branch(insn->opcode())) {
        if (is_multi_branch(insn->opcode())) {
          auto fopcode = get_target(mentry, addr_to_data);
          shard_multi_target(fm, fopcode, mentry, addr_to_mei);
          delete fopcode;
          // TODO: erase fopcode from map
        } else {
          auto target = get_target(mentry, addr_to_mei);
          insert_branch_target(fm, target, mentry);
        }
      }
    }
  }
}

/*
 * Store the pseudo opcodes representing fill-array-data-payload in a separate
 * hashtable instead of in-line with the rest of the method body.
 */
void gather_array_data(
    FatMethod* fm,
    std::unordered_map<uint32_t, DexOpcodeData*>& addr_to_data,
    std::unordered_map<IRInstruction*, DexOpcodeData*>* m_array_data) {
  std::unordered_set<MethodItemEntry*> to_delete;
  for (MethodItemEntry& mentry : *fm) {
    if (mentry.type != MFLOW_OPCODE) {
      continue;
    }
    auto insn = mentry.insn;
    if (insn->opcode() == OPCODE_FILL_ARRAY_DATA) {
      auto fopcode = get_target(&mentry, addr_to_data);
      m_array_data->emplace(insn, fopcode);
    }
  }
}

static void associate_debug_entries(FatMethod* fm,
                                    DexDebugItem& dbg,
                                    addr_mei_t& addr_to_mei) {
  for (auto& entry : dbg.get_entries()) {
    auto insert_point = addr_to_mei[entry.addr];
    if (!insert_point) {
      /* We don't have a way of emitting debug info for fopcodes.  To
       * be honest, I'm not sure why DX emits them.  We don't.
       */
      TRACE(MTRANS, 5, "Warning..Skipping fopcode debug opcode\n");
      continue;
    }
    MethodItemEntry* mentry;
    switch (entry.type) {
      case DexDebugEntryType::Instruction:
        mentry = new MethodItemEntry(std::move(entry.insn));
        break;
      case DexDebugEntryType::Position:
        mentry = new MethodItemEntry(std::move(entry.pos));
        break;
      default:
        not_reached();
    }
    insert_mentry_before(fm, mentry, insert_point);
  }
  dbg.get_entries().clear();
}

static void associate_try_items(FatMethod* fm,
                                DexCode& code,
                                addr_mei_t& addr_to_mei) {
  auto const& tries = code.get_tries();
  for (auto& tri : tries) {
    MethodItemEntry* catch_start = nullptr;
    CatchEntry* last_catch = nullptr;
    for (auto catz : tri->m_catches) {
      auto catzop = addr_to_mei[catz.second];
      TRACE(MTRANS, 3, "try_catch %08x mei %p\n", catz.second, catzop);
      auto catch_mei = new MethodItemEntry(catz.first);
      catch_start = catch_start == nullptr ? catch_mei : catch_start;
      if (last_catch != nullptr) {
        last_catch->next = catch_mei;
      }
      last_catch = catch_mei->centry;
      insert_mentry_before(fm, catch_mei, catzop);
    }

    auto begin = addr_to_mei[tri->m_start_addr];
    TRACE(MTRANS, 3, "try_start %08x mei %p\n", tri->m_start_addr, begin);
    auto try_start = new MethodItemEntry(TRY_START, catch_start);
    insert_mentry_before(fm, try_start, begin);
    uint32_t lastaddr = tri->m_start_addr + tri->m_insn_count;
    auto end = addr_to_mei[lastaddr];
    TRACE(MTRANS, 3, "try_end %08x mei %p\n", lastaddr, end);
    auto try_end = new MethodItemEntry(TRY_END, catch_start);
    insert_mentry_before(fm, try_end, end);
  }
}

bool has_aliased_arguments(IRInstruction* invoke) {
  assert(invoke->has_methods());
  std::unordered_set<uint16_t> seen;
  for (size_t i = 0; i < invoke->srcs_size(); ++i) {
    auto pair = seen.emplace(invoke->src(i));
    bool did_insert = pair.second;
    if (!did_insert) {
      return true;
    }
  }
  return false;
}

}

FatMethod* IRCode::balloon(DexMethod* method) {
  auto dex_code = method->get_dex_code();
  FatMethod* fmethod = new FatMethod();
  auto instructions = dex_code->release_instructions();
  addr_mei_t addr_to_mei;
  std::unordered_map<uint32_t, DexOpcodeData*> addr_to_data;

  uint32_t addr = 0;
  for (auto insn : *instructions) {
    if (is_fopcode(insn->opcode())) {
      addr_to_data.emplace(addr, static_cast<DexOpcodeData*>(insn));
    } else if (insn->opcode() != OPCODE_NOP) {
      // NOPs are used for alignment, which FatMethod doesn't care about
      MethodItemEntry* mei = new MethodItemEntry(IRInstruction::make(insn));
      fmethod->push_back(*mei);
      addr_to_mei[addr] = mei;
      mei->addr = addr;
      TRACE(MTRANS, 5, "%08x: %s[mei %p]\n", addr, SHOW(insn), mei);
    }
    addr += insn->size();
  }
  generate_branch_targets(fmethod, addr_to_mei, addr_to_data);
  gather_array_data(fmethod, addr_to_data, &m_array_data);
  associate_try_items(fmethod, *dex_code, addr_to_mei);
  auto debugitem = dex_code->get_debug_item();
  if (debugitem) {
    associate_debug_entries(fmethod, *debugitem, addr_to_mei);
  }
  return fmethod;
}

void IRCode::remove_branch_target(IRInstruction *branch_inst) {
  always_assert_log(is_branch(branch_inst->opcode()),
                    "Instruction is not a branch instruction.");
  for (auto miter = m_fmethod->begin(); miter != m_fmethod->end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    if (mentry->type == MFLOW_TARGET) {
      BranchTarget* bt = mentry->target;
      auto btmei = bt->src;
      if(btmei->insn == branch_inst) {
        mentry->type = MFLOW_FALLTHROUGH;
        delete mentry->target;
        mentry->throwing_mie = nullptr;
        break;
      }
    }
  }
}

void IRCode::replace_branch(IRInstruction* from, IRInstruction* to) {
  always_assert(is_branch(from->opcode()));
  always_assert(is_branch(to->opcode()));
  for (auto& mentry : *m_fmethod) {
    if (mentry.type == MFLOW_OPCODE && mentry.insn == from) {
      mentry.insn = to;
      delete from;
      return;
    }
  }
  always_assert_log(
      false,
      "No match found while replacing '%s' with '%s'",
      SHOW(from),
      SHOW(to));
}

void IRCode::replace_opcode_with_infinite_loop(IRInstruction* from) {
  IRInstruction* to = new IRInstruction(OPCODE_GOTO_32);
  to->set_offset(0);
  for (auto miter = m_fmethod->begin(); miter != m_fmethod->end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    if (mentry->type == MFLOW_OPCODE && mentry->insn == from) {
      if (is_branch(from->opcode())) {
        remove_branch_target(from);
      }
      mentry->insn = to;
      delete from;
      return;
    }
  }
  always_assert_log(
      false,
      "No match found while replacing '%s' with '%s'",
      SHOW(from),
      SHOW(to));
}

void IRCode::replace_opcode(IRInstruction* from, IRInstruction* to) {
  always_assert_log(!is_branch(to->opcode()),
                    "You may want replace_branch instead");
  for (auto miter = m_fmethod->begin(); miter != m_fmethod->end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    if (mentry->type == MFLOW_OPCODE && mentry->insn == from) {
      if (is_branch(from->opcode())) {
        remove_branch_target(from);
      }
      mentry->insn = to;
      delete from;
      return;
    }
  }
  always_assert_log(
      false,
      "No match found while replacing '%s' with '%s'",
      SHOW(from),
      SHOW(to));
}

void IRCode::insert_after(IRInstruction* position,
                                   const std::vector<IRInstruction*>& opcodes) {
  /* The nullptr case handling is strange-ish..., this will not work as expected
   *if
   * a method has a branch target as it's first instruction.
   *
   * To handle this case sanely, we'd need to export a interface based on
   * MEI's probably.
   *
   */
  for (auto const& mei : *m_fmethod) {
    if (mei.type == MFLOW_OPCODE &&
        (position == nullptr || mei.insn == position)) {
      auto insertat = m_fmethod->iterator_to(mei);
      if (position != nullptr) insertat++;
      for (auto* opcode : opcodes) {
        MethodItemEntry* mentry = new MethodItemEntry(opcode);
        m_fmethod->insert(insertat, *mentry);
      }
      return;
    }
  }
  always_assert_log(false, "No match found");
}

FatMethod::iterator IRCode::insert_before(
    const FatMethod::iterator& position, MethodItemEntry& mie) {
  return m_fmethod->insert(position, mie);
}

FatMethod::iterator IRCode::insert_after(
    const FatMethod::iterator& position, MethodItemEntry& mie) {
  always_assert(position != m_fmethod->end());
  return m_fmethod->insert(std::next(position), mie);
}

/*
 * Param `insn` should be part of a switch...case statement. Find the case
 * block it is contained within and remove it. Then decrement the index of
 * all the other case blocks that are larger than the index of the removed
 * block so that the case numbers don't have any gaps and the switch can
 * still be encoded as a packed-switch opcode.
 *
 * We do the removal by removing the MFLOW_TARGET corresponding to that
 * case label. Its contents are dead code which will be removed by LocalDCE
 * later. (We could do it here too, but LocalDCE already knows how to find
 * block boundaries.)
 */
void IRCode::remove_switch_case(IRInstruction* insn) {

  TRACE(MTRANS, 3, "Removing switch case from: %s\n", SHOW(m_fmethod));
  // Check if we are inside switch method.
  MethodItemEntry* switch_mei {nullptr};
  for (auto& mei : *m_fmethod) {
    if (mei.type != MFLOW_OPCODE) continue;
    assert_log(is_multi_branch(mei.insn->opcode()), " Method is not a switch");
    switch_mei = &mei;
    break;
  }
  always_assert(switch_mei != nullptr);

  int target_count = 0;
  for (auto& mei : *m_fmethod) {
    if (mei.type == MFLOW_TARGET && mei.target->type == BRANCH_MULTI) {
      target_count++;
    }
  }
  assert_log(target_count != 0, " There should be atleast one target");
  if (target_count == 1) {
    auto excpt_str = DexString::make_string("Redex switch Exception");
    std::vector<IRInstruction*> excpt_block;
    create_runtime_exception_block(excpt_str, excpt_block);
    insert_after(insn, excpt_block);
    remove_opcode(insn);
    return;
  }

  // Find the starting MULTI Target point to delete.
  MethodItemEntry* target_mei = nullptr;
  for (auto miter = m_fmethod->begin(); miter != m_fmethod->end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    if (mentry->type == MFLOW_TARGET) {
      target_mei = mentry;
    }
    // Check if insn belongs to the current block.
    if (mentry->type == MFLOW_OPCODE && mentry->insn == insn) {
      break;
    }
  }
  always_assert_log(target_mei != nullptr,
                    "Could not find target for %s in %s",
                    SHOW(insn),
                    SHOW(m_fmethod));

  for (const auto& mie : *m_fmethod) {
    if (mie.type == MFLOW_TARGET) {
      BranchTarget* bt = mie.target;
      if (bt->src == switch_mei && bt->index > target_mei->target->index) {
        bt->index -= 1;
      }
    }
  }

  target_mei->type = MFLOW_FALLTHROUGH;
  delete target_mei->target;
  target_mei->throwing_mie = nullptr;
}

size_t IRCode::count_opcodes() const {
  size_t count {0};
  for (const auto& mie : *m_fmethod) {
    if (mie.type == MFLOW_OPCODE) {
      ++count;
    }
  }
  return count;
}

size_t IRCode::sum_opcode_sizes() const {
  size_t size {0};
  for (const auto& mie : *m_fmethod) {
    if (mie.type == MFLOW_OPCODE) {
      size += mie.insn->size();
    }
  }
  return size;
}

void IRCode::remove_opcode(const FatMethod::iterator& it) {
  always_assert(it->type == MFLOW_OPCODE);
  auto insn = it->insn;
  if (may_throw(insn->opcode())) {
    for (auto rev = --FatMethod::reverse_iterator(it);
         rev != m_fmethod->rend();
         ++rev) {
      if (rev->type == MFLOW_FALLTHROUGH && rev->throwing_mie) {
        assert(rev->throwing_mie == &*it);
        rev->throwing_mie = nullptr;
        break;
      } else if (rev->type == MFLOW_OPCODE) {
        break;
      }
    }
  }
  if (is_branch(insn->opcode())) {
    remove_branch_target(insn);
  }
  it->type = MFLOW_FALLTHROUGH;
  it->insn = nullptr;
  delete insn;
}

void IRCode::remove_opcode(IRInstruction* insn) {
  for (auto& mei : *m_fmethod) {
    if (mei.type == MFLOW_OPCODE && mei.insn == insn) {
      auto it = m_fmethod->iterator_to(mei);
      remove_opcode(it);
      return;
    }
  }
  always_assert_log(false,
                    "No match found while removing '%s' from method",
                    SHOW(insn));
}

FatMethod::iterator IRCode::main_block() { return m_fmethod->begin(); }

FatMethod::iterator IRCode::insert(FatMethod::iterator cur,
                                            IRInstruction* insn) {
  MethodItemEntry* mentry = new MethodItemEntry(insn);
  return m_fmethod->insert(cur, *mentry);
}

FatMethod::iterator IRCode::make_if_block(
    FatMethod::iterator cur,
    IRInstruction* insn,
    FatMethod::iterator* false_block) {
  auto if_entry = new MethodItemEntry(insn);
  *false_block = m_fmethod->insert(cur, *if_entry);
  auto bt = new BranchTarget();
  bt->src = if_entry;
  bt->type = BRANCH_SIMPLE;
  auto bentry = new MethodItemEntry(bt);
  return m_fmethod->insert(m_fmethod->end(), *bentry);
}

FatMethod::iterator IRCode::make_if_else_block(
    FatMethod::iterator cur,
    IRInstruction* insn,
    FatMethod::iterator* false_block,
    FatMethod::iterator* true_block) {
  // if block
  auto if_entry = new MethodItemEntry(insn);
  *false_block = m_fmethod->insert(cur, *if_entry);

  // end of else goto
  auto goto_entry = new MethodItemEntry(new IRInstruction(OPCODE_GOTO));
  auto goto_it = m_fmethod->insert(m_fmethod->end(), *goto_entry);

  // main block
  auto main_bt = new BranchTarget();
  main_bt->src = goto_entry;
  main_bt->type = BRANCH_SIMPLE;
  auto mb_entry = new MethodItemEntry(main_bt);
  auto main_block = m_fmethod->insert(goto_it, *mb_entry);

  // else block
  auto else_bt = new BranchTarget();
  else_bt->src = if_entry;
  else_bt->type = BRANCH_SIMPLE;
  auto eb_entry = new MethodItemEntry(else_bt);
  *true_block = m_fmethod->insert(goto_it, *eb_entry);

  return main_block;
}

FatMethod::iterator IRCode::make_switch_block(
    FatMethod::iterator cur,
    IRInstruction* insn,
    FatMethod::iterator* default_block,
    std::map<int, FatMethod::iterator>& cases) {
  auto switch_entry = new MethodItemEntry(insn);
  *default_block = m_fmethod->insert(cur, *switch_entry);
  FatMethod::iterator main_block = *default_block;
  for (auto case_it = cases.begin(); case_it != cases.end(); ++case_it) {
    auto goto_entry = new MethodItemEntry(new IRInstruction(OPCODE_GOTO));
    auto goto_it = m_fmethod->insert(m_fmethod->end(), *goto_entry);

    auto main_bt = new BranchTarget();
    main_bt->src = goto_entry;
    main_bt->type = BRANCH_SIMPLE;
    auto mb_entry = new MethodItemEntry(main_bt);
    main_block = m_fmethod->insert(++main_block, *mb_entry);

    // case block
    auto case_bt = new BranchTarget();
    case_bt->src = switch_entry;
    case_bt->index = case_it->first;
    case_bt->type = BRANCH_MULTI;
    auto eb_entry = new MethodItemEntry(case_bt);
    case_it->second = m_fmethod->insert(goto_it, *eb_entry);
  }
  return main_block;
}

namespace {
using RegMap = std::unordered_map<uint16_t, uint16_t>;

/*
 * If the callee has wide instructions, naive 1-to-1 remapping of registers
 * won't work. Suppose we want to map v1 in the callee to v1 in the caller,
 * because we know v1 is not live. If the instruction using v1 in the callee
 * is wide, we need to check that v2 in the caller is also not live.
 *
 * Similarly, range opcodes require contiguity in their registers, and that
 * cannot be handled by a naive 1-1 remapping.
 */
bool simple_reg_remap(IRCode* mt) {
  for (auto& mie : InstructionIterable(mt)) {
    auto insn = mie.insn;
    if (insn->is_wide() || opcode::has_range(insn->opcode())) {
      return false;
    }
  }
  return true;
}

const char* DEBUG_ONLY show_reg_map(RegMap& map) {
  for (auto pair : map) {
    TRACE(INL, 5, "%u -> %u\n", pair.first, pair.second);
  }
  return "";
}

void remap_dest(IRInstruction* inst, const RegMap& reg_map) {
  if (!inst->dests_size()) return;
  auto it = reg_map.find(inst->dest());
  if (it == reg_map.end()) return;
  inst->set_dest(it->second);
}

void remap_srcs(IRInstruction* inst, const RegMap& reg_map) {
  for (unsigned i = 0; i < inst->srcs_size(); i++) {
    auto it = reg_map.find(inst->src(i));
    if (it == reg_map.end()) continue;
    inst->set_src(i, it->second);
  }
}

void remap_debug(DexDebugInstruction& dbgop, const RegMap& reg_map) {
  switch (dbgop.opcode()) {
  case DBG_START_LOCAL:
  case DBG_START_LOCAL_EXTENDED:
  case DBG_END_LOCAL:
  case DBG_RESTART_LOCAL: {
    auto it = reg_map.find(dbgop.uvalue());
    if (it == reg_map.end()) return;
    dbgop.set_uvalue(it->second);
    break;
  }
  default:
    break;
  }
}

void remap_registers(IRInstruction* insn, const RegMap& reg_map) {
  remap_dest(insn, reg_map);
  remap_srcs(insn, reg_map);

  if (opcode::has_range(insn->opcode())) {
    auto it = reg_map.find(insn->range_base());
    if (it != reg_map.end()) {
      insn->set_range_base(it->second);
    }
  }
}

void remap_registers(MethodItemEntry& mei, const RegMap& reg_map) {
  switch (mei.type) {
  case MFLOW_OPCODE:
    remap_registers(mei.insn, reg_map);
    break;
  case MFLOW_DEBUG:
    remap_debug(*mei.dbgop, reg_map);
    break;
  default:
    break;
  }
}

void remap_registers(FatMethod* fmethod, const RegMap& reg_map) {
  for (auto& mei : *fmethod) {
    remap_registers(mei, reg_map);
  }
}

void remap_reg_set(RegSet& reg_set, const RegMap& reg_map, uint16_t newregs) {
  RegSet mapped(newregs);
  for (auto pair : reg_map) {
    mapped[pair.second] = reg_set[pair.first];
    reg_set[pair.first] = false;
  }
  reg_set.resize(newregs);
  reg_set |= mapped;
}

void enlarge_registers(IRCode* code,
                       FatMethod* fmethod,
                       uint16_t newregs) {
  RegMap reg_map;
  auto oldregs = code->get_registers_size();
  auto ins = code->get_ins_size();
  for (uint16_t i = 0; i < ins; ++i) {
    reg_map[oldregs - ins + i] = newregs - ins + i;
  }
  remap_registers(fmethod, reg_map);
  code->set_registers_size(newregs);
}

void remap_callee_regs(IRInstruction* invoke,
                       DexMethod* method,
                       FatMethod* fmethod,
                       uint16_t newregs) {
  RegMap reg_map;
  auto oldregs = method->get_code()->get_registers_size();
  auto ins = method->get_code()->get_ins_size();
  auto wc = invoke->arg_word_count();
  always_assert(ins == wc);
  for (uint16_t i = 0; i < wc; ++i) {
    reg_map[oldregs - ins + i] = invoke->src(i);
  }
  remap_registers(fmethod, reg_map);
  method->get_code()->set_registers_size(newregs);
}

/**
 * Maps the callee param registers to the argument registers of the caller's
 * invoke instruction.
 */
RegMap build_callee_param_reg_map(IRInstruction* invoke, DexMethod* callee) {
  RegMap reg_map;
  auto oldregs = callee->get_code()->get_registers_size();
  auto ins = callee->get_code()->get_ins_size();
  if (is_invoke_range(invoke->opcode())) {
    auto base = invoke->range_base();
    auto range = invoke->range_size();
    always_assert(ins == range);
    for (uint16_t i = 0; i < range; ++i) {
      reg_map[oldregs - ins + i] = base + i;
    }
  } else {
    auto wc = invoke->arg_word_count();
    always_assert(ins == wc);
    for (uint16_t i = 0; i < wc; ++i) {
      reg_map[oldregs - ins + i] = invoke->src(i);
    }
  }
  return reg_map;
}

/**
 * Builds a register map for a callee.
 */
RegMap build_callee_reg_map(IRInstruction* invoke,
                            DexMethod* callee,
                            RegSet invoke_live_in) {
  RegMap reg_map;
  auto oldregs = callee->get_code()->get_registers_size();
  auto ins = callee->get_code()->get_ins_size();
  // remap all local regs (not args)
  auto avail_regs = ~invoke_live_in;
  auto caller_reg = avail_regs.find_first();
  for (uint16_t i = 0; i < oldregs - ins; ++i) {
    always_assert_log(caller_reg != RegSet::npos,
                      "Ran out of caller registers for callee register %d", i);
    reg_map[i] = caller_reg;
    caller_reg = avail_regs.find_next(caller_reg);
  }
  auto param_reg_map = build_callee_param_reg_map(invoke, callee);
  for (auto pair : param_reg_map) {
    reg_map[pair.first] = pair.second;
  }
  return reg_map;
}

/**
 * Create a move instruction given a return instruction in a callee and
 * a move-result instruction in a caller.
 */
IRInstruction* move_result(IRInstruction* res, IRInstruction* move_res) {
  auto opcode = res->opcode();
  always_assert(opcode != OPCODE_RETURN_VOID);
  IRInstruction* move;
  if (opcode == OPCODE_RETURN_OBJECT) {
    move = new IRInstruction(OPCODE_MOVE_OBJECT);
  } else if (opcode == OPCODE_RETURN_WIDE) {
    move = new IRInstruction(OPCODE_MOVE_WIDE);
  } else {
    always_assert(opcode == OPCODE_RETURN);
    move = new IRInstruction(OPCODE_MOVE);
  }
  move->set_dest(move_res->dest());
  move->set_src(0, res->src(0));
  return move;
}

void cleanup_callee_debug(FatMethod* fcallee) {
  std::unordered_set<uint16_t> valid_regs;
  auto it = fcallee->begin();
  while (it != fcallee->end()) {
    auto& mei = *it++;
    if (mei.type == MFLOW_DEBUG) {
      switch(mei.dbgop->opcode()) {
      case DBG_SET_PROLOGUE_END:
        fcallee->erase(fcallee->iterator_to(mei));
        break;
      case DBG_START_LOCAL:
      case DBG_START_LOCAL_EXTENDED: {
        auto reg = mei.dbgop->uvalue();
        valid_regs.insert(reg);
        break;
      }
      case DBG_END_LOCAL:
      case DBG_RESTART_LOCAL: {
        auto reg = mei.dbgop->uvalue();
        if (valid_regs.find(reg) == valid_regs.end()) {
          fcallee->erase(fcallee->iterator_to(mei));
        }
        break;
      }
      default:
        break;
      }
    }
  }
}

}

/*
 * For splicing a callee's FatMethod into a caller.
 */
class MethodSplicer {
  IRCode* m_mtcaller;
  IRCode* m_mtcallee;
  // We need a map of MethodItemEntry we have created because a branch
  // points to another MethodItemEntry which may have been created or not
  std::unordered_map<MethodItemEntry*, MethodItemEntry*> m_entry_map;
  // for remapping the parent position pointers
  std::unordered_map<DexPosition*, DexPosition*> m_pos_map;
  const RegMap& m_callee_reg_map;
  DexPosition* m_invoke_position;
  MethodItemEntry* m_active_catch;
  std::unordered_set<uint16_t> m_valid_dbg_regs;

 public:
  MethodSplicer(IRCode* mtcaller,
                IRCode* mtcallee,
                const RegMap& callee_reg_map,
                DexPosition* invoke_position,
                MethodItemEntry* active_catch)
      : m_mtcaller(mtcaller),
        m_mtcallee(mtcallee),
        m_callee_reg_map(callee_reg_map),
        m_invoke_position(invoke_position),
        m_active_catch(active_catch) {
    m_entry_map[nullptr] = nullptr;
    m_pos_map[nullptr] = nullptr;
  }

  MethodItemEntry* clone(MethodItemEntry* mei) {
    MethodItemEntry* cloned_mei;
    auto entry = m_entry_map.find(mei);
    if (entry != m_entry_map.end()) {
      return entry->second;
    }
    cloned_mei = new MethodItemEntry(*mei);
    m_entry_map[mei] = cloned_mei;
    switch (cloned_mei->type) {
    case MFLOW_TRY:
      cloned_mei->tentry = new TryEntry(*cloned_mei->tentry);
      cloned_mei->tentry->catch_start = clone(cloned_mei->tentry->catch_start);
      return cloned_mei;
    case MFLOW_CATCH:
      cloned_mei->centry = new CatchEntry(*cloned_mei->centry);
      cloned_mei->centry->next = clone(cloned_mei->centry->next);
      return cloned_mei;
    case MFLOW_OPCODE:
      cloned_mei->insn = cloned_mei->insn->clone();
      if (cloned_mei->insn->opcode() == OPCODE_FILL_ARRAY_DATA) {
        m_mtcaller->m_array_data.emplace(
            cloned_mei->insn, m_mtcallee->m_array_data.at(mei->insn)->clone());
      }
      return cloned_mei;
    case MFLOW_TARGET:
      cloned_mei->target = new BranchTarget(*cloned_mei->target);
      cloned_mei->target->src = clone(cloned_mei->target->src);
      return cloned_mei;
    case MFLOW_DEBUG:
      return cloned_mei;
    case MFLOW_POSITION:
      m_pos_map[mei->pos.get()] = cloned_mei->pos.get();
      cloned_mei->pos->parent = m_pos_map.at(cloned_mei->pos->parent);
      return cloned_mei;
    case MFLOW_FALLTHROUGH:
      return cloned_mei;
    }
    not_reached();
  }

  void operator()(FatMethod::iterator insert_pos,
                  FatMethod::iterator fcallee_start,
                  FatMethod::iterator fcallee_end) {
    auto fcaller = m_mtcaller->m_fmethod;
    for (auto it = fcallee_start; it != fcallee_end; ++it) {
      if (should_skip_debug(&*it)) {
        continue;
      }
      auto mei = clone(&*it);
      remap_registers(*mei, m_callee_reg_map);
      if (mei->type == MFLOW_TRY && m_active_catch != nullptr) {
        auto tentry = mei->tentry;
        // try ranges cannot be nested, so we flatten them here
        switch (tentry->type) {
          case TRY_START:
            fcaller->insert(insert_pos,
                *(new MethodItemEntry(TRY_END, m_active_catch)));
            fcaller->insert(insert_pos, *mei);
            break;
          case TRY_END:
            fcaller->insert(insert_pos, *mei);
            fcaller->insert(insert_pos,
                *(new MethodItemEntry(TRY_START, m_active_catch)));
            break;
        }
      } else {
        if (mei->type == MFLOW_POSITION && mei->pos->parent == nullptr) {
          mei->pos->parent = m_invoke_position;
        }
        // if a handler list does not terminate in a catch-all, have it point to
        // the parent's active catch handler. TODO: Make this more precise by
        // checking if the parent catch type is a subtype of the callee's.
        if (mei->type == MFLOW_CATCH && mei->centry->next == nullptr &&
            mei->centry->catch_type != nullptr) {
          mei->centry->next = m_active_catch;
        }
        fcaller->insert(insert_pos, *mei);
      }
    }
  }

 private:
  /* We need to skip two cases:
   * Duplicate DBG_SET_PROLOGUE_END
   * Uninitialized parameters
   *
   * The parameter names are part of the debug info for the method.
   * The technically correct solution would be to make a start
   * local for each of them.  However, that would also imply another
   * end local after the tail to correctly set what the register
   * is at the end.  This would bloat the debug info parameters for
   * a corner case.
   *
   * Instead, we just delete locals lifetime information for parameters.
   * This is an exceedingly rare case triggered by goofy code that
   * reuses parameters as locals.
   */
  bool should_skip_debug(const MethodItemEntry* mei) {
    if (mei->type != MFLOW_DEBUG) {
      return false;
    }
    switch (mei->dbgop->opcode()) {
    case DBG_SET_PROLOGUE_END:
      return true;
    case DBG_START_LOCAL:
    case DBG_START_LOCAL_EXTENDED: {
      auto reg = mei->dbgop->uvalue();
      m_valid_dbg_regs.insert(reg);
      return false;
    }
    case DBG_END_LOCAL:
    case DBG_RESTART_LOCAL: {
      auto reg = mei->dbgop->uvalue();
      if (m_valid_dbg_regs.find(reg) == m_valid_dbg_regs.end()) {
        return true;
      }
    }
    default:
      return false;
    }
  }
};

namespace {

MethodItemEntry* find_active_catch(FatMethod* method,
                                   FatMethod::iterator pos) {
  while (++pos != method->end() && pos->type != MFLOW_TRY);
  return pos != method->end() && pos->tentry->type == TRY_END
    ? pos->tentry->catch_start : nullptr;
}

/**
 * Return a RegSet indicating the registers that the callee interferes with
 * either via a check-cast to or by writing to one of the ins.
 * When inlining, writing over one of the ins may change the type of the
 * register to a type that breaks the invariants in the caller.
 */
RegSet ins_reg_defs(IRCode& code) {
  RegSet def_ins(code.get_registers_size());
  for (auto& mie : InstructionIterable(&code)) {
    auto insn = mie.insn;
    if (insn->opcode() == OPCODE_CHECK_CAST) {
      def_ins.set(insn->src(0));
    } else if (insn->dests_size() > 0) {
      def_ins.set(insn->dest());
      if (insn->dest_is_wide()) {
        def_ins.set(insn->dest() + 1);
      }
    }
  }
  // temp_regs are the first n registers in the method that are not ins.
  // Dx methods use the last k registers for the arguments (where k is the size
  // of the args).
  // So an instruction writes an ins if it has a destination and the
  // destination is bigger or equal than temp_regs.
  auto temp_regs = code.get_registers_size() - code.get_ins_size();
  RegSet param_filter(temp_regs);
  param_filter.resize(code.get_registers_size(), true);
  return param_filter & def_ins;
}

}

void IRCode::inline_tail_call(DexMethod* caller,
                                       DexMethod* callee,
                                       IRInstruction* invoke) {
  TRACE(INL, 2, "caller: %s\ncallee: %s\n", SHOW(caller), SHOW(callee));
  auto fcaller = caller->get_code()->m_fmethod;
  auto fcallee = callee->get_code()->m_fmethod;

  auto bregs = caller->get_code()->get_registers_size();
  auto eregs = callee->get_code()->get_registers_size();
  auto bins = caller->get_code()->get_ins_size();
  auto eins = callee->get_code()->get_ins_size();
  always_assert(bins >= eins);
  auto newregs = std::max(bregs, uint16_t(eregs + bins - eins));
  always_assert(newregs <= 16);

  // Remap registers to account for possibly larger frame, more ins
  enlarge_registers(&*caller->get_code(), fcaller, newregs);
  remap_callee_regs(invoke, callee, fcallee, newregs);

  callee->get_code()->set_ins_size(bins);

  auto pos = std::find_if(fcaller->begin(),
                          fcaller->end(),
                          [invoke](const MethodItemEntry& mei) {
                            return mei.type == MFLOW_OPCODE && mei.insn == invoke;
                          });

  cleanup_callee_debug(fcallee);
  auto it = fcallee->begin();
  while (it != fcallee->end()) {
    auto& mei = *it++;
    fcallee->erase(fcallee->iterator_to(mei));
    fcaller->insert(pos, mei);
  }
  // Delete the vestigial tail.
  while (pos != fcaller->end()) {
    if (pos->type == MFLOW_OPCODE) {
      pos = fcaller->erase_and_dispose(pos, FatMethodDisposer());
    } else {
      ++pos;
    }
  }

  caller->get_code()->set_outs_size(callee->get_code()->get_outs_size());
}

bool IRCode::inline_method(InlineContext& context,
                           DexMethod* callee,
                           IRMethodInstruction* invoke,
                           bool no_exceed_16regs) {
  auto caller_code = context.caller_code;
  TRACE(INL, 5, "callee code:\n%s\n", SHOW(callee->get_code()));
  uint16_t newregs = caller_code->get_registers_size();
  if (no_exceed_16regs && newregs > 16) {
    return false;
  }

  auto callee_code = callee->get_code();
  bool simple_remap_ok = simple_reg_remap(&*callee_code);
  // if the simple approach won't work, just be conservative and assume all
  // caller temp regs are live
  auto invoke_live_out = context.live_out(invoke);
  if (!simple_remap_ok) {
    auto rs = RegSet(context.original_regs);
    rs.flip();
    invoke_live_out = Liveness(std::move(rs));
  }
  // the caller liveness info is cached across multiple inlinings but the caller
  // regs may have increased in the meantime, so update the liveness here
  invoke_live_out.enlarge(caller_code->get_ins_size(), newregs);

  auto callee_param_reg_map = build_callee_param_reg_map(invoke, callee);
  auto def_ins = ins_reg_defs(*callee_code);
  // if we map two callee registers v0 and v1 to the same caller register v2,
  // and v1 gets written to in the callee, we're gonna have a bad time
  if (def_ins.any() && has_aliased_arguments(invoke)) {
    return false;
  }
  remap_reg_set(def_ins, callee_param_reg_map, newregs);
  if (def_ins.intersects(invoke_live_out.bits())) {
    return false;
  }

  auto fcaller = caller_code->m_fmethod;
  auto fcallee = callee_code->m_fmethod;

  auto temps_needed =
      callee_code->get_registers_size() - callee_code->get_ins_size();
  auto invoke_live_in = invoke_live_out;
  Liveness::trans(invoke, &invoke_live_in);
  uint16_t temps_avail = newregs - invoke_live_in.bits().count();
  if (temps_avail < temps_needed) {
    newregs += temps_needed - temps_avail;
    if (no_exceed_16regs && newregs > 16) {
      return false;
    }
    enlarge_registers(caller_code, fcaller, newregs);
    invoke_live_in.enlarge(caller_code->get_ins_size(), newregs);
    invoke_live_out.enlarge(caller_code->get_ins_size(), newregs);
  }
  auto callee_reg_map =
      build_callee_reg_map(invoke, callee, invoke_live_in.bits());
  TRACE(INL, 5, "Callee reg map\n");
  TRACE(INL, 5, "%s", show_reg_map(callee_reg_map));

  auto pos = std::find_if(
    fcaller->begin(), fcaller->end(),
    [invoke](const MethodItemEntry& mei) {
      return mei.type == MFLOW_OPCODE && mei.insn == invoke;
    });
  // find the move-result after the invoke, if any. Must be the first
  // instruction after the invoke
  auto move_res = pos;
  while (move_res++ != fcaller->end() && move_res->type != MFLOW_OPCODE);
  if (!is_move_result(move_res->insn->opcode())) {
    move_res = fcaller->end();
  }

  // find the last position entry before the invoke.
  // we need to decrement the reverse iterator because it gets constructed
  // as pointing to the element preceding pos
  auto position_it = --FatMethod::reverse_iterator(pos);
  while (++position_it != fcaller->rend()
      && position_it->type != MFLOW_POSITION);
  std::unique_ptr<DexPosition> pos_nullptr;
  auto& invoke_position =
    position_it == fcaller->rend() ? pos_nullptr : position_it->pos;
  if (invoke_position) {
    TRACE(INL, 3, "Inlining call at %s:%d\n",
          invoke_position->file->c_str(),
          invoke_position->line);
  }

  // check if we are in a try block
  auto caller_catch = find_active_catch(fcaller, pos);

  // Copy the callee up to the return. Everything else we push at the end
  // of the caller
  auto splice = MethodSplicer(&*caller_code,
                              &*callee_code,
                              callee_reg_map,
                              invoke_position.get(),
                              caller_catch);
  auto ret_it = std::find_if(
      fcallee->begin(), fcallee->end(), [](const MethodItemEntry& mei) {
        return mei.type == MFLOW_OPCODE && is_return(mei.insn->opcode());
      });
  splice(pos, fcallee->begin(), ret_it);

  // try items can span across a return opcode
  auto callee_catch = splice.clone(find_active_catch(fcallee, ret_it));
  if (callee_catch != nullptr) {
    fcaller->insert(pos, *(new MethodItemEntry(TRY_END, callee_catch)));
    if (caller_catch != nullptr) {
      fcaller->insert(pos, *(new MethodItemEntry(TRY_START, caller_catch)));
    }
  }

  if (move_res != fcaller->end() && ret_it != fcallee->end()) {
    std::unique_ptr<IRInstruction> ret_insn(ret_it->insn->clone());
    remap_registers(ret_insn.get(), callee_reg_map);
    IRInstruction* move = move_result(ret_insn.get(), move_res->insn);
    auto move_mei = new MethodItemEntry(move);
    fcaller->insert(pos, *move_mei);
  }
  // ensure that the caller's code after the inlined method retain their
  // original position
  if (invoke_position) {
    fcaller->insert(pos,
                    *(new MethodItemEntry(
                        std::make_unique<DexPosition>(*invoke_position))));
  }

  // remove invoke
  fcaller->erase_and_dispose(pos, FatMethodDisposer());
  // remove move_result
  if (move_res != fcaller->end()) {
    fcaller->erase_and_dispose(move_res, FatMethodDisposer());
  }

  if (ret_it != fcallee->end()) {
    if (callee_catch != nullptr) {
      fcaller->push_back(*(new MethodItemEntry(TRY_START, callee_catch)));
    } else if (caller_catch != nullptr) {
      fcaller->push_back(*(new MethodItemEntry(TRY_START, caller_catch)));
    }
    // Copy the opcodes in the callee after the return and put them at the end
    // of the caller.
    splice(fcaller->end(), std::next(ret_it), fcallee->end());
    if (caller_catch != nullptr) {
      fcaller->push_back(*(new MethodItemEntry(TRY_END, caller_catch)));
    }
  }

  // adjust method header
  caller_code->set_registers_size(newregs);
  caller_code->set_outs_size(
      std::max(callee->get_code()->get_outs_size(),
      caller_code->get_outs_size()));
  return true;
}

void IRCode::enlarge_regs(DexMethod* method, uint16_t newregs) {
  auto code = method->get_code();
  always_assert(code != nullptr);
  always_assert(code->get_registers_size() <= newregs);

  auto fcaller = code->m_fmethod;

  enlarge_registers(&*code, fcaller, newregs);
}

namespace {
bool end_of_block(const FatMethod* fm,
                  FatMethod::iterator it,
                  bool in_try,
                  bool end_block_before_throw) {
  auto next = std::next(it);
  if (next == fm->end()) {
    return true;
  }
  if (next->type == MFLOW_TARGET || next->type == MFLOW_TRY ||
      next->type == MFLOW_CATCH) {
    return true;
  }
  if (end_block_before_throw) {
    if (in_try && it->type == MFLOW_FALLTHROUGH &&
        it->throwing_mie != nullptr) {
      return true;
    }
  } else {
    if (in_try && it->type == MFLOW_OPCODE && may_throw(it->insn->opcode())) {
      return true;
    }
  }
  if (it->type != MFLOW_OPCODE) {
    return false;
  }
  if (is_branch(it->insn->opcode()) || is_return(it->insn->opcode()) ||
      it->insn->opcode() == OPCODE_THROW) {
    return true;
  }
  return false;
}

void split_may_throw(FatMethod* fm, FatMethod::iterator it) {
  auto& mie = *it;
  if (mie.type == MFLOW_OPCODE && may_throw(mie.insn->opcode())) {
    fm->insert(it, *MethodItemEntry::make_throwing_fallthrough(&mie));
  }
}
}

bool ends_with_may_throw(Block* p, bool end_block_before_throw) {
  if (!end_block_before_throw) {
    for (auto last = p->rbegin(); last != p->rend(); ++last) {
      if (last->type != MFLOW_OPCODE) {
        continue;
      }
      return last->insn->opcode() == OPCODE_THROW ||
             may_throw(last->insn->opcode());
    }
  }
  for (auto last = p->rbegin(); last != p->rend(); ++last) {
    switch (last->type) {
    case MFLOW_FALLTHROUGH:
      if (last->throwing_mie) {
        return true;
      }
      break;
    case MFLOW_OPCODE:
      if (last->insn->opcode() == OPCODE_THROW) {
        return true;
      } else {
        return false;
      }
    case MFLOW_TRY:
    case MFLOW_CATCH:
    case MFLOW_TARGET:
    case MFLOW_POSITION:
    case MFLOW_DEBUG:
      break;
    }
  }
  return false;
}

void IRCode::clear_cfg() {
  m_cfg.reset();
  std::vector<FatMethod::iterator> fallthroughs;
  for (auto it = m_fmethod->begin(); it != m_fmethod->end(); ++it) {
    if (it->type == MFLOW_FALLTHROUGH) {
      fallthroughs.emplace_back(it);
    }
  }
  for (auto it : fallthroughs) {
    m_fmethod->erase_and_dispose(it, FatMethodDisposer());
  }
}

void IRCode::build_cfg(bool end_block_before_throw) {
  clear_cfg();
  m_cfg = std::make_unique<ControlFlowGraph>();
  // Find the block boundaries
  std::unordered_map<MethodItemEntry*, std::vector<Block*>> branch_to_targets;
  std::vector<std::pair<TryEntry*, Block*>> try_ends;
  std::unordered_map<CatchEntry*, Block*> try_catches;
  size_t id = 0;
  bool in_try = false;
  auto& blocks = m_cfg->blocks();
  blocks.emplace_back(new Block(id++));
  blocks.back()->m_begin = m_fmethod->begin();
  // The first block can be a branch target.
  auto begin = m_fmethod->begin();
  if (begin->type == MFLOW_TARGET) {
    branch_to_targets[begin->target->src].push_back(blocks.back());
  }
  for (auto it = m_fmethod->begin(); it != m_fmethod->end(); ++it) {
    split_may_throw(m_fmethod, it);
  }
  for (auto it = m_fmethod->begin(); it != m_fmethod->end(); ++it) {
    if (it->type == MFLOW_TRY) {
      if (it->tentry->type == TRY_START) {
        in_try = true;
      } else if (it->tentry->type == TRY_END) {
        in_try = false;
      }
    }
    if (!end_of_block(m_fmethod, it, in_try, end_block_before_throw)) {
      continue;
    }
    // End the current block.
    auto next = std::next(it);
    if (next == m_fmethod->end()) {
      blocks.back()->m_end = next;
      continue;
    }
    // Start a new block at the next MethodItem.
    auto next_block = new Block(id++);
    if (next->type == MFLOW_OPCODE) {
      next = std::next(it);
    }
    blocks.back()->m_end = next;
    next_block->m_begin = next;
    blocks.emplace_back(next_block);
    // Record branch targets to add edges in the next pass.
    if (next->type == MFLOW_TARGET) {
      branch_to_targets[next->target->src].push_back(next_block);
      continue;
    }
    // Record try/catch blocks to add edges in the next pass.
    if (next->type == MFLOW_TRY && next->tentry->type == TRY_END) {
      try_ends.emplace_back(next->tentry, next_block);
    } else if (next->type == MFLOW_CATCH) {
      try_catches[next->centry] = next_block;
    }
  }
  // Link the blocks together with edges
  for (auto it = blocks.begin(); it != blocks.end(); ++it) {
    // Set outgoing edge if last MIE falls through
    auto lastmei = (*it)->rbegin();
    bool fallthrough = true;
    if (lastmei->type == MFLOW_OPCODE) {
      auto lastop = lastmei->insn->opcode();
      if (is_branch(lastop)) {
        fallthrough = !is_goto(lastop);
        auto const& targets = branch_to_targets[&*lastmei];
        for (auto target : targets) {
          m_cfg->add_edge(
              *it, target, is_goto(lastop) ? EDGE_GOTO : EDGE_BRANCH);
        }
      } else if (is_return(lastop) || lastop == OPCODE_THROW) {
        fallthrough = false;
      }
    }
    if (fallthrough && std::next(it) != blocks.end()) {
      Block* next = *std::next(it);
      m_cfg->add_edge(*it, next, EDGE_GOTO);
    }
  }
  /*
   * Now add the catch edges.  Every block inside a try-start/try-end region
   * gets an edge to every catch block.  This simplifies dataflow analysis
   * since you can always get the exception state by looking at successors,
   * without any additional analysis.
   *
   * NB: This algorithm assumes that a try-start/try-end region will consist of
   * sequentially-numbered blocks, which is guaranteed because catch regions
   * are contiguous in the bytecode, and we generate blocks in bytecode order.
   */
  for (auto tep : try_ends) {
    auto try_end = tep.first;
    auto tryendblock = tep.second;
    size_t bid = tryendblock->id();
    always_assert(bid > 0);
    --bid;
    while (true) {
      auto block = blocks[bid];
      if (ends_with_may_throw(block, end_block_before_throw)) {
        for (auto mei = try_end->catch_start;
             mei != nullptr;
             mei = mei->centry->next) {
          auto catchblock = try_catches.at(mei->centry);
          m_cfg->add_edge(block, catchblock, EDGE_THROW);
        }
      }
      auto begin = block->begin();
      if (begin->type == MFLOW_TRY) {
        auto tentry = begin->tentry;
        if (tentry->type == TRY_START) {
          always_assert(tentry->catch_start == try_end->catch_start);
          break;
        }
      }
      always_assert_log(bid > 0, "No beginning of try region found");
      --bid;
    }
  }
  TRACE(CFG, 5, "%s", SHOW(*m_cfg));
}

std::unique_ptr<DexCode> IRCode::sync(const DexMethod*) {
  // TODO: when we have load-param opcodes, check that they square with the
  // prototype of the DexMethod
  auto dex_code = std::make_unique<DexCode>();
  dex_code->set_registers_size(m_registers_size);
  dex_code->set_ins_size(m_ins_size);
  dex_code->set_outs_size(m_outs_size);
  dex_code->set_debug_item(std::move(m_dbg));
  while (try_sync(&*dex_code) == false)
    ;
  return dex_code;
}

bool IRCode::try_sync(DexCode* code) {
  uint32_t addr = 0;
  addr_mei_t addr_to_mei;
  // Step 1, regenerate opcode list for the method, and
  // and calculate the opcode entries address offsets.
  TRACE(MTRANS, 5, "Emitting opcodes\n");
  for (auto miter = m_fmethod->begin(); miter != m_fmethod->end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    TRACE(MTRANS, 5, "Analyzing mentry %p\n", mentry);
    mentry->addr = addr;
    if (mentry->type == MFLOW_OPCODE) {
      addr_to_mei[addr] = mentry;
      TRACE(MTRANS, 5, "Emitting mentry %p at %08x\n", mentry, addr);
      addr += mentry->insn->size();
    }
  }
  // Step 2, recalculate branches..., save off multi-branch data.
  TRACE(MTRANS, 5, "Recalculating branches\n");
  std::vector<MethodItemEntry*> multi_branches;
  std::unordered_map<MethodItemEntry*, std::vector<BranchTarget*>> multis;
  std::unordered_map<BranchTarget*, uint32_t> multi_targets;
  bool needs_resync = false;
  for (auto miter = m_fmethod->begin(); miter != m_fmethod->end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    if (mentry->type == MFLOW_OPCODE) {
      auto opcode = mentry->insn->opcode();
      if (is_branch(opcode) && is_multi_branch(opcode)) {
        multi_branches.push_back(mentry);
      }
    }
    if (mentry->type == MFLOW_TARGET) {
      BranchTarget* bt = mentry->target;
      if (bt->type == BRANCH_MULTI) {
        multis[bt->src].push_back(bt);
        multi_targets[bt] = mentry->addr;
        // We can't fix the primary switch opcodes address until we emit
        // the fopcode, which comes later.
      } else if (bt->type == BRANCH_SIMPLE) {
        MethodItemEntry* branch_op_mie = bt->src;
        int32_t branchoffset = mentry->addr - branch_op_mie->addr;
        needs_resync |= !encode_offset(m_fmethod, branch_op_mie, branchoffset);
      }
    }
  }
  if (needs_resync) {
    return false;
  }
  auto& opout = code->reset_instructions();
  std::unordered_map<IRInstruction*, DexInstruction*> ir_to_dex_insn;
  for (auto& mie : InstructionIterable(this)) {
    TRACE(MTRANS, 6, "Emitting insn %s\n", SHOW(mie.insn));
    auto dex_insn = mie.insn->to_dex_instruction();
    opout.push_back(dex_insn);
    ir_to_dex_insn.emplace(mie.insn, dex_insn);
  }
  TRACE(MTRANS, 5, "Emitting multi-branches\n");
  // Step 3, generate multi-branch fopcodes
  for (auto multiopcode : multi_branches) {
    auto& targets = multis[multiopcode];
    auto multi_insn = ir_to_dex_insn.at(multiopcode->insn);
    std::sort(targets.begin(), targets.end(), multi_target_compare_index);
    if (multi_contains_gaps(targets)) {
      // Emit sparse.
      unsigned long count = (targets.size() * 4) + 2;
      uint16_t sparse_payload[count];
      sparse_payload[0] = FOPCODE_SPARSE_SWITCH;
      sparse_payload[1] = targets.size();
      uint32_t* spkeys = (uint32_t*)&sparse_payload[2];
      uint32_t* sptargets =
          (uint32_t*)&sparse_payload[2 + (targets.size() * 2)];
      for (BranchTarget* target : targets) {
        *spkeys++ = target->index;
        *sptargets++ = multi_targets[target] - multiopcode->addr;
      }
      // Emit align nop
      if (addr & 1) {
        DexInstruction* nop = new DexInstruction(0);
        opout.push_back(nop);
        addr++;
      }
      // Insert the new fopcode...
      DexInstruction* fop = new DexOpcodeData(sparse_payload, (int)(count - 1));
      opout.push_back(fop);
      // re-write the source opcode with the address of the
      // fopcode, increment the address of the fopcode.
      multi_insn->set_offset(addr - multiopcode->addr);
      multi_insn->set_opcode(OPCODE_SPARSE_SWITCH);
      addr += count;
    } else {
      // Emit packed.
      unsigned long count = (targets.size() * 2) + 4;
      uint16_t packed_payload[count];
      packed_payload[0] = FOPCODE_PACKED_SWITCH;
      packed_payload[1] = targets.size();
      uint32_t* psdata = (uint32_t*)&packed_payload[2];
      *psdata++ = targets.front()->index;
      for (BranchTarget* target : targets) {
        *psdata++ = multi_targets[target] - multiopcode->addr;
      }
      // Emit align nop
      if (addr & 1) {
        DexInstruction* nop = new DexInstruction(0);
        opout.push_back(nop);
        addr++;
      }
      // Insert the new fopcode...
      DexInstruction* fop = new DexOpcodeData(packed_payload, (int) (count - 1));
      opout.push_back(fop);
      // re-write the source opcode with the address of the
      // fopcode, increment the address of the fopcode.
      multi_insn->set_offset(addr - multiopcode->addr);
      multi_insn->set_opcode(OPCODE_PACKED_SWITCH);
      addr += count;
    }
  }

  TRACE(MTRANS, 5, "Emitting filled array data\n");
  for (auto miter = m_fmethod->begin(); miter != m_fmethod->end(); miter++) {
    MethodItemEntry* mentry = &*miter;
    if (mentry->type != MFLOW_OPCODE) {
      continue;
    }
    auto insn = ir_to_dex_insn.at(mentry->insn);
    if (insn->opcode() == OPCODE_FILL_ARRAY_DATA) {
      if (addr & 1) {
        opout.push_back(new DexInstruction(OPCODE_NOP));
        ++addr;
      }
      insn->set_offset(addr - mentry->addr);
      auto fopcode = m_array_data.at(mentry->insn);
      opout.push_back(fopcode);
      addr += fopcode->size();
    }
  }

  // Step 4, emit debug opcodes
  TRACE(MTRANS, 5, "Emitting debug opcodes\n");
  auto debugitem = code->get_debug_item();
  if (debugitem) {
    auto& entries = debugitem->get_entries();
    for (auto& mentry : *m_fmethod) {
      if (mentry.type == MFLOW_DEBUG) {
        entries.emplace_back(mentry.addr, std::move(mentry.dbgop));
      } else if (mentry.type == MFLOW_POSITION) {
        entries.emplace_back(mentry.addr, std::move(mentry.pos));
      }
    }
  }
  // Step 5, try/catch blocks
  TRACE(MTRANS, 5, "Emitting try items & catch handlers\n");
  auto& tries = code->get_tries();
  tries.clear();
  MethodItemEntry* active_try = nullptr;
  for (auto& mentry : *m_fmethod) {
    if (mentry.type != MFLOW_TRY) {
      continue;
    }
    auto& tentry = mentry.tentry;
    if (tentry->type == TRY_START) {
      always_assert(active_try == nullptr);
      active_try = &mentry;
      continue;
    }
    assert(tentry->type == TRY_END);
    auto try_end = &mentry;
    auto try_start = active_try;
    active_try = nullptr;

    always_assert(try_end->tentry->catch_start ==
                  try_start->tentry->catch_start);
    auto insn_count = try_end->addr - try_start->addr;
    if (insn_count == 0) {
      continue;
    }
    auto try_item = new DexTryItem(try_start->addr, insn_count);
    for (auto mei = try_end->tentry->catch_start;
        mei != nullptr;
        mei = mei->centry->next) {
      try_item->m_catches.emplace_back(mei->centry->catch_type, mei->addr);
    }
    tries.emplace_back(try_item);
  }
  always_assert_log(active_try == nullptr, "unclosed try_start found");

  std::sort(tries.begin(),
            tries.end(),
            [](const std::unique_ptr<DexTryItem>& a,
               const std::unique_ptr<DexTryItem>& b) {
              return a->m_start_addr < b->m_start_addr;
            });
  return true;
}
