# RUN: %opt - -triple x86_64
  .section .text

_caller:
  .call        c

  mov.i64      $0, _callee
  mov.i32      $1, 0
  mov.i32      $2, 1
  mov.i32      $3, 2
  mov.i32      $4, 3
  mov.i32      $5, 4

  tcall.c      $0, $1, $2, $3, $4, $5
  .end

_callee:
  .args        i64, i64, i64, i64, i64
  ret
  .end


_caller_bail:
  mov.i64       $0, _caller_bail
  mov.i64       $1, 1
  mov.i64       $2, 2
  mov.i64       $3, 3
  mov.i64       $4, 4
  mov.i64       $5, 5
  mov.i64       $6, 6
  mov.i64       $7, 7
  tcall.i64.5.c $0, $1, $2, $3, $4, $5, $6, $7
  .end

_callee_bail:
  .args        i64, i64, i64, i64, i64
  .vararg
  mov.i64      $0, 0
  ret.i64      $0
  .end

call_indirect:
  .call       caml
  .args       i64, i64

  arg.i64     $0, 0
  arg.i64     $1, 1
  tcall.i64.c $0, $1
  .end
