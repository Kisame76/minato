# Soong's global flags for a host module (build/soong cc/config/global.go at android-15.0.0_r1), so that the AOSP
# sources compile with the warnings their -Werror was written for: the common C and C++ flags come before a module's
# own, the no-override flags after them, and third-party code (external/) gets Soong's external flags too. Left out:
# -O2 (CMake's build type), the debug-info, diagnostics-color and path flags, and -Wno-error=enum-constexpr-conversion,
# a warning the Mac's clang no longer knows.
set(soong_common_flags
  -Wall -Wextra -Winit-self -Wpointer-arith -Wunguarded-availability -Werror=date-time -Werror=int-conversion
  -Werror=pragma-pack -Werror=pragma-pack-suspicious-include -Werror=sizeof-array-div -Werror=string-plus-int
  -Werror=unreachable-code-loop-increment -Wno-error=deprecated-declarations -Wno-c99-designator
  -Wno-gnu-folding-constant -Wno-inconsistent-missing-override -Wno-error=reorder-init-list -Wno-reorder-init-list
  -Wno-sign-compare -Wno-unused -DANDROID -DNDEBUG -UDEBUG -D__compiler_offsetof=__builtin_offsetof
  -D__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__ -ffp-contract=off -fno-exceptions -fno-strict-aliasing -Wno-error=format
  $<$<COMPILE_LANGUAGE:CXX>:-Wimplicit-fallthrough -D_LIBCPP_ENABLE_THREAD_SAFETY_ANNOTATIONS -Wno-gnu-include-next>)
set(soong_no_override_flags
  -Werror=bool-operation -Werror=format-insufficient-args -Werror=implicit-int-float-conversion
  -Werror=int-in-bool-context -Werror=int-to-pointer-cast -Werror=pointer-to-int-cast -Werror=xor-used-as-pow
  -Wno-void-pointer-to-enum-cast -Wno-void-pointer-to-int-cast -Wno-pointer-to-int-cast -Werror=fortify-source
  -Wno-unused-variable -Wno-missing-field-initializers -Wno-packed-non-pod -Werror=address-of-temporary
  -Werror=incompatible-function-pointer-types -Werror=null-dereference -Werror=return-type
  -Wno-tautological-constant-compare -Wno-tautological-type-limit-compare -Wno-implicit-int-float-conversion
  -Wno-tautological-overlap-compare -Wno-deprecated-copy -Wno-range-loop-construct -Wno-zero-as-null-pointer-constant
  -Wno-deprecated-anon-enum-enum-conversion -Wno-deprecated-enum-enum-conversion -Wno-pessimizing-move
  -Wno-non-c-typedef-for-linkage -Wno-align-mismatch -Wno-error=unused-but-set-variable
  -Wno-error=unused-but-set-parameter -Wno-error=deprecated-builtins -Wno-error=deprecated
  -Wno-error=single-bit-bitfield-constant-conversion -Wno-error=invalid-offsetof
  -Wno-error=thread-safety-reference-return -Wno-deprecated-dynamic-exception-spec -Wno-vla-cxx-extension)
# Warnings that the Mac's clang (Apple clang 21) has and AOSP's clang at the tag (clang-r510928, 18) has not, each met
# in these sources: a virtual method in a final class (protobuf's headers), memcpy and memset on types that are not
# trivially copyable (libutils' and libandroidfw's headers), a cast between thread function types (libutils'
# Threads.cpp), and ATOMIC_VAR_INIT marked deprecated (libcutils' trace-host.cpp, protobuf's common.cc). They come
# last, as Soong's no-override flags do for its own clang's new warnings.
set(newer_clang_flags -Wno-unnecessary-virtual-specifier -Wno-nontrivial-memcall -Wno-cast-function-type-mismatch
  -Wno-deprecated-pragma)
set(soong_external_flags
  -Wno-enum-compare -Wno-enum-compare-switch -Wno-null-pointer-arithmetic -Wno-null-dereference -Wno-pointer-compare
  -Wno-final-dtor-non-final-class -Wno-psabi -Wno-null-pointer-subtraction -Wno-string-concatenation
  -Wno-deprecated-non-prototype -Wno-unused -Wno-deprecated)
set(soong_no_override_external_flags
  -fcommon -Wno-format-insufficient-args -Wno-misleading-indentation -Wno-bitwise-instead-of-logical -Wno-unused
  -Wno-unused-parameter -Wno-unused-but-set-parameter -Wno-unqualified-std-cast-call -Wno-array-parameter
  -Wno-gnu-offsetof-extensions -Wno-fortify-source)

# Soong's language versions: gnu17 and gnu++20, unless a module asks for another (cpp_std).
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# A module's flags in Soong's order: the common flags (and for external code the external ones), the module's own
# `flags`, then the no-override flags.
function(soong_module target)
  cmake_parse_arguments(module "EXTERNAL" "" "FLAGS" ${ARGN})
  set(flags ${soong_common_flags})
  if(module_EXTERNAL)
    list(APPEND flags ${soong_external_flags})
  endif()
  list(APPEND flags ${module_FLAGS} ${soong_no_override_flags} ${newer_clang_flags})
  if(module_EXTERNAL)
    list(APPEND flags ${soong_no_override_external_flags})
  endif()
  target_compile_options(${target} PRIVATE ${flags})
endfunction()
