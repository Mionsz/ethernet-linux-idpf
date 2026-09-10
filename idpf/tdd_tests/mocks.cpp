/**
 * @file mocks.cpp
 * @brief Single, shared inclusion point for reused common/mock_src
 * mock implementations.
 *
 * Per common/mock_src/README.md's documented design goal, mock_src
 * files are meant to be #include'd directly rather than linked as
 * separate objects. Since several idpf test files need overlapping
 * mocks (e.g. both test_if_idpf.cpp and test_idpf_lib.cpp call
 * bus_alloc_resource_any()/bus_release_resource(), mocked in
 * mock_kernel.cpp), each mock_src file is included here exactly once
 * and compiled as its own translation unit, avoiding duplicate-symbol
 * link errors that would occur if multiple test files each included
 * the same mock_src file directly.
 */
#include "idpf_utest.h"

#include "../../common/mock_src/mock_iflib.cpp"
#include "../../common/mock_src/mock_kernel.cpp"
#include "../../common/mock_src/mock_sbuf.cpp"
#include "../../common/mock_src/mock_kobj.cpp"
#include "../../common/mock_src/mock_malloc.cpp"
#include "../../common/mock_src/mock_device.cpp"
