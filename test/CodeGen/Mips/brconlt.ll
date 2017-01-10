; RUN: llc  -march=mipsel -mattr=mips16 -relocation-model=pic -O3 < %s | FileCheck %s -check-prefix=16
; RUN: llc  -march=mips -mattr=micromips -mcpu=mips32r6 -relocation-model=pic -O3 < %s | FileCheck %s -check-prefix=MM32R6

@i = global i32 5, align 4
@j = global i32 10, align 4
@k = global i32 5, align 4
@result = global i32 0, align 4

define void @test() nounwind {
entry:
  %0 = load i32, i32* @j, align 4
  %1 = load i32, i32* @i, align 4
  %cmp = icmp sge i32 %0, %1
  br i1 %cmp, label %if.then, label %if.end

; 16:     slt   ${{[0-9]+}}, ${{[0-9]+}}
; MM32R6: slt   ${{[0-9]+}}, ${{[0-9]+}}
; 16:     btnez $[[LABEL:[0-9A-Ba-b_]+]]
; 16:     $[[LABEL]]:

if.then:                                          ; preds = %entry
  store i32 1, i32* @result, align 4
  br label %if.end

if.end:                                           ; preds = %entry, %if.then
  ret void
}


