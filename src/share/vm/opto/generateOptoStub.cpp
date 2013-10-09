/*
 * Copyright (c) 1999, 2013, Oracle and/or its affiliates. All rights reserved.
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
#include "opto/addnode.hpp"
#include "opto/callnode.hpp"
#include "opto/cfgnode.hpp"
#include "opto/compile.hpp"
#include "opto/connode.hpp"
#include "opto/locknode.hpp"
#include "opto/memnode.hpp"
#include "opto/mulnode.hpp"
#include "opto/node.hpp"
#include "opto/parse.hpp"
#include "opto/phaseX.hpp"
#include "opto/rootnode.hpp"
#include "opto/runtime.hpp"
#include "opto/type.hpp"

//--------------------gen_stub-------------------------------
void GraphKit::gen_stub(address C_function,
                        const char *name,
                        int is_fancy_jump,
                        bool pass_tls,
                        bool return_pc) {
  ResourceMark rm;

  const TypeTuple *jdomain = C->tf()->domain();
  const TypeTuple *jrange  = C->tf()->range();

  // The procedure start
  StartNode* start = new (C) StartNode(root(), jdomain);
  _gvn.set_type_bottom(start);

  // Make a map, with JVM state
  uint parm_cnt = jdomain->cnt();
  uint max_map = MAX2(2*parm_cnt+1, jrange->cnt());
  // %%% SynchronizationEntryBCI is redundant; use InvocationEntryBci in interfaces
  assert(SynchronizationEntryBCI == InvocationEntryBci, "");
  JVMState* jvms = new (C) JVMState(0);
  jvms->set_bci(InvocationEntryBci);
  jvms->set_monoff(max_map);
  jvms->set_scloff(max_map);
  jvms->set_endoff(max_map);
  {
    SafePointNode *map = new (C) SafePointNode( max_map, jvms );
    jvms->set_map(map);
    set_jvms(jvms);
    assert(map == this->map(), "kit.map is set");
  }

  // Make up the parameters
  uint i;
  for( i = 0; i < parm_cnt; i++ )
    map()->init_req(i, _gvn.transform(new (C) ParmNode(start, i)));
  for( ; i<map()->req(); i++ )
    map()->init_req(i, top());      // For nicer debugging

  // GraphKit requires memory to be a MergeMemNode:
  set_all_memory(map()->memory());

  // Get base of thread-local storage area
  Node* thread = _gvn.transform( new (C) ThreadLocalNode() );

  const int NoAlias = Compile::AliasIdxBot;

  Node* adr_last_Java_pc = basic_plus_adr(top(),
                                            thread,
                                            in_bytes(JavaThread::frame_anchor_offset()) +
                                            in_bytes(JavaFrameAnchor::last_Java_pc_offset()));
#if defined(SPARC)
  Node* adr_flags = basic_plus_adr(top(),
                                   thread,
                                   in_bytes(JavaThread::frame_anchor_offset()) +
                                   in_bytes(JavaFrameAnchor::flags_offset()));
#endif /* defined(SPARC) */

#if defined(AARCH64)
  // the x86 code does not need to write return pc which follows the
  // VM callout to the thread anchor pc slot. the frame walking code
  // can safely compute that a called the stub's return pc by
  // evaluating sp[-1] where sp is the top of the stub frame saved in
  // the anchor sp field. AArch64 cannot make this assumption because
  // the C compiler does not have to push the link register and, even
  // if it does, can locate it somewhere other than at the bottom of
  // the frame.

  // this poses a problem when we come to generate the opto runtime
  // stubs. we cannot actually refer to generated code addresses in
  // ideal code and, worse, we need to get the memory write to write a
  // constant value derived form the code address of the instruction
  // following the runtime callout i.e. the constant we need is only
  // determined when we plant a separate instruction later in the
  // instruction stream.
  // 
  // rather than introduce a mechanism into the opto compiler to allow
  // us to provide some sort of label and label resolve mechanism and
  // then backpatch the required address we can actually do this with
  // a minor hack. we generate a store to the anchor pc field but
  // supply a small negative constant address (-1 or -2) as the value
  // for the memory store. in the back end we detect this type of
  // store and compute the desired address, substituting it in place
  // of the supplied constant.
  // 
  // the back end employs a special lowering rule to do this. it
  // matches a write via the thread register with an offset which
  // equals the thread anchor's pc slot. of course, those sort of
  // writes only happen in these stubs! the encodign for this rule
  // substitutes the supplied value with the current code buffer
  // address plus an offset to the instruction following the VM
  // callout. Of course, the offset varies according to what arguments
  // are passed to the callout. For any given stub the arguments
  // passed to the VM include the stub arguments, the thread and,
  // optionally, the stub's caller's return pc. We supply -1 in the
  // store if the call will not include the caller ret pc or -2 if it
  // does. The back end can use this to work out exactly what the
  // required offset is.

  const TypeRawPtr *t = TypeRawPtr::make((address)(return_pc ? -2L : -1L));

  Node *last_pc = new (C) ConPNode(t);
  _gvn.set_type(last_pc, t);
  store_to_memory(NULL, adr_last_Java_pc, last_pc, T_ADDRESS, NoAlias);

#endif /* defined(AARCH64) */

  // Drop in the last_Java_sp.  last_Java_fp is not touched.
  // Always do this after the other "last_Java_frame" fields are set since
  // as soon as last_Java_sp != NULL the has_last_Java_frame is true and
  // users will look at the other fields.
  //
  Node *adr_sp = basic_plus_adr(top(), thread, in_bytes(JavaThread::last_Java_sp_offset()));
  Node *last_sp = basic_plus_adr(top(), frameptr(), (intptr_t) STACK_BIAS);
  store_to_memory(NULL, adr_sp, last_sp, T_ADDRESS, NoAlias, MemNode::unordered);

  // Set _thread_in_native
  // The order of stores into TLS is critical!  Setting _thread_in_native MUST
  // be last, because a GC is allowed at any time after setting it and the GC
  // will require last_Java_pc and last_Java_sp.

  //-----------------------------
  // Compute signature for C call.  Varies from the Java signature!
  const Type **fields = TypeTuple::fields(2*parm_cnt+2);
  uint cnt = TypeFunc::Parms;
  // The C routines gets the base of thread-local storage passed in as an
  // extra argument.  Not all calls need it, but its cheap to add here.
  for (uint pcnt = cnt; pcnt < parm_cnt; pcnt++, cnt++) {
    // Convert ints to longs if required.
    if (CCallingConventionRequiresIntsAsLongs && jdomain->field_at(pcnt)->isa_int()) {
      fields[cnt++] = TypeLong::LONG;
      fields[cnt]   = Type::HALF; // must add an additional half for a long
    } else {
      fields[cnt] = jdomain->field_at(pcnt);
    }
  }

  fields[cnt++] = TypeRawPtr::BOTTOM; // Thread-local storage
  // Also pass in the caller's PC, if asked for.
  if( return_pc )
    fields[cnt++] = TypeRawPtr::BOTTOM; // Return PC

  const TypeTuple* domain = TypeTuple::make(cnt,fields);
  // The C routine we are about to call cannot return an oop; it can block on
  // exit and a GC will trash the oop while it sits in C-land.  Instead, we
  // return the oop through TLS for runtime calls.
  // Also, C routines returning integer subword values leave the high
  // order bits dirty; these must be cleaned up by explicit sign extension.
  const Type* retval = (jrange->cnt() == TypeFunc::Parms) ? Type::TOP : jrange->field_at(TypeFunc::Parms);
  // Make a private copy of jrange->fields();
  const Type **rfields = TypeTuple::fields(jrange->cnt() - TypeFunc::Parms);
  // Fixup oop returns
  int retval_ptr = retval->isa_oop_ptr();
  if( retval_ptr ) {
    assert( pass_tls, "Oop must be returned thru TLS" );
    // Fancy-jumps return address; others return void
    rfields[TypeFunc::Parms] = is_fancy_jump ? TypeRawPtr::BOTTOM : Type::TOP;

  } else if( retval->isa_int() ) { // Returning any integer subtype?
    // "Fatten" byte, char & short return types to 'int' to show that
    // the native C code can return values with junk high order bits.
    // We'll sign-extend it below later.
    rfields[TypeFunc::Parms] = TypeInt::INT; // It's "dirty" and needs sign-ext

  } else if( jrange->cnt() >= TypeFunc::Parms+1 ) { // Else copy other types
    rfields[TypeFunc::Parms] = jrange->field_at(TypeFunc::Parms);
    if( jrange->cnt() == TypeFunc::Parms+2 )
      rfields[TypeFunc::Parms+1] = jrange->field_at(TypeFunc::Parms+1);
  }
  const TypeTuple* range = TypeTuple::make(jrange->cnt(),rfields);

  // Final C signature
  const TypeFunc *c_sig = TypeFunc::make(domain,range);

  //-----------------------------
  // Make the call node
  CallRuntimeNode *call = new (C)
    CallRuntimeNode(c_sig, C_function, name, TypePtr::BOTTOM);
  //-----------------------------

  // Fix-up the debug info for the call
  call->set_jvms( new (C) JVMState(0) );
  call->jvms()->set_bci(0);
  call->jvms()->set_offsets(cnt);

  // Set fixed predefined input arguments
  cnt = 0;
  for (i = 0; i < TypeFunc::Parms; i++)
    call->init_req(cnt++, map()->in(i));
  // A little too aggressive on the parm copy; return address is not an input
  call->set_req(TypeFunc::ReturnAdr, top());
  for (; i < parm_cnt; i++) { // Regular input arguments
    // Convert ints to longs if required.
    if (CCallingConventionRequiresIntsAsLongs && jdomain->field_at(i)->isa_int()) {
      Node* int_as_long = _gvn.transform(new (C) ConvI2LNode(map()->in(i)));
      call->init_req(cnt++, int_as_long); // long
      call->init_req(cnt++, top());       // half
    } else {
      call->init_req(cnt++, map()->in(i));
    }
  }

  call->init_req( cnt++, thread );
  if( return_pc )             // Return PC, if asked for
    call->init_req( cnt++, returnadr() );
  _gvn.transform_no_reclaim(call);


  //-----------------------------
  // Now set up the return results
  set_control( _gvn.transform( new (C) ProjNode(call,TypeFunc::Control)) );
  set_i_o(     _gvn.transform( new (C) ProjNode(call,TypeFunc::I_O    )) );
  set_all_memory_call(call);
  if (range->cnt() > TypeFunc::Parms) {
    Node* retnode = _gvn.transform( new (C) ProjNode(call,TypeFunc::Parms) );
    // C-land is allowed to return sub-word values.  Convert to integer type.
    assert( retval != Type::TOP, "" );
    if (retval == TypeInt::BOOL) {
      retnode = _gvn.transform( new (C) AndINode(retnode, intcon(0xFF)) );
    } else if (retval == TypeInt::CHAR) {
      retnode = _gvn.transform( new (C) AndINode(retnode, intcon(0xFFFF)) );
    } else if (retval == TypeInt::BYTE) {
      retnode = _gvn.transform( new (C) LShiftINode(retnode, intcon(24)) );
      retnode = _gvn.transform( new (C) RShiftINode(retnode, intcon(24)) );
    } else if (retval == TypeInt::SHORT) {
      retnode = _gvn.transform( new (C) LShiftINode(retnode, intcon(16)) );
      retnode = _gvn.transform( new (C) RShiftINode(retnode, intcon(16)) );
    }
    map()->set_req( TypeFunc::Parms, retnode );
  }

  //-----------------------------

  // Clear last_Java_sp
  store_to_memory(NULL, adr_sp, null(), T_ADDRESS, NoAlias, MemNode::unordered);
  // Clear last_Java_pc and (optionally)_flags
  store_to_memory(NULL, adr_last_Java_pc, null(), T_ADDRESS, NoAlias, MemNode::unordered);
#if defined(SPARC)
  store_to_memory(NULL, adr_flags, intcon(0), T_INT, NoAlias, MemNode::unordered);
#endif /* defined(SPARC) */
#if (defined(IA64) && !defined(AIX))
  Node* adr_last_Java_fp = basic_plus_adr(top(), thread, in_bytes(JavaThread::last_Java_fp_offset()));
  store_to_memory(NULL, adr_last_Java_fp, null(), T_ADDRESS, NoAlias, MemNode::unordered);
#endif

  // For is-fancy-jump, the C-return value is also the branch target
  Node* target = map()->in(TypeFunc::Parms);
  // Runtime call returning oop in TLS?  Fetch it out
  if( pass_tls ) {
    Node* adr = basic_plus_adr(top(), thread, in_bytes(JavaThread::vm_result_offset()));
    Node* vm_result = make_load(NULL, adr, TypeOopPtr::BOTTOM, T_OBJECT, NoAlias, MemNode::unordered);
    map()->set_req(TypeFunc::Parms, vm_result); // vm_result passed as result
    // clear thread-local-storage(tls)
    store_to_memory(NULL, adr, null(), T_ADDRESS, NoAlias, MemNode::unordered);
  }

  //-----------------------------
  // check exception
  Node* adr = basic_plus_adr(top(), thread, in_bytes(Thread::pending_exception_offset()));
  Node* pending = make_load(NULL, adr, TypeOopPtr::BOTTOM, T_OBJECT, NoAlias, MemNode::unordered);

  Node* exit_memory = reset_memory();

  Node* cmp = _gvn.transform( new (C) CmpPNode(pending, null()) );
  Node* bo  = _gvn.transform( new (C) BoolNode(cmp, BoolTest::ne) );
  IfNode   *iff = create_and_map_if(control(), bo, PROB_MIN, COUNT_UNKNOWN);

  Node* if_null     = _gvn.transform( new (C) IfFalseNode(iff) );
  Node* if_not_null = _gvn.transform( new (C) IfTrueNode(iff)  );

  assert (StubRoutines::forward_exception_entry() != NULL, "must be generated before");
  Node *exc_target = makecon(TypeRawPtr::make( StubRoutines::forward_exception_entry() ));
  Node *to_exc = new (C) TailCallNode(if_not_null,
                                      i_o(),
                                      exit_memory,
                                      frameptr(),
                                      returnadr(),
                                      exc_target, null());
  root()->add_req(_gvn.transform(to_exc));  // bind to root to keep live
  C->init_start(start);

  //-----------------------------
  // If this is a normal subroutine return, issue the return and be done.
  Node *ret = NULL;
  switch( is_fancy_jump ) {
  case 0:                       // Make a return instruction
    // Return to caller, free any space for return address
    ret = new (C) ReturnNode(TypeFunc::Parms, if_null,
                             i_o(),
                             exit_memory,
                             frameptr(),
                             returnadr());
    if (C->tf()->range()->cnt() > TypeFunc::Parms)
      ret->add_req( map()->in(TypeFunc::Parms) );
    break;
  case 1:    // This is a fancy tail-call jump.  Jump to computed address.
    // Jump to new callee; leave old return address alone.
    ret = new (C) TailCallNode(if_null,
                               i_o(),
                               exit_memory,
                               frameptr(),
                               returnadr(),
                               target, map()->in(TypeFunc::Parms));
    break;
  case 2:                       // Pop return address & jump
    // Throw away old return address; jump to new computed address
    //assert(C_function == CAST_FROM_FN_PTR(address, OptoRuntime::rethrow_C), "fancy_jump==2 only for rethrow");
    ret = new (C) TailJumpNode(if_null,
                               i_o(),
                               exit_memory,
                               frameptr(),
                               target, map()->in(TypeFunc::Parms));
    break;
  default:
    ShouldNotReachHere();
  }
  root()->add_req(_gvn.transform(ret));
}
