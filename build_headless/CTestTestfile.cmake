# CMake generated Testfile for 
# Source directory: /workspace
# Build directory: /workspace/build_headless
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[elysium_headless]=] "/workspace/build_headless/elysium_headless_tests")
set_tests_properties([=[elysium_headless]=] PROPERTIES  _BACKTRACE_TRIPLES "/workspace/CMakeLists.txt;74;add_test;/workspace/CMakeLists.txt;0;")
add_test([=[include_boundary]=] "bash" "/workspace/scripts/check_include_boundaries.sh")
set_tests_properties([=[include_boundary]=] PROPERTIES  LABELS "architecture;headless" _BACKTRACE_TRIPLES "/workspace/CMakeLists.txt;75;add_test;/workspace/CMakeLists.txt;0;")
