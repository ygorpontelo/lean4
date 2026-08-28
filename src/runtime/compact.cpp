/*
Copyright (c) 2018 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include <algorithm>
#include <string>
#include <vector>
#include <cstring>
#include <lean/lean.h>
#include "runtime/hash.h"
#include "runtime/compact.h"
#include "runtime/exception.h"
#include "util/alloc.h"

// Defined in ir_interpreter.cpp (namespace lean::ir); reverse-looks-up a function
// address to its declared symbol name via the interpreter's symbol cache.
namespace lean::ir { std::string lookup_symbol_name(void * addr); }

#ifdef LEAN_WINDOWS
#include <windows.h>  // must precede <psapi.h>: it relies on `WINBOOL`/`DWORD`/`WINAPI` from here
#include <psapi.h>
#else
#include <sys/mman.h>
#include <dlfcn.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#elif !defined(LEAN_WINDOWS)
#include <link.h>
#endif

#define LEAN_COMPACTOR_INIT_SZ 1024*1024
#define LEAN_MAX_SHARING_TABLE_INITIAL_SIZE 1024*1024

// uncomment to track the number of each kind of object in an .olean file
// #define LEAN_TAG_COUNTERS

namespace lean {

struct max_sharing_key {
    size_t m_offset;
    size_t m_size;
    max_sharing_key(size_t offset, size_t sz):m_offset(offset), m_size(sz) {}
};

struct max_sharing_hash {
    object_compactor * m;
    max_sharing_hash(object_compactor * manager):m(manager) {}
    unsigned operator()(max_sharing_key const & k) const {
        return hash_str(k.m_size, reinterpret_cast<unsigned char const *>(m->m_begin) + k.m_offset, 17);
    }
};

struct max_sharing_eq {
    object_compactor * m;
    max_sharing_eq(object_compactor * manager):m(manager) {}
    bool operator()(max_sharing_key const & k1, max_sharing_key const & k2) const {
        if (k1.m_size != k2.m_size) return false;
        return memcmp(reinterpret_cast<char*>(m->m_begin) + k1.m_offset, reinterpret_cast<char*>(m->m_begin) + k2.m_offset, k1.m_size) == 0;
    }
};


struct object_compactor::max_sharing_table {
    lean::unordered_set<max_sharing_key, max_sharing_hash, max_sharing_eq> m_table;
    max_sharing_table(object_compactor * manager):
        m_table(LEAN_MAX_SHARING_TABLE_INITIAL_SIZE, max_sharing_hash(manager), max_sharing_eq(manager)) {
    }
};

LEAN_EXPORT std::vector<lib_info> get_loaded_libs() {
    std::vector<lib_info> libs;
#ifdef LEAN_WINDOWS
    HANDLE proc = GetCurrentProcess();
    std::vector<HMODULE> mods(128);
    DWORD needed;
    if (!EnumProcessModules(proc, mods.data(), mods.size() * sizeof(HMODULE), &needed))
        return libs;
    unsigned n = needed / sizeof(HMODULE);
    if (n > mods.size()) {
        mods.resize(n);
        if (!EnumProcessModules(proc, mods.data(), mods.size() * sizeof(HMODULE), &needed))
            return libs;
    } else {
        mods.resize(n);
    }
    for (HMODULE h : mods) {
        MODULEINFO mi;
        if (!GetModuleInformation(proc, h, &mi, sizeof(mi))) continue;
        char path[MAX_PATH];
        if (!GetModuleFileNameA(h, path, sizeof(path))) continue;
        libs.push_back({reinterpret_cast<size_t>(mi.lpBaseOfDll), path});
    }
#elif defined(__APPLE__)
    uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct mach_header * hdr = _dyld_get_image_header(i);
        if (!hdr) continue;
        const char * name = _dyld_get_image_name(i);
        if (!name) continue;
        libs.push_back({reinterpret_cast<size_t>(hdr), name});
    }
#elif defined(__EMSCRIPTEN__)
    // No dynamic-loader introspection under Emscripten. Closure saves on WASM use the
    // cross-compile symbol-name table (flags bit 2), never the lib relocation table.
    return libs;
#else
    // Linux: use dl_iterate_phdr
    dl_iterate_phdr([](struct dl_phdr_info * info, size_t, void * data) -> int {
        std::vector<lib_info> * libs = static_cast<std::vector<lib_info> *>(data);
        const char * name = info->dlpi_name;
        libs->push_back({info->dlpi_addr, name ? name : ""});
        return 0;
    }, &libs);
#endif
    std::sort(libs.begin(), libs.end(), [](lib_info const & a, lib_info const & b) { return a.base_addr < b.base_addr; });
    return libs;
}

std::vector<lib_info> object_compactor::used_libs() const {
    // Mark each loaded library that contains a compacted closure's `m_fun` pointer. The recorded
    // offsets are relative to `m_begin`, and `m_fun` still holds the raw (un-relocated) code
    // pointer at this point, so it identifies the library directly.
    std::vector<bool> used(m_libs.size(), false);
    for (size_t off : m_closure_offsets) {
        size_t fn = reinterpret_cast<size_t>(
            *reinterpret_cast<void * const *>(static_cast<char const *>(m_begin) + off));
        std::vector<lib_info>::const_iterator it =
            std::upper_bound(m_libs.begin(), m_libs.end(), fn,
                [](size_t addr, lib_info const & lib) { return addr < lib.base_addr; });
        if (it == m_libs.begin())
            throw exception("closure function pointer does not belong to any loaded library");
        used[(it - 1) - m_libs.begin()] = true;
    }
    std::vector<lib_info> result;
    for (size_t i = 0; i < m_libs.size(); i++)
        if (used[i]) result.push_back(m_libs[i]);
    return result;
}

object_compactor::object_compactor(void * base_addr, std::vector<region_view> dep_regions,
                                   bool allow_closures, uint8 target_ptr_width):
    m_max_sharing_table(new max_sharing_table(this)),
    m_dep_regions(std::move(dep_regions)),
    m_libs(get_loaded_libs()),
    m_allow_closures(allow_closures),
    m_target_ptr_width(target_ptr_width),
    m_base_addr(base_addr),
    m_begin(malloc(LEAN_COMPACTOR_INIT_SZ)),
    m_end(m_begin),
    m_capacity(static_cast<char*>(m_begin) + LEAN_COMPACTOR_INIT_SZ) {
    // Sort dep regions by `begin` address for binary search in `to_offset`.
    std::sort(m_dep_regions.begin(), m_dep_regions.end(),
              [](region_view const & a, region_view const & b) { return a.begin < b.begin; });
}

object_compactor::~object_compactor() {
    free(m_begin);
}

/*
  Remark: g_null_offset must NOT be a valid Lean scalar value (e.g., static_cast<size_t>(-1)).
  Recall that Lean scalar are odd size_t values. So, we use (static_cast<size_t>(-1) - 1) which is an even number.
  In the past we used `static_cast<size_t>(-1)`, and it caused nontermination in the object compactor.
*/
object_offset g_null_offset = reinterpret_cast<object_offset>(static_cast<size_t>(-1) - 1);

void object_compactor::write_tgt_size(void * ptr, size_t val, uint8 width) {
    if (width == 4) {
        uint32_t v = static_cast<uint32_t>(val);
        memcpy(ptr, &v, sizeof(v));
    } else {
        memcpy(ptr, &val, sizeof(val));
    }
}

void object_compactor::write_tgt_ptr(void * ptr, size_t val, uint8 width) {
    write_tgt_size(ptr, val, width);
}

void object_compactor::emit_target_header(char * mem, object * o, size_t obj_sz, unsigned tag, unsigned other) {
    memcpy(mem, o, sizeof(lean_object));
    lean_set_non_heap_header(reinterpret_cast<lean_object*>(mem), obj_sz, tag, other);
}

void * object_compactor::alloc(size_t sz) {
    // In cross-compile mode, align to the target pointer width so target struct
    // offsets are correct. In native mode, align to host sizeof(void*) as before.
    size_t align = is_cross_compile() ? m_target_ptr_width : sizeof(void*);
    size_t rem = sz % align;
    if (rem != 0)
        sz = sz + align - rem;
    while (static_cast<char*>(m_end) + sz > m_capacity) {
        size_t new_capacity = capacity()*2;
        void * new_begin = malloc(new_capacity);
        memcpy(new_begin, m_begin, size());
        m_end      = static_cast<char*>(new_begin) + size();
        m_capacity = static_cast<char*>(new_begin) + new_capacity;
        free(m_begin);
        m_begin    = new_begin;
    }
    void * r = m_end;
    memset(r, 0, sz);
    m_end = static_cast<char*>(m_end) + sz;
    lean_assert(m_end <= m_capacity);
    return r;
}

object_offset object_compactor::save(object * o, object * new_o) {
    lean_assert(m_begin <= new_o && new_o < m_end);
    object_offset off = reinterpret_cast<object_offset>(reinterpret_cast<char*>(new_o) - reinterpret_cast<char*>(m_begin) + reinterpret_cast<size_t>(m_base_addr));
    m_obj_table.insert(std::make_pair(o, off));
    return off;
}

// In cross-compile mode, `new_o` points to a target-layout serialization (not a native
// struct). The `max_sharing_key` byte range and `memcmp` comparison operate on these
// target-layout bytes, which correctly identifies structurally identical objects since
// identical source objects produce identical target-layout bytes.
object_offset object_compactor::save_max_sharing(object * o, object * new_o, size_t new_o_sz) {
    max_sharing_key k(reinterpret_cast<char*>(new_o) - reinterpret_cast<char*>(m_begin), new_o_sz);
    auto it = m_max_sharing_table->m_table.find(k);
    if (it != m_max_sharing_table->m_table.end()) {
        m_end = new_o;
        new_o = reinterpret_cast<lean_object*>(reinterpret_cast<char*>(m_begin) + it->m_offset);
    } else {
        m_max_sharing_table->m_table.insert(k);
    }
    return save(o, new_o);
}

object_offset object_compactor::to_offset(object * o) {
    if (lean_is_scalar(o)) {
        if (is_cross_compile()) {
            // Scalar promotion: a 64-bit-host scalar may exceed the 32-bit target's
            // tagged range (SIZE_MAX>>1 at target width). Promote to boxed mpz.
            size_t n = lean_unbox(o);
            size_t tgt_max = (static_cast<size_t>(1) << (m_target_ptr_width * 8 - 1)) - 1;
            if (n > tgt_max) {
                // Create a boxed mpz for n and compact it
                object * mpz_o = alloc_mpz(mpz::of_size_t(n));
                object_offset off = compact(mpz_o);
                lean_dec(mpz_o);
                return off;
            }
        }
        return o;
    } else {
        auto it = m_obj_table.find(o);
        if (it != m_obj_table.end()) {
            return it->second;
        }
        // Only check dep regions for non-heap objects
        if (!m_dep_regions.empty() && !lean_has_rc(o)) {
            // Binary search dep regions (sorted by begin)
            char * addr = reinterpret_cast<char *>(o);
            // Find the first region whose begin > addr, then step back
            std::vector<region_view>::iterator upper = std::upper_bound(
                m_dep_regions.begin(), m_dep_regions.end(), addr,
                [](char * a, region_view const & r) { return a < static_cast<char *>(r.begin); });
            if (upper != m_dep_regions.begin()) {
                region_view const & region = *(upper - 1);
                char * region_end = static_cast<char *>(region.begin) + region.size;
                if (addr < region_end) {
                    // Object is in this dep region, compute its base_addr-relative pointer
                    object_offset off = reinterpret_cast<object_offset>(
                        reinterpret_cast<size_t>(region.base_addr) + (addr - static_cast<char *>(region.begin)));
                    m_obj_table.insert(std::make_pair(o, off));
                    return off;
                }
            }
        }
        return compact(o);
    }
}

object_offset object_compactor::compact(object * o) {
    lean_assert(!lean_is_scalar(o));
#ifdef LEAN_TAG_COUNTERS
    g_tag_counters[lean_ptr_tag(o)]++;
#endif
    switch (lean_ptr_tag(o)) {
    case LeanClosure:         return insert_closure(o);
    case LeanArray:           return insert_array(o);
    case LeanScalarArray:     return insert_sarray(o);
    case LeanString:          return insert_string(o);
    case LeanMPZ:             return insert_mpz(o);
    case LeanThunk:           return insert_thunk(o);
    case LeanTask:            return insert_task(o);
    case LeanPromise:         return insert_promise(o);
    case LeanRef:             return insert_ref(o);
    case LeanExternal:        throw exception("external objects cannot be compacted");
    case LeanReserved:        lean_unreachable();
    default:                  return insert_constructor(o);
    }
}

object * object_compactor::copy_object(object * o, size_t sz) {
    if (sz == 0) sz = lean_object_byte_size(o);
    void * mem = alloc(sz);
    memcpy(mem, o, sz);
    object * r = static_cast<object*>(mem);
    lean_set_non_heap_header(r, sz, lean_ptr_tag(o), lean_ptr_other(o));
    lean_assert(!lean_has_rc(r));
    lean_assert(lean_ptr_tag(r) == lean_ptr_tag(o));
    lean_assert(lean_ptr_other(r) == lean_ptr_other(o));
    return r;
}

object_offset object_compactor::insert_sarray(object * o) {
    size_t sz        = lean_sarray_size(o);
    unsigned elem_sz = lean_sarray_elem_size(o);
    if (is_cross_compile()) {
        // header(8B) + m_size(tgt) + m_capacity(tgt) + elem_sz*sz bytes (width-independent data)
        size_t data_sz = elem_sz * sz;
        size_t obj_sz = tgt_align(sizeof(lean_object) + 2 * tgt_ptr_sz() + data_sz);
        char * mem = (char*)alloc(obj_sz);
        emit_target_header(mem, o, obj_sz, LeanScalarArray, elem_sz);
        char * p = mem + sizeof(lean_object);
        write_tgt_size(p, sz, m_target_ptr_width);    p += tgt_ptr_sz();
        write_tgt_size(p, sz, m_target_ptr_width);    p += tgt_ptr_sz();
        memcpy(p, lean_to_sarray(o)->m_data, data_sz);
        object * new_o = reinterpret_cast<object*>(mem);
        return save_max_sharing(o, new_o, obj_sz);
    }
    size_t obj_sz = lean_usize_add_checked(sizeof(lean_sarray_object), lean_usize_mul_checked(elem_sz, sz));
    lean_sarray_object * new_o = (lean_sarray_object*)alloc(obj_sz);
    lean_set_non_heap_header_for_big((lean_object*)new_o, LeanScalarArray, elem_sz);
    new_o->m_size     = sz;
    new_o->m_capacity = sz;
    memcpy(new_o->m_data, lean_to_sarray(o)->m_data, elem_sz*sz);
    return save_max_sharing(o, (lean_object*)new_o, obj_sz);
}

object_offset object_compactor::insert_string(object * o) {
    size_t sz        = lean_string_size(o);
    size_t len       = lean_string_len(o);
    if (is_cross_compile()) {
        // header(8B) + m_size(tgt) + m_capacity(tgt) + m_length(tgt) + sz bytes (width-independent data)
        size_t obj_sz = tgt_align(sizeof(lean_object) + 3 * tgt_ptr_sz() + sz);
        char * mem = (char*)alloc(obj_sz);
        emit_target_header(mem, o, obj_sz, LeanString, 0);
        char * p = mem + sizeof(lean_object);
        write_tgt_size(p, sz, m_target_ptr_width);  p += tgt_ptr_sz();
        write_tgt_size(p, sz, m_target_ptr_width);  p += tgt_ptr_sz();
        write_tgt_size(p, len, m_target_ptr_width); p += tgt_ptr_sz();
        memcpy(p, lean_to_string(o)->m_data, sz);
        object * new_o = reinterpret_cast<object*>(mem);
        return save_max_sharing(o, new_o, obj_sz);
    }
    size_t obj_sz = lean_usize_add_checked(sizeof(lean_string_object), sz);
    lean_string_object * new_o = (lean_string_object*)alloc(obj_sz);
    lean_set_non_heap_header_for_big((lean_object*)new_o, LeanString, 0);
    new_o->m_size     = sz;
    new_o->m_capacity = sz;
    new_o->m_length   = len;
    memcpy(new_o->m_data, lean_to_string(o)->m_data, sz);
    return save_max_sharing(o, (lean_object*)new_o, obj_sz);
}

// #define ShowCtors

// `m_tmp` is used as a recursion stack: each `insert_*` with children reserves a
// window `[base, base + n)` for its child offsets and pops it before returning, so
// nested calls never share slots and arbitrarily large arrays stay off the C stack.
object_offset object_compactor::insert_constructor(object * o) {
    unsigned num_objs = lean_ctor_num_objs(o);
    size_t base = m_tmp.size();
    m_tmp.resize(base + num_objs);
    for (unsigned i = 0; i < num_objs; i++) {
        object_offset c = to_offset(cnstr_get(o, i));
        m_tmp[base + i] = c;
    }
    if (is_cross_compile()) {
        // Target-layout: header (8B) + num_objs * tgt_ptr_sz + scalar data
        // Constructors store object pointer slots followed by inline scalar data
        // (UInt8/16/32/64, Bool, Float — all width-independent). The scalar region
        // starts after the pointer slots and extends to the end of the host object.
        size_t host_byte_sz = lean_object_byte_size(o);
        size_t scalar_sz = host_byte_sz - sizeof(lean_object) - num_objs * sizeof(void*);
        size_t data_sz = num_objs * tgt_ptr_sz() + scalar_sz;
        size_t obj_sz = tgt_align(sizeof(lean_object) + data_sz);
        char * mem = (char*)alloc(obj_sz);
        emit_target_header(mem, o, obj_sz, lean_ptr_tag(o), lean_ptr_other(o));
        char * slots = mem + sizeof(lean_object);
        for (unsigned i = 0; i < num_objs; i++) {
            write_tgt_ptr(slots + i * tgt_ptr_sz(),
                          reinterpret_cast<size_t>(m_tmp[base + i]), m_target_ptr_width);
        }
        if (scalar_sz > 0) {
            memcpy(slots + num_objs * tgt_ptr_sz(), lean_ctor_scalar_cptr(o), scalar_sz);
        }
        m_tmp.resize(base);
        object * new_o = reinterpret_cast<object*>(mem);
        return save_max_sharing(o, new_o, obj_sz);
    }
    object * new_o = copy_object(o);
    for (unsigned i = 0; i < num_objs; i++)
        lean_ctor_set(new_o, i, m_tmp[base + i]);
    m_tmp.resize(base);
    return save_max_sharing(o, new_o, lean_object_byte_size(o));
}

object_offset object_compactor::insert_array(object * o) {
    size_t sz = array_size(o);
    size_t base = m_tmp.size();
    m_tmp.resize(base + sz);
    for (size_t i = 0; i < sz; i++) {
        object_offset c = to_offset(array_get(o, i));
        m_tmp[base + i] = c;
    }
    if (is_cross_compile()) {
        // Target-layout: header(8B) + m_size(tgt) + m_capacity(tgt) + sz * tgt_ptr_sz
        size_t data_sz = sz * tgt_ptr_sz();
        size_t obj_sz = tgt_align(sizeof(lean_object) + 2 * tgt_ptr_sz() + data_sz);
        char * mem = (char*)alloc(obj_sz);
        emit_target_header(mem, o, obj_sz, LeanArray, 0);
        char * p = mem + sizeof(lean_object);
        write_tgt_size(p, sz, m_target_ptr_width);             p += tgt_ptr_sz();
        write_tgt_size(p, sz, m_target_ptr_width);             p += tgt_ptr_sz();
        for (size_t i = 0; i < sz; i++) {
            write_tgt_ptr(p, reinterpret_cast<size_t>(m_tmp[base + i]), m_target_ptr_width);
            p += tgt_ptr_sz();
        }
        m_tmp.resize(base);
        object * new_o = reinterpret_cast<object*>(mem);
        return save_max_sharing(o, new_o, obj_sz);
    }
    size_t obj_sz = lean_usize_add_checked(sizeof(lean_array_object), lean_usize_mul_checked(sizeof(void*), sz));
    lean_array_object * new_o = (lean_array_object*)alloc(obj_sz);
    lean_set_non_heap_header_for_big((lean_object*)new_o, LeanArray, 0);
    new_o->m_size     = sz;
    new_o->m_capacity = sz;
    for (size_t i = 0; i < sz; i++)
        lean_array_set_core((lean_object*)new_o, i, m_tmp[base + i]);
    m_tmp.resize(base);
    return save_max_sharing(o, (lean_object*)new_o, obj_sz);
}

object_offset object_compactor::insert_thunk(object * o) {
    object_offset c = to_offset(lean_thunk_get(o));
    if (is_cross_compile()) {
        // Compacted thunks must be evaluated (m_closure == null); the reader only
        // fixes m_value, not m_closure.
        lean_assert(lean_to_thunk(o)->m_closure == nullptr);
        // header(8B) + m_value(tgt) + m_closure(tgt)
        size_t obj_sz = tgt_align(sizeof(lean_object) + 2 * tgt_ptr_sz());
        char * mem = (char*)alloc(obj_sz);
        emit_target_header(mem, o, obj_sz, lean_ptr_tag(o), lean_ptr_other(o));
        char * p = mem + sizeof(lean_object);
        write_tgt_ptr(p, reinterpret_cast<size_t>(c), m_target_ptr_width);
        p += tgt_ptr_sz();
        write_tgt_ptr(p, 0, m_target_ptr_width); // m_closure (null)
        object * new_o = reinterpret_cast<object*>(mem);
        return save_max_sharing(o, new_o, obj_sz);
    }
    size_t sz = sizeof(lean_thunk_object);
    object * r = copy_object(o, sz);
    lean_to_thunk(r)->m_value = c;
    return save_max_sharing(o, r, sz);
}

object_offset object_compactor::insert_ref(object * o) {
    object_offset c = to_offset(lean_to_ref(o)->m_value);
    if (is_cross_compile()) {
        // header(8B) + m_value(tgt)
        size_t obj_sz = tgt_align(sizeof(lean_object) + tgt_ptr_sz());
        char * mem = (char*)alloc(obj_sz);
        emit_target_header(mem, o, obj_sz, lean_ptr_tag(o), lean_ptr_other(o));
        write_tgt_ptr(mem + sizeof(lean_object), reinterpret_cast<size_t>(c), m_target_ptr_width);
        object * new_o = reinterpret_cast<object*>(mem);
        return save(o, new_o); // must NOT be max-shared
    }
    size_t sz = sizeof(lean_ref_object);
    object * r = copy_object(o, sz);
    lean_to_ref(r)->m_value = c;
    // must NOT be max-shared
    return save(o, r);
}

object_offset object_compactor::insert_task(object * o) {
    object_offset c = to_offset(lean_task_get(o));
    if (is_cross_compile()) {
        // header(8B) + m_value(tgt) + m_imp(tgt, null in compacted)
        size_t obj_sz = tgt_align(sizeof(lean_object) + 2 * tgt_ptr_sz());
        char * mem = (char*)alloc(obj_sz);
        emit_target_header(mem, o, obj_sz, lean_ptr_tag(o), lean_ptr_other(o));
        char * p = mem + sizeof(lean_object);
        write_tgt_ptr(p, reinterpret_cast<size_t>(c), m_target_ptr_width);
        p += tgt_ptr_sz();
        write_tgt_ptr(p, 0, m_target_ptr_width); // m_imp (null)
        object * new_o = reinterpret_cast<object*>(mem);
        return save_max_sharing(o, new_o, obj_sz);
    }
    size_t sz = sizeof(lean_task_object);
    object * r = copy_object(o, sz);
    lean_assert(lean_to_task(r)->m_imp == nullptr);
    lean_to_task(r)->m_value = c;
    return save_max_sharing(o, r, sz);
}

object_offset object_compactor::insert_closure(object * o) {
    if (!m_allow_closures) {
        throw exception("Closures cannot be compacted (unless explicitly calling "
                        "`CompactedRegion.save (allowClosures := true)`). One possible cause of this error is "
                        "trying to store a function in a persistent environment extension.");
    }
    unsigned n = lean_closure_num_fixed(o);
    size_t base = m_tmp.size();
    m_tmp.resize(base + n);
    for (unsigned i = 0; i < n; i++) {
        object_offset c = to_offset(lean_closure_arg_cptr(o)[i]);
        m_tmp[base + i] = c;
    }
    if (is_cross_compile()) {
        // Cross-compile: resolve m_fun to a symbol name. Try the declared-symbol
        // registry first (covers all @[extern]/@[export] functions the interpreter
        // has resolved), then fall back to dladdr (covers LEAN_EXPORT runtime fns).
        void * fn = lean_closure_fun(o);
        std::string sym_name = lean::ir::lookup_symbol_name(fn);
        if (sym_name.empty()) {
#if defined(__linux__) || defined(__APPLE__)
            Dl_info info;
            if (dladdr(fn, &info) && info.dli_sname) {
                sym_name = info.dli_sname;
            }
#endif
        }
        if (sym_name.empty()) {
            throw exception(std::string("Cross-compile closure: could not resolve symbol name for "
                                        "function pointer. The function must be an exported symbol "
                                        "(LEAN_EXPORT or default visibility) or an @[extern] "
                                        "function resolved by the interpreter. Non-exported "
                                        "@[extern] functions that were never called are not in "
                                        "the registry; call the function once before saving."));
        }
        // Target layout: header(8B) + m_fun(tgt ptr) + m_arity(u16) + m_num_fixed(u16) + n * tgt_ptr
        size_t obj_sz = tgt_align(sizeof(lean_object) + tgt_ptr_sz() + 2 * sizeof(uint16_t) + n * tgt_ptr_sz());
        char * mem = (char*)alloc(obj_sz);
        emit_target_header(mem, o, obj_sz, LeanClosure, 0);
        char * p = mem + sizeof(lean_object);
        write_tgt_ptr(p, 0, m_target_ptr_width);                  p += tgt_ptr_sz();  // m_fun = 0 (placeholder)
        uint16_t arity = lean_closure_arity(o);
        uint16_t num_fixed = lean_closure_num_fixed(o);
        memcpy(p, &arity, sizeof(arity));                          p += sizeof(uint16_t);
        memcpy(p, &num_fixed, sizeof(num_fixed));                  p += sizeof(uint16_t);
        for (unsigned i = 0; i < n; i++) {
            write_tgt_ptr(p, reinterpret_cast<size_t>(m_tmp[base + i]), m_target_ptr_width);
            p += tgt_ptr_sz();
        }
        m_tmp.resize(base);
        // Record the buffer-relative offset of the m_fun field and its symbol name.
        size_t fn_field_off = (mem + sizeof(lean_object))
                            - reinterpret_cast<char *>(m_begin);
        m_closure_offsets.push_back(fn_field_off);
        m_closure_symbols.push_back(sym_name);
        object * new_o = reinterpret_cast<object*>(mem);
        return save_max_sharing(o, new_o, obj_sz);
    }
    object * r = copy_object(o);
    for (unsigned i = 0; i < n; i++)
        lean_closure_arg_cptr(r)[i] = m_tmp[base + i];
    m_tmp.resize(base);
    // Record the buffer-relative offset of `r`'s `m_fun` field so the reader can patch
    // closure fn pointers on load without scanning the compacted region.
    size_t fn_field_off = reinterpret_cast<char *>(&lean_to_closure(r)->m_fun)
                          - reinterpret_cast<char *>(m_begin);
    m_closure_offsets.push_back(fn_field_off);
    return save(o, r);
}

object_offset object_compactor::insert_promise(object * o) {
    object_offset c = to_offset((object *)lean_to_promise(o)->m_result);
    if (is_cross_compile()) {
        // header(8B) + m_result(tgt)
        size_t obj_sz = tgt_align(sizeof(lean_object) + tgt_ptr_sz());
        char * mem = (char*)alloc(obj_sz);
        emit_target_header(mem, o, obj_sz, lean_ptr_tag(o), lean_ptr_other(o));
        write_tgt_ptr(mem + sizeof(lean_object), reinterpret_cast<size_t>(c), m_target_ptr_width);
        object * new_o = reinterpret_cast<object*>(mem);
        return save_max_sharing(o, new_o, obj_sz);
    }
    size_t sz = sizeof(lean_promise_object);
    object * r = copy_object(o, sz);
    lean_to_promise(r)->m_result = (lean_task_object *)c;
    return save_max_sharing(o, r, sz);
}

object_offset object_compactor::insert_mpz(object * o) {
#ifdef LEAN_USE_GMP
    size_t nlimbs = mpz_size(to_mpz(o)->m_value.m_val);
    size_t data_sz = lean_usize_mul_checked(sizeof(mp_limb_t), nlimbs);
    size_t sz = lean_usize_add_checked(sizeof(mpz_object), data_sz);
    mpz_object * new_o = (mpz_object *)alloc(sz);
    memcpy((void*)new_o, to_mpz(o), sizeof(mpz_object));
    lean_set_non_heap_header((lean_object*)new_o, sz, LeanMPZ, 0);
    __mpz_struct & m = new_o->m_value.m_val[0];
    // we assume the limb array is the only indirection in an `__mpz_struct` and everything else can be bitcopied
    void * data = reinterpret_cast<char*>(new_o) + sizeof(mpz_object);
    memcpy(data, m._mp_d, data_sz);
    m._mp_d = reinterpret_cast<mp_limb_t *>(reinterpret_cast<char *>(data) - reinterpret_cast<char *>(m_begin) + reinterpret_cast<ptrdiff_t>(m_base_addr));
    m._mp_alloc = nlimbs;
    return save(o, (lean_object*)new_o);
#else
    if (is_cross_compile()) {
        // Target-layout mpz: header(8B) + mpz_struct(3*tgt_ptr_sz: m_sign + pad + m_size + m_digits) + data[]
        size_t mpz_sz = 3 * tgt_ptr_sz(); // m_sign(1B+pad) + m_size(tgt) + m_digits(tgt)
        size_t data_sz = lean_usize_mul_checked(sizeof(mpn_digit), to_mpz(o)->m_value.m_size);
        size_t obj_sz = tgt_align(sizeof(lean_object) + mpz_sz + data_sz);
        char * mem = (char*)alloc(obj_sz);
        emit_target_header(mem, o, obj_sz, LeanMPZ, 0);
        char * p = mem + sizeof(lean_object);
        // m_sign at offset 0 (1 byte), padding to tgt_ptr_sz
        *p = static_cast<char>(to_mpz(o)->m_value.m_sign ? 1 : 0);
        p += tgt_ptr_sz(); // skip m_sign + padding
        // m_size at tgt_ptr_sz
        write_tgt_size(p, to_mpz(o)->m_value.m_size, m_target_ptr_width);
        p += tgt_ptr_sz();
        // m_digits at 2*tgt_ptr_sz — written as base-relative offset (filled after data is placed)
        char * digits_field = p;
        p += tgt_ptr_sz();
        // data follows the mpz struct
        memcpy(p, to_mpz(o)->m_value.m_digits, data_sz);
        // m_digits = data_ptr - m_begin + m_base_addr (target-width pointer)
        size_t digits_off = reinterpret_cast<size_t>(p) - reinterpret_cast<size_t>(m_begin)
                          + reinterpret_cast<size_t>(m_base_addr);
        lean_assert(digits_off < (static_cast<size_t>(1) << (m_target_ptr_width * 8)));
        write_tgt_ptr(digits_field, digits_off, m_target_ptr_width);
        object * new_o = reinterpret_cast<object*>(mem);
        return save(o, new_o);
    }
    size_t data_sz = lean_usize_mul_checked(sizeof(mpn_digit), to_mpz(o)->m_value.m_size);
    size_t sz      = lean_usize_add_checked(sizeof(mpz_object), data_sz);
    mpz_object * new_o = (mpz_object *)alloc(sz);
    // Manually copy the `mpz_object` to ensure `mpz` struct padding is left as
    // zero as prepared by `object_compactor::alloc`. `memcpy` would copy the
    // padding and lead to non-deterministic outputs.
    new_o->m_header = to_mpz(o)->m_header;
    new_o->m_value.m_sign = to_mpz(o)->m_value.m_sign;
    new_o->m_value.m_size = to_mpz(o)->m_value.m_size;
    lean_set_non_heap_header((lean_object*)new_o, sz, LeanMPZ, 0);
    void * data = reinterpret_cast<char*>(new_o) + sizeof(mpz_object);
    memcpy(data, to_mpz(o)->m_value.m_digits, data_sz);
    new_o->m_value.m_digits = reinterpret_cast<mpn_digit *>(reinterpret_cast<char *>(data) - reinterpret_cast<char *>(m_begin) + reinterpret_cast<ptrdiff_t>(m_base_addr));
    return save(o, (lean_object*)new_o);
#endif
}

#ifdef LEAN_TAG_COUNTERS

static size_t g_tag_counters[256];

struct tag_counter_manager {
    static void display_kind(char const * msg, unsigned k) {
        if (g_tag_counters[k] != 0)
            std::cout << msg << " " << g_tag_counters[k] << "\n";
    }

    tag_counter_manager() {
        for (unsigned i = 0; i < 256; i++) g_tag_counters[i] = 0;
    }

    ~tag_counter_manager() {
        display_kind("#closure:  ", LeanClosure);
        display_kind("#array:    ", LeanArray);
        display_kind("#sarray:   ", LeanStructArray);
        display_kind("#scarray:  ", LeanScalarArray);
        display_kind("#string:   ", LeanString);
        display_kind("#mpz:      ", LeanMPZ);
        display_kind("#thunk:    ", LeanThunk);
        display_kind("#task:     ", LeanTask);
        display_kind("#promise:  ", LeanPromise);
        display_kind("#ref:      ", LeanRef);
        display_kind("#external: ", LeanExternal);

        size_t num_ctors = 0;
        for (unsigned i = 0; i <= LeanMaxCtorTag; i++)
            num_ctors += g_tag_counters[i];
        std::cout << "#ctors:     " << num_ctors << "\n";
    }
};

tag_counter_manager g_tag_counter_manager;

#endif

void object_compactor::operator()(object * o) {
    lean_assert(m_tmp.empty());
    // Allocate the root-address slot first (see below). We store an offset rather
    // than a pointer because `m_begin` may be reallocated while compacting `o`.
    size_t root_slot_sz = is_cross_compile() ? tgt_ptr_sz() : sizeof(object_offset);
    size_t root_offset =
      static_cast<char *>(alloc(root_slot_sz)) - static_cast<char *>(m_begin);
    object_offset off = to_offset(o);
    char * root = static_cast<char *>(m_begin) + root_offset;
    if (is_cross_compile()) {
        write_tgt_ptr(root, reinterpret_cast<size_t>(off), m_target_ptr_width);
    } else {
        *reinterpret_cast<object_offset *>(root) = off;
    }
}

region_reader::region_reader(size_t sz, void * data, void * base_addr,
                             std::vector<region_view> dep_regions,
                             std::vector<std::pair<size_t, ptrdiff_t>> lib_relocs,
                             std::vector<size_t> closure_offsets,
                             std::vector<void *> closure_fn_ptrs):
    m_size(sz),
    m_base_addr(base_addr),
    m_begin(data),
    m_next(data),
    m_end(static_cast<char*>(data)+sz),
    m_dep_regions(std::move(dep_regions)),
    m_lib_relocs(std::move(lib_relocs)),
    m_closure_offsets(std::move(closure_offsets)),
    m_closure_fn_ptrs(std::move(closure_fn_ptrs)) {
    // Sort + overlap validation are deferred to `sort_and_validate_dep_regions()`: needed only
    // before the fixup walk; the fast path in `read()` skips both. Doing them eagerly here was
    // an O(N log N) per construction over a `dep_regions` array that grows linearly across
    // successive `CompactedRegion.read` calls (e.g. `--incr-load` chaining 9000+ dep oleans).
}

void region_reader::sort_and_validate_dep_regions() {
    // Sort dep regions by `base_addr` for binary search in `fix_object_ptr`
    std::sort(m_dep_regions.begin(), m_dep_regions.end(),
              [](region_view const & a, region_view const & b) { return a.base_addr < b.base_addr; });
    // Reject overlapping saved address ranges: `fix_object_ptr` resolves cross-region pointers
    // by binary-searching `base_addr`s, and would silently translate via the wrong region if
    // two deps overlap (or if a dep overlaps our own range). This should not happen with regular
    // .olean use as we use only use `read`'s `prev` instead of `dep_regions` there.
    for (size_t i = 1; i < m_dep_regions.size(); i++) {
        region_view const & prev = m_dep_regions[i - 1];
        region_view const & curr = m_dep_regions[i];
        if (reinterpret_cast<size_t>(prev.base_addr) + prev.size
                > reinterpret_cast<size_t>(curr.base_addr)) {
            throw exception("region_reader: dep regions have overlapping `base_addr` ranges");
        }
    }
    size_t self_base = reinterpret_cast<size_t>(m_base_addr);
    size_t self_end = self_base + m_size;
    for (region_view const & dep : m_dep_regions) {
        size_t dep_base = reinterpret_cast<size_t>(dep.base_addr);
        size_t dep_end = dep_base + dep.size;
        if (self_base < dep_end && dep_base < self_end) {
            throw exception("region_reader: own region overlaps a dep region's `base_addr` range");
        }
    }
}


inline object * region_reader::fix_object_ptr(object * o) {
    if (lean_is_scalar(o)) return o;
    size_t addr = reinterpret_cast<size_t>(o);
    size_t self_base = reinterpret_cast<size_t>(m_base_addr);
    // Check own region first (most common case)
    if (addr >= self_base && addr < self_base + m_size) {
        return reinterpret_cast<object*>(static_cast<char*>(m_begin) + (addr - self_base));
    }
    // Binary search dep regions (sorted by `base_addr`)
    char * addr_ptr = reinterpret_cast<char *>(addr);
    // Find the first region whose base_addr > addr, then step back
    std::vector<region_view>::iterator upper = std::upper_bound(
        m_dep_regions.begin(), m_dep_regions.end(), addr_ptr,
        [](char * a, region_view const & r) { return a < static_cast<char *>(r.base_addr); });
    if (upper != m_dep_regions.begin()) {
        region_view const & dep = *(upper - 1);
        size_t dep_base = reinterpret_cast<size_t>(dep.base_addr);
        if (addr < dep_base + dep.size) {
            return reinterpret_cast<object*>(static_cast<char*>(dep.begin) + (addr - dep_base));
        }
    }
    lean_unreachable();
}

inline void region_reader::move(size_t d) {
    lean_assert(m_next < m_end);
    size_t rem = d % sizeof(void*);
    if (rem != 0)
        d = d + sizeof(void*) - rem;
    m_next = static_cast<char*>(m_next) + d;
}

inline void region_reader::move(object * o) {
    return move(lean_object_byte_size(o));
}

inline void region_reader::fix_constructor(object * o) {
    lean_assert(!lean_has_rc(o));
    object ** it  = lean_ctor_obj_cptr(o);
    object ** end = it + lean_ctor_num_objs(o);
    for (; it != end; it++) {
        *it = fix_object_ptr(*it);
    }
    lean_assert(lean_object_byte_size(o) < 4192);
    move(o);
}

inline void region_reader::fix_array(object * o) {
    object ** it  = lean_array_cptr(o);
    object ** end = it + lean_array_size(o);
    for (; it != end; it++) {
        *it = fix_object_ptr(*it);
    }
    move(o);
}

inline void region_reader::fix_thunk(object * o) {
    lean_to_thunk(o)->m_value = fix_object_ptr(lean_to_thunk(o)->m_value);
    move(sizeof(lean_thunk_object));
}

inline void region_reader::fix_ref(object * o) {
    lean_to_ref(o)->m_value = fix_object_ptr(lean_to_ref(o)->m_value);
    move(sizeof(lean_ref_object));
}

inline void region_reader::fix_task(object * o) {
    lean_to_task(o)->m_value = fix_object_ptr(lean_to_task(o)->m_value);
    move(sizeof(lean_task_object));
}

inline void region_reader::fix_promise(object * o) {
    lean_to_promise(o)->m_result = (lean_task_object *)fix_object_ptr((lean_object *)lean_to_promise(o)->m_result);
    move(sizeof(lean_promise_object));
}

void region_reader::fix_mpz(object * o) {
#ifdef LEAN_USE_GMP
    __mpz_struct & m = to_mpz(o)->m_value.m_val[0];
    m._mp_d = reinterpret_cast<mp_limb_t *>(static_cast<char *>(m_begin) + reinterpret_cast<size_t>(m._mp_d) - reinterpret_cast<size_t>(m_base_addr));
    move(sizeof(mpz_object) + sizeof(mp_limb_t) * mpz_size(to_mpz(o)->m_value.m_val));
#else
    to_mpz(o)->m_value.m_digits = reinterpret_cast<mpn_digit*>(reinterpret_cast<char*>(o) + sizeof(mpz_object));
    move(sizeof(mpz_object) + sizeof(mpn_digit) * to_mpz(o)->m_value.m_size);
#endif
}

void region_reader::fix_closure(object * o) {
    // Fix captured object pointers. The closure's `m_fun` is relocated separately
    // via `m_closure_offsets` before the walk runs.
    object ** it = lean_closure_arg_cptr(o);
    object ** end = it + lean_closure_num_fixed(o);
    for (; it != end; it++)
        *it = fix_object_ptr(*it);
    move(o);
}

object * region_reader::read() {
    if (m_next == m_end)
        return nullptr; /* all objects have been read */

    // Patch closure fn pointers. Two modes:
    // 1. Symbol-resolved (cross-compile): m_closure_fn_ptrs has one resolved pointer per
    //    closure; patch m_fun directly with the native pointer.
    // 2. Lib-relocation (native): adjust m_fun by the delta between saved and current
    //    library base addresses.
    if (!m_closure_offsets.empty()) {
        char * begin = static_cast<char *>(m_begin);
        if (!m_closure_fn_ptrs.empty()) {
            // Symbol-resolved: patch each closure's m_fun with the resolved pointer.
            for (size_t i = 0; i < m_closure_offsets.size(); i++) {
                void ** fn_field = reinterpret_cast<void **>(begin + m_closure_offsets[i]);
                *fn_field = m_closure_fn_ptrs[i];
            }
        } else {
            // Lib-relocation: apply base-address deltas.
            bool needs_fn_reloc = false;
            for (std::pair<size_t, ptrdiff_t> const & reloc : m_lib_relocs) {
                if (reloc.second != 0) { needs_fn_reloc = true; break; }
            }
            if (needs_fn_reloc) {
                for (size_t off : m_closure_offsets) {
                    void ** fn_field = reinterpret_cast<void **>(begin + off);
                    size_t fn = reinterpret_cast<size_t>(*fn_field);
                    std::vector<std::pair<size_t, ptrdiff_t>>::const_iterator upper =
                        std::upper_bound(m_lib_relocs.begin(), m_lib_relocs.end(), fn,
                            [](size_t addr, std::pair<size_t, ptrdiff_t> const & e) { return addr < e.first; });
                    if (upper != m_lib_relocs.begin()) {
                        ptrdiff_t delta = (upper - 1)->second;
                        if (delta != 0)
                            *fn_field = reinterpret_cast<void *>(static_cast<ptrdiff_t>(fn) + delta);
                    }
                }
            }
        }
    }

    if (m_begin == m_base_addr) {
        bool needs_dep_reloc = false;
        for (region_view const & dep : m_dep_regions) {
            if (dep.begin != dep.base_addr) { needs_dep_reloc = true; break; }
        }
        if (!needs_dep_reloc) {
            // Fast path: own pointers and all dep pointers are already correct, so the saved root
            // pointer needs no fixup and the structural walk can be skipped entirely. This also
            // lets us avoid sorting and validating `m_dep_regions` (the dominant cost when chaining
            // many dep oleans into a single `region_reader`).
            object * root = *static_cast<object_offset *>(m_next);
            m_end = m_next;
            return root;
        }
    }

    // Slow path: dep-region fixup needed. Sort and validate dep regions now.
    sort_and_validate_dep_regions();
    object * root = fix_object_ptr(*static_cast<object_offset *>(m_next));
    move(sizeof(object_offset));

    while (m_next < m_end) {
        object * curr = reinterpret_cast<object*>(m_next);
        uint8 tag = lean_ptr_tag(curr);
        if (tag <= LeanMaxCtorTag) {
            fix_constructor(curr);
        } else {
            switch (tag) {
            case LeanClosure:         fix_closure(curr); break;
            case LeanArray:           fix_array(curr); break;
            case LeanScalarArray:     move(lean_sarray_byte_size(curr)); break;
            case LeanString:          move(lean_string_byte_size(curr)); break;
            case LeanMPZ:             fix_mpz(curr); break;
            case LeanThunk:           fix_thunk(curr); break;
            case LeanRef:             fix_ref(curr); break;
            case LeanTask:            fix_task(curr); break;
            case LeanPromise:         fix_promise(curr); break;
            case LeanExternal:        lean_unreachable();
            default:                  lean_unreachable();
            }
        }
    }
    return root;
}

#ifdef LEAN_DEBUG
// Self-check: compact a small object graph at target_ptr_width=4 on a 64-bit host
// and verify the root offset and first object header are at the expected target-width
// offsets. This is the smallest check that fails if the target-layout codec is broken.
void cross_compile_self_check() {
    if (sizeof(void*) != 8) return; // only meaningful on 64-bit host
    object * s = lean_mk_string("test");
    object_compactor compactor(nullptr, {}, false, 4);
    compactor(s);
    // Root offset should be 4 bytes (target ptr width)
    char const * data = static_cast<char const *>(compactor.data());
    uint32_t root_off;
    memcpy(&root_off, data, 4);
    lean_assert(root_off == 4); // root offset = tgt_ptr_sz (slot) + base_addr(0)
    // The first object after the root slot should have a valid lean_object header
    lean_object const * hdr = reinterpret_cast<lean_object const *>(data + root_off);
    lean_assert(lean_ptr_tag(const_cast<lean_object*>(hdr)) == LeanString);
    lean_dec(s);
}
#endif

}
