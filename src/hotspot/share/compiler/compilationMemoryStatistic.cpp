/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2023, Red Hat, Inc. and/or its affiliates.
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
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "compiler/abstractCompiler.hpp"
#include "compiler/compilationMemoryStatistic.hpp"
#include "compiler/compilerDirectives.hpp"
#include "compiler/compileTask.hpp"
#include "compiler/compilerDefinitions.hpp"
#include "compiler/compilerThread.hpp"
#include "memory/arena.hpp"
#include "memory/resourceArea.hpp"
#include "oops/symbol.hpp"
#ifdef COMPILER2
#include "opto/node.hpp" // compile.hpp is not self-contained
#include "opto/compile.hpp"
#endif
#include "services/nmtCommon.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"
#include "utilities/quickSort.hpp"
#include "utilities/resourceHash.hpp"


ArenaStatCounter::ArenaStatCounter() :
  _current(0), _start(0), _peak(0),
  _na(0), _ra(0),
  _na_at_peak(0), _ra_at_peak(0), _live_nodes_at_peak(0)
{}

size_t ArenaStatCounter::peak_since_start() const {
  return _peak > _start ? _peak - _start : 0;
}

void ArenaStatCounter::start() {
  _peak = _start = _current;
}

void ArenaStatCounter::update_c2_node_count() {
#ifdef COMPILER2
  CompilerThread* const th = Thread::current()->as_Compiler_thread();
  const CompileTask* const task = th->task();
  if (task != nullptr &&
      th->task()->compiler() != nullptr &&
      th->task()->compiler()->type() == compiler_c2) {
    const Compile* const comp = Compile::current();
    if (comp != nullptr) {
      _live_nodes_at_peak = comp->live_nodes();
    }
  }
#endif
}

// Account an arena allocation or de-allocation.
bool ArenaStatCounter::account(ssize_t delta, int tag) {
  bool rc = false;
#ifdef ASSERT
  // Note: if this fires, we free more arena memory under the scope of the
  // CompilationMemoryHistoryMark than we allocate. This cannot be since we
  // assume arena allocations in CompilerThread to be stack bound and symmetric.
  assert(delta >= 0 || ((ssize_t)_current + delta) >= 0,
         "Negative overflow (d=%zd %zu %zu %zu)", delta, _current, _start, _peak);
#endif
  // Update totals
  _current += delta;
  // Update detail counter
  switch ((Arena::Tag)tag) {
    case Arena::Tag::tag_ra: _ra += delta; break;
    case Arena::Tag::tag_node: _na += delta; break;
    default: // ignore
      break;
  };
  // Did we reach a peak?
  if (_current > _peak) {
    _peak = _current;
    assert(delta > 0, "Sanity (%zu %zu %zu)", _current, _start, _peak);
    _na_at_peak = _na;
    _ra_at_peak = _ra;
    update_c2_node_count();
    rc = true;
  }
  return rc;
}

void ArenaStatCounter::print_on(outputStream* st) const {
  st->print("%zu [na %zu ra %zu]", peak_since_start(), _na_at_peak, _ra_at_peak);
#ifdef ASSERT
  st->print(" (%zu->%zu->%zu)", _start, _peak, _current);
#endif
}

//////////////////////////
// Backend

class FullMethodName {
  Symbol* const _k;
  Symbol* const _m;
  Symbol* const _s;

public:

  FullMethodName(Symbol* k, Symbol* m, Symbol* s) : _k(k), _m(m), _s(s) {}
  FullMethodName(const FullMethodName& o) : _k(o._k), _m(o._m), _s(o._s) {}

  void make_permanent() {
    _k->make_permanent();
    _m->make_permanent();
    _s->make_permanent();
  }

  static unsigned compute_hash(const FullMethodName& n) {
    return Symbol::compute_hash(n._k) ^
        Symbol::compute_hash(n._m) ^
        Symbol::compute_hash(n._s);
  }

  char* as_C_string(char* buf, size_t len) const {
    stringStream ss(buf, len);
    ResourceMark rm;
    ss.print_raw(_k->as_C_string());
    ss.print_raw("::");
    ss.print_raw(_m->as_C_string());
    ss.put('(');
    ss.print_raw(_s->as_C_string());
    ss.put(')');
    return buf;
  }

  bool equals(const FullMethodName& b) const {
    return _k == b._k && _m == b._m && _s == b._s;
  }

  bool operator== (const FullMethodName& other) const { return equals(other); }
};

// Note: not mtCompiler since we don't want to change what we measure
class MemStatEntry : public CHeapObj<mtInternal> {
  const FullMethodName _method;
  CompilerType _comptype;
  double _time;
  // How often this has been recompiled.
  int _num_recomp;
  // Compiling thread. Only for diagnostic purposes. Thread may not be alive anymore.
  const Thread* _thread;

  size_t _total;
  size_t _na_at_peak;
  size_t _ra_at_peak;
  unsigned _live_nodes_at_peak;

public:

  MemStatEntry(FullMethodName method)
    : _method(method), _comptype(compiler_c1),
      _time(0), _num_recomp(0), _thread(nullptr),
      _total(0), _na_at_peak(0), _ra_at_peak(0), _live_nodes_at_peak(0) {
  }

  void set_comptype(CompilerType comptype) { _comptype = comptype; }
  void set_current_time() { _time = os::elapsedTime(); }
  void set_current_thread() { _thread = Thread::current(); }
  void inc_recompilation() { _num_recomp++; }

  void set_total(size_t n) { _total = n; }
  void set_na_at_peak(size_t n) { _na_at_peak = n; }
  void set_ra_at_peak(size_t n) { _ra_at_peak = n; }
  void set_live_nodes_at_peak(unsigned n) { _live_nodes_at_peak = n; }

  size_t total() const { return _total; }

  static void print_legend(outputStream* st) {
    st->print_cr("Legend:");
    st->print_cr("  total  : memory allocated via arenas while compiling");
    st->print_cr("  NA     : ...how much in node arenas (if c2)");
    st->print_cr("  RA     : ...how much in resource areas");
    st->print_cr("  #nodes : ...how many nodes (if c2)");
    st->print_cr("  time   : time of last compilation (sec)");
    st->print_cr("  type   : compiler type");
    st->print_cr("  #rc    : how often recompiled");
    st->print_cr("  thread : compiler thread");
  }

  static void print_header(outputStream* st) {
    st->print_cr("total     NA        RA        #nodes  time    type  #rc thread              method");
  }

  void print_on(outputStream* st, bool human_readable) const {
    int col = 0;

    // Total
    if (human_readable) {
      st->print(PROPERFMT " ", PROPERFMTARGS(_total));
    } else {
      st->print("%zu ", _total);
    }
    col += 10; st->fill_to(col);

    // NA
    if (human_readable) {
      st->print(PROPERFMT " ", PROPERFMTARGS(_na_at_peak));
    } else {
      st->print("%zu ", _na_at_peak);
    }
    col += 10; st->fill_to(col);

    // RA
    if (human_readable) {
      st->print(PROPERFMT " ", PROPERFMTARGS(_ra_at_peak));
    } else {
      st->print("%zu ", _ra_at_peak);
    }
    col += 10; st->fill_to(col);

    // Number of Nodes when memory peaked
    st->print("%u ", _live_nodes_at_peak);
    col += 8; st->fill_to(col);

    // TimeStamp
    st->print("%.3f ", _time);
    col += 8; st->fill_to(col);

    // Type
    st->print("%s ", compilertype2name(_comptype));
    col += 6; st->fill_to(col);

    // Recomp
    st->print("%u ", _num_recomp);
    col += 4; st->fill_to(col);

    // Thread
    st->print(PTR_FORMAT "  ", p2i(_thread));

    // MethodName
    char buf[1024];
    st->print("%s ", _method.as_C_string(buf, sizeof(buf)));
    st->cr();
  }

  int compare_by_size(const MemStatEntry* b) const {
    const size_t x1 = b->_total;
    const size_t x2 = _total;
    return x1 < x2 ? -1 : x1 == x2 ? 0 : 1;
  }

  bool equals(const FullMethodName& b) const {
    return _method.equals(b);
  }
};

class MemStatTable :
    public ResourceHashtable<FullMethodName, MemStatEntry*, 7919, AnyObj::C_HEAP,
                             mtInternal, FullMethodName::compute_hash>
{
public:

  void add(const FullMethodName& fmn, CompilerType comptype,
           size_t total, size_t na_at_peak, size_t ra_at_peak,
           unsigned live_nodes_at_peak) {
    assert_lock_strong(NMTCompilationCostHistory_lock);

    MemStatEntry** pe = get(fmn);
    MemStatEntry* e = nullptr;
    if (pe == nullptr) {
      e = new MemStatEntry(fmn);
      put(fmn, e);
    } else {
      // Update existing entry
      e = *pe;
      assert(e != nullptr, "Sanity");
    }
    e->set_current_time();
    e->set_current_thread();
    e->set_comptype(comptype);
    e->inc_recompilation();
    e->set_total(total);
    e->set_na_at_peak(na_at_peak);
    e->set_ra_at_peak(ra_at_peak);
    e->set_live_nodes_at_peak(live_nodes_at_peak);
  }

  // Returns a C-heap-allocated SortMe array containing all entries from the table,
  // optionally filtered by entry size
  MemStatEntry** calc_flat_array(int& num, size_t min_size) {
    assert_lock_strong(NMTCompilationCostHistory_lock);

    const int num_all = number_of_entries();
    MemStatEntry** flat = NEW_C_HEAP_ARRAY(MemStatEntry*, num_all, mtInternal);
    int i = 0;
    auto do_f = [&] (const FullMethodName& ignored, MemStatEntry* e) {
      if (e->total() >= min_size) {
        flat[i] = e;
        assert(i < num_all, "Sanity");
        i ++;
      }
    };
    iterate_all(do_f);
    if (min_size == 0) {
      assert(i == num_all, "Sanity");
    } else {
      assert(i <= num_all, "Sanity");
    }
    num = i;
    return flat;
  }
};

bool CompilationMemoryStatistic::_enabled = false;

static MemStatTable* _the_table = nullptr;

void CompilationMemoryStatistic::initialize() {
  assert(_enabled == false && _the_table == nullptr, "Only once");
  _the_table = new (mtCompiler) MemStatTable;
  _enabled = true;
  log_info(compilation, alloc)("Compilation memory statistic enabled");
}

void CompilationMemoryStatistic::on_start_compilation() {
  assert(enabled(), "Not enabled?");
  Thread::current()->as_Compiler_thread()->arena_stat()->start();
}

void CompilationMemoryStatistic::on_end_compilation() {
  assert(enabled(), "Not enabled?");
  ResourceMark rm;
  CompilerThread* const th = Thread::current()->as_Compiler_thread();
  const ArenaStatCounter* const arena_stat = th->arena_stat();
  const CompilerType ct = th->task()->compiler()->type();

  const Method* const m = th->task()->method();
  FullMethodName fmn(m->klass_name(), m->name(), m->signature());
  fmn.make_permanent();

  const DirectiveSet* directive = th->task()->directive();
  assert(directive->should_collect_memstat(), "Only call if memstat is enabled");
  const bool print = directive->should_print_memstat();

  if (print) {
    char buf[1024];
    fmn.as_C_string(buf, sizeof(buf));
    tty->print("%s Arena usage %s: ", compilertype2name(ct), buf);
    arena_stat->print_on(tty);
    tty->cr();
  }
  {
    MutexLocker ml(NMTCompilationCostHistory_lock, Mutex::_no_safepoint_check_flag);
    assert(_the_table != nullptr, "not initialized");

    _the_table->add(fmn, ct,
                    arena_stat->peak_since_start(), // total
                    arena_stat->na_at_peak(),
                    arena_stat->ra_at_peak(),
                    arena_stat->live_nodes_at_peak());
  }
}

void CompilationMemoryStatistic::on_arena_change(ssize_t diff, const Arena* arena) {
  assert(enabled(), "Not enabled?");
  CompilerThread* const th = Thread::current()->as_Compiler_thread();
  th->arena_stat()->account(diff, (int)arena->get_tag());
}

static inline ssize_t diff_entries_by_size(const MemStatEntry* e1, const MemStatEntry* e2) {
  return e1->compare_by_size(e2);
}

void CompilationMemoryStatistic::print_all_by_size(outputStream* st, bool human_readable, size_t min_size) {
  st->print_cr("Compilation memory statistics");

  if (!enabled()) {
    st->print_cr("(unavailable)");
    return;
  }

  st->cr();

  MemStatEntry::print_legend(st);
  st->cr();

  if (min_size > 0) {
    st->print_cr(" (cutoff: %zu bytes)", min_size);
  }
  st->cr();

  MemStatEntry::print_header(st);

  MemStatEntry** filtered = nullptr;
  {
    MutexLocker ml(NMTCompilationCostHistory_lock, Mutex::_no_safepoint_check_flag);

    if (_the_table != nullptr) {
      // We sort with quicksort
      int num = 0;
      filtered = _the_table->calc_flat_array(num, min_size);
      if (min_size > 0) {
        st->print_cr("(%d/%d)", num, _the_table->number_of_entries());
      }
      if (num > 0) {
        QuickSort::sort(filtered, num, diff_entries_by_size, false);
        // Now print. Has to happen under lock protection too, since entries may be changed.
        for (int i = 0; i < num; i ++) {
          filtered[i]->print_on(st, human_readable);
        }
      } else {
        st->print_cr("No entries.");
      }
    } else {
      st->print_cr("Not initialized.");
    }
  } // locked

  FREE_C_HEAP_ARRAY(Entry, filtered);
}

CompilationMemoryStatisticMark::CompilationMemoryStatisticMark(const DirectiveSet* directive)
  : _active(directive->should_collect_memstat()) {
  if (_active) {
    CompilationMemoryStatistic::on_start_compilation();
  }
}
CompilationMemoryStatisticMark::~CompilationMemoryStatisticMark() {
  if (_active) {
    CompilationMemoryStatistic::on_end_compilation();
  }
}
