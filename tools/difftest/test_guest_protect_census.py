"""Regressions for the guest protection / release census (guest_protect_census.py). The base is the census' own
funnels and gate owners read from this tree; each test adds or mutates one input and requires the named problem.
Astra's round A negative controls (a pointer call, VirtualFree with any flags, VirtualAlloc with any protection, a
watch-arm alias, a conditional declaration) all fail. The full census over the exe's build inputs runs on the worker
in native_transport_test_run.py (it needs a built tree)."""
import tempfile
import unittest
from pathlib import Path

import guest_protect_census as gpc

BASE = ('kernel/image_write_watchdog.cpp', 'kernel/audio_cmdq_obs.cpp', 'kernel/guest_context.cpp',
        'kernel/guest_protect.hpp', 'kernel/native_hardware_memory.cpp',
        'kernel/cs_timeout_underflow_fix.cpp', 'kernel/gpu_ring_alloc_fix.cpp')


def base():
    return {p: (gpc.ROOT / p).read_text(encoding='utf-8', errors='replace').replace('\r\n', '\n') for p in BASE}


class CensusTests(unittest.TestCase):
    def problems(self, path=None, text=None, files=None):
        files = dict(files or base())
        if path:
            files[path] = text
        return gpc.census(files)[0]

    def assertNamed(self, problems, *needles):
        self.assertTrue(problems, 'expected a problem')
        for needle in needles:
            self.assertTrue(any(needle in p for p in problems), '%r not in %r' % (needle, problems))

    def test_base_passes(self):
        self.assertEqual(self.problems(), [])

    def test_log_tls_retention_is_exact(self):
        for symbol in ('_tls_used', 'sr_log_tls_callback'):
            self.assertEqual(gpc.linker_flag('/INCLUDE:' + symbol, 'app/log.cpp'), ({}, []))
            self.assertTrue(gpc.linker_flag('/INCLUDE:' + symbol, 'app/other.cpp')[1])
        self.assertTrue(gpc.linker_flag('/INCLUDE:unexpected_callback', 'app/log.cpp')[1])

    def test_comments_do_not_count(self):
        text = '// VirtualProtect(p, n, PAGE_NOACCESS, &o);\n/* VirtualFree(p, 0, MEM_RELEASE); */\nint x;\n'
        self.assertEqual(self.problems('kernel/new_probe.cpp', text), [])

    def test_call_through_a_function_pointer(self):
        text = ('static void f(void* p) {\n  auto vp = &VirtualProtect;\n  DWORD o;\n'
                '  vp(p, 4096, PAGE_NOACCESS, &o);\n}\n')
        self.assertNamed(self.problems('gfx/new_probe.cpp', text),
                         'gfx/new_probe.cpp: unclassified reference in f to VirtualProtect')

    def test_virtualfree_with_any_flags(self):
        for flags in ('0x4000', 'MEM_RELEASE'):
            text = 'void g(void* p, size_t n) { VirtualFree(p, %s == 0x4000 ? n : 0, %s); }\n' % (flags, flags)
            self.assertNamed(self.problems('kernel/new_free.cpp', text),
                             'kernel/new_free.cpp: unclassified reference in g to VirtualFree')

    def test_virtualalloc_with_any_protection(self):
        text = 'void h(void* p, size_t n) { VirtualAlloc(p, n, MEM_COMMIT, PAGE_NOACCESS); }\n'
        self.assertNamed(self.problems('app/new_alloc.cpp', text),
                         'app/new_alloc.cpp: unclassified reference in h to VirtualAlloc')

    def test_lookup_by_name_counts(self):
        text = ('void k() {\n  auto p = GetProcAddress(GetModuleHandleA("kernelbase.dll"), "VirtualProtect");\n}\n'
                'void m() { NtProtectVirtualMemory(h, &b, &n, 2, &o); UnmapViewOfFile(v); }\n')
        self.assertNamed(self.problems('kernel/new_lookup.cpp', text),
                         'unclassified reference in k to VirtualProtect', 'in m to NtProtectVirtualMemory',
                         'in m to UnmapViewOfFile')

    def test_watch_arm_through_an_alias(self):
        text = ('namespace sr { namespace kernel { bool armRuntimeWriteWatch(uint32_t, uint32_t); } }\n'
                'void w() { auto arm = sr::kernel::armRuntimeWriteWatch; arm(0x82000000u, 4); }\n')
        self.assertNamed(self.problems('kernel/new_watch.cpp', text),
                         'kernel/new_watch.cpp: unclassified reference in w to armRuntimeWriteWatch',
                         'kernel/new_watch.cpp: unclassified reference in <file> to armRuntimeWriteWatch')

    def test_conditional_declaration_fails_the_funnel(self):
        files = base()
        path = 'kernel/audio_cmdq_obs.cpp'
        old = '  guest_protect::declare(hostBase, bytes, guest_protect::Scope::All);\n'
        self.assertIn(old, files[path])
        files[path] = files[path].replace(old, '  if (false) guest_protect::declare(hostBase, bytes, '
                                               'guest_protect::Scope::All);\n')
        self.assertNamed(self.problems(files=files), 'protectRange does not make declare its first effective statement')
        path = 'kernel/image_write_watchdog.cpp'
        files = base()
        old = '  sr::kernel::guest_protect::declare(addr, size, wdScope(addr, size));\n'
        self.assertIn(old, files[path])
        files[path] = files[path].replace(old, '  if (prot != PAGE_READWRITE) sr::kernel::guest_protect::declare('
                                               'addr, size, wdScope(addr, size));\n')
        self.assertNamed(self.problems(files=files), 'wdProtect does not make declare its first effective statement')

    def test_release_must_retire_first(self):
        files = base()
        path = 'kernel/guest_context.cpp'
        old = '  const bool drained = guest_protect::retireArena(kArenaRetireMs);\n'
        self.assertIn(old, files[path])
        files[path] = files[path].replace(old, '  const bool drained = true;\n')
        self.assertNamed(self.problems(files=files), 'releaseGuestArena does not make retireArena its first')

    def test_a_second_site_in_a_classified_function_is_named(self):
        files = base()
        path = 'kernel/audio_cmdq_obs.cpp'
        old = '  hostRangeReadableEndRemoval();\n  return ok != 0;'
        self.assertIn(old, files[path])
        files[path] = files[path].replace(old, '  VirtualProtect(hostBase, bytes, PAGE_READONLY, &old);\n' + old)
        self.assertNamed(self.problems(files=files), 'protectRange references VirtualProtect 2 times, classified 1')

    def test_gates_track_their_owners(self):
        files = base()
        path = 'kernel/gpu_ring_alloc_fix.cpp'
        self.assertIn('"SR_ALLOC_WATCH"', files[path])
        files[path] = files[path].replace('"SR_ALLOC_WATCH"', '"SR_ALLOC_WATCH2"')
        self.assertNamed(self.problems(files=files), 'no longer reads its diagnostic gate SR_ALLOC_WATCH')

    def fake_build(self, inputs, deps, link, extra=''):
        """A temporary build tree with build.ninja (target x.exe linking `link`) and ninja faked from the listings
        ('{build}' in them is the tree's path)."""
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        build = Path(tmp.name)
        (build / 'build.ninja').write_text('include rules.ninja\n\nbuild x.exe: CXX_EXECUTABLE_LINKER__x a.obj\n'
                                           '  LINK_LIBRARIES = %s\n\n%s' % (link, extra))
        (build / 'rules.ninja').write_text('rule CXX_EXECUTABLE_LINKER__x\n  command = clang $FLAGS -o $out\n'
                                           'rule CXX_COMPILER__x\n  command = clang $FLAGS -c $in\n')
        saved = gpc.ninja
        listing = {'inputs': inputs, 'deps': deps}
        gpc.ninja = lambda b, *args: listing[args[0]].replace('{build}', build.as_posix())
        self.addCleanup(setattr, gpc, 'ninja', saved)
        return build

    def test_build_inputs_need_a_built_target(self):
        build = self.fake_build(
            'CMakeFiles/x.dir/a.cpp.obj\nCMakeFiles/x.dir/b.cpp.obj\nCMakeFiles/x.dir/c.cpp.obj\n',
            ('CMakeFiles/x.dir/a.cpp.obj: #deps 2, deps mtime 5 (VALID)\n    %s/kernel/guest_protect.hpp\n'
             '    C:/Program Files (x86)/Windows Kits/10/Include/um/memoryapi.h\n\n'
             'CMakeFiles/x.dir/b.cpp.obj: #deps 1, deps mtime 5 (STALE)\n    x.h\n\n') % gpc.ROOT.as_posix(), '')
        files, binaries, problems, platform = gpc.build_inputs(build, 'x.exe')
        self.assertIn('kernel/guest_protect.hpp', files)
        self.assertEqual(platform, 1)
        self.assertTrue(any('b.cpp.obj: deps entry STALE' in p for p in problems))
        self.assertTrue(any('c.cpp.obj: no deps entry' in p for p in problems))

    def test_opaque_libraries_and_unknown_suffixes_fail_closed(self):
        # Astra round B: a protector.lib input and a recorded guest_memory.def dependency gave no problem
        build = self.fake_build('CMakeFiles/x.dir/a.cpp.obj\nprotector.lib\n',
                                'CMakeFiles/x.dir/a.cpp.obj: #deps 1, deps mtime 5 (VALID)\n'
                                '    {build}/guest_memory.def\n\n', 'protector.lib -lfoo.lib')
        (build / 'protector.lib').write_bytes(b'!<arch>\n\0\0VirtualProtect')
        (build / 'guest_memory.def').write_text('void f(void* p) { DWORD o; VirtualProtect(p, 1, 1, &o); }\n')
        files, binaries, problems, _ = gpc.build_inputs(build, 'x.exe')
        found = gpc.census({p: Path(a).read_text() for p, a in files.items()}, binaries)[0]
        self.assertNamed(found, 'protector.lib: unclassified binary input', 'lib:foo.lib: unclassified binary input',
                         'guest_memory.def: unclassified reference in f to VirtualProtect')
        pragma = 'void g() {}\n#pragma comment(lib, "protector2.lib")\n'
        self.assertNamed(self.problems('kernel/new_pragma.cpp', pragma), 'lib:protector2.lib: unclassified binary')

    def test_resolvers_concatenated_names_and_section_apis(self):
        # Astra round B: GetProcAddress(..., "Virtual" "Protect") and NtUnmapViewOfSection passed
        text = ('void r(HMODULE k) {\n  auto p = GetProcAddress(k, "Virtual" "Protect");\n}\n'
                'const char* n = "Virtual"\n  "Free";\n'
                'void s(HANDLE h, void* v) { NtUnmapViewOfSection(h, v); NtMapViewOfSection(h, 0, 0, 0, 0, 0, 0, 0, 0, 0);'
                ' NtCreateSection(0, 0, 0, 0, 0, 0, 0); MapViewOfFileEx(h, 0, 0, 0, 0, 0); UnmapViewOfFileEx(v, 0); }\n'
                'void t(void* m) { LdrGetProcedureAddress(m, 0, 0, 0); }\n')
        self.assertNamed(self.problems('kernel/new_resolve.cpp', text),
                         'in r to GetProcAddress', 'in r to VirtualProtect', 'in <file> to VirtualFree',
                         'in s to NtUnmapViewOfSection', 'in s to NtMapViewOfSection', 'in s to NtCreateSection',
                         'in s to MapViewOfFileEx', 'in s to UnmapViewOfFileEx', 'in t to LdrGetProcedureAddress')
        self.assertEqual(self.problems('kernel/new_plain.cpp', 'const char* a = "Virtual" "Memory";\n'), [])

    def test_inactive_funnel_copies_fail(self):
        # Astra round B: a compliant wdProtect inside #if 0 before the real one, whose declaration became conditional
        files = base()
        path = 'kernel/image_write_watchdog.cpp'
        old = '  sr::kernel::guest_protect::declare(addr, size, wdScope(addr, size));\n'
        real = 'static BOOL wdProtect(LPVOID addr, SIZE_T size, DWORD prot, PDWORD old) {\n'
        self.assertIn(real, files[path])
        copy = ('#if 0\n' + real + old + '  return VirtualProtect(addr, size, prot, old);\n}\n#endif\n')
        files[path] = files[path].replace(real, copy + real, 1).replace(old, '  if (false) ' + old.lstrip(), 1)
        self.assertNamed(self.problems(files=files), 'wdProtect has 2 textual definitions')
        files = base()
        files[path] = files[path].replace(real, '#if SR_SOMETHING\n' + real, 1)
        files[path] = files[path].replace('static uint8_t* allocGuestMemoryUnified', '', 1)
        end = files[path].index(real) + len(real)
        close = files[path].index('\n}\n', end) + 3
        files[path] = files[path][:close] + '#endif\n' + files[path][close:]
        self.assertNamed(self.problems(files=files), 'wdProtect is defined inside a preprocessor conditional')
        files = base()
        files[path] = files[path].replace(real + old, real + '#if 1\n' + old + '#endif\n', 1)
        self.assertNamed(self.problems(files=files), 'wdProtect has a preprocessor directive before its declare')

    def test_macro_bodies_and_exception_specifications(self):
        # Astra round C: a compliant wdProtect in an unused #define, the real one noexcept(true) with a conditional
        # declaration, passed
        path = 'kernel/image_write_watchdog.cpp'
        real = 'static BOOL wdProtect(LPVOID addr, SIZE_T size, DWORD prot, PDWORD old) {\n'
        old = '  sr::kernel::guest_protect::declare(addr, size, wdScope(addr, size));\n'
        files = base()
        files[path] = files[path].replace(real, real.replace(') {', ') noexcept(true) {'), 1)
        self.assertEqual(self.problems(files=files), [])  # noexcept(true) is still the one definition
        files[path] = files[path].replace(old, '  if (false) ' + old.lstrip(), 1)
        self.assertNamed(self.problems(files=files), 'wdProtect does not make declare its first effective statement')
        files = base()
        macro = ('#define UNUSED_FUNNEL static BOOL wdProtect(LPVOID addr, SIZE_T size, DWORD prot, PDWORD old) { \\\n'
                 '  sr::kernel::guest_protect::declare(addr, size, wdScope(addr, size)); \\\n'
                 '  return VirtualProtect(addr, size, prot, old); }\n')
        files[path] = files[path].replace(real, macro + real, 1)
        self.assertNamed(self.problems(files=files), 'wdProtect appears in a preprocessor directive')
        files = base()
        files[path] = files[path].replace(real, 'auto wdAlias = wdProtect;\n' + real, 1)
        self.assertNamed(self.problems(files=files), 'wdProtect is referenced at file scope in a form')

    def test_pragma_forms_resolve_or_fail(self):
        # Astra round C: comment(lib, MACRO), __pragma(comment(lib, "x")) and "kernel32" "evil.lib" were accepted
        cases = {
            '#define PROTECTOR_LIB "protector.lib"\n#pragma comment(lib, PROTECTOR_LIB)\n':
                'not a plain string literal sequence',
            '__pragma(comment(lib, "protector.lib"))\n': 'lib:protector.lib: unclassified binary',
            '#pragma comment(lib, "kernel32" "evil.lib")\n': 'lib:kernel32evil.lib: unclassified binary',
            '#pragma comment(linker, "/DEFAULTLIB:evil")\n': 'lib:evil.lib: unclassified binary',
            '#pragma comment(linker, "/alternatename:a=b")\n': "linker flag '/alternatename:a=b' is not modelled",
            '_Pragma("comment(lib, \\"pragma2.lib\\")")\n': 'lib:pragma2.lib: unclassified binary',
            '#define LINK(x) __pragma(comment(lib, x))\n': 'not a plain string literal sequence',
            '#define P(x) _Pragma(x)\n': 'a _Pragma whose argument is not a plain string literal sequence',
            '#define MY_PRAGMA(x) _Pragma(#x)\n': 'a _Pragma whose argument is not a plain string literal sequence',
            '__pragma(LINK_PRAGMA)\n': 'a __pragma the census does not resolve (LINK_PRAGMA)',
            '#define P _Pragma\nP("comment(lib, \\"alias.lib\\")")\n': 'names _Pragma outside a balanced call',
            '#define Q __pragma\nQ(comment(lib, "alias.lib"))\n': 'names __pragma outside a balanced call',
            '#pragma comment(LIBRARY, "x.lib")\n': 'a comment pragma the census does not resolve',
            '#pra\\\ngma comment(lib, "splice.lib")\n': 'lib:splice.lib: unclassified binary',
            '#pragma /* x */ comment(lib, "a" /* y */ "b.lib")\n': 'lib:ab.lib: unclassified binary',
        }
        for text, needle in cases.items():
            self.assertNamed(self.problems('kernel/new_pragma.cpp', text), needle)
        self.assertEqual(self.problems('kernel/new_pragma.cpp', '#pragma comment(lib, "kernel32.lib")\n'
                                                              '#pragma warning(disable: 4996)\n'
                                                              '__pragma(warning(push))\n'), [])

    def test_pragma_wrappers_are_classified_and_their_uses_checked(self):
        # Astra round C (unresolved forms are rejected): a stringizing _Pragma wrapper is accepted only where
        # PRAGMA_WRAPPERS classifies it, and each use must pass a pragma the census knows (never comment)
        files = base()
        files['deps/FFmpeg/libavutil/macros.h'] = '#ifndef AV_PRAGMA\n#define AV_PRAGMA(s) _Pragma(#s)\n#endif\n'
        files['kernel/uses.cpp'] = ('AV_PRAGMA(GCC diagnostic push)\n#if defined(AV_PRAGMA)\n#undef AV_PRAGMA\n'
                                    '#endif\n')
        self.assertEqual(self.problems(files=files), [])
        cases = {
            'AV_PRAGMA(comment(lib, "evil.lib"))\n':
                'AV_PRAGMA(comment(lib, )) passes a pragma the census does not know',
            '#define MYP AV_PRAGMA\nMYP(comment(lib, "evil.lib"))\n':
                'names the pragma wrapper AV_PRAGMA outside a call',
            '#define LINKP(p) AV_PRAGMA(p)\n': 'AV_PRAGMA(p) passes a pragma the census does not know',
            '#define AV_PRAGMA(s) __pragma(s)\n': 'defines the pragma wrapper name AV_PRAGMA',
        }
        for text, needle in cases.items():
            self.assertNamed(self.problems('kernel/new_wrap.cpp', text, files), needle)
        files['deps/FFmpeg/libavutil/macros.h'] += '#define AV_LINK(s) _Pragma(#s)\n'
        self.assertNamed(self.problems(files=files), 'macros.h: a _Pragma whose argument is not a plain string')

    def test_flags_of_every_contributing_statement(self):
        # Astra round C: an object compiled with -Xclang --dependent-lib=protector, and LINK_LIBRARIES =
        # -Wl,/DELAYLOAD:protector.dll, slipped through
        extra = ('build CMakeFiles/x.dir/a.cpp.obj: CXX_COMPILER__x a.cpp\n'
                 '  FLAGS = -O2 -Xclang --dependent-lib=protector\n\n')
        build = self.fake_build('CMakeFiles/x.dir/a.cpp.obj\n',
                                'CMakeFiles/x.dir/a.cpp.obj: #deps 0, deps mtime 5 (VALID)\n\n',
                                '-Wl,/DELAYLOAD:protector.dll -Xlinker /alternatename:a=b -fsanitize=address', extra)
        files, binaries, problems, _ = gpc.build_inputs(build, 'x.exe')
        problems += gpc.census({}, binaries)[0]
        self.assertNamed(problems, 'lib:protector.lib: unclassified binary input', 'dll:protector.dll: unclassified',
                         "linker flag '/alternatename:a=b' is not modelled",
                         "link-line flag '-fsanitize=address' is not modelled")
        # every other place a flag can sit: a module-definition file, -Xclang values, file variables, other rules
        extra = ('EVIL = -Wl,/DEFAULTLIB:top\n\n'
                 'build CMakeFiles/x.dir/a.cpp.obj: CXX_COMPILER__x a.cpp\n'
                 '  FLAGS = -O2 -Xclang -include -Xclang pch.hxx -Xclang --linker-option=/DEFAULTLIB:evil '
                 '-fms-runtime-lib=static -Xclang -mllvm\n\n'
                 'build CMakeFiles/x.dir/b.cpp.obj: MISSING_RULE b.cpp\n\n')
        build = self.fake_build('CMakeFiles/x.dir/a.cpp.obj\nCMakeFiles/x.dir/b.cpp.obj\n',
                                'CMakeFiles/x.dir/a.cpp.obj: #deps 0, deps mtime 5 (VALID)\n\n'
                                'CMakeFiles/x.dir/b.cpp.obj: #deps 0, deps mtime 5 (VALID)\n\n',
                                '-shared -Xlinker /DEF:CMakeFiles\\x.dir\\.\\exports.def', extra)
        files, binaries, problems, _ = gpc.build_inputs(build, 'x.exe')
        problems += gpc.census({}, binaries)[0]
        self.assertNamed(problems, 'def:CMakeFiles/x.dir/exports.def: unclassified binary input',
                         'lib:top.lib: unclassified binary input',
                         "-Xclang '--linker-option=/DEFAULTLIB:evil' is not modelled",
                         "'--linker-option=/DEFAULTLIB:evil' is not modelled",
                         "'-fms-runtime-lib=static' is not modelled", "-Xclang '-mllvm' is not modelled",
                         "rule 'MISSING_RULE' has no command")
        self.assertFalse([p for p in problems if 'pch.hxx' in p or '-include' in p or "'-shared'" in p], problems)

    def test_comments_escapes_and_raw_strings_between_literals(self):
        # Astra round C: "Virtual" /* join */ "Protect" passed the prefilter
        for text in ('const char* n = "Virtual" /* join */ "Protect";\n', 'const char* n = "Virtual" // x\n "Protect";\n',
                     'const char* n = "Virtual\\x50rotect";\n', 'const char* n = R"(Virtual)" "Protect";\n',
                     'const char* n = u8"Virtual" L"Protect";\n', 'const char* n = "Virtual\\\nProtect";\n',
                     'auto* n = &Virtual\\\nProtect;\n'):
            self.assertNamed(self.problems('kernel/new_join.cpp', text), 'in <file> to VirtualProtect')

    def test_digit_separators(self):
        # Astra round D: the apostrophe of 4'096 opened a character literal and hid the rest of the line, in the
        # literal scanner and in views()
        text = ('struct ApiSpec { unsigned bytes; const char* name; };\n'
                'ApiSpec spec{4\'096, "Virtual" "Protect"};\n')
        self.assertNamed(self.problems('kernel/new_sep.cpp', text), 'in <file> to VirtualProtect')
        text = 'constexpr int n = 1\'000; __pragma(comment(lib, "protector.lib"))\n'
        self.assertNamed(self.problems('kernel/new_sep.cpp', text), 'lib:protector.lib: unclassified binary')
        for number in ("0xFF'FF", "0b1010'1010", "1'000.0'5e+1'0", "123'456ull", ".5'0f"):
            text = 'double a[] = {%s}; auto* p = &VirtualFree;\nconst char* s = "Virtual" "Alloc";\n' % number
            self.assertNamed(self.problems('kernel/new_sep.cpp', text), 'in <file> to VirtualFree',
                             'in <file> to VirtualAlloc')
        # character literals stay literals: a quote inside one opens nothing, and u8'a' is not a number
        self.assertEqual(self.problems('kernel/new_sep.cpp', "char q = '\"'; char p = u8'a'; int k = 1'0;\n"), [])

    def test_function_labels(self):
        cases = {'\n#if defined(_WIN32)\nbool protectRun(uint32_t p, uint32_t n, DWORD prot) ': 'protectRun',
                 'PPC_FUNC(sub_82110768) ': 'PPC_FUNC(sub_82110768)', 'if (x) ': None, 'auto f = [](int a) ': None,
                 'X::X(int a) : b(a), c(2) ': 'X', 'bool f() const noexcept ': 'f', 'namespace sr ': None}
        for header, label in cases.items():
            self.assertEqual(gpc.function_label(header), label, header)


if __name__ == '__main__':
    unittest.main()
