/*
 * Copyright (c) 1997, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "jvm.h"
#include "asm/macroAssembler.inline.hpp"
#include "compiler/disassembler.hpp"
#include "gc/shared/collectedHeap.inline.hpp"
#include "gc/shared/barrierSet.hpp"
#include "gc/shared/barrierSetAssembler.hpp"
#include "interpreter/interpreter.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/accessDecorators.hpp"
#include "oops/compressedOops.hpp"
#include "oops/klass.inline.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/biasedLocking.hpp"
#include "runtime/flags/flagSetting.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/jniHandles.inline.hpp"
#include "runtime/objectMonitor.hpp"
#include "runtime/os.inline.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/safepointMechanism.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "utilities/align.hpp"
#include "utilities/macros.hpp"
#ifdef COMPILER2
#include "opto/intrinsicnode.hpp"
#endif

#ifdef PRODUCT
#define BLOCK_COMMENT(str) /* nothing */
#define STOP(error) stop(error)
#else
#define BLOCK_COMMENT(str) block_comment(str)
#define STOP(error) block_comment(error); stop(error)
#endif

// Convert the raw encoding form into the form expected by the
// constructor for Address.
Address Address::make_raw(int base, int index, int scale, int disp, relocInfo::relocType disp_reloc) {
  assert(scale == 0, "not supported");
  RelocationHolder rspec;
  if (disp_reloc != relocInfo::none) {
    rspec = Relocation::spec_simple(disp_reloc);
  }

  Register rindex = as_Register(index);
  if (rindex != G0) {
    Address madr(as_Register(base), rindex);
    madr._rspec = rspec;
    return madr;
  } else {
    Address madr(as_Register(base), disp);
    madr._rspec = rspec;
    return madr;
  }
}

Address Argument::address_in_frame() const {
  // Warning: In LP64 mode disp will occupy more than 10 bits, but
  //          op codes such as ld or ldx, only access disp() to get
  //          their simm13 argument.
  int disp = ((_number - Argument::n_register_parameters + frame::memory_parameter_word_sp_offset) * BytesPerWord) + STACK_BIAS;
  if (is_in())
    return Address(FP, disp); // In argument.
  else
    return Address(SP, disp); // Out argument.
}

static const char* argumentNames[][2] = {
  {"A0","P0"}, {"A1","P1"}, {"A2","P2"}, {"A3","P3"}, {"A4","P4"},
  {"A5","P5"}, {"A6","P6"}, {"A7","P7"}, {"A8","P8"}, {"A9","P9"},
  {"A(n>9)","P(n>9)"}
};

const char* Argument::name() const {
  int nofArgs = sizeof argumentNames / sizeof argumentNames[0];
  int num = number();
  if (num >= nofArgs)  num = nofArgs - 1;
  return argumentNames[num][is_in() ? 1 : 0];
}

#ifdef ASSERT
// On RISC, there's no benefit to verifying instruction boundaries.
bool AbstractAssembler::pd_check_instruction_mark() { return false; }
#endif

// Patch instruction inst at offset inst_pos to refer to dest_pos
// and return the resulting instruction.
// We should have pcs, not offsets, but since all is relative, it will work out
// OK.
int MacroAssembler::patched_branch(int dest_pos, int inst, int inst_pos) {
  int m; // mask for displacement field
  int v; // new value for displacement field
  const int word_aligned_ones = -4;
  switch (inv_op(inst)) {
  default: ShouldNotReachHere();
  case call_op:    m = wdisp(word_aligned_ones, 0, 30);  v = wdisp(dest_pos, inst_pos, 30); break;
  case branch_op:
    switch (inv_op2(inst)) {
      case fbp_op2:    m = wdisp(  word_aligned_ones, 0, 19);  v = wdisp(  dest_pos, inst_pos, 19); break;
      case bp_op2:     m = wdisp(  word_aligned_ones, 0, 19);  v = wdisp(  dest_pos, inst_pos, 19); break;
      case fb_op2:     m = wdisp(  word_aligned_ones, 0, 22);  v = wdisp(  dest_pos, inst_pos, 22); break;
      case br_op2:     m = wdisp(  word_aligned_ones, 0, 22);  v = wdisp(  dest_pos, inst_pos, 22); break;
      case bpr_op2: {
        if (is_cbcond(inst)) {
          m = wdisp10(word_aligned_ones, 0);
          v = wdisp10(dest_pos, inst_pos);
        } else {
          m = wdisp16(word_aligned_ones, 0);
          v = wdisp16(dest_pos, inst_pos);
        }
        break;
      }
      default: ShouldNotReachHere();
    }
  }
  return  inst & ~m  |  v;
}

// Return the offset of the branch destionation of instruction inst
// at offset pos.
// Should have pcs, but since all is relative, it works out.
int MacroAssembler::branch_destination(int inst, int pos) {
  int r;
  switch (inv_op(inst)) {
  default: ShouldNotReachHere();
  case call_op:        r = inv_wdisp(inst, pos, 30);  break;
  case branch_op:
    switch (inv_op2(inst)) {
      case fbp_op2:    r = inv_wdisp(  inst, pos, 19);  break;
      case bp_op2:     r = inv_wdisp(  inst, pos, 19);  break;
      case fb_op2:     r = inv_wdisp(  inst, pos, 22);  break;
      case br_op2:     r = inv_wdisp(  inst, pos, 22);  break;
      case bpr_op2: {
        if (is_cbcond(inst)) {
          r = inv_wdisp10(inst, pos);
        } else {
          r = inv_wdisp16(inst, pos);
        }
        break;
      }
      default: ShouldNotReachHere();
    }
  }
  return r;
}

void MacroAssembler::resolve_jobject(Register value, Register tmp) {
  Label done, not_weak;
  br_null(value, false, Assembler::pn, done); // Use NULL as-is.
  delayed()->andcc(value, JNIHandles::weak_tag_mask, G0); // Test for jweak
  brx(Assembler::zero, true, Assembler::pt, not_weak);
  delayed()->nop();
  access_load_at(T_OBJECT, IN_NATIVE | ON_PHANTOM_OOP_REF,
                 Address(value, -JNIHandles::weak_tag_value), value, tmp);
  verify_oop(value);
  br (Assembler::always, true, Assembler::pt, done);
  delayed()->nop();
  bind(not_weak);
  access_load_at(T_OBJECT, IN_NATIVE, Address(value, 0), value, tmp);
  verify_oop(value);
  bind(done);
}

void MacroAssembler::null_check(Register reg, int offset) {
  if (needs_explicit_null_check((intptr_t)offset)) {
    // provoke OS NULL exception if reg = NULL by
    // accessing M[reg] w/o changing any registers
    ld_ptr(reg, 0, G0);
  }
  else {
    // nothing to do, (later) access of M[reg + offset]
    // will provoke OS NULL exception if reg = NULL
  }
}

// Ring buffer jumps


void MacroAssembler::jmp2(Register r1, Register r2, const char* file, int line ) {
  assert_not_delayed();
  jmpl(r1, r2, G0);
}
void MacroAssembler::jmp(Register r1, int offset, const char* file, int line ) {
  assert_not_delayed();
  jmp(r1, offset);
}

// This code sequence is relocatable to any address, even on LP64.
void MacroAssembler::jumpl(const AddressLiteral& addrlit, Register temp, Register d, int offset, const char* file, int line) {
  assert_not_delayed();
  // Force fixed length sethi because NativeJump and NativeFarCall don't handle
  // variable length instruction streams.
  patchable_sethi(addrlit, temp);
  Address a(temp, addrlit.low10() + offset);  // Add the offset to the displacement.
  jmpl(a.base(), a.disp(), d);
}

void MacroAssembler::jump(const AddressLiteral& addrlit, Register temp, int offset, const char* file, int line) {
  jumpl(addrlit, temp, G0, offset, file, line);
}


// Conditional breakpoint (for assertion checks in assembly code)
void MacroAssembler::breakpoint_trap(Condition c, CC cc) {
  trap(c, cc, G0, ST_RESERVED_FOR_USER_0);
}

// We want to use ST_BREAKPOINT here, but the debugger is confused by it.
void MacroAssembler::breakpoint_trap() {
  trap(ST_RESERVED_FOR_USER_0);
}

void MacroAssembler::safepoint_poll(Label& slow_path, bool a, Register thread_reg, Register temp_reg) {
  if (SafepointMechanism::uses_thread_local_poll()) {
    ldx(Address(thread_reg, Thread::polling_page_offset()), temp_reg, 0);
    // Armed page has poll bit set.
    and3(temp_reg, SafepointMechanism::poll_bit(), temp_reg);
    br_notnull(temp_reg, a, Assembler::pn, slow_path);
  } else {
    AddressLiteral sync_state(SafepointSynchronize::address_of_state());

    load_contents(sync_state, temp_reg);
    cmp(temp_reg, SafepointSynchronize::_not_synchronized);
    br(Assembler::notEqual, a, Assembler::pn, slow_path);
  }
}

void MacroAssembler::enter() {
  Unimplemented();
}

void MacroAssembler::leave() {
  Unimplemented();
}

// Calls to C land

#ifdef ASSERT
// a hook for debugging
static Thread* reinitialize_thread() {
  return Thread::current();
}
#else
#define reinitialize_thread Thread::current
#endif

#ifdef ASSERT
address last_get_thread = NULL;
#endif

// call this when G2_thread is not known to be valid
void MacroAssembler::get_thread() {
  save_frame(0);                // to avoid clobbering O0
  mov(G1, L0);                  // avoid clobbering G1
  mov(G5_method, L1);           // avoid clobbering G5
  mov(G3, L2);                  // avoid clobbering G3 also
  mov(G4, L5);                  // avoid clobbering G4
#ifdef ASSERT
  AddressLiteral last_get_thread_addrlit(&last_get_thread);
  set(last_get_thread_addrlit, L3);
  rdpc(L4);
  inc(L4, 3 * BytesPerInstWord); // skip rdpc + inc + st_ptr to point L4 at call  st_ptr(L4, L3, 0);
#endif
  call(CAST_FROM_FN_PTR(address, reinitialize_thread), relocInfo::runtime_call_type);
  delayed()->nop();
  mov(L0, G1);
  mov(L1, G5_method);
  mov(L2, G3);
  mov(L5, G4);
  restore(O0, 0, G2_thread);
}

static Thread* verify_thread_subroutine(Thread* gthread_value) {
  Thread* correct_value = Thread::current();
  guarantee(gthread_value == correct_value, "G2_thread value must be the thread");
  return correct_value;
}

void MacroAssembler::verify_thread() {
  if (VerifyThread) {
    // NOTE: this chops off the heads of the 64-bit O registers.
    // make sure G2_thread contains the right value
    save_frame_and_mov(0, Lmethod, Lmethod);   // to avoid clobbering O0 (and propagate Lmethod)
    mov(G1, L1);                // avoid clobbering G1
    // G2 saved below
    mov(G3, L3);                // avoid clobbering G3
    mov(G4, L4);                // avoid clobbering G4
    mov(G5_method, L5);         // avoid clobbering G5_method
    call(CAST_FROM_FN_PTR(address,verify_thread_subroutine), relocInfo::runtime_call_type);
    delayed()->mov(G2_thread, O0);

    mov(L1, G1);                // Restore G1
    // G2 restored below
    mov(L3, G3);                // restore G3
    mov(L4, G4);                // restore G4
    mov(L5, G5_method);         // restore G5_method
    restore(O0, 0, G2_thread);
  }
}


void MacroAssembler::save_thread(const Register thread_cache) {
  verify_thread();
  if (thread_cache->is_valid()) {
    assert(thread_cache->is_local() || thread_cache->is_in(), "bad volatile");
    mov(G2_thread, thread_cache);
  }
  if (VerifyThread) {
    // smash G2_thread, as if the VM were about to anyway
    set(0x67676767, G2_thread);
  }
}


void MacroAssembler::restore_thread(const Register thread_cache) {
  if (thread_cache->is_valid()) {
    assert(thread_cache->is_local() || thread_cache->is_in(), "bad volatile");
    mov(thread_cache, G2_thread);
    verify_thread();
  } else {
    // do it the slow way
    get_thread();
  }
}


// %%% maybe get rid of [re]set_last_Java_frame
void MacroAssembler::set_last_Java_frame(Register last_java_sp, Register last_Java_pc) {
  assert_not_delayed();
  Address flags(G2_thread, JavaThread::frame_anchor_offset() +
                           JavaFrameAnchor::flags_offset());
  Address pc_addr(G2_thread, JavaThread::last_Java_pc_offset());

  // Always set last_Java_pc and flags first because once last_Java_sp is visible
  // has_last_Java_frame is true and users will look at the rest of the fields.
  // (Note: flags should always be zero before we get here so doesn't need to be set.)

#ifdef ASSERT
  // Verify that flags was zeroed on return to Java
  Label PcOk;
  save_frame(0);                // to avoid clobbering O0
  ld_ptr(pc_addr, L0);
  br_null_short(L0, Assembler::pt, PcOk);
  STOP("last_Java_pc not zeroed before leaving Java");
  bind(PcOk);

  // Verify that flags was zeroed on return to Java
  Label FlagsOk;
  ld(flags, L0);
  tst(L0);
  br(Assembler::zero, false, Assembler::pt, FlagsOk);
  delayed() -> restore();
  STOP("flags not zeroed before leaving Java");
  bind(FlagsOk);
#endif /* ASSERT */
  //
  // When returning from calling out from Java mode the frame anchor's last_Java_pc
  // will always be set to NULL. It is set here so that if we are doing a call to
  // native (not VM) that we capture the known pc and don't have to rely on the
  // native call having a standard frame linkage where we can find the pc.

  if (last_Java_pc->is_valid()) {
    st_ptr(last_Java_pc, pc_addr);
  }

#ifdef ASSERT
  // Make sure that we have an odd stack
  Label StackOk;
  andcc(last_java_sp, 0x01, G0);
  br(Assembler::notZero, false, Assembler::pt, StackOk);
  delayed()->nop();
  STOP("Stack Not Biased in set_last_Java_frame");
  bind(StackOk);
#endif // ASSERT
  assert( last_java_sp != G4_scratch, "bad register usage in set_last_Java_frame");
  add( last_java_sp, STACK_BIAS, G4_scratch );
  st_ptr(G4_scratch, G2_thread, JavaThread::last_Java_sp_offset());
}

void MacroAssembler::reset_last_Java_frame(void) {
  assert_not_delayed();

  Address sp_addr(G2_thread, JavaThread::last_Java_sp_offset());
  Address pc_addr(G2_thread, JavaThread::frame_anchor_offset() + JavaFrameAnchor::last_Java_pc_offset());
  Address flags  (G2_thread, JavaThread::frame_anchor_offset() + JavaFrameAnchor::flags_offset());

#ifdef ASSERT
  // check that it WAS previously set
    save_frame_and_mov(0, Lmethod, Lmethod);     // Propagate Lmethod to helper frame
    ld_ptr(sp_addr, L0);
    tst(L0);
    breakpoint_trap(Assembler::zero, Assembler::ptr_cc);
    restore();
#endif // ASSERT

  st_ptr(G0, sp_addr);
  // Always return last_Java_pc to zero
  st_ptr(G0, pc_addr);
  // Always null flags after return to Java
  st(G0, flags);
}


void MacroAssembler::call_VM_base(
  Register        oop_result,
  Register        thread_cache,
  Register        last_java_sp,
  address         entry_point,
  int             number_of_arguments,
  bool            check_exceptions)
{
  assert_not_delayed();

  // determine last_java_sp register
  if (!last_java_sp->is_valid()) {
    last_java_sp = SP;
  }
  // debugging support
  assert(number_of_arguments >= 0   , "cannot have negative number of arguments");

  // 64-bit last_java_sp is biased!
  set_last_Java_frame(last_java_sp, noreg);
  if (VerifyThread)  mov(G2_thread, O0); // about to be smashed; pass early
  save_thread(thread_cache);
  // do the call
  call(entry_point, relocInfo::runtime_call_type);
  if (!VerifyThread)
    delayed()->mov(G2_thread, O0);  // pass thread as first argument
  else
    delayed()->nop();             // (thread already passed)
  restore_thread(thread_cache);
  reset_last_Java_frame();

  // check for pending exceptions. use Gtemp as scratch register.
  if (check_exceptions) {
    check_and_forward_exception(Gtemp);
  }

#ifdef ASSERT
  set(badHeapWordVal, G3);
  set(badHeapWordVal, G4);
  set(badHeapWordVal, G5);
#endif

  // get oop result if there is one and reset the value in the thread
  if (oop_result->is_valid()) {
    get_vm_result(oop_result);
  }
}

void MacroAssembler::check_and_forward_exception(Register scratch_reg)
{
  Label L;

  check_and_handle_popframe(scratch_reg);
  check_and_handle_earlyret(scratch_reg);

  Address exception_addr(G2_thread, Thread::pending_exception_offset());
  ld_ptr(exception_addr, scratch_reg);
  br_null_short(scratch_reg, pt, L);
  // we use O7 linkage so that forward_exception_entry has the issuing PC
  call(StubRoutines::forward_exception_entry(), relocInfo::runtime_call_type);
  delayed()->nop();
  bind(L);
}


void MacroAssembler::check_and_handle_popframe(Register scratch_reg) {
}


void MacroAssembler::check_and_handle_earlyret(Register scratch_reg) {
}


void MacroAssembler::call_VM(Register oop_result, address entry_point, int number_of_arguments, bool check_exceptions) {
  call_VM_base(oop_result, noreg, noreg, entry_point, number_of_arguments, check_exceptions);
}


void MacroAssembler::call_VM(Register oop_result, address entry_point, Register arg_1, bool check_exceptions) {
  // O0 is reserved for the thread
  mov(arg_1, O1);
  call_VM(oop_result, entry_point, 1, check_exceptions);
}


void MacroAssembler::call_VM(Register oop_result, address entry_point, Register arg_1, Register arg_2, bool check_exceptions) {
  // O0 is reserved for the thread
  mov(arg_1, O1);
  mov(arg_2, O2); assert(arg_2 != O1, "smashed argument");
  call_VM(oop_result, entry_point, 2, check_exceptions);
}


void MacroAssembler::call_VM(Register oop_result, address entry_point, Register arg_1, Register arg_2, Register arg_3, bool check_exceptions) {
  // O0 is reserved for the thread
  mov(arg_1, O1);
  mov(arg_2, O2); assert(arg_2 != O1,                "smashed argument");
  mov(arg_3, O3); assert(arg_3 != O1 && arg_3 != O2, "smashed argument");
  call_VM(oop_result, entry_point, 3, check_exceptions);
}



// Note: The following call_VM overloadings are useful when a "save"
// has already been performed by a stub, and the last Java frame is
// the previous one.  In that case, last_java_sp must be passed as FP
// instead of SP.


void MacroAssembler::call_VM(Register oop_result, Register last_java_sp, address entry_point, int number_of_arguments, bool check_exceptions) {
  call_VM_base(oop_result, noreg, last_java_sp, entry_point, number_of_arguments, check_exceptions);
}


void MacroAssembler::call_VM(Register oop_result, Register last_java_sp, address entry_point, Register arg_1, bool check_exceptions) {
  // O0 is reserved for the thread
  mov(arg_1, O1);
  call_VM(oop_result, last_java_sp, entry_point, 1, check_exceptions);
}


void MacroAssembler::call_VM(Register oop_result, Register last_java_sp, address entry_point, Register arg_1, Register arg_2, bool check_exceptions) {
  // O0 is reserved for the thread
  mov(arg_1, O1);
  mov(arg_2, O2); assert(arg_2 != O1, "smashed argument");
  call_VM(oop_result, last_java_sp, entry_point, 2, check_exceptions);
}


void MacroAssembler::call_VM(Register oop_result, Register last_java_sp, address entry_point, Register arg_1, Register arg_2, Register arg_3, bool check_exceptions) {
  // O0 is reserved for the thread
  mov(arg_1, O1);
  mov(arg_2, O2); assert(arg_2 != O1,                "smashed argument");
  mov(arg_3, O3); assert(arg_3 != O1 && arg_3 != O2, "smashed argument");
  call_VM(oop_result, last_java_sp, entry_point, 3, check_exceptions);
}



void MacroAssembler::call_VM_leaf_base(Register thread_cache, address entry_point, int number_of_arguments) {
  assert_not_delayed();
  save_thread(thread_cache);
  // do the call
  call(entry_point, relocInfo::runtime_call_type);
  delayed()->nop();
  restore_thread(thread_cache);
#ifdef ASSERT
  set(badHeapWordVal, G3);
  set(badHeapWordVal, G4);
  set(badHeapWordVal, G5);
#endif
}


void MacroAssembler::call_VM_leaf(Register thread_cache, address entry_point, int number_of_arguments) {
  call_VM_leaf_base(thread_cache, entry_point, number_of_arguments);
}


void MacroAssembler::call_VM_leaf(Register thread_cache, address entry_point, Register arg_1) {
  mov(arg_1, O0);
  call_VM_leaf(thread_cache, entry_point, 1);
}


void MacroAssembler::call_VM_leaf(Register thread_cache, address entry_point, Register arg_1, Register arg_2) {
  mov(arg_1, O0);
  mov(arg_2, O1); assert(arg_2 != O0, "smashed argument");
  call_VM_leaf(thread_cache, entry_point, 2);
}


void MacroAssembler::call_VM_leaf(Register thread_cache, address entry_point, Register arg_1, Register arg_2, Register arg_3) {
  mov(arg_1, O0);
  mov(arg_2, O1); assert(arg_2 != O0,                "smashed argument");
  mov(arg_3, O2); assert(arg_3 != O0 && arg_3 != O1, "smashed argument");
  call_VM_leaf(thread_cache, entry_point, 3);
}


void MacroAssembler::get_vm_result(Register oop_result) {
  verify_thread();
  Address vm_result_addr(G2_thread, JavaThread::vm_result_offset());
  ld_ptr(    vm_result_addr, oop_result);
  st_ptr(G0, vm_result_addr);
  verify_oop(oop_result);
}


void MacroAssembler::get_vm_result_2(Register metadata_result) {
  verify_thread();
  Address vm_result_addr_2(G2_thread, JavaThread::vm_result_2_offset());
  ld_ptr(vm_result_addr_2, metadata_result);
  st_ptr(G0, vm_result_addr_2);
}


// We require that C code which does not return a value in vm_result will
// leave it undisturbed.
void MacroAssembler::set_vm_result(Register oop_result) {
  verify_thread();
  Address vm_result_addr(G2_thread, JavaThread::vm_result_offset());
  verify_oop(oop_result);

# ifdef ASSERT
    // Check that we are not overwriting any other oop.
    save_frame_and_mov(0, Lmethod, Lmethod);     // Propagate Lmethod
    ld_ptr(vm_result_addr, L0);
    tst(L0);
    restore();
    breakpoint_trap(notZero, Assembler::ptr_cc);
    // }
# endif

  st_ptr(oop_result, vm_result_addr);
}


void MacroAssembler::ic_call(address entry, bool emit_delay, jint method_index) {
  RelocationHolder rspec = virtual_call_Relocation::spec(pc(), method_index);
  patchable_set((intptr_t)Universe::non_oop_word(), G5_inline_cache_reg);
  relocate(rspec);
  call(entry, relocInfo::none);
  if (emit_delay) {
    delayed()->nop();
  }
}


void MacroAssembler::internal_sethi(const AddressLiteral& addrlit, Register d, bool ForceRelocatable) {
  address save_pc;
  int shiftcnt;
#ifdef VALIDATE_PIPELINE
  assert_no_delay("Cannot put two instructions in delay-slot.");
#endif
  v9_dep();
  save_pc = pc();

  int msb32 = (int) (addrlit.value() >> 32);
  int lsb32 = (int) (addrlit.value());

  if (msb32 == 0 && lsb32 >= 0) {
    Assembler::sethi(lsb32, d, addrlit.rspec());
  }
  else if (msb32 == -1) {
    Assembler::sethi(~lsb32, d, addrlit.rspec());
    xor3(d, ~low10(~0), d);
  }
  else {
    Assembler::sethi(msb32, d, addrlit.rspec());  // msb 22-bits
    if (msb32 & 0x3ff)                            // Any bits?
      or3(d, msb32 & 0x3ff, d);                   // msb 32-bits are now in lsb 32
    if (lsb32 & 0xFFFFFC00) {                     // done?
      if ((lsb32 >> 20) & 0xfff) {                // Any bits set?
        sllx(d, 12, d);                           // Make room for next 12 bits
        or3(d, (lsb32 >> 20) & 0xfff, d);         // Or in next 12
        shiftcnt = 0;                             // We already shifted
      }
      else
        shiftcnt = 12;
      if ((lsb32 >> 10) & 0x3ff) {
        sllx(d, shiftcnt + 10, d);                // Make room for last 10 bits
        or3(d, (lsb32 >> 10) & 0x3ff, d);         // Or in next 10
        shiftcnt = 0;
      }
      else
        shiftcnt = 10;
      sllx(d, shiftcnt + 10, d);                  // Shift leaving disp field 0'd
    }
    else
      sllx(d, 32, d);
  }
  // Pad out the instruction sequence so it can be patched later.
  if (ForceRelocatable || (addrlit.rtype() != relocInfo::none &&
                           addrlit.rtype() != relocInfo::runtime_call_type)) {
    while (pc() < (save_pc + (7 * BytesPerInstWord)))
      nop();
  }
}


void MacroAssembler::sethi(const AddressLiteral& addrlit, Register d) {
  internal_sethi(addrlit, d, false);
}


void MacroAssembler::patchable_sethi(const AddressLiteral& addrlit, Register d) {
  internal_sethi(addrlit, d, true);
}


int MacroAssembler::insts_for_sethi(address a, bool worst_case) {
  if (worst_case)  return 7;
  intptr_t iaddr = (intptr_t) a;
  int msb32 = (int) (iaddr >> 32);
  int lsb32 = (int) (iaddr);
  int count;
  if (msb32 == 0 && lsb32 >= 0)
    count = 1;
  else if (msb32 == -1)
    count = 2;
  else {
    count = 2;
    if (msb32 & 0x3ff)
      count++;
    if (lsb32 & 0xFFFFFC00 ) {
      if ((lsb32 >> 20) & 0xfff)  count += 2;
      if ((lsb32 >> 10) & 0x3ff)  count += 2;
    }
  }
  return count;
}

int MacroAssembler::worst_case_insts_for_set() {
  return insts_for_sethi(NULL, true) + 1;
}


// Keep in sync with MacroAssembler::insts_for_internal_set
void MacroAssembler::internal_set(const AddressLiteral& addrlit, Register d, bool ForceRelocatable) {
  intptr_t value = addrlit.value();

  if (!ForceRelocatable && addrlit.rspec().type() == relocInfo::none) {
    // can optimize
    if (-4096 <= value && value <= 4095) {
      or3(G0, value, d); // setsw (this leaves upper 32 bits sign-extended)
      return;
    }
    if (inv_hi22(hi22(value)) == value) {
      sethi(addrlit, d);
      return;
    }
  }
  assert_no_delay("Cannot put two instructions in delay-slot.");
  internal_sethi(addrlit, d, ForceRelocatable);
  if (ForceRelocatable || addrlit.rspec().type() != relocInfo::none || addrlit.low10() != 0) {
    add(d, addrlit.low10(), d, addrlit.rspec());
  }
}

// Keep in sync with MacroAssembler::internal_set
int MacroAssembler::insts_for_internal_set(intptr_t value) {
  // can optimize
  if (-4096 <= value && value <= 4095) {
    return 1;
  }
  if (inv_hi22(hi22(value)) == value) {
    return insts_for_sethi((address) value);
  }
  int count = insts_for_sethi((address) value);
  AddressLiteral al(value);
  if (al.low10() != 0) {
    count++;
  }
  return count;
}

void MacroAssembler::set(const AddressLiteral& al, Register d) {
  internal_set(al, d, false);
}

void MacroAssembler::set(intptr_t value, Register d) {
  AddressLiteral al(value);
  internal_set(al, d, false);
}

void MacroAssembler::set(address addr, Register d, RelocationHolder const& rspec) {
  AddressLiteral al(addr, rspec);
  internal_set(al, d, false);
}

void MacroAssembler::patchable_set(const AddressLiteral& al, Register d) {
  internal_set(al, d, true);
}

void MacroAssembler::patchable_set(intptr_t value, Register d) {
  AddressLiteral al(value);
  internal_set(al, d, true);
}


void MacroAssembler::set64(jlong value, Register d, Register tmp) {
  assert_not_delayed();
  v9_dep();

  int hi = (int)(value >> 32);
  int lo = (int)(value & ~0);
  int bits_33to2 = (int)((value >> 2) & ~0);
  // (Matcher::isSimpleConstant64 knows about the following optimizations.)
  if (Assembler::is_simm13(lo) && value == lo) {
    or3(G0, lo, d);
  } else if (hi == 0) {
    Assembler::sethi(lo, d);   // hardware version zero-extends to upper 32
    if (low10(lo) != 0)
      or3(d, low10(lo), d);
  }
  else if ((hi >> 2) == 0) {
    Assembler::sethi(bits_33to2, d);  // hardware version zero-extends to upper 32
    sllx(d, 2, d);
    if (low12(lo) != 0)
      or3(d, low12(lo), d);
  }
  else if (hi == -1) {
    Assembler::sethi(~lo, d);  // hardware version zero-extends to upper 32
    xor3(d, low10(lo) ^ ~low10(~0), d);
  }
  else if (lo == 0) {
    if (Assembler::is_simm13(hi)) {
      or3(G0, hi, d);
    } else {
      Assembler::sethi(hi, d);   // hardware version zero-extends to upper 32
      if (low10(hi) != 0)
        or3(d, low10(hi), d);
    }
    sllx(d, 32, d);
  }
  else {
    Assembler::sethi(hi, tmp);
    Assembler::sethi(lo,   d); // macro assembler version sign-extends
    if (low10(hi) != 0)
      or3 (tmp, low10(hi), tmp);
    if (low10(lo) != 0)
      or3 (  d, low10(lo),   d);
    sllx(tmp, 32, tmp);
    or3 (d, tmp, d);
  }
}

int MacroAssembler::insts_for_set64(jlong value) {
  v9_dep();

  int hi = (int) (value >> 32);
  int lo = (int) (value & ~0);
  int count = 0;

  // (Matcher::isSimpleConstant64 knows about the following optimizations.)
  if (Assembler::is_simm13(lo) && value == lo) {
    count++;
  } else if (hi == 0) {
    count++;
    if (low10(lo) != 0)
      count++;
  }
  else if (hi == -1) {
    count += 2;
  }
  else if (lo == 0) {
    if (Assembler::is_simm13(hi)) {
      count++;
    } else {
      count++;
      if (low10(hi) != 0)
        count++;
    }
    count++;
  }
  else {
    count += 2;
    if (low10(hi) != 0)
      count++;
    if (low10(lo) != 0)
      count++;
    count += 2;
  }
  return count;
}

// compute size in bytes of sparc frame, given
// number of extraWords
int MacroAssembler::total_frame_size_in_bytes(int extraWords) {

  int nWords = frame::memory_parameter_word_sp_offset;

  nWords += extraWords;

  if (nWords & 1) ++nWords; // round up to double-word

  return nWords * BytesPerWord;
}


// save_frame: given number of "extra" words in frame,
// issue approp. save instruction (p 200, v8 manual)

void MacroAssembler::save_frame(int extraWords) {
  int delta = -total_frame_size_in_bytes(extraWords);
  if (is_simm13(delta)) {
    save(SP, delta, SP);
  } else {
    set(delta, G3_scratch);
    save(SP, G3_scratch, SP);
  }
}


void MacroAssembler::save_frame_c1(int size_in_bytes) {
  if (is_simm13(-size_in_bytes)) {
    save(SP, -size_in_bytes, SP);
  } else {
    set(-size_in_bytes, G3_scratch);
    save(SP, G3_scratch, SP);
  }
}


void MacroAssembler::save_frame_and_mov(int extraWords,
                                        Register s1, Register d1,
                                        Register s2, Register d2) {
  assert_not_delayed();

  // The trick here is to use precisely the same memory word
  // that trap handlers also use to save the register.
  // This word cannot be used for any other purpose, but
  // it works fine to save the register's value, whether or not
  // an interrupt flushes register windows at any given moment!
  Address s1_addr;
  if (s1->is_valid() && (s1->is_in() || s1->is_local())) {
    s1_addr = s1->address_in_saved_window();
    st_ptr(s1, s1_addr);
  }

  Address s2_addr;
  if (s2->is_valid() && (s2->is_in() || s2->is_local())) {
    s2_addr = s2->address_in_saved_window();
    st_ptr(s2, s2_addr);
  }

  save_frame(extraWords);

  if (s1_addr.base() == SP) {
    ld_ptr(s1_addr.after_save(), d1);
  } else if (s1->is_valid()) {
    mov(s1->after_save(), d1);
  }

  if (s2_addr.base() == SP) {
    ld_ptr(s2_addr.after_save(), d2);
  } else if (s2->is_valid()) {
    mov(s2->after_save(), d2);
  }
}


AddressLiteral MacroAssembler::allocate_metadata_address(Metadata* obj) {
  assert(oop_recorder() != NULL, "this assembler needs a Recorder");
  int index = oop_recorder()->allocate_metadata_index(obj);
  RelocationHolder rspec = metadata_Relocation::spec(index);
  return AddressLiteral((address)obj, rspec);
}

AddressLiteral MacroAssembler::constant_metadata_address(Metadata* obj) {
  assert(oop_recorder() != NULL, "this assembler needs a Recorder");
  int index = oop_recorder()->find_index(obj);
  RelocationHolder rspec = metadata_Relocation::spec(index);
  return AddressLiteral((address)obj, rspec);
}


AddressLiteral MacroAssembler::constant_oop_address(jobject obj) {
#ifdef ASSERT
  {
    ThreadInVMfromUnknown tiv;
    assert(oop_recorder() != NULL, "this assembler needs an OopRecorder");
    assert(Universe::heap()->is_in_reserved(JNIHandles::resolve(obj)), "not an oop");
  }
#endif
  int oop_index = oop_recorder()->find_index(obj);
  return AddressLiteral(obj, oop_Relocation::spec(oop_index));
}

void  MacroAssembler::set_narrow_oop(jobject obj, Register d) {
  assert(oop_recorder() != NULL, "this assembler needs an OopRecorder");
  int oop_index = oop_recorder()->find_index(obj);
  RelocationHolder rspec = oop_Relocation::spec(oop_index);

  assert_not_delayed();
  // Relocation with special format (see relocInfo_sparc.hpp).
  relocate(rspec, 1);
  // Assembler::sethi(0x3fffff, d);
  emit_int32( op(branch_op) | rd(d) | op2(sethi_op2) | hi22(0x3fffff) );
  // Don't add relocation for 'add'. Do patching during 'sethi' processing.
  add(d, 0x3ff, d);

}

void  MacroAssembler::set_narrow_klass(Klass* k, Register d) {
  assert(oop_recorder() != NULL, "this assembler needs an OopRecorder");
  int klass_index = oop_recorder()->find_index(k);
  RelocationHolder rspec = metadata_Relocation::spec(klass_index);
  narrowOop encoded_k = CompressedKlassPointers::encode(k);

  assert_not_delayed();
  // Relocation with special format (see relocInfo_sparc.hpp).
  relocate(rspec, 1);
  // Assembler::sethi(encoded_k, d);
  emit_int32( op(branch_op) | rd(d) | op2(sethi_op2) | hi22(encoded_k) );
  // Don't add relocation for 'add'. Do patching during 'sethi' processing.
  add(d, low10(encoded_k), d);

}

void MacroAssembler::align(int modulus) {
  while (offset() % modulus != 0) nop();
}

void RegistersForDebugging::print(outputStream* s) {
  FlagSetting fs(Debugging, true);
  int j;
  for (j = 0; j < 8; ++j) {
    if (j != 6) { s->print("i%d = ", j); os::print_location(s, i[j]); }
    else        { s->print( "fp = "   ); os::print_location(s, i[j]); }
  }
  s->cr();

  for (j = 0;  j < 8;  ++j) {
    s->print("l%d = ", j); os::print_location(s, l[j]);
  }
  s->cr();

  for (j = 0; j < 8; ++j) {
    if (j != 6) { s->print("o%d = ", j); os::print_location(s, o[j]); }
    else        { s->print( "sp = "   ); os::print_location(s, o[j]); }
  }
  s->cr();

  for (j = 0; j < 8; ++j) {
    s->print("g%d = ", j); os::print_location(s, g[j]);
  }
  s->cr();

  // print out floats with compression
  for (j = 0; j < 32; ) {
    jfloat val = f[j];
    int last = j;
    for ( ;  last+1 < 32;  ++last ) {
      char b1[1024], b2[1024];
      sprintf(b1, "%f", val);
      sprintf(b2, "%f", f[last+1]);
      if (strcmp(b1, b2))
        break;
    }
    s->print("f%d", j);
    if ( j != last )  s->print(" - f%d", last);
    s->print(" = %f", val);
    s->fill_to(25);
    s->print_cr(" (0x%x)", *(int*)&val);
    j = last + 1;
  }
  s->cr();

  // and doubles (evens only)
  for (j = 0; j < 32; ) {
    jdouble val = d[j];
    int last = j;
    for ( ;  last+1 < 32;  ++last ) {
      char b1[1024], b2[1024];
      sprintf(b1, "%f", val);
      sprintf(b2, "%f", d[last+1]);
      if (strcmp(b1, b2))
        break;
    }
    s->print("d%d", 2 * j);
    if ( j != last )  s->print(" - d%d", last);
    s->print(" = %f", val);
    s->fill_to(30);
    s->print("(0x%x)", *(int*)&val);
    s->fill_to(42);
    s->print_cr("(0x%x)", *(1 + (int*)&val));
    j = last + 1;
  }
  s->cr();
}

void RegistersForDebugging::save_registers(MacroAssembler* a) {
  a->sub(FP, align_up(sizeof(RegistersForDebugging), sizeof(jdouble)) - STACK_BIAS, O0);
  a->flushw();
  int i;
  for (i = 0; i < 8; ++i) {
    a->ld_ptr(as_iRegister(i)->address_in_saved_window().after_save(), L1);  a->st_ptr( L1, O0, i_offset(i));
    a->ld_ptr(as_lRegister(i)->address_in_saved_window().after_save(), L1);  a->st_ptr( L1, O0, l_offset(i));
    a->st_ptr(as_oRegister(i)->after_save(), O0, o_offset(i));
    a->st_ptr(as_gRegister(i)->after_save(), O0, g_offset(i));
  }
  for (i = 0;  i < 32; ++i) {
    a->stf(FloatRegisterImpl::S, as_FloatRegister(i), O0, f_offset(i));
  }
  for (i = 0; i < 64; i += 2) {
    a->stf(FloatRegisterImpl::D, as_FloatRegister(i), O0, d_offset(i));
  }
}

void RegistersForDebugging::restore_registers(MacroAssembler* a, Register r) {
  for (int i = 1; i < 8;  ++i) {
    a->ld_ptr(r, g_offset(i), as_gRegister(i));
  }
  for (int j = 0; j < 32; ++j) {
    a->ldf(FloatRegisterImpl::S, O0, f_offset(j), as_FloatRegister(j));
  }
  for (int k = 0; k < 64; k += 2) {
    a->ldf(FloatRegisterImpl::D, O0, d_offset(k), as_FloatRegister(k));
  }
}


// pushes double TOS element of FPU stack on CPU stack; pops from FPU stack
void MacroAssembler::push_fTOS() {
  // %%%%%% need to implement this
}

// pops double TOS element from CPU stack and pushes on FPU stack
void MacroAssembler::pop_fTOS() {
  // %%%%%% need to implement this
}

void MacroAssembler::empty_FPU_stack() {
  // %%%%%% need to implement this
}

void MacroAssembler::_verify_oop(Register reg, const char* msg, const char * file, int line) {
  // plausibility check for oops
  if (!VerifyOops) return;

  if (reg == G0)  return;       // always NULL, which is always an oop

  BLOCK_COMMENT("verify_oop {");
  char buffer[64];
#ifdef COMPILER1
  if (CommentedAssembly) {
    snprintf(buffer, sizeof(buffer), "verify_oop at %d", offset());
    block_comment(buffer);
  }
#endif

  const char* real_msg = NULL;
  {
    ResourceMark rm;
    stringStream ss;
    ss.print("%s at offset %d (%s:%d)", msg, offset(), file, line);
    real_msg = code_string(ss.as_string());
  }

  // Call indirectly to solve generation ordering problem
  AddressLiteral a(StubRoutines::verify_oop_subroutine_entry_address());

  // Make some space on stack above the current register window.
  // Enough to hold 8 64-bit registers.
  add(SP,-8*8,SP);

  // Save some 64-bit registers; a normal 'save' chops the heads off
  // of 64-bit longs in the 32-bit build.
  stx(O0,SP,frame::register_save_words*wordSize+STACK_BIAS+0*8);
  stx(O1,SP,frame::register_save_words*wordSize+STACK_BIAS+1*8);
  mov(reg,O0); // Move arg into O0; arg might be in O7 which is about to be crushed
  stx(O7,SP,frame::register_save_words*wordSize+STACK_BIAS+7*8);

  // Size of set() should stay the same
  patchable_set((intptr_t)real_msg, O1);
  // Load address to call to into O7
  load_ptr_contents(a, O7);
  // Register call to verify_oop_subroutine
  callr(O7, G0);
  delayed()->nop();
  // recover frame size
  add(SP, 8*8,SP);
  BLOCK_COMMENT("} verify_oop");
}

void MacroAssembler::_verify_oop_addr(Address addr, const char* msg, const char * file, int line) {
  // plausibility check for oops
  if (!VerifyOops) return;

  const char* real_msg = NULL;
  {
    ResourceMark rm;
    stringStream ss;
    ss.print("%s at SP+%d (%s:%d)", msg, addr.disp(), file, line);
    real_msg = code_string(ss.as_string());
  }

  // Call indirectly to solve generation ordering problem
  AddressLiteral a(StubRoutines::verify_oop_subroutine_entry_address());

  // Make some space on stack above the current register window.
  // Enough to hold 8 64-bit registers.
  add(SP,-8*8,SP);

  // Save some 64-bit registers; a normal 'save' chops the heads off
  // of 64-bit longs in the 32-bit build.
  stx(O0,SP,frame::register_save_words*wordSize+STACK_BIAS+0*8);
  stx(O1,SP,frame::register_save_words*wordSize+STACK_BIAS+1*8);
  ld_ptr(addr.base(), addr.disp() + 8*8, O0); // Load arg into O0; arg might be in O7 which is about to be crushed
  stx(O7,SP,frame::register_save_words*wordSize+STACK_BIAS+7*8);

  // Size of set() should stay the same
  patchable_set((intptr_t)real_msg, O1);
  // Load address to call to into O7
  load_ptr_contents(a, O7);
  // Register call to verify_oop_subroutine
  callr(O7, G0);
  delayed()->nop();
  // recover frame size
  add(SP, 8*8,SP);
}

// side-door communication with signalHandler in os_solaris.cpp
address MacroAssembler::_verify_oop_implicit_branch[3] = { NULL };

// This macro is expanded just once; it creates shared code.  Contract:
// receives an oop in O0.  Must restore O0 & O7 from TLS.  Must not smash ANY
// registers, including flags.  May not use a register 'save', as this blows
// the high bits of the O-regs if they contain Long values.  Acts as a 'leaf'
// call.
void MacroAssembler::verify_oop_subroutine() {
  // Leaf call; no frame.
  Label succeed, fail, null_or_fail;

  // O0 and O7 were saved already (O0 in O0's TLS home, O7 in O5's TLS home).
  // O0 is now the oop to be checked.  O7 is the return address.
  Register O0_obj = O0;

  // Save some more registers for temps.
  stx(O2,SP,frame::register_save_words*wordSize+STACK_BIAS+2*8);
  stx(O3,SP,frame::register_save_words*wordSize+STACK_BIAS+3*8);
  stx(O4,SP,frame::register_save_words*wordSize+STACK_BIAS+4*8);
  stx(O5,SP,frame::register_save_words*wordSize+STACK_BIAS+5*8);

  // Save flags
  Register O5_save_flags = O5;
  rdccr( O5_save_flags );

  { // count number of verifies
    Register O2_adr   = O2;
    Register O3_accum = O3;
    inc_counter(StubRoutines::verify_oop_count_addr(), O2_adr, O3_accum);
  }

  Register O2_mask = O2;
  Register O3_bits = O3;
  Register O4_temp = O4;

  // mark lower end of faulting range
  assert(_verify_oop_implicit_branch[0] == NULL, "set once");
  _verify_oop_implicit_branch[0] = pc();

  // We can't check the mark oop because it could be in the process of
  // locking or unlocking while this is running.
  set(Universe::verify_oop_mask (), O2_mask);
  set(Universe::verify_oop_bits (), O3_bits);

  // assert((obj & oop_mask) == oop_bits);
  and3(O0_obj, O2_mask, O4_temp);
  cmp_and_brx_short(O4_temp, O3_bits, notEqual, pn, null_or_fail);

  if ((NULL_WORD & Universe::verify_oop_mask()) == Universe::verify_oop_bits()) {
    // the null_or_fail case is useless; must test for null separately
    br_null_short(O0_obj, pn, succeed);
  }

  // Check the Klass* of this object for being in the right area of memory.
  // Cannot do the load in the delay above slot in case O0 is null
  load_klass(O0_obj, O0_obj);
  // assert((klass != NULL)
  br_null_short(O0_obj, pn, fail);

  wrccr( O5_save_flags ); // Restore CCR's

  // mark upper end of faulting range
  _verify_oop_implicit_branch[1] = pc();

  //-----------------------
  // all tests pass
  bind(succeed);

  // Restore prior 64-bit registers
  ldx(SP,frame::register_save_words*wordSize+STACK_BIAS+0*8,O0);
  ldx(SP,frame::register_save_words*wordSize+STACK_BIAS+1*8,O1);
  ldx(SP,frame::register_save_words*wordSize+STACK_BIAS+2*8,O2);
  ldx(SP,frame::register_save_words*wordSize+STACK_BIAS+3*8,O3);
  ldx(SP,frame::register_save_words*wordSize+STACK_BIAS+4*8,O4);
  ldx(SP,frame::register_save_words*wordSize+STACK_BIAS+5*8,O5);

  retl();                       // Leaf return; restore prior O7 in delay slot
  delayed()->ldx(SP,frame::register_save_words*wordSize+STACK_BIAS+7*8,O7);

  //-----------------------
  bind(null_or_fail);           // nulls are less common but OK
  br_null(O0_obj, false, pt, succeed);
  delayed()->wrccr( O5_save_flags ); // Restore CCR's

  //-----------------------
  // report failure:
  bind(fail);
  _verify_oop_implicit_branch[2] = pc();

  wrccr( O5_save_flags ); // Restore CCR's

  save_frame(align_up(sizeof(RegistersForDebugging) / BytesPerWord, 2));

  // stop_subroutine expects message pointer in I1.
  mov(I1, O1);

  // Restore prior 64-bit registers
  ldx(FP,frame::register_save_words*wordSize+STACK_BIAS+0*8,I0);
  ldx(FP,frame::register_save_words*wordSize+STACK_BIAS+1*8,I1);
  ldx(FP,frame::register_save_words*wordSize+STACK_BIAS+2*8,I2);
  ldx(FP,frame::register_save_words*wordSize+STACK_BIAS+3*8,I3);
  ldx(FP,frame::register_save_words*wordSize+STACK_BIAS+4*8,I4);
  ldx(FP,frame::register_save_words*wordSize+STACK_BIAS+5*8,I5);

  // factor long stop-sequence into subroutine to save space
  assert(StubRoutines::Sparc::stop_subroutine_entry_address(), "hasn't been generated yet");

  // call indirectly to solve generation ordering problem
  AddressLiteral al(StubRoutines::Sparc::stop_subroutine_entry_address());
  load_ptr_contents(al, O5);
  jmpl(O5, 0, O7);
  delayed()->nop();
}


void MacroAssembler::stop(const char* msg) {
  // save frame first to get O7 for return address
  // add one word to size in case struct is odd number of words long
  // It must be doubleword-aligned for storing doubles into it.

    save_frame(align_up(sizeof(RegistersForDebugging) / BytesPerWord, 2));

    // stop_subroutine expects message pointer in I1.
    // Size of set() should stay the same
    patchable_set((intptr_t)msg, O1);

    // factor long stop-sequence into subroutine to save space
    assert(StubRoutines::Sparc::stop_subroutine_entry_address(), "hasn't been generated yet");

    // call indirectly to solve generation ordering problem
    AddressLiteral a(StubRoutines::Sparc::stop_subroutine_entry_address());
    load_ptr_contents(a, O5);
    jmpl(O5, 0, O7);
    delayed()->nop();

    breakpoint_trap();   // make stop actually stop rather than writing
                         // unnoticeable results in the output files.

    // restore(); done in callee to save space!
}


void MacroAssembler::warn(const char* msg) {
  save_frame(align_up(sizeof(RegistersForDebugging) / BytesPerWord, 2));
  RegistersForDebugging::save_registers(this);
  mov(O0, L0);
  // Size of set() should stay the same
  patchable_set((intptr_t)msg, O0);
  call( CAST_FROM_FN_PTR(address, warning) );
  delayed()->nop();
//  ret();
//  delayed()->restore();
  RegistersForDebugging::restore_registers(this, L0);
  restore();
}


void MacroAssembler::untested(const char* what) {
  // We must be able to turn interactive prompting off
  // in order to run automated test scripts on the VM
  // Use the flag ShowMessageBoxOnError

  const char* b = NULL;
  {
    ResourceMark rm;
    stringStream ss;
    ss.print("untested: %s", what);
    b = code_string(ss.as_string());
  }
  if (ShowMessageBoxOnError) { STOP(b); }
  else                       { warn(b); }
}


void MacroAssembler::unimplemented(const char* what) {
  const char* buf = NULL;
  {
    ResourceMark rm;
    stringStream ss;
    ss.print("unimplemented: %s", what);
    buf = code_string(ss.as_string());
  }
  stop(buf);
}


void MacroAssembler::stop_subroutine() {
  RegistersForDebugging::save_registers(this);

  // for the sake of the debugger, stick a PC on the current frame
  // (this assumes that the caller has performed an extra "save")
  mov(I7, L7);
  add(O7, -7 * BytesPerInt, I7);

  save_frame(); // one more save to free up another O7 register
  mov(I0, O1); // addr of reg save area

  // We expect pointer to message in I1. Caller must set it up in O1
  mov(I1, O0); // get msg
  call (CAST_FROM_FN_PTR(address, MacroAssembler::debug), relocInfo::runtime_call_type);
  delayed()->nop();

  restore();

  RegistersForDebugging::restore_registers(this, O0);

  save_frame(0);
  call(CAST_FROM_FN_PTR(address,breakpoint));
  delayed()->nop();
  restore();

  mov(L7, I7);
  retl();
  delayed()->restore(); // see stop above
}


void MacroAssembler::debug(char* msg, RegistersForDebugging* regs) {
  if ( ShowMessageBoxOnError ) {
    JavaThread* thread = JavaThread::current();
    JavaThreadState saved_state = thread->thread_state();
    thread->set_thread_state(_thread_in_vm);
      {
        // In order to get locks work, we need to fake a in_VM state
        ttyLocker ttyl;
        ::tty->print_cr("EXECUTION STOPPED: %s\n", msg);
        if (CountBytecodes || TraceBytecodes || StopInterpreterAt) {
        BytecodeCounter::print();
        }
        if (os::message_box(msg, "Execution stopped, print registers?"))
          regs->print(::tty);
      }
    BREAKPOINT;
      ThreadStateTransition::transition(JavaThread::current(), _thread_in_vm, saved_state);
  }
  else {
     ::tty->print_cr("=============== DEBUG MESSAGE: %s ================\n", msg);
  }
  assert(false, "DEBUG MESSAGE: %s", msg);
}


void MacroAssembler::calc_mem_param_words(Register Rparam_words, Register Rresult) {
  subcc( Rparam_words, Argument::n_register_parameters, Rresult); // how many mem words?
  Label no_extras;
  br( negative, true, pt, no_extras ); // if neg, clear reg
  delayed()->set(0, Rresult);          // annuled, so only if taken
  bind( no_extras );
}


void MacroAssembler::calc_frame_size(Register Rextra_words, Register Rresult) {
  add(Rextra_words, frame::memory_parameter_word_sp_offset, Rresult);
  bclr(1, Rresult);
  sll(Rresult, LogBytesPerWord, Rresult);  // Rresult has total frame bytes
}


void MacroAssembler::calc_frame_size_and_save(Register Rextra_words, Register Rresult) {
  calc_frame_size(Rextra_words, Rresult);
  neg(Rresult);
  save(SP, Rresult, SP);
}


// ---------------------------------------------------------
Assembler::RCondition cond2rcond(Assembler::Condition c) {
  switch (c) {
    /*case zero: */
    case Assembler::equal:        return Assembler::rc_z;
    case Assembler::lessEqual:    return Assembler::rc_lez;
    case Assembler::less:         return Assembler::rc_lz;
    /*case notZero:*/
    case Assembler::notEqual:     return Assembler::rc_nz;
    case Assembler::greater:      return Assembler::rc_gz;
    case Assembler::greaterEqual: return Assembler::rc_gez;
  }
  ShouldNotReachHere();
  return Assembler::rc_z;
}

// compares (32 bit) register with zero and branches.  NOT FOR USE WITH 64-bit POINTERS
void MacroAssembler::cmp_zero_and_br(Condition c, Register s1, Label& L, bool a, Predict p) {
  tst(s1);
  br (c, a, p, L);
}

// Compares a pointer register with zero and branches on null.
// Does a test & branch on 32-bit systems and a register-branch on 64-bit.
void MacroAssembler::br_null( Register s1, bool a, Predict p, Label& L ) {
  assert_not_delayed();
  bpr( rc_z, a, p, s1, L );
}

void MacroAssembler::br_notnull( Register s1, bool a, Predict p, Label& L ) {
  assert_not_delayed();
  bpr( rc_nz, a, p, s1, L );
}

// Compare registers and branch with nop in delay slot or cbcond without delay slot.

// Compare integer (32 bit) values (icc only).
void MacroAssembler::cmp_and_br_short(Register s1, Register s2, Condition c,
                                      Predict p, Label& L) {
  assert_not_delayed();
  if (use_cbcond(L)) {
    Assembler::cbcond(c, icc, s1, s2, L);
  } else {
    cmp(s1, s2);
    br(c, false, p, L);
    delayed()->nop();
  }
}

// Compare integer (32 bit) values (icc only).
void MacroAssembler::cmp_and_br_short(Register s1, int simm13a, Condition c,
                                      Predict p, Label& L) {
  assert_not_delayed();
  if (is_simm(simm13a,5) && use_cbcond(L)) {
    Assembler::cbcond(c, icc, s1, simm13a, L);
  } else {
    cmp(s1, simm13a);
    br(c, false, p, L);
    delayed()->nop();
  }
}

// Branch that tests xcc in LP64 and icc in !LP64
void MacroAssembler::cmp_and_brx_short(Register s1, Register s2, Condition c,
                                       Predict p, Label& L) {
  assert_not_delayed();
  if (use_cbcond(L)) {
    Assembler::cbcond(c, ptr_cc, s1, s2, L);
  } else {
    cmp(s1, s2);
    brx(c, false, p, L);
    delayed()->nop();
  }
}

// Branch that tests xcc in LP64 and icc in !LP64
void MacroAssembler::cmp_and_brx_short(Register s1, int simm13a, Condition c,
                                       Predict p, Label& L) {
  assert_not_delayed();
  if (is_simm(simm13a,5) && use_cbcond(L)) {
    Assembler::cbcond(c, ptr_cc, s1, simm13a, L);
  } else {
    cmp(s1, simm13a);
    brx(c, false, p, L);
    delayed()->nop();
  }
}

// Short branch version for compares a pointer with zero.

void MacroAssembler::br_null_short(Register s1, Predict p, Label& L) {
  assert_not_delayed();
  if (use_cbcond(L)) {
    Assembler::cbcond(zero, ptr_cc, s1, 0, L);
  } else {
    br_null(s1, false, p, L);
    delayed()->nop();
  }
}

void MacroAssembler::br_notnull_short(Register s1, Predict p, Label& L) {
  assert_not_delayed();
  if (use_cbcond(L)) {
    Assembler::cbcond(notZero, ptr_cc, s1, 0, L);
  } else {
    br_notnull(s1, false, p, L);
    delayed()->nop();
  }
}

// Unconditional short branch
void MacroAssembler::ba_short(Label& L) {
  assert_not_delayed();
  if (use_cbcond(L)) {
    Assembler::cbcond(equal, icc, G0, G0, L);
  } else {
    br(always, false, pt, L);
    delayed()->nop();
  }
}

// Branch if 'icc' says zero or not (i.e. icc.z == 1|0).

void MacroAssembler::br_icc_zero(bool iszero, Predict p, Label &L) {
  assert_not_delayed();
  Condition cf = (iszero ? Assembler::zero : Assembler::notZero);
  br(cf, false, p, L);
  delayed()->nop();
}

// instruction sequences factored across compiler & interpreter


void MacroAssembler::lcmp( Register Ra_hi, Register Ra_low,
                           Register Rb_hi, Register Rb_low,
                           Register Rresult) {

  Label check_low_parts, done;

  cmp(Ra_hi, Rb_hi );  // compare hi parts
  br(equal, true, pt, check_low_parts);
  delayed()->cmp(Ra_low, Rb_low); // test low parts

  // And, with an unsigned comparison, it does not matter if the numbers
  // are negative or not.
  // E.g., -2 cmp -1: the low parts are 0xfffffffe and 0xffffffff.
  // The second one is bigger (unsignedly).

  // Other notes:  The first move in each triplet can be unconditional
  // (and therefore probably prefetchable).
  // And the equals case for the high part does not need testing,
  // since that triplet is reached only after finding the high halves differ.

  mov(-1, Rresult);
  ba(done);
  delayed()->movcc(greater, false, icc,  1, Rresult);

  bind(check_low_parts);

  mov(                               -1, Rresult);
  movcc(equal,           false, icc,  0, Rresult);
  movcc(greaterUnsigned, false, icc,  1, Rresult);

  bind(done);
}

void MacroAssembler::lneg( Register Rhi, Register Rlow ) {
  subcc(  G0, Rlow, Rlow );
  subc(   G0, Rhi,  Rhi  );
}

void MacroAssembler::lshl( Register Rin_high,  Register Rin_low,
                           Register Rcount,
                           Register Rout_high, Register Rout_low,
                           Register Rtemp ) {


  Register Ralt_count = Rtemp;
  Register Rxfer_bits = Rtemp;

  assert( Ralt_count != Rin_high
      &&  Ralt_count != Rin_low
      &&  Ralt_count != Rcount
      &&  Rxfer_bits != Rin_low
      &&  Rxfer_bits != Rin_high
      &&  Rxfer_bits != Rcount
      &&  Rxfer_bits != Rout_low
      &&  Rout_low   != Rin_high,
        "register alias checks");

  Label big_shift, done;

  // This code can be optimized to use the 64 bit shifts in V9.
  // Here we use the 32 bit shifts.

  and3( Rcount, 0x3f, Rcount);     // take least significant 6 bits
  subcc(Rcount,   31, Ralt_count);
  br(greater, true, pn, big_shift);
  delayed()->dec(Ralt_count);

  // shift < 32 bits, Ralt_count = Rcount-31

  // We get the transfer bits by shifting right by 32-count the low
  // register. This is done by shifting right by 31-count and then by one
  // more to take care of the special (rare) case where count is zero
  // (shifting by 32 would not work).

  neg(Ralt_count);

  // The order of the next two instructions is critical in the case where
  // Rin and Rout are the same and should not be reversed.

  srl(Rin_low, Ralt_count, Rxfer_bits); // shift right by 31-count
  if (Rcount != Rout_low) {
    sll(Rin_low, Rcount, Rout_low); // low half
  }
  sll(Rin_high, Rcount, Rout_high);
  if (Rcount == Rout_low) {
    sll(Rin_low, Rcount, Rout_low); // low half
  }
  srl(Rxfer_bits, 1, Rxfer_bits ); // shift right by one more
  ba(done);
  delayed()->or3(Rout_high, Rxfer_bits, Rout_high);   // new hi value: or in shifted old hi part and xfer from low

  // shift >= 32 bits, Ralt_count = Rcount-32
  bind(big_shift);
  sll(Rin_low, Ralt_count, Rout_high  );
  clr(Rout_low);

  bind(done);
}


void MacroAssembler::lshr( Register Rin_high,  Register Rin_low,
                           Register Rcount,
                           Register Rout_high, Register Rout_low,
                           Register Rtemp ) {

  Register Ralt_count = Rtemp;
  Register Rxfer_bits = Rtemp;

  assert( Ralt_count != Rin_high
      &&  Ralt_count != Rin_low
      &&  Ralt_count != Rcount
      &&  Rxfer_bits != Rin_low
      &&  Rxfer_bits != Rin_high
      &&  Rxfer_bits != Rcount
      &&  Rxfer_bits != Rout_high
      &&  Rout_high  != Rin_low,
        "register alias checks");

  Label big_shift, done;

  // This code can be optimized to use the 64 bit shifts in V9.
  // Here we use the 32 bit shifts.

  and3( Rcount, 0x3f, Rcount);     // take least significant 6 bits
  subcc(Rcount,   31, Ralt_count);
  br(greater, true, pn, big_shift);
  delayed()->dec(Ralt_count);

  // shift < 32 bits, Ralt_count = Rcount-31

  // We get the transfer bits by shifting left by 32-count the high
  // register. This is done by shifting left by 31-count and then by one
  // more to take care of the special (rare) case where count is zero
  // (shifting by 32 would not work).

  neg(Ralt_count);
  if (Rcount != Rout_low) {
    srl(Rin_low, Rcount, Rout_low);
  }

  // The order of the next two instructions is critical in the case where
  // Rin and Rout are the same and should not be reversed.

  sll(Rin_high, Ralt_count, Rxfer_bits); // shift left by 31-count
  sra(Rin_high,     Rcount, Rout_high ); // high half
  sll(Rxfer_bits,        1, Rxfer_bits); // shift left by one more
  if (Rcount == Rout_low) {
    srl(Rin_low, Rcount, Rout_low);
  }
  ba(done);
  delayed()->or3(Rout_low, Rxfer_bits, Rout_low); // new low value: or shifted old low part and xfer from high

  // shift >= 32 bits, Ralt_count = Rcount-32
  bind(big_shift);

  sra(Rin_high, Ralt_count, Rout_low);
  sra(Rin_high,         31, Rout_high); // sign into hi

  bind( done );
}



void MacroAssembler::lushr( Register Rin_high,  Register Rin_low,
                            Register Rcount,
                            Register Rout_high, Register Rout_low,
                            Register Rtemp ) {

  Register Ralt_count = Rtemp;
  Register Rxfer_bits = Rtemp;

  assert( Ralt_count != Rin_high
      &&  Ralt_count != Rin_low
      &&  Ralt_count != Rcount
      &&  Rxfer_bits != Rin_low
      &&  Rxfer_bits != Rin_high
      &&  Rxfer_bits != Rcount
      &&  Rxfer_bits != Rout_high
      &&  Rout_high  != Rin_low,
        "register alias checks");

  Label big_shift, done;

  // This code can be optimized to use the 64 bit shifts in V9.
  // Here we use the 32 bit shifts.

  and3( Rcount, 0x3f, Rcount);     // take least significant 6 bits
  subcc(Rcount,   31, Ralt_count);
  br(greater, true, pn, big_shift);
  delayed()->dec(Ralt_count);

  // shift < 32 bits, Ralt_count = Rcount-31

  // We get the transfer bits by shifting left by 32-count the high
  // register. This is done by shifting left by 31-count and then by one
  // more to take care of the special (rare) case where count is zero
  // (shifting by 32 would not work).

  neg(Ralt_count);
  if (Rcount != Rout_low) {
    srl(Rin_low, Rcount, Rout_low);
  }

  // The order of the next two instructions is critical in the case where
  // Rin and Rout are the same and should not be reversed.

  sll(Rin_high, Ralt_count, Rxfer_bits); // shift left by 31-count
  srl(Rin_high,     Rcount, Rout_high ); // high half
  sll(Rxfer_bits,        1, Rxfer_bits); // shift left by one more
  if (Rcount == Rout_low) {
    srl(Rin_low, Rcount, Rout_low);
  }
  ba(done);
  delayed()->or3(Rout_low, Rxfer_bits, Rout_low); // new low value: or shifted old low part and xfer from high

  // shift >= 32 bits, Ralt_count = Rcount-32
  bind(big_shift);

  srl(Rin_high, Ralt_count, Rout_low);
  clr(Rout_high);

  bind( done );
}

void MacroAssembler::lcmp( Register Ra, Register Rb, Register Rresult) {
  cmp(Ra, Rb);
  mov(-1, Rresult);
  movcc(equal,   false, xcc,  0, Rresult);
  movcc(greater, false, xcc,  1, Rresult);
}


void MacroAssembler::load_sized_value(Address src, Register dst, size_t size_in_bytes, bool is_signed) {
  switch (size_in_bytes) {
  case  8:  ld_long(src, dst); break;
  case  4:  ld(     src, dst); break;
  case  2:  is_signed ? ldsh(src, dst) : lduh(src, dst); break;
  case  1:  is_signed ? ldsb(src, dst) : ldub(src, dst); break;
  default:  ShouldNotReachHere();
  }
}

void MacroAssembler::store_sized_value(Register src, Address dst, size_t size_in_bytes) {
  switch (size_in_bytes) {
  case  8:  st_long(src, dst); break;
  case  4:  st(     src, dst); break;
  case  2:  sth(    src, dst); break;
  case  1:  stb(    src, dst); break;
  default:  ShouldNotReachHere();
  }
}


void MacroAssembler::float_cmp( bool is_float, int unordered_result,
                                FloatRegister Fa, FloatRegister Fb,
                                Register Rresult) {
  if (is_float) {
    fcmp(FloatRegisterImpl::S, fcc0, Fa, Fb);
  } else {
    fcmp(FloatRegisterImpl::D, fcc0, Fa, Fb);
  }

  if (unordered_result == 1) {
    mov(                                    -1, Rresult);
    movcc(f_equal,              true, fcc0,  0, Rresult);
    movcc(f_unorderedOrGreater, true, fcc0,  1, Rresult);
  } else {
    mov(                                    -1, Rresult);
    movcc(f_equal,              true, fcc0,  0, Rresult);
    movcc(f_greater,            true, fcc0,  1, Rresult);
  }
}


void MacroAssembler::save_all_globals_into_locals() {
  mov(G1,L1);
  mov(G2,L2);
  mov(G3,L3);
  mov(G4,L4);
  mov(G5,L5);
  mov(G6,L6);
  mov(G7,L7);
}

void MacroAssembler::restore_globals_from_locals() {
  mov(L1,G1);
  mov(L2,G2);
  mov(L3,G3);
  mov(L4,G4);
  mov(L5,G5);
  mov(L6,G6);
  mov(L7,G7);
}

RegisterOrConstant MacroAssembler::delayed_value_impl(intptr_t* delayed_value_addr,
                                                      Register tmp,
                                                      int offset) {
  intptr_t value = *delayed_value_addr;
  if (value != 0)
    return RegisterOrConstant(value + offset);

  // load indirectly to solve generation ordering problem
  AddressLiteral a(delayed_value_addr);
  load_ptr_contents(a, tmp);

#ifdef ASSERT
  tst(tmp);
  breakpoint_trap(zero, xcc);
#endif

  if (offset != 0)
    add(tmp, offset, tmp);

  return RegisterOrConstant(tmp);
}


RegisterOrConstant MacroAssembler::regcon_andn_ptr(RegisterOrConstant s1, RegisterOrConstant s2, RegisterOrConstant d, Register temp) {
  assert(d.register_or_noreg() != G0, "lost side effect");
  if ((s2.is_constant() && s2.as_constant() == 0) ||
      (s2.is_register() && s2.as_register() == G0)) {
    // Do nothing, just move value.
    if (s1.is_register()) {
      if (d.is_constant())  d = temp;
      mov(s1.as_register(), d.as_register());
      return d;
    } else {
      return s1;
    }
  }

  if (s1.is_register()) {
    assert_different_registers(s1.as_register(), temp);
    if (d.is_constant())  d = temp;
    andn(s1.as_register(), ensure_simm13_or_reg(s2, temp), d.as_register());
    return d;
  } else {
    if (s2.is_register()) {
      assert_different_registers(s2.as_register(), temp);
      if (d.is_constant())  d = temp;
      set(s1.as_constant(), temp);
      andn(temp, s2.as_register(), d.as_register());
      return d;
    } else {
      intptr_t res = s1.as_constant() & ~s2.as_constant();
      return res;
    }
  }
}

RegisterOrConstant MacroAssembler::regcon_inc_ptr(RegisterOrConstant s1, RegisterOrConstant s2, RegisterOrConstant d, Register temp) {
  assert(d.register_or_noreg() != G0, "lost side effect");
  if ((s2.is_constant() && s2.as_constant() == 0) ||
      (s2.is_register() && s2.as_register() == G0)) {
    // Do nothing, just move value.
    if (s1.is_register()) {
      if (d.is_constant())  d = temp;
      mov(s1.as_register(), d.as_register());
      return d;
    } else {
      return s1;
    }
  }

  if (s1.is_register()) {
    assert_different_registers(s1.as_register(), temp);
    if (d.is_constant())  d = temp;
    add(s1.as_register(), ensure_simm13_or_reg(s2, temp), d.as_register());
    return d;
  } else {
    if (s2.is_register()) {
      assert_different_registers(s2.as_register(), temp);
      if (d.is_constant())  d = temp;
      add(s2.as_register(), ensure_simm13_or_reg(s1, temp), d.as_register());
      return d;
    } else {
      intptr_t res = s1.as_constant() + s2.as_constant();
      return res;
    }
  }
}

RegisterOrConstant MacroAssembler::regcon_sll_ptr(RegisterOrConstant s1, RegisterOrConstant s2, RegisterOrConstant d, Register temp) {
  assert(d.register_or_noreg() != G0, "lost side effect");
  if (!is_simm13(s2.constant_or_zero()))
    s2 = (s2.as_constant() & 0xFF);
  if ((s2.is_constant() && s2.as_constant() == 0) ||
      (s2.is_register() && s2.as_register() == G0)) {
    // Do nothing, just move value.
    if (s1.is_register()) {
      if (d.is_constant())  d = temp;
      mov(s1.as_register(), d.as_register());
      return d;
    } else {
      return s1;
    }
  }

  if (s1.is_register()) {
    assert_different_registers(s1.as_register(), temp);
    if (d.is_constant())  d = temp;
    sll_ptr(s1.as_register(), ensure_simm13_or_reg(s2, temp), d.as_register());
    return d;
  } else {
    if (s2.is_register()) {
      assert_different_registers(s2.as_register(), temp);
      if (d.is_constant())  d = temp;
      set(s1.as_constant(), temp);
      sll_ptr(temp, s2.as_register(), d.as_register());
      return d;
    } else {
      intptr_t res = s1.as_constant() << s2.as_constant();
      return res;
    }
  }
}


// Look up the method for a megamorphic invokeinterface call.
// The target method is determined by <intf_klass, itable_index>.
// The receiver klass is in recv_klass.
// On success, the result will be in method_result, and execution falls through.
// On failure, execution transfers to the given label.
void MacroAssembler::lookup_interface_method(Register recv_klass,
                                             Register intf_klass,
                                             RegisterOrConstant itable_index,
                                             Register method_result,
                                             Register scan_temp,
                                             Register sethi_temp,
                                             Label& L_no_such_interface,
                                             bool return_method) {
  assert_different_registers(recv_klass, intf_klass, method_result, scan_temp);
  assert(!return_method || itable_index.is_constant() || itable_index.as_register() == method_result,
         "caller must use same register for non-constant itable index as for method");

  Label L_no_such_interface_restore;
  bool did_save = false;
  if (scan_temp == noreg || sethi_temp == noreg) {
    Register recv_2 = recv_klass->is_global() ? recv_klass : L0;
    Register intf_2 = intf_klass->is_global() ? intf_klass : L1;
    assert(method_result->is_global(), "must be able to return value");
    scan_temp  = L2;
    sethi_temp = L3;
    save_frame_and_mov(0, recv_klass, recv_2, intf_klass, intf_2);
    recv_klass = recv_2;
    intf_klass = intf_2;
    did_save = true;
  }

  // Compute start of first itableOffsetEntry (which is at the end of the vtable)
  int vtable_base = in_bytes(Klass::vtable_start_offset());
  int scan_step   = itableOffsetEntry::size() * wordSize;
  int vte_size    = vtableEntry::size_in_bytes();

  lduw(recv_klass, in_bytes(Klass::vtable_length_offset()), scan_temp);
  // %%% We should store the aligned, prescaled offset in the klassoop.
  // Then the next several instructions would fold away.

  int itb_offset = vtable_base;
  int itb_scale = exact_log2(vtableEntry::size_in_bytes());
  sll(scan_temp, itb_scale,  scan_temp);
  add(scan_temp, itb_offset, scan_temp);
  add(recv_klass, scan_temp, scan_temp);

  if (return_method) {
    // Adjust recv_klass by scaled itable_index, so we can free itable_index.
    RegisterOrConstant itable_offset = itable_index;
    itable_offset = regcon_sll_ptr(itable_index, exact_log2(itableMethodEntry::size() * wordSize), itable_offset);
    itable_offset = regcon_inc_ptr(itable_offset, itableMethodEntry::method_offset_in_bytes(), itable_offset);
    add(recv_klass, ensure_simm13_or_reg(itable_offset, sethi_temp), recv_klass);
  }

  // for (scan = klass->itable(); scan->interface() != NULL; scan += scan_step) {
  //   if (scan->interface() == intf) {
  //     result = (klass + scan->offset() + itable_index);
  //   }
  // }
  Label L_search, L_found_method;

  for (int peel = 1; peel >= 0; peel--) {
    // %%%% Could load both offset and interface in one ldx, if they were
    // in the opposite order.  This would save a load.
    ld_ptr(scan_temp, itableOffsetEntry::interface_offset_in_bytes(), method_result);

    // Check that this entry is non-null.  A null entry means that
    // the receiver class doesn't implement the interface, and wasn't the
    // same as when the caller was compiled.
    bpr(Assembler::rc_z, false, Assembler::pn, method_result, did_save ? L_no_such_interface_restore : L_no_such_interface);
    delayed()->cmp(method_result, intf_klass);

    if (peel) {
      brx(Assembler::equal,    false, Assembler::pt, L_found_method);
    } else {
      brx(Assembler::notEqual, false, Assembler::pn, L_search);
      // (invert the test to fall through to found_method...)
    }
    delayed()->add(scan_temp, scan_step, scan_temp);

    if (!peel)  break;

    bind(L_search);
  }

  bind(L_found_method);

  if (return_method) {
    // Got a hit.
    int ito_offset = itableOffsetEntry::offset_offset_in_bytes();
    // scan_temp[-scan_step] points to the vtable offset we need
    ito_offset -= scan_step;
    lduw(scan_temp, ito_offset, scan_temp);
    ld_ptr(recv_klass, scan_temp, method_result);
  }

  if (did_save) {
    Label L_done;
    ba(L_done);
    delayed()->restore();

    bind(L_no_such_interface_restore);
    ba(L_no_such_interface);
    delayed()->restore();

    bind(L_done);
  }
}


// virtual method calling
void MacroAssembler::lookup_virtual_method(Register recv_klass,
                                           RegisterOrConstant vtable_index,
                                           Register method_result) {
  assert_different_registers(recv_klass, method_result, vtable_index.register_or_noreg());
  Register sethi_temp = method_result;
  const int base = in_bytes(Klass::vtable_start_offset()) +
                   // method pointer offset within the vtable entry:
                   vtableEntry::method_offset_in_bytes();
  RegisterOrConstant vtable_offset = vtable_index;
  // Each of the following three lines potentially generates an instruction.
  // But the total number of address formation instructions will always be
  // at most two, and will often be zero.  In any case, it will be optimal.
  // If vtable_index is a register, we will have (sll_ptr N,x; inc_ptr B,x; ld_ptr k,x).
  // If vtable_index is a constant, we will have at most (set B+X<<N,t; ld_ptr k,t).
  vtable_offset = regcon_sll_ptr(vtable_index, exact_log2(vtableEntry::size_in_bytes()), vtable_offset);
  vtable_offset = regcon_inc_ptr(vtable_offset, base, vtable_offset, sethi_temp);
  Address vtable_entry_addr(recv_klass, ensure_simm13_or_reg(vtable_offset, sethi_temp));
  ld_ptr(vtable_entry_addr, method_result);
}


void MacroAssembler::check_klass_subtype(Register sub_klass,
                                         Register super_klass,
                                         Register temp_reg,
                                         Register temp2_reg,
                                         Label& L_success) {
  Register sub_2 = sub_klass;
  Register sup_2 = super_klass;
  if (!sub_2->is_global())  sub_2 = L0;
  if (!sup_2->is_global())  sup_2 = L1;
  bool did_save = false;
  if (temp_reg == noreg || temp2_reg == noreg) {
    temp_reg = L2;
    temp2_reg = L3;
    save_frame_and_mov(0, sub_klass, sub_2, super_klass, sup_2);
    sub_klass = sub_2;
    super_klass = sup_2;
    did_save = true;
  }
  Label L_failure, L_pop_to_failure, L_pop_to_success;
  check_klass_subtype_fast_path(sub_klass, super_klass,
                                temp_reg, temp2_reg,
                                (did_save ? &L_pop_to_success : &L_success),
                                (did_save ? &L_pop_to_failure : &L_failure), NULL);

  if (!did_save)
    save_frame_and_mov(0, sub_klass, sub_2, super_klass, sup_2);
  check_klass_subtype_slow_path(sub_2, sup_2,
                                L2, L3, L4, L5,
                                NULL, &L_pop_to_failure);

  // on success:
  bind(L_pop_to_success);
  restore();
  ba_short(L_success);

  // on failure:
  bind(L_pop_to_failure);
  restore();
  bind(L_failure);
}


void MacroAssembler::check_klass_subtype_fast_path(Register sub_klass,
                                                   Register super_klass,
                                                   Register temp_reg,
                                                   Register temp2_reg,
                                                   Label* L_success,
                                                   Label* L_failure,
                                                   Label* L_slow_path,
                                        RegisterOrConstant super_check_offset) {
  int sc_offset = in_bytes(Klass::secondary_super_cache_offset());
  int sco_offset = in_bytes(Klass::super_check_offset_offset());

  bool must_load_sco  = (super_check_offset.constant_or_zero() == -1);
  bool need_slow_path = (must_load_sco ||
                         super_check_offset.constant_or_zero() == sco_offset);

  assert_different_registers(sub_klass, super_klass, temp_reg);
  if (super_check_offset.is_register()) {
    assert_different_registers(sub_klass, super_klass, temp_reg,
                               super_check_offset.as_register());
  } else if (must_load_sco) {
    assert(temp2_reg != noreg, "supply either a temp or a register offset");
  }

  Label L_fallthrough;
  int label_nulls = 0;
  if (L_success == NULL)   { L_success   = &L_fallthrough; label_nulls++; }
  if (L_failure == NULL)   { L_failure   = &L_fallthrough; label_nulls++; }
  if (L_slow_path == NULL) { L_slow_path = &L_fallthrough; label_nulls++; }
  assert(label_nulls <= 1 ||
         (L_slow_path == &L_fallthrough && label_nulls <= 2 && !need_slow_path),
         "at most one NULL in the batch, usually");

  // If the pointers are equal, we are done (e.g., String[] elements).
  // This self-check enables sharing of secondary supertype arrays among
  // non-primary types such as array-of-interface.  Otherwise, each such
  // type would need its own customized SSA.
  // We move this check to the front of the fast path because many
  // type checks are in fact trivially successful in this manner,
  // so we get a nicely predicted branch right at the start of the check.
  cmp(super_klass, sub_klass);
  brx(Assembler::equal, false, Assembler::pn, *L_success);
  delayed()->nop();

  // Check the supertype display:
  if (must_load_sco) {
    // The super check offset is always positive...
    lduw(super_klass, sco_offset, temp2_reg);
    super_check_offset = RegisterOrConstant(temp2_reg);
    // super_check_offset is register.
    assert_different_registers(sub_klass, super_klass, temp_reg, super_check_offset.as_register());
  }
  ld_ptr(sub_klass, super_check_offset, temp_reg);
  cmp(super_klass, temp_reg);

  // This check has worked decisively for primary supers.
  // Secondary supers are sought in the super_cache ('super_cache_addr').
  // (Secondary supers are interfaces and very deeply nested subtypes.)
  // This works in the same check above because of a tricky aliasing
  // between the super_cache and the primary super display elements.
  // (The 'super_check_addr' can address either, as the case requires.)
  // Note that the cache is updated below if it does not help us find
  // what we need immediately.
  // So if it was a primary super, we can just fail immediately.
  // Otherwise, it's the slow path for us (no success at this point).

  // Hacked ba(), which may only be used just before L_fallthrough.
#define FINAL_JUMP(label)            \
  if (&(label) != &L_fallthrough) {  \
    ba(label);  delayed()->nop();    \
  }

  if (super_check_offset.is_register()) {
    brx(Assembler::equal, false, Assembler::pn, *L_success);
    delayed()->cmp(super_check_offset.as_register(), sc_offset);

    if (L_failure == &L_fallthrough) {
      brx(Assembler::equal, false, Assembler::pt, *L_slow_path);
      delayed()->nop();
    } else {
      brx(Assembler::notEqual, false, Assembler::pn, *L_failure);
      delayed()->nop();
      FINAL_JUMP(*L_slow_path);
    }
  } else if (super_check_offset.as_constant() == sc_offset) {
    // Need a slow path; fast failure is impossible.
    if (L_slow_path == &L_fallthrough) {
      brx(Assembler::equal, false, Assembler::pt, *L_success);
      delayed()->nop();
    } else {
      brx(Assembler::notEqual, false, Assembler::pn, *L_slow_path);
      delayed()->nop();
      FINAL_JUMP(*L_success);
    }
  } else {
    // No slow path; it's a fast decision.
    if (L_failure == &L_fallthrough) {
      brx(Assembler::equal, false, Assembler::pt, *L_success);
      delayed()->nop();
    } else {
      brx(Assembler::notEqual, false, Assembler::pn, *L_failure);
      delayed()->nop();
      FINAL_JUMP(*L_success);
    }
  }

  bind(L_fallthrough);

#undef FINAL_JUMP
}


void MacroAssembler::check_klass_subtype_slow_path(Register sub_klass,
                                                   Register super_klass,
                                                   Register count_temp,
                                                   Register scan_temp,
                                                   Register scratch_reg,
                                                   Register coop_reg,
                                                   Label* L_success,
                                                   Label* L_failure) {
  assert_different_registers(sub_klass, super_klass,
                             count_temp, scan_temp, scratch_reg, coop_reg);

  Label L_fallthrough, L_loop;
  int label_nulls = 0;
  if (L_success == NULL)   { L_success   = &L_fallthrough; label_nulls++; }
  if (L_failure == NULL)   { L_failure   = &L_fallthrough; label_nulls++; }
  assert(label_nulls <= 1, "at most one NULL in the batch");

  // a couple of useful fields in sub_klass:
  int ss_offset = in_bytes(Klass::secondary_supers_offset());
  int sc_offset = in_bytes(Klass::secondary_super_cache_offset());

  // Do a linear scan of the secondary super-klass chain.
  // This code is rarely used, so simplicity is a virtue here.

#ifndef PRODUCT
  int* pst_counter = &SharedRuntime::_partial_subtype_ctr;
  inc_counter((address) pst_counter, count_temp, scan_temp);
#endif

  // We will consult the secondary-super array.
  ld_ptr(sub_klass, ss_offset, scan_temp);

  Register search_key = super_klass;

  // Load the array length.  (Positive movl does right thing on LP64.)
  lduw(scan_temp, Array<Klass*>::length_offset_in_bytes(), count_temp);

  // Check for empty secondary super list
  tst(count_temp);

  // In the array of super classes elements are pointer sized.
  int element_size = wordSize;

  // Top of search loop
  bind(L_loop);
  br(Assembler::equal, false, Assembler::pn, *L_failure);
  delayed()->add(scan_temp, element_size, scan_temp);

  // Skip the array header in all array accesses.
  int elem_offset = Array<Klass*>::base_offset_in_bytes();
  elem_offset -= element_size;   // the scan pointer was pre-incremented also

  // Load next super to check
    ld_ptr( scan_temp, elem_offset, scratch_reg );

  // Look for Rsuper_klass on Rsub_klass's secondary super-class-overflow list
  cmp(scratch_reg, search_key);

  // A miss means we are NOT a subtype and need to keep looping
  brx(Assembler::notEqual, false, Assembler::pn, L_loop);
  delayed()->deccc(count_temp); // decrement trip counter in delay slot

  // Success.  Cache the super we found and proceed in triumph.
  st_ptr(super_klass, sub_klass, sc_offset);

  if (L_success != &L_fallthrough) {
    ba(*L_success);
    delayed()->nop();
  }

  bind(L_fallthrough);
}


RegisterOrConstant MacroAssembler::argument_offset(RegisterOrConstant arg_slot,
                                                   Register temp_reg,
                                                   int extra_slot_offset) {
  // cf. TemplateTable::prepare_invoke(), if (load_receiver).
  int stackElementSize = Interpreter::stackElementSize;
  int offset = extra_slot_offset * stackElementSize;
  if (arg_slot.is_constant()) {
    offset += arg_slot.as_constant() * stackElementSize;
    return offset;
  } else {
    assert(temp_reg != noreg, "must specify");
    sll_ptr(arg_slot.as_register(), exact_log2(stackElementSize), temp_reg);
    if (offset != 0)
      add(temp_reg, offset, temp_reg);
    return temp_reg;
  }
}


Address MacroAssembler::argument_address(RegisterOrConstant arg_slot,
                                         Register temp_reg,
                                         int extra_slot_offset) {
  return Address(Gargs, argument_offset(arg_slot, temp_reg, extra_slot_offset));
}


void MacroAssembler::biased_locking_enter(Register obj_reg, Register mark_reg,
                                          Register temp_reg,
                                          Label& done, Label* slow_case,
                                          BiasedLockingCounters* counters) {
  assert(UseBiasedLocking, "why call this otherwise?");

  if (PrintBiasedLockingStatistics) {
    assert_different_registers(obj_reg, mark_reg, temp_reg, O7);
    if (counters == NULL)
      counters = BiasedLocking::counters();
  }

  Label cas_label;

  // Biased locking
  // See whether the lock is currently biased toward our thread and
  // whether the epoch is still valid
  // Note that the runtime guarantees sufficient alignment of JavaThread
  // pointers to allow age to be placed into low bits
  assert(markWord::age_shift == markWord::lock_bits + markWord::biased_lock_bits, "biased locking makes assumptions about bit layout");
  and3(mark_reg, markWord::biased_lock_mask_in_place, temp_reg);
  cmp_and_brx_short(temp_reg, markWord::biased_lock_pattern, Assembler::notEqual, Assembler::pn, cas_label);

  load_klass(obj_reg, temp_reg);
  ld_ptr(Address(temp_reg, Klass::prototype_header_offset()), temp_reg);
  or3(G2_thread, temp_reg, temp_reg);
  xor3(mark_reg, temp_reg, temp_reg);
  andcc(temp_reg, ~((int) markWord::age_mask_in_place), temp_reg);
  if (counters != NULL) {
    cond_inc(Assembler::equal, (address) counters->biased_lock_entry_count_addr(), mark_reg, temp_reg);
    // Reload mark_reg as we may need it later
    ld_ptr(Address(obj_reg, oopDesc::mark_offset_in_bytes()), mark_reg);
  }
  brx(Assembler::equal, true, Assembler::pt, done);
  delayed()->nop();

  Label try_revoke_bias;
  Label try_rebias;
  Address mark_addr = Address(obj_reg, oopDesc::mark_offset_in_bytes());
  assert(mark_addr.disp() == 0, "cas must take a zero displacement");

  // At this point we know that the header has the bias pattern and
  // that we are not the bias owner in the current epoch. We need to
  // figure out more details about the state of the header in order to
  // know what operations can be legally performed on the object's
  // header.

  // If the low three bits in the xor result aren't clear, that means
  // the prototype header is no longer biased and we have to revoke
  // the bias on this object.
  btst(markWord::biased_lock_mask_in_place, temp_reg);
  brx(Assembler::notZero, false, Assembler::pn, try_revoke_bias);

  // Biasing is still enabled for this data type. See whether the
  // epoch of the current bias is still valid, meaning that the epoch
  // bits of the mark word are equal to the epoch bits of the
  // prototype header. (Note that the prototype header's epoch bits
  // only change at a safepoint.) If not, attempt to rebias the object
  // toward the current thread. Note that we must be absolutely sure
  // that the current epoch is invalid in order to do this because
  // otherwise the manipulations it performs on the mark word are
  // illegal.
  delayed()->btst(markWord::epoch_mask_in_place, temp_reg);
  brx(Assembler::notZero, false, Assembler::pn, try_rebias);

  // The epoch of the current bias is still valid but we know nothing
  // about the owner; it might be set or it might be clear. Try to
  // acquire the bias of the object using an atomic operation. If this
  // fails we will go in to the runtime to revoke the object's bias.
  // Note that we first construct the presumed unbiased header so we
  // don't accidentally blow away another thread's valid bias.
  delayed()->and3(mark_reg,
                  markWord::biased_lock_mask_in_place | markWord::age_mask_in_place | markWord::epoch_mask_in_place,
                  mark_reg);
  or3(G2_thread, mark_reg, temp_reg);
  cas_ptr(mark_addr.base(), mark_reg, temp_reg);
  // If the biasing toward our thread failed, this means that
  // another thread succeeded in biasing it toward itself and we
  // need to revoke that bias. The revocation will occur in the
  // interpreter runtime in the slow case.
  cmp(mark_reg, temp_reg);
  if (counters != NULL) {
    cond_inc(Assembler::zero, (address) counters->anonymously_biased_lock_entry_count_addr(), mark_reg, temp_reg);
  }
  if (slow_case != NULL) {
    brx(Assembler::notEqual, true, Assembler::pn, *slow_case);
    delayed()->nop();
  }
  ba_short(done);

  bind(try_rebias);
  // At this point we know the epoch has expired, meaning that the
  // current "bias owner", if any, is actually invalid. Under these
  // circumstances _only_, we are allowed to use the current header's
  // value as the comparison value when doing the cas to acquire the
  // bias in the current epoch. In other words, we allow transfer of
  // the bias from one thread to another directly in this situation.
  //
  // FIXME: due to a lack of registers we currently blow away the age
  // bits in this situation. Should attempt to preserve them.
  load_klass(obj_reg, temp_reg);
  ld_ptr(Address(temp_reg, Klass::prototype_header_offset()), temp_reg);
  or3(G2_thread, temp_reg, temp_reg);
  cas_ptr(mark_addr.base(), mark_reg, temp_reg);
  // If the biasing toward our thread failed, this means that
  // another thread succeeded in biasing it toward itself and we
  // need to revoke that bias. The revocation will occur in the
  // interpreter runtime in the slow case.
  cmp(mark_reg, temp_reg);
  if (counters != NULL) {
    cond_inc(Assembler::zero, (address) counters->rebiased_lock_entry_count_addr(), mark_reg, temp_reg);
  }
  if (slow_case != NULL) {
    brx(Assembler::notEqual, true, Assembler::pn, *slow_case);
    delayed()->nop();
  }
  ba_short(done);

  bind(try_revoke_bias);
  // The prototype mark in the klass doesn't have the bias bit set any
  // more, indicating that objects of this data type are not supposed
  // to be biased any more. We are going to try to reset the mark of
  // this object to the prototype value and fall through to the
  // CAS-based locking scheme. Note that if our CAS fails, it means
  // that another thread raced us for the privilege of revoking the
  // bias of this particular object, so it's okay to continue in the
  // normal locking code.
  //
  // FIXME: due to a lack of registers we currently blow away the age
  // bits in this situation. Should attempt to preserve them.
  load_klass(obj_reg, temp_reg);
  ld_ptr(Address(temp_reg, Klass::prototype_header_offset()), temp_reg);
  cas_ptr(mark_addr.base(), mark_reg, temp_reg);
  // Fall through to the normal CAS-based lock, because no matter what
  // the result of the above CAS, some thread must have succeeded in
  // removing the bias bit from the object's header.
  if (counters != NULL) {
    cmp(mark_reg, temp_reg);
    cond_inc(Assembler::zero, (address) counters->revoked_lock_entry_count_addr(), mark_reg, temp_reg);
  }

  bind(cas_label);
}

void MacroAssembler::biased_locking_exit (Address mark_addr, Register temp_reg, Label& done,
                                          bool allow_delay_slot_filling) {
  // Check for biased locking unlock case, which is a no-op
  // Note: we do not have to check the thread ID for two reasons.
  // First, the interpreter checks for IllegalMonitorStateException at
  // a higher level. Second, if the bias was revoked while we held the
  // lock, the object could not be rebiased toward another thread, so
  // the bias bit would be clear.
  ld_ptr(mark_addr, temp_reg);
  and3(temp_reg, markWord::biased_lock_mask_in_place, temp_reg);
  cmp(temp_reg, markWord::biased_lock_pattern);
  brx(Assembler::equal, allow_delay_slot_filling, Assembler::pt, done);
  delayed();
  if (!allow_delay_slot_filling) {
    nop();
  }
}


// compiler_lock_object() and compiler_unlock_object() are direct transliterations
// of i486.ad fast_lock() and fast_unlock().  See those methods for detailed comments.
// The code could be tightened up considerably.
//
// box->dhw disposition - post-conditions at DONE_LABEL.
// -   Successful inflated lock:  box->dhw != 0.
//     Any non-zero value suffices.
//     Consider G2_thread, rsp, boxReg, or markWord::unused_mark()
// -   Successful Stack-lock: box->dhw == mark.
//     box->dhw must contain the displaced mark word value
// -   Failure -- icc.ZFlag == 0 and box->dhw is undefined.
//     The slow-path fast_enter() and slow_enter() operators
//     are responsible for setting box->dhw = NonZero (typically markWord::unused_mark()).
// -   Biased: box->dhw is undefined
//
// SPARC refworkload performance - specifically jetstream and scimark - are
// extremely sensitive to the size of the code emitted by compiler_lock_object
// and compiler_unlock_object.  Critically, the key factor is code size, not path
// length.  (Simply experiments to pad CLO with unexecuted NOPs demonstrte the
// effect).


void MacroAssembler::compiler_lock_object(Register Roop, Register Rmark,
                                          Register Rbox, Register Rscratch,
                                          BiasedLockingCounters* counters,
                                          bool try_bias) {
   Address mark_addr(Roop, oopDesc::mark_offset_in_bytes());

   verify_oop(Roop);
   Label done ;

   if (counters != NULL) {
     inc_counter((address) counters->total_entry_count_addr(), Rmark, Rscratch);
   }

   // Aggressively avoid the Store-before-CAS penalty
   // Defer the store into box->dhw until after the CAS
   Label IsInflated, Recursive ;

// Anticipate CAS -- Avoid RTS->RTO upgrade
// prefetch (mark_addr, Assembler::severalWritesAndPossiblyReads);

   ld_ptr(mark_addr, Rmark);           // fetch obj->mark
   // Triage: biased, stack-locked, neutral, inflated

   if (try_bias) {
     biased_locking_enter(Roop, Rmark, Rscratch, done, NULL, counters);
     // Invariant: if control reaches this point in the emitted stream
     // then Rmark has not been modified.
   }
   andcc(Rmark, 2, G0);
   brx(Assembler::notZero, false, Assembler::pn, IsInflated);
   delayed()->                         // Beware - dangling delay-slot

   // Try stack-lock acquisition.
   // Transiently install BUSY (0) encoding in the mark word.
   // if the CAS of 0 into the mark was successful then we execute:
   //   ST box->dhw  = mark   -- save fetched mark in on-stack basiclock box
   //   ST obj->mark = box    -- overwrite transient 0 value
   // This presumes TSO, of course.

   mov(0, Rscratch);
   or3(Rmark, markWord::unlocked_value, Rmark);
   assert(mark_addr.disp() == 0, "cas must take a zero displacement");
   cas_ptr(mark_addr.base(), Rmark, Rscratch);
// prefetch (mark_addr, Assembler::severalWritesAndPossiblyReads);
   cmp(Rscratch, Rmark);
   brx(Assembler::notZero, false, Assembler::pn, Recursive);
   delayed()->st_ptr(Rmark, Rbox, BasicLock::displaced_header_offset_in_bytes());
   if (counters != NULL) {
     cond_inc(Assembler::equal, (address) counters->fast_path_entry_count_addr(), Rmark, Rscratch);
   }
   ba(done);
   delayed()->st_ptr(Rbox, mark_addr);

   bind(Recursive);
   // Stack-lock attempt failed - check for recursive stack-lock.
   // Tests show that we can remove the recursive case with no impact
   // on refworkload 0.83.  If we need to reduce the size of the code
   // emitted by compiler_lock_object() the recursive case is perfect
   // candidate.
   //
   // A more extreme idea is to always inflate on stack-lock recursion.
   // This lets us eliminate the recursive checks in compiler_lock_object
   // and compiler_unlock_object and the (box->dhw == 0) encoding.
   // A brief experiment - requiring changes to synchronizer.cpp, interpreter,
   // and showed a performance *increase*.  In the same experiment I eliminated
   // the fast-path stack-lock code from the interpreter and always passed
   // control to the "slow" operators in synchronizer.cpp.

   // RScratch contains the fetched obj->mark value from the failed CAS.
   sub(Rscratch, STACK_BIAS, Rscratch);
   sub(Rscratch, SP, Rscratch);
   assert(os::vm_page_size() > 0xfff, "page size too small - change the constant");
   andcc(Rscratch, 0xfffff003, Rscratch);
   if (counters != NULL) {
     // Accounting needs the Rscratch register
     st_ptr(Rscratch, Rbox, BasicLock::displaced_header_offset_in_bytes());
     cond_inc(Assembler::equal, (address) counters->fast_path_entry_count_addr(), Rmark, Rscratch);
     ba_short(done);
   } else {
     ba(done);
     delayed()->st_ptr(Rscratch, Rbox, BasicLock::displaced_header_offset_in_bytes());
   }

   bind   (IsInflated);

   // Try to CAS m->owner from null to Self
   // Invariant: if we acquire the lock then _recursions should be 0.
   add(Rmark, OM_OFFSET_NO_MONITOR_VALUE_TAG(owner), Rmark);
   mov(G2_thread, Rscratch);
   cas_ptr(Rmark, G0, Rscratch);
   andcc(Rscratch, Rscratch, G0);             // set ICCs for done: icc.zf iff success
   // set icc.zf : 1=success 0=failure
   // ST box->displaced_header = NonZero.
   // Any non-zero value suffices:
   //    markWord::unused_mark(), G2_thread, RBox, RScratch, rsp, etc.
   st_ptr(Rbox, Rbox, BasicLock::displaced_header_offset_in_bytes());
   // Intentional fall-through into done

   bind   (done);
}

void MacroAssembler::compiler_unlock_object(Register Roop, Register Rmark,
                                            Register Rbox, Register Rscratch,
                                            bool try_bias) {
   Address mark_addr(Roop, oopDesc::mark_offset_in_bytes());

   Label done ;

   // Beware ... If the aggregate size of the code emitted by CLO and CUO is
   // is too large performance rolls abruptly off a cliff.
   // This could be related to inlining policies, code cache management, or
   // I$ effects.
   Label LStacked ;

   if (try_bias) {
      // TODO: eliminate redundant LDs of obj->mark
      biased_locking_exit(mark_addr, Rscratch, done);
   }

   ld_ptr(Roop, oopDesc::mark_offset_in_bytes(), Rmark);
   ld_ptr(Rbox, BasicLock::displaced_header_offset_in_bytes(), Rscratch);
   andcc(Rscratch, Rscratch, G0);
   brx(Assembler::zero, false, Assembler::pn, done);
   delayed()->nop();      // consider: relocate fetch of mark, above, into this DS
   andcc(Rmark, 2, G0);
   brx(Assembler::zero, false, Assembler::pt, LStacked);
   delayed()->nop();

   // It's inflated
   // Conceptually we need a #loadstore|#storestore "release" MEMBAR before
   // the ST of 0 into _owner which releases the lock.  This prevents loads
   // and stores within the critical section from reordering (floating)
   // past the store that releases the lock.  But TSO is a strong memory model
   // and that particular flavor of barrier is a noop, so we can safely elide it.
   // Note that we use 1-0 locking by default for the inflated case.  We
   // close the resultant (and rare) race by having contended threads in
   // monitorenter periodically poll _owner.

   // 1-0 form : avoids CAS and MEMBAR in the common case
   // Do not bother to ratify that m->Owner == Self.
   ld_ptr(Address(Rmark, OM_OFFSET_NO_MONITOR_VALUE_TAG(recursions)), Rbox);
   orcc(Rbox, G0, G0);
   brx(Assembler::notZero, false, Assembler::pn, done);
   delayed()->
   ld_ptr(Address(Rmark, OM_OFFSET_NO_MONITOR_VALUE_TAG(EntryList)), Rscratch);
   ld_ptr(Address(Rmark, OM_OFFSET_NO_MONITOR_VALUE_TAG(cxq)), Rbox);
   orcc(Rbox, Rscratch, G0);
   brx(Assembler::zero, false, Assembler::pt, done);
   delayed()->
   st_ptr(G0, Address(Rmark, OM_OFFSET_NO_MONITOR_VALUE_TAG(owner)));

   membar(StoreLoad);
   // Check that _succ is (or remains) non-zero
   ld_ptr(Address(Rmark, OM_OFFSET_NO_MONITOR_VALUE_TAG(succ)), Rscratch);
   andcc(Rscratch, Rscratch, G0);
   brx(Assembler::notZero, false, Assembler::pt, done);
   delayed()->andcc(G0, G0, G0);
   add(Rmark, OM_OFFSET_NO_MONITOR_VALUE_TAG(owner), Rmark);
   mov(G2_thread, Rscratch);
   cas_ptr(Rmark, G0, Rscratch);
   cmp(Rscratch, G0);
   // invert icc.zf and goto done
   // A slightly better v8+/v9 idiom would be the following:
   //   movrnz Rscratch,1,Rscratch
   //   ba done
   //   xorcc Rscratch,1,G0
   // In v8+ mode the idiom would be valid IFF Rscratch was a G or O register
   brx(Assembler::notZero, false, Assembler::pt, done);
   delayed()->cmp(G0, G0);
   br(Assembler::always, false, Assembler::pt, done);
   delayed()->cmp(G0, 1);

   bind   (LStacked);
   // Consider: we could replace the expensive CAS in the exit
   // path with a simple ST of the displaced mark value fetched from
   // the on-stack basiclock box.  That admits a race where a thread T2
   // in the slow lock path -- inflating with monitor M -- could race a
   // thread T1 in the fast unlock path, resulting in a missed wakeup for T2.
   // More precisely T1 in the stack-lock unlock path could "stomp" the
   // inflated mark value M installed by T2, resulting in an orphan
   // object monitor M and T2 becoming stranded.  We can remedy that situation
   // by having T2 periodically poll the object's mark word using timed wait
   // operations.  If T2 discovers that a stomp has occurred it vacates
   // the monitor M and wakes any other threads stranded on the now-orphan M.
   // In addition the monitor scavenger, which performs deflation,
   // would also need to check for orpan monitors and stranded threads.
   //
   // Finally, inflation is also used when T2 needs to assign a hashCode
   // to O and O is stack-locked by T1.  The "stomp" race could cause
   // an assigned hashCode value to be lost.  We can avoid that condition
   // and provide the necessary hashCode stability invariants by ensuring
   // that hashCode generation is idempotent between copying GCs.
   // For example we could compute the hashCode of an object O as
   // O's heap address XOR some high quality RNG value that is refreshed
   // at GC-time.  The monitor scavenger would install the hashCode
   // found in any orphan monitors.  Again, the mechanism admits a
   // lost-update "stomp" WAW race but detects and recovers as needed.
   //
   // A prototype implementation showed excellent results, although
   // the scavenger and timeout code was rather involved.

   cas_ptr(mark_addr.base(), Rbox, Rscratch);
   cmp(Rbox, Rscratch);
   // Intentional fall through into done ...

   bind(done);
}



void MacroAssembler::print_CPU_state() {
  // %%%%% need to implement this
}

void MacroAssembler::verify_FPU(int stack_depth, const char* s) {
  // %%%%% need to implement this
}

void MacroAssembler::push_IU_state() {
  // %%%%% need to implement this
}


void MacroAssembler::pop_IU_state() {
  // %%%%% need to implement this
}


void MacroAssembler::push_FPU_state() {
  // %%%%% need to implement this
}


void MacroAssembler::pop_FPU_state() {
  // %%%%% need to implement this
}


void MacroAssembler::push_CPU_state() {
  // %%%%% need to implement this
}


void MacroAssembler::pop_CPU_state() {
  // %%%%% need to implement this
}



void MacroAssembler::verify_tlab() {
#ifdef ASSERT
  if (UseTLAB && VerifyOops) {
    Label next, next2, ok;
    Register t1 = L0;
    Register t2 = L1;
    Register t3 = L2;

    save_frame(0);
    ld_ptr(G2_thread, in_bytes(JavaThread::tlab_top_offset()), t1);
    ld_ptr(G2_thread, in_bytes(JavaThread::tlab_start_offset()), t2);
    or3(t1, t2, t3);
    cmp_and_br_short(t1, t2, Assembler::greaterEqual, Assembler::pn, next);
    STOP("assert(top >= start)");
    should_not_reach_here();

    bind(next);
    ld_ptr(G2_thread, in_bytes(JavaThread::tlab_top_offset()), t1);
    ld_ptr(G2_thread, in_bytes(JavaThread::tlab_end_offset()), t2);
    or3(t3, t2, t3);
    cmp_and_br_short(t1, t2, Assembler::lessEqual, Assembler::pn, next2);
    STOP("assert(top <= end)");
    should_not_reach_here();

    bind(next2);
    and3(t3, MinObjAlignmentInBytesMask, t3);
    cmp_and_br_short(t3, 0, Assembler::lessEqual, Assembler::pn, ok);
    STOP("assert(aligned)");
    should_not_reach_here();

    bind(ok);
    restore();
  }
#endif
}


void MacroAssembler::eden_allocate(
  Register obj,                        // result: pointer to object after successful allocation
  Register var_size_in_bytes,          // object size in bytes if unknown at compile time; invalid otherwise
  int      con_size_in_bytes,          // object size in bytes if   known at compile time
  Register t1,                         // temp register
  Register t2,                         // temp register
  Label&   slow_case                   // continuation point if fast allocation fails
){
  // make sure arguments make sense
  assert_different_registers(obj, var_size_in_bytes, t1, t2);
  assert(0 <= con_size_in_bytes && Assembler::is_simm13(con_size_in_bytes), "illegal object size");
  assert((con_size_in_bytes & MinObjAlignmentInBytesMask) == 0, "object size is not multiple of alignment");

  if (!Universe::heap()->supports_inline_contig_alloc()) {
    // No allocation in the shared eden.
    ba(slow_case);
    delayed()->nop();
  } else {
    // get eden boundaries
    // note: we need both top & top_addr!
    const Register top_addr = t1;
    const Register end      = t2;

    CollectedHeap* ch = Universe::heap();
    set((intx)ch->top_addr(), top_addr);
    intx delta = (intx)ch->end_addr() - (intx)ch->top_addr();
    ld_ptr(top_addr, delta, end);
    ld_ptr(top_addr, 0, obj);

    // try to allocate
    Label retry;
    bind(retry);
#ifdef ASSERT
    // make sure eden top is properly aligned
    {
      Label L;
      btst(MinObjAlignmentInBytesMask, obj);
      br(Assembler::zero, false, Assembler::pt, L);
      delayed()->nop();
      STOP("eden top is not properly aligned");
      bind(L);
    }
#endif // ASSERT
    const Register free = end;
    sub(end, obj, free);                                   // compute amount of free space
    if (var_size_in_bytes->is_valid()) {
      // size is unknown at compile time
      cmp(free, var_size_in_bytes);
      brx(Assembler::lessUnsigned, false, Assembler::pn, slow_case); // if there is not enough space go the slow case
      delayed()->add(obj, var_size_in_bytes, end);
    } else {
      // size is known at compile time
      cmp(free, con_size_in_bytes);
      brx(Assembler::lessUnsigned, false, Assembler::pn, slow_case); // if there is not enough space go the slow case
      delayed()->add(obj, con_size_in_bytes, end);
    }
    // Compare obj with the value at top_addr; if still equal, swap the value of
    // end with the value at top_addr. If not equal, read the value at top_addr
    // into end.
    cas_ptr(top_addr, obj, end);
    // if someone beat us on the allocation, try again, otherwise continue
    cmp(obj, end);
    brx(Assembler::notEqual, false, Assembler::pn, retry);
    delayed()->mov(end, obj);                              // nop if successfull since obj == end

#ifdef ASSERT
    // make sure eden top is properly aligned
    {
      Label L;
      const Register top_addr = t1;

      set((intx)ch->top_addr(), top_addr);
      ld_ptr(top_addr, 0, top_addr);
      btst(MinObjAlignmentInBytesMask, top_addr);
      br(Assembler::zero, false, Assembler::pt, L);
      delayed()->nop();
      STOP("eden top is not properly aligned");
      bind(L);
    }
#endif // ASSERT
  }
}


void MacroAssembler::tlab_allocate(
  Register obj,                        // result: pointer to object after successful allocation
  Register var_size_in_bytes,          // object size in bytes if unknown at compile time; invalid otherwise
  int      con_size_in_bytes,          // object size in bytes if   known at compile time
  Register t1,                         // temp register
  Label&   slow_case                   // continuation point if fast allocation fails
){
  // make sure arguments make sense
  assert_different_registers(obj, var_size_in_bytes, t1);
  assert(0 <= con_size_in_bytes && is_simm13(con_size_in_bytes), "illegal object size");
  assert((con_size_in_bytes & MinObjAlignmentInBytesMask) == 0, "object size is not multiple of alignment");

  const Register free  = t1;

  verify_tlab();

  ld_ptr(G2_thread, in_bytes(JavaThread::tlab_top_offset()), obj);

  // calculate amount of free space
  ld_ptr(G2_thread, in_bytes(JavaThread::tlab_end_offset()), free);
  sub(free, obj, free);

  Label done;
  if (var_size_in_bytes == noreg) {
    cmp(free, con_size_in_bytes);
  } else {
    cmp(free, var_size_in_bytes);
  }
  br(Assembler::less, false, Assembler::pn, slow_case);
  // calculate the new top pointer
  if (var_size_in_bytes == noreg) {
    delayed()->add(obj, con_size_in_bytes, free);
  } else {
    delayed()->add(obj, var_size_in_bytes, free);
  }

  bind(done);

#ifdef ASSERT
  // make sure new free pointer is properly aligned
  {
    Label L;
    btst(MinObjAlignmentInBytesMask, free);
    br(Assembler::zero, false, Assembler::pt, L);
    delayed()->nop();
    STOP("updated TLAB free is not properly aligned");
    bind(L);
  }
#endif // ASSERT

  // update the tlab top pointer
  st_ptr(free, G2_thread, in_bytes(JavaThread::tlab_top_offset()));
  verify_tlab();
}

void MacroAssembler::zero_memory(Register base, Register index) {
  assert_different_registers(base, index);
  Label loop;
  bind(loop);
  subcc(index, HeapWordSize, index);
  brx(Assembler::greaterEqual, true, Assembler::pt, loop);
  delayed()->st_ptr(G0, base, index);
}

void MacroAssembler::incr_allocated_bytes(RegisterOrConstant size_in_bytes,
                                          Register t1, Register t2) {
  // Bump total bytes allocated by this thread
  assert(t1->is_global(), "must be global reg"); // so all 64 bits are saved on a context switch
  assert_different_registers(size_in_bytes.register_or_noreg(), t1, t2);
  // v8 support has gone the way of the dodo
  ldx(G2_thread, in_bytes(JavaThread::allocated_bytes_offset()), t1);
  add(t1, ensure_simm13_or_reg(size_in_bytes, t2), t1);
  stx(t1, G2_thread, in_bytes(JavaThread::allocated_bytes_offset()));
}

Assembler::Condition MacroAssembler::negate_condition(Assembler::Condition cond) {
  switch (cond) {
    // Note some conditions are synonyms for others
    case Assembler::never:                return Assembler::always;
    case Assembler::zero:                 return Assembler::notZero;
    case Assembler::lessEqual:            return Assembler::greater;
    case Assembler::less:                 return Assembler::greaterEqual;
    case Assembler::lessEqualUnsigned:    return Assembler::greaterUnsigned;
    case Assembler::lessUnsigned:         return Assembler::greaterEqualUnsigned;
    case Assembler::negative:             return Assembler::positive;
    case Assembler::overflowSet:          return Assembler::overflowClear;
    case Assembler::always:               return Assembler::never;
    case Assembler::notZero:              return Assembler::zero;
    case Assembler::greater:              return Assembler::lessEqual;
    case Assembler::greaterEqual:         return Assembler::less;
    case Assembler::greaterUnsigned:      return Assembler::lessEqualUnsigned;
    case Assembler::greaterEqualUnsigned: return Assembler::lessUnsigned;
    case Assembler::positive:             return Assembler::negative;
    case Assembler::overflowClear:        return Assembler::overflowSet;
  }

  ShouldNotReachHere(); return Assembler::overflowClear;
}

void MacroAssembler::cond_inc(Assembler::Condition cond, address counter_ptr,
                              Register Rtmp1, Register Rtmp2 /*, Register Rtmp3, Register Rtmp4 */) {
  Condition negated_cond = negate_condition(cond);
  Label L;
  brx(negated_cond, false, Assembler::pt, L);
  delayed()->nop();
  inc_counter(counter_ptr, Rtmp1, Rtmp2);
  bind(L);
}

void MacroAssembler::inc_counter(address counter_addr, Register Rtmp1, Register Rtmp2) {
  AddressLiteral addrlit(counter_addr);
  sethi(addrlit, Rtmp1);                 // Move hi22 bits into temporary register.
  Address addr(Rtmp1, addrlit.low10());  // Build an address with low10 bits.
  ld(addr, Rtmp2);
  inc(Rtmp2);
  st(Rtmp2, addr);
}

void MacroAssembler::inc_counter(int* counter_addr, Register Rtmp1, Register Rtmp2) {
  inc_counter((address) counter_addr, Rtmp1, Rtmp2);
}

SkipIfEqual::SkipIfEqual(
    MacroAssembler* masm, Register temp, const bool* flag_addr,
    Assembler::Condition condition) {
  _masm = masm;
  AddressLiteral flag(flag_addr);
  _masm->sethi(flag, temp);
  _masm->ldub(temp, flag.low10(), temp);
  _masm->tst(temp);
  _masm->br(condition, false, Assembler::pt, _label);
  _masm->delayed()->nop();
}

SkipIfEqual::~SkipIfEqual() {
  _masm->bind(_label);
}

void MacroAssembler::bang_stack_with_offset(int offset) {
  // stack grows down, caller passes positive offset
  assert(offset > 0, "must bang with negative offset");
  set((-offset)+STACK_BIAS, G3_scratch);
  st(G0, SP, G3_scratch);
}

// Writes to stack successive pages until offset reached to check for
// stack overflow + shadow pages.  This clobbers tsp and scratch.
void MacroAssembler::bang_stack_size(Register Rsize, Register Rtsp,
                                     Register Rscratch) {
  // Use stack pointer in temp stack pointer
  mov(SP, Rtsp);

  // Bang stack for total size given plus stack shadow page size.
  // Bang one page at a time because a large size can overflow yellow and
  // red zones (the bang will fail but stack overflow handling can't tell that
  // it was a stack overflow bang vs a regular segv).
  int offset = os::vm_page_size();
  Register Roffset = Rscratch;

  Label loop;
  bind(loop);
  set((-offset)+STACK_BIAS, Rscratch);
  st(G0, Rtsp, Rscratch);
  set(offset, Roffset);
  sub(Rsize, Roffset, Rsize);
  cmp(Rsize, G0);
  br(Assembler::greater, false, Assembler::pn, loop);
  delayed()->sub(Rtsp, Roffset, Rtsp);

  // Bang down shadow pages too.
  // At this point, (tmp-0) is the last address touched, so don't
  // touch it again.  (It was touched as (tmp-pagesize) but then tmp
  // was post-decremented.)  Skip this address by starting at i=1, and
  // touch a few more pages below.  N.B.  It is important to touch all
  // the way down to and including i=StackShadowPages.
  for (int i = 1; i < JavaThread::stack_shadow_zone_size() / os::vm_page_size(); i++) {
    set((-i*offset)+STACK_BIAS, Rscratch);
    st(G0, Rtsp, Rscratch);
  }
}

void MacroAssembler::reserved_stack_check() {
  // testing if reserved zone needs to be enabled
  Label no_reserved_zone_enabling;

  ld_ptr(G2_thread, JavaThread::reserved_stack_activation_offset(), G4_scratch);
  cmp_and_brx_short(SP, G4_scratch, Assembler::lessUnsigned, Assembler::pt, no_reserved_zone_enabling);

  call_VM_leaf(L0, CAST_FROM_FN_PTR(address, SharedRuntime::enable_stack_reserved_zone), G2_thread);

  AddressLiteral stub(StubRoutines::throw_delayed_StackOverflowError_entry());
  jump_to(stub, G4_scratch);
  delayed()->restore();

  should_not_reach_here();

  bind(no_reserved_zone_enabling);
}
// ((OopHandle)result).resolve();
void MacroAssembler::resolve_oop_handle(Register result, Register tmp) {
  // OopHandle::resolve is an indirection.
  access_load_at(T_OBJECT, IN_NATIVE, Address(result, 0), result, tmp);
}

void MacroAssembler::load_mirror(Register mirror, Register method, Register tmp) {
  const int mirror_offset = in_bytes(Klass::java_mirror_offset());
  ld_ptr(method, in_bytes(Method::const_offset()), mirror);
  ld_ptr(mirror, in_bytes(ConstMethod::constants_offset()), mirror);
  ld_ptr(mirror, ConstantPool::pool_holder_offset_in_bytes(), mirror);
  ld_ptr(mirror, mirror_offset, mirror);
  resolve_oop_handle(mirror, tmp);
}

void MacroAssembler::load_klass(Register src_oop, Register klass) {
  // The number of bytes in this code is used by
  // MachCallDynamicJavaNode::ret_addr_offset()
  // if this changes, change that.
  if (UseCompressedClassPointers) {
    lduw(src_oop, oopDesc::klass_offset_in_bytes(), klass);
    decode_klass_not_null(klass);
  } else {
    ld_ptr(src_oop, oopDesc::klass_offset_in_bytes(), klass);
  }
}

void MacroAssembler::store_klass(Register klass, Register dst_oop) {
  if (UseCompressedClassPointers) {
    assert(dst_oop != klass, "not enough registers");
    encode_klass_not_null(klass);
    st(klass, dst_oop, oopDesc::klass_offset_in_bytes());
  } else {
    st_ptr(klass, dst_oop, oopDesc::klass_offset_in_bytes());
  }
}

void MacroAssembler::store_klass_gap(Register s, Register d) {
  if (UseCompressedClassPointers) {
    assert(s != d, "not enough registers");
    st(s, d, oopDesc::klass_gap_offset_in_bytes());
  }
}

void MacroAssembler::access_store_at(BasicType type, DecoratorSet decorators,
                                     Register src, Address dst, Register tmp) {
  BarrierSetAssembler* bs = BarrierSet::barrier_set()->barrier_set_assembler();
  decorators = AccessInternal::decorator_fixup(decorators);
  bool as_raw = (decorators & AS_RAW) != 0;
  if (as_raw) {
    bs->BarrierSetAssembler::store_at(this, decorators, type, src, dst, tmp);
  } else {
    bs->store_at(this, decorators, type, src, dst, tmp);
  }
}

void MacroAssembler::access_load_at(BasicType type, DecoratorSet decorators,
                                    Address src, Register dst, Register tmp) {
  BarrierSetAssembler* bs = BarrierSet::barrier_set()->barrier_set_assembler();
  decorators = AccessInternal::decorator_fixup(decorators);
  bool as_raw = (decorators & AS_RAW) != 0;
  if (as_raw) {
    bs->BarrierSetAssembler::load_at(this, decorators, type, src, dst, tmp);
  } else {
    bs->load_at(this, decorators, type, src, dst, tmp);
  }
}

void MacroAssembler::load_heap_oop(const Address& s, Register d, Register tmp, DecoratorSet decorators) {
  access_load_at(T_OBJECT, IN_HEAP | decorators, s, d, tmp);
}

void MacroAssembler::load_heap_oop(Register s1, Register s2, Register d, Register tmp, DecoratorSet decorators) {
  access_load_at(T_OBJECT, IN_HEAP | decorators, Address(s1, s2), d, tmp);
}

void MacroAssembler::load_heap_oop(Register s1, int simm13a, Register d, Register tmp, DecoratorSet decorators) {
  access_load_at(T_OBJECT, IN_HEAP | decorators, Address(s1, simm13a), d, tmp);
}

void MacroAssembler::load_heap_oop(Register s1, RegisterOrConstant s2, Register d, Register tmp, DecoratorSet decorators) {
  if (s2.is_constant()) {
    access_load_at(T_OBJECT, IN_HEAP | decorators, Address(s1, s2.as_constant()), d, tmp);
  } else {
    access_load_at(T_OBJECT, IN_HEAP | decorators, Address(s1, s2.as_register()), d, tmp);
  }
}

void MacroAssembler::store_heap_oop(Register d, Register s1, Register s2, Register tmp, DecoratorSet decorators) {
  access_store_at(T_OBJECT, IN_HEAP | decorators, d, Address(s1, s2), tmp);
}

void MacroAssembler::store_heap_oop(Register d, Register s1, int simm13a, Register tmp, DecoratorSet decorators) {
  access_store_at(T_OBJECT, IN_HEAP | decorators, d, Address(s1, simm13a), tmp);
}

void MacroAssembler::store_heap_oop(Register d, const Address& a, int offset, Register tmp, DecoratorSet decorators) {
  if (a.has_index()) {
    assert(!a.has_disp(), "not supported yet");
    assert(offset == 0, "not supported yet");
    access_store_at(T_OBJECT, IN_HEAP | decorators, d, Address(a.base(), a.index()), tmp);
  } else {
    access_store_at(T_OBJECT, IN_HEAP | decorators, d, Address(a.base(), a.disp() + offset), tmp);
  }
}


void MacroAssembler::encode_heap_oop(Register src, Register dst) {
  assert (UseCompressedOops, "must be compressed");
  assert (Universe::heap() != NULL, "java heap should be initialized");
  assert (LogMinObjAlignmentInBytes == CompressedOops::shift(), "decode alg wrong");
  verify_oop(src);
  if (CompressedOops::base() == NULL) {
    srlx(src, LogMinObjAlignmentInBytes, dst);
    return;
  }
  Label done;
  if (src == dst) {
    // optimize for frequent case src == dst
    bpr(rc_nz, true, Assembler::pt, src, done);
    delayed() -> sub(src, G6_heapbase, dst); // annuled if not taken
    bind(done);
    srlx(src, LogMinObjAlignmentInBytes, dst);
  } else {
    bpr(rc_z, false, Assembler::pn, src, done);
    delayed() -> mov(G0, dst);
    // could be moved before branch, and annulate delay,
    // but may add some unneeded work decoding null
    sub(src, G6_heapbase, dst);
    srlx(dst, LogMinObjAlignmentInBytes, dst);
    bind(done);
  }
}


void MacroAssembler::encode_heap_oop_not_null(Register r) {
  assert (UseCompressedOops, "must be compressed");
  assert (Universe::heap() != NULL, "java heap should be initialized");
  assert (LogMinObjAlignmentInBytes == CompressedOops::shift(), "decode alg wrong");
  verify_oop(r);
  if (CompressedOops::base() != NULL)
    sub(r, G6_heapbase, r);
  srlx(r, LogMinObjAlignmentInBytes, r);
}

void MacroAssembler::encode_heap_oop_not_null(Register src, Register dst) {
  assert (UseCompressedOops, "must be compressed");
  assert (Universe::heap() != NULL, "java heap should be initialized");
  assert (LogMinObjAlignmentInBytes == CompressedOops::shift(), "decode alg wrong");
  verify_oop(src);
  if (CompressedOops::base() == NULL) {
    srlx(src, LogMinObjAlignmentInBytes, dst);
  } else {
    sub(src, G6_heapbase, dst);
    srlx(dst, LogMinObjAlignmentInBytes, dst);
  }
}

// Same algorithm as oops.inline.hpp decode_heap_oop.
void  MacroAssembler::decode_heap_oop(Register src, Register dst) {
  assert (UseCompressedOops, "must be compressed");
  assert (Universe::heap() != NULL, "java heap should be initialized");
  assert (LogMinObjAlignmentInBytes == CompressedOops::shift(), "decode alg wrong");
  sllx(src, LogMinObjAlignmentInBytes, dst);
  if (CompressedOops::base() != NULL) {
    Label done;
    bpr(rc_nz, true, Assembler::pt, dst, done);
    delayed() -> add(dst, G6_heapbase, dst); // annuled if not taken
    bind(done);
  }
  verify_oop(dst);
}

void  MacroAssembler::decode_heap_oop_not_null(Register r) {
  // Do not add assert code to this unless you change vtableStubs_sparc.cpp
  // pd_code_size_limit.
  // Also do not verify_oop as this is called by verify_oop.
  assert (UseCompressedOops, "must be compressed");
  assert (Universe::heap() != NULL, "java heap should be initialized");
  assert (LogMinObjAlignmentInBytes == CompressedOops::shift(), "decode alg wrong");
  sllx(r, LogMinObjAlignmentInBytes, r);
  if (CompressedOops::base() != NULL)
    add(r, G6_heapbase, r);
}

void  MacroAssembler::decode_heap_oop_not_null(Register src, Register dst) {
  // Do not add assert code to this unless you change vtableStubs_sparc.cpp
  // pd_code_size_limit.
  // Also do not verify_oop as this is called by verify_oop.
  assert (UseCompressedOops, "must be compressed");
  assert (LogMinObjAlignmentInBytes == CompressedOops::shift(), "decode alg wrong");
  sllx(src, LogMinObjAlignmentInBytes, dst);
  if (CompressedOops::base() != NULL)
    add(dst, G6_heapbase, dst);
}

void MacroAssembler::encode_klass_not_null(Register r) {
  assert (UseCompressedClassPointers, "must be compressed");
  if (CompressedKlassPointers::base() != NULL) {
    assert(r != G6_heapbase, "bad register choice");
    set((intptr_t)CompressedKlassPointers::base(), G6_heapbase);
    sub(r, G6_heapbase, r);
    if (CompressedKlassPointers::shift() != 0) {
      assert (LogKlassAlignmentInBytes == CompressedKlassPointers::shift(), "decode alg wrong");
      srlx(r, LogKlassAlignmentInBytes, r);
    }
    reinit_heapbase();
  } else {
    assert (LogKlassAlignmentInBytes == CompressedKlassPointers::shift() || CompressedKlassPointers::shift() == 0, "decode alg wrong");
    srlx(r, CompressedKlassPointers::shift(), r);
  }
}

void MacroAssembler::encode_klass_not_null(Register src, Register dst) {
  if (src == dst) {
    encode_klass_not_null(src);
  } else {
    assert (UseCompressedClassPointers, "must be compressed");
    if (CompressedKlassPointers::base() != NULL) {
      set((intptr_t)CompressedKlassPointers::base(), dst);
      sub(src, dst, dst);
      if (CompressedKlassPointers::shift() != 0) {
        srlx(dst, LogKlassAlignmentInBytes, dst);
      }
    } else {
      // shift src into dst
      assert (LogKlassAlignmentInBytes == CompressedKlassPointers::shift() || CompressedKlassPointers::shift() == 0, "decode alg wrong");
      srlx(src, CompressedKlassPointers::shift(), dst);
    }
  }
}

// Function instr_size_for_decode_klass_not_null() counts the instructions
// generated by decode_klass_not_null() and reinit_heapbase().  Hence, if
// the instructions they generate change, then this method needs to be updated.
int MacroAssembler::instr_size_for_decode_klass_not_null() {
  assert (UseCompressedClassPointers, "only for compressed klass ptrs");
  int num_instrs = 1;  // shift src,dst or add
  if (CompressedKlassPointers::base() != NULL) {
    // set + add + set
    num_instrs += insts_for_internal_set((intptr_t)CompressedKlassPointers::base()) +
                  insts_for_internal_set((intptr_t)CompressedOops::ptrs_base());
    if (CompressedKlassPointers::shift() != 0) {
      num_instrs += 1;  // sllx
    }
  }
  return num_instrs * BytesPerInstWord;
}

// !!! If the instructions that get generated here change then function
// instr_size_for_decode_klass_not_null() needs to get updated.
void  MacroAssembler::decode_klass_not_null(Register r) {
  // Do not add assert code to this unless you change vtableStubs_sparc.cpp
  // pd_code_size_limit.
  assert (UseCompressedClassPointers, "must be compressed");
  if (CompressedKlassPointers::base() != NULL) {
    assert(r != G6_heapbase, "bad register choice");
    set((intptr_t)CompressedKlassPointers::base(), G6_heapbase);
    if (CompressedKlassPointers::shift() != 0)
      sllx(r, LogKlassAlignmentInBytes, r);
    add(r, G6_heapbase, r);
    reinit_heapbase();
  } else {
    assert (LogKlassAlignmentInBytes == CompressedKlassPointers::shift() || CompressedKlassPointers::shift() == 0, "decode alg wrong");
    sllx(r, CompressedKlassPointers::shift(), r);
  }
}

void  MacroAssembler::decode_klass_not_null(Register src, Register dst) {
  if (src == dst) {
    decode_klass_not_null(src);
  } else {
    // Do not add assert code to this unless you change vtableStubs_sparc.cpp
    // pd_code_size_limit.
    assert (UseCompressedClassPointers, "must be compressed");
    if (CompressedKlassPointers::base() != NULL) {
      if (CompressedKlassPointers::shift() != 0) {
        assert((src != G6_heapbase) && (dst != G6_heapbase), "bad register choice");
        set((intptr_t)CompressedKlassPointers::base(), G6_heapbase);
        sllx(src, LogKlassAlignmentInBytes, dst);
        add(dst, G6_heapbase, dst);
        reinit_heapbase();
      } else {
        set((intptr_t)CompressedKlassPointers::base(), dst);
        add(src, dst, dst);
      }
    } else {
      // shift/mov src into dst.
      assert (LogKlassAlignmentInBytes == CompressedKlassPointers::shift() || CompressedKlassPointers::shift() == 0, "decode alg wrong");
      sllx(src, CompressedKlassPointers::shift(), dst);
    }
  }
}

void MacroAssembler::reinit_heapbase() {
  if (UseCompressedOops || UseCompressedClassPointers) {
    if (Universe::heap() != NULL) {
      set((intptr_t)CompressedOops::ptrs_base(), G6_heapbase);
    } else {
      AddressLiteral base(CompressedOops::ptrs_base_addr());
      load_ptr_contents(base, G6_heapbase);
    }
  }
}

#ifdef COMPILER2

// Compress char[] to byte[] by compressing 16 bytes at once. Return 0 on failure.
void MacroAssembler::string_compress_16(Register src, Register dst, Register cnt, Register result,
                                        Register tmp1, Register tmp2, Register tmp3, Register tmp4,
                                        FloatRegister ftmp1, FloatRegister ftmp2, FloatRegister ftmp3, Label& Ldone) {
  Label Lloop, Lslow;
  assert(UseVIS >= 3, "VIS3 is required");
  assert_different_registers(src, dst, cnt, tmp1, tmp2, tmp3, tmp4, result);
  assert_different_registers(ftmp1, ftmp2, ftmp3);

  // Check if cnt >= 8 (= 16 bytes)
  cmp(cnt, 8);
  br(Assembler::less, false, Assembler::pn, Lslow);
  delayed()->mov(cnt, result); // copy count

  // Check for 8-byte alignment of src and dst
  or3(src, dst, tmp1);
  andcc(tmp1, 7, G0);
  br(Assembler::notZero, false, Assembler::pn, Lslow);
  delayed()->nop();

  // Set mask for bshuffle instruction
  Register mask = tmp4;
  set(0x13579bdf, mask);
  bmask(mask, G0, G0);

  // Set mask to 0xff00 ff00 ff00 ff00 to check for non-latin1 characters
  Assembler::sethi(0xff00fc00, mask); // mask = 0x0000 0000 ff00 fc00
  add(mask, 0x300, mask);             // mask = 0x0000 0000 ff00 ff00
  sllx(mask, 32, tmp1);               // tmp1 = 0xff00 ff00 0000 0000
  or3(mask, tmp1, mask);              // mask = 0xff00 ff00 ff00 ff00

  // Load first 8 bytes
  ldx(src, 0, tmp1);

  bind(Lloop);
  // Load next 8 bytes
  ldx(src, 8, tmp2);

  // Check for non-latin1 character by testing if the most significant byte of a char is set.
  // Although we have to move the data between integer and floating point registers, this is
  // still faster than the corresponding VIS instructions (ford/fand/fcmpd).
  or3(tmp1, tmp2, tmp3);
  btst(tmp3, mask);
  // annul zeroing if branch is not taken to preserve original count
  brx(Assembler::notZero, true, Assembler::pn, Ldone);
  delayed()->mov(G0, result); // 0 - failed

  // Move bytes into float register
  movxtod(tmp1, ftmp1);
  movxtod(tmp2, ftmp2);

  // Compress by copying one byte per char from ftmp1 and ftmp2 to ftmp3
  bshuffle(ftmp1, ftmp2, ftmp3);
  stf(FloatRegisterImpl::D, ftmp3, dst, 0);

  // Increment addresses and decrement count
  inc(src, 16);
  inc(dst, 8);
  dec(cnt, 8);

  cmp(cnt, 8);
  // annul LDX if branch is not taken to prevent access past end of string
  br(Assembler::greaterEqual, true, Assembler::pt, Lloop);
  delayed()->ldx(src, 0, tmp1);

  // Fallback to slow version
  bind(Lslow);
}

// Compress char[] to byte[]. Return 0 on failure.
void MacroAssembler::string_compress(Register src, Register dst, Register cnt, Register result, Register tmp, Label& Ldone) {
  Label Lloop;
  assert_different_registers(src, dst, cnt, tmp, result);

  lduh(src, 0, tmp);

  bind(Lloop);
  inc(src, sizeof(jchar));
  cmp(tmp, 0xff);
  // annul zeroing if branch is not taken to preserve original count
  br(Assembler::greater, true, Assembler::pn, Ldone); // don't check xcc
  delayed()->mov(G0, result); // 0 - failed
  deccc(cnt);
  stb(tmp, dst, 0);
  inc(dst);
  // annul LDUH if branch is not taken to prevent access past end of string
  br(Assembler::notZero, true, Assembler::pt, Lloop);
  delayed()->lduh(src, 0, tmp); // hoisted
}

// Inflate byte[] to char[] by inflating 16 bytes at once.
void MacroAssembler::string_inflate_16(Register src, Register dst, Register cnt, Register tmp,
                                       FloatRegister ftmp1, FloatRegister ftmp2, FloatRegister ftmp3, FloatRegister ftmp4, Label& Ldone) {
  Label Lloop, Lslow;
  assert(UseVIS >= 3, "VIS3 is required");
  assert_different_registers(src, dst, cnt, tmp);
  assert_different_registers(ftmp1, ftmp2, ftmp3, ftmp4);

  // Check if cnt >= 8 (= 16 bytes)
  cmp(cnt, 8);
  br(Assembler::less, false, Assembler::pn, Lslow);
  delayed()->nop();

  // Check for 8-byte alignment of src and dst
  or3(src, dst, tmp);
  andcc(tmp, 7, G0);
  br(Assembler::notZero, false, Assembler::pn, Lslow);
  // Initialize float register to zero
  FloatRegister zerof = ftmp4;
  delayed()->fzero(FloatRegisterImpl::D, zerof);

  // Load first 8 bytes
  ldf(FloatRegisterImpl::D, src, 0, ftmp1);

  bind(Lloop);
  inc(src, 8);
  dec(cnt, 8);

  // Inflate the string by interleaving each byte from the source array
  // with a zero byte and storing the result in the destination array.
  fpmerge(zerof, ftmp1->successor(), ftmp2);
  stf(FloatRegisterImpl::D, ftmp2, dst, 8);
  fpmerge(zerof, ftmp1, ftmp3);
  stf(FloatRegisterImpl::D, ftmp3, dst, 0);

  inc(dst, 16);

  cmp(cnt, 8);
  // annul LDX if branch is not taken to prevent access past end of string
  br(Assembler::greaterEqual, true, Assembler::pt, Lloop);
  delayed()->ldf(FloatRegisterImpl::D, src, 0, ftmp1);

  // Fallback to slow version
  bind(Lslow);
}

// Inflate byte[] to char[].
void MacroAssembler::string_inflate(Register src, Register dst, Register cnt, Register tmp, Label& Ldone) {
  Label Loop;
  assert_different_registers(src, dst, cnt, tmp);

  ldub(src, 0, tmp);
  bind(Loop);
  inc(src);
  deccc(cnt);
  sth(tmp, dst, 0);
  inc(dst, sizeof(jchar));
  // annul LDUB if branch is not taken to prevent access past end of string
  br(Assembler::notZero, true, Assembler::pt, Loop);
  delayed()->ldub(src, 0, tmp); // hoisted
}

void MacroAssembler::string_compare(Register str1, Register str2,
                                    Register cnt1, Register cnt2,
                                    Register tmp1, Register tmp2,
                                    Register result, int ae) {
  Label Ldone, Lloop;
  assert_different_registers(str1, str2, cnt1, cnt2, tmp1, result);
  int stride1, stride2;

  // Note: Making use of the fact that compareTo(a, b) == -compareTo(b, a)
  // we interchange str1 and str2 in the UL case and negate the result.
  // Like this, str1 is always latin1 encoded, expect for the UU case.

  if (ae == StrIntrinsicNode::LU || ae == StrIntrinsicNode::UL) {
    srl(cnt2, 1, cnt2);
  }

  // See if the lengths are different, and calculate min in cnt1.
  // Save diff in case we need it for a tie-breaker.
  Label Lskip;
  Register diff = tmp1;
  subcc(cnt1, cnt2, diff);
  br(Assembler::greater, true, Assembler::pt, Lskip);
  // cnt2 is shorter, so use its count:
  delayed()->mov(cnt2, cnt1);
  bind(Lskip);

  // Rename registers
  Register limit1 = cnt1;
  Register limit2 = limit1;
  Register chr1   = result;
  Register chr2   = cnt2;
  if (ae == StrIntrinsicNode::LU || ae == StrIntrinsicNode::UL) {
    // We need an additional register to keep track of two limits
    assert_different_registers(str1, str2, cnt1, cnt2, tmp1, tmp2, result);
    limit2 = tmp2;
  }

  // Is the minimum length zero?
  cmp(limit1, (int)0); // use cast to resolve overloading ambiguity
  br(Assembler::equal, true, Assembler::pn, Ldone);
  // result is difference in lengths
  if (ae == StrIntrinsicNode::UU) {
    delayed()->sra(diff, 1, result);  // Divide by 2 to get number of chars
  } else {
    delayed()->mov(diff, result);
  }

  // Load first characters
  if (ae == StrIntrinsicNode::LL) {
    stride1 = stride2 = sizeof(jbyte);
    ldub(str1, 0, chr1);
    ldub(str2, 0, chr2);
  } else if (ae == StrIntrinsicNode::UU) {
    stride1 = stride2 = sizeof(jchar);
    lduh(str1, 0, chr1);
    lduh(str2, 0, chr2);
  } else {
    stride1 = sizeof(jbyte);
    stride2 = sizeof(jchar);
    ldub(str1, 0, chr1);
    lduh(str2, 0, chr2);
  }

  // Compare first characters
  subcc(chr1, chr2, chr1);
  br(Assembler::notZero, false, Assembler::pt, Ldone);
  assert(chr1 == result, "result must be pre-placed");
  delayed()->nop();

  // Check if the strings start at same location
  cmp(str1, str2);
  brx(Assembler::equal, true, Assembler::pn, Ldone);
  delayed()->mov(G0, result);  // result is zero

  // We have no guarantee that on 64 bit the higher half of limit is 0
  signx(limit1);

  // Get limit
  if (ae == StrIntrinsicNode::LU || ae == StrIntrinsicNode::UL) {
    sll(limit1, 1, limit2);
    subcc(limit2, stride2, chr2);
  }
  subcc(limit1, stride1, chr1);
  br(Assembler::zero, true, Assembler::pn, Ldone);
  // result is difference in lengths
  if (ae == StrIntrinsicNode::UU) {
    delayed()->sra(diff, 1, result);  // Divide by 2 to get number of chars
  } else {
    delayed()->mov(diff, result);
  }

  // Shift str1 and str2 to the end of the arrays, negate limit
  add(str1, limit1, str1);
  add(str2, limit2, str2);
  neg(chr1, limit1);  // limit1 = -(limit1-stride1)
  if (ae == StrIntrinsicNode::LU || ae == StrIntrinsicNode::UL) {
    neg(chr2, limit2);  // limit2 = -(limit2-stride2)
  }

  // Compare the rest of the characters
  load_sized_value(Address(str1, limit1), chr1, (ae == StrIntrinsicNode::UU) ? 2 : 1, false);

  bind(Lloop);
  load_sized_value(Address(str2, limit2), chr2, (ae == StrIntrinsicNode::LL) ? 1 : 2, false);

  subcc(chr1, chr2, chr1);
  br(Assembler::notZero, false, Assembler::pt, Ldone);
  assert(chr1 == result, "result must be pre-placed");
  delayed()->inccc(limit1, stride1);
  if (ae == StrIntrinsicNode::LU || ae == StrIntrinsicNode::UL) {
    inccc(limit2, stride2);
  }

  // annul LDUB if branch is not taken to prevent access past end of string
  br(Assembler::notZero, true, Assembler::pt, Lloop);
  delayed()->load_sized_value(Address(str1, limit1), chr1, (ae == StrIntrinsicNode::UU) ? 2 : 1, false);

  // If strings are equal up to min length, return the length difference.
  if (ae == StrIntrinsicNode::UU) {
    // Divide by 2 to get number of chars
    sra(diff, 1, result);
  } else {
    mov(diff, result);
  }

  // Otherwise, return the difference between the first mismatched chars.
  bind(Ldone);
  if(ae == StrIntrinsicNode::UL) {
    // Negate result (see note above)
    neg(result);
  }
}

void MacroAssembler::array_equals(bool is_array_equ, Register ary1, Register ary2,
                                  Register limit, Register tmp, Register result, bool is_byte) {
  Label Ldone, Lloop, Lremaining;
  assert_different_registers(ary1, ary2, limit, tmp, result);

  int length_offset  = arrayOopDesc::length_offset_in_bytes();
  int base_offset    = arrayOopDesc::base_offset_in_bytes(is_byte ? T_BYTE : T_CHAR);
  assert(base_offset % 8 == 0, "Base offset must be 8-byte aligned");

  if (is_array_equ) {
    // return true if the same array
    cmp(ary1, ary2);
    brx(Assembler::equal, true, Assembler::pn, Ldone);
    delayed()->mov(1, result);  // equal

    br_null(ary1, true, Assembler::pn, Ldone);
    delayed()->clr(result);     // not equal

    br_null(ary2, true, Assembler::pn, Ldone);
    delayed()->clr(result);     // not equal

    // load the lengths of arrays
    ld(Address(ary1, length_offset), limit);
    ld(Address(ary2, length_offset), tmp);

    // return false if the two arrays are not equal length
    cmp(limit, tmp);
    br(Assembler::notEqual, true, Assembler::pn, Ldone);
    delayed()->clr(result);     // not equal
  }

  cmp_zero_and_br(Assembler::zero, limit, Ldone, true, Assembler::pn);
  delayed()->mov(1, result); // zero-length arrays are equal

  if (is_array_equ) {
    // load array addresses
    add(ary1, base_offset, ary1);
    add(ary2, base_offset, ary2);
    // set byte count
    if (!is_byte) {
      sll(limit, exact_log2(sizeof(jchar)), limit);
    }
  } else {
    // We have no guarantee that on 64 bit the higher half of limit is 0
    signx(limit);
  }

#ifdef ASSERT
  // Sanity check for doubleword (8-byte) alignment of ary1 and ary2.
  // Guaranteed on 64-bit systems (see arrayOopDesc::header_size_in_bytes()).
  Label Laligned;
  or3(ary1, ary2, tmp);
  andcc(tmp, 7, tmp);
  br_null_short(tmp, Assembler::pn, Laligned);
  STOP("First array element is not 8-byte aligned.");
  should_not_reach_here();
  bind(Laligned);
#endif

  // Shift ary1 and ary2 to the end of the arrays, negate limit
  add(ary1, limit, ary1);
  add(ary2, limit, ary2);
  neg(limit, limit);

  // MAIN LOOP
  // Load and compare array elements of size 'byte_width' until the elements are not
  // equal or we reached the end of the arrays. If the size of the arrays is not a
  // multiple of 'byte_width', we simply read over the end of the array, bail out and
  // compare the remaining bytes below by skipping the garbage bytes.
  ldx(ary1, limit, result);
  bind(Lloop);
  ldx(ary2, limit, tmp);
  inccc(limit, 8);
  // Bail out if we reached the end (but still do the comparison)
  br(Assembler::positive, false, Assembler::pn, Lremaining);
  delayed()->cmp(result, tmp);
  // Check equality of elements
  brx(Assembler::equal, false, Assembler::pt, target(Lloop));
  delayed()->ldx(ary1, limit, result);

  ba(Ldone);
  delayed()->clr(result); // not equal

  // TAIL COMPARISON
  // We got here because we reached the end of the arrays. 'limit' is the number of
  // garbage bytes we may have compared by reading over the end of the arrays. Shift
  // out the garbage and compare the remaining elements.
  bind(Lremaining);
  // Optimistic shortcut: elements potentially including garbage are equal
  brx(Assembler::equal, true, Assembler::pt, target(Ldone));
  delayed()->mov(1, result); // equal
  // Shift 'limit' bytes to the right and compare
  sll(limit, 3, limit); // bytes to bits
  srlx(result, limit, result);
  srlx(tmp, limit, tmp);
  cmp(result, tmp);
  clr(result);
  movcc(Assembler::equal, false, xcc, 1, result);

  bind(Ldone);
}

void MacroAssembler::has_negatives(Register inp, Register size, Register result, Register t2, Register t3, Register t4, Register t5) {

  // test for negative bytes in input string of a given size
  // result 1 if found, 0 otherwise.

  Label Lcore, Ltail, Lreturn, Lcore_rpt;

  assert_different_registers(inp, size, t2, t3, t4, t5, result);

  Register i     = result;  // result used as integer index i until very end
  Register lmask = t2;      // t2 is aliased to lmask

  // INITIALIZATION
  // ===========================================================
  // initialize highbits mask -> lmask = 0x8080808080808080  (8B/64b)
  // compute unaligned offset -> i
  // compute core end index   -> t5
  Assembler::sethi(0x80808000, t2);   //! sethi macro fails to emit optimal
  add(t2, 0x80, t2);
  sllx(t2, 32, t3);
  or3(t3, t2, lmask);                 // 0x8080808080808080 -> lmask
  sra(size,0,size);
  andcc(inp, 0x7, i);                 // unaligned offset -> i
  br(Assembler::zero, true, Assembler::pn, Lcore); // starts 8B aligned?
  delayed()->add(size, -8, t5);       // (annuled) core end index -> t5

  // ===========================================================

  // UNALIGNED HEAD
  // ===========================================================
  // * unaligned head handling: grab aligned 8B containing unaligned inp(ut)
  // * obliterate (ignore) bytes outside string by shifting off reg ends
  // * compare with bitmask, short circuit return true if one or more high
  //   bits set.
  cmp(size, 0);
  br(Assembler::zero, true, Assembler::pn, Lreturn); // short-circuit?
  delayed()->mov(0,result);      // annuled so i not clobbered for following
  neg(i, t4);
  add(i, size, t5);
  ldx(inp, t4, t3);  // raw aligned 8B containing unaligned head -> t3
  mov(8, t4);
  sub(t4, t5, t4);
  sra(t4, 31, t5);
  andn(t4, t5, t5);
  add(i, t5, t4);
  sll(t5, 3, t5);
  sll(t4, 3, t4);   // # bits to shift right, left -> t5,t4
  srlx(t3, t5, t3);
  sllx(t3, t4, t3); // bytes outside string in 8B header obliterated -> t3
  andcc(lmask, t3, G0);
  brx(Assembler::notZero, true, Assembler::pn, Lreturn); // short circuit?
  delayed()->mov(1,result);      // annuled so i not clobbered for following
  add(size, -8, t5);             // core end index -> t5
  mov(8, t4);
  sub(t4, i, i);                 // # bytes examined in unalgn head (<8) -> i
  // ===========================================================

  // ALIGNED CORE
  // ===========================================================
  // * iterate index i over aligned 8B sections of core, comparing with
  //   bitmask, short circuit return true if one or more high bits set
  // t5 contains core end index/loop limit which is the index
  //     of the MSB of last (unaligned) 8B fully contained in the string.
  // inp   contains address of first byte in string/array
  // lmask contains 8B high bit mask for comparison
  // i     contains next index to be processed (adr. inp+i is on 8B boundary)
  bind(Lcore);
  cmp_and_br_short(i, t5, Assembler::greater, Assembler::pn, Ltail);
  bind(Lcore_rpt);
  ldx(inp, i, t3);
  andcc(t3, lmask, G0);
  brx(Assembler::notZero, true, Assembler::pn, Lreturn);
  delayed()->mov(1, result);    // annuled so i not clobbered for following
  add(i, 8, i);
  cmp_and_br_short(i, t5, Assembler::lessEqual, Assembler::pn, Lcore_rpt);
  // ===========================================================

  // ALIGNED TAIL (<8B)
  // ===========================================================
  // handle aligned tail of 7B or less as complete 8B, obliterating end of
  // string bytes by shifting them off end, compare what's left with bitmask
  // inp   contains address of first byte in string/array
  // lmask contains 8B high bit mask for comparison
  // i     contains next index to be processed (adr. inp+i is on 8B boundary)
  bind(Ltail);
  subcc(size, i, t4);   // # of remaining bytes in string -> t4
  // return 0 if no more remaining bytes
  br(Assembler::lessEqual, true, Assembler::pn, Lreturn);
  delayed()->mov(0, result); // annuled so i not clobbered for following
  ldx(inp, i, t3);       // load final 8B (aligned) containing tail -> t3
  mov(8, t5);
  sub(t5, t4, t4);
  mov(0, result);        // ** i clobbered at this point
  sll(t4, 3, t4);        // bits beyond end of string          -> t4
  srlx(t3, t4, t3);      // bytes beyond end now obliterated   -> t3
  andcc(lmask, t3, G0);
  movcc(Assembler::notZero, false, xcc,  1, result);
  bind(Lreturn);
}

#endif


// Use BIS for zeroing (count is in bytes).
void MacroAssembler::bis_zeroing(Register to, Register count, Register temp, Label& Ldone) {
  assert(UseBlockZeroing && VM_Version::has_blk_zeroing(), "only works with BIS zeroing");
  Register end = count;
  int cache_line_size = VM_Version::prefetch_data_size();
  assert(cache_line_size > 0, "cache line size should be known for this code");
  // Minimum count when BIS zeroing can be used since
  // it needs membar which is expensive.
  int block_zero_size  = MAX2(cache_line_size*3, (int)BlockZeroingLowLimit);

  Label small_loop;
  // Check if count is negative (dead code) or zero.
  // Note, count uses 64bit in 64 bit VM.
  cmp_and_brx_short(count, 0, Assembler::lessEqual, Assembler::pn, Ldone);

  // Use BIS zeroing only for big arrays since it requires membar.
  if (Assembler::is_simm13(block_zero_size)) { // < 4096
    cmp(count, block_zero_size);
  } else {
    set(block_zero_size, temp);
    cmp(count, temp);
  }
  br(Assembler::lessUnsigned, false, Assembler::pt, small_loop);
  delayed()->add(to, count, end);

  // Note: size is >= three (32 bytes) cache lines.

  // Clean the beginning of space up to next cache line.
  for (int offs = 0; offs < cache_line_size; offs += 8) {
    stx(G0, to, offs);
  }

  // align to next cache line
  add(to, cache_line_size, to);
  and3(to, -cache_line_size, to);

  // Note: size left >= two (32 bytes) cache lines.

  // BIS should not be used to zero tail (64 bytes)
  // to avoid zeroing a header of the following object.
  sub(end, (cache_line_size*2)-8, end);

  Label bis_loop;
  bind(bis_loop);
  stxa(G0, to, G0, Assembler::ASI_ST_BLKINIT_PRIMARY);
  add(to, cache_line_size, to);
  cmp_and_brx_short(to, end, Assembler::lessUnsigned, Assembler::pt, bis_loop);

  // BIS needs membar.
  membar(Assembler::StoreLoad);

  add(end, (cache_line_size*2)-8, end); // restore end
  cmp_and_brx_short(to, end, Assembler::greaterEqualUnsigned, Assembler::pn, Ldone);

  // Clean the tail.
  bind(small_loop);
  stx(G0, to, 0);
  add(to, 8, to);
  cmp_and_brx_short(to, end, Assembler::lessUnsigned, Assembler::pt, small_loop);
  nop(); // Separate short branches
}

/**
 * Update CRC-32[C] with a byte value according to constants in table
 *
 * @param [in,out]crc   Register containing the crc.
 * @param [in]val       Register containing the byte to fold into the CRC.
 * @param [in]table     Register containing the table of crc constants.
 *
 * uint32_t crc;
 * val = crc_table[(val ^ crc) & 0xFF];
 * crc = val ^ (crc >> 8);
 */
void MacroAssembler::update_byte_crc32(Register crc, Register val, Register table) {
  xor3(val, crc, val);
  and3(val, 0xFF, val);
  sllx(val, 2, val);
  lduw(table, val, val);
  srlx(crc, 8, crc);
  xor3(val, crc, crc);
}

// Reverse byte order of lower 32 bits, assuming upper 32 bits all zeros
void MacroAssembler::reverse_bytes_32(Register src, Register dst, Register tmp) {
    srlx(src, 24, dst);

    sllx(src, 32+8, tmp);
    srlx(tmp, 32+24, tmp);
    sllx(tmp, 8, tmp);
    or3(dst, tmp, dst);

    sllx(src, 32+16, tmp);
    srlx(tmp, 32+24, tmp);
    sllx(tmp, 16, tmp);
    or3(dst, tmp, dst);

    sllx(src, 32+24, tmp);
    srlx(tmp, 32, tmp);
    or3(dst, tmp, dst);
}

void MacroAssembler::movitof_revbytes(Register src, FloatRegister dst, Register tmp1, Register tmp2) {
  reverse_bytes_32(src, tmp1, tmp2);
  movxtod(tmp1, dst);
}

void MacroAssembler::movftoi_revbytes(FloatRegister src, Register dst, Register tmp1, Register tmp2) {
  movdtox(src, tmp1);
  reverse_bytes_32(tmp1, dst, tmp2);
}

void MacroAssembler::fold_128bit_crc32(Register xcrc_hi, Register xcrc_lo, Register xK_hi, Register xK_lo, Register xtmp_hi, Register xtmp_lo, Register buf, int offset) {
  xmulx(xcrc_hi, xK_hi, xtmp_lo);
  xmulxhi(xcrc_hi, xK_hi, xtmp_hi);
  xmulxhi(xcrc_lo, xK_lo, xcrc_hi);
  xmulx(xcrc_lo, xK_lo, xcrc_lo);
  xor3(xcrc_lo, xtmp_lo, xcrc_lo);
  xor3(xcrc_hi, xtmp_hi, xcrc_hi);
  ldxl(buf, G0, xtmp_lo);
  inc(buf, 8);
  ldxl(buf, G0, xtmp_hi);
  inc(buf, 8);
  xor3(xcrc_lo, xtmp_lo, xcrc_lo);
  xor3(xcrc_hi, xtmp_hi, xcrc_hi);
}

void MacroAssembler::fold_128bit_crc32(Register xcrc_hi, Register xcrc_lo, Register xK_hi, Register xK_lo, Register xtmp_hi, Register xtmp_lo, Register xbuf_hi, Register xbuf_lo) {
  mov(xcrc_lo, xtmp_lo);
  mov(xcrc_hi, xtmp_hi);
  xmulx(xtmp_hi, xK_hi, xtmp_lo);
  xmulxhi(xtmp_hi, xK_hi, xtmp_hi);
  xmulxhi(xcrc_lo, xK_lo, xcrc_hi);
  xmulx(xcrc_lo, xK_lo, xcrc_lo);
  xor3(xcrc_lo, xbuf_lo, xcrc_lo);
  xor3(xcrc_hi, xbuf_hi, xcrc_hi);
  xor3(xcrc_lo, xtmp_lo, xcrc_lo);
  xor3(xcrc_hi, xtmp_hi, xcrc_hi);
}

void MacroAssembler::fold_8bit_crc32(Register xcrc, Register table, Register xtmp, Register tmp) {
  and3(xcrc, 0xFF, tmp);
  sllx(tmp, 2, tmp);
  lduw(table, tmp, xtmp);
  srlx(xcrc, 8, xcrc);
  xor3(xtmp, xcrc, xcrc);
}

void MacroAssembler::fold_8bit_crc32(Register crc, Register table, Register tmp) {
  and3(crc, 0xFF, tmp);
  srlx(crc, 8, crc);
  sllx(tmp, 2, tmp);
  lduw(table, tmp, tmp);
  xor3(tmp, crc, crc);
}

#define CRC32_TMP_REG_NUM 18

#define CRC32_CONST_64  0x163cd6124
#define CRC32_CONST_96  0x0ccaa009e
#define CRC32_CONST_160 0x1751997d0
#define CRC32_CONST_480 0x1c6e41596
#define CRC32_CONST_544 0x154442bd4

void MacroAssembler::kernel_crc32(Register crc, Register buf, Register len, Register table) {

  Label L_cleanup_loop, L_cleanup_check, L_align_loop, L_align_check;
  Label L_main_loop_prologue;
  Label L_fold_512b, L_fold_512b_loop, L_fold_128b;
  Label L_fold_tail, L_fold_tail_loop;
  Label L_8byte_fold_check;

  const Register tmp[CRC32_TMP_REG_NUM] = {L0, L1, L2, L3, L4, L5, L6, G1, I0, I1, I2, I3, I4, I5, I7, O4, O5, G3};

  Register const_64  = tmp[CRC32_TMP_REG_NUM-1];
  Register const_96  = tmp[CRC32_TMP_REG_NUM-1];
  Register const_160 = tmp[CRC32_TMP_REG_NUM-2];
  Register const_480 = tmp[CRC32_TMP_REG_NUM-1];
  Register const_544 = tmp[CRC32_TMP_REG_NUM-2];

  set(ExternalAddress(StubRoutines::crc_table_addr()), table);

  not1(crc); // ~c
  clruwu(crc); // clear upper 32 bits of crc

  // Check if below cutoff, proceed directly to cleanup code
  mov(31, G4);
  cmp_and_br_short(len, G4, Assembler::lessEqualUnsigned, Assembler::pt, L_cleanup_check);

  // Align buffer to 8 byte boundry
  mov(8, O5);
  and3(buf, 0x7, O4);
  sub(O5, O4, O5);
  and3(O5, 0x7, O5);
  sub(len, O5, len);
  ba(L_align_check);
  delayed()->nop();

  // Alignment loop, table look up method for up to 7 bytes
  bind(L_align_loop);
  ldub(buf, 0, O4);
  inc(buf);
  dec(O5);
  xor3(O4, crc, O4);
  and3(O4, 0xFF, O4);
  sllx(O4, 2, O4);
  lduw(table, O4, O4);
  srlx(crc, 8, crc);
  xor3(O4, crc, crc);
  bind(L_align_check);
  nop();
  cmp_and_br_short(O5, 0, Assembler::notEqual, Assembler::pt, L_align_loop);

  // Aligned on 64-bit (8-byte) boundry at this point
  // Check if still above cutoff (31-bytes)
  mov(31, G4);
  cmp_and_br_short(len, G4, Assembler::lessEqualUnsigned, Assembler::pt, L_cleanup_check);
  // At least 32 bytes left to process

  // Free up registers by storing them to FP registers
  for (int i = 0; i < CRC32_TMP_REG_NUM; i++) {
    movxtod(tmp[i], as_FloatRegister(2*i));
  }

  // Determine which loop to enter
  // Shared prologue
  ldxl(buf, G0, tmp[0]);
  inc(buf, 8);
  ldxl(buf, G0, tmp[1]);
  inc(buf, 8);
  xor3(tmp[0], crc, tmp[0]); // Fold CRC into first few bytes
  and3(crc, 0, crc); // Clear out the crc register
  // Main loop needs 128-bytes at least
  mov(128, G4);
  mov(64, tmp[2]);
  cmp_and_br_short(len, G4, Assembler::greaterEqualUnsigned, Assembler::pt, L_main_loop_prologue);
  // Less than 64 bytes
  nop();
  cmp_and_br_short(len, tmp[2], Assembler::lessUnsigned, Assembler::pt, L_fold_tail);
  // Between 64 and 127 bytes
  set64(CRC32_CONST_96,  const_96,  tmp[8]);
  set64(CRC32_CONST_160, const_160, tmp[9]);
  fold_128bit_crc32(tmp[1], tmp[0], const_96, const_160, tmp[2], tmp[3], buf, 0);
  fold_128bit_crc32(tmp[1], tmp[0], const_96, const_160, tmp[4], tmp[5], buf, 16);
  fold_128bit_crc32(tmp[1], tmp[0], const_96, const_160, tmp[6], tmp[7], buf, 32);
  dec(len, 48);
  ba(L_fold_tail);
  delayed()->nop();

  bind(L_main_loop_prologue);
  for (int i = 2; i < 8; i++) {
    ldxl(buf, G0, tmp[i]);
    inc(buf, 8);
  }

  // Fold total 512 bits of polynomial on each iteration,
  // 128 bits per each of 4 parallel streams
  set64(CRC32_CONST_480, const_480, tmp[8]);
  set64(CRC32_CONST_544, const_544, tmp[9]);

  mov(128, G4);
  bind(L_fold_512b_loop);
  fold_128bit_crc32(tmp[1], tmp[0], const_480, const_544, tmp[9],  tmp[8],  buf,  0);
  fold_128bit_crc32(tmp[3], tmp[2], const_480, const_544, tmp[11], tmp[10], buf, 16);
  fold_128bit_crc32(tmp[5], tmp[4], const_480, const_544, tmp[13], tmp[12], buf, 32);
  fold_128bit_crc32(tmp[7], tmp[6], const_480, const_544, tmp[15], tmp[14], buf, 64);
  dec(len, 64);
  cmp_and_br_short(len, G4, Assembler::greaterEqualUnsigned, Assembler::pt, L_fold_512b_loop);

  // Fold 512 bits to 128 bits
  bind(L_fold_512b);
  set64(CRC32_CONST_96,  const_96,  tmp[8]);
  set64(CRC32_CONST_160, const_160, tmp[9]);

  fold_128bit_crc32(tmp[1], tmp[0], const_96, const_160, tmp[8], tmp[9], tmp[3], tmp[2]);
  fold_128bit_crc32(tmp[1], tmp[0], const_96, const_160, tmp[8], tmp[9], tmp[5], tmp[4]);
  fold_128bit_crc32(tmp[1], tmp[0], const_96, const_160, tmp[8], tmp[9], tmp[7], tmp[6]);
  dec(len, 48);

  // Fold the rest of 128 bits data chunks
  bind(L_fold_tail);
  mov(32, G4);
  cmp_and_br_short(len, G4, Assembler::lessEqualUnsigned, Assembler::pt, L_fold_128b);

  set64(CRC32_CONST_96,  const_96,  tmp[8]);
  set64(CRC32_CONST_160, const_160, tmp[9]);

  bind(L_fold_tail_loop);
  fold_128bit_crc32(tmp[1], tmp[0], const_96, const_160, tmp[2], tmp[3], buf, 0);
  sub(len, 16, len);
  cmp_and_br_short(len, G4, Assembler::greaterEqualUnsigned, Assembler::pt, L_fold_tail_loop);

  // Fold the 128 bits in tmps 0 - 1 into tmp 1
  bind(L_fold_128b);

  set64(CRC32_CONST_64, const_64, tmp[4]);

  xmulx(const_64, tmp[0], tmp[2]);
  xmulxhi(const_64, tmp[0], tmp[3]);

  srl(tmp[2], G0, tmp[4]);
  xmulx(const_64, tmp[4], tmp[4]);

  srlx(tmp[2], 32, tmp[2]);
  sllx(tmp[3], 32, tmp[3]);
  or3(tmp[2], tmp[3], tmp[2]);

  xor3(tmp[4], tmp[1], tmp[4]);
  xor3(tmp[4], tmp[2], tmp[1]);
  dec(len, 8);

  // Use table lookup for the 8 bytes left in tmp[1]
  dec(len, 8);

  // 8 8-bit folds to compute 32-bit CRC.
  for (int j = 0; j < 4; j++) {
    fold_8bit_crc32(tmp[1], table, tmp[2], tmp[3]);
  }
  srl(tmp[1], G0, crc); // move 32 bits to general register
  for (int j = 0; j < 4; j++) {
    fold_8bit_crc32(crc, table, tmp[3]);
  }

  bind(L_8byte_fold_check);

  // Restore int registers saved in FP registers
  for (int i = 0; i < CRC32_TMP_REG_NUM; i++) {
    movdtox(as_FloatRegister(2*i), tmp[i]);
  }

  ba(L_cleanup_check);
  delayed()->nop();

  // Table look-up method for the remaining few bytes
  bind(L_cleanup_loop);
  ldub(buf, 0, O4);
  inc(buf);
  dec(len);
  xor3(O4, crc, O4);
  and3(O4, 0xFF, O4);
  sllx(O4, 2, O4);
  lduw(table, O4, O4);
  srlx(crc, 8, crc);
  xor3(O4, crc, crc);
  bind(L_cleanup_check);
  nop();
  cmp_and_br_short(len, 0, Assembler::greaterUnsigned, Assembler::pt, L_cleanup_loop);

  not1(crc);
}

#define CHUNK_LEN   128          /* 128 x 8B = 1KB */
#define CHUNK_K1    0x1307a0206  /* reverseBits(pow(x, CHUNK_LEN*8*8*3 - 32) mod P(x)) << 1 */
#define CHUNK_K2    0x1a0f717c4  /* reverseBits(pow(x, CHUNK_LEN*8*8*2 - 32) mod P(x)) << 1 */
#define CHUNK_K3    0x0170076fa  /* reverseBits(pow(x, CHUNK_LEN*8*8*1 - 32) mod P(x)) << 1 */

void MacroAssembler::kernel_crc32c(Register crc, Register buf, Register len, Register table) {

  Label L_crc32c_head, L_crc32c_aligned;
  Label L_crc32c_parallel, L_crc32c_parallel_loop;
  Label L_crc32c_serial, L_crc32c_x32_loop, L_crc32c_x8, L_crc32c_x8_loop;
  Label L_crc32c_done, L_crc32c_tail, L_crc32c_return;

  set(ExternalAddress(StubRoutines::crc32c_table_addr()), table);

  cmp_and_br_short(len, 0, Assembler::lessEqual, Assembler::pn, L_crc32c_return);

  // clear upper 32 bits of crc
  clruwu(crc);

  and3(buf, 7, G4);
  cmp_and_brx_short(G4, 0, Assembler::equal, Assembler::pt, L_crc32c_aligned);

  mov(8, G1);
  sub(G1, G4, G4);

  // ------ process the misaligned head (7 bytes or less) ------
  bind(L_crc32c_head);

  // crc = (crc >>> 8) ^ byteTable[(crc ^ b) & 0xFF];
  ldub(buf, 0, G1);
  update_byte_crc32(crc, G1, table);

  inc(buf);
  dec(len);
  cmp_and_br_short(len, 0, Assembler::equal, Assembler::pn, L_crc32c_return);
  dec(G4);
  cmp_and_br_short(G4, 0, Assembler::greater, Assembler::pt, L_crc32c_head);

  // ------ process the 8-byte-aligned body ------
  bind(L_crc32c_aligned);
  nop();
  cmp_and_br_short(len, 8, Assembler::less, Assembler::pn, L_crc32c_tail);

  // reverse the byte order of lower 32 bits to big endian, and move to FP side
  movitof_revbytes(crc, F0, G1, G3);

  set(CHUNK_LEN*8*4, G4);
  cmp_and_br_short(len, G4, Assembler::less, Assembler::pt, L_crc32c_serial);

  // ------ process four 1KB chunks in parallel ------
  bind(L_crc32c_parallel);

  fzero(FloatRegisterImpl::D, F2);
  fzero(FloatRegisterImpl::D, F4);
  fzero(FloatRegisterImpl::D, F6);

  mov(CHUNK_LEN - 1, G4);
  bind(L_crc32c_parallel_loop);
  // schedule ldf's ahead of crc32c's to hide the load-use latency
  ldf(FloatRegisterImpl::D, buf, 0,            F8);
  ldf(FloatRegisterImpl::D, buf, CHUNK_LEN*8,  F10);
  ldf(FloatRegisterImpl::D, buf, CHUNK_LEN*16, F12);
  ldf(FloatRegisterImpl::D, buf, CHUNK_LEN*24, F14);
  crc32c(F0, F8,  F0);
  crc32c(F2, F10, F2);
  crc32c(F4, F12, F4);
  crc32c(F6, F14, F6);
  inc(buf, 8);
  dec(G4);
  cmp_and_br_short(G4, 0, Assembler::greater, Assembler::pt, L_crc32c_parallel_loop);

  ldf(FloatRegisterImpl::D, buf, 0,            F8);
  ldf(FloatRegisterImpl::D, buf, CHUNK_LEN*8,  F10);
  ldf(FloatRegisterImpl::D, buf, CHUNK_LEN*16, F12);
  crc32c(F0, F8,  F0);
  crc32c(F2, F10, F2);
  crc32c(F4, F12, F4);

  inc(buf, CHUNK_LEN*24);
  ldfl(FloatRegisterImpl::D, buf, G0, F14);  // load in little endian
  inc(buf, 8);

  prefetch(buf, 0,            Assembler::severalReads);
  prefetch(buf, CHUNK_LEN*8,  Assembler::severalReads);
  prefetch(buf, CHUNK_LEN*16, Assembler::severalReads);
  prefetch(buf, CHUNK_LEN*24, Assembler::severalReads);

  // move to INT side, and reverse the byte order of lower 32 bits to little endian
  movftoi_revbytes(F0, O4, G1, G4);
  movftoi_revbytes(F2, O5, G1, G4);
  movftoi_revbytes(F4, G5, G1, G4);

  // combine the results of 4 chunks
  set64(CHUNK_K1, G3, G1);
  xmulx(O4, G3, O4);
  set64(CHUNK_K2, G3, G1);
  xmulx(O5, G3, O5);
  set64(CHUNK_K3, G3, G1);
  xmulx(G5, G3, G5);

  movdtox(F14, G4);
  xor3(O4, O5, O5);
  xor3(G5, O5, O5);
  xor3(G4, O5, O5);

  // reverse the byte order to big endian, via stack, and move to FP side
  // TODO: use new revb instruction
  add(SP, -8, G1);
  srlx(G1, 3, G1);
  sllx(G1, 3, G1);
  stx(O5, G1, G0);
  ldfl(FloatRegisterImpl::D, G1, G0, F2);  // load in little endian

  crc32c(F6, F2, F0);

  set(CHUNK_LEN*8*4, G4);
  sub(len, G4, len);
  cmp_and_br_short(len, G4, Assembler::greaterEqual, Assembler::pt, L_crc32c_parallel);
  nop();
  cmp_and_br_short(len, 0, Assembler::equal, Assembler::pt, L_crc32c_done);

  bind(L_crc32c_serial);

  mov(32, G4);
  cmp_and_br_short(len, G4, Assembler::less, Assembler::pn, L_crc32c_x8);

  // ------ process 32B chunks ------
  bind(L_crc32c_x32_loop);
  ldf(FloatRegisterImpl::D, buf, 0, F2);
  crc32c(F0, F2, F0);
  ldf(FloatRegisterImpl::D, buf, 8, F2);
  crc32c(F0, F2, F0);
  ldf(FloatRegisterImpl::D, buf, 16, F2);
  crc32c(F0, F2, F0);
  ldf(FloatRegisterImpl::D, buf, 24, F2);
  inc(buf, 32);
  crc32c(F0, F2, F0);
  dec(len, 32);
  cmp_and_br_short(len, G4, Assembler::greaterEqual, Assembler::pt, L_crc32c_x32_loop);

  bind(L_crc32c_x8);
  nop();
  cmp_and_br_short(len, 8, Assembler::less, Assembler::pt, L_crc32c_done);

  // ------ process 8B chunks ------
  bind(L_crc32c_x8_loop);
  ldf(FloatRegisterImpl::D, buf, 0, F2);
  inc(buf, 8);
  crc32c(F0, F2, F0);
  dec(len, 8);
  cmp_and_br_short(len, 8, Assembler::greaterEqual, Assembler::pt, L_crc32c_x8_loop);

  bind(L_crc32c_done);

  // move to INT side, and reverse the byte order of lower 32 bits to little endian
  movftoi_revbytes(F0, crc, G1, G3);

  cmp_and_br_short(len, 0, Assembler::equal, Assembler::pt, L_crc32c_return);

  // ------ process the misaligned tail (7 bytes or less) ------
  bind(L_crc32c_tail);

  // crc = (crc >>> 8) ^ byteTable[(crc ^ b) & 0xFF];
  ldub(buf, 0, G1);
  update_byte_crc32(crc, G1, table);

  inc(buf);
  dec(len);
  cmp_and_br_short(len, 0, Assembler::greater, Assembler::pt, L_crc32c_tail);

  bind(L_crc32c_return);
  nop();
}
