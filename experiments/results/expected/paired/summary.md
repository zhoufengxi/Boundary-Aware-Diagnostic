# Real-project diagnostic evidence re-evaluation

Matching never uses allocation, release, or branch witnesses. Terminal locations identify bugs; evidence-stripped path skeletons pair duplicate instances.

## OpenHarmony

- Paired analysis actions: 2,598
- Action groups by status: `{"baseline_only": 3, "ours_only": 378, "paired": 2598}`
- Baseline reports / unique terminal issues: 979 / 424
- Ours reports / unique terminal issues: 4,149 / 1,089
- Baseline reports by checker family: `{"core": 323, "cplusplus": 655, "unix": 1}`
- Ours reports by checker family: `{"core": 726, "cplusplus": 3391, "unix": 32}`
- Baseline reports by checker: `{"core.CallAndMessage": 70, "core.DivideZero": 2, "core.NonNullParamChecker": 70, "core.NullDereference": 96, "core.uninitialized.Assign": 17, "core.uninitialized.Branch": 42, "core.uninitialized.UndefReturn": 26, "cplusplus.InnerPointer": 29, "cplusplus.Move": 1, "cplusplus.NewDelete": 31, "cplusplus.NewDeleteLeaks": 593, "cplusplus.PlacementNew": 1, "unix.cstring.NullArg": 1}`
- Ours reports by checker: `{"core.CallAndMessage": 306, "core.DivideZero": 2, "core.NonNullParamChecker": 70, "core.NullDereference": 95, "core.uninitialized.Assign": 50, "core.uninitialized.Branch": 79, "core.uninitialized.UndefReturn": 124, "cplusplus.InnerPointer": 29, "cplusplus.Move": 1, "cplusplus.NewDelete": 38, "cplusplus.NewDeleteLeaks": 3322, "cplusplus.PlacementNew": 1, "unix.Malloc": 31, "unix.cstring.NullArg": 1}`
- Baseline reports by defect kind: `{"leak": 593, "other": 355, "uaf": 31}`
- Ours reports by defect kind: `{"leak": 3353, "other": 758, "uaf": 38}`
- Global unique issues (common / ours-only / baseline-only): 398 / 691 / 26
- Terminal issue groups: `{"ambiguous": 1, "baseline_only": 36, "common": 871, "ours_only": 3023}`
- Path instances: `{"ambiguous": 72, "baseline_only": 36, "matched": 907, "ours_only": 3148, "ours_surplus": 58}`
- Matched-path classes: `{"not_applicable": 327, "unchanged": 580}`

Lifecycle-applicable matched paths (non-applicable diagnostics are excluded):

| Evidence | Gained | Retained | Lost | Absent in both |
|---|---:|---:|---:|---:|
| allocation | 0 | 554 | 0 | 26 |
| release | 0 | 30 | 0 | 550 |
| branch | 0 | 572 | 0 | 8 |

- Matched unique-issue classes: `{"not_applicable": 180, "unchanged": 217}`
- Ours-only lifecycle-applicable unique issues complete in every instance: 604/604 (all ours-only issues: 691)
- Baseline-only lifecycle-applicable unique issues complete in every instance: 3/3 (all baseline-only issues: 26)
- Action-local ours-only lifecycle-complete instances: 2714/2717 (all instances: 3148)
- Action-local baseline-only lifecycle-complete instances: 7/8 (all instances: 36)
- Parse errors: 0

Case-study metrics:

- shared_event_data_uaf (baseline): 0 reports, 0 unique issues, 0 lifecycle-complete
- shared_event_data_uaf (ours): 4 reports, 1 unique issues, 4 lifecycle-complete
- failed_napi_wrap_cleanup (baseline): 0 reports, 0 unique issues, 0 lifecycle-complete
- failed_napi_wrap_cleanup (ours): 1 reports, 1 unique issues, 1 lifecycle-complete
- custom_shared_ptr_ownership (baseline): 0 reports, 0 unique issues, 0 lifecycle-complete
- custom_shared_ptr_ownership (ours): 124 reports, 1 unique issues, 124 lifecycle-complete

## Android

- Paired analysis actions: 29,758
- Action groups by status: `{"baseline_only": 137, "ours_only": 737, "paired": 29758}`
- Baseline reports / unique terminal issues: 25,423 / 5,546
- Ours reports / unique terminal issues: 38,907 / 11,210
- Baseline reports by checker family: `{"core": 21190, "cplusplus": 1846, "unix": 2387}`
- Ours reports by checker family: `{"core": 23527, "cplusplus": 5837, "unix": 9543}`
- Baseline reports by checker: `{"core.BitwiseShift": 3434, "core.CallAndMessage": 7975, "core.DivideZero": 609, "core.NonNullParamChecker": 925, "core.NullDereference": 4220, "core.StackAddressEscape": 60, "core.VLASize": 9, "core.uninitialized.ArraySubscript": 17, "core.uninitialized.Assign": 2019, "core.uninitialized.Branch": 115, "core.uninitialized.UndefReturn": 1807, "cplusplus.InnerPointer": 1, "cplusplus.Move": 97, "cplusplus.NewDelete": 626, "cplusplus.NewDeleteLeaks": 1106, "cplusplus.PlacementNew": 12, "cplusplus.PureVirtualCall": 4, "unix.API": 4, "unix.Errno": 55, "unix.Malloc": 2206, "unix.MallocSizeof": 40, "unix.MismatchedDeallocator": 8, "unix.StdCLibraryFunctions": 6, "unix.Vfork": 53, "unix.cstring.NullArg": 15}`
- Ours reports by checker: `{"core.BitwiseShift": 3437, "core.CallAndMessage": 9488, "core.DivideZero": 639, "core.NonNullParamChecker": 914, "core.NullDereference": 4225, "core.StackAddressEscape": 61, "core.VLASize": 9, "core.uninitialized.ArraySubscript": 21, "core.uninitialized.Assign": 2050, "core.uninitialized.Branch": 114, "core.uninitialized.UndefReturn": 2569, "cplusplus.InnerPointer": 1, "cplusplus.Move": 96, "cplusplus.NewDelete": 686, "cplusplus.NewDeleteLeaks": 5037, "cplusplus.PlacementNew": 13, "cplusplus.PureVirtualCall": 4, "unix.API": 4, "unix.Errno": 55, "unix.Malloc": 9362, "unix.MallocSizeof": 40, "unix.MismatchedDeallocator": 8, "unix.StdCLibraryFunctions": 6, "unix.Vfork": 52, "unix.cstring.NullArg": 16}`
- Baseline reports by defect kind: `{"double_free": 40, "leak": 2087, "other": 21791, "uaf": 1505}`
- Ours reports by defect kind: `{"double_free": 47, "leak": 13127, "other": 24125, "uaf": 1608}`
- Global unique issues (common / ours-only / baseline-only): 5,437 / 5,773 / 109
- Terminal issue groups: `{"ambiguous": 273, "baseline_only": 390, "common": 23927, "ours_only": 11685}`
- Path instances: `{"ambiguous": 2096, "baseline_only": 398, "matched": 23977, "ours_only": 13822, "ours_surplus": 60}`
- Matched-path classes: `{"improved": 9, "not_applicable": 20555, "unchanged": 3413}`

Lifecycle-applicable matched paths (non-applicable diagnostics are excluded):

| Evidence | Gained | Retained | Lost | Absent in both |
|---|---:|---:|---:|---:|
| allocation | 9 | 2353 | 0 | 1060 |
| release | 0 | 1515 | 0 | 1907 |
| branch | 0 | 3417 | 0 | 5 |

- Matched unique-issue classes: `{"improved": 5, "not_applicable": 4151, "unchanged": 1223}`
- Ours-only lifecycle-applicable unique issues complete in every instance: 5597/5623 (all ours-only issues: 5773)
- Baseline-only lifecycle-applicable unique issues complete in every instance: 61/67 (all baseline-only issues: 109)
- Action-local ours-only lifecycle-complete instances: 11069/11193 (all instances: 13822)
- Action-local baseline-only lifecycle-complete instances: 72/99 (all instances: 398)
- Parse errors: 0
- Input metadata: `{"baseline_analyzer_version": "", "baseline_checker_groups": ["cplusplus", "core", "unix"], "baseline_existing_result_plists": 29895, "baseline_missing_result_plists": 0, "baseline_missing_results": [], "baseline_planned_actions": 30634, "baseline_result_plists": 29895, "baseline_working_directory": "/artifact/android", "ours_analyzer_version": "", "ours_checker_groups": ["cplusplus", "core", "unix"], "ours_existing_result_plists": 30495, "ours_missing_result_plists": 14, "ours_missing_results": [{"plist": "pthread_h.c_clangsa_6dc8db2fd89a3562ac9108bb58fa1a0f.plist", "source": "bionic/tests/headers/posix/pthread_h.c"}, {"plist": "enc_output_format_tab.cpp_clangsa_8cc7ed734252f362869aea703e75fd31.plist", "source": "frameworks/av/media/module/codecs/amrnb/enc/src/enc_output_format_tab.cpp"}, {"plist": "ihevc_quant_tables.c_clangsa_c0e4576f99452174be47d30fc3ddaad8.plist", "source": "external/libhevc/common/ihevc_quant_tables.c"}, {"plist": "ETC_Decoder.cpp_clangsa_733ac94513e2cf9c6f43e4560fd7ab28.plist", "source": "external/swiftshader/src/Device/ETC_Decoder.cpp"}, {"plist": "ih264e_globals.c_clangsa_9d0d2e1e32ce9c4471aebfcf602c1416.plist", "source": "external/libavc/encoder/ih264e_globals.c"}, {"plist": "s_ilogb.c_clangsa_cc9669b00aa2f2e81f42e3d8018fe7ee.plist", "source": "external/fdlibm/s_ilogb.c"}, {"plist": "DisplayConnectionType.cpp_clangsa_52fcffdebfa5f407c84fc0ef5dcb4526.plist", "source": "out/soong/.intermediates/frameworks/native/libs/gui/libgui_aidl_static/android_x86_silvermont_static_lto-thin/a7ec4cedd350a9f16160166d32d1dd66/gen/aidl_library/android/gui/DisplayConnectionType.cpp"}, {"plist": "windows_listener.cc_clangsa_d70438659977898f9e03de0400d1e111.plist", "source": "external/grpc-grpc/src/core/lib/event_engine/windows/windows_listener.cc"}, {"plist": "inline_variable_test_a.cc_clangsa_2f94b706090e8f699d492fcc9f1160fe.plist", "source": "external/tensorflow/third_party/absl/abseil-cpp/absl/base/inline_variable_test_a.cc"}, {"plist": "DWARFLineInfo.cpp_clangsa_c30ebf1dd57caefd8e5606548c84c362.plist", "source": "frameworks/compile/mclinker/lib/LD/DWARFLineInfo.cpp"}, {"plist": "s_fmax.c_clangsa_f0fe9ea7dd8b05433df4617f29d793c0.plist", "source": "bionic/libm/upstream-freebsd/lib/msun/src/s_fmax.c"}, {"plist": "ixheaacd_rom.c_clangsa_5b0f4d25b780732765cee6f1318355ac.plist", "source": "external/libxaac/decoder/ixheaacd_rom.c"}, {"plist": "socket_utils_windows.cc_clangsa_1f5496a11ce2ee35332b63ef1a7d736a.plist", "source": "external/grpc-grpc/src/core/lib/iomgr/socket_utils_windows.cc"}, {"plist": "srp_server_api.cpp_clangsa_cdb8394936042be4c64640ee8345a9d9.plist", "source": "external/openthread/src/core/api/srp_server_api.cpp"}], "ours_planned_actions": 30634, "ours_result_plists": 30509, "ours_working_directory": "/artifact/android"}`
