/*
 * Copyright (c) 1997, 2019, Oracle and/or its affiliates. All rights reserved.
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
#include "oops/markOop.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/objectMonitor.hpp"

void markWord::print_on(outputStream* st) const {
  if (is_marked()) {  // last bits = 11
    st->print(" marked(" INTPTR_FORMAT ")", value());
  } else if (has_monitor()) {  // last bits = 10
    // have to check has_monitor() before is_locked()
    st->print(" monitor(" INTPTR_FORMAT ")=", value());
    ObjectMonitor* mon = monitor();
    if (mon == NULL) {
      st->print("NULL (this should never be seen!)");
    } else {
      mon->print_on(st);
    }
  } else if (is_locked()) {  // last bits != 01 => 00
    // thin locked
    st->print(" locked(" INTPTR_FORMAT ")", value());
  } else {
    st->print(" mark(");
    // Biased bit is 3rd rightmost bit
    if (is_neutral()) {   // last bits = 001
      st->print("is_neutral");
      if (has_no_hash()) {
        st->print(" no_hash");
      } else {
        st->print(" hash=" INTPTR_FORMAT, hash());
      }
    } else if (has_bias_pattern()) {  // last bits = 101
      st->print("is_biased");
      JavaThread* jt = biased_locker();
      st->print(" biased_locker=" INTPTR_FORMAT " epoch=%d", p2i(jt), bias_epoch());
    } else {
      st->print("??");
    }
    st->print(" age=%d)", age());
  }
}
