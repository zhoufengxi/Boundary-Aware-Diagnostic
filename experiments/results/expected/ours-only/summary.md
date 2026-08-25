# Real-project diagnostic evidence re-evaluation

Ours-only results describe evidence in the released reports. In paired mode, matching never uses allocation, release, or branch witnesses; terminal locations identify bugs and evidence-stripped path skeletons pair duplicate instances.

## OpenHarmony (ours only)

- Analyzed actions: 2,976
- Result plist files: 2,976
- Report instances / unique terminal issues: 4,283 / 1,136
- Reports by checker family: `{"core": 755, "cplusplus": 3496, "unix": 32}`
- Reports by checker: `{"core.CallAndMessage": 325, "core.DivideZero": 2, "core.NonNullParamChecker": 73, "core.NullDereference": 95, "core.uninitialized.Assign": 50, "core.uninitialized.Branch": 81, "core.uninitialized.UndefReturn": 129, "cplusplus.InnerPointer": 29, "cplusplus.Move": 1, "cplusplus.NewDelete": 46, "cplusplus.NewDeleteLeaks": 3419, "cplusplus.PlacementNew": 1, "unix.Malloc": 31, "unix.cstring.NullArg": 1}`
- Reports by defect kind: `{"leak": 3450, "other": 787, "uaf": 46}`
- Lifecycle-complete applicable reports: 3,459/3,496
- Lifecycle completeness by defect kind: `{"leak": {"applicable": 3450, "complete": 3450}, "uaf": {"applicable": 46, "complete": 9}}`

Evidence present in emitted paths:

| Evidence | All reports | Lifecycle-applicable reports |
|---|---:|---:|
| allocation | 3459 | 3459 |
| release | 46 | 46 |
| branch | 2633 | 2633 |

- Parse errors: 0

Case-study metrics:

- shared_event_data_uaf: 4 reports, 1 unique issues, 4 lifecycle-complete
- failed_napi_wrap_cleanup: 1 reports, 1 unique issues, 1 lifecycle-complete
- custom_shared_ptr_ownership: 124 reports, 1 unique issues, 124 lifecycle-complete

## Android (ours only)

- Analyzed actions: 30,495
- Result plist files: 30,495
- Report instances / unique terminal issues: 43,088 / 11,452
- Reports by checker family: `{"core": 26551, "cplusplus": 6253, "unix": 10284}`
- Reports by checker: `{"core.BitwiseShift": 3970, "core.CallAndMessage": 10702, "core.DivideZero": 661, "core.NonNullParamChecker": 925, "core.NullDereference": 4237, "core.StackAddressEscape": 72, "core.VLASize": 9, "core.uninitialized.ArraySubscript": 21, "core.uninitialized.Assign": 2336, "core.uninitialized.Branch": 115, "core.uninitialized.UndefReturn": 3503, "cplusplus.InnerPointer": 1, "cplusplus.Move": 97, "cplusplus.NewDelete": 786, "cplusplus.NewDeleteLeaks": 5352, "cplusplus.PlacementNew": 13, "cplusplus.PureVirtualCall": 4, "unix.API": 4, "unix.Errno": 55, "unix.Malloc": 10103, "unix.MallocSizeof": 40, "unix.MismatchedDeallocator": 8, "unix.StdCLibraryFunctions": 6, "unix.Vfork": 52, "unix.cstring.NullArg": 16}`
- Reports by defect kind: `{"double_free": 48, "leak": 14155, "other": 27171, "uaf": 1714}`
- Lifecycle-complete applicable reports: 14,628/15,917
- Lifecycle completeness by defect kind: `{"double_free": {"applicable": 48, "complete": 20}, "leak": {"applicable": 14155, "complete": 14155}, "uaf": {"applicable": 1714, "complete": 453}}`

Evidence present in emitted paths:

| Evidence | All reports | Lifecycle-applicable reports |
|---|---:|---:|
| allocation | 14628 | 14628 |
| release | 1762 | 1762 |
| branch | 15060 | 15060 |

- Parse errors: 0
- Input metadata: `{"ours_analyzer_version": "", "ours_checker_groups": ["cplusplus", "core", "unix"], "ours_existing_result_plists": 30495, "ours_missing_result_plists": 14, "ours_missing_results": [{"plist": "pthread_h.c_clangsa_6dc8db2fd89a3562ac9108bb58fa1a0f.plist", "source": "bionic/tests/headers/posix/pthread_h.c"}, {"plist": "enc_output_format_tab.cpp_clangsa_8cc7ed734252f362869aea703e75fd31.plist", "source": "frameworks/av/media/module/codecs/amrnb/enc/src/enc_output_format_tab.cpp"}, {"plist": "ihevc_quant_tables.c_clangsa_c0e4576f99452174be47d30fc3ddaad8.plist", "source": "external/libhevc/common/ihevc_quant_tables.c"}, {"plist": "ETC_Decoder.cpp_clangsa_733ac94513e2cf9c6f43e4560fd7ab28.plist", "source": "external/swiftshader/src/Device/ETC_Decoder.cpp"}, {"plist": "ih264e_globals.c_clangsa_9d0d2e1e32ce9c4471aebfcf602c1416.plist", "source": "external/libavc/encoder/ih264e_globals.c"}, {"plist": "s_ilogb.c_clangsa_cc9669b00aa2f2e81f42e3d8018fe7ee.plist", "source": "external/fdlibm/s_ilogb.c"}, {"plist": "DisplayConnectionType.cpp_clangsa_52fcffdebfa5f407c84fc0ef5dcb4526.plist", "source": "out/soong/.intermediates/frameworks/native/libs/gui/libgui_aidl_static/android_x86_silvermont_static_lto-thin/a7ec4cedd350a9f16160166d32d1dd66/gen/aidl_library/android/gui/DisplayConnectionType.cpp"}, {"plist": "windows_listener.cc_clangsa_d70438659977898f9e03de0400d1e111.plist", "source": "external/grpc-grpc/src/core/lib/event_engine/windows/windows_listener.cc"}, {"plist": "inline_variable_test_a.cc_clangsa_2f94b706090e8f699d492fcc9f1160fe.plist", "source": "external/tensorflow/third_party/absl/abseil-cpp/absl/base/inline_variable_test_a.cc"}, {"plist": "DWARFLineInfo.cpp_clangsa_c30ebf1dd57caefd8e5606548c84c362.plist", "source": "frameworks/compile/mclinker/lib/LD/DWARFLineInfo.cpp"}, {"plist": "s_fmax.c_clangsa_f0fe9ea7dd8b05433df4617f29d793c0.plist", "source": "bionic/libm/upstream-freebsd/lib/msun/src/s_fmax.c"}, {"plist": "ixheaacd_rom.c_clangsa_5b0f4d25b780732765cee6f1318355ac.plist", "source": "external/libxaac/decoder/ixheaacd_rom.c"}, {"plist": "socket_utils_windows.cc_clangsa_1f5496a11ce2ee35332b63ef1a7d736a.plist", "source": "external/grpc-grpc/src/core/lib/iomgr/socket_utils_windows.cc"}, {"plist": "srp_server_api.cpp_clangsa_cdb8394936042be4c64640ee8345a9d9.plist", "source": "external/openthread/src/core/api/srp_server_api.cpp"}], "ours_planned_actions": 30634, "ours_result_plists": 30509}`
