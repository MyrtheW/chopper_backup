cmake_minimum_required (VERSION 3.11)

add_api_test (hyperloglog_test.cpp)
target_use_datasources (hyperloglog_test FILES small.fa)

add_api_test (user_bin_sequence_test.cpp)
target_use_datasources (user_bin_sequence_test FILES small.fa)
target_use_datasources (user_bin_sequence_test FILES small2.fa)
target_use_datasources (user_bin_sequence_test FILES small.hll)
target_use_datasources (user_bin_sequence_test FILES small2.hll)
