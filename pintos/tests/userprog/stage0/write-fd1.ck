# -*- perl -*-
# stage 0: write(fd=1) 검증.
#
# common_checks를 호출하지 않는 이유:
#   common_checks는 "Powering off" / "Timer: # ticks" 등의 정상 종료 표식을
#   강제하지만, stage 0에서는 process_wait이 영구 블록되므로 kernel이
#   power_off까지 가지 못하고 timeout으로 종료된다. 따라서 마커 grep만 수행.

use strict;
use warnings;
use tests::tests;

our ($test);
my (@output) = read_text_file ("$test.output");

fail "STAGE0_WRITE_OK marker missing — write(fd=1) didn't reach console\n"
  if !grep (/STAGE0_WRITE_OK/, @output);

pass;
